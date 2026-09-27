# intel-handoff37

Intel Gen12 (Tiger Lake) track. Continues from `intel-handoff36.md`.

This file was written in two passes -- a skeleton holding the measurements while
they were fresh, then the arguments filled in place. It is complete as it
stands; no section is a placeholder. `intel-handoff36.md` §6 item 5
forward-references an `intel-handoff37.md` that did not exist until this write
-- that reference is now satisfied, and its subject
(`IG_GTP_FIRST_PAIR_I915_0x244`) is §1 here.

All five gates end green. Readings are in §4.

---

## 0. What moved

The execlist layer now has a caller. Before this session
`phase0-kext/IntelGtExec.hpp` was arithmetic nothing outside `tests/` invoked;
now `IntelGtProbe::rehearseSubmit()` reads `EXECLIST_STATUS`, composes a context
descriptor over `IG_GTP_LRCA_POLICY`, judges the whole submission through
`igGtxSubmitReady()`, and `publishSubmitRehearsal()` puts the verdict, its name,
the descriptor and its validity into the IORegistry.

It **submits nothing.** `igGtxExeclistSqReg()` and `igGtxExeclistControlReg()`
-- the two accessors that would actually submit -- do not occur in the shell at
all, and `IGGtSubmitted` is published `false`. That absence is now a gated
property rather than a prose claim; see §3.

This is still not acceleration. `PLAN.md` termination condition A still holds
(the laptop is muxless), and nothing here has been compiled by Apple's clang,
loaded, or matched.

---

## 1. The three `gt/` headers were vendored

`reference/i915/` grew `intel_lrc_reg.h`, `intel_engine_regs.h` and
`intel_gpu_commands.h`. That closes intel-handoff36 §6 item 5:
`RING_CONTEXT_CONTROL(base)` is `(base) + 0x244` in `intel_engine_regs.h:181`,
so `IG_GTP_FIRST_PAIR_UNCITED` became `IG_GTP_FIRST_PAIR_I915_0x244`. **The
value did not move; it stopped being a guess.**

The register gate reads all three and now reports **260/260**, up from 242.

### 1a. What is cited now, and what is still POLICY

The vendoring does NOT make the GT stack cited. It makes three specific things
cited and leaves the rest exactly as uncertain as they were, which is the
distinction this tree exists to preserve -- a value invented to fill a field and
then filed next to values witnessed on hardware destroys the only evidence trail
there is.

Cited by the three new files:

- `RING_CONTEXT_CONTROL(base)` = `(base) + 0x244`, `intel_engine_regs.h:181`.
  This is the one that closed handoff36 §6 item 5.
- The `MI_*` and `XY_*` command opcodes `IntelGtCmd.hpp` and `IntelGtBlt.hpp`
  encode, `intel_gpu_commands.h`. `IG_GTB_DEPTH_32_I915_0x03000000` at :278 is
  the one §2 turns on.
- The LRC register offsets `IntelGtContext.hpp` composes, `intel_lrc_reg.h`.

Still POLICY, and named so:

- **Every constant in `IntelGtExec.hpp`.** Greps for `EXECLIST`, `ELSP`,
  `CTX_DESC`, `GEN8_PDE`, `GEN8_PTE`, `PPAT` and `GGTT` still return ZERO across
  `reference/i915/`. The submission port layout is this project's choice end to
  end, which is precisely why §3 property 5 exists.
- `IG_GTP_LRCA_POLICY`, `IG_GTP_CTX_SWID_POLICY`, `IG_GTP_BATCH_ADDR_POLICY`,
  `IG_GTP_RING_BASE_POLICY`, `IG_GTP_RING_BYTES_POLICY` and the six
  `IG_GTP_BLIT_*` geometry values. None of these describes anything allocated;
  they exist so the compositions have admissible inputs and the published
  verdicts are statements about the WRITER rather than about a surface.

The asymmetry to carry: a cited constant can be checked by
`tools/check-registers.py` against i915's own header, and a POLICY one cannot be
checked by anything. The gate's 260 is therefore a count of the FIRST kind only,
and a POLICY constant going wrong is invisible to it by construction.

---

## 2. Two clang errors, both ARITY

`tests\check-kext-syntax.bat` was RED once, and both defects were
name-and-signature -- the class only this gate can find, for the third session
running (36 §2 is the same finding).

| Call | Passed | Required | Missing |
|---|---|---|---|
| `igGtbWriteColorBlit()` | 10 | 11 | `depthFlag` |
| `igGtxSubmitReady()` | 7 | 8 | `lengthBytes` |

Both new constants are **COMPOSED, not spelled**:

- `IG_GTP_BLIT_DEPTH_POLICY` = `IG_GTB_DEPTH_32_I915_0x03000000`. That header's
  line 110 is why: the value is a PRE-SHIFTED header dword -- `BLT_DEPTH_32` is
  `(3 << 24)`, not `3` -- so a hand-written `3` composes an 8bpp packet while
  reading as thirty-two to anybody skimming.
