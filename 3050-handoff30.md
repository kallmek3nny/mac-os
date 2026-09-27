# 3050-handoff30 — the guard that nothing was watching

NVIDIA track, 2026-08-18, continuing from `3050-handoff29.md`. One item, and it
is that file's own §9 item 3: the hole the previous session's fix left behind.

`phase0-kext/` is **BYTE-IDENTICAL**. No shipping code changed, so
`build-payload/` was NOT rebuilt and the thirtieth image is still current. What
moved is the evidence layer: one new Python module,
`tests/test_probe_gsp_boot_publication.py`.

---

## §1 — what was actually missing

`3050-handoff29.md` fixed `publishGspBootResult()` by adding `readMask` to
`NVGspBoot` and guarding each of the five diagnostic words behind
`nvGspBootWasRead()`. `tests/test_gsp_boot.cpp` grew 313 → 341 checks proving the
mask is **correct** — that it tracks the reads, that the bits are disjoint, that
a failed read never sets one.

None of that proves the publisher **consumes** it.

`publishGspBootResult()` lives in `phase0-kext/NV3050Probe.cpp`, inside the IOKit
shell. No host suite compiles that translation unit — it pulls in IOKit's own
`IOService` declarations, which do not exist off macOS. So the five guarded
`setProperty()` calls are reachable **only by clang's front end**, and clang has
nothing to say about a deleted `if`. Delete all five guards and:

- the register gate still reads 240/240 (no `#define` moved),
- the C++ suite still reads 341/341 (the mask is still correct; nobody asks who
  reads it),
- the syntax gate still exits 0 (`setProperty()` with an unguarded `UInt64` is
  well-formed C++),
- the Python gate was silent on this file entirely.

Four green gates over a restored defect. That is the same shape as
`handoff24.md` §5 item 4 one track over, and the same shape as this project's
recurring lesson: when no gate can see it, pin the shape.

---

## §2 — the module, and the one choice worth arguing about

`tests/test_probe_gsp_boot_publication.py`, **16 tests**, built on the idiom of
`tests/test_probe_identity_publication.py` and
`tests/test_probe_wpr2_publication.py`: one `audit()` shared with the live tree,
COMMENT-STRIPPED source, one positive control and a negative control per
property each asserting WHICH one broke.

Five properties:

| # | Property | Why it is not redundant |
|---|---|---|
| 1 | a null `machine` is refused before any dereference, and before any `setProperty()` | the mask publication dereferences `machine` unconditionally |
| 2 | `NVGspBootReadMask` is published exactly once, unconditionally, and FIRST | its absence must not read as an older kext, and it must sort above the words it qualifies |
| 3 | no diagnostic word is published outside a `nvGspBootWasRead()` guard | this is the defect itself |
| 4 | in every guard the BIT, the KEY and the FIELD agree, and no bit guards two words | the copy-paste shape |
| 5 | the set of bits guarded here EQUALS the `NVGSP_READ_*` enum in `phase0-kext/NVGspBoot.hpp` | the tripwire |

**Property 5 is the choice worth arguing about, and it is the one that makes the
module outlive this session.** The bit set is DERIVED from the header's own enum
rather than spelled in the test. A sixth bit added to `NVGspBoot.hpp` with no
corresponding publication reddens here, on the commit that adds it, with a
message naming the bit. That is `intel-handoff26.md` §2a's reasoning — derive the
subject set, do not spell it — arrived at for a different family. The triple
table still fixes the PAIRING of bit to field to key, because equality of sets
says nothing about which word each bit guards.

### §2a — comment stripping is mandatory here, not stylistic

The fix's own comment block in `NV3050Probe.cpp` explains the defect and in doing
so names every key and every field it publishes, and quotes the phrase
"byte-identical". A gate reading prose would find `NVGspBootPrivErrStat` in a
comment and count it as published on a publisher whose five guards had all been
deleted. `test_comment_stripping_removes_the_fixs_own_prose` asserts the stripper
actually removes it rather than trusting that it does — the same reason
`tests/test_probe_dead_read_publication.py` scans code and not text, and that
module went red on a pristine tree learning it.

### §2b — two tests exist so the gate cannot pass by measuring nothing

`audit()` returns a REFUSAL, not silence, when it cannot find its subject:
renaming `publishGspBootResult` produces `subject not found`, and a header with no
`NVGSP_READ_*` enumerators produces `the gate could not derive the bit set`. Both
are pinned by their own test. A gate that reports nothing when its anchor moves is
indistinguishable from a gate that is green, which is the vacuity direction and the
worst failure available to a module whose entire job is watching a file no other
gate reads.

### §2c — the guard regex is structural, not a negated class

The pattern matches `if ( nvGspBootWasRead ( <obj> , <BIT> ) ) setProperty ( "<KEY>"
, ( UInt64 ) <obj> -> <field> ,` with explicit tokens and captures at every point
it later compares. It does NOT use a character class excluding the statement
terminator: `handoff25.md` §1c records what that costs — a class that excludes only
`;` runs past the `if`'s closing paren, the newline and the whole guarded body to
land on the next statement's semicolon, and matches CORRECT code as a violation.
The mask's unconditional-ness is likewise decided by walking back to the previous
statement boundary and refusing any branch keyword in the gap, rather than by a
regex over the line.

