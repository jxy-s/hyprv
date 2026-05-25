#include "VmTileFactory.h"

#include "ConfirmDialog.h"
#include "PopupBackdrop.h"
#include "../MainWindow.xaml.h"
#include "../VmSettingsDialog.xaml.h"
#include "../VmTabPage.xaml.h"
#include "../settings/Settings.h"
#include "../vm/VMManager.h"
#include "../vm/VirtualMachine.h"

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <windows.h>      // OpenClipboard, GlobalLock, ShellExecuteW
#include <shellapi.h>

#include <chrono>
#include <functional>
#include <string>

extern void HyprvAppLog(const wchar_t* fmt, ...);

// Match the using-directive set in MainWindow.xaml.cpp so the moved code
// can keep its original short-form qualifications (Microsoft::UI::Xaml::...
// instead of winrt::Microsoft::UI::Xaml::...).
using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace
{
    // Pull plain UTF-16 text out of the system clipboard. Returns empty if
    // the clipboard is unavailable or holds no text. Used by the VM context
    // menu's "Type clipboard" action.
    std::wstring GetClipboardTextW()
    {
        if (!OpenClipboard(nullptr)) return {};
        std::wstring text;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h)
        {
            auto p = static_cast<wchar_t const*>(GlobalLock(h));
            if (p) { text = p; GlobalUnlock(h); }
        }
        CloseClipboard();
        return text;
    }
}

