# handoff20 — the Intel shell's pure logic gets a suite, and a missing switch arm falls out of it

Written 2026-08-17, later the same day as `handoff19.md`. Continues it directly.

handoff19 §8 item 1 was the highest-value open item on the Intel track:
*"Nothing tests the shell's own logic."* This session closes it, and the closing
found a real defect in the IORegistry output on the first exhaustiveness check.
No register constant moved. No decoder changed. What moved is **which
translation unit** the Intel track's pure functions live in — and that alone
turned four gates' worth of green into one red check.

## Gate state, measured after the last edit in this session

| Gate | Reading |
|---|---|
| `python tools/check-registers.py` | exit 0 — 239/239 passed, 0 failed, 0 skipped, over 3180 i915 defines |
| `tests\build-msvc.bat` | exit 0 — 100% tests passed, 0 failed out of **9** |
| `tests\check-kext-syntax.bat` | exit 0 — ALL SYNTAX GATES PASSED |
| `tests\run-python-tests.bat` | exit 0 — Ran 885 tests, OK |

C++ suite counts, read off the binaries in `tests/build/` at the end of the
session rather than remembered:

| Suite | Checks |
|---|---|
| `test_decode` | 96/96 passed, 0 failed — moved by a PEER mid-session, see §1d |
| `test_falcon` | 169/169 passed, 0 failed |
| `test_riscv_ucode` | 134 checks, 0 failures |
| `test_gsp_boot` | 313 checks, 0 failures |
| `test_gsp_stage` | 181 checks, 0 failures |
| `test_intel_display` | 373 checks, 0 failures — 354 plus §1c's 19 |
| `test_intel_gpu_id` | 77 checks, 0 failures |
| `test_intel_framebuffer` | 110 checks, 0 failures |
| **`test_intel_display_names`** | **236 checks, 0 failures — new this session** |

Eight suites became nine. Read the count table as of this date only; the point
of `tests/test_check_count_floor.py` is that a number in prose cannot be gated.

## §1 — `igModeVerdictName()` was missing an arm, and the missing arm was the one that matters

### What was wrong

`igModeVerdictName()` had case arms for verdicts 0 through 5.
`IG_MODE_VERDICT_LIMIT` is **6**. So `IG_MODE_SRC_MISMATCH` — verdict 6 — fell
to `default` and published the unknown string into the IORegistry.

That is the single worst verdict in the family to lose. `IG_MODE_SRC_MISMATCH`
is PIPESRC disagreeing with the active region computed from the raster timings,
which is the byte-swapped `PLANE_SIZE`/`PIPESRC` shape handoff15 §1 measured on
real silicon (`0x077F0437` against `0x0437077F`). It is the verdict that catches
a decoder reused across the two registers with the axes transposed — a mistake
that programs cleanly, yields a plausible number, and produces a picture that is
wrong in a way nobody notices from a register dump. The one verdict whose whole
job is to name that condition on the operator's screen said nothing useful.

### Why no gate could see it

All four gates were green over this, and each was blind for its own structural
reason. Worth spelling out, because the shape recurs:

- **The register gate** (`tools/check-registers.py`) checks *constants* against
  vendored upstream headers. `IG_MODE_SRC_MISMATCH` is defined correctly and
  `IG_MODE_VERDICT_LIMIT` is 6 correctly. A switch with a missing arm is not a
  constant defect.
- **The C++ suites** compile the dependency-free headers. The switches were
  file statics inside `phase0-kext/IntelDisplayProbe.cpp`, which pulls in
  IOKit's `IOService` declarations and therefore cannot be compiled by any host
  suite on any machine without macOS. Not "was not covered" — **could not be**.
- **The clang syntax gate** parses that file against `tests/kextshim/`. A switch
  over an unsigned int with six arms and a `default` is valid C++. `-Werror`
  does not fire: there is no enum to be exhaustive over, because these verdicts
  are `#define`d `unsigned int` constants, not an `enum`. The very thing that
  makes the headers dependency-free removes the compiler's ability to help.
