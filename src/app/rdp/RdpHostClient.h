// RdpHostClient — parent-side wrapper around hyprv-rdphost.exe.
//
// One instance manages exactly one child process and one named pipe. It handles:
//   - creating the pipe server and spawning the child with --pipe=<guid>
//   - Hello / HelloAck handshake
//   - reading HwndReady, then SetParent + AttachThreadInput so the embedded
//     mstscax becomes a child of the supplied host HWND and shares input queues
//   - forwarding caller method calls (Disconnect/Resize/...) as IPC messages
//   - dispatching inbound events (Connecting/Connected/Disconnected/...) via
//     std::function callbacks. Callbacks fire on the IPC receive thread; if you
//     touch XAML from them, marshal yourself via DispatcherQueue.TryEnqueue.
//   - Stop(): graceful Shutdown send → 2s wait → TerminateProcess.
//
// pimpl pattern hides Windows.h/pipe state from callers.

#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "RdpIpc.h"

namespace hyprv::app
{
    struct ConnectedInfo
    {
        bool     enhanced;
        uint32_t desktopWidth;
        uint32_t desktopHeight;
    };

    struct DisconnectedInfo
    {
        int32_t      discReason;
        int32_t      extendedReason;
        // true when the disconnect is a real error (socket/timeout/auth/
        // protocol) rather than a benign/expected drop (clean logoff, server-
        // initiated, guest reboot). Classified on the child from the mstscax
        // reason code; the parent uses it to decide whether to surface a
        // connect failure vs. silently reconnect.
        bool         fatal = false;
        // Localized human-readable description from mstscax GetErrorDescription
        // (only populated for fatal disconnects; empty otherwise).
        std::wstring description;
    };

    class RdpHostClient
    {
    public:
        // ---- callbacks (set before calling Connect/ConnectLocalVm) ----
        std::function<void()>                          OnConnecting;
        std::function<void(ConnectedInfo const&)>      OnConnected;
        std::function<void(bool /*ready*/)>            OnEnhancedReady;
        std::function<void(DisconnectedInfo const&)>   OnDisconnected;
        std::function<void(uint32_t, uint32_t)>        OnDesktopResized;
        std::function<void(uint32_t /*code*/)>         OnError;
        std::function<void(DWORD /*exitCode*/)>        OnChildExited;

        explicit RdpHostClient(std::wstring rdphostExePath);
        ~RdpHostClient();

        RdpHostClient(const RdpHostClient&) = delete;
        RdpHostClient& operator=(const RdpHostClient&) = delete;

        // Spawn child, do handshake, wait for HwndReady, attach the rdphost window
        // as an OWNED top-level popup of ownerHwnd at the given screen-coord rect,
        // then send the ConnectLocalVm command. Returns true if reaching the embed
        // step succeeded (post-Connect outcome arrives via the OnConnected/OnDisconnected
        // callbacks). Call on the thread that owns ownerHwnd.
        // sx/sy/w/h: screen coordinates of where the rdphost popup should render
        // (typically the rect of the XAML Border converted via ClientToScreen).
        bool ConnectLocalVm(HWND ownerHwnd, int sx, int sy, int w, int h,
                            GUID const& vmGuid,
                            hyprv::ipc::RdpOptions const& opts);

        // Same shape but for generic RDP (no Hyper-V PCB / VMBus auth). username
        // pre-fills mstscax's credential prompt; no password is ever sent (the
        // child prompts via PromptForCredsOnClient). domain may be empty.
        bool Connect(HWND ownerHwnd, int sx, int sy, int w, int h,
                     hyprv::ipc::RdpOptions const& opts,
                     std::wstring const& server, std::wstring const& domain,
                     std::wstring const& username);

        // Reposition the owned popup. Caller should drive this from owner-window
        // WM_MOVE/WM_SIZE and any XAML SizeChanged that moves the Border.
        void Reposition(int sx, int sy, int w, int h);

        // Re-own the embedded popup to a NEW top-level owner HWND. Used by tab
        // tear-out: a VM/Remote tab moving to another window on the same UI
        // thread. Just re-runs the GWLP_HWNDPARENT owner set that EmbedInto did
        // at connect time, and invalidates the Reposition cache so the next
        // UpdateRdphostBounds actually moves the popup to the new owner's coords.
        // No-op before HwndReady. Same UI thread, so no input re-attach needed.
        void Reown(HWND newOwner);

        // Show / hide the owned popup. Used by tab-switch logic so the rdphost
        // only paints when its VmTab is active (otherwise it would float over
        // other tabs at the same screen rect).
        void Show();
        void Hide();

        // Tear down the RDP session but keep the host process alive (for reconnect).
        void Disconnect();

        // Tell the child the host area resized. Caller decides whether to also send
        // UpdateSessionDisplaySettings (typically after the user finishes a drag, in
        // enhanced-session mode).
        void Resize(uint32_t width, uint32_t height);
        void UpdateSessionDisplaySettings(uint32_t width, uint32_t height,
                                          uint32_t dpiX = 96, uint32_t dpiY = 96);

        // Drop the UpdateSessionDisplaySettings short-circuit cache so the next
        // call goes through even if the IPC payload matches what we last sent.
        // Used by VmTabPage after OnEnhancedReady: mstscax sometimes rejects
        // the immediate post-login resize (it's still mid-transition), and the
        // cache otherwise records the rejected dimensions so subsequent
        // identical calls no-op until the user wiggles the window.
        void InvalidateSessionDisplayCache();

        // Toggle enhanced flag; the parent should follow with Disconnect + a fresh
        // ConnectLocalVm to apply the new flag in the PCB.
        void SetEnhanced(bool enhanced);

        // AttachThreadInput + SetFocus so keyboard input flows from the parent UI
        // thread into the embedded mstscax control.
        // CRITICAL: must NOT be called from the same call stack as Window.Activate()
        // / SetActiveWindow. Doing so creates a 3-way deadlock: parent's SetActiveWindow
        // synchronously sends activation messages cross-process via the shared input
        // queue, child's UI thread is busy processing mstscax's SetDesktopSize SendMessage
        // whose handler blocks in a pipe WriteFile, pipe stays half-drained because the
        // kernel can't complete the chain while activation is pending. Call this after
        // the OnConnected event fires, NOT inside ConnectLocalVm.
        void AttachInput();
        void DetachInput();

        // Bring keyboard focus to the mstscax popup. Cross-process SetFocus
        // works via the AttachThreadInput plumbing already done by
        // AttachInput; this call also auto-attaches if not already done.
        // Called by VmTabPage::ShowPopup so switching back to a VM tab
        // delivers keystrokes / clicks to the VM without an extra click.
        void Focus();

        // Idempotent. Sends Shutdown, waits 2s, TerminateProcess if needed, cleans up.
        void Stop();

        // Optional human-readable tag (typically the VM's friendly name) that
        // appears in the unified hyprv.log next to the child's process id —
        // makes it possible to tell which rdphost child is for which VM tab
        // without correlating pids to spawn-time messages. Set this right
        // after construction; safe to update later (next log line picks it up).
        void SetLogLabel(std::wstring label);

        // The re-parented child window (or nullptr before HwndReady). Useful for the
        // host's WndProc to forward focus / sizing.
        HWND childHwnd() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
