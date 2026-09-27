# handoff21 — the NVIDIA name switches had never executed, and the sentinel I guessed made a coverage loop that could not fail

Written 2026-08-17, later the same day as `handoff20.md`. Continues it directly,
and deliberately mirrors it: handoff20 found that the Intel shell's pure logic
had never *run*, and this session asked the same question of the NVIDIA decode
layer. No register constant moved. No decoder changed. No state machine changed.
`phase0-kext/` is byte-identical to how this session found it, and that is proven
rather than asserted — see §4.

**The Intel boundary held for the whole session.** A separate agent owns that
track. No Intel header, shell, or suite was read or written. The Intel rows in §5
are copied from `handoff20.md` rather than re-measured, for the same reason as in
handoff18: reading a peer's half-written file to take a measurement is worse than
not taking the measurement.

---

## 1. The finding: two switches, one arm each, ever executed

`NVDecode.hpp` has exactly two string-producing functions, and they are the only
ones in the NVIDIA decode layer:

| Function | Arms | Sentinel |
|---|---|---|
| `nvArchitectureName()` at `:268` | 7 (`0x16`..`0x1C`) | `"unknown"` at `:278` |
| `nvAmpereImplementationName()` at `:282` | 6 (`0x00`, `0x02`, `0x03`, `0x04`, `0x06`, `0x07`) | `"unrecognised Ampere part"` at `:291` |

Before this session, `tests/test_decode.cpp` called each of them **exactly
once**: `nvArchitectureName(NV_ARCHITECTURE_GA100)` inside `testBoot0()`, and
`nvAmpereImplementationName(NV_IMPLEMENTATION_GA107)` inside
`testSplitArchitecture()`. Both calls pass the target part. So six of seven
architecture arms and five of six implementation arms had **never executed** —
64 checks, all green, over eleven arms nothing had ever entered.

That is handoff20's finding reached by a different road. There, the switches were
unreachable *by construction*: they were file statics inside an IOKit translation
unit no host suite can compile, so `igModeVerdictName()` could lose
`IG_MODE_SRC_MISMATCH` to `default` and all four gates stayed green. Here the
switches were perfectly reachable — dependency-free header, compiled by MSVC,
driven by a suite that has existed for the whole project — and simply unvisited.
The same hole, one level down. Reachability is not coverage.

**Both switches are complete as written.** That was checked first, by
set-difference rather than by reading, because that is how every one of this
tree's missing-arm defects has been found: seven `NV_ARCHITECTURE_*` defines
against seven `case` arms, six `NV_IMPLEMENTATION_*` against six. Unlike
`igModeVerdictName()` there was no defect to find. The two nameless defines at
`:99` and `:100` (`GB200`, `GR100` — the only two in the family with no trailing
comment, which is usually the signature of a define added late) both have arms:
`"Blackwell 2"` and `"Rubin"`.

So this section adds no fix. It adds the thing whose absence let the Intel defect
through, which is a reason for the completeness to *stay* true.

### What `testNameSwitches()` pins

Three properties, because per-arm pinning alone is not enough:

1. **Every arm returns its OWN string** — per-arm `checkStr()`, plus a
   pairwise-distinct count over all seven and all six. A copy-paste arm returning
   a sibling's name satisfies every "has a name" assertion ever written. The
   likeliest pair in this header is `GA106`/`GA107`: one character apart,
   adjacent in the switch, and mixing them up mis-names *the target part* in a
   kernel log while nothing reddens.
2. **Every value in each family is named** — so an arm DELETED later reddens
   without this section having to list the family a second time. The
   architecture side is a range walk derived from the family's own endpoint
   constants (`NV_ARCHITECTURE_TU100`..`NV_ARCHITECTURE_GR100`), with the span
   asserted to be 7 so a constant moved out of the range cannot shrink the loop
   silently. The implementation side is **not** a range walk, because `0x01` and
   `0x05` are genuine holes in NVIDIA's numbering; the defined values are visited
   by name, and both holes are pinned to the sentinel so nobody "simplifies" the
   list into `0x00..0x07` and gets a failure that is correct about its own
   arithmetic and wrong about the world. (That is `handoff16.md` §1's lesson in a
   new place.)
3. **The value one past each family answers the sentinel** — the tripwire, and
   the point of the whole section. An eighth architecture cannot be given a
   switch arm without reddening `0x1D (above Rubin) is unknown`, which forces
   whoever adds it to come *here* and extend properties 1 and 2. Without that,
   property 2 would silently stop covering the newest arm, which is precisely the
   step whose absence produced the Intel defect.

