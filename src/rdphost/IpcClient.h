// IpcClient — pipe client used by hyprv-rdphost to talk to the parent (hyprv.exe).
// Blocking IO: a dedicated background thread does the reads and posts each
// received message to the UI thread via a caller-supplied callback. Sends are
// synchronous and safe to call from any thread (internal mutex).

#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RdpIpc.h"

namespace hyprv::rdphost
{
    // One incoming message; ownership transfers from the receive thread to whoever
    // consumes the OnMessage callback (UI thread deletes after handling).
    struct IpcMessage
    {
        hyprv::ipc::Header   header;
        std::vector<uint8_t> payload;
    };

    class IpcClient
    {
    public:
        using OnMessageFn    = std::function<void(IpcMessage*)>;
        using OnDisconnectFn = std::function<void()>;

        explicit IpcClient(std::wstring pipeName);
        ~IpcClient();

        IpcClient(const IpcClient&) = delete;
        IpcClient& operator=(const IpcClient&) = delete;

        // Wait for the parent's pipe instance to be available, then open it.
        bool Connect(DWORD timeoutMs);

        // Receive Hello (P2C), respond with HelloAck (C2P). Called once after Connect.
        bool Handshake();

        // Send a typed message to the parent. Direction is implicit (we are the child,
        // so type values come from C2P). Payload may be nullptr if payloadSize == 0.
        bool Send(hyprv::ipc::C2P type, const void* payload, uint32_t payloadSize);

        // Send a pre-framed buffer (Header + payload, fully assembled by caller) as
        // one WriteFile. Used by RdpHost's outbound sender thread to avoid building
        // the Header twice. Safe from any thread, mutex-protected like Send().
        bool SendRaw(const void* framedBytes, uint32_t totalSize);

        // Spawn the receive thread. onMsg is invoked once per inbound message
        // (heap-allocated IpcMessage*; receiver owns it). onDisconnect fires once
        // when the pipe breaks or Stop() is called.
        void StartReceiveLoop(OnMessageFn onMsg, OnDisconnectFn onDisc);

        // Idempotent. Closes the pipe (which unblocks any pending ReadFile),
        // then joins the receive thread if it was started.
        void Stop();

    private:
        bool ReadExact(void* buf, uint32_t bytes);

        std::wstring        m_name;
        HANDLE              m_pipe = INVALID_HANDLE_VALUE;
        std::mutex          m_sendMutex;
        std::thread         m_thread;
        std::atomic<bool>   m_running{ false };
    };
}
