# handoff15 — one hour of Tiger Lake silicon, and exactly three claims moved

Newest handoff. Read `handoff14.md` for the state this continues from, and
`CLAUDE.md` for the four verification gates.

This session did not touch the NVIDIA path. It is the third session on the Intel
track, and the first in the whole tree where an Intel constant met live silicon.
The operator had a Linux boot available for roughly one hour on the target
laptop's Tiger Lake iGPU, said plainly that exactly ONE script would be run, and
ran it. §1 is what the hardware answered. §2 is the larger and more important
list: what it did **not** answer, including two questions the probe was
structurally incapable of answering, which is not the same thing as "absent".

---

## 1. What the capture settled

The artifact is `igpu-results/tgl-capture-2026-08-16-022556.txt`. Everything
above its READING NOTES divider is the machine's verbatim output; everything
below it is host-side analysis added afterwards and is marked as such. Do not
edit above the divider — there will not be a second run of this boot.

Machine: Tiger Lake mobile, PCI `8086:9a68` at `0000:00:02.0`, i915 bound, eDP-1
connected and lit at 1920x1080 (its only advertised mode), HDMI-A-1
disconnected, kernel 7.0.0-29-generic.

Three things are now witnessed rather than transcribed:

**Timing registers store value − 1, on both halves.** Read off transcoder A:

```
HTOTAL  0x081F077F   hactive 1919 -> 1920   htotal 2079 -> 2080
VTOTAL  0x049B0437   vactive 1079 -> 1080   vtotal 1179 -> 1180
PIPESRC 0x077F0437   width  1919 -> 1920   height 1079 -> 1080
```

PIPESRC's `+1` equals eDP-1's advertised mode exactly. That is the item
`IntelDisplay.hpp` hazard 5 flagged as unsettled, and a lit panel was the only
thing that could settle it.

**`PLANE_SIZE` follows the same − 1 convention.** `0x0437077F` at `0x70190`,
pipe A plane 1, read in the same breath as the PIPESRC above. Before this,
handoff14 §2 recorded the convention as UNPROVEN because the writer
(`skl_universal_plane.c`) is not vendored and the vendored `intel_display.c`
never mentions `PLANE_WIDTH`/`PLANE_HEIGHT`. The header now carries
`PLANE_SIZE_OBSERVED_2026_08_16` and two **new** decoded wrappers,
`igPlaneSizeWidth()` / `igPlaneSizeHeight()`. The `Raw` pair is unchanged on
purpose — callers written against raw behaviour must not silently start getting
a different number.

**And the pair of them is a hazard, which is the real finding.** The field order
is INVERTED between the two registers:

```
PIPESRC    0x077F0437 -> width  in 31:16, height in 15:0
PLANE_SIZE 0x0437077F -> height in 31:16, width  in 15:0
```

Both encode 1920x1080 minus one. They are byte-swapped halves of one another.
Reuse one decoder for the other and you get a plausible number with the axes
transposed — a wrong surface that programs cleanly. On a square test mode the two
readings are indistinguishable, so the suite needs separating inputs; that is the
family `cross-checks-probe-the-far-side-bit` is in.

Two smaller positives worth carrying: `PLANE_SURF` (+0x1C) and `PLANE_SURFLIVE`
(+0x2C) both read `0x01058000`, a real 4 KiB-aligned GGTT offset, and their
agreement is evidence both addresses are right rather than one aliasing the
other. `PLANE_CTL` `0x82009000` decodes as ENABLE=1, format 4 (XRGB8888), tiling
4 (`PLANE_CTL_TILED_Y`). Bit 15 is set and is deliberately **not** named — naming
it here would mean inventing a name and filing it next to traced ones.

## 2. What it did NOT settle, and why the probe could not have

This is the section most likely to be misread, so it is stated before anything
else in the tree gets updated.

**Every unused display page on this silicon reads `0x00000000`, not all-ones.**
That one fact invalidates three of the script's four verdicts.

The script's `dead()` treated only `None` and `0xFFFFFFFF` as unreadable. So
transcoders B, C and D — which certainly EXIST on Tiger Lake — were printed with
the verdict word **ABSENT**. That word is simply false for them; the true state
is DECODED-BUT-UNPROGRAMMED, and a single read cannot separate the two. Read
every `ABSENT` in the capture as UNPROGRAMMED. `dead()` and the verdict word have
both been corrected since (`UNPROGRAMMED-OR-ABSENT`), and
`tools/_igpu-reader-dryrun.py` gained a zero-fill arm — but note *when*: after
the run. The dry-run harness could not have caught this, because its synthetic
BAR was filled with `0xFF`, which is what the author expected hardware to do
rather than what this hardware does. A harness that encodes the author's
expectation tests the expectation.

