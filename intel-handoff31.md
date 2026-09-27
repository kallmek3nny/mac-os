# intel-handoff31.md

**Track:** Intel Gen12 (Tiger Lake) display
**Date:** 2026-08-20
**Continues:** `intel-handoff30.md`
**Shipping code changed:** yes -- `phase0-kext/IntelFbPresentProbe.cpp`, so `build-payload/` was rebuilt (FORTY SECOND image)

## 0 -- What this session did, and the one thing to read first

The operator asked for an `IOFramebuffer`. One already existed: `IntelFbPresentProbe`,
landed earlier the same day, wired into `SOURCES`, `Info.plist`,
`tests/CMakeLists.txt` and `tests/check-kext-syntax.bat`. So the honest continuation
was not a second copy but the hole this tree files after **every** IOKit shell
lands -- **no Python module named it**, which means deleting a verdict guard out of
any override left all four gates green.

**Section 2 is the one to read.** The gate found a second defect on its first run, of
the same class as the one found by hand an hour earlier, and that is the whole
argument for building it.

Unchanged and still true: this framebuffer cannot put a picture on the internal
panel. The laptop is muxless and Sequoia ships no Tiger Lake framebuffer, so
`PLAN.md` termination condition A holds from two independent directions. The gate
is worth having anyway, because what it protects is the **honesty of what the
driver reports**, and that is the only thing this track can still get wrong.

## 1 -- Why no existing gate could see it

Re-derived rather than remembered, because the four reasons differ per subject:

- the **register gate** reads `#define`s, and every constant involved is correct;
- the **C++ suites** drive `IntelFbPresent.hpp` directly and prove
  `igPresentVerdict()` and `igPresentUsable()` are correct -- they say nothing
  about whether the shell *consults* them;
- **`tests/check-kext-syntax.bat`** parses the translation unit, and clang has
  nothing to say about a deleted `if` around a well-formed `return`;
- nothing in the **Python gate** modelled 'every override consults the verdict'.

The consequence is worse here than in the five earlier occurrences of this class.
These eight overrides are what **macOS itself calls**, so a lost guard hands the
window server an `IOPixelInformation` filled out of `IG_PRESENT_INVALID32` -- and
at the IOKit boundary that is byte-identical to one filled from four registers
that genuinely read.

## 2 -- Two defects, and the second was found by the gate

**2a -- `getPixelFormatsForDisplayMode()` (`:564`), found by hand.** No
`presentable()` call, while three separate prose claims said otherwise: its own
comment (It is still guarded), the section header at `:440` (Every one of them
consults `presentable()` first), and `IntelFbPresentProbe.hpp:168` (Every one of
them consults `fPresentVerdict` before answering). A sentence nothing executes,
describing code that does the opposite -- the same class as a stale prose
constant, one level up.

**2b -- `isConsoleDevice()` (`:769`), found by the gate on its first run.** Same
shape, same three false claims covering it, and it survived a session in which
the first defect had already been read, fixed, and written about. That is the
durable half of this handoff: **a prose claim about a rule is not a measurement
of the rule**, and the only thing that separates them is a gate that derives its
subject set.

**2c -- both guards are BEHAVIOURALLY INERT and that is written into the code.**
`getPixelFormatsForDisplayMode()` returns `0` on both arms; `isConsoleDevice()`
returns `false` on both. No caller can distinguish a guarded run from an
unguarded one. The comment blocks say so explicitly, so no future reader infers a
refusal that does not happen -- and `isConsoleDevice()`'s note adds that the guard
must **never** be read as a hint that a presentable plane would claim the console.

## 3 -- The choice worth arguing about: an exemption-free rule

Two ways to make the rule gateable:

1. exempt the two inert methods inside `tests/test_intel_fb_present_shape.py`;
2. write the inert guards so the rule has **no exemptions**.

Chose (2). A hand-typed exemption set is precisely the anchor shape that silently
narrows its own subject and reports green over a smaller set --
`intel-handoff28.md` section 3 (a glob that matched neither of the two
irreplaceable captures) and `intel-handoff30.md` section 4b (an anchor sweeping a
set six smaller than it appeared) are both that defect. **One inert branch is
cheaper than a gate that can shrink without reddening.**

## 4 -- The gate

`tests/test_intel_fb_present_shape.py`, **12 tests**, in the idiom of
`tests/test_intel_publisher_scan_shape.py`: one `audit()` shared with the live
tree, comment- **and** string-stripped source, one positive control, and a
negative control per property each asserting *which* property broke.

| Property | What it pins |
|---|---|
| P1 | Every answering override consults `presentable()`; the override set is **derived** from the header's `APPLE_KEXT_OVERRIDE` declarations |
| P2 | `presentable()` is a conjunction of `fSampleTaken` **and** an equality against `IG_PRESENT_OK` |
| P3 | `fSampleTaken` is set once, only after the reads return |
| P4 | `IGPresentSampled` and `IGPresentVerdict` published first and unconditionally |
| P5 | Every value **decoded** from the four sampled words is published behind the guard |

String stripping is mandatory rather than stylistic: the shell logs a
not-presentable line, so a scan keeping string bodies finds the guard's own name
inside a log line in a method whose guard was deleted. `init`/`free`/`start`/`stop`
are excluded because they *run* the sample rather than answer a question about it
-- and that exclusion is itself asserted, so it cannot be widened silently. A
renamed subject, an absent verdict family, and an unmatched declaration pattern
each **refuse loudly** rather than pass; a module that measures nothing when its
anchor moves is indistinguishable from a green one.

## 5 -- Three mistakes of mine, and the second is the interesting one

