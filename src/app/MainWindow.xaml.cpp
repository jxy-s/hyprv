#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "VmTabPage.xaml.h"
#include "vm/VMManager.h"
#include "wmi/WmiScope.h"
#include "settings/Settings.h"
#include "ui/VmTileFactory.h"
#include "ui/ConfirmDialog.h"
#include "ui/PopupBackdrop.h"
#include "ui/WindowManager.h"
#include "AppSettingsPage.xaml.h"
#include "NewVmDialog.xaml.h"
#include "RemoteHostTabPage.xaml.h"
#include "RemoteHostDialog.xaml.h"

#include <microsoft.ui.xaml.window.h>   // IWindowNative
#include <commctrl.h>                   // SetWindowSubclass

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.h>                  // WindowId
#include <winrt/Microsoft.UI.Xaml.Media.h>       // MicaBackdrop, SolidColorBrush, FontFamily
#include <winrt/Microsoft.UI.Xaml.Shapes.h>      // Ellipse, Polyline
#include <winrt/Windows.UI.h>                    // Color, ColorHelper
#include <winrt/Microsoft.UI.Xaml.Input.h>       // PointerRoutedEventArgs
#include <winrt/Microsoft.UI.Input.h>            // PointerPoint (needed for .Position())
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>               // FontWeights
#include <winrt/Windows.ApplicationModel.DataTransfer.h>  // DataPackage(Operation) for tab drag
#include <winrt/Windows.Graphics.h>              // PointInt32 / RectInt32 for window placement

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// ---- Shared logger ------------------------------------------------------
// VmTabPage.xaml.cpp + RdpHostClient.cpp + Settings.cpp all declare this
// extern. Writes go to %LOCALAPPDATA%\hyprv\hyprv.log when enabled, gated by
// Settings::LoggingEnabled(). When the setting is off the function returns
// before opening the file — no disk activity at all for release users who
// haven't opted into diagnostics.
//
// The Settings call is cheap (one mutex + one bool read). Even at the spammy
// rates we hit during connect (~50 lines/sec) it's negligible. Gating here
// instead of at each callsite means we don't need to thread Settings into
// every translation unit that wants to log.
static FILE*      g_log = nullptr;
static std::mutex g_logMutex;
void HyprvAppLog(const wchar_t* fmt, ...)
{
    if (!hyprv::app::settings::Settings::Instance().LoggingEnabled())
        return;
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_log)
    {
        // Sit next to settings.json for easy discoverability. The Settings
        // singleton creates the parent dir on first run, so by the time we
        // get here the path is guaranteed to exist.
        auto path = hyprv::app::settings::Settings::Instance().FilePath()
                        .parent_path() / L"hyprv.log";
        // _wfsopen with _SH_DENYWR (deny WRITE share) — leaves the file
        // readable while we have it open for write. tail -f / notepad /
        // VS Code can open and follow the log live. _wfopen_s opens with
        // _SH_DENYRW (deny everything) which is the prior behavior that
        // blocked external readers.
        g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt);
    vfwprintf(g_log, fmt, ap);
    fputwc(L'\n', g_log);
    fflush(g_log);
    va_end(ap);
}

namespace
{
    // ---- Flyout value formatters --------------------------------------------
    // All UI-side concerns — return "-" for missing/zero values so the
    // flyout never shows a numeric 0 that would falsely imply a known value.

    std::wstring FormatMemoryMb(uint64_t mb)
    {
        if (mb == 0) return L"-";
        wchar_t buf[32];
        if (mb >= 1024)
        {
            // Two decimals so demand vs assigned (e.g. 3.84 GB vs 4.00 GB) is
            // visible at a glance.
            double gb = mb / 1024.0;
            swprintf_s(buf, L"%.2f GB", gb);
        }
        else
        {
            swprintf_s(buf, L"%llu MB", static_cast<unsigned long long>(mb));
        }
        return buf;
    }

    std::wstring FormatUptime(uint64_t ms)
    {
        if (ms == 0) return L"-";
        uint64_t s = ms / 1000;
        uint64_t d = s / 86400; s %= 86400;
        uint64_t h = s / 3600;  s %= 3600;
        uint64_t m = s / 60;    uint64_t sec = s % 60;
        wchar_t buf[64];
        if (d > 0)
            swprintf_s(buf, L"%llud %lluh %llum",
                static_cast<unsigned long long>(d),
                static_cast<unsigned long long>(h),
                static_cast<unsigned long long>(m));
        else if (h > 0)
            swprintf_s(buf, L"%lluh %llum %llus",
                static_cast<unsigned long long>(h),
                static_cast<unsigned long long>(m),
                static_cast<unsigned long long>(sec));
        else
            swprintf_s(buf, L"%llum %llus",
                static_cast<unsigned long long>(m),
                static_cast<unsigned long long>(sec));
        return buf;
    }

    std::wstring FormatLocalTime(std::chrono::system_clock::time_point tp)
    {
        auto t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
        if (localtime_s(&tm, &t) != 0) return L"-";
        wchar_t buf[64];
        if (!wcsftime(buf, 64, L"%Y-%m-%d %H:%M", &tm)) return L"-";
        return buf;
    }

    // Msvm_SummaryInformation.Heartbeat uses CIM OperationalStatus codes.
    // 0 means the integration service isn't installed/running (very common
    // on stopped or non-Hyper-V-aware guests); show that as "-" not "Unknown".
    std::wstring HeartbeatLabel(uint16_t state)
    {
        switch (state)
        {
            case 0:  return L"-";
            case 2:  return L"OK";
            case 3:  return L"Degraded";
            case 6:  return L"Error";
            case 12: return L"No contact";
            case 13: return L"Lost communication";
            default: return std::to_wstring(state);
        }
    }

    // Render a sparkline into the given Grid. `host` is the chart area; we
    // wipe its children and add a single Polyline whose Points map the
    // history values 0..maxVal vertically and 0..host.ActualWidth horizontally.
    template <typename Sample>
    void DrawSparkline(
        Microsoft::UI::Xaml::Controls::Grid const& host,
        std::vector<Sample> const& history,
        double maxVal,
        Windows::UI::Color stroke)
    {
        if (!host) return;
        host.Children().Clear();
        double w = host.ActualWidth();
        double h = host.ActualHeight();
        if (w <= 1 || h <= 1) return;
        if (history.size() < 2 || maxVal <= 0) return;

        Microsoft::UI::Xaml::Shapes::Polyline poly;
        poly.Stroke(Media::SolidColorBrush{ stroke });
        poly.StrokeThickness(1.5);
        poly.StrokeLineJoin(Media::PenLineJoin::Round);

        // Light fill under the line for visual weight. Match stroke but at
        // ~20% alpha so it never competes with the text on top.
        auto fillColor = stroke;
        fillColor.A = 50;
        poly.Fill(Media::SolidColorBrush{ fillColor });

        auto pts = poly.Points();
        // Anchor the fill to the bottom-left corner so the closed polygon
        // shaded region matches the area under the curve.
        pts.Append(Windows::Foundation::Point{ 0.0f, static_cast<float>(h) });

        size_t n = history.size();
        for (size_t i = 0; i < n; ++i)
        {
            double x = (n == 1) ? w / 2.0 : static_cast<double>(i) * w / (n - 1);
            // Parenthesize to defeat the windows.h `min` macro (pch.h has no
            // NOMINMAX, so a bare `std::min(...)` expands to a broken expression).
            double clamped = (std::min)(static_cast<double>(history[i]), maxVal);
            // 1 DIP padding top/bottom so the line never clips against the
            // background's edge.
            double y = (h - 2) - (clamped / maxVal) * (h - 2) + 1;
            pts.Append(Windows::Foundation::Point{
                static_cast<float>(x), static_cast<float>(y) });
        }
        // Close the polygon at the bottom-right so Fill paints the area
        // under the curve cleanly.
        pts.Append(Windows::Foundation::Point{ static_cast<float>(w),
                                                static_cast<float>(h) });

        host.Children().Append(poly);
    }

    // Set a TextBlock's text only when it actually differs from the current
    // value. Calling .Text(...) unconditionally invalidates layout and clears
    // any active text selection — the flyout's selectable fields (GUID, IPs,
    // paths) would be impossible to copy if we wiped selection on every tick.
    void SetText(Microsoft::UI::Xaml::Controls::TextBlock const& target,
                 winrt::hstring const& value)
    {
        if (!target) return;
        if (target.Text() == value) return;
        target.Text(value);
    }
    void SetText(Microsoft::UI::Xaml::Controls::TextBlock const& tb,
                 std::wstring const& value)
    {
        SetText(tb, winrt::hstring{ value });
    }
    void SetText(Microsoft::UI::Xaml::Controls::TextBlock const& tb,
                 wchar_t const* value)
    {
        SetText(tb, winrt::hstring{ value ? value : L"" });
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

    // VM state dot color + the per-VM right-click context menu have moved
    // to src/app/ui/VmTileFactory.{h,cpp} so WelcomePage tiles can reuse
    // them. Reference them via hyprv::app::ui::VmDotColor / BuildVmContextMenu.

    // Build a fresh ListViewItem for a VM. Used the first time we see a GUID;
    // subsequent updates mutate the existing item via UpdateRailItem.
    // weakWindow lets the per-item DoubleTapped handler call OpenVmTab on the
    // owning MainWindow without holding a strong cycle. Type is weak_ref of the
    // implementation class (what get_weak() returns), not the projection — the
    // resolved com_ptr derefs directly to the impl so we can call OpenVmTab on
    // it without a winrt::get_self round-trip.
    Microsoft::UI::Xaml::Controls::ListViewItem CreateRailItem(
        hyprv::app::vm::VirtualMachine const& vm,
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow)
    {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;

        Grid row;
        row.HorizontalAlignment(HorizontalAlignment::Stretch);
        ColumnDefinition c0, c1;
        c0.Width(GridLengthHelper::FromValueAndType(10, GridUnitType::Pixel));
        c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);

        Shapes::Ellipse dot;
        dot.Width(6); dot.Height(6);
        dot.Margin({ 0, 0, 4, 0 });
        dot.VerticalAlignment(VerticalAlignment::Center);
        hyprv::app::ui::ApplyVmDotState(dot, vm);   // color + blink-while-transitioning
        Grid::SetColumn(dot, 0);
        row.Children().Append(dot);

        TextBlock tb;
        auto displayName = vm.elementName.empty() ? std::wstring{ L"<no name>" }
                                                  : vm.elementName;
        winrt::hstring rowText = vm.statusText.empty()
            ? winrt::hstring{ displayName }
            : winrt::hstring{ displayName + L" - " + vm.statusText };
        SetText(tb, rowText);
        tb.FontSize(12);
        tb.VerticalAlignment(VerticalAlignment::Center);
        tb.TextTrimming(TextTrimming::CharacterEllipsis);
        Grid::SetColumn(tb, 1);
        row.Children().Append(tb);

        ListViewItem lvi;
        lvi.Content(row);
        lvi.Tag(winrt::box_value(winrt::hstring{ vm.guid }));
        lvi.Padding({ 8, 0, 8, 0 });
        lvi.MinHeight(26);
        lvi.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        lvi.ContextFlyout(hyprv::app::ui::BuildVmContextMenu(
            winrt::hstring{ vm.guid }, winrt::hstring{ vm.elementName },
            weakWindow));

        // Double-click → open or focus the tab. Single click stays as
        // selection only (drives SyncRailSelectionToActive / future flyout
        // wiring) to avoid accidental tab opens when navigating the rail.
        winrt::hstring guidH{ vm.guid };
        lvi.DoubleTapped([guidH, weakWindow](
            winrt::Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& e)
        {
            e.Handled(true);
            auto win = weakWindow.get();
            if (!win) return;
            // Resolve the display name fresh — VMManager may have learned
            // the elementName since this rail item was created (cold-start race).
            std::wstring display;
            if (auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(
                    std::wstring{ guidH }))
            {
                display = vmOpt->elementName;
            }
            if (display.empty()) display = std::wstring{ guidH };
            HyprvAppLog(L"[ui] rail VM double-clicked guid=%s display=%s",
                std::wstring{ guidH }.c_str(), display.c_str());
            win->OpenVmTab(guidH, winrt::hstring{ display });
        });

        winrt::hstring tooltip;
        if (!vm.statusText.empty())
            tooltip = rowText;
        else
        {
            auto stable = vm.StableStatusLabel();
            tooltip = stable.empty()
                ? winrt::hstring{ displayName }
                : winrt::hstring{ displayName + L" - " + stable };
        }
        ToolTipService::SetToolTip(lvi, winrt::box_value(tooltip));
        return lvi;
    }

    void UpdateRailItem(
        Microsoft::UI::Xaml::Controls::ListViewItem const& lvi,
        hyprv::app::vm::VirtualMachine const& vm)
    {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        auto grid = lvi.Content().try_as<Grid>();
        if (!grid) return;

        auto displayName = vm.elementName.empty() ? std::wstring{ L"<no name>" }
                                                  : vm.elementName;
        winrt::hstring rowText = vm.statusText.empty()
            ? winrt::hstring{ displayName }
            : winrt::hstring{ displayName + L" - " + vm.statusText };
        winrt::hstring tooltip;
        if (!vm.statusText.empty())
            tooltip = rowText;
        else
        {
            auto stable = vm.StableStatusLabel();
            tooltip = stable.empty()
                ? winrt::hstring{ displayName }
                : winrt::hstring{ displayName + L" - " + stable };
        }

        if (grid.Children().Size() >= 2)
        {
            if (auto dot = grid.Children().GetAt(0).try_as<Shapes::Ellipse>())
                hyprv::app::ui::ApplyVmDotState(dot, vm);
            if (auto tb = grid.Children().GetAt(1).try_as<TextBlock>())
                SetText(tb, rowText);
        }
        ToolTipService::SetToolTip(lvi, winrt::box_value(tooltip));
    }

    // Diff the current snapshot against the existing ListView items. Updates
    // matching items in place, inserts new ones at the right sorted position,
    // removes any that have disappeared. No Clear() — no flash.
    void RenderRail(Microsoft::UI::Xaml::Controls::ListView const& list,
                    winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow)
    {
        if (!list) return;
        std::vector<hyprv::app::vm::VirtualMachine> snapshot;
        try
        {
            snapshot = hyprv::app::vm::VMManager::Instance().GetAll();
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[rail] VMManager EXCEPTION: %s", e.whatW.c_str());
            return;
        }
        std::sort(snapshot.begin(), snapshot.end(),
            [](auto const& a, auto const& b) {
                return _wcsicmp(a.elementName.c_str(), b.elementName.c_str()) < 0;
            });

        std::unordered_map<std::wstring, Microsoft::UI::Xaml::Controls::ListViewItem> existing;
        existing.reserve(list.Items().Size());
        for (uint32_t i = 0; i < list.Items().Size(); ++i)
        {
            auto lvi = list.Items().GetAt(i).try_as<Microsoft::UI::Xaml::Controls::ListViewItem>();
            if (!lvi) continue;
            auto g = winrt::unbox_value_or<winrt::hstring>(lvi.Tag(), L"");
            existing.emplace(std::wstring{ g }, lvi);
        }

        std::unordered_set<std::wstring> seen;
        seen.reserve(snapshot.size());

        for (uint32_t idx = 0; idx < snapshot.size(); ++idx)
        {
            auto const& vm = snapshot[idx];
            seen.insert(vm.guid);

            auto it = existing.find(vm.guid);
            if (it != existing.end())
            {
                UpdateRailItem(it->second, vm);
                uint32_t curIdx = 0;
                if (list.Items().IndexOf(it->second, curIdx))
                {
                    if (curIdx != idx)
                    {
                        list.Items().RemoveAt(curIdx);
                        list.Items().InsertAt(idx, it->second);
                    }
                }
            }
            else
            {
                auto lvi = CreateRailItem(vm, weakWindow);
                list.Items().InsertAt(idx, lvi);
            }
        }

        for (uint32_t i = list.Items().Size(); i-- > 0; )
        {
            auto lvi = list.Items().GetAt(i).try_as<Microsoft::UI::Xaml::Controls::ListViewItem>();
            if (!lvi) continue;
            auto g = winrt::unbox_value_or<winrt::hstring>(lvi.Tag(), L"");
            if (seen.find(std::wstring{ g }) == seen.end())
                list.Items().RemoveAt(i);
        }

        // Hide the "Loading..." placeholder once the first poll has produced
        // anything (and keep it hidden — empty list after this point means
        // WMI returned no VMs, which is a real result not a loading state).
        if (auto win = weakWindow.get())
        {
            if (auto lbl = win->hvVmListLoading())
            {
                lbl.Visibility(snapshot.empty() && list.Items().Size() == 0
                    ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed);
            }
        }
    }

    std::wstring FormatMac(std::wstring const& raw)
    {
        if (raw.size() != 12) return raw;
        std::wstring out;
        out.reserve(17);
        for (size_t i = 0; i < 12; ++i)
        {
            out.push_back(raw[i]);
            if (i % 2 == 1 && i != 11) out.push_back(L':');
        }
        return out;
    }

    std::wstring JoinIps(std::vector<std::wstring> const& ips)
    {
        if (ips.empty()) return L"-";
        std::wstring out;
        for (size_t i = 0; i < ips.size(); ++i)
        {
            if (i > 0) out += L", ";
            out += ips[i];
        }
        return out;
    }

    std::wstring FormatFileSize(uint64_t bytes)
    {
        if (bytes == 0) return L"-";
        wchar_t buf[32];
        double gb = bytes / 1073741824.0;
        double mb = bytes / 1048576.0;
        if (gb >= 1.0)       swprintf_s(buf, L"%.2f GB", gb);
        else if (mb >= 1.0)  swprintf_s(buf, L"%.1f MB", mb);
        else                 swprintf_s(buf, L"%llu KB",
                                static_cast<unsigned long long>(bytes / 1024));
        return buf;
    }

    std::wstring PathLeaf(std::wstring const& path)
    {
        auto p = path.find_last_of(L"\\/");
        return (p == std::wstring::npos) ? path : path.substr(p + 1);
    }

    std::wstring DiskKindLabel(hyprv::app::vm::DiskKind k)
    {
        using K = hyprv::app::vm::DiskKind;
        switch (k)
        {
            case K::Hdd: return L"Hard disk";
            case K::Dvd: return L"DVD drive";
            default:     return L"Storage";
        }
    }

    // (The earlier full-strip hover-tint helper was removed when splitter
    // hover switched to a centered pill — the pill's Background is set
    // directly in XAML via {ThemeResource AccentFillColorTertiaryBrush}.)
}

namespace winrt::hyprv_app::implementation
{
    MainWindow::MainWindow()
    {
        HyprvAppLog(L"[main] MainWindow ctor");
        m_uiQueue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        // Defer real work to first activation; XAML accessors aren't always
        // safe to touch in the ctor.
        Activated([this](IInspectable const&, WindowActivatedEventArgs const& args)
        {
            OnActivated(args);
        });
    }

    MainWindow::~MainWindow() = default;