**Q1/Q2 — the DSI pages and the 0x800 stride: NOT settled, and unsettleable
here.** `0x6B000`, `0x6B800` and the `0x6C000` stride probe all read
`0x00000000`. Zero is what an unprogrammed page reads on this silicon, so zero at
`0x6C000` does not distinguish "DPCLKA lives here" from "a timing block lives
here" — the probe's whole separating logic assumed all-ones. The `0x800` DSI
stride stays sourced to `reference/i915/intel_display_device.c:79-80` and
UNWITNESSED. `IG_TRANSCODER_DSI0`/`DSI1` remain pure transcription. An eDP-only
laptop cannot ever answer this: nothing will program a DSI transcoder on a
machine with no DSI panel. Do not file this as a re-run; file it as needing
different hardware.

**Q3 — plane count: NOT settled, same cause.** All 7 probed slots on all 6 pipes
returned a readable `0x00000000`. A disabled plane and a non-existent slot are
therefore indistinguishable, so "7 readable slots" bounds nothing at all. The
only positive fact is that pipe A plane 1 is live at `0x70180` and plane 2's slot
at `0x70280` is a distinct address — consistent with the `0x100` stride, no
evidence of a count. `IG_PLANE_POLICY_COUNT` stays 5 and stays a policy ceiling.

**The first run failed, and the failure is kept on purpose.** The 02:25:56
attempt had lockdown `none [integrity] confidentiality` and the BAR0 mmap raised
at reader line 6, because integrity lockdown refuses `LOCKDOWN_PCI_ACCESS`.
Secure Boot was disabled between the two runs. Both logs are retained: the
failure is the proof that the mmap path is real and gated rather than silently
faked. A reader who sees only the success cannot tell those apart.

## 3. Gate state, measured at the end of this session

All four gates were run to completion after the last edit. Verdict words, not
exit codes — and each one was redirected to a file and grepped for its verdict
line rather than piped into `tail`, because `$?` after a pipeline is the last
stage's status:

```
python tools/check-registers.py     114/114 passed, 0 failed, 0 skipped   exit 0
tests\build-msvc.bat                100% tests passed, 0 failed out of 6  exit 0
tests\check-kext-syntax.bat         === ALL SYNTAX GATES PASSED ===       exit 0
tests\run-python-tests.bat          Ran 802 tests ... OK                  exit 0
```

The Python total was 788 before this session; the 14 new tests are
`tests/test_igpu_dryrun_arms.py` (§5a).

The six C++ suites, each run directly and its final line read:

```
test_decode          64/64 passed, 0 failed
test_falcon        169/169 passed, 0 failed
test_riscv_ucode     116 checks, 0 failures
test_gsp_boot        217 checks, 0 failures
test_gsp_stage       181 checks, 0 failures
test_intel_display   218 checks, 0 failures
```

`test_guest_build_kext.py` was RED before the payload rebuild below and is `Ran
39 tests ... OK` after it.

`test_intel_display` was 205 at the start of this session; the 13 new checks are
the hazard-5 conversion from hypothetical to measured plus the two new decoded
wrappers. `tests/test_check_count_floor.py` `MEASURED["test_intel_display"]` is
205 → 218, and that entry now carries its own paragraph explaining why it keeps
going stale: the floor is `MEASURED // 2`, so a suite that GROWS never reddens
the module and nothing anywhere fails when a number there falls behind. Only a
suite that SHRINKS past half is caught. Fifth staleness in that file.

**`114/114` still says nothing about the Intel constants.** `tools/check-registers.py`
still contains no `IntelDisplay` token — verified this session by grep, not
assumed. handoff14 §3 remains the highest-value open item in the tree, unchanged,
and it is now assigned as a crew card (§5).

**The payload tar was stale and is rebuilt.** `IntelDisplay.hpp` was edited at
10:08 while `build-payload/kext-src.tar` was from the previous day, so
`test_guest_build_kext.py` was RED naming the header — the documented trap in
`editing-a-kext-header-reddens-the-python-gate`. Rebuilt: iso md5
`d9880069620e064b5f6751727a05e93a` (303104 bytes), `kext-src.tar` md5
`413ab5e2488bdcd0907bd7bfe1ef5cb1` (225280 bytes), vol ident `PAYLOAD`.

## 4. The sweep: what was corrected, and what was deliberately left standing

The capture falsified prose in the tree. The sweep was kept narrow on purpose —
the temptation after a successful capture is to mark the whole track witnessed,
and exactly three constants earned that.

- `tools/igpu-plane-dsi.sh` said every plane constant was "untouched by live
  silicon" and that the DSI transcoders were "pure transcription". The first half
  is now false and the second is still true. The paragraph is **kept** as an
  authoring-time record with a dated PARTIALLY SUPERSEDED block beneath it that
  states per-question what moved and, more importantly, that Q1/Q2/Q3 did not and
  cannot on this machine.
