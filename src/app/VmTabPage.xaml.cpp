#include "pch.h"
#include "VmTabPage.xaml.h"
#if __has_include("VmTabPage.g.cpp")
#include "VmTabPage.g.cpp"
#endif

#include "rdp/RdpHostClient.h"
#include "settings/Settings.h"
#include "vm/VMManager.h"

#include <microsoft.ui.xaml.window.h>   // IWindowNative
#include <objbase.h>
#include <combaseapi.h>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.h>                  // WindowId

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// Shared logger lives in MainWindow.xaml.cpp now; declare extern so any of
// our internal logging here resolves at link time.
extern void HyprvAppLog(const wchar_t* fmt, ...);
static void Log(const wchar_t* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    wchar_t buf[1024];
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    HyprvAppLog(L"%s", buf);
}

namespace
{
    // How many CONSECUTIVE fatal disconnects on a Running VM that never reached
    // a live session we tolerate before giving up and surfacing the error (with
    // a Retry button) instead of reconnecting again. ~4 quick attempts (basic
    // reconnect is 700ms) gives a brand-new Running VM a couple of ticks for its
    // console to become accept-ready before we declare a real failure.
    constexpr int kMaxConnectFailures = 4;

    std::wstring FindRdphost()
    {
        // SINGLE enforced location: right next to hyprv.exe. The solution build
        // (shared bin/<Plat>/<Cfg>/ OutDir) and the MSIX/zip package both put
        // hyprv-rdphost.exe in the same directory as hyprv.exe. If it isn't there,
        // the spawn fails and the caller surfaces "hyprv-rdphost.exe not found near
        // hyprv.exe" — we do NOT hunt other locations.
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir = self;
        if (auto p = dir.find_last_of(L"\\/"); p != std::wstring::npos) dir.resize(p);
        return dir + L"\\hyprv-rdphost.exe";
    }

    GUID ParseGuid(const wchar_t* s)
    {
        GUID g{};
        std::wstring braced = L"{";
        braced += s;
        braced += L"}";
        IIDFromString(braced.data(), &g);
        return g;
    }

    // Resolve a theme brush (e.g. "SystemFillColorCriticalBrush",
    // "TextFillColorSecondaryBrush") from the app's merged resource
    // dictionaries. Mirrors AppSettingsPage's TryLookup pattern. Returns
    // nullptr if absent so callers can no-op the Foreground assignment.
    Media::Brush BrushFromTheme(winrt::hstring const& key)
    {
        try
        {
            if (auto v = Application::Current().Resources().TryLookup(winrt::box_value(key)))
                return v.try_as<Media::Brush>();
        }
        catch (...) {}
        return nullptr;
    }
}

namespace winrt::hyprv_app::implementation
{
    VmTabPage::VmTabPage()
    {
        InitializeComponent();
        Log(L"[tab] VmTabPage ctor");
        m_uiQueue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        // UserControl fires Loaded once it's attached to the visual tree —
        // safe at that point to touch named XAML elements + start the
        // rdphost connection.
        Loaded([this](IInspectable const&, RoutedEventArgs const&)
        {
            Log(L"[tab] Loaded fired vm=%s", m_vmGuid.c_str());
            OnLoaded();
        });
    }

    VmTabPage::~VmTabPage()
    {
        if (m_client) m_client->Stop();
    }

    void VmTabPage::Initialize(hstring const& vmGuid)
    {
        m_vmGuid = std::wstring{ vmGuid };
        HyprvAppLog(L"[tab] VmTabPage initialized for vm=%s", m_vmGuid.c_str());
    }

    void VmTabPage::SetWindowHwnd(HWND hwnd)
    {
        const bool changed = (m_windowHwnd != hwnd);
        m_windowHwnd = hwnd;
        // Tab tear-out: when this page moves to another window, re-own the
        // out-of-process rdphost popup to the new window's HWND so it floats
        // over the right window (the next ShowPopup repositions it). No-op at
        // first wire-up (no client yet) — StartConnection's EmbedInto sets it.
        if (changed && m_client)
            m_client->Reown(hwnd);
    }

    void VmTabPage::SetTabItem(Microsoft::UI::Xaml::Controls::TabViewItem const& tab)
    {
        m_tabItem = tab;
        UpdateVmTabHeader();
    }

    void VmTabPage::ShowPopup()
    {
        // MainWindow's intent: "make this tab's popup visible". Latch it so
        // async callbacks (OnConnected etc) only surface the popup when this
        // tab is still active — otherwise a slow connect would paint over a
        // different tab the user navigated to in the meantime.
        m_wantsVisible = true;
        // A modal dialog is up (its owner pushed popup suppression). NEVER surface
        // the rdphost popup over it — it's a separate top-level window, not part of
        // the XAML modal overlay, so it would paint on top and block the dialog.
        // SetPopupSuppressed(false) re-runs ShowPopup once the dialog closes.
        if (m_popupSuppressed) return;
        if (!m_client || !m_connected) return;

        // Critical: don't call m_client->Show() before we can compute the
        // correct popup rect. MainWindow invokes ShowPopup synchronously
        // from OnTabSelectionChanged right after mounting the page into
        // tabContentHost, and WinUI's layout pass hasn't run yet at this
        // point — Border.ActualWidth/Height is still 0. If the popup was
        // previously placed at the StartConnection fallback rect (because
        // the rdphost child was spawned by VMManager's first poll while
        // this tab was unmounted during cold-start restore), Show() would
        // make the popup visible at THAT rect (top-left of the window),
        // and the user sees a noticeable flash before OnLoaded eventually
        // moves it. So: try the pin first; only Show on success.
        int sx, sy, bw, bh;
        if (ComputeBorderScreenRect(sx, sy, bw, bh))
        {
            UpdateRdphostBounds();
            m_client->Show();
            // Hand keyboard focus straight to the mstscax popup so the
            // user can type / click inside the VM without having to click
            // the VM area first. Cross-process focus relies on the
            // AttachThreadInput plumbing inside RdpHostClient::Focus —
            // idempotent if already attached.
            m_client->Focus();
            m_deferredShow = false;
        }
        else
        {
            // Layout-pass-pending case. Leave the popup hidden, mark
            // deferred, and rely on OnLoaded (first mount) or
            // OnRdpHostSizeChanged (subsequent re-mount) to surface it
            // once the Border has a measured size.
            HyprvAppLog(L"[tab] ShowPopup deferred — border not measured vm=%s",
                m_vmGuid.c_str());
            m_deferredShow = true;
        }
    }
    void VmTabPage::HidePopup()
    {
        // User navigated away. Drop both intent flags so nothing else
        // (Loaded, SizeChanged) tries to surface the popup behind their
        // back.
        m_wantsVisible = false;
        m_deferredShow = false;
        if (m_client) m_client->Hide();
    }
    void VmTabPage::RefreshPopupBounds() { UpdateRdphostBounds(); }

    void VmTabPage::SetPopupSuppressed(bool suppressed)
    {
        // Distinct from HidePopup: leaves m_wantsVisible alone so the popup
        // returns to its prior visibility when the modal scope ends. The flag
        // gates the async show paths too (see m_popupSuppressed), so a VM that
        // finishes booting + connects WHILE the dialog is open can't surface its
        // popup over the dialog.
        m_popupSuppressed = suppressed;
        if (suppressed)
        {
            if (m_client) m_client->Hide();
        }
        else if (m_wantsVisible)
        {
            // Re-run the full show path (client/connected/border-measured + focus).
            // Handles a connect that landed while the dialog was up.
            ShowPopup();
        }
    }

