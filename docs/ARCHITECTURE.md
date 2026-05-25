# hyprv architecture

This document is a deeper walkthrough than `CLAUDE.md`. Read CLAUDE.md first for the elevator pitch and file map; come here when you need to understand how something actually works.

## Two processes

### `hyprv.exe` — the WinUI 3 app

Owns:
- The single top-level Window + its HWND subclass.
- The title bar (custom, extending into the caption area Windows-Terminal-style).
- The tab strip (a single `TabView`, header-only — content is mounted into a separate `tabContentHost` `ContentControl`).
- The left rail (window-level VM browser).
- The right info flyout (window-level VM detail panel).
- One `VmTabPage` UserControl per open tab. Each owns one `RdpHostClient` + one spawned `hyprv-rdphost.exe`.
- `VMManager` (process-wide singleton) — WMI polling, state-change requests.
- `Settings` (process-wide singleton) — persisted prefs.
- `HyprvAppLog` — the unified file logger.

### `hyprv-rdphost.exe` — the out-of-process RDP host

One instance per open VM tab. Owns:
- A `WS_POPUP` top-level window (the "popup") that the parent re-parents via `GWLP_HWNDPARENT` so it tracks the parent's lifetime + alt-tab grouping.
- An `AtlAxWin140` child of the popup, hosting the `MsTscAx.MsTscAx.9` ActiveX (mstscax) — the actual RDP client.
- A named-pipe `IpcClient` chatting with the parent over `src/shared/RdpIpc.h`.
- A `DispSink` (`IDispatch` implementation) bound via `IConnectionPoint` to mstscax's `IMsTscAxEvents` so we receive `OnConnecting` / `OnConnected` / `OnLoginComplete` / `OnRemoteDesktopSizeChange` / `OnDisconnected`.

The popup is intentionally a top-level Win32 window, not a `WS_CHILD` of the WinUI XAML island. WinUI 3 renders via DirectComposition swap chains that paint over any same-process HWND children; making the rdphost popup an owned top-level dodges that Z-order trap. See `src/shared/RdpIpc.h` + `RdpHostClient::Impl::EmbedInto` for the cross-process re-parent dance.

## Process lifetime

### Spawning a tab