- `phase0-kext/IntelDisplay.hpp` hazard 5 was rewritten from UNKNOWN to WITNESSED
  with `PLANE_SIZE_OBSERVED_2026_08_16` carrying the timestamp, PCI ID and panel
  state. It follows the `*_OBSERVED_*` convention: measured values are named as
  measured, and the raw accessors were not harmonised into the decoded ones.
- The numbered handoffs are **not** edited. Every one up to handoff14 says the
  Intel constants have never met silicon; they are a chronological record, and
  that sentence was true when written. This file is where the change is recorded.

## 5. Crew state — four cards out, all of them the same shape

Four agents are on the Intel and kext-load work. Every card carries its
separating-input requirement in the card text, because this tree's recurring
failure is not a wrong answer, it is a green check that matched nothing:

| card | agent | what it must prove |
|---|---|---|
| `t-2373c0` | brave-ibis-66 | `check-registers.py` reads `IntelDisplay.hpp` against `reference/i915/` as a SECOND corpus; mutate one `IG_` constant in memory and show the gate reddens naming it |
| `t-3ec220` | swift-ibis-f3 | the suite covers the two new wrappers, and a check FAILS if `igPlaneSizeWidth()` and `igPipesrcWidth()` agree on the same word |
| `t-860050` | calm-wolf-a8 (**done**, §5a) | the four dry-run arms reach FOUR DIFFERENT verdicts; mutate `dead()` back to the pre-fix form and show the test reddens naming the arm |
| `t-96282c` | keen-moth-e0 | statically prove no unguarded MMIO STORE exists outside the single guarded accessor; plant a bare store in a temp copy and prove the checker reddens |

### 5a. `t-860050` closed — the four dry-run arms now provably disagree

`tests/test_igpu_dryrun_arms.py`, 14 tests, carried by
`tests\run-python-tests.bat` (788 → 802).

The thing that needed proving was not that the harness runs. It is that the four
arms of `tools/_igpu-reader-dryrun.py` reach four DIFFERENT verdicts — because
`dead()`, the corrected verdict word, and the zero-fill arm were all added AFTER
the capture, so as of this morning nothing had shown the arms separate anything.
Four arms printing the same thing are decoration, and that is the exact family
this tree keeps getting bitten by.

Measured, and each arm is asserted on the quantity it exists to vary:

```
arm 0     trans A PIPESRC=0x077F0437  raw=1919x1079  +1=1920x1080
arm 1     trans A PIPESRC=0x07800438  raw=1920x1080  +1=1921x1081
arm dead  CONTROL  FAILED -- no transcoder A..D has a live HTOTAL
          reader unproven; every null below is INCONCLUSIVE, not ABSENT
arm zero  no bare ABSENT anywhere; pipes print "(no readable plane slot)"
          while pipe A still finds plane 1 @0x70180
```

Arm 1 is the one that matters most: a reader that ignored PIPESRC entirely and
echoed the panel mode would print arm 0's line in both arms, and every other
assertion would still pass.

**Non-vacuity is proved by mutation, not by the green run.** Three mutations,
each required to REDDEN a named assertion:

- `dead()` reverted to `None`/`0xFFFFFFFF` only — the shipped bug — makes the
  zero arm print `7 readable slots` again. Without that, the
  "does not count zero slots as readable" assertion proves nothing.
- the verdict word reverted to `'ABSENT'` makes the bare word reappear. Note the
  matcher needs `(?<!OR-)` — `\bABSENT\b` matches inside
  `UNPROGRAMMED-OR-ABSENT` because `-` is a word boundary, so the naïve test
  would have failed against the CORRECT word.
- the reader's `rd(base + 0x1C)` changed to `+ 0x00` makes arms 0 and 1 stop
  differing, which is what anchors the Q4 assertion to PIPESRC rather than to
  some incidental byte.

Each mutation asserts its anchor occurs **exactly once** before replacing it —
`a-mutation-that-matches-nothing-prints-ok` is in this tree's history and a
no-op mutation passes silently.

Two changes to `tools/_igpu-reader-dryrun.py` were needed to make this testable
rather than reckless: `IGPU_DRYRUN_SCRIPT` lets the test drive a MUTATED temp
copy (editing the live script would make a concurrent gate run read a
half-written file and report a named FAIL with no defect present — the
`a-tree-mutating-probe-fakes-a-named-fail` failure mode), and the scratch BAR
image is now PID-named, because five runs in one module plus other agents' gate
runs would otherwise share one path and one run's `os.remove()` would starve
another.

