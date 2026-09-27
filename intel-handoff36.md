# intel-handoff36

Intel Gen12 (Tiger Lake) track. Continues from `intel-handoff35.md`.

This session closes that file's §6 item 2 -- the item it called the largest
piece of Intel work not blocked on hardware, and the one that had grown by one
header each session for five sessions. `phase0-kext/IntelGtProbe.hpp` and
`.cpp` are the **first IOKit consumer of the GT stack**: the first translation
unit in this tree that reaches for those headers from code that touches
hardware.

All five gates end green and the readings are in §5.

The session's real result is §2 -- three defects clang found in a file that
read correctly, one of which was in `tests/kextshim/` rather than in my code and
is the more dangerous of the three.

---

## 0. What this is, and the sentence it is easy to misread

The operator asked for an **accelerated** driver, retargeted it to the Intel
iGPU, and said to write it here rather than delegate it. This is the sixth
session on the GT stack and **it is still not acceleration.**

What changed is narrow and worth stating precisely. Before this session, five GT
headers and sixteen suites existed and *nothing called any of them* from outside
`tests/`. Every claim the stack made was a claim about arithmetic. Now one
`IOService` subclass maps BAR0, reads the render engine's ring registers,
composes a logical ring context image, and publishes what it found.

It **submits nothing.** There is no write path in `IntelGtProbe.cpp` -- not a
refusing one, not a disabled one, not one behind an `#if`. Section 1 is why.

So: a consumer exists. A picture does not. `PLAN.md` termination condition A
still holds -- the laptop is muxless -- and nothing here has been compiled by
Apple's clang, loaded, or matched.

---

## 1. Why the consumer submits nothing, and why that is the finding

`intel-handoff35.md` §1 measured it and it has not changed: greps of
`reference/i915/` for `LRC`, `CTX_DESC`, `RING_BUFFER_START`, `RING_BUFFER_CTL`,
`BB_PER_CTX` and `CTX_TIMESTAMP` return **ZERO matches**. All seven vendored
files are display. i915 keeps the logical ring context layout in `gt/` headers
that were never vendored here.

The page LAYOUT `IntelGtImage.hpp` composes is therefore this project's choice
end to end. Writing a policy-composed context descriptor into a real execlist
port is not a bringup step -- it is a guess submitted to silicon, and the failure
mode is not a refusal. It is an engine executing from an address nobody chose.

So the class composes, judges, and **publishes for inspection**. `IGGtSubmitted`
is published `false` unconditionally, as a constant rather than a derived value,
because no code path in the file can make it false -- there is no write path in
the file to make it false.

The one part of the GT stack with a citation is the ring block, and that is
exactly the part this class READS rather than assumes: `RENDER_RING_BASE` at
`reference/i915/i915_reg.h:289`, with `PRB0_BASE` spelled `(0x2030 - 0x30)`,
the subtraction being what fixes the ring block ABOVE the engine base rather
than at it.

---

## 2. Three defects clang found, and the third was in the shim

The file read correctly. It compiled with three errors, and the interesting
thing is that all three were **name-and-signature** defects of exactly the class
this gate exists for -- none would have been caught by reading, and none could
have been caught by any other gate in the tree.

### 2a. Two MMIO constants named from memory, in a header not included

`IG_MMIO_INVALID` and `IG_MMIO_ACCESS_BYTES` live in `IntelDisplay.hpp:105` and
`:113`. The header included five siblings and not that one.

The fix is the include, **not** a local respelling, and the header now says why:
neither constant is a display constant despite the filename. The sentinel is
what `regRead32()` answers on a refusal; the access width is what makes the
past-mapping test an overhang test rather than an off-by-three one. Both are
properties of MMIO on this device. Respelling them here would be this project's
own recorded defect -- two answers to one question, agreeing until one moves.

### 2b. The ring accessors have a different shape than I recalled

I wrote `igGtcRingReg(engine, offset)`. There is no such function. The real ones
are `igGtRingHeadReg(engine)`, `igGtRingTailReg()`, `igGtRingStartReg()` and
`igGtRingCtlReg()` -- **one argument each**, with the offset owned by the
accessor.