- **The Python gate** reads source shape, plists, Makefiles and docs. Nothing in
  it models the relation "every verdict below the family's own limit has a name".

So the defect was invisible to every gate *by construction*, and the only reason
it was ever found is that the functions were **moved into a file a test can
compile**. That is the entire argument for the logic/IOKit split, restated: a
pure function written on the wrong side of it is a pure function nothing runs.

### What changed

**New:** `phase0-kext/IntelDisplayNames.hpp` — a dependency-free header holding
everything extracted from the shell:

- `igLinkVerdictName()`, `igModeVerdictName()`, `igUsableVerdictName()`,
  `igInheritVerdictName()` — the four verdict-name switches, with the missing
  `IG_MODE_SRC_MISMATCH` arm added.
- `igTransName()` — transcoder/pipe index to `A B C D DSI0 DSI1`, `"?"` outside.
- `IG_KEY_BUFFER_BYTES` and `igComposeKey()` / `igTranscoderKey()` /
  `igPlaneKey()` — IORegistry property-key composition.

It is the **only** dependency-free header in this tree with `#include`s, and the
reason is that it switches on constants defined elsewhere and has no forward
declarations available. Both are *quoted* includes of siblings
(`IntelDisplay.hpp`, `IntelFramebuffer.hpp`), which stay inside `phase0-kext/`,
so it still parses standalone under `-I phase0-kext`. That distinction is
load-bearing for `tools/check-header-coverage.py`, which derives the section-1
exemption from the presence of an **angle-bracket** include rather than from a
hand-kept list — so this header is expected to be parsed in section 1 of
`tests/check-kext-syntax.bat`, not exempted from it. The gate line was added.

**New:** `tests/test_intel_display_names.cpp`, 236 checks, and
`tests/CMakeLists.txt` gained the suite.

**Changed:** `phase0-kext/IntelDisplayProbe.cpp` lost the ~85-line static block
and gained a comment pointing at the new header and the suite, recording the
defect verbatim so the next reader does not have to reconstruct it.
`IntelDisplayProbe.hpp` includes the new header.

### The check that found it, and why it is written the way it is

```cpp
for (IGN32 v = 0; v <= IG_MODE_VERDICT_LIMIT; v++)
    /* assert igModeVerdictName(v) is NOT the unknown string */;
/* then assert IG_MODE_VERDICT_LIMIT + 1u IS the unknown string */
```

The loop bound is **the family's own `_VERDICT_LIMIT`**, not a count typed into
the test. That is the whole design: a family that grows a verdict reddens this
suite on the commit that grows it, with no second number to remember to update.
A test that said `for (v = 0; v <= 6; v++)` would have found today's bug and
then gone stale at the next one — which is the
`prose-carrying-a-derived-value-goes-stale-silently` failure with a `for` loop
around it.

The `LIMIT + 1u` assertion is the negative control, and it is not decoration.
Without it, a `igModeVerdictName()` rewritten to `return "ok";` unconditionally
passes the exhaustiveness loop for every family, forever.

Two further sections exist because exhaustiveness alone is weak:

- **Uniqueness** (`testNoDuplicateNames()`, O(n²) pairwise within each family).
  Exhaustiveness accepts two arms returning the *same* literal, which is the
  `a-connector-device-link-points-at-the-card` shape: every row prints something,
  and two rows print the same something.
- **Bare literals.** Every name is pinned to a spelled-out string in the test,
  never to the `#define` that produced it. An accessor checked against its own
  constant passes under any value — that defect has already cost this project
  fifteen undetectable offsets (`self-cancelling-constants-are-untestable`).

## §1b — `snprintf` was replaced, and the reason is truncation, not availability

`publishTranscoder()` and `publishPlane()` composed their twelve property keys
with `snprintf` into a hand-sized `char[]`. handoff19 §2 is about the shim
*declaring* `snprintf`; this is about what the call does when the buffer is
short, and it is a worse problem than an undeclared identifier.

