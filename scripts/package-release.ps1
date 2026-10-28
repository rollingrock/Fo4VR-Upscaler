# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
	Builds the release archive, and refuses to build one that is not distributable.

.DESCRIPTION
	A release archive conveys the program in object-code form, so the licence set has to
	travel with the binary. This script is the single audited path to a release; the
	if(PACKAGE) block in CMakeLists.txt is a build smoke test for handing a build to a
	tester, not a conveyance.

	The archive carries, at its root: LICENSE, EXCEPTIONS.md, THIRD_PARTY.md, SOURCE.txt
	and a licenses/ directory. SOURCE.txt names the repository and the exact commit the
	binary was built from and carries the GPL-3 section 6(b) written offer, which is why
	a dirty work tree is a hard failure - a commit SHA that does not describe the bytes
	in the archive is worse than no SHA at all. licenses/ carries the verbatim notice
	text of every statically linked third party, collected from the copyright file vcpkg
	installs for each port; naming a licence in THIRD_PARTY.md is not including its
	notice, and the MIT and BSD terms require the notice.

	The main mod archive keeps the PDB. The documents are split into a companion
	ZIP after the full build archive has been verified. The full archive remains a
	local verification intermediate, not the mod-list download.

	The PDB ships deliberately. It is what makes a Buffout crash log name our code.

	Every required file is asserted present before the archive is produced, the staging
	tree is swept for vendor runtime files, and the finished archive is listed back and
	swept again, so a proprietary binary cannot leave in this archive by accident. The
	NVIDIA NGX client library is inside fo4vr_upscaler.dll by design and by the licence
	row that permits it; "no vendor DLL" is a statement about loose runtime files.

	The full archive therefore carries no vendor runtime. For an NGX build,
	split-release.ps1 adds NVIDIA's DLSS runtime to the published -mod.7z, unmodified,
	with its licence, after checking it against the pins it carries.

	The positive required-files assertion follows the same pattern as
	scripts/package-release.ps1 in the GPL-3.0 FO4 Community Shaders reference, which is
	a permitted source under this project's licence decision.

.PARAMETER BuildDirectory
	The CMake build directory. Defaults to <repo>/buildvr, which is what the vr presets set.

.PARAMETER Configuration
	Build configuration to package. Defaults to Release.

.PARAMETER Triplet
	vcpkg triplet whose installed tree holds the port copyright files. Read from the
	build tree's own CMakeCache.txt when not given, because the triplet depends on how
	the tree was configured: the default NGX build forces x64-windows-static-md so its
	objects agree with NVIDIA's dynamic-CRT client library, and
	the no-NGX preset also uses x64-windows-static-md for the OpenVR headers.

.PARAMETER Contact
	Where a section 6(b) source request goes. Defaults to the repository issue tracker.

.PARAMETER CandidateLabel
	Optional candidate label, such as rc1. The archive also includes the source commit.

.PARAMETER AcceptedRendererCommit
	The accepted renderer checkpoint; candidates require unchanged src/Upscaler files.

.PARAMETER KeepStaging
	Leave staging and extracted verification trees after packaging, for inspection.
#>
[CmdletBinding()]
param(
	[string]$BuildDirectory,
	[string]$Configuration = 'Release',
	[string]$Triplet,
	[string]$Contact,
	[ValidatePattern('^(rc[1-9][0-9]*)?$')][string]$CandidateLabel = '',
	[ValidatePattern('^([0-9a-f]{40})?$')][string]$AcceptedRendererCommit = '',
	[switch]$KeepStaging
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$gitSafeRoot = $repoRoot.Replace('\', '/')
$repositoryUrl = 'https://github.com/rollingrock/Fo4VR-Upscaler'
$pluginName = 'fo4vr_upscaler'
$tomlName = 'Fo4VRUpscaler.toml'

if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repoRoot 'buildvr' }
if (-not $Contact) { $Contact = "$repositoryUrl/issues" }

# ---- the vcpkg triplet, read from the tree that was built rather than assumed ----
#
# The notices come from vcpkg_installed/<triplet>/share, and which triplet exists is
# decided by how the tree was configured - the NGX default forces x64-windows-static-md.
# A hard-coded guess fails as "required vcpkg installed tree not found" on a build that
# is in fact perfectly good, which is the least informative way to stop a release.
$cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
if (-not $Triplet) {
	if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
		throw "No CMakeCache.txt under $BuildDirectory, so the vcpkg triplet cannot be read. Configure and build the tree first, or pass -Triplet."
	}
	$cacheText = Get-Content -LiteralPath $cachePath -Raw
	if ($cacheText -notmatch '(?m)^VCPKG_TARGET_TRIPLET:[A-Za-z]+=(.+)$') {
		throw "CMakeCache.txt in $BuildDirectory declares no VCPKG_TARGET_TRIPLET, so the notices have no source tree. Pass -Triplet."
	}
	$Triplet = $Matches[1].Trim()
}