    void MainWindow::OnActivated(Microsoft::UI::Xaml::WindowActivatedEventArgs const&)
    {
        if (m_activated) return;
        m_activated = true;
        HyprvAppLog(L"[main] OnActivated");

        // Order matters here. Steps below must run in this exact sequence:
        //   1. Resolve the HWND so we can call Win32 APIs against it.
        //   2. Apply persisted geometry — SetWindowPos moves the window to
        //      the user's last-known rect.
        //   3. ONLY THEN attach the subclass that persists future geometry
        //      changes.
        //
        // If 3 happens before 2, WinUI's initial layout pass fires
        // WM_WINDOWPOSCHANGED events for the OS-default window rect. Our
        // subclass would treat those as user-driven moves and call
        // PersistGeometry, which clobbers Settings::m_window in-memory
        // before ApplyPersistedGeometry ever reads it. Net effect: every
        // launch "restores" to whatever WinUI's default placement was,
        // *not* where the user left the window. See the long comment in
        // ApplyPersistedGeometry.
        ResolveWindowHwnd();
        // A secondary (torn-off) window is positioned under the cursor by the
        // tear-out framework — don't yank it to the primary's persisted rect.
        if (m_isPrimary)
            ApplyPersistedGeometry();
        AttachWindowSubclass();
        ExtendIntoTitleBar();

        // Initial rail render. VMManager OnChanged subscription drives future
        // refreshes. Marshal to the UI thread since the callback fires from
        // a WMI worker thread.
        RenderRail(hvVmList(), get_weak());
        // Remote hosts don't change on a WMI tick — render once here and again
        // only after an add / edit / forget (the rail expander stays hidden
        // while there are no saved hosts).
        RenderRemoteHostsRail();
        auto queue = m_uiQueue;
        auto weakSelf = get_weak();
        // Multicast subscribe — EVERY open window gets ticks (a single-sink
        // setter would starve all but the last-activated window once tab
        // tear-out opens a second window). Tokens removed on Closed below.
        m_onChangedToken = hyprv::app::vm::VMManager::Instance().AddOnChanged([queue, weakSelf]() {
            queue.TryEnqueue([weakSelf] {
                if (auto self = weakSelf.get())
                    self->OnVmManagerChanged();
            });
        });
        // Surface async VM-operation failures (e.g. a VM that can't start). The
        // callback fires off a job-watcher thread → marshal to the UI thread →
        // show an error dialog.
        m_onErrorToken = hyprv::app::vm::VMManager::Instance().AddOnError(
            [queue, weakSelf](std::wstring const& vmName, std::wstring const& message) {
                queue.TryEnqueue([weakSelf, vmName, message] {
                    if (weakSelf.get())
                        hyprv::app::ui::ShowErrorDialog(weakSelf,
                            vmName.empty() ? std::wstring{ L"Virtual machine error" }
                                           : vmName + L" — operation failed",
                            message);
                });
            });
        // On close: drop the VMManager subscriptions (so a torn-off window's
        // stale callback can't fire post-teardown) AND explicitly tear down each
        // tab's rdphost child. Closing a SECONDARY window does NOT exit the
        // process, so the kill-on-job-close job that reaps rdphost on app exit
        // won't fire — without this a torn-off window's rdphost.exe would orphan
        // (the VmTabPage dtor is only a best-effort safety net).
        uint64_t changedTok = m_onChangedToken, errorTok = m_onErrorToken;
        this->Closed([changedTok, errorTok, weakSelf](
            winrt::Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::WindowEventArgs const&) {
            auto& vmm = hyprv::app::vm::VMManager::Instance();
            vmm.RemoveOnChanged(changedTok);
            vmm.RemoveOnError(errorTok);
            // weakSelf.get() yields the impl directly (get_weak() on this class
            // is weak_ref<implementation::MainWindow>), so call it directly —
            // exactly like self->OnVmManagerChanged() above. (get_self is for
            // PROJECTED objects and would mis-cast the impl.)
            if (auto self = weakSelf.get())
                self->ShutdownAllTabClients();
        });

        // Session restore: rebuild the tab strip from the persisted list.
        // Falls through to a fresh welcome tab if nothing was persisted
        // (first run, migration from older settings, or empty list).
        // Session restore + welcome fallback only for the PRIMARY. A secondary
        // (torn-off) window starts empty; if it was created by a tear-out it
        // adopts the handed-off tab now that tabView() exists. Restoring here
        // would spawn a stray welcome tab + reload the primary's tabs wrongly.
        if (m_isPrimary)
        {
            RestoreOpenTabs();
            if (auto tv = tabView(); tv && tv.TabItems().Size() == 0)
            {
                HyprvAppLog(L"[main] OnActivated: no tabs restored, opening welcome");
                OpenWelcomeTab();
            }
        }
        else if (m_pendingAdoptItem)
        {
            if (auto tv = tabView())
            {
                tv.TabItems().Append(m_pendingAdoptItem);
                AdoptMovedTab(m_pendingAdoptItem);
            }
            m_pendingAdoptItem = nullptr;
        }
    }

    void MainWindow::ResolveWindowHwnd()
    {
        if (m_windowHwnd) return;
        auto native = this->try_as<::IWindowNative>();
        if (!native) { HyprvAppLog(L"[main] no IWindowNative"); return; }
        native->get_WindowHandle(&m_windowHwnd);
        if (!m_windowHwnd) { HyprvAppLog(L"[main] null window HWND"); return; }
    }

    void MainWindow::AttachWindowSubclass()
    {
        // Subclass the main HWND so we get notified when it moves/resizes —
        // those events don't reach XAML's SizeChanged. The active tab's
        // owned popup needs to follow. Caller must already have applied any
        // persisted geometry — the subclass-driven PersistGeometry path
        // would otherwise treat WinUI's initial layout pass as a user move
        // and clobber the persisted-settings cache in-memory.
        if (m_subclassed || !m_windowHwnd) return;
        BOOL ok = SetWindowSubclass(m_windowHwnd, &MainWindow::WindowSubclassProc, 1,
                          reinterpret_cast<DWORD_PTR>(this));
        HyprvAppLog(L"[sub] SetWindowSubclass hwnd=%p ok=%d", m_windowHwnd, ok ? 1 : 0);
        m_subclassed = (ok != 0);
    }

    void MainWindow::ExtendIntoTitleBar()
    {
        // Windows-Terminal-style title bar: XAML content extends behind the
        // title bar area; the OS still draws the min/max/close caption
        // buttons but we own everything else (tabs + custom buttons).
        ExtendsContentIntoTitleBar(true);
        if (auto drag = dragRegion()) SetTitleBar(drag);

        // Backdrop + theme — pulled from Settings::AppearancePref. Default
        // is Mica (subtle wallpaper tint) + System theme. The VM rdphost
        // popup is opaque so it covers the backdrop in the content area;
        // the title bar / tab strip area shows the backdrop through.
        ApplyAppearance();

        // Size the caption-reserve column to the real AppWindow inset
        // (English LTR ≈ 150 DIP but varies with DPI / RTL).
        try
        {
            Microsoft::UI::WindowId windowId{ reinterpret_cast<uint64_t>(m_windowHwnd) };
            auto appWindow = Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
            if (appWindow)
            {
                auto tb = appWindow.TitleBar();
                int rightInsetPx = tb.RightInset();
                UINT dpi = GetDpiForWindow(m_windowHwnd);
                double dipScale = dpi > 0 ? (dpi / 96.0) : 1.0;
                double rightInsetDip = rightInsetPx / dipScale;
                if (rightInsetDip > 0)
                {
                    if (auto col = captionButtonReserve())
                        col.Width(Microsoft::UI::Xaml::GridLength{
                            rightInsetDip, Microsoft::UI::Xaml::GridUnitType::Pixel });
                }
                HyprvAppLog(L"[tb] caption right inset=%dpx (%.1fdip)",
                    rightInsetPx, rightInsetDip);
            }
        }
        catch (...) { /* fall back to XAML default of 150 DIP */ }
    }

