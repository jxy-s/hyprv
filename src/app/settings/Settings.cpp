#include "pch.h"
#include "Settings.h"

#include <shlobj.h>           // SHGetKnownFolderPath, FOLDERID_LocalAppData
#include <winrt/Windows.Data.Json.h>

#include <fstream>
#include <sstream>

// HyprvAppLog is defined in MainWindow.xaml.cpp — gate-checked against our
// own Settings::LoggingEnabled() inside that function. Used here only for
// load/save error reporting (which we want visible in Debug builds even if
// the user has disabled logging on a clean reinstall — but if they have it
// disabled, the call just no-ops, which is fine).
extern void HyprvAppLog(const wchar_t* fmt, ...);

namespace
{
    // Resolve the per-user local app-data dir for hyprv (the REAL %LOCALAPPDATA%\hyprv),
    // creating it on first run.
    //
    // KF_FLAG_NO_PACKAGE_REDIRECTION is REQUIRED on MSIX. Without it,
    // SHGetKnownFolderPath returns the package-redirected LocalCache path
    // (...\Packages\<pfn>\LocalCache\Local\hyprv); the app would write there, and the
    // Package.appxmanifest write-virtualization exclusion — which targets the REAL
    // $(KnownFolder:LocalAppData)\hyprv — wouldn't match, so the redirect would stand.
    // With the flag we get the real %LOCALAPPDATA%\hyprv, which is exactly the dir the
    // exclusion un-virtualizes, so settings.json + hyprv.log land in the real,
    // findable location (and persist across reinstall).
    //
    // Two halves, both needed: this flag fixes the PATH; the manifest
    // (unvirtualizedResources + FileSystemWriteVirtualization) disables the WRITE
    // redirection for that path. The flag is a harmless no-op for unpackaged (Debug)
    // builds — they're never redirected.
    std::filesystem::path LocalAppDataDir()
    {
        PWSTR p = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData,
                                           KF_FLAG_NO_PACKAGE_REDIRECTION | KF_FLAG_CREATE,
                                           nullptr, &p);
        if (FAILED(hr) || !p)
        {
            // Last-ditch fallback — TEMP. Better than crashing.
            wchar_t tmp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tmp);
            return std::filesystem::path{ tmp };
        }
        std::filesystem::path dir{ p };
        CoTaskMemFree(p);
        dir /= L"hyprv";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // ISO-8601 round-trip for time_point. Windows::Data::Json doesn't have a
    // built-in DateTime type, so we serialize as int64 ticks-since-epoch
    // (microseconds — enough resolution, fits in JsonNumber's double range
    // for the next 285k years).
    int64_t TimePointToTicks(std::chrono::system_clock::time_point tp)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            tp.time_since_epoch()).count();
    }
    std::chrono::system_clock::time_point TicksToTimePoint(int64_t t)
    {
        return std::chrono::system_clock::time_point{
            std::chrono::microseconds{ t } };
    }
}

namespace hyprv::app::settings
{
    using namespace winrt::Windows::Data::Json;

    Settings& Settings::Instance()
    {
        // C++11 guarantees thread-safe static-local initialization. The ctor
        // does the Load + spawns the save thread, so first caller pays that
        // cost on the calling thread; everyone else just gets the reference.
        static Settings s;
        return s;
    }

    Settings::Settings()
        : m_path(LocalAppDataDir() / L"settings.json")
    {
        Load();
        m_saveThread = std::thread([this] { SaveLoop(); });
    }

    Settings::~Settings()
    {
        // Signal shutdown and flush any pending changes.
        {
            std::lock_guard<std::mutex> lk(m_saveLock);
            m_shutdown.store(true);
        }
        m_saveCv.notify_all();
        if (m_saveThread.joinable()) m_saveThread.join();
        if (m_dirty.exchange(false))
        {
            std::lock_guard<std::mutex> lk(m_lock);
            try { SaveLocked(); } catch (...) {}
        }
    }

