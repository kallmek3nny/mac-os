# 3050-handoff40

NVIDIA track. Direct continuation of `3050-handoff39.md`, and it closes that
file's **§11 item 2** — `decodeBoot42()`'s log-only findings. Third member of the
log-only-diagnosis family after the result switch (handoff39 §9) and
`decodeBoot0()` (handoff39 §10), and the item was TRUE as written.

Both shipping files changed, so `build-payload/` was rebuilt — the **FORTIETH**
image — and `OPERATOR-ACTION-step7-build.md` carries its entry.

**Read §3 first.** It is the only genuinely new thing in this session, and it is
about mutation testing rather than about GPUs: a mutation that reddens NOTHING is
not automatically evidence that the tests are vacuous. It can mean the CLAIM was
false. Mine was.

---

## 1. What was wrong

`decodeBoot42()` in `phase0-kext/NV3050Probe.cpp` (`:456`) has always detected
four facts about the die, and has always written all four to `NVLOG` and nowhere
else. It returns `void`:

1. An unexpected dead read — BOOT_0 answered, `NV_PMC_BOOT_42` came back all-ones
   or all-zeroes. It logs *aperture may be shorter than 16 MiB* and returns
   without decoding a single field.
2. The composite chip ID matching GA107.
3. The chip ID not matching.
4. The not-matching value being **GA106 specifically** — the
   'RTX 3050 Ti Laptop GPU'. Same Ampere architecture, different die, different
   unit counts, and the near miss most likely to be mistaken for the target part.

### 1a. What the registry said instead

`publishIdentity()` published `NV_PMC_BOOT_42` as a raw 32-bit word and
`NVChipIdExpected` as a statement of what this driver wants, **and never compared
the two**. So the IORegistry left "is this the die the driver was written for?"
to whoever happened to be reading it, and finding 4 — the one with an operator
consequence attached, because every register offset in this driver was derived
for GA107 — existed only in the log.

This is weaker than handoff31 §1's defect (which published a fabricated word) and
stronger than nothing: no value was invented. What was lost is a *verdict* the
driver had already computed.

### 1b. Why the gates were blind

Same four by-construction reasons, re-derived rather than copied:

- The register gate checks `#define`s against NVIDIA's headers. Every constant
  here is correct; the join between them is not a constant.
- No C++ suite compiles `NV3050Probe.cpp` — it reaches IOKit. A decision written
  in the shell is a decision nothing runs.
- `-Werror` has nothing to say. A `void` function that logs and returns is
  well-formed.
- `tests/test_probe_identity_publication.py` existed and was GREEN with 21 tests,
  because its properties are about BOOT_42's *readability* — the dead-read guard,
  the flag on both arms, the derivation. Nothing asked what the readable path
  says about the DIE it read. Same shape as handoff31 §1a's fourth entry and
  handoff33 §1b's fourth entry, for the third time.

---

## 2. The fix, and its three load-bearing choices

### 2a. The decision goes in the header

`phase0-kext/NVDecode.hpp` gained `NVChipVerdict`, `nvChipVerdictName()` and
`nvChipVerdict()`, appended below `nvPartVerdict()` — these headers have no
forward declarations, and both dependencies sit above it (`nvLooksLikeDeadRead()`
at `:192`, `nvBoot42ChipId()` at `:233`).

It is `nvPartVerdict()`'s sibling one register over and exists for the same
reason. A pure function written in a shell is a pure function nothing runs.

### 2b. GA106 sits INSIDE the mismatch case, not beside it

GA106 is a **subset** of "not GA107". Written as two sibling `if`s, whichever
comes first wins and the other arm is dead — the shape handoff32 exists to
remove, handoff33 §2 hit one level up with Ada, and this is its third appearance.
The suite pins it with a check named *GA106 is not swallowed by the general
mismatch arm*.

### 2c. `NV_CHIP_UNREADABLE` is the ZERO value

So a verdict variable that was never assigned reads as "not measured" rather than
as the target part. `test_decode.cpp` asserts the numeric zero rather than
trusting declaration order.

### 2d. Publication is an EQUALITY, never a negated error arm

In `publishIdentity()`, immediately after `setProperty("NVBoot42Readable", …)` —
so on BOTH arms and ahead of the word it qualifies:

