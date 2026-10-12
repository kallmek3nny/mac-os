# intel-handoff41

Intel Gen12 (Tiger Lake) track. Continues from `intel-handoff40.md`, whose
section 7a defines the 3050 "sink" -- the three things the Intel driver owes
the RTX 3050 on this muxless panel: (1) a scanout surface it owns, (2) a
plane flip to that surface, (3) a published bus address + stride.

This session found the sink family half-landed and UNCOMMITTED from a prior
session (four headers, four suites, partial gate registration, no handoff),
and finished step (b) plus the registration and payload obligations around
it. Nothing here touches hardware. `IntelPlaneProgram.hpp` composes register
VALUES; no shipping `.cpp` includes any sink header yet.

---

## 1. What landed

| Piece | State |
|---|---|
| `phase0-kext/IntelSinkSurface.hpp` | (a) validation, prior session, unchanged |
| `phase0-kext/IntelSinkAlloc.hpp` | (a) GGTT placement; `<stdint.h>` replaced by `IGS32`/`IGS64` from `IntelScanout.hpp` |
| `phase0-kext/IntelSinkPublish.hpp` | (c) descriptor, prior session, unchanged |
| `phase0-kext/IntelPlaneProgram.hpp` | (b) flip encoder; write-order table replaced by `igPlaneWriteOffset()` |
| `tests/test_intel_plane_program.cpp` | 17 -> **91** checks |
| `tests/test_intel_sink_alloc.cpp` | 16 -> **21** checks |

`test_intel_plane_program.cpp` now covers every refusal arm with a
singly-bad input and proves `out` untouched (poison fill, with a positive
control that a SUCCESS does write `out`); pins the order with doubly-bad
inputs; pins last-accepting values (stride equal to the row, a surface
ending on the last GGTT byte); decodes SIZE and POS back through
`IntelFramebuffer.hpp`; builds from the firmware CTL observed 2026-08-16 and
proves every non-owned bit carried over; and runs the real chain
`igSinkAllocPlan()` -> `igPlaneBuildLinear()` past the firmware plane
(sink lands at GGTT 0x01880000).

## 2. Findings worth keeping

1. **The stride decoder does NOT invert the linear encoder.**
   `igFbStrideBytes()` multiplies by the 128-byte unit DERIVED from the
   Y-tiled firmware plane. A linear sink uses 64. Read back through the
   existing decoder, a linear 7680-byte stride comes out 15360. Pinned in
   `testStrideDecoderAsymmetry`. Consequence for step (b) on hardware: a
   verify-after-write of a LINEAR plane must not use `igFbStrideBytes()`,
   and nobody should "fix" the encoder to 128 to make a round trip agree.
2. **Sentinel collision, safe direction.** `igPlaneEncodeSize(65536, 65536)`
   encodes to 0xFFFFFFFF, which IS `IG_FB_INVALID`, so the largest field
   value reads as a refusal. Gen12 planes stop far below 65536, so it is
   pinned, not fixed.
3. **A header-scope `static const` table breaks the kext.** Every TU that
   includes the header without using it trips `-Wunused-const-variable`
   under `-Werror`. g++ on the suite never saw it, because the suite USES
   the table; only section 1 of `tests/check-kext-syntax.bat` did.
4. **Bare `check(` counts as zero.** `CALL_RE` in
   `tests/test_check_count_floor.py` matches only `check[A-Z]...(`. Both
   new suites used a bare helper and read as 0 call sites. Renamed to
   `checkTrue`. Same class as `intel-handoff40.md` section 3: the gates read
   spellings, not intent.

## 3. Gate readings (2026-10-11, after the last code edit)

| Gate | Reading |
|---|---|
| `python tools/check-registers.py` | **260/260**, unmoved: every new constant is POLICY or DOCUMENTED |
| `tests\build-msvc.bat` | 100% passed, **25** suites (was 23) |
| `tests\check-kext-syntax.bat` | ALL SYNTAX GATES PASSED (was FAILED on both new headers) |
| `tests\run-python-tests.bat` | 1407 run; prior-session baseline had **14** failures |

Of the Python failures after the gate edits, two were payload freshness,
closed by the rebuild below (targeted re-run: 106 tests OK). One remains and
is environmental, see section 5.

Read the kext gate's VERDICT LINE, not an exit code captured in the same
`cmd` line: `%errorlevel%` is expanded at parse time, which printed `EXIT 0`
beside `SYNTAX GATE FAILED` once this session.

## 4. The payload

FIFTY SIXTH image. tar 778240 -> **808960**, ISO 856064 -> **886784**, three
10240-byte blocks each; md5s in `OPERATOR-ACTION-step7-build.md`.
Reproducible across two consecutive `tools\build-step7-payload.py --skip-gate`
runs. The fifty fifth image was already stale before this session: its copy
of `phase0-kext/IntelGtProbe.hpp` differed from the committed tree.

## 5. Still open, highest value first

1. **Wire step (b) into a shell.** Nothing calls `igPlaneBuildLinear()`.
   Next: a probe path that samples the firmware PLANE_CTL, builds the regs,
   and PUBLISHES them as properties without writing -- the same compose-then-
   publish discipline as `IntelGtProbe.cpp`, with a shape gate.
2. **The real GGTT size and reserved end** are still caller inputs to
   `igSinkAllocPlan()`. Both need a measurement from hardware before a
   `MEASURED` sink allocation exists (handoff40 7a item (a), still open).
3. **Shell-half test is environmental.** `test_the_shell_half_parses` runs
   `bash`, which on this host resolves to WSL's System32 `bash.exe`; WSL
   cannot open the `D:\` path it is handed and exits 127. The test file and
   `tools/igpu-final-capture.sh` are unchanged since the baseline commit.
   Either prefer Git Bash or translate the path; an operator decision.
4. Unchanged from `intel-handoff40.md` section 7: the five shape gates still
   spell their subject by name.
