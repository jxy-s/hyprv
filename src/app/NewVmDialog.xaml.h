#pragma once

#include "NewVmDialog.g.h"

#include <string>

namespace winrt::hyprv_app::implementation
{
    // The welcome-page "New VM..." wizard (a modal ContentDialog). Single-column
    // form: name + generation, memory + processor, storage, network, install
    // media. On Create it validates the inputs, runs VMManager::CreateVM on the
    // UI thread behind a "Creating..." overlay, and stashes the new VM's GUID +
    // name. MainWindow::OpenNewVmDialog reads those back via CreatedVmGuid()/
    // CreatedVmName() after ShowAsync returns Primary and opens a tab for it.
    //
    // Lifecycle:
    //   1. Construct via winrt::make<NewVmDialog>() (or the projected ctor).
    //   2. dlg.XamlRoot(parentXamlRoot); dlg.RequestedTheme(parentTheme);
    //   3. co_await dlg.ShowAsync();
    //   4. If Primary AND CreatedVmGuid() non-empty -> open the VM tab.
    struct NewVmDialog : NewVmDialogT<NewVmDialog>
    {
        NewVmDialog();

        // Result of a successful create — read by MainWindow after ShowAsync.
        // Empty when the user cancelled or the create failed.
        std::wstring CreatedVmGuid() const { return m_createdGuid; }
        std::wstring CreatedVmName() const { return m_createdName; }

        // XAML event handlers — public so the generated .g.cpp can bind them.
        void OnDiskModeChanged(Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnNameChanged(Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        // A "common amount" preset was picked — fill the startup-memory box
        // with that value (the ComboBoxItem's Tag carries the MB amount).
        void OnMemoryPresetChanged(Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnBrowseNewDisk(Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBrowseExistingDisk(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBrowseIso(Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
        // "Store the VM in a different location" — enable/disable the folder
        // row to match the checkbox.
        void OnCustomLocationToggled(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBrowseVmLocation(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        // Fill the switch combo + the default new-VHD directory; seed initial
        // control state. Runs once in the ctor (on the UI thread).
        void PopulateDefaults();
        // Show only the sub-panel matching the selected storage radio.
        void UpdateDiskPanels();
        // Rebuild the auto-derived new-VHD path from the current name (unless
        // the user has hand-edited / browsed for a path).
        void RefreshAutoDiskPath();
        // Surface a validation / create error in the InfoBar.
        void ShowError(std::wstring const& message);

        // Create handler (wired to ContentDialog::PrimaryButtonClick). Validates,
        // runs the create behind the overlay, and either closes (success) or
        // cancels the close + shows an error (failure).
        winrt::fire_and_forget OnPrimaryButtonClick(
            Microsoft::UI::Xaml::Controls::ContentDialog const&,
            Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const&);

        // Host default VHD directory (for the auto-derived new-disk path).
        std::wstring m_defaultVhdDir;
        // True once the user manually edits or browses the new-disk path — stops
        // the name->path auto-fill from clobbering their choice.
        bool m_userSetDiskPath = false;

        // Filled on a successful create.
        std::wstring m_createdGuid;
        std::wstring m_createdName;
    };
}

namespace winrt::hyprv_app::factory_implementation
{
    struct NewVmDialog : NewVmDialogT<NewVmDialog, implementation::NewVmDialog>
    {
    };
}
