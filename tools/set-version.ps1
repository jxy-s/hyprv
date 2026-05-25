#Requires -Version 7
<#
.SYNOPSIS
    Stamp one version (X.Y.Z.W) across every place hyprv records it, in lockstep.

.DESCRIPTION
    A release version lives in three files; this keeps them consistent so the MSIX,
    the .appinstaller, and the EXE properties never drift apart:

      * src/package/Package.appxmanifest   <Identity Version="...">
          The source of truth. tools/make-msix.ps1 reads this and stamps it into the
          MSIX, the generated .appinstaller (root + MainPackage Version), and the
          default release tag (v<version>).
      * src/app/hyprv.rc, src/rdphost/hyprv-rdphost.rc
          FILEVERSION / PRODUCTVERSION (the X,Y,Z,W comma form) plus the FileVersion /
          ProductVersion strings (the X.Y.Z.W form) shown in Explorer > Properties >
          Details for hyprv.exe and hyprv-rdphost.exe.

    Not touched: the app.manifest assemblyIdentity version is an internal side-by-side
    identity, not a user-facing/product version, so it's intentionally left alone.

    Release flow:
      pwsh tools/set-version.ps1 1.0.0.1
      pwsh tools/make-msix.ps1
      gh release create v1.0.0.1 dist\hyprv.msix dist\Microsoft.WindowsAppRuntime.2.msix dist\hyprv.appinstaller --repo jxy-s/hyprv --generate-notes

.EXAMPLE
    pwsh tools/set-version.ps1 1.0.0.1
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')]   # MSIX requires the 4-part X.Y.Z.W form
    [string] $Version
)
$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$dots  = $Version                      # 1.0.0.1
$comma = $Version -replace '\.', ','   # 1,0,0,1  (the .rc FILEVERSION / PRODUCTVERSION form)

# Each rule's regex brackets the version literal in capture groups so ONLY the version
# changes. Every pattern is expected to match exactly once in its file (a miss throws,
# so a renamed/reformatted field can't silently leave a stale version behind).
$rcRules = @(
    @{ Re = '(FILEVERSION\s+)[\d,]+';                    To = "`${1}$comma" }
    @{ Re = '(PRODUCTVERSION\s+)[\d,]+';                 To = "`${1}$comma" }
    @{ Re = '(VALUE\s+"FileVersion",\s*")[\d.]+(")';     To = "`${1}$dots`${2}" }
    @{ Re = '(VALUE\s+"ProductVersion",\s*")[\d.]+(")';  To = "`${1}$dots`${2}" }
)
$files = @(
    @{ File = 'src\package\Package.appxmanifest'; Rules = @(
        # Anchored on <Identity ...> so PackageDependency / TargetDeviceFamily versions are untouched.
        @{ Re = '(<Identity\b[^>]*?\bVersion=")[\d.]+(")'; To = "`${1}$dots`${2}" }
    )}
    @{ File = 'src\app\hyprv.rc';            Rules = $rcRules }
    @{ File = 'src\rdphost\hyprv-rdphost.rc'; Rules = $rcRules }
)

Write-Host "Stamping version $dots ..." -ForegroundColor Cyan
foreach ($f in $files) {
    $full = Join-Path $RepoRoot $f.File
    if (-not (Test-Path $full)) { throw "missing file: $($f.File)" }

    # Preserve the file's existing BOM state + encoding (manifest may have a BOM; .rc don't).
    $bytes  = [System.IO.File]::ReadAllBytes($full)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $text   = [System.IO.File]::ReadAllText($full)
    $orig   = $text
    foreach ($r in $f.Rules) {
        if (-not [regex]::IsMatch($text, $r.Re)) { throw "version pattern not found in $($f.File): $($r.Re)" }
        $text = [regex]::Replace($text, $r.Re, $r.To)
    }
    if ($text -ne $orig) {
        [System.IO.File]::WriteAllText($full, $text, (New-Object System.Text.UTF8Encoding($hasBom)))
        Write-Host "  updated  $($f.File)" -ForegroundColor Green
    } else {
        Write-Host "  same     $($f.File)" -ForegroundColor DarkGray
    }
}
Write-Host "Done. Next: pwsh tools/make-msix.ps1  then  gh release create v$dots ..." -ForegroundColor Cyan
