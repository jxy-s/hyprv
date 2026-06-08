---
name: multi-agent
description: Sub-area file-scoping for running multiple agents in parallel on the hyprv repo without stepping on each other. Use this when coordinating parallel/concurrent work, claiming an in-flight area, or deciding which files an agent may touch.
---

# Multi-agent guidance

If running multiple agents in parallel on this repo, give each a clearly scoped sub-area to avoid stepping on each other. (The canonical "concern → file" map lives in the per-subtree `CLAUDE.md` files — start at `src/CLAUDE.md`'s area index → `src/app/CLAUDE.md` etc.; the list below is the parallel-work scoping on top of it.)

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
