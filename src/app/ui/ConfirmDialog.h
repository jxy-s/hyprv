// ConfirmDialog — single entry point for "are you sure?" prompts.
//
// Every destructive action in hyprv should funnel through ConfirmAndAct so
// that:
//   1. The user's per-action Settings.confirmations override is honored
//      (action runs immediately when the setting says "don't ask"), and
//   2. The dialog is bookended with PopupSuppressionScope so it renders
//      above any active rdphost popup (top-level child-process HWND that
//      otherwise eclipses the XAML composition surface).
//
// The action callback is run on the UI thread (the coroutine resumes on
// the calling apartment).
//
// Action keys are documented in Settings::ConfirmationEnabled's defaults
// switch — add a new entry there at the time of first use.

#pragma once

#include <functional>
#include <string>

#include <winrt/Windows.Foundation.h>

namespace winrt::hyprv_app::implementation { struct MainWindow; }

namespace hyprv::app::ui
{
    void ConfirmAndAct(
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::wstring actionKey,
        std::wstring title,
        std::wstring body,
        std::wstring primaryButtonText,
        std::function<void()> action);

    // ConfirmAndAct variant with an extra opt-in checkbox (e.g. "Also delete the
    // virtual hard disks"). The action receives the checkbox's final state.
    // Honors the same per-action confirmation override as ConfirmAndAct — when
    // the user has opted out of the prompt, the action runs immediately with the
    // checkbox's value defaulted to `checkboxInitiallyChecked` (so a suppressed
    // prompt can't silently trigger the extra destructive option unless it was
    // the default). Must be called on the UI thread.
    void ConfirmAndActWithCheckbox(
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::wstring actionKey,
        std::wstring title,
        std::wstring body,
        std::wstring checkboxLabel,
        bool checkboxInitiallyChecked,
        std::wstring primaryButtonText,
        std::function<void(bool checked)> action);

    // A simple OK-only error dialog (no action, no confirmation setting),
    // sharing ConfirmAndAct's XamlRoot/theme/popup-suppression machinery.
    // Used to surface async failures (e.g. a VM state-change job that fails).
    // Must be called on the UI thread. No-ops if the window is gone or another
    // dialog is already open (logged).
    void ShowErrorDialog(
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::wstring title,
        std::wstring body);

    // A single-line text-input dialog (e.g. rename). Pre-fills `initialValue`
    // (pre-selected for easy replace); on the primary button runs
    // `onAccept(enteredText)` (trimmed text, only if non-empty). Shares the same
    // XamlRoot/theme/popup-suppression machinery. Must be called on the UI thread.
    void ShowInputDialog(
        winrt::weak_ref<winrt::hyprv_app::implementation::MainWindow> const& weakWindow,
        std::wstring title,
        std::wstring prompt,
        std::wstring initialValue,
        std::wstring primaryButtonText,
        std::function<void(std::wstring)> onAccept);
}