`snprintf` truncates and reports it **only in a return value nobody was
reading**. A buffer one byte tight publishes `IGTranscoderDSI0-Inher` — a key
that looks deliberate, sorts immediately next to the real one in an IORegistry
dump, and is wrong. There is no failure signal anywhere.

`igComposeKey()` refuses instead, and on refusal writes an **empty** string
before returning false. So a caller that ignores the result publishes something
visibly broken rather than something plausible. Both call sites check it:

```cpp
char key[IG_KEY_BUFFER_BYTES];
if (igTranscoderKey(key, sizeof(key), trans, "-Link")) setProperty(key, igLinkVerdictName(link));
```

The second reason is testability, and it is the same reason as §1: a kernel-side
`snprintf` can be exercised nowhere but the operator's laptop, while
`igComposeKey()` is a pure function of three `const char*` and gets driven on
Windows. `IG_KEY_BUFFER_BYTES` is 32 against a longest real key of 23 bytes
(`IGTranscoderDSI0-Height`), and the suite pins both that length **and** the
refusal at 23 — an exact-fit pair, because a bound tested only from the slack
side is `containment-is-one-sided` again.

`tests/kextshim/libkern/libkern.h` now records that **no kext source calls
`snprintf` any more** and that nothing in `phase0-kext/` reaches that file. The
header stays anyway: deleting it would make the shim flatter than Apple's include
graph, which is precisely the hazard handoff19 §2 was written about.

## §1c — the read guards came out too, and the sentinel arm was undetectable

§5 item 2 named the `regRead32()` policy-range refusal as the last piece of shell
logic that is a pure predicate in disguise, and filed it as blocked on "deciding
what the dependency-free half is allowed to know about a mapping length". That
sentence was the only obstacle. The mapping is not an input to the decision — only
*whether a base pointer exists* is, and that is one `bool`.

`regRead32()` had three guards in front of its one dereference:

1. `!fBar0Base` — BAR0 never mapped.
2. `offset == IG_MMIO_INVALID` — an offset helper (`igTransTimingReg()`,
   `igPlaneReg()`) declined and returned its refusal sentinel.
3. `!igIsInDisplayPolicyRange(offset)` — a real offset outside this track's window.

**All three return `IG_MMIO_INVALID`.** So no test can separate them through the
return value. Worse, `IG_MMIO_INVALID` is `0xFFFFFFFF`, which is *itself* outside
the policy range — so guard 2 and guard 3 overlap on the one input guard 2 exists
for. Delete guard 2 entirely and `regRead32()` still refuses `0xFFFFFFFF`, still
returns `IG_MMIO_INVALID`, still declines to dereference. Byte-identical behaviour
through the return value. The **only** thing that changes is the log line, which
starts blaming the policy range for an offset helper's refusal — sending an
operator reading NVLOG after a bad constant in the offset tables instead of after
the declining helper that produced the sentinel.

That is a defect no gate in this tree could have caught, for the same structural
reason as §1: it lived in a translation unit no host suite can compile, and it is
invisible to the return value even if one could.

### What changed

`igReadDecision(bool barMapped, IGU32 offset)` is now in
`phase0-kext/IntelDisplay.hpp` beside `igIsInDisplayPolicyRange()`, returning a
**reason** rather than a bool:

```
IG_READ_OK               0u   safe to dereference
IG_READ_UNMAPPED         1u   BAR0 was never mapped
IG_READ_SENTINEL_OFFSET  2u   an offset helper declined and said so
IG_READ_OUT_OF_POLICY    3u   a real offset, outside this track's window
IG_READ_VERDICT_LIMIT    3u
```

It went into `IntelDisplay.hpp` rather than `IntelDisplayNames.hpp` deliberately:
`tools/check-registers.py` parses only `IntelDisplay.hpp` and
`IntelFramebuffer.hpp` on the Intel side, so a verdict family in the names header
would get no set-check at all. The gate gained
`intel_verdict_set_checks(rep, intel, "IG_READ_", 4, "igReadDecision()")` and reads
**239/239** where it read 238.