That is the third session running where I reached for a GT constant or function
by memory and got it wrong (handoff35 §4c records `IG_GT_MI_NOOP_POLICY_0x00`,
which is really `IG_GT_MI_OPCODE_NOOP_POLICY_0x00`). **Grep the tree; do not
recall the name.**

The purpose-built accessors are better than the general form plus a constant,
for the same reason `igGtMiNoop()` is better than the opcode literal: the
sibling owns which offset is HEAD, so a changed offset moves one spelling rather
than agreeing with a stale second one.

### 2c. The shim did not declare the `setProperty` Apple actually has

This is the one to carry.

`IntelGtProbe` publishes the composed image as a **blob** -- twelve dwords under
one key -- rather than as twelve numbered keys, because twelve keys invite
reading slot N as "the value of register N" and the slots are not registers.
That needs `setProperty(const char *, void *, unsigned int)`, which Apple's
`IORegistryEntry` really does declare and which `tests/kextshim/IOKit/IOService.h`
did not.

So the gate refused a call that is CORRECT on macOS. But look at the direction
of the omission before calling it a nuisance, because the mirror image is the
dangerous one and this shim exists to avoid it: a shim **narrower** than Apple's
rejects valid code loudly, here, for free. A shim **wider or flatter** than
Apple's accepts code that does not compile on macOS, and that failure appears on
the operator's one build attempt.

This is `handoff19.md` §2's finding one level down -- there the shim declared
`IODeviceMemory` in the wrong header, a file-layout difference; here it is a
missing overload. Matching Apple's SYMBOL set is not matching Apple's
SIGNATURE set either.

`length` is declared `unsigned int` rather than `IOByteCount` on purpose: Apple's
signature is `unsigned int`, and a wider parameter here would accept a call that
does not compile on the real SDK.

**`IntelGtProbe.cpp` is the first file in this tree to publish a blob**, which is
why nothing found this in eleven prior shell-parsing sessions.

---

## 3. What the class does, and the four choices worth arguing about

### 3a. It attaches even when the sample fails

`start()` calls `sampleRing()` and **discards the bool**. That is the opposite
call from `IntelFbPresentProbe`, which refuses to attach on a bad plane, and the
difference is what the two classes CLAIM.

`IntelFbPresentProbe` promises IOKit a presentable framebuffer, so attaching over
a surface it cannot describe is a promise it cannot honour. `IntelGtProbe`
promises nothing to anybody -- it is an inspection point. Refusing here would
delete the IORegistry node carrying the reading that says WHY it refused.

### 3b. The four ring words initialise to the SENTINEL, not to zero

`init()` sets all four to `IG_MMIO_INVALID` and carries `fSampleTaken`
separately.

Zero is a legal reading for every one of them. `igpu-results/` captured a machine
whose render engine was IDLE, so a zero CTL here is the EXPECTED value and is
published as a measurement rather than treated as a fault. An initialiser zero
that reads as a measurement is the exact shape that put three fabricated register
words into the NVIDIA track's IORegistry (`3050-handoff29.md` §2).

`fSampleTaken` is set **after** all four reads return, never before. A flag set
first reports a group as sampled on the one path where nothing was.

### 3c. The sample is taken ONCE

Same reason `IntelFbPresentProbe` caches its four plane words, and the same
measurement behind it -- `intel-handoff28.md` §4d: two reads of one register
inside a single run of `tools/igpu-final-capture.sh` **DISAGREED**. A register
read on running silicon is a SAMPLE, not a constant, and HEAD in particular is a
live pointer the engine moves. Reading it twice and publishing both would put two
descriptions of one ring in the IORegistry with no way to tell which was acted
on.

### 3d. The policy guard is deliberately NOT applied

`regRead32()` here has **two** guards -- unmapped, and past-the-mapping -- where
the display shells have three. `igReadDecision()` bounds offsets against
`IG_DISPLAY_POLICY_BASE..LIMIT`, and the render ring lives at `0x2030` and below,
underneath that range entirely. Passing a GT offset through the display decision
would refuse **every read this class makes**, correctly by its own arithmetic and
wrongly about the world.

The past-mapping test is `offset + IG_MMIO_ACCESS_BYTES > fBar0Length`, never
`offset >= fBar0Length`, because an offset at `length - 1` is inside the mapping
by any naive test and still reads three bytes past the end of it
(`handoff22.md` §2).