foreach ($tool in @('git', 'cmake')) {
	if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
		throw "$tool is not on PATH."
	}
}

function Assert-SourcePath {
	param([string]$Path, [string]$What)
	if (-not (Test-Path -LiteralPath $Path)) {
		throw "Required $What not found: $Path"
	}
}

function Assert-PackageFile {
	param([string]$RelativePath)
	$path = Join-Path $stagingRoot $RelativePath
	if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
		throw "Required file is missing from the archive: $RelativePath"
	}
}

# ---- version, from the one source that also feeds the binary ----

$cmakeLists = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch '(?m)^\s*set\(VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s*\)') {
	throw 'Could not read set(VERSION ...) from CMakeLists.txt, so the archive name has no source.'
}
$version = $Matches[1]

# ---- provenance, and the reason a dirty tree stops here ----

$commit = & git -c safe.directory=$gitSafeRoot -c core.excludesFile= -C $repoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or -not $commit) {
	throw 'git rev-parse HEAD failed, so there is no commit to name in SOURCE.txt.'
}
$commit = "$commit".Trim()

$dirty = @(& git -c safe.directory=$gitSafeRoot -c core.excludesFile= -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0) {
	throw 'git status failed, so the work tree could not be proven clean.'
}
if ($dirty.Count -gt 0) {
	throw @"
The work tree is dirty, so no commit describes the binary being packaged:
$($dirty -join [Environment]::NewLine)
Commit or stash, rebuild, and run again. SOURCE.txt has to be true.
"@
}

# ---- build this clean source, without deploying to a game installation ----

$cache = (Get-Content -LiteralPath $cachePath -Raw).Replace("`r`n", "`n")
if ($cache -notmatch '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$' -or
    [IO.Path]::GetFullPath($Matches[1].Trim()) -ne [IO.Path]::GetFullPath($repoRoot)) {
    throw 'The build directory belongs to a different source checkout.'
}
foreach ($option in @('COPY_BUILD', 'PACKAGE')) {
    if ($cache -match "(?m)^${option}:[^=]+=(ON|TRUE|1|YES)$") {
        throw "$option must be OFF for isolated packaging. Configure without deployment first."
    }
}
if ($cache -match '(?m)^MO2_INSTALL_PATH:[^=]+=(.+)$' -and $Matches[1].Trim()) {
    throw 'MO2_INSTALL_PATH must be empty for isolated packaging.'
}
if ($CandidateLabel -and ($Configuration -ne 'Release' -or
    $cache -notmatch '(?m)^UPSCALER_ENABLE_NGX:BOOL=ON$' -or
    $cache -match '(?m)^UPSCALER_ENABLE_(FFX|STREAMLINE):BOOL=ON$')) {
    throw 'This tester candidate requires Release with NGX ON, FFX OFF and Streamline OFF.'
}

if ($CandidateLabel) {
    if (-not $AcceptedRendererCommit) { throw 'Candidates must name the accepted renderer checkpoint.' }
    & git -c safe.directory=$gitSafeRoot -c core.excludesFile= -C $repoRoot merge-base --is-ancestor $AcceptedRendererCommit $commit
    if ($LASTEXITCODE -ne 0) { throw 'Accepted renderer checkpoint is not an ancestor of this build.' }
    & git -c safe.directory=$gitSafeRoot -c core.excludesFile= -C $repoRoot diff --quiet $AcceptedRendererCommit $commit -- src/Upscaler
    if ($LASTEXITCODE -ne 0) { throw 'Renderer files changed since acceptance; qualify and name the new baseline.' }
}
$ngxLibraryHash = $null
if ($cache -match '(?m)^UPSCALER_ENABLE_NGX:BOOL=ON$') {
    if ($cache -notmatch '(?m)^UPSCALER_NGX_LIBRARY:FILEPATH=(.+)$') { throw 'NGX client library path is missing.' }
    $ngxLibraryPath = $Matches[1].Trim()
    $ngxLibraryHash = (Get-FileHash -LiteralPath $ngxLibraryPath -Algorithm SHA256).Hash
    if ($ngxLibraryHash -ne '31E82B4EC3242EC6E5B42C73E1F5F5E260338A6EE213BD64444F6C2FA364AA84') {
        throw 'The NGX client library does not match the recorded SDK pin.'
    }
}

