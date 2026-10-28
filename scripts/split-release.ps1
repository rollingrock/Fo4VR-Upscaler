# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
    Splits a verified build archive into an installable mod and a documentation ZIP.
.DESCRIPTION
    Preserves the original DLL, PDB and TOML. The main archive retains consolidated
    licensing, notices, source information and build identity. For an NGX build it
    also carries NVIDIA's DLSS runtime, unmodified and with NVIDIA's licence beside it,
    taken from the pinned Streamline SDK that fetch-sdks.ps1 stages. The documents live
    in the companion ZIP, which is not installed into MO2.
.PARAMETER NgxRuntimeDirectory
    Where nvngx_dlss.dll and nvngx_dlss.license.txt are read from for an NGX build.
    Defaults to <repo>/.sdk-cache/streamline. Both must match the pins below.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceArchive,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$SourceArchiveSha256,
    [string]$OutputDirectory,
    [string]$NgxRuntimeDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$gitSafeRoot = $repoRoot.Replace('\', '/')
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'dist' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$SourceArchive = [IO.Path]::GetFullPath($SourceArchive)
if (-not $NgxRuntimeDirectory) { $NgxRuntimeDirectory = Join-Path $repoRoot '.sdk-cache/streamline' }
$NgxRuntimeDirectory = [IO.Path]::GetFullPath($NgxRuntimeDirectory)
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

# NVIDIA's DLSS runtime is the one vendor file a distribution carries, and only inside the
# mod archive. These pins name NVIDIA's production build in the Streamline SDK v2.12.0
# archive that sdk-manifest.psd1 pins. bin/x64/development in the same archive holds a
# watermarked build under the same file name, so the hash is the guard, not the name.
$ngxClientLibrarySha256 = '31E82B4EC3242EC6E5B42C73E1F5F5E260338A6EE213BD64444F6C2FA364AA84'
$ngxRuntimeSha256 = 'BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E'
$ngxRuntimeVersion = '310.7.0.0'
$ngxRuntimeDescription = 'NVIDIA DLSS - DVS PRODUCTION'
$ngxLicenseSha256 = '3027F23CA5A46DD9CB8183FBD522983A86F64D7DAAC5982912BF9F214671F294'
$ngxRuntimePath = 'F4SE/Plugins/Fo4VRUpscaler/nvngx_dlss.dll'
$ngxLicensePath = 'F4SE/Plugins/Fo4VRUpscaler/nvngx_dlss.license.txt'
$vendorRuntimePatterns = @('nvngx*.dll', 'sl.*.dll', 'amd_fidelityfx_*.dll', 'ffx_*.dll', 'libxess*.dll')
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
function MetaPath([string]$MetaDir, [string]$Name) { if ($MetaDir) { "$MetaDir/$Name" } else { $Name } }
function VerifyTree([string]$Tree, [string]$MetaDir = '') {
    $identity = Get-Content -LiteralPath (Join-Path $Tree (MetaPath $MetaDir 'BUILD.json')) -Raw | ConvertFrom-Json
    $actual = @(Get-ChildItem -LiteralPath $Tree -Recurse -File | ForEach-Object {
        $_.FullName.Substring($Tree.Length + 1).Replace('\', '/')
    })
    $expected = @($identity.files.PSObject.Properties.Name) + @(MetaPath $MetaDir 'BUILD.json')
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

# The runtime ships only beside a plugin that links NGX; an SDK-free build ships none. An
# NGX build whose runtime is missing or off its pin stops here, rather than publishing a
# mod archive without the DLL its players are told is included.
$backend = $original.PSObject.Properties['backend']
$bundleNgxRuntime = $null -ne $backend -and $backend.Value -eq 'ngx'
$vendorPins = [ordered]@{}
$ngxRuntimeRecord = $null
$ngxRuntimeNotice = ''
if ($bundleNgxRuntime) {
    if ($original.ngxClientLibrarySha256 -ne $ngxClientLibrarySha256) { throw 'This NGX build names a client library other than the pinned one.' }
    $runtimeSource = Join-Path $NgxRuntimeDirectory 'nvngx_dlss.dll'
    $runtimeLicenseSource = Join-Path $NgxRuntimeDirectory 'nvngx_dlss.license.txt'
    foreach ($path in @($runtimeSource, $runtimeLicenseSource)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "This NGX build ships NVIDIA's DLSS runtime, and $path is missing. Run scripts/fetch-sdks.ps1." }
    }
    if ((Hash $runtimeSource) -ne $ngxRuntimeSha256) { throw "nvngx_dlss.dll is not the pinned production build: $runtimeSource" }
    $runtimeInfo = (Get-Item -LiteralPath $runtimeSource).VersionInfo
    $runtimeVersion = '{0}.{1}.{2}.{3}' -f $runtimeInfo.FileMajorPart, $runtimeInfo.FileMinorPart, $runtimeInfo.FileBuildPart, $runtimeInfo.FilePrivatePart
    if ($runtimeVersion -ne $ngxRuntimeVersion) { throw "nvngx_dlss.dll reports version $runtimeVersion, not $ngxRuntimeVersion." }
    if ($runtimeInfo.FileDescription -ne $ngxRuntimeDescription) { throw "nvngx_dlss.dll describes itself as '$($runtimeInfo.FileDescription)', not '$ngxRuntimeDescription'." }
    $signature = Get-AuthenticodeSignature -LiteralPath $runtimeSource
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notlike 'CN=NVIDIA Corporation,*') {
        throw "nvngx_dlss.dll does not carry a valid NVIDIA signature (status $($signature.Status))."
    }
    if ((Hash $runtimeLicenseSource) -ne $ngxLicenseSha256) { throw "nvngx_dlss.license.txt is not the pinned NVIDIA licence: $runtimeLicenseSource" }
    # The notice the full archive reproduces as NVIDIA-NGX.txt must be the same text that
    # travels beside the DLL, so NOTICES.txt and the licence file cannot disagree.
    $inputNgxNotice = Join-Path $inputTree 'licenses/NVIDIA-NGX.txt'
    if (-not (Test-Path -LiteralPath $inputNgxNotice -PathType Leaf) -or (Hash $inputNgxNotice) -ne $ngxLicenseSha256) {
        throw 'The source archive does not carry the pinned NVIDIA licence as licenses/NVIDIA-NGX.txt.'
    }
    $vendorPins[$ngxRuntimePath] = $ngxRuntimeSha256
    $vendorPins[$ngxLicensePath] = $ngxLicenseSha256
    $ngxRuntimeRecord = [ordered]@{
        path = $ngxRuntimePath; sha256 = $ngxRuntimeSha256; fileVersion = $ngxRuntimeVersion
        origin = 'NVIDIA Streamline SDK v2.12.0, bin/x64/nvngx_dlss.dll, unmodified'
        licence = $ngxLicensePath; licenceSha256 = $ngxLicenseSha256
    }
    $ngxRuntimeNotice = @(
        'NVIDIA DLSS runtime.'
        'Includes NVIDIA DLSS, redistributed unmodified under the NVIDIA RTX SDKs License.'
        'NVIDIA, NVIDIA RTX and DLSS are trademarks of NVIDIA Corporation; this mod is not sponsored or endorsed by NVIDIA.'
        ''
        "$ngxRuntimePath is NVIDIA DLSS $ngxRuntimeVersion"
        "(SHA-256 $ngxRuntimeSha256), copied"
        "unmodified from NVIDIA's official Streamline SDK v2.12.0 (bin/x64/nvngx_dlss.dll)."
        "It is NVIDIA's software, licensed under the NVIDIA RTX SDKs License, whose full text"
        'is beside it in nvngx_dlss.license.txt and reproduced below as NVIDIA-NGX.txt. It is'
        'not covered by the GNU General Public License and may not be redistributed separately'
        'from this mod.'
    ) -join "`n"
}
# Kept apart from $runtimeFiles: those are checked against the source archive, which never
# carries a vendor runtime; these are checked against their pins.
$vendorFiles = @($vendorPins.Keys)

$runtimeFiles = @('F4SE/Plugins/fo4vr_upscaler.dll', 'F4SE/Plugins/fo4vr_upscaler.pdb', 'F4SE/Plugins/Fo4VRUpscaler.toml')
$runtimeFiles += @('Fo4VRUpscalerOpenXR.dll', 'Fo4VRUpscalerOpenXR.pdb', 'Fo4VRUpscalerOpenXR.json') | ForEach-Object { "F4SE/Plugins/Fo4VRUpscaler/OpenXR/$_" }
# Everything the build archive carries under F4SE/ is payload. Payload this split does not
# name would otherwise be left out of the mod archive without a word, and every later check
# compares against the same list, so none of them would notice.
$inputPayload = @(Get-ChildItem -LiteralPath (Join-Path $inputTree 'F4SE') -Recurse -File | ForEach-Object {
    $_.FullName.Substring($inputTree.Length + 1).Replace('\', '/')
})
$unplaced = @($inputPayload | Where-Object { $_ -notin $runtimeFiles })
if ($unplaced.Count) { throw "The source archive carries payload this split does not place: $($unplaced -join ', ')" }
foreach ($file in $runtimeFiles) { CopyRelative $inputTree $mainTree $file }
# Every file the mod archive installs lives under the plugin's own folder, so nothing it
# carries can collide with another mod's LICENSE.txt or BUILD.json at the Data root.
$modMeta = 'F4SE/Plugins/Fo4VRUpscaler'
$mainMeta = Join-Path $mainTree $modMeta
New-Item -ItemType Directory -Force -Path $mainMeta | Out-Null
if ($bundleNgxRuntime) {
    # Byte for byte, never through WriteText: the pins are NVIDIA's own bytes.
    Copy-Item -LiteralPath $runtimeSource -Destination (Join-Path $mainTree $ngxRuntimePath)
    Copy-Item -LiteralPath $runtimeLicenseSource -Destination (Join-Path $mainTree $ngxLicensePath)
}

$licenseParts = @('LICENSE', 'EXCEPTIONS.md')
$licenseText = @('BlurAway VR licensing. Original texts follow, without changes.')
foreach ($relative in $licenseParts) {
    $licenseText += "`n----- $relative -----`n" + [IO.File]::ReadAllText((Join-Path $inputTree $relative))
}
WriteText (Join-Path $mainMeta 'LICENSE.txt') ($licenseText -join "`n")
$noticeFiles = @(Get-ChildItem -LiteralPath (Join-Path $inputTree 'licenses') -File | Where-Object { $_.Name -ne 'INDEX.txt' } | Sort-Object Name)
if (-not $noticeFiles.Count) { throw 'Source distribution contains no notices.' }
# The notices package-release.ps1 always writes, whatever the vcpkg tree held. ProggyClean
# is the font Dear ImGui compiles into the DLL; an archive built before its notice existed
# must not reach players.
foreach ($required in @('AMD-RCAS.txt', 'CommonLibF4.txt', 'DevBenchAPI.txt', 'OpenXR.txt', 'ProggyClean.txt')) {
    if (-not @($noticeFiles | Where-Object { $_.Name -eq $required }).Count) { throw "Source distribution lacks the $required notice." }
}
$noticeText = @('Third-party notices. Each original component notice is reproduced below.')
if ($bundleNgxRuntime) { $noticeText += "`n" + $ngxRuntimeNotice }
foreach ($file in $noticeFiles) { $noticeText += "`n----- $($file.Name) -----`n" + [IO.File]::ReadAllText($file.FullName) }
WriteText (Join-Path $mainMeta 'NOTICES.txt') ($noticeText -join "`n")
$sourceText = [IO.File]::ReadAllText((Join-Path $inputTree 'SOURCE.txt'))
$sourceText = $sourceText.Replace('recorded' + "`r`n" + 'in EXCEPTIONS.md', 'recorded in LICENSE.txt')
$sourceText = $sourceText.Replace('recorded' + "`n" + 'in EXCEPTIONS.md', 'recorded in LICENSE.txt')
$sourceText = $sourceText.Replace('LICENSE carries the full licence text.', 'LICENSE.txt carries the full licence and exception texts.')
$sourceText = $sourceText.Replace('THIRD_PARTY.md records every' + "`r`n" + 'third-party component and licenses/ carries their notices verbatim.', 'NOTICES.txt carries the third-party notices verbatim. The companion documentation ZIP includes the component ledger.')
$sourceText = $sourceText.Replace('THIRD_PARTY.md records every' + "`n" + 'third-party component and licenses/ carries their notices verbatim.', 'NOTICES.txt carries the third-party notices verbatim. The companion documentation ZIP includes the component ledger.')
if ($bundleNgxRuntime) {
    # The written offer covers the Corresponding Source for the software in this archive,
    # and NVIDIA's runtime is in it now, so the offer says plainly what it does not cover.
    $newline = if ($sourceText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $sourceText = $sourceText.TrimEnd() + $newline + $newline + (@(
            "This archive also contains nvngx_dlss.dll, NVIDIA's DLSS runtime, redistributed"
            'unmodified under the NVIDIA RTX SDKs License in nvngx_dlss.license.txt. It is a'
            'Proprietary Runtime Library under the Linking Exception in LICENSE.txt. It is not'
            'part of the Corresponding Source, and the offer above does not cover it.'
        ) -join $newline) + $newline
}
# The replacements above depend on how package-release.ps1 wraps its lines; a wording change
# there must fail here rather than ship a SOURCE.txt naming files this archive does not carry.
foreach ($absent in @('EXCEPTIONS.md', 'THIRD_PARTY.md', 'licenses/')) {
    if ($sourceText.Contains($absent)) { throw "SOURCE.txt still names $absent, which the mod archive does not carry." }
}
WriteText (Join-Path $mainMeta 'SOURCE.txt') $sourceText

$documents = @('README.md', 'KNOWN_ISSUES.md', 'RELEASE_NOTES.md', 'THIRD_PARTY.md', 'EXCEPTIONS.md')
foreach ($file in $documents) { CopyRelative $repoRoot $docsTree $file }
Copy-Item -LiteralPath (Join-Path $mainMeta 'LICENSE.txt') -Destination $docsTree
$payloadLine = if ($bundleNgxRuntime) {
    "The main mod archive includes the matching DLL, PDB, configuration and NVIDIA's DLSS runtime (nvngx_dlss.dll $ngxRuntimeVersion)."
} else {
    'The main mod archive includes the matching DLL, PDB and configuration.'
}
WriteText (Join-Path $docsTree 'READ-ME-FIRST.txt') @"
This ZIP is documentation for BlurAway VR $($original.version).
Do not install it in MO2. Extract it somewhere convenient and start with README.md.
Install $([IO.Path]::GetFileName($mainArchive)) as the mod.
$payloadLine
Its DLL/configuration were preserved from source build $($original.sourceCommit).
The documentation and archive layout are from packaging commit $packagingCommit.
"@

function MakeArchive([string]$Tree, [string]$Archive, [string]$Format, [string]$Role, [string]$MetaDir = '') {
    $hashes = [ordered]@{}
    foreach ($file in (Get-ChildItem -LiteralPath $Tree -Recurse -File | Sort-Object FullName)) {
        $hashes[$file.FullName.Substring($Tree.Length + 1).Replace('\', '/')] = Hash $file.FullName
    }
    $manifest = [ordered]@{
        version = $original.version; candidate = $original.candidate; sourceCommit = $original.sourceCommit
        rendererBaseline = $original.rendererBaseline; dllVersion = $original.dllVersion
        ngxClientLibrarySha256 = $original.ngxClientLibrarySha256
        packagingCommit = $packagingCommit; sourceArchiveSha256 = $SourceArchiveSha256
    }
    # The files map already hashes the runtime; this names where it came from and under
    # which licence. Null in a mod archive that carries no vendor runtime.
    if ($Role -eq 'mod') { $manifest['ngxRuntime'] = $ngxRuntimeRecord }
    $manifest['role'] = $Role
    $manifest['files'] = $hashes
    $buildJson = MetaPath $MetaDir 'BUILD.json'
    WriteText (Join-Path $Tree $buildJson) ($manifest | ConvertTo-Json -Depth 6)
    # The mod archive's root holds F4SE and nothing else, so no file lands at the Data root.
    if ($Role -eq 'mod') {
        $roots = @(Get-ChildItem -LiteralPath $Tree -Force | ForEach-Object { $_.Name })
        if (@(Compare-Object @('F4SE') $roots).Count) { throw "Mod archive root holds more than F4SE: $($roots -join ', ')" }
    }
    Push-Location $Tree
    try {
        & cmake -E tar cf $Archive "--format=$Format" -- .
        if ($LASTEXITCODE -ne 0) { throw 'Archive creation failed.' }
    } finally { Pop-Location }
    $verify = Join-Path $scratch ($Role + '-verify')
    New-Item -ItemType Directory -Path $verify | Out-Null
    Extract $Archive $verify
    $readback = VerifyTree $verify $MetaDir
    if ((Hash (Join-Path $Tree $buildJson)) -ne (Hash (Join-Path $verify $buildJson))) { throw 'Manifest changed in archive.' }
    if ($Role -eq 'mod') {
        $expected = @($runtimeFiles) + @($vendorFiles) + @('LICENSE.txt', 'NOTICES.txt', 'SOURCE.txt' | ForEach-Object { MetaPath $MetaDir $_ })
        if (@(Compare-Object $expected @($readback.files.PSObject.Properties.Name)).Count) { throw 'Unexpected main package content.' }
        foreach ($file in $runtimeFiles) {
            if ((Hash (Join-Path $verify $file)) -ne (Hash (Join-Path $inputTree $file))) { throw 'Runtime payload changed.' }
        }
        foreach ($vendor in $vendorPins.GetEnumerator()) {
            if ((Hash (Join-Path $verify $vendor.Key)) -ne $vendor.Value) { throw "Vendor file changed: $($vendor.Key)" }
        }
        # Exactly the DLLs this layout names, and no vendor runtime but the pinned one,
        # matched by full path rather than by file name.
        $verifyFiles = @(Get-ChildItem -LiteralPath $verify -Recurse -File | ForEach-Object { $_.FullName.Substring($verify.Length + 1).Replace('\', '/') })
        $allowedDlls = @('F4SE/Plugins/fo4vr_upscaler.dll', 'F4SE/Plugins/Fo4VRUpscaler/OpenXR/Fo4VRUpscalerOpenXR.dll') + @($vendorFiles | Where-Object { $_ -like '*.dll' })
        $strayDlls = @($verifyFiles | Where-Object { $_ -like '*.dll' -and $_ -notin $allowedDlls })
        if ($strayDlls.Count) { throw "Unexpected DLL in the mod archive: $($strayDlls -join ', ')" }
        $unpinned = @($verifyFiles | Where-Object {
                $entry = $_
                $leaf = Split-Path -Leaf $entry
                $isVendor = @($vendorRuntimePatterns | Where-Object { $leaf -like $_ }).Count -gt 0
                $isVendor -and $entry -notin $vendorFiles
            })
        if ($unpinned.Count) { throw "The mod archive carries an unpinned vendor runtime: $($unpinned -join ', ')" }
        if ($bundleNgxRuntime -and -not [IO.File]::ReadAllText((Join-Path $verify (MetaPath $MetaDir 'NOTICES.txt'))).Contains($ngxRuntimeNotice)) {
            throw 'The NVIDIA runtime notice was lost.'
        }
        foreach ($relative in $licenseParts) {
            if (-not [IO.File]::ReadAllText((Join-Path $verify (MetaPath $MetaDir 'LICENSE.txt'))).Contains([IO.File]::ReadAllText((Join-Path $inputTree $relative)))) { throw 'License text was lost.' }
        }
        foreach ($file in $noticeFiles) {
            if (-not [IO.File]::ReadAllText((Join-Path $verify (MetaPath $MetaDir 'NOTICES.txt'))).Contains([IO.File]::ReadAllText($file.FullName))) { throw 'Component notice was lost.' }
        }
    } elseif (@(Get-ChildItem -LiteralPath $verify -Recurse -File | Where-Object { $_.Extension -in @('.dll', '.pdb', '.toml') }).Count) {
        throw 'Documentation archive contains runtime files.'
    }
    $digest = Hash $Archive
    WriteText "$Archive.sha256" "$digest  $([IO.Path]::GetFileName($Archive))`n"
    [pscustomobject]@{role=$Role;path=$Archive;bytes=(Get-Item -LiteralPath $Archive).Length;sha256=$digest;files=$hashes.Count+1}
}
$results = @((MakeArchive $mainTree $mainArchive '7zip' 'mod' $modMeta), (MakeArchive $docsTree $docsArchive 'zip' 'docs'))
$results | ConvertTo-Json -Depth 4
if (-not $scratch.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe scratch cleanup path.' }
Remove-Item -LiteralPath $scratch -Recurse -Force
