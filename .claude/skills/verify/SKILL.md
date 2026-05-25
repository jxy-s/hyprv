---
name: verify
description: Validate a hyprv code change by running the built binary and inspecting its log. Use after build succeeds and the change is observable (UI behavior, IPC, settings persistence, VM action wiring).
---

# Verify a hyprv change

Building proves the code compiles. Verifying proves it does what you intended. For most changes you need the user to drive the UI — but a lot can be checked from the log file alone.

## When to use this skill

- After landing a UI behavior change (rail click flow, context menu wiring, popup positioning)
- After touching IPC (`src/shared/RdpIpc.h`, `RdpHostClient.cpp`, `src/rdphost/main.cpp` / `RdpHost.cpp`)
- After touching the settings layer or persisted state
- Anytime the build passed but you want evidence the runtime behavior matches expectations

Skip for:
- Pure refactors with identical behavior
- Doc-only or comment-only changes

## What you can verify without the user's hand on the wheel

The log file lives at `%LOCALAPPDATA%\hyprv\hyprv.log` and is opened with `_SH_DENYWR`, so you can `Get-Content -Wait` it while hyprv runs. Settings file is at `%LOCALAPPDATA%\hyprv\settings.json` and is atomic-write friendly — safe to `cat` at any time.

```powershell
# Tail the unified log live
Get-Content "$env:LOCALAPPDATA\hyprv\hyprv.log" -Wait -Tail 50

# Snapshot current persisted settings
Get-Content "$env:LOCALAPPDATA\hyprv\settings.json"
```

The log shows both processes interleaved. Child process lines are prefixed `[rdphost <VMNAME> pid=N]`. Look for:

- `[main] OnActivated` — app launched
- `[settings] loaded window pref: pos=(x,y) size=WxH rail=R flyout=F` — Settings cache contents at restore time
- `[settings] restored window rect: requested (x,y) WxH, GetWindowRect now (x',y') W'xH'` — the OS-accepted rect. Mismatch with `requested` means something is fighting our `SetWindowPos` (DPI awareness, shell snap layout, etc.) — investigate.
- `[tab] StartConnection vm=GUID` — a tab attempted to open
- `[hc] EmbedInto …` — parent positioned the rdphost popup
- `[rdphost <NAME> pid=N] [host] pre-Connect: popup=… ax=… desktopW/H=…` — confirms popup + AtlAxWin are matched in size
- `[cb] OnConnected …` — RDP session established
- `[bnd] UpdateRdphostBounds: …` — popup re-pinned (e.g. after `OnDesktopResized`)

If logging looks disabled (`Settings.diagnostics.loggingEnabled: false` in Release default), tell the user to flip it in `settings.json` OR set it via debug build.

## What needs the user

These have to be exercised through real UI:
- Mouse interactions (rail click, double-click, right-click flow, splitter drag)
- Multi-tab flow + tab tear-down
- VM state transitions (Start / Save / Pause / Stop) — these touch real Hyper-V
- Anything visual (tab strip cosmetics, flyout layout, placeholder)

In those cases, ask the user for the specific exercise and request both:
1. **Behavioral confirmation** ("did the popup fill the tab edge-to-edge?")
2. **The log file** so you can correlate timestamps and dimensions

## Don't fabricate verification

If you can't actually run the binary (running app holds the .exe lock, or the user is mid-test), say so. Reading the source and "looks right to me" is not verification.

A real verification report names the user-visible behavior, the log line that proves it, and any concerning sidebars (warnings, late retries, cache-misses where there shouldn't be).

## Clean up before re-testing

If the user's run was already polluted by an earlier broken build:

```powershell
# Force a fresh log on next launch
Remove-Item "$env:LOCALAPPDATA\hyprv\hyprv.log" -ErrorAction SilentlyContinue
# Settings is a real user preference — don't delete unless explicitly testing first-run
```

The log is opened with `mode="w"` on first write, so it's automatically truncated on each hyprv launch. Manual removal isn't usually needed.
