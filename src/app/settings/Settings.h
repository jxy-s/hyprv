// Settings — process-wide singleton holding persisted user preferences.
//
// Backing store: %LOCALAPPDATA%\hyprv\settings.json. The location works for
// both the unpackaged dev build (writes directly to AppData\Local\hyprv) and
// a future MSIX-packaged install (the OS transparently redirects writes to
// %LOCALAPPDATA% into the package's LocalState folder). No hidden coupling
// to the install directory — MSIX prohibits writes to its install root, so
// we never go there.
//
// Threading: every getter/setter is mutex-protected. Setters mark the cache
// dirty and notify a background save thread that debounces writes (~500ms)
// so a burst of changes (splitter drag, rapid focus shuffles) results in
// one disk write, not dozens. Final flush happens in the dtor / SaveNow().
//
// Schema versioning: top-level "version" field starts at 1. Future
// migrations switch on this when loading.

#pragma once

#include <atomic>
#include <chrono>
#include <climits>          // INT_MIN sentinel for "position never persisted"
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hyprv::app::settings
{
    // Diagnostics — controls the file logger that HyprvAppLog writes to.
    // Default is build-config-dependent (on in Debug, off in Release) so
    // developers get logs out of the box and end users don't pay disk for
    // them. Toggle at runtime by editing settings.json or via a future UI.
    struct Diagnostics
    {
        bool loggingEnabled =
#ifdef _DEBUG
            true
#else
            false
#endif
            ;
    };

    // General app behaviour that isn't a direct mirror of a UI state.
    struct General
    {
        // When true, opening a VM from a welcome/home tab leaves the home
        // tab open (just opens + selects the VM tab) instead of the default
        // replace-on-open gesture that closes the home tab. Default false =
        // current browser-new-tab-page behaviour.
        bool keepHomeTabOpen = false;
    };

    // VM debugger launch — a niche feature (gated by `enabled`, off by
    // default) that spawns a user-configured debugger command for a VM. `exe`
    // is the global debugger (default the modern WinDbg, "windbgx"); the
    // per-VM exe override + the actual transport-bearing arguments live in
    // VmPrefs. hyprv is transport-agnostic — the user puts whatever they want
    // (`-k net:...`, `-k com:...`, etc.) in the per-VM arguments.
    struct Debugger
    {
        bool         enabled = false;
        std::wstring exe = L"windbgx";
    };

    // Persisted main-window geometry. Restored on launch so the user's
    // last layout sticks. railWidth / flyoutWidth are the in-window
    // splitter positions (DIPs); width / height are the window's outer
    // dimensions (pixels). Zero values mean "no override — use defaults".
    //
    // Visibility (railVisible / flyoutVisible) is tracked separately from
    // width so toggling a panel off → on restores the user's last width.
    // Both default to false on a fresh install — the welcome tab is the
    // primary surface, rail and flyout are secondary navigation.
    struct Window
    {
        // Outer window rect in screen pixels. width/height = 0 means
        // "no persisted value — keep system default placement". x/y default
        // to INT_MIN so we can distinguish "never persisted" from "persisted
        // at origin", and so a torn-off monitor can fall back to default
        // placement instead of restoring to coordinates that no longer exist.
        int    x             = INT_MIN;
        int    y             = INT_MIN;
        int    width         = 0;
        int    height        = 0;
        double railWidth     = 190;   // last user-set width (only used while visible)
        double flyoutWidth   = 400;
        bool   railVisible   = false; // hidden by default; user opens via hamburger
        bool   flyoutVisible = false; // hidden by default; user opens via info button
    };

    // One entry in the persisted open-tabs list. type:
    //   "welcome" = a host welcome tab. Today only the local host is
    //               supported (identifier = "local"); remote hosts (Tier 3)
    //               will use the host's display name or address.
    //   "vm"      = a VM session tab. identifier = the VM's GUID
    //               (Msvm_ComputerSystem.Name). Restored only if VMManager
    //               still reports a VM with that GUID — silent skip on
    //               missing.
    struct OpenTab
    {
        std::wstring type;        // "welcome" | "vm"
        std::wstring identifier;  // "local" for welcome, VM GUID for vm
    };

    // What a Recent entry points at — a local VM or a saved Remote Host.
    // Persisted as a "kind" string ("vm"/"remote"); absent => Vm (back-compat
    // with the pre-Remote-Hosts schema).
    enum class RecentKind : uint8_t { Vm = 0, Remote = 1 };

    // One entry in the recently-opened list. For Vm the key is the VM GUID
    // (survives a rename); for Remote it's the host address. lastOpened drives
    // MRU ordering. The welcome screen renders this list (resolving each key
    // against VMManager or the saved-hosts list per kind; unresolvable entries
    // — deleted VM / forgotten host — are skipped).
    struct Recent
    {
        std::wstring guid;        // VM GUID (Vm) or host address (Remote) — the key
        RecentKind   kind = RecentKind::Vm;
        std::chrono::system_clock::time_point lastOpened{};
    };

    // Application-wide visual chrome.
    //
    // backdrop: the window's SystemBackdrop. Mica = subtle wallpaper tint
    // (Win11 default for Windows apps); Acrylic = thicker blur. A prior
    // "None" option was removed — clearing SystemBackdrop left the root
    // surface to show whatever brush WinUI picked, which didn't theme
    // cleanly. Stick with one of the two real backdrops.
    //
    // theme: how XAML resolves Light vs Dark resource brushes.
    //   System = follow OS theme (default)
    //   Light  = force light
    //   Dark   = force WinUI's stock dark (RGB ~32,32,32 base)
    //   Black  = force dark + override page-background brushes to near-
    //            black (RGB ~10,10,10). OLED-friendly. The brush override
    //            applies on the next launch — RequestedTheme cascades live
    //            but Application.Resources brush overrides don't repaint
    //            existing visuals.
    struct Appearance
    {
        enum class Backdrop : uint8_t { Mica = 0, Acrylic = 1 };
        enum class Theme    : uint8_t { System = 0, Light = 1, Dark = 2, Black = 3 };
        Backdrop backdrop = Backdrop::Mica;
        Theme    theme    = Theme::System;
        // TintOpacity per backdrop. 0.0 = mostly wallpaper, 1.0 = mostly
        // tint color. Each is honored only when its matching backdrop is
        // active; the App Settings page surfaces one slider at a time.
        // Plumbed into DesktopAcrylicController / MicaController in
        // MainWindow::ApplyAppearance (and UpdateBackdropTintOpacity for
        // the live-drag fast path).
        double acrylicTintOpacity = 0.5;
        double micaTintOpacity    = 0.5;
    };

    // RDP session options surfaced to the user. Persisted in two scopes:
    //
    //   1. App-wide defaults (Settings::RdpDefaults / SetRdpDefaults).
    //      Applied to any VM that doesn't have its own override.
    //   2. Per-VM overrides (Settings::SetRdpOptionsOverride). When set,
    //      these win over the defaults for that VM. Sparse — only VMs
    //      the user has explicitly customized have an entry.
    //
    // `VmTabPage::StartConnection` calls `RdpOptionsFor(guid)` which
    // returns the effective struct (override if set, else defaults).
    // Values are then mapped into the wire-level `hyprv::ipc::RdpOptions`
    // — same numeric values, but the enum types differ so Settings
    // doesn't depend on the IPC header.
    //
    // The IPC layer already carries all the redirection bits we expose
    // (RdpFlags in src/shared/RdpIpc.h); this struct picks the subset
    // that's worth surfacing in the UI. Add more knobs by extending
    // this struct + the JSON shape + the UI surfaces, in that order.
    struct RdpOptions
    {
        // Mirrors hyprv::ipc::AudioMode — kept as a separate type so
        // Settings doesn't transitively depend on the IPC wire header.
        // Redirect = play on client (the common case for a desktop user
        // who wants their VM's audio out of their speakers); PlayOnServer
        // = play on the VM's virtual sound device (rare — handy if the
        // guest is a media server you want hearing itself); None = mute.
        enum class AudioMode : uint8_t
        {
            Redirect     = 0,
            PlayOnServer = 1,
            None         = 2,
        };
        AudioMode audioMode = AudioMode::Redirect;

        // Redirection flags. Each maps to one bit in hyprv::ipc::RdpFlags.
        // Clipboard is on by default because it's the single most useful
        // thing a desktop user expects from RDP; the rest default off
        // because they have security / hardware implications and aren't
        // universally available on the guest.
        bool redirectClipboard    = true;
        bool redirectDrives       = false;
        bool redirectDevices      = false;
        bool redirectSmartCards   = false;
        bool redirectPorts        = false;
        bool audioCaptureRedirect = false;

        // Display knobs that mostly matter in basic-session mode —
        // enhanced session ignores the initial WxH and renegotiates with
        // mstscax via UpdateSessionDisplaySettings once login completes.
        // Kept here anyway so users with basic-only guests can get a
        // sensible starting resolution. 16/24/32 bpp; 32 is the modern
        // default (mstscax falls back gracefully if the guest can't).
        uint16_t initialDesktopWidth  = 1024;
        uint16_t initialDesktopHeight = 768;
        uint16_t colorDepth           = 32;
        // Display scale override (as a percent: 100/125/150/175/200). 0 means
        // "Auto" — follow the host window's DPI (the historical behavior). A
        // non-zero value pins the guest's render scale independent of the
        // host monitor, applied at connect (DesktopScaleFactor) AND on the
        // enhanced-session fit-to-window resize (via the physical-size DPI
        // the parent passes to UpdateSessionDisplaySettings).
        uint16_t dpiScaleOverridePercent = 0;
    };

    // A saved "Remote Host" — a physical machine the user RDPs into directly
    // (generic RDP, NOT Hyper-V management). `address` (hostname or IP) is the
    // stable key, used as the OpenTab identifier + rail/welcome lookup. NO
    // password is ever stored: the "prompt every time" model means mstscax
    // shows its own credential prompt on connect (with `username` pre-filled);
    // Windows' own "remember me" still works. `rdp` carries per-host RDP
    // options (seeded from RdpDefaults() when the host is created).
    struct RemoteHost
    {
        std::wstring name;        // friendly display name (falls back to address if empty)
        std::wstring address;     // hostname / IP — the stable key (case-insensitive)
        std::wstring username;    // saved user name to pre-fill the prompt (may be empty)
        std::wstring domain;      // optional domain (may be empty)
        uint16_t     port = 3389; // RDP port
        RdpOptions   rdp;         // per-host RDP options
    };

    class Settings
    {
    public:
        // Lazily constructs on first call. Construction loads settings.json
        // from disk (or applies defaults if missing/corrupt) and spawns the
        // debounced save thread. Safe to call from any thread.
        static Settings& Instance();

        // ---- Diagnostics ----------------------------------------------------
        bool LoggingEnabled() const;
        void SetLoggingEnabled(bool on);

        // ---- General behaviour ---------------------------------------------
        bool KeepHomeTabOpen() const;
        void SetKeepHomeTabOpen(bool on);

        // ---- VM debugger launch --------------------------------------------
        bool         DebuggerEnabled() const;
        void         SetDebuggerEnabled(bool on);
        std::wstring DebuggerExe() const;                 // global default exe
        void         SetDebuggerExe(std::wstring const& exe);
        // Per-VM: the raw exe override (empty = use the global exe) + args
        // (empty = none, which disables the launch button for that VM).
        std::wstring VmDebuggerExe(std::wstring const& guid) const;   // raw override
        std::wstring VmDebuggerArgs(std::wstring const& guid) const;
        // Resolves the per-VM override-or-global exe — what the launcher runs.
        std::wstring EffectiveDebuggerExe(std::wstring const& guid) const;
        void         SetVmDebugger(std::wstring const& guid,
                                   std::wstring const& exeOverride,
                                   std::wstring const& args);

        // ---- Window geometry -----------------------------------------------
        Window WindowGeometry() const;
        void   SetWindowGeometry(Window const& w);

        // ---- Recently-opened VMs ------------------------------------------
        std::vector<Recent> Recents() const;
        // Push or refresh an entry in the recents list. Most-recent first;
        // duplicate keys are deduped (existing entry's lastOpened + kind
        // updated). List is implicitly trimmed to kMaxRecents. kind defaults
        // to Vm so existing VM call sites are unchanged; OpenRemoteHostTab
        // passes Remote.
        void                BumpRecent(std::wstring const& key,
                                       RecentKind kind = RecentKind::Vm);
        // Drop an entry from the list by key (VM deleted / host forgotten).
        // Key-only match (VM GUIDs and host addresses don't collide).
        void                ForgetRecent(std::wstring const& key);

        // ---- Open tabs (session restore) ----------------------------------
        // Snapshot of the tabs that were open at last persist + which one
        // was active. Restored on launch by MainWindow::OnActivated after
        // VMManager has the VM list available.
        std::vector<OpenTab> OpenTabs() const;
        int                  SelectedTabIndex() const;
        // Replace the persisted snapshot atomically. Called whenever the
        // tab strip changes (open / close / reorder / select). The
        // debounced save thread coalesces rapid calls.
        void                 SetOpenTabs(std::vector<OpenTab> const& tabs,
                                         int selectedIndex);

        // ---- Remote hosts (saved RDP machines) ----------------------------
        // The full saved-hosts list, in user/insertion order. Drives the
        // welcome-page "Remote Hosts" section + the rail expander.
        std::vector<RemoteHost> RemoteHosts() const;
        // Look up one host by its address key (case-insensitive). nullopt if
        // not found — used by OpenRemoteHostTab / restore to resolve a tab's
        // identifier back to its connection details.
        std::optional<RemoteHost> FindRemoteHost(std::wstring const& address) const;
        // Add a new host, or update an existing one. originalAddress empty =>
        // add (deduped by host.address); otherwise the entry whose address ==
        // originalAddress is replaced (allowing the address itself to change).
        // Dedup + replace are case-insensitive on address.
        void AddOrUpdateRemoteHost(std::wstring const& originalAddress,
                                   RemoteHost const& host);
        // Drop a saved host by address (case-insensitive). No-op if absent.
        void RemoveRemoteHost(std::wstring const& address);

        // ---- Appearance ----------------------------------------------------
        Appearance AppearancePref() const;
        void       SetAppearance(Appearance const& a);

        // ---- Per-VM preferences -------------------------------------------
        // Whether the user wants to use enhanced session mode for this VM.
        // Defaults to true (the friendlier session mode — clipboard, audio,
        // device redirection). Settable to false to force a basic-session
        // connect even when Hyper-V reports enhanced as available. The
        // StartConnection path still gates on the VM's currently-reported
        // enhancedSessionAvailable, so flipping this on for a guest that
        // can't actually do enhanced (Linux without LIS, firmware/boot
        // screen, etc.) is a no-op — pref acts as an opt-out, not an
        // override that ignores Hyper-V's report.
        bool EnhancedSessionPref(std::wstring const& vmGuid) const;
        void SetEnhancedSessionPref(std::wstring const& vmGuid, bool on);

        // Cached observation: has this VM ever been seen reporting enhanced
        // session as available? Lets the context menu correctly grey the
        // toggle even when the VM is currently off (Hyper-V only reports
        // EnhancedSessionModeState while the VM is running with LIS up).
        // Defaults to true — for a VM we've never observed running, give
        // the benefit of the doubt and don't grey the toggle.
        bool EnhancedSessionEverSupported(std::wstring const& vmGuid) const;
        // Record a positive observation. Sticky — once we've seen support
        // for a VM we keep it, even across "no LIS yet" snapshots at the
        // boot screen. No method to clear other than ClearVmPrefs.
        void ObserveEnhancedSupport(std::wstring const& vmGuid);

        // ---- RDP options (app-wide defaults + sparse per-VM overrides) ----
        // App-wide defaults — what gets used for any VM that doesn't have
        // its own override. Mutating these instantly affects subsequent
        // VM connections (Set fires the debounced save; the live RDP
        // sessions don't reconnect automatically — the new values land on
        // the next StartConnection).
        RdpOptions RdpDefaults() const;
        void       SetRdpDefaults(RdpOptions const& opts);

        // Per-VM override accessors. HasRdpOptionsOverride returns true
        // when the user has explicitly customized this VM; SetRdpOptionsOverride
        // installs a new override (Save fires); ClearRdpOptionsOverride
        // drops the override and the VM falls back to defaults.
        bool       HasRdpOptionsOverride(std::wstring const& vmGuid) const;
        void       SetRdpOptionsOverride(std::wstring const& vmGuid, RdpOptions const& opts);
        void       ClearRdpOptionsOverride(std::wstring const& vmGuid);

        // Effective options for the VM = override if present, defaults
        // otherwise. The hot path that VmTabPage::StartConnection should
        // call — single lookup, no caller-side branching.
        RdpOptions RdpOptionsFor(std::wstring const& vmGuid) const;

        // Drop the user override for a VM (e.g. when the VM is deleted).
        void ClearVmPrefs(std::wstring const& vmGuid);

        // ---- Confirmations -------------------------------------------------
        // Whether the per-action confirmation dialog should be shown.
        // Returns the user's explicit override if one is set, otherwise the
        // baked-in default (see DefaultConfirmationEnabled in the .cpp).
        // Keys are short lowercase camelCase strings — e.g. "reset",
        // "turnOff", "deleteVm". Unknown keys default to true (safer fallback;
        // a misuse surfaces as an extra dialog rather than a silent destructive
        // action). All known keys SHOULD live in the defaults switch.
        bool ConfirmationEnabled(std::wstring const& key) const;
        // Persist an explicit override. Pass the inverse of the default to
        // make the suppression a user-explicit choice (writing a key that
        // equals the default still works — it just stores the override).
        void SetConfirmationEnabled(std::wstring const& key, bool on);
        // Drop the user override for a key, falling back to the default.
        // Reserved for a future "Reset to defaults" affordance in the App
        // Settings dialog; not used today.
        void ClearConfirmationOverride(std::wstring const& key);

        // ---- Misc -----------------------------------------------------------
        // Absolute path to the settings file — useful for an "Open in File
        // Explorer" entry in a future settings UI.
        std::filesystem::path FilePath() const;
        // Force-write now (e.g. on app shutdown so unflushed changes land).
        void                  SaveNow();

        // How many MRU entries we keep. Trim happens inside BumpRecent.
        static constexpr size_t kMaxRecents = 10;

    private:
        Settings();
        ~Settings();
        Settings(Settings const&)            = delete;
        Settings& operator=(Settings const&) = delete;

        void Load();
        void SaveLocked() const;   // assumes m_lock held by caller — writes to disk
        void MarkDirty();          // schedules a debounced save
        void SaveLoop();           // background thread body

        mutable std::mutex      m_lock;
        std::filesystem::path   m_path;
        Diagnostics             m_diagnostics;
        General                 m_general;
        Debugger                m_debugger;
        Window                  m_window;
        std::vector<Recent>     m_recents;
        std::vector<OpenTab>    m_openTabs;
        int                     m_selectedTabIndex = -1;
        std::vector<RemoteHost> m_remoteHosts;

        // Per-action confirmation overrides. Sparse — only keys the user has
        // explicitly set live here; everything else reads from
        // DefaultConfirmationEnabled. Stored in settings.json as
        // "confirmations": { "reset": true, "save": false, ... }.
        std::unordered_map<std::wstring, bool> m_confirmOverrides;

        // Per-VM preferences. Sparse — only VMs the user has explicitly
        // toggled have an entry. Stored in settings.json as
        // "vmPrefs": { "<guid>": { "enhancedSession": true, ... } } so the
        // shape can grow more keys later without a schema bump.
        struct VmPrefs
        {
            std::optional<bool> enhancedSession;          // user pref
            std::optional<bool> enhancedSessionSupported; // cached observation (sticky-true)
            // RDP override. Absent (nullopt) means "use app defaults";
            // present means the user has explicitly customized this VM
            // and we persist the full snapshot. JSON shape:
            //   "vmPrefs": { "<guid>": { "rdpOptions": { ... } } }
            std::optional<RdpOptions> rdpOptions;
            // Per-VM debugger override exe (absent = use the global exe) +
            // the launch arguments (absent/empty = no args = button disabled).
            std::optional<std::wstring> debuggerExe;
            std::optional<std::wstring> debuggerArgs;
        };
        std::unordered_map<std::wstring, VmPrefs> m_vmPrefs;

        // App-wide RDP defaults — used for any VM without an override.
        RdpOptions m_rdpDefaults{};

        Appearance m_appearance{};

        // Debounced background save thread.
        std::thread             m_saveThread;
        std::mutex              m_saveLock;
        std::condition_variable m_saveCv;
        std::atomic<bool>       m_dirty{ false };
        std::atomic<bool>       m_shutdown{ false };
    };
}
