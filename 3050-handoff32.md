# 3050-handoff32 — six refusals, one word, and two enum arms that could not run

NVIDIA track, 2026-08-18, continuing from `3050-handoff31.md`. It takes that
file's §7 item 3 — `nvRiscvUcodeIsWellFormed()` — and the first thing it does is
**correct the item's premise**.

`phase0-kext/NVRiscvUcode.hpp` and `phase0-kext/NVGspStage.hpp` both CHANGED, so
`build-payload/` was rebuilt and `OPERATOR-ACTION-step7-build.md` updated. That
is the **THIRTY SECOND** image.

---

## §1 — the open item was half wrong, and the wrong half was the headline

`3050-handoff31.md` §7 item 3 reads, verbatim:

> `nvRiscvUcodeIsWellFormed()` returns a bare `bool` with six distinct refusal
> reasons behind it, **and nothing in the IOKit shell calls it at all**. Both
> halves are worth fixing and the second is the one that matters — a validator
> with no caller is `intel-handoff26.md` §1's tested-function-with-no-callers in
> its purest form.

The second half is **FALSE**. One grep answers it:

- `phase0-kext/NVGspStage.hpp:273` — `if (!nvRiscvUcodeIsWellFormed(blob, blobLen))`
  inside `nvGspStagePlan()`;
- `phase0-kext/NV3050Probe.cpp:1369` — `nvGspStagePlan(blob, (NVU32)blobLen, &plan)`
  inside `loadGspFirmware()`;
- `NV3050Probe.cpp:1125` — `loadGspFirmware()` called from the GSP boot path.

So the validator has a caller, that caller has a caller, and the chain reaches
the shell. The item was written from the shape of the previous session's defect
rather than from a grep, and it named the wrong half as the one that matters.

This is `3050-handoff29.md` §1 recurring in the same open list: **an item
inherited across handoffs is a claim with an age**, and "nothing calls X" is a
set-difference somebody has to actually run. handoff31 was NOT edited to remove
the item — same rule as `3050-handoff28.md` §7 item 5 and handoff29 §1. The
chronological record stands; this file is the correction.

The FIRST half was true, and it turned out to have a second defect underneath it
that the item did not mention.

---

## §2 — what was actually wrong: six refusals collapsed into one word

`nvRiscvUcodeIsWellFormed()` refused a blob for six structurally distinct
reasons and returned `false` for all of them:

| Reason | Site |
|---|---|
| container header short, bad magic, or inconsistent | `nvRiscvBinHdrIsValid()` |
| descriptor lacks the fields Phase 4 reads | field-count test |
| descriptor version is not 5 | version equality |
| bootloader region zero-length | `blSize == 0 \|\| ...` |
| bootloader region escapes the payload | same `\|\|` |
| bootloader-param region escapes the payload | param containment |
| an optional region escapes the payload | the six-row loop |

`nvGspStagePlan()` mapped that single `false` onto `NVGSP_STAGE_ERR_MALFORMED`,
and `NV3050Probe::loadGspFirmware()` at `:1372` put the result through
`nvGspStageResultName()` into one log line:

```cpp
NVLOG("GSP firmware: refusing %u-byte blob -- %s",
      (unsigned)blobLen, nvGspStageResultName(planned));
```

So every structurally bad blob produced the word `ERR_MALFORMED`. Those seven
rows call for **three different operator actions** — re-extract a truncated
file, use a different firmware file, or file a bug against this parser — and the
driver said the same thing for all of them.

The version case is not hypothetical. `3050-handoff27.md` §2 measured that both
booters in `reference/ga107-firmware/` carry an `nvfw_hs_header_v2` where the
bootloader carries an `RM_RISCV_UCODE_DESC`. Hand this driver a booter by
mistake and the honest diagnosis is *that is the wrong kind of file*; what it
printed was *malformed*.

### §2a — the sharper half: two enum arms that could not execute

Underneath that, `nvGspStagePlan()` had this shape:

```cpp
if (!nvRiscvUcodeIsWellFormed(blob, blobLen))
    return NVGSP_STAGE_ERR_MALFORMED;

NVU32 blOff  = nvRiscvDescField(blob, NV_RISCV_DESC_BOOTLOADER_OFFSET, 0);
NVU32 blSize = nvRiscvDescField(blob, NV_RISCV_DESC_BOOTLOADER_SIZE, 0);

if (blSize == 0u)
    return NVGSP_STAGE_ERR_EMPTY_BOOTLOADER;

if (!nvRiscvRegionInPayload(blob, blOff, blSize))
    return NVGSP_STAGE_ERR_REGION_OUTSIDE;
```

Both of those tests re-read the **same two descriptor fields** the well-formed
gate had already refused on at `NVRiscvUcode.hpp:370`. `NVGSP_STAGE_ERR_EMPTY_BOOTLOADER`
and `NVGSP_STAGE_ERR_REGION_OUTSIDE` were therefore **provably unreachable** —
two named codes in a family whose whole reason for being an enum, per its own
header comment at `NVGspStage.hpp:152`, is "so a test can assert WHICH check
fired."

**And the suite said so.** `tests/test_gsp_stage.cpp:601`–`:606`, before this
session:

```
// Zero-length bootloader region. The plan has an ERR_EMPTY_BOOTLOADER
// branch for this, but nvRiscvUcodeIsWellFormed() rejects blSize == 0
// first, so the API returns MALFORMED. Asserting EMPTY_BOOTLOADER here
// would be asserting a branch that does not execute.
```

That is a gate **documenting its subject's dead arm and then asserting the
collapse**. The reasoning in the comment is locally correct — asserting a branch
that does not execute would indeed be wrong — and the conclusion it reached was
to ratify the defect in four `checkResult()` calls rather than to ask why the
branch was dead. An unreachable guard is indistinguishable from a deleted one;
four green assertions saying `-> MALFORMED` made it look deliberate.

This is a new member of `handoff18.md` §1d's family and it deserves its own
name: not a fabricated publication, but **a test that pins the collapse instead
of the fact**. The distinguishing tell is a test comment that explains why a
branch in the subject cannot run.

### §2b — why the four gates were blind

- **register gate** — checks `#define`s against NVIDIA's headers. Every constant
  involved is correct, and the reachability of an enum arm is not a constant.
  Unmoved at 240.
- **C++ suites** — `test_gsp_stage` had 181 checks over this exact function and
  four of them asserted the collapse. A suite that agrees with the defect passes.
- **syntax gate** — a `return` of a well-formed enumerator is well-formed C++.
  clang has nothing to say about an arm nothing reaches, because the arm is
  *reachable in the CFG* — it is only unreachable given a fact about a callee.
- **Python gate** — nothing models "every code in this enum is producible".

---

## §3 — the fix, and the three choices worth arguing about

Two shipping headers changed.

**`NVRiscvUcode.hpp`** gains `NVRiscvUcodeVerdict` (eight codes plus
`NV_RISCV_UCODE_VERDICT_LIMIT`), `nvRiscvUcodeVerdictName()`, and
`nvRiscvUcodeVerdict()` carrying the old body with each `return false` replaced
by the code that names the case.

**`NVGspStage.hpp`** maps that verdict through an exhaustive `switch` and drops
its two re-tests.

### The bool is KEPT as a wrapper

```cpp
static inline bool nvRiscvUcodeIsWellFormed(const unsigned char *buf, NVU32 len)
{
    return nvRiscvUcodeVerdict(buf, len) == NV_RISCV_UCODE_OK;
}
```

"Is this blob usable at all" is a real question with a real answer, and every
existing caller and every existing assertion in `tests/test_riscv_ucode.cpp`
asks exactly that. Deleting it would have meant rewriting `nvGspStagePlan()` and
several dozen assertions in the same commit that introduced the verdict — which
is how a widening turns into a rewrite nobody can review.

