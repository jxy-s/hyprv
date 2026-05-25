#include "pch.h"
#include "AppSettingsPage.xaml.h"
#if __has_include("AppSettingsPage.g.cpp")
#include "AppSettingsPage.g.cpp"
#endif

#include "settings/Settings.h"
#include "MainWindow.xaml.h"

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Text.h>

#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <functional>

extern void HyprvAppLog(const wchar_t* fmt, ...);

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace
{
    // ---- Confirmations table -----------------------------------------------
    // Keep in sync with Settings.cpp::DefaultConfirmationEnabled. Adding a
    // new action key means adding one row here and one default there.
    // No hint string: confirmations render as one-line checkboxes so the
    // label is the entire row.
    struct ConfirmEntry
    {
        wchar_t const* key;
        wchar_t const* label;
    };
    // Ordering matches the VM context menu so the user can scan the
    // settings list in the same order they think about the actions:
    // graceful power, hard power, delete, session, snapshots. Items not
    // surfaced in the right-click menu (Apply / Delete snapshot, Delete
    // snapshot subtree — invoked from the flyout's snapshot tree) come
    // last in their natural order. Labels match the menu text exactly
    // ("Save", not "Save (suspend to disk)") so the cross-reference is
    // unambiguous.
    static ConfirmEntry const kConfirmEntries[] = {
        { L"startResume",            L"Start / Resume" },
        { L"pause",                  L"Pause" },
        { L"save",                   L"Save" },
        { L"restart",                L"Restart" },
        { L"shutdown",               L"Shut down" },
        { L"reset",                  L"Reset" },
        { L"turnOff",                L"Turn off" },
        { L"deleteVm",               L"Delete VM" },
        { L"toggleEnhancedSession",  L"Enhanced session" },
        { L"takeSnapshot",           L"Take snapshot" },
        { L"revertToLastSnapshot",   L"Revert to last snapshot" },
        { L"applySnapshot",          L"Apply snapshot" },
        { L"deleteSnapshot",         L"Delete snapshot" },
        { L"deleteSnapshotSubtree",  L"Delete snapshot subtree" },
    };

    // Lowercase a wide string in place. Used by the search filter — both
    // the row keywords and the search box text are lowercased so the match
    // is case-insensitive without per-comparison conversion cost.
    void ToLowerInPlace(std::wstring& s)
    {
        for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    }
    std::wstring ToLower(std::wstring s) { ToLowerInPlace(s); return s; }
}

namespace winrt::hyprv_app::implementation
{
    AppSettingsPage::AppSettingsPage()
    {
        // BuildSections needs the named XAML elements (formHost, searchBox)
        // resolved, which happens in InitializeComponent.
        InitializeComponent();
        BuildSections();
    }

    void AppSettingsPage::SetMainWindow(winrt::weak_ref<MainWindow> const& weakWindow)
    {
        m_mainWindow = weakWindow;
    }

    // --- helpers ------------------------------------------------------------

    // Build a [label + Slider + hint] vertical block. ValueChanged fires
    // the supplied lambda with the integer position (0..max). Used for
    // continuous-feeling settings like acrylic tint opacity.
    static StackPanel MakeSliderRow(
        std::wstring const& label,
        std::wstring const& hint,
        int minValue, int maxValue, int initialValue,
        std::function<void(int)> onChanged)
    {
        StackPanel row;
        row.Spacing(2);
        row.Padding({ 0, 6, 0, 6 });

        TextBlock head;
        head.Text(winrt::hstring{ label });
        head.FontSize(12);
        head.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        row.Children().Append(head);

        Microsoft::UI::Xaml::Controls::Slider slider;
        slider.Minimum(minValue);
        slider.Maximum(maxValue);
        slider.Value(initialValue);
        slider.StepFrequency(1);
        slider.TickFrequency(0);   // 0 → no ticks rendered
        slider.Width(320);
        slider.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Left);
        slider.Margin({ 0, 4, 0, 0 });
        slider.ValueChanged([cb = std::move(onChanged)](
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&)
        {
            if (auto s = sender.try_as<Microsoft::UI::Xaml::Controls::Slider>())
                cb(static_cast<int>(s.Value()));
        });
        row.Children().Append(slider);

