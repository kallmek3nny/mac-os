# intel-handoff45 -- explicit render descriptor and WSL syntax coverage

Date: 2026-10-12. Continues intel-handoff44.md, baseline commit cbcfa70.

## Landed source changes

- Added `igGtxRenderContextDescriptor(lrca, contextId, addressMode, forceRestore)`
  in `phase0-kext/IntelGtExec.hpp`. Address mode is explicit, with no default:
  only Linux's legacy32 (1) and legacy64 (3) selectors are accepted. Privilege
  bit 8 is set; force-restore bit 2 is optional. Invalid mode, LRCA or context
  id returns zero before adding flags. RCS0 only: class and instance fields
  are zero, not a generic descriptor for BLT/BSD/VEBOX.
- Retained the existing two-argument composer for the read-only probe's
  rehearsal. It remains incomplete for real submission and is documented as
  such. No call sites were converted to hardware submission.
- `tools/check-registers.py` derives the three new fields and two mode enum
  values from the unmodified vendored intel_lrc.h, validates five new line
  citations, and requires at least eight exec citations. 290 -> 300 checks.
- Added 21 bare-literal C++ checks. The measured exec count is now 123, with
  its count table updated. Added five Python source-check tests including
  negative controls for every new constant, a missing upstream header, a
  missing enum and removed citations. Controls use a temporary tree, not edits
  to the live source.
- Fixed `test_the_shell_half_parses` in
  `tests/test_igpu_final_capture_embedded.py` without skipping or relocating
  files. Python reads the real source and feeds UTF-8 bytes to `sh -n` on stdin.
  WSL never receives the Windows drive path. Added controls for broken syntax,
  parse-not-execute, and a quoted heredoc containing a Windows path with spaces.
  All 23 tests in this module pass, including the previously failing test.

## Correction to the GGTT-only plan

The suggestion in handoff44 to choose GGTT as the simpler context mode was
not established by the vendored source. `intel_lrc.c:1521-1526` selects
INTEL_LEGACY_32B_CONTEXT or INTEL_LEGACY_64B_CONTEXT, shifts the selector by 3
and adds valid/privilege. Mode zero is not the descriptor Linux builds.

More importantly, `intel_lrc.c:901-906` resolves a GGTT VM to its PPGTT alias,
and `:942` calls init_ppgtt_regs() on that alias. `:885-898` populates PDPs or
PML4. Therefore neither selector proves that page-table initialization may be
omitted. GGTT residency of the context/ring and context addressing mode are
separate questions. No GGTT-only submission claim is made here.

## Verification

- Register gate: 300/300 passed, 0 failed, 0 skipped.
- MSVC/CMake/ctest: 27/27 suites passed; direct exec suite 123 checks, 0 failures.
- Clang shim syntax gate: passed, including unchanged IntelGtProbe.cpp.
- New source-check module: 5/5 passed, including all constant negative controls.
- Capture embedded module: 23/23 passed; WSL syntax test no longer fails.
- Full Python gate: 1438 tests in 126.791s, OK (27 skipped), after the
  payload and build card were brought into sync. The WSL syntax test ran and
  passed; it was not skipped.

No real hardware, macOS link build, boot or GPU submission was performed.

## Recovery and payload status

The first full Python result was recovered after reconnecting: 1438 tests,
one failure (the stale payload copy of IntelGtExec.hpp), 27 skips. The source
changes had survived intact. The vgfr connection was replaced by vfcSDX.
A subsequent run before the card update reported only the new image hash
missing from the card. The final run after updating the card passed.

The sixtieth payload was rebuilt twice after confirming that no VMware VM
process was running. The builds were byte-identical and verified all 38 source
files against the checkout, with no extras or host build directory. Current
ISO: 927744 bytes, md5 56da2252b3a2de80322eb764dcad4cd3, sha256 prefix
7fb6437c87db1948. Current tar: 849920 bytes, md5 f1a0f15cf0106f5875fe97a3fddf639b,
sha256 prefix ecb02565857d291b. The guest build script is unchanged at 12677
bytes. OPERATOR-ACTION-step7-build.md and the CLAUDE.md index were updated.

The previous local-only handoff attachment is superseded by this repository
copy. No macOS build, kext load, real hardware boot or GPU submission occurred.
IntelGtProbe.cpp and all of its write bans remain unchanged. The new descriptor
function is not wired to any submission path.

## Still open

1. Establish the Tiger Lake context image and PPGTT setup required by the
   selected addressing mode; the new descriptor alone is not a valid context.
2. Implement the separate opt-in bring-up class and trace format, with bounded
   forcewake polling, ownership checks, safe failure/cleanup, uncached GGTT
   mapping and staged wake/map/submit. Do not relax the probe's write bans.
3. Validate write/readback semantics per register: a doorbell or write-only
   register cannot be verified by demanding that its readback equal the write.
4. Rebuild and verify a payload matching the eventual writer, without replacing
   a mounted/live image. Host tests cannot establish a macOS kernel link or
   silicon behavior.
5. Full hardware trace and completion validation still require a real boot.

The WSL skip-versus-move decision is closed by a third approach: stdin syntax
checking retains the original test's purpose without requiring either choice.

All four verification gates are green. Changes were reviewed with git diff;
git diff --check passed. The pre-existing untracked .pygate-baseline.log and
tests/test_intel_sink_surface.exe are unrelated and excluded from the commit.