& cmake --build $BuildDirectory --config $Configuration --target $pluginName --clean-first
if ($LASTEXITCODE -ne 0) { throw "Candidate build failed with exit code $LASTEXITCODE." }
$postCommit = & git -c safe.directory=$gitSafeRoot -c core.excludesFile= -C $repoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or "$postCommit".Trim() -ne $commit) {
    throw 'Source HEAD changed during the build.'
}
$postDirty = @(& git -c safe.directory=$gitSafeRoot -c core.excludesFile= -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0 -or $postDirty.Count -ne 0) { throw 'Source changed during the build.' }

if ($ngxLibraryHash -and (Get-FileHash -LiteralPath $ngxLibraryPath -Algorithm SHA256).Hash -ne $ngxLibraryHash) {
    throw 'NGX client library changed during build.'
}

# ---- build outputs ----

$binaryDirectory = Join-Path $BuildDirectory $Configuration
if (-not (Test-Path -LiteralPath (Join-Path $binaryDirectory "$pluginName.dll"))) {
	# Single-config generators put the output straight in the build directory.
	$binaryDirectory = $BuildDirectory
}

$dllSource = Join-Path $binaryDirectory "$pluginName.dll"
$pdbSource = Join-Path $binaryDirectory "$pluginName.pdb"
$tomlSource = Join-Path $repoRoot "Data/F4SE/Plugins/$tomlName"
$devBenchLicense = Join-Path $repoRoot 'src/DevBenchClient/DevBenchAPI.LICENSE.txt'
$vcpkgInstalled = Join-Path $BuildDirectory 'vcpkg_installed'
if ($cache -match '(?m)^VCPKG_INSTALLED_DIR:[A-Za-z]+=(.+)$') {
    $vcpkgInstalled = $Matches[1].Trim()
}
$vcpkgShare = Join-Path $vcpkgInstalled "$Triplet/share"

Assert-SourcePath $dllSource 'plugin DLL'
Assert-SourcePath $pdbSource 'plugin PDB'
$panelDestinationRelative = 'F4SE/Plugins/Fo4VRUpscaler/OpenXR'
$panelSources = @{
    'Fo4VRUpscalerOpenXR.dll' = (Join-Path $binaryDirectory 'Fo4VRUpscalerOpenXR.dll')
    'Fo4VRUpscalerOpenXR.pdb' = (Join-Path $binaryDirectory 'Fo4VRUpscalerOpenXR.pdb')
    'Fo4VRUpscalerOpenXR.json' = (Join-Path $repoRoot "Data/$panelDestinationRelative/Fo4VRUpscalerOpenXR.json")
}
foreach ($path in $panelSources.Values) { Assert-SourcePath $path 'OpenXR panel file' }
$dllVersion = (Get-Item -LiteralPath $dllSource).VersionInfo.FileVersion
if ($dllVersion -ne "$version.0") { throw "DLL version $dllVersion does not match source version $version." }
Assert-SourcePath $tomlSource 'shipped TOML'
Assert-SourcePath $devBenchLicense 'DevBenchAPI licence'
Assert-SourcePath $vcpkgShare 'vcpkg installed tree (build the project before packaging)'
$documents = @('LICENSE', 'EXCEPTIONS.md', 'THIRD_PARTY.md', 'README.md',
    'KNOWN_ISSUES.md', 'RELEASE_NOTES.md')
foreach ($document in $documents) {
    Assert-SourcePath (Join-Path $repoRoot $document) 'package document'
}

# ---- an NGX-linked build may not ship until its disposition is recorded ----
#
# This is the normal case since the default build carries the backend, so the check
# looks for the component row's own heading rather than the three letters "NGX", which
# any passing mention of the option would satisfy.