What this does **not** establish: nothing here validates the capture. The arms
are synthetic BARs. The test proves the reader's verdicts are sensitive to the
inputs they claim to measure — which is the property that was missing when the
real run printed a false ABSENT.

The second corpus point in `t-2373c0` is load-bearing and worth repeating here:
do **not** append the i915 headers to `SOURCE_HEADERS`. That table is merged
first-definition-wins for NVIDIA's swref headers, and mixing two vendors'
namespaces into one dict invites exactly the silent collision `merge_sources()`
exists to report.

## 6. What did not change, and it is the part that matters

handoff14 §6 stands verbatim. Three witnessed constants do not move the distance
to a working driver. What exists is still read-only decode: address computation
and 32-bit word interpretation, now with three of its conventions confirmed
against one sample on one machine. There is still no write path, no DPLL or combo
PHY programming, no power-well sequencing, no GMBUS/DDC/EDID/AUX/link training,
no GGTT or framebuffer allocation, and no Intel `IOService` match dictionary.
Apple never shipped Tiger Lake, so there is no `AppleIntelFramebuffer` to lean
on, patch, or imitate.

One sample is also worth naming as a limit rather than a caveat. `PLANE_STRIDE`
read `0x3C` = 60; the panel is 1920 px at 4 B/px = 7680 B/line, and 7680 / 60 =
128, so the stride unit on this Y-tiled surface is 128 bytes. That is derived
arithmetic from a single observation, not a witnessed unit field, and it is
recorded that way.

## 7. Start here

`t-2373c0` — wire `IntelDisplay.hpp` into `tools/check-registers.py`. It was the
highest-value open item before this capture and it still is, and the capture made
it slightly worse rather than better: there are now three constants in that
header whose values came from measured hardware, sitting in a file that the one
gate built to catch transcription errors does not read.

## 8. Added later the same day — §3's payload hashes are superseded

§3 records the seventh rebuild's iso md5 `d9880069620e064b5f6751727a05e93a` and
tar md5 `413ab5e2488bdcd0907bd7bfe1ef5cb1`. Both are stale, and the reason is
worth more than the numbers.

The tar md5 in §3 was never canonical. It came from a tar assembled by hand
rather than by `tools/build-step7-payload.py`, which is the recipe
`OPERATOR-ACTION-step7-build.md` documents. Identical size, identical content,
different hash. The canonical tar is `02a381ba54247e7ddc4aab9968b401ea`.

The iso md5 was worse than stale: it was **not a fingerprint at all**. Two
rebuilds from byte-identical inputs both came out 303104 bytes and differed in 34
bytes across 28 runs — `0x24` (36) vs `0x36` (54) in the seconds field of binary
directory records, and the ASCII `'36'`/`'54'` at offset 12 of the PVD date
strings. pycdlib reads the wall clock internally and encodes it through
`time.localtime()`, so the image depended on both the moment of the build and the
host timezone.

That is why `tests/test_operator_step7_doc.py`'s
`test_every_md5_the_card_quotes_can_be_accounted_for` was RED, and why editing
the card would only have satisfied it until the next rebuild. The fix went into
the generator: `pinned_iso_clock()` and `PINNED_ISO_EPOCH` in
`tools/deliver-via-cd.py` freeze `time.time` and point `time.localtime` at
`time.gmtime` for the duration of the write. Pinning the epoch alone would have
left the image stable on this box and different under another TZ.

Eighth rebuild, verified by two consecutive `--skip-gate` runs hashing
identically: iso md5 `b4cafd723cb2ee51a723f2bf507ae431` (303104 bytes), tar md5
`02a381ba54247e7ddc4aab9968b401ea` (225280 bytes), staged script md5
`599b51eb31500c9ef9ff4b1f2dee0476` (12677 bytes), vol ident `PAYLOAD`.

One wrinkle in the sweep is its own small lesson. The first version of the card
edit cited pycdlib's module filenames and line numbers, which reddened
`tests/test_documented_paths.py` — that gate cannot tell a path inside an
installed site-packages package from a file in this checkout, so three real
citations read as three broken paths. The line numbers now live only in
`tools/deliver-via-cd.py`, where they are comments beside the code they describe
rather than prose a gate has to resolve.

The pin is gated rather than merely reasoned about. `tests/test_deliver_via_cd.py`
gained five tests (802 → 807): three on the context manager's scope — both
patched names restored, restored on exception, and the epoch frozen as a
self-invalidating check because changing it moves every payload md5 the operator
card quotes — and two on the image. The second image test is the one that matters:
it swaps in a pin set one day later and requires the bytes to MOVE at unchanged
size. Without it, `test_two_builds_from_the_same_input_are_byte_identical` could
be measuring a constant, and would stay green if `build_iso()` stopped routing
through `pinned_iso_clock()` altogether.

