#pragma once

#include "RemoteHostTabPage.g.h"

#include <memory>
#include <string>

namespace hyprv::app { class RdpHostClient; }

namespace winrt::hyprv_app::implementation
{
    // UserControl that owns one saved Remote Host's RDP session — its rdpHost
    // popup, its RdpHostClient + rdphost.exe child, and the cross-process
    // window positioning. A leaner sibling of VmTabPage: no Hyper-V VM state,
    // no enhanced-session negotiation, no polling. It connects on load and
    // re-establishes via a single action button.
    struct RemoteHostTabPage : RemoteHostTabPageT<RemoteHostTabPage>
    {
        RemoteHostTabPage();
        ~RemoteHostTabPage();

        // Bind this page to a saved remote host (by its address key). The full
        // record is resolved from Settings at connect time so edits are picked
        // up. Must be called right after construction.
        void Initialize(hstring const& address);
        hstring HostAddress() const { return hstring{ m_address }; }

        // Whether to connect automatically when the page first loads. TRUE for a
        // user-initiated open (welcome card / rail double-click) — connect now.
        // FALSE for a tab restored from saved state on launch — show an idle
        // "Connect" placeholder instead so we don't fire a credential prompt for
        // every saved remote tab at startup. Set before the page is mounted.
        void SetAutoConnect(bool v) { m_autoConnect = v; }

        // MainWindow drops the owning Window's HWND in before adding the page
        // to the TabView — popup positioning translates client-rect DIPs into
        // screen pixels.
        void SetWindowHwnd(HWND hwnd);
        // Connect the page to its TabViewItem so it can keep its own header
        // text in sync without MainWindow mediating.
        void SetTabItem(Microsoft::UI::Xaml::Controls::TabViewItem const& tab);

        // Tab-activation lifecycle — MainWindow calls Show/Hide on selection
        // change so only the active page's popup is visible.
        void ShowPopup();
        void HidePopup();
        void RefreshPopupBounds();
        // Reference-counted temporary hide while a modal ContentDialog is open
        // (the mstscax popup is a top-level HWND that paints above XAML).
        void SetPopupSuppressed(bool suppressed);

        // Deterministic teardown of the rdphost child + IPC pipe. MainWindow
        // calls this from OnTabCloseRequested before removing the TabViewItem.
        void ShutdownClient();

        // Apply changed per-host settings (RDP options / credentials) to a LIVE
        // session by respawning the rdphost child — StartConnection re-resolves
        // the host from Settings. No-op if no client is up (the next manual
        // Connect reads the fresh record). The edit dialog calls this on Save.
        void ReapplyConnectionSettings();

        // XAML event — the Connect / Reconnect / Retry button on the placeholder.
        void OnPlaceholderActionClick(Windows::Foundation::IInspectable const&,
                                      Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void OnLoaded();
        void OnRdpHostSizeChanged(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        bool ComputeBorderScreenRect(int& sx, int& sy, int& w, int& h);
        void UpdateRdphostBounds();
        void StartConnection();
        void TearDownClient();
        void SetStatus(hstring const& text);
        void UpdateHeader();

        // Placeholder presentations. RenderPlaceholder is the shared core;
        // the three wrappers set the right state/hint/action for each phase.
        void RenderPlaceholder(std::wstring const& state, std::wstring const& hint,
                               std::wstring const& actionLabel, wchar_t actionGlyph,
                               bool showProgress, bool isError);
        void ShowConnectingPlaceholder();
        void ShowDisconnectedPlaceholder(std::wstring const& detail, bool isError);
        // Idle state for a restored tab that hasn't connected yet: host name +
        // a "Connect" button. The user clicks to start the session.
        void ShowIdlePlaceholder();

        // ---- Per-tab state -----------------------------------------------
        std::wstring                                  m_address;     // host key
        std::wstring                                  m_displayName; // resolved name/address
        std::unique_ptr<hyprv::app::RdpHostClient>    m_client;
        HWND                                          m_windowHwnd = nullptr;
        bool                                          m_loaded     = false;
        bool                                          m_connected  = false;
        bool                                          m_wantsVisible = false;
        bool                                          m_deferredShow = false;
        // True while a modal dialog is open (owner pushed popup suppression).
        // Gates every show path so a remote session connecting WHILE the dialog
        // is up can't paint its popup over it. See VmTabPage::m_popupSuppressed.
        bool                                          m_popupSuppressed = false;
        // A connect attempt is in flight (between StartConnection and the first
        // Connected/Disconnected/Error). Gates the "Connecting..." placeholder.
        bool                                          m_connecting = false;
        // Whether to connect on first load (see SetAutoConnect). Default true.
        bool                                          m_autoConnect = true;
        // Latched when we've surfaced a connect error so poll-independent
        // re-renders keep the error visible until the user retries. Cleared on
        // a successful connect or a user action.
        bool                                          m_hadConnectError = false;
        std::wstring                                  m_lastErrorText;
        // mstscax-reported session render size; popup is sized to this, centered
        // in the rdpHost Border. Seeded from the connect-time Border size.
        int                                           m_sessionW = 1024;
        int                                           m_sessionH = 768;
        // User's display-scale override (percent; 0 = Auto = follow host DPI).
        uint16_t                                      m_dpiOverridePercent = 0;
        Microsoft::UI::Dispatching::DispatcherQueue   m_uiQueue{ nullptr };
        Microsoft::UI::Xaml::Controls::TabViewItem    m_tabItem{ nullptr };
    };
}

namespace winrt::hyprv_app::factory_implementation
{
    struct RemoteHostTabPage : RemoteHostTabPageT<RemoteHostTabPage, implementation::RemoteHostTabPage>
    {
    };
}
