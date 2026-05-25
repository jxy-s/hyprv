#include "rdp/RdpHostClient.h"

#include <objbase.h>
#include <combaseapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

extern void HyprvAppLog(const wchar_t* fmt, ...);

namespace hyprv::app
{
    namespace
    {
        constexpr DWORD kPipeBuf       = 64 * 1024;
        constexpr DWORD kHandshakeWait = 5000;       // ms — child must connect this fast
        constexpr DWORD kShutdownWait  = 2000;       // ms — wait before TerminateProcess

        // mstscax's UpdateSessionDisplaySettings rejects an arbitrary
        // desktopScaleFactor — only a fixed set of percentages is valid
        // (E_INVALIDARG otherwise, which silently leaves the guest at its
        // current scale). Snap a computed host-scale percent to the nearest
        // allowed value. Set per [MS-RDPBCGR] / IMsRdpClient9 docs.
        uint32_t SnapRdpScalePercent(uint32_t pct)
        {
            constexpr uint32_t kValid[] = { 100, 125, 150, 175, 200, 250, 300, 400, 500 };
            uint32_t best = kValid[0];
            uint32_t bestDelta = 0xFFFFFFFFu;
            for (uint32_t v : kValid)
            {
                uint32_t d = (v > pct) ? (v - pct) : (pct - v);
                if (d < bestDelta) { bestDelta = d; best = v; }
            }
            return best;
        }

        std::wstring MakeUniqueId()
        {
            GUID g{};
            CoCreateGuid(&g);
            wchar_t buf[40];
            swprintf_s(buf, L"%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X",
                g.Data1, g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
            return buf;
        }

        // Sync-style I/O on a FILE_FLAG_OVERLAPPED handle. The pipe is opened
        // OVERLAPPED so concurrent reads (rx thread) and writes (any thread)
        // don't serialize at the handle level — without OVERLAPPED the kernel
        // would queue every write behind the rx thread's pending blocking read,
        // deadlocking the whole IPC. Each call here owns its own auto-reset
        // event and waits on it, giving us synchronous semantics on top of
        // async I/O.
        bool OverlappedRead(HANDLE pipe, void* buf, DWORD bytes, DWORD* outRead)
        {
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL ok = ReadFile(pipe, buf, bytes, nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING)
            {
                CloseHandle(ov.hEvent);
                return false;
            }
            ok = GetOverlappedResult(pipe, &ov, outRead, TRUE);
            CloseHandle(ov.hEvent);
            return ok != FALSE;
        }

        bool OverlappedWrite(HANDLE pipe, void const* buf, DWORD bytes, DWORD* outWritten)
        {
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL ok = WriteFile(pipe, buf, bytes, nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING)
            {
                CloseHandle(ov.hEvent);
                return false;
            }
            ok = GetOverlappedResult(pipe, &ov, outWritten, TRUE);
            CloseHandle(ov.hEvent);
            return ok != FALSE;
        }

        bool ReadExact(HANDLE pipe, void* buf, uint32_t bytes)
        {
            auto* p = static_cast<uint8_t*>(buf);
            uint32_t remaining = bytes;
            while (remaining > 0)
            {
                DWORD got = 0;
                if (!OverlappedRead(pipe, p, remaining, &got) || got == 0)
                    return false;
                p += got;
                remaining -= got;
            }
            return true;
        }

        bool WriteExact(HANDLE pipe, void const* buf, uint32_t bytes)
        {
            DWORD written = 0;
            if (!OverlappedWrite(pipe, buf, bytes, &written) || written != bytes)
                return false;
            return true;
        }
    }

