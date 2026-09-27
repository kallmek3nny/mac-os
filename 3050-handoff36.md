# 3050-handoff36 — a bool that called this driver's own bug a hardware fault

NVIDIA track, 2026-08-19, continuing from `3050-handoff35.md`. It closes that
file's §6 item 3 — the item named there as the remaining log-only diagnosis worth
auditing, and the fourth occurrence of the same class in four sessions.

`phase0-kext/NVFalcon.hpp` and `phase0-kext/NV3050Probe.cpp` both CHANGED, so
`build-payload/` was rebuilt — the **THIRTY FIFTH** image — and
`OPERATOR-ACTION-step7-build.md` updated.

---

## §1 — the subject, and why it is worse than the three before it

`inspectGspEngine()` read the two RISC-V error-status registers and did this with
them:

```c
if (nvRiscvHasError(privErr, hubErr))
    NVLOG("GSP: ERROR LATCHES SET -- PRIV_ERR_STAT = 0x%08x  HUB_ERR_STAT = 0x%08x",
          (unsigned)privErr, (unsigned)hubErr);
```

and `nvRiscvHasError()` was two lines:

```c
return (privErrStat != 0) || (hubErrStat != 0);
```

The two words DID reach the IORegistry — `publishGspState()` has always published
`GSP_PRIV_ERR_STAT` and `GSP_HUB_ERR_STAT`. So this is not handoff31/33/35's
class exactly. It is that class one level down: **the WORDS were published and the
DECODE of them was not**, and the decode is where the whole finding lives.

### §1a — the four situations the bool collapsed

`nvRiscvHasError()` answered one question over four structurally different
states:

| State | Old answer | What it calls for |
|---|---|---|
| both latches clear | `false` | proceed |
| `PRIV_ERR_STAT` set | `true` | reset the engine |
| `HUB_ERR_STAT` set | `true` | reset the engine |
| **neither register decoded** | **`true`** | **fix this driver's BAR0 mapping** |

The fourth row is the defect. `regRead32()` reports every refusal it has by
returning all-ones, and the RISC-V block sits exactly one 4 KiB page above the
Falcon block (`NV_PGSP_BASE`..`NV_PGSP_LIMIT`, the reason
`nvGspFalconReg()`/`nvGspRiscvReg()` are two functions rather than one). A BAR0
mapping that covers the Falcon block and stops before the RISC-V block makes both
latches read `0xFFFFFFFF`, `(0xFFFFFFFF != 0)` is true, and the driver reported a
bug in ITSELF as a latched hardware fault.

Those two call for **opposite actions**, and only one of them is something
somebody can go and fix. That is `handoff22.md` §1b's ordering rule — the driver's
own bug outranks the fact about the machine — arriving in a subsystem that had no
verdict to order at all.

And the collapse was load-bearing beyond the log line: the go/no-go join read
`&& !nvRiscvHasError(privErr, hubErr)`, so an unmappable engine was reported as
failing its fault precondition rather than as unmeasurable.

### §1b — the second defect, found by set-difference

The error block in `NVFalcon.hpp` is five registers (`:299`–`:303`):

| Register | Occurrences in the tree, before this session |
|---|---|
| `NV_PRISCV_RISCV_PRIV_ERR_STAT` | read, published |
| `NV_PRISCV_RISCV_HUB_ERR_STAT` | read, published |
| `NV_PRISCV_RISCV_PRIV_ERR_INFO` | **1 — its own `#define` line** |
| `NV_PRISCV_RISCV_PRIV_ERR_ADDR` | **1 — its own `#define` line** |
| `NV_PRISCV_RISCV_PRIV_ERR_ADDR_HI` | **1 — its own `#define` line** |

Three of the five were read by nothing at all. So every fault this driver could
ever report was *"something was refused"* with the actionable half — **which
access, at what address** — simply absent. `handoff18.md` §1's method (enumerate a
header's `#define`s against occurrences in consumers; a register occurring only at
its own definition line is read by nothing) for the third time, and this is the
first time it found two defects in one block.

### §1c — why no gate could see either half

The four by-construction blindnesses, and the fourth entry is the sharp one:

1. `tools/check-registers.py` parses `#define`s. All five registers were already
   defined and CORRECT — the defect was that nothing read three of them. The
   register gate is correctly **unmoved at 240**.
