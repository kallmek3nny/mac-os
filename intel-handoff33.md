# intel-handoff33.md

**Track:** Intel Gen12 (Tiger Lake) display
**Date:** 2026-08-25
**Continues:** `intel-handoff32.md`
**Shipping code changed:** NO -- `phase0-kext/` is byte-identical, so `build-payload/`
was NOT rebuilt and the FORTY SECOND image is still current.

## 0 -- What the operator asked for, and why this is the Intel track

The request was an accelerated graphics driver, then redirected: *make one for
the Intel iGPU*. Two things have to be said before any of the work below reads
as progress, and both are measurements in this tree rather than opinions.

**Acceleration is not reachable and this session did not move toward it.**
Metal on macOS needs an `IOAccelerator` family plus a userspace driver plugin
against private, unversioned Apple interfaces. `PLAN.md` §0 calls that the wall
and caps the project at Phases 0-5. Nothing here is an accelerator.

**The Intel track is the only one that can reach the panel, and that is why the
redirect was the right call.** `PLAN.md` termination condition A is SATISFIED --
the laptop is muxless, confirmed from the operator's report and independently
from the GA107 VBIOS, whose DCB declares `entry_count = 0x00`. A flawless
NVIDIA driver puts zero pixels on that screen. Condition B is the matching
constraint on this side: macOS ships no Tiger Lake framebuffer, and
`display-match-report.txt` measures that `0x9A688086` is absent from all 203
device ids in every shipping personality.

So this track is a **display/framebuffer** track. `IntelFbPresentProbe` is its
IOFramebuffer and it already existed before this session.

## 1 -- The inherited item was already closed, and the mtimes say so

`intel-handoff32.md` §6 item 1 names register harvesting as *"the largest
remaining Intel item not blocked on hardware"*. It is **closed**, and it was
closed before that sentence was written:

| file | mtime |
|---|---|
| `phase0-kext/IntelDisplay.hpp` | 2026-08-19 19:59:15 |
| `tests/test_capture_vs_header.py` | 2026-08-19 20:46 |
| `intel-handoff32.md` | **2026-08-20 17:02:51** |

The header carries `IG_OBSERVED_*_2026_08_18` constants covering every offset on
the capture's own WHAT IS SAFE TO HARVEST list, and `test_capture_vs_header.py`
compares each one against
`igpu-results/tgl-final-capture-2026-08-18-201412.txt` -- the gate that stops a
hand transcription drifting a nibble. All five double-read status offsets
correctly have **no** constant, which is `intel-handoff28.md` §4d obeyed: a
register read on a running display is a SAMPLE.

`3050-handoff29.md` §1 for the third time -- **an item inherited across handoffs
is a claim with an age**, and "X is the largest remaining item" is a
set-difference somebody has to actually run. handoff32 was NOT edited to remove
the item; same rule as `intel-handoff28.md` §7 item 5.

## 2 -- The item that WAS open, and how its own filing was wrong

`intel-handoff32.md` §6 item 3 -- the hole that session's fix left behind:

> `demanded_pure_virtuals()` decides pure-ness by a `= 0` tail on a `;`-split
> chunk, so a pure virtual written `= 0` across a line break inside a default
> argument would be missed.

**That prediction is FALSE, and measuring it is the load-bearing half of this
session.** Python's `\s` already matches a newline, so `=\s*0\s*$` found every
wrapped spelling the old parser was accused of missing. I ran both parsers over
eight declaration forms:

```
case                       old      new      DIVERGE
B wrapped before eq        ['bbb']  ['bbb']
C split across eq          ['ccc']  ['ccc']
H eq-zero then newline     ['hhh']  ['hhh']
...
any divergence: False
```

A whitespace-collapsing "fix" for that item changes the output on **no input
whatsoever**. I wrote that fix first, complete with a docstring explaining the
line-break hole, and the measurement killed it -- so it was reverted rather than
shipped. A gate edit that alters behaviour on nothing is a comment claiming
coverage it does not add, which is this module's own subject one level up.

### 2a -- The real hole

Any **token standing after the `= 0`**:

```c
virtual IOReturn getPixelInformation(...) = 0 APPLE_KEXT_PURE;
virtual IOReturn getCurrentDisplayMode(...) = 0 __attribute__((z));
```

Apple spells pure virtuals with a trailing macro in several graphics headers,
and an attribute lands in the same place. Under a tail test the chunk no longer
*ends* in `= 0`, so the method **drops out of the demanded set** -- P0 stops
asking about it and the gate reports green over a method it never considered.
That is `intel-handoff32.md` §1a's defect exactly, arrived at from the other
side: it shrinks the subject set rather than failing, the vacuity direction, and
the floor at 5 is the only thing that would ever notice. The floor does not pin
8.

### 2b -- The fix, and the trap on the other side of it

Pure-ness is now decided on the suffix **after a balanced walk of the parameter
list**, not on the chunk's tail. The obvious cheaper fix -- search the chunk for
`= 0` anywhere -- reintroduces the defect from the opposite direction, because a
default argument carries its own `= 0` *inside* the parens:

```c
virtual IOReturn setAttribute(IOSelect a, uintptr_t v = 0);   // CONCRETE
```

A widened parser calls that pure, P0 then demands a declaration the subclass has
no reason to carry, and the gate reddens correct shipping code --
`intel-handoff26.md` §2a's trap. Walking the parameter list buys both halves at
once, and both halves are asserted.

Verified across twelve forms. The new parser is correct on all twelve and
diverges from the old one on **exactly the two** that were the real hole:

```
G pure + trailing macro   expect True   old False   new True   DIVERGE
I pure + attribute        expect True   old False   new True   DIVERGE
divergences old->new: 2
```