    // Process-wide singleton job object. Every hyprv-rdphost child gets
    // assigned to it after CreateProcess (and before its main thread runs).
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE means when the last handle to the
    // job closes — i.e. hyprv exits, including via crash / TerminateProcess
    // / debugger detach — the kernel reaps every child still in the job. This
    // is defense in depth on top of the explicit Stop() path; without it,
    // an unhandled exception or hard-kill leaves orphan rdphost.exe processes.
    static HANDLE EnsureChildJob()
    {
        static HANDLE s_job = []() -> HANDLE {
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (!job) return nullptr;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                JOB_OBJECT_LIMIT_BREAKAWAY_OK;  // future opt-out, if needed
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                         &info, sizeof(info)))
            {
                CloseHandle(job);
                return nullptr;
            }
            return job;
        }();
        return s_job;
    }

    struct RdpHostClient::Impl
    {
        std::wstring          rdphostExe;
        HANDLE                pipe        = INVALID_HANDLE_VALUE;
        PROCESS_INFORMATION   procInfo    = {};
        HWND                  childHwnd   = nullptr;
        std::thread           rxThread;
        std::atomic<bool>     rxRunning   { false };
        std::mutex            sendMutex;

        // ---- IPC short-circuit cache --------------------------------------
        // Last (sx, sy, w, h) sent through Reposition and last (w, h, dpiX)
        // sent through UpdateSessionDisplaySettings. Tab switches with no
        // actual geometry change otherwise flicker because every cross-process
        // SetWindowPos triggers WM_WINDOWPOSCHANGED/WM_PAINT in the child, and
        // every UpdateSessionDisplaySettings IPC re-issues a Display PDU to
        // the server. Cache invalidated in Disconnect() and at SpawnAndHandshake.
        int      lastSx     = INT_MIN;
        int      lastSy     = INT_MIN;
        int      lastW      = 0;
        int      lastH      = 0;
        uint32_t lastDsW    = 0;
        uint32_t lastDsH    = 0;
        uint32_t lastDsDpiX = 0;

        // Human-readable label (typically VM name) for log-line prefixing.
        // Stays empty until VmTabPage calls SetLogLabel; falls back to the
        // child's pid alone when empty.
        std::wstring          logLabel;

        // Callbacks owned by the public class — Impl just calls them.
        RdpHostClient*        owner       = nullptr;

        explicit Impl(std::wstring exePath) : rdphostExe(std::move(exePath)) {}
        ~Impl() { Cleanup(); }

        bool SendP2C(hyprv::ipc::P2C type, void const* payload, uint32_t payloadSize)
        {
            std::lock_guard<std::mutex> lock(sendMutex);
            if (pipe == INVALID_HANDLE_VALUE) { HyprvAppLog(L"[hc] SendP2C: pipe INVALID"); return false; }
            HyprvAppLog(L"[hc] SendP2C type=%u size=%u pipe=%p", static_cast<unsigned>(type), payloadSize, pipe);
            hyprv::ipc::Header h{};
            h.type = static_cast<uint8_t>(type);
            h.payloadSize = payloadSize;
            if (!WriteExact(pipe, &h, sizeof(h)))
            {
                HyprvAppLog(L"[hc] SendP2C header write failed err=%lu", GetLastError());
                return false;
            }
            if (payloadSize && !WriteExact(pipe, payload, payloadSize))
            {
                HyprvAppLog(L"[hc] SendP2C payload write failed err=%lu", GetLastError());
                return false;
            }
            return true;
        }

        // Synchronous: create pipe, spawn child, do Hello/HelloAck, read HwndReady.
        // After this returns true, childHwnd is set and the IPC channel is ready for
        // ongoing command/event traffic.
        bool SpawnAndHandshake()
        {
            const auto id = MakeUniqueId();
            const auto pipeName = std::wstring(hyprv::ipc::kPipeNamePrefix) + id;

            pipe = CreateNamedPipeW(pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1, kPipeBuf, kPipeBuf, 0, nullptr);
            HyprvAppLog(L"[hc] CreateNamedPipe '%s' -> %p", pipeName.c_str(), pipe);
            if (pipe == INVALID_HANDLE_VALUE) return false;

            // Spawn child suspended so we can assign it to the kill-on-job-close
            // job before any code runs in the child — guarantees that even if
            // the parent dies between CreateProcess and AssignProcessToJobObject,
            // the child can't slip out.
            std::wstring cmd = L"\"" + rdphostExe + L"\" --pipe=" + id;
            STARTUPINFOW si{ sizeof(si) };
            const DWORD createFlags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
            if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                createFlags, nullptr, nullptr, &si, &procInfo))
                return false;

            HyprvAppLog(L"[hc] spawned (suspended) child pid=%lu", procInfo.dwProcessId);

            // Assign to the process-wide job so a hyprv crash takes the child
            // with it. Failure here is logged but non-fatal — the explicit
            // Stop() path is still our primary teardown.
            if (HANDLE job = EnsureChildJob())
            {
                if (!AssignProcessToJobObject(job, procInfo.hProcess))
                {
                    HyprvAppLog(L"[hc] AssignProcessToJobObject FAILED err=%lu — "
                                L"orphan-on-crash protection disabled for pid=%lu",
                                GetLastError(), procInfo.dwProcessId);
                }
            }
            else
            {
                HyprvAppLog(L"[hc] no child job available — orphan-on-crash "
                            L"protection disabled for pid=%lu",
                            procInfo.dwProcessId);
            }

            if (ResumeThread(procInfo.hThread) == static_cast<DWORD>(-1))
            {
                HyprvAppLog(L"[hc] ResumeThread FAILED err=%lu", GetLastError());
                return false;
            }

            // Cache invariant: a fresh child has no known geometry yet.
            lastSx = INT_MIN; lastSy = INT_MIN; lastW = 0; lastH = 0;
            lastDsW = 0; lastDsH = 0; lastDsDpiX = 0;

            // Wait for the child to open its end. Pipe is OVERLAPPED, so
            // ConnectNamedPipe returns ERROR_IO_PENDING and we wait on the event.
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL r = ConnectNamedPipe(pipe, &ov);
            DWORD err = r ? 0 : GetLastError();
            if (!r && err == ERROR_IO_PENDING)
            {
                WaitForSingleObject(ov.hEvent, INFINITE);
                DWORD dummy = 0;
                r = GetOverlappedResult(pipe, &ov, &dummy, FALSE);
                err = r ? 0 : GetLastError();
            }
            CloseHandle(ov.hEvent);
            HyprvAppLog(L"[hc] ConnectNamedPipe ret=%d err=%lu", r ? 1 : 0, err);
            if (!r && err != ERROR_PIPE_CONNECTED) return false;

            // Send Hello (we're the server / parent — child sends HelloAck back).
            hyprv::ipc::Hello hello{
                hyprv::ipc::kProtocolVersion,
                hyprv::ipc::kProtocolVersion, 0 };
            if (!SendP2C(hyprv::ipc::P2C::Hello, &hello, sizeof(hello))) return false;

            // Read HelloAck.
            hyprv::ipc::Header h{};
            if (!ReadExact(pipe, &h, sizeof(h))) return false;
            if (h.type != static_cast<uint8_t>(hyprv::ipc::C2P::HelloAck)) return false;
            if (h.payloadSize != sizeof(hyprv::ipc::HelloAck)) return false;
            hyprv::ipc::HelloAck ack{};
            if (!ReadExact(pipe, &ack, sizeof(ack))) return false;
            if (ack.status != 0) return false;

            // Read HwndReady.
            if (!ReadExact(pipe, &h, sizeof(h))) return false;
            if (h.type != static_cast<uint8_t>(hyprv::ipc::C2P::HwndReady)) return false;
            if (h.payloadSize != sizeof(hyprv::ipc::HwndReady)) return false;
            hyprv::ipc::HwndReady hr{};
            if (!ReadExact(pipe, &hr, sizeof(hr))) return false;
            childHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(hr.hwnd));
            return childHwnd != nullptr;
        }

        // Attach the child as an OWNED top-level window of ownerHwnd, positioned
        // in screen coords. We deliberately do NOT SetParent + WS_CHILD: WinUI 3
        // renders its XAML via a DirectComposition swap chain that paints over
        // any native HWND children of the same parent, regardless of Z-order. The
        // standard workaround is an owned popup — top-level in the Win32 sense
        // (so the composition layer can't draw over it), but tied to the owner's
        // lifetime + alt-tab grouping + minimize behavior. See gotchas.
        //
        // Does NOT show the window, attach input queues, or set focus — those
        // are deferred to AttachInputNow() to avoid deadlocking against mstscax
        // worker SendMessages during cross-process activation. Caller (parent)
        // is responsible for SetWindowPos-ing the child whenever ownerHwnd moves
        // or the Border bounds change.
        void EmbedInto(HWND ownerHwnd, int sx, int sy, int w, int h)
        {
            HyprvAppLog(L"[hc] EmbedInto owner=%p childHwnd=%p rect=%d,%d %dx%d",
                ownerHwnd, childHwnd, sx, sy, w, h);
            // GWLP_HWNDPARENT on a top-level window sets the OWNER. Cross-process
            // ownership is fine — Win32 looks up the HWND, not the process.
            SetWindowLongPtrW(childHwnd, GWLP_HWNDPARENT,
                reinterpret_cast<LONG_PTR>(ownerHwnd));
            // Keep WS_POPUP. WS_CHILD + cross-process bottoms out at the parent's
            // XAML island Z-order, which is the bug we're working around.
            LONG_PTR style = GetWindowLongPtrW(childHwnd, GWL_STYLE);
            style |= WS_POPUP | WS_CLIPCHILDREN;
            style &= ~(WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU);
            SetWindowLongPtrW(childHwnd, GWL_STYLE, style);
            SetWindowPos(childHwnd, nullptr, sx, sy, w, h,
                SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
            // NOTE: no ShowWindow here. See AttachInputNow.
        }

        // Move the owned popup to follow the parent's Border. Cheap to call from
        // size/move handlers. Short-circuits when the rect hasn't changed since
        // the last call — every SetWindowPos cross-process triggers
        // WM_WINDOWPOSCHANGED / WM_SIZE / WM_PAINT in the child, which is the
        // dominant cause of tab-switch flicker even when geometry is identical.
        void Reposition(int sx, int sy, int w, int h)
        {
            if (!childHwnd) return;
            if (sx == lastSx && sy == lastSy && w == lastW && h == lastH)
            {
                HyprvAppLog(L"[hc] Reposition CACHED %d,%d %dx%d (no-op)", sx, sy, w, h);
                return;
            }
            HyprvAppLog(L"[hc] Reposition %d,%d %dx%d (was %d,%d %dx%d) -> SetWindowPos",
                sx, sy, w, h, lastSx, lastSy, lastW, lastH);
            lastSx = sx; lastSy = sy; lastW = w; lastH = h;
            SetWindowPos(childHwnd, nullptr, sx, sy, w, h,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        bool inputAttached = false;

        void AttachInputNow()
        {
            if (!childHwnd) return;
            if (inputAttached) return;
            const DWORD childThread = GetWindowThreadProcessId(childHwnd, nullptr);
            if (childThread && childThread != GetCurrentThreadId())
            {
                AttachThreadInput(GetCurrentThreadId(), childThread, TRUE);
                SetFocus(childHwnd);
                inputAttached = true;
            }
        }

        void ShowNow()
        {
            if (childHwnd)
            {
                RECT r{};
                GetWindowRect(childHwnd, &r);
                HyprvAppLog(L"[hc] ShowNow childHwnd=%p rect=%d,%d-%d,%d (%dx%d)",
                    childHwnd, r.left, r.top, r.right, r.bottom,
                    r.right - r.left, r.bottom - r.top);
                ShowWindow(childHwnd, SW_SHOWNOACTIVATE);
            }
        }

        void HideNow()
        {
            if (childHwnd)
            {
                HyprvAppLog(L"[hc] HideNow childHwnd=%p", childHwnd);
                ShowWindow(childHwnd, SW_HIDE);
            }
        }

        void FocusNow()
        {
            if (!childHwnd) return;
            // Cross-process SetFocus only works when our thread is attached
            // to the child's input queue. AttachInputNow handles the
            // idempotent attach; we always re-call SetFocus afterwards so
            // tab-switch-back can re-focus the popup even if attachment
            // happened ages ago at OnConnected time.
            if (!inputAttached)
            {
                const DWORD childThread = GetWindowThreadProcessId(childHwnd, nullptr);
                if (childThread && childThread != GetCurrentThreadId())
                {
                    AttachThreadInput(GetCurrentThreadId(), childThread, TRUE);
                    inputAttached = true;
                }
            }
            SetFocus(childHwnd);
        }

        void DetachInputNow()
        {
            if (!inputAttached || !childHwnd) return;
            const DWORD childThread = GetWindowThreadProcessId(childHwnd, nullptr);
            if (childThread && childThread != GetCurrentThreadId())
            {
                AttachThreadInput(GetCurrentThreadId(), childThread, FALSE);
                inputAttached = false;
            }
        }

        void StartRxLoop()
        {
            HyprvAppLog(L"[hc] StartRxLoop pipe=%p", pipe);
            rxRunning = true;
            rxThread = std::thread([this]
            {
                HyprvAppLog(L"[rx] thread started pipe=%p tid=%lu", pipe, GetCurrentThreadId());
                while (rxRunning.load(std::memory_order_relaxed))
                {
                    hyprv::ipc::Header h{};
                    if (!ReadExact(pipe, &h, sizeof(h)))
                    {
                        HyprvAppLog(L"[rx] header read failed err=%lu pipe=%p, exiting loop", GetLastError(), pipe);
                        break;
                    }
                    HyprvAppLog(L"[rx] got header type=%u size=%u", h.type, h.payloadSize);

                    std::vector<uint8_t> payload;
                    if (h.payloadSize > 0)
                    {
                        payload.resize(h.payloadSize);
                        if (!ReadExact(pipe, payload.data(), h.payloadSize))
                        {
                            HyprvAppLog(L"[rx] payload read failed err=%lu", GetLastError());
                            break;
                        }
                    }
                    DispatchEvent(h, payload);
                }
                HyprvAppLog(L"[rx] thread exiting");
                // Wait briefly for the child to exit so OnChildExited reports the right code.
                if (procInfo.hProcess && owner && owner->OnChildExited)
                {
                    WaitForSingleObject(procInfo.hProcess, 1000);
                    DWORD code = 0;
                    GetExitCodeProcess(procInfo.hProcess, &code);
                    owner->OnChildExited(code);
                }
            });
        }

        void DispatchEvent(hyprv::ipc::Header const& h, std::vector<uint8_t> const& payload)
        {
            HyprvAppLog(L"[rx] DispatchEvent type=%u payload=%zu", h.type, payload.size());
            if (!owner) { HyprvAppLog(L"[rx] no owner — drop"); return; }
            switch (static_cast<hyprv::ipc::C2P>(h.type))
            {
            case hyprv::ipc::C2P::Connecting:
                HyprvAppLog(L"[rx] -> OnConnecting hooked=%d", owner->OnConnecting ? 1 : 0);
                if (owner->OnConnecting) owner->OnConnecting();
                break;
            case hyprv::ipc::C2P::Connected:
                HyprvAppLog(L"[rx] -> OnConnected hooked=%d payloadOk=%d",
                    owner->OnConnected ? 1 : 0,
                    (payload.size() >= sizeof(hyprv::ipc::Connected)) ? 1 : 0);
                if (payload.size() >= sizeof(hyprv::ipc::Connected) && owner->OnConnected)
                {
                    hyprv::ipc::Connected c{};
                    memcpy(&c, payload.data(), sizeof(c));
                    owner->OnConnected({ c.enhancedActive != 0, c.desktopWidth, c.desktopHeight });
                    HyprvAppLog(L"[rx] OnConnected returned");
                }
                break;
            case hyprv::ipc::C2P::EnhancedReady:
                if (payload.size() >= sizeof(hyprv::ipc::EnhancedReady) && owner->OnEnhancedReady)
                {
                    hyprv::ipc::EnhancedReady e{};
                    memcpy(&e, payload.data(), sizeof(e));
                    owner->OnEnhancedReady(e.ready != 0);
                }
                break;
            case hyprv::ipc::C2P::Disconnected:
                if (payload.size() >= sizeof(hyprv::ipc::Disconnected) && owner->OnDisconnected)
                {
                    hyprv::ipc::Disconnected d{};
                    memcpy(&d, payload.data(), sizeof(d));
                    // Trailing UTF-8 description (present only for fatal drops).
                    std::wstring desc;
                    const size_t avail = payload.size() - sizeof(d);
                    const size_t nb = (d.descByteLen <= avail) ? d.descByteLen : avail;
                    if (nb > 0)
                    {
                        const char* src = reinterpret_cast<const char*>(
                            payload.data() + sizeof(d));
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, src,
                            static_cast<int>(nb), nullptr, 0);
                        if (wlen > 0)
                        {
                            desc.resize(static_cast<size_t>(wlen));
                            MultiByteToWideChar(CP_UTF8, 0, src,
                                static_cast<int>(nb), desc.data(), wlen);
                        }
                    }
                    DisconnectedInfo info{};
                    info.discReason     = d.discReason;
                    info.extendedReason = d.extendedReason;
                    info.fatal          = (d.fatal != 0);
                    info.description    = std::move(desc);
                    owner->OnDisconnected(info);
                }
                break;
            case hyprv::ipc::C2P::DesktopResized:
                if (payload.size() >= sizeof(hyprv::ipc::DesktopResized) && owner->OnDesktopResized)
                {
                    hyprv::ipc::DesktopResized r{};
                    memcpy(&r, payload.data(), sizeof(r));
                    owner->OnDesktopResized(r.width, r.height);
                }
                break;
            case hyprv::ipc::C2P::Error:
                if (payload.size() >= sizeof(hyprv::ipc::Error) && owner->OnError)
                {
                    hyprv::ipc::Error e{};
                    memcpy(&e, payload.data(), sizeof(e));
                    owner->OnError(e.code);
                }
                break;
            case hyprv::ipc::C2P::LogLine:
                if (payload.size() >= sizeof(hyprv::ipc::LogLine))
                {
                    hyprv::ipc::LogLine head{};
                    memcpy(&head, payload.data(), sizeof(head));
                    const size_t avail = payload.size() - sizeof(hyprv::ipc::LogLine);
                    const size_t nb = (head.byteLen <= avail) ? head.byteLen : avail;
                    // UTF-8 → UTF-16 for HyprvAppLog (which is wide).
                    int wlen = MultiByteToWideChar(CP_UTF8, 0,
                        reinterpret_cast<const char*>(payload.data() + sizeof(hyprv::ipc::LogLine)),
                        static_cast<int>(nb), nullptr, 0);
                    if (wlen > 0)
                    {
                        std::wstring text(static_cast<size_t>(wlen), L'\0');
                        MultiByteToWideChar(CP_UTF8, 0,
                            reinterpret_cast<const char*>(payload.data() + sizeof(hyprv::ipc::LogLine)),
                            static_cast<int>(nb), text.data(), wlen);
                        // Prefer the friendly label (set by VmTabPage from the
                        // VM's elementName) over the bare pid — easier to read
                        // when multiple rdphost children are interleaving.
                        if (!logLabel.empty())
                            HyprvAppLog(L"[rdphost %s pid=%lu] %s",
                                logLabel.c_str(), procInfo.dwProcessId, text.c_str());
                        else
                            HyprvAppLog(L"[rdphost pid=%lu] %s",
                                procInfo.dwProcessId, text.c_str());
                    }
                }
                break;
            default:
                break;
            }
        }

        void Cleanup()
        {
            HyprvAppLog(L"[hc] Cleanup tid=%lu pipe=%p", GetCurrentThreadId(), pipe);
            rxRunning = false;
            if (pipe != INVALID_HANDLE_VALUE)
            {
                // CancelIoEx for the overlapped read pending on the rx thread.
                CancelIoEx(pipe, nullptr);
            }
            if (rxThread.joinable()) rxThread.join();
            if (pipe != INVALID_HANDLE_VALUE)
            {
                HyprvAppLog(L"[hc] Cleanup CloseHandle pipe=%p", pipe);
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
            }
            if (procInfo.hProcess)
            {
                // Wait briefly for clean exit, terminate if needed.
                if (WaitForSingleObject(procInfo.hProcess, 200) == WAIT_TIMEOUT)
                    TerminateProcess(procInfo.hProcess, 0);
                CloseHandle(procInfo.hProcess);
                CloseHandle(procInfo.hThread);
                procInfo = {};
            }
        }
    };

    RdpHostClient::RdpHostClient(std::wstring exe) : m_impl(std::make_unique<Impl>(std::move(exe))) {
        m_impl->owner = this;
    }
    RdpHostClient::~RdpHostClient() { Stop(); }

    HWND RdpHostClient::childHwnd() const { return m_impl ? m_impl->childHwnd : nullptr; }

    bool RdpHostClient::ConnectLocalVm(HWND ownerHwnd, int sx, int sy, int w, int h,
                                       GUID const& vmGuid,
                                       hyprv::ipc::RdpOptions const& opts)
    {
        if (!m_impl->SpawnAndHandshake()) return false;

        m_impl->EmbedInto(ownerHwnd, sx, sy, w, h);

        // Build ConnectLocalVm payload: ConnectLocalVm struct + server bytes + domain bytes.
        const std::string server = "localhost";
        hyprv::ipc::ConnectLocalVm cmd{};
        memcpy(cmd.vmGuid, &vmGuid, sizeof(cmd.vmGuid));
        cmd.options = opts;
        cmd.options.serverByteLen = static_cast<uint32_t>(server.size());
        cmd.options.domainByteLen = 0;

        std::vector<uint8_t> buf(sizeof(cmd) + server.size());
        memcpy(buf.data(), &cmd, sizeof(cmd));
        memcpy(buf.data() + sizeof(cmd), server.data(), server.size());

        if (!m_impl->SendP2C(hyprv::ipc::P2C::ConnectLocalVm, buf.data(), static_cast<uint32_t>(buf.size())))
            return false;

        m_impl->StartRxLoop();
        return true;
    }

    bool RdpHostClient::Connect(HWND ownerHwnd, int sx, int sy, int w, int h,
                                hyprv::ipc::RdpOptions const& opts,
                                std::wstring const& server, std::wstring const& domain,
                                std::wstring const& username)
    {
        if (!m_impl->SpawnAndHandshake()) return false;

        m_impl->EmbedInto(ownerHwnd, sx, sy, w, h);

        // UTF-16 -> UTF-8 for the wire strings.
        auto toUtf8 = [](std::wstring const& w) {
            if (w.empty()) return std::string{};
            int sz = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(sz), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), sz, nullptr, nullptr);
            return s;
        };
        const std::string serverBytes = toUtf8(server);
        const std::string domainBytes = toUtf8(domain);
        const std::string userBytes   = toUtf8(username);

        hyprv::ipc::Connect cmd{};
        cmd.options = opts;
        cmd.options.serverByteLen = static_cast<uint32_t>(serverBytes.size());
        cmd.options.domainByteLen = static_cast<uint32_t>(domainBytes.size());
        cmd.options.userByteLen   = static_cast<uint32_t>(userBytes.size());

        std::vector<uint8_t> buf(sizeof(cmd) + serverBytes.size() + domainBytes.size()
                                 + userBytes.size());
        size_t off = 0;
        memcpy(buf.data() + off, &cmd, sizeof(cmd));                 off += sizeof(cmd);
        memcpy(buf.data() + off, serverBytes.data(), serverBytes.size()); off += serverBytes.size();
        memcpy(buf.data() + off, domainBytes.data(), domainBytes.size()); off += domainBytes.size();
        memcpy(buf.data() + off, userBytes.data(),   userBytes.size());   off += userBytes.size();

        if (!m_impl->SendP2C(hyprv::ipc::P2C::Connect, buf.data(), static_cast<uint32_t>(buf.size())))
            return false;

        m_impl->StartRxLoop();
        return true;
    }

    void RdpHostClient::Disconnect()
    {
        // Invalidate the geometry cache so the next Connect's first Reposition
        // / UpdateSessionDisplaySettings actually fires (the child resets state
        // server-side; cached rect from the prior session is no longer valid).
        m_impl->lastSx = INT_MIN; m_impl->lastSy = INT_MIN;
        m_impl->lastW  = 0;       m_impl->lastH  = 0;
        m_impl->lastDsW = 0; m_impl->lastDsH = 0; m_impl->lastDsDpiX = 0;
        m_impl->SendP2C(hyprv::ipc::P2C::Disconnect, nullptr, 0);
    }

    void RdpHostClient::InvalidateSessionDisplayCache()
    {
        if (!m_impl) return;
        m_impl->lastDsW = 0;
        m_impl->lastDsH = 0;
        m_impl->lastDsDpiX = 0;
    }

    void RdpHostClient::SetLogLabel(std::wstring label)
    {
        if (!m_impl) return;
        m_impl->logLabel = std::move(label);
    }

    void RdpHostClient::Resize(uint32_t w, uint32_t h)
    {
        hyprv::ipc::Resize r{ static_cast<uint16_t>(w), static_cast<uint16_t>(h) };
        m_impl->SendP2C(hyprv::ipc::P2C::Resize, &r, sizeof(r));
    }

    void RdpHostClient::UpdateSessionDisplaySettings(uint32_t w, uint32_t h, uint32_t dpiX, uint32_t /*dpiY*/)
    {
        // Short-circuit when nothing meaningful changed. The IPC frame forwards
        // to mstscax → server, which re-issues a Display PDU even when the
        // settings are identical — the dominant cost of tab-switch flicker.
        if (w == m_impl->lastDsW && h == m_impl->lastDsH && dpiX == m_impl->lastDsDpiX)
            return;
        m_impl->lastDsW    = w;
        m_impl->lastDsH    = h;
        m_impl->lastDsDpiX = dpiX;
        hyprv::ipc::UpdateSessionDisplaySettings u{};
        u.width = w;
        u.height = h;
        u.physWidth = static_cast<uint32_t>(w * 25.4 / dpiX);
        u.physHeight = static_cast<uint32_t>(h * 25.4 / dpiX);
        u.orientation = 0;
        // desktopScale = the guest's UI scale. rdphost is PerMonitorV2 and w/h
        // are PHYSICAL pixels, so the guest MUST scale to match the host — a
        // hardcoded 100 makes it render its UI at 100% across a physical-res
        // desktop and everything inside the VM is tiny while the host is scaled
        // (the reported "scaling inside the VM doesn't match the host" bug).
        // Derive the host scale percent from the DPI the parent passed (dpiX,
        // 96-based: 144→150%) and snap to an RDP-valid desktopScaleFactor.
        u.desktopScale = SnapRdpScalePercent((dpiX * 100u + 48u) / 96u);
        u.deviceScale = 100;
        m_impl->SendP2C(hyprv::ipc::P2C::UpdateSessionDisplaySettings, &u, sizeof(u));
    }

    void RdpHostClient::SetEnhanced(bool enhanced)
    {
        hyprv::ipc::SetEnhanced s{ enhanced ? uint8_t{1} : uint8_t{0}, {0,0,0} };
        m_impl->SendP2C(hyprv::ipc::P2C::SetEnhanced, &s, sizeof(s));
    }

    void RdpHostClient::AttachInput() { if (m_impl) m_impl->AttachInputNow(); }
    void RdpHostClient::DetachInput() { if (m_impl) m_impl->DetachInputNow(); }
    void RdpHostClient::Focus()       { if (m_impl) m_impl->FocusNow(); }
    void RdpHostClient::Reposition(int sx, int sy, int w, int h)
    {
        if (m_impl) m_impl->Reposition(sx, sy, w, h);
    }
    void RdpHostClient::Show() { if (m_impl) m_impl->ShowNow(); }
    void RdpHostClient::Hide() { if (m_impl) m_impl->HideNow(); }

    void RdpHostClient::Reown(HWND newOwner)
    {
        if (!m_impl || !m_impl->childHwnd) return;
        HyprvAppLog(L"[hc] Reown childHwnd=%p newOwner=%p", m_impl->childHwnd, newOwner);
        // Same GWLP_HWNDPARENT owner set that EmbedInto did at connect — Win32
        // resolves the HWND cross-process, so re-owning to another window in
        // this process is a single SetWindowLongPtr.
        SetWindowLongPtrW(m_impl->childHwnd, GWLP_HWNDPARENT,
            reinterpret_cast<LONG_PTR>(newOwner));
        // The new owner can have the same Border-relative geometry, so clear the
        // Reposition cache or the next UpdateRdphostBounds would short-circuit and
        // leave the popup parented-but-mispositioned (cf. Disconnect's reset).
        m_impl->lastSx = INT_MIN; m_impl->lastSy = INT_MIN;
        m_impl->lastW  = 0;       m_impl->lastH  = 0;
    }

    void RdpHostClient::Stop()
    {
        if (!m_impl) return;
        // Try graceful shutdown first.
        if (m_impl->pipe != INVALID_HANDLE_VALUE)
            m_impl->SendP2C(hyprv::ipc::P2C::Shutdown, nullptr, 0);
        if (m_impl->procInfo.hProcess)
        {
            if (WaitForSingleObject(m_impl->procInfo.hProcess, kShutdownWait) == WAIT_TIMEOUT)
                TerminateProcess(m_impl->procInfo.hProcess, 0);
        }
        m_impl->Cleanup();
    }
}