The prefix was grepped tree-wide before it was chosen and `IG_READ_` was free.
That is not ceremony: handoff16 §1 records a verdict family named `IG_PIPE_`
sweeping in `IG_PIPE_TRANSCONF`, a register offset that had sat in the header since
the first Intel session, and printing a contiguity failure that was correct about
its arithmetic and wrong about the world.

`regRead32()` now switches on the reason and keeps all three `IGLOG` lines
verbatim. Every arm still returns `IG_MMIO_INVALID`, unchanged — that value is
also what a dead aperture reads, and the two stay distinguished by the log line
rather than by the return value, deliberately, because a caller that cannot tell
them apart should not be silently choosing between them.

### The separating check

`tests/test_intel_display.cpp` gained `testReadDecision()`, **19 checks**, 354 →
**373**. The load-bearing line is one:

```cpp
checkU32("a sentinel offset is diagnosed as a sentinel, not as out-of-policy",
         igReadDecision(true, IG_MMIO_INVALID), 2u);
```

That is the only assertion in the tree that separates a live `igReadDecision()`
from one with the sentinel arm deleted. Everything else in the section survives
that mutation.

It is guarded by a vacuity check placed immediately above it:

```cpp
checkU32("IG_MMIO_INVALID is itself outside the policy range",
         igIsInDisplayPolicyRange(IG_MMIO_INVALID) ? 1u : 0u, 0u);
```

If that ever went green as `1`, the precedence assertions would be separating
conditions that cannot co-occur — i.e. vacuous while looking rigorous. It is the
`an-assertion-that-cannot-fail-is-not-coverage` guard written as a check rather
than as a comment.

The rest: verdicts pinned to bare literals (never to the `#define` that produced
them), `IG_READ_OK` pinned as the zero so `if (igReadDecision(...))` keeps reading
correctly, unmapped-beats-everything on three inputs where two guards are true at
once, both policy edges pinned **and** one step outside each
(`containment-is-one-sided` — widening a window produces no failure if every probe
stays inside it), and two end-to-end shapes: `igTransBase(IG_TRANSCODER_A)` and
`igPipeBase(IG_TRANSCODER_DSI1)` — the two ends of the span
`IG_DISPLAY_POLICY_LIMIT` was derived from — must read as OK, and
`igTransBase(IG_TRANSCODER_COUNT)` must be diagnosed as a sentinel. That last one
is the real call shape in the shell: a helper's refusal handed straight to the
read guard, rather than a literal typed by a test.

### The payload moved without moving

Two `phase0-kext/` files changed, so `build-payload/` was rebuilt — the
**twenty-third** image. Both sizes stayed exactly where they were (`kext-src.tar`
327680, `step7-payload.iso` 405504) while both md5s moved. That is
`a-byte-size-is-not-a-fingerprint` arriving for real rather than as a caution: tar
pads to a 10240-byte boundary, the content grew by well under one block, and a
card checked on size alone would have read as current while shipping the previous
source. `OPERATOR-ACTION-step7-build.md` carries the new md5s and says this in
place.

## §1d — a peer was editing `test_decode` underneath this session

Worth recording because the first reading looked like my own breakage.

The C++ gate came back `89% tests passed, 1 tests failed out of 9` with
`test_decode` red — a suite this session never touched. Its binary reported 93
checks against a table entry of 64, and its failures read
`got "unrecognised Ampere part", expected "unknown"`.

mtimes settled it in one call: `tests/test_decode.cpp` had been written **22
seconds** before I read the clock, after every edit of mine, while
`phase0-kext/NVDecode.hpp` had not moved since 2026-08-06. Then it moved again —
64 → 93 → 96 checks across three readings, with `NVDecode.hpp` written 71 seconds
before the last one. A peer was mid-edit on the NVIDIA track.

The Python gate then went red on `test_check_count_floor` for the same reason, and
the correct action was **to do nothing to it**: the peer's own `MEASURED` entry
landed a moment later and the module went green untouched. Editing a shared gate
table to accommodate a half-written peer suite is how a real regression gets
normalised into a table entry.