namespace hyprv::app::ui
{
    winrt::Windows::UI::Color VmDotColor(hyprv::app::vm::VirtualMachine const& vm)
    {
        if (vm.IsRunning())
            return winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x4C, 0xC2, 0x66);
        if (vm.IsPaused() || vm.IsTransitioning())
            return winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xE2, 0xB5, 0x3B);
        return winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x80, 0x80, 0x80);
    }

    void ApplyVmDotState(winrt::Microsoft::UI::Xaml::Shapes::Ellipse const& dot,
                         hyprv::app::vm::VirtualMachine const& vm)
    {
        namespace MUX  = winrt::Microsoft::UI::Xaml;
        namespace MUXM = winrt::Microsoft::UI::Xaml::Media;
        namespace MUXA = winrt::Microsoft::UI::Xaml::Media::Animation;
        if (!dot) return;

        // Blink while a state transition / job is in flight — including the
        // optimistic pending flag set the instant the user requests a change,
        // so feedback is immediate (not ~1 poll later).
        const bool wantBlink = vm.IsTransitioning() || vm.pendingStateChange;

        // Color: stable VMs show the normal state color (green/amber/grey).
        // While blinking: a snapshot/checkpoint job (pendingJobLabel set) pulses
        // BLUE — a distinct "data operation in progress" signal vs. a power
        // transition. Otherwise a running VM pulses GREEN (changing but still up)
        // and everything else pulses AMBER (so an Off/grey VM that's starting
        // reads as active amber rather than a barely-visible grey pulse).
        winrt::Windows::UI::Color color;
        if (!wantBlink)
            color = VmDotColor(vm);
        else if (!vm.pendingJobLabel.empty())
            color = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x3B, 0x8E, 0xE2);  // blue
        else if (vm.IsRunning())
            color = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x4C, 0xC2, 0x66);  // green
        else
            color = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xE2, 0xB5, 0x3B);  // amber
        dot.Fill(MUXM::SolidColorBrush{ color });

        // The looping Storyboard is cached in the Ellipse's Tag so repeated
        // update passes (the rail diffs per poll) don't restart it; cleared +
        // opacity reset when the VM settles into a stable state.
        auto existing = dot.Tag().try_as<MUXA::Storyboard>();
        if (wantBlink)
        {
            if (existing) return;   // already pulsing
            MUXA::DoubleAnimation anim;
            anim.From(1.0);
            anim.To(0.2);
            anim.Duration(MUX::Duration{ std::chrono::milliseconds(550) });
            anim.AutoReverse(true);
            MUXA::RepeatBehavior rb{};
            rb.Type = MUXA::RepeatBehaviorType::Forever;   // value struct — field, not setter
            anim.RepeatBehavior(rb);

            MUXA::Storyboard sb;
            sb.Children().Append(anim);
            MUXA::Storyboard::SetTarget(anim, dot);
            MUXA::Storyboard::SetTargetProperty(anim, L"Opacity");
            dot.Tag(sb);
            sb.Begin();
        }
        else if (existing)
        {
            existing.Stop();
            dot.Tag(nullptr);
            dot.Opacity(1.0);
        }
    }

    unsigned long LaunchVmDebugger(std::wstring const& vmGuid)
    {
        auto& s = hyprv::app::settings::Settings::Instance();
        std::wstring exe  = s.EffectiveDebuggerExe(vmGuid);
        std::wstring args = s.VmDebuggerArgs(vmGuid);
        if (exe.empty() || args.empty()) return 0;   // nothing to launch
        std::wstring cmdline = L"\"" + exe + L"\" " + args;
        HyprvAppLog(L"[dbg] launch: %s", cmdline.c_str());
        auto attempt = [](std::wstring cmd, DWORD flags) -> DWORD {
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                               flags, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return 0;
            }
            return GetLastError();
        };
        // Break away from the rdphost kill-on-job-close job so the debugger
        // outlives a hyprv crash/exit. If hyprv isn't in a job the flag is
        // ignored (first attempt succeeds); if it's in a non-breakaway job the
        // call fails with the flag, so retry once without it.
        DWORD err = attempt(cmdline, CREATE_BREAKAWAY_FROM_JOB);
        if (err == 0) return 0;
        return attempt(cmdline, 0);
    }

    // Open the VM hardware settings dialog (modal); on Save, if the RDP options
    // changed, reconnect the VM's live session. A NAMED coroutine with BY-VALUE
    // parameters, NOT an inline immediately-invoked capturing lambda: that
    // lambda's captures are destroyed at the first co_await, so reading
    // `window`/`guid` after ShowAsync would be a use-after-free (gotcha #19).
    // Coroutine parameters live in the coroutine frame and survive the await.
    static winrt::fire_and_forget ShowVmSettingsDialogCoro(
        winrt::com_ptr<winrt::hyprv_app::implementation::MainWindow> window,
        winrt::Microsoft::UI::Xaml::XamlRoot root,
        winrt::Microsoft::UI::Xaml::ElementTheme theme,
        std::wstring guid)
    {
        winrt::hyprv_app::VmSettingsDialog dlg;
        auto impl = winrt::get_self<
            winrt::hyprv_app::implementation::VmSettingsDialog>(dlg);
        impl->SetVm(guid);
        dlg.XamlRoot(root);
        dlg.RequestedTheme(theme);
        winrt::hyprv_app::implementation::PopupSuppressionScope suppress(window.get());
        co_await dlg.ShowAsync();
        // If the Save changed this VM's RDP options, reconnect its live session
        // so the change applies without a manual close + reopen.
        if (impl->RdpOptionsChanged())
        {
            if (auto page = window->FindVmTab(guid))
            {
                auto pImpl = winrt::get_self<
                    winrt::hyprv_app::implementation::VmTabPage>(page);
                pImpl->ReapplyConnectionSettings();
            }
        }
    }

    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout BuildVmContextMenu(
        winrt::hstring const& vmGuid, winrt::hstring const& vmName,
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow)
    {
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        MenuFlyout menu;

        auto addItem = [&](wchar_t const* text, wchar_t const* glyph,
                           bool enabled,
                           std::function<void()> onClick) -> MenuFlyoutItem
        {
            MenuFlyoutItem item;
            item.Text(text);
            item.FontSize(12);
            item.MinHeight(28);
            item.Padding({ 4, 2, 8, 2 });
            item.IsEnabled(enabled);
            if (glyph && *glyph)
            {
                FontIcon icon;
                icon.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe MDL2 Assets" });
                icon.Glyph(glyph);
                icon.FontSize(12);
                item.Icon(icon);
            }
            item.Click([cb = std::move(onClick)](
                winrt::Windows::Foundation::IInspectable const&,
                Microsoft::UI::Xaml::RoutedEventArgs const&) { cb(); });
            menu.Items().Append(item);
            return item;
        };

        auto sep = [&] { menu.Items().Append(MenuFlyoutSeparator{}); };

        // ---- State snapshot used to gate every item below -----------------
        // One WMI snapshot up front, then derive booleans for each transition.
        // If the VM is missing (just deleted, or pre-first-poll), every
        // VM-state-dependent item is greyed; "Settings..." stays enabled so
        // the user can still get an explanation of what happened.
        std::wstring guidW{ vmGuid.c_str() };
        auto& settings = hyprv::app::settings::Settings::Instance();
        auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(guidW);
        const bool vmExists       = vmOpt.has_value();
        const bool vmRunning      = vmExists && vmOpt->IsRunning();
        const bool vmPaused       = vmExists && vmOpt->IsPaused();
        const bool vmSaved        = vmExists && vmOpt->IsSaved();
        const bool vmOff          = vmExists && vmOpt->IsOff();
        const bool vmTransitioning = vmExists && vmOpt->IsTransitioning();
        // "Stable not running" mirrors the same gate VmTabPage uses for its
        // placeholder Start button — Off / Saved / Paused are all valid
        // starting states for Enabled=2 (Start / Resume).
        const bool canStart       = vmExists && !vmRunning && !vmTransitioning
                                    && (vmOff || vmSaved || vmPaused);
        // Whether enhanced is the active mode for the live session. Best
        // proxy without a live rdphost callback: VM is running AND Hyper-V
        // currently reports enhanced AND the user hasn't opted out.
        const bool enhancedActive = vmRunning && vmOpt->enhancedSessionAvailable
                                    && settings.EnhancedSessionPref(guidW);
        // Send-keys items go through Msvm_Keyboard, which an enhanced RDP
        // session intercepts before the host can react — so Ctrl+Alt+Del and
        // clipboard-typing only have a visible effect in basic sessions.
        // Also need the VM running for the keyboard target to be alive.
        const bool canSendKeys    = vmRunning && !enhancedActive;

        // ---- Power (graceful actions above the line, hard actions below) ---
        // Start / Resume covers Off→Running, Saved→Running (resume), and
        // Paused→Running (resume). Greyed in any other state.
        auto startItem = addItem(L"Start / Resume",         L"\xE768", canStart, [vmGuid] {
            hyprv::app::vm::VMManager::Instance().RequestStateChange(std::wstring{vmGuid.c_str()}, hyprv::app::vm::VmStateChange::Enabled); });
        // Pause (Quiesce) only valid from Running.
        auto pauseItem = addItem(L"Pause",                  L"\xE769", vmRunning, [vmGuid] {
            hyprv::app::vm::VMManager::Instance().RequestStateChange(std::wstring{vmGuid.c_str()}, hyprv::app::vm::VmStateChange::Quiesce); });
        // Save = suspend-to-disk. Confirmation defaults OFF (reversible —
        // Resume restores the same session) but users can opt in. The
        // copy is written so it still makes sense if the user has the
        // dialog turned on, since the rationale to confirm at all is
        // "don't confuse with snapshot".
        // Save (Offline) suspends-to-disk; needs the VM running.
        auto saveItem = addItem(L"Save",                   L"\xE74E", vmRunning, [vmGuid, vmName, weakWindow] {
            std::wstring body =
                L"Save the running state of \"" + std::wstring{ vmName.c_str() } +
                L"\" to disk and stop it?\n\n"
                L"Different from a snapshot: Save suspends the VM, writes its memory to disk, "
                L"and powers it down. Resume restores the same session. To capture a point-in-time "
                L"copy you can roll back to, use Take snapshot instead.";
            hyprv::app::ui::ConfirmAndAct(weakWindow, L"save",
                L"Save virtual machine", body, L"Save",
                [vmGuid] {
                    hyprv::app::vm::VMManager::Instance().RequestStateChange(
                        std::wstring{ vmGuid.c_str() },
                        hyprv::app::vm::VmStateChange::Offline);
                });
        });
        // Restart = graceful reboot via integration services. Confirmation
        // ON by default — in-progress work in guest apps that don't
        // auto-save can be lost. ICs prompt the guest to close apps but
        // some apps will discard unsaved changes on close.
        auto restartItem = addItem(L"Restart",                L"\xE72C", vmRunning, [vmGuid, vmName, weakWindow] {
            std::wstring body =
                L"Restart \"" + std::wstring{ vmName.c_str() } + L"\"?\n\n"
                L"Sends a graceful reboot request through integration services. Unsaved work in "
                L"guest applications may be lost if those apps don't prompt to save before exit.";
            hyprv::app::ui::ConfirmAndAct(weakWindow, L"restart",
                L"Restart virtual machine", body, L"Restart",
                [vmGuid] {
                    hyprv::app::vm::VMManager::Instance().RequestStateChange(
                        std::wstring{ vmGuid.c_str() },
                        hyprv::app::vm::VmStateChange::Reboot);
                });
        });
        // Shut down = graceful power-off via integration services.
        // Confirmation ON by default for the same reason as Restart.
        auto shutdownItem = addItem(L"Shut down",              L"\xE7E8", vmRunning, [vmGuid, vmName, weakWindow] {
            std::wstring body =
                L"Shut down \"" + std::wstring{ vmName.c_str() } + L"\"?\n\n"
                L"Sends a graceful shutdown request through integration services. Unsaved work in "
                L"guest applications may be lost if those apps don't prompt to save before exit.";
            hyprv::app::ui::ConfirmAndAct(weakWindow, L"shutdown",
                L"Shut down virtual machine", body, L"Shut down",
                [vmGuid] {
                    // Graceful guest shutdown via integration services
                    // (Msvm_ShutdownComponent::InitiateShutdown), NOT
                    // RequestStateChange(Shutdown). The IS path keeps the VM
                    // Running until the guest OS halts, so Turn off remains a
                    // working escape hatch if the shutdown stalls. force=false
                    // = graceful (a blocking guest app can stall it — exactly
                    // when the user reaches for Turn off).
                    hyprv::app::vm::VMManager::Instance().ShutdownVM(
                        std::wstring{ vmGuid.c_str() }, false);
                });
        });
        sep();
        // Reset = hard reboot. Confirmation ON by default — equivalent of
        // slamming the physical reset button. In-flight guest writes can
        // corrupt filesystems; this is rarely the right answer when
        // graceful Restart works.
        // Reset / Turn off (hard) are valid from Running OR Paused — both
        // hold guest memory, both can be "yanked".
        auto resetItem = addItem(L"Reset",                  L"\xE777", vmRunning || vmPaused, [vmGuid, vmName, weakWindow] {
            std::wstring body =
                L"Hard-reset \"" + std::wstring{ vmName.c_str() } + L"\"?\n\n"
                L"Equivalent to pressing the physical reset button — the guest gets no chance to "
                L"flush disk caches or shut down cleanly. Unsaved work in guest applications will "
                L"be lost. Use Restart for a graceful reboot via integration services.";
            hyprv::app::ui::ConfirmAndAct(weakWindow, L"reset",
                L"Reset virtual machine", body, L"Reset",
                [vmGuid] {
                    hyprv::app::vm::VMManager::Instance().RequestStateChange(
                        std::wstring{ vmGuid.c_str() },
                        hyprv::app::vm::VmStateChange::Reset);
                });
        });
        // Turn off = hard power-off. Confirmation ON by default —
        // equivalent to pulling the cable. Same data-loss risk as Reset;
        // users who actually want clean shutdown should use Shut down
        // (integration services path) or Save (suspend-to-disk).
        // Gated to Running/Paused only: once the VM is in a transitional
        // state (e.g. "Stopping" during our RequestStateChange-driven Shut
        // down) Hyper-V rejects a force-off via RequestStateChange with 32775
        // ("invalid state for this operation"), so enabling it there would be
        // a button that silently fails. Making force-off work mid-shutdown
        // (like Hyper-V Manager) needs Shut down to go through
        // Msvm_ShutdownComponent::InitiateShutdown, which leaves the VM
        // "Running" until the guest halts — tracked as a follow-up.
        auto turnOffItem = addItem(L"Turn off",               L"\xE7E8", vmRunning || vmPaused, [vmGuid, vmName, weakWindow] {
            std::wstring body =
                L"Turn off \"" + std::wstring{ vmName.c_str() } + L"\"?\n\n"
                L"Equivalent to pulling the power cable — the guest gets no chance to flush disk "
                L"caches or shut down cleanly. Unsaved work in guest applications will be lost. "
                L"Use Shut down for a graceful power-off via integration services.";
            hyprv::app::ui::ConfirmAndAct(weakWindow, L"turnOff",
                L"Turn off virtual machine", body, L"Turn off",
                [vmGuid] {
                    hyprv::app::vm::VMManager::Instance().RequestStateChange(
                        std::wstring{ vmGuid.c_str() },
                        hyprv::app::vm::VmStateChange::Disabled);
                });
        });
        // Delete VM — irreversible, only enabled from Off. Hyper-V refuses
        // to remove a VM that isn't powered off; greying communicates that
        // without needing a separate "must be off first" info dialog.
        auto deleteItem = addItem(L"Delete VM...",           L"\xE74D", vmOff, [vmGuid, vmName, weakWindow] {
            std::wstring guid{ vmGuid.c_str() };
            std::wstring name{ vmName.c_str() };
            std::wstring body =
                L"Permanently delete \"" + name + L"\"?\n\n"
                L"This removes the VM's configuration and snapshots. By default its "
                L"virtual hard disks are left on disk — tick the box below to delete "
                L"those files too (this cannot be undone).";
            // Checkbox variant: the user opts in to deleting the VHD files.
            // Defaults OFF so a careless click never erases disks. Pass-through
            // (physical) disks are never deleted (no file).
            hyprv::app::ui::ConfirmAndActWithCheckbox(weakWindow, L"deleteVm",
                L"Delete VM", body,
                L"Also delete the virtual hard disks", /*initiallyChecked*/ false,
                L"Delete",
                [guid](bool deleteVhds) {
                    hyprv::app::vm::VMManager::Instance().DestroyVM(guid, deleteVhds);
                });
        });

        sep();

        // ---- Session + input ------------------------------------------------
        // Grouped together: things that interact with the live session
        // (enhanced toggle + debugger) and things that send keystrokes
        // into the guest (Ctrl+Alt+Del + clipboard paste). The keyboard
        // paths go via Msvm_Keyboard, but an enhanced RDP session
        // intercepts the synthesized input on the way through — so we
        // only enable them when basic mode is active.
        //
        // Enhanced session — ToggleMenuFlyoutItem with an Icon. The
        // framework draws its check glyph in a separate left column
        // (column 0 of the toggle template) and the icon in column 1,
        // and that check column extends the leading whitespace of every
        // other item in the menu by ~28 DIPs. We tried retemplating to
        // narrow the check column and swap glyphs instead, but neither
        // approach gave a clean result — retemplating misaligned the
        // toggle vs. non-toggle items, and the glyph swap looked stale
        // on every refresh path we tried. Stock toggle + accept the
        // extra leading space is the readable answer.
        //
        // "Supports enhanced" answer:
        //   - VM running → trust the live WMI report (definitive).
        //   - VM not running → fall back to the cached observation in
        //     Settings (defaults to true when we've never seen it run).
        auto computeSupports = [&](
            std::optional<hyprv::app::vm::VirtualMachine> const& vo)
        {
            if (vo && vo->IsRunning()) return vo->enhancedSessionAvailable;
            if (vo) return settings.EnhancedSessionEverSupported(guidW);
            return true;
        };
        ToggleMenuFlyoutItem enhancedToggle;
        {
            enhancedToggle.Text(L"Enhanced session");
            enhancedToggle.FontSize(12);
            enhancedToggle.MinHeight(28);
            enhancedToggle.Padding({ 4, 2, 8, 2 });
            FontIcon enhIcon;
            enhIcon.FontFamily(
                Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe MDL2 Assets" });
            enhIcon.Glyph(L"\xE703");      // Devices2 — represents enhanced session
            enhIcon.FontSize(12);
            enhancedToggle.Icon(enhIcon);
            // Initial state — refreshed live on menu.Opening below.
            const bool pref0 = settings.EnhancedSessionPref(guidW);
            const bool supports0 = computeSupports(vmOpt);
            // IsChecked = pref AND supports. When unsupported, never
            // show as checked — even if pref is stored as true, the
            // session won't actually be enhanced, so the visual must
            // not lie about live state.
            enhancedToggle.IsChecked(pref0 && supports0);
            enhancedToggle.IsEnabled(supports0);
            // Click reads sender.IsChecked() — the framework auto-flips
            // it before firing Click, so this is the new state. Persist
            // + respawn rdphost on the matching open VmTabPage.
            enhancedToggle.Click([guidW, weakWindow](
                winrt::Windows::Foundation::IInspectable const& sender,
                Microsoft::UI::Xaml::RoutedEventArgs const&)
            {
                auto t = sender.try_as<ToggleMenuFlyoutItem>();
                if (!t) return;
                const bool newPref = t.IsChecked();
                HyprvAppLog(L"[menu] ToggleEnhanced %s -> %s",
                    guidW.c_str(), newPref ? L"on" : L"off");
                hyprv::app::settings::Settings::Instance()
                    .SetEnhancedSessionPref(guidW, newPref);
                if (auto win = weakWindow.get())
                {
                    if (auto page = win->FindVmTab(guidW))
                    {
                        auto impl = winrt::get_self<
                            winrt::hyprv_app::implementation::VmTabPage>(page);
                        impl->ApplyEnhancedSessionChange();
                    }
                }
            });
            menu.Items().Append(enhancedToggle);
        }
        // Launch debugger — hidden unless the global feature toggle is on;
        // enabled when this VM has debugger args. Launches the configured
        // command detached. Both states re-evaluated in menu.Opening.
        const bool dbgEnabled = settings.DebuggerEnabled();
        auto debuggerItem = addItem(L"Launch debugger",        L"\xEBE8",
            dbgEnabled && !settings.VmDebuggerArgs(guidW).empty(), [guidW] {
            unsigned long err = LaunchVmDebugger(guidW);
            if (err) HyprvAppLog(L"[menu] debugger launch failed err=%lu", err);
        });
        if (!dbgEnabled)
            debuggerItem.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        // Keep references so the menu.Opening hook can re-evaluate IsEnabled
        // when the user toggles enhanced inside the same cached menu
        // instance — without those refreshes, flipping enhanced on wouldn't
        // visibly grey these (and vice-versa) until the next rail rebuild.
        MenuFlyoutItem ctrlAltDelItem;
        {
            ctrlAltDelItem.Text(L"Send Ctrl+Alt+Del");
            ctrlAltDelItem.FontSize(12);
            ctrlAltDelItem.MinHeight(28);
            ctrlAltDelItem.Padding({ 4, 2, 8, 2 });
            ctrlAltDelItem.IsEnabled(canSendKeys);
            FontIcon ic;
            ic.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe MDL2 Assets" });
            ic.Glyph(L"\xE765");
            ic.FontSize(12);
            ctrlAltDelItem.Icon(ic);
            ctrlAltDelItem.Click([vmGuid](auto&&, auto&&) {
                hyprv::app::vm::VMManager::Instance().TypeCtrlAltDel(
                    std::wstring{ vmGuid.c_str() });
            });
            menu.Items().Append(ctrlAltDelItem);
        }
        MenuFlyoutItem typeClipboardItem;
        {
            typeClipboardItem.Text(L"Type clipboard");
            typeClipboardItem.FontSize(12);
            typeClipboardItem.MinHeight(28);
            typeClipboardItem.Padding({ 4, 2, 8, 2 });
            typeClipboardItem.IsEnabled(canSendKeys);
            FontIcon ic;
            ic.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe MDL2 Assets" });
            ic.Glyph(L"\xE77F");
            ic.FontSize(12);
            typeClipboardItem.Icon(ic);
            typeClipboardItem.Click([vmGuid](auto&&, auto&&) {
                std::wstring text = GetClipboardTextW();
                if (text.empty())
                {
                    HyprvAppLog(L"[menu] TypeClipboard %s: clipboard empty", vmGuid.c_str());
                    return;
                }
                hyprv::app::vm::VMManager::Instance().TypeText(
                    std::wstring{ vmGuid.c_str() }, text);
            });
            menu.Items().Append(typeClipboardItem);
        }

        sep();

        // ---- Snapshots ------------------------------------------------------
        // Take snapshot works in any state Hyper-V accepts (Off, Saved,
        // Running, Paused) — disabled only when the VM is missing or
        // mid-transition (Hyper-V rejects snapshot ops during a job).
        const bool canSnapshot = vmExists && !vmTransitioning;
        auto takeSnapItem = addItem(L"Take snapshot",          L"\xE722", canSnapshot, [vmGuid] {
            hyprv::app::vm::VMManager::Instance().TakeSnapshot(
                std::wstring{ vmGuid.c_str() });
        });
        // Revert needs at least one snapshot to exist; gate on both
        // existence + presence of snapshots.
        const bool canRevert = canSnapshot && vmExists
            && !vmOpt->snapshots.empty();
        auto revertItem = addItem(L"Revert to last snapshot", L"\xE7A7", canRevert, [vmGuid, weakWindow] {
            // Find the most recent snapshot for this VM and apply it. Needs
            // a XamlRoot for the confirmation dialog, hence weakWindow.
            auto win = weakWindow.get();
            if (!win) return;
            auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(
                std::wstring{ vmGuid.c_str() });
            if (!vmOpt || vmOpt->snapshots.empty())
            {
                HyprvAppLog(L"[menu] RevertSnapshot %s: no snapshots",
                    vmGuid.c_str());
                return;
            }
            auto const& snaps = vmOpt->snapshots;
            size_t bestIdx = 0;
            for (size_t i = 1; i < snaps.size(); ++i)
            {
                if (snaps[i].creationTime > snaps[bestIdx].creationTime)
                    bestIdx = i;
            }
            std::wstring path = snaps[bestIdx].path;
            std::wstring name = snaps[bestIdx].elementName;
            std::wstring body =
                L"Revert to \"" +
                (name.empty() ? std::wstring{ L"the most recent snapshot" } : name) +
                L"\"?\n\nThe virtual machine's current state will be lost.";
            hyprv::app::ui::ConfirmAndAct(weakWindow, L"revertToLastSnapshot",
                L"Revert to snapshot", body, L"Revert",
                [path, vmGuid] {
                    hyprv::app::vm::VMManager::Instance().ApplySnapshot(
                        std::wstring{ vmGuid.c_str() }, path);
                });
        });

        sep();

        // ---- Info / Hardware -----------------------------------------------
        // Edit hardware: launch the modal settings dialog. Wires through a
        // weak_ref so the menu (which can outlive the surface that built
        // it) doesn't keep the window alive past close.
        addItem(L"Settings...",            L"\xE713", vmExists, [vmGuid, weakWindow] {
            auto win = weakWindow.get();
            if (!win) return;
            auto root = win->Content() ? win->Content().XamlRoot() : nullptr;
            // Capture the parent window's resolved theme so the dialog
            // can be opened on the matching theme. ContentDialog renders
            // in its own popup layer and doesn't inherit RequestedTheme
            // from the window's Content automatically.
            auto parentTheme = winrt::Microsoft::UI::Xaml::ElementTheme::Default;
            if (auto fe = win->Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
                parentTheme = fe.ActualTheme();
            ShowVmSettingsDialogCoro(win, root, parentTheme,
                                     std::wstring{ vmGuid.c_str() });
        });

        // Refresh per-open: the MenuFlyout instance is assigned once to each
        // rail tile / tab header and reused across right-clicks. RenderRail
        // reuses tiles (UpdateRailItem) WITHOUT rebuilding the flyout, so
        // without this hook every state-gated item freezes at the VM state
        // that existed when the tile was first created — a VM started after
        // its tile was built would still show Off-state gating (Start enabled,
        // Shut down / Turn off greyed, Delete enabled). So on every open we
        // re-evaluate IsEnabled for all state-gated items from a fresh
        // snapshot. (The enhanced toggle + send-keys items also depend on the
        // user's pref, which can change without a WMI tick — same path.)
        winrt::weak_ref<ToggleMenuFlyoutItem> weakToggle{ enhancedToggle };
        winrt::weak_ref<MenuFlyoutItem>       weakCad{ ctrlAltDelItem };
        winrt::weak_ref<MenuFlyoutItem>       weakClip{ typeClipboardItem };
        winrt::weak_ref<MenuFlyoutItem>       wStart{ startItem };
        winrt::weak_ref<MenuFlyoutItem>       wPause{ pauseItem };
        winrt::weak_ref<MenuFlyoutItem>       wSave{ saveItem };
        winrt::weak_ref<MenuFlyoutItem>       wRestart{ restartItem };
        winrt::weak_ref<MenuFlyoutItem>       wShutdown{ shutdownItem };
        winrt::weak_ref<MenuFlyoutItem>       wReset{ resetItem };
        winrt::weak_ref<MenuFlyoutItem>       wTurnOff{ turnOffItem };
        winrt::weak_ref<MenuFlyoutItem>       wDelete{ deleteItem };
        winrt::weak_ref<MenuFlyoutItem>       wDebugger{ debuggerItem };
        winrt::weak_ref<MenuFlyoutItem>       wTakeSnap{ takeSnapItem };
        winrt::weak_ref<MenuFlyoutItem>       wRevert{ revertItem };
        menu.Opening([weakToggle, weakCad, weakClip,
                      wStart, wPause, wSave, wRestart, wShutdown, wReset,
                      wTurnOff, wDelete, wDebugger, wTakeSnap, wRevert, guidW](
            winrt::Windows::Foundation::IInspectable const&,
            winrt::Windows::Foundation::IInspectable const&)
        {
            auto& s = hyprv::app::settings::Settings::Instance();
            auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(guidW);
            const bool exists  = vmOpt.has_value();
            const bool running = exists && vmOpt->IsRunning();
            const bool paused  = exists && vmOpt->IsPaused();
            const bool saved   = exists && vmOpt->IsSaved();
            const bool off     = exists && vmOpt->IsOff();
            const bool trans   = exists && vmOpt->IsTransitioning();
            const bool canStart    = exists && !running && !trans && (off || saved || paused);
            const bool canSnapshot = exists && !trans;
            const bool canRevert   = canSnapshot && !vmOpt->snapshots.empty();

            auto setEnabled = [](winrt::weak_ref<MenuFlyoutItem> const& w, bool e) {
                if (auto i = w.get()) if (i.IsEnabled() != e) i.IsEnabled(e);
            };
            setEnabled(wStart,    canStart);
            setEnabled(wPause,    running);
            setEnabled(wSave,     running);
            setEnabled(wRestart,  running);
            // Shut down needs the guest Shutdown integration service up — grey it
            // (rather than no-op + log) when IS is down / not yet reporting, so the
            // user reaches for Turn off instead. Matches Hyper-V Manager.
            setEnabled(wShutdown, running && vmOpt->shutdownServiceAvailable);
            setEnabled(wReset,    running || paused);
            setEnabled(wTurnOff,  running || paused);
            setEnabled(wDelete,   off);
            // Debugger: hidden unless the feature toggle is on; enabled when
            // the VM has debugger args (matches the title-bar button).
            if (auto d = wDebugger.get())
            {
                const bool feat = s.DebuggerEnabled();
                auto vis = feat ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
                if (d.Visibility() != vis) d.Visibility(vis);
                const bool argsSet = feat && !s.VmDebuggerArgs(guidW).empty();
                if (d.IsEnabled() != argsSet) d.IsEnabled(argsSet);
            }
            setEnabled(wTakeSnap, canSnapshot);
            setEnabled(wRevert,   canRevert);

            // Enhanced toggle (IsChecked + IsEnabled) + send-keys items.
            // "supports enhanced": running → live WMI, else cached observation.
            bool supports = true;
            if (running)       supports = vmOpt->enhancedSessionAvailable;
            else if (exists)   supports = s.EnhancedSessionEverSupported(guidW);
            const bool pref = s.EnhancedSessionPref(guidW);
            const bool wantChecked = pref && supports;
            if (auto t = weakToggle.get())
            {
                if (t.IsChecked() != wantChecked) t.IsChecked(wantChecked);
                if (t.IsEnabled() != supports)   t.IsEnabled(supports);
            }
            const bool enhancedNow    = running && vmOpt->enhancedSessionAvailable && pref;
            const bool canSendKeysNow = running && !enhancedNow;
            setEnabled(weakCad,  canSendKeysNow);
            setEnabled(weakClip, canSendKeysNow);
        });

        // Tighten the presenter padding so every item's leading edge
        // sits close to the popup border. MenuFlyout doesn't expose
        // Resources, but it does expose a presenter Style that we can
        // populate with Setters. Built via an explicit Interop::TypeName
        // so we don't need the xaml_typename<T> helper (not surfaced in
        // this SDK build's projection).
        try
        {
            using namespace winrt::Microsoft::UI::Xaml;
            winrt::Windows::UI::Xaml::Interop::TypeName tn{
                winrt::hstring{ L"Microsoft.UI.Xaml.Controls.MenuFlyoutPresenter" },
                winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata
            };
            Style presenterStyle{ tn };
            // Padding is defined on Control; MenuFlyoutPresenter inherits.
            Setter pad{
                Controls::Control::PaddingProperty(),
                winrt::box_value(Thickness{ 0, 4, 0, 4 })
            };
            presenterStyle.Setters().Append(pad);
            menu.MenuFlyoutPresenterStyle(presenterStyle);
        }
        catch (...) { /* unsupported on this SDK build — ignore */ }

        // Apply the user's chosen Mica/Acrylic backdrop to the menu's
        // own popup composition layer — same reason ContentDialogs need
        // it: popup hosts don't inherit the window's backdrop.
        hyprv::app::ui::ApplyTo(menu);

        return menu;
    }

    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout BuildRemoteHostContextMenu(
        winrt::hstring const& address,
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow)
    {
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        MenuFlyout menu;

        auto addItem = [&](wchar_t const* text, wchar_t const* glyph,
                           std::function<void()> onClick)
        {
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
            item.Click([cb = std::move(onClick)](
                winrt::Windows::Foundation::IInspectable const&,
                Microsoft::UI::Xaml::RoutedEventArgs const&) { cb(); });
            menu.Items().Append(item);
        };

        addItem(L"Connect", L"\xE8AF", [address, weakWindow] {
            if (auto win = weakWindow.get()) win->OpenRemoteHostTab(address);
        });
        addItem(L"Edit…", L"\xE70F", [address, weakWindow] {
            if (auto win = weakWindow.get()) win->OpenRemoteHostDialog(address);
        });
        menu.Items().Append(MenuFlyoutSeparator{});
        addItem(L"Forget", L"\xE74D", [address, weakWindow] {
            if (auto win = weakWindow.get()) win->ForgetRemoteHost(address);
        });

        // Same tightened presenter padding as BuildVmContextMenu (explicit
        // TypeName — xaml_typename<T> isn't surfaced in this SDK projection).
        try
        {
            using namespace winrt::Microsoft::UI::Xaml;
            winrt::Windows::UI::Xaml::Interop::TypeName tn{
                winrt::hstring{ L"Microsoft.UI.Xaml.Controls.MenuFlyoutPresenter" },
                winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata
            };
            Style presenterStyle{ tn };
            Setter pad{
                Controls::Control::PaddingProperty(),
                winrt::box_value(Thickness{ 0, 4, 0, 4 })
            };
            presenterStyle.Setters().Append(pad);
            menu.MenuFlyoutPresenterStyle(presenterStyle);
        }
        catch (...) {}

        hyprv::app::ui::ApplyTo(menu);
        return menu;
    }
}
