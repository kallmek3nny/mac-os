# intel-handoff32.md

**Track:** Intel Gen12 (Tiger Lake) display
**Date:** 2026-08-20
**Continues:** `intel-handoff31.md`
**Shipping code changed:** NO -- `phase0-kext/` is byte-identical, so `build-payload/` was NOT rebuilt and the FORTY SECOND image is still current.

## 0 -- What this session did, and the one thing to read first

The operator asked to "make an IOFramebuffer" on the Intel side. **One already
exists** -- `IntelFbPresentProbe`, landed 2026-08-20 and hardened by
`intel-handoff31.md`: wired into `SOURCES`, into `Info.plist`, into
`tests/kextshim/IOKit/graphics/IOFramebuffer.h`, with eleven overrides, a
`presentable()` guard, and a shape gate over it. So the honest continuation was
not a second framebuffer beside the first -- it was `intel-handoff31.md` §9 item
1, which that session names as **its own hole** and as the cheapest remaining
work:

> derive P1's subject set from the pure-virtual set `IOFramebuffer` demands, not
> from a macro the author has to remember.

That item was TRUE as written, and §1 is the measurement rather than the
argument. Everything below is the evidence layer; not one byte of
`phase0-kext/` moved.

## 1 -- The defect, and why it is measured rather than reasoned about

`tests/test_intel_fb_present_shape.py` property 1 requires every *answering*
override to consult `presentable()` before it answers. Its subject set came from
`declared_overrides()`, which scans **the subclass's own header** for
`APPLE_KEXT_OVERRIDE` declarations and subtracts the four lifecycle methods.

Deriving rather than spelling was the right instinct and handoff31 says so. The
hole is the *direction* of the derivation: a set derived from the subclass
**ratifies whatever the subclass happens to declare**. Delete an override's
declaration from `IntelFbPresentProbe.hpp` and the name simply leaves the set --
so P1 stops asking about that method, and the gate reports **green over a method
it never considered**. An absence of coverage published as a positive result,
which is `intel-handoff28.md` §3a's self-narrowing gate one module over.

**§1a -- the measurement.** I did not assert this. I cut
`getPixelInformation`'s declaration out of the header, left its *definition* in
place, and ran both shapes over the identical mutated input:

```
NEW shape failures: 1
    P0: IOFramebuffer demands getPixelInformation() and
    phase0-kext/IntelFbPresentProbe.hpp declares no override for it
OLD subject set size: 10 -- getPixelInformation present: False
OLD shape failures: 0 []
```

Ten instead of eleven, and **zero failures**. The old gate did not fail to
detect a defect; it stopped having an opinion about the method entirely.

**§1b -- why no other gate covers it.** The four by-construction blindnesses
`intel-handoff26.md` §1 and `3050-handoff30.md` §1 each re-derive, plus one
specific to this case: the **C++ gate cannot see it either**, because
`test_intel_fb_present` drives `IntelFbPresent.hpp` (the pure header), never the
shell. And the syntax gate would only catch a deleted declaration if the
*definition* went with it -- `IntelFbPresentProbe.cpp` defining a method the
header does not declare is the error clang reports, so the mutation used here is
deliberately the half clang has nothing to say about... except it does. Which is
the next paragraph, and the honest limit on this whole fix.

**§1c -- the honest limit, stated up front.** Apple's real `IOFramebuffer` makes
these eight methods pure virtual, so a subclass that drops a declaration *and*
its definition fails to compile as an abstract class. This gate therefore covers
the case where the declaration goes and the definition stays (silent narrowing,
zero compile diagnostics off macOS) and the case where a **concrete** base
method's override is dropped. It is not a substitute for the real compiler, and
`tests/kextshim/IOKit/graphics/IOFramebuffer.h` says the same thing about itself
in its own file comment.

## 2 -- The fix, and the choice that is the opposite of the item's wording

`intel-handoff31.md` §9 item 1 says derive the set from the pure virtuals "not
from a macro the author has to remember." **Taken literally that would have made
the gate worse**, and this is the load-bearing finding of the session:

| set | size | contents |
|---|---|---|
| demanded (shim pure virtuals) | **8** | `getApertureRange` … `getCurrentDisplayMode` |
| declared (header, minus lifecycle) | **11** | those eight, plus three |

`setDisplayMode`, `enableController` and `isConsoleDevice` are **concrete** in
`IOFramebuffer` and overridden here, and every one of them answers a question
about the sample. Repointing P1 at the demanded set alone would have dropped all
three out of coverage -- trading a silent narrowing for a *different* silent
narrowing, three methods wide. That is the same defect arrived at from the other
side, and it is the third session running (`intel-handoff26.md` §2a,
`3050-handoff37.md` §3a item 2) where the first-draft reading would have been the
wrong one.

So the subject set is the **UNION**, and the union is made load-bearing by three
arms rather than left as a comment:

- **P0** -- every demanded pure virtual must be DECLARED by the subclass. This is
  the arm that reddens on the §1a mutation, and it names the method.
- **set EQUALITY** (`test_answering_is_the_union_of_both_sets`) -- containment
  alone is satisfied by the union, by the declared set, *and* by the demanded
  set, so it would ratify exactly the collapse it exists to forbid.
- **strict subset** (`test_demanded_is_a_strict_subset_of_declared`) --
  anti-degeneracy. If the two sets were equal in this tree, the equality arm
  above would hold under a P1 pointed at the demanded half alone, and nobody
  would notice until a non-pure override was added. `intel-handoff27.md` §3's
  reasoning, one module over.

Plus a fourth arm naming the three union-only methods explicitly, which asserts
both halves: each is NOT in the demanded set (so if the shim ever makes it pure,
this test is the wrong shape rather than the code) and IS in `answering`.

**§2a -- the parse, and an over-match I walked into.** My first throwaway probe
for the pure virtuals was one regex over the whole file with a lazy run before
`= 0`. It reported **seventeen** names where eight are pure, because the lazy run
crossed `OSDeclareDefaultStructors`, the comments and every intervening
declaration to reach the first `= 0` in the file -- which is an *enumerator*
(`kIOFBNoError = 0`), not a pure virtual. `intel-handoff26.md` §3's over-match
direction, and it cost one round trip. `demanded_pure_virtuals()` splits on the
statement terminator and decides pure-ness **per chunk**, so a declaration made
concrete in the shim LEAVES the set -- the direction that shrinks a subject set
silently, which is why the floor below refuses.

**§2b -- refusal, not fallback.** If every pure virtual in the shim were made
concrete, the demanded set collapses. The audit REFUSES rather than falling back
on the subclass's declarations, because a gate reporting nothing when its anchor
moves is indistinguishable from a green one (`3050-handoff30.md` §2b). There is
a control on it.

## 3 -- Non-vacuity

Two new negative controls, both asserting WHICH property broke:

1. `test_deleted_override_declaration_reddens_P0` -- the §1a mutation. Asserts
   P0 reddens **and names `getPixelInformation`**, and additionally that the
   method is still in `facts["answering"]`, because a P0 that reddened while the
   method fell out of P1 would be the same trade §2 refuses.
2. `test_base_header_with_no_pure_virtuals_refuses` -- every `) = 0;` rewritten
   to `);`. Asserts a REFUSED naming the collapsed set.

Both mutation anchors are checked for a match before use (`assertIsNotNone` /
`assertNotEqual`), because **a mutation that matches nothing prints ok**.

handoff18 §1e's every-pre-existing-check-green rule applies cleanly here, unlike
an all-new module: the twelve pre-existing checks stayed green across both
controls and through the union rewrite. **12 → 18 tests.**

## 4 -- Gate readings

