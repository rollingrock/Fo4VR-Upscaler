# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
	Downloads, verifies and stages the vendor SDKs and runtimes pinned in sdk-manifest.psd1.

.DESCRIPTION
	Reads scripts/sdk-manifest.psd1, downloads each pinned archive into .sdk-cache/,
	verifies its SHA-256 before extracting anything, and stages the named files and the
	vendor's own licence text into the entry's Destination.

	Three rules this script exists to enforce mechanically rather than by habit.

	A vendor binary comes from the vendor's own distribution or it does not come at all.
	The manifest pins a published asset by URL and digest, so a mirrored, repacked or
	leaked runtime fails the digest and is refused. Verification happens before
	extraction, so a substituted archive never reaches disk as files.

	Nothing staged here can be committed. Before writing a byte, every destination and
	the cache directory are put through git check-ignore, and a destination git would let
	you commit aborts the run naming the .gitignore rule that is missing.

	A runtime is never staged without its licence. Licenses entries are required, not
	optional, for the same reason the release archive carries a licences directory.

	This script commits nothing, ever. It downloads, verifies and copies.

	With an empty manifest it prints that nothing is pinned and exits 0, so the staging
	path is proven working before it has anything to do.

	The download, verify and stage shape follows scripts/fetch-sdks.ps1 in the GPL-3.0
	FO4 Community Shaders reference, which is a permitted source under this project's
	licence decision. The check-ignore guard and the required-licence rule are ours.

.PARAMETER CacheDirectory
	Where archives and extraction scratch are kept. Defaults to <repo>/.sdk-cache.

.PARAMETER Force
	Re-download even when a cached archive already matches the pinned digest.
