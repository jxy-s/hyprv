#include "WindowManager.h"

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.Foundation.h>

#include <vector>

namespace hyprv::app::ui
{
    namespace
    {
        // The one strong ref per live window. Function-local static so it's
        // constructed on first use (avoids a static-init-order question with the
        // winrt apartment). Single UI thread ⇒ no synchronization.
        std::vector<winrt::Microsoft::UI::Xaml::Window>& Windows()
        {
            static std::vector<winrt::Microsoft::UI::Xaml::Window> s;
            return s;
        }
    }

    void WindowManager::Track(winrt::Microsoft::UI::Xaml::Window const& w)
    {
        if (!w) return;
        Windows().push_back(w);
        // Drop our strong ref when the window closes. The sender IS the window,
        // so match on it (capturing `w` would keep the entry's own copy alive in
        // the lambda and defeat the erase).
        w.Closed([](winrt::Windows::Foundation::IInspectable const& sender,
                    winrt::Microsoft::UI::Xaml::WindowEventArgs const&)
        {
            auto win = sender.try_as<winrt::Microsoft::UI::Xaml::Window>();
            if (!win) return;
            std::erase_if(Windows(),
                [&](winrt::Microsoft::UI::Xaml::Window const& e) { return e == win; });
        });
    }

    winrt::Microsoft::UI::Xaml::Window WindowManager::Find(winrt::Microsoft::UI::WindowId id)
    {
        for (auto const& w : Windows())
        {
            auto aw = w.AppWindow();
            if (aw && aw.Id().Value == id.Value)
                return w;
        }
        return nullptr;
    }

    std::vector<winrt::Microsoft::UI::Xaml::Window> WindowManager::All()
    {
        return Windows();   // copy
    }

    std::size_t WindowManager::Count() { return Windows().size(); }
}