The relation is an **EQUALITY against `NV_RISCV_UCODE_OK`**, never a comparison
against an error code. A `!= NV_RISCV_UCODE_ERR_HEADER` form would call a
version-mismatched booter well-formed, which is precisely the blob handoff27
found being read as a descriptor.

### The `||` is SPLIT, and the order is load-bearing

```cpp
if (blSize == 0) {
    return NV_RISCV_UCODE_ERR_BL_EMPTY;
}

if (!nvRiscvRegionInPayload(buf, blOff, blSize)) {
    return NV_RISCV_UCODE_ERR_BL_OUTSIDE;
}
```

A zero-length region **trivially satisfies containment**, so a version that
tested containment first would make `NV_RISCV_UCODE_ERR_BL_EMPTY` unreachable —
the same dead-arm shape this whole change exists to remove, reintroduced one
level down. `testUcodeVerdict()` pins the order with a blob that is BOTH empty
and out of the payload; only the emptiness-first order returns `BL_EMPTY`.

### The stage mapping has NO `default:` arm

```cpp
switch (uv) {
case NV_RISCV_UCODE_OK:                 break;
case NV_RISCV_UCODE_ERR_HEADER:
case NV_RISCV_UCODE_ERR_DESC_SHORT:
case NV_RISCV_UCODE_ERR_DESC_VERSION:   return NVGSP_STAGE_ERR_MALFORMED;
case NV_RISCV_UCODE_ERR_BL_EMPTY:       return NVGSP_STAGE_ERR_EMPTY_BOOTLOADER;
case NV_RISCV_UCODE_ERR_BL_OUTSIDE:
case NV_RISCV_UCODE_ERR_PARAM_OUTSIDE:
case NV_RISCV_UCODE_ERR_REGION_OUTSIDE: return NVGSP_STAGE_ERR_REGION_OUTSIDE;
case NV_RISCV_UCODE_VERDICT_LIMIT:      return NVGSP_STAGE_ERR_MALFORMED;
}
```

Exhaustive with no `default:`, so a ninth code added to the verdict enum fails
to **compile** here rather than silently landing on `ERR_MALFORMED`. That is
`handoff20.md` §1's `igModeVerdictName()` lesson applied at the mapping rather
than at the name function: there, a `default:` arm swallowed
`IG_MODE_SRC_MISMATCH` and all four gates stayed green.

The two re-tests below the switch are **deleted rather than kept as defence in
depth**. Past the verdict the bootloader region is guaranteed non-empty and
contained; a guard restating a guarantee is an unreachable guard, and an
unreachable guard is indistinguishable from a deleted one. That is the defect
this session is fixing, so keeping them would have been the fix reproducing its
own subject.

### The verdict codes are an enum, not `#define`s

Same reasoning as `NVGSP_READ_*` in `3050-handoff29.md` §3:
`tools/check-registers.py` parses this header's `#define`s against NVIDIA's
published headers, and these codes are this driver's own bookkeeping rather than
hardware. The register gate is correctly **unmoved at 240**.

---

## §4 — the gate: coverage added, and four assertions corrected

`tests/test_riscv_ucode.cpp` gains `checkVerdict()` (which prints the NAME on
both arms — "got 3, expected 4" makes the reader go and count enumerators) and
`testUcodeVerdict()`. **188 → 211 checks.**

What it pins:

| Claim | Why it is not redundant |
|---|---|
| a separating input for each of the eight codes | asserting only "refused" passes with every arm returning the same thing |
| both roads into `ERR_HEADER` (length, magic) | different failures that happen to share a code |
| empty-AND-outside returns `BL_EMPTY` | pins the ORDER; containment-first makes `BL_EMPTY` dead |
| every verdict below the family's own limit has a name | a code added with no arm reddens on the commit that adds it |
| the names are pairwise distinct | a copy-paste arm returning a sibling's string passes every "has a name" check |
| the wrapper agrees on OK and on a refusal | the equality, pinned at both ends |