1. User double-clicks a VM in the rail (`MainWindow::CreateRailItem`'s `DoubleTapped` handler).
2. `MainWindow::OpenVmTab(guid, name)` constructs a `VmTabPage`, calls `Initialize(guid)`, `SetWindowHwnd(parentHwnd)`, builds a `TabViewItem`, attaches the VM context menu, stashes the `VmTabPage` projection in `TabViewItem.Tag`, appends it to `TabView.TabItems`, selects it.
3. `MainWindow::OnTabSelectionChanged` mounts the active `VmTabPage` into `tabContentHost.Content()`, calls `ShowPopup()`/`HidePopup()` on the right pages.
4. The mounted `VmTabPage`'s `Loaded` event fires → `OnLoaded()` → `UpdatePlaceholderAndClient()`. If the VM is `Running`, `StartConnection()` runs:
   - `RdpHostClient` ctor (no IPC yet)
   - `m_client->ConnectLocalVm(...)` →
     - `Impl::SpawnAndHandshake()`: creates the named pipe server, spawns `hyprv-rdphost.exe --pipe=<id>` with `CREATE_SUSPENDED`, assigns to the process-wide kill-on-job-close `JobObject`, `ResumeThread`, waits for `ConnectNamedPipe`, exchanges `Hello`/`HelloAck`, reads `HwndReady` (the child's popup HWND).
     - `Impl::EmbedInto(parentHwnd, sx, sy, w, h)`: cross-process `SetWindowLongPtr GWLP_HWNDPARENT` + `SetWindowPos`. Popup is now an owned top-level of the parent.
     - `Impl::SendP2C(P2C::ConnectLocalVm, ...)`: tells the child to call mstscax `Connect()`.
     - `Impl::StartRxLoop()`: async dispatcher of child events.
5. mstscax fires `OnConnected` → IPC `Connected` → `VmTabPage`'s `OnConnected` callback flips `m_connected = true`, calls `UpdateRdphostBounds()` (re-pin), calls `m_client->Show()` (cross-process `ShowWindow`).

### Tearing down a tab

1. User clicks ✕ on a tab → `MainWindow::OnTabCloseRequested`.
2. `HidePopup()` first (cross-process `ShowWindow(SW_HIDE)` to avoid a flash).
3. `impl->ShutdownClient()` — `RdpHostClient::Stop()` sends `P2C::Shutdown`, waits up to 2s, `TerminateProcess` as fallback.
4. Clear `tabContentHost.Content()` if this was the mounted page.
5. `item.Tag(nullptr)` to drop the TabView item-cache's strong ref to the `VmTabPage`.
6. `RemoveAt(idx)` on `TabView.TabItems`.

The job-object safety net: every spawned `hyprv-rdphost.exe` is in a process-wide `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` job. If the parent crashes or is `TerminateProcess`'d, the OS reaps all child rdphost processes. No orphans.

### Restoring a tab on launch (cold start)

`Settings.openTabs` is the persisted list of `{type, identifier}` entries (`welcome` / `vm` / `settings`). `MainWindow::OnActivated` invokes `RestoreOpenTabs` synchronously after attaching the window subclass. There are two timing traps that fall out of doing this on launch:

**Trap 1 — `VMManager::GetByGuid` returns `nullopt` for every VM tab during restore.** `OnActivated` runs on the UI thread; `VMManager`'s poll thread is a separate apartment and hasn't completed its first WMI snapshot yet. So `RestoreOpenTabs` can't use `GetByGuid` to fetch the VM's `elementName` for the tab header — every call returns null, and a naive "skip null" branch would silently drop every VM tab, then write the truncated list back to `settings.json` at the end of restore (data loss on every launch).

The fix is twofold:
- `RestoreOpenTabs` opens VM tabs unconditionally with an empty display name. `VmTabPage::UpdateVmTabHeader` resolves the header on the first `OnVmManagerChanged` tick after VMManager finishes its first poll.
- `VmTabPage::UpdatePlaceholderAndClient` uses a new `VMManager::HasFirstSnapshot()` accessor (returns `m_pollGen.load() > 0`) to disambiguate "VM not in cache YET" from "VM was deleted between sessions." When `!vmOpt && !HasFirstSnapshot()`, show the centered `loadingOverlay` (same visual as `WelcomePage::welcomeLoadingOverlay`) and DON'T spawn `rdphost`. Once VMManager polls, `OnVmManagerChanged` re-runs `UpdatePlaceholderAndClient` and we naturally fall through to the right branch (VM running → spawn; VM stopped → state placeholder; VM truly missing → `(missing VM)` placeholder).
- `UpdateVmTabHeader` has the matching three-way branch: real `elementName` → `Loading...` → `(missing VM)`.

**Trap 2 — Cold-start popup-rect race.** If `OnVmManagerChanged` ends up spawning `rdphost` for a restored VM tab that is currently inactive (page unmounted), `StartConnection`'s `ComputeBorderScreenRect` fails (Border has no measured size off-tree) and falls back to `(window_x, window_y+32, 1024x768)`. The popup gets shown at that fallback rect by `OnConnected`'s show path (well, only if `m_wantsVisible` is true — but `m_wantsVisible` may flip to true mid-flight). When the user later activates the tab, `MainWindow::OnTabSelectionChanged` mounts the page and calls `VmTabPage::ShowPopup` synchronously — WinUI's layout pass that gives Border its real size hasn't run yet in the same call stack, so calling `m_client->Show()` here would surface the popup at the stale fallback rect. Visible flash before the eventual reposition.

The fix is a `m_deferredShow` latch in `VmTabPage`:
- `ShowPopup` pins FIRST via `ComputeBorderScreenRect` + `UpdateRdphostBounds`, and only calls `m_client->Show()` on success. If the Border isn't measured yet, it sets `m_deferredShow = true` and leaves the popup hidden.
- `OnLoaded` (fires once after the first layout pass mounted this page) and `OnRdpHostSizeChanged` (fires when the Border goes `0 → real` on a subsequent re-mount) both check the latch: if `m_wantsVisible && m_connected && m_deferredShow`, do `UpdateRdphostBounds → Show → Focus → clear m_deferredShow`.
- `HidePopup` and `TearDownClient` also clear `m_deferredShow` so a stale latch can't surface a popup behind the user's back if they navigate away or the VM tears down mid-defer.

Touching `ShowPopup` requires also touching `OnLoaded`, `OnRdpHostSizeChanged`, `HidePopup`, and `TearDownClient` — they share the latch.

## XAML window layout

```
MainWindow (Window)
└── Grid (2 rows)
    ├── Row 0 (Height="Auto")  ← Tab strip
    │   └── TabView
    │       ├── TabStripHeader: hamburger button (toggles rail)
    │       ├── TabStripFooter: drag region + info button + settings + caption-reserve
    │       └── TabViewItems (per VM; .Content is empty — see Row 1)
    │
    └── Row 1 (Height="*")  ← Content area
        └── Grid (5 columns: rail | splitter | tabContentHost | splitter | flyout)
            ├── Col 0: leftRail Border (Virtual Machines + Remote Hosts Expanders)
            ├── Col 1: railSplitter (6 DIP, hover pill via railSplitterHint)
            ├── Col 2: tabContentHost (ContentControl — receives active page)
            ├── Col 3: flyoutSplitter
            └── Col 4: infoFlyout Border (Properties + Memory + Network + Storage + Notes + Performance + Snapshots Expanders)
```

Four tab content types live in `tabContentHost.Content` at different times:

- **`VmTabPage`** — per-VM session content. Minimal Grid: transparent `<Border x:Name="rdpHost">` (the cross-process popup paints over its screen rect) + a `<Grid x:Name="placeholderRoot">` overlay (collapsed by default) shown when the VM isn't connectable. `placeholderRoot.Background="Transparent"` so the window's backdrop bleeds through — the previous `ApplicationPageBackgroundThemeBrush` was opaque and theme-mismatched.
- **`RemoteHostTabPage`** — per-saved-host generic RDP session (see "Remote Hosts" below). A leaner sibling of `VmTabPage` sharing the same rdpHost popup / positioning / connect-error machinery, minus the Hyper-V state/poll/enhanced logic.
- **`WelcomePage`** — per-host portal (today, always the local host). Top bar (filter pill), Recents pills, sortable All-VMs table (with a right-aligned `New VM` header action), and a sortable Remote Hosts table below it (right-aligned `Add remote host` action). While we wait for the first VMManager snapshot, the table is collapsed and a centered `welcomeLoadingOverlay` (48 DIP ProgressRing + "Loading VMs…") is shown.
- **`AppSettingsPage`** — singleton tab (deduped via `MainWindow::OpenAppSettingsTab`). Left nav + search + form sections (General, Appearance, Confirmations, Logging). Instant-apply; restart-required banner for appearance changes that need a fresh process.

The rail and flyout are MainWindow-level so they're shared across tabs (one rail per window, not one per tab). Tab tear-away (shipped — gotchas #44–48) spawns a new MainWindow with its own rail/flyout; a moved `TabViewItem` re-parents intact (the page + its rdphost child survive) and `AdoptMovedTab` re-wires it to the new window. Tabs discriminate via `TabViewItem.Tag().try_as<...>()`; the per-tab-type loops in `MainWindow` (`OnTabSelectionChanged`, `CloseTabItem`, `OnVmManagerChanged`, `PersistOpenTabs`, `RestoreOpenTabs`, `Push/PopPopupSuppression`) all use this idiom — adding a tab type (as Remote Hosts did) means one branch in each. Multiple windows are kept alive by `ui/WindowManager` + made consistent by multicast `VMManager` callbacks; only the `m_isPrimary` window persists geometry/open-tabs.

## Remote Hosts (generic RDP)

Saved physical machines the user RDPs into directly (generic RDP on `:3389`) — distinct from remote *Hyper-V management* (a future Tier-3 feature). The whole point is that the existing rdphost / popup / focus / popup-suppression / tab-persistence / connect-error stack generalises with minimal new plumbing.

- **Connect path:** `RemoteHostTabPage` drives `RdpHostClient::Connect(server, domain, username)` → `P2C::Connect` → the child's `RdpHost::OnConnect`. `OnConnect` reuses the generic mstscax setup from `OnConnectLocalVm` (color/desktop/SmartSizing/KeyboardHook/audio/redirect/DPI) and drops the VMBus-console bits (PCB, `AuthenticationServiceClass`, port 2179, `NegotiateSecurityLayer=FALSE`, `EnableFrameBufferRedirection`). See gotcha #43.
- **Credentials = prompt every time.** NO password is stored or sent. `EnableCredSspSupport` + `PromptForCredsOnClient` make mstscax show its own NLA credential prompt; we deliberately don't pre-set `put_UserName`/`put_Domain` (that triggers a failed incomplete-credential attempt → "The logon attempt failed" before the prompt). Windows' own `TERMSRV/<host>` store remembers the username for later connects. The username rides a v3 wire field (`RdpOptions.userByteLen`).
- **Persistence:** a `remoteHosts` array in `settings.json` of `RemoteHost { name, address, username, domain, port, rdp }` (no password). Address is the case-insensitive key + the `OpenTab` identifier (`type="rdp"`). On launch a restored remote tab does NOT auto-connect — it shows an idle "Connect" placeholder (`OpenRemoteHostTab(addr, autoConnect=false)` from `RestoreOpenTabs`) so startup doesn't fire a credential prompt per saved host.
- **Surfaces:** the welcome-page Remote Hosts table (sortable, double-click to connect, single-click selects), the rail `remoteHostsExpander` (a `ListView` for native selection matching the VM rail), the add/edit `RemoteHostDialog`, the welcome RECENT row (remote opens ride the `RecentKind::Remote` discriminator), and the shared `VmTileFactory::BuildRemoteHostContextMenu` (Connect/Edit/Forget).

## IPC

See `docs/IPC.md` for the wire format. Briefly:
- One named pipe per child: `\\.\pipe\hyprv-rdp-<guid>`.
- 8-byte header (`type`, `payloadSize`) + variable payload.
- P2C (parent→child) values 0-127; C2P (child→parent) values 128-255.
- All structs in `src/shared/RdpIpc.h` are `#pragma pack(push, 1)` + `static_assert` on size to catch wire-protocol drift.

## Settings

`src/app/settings/Settings.{h,cpp}`. Process-wide singleton, lazy ctor reads `%LOCALAPPDATA%\hyprv\settings.json` via `Windows::Data::Json`, populates struct fields, spawns a background save thread.

Save flow:
- Setter mutates the in-memory state under `m_lock`, sets `m_dirty = true`, notifies `m_saveCv`.
- Save thread wakes on first dirty, waits up to 500ms (debounce — splitter drags coalesce into one disk write), then writes the file atomically via temp + `ReplaceFileW`.
- On shutdown the dtor signals + joins the thread + flushes any unflushed dirty.

JSON output is hand-formatted (not `Stringify()`) so the file is human-editable.

### Schema sections

- `diagnostics.loggingEnabled` — file-logger gate (default `true` in Debug, `false` in Release).
- `window` — position / size / rail width / flyout width / rail visible / flyout visible. `x`/`y` use `INT_MIN` as "never persisted" so we can tell first-launch from "persisted at origin".
- `openTabs` + `selectedTabIndex` — tab-strip restore. Tab types: `welcome` (identifier = host name, today always `local`), `vm` (identifier = VM GUID), `settings` (identifier = `app`).
- `recents` — MRU VM list (capped at 10), each entry is `{guid, lastOpened}`. Drives the welcome page Recents pills.
- `appearance` — backdrop (`mica` / `acrylic`), theme (`system` / `light` / `dark` / `black`), per-backdrop tint opacities (`micaTintOpacity`, `acrylicTintOpacity`, both 0..1).
- `confirmations` — sparse map of `{actionKey: bool}` overriding `DefaultConfirmationEnabled` per key. Only entries the user has explicitly toggled persist. Missing keys read from the defaults switch.
- `vmPrefs` — sparse map keyed by VM GUID. Each entry holds three optional fields: `enhancedSession` (the user's persisted enhanced-mode preference for this VM, default `true`), `enhancedSessionSupported` (sticky-true observation — recorded once we've ever seen the VM run with enhanced available; never written `false` so a transient "no LIS" report doesn't grey the toggle), and `rdpOptions` (per-VM Remote Desktop override; see RDP options section below).
- `rdpDefaults` — app-wide Remote Desktop defaults: audio mode (Redirect / PlayOnServer / None), redirection bools (clipboard, drives, devices, smart cards, ports, audio capture), color depth, initial desktop W/H (basic-session only), and display-scale override (`dpiScaleOverridePercent`, 0 = Auto). Applied to any VM whose `vmPrefs.<guid>.rdpOptions` is absent. See the dedicated RDP options section below for the load/apply story.

### Confirmations

Every destructive action funnels through `hyprv::app::ui::ConfirmAndAct(weakWindow, actionKey, title, body, primaryText, action)` in `src/app/ui/ConfirmDialog.{h,cpp}`. The helper:
1. Looks up `Settings::ConfirmationEnabled(actionKey)`. If `false`, runs `action` immediately (no dialog, no popup suppression) and logs the bypass.
2. Otherwise opens a `ContentDialog` with `DefaultButton=Close` (Enter dismisses) and `RequestedTheme = parent's ActualTheme()` (`ContentDialog`s in WinUI 3 don't inherit theme from the visual tree automatically).
3. Wraps the `ShowAsync` in a `PopupSuppressionScope` so the rdphost popup doesn't paint over the dialog.

Default policy in `Settings::DefaultConfirmationEnabled`: ON for irreversible / data-losing actions (`reset`, `turnOff`, `restart`, `shutdown`, `deleteVm`, `deleteSnapshot`, `deleteSnapshotSubtree`, `applySnapshot`, `revertToLastSnapshot`); OFF for reversible / no-work-lost actions (`save`, `pause`, `startResume`, `takeSnapshot`, `toggleEnhancedSession`). Unknown keys default `true` so a misspelled key surfaces as an extra dialog rather than a silent destructive action.

The App Settings page renders this as a flat checkbox list (no per-row hints, no toggle chrome). Labels and order match the right-click VM context menu so the cross-reference is unambiguous.

### Window geometry restore ordering

`MainWindow::OnActivated` must run its setup in this exact order:

1. `ResolveWindowHwnd()` — fetch the HWND via `IWindowNative`.
2. `ApplyPersistedGeometry()` — `SetWindowPos` to the rect loaded from `Settings`.
3. `AttachWindowSubclass()` — install the subclass that forwards `WM_WINDOWPOSCHANGED` to `PersistGeometry`.
4. `ExtendIntoTitleBar()` — Mica + title-bar customisation.

Reordering 2 and 3 is a silent footgun. `PersistGeometry` calls `Settings::SetWindowGeometry`, which **synchronously** rewrites `m_window` in-memory before the debounced save thread writes anything to disk. If the subclass is live during WinUI's startup layout pass, the first few `WM_WINDOWPOSCHANGED` events for the OS-default window placement get persisted on top of the cache we just loaded — and `ApplyPersistedGeometry` then "restores" to the OS-default values. Visible symptom: every launch the window comes up at the wrong spot, but `settings.json` *looks* correct because the user moves it back and the last move wins.

## Theme / appearance

`MainWindow::ApplyAppearance()` is the single owner. Called on launch from `OnActivated` and from `AppSettingsPage` whenever the user changes any appearance setting.

Three knobs in `Settings::AppearancePref()`:
- **Backdrop**: `Mica` or `Acrylic`. (A previous `None` option was removed — clearing `SystemBackdrop` left the root surface to show whatever brush WinUI picked and didn't theme cleanly.)
- **Theme**: `System` / `Light` / `Dark` / `Black`.
- **Tint opacity**: per-backdrop (`micaTintOpacity`, `acrylicTintOpacity`), 0..1.

### Why we manage backdrop *controllers* directly

The built-in `MicaBackdrop` / `DesktopAcrylicBackdrop` wrapper types don't expose `IsInputActive` (so Acrylic falls back to a solid fill when the window deactivates) and don't surface `TintOpacity` (no live intensity slider). `ApplyAppearance` tears down any prior `MicaController` / `DesktopAcrylicController` + `SystemBackdropConfiguration`, then creates a fresh pair for the current backdrop:

```cpp
m_backdropConfig = SystemBackdropConfiguration{};
m_backdropConfig.IsInputActive(true);                   // Acrylic stays vibrant on deactivation
m_backdropConfig.Theme(isDark ? Dark : Light);
m_acrylicController = DesktopAcrylicController{};
m_acrylicController.TintColor(tintColor);               // RGB(8,8,8) in Black mode
m_acrylicController.LuminosityOpacity(luminosity);      // 1.0 in Black, 0.96 dark, 0.85 light
m_acrylicController.TintOpacity(acrylicTintOpacity);    // user-controllable
m_acrylicController.SetSystemBackdropConfiguration(m_backdropConfig);
m_acrylicController.AddSystemBackdropTarget(target);
```

`UpdateBackdropTintOpacity(double)` is a fast path used by the slider's `ValueChanged` so a drag updates `TintOpacity` on the existing controller without a full teardown / rebuild.

### Black mode

Not a backdrop-off case. `useBlackTint = (theme == Black)` flips the tint to `RGB(8,8,8)` and pins `LuminosityOpacity=1.0` on the active controller — the backdrop is still active, just with a near-black tint and no wallpaper luminosity passthrough. Result: Black mode stacks with the user's intensity slider, so OLED-friendly opaque black is `TintOpacity=1.0` (still using the backdrop) without disabling it entirely. Pure 0,0,0 was tried and produces a flat-grey result because the acrylic blend math gives no luminosity to mix; near-black grey actually renders darker overall.

### Theme cascade

`RequestedTheme` on `Content()` cascades live for `{ThemeResource Foo}` references in Styles. Existing controls re-resolve on the next layout pass. **Already-resolved `StaticResource` lookups and code-cached brushes don't repaint** — this is why the AppSettingsPage shows a restart-required banner after appearance changes that need a fresh process for the full effect (especially backdrop swaps).

### Popup chrome

WinUI 3 popups don't inherit the window's `SystemBackdrop` automatically. Two categories:

- **`MenuFlyout` (and `FlyoutBase` subclasses)**: take a `SystemBackdrop` directly. Applied per-flyout via `hyprv::app::ui::PopupBackdrop::ApplyTo(menu)` (`src/app/ui/PopupBackdrop.{h,cpp}`) — `PopupBackdropFor()` returns a fresh `MicaBackdrop` or `DesktopAcrylicBackdrop` instance matching the user's choice (the built-in wrappers are fine here — we don't need `IsInputActive`/`TintOpacity` per-flyout). Set inside a `try`/`catch` since the property isn't available on every SDK build.
- **`ContentDialog` and `ComboBox` dropdowns**: don't expose `SystemBackdrop` in WindowsAppSDK 2.1 at all. They paint with theme brushes (`ContentDialogBackground`, `ComboBoxDropDownBackground`). In Black mode we override those keys to opaque near-black (RGB 8,8,8) via `Application.Current.Resources.Insert` so dialogs and dropdowns match the window's near-black look. In other themes the framework defaults apply.

On top of the menu's backdrop, in Dark + Black modes we also paint a translucent `MenuFlyoutPresenterBackground` overlay (alpha 0xA0 over the dark/black tint) so the menu chrome reads consistent with the window's tint instead of looking like a flat block over Mica. In Light / System (resolves-light) modes we clear the override and the framework default applies.

### Caption buttons

The OS draws min/max/close — XAML doesn't reach those pixels. `AppWindow.TitleBar.{ButtonForegroundColor, ButtonHoverBackgroundColor, …}` is set per-theme so the glyphs remain legible against the window's actual tint.

### Restart-required banner

Some appearance changes only fully apply on a fresh process — backdrop swaps + theme cascades through cached `StaticResource` lookups. `AppSettingsPage::SetRestartRequired(true)` arms an InfoBar with a "Restart now" action button. The button launches a new `hyprv.exe` and calls `Application::Current().Exit()`. Without the banner, the user has to figure out to relaunch on their own.

## Logging

`HyprvAppLog` (defined in `src/app/MainWindow.xaml.cpp`) is the single sink. Gated by `Settings::LoggingEnabled()`. Opens `%LOCALAPPDATA%\hyprv\hyprv.log` lazily via `_wfsopen(..., L"w", _SH_DENYWR)` so external readers can tail it.

The rdphost child has its own `HyprvLog` (in `src/rdphost/main.cpp`) that:
- Before handshake: routes to `OutputDebugStringW` (debugger only).
- After `HwndReady` + `StartReceiveLoop`: sends `C2P::LogLine` frames over IPC. The parent's `RdpHostClient::DispatchEvent` decodes the UTF-8 payload and calls `HyprvAppLog` with a `[rdphost <VMNAME> pid=N]` prefix. The VM label is set by `VmTabPage::StartConnection` via `RdpHostClient::SetLogLabel`.

End result: single unified log file with both processes' output interleaved, identifiable by prefix.

## VM model + WMI

`src/app/vm/VirtualMachine.h` is the in-memory shape. `VMManager` polls Hyper-V every second from a background thread (using its own COM apartment — main-thread proxies don't survive cross-apartment calls):
- `Msvm_SummaryInformation::GetSummaryInformation` — counters + snapshots
- `Msvm_ComputerSystem` — state, enhanced-mode availability
- `Msvm_VirtualSystemSettingData` — VM config + snapshot tree
- `Msvm_MemorySettingData` — dynamic-memory min/max
- `Msvm_GuestNetworkAdapterConfiguration` — guest IPs
- `Win32_PerfFormattedData_BalancerStats_HyperVDynamicMemoryVM` (root\cimv2) — memory pressure %
- ... and several more (see `VMManager::UpdateAll`)

WMI subscriptions on `Msvm_ComputerSystem` instance-change events nudge the poll thread for faster transitions (state changes don't have to wait the full 1s).

UI subscribes via `VMManager::SetOnChanged` — the callback fires on the WMI worker thread; consumers marshal to the UI dispatcher.

### Generated typed wrappers

`src/app/wmi/generated/HyperV.h` is the codegen output of `src/app/wmi/gen/gen-hyperv-h.ps1`. Each class listed in `gen/hyperv.json` becomes a strongly-typed struct deriving from `WmiObject`, with property accessors (`VirtualQuantity()`, `Reservation()`) and method wrappers (`ModifyResourceSettings(std::vector<std::wstring>)`). Regenerate by running the script in a PowerShell terminal connected to a host that has the schema installed (i.e. any Hyper-V-enabled Windows machine).

To add a class: add its `Msvm_*` name to `hyperv.json`, re-run `gen-hyperv-h.ps1`, commit the resulting `HyperV.h` diff.

### Edit path (writes)

`VMManager` exposes `RenameVM`, `SetNotes`, `SetMemoryConfig`, `SetProcessorConfig`, `SetIntegrationServiceEnabled`, etc. Each:

1. Resolves the target SettingData via `FindRealizedVssd` + `GetAssociated` (the Realized VSSD's `InstanceID` is `Microsoft:<guid>`).
2. Mutates fields via the generated typed setters.
3. Serializes to WMI DTD 2.0 XML via `WmiObject::GetCimXml()` (uses `IWbemObjectTextSrc` format ID 2 — what `Set-VMProcessor` sends on the wire; MOF text is rejected with CIM 32773).
4. Calls `Msvm_VirtualSystemManagementService::ModifySystemSettings` (for VSSD-level edits like Name/Notes and Firmware/Secure Boot) or `ModifyResourceSettings` (for associated SettingData like memory/processor/integration services).
5. Waits on the returned `Msvm_ConcreteJob` via `WaitForJob` if `ReturnValue == 4096` (async job).
6. On success, calls `KickPoll()` to wake the poller for an immediate cache refresh.

The settings dialog batches edits and calls `KickPollAndWait(15000)` once at the end so reopening the dialog reflects the new values rather than the pre-edit cached snapshot. The dialog itself runs the whole flow as a `fire_and_forget` coroutine with a `ContentDialogButtonClickEventArgs::GetDeferral()` keeping the dialog open and a "Saving..." `ProgressRing` overlay animating during the cache wait (the wait happens on a thread-pool thread so the UI dispatcher can paint the spinner; the WMI Set calls themselves stay on the UI thread because the `IWbemServices` proxy is apartment-bound).

Edit operations are gated to strict `VmState::Off` for processor count and memory startup — Hyper-V silently rejects these on Saved VMs (the modify job completes with no error but no change applies). The same silent-no-op trap bit Secure Boot for a different reason: `VMManager::SetSecureBoot` (Firmware section, Gen 2 only, VM-Off gated) writes `Msvm_VirtualSystemSettingData.SecureBootEnabled` + `SecureBootTemplateId`, and **the template must be a bare GUID** (`1734c6e8-…`, no braces). A braced value makes the whole `ModifySystemSettings` return `4096` with a clean job that applies nothing — toggle "saves" but Secure Boot never changes. `SetSecureBoot` strips braces at the write boundary; see CLAUDE.md gotcha #16.

DVD media (Storage section) is the one edit path that uses all three resource verbs: `VMManager::SetDvdMedia` mounts an ISO with `AddResourceSettings` (cloning the default `Virtual CD/DVD Disk` template), changes one with `ModifyResourceSettings`, and ejects with `RemoveResourceSettings`. Because an empty drive has no media object, mount=Add / change=Modify / eject=Remove (clearing `HostResource` is rejected). The generated `Remove*` wrappers are still ReferenceArray TODO stubs, so eject invokes the method by hand via `WmiObject::SetReferenceArray` (a SAFEARRAY-of-BSTR object-path setter added for exactly this). That `SetReferenceArray` + manual-invoke pattern is the template for the still-unwired NIC add/remove. See CLAUDE.md gotcha #18.

### Per-VM preferences (out-of-band of WMI)

Some bits of per-VM state don't live in Hyper-V at all — they're hyprv's own preferences for how to treat the VM. Today there are two: the user's enhanced-session preference (opt-out of enhanced mode for a specific VM even when Hyper-V reports it as available), and the per-VM RDP options override.

Lives in `Settings::m_vmPrefs` (sparse map keyed by VM GUID). Three optional fields per entry:
- `enhancedSession` — the user's persisted ON/OFF choice. Default `true`. `StartConnection` uses `enhancedSessionAvailable AND enhancedSession`, so pref-on for a guest that can't do enhanced is a no-op rather than a broken session.
- `enhancedSessionSupported` — sticky-true observation. Recorded once we've ever seen the VM run with enhanced available. Lets the context menu correctly gate the toggle when the VM is currently off (Hyper-V only reports `EnhancedSessionModeState` while the VM is running with LIS up).
- `rdpOptions` — per-VM override of the app-wide `rdpDefaults`. Absent (nullopt) means "use app defaults"; present means the user has explicitly customized this VM and we persist the full snapshot. See the dedicated RDP options section below.

`MainWindow::OnVmManagerChanged` iterates the latest snapshot and calls `Settings::ObserveEnhancedSupport(guid)` for each VM that currently reports `enhancedSessionAvailable=true`. The observation is sticky-true: we never write a `false` from this path, so a transient "no LIS at boot screen" snapshot doesn't grey out the toggle for a capable VM.

### RDP options: a four-touchpoint pipeline

User-facing Remote Desktop knobs flow through four coordinated pieces of code; editing one without the others produces a "settings save fine, but nothing happens" bug. Adding a new knob requires all four edits.

1. **Storage** — `Settings::RdpOptions` struct + `RdpDefaults()` / `SetRdpDefaults()` + per-VM `SetRdpOptionsOverride(guid)` / `ClearRdpOptionsOverride(guid)`. Hot accessor `RdpOptionsFor(guid)` returns override-if-present, defaults otherwise. JSON load (`RdpOptionsFromJson`) + JSON save (the `writeRdpOptsBody` closure in `SaveLocked`) — keep the field list in lock-step across both. The Settings struct uses its own `AudioMode` enum deliberately separate from `hyprv::ipc::AudioMode` so Settings.h doesn't pull in `RdpIpc.h`; the values are aligned so the wire cast is a no-op.
2. **UI surfaces** — App Settings "Remote Desktop" section (`AppSettingsPage.xaml.cpp`, code-built section using the existing `MakeComboRow` / `MakeCheckRow` helpers, instant-apply on every control change) and VmSettingsDialog "Remote Desktop" section (`VmSettingsDialog.xaml` for the XAML controls, `LoadFromVm` / `OnPrimaryButtonClick` for the diff-against-snapshot apply with "Use app defaults" toggle).
3. **Wire prep** — `VmTabPage::StartConnection` calls `Settings::RdpOptionsFor(m_vmGuid)` and copies the values into `hyprv::ipc::RdpOptions` for the `ConnectLocalVm` IPC frame. Redirection bools OR into `opts.flags` via the `RdpFlags` bitmask; audio mode casts through to `opts.audioMode`; numeric fields pass straight.
4. **Wire apply** — `RdpHost::OnConnectLocalVm` (in `src/rdphost/RdpHost.cpp`) calls the matching `IMsRdpClientAdvancedSettings::put_*` methods on the mstscax COM proxy. Audio mode → `put_AudioRedirectionMode(UINT)`; clipboard / drives / printers / ports / smart cards → matching `put_Redirect*(VARIANT_BOOL)`; audio capture → `adv7->put_AudioCaptureRedirectionMode(VARIANT_BOOL)`. **Without this step, the wire field is silently dropped** — we shipped per-VM RDP options once and they were observably useless because only `colorDepth` / `desktopWidth` / `desktopHeight` had `put_*` calls.

Effective-options resolution is a single `RdpOptionsFor(guid)` lookup; the apply order is wire-then-`put_*`. Live sessions don't auto-reconnect on Settings changes — the new values land on the next `Connect()`. Both UI intros document the "close and reopen to refresh" workflow.

**Two knobs break the simple four-touchpoint shape (v13):**
- **Display scale** (`dpiScaleOverridePercent`, 0 = Auto = follow host DPI) is applied in a *fifth* place beyond connect: `VmTabPage::UpdateRdphostBounds`' enhanced fit-to-window resize passes a DPI to `UpdateSessionDisplaySettings` on every window resize, which would otherwise snap the guest back to host DPI. `VmTabPage` caches `m_dpiOverridePercent` and honors it in both `StartConnection` and the resize path. A scale-ish knob wired only at connect appears to work until the first resize.
- **Initial size** (`initialDesktopWidth/Height`) only affects *basic* sessions — enhanced sessions renegotiate to fit the tab window — so it's surfaced with an "applies to basic sessions" hint and needs no resize-path work. See CLAUDE.md gotcha #15.

### Context menu state gating

`hyprv::app::ui::BuildVmContextMenu` derives a single VM-state snapshot at the top and passes per-item `enabled` flags through the `addItem` helper. Mapping reflects what Hyper-V will actually accept (or what the operation does):

- Power: `Start/Resume` only from stable not-running (Off / Saved / Paused); `Pause` / `Save` / `Restart` / `Shut down` from Running; `Reset` / `Turn off` from Running OR Paused; `Delete VM` from Off only.
- Session: `Enhanced session` toggle enabled when `supports = (running ? enhancedSessionAvailable : EnhancedSessionEverSupported)`. `Send Ctrl+Alt+Del` / `Type clipboard` enabled when Running AND NOT in enhanced mode — an enhanced RDP session intercepts the synthesized `Msvm_Keyboard` input on its way through, so only basic mode passes it to the guest.
- Snapshots: `Take snapshot` enabled when VM exists + not transitioning; `Revert to last snapshot` additionally requires `!snapshots.empty()`.

The toggle item's `IsChecked` AND the send-keys items' `IsEnabled` get refreshed in `menu.Opening` so a cached `ContextFlyout` instance stays in sync with `Settings` and live state on every right-click (state-gated power items ride on the ~1 s VMManager poll and don't need this path).

## Threading rules

- **UI thread**: XAML touches only. Never block.
- **WMI poll thread**: lives in `VMManager`. Its own MTA COM apartment. Don't share WmiScope proxies across threads.
- **RdpHostClient rx thread**: per-client. All IPC callbacks fire here. Marshal to UI via `m_uiQueue.TryEnqueue` before touching XAML or `m_client` state from the UI's perspective.
- **Save thread (Settings)**: blocks on a condvar, wakes for debounced writes.

Lambdas that need to outlive their setup capture `winrt::weak_ref` (not `this`) and resolve inside the marshaled UI dispatch.

## Recent topology decisions (DO NOT regress without discussion)

- **Out-of-process rdphost** is the whole reason this project exists. Don't collapse mstscax back in-process.
- **Rail + flyout are MainWindow-level**, not per-tab. This is what makes tab tear-away (shipped) clean — each torn window gets its own rail/flyout for free.
- **AtlAxWin starts at the popup's current client size**, not a hardcoded value. The original 800×600 hardcode caused mstscax to bake in 800×600 as the session area regardless of how big the popup was — manifested as "wonky offset" credential UI.
- **Popup follows mstscax-reported size via `OnRemoteDesktopSizeChange`**. Don't guard out `OnDesktopResized` events — we want them to drive the popup pin.
- **Settings + log live in `%LOCALAPPDATA%\hyprv\`**, not next to the .exe. MSIX install dirs are read-only.
- **App Settings is a tab, not a dialog.** Instant-apply on every control; appearance changes that need a relaunch arm a restart banner. Settings tab is deduped (singleton); closing it as the last tab exits the program (same as the last welcome tab).
- **Backdrop controllers, not `*Backdrop` wrappers.** `MicaController` / `DesktopAcrylicController` give us `IsInputActive=true` (Acrylic stays vibrant on deactivation) and `TintOpacity` for the intensity slider. Don't reach back for `MicaBackdrop` / `DesktopAcrylicBackdrop`.
- **Black mode = pure-black tint on the active controller**, not a backdrop-off case. `TintColor=RGB(8,8,8) + LuminosityOpacity=1.0`. Stacks with the user's intensity slider.
- **Single confirmation entry point.** All destructive actions go through `hyprv::app::ui::ConfirmAndAct`. Adding a new destructive action MUST add the key to `DefaultConfirmationEnabled` (Settings.cpp) AND the `kConfirmEntries` table (AppSettingsPage.xaml.cpp).
- **Per-VM `vmPrefs` are sparse.** Only VMs the user has touched persist. `EnhancedSessionEverSupported` is sticky-true (positive observation only) so transient "no LIS" snapshots don't grey the toggle.
- **Enhanced session pref acts as an opt-OUT**, not an override. `StartConnection` uses `enhancedSessionAvailable AND userPref`. Pref-on for a guest that can't do enhanced is a no-op.
- **VmTabPage placeholder background is `Transparent`**, not `ApplicationPageBackgroundThemeBrush`. The window's backdrop tint should show through so the placeholder reads continuous with the rest of the chrome in all themes.
- **Closing the last non-VM tab exits the program**, regardless of whether it's a welcome tab or the settings tab. VM tabs special-case: closing the last VM tab opens a welcome tab instead.

## Sibling project

`vmplex-ws` is the C#/WPF **VMPlex** predecessor — a useful reference for mstscax property setups, WMI query patterns, and VM lifecycle nuances. We've borrowed several non-obvious mstscax property settings from its `Rdp/RdpClient.cs` — `EnableFrameBufferRedirection`, `DesktopScaleFactor`, etc.
