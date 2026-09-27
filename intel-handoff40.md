# intel-handoff40

Intel Gen12 (Tiger Lake) track. Continues from `intel-handoff39.md`.

The **COMPLETION** layer landed. `phase0-kext/IntelGtFence.hpp` is the ninth GT
header, `tests/test_intel_gt_fence.cpp` (**73** checks) drives it on Windows,
`phase0-kext/IntelGtProbe.cpp` grew `composeFence()` and
`publishComposedFence()` to consume it, and
`tests/test_intel_probe_fence_shape.py` (**22** tests) is the FIFTH structural
observer over that shell.

Shipping code CHANGED, so `build-payload/` was rebuilt -- the **FIFTY FOURTH**
image -- and `OPERATOR-ACTION-step7-build.md` was updated. Both were
obligations rather than choices: the rule that decides a rebuild is CONTENT,
and the Python gate is the thing that reads content.

All four gates are green as of this writing. Read §3 before trusting that
sentence, because five of the nine failures that stood mid-session were about
the COUNT LINE rather than about the fence at all.

---

## 0. What this is and what it is not

Same sentence every GT handoff since `intel-handoff33.md` §0 has had to carry,
and it does not get weaker by repetition. The operator asked for an
**accelerated** driver and retargeted it to the Intel iGPU. What lands each
session is command-packet arithmetic with a host suite over it. This session's
header composes a completion signal; **it fires nothing**, and
`IntelGtProbe.cpp` writes ZERO registers on every path it has ever had.

`fFenceRetired` is published `false` unconditionally, because nothing in this
tree has ever observed a fence retire. That is a measurement of this driver,
not of the hardware.

---

## 1. What was missing, and why it is the last structural gap

`IntelGtBlt.hpp` composes packets saying what the engine would DO.
`IntelGtBatch.hpp` assembles those into a terminated batch buffer.
`IntelGtRing.hpp` writes an `MI_BATCH_BUFFER_START` into a circular buffer.
`IntelGtExec.hpp` composes the descriptor that would hand it over.

None of them answers **how this driver would ever learn the work finished**.

That absence is not cosmetic. A submission whose completion cannot be observed
is indistinguishable, from the driver's side, from a submission that never ran
at all -- and the two call for opposite next actions. Without a fence, the only
available completion test is a timeout, and a timeout reports the same word for
"the engine is wedged" and "the engine is slow".

`IntelGtFence.hpp` composes the store that would make the difference
observable: a sequence number written to a driver-owned address, ordered AFTER
the work it is reporting on. The ordering is the whole content of the layer --
a fence store that can be reordered ahead of the work it fences reports
completion for work that has not happened, which is worse than no fence,
because it is a fence that lies in the direction of proceeding.

---

## 2. The five properties the shape gate pins

`tests/test_intel_probe_fence_shape.py`, 22 tests, in the idiom of the four
siblings: one `audit()` shared with the live tree, comment- and string-stripped
source, positive controls, and negative controls each asserting WHICH property
broke. The live tree is never written to -- every mutation is in memory.

The properties are the ones no C++ suite can reach, because
`IntelGtProbe.cpp` pulls in `IOService` and no host suite can compile the
translation unit:

1. `composeFence()` is CALLED from the probe's compose path, not merely
   defined. This is the eighth appearance of handoff18 §1d's class in this
   tree, and the separating mutation is one line: delete the call and leave the
   publisher alone, and `IGGtFenceVerdict` publishes `init()`'s
   `IG_GTF_FENCE_NOT_ATTEMPTED` as though composed.
2. The verdict is published UNCONDITIONALLY -- no guard between the previous
   statement boundary and the `setProperty()`. Decided by handoff38 §2's
   paren-balanced walk back to the boundary with the counter CLAMPED at zero,
   treating `}` as a boundary. Not by brace depth: `if (x) setProperty(...);`
   has no braces and sits at exactly the same depth as an unconditional call.
3. `fFenceRetired` is published and is published `false`. A retired flag that
   is absent is a question rather than an answer
   (`an-absent-key-is-a-question-not-an-answer`), and a retired flag that is
   true is a fabricated measurement.
