# intel-handoff29 — the MMIO path gets a structural observer

Continues `intel-handoff28.md`. Intel track only; the NVIDIA track was worked
concurrently by a peer and every NVIDIA number in this file is copied rather
than re-measured.

`phase0-kext/` is **BYTE-IDENTICAL**. Nothing in the kext was edited, so
`build-payload/` was NOT rebuilt and the current image is still the peer's
latest. The proof of that is the Python gate's payload comparison being green
rather than an assertion — that gate reads `build-payload/kext-src.tar`
byte-for-byte against the tree, so a stale payload cannot pass silently.

One thing landed: `tests/test_intel_probe_mmio_path.py`, **17 tests**, closing
item 2 of the operator's five-item list.

---

## 0. Why this gate exists at all

`phase0-kext/IntelDisplayProbe.cpp` pulls in `IOService`, which does not exist
off macOS, so **no host C++ suite can execute a line of it**. That is the
split `CLAUDE.md` describes and it is deliberate — but it means the shell's
invariants have exactly one available observer: parse the source, assert the
properties, and prove the assertions can fail by mutating the source **in
memory**. The live tree is never written to by a test.

The subject invariant is the one `IntelDisplayProbe.hpp:21` already states in
prose: every MMIO access goes through `regRead32()`. Prose is not a gate. As
of this session it is structural.

---

## 1. The one to read: the gate derives the DEREFERENCE set, not the ACCESSOR set

`intel-handoff26.md` §6 item 4 names the hole every sibling module in this
family still has. `tests/test_intel_publisher_sample_shape.py` and
`tests/test_probe_register_access_refusals.py` both spell their subject against
an accessor **by name** — `regRead32(`, `regWrite32(`. A second MMIO accessor
added later is therefore not considered part of the subject at all, and the
gate **reports nothing rather than failing**. That is the vacuity direction,
and it is the worse of the two: a gate that goes quiet when its subject grows
is indistinguishable from a gate that is passing.

This module does not spell the accessor as its subject set. It derives the set
of **dereferences through a volatile-qualified pointer** and then asks where
they live. A second accessor cannot hide from that, because an accessor that
does not dereference is not an accessor.

MEASURED 2026-08-19 against the live tree: **exactly one** such dereference in
the whole file, at `:289`, inside `IntelDisplayProbe::regRead32`.
`OSReadLittleInt32` / `OSWriteLittleInt32` return **zero** hits, so the second
accessor is latent rather than live — which is precisely why the gate had to be
built against the structural property while the count is still one.

### 1a. A cast is not a dereference, and getting that wrong reddens a pristine tree

`volatile` occurs **twice** in the subject, not once:

    :179   fBar0Base = (volatile UInt8 *) fBar0Map->getVirtualAddress();
    :289   return *(volatile UInt32 *) (fBar0Base + offset);

The first is a cast whose result is **stored**. The second is a cast whose
result is **loaded through**. Only the second touches hardware. They are told
apart by the `*` immediately preceding the cast's open paren, so `DEREF_RE`
requires it. A pattern matching the cast alone counts two on correct source,
and property 1 would have reddened on a pristine tree — the failure mode this
tree has hit twice before by asserting on remembered code rather than code just
read.

---

## 2. The load is dominated by a FALL-THROUGH, not by an `IG_READ_OK` arm

This is the shape fact that had to be measured before anything could be
written, and the obvious guess is wrong. The switch is:

    switch (igReadDecision(fBar0Base != nullptr, fBar0Length, offset)) {
    case IG_READ_UNMAPPED:          ... return IG_MMIO_INVALID;
    case IG_READ_SENTINEL_OFFSET:   ... return IG_MMIO_INVALID;
    case IG_READ_OUT_OF_POLICY:     ... return IG_MMIO_INVALID;
    case IG_READ_PAST_MAPPING:      ... return IG_MMIO_INVALID;
    default: break;
    }

    return *(volatile UInt32 *) (fBar0Base + offset);

There is **no `case IG_READ_OK:`**. OK is the fall-through: `default: break;`
drops out of the switch and the load sits below its closing brace. So the
domination claim cannot be "the deref is inside the OK arm" — that arm does not
exist, and a gate searching for it would fail on correct code. The claim is an
**offset relation** (the deref begins at or after the switch body's closing
brace) plus a **refusal-per-arm** check (every named arm contains a `return`
and `IG_MMIO_INVALID`). Together those say the load is reachable only when no
refusal fired.

