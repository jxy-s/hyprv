# hyprv — roadmap index

Read [`.claude/CLAUDE.md`](CLAUDE.md) first for the briefing. This file is the **index** into `.claude/plans/`; the detail lives in the linked files below. Keep this page short — when work lands, trim its entry to a line and push detail into the right plan file (see *Maintaining this* at the bottom).

## North star

Replace VMPlex with a from-scratch C++/WinRT WinUI 3 app that:

- Hosts mstscax out-of-process so closing a VM tab fully releases the leak.
- Supports multi-tab VM sessions side-by-side, including tear-away (drag tab → new window).
- Reaches feature parity with VMPlex + Hyper-V Manager (snapshots, settings editor, basic + enhanced sessions, remote hosts, new VM wizard).
- Ships as a signed MSIX.

## Status snapshot

Core platform ✅ · Welcome/session-restore ✅ · VM settings editor ✅ (v1–v23) · Theme/appearance ✅ · Confirmations ✅ · Per-VM enhanced session + RDP options ✅ · New VM wizard ✅ · VM debugger launch ✅ · Remote Hosts ✅ · High-DPI rdphost ✅ · Tab tear-away / multi-window ✅ · Auto-reconnect ✅ · Signed MSIX + `.appinstaller` release ✅

Full history: [`plans/completed/changelog.md`](plans/completed/changelog.md).

## Plans

| File | What's in it | When to read |
|------|--------------|--------------|
| [`plans/roadmap.md`](plans/roadmap.md) | Active + backlog work (Tier 2–4, App Settings future, parked stubs) | Before picking up new work |
| [`plans/open-questions.md`](plans/open-questions.md) | Decisions to make before starting an area | When that area comes up |
| [`plans/decisions.md`](plans/decisions.md) | Settled rationale — **don't second-guess these** | Before reworking an area |
| [`plans/completed/changelog.md`](plans/completed/changelog.md) | Archive of shipped work | History — you usually don't need it |

## Active focus

The live priorities in [`plans/roadmap.md`](plans/roadmap.md):

- **🔴 P0 — VM-settings correctness pass.** Always surface a save failure (no silent `ret=4096` no-ops); audit state-gating so off-only settings grey out while the VM runs. Affects trust.
- **⚠️ OPEN — WinUI backdrop shutdown-hang.** `SystemBackdropInternal::BaseController` teardown hangs during `DispatcherQueue` shutdown on some machines — not yet fixed; next step is disposing the Mica/Acrylic controllers on window `Closed`.
- Then: remaining niche P2 parity (VLAN trunk, legacy/emulated NIC, floppy, Fibre Channel), Tier 3 Remote Hyper-V, embedded PowerShell-Direct terminal, Virtual Switch Manager.

## Multi-agent coordination

Claim a sub-area **here** before starting concurrent work; clear it on completion. The intent is to avoid two agents editing the same XAML/cpp file at once.

```
In flight:
  (none)
```

The canonical sub-area → file scoping lives in the `multi-agent` skill (and the path-scoped rules under `.claude/rules/`). **Any change to `src/shared/RdpIpc.h` must be sequenced through a single agent** — a wire change needs matching updates in `src/app/rdp/RdpHostClient.cpp` AND `src/rdphost/main.cpp` (+ the header `static_assert`); other agents wait for it to land.

## Maintaining this

- **Work lands** → trim its roadmap entry to a one-liner and move the detail into [`plans/completed/changelog.md`](plans/completed/changelog.md); refresh the status snapshot above.
- **New settled call** → add it to [`plans/decisions.md`](plans/decisions.md).
- **New gotcha** → `docs/GOTCHAS.md` (next number, matching area) — **NEVER** here or in the plan files.
