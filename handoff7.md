# handoff7 — Phase 5 staging is tested, and the register gate covers it

Newest handoff. Read `handoff6.md` for the state this one continues from, and
`CLAUDE.md` for the four verification gates. Nothing here changes the project's
ceiling: Phases 0–5 are still the honest limit, the kext has still never been
compiled with Apple clang, and it has still never been loaded on hardware.

What this handoff does change is that **all four gates have now actually been
run**, in this session, and all four pass. handoff6 §"Gate status" listed two of
them as `NOT re-run`. They are re-run.

---

## The handoff6 open gap is closed

handoff6 §"The one open gap" said `NVGspStage.hpp` had no test suite and no
`add_kext_test` line. Both are now done:

- **`tests/test_gsp_stage.cpp`** — 659 lines, eight sections, `main()` calls all
  eight. Written by a crewmate against the `test_riscv_ucode.cpp` idiom
  (`gChecks`/`gFailures`, `section()`, one static blob buffer, no file I/O).
- **`tests/CMakeLists.txt:62`** — `add_kext_test(test_gsp_stage test_gsp_stage.cpp)`.

Running the binary directly, which is the only claim that survives:

```
./tests/build/test_gsp_stage.exe    142 checks, 0 failures
```

The eight sections map onto handoff6's five-item spec plus three the spec did
not ask for:

| # | Section | What it pins |
|---|---|---|
| 1 | `testWindowConstants` | the three addressing constants against each other |
| 2 | `testPadUp` | round **up**, and identity when already 256-aligned |
| 3 | `testAddressOk` | misalignment and magnitude as *separate* refusals |
| 4 | `testFillImage` | the `physAddrLo` / `physAddrHi` 32-bit split |
| 5 | `testPlanHappyPath` | synthesised GA107 geometry → expected plan |
| 6 | `testPlanRefusals` | every enum reached by its own path |
| 7 | `testPlanThenImage` | the two calls composed, as `loadGspFirmware()` uses them |
| 8 | `testResultNames` | every enum has a name, so a refusal never prints UNKNOWN |

Section 3 carries the regression test that matters most: an address in the low
256 bytes of a valid region **passes** the `LIMIT` check and would be **lost**
by a `MASK` comparison. That is the mask-as-ceiling bug, asserted rather than
described in a comment.

Sections 6 and 8 document something worth keeping: `ERR_EMPTY_BOOTLOADER` and
`ERR_REGION_OUTSIDE` are **unreachable through the public API**, because
`nvRiscvUcodeIsWellFormed()` rejects those blobs first and
`nvGspStagePlan()` returns `ERR_MALFORMED`. They are still named in section 8 —
an unreachable enum that later becomes reachable should not print `UNKNOWN` on
the day it does.

---

## `tools/check-registers.py` — the fourth header

handoff6 §4 added `NVGspBoot.hpp` as the third parsed header. `NVGspStage.hpp`
was still absent, so `NV_GSP_FW_ADDR_BITS`, `NV_GSP_FW_PHYS_MASK` and
`NV_GSP_FW_PHYS_LIMIT` were compared against **nothing**. The gate printed a
clean 109/109 while three Phase 5 constants went unverified.

Four changes, all in `tools/check-registers.py`:

### 1. `STAGE_HEADER`, merged last

`NVGspStage.hpp` includes the other three and derives its window from their
field widths, so it merges after them — same ordering rationale as
`NVGspBoot.hpp`.

### 2. `stage_sanity_checks()` — four assertions

Nothing in `NVGspStage.hpp` is transcribed from an NVIDIA header, so there is no
offset table to diff. What is checkable is the *relationship* between the three
constants:

- `NV_GSP_FW_ADDR_BITS == 41` — a **policy ceiling**, not a derived width
  (corrected 2026-08-10, t-53137f). DMATRFBASE carries physical bits 39:8 and
  DMATRFBASE1 continues the *shifted* value with physical bits 48:40, so the
  pair reaches 49. The checker now verifies the ceiling is below the pair's
  reach and above what BASE reaches alone, rather than deriving 41.
- `NV_GSP_FW_PHYS_LIMIT == (1 << bits) - 1`, **recomputed from the resolved bit
  count**, not pinned to a literal. Changing `ADDR_BITS` now moves both or fails
  loudly.
- `NV_GSP_FW_PHYS_MASK == LIMIT & ~(chunk - 1)` — the mask is the limit with the
  alignment bits cleared, not an independently-written second literal.
- **mask ≠ limit**, and they differ in *exactly* the low 256-byte bits. This is
  the allocation-constraint-vs-magnitude-ceiling confusion, made mechanical.

