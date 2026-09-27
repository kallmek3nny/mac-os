# 3050-handoff33 — four findings that reached the log and stopped there

NVIDIA track, 2026-08-18, continuing from `3050-handoff32.md`. It closes that
file's §8 item 3 — `decodeBoot0()`'s log-only diagnoses — which was named there
as the highest-value item not blocked on hardware.

`phase0-kext/NVDecode.hpp` and `phase0-kext/NV3050Probe.cpp` both CHANGED, so
`build-payload/` was rebuilt and `OPERATOR-ACTION-step7-build.md` updated. That
is the **THIRTY THIRD** image, and this time **neither byte size moved** while
both md5s did.

---

## §1 — the item, and why it is the same class as handoff31 §1

`3050-handoff32.md` §8 item 3, verbatim:

> **`decodeBoot0()`'s log-only diagnoses.** Carried forward from
> `3050-handoff31.md` §7 item 4 and now the highest-value item not blocked on
> hardware. `decodeBoot0()` at `:432` detects a non-Ampere architecture and an
> Ada part specifically, and at `:447` detects a non-GA107 implementation — all
> four findings go to `NVLOG` and none reaches the IORegistry. handoff31 fixed
> the `decodeBoot42()` half; this half has not been swept.

Unlike the last two sessions, the inherited item was **TRUE as written**. Both
addresses check out and the count of four is right. Recorded because handoff29
§1 and handoff32 §1 each found an inherited item false, and the rule that
follows from those two is *run the grep*, not *assume the item is stale* — an
item with an age is a claim to be measured, and the measurement can come back
confirming it.

What `decodeBoot0()` does at `NV3050Probe.cpp:409`–`:453`, after the two dead-read
arms:

| Finding | Site | Where it went |
|---|---|---|
| architecture is not Ampere | `:432` | `NVLOG` |
| ...and it is an Ada part specifically | `:436` | `NVLOG` |
| GA107 — Phase 3 milestone reached | `:447` | `NVLOG` |
| Ampere but not GA107 | `:450` | `NVLOG` |

All four `return` or fall through, and the method returns `void`. Nothing
carries a finding out.

This is `3050-handoff31.md` §1's class for the second time: **a diagnosis can
reach the log and not the consumer**. There the detection was
`decodeBoot42()`'s dead-read arm and the consumer was `publishIdentity()`;
here the detection is `decodeBoot0()`'s four arms and the consumer is the same
function. handoff31 fixed one half of one method and this file fixes the other.

### §1a — what the registry actually said

`publishIdentity()` at `:956`–`:964`, before this session:

```cpp
UInt32 arch = nvBoot0Architecture(boot0);
UInt32 impl = nvBoot0Implementation(boot0);

setProperty("NVArchitecture", nvArchitectureName(arch));

if (arch == NV_ARCHITECTURE_GA100)
    setProperty("NVImplementation", nvAmpereImplementationName(impl));

setProperty("NVChipIdExpected", (UInt64)NV_CHIP_ID_GA107, 32);
```

Three separate problems, and the third is the sharpest.

1. **A NAME is not a verdict.** An Ada part publishes `NVArchitecture = "Ada"`
   and an Ampere part publishes `"Ampere"`. Those are two strings; nothing says
   one of them means *no constant in this driver applies to this silicon*. The
   log line at `:436` says exactly that, in prose, with the three device IDs and
   a pointer to `DEVICE-IDS.md` — and it says it somewhere an operator reading
   IORegistryExplorer is not looking.

2. **`NVChipIdExpected` states the target and never compares.** It publishes
   `0x177` unconditionally. An operator holding a GA106 sees `NVChipIdExpected =
   0x177`, `NVArchitecture = "Ampere"`, `NVImplementation = "GA106"`, and has to
   know NVIDIA's implementation numbering by heart to notice the mismatch.

3. **`NVImplementation` was ABSENT on a non-Ampere part.** This is the one worth
   carrying. The key was guarded by the Ampere test, so an Ada part left it
   missing — and **an absence is a question rather than an answer**. A missing
   key is byte-identical to an older kext that never published it at all, which
   is the same shape as handoff18 §1d's fabricated publication arrived at from
   the other direction: there a value nobody measured looked measured; here a
   real measurement's absence looks like a version difference.

### §1b — why the four gates were blind

- **register gate** — checks `#define`s against NVIDIA's headers. Every constant
  is correct; "this finding reaches a consumer" is not a constant. Unmoved
  at 240.
- **C++ suites** — `decodeBoot0()` and `publishIdentity()` are methods on the
  IOKit shell. No host suite can compile the translation unit, so nothing had
  ever *executed* one of those four arms. Verbatim handoff20 §1's reason and
  intel-handoff26 §1's.
