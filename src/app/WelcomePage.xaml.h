#pragma once

#include "WelcomePage.g.h"

#include <string>

namespace winrt::hyprv_app::implementation
{
    struct MainWindow;

    // Start-page UserControl. Owned by a TabViewItem.Tag like VmTabPage,
    // distinguishable by try_as<WelcomePage>(). MainWindow's tab loops
    // (OnTabSelectionChanged, OnTabCloseRequested, OnVmManagerChanged)
    // discriminate on the projected type — keep that pattern.
    //
    // The page holds a weak_ref to MainWindow rather than a raw pointer:
    // tab-tear-away (future) re-parents pages into a new Window, and a
    // strong ref would create cross-window cycles.
    struct WelcomePage : WelcomePageT<WelcomePage>
    {
        WelcomePage();

        // Internal — called by MainWindow.OpenWelcomeTab right after ctor
        // (not in IDL; kept out of the projection so tab tear-away can
        // re-wire to a new owner without IDL/ABI churn).
        void SetMainWindow(winrt::weak_ref<MainWindow> const& weakWindow);

        // Back-pointer to the TabViewItem owning this page so the page can
        // request its own close (replace-on-open gesture). MainWindow's
        // OpenWelcomeTab wires this immediately after appending to the
        // TabView. Cleared on close so the same instance can't double-close.
        void SetTabItem(Microsoft::UI::Xaml::Controls::TabViewItem const& tab);

        // MainWindow pings on every VMManager OnChanged tick — refresh
        // the All-VMs list (state dots, names, transitions). Recents
        // also refreshes here since a VM rename should reflect.
        void OnVmManagerChanged();

        // MainWindow calls this when this tab becomes active. Rebuilds the
        // recents section in case BumpRecent landed while we weren't
        // mounted. Cheap — just re-reads Settings + VMManager snapshots.
        void OnTabActivated();

        // Currently-selected row's GUID (empty if nothing selected).
        // MainWindow reads this when activating a welcome tab so the info
        // flyout, if open, tracks the welcome page's selection rather
        // than going blank.
        std::wstring SelectedGuid() const { return m_selectedGuid; }

        // XAML event handlers — must be public so the generated .g.cpp can
        // bind them.
        void OnNewVmClick(Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Retry the Hyper-V connection (shown when VMManager couldn't reach
        // root\virtualization\v2 — Hyper-V not installed / no access).
        void OnRetryConnectClick(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
        // "Add remote host" button — opens the add dialog on the owning window.
        void OnAddRemoteHostClick(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Remote Hosts table sort-header click (Name / Address columns). Like
        // OnSortHeaderTapped but for the saved-hosts table; sender.Tag carries
        // the RemoteSortKey int.
        void OnRemoteSortHeaderTapped(Windows::Foundation::IInspectable const& sender,
                                      Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
        void OnFilterTextChanged(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        // Sort header click. All five column-header Borders route here;
        // sender.Tag() carries the SortKey (int) for that column. Clicking
        // the active column toggles direction; clicking another column
        // sets that column with ascending direction.
        void OnSortHeaderTapped(Windows::Foundation::IInspectable const& sender,
                                Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);

        // Click-away from the search box. Wired on the page-root Grid so
        // a tap anywhere outside the search pill takes focus off the
        // TextBox (otherwise the input keeps focus + caret indefinitely).
        void OnPageTapped(Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);

        // Keyboard navigation. Up/Down arrows move row selection within
        // the All-VMs list; Enter opens the selected row. Skipped when
        // the search box has focus (TextBox owns those keys for caret
        // movement + form submission).
        void OnPageKeyDown(Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);

        // Sort key — matches the Tag ints on the header Borders in
        // WelcomePage.xaml. Order matters: integer values are wire-coded
        // into the XAML Tag literals.
        enum class SortKey : int { Name = 0, Status = 1, Cpu = 2, Memory = 3, Uptime = 4 };
        enum class SortDir { Asc, Desc };
        // Remote Hosts table sort keys (Tag ints on the WelcomePage.xaml headers).
        enum class RemoteSortKey : int { Name = 0, Address = 1 };

    private:
        void RenderRecents();
        void RenderRemoteHosts();
        void UpdateRemoteSortIndicators();
        // Paint the single-click selection highlight on the remote-host row
        // matching m_selectedRemoteKey (transparent on the rest). Mirrors
        // ApplyRowSelection for the ALL VMs table.
        void ApplyRemoteRowSelection();
        void RenderAllVms();
        // Sync the sort-arrow glyphs to the current m_sortKey / m_sortDir.
        // Called after every sort change.
        void UpdateSortIndicators();
        // Sync the row-selection visual to m_selectedGuid. Walks m_rows
        // and paints exactly one selected background; transparent on the
        // rest. Idempotent.
        void ApplyRowSelection();

        // Open the VM in the owning MainWindow (or focus its existing tab).
        // No-op if the weak ref to MainWindow has dropped.
        void OpenVm(std::wstring const& guid, std::wstring const& name);

        // Open a saved Remote Host — same "Keep home tab open" replace-or-append
        // gesture as OpenVm, so the welcome tab behaves identically for VMs and
        // remote sessions.
        void OpenRemoteHost(std::wstring const& address);

        // One entry per VM row currently rendered in the All-VMs table.
        // Kept in display order so arrow-key navigation can index by
        // position. RenderAllVms rebuilds this on every refresh.
        struct RowEntry
        {
            std::wstring guid;
            std::wstring name;
            Microsoft::UI::Xaml::Controls::Border row;
        };
        // Recents pills, kept so a poll can refresh their dots in place rather
        // than rebuilding (which would dismiss an open right-click menu).
        std::vector<RowEntry> m_recentPills;

        // Signature of the last-rendered Remote Hosts set (joined name/address).
        // RenderRemoteHosts short-circuits when unchanged so the ~1s VMManager
        // poll doesn't rebuild the cards (which would dismiss an open menu).
        std::wstring m_remoteHostsSig;
        // Remote-host table rows (key = address) + the single-click selection,
        // mirroring m_rows / m_selectedGuid for the ALL VMs table.
        std::vector<RowEntry> m_remoteRows;
        std::wstring          m_selectedRemoteKey;

        winrt::weak_ref<MainWindow> m_mainWindow;
        // Owned by the TabView, but we hold the projection so OpenVm can
        // request close-self after handing focus to the new VM tab.
        Microsoft::UI::Xaml::Controls::TabViewItem m_tabItem{ nullptr };
        std::wstring m_filter;

        // Sort state. Defaults match the rail (name, ascending).
        SortKey m_sortKey = SortKey::Name;
        SortDir m_sortDir = SortDir::Asc;
        // Remote Hosts table sort state (independent of the ALL VMs table).
        RemoteSortKey m_remoteSortKey = RemoteSortKey::Name;
        SortDir       m_remoteSortDir = SortDir::Asc;

        // Selection state for the All-VMs table. Empty when nothing is
        // selected. Persists across RenderAllVms refreshes — the renderer
        // re-applies the highlight to whichever row still matches.
        std::wstring m_selectedGuid;
        std::vector<RowEntry> m_rows;

        // True once the first non-empty VM snapshot has been rendered.
        // Drives the "Loading VMs..." indicator: shown while false and
        // the VM list is empty; hidden the moment the first VMs land.
        // Stays hidden thereafter even if VMs all get deleted — at that
        // point an empty list is a real state, not "still loading".
        bool m_firstSnapshotRendered = false;
    };
}

namespace winrt::hyprv_app::factory_implementation
{
    struct WelcomePage : WelcomePageT<WelcomePage, implementation::WelcomePage>
    {
    };
}