4. The composed address and the ordering flag AGREE with what the composer
   produced -- the mutation hits the join's argument list only, with the
   composer pinned as untouched, per handoff37 §3b. A mutation that swaps a
   value in BOTH places reddens the property and proves nothing, because the
   property is about their agreement.
5. **No MMIO store exists on the fence path.** The absence is a GATED property
   rather than a prose claim, same construction as handoff37 §3's property 5.

Keys are matched WITH their closing quote, because `IGGtFenceVerdict` is a
PREFIX of `IGGtFenceVerdictName` (handoff37 §3b, still biting).

---

## 3. The count line is an INTERFACE -- five failures from one printf

This is the section to read, and it is not about the fence.

`tests/test_intel_gt_fence.cpp` ended on
`"\ntest_intel_gt_fence %d checks, %d failures\n"`. Reasonable-looking: it names
the suite, which is friendly to a human reading a scrollback of twenty suites.

It reddened **five** checks across two modules, in two different ways, and
neither failure message says "format string":

- `test_check_count_last_line` matches the count line against exactly two
  COMPILED patterns, and a suite name inside the literal matches NEITHER. So
  `_format_index()` returned `None`, and the two tests that INDEX on that
  return **ERRORED** rather than failed -- `test_count_printf_is_the_last_printf_in_main`
  and `test_nothing_is_called_after_the_count` raised on the `None`.
- Two more FAILED as list-equality against a hardcoded roster of which suites
  use which format.
- And `test_check_count_floor` could not read the binary's own output at all,
  because its extractor is the same pair of patterns. **The suite ran clean and
  printed 73, while the gate over it reported the count as MISSING.**

The durable half is that last bullet. `test_intel_gt_ring` hit the identical
thing on 2026-08-28 -- the floor table's own provenance comment at
`tests/test_check_count_floor.py:1254` records it, in those words: *the final
printf is an INTERFACE, and a suite name in it is not a decoration.* I hit it
again one session later, on a new suite, having read that comment.

So the rule is worth stating flatly rather than as a lesson: **the last line of
a C++ suite is consumed by machines, and there are exactly two legal spellings
of it.** Copy the line from a sibling; do not compose it.

The fix was one edit. `73 checks, 0 failures` now, matching the MEASURED entry
at `:1303` exactly, so the floor gate needed no edit.

### 3a. A sixth failure from a comment

`test_cmake_registration.PremiseTests.test_no_quoted_hash_in_cmakelists`
reddened on `tests/CMakeLists.txt:461`, a COMMENT I wrote describing the poison
fill:

> a zero-filled buffer cannot tell "wrote nothing" from "wrote zeroes"

The module strips CMake comments by treating every `#` as a comment start, and
this premise test exists to pin that the stripper's assumption HOLDS in this
file -- quoted text on a `#` line is exactly the case where it would not. The
gate is right and my prose was the counterexample. Rewritten without the
quotes.

Worth noting because it is the third distinct time this tree has reddened a
gate with a **comment** rather than with code (`intel-handoff26.md` §3,
`handoff20.md` §3, this). Docs in this checkout are inputs to gates.

---

## 4. Gate readings

All four run from the project root, all four measured after the last edit:

| Gate | Reading |
|---|---|
| `python tools/check-registers.py` | **260/260** passed, 0 failed, 0 skipped |
| `tests\build-msvc.bat` | 100% passed, **20** suites, 0 failed |
| `tests\check-kext-syntax.bat` | **exit 0**, VERDICT: PASS |
| `tests\run-python-tests.bat` | **Ran 1407 tests**, OK, exit 0 |

Register gate **unmoved at 260**, correctly: every constant in
`IntelGtFence.hpp` is POLICY, and the register gate counts only cited ones.
handoff37 §1a's asymmetry still applies and is still the honest limit -- **a
POLICY constant going wrong is invisible to that gate by construction.**

