# Pinned vendor SDK and runtime archives. One entry, added 2026-08-29 after the vendor's
# own licence text was read out of the vendor's own archive.
# An entry may be added only once that has happened - entering a URL before reading the
# terms is the wrong order.
#
# The rule this file enforces: a vendor binary comes from the vendor's own distribution or
# it does not come at all. A mirrored, repacked or leaked runtime is refused. Pinning a
# published release asset by URL and SHA-256 is the operational form of that rule - it
# forecloses the temptation mechanically rather than by good intentions.
#
# Schema, one hashtable per entry:
#
#   Name         Short identifier. Names the cache file and every log line.
#   Version      The vendor's own version string for the archive.
#   Url          The vendor's published download. Not a mirror, not a re-host.
#   Sha256       Digest of the archive as the vendor publishes it. Verified before
#                anything is extracted, so a substituted archive never reaches disk
#                as files.
#   Files        Archive-relative paths to stage. A missing one aborts the run.
#   Licenses     Archive-relative paths to the vendor's licence text. Also required:
#                a runtime staged without its licence is a licence violation waiting
#                to be packaged.
#   Destination  Repo-relative directory the files land in. Load-bearing: fetch-sdks.ps1
#                puts it through git check-ignore before writing a byte and refuses a
#                destination git would let you commit. Two are ignored today -
#                '.sdk-cache/<name>' for headers and import libraries the build links
#                against, and 'Data/F4SE/Plugins/Fo4VRUpscaler' for runtime DLLs the
#                shipped plugin loads.
#   Notes        Why this pin, and what a future reader needs before bumping it.
#
# One trap worth carrying: at least one vendor archive ships both production and
# watermarked development builds of the same DLLs, under sibling directories with
# identical file names. Stage the production ones, and spell the full archive-relative
# path rather than the leaf.

@{
	Packages = @(
		@{
			Name        = 'Streamline'
			Version     = 'v2.12.0'
			Url         = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.12.0/streamline-sdk-v2.12.0.zip'
			Sha256      = 'f5c0a3d870707dddc3570fb4bcd3655cf48a8a68c3a9d342910cfa21b77dcf48'
			Destination = '.sdk-cache/streamline'
			Files       = @(
				# The whole public header set. sl.h is an umbrella that includes
				# sl_struct, sl_core_api and sl_core_types, so a partial set does not
				# compile - staging the lot is simpler than tracking the closure.
				'include/sl.h'
				'include/sl_appidentity.h'
				'include/sl_consts.h'
				'include/sl_core_api.h'
				'include/sl_core_types.h'
				'include/sl_deepdvc.h'
				'include/sl_device_wrappers.h'
				'include/sl_directsr.h'
				'include/sl_dlss.h'
				'include/sl_dlss_d.h'
				'include/sl_dlss_g.h'
				'include/sl_helpers.h'
				'include/sl_helpers_vk.h'
				'include/sl_hooks.h'
				'include/sl_matrix_helpers.h'
				'include/sl_nis.h'
				'include/sl_nvperf.h'
				'include/sl_pcl.h'
				'include/sl_reflex.h'
				'include/sl_result.h'
				'include/sl_security.h'
				'include/sl_struct.h'
				'include/sl_template.h'
				'include/sl_version.h'
				'lib/x64/sl.interposer.lib'

				# Raw NGX D3D11 integration. These four headers are the complete
				# include closure for nvsdk_ngx_helpers.h; the client library is the
				# matching Windows x64 build from the same official archive.
				'external/ngx-sdk/include/nvsdk_ngx.h'
				'external/ngx-sdk/include/nvsdk_ngx_defs.h'
				'external/ngx-sdk/include/nvsdk_ngx_helpers.h'
				'external/ngx-sdk/include/nvsdk_ngx_params.h'
				'external/ngx-sdk/lib/Windows_x86_64/nvsdk_ngx_d.lib'

				# Runtime, staged into the git-ignored cache. nvngx_dlss.dll and its
				# licence are a release input: split-release.ps1 copies them from here
				# into the mod archive, unmodified, after checking them against the pins
				# it carries. The three sl.*.dll are for tools/streamline-probe only, so
				# the probe can run on this machine, and never ship.
				#
				# These are the PRODUCTION builds. bin/x64/development/ carries the same
				# file names with watermarked development builds - a 70 MB nvngx_dlss.dll
				# against this 59 MB one - which is why the full archive-relative path is
				# spelled out rather than the leaf.
				'bin/x64/sl.interposer.dll'
				'bin/x64/sl.common.dll'
				'bin/x64/sl.dlss.dll'
				'bin/x64/nvngx_dlss.dll'
			)
			Licenses    = @(
				# MIT, covering the framework source. Read on this project: it is a
				# verbatim MIT licence, "Copyright (c) 2023 NVIDIA CORPORATION".
				'license.txt'
				# The NVIDIA RTX SDKs License, covering the DLSS runtime and the NGX
				# client library. split-release.ps1 ships it beside nvngx_dlss.dll, and
				# package-release.ps1 reproduces it in the notices as NVIDIA-NGX.txt.
				'bin/x64/nvngx_dlss.license.txt'
				'3rd-party-licenses.md'
			)
			Notes       = @'
Streamline 2.12.0, published 2026-06-23, the vendor's own single release asset,
231,958,617 bytes. Digest computed here from that download.

Headers, the import library, and four runtime DLLs staged into the git-ignored
cache. nvsdk_ngx.h and nvsdk_ngx_d.lib are inputs to the default plugin build
(UPSCALER_ENABLE_NGX is ON), whose client object code ships inside
fo4vr_upscaler.dll. bin/x64/nvngx_dlss.dll and bin/x64/nvngx_dlss.license.txt
are the release input for the bundled DLSS runtime: split-release.ps1 copies both
into the mod archive of an NGX build, unmodified, and refuses unless they match
the SHA-256, version, description and NVIDIA signature it pins. package-release.ps1
keeps its own full archive free of every vendor runtime. The sl.*.dll are for
tools/streamline-probe and never ship.

Two traps this entry is shaped around.

bin/x64/ and bin/x64/development/ carry the same file names - production and
watermarked development builds of every DLL. Any future entry that stages a
runtime spells the full archive-relative path, never the leaf.

The archive states a licence for exactly two binaries, nvngx_dlss and nis. What
covers NVIDIA's prebuilt signed sl.*.dll is OPEN, which is a second reason this
entry stakes nothing on them: the framework source is MIT and can be built here
if it ever needs to ship.

Before bumping this, re-read the RTX SDKs License. In particular 4(e), which our
Linking Exception exists to satisfy. A new nvngx_dlss.dll or licence also needs
new pins in split-release.ps1, and the client library pin lives in both
package-release.ps1 and split-release.ps1.
'@
		}
		# @{
		# 	Name        = 'ExampleSdk'
		# 	Version     = 'v1.0.0'
		# 	Url         = 'https://example.invalid/releases/download/v1.0.0/example-sdk-v1.0.0.zip'
		# 	Sha256      = '0000000000000000000000000000000000000000000000000000000000000000'
		# 	Destination = '.sdk-cache/example'
		# 	Files       = @(
		# 		'include/example.h'
		# 		'lib/x64/example_s.lib'
		# 	)
		# 	Licenses    = @(
		# 		'license.txt'
		# 	)
		# 	Notes       = 'Which clause permits redistribution, and the THIRD_PARTY.md row that records it.'
		# }
	)
}