2. No C++ suite can link a translation unit reaching `IOService`, so
   `inspectGspEngine()` is unreachable from `tests/*.cpp` by construction.
3. `-Werror` has nothing to say about a `#define` nobody references.
4. **`tests/test_falcon.cpp` asserted `nvRiscvHasError()` on four inputs and was
   green through every session.** All four are satisfied IDENTICALLY by the
   broken bool and by the verdict wrapper that replaced it — `== true` has three
   preimages here, `== false` has one. That is `3050-handoff34.md` §1's lesson
   arriving in the same file it was written about, one register block over: a
   bool assertion cannot tell a fix from the bug it fixed.

---

## §2 — the fix, and its five load-bearing choices

`NVRISCV_ERR_*` in `NVFalcon.hpp` is a five-value verdict
(`NONE`/`PRIV`/`HUB`/`BOTH`/`UNREADABLE` plus `_VERDICT_LIMIT`), mirroring the
WPR2 idiom at `:731`–`:785` in the same file.

**UNREADABLE takes precedence**, tested before the three fault arms. The two
mixed cases pin it: a genuine hub latch beside a refused priv read must NOT
report *"hub error latched"*, because the half that did not decode is itself the
finding. §3's mutation is exactly this ordering reversed.

**The sentinel is spelled for THIS family**, `NVRISCV_ERR_READ_REFUSED_VALUE`,
rather than borrowed from `NVWPR2_READ_REFUSED_VALUE` five hundred lines up. Both
land on all-ones by different roads; a sentinel read out of a sibling family is
`handoff21.md` §1b's mistake, and a check asserts the two agree so a divergence
reddens rather than passing silently.

**`nvRiscvErrReadRefused(0u)` is deliberately FALSE**, which is the OPPOSITE
polarity to WPR2. Zero in `ADDR_HI` is the healthy DOWN case there; zero in an
error-status register is the healthy CLEAR case here. One sentinel value, two
families, arrived at from opposite directions — and refusal is an **equality**,
never a threshold, because every bit of a status register is a distinct latch and
there is no "too large to be real" reading to threshold against.

**`nvRiscvHasError()` is KEPT** as a one-line wrapper, and it is a **NEGATION of
the single OK arm** (`!= NVRISCV_ERR_NONE`) rather than a disjunction over the
three fault arms. A disjunction is FALSE for a sixth verdict added later, so it
would report an unmeasured latch as "no fault" and send an unmeasurable
precondition through the go/no-go as satisfied — handoff32 §3's `!= ERR_HEADER`,
handoff33 §2's `NVPartIsTarget` and handoff35 §2's `NVGspStageOk` for the fourth
time. The negation also preserves the old bool's behaviour on all-ones exactly,
so no existing caller or assertion changed. Keeping it is `3050-handoff34.md`
§2's PAIR: the wrapper is what the go/no-go actually calls, so a verdict-only
suite would leave it free to stop consulting the verdict with every check green.

**The verdict is derived ONCE** in `inspectGspEngine()` and used by the log
block, the publisher and the go/no-go join. Deriving it three times is how a
predicate ends up disagreeing with itself.

`publishGspErrorLatches()` publishes eight keys. `GSPErrorVerdict`,
`GSPErrorLatchesReadable`, `GSP_PRIV_ERR_STAT` and `GSP_HUB_ERR_STAT` go on BOTH
arms — the first two because an absent key is a question rather than an answer
(handoff33 §1a), the two status words because on the UNREADABLE arm their
all-ones value IS the evidence and the verdict beside them says how to read it.
The three localization words go ONLY behind the readable arm: nothing on the
UNREADABLE path measured them, and a refusal value published as a word is
byte-identical to a measured one (handoff18 §1d).

---

## §3 — the non-vacuity proof

The discriminating mutation is the precedence reversed — the refusal test moved
BELOW the three fault tests, which is precisely the pre-fix behaviour expressed
in the new vocabulary. It reddened exactly **5 checks, all five new, every
pre-existing check green**, first try:

```
FAIL  priv refused -> UNREADABLE                         got 0x1, expected 0x4
FAIL  hub refused -> UNREADABLE                          got 0x2, expected 0x4
FAIL  priv refused beside a real hub latch -> UNREADABLE  got 0x3, expected 0x4
FAIL  hub refused beside a real priv latch -> UNREADABLE  got 0x3, expected 0x4
FAIL  both refused -> UNREADABLE                         got 0x3, expected 0x4
234/239 passed, 5 failed
```

