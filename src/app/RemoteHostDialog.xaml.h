#pragma once

#include "RemoteHostDialog.g.h"

#include "settings/Settings.h"

#include <string>

namespace winrt::hyprv_app::implementation
{
    // Add / edit a saved Remote Host (a modal ContentDialog). Compact form:
    // display name, computer (address), port, user name, domain. NO password —
    // the "prompt every time" model means mstscax asks on connect. On Save it
    // validates + writes the host through Settings::AddOrUpdateRemoteHost and
    // stashes the saved address; the opener reads SavedAddress() after ShowAsync
    // to open / refresh the host's tab + rail entry.
    //
    // Lifecycle:
    //   1. winrt::make<RemoteHostDialog>(); set XamlRoot + RequestedTheme.
    //   2. dlg.InitializeForEdit(address)  // "" = add new
    //   3. co_await dlg.ShowAsync();
    //   4. If Primary AND SavedAddress() non-empty -> open/refresh the tab.
    struct RemoteHostDialog : RemoteHostDialogT<RemoteHostDialog>
    {
        RemoteHostDialog();

        // Pre-fill for editing an existing host (by address key); "" = add new.
        void InitializeForEdit(hstring const& address);

        // The address of the host saved on Primary (empty if cancelled).
        hstring SavedAddress() const { return hstring{ m_savedAddress }; }

        // True after a Primary save on an EDIT where a connection-affecting field
        // (address / port / user / domain / any RDP option) changed. The opener
        // reads this after ShowAsync to reconnect the host's open tab so the
        // change applies without a manual close + reopen.
        bool ReconnectRecommended() const { return m_reconnect; }

    private:
        void ShowError(std::wstring const& message);
        // Populate the RDP-options expander controls from an options set
        // (RdpDefaults on add; the host's saved options on edit).
        void LoadRdpControls(hyprv::app::settings::RdpOptions const& o);
        // Read the RDP-options expander controls back into an options set.
        // (Non-const: the generated x:Name element accessors are non-const.)
        hyprv::app::settings::RdpOptions ReadRdpControls();
        // Wired to ContentDialog::PrimaryButtonClick. Synchronous — validates +
        // writes through Settings; cancels the close + shows the InfoBar on a
        // validation failure.
        void OnPrimaryButtonClick(
            Microsoft::UI::Xaml::Controls::ContentDialog const&,
            Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const&);

        // When editing, the host's original address key (so AddOrUpdate replaces
        // the right entry even if the address itself changed). Empty = add mode.
        std::wstring m_originalAddress;
        // Carries the existing host's per-host RDP options across an edit so a
        // save doesn't reset them to defaults (the dialog doesn't expose them).
        bool         m_isEdit = false;
        std::wstring m_savedAddress;
        // The host as loaded for editing (for the connection-affecting diff that
        // drives ReconnectRecommended). Default-constructed in add mode.
        hyprv::app::settings::RemoteHost m_origHost{};
        bool         m_reconnect = false;
    };
}

namespace winrt::hyprv_app::factory_implementation
{
    struct RemoteHostDialog : RemoteHostDialogT<RemoteHostDialog, implementation::RemoteHostDialog>
    {
    };
}
