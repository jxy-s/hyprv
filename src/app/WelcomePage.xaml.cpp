#include "pch.h"
#include "WelcomePage.xaml.h"
#if __has_include("WelcomePage.g.cpp")
#include "WelcomePage.g.cpp"
#endif

#include "MainWindow.xaml.h"
#include "settings/Settings.h"
#include "vm/VMManager.h"
#include "vm/VirtualMachine.h"
#include "ui/VmTileFactory.h"

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>          // PointerPoint::Properties()
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>             // FontWeights::SemiBold()

#include <algorithm>
#include <chrono>
#include <unordered_map>

extern void HyprvAppLog(const wchar_t* fmt, ...);

// Match MainWindow.xaml.cpp's using set so the moved code can use bare
// Microsoft::... in this TU.
using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace
{
    using winrt::Microsoft::UI::Xaml::HorizontalAlignment;
    using winrt::Microsoft::UI::Xaml::VerticalAlignment;

    // Brush lookup via Application.Current().Resources() — every value we
    // need is a theme resource; centralising the lookup keeps the row /
    // pill builders readable.
    Media::Brush ThemeBrush(wchar_t const* key)
    {
        return Application::Current().Resources().Lookup(
            winrt::box_value(winrt::hstring{ key })).as<Media::Brush>();
    }

    std::wstring StateLabel(hyprv::app::vm::VmState s)
    {
        using S = hyprv::app::vm::VmState;
        switch (s)
        {
            case S::Running:    return L"Running";
            case S::Off:        return L"Off";
            case S::Stopping:   return L"Stopping";
            case S::Saved:      return L"Saved";
            case S::Paused:     return L"Paused";
            case S::Starting:   return L"Starting";
            case S::Reset:      return L"Resetting";
            case S::Saving:     return L"Saving";
            case S::Pausing:    return L"Pausing";
            case S::Resuming:   return L"Resuming";
            case S::FastSaving: return L"Saving (fast)";
            case S::FastSaved:  return L"Saved (fast)";
            case S::Hibernated: return L"Hibernated";
            default:            return L"Unknown";
        }
    }

    // "-" for missing data so empty cells don't render as 0 (which would
    // imply "actually zero" rather than "no value").
    std::wstring FormatMemoryMb(uint64_t mb)
    {
        if (mb == 0) return L"-";
        wchar_t buf[32];
        if (mb >= 1024)
            swprintf_s(buf, L"%.2f GB", mb / 1024.0);
        else
            swprintf_s(buf, L"%llu MB", static_cast<unsigned long long>(mb));
        return buf;
    }
    std::wstring FormatCpuPct(uint16_t pct, hyprv::app::vm::VmState s)
    {
        if (s != hyprv::app::vm::VmState::Running) return L"-";
        wchar_t buf[16]; swprintf_s(buf, L"%u%%", pct);
        return buf;
    }
    std::wstring FormatUptime(uint64_t ms)
    {
        if (ms == 0) return L"-";
        uint64_t s = ms / 1000;
        uint64_t d = s / 86400; s %= 86400;
        uint64_t h = s / 3600;  s %= 3600;
        uint64_t m = s / 60;    uint64_t sec = s % 60;
        wchar_t buf[32];
        if (d > 0)
            swprintf_s(buf, L"%llud %lluh", (unsigned long long)d, (unsigned long long)h);
        else if (h > 0)
            swprintf_s(buf, L"%lluh %llum", (unsigned long long)h, (unsigned long long)m);
        else
            swprintf_s(buf, L"%llum %llus", (unsigned long long)m, (unsigned long long)sec);
        return buf;
    }

    // Single-line compact pill for the Recents row: dot + name. Click
    // opens (and replaces welcome). Right-click = standard VM menu.
    Controls::Border BuildRecentPill(
        hyprv::app::vm::VirtualMachine const& vm,
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::function<void()> onOpen)
    {
        Controls::StackPanel row;
        row.Orientation(Controls::Orientation::Horizontal);
        row.Spacing(6);
        row.VerticalAlignment(VerticalAlignment::Center);

        Shapes::Ellipse dot;
        dot.Width(6); dot.Height(6);
        dot.VerticalAlignment(VerticalAlignment::Center);
        hyprv::app::ui::ApplyVmDotState(dot, vm);
        row.Children().Append(dot);

        Controls::TextBlock name;
        auto displayName = vm.elementName.empty() ? std::wstring{ L"<no name>" }
                                                  : vm.elementName;
        name.Text(winrt::hstring{ displayName });
        name.FontSize(12);
        name.VerticalAlignment(VerticalAlignment::Center);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        row.Children().Append(name);

        Controls::Border pill;
        // Style holds the theme-aware Background + BorderBrush + corner
        // radius. Padding is per-pill so each row builder can tweak; the
        // remaining setters live in the App.xaml "Pill" Style.
        if (auto st = Application::Current().Resources().TryLookup(
                winrt::box_value(winrt::hstring{ L"Pill" })))
            pill.Style(st.try_as<Microsoft::UI::Xaml::Style>());
        pill.Padding({ 8, 3, 10, 3 });
        pill.Child(row);

        // Hover affordance — Recents are explicit "quick access" entries
        // (different from the main All-VMs table which uses double-click
        // + selection). Hover brightens the pill background. On exit,
        // ClearValue lets the Style's theme-aware brush come back —
        // re-setting the brush via ThemeBrush would freeze it again.
        pill.PointerEntered([](winrt::Windows::Foundation::IInspectable const& s,
                               Input::PointerRoutedEventArgs const&)
        {
            if (auto b = s.try_as<Controls::Border>())
                b.Background(ThemeBrush(L"SubtleFillColorTertiaryBrush"));
        });
        pill.PointerExited([](winrt::Windows::Foundation::IInspectable const& s,
                              Input::PointerRoutedEventArgs const&)
        {
            if (auto b = s.try_as<Controls::Border>())
                b.ClearValue(Controls::Border::BackgroundProperty());
        });
        // Single-click opens — Recents are quick-access shortcuts and
        // don't need the "select then double-click" gesture that the
        // main table uses to prevent accidental opens while scanning.
        pill.Tapped([onOpen](winrt::Windows::Foundation::IInspectable const&,
                             Input::TappedRoutedEventArgs const& e)
        {
            e.Handled(true);
            onOpen();
        });
        pill.ContextFlyout(hyprv::app::ui::BuildVmContextMenu(
            winrt::hstring{ vm.guid }, winrt::hstring{ vm.elementName },
            weakWindow));
        return pill;
    }

    // In-place dot refresh for an existing Recents pill (built above) — same
    // motivation as UpdateVmRow: don't recreate the pill each poll (keeps the
    // ContextFlyout + dot blink alive). Pill child = StackPanel [0]=dot, [1]=name.
    void UpdateRecentPill(Controls::Border const& pill,
                          hyprv::app::vm::VirtualMachine const& vm)
    {
        auto row = pill ? pill.Child().try_as<Controls::StackPanel>() : nullptr;
        if (!row || row.Children().Size() < 1) return;
        if (auto dot = row.Children().GetAt(0).try_as<Shapes::Ellipse>())
            hyprv::app::ui::ApplyVmDotState(dot, vm);
    }

    // Compact pill for a saved Remote Host — a Remote glyph + display name in
    // the same "Pill" chrome the recent-VM pills use. Click connects (opens /
    // focuses the host tab); right-click = Connect / Edit / Forget. Shared by
    // the welcome "Remote Hosts" section AND the RECENT row (remote entries).
    Controls::Border BuildRemoteHostPill(
        std::wstring const& display, winrt::hstring const& addrH,
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::function<void()> onOpen)
    {
        Controls::StackPanel row;
        row.Orientation(Controls::Orientation::Horizontal);
        row.Spacing(6);
        row.VerticalAlignment(VerticalAlignment::Center);

        Controls::FontIcon glyph;
        glyph.FontFamily(Media::FontFamily{ L"Segoe MDL2 Assets" });
        glyph.Glyph(L"\xE8AF");   // Remote
        glyph.FontSize(11);
        glyph.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(glyph);

        Controls::TextBlock name;
        name.Text(winrt::hstring{ display });
        name.FontSize(12);
        name.VerticalAlignment(VerticalAlignment::Center);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        row.Children().Append(name);

        Controls::Border pill;
        if (auto st = Application::Current().Resources().TryLookup(
                winrt::box_value(winrt::hstring{ L"Pill" })))
            pill.Style(st.try_as<Microsoft::UI::Xaml::Style>());
        pill.Padding({ 8, 3, 10, 3 });
        pill.Child(row);

        pill.PointerEntered([](winrt::Windows::Foundation::IInspectable const& s,
                               Input::PointerRoutedEventArgs const&)
        {
            if (auto b = s.try_as<Controls::Border>())
                b.Background(ThemeBrush(L"SubtleFillColorTertiaryBrush"));
        });
        pill.PointerExited([](winrt::Windows::Foundation::IInspectable const& s,
                              Input::PointerRoutedEventArgs const&)
        {
            if (auto b = s.try_as<Controls::Border>())
                b.ClearValue(Controls::Border::BackgroundProperty());
        });
        pill.Tapped([onOpen](winrt::Windows::Foundation::IInspectable const&,
                             Input::TappedRoutedEventArgs const& e)
        {
            e.Handled(true);
            if (onOpen) onOpen();   // honors "Keep home tab open" (WelcomePage::OpenRemoteHost)
        });
        pill.ContextFlyout(hyprv::app::ui::BuildRemoteHostContextMenu(addrH, weakWindow));
        return pill;
    }

    // Tabular row for a saved Remote Host (the welcome "Remote Hosts" section),
    // styled like the ALL VMs rows. Columns MUST match remoteHostsHeader in
    // WelcomePage.xaml: [16 glyph][Name *][Address *]. The whole row is a
    // transparent-background Border so the ENTIRE width is hit-testable (a null
    // background only hits where content is). DOUBLE-click connects (single
    // click is a no-op, matching the ALL VMs select/open split); right-click =
    // the host menu; hover highlights. Single click = select (onSelect); the
    // page paints the persistent selection via ApplyRemoteRowSelection.
    Controls::Border BuildRemoteHostRow(
        std::wstring const& display, std::wstring const& address,
        winrt::hstring const& addrH,
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::function<void()> onSelect, std::function<void()> onOpen)
    {
        Controls::Grid row;
        row.MinHeight(26);
        auto addCol = [&](double px, bool star, double minw) {
            Controls::ColumnDefinition cd;
            cd.Width(star ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star)
                          : GridLengthHelper::FromValueAndType(px, GridUnitType::Pixel));
            if (minw > 0) cd.MinWidth(minw);
            row.ColumnDefinitions().Append(cd);
        };
        addCol(16,  false, 0);    // glyph
        addCol(0,   true, 180);   // name (stretches)
        addCol(320, false, 0);    // address (fixed, right cluster — see header)

        Controls::FontIcon glyph;
        glyph.FontFamily(Media::FontFamily{ L"Segoe MDL2 Assets" });
        glyph.Glyph(L"\xE8AF");   // Remote
        glyph.FontSize(12);
        glyph.VerticalAlignment(VerticalAlignment::Center);
        Controls::Grid::SetColumn(glyph, 0);
        row.Children().Append(glyph);

        Controls::TextBlock name;
        name.Text(winrt::hstring{ display });
        name.FontSize(12);
        name.VerticalAlignment(VerticalAlignment::Center);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        Controls::Grid::SetColumn(name, 1);
        row.Children().Append(name);

        Controls::TextBlock addr;
        addr.Text(winrt::hstring{ address });
        addr.FontSize(12);
        addr.VerticalAlignment(VerticalAlignment::Center);
        // Right-aligned within the fixed column, mirroring the ALL VMs
        // non-name columns + the right-aligned header above.
        addr.HorizontalAlignment(HorizontalAlignment::Right);
        addr.Margin({ 0, 0, 8, 0 });
        addr.TextTrimming(TextTrimming::CharacterEllipsis);
        // Dimmed, theme-reactive (Style, not a frozen ThemeBrush — see BuildVmRow).
        if (auto style = Application::Current().Resources().TryLookup(
                winrt::box_value(winrt::hstring{ L"SecondaryText" })))
            addr.Style(style.try_as<Microsoft::UI::Xaml::Style>());
        Controls::Grid::SetColumn(addr, 2);
        row.Children().Append(addr);

        // Whole-row transparent hit target + hover affordance.
        Controls::Border item;
        item.Padding({ 8, 3, 8, 3 });
        item.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 2, 2, 2, 2 });
        item.Background(Media::SolidColorBrush{
            Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0) });   // transparent => full-width hits
        item.Child(row);

        // Hover affordance — skips when the row is selected (the selection
        // background, set by ApplyRemoteRowSelection, wins). The selected flag
        // rides the Border's Tag (a bool), same idiom as BuildVmRow.
        item.PointerEntered([](winrt::Windows::Foundation::IInspectable const& s,
                               Input::PointerRoutedEventArgs const&)
        {
            if (auto b = s.try_as<Controls::Border>())
            {
                auto sel = b.Tag().try_as<bool>();
                if (sel && *sel) return;
                b.Background(ThemeBrush(L"ControlFillColorSecondaryBrush"));
            }
        });
        item.PointerExited([](winrt::Windows::Foundation::IInspectable const& s,
                              Input::PointerRoutedEventArgs const&)
        {
            if (auto b = s.try_as<Controls::Border>())
            {
                auto sel = b.Tag().try_as<bool>();
                if (sel && *sel) return;
                b.Background(Media::SolidColorBrush{
                    Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0) });
            }
        });
        // Single click = select (highlight), like the ALL VMs rows. DOUBLE
        // click connects (a saved host is a deliberate connection).
        item.Tapped([onSelect](winrt::Windows::Foundation::IInspectable const&,
                               Input::TappedRoutedEventArgs const& e)
        {
            e.Handled(true);
            onSelect();
        });
        item.DoubleTapped([onOpen](winrt::Windows::Foundation::IInspectable const&,
                                   Input::DoubleTappedRoutedEventArgs const& e)
        {
            e.Handled(true);
            if (onOpen) onOpen();   // honors "Keep home tab open" (WelcomePage::OpenRemoteHost)
        });
        item.ContextFlyout(hyprv::app::ui::BuildRemoteHostContextMenu(addrH, weakWindow));
        return item;
    }

    // Tabular row — one per VM. Columns mirror the header in WelcomePage.xaml.
    // Click anywhere on the row = select (highlight). Double-click = open.
    // Right-click = VM context menu. The selection visual is driven by
    // RenderAllVms (which knows m_selectedGuid) — this builder just lays
    // out the cells.
    Controls::Border BuildVmRow(
        hyprv::app::vm::VirtualMachine const& vm,
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::function<void()> onOpen,
        std::function<void()> onSelect)
    {
        Controls::Grid row;
        row.MinHeight(26);
        // Column layout MUST match the header Grid in WelcomePage.xaml — any
        // change here needs the matching change there or the header labels
        // drift out of alignment with the data cells. Widths (after the
        // 14 DIP dot column): Name=* (MinWidth 180), Status=100, CPU=60,
        // Memory=100, Uptime=100.
        auto addCol = [&](double px, bool star = false) {
            Controls::ColumnDefinition cd;
            // GridLengthHelper / GridUnitType live in Microsoft::UI::Xaml,
            // NOT in ::Controls. The using-directive on this TU brings them
            // into scope unqualified.
            cd.Width(star
                ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star)
                : GridLengthHelper::FromValueAndType(px, GridUnitType::Pixel));
            if (star) cd.MinWidth(180);
            row.ColumnDefinitions().Append(cd);
        };
        addCol(14);                       // dot
        addCol(0, true);                  // name (stretch, MinWidth 180)
        addCol(100);                      // status
        addCol(60);                       // cpu
        addCol(100);                      // memory
        addCol(100);                      // uptime

        // Dot
        Shapes::Ellipse dot;
        dot.Width(8); dot.Height(8);
        dot.VerticalAlignment(VerticalAlignment::Center);
        hyprv::app::ui::ApplyVmDotState(dot, vm);
        Controls::Grid::SetColumn(dot, 0);
        row.Children().Append(dot);

        auto addText = [&](int col, std::wstring const& text,
                           bool secondary = false, bool rightAlign = false)
        {
            Controls::TextBlock tb;
            tb.Text(winrt::hstring{ text });
            tb.FontSize(12);
            tb.VerticalAlignment(VerticalAlignment::Center);
            tb.TextTrimming(TextTrimming::CharacterEllipsis);
            if (secondary)
            {
                // Style (not ThemeBrush!) so the foreground re-resolves
                // on a runtime RequestedTheme switch. ThemeBrush does a
                // one-shot Resources.Lookup which freezes the brush at
                // construction time, leaving Status / CPU / Memory /
                // Uptime cells unreadable after Dark → Light.
                if (auto style = Application::Current().Resources().TryLookup(
                        winrt::box_value(winrt::hstring{ L"SecondaryText" })))
                    tb.Style(style.try_as<Microsoft::UI::Xaml::Style>());
            }
            if (rightAlign)
            {
                tb.HorizontalAlignment(HorizontalAlignment::Right);
                tb.Margin({ 0, 0, 8, 0 });
            }
            Controls::Grid::SetColumn(tb, col);
            row.Children().Append(tb);
        };

        auto displayName = vm.elementName.empty() ? std::wstring{ L"<no name>" }
                                                  : vm.elementName;
        addText(1, displayName);
        // statusText wins over the stable state label (matches rail/flyout
        // idiom — e.g. "Saving Virtual Machine (35%)" when an async job
        // is in flight).
        std::wstring statusCell = vm.statusText.empty()
            ? StateLabel(vm.state)
            : vm.statusText;
        addText(2, statusCell, /*secondary*/ true);
        addText(3, FormatCpuPct(vm.processorLoadPct, vm.state),
                /*secondary*/ true, /*rightAlign*/ true);
        addText(4, FormatMemoryMb(vm.memoryAssignedMb),
                /*secondary*/ true, /*rightAlign*/ true);
        addText(5, FormatUptime(vm.uptimeMs), /*secondary*/ true);

        // Whole-row click target with subtle hover affordance. Border
        // wraps the Grid so we can flip its background on pointer enter
        // without disturbing the Grid's layout.
        Controls::Border bg;
        bg.Padding({ 0, 3, 0, 3 });
        bg.CornerRadius({ 2, 2, 2, 2 });
        bg.Background(Media::SolidColorBrush{
            Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0) });    // transparent
        bg.Child(row);

        // Hover affordance — only paints while NOT selected. Selection
        // state is owned by the page (m_selectedGuid) and applied via
        // ApplyRowSelection; we read the Tag to decide whether to override
        // the persistent selection background.
        bg.PointerEntered([](winrt::Windows::Foundation::IInspectable const& s,
                             Input::PointerRoutedEventArgs const&)
        {
            if (auto b = s.try_as<Controls::Border>())
            {
                // Tag carries "selected" boolean (set by ApplyRowSelection).
                auto sel = b.Tag().try_as<bool>();
                if (sel && *sel) return;   // selection background wins
                // ControlFillColorSecondaryBrush is meaningfully more
                // opaque than SubtleFillColorSecondaryBrush — visible on
                // light backgrounds where the subtle variant gets lost.
                // Looked up at hover time (not construction time) so
                // theme switches keep working.
                b.Background(ThemeBrush(L"ControlFillColorSecondaryBrush"));
            }
        });
        bg.PointerExited([](winrt::Windows::Foundation::IInspectable const& s,
                            Input::PointerRoutedEventArgs const&)
        {
            if (auto b = s.try_as<Controls::Border>())
            {
                auto sel = b.Tag().try_as<bool>();
                if (sel && *sel) return;
                b.Background(Media::SolidColorBrush{
                    Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0) });
            }
        });
        // Single-click selects (highlights). Double-click opens.
        // Tapped fires for both mouse + touch + keyboard activation —
        // exactly the gesture we want for select.
        bg.Tapped([onSelect](winrt::Windows::Foundation::IInspectable const&,
                             Input::TappedRoutedEventArgs const& e)
        {
            e.Handled(true);
            onSelect();
        });
        bg.DoubleTapped([onOpen](winrt::Windows::Foundation::IInspectable const&,
                                 Input::DoubleTappedRoutedEventArgs const& e)
        {
            e.Handled(true);
            onOpen();
        });
        bg.ContextFlyout(hyprv::app::ui::BuildVmContextMenu(
            winrt::hstring{ vm.guid }, winrt::hstring{ vm.elementName },
            weakWindow));
        return bg;
    }

    // Update just the dynamic cells of an existing row (built by BuildVmRow)
    // WITHOUT recreating it — keeps the row's ContextFlyout alive (so a poll
    // doesn't dismiss an open right-click menu) and the dot's blink Storyboard
    // running smoothly (no per-poll restart). The child order matches
    // BuildVmRow: Grid children [0]=dot, [2]=status, [3]=cpu, [4]=memory,
    // [5]=uptime (index 1 = name, only changes on rename → handled by rebuild).
    void UpdateVmRow(Controls::Border const& bg,
                     hyprv::app::vm::VirtualMachine const& vm)
    {
        auto row = bg ? bg.Child().try_as<Controls::Grid>() : nullptr;
        if (!row) return;
        auto kids = row.Children();
        if (kids.Size() < 6) return;
        if (auto dot = kids.GetAt(0).try_as<Shapes::Ellipse>())
            hyprv::app::ui::ApplyVmDotState(dot, vm);
        auto setCell = [&](uint32_t idx, std::wstring const& text) {
            if (auto tb = kids.GetAt(idx).try_as<Controls::TextBlock>())
                if (std::wstring{ tb.Text() } != text)   // skip no-op sets (less layout churn)
                    tb.Text(winrt::hstring{ text });
        };
        setCell(2, vm.statusText.empty() ? StateLabel(vm.state) : vm.statusText);
        setCell(3, FormatCpuPct(vm.processorLoadPct, vm.state));
        setCell(4, FormatMemoryMb(vm.memoryAssignedMb));
        setCell(5, FormatUptime(vm.uptimeMs));
    }
}

