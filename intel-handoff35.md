# intel-handoff35

Intel Gen12 (Tiger Lake) track. Continues from `intel-handoff34.md`.

This session added `phase0-kext/IntelGtImage.hpp` and its suite, registered both
in every gate that has to know about them, and rebuilt the payload. All five
gates end green and the readings are in section 4.

The session's real result is not the header. It is the CRASH the suite produced
on its first run, and the two defects that crash measured -- both of them mine,
both in the header, and both of the kind that compose a WELL-FORMED page. See
section 2, which is the one to read.

---

## 0. What this is, and the sentence it is easy to misread

The operator asked for an **accelerated** driver, retargeted it to the Intel
iGPU, and said to write it here rather than delegate it. This session is the
fifth on the GT stack and it is still not acceleration.

What exists after it is a five-header pile that can DESCRIBE a render-engine
submission on paper:

- `IntelGtCmd.hpp` -- engine MMIO bases, the ring register block, ring geometry
  verdicts, head/tail wrap arithmetic, MI command words.
- `IntelGtExec.hpp` -- the execlist port, the 64-bit context descriptor, GTT and
  page-table entry composition.
- `IntelGtContext.hpp` -- WHERE each dword of the logical ring context's
  register-state page goes. Pure arithmetic over slot indices.
- `IntelGtImage.hpp` -- **new this session** -- actually PUTS them there, into a
  span the caller owns.

Nothing consumes any of it. There is no IOKit shell above the GT stack at all,
no mapping, no submission, no fence, no interrupt path. A green run of all
sixteen C++ suites establishes that this pile is self-consistent with itself and
**nothing whatsoever about whether Gen12 silicon agrees**, because the evidence
for the whole stack is one measurement, repeated in section 1.

Do not read "sixteen suites pass" as progress toward a picture on a screen. It
is progress toward a driver that is internally coherent about a submission it
has never performed.

---

## 1. The evidence story, restated because it gets weaker as the stack grows

`reference/i915/` holds seven files and all seven are display. Greps across that
whole directory for `LRC`, `CTX_DESC`, `RING_BUFFER_START`, `RING_BUFFER_CTL`,
`BB_PER_CTX` and `CTX_TIMESTAMP` return **ZERO matches**, measured 2026-08-26.
i915 keeps the logical ring context layout in `gt/` headers that were never
vendored here.

So `IntelGtImage.hpp` has no cited half at all. The one exception in the stack
beneath it is the ring block -- `RENDER_RING_BASE` at
`reference/i915/i915_reg.h:289`, and `PRB0_BASE` spelled `(0x2030 - 0x30)`,
which is what fixes the ring block ABOVE the engine base rather than at it --
and the four ring register offsets this writer stores come from there. Every
SLOT they land in is policy.

No `*_OBSERVED_*` constant may enter this header. `igpu-results/` captured a
machine whose render engine was IDLE, so there is no MMIO capture of a running
render engine anywhere in this tree, and inventing one would file a guess beside
values witnessed on silicon.

The suite prints that caveat ABOVE its own count, so a reader of
`tail -1` gets the number and a reader of the output cannot miss the caveat.

---

## 2. The crash, and the two defects it measured

`test_intel_gt_image.exe` did not fail on its first run. It **crashed**:

```
Run-Time Check Failure #2 - Stack around the variable 'tight' was corrupted
```

That is the measurement. Two distinct defects, both in `IntelGtImage.hpp`, both
written by me in the same sitting.

### 2a. The image is TWELVE dwords and the guard said ten

`IG_GTI_RING_IMAGE_DWORDS` was `10`. `IntelGtContext.hpp` puts the CTL pair at
slot `0x0A`, which occupies slots **10 and 11**. Ten dwords reaches slot 9.

So the length guard **ADMITTED** a span two dwords too small and then stored
past the end of it. A refusal that should have fired, followed by a write into
memory the caller never offered -- both halves wrong, in the same call.

The reason it was loud rather than silent is the fixture, not the header. The
tight-span arm drives an `IGU32 tight[12]` -- a **stack** array, deliberately.
A heap span or a roomier buffer would have taken those two stores in silence and
the suite would have printed green over a writer that scribbles.

The ten-dword span is now pinned as its own refusal arm, so the regression
reddens here rather than in a caller's stack frame.

### 2b. The LRI header declared FOUR pairs while the composer skipped slots 2/3

This is the more dangerous of the two and it would never have faulted.

`IntelGtContext.hpp` puts HEAD at slot `0x04`, so the pair slot at `0x02` --
immediately below HEAD -- belongs to something else, and the composer was
leaving it alone while the header word declared four pairs.

An LRI header declaring N pairs while the composer leaves a gap **does not
describe a page with a hole in it**. The engine reads pairs CONTIGUOUSLY from
the slot after the header. It would take whatever happened to be sitting at 2/3
as the first register offset and walk the rest of the image one pair out of
step, so every dword after that names a register nobody chose.

That is handoff15's byte-swapped `PLANE_SIZE` hazard one subsystem over: it
programs cleanly and does the wrong thing.

