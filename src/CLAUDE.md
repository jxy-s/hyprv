# src/ — conventions + area index

Applies to all source. Per-area navigation lives in the nested `CLAUDE.md` files below (they load when you open those subtrees). Deep detail → `docs/`; recipes → skills; gotcha detail → `docs/GOTCHAS.md` (never duplicated here).

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

## Area index (concern → where it's documented)

| Area | Where |
|------|-------|
| App lifecycle, MainWindow chrome, tabs, dialogs, welcome/settings pages, `ui/` helpers, theme, rdp **client**, logger | `src/app/CLAUDE.md` |
| VM state / polling / connect status | `src/app/vm/CLAUDE.md` |
| WMI primitives + generated bindings | `src/app/wmi/CLAUDE.md` |
| Persisted prefs, log path, real-`%LOCALAPPDATA%` | `src/app/settings/CLAUDE.md` |
| IPC server / mstscax host (child) | `src/rdphost/CLAUDE.md` |
| IPC wire contract (both processes) | `src/shared/CLAUDE.md` |
| MSIX packaging + `.appinstaller` | the `release` skill |
| Parallel-work file scoping | the `multi-agent` skill |
