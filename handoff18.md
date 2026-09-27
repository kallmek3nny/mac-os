# handoff18 — the terminal error nothing reached, found by set-difference

Continues from `handoff17.md`. Read that one first for the provenance gate in
`tests/test_check_count_floor.py`; this session uses it and one section here is
about paying its cost.

This session is **NVIDIA-side only, deliberately**. The operator has a separate
agent working the Intel track concurrently, so `phase0-kext/IntelDisplay.hpp`,
`phase0-kext/IntelFramebuffer.hpp`, `phase0-kext/IntelGpuId.hpp` and their three
suites under `tests/` were treated as read-only for the whole session. Nothing
below touches them. If you are resuming and the Intel track has moved, that is
somebody else's work and this handoff makes no claim about it.

Five things moved in the code. Three are in `tests/test_gsp_boot.cpp` and the
first two were found by the same one-line technique rather than by reading: a
section that reaches `NVGSP_RESULT_ERR_APERTURE_VIOLATION`, and a section that
walks the DMA XFER/DRAIN loop as a state sequence. Check count 217 to 246 to
258. The third is in `tests/test_riscv_ucode.cpp` and came from the concurrent
audit workflow §5 used to say was unharvested — 116 to 134.

The fourth is the only one that changed shipping code. `phase0-kext/NV3050Probe.cpp`
and `.hpp` gained `publishGspUnreadable()`, because the dead-read early return in
`inspectGspEngine()` was calling `publishGspState(hwcfg2, 0, 0, 0, 0, 0, 0, false)`
— publishing six registers it had never read as though they had been measured.
That is §1d, and it is gated by a new Python module rather than by any of the four
existing gates, none of which can see the class.

The fifth closes the last NVIDIA-side item this handoff's own §5 filed:
`BROM_PARAMS` had journal-index coverage and no state coverage.
`testBromParamsStateSequence()` supplies it, 258 to 276, and §1e is mostly about
what it took to *prove* the section adds reach rather than duplicating checks
that already existed — three mutations, of which only the third discriminates.

The sixth closes the §5 item directly below that one, and it is the sharpest
asymmetry in the tree: every WRITE in the state machine is pinned to an absolute
address by `checkJournal()`, so a write through the wrong engine translator
reddens instantly, while every READ discarded its address entirely.
`testReadTranslators()` records the SET of addresses the machine reached for and
pins all six read sites, 276 to 313. §1f is also the one place in this handoff
where a test of mine went RED on correct shipping code and the test was the thing
that was wrong — worth reading for that alone.

Read §1 first — it is the most serious, and §1b is its sequel. §1c is the
harvest. §1d is the only one that touched the kext. §1e and §1f are the two that
close the list.

---

## §1 — a result code produced at two sites and reached by none

`NVGspBoot.hpp` defines fourteen `NVGSP_RESULT_*` codes. `test_gsp_boot.cpp`
spelled thirteen. The missing one:

```
--- in header, NOT in suite ---
NVGSP_RESULT_ERR_APERTURE_VIOLATION
--- header total / suite total ---
14
13
```

It is produced at `NVGspBoot.hpp:365` on the read path and at the mirror site on
the write path. **Both guards existed and neither was exercised** — deleting
either `if (!nvIsInGspAperture(absolute))` block left 217 checks green.

That is the single worst place in this tree for a blind spot. The reason
`nvIsInGspAperture()` exists at all is that the RISC-V block sits exactly one
4 KiB page above the Falcon block, so an address one page low is a **valid**
address into the wrong engine. It does not fault. It reads and writes real
hardware silently. The guard is the only thing standing between a
mistranscribed base constant and corrupting an unrelated engine, and nothing
asked whether it worked.

**How it was found, and why no gate could have.** Not by reading the suite —
217 checks is far too much prose to eyeball for an absence. By set-difference:
extract every `NVGSP_RESULT_*` token from the header, extract every one from the
suite, `comm -23`. The general shape: **a gate can only fail on something it
names, so the interesting question is never "do the assertions pass" but "which
names does the file never mention."** This is the same family as handoff17 §1 —
there the table entry had no evidence behind it, here the error path had no test
in front of it, and in both cases every existing assertion was green and correct.

**The section that was added.** `testApertureGuard()`, wired into `main()` after
`testPreconditions()`. The machine cannot be steered out of the aperture through
its public API — every address it computes comes from `nvGspFalconReg()` or
`nvGspRiscvReg()` over constants — so the section drives `nvGspBootRead()` and
`nvGspBootWrite()` directly, exactly as `testPreconditions()` already does when
it nulls a callback.

Six blocks, and each earns its place:

- **Both edges of the window, and the first address past each.** `NV_PGSP_BASE`
  and `NV_PGSP_LIMIT` accepted, `BASE - 1` and `LIMIT + 1` refused. Containment
  is one-sided, so a window widened in either direction has to redden.
- **A read one page below the base.** `NV_PGSP_BASE - 0x1000` rather than a wild
  pointer, because a base constant short by a page is the realistic bug.
- **A write one past the limit.** The write guard is a separate block in the
  header from the read guard, so removing one and keeping the other must not
  leave the suite green.
- **`NO_IO` beats aperture.** With a null `write32` **and** a bad address, the
  header checks the callback first. Pinned, because the two failures send a
  reader to different files — `ERR_NO_IO` means the driver wired itself wrong,
  `APERTURE_VIOLATION` means a register constant is wrong.
- **Aperture beats writes-disabled.** The `allowWrites` gate sits *below* the
  aperture check. If a dry-run machine reported `ERR_WRITES_DISABLED` for a bad
  address, a wrong base constant would be invisible on every dry run — and a dry
  run is the only way this code has ever been executed near real silicon.
