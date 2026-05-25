# hyprv — Claude project briefing

This file briefs Claude on the hyprv codebase. Read this before touching anything. The living roadmap lives at `.claude/PLAN.md`; detailed component docs live under `docs/`.

## What hyprv is

A Hyper-V VM client for Windows. C++/WinRT + WinUI 3, unpackaged for now (MSIX-packaged eventually). It's a from-scratch rewrite of the older VMPlex (C#/WPF, sibling repo) — with one key architectural change:

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

## Runtime data layout

- `%LOCALAPPDATA%\hyprv\settings.json` — persisted user prefs (pretty-printed JSON, schema v1)
- `%LOCALAPPDATA%\hyprv\hyprv.log` — unified log. rdphost children forward log lines via IPC; parent writes them prefixed `[rdphost <VMNAME> pid=N]`. Logging gated by `Settings::LoggingEnabled()`. Default: on in Debug, off in Release.
- Both files opened with share-read so external tools can tail/inspect while hyprv runs.

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

## Hard-won gotchas

**Full detail lives in `docs/GOTCHAS.md`** (also written up inline at the code site and in the user's memory file `hyprv-rdphost-gotchas.md`). The numbered index below is a pointer — read the matching section in `docs/GOTCHAS.md` before touching the relevant area.

Recurring meta-rule — **the silent-no-op family** (#7, #16, #17, #26, #30, #35): a `Modify*Settings`/`Add*Settings` that returns `4096` + a clean `Msvm_ConcreteJob` (`JobState=7`, `ErrorCode=0`) but doesn't stick means Hyper-V silently rejected a property — wrong format (#16 braces, #35 empty-array-vs-`[""]`) or a state gate (Saved/running). No error to grep; diff your serialized XML against what the PowerShell cmdlet sends.

*rdphost / mstscax / IPC*
1. **AtlAxWin size = popup size at creation** — query popup client rect at child startup or you get a letterboxed render area.
2. **mstscax `EnableFrameBufferRedirection` is essential** for VM connections (else credential UI renders in a fixed upper-left rect).
3. **`Msvm_Keyboard` for Ctrl+Alt+Del and clipboard-paste**, not mstscax.
4. **`SELECT *` in WQL when you need `__PATH`** — explicit column lists drop system properties.
5. **rdphost log IPC must wait until after `HwndReady` is sent** + parent rx thread up, or a `LogLine` header is mis-read as `HwndReady`.
24. **Windows-key passthrough = `KeyboardHookMode=1` on `IMsRdpClientSecuredSettings`** (via `get_SecuredSettings2`, NOT the base `get_SecuredSettings`). Connect-time only.
15. **RDP options are parent-prepared (`VmTabPage::StartConnection`), child-applied (`RdpHost::OnConnectLocalVm` `put_*`)** — a new knob needs FOUR coordinated edits; DPI override needs the resize path too.
41. **Optimistic pre-connect on Start — and the keep-alive latch must NOT clear on `OnConnected`.** Clicking the placeholder Start button spawns the rdphost immediately (`m_pendingConnect`) so mstscax attaches as the VM powers on and catches the firmware boot prompt ("Press any key to boot from CD"), instead of waiting for the poll to report Running + a cold spawn. The latch keeps `UpdatePlaceholderAndClient` wanting a live client across the Off→Running gap; **clearing it when the session connects** (before the poll reports Running) makes the next poll compute `wantClient=false`, tear the working client down, and respawn a SECOND one when Running lands — two mstscax on one VM console → the new kicks the old (`disc=2`) → churn the user can't interact through. Clear it ONLY when the VM is actually Running (`UpdatePlaceholderAndClient`) or on a 25 s timeout. Reconnect from a basic/console session is fast (700 ms, catches per-reboot boot prompts during install) vs enhanced (2 s, the 20 s cooldown already routes not-ready enhanced to basic).
43. **Generic RDP (Remote Hosts) is `RdpHost::OnConnect` — distinct from the VM-console `OnConnectLocalVm`; reuse the generic mstscax setup, drop the VMBus bits.** A Remote Host is a real RDP server on `:3389`, not the Hyper-V console on `:2179`. Keep: ColorDepth / Desktop W·H / SmartSizing / KeyboardHookMode=1 / audio+redirect flags / DPI. Drop: `put_PCB`, `AuthenticationServiceClass("Microsoft Virtual Console Service")`, RDPPort 2179, `NegotiateSecurityLayer=FALSE`, `EnableFrameBufferRedirection`, `EnhancedMode` (all VMBus-console specific). Add: `put_Server(host)`, RDPPort 3389. **Credential model = "prompt every time" [user choice]:** NO password is ever stored or sent on the wire — set `EnableCredSspSupport=TRUE` (on `IMsRdpClientNonScriptable3`) + `PromptForCredsOnClient=TRUE` (**on `IMsRdpClientNonScriptable4`, NOT 3** — verify interface membership by grepping the generated `src/obj/.../mstscax.tlh`, the same way the DISPID/IID facts were found) so mstscax shows its own client-side credential prompt. **Do NOT pre-set `put_UserName`/`put_Domain`:** with NLA, a username + no password makes the control attempt an auth pass with incomplete creds (or the logged-in user's SSO), the server rejects it, and the prompt then carries **"The logon attempt failed"**. Supplying NO credentials gives a clean first prompt; the saved username won't pre-fill the first prompt but Windows remembers it via `TERMSRV/<host>` for later connects. The username rides a v3 wire field (`RdpOptions.userByteLen` + trailing UTF-8, after server/domain). `RemoteHostTabPage` is a leaner clone of VmTabPage's connection half (no VM state / enhanced / poll / pre-connect) — connects on load, initial desktop = window size, single Connect/Reconnect/Retry action. A new tab type means a branch in all 7 MainWindow discrimination sites (OnTabSelectionChanged, CloseTabItem, Persist/RestoreOpenTabs, Push/PopPopupSuppression) + WindowSubclassProc popup refresh.
42. **Connect-error surfacing keys off `fatal` + a `wasConnected` gate — do NOT show an error for every fatal drop.** The child classifies each mstscax disconnect: reason codes 0–3 (NoInfo/LocalNotError/RemoteByUser/ByServer) are benign, `>3` is `fatal`, and for fatal drops it forwards `IMsRdpClient::GetErrorDescription`'s localized text over the wire (`Disconnected` grew a `fatal` byte + trailing UTF-8 string → **protocol v2**, both EXEs rebuilt together). The parent only surfaces an error (inline in the placeholder, with a Retry button — never a modal, which would spam during churn) when a connect attempt that **never reached `Connected`** keeps failing `fatal` on a **Running** VM past `kMaxConnectFailures` (4). An *established* session that drops (was Connected, any reason) is a benign reboot/logoff → reconnect quietly; the optimistic pre-connect window never counts. `m_hadConnectError` latches the error view (so poll ticks + tab-switch-back keep it up, not a respawn) and clears on a successful connect / user Start / Retry / the VM leaving a connectable state. `Error` codes (HostStartupFailed/ProtocolError/BasicSessionWithShieldedVm) map to friendly strings on the parent and ride the same Retry path. `SetStatus` is log-only (no status line) — the placeholder is the sole visible surface.
49. **`hyprv-rdphost.exe` MUST be manifested `PerMonitorV2` (matching the parent) — a DPI-awareness mismatch mis-scales the whole RDP surface above 100% display scale.** The parent is `PerMonitorV2` (`src/app/app.manifest`) and `SetWindowPos`-es the child's owned popup with **physical-pixel** geometry + the real monitor DPI (`opts.dpiScalePercent` → `DesktopScaleFactor`). The child shipped with **no manifest → DPI-unaware**, so on a >100% monitor the OS bitmap-virtualizes it: child-side `GetWindowRect`/`SetWindowPos`/`WM_SIZE` run in virtualized 96-DPI coords while the parent feeds physical ones, the spaces diverge, and mstscax (reading 96 itself) renders the framebuffer shrunk into the upper-left of the `rdpHost` Border (only visible when scale ≠ 100%). Fix: `src/rdphost/app.manifest` (PerMonitorV2) wired via `<Manifest Include="app.manifest" />` in the rdphost `.vcxproj`. No parent-side change — it was already emitting physical px + real DPI. Rule: any process hosting a window the parent owns/positions across the boundary must declare the SAME awareness, or mixed-mode virtualization corrupts geometry above 100%. Verify with `mt.exe -inputresource:<exe>;#1 -out:<tmp>`. **PerMonitorV2 fixes the coordinate space but exposed two guest-SCALE follow-ups (both confirmed, full detail in `docs/GOTCHAS.md` #49):** enhanced post-login was sending `desktopScale=100` → tiny guest UI (now derives host scale %, snapped via `SnapRdpScalePercent`); and the enhanced PRE-login logon screen drifted to a corner at >100% because a fixed 1024×768 framebuffer ÷ scale gave a cramped logical desktop (now `VmTabPage::StartConnection` scales the initial enhanced framebuffer by the host scale so the logical size stays constant). BASIC sessions stay native 1:1 (no RDP scale knob; `SmartSizing` was tried + reverted — letterboxed).

*Tab tear-away / multi-window*
44. **Tear-away is the LEGACY drag-drop model (decide-on-release), NOT native `CanTearOutTabs`** (which flashes a pre-created window mid-drag — user-rejected). `CanDragTabs`+`CanReorderTabs`+`AllowDrop` on the TabView (NOT `AllowDropTabs`), with `rootGrid`/`titleBarTrailing` as `Background="Transparent"` drop targets. Identify the grabbed tab by HIT-TESTING the cursor (`TabUnderCursor` → static `g_dragItem`) — the page-in-`Tag` model makes `args.Tab()` report the first tab.
45. **NEVER restructure tabs inside a drag event — defer via `m_uiQueue.TryEnqueue` or you WEDGE the TabView** (drag adorner left painted, no further drags, a hang needing a kill; a thread dump shows NO deadlock — just the wedged control). Compute the decision synchronously, post `MoveTabToThisWindow`/`SpawnWindowForTab` to the next turn.
46. **The custom title bar is a Win32 caption — OLE can't drop onto it; route caption/desktop releases via `DropResult == None`** (`OnTabDragCompleted` → `WindowUnderPoint`). A `Move` result was already handled (OLE drop / reorder) → return early. Do NOT dynamically clear the caption mid-drag (`SetTitleBar(null)`/`ClearRegionRects`) — it REGRESSES first-drag drops + reorder. The no-drop cursor over the title bar is a cosmetic OS limit; the drop still works on release.
47. **`WindowUnderPoint` = topmost window (`WindowFromPoint`→`GA_ROOT`, NO `GW_OWNER`) + title-bar BAND check** — the rdphost surface is a separate cross-process window over the body, so resolving its owner wrongly made the RDP content a move target. Only row 0 (TabView `ActualHeight`) is a move-in target; body → tear out / break away. Lone tab never tears out a NEW window but DOES move INTO another. Torn-window foreground via a Low-priority dispatcher turn (the source re-asserts otherwise).
48. **Multi-window plumbing:** strong-ref `WindowManager` registry (else torn windows are collected); `m_isPrimary` gate (secondary skips geometry/tab restore + persist — the Settings blob is global ⇒ torn windows aren't session-persisted, v1 limit); MULTICAST `VMManager` `Add/RemoveOnChanged`/`OnError` (single-sink starved all but the last window); `AdoptMovedTab` re-wires (popup `Reown`/`SetMainWindow`/ContextFlyout); `ShutdownAllTabClients` on a secondary's Close (else rdphost orphans). `get_weak().get()` yields the IMPL directly; `get_self<MainWindow>` is for PROJECTED objects only (C2440 on an impl).

*Window / tab / popup lifecycle*
6. **`ApplyPersistedGeometry` before `AttachWindowSubclass`** in `OnActivated`, or the initial layout pass clobbers restored geometry.
13. **Tab restore can't call `VMManager::GetByGuid` during `OnActivated`** (poll hasn't run) — restore unconditionally with empty name; don't persist the restore's own output.
14. **Cold-start popup-position race in `VmTabPage::ShowPopup`** — pin FIRST, latch `m_deferredShow` on failure. Shared latch across `ShowPopup`/`OnLoaded`/`OnRdpHostSizeChanged`/`HidePopup`/`TearDownClient`.

*Coroutine / WinUI dialog lifetime*
10. **ContentDialog button-click coroutine: copy `args` by value before any `co_await`** (the `const&` dangles). Use `ResumeOnDispatcher` (no `resume_foreground` for `MUX` DispatcherQueue).
19. **An immediately-invoked capturing lambda coroutine dangles its captures after first `co_await`** — take everything as by-value PARAMETERS (`ShowConfirmCoro`). Bit every confirmation-ON destructive action.
25. **Show "working" overlay via `co_await resume_after(150ms)` before UI-thread WMI**, not a single `resume_background` round-trip.

*Theme / menu chrome*
11. **MenuFlyout chrome can't be cleanly narrowed** — stock toggle + stock chrome; don't re-attempt without retemplating both item types end-to-end.
12. **Backdrop controllers (`MicaController`/`DesktopAcrylicController`), not the `*Backdrop` wrappers** — needed for `IsInputActive` + live `TintOpacity`. Black mode = tint override, not backdrop-off. `ContentDialog`/`ComboBox` use brush overrides; `MenuFlyout` via `PopupBackdrop::ApplyTo`.
20. **Rail context-menu state-gating must re-evaluate in `menu.Opening`** — rail tiles are reused, not rebuilt per poll, so the menu freezes at first-build state otherwise.

*WMI fundamentals*
7. **`Modify{System,Resource}Settings` wants WMI DTD 2.0 XML (`WmiObject::GetCimXml()`), not MOF** — keep system props/qualifiers. Gate edits on `VmState::Off` strictly.
8. **WMI proxies are apartment-bound** — `m_scope` is UI-STA; `PollLoop` builds its own scope; do `Set*`s on the UI thread, only sleeps on bg threads.
9. **`WmiObject::GetObjectArray` is defensive** — Hyper-V returns embedded-XML `VT_BSTR` for declared `Instance[]` out-params; the check makes it a no-op instead of an AV.
33. **A single-object CIM `Reference` in-param must be set via `.Path()`, NOT the embedded `WmiObject`** (codegen gets this wrong → `0x80041005` "Put failed"). This is why snapshots never worked. Rule: any `Add*`/`Modify*`/`Destroy*`/`Apply*`/`Create*` param that references an EXISTING object → `.Path()`.

*Power / state-change*
22. **`RequestStateChange(Disabled)` → 32775 while transitional** — Shut down uses `Msvm_ShutdownComponent::InitiateShutdown` (keeps VM Running so Turn off works throughout); Restart on `RequestStateChange(Reboot=10)`.
29. **`KickPollAndWait` waits for a poll cycle that BEGAN AFTER the call** (req/serviced handshake, not a bare `m_pollGen` advance) — makes every cache-backed field fresh on dialog reopen. New `Set*` paths just call `KickPoll()`.

*Firmware / boot order / secure boot*
16. **`SecureBootTemplateId` must be a BARE GUID (no braces)** — braced form is a silent no-op.
17. **Gen 2 boot order: permute & write `BootSourceOrder` refs verbatim** (they regenerate after a reorder). Gen 1 = `BootOrder` `uint16[]` device-type codes, VM-Off-gated.
40. **Gen 2 boot entries are cryptic ("EFI SCSI Device" / "EFI Network") — correlate `Msvm_BootSourceSettingData` → its device** for friendly labels AND to find the DVD by KIND (string-matching the description fails — that's why a created VM didn't boot its install media). A boot source's `InstanceID` is its device RASD's `InstanceID` + `\B` (confirmed via the `Msvm_LogicalIdentity` association); `BootSourceType` 1=Drive (RT16=DVD / RT17=disk), 2=Network. The storage SASD `Parent` == the drive RASD `__PATH` (both FULL paths — `WmiObject::Path()` returns `__PATH`) so you can resolve the attached file. `CreateVM` promotes the DVD to boot-front when an install ISO is mounted (`PromoteDvdToBootFront`, kind-based).

*Storage (controllers / disks / DVD)*
18. **DVD mount=Add, change=Modify, eject=Remove** (empty drive has no media object; clearing `HostResource` does NOT eject). DVD *drive* add/remove mirrors disk-drive shape. `Remove*` wrappers are stubs — invoke manually.
23. **Attaching a VHD = two layered `AddResourceSettings`** (disk-drive RASD, then VHD SASD); re-query the new drive by (controller, slot); add-VHD step is eventually consistent (settle + poll + rollback orphan).
30. **A SCSI controller has no intrinsic index (number by enumeration order); adding one is Off-only** + capped at 4. Add/remove apply immediately; controller-centric UI with per-device placement flyouts.

*Network adapters*
21. **A "dynamic" NIC MAC is an EMPTY `Address`, not zeros** (`StaticMacAddress=false` + `Address=""`).
28. **Adding a NIC = TWO `AddResourceSettings` (port + connection)**; VLAN/advanced/bandwidth are feature settings on the *connection*. State gate `isOff || (Gen2 && Running)`. The "nothing changed → bail" guard in `OnPrimaryButtonClick` is a FIFTH edit site for any new field.

*Memory / processor / NUMA*
26. **`AutomaticShutdownAction` (stop) rejected while Running/Saved; `AutomaticStartupAction` (start) modifiable any state** — gate only `autoStopCombo` to Off. Start-delay is a CIM datetime interval string.
31. **`ProcessorSettingData.Reservation`/`Limit` are stored percent × 1000** — dialog scales ÷1000/×1000. Resource-control + NUMA fields need explicit fresh reads in `LoadFromVm` (not in poll cache).

*VM security (vTPM / encryption / shielding)*
27. **A valid key protector is a PREREQUISITE for ANY security change**; `GetKeyProtector` returns a 4-byte placeholder (not empty) when none set (test `size() >= 32`). KP gen lives in `root\Microsoft\Windows\Hgs`. Off + Gen 2 gated; dialog reads security LIVE.
32. **VM shielding is a COMPOSITE write** — `ShieldingRequested` alone is a silent no-op; enable also sets TPM + state-encryption. `Get-VMSecurity` lags raw writes >400 ms.

*Snapshots / checkpoints*
34. **`ApplySnapshot` needs VM Off/Saved (running → 32775; for Running/Paused we save→apply→resume)**; a snapshot's InstanceID is its OWN guid (caller must pass the VM guid). `Msvm_MostCurrentSnapshotInBranch` marks the "Now" position (rendered as a green ● child node).

*COM (serial) ports*
35. **A COM-port disconnect must write `Connection=[""]` (a one-element EMPTY STRING), NOT `[]` (an empty array)** — the empty-array `ModifyResourceSettings` is a silent no-op (`ret=4096`, value unchanged; verified by diffing the WMI DTD 2.0 XML: `<VALUE.ARRAY></VALUE.ARRAY>` is ignored, `<VALUE.ARRAY><VALUE></VALUE></VALUE.ARRAY>` clears it and reads back as `[]`). The silent-no-op family. Two more facts: the host named pipe binds at VM **power-on (cold start)**, not on the setting write — UARTs can't be hot-added and a guest reboot isn't enough; and **Gen 2 COM ports are debug-only** (NOT PnP devices in guest Device Manager or Hyper-V Manager), while Gen 1 exposes them normally. The kernel-debug port (`bcdedit /dbgsettings serial debugport:N`) is hidden from Device Manager — Windows reserves it for the debugger.

*Storage — physical disk (pass-through)*
36. **Enumerating offline host physical disks for pass-through requires hyprv to run ELEVATED (full admin)** — the **Hyper-V Administrators** group (enough for every other VM operation) is NOT enough. Non-elevated, `SELECT * FROM Msvm_DiskDrive` returns only the VM-attached synthetic drives (empty `DriveNumber`); the offline host disk (with a `DriveNumber`) is invisible, so the pass-through picker is empty. Pass-through itself = a single RT-17 `Microsoft:Hyper-V:Physical Disk Drive` RASD whose `HostResource` is the host `Msvm_DiskDrive` `__PATH` (no VHD `Msvm_StorageAllocationSettingData` layer); detach reuses `DetachVhd` with an empty `vhdRef`. The host disk must be **offline + fixed** — removable USB flash drives can't be offlined (`Set-Disk -IsOffline` → "Removable media") so they can't be passed through.

*VM creation (New VM wizard)*
38. **A raw `DefineSystem` VM ships BARE — no SCSI controller, no NIC, no DVD drive** (unlike the `New-VM` cmdlet, which adds them). So `VMManager::CreateVM` adds a SCSI controller for Gen 2 BEFORE `AttachVhd` (Gen 1 already has its two IDE controllers from DefineSystem), adds a NIC only if a switch is chosen, and adds a DVD only for an install ISO. `SystemSettings` is a spawned `Msvm_VirtualSystemSettingData` (`ElementName` + `VirtualSystemSubType` `Microsoft:Hyper-V:SubType:1|2`, `ResourceSettings`=empty, `ReferenceConfiguration`=null); the rest reuses the verified `SetMemoryConfig`/`SetProcessorConfig`/`AddScsiController`/`CreateAndAttachVhd`/`AddNetworkAdapter`/`AddDvdDrive`/`SetDvdMedia` helpers (so CreateVM runs on the UI/`m_scope` apartment). Setting `ConfigurationDataRoot`/`SnapshotDataRoot`/`SwapFileDataRoot` on the SystemSettings VSSD relocates the VM (Hyper-V auto-creates the dir) — Hyper-V Manager's "store the VM in a different location." Verified reversibly (DefineSystem → inspect default devices → destroy).

*WinUI styling*
37. **Stock-control chrome referenced via `{StaticResource}` in the control template CANNOT be compacted by an app-/page-level resource override** — StaticResource is resolved once at template-parse time against generic.xaml, so a local `<x:Double x:Key="...">` override is silently ignored. The `Expander` header (`ExpanderMinHeight`=48, `ExpanderHeaderPadding`) is StaticResource. Levers that DO work: set `MinHeight`/`Padding` as **local values on the instance** (a local value outranks the style setter, and the header `ToggleButton` template-binds `MinHeight`); the chevron (`ExpanderChevronButtonSize`=32, which floors the header height) IS a `{ThemeResource}`, so a scoped override shrinks it. General rule: grep generic.xaml for `{StaticResource}` vs `{ThemeResource}` before trying to restyle a stock control via resource overrides.
39. **A `ContentDialog` body taller than the default `ContentDialogMaxHeight` is CLIPPED, not scrolled** — like `ContentDialogMaxWidth`, override `ContentDialogMaxHeight` in the dialog's resources. Without it the dialog compresses the content area below the body's fixed-height Grid, the Grid overflows, and the bottom rows are unreachable even at max scroll (the New VM dialog at `1040×620` needed `ContentDialogMaxHeight=900`).

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