namespace winrt::hyprv_app::implementation
{
    WelcomePage::WelcomePage()
    {
        HyprvAppLog(L"[welcome] ctor");
        // Re-apply selection brushes when the resolved theme changes —
        // ApplyRowSelection does a one-shot lookup of the selected-row
        // background, which would otherwise stay frozen at the theme
        // active at first selection. ActualThemeChanged fires whenever
        // the framework re-resolves RequestedTheme.
        this->ActualThemeChanged([](
            winrt::Microsoft::UI::Xaml::FrameworkElement const& sender,
            winrt::Windows::Foundation::IInspectable const&)
        {
            if (auto self = sender.try_as<winrt::hyprv_app::WelcomePage>())
            {
                auto impl = winrt::get_self<WelcomePage>(self);
                impl->ApplyRowSelection();
            }
        });
    }

    void WelcomePage::SetMainWindow(winrt::weak_ref<MainWindow> const& weakWindow)
    {
        m_mainWindow = weakWindow;
        // First-render: sync sort indicator glyphs to the default state
        // (Name asc) so the active-column arrow shows on cold start.
        UpdateSortIndicators();
        UpdateRemoteSortIndicators();
        RenderRecents();
        RenderRemoteHosts();
        RenderAllVms();
    }

    void WelcomePage::SetTabItem(Microsoft::UI::Xaml::Controls::TabViewItem const& tab)
    {
        m_tabItem = tab;
    }