The composed image lives in a fixed member array rather than an
`IOBufferMemoryDescriptor` **because nothing is handed to hardware**: an
allocation would imply a page the engine could reach, and this class deliberately
produces no such page. It is sized from `IG_GTI_RING_IMAGE_DWORDS_POLICY_12`
rather than a literal, so the ten-vs-twelve defect of handoff35 §2a cannot recur
by a second spelling drifting from the first.

---

## 4. Registering the shell touched three places, and one of them was already right

1. `phase0-kext/Makefile` -- `SOURCES` grew from three shells to **four**. No
   test edit was needed, and that is worth knowing rather than assuming:
   `tests/test_kext_makefile.py` used to pin the COUNT of shells and that
   assertion was **removed** (handoff19 §3) precisely because it went red on
   correct code when the Intel track landed its first shell. The property it
   actually holds is "no header is compiled", which a fourth `.cpp` satisfies.
2. `tests/check-kext-syntax.bat` **section 3**, not section 1. `IntelGtProbe.hpp`
   carries angle-bracket IOKit includes, so `check-header-coverage.py` --  which
   derives the exemption from the presence of an ANGLE-BRACKET include -- EXEMPTS
   it from standalone parsing and instead requires it to be **reachable from a
   `.cpp` compiled in section 3**. That is the inverse of every GT header beneath
   it, and the gate's own output now confirms the reachability by name.
3. `Info.plist` needed nothing. `check-kext-bundle.py` derives `IOClass` from the
   `OSDefineMetaClassAndStructors` calls, and section 5 pairs
   `IntelGtProbe : IOService` with no personality required -- a class with no
   personality never matches, which for a read-only inspection point attached by
   hand is the correct default rather than an omission.

---

## 5. Gate readings

All five green, measured 2026-08-26 in this order:

| Gate | Reading |
|---|---|
| `tests\check-kext-syntax.bat` | all twelve sections ok, **exit 0** |
| `tools/check-registers.py` | **242/242 passed**, 0 failed, 0 skipped -- unmoved |
| `tests\run-python-tests.bat` | **Ran 1279 tests ... OK**, exit 0 |
| `tests\build-msvc.bat` | **100% tests passed, 0 failed out of 16**, exit 0 |
| payload freshness | green after the forty seventh rebuild, see §5b |

The register gate is unmoved at 242 **correctly**: `IG_GTP_` constants are this
driver's own bookkeeping rather than hardware, and `tools/check-registers.py`
parses only two Intel headers. The C++ count is unmoved at 16 correctly too --
this session added no suite, because a suite cannot compile a translation unit
that reaches IOKit. That is the whole point of the split and it is also this
session's honest limit (§6 item 1).

### 5a. The syntax gate was RED once, correctly, and it was the whole value of the session

Exit 1, one line: `FAIL IntelGtProbe.cpp`. §2 is what it measured. Three defects
in a file that read correctly, found in about four minutes, by the only gate in
the tree that reads a shell as C++.

Two recorded traps fired again on the way to that reading and each cost a round
trip:

- `cmd /c check-kext-syntax.bat` printed a **cmd banner and nothing else**, then
  `EXIT=0`. That zero is worthless -- the batch never started. Invoke the `.bat`
  directly from `tests/`.
- `tail -60` on the gate output showed sections 7 through 12 all passing while
  the gate had FAILED, because the failing line was above the window. **Grep for
  `FAIL` and read the exit code**; never read the last verdict line, which comes
  from a later checker (handoff18 §3).

A third is new and is a shell-quoting one: `"$ProgramFiles/..."` inside bash
expands to an unset variable, so clang appeared to be missing when the path was
simply wrong. The literal `/c/Program Files/...` form works.

### 5b. The payload rebuild grew the file COUNT for the first time on this track

The Python gate went red on two `test_operator_step7_doc` checks -- the md5 the
card quotes and the tar byte size -- which is expected and correct: `phase0-kext/`
gained two files and `build-payload/kext-src.tar` is compared byte-for-byte.

No peer attribution is available and none is needed; the operator has said there
are no peers, so `intel-handoff27.md` §5a has no premise left and the staleness
was this session's to own.