- **A negative control.** The same two calls with an in-window address must
  **succeed** and must **reach the fixture**. Without it, every check above would
  pass against a guard that refuses everything, which is a different bug wearing
  the same green.

**The separating observation, which is the part worth copying.** The fixture
callbacks in `test_gsp_boot.cpp` police the aperture themselves and set
`gFix.apertureViolation` if one ever reaches them. So a guard that fired *after*
invoking the callback would produce the right result code, the right
`faultingOffset`, the right stopped state — and on hardware would have already
performed the illegal access. `checkU32("  callback never invoked",
gFix.readCount, 0u)` is the assertion that tells those two worlds apart. When
the thing you care about is that something did **not** happen, assert on the
counter, not on the verdict.

---

## §1b — the same technique again, and a subtler answer

Running the set-difference against the other enums was the second item on this
session's own open list. It is one line each, so it was done rather than filed:

| set | header | suite | missing |
|---|---|---|---|
| `NVGSP_RESULT_*` | 14 | 13 | `ERR_APERTURE_VIOLATION` (§1) |
| `NVGSP_STATE_*` | 15 | 13 | `DMA_XFER`, `DMA_DRAIN` |
| `nvGspStage*` in `NVGspStage.hpp` | 7 | 7 | none |
| `nv*` inline accessors in `NVFalcon.hpp` | 27 | — | none |

The Stage and Falcon rows came back clean, which is worth recording precisely
because a technique that only ever produces findings is a technique nobody has
calibrated.

**The two state names needed a real answer rather than a reflex.** Unlike §1 this
was *not* total coverage loss, and I nearly filed it as one. Both states are
reached at runtime by `testDmaSingleChunk` / `MultiChunk` / `DrainPolls`, and
`nvGspBootStateName()`'s arms for both are covered by `testNames()`'s range loop
— so deleting either enumerator would not have left the suite green. The
set-difference reported a *spelling* absence, and spelling absence is a proxy for
coverage absence, not the thing itself. Check what the name's absence actually
implies before writing a test for it.

What it did imply, once checked: **no test ever observed the machine while it was
in those states.** Every DMA test calls `nvGspBootRun()` and then inspects the
journal and the counters afterwards, so all of them assert the loop's *effects*.
`testStepDriving()` — the one test that pins states as an ordered sequence — sets
`skipFirmwareLoad`, so the sequence it pins jumps `FIRMWARE_LOAD` straight to
`BROM_PARAMS` and never enters the DMA path at all.

That leaves the *shape* of the per-chunk loop unasserted. A machine that
programmed both chunks from `DMA_XFER` without passing through `DMA_DRAIN`
between them would produce the same journal, the same `chunksDone`, and the same
final state — and on silicon would land chunk N+1's offsets while chunk N was
still in flight. That is exactly the off-by-one `NVFalcon.hpp:191` warns about,
and it does not fault; it moves the wrong bytes. Effects-after-the-fact cannot
tell those two machines apart. A state sequence can.

`testDmaStateSequence()` steps a **two-chunk** image and pins the full walk:

```
PROBE RESET_ASSERT RESET_DEASSERT SCRUB_WAIT FIRMWARE_LOAD FBIF_CFG DMA_SETUP
DMA_XFER DMA_DRAIN DMA_XFER DMA_DRAIN BROM_PARAMS CORE_START ACTIVE_WAIT
```

Two chunks rather than one on purpose: one chunk walks XFER then DRAIN then
leaves, which proves the states exist but not that DRAIN *returns* to XFER. It
also samples `chunksDone` at the moment each `DMA_XFER` is entered — 0 then 1 —
because the counter is bumped by DRAIN and not by XFER, so those two readings are
what prove the second chunk was programmed *after* the first drained rather than
beside it. And it carries a negative control: a one-chunk image must enter XFER
exactly once, since an expectation list only says what it saw and never what must
not happen.

---

## §1c — the audit's one confirmed finding, re-derived and closed

§5 used to say a concurrent-audit workflow was still running when this file was
filed, and that any findings landing after it were unreviewed claims. One landed:
`phase0-kext/NVRiscvUcode.hpp:347`, kind `untested-path`, surviving three of
three independent refuters. It was re-derived against the live files before a
line was written, per that same instruction, and it held.

`nvRiscvUcodeIsWellFormed()` validates six optional regions through a
`pairs[6][2]` table at `NVRiscvUcode.hpp:335`, and each row is guarded by
`size != 0 &&`. That short-circuit is the whole defect. The real GA107
bootloader leaves all six optional sizes at zero, and `buildBlob()`
(`tests/test_riscv_ucode.cpp:91-113`) reproduces it faithfully — it writes
version, bootloader offset/size, bootloader param offset/size and fbReserved,
and nothing else. So `nvRiscvRegionInPayload()` was reached for exactly one of
the six rows: whichever one the suite happened to write a non-zero size into,
which was the manifest, once, at what was then line 535.

Five rows — riscvElf, monitorData, monitorCode, swbromCode, swbromData — had no
check in front of them at all. **A row deleted from `pairs[]` was
indistinguishable from a row present in it.**

**Verified by watching it fail, on the mutation the audit named.** Row 6 was
replaced with a duplicate of row 2 and the tree rebuilt. Against the old
116-check suite that mutation was invisible. Against the new one, ctest reports
one failure out of eight suites and the failing line is
`overrunning swbromData rejected ... true`. The header was reverted immediately
and the rebuild came back 8 of 8; nothing in `phase0-kext/` is changed by this
session.

**The fix is in the suite, not the header.** The guard is correct as written —
it does check every region whose size is non-zero, which is what its comment
claims. The dissenting refuter was right that this is test vacuity rather than a
live correctness defect: `NVGspStage.hpp:277-289` stages only the bootloader, so
no current caller feeds an unvalidated offset into
`nvRiscvRegionFileOffset()`'s unbounded addition. That is an argument about
today's callers, not about the table, so the coverage still had to exist.

