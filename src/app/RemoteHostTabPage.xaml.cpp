#include "pch.h"
#include "RemoteHostTabPage.xaml.h"
#if __has_include("RemoteHostTabPage.g.cpp")
#include "RemoteHostTabPage.g.cpp"
#endif

#include "rdp/RdpHostClient.h"
#include "settings/Settings.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

extern void HyprvAppLog(const wchar_t* fmt, ...);

namespace
{
    // Locate hyprv-rdphost.exe — SINGLE enforced location, right next to
    // hyprv.exe (in lock-step with VmTabPage::FindRdphost). No fallback search:
    // if it isn't beside hyprv.exe, the spawn fails and the caller surfaces the
    // "not found near hyprv.exe" error.
    std::wstring FindRdphost()
    {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir = self;
        if (auto p = dir.find_last_of(L"\\/"); p != std::wstring::npos) dir.resize(p);
        return dir + L"\\hyprv-rdphost.exe";
    }

    Media::Brush BrushFromTheme(winrt::hstring const& key)
    {
        try
        {
            if (auto v = Application::Current().Resources().TryLookup(winrt::box_value(key)))
                return v.try_as<Media::Brush>();
        }
        catch (...) {}
        return nullptr;
    }
}

namespace winrt::hyprv_app::implementation
{
    RemoteHostTabPage::RemoteHostTabPage()
    {
        InitializeComponent();
        m_uiQueue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        Loaded([this](IInspectable const&, RoutedEventArgs const&) { OnLoaded(); });
    }

    RemoteHostTabPage::~RemoteHostTabPage()
    {
        if (m_client) m_client->Stop();
    }

    void RemoteHostTabPage::Initialize(hstring const& address)
    {
        m_address = std::wstring{ address };
        // Resolve a display name up front (best-effort; refreshed at connect).
        if (auto h = hyprv::app::settings::Settings::Instance().FindRemoteHost(m_address))
            m_displayName = h->name.empty() ? h->address : h->name;
        else
            m_displayName = m_address;
        HyprvAppLog(L"[rh] RemoteHostTabPage initialized for %s", m_address.c_str());
    }

    void RemoteHostTabPage::SetWindowHwnd(HWND hwnd)
    {
        const bool changed = (m_windowHwnd != hwnd);
        m_windowHwnd = hwnd;
        // Tab tear-out: re-own the rdphost popup to the new window (see VmTabPage).
        if (changed && m_client)
            m_client->Reown(hwnd);
    }

    void RemoteHostTabPage::SetTabItem(Microsoft::UI::Xaml::Controls::TabViewItem const& tab)
    {
        m_tabItem = tab;
        UpdateHeader();
    }

    void RemoteHostTabPage::UpdateHeader()
    {
        if (!m_tabItem) return;
        std::wstring header = m_displayName.empty() ? m_address : m_displayName;
        m_tabItem.Header(box_value(winrt::hstring{ header }));
    }

    // ---- popup show / hide (mirrors VmTabPage) -------------------------------
    void RemoteHostTabPage::ShowPopup()
    {
        m_wantsVisible = true;
        // A modal dialog is up — never surface the rdphost popup over it (gated
        // like VmTabPage; SetPopupSuppressed(false) re-runs this on close).
        if (m_popupSuppressed) return;
        if (!m_client || !m_connected) return;
        int sx, sy, bw, bh;
        if (ComputeBorderScreenRect(sx, sy, bw, bh))
        {
            UpdateRdphostBounds();
            m_client->Show();
            m_client->Focus();
            m_deferredShow = false;
        }
        else
        {
            m_deferredShow = true;
        }
    }

    void RemoteHostTabPage::HidePopup()
    {
        m_wantsVisible = false;
        m_deferredShow = false;
        if (m_client) m_client->Hide();
    }

    void RemoteHostTabPage::RefreshPopupBounds() { UpdateRdphostBounds(); }

    void RemoteHostTabPage::SetPopupSuppressed(bool suppressed)
    {
        // The flag gates the async show paths too, so a remote session that
        // connects WHILE a dialog is open can't paint over it (see VmTabPage).
        m_popupSuppressed = suppressed;
        if (suppressed)
        {
            if (m_client) m_client->Hide();
        }
        else if (m_wantsVisible)
        {
            ShowPopup();   // re-evaluate full show path on un-suppress
        }
    }