if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
	$cache = (Get-Content -LiteralPath $cachePath -Raw).Replace("`r`n", "`n")
	$ngxLinked = $cache -match '(?m)^UPSCALER_ENABLE_NGX:BOOL=ON$'
	if ($ngxLinked) {
		$thirdParty = Get-Content -LiteralPath (Join-Path $repoRoot 'THIRD_PARTY.md') -Raw
		if ($thirdParty -notmatch 'NVIDIA NGX D3D11 client library') {
			throw @"
This build statically links the NVIDIA NGX SDK, and THIRD_PARTY.md has no row for it.
Statically linking vendor object code into a shipped DLL is a redistribution decision.
Record the component, the clause that permits it and the shipped disposition first.
"@
		}
	}
}

# ---- stage ----

$outputDirectory = Join-Path $repoRoot 'dist'
$label = if ($CandidateLabel) { "-$CandidateLabel" } else { '' }
$archiveStem = "${pluginName}_${version}${label}_$($commit.Substring(0, 12))"
$stagingRoot = [IO.Path]::GetFullPath((Join-Path $outputDirectory "$archiveStem.stage"))
$archivePath = Join-Path $outputDirectory "$archiveStem.7z"
$outputRoot = [IO.Path]::GetFullPath($outputDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $stagingRoot.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The staging path escaped the output directory.'
}
if ((Test-Path -LiteralPath $archivePath) -or (Test-Path -LiteralPath $stagingRoot)) {
    throw 'This candidate output already exists. Preserve it and use a new candidate identity.'
}
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

$pluginDestination = Join-Path $stagingRoot 'F4SE/Plugins'
$licenseDestination = Join-Path $stagingRoot 'licenses'
New-Item -ItemType Directory -Force -Path $pluginDestination | Out-Null
New-Item -ItemType Directory -Force -Path $licenseDestination | Out-Null

Copy-Item -LiteralPath $dllSource -Destination $pluginDestination
Copy-Item -LiteralPath $pdbSource -Destination $pluginDestination
Copy-Item -LiteralPath $tomlSource -Destination $pluginDestination
$panelDestination = Join-Path $stagingRoot $panelDestinationRelative
New-Item -ItemType Directory -Force -Path $panelDestination | Out-Null
foreach ($name in $panelSources.Keys) { Copy-Item -LiteralPath $panelSources[$name] -Destination (Join-Path $panelDestination $name) }
foreach ($document in $documents) {
    $documentTarget = Join-Path $stagingRoot $document
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $documentTarget) | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot $document) -Destination $documentTarget
}

# ---- third-party notices, collected rather than curated ----

Copy-Item -LiteralPath $devBenchLicense -Destination (Join-Path $licenseDestination 'DevBenchAPI.txt')

$collected = @('DevBenchAPI  -  src/DevBenchClient, MIT, vendored in this repository')
$openXrLicense = Join-Path $repoRoot 'external/openxr/LICENSE.txt'
Assert-SourcePath $openXrLicense 'OpenXR MIT licence'
$openXrNotice = "Copyright 2017-2024 The Khronos Group Inc.`nMIT option selected for OpenXR SDK 1.1.36 headers.`n`n" + [IO.File]::ReadAllText($openXrLicense)
[IO.File]::WriteAllText((Join-Path $licenseDestination 'OpenXR.txt'), $openXrNotice)
$collected += 'OpenXR SDK headers  -  external/openxr, MIT option; no loader redistributed'
$commonLibRoot = Join-Path $repoRoot 'external/CommonLibF4'
if ($cache -match '(?m)^CommonLibPath:[A-Za-z]+=(.+)$') {
    $commonLibRoot = $Matches[1].Trim()
}
$commonLibNotice = Join-Path $commonLibRoot 'LICENSE'
Assert-SourcePath $commonLibNotice 'CommonLibF4 notice'
Copy-Item -LiteralPath $commonLibNotice -Destination (Join-Path $licenseDestination 'CommonLibF4.txt')
$collected += 'CommonLibF4  -  configured CommonLibPath, MIT'
$rcasHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'src/Upscaler/SharpenShader.h')
$rcasNotice = [Collections.Generic.List[string]]::new()
$insideRcasNotice = $false
foreach ($line in $rcasHeader) {
    if ($line -match '^//   Copyright \(c\) 2023 Advanced Micro Devices, Inc\.$') { $insideRcasNotice = $true }
    if ($insideRcasNotice) { $rcasNotice.Add(($line -replace '^//   ?', '')) }
    if ($insideRcasNotice -and $line -match 'OTHER DEALINGS IN THE SOFTWARE\.$') { break }
}
if ($rcasNotice.Count -lt 15 -or $rcasNotice[-1] -notmatch 'OTHER DEALINGS IN THE SOFTWARE\.$') {
    throw 'The complete AMD RCAS notice could not be extracted.'
}
Set-Content -LiteralPath (Join-Path $licenseDestination 'AMD-RCAS.txt') -Value $rcasNotice -Encoding UTF8
$collected += 'AMD RCAS  -  notice reproduced from src/Upscaler/SharpenShader.h, MIT'
# Dear ImGui compiles its default font, ProggyClean, into the DLL from imgui_draw.cpp,
# and the panel draws with it. imgui_draw.cpp carries only the font's copyright line and
# points at the font's own MIT licence, so that licence is the notice, reproduced here.
$proggyCleanNotice = @'
ProggyClean.ttf, the default font compiled into Dear ImGui (imgui_draw.cpp).

