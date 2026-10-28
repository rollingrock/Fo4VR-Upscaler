# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
    Builds and packages a release from neutral drive letters, so the shipped DLLs and PDBs
    name no folder on the build machine.

.DESCRIPTION
    MSVC writes source, object and build paths into each PDB, and source paths into the
    DLL through __FILE__. vcpkg's static libraries carry the paths their ports were
    compiled under, and the linker names its temporary files in the PDB. This script
    therefore builds from drive letters mapped with subst:

      SourceDrive  the folder that holds this repository
      VcpkgDrive   the folder that holds the vcpkg root
      TempDrive    a new, empty folder, used as TEMP and TMP

    VCPKG_BINARY_SOURCES=clear makes vcpkg rebuild every port under the mapped root
    instead of restoring binaries compiled at another path. MSBuild node reuse is off, so
    no build node left over with the old environment takes part.

    The configuration is the release one: Visual Studio 17 2022, x64, the NGX backend on,
    FFX and Streamline off, no deployment, and CMake's default global compiler flags. No
    preset is used, so a local CMakeUserPresets.json cannot add a deployment path.
    package-release.ps1 then rebuilds the plugin clean from the committed HEAD and packages
    it, and split-release.ps1 writes the -mod.7z and the -docs.zip.

    Afterwards the plugin and OpenXR layer DLLs and PDBs are searched, as bytes in ASCII
    and UTF-16LE, for the real folders behind each mapping, the user profile and the
    account name as a path component. A hit fails the script. Every mapping is removed at
    the end, also on failure.

.PARAMETER BuildDirectoryName
    Build directory, relative to the repository root. Defaults to build-release. An
    existing one is reused only if it was configured from the same mapped path.

.PARAMETER OutputDirectory
    Where the archives are written. Defaults to <repo>/dist.

.PARAMETER VcpkgRoot
    The vcpkg root. Defaults to VCPKG_ROOT, then VCPKG_INSTALLATION_ROOT.

.PARAMETER SourceDrive
    Drive letter for the folder that holds this repository. Defaults to S.

.PARAMETER VcpkgDrive
    Drive letter for the folder that holds the vcpkg root. Defaults to V.

.PARAMETER TempDrive
    Drive letter for the temporary folder. Defaults to T.

.PARAMETER SkipPackage
    Configure, build and check the binaries, but do not package. The work tree may be
    dirty.
#>
[CmdletBinding()]
param(
    [string]$BuildDirectoryName = 'build-release',
    [string]$OutputDirectory,
    [string]$VcpkgRoot,
    [ValidatePattern('^[D-Zd-z]$')][string]$SourceDrive = 'S',
    [ValidatePattern('^[D-Zd-z]$')][string]$VcpkgDrive = 'V',
    [ValidatePattern('^[D-Zd-z]$')][string]$TempDrive = 'T',
    [switch]$SkipPackage
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $VcpkgRoot) { $VcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { $env:VCPKG_INSTALLATION_ROOT } }
if (-not $VcpkgRoot -or -not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake') -PathType Leaf)) {
    throw "No vcpkg root with scripts/buildsystems/vcpkg.cmake was found at '$VcpkgRoot'. Pass -VcpkgRoot."
}
$VcpkgRoot = [IO.Path]::GetFullPath($VcpkgRoot).TrimEnd('\', '/')
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'dist' }
$OutputDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if ($BuildDirectoryName -notmatch '^[A-Za-z0-9._-]+$') { throw 'BuildDirectoryName must be one plain folder name.' }

