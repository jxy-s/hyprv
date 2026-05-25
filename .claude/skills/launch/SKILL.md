# Launch hyprv

How to start `hyprv.exe` yourself after a build so you can hand the user
a "ready to test" instead of asking them to launch it. Pairs with the
[build skill](../build/SKILL.md) and [verify skill](../verify/SKILL.md).

## The recipe

```powershell
# 1. Make sure no prior instance is holding the binary lock.
Get-Process hyprv,hyprv-rdphost -ErrorAction SilentlyContinue | Stop-Process -Force

# 2. Truncate the log so the next run starts clean. (hyprv opens the
#    log with mode="w" on first write so this isn't strictly required,
#    but doing it explicitly means you can `Get-Content -Wait` from a
#    known empty starting point.)
Remove-Item "$env:LOCALAPPDATA\hyprv\hyprv.log" -ErrorAction SilentlyContinue

# 3. Launch detached. Start-Process returns immediately; the new
#    hyprv.exe stays up after PowerShell exits.
Start-Process -FilePath 'C:\path\to\hyprv\bin\x64\Debug\hyprv.exe'

# 4. Confirm it's actually up (the WinUI startup can fail silently if
#    a runtime DLL is missing or a XAML parse error happens — the
#    process exits with no popup).
Start-Sleep -Milliseconds 800
Get-Process hyprv -ErrorAction SilentlyContinue | Select-Object Id, StartTime
```

If step 4 returns nothing, the app died at startup — tail the log:

```powershell
Get-Content "$env:LOCALAPPDATA\hyprv\hyprv.log" -Tail 50
```

## Why this shape

- **Stop-Process before launch**: a running `hyprv.exe` holds the binary
  open, so building again gives LNK1104. Killing it first means the next
  build/launch cycle is one-shot. Stopping a not-running process is a
  no-op with `-ErrorAction SilentlyContinue`.
- **Detached launch**: do NOT use the Bash tool's `run_in_background`
  for the launch itself — the harness will tie the process lifetime to
  the bash session and you'll never get a useful handle. `Start-Process`
  in a foreground PowerShell call returns immediately and the app keeps
  running.
- **No sleep loop**: 800ms is enough for the main window to be created
  on this machine. Don't poll — if `Get-Process` finds it, that's
  proof of life.

## What to report to the user

After a successful launch say something like: "Built and launched
hyprv.exe (pid 12345). Reproduce the steps from <last bug> and I'll
read the log when you're back." Then stop and wait — don't tail the
log yourself, the user is driving.

If the build fails or the process dies on launch, surface BOTH the
last 40 lines of msbuild output AND the log tail — don't make the
user piece it together.

## When NOT to launch yourself

- The user already has hyprv open and is mid-test — killing their
  session is rude. Ask first.
- A feature is genuinely visual-only ("does the new icon look right?")
  — there's nothing to learn from a process check; just say "build is
  green, please launch and look."

## Pair with verify

Once it's running, the [verify skill](../verify/SKILL.md) covers what
the log will tell you and what genuinely needs the user to drive
through the UI.