        if (!hint.empty())
        {
            TextBlock hintBlock;
            hintBlock.Text(winrt::hstring{ hint });
            if (auto style = Application::Current().Resources().TryLookup(
                    box_value(winrt::hstring{ L"AppSettingsHint" })))
                hintBlock.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            hintBlock.Margin({ 0, 2, 0, 0 });
            row.Children().Append(hintBlock);
        }
        return row;
    }

    // Build a [label + ComboBox + hint] vertical block. The
    // SelectionChanged event fires the supplied lambda with the picked
    // item index — instant apply. items[0] is the initial selection
    // unless `initialIndex` overrides. Returns the outer StackPanel for
    // tracking in m_rows.
    static StackPanel MakeComboRow(
        std::wstring const& label,
        std::wstring const& hint,
        std::vector<std::wstring> const& items,
        int initialIndex,
        std::function<void(int)> onChanged)
    {
        StackPanel row;
        row.Spacing(2);
        row.Padding({ 0, 6, 0, 6 });

        TextBlock head;
        head.Text(winrt::hstring{ label });
        head.FontSize(12);
        head.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        row.Children().Append(head);

        ComboBox combo;
        combo.FontSize(12);
        combo.MinHeight(28);
        combo.Margin({ 0, 4, 0, 0 });
        // Explicit width + left alignment. Without an explicit width, the
        // ComboBox shrinks to the SelectedItem text, which (a) leaves the
        // dropdown chevron in a tiny hit-test region — easy to misclick —
        // and (b) makes consecutive dropdowns appear different widths
        // depending on which item is selected. 320 DIP comfortably fits
        // every label in this dialog while keeping the chevron a fat
        // target.
        combo.Width(320);
        combo.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Left);
        for (auto const& s : items)
            combo.Items().Append(box_value(winrt::hstring{ s }));
        if (initialIndex >= 0 && static_cast<size_t>(initialIndex) < items.size())
            combo.SelectedIndex(initialIndex);
        combo.SelectionChanged([cb = std::move(onChanged)](
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
        {
            auto c = sender.try_as<ComboBox>();
            if (c) cb(c.SelectedIndex());
        });
        row.Children().Append(combo);

        if (!hint.empty())
        {
            TextBlock hintBlock;
            hintBlock.Text(winrt::hstring{ hint });
            // Theme-aware Style from App.xaml — ThemeResource in the
            // Setter re-resolves on RequestedTheme change. A direct
            // .Foreground(Lookup(...)) would freeze the brush at
            // construction time.
            if (auto style = Application::Current().Resources().TryLookup(
                    box_value(winrt::hstring{ L"AppSettingsHint" })))
                hintBlock.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            hintBlock.Margin({ 0, 2, 0, 0 });
            row.Children().Append(hintBlock);
        }
        return row;
    }

    // Build a [label + ToggleSwitch + hint] vertical block. The Toggled
    // event fires the supplied lambda on each user flip — instant apply.
    // Returns the outer StackPanel for tracking in m_rows.
    // Lightweight one-line CheckBox row — for boolean settings where the
    // label fully describes the option and a ToggleSwitch's extra chrome
    // (separate "On"/"Off" label + larger track) would just add noise.
    // Confirmations use this since each row is a single yes/no with the
    // action name doing all the explaining.
    static CheckBox MakeCheckRow(
        std::wstring const& label,
        bool initial,
        std::function<void(bool)> onChanged)
    {
        CheckBox box;
        box.Content(box_value(winrt::hstring{ label }));
        box.IsChecked(initial);
        box.FontSize(12);
        box.MinHeight(24);
        box.Padding({ 4, 2, 0, 2 });
        box.Checked([cb = onChanged](
            Windows::Foundation::IInspectable const&,
            RoutedEventArgs const&) { cb(true); });
        box.Unchecked([cb = onChanged](
            Windows::Foundation::IInspectable const&,
            RoutedEventArgs const&) { cb(false); });
        return box;
    }

    static StackPanel MakeToggleRow(
        std::wstring const& label,
        std::wstring const& hint,
        bool initial,
        std::function<void(bool)> onChanged)
    {
        StackPanel row;
        row.Spacing(0);
        row.Padding({ 0, 6, 0, 6 });

        ToggleSwitch toggle;
        toggle.Header(box_value(winrt::hstring{ label }));
        toggle.OnContent(box_value(winrt::hstring{ L"On" }));
        toggle.OffContent(box_value(winrt::hstring{ L"Off" }));
        toggle.IsOn(initial);
        toggle.FontSize(12);
        toggle.MinHeight(28);
        // Toggled fires synchronously on every flip — perfect for
        // instant apply. The captured callback owns the write to Settings.
        toggle.Toggled([cb = std::move(onChanged)](
            Windows::Foundation::IInspectable const& sender,
            RoutedEventArgs const&)
        {
            auto t = sender.try_as<ToggleSwitch>();
            if (t) cb(t.IsOn());
        });
        row.Children().Append(toggle);

        if (!hint.empty())
        {
            TextBlock hintBlock;
            hintBlock.Text(winrt::hstring{ hint });
            if (auto style = Application::Current().Resources().TryLookup(
                    box_value(winrt::hstring{ L"AppSettingsHint" })))
                hintBlock.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            hintBlock.Margin({ 0, 2, 0, 0 });
            row.Children().Append(hintBlock);
        }
        return row;
    }

    // Build a [label + TextBox + hint] vertical block. TextChanged fires the
    // callback with the current text — instant apply. Returns the outer panel.
    static StackPanel MakeTextRow(
        std::wstring const& label,
        std::wstring const& hint,
        std::wstring const& initial,
        std::function<void(std::wstring const&)> onChanged)
    {
        StackPanel row;
        row.Spacing(2);
        row.Padding({ 0, 6, 0, 6 });

        TextBlock head;
        head.Text(winrt::hstring{ label });
        head.FontSize(12);
        head.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        row.Children().Append(head);

        TextBox box;
        box.FontSize(12);
        box.MinHeight(28);
        box.Margin({ 0, 4, 0, 0 });
        box.Width(320);
        box.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Left);
        box.Text(winrt::hstring{ initial });
        box.TextChanged([cb = std::move(onChanged)](
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
        {
            auto t = sender.try_as<TextBox>();
            if (t) cb(std::wstring{ t.Text() });
        });
        row.Children().Append(box);

        if (!hint.empty())
        {
            TextBlock hintBlock;
            hintBlock.Text(winrt::hstring{ hint });
            if (auto style = Application::Current().Resources().TryLookup(
                    box_value(winrt::hstring{ L"AppSettingsHint" })))
                hintBlock.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            hintBlock.Margin({ 0, 2, 0, 0 });
            row.Children().Append(hintBlock);
        }
        return row;
    }

    void AppSettingsPage::BuildSections()
    {
        auto host = formHost();
        auto nav  = navList();
        if (!host || !nav) return;

        auto& s = hyprv::app::settings::Settings::Instance();

        // Helper: add a section header + container to the form AND a
        // matching ListViewItem to the left nav. Returns section index for
        // row attribution. The nav item's Tag holds the section index so
        // OnNavSelectionChanged can scroll to the corresponding header.
        auto addSection = [&](std::wstring const& title) -> size_t
        {
            TextBlock header;
            header.Text(winrt::hstring{ title });
            if (auto style = Application::Current().Resources().TryLookup(
                    box_value(winrt::hstring{ L"AppSettingsSectionHeader" })))
                header.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            // First section: no top margin. Subsequent sections get a
            // generous top margin to visually separate them.
            header.Margin({ 0, m_sections.empty() ? 0.0 : 28.0, 0, 4 });

            StackPanel container;
            container.Spacing(2);

            host.Children().Append(header);
            host.Children().Append(container);

            // Build the left-nav row. Title-cased (the section header in
            // the form is uppercase per the existing density convention,
            // but the nav reads better in title case). Multi-word titles
            // like "REMOTE DESKTOP" → "Remote Desktop": each token after
            // a space gets its initial letter capitalized too.
            std::wstring navText;
            navText.reserve(title.size());
            bool capNext = true;
            for (size_t i = 0; i < title.size(); ++i)
            {
                wchar_t c = title[i];
                if (c == L' ')
                {
                    navText.push_back(c);
                    capNext = true;
                }
                else if (capNext)
                {
                    navText.push_back(static_cast<wchar_t>(std::towupper(c)));
                    capNext = false;
                }
                else
                {
                    navText.push_back(static_cast<wchar_t>(std::towlower(c)));
                }
            }
            ListViewItem navItem;
            navItem.Content(box_value(winrt::hstring{ navText }));
            navItem.FontSize(12);
            // Index into m_sections — set AFTER the section is pushed
            // below, so just reserve the value here.
            navItem.Tag(box_value(static_cast<int32_t>(m_sections.size())));
            nav.Items().Append(navItem);

            m_sections.push_back({ header, container, navItem });
            return m_sections.size() - 1;
        };

        // Helper: append a row into the given section and register for
        // filtering.
        auto addRow = [&](size_t sectionIdx,
                          Microsoft::UI::Xaml::UIElement const& el,
                          std::wstring const& kw)
        {
            m_sections[sectionIdx].container.Children().Append(el);
            SettingRow r;
            r.element       = el;
            r.keywords      = ToLower(kw);
            r.sectionIndex  = sectionIdx;
            m_rows.push_back(std::move(r));
        };

        // ---- GENERAL ----
        // Rail / flyout visibility is already auto-persisted whenever the
        // user toggles those panels from the title-bar buttons (see
        // PersistGeometry), so explicit toggles here would be redundant.
        // Only settings that aren't a direct mirror of an existing UI
        // state belong in this section.
        size_t generalIdx = addSection(L"GENERAL");
        {
            auto restoreRow = MakeToggleRow(
                L"Restore open tabs on launch",
                L"Reopen the tabs that were active when hyprv was last closed.",
                !s.OpenTabs().empty(),
                [](bool on) {
                    // OFF -> clear the persisted snapshot so next launch is
                    // fresh. ON -> harmless no-op now; PersistOpenTabs will
                    // re-snapshot on the next tab change anyway.
                    if (!on) hyprv::app::settings::Settings::Instance().SetOpenTabs({}, -1);
                });
            addRow(generalIdx, restoreRow,
                L"restore open tabs launch reopen session resume");

            auto keepHomeRow = MakeToggleRow(
                L"Keep the home tab open when opening a session",
                L"Opening a VM or remote host from the home screen opens it in a "
                L"new tab and leaves the home tab open, instead of replacing it.",
                s.KeepHomeTabOpen(),
                [](bool on) {
                    hyprv::app::settings::Settings::Instance().SetKeepHomeTabOpen(on);
                });
            addRow(generalIdx, keepHomeRow,
                L"home tab open vm remote host session replace welcome keep new tab");
        }

        // ---- APPEARANCE ----
        size_t appearanceIdx = addSection(L"APPEARANCE");
        {
            using App = hyprv::app::settings::Appearance;
            auto curr = s.AppearancePref();
            auto weakWin = m_mainWindow;
            auto applyNow = [weakWin] {
                if (auto win = weakWin.get()) win->ApplyAppearance();
            };

            // Backdrop dropdown. Order matches the enum so SelectedIndex
            // maps to enum value directly. "None" was offered briefly but
            // removed — clearing SystemBackdrop showed the un-themed root
            // brush through, looked broken especially in Light mode.
            // The lambda also toggles the acrylic intensity slider's
            // visibility — it's only meaningful when Acrylic is the
            // chosen backdrop.
            auto weakSelf = winrt::make_weak<winrt::hyprv_app::AppSettingsPage>(*this);
            // Every appearance change arms the restart banner — even
            // changes that we apply live (backdrop, theme cascade, tint
            // opacity) have residual visuals that only fully clear on
            // relaunch (existing dialogs, frozen StaticResource
            // lookups, etc). Once armed, the banner stays until restart.
            auto armRestart = [weakSelf]() {
                if (auto self = weakSelf.get())
                {
                    auto impl = winrt::get_self<AppSettingsPage>(self);
                    impl->SetRestartRequired(true);
                }
            };
            auto backdropRow = MakeComboRow(
                L"Window backdrop",
                L"Mica surfaces are subtly tinted with the user's desktop background "
                L"color. Acrylic is a semi-transparent material that replicates the "
                L"effect of frosted glass.",
                { L"Mica", L"Acrylic" },
                static_cast<int>(curr.backdrop),
                [applyNow, weakSelf, armRestart](int idx) {
                    auto& ss = hyprv::app::settings::Settings::Instance();
                    auto a = ss.AppearancePref();
                    a.backdrop = static_cast<App::Backdrop>(idx);
                    ss.SetAppearance(a);
                    applyNow();
                    armRestart();
                    // ApplyFilter re-runs the filter + the
                    // acrylic-slider visibility gate (which reads the
                    // freshly-set Settings.appearance.backdrop). One code
                    // path for show/hide instead of duplicating logic.
                    if (auto self = weakSelf.get())
                    {
                        auto impl = winrt::get_self<AppSettingsPage>(self);
                        impl->ApplyFilter();
                    }
                });
            addRow(appearanceIdx, backdropRow,
                L"backdrop mica acrylic blur background window chrome");

            // Intensity sliders — exactly one is visible at a time,
            // chosen by ApplyFilter's slider gate based on the current
            // backdrop. ValueChanged calls UpdateBackdropTintOpacity
            // directly on the MainWindow instead of going through the
            // full ApplyAppearance (which would tear down + rebuild
            // the controller per tick and stutter the drag). The
            // persist still happens via SetAppearance — that's cheap.
            auto applyTintOnly = [weakWin](double opacity) {
                if (auto win = weakWin.get())
                    win->UpdateBackdropTintOpacity(opacity);
            };
            auto acrylicSlider = MakeSliderRow(
                L"Acrylic intensity",
                L"How opaque the acrylic tint is. Lower = more wallpaper visible, "
                L"higher = more solid tint color.",
                0, 100,
                static_cast<int>(curr.acrylicTintOpacity * 100.0),
                [applyTintOnly, armRestart](int value) {
                    auto& ss = hyprv::app::settings::Settings::Instance();
                    auto a = ss.AppearancePref();
                    a.acrylicTintOpacity = value / 100.0;
                    ss.SetAppearance(a);
                    applyTintOnly(a.acrylicTintOpacity);
                    armRestart();
                });
            if (curr.backdrop != App::Backdrop::Acrylic)
                acrylicSlider.Visibility(Visibility::Collapsed);
            m_acrylicSliderRow = acrylicSlider;
            addRow(appearanceIdx, acrylicSlider,
                L"acrylic intensity tint opacity transparency strength");

            auto micaSlider = MakeSliderRow(
                L"Mica intensity",
                L"How strongly the Mica tint is applied. Lower = more wallpaper "
                L"color visible, higher = more theme-tinted.",
                0, 100,
                static_cast<int>(curr.micaTintOpacity * 100.0),
                [applyTintOnly, armRestart](int value) {
                    auto& ss = hyprv::app::settings::Settings::Instance();
                    auto a = ss.AppearancePref();
                    a.micaTintOpacity = value / 100.0;
                    ss.SetAppearance(a);
                    applyTintOnly(a.micaTintOpacity);
                    armRestart();
                });
            if (curr.backdrop != App::Backdrop::Mica)
                micaSlider.Visibility(Visibility::Collapsed);
            m_micaSliderRow = micaSlider;
            addRow(appearanceIdx, micaSlider,
                L"mica intensity tint opacity wallpaper strength");

            auto themeRow = MakeComboRow(
                L"Theme",
                L"System follows the Windows app-mode setting. Black is a "
                L"true-dark variant.",
                { L"System", L"Light", L"Dark", L"Black" },
                static_cast<int>(curr.theme),
                [applyNow, armRestart](int idx) {
                    auto& ss = hyprv::app::settings::Settings::Instance();
                    auto a = ss.AppearancePref();
                    a.theme = static_cast<App::Theme>(idx);
                    ss.SetAppearance(a);
                    applyNow();
                    armRestart();
                });
            addRow(appearanceIdx, themeRow,
                L"theme light dark black system color mode brightness oled");
        }

        // ---- CONFIRMATIONS ----
        size_t confirmIdx = addSection(L"CONFIRMATIONS");
        {
            // Intro paragraph as its own row — also filterable so a search
            // for "confirm" still surfaces the section context.
            TextBlock intro;
            intro.Text(L"Show a confirmation dialog before each of these "
                       L"actions.");
            if (auto style = Application::Current().Resources().TryLookup(
                    box_value(winrt::hstring{ L"AppSettingsHint" })))
                intro.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            // Override the AppSettingsHint default 11pt → 12pt for the
            // intro paragraph (slightly larger reads as a section lead-in).
            intro.FontSize(12);
            intro.Margin({ 0, 0, 0, 8 });
            addRow(confirmIdx, intro, L"confirmations dialogs prompts default");

            for (auto const& e : kConfirmEntries)
            {
                std::wstring keyStr{ e.key };
                bool enabled = s.ConfirmationEnabled(keyStr);
                auto row = MakeCheckRow(
                    e.label,
                    enabled,
                    [keyStr](bool on) {
                        hyprv::app::settings::Settings::Instance()
                            .SetConfirmationEnabled(keyStr, on);
                    });
                std::wstring kw = std::wstring{ e.label } + L" " +
                                  std::wstring{ e.key }   + L" confirm dialog prompt";
                addRow(confirmIdx, row, kw);
            }
        }

        // ---- REMOTE DESKTOP ----
        // Defaults used when a VM doesn't have its own override (set via
        // the VM settings dialog's Remote Desktop section). Instant-apply:
        // each control writes through to Settings::SetRdpDefaults immediately
        // so the next VM connect picks up the new values. Live RDP sessions
        // are NOT reconnected automatically — the user would see the change
        // after restarting the session via the context menu's enhanced
        // toggle or by close + reopen.
        size_t rdpIdx = addSection(L"REMOTE DESKTOP");
        {
            using RdpOpts = hyprv::app::settings::RdpOptions;
            auto rdp = s.RdpDefaults();

            // Intro paragraph. Same shape as the Confirmations intro —
            // 12pt, hint-style colour, registered as a filterable row so
            // a search for "rdp" / "remote desktop" still surfaces the
            // section context. MaxWidth caps the wrap so the paragraph
            // doesn't sprawl edge-to-edge across the content column on
            // a wide window; HorizontalAlignment=Left anchors it to the
            // form's left edge instead of centering inside the cap.
            TextBlock intro;
            intro.Text(L"Defaults for new Remote Desktop sessions. "
                       L"Per-session overrides live in each session's "
                       L"own settings. Changes here apply the next time "
                       L"you connect.");
            if (auto style = Application::Current().Resources().TryLookup(
                    box_value(winrt::hstring{ L"AppSettingsHint" })))
                intro.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            intro.FontSize(12);
            intro.MaxWidth(560);
            intro.HorizontalAlignment(
                Microsoft::UI::Xaml::HorizontalAlignment::Left);
            intro.Margin({ 0, 0, 0, 8 });
            addRow(rdpIdx, intro,
                L"remote desktop rdp defaults per session");

            // Audio mode — Settings::RdpOptions::AudioMode values are
            // 0/1/2 in the same order as the dropdown items, so SelectedIndex
            // maps directly via static_cast. Keep the order in lock-step.
            auto audioRow = MakeComboRow(
                L"Audio playback",
                L"Where the VM's audio plays.",
                { L"Play on this computer",
                  L"Play on the VM",
                  L"Mute (no audio)" },
                static_cast<int>(rdp.audioMode),
                [](int idx) {
                    auto& ss = hyprv::app::settings::Settings::Instance();
                    auto r = ss.RdpDefaults();
                    r.audioMode = static_cast<RdpOpts::AudioMode>(idx);
                    ss.SetRdpDefaults(r);
                });
            addRow(rdpIdx, audioRow,
                L"audio sound speaker rdp playback redirect mute default");

            // Audio capture (microphone). Separate from playback mode —
            // even when audio is "play on the VM", the user may want to
            // send their mic into the guest.
            auto audioCapRow = MakeCheckRow(
                L"Send microphone audio to the VM",
                rdp.audioCaptureRedirect,
                [](bool on) {
                    auto& ss = hyprv::app::settings::Settings::Instance();
                    auto r = ss.RdpDefaults();
                    r.audioCaptureRedirect = on;
                    ss.SetRdpDefaults(r);
                });
            addRow(rdpIdx, audioCapRow,
                L"audio microphone mic capture record voice rdp default");

            // Redirection checkboxes — each maps to one hyprv::ipc::Flag_*
            // bit at StartConnection time. Per-item helper to keep the
            // boilerplate low.
            auto addFlag = [&](wchar_t const* label,
                               wchar_t const* keywords,
                               bool initial,
                               std::function<void(RdpOpts&, bool)> apply)
            {
                auto row = MakeCheckRow(label, initial,
                    [apply = std::move(apply)](bool on) {
                        auto& ss = hyprv::app::settings::Settings::Instance();
                        auto r = ss.RdpDefaults();
                        apply(r, on);
                        ss.SetRdpDefaults(r);
                    });
                addRow(rdpIdx, row, keywords);
            };
            addFlag(L"Share clipboard with the VM",
                L"clipboard copy paste share rdp default",
                rdp.redirectClipboard,
                [](RdpOpts& r, bool on) { r.redirectClipboard = on; });
            addFlag(L"Share local drives with the VM",
                L"drives storage disks share files redirect rdp default",
                rdp.redirectDrives,
                [](RdpOpts& r, bool on) { r.redirectDrives = on; });
            addFlag(L"Share USB and other Plug-and-Play devices",
                L"devices usb plug play pnp share redirect rdp default",
                rdp.redirectDevices,
                [](RdpOpts& r, bool on) { r.redirectDevices = on; });
            addFlag(L"Share smart cards",
                L"smart card smartcard credential auth share rdp default",
                rdp.redirectSmartCards,
                [](RdpOpts& r, bool on) { r.redirectSmartCards = on; });
            addFlag(L"Share serial / parallel ports",
                L"ports serial parallel com lpt share redirect rdp default",
                rdp.redirectPorts,
                [](RdpOpts& r, bool on) { r.redirectPorts = on; });

            // Color depth — 16 / 24 / 32 bpp. Items index 0..2 map to
            // 16 / 24 / 32 via the lookup table below.
            constexpr uint16_t kDepths[] = { 16, 24, 32 };
            int depthIdx = 2; // default to 32
            for (int i = 0; i < 3; ++i)
                if (kDepths[i] == rdp.colorDepth) depthIdx = i;
            auto depthRow = MakeComboRow(
                L"Color depth",
                L"Higher uses more bandwidth; modern Windows guests handle 32 bpp fine.",
                { L"16 bpp", L"24 bpp", L"32 bpp" },
                depthIdx,
                [](int idx) {
                    constexpr uint16_t depths[] = { 16, 24, 32 };
                    if (idx < 0 || idx > 2) return;
                    auto& ss = hyprv::app::settings::Settings::Instance();
                    auto r = ss.RdpDefaults();
                    r.colorDepth = depths[idx];
                    ss.SetRdpDefaults(r);
                });
            addRow(rdpIdx, depthRow,
                L"color depth bpp 16 24 32 quality rdp default");

            // Display scale override — pins the guest's render scale
            // independent of the host monitor. Index 0 = Auto (follow host
            // DPI); 1..5 = 100/125/150/175/200 %. Applies to all sessions.
            constexpr uint16_t kScales[] = { 0, 100, 125, 150, 175, 200 };
            int scaleIdx = 0;
            for (int i = 0; i < 6; ++i)
                if (kScales[i] == rdp.dpiScaleOverridePercent) scaleIdx = i;
            auto scaleRow = MakeComboRow(
                L"Display scale",
                L"Text and UI scale inside the guest. Auto matches this PC's display.",
                { L"Auto", L"100%", L"125%", L"150%", L"175%", L"200%" },
                scaleIdx,
                [](int idx) {
                    constexpr uint16_t scales[] = { 0, 100, 125, 150, 175, 200 };
                    if (idx < 0 || idx > 5) return;
                    auto& ss = hyprv::app::settings::Settings::Instance();
                    auto r = ss.RdpDefaults();
                    r.dpiScaleOverridePercent = scales[idx];
                    ss.SetRdpDefaults(r);
                });
            addRow(rdpIdx, scaleRow,
                L"dpi display scale zoom text size scaling resolution rdp default");

            // Initial size — starting resolution for BASIC sessions only
            // (enhanced sessions resize to fit the tab window). Preset list;
            // an odd hand-edited size falls back to the 1024x768 selection.
            constexpr uint16_t kResW[] = { 800, 1024, 1280, 1280, 1366, 1600, 1920, 2560 };
            constexpr uint16_t kResH[] = { 600,  768,  720, 1024,  768,  900, 1080, 1440 };
            int resIdx = 1; // default 1024x768
            for (int i = 0; i < 8; ++i)
                if (kResW[i] == rdp.initialDesktopWidth &&
                    kResH[i] == rdp.initialDesktopHeight) resIdx = i;
            auto resRow = MakeComboRow(
                L"Initial size (basic sessions)",
                L"Starting resolution for basic-session guests. Enhanced sessions fit the window.",
                { L"800 x 600", L"1024 x 768", L"1280 x 720",
                  L"1280 x 1024", L"1366 x 768", L"1600 x 900",
                  L"1920 x 1080", L"2560 x 1440" },
                resIdx,
                [](int idx) {
                    constexpr uint16_t w[] = { 800, 1024, 1280, 1280, 1366, 1600, 1920, 2560 };
                    constexpr uint16_t h[] = { 600,  768,  720, 1024,  768,  900, 1080, 1440 };
                    if (idx < 0 || idx > 7) return;
                    auto& ss = hyprv::app::settings::Settings::Instance();
                    auto r = ss.RdpDefaults();
                    r.initialDesktopWidth  = w[idx];
                    r.initialDesktopHeight = h[idx];
                    ss.SetRdpDefaults(r);
                });
            addRow(rdpIdx, resRow,
                L"resolution initial size width height basic session rdp default");
        }

        // ---- LOGGING ----
        size_t loggingIdx = addSection(L"LOGGING");
        {
            auto logEnabledRow = MakeToggleRow(
                L"Write log files",
                L"Captures app activity and rdphost output. Useful when reporting bugs.",
                s.LoggingEnabled(),
                [](bool on) {
                    hyprv::app::settings::Settings::Instance().SetLoggingEnabled(on);
                });
            addRow(loggingIdx, logEnabledRow,
                L"log logging diagnostics hyprv.log file output debug");

            // Log path display + two action buttons. Built inline (not as
            // a MakeToggleRow) because the row shape differs.
            StackPanel pathRow;
            pathRow.Spacing(2);
            pathRow.Padding({ 0, 6, 0, 6 });

            TextBlock pathLabel;
            pathLabel.Text(L"Log file");
            pathLabel.FontSize(12);
            pathLabel.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            pathRow.Children().Append(pathLabel);

            auto logPath = s.FilePath().parent_path() / L"hyprv.log";
            TextBlock pathText;
            pathText.Text(winrt::hstring{ logPath.wstring() });
            if (auto style = Application::Current().Resources().TryLookup(
                    box_value(winrt::hstring{ L"AppSettingsMonoPath" })))
                pathText.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            pathRow.Children().Append(pathText);

            StackPanel buttonRow;
            buttonRow.Orientation(Orientation::Horizontal);
            buttonRow.Spacing(8);
            buttonRow.Margin({ 0, 6, 0, 0 });

            Button openLog;
            openLog.Content(box_value(winrt::hstring{ L"Open log file" }));
            openLog.FontSize(12);
            openLog.MinHeight(28);
            openLog.Click([](Windows::Foundation::IInspectable const&,
                             RoutedEventArgs const&) {
                auto p = hyprv::app::settings::Settings::Instance().FilePath()
                             .parent_path() / L"hyprv.log";
                ShellExecuteW(nullptr, L"open", p.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            });
            buttonRow.Children().Append(openLog);

            Button openFolder;
            openFolder.Content(box_value(winrt::hstring{ L"Open settings folder" }));
            openFolder.FontSize(12);
            openFolder.MinHeight(28);
            openFolder.Click([](Windows::Foundation::IInspectable const&,
                                RoutedEventArgs const&) {
                auto p = hyprv::app::settings::Settings::Instance().FilePath()
                             .parent_path();
                ShellExecuteW(nullptr, L"open", p.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            });
            buttonRow.Children().Append(openFolder);

            pathRow.Children().Append(buttonRow);

            addRow(loggingIdx, pathRow,
                L"log path open folder explorer file location appdata");
        }

        // ---- DEBUGGER ----
        size_t debuggerIdx = addSection(L"DEBUGGER");
        {
            TextBlock intro;
            intro.Text(L"Launch an external debugger for a VM. When enabled, a "
                "debugger button appears in the title bar; it's active for any "
                "VM whose debugger arguments are set in that VM's settings. "
                "hyprv just runs the configured command — you supply the "
                "transport (e.g. -k net:port=...,key=... or -k com:pipe,...).");
            intro.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            intro.MaxWidth(560);
            intro.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Left);
            if (auto style = Application::Current().Resources().TryLookup(
                    box_value(winrt::hstring{ L"AppSettingsHint" })))
                intro.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            intro.Margin({ 0, 2, 0, 4 });
            addRow(debuggerIdx, intro,
                L"debugger windbg kernel bug launch attach");

            auto enableRow = MakeToggleRow(
                L"Enable VM debugger",
                L"Shows the title-bar debugger button for VMs with debugger "
                "arguments configured.",
                s.DebuggerEnabled(),
                [](bool on) {
                    hyprv::app::settings::Settings::Instance().SetDebuggerEnabled(on);
                });
            addRow(debuggerIdx, enableRow,
                L"debugger enable windbg launch bug kernel attach toggle");

            auto exeRow = MakeTextRow(
                L"Debugger",
                L"The debugger executable to launch (default windbgx). A VM can "
                "override this in its own settings.",
                s.DebuggerExe(),
                [](std::wstring const& t) {
                    hyprv::app::settings::Settings::Instance().SetDebuggerExe(t);
                });
            addRow(debuggerIdx, exeRow,
                L"debugger exe executable windbg windbgx path command");
        }

        ApplyFilter();
        // Default-select the first nav item so the highlight isn't blank
        // on open. Programmatic selection — suppress the scroll-to-section
        // side effect so the page opens scrolled to the top.
        if (auto n = navList(); n && !m_sections.empty())
        {
            m_suppressNavSelection = true;
            n.SelectedIndex(0);
            m_suppressNavSelection = false;
        }
    }

    void AppSettingsPage::OnNavSelectionChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (m_suppressNavSelection) return;
        auto list = sender.try_as<Microsoft::UI::Xaml::Controls::ListView>();
        if (!list) return;
        auto item = list.SelectedItem().try_as<
            Microsoft::UI::Xaml::Controls::ListViewItem>();
        if (!item) return;
        int32_t idx = unbox_value_or<int32_t>(item.Tag(), -1);
        if (idx < 0 || static_cast<size_t>(idx) >= m_sections.size()) return;

        // Scroll the right pane to bring the section header to the top
        // (or as close as the bottom of the content allows). The header
        // is a XAML element parented under formHost, which is inside
        // contentScroller — TransformToVisual gives its current position
        // relative to the scroller's viewport; add VerticalOffset to get
        // an absolute scroll target.
        auto header   = m_sections[idx].header;
        auto scroller = contentScroller();
        if (!header || !scroller) return;
        try
        {
            auto transform = header.TransformToVisual(scroller);
            auto pt = transform.TransformPoint(Windows::Foundation::Point{ 0, 0 });
            double target = scroller.VerticalOffset() + pt.Y;
            // Small negative bias so the section header isn't hugging the
            // very top edge of the viewport — matches what feels natural.
            target = (target > 8.0) ? (target - 8.0) : 0.0;
            scroller.ChangeView(nullptr, winrt::box_value(target).as<
                Windows::Foundation::IReference<double>>(), nullptr,
                /* disableAnimation */ false);
        }
        catch (...) { /* swallow — scroll is a nice-to-have */ }
    }

    void AppSettingsPage::OnSearchTextChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
    {
        if (auto box = searchBox())
            m_filter = ToLower(std::wstring{ box.Text() });
        else
            m_filter.clear();
        ApplyFilter();
    }

    void AppSettingsPage::ApplyFilter()
    {
        // Per-row visibility from the keyword match. Empty filter shows
        // everything. Track per-section visibility so empty sections
        // collapse AND their left-nav items disappear so the nav stays
        // honest about what's reachable from the search.
        std::vector<bool> sectionHasVisible(m_sections.size(), false);
        bool anyVisible = false;
        for (auto const& row : m_rows)
        {
            bool match = m_filter.empty()
                      || (row.keywords.find(m_filter) != std::wstring::npos);
            if (auto fe = row.element.try_as<FrameworkElement>())
                fe.Visibility(match ? Visibility::Visible : Visibility::Collapsed);
            if (match)
            {
                sectionHasVisible[row.sectionIndex] = true;
                anyVisible = true;
            }
        }

        // If the currently-selected nav item is about to disappear, we
        // need to clear or re-pick the selection — otherwise the highlight
        // stays on an invisible item. Compute the first visible section
        // up front so we can re-target the selection without firing a
        // scroll side-effect.
        int firstVisibleSection = -1;
        for (size_t i = 0; i < m_sections.size(); ++i)
        {
            if (sectionHasVisible[i]) { firstVisibleSection = static_cast<int>(i); break; }
        }

        for (size_t i = 0; i < m_sections.size(); ++i)
        {
            auto vis = sectionHasVisible[i] ? Visibility::Visible
                                            : Visibility::Collapsed;
            if (m_sections[i].header)    m_sections[i].header.Visibility(vis);
            if (m_sections[i].container) m_sections[i].container.Visibility(vis);
            if (m_sections[i].navItem)   m_sections[i].navItem.Visibility(vis);
        }

        // Re-point selection to the first visible section if the current
        // selection has been hidden. Suppress the scroll so the user
        // doesn't get yanked while they're typing.
        if (auto nav = navList(); nav && !m_sections.empty())
        {
            int selIdx = nav.SelectedIndex();
            bool selectedHidden = (selIdx >= 0
                && static_cast<size_t>(selIdx) < m_sections.size()
                && !sectionHasVisible[selIdx]);
            if (selectedHidden || selIdx < 0)
            {
                m_suppressNavSelection = true;
                nav.SelectedIndex(firstVisibleSection);
                m_suppressNavSelection = false;
            }
        }

        if (auto t = noResultsText())
            t.Visibility((!anyVisible && !m_filter.empty())
                ? Visibility::Visible
                : Visibility::Collapsed);

        // Slider visibility gates. The filter pass above forces every
        // m_rows entry Visible when the search box is empty; the two
        // intensity sliders are also conditional on the current
        // backdrop, so a second pass re-collapses whichever doesn't
        // apply. Section-header visibility was already decided based on
        // rows that could in principle match — collapsing a slider here
        // doesn't ripple back into the section header gate, which is fine.
        auto backdrop = hyprv::app::settings::Settings::Instance()
                          .AppearancePref().backdrop;
        auto gateSlider = [&](Microsoft::UI::Xaml::UIElement const& el,
                              bool wantVisible)
        {
            if (!el) return;
            if (auto fe = el.try_as<Microsoft::UI::Xaml::FrameworkElement>())
            {
                if (!wantVisible && fe.Visibility() == Visibility::Visible)
                    fe.Visibility(Visibility::Collapsed);
                else if (wantVisible && fe.Visibility() == Visibility::Collapsed)
                {
                    // Search filter would also need to allow it. Re-check.
                    bool searchAllows = true;
                    if (!m_filter.empty())
                    {
                        searchAllows = false;
                        for (auto const& row : m_rows)
                        {
                            if (row.element == el)
                            {
                                searchAllows = row.keywords.find(m_filter)
                                            != std::wstring::npos;
                                break;
                            }
                        }
                    }
                    if (searchAllows) fe.Visibility(Visibility::Visible);
                }
            }
        };
        gateSlider(m_acrylicSliderRow,
            backdrop == hyprv::app::settings::Appearance::Backdrop::Acrylic);
        gateSlider(m_micaSliderRow,
            backdrop == hyprv::app::settings::Appearance::Backdrop::Mica);
    }

    void AppSettingsPage::SetRestartRequired(bool required)
    {
        if (m_restartRequired == required) return;
        m_restartRequired = required;
        if (auto bar = restartBanner())
            bar.IsOpen(required);
    }

    void AppSettingsPage::OnRestartNowClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Flush any pending Settings writes synchronously so the new
        // instance reads the same values the user just set. The
        // debounced save thread coalesces writes every ~500 ms; without
        // SaveNow the relaunch could land before the last batch flushes.
        hyprv::app::settings::Settings::Instance().SaveNow();

        // Spawn a fresh hyprv.exe from the same path as the current
        // process. CreateProcess returns immediately; the new process
        // waits for OUR process to exit before its window appears
        // gracefully (well, in practice both windows overlap briefly).
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
        {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(exePath, nullptr, nullptr, nullptr,
                               FALSE, 0, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }

        // Tear down the current app. Application::Exit closes every
        // window in this process — including the rdphost children via
        // their job-object linkage in main.cpp — so no orphans.
        Microsoft::UI::Xaml::Application::Current().Exit();
    }

    // (OnPageTapped removed — it was stealing focus on every page tap,
    // including taps inside a ComboBox, which made the dropdown impossible
    // to open via the chevron. Search-box focus is left to default
    // dismissal: tab away, click another control, or hit Escape.)
}
