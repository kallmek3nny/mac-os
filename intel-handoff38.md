# intel-handoff38

Intel Gen12 (Tiger Lake) track. Continues from `intel-handoff37.md`.

One item, closed: `intel-handoff37.md` §6 item 1, the shape gate over
`composeImage()` / `publishComposedImage()`. That was the highest-value Intel
work not blocked on hardware and it is done.

`phase0-kext/` is **BYTE-IDENTICAL**. No shipping code changed, so
`build-payload/` was NOT rebuilt and the fifty third image is still current --
the proof is the payload freshness check being green rather than an assertion.

All four gates end green. Readings are in §4.

---

## 0. What moved

`tests/test_intel_probe_image_shape.py`, **18 tests**. It is the FOURTH
structural observer over `IntelGtProbe.cpp` and it completes the set: the MMIO
path, the ring write, the blit and the submit rehearsal each had one, and the
twelve-dword ring context image was the gap.

Nothing else changed. Python 1347 -> **1365**, and the +18 is attributed by
running the module alone BEFORE the gate rather than by subtraction -- the
numbers agree exactly, which is the only reason the attribution is worth
anything.

---

## 1. Why the gate had to exist

`tests/test_intel_gt_image.cpp` drives `igGtiWriteRingImage()` hard and proves
the WRITER is correct. It proves nothing whatsoever about the CONSUMER, because
no C++ suite can compile a translation unit that reaches `IOService`.

The separating mutation is one line: delete the `igGtiWriteRingImage()` call
from `composeImage()` and leave `publishComposedImage()` untouched. All four
gates stay green, and `IGGtImageDwords` publishes `init()`'s zero into the
IORegistry as though composed. That is this project's eighth meeting with
`handoff18.md` §1d's class: **`setProperty()` with a fabricated word is
byte-identical to `setProperty()` with a measured one.**

### 1a. The six properties

1. **The composer calls the writer and keeps BOTH out-values.**
   `igGtiWriteRingImage(` occurs in `composeImage()`, and the body mentions
   `fWriteVerdict`, `&fImageDwords` and `fImage`. A composer that calls the
   writer and drops the dword out-param publishes a count nobody measured.
2. **`start()` drives both halves, composition FIRST.** Publishing before
   composing publishes the initialiser, not the verdict.
3. **The verdict sorts ABOVE the values it qualifies.** `IGGtImageVerdict`
   before `IGGtImageDwords`, by byte offset in the publisher's body.
4. **The verdict is UNCONDITIONAL.** A verdict published only on the success
   arm cannot report a refusal, and an absent key is a question rather than an
   answer -- indistinguishable from an older kext that never published it.
5. **The span is sized from `IG_GTI_RING_IMAGE_DWORDS_POLICY_12`**, the
   header's own policy constant, never a second literal `12`. `intel-handoff37`
   §2 is why: a hand-written literal that is CORRECT today goes stale silently
   the day the header's policy moves, and no gate is positioned to notice.
6. **Exactly ONE call site in the whole file.** A second composer would race
   the first into the same `fImage` array.

---

## 2. `statement_is_bare()` was wrong twice, and both wrongs pass a guarded call

Property 4's negative control -- wrap the verdict's `setProperty()` in
`if (fImageDwords)` -- returned an **empty failure list** on the first run. The
mutation was right; the predicate was wrong, and it is worth recording because
both of the natural implementations are wrong in the same direction.

**Attempt one: brace depth.** Walk back counting braces; a guarded call sits one
level deeper than a bare one. This is false for the mutation that matters,
because `if (x) setProperty(...);` **has no braces at all** -- the guarded call
sits at exactly the same depth as the unconditional one. That is
`bareness-is-not-nesting-depth` arrived at from a third direction.

It fails a second way too. The walk starts INSIDE `setProperty()`'s own argument
list, so the first thing it meets going backwards is that call's unmatched `(`.
Decrementing an already-zero paren counter unwinds the walk into nonsense long
before it reaches any boundary.

**Attempt two, not taken: a regex over the preceding line.** `handoff25.md` §1c
is the standing cost of that -- a character class excluding only the statement
terminator is not scoped to the statement.

**What it is now:** walk back to the previous statement boundary by a
paren-balanced scan with the counter CLAMPED at zero (stepping out of an
enclosing call is expected, not an imbalance), then read what stands between
that boundary and the key against a guard-keyword set. A boundary is a `;` at
paren depth zero **or either brace**.

The `}` half is the load-bearing detail. A `}` met walking backwards is the END
of a preceding block statement, and it terminates that statement as firmly as a
semicolon does. Walk THROUGH it and the `if` heading that finished block gets
read as this statement's own guard -- so a correct unconditional publication
sitting directly below any guarded block reports as guarded, and the positive
control reddens on correct shipping code.

---

## 3. What the eight controls establish

`handoff18.md` §1e's every-pre-existing-check-green rule does NOT apply to an
all-new module -- there are no pre-existing checks to leave green. What the
controls establish instead is ISOLATION, and every one asserts the failure TEXT
rather than merely that the list is non-empty.