The name-coverage loop bounds on `NV_RISCV_UCODE_VERDICT_LIMIT` rather than on a
typed count, and the sentinel is **read out of the subject** —
`nvRiscvUcodeVerdictName((NVRiscvUcodeVerdict)NV_RISCV_UCODE_VERDICT_LIMIT)` —
rather than spelled. `handoff21.md` §1b is the reason: a sentinel guessed from a
sibling function is a comparison against a string the subject can never return,
and every arm then counts as named unconditionally.

### §4a — the four assertions that changed, and why that is not a weakening

`tests/test_gsp_stage.cpp` is **unmoved at 181 checks**. Four `checkResult()`
calls changed their EXPECTED value:

| Input | Was | Now |
|---|---|---|
| zero-length bootloader | `ERR_MALFORMED` | `ERR_EMPTY_BOOTLOADER` |
| region starts at payload end | `ERR_MALFORMED` | `ERR_REGION_OUTSIDE` |
| region runs past payload end | `ERR_MALFORMED` | `ERR_REGION_OUTSIDE` |
| region offset near `u32` max | `ERR_MALFORMED` | `ERR_REGION_OUTSIDE` |

The count not moving is the point. No assertion was added or removed; four
assertions that pinned a collapse now pin a fact. The comment explaining why
`EMPTY_BOOTLOADER` "does not execute" is replaced by one recording that it used
to not execute and why it does now.

This is the one place where a reader should be suspicious of me, so it is worth
saying plainly: **changing four expected values in a suite is exactly what
somebody covering up a regression would do.** The distinguishing evidence is
that the old expected value is still produced for other inputs — three of the
eight verdict codes still map to `ERR_MALFORMED`, and
`test_gsp_stage.cpp:590`/`:598` still assert it for a bad magic and a 16-byte
descriptor. If the mapping had simply been broken, those would have moved too.

---

## §5 — gate state, all four run after the last edit

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped, exit 0 |
| `tests/build-msvc.bat` | **9/9** suites passed, exit 0 |
| `tests/check-kext-syntax.bat` | exit **0** |
| `tests/run-python-tests.bat` | **974** tests, OK, exit 0 |

Python unmoved at 974, correctly: no Python test was added, and the count-floor
table entry is an EDIT rather than a new test. Register gate unmoved at 240,
correctly: the verdict family is an enum, and the stage enum gained no members.
C++ 9 suites, `test_riscv_ucode` 188 → **211**, `test_gsp_stage` unmoved at 181.

### §5a — the count reading grazed its own ceiling

211 lands **EXACTLY** on the previous entry's ceiling: 188 + max(8, 188//8) =
188 + 23 = 211. The check in `tests/test_check_count_floor.py:603` is `>`, so it
did not fire — one check further and it would have.

Recorded in the provenance block because a reading that grazes its own edge is
the one most likely to be misread later as comfortably inside. The band exists
to catch a stale number in that table, and a growth that stops one short of
tripping it is the growth whose provenance paragraph is doing all the work.

### §5b — the Python gate went red once, and the staleness was mine

First reading: `Ran 974 tests`, `FAILED (failures=1)`, exit 1. The failure was
`test_guest_build_kext.PayloadFreshnessTests.test_every_shipped_file_is_byte_identical_to_the_tree`,
and its message named exactly the two files I had edited:

```
AssertionError: Lists differ: ['phase0-kext/NVGspStage.hpp',
                              'phase0-kext/NVRiscvUcode.hpp'] != []
```

Attributed by mtime and by the failure MESSAGE, per `intel-handoff27.md` §5a and
`3050-handoff29.md` §7: both paths are mine, both edits are from this session,
so rebuilding was correct here. Payload rebuilt, card updated, gate re-run green.

---

## §6 — the payload, and both sizes moving again

| Artifact | Bytes | md5 |
|---|---|---|
| `step7-payload.iso` | 456704 | `12fcdcd3d157e4cea66f932773a732f0` |
| `kext-src.tar` | 378880 | `e1f569762dbeb0db55149cf058152589` |

