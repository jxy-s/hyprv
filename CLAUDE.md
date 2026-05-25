# hyprv — Claude project briefing

This file briefs Claude on the hyprv codebase. Read this before touching anything. The living roadmap lives at `.claude/PLAN.md`; detailed component docs live under `docs/`.

## What hyprv is

A Hyper-V VM client for Windows. C++/WinRT + WinUI 3. **Debug builds are unpackaged; Release ships as a signed MSIX bundle (x64 + ARM64) distributed via an `.appinstaller`** — see *Release / distribution* below. It's a from-scratch rewrite of the older VMPlex (C#/WPF, sibling repo) — with one key architectural change:

**Out-of-process RDP host (`hyprv-rdphost.exe`).** VMPlex hosted the leaky `mstscax` ActiveX control in-process; closing a tab leaked GDI handles + memory until the whole process died. hyprv spawns a separate child process per VM tab to host mstscax. Closing a tab tears down the child; the parent's working set stays clean.

```
┌────────────────────┐         IPC: named pipe         ┌──────────────────────┐
│  hyprv.exe         │ ◀───────────────────────────▶  │  hyprv-rdphost.exe   │
│  WinUI 3 main app  │   Hello/Connect/Resize/...     │  hosts mstscax       │
│  tabs, rail, ...   │                                 │  one per VM tab      │
└────────────────────┘                                 └──────────────────────┘
                  ▼ owns popup HWND (cross-process)
                  ▼ paints over a XAML `<Border rdpHost>`
```

**Multi-window (tab tear-away).** The app is no longer single-window: a tab can be dragged out into its own `MainWindow`, or dragged onto another open window's strip. A strong-ref `ui/WindowManager` registry keeps torn windows alive; the moved `TabViewItem` (and its live `RdpHostClient` + rdphost child) re-parents intact — only the popup HWND owner and the page↔window weak_ref get re-wired (`AdoptMovedTab`). One window is the `m_isPrimary`; only it persists geometry + open tabs. See gotchas #44–48.

## Repository layout

```
.
├── CLAUDE.md              ← you're here
├── README.md              human intro
├── .claude/
│   ├── settings.json      project-scoped permissions for Claude Code
│   ├── PLAN.md            living roadmap, current focus, open questions
│   └── skills/            project-specific skills (build, verify, …)
├── docs/
│   ├── ARCHITECTURE.md    detailed component breakdown
│   └── IPC.md             rdphost ↔ parent wire protocol
├── hyprv.slnx             MSBuild solution (.slnx — NOT .sln)
├── nuget.config           pins repositoryPath="packages" at repo root
├── src/                   all C++ source under here
│   ├── app/               the main WinUI 3 app — produces hyprv.exe
│   │   ├── hyprv.vcxproj
│   │   ├── MainWindow.{xaml,xaml.h,xaml.cpp}      title bar + tab strip + rail + flyout
│   │   ├── VmTabPage.{xaml,xaml.h,xaml.cpp}       per-VM tab content (the rdpHost Border)
│   │   ├── App.{xaml,xaml.cpp,xaml.h}             app entry
│   │   ├── rdp/RdpHostClient.{h,cpp}              IPC client to hyprv-rdphost.exe
│   │   ├── settings/Settings.{h,cpp}              persisted-prefs singleton
│   │   ├── vm/                                    VMManager + VirtualMachine model
│   │   ├── wmi/                                   thin WMI wrapper + generated Hyper-V bindings
│   │   └── Assets/                                appx icons
│   ├── rdphost/           the out-of-process RDP host — produces hyprv-rdphost.exe
│   │   ├── hyprv-rdphost.vcxproj
│   │   ├── main.cpp                               wWinMain, popup window, message loop
│   │   ├── RdpHost.{h,cpp}                        mstscax setup + DISPID event sink
│   │   └── IpcClient.{h,cpp}                      named-pipe transport
│   ├── shared/
│   │   └── RdpIpc.h                               IPC message types — wire contract between both processes
│   ├── resources/         icons (referenced by both .rc files)
│   └── obj/               intermediate build files (gitignored)
├── bin/                   final build output — `bin/x64/Debug/{hyprv,hyprv-rdphost}.exe` (gitignored)
└── packages/              NuGet packages (gitignored — restore once after clone)
```