Python **1385 → 1407**, and the +22 is attributed by running
`test_intel_probe_fence_shape.py` ALONE before the gate: it reported 22, and
the total moved by exactly 22. Attributed, not inferred from arithmetic --
which is the correction `intel-handoff39.md` §5a had to make about a number
nobody measured.

C++ suites 19 → **20**.

---

## 5. The payload

Rebuilt, because `phase0-kext/` CHANGED (one new header, one modified shell).
FIFTY FOURTH image.

Both md5s AND both byte sizes moved -- tar 727040 → **757760**, ISO 804864 →
**835584**, exactly THREE 10240-byte tar blocks each. Size and hash were read
off the disk in one measurement (`len()` and `hashlib.md5()` over the same
bytes), which is the forty fifth block's correction still applied, and it
matters because `a-byte-size-is-not-a-fingerprint`: tar pads to 10240 bytes, so
three of the last six images moved a hash with a size fixed.

---

## 6. What this does NOT establish

1. **The fence has never fired.** Nothing in this tree has observed a sequence
   number retire, because nothing in this tree has submitted anything. Property
   5 gates that the absence is real; it does not make the absence progress.
2. `IntelGtFence.hpp`'s constants are POLICY. The vendored `reference/i915/`
   headers do not cite them -- grep before assuming otherwise, per
   handoff37 §2a, and **compose a value from the sibling that owns it** so that
   getting the NAME wrong is the only mistake available.
3. The shape gate spells `composeFence(` by NAME. A second fence composer added
   later yields a composition the gate does not consider one at all, and it
   reports NOTHING rather than failing -- the vacuity direction, the same hole
   `intel-handoff26.md` §6 item 4 names for `regRead32(` and handoff38 §5 item 2
   names for `igGtiWriteRingImage(`. Five modules now share this hole.
4. `IntelGtProbe.cpp` has still never been `kextload`ed, never matched, never
   run. It compiles under Apple clang and that is the entire claim.

---

## 7. Open, highest value first

1. **The five shape gates all spell their subject by name** (§6 item 3). This
   is now a class rather than an instance: five independent modules with the
   identical vacuity hole, each reporting green when its anchor moves. A
   DERIVED subject set is what fixes it -- `intel-handoff26.md` §2a already did
   this once for publishers (a publisher is a method that BOTH calls
   `regRead32(` and `setProperty(`), so the pattern exists in this tree and has
   not been reached for since.
2. ~~`tests/test_intel_scanout.cpp` still has **no `MEASURED` entry**~~ **DONE
   2026-09-26.** The entry exists in `tests/test_check_count_floor.py`
   (`"test_intel_scanout": 61`, with a dated provenance comment beside the
   table), and the binary was re-run the same day: 61 checks, 0 failures. Two
   sessions planned this item from the stale text before re-measuring it;
   CLAUDE.md §Session discipline rule 2 now requires confirming an item is
   still open before planning it.
3. The embedded Python in `tools/igpu-final-capture.sh` has still never RUN,
   and the machine that could run it is gone (`intel-handoff39.md` §6 item 1).
   Permanently unclosable; do not open it as work.
4. Before building any IOFramebuffer: **one already exists.**
   `IntelFbPresentProbe`, wired into `SOURCES` and `Info.plist`, eleven
   overrides, a `presentable()` guard and a shape gate over it. Grep the tree
   first -- `intel-handoff31.md` §0 and `intel-handoff32.md` §0 are the same
   finding filed twice because somebody nearly built it a third time.
5. Environment, measured 2026-09-26: `cl` and `clang` are NOT on PATH (both
   report "Could not find" from the build scripts); MinGW `g++` IS available
   (WinGet mingw64). Some `tests/build` artifacts (`test_intel_display_names`,
   `test_intel_fb_present`) date to Aug 19-20 while MEASURED cites runs
   through Aug 30 -- check a binary's mtime against its source before
   trusting a run, and resolve the actual working build path before planning
   any item that requires building a suite. The project root is now a git
   repository (baseline committed 2026-09-26); diff it instead of re-deriving
   what the last session changed.