- `IG_GTP_SUBMIT_LENGTH_BYTES_POLICY` =
  `IG_GT_MI_BB_START_DWORDS_POLICY_3 * IG_GT_CMD_WORD_BYTES_I915_4`. The ring
  writer emits exactly one `MI_BATCH_BUFFER_START`, so a packet that grows a
  dword moves this with it rather than drifting from a literal 12.

### 2a. I invented a constant name twice and grep caught it both times

My first edit wrote `IG_GTP_BLIT_DEPTH_FLAG_POLICY` and my second wrote
`IG_GTP_SUBMIT_LENGTH_BYTES_POLICY` -- **neither existed**. This is
intel-handoff36 §2b for the fourth session running: **grep the tree, do not
recall the name.** The difference this time is that the grep ran BEFORE the
gate rather than after, so each cost one call instead of one round trip.

The two failure modes are not symmetric, and the safe one is the one that
happened. A constant name that does not exist is a COMPILE error: clang names
the identifier, the gate goes red, and the cost is one edit. A constant VALUE
written by hand is not -- `3` where `(3 << 24)` is meant compiles cleanly,
composes a packet claiming 8bpp, and every gate in this tree stays green over
it, because `tools/check-registers.py` checks the constants a header DEFINES
against i915's own and has nothing to say about an argument spelled at a call
site.

So the rule is narrower than "grep the name": **compose the value from the
sibling that owns it, so that getting the name wrong is the only mistake
available.** Both of §2's new constants are written that way, and both would
have been silently wrong as literals -- `3` for the depth flag, `12` for the
length. A wrong literal in the length is the worse of the two, because it does
not even look like a magic number: twelve bytes is what three dwords ARE today,
so the value is CORRECT until somebody adds a dword to the packet, at which
point the ring writer and the submit verdict disagree about what was written
with no gate positioned to notice.

That is `3050-handoff33.md` §5a's finding one track over: a hand-written field
width in place of a missing `_BITS` constant "would have compiled and passed as
a silent second copy of a field layout."

---

## 3. `tests/test_intel_probe_submit_shape.py` -- 32 tests

The structural observer over `rehearseSubmit()` / `publishSubmitRehearsal()`.
Same idiom as its three siblings (`..._mmio_path.py`,
`..._ring_write_shape.py`, `..._blit_shape.py`): one `audit()` shared with the
live tree, comment-stripped source preserving byte offsets, balanced brace and
paren walks rather than regexes, REFUSAL strings when an anchor moves, and
thirteen negative controls each asserting WHICH property broke.

**Property 5 is the one no sibling has.** It asserts the two SUBMITTING
registers are never even ADDRESSED -- `igGtxExeclistSqReg(`,
`igGtxExeclistControlReg(`, `IG_GTX_EXECLIST_SQ_POLICY`,
`IG_GTX_EXECLIST_CONTROL_POLICY` and `IG_GTX_EXECLIST_LOAD_BIT_POLICY` must
not occur anywhere in the file. §0's claim is worth exactly what an observer
can falsify.

### 3a. The six properties

1. **The shell reaches the header.** `rehearseSubmit()` calls
   `igGtxExeclistStatusReg(` exactly once, `igGtxContextDescriptor(` exactly
   once and `igGtxSubmitReady(` exactly once; `start()` calls
   `rehearseSubmit();` before `publishSubmitRehearsal();`. Publishing before
   composing publishes the initialiser, not the verdict.
2. **The verdict is published, and named.** Both `IGGtSubmitReadyVerdict` and
   `IGGtSubmitReadyVerdictName` occur in the publisher, and the name comes from
   `igGtxReadyVerdictName(` -- the header's own switch, never a string composed
   in the shell. `IGGtSubmitRehearsed` sorts above every value it qualifies.
3. **The verdict is published OUTSIDE the admissibility guard.** A verdict
   published only when `igGtxSubmitAdmissible()` is true cannot report a
   refusal, which is the entire class of thing it exists to report.
4. **The descriptor is composed once and judged over the same inputs.**
   `igGtxSubmitReady()`'s argument list must mention `IG_GTP_LRCA_POLICY` --
   the same LRCA `igGtxContextDescriptor()` was handed. Judging a different one
   makes the verdict a statement about a context page nothing in this class
   ever composed.
5. **The submitting registers are never addressed.** §3 above.
6. **`IGGtSubmitted` is published `false`, as a literal.** Not a derived value:
   no code path in the file can make it false, because there is no write path
   in the file to make it false.

### 3b. The control that had to be narrowed twice

The sharpest of the thirteen is property 4's: replace `IG_GTP_LRCA_POLICY` with
a bare `0x2000ull` in the JOIN's argument list, then assert that
`igGtxSubmitReady()`'s arguments no longer mention the constant **while
`igGtxContextDescriptor()`'s still do**.

That second half is the whole control. A mutation that swaps the LRCA in BOTH
calls reddens property 4 too -- and proves nothing, because the two calls then
still agree with each other and the property is about their AGREEMENT. A control
that reddens both halves is not isolating a disagreement; it is renaming a
constant. So the mutation is written to hit the join's argument list only, and
the assertion pins the composer as untouched.