$letters = @($SourceDrive, $VcpkgDrive, $TempDrive | ForEach-Object { $_.ToUpperInvariant() })
if (@($letters | Select-Object -Unique).Count -ne 3) { throw 'The three drive letters must differ.' }
foreach ($letter in $letters) {
    if (Test-Path -LiteralPath "${letter}:\") { throw "Drive ${letter}: is already in use. Pass a free letter." }
}
foreach ($tool in @('git', 'cmake', 'subst')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is not on PATH." }
}

$repoParent = Split-Path -Parent $repoRoot
$vcpkgParent = Split-Path -Parent $VcpkgRoot
if (-not $repoParent -or -not $vcpkgParent) { throw 'The repository and the vcpkg root must each sit inside a folder, not at a drive root.' }
$tempReal = Join-Path ([IO.Path]::GetTempPath()) ('fo4vr-release-' + [Guid]::NewGuid().ToString('N'))
$sourceMapped = "$($letters[0]):\$(Split-Path -Leaf $repoRoot)"
$vcpkgMapped = "$($letters[1]):\$(Split-Path -Leaf $VcpkgRoot)"
$tempMapped = "$($letters[2]):\"
$buildMapped = "$sourceMapped\$BuildDirectoryName"

# What the binaries must not contain: the real folders behind each mapping, the user
# profile, and the account name where it would appear as a path component. Searched
# case-insensitively, with either slash.
$needles = [Collections.Generic.List[string]]::new()
foreach ($path in @($repoParent, $vcpkgParent, $tempReal, $env:USERPROFILE)) {
    if ($path -and $path.Length -ge 4) {
        $needles.Add($path.TrimEnd('\', '/'))
        $needles.Add($path.TrimEnd('\', '/').Replace('\', '/'))
    }
}
if ($env:USERNAME) {
    foreach ($form in @("\$env:USERNAME\", "/$env:USERNAME/", "MSBuildTemp$env:USERNAME")) { $needles.Add($form) }
}

function Find-RealPaths([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $texts = @(
        [Text.Encoding]::GetEncoding(28591).GetString($bytes),
        [Text.Encoding]::Unicode.GetString($bytes),
        [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)
    )
    $found = @()
    foreach ($needle in ($needles | Select-Object -Unique)) {
        foreach ($text in $texts) {
            if ($text.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $found += $needle; break }
        }
    }
    $found
}

$saved = @{}
foreach ($name in @('VCPKG_ROOT', 'VCPKG_INSTALLATION_ROOT', 'VCPKG_BINARY_SOURCES', 'TEMP', 'TMP', 'MSBUILDDISABLENODEREUSE')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$mapped = [Collections.Generic.List[string]]::new()
try {
    New-Item -ItemType Directory -Path $tempReal | Out-Null
    foreach ($pair in @(@($letters[0], $repoParent), @($letters[1], $vcpkgParent), @($letters[2], $tempReal))) {
        & subst "$($pair[0]):" $pair[1]
        if ($LASTEXITCODE -eq 0) { $mapped.Add($pair[0]) }
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath "$($pair[0]):\")) { throw "subst $($pair[0]): $($pair[1]) failed." }
    }
    Write-Host "Building from $sourceMapped with vcpkg at $vcpkgMapped and TEMP at $tempMapped."

    $env:VCPKG_ROOT = $vcpkgMapped
    $env:VCPKG_INSTALLATION_ROOT = $vcpkgMapped
    $env:VCPKG_BINARY_SOURCES = 'clear'
    $env:TEMP = $tempMapped
    $env:TMP = $tempMapped
    $env:MSBUILDDISABLENODEREUSE = '1'

    $cachePath = Join-Path $buildMapped 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
        $cache = (Get-Content -LiteralPath $cachePath -Raw).Replace("`r`n", "`n")
        if ($cache -notmatch '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$' -or $Matches[1].Trim() -ne $sourceMapped.Replace('\', '/')) {
            throw "$BuildDirectoryName was configured from another path. Remove it, or pass a new -BuildDirectoryName."
        }
    }

    & cmake -S $sourceMapped -B $buildMapped -G 'Visual Studio 17 2022' -A x64 `
        "-DCMAKE_TOOLCHAIN_FILE=$($vcpkgMapped.Replace('\', '/'))/scripts/buildsystems/vcpkg.cmake" `
        -DBUILD_FALLOUTVR=ON -DUPSCALER_ENABLE_NGX=ON -DUPSCALER_ENABLE_FFX=OFF -DUPSCALER_ENABLE_STREAMLINE=OFF `
        -DBUILD_TOOLS=OFF -DCOPY_BUILD=OFF -DPACKAGE=OFF -DMO2_INSTALL_PATH=
    if ($LASTEXITCODE -ne 0) { throw "Configure failed with exit code $LASTEXITCODE." }

    # CMake and vcpkg must keep the mapped spelling. A tree that resolved a mapping back to
    # the real folder would put that folder into every object it compiles.
    $cache = (Get-Content -LiteralPath $cachePath -Raw).Replace("`r`n", "`n")
    foreach ($key in @('CMAKE_HOME_DIRECTORY', 'CMAKE_TOOLCHAIN_FILE', 'VCPKG_INSTALLED_DIR', 'UPSCALER_NGX_LIBRARY', 'CommonLibPath')) {
        if ($cache -notmatch "(?m)^${key}:[A-Z]+=(.+)$") { throw "CMakeCache.txt has no $key." }
        $value = $Matches[1].Trim()
        if (-not ($value.StartsWith("$($letters[0]):", [StringComparison]::OrdinalIgnoreCase) -or
                $value.StartsWith("$($letters[1]):", [StringComparison]::OrdinalIgnoreCase))) {
            throw "$key resolved outside the mapped drives: $value"
        }
    }
    if ($cache -match '(?m)^MO2_INSTALL_PATH:[^=]+=(.+)$' -and $Matches[1].Trim()) { throw 'MO2_INSTALL_PATH is set in the build tree.' }

    if ($SkipPackage) {
        & cmake --build $buildMapped --config Release --target fo4vr_upscaler
        if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
    } else {
        & (Join-Path $sourceMapped 'scripts/package-release.ps1') -BuildDirectory $buildMapped -OutputDirectory $OutputDirectory
    }

    $binaries = @('fo4vr_upscaler.dll', 'fo4vr_upscaler.pdb', 'Fo4VRUpscalerOpenXR.dll', 'Fo4VRUpscalerOpenXR.pdb') |
        ForEach-Object { Join-Path $buildMapped "Release/$_" }
    $failed = $false
    foreach ($binary in $binaries) {
        if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Build output missing: $binary" }
        $hits = @(Find-RealPaths $binary)
        if ($hits.Count) {
            Write-Host "$(Split-Path -Leaf $binary) names local folders: $($hits -join ', ')"
            $failed = $true
        } else {
            Write-Host "$(Split-Path -Leaf $binary): no local folder or account name found."
        }
    }
    if ($failed) { throw 'The binaries name local folders. Do not publish this build.' }
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    foreach ($letter in $mapped) { & subst "${letter}:" /D | Out-Null }
    if (Test-Path -LiteralPath $tempReal) { Remove-Item -LiteralPath $tempReal -Recurse -Force -ErrorAction SilentlyContinue }
}