```c
    NVChipVerdict chip = nvChipVerdict(boot42);

    setProperty("NVChipVerdict", nvChipVerdictName(chip));
    setProperty("NVChipIsTarget", chip == NV_CHIP_TARGET);

    if (chip == NV_CHIP_SIBLING_GA106) {
        setProperty("NVChipSiblingNote", …);
    }
```

`NVChipIsTarget` is `chip == NV_CHIP_TARGET`, never `chip != NV_CHIP_SIBLING_GA106`.
A negation is true for a dead read and for every chip ID this tree has never
seen, so it would report an unmeasured register as the target die. Same reasoning
as handoff33 §2's `NVPartIsTarget` and handoff32 §3's `!= ERR_HEADER`, and the
third time the negated-error-arm form has been rejected in this shell.

---

## 3. The mutation that lied, and in which direction

This is the section worth carrying.

My first draft's header comment said the ordering was load-bearing **twice** and
that both were subset relations: that the dead-read test must precede the chip ID
tests, because otherwise a dead read would be stolen by a mismatch arm.

Mutation A hoisted the dead-read test below both chip ID equality tests. It
reddened **ZERO checks**.

The reflex from handoff18 §1e is that a mutation reddening nothing means the test
is vacuous. That reflex is wrong here, and the honest reading is the other one:
**the claim was false.** Measured from the header's own constants —
`NV_PMC_BOOT_42_CHIP_ID_MASK` is `0x3FF`, so `CHIP_ID` is ten bits:

| BOOT_42 word | decoded chip ID | equals GA107 `0x177`? | equals GA106 `0x176`? |
|---|---|---|---|
| `0xFFFFFFFF` | `0x3FF` | no | no |
| `0x00000000` | `0x000` | no | no |

So the two equality arms **cannot** steal a dead read, and moving the dead-read
test past them is behaviour-preserving. My comment had also written `0x1FF`,
which is plainly wrong for a ten-bit field — an arithmetic slip in prose that the
mutation is what caught.

What IS load-bearing is the dead-read test's position relative to the general
**fall-through**. Two mutations isolate exactly that, and each reddens exactly
**2 checks**:

- **A1** — delete the dead-read test outright. Both dead words land on
  `NV_CHIP_OTHER`, i.e. "wrong die": a statement about silicon, made over a
  register nobody successfully read.
- **A2** — move the test below the general `return NV_CHIP_OTHER`. The literal
  dead-arm shape: the test is still spelled and can never execute.

Two consequences, both landed:

1. The header and test comments were rewritten to the measured facts, and the
   "both are subset relations" claim was downgraded to one. The comment now says
   explicitly what the ordering does NOT depend on, and dates the measurement,
   because the next reader's reflex will be the same as mine was.
2. Four checks were added pinning the **non-collision** through
   `nvBoot42ChipId()` rather than by spelling `0x3FF`:

```c
    checkBool("all-ones chip ID is not GA107",
              nvBoot42ChipId(0xFFFFFFFFu) != (NVU32)NV_CHIP_ID_GA107, true);
```

…and the three siblings. The surviving ordering claim now rests on an asserted
fact rather than on arithmetic in a comment, and a future chip ID landing on
`0x3FF` or `0x000` reddens on the commit that adds it.

### 3a. The probe harness, and the trap it was built to avoid

Both mutation runs went through `tests/build-msvc.bat` rather than a bare
`cmake --build`/`ninja`. A bare generator invocation from a shell with no MSVC
`INCLUDE` fails on `<cstdio>` and **leaves the old binary in place** — every
mutation then reads as vacuous for a reason that has nothing to do with the
subject (handoff28 §4a; it cost a round trip here too, on the first draft of the
probe). The probe also asserts the exe mtime POSTDATES the header write, so a
failed compile can never report a planted defect as a no-op.

Anchor counts were asserted in both directions before each mutation applied, so a
drifted anchor raises instead of silently matching nothing.

The header was mutated and restored by md5 nine times and ends the session as
intended. Probe scripts lived in `%TEMP%`, never under `tests/` — a stray `.py`
there is picked up by unittest discovery and becomes an unnumbered member of the
Python gate. All probe files have been deleted, including a stray
`tests/build/_chipid_probe.cpp` whose absence was verified.

---

## 4. Execution coverage

`tests/test_decode.cpp` gained `testChipVerdict()`, wired into `main()` directly
below `testPartVerdict()`. **121 → 148 checks.**