**5a -- a capture-free pattern.** `m.group(1)` over a `setProperty` pattern with
no group 1 -- dead code anyway, since keys are recovered from the unstripped body
below it. Cheap.

**5b -- P5 was over-broad, and the live tree was right.** My first P5 reddened
five correct publications, because the four **raw** plane words are published
*ahead* of the guard on purpose: they are the evidence every decode below is
derived from, and an operator reading a refused run needs to see the words that
produced the refusal. The fix is not a typed key list -- that is section 3's
defect arrived at from the other side. A publication is a **decode** when an
`ig*()` function consumes one of `fCtl`/`fStride`/`fSize`/`fSurf`; a bare field in
the value position is a raw word. Structural, so a ninth decode publication is
policed on the commit that adds it. P5 also fails when it recognises **zero**
decodes, so it cannot pass over an empty set.

**5c -- a mutation anchor that matched something real and wrong.** The control
deleting a guard searched the file for the first `if (!presentable())`, which is
**`publishPresentation()`'s** -- a method P1 deliberately does not police. The
control read as a vacuous gate while the gate was right. Fixed by anchoring the
search from `IntelFbPresentProbe::getPixelInformation` and asserting the failure
**names that method**. This is `a-mutation-anchor-can-match-more-than-it-means` in
its third direction: not matching nothing, not matching twice, but matching one
real thing that is not the subject.

## 6 -- Gate readings

All four were run. Three of them were still unrun when this file was first
drafted, so the drafted version pointed at a later run rather than quoting
numbers -- that pointer is now replaced by the measurements themselves, which is
the only form of this table worth keeping.

| Gate | Baseline | This session |
|---|---|---|
| Python | 1219, exit 0 | **1257, exit 0, OK** |
| Registers | 242/242 | **242/242 passed, 0 failed, 0 skipped, exit 0** |
| C++ suites | 12/12 | **12/12 passed, 0 failed, exit 0** |
| Kext syntax | exit 0 | **exit 0, VERDICT: PASS** |

The register gate is unmoved at **242** and that is correct rather than
uninformative: this session added no `#define` anywhere. Its header list still
reads `IntelDisplay.hpp` at 133 defines and `IntelFramebuffer.hpp` at 48, checked
against the 3180 vendored in `reference/i915/`.

The C++ gate reports **12** suites, `test_intel_fb_present` among them, and that
number did not move either -- the suite over `phase0-kext/IntelFbPresent.hpp`
already existed. What this session added is a **Python** module, because the
subject is a shell no C++ suite can compile.

The syntax gate is the one that could genuinely have moved, since
`phase0-kext/IntelFbPresentProbe.cpp` changed. It exits 0. Read the exit code and
not the last verdict line: section 11 of that gate is titled *negative control --
each defect must FAIL*, so the word `FAIL` occurs on a passing run, and the file
prints `VERDICT: PASS` from a later checker even when an earlier section exited 1.
Both of those appeared in this reading and neither means anything on its own.

The Python gate went red once first, on two `test_operator_step7_doc`
derived-value checks, and the staleness was **mine** from ninety seconds earlier
-- I had rebuilt the payload and not yet updated the card. Attribute by mtime and
by the failure message; the correct action here was to fix it, unlike
`intel-handoff27.md` section 5a.

+38 total. **12** are the new module, measured by running it alone. The remaining
**26 are UNATTRIBUTED** -- most likely the payload-freshness suite now finding a
rebuilt image, but nothing was run alone to establish that, so it is not recorded
as a fact.

Two reading traps, both already recorded and both hit again: wrapping the batch
gates in a `cmd` call with a doubled-slash flag collapses the path because bash
eats the backslash, and a mangled invocation reads exactly like a gate failure --
invoke the batch directly from `tests/`. And `tail` on the Python gate lands in an
**HFS catalog dump**, because a tool test prints its own parse output; grep for
the `Ran N tests` line, `OK`, `FAILED`, and capture the exit code separately.

## 7 -- Payload

FORTY SECOND image. tar 522240 -> **532480**, ISO 600064 -> **610304** -- exactly
one 10240-byte block each, and the cause is **comment volume rather than logic**,
which is the fifth time this card has had to say that a byte size is not a
fingerprint.

- `kext-src.tar` md5 `79188866f5d95f5534e07946e63ce1af`
- `step7-payload.iso` md5 `8d6e4096986ef82f173fbec92c2acbfd`
- `guest-build-kext.sh` 12677 bytes, md5 `599b51eb31500c9ef9ff4b1f2dee0476` (unchanged)

## 8 -- What this does not establish

1. The two new guards are **inert**, so no run of anything can distinguish them
   from their absence. They exist to make a rule exemption-free, not to change
   behaviour.
2. `IntelFbPresentProbe` has still never been loaded and never matched. The
   syntax gate parses it; a parse is not a load.
3. P1's subject set is derived from `APPLE_KEXT_OVERRIDE`. A pure virtual
   satisfied **without** that macro is a method the gate does not consider an
   override at all -- and it reports nothing rather than failing, which is the
   vacuity direction. That is this session's own hole.

## 9 -- Open, highest value first

1. Close section 8 item 3 -- derive P1's subject set from the pure-virtual set
   `IOFramebuffer` demands, not from a macro the author has to remember. This is
   the cheapest remaining item and it is this session's own hole.
2. Attribute the unexplained +26 by running the payload-freshness module alone.
3. Harvest configuration registers out of
   `igpu-results/tgl-final-capture-2026-08-18-201412.txt` into
   `phase0-kext/IntelDisplay.hpp` -- still the largest piece of unspent evidence
   on this track, carried from `intel-handoff28.md` section 7 item 2.
   Configuration registers only: a read on a running display is a sample, not a
   constant.
