#pragma once

#include "VmTabPage.g.h"

#include "vm/VirtualMachine.h"   // VmState — needed for m_lastState field type

#include <chrono>
#include <memory>
#include <string>

namespace hyprv::app { class RdpHostClient; }

namespace winrt::hyprv_app::implementation
{
    // UserControl that owns one VM's lifetime — its rdpHost popup, its
    // RdpHostClient + rdphost.exe child process, and the cross-process
    // window positioning. Rail / flyout / window subclass live on the
    // owning MainWindow now.
    struct VmTabPage : VmTabPageT<VmTabPage>
    {
        VmTabPage();
        ~VmTabPage();

        // Bind this page to a specific VM. Must be called immediately after
        // construction. vmGuid is the Msvm_ComputerSystem.Name; comparisons
        // elsewhere are case-insensitive.
        void Initialize(hstring const& vmGuid);
        hstring VmGuid() const { return hstring{ m_vmGuid }; }

        // MainWindow drops the owning Window's HWND into the page before
        // adding it to the TabView — popup positioning needs to translate
        // from client-rect DIPs into screen pixels.
        void SetWindowHwnd(HWND hwnd);
        // Connect this page to its containing TabViewItem so the page can
        // keep its own header text in sync with the VM name without
        // MainWindow having to mediate.
        void SetTabItem(Microsoft::UI::Xaml::Controls::TabViewItem const& tab);

        // Tab-activation lifecycle — MainWindow calls Show/Hide on selection
        // change so only the active page's popup is visible. RefreshPopupBounds
        // is wired from MainWindow's window-subclass on WM_WINDOWPOSCHANGED.
        void ShowPopup();
        void HidePopup();
        void RefreshPopupBounds();

        // Temporary popup hide that DOESN'T touch m_wantsVisible. Used by
        // MainWindow when a modal ContentDialog opens — the mstscax popup
        // is a top-level HWND owned by the rdphost child process and paints
        // above the XAML composition surface, so dialogs render behind it
        // and become unclickable. SetPopupSuppressed(true) hides the popup
        // via m_client->Hide() without disturbing the active-tab latch;
        // SetPopupSuppressed(false) re-shows it if the tab is still active.
        // Reference-counted by the caller (MainWindow holds the count).
        void SetPopupSuppressed(bool suppressed);

        // MainWindow pings each open page on VMManager OnChanged so the
        // tab header tracks rename / state transitions. Cheap — just
        // re-reads the VM snapshot and reassigns header text.
        void OnVmManagerChanged();

        // React to a per-VM Settings flip (e.g. enhanced-session toggle).
        // If the VM is currently running and an rdphost child exists, tears
        // it down + respawns with the new options picked up from Settings
        // and the live VirtualMachine snapshot. If no client is running yet,
        // it's a no-op — the next StartConnection will read the fresh pref.
        // Idempotent; safe to call repeatedly.
        void ApplyEnhancedSessionChange();

        // Apply a changed connection option (enhanced-session pref OR a per-VM
        // RDP option) to a LIVE session by respawning the rdphost child so
        // StartConnection re-reads RdpOptionsFor(guid). No-op when no client is
        // running (the next connect reads the fresh options). The VM Settings
        // dialog calls this after a Save that changed the RDP options.
        void ReapplyConnectionSettings();

        // Deterministic teardown of the rdphost child + IPC pipe. MainWindow
        // calls this from OnTabCloseRequested before removing the TabViewItem.
        // The dtor remains a safety net but can't be relied on alone because
        // TabView's internal item cache often retains the page projection past
        // RemoveAt, delaying ~VmTabPage indefinitely — leaving rdphost.exe
        // orphaned until process exit. Idempotent.
        void ShutdownClient();

        // XAML event — Start button inside the placeholder overlay.
        void OnPlaceholderStartClick(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&);

