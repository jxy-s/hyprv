#pragma once

#include "AppSettingsPage.g.h"

#include <string>
#include <vector>

namespace winrt::hyprv_app::implementation
{
    struct MainWindow;

    // Application settings as a tab page. UserControl owned by a
    // TabViewItem.Tag like VmTabPage / WelcomePage; MainWindow's tab loops
    // discriminate via try_as<AppSettingsPage>.
    //
    // Instant-apply: every input writes through to Settings the moment it
    // changes. No Save button, no snapshot/diff, no Cancel. The user can
    // keep the tab open while testing changes; settings persist as they
    // touch them. Search filters the visible row set.
    struct AppSettingsPage : AppSettingsPageT<AppSettingsPage>
    {
        AppSettingsPage();

        // Wire the owning window so appearance-change callbacks can call
        // back to ApplyAppearance for live preview. MainWindow::
        // OpenAppSettingsTab sets this right after construction. Kept out
        // of the IDL projection so it can be re-wired on tab tear-away.
        void SetMainWindow(winrt::weak_ref<MainWindow> const& weakWindow);

        // XAML event handlers — must be public so the generated .g.cpp can
        // bind them.
        void OnSearchTextChanged(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void OnNavSelectionChanged(Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        // Restart-banner Action button. Flushes pending settings,
        // spawns a fresh hyprv.exe, then exits the current process.
        void OnRestartNowClick(Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        // Build every section + row into formHost. Called once from the
        // ctor; rows are stable for the lifetime of the page (toggles
        // mutate their state in place, no rebuild needed).
        void BuildSections();

        // Apply the current m_filter to every row; collapse sections with
        // no visible rows. Surface the "no results" hint when everything
        // is hidden.
        void ApplyFilter();

        // Arm or clear the restart-required banner. Called from the
        // theme combo callback when the user picks Black (set) or
        // anything else (clear). Idempotent.
        void SetRestartRequired(bool required);

        // One row in the settings form. element is the outermost UI widget
        // appended into a section's StackPanel. keywords is lowercase
        // text the search filter matches against (label + hint + extras).
        struct SettingRow
        {
            Microsoft::UI::Xaml::UIElement                element{ nullptr };
            std::wstring                                  keywords;
            // Index into m_sections — sections show only if at least one
            // of their owned rows is visible.
            size_t                                        sectionIndex = 0;
        };

        // One section in the form. The header TextBlock + container
        // StackPanel hide together when no children match the search; the
        // matching nav ListViewItem hides at the same time so the left
        // list stays honest about what's reachable.
        struct SettingSection
        {
            Microsoft::UI::Xaml::Controls::TextBlock      header{ nullptr };
            Microsoft::UI::Xaml::Controls::StackPanel     container{ nullptr };
            Microsoft::UI::Xaml::Controls::ListViewItem   navItem{ nullptr };
        };

        std::vector<SettingSection>                       m_sections;
        std::vector<SettingRow>                           m_rows;
        std::wstring                                      m_filter;   // lowercase

        // Back-pointer to the owning window so appearance-change callbacks
        // can call MainWindow::ApplyAppearance for live preview. Weak so
        // tab tear-away can move the page without dangling.
        winrt::weak_ref<MainWindow>                       m_mainWindow;

        // Backdrop-conditional slider rows. Exactly one is visible at a
        // time — ApplyFilter's slider gate flips them based on the
        // currently-selected backdrop. Tracked as UIElements (the
        // outer StackPanel containing label + slider + hint) rather
        // than the inner Slider so the whole row collapses cleanly.
        Microsoft::UI::Xaml::UIElement                    m_acrylicSliderRow{ nullptr };
        Microsoft::UI::Xaml::UIElement                    m_micaSliderRow{ nullptr };

        // True when the user has touched a setting that doesn't apply
        // live (currently: switching the theme to Black). Drives the
        // restart banner at the top of the page. Cleared when the user
        // reverts to a non-restart-required value.
        bool                                              m_restartRequired = false;

        // Suppress recursive selection events. OnNavSelectionChanged
        // scrolls the right pane; when the SelectedIndex is updated
        // programmatically (e.g. ApplyFilter clearing selection when
        // the selected section gets hidden) we don't want a feedback
        // scroll.
        bool m_suppressNavSelection = false;
    };
}

namespace winrt::hyprv_app::factory_implementation
{
    struct AppSettingsPage : AppSettingsPageT<AppSettingsPage, implementation::AppSettingsPage>
    {
    };
}
