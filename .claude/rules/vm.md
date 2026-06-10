---
paths:
  - "src/app/vm/**"
---

# src/app/vm/ — VM state

**Trap zone.** Before any state-change or `Modify*`/`Add*` WMI write, read the WMI section of `docs/GOTCHAS.md` and verify reversibly first — Hyper-V silently no-ops invalid writes. WMI primitives live in `src/app/wmi/`.

| Concern | File(s) |
|---------|---------|
| VM state, polling, state-change requests | `VMManager.{h,cpp}`, `VirtualMachine.h` |
| Hyper-V connect status / poll / connect-error UI | `VMManager.cpp` (`TryConnect`/`ConnectStatus`/`PollLoop`) + `src/app/WelcomePage.xaml.cpp` (connect-error panel + `OnRetryConnectClick`) |
| VM creation | `VMManager::CreateVM` (+ `src/app/NewVmDialog`) |

Canonical try/catch pattern (`WmiException` → `HyprvAppLog`, no exceptions across threads) lives in `VMManager.cpp`. `VMManager` is multicast-subscribed by every window — coordinate edits with tab tear-away / multi-window owners (gotchas #44–48).