The switch body is found by a balanced-**paren** walk over the controlling
expression followed by a balanced-**brace** walk over the body. Both are
necessary: `igReadDecision()` takes three arguments and one of them contains a
comparison, and the arms contain braces inside `IGLOG` argument lists.
`handoff25.md` §1c is the cost of the alternative — a character class excluding
only the statement terminator is not scoped to the statement.

Property 3 asserts `IG_READ_OK` is **absent** from the arm set, and the failure
message says why: an explicit OK arm is not wrong in itself, but it invalidates
the domination proof above and that proof must then be re-derived. A gate whose
reasoning silently stops applying is the thing this whole family exists to
prevent.

---

## 3. `IG_READ_VERDICT_LIMIT` is INCLUSIVE, and the exhaustiveness loop reads it

`handoff20.md` §1 is the reason property 3 exists: `igModeVerdictName()` had
arms for verdicts 0 through 5 while `IG_MODE_VERDICT_LIMIT` is 6,
`IG_MODE_SRC_MISMATCH` published the unknown string into the IORegistry, and
all four gates were green over it. So the exhaustiveness loop must bound on the
family's own limit constant rather than a hand-counted total.

MEASURED 2026-08-19 in `phase0-kext/IntelDisplay.hpp`:

    #define IG_READ_OK                      0u
    #define IG_READ_UNMAPPED                1u
    #define IG_READ_SENTINEL_OFFSET         2u
    #define IG_READ_OUT_OF_POLICY           3u
    #define IG_READ_PAST_MAPPING            4u
    #define IG_READ_VERDICT_LIMIT           4u   // the largest verdict above

The limit is **4u and inclusive** — the same numeric value as the largest
verdict, and the comment beside it says so. This is the OPPOSITE convention to
`IG_MODE_VERDICT_LIMIT`, which is exclusive. A loop written `range(0, limit)`
from habit would skip `IG_READ_PAST_MAPPING` entirely and pass while blind to
the newest verdict in the family; a loop written `range(0, limit + 1)` against
the mode family's convention would demand an arm for a verdict that does not
exist. Two families, two conventions, one header. The loop here is
`range(0, limit + 1)` and the limit is parsed out of the header at run time.

That parse is what makes the forward control possible: one negative control
mutates the **header** rather than the subject, adding a sixth verdict and
bumping the limit to `5u`, and requires the gate to name `IG_READ_TORN_READ`
as unhandled. So a verdict added to the family with no arm in the switch
reddens on the commit that adds it, **without anybody editing this module**.

---

## 4. What the 17 tests pin

Five properties, each with at least one negative control naming which property
broke. `handoff18.md` §1e's every-pre-existing-check-green rule does NOT apply
here — this is an all-new module, so there are no pre-existing checks to hold
green, and the controls carry the whole non-vacuity argument instead.

| # | Property | Controls |
|---|---|---|
| 1 | Exactly one volatile dereference, and it is inside `regRead32` | a second deref planted in a new method; a renamed accessor must REFUSE |
| 2 | The deref is below the decision switch and every arm refuses | the load hoisted above the switch; an arm that `break`s instead of returning |
| 3 | Every verdict below the inclusive limit has an arm | a deleted arm named by verdict; a sixth verdict added to the header |
| 4 | The deref is the operand of a `return`, and there are no `OSWrite*` calls | the load rewritten as a store |
| 5 | `fBar0Base` / `fBar0Length` assigned only in `init`, `mapBar0`, `unmapBar0`, and zeroed in two of them | a stray writer of the length; `unmapBar0` stopping zeroing |

Property 4 is the file's own header claim made structural. `IntelDisplayProbe.cpp`
opens by saying it reads registers and writes none; a dereference that is the
operand of a `return` cannot be the target of an assignment, and zero `OSWrite*`
calls closes the other road. That is the claim, rather than prose asserting it.

Property 5 exists because of `handoff22.md` §1: `fBar0Length` was added when a
warned-about short BAR was still read `0x20000` bytes past the end of its own
mapping. The bound is only load-bearing if nothing outside `mapBar0()` can set
it and both zeroing paths genuinely zero it — otherwise a stale bound is
trusted after unmap. Measured: six assignments, three methods, both zeroing
paths intact.

### 4a. Refusal rather than silence

If the subject file, the accessor, or the switch cannot be found, `audit()`
returns a **REFUSAL string** and the tests assert on it. A gate that reports
nothing when its anchor moves reads exactly like a green one — the same
reasoning as `tests/test_capture_integrity.py` refusing on an empty subject set
rather than skipping.