- **syntax gate** — a `void` method that logs and returns is well-formed C++,
  and so is a `setProperty()` that is not there.
- **Python gate** — `tests/test_probe_identity_publication.py` existed and was
  GREEN with fifteen tests, because every one of its five properties was about
  BOOT_42 and the dead-read path. Nothing asked what the readable path says
  about the part it read. Same shape as handoff31 §1a's fourth entry.

---

## §2 — the fix, and the three choices worth arguing about

Two shipping files changed.

**`NVDecode.hpp`** gains `NVPartVerdict` (four codes plus
`NV_PART_VERDICT_LIMIT`), `nvPartVerdictName()`, and `nvPartVerdict(boot0)`
carrying the decision the four log arms encoded.

**`NV3050Probe.cpp`**'s `publishIdentity()` publishes the verdict, a boolean,
and — on the Ada arm — the retarget note the log has always carried.

### The verdict lives in the dependency-free header

The whole point of the split described in `CLAUDE.md` is that a pure function
written in a shell is a pure function nothing runs. Putting the four-way
decision in `NVDecode.hpp` is what makes §3's twenty-five checks possible at
all; leaving it in `decodeBoot0()` would have moved the defect rather than
fixed it. `IntelDisplayNames.hpp` exists for exactly this reason one track over.

### The Ada test runs FIRST, and the order is load-bearing

```cpp
if (arch == NV_ARCHITECTURE_GA100) {
    return (impl == NV_IMPLEMENTATION_GA107) ? NV_PART_TARGET
                                             : NV_PART_AMPERE_OTHER;
}

if (arch == NV_ARCHITECTURE_AD100) {
    return NV_PART_ADA;
}

return NV_PART_OTHER_ARCHITECTURE;
```

`NV_PART_ADA` is a **SUBSET** of "not Ampere". A version testing the general
case first makes the Ada arm unreachable — the dead-arm shape
`3050-handoff32.md` exists to remove, reintroduced one level over — and it is
the one finding of the four with an operator action attached to it. The suite
pins the order with a blob that is both: an Ada part carrying GA107's
implementation number. Only the Ada-first order returns `NV_PART_ADA`.

This is handoff32 §3's emptiness-before-containment reasoning in a new place.
When you split a collapsed decision into named arms, ask which arm is a subset
of which, and pin the order with an input that satisfies both.

### `NVPartIsTarget` is an EQUALITY, never a negation

```cpp
setProperty("NVPartIsTarget", part == NV_PART_TARGET);
```

A `part != NV_PART_ADA` form is TRUE for every non-Ada part **including a Turing
one this driver has never seen**, so the registry would report an unknown part
as the part the driver was written for. Same family as handoff32 §3's
`!= ERR_HEADER` reasoning: a comparison against the wrong pole of a verdict
family answers *fine* for everything the family does not specifically name.
The negative control at §3a pins it.

### `NVImplementation` is published on EVERY arm

The guard is gone. `nvAmpereImplementationName()` answers `"unrecognised Ampere
part"` for a non-Ampere implementation number, which is a **statement**;
`NVPartVerdict` above it says why. An absent key is not.

The verdict and the flag are published **FIRST**, ahead of every value they
qualify — the fifth family after `GSPEngineReadable`, `NVIdentityReadable`,
`NVGspBootReadMask` and `NVBoot42Readable`.

---

## §3 — the C++ gate: `testPartVerdict()`, 96 → 121

`tests/test_decode.cpp` gains a section driving the new family. **96 → 121
checks.**

What it pins:

| Claim | Why it is not redundant |
|---|---|
| a separating input for each of the four verdicts | a family whose arms all return the same value satisfies every "it refused" assertion |
| an Ada part carrying GA107's impl number is still Ada | pins the ORDER; general-first makes the Ada arm dead |
| the Ada verdict ignores the implementation field | the obvious wrong refactor is one join over both fields, and `impl` means something else on Ada silicon |
| all-ones and all-zeroes are not the target part | a verdict answering TARGET for a dead read would say the target was found on a machine that answered nothing |
| every verdict below the family's own limit has a name | a fifth code added with no arm reddens on the commit that adds it |
| the names are pairwise distinct | a copy-paste arm returning a sibling's string passes every "has a name" check — and `"Ampere, not GA107"` beside `"GA107 -- target part"` is exactly the pair that must be distinguishable |

