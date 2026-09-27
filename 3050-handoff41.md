# 3050-handoff41

NVIDIA track. Direct continuation of `3050-handoff40.md`. Two things happened,
and the FIRST one is the reason to read this file at all:

**`3050-handoff40.md`'s own §9 open list was wrong about two of its five items,
and I wrote it.** Item 2 was false when written. Item 1 was two-thirds false when
written. Both were written from the SHAPE of previous sessions' defects rather
than from a grep — which is the exact class `3050-handoff29.md` §1 and
`3050-handoff32.md` §1 each record, now for the third time and the first time
against an item I authored myself in the immediately preceding session.

The second thing is the work: the genuinely-remaining third of item 1 is closed.
`tests/test_probe_boot42_log_arms.py`, **20 tests**.

`phase0-kext/` is **BYTE-IDENTICAL** — no shipping file changed — so
`build-payload/` was NOT rebuilt and the FORTIETH image is still current.

---

## 1. The open list I wrote was wrong, and how it was measured

handoff40 §9 listed five items. Before acting on any of them I measured them,
which took four greps and about five minutes.

### 1a. Item 2 was FALSE when written

The item said `loadGspFirmware()` still returns `bool` while the stage verdict
beneath it dies in `NVLOG`, and called it *the oldest unclosed item on the
NVIDIA track*. It is closed, and has been since `3050-handoff35.md`:

| Claim in the item | What is actually in the tree |
|---|---|
| the stage verdict is unpublished | `publishGspStageVerdict()` at `NV3050Probe.cpp:1581` |
| nothing declares it | `NV3050Probe.hpp:145` |
| nothing calls it | `NV3050Probe.cpp:1645`, inside `loadGspFirmware()` |
| no gate covers it | `tests/test_probe_gsp_stage_publication.py`, nine properties, ~eleven negative controls |
| the operator cannot see it | `OPERATOR-ACTION-step7-build.md:432`–434 documents all four keys |

It publishes `NVGspStageVerdict`, `NVGspStageOk` (an EQUALITY against
`NVGSP_STAGE_OK`, not a negated error arm), `NVGspStageUcodeEvaluated` and
`NVGspStageUcode`, on every arm including the `!plan` one.

So the item was not merely stale — it was false on the day it was written, one
session after the work landed.

### 1b. Item 1 was two-thirds false when written

The item said the log/registry agreement has no gate anywhere, and proposed a
shape gate deriving the arm count from the header's `_VERDICT_LIMIT`. Two such
gates already existed, in exactly that idiom:

- `tests/test_probe_gsp_result_log_arms.py` — the result switch (handoff39 §9).
- `tests/test_probe_identity_log_arms.py` — `decodeBoot0()` (handoff39 §10).

The honest remainder is one function: **`decodeBoot42()`**, whose four log arms
had no agreement gate. That third is what §2 closes.

### 1c. Why this keeps happening, stated plainly

The three log-only-diagnosis fixes of handoff39–40 have a rhythm, and an open
item written at the end of a session is written from that rhythm. The sentence
*"and the same is presumably true one register over"* is a HYPOTHESIS. Written
into an open list it becomes indistinguishable from a measurement, and the next
session inherits it as a fact with an age.

handoff40 was **NOT edited**. Same rule as handoff28 §7 item 5 and handoff32 §1:
the numbered handoffs are a chronological record, not a status page, and a
silently-corrected record cannot teach anyone that the error happened. The
correction lives here.

**The durable rule: an open item is a claim with an age even when YOU wrote it
sixty minutes ago.** Authorship is not evidence.

---

## 2. What was actually missing, and the gate that closes it

`decodeBoot42()` (`NV3050Probe.cpp:456`) returns `void` and writes four findings
to `NVLOG` and nowhere else. handoff40 gave the same four findings a value —
`nvChipVerdict()` in `phase0-kext/NVDecode.hpp` — and taught `publishIdentity()`
to publish it. What it did NOT do is require the two to keep describing the same
partition of one register.

Nothing kept them in step. Delete the GA106 arm from the log, or hoist the
dead-read test below the fall-through, and **all five gates stay green** while
the log and the IORegistry disagree about the same word.

`tests/test_probe_boot42_log_arms.py` pins six properties:

1. Every verdict the header declares has a log arm, with the arm markers read
   out of the source's own message strings.
2. The arm COUNT matches — property 1 alone is satisfied by a body that logs
   every marker unconditionally in one blob, so the two bare `return;`
   statements are pinned.
3. The dead-read test precedes the general fall-through. This is the ONE
   ordering claim handoff40 §3 measured as load-bearing, and the gate encodes
   the measured version rather than the version I first believed.
