# Pinned vendor SDK and runtime archives. Deliberately empty: no vendor licence text has
# been read by this project yet, and entering a URL before reading the terms is the wrong
# order. An entry may be added only once the vendor's redistribution clause has been read
# and its disposition recorded in THIRD_PARTY.md.
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
