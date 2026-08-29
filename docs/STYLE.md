# Code style

House style for Fo4VR-Upscaler. Same conventions as FRIK and True Scopes: write
like an engineer leaving notes for himself, not like someone writing an article.
Where a rule here conflicts with `CLEAN_ROOM_PROTOCOL.md`, the protocol wins and
the conflict gets flagged, not resolved silently.

## Comments

- Comments carry facts about someone else's binary that nobody can re-derive by
  reading our code. Ordinary C++ needs none; a byte offset, a bind order, or a
  reason we do something in an unnatural order needs one.
- Say what and why, not the story of how it was found. No version tags, dates,
  session references or quoted field reports - `git log` carries that.
- Sentence case. No ALL-CAPS emphasis, no exclamation marks, no emoji. `AND` and
  `OR` in caps mean the boolean operators; that carve-out is deliberate, so do
  not "fix" it.
- Density is not uniform: sparse where the code is ordinary, dense where it
  encodes an engine or vendor-SDK fact, near zero in glue, and in `Addresses.h`
  the comment is the artefact and the constant is the footnote. If you cannot say
  why a comment earns its line, cut it.
- `//` only in our own source, file headers included - a run of `//` lines is the
  file header. The one sanctioned `/* */` is the named-argument idiom at a call
  site: `Backend::Select(name, /*a_device=*/nullptr)`. Vendored third-party
  files are copied byte for byte and are exempt from every rule here; reformatting
  one breaks the diff against upstream and the notice-retention claim with it.
- Inline trailing comments are fine for a non-obvious constant or instruction.
- Delete dead code; `git log` is the reference copy. A commented-out block under
  a marker is a FRIK-era habit this repo does not inherit.

  Bad:

      // v0.2.7 - WHY v0.2.6 GOT THIS WRONG. Tester reported right-eye ghosting,
      // took three sessions to pin down that NGX wants it set at CREATION time.

  Good:

      // REPORTED (vendor documentation, not verified by us): NGX on D3D11
      // rejects a non-zero subrect base unless output subrects were enabled at
      // feature creation. Caps::supportsSubrects gates it; when false we copy
      // each eye into its own zero-origin intermediate first.

## Settings comments

Comments in `Settings.h` and in the shipped TOML double as user documentation.
One or two lines per setting: what it does, units or range if not obvious, and
the failure mode when a wrong value is quiet rather than loud - "half-magnitude
jitter looks right and samples wrong" beats "controls jitter". A setting inert on
this build names the measurement that switches it on, so nobody has to guess
whether it is broken or unbuilt. The TOML is documentation, not a changelog.

## Code

CommonLibF4 conventions. `.clang-format` and `.editorconfig` are the authority and
settle brace style, pointer alignment, include sorting and trailing-comment
spacing. Two things they will not: `ColumnLimit: 0` with comment reflow off means
nothing you write is ever rewrapped, so wrap your own comments; and
`UseTab: AlignWithSpaces` means tabs indent and spaces align, so an editor set to
spaces-only fights the file on every save.

- `a_` on parameters, `g_` on file-scope globals, `s_` on function-local statics,
  `_` on private members. `a_` and `g_` are inherited; `s_` is a tightening,
  since True Scopes prefixes about half its function-local statics and the
  unprefixed half is where a per-frame log latch hides in review.
- `k` + PascalCase on enumerators and file-scope `constexpr` constants;
  CommonLibF4 uses SCREAMING_SNAKE at namespace scope and we do not. PascalCase
  functions and types. `US_` prefix on anything we name in Ghidra.
- `#pragma once` on every header. Own header first, then system, then project.
- Internal helpers in an anonymous namespace inside the module namespace; only
  the `extern "C"` exports at global scope, or two translation units collide at
  link time on the same helper name.
- `[[nodiscard]]` wherever the return value is the point - a query, a status, or
  a bool saying whether the thing happened. Universally from day one. A
  tightening on the reference, where most headers carry none: `Install()`
  returning false unchecked is the bug it prevents.
- ASCII ` - ` as the sentence dash, in comments and log text both. A change, not
  an inheritance: True Scopes' source runs on em dashes while its docs and every
  commit message do not. One character means one grep finds all of it.
- One spelling per idiom. Exactly one `Fn<T>(rva)` and one `SafeRelease`, both in
  `Upscaler/Common.h`. Copying a two-line helper into a second file is a defect -
  the reference triplicated `Fn<T>` and its own review caught it.
- Public headers document the contract - what a function returns, what the caller
  does on failure, which thread may call it; the `.cpp` documents the mechanism.
  A header comment goes stale silently, so re-read it on every change.
- Prefer the existing patterns in the file over inventing new ones. Where this
  document and the surrounding file disagree, match the file and flag the
  conflict rather than resolving it silently.

