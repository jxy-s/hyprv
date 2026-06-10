---
paths:
  - "src/app/**"
---

# src/app/ — app/UI hot files

Canonical "concern → file" map for the WinUI 3 app. Conventions + naming live in `.claude/rules/src-conventions.md`; gotcha detail in `docs/GOTCHAS.md`. Tab tear-away / multi-window / WMI / IPC are trap zones — read the matching `docs/GOTCHAS.md` section first.

| Concern | File(s) |
|---------|---------|
| App lifecycle (`OnLaunched`); primary-window tracking | `App.xaml.cpp` |
| Title bar, tab strip, rail, info flyout | `MainWindow.xaml{,.h,.cpp}` |
| Per-VM tab content + rdphost client lifecycle | `VmTabPage.xaml{,.h,.cpp}` |
| Welcome (per-host portal) page | `WelcomePage.xaml{,.h,.cpp}` |
| App Settings (search + instant-apply tab) | `AppSettingsPage.xaml{,.h,.cpp}` |
| VM hardware settings editor (modal) | `VmSettingsDialog.xaml{,.h,.cpp}` |
| New VM wizard (modal) | `NewVmDialog.xaml{,.h,.cpp}` + `VMManager::CreateVM`; opened via `MainWindow::OpenNewVmDialog` |
| Remote host tab (generic RDP session) | `RemoteHostTabPage.xaml{,.h,.cpp}`; opened via `MainWindow::OpenRemoteHostTab` |
| Add/edit Remote Host dialog (modal) | `RemoteHostDialog.xaml{,.h,.cpp}`; opened via `MainWindow::OpenRemoteHostDialog` |
| Tab tear-away + cross-window drag (gotchas #44–48) | `MainWindow.xaml.cpp` (`OnTabDragStarting`/`OnTabViewDrop`/`OnWindowDrop`/`OnTabDragCompleted`/`WindowUnderPoint`/`MoveTabToThisWindow`/`SpawnWindowForTab`/`AdoptMovedTab`) + `MainWindow.xaml` (TabView + `rootGrid` + `titleBarTrailing` drop targets) |
| Multi-window registry (keeps torn windows alive) | `ui/WindowManager.{h,cpp}` (`Track`/`Find`/`All`) |
| Per-VM tile + context menu builder (shared) | `ui/VmTileFactory.{h,cpp}` (`BuildVmContextMenu`/`BuildRemoteHostContextMenu`) |
| Centralised confirmation dialog helper | `ui/ConfirmDialog.{h,cpp}` (`ConfirmAndAct`/`ConfirmAndActWithCheckbox`) |
| Popup `SystemBackdrop` helper for menus/flyouts | `ui/PopupBackdrop.{h,cpp}` |
| Theme / backdrop / restart-banner logic | `MainWindow.xaml.cpp` `ApplyAppearance`/`UpdateBackdropTintOpacity` + `App.xaml` |
| IPC client (parent side) | `rdp/RdpHostClient.{h,cpp}` — wire types in `src/shared/RdpIpc.h`, server in `src/rdphost/` |
| Shared logger | `HyprvAppLog` defined in `MainWindow.xaml.cpp` |