| Control | Reddens |
|---|---|
| rename the writer call | property 1 (`does not call`) |
| `&fImageDwords` -> `nullptr` | property 1 (out-param) |
| swap the two publication lines | property 3 (`sort above`) |
| wrap the verdict in an `if` | property 4 (`inside a conditional`) |
| `IG_GTI_..._POLICY_12` -> `12u` | property 5 |
| plant a second writer call | property 6 (`called at 2 sites`) |
| rename `composeImage(` in `start()` | property 2 |

Plus two REFUSAL tests. A renamed composer and a renamed publisher must each
RAISE with `REFUSING TO PASS`, never return an empty list -- **a gate that
reports nothing when its own subject has moved is indistinguishable from a green
one.**

`mutate()` checks its anchor occurs an EXACT expected number of times rather
than merely being present. Both directions are real failures and they look
nothing alike: an anchor matching nothing no-ops silently and reads as proof the
property is vacuous, while an over-matching one raises loudly and reads as
though the mutation is wrong when the precondition is working
(`intel-handoff26.md` §3).

### 3a. Comment stripping is mandatory here, not stylistic

`publishComposedImage()`'s own comment block names `IGGtImageVerdict` and
discusses the no-submission sentence in prose. A gate reading comments would
find every key it is looking for on a publisher whose `setProperty()` calls had
ALL been deleted. `test_the_stripper_removes_prose_naming_the_keys` asserts the
stripper actually removes them AND that it preserves every byte offset, because
every span this module computes is a slice index into the same string.

---

## 4. Gate readings

All four green, measured 2026-08-29 in this order:

| Gate | Reading |
|---|---|
| `tests\run-python-tests.bat` | **Ran 1365 tests ... OK**, exit 0, zero FAIL/ERROR lines |
| `tools/check-registers.py` | **260/260** passed, 0 failed, 0 skipped |
| `tests\check-kext-syntax.bat` | exit 0 |
| `tests\build-msvc.bat` | **100% tests passed, 0 failed out of 19** |

The register gate is unmoved at 260, correctly: a Python shape gate carries no
`#define`s. Every C++ count is unmoved, so `tests/test_check_count_floor.py`
needed no edit. Payload freshness is green without a rebuild, because
`phase0-kext/` did not change.

### 4a. The two reading traps fired again, exactly as `intel-handoff37` §4a said

Both were expected and both cost nothing, which is the only reason to write them
down a third time.

- **The Python gate's `tail` shows an HFS catalog dump**, not a verdict -- a
  tool test prints its own parse output. The reading that matters is
  `grep -E "^(Ran |OK|FAILED)"` plus the exit code captured BEFORE any pipe.
- **`grep FAIL` on a PASSING syntax run returns a hit.** Line 272 is section
  11's own title, `negative control -- each defect must FAIL`. This run grepped
  exactly that one line and exited 0.

---

## 5. What this does not establish

1. **Still no suite executes a line of `IntelGtProbe.cpp`**, by construction.
   The four shape gates are the only observers it has, and they are structural:
   they assert the code has the right SHAPE, never that it computes the right
   VALUE.
2. The gate spells `igGtiWriteRingImage(` by NAME. A second image writer added
   later yields a composition the gate does not consider one at all -- and it
   reports nothing rather than failing, which is the vacuity direction. Same
   hole `intel-handoff26.md` §6 item 4 names for `regRead32(`.
3. Never compiled by Apple's clang, never loaded, never matched.
4. Nothing was submitted to an engine. `intel-handoff37.md` §0.
5. The context image describes an LRCA **nobody allocated**.
6. Not acceleration.

---

## 6. Open, highest value first

1. **`CLAUDE.md`'s read-order list stops at `intel-handoff28.md`** while the
   tree reaches 38. Oldest item on the list; inherited unchanged from 37 §6
   item 2, 36 §7 item 2, 35 §6 item 1 and 34 §6 item 1, and this handoff makes
   it worse by one file. It is now the highest-value item precisely because
   every cheaper one has been closed.
2. **Attribute the Python total.** 1279 -> 1347 -> **1365**. This session's +18
   IS attributed (§0) and last session's +32 was, so what remains unattributed
   is the older 1279 -> 1315 stretch. Inherited through 37 §6 item 3 and 36 §7
   item 3 from `intel-handoff31.md` §9 item 2.
3. **No gate covers the embedded Python in `tools/igpu-final-capture.sh`** --
   295 lines inside a heredoc, checked by hand once. Inherited from
   `intel-handoff27.md` §7 item 2.
4. **`tests/test_intel_scanout.cpp` still has no `MEASURED` entry**, so its
   check count has no floor. Inherited from handoff34 §6 item 4.
5. An `IOFramebuffer` subclass remains the largest piece of unwritten DISPLAY
   work not blocked on hardware, and `phase0-kext/IntelFbPresent.hpp` already
   computes what it would need to publish.