    void WelcomePage::OnVmManagerChanged()
    {
        // VMManager fires from a worker thread; MainWindow already marshals
        // onto the UI dispatcher before calling here, so direct XAML touches
        // are safe.
        // A poll reported (even an empty VM list) — stop the cold-start spinner.
        // This callback fires ONLY from NotifyChanged (a completed poll), so keying
        // off it rather than "vms is non-empty" is what keeps a zero-VM machine from
        // spinning "Loading VMs…" forever.
        m_firstSnapshotRendered = true;
        RenderRecents();
        RenderRemoteHosts();
        RenderAllVms();
    }

    void WelcomePage::OnTabActivated()
    {
        // BumpRecent runs whenever any tab opens, including from this page.
        // Refresh so a freshly-opened VM moves to the top of Recents when
        // the user comes back here.
        RenderRecents();
        RenderRemoteHosts();
        RenderAllVms();
    }

    void WelcomePage::OnNewVmClick(Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Hand off to MainWindow, which owns the XamlRoot/theme/popup-suppression
        // plumbing the modal needs and opens a tab for the new VM on success.
        if (auto win = m_mainWindow.get())
        {
            HyprvAppLog(L"[welcome] opening New VM wizard");
            win->OpenNewVmDialog();
        }
        else
        {
            HyprvAppLog(L"[welcome] OnNewVmClick: mainWindow gone");
        }
    }