Live tree unchanged at **8** demanded, same eight names -- so the fix is not a
re-measurement of the tree, it is reach the tree does not currently exercise.

## 3 -- Non-vacuity

Two new controls, both asserting the SET rather than a membership:

1. `test_pure_virtual_with_a_trailing_macro_stays_demanded` -- rewrites every
   `) = 0;` in the shim to `) = 0 APPLE_KEXT_PURE;` and requires the demanded set
   to be **equal**, not merely non-empty (a parser answering pure for everything
   would also keep the method). It then requires the whole audit to stay GREEN,
   because a set that survived the parse and failed to reach P0 would satisfy the
   equality and still leave the gate blind. The anchor is checked with
   `assertNotEqual(n, 0)` -- **a mutation that matches nothing prints ok**.
2. `test_default_argument_zero_is_not_a_pure_virtual` -- the anti-degeneracy arm
   from §2b, asserting both directions: a concrete method with a defaulted last
   parameter is NOT demanded, and a pure virtual with one still IS.

handoff18 §1e's every-pre-existing-check-green rule applies cleanly here: the
eighteen pre-existing checks stayed green across both controls and through the
parser rewrite. **18 → 20 tests.**

## 4 -- Gate readings

All four run after the last edit. Exit codes captured **before any pipe**.

| gate | reading | note |
|---|---|---|
| `tools/check-registers.py` | **242/242**, 0 failed, 0 skipped, exit 0 | unmoved and correctly -- a test-module edit carries no `#define` |
| `tests\build-msvc.bat` | exit **0**, **12/12** suites, 0 failed | `test_intel_fb_present` among them, untouched by me |
| `tests\check-kext-syntax.bat` | exit **0**, zero `FAIL` lines | read the exit code, not the verdict line -- section 11 is TITLED "each defect must FAIL" |
| `tests\run-python-tests.bat` | **1279 tests**, exit 1, RED: 1 failure | not mine -- §4a |

Python 1263 → **1279**. The +16 is +2 mine and +14 not attributed by me; I ran
my own module alone (20) before the gate, which is the only reading that
separates them.

### 4a -- The one red, attributed and deliberately not touched

`test_operator_step7_doc.DerivedValueTests.test_every_md5_the_card_quotes_can_be_accounted_for`
-- *the live ISO's md5 must be among the md5s the card prints*.

Attributed by md5 rather than by memory:

```
live   build-payload/step7-payload.iso   23ec30fb847be52ed25373a7c08bc6f4
card   OPERATOR-ACTION-step7-build.md    68 md5s, none of them that one
```

And by mtime: `build-payload/` was rebuilt at **16:54**, the card last written at
**16:03** -- fifty-one minutes stale, on 2026-08-20, five days before this
session. This is `intel-handoff32.md` §4a's red unchanged, and
`intel-handoff27.md` §5a is the rule: rebuilding the payload or editing the card
would vouch for a peer's half-landed work under this session's name.

Note for whoever closes it: **a byte size is not a fingerprint** -- tar pads to
10240, so compare the md5.

### 4b -- The invocation traps, both avoided

`run-python-tests.bat` was invoked directly from `tests/`, not through
`cmd /c` from the root -- the tell for that failure is a 137-byte log holding a
cmd banner and an empty prompt, so the artifact SIZE is read before the status
(220167 bytes here). Verdicts came from `grep -E "^(Ran |OK|FAILED)"`, never
from `tail`, because a tool test prints an HFS catalog B-tree dump of its own.

## 5 -- What this does NOT establish

1. **The kext has still never been `kextload`ed.** Twenty handoffs, and
   unchangeable from here: SIP down, a reboot, and a machine with the card.
2. `IntelFbPresentProbe` has **never executed a line** on any machine. This
   session added zero runtime coverage; it widened the reach of a shape gate.
3. Nothing here is acceleration, and nothing here moves toward it. §0.
4. P0 still compares **names**. An override declared with a mismatched signature
   is a compile error on macOS and invisible here.
5. The demanded set is parsed from **the shim**, not Apple's header. If the shim
   is wrong about which methods are pure, this gate is wrong in the same
   direction -- and `tests/kextshim/IOKit/graphics/IOFramebuffer.h` calls itself
   the riskiest file in the shim for exactly that reason.
6. §2's divergence table is a measurement of **two parsers**, not of Apple. That
   Apple spells some pure virtuals with a trailing macro is why the form matters;
   this tree's shim does not currently use it, which is precisely why the old
   parser passed while being wrong.

## 6 -- Open, highest value first

1. **Rebuild `build-payload/` and update `OPERATOR-ACTION-step7-build.md`** --
   §4a's red, owed by the peer's 2026-08-20 work and five days old now. It is
   the only red in the tree and the cheapest thing on this list.
2. Attribute the **+14** in the Python total that is not mine, by running the
   payload-freshness module alone -- inherited from `intel-handoff31.md` §9 item
   2 through handoff32 §6 item 2, still not done, and still a claim with an age.
3. No gate covers the **embedded Python** in `tools/igpu-final-capture.sh` --
   inherited from `intel-handoff27.md` §7 item 2, still open.
4. `tests/test_intel_scanout.cpp` still has no `MEASURED` entry (the peer's, left
   alone for the fourth session running).
5. `CLAUDE.md`'s read-order list stops at `intel-handoff28.md` / `3050-handoff37.md`
   while the tree reaches `intel-handoff33.md` and `3050-handoff44.md`. That
   staleness cost this session a wrong first read -- `3050-handoff43.md` §7 item 4
   filed the same cost. Not fixed here for that item's own reason: the file is
   shared with the concurrent NVIDIA agent.
