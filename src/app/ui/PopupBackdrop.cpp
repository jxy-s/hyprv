#include "PopupBackdrop.h"

#include "../settings/Settings.h"

namespace hyprv::app::ui
{
    winrt::Microsoft::UI::Xaml::Media::SystemBackdrop PopupBackdropFor()
    {
        auto a = hyprv::app::settings::Settings::Instance().AppearancePref();
        if (a.backdrop == hyprv::app::settings::Appearance::Backdrop::Mica)
            return winrt::Microsoft::UI::Xaml::Media::MicaBackdrop{};
        return winrt::Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{};
    }

    void ApplyTo(winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutBase const& flyout)
    {
        try { flyout.SystemBackdrop(PopupBackdropFor()); }
        catch (...) { /* older SDK or unsupported — fall back to default */ }
    }
}
