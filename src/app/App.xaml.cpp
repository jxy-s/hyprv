#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

#include "vm/VMManager.h"
#include "settings/Settings.h"
#include "ui/WindowManager.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace winrt::hyprv_app::implementation
{
    /// <summary>
    /// Initializes the singleton application object.  This is the first line of authored code
    /// executed, and as such is the logical equivalent of main() or WinMain().
    /// </summary>
    App::App()
    {
        // Xaml objects should not call InitializeComponent during construction.
        // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    /// <summary>
    /// Invoked when the application is launched.
    /// </summary>
    /// <param name="e">Details about the launch request and process.</param>
    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // Bring up Settings first — its ctor reads %LOCALAPPDATA%\hyprv\
        // settings.json and creates the directory if missing. HyprvAppLog
        // gates on Settings::LoggingEnabled() and looks up the log file path
        // via Settings::FilePath(), so anything that wants to log later
        // (including VMManager construction) needs Settings online first.
        hyprv::app::settings::Settings::Instance();

        // Kick the WMI poll thread BEFORE constructing MainWindow. VMManager's
        // ctor spawns the polling thread which immediately queries Hyper-V for
        // the full VM list — that takes 200-500ms. Doing it here means the
        // query runs in parallel with MainWindow's XAML markup + page-control
        // construction, so by the time MainWindow's first OnActivated fires
        // the rail's RenderRail call has fresh data instead of an empty list.
        hyprv::app::vm::VMManager::Instance();

        window = make<MainWindow>();
        // Track the primary in the window registry so tab tear-out can keep
        // secondary windows alive uniformly + resolve windows by id.
        hyprv::app::ui::WindowManager::Track(window);
        window.Activate();
    }
}
