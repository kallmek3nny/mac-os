# 3050-handoff39 -- a bool with four preimages, one subsystem over

Direct continuation of `3050-handoff38.md`. It closes that file's section 6
item 3 -- `nvGspBootCheckRiscvErrors()`'s own distinctions -- and unlike the last
two sessions the inherited item was **TRUE as written**.

---

## 0. What moved

`phase0-kext/NVGspBoot.hpp` and `phase0-kext/NV3050Probe.cpp` both CHANGED, so
`build-payload/` was rebuilt -- the **THIRTY EIGHTH** image -- and
`OPERATOR-ACTION-step7-build.md` records it. `tests/test_gsp_boot.cpp` gained
section 7b, **341 -> 368** checks, and `tests/test_check_count_floor.py` carries
that reading with dated provenance.

Then, in the same session, `tests/test_probe_gsp_boot_publication.py` grew
property 6 and five controls, **16 -> 21** tests, closing the hole the fix above
left behind on the day it landed. Section 8 is that half. `phase0-kext/` did NOT
move again for it -- the tar's mtime is later than both source files', so the
thirty eighth image is still current and the payload gate's silence is the proof.

And then, still in the same session, section 8's own open item was closed as
well: `tests/test_probe_gsp_result_log_arms.py` is NEW, **12 tests**, over the
LOG half that section 8 pinned only the publication of. Section 9 is that third
half. Python **1132 -> 1144**; `phase0-kext/` still byte-identical since the
thirty eighth image, so no rebuild for either of the last two halves.

And then section 9's own successor item was closed too, which makes four halves
in one session: `tests/test_probe_identity_log_arms.py` is NEW, **16 tests**,
over `decodeBoot0()`'s log arms -- the SIBLING gap section 7 item 3 names and the
one `3050-handoff33.md` section 7 item 2 left standing. Section 10 is that fourth
half. Python **1144 -> 1160** for my part of it, against a final gate reading of
**1166** whose extra six are a peer's -- see section 6b, which is the reason to
quote both numbers rather than the one that flatters this session.
`phase0-kext/` is byte-identical still, so the thirty eighth image remains
current and no shipping code moved in either of the last three halves.

---

## 1. The inherited item was measured before it was worked

handoff38's own section 1 is about an item that was stale when it was written, so
its list was greped rather than trusted:

- **Item 1 was already closed**, and handoff38 says so itself:
  `phase0-kext/NV3050Probe.cpp` line 948 publishes `GSPErrorVerdict` through
  `nvRiscvErrorVerdictName()`.
- **Item 3 was TRUE.** A measurement coming back CONFIRMING the claim is worth
  recording on its own. Three sessions running have found an inherited item
  false, which is exactly what makes the fourth feel safe to assume in the other
  direction -- "it is probably stale too" is the same unmeasured guess with the
  sign flipped.

---

## 2. The defect: `true` had four preimages

`nvGspBootCheckRiscvErrors()` read both RISC-V error latches into
`m->privErrStat` and `m->hubErrStat`, then stopped the machine on
`nvRiscvHasError()`.

That function is a one-line wrapper spelled
`nvRiscvErrorVerdict(priv, hub) != NVRISCV_ERR_NONE`. The verdict family has
five members, so the bool is satisfied by **four of them at once** -- `PRIV`,
`HUB`, `BOTH`, and `UNREADABLE` -- and all four stopped the boot on
`NVGSP_RESULT_ERR_RISCV_FAULT`.

So a boot halted because one half answered with the `0xFFFFFFFF` refusal value
was reported as a latched card fault. That is `3050-handoff34.md` section 1's
class one subsystem over: **a bool is satisfied by every refusal at once**, and
the refusals are not interchangeable. `handoff22.md` section 1b is the half that
matters for the operator -- a set latch is a fact about the CARD and an engine
reset is the action, while a refused read is a fact about THIS DRIVER's mapping
and is the one somebody can go and fix. The collapse reported the second as the
first.

