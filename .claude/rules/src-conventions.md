---
paths:
  - "src/**"
---

# src/ — conventions + area index

Applies to all source. Per-area guidance lives in the sibling rules under `.claude/rules/`; each loads automatically when you open a file in its subtree (see the area index below). Deep detail → `docs/`; recipes → skills; gotcha detail → `docs/GOTCHAS.md` (never duplicated here).

## Coding conventions

- **C++20**, MSVC v143/v145 toolset.
- **C++/WinRT** for all XAML / WinRT calls. Never C++/CX.
- **`std::wstring`** at C++ boundaries; convert to `winrt::hstring` only at the XAML edge. **UTF-8 only on the IPC wire** (encode/decode at the boundary).
- **No exceptions across thread boundaries.** WMI calls go inside `try { ... } catch (hyprv::wmi::WmiException const& e) { HyprvAppLog(...); }` — canonical pattern in `vm/VMManager.cpp`.
- **All callbacks fire on the rx thread.** Anything touching XAML marshals via `m_uiQueue.TryEnqueue(...)`. Capture `weak_ref` (not `this`) in lambdas.
- **Comments explain *why*, not *what*.** Many subtle invariants are documented inline at the code site — read them.

## Project naming (subtle — do not "fix")

- Repo and EXE are both `hyprv`; main project `src/app/hyprv.vcxproj` has `<RootNamespace>hyprv_app` and `<TargetName>hyprv`. WinRT projection is `winrt::hyprv_app::`; internal C++ is `::hyprv::`.
- **Do not rename `hyprv_app` → `hyprv`.** Tried once and reverted — the `_app` suffix prevents a namespace collision that makes `::hyprv::` unreachable. Rationale: `docs/GOTCHAS.md` #59.

## Area index (concern → rule that covers it)

Each rule below loads automatically when you open a file under its path — you don't need to open them manually.

| Area | Rule / where |
|------|------|
| App lifecycle, MainWindow chrome, tabs, dialogs, welcome/settings pages, `ui/` helpers, theme, rdp **client**, logger | `.claude/rules/app.md` (`src/app/**`) |
| VM state / polling / connect status | `.claude/rules/vm.md` (`src/app/vm/**`) |
| WMI primitives + generated bindings | `.claude/rules/wmi.md` (`src/app/wmi/**`) |
| Persisted prefs, log path, real-`%LOCALAPPDATA%` | `.claude/rules/settings.md` (`src/app/settings/**`) |
| IPC server / mstscax host (child) | `.claude/rules/rdphost.md` (`src/rdphost/**`) |
| IPC wire contract (both processes) | `.claude/rules/shared-ipc.md` (`src/shared/**`) |
| MSIX packaging + `.appinstaller` | the `release` skill |
| Parallel-work file scoping | the `multi-agent` skill |
