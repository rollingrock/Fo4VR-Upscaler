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

	The PDB ships deliberately. It is what makes a Buffout crash log name our code, and
	the reporting contract in README.md depends on it.

	Every required file is asserted present before the archive is produced, the staging
	tree is swept for vendor runtime files, and the finished archive is listed back and
	swept again, so a proprietary binary cannot leave in an archive by accident. The
	NVIDIA NGX client library is inside fo4vr_upscaler.dll by design and by the licence
	row that permits it; "no vendor DLL" is a statement about loose runtime files.

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
	objects agree with NVIDIA's dynamic-CRT client library, while
	-DUPSCALER_ENABLE_NGX=OFF stays on x64-windows-static.

.PARAMETER Contact
	Where a section 6(b) source request goes. Defaults to the repository issue tracker.

.PARAMETER KeepStaging
	Leave dist/stage in place after the archive is written, for inspection.
#>
[CmdletBinding()]
param(
	[string]$BuildDirectory,
	[string]$Configuration = 'Release',
	[string]$Triplet,
	[string]$Contact,
	[switch]$KeepStaging
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
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

$commit = & git -C $repoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or -not $commit) {
	throw 'git rev-parse HEAD failed, so there is no commit to name in SOURCE.txt.'
}
$commit = "$commit".Trim()

$dirty = @(& git -C $repoRoot status --porcelain)
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
$vcpkgShare = Join-Path $BuildDirectory "vcpkg_installed/$Triplet/share"

Assert-SourcePath $dllSource 'plugin DLL'
Assert-SourcePath $pdbSource 'plugin PDB'
Assert-SourcePath $tomlSource 'shipped TOML'
Assert-SourcePath $devBenchLicense 'DevBenchAPI licence'
Assert-SourcePath $vcpkgShare 'vcpkg installed tree (build the project before packaging)'
foreach ($document in @('LICENSE', 'EXCEPTIONS.md', 'THIRD_PARTY.md')) {
	Assert-SourcePath (Join-Path $repoRoot $document) 'licence document'
}

# ---- an NGX-linked build may not ship until its disposition is recorded ----
#
# This is the normal case since the default build carries the backend, so the check
# looks for the component row's own heading rather than the three letters "NGX", which
# any passing mention of the option would satisfy.

if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
	$cache = Get-Content -LiteralPath $cachePath -Raw
	$ngxLinked = ($cache -match '(?m)^UPSCALER_NGX_SDK_DIR:PATH=(.+)$') -and $Matches[1].Trim()
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
$stagingRoot = Join-Path $outputDirectory 'stage'
$archivePath = Join-Path $outputDirectory "${pluginName}_$version.7z"

New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue

$pluginDestination = Join-Path $stagingRoot 'F4SE/Plugins'
$licenseDestination = Join-Path $stagingRoot 'licenses'
New-Item -ItemType Directory -Force -Path $pluginDestination | Out-Null
New-Item -ItemType Directory -Force -Path $licenseDestination | Out-Null

Copy-Item -LiteralPath $dllSource -Destination $pluginDestination
Copy-Item -LiteralPath $pdbSource -Destination $pluginDestination
Copy-Item -LiteralPath $tomlSource -Destination $pluginDestination
foreach ($document in @('LICENSE', 'EXCEPTIONS.md', 'THIRD_PARTY.md')) {
	Copy-Item -LiteralPath (Join-Path $repoRoot $document) -Destination $stagingRoot
}

# ---- third-party notices, collected rather than curated ----

Copy-Item -LiteralPath $devBenchLicense -Destination (Join-Path $licenseDestination 'DevBenchAPI.txt')

$collected = @('DevBenchAPI  -  src/DevBenchClient, MIT, vendored in this repository')
$ports = @(Get-ChildItem -LiteralPath $vcpkgShare -Directory | Sort-Object Name)
foreach ($port in $ports) {
	$copyright = Join-Path $port.FullName 'copyright'
	if (-not (Test-Path -LiteralPath $copyright -PathType Leaf)) { continue }
	Copy-Item -LiteralPath $copyright -Destination (Join-Path $licenseDestination "$($port.Name).txt")
	$collected += "$($port.Name)  -  vcpkg port, $Triplet"
}

# A build whose vcpkg tree yielded no notices is a build whose notices are wrong.
$expectedPorts = @('spdlog', 'tomlplusplus', 'rapidcsv', 'xbyak', 'rsm-mmio')
foreach ($expected in $expectedPorts) {
	if (-not (Test-Path -LiteralPath (Join-Path $licenseDestination "$expected.txt") -PathType Leaf)) {
		throw "No notice text was collected for '$expected'. Expected it at $vcpkgShare/$expected/copyright."
	}
}

$indexText = @"
Third-party notices for Fo4VR Upscaler $version.

Every file in this directory is the copyright and licence notice of a component
compiled into fo4vr_upscaler.dll, reproduced verbatim as its licence requires.
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
Fo4VR Upscaler $version
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

Fo4VR Upscaler is licensed GPL-3.0-or-later with the additional permissions recorded
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
	'licenses/DevBenchAPI.txt'
)
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
Write-Host "Archive verified: $($listing.Count) entries, no vendor runtime."

if (-not $KeepStaging) {
	Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}

Write-Host "Packaged $pluginName $version from commit $commit."
Write-Host "  $archivePath"
$archivePath
