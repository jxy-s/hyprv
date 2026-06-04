# hyprv — Claude project briefing

This file briefs Claude on the hyprv codebase. Read this before touching anything. The living roadmap lives at `.claude/PLAN.md`; detailed component docs live under `docs/`.

## What hyprv is

A Hyper-V VM client for Windows. C++/WinRT + WinUI 3. From-scratch rewrite of the older VMPlex (C#/WPF, sibling repo). Debug builds are unpackaged; Release ships as a signed MSIX bundle (x64 + ARM64) via an `.appinstaller` (see the `release` skill).

Two architectural points that drive most of the design — the full mental model + diagrams are in `docs/ARCHITECTURE.md` (and `docs/IPC.md` for the wire protocol):

- **Out-of-process RDP host (`hyprv-rdphost.exe`).** VMPlex hosted the leaky `mstscax` ActiveX control in-process; closing a tab leaked handles+memory until the process died. hyprv spawns one child process per VM tab to host mstscax over a named-pipe IPC; closing a tab tears down the child so the parent's working set stays clean. Wire contract: `src/shared/RdpIpc.h`.
- **Multi-window (tab tear-away).** Not single-window: a tab can be dragged out into its own `MainWindow` or onto another window's strip. A strong-ref `ui/WindowManager` registry keeps torn windows alive; the moved `TabViewItem` (+ its live `RdpHostClient` + rdphost child) re-parents intact via `AdoptMovedTab`. One window is `m_isPrimary` and persists geometry + open tabs. See gotchas #44–48.

## Repository layout

All C++ source is under `src/`. The non-obvious split: `src/app/` = the main WinUI 3 app (produces `hyprv.exe`); `src/rdphost/` = the out-of-process mstscax host (produces `hyprv-rdphost.exe`); `src/shared/RdpIpc.h` = the IPC wire contract shared by both. Intermediate build files land in `src/obj/` (gitignored); final binaries in `bin/` (gitignored). For where any specific concern lives, use the *Hot files* table below — it's the canonical map. Otherwise explore the tree directly.

### Project naming (subtle — do not "fix")

- The repo and the EXE are both named `hyprv`; the main project is `src/app/hyprv.vcxproj` with `<RootNamespace>hyprv_app` and `<TargetName>hyprv`.
- The C++/WinRT projection is `winrt::hyprv_app::`; internal C++ code is `::hyprv::`.
- **Do not rename `hyprv_app` → `hyprv`.** It was attempted once and reverted — the `_app` suffix prevents a namespace collision that makes our internal `::hyprv::` unreachable. Full rationale in `docs/GOTCHAS.md` #59.

## Build

See `.claude/skills/build/SKILL.md` for the full recipe (and `.claude/skills/verify/SKILL.md` to validate a change). Quick form, inside a VS 2026 DevShell:

```powershell
# One-time per clone:
msbuild 'hyprv.slnx' -t:Restore -p:RestorePackagesConfig=true -p:Configuration=Debug -p:Platform=x64
# Each build:
msbuild 'hyprv.slnx' -p:Configuration=Debug -p:Platform=x64 -v:minimal -nologo
```

Always build via `hyprv.slnx`, never the vcxproj directly — `$(SolutionDir)` resolves differently and outputs land in the wrong place. Final binaries: `bin/x64/Debug/{hyprv,hyprv-rdphost}.exe`. Intermediates: `src/obj/<project>/x64/Debug/`. Both gitignored.

## Release / distribution

Release ships as a signed MSIX bundle (x64 + ARM64) via an `.appinstaller`, framework-dependent, Azure Trusted Signing. **Full recipe, asset list, and signing caveats are in the `release` skill (`.claude/skills/release/SKILL.md`).** Gotchas #50–54.

## Runtime data layout

- `%LOCALAPPDATA%\hyprv\settings.json` — persisted user prefs (pretty-printed JSON, schema v1)
- `%LOCALAPPDATA%\hyprv\hyprv.log` — unified log. rdphost children forward log lines via IPC; parent writes them prefixed `[rdphost <VMNAME> pid=N]`. Gated by `Settings::LoggingEnabled()` (default: on in Debug, off in Release).
- Both opened with share-read so external tools can tail/inspect while hyprv runs.
- **They live in the REAL `%LOCALAPPDATA%\hyprv` even under MSIX** (intentional persisted state, survives uninstall). Requires BOTH `SHGetKnownFolderPath(..., KF_FLAG_NO_PACKAGE_REDIRECTION)` AND the manifest `unvirtualizedResources` + `FileSystemWriteVirtualization` exclusion — either alone silently lands the file in the package LocalCache. Gotcha #53.

## Coding conventions

- **C++20**, MSVC v143/v145 toolset.
- **C++/WinRT** for all XAML / WinRT calls. Never use C++/CX.
- **`std::wstring`** at C++ boundaries; convert to `winrt::hstring` only at the XAML edge. UTF-8 only on the IPC wire (encode/decode at the boundary).
- **No exceptions across thread boundaries.** WMI calls go inside `try { ... } catch (hyprv::wmi::WmiException const& e) { HyprvAppLog(...); }` — see `VMManager.cpp` for the canonical pattern.
- **All callbacks fire on the rx thread.** Anything that touches XAML marshals via `m_uiQueue.TryEnqueue(...)`. Capture `weak_ref` (not `this`) in lambdas.
- **Comments explain *why*, not *what*.** Many subtle invariants are documented inline at the relevant code site. Read them.

## Hot files / where things live

This table is the canonical "concern → file" map for the repo.

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
| MSIX packaging + `.appinstaller` + version stamping | see the `release` skill                         |
| Hyper-V connect status / poll / connect-error UI | `src/app/vm/VMManager.cpp` (`TryConnect`/`ConnectStatus`/`PollLoop`) + `src/app/WelcomePage.xaml.cpp` (connect-error panel + `OnRetryConnectClick`) |
| Settings + log path (real %LOCALAPPDATA% on MSIX) | `src/app/settings/Settings.cpp` (`LocalAppDataDir`) + `src/package/Package.appxmanifest` (`unvirtualizedResources`) |

## Hard-won gotchas

**ALL hard-won gotchas live in `docs/GOTCHAS.md` — that file is the single source of truth.** It holds the full detail behind every subtle, expensive-to-rediscover invariant in hyprv (rdphost/mstscax/IPC, WMI, tab tear-away, theme, MSIX, naming, and more), grouped by area and numbered. Do NOT duplicate any of that here.

Two strict rules:

1. **Consulting gotchas — ALWAYS.** Any time you are about to touch an area with known traps (anything involving WMI `Modify*`/`Add*` settings, mstscax/RDP options, the rdphost popup or IPC wire format, tab drag/tear-away, multi-window, backdrop/theme chrome, ContentDialog/coroutine lifetime, boot order, storage/NIC/security/snapshot/serial-port edits, VM creation, or MSIX packaging), **read the matching section of `docs/GOTCHAS.md` FIRST.** When debugging a "settings save fine but nothing happens" / silent-no-op symptom, `docs/GOTCHAS.md` is the first place to look.

2. **Recording gotchas — ALWAYS into `docs/GOTCHAS.md`, NEVER here.** When you discover a new hard-won invariant, add it to `docs/GOTCHAS.md` (next number, in the right area section; also write it up inline at the code site). Keep `CLAUDE.md` free of gotcha detail so it stays small.

## Multi-agent guidance

If running multiple agents in parallel on this repo, each must claim a clearly scoped sub-area so they don't step on each other. **The per-area file scoping lives in the `multi-agent` skill (`.claude/skills/multi-agent/SKILL.md`).** Coordinate any change to `src/shared/RdpIpc.h` through a single agent (both processes need a matching update). When in doubt, claim a sub-area on `.claude/PLAN.md` under "in-flight".

## See also

- `.claude/PLAN.md` — current roadmap + open questions
- `docs/GOTCHAS.md` — full detail behind every numbered gotcha
- `docs/ARCHITECTURE.md` — detailed component descriptions + diagrams
- `docs/IPC.md` — wire-protocol reference
- `.claude/skills/{build,verify,release,multi-agent}/SKILL.md` — build, validation, release, parallel-work scoping