    void MainWindow::ApplyPersistedGeometry()
    {
        auto wg = hyprv::app::settings::Settings::Instance().WindowGeometry();

        // Restore window position + size if we have a valid persisted rect.
        // Both must be present — restoring size alone is what made the user
        // think persistence "wasn't working" (the window came up the right
        // size but the OS placed it at its own default position).
        //
        // CRITICAL: this method MUST run before AttachWindowSubclass. The
        // subclass dispatches WM_WINDOWPOSCHANGED to PersistGeometry, which
        // synchronously rewrites Settings::m_window in-memory. If the
        // subclass is attached first, WinUI's own startup layout passes
        // fire WM_WINDOWPOSCHANGED for the OS-default position, clobbering
        // the values we just loaded from disk — and the user sees the
        // window "fail to restore" on every launch (it actually does call
        // SetWindowPos, just with whatever WinUI happened to leave on the
        // screen instead of with the persisted values).
        const bool havePos  = (wg.x != INT_MIN && wg.y != INT_MIN);
        const bool haveSize = (wg.width > 0 && wg.height > 0);

        // Log what we found, before anything else might rewrite it. Lets
        // us tell at a glance whether Settings actually loaded our prefs
        // or whether something earlier silently clobbered the cache.
        HyprvAppLog(L"[settings] loaded window pref: pos=(%d,%d) size=%dx%d "
                    L"rail=%.0f flyout=%.0f",
                    havePos ? wg.x : -1, havePos ? wg.y : -1,
                    wg.width, wg.height, wg.railWidth, wg.flyoutWidth);

        if (m_windowHwnd && haveSize)
        {
            // Validate position is on a still-attached monitor — if the user
            // last ran on a now-unplugged secondary, restoring there would
            // put the window where they can't see it. MonitorFromPoint with
            // MONITOR_DEFAULTTONULL returns nullptr for off-screen coords.
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            int  x     = wg.x;
            int  y     = wg.y;
            if (!havePos)
            {
                flags |= SWP_NOMOVE;
                x = y = 0;
            }
            else
            {
                POINT topLeft{ x + 16, y + 16 };   // sample inside the rect
                if (!MonitorFromPoint(topLeft, MONITOR_DEFAULTTONULL))
                {
                    HyprvAppLog(L"[settings] persisted position (%d,%d) is "
                                L"off-screen — falling back to OS default", x, y);
                    flags |= SWP_NOMOVE;
                    x = y = 0;
                }
            }
            SetWindowPos(m_windowHwnd, nullptr, x, y, wg.width, wg.height, flags);

            // Confirm the OS actually accepted the rect we asked for.
            // GetWindowRect should now report (x, y, x+w, y+h). A mismatch
            // means something is fighting us (DPI awareness mismatch, a
            // shell snap layout, etc.) and the bug log will make it
            // obvious which.
            RECT after{};
            if (GetWindowRect(m_windowHwnd, &after))
            {
                int aw = after.right  - after.left;
                int ah = after.bottom - after.top;
                HyprvAppLog(L"[settings] restored window rect: requested "
                            L"(%d,%d) %dx%d, GetWindowRect now (%d,%d) %dx%d",
                    havePos ? wg.x : -1, havePos ? wg.y : -1, wg.width, wg.height,
                    after.left, after.top, aw, ah);
            }
        }
        else
        {
            HyprvAppLog(L"[settings] no persisted window rect to restore "
                        L"(havePos=%d haveSize=%d hwnd=%p)",
                        havePos ? 1 : 0, haveSize ? 1 : 0, m_windowHwnd);
        }

        // Rail + flyout visibility — both default to hidden (per Settings),
        // so the welcome tab is the primary surface on a fresh install.
        // Width is preserved separately from visibility so toggling off →
        // on restores the user's last preferred width.
        if (wg.railWidth > 0)     m_railSavedWidth   = wg.railWidth;
        if (wg.flyoutWidth > 0)   m_flyoutSavedWidth = wg.flyoutWidth;

        if (auto col = railColumn())
        {
            double targetW = wg.railVisible ? m_railSavedWidth : 0.0;
            col.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                targetW, Microsoft::UI::Xaml::GridUnitType::Pixel));
        }
        if (auto split = splitterColumn())
        {
            double splitW = wg.railVisible ? 6.0 : 0.0;
            split.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                splitW, Microsoft::UI::Xaml::GridUnitType::Pixel));
        }
        if (auto col = flyoutColumn())
        {
            double targetW = wg.flyoutVisible ? m_flyoutSavedWidth : 0.0;
            col.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                targetW, Microsoft::UI::Xaml::GridUnitType::Pixel));
        }
        if (auto split = flyoutSplitterColumn())
        {
            double splitW = wg.flyoutVisible ? 6.0 : 0.0;
            split.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                splitW, Microsoft::UI::Xaml::GridUnitType::Pixel));
        }
        HyprvAppLog(L"[settings] applied rail visible=%d width=%.0f, flyout visible=%d width=%.0f",
            wg.railVisible ? 1 : 0, m_railSavedWidth,
            wg.flyoutVisible ? 1 : 0, m_flyoutSavedWidth);
    }

    void MainWindow::PersistOpenTabs()
    {
        // Only the primary persists the open-tabs list — it's a single global
        // Settings blob, so a secondary (torn-off) window writing it would
        // corrupt the primary's restore. Torn-off windows are not session-
        // persisted across launches (accepted v1 limitation).
        if (!m_isPrimary) return;
        // Skip while RestoreOpenTabs is building the strip — every tab it
        // creates would otherwise fire a Persist call with a half-built
        // state, racing the final SelectedItem write.
        if (m_restoringTabs) return;
        auto tv = tabView();
        if (!tv) return;

        std::vector<hyprv::app::settings::OpenTab> snapshot;
        snapshot.reserve(tv.TabItems().Size());
        int selectedIdx = -1;
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            if (auto vm = item.Tag().try_as<winrt::hyprv_app::VmTabPage>())
            {
                snapshot.push_back({ L"vm", std::wstring{ vm.VmGuid() } });
            }
            else if (item.Tag().try_as<winrt::hyprv_app::WelcomePage>())
            {
                // Today every welcome tab is for the local host. Once
                // remote-host welcome tabs land, the identifier will be
                // the remote's hostname / display name.
                snapshot.push_back({ L"welcome", L"local" });
            }
            else if (item.Tag().try_as<winrt::hyprv_app::AppSettingsPage>())
            {
                // Application settings is a singleton surface — identifier
                // is just "app". Settings tab dedupes on open so there's
                // never more than one in the strip.
                snapshot.push_back({ L"settings", L"app" });
            }
            else if (auto rh = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>())
            {
                // Remote host tab — identifier is the host's address key.
                snapshot.push_back({ L"rdp", std::wstring{ rh.HostAddress() } });
            }
            else
            {
                // Unknown tab type — skip from persistence so a future
                // launch doesn't choke on it.
                continue;
            }
            if (tv.SelectedItem() == item)
                selectedIdx = static_cast<int>(snapshot.size()) - 1;
        }
        hyprv::app::settings::Settings::Instance().SetOpenTabs(snapshot, selectedIdx);
    }

    void MainWindow::RestoreOpenTabs()
    {
        auto persisted = hyprv::app::settings::Settings::Instance().OpenTabs();
        int  wantSel   = hyprv::app::settings::Settings::Instance().SelectedTabIndex();

        // First-run / migrated-from-old-version path: nothing to restore.
        // Caller (OnActivated) handles the empty-strip case by opening a
        // fresh welcome tab.
        if (persisted.empty())
        {
            HyprvAppLog(L"[main] RestoreOpenTabs: nothing persisted");
            return;
        }

        m_restoringTabs = true;
        int restoredCount = 0;
        for (auto const& t : persisted)
        {
            if (_wcsicmp(t.type.c_str(), L"welcome") == 0)
            {
                OpenWelcomeTab();
                ++restoredCount;
            }
            else if (_wcsicmp(t.type.c_str(), L"vm") == 0)
            {
                // Don't gate on GetByGuid here. VMManager hasn't done its
                // first WMI poll yet (we're running synchronously inside
                // OnActivated), so every lookup would return nullopt during
                // cold start and every restored VM tab would be silently
                // skipped — then the final PersistOpenTabs below would
                // write back the truncated list, erasing the user's VM
                // tabs from settings.json forever.
                //
                // Instead, always open the tab. VmTabPage shows a centered
                // "Loading VM..." overlay until VMManager fires its first
                // OnChanged (HasFirstSnapshot becomes true), at which
                // point UpdatePlaceholderAndClient + UpdateVmTabHeader
                // resolve to the live VM data. If the VM was genuinely
                // deleted between sessions, the placeholder flips to
                // "(missing VM)" once the cache is populated and the user
                // can close the tab manually.
                //
                // Display name is the empty string at restore time —
                // UpdateVmTabHeader fills it in on the first VMManager
                // tick that knows about this VM.
                OpenVmTab(winrt::hstring{ t.identifier },
                          winrt::hstring{ L"" });
                ++restoredCount;
            }
            else if (_wcsicmp(t.type.c_str(), L"settings") == 0)
            {
                OpenAppSettingsTab();
                ++restoredCount;
            }
            else if (_wcsicmp(t.type.c_str(), L"rdp") == 0)
            {
                // Only restore a remote-host tab if the host is still saved
                // (the user may have forgotten it between sessions). Unlike VM
                // tabs, we CAN check synchronously — the saved-hosts list is
                // loaded from settings.json, not awaiting a WMI poll.
                if (hyprv::app::settings::Settings::Instance()
                        .FindRemoteHost(t.identifier))
                {
                    // Restore the tab but DON'T auto-connect — show an idle
                    // "Connect" placeholder so launch doesn't fire a credential
                    // prompt for every saved remote tab.
                    OpenRemoteHostTab(winrt::hstring{ t.identifier },
                                      /*autoConnect*/ false);
                    ++restoredCount;
                }
            }
        }
        m_restoringTabs = false;

        // Restore selection. wantSel is an index into the PERSISTED list;
        // after skipping deleted VMs, the live tab strip may be shorter,
        // so clamp. If selection wasn't recorded or all referenced tabs
        // skipped, leave whatever's currently selected.
        if (auto tv = tabView())
        {
            uint32_t count = tv.TabItems().Size();
            if (count > 0)
            {
                int idx = (wantSel >= 0 && wantSel < static_cast<int>(count))
                              ? wantSel : 0;
                auto item = tv.TabItems().GetAt(static_cast<uint32_t>(idx))
                    .try_as<Microsoft::UI::Xaml::Controls::TabViewItem>();
                if (item) tv.SelectedItem(item);
            }
        }
        HyprvAppLog(L"[main] RestoreOpenTabs: restored=%d, wanted_sel=%d",
            restoredCount, wantSel);
        // Final persist now that the strip is stable — captures
        // post-skip indices.
        PersistOpenTabs();
    }

    void MainWindow::PersistGeometry()
    {
        // Only the primary persists window geometry — same single-global-blob
        // reason as PersistOpenTabs (a secondary would overwrite the primary's
        // saved rect).
        if (!m_isPrimary) return;

        hyprv::app::settings::Window wg{};

        if (m_windowHwnd)
        {
            RECT r{};
            if (GetWindowRect(m_windowHwnd, &r))
            {
                wg.x      = r.left;
                wg.y      = r.top;
                wg.width  = r.right  - r.left;
                wg.height = r.bottom - r.top;
            }
        }

        // Current rail/flyout widths AND visibility. Width 0 means
        // "hidden" — track that as visibility false but keep the saved
        // width so toggling on later restores the user's last size.
        if (auto col = railColumn())
        {
            double cur = col.Width().Value;
            wg.railVisible = (cur > 0);
            wg.railWidth   = (cur > 0) ? cur : m_railSavedWidth;
        }
        if (auto col = flyoutColumn())
        {
            double cur = col.Width().Value;
            wg.flyoutVisible = (cur > 0);
            wg.flyoutWidth   = (cur > 0) ? cur : m_flyoutSavedWidth;
        }

        // SetWindowGeometry is a no-op when nothing changed; the debounced
        // save thread coalesces rapid calls (splitter drag).
        hyprv::app::settings::Settings::Instance().SetWindowGeometry(wg);
    }

    LRESULT CALLBACK MainWindow::WindowSubclassProc(HWND hwnd, UINT msg,
        WPARAM wParam, LPARAM lParam, UINT_PTR /*id*/, DWORD_PTR refData)
    {
        // Owner-window position/size changes don't propagate to owned popups.
        // Refresh on WM_WINDOWPOSCHANGED (covers both moves and resizes) and
        // WM_DPICHANGED. Routed to the currently-active tab; others stay
        // hidden so their popup position doesn't matter.
        if (msg == WM_WINDOWPOSCHANGED || msg == WM_DPICHANGED)
        {
            auto* self = reinterpret_cast<MainWindow*>(refData);
            if (self)
            {
                if (auto active = self->ActiveTab())
                {
                    auto impl = winrt::get_self<implementation::VmTabPage>(active);
                    impl->RefreshPopupBounds();
                }
                else if (auto tv = self->tabView())
                {
                    // The active tab might be a remote-host tab (ActiveTab only
                    // resolves VmTabPage). Refresh its popup bounds too so the
                    // RDP surface follows window moves / resizes.
                    if (auto sel = tv.SelectedItem().try_as<
                            Microsoft::UI::Xaml::Controls::TabViewItem>())
                    {
                        if (auto rp = sel.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>())
                            winrt::get_self<implementation::RemoteHostTabPage>(rp)
                                ->RefreshPopupBounds();
                    }
                }
                // Also persist the new window geometry. Debounced save means
                // a drag-resize results in one disk write at the end.
                if (msg == WM_WINDOWPOSCHANGED)
                    self->PersistGeometry();
            }
        }
        // Final flush on window close — capture geometry before the HWND
        // goes away, so the persisted state matches what the user saw.
        else if (msg == WM_CLOSE || msg == WM_DESTROY)
        {
            auto* self = reinterpret_cast<MainWindow*>(refData);
            if (self) self->PersistGeometry();
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    // ---- Tab management -----------------------------------------------------

    winrt::hyprv_app::VmTabPage MainWindow::FindVmTab(std::wstring const& guid)
    {
        if (guid.empty()) return nullptr;
        auto tv = tabView();
        if (!tv) return nullptr;
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            auto page = item.Tag().try_as<winrt::hyprv_app::VmTabPage>();
            if (!page) continue;
            std::wstring g{ page.VmGuid() };
            if (_wcsicmp(g.c_str(), guid.c_str()) == 0) return page;
        }
        return nullptr;
    }

    winrt::hyprv_app::VmTabPage MainWindow::ActiveTab()
    {
        auto tv = tabView();
        if (!tv) return nullptr;
        auto sel = tv.SelectedItem().try_as<Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (!sel) return nullptr;
        // TabViewItem.Content is intentionally null (the page is mounted in
        // tabContentHost, not the item). The page is stashed as Tag instead.
        if (auto page = sel.Tag().try_as<winrt::hyprv_app::VmTabPage>())
            return page;
        // Fallback for any code path that did set Content.
        return sel.Content().try_as<winrt::hyprv_app::VmTabPage>();
    }

    winrt::hyprv_app::VmTabPage MainWindow::OpenVmTab(
        hstring const& vmGuid, hstring const& displayName)
    {
        auto tv = tabView();
        if (!tv) return nullptr;

        // Bump this VM in the recents MRU regardless of whether we end up
        // focusing an existing tab or creating a new one — every open is a
        // user-driven "I was just looking at this VM" signal. The 10-item cap
        // is enforced inside BumpRecent.
        hyprv::app::settings::Settings::Instance().BumpRecent(std::wstring{ vmGuid });

        // Already open? Focus it and return.
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            auto page = item.Tag().try_as<winrt::hyprv_app::VmTabPage>();
            if (!page) continue;
            if (_wcsicmp(std::wstring{ page.VmGuid() }.c_str(),
                         std::wstring{ vmGuid }.c_str()) == 0)
            {
                tv.SelectedItem(item);
                return page;
            }
        }

        // Build a fresh page + tab item.
        winrt::hyprv_app::VmTabPage page;
        auto pageImpl = winrt::get_self<implementation::VmTabPage>(page);
        pageImpl->Initialize(vmGuid);
        pageImpl->SetWindowHwnd(m_windowHwnd);

        Microsoft::UI::Xaml::Controls::TabViewItem item;
        item.Header(winrt::box_value(displayName));
        item.IsClosable(true);
        // Square corners (terminal-y flat style — selection is marked by
        // a bottom accent underline, not a rounded fill). Force on the
        // instance so the Win11 default TabViewItem style can't override
        // back to its template's rounded fill.
        item.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 0, 0, 0, 0 });
        Microsoft::UI::Xaml::Controls::SymbolIconSource icon;
        icon.Symbol(Microsoft::UI::Xaml::Controls::Symbol::ViewAll);
        item.IconSource(icon);
        // Stash the page on the item via Tag — the item's Content stays empty
        // (the page is mounted into tabContentHost on selection change).
        item.Tag(page);
        // Right-click the tab header → VM control context menu (start / stop /
        // checkpoint / etc). Same flyout the rail uses. Don't attach on the
        // rdpHost area — right-clicks there must reach the guest.
        item.ContextFlyout(hyprv::app::ui::BuildVmContextMenu(vmGuid, displayName, get_weak()));

        pageImpl->SetTabItem(item);

        tv.TabItems().Append(item);
        tv.SelectedItem(item);

        HyprvAppLog(L"[main] OpenVmTab opened vm=%s", std::wstring{ vmGuid }.c_str());
        PersistOpenTabs();
        return page;
    }

    winrt::hyprv_app::WelcomePage MainWindow::OpenWelcomeTab()
    {
        auto tv = tabView();
        if (!tv) return nullptr;

        // No dedup: the welcome page is intended as a Hyper-V hub, and the
        // user may want multiple instances side-by-side (compare two views,
        // keep one open while drilling into a wizard in another).
        winrt::hyprv_app::WelcomePage page;
        auto pageImpl = winrt::get_self<implementation::WelcomePage>(page);
        pageImpl->SetMainWindow(get_weak());

        // Tab label = the host's machine name. Frames the welcome tab as
        // "the portal to this host" — once remote-host support lands, each
        // remote gets its own welcome tab labeled with the remote's
        // hostname, and the '+' button gains a dropdown to pick local vs
        // remote. Devices glyph matches the framing (it's a *host*, not a
        // VM). GetComputerNameW returns the NetBIOS form, which is what
        // users recognise on Windows + caps at 15 chars (fits in a tab).
        wchar_t hostBuf[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD hostLen = ARRAYSIZE(hostBuf);
        winrt::hstring header;
        if (GetComputerNameW(hostBuf, &hostLen) && hostLen > 0)
            header = winrt::hstring{ hostBuf };
        else
            header = winrt::hstring{ L"Local host" };

        Microsoft::UI::Xaml::Controls::TabViewItem item;
        item.Header(winrt::box_value(header));
        item.IsClosable(true);
        // Square corners — selection is marked by a bottom accent
        // underline (see TabView.Resources in MainWindow.xaml). Forced on
        // the instance so the Win11 default style can't override.
        item.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 0, 0, 0, 0 });
        // Devices glyph (Segoe MDL2 E977) — laptop + tower. Reads as
        // "your machines" which matches the welcome page's role as the
        // entry point to every VM on the host.
        Microsoft::UI::Xaml::Controls::FontIconSource icon;
        icon.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe MDL2 Assets" });
        icon.Glyph(L"\xE977");
        item.IconSource(icon);
        item.Tag(page);

        // Wire the back-pointer BEFORE appending — the page needs the
        // item handle to request close-self via replace-on-open. Set
        // after Tag so the item is otherwise fully configured.
        pageImpl->SetTabItem(item);

        tv.TabItems().Append(item);
        tv.SelectedItem(item);

        HyprvAppLog(L"[main] OpenWelcomeTab opened");
        PersistOpenTabs();
        return page;
    }

    winrt::hyprv_app::AppSettingsPage MainWindow::OpenAppSettingsTab()
    {
        auto tv = tabView();
        if (!tv) return nullptr;

        // Dedup — application settings is a single global surface. If a
        // settings tab is already in the strip, just focus it. The user's
        // existing search filter / scroll position survives the focus.
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            auto existing = item.Tag().try_as<winrt::hyprv_app::AppSettingsPage>();
            if (!existing) continue;
            tv.SelectedItem(item);
            return existing;
        }

        // Otherwise build a fresh page + tab.
        winrt::hyprv_app::AppSettingsPage page;
        // Wire the back-pointer BEFORE Tag-stash + Append so the page's
        // appearance callbacks have the window when the user touches
        // anything. Same pattern as WelcomePage::SetMainWindow.
        auto pageImpl = winrt::get_self<implementation::AppSettingsPage>(page);
        pageImpl->SetMainWindow(get_weak());

        Microsoft::UI::Xaml::Controls::TabViewItem item;
        item.Header(winrt::box_value(winrt::hstring{ L"Settings" }));
        item.IsClosable(true);
        item.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 0, 0, 0, 0 });
        // Gear glyph (Segoe MDL2 E713) — same icon as the title-bar
        // button so the tab's origin is unambiguous.
        Microsoft::UI::Xaml::Controls::FontIconSource icon;
        icon.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe MDL2 Assets" });
        icon.Glyph(L"\xE713");
        item.IconSource(icon);
        item.Tag(page);

        tv.TabItems().Append(item);
        tv.SelectedItem(item);

        HyprvAppLog(L"[main] OpenAppSettingsTab opened");
        PersistOpenTabs();
        return page;
    }

    // Drive the New VM wizard through a coroutine whose state is all by-value
    // PARAMETERS, never lambda captures. An immediately-invoked capturing lambda
    // coroutine dangles its captures after the first co_await (CLAUDE.md gotcha
    // #19) — the earlier form AV'd in OpenVmTab because the captured MainWindow
    // com_ptr dangled once `co_await dlg.ShowAsync()` suspended. `window`
    // (com_ptr) keeps the window alive across the await; `dlg`/`impl` are
    // coroutine-frame locals, so they survive too. Mirrors ShowConfirmCoro.
    static winrt::fire_and_forget ShowNewVmDialogCoro(
        winrt::com_ptr<MainWindow> window,
        Microsoft::UI::Xaml::XamlRoot root,
        Microsoft::UI::Xaml::ElementTheme theme)
    {
        winrt::hyprv_app::NewVmDialog dlg;
        dlg.XamlRoot(root);
        dlg.RequestedTheme(theme);
        auto impl = winrt::get_self<implementation::NewVmDialog>(dlg);
        // The mstscax popup of any running VM tab paints above the XAML
        // composition surface; suppress popups while the modal is up.
        implementation::PopupSuppressionScope suppress(window.get());
        auto result = co_await dlg.ShowAsync();
        if (result == Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary)
        {
            std::wstring guid = impl->CreatedVmGuid();
            std::wstring name = impl->CreatedVmName();
            if (!guid.empty())
                window->OpenVmTab(winrt::hstring{ guid }, winrt::hstring{ name });
        }
    }

    void MainWindow::OpenNewVmDialog()
    {
        // Same root + theme capture as the VM settings dialog (a ContentDialog
        // renders in its own popup layer and doesn't inherit the window's
        // RequestedTheme automatically — see VmTileFactory's "Settings..." path).
        auto root = this->Content() ? this->Content().XamlRoot() : nullptr;
        auto parentTheme = Microsoft::UI::Xaml::ElementTheme::Default;
        if (auto fe = this->Content().try_as<Microsoft::UI::Xaml::FrameworkElement>())
            parentTheme = fe.ActualTheme();
        // get_strong() keeps the window alive; passed by value into the
        // coroutine frame (NOT captured in a lambda — see ShowNewVmDialogCoro).
        ShowNewVmDialogCoro(get_strong(), root, parentTheme);
    }

    void MainWindow::OnAddTabButtonClick(
        Microsoft::UI::Xaml::Controls::TabView const&,
        Windows::Foundation::IInspectable const&)
    {
        OpenWelcomeTab();
    }

    // ---- Tab tear-out (legacy drag/drop model) ------------------------------
    // Decide on mouse-RELEASE, with no window pre-created mid-drag: drop a tab
    // onto another window's strip → move it there (TabStripDrop); drop outside
    // any strip → a new window (TabDroppedOutside). The TabViewItem — and the
    // live page + RDP session in its Tag — moves INTACT across windows (single
    // UI thread); we only re-wire the page to its new owner.

    namespace
    {
        // The tab currently being dragged (set on TabDragStarting, consumed by
        // TabStripDrop / TabDroppedOutside, cleared on TabDragCompleted). A live
        // same-process handoff — the page can't be serialized through a
        // DataPackage. Single UI thread ⇒ no synchronization.
        Microsoft::UI::Xaml::Controls::TabViewItem g_dragItem{ nullptr };

        // Walk up the visual tree from a TabViewItem to its hosting TabView.
        Microsoft::UI::Xaml::Controls::TabView ParentTabView(
            Microsoft::UI::Xaml::DependencyObject obj)
        {
            while (obj)
            {
                if (auto tv = obj.try_as<Microsoft::UI::Xaml::Controls::TabView>())
                    return tv;
                obj = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(obj);
            }
            return nullptr;
        }

        // Detach a page from whatever ContentControl currently hosts it (the
        // source window's tabContentHost) before the destination mounts it — a
        // XAML element can have only one parent. Walks up from the page to its
        // hosting ContentControl and clears its Content. No-op if unmounted.
        void UnmountFromCurrentHost(Windows::Foundation::IInspectable const& page)
        {
            auto obj = page.try_as<Microsoft::UI::Xaml::DependencyObject>();
            if (!obj) return;
            while (obj)
            {
                obj = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(obj);
                if (auto cc = obj.try_as<Microsoft::UI::Xaml::Controls::ContentControl>())
                {
                    if (cc.Content() == page) cc.Content(nullptr);
                    return;
                }
            }
        }

        // The MainWindow impl that owns a given TabView (or null) — used to fix up
        // the SOURCE window after a cross-window drop.
        implementation::MainWindow* OwningWindowOf(
            Microsoft::UI::Xaml::Controls::TabView const& tv)
        {
            if (!tv) return nullptr;
            for (auto const& w : hyprv::app::ui::WindowManager::All())
            {
                if (auto mw = w.try_as<winrt::hyprv_app::MainWindow>())
                {
                    auto impl = winrt::get_self<implementation::MainWindow>(mw);
                    if (impl->tabView() == tv) return impl;
                }
            }
            return nullptr;
        }
    }

    MainWindow* MainWindow::WindowUnderPoint(POINT pt)
    {
        // The topmost window under the cursor (WindowFromPoint respects z-order).
        // Deliberately do NOT resolve the popup OWNER: a release over a VM tab's
        // rdphost surface (a separate cross-process window covering the body) must
        // NOT count as "over its main window" — dragging onto the RDP content is a
        // BODY release (tear out / break away), not a move-into-this-window.
        HWND hit  = WindowFromPoint(pt);
        HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
        if (!root) return nullptr;
        for (auto const& w : hyprv::app::ui::WindowManager::All())
        {
            auto mw = w.try_as<winrt::hyprv_app::MainWindow>();
            if (!mw) continue;
            auto impl = winrt::get_self<implementation::MainWindow>(mw);
            HWND h = impl->m_windowHwnd;
            if (!h || h != root) continue;
            // Only the title-bar / tab-strip BAND (row 0) is a move-in target. The
            // body below it — XAML content OR a child-hosted rdphost surface — is
            // not, so restrict by Y: a drop on the content tears out / breaks away
            // rather than snapping in. Band height = the TabView's row-0 height.
            POINT clientTop{ 0, 0 };
            ClientToScreen(h, &clientTop);
            UINT dpi = GetDpiForWindow(h);
            double scale = dpi > 0 ? dpi / 96.0 : 1.0;
            double bandDip = 48.0;
            if (auto tv = impl->tabView(); tv && tv.ActualHeight() > 0)
                bandDip = tv.ActualHeight();
            LONG bandPx = static_cast<LONG>(bandDip * scale + 0.5);
            if (pt.y >= clientTop.y && pt.y < clientTop.y + bandPx) return impl;
            return nullptr;   // over this window's body
        }
        return nullptr;
    }

    void MainWindow::AdoptMovedTab(
        Microsoft::UI::Xaml::Controls::TabViewItem const& item)
    {
        if (!item) return;
        // The framework may move the tab (TabTearOutRequested) BEFORE the new
        // window's OnActivated has run, so resolve our HWND now (idempotent) —
        // SetWindowHwnd below re-owns the popup to it.
        ResolveWindowHwnd();
        auto tag = item.Tag();
        // Release the page from its old host before THIS window mounts it.
        UnmountFromCurrentHost(tag);

        if (auto vm = tag.try_as<winrt::hyprv_app::VmTabPage>())
        {
            auto impl = winrt::get_self<implementation::VmTabPage>(vm);
            impl->SetWindowHwnd(m_windowHwnd);     // re-owns the rdphost popup
            impl->SetTabItem(item);
            std::wstring guid{ vm.VmGuid() };
            auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(guid);
            std::wstring name = vmOpt ? vmOpt->elementName : std::wstring{};
            item.ContextFlyout(hyprv::app::ui::BuildVmContextMenu(
                winrt::hstring{ guid }, winrt::hstring{ name }, get_weak()));
        }
        else if (auto rh = tag.try_as<winrt::hyprv_app::RemoteHostTabPage>())
        {
            auto impl = winrt::get_self<implementation::RemoteHostTabPage>(rh);
            impl->SetWindowHwnd(m_windowHwnd);     // re-owns the rdphost popup
            impl->SetTabItem(item);
            item.ContextFlyout(hyprv::app::ui::BuildRemoteHostContextMenu(
                rh.HostAddress(), get_weak()));
        }
        else if (auto wp = tag.try_as<winrt::hyprv_app::WelcomePage>())
        {
            auto impl = winrt::get_self<implementation::WelcomePage>(wp);
            impl->SetMainWindow(get_weak());
            impl->SetTabItem(item);
        }
        else if (auto sp = tag.try_as<winrt::hyprv_app::AppSettingsPage>())
        {
            winrt::get_self<implementation::AppSettingsPage>(sp)->SetMainWindow(get_weak());
            // Settings is a singleton surface per window. If this window already
            // had a Settings tab, close the OLD one (keep the moved one).
            if (auto tv = tabView())
            {
                for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
                {
                    auto other = tv.TabItems().GetAt(i).try_as<
                        Microsoft::UI::Xaml::Controls::TabViewItem>();
                    if (!other || other == item) continue;
                    if (other.Tag().try_as<winrt::hyprv_app::AppSettingsPage>())
                    {
                        CloseTabItem(other);
                        break;
                    }
                }
            }
        }

        // Select it — this window's OnTabSelectionChanged mounts the page into
        // tabContentHost + shows the popup at this window's coords (deferring if
        // the new window hasn't laid out yet, via the page's m_deferredShow).
        if (auto tv = tabView())
            tv.SelectedItem(item);
        PersistOpenTabs();   // primary-only (secondary no-ops)
        HyprvAppLog(L"[tear] AdoptMovedTab into hwnd=%p", m_windowHwnd);
    }

    void MainWindow::ShutdownAllTabClients()
    {
        auto tv = tabView();
        if (!tv) return;
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            if (auto vm = item.Tag().try_as<winrt::hyprv_app::VmTabPage>())
                winrt::get_self<implementation::VmTabPage>(vm)->ShutdownClient();
            else if (auto rh = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>())
                winrt::get_self<implementation::RemoteHostTabPage>(rh)->ShutdownClient();
        }
    }

    void MainWindow::HandleSourceAfterDetach()
    {
        auto tv = tabView();
        if (!tv) return;
        if (tv.TabItems().Size() != 0)
        {
            PersistOpenTabs();   // strip changed (primary persists; secondary no-ops)
            return;
        }
        // Strip empty after the tab left.
        if (m_isPrimary)
        {
            HyprvAppLog(L"[tear] source primary emptied -> welcome");
            OpenWelcomeTab();
        }
        else
        {
            HyprvAppLog(L"[tear] source secondary emptied -> close");
            this->Close();
        }
    }

    namespace
    {
        // Tab header text (diagnostic logging only).
        std::wstring TabHeaderText(Microsoft::UI::Xaml::Controls::TabViewItem const& item)
        {
            if (!item) return L"(null)";
            return std::wstring{ winrt::unbox_value_or<winrt::hstring>(item.Header(), L"") };
        }
    }

    void MainWindow::MoveTabToThisWindow(
        Microsoft::UI::Xaml::Controls::TabViewItem const& item, int index)
    {
        auto destTv = tabView();
        if (!item || !destTv) return;
        auto srcTv = ParentTabView(item);
        if (srcTv == destTv) return;   // already ours → native reorder, nothing to do
        auto srcWin = srcTv ? OwningWindowOf(srcTv) : nullptr;
        if (srcTv)
        {
            uint32_t idx;
            if (srcTv.TabItems().IndexOf(item, idx))
                srcTv.TabItems().RemoveAt(idx);
        }
        uint32_t count = destTv.TabItems().Size();
        if (index < 0 || static_cast<uint32_t>(index) > count)
            destTv.TabItems().Append(item);
        else
            destTv.TabItems().InsertAt(static_cast<uint32_t>(index), item);
        AdoptMovedTab(item);   // re-own popup / re-point weak_ref + select
        if (srcWin && srcWin != this) srcWin->HandleSourceAfterDetach();
        HyprvAppLog(L"[tear] moved tab '%s' into this window", TabHeaderText(item).c_str());
    }

    void MainWindow::SpawnWindowForTab(
        Microsoft::UI::Xaml::Controls::TabViewItem const& item)
    {
        if (!item) return;
        // Detach from the item's REAL source (not necessarily `this` — a body drop
        // over another window fires that window's OnWindowDrop).
        auto srcTv  = ParentTabView(item);
        auto srcWin = srcTv ? OwningWindowOf(srcTv) : nullptr;
        // Don't tear out a window's ONLY tab — breaking it away just recreates the
        // same window in a new place (obs #2). Leave it where it is. (Moving a lone
        // tab INTO another window is still honoured — that goes through
        // MoveTabToThisWindow, not here.) If the loose release was over ANOTHER
        // window's body, that window grabbed focus during the drag — restore the
        // source window's foreground so focus doesn't jump (obs #1).
        if (srcTv && srcTv.TabItems().Size() <= 1)
        {
            if (srcWin && srcWin->m_windowHwnd)
            {
                HWND sh = srcWin->m_windowHwnd;
                m_uiQueue.TryEnqueue(
                    Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                    [sh] { BringWindowToTop(sh); SetForegroundWindow(sh); });
            }
            HyprvAppLog(L"[tear] ignoring tear-out of the last remaining tab");
            return;
        }
        if (srcTv)
        {
            uint32_t idx;
            if (srcTv.TabItems().IndexOf(item, idx))
                srcTv.TabItems().RemoveAt(idx);
        }
        POINT cur{}; GetCursorPos(&cur);
        int w = 1000, h = 700;   // match the source window's size for a natural feel
        HWND srcHwnd = srcWin ? srcWin->m_windowHwnd : m_windowHwnd;
        if (srcHwnd)
        {
            RECT r{};
            if (GetWindowRect(srcHwnd, &r)) { w = r.right - r.left; h = r.bottom - r.top; }
        }
        auto win = winrt::make<MainWindow>();
        auto newImpl = winrt::get_self<MainWindow>(win);
        newImpl->MarkSecondary();
        newImpl->SetPendingAdopt(item);   // adopted in the new window's OnActivated
        hyprv::app::ui::WindowManager::Track(win);
        Microsoft::UI::Xaml::Window asWin = win;
        if (auto aw = asWin.AppWindow())
            aw.MoveAndResize(winrt::Windows::Graphics::RectInt32{ cur.x, cur.y, w, h });
        asWin.Activate();   // OnActivated adopts the pending tab once tabView() exists
        // Bring the torn-out window to the foreground. Activate() alone (and even a
        // synchronous SetForegroundWindow here) loses the race: removing the tab from
        // the source re-selects/focuses the source AFTER us, so it ends up on top.
        // Assert foreground again on a LOW-priority turn — last to touch focus wins.
        HWND nh = nullptr;
        if (auto native = asWin.try_as<::IWindowNative>())
            native->get_WindowHandle(&nh);
        if (nh)
        {
            BringWindowToTop(nh);
            SetForegroundWindow(nh);
            m_uiQueue.TryEnqueue(
                Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                [nh] { BringWindowToTop(nh); SetForegroundWindow(nh); });
        }
        if (srcWin) srcWin->HandleSourceAfterDetach();
        HyprvAppLog(L"[tear] tore tab '%s' into a new window at %d,%d",
            TabHeaderText(item).c_str(), cur.x, cur.y);
    }

    Microsoft::UI::Xaml::Controls::TabViewItem MainWindow::TabUnderCursor()
    {
        auto tv = tabView();
        if (!tv || !m_windowHwnd) return nullptr;
        POINT pt{};
        if (!GetCursorPos(&pt)) return nullptr;
        POINT cli = pt;
        ScreenToClient(m_windowHwnd, &cli);
        UINT dpi = GetDpiForWindow(m_windowHwnd);
        double scale = dpi > 0 ? (dpi / 96.0) : 1.0;
        double cx = cli.x / scale, cy = cli.y / scale;   // client DIP
        auto root = this->Content();
        if (!root) return nullptr;
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto it = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!it) continue;
            auto tr = it.TransformToVisual(root);
            auto r  = tr.TransformBounds(winrt::Windows::Foundation::Rect{
                0.0f, 0.0f,
                static_cast<float>(it.ActualWidth()),
                static_cast<float>(it.ActualHeight()) });
            if (cx >= r.X && cx <= r.X + r.Width && cy >= r.Y && cy <= r.Y + r.Height)
                return it;
        }
        return nullptr;
    }

    void MainWindow::OnTabDragStarting(
        Microsoft::UI::Xaml::Controls::TabView const&,
        Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs const& args)
    {
        // Identify the grabbed tab by HIT-TESTING the cursor — args.Tab is
        // unreliable with the page-in-Tag model (it reports the first tab). NO
        // window is created; placement is decided on release.
        g_dragItem = TabUnderCursor();
        if (!g_dragItem) g_dragItem = args.Tab();   // fallback
        args.Data().Properties().Insert(L"hyprvTab", winrt::box_value(true));
        args.Data().RequestedOperation(
            winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
        HyprvAppLog(L"[tear] DragStarting tab='%s' (args.Tab='%s')",
            TabHeaderText(g_dragItem).c_str(), TabHeaderText(args.Tab()).c_str());
    }

    // Tidy the drag feedback while a tab drag is accepted over a window: ALWAYS
    // keep the dragged-tab visual (its name) shown so the user can see what's
    // moving, but drop the operation glyph + caption text ("the icon hanging
    // above the tab"). The OS still draws its own no-drop cursor over the bare
    // desktop — that part isn't ours to override.
    static void SuppressDragUi(Microsoft::UI::Xaml::DragEventArgs const& e)
    {
        if (auto o = e.DragUIOverride())
        {
            o.IsContentVisible(true);    // keep showing the dragged tab name
            o.IsGlyphVisible(false);
            o.IsCaptionVisible(false);
        }
    }

    void MainWindow::OnTabViewDrop(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::DragEventArgs const& e)
    {
        auto item = g_dragItem;
        if (!item) return;
        auto destTv = sender.try_as<Microsoft::UI::Xaml::Controls::TabView>();
        if (!destTv) { g_dragItem = nullptr; return; }
        // Same-window drop = a reorder — leave it to the native CanReorderTabs
        // machinery (don't consume the event or it won't reorder).
        if (ParentTabView(item) == destTv) { g_dragItem = nullptr; return; }
        // Insert index from the drop X over the destination's existing tabs;
        // a drop over the empty strip/footer (no tab to the right) → append (-1).
        int index = -1;
        for (uint32_t i = 0; i < destTv.TabItems().Size(); ++i)
        {
            auto cont = destTv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!cont) continue;
            auto pos = e.GetPosition(cont);
            if (pos.X - cont.ActualWidth() < 0) { index = static_cast<int>(i); break; }
        }
        g_dragItem = nullptr;
        e.Handled(true);                    // don't also bubble to OnWindowDrop
        // Defer the reparent. Mutating TabItems() (or spawning a window) WHILE the
        // TabView's drag state machine is still unwinding (Drop → DragCompleted are
        // both still on the stack) wedges the control: the drag adorner is left
        // painted on-screen and no further drag can start. Post it to the next pump
        // turn, after the drag has fully torn down. (item is captured strong, so it
        // survives the gap even though it's mid-move.)
        auto weakSelf = get_weak();
        m_uiQueue.TryEnqueue([weakSelf, item, index] {
            if (auto self = weakSelf.get())
                self->MoveTabToThisWindow(item, index);   // top-bar drop = move here
        });
    }

    void MainWindow::OnWindowDragOver(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::DragEventArgs const& e)
    {
        // Accept the drag over the whole window so the cursor reads "move" off the
        // strip too (and suppress the feedback glyph that hangs above the tab).
        if (g_dragItem)
        {
            e.AcceptedOperation(
                winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
            SuppressDragUi(e);
        }
    }

    void MainWindow::OnWindowDrop(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::DragEventArgs const& e)
    {
        auto item = g_dragItem;
        if (!item) return;
        g_dragItem = nullptr;
        e.Handled(true);                     // consume — don't also bubble to OnTabViewDrop
        // Fires for the title-bar FOOTER (the empty area beside the tabs, row 0)
        // and the window BODY (row 1). Row 0 = move into THIS window (append);
        // the BODY = tear out a new window. (Drops precisely over the tabs go to
        // OnTabViewDrop instead, which sets Handled so we don't reach here.)
        double y = 1.0e9;
        if (auto rg = rootGrid()) y = e.GetPosition(rg).Y;
        bool moveHere = (y < 32.0);          // top bar → move here; body → new window
        // Deferred — see OnTabViewDrop: never restructure during the drag teardown.
        auto weakSelf = get_weak();
        m_uiQueue.TryEnqueue([weakSelf, item, moveHere] {
            auto self = weakSelf.get();
            if (!self) return;
            if (moveHere)
                self->MoveTabToThisWindow(item, -1);
            else
                self->SpawnWindowForTab(item);
        });
    }

    void MainWindow::OnTabDragCompleted(
        Microsoft::UI::Xaml::Controls::TabView const&,
        Microsoft::UI::Xaml::Controls::TabViewTabDragCompletedEventArgs const& args)
    {
        auto item = g_dragItem;
        g_dragItem = nullptr;
        if (!item) return;

        // DropResult Move => a real OLE drop already handled it (OnTabViewDrop over
        // the tabs/strip, or OnWindowDrop over the body), or it was a same-window
        // reorder. Nothing left to do — and this is what avoids re-tearing a tab
        // that was just dropped or reordered.
        if (args.DropResult() ==
            winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move)
            return;

        // DropResult None => released where no OLE drop target accepted it. That's
        // either a window's TITLE BAR (a Win32 caption — OLE can't drop onto a
        // non-client area, which is also why the OS draws a no-drop cursor there)
        // or the bare desktop. Route by what's under the cursor at release.
        POINT cur{}; GetCursorPos(&cur);
        auto target = WindowUnderPoint(cur);
        auto srcTv  = ParentTabView(item);
        auto srcWin = srcTv ? OwningWindowOf(srcTv) : nullptr;

        // Defer the restructure to the next pump turn — mutating TabItems() / making
        // a window WHILE the drag is still unwinding wedges the control (the drag
        // adorner is left painted and no further drag can start).
        if (target && target != srcWin)
        {
            // Over ANOTHER hyprv window (its title bar) → move into it, at the end.
            // Honoured even for a lone tab: it's an explicit move into a real window
            // (the source then empties → closes if secondary / shows welcome if
            // primary), NOT a tear-out into a brand-new window (obs #1).
            auto weakTarget = target->get_weak();
            m_uiQueue.TryEnqueue([weakTarget, item] {
                if (auto self = weakTarget.get())
                    self->MoveTabToThisWindow(item, -1);
            });
        }
        else if (!target)
        {
            // Over the bare desktop / a foreign window → tear out a new window.
            // SpawnWindowForTab ignores a window's only tab (it would just recreate
            // the same window) and restores the source window's foreground so a
            // loose release doesn't steal focus (obs #1/#2).
            auto weakSelf = get_weak();
            m_uiQueue.TryEnqueue([weakSelf, item] {
                if (auto self = weakSelf.get())
                    self->SpawnWindowForTab(item);
            });
        }
        // else target == srcWin → released over the source window's own title bar:
        // leave the tab where it is (a lone tab stays put; the source keeps focus).
    }

    // ---- Remote hosts -------------------------------------------------------

    winrt::hyprv_app::RemoteHostTabPage MainWindow::OpenRemoteHostTab(
        hstring const& address, bool autoConnect)
    {
        auto tv = tabView();
        if (!tv) return nullptr;
        std::wstring addr{ address };
        if (addr.empty()) return nullptr;

        // MRU bump — a user-initiated open is a "I just connected to this host"
        // signal, so it surfaces in the welcome RECENT row (kind=Remote). Skip
        // it for a restore (autoConnect=false) — we didn't actively open it.
        if (autoConnect)
            hyprv::app::settings::Settings::Instance().BumpRecent(
                addr, hyprv::app::settings::RecentKind::Remote);

        // Already open? Focus it.
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            auto page = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>();
            if (!page) continue;
            if (_wcsicmp(std::wstring{ page.HostAddress() }.c_str(), addr.c_str()) == 0)
            {
                tv.SelectedItem(item);
                return page;
            }
        }

        // Resolve a display name for the tab header.
        std::wstring display = addr;
        if (auto h = hyprv::app::settings::Settings::Instance().FindRemoteHost(addr))
            display = h->name.empty() ? h->address : h->name;

        winrt::hyprv_app::RemoteHostTabPage page;
        auto pageImpl = winrt::get_self<implementation::RemoteHostTabPage>(page);
        pageImpl->Initialize(address);
        pageImpl->SetAutoConnect(autoConnect);
        pageImpl->SetWindowHwnd(m_windowHwnd);

        Microsoft::UI::Xaml::Controls::TabViewItem item;
        item.Header(winrt::box_value(winrt::hstring{ display }));
        item.IsClosable(true);
        item.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 0, 0, 0, 0 });
        Microsoft::UI::Xaml::Controls::FontIconSource icon;
        icon.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe MDL2 Assets" });
        icon.Glyph(L"\xE8AF");   // Remote
        item.IconSource(icon);
        item.Tag(page);
        item.ContextFlyout(hyprv::app::ui::BuildRemoteHostContextMenu(address, get_weak()));

        pageImpl->SetTabItem(item);

        tv.TabItems().Append(item);
        tv.SelectedItem(item);

        HyprvAppLog(L"[main] OpenRemoteHostTab opened %s", addr.c_str());
        PersistOpenTabs();
        return page;
    }

    // Coroutine whose state is all by-value PARAMETERS (gotcha #19 — never
    // lambda captures across a co_await). `window` keeps the window alive;
    // `wasAdd` decides whether to open the tab after a save (add => connect now;
    // edit => just refresh the rail). Mirrors ShowNewVmDialogCoro.
    static winrt::fire_and_forget ShowRemoteHostDialogCoro(
        winrt::com_ptr<MainWindow> window,
        Microsoft::UI::Xaml::XamlRoot root,
        Microsoft::UI::Xaml::ElementTheme theme,
        winrt::hstring address,
        bool wasAdd)
    {
        winrt::hyprv_app::RemoteHostDialog dlg;
        dlg.XamlRoot(root);
        dlg.RequestedTheme(theme);
        auto impl = winrt::get_self<implementation::RemoteHostDialog>(dlg);
        impl->InitializeForEdit(address);
        implementation::PopupSuppressionScope suppress(window.get());
        auto result = co_await dlg.ShowAsync();
        if (result == Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary)
        {
            std::wstring saved{ impl->SavedAddress() };
            if (!saved.empty())
            {
                window->RenderRemoteHostsRail();
                if (wasAdd)
                    window->OpenRemoteHostTab(winrt::hstring{ saved });
                else if (impl->ReconnectRecommended())
                    // An edit that changed a connection-affecting field —
                    // reconnect the host's open tab so it applies live.
                    window->ReconnectRemoteHostTab(saved);
            }
        }
    }

    void MainWindow::OpenRemoteHostDialog(hstring const& address)
    {
        auto root = this->Content() ? this->Content().XamlRoot() : nullptr;
        auto parentTheme = Microsoft::UI::Xaml::ElementTheme::Default;
        if (auto fe = this->Content().try_as<Microsoft::UI::Xaml::FrameworkElement>())
            parentTheme = fe.ActualTheme();
        const bool wasAdd = std::wstring{ address }.empty();
        ShowRemoteHostDialogCoro(get_strong(), root, parentTheme, address, wasAdd);
    }

    void MainWindow::ForgetRemoteHost(hstring const& address)
    {
        std::wstring addr{ address };
        if (addr.empty()) return;
        // Close any open tab for this host first (teardown its rdphost child).
        if (auto tv = tabView())
        {
            for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
            {
                auto item = tv.TabItems().GetAt(i).try_as<
                    Microsoft::UI::Xaml::Controls::TabViewItem>();
                if (!item) continue;
                auto page = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>();
                if (page && _wcsicmp(std::wstring{ page.HostAddress() }.c_str(),
                                     addr.c_str()) == 0)
                {
                    CloseTabItem(item);
                    break;
                }
            }
        }
        auto& settings = hyprv::app::settings::Settings::Instance();
        settings.RemoveRemoteHost(addr);
        settings.ForgetRecent(addr);   // drop it from the welcome RECENT row too
        RenderRemoteHostsRail();
        HyprvAppLog(L"[main] ForgetRemoteHost %s", addr.c_str());
    }

    void MainWindow::ReconnectRemoteHostTab(std::wstring const& address)
    {
        if (address.empty()) return;
        auto tv = tabView();
        if (!tv) return;
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            auto page = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>();
            if (page && _wcsicmp(std::wstring{ page.HostAddress() }.c_str(),
                                 address.c_str()) == 0)
            {
                auto impl = winrt::get_self<
                    winrt::hyprv_app::implementation::RemoteHostTabPage>(page);
                impl->ReapplyConnectionSettings();
                return;
            }
        }
    }

    void MainWindow::RenderRemoteHostsRail()
    {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        auto list = remoteHostsList();
        if (!list) return;

        auto hosts = hyprv::app::settings::Settings::Instance().RemoteHosts();
        list.Items().Clear();
        auto weakWindow = get_weak();

        for (auto const& h : hosts)
        {
            std::wstring display = h.name.empty() ? h.address : h.name;
            winrt::hstring addrH{ h.address };

            Grid row;
            row.HorizontalAlignment(HorizontalAlignment::Stretch);
            ColumnDefinition c0, c1;
            c0.Width(GridLengthHelper::FromValueAndType(16, GridUnitType::Pixel));
            c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);

            FontIcon glyph;
            glyph.FontFamily(Media::FontFamily{ L"Segoe MDL2 Assets" });
            glyph.Glyph(L"\xE8AF");
            glyph.FontSize(11);
            glyph.VerticalAlignment(VerticalAlignment::Center);
            glyph.Margin({ 0, 0, 4, 0 });
            Grid::SetColumn(glyph, 0);
            row.Children().Append(glyph);

            TextBlock tb;
            tb.Text(winrt::hstring{ display });
            tb.FontSize(12);
            tb.VerticalAlignment(VerticalAlignment::Center);
            tb.TextTrimming(TextTrimming::CharacterEllipsis);
            Grid::SetColumn(tb, 1);
            row.Children().Append(tb);

            // ListViewItem (not a Border) so single-click selection uses the
            // SAME native highlight as the VM rail. DOUBLE click connects; the
            // ListView's own selection handles the highlight on single click.
            // Mirrors CreateRailItem (the VM rail item builder).
            ListViewItem lvi;
            lvi.Content(row);
            lvi.Tag(winrt::box_value(addrH));
            lvi.Padding({ 8, 0, 8, 0 });
            lvi.MinHeight(26);
            lvi.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            lvi.ContextFlyout(hyprv::app::ui::BuildRemoteHostContextMenu(addrH, weakWindow));
            lvi.DoubleTapped([addrH, weakWindow](
                winrt::Windows::Foundation::IInspectable const&,
                Input::DoubleTappedRoutedEventArgs const& e)
            {
                e.Handled(true);
                if (auto win = weakWindow.get()) win->OpenRemoteHostTab(addrH);
            });
            ToolTipService::SetToolTip(lvi, winrt::box_value(addrH));
            list.Items().Append(lvi);
        }

        if (auto exp = remoteHostsExpander())
            exp.Visibility(hosts.empty() ? Visibility::Collapsed : Visibility::Visible);
    }

    void MainWindow::OnRemoteRailSelectionChanged(IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e)
    {
        if (e.AddedItems().Size() == 0) return;   // a deselect, not a select
        // Single selection across the rail — a remote selection drops the VM
        // list highlight (clearing it fires OnRailVmSelected with no AddedItems,
        // which returns early, so there's no feedback loop).
        if (auto vl = hvVmList())
            if (vl.SelectedItem()) vl.SelectedItem(nullptr);
    }

    void MainWindow::OnTabSelectionChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        // Hide every tab's popup, then show the active one. Doing this in
        // two passes avoids a flash where the new popup briefly overlaps
        // the old one (Win32 z-order isn't deterministic mid-swap).
        auto tv = tabView();
        if (!tv) return;

        // The active tab can be either a VmTabPage or a WelcomePage —
        // discriminate via try_as. Welcome tabs have no popup, so the
        // hide-other-popups pass naturally skips them.
        auto activeItem = tv.SelectedItem().try_as<
            Microsoft::UI::Xaml::Controls::TabViewItem>();
        Windows::Foundation::IInspectable activeTag =
            activeItem ? activeItem.Tag() : nullptr;
        auto activeVm      = activeTag.try_as<winrt::hyprv_app::VmTabPage>();
        auto activeWelcome = activeTag.try_as<winrt::hyprv_app::WelcomePage>();
        auto activeRemote  = activeTag.try_as<winrt::hyprv_app::RemoteHostTabPage>();

        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            // Both VM tabs and remote-host tabs own an mstscax popup that must
            // be hidden when not the active tab.
            if (auto page = item.Tag().try_as<winrt::hyprv_app::VmTabPage>())
            {
                if (page == activeVm) continue;
                winrt::get_self<implementation::VmTabPage>(page)->HidePopup();
            }
            else if (auto rpage = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>())
            {
                if (rpage == activeRemote) continue;
                winrt::get_self<implementation::RemoteHostTabPage>(rpage)->HidePopup();
            }
        }

        // Mount the active tab's content into the window-level host. Both
        // page types are UIElements, so the tag (IInspectable) drops in
        // directly. Clear when no active tab.
        if (auto host = tabContentHost())
        {
            host.Content(activeTag ? activeTag
                                   : Windows::Foundation::IInspectable{ nullptr });
        }

        if (activeVm)
        {
            auto impl = winrt::get_self<implementation::VmTabPage>(activeVm);
            impl->ShowPopup();
            // Sync the flyout to the active tab's VM if the flyout is open.
            // Gate on IsFlyoutOpen (column width) instead of
            // !m_flyoutVmGuid.empty() — the guid can be empty even with the
            // flyout open (e.g. user opened it while a welcome tab was
            // active), and the old gate left subsequent tab switches stuck
            // on stale data.
            if (IsFlyoutOpen())
            {
                m_flyoutVmGuid = std::wstring{ activeVm.VmGuid() };
                UpdateInfoFlyoutContent();
            }
        }
        else if (activeRemote)
        {
            // Remote host tab — surface its popup. No VM, so clear the flyout
            // if it happens to be open (empty beats stale VM data).
            auto impl = winrt::get_self<implementation::RemoteHostTabPage>(activeRemote);
            impl->ShowPopup();
            if (IsFlyoutOpen())
            {
                m_flyoutVmGuid.clear();
                UpdateInfoFlyoutContent();
            }
        }
        else if (activeWelcome)
        {
            // Refresh recents — the user may have opened a VM since this
            // welcome tab was last in view, and BumpRecent updates settings
            // out of band.
            auto impl = winrt::get_self<implementation::WelcomePage>(activeWelcome);
            impl->OnTabActivated();
            // The welcome tab itself doesn't represent a single VM; if the
            // flyout is open, mirror whichever VM the welcome page has
            // selected (or clear it if nothing's selected — empty sections
            // beats stale data from the previous tab).
            if (IsFlyoutOpen())
            {
                m_flyoutVmGuid = impl->SelectedGuid();
                UpdateInfoFlyoutContent();
            }
        }
        else
        {
            // No active tab — clear the flyout if it's open.
            if (IsFlyoutOpen())
            {
                m_flyoutVmGuid.clear();
                UpdateInfoFlyoutContent();
            }
        }
        SyncRailSelectionToActive();
        UpdateDebuggerButton();   // enabled state follows the active VM
        // Selection change is a persistable event — restore on next
        // launch should land on the same tab the user left active.
        PersistOpenTabs();
    }

    void MainWindow::OnTabCloseRequested(
        Microsoft::UI::Xaml::Controls::TabView const&,
        Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& args)
    {
        // X-button close path. Same teardown semantics as the welcome
        // page's replace-on-open gesture — both go through CloseTabItem.
        CloseTabItem(args.Tab());
    }

    void MainWindow::ReplaceTabWith(
        Microsoft::UI::Xaml::Controls::TabViewItem const& toClose,
        hstring const& vmGuid, hstring const& displayName)
    {
        // Build (or focus) the VM tab. OpenVmTab appends + sets
        // SelectedItem(newTab); for the "already open" case it just
        // focuses the existing tab and returns its page.
        auto newPage = OpenVmTab(vmGuid, displayName);
        if (!newPage)
        {
            HyprvAppLog(L"[main] ReplaceTabWith: OpenVmTab returned null");
            return;
        }

        // Close the old tab. WinUI TabView dispatches SelectionChanged
        // for SelectedItem changes asynchronously; this means the
        // selection-revert-on-remove fallback can fire AFTER our select
        // call, leaving the new VM tab appended-but-not-selected.
        CloseTabItem(toClose);

        // Force the selection onto the new VM tab as a safety net. Walk
        // TabItems to find the TabViewItem whose Tag matches newPage and
        // SelectedItem-set it. Idempotent if selection is already correct.
        auto tv = tabView();
        if (!tv) return;
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto ti = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!ti) continue;
            auto page = ti.Tag().try_as<winrt::hyprv_app::VmTabPage>();
            if (page && page == newPage)
            {
                if (tv.SelectedItem() != ti)
                {
                    HyprvAppLog(L"[main] ReplaceTabWith: forcing selection to new VM tab");
                    tv.SelectedItem(ti);
                }
                else
                {
                    HyprvAppLog(L"[main] ReplaceTabWith: selection already on new tab");
                }
                break;
            }
        }
    }

    void MainWindow::ReplaceTabWithRemoteHost(
        Microsoft::UI::Xaml::Controls::TabViewItem const& toClose,
        hstring const& address)
    {
        auto newPage = OpenRemoteHostTab(address);   // append + select (or focus existing)
        if (!newPage)
        {
            HyprvAppLog(L"[main] ReplaceTabWithRemoteHost: OpenRemoteHostTab returned null");
            return;
        }
        CloseTabItem(toClose);
        auto tv = tabView();
        if (!tv) return;
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto ti = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!ti) continue;
            auto page = ti.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>();
            if (page && page == newPage)
            {
                if (tv.SelectedItem() != ti) tv.SelectedItem(ti);
                break;
            }
        }
    }

    void MainWindow::CloseTabItem(
        Microsoft::UI::Xaml::Controls::TabViewItem const& item)
    {
        if (!item) return;
        auto tv = tabView();
        if (!tv) return;

        // Capture the closed-tab type BEFORE teardown — Tag is cleared
        // mid-method and the post-removal "what spawned next?" decision
        // depends on knowing what just left.
        bool wasVmTab      = item.Tag().try_as<winrt::hyprv_app::VmTabPage>()  != nullptr;
        bool wasWelcomeTab = item.Tag().try_as<winrt::hyprv_app::WelcomePage>() != nullptr;
        bool wasRemoteTab  = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>() != nullptr;

        // VM-tab-specific teardown: kill the rdphost child before removing
        // the tab. Welcome tabs have no popup / no child process so the
        // VmTabPage branch is skipped naturally via try_as.
        if (auto page = item.Tag().try_as<winrt::hyprv_app::VmTabPage>())
        {
            auto impl = winrt::get_self<implementation::VmTabPage>(page);
            impl->HidePopup();
            HyprvAppLog(L"[main] closing tab vm=%s",
                std::wstring{ page.VmGuid() }.c_str());
            // Tear down the rdphost child deterministically NOW — don't wait
            // for ~VmTabPage. TabView's internal item cache often retains the
            // projection past RemoveAt, which previously orphaned rdphost.exe
            // until the parent process exited.
            impl->ShutdownClient();
        }
        else if (auto rpage = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>())
        {
            // Same deterministic rdphost teardown for a remote-host tab.
            auto impl = winrt::get_self<implementation::RemoteHostTabPage>(rpage);
            impl->HidePopup();
            HyprvAppLog(L"[main] closing remote tab %s",
                std::wstring{ rpage.HostAddress() }.c_str());
            impl->ShutdownClient();
        }
        else if (wasWelcomeTab)
        {
            HyprvAppLog(L"[main] closing welcome tab");
        }

        // Common teardown — applies to every tab type. If this was the
        // currently-mounted content, detach so the projection releases; then
        // drop the item's strong ref so the TabView's item cache doesn't
        // keep the projection alive past close.
        Windows::Foundation::IInspectable tag = item.Tag();
        if (auto host = tabContentHost())
        {
            if (host.Content() == tag)
                host.Content(Windows::Foundation::IInspectable{ nullptr });
        }
        item.Tag(Windows::Foundation::IInspectable{ nullptr });

        uint32_t idx = 0;
        if (tv.TabItems().IndexOf(item, idx))
            tv.TabItems().RemoveAt(idx);

        // Empty-tab-strip resolution: VM tab closed last → drop user back
        // on the welcome screen (home base); every other last-tab close
        // (welcome, settings) is the user signalling "I'm done", so close
        // the window. The window close gesture (X button on the title bar)
        // still works for the VM-tab-active case too.
        //
        // Originally only welcome tabs triggered the window close — when
        // the app settings tab was the last one open, closing it left the
        // window with an empty tab strip and no obvious way out. Treat
        // any non-VM last-tab close the same way.
        if (tv.TabItems().Size() == 0)
        {
            if (wasVmTab || wasRemoteTab)
            {
                // A session tab (VM or remote host) closed last → drop back to
                // the welcome hub rather than exiting.
                HyprvAppLog(L"[main] last session tab closed -- opening welcome");
                OpenWelcomeTab();
            }
            else
            {
                HyprvAppLog(L"[main] last non-session tab closed -- closing window");
                this->Close();
            }
        }
        else
        {
            // Persist the post-close strip. OpenWelcomeTab above already
            // persists via its own call site, so skip in that branch.
            PersistOpenTabs();
        }
    }

    void MainWindow::OnSettingsClick(IInspectable const&, RoutedEventArgs const&)
    {
        OpenAppSettingsTab();
    }

    // ---- VMManager dispatch -------------------------------------------------

    void MainWindow::OnVmManagerChanged()
    {
        RenderRail(hvVmList(), get_weak());
        SyncRailSelectionToActive();
        UpdateInfoFlyoutContent();
        // Cheap; catches debugger feature-toggle / per-VM args changes within
        // ~1s without needing a tab switch.
        UpdateDebuggerButton();
        // Record positive enhanced-session observations into Settings so the
        // context menu can correctly grey the toggle even when the VM is
        // currently off (Hyper-V only reports EnhancedSessionModeState while
        // the VM is running with LIS up). Sticky-true; we never write false
        // from here — boot-screen / no-LIS transient negatives would
        // otherwise grey a perfectly capable VM.
        {
            auto snapshot = hyprv::app::vm::VMManager::Instance().GetAll();
            auto& s = hyprv::app::settings::Settings::Instance();
            for (auto const& vm : snapshot)
            {
                if (vm.enhancedSessionAvailable)
                    s.ObserveEnhancedSupport(vm.guid);
            }
        }
        // Auto-close any VM tab whose VM has been deleted (it vanished from a
        // completed snapshot — whether deleted from hyprv or externally). We
        // collect first and close after: CloseTabItem mutates TabItems, so
        // closing mid-iteration would invalidate the loop. Gated on
        // HasFirstSnapshot so a not-yet-polled cold start doesn't close restored
        // tabs; UpdateAll only publishes a fresh list on a SUCCESSFUL
        // GetSummaryInformation (it returns early on failure), so a missing VM
        // here is a real delete, not a transient empty snapshot. CloseTabItem
        // keeps the usual close semantics (last VM tab -> a welcome tab).
        if (auto tv = tabView();
            tv && hyprv::app::vm::VMManager::Instance().HasFirstSnapshot())
        {
            std::vector<Microsoft::UI::Xaml::Controls::TabViewItem> toClose;
            for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
            {
                auto item = tv.TabItems().GetAt(i).try_as<
                    Microsoft::UI::Xaml::Controls::TabViewItem>();
                if (!item) continue;
                auto vmPage = item.Tag().try_as<winrt::hyprv_app::VmTabPage>();
                if (!vmPage) continue;
                std::wstring guid{ vmPage.VmGuid() };
                if (!hyprv::app::vm::VMManager::Instance().GetByGuid(guid))
                    toClose.push_back(item);
            }
            for (auto const& item : toClose)
            {
                HyprvAppLog(L"[main] auto-closing tab for deleted VM");
                CloseTabItem(item);
            }
        }

        // Ping every open page so it can react to VM rename / state changes:
        //   - VmTabPage: tab header text + placeholder vs rdphost.
        //   - WelcomePage: recents + all-VMs lists.
        if (auto tv = tabView())
        {
            for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
            {
                auto item = tv.TabItems().GetAt(i).try_as<
                    Microsoft::UI::Xaml::Controls::TabViewItem>();
                if (!item) continue;
                if (auto vmPage = item.Tag().try_as<winrt::hyprv_app::VmTabPage>())
                {
                    auto impl = winrt::get_self<implementation::VmTabPage>(vmPage);
                    impl->OnVmManagerChanged();
                    // Rebuild the per-tab context menu so its state-gated
                    // items (Start/Pause/Save/Restart/Reset/Turn off/Delete,
                    // canSendKeys-coupled items, etc.) reflect the current
                    // VM state. Rail items + welcome tiles get this for
                    // free because their entire host control is rebuilt on
                    // every OnVmManagerChanged. The per-tab ContextFlyout
                    // is set ONCE at OpenVmTab time, and the menu.Opening
                    // hook only refreshes the enhanced-session toggle and
                    // the two send-keys items — every other state-gated
                    // item would otherwise stay frozen at whatever the VM
                    // state was when the tab was first opened.
                    std::wstring guid{ vmPage.VmGuid() };
                    auto vmOpt =
                        hyprv::app::vm::VMManager::Instance().GetByGuid(guid);
                    std::wstring name = vmOpt ? vmOpt->elementName : std::wstring{};
                    item.ContextFlyout(hyprv::app::ui::BuildVmContextMenu(
                        winrt::hstring{ guid },
                        winrt::hstring{ name },
                        get_weak()));
                }
                else if (auto wp = item.Tag().try_as<winrt::hyprv_app::WelcomePage>())
                {
                    auto impl = winrt::get_self<implementation::WelcomePage>(wp);
                    impl->OnVmManagerChanged();
                }
            }
        }
    }

    void MainWindow::SyncRailSelectionToActive()
    {
        // Keep the rail's selection highlight aligned with the active tab's
        // VM. Setting SelectedItem fires SelectionChanged, but OnRailVmSelected
        // calls OpenVmTab which is idempotent for an already-open VM (focuses
        // the existing tab), so the call is cheap.
        auto list = hvVmList();
        if (!list) return;
        auto active = ActiveTab();
        if (!active)
        {
            // No active tab — clear the rail selection so nothing's
            // highlighted while there's no tab to track. Single-click
            // never opens a tab, so we don't need the old "force re-fire"
            // workaround for the previously-selected item.
            if (list.SelectedItem()) list.SelectedItem(nullptr);
            return;
        }
        std::wstring guid{ active.VmGuid() };
        for (uint32_t i = 0; i < list.Items().Size(); ++i)
        {
            auto lvi = list.Items().GetAt(i).try_as<Microsoft::UI::Xaml::Controls::ListViewItem>();
            if (!lvi) continue;
            auto g = winrt::unbox_value_or<winrt::hstring>(lvi.Tag(), L"");
            if (_wcsicmp(std::wstring{ g }.c_str(), guid.c_str()) == 0)
            {
                // Only update if not already the selection — keeps the
                // SelectionChanged event from re-firing on every tab swap.
                if (list.SelectedItem() != lvi)
                    list.SelectedItem(lvi);
                return;
            }
        }
        // Active tab's VM isn't in the rail (e.g. just deleted) — clear.
        if (list.SelectedItem()) list.SelectedItem(nullptr);
    }

    // ---- Rail selection (single-click) -------------------------------------
    // Single click on a rail VM just selects the row — it does NOT open a tab
    // anymore. Double-click (wired in CreateRailItem) is the tab-open gesture.
    // SyncRailSelectionToActive drives selection from active tab, and this
    // hook is kept as the place to wire future single-click side effects
    // (e.g. drive the flyout's tracked VM independent of the open tab).
    void MainWindow::OnRailVmSelected(IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e)
    {
        if (e.AddedItems().Size() == 0) return;
        auto item = e.AddedItems().GetAt(0).try_as<Microsoft::UI::Xaml::Controls::ListViewItem>();
        if (!item) return;
        auto tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), L"");
        if (tag.empty()) return;
        // Single selection across the rail — a VM selection drops the remote
        // list highlight (the remote ListView's deselect fires
        // OnRemoteRailSelectionChanged with no AddedItems → returns early).
        if (auto rl = remoteHostsList())
            if (rl.SelectedItem()) rl.SelectedItem(nullptr);
        HyprvAppLog(L"[ui] rail VM selected guid=%s (single-click — no tab open)",
            std::wstring{ tag }.c_str());
    }

    // ---- Title-bar buttons → rail / flyout toggle --------------------------

    void MainWindow::OnHamburgerClick(IInspectable const&, RoutedEventArgs const&)
    {
        ToggleRail();
    }

    void MainWindow::OnInfoClick(IInspectable const&, RoutedEventArgs const&)
    {
        ToggleInfoFlyout();
    }

    void MainWindow::OnDebuggerClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto active = ActiveTab();
        if (!active) return;
        std::wstring guid{ active.VmGuid() };
        // The detached-spawn + exe/args resolution lives in the shared helper
        // (also used by the context-menu item).
        unsigned long err = hyprv::app::ui::LaunchVmDebugger(guid);
        if (err != 0)
        {
            auto& s = hyprv::app::settings::Settings::Instance();
            HyprvAppLog(L"[main] debugger launch failed err=%lu", err);
            hyprv::app::ui::ShowErrorDialog(get_weak(),
                std::wstring{ L"Couldn't launch the debugger" },
                L"Failed to start \"" + s.EffectiveDebuggerExe(guid)
                + L"\" (error " + std::to_wstring(err) +
                L").\n\nCheck the debugger in App Settings → Debugger and the "
                L"arguments in this VM's Debugger settings.");
        }
    }

    void MainWindow::UpdateDebuggerButton()
    {
        auto btn = debugBtn();
        if (!btn) return;
        auto& s = hyprv::app::settings::Settings::Instance();
        const bool feature = s.DebuggerEnabled();
        btn.Visibility(feature ? Microsoft::UI::Xaml::Visibility::Visible
                               : Microsoft::UI::Xaml::Visibility::Collapsed);
        if (!feature) return;
        // "Armed" = active tab is a VM whose debugger args are set. Rather than
        // IsEnabled (which paints the chunky grey disabled background in the
        // title bar), convey the inactive state by dimming the glyph + turning
        // off hit-testing (no hover, no click) — the transparent chrome stays
        // intact, only the icon desaturates.
        bool armed = false;
        if (auto active = ActiveTab())
            armed = !s.VmDebuggerArgs(std::wstring{ active.VmGuid() }).empty();
        btn.IsHitTestVisible(armed);
        btn.Opacity(armed ? 1.0 : 0.4);
    }

    void MainWindow::ToggleRail()
    {
        auto col = railColumn();
        auto split = splitterColumn();
        if (!col || !split) return;
        const bool open = col.Width().Value > 0;
        if (open)
        {
            m_railSavedWidth = col.Width().Value > 0 ? col.Width().Value : m_railSavedWidth;
            col.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                0, Microsoft::UI::Xaml::GridUnitType::Pixel));
            split.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                0, Microsoft::UI::Xaml::GridUnitType::Pixel));
            HyprvAppLog(L"[ui] rail collapsed (saved width=%.0f)", m_railSavedWidth);
        }
        else
        {
            col.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                m_railSavedWidth, Microsoft::UI::Xaml::GridUnitType::Pixel));
            split.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                6, Microsoft::UI::Xaml::GridUnitType::Pixel));
            HyprvAppLog(L"[ui] rail expanded to %.0f", m_railSavedWidth);
        }
        // Persist the new visibility — without this, hamburger toggles
        // would only stick until the next splitter-drag-driven persist.
        PersistGeometry();
    }

    void MainWindow::ToggleInfoFlyout()
    {
        auto col = flyoutColumn();
        auto split = flyoutSplitterColumn();
        if (!col || !split) return;
        const bool open = col.Width().Value > 0;

        if (open)
        {
            m_flyoutSavedWidth = col.Width().Value > 0 ? col.Width().Value
                                                       : m_flyoutSavedWidth;
            col.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                0, Microsoft::UI::Xaml::GridUnitType::Pixel));
            split.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                0, Microsoft::UI::Xaml::GridUnitType::Pixel));
            m_flyoutVmGuid.clear();
            HyprvAppLog(L"[flyout] closed (saved width=%.0f)", m_flyoutSavedWidth);
        }
        else
        {
            // Open for the active surface's VM:
            //   - VM tab: that tab's VM
            //   - Welcome tab: the welcome page's selected row (empty if
            //     nothing selected)
            //   - No active tab: empty
            m_flyoutVmGuid.clear();
            if (auto tv = tabView())
            {
                auto activeItem = tv.SelectedItem().try_as<
                    Microsoft::UI::Xaml::Controls::TabViewItem>();
                if (activeItem)
                {
                    if (auto vmPage = activeItem.Tag().try_as<winrt::hyprv_app::VmTabPage>())
                    {
                        m_flyoutVmGuid = std::wstring{ vmPage.VmGuid() };
                    }
                    else if (auto welcome = activeItem.Tag().try_as<winrt::hyprv_app::WelcomePage>())
                    {
                        auto impl = winrt::get_self<implementation::WelcomePage>(welcome);
                        m_flyoutVmGuid = impl->SelectedGuid();
                    }
                }
            }
            col.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                m_flyoutSavedWidth, Microsoft::UI::Xaml::GridUnitType::Pixel));
            split.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                6, Microsoft::UI::Xaml::GridUnitType::Pixel));
            UpdateInfoFlyoutContent();
            HyprvAppLog(L"[flyout] open for %s width=%.0f",
                m_flyoutVmGuid.c_str(), m_flyoutSavedWidth);
        }
        // Persist the new visibility — the info button toggle path needs
        // an explicit save (splitter-drag persist won't fire for a click).
        PersistGeometry();
    }

    // ---- Info flyout content ----------------------------------------------

    bool MainWindow::IsFlyoutOpen() const
    {
        // const_cast: the XAML-generated accessors aren't marked const,
        // but reading Width().Value is conceptually const. Same pattern
        // the rest of the IsX-checks in this file would need if added.
        auto col = const_cast<MainWindow*>(this)->flyoutColumn();
        return col && col.Width().Value > 0;
    }

    void MainWindow::SetFlyoutVm(std::wstring const& guid)
    {
        if (!IsFlyoutOpen()) return;
        if (m_flyoutVmGuid == guid) return;     // no-op
        m_flyoutVmGuid = guid;
        UpdateInfoFlyoutContent();
    }

    void MainWindow::ApplyAppearance()
    {
        auto a = hyprv::app::settings::Settings::Instance().AppearancePref();

        // Theme → effective tint resolution.
        //
        // Mica and Acrylic are layered atop the user's desktop wallpaper.
        // TintColor + TintOpacity determine how much that wallpaper bleeds
        // through. Black mode is now just "Dark theme with pure-black
        // tint" instead of a backdrop-off special case — so users can
        // stack Black + Mica + intensity (e.g. intensity=1.0 gives an
        // OLED-friendly opaque black via the controller without disabling
        // the backdrop entirely). Light / Dark / System pick their stock
        // tint values; Black overrides to pure black.
        bool isDark;
        bool useBlackTint = (a.theme == hyprv::app::settings::Appearance::Theme::Black);
        switch (a.theme)
        {
        case hyprv::app::settings::Appearance::Theme::Light:
            isDark = false; break;
        case hyprv::app::settings::Appearance::Theme::Dark:
        case hyprv::app::settings::Appearance::Theme::Black:
            isDark = true;  break;
        default:
            isDark = (Microsoft::UI::Xaml::Application::Current().RequestedTheme()
                      == Microsoft::UI::Xaml::ApplicationTheme::Dark);
            break;
        }

        // Black uses RGB(8,8,8) rather than pure 0,0,0 — the acrylic
        // blend math gives a flat result at pure black (no luminosity
        // information for the blur to mix), so a near-black grey actually
        // renders darker overall.
        auto blackTint = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x08, 0x08, 0x08);
        auto darkTint  = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x2C, 0x2C, 0x2C);
        auto lightTint = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xFC, 0xFC, 0xFC);
        auto tintColor = useBlackTint ? blackTint : (isDark ? darkTint : lightTint);
        auto fallbackColor = tintColor;
        // LuminosityOpacity: 0 = bright/vibrant wallpaper, 1 = fully
        // dampened. Higher = darker output. Black mode pegs it at 1.0
        // so the underlying wallpaper luminosity can't brighten the
        // result back up.
        float luminosity = useBlackTint ? 1.0f : (isDark ? 0.96f : 0.85f);

        // Backdrop application.
        //
        // Mica and Acrylic both go through their respective controllers
        // (MicaController / DesktopAcrylicController) — the lower-level
        // SystemBackdrops API — so we can pin IsInputActive=true (acrylic
        // stays on when window deactivates) AND expose TintOpacity to
        // the user via the App Settings page sliders. The built-in
        // wrapper backdrops (MicaBackdrop / DesktopAcrylicBackdrop)
        // offer neither.
        try
        {
            // Always tear down any previously-active controllers first;
            // we'll re-create below for whichever backdrop applies.
            if (m_acrylicController)
            {
                if (auto target = this->try_as<
                        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>())
                    m_acrylicController.RemoveSystemBackdropTarget(target);
                m_acrylicController.Close();
                m_acrylicController = nullptr;
            }
            if (m_micaController)
            {
                if (auto target = this->try_as<
                        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>())
                    m_micaController.RemoveSystemBackdropTarget(target);
                m_micaController.Close();
                m_micaController = nullptr;
            }
            m_backdropConfig = nullptr;

            // Clear any prior built-in SystemBackdrop — we manage
            // everything via controllers now, regardless of theme.
            SystemBackdrop(nullptr);

            if (auto target = this->try_as<
                    winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>())
            {
                using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
                m_backdropConfig = SystemBackdropConfiguration{};
                m_backdropConfig.IsInputActive(true);
                m_backdropConfig.Theme(isDark ? SystemBackdropTheme::Dark
                                              : SystemBackdropTheme::Light);

                if (a.backdrop == hyprv::app::settings::Appearance::Backdrop::Mica)
                {
                    m_micaController = MicaController{};
                    m_micaController.TintColor       (tintColor);
                    m_micaController.FallbackColor   (fallbackColor);
                    m_micaController.LuminosityOpacity(luminosity);
                    m_micaController.TintOpacity     (static_cast<float>(a.micaTintOpacity));
                    m_micaController.SetSystemBackdropConfiguration(m_backdropConfig);
                    m_micaController.AddSystemBackdropTarget(target);
                }
                else /* Acrylic */
                {
                    m_acrylicController = DesktopAcrylicController{};
                    m_acrylicController.TintColor       (tintColor);
                    m_acrylicController.FallbackColor   (fallbackColor);
                    m_acrylicController.LuminosityOpacity(luminosity);
                    m_acrylicController.TintOpacity     (static_cast<float>(a.acrylicTintOpacity));
                    m_acrylicController.SetSystemBackdropConfiguration(m_backdropConfig);
                    m_acrylicController.AddSystemBackdropTarget(target);
                }
            }
        }
        catch (...) { /* swallow */ }

        // Theme. RequestedTheme on the root XAML element cascades live —
        // existing controls re-resolve {ThemeResource Foo} brushes on the
        // next layout pass. Black is "Dark + black background brush" — the
        // brush override applies via Application.Resources but, like the
        // accent-color note in Open questions #3, doesn't repaint
        // already-resolved StaticResource lookups. Good enough for next
        // launch and for any newly-rendered tab content.
        if (auto root = Content().try_as<Microsoft::UI::Xaml::FrameworkElement>())
        {
            using winrt::Microsoft::UI::Xaml::ElementTheme;
            switch (a.theme)
            {
            case hyprv::app::settings::Appearance::Theme::System:
                root.RequestedTheme(ElementTheme::Default); break;
            case hyprv::app::settings::Appearance::Theme::Light:
                root.RequestedTheme(ElementTheme::Light); break;
            case hyprv::app::settings::Appearance::Theme::Dark:
            case hyprv::app::settings::Appearance::Theme::Black:
                root.RequestedTheme(ElementTheme::Dark); break;
            }
        }

        // Popup background brushes.
        //
        // Two categories of popup chrome:
        //   - SystemBackdrop-capable: FlyoutBase (incl. MenuFlyout)
        //     gets a backdrop instance per-popup via PopupBackdropFor.
        //     Those follow the user's Mica/Acrylic choice cleanly,
        //     even in Black mode (the window backdrop's controller is
        //     using the black tint, and the FlyoutBase backdrop picks
        //     up the same general look from the framework defaults).
        //   - SystemBackdrop-incapable: ContentDialog and ComboBox
        //     dropdown (popup-internal). Both paint with their
        //     respective theme brush — ContentDialogBackground and
        //     ComboBoxDropDownBackground. In Black mode we override
        //     those to opaque RGB(8,8,8) so dialogs / dropdowns match
        //     the window's near-black appearance. In other themes the
        //     framework's stock dark/light value applies.
        auto resources = Microsoft::UI::Xaml::Application::Current().Resources();
        auto setKey = [&](winrt::hstring key,
                          Microsoft::UI::Xaml::Media::Brush const& brush) {
            if (resources.HasKey(winrt::box_value(key)))
                resources.Remove(winrt::box_value(key));
            resources.Insert(winrt::box_value(key), brush);
        };
        auto clearKey = [&](winrt::hstring key) {
            if (resources.HasKey(winrt::box_value(key)))
                resources.Remove(winrt::box_value(key));
        };
        const wchar_t* solidPopupKeys[] = {
            L"ContentDialogBackground",
            L"ComboBoxDropDownBackground",
        };
        if (useBlackTint)
        {
            // Dialogs + ComboBox dropdowns can't take a backdrop in
            // this SDK, so paint them opaque near-black.
            Microsoft::UI::Xaml::Media::SolidColorBrush nearBlack;
            nearBlack.Color(winrt::Windows::UI::ColorHelper::FromArgb(
                0xFF, 0x08, 0x08, 0x08));
            for (auto k : solidPopupKeys) setKey(k, nearBlack);

            // MenuFlyoutPresenterBackground is the brush that renders
            // ABOVE the menu's SystemBackdrop. Painting it
            // semi-transparent near-black darkens the acrylic instead
            // of replacing it — Black mode menus then read distinctly
            // darker than Dark mode menus while keeping the frosted
            // texture. Alpha 0xA0 (~63%) was picked empirically: dark
            // enough to read as Black, light enough to leave the
            // acrylic visible underneath.
            Microsoft::UI::Xaml::Media::SolidColorBrush menuOverlay;
            menuOverlay.Color(winrt::Windows::UI::ColorHelper::FromArgb(
                0xA0, 0x08, 0x08, 0x08));
            setKey(L"MenuFlyoutPresenterBackground", menuOverlay);
        }
        else if (isDark)
        {
            // Dark (non-Black) mode: the framework's default
            // MenuFlyoutPresenterBackground is near-opaque dark gray,
            // which hides the menu's SystemBackdrop and makes the menu
            // read as a flat solid block over Mica/Acrylic. Paint a
            // semi-transparent dark overlay — same idea as Black mode
            // but using the same RGB(0x2C,0x2C,0x2C) tint the window's
            // controller is using, so the menu visually matches the
            // window chrome while letting Mica's wallpaper-derived tint
            // bleed through. Alpha tuned to match Black-mode legibility.
            Microsoft::UI::Xaml::Media::SolidColorBrush menuOverlay;
            menuOverlay.Color(winrt::Windows::UI::ColorHelper::FromArgb(
                0xA0, 0x2C, 0x2C, 0x2C));
            setKey(L"MenuFlyoutPresenterBackground", menuOverlay);
            for (auto k : solidPopupKeys) clearKey(k);
        }
        else
        {
            for (auto k : solidPopupKeys) clearKey(k);
            clearKey(L"MenuFlyoutPresenterBackground");
        }

        // rootGrid Background — a TRANSPARENT brush (NOT null/ClearValue) so the
        // window-level backdrop still bleeds through AND the grid is hit-testable
        // across its whole area. The tab tear-out drag needs the window body /
        // empty title-bar to be a drop target; a null background isn't hit-
        // testable, so the OS would show a no-drop cursor over those regions.
        // Black mode no longer paints opaque black here; the backdrop controllers
        // (pure-black tint + LuminosityOpacity 1.0 + user TintOpacity) do that.
        if (auto grid = rootGrid())
            grid.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{
                winrt::Windows::UI::Colors::Transparent() });

        // Caption button (min / max / close) colours follow the theme.
        // These are drawn by the OS via AppWindow.TitleBar, NOT by XAML,
        // so they don't pick up RequestedTheme — we have to push the
        // colours explicitly. Without this, the buttons stay white-on-
        // transparent on Light theme and end up invisible.
        try
        {
            auto root = Content().try_as<Microsoft::UI::Xaml::FrameworkElement>();
            using winrt::Microsoft::UI::Xaml::ElementTheme;
            bool isLight;
            switch (a.theme)
            {
            case hyprv::app::settings::Appearance::Theme::Light: isLight = true;  break;
            case hyprv::app::settings::Appearance::Theme::Dark:
            case hyprv::app::settings::Appearance::Theme::Black: isLight = false; break;
            default:
                // System — rely on the root's ActualTheme which reflects
                // the OS app-mode setting via Default propagation.
                isLight = root && root.ActualTheme() == ElementTheme::Light;
                break;
            }
            Microsoft::UI::WindowId windowId{ reinterpret_cast<uint64_t>(m_windowHwnd) };
            auto appWindow = Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
            if (appWindow)
            {
                auto tb = appWindow.TitleBar();
                auto fg          = isLight
                    ? winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x00, 0x00, 0x00)
                    : winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xFF, 0xFF, 0xFF);
                auto fgInactive  = isLight
                    ? winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x60, 0x60, 0x60)
                    : winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x9E, 0x9E, 0x9E);
                tb.ButtonForegroundColor(fg);
                tb.ButtonHoverForegroundColor(fg);
                tb.ButtonPressedForegroundColor(fg);
                tb.ButtonInactiveForegroundColor(fgInactive);
                // Backgrounds stay transparent so the caption blends
                // with the backdrop. Hover/pressed get a subtle tint
                // appropriate for the theme.
                auto transparent = winrt::Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0);
                tb.ButtonBackgroundColor(transparent);
                tb.ButtonInactiveBackgroundColor(transparent);
                auto hover = isLight
                    ? winrt::Windows::UI::ColorHelper::FromArgb(0x18, 0x00, 0x00, 0x00)
                    : winrt::Windows::UI::ColorHelper::FromArgb(0x18, 0xFF, 0xFF, 0xFF);
                tb.ButtonHoverBackgroundColor(hover);
                tb.ButtonPressedBackgroundColor(hover);
            }
        }
        catch (...) { /* swallow — caption colours are cosmetic */ }

        HyprvAppLog(L"[main] ApplyAppearance backdrop=%u theme=%u",
            static_cast<unsigned>(a.backdrop), static_cast<unsigned>(a.theme));
    }

    void MainWindow::UpdateBackdropTintOpacity(double opacity)
    {
        // Fast path for the intensity sliders. Calling the full
        // ApplyAppearance on every ValueChanged tick tears down and
        // rebuilds the controller, which is expensive enough that a
        // slider drag stutters / doesn't feel live. Setting TintOpacity
        // directly on the existing controller is essentially free —
        // the composition layer picks up the new value on the next
        // frame.
        const float v = static_cast<float>(
            opacity < 0.0 ? 0.0 : (opacity > 1.0 ? 1.0 : opacity));
        if (m_acrylicController) m_acrylicController.TintOpacity(v);
        else if (m_micaController) m_micaController.TintOpacity(v);
        // Popup backdrops are MicaBackdrop / DesktopAcrylicBackdrop
        // wrappers set per-dialog (see ui/PopupBackdrop.cpp). Those
        // don't expose TintOpacity — they always use the framework
        // defaults — so the slider only affects the main window's
        // controller. Acceptable trade-off: the window is where the
        // user sees the slider's effect; popups stay consistent with
        // the system backdrop family.
    }

    void MainWindow::PushPopupSuppression()
    {
        // 0 -> 1 edge: hide every open VmTabPage's popup. Higher counts
        // are no-ops at the popup level — nested dialogs (e.g. settings
        // dialog spawning a confirm-discard dialog) share one suppression.
        if (m_popupSuppressionDepth++ != 0) return;
        auto tv = tabView();
        if (!tv) return;
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            if (auto page = item.Tag().try_as<winrt::hyprv_app::VmTabPage>())
                winrt::get_self<implementation::VmTabPage>(page)->SetPopupSuppressed(true);
            else if (auto rpage = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>())
                winrt::get_self<implementation::RemoteHostTabPage>(rpage)->SetPopupSuppressed(true);
        }
    }

    void MainWindow::PopPopupSuppression()
    {
        // Mirror of Push. Guard against under-flow in case a scope leaks
        // (defensive — shouldn't happen with the RAII wrapper).
        if (m_popupSuppressionDepth <= 0)
        {
            HyprvAppLog(L"[main] PopPopupSuppression underflow — ignoring");
            return;
        }
        if (--m_popupSuppressionDepth != 0) return;
        auto tv = tabView();
        if (!tv) return;
        for (uint32_t i = 0; i < tv.TabItems().Size(); ++i)
        {
            auto item = tv.TabItems().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!item) continue;
            if (auto page = item.Tag().try_as<winrt::hyprv_app::VmTabPage>())
                winrt::get_self<implementation::VmTabPage>(page)->SetPopupSuppressed(false);
            else if (auto rpage = item.Tag().try_as<winrt::hyprv_app::RemoteHostTabPage>())
                winrt::get_self<implementation::RemoteHostTabPage>(rpage)->SetPopupSuppressed(false);
        }
    }

    void MainWindow::ResetFlyoutSections()
    {
        // Wipe every section below the header. Caller chooses what header
        // text to display (blank for "no selection", informative for
        // "VM not found"). Idempotent.
        if (auto tb = flyoutGeneration()) SetText(tb, L"-");
        if (auto tb = flyoutVcpus())      SetText(tb, L"-");
        if (auto tb = flyoutMemory())     SetText(tb, L"-");
        if (auto tb = flyoutCpuLoad())    SetText(tb, L"-");
        if (auto tb = flyoutUptime())     SetText(tb, L"-");
        if (auto tb = flyoutHeartbeat())  SetText(tb, L"-");
        if (auto tb = flyoutGuestOs())    SetText(tb, L"-");
        if (auto tb = flyoutCreated())    SetText(tb, L"-");
        if (auto tb = flyoutVersion())    SetText(tb, L"-");
        if (auto tb = flyoutGuid())       SetText(tb, L"-");
        if (auto tb = flyoutSecureBoot()) SetText(tb, L"-");
        if (auto tb = flyoutBiosGuid())   SetText(tb, L"-");
        if (auto tb = flyoutConfigPath()) SetText(tb, L"-");
        if (auto tb = flyoutSnapshotPath())SetText(tb, L"-");
        if (auto tb = flyoutMemStartup()) SetText(tb, L"-");
        if (auto tb = flyoutMemDynamic()) SetText(tb, L"-");
        if (auto tb = flyoutMemMin())     SetText(tb, L"-");
        if (auto tb = flyoutMemMax())     SetText(tb, L"-");
        if (auto tb = flyoutMemAssigned())SetText(tb, L"-");
        if (auto tb = flyoutMemDemand())  SetText(tb, L"-");
        if (auto tb = flyoutMemStatus())  SetText(tb, L"-");
        if (auto e = flyoutNetworkExpander())
            e.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        if (auto h = flyoutAdaptersHost())
            h.Children().Clear();
        if (auto e = flyoutStorageExpander())
            e.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        if (auto h = flyoutDisksHost())
            h.Children().Clear();
        if (auto tb = flyoutNotesHeader())
            tb.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        if (auto tb = flyoutNotes())
        {
            SetText(tb, L"");
            tb.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        }
        // Charts + snapshots also need clearing — their per-VM update
        // helpers paint into named hosts so clear those too.
        if (auto h = flyoutCpuChart())    h.Children().Clear();
        if (auto h = flyoutMemoryChart()) h.Children().Clear();
        if (auto tb = flyoutCpuChartLabel())    SetText(tb, L"-");
        if (auto tb = flyoutMemoryChartLabel()) SetText(tb, L"-");
        if (auto lv = flyoutSnapshotList()) lv.Items().Clear();
        if (auto tb = flyoutNoSnapshots())
            tb.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
    }

    void MainWindow::UpdateInfoFlyoutContent()
    {
        if (m_flyoutVmGuid.empty())
        {
            // No VM selected — blank the header (don't write any
            // "(no VM selected)" placeholder; the empty sections are
            // self-explanatory) and wipe every section below so stale
            // data from a previously-shown VM doesn't masquerade.
            if (auto tb = flyoutVmName())  SetText(tb, L"");
            if (auto tb = flyoutVmState()) SetText(tb, L"");
            ResetFlyoutSections();
            return;
        }
        auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(m_flyoutVmGuid);

        if (!vmOpt)
        {
            // VM disappeared while the flyout was open on it. Informative
            // header so the user knows what happened; then reset sections.
            if (auto tb = flyoutVmName())  SetText(tb, L"(VM not found)");
            if (auto tb = flyoutVmState()) SetText(tb, L"");
            ResetFlyoutSections();
            return;
        }
        auto const& vm = *vmOpt;

        if (auto tb = flyoutVmName()) SetText(tb,winrt::hstring{ vm.elementName });
        std::wstring stateLine = vm.statusText.empty() ? StateLabel(vm.state) : vm.statusText;
        if (auto tb = flyoutVmState()) SetText(tb,winrt::hstring{ stateLine });

        if (auto tb = flyoutGeneration())
            SetText(tb,winrt::hstring{ vm.generation.empty() ? L"-" : vm.generation });
        if (auto tb = flyoutVcpus())
            SetText(tb,winrt::hstring{ vm.numProcessors == 0
                ? L"-" : std::to_wstring(vm.numProcessors) });
        if (auto tb = flyoutMemory())
        {
            std::wstring s;
            if (vm.memoryAssignedMb == 0) s = L"-";
            else if (vm.memoryDemandMb == vm.memoryAssignedMb)
                s = FormatMemoryMb(vm.memoryAssignedMb);
            else
                s = FormatMemoryMb(vm.memoryDemandMb) + L" / " +
                    FormatMemoryMb(vm.memoryAssignedMb);
            SetText(tb,winrt::hstring{ s });
        }
        if (auto tb = flyoutCpuLoad())
        {
            std::wstring s = (vm.IsRunning() ? (std::to_wstring(vm.processorLoadPct) + L"%")
                                             : std::wstring{ L"-" });
            SetText(tb,winrt::hstring{ s });
        }
        if (auto tb = flyoutUptime())
            SetText(tb,winrt::hstring{ vm.IsRunning() ? FormatUptime(vm.uptimeMs)
                                                   : std::wstring{ L"-" } });
        if (auto tb = flyoutHeartbeat())
            SetText(tb,winrt::hstring{ HeartbeatLabel(vm.heartbeatState) });
        if (auto tb = flyoutGuestOs())
        {
            std::wstring os;
            if (!vm.kvpOsName.empty())
            {
                os = vm.kvpOsName;
                if (!vm.kvpOsBuildNumber.empty())
                    os += L" (build " + vm.kvpOsBuildNumber + L")";
            }
            else
            {
                os = vm.guestOs.empty() ? L"-" : vm.guestOs;
            }
            SetText(tb,winrt::hstring{ os });
        }
        if (auto tb = flyoutCreated())
            SetText(tb,winrt::hstring{ vm.creationTime
                ? FormatLocalTime(*vm.creationTime) : std::wstring{ L"-" } });
        if (auto tb = flyoutVersion())
            SetText(tb,winrt::hstring{ vm.configVersion.empty() ? L"-" : vm.configVersion });
        if (auto tb = flyoutGuid())
            SetText(tb,winrt::hstring{ vm.guid });

        if (auto tb = flyoutSecureBoot())
        {
            std::wstring sb;
            if (!vm.secureBootEnabled)         sb = L"-";
            else if (vm.generation == L"Generation 1") sb = L"n/a (Gen 1)";
            else                                sb = *vm.secureBootEnabled ? L"On" : L"Off";
            SetText(tb,winrt::hstring{ sb });
        }
        if (auto tb = flyoutBiosGuid())
            SetText(tb,winrt::hstring{ vm.biosGuid.empty() ? L"-" : vm.biosGuid });
        if (auto tb = flyoutConfigPath())
            SetText(tb,winrt::hstring{ vm.configDataRoot.empty() ? L"-" : vm.configDataRoot });
        if (auto tb = flyoutSnapshotPath())
            SetText(tb,winrt::hstring{ vm.snapshotDataRoot.empty() ? L"-" : vm.snapshotDataRoot });

        bool hasNotes = !vm.notes.empty();
        auto vis = hasNotes ? Microsoft::UI::Xaml::Visibility::Visible
                            : Microsoft::UI::Xaml::Visibility::Collapsed;
        if (auto e = flyoutNotesHeader()) e.Visibility(vis);
        if (auto tb = flyoutNotes())
        {
            if (hasNotes) SetText(tb,winrt::hstring{ vm.notes });
        }

        bool hasNet = !vm.kvpFqdn.empty()
                   || !vm.guestIpv4.empty()
                   || !vm.guestIpv6.empty()
                   || !vm.kvpIntegrationServicesVersion.empty();
        auto netVis = hasNet ? Microsoft::UI::Xaml::Visibility::Visible
                             : Microsoft::UI::Xaml::Visibility::Collapsed;
        if (auto e = flyoutNetworkExpander()) e.Visibility(netVis);
        if (hasNet)
        {
            auto dash = [](std::wstring const& s) -> std::wstring {
                return s.empty() ? std::wstring{ L"-" } : s;
            };
            if (auto tb = flyoutHostname())  SetText(tb,winrt::hstring{ dash(vm.kvpFqdn) });
            if (auto tb = flyoutIsVersion()) SetText(tb,winrt::hstring{ dash(vm.kvpIntegrationServicesVersion) });
        }
        UpdateInfoFlyoutAdapters(vm);
        UpdateInfoFlyoutDisks(vm);

        auto memDash = [](uint64_t mb) -> std::wstring {
            return mb == 0 ? std::wstring{ L"-" } : FormatMemoryMb(mb);
        };
        if (auto tb = flyoutMemStartup())  SetText(tb,winrt::hstring{ memDash(vm.memStartupMb) });
        if (auto tb = flyoutMemDynamic())
            SetText(tb,winrt::hstring{ vm.dynamicMemoryEnabled ? L"Enabled" : L"Disabled" });
        if (auto tb = flyoutMemMin())
            SetText(tb,winrt::hstring{ vm.dynamicMemoryEnabled ? memDash(vm.memMinMb)
                                                            : std::wstring{ L"-" } });
        if (auto tb = flyoutMemMax())
            SetText(tb,winrt::hstring{ vm.dynamicMemoryEnabled ? memDash(vm.memMaxMb)
                                                            : std::wstring{ L"-" } });
        if (auto tb = flyoutMemAssigned())
            SetText(tb,winrt::hstring{ vm.IsRunning() ? memDash(vm.memoryAssignedMb)
                                                   : std::wstring{ L"-" } });
        if (auto tb = flyoutMemDemand())
            SetText(tb,winrt::hstring{ vm.IsRunning() ? memDash(vm.memoryDemandMb)
                                                   : std::wstring{ L"-" } });
        if (auto tb = flyoutMemStatus())
        {
            std::wstring s;
            if (!vm.IsRunning() || vm.memoryAssignedMb == 0) s = L"-";
            else if (vm.memoryPressurePct == 0) s = L"-";
            else if (vm.memoryPressurePct > 100) s = L"Warning";
            else if (vm.memoryPressurePct > 80)  s = L"Low";
            else                                  s = L"OK";
            SetText(tb,winrt::hstring{ s });
        }

        UpdateInfoFlyoutCharts(vm);
        UpdateInfoFlyoutSnapshots(vm);
    }

    void MainWindow::UpdateInfoFlyoutCharts(hyprv::app::vm::VirtualMachine const& vm)
    {
        if (auto label = flyoutCpuChartLabel())
        {
            label.Text(winrt::hstring{
                vm.IsRunning()
                    ? std::to_wstring(vm.processorLoadPct) + L"%"
                    : std::wstring{ L"-" } });
        }
        auto cpuGreen = Windows::UI::ColorHelper::FromArgb(0xFF, 0x4C, 0xC2, 0x66);
        DrawSparkline(flyoutCpuChart(), vm.cpuHistoryPct, 100.0, cpuGreen);

        uint32_t maxMem = 1;
        for (auto v : vm.memoryHistoryMb) if (v > maxMem) maxMem = v;
        if (auto label = flyoutMemoryChartLabel())
        {
            label.Text(winrt::hstring{
                vm.IsRunning() ? FormatMemoryMb(vm.memoryDemandMb)
                               : std::wstring{ L"-" } });
        }
        auto memBlue = Windows::UI::ColorHelper::FromArgb(0xFF, 0x4C, 0x8B, 0xF5);
        DrawSparkline(flyoutMemoryChart(), vm.memoryHistoryMb,
                      static_cast<double>(maxMem), memBlue);
    }

    void MainWindow::UpdateInfoFlyoutAdapters(hyprv::app::vm::VirtualMachine const& vm)
    {
        auto host = flyoutAdaptersHost();
        if (!host) return;

        if (vm.networkAdapters.empty())
        {
            host.Children().Clear();
            host.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }
        host.Visibility(Microsoft::UI::Xaml::Visibility::Visible);

        std::unordered_map<std::wstring,
            Microsoft::UI::Xaml::Controls::Border> existing;
        existing.reserve(host.Children().Size());
        for (uint32_t i = 0; i < host.Children().Size(); ++i)
        {
            auto b = host.Children().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::Border>();
            if (!b) continue;
            auto key = winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"");
            existing.emplace(std::wstring{ key }, b);
        }

        std::unordered_set<std::wstring> seen;

        for (uint32_t idx = 0; idx < vm.networkAdapters.size(); ++idx)
        {
            auto const& nic = vm.networkAdapters[idx];
            std::wstring key = nic.macAddress.empty() ? nic.name : nic.macAddress;
            seen.insert(key);

            std::wstring nameLine = nic.name.empty() ? std::wstring{ L"Network Adapter" }
                                                    : nic.name;
            if (!nic.macAddress.empty())
                nameLine += L"  ·  " + FormatMac(nic.macAddress);
            if (nic.dynamicMac) nameLine += L" (dynamic)";

            std::wstring statusText = nic.connected ? L"Connected" : L"Disconnected";

            auto findOrCreateValueTb = [&](
                Microsoft::UI::Xaml::Controls::Grid const& grid,
                int row, std::wstring const& text, bool mono) ->
                Microsoft::UI::Xaml::Controls::TextBlock
            {
                for (uint32_t c = 0; c < grid.Children().Size(); ++c)
                {
                    auto tb = grid.Children().GetAt(c).try_as<
                        Microsoft::UI::Xaml::Controls::TextBlock>();
                    if (!tb) continue;
                    int gridRow = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
                    int gridCol = Microsoft::UI::Xaml::Controls::Grid::GetColumn(tb);
                    if (gridRow == row && gridCol == 1)
                    {
                        SetText(tb, winrt::hstring{ text });
                        return tb;
                    }
                }
                Microsoft::UI::Xaml::Controls::TextBlock tb;
                tb.Text(winrt::hstring{ text });
                tb.FontSize(12);
                tb.Margin({ 0, 2, 0, 2 });
                tb.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
                tb.IsTextSelectionEnabled(true);
                if (mono)
                    tb.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Consolas" });
                Microsoft::UI::Xaml::Controls::Grid::SetRow(tb, row);
                Microsoft::UI::Xaml::Controls::Grid::SetColumn(tb, 1);
                grid.Children().Append(tb);
                return tb;
            };

            Microsoft::UI::Xaml::Controls::Grid grid{ nullptr };
            Microsoft::UI::Xaml::Controls::Border card{ nullptr };

            auto it = existing.find(key);
            if (it != existing.end())
            {
                card = it->second;
                grid = card.Child().try_as<Microsoft::UI::Xaml::Controls::Grid>();
                if (!grid) continue;
                uint32_t cur = 0;
                if (host.Children().IndexOf(card, cur) && cur != idx)
                {
                    host.Children().RemoveAt(cur);
                    host.Children().InsertAt(idx, card);
                }
            }
            else
            {
                grid = Microsoft::UI::Xaml::Controls::Grid{};
                Microsoft::UI::Xaml::Controls::ColumnDefinition c0, c1;
                c0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                    70, Microsoft::UI::Xaml::GridUnitType::Pixel));
                c1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                    1, Microsoft::UI::Xaml::GridUnitType::Star));
                grid.ColumnDefinitions().Append(c0);
                grid.ColumnDefinitions().Append(c1);
                for (int i = 0; i < 5; ++i)
                {
                    Microsoft::UI::Xaml::Controls::RowDefinition rd;
                    rd.Height(Microsoft::UI::Xaml::GridLengthHelper::Auto());
                    grid.RowDefinitions().Append(rd);
                }
                auto addLabel = [&](int row, wchar_t const* text) {
                    Microsoft::UI::Xaml::Controls::TextBlock tb;
                    tb.Text(text);
                    tb.FontSize(12);
                    tb.Margin({ 0, 2, 8, 2 });
                    // Theme-aware Style — direct .Foreground(Lookup(...))
                    // freezes the brush at construction time and leaves
                    // labels white when the user switches to Light theme.
                    if (auto st = Microsoft::UI::Xaml::Application::Current()
                            .Resources().TryLookup(winrt::box_value(
                                winrt::hstring{ L"SecondaryText" })))
                        tb.Style(st.try_as<Microsoft::UI::Xaml::Style>());
                    Microsoft::UI::Xaml::Controls::Grid::SetRow(tb, row);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(tb, 0);
                    grid.Children().Append(tb);
                };
                addLabel(1, L"Switch");
                addLabel(2, L"IPv4");
                addLabel(3, L"IPv6");
                addLabel(4, L"Status");

                card = Microsoft::UI::Xaml::Controls::Border{};
                // CornerRadius / Padding / Background all live in the
                // "InfoCard" Style (App.xaml) so the background is
                // theme-aware on a RequestedTheme switch.
                if (auto st = Microsoft::UI::Xaml::Application::Current()
                        .Resources().TryLookup(winrt::box_value(
                            winrt::hstring{ L"InfoCard" })))
                    card.Style(st.try_as<Microsoft::UI::Xaml::Style>());
                card.Child(grid);
                card.Tag(winrt::box_value(winrt::hstring{ key }));
                host.Children().InsertAt(idx, card);
            }

            {
                bool found = false;
                for (uint32_t c = 0; c < grid.Children().Size(); ++c)
                {
                    auto tb = grid.Children().GetAt(c).try_as<
                        Microsoft::UI::Xaml::Controls::TextBlock>();
                    if (!tb) continue;
                    int row = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
                    int colSpan = Microsoft::UI::Xaml::Controls::Grid::GetColumnSpan(tb);
                    if (row == 0 && colSpan == 2)
                    {
                        SetText(tb, winrt::hstring{ nameLine });
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    Microsoft::UI::Xaml::Controls::TextBlock tb;
                    tb.Text(winrt::hstring{ nameLine });
                    tb.FontSize(12);
                    tb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
                    tb.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
                    tb.Margin({ 0, 0, 0, 4 });
                    Microsoft::UI::Xaml::Controls::Grid::SetRow(tb, 0);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(tb, 0);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumnSpan(tb, 2);
                    grid.Children().Append(tb);
                }
            }

            findOrCreateValueTb(grid, 1,
                nic.switchName.empty() ? std::wstring{ L"-" } : nic.switchName, false);
            findOrCreateValueTb(grid, 2, JoinIps(nic.ipv4), true);
            findOrCreateValueTb(grid, 3, JoinIps(nic.ipv6), true);
            findOrCreateValueTb(grid, 4, statusText, false);
        }

        for (uint32_t i = host.Children().Size(); i-- > 0; )
        {
            auto b = host.Children().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::Border>();
            if (!b) continue;
            auto key = winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"");
            if (seen.find(std::wstring{ key }) == seen.end())
                host.Children().RemoveAt(i);
        }
    }

    void MainWindow::UpdateInfoFlyoutDisks(hyprv::app::vm::VirtualMachine const& vm)
    {
        auto host = flyoutDisksHost();
        auto expander = flyoutStorageExpander();
        if (!host || !expander) return;

        if (vm.disks.empty())
        {
            host.Children().Clear();
            expander.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }
        expander.Visibility(Microsoft::UI::Xaml::Visibility::Visible);

        std::unordered_map<std::wstring,
            Microsoft::UI::Xaml::Controls::Border> existing;
        existing.reserve(host.Children().Size());
        for (uint32_t i = 0; i < host.Children().Size(); ++i)
        {
            auto b = host.Children().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::Border>();
            if (!b) continue;
            auto key = winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"");
            existing.emplace(std::wstring{ key }, b);
        }

        std::unordered_set<std::wstring> seen;

        for (uint32_t idx = 0; idx < vm.disks.size(); ++idx)
        {
            auto const& disk = vm.disks[idx];
            seen.insert(disk.path);

            std::wstring leaf = PathLeaf(disk.path);
            std::wstring subline = DiskKindLabel(disk.kind);
            if (disk.fileSizeBytes > 0)
                subline += L"  ·  " + FormatFileSize(disk.fileSizeBytes);

            auto setStackChildText = [&](
                Microsoft::UI::Xaml::Controls::StackPanel const& sp,
                uint32_t childIdx, std::wstring const& text)
            {
                if (childIdx >= sp.Children().Size()) return;
                if (auto tb = sp.Children().GetAt(childIdx)
                    .try_as<Microsoft::UI::Xaml::Controls::TextBlock>())
                {
                    SetText(tb, winrt::hstring{ text });
                }
            };

            auto it = existing.find(disk.path);
            Microsoft::UI::Xaml::Controls::Border card{ nullptr };
            Microsoft::UI::Xaml::Controls::StackPanel sp{ nullptr };

            if (it != existing.end())
            {
                card = it->second;
                sp = card.Child().try_as<Microsoft::UI::Xaml::Controls::StackPanel>();
                if (!sp) continue;
                uint32_t cur = 0;
                if (host.Children().IndexOf(card, cur) && cur != idx)
                {
                    host.Children().RemoveAt(cur);
                    host.Children().InsertAt(idx, card);
                }
                setStackChildText(sp, 0, leaf);
                setStackChildText(sp, 1, disk.path);
                setStackChildText(sp, 2, subline);
            }
            else
            {
                sp = Microsoft::UI::Xaml::Controls::StackPanel{};
                sp.Spacing(2);

                Microsoft::UI::Xaml::Controls::TextBlock nameTb;
                nameTb.Text(winrt::hstring{ leaf });
                nameTb.FontSize(12);
                nameTb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
                nameTb.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
                nameTb.IsTextSelectionEnabled(true);
                sp.Children().Append(nameTb);

                Microsoft::UI::Xaml::Controls::TextBlock pathTb;
                pathTb.Text(winrt::hstring{ disk.path });
                pathTb.FontSize(11);
                pathTb.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Consolas" });
                pathTb.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
                pathTb.IsTextSelectionEnabled(true);
                sp.Children().Append(pathTb);

                Microsoft::UI::Xaml::Controls::TextBlock subTb;
                subTb.Text(winrt::hstring{ subline });
                subTb.FontSize(12);
                if (auto st = Microsoft::UI::Xaml::Application::Current()
                        .Resources().TryLookup(winrt::box_value(
                            winrt::hstring{ L"SecondaryText" })))
                    subTb.Style(st.try_as<Microsoft::UI::Xaml::Style>());
                sp.Children().Append(subTb);

                card = Microsoft::UI::Xaml::Controls::Border{};
                // CornerRadius / Padding / Background all from "InfoCard"
                // Style — theme-aware Background via ThemeResource Setter.
                if (auto st = Microsoft::UI::Xaml::Application::Current()
                        .Resources().TryLookup(winrt::box_value(
                            winrt::hstring{ L"InfoCard" })))
                    card.Style(st.try_as<Microsoft::UI::Xaml::Style>());
                card.Child(sp);
                card.Tag(winrt::box_value(winrt::hstring{ disk.path }));
                host.Children().InsertAt(idx, card);
            }
        }

        for (uint32_t i = host.Children().Size(); i-- > 0; )
        {
            auto b = host.Children().GetAt(i).try_as<
                Microsoft::UI::Xaml::Controls::Border>();
            if (!b) continue;
            auto key = winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"");
            if (seen.find(std::wstring{ key }) == seen.end())
                host.Children().RemoveAt(i);
        }
    }

    void MainWindow::UpdateInfoFlyoutSnapshots(hyprv::app::vm::VirtualMachine const& vm)
    {
        auto list = flyoutSnapshotList();
        if (!list) return;

        // In-flight snapshot job status (inline, right of Take). `pendingJobLabel`
        // is set the instant the user starts Take/Apply/Delete and cleared when
        // the async job ends; `statusText` upgrades to Hyper-V's native "Creating
        // Checkpoint (35%)" while it runs. Take is disabled meanwhile. (Runs
        // BEFORE the empty-snapshots early return so a first Take still shows it.)
        bool jobRunning = !vm.pendingJobLabel.empty();
        if (auto prog = flyoutSnapProgress())
            prog.Visibility(jobRunning ? Microsoft::UI::Xaml::Visibility::Visible
                                       : Microsoft::UI::Xaml::Visibility::Collapsed);
        if (auto ring = flyoutSnapProgressRing())
            ring.IsActive(jobRunning);
        if (jobRunning)
            if (auto t = flyoutSnapProgressText())
                SetText(t, winrt::hstring{
                    vm.statusText.empty() ? vm.pendingJobLabel : vm.statusText });
        if (auto btn = flyoutTakeSnapBtn()) btn.IsEnabled(!jobRunning);

        if (vm.snapshots.empty())
        {
            list.Items().Clear();
            if (auto tb = flyoutNoSnapshots())
                tb.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
            list.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
            m_selectedSnapshotPath.clear();
            m_selectedSnapshotName.clear();
            return;
        }
        if (auto tb = flyoutNoSnapshots())
            tb.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        list.Visibility(Microsoft::UI::Xaml::Visibility::Visible);

        std::unordered_map<std::wstring, Microsoft::UI::Xaml::Controls::ListViewItem> existing;
        existing.reserve(list.Items().Size());
        for (uint32_t i = 0; i < list.Items().Size(); ++i)
        {
            auto lvi = list.Items().GetAt(i).try_as<Microsoft::UI::Xaml::Controls::ListViewItem>();
            if (!lvi) continue;
            auto p = winrt::unbox_value_or<winrt::hstring>(lvi.Tag(), L"");
            existing.emplace(std::wstring{ p }, lvi);
        }

        // Build a flat render list from the snapshot tree, inserting a synthetic
        // "Now" node as a CHILD (depth+1) of the current snapshot — so the live
        // position reads as a node inside the active subtree, like Hyper-V
        // Manager (not a badge beside the snapshot name).
        struct SnapRow { std::wstring key; std::wstring display; int depth; bool isNow; };
        const std::wstring kNowKey = L"\x01now";
        std::vector<SnapRow> rows;
        rows.reserve(vm.snapshots.size() + 1);
        for (auto const& s : vm.snapshots)
        {
            std::wstring subtitle = s.creationTime ? FormatLocalTime(*s.creationTime)
                                                   : std::wstring{};
            std::wstring display = s.elementName.empty() ? std::wstring{ L"(unnamed)" }
                                                         : s.elementName;
            if (!subtitle.empty()) display += L"  ·  " + subtitle;
            rows.push_back({ s.path, std::move(display), s.depth, false });
            if (s.isCurrent)
                rows.push_back({ kNowKey, std::wstring{ L"\x25CF Now" }, s.depth + 1, true });
        }

        std::unordered_set<std::wstring> seen;
        seen.reserve(rows.size());

        for (uint32_t idx = 0; idx < rows.size(); ++idx)
        {
            auto const& r = rows[idx];
            seen.insert(r.key);
            Microsoft::UI::Xaml::Thickness indent{
                static_cast<double>(r.depth) * 14.0, 0, 0, 0 };

            auto it = existing.find(r.key);
            if (it != existing.end())
            {
                if (auto tb = it->second.Content().try_as<Microsoft::UI::Xaml::Controls::TextBlock>())
                {
                    SetText(tb, winrt::hstring{ r.display });
                    tb.Margin(indent);
                }
                uint32_t curIdx = 0;
                if (list.Items().IndexOf(it->second, curIdx) && curIdx != idx)
                {
                    list.Items().RemoveAt(curIdx);
                    list.Items().InsertAt(idx, it->second);
                }
            }
            else
            {
                Microsoft::UI::Xaml::Controls::TextBlock tb;
                SetText(tb, winrt::hstring{ r.display });
                tb.FontSize(r.isNow ? 11 : 12);
                tb.Margin(indent);
                tb.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);

                Microsoft::UI::Xaml::Controls::ListViewItem lvi;
                lvi.Content(tb);
                lvi.Tag(winrt::box_value(winrt::hstring{ r.key }));
                if (r.isNow)
                {
                    // Green, like the running dot; non-interactive (it's a marker,
                    // not a snapshot — no selection, no context menu).
                    tb.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                        winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x4C, 0xC2, 0x66) });
                    tb.FontWeight(winrt::Windows::UI::Text::FontWeight{ 600 });
                    lvi.IsHitTestVisible(false);
                }
                else
                {
                    // Right-click menu (Apply / Rename / Delete / Delete subtree).
                    lvi.ContextFlyout(BuildSnapshotItemMenu(r.key));
                }
                list.Items().InsertAt(idx, lvi);
            }
        }

        for (uint32_t i = list.Items().Size(); i-- > 0; )
        {
            auto lvi = list.Items().GetAt(i).try_as<Microsoft::UI::Xaml::Controls::ListViewItem>();
            if (!lvi) continue;
            auto p = winrt::unbox_value_or<winrt::hstring>(lvi.Tag(), L"");
            if (seen.find(std::wstring{ p }) == seen.end())
                list.Items().RemoveAt(i);
        }

        if (!m_selectedSnapshotPath.empty() &&
            seen.find(m_selectedSnapshotPath) == seen.end())
        {
            m_selectedSnapshotPath.clear();
            m_selectedSnapshotName.clear();
            list.SelectedItem(nullptr);
        }
    }

    // Resolve a snapshot's current display name from the cache (by stable path).
    std::wstring MainWindow::SnapshotNameForPath(std::wstring const& path)
    {
        if (m_flyoutVmGuid.empty()) return {};
        auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(m_flyoutVmGuid);
        if (!vmOpt) return {};
        for (auto const& s : vmOpt->snapshots)
            if (s.path == path) return s.elementName;
        return {};
    }

    // Build the right-click menu for a snapshot list item. Items are gated on
    // job-in-flight re-evaluated each Opening (a snapshot's path is stable, so
    // the menu is created once but its enabled state stays fresh).
    Microsoft::UI::Xaml::Controls::MenuFlyout MainWindow::BuildSnapshotItemMenu(std::wstring path)
    {
        using namespace Microsoft::UI::Xaml::Controls;
        MenuFlyout menu;
        hyprv::app::ui::ApplyTo(menu);
        auto weak = get_weak();

        auto mkItem = [&](wchar_t const* text, wchar_t const* glyph,
                          std::function<void(MainWindow*)> fn) -> MenuFlyoutItem {
            MenuFlyoutItem item;
            item.Text(text);
            item.FontSize(12);
            item.MinHeight(28);
            item.Padding({ 4, 2, 8, 2 });
            if (glyph && *glyph)
            {
                FontIcon icon;
                icon.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe MDL2 Assets" });
                icon.Glyph(glyph);
                icon.FontSize(12);
                item.Icon(icon);
            }
            item.Click([weak, fn](winrt::Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&) {
                if (auto s = weak.get()) fn(s.get());
            });
            menu.Items().Append(item);
            return item;
        };

        auto applyItem  = mkItem(L"Apply",          L"\xE7A7", [path](MainWindow* s) { s->ApplySnapshotAction(path); });
        auto renameItem = mkItem(L"Rename\x2026",   L"\xE8AC", [path](MainWindow* s) { s->RenameSnapshotAction(path); });
        menu.Items().Append(MenuFlyoutSeparator{});
        auto delItem    = mkItem(L"Delete",         L"\xE74D", [path](MainWindow* s) { s->DeleteSnapshotAction(path, false); });
        auto delSubItem = mkItem(L"Delete subtree", L"\xE74D", [path](MainWindow* s) { s->DeleteSnapshotAction(path, true); });

        // Re-evaluate enabled state when the menu opens (disable everything while
        // a snapshot job is in flight).
        menu.Opening([weak, applyItem, renameItem, delItem, delSubItem](
            winrt::Windows::Foundation::IInspectable const&,
            winrt::Windows::Foundation::IInspectable const&) {
            bool jobRunning = false;
            if (auto s = weak.get(); s && !s->m_flyoutVmGuid.empty())
                if (auto vm = hyprv::app::vm::VMManager::Instance().GetByGuid(s->m_flyoutVmGuid); vm)
                    jobRunning = !vm->pendingJobLabel.empty();
            applyItem.IsEnabled(!jobRunning);
            renameItem.IsEnabled(!jobRunning);
            delItem.IsEnabled(!jobRunning);
            delSubItem.IsEnabled(!jobRunning);
        });
        return menu;
    }

    void MainWindow::ApplySnapshotAction(std::wstring path)
    {
        if (m_flyoutVmGuid.empty() || path.empty()) return;
        std::wstring vmGuid = m_flyoutVmGuid;
        std::wstring name = SnapshotNameForPath(path);
        std::wstring body =
            L"Apply \"" + (name.empty() ? std::wstring{ L"this snapshot" } : name) +
            L"\"?\n\nThe virtual machine's current state will be lost.";
        hyprv::app::ui::ConfirmAndAct(get_weak(), L"applySnapshot",
            L"Apply snapshot", body, L"Apply",
            [vmGuid, path] {
                hyprv::app::vm::VMManager::Instance().ApplySnapshot(vmGuid, path);
            });
    }

    void MainWindow::DeleteSnapshotAction(std::wstring path, bool subtree)
    {
        if (m_flyoutVmGuid.empty() || path.empty()) return;
        std::wstring vmGuid = m_flyoutVmGuid;
        std::wstring name = SnapshotNameForPath(path);
        std::wstring quoted = L"\"" + (name.empty() ? std::wstring{ L"this snapshot" } : name) + L"\"";
        std::wstring body = subtree
            ? L"Delete " + quoted + L" and ALL its descendants?\n\nThis can't be undone."
            : L"Delete " + quoted + L"?\n\nThis can't be undone. Child snapshots stay; "
              L"their parent rewires automatically.";
        hyprv::app::ui::ConfirmAndAct(get_weak(),
            subtree ? L"deleteSnapshotSubtree" : L"deleteSnapshot",
            subtree ? L"Delete snapshot subtree" : L"Delete snapshot", body,
            subtree ? L"Delete subtree" : L"Delete",
            [vmGuid, path, subtree] {
                hyprv::app::vm::VMManager::Instance().DeleteSnapshot(vmGuid, path, subtree);
            });
    }

    void MainWindow::RenameSnapshotAction(std::wstring path)
    {
        if (path.empty()) return;
        std::wstring current = SnapshotNameForPath(path);
        hyprv::app::ui::ShowInputDialog(get_weak(), L"Rename snapshot",
            L"New name", current, L"Rename",
            [path](std::wstring newName) {
                hyprv::app::vm::VMManager::Instance().RenameSnapshot(path, newName);
            });
    }

    // ---- Splitter drag-to-resize -------------------------------------------
    // Lightweight hand-rolled GridSplitter equivalent. WinUI 3 doesn't ship one
    // and CommunityToolkit isn't pulled in yet.

    // The outer splitter Border stays transparent-full-height so cursor change
    // and click hit-testing cover the whole 6 DIP grab strip. The visible hint
    // is a small centered pill (railSplitterHint / flyoutSplitterHint) — its
    // Border.Child of the splitter. We toggle that child's Visibility on
    // hover / drag instead of tinting the whole strip.
    void MainWindow::OnSplitterPointerEntered(IInspectable const& sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        auto outer = sender.try_as<Microsoft::UI::Xaml::Controls::Border>();
        if (!outer) return;
        auto inner = outer.Child().try_as<Microsoft::UI::Xaml::Controls::Border>();
        if (!inner) return;
        inner.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
    }
    void MainWindow::OnSplitterPointerExited(IInspectable const& sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        // Don't hide the pill while a drag is in progress — capture keeps the
        // pointer routed here but Exited can still fire on capture loss
        // boundaries on some hardware. The active drag should keep the hint
        // visible until release.
        if (m_railDragging || m_flyoutDragging) return;
        auto outer = sender.try_as<Microsoft::UI::Xaml::Controls::Border>();
        if (!outer) return;
        auto inner = outer.Child().try_as<Microsoft::UI::Xaml::Controls::Border>();
        if (!inner) return;
        inner.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
    }
    void MainWindow::OnSplitterPointerPressed(IInspectable const& sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        auto fe = sender.try_as<Microsoft::UI::Xaml::UIElement>();
        if (!fe) return;
        m_railDragging = true;
        // Pointer coords relative to the window's root visual — nullptr would
        // give us screen coords which the splitter math doesn't want.
        auto root = Content().try_as<UIElement>();
        auto pos = e.GetCurrentPoint(root).Position();
        m_railDragStartX = pos.X;
        m_railDragStartWidth = railColumn().Width().Value;
        fe.CapturePointer(e.Pointer());
    }
    void MainWindow::OnSplitterPointerMoved(IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        if (!m_railDragging) return;
        auto root = Content().try_as<UIElement>();
        auto pos = e.GetCurrentPoint(root).Position();
        const double dx = pos.X - m_railDragStartX;
        double w = m_railDragStartWidth + dx;
        if (w < 120) w = 120;
        if (w > 480) w = 480;
        railColumn().Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
            w, Microsoft::UI::Xaml::GridUnitType::Pixel));
        m_railSavedWidth = w;
    }
    void MainWindow::OnSplitterPointerReleased(IInspectable const& sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        m_railDragging = false;
        if (auto fe = sender.try_as<Microsoft::UI::Xaml::UIElement>())
            fe.ReleasePointerCapture(e.Pointer());
        // Snapshot the new width into Settings — debounced save means a
        // rapid drag-twiddle-release sequence still results in one disk hit.
        PersistGeometry();
    }

    void MainWindow::OnFlyoutSplitterPointerPressed(IInspectable const& sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        auto fe = sender.try_as<Microsoft::UI::Xaml::UIElement>();
        if (!fe) return;
        m_flyoutDragging = true;
        auto root = Content().try_as<UIElement>();
        auto pos = e.GetCurrentPoint(root).Position();
        m_flyoutDragStartX = pos.X;
        m_flyoutDragStartWidth = flyoutColumn().Width().Value;
        fe.CapturePointer(e.Pointer());
    }
    void MainWindow::OnFlyoutSplitterPointerMoved(IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        if (!m_flyoutDragging) return;
        auto root = Content().try_as<UIElement>();
        auto pos = e.GetCurrentPoint(root).Position();
        // Flyout splitter sits LEFT of the flyout column: dragging right
        // shrinks the flyout, dragging left grows it. So subtract dx.
        const double dx = pos.X - m_flyoutDragStartX;
        double w = m_flyoutDragStartWidth - dx;
        if (w < 200) w = 200;
        if (w > 600) w = 600;
        flyoutColumn().Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
            w, Microsoft::UI::Xaml::GridUnitType::Pixel));
        m_flyoutSavedWidth = w;
    }
    void MainWindow::OnFlyoutSplitterPointerReleased(IInspectable const& sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        m_flyoutDragging = false;
        if (auto fe = sender.try_as<Microsoft::UI::Xaml::UIElement>())
            fe.ReleasePointerCapture(e.Pointer());
        PersistGeometry();
    }

    // ---- Flyout: chart size + snapshot handlers ---------------------------

    void MainWindow::OnChartSizeChanged(IInspectable const& sender,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        if (m_flyoutVmGuid.empty()) return;
        auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(m_flyoutVmGuid);
        if (!vmOpt) return;
        auto grid = sender.try_as<Microsoft::UI::Xaml::Controls::Grid>();
        if (!grid) return;
        if (grid == flyoutCpuChart())
        {
            auto cpuGreen = Windows::UI::ColorHelper::FromArgb(0xFF, 0x4C, 0xC2, 0x66);
            DrawSparkline(grid, vmOpt->cpuHistoryPct, 100.0, cpuGreen);
        }
        else if (grid == flyoutMemoryChart())
        {
            uint32_t maxMem = 1;
            for (auto v : vmOpt->memoryHistoryMb) if (v > maxMem) maxMem = v;
            auto memBlue = Windows::UI::ColorHelper::FromArgb(0xFF, 0x4C, 0x8B, 0xF5);
            DrawSparkline(grid, vmOpt->memoryHistoryMb,
                          static_cast<double>(maxMem), memBlue);
        }
    }

    void MainWindow::OnSnapshotSelected(IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        // Selection is just a visual highlight now — the per-snapshot actions
        // come from each item's right-click menu (which targets its own path).
        // We still track the selected path/name for convenience.
        auto list = flyoutSnapshotList();
        if (!list) return;
        auto sel = list.SelectedItem().try_as<Microsoft::UI::Xaml::Controls::ListViewItem>();
        if (!sel)
        {
            m_selectedSnapshotPath.clear();
            m_selectedSnapshotName.clear();
            return;
        }
        auto p = winrt::unbox_value_or<winrt::hstring>(sel.Tag(), L"");
        m_selectedSnapshotPath = std::wstring{ p };
        m_selectedSnapshotName = SnapshotNameForPath(m_selectedSnapshotPath);
    }

    void MainWindow::OnTakeSnapshotClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_flyoutVmGuid.empty()) return;
        hyprv::app::vm::VMManager::Instance().TakeSnapshot(m_flyoutVmGuid);
    }
}
