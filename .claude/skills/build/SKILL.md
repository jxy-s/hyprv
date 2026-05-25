---
name: build
description: Build the hyprv solution from PowerShell using VS 2026 DevShell + msbuild. Use this when verifying that a code change compiles, or after touching anything under src/.
---

# Build hyprv

## Prerequisite (one-time per clone)

NuGet packages are gitignored. Before the first build of a fresh checkout, restore them:

```powershell
Import-Module 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Enter-VsDevShell -VsInstallPath 'C:\Program Files\Microsoft Visual Studio\18\Community' -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -no_logo' 2>$null | Out-Null
cd 'C:\path\to\hyprv'
msbuild 'hyprv.slnx' '-t:Restore' '-p:RestorePackagesConfig=true' '-p:Configuration=Debug' '-p:Platform=x64' '-v:minimal' '-nologo'
```

Note `-p:RestorePackagesConfig=true` — these projects use packages.config not PackageReference, so plain `-t:Restore` says "Nothing to do".

If `packages/` already exists, skip this step.

## Full build

Chain DevShell + msbuild in a single PowerShell tool call (DevShell env only lives within the calling process):

```powershell
Import-Module 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Enter-VsDevShell -VsInstallPath 'C:\Program Files\Microsoft Visual Studio\18\Community' -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -no_logo' 2>$null | Out-Null
cd 'C:\path\to\hyprv'
msbuild 'hyprv.slnx' '-p:Configuration=Debug' '-p:Platform=x64' '-v:minimal' '-nologo'
```

Expected output ends with `hyprv.vcxproj -> C:\path\to\hyprv\bin\x64\Debug\hyprv.exe` and `hyprv-rdphost.vcxproj -> ...\hyprv-rdphost.exe`.

Build artifacts land at:
- `bin/x64/Debug/` — `hyprv.exe`, `hyprv-rdphost.exe`, plus `.pdb` / `.lib` / `.winmd` / runtime DLLs.
- `src/obj/<project>/x64/Debug/` — intermediate `.obj` / `.pch` / generated headers.

Both are gitignored.

## Why it has to be like this

- **VS 2026 specifically**: VS 2022 doesn't ship the v143 Windows Store toolset and bails with MSB8020. The "18" in the install path is VS's internal version (VS 2026 ships as MSVS 18.x).
- **DevShell required**: hyprv.vcxproj uses `<ApplicationType>Windows Store</ApplicationType>` with `<WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>`. Plain PowerShell hits MSB8036 because it can't resolve "10.0" to a concrete SDK version. DevShell loads the Store extension that does the resolution.
- **`hyprv.slnx` not `src/app/hyprv.vcxproj`**: the vcxproj sets `OutDir=$(SolutionDir)bin\$(Platform)\$(Configuration)\` and `IntDir=$(SolutionDir)src\obj\<project>\$(Platform)\$(Configuration)\`. Building the vcxproj directly makes `$(SolutionDir)` resolve to the project dir, so outputs land in `src/app/bin/...` instead of the repo root's `bin/`. Via slnx, `$(SolutionDir)` is the repo root and everything lands where intended.

## Fallback when the running app holds a binary lock

If `hyprv.exe` or `hyprv-rdphost.exe` is running, link fails with LNK1104 "cannot open file". Either close hyprv, or build just the unlocked project:

```powershell
msbuild 'src\app\hyprv.vcxproj' '-p:Configuration=Debug' '-p:Platform=x64' `
  '-p:SolutionDir=C:\path\to\hyprv\' `
  '-p:BuildProjectReferences=false' '-v:minimal' '-nologo'
```

`-p:SolutionDir=…` is critical — without it the output goes to the wrong `bin/`. `-p:BuildProjectReferences=false` avoids trying to relink the locked rdphost.

## Output to ignore

- `'vswhere.exe' is not recognized` from `Enter-VsDevShell` — harmless, DevShell still loads.
- `Processing WinMD …` lines — normal cppwinrt codegen.
- WinMD merge "saved output metadata" — normal.

## When you see compile errors

The actual MSVC errors are what matter. Clang LSP intellisense in the editor produces phantom diagnostics (STL1009 about experimental/coroutine, "VmTabPage.g.h not found", undeclared `Microsoft`/`Media`/`InitializeComponent`) — these are not real errors. Clang doesn't see the C++/WinRT-generated headers and can't consume MSVC's experimental coroutine headers. **Trust the MSBuild output, not the LSP.**

## After a structural change

Solution / vcxproj / packages.config edits sometimes need a `-t:Rebuild` to flush the up-to-date check. If a clean build behaves differently than incremental, that's the culprit.

## Clean slate

To wipe all build state without touching git-tracked files:

```powershell
Remove-Item -Recurse -Force C:\path\to\hyprv\bin, C:\path\to\hyprv\src\obj -ErrorAction SilentlyContinue
```

Then re-run the full-build block above. (`Generated Files` for MIDL/cppwinrt now lives under `src/obj/`, so this gets it too.)
