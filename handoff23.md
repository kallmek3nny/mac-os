# handoff23 — the register accessors in NV3050Probe.cpp had no observer, and the first gate I wrote for them could not fail

Written 2026-08-18. Continues `handoff21.md` on the NVIDIA track and closes the
register-access half of its §6 item 2. No register constant moved. No decoder
changed. No state machine changed. **`phase0-kext/` is byte-identical to how
this session found it** — the only file this session created anywhere in the
tree is `tests/test_probe_register_access_refusals.py`, and the only files it
edited are that one and this handoff.

**The Intel boundary held for the whole session.** A separate agent owns that
track. No Intel header, shell, or suite was read or written at any point. Where
an Intel file appears below it is as an *mtime in a gate attribution*, never as
content.

---

## 1. The finding: three refusals, zero observers

`NV3050Probe.cpp` is the NVIDIA IOKit shell, and by construction **no C++ suite
can execute a line of it** — it pulls in `IOService`, which does not exist off
macOS. `handoff18.md` §1d established the only technique that reaches it: pin
the code SHAPE from Python, parse the source, assert the properties, and prove
the assertions can fail by mutating the source in memory.

That technique had been applied to exactly one property in the file
(`tests/test_probe_dead_read_publication.py`, the dead-read publication shape).
Everything else in the shell was reached only by parse — including the two
functions through which **every single MMIO access in the NVIDIA track passes**:

| Function | Lines | What stands in front of the dereference |
|---|---|---|
| `regRead32()` | `:326`–`:339` | 2 refusals (`:330`, `:336`), both returning `0xFFFFFFFF` |
| `regWrite32()` | `:341`–`:368` | 3 refusals (`:348`, `:354`, `:360`), all bare `return;` |

Nothing observed any of them. Delete the bounds guard from `regRead32()` and all
four gates stay green: the register gate checks constants and both constants
(`NV_BAR0_INDEX`, `NV_BAR0_EXPECTED_LENGTH`) are correct; the C++ suites cannot
link a translation unit that reaches IOKit; `-Werror` has nothing to say about a
deleted `if`; and nothing in the Python gate modelled the shape of the file.

That is the same blind spot handoff20 §1 and handoff22 §1 each found on the
Intel side, one level down. Both of those turned out to be **live defects**. This
one did not — the accessors are correct as written, and §1a is what got added
instead: a reason for them to STAY correct.

### 1a. What `tests/test_probe_register_access_refusals.py` pins

One `audit()` function. The live tree and every mutant enter through it and
nowhere else — the design rule carried from `test_probe_dead_read_publication.py`,
whose docstring states it plainly: *a gate whose mutants are checked by different
code than its real input is two gates, and only one of them has ever been seen to
fail.*

Scanning is done over **comment-stripped source**. That is not hygiene, it is
the lesson from handoff18 §1d, where the fix's own comment quotes the defective
call it replaced — so a structural gate that counted prose found the defect it
exists to forbid and reddened a pristine tree. `regWrite32()`'s comment at `:342`
literally says "Three refusals stand in front of the store", and a gate reading
that sentence would pass on a function whose three guards had all been deleted.
Every count below was measured from the guards themselves.

Four properties:

1. **Both accessors refuse, in order, before the single dereference.** Shared
   helper `_accessor_failures()`, because the mapped-guard / bounds-guard /
   one-store shape is identical in both. The bounds guard is not merely matched,
   it is **captured** — `BOUNDS_RE` grabs the whole condition so the
   `sizeof(UInt32)` term can be inspected inside it. `offset < getLength()` is
   inside the mapping by every naive test and still reads three bytes past the
   end of it; that is handoff22 §1b's rule, arrived at independently on the
   NVIDIA side, and it is the single most valuable assertion in the file.
   Ordering is pinned by index comparison, so a dereference hoisted above either
   guard reddens.
2. **The guarded accessors are the ONLY MMIO path in the shell.** Whole-file
   counts: exactly one `OSReadLittleInt32(`, exactly one `OSWriteLittleInt32(`.
   A second store anywhere in the file stands behind none of the three refusals,
   and no other gate in this tree could see one.
3. **A stock build physically cannot open the write gate.** `fAllowGspWrites` is
   assigned exactly twice in the whole file, both assignments sit inside
   `init()`'s `#ifdef PHASE4_ALLOW_WRITES` block, the `#ifdef` arm sets it true
   and the `#else` arm sets it false. The arm split matters on its own: flipping
   `#else` to `true` leaves the assignment COUNT at two, so only an arm-aware
   check sees it. `ASSIGN_RE` requires the identifier on the LEFT of a single
   `=`, which is why `io.allowWrites = fAllowGspWrites;` at `:953` is correctly
   read as a use rather than a fourth assignment.