Its first check is the composer anchor, and it is not decoration: the fixture
builds a BOOT_42 word through `NV_PMC_BOOT_42_CHIP_ID_SHIFT`, the same `#define`
the accessor reads back, so without an anchor against the real `kBoot42_GA107`
word the fixture and the accessor cancel and every check below tests two copies
of one belief. Mutation G (shift 20 → 21) reddens it.

Beyond the arms themselves: the subset pin, the two dead-read arms, the two
"a dead read is not the target die" checks, the numeric zero of
`NV_CHIP_UNREADABLE`, a name-coverage loop bounded on `NV_CHIP_VERDICT_LIMIT`
with the sentinel read out of the subject, a pairwise-distinct double loop (a
copy-paste arm returning a sibling's string satisfies every "has a name"
assertion — handoff21 §1b), and a cross-family check that `NVChipVerdict` and
`NVPartVerdict` do NOT share a sentinel string. Seven mutations were run; each
reddened its own property with the pre-existing 121 staying green.

148 is **above** the previous ceiling (121 + max(8, 12) = 133) by fifteen, so the
count band fired on this reading rather than absorbing it.

---

## 5. Two gate failures I caused, and how they were attributed

The Python gate went red with three failures. Attribution came first, by mtime
and by the failure MESSAGE — a gate's verdict in a two-agent tree is a joint
measurement.

### 5a. The count table (mine, expected)

`test_check_count_floor.py` fired its CEILING on `test_decode` at 148 against a
table entry of 121. Correct behaviour: the band exists to force a re-measurement
rather than to absorb growth. Re-measured to 148 with dated provenance recording
what the new section covers and why four of the checks exist.

### 5b. A negative control whose anchor spanned two statements (mine, NOT expected)

`test_probe_identity_publication.py`'s
`test_the_readable_flag_inside_the_guard_is_caught` failed — and it is a POSITIVE
failure mode worth naming, because the control failed for a reason with nothing
to do with its own property. Its mutation anchored on:

```
    setProperty("NVBoot42Readable", boot42Readable);

    if (boot42Readable) {
```

That is two statements and the gap between them, i.e. a silent claim that nothing
ever lands there. My chip verdict block landed there — correctly — so the anchor
occurred ZERO times, `mutate()` raised as designed, and the control reddened.

**An anchor spanning two statements is a claim about everything between them, and
the next correct edit to the subject breaks it.** Split into two independent
edits: delete the flag publication, then insert it inside the guard. Both anchors
were checked for uniqueness in the subject first — `NVBoot42Readable` occurs at
`:1068` and `:1201`, the second inside `publishIdentityUnreadable()`, and only the
first matches the deleted spelling.

This is the counterpart to handoff26 §3's over-match: that one raised loudly
because an anchor matched twice, this one raised loudly because a correct
insertion made it match zero times. Both are the loud direction; the silent
direction is still the dangerous one.

### 5c. Payload freshness (mixed attribution, and the call made)

Four files stale in `build-payload/kext-src.tar`: `NVDecode.hpp` (21:14) and
`NV3050Probe.cpp` (20:57), mine; `IntelDisplayNames.hpp` (21:13) and
`IntelScanout.hpp` (21:13, then 21:25), the concurrent Intel track's.

The tar is a whole-directory snapshot with no per-file staleness, so shipping my
two means shipping theirs. The rule from intel-handoff27 §5a is not "never touch
a red gate" — it is to attribute first and not to vouch for a peer's HALF-LANDED
work. So: `IntelScanout.hpp` was re-measured after a 45-second wait and had
stopped moving (21:25:57 both times), and all five gates were green over the tree
as it stood. Rebuilt on that basis, and the card says in words that it vouches
for the NVIDIA half only.

Rebuilding then reddened `test_operator_step7_doc` on two derived values (the ISO
md5 and the tar byte size the card quotes) — the card carries the fingerprints,
so a rebuild without a card edit is an incomplete change. Both cleared once the
fortieth entry landed.

---

## 6. Gate readings

All five green, measured after the last edit.

| Gate | Reading |
|---|---|
| `python tools/check-registers.py` | exit 0, **240/240** passed, 0 failed, 0 skipped |
| `tests\build-msvc.bat` | exit 0, 100% tests passed, **10 of 10** suites |
| `tests\check-kext-syntax.bat` | exit 0 |
| `tests\run-python-tests.bat` | exit 0, **Ran 1183 tests — OK** |
| `python tools/check-documented-paths.py` | exit 0, **134** documented paths accounted for |