    void RemoteHostTabPage::ShutdownClient()
    {
        if (m_client)
        {
            HyprvAppLog(L"[rh] ShutdownClient %s", m_address.c_str());
            m_client->Stop();
            m_client.reset();
        }
    }

    void RemoteHostTabPage::TearDownClient()
    {
        if (!m_client) return;
        m_client->Hide();
        m_client->Stop();
        m_client.reset();
        m_connected    = false;
        m_deferredShow = false;
    }

    void RemoteHostTabPage::OnLoaded()
    {
        if (m_loaded) return;
        m_loaded = true;
        if (auto border = rdpHost())
            border.SizeChanged({ this, &RemoteHostTabPage::OnRdpHostSizeChanged });
        UpdateHeader();
        // Connect immediately ONLY for a user-initiated open. A tab restored
        // from saved state on launch shows an idle "Connect" placeholder so we
        // don't fire a credential prompt for every saved remote tab at startup.
        if (m_autoConnect)
            StartConnection();
        else
            ShowIdlePlaceholder();

        if (m_client && m_wantsVisible && m_connected && m_deferredShow && !m_popupSuppressed)
        {
            UpdateRdphostBounds();
            m_client->Show();
            m_client->Focus();
            m_deferredShow = false;
        }
        else if (m_client)
        {
            UpdateRdphostBounds();
        }
    }

    void RemoteHostTabPage::OnRdpHostSizeChanged(Windows::Foundation::IInspectable const&,
                                                 Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        UpdateRdphostBounds();
        if (m_client && m_wantsVisible && m_connected && m_deferredShow && !m_popupSuppressed)
        {
            m_client->Show();
            m_client->Focus();
            m_deferredShow = false;
        }
    }

    bool RemoteHostTabPage::ComputeBorderScreenRect(int& sx, int& sy, int& w, int& h)
    {
        if (!m_windowHwnd) return false;
        auto border = rdpHost();
        if (!border) return false;
        const double aw = border.ActualWidth();
        const double ah = border.ActualHeight();
        if (aw <= 0 || ah <= 0) return false;
        auto root = XamlRoot() ? XamlRoot().Content() : nullptr;
        if (!root) return false;
        auto transform = border.TransformToVisual(root);
        auto topLeft = transform.TransformPoint(Windows::Foundation::Point{ 0.f, 0.f });
        auto bottomRight = transform.TransformPoint(
            Windows::Foundation::Point{ (float)aw, (float)ah });
        UINT dpi = GetDpiForWindow(m_windowHwnd);
        double scale = dpi / 96.0;
        POINT origin{ 0, 0 };
        ClientToScreen(m_windowHwnd, &origin);
        long sxL = origin.x + std::lround(topLeft.X * scale);
        long syL = origin.y + std::lround(topLeft.Y * scale);
        long exL = origin.x + std::lround(bottomRight.X * scale);
        long eyL = origin.y + std::lround(bottomRight.Y * scale);
        sx = static_cast<int>(sxL);
        sy = static_cast<int>(syL);
        w  = static_cast<int>(exL - sxL);
        h  = static_cast<int>(eyL - syL);
        return true;
    }

    void RemoteHostTabPage::UpdateRdphostBounds()
    {
        int sx = 0, sy = 0, bw = 0, bh = 0;
        if (!ComputeBorderScreenRect(sx, sy, bw, bh)) return;
        if (!m_client) return;
        int w = m_sessionW, h = m_sessionH;
        if (w <= 0) w = 1024;
        if (h <= 0) h = 768;
        if (w > bw) w = bw;
        if (h > bh) h = bh;
        sx += (bw - w) / 2;
        sy += (bh - h) / 2;
        m_client->Reposition(sx, sy, w, h);
    }