All four gates after the change: `114/114 passed, 0 failed, 0 skipped` ·
`100% tests passed, 0 failed out of 6` · `=== ALL SYNTAX GATES PASSED ===` ·
`Ran 807 tests ... OK`.

## 9. Crew board, audited rather than believed — measured 2026-08-16 11:49

§5's table is the state the cards were ISSUED in. This is the state they are
actually in, and the difference between the two is the point of the section: a
card reports what an agent claims, and two of the four claims did not survive
being run.

**`t-3ec220` (swift-ibis-f3) — closed, and verified in the source rather than on
the board.** `tests/test_intel_display.cpp` is 218 checks, 0 failures. The card's
load-bearing requirement was not "the wrappers are covered" but "a check FAILS if
`igPlaneSizeWidth()` and `igPipesrcWidth()` agree on the same word", and that is
present: the separating negative controls sit at lines 780-783, driven by
`0x0437077F` — the real captured word, whose halves are unequal, so the assertion
cannot pass by coincidence the way it would on a square mode.

**`t-860050` (calm-wolf-a8) — closed, §5a.**

**`t-2373c0` (brave-ibis-66) — CLOSED. handoff14 §3, the highest-value open item
in the tree for two sessions, is answered.** `tools/check-registers.py` now
reads `phase0-kext/IntelDisplay.hpp` against `reference/i915/` as a genuinely
SECOND corpus. Measured 2026-08-16 11:55:

```
kext headers  : ... IntelDisplay.hpp (50 defines)
i915 headers  : 3180 defines from 3 files
165/165 passed, 0 failed, 0 skipped        (was 114/114)
```

The structure is `INTEL_HEADER` at :44, `I915_HEADERS` at :62, `INTEL_CHECKS` at
:400, `merge_i915_sources()` / `i915_resolve()` / `i915_genmask()` /
`ig_switch_cases()` at 1303-1424, `run_intel_checks()` at :1425 and
`intel_sanity_checks()` at :1511, called from `main()` at 1825-1826. The i915
corpus is deliberately NOT appended to `SOURCE_HEADERS` — handoff15 §5 said not
to, and the comment block at 48-60 gives the two reasons: first-definition-wins
merging would let two vendors' namespaces collide silently, and i915 spells its
values through `REG_BIT`/`REG_GENMASK` macros that mean nothing in NVIDIA's swref
headers. So the Intel side gets its own table and its own resolver front end.

**I recorded the opposite finding first, and the way I got it wrong is the
lesson.** At 11:49 I grepped for the three TABLE names, found definitions and no
other references, and wrote that the tables were inert — a plausible and
familiar-looking false-green. The functions that consume them are 900 lines
further down and share no token with the tables' names, so a grep scoped to the
names I already knew could not see them. A grep for identifiers you chose
yourself only confirms your own hypothesis. Running the gate and reading its
printed count would have settled it in one call, and that is what settled it.

**The PROOF REQUIRED half is now a gate rather than a one-time run:
`tests/test_intel_gate_nonvacuity.py`, 6 tests (807 → 813).** A moving count
proves the rows are EVALUATED; it does not prove they are SENSITIVE. Three shapes
print identical green — a row comparing a symbol against itself, a row whose i915
name is missing (which `Report.skip()` records, and a skip is a silent pass), and
a whole `INTEL_CHECKS` kind whose dispatcher branch is dead while the other kinds
carry the pass. So the module mutates one `IG_` constant at a time in an
in-memory copy of the parsed table and requires the report to redden NAMING that
constant, one arm per kind — and the kinds are discovered from `INTEL_CHECKS`
rather than hardcoded, so a newly added kind is covered the day it appears.
Measured: kinds present are `delta`, `mask`, `shift`, `value`; the clean tree is
53 passes, 0 failures, **0 skips**.

Two further arms cover what `parse_defines()` cannot see. The twelve
`igTransBase()`/`igPipeBase()` bases are switch cases, not `#define`s, so
`intel_sanity_checks()` reads them out of the header TEXT; moving transcoder B's
`0x61000u` by 0x40 must redden, or every base is unverified while the summary
still names the header. And an emptied corpus — either one — must FAIL rather
than skip, which is the guard at the top of `run_intel_checks()`: without it a
rename empties the table, every row degrades to a skip, and the gate prints green
while checking nothing.

That module was written twice, and both defects are kept as comments where they
bit, because both were silent rather than loud. `parse_defines()` and
`merge_i915_sources()` each return a TUPLE; binding the tuple as the table does
not raise — `resolve()` simply misses every name and the output becomes a
plausible wall of "not found in vendored i915 headers". The magnitude assertions
exist for exactly that. And the first switch-base arm anchored on `0x60400`,
which the header spells zero times, so the mutation could not be applied at all
— `a-mutation-that-matches-nothing-prints-ok`, one level up. Every anchor is now
asserted to occur exactly once before it is replaced.

