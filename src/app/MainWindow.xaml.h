#pragma once

#include "MainWindow.g.h"

// DesktopAcrylicController / SystemBackdropConfiguration are members
// below — include them directly rather than relying on pch.h (this
// header is also pulled by generated files that don't see pch).
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include "VmTabPage.xaml.h"
#include "WelcomePage.xaml.h"

#include <memory>
#include <string>
#include <vector>

namespace hyprv::app::vm { struct VirtualMachine; }

namespace winrt::hyprv_app::implementation
{
    // Top-level Window. Owns the title bar, the TabView, the window-level
    // rail / info flyout chrome, the HWND subclass (for window-move/resize →
    // popup-bounds dispatch), and the list of open VmTabPage instances.
    // Each tab's VmTabPage is mounted into tabContentHost on selection
    // change so the rail + flyout stay window-level (one per Window).
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        // IDL-declared example stub (kept to keep the runtime class shape stable).
        int32_t MyProperty() { return 0; }
        void    MyProperty(int32_t) {}

    public:
        // ---- XAML event handlers — public so the generated .g.cpp can wire them. ----

        // Tab strip events.
        void OnTabSelectionChanged(Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnTabCloseRequested(Microsoft::UI::Xaml::Controls::TabView const&,
                                 Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const&);
        // '+' button on the tab strip — always creates a fresh welcome tab
        // (no dedup; the welcome page is intended as a Hyper-V hub the user
        // may want side-by-side instances of).
        void OnAddTabButtonClick(Microsoft::UI::Xaml::Controls::TabView const&,
                                 Windows::Foundation::IInspectable const&);
        // Tab tear-out (legacy drag/drop model — decide on mouse-release, no
        // window pre-created mid-drag). Drag a tab onto another window's strip →
        // move it there (TabStripDrop); drop outside any strip → new window
        // (TabDroppedOutside).
        void OnTabDragStarting(Microsoft::UI::Xaml::Controls::TabView const&,
            Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs const&);
        // Drop onto the TabView (the whole top bar — tabs + the empty footer):
        // move the dragged tab into THIS window at the drop position. We use a
        // generic Drop instead of AllowDropTabs/TabStripDrop because the latter
        // only fires over the tabs, leaving the empty title-bar a dead zone.
        void OnTabViewDrop(Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::DragEventArgs const&);
        // Window-level drop target (rootGrid + the title-bar footer): makes the
        // WHOLE window accept a dragged tab so the cursor reads "move" (not "no-
        // drop"); a drop on the title bar moves into this window, a drop on the
        // body tears out a new window.
        void OnWindowDragOver(Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::DragEventArgs const&);
        void OnWindowDrop(Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::DragEventArgs const&);
        void OnTabDragCompleted(Microsoft::UI::Xaml::Controls::TabView const&,
            Microsoft::UI::Xaml::Controls::TabViewTabDragCompletedEventArgs const&);
        // Title bar buttons.
        void OnHamburgerClick(Windows::Foundation::IInspectable const&,
                              Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSettingsClick(Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnInfoClick(Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Title-bar debugger button. OnDebuggerClick launches the configured
        // debugger for the active VM (detached from the job object);
        // UpdateDebuggerButton drives its visibility (global feature toggle) +
        // enabled state (active VM has debugger args). Refreshed on tab switch
        // and each VMManager poll so settings changes reflect within ~1s.
        void OnDebuggerClick(Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
        void UpdateDebuggerButton();

        // Rail.
        void OnRailVmSelected(Windows::Foundation::IInspectable const&,
                              Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        // Rail Remote Hosts ListView selection changed (single-click highlight,
        // native — matches the VM list). Clears the VM selection for a unified
        // single selection across the rail.
        void OnRemoteRailSelectionChanged(Windows::Foundation::IInspectable const&,
                                          Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        // Splitters (rail + flyout).
        void OnSplitterPointerEntered(Windows::Foundation::IInspectable const&,
                                      Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnSplitterPointerExited(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnSplitterPointerPressed(Windows::Foundation::IInspectable const&,
                                      Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnSplitterPointerMoved(Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnSplitterPointerReleased(Windows::Foundation::IInspectable const&,
                                       Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnFlyoutSplitterPointerPressed(Windows::Foundation::IInspectable const&,
                                            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnFlyoutSplitterPointerMoved(Windows::Foundation::IInspectable const&,
                                          Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnFlyoutSplitterPointerReleased(Windows::Foundation::IInspectable const&,
                                             Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);

        // Flyout chart + snapshot events.
        void OnChartSizeChanged(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void OnSnapshotSelected(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnTakeSnapshotClick(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Per-snapshot actions, invoked from each list item's context menu. They
        // resolve the snapshot's current name from the cache, confirm, and call
        // VMManager (the snapshot path is stable; the name can change via rename).
        Microsoft::UI::Xaml::Controls::MenuFlyout BuildSnapshotItemMenu(std::wstring path);
        std::wstring SnapshotNameForPath(std::wstring const& path);
        void ApplySnapshotAction(std::wstring path);
        void DeleteSnapshotAction(std::wstring path, bool subtree);
        void RenameSnapshotAction(std::wstring path);

        // Open a tab for the given VM, or focus the existing one if a tab
        // for that GUID is already open. Returns the (existing or new) page.
        // Side effect: every successful open bumps the VM into Settings'
        // recents MRU so the WelcomePage Recents section populates.
        winrt::hyprv_app::VmTabPage OpenVmTab(hstring const& vmGuid,
                                              hstring const& displayName);

        // Open a fresh welcome tab and focus it. No dedup — every call
        // produces a new tab. OnActivated calls this once at cold start
        // when there are no other tabs; OnAddTabButtonClick calls it on
        // every '+' click.
        winrt::hyprv_app::WelcomePage OpenWelcomeTab();

        // Open the application-settings tab, or focus the existing one if
        // already open. Deduped (unlike welcome) — settings is a single
        // global surface; opening it twice would split the user's
        // attention. Wired from the title-bar gear button.
        winrt::hyprv_app::AppSettingsPage OpenAppSettingsTab();

        // Show the "New VM..." wizard (modal ContentDialog). On a successful
        // create it opens a tab for the new VM. Wired from the welcome page's
        // "New VM" button. Self-contained: builds the dialog with this window's
        // XamlRoot + theme + popup suppression, then reads the created VM's
        // GUID/name back after ShowAsync.
        void OpenNewVmDialog();

        // Open (or focus) a saved Remote Host's RDP tab. address is the host's
        // Settings::RemoteHost.address key. Deduped — connecting to an already-
        // open host focuses its tab. autoConnect=true (user-initiated open) starts
        // the session immediately; false (session restore on launch) shows an idle
        // "Connect" placeholder so launch doesn't spam credential prompts.
        winrt::hyprv_app::RemoteHostTabPage OpenRemoteHostTab(hstring const& address,
                                                              bool autoConnect = true);

        // Show the add/edit Remote Host dialog (modal). Empty address = add a
        // new host; a non-empty address edits that saved host. On Save it
        // refreshes the rail; for a new host it also opens its tab. Wired from
        // the welcome "Add remote host" button + the host context menus.
        void OpenRemoteHostDialog(hstring const& address);

        // Rebuild the rail's Remote Hosts expander from Settings::RemoteHosts()
        // and flip its visibility (hidden when there are no saved hosts). Called
        // alongside RenderRail and after the add/edit/forget flows.
        void RenderRemoteHostsRail();

        // Remove a saved host: drop it from Settings, close any open tab for it,
        // and refresh the rail. Wired from the host context menus (Forget).
        void ForgetRemoteHost(hstring const& address);

        // Respawn the rdphost child of the open tab for `address` (if any) so an
        // edited host's RDP options / credentials apply to a live session. Used
        // by the edit dialog after a Save that changed connection settings.
        void ReconnectRemoteHostTab(std::wstring const& address);

        // Close (and tear down) a tab. Encapsulates the per-tab-type
        // teardown so OnTabCloseRequested and WelcomePage's replace-on-
        // open gesture share one path. Idempotent — no-op if the item
        // isn't currently in the tab strip.
        void CloseTabItem(Microsoft::UI::Xaml::Controls::TabViewItem const& item);

        // "Replace" gesture: open (or focus) a VM tab AND close the given
        // tab, then force-select the VM tab. Used by WelcomePage to swap
        // itself out when the user opens a VM from a tile. Single entry
        // point so the open / close / re-select ordering — which interacts
        // with WinUI TabView's deferred SelectionChanged dispatch — lives
        // in one place. Idempotent for an already-open VM (focuses
        // existing tab, then closes the welcome tab).
        void ReplaceTabWith(Microsoft::UI::Xaml::Controls::TabViewItem const& toClose,
                            hstring const& vmGuid, hstring const& displayName);
        // Remote-host sibling of ReplaceTabWith — opens (or focuses) the host tab,
        // closes the welcome tab, force-selects the host tab.
        void ReplaceTabWithRemoteHost(Microsoft::UI::Xaml::Controls::TabViewItem const& toClose,
                                      hstring const& address);

    private:
        void OnActivated(Microsoft::UI::Xaml::WindowActivatedEventArgs const&);
        // Three-step setup, called in order from OnActivated. Order matters:
        // restoring geometry must happen between resolving the HWND and
        // attaching the subclass — otherwise WinUI's initial layout pass
        // fires WM_WINDOWPOSCHANGED → PersistGeometry → clobbers the loaded
        // Settings::m_window before ApplyPersistedGeometry ever reads it.
        void ResolveWindowHwnd();
        void AttachWindowSubclass();
        void ExtendIntoTitleBar();

        // Apply window position/size + rail/flyout widths from persisted
        // Settings. Called once per session right after EnsureWindowHwnd.
        // Skips fields that are zero (defaults — use OS placement).
        void ApplyPersistedGeometry();

        // Snapshot current window rect + rail/flyout widths into Settings.
        // Called from splitter release, rail/flyout toggle, and WM_CLOSE.
        // The debounced save thread coalesces rapid calls.
        void PersistGeometry();

        // Snapshot the current tab strip (types + identifiers + selected
        // index) into Settings. Called after every tab open / close /
        // selection change so a crash, kill, or normal close all leave
        // the same restorable state. Cheap: SetOpenTabs is a no-op if
        // nothing changed.
        void PersistOpenTabs();

        // Rebuild the tab strip from Settings::OpenTabs(). Called from
        // OnActivated after VMManager is constructed (so VM lookups
        // succeed). Missing VMs (deleted between sessions) skip silently.
        // If the persisted list is empty, a fresh welcome tab is opened.
        void RestoreOpenTabs();

        // Suppress PersistOpenTabs writes while RestoreOpenTabs is in
        // flight — the restore creates tabs one-by-one which would
        // otherwise fire a Persist on each call, all racing the final
        // SelectedItem write.
        bool m_restoringTabs = false;

        // Lookup of the currently-selected VmTabPage. Returns null when no
        // tabs are open.
        winrt::hyprv_app::VmTabPage ActiveTab();

        // VMManager OnChanged dispatch. Rebuilds the rail, refreshes the
        // flyout content, and pings every open VmTabPage so the per-tab
        // headers track rename / state transitions.
        void OnVmManagerChanged();

        // Rail/flyout toggles.
        void ToggleRail();
        void ToggleInfoFlyout();
        // Refresh the rail's selection highlight to track the active tab.
        void SyncRailSelectionToActive();

        // Flyout update helpers.
        void UpdateInfoFlyoutContent();
        void UpdateInfoFlyoutCharts(hyprv::app::vm::VirtualMachine const& vm);
        void UpdateInfoFlyoutSnapshots(hyprv::app::vm::VirtualMachine const& vm);
        void UpdateInfoFlyoutAdapters(hyprv::app::vm::VirtualMachine const& vm);
        void UpdateInfoFlyoutDisks(hyprv::app::vm::VirtualMachine const& vm);
        // Clear every section below the header back to its empty state.
        // Used for both "no VM selected" and "VM not found" — only the
        // header label differs between those two cases.
        void ResetFlyoutSections();

    public:
        // True while the info flyout is open (column width > 0). Checking
        // the actual column avoids the prior bug where m_flyoutVmGuid was
        // used as a proxy and got out of sync with the visible state.
        bool IsFlyoutOpen() const;

        // Switch the flyout to a different VM. No-op if the flyout isn't
        // open. Called from welcome-page row selection so single-clicking
        // a VM tile populates the flyout even before a VM tab exists.
        void SetFlyoutVm(std::wstring const& guid);

        // Find the open VmTabPage for a given VM GUID (case-insensitive),
        // or nullptr if no tab is open for it. Used by surfaces that don't
        // hold a direct page reference (context-menu callbacks built from
        // VmTileFactory) but need to drive per-tab state — e.g. flipping
        // enhanced-session mid-session via ApplyEnhancedSessionChange.
        winrt::hyprv_app::VmTabPage FindVmTab(std::wstring const& guid);

        // Apply the persisted Appearance (backdrop + theme + optional
        // black-background overrides) to this window. Called from
        // OnActivated once on launch and from AppSettingsPage whenever
        // the user changes either dropdown. Backdrop swap + RequestedTheme
        // cascade live; the Black-theme brush overrides apply on the
        // current window for newly-rendered visuals but already-rendered
        // controls may need a relaunch to fully repaint.
        void ApplyAppearance();

        // Fast path for the intensity sliders — sets only TintOpacity on
        // the existing live controller, no teardown / rebuild. Called by
        // the AppSettingsPage slider's ValueChanged so a drag feels
        // smooth instead of flashing through a full controller rebuild
        // each tick. No-op if the matching controller isn't currently
        // active.
        void UpdateBackdropTintOpacity(double opacity);

        // Reference-counted popup suppression for all open VmTabPages.
        // Used to hide the rdphost popups while a modal ContentDialog is
        // showing — the popup is a top-level HWND owned by the rdphost
        // child process that paints above the XAML composition surface,
        // so dialogs render behind it. PushPopupSuppression hides every
        // tab's popup; PopPopupSuppression re-shows them once the last
        // outstanding caller releases. Nested dialogs increment the count
        // so the popups stay hidden until the outermost dialog closes.
        // RAII via PopupSuppressionScope below is the preferred call form.
        void PushPopupSuppression();
        void PopPopupSuppression();

        // Tab tear-out: mark this window SECONDARY (torn-off) before the
        // framework activates it. A secondary skips persisted-geometry restore
        // (the framework positions it under the cursor) and session restore (it
        // starts empty, populated by the moved tab), and does NOT persist its
        // geometry / open-tabs to the single global Settings blob — only the
        // primary does, so a secondary can't corrupt the primary's restore.
        void MarkSecondary() { m_isPrimary = false; }

        // Re-wire an already-built TabViewItem (moved here by tab tear-out) to
        // THIS window: re-own the rdphost popup (VM/Remote), re-point the
        // page→window weak_ref (Welcome/Settings), rebuild the context flyout,
        // select it. The page + its live RDP session move intact — no rebuild.
        void AdoptMovedTab(Microsoft::UI::Xaml::Controls::TabViewItem const& item);

        // After a tab has left THIS window (tear-out or cross-window drop): if the
        // strip is now empty, a secondary window closes itself and the primary
        // opens a welcome tab (its home base); otherwise just re-persist.
        void HandleSourceAfterDetach();

        // Tear down every tab's rdphost child. Called from this window's Closed
        // handler — a secondary window's close doesn't exit the process, so the
        // job-object reap won't fire and the children must be stopped explicitly.
        void ShutdownAllTabClients();

        // Move a dragged tab into THIS window's strip at `index` (-1 = append) —
        // detaches it from its source window + re-wires. No-op if it's already
        // ours. Used by the bar drop (TabStripDrop).
        void MoveTabToThisWindow(Microsoft::UI::Xaml::Controls::TabViewItem const& item,
                                 int index);
        // Tear `item` out into a NEW window at the cursor (body / desktop drop).
        void SpawnWindowForTab(Microsoft::UI::Xaml::Controls::TabViewItem const& item);

        // The hyprv MainWindow whose top-level HWND is under a screen point, or
        // null (the bare desktop / a foreign window). Used to route a release that
        // no OLE drop target accepted (DropResult None — i.e. over a window's title
        // bar caption, which can't be an OLE target) to the right destination.
        static MainWindow* WindowUnderPoint(POINT pt);

        // The TabViewItem currently under the mouse cursor (hit-test by screen
        // bounds), or null. Used at drag-start to identify the grabbed tab —
        // TabViewTabDragStartingEventArgs.Tab is unreliable with our page-in-Tag
        // model (it reports the first tab).
        Microsoft::UI::Xaml::Controls::TabViewItem TabUnderCursor();

        // Stash a tab the new (secondary) window should adopt once its XAML is
        // loaded. Tear-out (TabDroppedOutside) creates the window + sets this,
        // then Activate()s it; OnActivated appends + AdoptMovedTab once tabView()
        // exists (it isn't ready synchronously right after make<MainWindow>()).
        void SetPendingAdopt(Microsoft::UI::Xaml::Controls::TabViewItem const& item)
        { m_pendingAdoptItem = item; }
    private:

        // Subclass on m_windowHwnd so we can refresh the active tab's popup
        // position on WM_MOVE / WM_SIZE / WM_WINDOWPOSCHANGED.
        static LRESULT CALLBACK WindowSubclassProc(HWND, UINT, WPARAM, LPARAM,
                                                   UINT_PTR, DWORD_PTR);

        HWND                                          m_windowHwnd     = nullptr;
        bool                                          m_subclassed     = false;
        bool                                          m_activated      = false;
        // False for a torn-off (secondary) window — gates persisted-geometry +
        // session restore on activate and persistence writes (MarkSecondary).
        bool                                          m_isPrimary      = true;
        // VMManager multicast subscription tokens (removed on this window's Closed).
        uint64_t                                      m_onChangedToken = 0;
        uint64_t                                      m_onErrorToken   = 0;
        // Tab handed to a freshly-torn-out secondary window to adopt on activate.
        Microsoft::UI::Xaml::Controls::TabViewItem    m_pendingAdoptItem{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueue   m_uiQueue{ nullptr };

        // Rail splitter drag state.
        bool   m_railDragging      = false;
        double m_railDragStartX    = 0;
        double m_railDragStartWidth= 0;
        double m_railSavedWidth    = 190;

        // Flyout splitter drag state.
        bool   m_flyoutDragging      = false;
        double m_flyoutDragStartX    = 0;
        double m_flyoutDragStartWidth= 0;
        double m_flyoutSavedWidth    = 400;

        // Info flyout selection — tracks which VM the flyout is showing.
        // Empty means closed.
        std::wstring m_flyoutVmGuid;
        std::wstring m_selectedSnapshotPath;
        std::wstring m_selectedSnapshotName;

        // Popup-suppression refcount. Incremented by PushPopupSuppression,
        // decremented by Pop. Each tab's popup hides on the 0->1 edge and
        // re-shows on 1->0. Single-threaded — only touched from the UI
        // thread so no atomic needed.
        int m_popupSuppressionDepth = 0;

        // Custom backdrop controllers. The built-in MicaBackdrop /
        // DesktopAcrylicBackdrop don't expose TintOpacity (and Acrylic
        // also drops back to solid colour on deactivation). Managing
        // MicaController / DesktopAcrylicController directly lets us pin
        // IsInputActive=true AND surface tint opacity to the user. Only
        // one of these is non-null at a time — whichever matches the
        // current Appearance.backdrop. m_backdropConfig is shared
        // between them (only one is active at a time).
        winrt::Microsoft::UI::Composition::SystemBackdrops::MicaController              m_micaController{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController     m_acrylicController{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_backdropConfig{ nullptr };
    };

    // RAII wrapper that bookends a co_await dlg.ShowAsync(). Construct
    // before the await with a pointer to the MainWindow implementation;
    // the destructor pops on coroutine resume after the dialog closes.
    // Safe to copy a weak_ref + Resolve at scope entry if the call site
    // can't keep a hot pointer (UI-thread-only access).
    struct PopupSuppressionScope
    {
        MainWindow* mw;
        PopupSuppressionScope(MainWindow* w) : mw(w) { if (mw) mw->PushPopupSuppression(); }
        ~PopupSuppressionScope() { if (mw) mw->PopPopupSuppression(); }
        PopupSuppressionScope(PopupSuppressionScope const&) = delete;
        PopupSuppressionScope& operator=(PopupSuppressionScope const&) = delete;
    };
}

namespace winrt::hyprv_app::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
