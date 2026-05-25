// PopupBackdrop — small helper for syncing ContentDialog / FlyoutBase
// SystemBackdrop to the user's selected Mica vs Acrylic choice.
//
// Popup-hosted controls (ContentDialog, MenuFlyout, etc) render in a
// separate composition layer that does NOT inherit the Window's
// SystemBackdrop. To keep dialogs / menus visually part of the same
// surface family as the main window, each one needs its own
// SystemBackdrop set explicitly before it's shown.
//
// We use the framework's built-in MicaBackdrop / DesktopAcrylicBackdrop
// wrappers for popups — not our custom MicaController / DesktopAcrylicController
// (which the main window uses for always-active behavior + user-tunable
// TintOpacity). Popups therefore don't carry the user's intensity slider
// through, but they do match the chosen backdrop family. That's a
// deliberate trade — sharing a controller across multiple targets is
// involved enough that it's not worth the complexity for what the user
// sees as a brief popup.

#pragma once

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace hyprv::app::ui
{
    // Returns a fresh SystemBackdrop instance suitable for assigning to a
    // FlyoutBase, choosing between MicaBackdrop and DesktopAcrylicBackdrop
    // based on Settings::AppearancePref().backdrop. Caller assigns via
    // target.SystemBackdrop(...) before showing.
    winrt::Microsoft::UI::Xaml::Media::SystemBackdrop PopupBackdropFor();

    // Set SystemBackdrop on the supplied flyout. No-op on older SDKs
    // without the property. (ContentDialog does NOT expose
    // SystemBackdrop in WindowsAppSDK 2.1, so dialogs paint with their
    // theme-default ContentDialogBackground brush — that's a framework
    // limitation, not something we can route around without a custom
    // template override.)
    void ApplyTo(winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutBase const& flyout);
}
