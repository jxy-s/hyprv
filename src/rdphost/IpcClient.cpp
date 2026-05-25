#include "IpcClient.h"

#include <utility>

extern void HyprvLog(const wchar_t* fmt, ...);

namespace hyprv::rdphost
{
    IpcClient::IpcClient(std::wstring name) : m_name(std::move(name)) {}

    IpcClient::~IpcClient()
    {
        Stop();
        if (m_pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
    }

    bool IpcClient::Connect(DWORD timeoutMs)
    {
        if (!WaitNamedPipeW(m_name.c_str(), timeoutMs)) return false;

        // FILE_FLAG_OVERLAPPED is required to avoid kernel-level serialization
        // between the rx thread's pending blocking read and any concurrent write.
        // See OverlappedRead/OverlappedWrite below.
        m_pipe = CreateFileW(
            m_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        return m_pipe != INVALID_HANDLE_VALUE;
    }

    static bool OverlappedRead(HANDLE pipe, void* buf, DWORD bytes, DWORD* outRead)
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

    static bool OverlappedWrite(HANDLE pipe, void const* buf, DWORD bytes, DWORD* outWritten)
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

    bool IpcClient::ReadExact(void* buf, uint32_t bytes)
    {
        auto* p = static_cast<uint8_t*>(buf);
        uint32_t remaining = bytes;
        while (remaining > 0)
        {
            DWORD got = 0;
            if (!OverlappedRead(m_pipe, p, remaining, &got) || got == 0)
                return false;
            p += got;
            remaining -= got;
        }
        return true;
    }

    bool IpcClient::Handshake()
    {
        // Parent sends Hello first; we read it, send HelloAck.
        hyprv::ipc::Header h{};
        if (!ReadExact(&h, sizeof(h))) return false;
        if (h.type != static_cast<uint8_t>(hyprv::ipc::P2C::Hello)) return false;
        if (h.payloadSize != sizeof(hyprv::ipc::Hello)) return false;

        hyprv::ipc::Hello hello{};
        if (!ReadExact(&hello, sizeof(hello))) return false;

        // Pick the highest version both sides understand. v1 is the only one for now.
        uint8_t chosen = hyprv::ipc::kProtocolVersion;
        bool ok = (hello.minVersion <= chosen && chosen <= hello.maxVersion);

        hyprv::ipc::HelloAck ack{ chosen, ok ? uint8_t{0} : uint8_t{1}, 0 };
        if (!Send(hyprv::ipc::C2P::HelloAck, &ack, sizeof(ack))) return false;
        return ok;
    }

    bool IpcClient::SendRaw(const void* framedBytes, uint32_t totalSize)
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_pipe == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        if (!OverlappedWrite(m_pipe, framedBytes, totalSize, &written) || written != totalSize)
        {
            HyprvLog(L"[ipc] SendRaw WriteFile failed: err=%lu written=%lu/%lu",
                GetLastError(), written, totalSize);
            return false;
        }
        return true;
    }

    bool IpcClient::Send(hyprv::ipc::C2P type, const void* payload, uint32_t payloadSize)
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_pipe == INVALID_HANDLE_VALUE)
        {
            HyprvLog(L"[ipc] Send: pipe is INVALID_HANDLE_VALUE");
            return false;
        }

        hyprv::ipc::Header h{};
        h.type = static_cast<uint8_t>(type);
        h.payloadSize = payloadSize;

        DWORD written = 0;
        if (!OverlappedWrite(m_pipe, &h, sizeof(h), &written) || written != sizeof(h))
        {
            HyprvLog(L"[ipc] Send header WriteFile failed: err=%lu written=%lu", GetLastError(), written);
            return false;
        }
        if (payloadSize > 0)
        {
            if (!OverlappedWrite(m_pipe, payload, payloadSize, &written) || written != payloadSize)
            {
                HyprvLog(L"[ipc] Send payload WriteFile failed: err=%lu written=%lu/%lu", GetLastError(), written, payloadSize);
                return false;
            }
        }
        return true;
    }

    void IpcClient::StartReceiveLoop(OnMessageFn onMsg, OnDisconnectFn onDisc)
    {
        m_running = true;
        m_thread = std::thread([this, onMsg = std::move(onMsg), onDisc = std::move(onDisc)]
        {
            HyprvLog(L"[ipc] rx thread started pipe=%p", m_pipe);
            while (m_running.load(std::memory_order_relaxed))
            {
                hyprv::ipc::Header h{};
                if (!ReadExact(&h, sizeof(h)))
                {
                    HyprvLog(L"[ipc] rx header read failed err=%lu pipe=%p", GetLastError(), m_pipe);
                    break;
                }
                HyprvLog(L"[ipc] rx got type=%u size=%u", h.type, h.payloadSize);

                auto msg = std::make_unique<IpcMessage>();
                msg->header = h;
                if (h.payloadSize > 0)
                {
                    msg->payload.resize(h.payloadSize);
                    if (!ReadExact(msg->payload.data(), h.payloadSize))
                    {
                        HyprvLog(L"[ipc] rx payload read failed err=%lu", GetLastError());
                        break;
                    }
                }
                onMsg(msg.release());
            }
            HyprvLog(L"[ipc] rx thread exiting, calling onDisc");
            if (onDisc) onDisc();
        });
    }

    void IpcClient::Stop()
    {
        HyprvLog(L"[ipc] Stop tid=%lu pipe=%p", GetCurrentThreadId(), m_pipe);
        m_running = false;
        if (m_pipe != INVALID_HANDLE_VALUE)
        {
            // CancelIoEx aborts the overlapped read pending on the rx thread,
            // letting it join cleanly. Note: CancelSynchronousIo doesn't apply
            // here — pipe is OVERLAPPED, so ReadFile is async at the kernel level.
            CancelIoEx(m_pipe, nullptr);
        }
        if (m_thread.joinable()) m_thread.join();
        if (m_pipe != INVALID_HANDLE_VALUE)
        {
            HyprvLog(L"[ipc] Stop CloseHandle pipe=%p", m_pipe);
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
    }
}