    void WelcomePage::OnAddRemoteHostClick(Windows::Foundation::IInspectable const&,
                                           Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (auto win = m_mainWindow.get())
        {
            HyprvAppLog(L"[welcome] opening Add Remote Host dialog");
            win->OpenRemoteHostDialog(winrt::hstring{ L"" });
        }
    }

    void WelcomePage::RenderRemoteHosts()
    {
        auto host = remoteHostsHost();
        if (!host) return;
        auto hosts = hyprv::app::settings::Settings::Instance().RemoteHosts();

        // Apply the active sort (Name or Address, asc/desc). Name uses the
        // display label (falls back to address when blank) so sorting matches
        // what the row shows.
        const bool asc = (m_remoteSortDir == SortDir::Asc);
        const auto key = m_remoteSortKey;
        std::sort(hosts.begin(), hosts.end(),
            [&](hyprv::app::settings::RemoteHost const& a,
                hyprv::app::settings::RemoteHost const& b)
            {
                std::wstring av, bv;
                if (key == RemoteSortKey::Address)
                {
                    av = a.address; bv = b.address;
                }
                else
                {
                    av = a.name.empty() ? a.address : a.name;
                    bv = b.name.empty() ? b.address : b.name;
                }
                int c = _wcsicmp(av.c_str(), bv.c_str());
                return asc ? (c < 0) : (c > 0);
            });

        // Short-circuit when nothing visible changed — the ~1s VMManager poll
        // calls this, and rebuilding would dismiss an open right-click menu.
        // The signature reflects the SORTED order, so a sort toggle re-renders.
        std::wstring sig;
        for (auto const& h : hosts)
        {
            sig += h.address; sig += L'\x1f';
            sig += h.name;    sig += L'\x1e';
        }
        if (sig != m_remoteHostsSig || host.Children().Size() != hosts.size())
        {
            m_remoteHostsSig = sig;
            host.Children().Clear();
            m_remoteRows.clear();
            auto weakWindow = m_mainWindow;
            auto self = get_weak();
            for (auto const& h : hosts)
            {
                std::wstring display = h.name.empty() ? h.address : h.name;
                std::wstring keyCopy = h.address;
                auto rowBorder = BuildRemoteHostRow(
                    display, h.address, winrt::hstring{ h.address }, weakWindow,
                    /*onSelect*/ [self, keyCopy]() {
                        if (auto sp = self.get())
                        {
                            sp->m_selectedRemoteKey = keyCopy;
                            // Single selection across the welcome page — clear
                            // any ALL-VMs row selection.
                            sp->m_selectedGuid.clear();
                            sp->ApplyRowSelection();
                            sp->ApplyRemoteRowSelection();
                        }
                    },
                    /*onOpen*/ [self, keyCopy]() {
                        if (auto sp = self.get()) sp->OpenRemoteHost(keyCopy);
                    });
                host.Children().Append(rowBorder);
                m_remoteRows.push_back({ keyCopy, display, rowBorder });
            }
            // Re-apply the persistent selection highlight after a rebuild.
            ApplyRemoteRowSelection();
        }

        // Header + empty hint toggle on whether there are any saved hosts.
        if (auto hdr = remoteHostsHeader())
            hdr.Visibility(hosts.empty() ? Visibility::Collapsed : Visibility::Visible);
        if (auto hint = remoteHostsEmptyHint())
            hint.Visibility(hosts.empty() ? Visibility::Visible : Visibility::Collapsed);
    }