The name-coverage loop bounds on `NV_PART_VERDICT_LIMIT`, and the sentinel is
**read out of the subject** — `nvPartVerdictName((NVPartVerdict)NV_PART_VERDICT_LIMIT)`
— rather than spelled. handoff21 §1b is the reason and it is now a standing rule
in this tree: a sentinel guessed from a sibling function is a comparison against
a string the subject can never return, and every arm then counts as named
unconditionally.

### §3a — the composer, and a constant that does not exist

The section needs boot0 words for architectures the real part never reports, and
architecture is **SPLIT** across two fields in BOOT_0. The composer builds them
through the same shifts the decoder reads, and two checks up front assert it
reproduces the real GA107 word's two fields — a composer that is wrong makes
every check below it a test of two agreeing mistakes.

The first draft spelled `NV_PMC_BOOT_0_ARCHITECTURE_0_BITS` for the low half's
width. **That constant does not exist**; the header defines only
`_ARCHITECTURE_0_MASK`. The fix derives the width from the mask by counting
bits rather than writing `5`, because a hand-written 5 is a second, unchecked
copy of a field layout the decoder already owns — widen the mask and the
composer silently keeps building the old shape.

---

## §4 — the shape gate: property 6, 15 → 21 tests

`tests/test_probe_identity_publication.py` was **extended rather than
duplicated**, for handoff31 §3's reason: a second module over
`publishIdentity()` would duplicate the `audit()` that every module in this
family deliberately shares with the live tree.

Property 6 pins six things: the verdict key is published, the boolean is
published, the verdict is DERIVED by calling `nvPartVerdict(boot0)` rather than
hand-rolled, the boolean is an equality against `NV_PART_TARGET`,
`NVImplementation` is BARE, and the verdict sorts above `NVArchitecture`.

Six negative controls, each asserting WHICH property broke:

| Mutation | Reddens |
|---|---|
| delete `NVPartVerdict` | the verdict-publication check |
| delete `NVPartIsTarget` | the target-flag check |
| `part != NV_PART_ADA` instead of the equality | the equality check |
| hand-roll the architecture test inline | the derivation check |
| re-guard `NVImplementation` on the Ampere test | the bareness check |
| move the verdict below `NVArchitecture` | the ordering check |

The first two are the defect itself, restored. The third is the sharp one.

### §4a — `statement_is_bare()`, and why not a regex

The bareness property needs to decide whether a `setProperty()` is the body of
an `if`. It walks BACK to the previous statement boundary (`;`, `{` or `}`) and
reads what sits between — the only place a guard can hide. A `{` boundary is
ambiguous (function body or `if` block), so it takes one more step and reads the
head before the brace.

Deliberately **not** a regex over the preceding line. `handoff25.md` §1c is the
cost of a character class excluding only the statement terminator: it runs past
the `if`'s closing paren, the newline and the whole guarded body to land on the
next statement's semicolon, and then reports a guarded call as bare. The
re-guard control at §4 is what exercises this — a line-based check would not
distinguish the two shapes.

handoff18 §1e's every-pre-existing-check-green rule DOES apply here, and it
held: all six controls ran against fifteen pre-existing tests and each reddened
only its own property with every pre-existing test green.

---

## §5 — gate state, all four run after the last edit

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped, exit 0 |
| `tests/build-msvc.bat` | **9/9** suites passed, exit 0 |
| `tests/check-kext-syntax.bat` | exit **0** |
| `tests/run-python-tests.bat` | **980** tests, OK, exit 0 |

Register gate unmoved at 240, correctly: `NVPartVerdict` is an enum, and the
register gate parses `#define`s. Same reasoning as `NVGSP_READ_*` in handoff29
§3 and the ucode verdict family in handoff32 §3.

Python 974 → **980**, the +6 attributed by running
`test_probe_identity_publication` alone: **21 tests**, fifteen pre-existing plus
six new.

`test_decode` 96 → **121**. That is **ABOVE** its own previous ceiling
(96 + max(8, 12) = 108) by thirteen, so the count-floor band **FIRED on its own**
— like 217 → 246 and 276 → 313, and unlike yesterday's 188 → 211 which grazed
its edge without tripping it. Second movement of this entry in two days; the
provenance paragraph in `tests/test_check_count_floor.py` records both.

### §5a — four mistakes of mine, all caught by the gates

Worth recording because three of the four were *referencing something that does
not exist*, which is the failure mode of writing a test section from the shape
of a sibling rather than from the file:

1. **`check(label, cond)`** — this suite has `checkU32`, `checkBool` and
   `checkStr`, and no two-argument helper. Seventeen call sites, caught by the
   compiler.
2. **`NV_PMC_BOOT_0_ARCHITECTURE_0_BITS`** — §3a. Does not exist.
3. **`statement_is_bare()`** — referenced in property 6 before it was written.
   A grep for the name returned only my own call site, which is the tell.