**`t-96282c` (keen-moth-e0) — in flight.** `tests/test_no_unguarded_writes.py`
does not exist yet; the peer is active on it. The store inventory was measured
and handed over rather than left to be rediscovered, because the obvious way to
find it is the way that fails. There are exactly **two** store sites in
`phase0-kext/` and **one decoy**:

| Site | Guards standing in front of it |
|---|---|
| `NV3050Probe.cpp:367` `OSWriteLittleInt32(fBar0Base, offset, value)` | `!fBar0Base \|\| !fBar0Map`; `offset + sizeof(UInt32) > fBar0Map->getLength()`; `!fAllowGspWrites` |
| `NVGspBoot.hpp` `nvGspBootWrite()` → `m->io.write32(m->io.ctx, absolute, value)` | `io.write32 == 0`; `!nvIsInGspAperture(absolute)`; `!m->io.allowWrites` |

The decoy is `NV3050Probe.cpp:910`, `io.write32 = &NV3050Probe::gspBootWrite32`.
That is a function-pointer **assignment**, not a store, and a matcher that counts
it reports three sites and is wrong about its own arithmetic forever.
`fAllowGspWrites` is written in exactly one place, `NV3050Probe.cpp:45-48`, under
`#ifdef PHASE4_ALLOW_WRITES`. Token counts by file: `NVGspBoot.hpp` 4,
`NV3050Probe.cpp` 1, every other header 0.

**`t-ae7a38` — CLOSED, satisfied by `t-e6c35c`'s rewrite rather than by a second
edit to the same file.** Both cards named `tests/test_check_count_floor.py`, and
two agents editing one table is how a mid-write snapshot becomes a false red, so
the card was folded into the peer's and closed on evidence from **running** it:
13 tests OK, and `tests/_negctl_check_count_floor.py` diagnoses every planted
defect it claims to catch.

The card asked for floors that are machine-derived instead of prose-attested, and
that is now the file's shape. `floor_for()` at 131 is `max(1, MEASURED//2)` and
`ceiling_for()` at 210 is `measured + max(8, measured//8)` — both derived, and
the asymmetry is deliberate. `test_each_built_suite_reports_at_least_its_floor`
at 266 runs each binary and asserts the live count against **both** edges,
naming `MEASURED[stem]` and the number to change it to.

That upper edge is the entire fix. Its absence is why the table went stale five
times without a single red run: a floor of `measured // 2` cannot notice a suite
that GROWS, and growing is the only thing these suites ever do. The set of suites
still comes from `suite_stems_on_disk()` rather than from `MEASURED`'s keys, so a
new suite cannot be skipped, and 296-300 refuses to pass by absence — if
`tests/build` exists, every recorded stem must actually have been run.

`MEASURED` was verified the same way, by running all six binaries: decode 64,
falcon 169, gsp_boot 217, gsp_stage 181, intel_display 218, riscv_ucode 116.
Every entry matches live output exactly. Worth being precise about what that
means — the table was **already accurate** when the card was filed. The defect
was never a wrong number; it was that nothing could have told us either way.

**`t-45010b` — CLOSED. `tests/test_capture_integrity.py` exists, 5 arms, all
green (813 → 819).** The capture is the only copy of a boot that cannot be
repeated, and until now "nothing above the READING NOTES divider may be edited"
was enforced by PROSE ONLY — no gate read
`igpu-results/tgl-capture-2026-08-16-022556.txt` at all. Measured 2026-08-16 by
hashing the live artifact:

```
capture region   lines 1-69 of 141, 3539 bytes, LF-only
sha256           ca500bd06c906a08fbe2f8cce3df7c3b641153de192500e85328b387410c4dd9
```

Note the boundary: the divider MARK is on line 71, but the frozen region ends at
**69**, because the rule of `=` characters on line 70 was typed on the host and
belongs to the notes half. Handoff prose above said "line 71 of 140" — off by one
in both numbers, which is why the module derives the split by content and pins
nothing about line position except through the hash itself.

The file has two halves with OPPOSITE rules, and the gate has to encode both.
Above the divider is machine output and is frozen. Below is host analysis — the
`0x00000000`-not-all-ones finding, still being applied to the Intel header — and
it is MEANT to keep growing. So arm 2 mutates `0x9a68` (counted: exactly 1 above,
0 below) and requires the hash to MOVE, which is what proves the digest actually
covers the frozen half rather than the whole file. Arm 3 rewrites `UNPROGRAMMED`
(0 above, 2 below), appends a line, and requires the hash to HOLD — without that
arm the module could be hashing everything and nobody would find out until the
next annotation reddened a gate for no reason. Arm 4 appends a trailing line and
requires the frozen region to be byte-identical, which is what would catch a
future refactor swapping the content search for a hardcoded index. Arm 5 asserts
zero CR bytes above the divider: the artifact came off a Linux box, and a
round-trip through a Windows editor is an edit even when no visible character
changes — it names the cause before arm 1 reports the symptom.