| gate | reading | note |
|---|---|---|
| `tools/check-registers.py` | **242/242**, 0 failed, 0 skipped | unmoved and correctly -- no `#define` added. Was 240 at handoff28; the +2 is peer Intel work. |
| `tests\build-msvc.bat` | exit **0**, 12/12 suites | `test_intel_fb_present` among them, untouched by me |
| `tests\check-kext-syntax.bat` | exit **0** | read the exit code, not `VERDICT: PASS` -- section 11 is TITLED "each defect must FAIL" |
| `tests\run-python-tests.bat` | **1263 tests**, RED: 1 failure + 1 error | neither mine -- §4a |

**§4a -- both reds attributed, neither touched.** This is the rule
(`intel-handoff27.md` §5a): attribute by mtime and by the failure MESSAGE.

- `test_probe_firmware_state_publication.LiveTreeTests.test_live_tree_is_clean`
  ERRORed in the gate run and **passes 40/40 when run alone**. Its subject was
  written at **16:59:21**, during the gate run. A mid-write snapshot reads as
  finished-but-broken, and for a GATE it inverts to a false RED.
- `test_operator_step7_doc` -- the peer rebuilt `build-payload/` at **16:54**
  against a card (`OPERATOR-ACTION-step7-build.md`) last written at **16:03**.
  Their staleness, sixty minutes old, not mine. Rebuilding the payload or
  editing the card would vouch for a peer's half-landed work under this
  session's name.

My own module is **18 of 18 green** inside that same gate run -- verified by
grepping its 18 lines in the retained log and counting the non-`ok` ones (4,
all of which are docstring continuation lines, not verdicts).

**§4b -- two invocation traps, both hit.** `cmd /c tests\run-python-tests.bat`
from the project root printed a `cmd` banner with an empty prompt: the batch
**never started**. Invoke it directly from `tests/`. And its `tail` shows an HFS
catalog B-tree dump, because a tool test prints its own parse output -- grep for
`Ran N tests` / `OK` / `FAILED` and the carried exit status, never the last
lines.

## 5 -- What this does NOT establish

1. **The kext has still never been `kextload`ed.** Unchanged for nineteen
   handoffs and unchangeable from here: SIP down, a reboot, and a machine with
   the card.
2. `IntelFbPresentProbe` has **never executed a line** on any machine. This
   session added zero runtime coverage; it added a reason for a shape to stay
   correct.
3. §1c's limit: the real compiler catches the drop-both case on macOS. This gate
   covers the drop-declaration-only case and the concrete-base-override case,
   which are the two clang has nothing to say about off macOS.
4. P0 compares NAMES. An override declared with a mismatched signature is a
   compile error on macOS and invisible here.
5. The demanded set is parsed from **the shim**, not from Apple's header. If the
   shim is wrong about which methods are pure, this gate is wrong in the same
   direction -- and the shim's own file comment calls itself the riskiest file in
   `tests/kextshim/` for exactly that reason.

## 6 -- Open, highest value first

1. **Harvest configuration registers out of
   `igpu-results/tgl-final-capture-2026-08-18-201412.txt` into
   `phase0-kext/IntelDisplay.hpp`** -- inherited from `intel-handoff31.md` §9
   item 3 and now the largest remaining Intel item not blocked on hardware. Obey
   `intel-handoff28.md` §4d: a register read on a running display is a SAMPLE,
   not a constant, and only configuration registers may become `*_OBSERVED_*`.
2. Attribute the unexplained **+26** in the Python total by running the
   payload-freshness module alone -- inherited from `intel-handoff31.md` §9 item
   2, still not done, and it is a claim with an age rather than a fact.
3. The hole THIS session's fix leaves: `demanded_pure_virtuals()` decides
   pure-ness by a `= 0` tail on a `;`-split chunk, so a pure virtual written
   `= 0` across a line break inside a default argument would be missed. It would
   report a smaller set rather than fail -- the vacuity direction. The floor at
   5 bounds how far it can silently shrink, but does not pin 8.
4. No gate covers the **embedded Python** in `tools/igpu-final-capture.sh` --
   inherited from `intel-handoff27.md` §7 item 2, still open.
5. `tests/test_intel_scanout.cpp` still has no `MEASURED` entry (the peer's, left
   alone for the third session running).
