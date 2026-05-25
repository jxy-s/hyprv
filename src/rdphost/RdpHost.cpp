#include "RdpHost.h"
#include "IpcClient.h"

#include <atlbase.h>
#include <atlcom.h>
#include <atlhost.h>
#include <comdef.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

extern void HyprvLog(const wchar_t* fmt, ...);

#pragma warning(push)
#pragma warning(disable: 4192 4471 4278)
#import "C:\\Windows\\System32\\mstscax.dll" raw_interfaces_only no_namespace named_guids
#pragma warning(pop)

namespace hyprv::rdphost
{
    // The IID the control's connection point actually exposes for events.
    // `#import`'s DIID_IMsTscAxEvents has a different value — AtlAdvise on that fails.
    // See memory/hyprv-rdphost-gotchas.md.
    static const IID IID_IMsTscAxEventsReal =
        { 0x336D5562, 0xEFA8, 0x482E, { 0x8C, 0xB3, 0xC5, 0xC0, 0xFC, 0x7A, 0x7D, 0xB6 } };

    // IMsTscAxEvents DISPIDs we care about.
    enum : DISPID
    {
        DISPID_OnConnecting             = 1,
        DISPID_OnConnected              = 2,
        DISPID_OnLoginComplete          = 3,
        DISPID_OnDisconnected           = 4,    // arg: long discReason
        DISPID_OnEnterFullScreenMode    = 5,
        DISPID_OnLeaveFullScreenMode    = 6,
        DISPID_OnRemoteDesktopSizeChange = 12,  // args: long width, long height
    };

    // ----- Forward declaration so Impl can reference DispSink ---------------------
    struct ImplCallbacks;

    // Minimal IDispatch sink. Forwards events to ImplCallbacks (which owns the IPC).
    class DispSink : public IDispatch
    {
    public:
        explicit DispSink(ImplCallbacks* cb) : m_cb(cb) {}

        STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override
        {
            if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IMsTscAxEventsReal)
            {
                *ppv = static_cast<IDispatch*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
        STDMETHODIMP_(ULONG) Release() override
        {
            LONG r = InterlockedDecrement(&m_ref);
            if (!r) delete this;
            return r;
        }
        STDMETHODIMP GetTypeInfoCount(UINT*) override { return E_NOTIMPL; }
        STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
        STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
        STDMETHODIMP Invoke(DISPID dispid, REFIID, LCID, WORD, DISPPARAMS* dp,
                            VARIANT*, EXCEPINFO*, UINT*) override;

    private:
        LONG            m_ref = 1;
        ImplCallbacks*  m_cb;
    };

    // ----- The pimpl --------------------------------------------------------------
    struct ImplCallbacks
    {
        virtual void OnEvActConnecting() = 0;
        virtual void OnEvActConnected() = 0;
        virtual void OnEvActLoginComplete() = 0;
        virtual void OnEvActDisconnected(long discReason) = 0;
        virtual void OnEvActDesktopSizeChange(long w, long h) = 0;
        virtual ~ImplCallbacks() = default;
    };

    struct RdpHost::Impl : public ImplCallbacks
    {
        IpcClient&                       ipc;
        HWND                             ax = nullptr;
        CComPtr<IUnknown>                unk;
        CComQIPtr<IMsRdpClient9>         rdp;
        DispSink*                        sink = nullptr;
        DWORD                            eventCookie = 0;
        bool                             enhancedRequested = false;
        bool                             enhancedActive = false;
        int                              sessionWidth = 0;
        int                              sessionHeight = 0;

        explicit Impl(IpcClient& ref) : ipc(ref) {}

