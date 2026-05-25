#include "ConfirmDialog.h"

#include "../MainWindow.xaml.h"
#include "../settings/Settings.h"
#include "PopupBackdrop.h"

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>

#include <memory>

extern void HyprvAppLog(const wchar_t* fmt, ...);

namespace
{
    // Show the confirm dialog and run `action` on Primary.
    //
    // CRITICAL: everything this coroutine uses is a by-value PARAMETER, never
    // a lambda capture. Coroutine parameters are copied into the coroutine
    // frame and stay valid across co_await. The previous form —
    // `[captures]() -> fire_and_forget { ...co_await...; action(); }()` — kept
    // its captures in the lambda TEMPORARY, which is destroyed at the end of
    // the full-expression, i.e. the moment the coroutine first suspends at
    // `co_await dlg.ShowAsync()`. After the dialog closed and the coroutine
    // resumed, `action` (and `winStrong`) were dangling, so calling the
    // freed `std::function` AV'd at a non-executable address (BEX64). This
    // bit every confirmation-ON destructive action; it surfaced reliably on
    // "Turn off" because the surrounding popup-suppression churn reused the
    // freed closure memory. Passing as params is the fix.
    winrt::fire_and_forget ShowConfirmCoro(
        winrt::com_ptr<winrt::hyprv_app::implementation::MainWindow> win,
        winrt::Microsoft::UI::Xaml::XamlRoot root,
        winrt::Microsoft::UI::Xaml::ElementTheme parentTheme,
        std::wstring title,
        std::wstring body,
        std::wstring primaryButtonText,
        std::function<void()> action)
    {
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring{ title }));
        dlg.Content(winrt::box_value(winrt::hstring{ body }));
        dlg.PrimaryButtonText(winrt::hstring{ primaryButtonText });
        dlg.CloseButtonText(L"Cancel");
        // DefaultButton=Close: Enter dismisses without firing the destructive
        // path. The user has to click the primary button deliberately.
        dlg.DefaultButton(ContentDialogButton::Close);
        dlg.XamlRoot(root);
        dlg.RequestedTheme(parentTheme);
        // Suppress the rdphost popup (a top-level HWND that paints over the
        // XAML composition surface) for the dialog's lifetime. The scope's
        // dtor runs at coroutine end — `win` lives in the frame, so the
        // MainWindow stays alive for the Pop.
        winrt::hyprv_app::implementation::PopupSuppressionScope suppress(win.get());
        auto res = co_await dlg.ShowAsync();
        if (res == ContentDialogResult::Primary && action) action();
    }

    // Confirmation + opt-in checkbox. The body text + checkbox live in a
    // StackPanel (locals in the coroutine frame, like ShowInputCoro's TextBox),
    // so they survive the co_await; we read the checkbox after the dialog
    // closes. action receives the final checkbox state on Primary.
    winrt::fire_and_forget ShowConfirmCheckboxCoro(
        winrt::com_ptr<winrt::hyprv_app::implementation::MainWindow> win,
        winrt::Microsoft::UI::Xaml::XamlRoot root,
        winrt::Microsoft::UI::Xaml::ElementTheme parentTheme,
        std::wstring title,
        std::wstring body,
        std::wstring checkboxLabel,
        bool checkboxInitiallyChecked,
        std::wstring primaryButtonText,
        std::function<void(bool)> action)
    {
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        TextBlock bodyText;
        bodyText.Text(winrt::hstring{ body });
        bodyText.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);

        CheckBox check;
        check.Content(winrt::box_value(winrt::hstring{ checkboxLabel }));
        check.IsChecked(checkboxInitiallyChecked);
        check.Margin({ 0, 12, 0, 0 });

        StackPanel panel;
        panel.Spacing(2);
        panel.Children().Append(bodyText);
        panel.Children().Append(check);

        ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring{ title }));
        dlg.Content(panel);
        dlg.PrimaryButtonText(winrt::hstring{ primaryButtonText });
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(ContentDialogButton::Close);
        dlg.XamlRoot(root);
        dlg.RequestedTheme(parentTheme);
        winrt::hyprv_app::implementation::PopupSuppressionScope suppress(win.get());
        auto res = co_await dlg.ShowAsync();
        if (res == ContentDialogResult::Primary && action)
        {
            auto r = check.IsChecked();
            action(r ? r.Value() : false);
        }
    }

    // OK-only variant — same machinery, no action. ShowAsync can throw if
    // another ContentDialog is already open (WinUI allows only one); catch +
    // drop rather than crash (a second simultaneous error is rare).
    winrt::fire_and_forget ShowErrorCoro(
        winrt::com_ptr<winrt::hyprv_app::implementation::MainWindow> win,
        winrt::Microsoft::UI::Xaml::XamlRoot root,
        winrt::Microsoft::UI::Xaml::ElementTheme parentTheme,
        std::wstring title,
        std::wstring body)
    {
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring{ title }));
        dlg.Content(winrt::box_value(winrt::hstring{ body }));
        dlg.CloseButtonText(L"OK");
        dlg.DefaultButton(ContentDialogButton::Close);
        dlg.XamlRoot(root);
        dlg.RequestedTheme(parentTheme);
        winrt::hyprv_app::implementation::PopupSuppressionScope suppress(win.get());
        try { co_await dlg.ShowAsync(); }
        catch (...) { HyprvAppLog(L"[error-dialog] ShowAsync threw (another dialog open?)"); }
    }

    // Text-input variant. The TextBox lives in the coroutine frame (a local, not
    // a capture) so it survives the co_await; we read its text after the dialog
    // closes. Trims whitespace and only fires onAccept on a non-empty result.
    winrt::fire_and_forget ShowInputCoro(
        winrt::com_ptr<winrt::hyprv_app::implementation::MainWindow> win,
        winrt::Microsoft::UI::Xaml::XamlRoot root,
        winrt::Microsoft::UI::Xaml::ElementTheme parentTheme,
        std::wstring title,
        std::wstring prompt,
        std::wstring initialValue,
        std::wstring primaryButtonText,
        std::function<void(std::wstring)> onAccept)
    {
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        TextBox box;
        box.Text(winrt::hstring{ initialValue });
        box.SelectAll();
        StackPanel panel;
        panel.Spacing(6);
        if (!prompt.empty())
        {
            TextBlock lbl;
            lbl.Text(winrt::hstring{ prompt });
            lbl.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
            panel.Children().Append(lbl);
        }
        panel.Children().Append(box);

        ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring{ title }));
        dlg.Content(panel);
        dlg.PrimaryButtonText(winrt::hstring{ primaryButtonText });
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(ContentDialogButton::Primary);
        dlg.XamlRoot(root);
        dlg.RequestedTheme(parentTheme);

        // Enter in the TextBox should accept (the ContentDialog's DefaultButton
        // doesn't reliably fire while a focused single-line TextBox is up — it
        // swallows the key). Handle it explicitly: flag + Hide; we treat Hide()
        // here (result None) as an accept via the flag. Weak-capture the dialog
        // to avoid a reference cycle (dialog → TextBox → handler → dialog).
        auto acceptedViaEnter = std::make_shared<bool>(false);
        auto dlgWeak = winrt::make_weak(dlg);
        box.KeyDown([acceptedViaEnter, dlgWeak](
            winrt::Windows::Foundation::IInspectable const&,
            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (e.Key() == winrt::Windows::System::VirtualKey::Enter)
            {
                *acceptedViaEnter = true;
                e.Handled(true);
                if (auto d = dlgWeak.get()) d.Hide();
            }
        });

        winrt::hyprv_app::implementation::PopupSuppressionScope suppress(win.get());
        winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult res{};
        try { res = co_await dlg.ShowAsync(); }
        catch (...) { HyprvAppLog(L"[input-dialog] ShowAsync threw (another dialog open?)"); co_return; }
        if ((res == ContentDialogResult::Primary || *acceptedViaEnter) && onAccept)
        {
            std::wstring val{ box.Text() };
            size_t b = val.find_first_not_of(L" \t\r\n");
            size_t e = val.find_last_not_of(L" \t\r\n");
            std::wstring trimmed = (b == std::wstring::npos) ? std::wstring{} : val.substr(b, e - b + 1);
            if (!trimmed.empty()) onAccept(trimmed);
        }
    }
}