### 4b. Comment and string stripping is mandatory here, not stylistic

`regRead32()`'s own comment block discusses `IG_MMIO_INVALID`, the guard order
and the dereference **by name**, and `IntelDisplay.hpp`'s prose quotes the cast.
A prose-reading gate would pass on a function whose guards were all deleted.
The stripper blanks comments and string literals while **preserving byte
offsets** — each removed character becomes a space, newlines are kept — so every
index into the stripped text is also an index into the original and a match can
be reported against a real line number. One test asserts the stripper actually
removed a known comment phrase; without it the stripper could no-op and every
other property would silently start reading documentation.

---

## 5. One defect of mine, in the message rather than the property

First run went red on `test_the_dereference_is_dominated_by_the_decision_switch`
with `not enough arguments for format string`. The failure was in the assertion
MESSAGE, not the assertion: `self.found["switch"]` holds a `(start, end)` tuple
on the healthy path, and a bare tuple passed to `%` is unpacked as an argument
list rather than formatted as one value. Fixed by wrapping it — `% (value,)`.

Worth recording because of the direction it failed in. The property was
CORRECT and the tree was CORRECT; what broke was the code that runs only when
the property fails. A formatting bug on a failure path is invisible until the
day the gate is supposed to be useful, and on that day it replaces a diagnosis
with a `TypeError`. Every negative control in this module exercises its own
failure message for exactly that reason.

---

## 6. Gate readings, 2026-08-19

| Gate | Reading | Note |
|---|---|---|
| Python | **exit 0, 997 tests, OK** | 980 → 997, exactly +17 |
| C++ | not re-run | no C++ source touched |
| Syntax | not re-run | no kext source touched |
| Registers | not re-run | no `#define` added |

The +17 is worth one sentence. Running the new module alone printed 17, and the
full gate moved by exactly 17, so the delta is entirely attributable to this
session and no peer edit is mixed into the reading. In a two-agent tree a gate
verdict is a JOINT measurement — a matching delta is what turns "the gate is
green" into "the gate is green **because of what I did**".

The register gate is correctly unmoved: this session added no constant. The
payload was correctly not rebuilt: `phase0-kext/` is byte-identical, and the
rule that decides a rebuild is CONTENT.

---

## 7. What this does NOT establish

1. **`IntelDisplayProbe` has still never run.** Not one line of it has
   executed, here or anywhere. This gate reads the source; it does not
   evaluate it. Every property is a claim about the text.
2. **The guards' correctness is still `IntelDisplay.hpp`'s to prove**, not
   this module's. `igReadDecision()` is covered by `tests/test_intel_display.cpp`.
   This gate only pins that the shell CONSULTS it and dereferences nowhere else.
3. **Property 1 is spelled against a volatile-qualified pointer load.** An MMIO
   access made some other way — inline assembly, a helper in another
   translation unit handed the base pointer — is not in the derived set. That
   is a smaller hole than the accessor-by-name one it replaces, but it is the
   same shape and it fails in the same silent direction.
4. **Property 5 does not prove ordering**, only ownership. `mapBar0()`
   assigning the length before validating it would satisfy every check here.

---

## 8. Open list

1. **Item 4 — scaffold active framebuffer output.** The highest-value item and
   the only one that serves the operator's actual goal, which is a picture on
   the panel rather than more evidence about it. An `IOFramebuffer` /
   `IOGraphicsDevice` scanout handoff inheriting the mode UEFI already
   programmed. Everything on this track so far reads; nothing presents.
2. **Harvest the remaining configuration registers** from
   `igpu-results/tgl-final-capture-2026-08-18-201412.txt` into
   `phase0-kext/IntelDisplay.hpp`. Partially done — 86 → 120 defines this
   round of work — and `intel-handoff28.md` §4d is the rule that bounds it: a
   register read on a running display is a SAMPLE, not a constant, so only
   configuration registers may become `*_OBSERVED_*`. Live status bits and the
   watermark band are excluded on evidence, not on taste.
3. **The §7 item 3 hole**, and it is this session's own. A second MMIO path
   that does not go through a volatile pointer load is invisible to property 1
   and reports nothing. Closing it properly means deriving from the base
   pointer's use rather than from the cast's spelling.
4. **Nothing on this track is blocked on hardware any more.** The Linux phase
   is closed (`intel-handoff28.md` §0) and the two captures in `igpu-results/`
   are all the silicon evidence this track will ever have. Every remaining
   question is answered out of those two files or it is not answered.