4. **`pub` vs `good`** — property 6 referenced the publisher body by a name
   scoped to a different block. Caught by the module's own positive control
   raising `NameError`.

None of these reached a gate reading anyone would have believed. The one that
could have is the second: a hand-written `5` in place of the missing constant
would have compiled and passed, and would have been a silent second copy of a
field layout.

---

## §6 — the payload, and neither size moving

| Artifact | Bytes | md5 |
|---|---|---|
| `step7-payload.iso` | 456704 | `ddb849e49f431cc04600b9c265d8a92b` |
| `kext-src.tar` | 378880 | `97b34eccd8303976f83d8e8ffdf93425` |

**Neither size moved** — tar stayed 378880 and ISO stayed 456704 — while both
md5s changed. That is the padding case, and it is the fourth time this tree has
produced it. tar rounds to 10240-byte blocks, so a change of a couple of KiB
inside an existing block ships a different image at an identical byte count.
`3050-handoff26.md`'s comment-only rebuild remains the standing proof and this is
another instance: **a byte size is not a fingerprint.**

Superseded: ISO `12fcdcd3d157e4cea66f932773a732f0`, tar
`e1f569762dbeb0db55149cf058152589`.

---

## §7 — what this does NOT establish

1. **Nothing here ran on hardware.** `nvPartVerdict()` is a pure header function
   driven by a host suite; `publishIdentity()` has never executed on a machine
   with the card.
2. **`decodeBoot0()` still logs all four findings and still returns `void`.**
   The verdict is recomputed in `publishIdentity()` from the same word rather
   than threaded out of the decoder. That is deliberate — both call sites read
   `boot0` and `nvPartVerdict()` is pure, so there is one decision and two
   readers of it rather than two decisions — but it means the LOG and the
   REGISTRY agree by construction only as long as nobody edits one arm of
   `decodeBoot0()` without the other. **No gate covers that agreement.** See §8
   item 4.
3. **The two dead-read arms of `decodeBoot0()` at `:411` and `:417` are still
   log-only**, and correctly so — `readIdentity()` already routes those to
   `publishIdentityUnreadable()`, which is handoff25's fix. `nvPartVerdict()` is
   only reached on a word that decoded.
4. **`NVChipIdExpected` still states the target without comparing.**
   `NVPartIsTarget` now answers the question it implied, but the two properties
   sit side by side and a reader could still take the chip ID for a measurement.
5. Nothing published here changes what happens on the internal panel. Per
   `3050-handoff27.md` §1 the laptop is muxless, so this is Phase 4/5
   engineering and is not on the critical path to a picture.

---

## §8 — open, most valuable first

1. **The operator's scheduling decision.** Unchanged and still theirs: the laptop
   holds Linux or macOS, not both, so more i915 traces and one `kextload` are
   mutually exclusive. Only the Intel track can produce a picture on the
   internal panel, so the traces are almost certainly worth more.
2. **One `kextload` on a machine containing the card.** Unchanged from handoff22
   §4 through here. Still the only thing that turns twelve handoffs of source
   claims into measurements.
3. **Assert the real booters against the verdict, not the bool.** Carried forward
   unchanged from `3050-handoff32.md` §8 item 4 and now the cheapest item on
   this list. `testRealFirmwareFiles()` opens all three files in
   `reference/ga107-firmware/` and checks `nvRiscvUcodeIsWellFormed()` on them;
   changing those to `checkVerdict()` would pin that a real booter returns
   `ERR_DESC_VERSION` specifically, turning handoff27's measurement into a
   standing assertion.
4. **Nothing pins that `decodeBoot0()`'s log and `publishIdentity()`'s registry
   agree.** §7 item 2. Both read `boot0` and both decide the same four-way
   question, one in prose and one through `nvPartVerdict()`. A gate could
   require `decodeBoot0()` to call the verdict function too, which would make
   the agreement structural rather than conventional. No gate can currently see
   its absence.
5. **The GSP refusal reason is not published.** Carried forward from handoff32
   §8 item 5. `loadGspFirmware()` returns `bool` and the eight-way ucode verdict
   dies in the log — the same class this file just closed for BOOT_0, one
   subsystem over.
6. Nothing tests that `regRead32()`'s refusal value is *the same constant* the
   consumers compare against. `NVWPR2_READ_REFUSED_VALUE` in
   `phase0-kext/NVFalcon.hpp` and `IG_MMIO_INVALID` in
   `phase0-kext/IntelDisplay.hpp` are both `0xFFFFFFFF` by agreement rather than
   by construction. Unchanged from `3050-handoff26.md` §7 item 4.