MIT License

Copyright (c) 2004, 2005 Tristan Grimmer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
'@
[IO.File]::WriteAllText((Join-Path $licenseDestination 'ProggyClean.txt'), $proggyCleanNotice.Replace("`r`n", "`n") + "`n")
$collected += 'ProggyClean font  -  compiled into the DLL through Dear ImGui, MIT'
if ($ngxLinked) {
    if ($cache -notmatch '(?m)^UPSCALER_NGX_SDK_DIR:PATH=(.+)$') { throw 'NGX SDK path is missing.' }
    $ngxNotice = Join-Path $Matches[1].Trim() 'nvngx_dlss.license.txt'
    Assert-SourcePath $ngxNotice 'NVIDIA NGX notice'
    # split-release.ps1 ships this same file beside the bundled runtime and checks that the
    # two copies agree, so the pin is held here as well as there.
    if ((Get-FileHash -LiteralPath $ngxNotice -Algorithm SHA256).Hash -ne '3027F23CA5A46DD9CB8183FBD522983A86F64D7DAAC5982912BF9F214671F294') {
        throw "The staged NVIDIA licence does not match the recorded pin: $ngxNotice"
    }
    Copy-Item -LiteralPath $ngxNotice -Destination (Join-Path $licenseDestination 'NVIDIA-NGX.txt')
    $collected += 'NVIDIA NGX client library  -  staged RTX SDK licence and supplement'
}
$ports = @(Get-ChildItem -LiteralPath $vcpkgShare -Directory | Sort-Object Name)
foreach ($port in $ports) {
	$copyright = Join-Path $port.FullName 'copyright'
	if (-not (Test-Path -LiteralPath $copyright -PathType Leaf)) { continue }
	Copy-Item -LiteralPath $copyright -Destination (Join-Path $licenseDestination "$($port.Name).txt")
	$collected += "$($port.Name)  -  vcpkg port, $Triplet"
}

# A build whose vcpkg tree yielded no notices is a build whose notices are wrong.
$portManifest = Get-Content -LiteralPath (Join-Path $repoRoot 'vcpkg.json') -Raw | ConvertFrom-Json
$expectedPorts = @($portManifest.dependencies | ForEach-Object {
    if ($_ -is [string]) { $_ } else { $_.name }
}) + @('fmt')
foreach ($expected in $expectedPorts) {
	if (-not (Test-Path -LiteralPath (Join-Path $licenseDestination "$expected.txt") -PathType Leaf)) {
		throw "No notice text was collected for '$expected'. Expected it at $vcpkgShare/$expected/copyright."
	}
}

$indexText = @"
Third-party notices for BlurAway VR $version.

This directory contains notices from the configured build dependency tree and
linked components, reproduced from their original licence files.
THIRD_PARTY.md at the archive root records which component is which and why it is
there; this directory is the notice text itself.

The list is collected from the build, not hand-maintained, so it includes ports
that vcpkg installed for the build even where only some of their code is linked.
Over-including a notice is not a problem; omitting one is.

$($collected -join [Environment]::NewLine)
"@
Set-Content -LiteralPath (Join-Path $licenseDestination 'INDEX.txt') -Value $indexText -Encoding UTF8

# ---- the offer ----

$builtOn = [DateTime]::UtcNow.ToString('yyyy-MM-dd')
$sourceText = @"
BlurAway VR $version (fo4vr_upscaler)
Archive built $builtOn (UTC) from commit $commit.

