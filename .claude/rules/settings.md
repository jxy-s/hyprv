---
paths:
  - "src/app/settings/**"
---

# src/app/settings/ — persisted prefs + log path

| Concern | File(s) |
|---------|---------|
| Persisted prefs + JSON I/O | `Settings.{h,cpp}` |
| Settings + log path (real `%LOCALAPPDATA%` on MSIX) | `Settings.cpp` (`LocalAppDataDir`) + `src/package/Package.appxmanifest` (`unvirtualizedResources`) |

## Runtime data layout

- `%LOCALAPPDATA%\hyprv\settings.json` — user prefs (pretty-printed JSON, schema v1).
- `%LOCALAPPDATA%\hyprv\hyprv.log` — unified log. rdphost children forward lines via IPC; parent writes them prefixed `[rdphost <VMNAME> pid=N]`. Gated by `Settings::LoggingEnabled()` (default: on Debug, off Release).
- Both opened share-read so external tools can tail while hyprv runs.

**They live in the REAL `%LOCALAPPDATA%\hyprv` even under MSIX** (intentional persisted state, survives uninstall). Requires BOTH `SHGetKnownFolderPath(..., KF_FLAG_NO_PACKAGE_REDIRECTION)` AND the manifest `unvirtualizedResources` + `FileSystemWriteVirtualization` exclusion — either alone silently lands the file in the package LocalCache. **Gotcha #53.**
