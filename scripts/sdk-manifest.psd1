# Pinned vendor SDK and runtime archives. One entry, added 2026-08-29 after the vendor's
# own licence text was read out of the vendor's own archive and quoted in THIRD_PARTY.md.
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
				# Headers and the import library the build links against. Nothing here
				# is a runtime and nothing here is shipped.
				'include/sl.h'
				'include/sl_consts.h'
				'include/sl_dlss.h'
				'include/sl_helpers.h'
				'include/sl_hooks.h'
				'include/sl_result.h'
				'include/sl_security.h'
				'include/sl_version.h'
				'lib/x64/sl.interposer.lib'
			)
			Licenses    = @(
				# MIT, covering the framework source. Read on this project: it is a
				# verbatim MIT licence, "Copyright (c) 2023 NVIDIA CORPORATION".
				'license.txt'
				# The NVIDIA RTX SDKs License, covering the DLSS runtime. Staged even
				# though no runtime is staged from this entry, because the header set
				# is what an integration is built against and the terms travel with it.
				'bin/x64/nvngx_dlss.license.txt'
				'3rd-party-licenses.md'
			)
			Notes       = @'
Streamline 2.12.0, published 2026-06-23, the vendor's own single release asset,
231,958,617 bytes. Digest computed here from that download.

Headers and the import library only. No runtime DLL is staged by this entry, and
none is shipped: THIRD_PARTY.md keeps every NVIDIA runtime user-supplied, and
flipping that is a decision-log entry, not an edit here.

Two traps this entry is shaped around.

bin/x64/ and bin/x64/development/ carry the same file names - production and
watermarked development builds of every DLL. Any future entry that stages a
runtime spells the full archive-relative path, never the leaf.

The archive states a licence for exactly two binaries, nvngx_dlss and nis. What
covers NVIDIA's prebuilt signed sl.*.dll is OPEN, which is a second reason this
entry stakes nothing on them: the framework source is MIT and can be built here
if it ever needs to ship.

Read THIRD_PARTY.md's clause-by-clause notes before bumping this. In particular
RTX SDKs License 4(e), which our Linking Exception exists to satisfy, and
Supplement 4, which requires notifying NVIDIA before release.
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
