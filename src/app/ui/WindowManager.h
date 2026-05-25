#pragma once

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.h>

#include <cstddef>
#include <vector>

namespace hyprv::app::ui
{
    // Process-wide registry of open MainWindows. WinUI does NOT keep secondary
    // (tab-torn-off) windows alive for you — without a strong ref somewhere a
    // freshly-created Window is destroyed the moment the local handle drops. The
    // registry holds one strong ref per window and releases it on the window's
    // Closed event. Used by tab tear-out to (a) keep torn-off windows alive and
    // (b) resolve the framework-created window from the WindowId handed back by
    // TabTearOutWindowRequested. All windows live on the single UI thread, so no
    // locking is needed.
    struct WindowManager
    {
        // Add a window + auto-remove on its Closed event. Call right after
        // make<MainWindow>() (for the primary in App::OnLaunched, and for each
        // torn-off secondary).
        static void Track(winrt::Microsoft::UI::Xaml::Window const& w);

        // Resolve a tracked window by its AppWindow id. Returns nullptr if absent.
        static winrt::Microsoft::UI::Xaml::Window Find(winrt::Microsoft::UI::WindowId id);

        // A snapshot copy of all tracked windows (used to locate the MainWindow
        // owning a given TabView during a cross-window tab drop).
        static std::vector<winrt::Microsoft::UI::Xaml::Window> All();

        // Number of windows currently tracked.
        static std::size_t Count();
    };
}