### 2a. The driver already drew this line, one path over

The sharpest part is that the same file already distinguishes these two
readings. `NV3050Probe.cpp`'s inspect path splits "the error latches did not
decode" out of its fault arm, in those words, and `NV3050Probe.hpp` line 222
carries the reasoning. The BOOT path did not, and the two paths read the SAME
two registers on the SAME card.

That is a new shape worth a name: not a distinction nobody had thought of, but
**a distinction made in one place and not in its sibling**. It is cheaper to
find than the unknowing cases (`handoff18.md` section 1d's family) because the
prose explaining why it matters is already committed -- and easier to miss,
because a reader who greps the file finds the correct arm and stops.

### 2b. Why 341 checks could not see it

Section 7 of `tests/test_gsp_boot.cpp` already drove the fault arm. Every latch
it plants is a small non-zero value, and a set latch and a refused read both
satisfy `!= NVRISCV_ERR_NONE` identically. The suite's assertions could not tell
the fix from the bug it fixes -- verbatim handoff34 section 1's problem, in a
different file, found by asking the same question of a different bool.

The C++ suite is also the only gate with anything to say here: the register gate
checks `#define`s and this is an enum, the syntax gate parses and both spellings
parse, and the Python gate does not model result-code semantics.

---

## 3. The fix and its four load-bearing choices

`NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE` joins the result enum beside
`NVGSP_RESULT_ERR_RISCV_FAULT`, and each of these was decided rather than
defaulted:

1. **The UNREADABLE test runs FIRST, and it is an EQUALITY.** It is
   `errVerdict == NVRISCV_ERR_UNREADABLE`, never a negation of `NONE` with cases
   subtracted. A negation form sweeps a sixth verdict added later into the fault
   arm silently, and the fault arm is the one that makes a claim about the
   customer's hardware. `3050-handoff32.md` section 3 argued this for
   `!= ERR_HEADER` and `3050-handoff33.md` for `NVPartIsTarget`; this is the same
   rule at a third site.
2. **Only TWO result codes, not five.** `PRIV`, `HUB` and `BOTH` deliberately get
   no codes of their own -- they ride in the new `NVGspBoot::errVerdict` field. A
   result code answers "what stopped the boot"; the verdict answers "which
   latch". Multiplying result codes for the second question would put a
   card-fault taxonomy inside an enum whose every other member describes this
   machine's own progress.
3. **The verdict is recorded BEFORE any stop.** A field written only on the clean
   arm would be absent from exactly the run an operator is reading it for.
4. **`nvGspBootErrVerdictKnown()` is a CONJUNCTION of both `readMask` bits.** The
   field's initialiser is `NVRISCV_ERR_NONE`, which is ALSO the healthy reading,
   so an unguarded publication reports a run that never reached the error check
   as one that checked and found nothing -- `3050-handoff29.md` section 2's
   fabricated-publication class, which is why that `readMask` exists at all. A
   sixth "never decoded" sentinel was deliberately NOT invented; the mask already
   answers the question and a second answer can disagree with the first.

`publishGspBootResult()` publishes `NVGspBootErrVerdict` only behind that guard,
and the refusal log arm now says the stop happened WITHOUT a fault having been
established and names the driver as the place to look.

---

## 4. Non-vacuity: two mutations, run separately

**Mutation A -- restore the collapse.** Put `nvRiscvHasError(...)` back as the
stopping condition. Reddened exactly **3 checks, all three new, every
pre-existing check green**:

```
PRIV refused stops UNREADABLE, not FAULT
HUB refused stops UNREADABLE, not FAULT
an unreadable half BEATS a real latch in the other
```

All three report `got ERR_RISCV_FAULT, expected ERR_RISCV_ERR_UNREADABLE`, which
is the defect's own signature. `handoff18.md` section 1e's rule is what makes
this evidence: a mutation that also reddened old checks would have proved the old
tests already sufficed.

**Mutation B -- weaken the conjunction** to `NVGSP_READ_PRIV_ERR_STAT` alone.
Reddened exactly **1 check**: `PRIV alone is not a verdict -- got true, expected
false`. B exists because A cannot isolate it.

### 4a. The conjunction had no separating case until it was given one

The first green build read 364, and that reading was weak for a reason no
mutation would have surfaced: **no run in the whole file separates the two mask
bits**. The refusal VALUE arrives through a read that SUCCEEDED, so a run that
reads PRIV always reads HUB and both bits always land together. A single-bit
`nvGspBootErrVerdictKnown()` would have passed every check in the suite.

The fix is a hand-driven `readMask` block -- PRIV alone false, HUB alone false,
both true, and `~(PRIV|HUB)` false -- which is what took the count 364 -> 368 and
what mutation B then reddened. The durable point: **a conjunction needs a case
where the halves disagree**, and if the machine cannot produce one, drive the
field directly rather than concluding the property is untestable.

---

## 5. Two of my own mistakes worth carrying

1. **`OBS_CPUCTL_RUNNING` does not exist.** I used it in the PROBE-stop fixture.
   The suite spells a not-halted core as a bare `0u` -- the `OBS_CPUCTL_HALTED`
   bit simply clear. Caught by grepping the constants before building rather than
   by the compiler, which would also have caught it; the reason to prefer the
   grep is that a hand-written bit VALUE in place of a missing name compiles and
   becomes a silent second copy of a field layout.
2. **The new section was not wired into `main()` on the first pass.** An unwired
   section compiles clean and contributes zero checks. The count is the only
   thing that reports it, which is what the count-floor table is for.

---

## 6. Gate readings

All five green. Every number below was read from a run in this session.

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped, exit 0 |
| `tests/build-msvc.bat` | 10/10 suites passed, exit 0 |
| `tests/check-kext-syntax.bat` | `ALL SYNTAX GATES PASSED`, exit 0 |
| `tests/run-python-tests.bat` | **1166** tests, OK, exit 0 (see 6b) |
| `tools/check-documented-paths.py` | **134** paths accounted for, exit 0 |

The register gate is correctly unmoved at 240: the new result code is an enum
member, not a `#define`, and `tools/check-registers.py` parses `#define`s against
NVIDIA's headers. `test_gsp_boot` reads **368**, up from 341, which lands INSIDE
its own band (341 + max(8, 34) = 375) by seven -- so the ceiling correctly did
not fire and the table entry is a plain measurement rather than a raised bound.

### 6a. The Python gate was RED twice, both mine

First reading: one failure, `test_guest_build_kext` payload freshness, naming
`NVGspBoot.hpp` and `NV3050Probe.cpp` -- exactly the two files I had edited. The
rule is attribute by mtime and by the failure MESSAGE, and here the message named
my own work, so rebuilding was correct. Second reading: two failures, the second
being `test_operator_step7_doc` asserting the live tar size appears in the card.
The card still quoted 419840 after the rebuild made it 430080. Both are the same
staleness at one remove -- rebuilding the payload creates the doc's staleness --
and the sizes are DERIVED by that test rather than hardcoded, which is why it
catches the second half.

### 6b. The final reading is 1166, and six of those are not mine

The last gate run printed **1166**, not the 1160 this session's own work accounts
for. The +6 was attributed rather than assumed, and the evidence is three
measurements:

- `tests/test_probe_identity_log_arms.py` runs **16** alone, mtime
  `2026-08-19 20:14:57` -- mine, and 1144 + 16 = 1160.
- `tests/test_capture_vs_header.py` runs **10** alone, mtime
  `2026-08-19 20:29:20` -- fourteen minutes AFTER my module landed, and I never
  opened it. It is an Intel-track capture module.
- The gate reading that printed 1160 came before that mtime; the reading that
  printed 1166 came after.

So a module I do not own grew by six underneath the session. Nothing was done
about it: no `MEASURED` entry, no edit, no rebuild. Both numbers are quoted
because quoting only 1166 would claim a peer's six checks under this session's
name, and quoting only 1160 would describe a gate reading that no run produced.
This is `handoff18.md` section 3's rule for the fourth or fifth time -- a gate's
verdict in a two-agent tree is a JOINT measurement -- with the new detail that it
applies to a GREEN reading too, not just a red one. A count that grew in your
favour is as much a peer's work as a failure that appeared in it.

---

## 7. What this does NOT establish

1. **Neither new arm has ever run on hardware.** `ERR_RISCV_ERR_UNREADABLE` is
   reachable only through the fixture. The refusal value it keys on is what
   `regRead32()` returns on a refused read, and no run of this driver has ever
   read a real GA107 register.
2. **A latch genuinely holding all-ones reports UNREADABLE.** This is the honest
   ambiguity `nvRiscvErrorVerdict()` already documents, inherited rather than
   introduced, and it is written in the header instead of being papered over.
3. **The log and the registry agree by construction only.** No gate covers the
   agreement between the new log arm and the published verdict -- the same gap
   `3050-handoff33.md` section 7 item 2 names for `decodeBoot0()`. **Partly
   superseded within this session: section 10 closed the `decodeBoot0()` half,
   and its property 6 pins the Ada log arm to `NVPartRetargetNote`. This item
   still stands for `attemptGspBoot()`'s own arms, where the sentence and the
   published verdict are pinned separately and nothing relates them.**
4. **The absence finding is unchanged.** This session added no evidence about the
   board, and the muxless finding still means a complete NVIDIA driver puts
   nothing on the internal panel.

---

## 8. The hole the fix left, closed in the same session

The verdict publication landed guarded, and **deleting that guard left all four
gates green**. `publishGspBootResult()` lives in the IOKit shell, so no host
suite compiles the translation unit; clang has nothing to say about a
well-formed `setProperty()` with one fewer `if` in front of it. This is
`3050-handoff30.md` section 1's shape exactly, one key later.

`tests/test_probe_gsp_boot_publication.py` was EXTENDED rather than duplicated,
for that module's own stated reason: a second module over the same publisher
would duplicate the `audit()` this family shares with the live tree. 16 -> **21**
tests.

### 8a. Why the verdict is not a member of `TRIPLES`

The five diagnostic words are each guarded by ONE `NVGSP_READ_*` bit, and the
table pairing bit to field to key is what properties 3-5 walk. The verdict is
guarded by a CONJUNCTION of two bits, so it does not fit that table -- which is
precisely why properties 1-5 had no opinion about it and why deleting its guard
was invisible. Property 6 is its own walk; the only thing folded into the older
properties is the ordering check, since the verdict is qualified by the mask
exactly as the five words are and so must sort below the mask like they do.

### 8b. The conjunction half is read out of the HEADER

A publisher calling a correctly-named guard that has been weakened to one bit is
byte-identical, in the shell, to a correct one. So the gate reads
`nvGspBootErrVerdictKnown()`'s own body and requires the set of
`NVGSP_READ_*` tokens in it to EQUAL both latch bits -- derived, never spelled,
for property 5's reason.

That arm is load-bearing rather than decorative, and section 4a of this file is
why: **no run this machine can produce reads one latch without the other**,
because the refusal value arrives through a read that SUCCEEDED. A single-bit
guard is green over all 368 checks in `tests/test_gsp_boot.cpp`. The C++ suite
cannot reach this property at all, and the Python gate can only reach it by
reading the source.

### 8c. Five controls, each reddening exactly one check

Measured by driving `audit()` directly and printing the FULL failure list, not
by asserting one message -- a control that asserts one string does not establish
that the mutation left the other properties alone.

| Mutation | Failures |
|---|---|
| delete the verdict guard | **1**, the unguarded-publication message |
| delete the publication entirely | **1**, never published |
| render with `nvGspBootResultName()` | **1**, wrong name function |
| weaken the conjunction to PRIV alone | **1**, consults one bit |
| rename the guard in the header only | **1**, subject not found |

Live tree: **0**. Every pre-existing check stayed green under all five, which is
`handoff18.md` section 1e's signal -- a mutation that also reddened an older
property would have proved the older properties already sufficed.

The fifth is the vacuity control rather than a defect control: a gate that
cannot find its subject must REFUSE, because reporting nothing is
indistinguishable from reporting green.

---

## 9. The LOG half, closed in the same session

Section 8's own open item 2 -- the half section 8 did NOT close -- is now closed
too. `tests/test_probe_gsp_result_log_arms.py`, **12 tests**, over
`attemptGspBoot()`'s `switch (machine.result)`.

The publication half is guarded and pinned. The SENTENCE an operator actually
reads is neither. Each terminal result renders through one `case` arm, and the
two RISC-V arms are where the whole point of section 3's split lives -- the
fault arm says the latches are a fact about the CARD and an engine reset is the
action, the refusal arm says the stop happened WITHOUT a fault and names THIS
DRIVER. Delete either and the result falls to `default:`, which prints a generic
line; **all five gates stay green**. The register gate parses `#define`s and
these are enum members, both C++ suites and the syntax gate cannot reach an
IOKit translation unit, clang has nothing to say about a `case` that is merely
absent, and no Python gate had ever read that switch -- confirmed by grep before
anything was written, not assumed.

### 9a. What is deliberately NOT asserted, and why the set-difference came first

The obvious gate is "every stop code has an arm." It is WRONG, and its failing
test would have been the positive control for the fourth session running.
Measured before writing: **fourteen** codes reach `nvGspBootStop()` and the
switch carries **nine** arms, so five -- `ERR_DMA_TIMEOUT`,
`ERR_FIRMWARE_INVALID`, `ERR_NOT_RISCV_CAPABLE`, `ERR_NO_IO`, `ERR_STEP_BUDGET`
-- legitimately fall to `default:` and are correct as written.

The subject is therefore the PAIR section 3 created, not the family. The pair
itself is still DERIVED -- read out of `nvGspBootCheckRiscvErrors()`'s own stop
sites in `NVGspBoot.hpp` -- so a third RISC-V code added later with no arm
reddens on the commit that adds it, naming the code. Spelling the pair here
would have made that growth silent.

### 9b. Presence alone passes a swap

Each arm's reading is held as markers in the arm's own log text, and each is
asserted PRESENT in its own arm and **ABSENT from its sibling**. Presence alone
is satisfied by a switch whose two bodies were exchanged, because both readings
still occur somewhere in the switch -- and that mutation is the exact defect
section 3 removed, restored in the log only, with the result codes still split
and publication property 6 still green.

### 9c. The gate must join wrapped string literals

First draft went RED on correct shipping code, and the anchor matched **zero**
times. `NVLOG` messages are wrapped across lines, so the sentence exists only
after the compiler concatenates adjacent literals -- a marker straddling the
wrap is absent from a file that plainly contains it. Worse in the other
direction: re-wrapping a line to fit a column limit would redden a gate that has
nothing to say about the change. `join_wrapped_literals()` runs first, which
makes every assertion a property of the MESSAGE rather than of its typography.

### 9d. A suffixed rename is not a vacuity control

Measured, not reasoned: renaming the subject to `attemptGspBootRenamed` yields
**0 failures** -- the fragment `attemptGspBoot` is a PREFIX of it, so `find()`
locates the function and the control passes for the wrong reason, reporting a
vacuity arm as working when it never ran. Both vacuity controls rename to names
the fragment is not a prefix of (`beginGspBringup`,
`nvGspBootScanRiscvLatches`). Same family as the substring-passes-a-suffixed-
rename note already in the memory index, arrived at here from the control side.

### 9e. Eleven mutations, measured against the full failure list

| Mutation | Failures |
|---|---|
| delete the fault arm | **1**, falls to `default:` |
| delete the unreadable arm | **1**, falls to `default:` |
| fault arm loses its CARD reading | **2**, one per marker |
| unreadable arm loses its DRIVER reading | **2**, one per marker |
| unreadable arm CLAIMS a card fault | **3**, sibling's markers plus its own loss |
| the two arm bodies SWAPPED | **8** |
| fault arm drops the verdict | **1**, no `nvRiscvErrorVerdictName()` |
| a THIRD RISC-V stop code, no arm | **1**, names the code |
| subject function renamed | **1**, refuses |
| check function renamed | **1**, refuses |
| suffixed rename (`...Renamed`) | **0** -- see 9d |

Live tree: **0**. The swap at 8 is the loudest and is NOT evidence the other
checks were redundant -- it is one edit touching both arms and both markers in
each, so eight is the arithmetic rather than a signal. The two arms are
symmetric, which is why the per-marker mutations come in pairs.

The last row is a measurement of the trap, not a control in the suite.

---

## 10. The sibling half: `decodeBoot0()`'s log arms

Section 7 item 3 named this and `3050-handoff33.md` section 7 item 2 named it
first. It was measured before it was worked, per handoff38 section 1, and the
claim was **TRUE**: `decodeBoot0()` writes four findings to `NVLOG` and no gate
reads that function at all. The grep is the whole measurement -- `decodeBoot0`
occurs in `tests/` only in `test_check_count_floor.py`'s provenance comment, in
`test_decode.cpp`'s header prose about `nvPartVerdict()`, and in
`test_probe_identity_publication.py`'s comment explaining where the four findings
came from. Three mentions, all in comments, none an assertion.

Delete the Ada arm's `NVLOG` and all five gates stay green, for the four
by-construction reasons this project keeps re-deriving: the register gate parses
`#define`s and these are enum members plus log text; no host suite compiles
`NV3050Probe.cpp`; clang has nothing to say about a deleted `if` around a
well-formed call; and the existing publication gate is about the REGISTRY side.

The answer is `tests/test_probe_identity_log_arms.py`, **16 tests**, six
properties, in the idiom of `tests/test_probe_gsp_result_log_arms.py`.

### 10a. The set-difference was run first, and it killed the obvious gate

Section 9a's trap, hit again and avoided the same way. `decodeBoot0()` has **six**
log arms; `NVPartVerdict` has **four** members. The two extra arms are the
dead-read cases -- `0xFFFFFFFF` (device not responding) and `0x00000000` (mapping
points at nothing) -- and neither has a verdict counterpart, correctly, because
`nvPartVerdict()` decodes a word that already read back. A gate demanding a
verdict per log arm would have reddened correct shipping code, and its failing
test would have been the POSITIVE CONTROL for the fifth session running.

So the mapping is **one-directional**: every verdict must have an arm; an arm
without a verdict is fine. That direction is still load-bearing, because the
verdict set is DERIVED from the header's own `typedef enum` rather than spelled
in the test -- a fifth member added with no log arm reddens on the commit that
adds it, naming the member.

### 10b. `NV_PART_AMPERE_OTHER` is an `else`, not an `if`

Three of the four arms open with a condition; the fourth is the `else` of the
GA107 test. That is correct as written -- "Ampere but not GA107" is the
complement of "GA107" inside the Ampere branch, and giving it its own `if` would
produce an arm guarded by a condition the compiler already knows. So `ELSE_OF`
maps it to its parent and `if_arm()` returns the else span alongside the then
span. A suite that only ever mutated `if`-shaped arms would leave that path
unproven, which is why one control targets it specifically.

### 10c. Nested arms must be subtracted or disjointness is free

The Ada arm sits INSIDE the not-Ampere arm. Read naively, the enclosing arm's
text contains the nested arm's markers, so the disjointness property could never
fail and the swap control would pass on any tree. `arm_texts()` subtracts every
span strictly contained in another before comparing.

Property 4 pins the nesting itself, and it is the same reasoning
`3050-handoff33.md` section 2 gives for the Ada test running FIRST inside
`nvPartVerdict()`: `NV_PART_ADA` is a SUBSET of "not Ampere". Hoisted out to sit
beside the general test, the arm lands below a `return` and cannot execute at
all -- the dead-arm shape that handoff removed, reintroduced one function over.

### 10d. Twelve mutations, measured against the full failure list

| Mutation | Failures |
|---|---|
| Ada arm loses its retarget sentence | **1** |
| not-Ampere arm loses its diagnosis | **1** |
| `AMPERE_OTHER` (the `else` arm) loses its sentence | **1** |
| TARGET arm also claims a plan retarget | **1**, names the sibling |
| Ada test hoisted out of the not-Ampere test | **1**, `SUBSET` |
| Ada publication guard retargeted to TARGET | **1** |
| retarget note KEY renamed | **1** |
| a FIFTH verdict added with no log arm | **1**, names the member |
| a verdict loses its `nvPartVerdictName()` arm | **1** |
| subject function renamed | **1**, refuses |
| publisher renamed | **1**, refuses |
| verdict enum renamed | **1**, refuses |

Live tree: **0**. Every mutation reddens exactly one property and no other,
which is the cleanest reading this family has produced -- unlike section 9e's
symmetric arms, these four findings are distinct, so a single edit cannot reach
two of them. Note that handoff18 section 1e's every-pre-existing-check-green rule
does not apply to an all-new module; what the twelve establish instead is that
no property passes by measuring nothing.

### 10e. Property 6 keeps the log and the registry pinned together

`3050-handoff33.md` section 7 item 2's honest limit was that the log and the
registry agree "only as long as nobody edits one arm of `decodeBoot0()` without
the other, and no gate covers that agreement". Property 6 covers the Ada half of
it specifically: the `part == NV_PART_ADA` guard and the `NVPartRetargetNote` key
must both still be in `publishIdentity()`. That is the one finding of the four
with an operator action attached, so it is the one where losing either end
matters most.

---

## 11. Open list

Ordered by value, hardware-blocked items last. **Each of these is a claim with a
date on it -- measure it before working it, per handoff38 section 1.**

1. **The Intel framebuffer is the critical path to a picture**, and it belongs to
   the Intel track by the operator's own division of labour. Do not start a
   second `IOFramebuffer` subclass here.
2. **`decodeBoot42()`'s log arms are the third member of this family and have no
   gate.** Sections 9 and 10 closed the result switch and `decodeBoot0()`;
   `decodeBoot42()` at `phase0-kext/NV3050Probe.cpp:456` is the one left. Measure
   before working it: unlike its two siblings it has no verdict enum to derive a
   subject set from, so the first question is whether a shape gate over it can
   assert anything that is not just a spelling of its own log text. If the answer
   is no, the honest move is to give the findings a verdict family in
   `NVDecode.hpp` FIRST -- `nvPartVerdict()` is what that looks like done --
   rather than write a gate that pins typography. `nvLooksLikeDeadRead()` already
   has a consumer here (section 1 of `3050-handoff31.md`), so the dead-read arm is
   not the gap; the decoded-field arms are.

   What sections 9 and 10 did NOT establish, and what a third member would have
   to: nothing pins the log arms of these three functions to each other, only
   each function's arms to its own publication. Three modules sharing no `audit()`
   is the direction `3050-handoff30.md` section 2 warns about from the other side.
3. **Reference `tools/parse-dcb.py` and `tools/find-display-matches.py` from
   `CLAUDE.md`.** Carried from handoff38 section 6 item 1, still true, still
   deliberately not done: `CLAUDE.md` is the one file both concurrent tracks
   edit. Do it in a session where the Intel agent is idle.
4. **One `kextload` on a machine containing the card.** Unchanged, still the
   highest-value action in the project, still blocked on hardware.
