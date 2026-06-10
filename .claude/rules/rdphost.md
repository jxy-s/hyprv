---
paths:
  - "src/rdphost/**"
---

# src/rdphost/ — IPC server / mstscax host (child process)

**Trap zone.** Read the rdphost/mstscax/IPC section of `docs/GOTCHAS.md` first; wire-protocol reference is `docs/IPC.md`. **Any change to `src/shared/RdpIpc.h` must be coordinated through a single agent — both this process and `src/app/` need a matching update.**

| Concern | File(s) |
|---------|---------|
| IPC server / mstscax host (child side) | `main.cpp`, `RdpHost.{h,cpp}` |
| IPC server transport | `IpcClient.{h,cpp}` |
| PerMonitorV2 DPI awareness (gotcha #49) | `app.manifest` |

Produces `hyprv-rdphost.exe`. Generic RDP (:3389) is `RdpHost::OnConnect`, not `OnConnectLocalVm`. Parent side (IPC client) is `src/app/rdp/RdpHostClient`.