Both sizes moved — tar 368640 → 378880, ISO 446464 → 456704, one 10240-byte tar
block each. Second image running where the size moved, which is a coincidence of
this change being ~2 KiB of code and comment rather than a signal. **The md5 is
still the only fingerprint** — `3050-handoff26.md`'s comment-only rebuild remains
the standing proof.

---

## §7 — what this does NOT establish

1. **Nothing here ran on hardware.** `nvRiscvUcodeVerdict()` is a pure header
   function driven by a host suite; `loadGspFirmware()`'s refusal log has never
   executed on a machine with the card.
2. **No verdict reaches the IORegistry.** The refusal still goes to `NVLOG` only.
   `loadGspFirmware()` returns a bare `bool` to its caller at `:1125`, and
   nothing publishes which of the eight codes fired. This is the same class as
   `3050-handoff31.md` §1 — a diagnosis reaching the log and not the registry —
   and it is now one level better (eight words instead of one) rather than
   fixed. See §8 item 2.
3. **`NV_RISCV_UCODE_ERR_PARAM_OUTSIDE` and `NV_RISCV_UCODE_ERR_REGION_OUTSIDE`
   both map to `NVGSP_STAGE_ERR_REGION_OUTSIDE`**, so the stage layer still
   loses the distinction between a param region and an optional region escaping.
   Deliberate — the stage enum was not grown — but it means the finer verdict is
   only visible to a caller of the header, not to `loadGspFirmware()`.
4. `NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED` is still in the enum and the state
   machine has still never had real firmware bytes pass through it.
5. Nothing tests that a REAL booter from `reference/ga107-firmware/` now returns
   `NV_RISCV_UCODE_ERR_DESC_VERSION` specifically. `testRealFirmwareFiles()`
   drives the real files, but its booter assertions are still through the bool.
   That is the cheapest remaining item and it is §8 item 3.

---

## §8 — open, most valuable first

1. **The operator's scheduling decision.** Unchanged and still theirs: the laptop
   holds Linux or macOS, not both, so more i915 traces and one `kextload` are
   mutually exclusive. Only the Intel track can produce a picture on the internal
   panel, so the traces are almost certainly worth more.
2. **One `kextload` on a machine containing the card.** Unchanged from handoff22
   §4 through here. Still the only thing that turns eleven handoffs of source
   claims into measurements.
3. **`decodeBoot0()`'s log-only diagnoses.** Carried forward from
   `3050-handoff31.md` §7 item 4 and now the highest-value item not blocked on
   hardware. `decodeBoot0()` at `:432` detects a non-Ampere architecture and an
   Ada part specifically, and at `:447` detects a non-GA107 implementation — all
   four findings go to `NVLOG` and none reaches the IORegistry. handoff31 fixed
   the `decodeBoot42()` half; this half has not been swept.
4. **Assert the real booters against the verdict, not the bool.**
   `testRealFirmwareFiles()` opens all three files in
   `reference/ga107-firmware/` and checks `nvRiscvUcodeIsWellFormed()` on them.
   Changing those to `checkVerdict()` would pin that a real booter returns
   `ERR_DESC_VERSION` specifically — turning handoff27's measurement into a
   standing assertion. Cheap, and it is the one thing that would make the version
   code a claim about the firmware rather than about a synthetic blob.
5. **The refusal reason is not published.** §7 item 2 — `loadGspFirmware()`
   returns `bool` and the eight-way verdict dies in the log. A
   `GSPFirmwareRefusalReason` property would be the same fix
   `3050-handoff31.md` §2 applied to BOOT_42, and no gate can currently see its
   absence.
6. Nothing tests that `regRead32()`'s refusal value is *the same constant* the
   consumers compare against. `NVWPR2_READ_REFUSED_VALUE` in
   `phase0-kext/NVFalcon.hpp` and `IG_MMIO_INVALID` in
   `phase0-kext/IntelDisplay.hpp` are both `0xFFFFFFFF` by agreement rather than
   by construction. Unchanged from `3050-handoff26.md` §7 item 4.