namespace hyprv::app::ui
{
    void ConfirmAndAct(
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::wstring actionKey,
        std::wstring title,
        std::wstring body,
        std::wstring primaryButtonText,
        std::function<void()> action)
    {
        // Honor the user's setting first. If they've opted out, run the
        // action immediately — no dialog, no popup suppression, no
        // coroutine machinery. Logs the bypass at Debug-level via
        // HyprvAppLog so the audit trail still shows what happened.
        if (!hyprv::app::settings::Settings::Instance().ConfirmationEnabled(actionKey))
        {
            HyprvAppLog(L"[confirm] key=%s suppressed — running action",
                actionKey.c_str());
            if (action) action();
            return;
        }

        auto win = weakWindow.get();
        if (!win)
        {
            // Window gone but the action was still requested. Best-effort
            // run — better than silently dropping the click.
            HyprvAppLog(L"[confirm] key=%s window gone — running action",
                actionKey.c_str());
            if (action) action();
            return;
        }
        auto root = win->Content() ? win->Content().XamlRoot() : nullptr;
        // Snapshot the parent's ActualTheme up front so the dialog can
        // be created on the matching theme. ContentDialog renders in its
        // own popup layer and doesn't inherit RequestedTheme from the
        // window's Content automatically — without this, the dialog
        // opens in System theme even when the user has forced Light/Dark.
        auto parentTheme = winrt::Microsoft::UI::Xaml::ElementTheme::Default;
        if (auto fe = win->Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
            parentTheme = fe.ActualTheme();

        // Drive the dialog through a coroutine whose state is all by-value
        // PARAMETERS (see ShowConfirmCoro) — NOT an immediately-invoked
        // capturing lambda, whose captures would dangle after the first
        // co_await. `win` (com_ptr) keeps the MainWindow alive for the
        // popup-suppression scope inside.
        ShowConfirmCoro(win, root, parentTheme,
            std::move(title), std::move(body), std::move(primaryButtonText),
            std::move(action));
    }

    void ConfirmAndActWithCheckbox(
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::wstring actionKey,
        std::wstring title,
        std::wstring body,
        std::wstring checkboxLabel,
        bool checkboxInitiallyChecked,
        std::wstring primaryButtonText,
        std::function<void(bool checked)> action)
    {
        // Suppressed prompt → run with the checkbox's default value (so a
        // "don't ask" override can't silently enable the extra destructive
        // option unless it was already the default).
        if (!hyprv::app::settings::Settings::Instance().ConfirmationEnabled(actionKey))
        {
            HyprvAppLog(L"[confirm] key=%s suppressed — running action (checked=%d)",
                actionKey.c_str(), checkboxInitiallyChecked ? 1 : 0);
            if (action) action(checkboxInitiallyChecked);
            return;
        }

        auto win = weakWindow.get();
        if (!win)
        {
            HyprvAppLog(L"[confirm] key=%s window gone — running action (checked=%d)",
                actionKey.c_str(), checkboxInitiallyChecked ? 1 : 0);
            if (action) action(checkboxInitiallyChecked);
            return;
        }
        auto root = win->Content() ? win->Content().XamlRoot() : nullptr;
        auto parentTheme = winrt::Microsoft::UI::Xaml::ElementTheme::Default;
        if (auto fe = win->Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
            parentTheme = fe.ActualTheme();

        ShowConfirmCheckboxCoro(win, root, parentTheme,
            std::move(title), std::move(body), std::move(checkboxLabel),
            checkboxInitiallyChecked, std::move(primaryButtonText), std::move(action));
    }

    void ShowErrorDialog(
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::wstring title,
        std::wstring body)
    {
        auto win = weakWindow.get();
        if (!win)
        {
            HyprvAppLog(L"[error-dialog] window gone — dropping: %s", body.c_str());
            return;
        }
        auto root = win->Content() ? win->Content().XamlRoot() : nullptr;
        auto parentTheme = winrt::Microsoft::UI::Xaml::ElementTheme::Default;
        if (auto fe = win->Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
            parentTheme = fe.ActualTheme();
        ShowErrorCoro(win, root, parentTheme, std::move(title), std::move(body));
    }

    void ShowInputDialog(
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::wstring title,
        std::wstring prompt,
        std::wstring initialValue,
        std::wstring primaryButtonText,
        std::function<void(std::wstring)> onAccept)
    {
        auto win = weakWindow.get();
        if (!win) { HyprvAppLog(L"[input-dialog] window gone — dropping"); return; }
        auto root = win->Content() ? win->Content().XamlRoot() : nullptr;
        auto parentTheme = winrt::Microsoft::UI::Xaml::ElementTheme::Default;
        if (auto fe = win->Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
            parentTheme = fe.ActualTheme();
        ShowInputCoro(win, root, parentTheme, std::move(title), std::move(prompt),
            std::move(initialValue), std::move(primaryButtonText), std::move(onAccept));
    }
}