4. **`mapBar0()` WARNS about a short BAR — it does not refuse.** The short-BAR
   branch is brace-matched and asserted to contain no `return`, and the function
   is asserted to end in `return true;`. This is deliberate, and handoff22 §1 is
   the Intel version of the identical call: declining to attach deletes the
   IORegistry node that makes a short BAR inspectable in the first place. Also
   pinned: exactly one `bar->map(kIOMapInhibitCache)`, because a cached MMIO
   mapping leaves writes in the cache line and returns stale reads, which reads
   as a GPU ignoring you rather than as a mapping bug.

And a refusal: if a function body cannot be found, `audit()` reports
`body not found` rather than passing over an empty string. `function_body()`
returns `None`, never `""`, for the same reason. That is the failure mode that
makes a shape gate vacuous, and it has its own negative control.

**11 tests: one positive control, ten negative controls.** Every negative control
asserts WHICH property broke, not merely that something did.

## 1b. The mistake: a comparison against a rendering measures the renderer

The part worth carrying forward is that **my first version went RED on correct
shipping code**, and the failing test was the positive control.

The sentinel `0xFFFFFFFF` was read out of `regRead32()`'s own body at `:330` and
`:336` — correctly, and specifically NOT guessed from a sibling, which is
handoff21 §1b's whole lesson. Then it was compared like this:

```python
READ_REFUSAL_VALUE = "0xFFFFFFFF"
...
if value.upper() != READ_REFUSAL_VALUE:
```

`"0xFFFFFFFF".upper()` is `"0XFFFFFFFF"`. The `x` in the prefix uppercases too,
so that comparison is false for **every possible input**, including the correct
one. The gate reported the shipping refusals as wrong sentinels.

This is the loud direction of a family this tree already knows in its quiet
direction. `a-sentinel-guessed-from-a-sibling-cannot-fail` is a comparison that
can never be TRUE, and it printed `ok` while measuring nothing. This one can
never be FALSE, and it reddened a correct tree. Same root: the comparison was
against a *rendering* of the value rather than the value. The fix is to hold the
sentinel as an `int` and parse every found literal with `int(value, 16)` before
comparing — then `0XFFFFFFFF`, `0xffffffff` and `0xFFFFFFFF` are all the same
number, which is the only thing the assertion ever meant.

Generalisation: **normalising a hex literal by case-folding the whole token
folds the prefix too.** Any gate that compares source literals should compare
parsed values, or the assertion is about spelling.

A second, smaller version of the same error hit three negative controls: they
asserted on substrings of failure messages I had written earlier and then
reworded during the edit, so the mutations fired correctly and the assertions
looked for text `audit()` no longer emitted. Caught immediately because a
negative control that goes green when it should be red is invisible, but one
that goes red is loud. Fixed by taking the substrings from `audit()`'s live
wording, and two of them were tightened while I was there: the width-term and
mapped-guard controls now require **2** failures rather than "any", because both
accessors carry those guards and an assertion satisfied by one of two passes on
a half-applied mutation.

## 2. Non-vacuity

Ten mutations, each planted in memory against the real source and run through the
same `audit()` as the live tree:

| Mutation | Caught by |
|---|---|
| Drop `sizeof(UInt32)` from the bounds guard (both accessors) | width-term check, **2** failures required |
| `if (!fBar0Base \|\| !fBar0Map)` → `if (false)` (both accessors) | mapped-guard check, **2** failures required |
| Add a second `OSWriteLittleInt32` after the guarded one | whole-file MMIO-path check |
| Delete the `if (!fAllowGspWrites)` build gate | build-gate check |
| `#else` arm sets `fAllowGspWrites = true` | `#else` arm check (count stays 2) |
| Assign `fAllowGspWrites` inside `mapBar0()` | outside-`init()` check |
| Short-BAR branch gains `return false;` | short-BAR-warns check |
| `bar->map(kIOMapInhibitCache)` → `bar->map(0)` | cache-flag check |
| Hoist the read above both guards, and return the hoisted value | BOTH ordering checks |
| Rename `regWrite32` so the body cannot be found | refusal check |

The hoist mutation is the one worth reading. It takes **two** edits — insert the
early read, then replace the original `return OSReadLittleInt32(...)` with
`return early;` — because a one-edit version leaves two stores in the function
and reddens the COUNT check, which proves nothing about ordering. Same shape as
handoff18 §1e's 13 → 30 → 2: a mutation that reddens the wrong check is not
evidence, however loudly it fails.

`mutate()` asserts its anchor's occurrence COUNT before replacing, so a mutation
whose anchor has drifted raises rather than silently matching nothing. That is
`a-mutation-that-matches-nothing-prints-ok` wired in as a precondition.

## 3. Gates: how each was read, not just its verdict

Every gate's exit status was carried out through a file
(`> /tmp/x.txt 2>&1; echo $? > /tmp/x.status`) rather than read after a pipe.
handoff21 §3 records me walking into `$?`-after-`tail` in a session whose whole
subject was assertions that cannot fail; not repeating it.

