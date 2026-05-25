// Shared VM-tile / context-menu primitives. Built so multiple surfaces
// (the rail in MainWindow, the WelcomePage's Recents + All-VMs sections,
// and any future VM-presenting UI) can paint the same dot colors and
// pop the same right-click menu without redefining either.
//
// Anything that needs the owning MainWindow (e.g. ContentDialog XamlRoot)
// takes a weak_ref<implementation::MainWindow> — keep the dependency
// one-way so this header doesn't drag the full XAML projection into every
// consumer.

#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.h>

namespace hyprv::app::vm { struct VirtualMachine; }
namespace winrt::hyprv_app::implementation { struct MainWindow; }

namespace hyprv::app::ui
{
    // State dot color for a VM:
    //   green  = Running
    //   amber  = Paused or any transitional state (Starting/Saving/etc.)
    //   gray   = Off / Saved / Hibernated / Unknown
    winrt::Windows::UI::Color VmDotColor(hyprv::app::vm::VirtualMachine const& vm);

    // Apply a VM's state to its status-dot Ellipse: sets the Fill to VmDotColor
    // AND pulses the dot's opacity (a looping Storyboard cached in the dot's
    // Tag) while the VM is mid-transition / has a job in flight, so a state
    // change is visible immediately. Idempotent — safe to call every poll.
    void ApplyVmDotState(winrt::Microsoft::UI::Xaml::Shapes::Ellipse const& dot,
                         hyprv::app::vm::VirtualMachine const& vm);

    // Right-click context menu shared by every surface that lets the user
    // act on a VM (rail rows, open-tab headers, welcome-page tiles). All
    // menu items capture vmGuid by value so the menu can outlive the
    // surface that built it. Fully-qualified `winrt::Microsoft::...` —
    // this header is included from translation units that don't `using
    // namespace winrt`, so the shorter `Microsoft::...` alias isn't in
    // scope here.
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout BuildVmContextMenu(
        winrt::hstring const& vmGuid, winrt::hstring const& vmName,
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow);

    // Right-click context menu for a saved Remote Host (rail rows, open-tab
    // headers, welcome cards): Connect / Edit / Forget. address is the host's
    // key; every item captures it by value so the menu outlives its surface.
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout BuildRemoteHostContextMenu(
        winrt::hstring const& address,
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow);

    // Launch the user-configured debugger for `vmGuid`, detached from the
    // rdphost job (CREATE_BREAKAWAY_FROM_JOB) so it survives a hyprv crash/
    // exit. Resolves the exe (per-VM override or global) + args from Settings.
    // Returns the Win32 error (0 = launched; also 0 if args are empty — the
    // caller is expected to gate the affordance on that). Shared by the
    // title-bar button and the context-menu item.
    unsigned long LaunchVmDebugger(std::wstring const& vmGuid);
}