4. GA106 is nested INSIDE the mismatch arm, never beside it.
5. The verdict function is still in the dependency-free header, not the shell.
6. `decodeBoot42()` publishes nothing — it stays log-only, so there are not two
   publishers of one fact.

### 2a. What it does NOT claim, stated in the module

It does not prove the two agree. Proving agreement needs both compiled against
one input and no host suite compiles `NV3050Probe.cpp` — it reaches IOKit. This
is the weaker structural claim handoff40 §9 proposed: the arm count is DERIVED
from `NV_CHIP_VERDICT_LIMIT`, so a fifth verdict added to the header with no log
arm reddens on the commit that adds it, naming the verdict.

### 2b. The subject set is derived, and the markers are the source's own bytes

Every marker was read out of `NV3050Probe.cpp` before being written into the
pairing table — `handoff21.md` §1b's lesson, that a comparison against a value
the subject cannot produce is not a test but an `ok` line. A test asserts every
marker occurs in the live subject, so a paraphrase reddens rather than passing.

The GA106 marker is deliberately the die name inside the nested arm's MESSAGE
rather than its guard. The guard `chipId == NV_CHIP_ID_GA106` would still be
present if somebody moved the arm out of the mismatch case and beside it — which
is precisely the subset defect handoff40 §2b exists to prevent.

---

## 3. The regex that reddened correct shipping code

My first draft's positive control failed on a pristine tree, and the failure
named `NV_PART_ADA` and five other `NV_PART_*` verdicts as missing chip log arms.

The enum extractor was:

```python
r"typedef\s+enum\s*\{(.*?)\}\s*NVChipVerdict\s*;"   # with re.DOTALL
```

`NVDecode.hpp` declares `NVPartVerdict` **above** `NVChipVerdict`. So the regex
anchors on the FIRST `typedef enum {` in the file — the part one — and `.*?`
then scans forward across that enum's members, both name switches and
`nvPartVerdict()` until it finally reaches `} NVChipVerdict;`. The capture is the
WRONG enum's members plus several hundred lines of unrelated source.

The fix is a body class that cannot cross a brace:

```python
r"typedef\s+enum\s*\{([^{}]*)\}\s*NVChipVerdict\s*;"   # no DOTALL needed
```

This is `handoff25.md` §1c one construct over. There the lesson was that a
negated class excluding only the statement terminator is not scoped to the
statement; here a non-greedy wildcard with DOTALL is not scoped to the enum. In
both cases the pattern LOOKS scoped because the delimiter it names is the right
one — and in both cases the scoping has to come from the character class, not
from the delimiter.

Worth noting which direction this failed in: it reddened LOUDLY on correct code.
The silent direction — a regex that matched the right enum for the wrong reason
and would drift later — is the one that costs a session.

### 3a. The literal-wrap join, and why it is not optional here

`decodeBoot42()`'s dead-read message wraps mid-sentence across two adjacent C
string literals. `unexpected dead read` is therefore ABSENT from the raw body and
present only after the literals are concatenated the way the compiler
concatenates them (`a-wrapped-literal-hides-its-own-sentence`). A dedicated test
drives the real subject through the join so the control cannot rot into a
self-satisfying pair.

---

## 4. Coverage