    std::filesystem::path Settings::FilePath() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_path;
    }

    bool Settings::LoggingEnabled() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_diagnostics.loggingEnabled;
    }

    void Settings::SetLoggingEnabled(bool on)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            if (m_diagnostics.loggingEnabled == on) return;   // no change
            m_diagnostics.loggingEnabled = on;
        }
        MarkDirty();
    }

    bool Settings::KeepHomeTabOpen() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_general.keepHomeTabOpen;
    }

    void Settings::SetKeepHomeTabOpen(bool on)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            if (m_general.keepHomeTabOpen == on) return;   // no change
            m_general.keepHomeTabOpen = on;
        }
        MarkDirty();
    }

    // Defined further down; forward-declared so the per-VM debugger accessors
    // below can normalize their guid keys like the other vmPrefs accessors do.
    static std::wstring NormalizeGuid(std::wstring const& g);

    bool Settings::DebuggerEnabled() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_debugger.enabled;
    }

    void Settings::SetDebuggerEnabled(bool on)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            if (m_debugger.enabled == on) return;
            m_debugger.enabled = on;
        }
        MarkDirty();
    }

    std::wstring Settings::DebuggerExe() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_debugger.exe;
    }

    void Settings::SetDebuggerExe(std::wstring const& exe)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            if (m_debugger.exe == exe) return;
            m_debugger.exe = exe;
        }
        MarkDirty();
    }

    std::wstring Settings::VmDebuggerExe(std::wstring const& guid) const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        auto it = m_vmPrefs.find(NormalizeGuid(guid));
        if (it == m_vmPrefs.end() || !it->second.debuggerExe.has_value()) return {};
        return *it->second.debuggerExe;
    }

    std::wstring Settings::VmDebuggerArgs(std::wstring const& guid) const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        auto it = m_vmPrefs.find(NormalizeGuid(guid));
        if (it == m_vmPrefs.end() || !it->second.debuggerArgs.has_value()) return {};
        return *it->second.debuggerArgs;
    }

    std::wstring Settings::EffectiveDebuggerExe(std::wstring const& guid) const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        auto it = m_vmPrefs.find(NormalizeGuid(guid));
        if (it != m_vmPrefs.end() && it->second.debuggerExe.has_value()
            && !it->second.debuggerExe->empty())
            return *it->second.debuggerExe;
        return m_debugger.exe;
    }

    void Settings::SetVmDebugger(std::wstring const& guid,
                                 std::wstring const& exeOverride,
                                 std::wstring const& args)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            // Empty exe override = "use the global exe" (stored as nullopt);
            // empty args = "none" (nullopt) so the slot can be dropped on save.
            std::optional<std::wstring> newExe =
                exeOverride.empty() ? std::nullopt
                                    : std::optional<std::wstring>{ exeOverride };
            std::optional<std::wstring> newArgs =
                args.empty() ? std::nullopt
                             : std::optional<std::wstring>{ args };
            auto& slot = m_vmPrefs[NormalizeGuid(guid)];
            if (slot.debuggerExe == newExe && slot.debuggerArgs == newArgs) return;
            slot.debuggerExe  = std::move(newExe);
            slot.debuggerArgs = std::move(newArgs);
        }
        MarkDirty();
    }

    Window Settings::WindowGeometry() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_window;
    }

    void Settings::SetWindowGeometry(Window const& w)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            // Cheap equality check — splitter drags fire this every pixel
            // and we don't want to wake the save thread for a no-op.
            if (m_window.x             == w.x             &&
                m_window.y             == w.y             &&
                m_window.width         == w.width         &&
                m_window.height        == w.height        &&
                m_window.railWidth     == w.railWidth     &&
                m_window.flyoutWidth   == w.flyoutWidth   &&
                m_window.railVisible   == w.railVisible   &&
                m_window.flyoutVisible == w.flyoutVisible)
            {
                return;
            }
            m_window = w;
        }
        MarkDirty();
    }

    std::vector<Recent> Settings::Recents() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_recents;
    }

    void Settings::BumpRecent(std::wstring const& key, RecentKind kind)
    {
        if (key.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            // Remove any existing entry — we'll reinsert at the front.
            m_recents.erase(
                std::remove_if(m_recents.begin(), m_recents.end(),
                    [&](Recent const& r) {
                        return _wcsicmp(r.guid.c_str(), key.c_str()) == 0;
                    }),
                m_recents.end());
            // Push new front entry.
            m_recents.insert(m_recents.begin(),
                Recent{ key, kind, std::chrono::system_clock::now() });
            // Trim to cap.
            if (m_recents.size() > kMaxRecents)
                m_recents.resize(kMaxRecents);
        }
        MarkDirty();
    }

    void Settings::ForgetRecent(std::wstring const& guid)
    {
        if (guid.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            auto it = std::remove_if(m_recents.begin(), m_recents.end(),
                [&](Recent const& r) {
                    return _wcsicmp(r.guid.c_str(), guid.c_str()) == 0;
                });
            if (it == m_recents.end()) return;   // nothing to drop
            m_recents.erase(it, m_recents.end());
        }
        MarkDirty();
    }

    std::vector<OpenTab> Settings::OpenTabs() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_openTabs;
    }

    int Settings::SelectedTabIndex() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_selectedTabIndex;
    }

    void Settings::SetOpenTabs(std::vector<OpenTab> const& tabs, int selectedIndex)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            // Cheap equality check to dodge no-op writes (tab restore on
            // launch re-asserts the same list; selection changes during
            // session also re-fire here every tick).
            bool same = (m_openTabs.size() == tabs.size())
                     && (m_selectedTabIndex == selectedIndex);
            if (same)
            {
                for (size_t i = 0; i < tabs.size(); ++i)
                {
                    if (m_openTabs[i].type != tabs[i].type ||
                        m_openTabs[i].identifier != tabs[i].identifier)
                    {
                        same = false; break;
                    }
                }
            }
            if (same) return;
            m_openTabs = tabs;
            m_selectedTabIndex = selectedIndex;
        }
        MarkDirty();
    }

    std::vector<RemoteHost> Settings::RemoteHosts() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_remoteHosts;
    }

    std::optional<RemoteHost> Settings::FindRemoteHost(std::wstring const& address) const
    {
        if (address.empty()) return std::nullopt;
        std::lock_guard<std::mutex> lk(m_lock);
        for (auto const& h : m_remoteHosts)
            if (_wcsicmp(h.address.c_str(), address.c_str()) == 0)
                return h;
        return std::nullopt;
    }

    void Settings::AddOrUpdateRemoteHost(std::wstring const& originalAddress,
                                         RemoteHost const& host)
    {
        if (host.address.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            // Resolve the slot to replace: the original entry if editing
            // (originalAddress set), otherwise dedup against the new address.
            std::wstring const& key =
                originalAddress.empty() ? host.address : originalAddress;
            auto it = std::find_if(m_remoteHosts.begin(), m_remoteHosts.end(),
                [&](RemoteHost const& h) {
                    return _wcsicmp(h.address.c_str(), key.c_str()) == 0;
                });
            if (it != m_remoteHosts.end())
                *it = host;
            else
                m_remoteHosts.push_back(host);
        }
        MarkDirty();
    }

    void Settings::RemoveRemoteHost(std::wstring const& address)
    {
        if (address.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            auto it = std::remove_if(m_remoteHosts.begin(), m_remoteHosts.end(),
                [&](RemoteHost const& h) {
                    return _wcsicmp(h.address.c_str(), address.c_str()) == 0;
                });
            if (it == m_remoteHosts.end()) return;
            m_remoteHosts.erase(it, m_remoteHosts.end());
        }
        MarkDirty();
    }

    void Settings::SaveNow()
    {
        std::lock_guard<std::mutex> lk(m_lock);
        try { SaveLocked(); m_dirty.store(false); }
        catch (...) { HyprvAppLog(L"[settings] SaveNow threw"); }
    }

    // ---- Appearance --------------------------------------------------------

    Appearance Settings::AppearancePref() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_appearance;
    }

    void Settings::SetAppearance(Appearance const& a)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            // All three fields participate in the equality check —
            // missing acrylicTintOpacity here was why slider-only changes
            // never persisted.
            if (m_appearance.backdrop           == a.backdrop &&
                m_appearance.theme              == a.theme    &&
                m_appearance.acrylicTintOpacity == a.acrylicTintOpacity &&
                m_appearance.micaTintOpacity    == a.micaTintOpacity) return;
            m_appearance = a;
        }
        MarkDirty();
    }

    // String ↔ enum helpers for JSON. Strings are stable user-facing tokens
    // — keeping them out of the enum value-int encoding means renaming the
    // enum constant later doesn't silently flip persisted values.
    static wchar_t const* BackdropToString(Appearance::Backdrop b)
    {
        switch (b)
        {
        case Appearance::Backdrop::Mica:    return L"mica";
        case Appearance::Backdrop::Acrylic: return L"acrylic";
        }
        return L"mica";
    }
    static Appearance::Backdrop BackdropFromString(std::wstring const& s)
    {
        if (s == L"acrylic") return Appearance::Backdrop::Acrylic;
        // Legacy "none" + any unknown value falls back to the default.
        return Appearance::Backdrop::Mica;
    }
    static wchar_t const* ThemeToString(Appearance::Theme t)
    {
        switch (t)
        {
        case Appearance::Theme::System: return L"system";
        case Appearance::Theme::Light:  return L"light";
        case Appearance::Theme::Dark:   return L"dark";
        case Appearance::Theme::Black:  return L"black";
        }
        return L"system";
    }
    static Appearance::Theme ThemeFromString(std::wstring const& s)
    {
        if (s == L"light") return Appearance::Theme::Light;
        if (s == L"dark")  return Appearance::Theme::Dark;
        if (s == L"black") return Appearance::Theme::Black;
        return Appearance::Theme::System;
    }

    // Stable string tokens for the AudioMode enum so renaming the enum
    // constant later doesn't silently change persisted values. Mirrors
    // the BackdropToString / ThemeToString pattern above.
    static wchar_t const* AudioModeToString(RdpOptions::AudioMode m)
    {
        switch (m)
        {
        case RdpOptions::AudioMode::Redirect:     return L"redirect";
        case RdpOptions::AudioMode::PlayOnServer: return L"playOnServer";
        case RdpOptions::AudioMode::None:         return L"none";
        }
        return L"redirect";
    }
    static RdpOptions::AudioMode AudioModeFromString(std::wstring const& s)
    {
        if (s == L"playOnServer") return RdpOptions::AudioMode::PlayOnServer;
        if (s == L"none")         return RdpOptions::AudioMode::None;
        return RdpOptions::AudioMode::Redirect;
    }

    // Parse an RdpOptions struct from a JsonObject. Missing keys fall
    // back to the struct's defaults (the caller hands us a default-
    // constructed instance and we overlay only the fields actually
    // present in JSON). Used for both `rdpDefaults` top-level and
    // `vmPrefs.<guid>.rdpOptions` sub-object reads.
    static void RdpOptionsFromJson(JsonObject const& obj, RdpOptions& out)
    {
        if (obj.HasKey(L"audioMode"))
            out.audioMode = AudioModeFromString(
                std::wstring{ obj.GetNamedString(L"audioMode", L"redirect") });
        if (obj.HasKey(L"redirectClipboard"))
            out.redirectClipboard =
                obj.GetNamedBoolean(L"redirectClipboard", out.redirectClipboard);
        if (obj.HasKey(L"redirectDrives"))
            out.redirectDrives =
                obj.GetNamedBoolean(L"redirectDrives", out.redirectDrives);
        if (obj.HasKey(L"redirectDevices"))
            out.redirectDevices =
                obj.GetNamedBoolean(L"redirectDevices", out.redirectDevices);
        if (obj.HasKey(L"redirectSmartCards"))
            out.redirectSmartCards =
                obj.GetNamedBoolean(L"redirectSmartCards", out.redirectSmartCards);
        if (obj.HasKey(L"redirectPorts"))
            out.redirectPorts =
                obj.GetNamedBoolean(L"redirectPorts", out.redirectPorts);
        if (obj.HasKey(L"audioCaptureRedirect"))
            out.audioCaptureRedirect =
                obj.GetNamedBoolean(L"audioCaptureRedirect", out.audioCaptureRedirect);
        if (obj.HasKey(L"initialDesktopWidth"))
        {
            double v = obj.GetNamedNumber(L"initialDesktopWidth",
                static_cast<double>(out.initialDesktopWidth));
            // Clamp to a sensible range so a hand-edited file can't
            // produce a 0-sized or absurdly large initial popup. Upper
            // bound matches what most modern monitors can fit.
            if (v < 320) v = 320;
            if (v > 7680) v = 7680;
            out.initialDesktopWidth = static_cast<uint16_t>(v);
        }
        if (obj.HasKey(L"initialDesktopHeight"))
        {
            double v = obj.GetNamedNumber(L"initialDesktopHeight",
                static_cast<double>(out.initialDesktopHeight));
            if (v < 240) v = 240;
            if (v > 4320) v = 4320;
            out.initialDesktopHeight = static_cast<uint16_t>(v);
        }
        if (obj.HasKey(L"colorDepth"))
        {
            double v = obj.GetNamedNumber(L"colorDepth",
                static_cast<double>(out.colorDepth));
            // 16 / 24 / 32 are the values mstscax actually negotiates;
            // snap to one of them so a hand-edit can't push us out of
            // band.
            uint16_t iv = static_cast<uint16_t>(v);
            if (iv != 16 && iv != 24 && iv != 32) iv = 32;
            out.colorDepth = iv;
        }
        if (obj.HasKey(L"dpiScaleOverridePercent"))
        {
            double v = obj.GetNamedNumber(L"dpiScaleOverridePercent",
                static_cast<double>(out.dpiScaleOverridePercent));
            // 0 = Auto; otherwise snap to a valid RDP scale factor so a
            // hand-edit can't push mstscax out of band.
            uint16_t iv = static_cast<uint16_t>(v);
            if (iv != 0 && iv != 100 && iv != 125 && iv != 150 &&
                iv != 175 && iv != 200)
                iv = 0;
            out.dpiScaleOverridePercent = iv;
        }
    }

    // Per-action defaults — single source of truth. The defaults policy is
    // documented in PLAN.md ("App Settings dialog v1 → Confirmations"):
    // confirm by default for irreversible / data-losing actions; don't
    // confirm reversible state changes. Unknown keys fall through to true
    // (safer fallback — an extra dialog is less surprising than silently
    // bypassing one).
    static bool DefaultConfirmationEnabled(std::wstring const& key)
    {
        // ON by default — destructive / data-losing
        if (key == L"reset")                    return true;
        if (key == L"turnOff")                  return true;
        if (key == L"restart")                  return true;
        if (key == L"shutdown")                 return true;
        if (key == L"deleteVm")                 return true;
        if (key == L"deleteSnapshot")           return true;
        if (key == L"deleteSnapshotSubtree")    return true;
        if (key == L"applySnapshot")            return true;
        if (key == L"revertToLastSnapshot")     return true;
        // OFF by default — reversible / no work lost
        if (key == L"save")                     return false;
        if (key == L"pause")                    return false;
        if (key == L"startResume")              return false;
        if (key == L"takeSnapshot")             return false;
        if (key == L"toggleEnhancedSession")    return false;
        // Unknown key — fail safe.
        return true;
    }

    bool Settings::ConfirmationEnabled(std::wstring const& key) const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (auto it = m_confirmOverrides.find(key); it != m_confirmOverrides.end())
            return it->second;
        return DefaultConfirmationEnabled(key);
    }

    void Settings::SetConfirmationEnabled(std::wstring const& key, bool on)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            auto it = m_confirmOverrides.find(key);
            if (it != m_confirmOverrides.end() && it->second == on) return;
            m_confirmOverrides[key] = on;
        }
        MarkDirty();
    }

    void Settings::ClearConfirmationOverride(std::wstring const& key)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            if (m_confirmOverrides.erase(key) == 0) return;
        }
        MarkDirty();
    }

    // ---- Per-VM preferences ---------------------------------------------------
    //
    // VM GUIDs from Hyper-V are formatted lowercase in our internal use, but
    // user-edited JSON or future remote-host sync could feed us either case.
    // Normalize to lowercase on both read and write so settings.json round-
    // trips cleanly and a stray uppercase character can't fork the map.
    static std::wstring NormalizeGuid(std::wstring const& g)
    {
        std::wstring out = g;
        for (auto& c : out) c = static_cast<wchar_t>(towlower(c));
        return out;
    }

    bool Settings::EnhancedSessionPref(std::wstring const& vmGuid) const
    {
        if (vmGuid.empty()) return true;
        std::lock_guard<std::mutex> lk(m_lock);
        auto it = m_vmPrefs.find(NormalizeGuid(vmGuid));
        if (it == m_vmPrefs.end()) return true;
        return it->second.enhancedSession.value_or(true);
    }

    void Settings::SetEnhancedSessionPref(std::wstring const& vmGuid, bool on)
    {
        if (vmGuid.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            auto key = NormalizeGuid(vmGuid);
            auto& slot = m_vmPrefs[key];
            if (slot.enhancedSession.has_value() && slot.enhancedSession.value() == on)
                return;
            slot.enhancedSession = on;
        }
        MarkDirty();
    }

    bool Settings::EnhancedSessionEverSupported(std::wstring const& vmGuid) const
    {
        if (vmGuid.empty()) return true;
        std::lock_guard<std::mutex> lk(m_lock);
        auto it = m_vmPrefs.find(NormalizeGuid(vmGuid));
        if (it == m_vmPrefs.end()) return true;
        return it->second.enhancedSessionSupported.value_or(true);
    }

    void Settings::ObserveEnhancedSupport(std::wstring const& vmGuid)
    {
        if (vmGuid.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            auto key = NormalizeGuid(vmGuid);
            auto& slot = m_vmPrefs[key];
            if (slot.enhancedSessionSupported.has_value() &&
                slot.enhancedSessionSupported.value())
                return;   // already recorded as supported — no write needed
            slot.enhancedSessionSupported = true;
        }
        MarkDirty();
    }

    void Settings::ClearVmPrefs(std::wstring const& vmGuid)
    {
        if (vmGuid.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            if (m_vmPrefs.erase(NormalizeGuid(vmGuid)) == 0) return;
        }
        MarkDirty();
    }

    // ---- RDP options -------------------------------------------------------
    //
    // Field-by-field equality helper. We persist a per-VM override as the
    // full snapshot rather than a sparse diff, and the setters short-
    // circuit no-op writes via this comparator so debouncer doesn't wake
    // for setting-the-same-thing.
    static bool RdpOptionsEq(RdpOptions const& a, RdpOptions const& b)
    {
        return a.audioMode             == b.audioMode
            && a.redirectClipboard     == b.redirectClipboard
            && a.redirectDrives        == b.redirectDrives
            && a.redirectDevices       == b.redirectDevices
            && a.redirectSmartCards    == b.redirectSmartCards
            && a.redirectPorts         == b.redirectPorts
            && a.audioCaptureRedirect  == b.audioCaptureRedirect
            && a.initialDesktopWidth   == b.initialDesktopWidth
            && a.initialDesktopHeight  == b.initialDesktopHeight
            && a.colorDepth            == b.colorDepth
            && a.dpiScaleOverridePercent == b.dpiScaleOverridePercent;
    }

    RdpOptions Settings::RdpDefaults() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_rdpDefaults;
    }

    void Settings::SetRdpDefaults(RdpOptions const& opts)
    {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            if (RdpOptionsEq(m_rdpDefaults, opts)) return;
            m_rdpDefaults = opts;
        }
        MarkDirty();
    }

    bool Settings::HasRdpOptionsOverride(std::wstring const& vmGuid) const
    {
        if (vmGuid.empty()) return false;
        std::lock_guard<std::mutex> lk(m_lock);
        auto it = m_vmPrefs.find(NormalizeGuid(vmGuid));
        return it != m_vmPrefs.end() && it->second.rdpOptions.has_value();
    }

    void Settings::SetRdpOptionsOverride(std::wstring const& vmGuid,
                                         RdpOptions const& opts)
    {
        if (vmGuid.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            auto key = NormalizeGuid(vmGuid);
            auto& slot = m_vmPrefs[key];
            if (slot.rdpOptions.has_value() &&
                RdpOptionsEq(*slot.rdpOptions, opts))
                return;
            slot.rdpOptions = opts;
        }
        MarkDirty();
    }

    void Settings::ClearRdpOptionsOverride(std::wstring const& vmGuid)
    {
        if (vmGuid.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            auto it = m_vmPrefs.find(NormalizeGuid(vmGuid));
            if (it == m_vmPrefs.end() || !it->second.rdpOptions.has_value())
                return;
            it->second.rdpOptions.reset();
        }
        MarkDirty();
    }

    RdpOptions Settings::RdpOptionsFor(std::wstring const& vmGuid) const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (!vmGuid.empty())
        {
            auto it = m_vmPrefs.find(NormalizeGuid(vmGuid));
            if (it != m_vmPrefs.end() && it->second.rdpOptions.has_value())
                return *it->second.rdpOptions;
        }
        return m_rdpDefaults;
    }

    // ---- Internals ---------------------------------------------------------

    void Settings::MarkDirty()
    {
        m_dirty.store(true);
        m_saveCv.notify_one();
    }

    void Settings::SaveLoop()
    {
        // Two-phase wait: block indefinitely for the first dirty signal,
        // then sleep up to 500ms to let bursts coalesce, then write once.
        // Shutdown is checked at both wait points and forces a final flush.
        while (!m_shutdown.load())
        {
            std::unique_lock<std::mutex> lk(m_saveLock);
            m_saveCv.wait(lk, [this] {
                return m_shutdown.load() || m_dirty.load();
            });
            if (m_shutdown.load()) break;
            // Debounce window — bail early if a shutdown lands during the wait.
            m_saveCv.wait_for(lk, std::chrono::milliseconds(500),
                              [this] { return m_shutdown.load(); });
            if (m_shutdown.load()) break;
            m_dirty.store(false);
            lk.unlock();

            // SaveLocked needs the data lock, not the save lock.
            std::lock_guard<std::mutex> dataLk(m_lock);
            try { SaveLocked(); }
            catch (...) { HyprvAppLog(L"[settings] save failed (will retry on next dirty)"); }
        }
    }

    void Settings::Load()
    {
        std::ifstream f(m_path, std::ios::binary);
        if (!f.is_open()) return;   // first launch — defaults already set
        std::stringstream ss;
        ss << f.rdbuf();
        std::string utf8 = ss.str();
        if (utf8.empty()) return;

        // JsonObject takes a Platform String — convert from utf8.
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
            static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
            static_cast<int>(utf8.size()), w.data(), wlen);

        JsonObject root{ nullptr };
        if (!JsonObject::TryParse(winrt::hstring{ w }, root))
        {
            HyprvAppLog(L"[settings] %s: parse failed, keeping defaults",
                m_path.c_str());
            return;
        }

        // Version check — currently only one schema; future migration code
        // would inspect this and rewrite the in-memory shape before falling
        // through to the field reads below.
        // const int version = static_cast<int>(root.GetNamedNumber(L"version", 1));

        // Diagnostics.
        if (root.HasKey(L"diagnostics"))
        {
            auto diag = root.GetNamedObject(L"diagnostics");
            if (diag.HasKey(L"loggingEnabled"))
                m_diagnostics.loggingEnabled =
                    diag.GetNamedBoolean(L"loggingEnabled", m_diagnostics.loggingEnabled);
        }

        // General behaviour.
        if (root.HasKey(L"general"))
        {
            auto gen = root.GetNamedObject(L"general");
            if (gen.HasKey(L"keepHomeTabOpen"))
                m_general.keepHomeTabOpen =
                    gen.GetNamedBoolean(L"keepHomeTabOpen", m_general.keepHomeTabOpen);
        }

        // Debugger (global).
        if (root.HasKey(L"debugger"))
        {
            auto dbg = root.GetNamedObject(L"debugger");
            if (dbg.HasKey(L"enabled"))
                m_debugger.enabled = dbg.GetNamedBoolean(L"enabled", m_debugger.enabled);
            if (dbg.HasKey(L"exe"))
                m_debugger.exe = std::wstring{ dbg.GetNamedString(L"exe",
                    winrt::hstring{ m_debugger.exe }) };
        }

        // Window.
        if (root.HasKey(L"window"))
        {
            auto wnd = root.GetNamedObject(L"window");
            if (wnd.HasKey(L"x"))
                m_window.x = static_cast<int>(wnd.GetNamedNumber(L"x", 0));
            if (wnd.HasKey(L"y"))
                m_window.y = static_cast<int>(wnd.GetNamedNumber(L"y", 0));
            m_window.width       = static_cast<int>(wnd.GetNamedNumber(L"width", 0));
            m_window.height      = static_cast<int>(wnd.GetNamedNumber(L"height", 0));
            m_window.railWidth   = wnd.GetNamedNumber(L"railWidth",   m_window.railWidth);
            m_window.flyoutWidth = wnd.GetNamedNumber(L"flyoutWidth", m_window.flyoutWidth);
            // Visibility — additive fields; existing settings.json files
            // without these keys keep the struct defaults (both hidden).
            if (wnd.HasKey(L"railVisible"))
                m_window.railVisible = wnd.GetNamedBoolean(L"railVisible", false);
            if (wnd.HasKey(L"flyoutVisible"))
                m_window.flyoutVisible = wnd.GetNamedBoolean(L"flyoutVisible", false);
        }

        // Open tabs (additive — missing means "no restore, just launch as
        // if first-run").
        if (root.HasKey(L"openTabs"))
        {
            auto arr = root.GetNamedArray(L"openTabs");
            m_openTabs.clear();
            m_openTabs.reserve(arr.Size());
            for (uint32_t i = 0; i < arr.Size(); ++i)
            {
                auto tv = arr.GetObjectAt(i);
                OpenTab ot;
                ot.type       = std::wstring{ tv.GetNamedString(L"type", L"") };
                ot.identifier = std::wstring{ tv.GetNamedString(L"identifier", L"") };
                if (ot.type.empty()) continue;
                m_openTabs.push_back(std::move(ot));
            }
            if (root.HasKey(L"selectedTabIndex"))
                m_selectedTabIndex = static_cast<int>(
                    root.GetNamedNumber(L"selectedTabIndex", -1));
        }

        // Appearance.
        if (root.HasKey(L"appearance"))
        {
            auto obj = root.GetNamedObject(L"appearance");
            if (obj.HasKey(L"backdrop"))
                m_appearance.backdrop = BackdropFromString(
                    std::wstring{ obj.GetNamedString(L"backdrop", L"mica") });
            if (obj.HasKey(L"theme"))
                m_appearance.theme = ThemeFromString(
                    std::wstring{ obj.GetNamedString(L"theme", L"system") });
            if (obj.HasKey(L"acrylicTintOpacity"))
            {
                double v = obj.GetNamedNumber(L"acrylicTintOpacity", 0.5);
                // Clamp to the valid range so a hand-edited settings file
                // can't push the controller into undefined territory.
                if (v < 0.0) v = 0.0;
                if (v > 1.0) v = 1.0;
                m_appearance.acrylicTintOpacity = v;
            }
            if (obj.HasKey(L"micaTintOpacity"))
            {
                double v = obj.GetNamedNumber(L"micaTintOpacity", 0.5);
                if (v < 0.0) v = 0.0;
                if (v > 1.0) v = 1.0;
                m_appearance.micaTintOpacity = v;
            }
        }

        // RDP defaults — top-level object. Optional; absent means use the
        // struct defaults baked into RdpOptions's member initializers.
        if (root.HasKey(L"rdpDefaults"))
        {
            auto obj = root.GetNamedObject(L"rdpDefaults");
            RdpOptionsFromJson(obj, m_rdpDefaults);
        }

        // Confirmation overrides. Sparse object; only keys the user has
        // explicitly toggled are present. Anything missing reads its
        // baked-in default at lookup time.
        if (root.HasKey(L"confirmations"))
        {
            auto obj = root.GetNamedObject(L"confirmations");
            m_confirmOverrides.clear();
            // JsonObject::First/Size — no direct iteration in Windows.Data.Json;
            // use the Lookup<keys-via-First-walk> idiom.
            for (auto const& kv : obj)
            {
                auto value = kv.Value();
                if (value.ValueType() != JsonValueType::Boolean) continue;
                std::wstring key{ kv.Key() };
                m_confirmOverrides[std::move(key)] = value.GetBoolean();
            }
        }

        // Per-VM preferences. Sparse object keyed by VM GUID; each value is
        // a sub-object holding only the keys the user has explicitly toggled.
        // Unknown sub-keys (forward-compat) are tolerated and silently
        // dropped — they won't round-trip but they also won't break the
        // existing fields.
        if (root.HasKey(L"vmPrefs"))
        {
            auto obj = root.GetNamedObject(L"vmPrefs");
            m_vmPrefs.clear();
            for (auto const& kv : obj)
            {
                if (kv.Value().ValueType() != JsonValueType::Object) continue;
                auto sub = kv.Value().GetObject();
                VmPrefs p;
                if (sub.HasKey(L"enhancedSession"))
                {
                    auto v = sub.Lookup(L"enhancedSession");
                    if (v.ValueType() == JsonValueType::Boolean)
                        p.enhancedSession = v.GetBoolean();
                }
                if (sub.HasKey(L"enhancedSessionSupported"))
                {
                    auto v = sub.Lookup(L"enhancedSessionSupported");
                    if (v.ValueType() == JsonValueType::Boolean)
                        p.enhancedSessionSupported = v.GetBoolean();
                }
                if (sub.HasKey(L"rdpOptions"))
                {
                    auto v = sub.Lookup(L"rdpOptions");
                    if (v.ValueType() == JsonValueType::Object)
                    {
                        // Seed from current app defaults so any keys
                        // missing inside the per-VM rdpOptions object
                        // (e.g. from an earlier hyprv version that didn't
                        // know about a particular knob) fall back to the
                        // user's current default — NOT the struct's hard-
                        // coded default. Matches the user's mental model:
                        // "anything I didn't override comes from defaults."
                        RdpOptions o = m_rdpDefaults;
                        RdpOptionsFromJson(v.GetObject(), o);
                        p.rdpOptions = o;
                    }
                }
                if (sub.HasKey(L"debuggerExe"))
                {
                    auto v = sub.Lookup(L"debuggerExe");
                    if (v.ValueType() == JsonValueType::String)
                        p.debuggerExe = std::wstring{ v.GetString() };
                }
                if (sub.HasKey(L"debuggerArgs"))
                {
                    auto v = sub.Lookup(L"debuggerArgs");
                    if (v.ValueType() == JsonValueType::String)
                        p.debuggerArgs = std::wstring{ v.GetString() };
                }
                if (!p.enhancedSession.has_value() &&
                    !p.enhancedSessionSupported.has_value() &&
                    !p.rdpOptions.has_value() &&
                    !p.debuggerExe.has_value() &&
                    !p.debuggerArgs.has_value()) continue;
                std::wstring key = NormalizeGuid(std::wstring{ kv.Key() });
                m_vmPrefs[std::move(key)] = std::move(p);
            }
        }

        // Recents.
        if (root.HasKey(L"recents"))
        {
            auto arr = root.GetNamedArray(L"recents");
            m_recents.clear();
            m_recents.reserve(arr.Size());
            for (uint32_t i = 0; i < arr.Size(); ++i)
            {
                auto rv = arr.GetObjectAt(i);
                Recent r;
                r.guid = std::wstring{ rv.GetNamedString(L"guid", L"") };
                if (r.guid.empty()) continue;
                // "kind" absent => Vm (back-compat with the pre-Remote-Hosts schema).
                std::wstring kind{ rv.GetNamedString(L"kind", L"vm") };
                r.kind = (_wcsicmp(kind.c_str(), L"remote") == 0)
                       ? RecentKind::Remote : RecentKind::Vm;
                int64_t ticks = static_cast<int64_t>(
                    rv.GetNamedNumber(L"lastOpened", 0));
                r.lastOpened = TicksToTimePoint(ticks);
                m_recents.push_back(std::move(r));
                if (m_recents.size() >= kMaxRecents) break;
            }
        }

        // Remote hosts. rdpDefaults is loaded above, so a host's missing rdp
        // keys inherit the app-wide defaults.
        if (root.HasKey(L"remoteHosts"))
        {
            auto arr = root.GetNamedArray(L"remoteHosts");
            m_remoteHosts.clear();
            m_remoteHosts.reserve(arr.Size());
            for (uint32_t i = 0; i < arr.Size(); ++i)
            {
                auto rv = arr.GetObjectAt(i);
                RemoteHost h;
                h.address = std::wstring{ rv.GetNamedString(L"address", L"") };
                if (h.address.empty()) continue;   // address is the key
                h.name     = std::wstring{ rv.GetNamedString(L"name", L"") };
                h.username = std::wstring{ rv.GetNamedString(L"username", L"") };
                h.domain   = std::wstring{ rv.GetNamedString(L"domain", L"") };
                double pv  = rv.GetNamedNumber(L"port", 3389.0);
                h.port = (pv >= 1 && pv <= 65535)
                       ? static_cast<uint16_t>(pv) : uint16_t{ 3389 };
                h.rdp = m_rdpDefaults;   // seed, then overlay any saved keys
                if (rv.HasKey(L"rdp"))
                    RdpOptionsFromJson(rv.GetNamedObject(L"rdp"), h.rdp);
                m_remoteHosts.push_back(std::move(h));
            }
        }
    }

    void Settings::SaveLocked() const
    {
        // Hand-rolled pretty-printer instead of JsonObject::Stringify(). We
        // own the schema so we get to control the key order, the 2-space
        // indentation, and the line-per-field layout that makes the file
        // human-editable. Windows::Data::Json's Stringify is single-line.
        std::wstring out;
        auto esc = [](std::wstring const& s) -> std::wstring {
            std::wstring r; r.reserve(s.size() + 2);
            r += L'"';
            for (wchar_t c : s)
            {
                switch (c)
                {
                case L'"':  r += L"\\\""; break;
                case L'\\': r += L"\\\\"; break;
                case L'\n': r += L"\\n";  break;
                case L'\r': r += L"\\r";  break;
                case L'\t': r += L"\\t";  break;
                default:    r += c;       break;
                }
            }
            r += L'"';
            return r;
        };
        auto i64 = [](int64_t v) -> std::wstring { return std::to_wstring(v); };
        auto i32 = [](int v)     -> std::wstring { return std::to_wstring(v); };
        auto d   = [](double v)  -> std::wstring {
            // %g drops the trailing .000000 on integral doubles for cleaner output.
            wchar_t buf[64];
            swprintf_s(buf, L"%g", v);
            return buf;
        };

        out += L"{\n";
        out += L"  \"version\": 1,\n";

        // Diagnostics.
        out += L"  \"diagnostics\": {\n";
        out += L"    \"loggingEnabled\": ";
        out += m_diagnostics.loggingEnabled ? L"true" : L"false";
        out += L"\n  },\n";

        // General behaviour.
        out += L"  \"general\": {\n";
        out += L"    \"keepHomeTabOpen\": ";
        out += m_general.keepHomeTabOpen ? L"true" : L"false";
        out += L"\n  },\n";

        // Debugger (global).
        out += L"  \"debugger\": {\n";
        out += L"    \"enabled\": ";
        out += m_debugger.enabled ? L"true" : L"false";
        out += L",\n";
        out += L"    \"exe\": " + esc(m_debugger.exe) + L"\n";
        out += L"  },\n";

        // Window. x/y are omitted if INT_MIN (never persisted) so a hand-
        // edited file that just deletes the position keeps the OS-default
        // placement on next launch.
        out += L"  \"window\": {\n";
        if (m_window.x != INT_MIN && m_window.y != INT_MIN)
        {
            out += L"    \"x\": " + i32(m_window.x) + L",\n";
            out += L"    \"y\": " + i32(m_window.y) + L",\n";
        }
        out += L"    \"width\": "         + i32(m_window.width)  + L",\n";
        out += L"    \"height\": "        + i32(m_window.height) + L",\n";
        out += L"    \"railWidth\": "     + d(m_window.railWidth)   + L",\n";
        out += L"    \"flyoutWidth\": "   + d(m_window.flyoutWidth) + L",\n";
        out += L"    \"railVisible\": ";
        out += m_window.railVisible ? L"true" : L"false";
        out += L",\n";
        out += L"    \"flyoutVisible\": ";
        out += m_window.flyoutVisible ? L"true" : L"false";
        out += L"\n  },\n";

        // Open tabs — array of {type, identifier}. selectedTabIndex sits
        // alongside (not inside) the array because it's a single value.
        out += L"  \"openTabs\": [";
        if (m_openTabs.empty())
        {
            out += L"],\n";
        }
        else
        {
            out += L"\n";
            for (size_t k = 0; k < m_openTabs.size(); ++k)
            {
                out += L"    {\n";
                out += L"      \"type\": "       + esc(m_openTabs[k].type)       + L",\n";
                out += L"      \"identifier\": " + esc(m_openTabs[k].identifier) + L"\n";
                out += L"    }";
                if (k + 1 < m_openTabs.size()) out += L",";
                out += L"\n";
            }
            out += L"  ],\n";
        }
        out += L"  \"selectedTabIndex\": " + i32(m_selectedTabIndex) + L",\n";

        // Appearance — stable string tokens (not enum ints) so renames in
        // the enum don't silently shift persisted values.
        out += L"  \"appearance\": {\n";
        out += L"    \"backdrop\": "           + esc(BackdropToString(m_appearance.backdrop)) + L",\n";
        out += L"    \"theme\": "              + esc(ThemeToString(m_appearance.theme))       + L",\n";
        out += L"    \"acrylicTintOpacity\": " + d(m_appearance.acrylicTintOpacity)           + L",\n";
        out += L"    \"micaTintOpacity\": "    + d(m_appearance.micaTintOpacity)              + L"\n";
        out += L"  },\n";

        // Helper closure that writes an RdpOptions object body (everything
        // between { and }) prefixed with the given indent string. Used by
        // both the top-level rdpDefaults block AND the per-VM override
        // block inside vmPrefs, so the field list and order stay in lock-
        // step. Caller is responsible for the surrounding { } braces +
        // trailing comma.
        auto writeRdpOptsBody = [&](RdpOptions const& o, std::wstring const& indent) {
            out += indent + L"\"audioMode\": "
                + esc(AudioModeToString(o.audioMode)) + L",\n";
            out += indent + L"\"redirectClipboard\": "
                + (o.redirectClipboard ? std::wstring{L"true"} : std::wstring{L"false"}) + L",\n";
            out += indent + L"\"redirectDrives\": "
                + (o.redirectDrives ? std::wstring{L"true"} : std::wstring{L"false"}) + L",\n";
            out += indent + L"\"redirectDevices\": "
                + (o.redirectDevices ? std::wstring{L"true"} : std::wstring{L"false"}) + L",\n";
            out += indent + L"\"redirectSmartCards\": "
                + (o.redirectSmartCards ? std::wstring{L"true"} : std::wstring{L"false"}) + L",\n";
            out += indent + L"\"redirectPorts\": "
                + (o.redirectPorts ? std::wstring{L"true"} : std::wstring{L"false"}) + L",\n";
            out += indent + L"\"audioCaptureRedirect\": "
                + (o.audioCaptureRedirect ? std::wstring{L"true"} : std::wstring{L"false"}) + L",\n";
            out += indent + L"\"initialDesktopWidth\": "
                + std::to_wstring(o.initialDesktopWidth) + L",\n";
            out += indent + L"\"initialDesktopHeight\": "
                + std::to_wstring(o.initialDesktopHeight) + L",\n";
            out += indent + L"\"colorDepth\": "
                + std::to_wstring(o.colorDepth) + L",\n";
            out += indent + L"\"dpiScaleOverridePercent\": "
                + std::to_wstring(o.dpiScaleOverridePercent) + L"\n";
        };

        // App-wide RDP defaults. Always emitted (even when fields equal
        // the struct defaults) so a hand-edit has a discoverable place
        // to tweak each knob — symmetric with `appearance`.
        out += L"  \"rdpDefaults\": {\n";
        writeRdpOptsBody(m_rdpDefaults, L"    ");
        out += L"  },\n";

        // Confirmation overrides — sparse object. Sort keys for stable diffs
        // and human-readable settings.json (unordered_map's iteration order
        // is otherwise arbitrary and shifts on rehash).
        out += L"  \"confirmations\": {";
        if (m_confirmOverrides.empty())
        {
            out += L"},\n";
        }
        else
        {
            std::vector<std::wstring> keys;
            keys.reserve(m_confirmOverrides.size());
            for (auto const& kv : m_confirmOverrides) keys.push_back(kv.first);
            std::sort(keys.begin(), keys.end());
            out += L"\n";
            for (size_t k = 0; k < keys.size(); ++k)
            {
                out += L"    " + esc(keys[k]) + L": ";
                out += m_confirmOverrides.at(keys[k]) ? L"true" : L"false";
                if (k + 1 < keys.size()) out += L",";
                out += L"\n";
            }
            out += L"  },\n";
        }

        // Per-VM preferences — sparse object of objects. Sort outer keys for
        // stable diffs; each sub-object only includes the fields the user
        // has explicitly set.
        out += L"  \"vmPrefs\": {";
        if (m_vmPrefs.empty())
        {
            out += L"},\n";
        }
        else
        {
            std::vector<std::wstring> keys;
            keys.reserve(m_vmPrefs.size());
            for (auto const& kv : m_vmPrefs) keys.push_back(kv.first);
            std::sort(keys.begin(), keys.end());
            out += L"\n";
            for (size_t k = 0; k < keys.size(); ++k)
            {
                auto const& p = m_vmPrefs.at(keys[k]);
                // Two formats: compact single-line when only scalar
                // flags are set, multi-line when an rdpOptions override OR a
                // debugger config (esp. a long args string) is present
                // (compact would produce an awkwardly long line). Behaviour-
                // equivalent on the JSON side either way.
                if (p.rdpOptions.has_value() || p.debuggerExe.has_value()
                    || p.debuggerArgs.has_value())
                {
                    out += L"    " + esc(keys[k]) + L": {\n";
                    bool wroteAny = false;
                    auto comma = [&] {
                        if (wroteAny) out += L",\n";
                        wroteAny = true;
                    };
                    if (p.enhancedSession.has_value())
                    {
                        comma();
                        out += L"      \"enhancedSession\": ";
                        out += (*p.enhancedSession) ? L"true" : L"false";
                    }
                    if (p.enhancedSessionSupported.has_value())
                    {
                        comma();
                        out += L"      \"enhancedSessionSupported\": ";
                        out += (*p.enhancedSessionSupported) ? L"true" : L"false";
                    }
                    if (p.debuggerExe.has_value())
                    {
                        comma();
                        out += L"      \"debuggerExe\": " + esc(*p.debuggerExe);
                    }
                    if (p.debuggerArgs.has_value())
                    {
                        comma();
                        out += L"      \"debuggerArgs\": " + esc(*p.debuggerArgs);
                    }
                    if (p.rdpOptions.has_value())
                    {
                        comma();
                        out += L"      \"rdpOptions\": {\n";
                        writeRdpOptsBody(*p.rdpOptions, L"        ");
                        out += L"      }";
                    }
                    out += L"\n    }";
                }
                else
                {
                    out += L"    " + esc(keys[k]) + L": { ";
                    bool first = true;
                    if (p.enhancedSession.has_value())
                    {
                        if (!first) out += L", ";
                        out += L"\"enhancedSession\": ";
                        out += (*p.enhancedSession) ? L"true" : L"false";
                        first = false;
                    }
                    if (p.enhancedSessionSupported.has_value())
                    {
                        if (!first) out += L", ";
                        out += L"\"enhancedSessionSupported\": ";
                        out += (*p.enhancedSessionSupported) ? L"true" : L"false";
                        first = false;
                    }
                    out += L" }";
                }
                if (k + 1 < keys.size()) out += L",";
                out += L"\n";
            }
            out += L"  },\n";
        }

        // Remote hosts — array of saved RDP machines. No password is ever
        // stored (the "prompt every time" model); only the connection identity
        // + per-host RDP options (nested rdp object, same shape as rdpDefaults).
        out += L"  \"remoteHosts\": [";
        if (m_remoteHosts.empty())
        {
            out += L"],\n";
        }
        else
        {
            out += L"\n";
            for (size_t k = 0; k < m_remoteHosts.size(); ++k)
            {
                auto const& rh = m_remoteHosts[k];
                out += L"    {\n";
                out += L"      \"name\": "     + esc(rh.name)     + L",\n";
                out += L"      \"address\": "  + esc(rh.address)  + L",\n";
                out += L"      \"username\": " + esc(rh.username) + L",\n";
                out += L"      \"domain\": "   + esc(rh.domain)   + L",\n";
                out += L"      \"port\": "     + std::to_wstring(rh.port) + L",\n";
                out += L"      \"rdp\": {\n";
                writeRdpOptsBody(rh.rdp, L"        ");
                out += L"      }\n";
                out += L"    }";
                if (k + 1 < m_remoteHosts.size()) out += L",";
                out += L"\n";
            }
            out += L"  ],\n";
        }

        // Recents — array of objects, one per line if empty, indented if not.
        out += L"  \"recents\": [";
        if (m_recents.empty())
        {
            out += L"]\n";
        }
        else
        {
            out += L"\n";
            for (size_t k = 0; k < m_recents.size(); ++k)
            {
                out += L"    {\n";
                out += L"      \"guid\": "       + esc(m_recents[k].guid) + L",\n";
                out += L"      \"kind\": "       +
                       esc(m_recents[k].kind == RecentKind::Remote
                               ? std::wstring{ L"remote" } : std::wstring{ L"vm" }) + L",\n";
                out += L"      \"lastOpened\": " +
                       i64(TimePointToTicks(m_recents[k].lastOpened)) + L"\n";
                out += L"    }";
                if (k + 1 < m_recents.size()) out += L",";
                out += L"\n";
            }
            out += L"  ]\n";
        }
        out += L"}\n";

        // UTF-8 encode + atomic write via temp file + rename.
        int nb = WideCharToMultiByte(CP_UTF8, 0,
            out.c_str(), static_cast<int>(out.size()),
            nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(nb), '\0');
        WideCharToMultiByte(CP_UTF8, 0,
            out.c_str(), static_cast<int>(out.size()),
            utf8.data(), nb, nullptr, nullptr);

        auto tmp = m_path;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return;
            f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        }
        // ReplaceFileW is safer than rename on Windows — handles destination
        // existing + atomically swaps in one syscall. std::filesystem::rename
        // throws on Windows if the dest file exists.
        if (!ReplaceFileW(m_path.c_str(), tmp.c_str(), nullptr,
                          REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
        {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND)
            {
                // First-run case — destination doesn't exist yet.
                MoveFileExW(tmp.c_str(), m_path.c_str(),
                            MOVEFILE_REPLACE_EXISTING);
            }
            else
            {
                HyprvAppLog(L"[settings] ReplaceFile err=%lu", err);
            }
        }
    }
}
