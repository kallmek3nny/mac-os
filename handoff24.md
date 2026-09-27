# handoff24 — the guard from handoff22 produced refusals that nothing consumed

Written 2026-08-18. Intel track. Continues `handoff22.md` and closes the other
half of its §1b. `handoff23.md` is a peer's NVIDIA-side session from the same
day and this file does not depend on it — read either order.

One live defect in shipping code, and it is the direct consequence of the fix
handoff22 landed rather than an unrelated find. Three files in `phase0-kext/`
changed, so `build-payload/` was rebuilt and the operator card records the
**TWENTY FIFTH** image.

---

## §1 — a refusal that nobody consumes is a zero

handoff22 taught `regRead32()` to REFUSE a read past the end of the BAR0
mapping and return `IG_MMIO_INVALID`. That was correct and it was half a fix.

The finding is one grep. `IG_MMIO_INVALID` occurred at exactly **one** place in
the whole of `phase0-kext/IntelDisplayProbe.cpp` — inside `regRead32()` itself,
where it is produced. Nothing anywhere in that file ever tested for it. Both
publishers took whatever came back and handed it straight into the decoders:

```
regRead32() refuses  ->  0xFFFFFFFF  ->  igModeConsistent()
                                          igPipeUsable()
                                          igFbInheritable()
                                     ->  setProperty(key, <decode>)
```

`setProperty()` with a decode of a **refused** read is byte-identical to
`setProperty()` with a decode of a register that genuinely read that value. An
operator reading the IORegistry could not tell *"transcoder B is unprogrammed"*
from *"transcoder B was never read, because this BAR is shorter than the block
needs"* — and the second one is a fact about **this driver** that somebody could
go and fix. The first is a fact about the machine that nobody can.

This is handoff18 §1d's `GSPEngineReadable` finding on the Intel side, one level
down. There it was a dead-read early return publishing six literal zeros; here
it is a live guard whose output has no reader. Same shape, same consequence:
**a value the driver never measured, published as a measurement.**

### Why none of the four gates could see it, by construction

The same four-part argument as handoff20 §1 and handoff22 §1, and it is worth
re-deriving each time rather than citing, because the reasons are not the same
reasons in the same order:

- **The register gate checks constants.** `IG_MMIO_INVALID` is `0xFFFFFFFFu` and
  that is correct. Whether anything *reads* it is not a property of a `#define`.
- **The C++ suites cannot compile the subject.** The consumption happens in
  `IntelDisplayProbe.cpp`, which reaches `IOService`, so no host suite can link
  it. The pure headers it calls were all individually correct.
- **`-Werror` has no enum to bite on.** The verdicts are `#define`d `IGU32`, so
  a decoder fed `0xFFFFFFFF` is a decoder fed a legal `IGU32`. Verbatim
  handoff20 §1's reason and now the fourth session it has applied.
- **The Python gate models file shapes, not dataflow.** Nothing in it could
  express "the value produced at this site is tested before that site".

### §1a — the fix, and why it is a GROUP verdict

`phase0-kext/IntelDisplay.hpp` gains two things after `igReadDecision()`:

```c
static inline bool igReadRefused(IGU32 value)
{
    return value == IG_MMIO_INVALID;
}

#define IG_SAMPLE_OK                    0u
#define IG_SAMPLE_REFUSED               1u
#define IG_SAMPLE_VERDICT_LIMIT         1u

static inline IGU32 igSampleVerdict(const IGU32 *values, IGU32 count)
{
    if (!values || count == 0u) {
        return IG_SAMPLE_REFUSED;
    }
    for (IGU32 i = 0u; i < count; i++) {
        if (igReadRefused(values[i])) {
            return IG_SAMPLE_REFUSED;
        }
    }
    return IG_SAMPLE_OK;
}
```

Four load-bearing choices, in descending order of how easy each is to get wrong
later:

**It judges a GROUP, not a field.** This is the one to keep. The decoders are
*joins* — `igPipeUsable()` consumes nine registers and returns a single verdict,
`igFbInheritable()` three — so one refused read anywhere in the group poisons
the *whole answer* rather than one field of it. A per-field flag would publish
eight good values, one sentinel, and a verdict that is arithmetically correct
over an input that was never measured. That verdict is the thing an operator
acts on, and it would carry no mark at all.

**Zero is a measurement, not a refusal.** `igReadRefused(0u)` is `false` and
there is a test asserting exactly that with its reason attached. This is
evidence rather than taste: every unprogrammed display page on this silicon read
`0x00000000` in the 2026-08-16 capture (handoff15 §2). All-ones is not what an
idle page reports — it is what a block that is not decoding reports.