        ~Impl() override
        {
            StopSenderThread();
            if (eventCookie && unk)
            {
                CComPtr<IConnectionPointContainer> cpc;
                if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&cpc))) && cpc)
                {
                    CComPtr<IConnectionPoint> cp;
                    if (SUCCEEDED(cpc->FindConnectionPoint(IID_IMsTscAxEventsReal, &cp)) && cp)
                        cp->Unadvise(eventCookie);
                }
                eventCookie = 0;
            }
            rdp.Release();
            unk.Release();
        }

        bool ActivateAx(HWND parent, int w, int h)
        {
            // ATL 14.x v143 registers "AtlAxWin140". v145 likely differs — verify per toolset.
            ax = CreateWindowExW(0, L"AtlAxWin140", L"MsTscAx.MsTscAx.9",
                WS_CHILD | WS_VISIBLE, 0, 0, w, h, parent, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            if (!ax) { OutputDebugStringW(L"[rdphost] AtlAxWin create failed\n"); return false; }

            if (FAILED(AtlAxGetControl(ax, &unk)) || !unk)
            {
                OutputDebugStringW(L"[rdphost] AtlAxGetControl failed\n");
                return false;
            }
            rdp = unk;
            if (!rdp) { OutputDebugStringW(L"[rdphost] QI IMsRdpClient9 failed\n"); return false; }

            // Bind the event sink via the IMsTscAxEvents connection point.
            CComPtr<IConnectionPointContainer> cpc;
            if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&cpc))) || !cpc)
            {
                OutputDebugStringW(L"[rdphost] no IConnectionPointContainer\n");
                return false;
            }
            CComPtr<IConnectionPoint> cp;
            if (FAILED(cpc->FindConnectionPoint(IID_IMsTscAxEventsReal, &cp)) || !cp)
            {
                OutputDebugStringW(L"[rdphost] no IMsTscAxEvents connection point\n");
                return false;
            }
            sink = new DispSink(this);
            HRESULT hr = cp->Advise(static_cast<IDispatch*>(sink), &eventCookie);
            sink->Release();
            HyprvLog(L"[host] Advise hr=0x%08lx cookie=%lu", hr, eventCookie);
            if (FAILED(hr)) return false;
            StartSenderThread();
            return true;
        }

        // (Kept signature for future use, but OnConnectLocalVm now configures inline
        // so the property-setting order matches the spike exactly — PCB must precede
        // the secure-mode setup or the ActiveX silently stops emitting events.)
        void ConfigureForLocalVm(const hyprv::ipc::RdpOptions& /*opt*/) {}

        // Outgoing event queue — mstscax events arrive on the UI thread INSIDE a
        // nested KiUserCallback chain (SendMessage from a worker → UI's GetMessage
        // callback → ActiveX wndproc → our sink). Calling WriteFile from inside that
        // chain deadlocks (the kernel can't complete IO back to a thread that's
        // currently mid-callback). A dedicated sender thread drains this queue
        // outside the message-dispatch chain, so WriteFile is free to block.
        std::mutex                          outMutex;
        std::condition_variable             outCv;
        std::deque<std::vector<uint8_t>>    outQueue;     // each entry = full framed message
        std::atomic<bool>                   outRunning{ false };
        std::thread                         outThread;

        void StartSenderThread()
        {
            HyprvLog(L"[host] StartSenderThread");
            outRunning = true;
            outThread = std::thread([this]
            {
                HyprvLog(L"[tx] thread started tid=%lu", GetCurrentThreadId());
                while (outRunning.load())
                {
                    std::vector<uint8_t> msg;
                    {
                        std::unique_lock<std::mutex> lk(outMutex);
                        HyprvLog(L"[tx] wait, queue size=%zu", outQueue.size());
                        outCv.wait(lk, [this] { return !outQueue.empty() || !outRunning.load(); });
                        if (!outRunning.load()) { HyprvLog(L"[tx] stop requested"); break; }
                        msg = std::move(outQueue.front());
                        outQueue.pop_front();
                    }
                    // Send the pre-framed buffer in one WriteFile via SendRaw helper.
                    uint8_t type = msg.size() >= sizeof(hyprv::ipc::Header)
                        ? reinterpret_cast<hyprv::ipc::Header*>(msg.data())->type
                        : 0xFF;
                    HyprvLog(L"[tx] sending type=%u bytes=%zu", type, msg.size());
                    bool ok = ipc.SendRaw(msg.data(), static_cast<uint32_t>(msg.size()));
                    HyprvLog(L"[tx] sent type=%u ok=%d", type, ok ? 1 : 0);
                }
                HyprvLog(L"[tx] thread exiting");
            });
        }

        void StopSenderThread()
        {
            outRunning = false;
            outCv.notify_all();
            if (outThread.joinable()) outThread.join();
        }

        void QueueOutbound(hyprv::ipc::C2P type, void const* payload, uint32_t payloadSize)
        {
            std::vector<uint8_t> buf(sizeof(hyprv::ipc::Header) + payloadSize);
            auto* h = reinterpret_cast<hyprv::ipc::Header*>(buf.data());
            *h = {};
            h->type = static_cast<uint8_t>(type);
            h->payloadSize = payloadSize;
            if (payloadSize) memcpy(buf.data() + sizeof(*h), payload, payloadSize);
            {
                std::lock_guard<std::mutex> lk(outMutex);
                outQueue.push_back(std::move(buf));
            }
            outCv.notify_one();
        }

        // --- ImplCallbacks: turn ActiveX events into queued IPC notifications -----
        void OnEvActConnecting() override
        {
            QueueOutbound(hyprv::ipc::C2P::Connecting, nullptr, 0);
            HyprvLog(L"[cb] Connecting (queued)");
        }
        void OnEvActConnected() override
        {
            long width = 0, height = 0;
            rdp->get_DesktopWidth(&width);
            rdp->get_DesktopHeight(&height);
            hyprv::ipc::Connected c{};
            c.enhancedActive = enhancedActive ? 1 : 0;
            c.desktopWidth = static_cast<uint32_t>(width);
            c.desktopHeight = static_cast<uint32_t>(height);
            QueueOutbound(hyprv::ipc::C2P::Connected, &c, sizeof(c));
            // Diag: also capture popup + AtlAxWin actual rects at Connected.
            if (ax)
            {
                HWND popup = GetParent(ax);
                RECT pr{}, axr{};
                if (popup) GetWindowRect(popup, &pr);
                GetWindowRect(ax, &axr);
                HyprvLog(L"[cb] Connected %ux%u | popup=%dx%d ax=%dx%d enh=%d",
                    c.desktopWidth, c.desktopHeight,
                    pr.right - pr.left, pr.bottom - pr.top,
                    axr.right - axr.left, axr.bottom - axr.top,
                    enhancedActive ? 1 : 0);
            }
            else
            {
                HyprvLog(L"[cb] Connected %ux%u (queued)", c.desktopWidth, c.desktopHeight);
            }
        }
        void OnEvActLoginComplete() override
        {
            hyprv::ipc::EnhancedReady r{ enhancedActive ? uint8_t{1} : uint8_t{0}, {0,0,0} };
            QueueOutbound(hyprv::ipc::C2P::EnhancedReady, &r, sizeof(r));
            // Diag: capture popup + AtlAxWin rects + desktop dims at LoginComplete.
            if (ax)
            {
                HWND popup = GetParent(ax);
                RECT pr{}, axr{};
                if (popup) GetWindowRect(popup, &pr);
                GetWindowRect(ax, &axr);
                long w = 0, h = 0;
                rdp->get_DesktopWidth(&w);
                rdp->get_DesktopHeight(&h);
                HyprvLog(L"[cb] LoginComplete | popup=%dx%d ax=%dx%d desktopW/H=%ldx%ld",
                    pr.right - pr.left, pr.bottom - pr.top,
                    axr.right - axr.left, axr.bottom - axr.top, w, h);
            }
            else
            {
                HyprvLog(L"[cb] EnhancedReady (queued)");
            }
        }
        void OnEvActDisconnected(long discReason) override
        {
            ExtendedDisconnectReasonCode ext = exDiscReasonNoInfo;
            rdp->get_ExtendedDisconnectReason(&ext);

            // Classify: mstscax disconnect-reason codes 0..3 (NoInfo /
            // LocalNotError / RemoteByUser / ByServer) are benign/expected —
            // a clean logoff, a server-initiated drop, or a guest reboot.
            // Anything > 3 (socket failure, timeout, auth/cert, protocol) is a
            // real error the parent may want to surface. The parent additionally
            // gates on whether the session ever reached Connected, so a benign
            // reason on an established session still just reconnects.
            const bool fatal = (discReason > 3);

            // Pull the same localized description string mstscax itself shows,
            // but only when the disconnect is an error — for benign reasons the
            // text is generic and the parent never displays it, so we keep the
            // common reboot/logoff frame small (descByteLen = 0).
            std::string descUtf8;
            if (fatal)
            {
                BSTR bstr = nullptr;
                HRESULT ghr = rdp->GetErrorDescription(
                    static_cast<UINT>(discReason), static_cast<UINT>(ext), &bstr);
                if (SUCCEEDED(ghr) && bstr)
                {
                    const int wlen = static_cast<int>(SysStringLen(bstr));
                    if (wlen > 0)
                    {
                        const int n = WideCharToMultiByte(
                            CP_UTF8, 0, bstr, wlen, nullptr, 0, nullptr, nullptr);
                        if (n > 0)
                        {
                            descUtf8.resize(static_cast<size_t>(n));
                            WideCharToMultiByte(CP_UTF8, 0, bstr, wlen,
                                descUtf8.data(), n, nullptr, nullptr);
                        }
                    }
                }
                if (bstr) SysFreeString(bstr);
            }

            // Build the framed payload: fixed Disconnected struct + UTF-8 desc.
            hyprv::ipc::Disconnected d{};
            d.discReason     = static_cast<int32_t>(discReason);
            d.extendedReason = static_cast<int32_t>(ext);
            d.fatal          = fatal ? 1 : 0;
            d.descByteLen    = static_cast<uint32_t>(descUtf8.size());

            std::vector<uint8_t> buf(sizeof(d) + descUtf8.size());
            memcpy(buf.data(), &d, sizeof(d));
            if (!descUtf8.empty())
                memcpy(buf.data() + sizeof(d), descUtf8.data(), descUtf8.size());
            QueueOutbound(hyprv::ipc::C2P::Disconnected,
                buf.data(), static_cast<uint32_t>(buf.size()));

            HyprvLog(L"[cb] Disconnected disc=%ld ext=%ld fatal=%d (queued)",
                discReason, static_cast<long>(ext), fatal ? 1 : 0);
        }
        void OnEvActDesktopSizeChange(long w, long h) override
        {
            sessionWidth  = static_cast<int>(w);
            sessionHeight = static_cast<int>(h);
            hyprv::ipc::DesktopResized r{ static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
            QueueOutbound(hyprv::ipc::C2P::DesktopResized, &r, sizeof(r));
            // Diag: capture popup + AtlAxWin actual sizes when mstscax tells
            // us the SESSION size changed. Useful for catching the case where
            // mstscax reports a different size than what we've made the popup.
            if (ax)
            {
                HWND popup = GetParent(ax);
                RECT pr{}, axr{};
                if (popup) GetWindowRect(popup, &pr);
                GetWindowRect(ax, &axr);
                HyprvLog(L"[cb] DesktopResized session=%ldx%ld | popup=%dx%d ax=%dx%d enh=%d",
                    w, h,
                    pr.right - pr.left, pr.bottom - pr.top,
                    axr.right - axr.left, axr.bottom - axr.top,
                    enhancedActive ? 1 : 0);
            }
            else
            {
                HyprvLog(L"[cb] DesktopResized %ldx%ld (queued)", w, h);
            }
        }
    };

    // ----- DispSink::Invoke -- routes ActiveX events to ImplCallbacks --------------
    STDMETHODIMP DispSink::Invoke(DISPID dispid, REFIID, LCID, WORD, DISPPARAMS* dp,
                                  VARIANT*, EXCEPINFO*, UINT*)
    {
        HyprvLog(L"[sink] DISPID=%ld cArgs=%u", static_cast<long>(dispid), dp ? dp->cArgs : 0u);
        if (!m_cb) return S_OK;
        switch (dispid)
        {
        case DISPID_OnConnecting:    m_cb->OnEvActConnecting(); break;
        case DISPID_OnConnected:     m_cb->OnEvActConnected(); break;
        case DISPID_OnLoginComplete: m_cb->OnEvActLoginComplete(); break;
        case DISPID_OnDisconnected:
            if (dp && dp->cArgs >= 1 && dp->rgvarg[0].vt == VT_I4)
                m_cb->OnEvActDisconnected(dp->rgvarg[0].lVal);
            else
                m_cb->OnEvActDisconnected(0);
            break;
        case DISPID_OnRemoteDesktopSizeChange:
            if (dp && dp->cArgs >= 2 && dp->rgvarg[1].vt == VT_I4 && dp->rgvarg[0].vt == VT_I4)
            {
                // DISPPARAMS args are in reverse order; rgvarg[1] is width, rgvarg[0] is height.
                m_cb->OnEvActDesktopSizeChange(dp->rgvarg[1].lVal, dp->rgvarg[0].lVal);
            }
            break;
        default: break;
        }
        return S_OK;
    }

    // ----- RdpHost public surface --------------------------------------------------
    RdpHost::RdpHost(IpcClient& ipc) : m_impl(new Impl(ipc)) {}
    RdpHost::~RdpHost() { delete m_impl; }

    bool RdpHost::Activate(HWND parent, int w, int h)
    {
        return m_impl->ActivateAx(parent, w, h);
    }

    HWND RdpHost::ax() const { return m_impl->ax; }

    void RdpHost::GetSessionSize(int& width, int& height) const
    {
        width  = m_impl->sessionWidth;
        height = m_impl->sessionHeight;
    }

    // UTF-8 (wire) -> UTF-16 (Win32/COM). Server/username/domain cross the pipe
    // as UTF-8; mstscax wants BSTR.
    static std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            w.data(), n);
        return w;
    }

    // Generic RDP to an external host (a saved "Remote Host"). Mirrors the
    // generic property setup of OnConnectLocalVm but targets a real RDP server
    // on :3389 over the network instead of the Hyper-V VMBus console on :2179.
    // Key differences from the VM-console path:
    //   - put_Server(host) + RDPPort 3389 (vs "localhost" + 2179)
    //   - NO PCB / AuthenticationServiceClass / EnableFrameBufferRedirection /
    //     EnhancedMode (those are VMBus-console specific)
    //   - put_UserName / put_Domain to pre-fill the credential prompt
    //   - NLA (CredSSP) on + PromptForCredsOnClient so mstscax shows its own
    //     credential prompt (we deliberately do NOT carry a password — the
    //     "prompt every time" model; Windows' own "remember me" still works).
    void RdpHost::OnConnect(const hyprv::ipc::Connect& cmd,
                            const std::string& server, const std::string& domain,
                            const std::string& username)
    {
        if (!m_impl->rdp) { HyprvLog(L"[host] no IMsRdpClient9 — abort"); return; }
        const auto& opt = cmd.options;
        // Generic RDP has no enhanced/VMBus mode.
        m_impl->enhancedRequested = false;
        m_impl->enhancedActive    = false;
        m_impl->sessionWidth  = opt.desktopWidth  ? opt.desktopWidth  : 1024;
        m_impl->sessionHeight = opt.desktopHeight ? opt.desktopHeight : 768;

        const std::wstring wServer = Utf8ToWide(server);
        const std::wstring wDomain = Utf8ToWide(domain);
        const std::wstring wUser   = Utf8ToWide(username);
        HyprvLog(L"[host] OnConnect server=%s port=%u dw=%u dh=%u user=%s domain=%s flags=0x%x",
            wServer.c_str(), opt.port, opt.desktopWidth, opt.desktopHeight,
            wUser.c_str(), wDomain.c_str(), opt.flags);

        auto* rdp = m_impl->rdp.p;
        rdp->put_Server(CComBSTR(wServer.c_str()));
        rdp->put_ColorDepth(opt.colorDepth ? opt.colorDepth : 32);
        rdp->put_DesktopWidth(opt.desktopWidth ? opt.desktopWidth : 1024);
        rdp->put_DesktopHeight(opt.desktopHeight ? opt.desktopHeight : 768);
        // Deliberately do NOT pre-set put_UserName / put_Domain. With NLA +
        // PromptForCredsOnClient, supplying a username but no password makes the
        // control attempt an authentication pass with incomplete credentials
        // (or the logged-in user's SSO creds), the server rejects it, and the
        // credential dialog then appears carrying "The logon attempt failed."
        // Supplying NO credentials lets the control show a CLEAN credential
        // prompt first (what PromptForCredsOnClient is for). The saved username
        // therefore won't pre-fill the FIRST prompt; Windows remembers it via
        // its own TERMSRV/<host> store for subsequent connects. (wUser/wDomain
        // are still logged above for diagnostics.) See gotcha #43.
        (void)wUser; (void)wDomain;

        CComPtr<IMsRdpClientAdvancedSettings6> adv6;
        if (FAILED(rdp->get_AdvancedSettings7(&adv6)) || !adv6)
        {
            HyprvLog(L"[host] get_AdvancedSettings7 failed");
            hyprv::ipc::Error err{ static_cast<uint32_t>(hyprv::ipc::RdpErrorCode::HostStartupFailed) };
            m_impl->ipc.Send(hyprv::ipc::C2P::Error, &err, sizeof(err));
            return;
        }
        adv6->put_SmartSizing(VARIANT_FALSE);
        // Route Win/Win+R/Alt+Tab to the remote session (windowed host; gotcha #24).
        {
            CComPtr<IMsRdpClientSecuredSettings> sec;
            if (SUCCEEDED(rdp->get_SecuredSettings2(&sec)) && sec)
                sec->put_KeyboardHookMode(1);
        }
        CComQIPtr<IMsRdpClientAdvancedSettings7> adv7(adv6);
        if (adv7)
        {
            adv7->put_RDPPort(opt.port ? opt.port : 3389);
            adv7->put_GrabFocusOnConnect(VARIANT_FALSE);
        }
        // Server-identity verification: 0 = connect without warning even if the
        // server cert can't be validated. Matches the VM-console leniency and
        // suits homelab machines with self-signed certs. (A future per-host
        // "warn on cert" setting could raise this to 2.)
        adv6->put_AuthenticationLevel(0);

        // --- User-configurable RDP options (same mapping as the VM path) ---
        {
            UINT amode = opt.audioMode;
            if (amode > 2) amode = 0;
            adv6->put_AudioRedirectionMode(amode);
        }
        auto setBool = [&](HRESULT (STDMETHODCALLTYPE
                                IMsRdpClientAdvancedSettings::*setter)(VARIANT_BOOL),
                           uint32_t flag)
        {
            VARIANT_BOOL v = (opt.flags & flag) ? VARIANT_TRUE : VARIANT_FALSE;
            (adv6->*setter)(v);
        };
        setBool(&IMsRdpClientAdvancedSettings::put_RedirectDrives,
                hyprv::ipc::Flag_RedirectDrives);
        setBool(&IMsRdpClientAdvancedSettings::put_RedirectPrinters,
                hyprv::ipc::Flag_RedirectDevices);
        setBool(&IMsRdpClientAdvancedSettings::put_RedirectPorts,
                hyprv::ipc::Flag_RedirectPorts);
        setBool(&IMsRdpClientAdvancedSettings::put_RedirectSmartCards,
                hyprv::ipc::Flag_RedirectSmartCards);
        adv6->put_RedirectClipboard(
            (opt.flags & hyprv::ipc::Flag_RedirectClipboard) ? VARIANT_TRUE : VARIANT_FALSE);
        if (adv7)
            adv7->put_AudioCaptureRedirectionMode(
                (opt.flags & hyprv::ipc::Flag_AudioCaptureRedirect) ? VARIANT_TRUE : VARIANT_FALSE);

        // --- NLA / credential prompt ---
        // Enable CredSSP and ask mstscax to prompt for credentials on the client
        // side (we never carry a password). With a saved username pre-filled the
        // user only types the password; Windows' own credential store still
        // offers "remember me". EnableCredSspSupport lives on
        // IMsRdpClientNonScriptable3; PromptForCredsOnClient on NonScriptable4.
        CComQIPtr<IMsRdpClientNonScriptable3> ns3(m_impl->unk);
        if (ns3)
            ns3->put_EnableCredSspSupport(VARIANT_TRUE);
        CComQIPtr<IMsRdpClientNonScriptable4> ns4(m_impl->unk);
        if (ns4)
        {
            HRESULT pr = ns4->put_PromptForCredsOnClient(VARIANT_TRUE);
            HyprvLog(L"[host] put_PromptForCredsOnClient hr=0x%08lx", pr);
        }
        else
        {
            HyprvLog(L"[host] no IMsRdpClientNonScriptable4 — client cred prompt unavailable");
        }

        // DPI scale so the credential UI is sized for the parent's display.
        CComQIPtr<IMsRdpExtendedSettings> ext(m_impl->unk);
        if (ext)
        {
            const uint16_t dpiScale = opt.dpiScalePercent ? opt.dpiScalePercent : 100;
            CComVariant vDesktop; vDesktop.vt = VT_UI4; vDesktop.ulVal = dpiScale;
            ext->put_Property(CComBSTR(L"DesktopScaleFactor"), &vDesktop);
            CComVariant vDevice; vDevice.vt = VT_UI4; vDevice.ulVal = 100u;
            ext->put_Property(CComBSTR(L"DeviceScaleFactor"), &vDevice);
        }

        HyprvLog(L"[host] calling Connect() (generic RDP)");
        HRESULT hr = rdp->Connect();
        HyprvLog(L"[host] Connect() returned 0x%08lx", hr);
        if (FAILED(hr))
        {
            hyprv::ipc::Error err{ static_cast<uint32_t>(hyprv::ipc::RdpErrorCode::HostStartupFailed) };
            m_impl->ipc.Send(hyprv::ipc::C2P::Error, &err, sizeof(err));
        }
    }

    void RdpHost::OnConnectLocalVm(const hyprv::ipc::ConnectLocalVm& cmd,
                                   const std::string&, const std::string&)
    {
        if (!m_impl->rdp) { HyprvLog(L"[host] no IMsRdpClient9 — abort"); return; }
        const auto& opt = cmd.options;
        m_impl->enhancedRequested = (opt.flags & hyprv::ipc::Flag_EnhancedSession) != 0;
        m_impl->enhancedActive = false;
        m_impl->sessionWidth  = opt.desktopWidth  ? opt.desktopWidth  : 1024;
        m_impl->sessionHeight = opt.desktopHeight ? opt.desktopHeight : 768;

        // Format PCB ahead of time (matches spike's local std::wstring style).
        wchar_t guidStr[64];
        const auto& g = cmd.vmGuid;
        swprintf_s(guidStr,
            L"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
            g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
        std::wstring pcb = guidStr;
        if (m_impl->enhancedRequested)
        {
            pcb += L";EnhancedMode=1";
            m_impl->enhancedActive = true;
        }
        HyprvLog(L"[host] OnConnectLocalVm port=%u dw=%u dh=%u flags=0x%x pcb=%s",
            opt.port, opt.desktopWidth, opt.desktopHeight, opt.flags, pcb.c_str());

        // ----- Property setup in the exact order the spike proved works ---------
        // Any deviation (notably putting secure-mode setup before PCB) silently
        // stalls the connection after only DISPID=12 (size change) fires.
        auto* rdp = m_impl->rdp.p;
        rdp->put_Server(CComBSTR(L"localhost"));
        rdp->put_ColorDepth(opt.colorDepth ? opt.colorDepth : 32);
        rdp->put_DesktopWidth(opt.desktopWidth ? opt.desktopWidth : 1024);
        rdp->put_DesktopHeight(opt.desktopHeight ? opt.desktopHeight : 768);

        CComPtr<IMsRdpClientAdvancedSettings6> adv6;
        if (FAILED(rdp->get_AdvancedSettings7(&adv6)) || !adv6)
        {
            HyprvLog(L"[host] get_AdvancedSettings7 failed");
            return;
        }
        // SmartSizing=false makes mstscax render the session at native
        // resolution and rely on the control rect being the right size
        // (which we ensure via popup pinning + AtlAxWin WM_SIZE). Without
        // this explicit value mstscax's default may scale or letterbox
        // depending on host version. VMPlex sets the same — RdpClient.cs:102.
        adv6->put_SmartSizing(VARIANT_FALSE);
        // Route Windows-key combos (Win, Win+R, Alt+Tab, …) to the GUEST.
        // KeyboardHookMode lives on IMsRdpClientSecuredSettings (via
        // get_SecuredSettings), NOT on AdvancedSettings. Default is 2 = "apply
        // to the remote session only in full-screen mode"; our mstscax host is
        // windowed/embedded, so the default lets the HOST swallow Win/Win+R
        // (host Start menu / Run dialog open instead of the guest's). 1 =
        // always apply to the remote session while the control has focus.
        {
            // get_SecuredSettings (the IMsTscAx base) returns the older
            // IMsTscSecuredSettings, which has no KeyboardHookMode. Use
            // get_SecuredSettings2 (IMsRdpClient) for the IMsRdpClientSecuredSettings
            // that does.
            CComPtr<IMsRdpClientSecuredSettings> sec;
            if (SUCCEEDED(rdp->get_SecuredSettings2(&sec)) && sec)
            {
                HRESULT khr = sec->put_KeyboardHookMode(1);
                HyprvLog(L"[host] put_KeyboardHookMode(1) hr=0x%08lx", khr);
            }
            else
            {
                HyprvLog(L"[host] get_SecuredSettings2 failed — Win-key passthrough off");
            }
        }
        CComQIPtr<IMsRdpClientAdvancedSettings7> adv7(adv6);
        if (adv7)
        {
            adv7->put_RDPPort(opt.port ? opt.port : 2179);
            adv7->put_AuthenticationServiceClass(CComBSTR(L"Microsoft Virtual Console Service"));
            adv7->put_GrabFocusOnConnect(VARIANT_FALSE);
        }
        adv6->put_AuthenticationLevel(0);
        if (adv7)
        {
            HRESULT phr = adv7->put_PCB(CComBSTR(pcb.c_str()));
            HyprvLog(L"[host] put_PCB hr=0x%08lx", phr);
        }

        // User-configurable RDP options. The parent sends these in
        // RdpOptions; until this block existed, every field except
        // colorDepth/desktopWidth/desktopHeight was silently dropped
        // on the child side, so toggling "audio: mute" in the UI had
        // no observable effect on the session — the wire payload
        // changed but mstscax was never told.
        //
        // AudioRedirectionMode values match our wire enum exactly:
        //   0 = redirect to client (play locally)
        //   1 = play on the server
        //   2 = no sound (mute)
        // So a direct cast is safe — but clamp anyway so a wire-protocol
        // mismatch or hand-edited settings file can't push mstscax into
        // an undefined value.
        {
            UINT amode = opt.audioMode;
            if (amode > 2) amode = 0;
            HRESULT ah = adv6->put_AudioRedirectionMode(amode);
            HyprvLog(L"[host] put_AudioRedirectionMode(%u) hr=0x%08lx", amode, ah);
        }

        // Redirection flags. Each maps 1:1 to a put_Redirect* on
        // IMsRdpClientAdvancedSettings (inherited via adv6). The audio
        // CAPTURE mode (microphone direction) lives on adv7 with
        // different semantics (a mode enum, where 0=enabled/1=disabled).
        // None of these have any effect outside enhanced session mode —
        // basic Hyper-V sessions are screen+keyboard+mouse only — but
        // setting them is harmless when ignored.
        auto setBool = [&](HRESULT (STDMETHODCALLTYPE
                                IMsRdpClientAdvancedSettings::*setter)(VARIANT_BOOL),
                           uint32_t flag, wchar_t const* name)
        {
            // IMsRdpClientAdvancedSettings6 inherits from
            // IMsRdpClientAdvancedSettings — direct member-function
            // pointer works.
            VARIANT_BOOL v = (opt.flags & flag) ? VARIANT_TRUE : VARIANT_FALSE;
            HRESULT hr = (adv6->*setter)(v);
            HyprvLog(L"[host] %s(%d) hr=0x%08lx", name, v ? 1 : 0, hr);
        };
        setBool(&IMsRdpClientAdvancedSettings::put_RedirectDrives,
                hyprv::ipc::Flag_RedirectDrives,      L"put_RedirectDrives");
        setBool(&IMsRdpClientAdvancedSettings::put_RedirectPrinters,
                hyprv::ipc::Flag_RedirectDevices,     L"put_RedirectPrinters");
        setBool(&IMsRdpClientAdvancedSettings::put_RedirectPorts,
                hyprv::ipc::Flag_RedirectPorts,       L"put_RedirectPorts");
        setBool(&IMsRdpClientAdvancedSettings::put_RedirectSmartCards,
                hyprv::ipc::Flag_RedirectSmartCards,  L"put_RedirectSmartCards");

        // Clipboard lives on IMsRdpClientAdvancedSettings2 — adv6
        // inherits from it. Same shape.
        {
            VARIANT_BOOL v = (opt.flags & hyprv::ipc::Flag_RedirectClipboard)
                ? VARIANT_TRUE : VARIANT_FALSE;
            HRESULT hr = adv6->put_RedirectClipboard(v);
            HyprvLog(L"[host] put_RedirectClipboard(%d) hr=0x%08lx", v ? 1 : 0, hr);
        }

        // Microphone capture — mstscax uses 0=enabled / 1=disabled here
        // (opposite of the VARIANT_BOOL pattern above). Lives on adv7.
        if (adv7)
        {
            UINT capMode =
                (opt.flags & hyprv::ipc::Flag_AudioCaptureRedirect) ? 0u : 1u;
            HRESULT hr = adv7->put_AudioCaptureRedirectionMode(
                capMode == 0 ? VARIANT_TRUE : VARIANT_FALSE);
            HyprvLog(L"[host] put_AudioCaptureRedirectionMode(%d) hr=0x%08lx",
                capMode == 0 ? 1 : 0, hr);
        }

        // Secure mode toggles — MUST come after PCB (spike order).
        CComQIPtr<IMsRdpClientNonScriptable3> ns3(m_impl->unk);
        if (ns3)
        {
            ns3->put_EnableCredSspSupport(VARIANT_TRUE);
            ns3->put_NegotiateSecurityLayer(VARIANT_FALSE);
        }
        // DisableCredentialsDelegation must be VT_BOOL — VT_I2 silently drops the flag.
        CComQIPtr<IMsRdpExtendedSettings> ext(m_impl->unk);
        if (ext)
        {
            CComVariant vTrue;
            vTrue.vt = VT_BOOL;
            vTrue.boolVal = VARIANT_TRUE;
            HRESULT eh = ext->put_Property(CComBSTR(L"DisableCredentialsDelegation"), &vTrue);
            if (FAILED(eh)) HyprvLog(L"[host] put_Property(DisableCredentialsDelegation) hr=0x%08lx", eh);

            // Tell mstscax the parent's display DPI. Without these properties
            // mstscax assumes 100% DPI and renders the credential UI at a fixed
            // pixel size that appears tiny in the upper-left of the popup on
            // high-DPI screens (the "credential UI off-center" complaint).
            // VMPlex sets the same pair — see RdpClient.cs:118-119.
            const uint16_t dpiScale = opt.dpiScalePercent ? opt.dpiScalePercent : 100;
            CComVariant vDesktop;
            vDesktop.vt   = VT_UI4;
            vDesktop.ulVal = static_cast<ULONG>(dpiScale);
            HRESULT dh = ext->put_Property(CComBSTR(L"DesktopScaleFactor"), &vDesktop);
            if (FAILED(dh)) HyprvLog(L"[host] put_Property(DesktopScaleFactor=%u) hr=0x%08lx",
                                     static_cast<unsigned>(dpiScale), dh);

            CComVariant vDevice;
            vDevice.vt   = VT_UI4;
            vDevice.ulVal = 100u;   // VMPlex always passes 100; mstscax wants device scale separate
            HRESULT vh = ext->put_Property(CComBSTR(L"DeviceScaleFactor"), &vDevice);
            if (FAILED(vh)) HyprvLog(L"[host] put_Property(DeviceScaleFactor=100) hr=0x%08lx", vh);

            // EnableFrameBufferRedirection routes the session through Hyper-V's
            // VMBus frame buffer pipeline. Without this, mstscax falls back to
            // a different render path that doesn't fill the control window —
            // the pre-login credential UI ends up at a smaller fixed size in
            // the upper-left of the popup with mstscax-painted background
            // around it. VMPlex sets this unconditionally for VM connections
            // (RdpClient.cs:706+). This is the actual fix for the "credential
            // UI off-center / offset" issue.
            CComVariant vFbr;
            vFbr.vt      = VT_BOOL;
            vFbr.boolVal = VARIANT_TRUE;
            HRESULT fh = ext->put_Property(CComBSTR(L"EnableFrameBufferRedirection"), &vFbr);
            if (FAILED(fh)) HyprvLog(L"[host] put_Property(EnableFrameBufferRedirection) hr=0x%08lx", fh);
        }

        // Snapshot the popup + AtlAxWin sizes right before Connect() — this
        // is the rect mstscax sees as its initial container.
        if (m_impl->ax)
        {
            HWND popup = GetParent(m_impl->ax);
            RECT pr{}, axr{};
            if (popup) GetWindowRect(popup, &pr);
            GetWindowRect(m_impl->ax, &axr);
            long curW = 0, curH = 0;
            rdp->get_DesktopWidth(&curW);
            rdp->get_DesktopHeight(&curH);
            HyprvLog(L"[host] pre-Connect: popup=%dx%d ax=%dx%d desktopW/H=%ldx%ld "
                     L"enhRequested=%d dpiScale=%u",
                pr.right - pr.left, pr.bottom - pr.top,
                axr.right - axr.left, axr.bottom - axr.top,
                curW, curH, m_impl->enhancedRequested ? 1 : 0,
                static_cast<unsigned>(opt.dpiScalePercent));
        }

        HyprvLog(L"[host] calling Connect()");
        HRESULT hr = rdp->Connect();
        HyprvLog(L"[host] Connect() returned 0x%08lx", hr);
        // Post-Connect snapshot — mstscax may have adjusted internal state.
        if (m_impl->ax)
        {
            HWND popup = GetParent(m_impl->ax);
            RECT pr{}, axr{};
            if (popup) GetWindowRect(popup, &pr);
            GetWindowRect(m_impl->ax, &axr);
            long curW = 0, curH = 0;
            rdp->get_DesktopWidth(&curW);
            rdp->get_DesktopHeight(&curH);
            HyprvLog(L"[host] post-Connect: popup=%dx%d ax=%dx%d desktopW/H=%ldx%ld",
                pr.right - pr.left, pr.bottom - pr.top,
                axr.right - axr.left, axr.bottom - axr.top,
                curW, curH);
        }
        if (FAILED(hr))
        {
            hyprv::ipc::Error err{ static_cast<uint32_t>(hyprv::ipc::RdpErrorCode::HostStartupFailed) };
            m_impl->ipc.Send(hyprv::ipc::C2P::Error, &err, sizeof(err));
        }
    }

    void RdpHost::OnDisconnect()
    {
        if (!m_impl->rdp) return;
        m_impl->rdp->Disconnect();
    }

    void RdpHost::OnUpdateSessionDisplaySettings(const hyprv::ipc::UpdateSessionDisplaySettings& cmd)
    {
        if (!m_impl->rdp) return;
        HRESULT hr = m_impl->rdp->UpdateSessionDisplaySettings(
            cmd.width, cmd.height, cmd.physWidth, cmd.physHeight,
            cmd.orientation, cmd.desktopScale, cmd.deviceScale);
        if (FAILED(hr))
        {
            wchar_t buf[80];
            swprintf_s(buf, L"[rdphost] UpdateSessionDisplaySettings(%u,%u) failed: 0x%08lx\n",
                cmd.width, cmd.height, hr);
            OutputDebugStringW(buf);
        }
    }

    void RdpHost::OnSetEnhanced(bool enhanced)
    {
        // Enhanced toggle requires a reconnect under the new PCB. For now just
        // remember the requested state; the parent should follow with Disconnect
        // then a fresh ConnectLocalVm with the updated flag.
        m_impl->enhancedRequested = enhanced;
    }

    void RdpHost::OnSendCtrlAltDel()
    {
        // mstscax doesn't expose a direct ctrl-alt-del API; the VMPlex C# version
        // routes through IMsvm_Keyboard via WMI. That lives on the parent side, so
        // the parent will send keyboard input directly via WMI rather than this IPC.
        OutputDebugStringW(L"[rdphost] OnSendCtrlAltDel — not implemented (parent uses WMI directly)\n");
    }
}