The single manifest case became a loop over all six rows, three checks each:

- **The size field is inside the descriptor.** Pinned rather than reasoned about.
  `swbromData`'s size sits at `0x48`, and the GA107 descriptor is 84 bytes
  (`0x54`), so `0x48 + 4 = 0x4C ≤ 0x54` — every row is writable. Without this,
  a `putDesc()` past the end would be a silent no-op and the overrun check below
  would pass against nothing.
- **The overrun is rejected.** Offset at `NV_RISCV_GA107_BL_DATA_SIZE - 4`, size
  64.
- **A zero-size positive control.** The same bogus offset with the size returned
  to zero must be **accepted**. This is what attributes the rejection to the
  size field rather than to the offset write — an overrun check reddening for an
  unrelated reason looks identical to a working bound.

Plus one check that the table has six rows, so a row added to the header without
a row added here is not silently uncovered.

---

## §2 — the provenance gate charged its first toll, and the band would have caught one of these

handoff17 §1 added `EveryTableEntryHasProvenance`, which requires every
`MEASURED` entry in `tests/test_check_count_floor.py` to have a dated reading in
that file's own comment block. This session is the first time an entry moved
since, so it is the first time the cost was paid: **217 to 246 needed a dict edit
AND a new reading line.** A dict-only edit is now a red. That is the intended
behaviour and it worked without incident.

Worth recording for the next reader: 246 is **above** the previous entry's
ceiling (`217 + max(8, 27) = 244`) by two. So unlike the `test_intel_display`
case in handoff17 §1 — which sat four checks *inside* its band and slipped past
the upper edge entirely — this reading was due to be caught by the ceiling on its
own. The provenance class was not what caught it; it is what made recording it
unavoidable. Do not read this session as evidence that the band works, and do not
read handoff17 as evidence that it does not. They are two different-sized jumps.

The second bump in this session settles that argument in the other direction.
`246 → 258` sits **inside** the 246 band (`246 + max(8, 30) = 276`) by eighteen,
so the ceiling would not have fired on it at all. Had the provenance class not
existed, `testDmaStateSequence()` could have been added, the dict edited to
match, and nothing in the tree would have required a reading to be taken. Two
bumps, one caught by the band and one invisible to it — which is the clearest
available statement of what each mechanism is for.

The superseded 217 reading is **left standing** in the comment block on purpose.
`test_at_least_one_suite_is_actually_backed` asserts the reader finds
`test_gsp_boot 217` in the live file; its job is to prove the reader locates real
lines rather than to track the current count, and rewriting history to the newest
number would quietly turn it into a test that follows whatever you just typed.

---

## §3 — gate state

All four run from the project root after the final edit rather than a spot check
earlier in the session. **All four are GREEN in the final reading, and that is the
least informative thing §3 has to say.** Two of them were RED earlier in the
session, both went green underneath me, and I changed nothing that fixed either.
Every reading is recorded below rather than smoothed into one, because a gate
whose verdict changed without any edit of mine is exactly the reading a later
session must not trust. If you take one thing from §3: in a two-agent tree a
gate's verdict is a JOINT measurement — red does not mean you broke it and green
does not mean you fixed it.

| Gate | Verdict |
|---|---|
| `python tools/check-registers.py` | 238 of 238 passed, zero failed, zero skipped |
| `tests\build-msvc.bat` | 9 of 9 suites passed under ctest, exit 0 |
| `tests\check-kext-syntax.bat` | `VERDICT: PASS`, exit 0 — was exit 1 earlier, see below |
| `tests\run-python-tests.bat` | 885 tests, OK, exit 0 — the FIFTH reading of five |

The register gate is unchanged at 238 because no constant moved. The C++ gate is
the one that carries this session's work, and `test_gsp_boot` is 313 checks, 0
failures inside it. It reports **9** suites rather than the 8 quoted elsewhere in
this handoff because the peer added `tests/test_intel_display_names.cpp` at 19:17;
the ninth is not mine and was never built by me on purpose.

### The red gate, and the one that healed itself

Between 17:49 and 18:26 the peer landed a new IOKit shell for the Intel track —
`phase0-kext/IntelDisplayProbe.hpp` (17:49) and `.cpp` (18:19) — and edited
`phase0-kext/Info.plist` and `phase0-kext/Makefile` (both 18:26) to carry it.
My last edit to anything under `phase0-kext/` was `NV3050Probe.cpp` at 17:02,
and the three `NVGspBoot.hpp` mutations were all reverted by 18:25.

**Syntax gate, exit 1 at 18:2x.** Section 3 read:

```
=== 3. kext against the IOKit shim ===
  ok   NV3050Probe.cpp
In file included from phase0-kext/IntelDisplayProbe.cpp:16:
phase0-kext\IntelDisplayProbe.hpp:57:10: fatal error: 'IOKit/IODeviceMemory.h' file not found
  FAIL IntelDisplayProbe.cpp
```

`tests/kextshim/` had no `IOKit/IODeviceMemory.h`. That was a shim gap the Intel
track had to fill, not a defect in the new header. My shell is the `ok` line
directly above it.

The trap in that reading is worth keeping even though the failure is gone: the
TAIL of this gate's output printed `VERDICT: PASS` and `ALL SYNTAX GATES PASSED`
from a LATER checker while the process exited 1 on section 3. Only the exit code
aggregates the numbered checkers. **Read the exit code and grep for `FAIL`, never
the last verdict line.**

**Syntax gate, exit 0 in the final reading, and again not my doing.** The peer
filled the gap: `tests/kextshim/IOKit/IODeviceMemory.h` is stamped 18:43 and
`tests/kextshim/libkern/libkern.h` 18:47. Section 3 now prints `ok NV3050Probe.cpp`
and `ok IntelDisplayProbe.cpp`, section 2 reports both shells reached, and the
process exits 0 with `VERDICT: PASS` — which this time the exit code actually
agrees with. Two headers, both Intel-side, neither read nor written by me.