That is `handoff18.md` §1e's signal: a mutation reddening MANY checks is usually
evidence the OLD tests sufficed, and only one leaving every pre-existing check
green proves added reach. The header was mutated and reverted, and the rebuild
after the revert was run and read rather than assumed —
`a-noop-mutation-reads-as-a-vacuous-test` is what skipping that costs.

Note what the four pre-existing `nvRiscvHasError()` checks did under this
mutation: they stayed **green**. They cannot see a precedence error, because the
wrapper's answer is `true` either way. Those four checks are the reason this bug
lived, and they are kept in `testEngineReset()` beside the new section rather
than replaced.

---

## §4 — the shape gate, and three defects it found in itself

`tests/test_probe_gsp_error_latch_publication.py`, **14 tests**, in the idiom of
the five sibling probe publication modules: one `audit()` shared between the live
tree and every mutant, comment-stripped source, one positive control, one
stripper test, and twelve negative controls each asserting WHICH property broke.

Nine properties. Property 8 is the one no publication check could supply on its
own: it asserts the CONSUMER reads all three localization registers, because a
publisher can be perfectly shaped around values nobody ever read.

The positive control went red three times on correct shipping code, and all three
are worth carrying.

### §4a — `body_of()` returns the inside of the braces

Property 9 required the publisher call at brace depth **1**, copied from
handoff35's own module. It is depth **0**: `body_of()` returns the text INSIDE
the function's outer braces, so an unconditional statement in that body has no
enclosing brace within the span being counted. The positive control said
`depth 0, expected 1` immediately, which is the good failure — but it is the
second session running where a predicate about nesting was off by one level, and
the first (handoff35 §3a) shipped green because another property caught its
control for an unrelated reason.

### §4b — a bare substring test is satisfied by any name it is a PREFIX of

Two controls failed with an EMPTY failure list, which is the vacuity direction.

- Property 1 asked `if PUBLISHER not in header`. The control renames the
  declaration to `publishGspErrorLatchesXX` — which **contains**
  `publishGspErrorLatches`, so the check passed on a header declaring nothing
  this file defines.
- Property 2 asked `if arm not in falcon`. Same shape:
  `NVRISCV_ERR_UNREADABLE_RENAMED` contains `NVRISCV_ERR_UNREADABLE`.

Both are now matched structurally — a call-shaped token for the declaration, and
`#define\s+NAME\b` for each arm, where `\b` refuses the suffixed spelling because
underscore is a word character. **This is the mirror of the closing-quote rule
this module already applied to its key anchors** (`GSP_PRIV_ERR_ADDR` is a strict
prefix of `GSP_PRIV_ERR_ADDR_HI`) — the same reasoning, missed one field over
because the anchors there were quoted strings and these were identifiers.

### §4c — `"return" in arm` is satisfied by the arm's own prose

Property 6 checked the UNREADABLE arm returns, spelled `if "return" not in arm`.
The control deletes the `return;` and it stayed green — because the reason string
inside that very arm says *"what regRead32() **return**s for every refusal it
has"*, and the string survives comment stripping by design. Now
`re.search(r"\breturn\s*;", arm)`: a STATEMENT, not the identifier. Same class as
`a-token-presence-check-is-not-a-return-check`, in a new place — there the token
survived in a comparison three lines above, here it survives in a string literal
inside the mutated block itself.

### §4d — one anchor matched twice

`} else if (errVerdict != NVRISCV_ERR_NONE) {` occurs TWICE in
`NV3050Probe.cpp` — once in the three-arm log block, once in the not-bootable
chain — so `mutate()` raised on the count rather than silently editing both.
That is `intel-handoff26.md` §3's over-match direction working as designed:
matching too much raises loudly, matching nothing prints `ok`. Fixed by extending
the anchor upward to include the preceding `NVLOG` argument line.

---

## §5 — gate readings

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240**, unmoved — correctly, no `#define` added |
| `tests\build-msvc.bat` | **9/9** suites passed; `test_falcon` **193 → 239** |
| `tests\check-kext-syntax.bat` | exit **0** |
| `tests\run-python-tests.bat` | **1024** tests, **1 failure, not mine** — see §5b |