Every hook is a struct with `static ... thunk(...)` and a comment naming the exact
call or function it replaces. A call-site hook keeps
`static inline REL::Relocation<decltype(&thunk)> func;` for the original; an
entry-replace hook has no `func` at all, because the original is never called
again, and adding a dead one is a defect rather than caution. Hook the call site
when only some callers should change - the usual case, and why 12 of 13 hooks in
True Scopes patch a `call rel32` rather than the callee, which would also have
caught callers whose behaviour must stay vanilla. Hook the entry only when every
caller including engine job threads must be intercepted, and the function is small
enough to branch over safely. Plain engine calls are a static accessor returning a
function-local `static REL::Relocation`, never a namespace-scope one.

One `F4SE::AllocTrampoline` for the whole plugin, in `F4SEPlugin_Load`, sized for
every hook. Per-hook allocation falls back to `Trampoline::create` on F4SEVR,
which releases the previous buffer and orphans every stub already written, so the
second hook crashes the game.

Anything touching engine memory is SEH-bracketed. The `__try` body is a separate
POD-only function - MSVC refuses `__try` in a frame needing C++ unwinding (C2712)
- the filter snapshots exception code, address, registers and a step breadcrumb,
and `__finally` runs what the unwind must not skip: a held lock, a refcount, a
restored engine global. A fault that says only "access violation" costs a session;
one that names the step costs a minute. A fault latches the feature off and falls
through to passthrough, never re-arming silently. Nothing is fatal after `Load`
returns either: a failed install, a missing runtime or an unreadable target each
log one line naming what degrades and what still works.

One log line per condition, rate-limited. A per-frame site needs a
`GetTickCount64` floor or a change-only predicate, and a once-per-episode latch is
re-armed by the enclosing pass, never by a single item's negative case, or a mixed
scene resets it every frame and the line spams. Log text follows the comment
rules: sentence case, no ALL-CAPS subsystem tags, no exclamation marks. A tester
cannot tell a shouting info line from a real error, and on this build the log is
the whole deliverable - it is what gets pasted into bug reports. Never emit
another plugin's warning either: probe for an optional dependency's module before
dispatching to it.

Cross-thread state is `std::atomic` with an explicit `memory_order`, cross-thread
structures are mutex-guarded, and the comment names which thread owns what. An
unexplained atomic reads as a claim that two threads touch it.

## D3D11 and COM

We are a guest on the engine's immediate context.

- One ownership spelling: raw `ID3D11*` at file scope with `g_`, created once
  behind an `Ensure*` latch, released in one place. `ComPtr` mixed into the same
  file hides which side owns the reference.
- `SafeRelease` everything the API hands back, in the block that acquired it -
  every `Get*`/`Query*`/`GetResource` adds a reference whether you wanted it or
  not. A leaked RTV per frame is invisible in a five-minute test and fatal in an
  hour of play.
- Save, draw, restore, with the stages numbered in comments so a reader can see
  none was skipped, and release every pointer the `Get*` calls returned. Null the
  SRV slots before `OMSetRenderTargets` and again before restoring - a resource
  cannot be an SRV and an RTV at once and a release runtime drops the bind
  silently - then invalidate the engine's cached state block and rebind its
  constant buffers, or artefacts appear several draw calls later in code we did
  not write. `Evaluate.h` carries the full list.
- Every `HRESULT` checked at the call, with a log line naming the call and
  `hr=0x{:08X}`. Never discovered three frames later as a null pointer.
- Cached resources carry the width, height and format they were made for and are
  rebuilt when any change. For an upscaler that is the normal case.

## Stereo

FO4VR renders both eyes into one double-wide target. Every mistake in this area
looks plausible and reads as nausea.

- Name the eye, always: `a_eye` with `Eye::kLeft` / `Eye::kRight`, never a bare
  `int` and never `bool isRight`. Where a resource is first touched, the comment
  states whether it is double-wide, an array slice, or per-eye.
- One per-eye loop, never two hand-written blocks. Values that differ - jitter
  phase, projection, motion-vector basis, subrect - are indexed by eye. Divergent
  hand-written paths are how one eye ends up a frame stale.
- Per-eye history is per-eye: one reconstruction instance, one history buffer,
  one exposure each. Shared history is one-sided ghosting, not an optimisation,
  and a reset that takes one eye and not the other is stereo rivalry - worse than
  the ghost it fixed.
- Jitter is written in exactly one place, and the comment there states the
  sequence, the phase count, and whether the divisor is per-eye or double-wide
  width.

## Provenance

Every constant describing `Fallout4VR.exe` says where it came from. This project
is built with disclosure: a constant nobody can audit cannot ship.

- `Upscaler/Addresses.h` carries the tag legend and the derivation rules in its
  own header comment, and is the single copy - do not restate them here or in
  `CLAUDE.md`. What this document adds is the review rule: a constant with no tag
  and no one-line derivation is a defect, and "read from a `DAT_` label" is not a
  derivation.
- Shared RVAs and offsets live in `Addresses.h` under one canonical `k` name; an
  RVA used by one translation unit may stay local, anything used twice is
  hoisted or the two copies drift. The `US_` prefix, the original-bytes rule and
  the tag legend are `Addresses.h`'s to state, not this file's.