Register gate correctly **unmoved at 240**: `NVChipVerdict` is an enum and three
property KEYS, not a `#define` describing hardware. Python total correctly
**unmoved at 1183** — two existing modules were edited, no module added.

Every exit status was captured before any pipe. `$?` after a pipeline is `tail`'s
status, and a `cmd /c` wrapper on a `.bat` yields a banner and no run; both were
hit and corrected earlier in this session.

---

## 7. Payload

**FORTIETH** image, rebuilt 2026-08-19 21:36.

| | before | after |
|---|---|---|
| `kext-src.tar` | 430080 | **440320** |
| `step7-payload.iso` | 507904 | **518144** |
| tar md5 | `85bd23145082b07502589aeede973d3b` | `80586918574534f3709c9ed054f60d72` |
| ISO md5 | `1ad2f821d6119d4b566fd906b9ca1046` | `a04fea290678d76c8d0fe05a7eb62069` |

Both sizes moved by exactly one 10240-byte tar block. That is a coincidence of
padding, not a signal — the thirty ninth rebuild moved NEITHER size while both
md5s changed. A byte size is not a fingerprint.

---

## 8. What this does NOT establish

1. **The verdict has never executed on hardware.** `nvChipVerdict()` is covered
   by 27 host checks; the three `setProperty()` calls that consume it are
   reachable only by parse, because no host suite compiles the shell. Same limit
   as every publication fix in this family.
2. **The log and the registry agree by construction only.** Nobody has to keep
   them in step: edit one arm of `decodeBoot42()` without touching
   `nvChipVerdict()` and both stay green. handoff33 §7 item 2 named this for
   `decodeBoot0()` and it is now true twice over. No gate covers the agreement.
3. **`decodeBoot42()` itself was not changed.** It still logs and returns `void`.
   The verdict is computed independently from the same word in
   `publishIdentity()`, which is what makes item 2 a real hole rather than a
   theoretical one.
4. **`NVChipSiblingNote` is published on a path no machine in this project has
   ever taken.** It requires a real GA106. It is reachable only by parse and by
   the host check that drives `nvChipVerdict()` with a composed GA106 word.
5. **Muxless still holds.** `PLAN.md` termination condition A is satisfied from
   two independent directions (the operator's report, handoff27 §1; the VBIOS DCB
   declaring zero display paths, handoff37 §0). None of this session's work moves
   a picture onto the internal panel, and none of it was meant to.

---

## 9. Open items, highest value first

1. **The log/registry agreement has no gate** (§8 item 2), and it is now the
   cheapest real hole on this track. Both `decodeBoot0()` and `decodeBoot42()`
   compute a diagnosis in the shell's log and a verdict in the header, and
   nothing pins that the two describe the same partition of the input space. A
   shape gate in the idiom of `tests/test_probe_identity_publication.py` could
   derive the arm count from the header's `_VERDICT_LIMIT` and require the log
   function to carry that many `NVLOG` arms — which is weaker than proving they
   agree, and much stronger than nothing.
2. **`loadGspFirmware()` still returns `bool`** while the stage verdict and the
   eight-way ucode verdict beneath it die in `NVLOG`. handoff34 §6 item 3 named
   it, handoff37 §1 gave `nvRiscvErrorVerdict()` five codes and the shell still
   consumes only the bool, handoff39 §11 item 1 carried it again. Same class as
   this session's work, and the detection side is already built and tested — only
   publication is missing. **This is now the oldest unclosed item on the NVIDIA
   track.**
3. **`CLAUDE.md`'s read-order list stops at `3050-handoff37.md`.** handoffs 38,
   39 and now 40 are not in it. Deliberately not edited here: that file is shared
   with the concurrent Intel agent and every path-shaped token in it is resolved
   by `tools/check-documented-paths.py`, so a careless edit reddens two gates.
   Worth one careful pass by whoever next has the file uncontended.
4. **The Intel peer's `test_intel_scanout` red is still absent from `MEASURED`**
   provenance in the sense handoff37 §4a describes — untouched again, for the
   third session. Adding an entry would vouch for a C++ suite this session never
   built. It is currently listed at 61 and the C++ gate is green, so this is a
   provenance gap rather than a failure.
5. **One `kextload` on a machine containing the card** remains the single
   highest-value action in the whole project, and remains blocked on hardware,
   SIP, and the mutually-exclusive Linux/macOS choice on the target laptop
   (handoff27 §1).