**Python gate — red at 18:2x, green at 18:3x, and I changed nothing in between.**
This is the part to read, because the final `885 tests, OK, exit 0` in the table
above is a true reading of a tree that was failing five tests twenty minutes
earlier, and nothing in my working set explains the difference.

The first run had 5 failures, in two groups:

- Three from `test_check_header_coverage` and `test_check_kext_personalities`,
  all saying some form of "`IntelDisplayProbe.hpp`: parsed by nothing" or
  listing `['NV3050Probe.cpp', 'IntelDisplayProbe.cpp'] != ['NV3050Probe.cpp']`.
- Two payload-freshness failures. `build-payload/kext-src.tar` is compared
  byte-for-byte against the live tree, and it was stale in exactly four places.
  Measured directly rather than inferred:

```
members 13
DIFFERS phase0-kext/Info.plist
DIFFERS phase0-kext/Makefile
---tree files missing from tar---
phase0-kext/IntelDisplayProbe.cpp
phase0-kext/IntelDisplayProbe.hpp
```

I deliberately did NOT rebuild the payload to green that. Rebuilding it would
have shipped the peer's half-landed Intel shell into the operator's ISO under my
name, and the operator card's md5 series would then record an image this session
cannot vouch for. The Intel track owns that rebuild.

The Intel track then did it. `build-payload/kext-src.tar` is stamped 18:34, and
the same diff re-run afterwards reports **15 members, zero DIFFERS, nothing
missing** — two more members than before, which are the two new Intel files. The
three header-coverage and personality failures cleared in the same window. So the
final green Python reading is real, but it is green because a peer landed work,
not because anything I did fixed it, and a later session reading only the table
would draw the wrong conclusion about who owned it.

**That same re-run is the byte-exactness proof for §1e's three mutations, and it
is a stronger one than the first.** `NVGspBoot.hpp` was mutated and reverted
three times today. In the 13-member diff it appeared in neither the DIFFERS list
nor the missing list; in the 15-member diff taken after a FRESH tar was built
from the live tree, it is byte-identical again. The first reading proves it
matches the pre-session tar; the second proves the live file the peer just
packaged is the same file. Checked by hash both times, not by eye.

The count-floor module was also run alone, to confirm the §1e provenance edit is
clean independent of all this: `Ran 16 tests — OK`.