239 sits **ABOVE** `test_falcon`'s previous ceiling (193 + max(8, 19) = 212) by
twenty seven, so the count band FIRED on its own — correctly, since this is a
whole new section rather than assertions over existing fixtures.
`tests/test_check_count_floor.py`'s entry was updated with a dated provenance
block, which is the thing that gate exists to require
(`a-band-around-a-table-cannot-audit-the-table`).

Python 1010 → 1024 is +14, and mine is **14**, measured by running the module
alone BEFORE the gate.

### §5a — the payload

Both shipping files changed, so `build-payload/` was rebuilt: **THIRTY FIFTH**
image, tar 399360 → 409600 (md5 `062e4ca8f1cc0859eaf049b0eafba981`), ISO 477184 →
487424 (md5 `ce6ad0bfef4bc676a550256e550e6d71`). One 10240-byte tar block each.

### §5b — the one failure, again not fixed

`test_source_cited_paths.test_every_cited_path_resolves` fails on the same single
token as in handoff35 §4a: `phase0-kext/IntelScanout.hpp` cites
`tests/test_intel_scanout.cpp`, which does not exist. Attributed by mtime —
`IntelScanout.hpp` predates every edit in this session by thirteen hours — and
`intel-handoff29.md` is in the tree. This is the peer's Intel-track work,
half-landed.

**Nothing was touched**, for the second session running. The gate's verdict in a
two-agent tree is a JOINT measurement; red does not mean you broke it. Reading
the gate before and after my own changes and getting the identical single failure
is also what establishes that every path token I added resolves.

---

## §6 — what this does NOT establish

1. **Nothing has run.** `publishGspErrorLatches()` and the three new reads are
   reachable only by parse. `tests/test_falcon.cpp` executes the verdict
   function 239 checks' worth; it executes no line of the shell, by
   construction, and the kext has never been `kextload`ed.
2. The verdict is a verdict about REGISTER READS, not about the card. Every arm
   of it can be produced on a machine with no NVIDIA hardware.
3. `NVRISCV_ERR_UNREADABLE` has one honest ambiguity, written into the header
   rather than fixed: a latch genuinely holding all-ones reports UNREADABLE.
   Threading the mapping length through would couple the dependency-free header
   to the shell's state, and the alternative — believing the value — publishes a
   fault report over a word nobody measured.
4. The log block and the registry agree by construction only as long as nobody
   edits one arm and not the other. No gate covers that agreement — handoff33
   §7 item 2's limit, now inherited across four handoffs.
5. The gate is spelled against `publishGspErrorLatches(` and `inspectGspEngine(`
   by name. A second publisher or a renamed consumer produces a REFUSAL string
   rather than silence, which is the correct direction, but a THIRD publisher
   added elsewhere is a subject this module does not consider at all.

---

## §7 — open, highest value first

1. **One `kextload` on a machine containing the card.** Unchanged for sixteen
   sessions and still the largest single piece of evidence this project does not
   have. It needs SIP lowered and a reboot, and the VMware guest has no NVIDIA
   card to match against.
2. **`3050-handoff27.md` §1 outranks everything on this track and should be read
   before planning more NVIDIA work.** The laptop is **muxless** — all graphics
   route through the Intel chip, HDMI being the only bypass — so `PLAN.md`
   termination condition A is SATISFIED. A complete, perfect NVIDIA framebuffer
   driver still puts nothing on the internal panel. Everything on this track is
   Phase 4/5 engineering and is not on the critical path to a picture.
3. **The publication sweep this session continues is now DONE for the diagnoses
   named across handoffs 31–35** (BOOT_42, BOOT_0, GSP boot mask, stage verdict,
   error latches). The next audit of this class needs a fresh enumeration rather
   than an inherited item — and an inherited item is a claim with an age
   (`3050-handoff29.md` §1, `3050-handoff32.md` §1).
4. A gate over the log/registry agreement in §6 item 4, which would close a limit
   now inherited across four handoffs.
5. `brace_depth()` and `guard_span()` now exist in two sibling modules with the
   depth convention differing by one between them (§4a). Neither is wrong for its
   own subject, but a third module copying either will get it wrong 50% of the
   time. Worth folding into one helper the family shares.
6. `tests/test_intel_scanout.cpp` (§5b) is the peer's to write, not this track's.
   Do not close it from here.