This archive conveys the program in object-code form. The Corresponding Source for
this exact build is the tree at that commit:

    $repositoryUrl
    git clone $repositoryUrl
    git checkout $commit

Written offer, GNU General Public License version 3 section 6(b): for at least three
years from the date this archive was distributed, and for as long as this project
offers spare parts or customer support for this product, the distributor will give
anyone who possesses the object code, for a price no more than the cost of physically
performing the distribution, a complete machine-readable copy of the Corresponding
Source for the software in this archive, on a durable physical medium customarily
used for software interchange. Requests go to:

    $Contact

BlurAway VR is licensed GPL-3.0-or-later with the additional permissions recorded
in EXCEPTIONS.md. LICENSE carries the full licence text. THIRD_PARTY.md records every
third-party component and licenses/ carries their notices verbatim.
"@
Set-Content -LiteralPath (Join-Path $stagingRoot 'SOURCE.txt') -Value $sourceText -Encoding UTF8

# ---- assert, then archive ----

$requiredFiles = @(
	"F4SE/Plugins/$pluginName.dll",
	"F4SE/Plugins/$pluginName.pdb",
	"F4SE/Plugins/$tomlName",
	'LICENSE',
	'EXCEPTIONS.md',
	'THIRD_PARTY.md',
	'SOURCE.txt',
	'licenses/INDEX.txt',
	'licenses/DevBenchAPI.txt',
	'licenses/CommonLibF4.txt',
	'licenses/AMD-RCAS.txt',
	'licenses/ProggyClean.txt'
)
$requiredFiles += $documents
$requiredFiles += 'licenses/OpenXR.txt'
$requiredFiles += @($panelSources.Keys | ForEach-Object { "$panelDestinationRelative/$_" })
if ($ngxLinked) { $requiredFiles += 'licenses/NVIDIA-NGX.txt' }
foreach ($relativePath in $requiredFiles) {
	Assert-PackageFile $relativePath
}

# No vendor runtime leaves in an archive. The patterns mirror .gitignore's.
$forbidden = @(Get-ChildItem -LiteralPath $stagingRoot -Recurse -File -Force |
	Where-Object {
		$_.Name -like 'nvngx*.dll' -or
		$_.Name -like 'sl.*.dll' -or
		$_.Name -like 'amd_fidelityfx_*.dll' -or
		$_.Name -like 'ffx_*.dll' -or
		$_.Name -like 'libxess*.dll' -or
		$_.Name -eq '.gitkeep'
	})
if ($forbidden.Count -gt 0) {
	throw "The staging tree holds files that must not ship: $($forbidden.FullName -join ', ')"
}

$fileHashes = [ordered]@{}
foreach ($file in (Get-ChildItem -LiteralPath $stagingRoot -Recurse -File | Sort-Object FullName)) {
    $relative = $file.FullName.Substring($stagingRoot.Length + 1).Replace('\', '/')
    $fileHashes[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
}
$identity = [ordered]@{
    version = $version
    candidate = $CandidateLabel
    sourceCommit = $commit
    rendererBaseline = $AcceptedRendererCommit
    dllVersion = $dllVersion
    ngxClientLibrarySha256 = $ngxLibraryHash
    builtUtc = [DateTime]::UtcNow.ToString('o')
    configuration = $Configuration
    backend = $(if ($ngxLinked) { 'ngx' } else { 'none' })
    files = $fileHashes
}
$identity | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stagingRoot 'BUILD.json') -Encoding UTF8

Push-Location $stagingRoot
try {
	& cmake -E tar cf $archivePath --format=7zip -- .
	if ($LASTEXITCODE -ne 0) {
		throw "cmake -E tar failed with exit code $LASTEXITCODE."
	}
} finally {
	Pop-Location
}

# The sweep above proves the staging tree; this proves the artefact that actually
# leaves. They can only disagree if something enters the archive that was never staged,
# and "the packaged zip contains no vendor DLL" is a claim about the archive, so the
# archive is what gets read back. Directories are listed with a trailing slash, which
# no pattern below matches.
$listing = @(& cmake -E tar tf $archivePath)
if ($LASTEXITCODE -ne 0) {
	throw "cmake -E tar tf failed with exit code $LASTEXITCODE, so the archive could not be verified."
}
$leaked = @($listing | Where-Object {
		$leaf = Split-Path -Leaf $_.Trim()
		$leaf -like 'nvngx*.dll' -or
		$leaf -like 'sl.*.dll' -or
		$leaf -like 'amd_fidelityfx_*.dll' -or
		$leaf -like 'ffx_*.dll' -or
		$leaf -like 'libxess*.dll'
	})
if ($leaked.Count -gt 0) {
	throw "The archive carries vendor runtime files: $($leaked -join ', ')"
}
foreach ($required in @($requiredFiles) + @('BUILD.json')) {
    if (@($listing | Where-Object { ($_.Trim() -replace '^\./', '') -eq $required }).Count -ne 1) {
        throw "Required archive entry missing or duplicated: $required"
    }
}
$verifyRoot = [IO.Path]::GetFullPath((Join-Path $outputDirectory "$archiveStem.verify"))
if (-not $verifyRoot.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $verifyRoot)) { throw 'Unsafe or existing archive verification directory.' }
New-Item -ItemType Directory -Path $verifyRoot | Out-Null
Push-Location $verifyRoot
try {
    & cmake -E tar xf $archivePath
    if ($LASTEXITCODE -ne 0) { throw 'Finished archive could not be extracted.' }
} finally { Pop-Location }
$readIdentity = Get-Content -LiteralPath (Join-Path $verifyRoot 'BUILD.json') -Raw | ConvertFrom-Json
if ($readIdentity.sourceCommit -ne $commit -or $readIdentity.candidate -ne $CandidateLabel -or
    $readIdentity.version -ne $version) { throw 'Extracted build identity disagrees.' }