    void WelcomePage::ApplyRemoteRowSelection()
    {
        // Same brushes / idiom as ApplyRowSelection (the ALL VMs table).
        auto isLight = false;
        if (auto fe = this->try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
            isLight = (fe.ActualTheme() == winrt::Microsoft::UI::Xaml::ElementTheme::Light);
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush selectedBrush;
        selectedBrush.Color(isLight
            ? winrt::Windows::UI::ColorHelper::FromArgb(0x33, 0x00, 0x00, 0x00)
            : winrt::Windows::UI::ColorHelper::FromArgb(0x40, 0xFF, 0xFF, 0xFF));
        auto transparent = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
            winrt::Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0) };
        for (auto const& r : m_remoteRows)
        {
            bool selected = !m_selectedRemoteKey.empty()
                && _wcsicmp(r.guid.c_str(), m_selectedRemoteKey.c_str()) == 0;
            r.row.Tag(winrt::box_value(selected));
            r.row.Background(selected
                ? selectedBrush
                : winrt::Microsoft::UI::Xaml::Media::Brush{ transparent });
        }
    }

    void WelcomePage::OnRemoteSortHeaderTapped(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&)
    {
        auto br = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::Border>();
        if (!br) return;
        int k = 0;
        auto tag = br.Tag();
        if (auto s = tag.try_as<winrt::hstring>())      k = _wtoi(s->c_str());
        else if (auto i = tag.try_as<int32_t>())        k = *i;
        auto newKey = static_cast<RemoteSortKey>(k);
        if (newKey == m_remoteSortKey)
            m_remoteSortDir = (m_remoteSortDir == SortDir::Asc) ? SortDir::Desc : SortDir::Asc;
        else
        {
            m_remoteSortKey = newKey;
            m_remoteSortDir = SortDir::Asc;
        }
        m_remoteHostsSig.clear();   // force a re-render under the new order
        UpdateRemoteSortIndicators();
        RenderRemoteHosts();
    }

    void WelcomePage::UpdateRemoteSortIndicators()
    {
        // E70E = up (asc), E70D = down (desc). Show only on the active column.
        struct Slot { RemoteSortKey key;
                      std::function<winrt::Microsoft::UI::Xaml::Controls::FontIcon()> get; };
        Slot slots[] = {
            { RemoteSortKey::Name,    [this] { return sortArrowRemoteName();    } },
            { RemoteSortKey::Address, [this] { return sortArrowRemoteAddress(); } },
        };
        for (auto const& slot : slots)
        {
            auto icon = slot.get();
            if (!icon) continue;
            if (slot.key == m_remoteSortKey)
            {
                icon.Visibility(Visibility::Visible);
                icon.Glyph(m_remoteSortDir == SortDir::Asc ? L"\xE70E" : L"\xE70D");
            }
            else
            {
                icon.Visibility(Visibility::Collapsed);
            }
        }
    }

    void WelcomePage::OnFilterTextChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
    {
        if (auto box = filterBox())
            m_filter = std::wstring{ box.Text() };
        RenderAllVms();
    }

    void WelcomePage::OnSortHeaderTapped(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&)
    {
        // Header Border's Tag carries the SortKey as an int (boxed). The
        // XAML Tag literal is a string by default, so unbox as either int
        // (style A) or string (style B) — string is what x:Bind-less Tag
        // assignment with bare integers in XAML produces.
        auto br = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::Border>();
        if (!br) return;
        int key = 0;
        auto tag = br.Tag();
        if (auto s = tag.try_as<winrt::hstring>())
        {
            key = _wtoi(s->c_str());
        }
        else if (auto i = tag.try_as<int32_t>())
        {
            key = *i;
        }
        auto newKey = static_cast<SortKey>(key);

        if (newKey == m_sortKey)
        {
            // Same column tapped — flip direction.
            m_sortDir = (m_sortDir == SortDir::Asc) ? SortDir::Desc : SortDir::Asc;
        }
        else
        {
            // New column — switch to it ascending. Standard data-grid feel:
            // sortable columns "start fresh" rather than inheriting the
            // previous column's direction.
            m_sortKey = newKey;
            m_sortDir = SortDir::Asc;
        }
        UpdateSortIndicators();
        RenderAllVms();
    }

    void WelcomePage::UpdateSortIndicators()
    {
        // Segoe MDL2 chevrons: E70E = up, E70D = down. Show the arrow only
        // on the currently-active sort column; hide on the others.
        struct ArrowSlot {
            SortKey key;
            std::function<winrt::Microsoft::UI::Xaml::Controls::FontIcon()> get;
        };
        ArrowSlot slots[] = {
            { SortKey::Name,   [this] { return sortArrowName();   } },
            { SortKey::Status, [this] { return sortArrowStatus(); } },
            { SortKey::Cpu,    [this] { return sortArrowCpu();    } },
            { SortKey::Memory, [this] { return sortArrowMemory(); } },
            { SortKey::Uptime, [this] { return sortArrowUptime(); } },
        };
        for (auto const& slot : slots)
        {
            auto icon = slot.get();
            if (!icon) continue;
            if (slot.key == m_sortKey)
            {
                icon.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                icon.Glyph(m_sortDir == SortDir::Asc ? L"\xE70E" : L"\xE70D");
            }
            else
            {
                icon.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            }
        }
    }

    void WelcomePage::OpenVm(std::wstring const& guid, std::wstring const& name)
    {
        auto win = m_mainWindow.get();
        if (!win)
        {
            HyprvAppLog(L"[welcome] OpenVm: mainWindow gone");
            return;
        }
        HyprvAppLog(L"[welcome] OpenVm guid=%s name=%s",
            guid.c_str(), name.c_str());

        // "Keep home tab open" (GENERAL setting) turns the replace-on-open
        // gesture into a plain open: the VM tab opens + selects, the home tab
        // stays put. Default off = the browser-new-tab-page replace behaviour.
        const bool keepHome =
            hyprv::app::settings::Settings::Instance().KeepHomeTabOpen();

        if (m_tabItem && !keepHome)
        {
            // Replace-on-open: hand the open + close + force-reselect dance
            // to MainWindow::ReplaceTabWith. Defer to the next dispatcher
            // tick so any in-flight TabView events from the click that
            // brought us here settle before we mutate TabItems.
            auto tab = m_tabItem;
            m_tabItem = nullptr;
            winrt::hstring guidH{ guid };
            winrt::hstring nameH{ name };
            auto winWeak = win->get_weak();
            auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
            if (queue)
            {
                queue.TryEnqueue([winWeak, tab, guidH, nameH]() {
                    if (auto w = winWeak.get())
                    {
                        HyprvAppLog(L"[welcome] ReplaceTabWith (deferred)");
                        w->ReplaceTabWith(tab, guidH, nameH);
                    }
                });
            }
            else
            {
                HyprvAppLog(L"[welcome] no dispatcher, replacing sync (likely a bug)");
                win->ReplaceTabWith(tab, winrt::hstring{ guid }, winrt::hstring{ name });
            }
        }
        else
        {
            // "Keep home tab open" is on, OR the welcome page isn't sitting in
            // a tab (tear-away path). Either way, just open + select the VM tab
            // and leave this welcome tab where it is. OpenVmTab appends + sets
            // SelectedItem, so the user lands on the VM.
            win->OpenVmTab(winrt::hstring{ guid }, winrt::hstring{ name });
        }
    }

    void WelcomePage::OpenRemoteHost(std::wstring const& address)
    {
        auto win = m_mainWindow.get();
        if (!win)
        {
            HyprvAppLog(L"[welcome] OpenRemoteHost: mainWindow gone");
            return;
        }
        HyprvAppLog(L"[welcome] OpenRemoteHost %s", address.c_str());

        // Same "Keep home tab open" gesture as OpenVm — replace the home tab
        // (default) or keep it (setting on). Identical to the VM path so VMs and
        // remote sessions behave the same on the welcome page.
        const bool keepHome =
            hyprv::app::settings::Settings::Instance().KeepHomeTabOpen();

        if (m_tabItem && !keepHome)
        {
            auto tab = m_tabItem;
            m_tabItem = nullptr;
            winrt::hstring addrH{ address };
            auto winWeak = win->get_weak();
            auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
            if (queue)
            {
                queue.TryEnqueue([winWeak, tab, addrH]() {
                    if (auto w = winWeak.get())
                    {
                        HyprvAppLog(L"[welcome] ReplaceTabWithRemoteHost (deferred)");
                        w->ReplaceTabWithRemoteHost(tab, addrH);
                    }
                });
            }
            else
            {
                win->ReplaceTabWithRemoteHost(tab, addrH);
            }
        }
        else
        {
            win->OpenRemoteHostTab(winrt::hstring{ address });
        }
    }

    void WelcomePage::RenderRecents()
    {
        auto host = recentsHost();
        if (!host) return;

        // VM lookup (by lowercased GUID). Missing GUIDs = deleted VMs → skipped.
        std::vector<hyprv::app::vm::VirtualMachine> vms;
        try { vms = hyprv::app::vm::VMManager::Instance().GetAll(); }
        catch (...) { vms.clear(); }
        std::unordered_map<std::wstring, hyprv::app::vm::VirtualMachine const*> byGuid;
        byGuid.reserve(vms.size());
        for (auto const& v : vms)
        {
            std::wstring k = v.guid;
            std::transform(k.begin(), k.end(), k.begin(), ::towlower);
            byGuid.emplace(std::move(k), &v);
        }
        // Saved-host lookup (by lowercased address). Missing = forgotten → skipped.
        auto hosts = hyprv::app::settings::Settings::Instance().RemoteHosts();
        std::unordered_map<std::wstring, hyprv::app::settings::RemoteHost const*> byAddr;
        byAddr.reserve(hosts.size());
        for (auto const& h : hosts)
        {
            std::wstring k = h.address;
            std::transform(k.begin(), k.end(), k.begin(), ::towlower);
            byAddr.emplace(std::move(k), &h);
        }

        // Materialize the visible recents in MRU order, resolving each per kind.
        // Remote pills carry no live state (vm == nullptr); VM pills carry the
        // snapshot so the fast path can refresh their state dot.
        struct Vis { bool remote; std::wstring key; std::wstring name;
                     hyprv::app::vm::VirtualMachine const* vm; };
        std::vector<Vis> visible;
        for (auto const& r : hyprv::app::settings::Settings::Instance().Recents())
        {
            std::wstring k = r.guid;
            std::transform(k.begin(), k.end(), k.begin(), ::towlower);
            if (r.kind == hyprv::app::settings::RecentKind::Remote)
            {
                auto it = byAddr.find(k);
                if (it == byAddr.end()) continue;
                std::wstring disp = it->second->name.empty()
                    ? it->second->address : it->second->name;
                visible.push_back({ true, r.guid, std::move(disp), nullptr });
            }
            else
            {
                auto it = byGuid.find(k);
                if (it == byGuid.end()) continue;
                visible.push_back({ false, r.guid, it->second->elementName, it->second });
            }
        }

        // FAST PATH: same pills in the same order → refresh VM dots in place
        // (no Clear → keeps an open right-click menu + smooth blink). Remote
        // pills have no state dot, so they're skipped in the refresh.
        bool sameStructure = (!m_recentPills.empty() && visible.size() == m_recentPills.size());
        if (sameStructure)
            for (size_t i = 0; i < visible.size(); ++i)
                if (_wcsicmp(visible[i].key.c_str(), m_recentPills[i].guid.c_str()) != 0 ||
                    _wcsicmp(visible[i].name.c_str(), m_recentPills[i].name.c_str()) != 0)
                { sameStructure = false; break; }
        if (sameStructure)
        {
            for (size_t i = 0; i < visible.size(); ++i)
                if (!visible[i].remote && visible[i].vm)
                    UpdateRecentPill(m_recentPills[i].row, *visible[i].vm);
            return;
        }

        // SLOW PATH: rebuild.
        host.Children().Clear();
        m_recentPills.clear();
        auto self = get_weak();
        for (auto const& e : visible)
        {
            Controls::Border pill{ nullptr };
            if (e.remote)
            {
                pill = BuildRemoteHostPill(e.name, winrt::hstring{ e.key }, m_mainWindow,
                    [self, key = std::wstring{ e.key }]() {
                        if (auto sp = self.get()) sp->OpenRemoteHost(key);
                    });
            }
            else
            {
                std::wstring guidCopy = e.key;
                std::wstring nameCopy = e.name;
                pill = BuildRecentPill(*e.vm, m_mainWindow,
                    [self, guidCopy, nameCopy]() {
                        if (auto sp = self.get())
                            sp->OpenVm(guidCopy, nameCopy);
                    });
            }
            host.Children().Append(pill);
            m_recentPills.push_back({ e.key, e.name, pill });
        }

        // Hide the whole Recents section when empty.
        if (auto section = recentsSection())
        {
            section.Visibility(visible.empty()
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : Microsoft::UI::Xaml::Visibility::Visible);
        }
    }

    void WelcomePage::OnRetryConnectClick(Windows::Foundation::IInspectable const&,
                                          Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Re-attempt the Hyper-V connection. On success the poll thread starts and
        // its first NotifyChanged repopulates the list; re-arm the cold-start
        // spinner so it shows until that lands. On failure the error stays put
        // (RenderAllVms re-reads the — possibly updated — message).
        if (hyprv::app::vm::VMManager::Instance().RetryConnect())
            m_firstSnapshotRendered = false;
        RenderAllVms();
    }

    void WelcomePage::RenderAllVms()
    {
        auto host = allVmsHost();
        if (!host) return;

        // Hyper-V unreachable (not installed / no access): show an actionable
        // error instead of a perpetual "Loading VMs…" spinner. Recents + Remote
        // Hosts (Settings-backed) stay usable, so this is scoped to this section.
        {
            using CS = hyprv::app::vm::VMManager::ConnectStatus;
            auto cs = hyprv::app::vm::VMManager::Instance().GetConnectStatus();
            bool failed = (cs != CS::Ok);
            if (auto e = allVmsError())
            {
                if (failed)
                    if (auto t = allVmsErrorText())
                        t.Text(winrt::hstring{ hyprv::app::vm::VMManager::Instance().ConnectErrorMessage() });
                e.Visibility(failed ? Microsoft::UI::Xaml::Visibility::Visible
                                    : Microsoft::UI::Xaml::Visibility::Collapsed);
            }
            if (failed)
            {
                if (auto l = allVmsLoading())   l.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
                if (auto t = allVmsTable())     t.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
                if (auto h = allVmsEmptyHint()) h.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
                return;
            }
        }

        std::vector<hyprv::app::vm::VirtualMachine> vms;
        try { vms = hyprv::app::vm::VMManager::Instance().GetAll(); }
        catch (...) { vms.clear(); }

        // Apply the active sort. Comparator returns "a comes before b". For
        // descending, we flip the result. Status sort uses the display
        // label (StateLabel) so reordering matches what the user sees in
        // the column. CPU/Memory/Uptime sort numerically. Stopped VMs have
        // 0 for runtime counters; they cluster at the bottom of ascending
        // numeric sorts naturally.
        SortKey key = m_sortKey;
        bool asc = (m_sortDir == SortDir::Asc);
        std::sort(vms.begin(), vms.end(),
            [key, asc](auto const& a, auto const& b) {
                int cmp = 0;
                switch (key)
                {
                case SortKey::Name:
                    cmp = _wcsicmp(a.elementName.c_str(), b.elementName.c_str());
                    break;
                case SortKey::Status:
                {
                    // statusText (live job text) > stable state label, so
                    // the table reads the way the user sees it. Fall back
                    // to elementName for stable order within a state.
                    std::wstring sa = a.statusText.empty() ? StateLabel(a.state) : a.statusText;
                    std::wstring sb = b.statusText.empty() ? StateLabel(b.state) : b.statusText;
                    cmp = _wcsicmp(sa.c_str(), sb.c_str());
                    if (cmp == 0)
                        cmp = _wcsicmp(a.elementName.c_str(), b.elementName.c_str());
                    break;
                }
                case SortKey::Cpu:
                    cmp = (a.processorLoadPct < b.processorLoadPct) ? -1
                        : (a.processorLoadPct > b.processorLoadPct) ?  1 : 0;
                    if (cmp == 0)
                        cmp = _wcsicmp(a.elementName.c_str(), b.elementName.c_str());
                    break;
                case SortKey::Memory:
                    cmp = (a.memoryAssignedMb < b.memoryAssignedMb) ? -1
                        : (a.memoryAssignedMb > b.memoryAssignedMb) ?  1 : 0;
                    if (cmp == 0)
                        cmp = _wcsicmp(a.elementName.c_str(), b.elementName.c_str());
                    break;
                case SortKey::Uptime:
                    cmp = (a.uptimeMs < b.uptimeMs) ? -1
                        : (a.uptimeMs > b.uptimeMs) ?  1 : 0;
                    if (cmp == 0)
                        cmp = _wcsicmp(a.elementName.c_str(), b.elementName.c_str());
                    break;
                }
                return asc ? (cmp < 0) : (cmp > 0);
            });

        std::wstring filter = m_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(), ::towlower);

        // Materialize the visible (sorted + filtered) list as pointers into the
        // stable local `vms`.
        std::vector<hyprv::app::vm::VirtualMachine const*> visible;
        visible.reserve(vms.size());
        for (auto const& vm : vms)
        {
            if (!filter.empty())
            {
                std::wstring name = vm.elementName;
                std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                if (name.find(filter) == std::wstring::npos) continue;
            }
            visible.push_back(&vm);
        }

        // FAST PATH: same rows in the same order (the common case — stable Name
        // sort, only stats/state changing per poll). Update cells IN PLACE
        // instead of Clear()+rebuild, so an open right-click menu isn't
        // dismissed and the dot's blink doesn't restart (the two UX bugs the
        // full rebuild caused). A rename or reorder falls through to a rebuild.
        bool sameStructure = (!m_rows.empty() && visible.size() == m_rows.size());
        if (sameStructure)
            for (size_t i = 0; i < visible.size(); ++i)
                if (_wcsicmp(visible[i]->guid.c_str(), m_rows[i].guid.c_str()) != 0 ||
                    _wcsicmp(visible[i]->elementName.c_str(), m_rows[i].name.c_str()) != 0)
                { sameStructure = false; break; }
        if (sameStructure)
        {
            for (size_t i = 0; i < visible.size(); ++i)
                UpdateVmRow(m_rows[i].row, *visible[i]);
            ApplyRowSelection();
            return;
        }

        // SLOW PATH: structure changed (add/remove/rename/reorder/filter) →
        // rebuild. Selection logic (ApplyRowSelection, arrow-key nav,
        // Enter-to-open) reads m_rows — display order matters.
        host.Children().Clear();
        m_rows.clear();
        m_rows.reserve(visible.size());

        int rendered = 0;
        for (auto const* vmp : visible)
        {
            auto const& vm = *vmp;
            std::wstring guidCopy = vm.guid;
            std::wstring nameCopy = vm.elementName;
            auto self = get_weak();
            auto rowEl = BuildVmRow(vm, m_mainWindow,
                /*onOpen*/ [self, guidCopy, nameCopy]() {
                    if (auto sp = self.get())
                        sp->OpenVm(guidCopy, nameCopy);
                },
                /*onSelect*/ [self, guidCopy]() {
                    if (auto sp = self.get())
                    {
                        sp->m_selectedGuid = guidCopy;
                        // Single selection across the welcome page — clear any
                        // remote-host row selection.
                        sp->m_selectedRemoteKey.clear();
                        sp->ApplyRowSelection();
                        sp->ApplyRemoteRowSelection();
                        // Push the selection into the info flyout (if it's
                        // open). MainWindow no-ops when closed, so safe to
                        // call unconditionally.
                        if (auto win = sp->m_mainWindow.get())
                            win->SetFlyoutVm(guidCopy);
                    }
                });
            host.Children().Append(rowEl);
            m_rows.push_back({ guidCopy, nameCopy, rowEl });
            ++rendered;
        }

        // If the previously-selected VM was filtered out / deleted, drop
        // the selection so arrow-key nav has a clean slate.
        if (!m_selectedGuid.empty())
        {
            bool stillVisible = false;
            for (auto const& r : m_rows)
            {
                if (_wcsicmp(r.guid.c_str(), m_selectedGuid.c_str()) == 0)
                {
                    stillVisible = true; break;
                }
            }
            if (!stillVisible) m_selectedGuid.clear();
        }
        ApplyRowSelection();

        // Cold-start spinner hides once the FIRST poll has COMPLETED — keyed off
        // "a poll reported" (m_firstSnapshotRendered, set in OnVmManagerChanged;
        // HasFirstSnapshot covers a welcome tab opened AFTER the initial poll), NOT
        // off "vms is non-empty". That distinction is what stops a machine with zero
        // VMs from spinning "Loading VMs…" forever. (A machine where Hyper-V isn't
        // functional never reaches here — the connect-error branch above returns.)
        // SCOPED to the ALL VMs section: Recents + Remote Hosts (Settings-backed)
        // stay usable throughout.
        using Vis = Microsoft::UI::Xaml::Visibility;
        const bool polled  = m_firstSnapshotRendered ||
                             hyprv::app::vm::VMManager::Instance().HasFirstSnapshot();
        const bool hasRows = (rendered > 0);
        if (auto loading = allVmsLoading()) loading.Visibility(polled ? Vis::Collapsed : Vis::Visible);
        // Table only when there are rows; otherwise the empty-state hint speaks.
        if (auto table = allVmsTable()) table.Visibility((polled && hasRows) ? Vis::Visible : Vis::Collapsed);
        if (auto hint = allVmsEmptyHint())
        {
            if (polled && !hasRows)
            {
                hint.Text(filter.empty()
                    ? winrt::hstring{ L"No virtual machines on this PC yet. Use New VM to create one." }
                    : winrt::hstring{ L"No VMs match your filter." });
                hint.Visibility(Vis::Visible);
            }
            else hint.Visibility(Vis::Collapsed);
        }
    }

    // Push m_selectedGuid into the rendered rows: tag the selected row's
    // Border with `true` and paint its background; clear on every other.
    // BuildVmRow's PointerEntered/Exited handlers check the same Tag to
    // avoid hover-painting over a persistent selection background.
    void WelcomePage::ApplyRowSelection()
    {
        // Explicit theme-aware brush instead of a Resources.Lookup.
        // AccentFillColorTertiaryBrush samples the user's Windows
        // accent at low alpha — for a dark-accent user it reads as a
        // near-black bar in Light mode. Hard-coded greys at moderate
        // alpha read consistently regardless of accent.
        auto isLight = false;
        if (auto fe = this->try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
            isLight = (fe.ActualTheme() == winrt::Microsoft::UI::Xaml::ElementTheme::Light);
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush selectedBrush;
        selectedBrush.Color(isLight
            ? winrt::Windows::UI::ColorHelper::FromArgb(0x33, 0x00, 0x00, 0x00)   // light: ~20% black
            : winrt::Windows::UI::ColorHelper::FromArgb(0x40, 0xFF, 0xFF, 0xFF)); // dark:  ~25% white
        auto transparent = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
            winrt::Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0) };

        for (auto const& r : m_rows)
        {
            bool selected = !m_selectedGuid.empty()
                && _wcsicmp(r.guid.c_str(), m_selectedGuid.c_str()) == 0;
            r.row.Tag(winrt::box_value(selected));
            r.row.Background(selected
                ? selectedBrush
                : winrt::Microsoft::UI::Xaml::Media::Brush{ transparent });
        }
    }

    void WelcomePage::OnPageTapped(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&)
    {
        // Click on blank page area → yield focus from the search box.
        // The page root is IsTabStop=True (per XAML) so this actually
        // accepts focus rather than silently no-op'ing.
        this->Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
    }

    void WelcomePage::OnPageKeyDown(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e)
    {
        // Skip when the search box has focus — TextBox owns arrows + Enter
        // for its own behavior (caret movement, single-line "submit"). The
        // OriginalSource is the focused element when KeyDown bubbles from
        // there.
        if (auto src = e.OriginalSource().try_as<
                winrt::Microsoft::UI::Xaml::Controls::TextBox>())
        {
            return;
        }

        if (m_rows.empty()) return;

        // Find current selection index (or -1 if nothing selected).
        int curIdx = -1;
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            if (_wcsicmp(m_rows[i].guid.c_str(), m_selectedGuid.c_str()) == 0)
            {
                curIdx = static_cast<int>(i);
                break;
            }
        }

        // Helper: keep the info flyout in sync with the new keyboard
        // selection. Mirrors what the row-click handler does so mouse
        // and keyboard paths behave identically.
        auto pushToFlyout = [this](std::wstring const& guid) {
            if (auto win = m_mainWindow.get())
                win->SetFlyoutVm(guid);
        };

        auto key = e.Key();
        using VirtualKey = winrt::Windows::System::VirtualKey;
        if (key == VirtualKey::Down)
        {
            int next = (curIdx < 0) ? 0
                                    : std::min<int>(curIdx + 1, static_cast<int>(m_rows.size()) - 1);
            m_selectedGuid = m_rows[next].guid;
            ApplyRowSelection();
            pushToFlyout(m_selectedGuid);
            // Bring into view if the table has scrolled — defer; rare on
            // the welcome screen since VM counts are small.
            e.Handled(true);
        }
        else if (key == VirtualKey::Up)
        {
            int next = (curIdx < 0) ? 0
                                    : std::max<int>(curIdx - 1, 0);
            m_selectedGuid = m_rows[next].guid;
            ApplyRowSelection();
            pushToFlyout(m_selectedGuid);
            e.Handled(true);
        }
        else if (key == VirtualKey::Enter)
        {
            if (curIdx >= 0)
            {
                OpenVm(m_rows[curIdx].guid, m_rows[curIdx].name);
                e.Handled(true);
            }
        }
        else if (key == VirtualKey::Escape)
        {
            // Convenience: Escape clears the selection + the filter (if any).
            m_selectedGuid.clear();
            if (!m_filter.empty())
            {
                m_filter.clear();
                if (auto box = filterBox()) box.Text(L"");
                RenderAllVms();
            }
            else
            {
                ApplyRowSelection();
            }
            // Clear the info flyout too so it doesn't show data for a row
            // that's no longer selected.
            pushToFlyout(std::wstring{});
            e.Handled(true);
        }
    }
}