### Project naming (subtle — do not "fix")

- The repo and the EXE are both named `hyprv`.
- The main project lives at `src/app/` (vcxproj file is `src/app/hyprv.vcxproj`).
- The C++/WinRT projection is `winrt::hyprv_app::` (with `_app` suffix).
- The vcxproj `<RootNamespace>` is `hyprv_app` and matches.
- The `<TargetName>` is `hyprv`, producing `hyprv.exe`.

**Do not rename `hyprv_app` → `hyprv`.** It was attempted once and reverted. Reason: the WinRT projection lives at `winrt::hyprv_app::` while our internal C++ code lives at `::hyprv::` (`hyprv::ipc`, `hyprv::wmi`, `hyprv::app::settings`, `hyprv::app::vm`, `hyprv::rdphost`). If the WinRT projection were just `winrt::hyprv::`, then inside any `winrt::hyprv::implementation` block, an unqualified `hyprv::` lookup finds `winrt::hyprv` (the enclosing namespace) first and our internal `::hyprv::` becomes unreachable without a leading `::`. MSVC reports `'app': the symbol to the left of a '::' must be a type` because it's trying `winrt::hyprv::app`.

The `_app` suffix on the projection is the standard pattern for exactly this case (projects whose root namespace would otherwise collide with their internal one).

## Build

See `.claude/skills/build/SKILL.md` for the full recipe. Quick form:

```powershell
# One-time per clone:
msbuild 'hyprv.slnx' -t:Restore -p:RestorePackagesConfig=true -p:Configuration=Debug -p:Platform=x64
# Each build:
msbuild 'hyprv.slnx' -p:Configuration=Debug -p:Platform=x64 -v:minimal -nologo
```

Both commands must run inside a VS 2026 DevShell (`Enter-VsDevShell -VsInstallPath 'C:\Program Files\Microsoft Visual Studio\18\Community' …`). Plain PowerShell can't resolve the `<WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>` for the Windows Store ApplicationType.

Output layout:
- **Final binaries**: `bin/x64/Debug/{hyprv,hyprv-rdphost}.exe` — at the repo root, next to `src/`.
- **Intermediate files** (.obj, .pch, generated headers, winmd): `src/obj/<project>/x64/Debug/`.

Both are gitignored. Always build via `hyprv.slnx`, never the vcxproj directly — `$(SolutionDir)` resolves differently and the outputs land in the wrong place.

## Release / distribution

Debug = unpackaged. **Release ships as a signed MSIX bundle (x64 + ARM64) via an `.appinstaller`** on GitHub Releases (`github.com/jxy-s/hyprv`). One command:

```powershell
pwsh tools/set-version.ps1 1.0.0.2   # stamp manifest Identity + both .rc FILEVERSION/PRODUCTVERSION in lockstep
pwsh tools/make-msix.ps1             # clean -> per-arch -> makeappx bundle -> Trusted-sign -> emit .appinstaller; prints the gh release line
```

