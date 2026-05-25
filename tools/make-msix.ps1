#Requires -Version 7
<#
.SYNOPSIS
    Build + Trusted-Sign the hyprv MSIX bundle (x64 + ARM64) in one command.

.DESCRIPTION
    Produces a signed, installable framework-dependent MSIX bundle:
      0. Enters a VS 2026 DevShell (msbuild needs the Store/WinApp SDK toolset).
      1. (default) Wipes the Release build outputs + prior dist artifacts so the
         release can't inherit a stale file. Skip with -NoClean for fast iteration.
      2. For each architecture (x64, ARM64): builds hyprv-rdphost.exe then the
         packaging project, producing a per-arch .msix (each carries its own-arch
         rdphost; the packaging project pulls rdphost from bin\<arch>\).
      3. Bundles the per-arch packages into dist/hyprv.msixbundle (makeappx).
      4. Signs the BUNDLE once with Azure Trusted Signing (the signature covers every
         package inside via the block map).
      5. Emits dist/hyprv.appinstaller from src/package/hyprv.appinstaller.template -
         ONE descriptor whose <MainBundle> serves both arches (App Installer auto-picks
         the right build) - and stages BOTH Windows App Runtime framework packages
         (x64 + ARM64) as the declared dependencies. Then prints the publish command.

    RUNTIME: framework-dependent on Microsoft.WindowsAppRuntime.2 (>= 2.1.3). The
      .appinstaller declares both arch framework packages as dependencies and this
      script stages them (from the restored NuGet) as release assets, so installing
      via the .appinstaller auto-installs the matching runtime.

    PREREQS:
      * Visual Studio 2026 (v145 toolset) WITH the ARM64 build tools.
      * Signed in to Azure (`az login`) with the "Trusted Signing Certificate
        Profile Signer" role on the signing account.
      The Trusted Signing dlib auto-downloads to dist/.tools on first run.

.PARAMETER NoClean
    Skip the clean wipe and build incrementally (faster, for local iteration).
    Distribution builds should NOT use this.

.PARAMETER SkipSign
    Build + bundle only; produce an UNSIGNED .msixbundle.

.EXAMPLE
    pwsh tools/make-msix.ps1
        Clean build, sign, and publish the x64 + ARM64 bundle.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    # Trusted Signing config. These are the MAINTAINER's non-secret identifiers (no
    # credentials here - signing still requires your own `az login` plus the "Trusted
    # Signing Certificate Profile Signer" role on the account). Forking the repo?
    # Override these with your own Azure signing account, or use -SkipSign to build
    # an unsigned package.
    [string] $Endpoint    = 'https://cus.codesigning.azure.net/',
    [string] $Account     = 'jxy-s',
    [string] $CertProfile = 'Individual',
    # Release tag the .appinstaller's per-version asset URIs point at. Defaults to
    # "v<manifest-version>" (e.g. v1.0.0.0). Must match the `gh release create` tag.
    [string] $Tag,
    [switch] $NoClean,
    [switch] $SkipSign
)
$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$VsPath   = 'C:\Program Files\Microsoft Visual Studio\18\Community'
$Arches   = @('x64', 'ARM64')   # architectures combined into the bundle

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Note($m) { Write-Host "    $m" -ForegroundColor DarkGray }
function Good($m) { Write-Host "    $m" -ForegroundColor Green }

function Get-SdkBin {
    $bin = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory |
        Where-Object Name -like '10.*' | Sort-Object Name -Descending | Select-Object -First 1
    Join-Path $bin.FullName 'x64'
}

