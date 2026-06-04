---
name: release
description: Build and ship a hyprv Release as a signed MSIX bundle (x64 + ARM64) distributed via an .appinstaller on GitHub Releases. Use this when cutting a version, stamping the manifest/.rc versions, packaging, signing, or attaching release assets.
---

# Release / distribution

Debug = unpackaged. **Release ships as a signed MSIX bundle (x64 + ARM64) via an `.appinstaller`** on GitHub Releases (`github.com/jxy-s/hyprv`). One sequence:

```powershell
pwsh tools/set-version.ps1 1.0.0.2   # stamp manifest Identity + both .rc FILEVERSION/PRODUCTVERSION in lockstep
pwsh tools/make-msix.ps1             # clean -> per-arch -> makeappx bundle -> Trusted-sign -> emit .appinstaller; prints the gh release line
```

- **Framework-dependent** (self-contained is broken for this WinUI stack — gotcha #50). The runtime is a `PackageDependency`; the `.appinstaller` `<Dependencies>` auto-installs it (gotcha #51).
- **Azure Trusted Signing** — you `az login` (need the "Trusted Signing Certificate Profile Signer" role); the script signs against that session. **Never handle the maintainer's Azure creds — direct them to do it.**
- **Release assets** (attach all four, exact filenames): `hyprv.msixbundle`, `Microsoft.WindowsAppRuntime.2-x64.msix`, `Microsoft.WindowsAppRuntime.2-arm64.msix`, `hyprv.appinstaller`. Install = **download the `.appinstaller` and double-click** (the `ms-appinstaller:` web protocol is disabled by default).
- Packaging lives in `src/package/` (`hyprv-package.wapproj`, `Package.appxmanifest`, `hyprv.appinstaller.template`, `Assets/`). Gotchas #50–54.

## Where the pieces live

| Concern | File(s) |
|---|---|
| MSIX packaging (manifest, wapproj, assets) | `src/package/{Package.appxmanifest, hyprv-package.wapproj, Assets/}` |
| `.appinstaller` template (auto-update + runtime dep) | `src/package/hyprv.appinstaller.template` (→ `dist/hyprv.appinstaller`) |
| Build/sign the signed MSIX bundle release | `tools/make-msix.ps1` |
| Stamp version across manifest + both `.rc` | `tools/set-version.ps1 X.Y.Z.W` |
| EXE version metadata (Explorer Properties) | `src/app/hyprv.rc`, `src/rdphost/hyprv-rdphost.rc` (`VERSIONINFO`) |
| Settings + log path (real %LOCALAPPDATA% on MSIX) | `src/app/settings/Settings.cpp` (`LocalAppDataDir`) + `src/package/Package.appxmanifest` (`unvirtualizedResources`) |

See `docs/GOTCHAS.md` (MSIX / release / packaging / robustness section) for the full detail behind gotchas #50–54.