**The one honest ambiguity is written down in the header.** A register that
genuinely reads `0xFFFFFFFF` is indistinguishable from a refusal, because the
predicate is spelled against the sentinel VALUE rather than re-deriving the
decision. It does not matter *here*, for the reason directly above, and both
readings say the same thing about the only question this probe asks: do NOT
inherit a mode from it. Do not "fix" this by threading the decision through —
that couples the pure header to the shell's mapping state, which is precisely
what the logic/IOKit split exists to prevent.

**A null pointer or a zero count answers REFUSED, not OK.** A caller that lost
its array must not be told its reads were fine. Same direction as handoff22's
`fBar0Length` being zeroed in `init()` and `unmapBar0()`: when the bound is
unknown, refuse rather than trust.

`IG_SAMPLE_*` went into `IntelDisplay.hpp` rather than `IntelDisplayNames.hpp`
for a gate reason, not an aesthetic one — `tools/check-registers.py` parses only
two Intel headers, so a verdict family in the third gets **no set-check**.
Only the *name* function lives in `IntelDisplayNames.hpp`. And `IG_SAMPLE` was
grepped tree-wide before the prefix was chosen, because
`intel_verdict_set_checks()` groups families **by prefix** and handoff16 §1 is
what happens when it sweeps in a register offset that happens to share one.

### §1b — publication: both paths, and FIRST

`publishTranscoder()` samples its ten words, `publishPlane()` its four, each
BEFORE any decode:

```cpp
const UInt32 sampled[] = { conf, ddi, hTotal, hBlank, hSync,
                           vTotal, vBlank, vSync, src, planeSize };
UInt32 sample = igSampleVerdict(sampled,
                               (UInt32) (sizeof(sampled) / sizeof(sampled[0])));
```

`SURF` is deliberately inside the plane group even though it feeds only the
offset and the range check. A refused SURF decodes to an offset of
`0xFFFFFFC0`-ish and a range verdict computed over an address nobody read —
exactly the published-zero shape this whole guard exists to stop.

The key is published **on both paths, never conditionally**, for handoff18 §1d's
`GSPEngineReadable` reason: a flag that appears only when something went wrong
is indistinguishable from an older kext that never published it at all. `"ok"`
is a measurement; an absent key is not.

It is published **first**, so `IGTranscoderA-Sample` and `IGPipeAPlane1-Sample`
sort above the values they qualify. Every key below one of them is a decode of
the words that verdict is about, and IORegistry output is read top-down.

`igSampleVerdictName()` returns `"ok"` / `"refused"` / `"unknown"`, and `"ok"`
matching the other four families is a decision rather than an accident — there
is a check in `tests/test_intel_display_names.cpp` asserting it reads the same
word as `igLinkVerdictName(0u)`. An operator scanning the IORegistry reads one
word for every verdict this driver publishes.

### §1c — why it is a named family and not a bool

Two arms is the smallest a family can be and still be a family. It stays a named
family for `igReadDecision()`'s reason: the **string** is what lands in the
IORegistry, and `"refused"` there is a different claim from an absent property or
a `0`. A bool published as 0/1 needs a reader who already knows which way round
it goes, and that reader is an operator looking at a machine they cannot debug.

The entry in `tools/check-registers.py` says this explicitly, because two is
exactly where somebody adds a third arm (a `PARTIAL`, say) without pinning it —
two looks like a bool that grew a name.

---

## §2 — non-vacuity, and the count that made it convincing

handoff18 §1e's rule: only a mutation that leaves **every pre-existing check
green** proves added reach. A loud mutation is usually evidence the OLD tests
sufficed.

Mutation planted: `igSampleVerdict()` reduced to testing `values[0]` only,
dropping the loop but keeping the null/zero-count guard.

**13 failures, and all 13 are new checks.** Attributed by line, every one:

| Failing check | Group |
|---|---|
| refusal at index 1, 2, 3 poisons the group | 4-word |
| ten-word group, refusal at index 1 … 9 | 10-word |
| a count that includes it refuses | count-as-bound |

Zero pre-existing checks reddened. Reverted; `phase0-kext/IntelDisplay.hpp` is
back byte-identical, proven by the C++ gate returning to 9/9 with zero `FAIL`
lines rather than by assertion.

What that table shows is why the tests walk the refusal across **every index of
both group sizes** rather than checking one edge. A single-edge check is a point
sample of a range property, and a bound written for the four-word plane group
would pass every four-word check while being wrong about the ten-word transcoder
group. The ten-word walk is what makes a size-specific bound impossible to hide.

---

## §3 — gate readings, taken serially