function Invoke-MsBuild($proj, $platform) {
    # Capture msbuild's (very chatty) per-file output; surface it only on failure.
    $log = & msbuild $proj "-p:Configuration=$Configuration" "-p:Platform=$platform" `
        "-p:SolutionDir=$RepoRoot\" '-v:quiet' '-nologo' 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | ForEach-Object { Write-Host $_ }
        throw "build failed: $proj ($platform) (exit $LASTEXITCODE)"
    }
}

# ---- 0. Enter the VS DevShell (gives msbuild the Store toolset) ----
Step 'Entering VS 2026 DevShell ...'
Import-Module (Join-Path $VsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
# DevShell's internal cmd probe shells out to `vswhere` and writes a raw "not
# recognized" line to the console (a grandchild write that *>$null can't catch) when
# it isn't on PATH. Put it on PATH so the probe stays silent; the rest of DevShell's
# chatter goes to $null. Env vars are a side effect, unaffected by the redirection.
$vsInstaller = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
if ((Test-Path $vsInstaller) -and ($env:PATH -notlike "*$vsInstaller*")) { $env:PATH = "$vsInstaller;$env:PATH" }
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64 -no_logo' *>$null
Set-Location $RepoRoot

# ---- 1. Clean (default) - no stale file can survive into the distributed bundle ----
if (-not $NoClean) {
    Step "Cleaning $Configuration outputs ..."
    $wipe = @( (Join-Path $RepoRoot 'dist\msix') )
    foreach ($a in $Arches) { $wipe += (Join-Path $RepoRoot "bin\$a\$Configuration") }
    Get-ChildItem (Join-Path $RepoRoot 'src\obj') -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        foreach ($a in $Arches) {
            $p = Join-Path $_.FullName "$a\$Configuration"
            if (Test-Path $p) { $wipe += $p }
        }
    }
    foreach ($p in $wipe) {
        if (Test-Path $p) { Remove-Item $p -Recurse -Force; Note "removed $($p.Substring($RepoRoot.Length + 1))" }
    }
    # Prior published artifacts (so a stale bundle / runtime / descriptor can't linger).
    Get-ChildItem (Join-Path $RepoRoot 'dist') -Filter 'hyprv*.msix*' -ErrorAction SilentlyContinue | Remove-Item -Force
    Remove-Item (Join-Path $RepoRoot 'dist\Microsoft.WindowsAppRuntime.2*.msix') -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $RepoRoot 'dist\hyprv.appinstaller') -Force -ErrorAction SilentlyContinue
} else {
    Note 'incremental build (-NoClean) - NOT for distribution'
}

# Version + publisher come from the package manifest (the single source of truth;
# bump with tools/set-version.ps1). Used for the bundle version, asset URIs, and tag.
$mtext  = Get-Content (Join-Path $RepoRoot 'src\package\Package.appxmanifest') -Raw
$verStr = [regex]::Match($mtext, '<Identity[\s\S]*?Version="([\d.]+)"').Groups[1].Value
$pub    = [regex]::Match($mtext, '<Identity[\s\S]*?Publisher="([^"]+)"').Groups[1].Value
if (-not $Tag) { $Tag = "v$verStr" }

# ---- 2. Build each architecture; stage its .msix for bundling ----
# The packaging project includes hyprv-rdphost.exe as <Content> from bin\$(Platform)\,
# so rdphost must be built for that arch first (it is NOT a ProjectReference - that
# would drop a duplicate copy in a subfolder).
$bundleSrc = Join-Path $RepoRoot 'dist\.bundle'
Remove-Item $bundleSrc -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $bundleSrc | Out-Null
foreach ($a in $Arches) {
    Step "Building $a (rdphost + package) ..."
    Invoke-MsBuild 'src\rdphost\hyprv-rdphost.vcxproj' $a
    Invoke-MsBuild 'src\package\hyprv-package.wapproj'  $a
    # The packaging project drops a single .msix under dist\msix\; grab the newest
    # (this arch's, just built) and stage it for the bundle.
    $m = Get-ChildItem (Join-Path $RepoRoot 'dist\msix') -Recurse -Filter '*.msix' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $m) { throw "no .msix produced for $a under dist\msix" }
    Copy-Item $m.FullName (Join-Path $bundleSrc "hyprv-$($a.ToLower()).msix") -Force
    Good "packed: $a"
}

# ---- 3. Bundle the per-arch packages -> dist/hyprv.msixbundle ----
Step 'Bundling -> hyprv.msixbundle ...'
$bundle = Join-Path $RepoRoot 'dist\hyprv.msixbundle'
$log = & (Join-Path (Get-SdkBin) 'makeappx.exe') bundle /d $bundleSrc /p $bundle /bv $verStr /o 2>&1
if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host $_ }; throw "makeappx bundle failed (exit $LASTEXITCODE)" }
Remove-Item $bundleSrc -Recurse -Force -ErrorAction SilentlyContinue
Good "bundled: hyprv.msixbundle (v$verStr, $($Arches -join ' + '))"

# ---- 4. Sign the bundle ONCE with Azure Trusted Signing ----
if (-not $SkipSign) {
    Step 'Signing with Azure Trusted Signing ...'
    $tools = Join-Path $RepoRoot 'dist\.tools'
    $dlib  = Get-ChildItem $tools -Recurse -Filter 'Azure.CodeSigning.Dlib.dll' -ErrorAction SilentlyContinue |
        Where-Object FullName -like '*x64*' | Select-Object -First 1 -ExpandProperty FullName
    if (-not $dlib) {
        Note 'fetching Trusted Signing client (Microsoft.Trusted.Signing.Client) ...'
        New-Item -ItemType Directory -Force -Path $tools | Out-Null
        $ver = '1.0.95'
        $nupkg = Join-Path $tools "tsc.$ver.zip"
        Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.trusted.signing.client/$ver/microsoft.trusted.signing.client.$ver.nupkg" -OutFile $nupkg
        Expand-Archive $nupkg (Join-Path $tools "tsc-$ver") -Force
        $dlib = Get-ChildItem (Join-Path $tools "tsc-$ver") -Recurse -Filter 'Azure.CodeSigning.Dlib.dll' |
            Where-Object FullName -like '*x64*' | Select-Object -First 1 -ExpandProperty FullName
    }
    $meta = Join-Path $tools 'trusted-signing-metadata.json'
    @{ Endpoint = $Endpoint; CodeSigningAccountName = $Account; CertificateProfileName = $CertProfile } |
        ConvertTo-Json | Set-Content $meta -Encoding utf8
    # az must be on PATH so DefaultAzureCredential can use your `az login` session.
    $azDir = 'C:\Program Files\Microsoft SDKs\Azure\CLI2\wbin'
    if ((Test-Path $azDir) -and ($env:PATH -notlike "*$azDir*")) { $env:PATH += ";$azDir" }
    # Capture the dlib's (chatty, multi-digest) output; only surface it on failure.
    $log = & (Join-Path (Get-SdkBin) 'signtool.exe') sign /v /fd SHA256 `
        /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib $dlib /dmdf $meta $bundle 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | ForEach-Object { Write-Host $_ }
        throw "signing failed (exit $LASTEXITCODE) - is ``az login`` done, with the 'Trusted Signing Certificate Profile Signer' role on '$Account'?"
    }
    Good 'signed (1 bundle - Trusted Signing, RFC-3161 timestamped)'
}