- Non-obvious claims about engine or SDK behaviour carry a status: **VERIFIED**
  (we read the primary source), **REPORTED** (secondhand), **INFERRED** (our
  reasoning). Never promote one silently - an untagged claim reads as VERIFIED to
  the next reader, and that is how a guess becomes a fact.
- Borrowed ideas are named where they are used. Where a design follows a GPL-3.0
  reference we may read, the file header says what was taken: code, or only the
  shape of the idea.

## Clean room

`CLEAN_ROOM_PROTOCOL.md` is the authority; this is what it means while you type.

- Never reference a closed prior-art artefact in source, comments, commit
  messages, logs or settings. The ban covers offsets, symbol names, struct
  layouts and mechanism attributed to it. Public bug reports about it are
  requirements we chose to meet, never design input, and no comment may cite one
  as a source.
- The ban is on attributed internals, not on disclosure. The posture is
  documented independent implementation *with disclosure*, so `README.md` and
  `SOURCES.md` state what was read and what was not. Deleting that disclosure to
  satisfy the bullet above would be worse than the thing it hides.
- Convergent design is disclosed, not hidden. Where our design lands where prior
  art also landed, the comment names the external constraint that forced it - a
  vendor guide section, an engine import, a formula the SDK specifies - and
  `SOURCES.md` carries the row. A named constraint is the defence; the same code
  with no explanation is not.
- A load-time module-presence probe naming a conflicting DLL by filename is a
  coexistence check, not reverse engineering, and is permitted. It states
  incompatibility and declines; it never attributes behaviour. That is the only
  place a competing plugin's filename appears.

## Vendor SDKs

- No vendor SDK type crosses the backend seam. `NVSDK_NGX_*`, `sl::*` and `Ffx*`
  each appear in exactly one `.cpp`; grepping a vendor prefix must return one
  file. The interface speaks our vocabulary - colour, depth, motion vectors,
  jitter, extents.
- The backend library links no engine code. If a backend reaches for
  `RE::BSGraphics` the link fails, and the layering violation is a build error
  rather than a review comment. No vendor binaries, headers or shader blobs are
  committed either; `THIRD_PARTY.md` says where each comes from and whether it
  ships, and the tree must build green with no vendor SDK staged.
- Vendor runtimes are loaded by explicit path, never by search order, and the
  resolved path and reported version are logged - otherwise a stray copy on
  `PATH` quietly wins a bug report. A missing runtime, driver or GPU is a
  supported configuration: report unavailable with a reason, log it, fall
  through. Never a hard failure, never a message box.
- Presets and mode names are open strings passed through, never a compile-time
  enum. Vendors add and remove presets faster than we ship, and a fixed list is
  how an integration gets stuck one preset behind.

## Licence headers

- The project is `GPL-3.0-or-later` with the Modding and Linking Exceptions;
  `LICENSE` and `EXCEPTIONS.md` are authoritative. No per-file header on our own
  source: the root licence covers it, and boilerplate buries the file header that
  says what the file does.
- A file whose licence differs carries `// SPDX-License-Identifier: <id>` as line
  1, a short note saying why it differs and what it may be used for, and a
  `<Name>.LICENSE.txt` beside it. The vendored devbench glue is the precedent.
- A file derived from a GPL-3.0 reference we read carries an SPDX line, both
  copyrights, and a `Derived from <repo>` line. That header on one file is
  evidence; on all of them it is wallpaper.
- Third-party code is never re-licensed by copying. If its licence cannot travel
  beside it, it is not vendored.

## Commit messages

Subject: `v<major>.<minor>.<patch>: <lowercase what changed> - <the memorable
hook>`. Lowercase after the colon, ASCII hyphen, roughly 40-80 characters; the
hook half is optional. Non-release commits drop the version for a topic prefix:
`Addresses: MV target identified, [GHIDRA] tagged`.

Body, hard-wrapped at 72 columns, in three movements: the provoking observation
first and quantified where it can be, numbers rather than adjectives; a diagnosis
paragraph naming the mechanism with the ground truth or the formula inline; then
a `-` bulleted change list, one bullet per user-visible knob, naming the setting
with its default in parentheses and what `0`/`false` means.

Any commit introducing a technique or touching `Addresses.h` ends with a
`Source:` line naming the permitted source - a Ghidra session and the `US_`
symbol, a named GPL-3.0 reference, a vendor guide section, or "forced by the API
shape". The protocol requires that trail and the log is the cheapest place for it.

No AI-attribution trailers. No `Co-Authored-By`, no "Generated with". Ever.

## For AI-assisted sessions

Read this file before writing a comment. The default AI register - narrating the
session, tagging versions, quoting the report that prompted the change,
emphasising in caps - is exactly what this file exists to prevent. Match the
surrounding code's voice, not a changelog's.

Three review rejections to avoid: an engine constant with no tag and no
derivation; a claim about engine or SDK behaviour with no VERIFIED / REPORTED /
INFERRED status; and a comment that explains what the code plainly says.