`build-payload/` was rebuilt -- the **FORTY SEVENTH** image -- and both md5s and
both byte sizes moved: tar 604160 -> **634880**, ISO 681984 -> **712704**,
exactly **three** 10240-byte tar blocks each rather than the usual one, because
this rebuild ADDED two files rather than editing one in place. Tar
`7b8cef8a...` -> `eaf09939...`; ISO `887da01c...` -> `08afb09e...`.

Both properties were read off the disk in the SAME measurement -- `md5sum` and
`stat` in one command -- which is the forty fifth block's own correction applied:
a byte size is not a fingerprint, and measuring one property of a file is not
measuring another.

---

## 6. What this does not establish

1. **No suite executes a single line of `IntelGtProbe.cpp`**, by construction:
   it pulls in IOKit's own `IOService` declarations, which do not exist off
   macOS. Every `IGLOG`, every guard, the sample, the composition call and all
   ten `setProperty()` calls are reachable **only by parse**. This is the same
   gap `IntelDisplayNames.hpp` cost when it was left standing, and the mitigation
   is the one that file's repair established: the file contains no shift, no
   mask, and no arithmetic over a register value, so there is nothing in it a
   suite would want to execute.
2. It has never been compiled by Apple's clang, never loaded, never matched. The
   shim is not the SDK -- §2c is a live demonstration that it differs.
3. Nothing was submitted to an engine and nothing will be until something in this
   tree can cite the LRC layout. §1.
4. The composed image describes a ring **nobody allocated**. `IG_GTP_RING_BASE_POLICY`
   and `IG_GTP_RING_BYTES_POLICY` carry POLICY in their names for that reason.
5. `IG_GTP_FIRST_PAIR_UNCITED` is a hole in the design, not a solved problem.
   The writer refuses a zero offset and otherwise writes whatever it is given.
   **CLOSED later on 2026-08-26 -- see `intel-handoff37.md` §1.** The three
   `gt/` headers were vendored, `RING_CONTEXT_CONTROL(base)` is
   `(base) + 0x244` in `reference/i915/intel_engine_regs.h:181`, and the
   constant is now `IG_GTP_FIRST_PAIR_I915_0x244`. The value did not move; it
   stopped being a guess.
6. This is not acceleration and does not approach it. See §0.

---

## 7. Open, highest value first

1. **A publication-shape gate for `IntelGtProbe.cpp`.** Delete the
   `fSampleTaken` guard, or the `IGGtSubmitted` publication, or swap a ring word
   for its neighbour, and **all five gates stay green** -- §6 item 1 is why. This
   is the exact hole `tests/test_probe_gsp_boot_publication.py` and
   `tests/test_intel_publisher_sample_shape.py` were each written to fill on
   their own subjects, and the idiom is established: one `audit()` shared with the
   live tree, comment-stripped source, negative controls each asserting WHICH
   property broke. This is now the largest piece of Intel work not blocked on
   hardware.
2. **`CLAUDE.md`'s read-order list stops at `intel-handoff28.md` and
   `3050-handoff37.md`** while the tree reaches `intel-handoff36.md` and
   `3050-handoff44.md`. Eight Intel sessions and seven NVIDIA ones undocumented in
   the one file every session reads first. Inherited from handoff35 §6 item 1 and
   handoff34 §6 item 1 unchanged; it is the oldest item on this list and this
   handoff makes it worse.
3. **Attribute the +14 in the Python total.** Inherited through
   `intel-handoff31.md` §9 item 2 to handoffs 32-35 unchanged, which makes it a
   claim with an age rather than a fact. Run the payload-freshness module alone
   and count.
4. **No gate covers the embedded Python in `tools/igpu-final-capture.sh`** -- 295
   lines inside a heredoc, checked by hand once. Inherited from
   `intel-handoff27.md` §7 item 2.
5. **`tests/test_intel_scanout.cpp` still has no `MEASURED` entry**, so its check
   count has no floor. Inherited from handoff34 §6 item 4.
6. An `IOFramebuffer` subclass remains the largest piece of unwritten DISPLAY
   work not blocked on hardware, and `phase0-kext/IntelFbPresent.hpp` already
   computes what it would need to publish.