The fix is pair count 4 -> 5, image dwords 10 -> 12, and the first pair supplied
by the CALLER through two new parameters. This tree cannot cite what that pair
holds -- `reference/i915/` has no LRC file at all -- so the offset is **not**
invented here and **not** given a name that would sit beside the four cited ring
offsets as though it had the same standing. A zero offset is REFUSED
(`IG_GTI_WRITE_FIRST_PAIR_MISSING`) rather than written, because zero is the
caller's initialiser and accepting it composes a page whose first restored
register is whatever lives at the engine base.

Both wrong constants are left **documented in comments** in the header rather
than quietly corrected. The defect and its detection are the same fact, and a
silent correction throws away the half that is worth keeping.

### 2c. Why a writer is its own layer

`IntelGtContext.hpp` is pure arithmetic -- drive it with anything, it answers
with a number. `IntelGtImage.hpp` is the first thing in the GT stack that
MUTATES CALLER MEMORY, and that is a different contract with a different failure
mode: **a refusal that has already written**.

A half-composed page is not a broken page. It is a well-formed image describing
a ring nobody configured -- the quiet direction, because it programs cleanly.

So the suite POISONS the span with `0xDEADBEEF`, a value the writer cannot
produce at any slot, and after every one of the refusal classes it asserts the
**WHOLE span** is still poisoned. A suite asserting only the return code cannot
distinguish "refused" from "composed half a page, then refused" at all.

Section 2 of the suite also asserts slots 2 and 3 on the SUCCESS path, and that
is what separates a writer placing its pairs at the documented slots from one
packing them contiguously from the first pair slot. Both fill twelve dwords.
Only the slot assertions tell them apart.

---

## 3. Two ordering claims, each pinned by an input bad in two ways

Refusal order in `igGtiWriteRingImage()` is load-bearing and each step is judged
before the one that would depend on it:

- **NULL before LENGTH.** A count is a promise about memory that does not exist:
  a span of a thousand dwords at address zero is still nothing, and reporting
  its LENGTH as the complaint sends somebody to look at a number that was never
  the problem. A length-first order would have to accept a legal count and fall
  through to dereference null.
- **LENGTH before the IMAGE verdict.** The image verdict judges the numbers
  rather than the destination, so an image that could not have been written into
  this span in any case is a true complaint about the wrong thing.

The out-param is written on **every** path, refusals included, and is ZERO on
every refusal -- which is true rather than merely conventional, since no refusal
writes a dword. An untouched out-param carries the caller's initialiser, and a
caller who pre-loaded it with the count they expected cannot tell a refusal from
a success. The suite pre-loads it with a plausible SUCCESS value rather than
zero, because pre-loading zero makes "the writer zeroed it" and "the writer
never touched it" indistinguishable.

One asymmetry worth knowing: a zero first-pair **offset** is refused while a
zero first-pair **value** is ACCEPTED. Registers are legitimately restored to
zero; a register at offset zero is the caller forgetting a parameter.

---

## 4. Gate readings

All five green, measured 2026-08-26 in this order:

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **242/242 passed**, 0 failed, 0 skipped -- unmoved |
| `tests\build-msvc.bat` | **16/16 suites passed** (was 15) |
| `tests\check-kext-syntax.bat` | all twelve sections ok, exit 0 |
| `tests\run-python-tests.bat` | **Ran 1279 tests ... OK** |
| payload freshness | green after the rebuild, see below |

`test_intel_gt_image` measures **111 checks, 0 failures**, confirmed twice --
once before the gate edits and once after the full rebuild, because the floor is
a relation between the TABLE and the BINARY and an entry typed to match a stale
reading satisfies that relation identically to a real measurement.

The register gate is unmoved at 242 **correctly**: `IG_GTI_WRITE_` constants are
this driver's own bookkeeping rather than hardware, and
`tools/check-registers.py` parses only two Intel headers. So `IG_GTI_WRITE_` is
the third Intel verdict family with no set-check from that gate, joining
`IG_PRESENT_` and `IG_GT_SUBMIT_`. The suite's own contiguity, distinctness and
limit-equality loops are the only thing holding the relation, and the
verdict-name loop bounds on `IG_GTI_WRITE_LIMIT` rather than a typed count, so a
family that grows reddens on the commit that grows it.

The Python total stays at **1279** because every Python edit this session added
a list **entry** rather than a test method -- the same reason handoff34 gives.

### 4a. The Python gate was RED once, correctly, and it was mine

One failure: `test_every_source_file_is_in_the_payload`. `phase0-kext/` changed,
and `build-payload/kext-src.tar` is compared byte-for-byte.

No peer attribution available and none needed -- the operator has said there are
no peers, so `intel-handoff27.md` section 5a's rule has no premise left. The
staleness was this session's to own. `build-payload/` was rebuilt (the **forty
sixth** image) and `OPERATOR-ACTION-step7-build.md` carries the measurements.

Both md5s AND both byte sizes moved: tar 593920 -> **604160**, ISO 671744 ->
**681984**, exactly one 10240-byte tar block each; tar `86fec920...` ->
`7b8cef8a...`, ISO `01ae1207...` -> `887da01c...`.