This is `a-mid-write-snapshot-reads-as-a-false-green` in its inverted form —
for gates it reads as a false **RED** — and the rule that resolved it is the same
one: check mtimes before believing a named FAIL in a tree with more than one agent
in it.

## §2 — `DSI0`/`DSI1` are named even though the scan stops at four

`igTransName()` returns names for six units while `IntelDisplayProbe.cpp` walks
only transcoders A–D (`igPlaneReg()` refuses pipes above `IG_TRANSCODER_D`, so a
DSI page has no plane registers to read). Naming all six is deliberate: the DSI
pages exist on this silicon, and if a future session widens the scan, a `"?"`
there would make the probe *misreport* rather than *fail*. A name that exists
before its caller costs nothing; a name that appears one commit late costs an
operator a debugging session.

## §3 — two gates went red on a prose edit, and the second reading was not reproducible

Worth recording because both readings were wrong in different ways.

My CLAUDE.md rewrite spelled an angle-bracket IOKit include path in prose.
`tools/check-documented-paths.py` resolves every path-shaped token in that
document against the checkout and cannot tell an Apple SDK header from a missing
repo file, so both the syntax gate and the Python gate went red naming
`CLAUDE.md:415`. That is the
`a-bundle-internal-path-reads-as-a-repo-directory` finding in a new costume:
**describe an out-of-tree path in words, never as a path.** Fixed by rewriting
the sentence, and the sentence now says why so the next author does not
reintroduce it.

The re-run then reported two *different* failures —
`CLAUDE.md:187: FAIL IntelDisplayProbe.cpp` and `CLAUDE.md:54: libkern/libkern.h`
— and neither reproduced. Running `tools/check-documented-paths.py` alone gave
exit 0 over 82 paths with zero FAILs, resolving those exact tokens as
`TAIL IntelDisplayProbe.cpp` and `ABSENT libkern/libkern.h`. A serial re-run of
both gates was clean.

The first of those two is a tell worth keeping: the token it complained about was
`FAIL IntelDisplayProbe.cpp`, which is not a path in CLAUDE.md at all — it is the
*output line of the gate itself*, quoted inside CLAUDE.md's handoff18 summary.
So the checker was tokenizing a quoted gate transcript. It resolves now via the
TAIL rule, which means the pass is a little lucky rather than principled. Filed
in §5 rather than fixed, because fixing a tokenizer while three other things are
in flight is how a green gate stops meaning anything.

The operational rule stands and it is the `selftest-and-pytest-gate-must-run-serially`
one: **a gate reading taken while another gate is running is not evidence.**
Run them serially, and re-run before believing a named FAIL.

## §4 — what this session does NOT establish

- **The kext has not been rebuilt.** `phase0-kext/` gained a header and changed
  two files; `make` last ran before that. handoff19 §8 item 2 is still open and
  is now larger, not smaller.
- **`IntelDisplayProbe.cpp` still has zero runtime coverage for everything that
  genuinely needs IOKit** — the BAR-length warning, and the
  `publishTranscoder()`/`publishPlane()` call shape wrapped around these
  now-tested pure functions. What moved out is what could move out. What is left
  is reachable only by parse, exactly as before.

  **PARTLY SUPERSEDED 2026-08-17, later the same day — see `handoff22.md` §1.**
  Naming the BAR-length warning as merely *untested* understated it: the warning
  was the only CONSEQUENCE a short BAR had. `regRead32()` bounded offsets against
  `IG_DISPLAY_POLICY_LIMIT`, which bounds what the decode headers can emit and
  says nothing about how many bytes were mapped, so a warned-about short BAR read
  past the end of its own mapping. `igReadDecision()` now takes the mapping
  length. The `IGLOG` line itself is still untested and always will be; what
  changed is that it is no longer the only thing that happens.

  This bullet used to name the `regRead32()` policy-range refusal as well. It was
  extracted later the same session — see §1c — so what remains inside
  `regRead32()` is the `IOMemoryMap` field read, the three `IGLOG` lines and the
  one dereference. The *decision* is out and tested; the *dereference* cannot be.