    void VmTabPage::OnVmManagerChanged()
    {
        // Called by MainWindow on the UI thread when VMManager fires
        // OnChanged. Refresh the tab header so renames + transition status
        // propagate, then re-evaluate the placeholder / client lifecycle —
        // running ↔ not-running transitions spawn or tear down rdphost.
        UpdateVmTabHeader();

        // Live upgrade basic -> enhanced. If we're connected in a BASIC session
        // (m_wantedEnhanced false) but the user wants enhanced and it's now
        // available + the back-off has expired, reconnect to pick up enhanced.
        // Once the reconnect respawns enhanced (m_wantedEnhanced becomes true)
        // this stops firing; a failed enhanced attempt re-arms the cooldown in
        // OnDisconnected, so it waits that out instead of thrashing.
        if (m_client && m_connected && !m_wantedEnhanced)
        {
            bool avail = false, running = false;
            if (auto vo = hyprv::app::vm::VMManager::Instance().GetByGuid(m_vmGuid))
            {
                running = vo->IsRunning();
                avail   = vo->enhancedSessionAvailable;
            }
            const bool cooldown =
                std::chrono::steady_clock::now() < m_enhancedCooldownUntil;
            if (running && avail && !cooldown
                && hyprv::app::settings::Settings::Instance().EnhancedSessionPref(m_vmGuid))
            {
                HyprvAppLog(L"[tab] enhanced available — upgrading basic->enhanced vm=%s",
                    m_vmGuid.c_str());
                ScheduleReconnect(std::chrono::milliseconds(250));
            }
        }

        UpdatePlaceholderAndClient();
    }

    void VmTabPage::TearDownClient()
    {
        if (!m_client) return;
        HyprvAppLog(L"[tab] TearDownClient vm=%s", m_vmGuid.c_str());
        m_client->Hide();
        m_client->Stop();
        m_client.reset();
        m_connected = false;
        // Deferred-show flag is tied to the existence of a connected
        // client; without one there's nothing to surface, so clear it
        // so a future spawn doesn't inherit stale state.
        m_deferredShow = false;
    }

    void VmTabPage::ScheduleReconnect(std::chrono::milliseconds delay)
    {
        if (!m_uiQueue) return;
        if (!m_reconnectTimer)
        {
            m_reconnectTimer = m_uiQueue.CreateTimer();
            m_reconnectTimer.IsRepeating(false);
            auto weak = get_weak();
            m_reconnectTimer.Tick([weak](auto const& t, auto const&) {
                t.Stop();
                if (auto self = weak.get()) self->DoReconnect();
            });
        }
        // Debounce: re-arming cancels any pending fire so a burst of disconnect
        // events (or repeated poll-upgrade requests) collapses to one reconnect.
        m_reconnectTimer.Stop();
        m_reconnectTimer.Interval(delay);
        m_reconnectTimer.Start();
    }

    void VmTabPage::DoReconnect()
    {
        // Only reconnect a Running VM. If it left Running while the timer was
        // pending (it was actually shutting down, not rebooting), just let
        // UpdatePlaceholderAndClient surface the right placeholder.
        bool running = false;
        if (auto vo = hyprv::app::vm::VMManager::Instance().GetByGuid(m_vmGuid))
            running = vo->IsRunning();
        // Reconnect when the VM is Running (normal recovery) OR when an
        // optimistic pre-connect is still in flight (the VM is mid-start and the
        // early mstscax attach failed because the console wasn't up yet —
        // UpdatePlaceholderAndClient respawns via the m_pendingConnect path and
        // ages the latch out after its timeout if the start never takes).
        if (!running && !m_pendingConnect)
        {
            UpdatePlaceholderAndClient();
            return;
        }
        HyprvAppLog(L"[tab] auto-reconnect vm=%s (running=%d pending=%d)",
            m_vmGuid.c_str(), running ? 1 : 0, m_pendingConnect ? 1 : 0);
        TearDownClient();
        m_lastState = hyprv::app::vm::VmState::Unknown;  // force respawn
        UpdatePlaceholderAndClient();   // StartConnection re-picks enhanced/basic
    }

    void VmTabPage::ApplyEnhancedSessionChange()
    {
        ReapplyConnectionSettings();
    }

    void VmTabPage::ReapplyConnectionSettings()
    {
        // No client → nothing to do. The next StartConnection will pick up
        // the new options out of Settings (enhanced pref + RDP options).
        if (!m_client)
        {
            HyprvAppLog(L"[tab] ReapplyConnectionSettings vm=%s — no client, no-op",
                m_vmGuid.c_str());
            return;
        }
        // The cleanest way to apply a changed connection option (enhanced-session
        // pref OR any per-VM RDP option) to a live session is to tear the child
        // down end-to-end and let UpdatePlaceholderAndClient respawn it on the
        // next running tick — that way mstscax state, AtlAxWin sizing, the popup
        // owner-window relationship, and the IPC pipe all get fresh, and
        // StartConnection re-reads RdpOptionsFor(guid) from scratch.
        HyprvAppLog(L"[tab] ReapplyConnectionSettings vm=%s — respawning rdphost",
            m_vmGuid.c_str());
        // Tearing down also drops the placeholder back to "Connecting..."
        // through UpdatePlaceholderAndClient since the VM is still Running.
        TearDownClient();
        m_lastState = hyprv::app::vm::VmState::Unknown;
        UpdatePlaceholderAndClient();
    }

    void VmTabPage::OnPlaceholderStartClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        HyprvAppLog(L"[tab] placeholder Start clicked vm=%s", m_vmGuid.c_str());
        // Latch "user requested start" so subsequent WMI poll ticks (which
        // can briefly still see the old Off/Saved/Paused state before the
        // state-change job registers) don't bring the Start button back.
        // Cleared once the state actually transitions, with a 5s timeout
        // safety so a failed/dropped request doesn't strand the placeholder.
        m_startRequested   = true;
        m_startRequestedAt = std::chrono::steady_clock::now();
        // Optimistic pre-connect: spawn the rdphost NOW so mstscax is already
        // connecting as the VM powers on, instead of waiting for the next poll
        // to report Running + a cold spawn (by which point the firmware
        // "Press any key to boot from CD" prompt has already timed out).
        m_pendingConnect   = true;
        m_pendingConnectAt = m_startRequestedAt;

        // Fresh start clears any prior connect-error latch / failure tally so the
        // new attempt isn't immediately short-circuited back into the error view.
        m_hadConnectError = false;
        m_connectFailures = 0;
        m_lastErrorText.clear();

        // Immediate visual ack — Hyper-V's WMI snapshot can take a second or
        // two to reflect that an Enabled job is in flight, and waiting for
        // that round-trip before updating the placeholder makes the click
        // feel unresponsive. Flip the placeholder to a transition look now;
        // the next OnVmManagerChanged tick will overwrite with the live
        // "Restoring (N%)" / "Starting..." text driven from WMI.
        using namespace Microsoft::UI::Xaml;
        if (placeholderState())
            placeholderState().Text(winrt::hstring{ L"Starting..." });
        if (placeholderHint())
            placeholderHint().Visibility(Visibility::Collapsed);
        if (placeholderStart())
            placeholderStart().Visibility(Visibility::Collapsed);
        if (placeholderRetry())
            placeholderRetry().Visibility(Visibility::Collapsed);

        hyprv::app::vm::VMManager::Instance().RequestStateChange(
            m_vmGuid, hyprv::app::vm::VmStateChange::Enabled);

