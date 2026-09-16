# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
    Splits a verified build archive into an installable mod and a documentation ZIP.
.DESCRIPTION
    Preserves the original DLL, PDB and TOML. The main archive retains consolidated
    licensing, notices, source information and build identity. Guides and research
    documents live in the companion ZIP, which is not installed into MO2.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceArchive,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$SourceArchiveSha256,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$gitSafeRoot = $repoRoot.Replace('\', '/')
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'dist' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$SourceArchive = [IO.Path]::GetFullPath($SourceArchive)
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function ReadGit([string[]]$Arguments) {
    $result = @(& git -c safe.directory=$gitSafeRoot -c core.excludesFile= -C $repoRoot @Arguments)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot establish packaging source identity.' }
    $result
}
if ((Hash $SourceArchive) -ne $SourceArchiveSha256) { throw 'Source archive hash mismatch.' }
$packagingCommit = (ReadGit @('rev-parse', 'HEAD')) -join ''
if (@(ReadGit @('status', '--porcelain')).Count) { throw 'Commit packaging edits before splitting a release.' }
$stem = [IO.Path]::GetFileNameWithoutExtension($SourceArchive)
$mainArchive = Join-Path $OutputDirectory "$stem-mod.7z"
$docsArchive = Join-Path $OutputDirectory "$stem-docs.zip"
foreach ($target in @($mainArchive, $docsArchive)) {
    if (Test-Path -LiteralPath $target) { throw "Refusing to replace an existing distribution: $target" }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$scratch = [IO.Path]::GetFullPath((Join-Path $OutputDirectory ('.split-' + [Guid]::NewGuid().ToString('N'))))
$outputPrefix = $OutputDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $scratch.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid scratch path.' }
New-Item -ItemType Directory -Path $scratch | Out-Null
$inputTree = Join-Path $scratch 'input'
$mainTree = Join-Path $scratch 'mod'
$docsTree = Join-Path $scratch 'docs'
foreach ($dir in @($inputTree, $mainTree, $docsTree)) { New-Item -ItemType Directory -Path $dir | Out-Null }

function Extract([string]$Archive, [string]$Destination) {
    $listing = @(& cmake -E tar tf $Archive)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list archive.' }
    foreach ($entry in $listing) {
        if ($entry -match '(^[/\\]|^[A-Za-z]:|(^|[/\\])\.\.([/\\]|$))') { throw 'Unsafe archive path.' }
    }
    Push-Location $Destination
    try {
        & cmake -E tar xf $Archive
        if ($LASTEXITCODE -ne 0) { throw 'Cannot extract archive.' }
    } finally { Pop-Location }
}
function VerifyTree([string]$Tree) {
    $identity = Get-Content -LiteralPath (Join-Path $Tree 'BUILD.json') -Raw | ConvertFrom-Json
    $actual = @(Get-ChildItem -LiteralPath $Tree -Recurse -File | ForEach-Object {
        $_.FullName.Substring($Tree.Length + 1).Replace('\', '/')
    })
    $expected = @($identity.files.PSObject.Properties.Name) + @('BUILD.json')
    if (@(Compare-Object $expected $actual).Count) { throw 'Archive file set differs from manifest.' }
    foreach ($file in $identity.files.PSObject.Properties) {
        $path = [IO.Path]::GetFullPath((Join-Path $Tree $file.Name))
        if (-not $path.StartsWith($Tree + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe manifest path.' }
        if ((Hash $path) -ne $file.Value) { throw "Archive hash mismatch: $($file.Name)" }
    }
    $identity
}
function CopyRelative([string]$From, [string]$To, [string]$Relative) {
    $destination = Join-Path $To $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path $From $Relative) -Destination $destination
}
function WriteText([string]$Path, [string]$Value) {
    [IO.File]::WriteAllText($Path, $Value, [Text.UTF8Encoding]::new($false))
}
Extract $SourceArchive $inputTree
$original = VerifyTree $inputTree
$runtimeFiles = @('F4SE/Plugins/fo4vr_upscaler.dll', 'F4SE/Plugins/fo4vr_upscaler.pdb', 'F4SE/Plugins/Fo4VRUpscaler.toml')
$runtimeFiles += @('Fo4VRUpscalerOpenXR.dll', 'Fo4VRUpscalerOpenXR.pdb', 'Fo4VRUpscalerOpenXR.json') | ForEach-Object { "F4SE/Plugins/Fo4VRUpscaler/OpenXR/$_" }
foreach ($file in $runtimeFiles) { CopyRelative $inputTree $mainTree $file }

$licenseParts = @('LICENSE', 'EXCEPTIONS.md')
$licenseText = @('FO4VR Upscaler licensing. Original texts follow, without changes.')
foreach ($relative in $licenseParts) {
    $licenseText += "`n----- $relative -----`n" + [IO.File]::ReadAllText((Join-Path $inputTree $relative))
}
WriteText (Join-Path $mainTree 'LICENSE.txt') ($licenseText -join "`n")
$noticeFiles = @(Get-ChildItem -LiteralPath (Join-Path $inputTree 'licenses') -File | Where-Object { $_.Name -ne 'INDEX.txt' } | Sort-Object Name)
if (-not $noticeFiles.Count) { throw 'Source distribution contains no notices.' }
$noticeText = @('Third-party notices. Each original component notice is reproduced below.')
foreach ($file in $noticeFiles) { $noticeText += "`n----- $($file.Name) -----`n" + [IO.File]::ReadAllText($file.FullName) }
WriteText (Join-Path $mainTree 'NOTICES.txt') ($noticeText -join "`n")
$sourceText = [IO.File]::ReadAllText((Join-Path $inputTree 'SOURCE.txt'))
$sourceText = $sourceText.Replace('recorded' + "`r`n" + 'in EXCEPTIONS.md', 'recorded in LICENSE.txt')
$sourceText = $sourceText.Replace('recorded' + "`n" + 'in EXCEPTIONS.md', 'recorded in LICENSE.txt')
$sourceText = $sourceText.Replace('LICENSE carries the full licence text.', 'LICENSE.txt carries the full licence and exception texts.')
$sourceText = $sourceText.Replace('THIRD_PARTY.md records every' + "`r`n" + 'third-party component and licenses/ carries their notices verbatim.', 'NOTICES.txt carries the third-party notices verbatim. The companion documentation ZIP includes the component ledger.')
$sourceText = $sourceText.Replace('THIRD_PARTY.md records every' + "`n" + 'third-party component and licenses/ carries their notices verbatim.', 'NOTICES.txt carries the third-party notices verbatim. The companion documentation ZIP includes the component ledger.')
WriteText (Join-Path $mainTree 'SOURCE.txt') $sourceText

$documents = @('README.md', 'TESTING.md', 'HEADSET_CHECKLIST.md', 'KNOWN_ISSUES.md',
    'RELEASE_NOTES.md', 'THIRD_PARTY.md', 'EXCEPTIONS.md', 'docs/ACCEPTANCE.md', 'docs/MODULE_MAP.md',
    'docs/OPENXR_PANEL_TEST_1.md', 'docs/QUALITY_CONTINUITY_TEST_1.md', 'docs/QUALITY_SOFTNESS_ISOLATION.md',
    'docs/QUALITY_HISTORY_CONTINUITY.md')
foreach ($file in $documents) { CopyRelative $repoRoot $docsTree $file }
Copy-Item -LiteralPath (Join-Path $mainTree 'LICENSE.txt') -Destination $docsTree
WriteText (Join-Path $docsTree 'READ-ME-FIRST.txt') @"
This ZIP is documentation for FO4VR Upscaler $($original.version).
Do not install it in MO2. Extract it somewhere convenient and start with TESTING.md.
Install $([IO.Path]::GetFileName($mainArchive)) as the mod.
The main mod archive includes the matching DLL, PDB and configuration.
Its DLL/configuration were preserved from source build $($original.sourceCommit).
The documentation and archive layout are from packaging commit $packagingCommit.
"@

function MakeArchive([string]$Tree, [string]$Archive, [string]$Format, [string]$Role) {
    $hashes = [ordered]@{}
    foreach ($file in (Get-ChildItem -LiteralPath $Tree -Recurse -File | Sort-Object FullName)) {
        $hashes[$file.FullName.Substring($Tree.Length + 1).Replace('\', '/')] = Hash $file.FullName
    }
    $manifest = [ordered]@{
        version = $original.version; candidate = $original.candidate; sourceCommit = $original.sourceCommit
        rendererBaseline = $original.rendererBaseline; dllVersion = $original.dllVersion
        ngxClientLibrarySha256 = $original.ngxClientLibrarySha256
        packagingCommit = $packagingCommit; sourceArchiveSha256 = $SourceArchiveSha256
        role = $Role; files = $hashes
    }
    WriteText (Join-Path $Tree 'BUILD.json') ($manifest | ConvertTo-Json -Depth 6)
    Push-Location $Tree
    try {
        & cmake -E tar cf $Archive "--format=$Format" -- .
        if ($LASTEXITCODE -ne 0) { throw 'Archive creation failed.' }
    } finally { Pop-Location }
    $verify = Join-Path $scratch ($Role + '-verify')
    New-Item -ItemType Directory -Path $verify | Out-Null
    Extract $Archive $verify
    $readback = VerifyTree $verify
    if ((Hash (Join-Path $Tree 'BUILD.json')) -ne (Hash (Join-Path $verify 'BUILD.json'))) { throw 'Manifest changed in archive.' }
    if ($Role -eq 'mod') {
        $expected = @($runtimeFiles) + @('LICENSE.txt', 'NOTICES.txt', 'SOURCE.txt')
        if (@(Compare-Object $expected @($readback.files.PSObject.Properties.Name)).Count) { throw 'Unexpected main package content.' }
        foreach ($file in $runtimeFiles) {
            if ((Hash (Join-Path $verify $file)) -ne (Hash (Join-Path $inputTree $file))) { throw 'Runtime payload changed.' }
        }
        foreach ($relative in $licenseParts) {
            if (-not [IO.File]::ReadAllText((Join-Path $verify 'LICENSE.txt')).Contains([IO.File]::ReadAllText((Join-Path $inputTree $relative)))) { throw 'License text was lost.' }
        }
        foreach ($file in $noticeFiles) {
            if (-not [IO.File]::ReadAllText((Join-Path $verify 'NOTICES.txt')).Contains([IO.File]::ReadAllText($file.FullName))) { throw 'Component notice was lost.' }
        }
    } elseif (@(Get-ChildItem -LiteralPath $verify -Recurse -File | Where-Object { $_.Extension -in @('.dll', '.pdb', '.toml') }).Count) {
        throw 'Documentation archive contains runtime files.'
    }
    $digest = Hash $Archive
    WriteText "$Archive.sha256" "$digest  $([IO.Path]::GetFileName($Archive))`n"
    [pscustomobject]@{role=$Role;path=$Archive;bytes=(Get-Item -LiteralPath $Archive).Length;sha256=$digest;files=$hashes.Count+1}
}
$results = @((MakeArchive $mainTree $mainArchive '7zip' 'mod'), (MakeArchive $docsTree $docsArchive 'zip' 'docs'))
$results | ConvertTo-Json -Depth 4
if (-not $scratch.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe scratch cleanup path.' }
Remove-Item -LiteralPath $scratch -Recurse -Force