The missing-artifact path FAILS rather than skips, and so does a second matching
file: a skip prints as a pass, and "the only evidence file is gone" is the
loudest thing this module could ever have to say. Nothing is ever written to the
artifact — it is opened `rb` once per arm and every mutation happens to a `bytes`
copy in memory.

The general shape, and it cuts both ways in one afternoon: **claimed is not run,
and absent-from-my-grep is not absent.** The pre-fix dry-run arms really were
code no execution path reached, and only running them showed it. `t-2373c0`'s
tables really were wired, and only running the gate showed THAT — my grep said
otherwise and my grep was the thing that was wrong. Reading source proves
authorship in one direction only; a printed count that moves is the evidence in
both.

Gate state at the end of this session, all four run to completion, verdict words
read from redirected files rather than piped into `tail`:

```
python tools/check-registers.py     167/167 passed, 0 failed, 0 skipped   exit 0
tests\build-msvc.bat                100% tests passed, 0 failed out of 6   exit 0
tests\check-kext-syntax.bat         === ALL SYNTAX GATES PASSED ===        exit 0
tests\run-python-tests.bat          Ran 824 tests ... OK                   exit 0
```

The register gate reads 167 rather than the 165 measured at 11:55 — a peer added
two rows between the two runs. That is expected on a live board and is the
reason nothing in this tree pins that number as an equality.

---

## 10. Two cards issued later the same day, and one write-ownership rule

Both of these came out of auditing `phase0-kext/IntelDisplay.hpp` against the
capture rather than against the board. Neither is a suspicion — each was
verified by grep before it was filed, and the measured basis is recorded here so
the next session does not have to re-derive it.

### 10a. `t-4bb4e6` (brave-ibis-66) — nothing pairs the header's measured values against the capture

`tools/check-registers.py` resolves `_OBSERVED_` names, but only for the NVIDIA
corpus — lines 986, 1029-1030 and 1071. `tests/test_capture_integrity.py`
hashes the artifact and never opens the header. So the two halves of the only
measured claim on the Intel track are each guarded and the JOIN between them is
not.

Measured anchors, verified 2026-08-16:

| Word | Where it is cited | Where it occurs in the capture |
|---|---|---|
| `0x0437077F` (PLANE_SIZE) | `IntelDisplay.hpp:562` | lines 38, 125 |
| `0x077F0437` (PIPESRC) | header prose + capture | lines 21, 67, 119, 124 |

The trap that card carries: the capture never spells the string `70190`. It
prints that dump RELATIVELY — `+0x10 0x0437077F` on line 38, under a base
printed earlier. A gate that greps the capture for the OFFSET literal matches
zero bytes and prints a confident green while testing nothing
(`a-mutation-that-matches-nothing-prints-ok`). Gate the VALUE word.

### 10b. `t-d2d8c3` (swift-ibis-f3) — the tree cites a constant no `#define` spells

Three greps, and they contradict each other:

- `IntelDisplay.hpp:29` states *"There are no `*_OBSERVED_*` constants in this
  file, on purpose."*
- The name `PLANE_SIZE_OBSERVED_2026_08_16` is **cited** at
  `IntelDisplay.hpp:562`, `IntelDisplay.hpp:606` and
  `tests/test_intel_display.cpp:712`.
- `grep -rn '^#define.*OBSERVED' phase0-kext/` returns **seven** defines, all in
  `NVFalcon.hpp` / `NVGspBoot.hpp`, and **zero** in `IntelDisplay.hpp`.

My read, handed over as a read and not as the finding: the PROSE is the stale
half. `0x0437077F` is genuinely witnessed and the citation is load-bearing for
HAZARD 5 at `tests/test_intel_display.cpp:706-729`, so deleting the reference
would discard real evidence. The likely correct fix is to ADD the define with
its timestamp per the project rule and correct line 29.

**A correction I issued on that card, recorded because the wrong version went
out first.** I filed `0x70190` as an unattributed number in prose. It is not.
`IntelDisplay.hpp:407` defines `IG_PLANE_CTL_BASE 0x70180u`, `:418` defines
`IG_PLANE_SIZE 0x10u`, and `:565` spells the sum out as
`IG_PLANE_CTL_BASE + IG_PLANE_SIZE = 0x70190`. It is a derived value shown with
its derivation. The mechanical warning about the capture's relative spelling
survives; the defect claim does not.

### 10c. Write ownership on `IntelDisplay.hpp`