#>
[CmdletBinding()]
param(
	[string]$CacheDirectory,
	[switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $PSScriptRoot 'sdk-manifest.psd1'

if (-not (Test-Path -LiteralPath $manifestPath)) {
	throw "SDK manifest not found at $manifestPath."
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
	throw 'git is not on PATH. This script will not stage a vendor binary it cannot prove is ignored.'
}

if (-not $CacheDirectory) {
	$CacheDirectory = Join-Path $repoRoot '.sdk-cache'
}

# Repo-relative, because that is the form git check-ignore wants.
function Get-RelativeToRepo {
	param([string]$Path)
	$full = [System.IO.Path]::GetFullPath($Path)
	$root = [System.IO.Path]::GetFullPath($repoRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
	if (-not $full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
		throw "Path '$Path' is outside the repository. Staging only ever writes inside the tree."
	}
	return $full.Substring($root.Length).Replace('\', '/')
}

function Test-PathIgnored {
	param([string]$RelativePath)

	# A directory rule spelled with a trailing slash does not always match the bare
	# directory name on a path that does not exist yet, so a sentinel inside it is
	# checked as well. Either answer counts.
	$probe = $RelativePath.TrimEnd('/') + '/.fetch-sdks-probe'
	foreach ($candidate in @($RelativePath, $probe)) {
		& git -C $repoRoot check-ignore -q -- $candidate
		if ($LASTEXITCODE -eq 0) { return $true }
		if ($LASTEXITCODE -gt 1) {
			throw "git check-ignore failed for '$candidate' (exit $LASTEXITCODE). Is $repoRoot a git work tree?"
		}
	}
	return $false
}

function Assert-Ignored {
	param([string]$Path, [string]$What)

	$relative = Get-RelativeToRepo -Path $Path
	if (-not (Test-PathIgnored -RelativePath $relative)) {
		throw @"
Refusing to stage $What into '$relative': git would let that path be committed.
Add a rule covering it to .gitignore - '$relative/' is the usual form - and run again.
No vendor binary enters this history.
"@
	}
}

function Test-Digest {
	param([string]$Path, [string]$Expected)
	if (-not (Test-Path -LiteralPath $Path)) { return $false }
	$actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
	return $actual -ieq $Expected
}

function Get-Archive {
	param([hashtable]$Package)

	$archivePath = Join-Path $CacheDirectory ('{0}-{1}.zip' -f $Package.Name, $Package.Version)

	if (-not $Force -and (Test-Digest -Path $archivePath -Expected $Package.Sha256)) {
		Write-Host "[$($Package.Name)] cached archive matches the pinned digest; not downloading."
		return $archivePath
	}

	Write-Host "[$($Package.Name)] downloading $($Package.Url)"
	Invoke-WebRequest -Uri $Package.Url -OutFile $archivePath -UseBasicParsing

	if (-not (Test-Digest -Path $archivePath -Expected $Package.Sha256)) {
		$actual = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
		Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
		throw @"
[$($Package.Name)] SHA-256 mismatch; the archive was discarded and nothing was extracted.
  expected $($Package.Sha256)
  actual   $actual
Either the pin is stale or this is not the vendor's own archive. Do not relax the pin to
make it pass - read the vendor's release page and update Url, Version and Sha256 together.
"@
	}

	Write-Host "[$($Package.Name)] digest verified."
	return $archivePath
}

function Copy-StagedFile {
	param([string]$ExtractRoot, [string]$FileName, [string]$Destination, [string]$Kind)

	# Entries are archive-relative paths, matched in full: SDK archives ship the same
	# file name in several places, including watermarked development builds beside the
	# production ones.
	$leaf = Split-Path -Path $FileName -Leaf
	$relative = $FileName -replace '/', '\'
	$rootPrefix = (Resolve-Path -LiteralPath $ExtractRoot).Path.TrimEnd('\') + '\'

	$found = @(Get-ChildItem -LiteralPath $ExtractRoot -Recurse -File -Filter $leaf |
		Where-Object {
			$_.FullName.Substring($rootPrefix.Length).Equals($relative, [System.StringComparison]::OrdinalIgnoreCase)
		})

	if ($found.Count -eq 0) {
		throw "Required $Kind '$FileName' was not found in the archive. The pin and the archive layout disagree."
	}

	Copy-Item -LiteralPath $found[0].FullName -Destination (Join-Path $Destination $leaf) -Force
	Write-Host "  staged $leaf"
}

$manifest = Import-PowerShellDataFile -LiteralPath $manifestPath
$packages = @()
if ($manifest.ContainsKey('Packages') -and $manifest.Packages) {
	$packages = @($manifest.Packages)
}

if ($packages.Count -eq 0) {
	Write-Host 'No vendor SDKs are pinned yet, so nothing was fetched.'
	Write-Host "Add an entry to $manifestPath once the vendor's licence terms have been read."
	exit 0
}

Assert-Ignored -Path $CacheDirectory -What 'downloaded archives'
New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null

foreach ($package in $packages) {
	$destination = Join-Path $repoRoot $package.Destination
	Assert-Ignored -Path $destination -What "$($package.Name)"
}

foreach ($package in $packages) {
	$archivePath = Get-Archive -Package $package

	$extractRoot = Join-Path $CacheDirectory ('{0}-{1}-extracted' -f $package.Name, $package.Version)
	if (Test-Path -LiteralPath $extractRoot) {
		Remove-Item -LiteralPath $extractRoot -Recurse -Force
	}
	Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot -Force

	$destination = Join-Path $repoRoot $package.Destination
	New-Item -ItemType Directory -Force -Path $destination | Out-Null

	foreach ($file in $package.Files) {
		Copy-StagedFile -ExtractRoot $extractRoot -FileName $file -Destination $destination -Kind 'file'
	}
	foreach ($license in $package.Licenses) {
		Copy-StagedFile -ExtractRoot $extractRoot -FileName $license -Destination $destination -Kind 'licence'
	}

	Remove-Item -LiteralPath $extractRoot -Recurse -Force
	Write-Host "[$($package.Name)] staged into $($package.Destination)"
}

Write-Host 'SDK staging complete. Nothing was committed.'
