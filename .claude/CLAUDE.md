# hyprv — Claude project briefing

Read this before touching anything. It's deliberately small: src-specific navigation and conventions live in path-scoped rules under `.claude/rules/` that load lazily when you open the matching subtree (`.claude/rules/src-conventions.md` is the area index). Deep detail lives in `docs/`; recipes live in skills.

## What hyprv is

A Hyper-V VM client for Windows. C++/WinRT + WinUI 3. From-scratch rewrite of the older VMPlex (C#/WPF, sibling repo). Debug builds are unpackaged; Release ships as a signed MSIX bundle (see the `release` skill). Full mental model + diagrams: `docs/ARCHITECTURE.md` (wire protocol: `docs/IPC.md`). Two architectural points drive most of the design:

- **Out-of-process RDP host (`hyprv-rdphost.exe`).** VMPlex hosted the leaky `mstscax` ActiveX control in-process; closing a tab leaked handles+memory until the process died. hyprv spawns one child per VM tab to host mstscax over named-pipe IPC; closing a tab tears down the child so the parent's working set stays clean. Wire contract: `src/shared/RdpIpc.h`.
- **Multi-window (tab tear-away).** A tab can be dragged out into its own `MainWindow` or onto another window's strip. A strong-ref `ui/WindowManager` registry keeps torn windows alive; the moved `TabViewItem` (+ its live `RdpHostClient` + rdphost child) re-parents intact via `AdoptMovedTab`. One window is `m_isPrimary` and persists geometry + open tabs. Gotchas #44–48.

## Repository layout

All C++ source is under `src/`: `src/app/` = main WinUI 3 app (`hyprv.exe`); `src/rdphost/` = out-of-process mstscax host (`hyprv-rdphost.exe`); `src/shared/RdpIpc.h` = the IPC wire contract shared by both. Intermediates → `src/obj/`, final binaries → `bin/` (both gitignored). For where a concern lives, see `.claude/rules/src-conventions.md` (area index) → the per-area rules, and the `multi-agent` skill for parallel-work scoping.

## Build / release

Build + validate via the `build` and `verify` skills (VS 2026 DevShell + `msbuild hyprv.slnx` — never the vcxproj directly). Release (signed MSIX x64+ARM64 via `.appinstaller`, Azure Trusted Signing) via the `release` skill. Gotchas #50–54.

## Hard-won gotchas — two strict rules

**`docs/GOTCHAS.md` is the single source of truth** for every subtle, expensive-to-rediscover invariant (rdphost/mstscax/IPC, WMI, tab tear-away, theme, MSIX, naming, …), grouped by area and numbered.

1. **Consult — ALWAYS.** Before touching any known-trap area (WMI `Modify*`/`Add*`, mstscax/RDP options, the rdphost popup or IPC wire format, tab drag/tear-away, multi-window, backdrop/theme chrome, ContentDialog/coroutine lifetime, boot order, storage/NIC/security/snapshot/serial-port edits, VM creation, MSIX packaging), read the matching `docs/GOTCHAS.md` section FIRST. It's also the first place to look for any "saves fine but nothing happens" silent-no-op.
2. **Record — ALWAYS into `docs/GOTCHAS.md`, NEVER here.** New invariant → next number in the right area section (and inline at the code site). Keep CLAUDE.md and the rules free of gotcha detail.

## See also

- `.claude/rules/src-conventions.md` — coding conventions, naming rule, area index into the per-area rules (all load lazily by path)
- `docs/{GOTCHAS,ARCHITECTURE,IPC}.md` — gotcha detail, component diagrams, wire protocol
- `.claude/PLAN.md` — roadmap index → `.claude/plans/` (active work, decisions, open questions; completed work archived under `plans/completed/`)
- `.claude/skills/{build,verify,release,multi-agent}/SKILL.md` — build, validation, release, parallel-work scoping