Three live cards now point at that one file — `t-62871b` (brave-yak-df),
`t-4bb4e6` (brave-ibis-66), `t-d2d8c3` (swift-ibis-f3). This tree has already
been bitten by two terminals writing one file: a peer's half-written file is
indistinguishable from a finished-but-broken one, and for a GATE it inverts into
a false RED. So the rule broadcast to the crew is:

- **brave-yak-df owns the header** and is the only agent that writes it.
- The other two write TEST FILES ONLY and mutate the header text **in memory**.
- swift-ibis-f3's card needs a real `#define` added — it goes to brave-yak-df as
  an exact line to apply, and is gated from the test file afterwards.

`crew locks` was empty when the rule went out, so an unexpected lock means
someone is mid-write; take other work rather than waiting on it.

### 10d. `tests/test_source_cited_paths.py` — new gate, green (819 → 824)

Taken by the lead rather than filed, because all four peers already hold cards
and this touches nothing they own — it reads `phase0-kext/` and writes only a
new test file.

The gap: `tests/test_documented_paths.py` is thorough, but its `DOCS` tuple is a
list of **markdown** files. Every token it resolves comes out of a `.md`.
Meanwhile the C++ sources are the most heavily commented files in the tree —
the whole design of this project is that the evidence trail lives in the header
next to the constant it justifies — and those comments cite **41 distinct file
paths that no gate had ever opened**.

Measured 2026-08-16 with the doc gate's own resolver:

```
sources          14   (phase0-kext/*.hpp + *.cpp, tests/*.cpp + *.hpp)
distinct tokens  41
DIRECT           16
DOCREL           24
MISSING           1
```

The kext half alone is 8 sources and 25 tokens; the six C++ suites add the rest
and every one of theirs resolves. They are in scope because each suite's header
comment cites the header it drives — a renamed `phase0-kext/NVFalcon.hpp` would
leave `tests/test_falcon.cpp` pointing at nothing while still compiling and
still printing its check count.

**`tools/*.py` is deliberately OUT of scope, and that is measured rather than a
preference.** The same scan over it yields 141 tokens with 27 MISSING, and all
27 were checked by hand: argv placeholders in usage strings (`notes.txt`,
`tools/x.py`), upstream library filenames (`pycdlib.py`), filenames the tool
WRITES at runtime rather than reads (`tgl-display-mmio.txt`), and paths interior
to a built bundle (`Contents/Info.plist`). None are citations of tree files. A
gate over them would need a 27-entry allowance list, and an allowance list that
long stops being a set of exceptions and becomes the rule — absorbing the real
defect it was meant to surface. The C++ sources have zero such artifacts because
their path tokens live in prose comments, not in argv.

The single MISSING is an **allowance, not a defect**. `NVRiscvUcode.hpp:40`
names `extract-firmware-nouveau.py` while quoting what that script says about
`RM_RISCV_UCODE_DESC` sizing. It is an upstream nouveau tool; vendoring someone
else's extraction script to satisfy a path gate would be the gate driving the
code. So it is declared in `ALLOWED` with its reason, and arm 3 requires every
allowance to still be **cited** — an allowance for a token nobody spells any
more is dead weight that will quietly absorb a future real defect with the same
basename.

Two deliberate choices worth not undoing:

- **It imports the doc gate's `resolve()`, `build_index()`, `file_tokens()` and
  `expand_pair()` rather than reimplementing them.** Two resolvers that agree on
  all but one token silently ratify a precedence winner nobody chose. There is
  one resolver in this tree and this module is a second CALLER of it.
- **`scan()` takes `[(rel_path, text)]`, not filenames.** That is what lets arm 4
  prove non-vacuity by mutating `../phase0-kext/NVFalcon.hpp` →
  `../phase0-kext/NVFalcon-does-not-exist.hpp` in memory, with the anchor
  asserted to occur exactly once first. Nothing on disk is written. A gate that
  must corrupt its subject to prove it works is not a gate.

  That anchor is the second one. The arm originally used
  `tools/parse-mmiotrace.py`, which occurs exactly once in the kext half and
  **twice** once `tests/*.cpp` came into scope — so widening the gate tripped the
  arm's own exactly-once assertion, on the first scope change, which is the
  entire reason that assertion is written before the replace rather than after.

Arm 5 names the capture citation specifically: `IntelDisplay.hpp` justifies three
constants against `igpu-results/tgl-capture-2026-08-16-022556.txt`, and if that
filename ever stops resolving, a generic "1 citation broken" line badly
under-reports what happened.

There is **no ceiling** on the token count, unlike `test_check_count_floor.py`.
A C++ suite's check count drifting up means a recorded measurement went stale;
here growth is just more provenance, and capping it would redden the gate for the
act of documenting something.