    void RemoteHostTabPage::StartConnection()
    {
        if (m_client) return;   // idempotent
        if (!m_windowHwnd) return;

        auto hostOpt = hyprv::app::settings::Settings::Instance().FindRemoteHost(m_address);
        if (!hostOpt)
        {
            m_hadConnectError = true;
            m_lastErrorText   = L"This remote host is no longer in your saved list.";
            ShowDisconnectedPlaceholder(m_lastErrorText, true);
            return;
        }
        auto host = *hostOpt;
        m_displayName = host.name.empty() ? host.address : host.name;
        UpdateHeader();

        const auto rdphostPath = FindRdphost();
        if (GetFileAttributesW(rdphostPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            m_hadConnectError = true;
            m_lastErrorText   = L"hyprv-rdphost.exe was not found next to hyprv.exe.";
            ShowDisconnectedPlaceholder(m_lastErrorText, true);
            return;
        }

        m_client = std::make_unique<hyprv::app::RdpHostClient>(rdphostPath);
        m_client->SetLogLabel(m_displayName);

        auto queue    = m_uiQueue;
        auto weakSelf = get_weak();

        m_client->OnConnecting = [queue, weakSelf]
        {
            queue.TryEnqueue([weakSelf] {
                if (auto self = weakSelf.get()) self->SetStatus(L"Connecting...");
            });
        };
        m_client->OnConnected = [queue, weakSelf](hyprv::app::ConnectedInfo const& c)
        {
            HyprvAppLog(L"[rh] OnConnected %ux%u", c.desktopWidth, c.desktopHeight);
            queue.TryEnqueue([weakSelf, c] {
                auto self = weakSelf.get();
                if (!self) return;
                self->m_connected       = true;
                self->m_connecting      = false;
                self->m_hadConnectError = false;
                if (c.desktopWidth > 0 && c.desktopHeight > 0)
                {
                    self->m_sessionW = static_cast<int>(c.desktopWidth);
                    self->m_sessionH = static_cast<int>(c.desktopHeight);
                }
                if (auto root = self->placeholderRoot())
                    root.Visibility(Visibility::Collapsed);
                if (self->m_client) self->m_client->AttachInput();
                if (self->m_wantsVisible && self->m_client && !self->m_popupSuppressed)
                {
                    self->UpdateRdphostBounds();
                    self->m_client->Show();
                }
            });
        };
        m_client->OnDesktopResized = [queue, weakSelf](uint32_t w, uint32_t h)
        {
            queue.TryEnqueue([weakSelf, w, h] {
                auto self = weakSelf.get();
                if (!self) return;
                if (w > 0 && h > 0)
                {
                    self->m_sessionW = static_cast<int>(w);
                    self->m_sessionH = static_cast<int>(h);
                    self->UpdateRdphostBounds();
                }
            });
        };
        m_client->OnDisconnected = [queue, weakSelf](hyprv::app::DisconnectedInfo const& d)
        {
            const bool         fatal = d.fatal;
            const std::wstring desc  = d.description;
            HyprvAppLog(L"[rh] OnDisconnected disc=%d ext=%d fatal=%d",
                d.discReason, d.extendedReason, fatal ? 1 : 0);
            queue.TryEnqueue([weakSelf, fatal, desc] {
                auto self = weakSelf.get();
                if (!self) return;
                const bool wasConnected = self->m_connected;
                self->m_connected  = false;
                self->m_connecting = false;
                if (self->m_client) self->m_client->Hide();
                if (wasConnected)
                {
                    // The session ended (logoff / idle timeout / server close).
                    // Offer a Reconnect — not an error.
                    self->m_hadConnectError = false;
                    self->m_lastErrorText.clear();
                    self->ShowDisconnectedPlaceholder(std::wstring{}, false);
                }
                else
                {
                    // The connect attempt never established. Surface it.
                    self->m_hadConnectError = true;
                    self->m_lastErrorText = desc.empty()
                        ? std::wstring{ L"Couldn't connect to the remote host." }
                        : desc;
                    self->ShowDisconnectedPlaceholder(self->m_lastErrorText, true);
                }
            });
        };
        m_client->OnError = [queue, weakSelf](uint32_t code)
        {
            queue.TryEnqueue([weakSelf, code] {
                auto self = weakSelf.get();
                if (!self) return;
                std::wstring msg;
                switch (static_cast<hyprv::ipc::RdpErrorCode>(code))
                {
                case hyprv::ipc::RdpErrorCode::HostStartupFailed:
                    msg = L"The session host couldn't start the connection to this host.";
                    break;
                case hyprv::ipc::RdpErrorCode::ProtocolError:
                    msg = L"An internal protocol error occurred between hyprv and its session host.";
                    break;
                default:
                    msg = std::format(L"The remote session reported an error (code {}).", code);
                    break;
                }
                self->m_connecting      = false;
                self->m_hadConnectError = true;
                self->m_lastErrorText   = msg;
                self->ShowDisconnectedPlaceholder(msg, true);
            });
        };
        m_client->OnChildExited = [queue, weakSelf](DWORD code)
        {
            queue.TryEnqueue([weakSelf, code] {
                if (auto self = weakSelf.get())
                    self->SetStatus(winrt::hstring{ std::format(L"rdphost exited (0x{:x})", code) });
            });
        };

        // --- Build the wire options from the host's RDP settings ---
        hyprv::ipc::RdpOptions opts{};
        opts.port = host.port ? host.port : 3389;

        // Initial desktop size = the current Border (window) size so the remote
        // desktop opens at the window resolution. Falls back to the host's saved
        // initial size if the Border isn't measured yet.
        int sx = 0, sy = 0, bw = 0, bh = 0;
        const bool borderValid = ComputeBorderScreenRect(sx, sy, bw, bh);
        int dw = (borderValid && bw > 0) ? bw : host.rdp.initialDesktopWidth;
        int dh = (borderValid && bh > 0) ? bh : host.rdp.initialDesktopHeight;
        if (dw <= 0) dw = 1024;
        if (dh <= 0) dh = 768;
        if (dw > 65535) dw = 65535;
        if (dh > 65535) dh = 65535;
        opts.desktopWidth  = static_cast<uint16_t>(dw);
        opts.desktopHeight = static_cast<uint16_t>(dh);
        opts.colorDepth    = host.rdp.colorDepth;

        uint32_t flags = 0;
        if (host.rdp.redirectClipboard)    flags |= hyprv::ipc::Flag_RedirectClipboard;
        if (host.rdp.redirectDrives)       flags |= hyprv::ipc::Flag_RedirectDrives;
        if (host.rdp.redirectDevices)      flags |= hyprv::ipc::Flag_RedirectDevices;
        if (host.rdp.redirectSmartCards)   flags |= hyprv::ipc::Flag_RedirectSmartCards;
        if (host.rdp.redirectPorts)        flags |= hyprv::ipc::Flag_RedirectPorts;
        if (host.rdp.audioCaptureRedirect) flags |= hyprv::ipc::Flag_AudioCaptureRedirect;
        opts.flags     = flags;
        opts.audioMode = static_cast<uint8_t>(host.rdp.audioMode);

        m_dpiOverridePercent = host.rdp.dpiScaleOverridePercent;
        UINT dpiPx = m_windowHwnd ? GetDpiForWindow(m_windowHwnd) : 96;
        opts.dpiScalePercent = m_dpiOverridePercent
            ? m_dpiOverridePercent
            : static_cast<uint16_t>((dpiPx * 100 + 48) / 96);

        m_sessionW = opts.desktopWidth;
        m_sessionH = opts.desktopHeight;

        // Popup rect: centered in the Border, sized to the session.
        if (!borderValid)
        {
            POINT origin{ 0, 0 };
            ClientToScreen(m_windowHwnd, &origin);
            sx = origin.x; sy = origin.y + 32;
            bw = m_sessionW; bh = m_sessionH;
        }
        int w = m_sessionW, h = m_sessionH;
        if (w > bw) w = bw;
        if (h > bh) h = bh;
        const int px = sx + (bw - w) / 2;
        const int py = sy + (bh - h) / 2;

        m_connecting      = true;
        m_hadConnectError = false;
        ShowConnectingPlaceholder();

        HyprvAppLog(L"[rh] connecting to %s:%u user=%s dw=%d dh=%d",
            host.address.c_str(), opts.port, host.username.c_str(), dw, dh);

        if (!m_client->Connect(m_windowHwnd, px, py, w, h, opts,
                               host.address, host.domain, host.username))
        {
            m_client.reset();
            m_connecting      = false;
            m_hadConnectError = true;
            m_lastErrorText   = L"Couldn't start the remote session host.";
            ShowDisconnectedPlaceholder(m_lastErrorText, true);
        }
    }

    // ---- placeholder rendering ----------------------------------------------
    void RemoteHostTabPage::RenderPlaceholder(std::wstring const& state,
                                              std::wstring const& hint,
                                              std::wstring const& actionLabel,
                                              wchar_t actionGlyph,
                                              bool showProgress, bool isError)
    {
        if (!placeholderRoot()) return;
        placeholderHostName().Text(winrt::hstring{
            m_displayName.empty() ? m_address : m_displayName });
        placeholderState().Text(winrt::hstring{ state });
        if (auto b = BrushFromTheme(isError ? L"SystemFillColorCriticalBrush"
                                            : L"TextFillColorSecondaryBrush"))
            placeholderState().Foreground(b);

        if (hint.empty())
        {
            placeholderHint().Visibility(Visibility::Collapsed);
        }
        else
        {
            placeholderHint().Text(winrt::hstring{ hint });
            placeholderHint().Visibility(Visibility::Visible);
        }

        if (auto pr = placeholderProgress())
        {
            pr.IsActive(showProgress);
            pr.Visibility(showProgress ? Visibility::Visible : Visibility::Collapsed);
        }

        if (actionLabel.empty())
        {
            placeholderAction().Visibility(Visibility::Collapsed);
        }
        else
        {
            placeholderActionText().Text(winrt::hstring{ actionLabel });
            placeholderActionGlyph().Glyph(winrt::hstring{ std::wstring(1, actionGlyph) });
            placeholderAction().Visibility(Visibility::Visible);
        }
        placeholderRoot().Visibility(Visibility::Visible);
    }

    void RemoteHostTabPage::ShowConnectingPlaceholder()
    {
        // Spinner, no action button while the connect is in flight.
        RenderPlaceholder(L"Connecting…", std::wstring{}, std::wstring{},
                          L'\0', /*showProgress*/ true, /*isError*/ false);
    }

    void RemoteHostTabPage::ShowDisconnectedPlaceholder(std::wstring const& detail, bool isError)
    {
        const std::wstring state = isError ? L"Connection failed" : L"Disconnected";
        const std::wstring label = isError ? L"Retry" : L"Reconnect";
        // 0xE72C = Segoe MDL2 Refresh glyph for the Retry / Reconnect button.
        RenderPlaceholder(state, detail, label, L'\xE72C',
                          /*showProgress*/ false, isError);
    }

    void RemoteHostTabPage::ShowIdlePlaceholder()
    {
        // Restored-but-not-yet-connected: host name + a Connect button.
        // 0xE8AF = Segoe MDL2 Remote glyph.
        RenderPlaceholder(L"Not connected",
                          L"Click Connect to start the remote session.",
                          L"Connect", L'\xE8AF',
                          /*showProgress*/ false, /*isError*/ false);
    }

    void RemoteHostTabPage::OnPlaceholderActionClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        HyprvAppLog(L"[rh] action clicked %s", m_address.c_str());
        m_hadConnectError = false;
        m_lastErrorText.clear();
        if (m_client) TearDownClient();
        StartConnection();
    }

    void RemoteHostTabPage::ReapplyConnectionSettings()
    {
        // Only a live (connecting/connected) session needs respawning; an idle
        // or errored tab reads the fresh record on its next manual Connect.
        if (!m_client) return;
        HyprvAppLog(L"[rh] ReapplyConnectionSettings %s — respawning rdphost",
            m_address.c_str());
        m_hadConnectError = false;
        m_lastErrorText.clear();
        TearDownClient();
        StartConnection();
    }

    void RemoteHostTabPage::SetStatus(hstring const& text)
    {
        HyprvAppLog(L"[rh status] %s", text.c_str());
    }
}