### 3. A second cast blind spot in `resolve()`

The composed-expression branch stripped only `(NVU8|16|32|64)` casts.
`NV_GSP_FW_PHYS_LIMIT` is written with a plain C cast:

```c
#define NV_GSP_FW_PHYS_LIMIT  ((unsigned long long)((1ULL << NV_GSP_FW_ADDR_BITS) - 1ULL))
```

So `unsigned`, `long`, `long` were each substituted as *symbolic operands*, none
resolved, and the body reported as `not found or unparseable`. Fixed with a
second `re.sub` for plain type-keyword casts.

**Note the failure mode, because it generalises.** An unresolvable body is never
reported as a *wrong* value. Here it surfaced as a FAIL and was visible. A new
constant written in an unhandled syntax, under a check that tolerates skips,
gets no verification at all and the gate still prints a clean score.

### 4. The banner lists it

The banner is the only place a reader learns *which* headers were parsed.
Leaving `NVGspStage.hpp` off it would make a clean four-header score look like a
three-header one — and a missing `NVGspStage.hpp` would silently drop the window
checks with nothing on screen saying so.

**Result: 113/113 passed** (was 109/109).

---

## Gate status as of this handoff

All four run from the project root, this session, in this order:

```
python tools/check-registers.py     113/113 passed, 0 failed, 0 skipped
tests\build-msvc.bat                EXIT=0, ctest 5/5 passed
tests\check-kext-syntax.bat         EXIT=0, all sections ok
tests\run-python-tests.bat          EXIT=0, Ran 260 tests, OK
```

The register banner now reads four headers:

```
kext headers  : NVDecode.hpp    (53 defines, 1 macros)
                NVFalcon.hpp    (177 defines, 10 macros)
                NVGspBoot.hpp   (7 defines, 0 macros)
                NVGspStage.hpp  (3 defines, 0 macros)
```

ctest is 5 targets, up from 4: `test_decode`, `test_falcon`,
`test_riscv_ucode`, `test_gsp_boot`, `test_gsp_stage`.

The kext syntax gate scans 7 sources and its structors pairing is 1/1. Its
negative controls (baseline must pass, seeded break must fail) both behaved.

---

## Things that will bite the next agent

Everything in handoff6 §"Things that will bite the next agent" still applies —
especially `ninja` from Bash failing with `cstdio not found` (run
`./tests/build-msvc.bat`, and invoke `.bat` directly, never via `cmd //c`).
Three new ones:

- **Reading a file another agent is actively writing gives you a mid-write
  snapshot that is indistinguishable from a finished-but-broken deliverable.**
  `tests/test_gsp_stage.cpp` read as 6305 bytes with an assertion-free `main()`
  — a textbook false green. Twenty minutes later the same path was 27271 bytes
  with 142 passing checks. Nothing distinguished the two states except mtime. I
  filed the false-green verdict publicly and had to retract it. Check whether
  the author is still online and whether mtime is moving, or just build and run
  it.

- **A suite that exits 0 with zero assertions is worse than no suite**, because
  once it is in ctest it reports PASS forever. `142 checks, 0 failures` is the
  claim; `ctest ... Passed` alone is not. Run the binary directly.

- **Four different agents filed the same card** for `test_gsp_stage.cpp`
  (`t-e41f80`, `t-c02bf9`, `t-3ceca1`, `t-bc30ac`) across sessions, because the
  gap was conspicuous in `handoff6.md` and nobody checked the board first.
  `t-e41f80` is the one that was worked. Check `crew task list` before filing
  against a documented gap.

---

## What is still open

- **The kext has never been compiled with Apple clang** and has never been
  loaded. `check-kext-syntax.bat` parses it with clang's front end against
  `tests/kextshim/`; that is a syntax claim, not a codegen or link claim.
- ~~**`nvGspDmaTrfBase1()`'s overlap with the shifted base is still
  UNVERIFIED**~~ — **RESOLVED 2026-08-10 (t-53137f).** There is no overlap:
  BASE1 continues the shifted value and carries physical bits 48:40, per
  `kernel_gsp_falcon_ga102.c:122-123`. The function was fixed, the comment trail
  corrected, and `NV_GSP_FW_ADDR_BITS` kept at 41 as an explicit policy ceiling.
  See `handoff5.md` section 5.
- **The macOS guest VM track is unchanged and still credential-blocked.**
  See `handoff2.md` / `handoff3.md`. It does not block driver work.
- `NVGspStage.hpp` contributes only 3 defines to the register gate. Its
  functions are covered by `test_gsp_stage.cpp`, not by `check-registers.py` —
  the two gates check different things and neither subsumes the other.