# ---- 5. Stage runtime deps + emit the .appinstaller ----
$rtMissing = @()
foreach ($a in $Arches) {
    $al = $a.ToLower()
    $fx = Get-ChildItem (Join-Path $RepoRoot 'packages') -Recurse -Filter 'Microsoft.WindowsAppRuntime.2.msix' -ErrorAction SilentlyContinue |
        Where-Object FullName -like "*\win10-$al\*" | Select-Object -First 1
    if ($fx) { Copy-Item $fx.FullName (Join-Path $RepoRoot "dist\Microsoft.WindowsAppRuntime.2-$al.msix") -Force }
    else { $rtMissing += $a }
}
$tpl = Get-Content (Join-Path $RepoRoot 'src\package\hyprv.appinstaller.template') -Raw
$ai  = $tpl.Replace('{VERSION}', $verStr).Replace('{TAG}', $Tag).Replace('{PUBLISHER}', $pub)
# Guard: no token may survive substitution (catches a renamed/typo'd token before release).
$leftover = [regex]::Matches($ai, '\{[A-Z]+\}') | ForEach-Object { $_.Value } | Sort-Object -Unique
if ($leftover) { throw "unsubstituted token(s) in hyprv.appinstaller.template: $($leftover -join ', ')" }
$aiOut = Join-Path $RepoRoot 'dist\hyprv.appinstaller'
# UTF-8, NO BOM, ASCII only - App Installer rejects a BOM / non-ASCII.
[System.IO.File]::WriteAllText($aiOut, $ai, (New-Object System.Text.UTF8Encoding($false)))

# ---- Report + release instructions ----
Write-Host ''
if ($SkipSign) { Step "Built (UNSIGNED): $bundle" } else { Step "Signed: $bundle" }
Write-Host ''
Step 'GitHub release - attach these assets (keep the exact filenames so the .appinstaller URIs resolve):'
Good '  - dist\hyprv.appinstaller                       (users download this and double-click it)'
Good '  - dist\hyprv.msixbundle                         (the signed x64 + ARM64 bundle)'
$rt = { param($a) "  - dist\Microsoft.WindowsAppRuntime.2-$a.msix   (Windows App Runtime $a; auto-installed by the .appinstaller)" }
foreach ($a in $Arches) {
    $al = $a.ToLower()
    if ($rtMissing -contains $a) {
        Write-Host "    - dist\Microsoft.WindowsAppRuntime.2-$al.msix   MISSING: restore packages, or $a users get no auto-runtime" -ForegroundColor Yellow
    } else {
        Good (& $rt $al)
    }
}
Write-Host ''
Step "Or publish them with the gh CLI (tag $Tag):"
Note "gh release create $Tag ``"
Note "  `"dist\hyprv.msixbundle`" `"dist\Microsoft.WindowsAppRuntime.2-x64.msix`" `"dist\Microsoft.WindowsAppRuntime.2-arm64.msix`" `"dist\hyprv.appinstaller`" ``"
Note "  --repo jxy-s/hyprv --title `"hyprv $verStr`" --generate-notes"
Note '(--prerelease for test builds, so they do not capture the latest/download URL.)'
Note 'Then users download hyprv.appinstaller from Releases and double-click it.'