- **Nothing here was run on Intel hardware.** The 2026-08-16 capture in
  `igpu-results/` is still the only silicon this track has ever seen, and it is
  one boot of one eDP-only laptop. `igModeVerdictName(IG_MODE_SRC_MISMATCH)` now
  returns the right string; whether the verdict *fires* correctly on a real
  transposed-axis programming has never been observed.
- **236 checks is not 236 units of confidence.** A large share are exhaustiveness
  and uniqueness assertions over six-arm switches, which are cheap. The
  separating checks are the `LIMIT + 1u` controls, the exact-fit buffer pair, and
  the bare-literal pins.

## §5 — open, highest value first

1. **`make` has not run since `SOURCES` gained a second object, and now also has
   not run since `IntelDisplayNames.hpp` landed.** Carried from handoff19 §8
   item 2 and enlarged. The guest build is the only thing that says Apple's real
   KPI signatures match `tests/kextshim/`, and this session changed both an
   include list and a shim comment. `build-payload/` and
   `OPERATOR-ACTION-step7-build.md` are current with the live tree — the Python
   gate compares the tar byte-for-byte and is green — so the operator card is
   ready as written.
2. ~~**The `regRead32()` policy-range refusal is untested and untestable where it
   sits.**~~ **CLOSED later the same session — §1c.** The stated obstacle was
   wrong: I wrote that extracting it "means deciding what the dependency-free half
   is allowed to know about a mapping length", and it does not. The mapping is not
   an input to the decision at all — only *whether a base pointer exists* is, which
   is one `bool`. `igReadDecision(bool barMapped, IGU32 offset)` takes that bool,
   and the `IOMemoryMap` field stays in the shell where it belongs. The obstacle
   was a sentence I had written rather than a property of the code.
3. **`check-documented-paths.py` tokenizes quoted gate output as paths** (§3).
   Currently harmless because the TAIL rule catches it. A gate that passes for a
   reason other than the one it claims is a gate that will surprise somebody.
4. ~~**The Intel track's verdicts have never been exercised against a
   deliberately-wrong programming.**~~ **WITHDRAWN before this file was an hour
   old — it was false when written, and I filed it from memory rather than from
   the tree.** Measured 2026-08-17 by grepping the suites and reading the lines:
   the transposed-axis case is pinned at three independent sites, each with a
   separating input.

   - `tests/test_intel_display.cpp:993` — `igModeConsistent()` with PIPESRC's
     halves transposed returns `IG_MODE_SRC_MISMATCH`. Its raster is 1280x800
     specifically so the two axes differ; the comment above it records that
     anchoring on the captured 1920x1080 would make the suite's only transpose
     evidence depend on one unrepeatable boot.
   - `tests/test_intel_display.cpp:1293` — the same transpose through
     `igPlaneMatchesPipe()`, i.e. the plane-rectangle join rather than the
     whole-mode one.
   - `tests/test_intel_framebuffer.cpp:164-165` — `igFbSizeWidth(0x077F0437)`
     must come back **1080** and `igFbSizeHeight()` **1920**, feeding PIPESRC's
     byte order to the PLANE_SIZE decoder and pinning the wrong answer as the
     wrong answer. The observed pair is pinned beside it at `:161-162`.

   So the header comments that say "DO NOT simplify this by decoding pipeSrcWord
   with a PLANE_SIZE decoder" are backed by executing checks, not by prose alone.
   Left in this file rather than deleted, because the mistake is the reusable
   part: `handoff-docs-understate-test-coverage` is already a recorded finding in
   this project — handoff5 filed nine Phase 5 DMA tests as unwritten when all
   nine existed and passed — and I have now repeated it in the same shape. **Run
   the grep before filing the item, not after.** A believed-absent test costs a
   session; a believed-present one costs a defect.

   What genuinely remains unobserved is narrower and is already §4's last bullet:
   no *silicon* has ever been seen holding a transposed programming, so what is
   pinned is that the decoders and joins disagree correctly on synthetic inputs,
   not that a real mis-programmed pipe reaches that verdict on hardware. That is
   not a missing test — it is a missing laptop.