The Python total needs splitting, because only part of it is mine. It read 854
at the start of this session and 872 after §1d — those eighteen are
`tests/test_probe_dead_read_publication.py`, measured directly by running the
module alone. It then read **884** on the next run, twelve higher, with no edit
of mine in between. Those twelve are the peer's: `find tests -name 'test_*.py'
-mmin -60` names exactly two modules, mine at 17:11 and
`tests/test_check_kext_personalities.py` at 17:40, and that module now runs 36
tests on its own. It is the test side of the `TARGET_DIE` → `TRACKS` refactor
whose half-written state is recorded below.

**854 → 872 is this session. 872 → 884 is not.** Recorded this way because a
handoff that claims a total it did not produce is the same defect as a `MEASURED`
entry somebody typed — the number is real, the attribution is not, and nothing in
the tree can tell them apart afterwards.

**Third reading, taken after §1f: RED again, and again not mine.** The gate
exited 1 and retained
`%TEMP%\macos-gpu-python-gate-2181212244.log`. The failures name
`tests/test_intel_display_names.cpp` and `phase0-kext/IntelDisplayNames.hpp`, two
files that did not exist earlier in this session — mtimes 19:17 and 19:14,
**both newer than the peer's own `build-payload/kext-src.tar` at 18:48**, which is
why the payload and header-coverage classes went red together. Attribution was
checked rather than assumed: the count-floor module run alone reports exactly one
failure and its message is `tests/test_intel_display_names.cpp exists but has no
entry in MEASURED, so nothing bounds its check count`. That is the provenance gate
working correctly on a suite that is not mine. Every occurrence of
`test_gsp_boot` in the whole log is either an `ok` line or an element inside one of
those peer-attributed list diffs; none of them is a failure.

I did **not** add a `MEASURED` entry for the peer's suite, and that refusal is
deliberate rather than lazy. The entry requires a measured check count, measuring
it requires building and running an Intel suite, and a peer's half-written file is
indistinguishable from a finished-but-broken one — so a number taken from it now
would be exactly the class of defect `test_check_count_floor.py` exists to catch.
The same reasoning is why `build-payload/` was not rebuilt: greening the gate that
way would ship the peer's half-landed work inside an image stamped with my name on
it.

**Fourth reading, minutes later: GREEN. 885 tests, exit 0.** Again with no edit of
mine. The peer closed their own
loop: `tests/test_check_count_floor.py` is stamped 19:24 and now carries a dated
2026-08-17 provenance block plus a dict entry for `test_intel_display_names` at
236, and `build-payload/kext-src.tar` and `step7-payload.iso` are both stamped
19:27. So both classes that were red at 19:2x — the missing `MEASURED` entry and
the stale payload — were fixed by the agent who created them, which is exactly the
outcome the refusal above was holding the door open for.

One thing was checked rather than assumed, because it matters: the peer edited the
SAME FILE I did. My reading and dict entry survived it — `test_gsp_boot 313 checks,
0 failures` is still in the provenance block and `"test_gsp_boot": 313` is still in
`MEASURED`, and the floor module passes with both entries present. Two agents
appending dated readings to one table is fine; two agents rewriting the same dict
entry would not be, and that is worth verifying by grep rather than hoping.

**Fifth reading, and the ONE red gate in this session that was mine.** Taken after
the §1f writeup and the CLAUDE.md edits that go with it: exit 1, three failures,
all from `test_check_documented_paths.RealRepository`. Nothing to do with the
kext or the tests — I had written path-SHAPED tokens into doc prose, and
`tools/check-documented-paths.py` cannot tell a path it must resolve from a
fragment of quoted tool output. The three tokens were `FAIL
IntelDisplayProbe.cpp` (a line of the syntax gate's output, quoted as if it were a
filename), a bare `libkern/libkern.h` (real, but only under `tests/kextshim/`),
and an angle-bracket include in a sentence about IOKit headers that by definition
do not exist in this tree.

Fixed by rewriting the prose rather than the checker: the gate's output line is
now described as "section 3's `IntelDisplayProbe.cpp` line", and the shim header
is spelled `tests/kextshim/libkern/libkern.h` so it resolves. That is the same
lesson already in memory as *a bundle-internal path reads as a repo directory* —
prose that spells something path-shaped will be resolved as a path, so describe
it in words or spell it from the repo root. After the fix the checker exits 0 on
its own and the full gate is back to **885 tests, OK, exit 0**, which is the
reading in the table above.

The generalisation for the next session, which is the real content of §3: **in a
two-agent tree the Python gate's verdict is a joint measurement.** Red does not
mean you broke something and green does not mean you fixed it. Five readings in
one session; four of the transitions were the peer's and exactly one was mine, and
the only way to tell them apart was the failure MESSAGE plus mtimes. Attribute
before touching anything, and if the message names a file that is not yours, the
correct action is to record the reading and leave it alone.

One caveat on the syntax gate, recorded because it will read as a defect
otherwise. Mid-session it exited 1 while still printing `VERDICT: PASS` on line
193 — that verdict belonged to `check-negative-controls.py`, and the real failure
was `NameError: name 'TARGET_DIE' is not defined` in
`tools/check-kext-personalities.py:265`, a peer's file caught mid-refactor
(`TARGET_DIE` → `TRACKS`, three references unconverted, mtime seventeen minutes
old alongside that day's `INTEL-DEVICE-IDS.md`). Its traceback never mentioned
`NV3050Probe`, and the clang parse of the edited shell was green in the same run.
The peer landed the refactor at 17:09 and the gate now prints both `VERDICT:
PASS` and `=== ALL SYNTAX GATES PASSED ===` at exit 0. **A verdict word is only
evidence for the checker that printed it** — this gate runs several, and reading
the last `VERDICT` line as the gate's own answer is how a red run looks green.

Measured per-suite check counts, 2026-08-17, by running each binary directly:

| suite | checks |
|---|---|
| `test_decode` | 64 |
| `test_falcon` | 169 |
| `test_riscv_ucode` | **134** |
| `test_gsp_boot` | **313** |
| `test_gsp_stage` | 181 |
| `test_intel_display` | 354 |
| `test_intel_gpu_id` | 77 |
| `test_intel_framebuffer` | 110 |

The three Intel rows are carried forward from handoff17 unchanged. They were not
re-measured here and are not mine to move.

The kext build state is unchanged from `handoff13.md`: it compiles and links
under Apple clang 17.0.0 in the guest, and it has still never been loaded.
Nothing in this session moved that.

`phase0-kext/` was touched twice, in opposite ways, and the distinction matters
to anyone reading a diff.

§1c's edit to `NVRiscvUcode.hpp` was a **mutation** — planted in the `pairs[]`
table, built, observed to redden exactly one check, and reverted. That header is
byte-identical to how this session found it. The revert was confirmed byte-exact
not by eye but by the Python gate: `build-payload/kext-src.tar` is compared
byte-for-byte against the live tree, so a near-miss revert would have reddened it
naming the header.

§1d's edit to `NV3050Probe.cpp` and `.hpp` is **permanent** — the dead-read
publisher. That is the first change to shipping kext code in three handoffs, and
it is why `build-payload/` was rebuilt and
`OPERATOR-ACTION-step7-build.md` moved to its twentieth image. The kext has not
been recompiled under Apple clang since; the change parses clean under the
clang 19 front end via `tests/kextshim/`, which is the same evidence every other
edit in this tree has before the operator builds it.

---

## §1d — the dead-read path published six registers it never read

The last two set-difference targets from §5 were run, and the second one found
something the technique was not aiming at.

**`NVDecode.hpp` against `test_decode.cpp` came back clean.** Eighteen `nv*`
tokens in the header, sixteen in the suite; the two extras are `nv_arch` and
`nv_ref` — NVIDIA header *filenames* appearing in provenance comments
(`nv_ref.h:108`, `nv_arch.h:100`), not accessors. Third clean row for the
technique, which matters for the same reason §1b's clean rows did.

**`NV3050Probe.cpp`'s published property keys are a different shape entirely.**
37 distinct keys across 40 `setProperty()` calls, and 27 of the 37 are spelled
in exactly one place in the tree — the call itself. The 10 that appear elsewhere
appear only in `phase0-kext/README.md` prose. The reverse direction is clean:
no doc names a key the code never publishes. So there is no index to go stale,
which makes this a weaker finding than the aperture one and it is filed as such.

What the enumeration *did* surface is at `NV3050Probe.cpp:699`. `NVPhase` is
written at four sites, which sent me reading the guards around each — and the
first one is the dead-read early return in `inspectGspEngine()`:

```cpp
UInt32 hwcfg2 = gspFalconRead(NV_PFALCON_FALCON_HWCFG2);
if (nvLooksLikeDeadRead(hwcfg2)) {
    NVLOG(...);
    publishGspState(hwcfg2, 0, 0, 0, 0, 0, 0, false);   // six values it never read
    return false;
}
```

HWCFG2 is the only register read before that branch. The other six arguments —
`GSP_HWCFG`, both CPUCTLs, `GSP_BCR_CTRL`, and both error-stat latches — are
literal zeros, and `publishGspState()` writes every one of them into the
IORegistry.

**Why this is the quiet kind of wrong.** `setProperty()` with a literal `0`
produces an entry **byte-identical** to one that genuinely read zero.
`GSP_BCR_CTRL = 0` is a plausible measurement, not an obviously-bogus sentinel,
so an operator in IORegistryExplorer could not tell a powered-down engine from
an engine reporting all clear — and those two readings send them to opposite
ends of `PLAN.md`. `GSPImemBytes` was worse than merely absent: it was
`nvFalconImemSizeBytes(0)`, a value **derived** from a register nobody read,
published as a byte count. `GSPRiscvCapable` is the subtler one — it decodes
`hwcfg2`, which that path *did* read, but read as a non-decoding value, so a
bit-field pulled out of it is noise wearing the shape of a capability flag.

**The fix is a dedicated publisher, not a flag.** `publishGspUnreadable(hwcfg2)`
publishes `GSP_HWCFG2` (the one real measurement), `GSPBootable false`,
`GSPEngineReadable false`, and `NVGspUnreadableReason` explaining that the rest
were never read. A parameter like `bool measured` would have kept the six
fabricated arguments at the call site, which is the thing being removed.

`GSPEngineReadable` is published on **both** paths deliberately —
`publishGspState()` sets it `true`. If it appeared only on the failure path, an
operator would have to distinguish "this kext reads the engine" from "this kext
predates the key" by its absence, which is the exact inference this fix exists
to remove.

**No existing gate could see this class**, which is why it needed a new one. The
register gate checks constants; the C++ suites cannot link the IOKit shell at
all; the clang front end is perfectly happy with a fabricated argument because
`0` is a valid `UInt32`. So the *shape* is pinned, the way
`tools/check-prepare-balance.py` pins a shape nothing else reaches:
`tests/test_probe_dead_read_publication.py`, 18 tests. Nine are negative
controls, each asserting the failure *reason* rather than merely that something
reddened — including a mutant that restores the exact deleted call, which must
trip both "branch calls publishGspState" and "bare literal zero". Every mutant
runs through the same `audit()` function as the live tree; a gate whose mutants
are checked by different code than its real input is two gates, and only one of
them has ever been seen to fail.

**One thing worth carrying forward.** The module went red on a pristine tree the
first time it ran. My own explanatory comment above `publishGspUnreadable()`
quotes the defective call verbatim, so a scanner reading raw text saw the fix's
rationale as the bug — and the only way to green it by editing the comment would
have been to delete the sentence recording why the gate exists. Fixed with a
`strip_comments()` pass that blanks comments to spaces (preserving byte offsets
so every `find()` still lines up) and leaves string literals intact, since the
property names are read out of them. **A structural gate over source must scan
code, not prose — the file most likely to describe a defect in detail is the
file that just fixed it.**

`phase0-kext/` changed for real here, so `build-payload/` was rebuilt
(`python tools/build-step7-payload.py`, 13 files byte-identical, 39 tests OK)
and `OPERATOR-ACTION-step7-build.md` records the twentieth rebuild. Sizes fixed,
hashes moved — the same shape as the fifteenth and sixteenth, and the first
entry in that series that is NVIDIA-side rather than an Intel header.

---

## §4 — things I checked myself rather than trusting a doc

Recorded because each one was a candidate defect that turned out not to be, and
re-deriving them costs a future session real time.

- **The GSP aperture edge is already pinned on both sides**, at
  `tests/test_falcon.cpp:120-121` — "limit 0x113FFF accepted" and "limit+1
  rejected". The one-sided-containment shape from
  `[[containment-is-one-sided]]` does **not** apply to `nvIsInGspAperture()`
  itself. What was missing was never the predicate's edges; it was that
  `NVGspBoot`'s two *call sites* never reached the refusal.
- **Both structural checkers are wired into a gate and have their own tests.**
  `tools/check-prepare-balance.py` at `check-kext-syntax.bat:99` and
  `tools/check-structors.py` at line 166, with `tests/test_check_prepare_balance.py`
  and `tests/test_check_structors.py` behind them. A previously suspected gap;
  it is closed.
- **`PHASE4_ALLOW_WRITES` is genuinely absent from the default build.** Opt-in
  `ifdef` at `phase0-kext/Makefile:103-104` with a loud warning at 116-117. The
  default build cannot write to the GPU.
- **The five NVIDIA suites were all current and green before any edit**, so the
  246 is a real delta rather than a stale binary finally being rebuilt.

---

## §1e — BROM_PARAMS, and three mutations of which only one discriminates

The last item on this handoff's own §5 list. `BROM_PARAMS` writes four registers
and `NVGspBoot.hpp:750` says plainly why the order is the whole assertion:
MOD_SEL is the LATCH, so writing it before the other three commits stale
parameters and the boot ROM rejects the signature. `testFullSequenceOrder()`
already pinned those four at whole-run journal indices 2..5. What it could not
pin is the WALK, for the same reason §1b gives in the other direction:

- A journal cannot see a missing state transition. Enter `BROM_PARAMS` twice,
  enter it from the wrong predecessor, or skip `CORE_START` so `ACTIVE_WAIT`
  runs against an unstarted core — the four writes still land at the same four
  indices carrying the same four values, and every `checkJournal()` is green.
- A state sequence cannot see a reordered write. The walk through the state is
  one step in and one step out either way.

`testBromParamsStateSequence()` asserts both, and the part that is genuinely new
is that the four writes are bounded to a SINGLE step. `testFullSequenceOrder()`
indexes into the journal of a whole run, where 2..5 happen to be the BROM writes
because of everything that came before. Here the claim is "this state emitted
exactly these four, in this order" — a much narrower thing to be true.

It uses `skipFirmwareLoad`, which `NVGspBoot.hpp:276` says exists for precisely
this. Part 4 is the control that makes that legitimate: the same machine with the
flag CLEAR must reach `BROM_PARAMS` by a demonstrably different route (through
the DMA states, with more than two writes behind it), and both entry edges must
land on `CORE_START`. If `DMA_DRAIN`'s exit and `FIRMWARE_LOAD`'s exit diverged
after this state, Part 1 would be describing a path hardware never takes.

**The part worth reading is the non-vacuity proof, because the first two attempts
at it were worthless and looked fine.**

| Mutation | Failures | What it actually proved |
|---|---|---|
| MOD_SEL swapped to the front of the block | 13 | Nothing about reach. Four of the thirteen were the PRE-EXISTING journal checks at lines 65-68 — the old gate already caught this. |
| `BROM_PARAMS` split across two entries | 30 | Also nothing. The extra entry made nine writes instead of seven, shifting every downstream journal index, so pre-existing checks reddened on arithmetic rather than on the walk. |
| MOD_SEL write RELOCATED into `CORE_START` | **2** | The discriminating one. Same five writes, same order, same whole-run indices, same walk, same `modSelWriteIndex`. Every pre-existing check stayed GREEN. Exactly two failed, both new: `one step emitted exactly four writes` got 3, and the fourth journal entry bounded to that step was absent. |

A mutation that reddens a lot of checks is not evidence a new test earns its
place; it is usually evidence the old tests were already sufficient. The count
going DOWN from 13 to 30 to 2 is the signal. Only the third mutation is a defect
that the tree as it stood before this section would have shipped.

`NVGspBoot.hpp` was mutated three times and reverted three times; it is
byte-identical to where it started, which the payload-freshness gate confirms
independently — it is absent from that gate's stale list (see §3).

Count 258 → 276. `MEASURED["test_gsp_boot"]` and a dated provenance reading were
both updated, as the provenance class requires. 276 sits inside the previous
entry's band (258 + max(8, 32) = 290) by fourteen, so nothing would have fired
here on its own — like 246 → 258, unlike 217 → 246 or 116 → 134.

---

## §1f — writes were pinned to an address; reads threw theirs away

This is the §5 item directly below §1e's, and the asymmetry behind it is worth
stating flatly because it is not obvious from reading either side:

- Every **write** in `NVGspBoot.hpp` is pinned to an ABSOLUTE address, because
  `checkJournal(label, index, offset, value)` compares the journalled offset
  against an `ABS_*` literal. A write that reached for `nvGspFalconReg()` where it
  wanted `nvGspRiscvReg()` reddens on the very next run, and always has.
- Every **read** discarded its address. `nvGspBootRead()` took the absolute
  address, handed it to the callback, and kept only the VALUE. Nothing in the
  suite could observe *where* a read went.

That second half is the exposure this project's own conventions section calls its
most dangerous class. The RISC-V block sits exactly one 4 KiB page above the
Falcon block, so a read one page off does not fault — it returns a plausible value
from a real register belonging to the wrong engine. And `nvIsInGspAperture()`
cannot help: `0x110000..0x113FFF` contains BOTH blocks, so the mistranslated
address is inside the window and the guard is satisfied.

The fixture now records the SET of distinct read offsets. A set rather than a
sequence, deliberately: `ACTIVE_WAIT` polls the same registers repeatedly, so read
ORDER is dominated by poll counts and says nothing, while the set of addresses is
exactly what distinguishes the two translators. The recording sits BEFORE the
aperture check in `fixtureRead32()`, so an address the machine reached for is
visible even when the read was refused.

**Read this part even if you skip the rest: my first version of the section went
RED on correct shipping code, and the test was the thing that was wrong.** It
asserted that one healthy run touches all six read sites and that the run ends
with something other than `RUNNING`. It printed 4 failures out of 297, and both
premises were false:

- `NVGSP_RESULT_RUNNING` is the SUCCESS terminal for this machine. The core came
  up. Asserting `r != NVGSP_RESULT_RUNNING` was asserting the boot FAILED.
- `ACTIVE_WAIT` stops on `RUNNING` at `NVGspBoot.hpp:805`, which is BEFORE
  `nvGspBootCheckRiscvErrors()` at `:808`. So a healthy boot can never reach the
  two error-status reads at `:400` and `:404`. Not "usually does not" — cannot,
  by construction.

So it takes two runs, and that is a fact about the state machine rather than a
testing convenience:

| Run | Setup | Read sites reached |
|---|---|---|
| A | healthy, one-chunk image | falcon CPUCTL `:514`, falcon HWCFG2 `:522`+`:558`, falcon DMATRFCMD `:730`, RISC-V CPUCTL `:800` — **four** |
| B | `skipFirmwareLoad`, core never reports ACTIVE, `maxActivePolls = 5` | RISC-V PRIV_ERR `:400`, RISC-V HUB_ERR `:404`, RISC-V CPUCTL `:800`, falcon CPUCTL `:514`, falcon HWCFG2 `:522` — **five** |

Run A carries the precedence claim as a positive statement: ACTIVE beats the error
check, so PRIV_ERR and HUB_ERR must be ABSENT there. If they ever appear, `:805`
stopped returning before `:808`, every successful boot pays two MMIO reads it does
not need, and run B is no longer testing what it claims. Run B asserts DMATRFCMD
is absent, which is what proves `skipFirmwareLoad` really kept the DMA path out
rather than the count being wrong for a reason no assertion names.

The union is pinned as `runACount + runBCount - 3 == 6` — arithmetic over the two
MEASURED counts, not a literal — so a read site added to EITHER path breaks it.
The exact-count assertions are the load-bearing ones: "CPUCTL was read" stays
green on a machine that read BOTH addresses, and only the count catches that.

Non-vacuity proof, and again the failure count going DOWN is the signal:

| Mutation in `NVGspBoot.hpp` | Failures | What it actually proved |
|---|---|---|
| `:400` PRIV_ERR_STAT read through `nvGspFalconReg()` | 5 | Nothing about reach. THREE were pre-existing — `priv error beats timeout` got `ERR_ACTIVE_TIMEOUT`, `PRIV_ERR_STAT surfaced` got 0, `detected on the first poll` got 5. The old suite already caught it, because the mistranslated read changes a value the machine ACTS on. |
| `:730` DMATRFCMD read through `nvGspRiscvReg()` | 54 | Nothing, louder. The wrong page reads zero, the drain never completes, and the whole DMA half of the suite collapses. |
| A STRAY read at `nvGspFalconReg(NV_PRISCV_RISCV_CPUCTL)` added to `ACTIVE_WAIT`, its value discarded | **5** | The discriminating one. Every pre-existing check stayed GREEN and all five failures were new: two `and NOT one page down` pins, both exact counts, and the union. |

The third mutation is the honest model of the class this section adds reach for: a
read whose value nothing consumes. Those two behaviour-visible mutations were
already covered; a stray or duplicated access at a wrong-engine address was not
visible to anything in the tree before this section, and on hardware it is a real
read of a real register on the wrong engine.

`NVGspBoot.hpp` was mutated three times and reverted three times, and is
byte-identical to where it started — grep confirms no `mutantScratch`, no
`nvGspFalconReg(NV_PRISCV`, and no `nvGspRiscvReg(NV_PFALCON` outside the four
legitimate BROM writes at `:757`–`:772`, which live in the Falcon2 block and are
right as written.

Count 276 → 313, both halves of the provenance requirement updated. 313 sits
ABOVE the previous entry's ceiling (276 + max(8, 34) = 310) by three, so the upper
edge was due to fire on this reading on its own — like 217 → 246 and 116 → 134,
unlike 246 → 258 or 258 → 276.

What this does NOT establish: the six sites are the ones in `NVGspBoot.hpp`. Reads
in `NV3050Probe.cpp` are untouched by it, because no C++ suite can link the IOKit
shell.

---

## §5 — what is open on the NVIDIA side

handoff16 §8 and handoff17 §7 both list the open items as "entirely on the Intel
side." That is still broadly true of the *big* items, and the Intel ones are not
mine this session. What is left on the NVIDIA path, honestly:

- **The kext has never been `kextload`ed.** Unchanged since handoff13. It needs
  SIP lowered and a reboot, and the guest has no NVIDIA card to match against, so
  a successful load would still not exercise `probe()` against real silicon. This
  is the ceiling `PLAN.md` already names.
- **The set-difference audit is DONE for all six sets.** §1b has the first four
  and the calibration that matters (two of four clean); §1d has the last two.
  `NVDecode.hpp` versus `test_decode.cpp` came back clean, and
  `NV3050Probe.cpp`'s property keys have no index to go stale against — but
  enumerating them is what led to the dead-read defect at `:699`. Do not re-open
  this as an open item; the technique has been run against every obvious set in
  the NVIDIA tree.
- **CLOSED — `BROM_PARAMS` now has both.** This item read "the DMA path has a
  state sequence and journal indices; `BROM_PARAMS` has only the indices." §1e
  supplies the sequence, bounded to the single step that performs the four
  writes, with the skip-flag route controlled against the DMA route. Do not
  re-open it. What it does NOT cover is every other state's writes at
  single-step granularity; `BROM_PARAMS` was singled out because MOD_SEL is a
  latch, and no other state in the machine has that property.
- **CLOSED — `nvGspRiscvReg()`'s call sites now have the treatment the aperture
  guard got.** This item read: the suite proves the aperture refusal fires but not
  that every RISC-V access went through the RISC-V translator rather than the
  Falcon one; `testAddressAgreement()` covers the translators, not the choice at
  each site. §1f closes it for the READ sites, which were the whole exposure —
  every WRITE was already pinned to an absolute address by `checkJournal()`, so a
  write through the wrong translator has always reddened instantly. 276 to 313.
  Do not re-open it. What it does NOT cover: the two runs pin the six read sites
  *in `NVGspBoot.hpp`*, and say nothing about read sites in `NV3050Probe.cpp`,
  which no C++ suite can link.
- **The concurrent-audit workflow is HARVESTED.** It swept ten NVIDIA-only areas
  with three independent refuters per finding, surviving on two of three, and its
  rules forbade reading or mentioning the six Intel files. Its one 3-of-3
  confirmed finding is §1c, re-derived against the live files first and closed in
  `tests/test_riscv_ucode.cpp`. Do not re-open it as an open item. Its persisted
  output lives under the session temp directory as `wl3nndcjv.output` (278191
  bytes) with a per-agent `journal.jsonl` beside the session transcript; 58
  agents, zero errors, six that never called `StructuredOutput`. If you go back
  to it for the lower-ranked findings, note that the `.output` path for a
  `local_agent` task can be a symlink to a full subagent transcript — do not Read
  it whole.

---

## §6 — for whoever is next

The habit that produced this session's only real finding, stated as a rule:
**ask which names a test file never mentions.** Passing assertions tell you about
the lines that exist. A terminal error path with no test in front of it, a table
entry with no measurement behind it, and a bound compared against the value it
was derived from are all the same defect seen from different angles — the file is
silent about something, and silence renders as green.

And the concurrency rule that governed this session: when another agent owns a
track, do not read its files to "check" something. A peer's half-written file is
indistinguishable from a finished-but-broken one, so a reading taken from it is
worse than no reading. The three Intel counts in §3 are copied from handoff17
rather than measured for exactly that reason.
