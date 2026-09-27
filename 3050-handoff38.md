# 3050-handoff38 -- the connector row loop had never executed

Direct continuation of `3050-handoff37.md`. Two things happened, and the first
one is a correction rather than a finding.

---

## 0. What moved

No shipping code changed. `phase0-kext/` is BYTE-IDENTICAL as far as this
session is concerned -- every recent mtime in that directory belongs to the
Intel peer -- so `build-payload/` was NOT rebuilt and the payload gate's silence
is the proof rather than an assertion.

What moved is `tests/test_parse_dcb.py`: 49 tests -> **59**, closing handoff37
section 6 item 4.

---

## 1. Section 6 item 1 was STALE, and it was stale when it was written

handoff37's open list opens with *"Publish the RISC-V error verdict --
`nvRiscvErrorVerdict()` five codes and the shell still consumes only the bool"*,
calling it the cheapest remaining NVIDIA work and the same class for the fifth
time.

**It was already closed.** One grep answers it: `phase0-kext/NV3050Probe.cpp`
line 948 does `setProperty("GSPErrorVerdict", nvRiscvErrorVerdictName(...))`,
`3050-handoff36.md` line 141 says `publishGspErrorLatches()` publishes eight keys
with `GSPErrorVerdict` first, and `tests/test_probe_gsp_error_latch_publication.py`
runs **14 tests, green**. handoff36 landed at 11:05; handoff37 at 14:43 named it
as open three and a half hours later.

This is `3050-handoff29.md` section 1 and `3050-handoff32.md` section 1 for the
third time, and the shape is identical each time: **an item inherited across
handoffs is a claim with an age**, and the claims of the form "nothing consumes
X" or "nothing has ever read X" are set-differences somebody has to actually
run. The failure mode is writing the next session's open list from the shape of
this session's defect rather than from a grep -- the class was real four times
running, which is exactly what makes the fifth assertion feel safe to write
without measuring.

`3050-handoff37.md` was NOT edited to remove the item. Same rule as
`3050-handoff28.md` section 7 item 5 and handoff32 section 1: the numbered files
are a chronological record, not a status page, and a record edited to be correct
in hindsight stops recording anything.

---

## 2. The real subject: a loop with no per-row evidence

handoff37 section 6 item 4 said `parse_connectors()` has never run over a
populated connector table. That one was TRUE, and it is worth being precise
about why, because the tree contained two independent reasons and either alone
would have been enough.

1. **The real table has zero rows.** `dcb-report.txt` line 27 reads
   `connector table @0x5a24 (0 entries, 4 bytes each)` -- which is the finding,
   not a gap. The GA107 board declares no display paths and no connectors.
2. **Every synthetic fixture also had zero rows.** `build_image()` carried a
   `conn_count` knob and no test in the file ever set it above its default of 0.

So the `for index in range(count)` body in `tools/parse-dcb.py` had never
executed, in either direction, while 49 checks printed green over the function
that contains it.

### 2a. Why a count-shaped assertion could not have caught it

The three defects that survive an empty table are not exotic:

- a loop that decodes row 0 and repeats its result for every index
- a stride that advances by the header size, or by one byte, instead of
  `entry_size`
- a name lookup that silently DROPS a type it cannot name

All three produce the correct row COUNT. The third is the one that matters most
here and it is worth stating on its own: dropping an unnamed connector
**undercounts this board's connectors**, and on a table whose entire job is
corroborating an absence, undercounting is the one direction that reads as
confirmation. A decoder that quietly discards rows it does not understand would
make a populated board look like this one.

### 2b. The load-bearing fixture choice

`build_subtable()` fills every row with the SAME byte (`fill=0x11`). That is
correct for the sibling tables, whose counts are all this tree reads, and it is
precisely the fill under which a stuck-on-row-zero loop is indistinguishable
from a correct one -- every row decodes to the same value either way.

So the fixture gained `conn_types`, which supersedes `conn_count` and writes one
row per element with that type in the low byte. Left `None` the whole block is
byte-identical to what it was, so every pre-existing fixture in the file is
unchanged and no pre-existing test moved.

Two smaller choices in the same edit:

- `CONN_HEADER_SIZE` and `CONN_ENTRY_SIZE` are named constants rather than a `5`
  and a `4` written inline in the seek arithmetic. A hand-written literal there
  is a second copy of a layout `build_subtable()` already owns, and the two would
  disagree silently the first time either moved.
- `known_types()` takes its values from `dcb.CONNECTOR_TYPE_NAMES` rather than
  spelling them. A hand-written `0x01` that the tool later stops naming would
  turn a test about correct names into an assertion about the string `unknown` --
  the opposite of what it claims to check, and green throughout.

### 2c. The refusal tests come in a PAIR

`test_a_connector_table_running_past_the_end_is_refused` relocates the table to
eight bytes from the end and expects `None`. On its own that check is satisfied
by a `parse_connectors()` that returns `None` for every relocated table, i.e. by
a function that reads nothing at all.

So `test_the_same_table_one_row_shorter_at_the_same_offset_is_read` is its
partner: the identical construction 32 bytes from the end must be READ, four
rows. Containment is a one-sided property and a refusal test alone measures only
the refusing.

---