Both properties were read off the disk in the SAME measurement rather than one
being carried forward. That is the forty fifth block's own correction applied:
a byte size is not a fingerprint, and measuring one property of a file is not
measuring another.

### 4b. Registering one header and one suite touched five places, again

handoff34 section 4a listed these and the list held exactly. Recording only what
was different this time:

1. `tests/CMakeLists.txt` -- `add_kext_test`.
2. `tests/check-kext-syntax.bat` section 1 -- auto-discovered by
   `tests/test_check_header_coverage.py`'s glob over `phase0-kext/*.hpp`, so
   absence here is a FAILURE rather than an omission. The header has one include
   and it is a QUOTED sibling, so it is expected here rather than exempted --
   `check-header-coverage.py` derives the exemption from the presence of an
   ANGLE-BRACKET include.
3. `tests/test_check_count_floor.py` -- `MEASURED` entry with a dated provenance
   block. First reading of a new suite, so no upper edge could fire and none is
   claimed.
4. `tests/test_check_count_last_line.py` -- **two** independent hardcoded lists,
   `SUITES` and the `%d checks, %d failures` format group. Its
   `test_no_unlisted_suite_in_tests_dir` asserts set-equality against
   `os.listdir`, so the gate is RED from the moment the file lands until both
   entries exist.
5. The count format itself, written correctly this time from handoff34's note.

### 4c. Two recorded traps fired again and cost a round trip each

Both are already in the memory index; both still caught me.

- Bare `ninja test_intel_gt_image` failed on `cstdio` -- a shell with no MSVC
  `INCLUDE`. The danger is that a no-op build leaves the OLD binary in place, so
  a planted defect reddens nothing and reads as a vacuous test. **Run the gate,
  not the generator.**
- An `Edit` against `check-kext-syntax.bat` failed with the anchor spelled using
  `\r\n`. The Read view is LF-normalised, so the anchor must be written with bare
  `\n` even in a CRLF file. Verified afterwards that the file is still **293
  CRLF, 0 bare LF** -- `tests/test_line_endings.py` is a gate, not a style check.

A third, from earlier in the session and worth keeping: I reached for a constant
named `IG_GT_MI_NOOP_POLICY_0x00` from memory. It does not exist. The real one
is `IG_GT_MI_OPCODE_NOOP_POLICY_0x00`, and there is a purpose-built
`igGtMiNoop()` wrapping it, which is what the writer now calls -- better than
the literal I intended, because the sibling owns "what a NOOP word is" and a
second spelling of it agrees with itself while a changed opcode moves only one
of the two. Grep the tree; do not recall the name.

---

## 5. What this does not establish

1. Not one line of `phase0-kext/IntelGtImage.hpp` has run on an Intel GPU, or on
   any GPU. The suite drives a pure header under MSVC.
2. The page LAYOUT is this project's choice end to end. Nothing here is evidence
   that Gen12 silicon wants these dwords in these slots, and section 1 is why
   there is nothing in this tree that could be.
3. The first pair's register offset is a hole in the design, not a solved
   problem. The writer refuses a zero and otherwise writes whatever it is given.
   A caller that supplies a wrong offset composes a page this suite calls
   correct.
4. Nothing writes a page. There is still no IOKit consumer of any GT header --
   no mapping, no LRCA allocation, no execlist write, no fence, no interrupt.
5. This is not acceleration and does not approach it. See section 0.
6. `IG_GTI_WRITE_` has no set-check from the register gate, per section 4.

---

## 6. Open, highest value first

1. **`CLAUDE.md`'s read-order list stops at `intel-handoff28.md` and
   `3050-handoff37.md`** while the tree reaches `intel-handoff35.md` and
   `3050-handoff44.md`. Seven Intel sessions and seven NVIDIA ones are
   undocumented in the one file every session reads first, and this handoff adds
   an eighth. Inherited from handoff34 section 6 item 1 unchanged. The reason it
   was never updated -- concurrent agents colliding -- no longer holds, and it
   is now the oldest item on this list.
2. **An IOKit consumer for the GT stack.** Four headers, sixteen suites, and
   nothing calls any of it from a translation unit that reaches hardware. Until
   something does, every claim above is about arithmetic. This is the largest
   piece of Intel work not blocked on hardware and it has grown by one header
   each session for five sessions.
3. **Attribute the +14 in the Python total.** Inherited through
   `intel-handoff31.md` section 9 item 2 to handoff32, 33 and 34 unchanged,
   which makes it a claim with an age rather than a fact. Run the
   payload-freshness module alone and count.
4. **No gate covers the embedded Python in `tools/igpu-final-capture.sh`** -- 295
   lines inside a heredoc, checked by hand once. Inherited from
   `intel-handoff27.md` section 7 item 2.
5. **`tests/test_intel_scanout.cpp` still has no `MEASURED` entry**, so its check
   count has no floor. Inherited from handoff34 section 6 item 4.
6. An `IOFramebuffer` subclass remains the largest piece of unwritten DISPLAY
   work not blocked on hardware, and `phase0-kext/IntelFbPresent.hpp` already
   computes what it would need to publish.