---

## §3 — non-vacuity, and an honest note about what that means here

`handoff18.md` §1e's signal — a mutation reddening only NEW checks, every
pre-existing check green — **does not apply to an all-new module**, because there
are no pre-existing checks in it. `intel-handoff26.md` §4 says the same thing about
its own gate and is right.

What the twelve negative controls establish instead is per-property discrimination:
each mutates the live source IN MEMORY (the tree is never written) and asserts the
specific failure string, so a control that passes proves the audit reached the
property it names rather than tripping on something else. Every mutation anchor goes
through `mutate()`, which asserts the OCCURRENCE COUNT before replacing —
`a-mutation-that-matches-nothing-prints-ok` in both directions, since an anchor
matching zero times is a no-op control and one matching twice edits a site the test
did not mean.

The sharpest of the twelve is `test_a_duplicated_bit_is_caught`, which spells
`NVGSP_READ_CPUCTL` in HWCFG2's guard — the exact copy-paste shape that runs C and D
of `test_gsp_boot.cpp` exist to catch one level down. It reddens **three** distinct
properties: the wrong-bit pairing, the duplicate-flag check, and property 5's
set-equality (HWCFG2's bit is now declared and guards nothing). Three independent
roads to the same defect is what a set-derived predicate buys.

---

## §4 — gate state, all four run after the last edit

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped, exit 0 |
| `tests/build-msvc.bat` | **9/9** suites passed, exit 0 |
| `tests/check-kext-syntax.bat` | exit **0** |
| `tests/run-python-tests.bat` | **970** tests, OK, exit 0 |

Python 954 → **970**, and the +16 is entirely this module — measured by running it
alone before running the gate, which is the only way to attribute a count in a
two-agent tree. Register gate unmoved at 240, correctly: no constant was added.
C++ unmoved at 9 suites and `test_gsp_boot` unmoved at 341, correctly: no C++ file
was touched, and `tests/test_check_count_floor.py` therefore needed no edit.

`phase0-kext/` is byte-identical, proven by the payload gate's SILENCE rather than
asserted — `test_operator_step7_doc` compares `build-payload/kext-src.tar` against
the live tree byte-for-byte and is green, so no rebuild was warranted and none was
done. `3050-handoff28.md` §6's rule: the thing that decides a rebuild is CONTENT.

The syntax gate still prints `FAIL` on a passing run — line 224 is the *title*
`=== 11. negative control -- each defect must FAIL ===`. Read the exit code.

---

## §5 — what this does NOT establish

1. **Still nothing ran on hardware.** This gate proves by PARSE that the publisher
   consumes the mask. It does not execute one line of `publishGspBootResult()`, and
   no suite can.
2. The mask records that a read COMPLETED, not that the value is trustworthy. A
   successful read of a dead aperture returns all-ones and still sets its bit.
   Unchanged from `3050-handoff29.md` §3.
3. The gate is spelled against `nvGspBootWasRead(` by name. A second guard helper
   added later produces publications this module does not consider guarded — and
   property 3 would report them as bare, which is the LOUD direction and the correct
   one, unlike `intel-handoff26.md` §6 item 4's accessor gap which reports nothing.
4. `NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED` is still in the enum and the state machine
   has still never had real firmware bytes pass through it.
5. `nvRiscvUcodeIsWellFormed()` is still a bare `bool` with six ways to say no, and
   still nothing in `NV3050Probe.cpp` calls it.

---

## §6 — open, most valuable first

1. **The operator's scheduling decision.** Unchanged and still theirs: the laptop
   holds Linux or macOS, not both, so more i915 traces and one `kextload` are
   mutually exclusive. Only the Intel track can produce a picture on the internal
   panel, so the traces are almost certainly worth more.
2. **One `kextload` on a machine containing the card.** Unchanged from handoff22
   §4 through here. Still the only thing that turns nine handoffs of source claims
   into measurements.
3. `readIdentity()`'s decode-and-publish path — the last of the four items
   `handoff23.md` §5 item 2 named, and now the highest-value item not blocked on
   hardware.
4. `nvRiscvUcodeIsWellFormed()` returns a bare `bool` with six distinct refusal
   reasons behind it, and nothing in the IOKit shell calls it at all. Both halves
   are worth fixing and the second is the one that matters.
5. Nothing tests that `regRead32()`'s refusal value is *the same constant* the
   consumers compare against. `NVWPR2_READ_REFUSED_VALUE` in `phase0-kext/NVFalcon.hpp`
   and `IG_MMIO_INVALID` in `phase0-kext/IntelDisplay.hpp` are both `0xFFFFFFFF` by
   agreement rather than by construction. Unchanged from `3050-handoff26.md` §7 item 4.
