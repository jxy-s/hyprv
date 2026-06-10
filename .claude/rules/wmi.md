---
paths:
  - "src/app/wmi/**"
---

# src/app/wmi/ — WMI primitives

**Trap zone.** Read the WMI section of `docs/GOTCHAS.md` before touching anything here. Hyper-V silently no-ops invalid `Modify*`/`Add*` writes — replicate the mutation reversibly (throwaway VM via `Invoke-CimMethod`, confirm with raw `Get-CimInstance`, restore) before coding it.

| Concern | File(s) |
|---------|---------|
| WMI primitives | `Wmi{Object,Scope,Subscription}.{h,cpp}` |
| Generated Hyper-V WMI bindings | `generated/HyperV.h` (regen via `wmi/gen/`) |

Callers wrap WMI in `try { ... } catch (hyprv::wmi::WmiException const& e) { HyprvAppLog(...); }` — see `src/app/vm/VMManager.cpp`. No exceptions across thread boundaries.