Also pinned: `0x15` (Volta, deliberately not carried), `0x00`, and `0xFFFFFFFF`.
The last one matters more than it looks — a register that reads all-ones is
caught by `nvLooksLikeDeadRead()`, but if a name switch answered for
`0xFFFFFFFF` the log line would read as authoritative anyway.

And the join, three checks: the real GA107 `boot0` and `boot42` words routed
*through the decoders* into both switches. The per-arm checks pin the switches;
these pin that the decoders feeding them select the right arms, which is the only
thing an operator ever actually reads.

`test_decode` 64 → **96**.

## 1b. The mistake: a sentinel guessed from a sibling function

The part of this session worth carrying forward is not the section. It is that
**my first version went RED against correct shipping code**, four failures, and
the fifth occurrence of the same error was not cosmetic.

I assumed both switches shared one sentinel and wrote `"unknown"` at five sites.
They do not: `nvArchitectureName()` answers `"unknown"` at `:278`,
`nvAmpereImplementationName()` answers `"unrecognised Ampere part"` at `:291`.
Four of the five were `checkStr()` calls and reddened loudly, which is the
harmless kind of wrong.

The fifth was property 2's implementation-side coverage loop:

```cpp
if (std::strcmp(nvAmpereImplementationName(defined[i]), "unknown") != 0) {
    named++;
}
```

That compares against a string that switch **can never return**. So the loop
counted 6 named arms unconditionally — green on the real header, and green on a
header with an arm deleted, which is the exact defect the loop exists to catch.
It printed `ok` while measuring nothing. Had the other four assertions happened
to be right, this one would have shipped as coverage and been believed.

Both sentinels are now bound to named constants, each asserted against what the
header actually returns, and asserted **distinct from each other** — so nobody
can unify the two switches onto one sentinel without noticing that this section's
two loops depend on which is which.

The generalisation, and it is a family this tree already knows under other names
(`an-assertion-that-cannot-fail-is-not-coverage`,
`a-mutation-that-matches-nothing-prints-ok`,
`a-negative-control-can-miss-before-it-tests`): **a comparison against a value
the subject cannot produce is not a test, it is an `ok` line.** The specific new
road into it is guessing a sentinel from a sibling function in the same header.
Two switches side by side, written by the same hand, five lines apart, do not
share a default string — and nothing about reading them makes that visible,
because each `default:` arm is correct in isolation.

## 2. Non-vacuity: the mutation that leaves every old check green

`handoff18.md` §1e and §1f established that a mutation's failure COUNT is a hint
and the per-section ATTRIBUTION is the evidence. Applied here, one mutation was
enough because the class is narrow:

| Mutation in `phase0-kext/NVDecode.hpp` | Failures | Attribution |
|---|---|---|
| `NV_ARCHITECTURE_GB200` arm returns `"Blackwell"` instead of `"Blackwell 2"` | **2** | Both NEW. `0x1B is Blackwell 2` (per-arm), and `the seven architecture names are pairwise distinct` going 0 → 1. Every pre-existing check GREEN. |

That is the honest model of the class this section adds reach for, and it is the
same shape as handoff18 §1f's stray discarded read: **nothing downstream consumes
the value.** The mutated string is still a plausible architecture name, no
control flow branches on it, no register is programmed from it, and it appears
only in a line a human reads at 2am. `-Werror` has nothing to say — the arms
switch on `#define`s, so there is no enum for exhaustiveness to bite on, which is
verbatim the reason `handoff20.md` §1 gives for the Intel defect surviving four
gates.

Note also which mutation was *not* worth planting: deleting an arm entirely. It
reddens property 2 as designed, but property 2 was the loop that could not fail
before §1b, so a green reading there would have proved nothing about the fix. The
sibling-string mutation exercises the pairwise-distinct check, which is the part
no pre-existing assertion could have caught under any sentinel.

`phase0-kext/NVDecode.hpp` was mutated and reverted once and is byte-identical.

## 3. Gates: all four green, in one reading each

Run from the project root after the revert.

| Gate | Verdict | Exit |
|---|---|---|
| `python tools/check-registers.py` | 239/239 passed, 0 failed, 0 skipped | 0 |
| `tests/build-msvc.bat` | 9 of 9 suites passed under ctest | 0 |
| `tests/check-kext-syntax.bat` | `ALL SYNTAX GATES PASSED` | 0 |
| `tests/run-python-tests.bat` | Ran 885 tests, `OK` | 0 |

Two notes on *how* those were read, both of them repeats of lessons this tree has
already paid for:

- The register gate now prints **239**, not the 238 in `handoff18.md`. That is the
  peer's Intel work (`IntelDisplay.hpp` at 91 defines rather than 86), not
  anything of mine. Attribute a moving gate number before treating it as yours.
- My first attempt at the Python gate ran `./tests/run-python-tests.bat 2>&1 |
  tail -40; echo "python gate exit: $?"` and printed **`0`** — which was
  `tail`'s status, not the gate's. This is
  `dollar-question-after-a-pipeline-is-tees-status` exactly, and it is worth
  recording that I walked into it in a session whose whole subject is assertions
  that cannot fail. Re-run carrying the status out through a file
  (`> /tmp/pygate.txt 2>&1; echo $? > /tmp/pygate.status`) before believing any
  gate verdict read through a pipe.

The Python gate's 885 also does double duty here: `build-payload/kext-src.tar` is
compared byte-for-byte against `phase0-kext/`, so a green payload gate is the
proof that the mutate/revert in §2 left `NVDecode.hpp` byte-identical. It is
listed as evidence rather than as a passing test.

## 4. What this does NOT establish

- **Nothing about hardware.** Same as every handoff since `handoff13.md`: the
  kext compiled and linked under Apple clang 17.0.0 on 2026-08-14 and has never
  been `kextload`ed. There is no NVIDIA card in the guest to match against.
- **Nothing about `NV3050Probe.cpp`.** The switches now covered live in
  `NVDecode.hpp`; the shell's own 37 property publications, its BAR mapping, and
  its `regRead32()` refusal are still reached only by parse, because no C++ suite
  can link an IOKit translation unit. `handoff18.md` §1d fixed one defect in
  there by pinning the code SHAPE from Python
  (`tests/test_probe_dead_read_publication.py`); that remains the only technique
  available for that file, and it has been applied to exactly one property.
- **No new evidence of any kind.** No register was read, no trace was parsed, no
  silicon was touched. This session moved only what the host suites execute.
- **The count band did not catch this.** 96 is above the previous entry's ceiling
  (64 + max(8, 8) = 72) by twenty-four, so the upper edge *would* have fired on
  this reading on its own — but the entry it fired on had not moved since
  2026-08-13, which is the point. The band notices a suite that grows. It cannot
  notice a suite that has been quietly complete-looking for four days.

## 5. Measured check counts

| Suite | Checks | Measured by |
|---|---|---|
| `test_decode` | **96** | this session (was 64) |
| `test_falcon` | 169 | handoff18 |
| `test_riscv_ucode` | 134 | handoff18 |
| `test_gsp_boot` | 313 | handoff18 §1f |
| `test_gsp_stage` | 181 | handoff18 |
| `test_intel_display` | 354 | handoff17 — not re-measured by me |
| `test_intel_framebuffer` | — | peer-owned |
| `test_intel_gpu_id` | — | peer-owned |
| `test_intel_display_names` | 236 | handoff20 — peer's, not re-measured by me |

NVIDIA-side total: 893 checks across five suites.

## 6. Open items, NVIDIA side

Ordered by value, and the first two are unchanged from `handoff18.md` §5 because
nothing in this session or handoff20 could touch them.

1. **One `kextload` on a machine that contains the card.** Still the single
   highest-information action available to this project, and still the one no
   amount of host-side work substitutes for. It needs macOS on hardware with the
   3050 in it, SIP lowered, and a reboot. Every phase from 2 onward is gated on
   it, and the code for phases 2 through 3.75 is written and tested.
2. **`NV3050Probe.cpp` has runtime coverage of exactly one property.** The
   dead-read publication shape from `handoff18.md` §1d is pinned from Python. The
   other 36 property keys, the BAR-length check, and the `regRead32()` refusal
   are not. The technique is known and cheap; it just has not been applied.
3. **`nvGspBootStateName()` and `nvGspBootResultName()` in `NVGspBoot.hpp` and
   `nvGspStageResultName()` in `NVGspStage.hpp` already have exhaustiveness loops
   driving them** — checked this session, which is why §1 is about `NVDecode.hpp`
   and not about those. Recorded here so nobody re-opens it: the set-difference
   over `NVGSP_RESULT_*` was already done in `handoff18.md` §1, and the name
   tables are bounded by their own families.
4. **No test observes a GSP boot from inside the DMA states with a real multi-MiB
   image.** `testDmaStateSequence()` steps two chunks. Real firmware is hundreds
   of chunks, and the only thing that scales with chunk count is the offset
   arithmetic, which `test_gsp_stage.cpp` covers separately. Low value, listed for
   completeness.

Nothing on this list is blocked on the Intel track, and nothing on it requires
editing an Intel file.