All four green, each read from its own exit code rather than a verdict line —
handoff18 §3's rule, and section 11 of the syntax gate is *titled* `negative
control -- each defect must FAIL`, so a bare `grep FAIL` over that log returns 1
hit on a **passing** run. Read the exit code; if you grep, read the line.

| Gate | Reading |
|---|---|
| `python tools/check-registers.py` | exit 0 — **240/240** passed, 0 failed, 0 skipped |
| `tests\build-msvc.bat` | exit 0 — 9/9, zero `FAIL` lines |
| `tests\check-kext-syntax.bat` | exit 0 — `VERDICT: PASS` |
| `tests\run-python-tests.bat` | exit 0 — **896** tests, `OK` |

The register gate moved 239 → **240**. `IG_SAMPLE_` is a new *set*, which is why
the total moved at all — handoff22's `IG_READ_` family growing 4 → 5 left it
unchanged, because that is one set getting larger rather than a new set.

Suite counts, read from the binaries directly:

| Suite | Was | Now |
|---|---|---|
| `test_intel_display` | 391 | **421** |
| `test_intel_display_names` | 236 | **245** |

Both entries in `tests/test_check_count_floor.py` carry a dated provenance block,
per handoff17 §1 — a band around a table cannot audit the table. Worth noting
for whoever tunes that gate: **421 sits INSIDE the previous band** (391 +
max(8, 39) = 430) by nine, the third consecutive real growth to land under its
own ceiling. 245 is inside its band too (236 + max(8, 23) = 259). The band did
not fire on either, and would not have.

---

## §4 — the payload

`phase0-kext/` changed for real, so `python tools/build-step7-payload.py` ran
(exit 0, 16 files byte-identical against the working tree) and
`OPERATOR-ACTION-step7-build.md` records the **TWENTY FIFTH** image:

| Artifact | md5 | bytes |
|---|---|---|
| `step7-payload.iso` | `dc17bd0da8fca184dcce1f5784181559` | 415744 |
| `kext-src.tar` | `cc16808b6e6184c50d479aa1adce1809` | 337920 |
| `guest-build-kext.sh` | `599b51eb31500c9ef9ff4b1f2dee0476` | 12677 |

Superseded: ISO `df4b5ebcab09d06969aa0f438693d037`, tar
`350935ec42fdf5b2d9bca61813a571ad`.

**Neither size moved.** Both stayed at 337920 and 415744 while both md5s moved —
the twenty third rebuild's shape, and the reason is tar padding to a 10240-byte
boundary. A byte size is not a fingerprint; only the md5 test catches a stale
card. The script is genuinely unchanged, which is why its md5 above is the same
one rather than a missing one.

---

## §5 — what this does NOT establish

Read this before the green table above, not after it.

1. **`igSampleVerdict()` has never run on hardware, and the two `-Sample` keys
   have never been published.** The call sites, the `sampled[]` arrays and both
   `setProperty()` calls are in `IntelDisplayProbe.cpp` and are reachable **only
   by parse**. The suites execute the pure header; nothing executes the shell.
   That is the same standing gap handoff19 §7 opened and handoff20 and handoff22
   each narrowed without closing.
2. **No `make` has run since this landed.** The guest build is now overdue by six
   sessions of `phase0-kext/` edits — `IGU64`, `fBar0Length`, `IG_READ_PAST_MAPPING`,
   `igReadRefused()`, `igSampleVerdict()` and `igSampleVerdictName()` have all
   been added since the last Apple-clang compile. The syntax gate parses them;
   parsing is not compiling and `tests/kextshim/` is not Apple's SDK.
3. **The ambiguity in §1a is real and unresolved.** A register genuinely reading
   `0xFFFFFFFF` publishes `"refused"`. Argued harmless above on evidence, but it
   is an argument, not a measurement.
4. **Nothing tests that the publishers still call the sampler.** Deleting either
   `sampled[]` block and its `setProperty()` returns the file to its defective
   state and all four gates stay green — the identical hole this session
   exploited, now one level up. See §6 item 3.

---

## §6 — open, in priority order

1. **One `kextload` on a machine containing the card.** Unchanged since
   handoff13 and still the highest-value action in the project. Needs hardware
   nobody in this loop has.
2. **`make` in the macOS guest.** See §5 item 2. This is the only open item that
   needs nothing but the operator booting the VM and mounting the ISO.
3. **A Python gate over the publishers' call SHAPE.** handoff18 §1d is the
   template and its rule is the important half: *when no gate can see it, pin the
   code shape from Python, derive the identifier rather than spelling it, and make
   the checker REFUSE rather than pass when it cannot find its subject.* The
   subject here is concrete — every `setProperty()` in a publisher must be
   preceded in that function by an `igSampleVerdict()` call whose array contains
   every register the publisher read. Note the gate must scan **code, not prose**:
   handoff18 §1d's own fix comment quotes the defective call, so a naive text scan
   goes red on a pristine tree.
4. **`check-documented-paths.py` tokenizes quoted gate output as paths.** Harmless
   — the TAIL rule catches it — which means it passes for a reason other than the
   one it claims. Lowest priority and the least interesting item here.
