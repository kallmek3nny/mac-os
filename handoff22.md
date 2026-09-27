# handoff22 — the policy range is not the mapping

2026-08-17, Intel track. Continues from `handoff20.md` (Intel) and `handoff21.md`
(NVIDIA, a concurrent peer). One finding, and unlike the last three it is a
**live defect in shipping code** rather than a coverage hole: the fourth guard
in front of `IntelDisplayProbe::regRead32()` did not exist, and its absence let
a short BAR0 read past the end of its own mapping.

## Gate state, measured after the last edit in this session

| Gate | Reading |
|---|---|
| `python tools/check-registers.py` | exit 0 — 239/239 passed, 0 failed, 0 skipped |
| `tests\build-msvc.bat` | exit 0 — 100% tests passed, 0 failed out of 9 |
| `tests\check-kext-syntax.bat` | exit 0 — `VERDICT: PASS` |
| `tests\run-python-tests.bat` | exit 0 — Ran 885 tests, OK |

Each was read once after the final edit, serially, never while another was
running. The register gate stays at 239 because `intel_verdict_set_checks()`
checks a family as a SET rather than counting lines — the `IG_READ_` family grew
from 4 verdicts to 5 and the gate's own total is unchanged.

## Measured check counts

| Suite | Count |
|---|---|
| `test_intel_display` | **391** checks, 0 failures — was 373 |
| `test_intel_display_names` | 236 checks, 0 failures |
| `test_intel_framebuffer` | 110 checks, 0 failures |
| `test_intel_gpu_id` | 77 checks, 0 failures |
| `test_decode` | 96 — NVIDIA, moved by the peer, see `handoff21.md` |
| `test_falcon` / `test_riscv_ucode` | 169 / 134 |
| `test_gsp_boot` / `test_gsp_stage` | 313 / 181 |

`tests/test_check_count_floor.py`'s `MEASURED` entry for `test_intel_display`
moved 354 → 391 with a dated provenance line, which that module now requires per
entry (see `handoff17.md` §1).

## §1 — the finding: a warning with no consequence

`IntelDisplayProbe::mapBar0()` has always done this, and it is deliberate:

```cpp
if (length < IG_BAR0_EXPECTED_LENGTH) {
    IGLOG("mapBar0: WARNING BAR0 is %llu bytes, shorter than the 0x%x the "
          "display block needs -- reads past the end are undefined, not "
          "faults", ...);
}

return true;
```

Attaching to a short BAR is the right call and `IntelDisplayProbe.hpp:94` says
why: declining would delete the IORegistry node that makes the problem
inspectable in the first place. The defect is that **the warning was the only
consequence there was.**

`regRead32()` bounded its one dereference against three guards — unmapped,
sentinel offset, out of policy — and the third of those is
`igIsInDisplayPolicyRange(offset)`, i.e. `0x60000..0x7BFFF`. Read what that pair
of constants actually claims. `IntelDisplay.hpp:164` is explicit about it:

> This pair bounds THE ADDRESSES THIS HEADER CAN PRODUCE, and says nothing about
> what the hardware exposes.

It also says nothing about what the **OS mapped**. Those are three different
numbers and the code joined none of them:

| Constant | What it is |
|---|---|
| `IG_DISPLAY_POLICY_LIMIT` `0x7BFFF` | largest offset the decode tables can emit |
| `IG_BAR0_EXPECTED_LENGTH` `0x80000` | how much the display block NEEDS — a warning threshold |
| `fBar0Map->getLength()` | how many bytes actually exist — read, logged, and **discarded** |

So on a 0x40000 BAR: `mapBar0()` warns and attaches, `inspectTranscoders()` asks
for transcoder A at `0x60000`, `igReadDecision()` says that is a fine offset
because it is inside the policy window, and the shell dereferences
`fBar0Base + 0x60000` — **0x20000 bytes past the end of the mapping**. Per the
warning's own wording that is not a fault. It is whatever the next mapping in
the address space happens to hold, decoded as `TRANS_HTOTAL` and published into
the IORegistry as a mode.

The shape is one this tree has paid for before, in
[`a-read-that-discards-its-address-has-no-observer`]: a value is read, a bound
exists somewhere, and the two never meet. Here the bound was even *logged*.

### Why no gate could see it

- **Register gate** — checks constants against `reference/i915/`. Both constants
  are correct. Their *relationship to a runtime length* is not a constant.
- **C++ suites** — `igReadDecision()` took `(bool, IGU32)`. There was no
  parameter to get it wrong with. A test cannot pin an input the function does
  not accept.
- **Syntax gate** — parses. Nothing to say.
- **Python gate** — models file shapes, not arithmetic.
- **`-Werror`** — the verdicts are `#define`d, so there is no enum to be
  exhaustive over. Verbatim the reason `handoff20.md` §1 gives for the
  `IG_MODE_SRC_MISMATCH` defect surviving four gates.

The honest summary is that this was reachable only by reading `mapBar0()` and
`regRead32()` side by side and noticing that one of them measures something the
other needs.

## §1b — what changed

`igReadDecision()` gained a fourth parameter and a fourth verdict:

```cpp
static inline IGU32 igReadDecision(bool barMapped, IGU64 mappedLength, IGU32 offset)
```

with `IG_READ_PAST_MAPPING 4u` and `IG_READ_VERDICT_LIMIT` moved 3 → 4. Three
choices in it are load-bearing:

**The length is `IGU64`, not `IGU32`.** IOKit hands it over as an `IOByteCount`,
which is 64 bits. Narrowing on the way in would be a silent truncation on the
one input whose entire job is to bound a dereference, and the failure is
asymmetric: `0x1_0000_0000` truncated to `0` refuses everything, which is wrong
and loud, while `0x1_0007_0000` truncated to `0x70000` **accepts reads it should
refuse**. That direction scribbles. `IGU64` is a new typedef beside `IGU32`,
`unsigned long long` for the same reason — `<cstdint>` is not available to a
`-mkernel -fapple-kext` translation unit.

**The test is `offset + IG_MMIO_ACCESS_BYTES <= mappedLength`, not
`offset < mappedLength`.** An offset at `length - 1` is inside the mapping by
any naive test and still reads three bytes past the end of it. `4u` is named
rather than spelled inline precisely so the width is visible at the bound.

**The new guard runs LAST**, and that is the reverse of the intuitive order. An
out-of-policy offset is a bug in this driver; a short BAR is a fact about the
machine. An offset that is both should be reported as the bug, because that is
the one somebody can go and fix.

On the shell side, `IntelDisplayProbe` gained `IGU64 fBar0Length`, assigned from
`fBar0Map->getLength()` in `mapBar0()`, zeroed in `init()` and in
`unmapBar0()`. Zero-on-unmap is the safe direction: `igReadDecision()` then
refuses every offset rather than trusting a stale bound. `regRead32()` gained
one more `case` with its own `IGLOG` naming the actual length, so the operator
sees the number rather than the policy range being blamed for a mapping problem
— the same failure mode `handoff20.md` §1c records one level up.

## §2 — non-vacuity, and the proof found a second defect in my own test

Planting the naive bound — `offset >= mappedLength` in place of
`offset + IG_MMIO_ACCESS_BYTES > mappedLength` — reddened **exactly one check,
and it was a new one**:

```
FAIL one byte short of that refuses -- the read would overhang by one got 0x00000000 want 0x00000004
389 checks, 1 failures
```

Every pre-existing check green. That is the only mutation signal this project
accepts as evidence of added reach, and `handoff18.md` §1e is the reason: a
mutation that reddens *many* checks is usually evidence the OLD tests sufficed.

But one failure is also the thing worth reading here, because it exposed that my
block pinned one **point** of the overhang window rather than the window. The
bound can be wrong by three different amounts and each spelling fails a
different subset:

| Wrong bound | Reddens |
|---|---|
| `offset >= length` | only the +3 case |
| `offset + 2 > length` | only +1 and +2 |
| `offset + 3 > length` | only +1 |

So a *partial* fix — someone patching the naive bound to `offset + 2` — would
have left my one check green. Two more checks now pin the remaining two bytes,
and a bound wrong by any amount reddens at least one of them. 389 → **391**.

This is the same lesson as [`containment-is-one-sided`] with the sides swapped:
there the failure was probing only the slack side of a window, here it was
probing only one byte of an off-by-N window. A single edge check is a point
sample of a range property.

## §3 — what this session does NOT establish

- **`IntelDisplayProbe.cpp` still has never run.** `igReadDecision()` is
  executed 40-odd times by `test_intel_display`; the `case IG_READ_PAST_MAPPING`
  arm that consumes it, the `IGLOG` inside it, and the `fBar0Length` assignment
  in `mapBar0()` are reachable only by parse. The dereference and the assignment
  are the two things a host suite structurally cannot reach, and both are now as
  thin as they can be made — which is the whole argument for the split, not a
  claim that the shell is tested.
- **No short BAR has ever been observed.** `IG_BAR0_EXPECTED_LENGTH` is a policy
  threshold; the 2026-08-16 capture is a Linux-side register dump and says
  nothing about what macOS would map. The guard is correct arithmetic over a
  situation this project has not seen.
- **`make` still has not run** since `SOURCES` gained a second object, and now
  also has not run since `IGU64`, the fourth parameter, and `fBar0Length`
  landed. The `IGU64` typedef and the `%llu` in the new `IGLOG` are the two
  things most likely to say something under Apple clang that MSVC did not.
- The BAR-length **warning itself** — the `IGLOG` in `mapBar0()` — is still
  untested, and now always will be: it is a log line in an IOKit translation
  unit. What changed is that it is no longer the only consequence.

## §4 — open, highest value first

1. **One `kextload` on a machine containing the card.** Unchanged from
   `handoff21.md` §6 item 1 and still the highest-value action in the project.
   Everything below is verification of code that has never executed on silicon.
2. **`make` in the macOS guest.** Cheaper than item 1 and now overdue by four
   sessions' worth of `phase0-kext/` edits across both tracks. See
   `OPERATOR-ACTION-step7-build.md` — the payload is at its TWENTY FOURTH
   rebuild (ISO md5 `df4b5ebcab09d06969aa0f438693d037`, 415744 bytes; tar md5
   `350935ec42fdf5b2d9bca61813a571ad`, 337920 bytes).
3. **`check-documented-paths.py` tokenizes quoted gate output as paths.**
   Carried unchanged from `handoff20.md` §5 item 3. Currently harmless — the
   TAIL rule catches it — which means it passes for a reason other than the one
   it claims.
4. **`publishTranscoder()` / `publishPlane()` call shape.** The last untested
   shell content with real logic in it. `handoff18.md` §1d is the template: when
   no gate can see it, pin the code SHAPE from Python and make the checker refuse
   rather than pass when it cannot find its subject.