| Gate | Verdict | Exit |
|---|---|---|
| `python tools/check-registers.py` | 240/240 passed, 0 failed, 0 skipped | 0 |
| `tests/build-msvc.bat` | 9 of 9 suites passed under ctest | 0 |
| `tests/check-kext-syntax.bat` | `ALL SYNTAX GATES PASSED` | 0 |
| `tests/run-python-tests.bat` | Ran 896 tests, **1 failure, peer-attributed** | 1 |

Four things about that table are worth more than the table.

**The register gate prints 240, not the 239 in handoff21.** `IntelDisplay.hpp`
now reports 96 defines where handoff21 saw 91. That is the peer's Intel work.
Attribute a moving gate number before treating it as yours — this is the second
consecutive handoff on which that number moved underneath a session that did not
touch it.

**The syntax gate prints `FAIL` on line 189 and passes.** That line is
`=== 11. negative control -- each defect must FAIL ===`, a section header for the
gate's own negative-control block. Grepping for `FAIL` and reading the first hit
would condemn a green gate. Read the exit code; grep for FAIL; then read what the
FAIL line actually says.

**The Python gate went red twice with a DIFFERENT failure each time, and neither
was mine.**

| Reading | Failure | Attribution |
|---|---|---|
| 1 | `test_every_shipped_file_is_byte_identical_to_the_tree` — three stale files in `build-payload/kext-src.tar` | All three Intel. `NV3050Probe.cpp` mtime 2026-08-17 17:02, untouched. |
| 2 | `test_every_md5_the_card_quotes_can_be_accounted_for` — the live ISO's md5 is not among those the operator card prints | Payload rebuilt 2026-08-18 10:24; `OPERATOR-ACTION-step7-build.md` last written 2026-08-17 20:47. The peer rebuilt the payload and has not yet landed its card entry. |

Between the two readings the peer rebuilt `build-payload/` — the tar's mtime
(10:24) is NEWER than all three files it was reported stale against (10:05,
10:08, 10:11), which is the signature of a rebuild landing mid-run. That is
`a-mid-write-snapshot-reads-as-a-false-green` in its inverted form: for gates it
reads as a false RED.

I deliberately did **not** rebuild `build-payload/` and did not add an md5 to the
operator card. Both would green a gate by vouching, under this session's name,
for a peer's half-landed Intel work I am forbidden to read. handoff18 §3 made the
same call for the same reason. **A gate's verdict in a two-agent tree is a joint
measurement**: red does not mean you broke it, green does not mean you fixed it.

**896 is 885 + 11**, and 885 is handoff21's reading. The new module contributed
every added test and no pre-existing test changed status between the readings
except the two peer-attributed ones. The module joins the gate by discovery —
`run-python-tests.bat` needs no registration edit, confirmed by grepping the tree
for `test_probe_dead_read_publication` and finding it only in docs and its own
docstring.

**No `MEASURED` entry was added to `tests/test_check_count_floor.py`.** That gate
globs `test_*.cpp` and bands C++ check counts; a Python module is outside its
subject entirely. Adding an entry would have been a number with nothing behind
it, which is the exact defect handoff17 §1 exists to prevent.

## 4. What this does NOT establish

- **Nothing about hardware.** Unchanged since `handoff13.md`: the kext compiled
  and linked under Apple clang 17.0.0 on 2026-08-14 and has never been
  `kextload`ed. There is no NVIDIA card in the guest to match against.
- **Nothing executes.** This is a SHAPE gate. It proves the guards are written,
  written in the right order, and are the only path to the hardware. It does not
  prove they behave correctly at runtime, because nothing in this tree can run
  them. A guard with correct structure and inverted logic passes every check in
  this file.
- **The rest of the shell is still unobserved.** `NV3050Probe.cpp` now has two
  properties pinned out of a file with 37 property publications, a BAR mapping,
  Phase 5 staging, and the GSP boot driver. §5 item 2 is the remaining list.
- **No new evidence of any kind.** No register was read, no trace parsed, no
  silicon touched.

## 5. Open items, NVIDIA side

1. **One `kextload` on a machine that contains the card.** Unchanged, and still
   the single highest-information action available to this project. macOS on
   hardware with the 3050 in it, SIP lowered, a reboot. Every phase from 2 onward
   is gated on it; the code for phases 2 through 3.75 is written and tested.
2. **`NV3050Probe.cpp` now has two properties pinned, not one.** Still unpinned:
   the 37 published property keys as a set (only the dead-read subset is covered),
   `readIdentity()`'s decode-and-publish path, `checkWpr2()`, and
   `publishGspBootResult()`. The technique is proven twice over now and costs one
   Python module each.
3. **`build-payload/` is stale with respect to the peer's Intel edits, and the
   operator card has no md5 for the ISO built 2026-08-18 10:24.** Left for the
   Intel agent on purpose. If the Python gate is red on resume and the failing
   test names an Intel file or an ISO md5, check mtimes before assuming it is
   yours.
4. **No test observes a GSP boot from inside the DMA states with a real multi-MiB
   image.** Carried forward unchanged from handoff21 §6 item 4. Low value.

Nothing on this list is blocked on the Intel track, and nothing on it requires
editing an Intel file.