This needed `call_args()` -- a balanced PAREN walk -- rather than a regex,
because `igGtxSubmitReady()`'s argument list spans seven lines and
`igGtxExeclistStatusReg(...)` nests inside `regRead32(...)` three lines above
it. `handoff25.md` §1c is the cost of the alternative: a character class
excluding only the statement terminator runs straight past the closing paren
and lands on the next statement.

The keys are also matched WITH their closing quote -- `"IGGtSubmitReadyVerdict"`
rather than `IGGtSubmitReadyVerdict` -- because the bare spelling is a PREFIX of
`IGGtSubmitReadyVerdictName` and a substring test cannot tell them apart. That
is `tests/test_intel_probe_ring_write_shape.py`'s own recorded red run
(`fRingTail` prefixing `fRingTailAfter`) inherited rather than repeated.

---

## 4. Gate readings

All five green, measured 2026-08-29 in this order:

| Gate | Reading |
|---|---|
| `tests\check-kext-syntax.bat` | exit 0 (RED once first -- §2) |
| `tools/check-registers.py` | **260/260** passed, 0 failed, 0 skipped |
| `tests\run-python-tests.bat` | **Ran 1347 tests ... OK**, exit 0 |
| `tests\build-msvc.bat` | **100% tests passed, 0 failed out of 19** |
| payload freshness | green after the FIFTY THIRD rebuild |

The register gate moved 242 -> 260 because §1 vendored three headers. The C++
count moved to 19 suites. The Python gate was RED once on payload freshness,
correctly and by my own edit; rebuilt, tar 706560 -> **727040**, ISO 784384 ->
**804864**, two 10240-byte tar blocks each. Both md5s and both sizes were read
in ONE measurement (`md5sum` and `stat` in one command) -- a byte size is not a
fingerprint.

### 4a. Two reading traps fired again, and neither is a bug in the gate

Both are recorded in `intel-handoff36.md` §5a and both cost a round trip there.
They cost nothing this session because the invocation was written to expect
them, which is the only reason to write them down again.

- **`grep FAIL` on a PASSING syntax run returns a hit.** Section 11 of
  `tests\check-kext-syntax.bat` is *titled* `negative control -- each defect must
  FAIL`, so the word occurs in the gate's own section header regardless of
  outcome. The reading that matters is the EXIT CODE; the grep is only useful
  for locating a real failure once the code says there is one. This session's
  green run grepped exactly one line -- line 272, that title -- and exit 0.
- **Never invoke the `.bat` through `cmd /c`.** It prints a cmd banner and
  nothing else, then reports exit 0. That zero is worthless: the batch never
  started, so it describes the wrapper rather than the kext. Invoke the file
  directly from `tests/`.

A third is worth adding to the pair, because it is the same shape one layer up:
**never read the last verdict line of the syntax gate.** It is printed by a
later checker than the one that failed, so a run that exits 1 on section 3 can
still end with `VERDICT: PASS` (`handoff18.md` §3). Every one of these three
traps has the identical structure -- a green-looking token produced by something
other than the thing under test -- which is this tree's oldest and most
frequently re-learned lesson, and the reason the shape gates in §3 answer with
REFUSAL strings instead of silence.

---

## 5. What this does not establish

1. **No suite executes a line of `IntelGtProbe.cpp`**, by construction. The
   four shape gates are the only observers it has.
2. Never compiled by Apple's clang, never loaded, never matched.
3. Nothing was submitted to an engine. §0.
4. The context descriptor describes an LRCA **nobody allocated**.
5. Not acceleration. §0.

---

## 6. Open, highest value first

1. **A shape gate over `composeImage()` / `publishComposedImage()`.** It is the
   one composition in `IntelGtProbe.cpp` with no structural observer: the four
   siblings cover the MMIO path, the ring write, the blit and the submit
   rehearsal, and the twelve-dword context image is the gap. Delete the
   `fSampleTaken` guard or swap a ring word for its neighbour and all five
   gates stay green -- §5 item 1 is why. Highest-value Intel work not blocked
   on hardware.
2. **`CLAUDE.md`'s read-order list stops at `intel-handoff28.md`** while the
   tree reaches 37. Oldest item on the list; inherited unchanged from 36 §7
   item 2, 35 §6 item 1 and 34 §6 item 1, and this handoff makes it worse by
   one file.
3. **Attribute the Python total.** 1279 -> 1347 is +68 across two sessions and
   nobody has run the new modules alone to split it. Inherited through 36 §7
   item 3 from `intel-handoff31.md` §9 item 2, which makes it a claim with an
   age rather than a fact -- and this session's own +32 is the first component
   anybody could measure cheaply, by running
   `tests/test_intel_probe_submit_shape.py` alone.
4. **No gate covers the embedded Python in `tools/igpu-final-capture.sh`** --
   295 lines inside a heredoc, checked by hand once. Inherited from
   `intel-handoff27.md` §7 item 2.
5. **`tests/test_intel_scanout.cpp` still has no `MEASURED` entry**, so its
   check count has no floor. Inherited from handoff34 §6 item 4.
6. An `IOFramebuffer` subclass remains the largest piece of unwritten DISPLAY
   work not blocked on hardware, and `phase0-kext/IntelFbPresent.hpp` already
   computes what it would need to publish.
