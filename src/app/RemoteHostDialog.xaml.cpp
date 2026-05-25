#include "pch.h"
#include "RemoteHostDialog.xaml.h"
#if __has_include("RemoteHostDialog.g.cpp")
#include "RemoteHostDialog.g.cpp"
#endif

#include "settings/Settings.h"

#include <cmath>
#include <string>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace
{
    std::wstring Trim(std::wstring s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return {};
        size_t b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Do two RDP option sets differ in any field the dialog exposes? (Every
    // RdpOptions field is surfaced, so this is a full comparison.)
    bool RdpDiffers(hyprv::app::settings::RdpOptions const& a,
                    hyprv::app::settings::RdpOptions const& b)
    {
        return a.audioMode            != b.audioMode
            || a.redirectClipboard    != b.redirectClipboard
            || a.redirectDrives       != b.redirectDrives
            || a.redirectDevices      != b.redirectDevices
            || a.redirectSmartCards   != b.redirectSmartCards
            || a.redirectPorts        != b.redirectPorts
            || a.audioCaptureRedirect != b.audioCaptureRedirect
            || a.initialDesktopWidth  != b.initialDesktopWidth
            || a.initialDesktopHeight != b.initialDesktopHeight
            || a.colorDepth           != b.colorDepth
            || a.dpiScaleOverridePercent != b.dpiScaleOverridePercent;
    }
}

namespace winrt::hyprv_app::implementation
{
    RemoteHostDialog::RemoteHostDialog()
    {
        InitializeComponent();
        this->PrimaryButtonClick({ this, &RemoteHostDialog::OnPrimaryButtonClick });
        // Seed the RDP-options expander with the app-wide defaults. An edit
        // overrides this with the host's saved options in InitializeForEdit.
        LoadRdpControls(hyprv::app::settings::Settings::Instance().RdpDefaults());
    }

    void RemoteHostDialog::InitializeForEdit(hstring const& address)
    {
        std::wstring addr = std::wstring{ address };
        if (addr.empty()) return;   // add mode
        auto hostOpt = hyprv::app::settings::Settings::Instance().FindRemoteHost(addr);
        if (!hostOpt) return;       // unknown -> treat as add
        auto const& h = *hostOpt;
        m_isEdit         = true;
        m_originalAddress = h.address;
        m_origHost        = h;
        if (auto b = nameBox())    b.Text(winrt::hstring{ h.name });
        if (auto b = addressBox()) b.Text(winrt::hstring{ h.address });
        if (auto b = userBox())    b.Text(winrt::hstring{ h.username });
        if (auto b = domainBox())  b.Text(winrt::hstring{ h.domain });
        if (auto b = portBox())    b.Value(static_cast<double>(h.port ? h.port : 3389));
        LoadRdpControls(h.rdp);
        Title(box_value(winrt::hstring{ L"Edit remote host" }));
    }

    void RemoteHostDialog::ShowError(std::wstring const& message)
    {
        if (auto bar = errorBar())
        {
            bar.Title(L"Can't save the host");
            bar.Message(winrt::hstring{ message });
            bar.IsOpen(true);
        }
    }

    void RemoteHostDialog::OnPrimaryButtonClick(
        Microsoft::UI::Xaml::Controls::ContentDialog const&,
        Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args)
    {
        if (auto bar = errorBar()) bar.IsOpen(false);

        std::wstring name    = nameBox()    ? Trim(std::wstring{ nameBox().Text() })    : std::wstring{};
        std::wstring address = addressBox() ? Trim(std::wstring{ addressBox().Text() }) : std::wstring{};
        std::wstring user    = userBox()    ? Trim(std::wstring{ userBox().Text() })    : std::wstring{};
        std::wstring domain  = domainBox()  ? Trim(std::wstring{ domainBox().Text() })  : std::wstring{};
        double portv = portBox() ? portBox().Value() : 3389.0;
        uint16_t port = (std::isnan(portv) || portv < 1 || portv > 65535)
                      ? uint16_t{ 3389 } : static_cast<uint16_t>(portv);

        if (address.empty())
        {
            ShowError(L"Enter the computer name or IP address to connect to.");
            args.Cancel(true);
            return;
        }
        // If the address changed (or is new) and it collides with a DIFFERENT
        // existing host, reject — address is the unique key.
        auto& settings = hyprv::app::settings::Settings::Instance();
        if (_wcsicmp(address.c_str(), m_originalAddress.c_str()) != 0)
        {
            if (settings.FindRemoteHost(address))
            {
                ShowError(L"A saved host with that address already exists.");
                args.Cancel(true);
                return;
            }
        }

        hyprv::app::settings::RemoteHost host;
        host.name     = name;
        host.address  = address;
        host.username = user;
        host.domain   = domain;
        host.port     = port;
        // Per-host RDP options come straight from the expander controls (seeded
        // from RdpDefaults on add / the host's saved options on edit).
        host.rdp      = ReadRdpControls();

        // On an edit, recommend a live reconnect when any connection-affecting
        // field changed (a display-name-only edit must NOT churn the session).
        m_reconnect = m_isEdit &&
            ( _wcsicmp(address.c_str(), m_origHost.address.c_str()) != 0
              || port != m_origHost.port
              || _wcsicmp(user.c_str(),   m_origHost.username.c_str()) != 0
              || _wcsicmp(domain.c_str(), m_origHost.domain.c_str())   != 0
              || RdpDiffers(host.rdp, m_origHost.rdp) );

        settings.AddOrUpdateRemoteHost(m_originalAddress, host);
        m_savedAddress = address;
        // Returning without Cancel lets the dialog close with a Primary result.
    }

    // ---- Per-host RDP options: combo/value mapping tables (mirror the App
    // Settings → Remote Desktop section so the two surfaces stay in lockstep).
    namespace
    {
        constexpr uint16_t kColorDepths[] = { 16, 24, 32 };
        constexpr uint16_t kScales[]      = { 0, 100, 125, 150, 175, 200 };
        constexpr uint16_t kResW[] = { 800, 1024, 1280, 1280, 1366, 1600, 1920, 2560 };
        constexpr uint16_t kResH[] = { 600,  768,  720, 1024,  768,  900, 1080, 1440 };
    }

    void RemoteHostDialog::LoadRdpControls(hyprv::app::settings::RdpOptions const& o)
    {
        if (auto c = rdpAudioCombo())
            c.SelectedIndex(static_cast<int>(o.audioMode));   // 0/1/2 == enum
        if (auto c = rdpColorCombo())
        {
            int idx = 2;   // default 32 bpp
            for (int i = 0; i < 3; ++i) if (kColorDepths[i] == o.colorDepth) { idx = i; break; }
            c.SelectedIndex(idx);
        }
        if (auto c = rdpScaleCombo())
        {
            int idx = 0;   // default Auto
            for (int i = 0; i < 6; ++i) if (kScales[i] == o.dpiScaleOverridePercent) { idx = i; break; }
            c.SelectedIndex(idx);
        }
        if (auto c = rdpSizeCombo())
        {
            int idx = 1;   // default 1024 x 768
            for (int i = 0; i < 8; ++i)
                if (kResW[i] == o.initialDesktopWidth && kResH[i] == o.initialDesktopHeight)
                    { idx = i; break; }
            c.SelectedIndex(idx);
        }
        auto setChk = [](auto cb, bool v) { if (cb) cb.IsChecked(v); };
        setChk(rdpClipboardCheck(),  o.redirectClipboard);
        setChk(rdpDrivesCheck(),     o.redirectDrives);
        setChk(rdpDevicesCheck(),    o.redirectDevices);
        setChk(rdpSmartCardsCheck(), o.redirectSmartCards);
        setChk(rdpPortsCheck(),      o.redirectPorts);
        setChk(rdpMicCheck(),        o.audioCaptureRedirect);
    }

    hyprv::app::settings::RdpOptions RemoteHostDialog::ReadRdpControls()
    {
        // Base off the load-time baseline so any future RdpOptions field the
        // dialog doesn't surface is preserved rather than reset.
        hyprv::app::settings::RdpOptions o =
            m_isEdit ? m_origHost.rdp
                     : hyprv::app::settings::Settings::Instance().RdpDefaults();

        if (auto c = rdpAudioCombo())
        {
            int i = c.SelectedIndex();
            if (i >= 0 && i <= 2)
                o.audioMode = static_cast<hyprv::app::settings::RdpOptions::AudioMode>(i);
        }
        if (auto c = rdpColorCombo())
        {
            int i = c.SelectedIndex();
            if (i >= 0 && i < 3) o.colorDepth = kColorDepths[i];
        }
        if (auto c = rdpScaleCombo())
        {
            int i = c.SelectedIndex();
            if (i >= 0 && i < 6) o.dpiScaleOverridePercent = kScales[i];
        }
        if (auto c = rdpSizeCombo())
        {
            int i = c.SelectedIndex();
            if (i >= 0 && i < 8)
            {
                o.initialDesktopWidth  = kResW[i];
                o.initialDesktopHeight = kResH[i];
            }
        }
        auto chk = [](auto cb, bool fb) {
            if (!cb) return fb;
            auto ib = cb.IsChecked();
            return ib ? ib.Value() : fb;
        };
        o.redirectClipboard    = chk(rdpClipboardCheck(),  o.redirectClipboard);
        o.redirectDrives       = chk(rdpDrivesCheck(),     o.redirectDrives);
        o.redirectDevices      = chk(rdpDevicesCheck(),    o.redirectDevices);
        o.redirectSmartCards   = chk(rdpSmartCardsCheck(), o.redirectSmartCards);
        o.redirectPorts        = chk(rdpPortsCheck(),      o.redirectPorts);
        o.audioCaptureRedirect = chk(rdpMicCheck(),        o.audioCaptureRedirect);
        return o;
    }
}