## 3. Non-vacuity: two mutations, run separately

Green on correct code establishes nothing in this suite. Both mutations were
planted in `tools/parse-dcb.py`, measured, and reverted; the file is
byte-identical now and the committed report proves it (below).

**Mutation A -- stuck on row zero.** `offset = ptr + hsize + index * esize`
becomes `offset = ptr + hsize`. Reddened exactly **4 checks, all four new, every
pre-existing check green**:

```
test_each_row_carries_its_own_type_rather_than_row_zeros
test_row_offsets_advance_by_the_entry_size_from_the_header
test_an_unnamed_type_is_reported_unknown_rather_than_dropped
test_the_report_renders_every_populated_connector_row
```

Four is the honest number here rather than a weak one: the mutation is a single
edit that breaks the identity, the stride, the unknown-row case and the rendered
output simultaneously, and each of those is a separate claim about the same
walk. What makes it evidence is the second half -- **no pre-existing check
moved**, which is `handoff18.md` section 1e's signal. A mutation that also
reddens old checks would have proved the old tests already sufficed.

**Mutation B -- drop the zero-entry-size guard.** `if esize == 0 or ptr + ...`
becomes `if ptr + ...`. Reddened exactly **1 check**, and it is the one that
names the removed guard:

```
test_a_zero_entry_size_is_refused_rather_than_looping
```

B exists because A does not isolate it. A zero `entry_size` is the
stuck-on-row-zero shape arriving from the DATA side rather than from the code --
every row lands on the same offset without a line of the loop being wrong -- and
the guard against it is a distinct claim that deserved its own control.

### 3a. The committed report is the revert proof

`dcb-report.txt` regenerates **byte-identically** (`diff` clean) after both
reverts. That is a stronger statement than "I put the line back", because it
exercises the mutated function over the real 65024-byte image and compares 40-odd
lines of its output against a file committed before this session started.

---

## 4. Gate readings

All four green. Every number below was read from a run in this session, not
carried forward.

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped, exit 0 |
| `tests/build-msvc.bat` | 10/10 suites passed, exit 0 |
| `tests/check-kext-syntax.bat` | `ALL SYNTAX GATES PASSED`, exit 0 |
| `tests/run-python-tests.bat` | **1127** tests, OK, exit 0 |
| `tools/check-documented-paths.py` | **134** paths accounted for, exit 0 |

The register gate is correctly unmoved at 240: this session added no `#define`
anywhere, only Python tests. `test_parse_dcb` alone reads **59**, up from 49.

The Python total of 1127 is a JOINT measurement -- an Intel-track agent is
working this tree concurrently and its files carry mtimes from 13:23 to 14:05
today. +10 of the delta is mine, attributed by running the module alone before
the gate. The rest is theirs and was not investigated, per the standing rule.

`build-payload/` was NOT rebuilt. The rule that decides a rebuild is CONTENT,
this session changed no file under `phase0-kext/`, and the payload comparison
inside the Python gate is green -- which also means the peer's own
`phase0-kext/` edits are already reflected in the current image and are not mine
to vouch for.

---

## 5. What this does NOT establish

1. **The row loop has still never decoded a REAL populated table.** Every one of
   the ten new checks drives a synthetic image. `3050-handoff27.md` section 2b
   item 5 is the standing warning here and it applies verbatim: a suite that only
   ever constructs the shape it expects cannot discover that a second shape
   exists. There is exactly one VBIOS in this tree and its connector table is
   empty.
2. **`conn_types` writes only the low byte of each row.** The upper three bytes
   stay at `build_subtable()`'s `0x11` fill, which is what
   `test_the_row_raw_word_keeps_the_bytes_above_the_type` leans on. Nothing here
   pins the MEANING of those upper bytes, because nothing in this tree decodes
   them.
3. **The absence finding is unchanged.** This session added no evidence about the
   board. `entry_count` is still 0, still `AUTHORED` on seven arms, and the
   provenance is still what the previous session measured out of the PCIR
   structure.

---

## 6. Open list

Ordered by value, hardware-blocked items last. **Each of these is a claim with a
date on it -- see section 1 before working one.**

1. **Reference `tools/parse-dcb.py` and `tools/find-display-matches.py` from
   `CLAUDE.md`.** Carried from handoff37 section 6 item 3, still true, and
   deliberately not done this session: `CLAUDE.md` is the one file both
   concurrent tracks edit, and a collision there is more expensive than the item
   is worth. Do it in a session where the Intel agent is idle.
2. **The Intel framebuffer is now the critical path to a picture.** The operator
   has directed the Intel agent to build an `IOFramebuffer` subclass the 3050
   side can eventually render into. Nothing in `phase0-kext/` subclasses
   `IOFramebuffer` today -- every occurrence is a comment -- and the kext's
   `Info.plist` declares one match dictionary, `IOPCIMatch = 0x25a210de`, which
   is NVIDIA-only. Ownership of that subclass belongs to the Intel track; do not
   start a second one here.
3. **`nvGspBootCheckRiscvErrors()`'s own distinctions**, carried from
   `3050-handoff35.md` line 253. Verify it is still open before starting.
4. **One `kextload` on a machine containing the card.** Unchanged, still the
   highest-value action in the project, still blocked on hardware.