$extractedFiles = @(Get-ChildItem -LiteralPath $verifyRoot -Recurse -File)
$expectedNames = @($readIdentity.files.PSObject.Properties.Name) + @('BUILD.json')
$actualNames = @($extractedFiles | ForEach-Object { $_.FullName.Substring($verifyRoot.Length + 1).Replace('\', '/') })
if (@(Compare-Object $expectedNames $actualNames).Count -ne 0) { throw 'Extracted file set differs from BUILD.json.' }
foreach ($property in $readIdentity.files.PSObject.Properties) {
    $verifyFile = [IO.Path]::GetFullPath((Join-Path $verifyRoot $property.Name))
    if (-not $verifyFile.StartsWith($verifyRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Manifest path escaped archive verification directory.'
    }
    if ((Get-FileHash -LiteralPath $verifyFile -Algorithm SHA256).Hash -ne $property.Value) {
        throw "Extracted file hash mismatch: $($property.Name)"
    }
}
$allowedDllPaths = @("F4SE/Plugins/$pluginName.dll", "$panelDestinationRelative/Fo4VRUpscalerOpenXR.dll")
$unexpectedDlls = @($extractedFiles | Where-Object {
    $_.Extension -eq '.dll' -and
    $_.FullName.Substring($verifyRoot.Length + 1).Replace('\', '/') -notin $allowedDllPaths
})
if ($unexpectedDlls.Count -ne 0) { throw 'Unexpected DLL in extracted package.' }
if ((Get-FileHash -LiteralPath (Join-Path $verifyRoot 'BUILD.json') -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $stagingRoot 'BUILD.json') -Algorithm SHA256).Hash) {
    throw 'Extracted manifest changed.'
}
$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
Set-Content -LiteralPath "$archivePath.sha256" -Value "$archiveHash  $([IO.Path]::GetFileName($archivePath))" -Encoding ASCII
Write-Host "Full archive verified: $($listing.Count) entries, no vendor runtime, SHA256 $archiveHash."

if (-not $KeepStaging) {
	if (-not $stagingRoot.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe staging cleanup path.' }
	if (-not $verifyRoot.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe verification cleanup path.' }
	Remove-Item -LiteralPath $verifyRoot -Recurse -Force
	Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}

Write-Host "Packaged $pluginName $version from commit $commit."
Write-Host "  $archivePath"
$archivePath

# Publishable layout: runtime files plus notices, and a separate documentation ZIP. For an
# NGX build split-release.ps1 also adds NVIDIA's pinned DLSS runtime to the mod archive,
# from .sdk-cache/streamline by default.
& (Join-Path $PSScriptRoot 'split-release.ps1') -SourceArchive $archivePath -SourceArchiveSha256 $archiveHash -OutputDirectory $outputDirectory