        // Kick the optimistic connection right away (UpdatePlaceholderAndClient
        // spawns the rdphost because m_pendingConnect makes "want a client" true
        // even though the VM hasn't reached Running in the cache yet).
        UpdatePlaceholderAndClient();
    }

    void VmTabPage::OnPlaceholderRetryClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        HyprvAppLog(L"[tab] placeholder Retry clicked vm=%s", m_vmGuid.c_str());
        // Clear the error latch + tally and force a fresh spawn. The prior
        // (dead) client was already torn down in ShowConnectError. If the VM is
        // still Running, UpdatePlaceholderAndClient will StartConnection; if it
        // has since gone Off/Saved, it falls through to the normal placeholder.
        m_hadConnectError = false;
        m_connectFailures = 0;
        m_lastErrorText.clear();
        if (placeholderRetry())
            placeholderRetry().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        m_lastState = hyprv::app::vm::VmState::Unknown;   // force respawn edge
        UpdatePlaceholderAndClient();
    }

    // Latch a fatal connect failure and render the error view. Called from
    // OnDisconnected (after kMaxConnectFailures fatal attempts on a Running VM)
    // and from the rdphost Error path. Idempotent; safe to re-enter.
    void VmTabPage::ShowConnectError(std::wstring const& detail)
    {
        m_hadConnectError = true;
        if (!detail.empty())
            m_lastErrorText = detail;
        if (m_lastErrorText.empty())
            m_lastErrorText = L"The remote session was disconnected.";
        HyprvAppLog(L"[tab] connect error vm=%s: %s",
            m_vmGuid.c_str(), m_lastErrorText.c_str());
        // The session/child is dead — tear it down so Retry respawns cleanly.
        // TearDownClient does NOT recurse into UpdatePlaceholderAndClient.
        TearDownClient();
        std::wstring name;
        if (auto vo = hyprv::app::vm::VMManager::Instance().GetByGuid(m_vmGuid))
            name = vo->elementName;
        if (name.empty()) name = L"(no name)";
        RenderErrorPlaceholder(name);
    }

    // Populate the placeholder overlay in its error presentation: VM name,
    // "Connection failed" in the critical color, the error detail as the hint,
    // no Start button, a Retry button. Shared by ShowConnectError and
    // UpdatePlaceholderAndClient (which re-renders it on every poll tick while
    // m_hadConnectError holds, so a tab-switch-back keeps the error visible).
    void VmTabPage::RenderErrorPlaceholder(std::wstring const& vmName)
    {
        using namespace Microsoft::UI::Xaml;
        if (!placeholderRoot()) return;
        if (auto lo = loadingOverlay()) lo.Visibility(Visibility::Collapsed);
        placeholderVmName().Text(winrt::hstring{ vmName });
        placeholderState().Text(winrt::hstring{ L"Connection failed" });
        if (auto b = BrushFromTheme(L"SystemFillColorCriticalBrush"))
            placeholderState().Foreground(b);
        if (m_lastErrorText.empty())
        {
            placeholderHint().Visibility(Visibility::Collapsed);
        }
        else
        {
            placeholderHint().Text(winrt::hstring{ m_lastErrorText });
            placeholderHint().Visibility(Visibility::Visible);
        }
        placeholderStart().Visibility(Visibility::Collapsed);
        placeholderRetry().Visibility(Visibility::Visible);
        placeholderRoot().Visibility(Visibility::Visible);
    }

    // Label for VM states that are mid-transition (Starting / Stopping /
    // Saving / etc.). StableStatusLabel returns empty for these because the
    // tooltip used live statusText instead — but the placeholder needs
    // SOMETHING to show during the brief window between a job clearing and
    // the next stable state being reported, otherwise the user sees "Not
    // running" flicker between e.g. "Restoring (95%)" and "Running".
    static std::wstring TransitionStateLabel(hyprv::app::vm::VmState s)
    {
        using S = hyprv::app::vm::VmState;
        switch (s)
        {
        case S::Starting:    return L"Starting...";
        case S::Stopping:    return L"Stopping...";
        case S::Saving:      return L"Saving...";
        case S::FastSaving:  return L"Saving...";
        case S::Pausing:     return L"Pausing...";
        case S::Resuming:    return L"Resuming...";
        case S::Reset:       return L"Resetting...";
        default:             return {};
        }
    }

    // Drive the placeholder overlay and the rdphost-client lifecycle from the
    // VM's current state. Idempotent — safe to call on every WMI tick. Edge-
    // triggers off m_lastState so the client only spawns once per transition
    // into Running (and tears down once per transition out).
    void VmTabPage::UpdatePlaceholderAndClient()
    {
        using namespace Microsoft::UI::Xaml;
        if (!placeholderRoot()) return;   // XAML not loaded yet

        auto& vmm = hyprv::app::vm::VMManager::Instance();
        auto vmOpt = vmm.GetByGuid(m_vmGuid);

        auto showPlaceholder = [this](std::wstring const& name,
                                      std::wstring const& stateLine,
                                      std::wstring const& hint,
                                      bool showStartButton)
        {
            // Make sure the cold-start loading overlay is dismissed when
            // we transition to the regular placeholder content.
            if (auto lo = loadingOverlay()) lo.Visibility(Visibility::Collapsed);
            // Clear any error chrome — the Retry button + critical-colored
            // state text are only for the error view (RenderErrorPlaceholder).
            placeholderRetry().Visibility(Visibility::Collapsed);
            if (auto b = BrushFromTheme(L"TextFillColorSecondaryBrush"))
                placeholderState().Foreground(b);
            placeholderVmName().Text(winrt::hstring{ name });
            placeholderState().Text(winrt::hstring{ stateLine });
            if (hint.empty())
            {
                placeholderHint().Visibility(Visibility::Collapsed);
            }
            else
            {
                placeholderHint().Text(winrt::hstring{ hint });
                placeholderHint().Visibility(Visibility::Visible);
            }
            placeholderStart().Visibility(
                showStartButton ? Visibility::Visible : Visibility::Collapsed);
            placeholderRoot().Visibility(Visibility::Visible);
        };
        auto hidePlaceholder = [this]
        {
            placeholderRoot().Visibility(Visibility::Collapsed);
            if (auto lo = loadingOverlay()) lo.Visibility(Visibility::Collapsed);
        };

        // Cold-start case: this tab was restored from settings.json but
        // VMManager hasn't done its first poll yet, so we can't tell
        // "deleted VM" from "VM exists, we just don't know it yet." Show
        // a centered loading overlay until the first snapshot lands;
        // UpdatePlaceholderAndClient re-runs on every OnVmManagerChanged
        // so we'll naturally fall through to the right branch once the
        // cache is populated. Skip the rdphost spawn during this window.
        if (!vmOpt && !vmm.HasFirstSnapshot())
        {
            TearDownClient();
            placeholderRoot().Visibility(Visibility::Collapsed);
            if (auto lo = loadingOverlay()) lo.Visibility(Visibility::Visible);
            m_lastState = hyprv::app::vm::VmState::Unknown;
            return;
        }

        // VM not known to WMI (deleted between sessions, or post-delete tick).
        // VMManager has polled at least once, so a null vmOpt now is real.
        if (!vmOpt)
        {
            TearDownClient();
            showPlaceholder(L"(missing VM)",
                            L"This VM is no longer registered with Hyper-V.",
                            std::wstring{}, false);
            m_lastState = hyprv::app::vm::VmState::Unknown;
            return;
        }

        auto const& vm = *vmOpt;
        std::wstring displayName = vm.elementName.empty()
            ? std::wstring{ L"(no name)" }
            : vm.elementName;

        // Optimistic pre-connect bookkeeping: once the VM is actually Running,
        // the normal Running-driven path takes over, so drop the latch. Also
        // drop it after a generous timeout so a start that never took (failed /
        // cancelled) doesn't keep a doomed connection retrying forever.
        if (m_pendingConnect)
        {
            const auto elapsed = std::chrono::steady_clock::now() - m_pendingConnectAt;
            if (vm.IsRunning() || elapsed > std::chrono::seconds(25))
                m_pendingConnect = false;
        }

        // Connect-error latch: once a fatal connect failure has been surfaced
        // (OnDisconnected past the retry threshold, or an rdphost Error), we
        // stop respawning and keep the error view (with its Retry button) up so
        // we don't churn against a console that won't accept us. The latch
        // clears the moment the VM leaves a connectable state — then the normal
        // Off/Saved placeholder (with its Start button) is the right surface.
        if (m_hadConnectError)
        {
            if (!vm.IsRunning() && !m_pendingConnect)
            {
                m_hadConnectError = false;
                m_connectFailures = 0;
                m_lastErrorText.clear();
            }
            else
            {
                if (m_client) TearDownClient();
                RenderErrorPlaceholder(displayName);
                m_lastState = vm.state;
                return;
            }
        }

        // Want a live rdphost when the VM is Running OR a start we initiated is
        // still in flight (pre-connect). The pre-connect case spawns mstscax
        // early so it connects as the VM powers on and catches the firmware
        // boot screen instead of waiting for the next poll + a cold spawn.
        const bool wantClient = vm.IsRunning() || m_pendingConnect;
        if (wantClient)
        {
            // Spawn on the Running/pending edge. Already have a client →
            // idempotent no-op (StartConnection self-guards on m_client).
            if (!m_client)
            {
                HyprvAppLog(L"[tab] vm=%s want-client (running=%d pending=%d), spawning rdphost",
                            m_vmGuid.c_str(), vm.IsRunning() ? 1 : 0,
                            m_pendingConnect ? 1 : 0);
                // Running: hide the placeholder so the popup paints over the
                // transparent Border the moment it connects. Pre-connect (VM
                // still booting): keep a "Starting..." placeholder up until
                // OnConnected surfaces the popup, so the click ack doesn't blink
                // to an empty pane during the seconds-long boot.
                if (vm.IsRunning())
                    hidePlaceholder();
                else
                    showPlaceholder(displayName, L"Starting...", std::wstring{}, false);
                StartConnection();
            }
            else if (m_connected)
            {
                hidePlaceholder();
            }
            else
            {
                // Client exists but the session hasn't connected yet — show
                // "Starting..." while the VM is still powering on (pre-connect),
                // "Connecting..." once it's Running, over the rdpHost Border
                // until OnConnected flips m_connected and the popup goes live.
                showPlaceholder(displayName,
                                vm.IsRunning() ? L"Connecting..." : L"Starting...",
                                std::wstring{}, false);
            }
            m_lastState = vm.state;
            return;
        }

        // VM is not Running and no start is pending. Tear down any existing
        // client + surface the placeholder with the current state / transition.
        if (m_client) TearDownClient();

        // Pick the best state line. Priority:
        //   1. live statusText if WMI has a Concrete Job in flight ("Restoring
        //      (35%)" etc.) — most informative.
        //   2. stable label ("Off" / "Saved" / "Paused") for steady states.
        //   3. transition label ("Starting..." / "Saving...") for the gap
        //      between a job finishing and the next stable state being
        //      reported by WMI — without this the placeholder briefly reads
        //      "Not running" between "Restoring (95%)" and "Running".
        //   4. last-resort "Not running" only when none of the above apply.
        std::wstring stateLine = vm.statusText;
        if (stateLine.empty()) stateLine = vm.StableStatusLabel();
        if (stateLine.empty()) stateLine = TransitionStateLabel(vm.state);
        if (stateLine.empty()) stateLine = L"Not running";

        // Whether Hyper-V is actively running a job against this VM. WMI
        // populates statusText from Msvm_SummaryInformation.AsynchronousTasks
        // (active Msvm_ConcreteJob entries). Crucially the JOB starts BEFORE
        // the VM's EnabledState transitions away from Saved/Off/Paused —
        // during the early "Restoring (10%)" phase the state field still
        // reads Saved while the restore job runs. We must NOT show the Start
        // button whenever a job is in flight, regardless of the underlying
        // state field — that's what was bringing it back during transitions.
        const bool stableNotRunning = vm.IsOff() || vm.IsSaved() || vm.IsPaused();
        const bool jobInFlight = !vm.statusText.empty();

        // Maintain the start-requested latch as a fallback for the brief gap
        // between click and the first WMI tick that picks up the new job.
        // Once a job is visible (jobInFlight) the latch becomes redundant —
        // clear it then to free the 5s safety window for genuinely failed
        // requests. Also clear on the 5s timeout if no job ever appeared.
        if (m_startRequested)
        {
            const auto elapsed = std::chrono::steady_clock::now() - m_startRequestedAt;
            if (jobInFlight || elapsed > std::chrono::seconds(5))
            {
                m_startRequested = false;
            }
        }

        // Override the placeholder text to "Starting..." while the latch is
        // held — the WMI snapshot can still report the prior stable state
        // for a tick after the click, and "Saved" + no button feels broken.
        if (m_startRequested) stateLine = L"Starting...";

        // Show Start only when the VM is in a stable off/saved/paused state
        // AND no job is running AND we're not still in the post-click latch.
        bool showStart = stableNotRunning && !m_startRequested && !jobInFlight;
        std::wstring hint = showStart
            ? std::wstring{ L"Start this VM to connect." }
            : std::wstring{};
        showPlaceholder(displayName, stateLine, hint, showStart);
        m_lastState = vm.state;
    }

    void VmTabPage::ShutdownClient()
    {
        // Idempotent. Called from MainWindow::OnTabCloseRequested before the
        // TabViewItem is removed, so the rdphost child exits deterministically
        // even if TabView's internal item cache retains this page projection
        // past RemoveAt. Stop() handles the graceful Shutdown IPC + 2s wait +
        // TerminateProcess fallback; resetting the unique_ptr releases handles.
        if (m_client)
        {
            HyprvAppLog(L"[tab] ShutdownClient vm=%s — stopping rdphost child",
                m_vmGuid.c_str());
            m_client->Stop();
            m_client.reset();
        }
    }

    void VmTabPage::OnLoaded()
    {
        if (m_loaded) return;
        m_loaded = true;

        if (auto border = rdpHost())
        {
            border.SizeChanged({ this, &VmTabPage::OnRdpHostSizeChanged });
        }
        UpdateVmTabHeader();
        // Drive client lifecycle from VM state instead of unconditionally
        // spawning. Off/Saved VMs get a placeholder; only Running spawns
        // rdphost. Subsequent state changes flow through OnVmManagerChanged.
        UpdatePlaceholderAndClient();

        // First-mount path for the deferred-show race documented on
        // ShowPopup and m_deferredShow. By the time Loaded fires, the
        // layout pass has measured the Border. If a ShowPopup was queued
        // up while the Border had no size, surface the popup now AT the
        // correct rect — never at the stale fallback position the user
        // would otherwise flash through. Also covers the cold-start
        // restore case where the rdphost was spawned by VMManager's first
        // poll while this page was still unmounted: StartConnection used
        // the fallback rect, and now we need to repin.
        if (m_client && m_wantsVisible && m_connected && m_deferredShow && !m_popupSuppressed)
        {
            HyprvAppLog(L"[tab] OnLoaded vm=%s — resolving deferred show",
                m_vmGuid.c_str());
            UpdateRdphostBounds();
            m_client->Show();
            m_client->Focus();
            m_deferredShow = false;
        }
        else if (m_client)
        {
            HyprvAppLog(L"[tab] OnLoaded vm=%s — re-pinning popup post-layout",
                m_vmGuid.c_str());
            UpdateRdphostBounds();
        }
    }

    void VmTabPage::OnRdpHostSizeChanged(Windows::Foundation::IInspectable const&,
                                          Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        UpdateRdphostBounds();
        // Subsequent-mount path for the deferred-show race. OnLoaded only
        // fires the first time the page enters the visual tree; if the
        // user tab-switches away and back, the page is unloaded + re-
        // mounted but Loaded does NOT re-fire. SizeChanged DOES, because
        // the Border's size goes 0 → real on re-mount. Surface the
        // deferred popup here if it's still pending.
        if (m_client && m_wantsVisible && m_connected && m_deferredShow && !m_popupSuppressed)
        {
            HyprvAppLog(L"[tab] OnRdpHostSizeChanged vm=%s — resolving deferred show",
                m_vmGuid.c_str());
            m_client->Show();
            m_client->Focus();
            m_deferredShow = false;
        }
    }

    bool VmTabPage::ComputeBorderScreenRect(int& sx, int& sy, int& w, int& h)
    {
        if (!m_windowHwnd) return false;

        // With the outer Grid wrap + explicit VerticalAlignment=Stretch on
        // TabView + Stretch content alignment on TabViewItem, the layout chain
        // finally propagates a real height to the inner Border. Anchor to the
        // Border — it gives us the precise rect XAML actually laid out, no
        // magic offsets for tab strip / status bar. See gotchas memory for the
        // long story behind why those XAML attributes are required.
        auto border = rdpHost();
        if (!border) return false;
        const double aw = border.ActualWidth();
        const double ah = border.ActualHeight();
        if (aw <= 0 || ah <= 0) return false;

        // Transform from this Border's local coords to the window's root.
        // XamlRoot.Content is the visual tree root for the window hosting
        // this UserControl; UserControl doesn't have a Content() of its own
        // that maps to the window client area.
        auto root = XamlRoot() ? XamlRoot().Content() : nullptr;
        if (!root) return false;
        auto transform = border.TransformToVisual(root);
        auto topLeft = transform.TransformPoint(Windows::Foundation::Point{ 0.f, 0.f });
        auto bottomRight = transform.TransformPoint(
            Windows::Foundation::Point{ (float)aw, (float)ah });

        UINT dpi = GetDpiForWindow(m_windowHwnd);
        double scale = dpi / 96.0;

        POINT origin{ 0, 0 };
        ClientToScreen(m_windowHwnd, &origin);

        // Use std::lround on the rounded edges so the popup's right/bottom
        // exactly meet the Border's right/bottom in pixel space (truncation
        // could leave a 1-2 px stripe of underlying chrome visible).
        long sxL = origin.x + std::lround(topLeft.X * scale);
        long syL = origin.y + std::lround(topLeft.Y * scale);
        long exL = origin.x + std::lround(bottomRight.X * scale);
        long eyL = origin.y + std::lround(bottomRight.Y * scale);
        sx = static_cast<int>(sxL);
        sy = static_cast<int>(syL);
        w  = static_cast<int>(exL - sxL);
        h  = static_cast<int>(eyL - syL);
        return true;
    }

    void VmTabPage::UpdateRdphostBounds()
    {
        int sx = 0, sy = 0, bw = 0, bh = 0;
        const bool ok = ComputeBorderScreenRect(sx, sy, bw, bh);
        HyprvAppLog(L"[bnd] UpdateRdphostBounds: border=%s bw=%d bh=%d "
                    L"m_session=%dx%d m_wantedEnh=%d m_enhReady=%d",
            ok ? L"OK" : L"FAIL", bw, bh, m_sessionW, m_sessionH,
            m_wantedEnhanced ? 1 : 0, m_enhancedReady ? 1 : 0);
        if (!ok) return;
        if (!m_client) { HyprvAppLog(L"[bnd]   no client"); return; }

        // The popup is always sized to exactly the mstscax-reported session
        // size, centered inside the rdpHost Border. mstscax's render area
        // therefore exactly matches the popup, so the user never sees
        // mstscax-internal grey/black letterbox — outside the popup is just
        // hyprv chrome (dark Mica). Initial value of m_sessionW/H comes from
        // opts.desktopWidth/Height; OnDesktopResized updates it whenever
        // mstscax renegotiates the session size (initial connect, post-login
        // enhanced expansion, server-driven resize, etc).
        int w = m_sessionW;
        int h = m_sessionH;
        if (w <= 0) w = 1024;
        if (h <= 0) h = 768;
        // Cap to Border in case the window is smaller than the session.
        if (w > bw) w = bw;
        if (h > bh) h = bh;
        sx += (bw - w) / 2;
        sy += (bh - h) / 2;
        HyprvAppLog(L"[bnd]   final popup rect = %d,%d %dx%d", sx, sy, w, h);

        m_client->Reposition(sx, sy, w, h);
        // Only send UpdateSessionDisplaySettings post-OnLoginComplete in
        // enhanced mode (basic-mode mstscax doesn't honor it, and pre-login
        // enhanced returns E_FAIL). The actual resize confirmation arrives
        // back via OnDesktopResized which then re-runs this method with
        // m_sessionW/H reflecting whatever mstscax accepted.
        if (m_wantedEnhanced && m_enhancedReady)
        {
            // Honor the user's display-scale override (if any) here too — the
            // resize path conveys scale via the physical-size DPI (RdpHostClient
            // derives physWidth from this), so a pinned override keeps the
            // guest at the chosen scale instead of snapping back to host DPI.
            // 0 (Auto) = follow the host window's DPI.
            UINT dpi = m_dpiOverridePercent
                ? static_cast<UINT>(m_dpiOverridePercent * 96 / 100)
                : GetDpiForWindow(m_windowHwnd);
            // Request the full Border as the new session size so mstscax
            // grows to fill the window — popup will follow once mstscax
            // confirms via OnDesktopResized.
            m_client->UpdateSessionDisplaySettings(
                static_cast<uint32_t>(bw), static_cast<uint32_t>(bh), dpi, dpi);
        }
    }

    void VmTabPage::StartConnection()
    {
        // Idempotent: if an rdphost child is already attached (e.g. caller
        // is UpdatePlaceholderAndClient firing on a poll that already saw
        // Running), don't double-spawn. The previous client persists.
        if (m_client)
        {
            Log(L"[tab] StartConnection vm=%s — client already running, skip",
                m_vmGuid.c_str());
            return;
        }
        Log(L"[tab] StartConnection vm=%s", m_vmGuid.c_str());
        // HWND was set by MainWindow via SetWindowHwnd before this page was
        // added to the visual tree. No subclassing needed here — MainWindow
        // owns the window-level subclass and routes WM_WINDOWPOSCHANGED
        // through RefreshPopupBounds on the active tab.
        Log(L"[main] window=%p", static_cast<void*>(m_windowHwnd));
        if (!m_windowHwnd) { Log(L"[main] no window HWND, abort"); return; }

        const auto rdphostPath = FindRdphost();
        Log(L"[main] rdphost: %s", rdphostPath.c_str());
        if (GetFileAttributesW(rdphostPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Log(L"[main] rdphost not found");
            SetStatus(L"hyprv-rdphost.exe not found near hyprv.exe");
            return;
        }

        m_client = std::make_unique<hyprv::app::RdpHostClient>(rdphostPath);

        // Tag the client with a friendly label so this VM's rdphost log lines
        // are identifiable in the unified hyprv.log alongside other VMs.
        // Prefer the elementName from VMManager; fall back to GUID prefix.
        {
            std::wstring label;
            if (auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(m_vmGuid))
                label = vmOpt->elementName;
            if (label.empty())
                label = m_vmGuid.substr(0, 8);   // short GUID prefix as last resort
            m_client->SetLogLabel(std::move(label));
        }

        auto queue   = m_uiQueue;
        auto weakSelf = get_weak();

        m_client->OnConnecting = [queue, weakSelf]
        {
            HyprvAppLog(L"[cb] OnConnecting (rx thread); enqueueing");
            bool ok = queue.TryEnqueue([weakSelf] {
                HyprvAppLog(L"[ui] Connecting marshaled");
                if (auto self = weakSelf.get()) self->SetStatus(L"Connecting...");
            });
            HyprvAppLog(L"[cb] OnConnecting TryEnqueue=%d", ok ? 1 : 0);
        };
        m_client->OnConnected = [queue, weakSelf](hyprv::app::ConnectedInfo const& c)
        {
            HyprvAppLog(L"[cb] OnConnected (rx thread) enhanced=%d %ux%u",
                c.enhanced ? 1 : 0, c.desktopWidth, c.desktopHeight);
            auto text = winrt::hstring{ std::format(
                L"Connected — enhanced={}, desktop={}x{}",
                c.enhanced ? 1 : 0, c.desktopWidth, c.desktopHeight) };
            bool ok = queue.TryEnqueue([weakSelf, text] {
                HyprvAppLog(L"[ui] Connected marshaled, applying status + AttachInput");
                if (auto self = weakSelf.get())
                {
                    self->SetStatus(text);
                    // Flip the connection gate BEFORE deciding whether to
                    // surface the popup.
                    self->m_connected = true;
                    // A live session clears any prior connect-error latch and
                    // resets the consecutive-failure counter so a later benign
                    // drop starts counting fresh.
                    self->m_connectFailures = 0;
                    self->m_hadConnectError = false;
                    // NOTE: do NOT clear m_pendingConnect here. The optimistic
                    // connect can succeed BEFORE the poll reports the VM Running;
                    // if we dropped the latch now, the next poll (VM still not
                    // Running in the cache) would compute wantClient=false and
                    // tear this freshly-connected client down — then respawn a
                    // second one when Running lands, and the second mstscax
                    // attaching to the same console kicks the first (disc=2),
                    // churning the console so the user can't interact. The latch
                    // is cleared in UpdatePlaceholderAndClient once the VM is
                    // actually Running (or on its safety timeout).
                    // Drop the "Connecting..." placeholder now that the
                    // session is live.
                    self->UpdatePlaceholderAndClient();
                    if (self->m_client) self->m_client->AttachInput();
                    // Only surface the popup if MainWindow still considers
                    // this tab active. If the user navigated to another tab
                    // while we were connecting, m_wantsVisible was cleared by
                    // MainWindow's HidePopup call — showing now would paint
                    // over the currently-active tab. Tab-switch back will
                    // call ShowPopup which will then surface it.
                    if (self->m_wantsVisible && self->m_client && !self->m_popupSuppressed)
                    {
                        // Re-pin the popup BEFORE showing so the user
                        // doesn't see a single frame of the wrong-sized /
                        // wrong-positioned popup that was set by the
                        // initial EmbedInto call (which may have used a
                        // pre-layout Border rect).
                        self->UpdateRdphostBounds();
                        self->m_client->Show();
                    }
                    // If suppressed (a modal dialog is up), the popup stays hidden;
                    // SetPopupSuppressed(false) re-runs ShowPopup when it closes.
                    else
                    {
                        HyprvAppLog(L"[ui] connected but tab inactive — "
                                    L"deferring popup show");
                    }
                }
                else
                {
                    HyprvAppLog(L"[ui] Connected marshaled but weakSelf expired");
                }
            });
            HyprvAppLog(L"[cb] OnConnected TryEnqueue=%d", ok ? 1 : 0);
        };
        m_client->OnDesktopResized = [queue, weakSelf](uint32_t w, uint32_t h)
        {
            HyprvAppLog(L"[cb] OnDesktopResized %ux%u (rx thread)", w, h);
            queue.TryEnqueue([weakSelf, w, h] {
                if (auto self = weakSelf.get())
                {
                    // Pin the popup to whatever mstscax says it's rendering —
                    // for both enhanced (pre-login credential UI, post-login
                    // desktop) and basic (VMBus frame buffer) modes. Earlier
                    // we guarded against this in enhanced pre-login (VMPlex
                    // does too), but our architecture is "popup follows
                    // mstscax-reported size" — the whole point is to never
                    // see mstscax-internal letterbox. Without this event flow
                    // the popup stays at whatever size the initial connect
                    // used, which is wrong for the credential UI.
                    if (w > 0 && h > 0)
                    {
                        self->m_sessionW = static_cast<int>(w);
                        self->m_sessionH = static_cast<int>(h);
                        HyprvAppLog(L"[ui] session size now %dx%d — repinning popup",
                            self->m_sessionW, self->m_sessionH);
                        self->UpdateRdphostBounds();
                    }
                }
            });
        };
        m_client->OnEnhancedReady = [queue, weakSelf](bool r)
        {
            auto text = winrt::hstring{ std::format(L"EnhancedReady={}", r ? 1 : 0) };
            queue.TryEnqueue([weakSelf, text, r] {
                if (auto self = weakSelf.get())
                {
                    self->SetStatus(text);
                    if (r)
                    {
                        // Login surface is done — let UpdateRdphostBounds
                        // expand the popup from the pinned login size to the
                        // full Border rect.
                        self->m_enhancedReady = true;
                        self->UpdateRdphostBounds();
                        // mstscax sometimes silently rejects the first
                        // UpdateSessionDisplaySettings right after OnLoginComplete
                        // (it's still finishing the enhanced transition). The
                        // IPC cache would then record the rejected dims and
                        // short-circuit subsequent identical calls until the
                        // user wiggled the window. Schedule a deferred retry
                        // that drops the cache and re-sends — by 500ms post-
                        // OnLoginComplete the session is reliably ready.
                        auto timer = self->m_uiQueue.CreateTimer();
                        timer.Interval(std::chrono::milliseconds(500));
                        timer.IsRepeating(false);
                        timer.Tick([weakSelf, timer](auto const&, auto const&) {
                            timer.Stop();
                            if (auto s = weakSelf.get())
                            {
                                if (s->m_client)
                                    s->m_client->InvalidateSessionDisplayCache();
                                s->UpdateRdphostBounds();
                            }
                        });
                        timer.Start();
                    }
                }
            });
        };
        m_client->OnDisconnected = [queue, weakSelf](hyprv::app::DisconnectedInfo const& d)
        {
            const bool         fatal = d.fatal;
            const std::wstring desc  = d.description;
            auto text = winrt::hstring{ std::format(
                L"Disconnected — disc={}, ext={}, fatal={}",
                d.discReason, d.extendedReason, fatal ? 1 : 0) };
            queue.TryEnqueue([weakSelf, text, fatal, desc] {
                if (auto self = weakSelf.get())
                {
                    // Whether THIS attempt ever reached a live session, captured
                    // before we clear the gate. It distinguishes a connect
                    // failure (never connected → escalate after the threshold)
                    // from an established session ending (reboot / logoff →
                    // reconnect quietly).
                    const bool wasConnected = self->m_connected;
                    self->SetStatus(text);
                    self->m_connected = false;

                    // Enhanced back-off: an enhanced attempt that dropped before
                    // ever reaching login-complete means enhanced isn't usable
                    // right now (guest rebooting / boot screen / LIS not up), so
                    // suppress it briefly so the reconnect below uses basic and
                    // we don't thrash enhanced. A genuine enhanced session that
                    // WAS ready and then dropped (e.g. a reboot) still has
                    // m_enhancedReady true here, so it does NOT arm the cooldown
                    // — it can go straight back to enhanced once available.
                    if (self->m_wantedEnhanced && !self->m_enhancedReady)
                        self->m_enhancedCooldownUntil =
                            std::chrono::steady_clock::now() + std::chrono::seconds(20);
                    // Reset the enhanced-ready latch so a reconnect re-pins the
                    // popup at the next login.
                    self->m_enhancedReady = false;
                    if (self->m_client) self->m_client->Hide();

                    // Auto-reconnect while the VM is still Running so reboots /
                    // logoffs / dropped sessions recover without the user
                    // reopening the tab. StartConnection re-picks enhanced-vs-
                    // basic from the live snapshot. If the VM is NOT running
                    // (shutting down / saving), fall through to the placeholder.
                    bool running = false;
                    if (auto vo = hyprv::app::vm::VMManager::Instance()
                            .GetByGuid(self->m_vmGuid))
                        running = vo->IsRunning();
                    if (running)
                    {
                        // Escalate to a surfaced error only for a CONNECT attempt
                        // (never reached Connected) that keeps failing with a
                        // fatal reason — a running VM whose console won't accept
                        // us, not a transient. A dropped established session
                        // (wasConnected) is a benign reboot/logoff: reset the
                        // counter and reconnect quietly.
                        if (wasConnected)
                        {
                            self->m_connectFailures = 0;
                        }
                        else if (fatal &&
                                 ++self->m_connectFailures >= kMaxConnectFailures)
                        {
                            HyprvAppLog(L"[tab] vm=%s connect failed %d× (fatal) "
                                L"— surfacing error", self->m_vmGuid.c_str(),
                                self->m_connectFailures);
                            self->ShowConnectError(desc);
                            return;   // stop churning; the user must Retry
                        }
                        self->SetStatus(winrt::hstring{ L"Reconnecting..." });
                        // Reconnect FAST from a basic (console) session so a
                        // guest reboot's early-boot screen is caught — notably
                        // the "Press any key to boot from CD" prompt during an OS
                        // install, which only shows for a few seconds after each
                        // reboot. The VMBus console stays valid across the reboot
                        // (the VM is still Running), so a quick respawn lands
                        // within that window. Keep the slower debounce for
                        // enhanced: a too-quick reconnect there thrashes an RDP
                        // negotiation that isn't ready yet, and the 20s cooldown
                        // above already routes a not-ready enhanced attempt to
                        // basic on the next try.
                        auto delay = self->m_wantedEnhanced
                            ? std::chrono::milliseconds(2000)
                            : std::chrono::milliseconds(700);
                        self->ScheduleReconnect(delay);
                    }
                    else if (self->m_pendingConnect)
                    {
                        // Optimistic pre-connect: the VM isn't Running yet, so
                        // mstscax couldn't attach — but a start we kicked is in
                        // flight. Keep retrying fast so we attach the instant the
                        // VM's console comes up and catch the boot screen.
                        self->SetStatus(winrt::hstring{ L"Starting..." });
                        self->ScheduleReconnect(std::chrono::milliseconds(700));
                    }
                    else
                    {
                        self->UpdatePlaceholderAndClient();
                    }
                }
            });
        };
        m_client->OnError = [queue, weakSelf](uint32_t code)
        {
            queue.TryEnqueue([weakSelf, code] {
                auto self = weakSelf.get();
                if (!self) return;
                // Infrastructural failures from the rdphost child: the mstscax
                // Connect() HRESULT failed, a wire/protocol error, or a basic
                // session refused by a shielded VM. These are terminal for the
                // attempt — surface with a Retry rather than churning silently.
                std::wstring msg;
                switch (static_cast<hyprv::ipc::RdpErrorCode>(code))
                {
                case hyprv::ipc::RdpErrorCode::BasicSessionWithShieldedVm:
                    msg = L"This shielded VM requires an enhanced session, but "
                          L"enhanced mode isn't available right now.";
                    break;
                case hyprv::ipc::RdpErrorCode::HostStartupFailed:
                    msg = L"The session host couldn't start the remote connection "
                          L"to this VM. Make sure the VM is running and try again.";
                    break;
                case hyprv::ipc::RdpErrorCode::ProtocolError:
                    msg = L"An internal protocol error occurred between hyprv and "
                          L"its session host.";
                    break;
                default:
                    msg = std::format(L"The remote session reported an error "
                                      L"(code {}).", code);
                    break;
                }
                self->ShowConnectError(msg);
            });
        };
        m_client->OnChildExited = [queue, weakSelf](DWORD code)
        {
            auto text = winrt::hstring{ std::format(L"rdphost exited (0x{:x})", code) };
            queue.TryEnqueue([weakSelf, text] {
                if (auto self = weakSelf.get()) self->SetStatus(text);
            });
        };

        // VM GUID was set by MainWindow via Initialize() before this page
        // entered the visual tree.
        GUID vm = ParseGuid(m_vmGuid.c_str());

        // Decide enhanced vs basic from VMManager's last snapshot. WMI's
        // EnhancedSessionModeState reflects whether the in-guest integration
        // services are currently reporting enhanced as usable. Linux VMs
        // without LIS, the firmware/boot screen, and pre-login on some SKUs
        // all report not-available — for those we MUST send a basic-session
        // PCB (no ";EnhancedMode=1"), otherwise mstscax negotiates a session
        // that never produces pixels and the rdpHost stays blank.
        bool hvAvailable = false;
        if (auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(m_vmGuid))
            hvAvailable = vmOpt->enhancedSessionAvailable;
        // User pref acts as an opt-OUT only: if the user has turned enhanced
        // off for this VM we honor it; if they've left it on (default true)
        // we still gate on what Hyper-V is currently reporting. So turning
        // pref on for a guest that can't do enhanced (no LIS, boot screen)
        // is a no-op rather than a broken connection.
        const bool userPref = hyprv::app::settings::Settings::Instance()
            .EnhancedSessionPref(m_vmGuid);
        // Enhanced back-off: a recent enhanced attempt that dropped before
        // login-complete suppresses enhanced (use basic) until the cooldown
        // passes, so a reconnect during a boot/not-ready window doesn't thrash
        // enhanced. The OnVmManagerChanged poll-upgrade promotes basic→enhanced
        // once availability holds and the cooldown has expired.
        const bool cooldown =
            std::chrono::steady_clock::now() < m_enhancedCooldownUntil;
        const bool wantEnhanced = hvAvailable && userPref && !cooldown;
        Log(L"[tab] vm=%s enhancedAvailable=%d userPref=%d cooldown=%d -> wantEnhanced=%d",
            m_vmGuid.c_str(), hvAvailable ? 1 : 0, userPref ? 1 : 0,
            cooldown ? 1 : 0, wantEnhanced ? 1 : 0);

        m_wantedEnhanced = wantEnhanced;
        m_enhancedReady  = false;
        // Initial guess at session size — replaced on first OnDesktopResized.
        m_sessionW = 1024;
        m_sessionH = 768;

        // Pull the user's RDP preferences for this VM. Returns the
        // per-VM override if one exists in Settings.vmPrefs.<guid>.rdpOptions,
        // otherwise the app-wide defaults. Lookup is cheap (one
        // unordered_map probe under a mutex); doing it here means each
        // (re)connect picks up the user's latest tweaks without any
        // separate refresh path.
        const auto userOpts =
            hyprv::app::settings::Settings::Instance().RdpOptionsFor(m_vmGuid);

        hyprv::ipc::RdpOptions opts{};
        opts.port          = 2179;
        // Initial desktop dimensions matter mostly in basic-session mode
        // (enhanced renegotiates via UpdateSessionDisplaySettings post-
        // login). The popup-and-mstscax-render-area sizing path below
        // still uses m_sessionW/H = 1024×768 as the *initial* container —
        // mstscax adjusts on first DesktopResized callback. Wire what the
        // user picked anyway so basic-only guests honor it.
        opts.desktopWidth  = userOpts.initialDesktopWidth;
        opts.desktopHeight = userOpts.initialDesktopHeight;
        opts.colorDepth    = userOpts.colorDepth;
        // Map user redirection flags into the wire bitmask. Enhanced-
        // session bit is computed above from VM availability + user
        // pref; OR it into the user's redirection choices.
        uint32_t flags = wantEnhanced ? hyprv::ipc::Flag_EnhancedSession : 0;
        if (userOpts.redirectClipboard)    flags |= hyprv::ipc::Flag_RedirectClipboard;
        if (userOpts.redirectDrives)       flags |= hyprv::ipc::Flag_RedirectDrives;
        if (userOpts.redirectDevices)      flags |= hyprv::ipc::Flag_RedirectDevices;
        if (userOpts.redirectSmartCards)   flags |= hyprv::ipc::Flag_RedirectSmartCards;
        if (userOpts.redirectPorts)        flags |= hyprv::ipc::Flag_RedirectPorts;
        if (userOpts.audioCaptureRedirect) flags |= hyprv::ipc::Flag_AudioCaptureRedirect;
        opts.flags = flags;
        // Settings::RdpOptions::AudioMode values are deliberately aligned
        // with hyprv::ipc::AudioMode so the cast is a no-op numerically —
        // the type split keeps Settings.h free of an RdpIpc.h dependency.
        opts.audioMode = static_cast<uint8_t>(userOpts.audioMode);
        // Tell rdphost the display scale so mstscax renders the credential UI
        // at the right size. 96 DPI = 100%. Critical for the pre-login
        // credential surface to land centered in the popup instead of in a
        // tiny upper-left rect. See VMPlex RdpClient.cs:118-119. A user
        // override pins the scale; 0 (Auto) follows the host window's DPI.
        // m_dpiOverridePercent is cached for UpdateRdphostBounds' resize path.
        m_dpiOverridePercent = userOpts.dpiScaleOverridePercent;
        UINT dpiPx = m_windowHwnd ? GetDpiForWindow(m_windowHwnd) : 96;
        opts.dpiScalePercent = m_dpiOverridePercent
            ? m_dpiOverridePercent
            : static_cast<uint16_t>((dpiPx * 100 + 48) / 96);
        Log(L"[tab] dpi=%u (scale=%u%%) audio=%u flags=0x%x",
            dpiPx, opts.dpiScalePercent, opts.audioMode, opts.flags);

        // Compute the screen rect of the XAML Border. WinUI 3 sometimes
        // fires Loaded before the first arrange pass propagates to the
        // Border's ActualWidth/Height — when that happens we MUST NOT cap
        // the popup to a stale fallback. The popup's initial size leaks
        // directly into the AtlAxWin's WM_SIZE handler in the child, which
        // mstscax then uses as the session render rect — a too-small
        // initial popup means mstscax renders the entire session (including
        // the pre-login credential UI) at that smaller size, and we can't
        // recover post-Connect because mstscax's pre-login resize events
        // are unreliable. So: always use m_sessionW/H as the popup size,
        // even if the Border isn't measured yet. Subsequent
        // OnRdpHostSizeChanged will re-center within the actual Border.
        int sx = 0, sy = 0, bw = 0, bh = 0;
        const bool borderValid = ComputeBorderScreenRect(sx, sy, bw, bh);
        if (!borderValid)
        {
            POINT origin{ 0, 0 };
            ClientToScreen(m_windowHwnd, &origin);
            sx = origin.x; sy = origin.y + 32;   // 32 DIP tab strip
            // Use the session size as the "container" so centering is a no-op.
            bw = m_sessionW; bh = m_sessionH;
        }
        // Enhanced pre-login centering fix. The guest centers the credential UI
        // within its LOGICAL desktop = framebuffer px ÷ scale. A fixed 1024×768
        // framebuffer at a >100% host scale yields a cramped, odd logical size
        // (≈682×512 at 150%) that's below what Windows' logon UI lays out
        // cleanly, so it drifts toward a corner (tester report; at 100% the
        // logical size is a healthy 1024×768 and it centers fine). Scale the
        // initial framebuffer by the host scale so the LOGICAL size stays
        // constant at any display scale → the logon centers identically to
        // 100%. Cap to the measured Border so the framebuffer never exceeds the
        // popup (SmartSizing is off, so a larger-than-control framebuffer would
        // clip the logon UI). ENHANCED ONLY — it's a transient placeholder
        // resized to fill the window post-login (UpdateSessionDisplaySettings);
        // basic sessions render at the guest's own native resolution.
        if (wantEnhanced)
        {
            const uint32_t sc = opts.dpiScalePercent ? opts.dpiScalePercent : 100u;
            uint32_t fw = (opts.desktopWidth  ? opts.desktopWidth  : 1024u) * sc / 100u;
            uint32_t fh = (opts.desktopHeight ? opts.desktopHeight : 768u)  * sc / 100u;
            if (bw > 0 && fw > static_cast<uint32_t>(bw)) fw = static_cast<uint32_t>(bw);
            if (bh > 0 && fh > static_cast<uint32_t>(bh)) fh = static_cast<uint32_t>(bh);
            opts.desktopWidth  = static_cast<uint16_t>(fw);
            opts.desktopHeight = static_cast<uint16_t>(fh);
            m_sessionW = static_cast<int>(fw);
            m_sessionH = static_cast<int>(fh);
        }
        int w = m_sessionW;
        int h = m_sessionH;
        // No cap — popup at session size always. OnRdpHostSizeChanged will
        // re-pin once the Border has its real measured rect.
        sx += (bw - w) / 2;
        sy += (bh - h) / 2;
        Log(L"[tab] StartConnection initial rect: border=%s bw=%d bh=%d session=%dx%d "
            L"final=%d,%d %dx%d wantEnh=%d dpi=%u",
            borderValid ? L"OK" : L"FAIL", bw, bh, m_sessionW, m_sessionH,
            sx, sy, w, h, wantEnhanced ? 1 : 0,
            static_cast<unsigned>(opts.dpiScalePercent));

        SetStatus(L"Spawning rdphost...");
        Log(L"[main] calling ConnectLocalVm rect=(%d,%d,%d,%d)", sx, sy, w, h);
        if (!m_client->ConnectLocalVm(m_windowHwnd, sx, sy, w, h, vm, opts))
        {
            Log(L"[main] ConnectLocalVm setup failed");
            SetStatus(L"ConnectLocalVm setup failed");
            return;
        }
        Log(L"[main] ConnectLocalVm succeeded; child=%p", static_cast<void*>(m_client->childHwnd()));
        SetStatus(L"Waiting for VM connect...");
    }

    void VmTabPage::SetStatus(hstring const& text)
    {
        // Visible status TextBlock was removed when we went pure-VM-display.
        // Status info now lands in the right info flyout (#37); keep logging here
        // so existing call sites still produce useful diagnostics.
        HyprvAppLog(L"[status] %s", text.c_str());
    }

    // Refresh this page's containing TabViewItem header text. Plain VM
    // name — status/transition info lives in the right info flyout. The
    // header tracks the live ElementName so renames in Hyper-V Manager
    // propagate without a re-open.
    void VmTabPage::UpdateVmTabHeader()
    {
        if (!m_tabItem) return;
        auto& vmm = hyprv::app::vm::VMManager::Instance();
        auto vmOpt = vmm.GetByGuid(m_vmGuid);
        // Three cases, in priority order:
        //   1. VM known: use its ElementName (or "(no name)" if blank).
        //   2. VM unknown AND VMManager hasn't polled yet: cold-start
        //      restore — show "Loading..." so the tab header doesn't
        //      flash "(missing VM)" for the ~1 s before the first poll.
        //   3. VM unknown AND VMManager has polled: VM was genuinely
        //      deleted between sessions — show "(missing VM)".
        winrt::hstring header;
        if (vmOpt)
        {
            header = winrt::hstring{
                vmOpt->elementName.empty() ? L"(no name)" : vmOpt->elementName };
        }
        else if (!vmm.HasFirstSnapshot())
        {
            header = winrt::hstring{ L"Loading..." };
        }
        else
        {
            header = winrt::hstring{ L"(missing VM)" };
        }
        m_tabItem.Header(winrt::box_value(header));
    }
}