- **Framework-dependent** (self-contained is broken for this WinUI stack — gotcha #50). The runtime is a `PackageDependency`; the `.appinstaller` `<Dependencies>` auto-installs it (gotcha #51).
- **Azure Trusted Signing** — you `az login` (need the "Trusted Signing Certificate Profile Signer" role); the script signs against that session. **Never handle the maintainer's Azure creds — direct them to do it.**
- **Release assets** (attach all four, exact filenames): `hyprv.msixbundle`, `Microsoft.WindowsAppRuntime.2-x64.msix`, `Microsoft.WindowsAppRuntime.2-arm64.msix`, `hyprv.appinstaller`. Install = **download the `.appinstaller` and double-click** (the `ms-appinstaller:` web protocol is disabled by default).
- Packaging lives in `src/package/` (`hyprv-package.wapproj`, `Package.appxmanifest`, `hyprv.appinstaller.template`, `Assets/`). Gotchas #50–54.

## Runtime data layout

- `%LOCALAPPDATA%\hyprv\settings.json` — persisted user prefs (pretty-printed JSON, schema v1)
- `%LOCALAPPDATA%\hyprv\hyprv.log` — unified log. rdphost children forward log lines via IPC; parent writes them prefixed `[rdphost <VMNAME> pid=N]`. Logging gated by `Settings::LoggingEnabled()`. Default: on in Debug, off in Release.
- Both files opened with share-read so external tools can tail/inspect while hyprv runs.
- **They live in the REAL `%LOCALAPPDATA%\hyprv` even under MSIX** (intentional persisted state, survives uninstall). Requires BOTH `SHGetKnownFolderPath(..., KF_FLAG_NO_PACKAGE_REDIRECTION)` AND the manifest `unvirtualizedResources` + `FileSystemWriteVirtualization` exclusion — either alone silently lands the file in the package LocalCache. Gotcha #53.

## Coding conventions

- **C++20**, MSVC v143/v145 toolset.
- **C++/WinRT** for all XAML / WinRT calls. Never use C++/CX.
- **`std::wstring`** at C++ boundaries; convert to `winrt::hstring` only at the XAML edge. UTF-8 only on the IPC wire (encode/decode at the boundary).
- **No exceptions across thread boundaries.** WMI calls go inside `try { ... } catch (hyprv::wmi::WmiException const& e) { HyprvAppLog(...); }` blocks — see `VMManager.cpp` for the canonical pattern.
- **All callbacks fire on the rx thread.** Anything that touches XAML marshals via `m_uiQueue.TryEnqueue(...)`. Capture `weak_ref` (not `this`) in lambdas.
- **Comments explain *why*, not *what*.** Many subtle invariants (popup-must-precede-Connect, mstscax DISPID order, etc.) are documented inline at the relevant code site. Read them.

## Hot files / where things live

| Concern                                          | File(s)                                          |
|--------------------------------------------------|--------------------------------------------------|
| App lifecycle (`OnLaunched`)                     | `src/app/App.xaml.cpp`                             |
| Title bar, tab strip, rail, info flyout          | `src/app/MainWindow.xaml{,.h,.cpp}`                |
| Per-VM tab content + rdphost client lifecycle    | `src/app/VmTabPage.xaml{,.h,.cpp}`                 |
| Welcome (per-host portal) page                   | `src/app/WelcomePage.xaml{,.h,.cpp}`               |
| App Settings (search + instant-apply tab)        | `src/app/AppSettingsPage.xaml{,.h,.cpp}`           |
| VM hardware settings editor (modal)              | `src/app/VmSettingsDialog.xaml{,.h,.cpp}`          |
| New VM wizard (modal)                            | `src/app/NewVmDialog.xaml{,.h,.cpp}` + `VMManager::CreateVM`; opened via `MainWindow::OpenNewVmDialog` |
| Remote host tab (generic RDP session)            | `src/app/RemoteHostTabPage.xaml{,.h,.cpp}`; opened via `MainWindow::OpenRemoteHostTab` |
| Add/edit Remote Host dialog (modal)              | `src/app/RemoteHostDialog.xaml{,.h,.cpp}`; opened via `MainWindow::OpenRemoteHostDialog` |
| Tab tear-away + cross-window drag (drag handlers) | `src/app/MainWindow.xaml.cpp` (`OnTabDragStarting` / `OnTabViewDrop` / `OnWindowDrop` / `OnTabDragCompleted` / `WindowUnderPoint` / `MoveTabToThisWindow` / `SpawnWindowForTab` / `AdoptMovedTab`) + `MainWindow.xaml` (TabView + `rootGrid` + `titleBarTrailing` drop targets) |
| Multi-window registry (keeps torn windows alive) | `src/app/ui/WindowManager.{h,cpp}` (`Track` / `Find` / `All`); primary tracked in `App.xaml.cpp` |
| Per-VM tile + context menu builder (shared)      | `src/app/ui/VmTileFactory.{h,cpp}` (`BuildVmContextMenu` / `BuildRemoteHostContextMenu`) |
| Centralised confirmation dialog helper           | `src/app/ui/ConfirmDialog.{h,cpp}` (`ConfirmAndAct` / `ConfirmAndActWithCheckbox`) |
| Popup `SystemBackdrop` helper for menus/flyouts  | `src/app/ui/PopupBackdrop.{h,cpp}`                 |
| Theme / backdrop / restart-banner logic          | `src/app/MainWindow.xaml.cpp` `ApplyAppearance` / `UpdateBackdropTintOpacity` |
| IPC client (parent side)                         | `src/app/rdp/RdpHostClient.{h,cpp}`                |
| IPC wire types (shared by both processes)        | `src/shared/RdpIpc.h`                              |
| IPC server / mstscax host (child side)           | `src/rdphost/main.cpp`, `src/rdphost/RdpHost.{h,cpp}` |
| Persisted prefs + JSON I/O                       | `src/app/settings/Settings.{h,cpp}`                |
| VM state, polling, state-change requests         | `src/app/vm/VMManager.{h,cpp}`, `VirtualMachine.h` |
| WMI primitives                                   | `src/app/wmi/Wmi{Object,Scope,Subscription}.{h,cpp}` |
| Generated Hyper-V WMI bindings                   | `src/app/wmi/generated/HyperV.h` (regen via `wmi/gen/`) |
| Shared logger                                    | `HyprvAppLog` defined in `MainWindow.xaml.cpp`     |
| MSIX packaging (manifest, wapproj, assets)       | `src/package/{Package.appxmanifest, hyprv-package.wapproj, Assets/}` |
| `.appinstaller` template (auto-update + runtime dep) | `src/package/hyprv.appinstaller.template` (→ `dist/hyprv.appinstaller`) |
| Build/sign the signed MSIX bundle release        | `tools/make-msix.ps1`                              |
| Stamp version across manifest + both `.rc`       | `tools/set-version.ps1 X.Y.Z.W`                    |
| EXE version metadata (Explorer Properties)       | `src/app/hyprv.rc`, `src/rdphost/hyprv-rdphost.rc` (`VERSIONINFO`) |
| Hyper-V connect status / poll / connect-error UI | `src/app/vm/VMManager.cpp` (`TryConnect`/`ConnectStatus`/`PollLoop`) + `src/app/WelcomePage.xaml.cpp` (connect-error panel + `OnRetryConnectClick`) |
| Settings + log path (real %LOCALAPPDATA% on MSIX) | `src/app/settings/Settings.cpp` (`LocalAppDataDir`) + `src/package/Package.appxmanifest` (`unvirtualizedResources`) |

## Hard-won gotchas

**ALL hard-won gotchas live in `docs/GOTCHAS.md` — that file is the single source of truth.** It holds the full detail behind every subtle, expensive-to-rediscover invariant in hyprv (rdphost/mstscax/IPC, WMI, tab tear-away, theme, MSIX, and more), grouped by area and numbered. Do NOT duplicate any of that here.

Two strict rules:

1. **Consulting gotchas — ALWAYS.** Any time you are about to touch an area with known traps (anything involving WMI `Modify*`/`Add*` settings, mstscax/RDP options, the rdphost popup or IPC wire format, tab drag/tear-away, multi-window, backdrop/theme chrome, ContentDialog/coroutine lifetime, boot order, storage/NIC/security/snapshot/serial-port edits, VM creation, or MSIX packaging), **read the matching section of `docs/GOTCHAS.md` FIRST.** When debugging a "settings save fine but nothing happens" / silent-no-op symptom, `docs/GOTCHAS.md` is the first place to look.

2. **Recording gotchas — ALWAYS into `docs/GOTCHAS.md`, NEVER here.** When you discover a new hard-won invariant, add it to `docs/GOTCHAS.md` (next number, in the right area section; also write it up inline at the code site). Keep `CLAUDE.md` free of gotcha detail so it stays small.

## Multi-agent guidance

If running multiple agents in parallel on this repo, give each a clearly scoped sub-area to avoid stepping on each other:

- **Rail / VM list / context menu**: `MainWindow.xaml.cpp` lines that touch `RenderRail` / `CreateRailItem` + `src/app/ui/VmTileFactory.cpp` `BuildVmContextMenu`.
- **Info flyout**: `MainWindow.xaml.cpp` `UpdateInfoFlyout*` / `ResetFlyoutSections`.
- **Welcome page**: `src/app/WelcomePage.xaml{,.h,.cpp}` + `MainWindow.xaml.cpp` (`OpenWelcomeTab`, `ReplaceTabWith`, `CloseTabItem`).
- **App Settings page**: `src/app/AppSettingsPage.xaml{,.h,.cpp}`.
- **VM settings editor (modal)**: `src/app/VmSettingsDialog.xaml{,.h,.cpp}`.
- **New VM wizard (modal)**: `src/app/NewVmDialog.xaml{,.h,.cpp}` + `VMManager::CreateVM` + `MainWindow::OpenNewVmDialog`.
- **Remote Hosts**: `src/app/RemoteHostTabPage.xaml{,.h,.cpp}` + `src/app/RemoteHostDialog.xaml{,.h,.cpp}` + `MainWindow::{OpenRemoteHostTab,OpenRemoteHostDialog,ForgetRemoteHost,RenderRemoteHostsRail}` + the welcome "Remote Hosts" section + `RdpHost::OnConnect` + the `remoteHosts` Settings section. Touches `RdpIpc.h` (coordinate per below).
- **Tab lifecycle / rdpHost popup**: `src/app/VmTabPage.xaml{,.h,.cpp}`.
- **Tab tear-away / multi-window**: the drag handlers + `WindowUnderPoint` / `MoveTabToThisWindow` / `SpawnWindowForTab` / `AdoptMovedTab` / `HandleSourceAfterDetach` in `MainWindow.xaml.cpp`, the TabView + `rootGrid` + `titleBarTrailing` drop markup in `MainWindow.xaml`, and `src/app/ui/WindowManager.{h,cpp}`. Cuts across `m_isPrimary` persistence gating + the `VMManager` multicast subscription — coordinate with the WMI/VM-state owner if touching `VMManager`. Gotchas #44–48.
- **Theme / appearance**: `MainWindow.xaml.cpp` `ApplyAppearance` / `UpdateBackdropTintOpacity` + `src/app/App.xaml`.
- **Popup chrome helpers**: `src/app/ui/PopupBackdrop.{h,cpp}`, `src/app/ui/ConfirmDialog.{h,cpp}`.
- **IPC protocol**: `src/shared/RdpIpc.h` — coordinate ANY change here through a single agent; both `src/app/` and `src/rdphost/` need a matching update.
- **Settings**: `src/app/settings/`.
- **WMI / VM state**: `src/app/vm/` and `src/app/wmi/`.
- **rdphost child process internals**: `src/rdphost/`.

When in doubt, claim a sub-area on `.claude/PLAN.md` under "in-flight" and don't cross those file boundaries.

## See also

- `.claude/PLAN.md` — current roadmap + open questions
- `docs/GOTCHAS.md` — full detail behind every numbered gotcha above
- `docs/ARCHITECTURE.md` — detailed component descriptions
- `docs/IPC.md` — wire-protocol reference
- `.claude/skills/build/SKILL.md` — build invocation
- `.claude/skills/verify/SKILL.md` — how to validate a change without committing