**20 tests.** One positive control over the live tree, four structural controls
(the subject exists, the enum parses, the pairing table equals the header's
member set, every marker is the source's own spelling), four comment-stripping
controls, two wrapped-literal controls, and **nine negative controls** each
asserting WHICH property broke:

| Mutation | Property it must redden |
|---|---|
| delete the GA106 arm | `NV_CHIP_SIBLING_GA106` has no log arm |
| `nvLooksLikeDeadRead(value)` → `false` | the dead-read guard is gone |
| delete the GA107 match log | `NV_CHIP_TARGET` has no log arm |
| add a fifth verdict to the header only | names `NV_CHIP_SIBLING_GA104` |
| rename the subject function | REFUSES rather than passing |
| rename the enum tag | REFUSES rather than passing |
| `NVLOG` → `setProperty` inside the log fn | the log function must stay log-only |
| define the verdict in the shell | belongs in the dependency-free header |
| delete the dead-read `return;` | the bare-`return;` count moved |

The two REFUSE controls are the ones that matter most for a gate of this shape:
a module reporting nothing when its anchor moves is byte-indistinguishable from
a green one.

Comment stripping is mandatory rather than stylistic and has its own control
that MEASURES the fact — `NVDecode.hpp` discusses GA106 in prose above
`nvChipVerdict()`, so a gate reading prose would report every arm as present on
a function whose arms were all deleted. The control asserts the prose occurrences
exist and tells the next reader to re-measure if they ever stop.

---

## 5. Gate readings, and why three are RED without being mine

Measured after the last edit. **Three gates are red and none of the failures is
attributable to this session.** Attribution came first, by mtime and by the
failure MESSAGE, before anything was touched.

| Gate | Reading |
|---|---|
| `python tools/check-registers.py` | exit 0, **241/241** passed |
| `python tools/check-documented-paths.py` | exit 0, **134** documented paths |
| `tests\run-python-tests.bat` | exit 1, **Ran 1203 — FAILED (failures=3)** |
| `tests\build-msvc.bat` | exit 8, 10 of 11 suites, `test_intel_pixel` failed |
| `tests\check-kext-syntax.bat` | exit 1, `IntelPixel.hpp` not parsed standalone |

Every one of the five failures names `phase0-kext/IntelPixel.hpp` or
`tests/test_intel_pixel`:

- `test_check_header_coverage` — `IntelPixel.hpp` is dependency-free but the
  syntax gate's section 1 never parses it standalone.
- `test_check_count_floor` — `test_intel_scanout`'s binary predates its source.
- `test_guest_build_kext` — `IntelPixel.hpp` is not in `build-payload/kext-src.tar`.
- C++ gate — `test_intel_pixel` fails.
- Syntax gate — the same missing section-1 line.

The mtimes settle it. `IntelPixel.hpp` **21:53:18** and `test_intel_scanout.cpp`
**21:53:23**, both AFTER my last shipping edit (21:14) and after the fortieth
payload build (21:36). My only write this session is a `tests/*.py` module.

The correct action is to touch NOTHING, and specifically not to rebuild
`build-payload/`. Rebuilding would put a peer's header into the shipped tar and
sign the operator card for it under this session's name — while that header's own
C++ suite is RED and the syntax gate refuses it. That is the definition of
vouching for half-landed work (`intel-handoff27.md` §5a). handoff40 §5c made the
opposite call and was right to, because there the peer's files had STOPPED moving
and all five gates were green over the tree as it stood. Neither condition holds
here.

Register gate **240 → 241**: also the peer's, since `NVChipVerdict` is an enum
and this session added no `#define` at all. Python **1183 → 1203**, the +20
attributed by running the new module alone BEFORE the gate.

### 5a. A red gate is not a broken gate

All three reds are the gates working. The header-coverage gate caught a
dependency-free header nobody wired into section 1; the count floor caught a
binary older than its source; the payload gate caught a shipping file missing
from the tar. Each is a real, correct, actionable finding — for the Intel agent.

---

## 6. What this does NOT establish

1. **The log and the registry still agree by construction only.** This gate pins
   that both describe a four-way partition and that the arms are ordered and
   nested correctly. It does not execute either one. A future edit that changes
   what an arm MEANS while keeping its marker passes.
2. **`decodeBoot42()` is unchanged.** It still returns `void`. The verdict is
   computed independently from the same word in `publishIdentity()`, which is
   what makes the agreement a real hole rather than a theoretical one.
3. **Nothing here has executed on hardware**, and `NVChipSiblingNote` is still
   published on a path no machine in this project has ever taken — it requires a
   real GA106.
4. **Muxless still holds.** `PLAN.md` termination condition A is satisfied from
   two independent directions (the operator's report, handoff27 §1; the VBIOS DCB
   declaring zero display paths, handoff37 §0). Nothing this session moves a
   picture onto the internal panel, and nothing was meant to.

---

## 7. Open items, highest value first

Each of these was MEASURED during this session rather than carried forward.

1. **`CLAUDE.md`'s read-order list stops at `3050-handoff37.md`.** Handoffs 38
   through 41 are absent. Deliberately not edited again: the file is shared with
   the concurrent Intel agent, every path-shaped token in it is resolved by
   `tools/check-documented-paths.py`, and that gate is currently one of only two
   green ones. Worth one careful pass by whoever has the file uncontended.
2. **No log-arm gate covers `loadGspFirmware()`'s own refusal logging.** The
   stage VERDICT is published and gated (§1a), but the eight-way ucode verdict
   beneath it still reaches only `NVLOG`. This is the honest remainder of what
   handoff40 §9 item 2 was reaching for, stated as what it actually is.
3. **The Intel peer's tree is mid-landing.** Five gate failures across three
   gates, all naming `IntelPixel.hpp` / `test_intel_pixel` from 21:53. Nothing
   for this track to do; noted so the next NVIDIA session does not attribute
   them to itself and "fix" them.
4. **`test_intel_scanout` still has no `MEASURED` provenance entry**, for the
   fourth session. Adding one would vouch for a C++ suite this session never
   built successfully.
5. **One `kextload` on a machine containing the card** remains the single
   highest-value action in the whole project, and remains blocked on hardware,
   SIP, and the mutually-exclusive Linux/macOS choice on the target laptop
   (handoff27 §1).