        // XAML event — Retry button inside the placeholder overlay. Shown only
        // after a connect attempt to a Running VM repeatedly fails (see
        // ShowConnectError). Clears the error latch + failure counter and forces
        // a fresh connection attempt.
        void OnPlaceholderRetryClick(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void OnLoaded();
        void OnRdpHostSizeChanged(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        bool ComputeBorderScreenRect(int& sx, int& sy, int& w, int& h);
        void UpdateRdphostBounds();
        // Enhanced fit-to-window retry. mstscax often silently rejects the
        // first UpdateSessionDisplaySettings right after OnLoginComplete (the
        // enhanced transition isn't finished), and the IPC cache then pins the
        // rejected dims so identical re-sends short-circuit — leaving the
        // session small until the user wiggles the window. StartEnhancedFit
        // arms a short repeating timer that drops the cache + re-pushes the
        // full-Border resize each tick; OnDesktopResized stops it once the
        // session actually reaches the Border (StopEnhancedFit).
        void StartEnhancedFit();
        void StopEnhancedFit();
        void StartConnection();
        void SetStatus(hstring const& text);
        void UpdateVmTabHeader();

        // Drive the placeholder overlay + rdphost-client lifecycle from the
        // current VM state. Called from OnLoaded and from OnVmManagerChanged.
        // Edge-triggers off m_lastState to avoid respawn churn on every poll.
        void UpdatePlaceholderAndClient();
        void TearDownClient();

        // Auto-reconnect: when a live session drops while the VM is still
        // Running (guest reboot, logoff, enhanced-not-ready), respawn the
        // rdphost after a short debounce so the user doesn't have to close +
        // reopen the tab. ScheduleReconnect(delay) (re)arms the one-shot
        // m_reconnectTimer; DoReconnect (the Tick) tears down + respawns if the
        // VM is still Running. StartConnection re-picks enhanced-vs-basic from
        // the live snapshot each time, so a rebooting guest shows basic during
        // boot and (via the OnVmManagerChanged poll-upgrade) switches to
        // enhanced once it's available again.
        void ScheduleReconnect(std::chrono::milliseconds delay);
        void DoReconnect();

        // Surface a fatal connect failure in the placeholder overlay: stops the
        // silent reconnect churn, latches m_hadConnectError, stashes the
        // description, tears the (dead) client down, and renders the error view
        // with a Retry button. Called from OnDisconnected (after N fatal
        // attempts on a Running VM) and from the rdphost Error path
        // (HostStartupFailed / ProtocolError). RenderErrorPlaceholder does the
        // actual XAML population and is reused by UpdatePlaceholderAndClient so
        // poll ticks keep the error visible instead of respawning.
        void ShowConnectError(std::wstring const& detail);
        void RenderErrorPlaceholder(std::wstring const& vmName);
        // Single source of truth for the placeholder-vs-watermark invariant:
        // backgroundVmName (the always-present dim VM-name watermark, drawn
        // behind the rdpHost popup + captured by the DWM taskbar thumbnail) is
        // visible EXACTLY when placeholderRoot is NOT. The placeholder draws its
        // own bright centered name + status + buttons, so showing the watermark
        // underneath it would double-draw the name. Every site that flips
        // placeholderRoot's visibility must go through here so the two never
        // both show the name (and the name is never absent). See VmTabPage.xaml.
        void SetPlaceholderVisible(bool visible);

        // ---- Per-tab state -----------------------------------------------
        std::wstring                                  m_vmGuid;
        std::unique_ptr<hyprv::app::RdpHostClient>    m_client;
        HWND                                          m_windowHwnd = nullptr;
        bool                                          m_loaded     = false;
        // Live RDP session state — driven by OnConnected / OnDisconnected
        // callbacks in StartConnection. ShowPopup gates on this so we never
        // surface an empty popup over a non-running / disconnected session
        // (which is the underlying cause of the white-screen-on-tab-back bug).
        bool                                          m_connected  = false;
        // Tracks MainWindow's last Show/HidePopup intent. True when this tab
        // is the active one. Gates async callbacks (OnConnected etc) from
        // surfacing the popup when the user has navigated away mid-connect —
        // otherwise a slow-to-connect VM ends up painting over whatever tab
        // is currently active when its connection completes.
        bool                                          m_wantsVisible = false;
        // Latched true when ShowPopup wants to make the popup visible but
        // can't yet — typically the cold-start restore + tab activate path
        // where MainWindow mounts the page and calls ShowPopup synchronously
        // BEFORE WinUI's layout pass measures the rdpHost Border. We refuse
        // to call m_client->Show() at the stale fallback rect from
        // StartConnection (would flash the popup in the top-left corner)
        // and instead defer to OnLoaded (first-mount path) or
        // OnRdpHostSizeChanged (subsequent-mount path) — both fire after
        // the Border has a real ActualWidth/Height. Cleared when we
        // successfully Show, or when HidePopup is called.
        bool                                          m_deferredShow = false;
        // True while a modal dialog (VM settings / new VM / remote host / a
        // confirm) is open — its owner pushed popup suppression (MainWindow's
        // PushPopupSuppression → SetPopupSuppressed(true)). The rdphost popup is a
        // separate top-level window, NOT part of the XAML modal overlay, so it
        // would paint over the dialog and block interaction. This flag gates
        // EVERY show path (ShowPopup / OnConnected / OnLoaded / OnRdpHostSizeChanged)
        // so a VM that finishes booting WHILE the dialog is open can't surface its
        // popup over it; PopPopupSuppression re-shows it once the dialog closes.
        bool                                          m_popupSuppressed = false;
        // Set on placeholder Start click, cleared on first WMI tick that
        // reports the VM has left a stable off/saved/paused state. While
        // set, UpdatePlaceholderAndClient suppresses the Start button + hint
        // text so they don't flicker back into view in the brief window
        // between the click and Hyper-V actually registering the state-
        // change job. Safety-timed in case the request never takes effect.
        bool                                          m_startRequested = false;
        std::chrono::steady_clock::time_point         m_startRequestedAt{};
        // Optimistic pre-connect: set when the user clicks the placeholder Start
        // button so UpdatePlaceholderAndClient spawns the rdphost IMMEDIATELY
        // (rather than waiting for the next poll to report the VM Running), and
        // keeps it alive across the brief Off->Running transition. mstscax then
        // connects as the VM powers on, catching the firmware boot screen (e.g.
        // "Press any key to boot from CD"). A failed early connect (VM not up
        // yet) retries fast via OnDisconnected while this is set; cleared once
        // the VM is Running or after a generous timeout (a start that never
        // took). See OnPlaceholderStartClick / UpdatePlaceholderAndClient.
        bool                                          m_pendingConnect = false;
        std::chrono::steady_clock::time_point         m_pendingConnectAt{};
        // Connect-error surfacing. m_hadConnectError latches true once we give
        // up on a connect attempt (a fatal RDP disconnect we've retried past the
        // threshold, or an rdphost startup/protocol Error) and switch the
        // placeholder into its error view with a Retry button; while set,
        // UpdatePlaceholderAndClient keeps the error visible instead of
        // respawning the child. Cleared on a successful OnConnected, on a user
        // Start/Retry, or when the VM leaves a connectable state. m_lastErrorText
        // is the message shown (mstscax's description, or a mapped Error string).
        // m_connectFailures counts CONSECUTIVE fatal disconnects on a Running VM
        // that never reached Connected — only the running-but-never-connected
        // case counts (the optimistic pre-connect window and benign reboots of
        // an established session are expected and don't escalate to an error).
        bool                                          m_hadConnectError = false;
        std::wstring                                  m_lastErrorText;
        int                                           m_connectFailures = 0;
        // Set true the first time this tab reaches a live session (OnConnected)
        // and never reset for the tab's life. Once a VM has connected at least
        // once, a later run of fatal connect failures on a still-Running VM is
        // almost always a transient transport stall — most importantly the
        // user breaking into a KERNEL DEBUGGER (guest CPU halted, VM still
        // Running, RDP refuses/drops) — NOT a genuinely unreachable console. So
        // we suppress the give-up-and-surface-error escalation in that case and
        // keep retrying on the debounce, so the session auto-revives the moment
        // the debugger resumes. A tab that has NEVER connected still escalates
        // after kMaxConnectFailures (a real bad-console / unsupported VM).
        bool                                          m_everConnected = false;
        // Set true the first time an ENHANCED session reaches login-complete
        // (OnEnhancedReady) and never reset for the tab's life. Once a VM has
        // had a working enhanced session, a later enhanced drop is a transient
        // (guest reboot, or the user breaking into a KERNEL DEBUGGER — frozen
        // guest, VM still Running) — NOT "enhanced isn't usable here." So we
        // SKIP arming the enhanced→basic cooldown back-off in that case and
        // keep reconnecting straight back to enhanced, instead of falling to a
        // basic session for 20s. The cooldown still applies to a tab that has
        // never achieved enhanced (OS install / first boot / no-LIS guest),
        // where basic is the right fallback to catch the boot screen. See the
        // cooldown logic in OnDisconnected + StartConnection (gotcha #41).
        bool                                          m_everEnhanced = false;
        // Whether the connect attempt is using enhanced session mode. Captured
        // from VirtualMachine.enhancedSessionAvailable at StartConnection time.
        // Drives whether OnEnhancedReady tries to expand the session via
        // UpdateSessionDisplaySettings (only valid in enhanced mode post-login).
        bool                                          m_wantedEnhanced = false;
        // Flips true on OnEnhancedReady (= mstscax DISPID_OnLoginComplete). Gates
        // post-login resize-to-fit-Border logic; reset on OnDisconnected /
        // StartConnection so reconnects re-pin at the next login.
        bool                                          m_enhancedReady  = false;
        // Actual session render size reported by mstscax. Updated on
        // OnDesktopResized callback (which fires from DISPID_OnRemoteDesktopSizeChange
        // = 12 every time mstscax changes the session resolution — initial
        // connect, post-login enhanced resize, server-driven resize, etc).
        // The rdphost popup is always centered & sized to (m_sessionW, m_sessionH)
        // inside the rdpHost Border, so mstscax's render area exactly matches
        // the popup window — no mstscax-internal letterbox visible. Outside
        // the popup the hyprv chrome shows (dark Mica).
        int                                           m_sessionW = 1024;
        int                                           m_sessionH = 768;
        // User's display-scale override (percent: 100/125/.../200; 0 = Auto =
        // follow host DPI). Captured from Settings in StartConnection so both
        // the connect path and the enhanced fit-to-window resize in
        // UpdateRdphostBounds honor the same value. See gotcha #15.
        uint16_t                                      m_dpiOverridePercent = 0;
        // Cached VM state at the previous UpdatePlaceholderAndClient pass.
        // Used to detect Running↔not-Running edges so the client only spawns
        // / tears down once per transition instead of every WMI poll tick.
        hyprv::app::vm::VmState                       m_lastState =
            hyprv::app::vm::VmState::Unknown;
        Microsoft::UI::Dispatching::DispatcherQueue   m_uiQueue{ nullptr };
        // One-shot debounce timer for auto-reconnect (re-armed by
        // ScheduleReconnect; fires DoReconnect on the UI thread).
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_reconnectTimer{ nullptr };
        // Repeating retry that re-pushes the enhanced fit-to-window resize
        // until mstscax confirms (via OnDesktopResized) the session reached the
        // Border size. Armed in OnEnhancedReady, stopped in OnDesktopResized /
        // teardown. See StartEnhancedFit. m_enhancedFitTicks caps the retries
        // so a guest that never accepts the size doesn't re-push forever.
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_enhancedFitTimer{ nullptr };
        int                                              m_enhancedFitTicks = 0;
        // Enhanced-session back-off: when an enhanced attempt disconnects before
        // ever reaching login-complete, enhanced isn't really usable right now,
        // so we suppress it (use basic) until this time. Prevents the
        // enhanced→fail→reconnect→enhanced thrash that the reverted first cut
        // suffered. Past/default = no cooldown.
        std::chrono::steady_clock::time_point         m_enhancedCooldownUntil{};
        // Back-pointer to the TabViewItem so the page can update its own
        // header without round-tripping through MainWindow.
        Microsoft::UI::Xaml::Controls::TabViewItem    m_tabItem{ nullptr };
    };
}

namespace winrt::hyprv_app::factory_implementation
{
    struct VmTabPage : VmTabPageT<VmTabPage, implementation::VmTabPage>
    {
    };
}
