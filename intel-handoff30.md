# intel-handoff30 — the second observed block, and the exclusions that shape it

Continues `intel-handoff29.md`. Intel track only; the NVIDIA track was worked
concurrently by a peer and every NVIDIA number in this file is copied rather
than re-measured.

This session closes `intel-handoff29.md` §8 item 2 — harvest the remaining
configuration registers out of the final capture — and unlike the last several
Intel sessions it CHANGED shipping code. `phase0-kext/IntelDisplay.hpp` moved,
so `build-payload/` was rebuilt (the **THIRTY NINTH** image) and
`OPERATOR-ACTION-step7-build.md` was updated. Both were obligations rather than
choices: the rule that decides a rebuild is CONTENT, and the Python gate is the
thing that reads content.

What this session did NOT do is find a defect. The harvest is evidence being
moved before it becomes unmovable, not a bug being fixed, and §5 is honest that
the interesting half of it is what was left OUT.

---

## 0. Why this had a deadline that has already passed

`intel-handoff28.md` §0 closed the Linux phase. The two files in
`igpu-results/` are every byte of silicon evidence this track will ever have,
and the laptop is being overwritten with macOS. So a value still sitting only
in a capture file is a value that survives exactly as long as that file does,
and nothing in the tree reads it, asserts it, or would notice if a future edit
quietly changed it.

That is the whole argument for this session. A constant nothing reads is
indistinguishable from a typo.

---

## 1. What landed

Ten words, one mask, one function, appended to `phase0-kext/IntelDisplay.hpp`
as a SECOND observed block below the timing/plane one:

| Constant | Value | Anchor |
|---|---|---|
| `IG_OBSERVED_CDCLK_CTL_2026_08_18` | `0x0038017E` | `i915_reg.h:4060` |
| `IG_OBSERVED_SKL_DFSM_2026_08_18` | `0x800C1240` | `i915_reg.h:2858` |
| `IG_OBSERVED_SOUTH_CHICKEN1_2026_08_18` | `0x00000901` | `i915_reg.h:3358` |
| `IG_OBSERVED_SFUSE_STRAP_2026_08_18` | `0x00000000` | `i915_reg.h:4391` |
| `IG_OBSERVED_DPCLKA_CFGCR0_2026_08_18` | `0x01E17800` | `i915_reg.h:4158` |
| `IG_OBSERVED_PANEL_PWR_C7200_2026_08_18` | `0x80000008` | none |
| `IG_OBSERVED_PANEL_PWR_C7204_2026_08_18` | `0x00000067` | none |
| `IG_OBSERVED_PANEL_PWR_C7208_2026_08_18` | `0x07D00001` | none |
| `IG_OBSERVED_PANEL_PWR_C720C_2026_08_18` | `0x01F40001` | none |
| `IG_OBSERVED_BACKLIGHT_C8250_2026_08_18` | `0x80000000` | none |
| `IG_OBSERVED_BACKLIGHT_C8254_2026_08_18` | `0x00017700` | none |
| `IG_OBSERVED_BACKLIGHT_C8258_2026_08_18` | `0x00017700` | none |

Plus `IG_CDCLK_FREQ_DECIMAL_MASK` (`0x7FF`, anchored at `i915_reg.h:4082`) and
`igCdclkFreqDecimal()`, which is the only executable thing in the block.

The header went 120 → **133** defines and the register gate is unmoved at
**240**, which is correct and worth one sentence: that gate checks Intel
`#define`s against the 3180 vendored in `reference/i915/`, and an
`IG_OBSERVED_*` name is a captured VALUE rather than a register definition, so
it has nothing to be checked against. A gate whose number moved here would be
claiming NVIDIA's or Intel's headers contain this laptop's panel state.

---

## 2. The naming split, and why five constants are named by offset

Five carry their i915 names because those names have a citable line in this
checkout. Five do not, and the reason is `intel-handoff27.md` §6 item 5:
`intel_pps_regs.h` and `intel_dp_aux_regs.h` were never vendored, so
`PP_STATUS`, `PP_CONTROL` and the `BLC_PWM` family have **no anchor in this
tree**. Four greps, zero hits, re-run this session rather than inherited.

So those five are named by their OFFSET, and the descriptive words
(`PANEL_PWR`, `BACKLIGHT`) come from the capture's own range table rather than
from me. The alternative — writing `IG_OBSERVED_PP_STATUS_2026_08_18` because
that is very probably what `0x0C7200` is — would put a **fabricated citation
beside five real ones**, and this project's only asset is that the difference
between a witnessed value and an invented one is legible.

The offset naming is uglier and says less. It says less *truthfully*.

---

## 3. The one to read: the exclusions are the measurement

Three section 6 reads look exactly as harvestable as the ten above. They are
not in the header, and the reasons are different from each other.

**`_DDI_BUF_CTL_A` (`0x064000`) is the one that proves the rule.** It read
`0x80000087` in one pass and `0x8000008F` in the other, **inside the same run**.
Bit 3 is `DDI_BUF_IS_IDLE` — a live status bit — so the two readings are one
register in two states rather than a bad read. Pin a value there and you have
pinned whichever pass happened to land first, filed beside the panel's timing
as though equally permanent.

This is `intel-handoff28.md` §4d in its sharpest available form, because
`_DDI_BUF_CTL_A` is a DDI **control** register. It reads like configuration.
It sits in a table of configuration registers. The only reason this project
knows better is that the capture script happened to read a subset twice, seven
registers disagreed, and this was one of them. **The disqualification is a
measurement, not a judgement** — and nothing in the tree asserts that a value
pinned as observed was read more than once, which is the honest gap under all
of this.

**`PCH_DREF_CONTROL` (`0x0C6200`) and `_TRANS_DP_CTL_A` (`0x0E0300`) both read
`0xFFFFFFFF`.** On this silicon all-ones means the block is not decoding at
all — that is the entire content of `igReadRefused()`. Filing either would file
a refusal as a reading, which is the class the Intel track has now hit from
four directions (`handoff24.md` §1, `intel-handoff26.md` §2a, `3050-handoff26.md`
§1b, and here).

**And `SFUSE_STRAP`'s zero is the asymmetry.** It also read a sentinel-looking
value and it IS in the header, because on this hardware zero is a legitimate
reading of a real register — measured across transcoders B/C/D on two separate
boots (`handoff15.md` §2). A rule spelled as "exclude the sentinels" would have
dropped a real measurement. The predicate that gets both cases right already
existed; the only new thing is that it is now *consulted about the header's own
contents*.

---

## 4. What the new test section pins, and the one design choice in it

`testObservedConfig20260818()` in `tests/test_intel_display.cpp`, 421 → **480**
checks.

Two of the three exclusions above are decidable by a predicate this suite
already had, so they are **asserted rather than left as prose**:
`igReadRefused(0xFFFFFFFFu)` is true, and
`igReadRefused(IG_OBSERVED_SFUSE_STRAP_2026_08_18)` is false. A comment
explaining why a value is absent decays into a comment nobody rechecks; an
assertion about the predicate that decides absence does not.

**The choice worth arguing about is the group arm.** The ten harvested words are
run through `igSampleVerdict()` as a single ten-element read group rather than
checked one at a time. That is the same function both Intel publishers use to
judge a set of reads before decoding any of them (`handoff24.md` §1a), and the
reason it is the right shape here is the reason it was the right shape there: a
per-constant check publishes nine good values and one sentinel and a verdict
arithmetically correct over an input nobody measured. The group form means a
future edit that pins an all-ones word into this block reddens **even if it is
spelled correctly**, because correctness of spelling is not the property.

The rest is copy-paste defence: the four panel-power words pairwise distinct,
the anchored words pairwise distinct, and no configuration word colliding with a
timing word from the block above (both blocks came out of the same run, so a
collision means somebody pasted across them).

The two equal backlight words are kept as **two constants**, deliberately.
`0x0C8254` and `0x0C8258` both read `0x00017700`, and the equality is pinned as
a fact about this panel — collapsing them into one constant would assert they
must always agree, which nothing measured.

`igCdclkFreqDecimal()` returns the RAW field and the kHz relation is
**deliberately not encoded**, because that relation is not vendored in this
checkout. The mask is a citation; the conversion would have been a guess
wearing a citation's clothes.

---

## 5. The non-vacuity proof

Two mutations, each planted in `phase0-kext/IntelDisplay.hpp`, each reverted,
and the header ends the session byte-for-byte as the harvest left it.

| Mutation | Reddened | Pre-existing green? |
|---|---|---|
| A — `C7204` duplicated onto `C7200`'s value | **1**, the pairwise-distinct arm | yes, all |
| B — `SKL_DFSM` set to `0xFFFFFFFF` | **1**, the group arm | yes, all |

Both are `handoff18.md` §1e's signal: exactly one new check, every pre-existing
check green. B exists because A cannot isolate the group arm — a duplicated
value is still not a refusal value, so the ten-word verdict stays `IG_SAMPLE_OK`
under A and the two arms would otherwise be indistinguishable from each other.

A is the copy-paste shape that four adjacent `#define`s in a hand-typed table
actually fail as. B is the failure the group arm exists for and would be
invisible to every other check in the section.

The gate exited **8** under both mutations, not 1, because `ctest` reports the
failing-suite count — worth knowing so a future reader does not go looking for
eight broken things.

---

## 6. Gate readings, 2026-08-19

| Gate | Reading | Note |
|---|---|---|
| Registers | **exit 0, 240/240 passed, 0 failed, 0 skipped** | unmoved, correctly — §1 |
| C++ | **exit 0, 10/10 suites**, `test_intel_display` **480 checks, 0 failures** | 421 → 480 |
| Syntax | **exit 0** | read the exit code, not `VERDICT: PASS` |
| Python | **exit 0, 1144 tests, OK** | see below |

**The Python reading is a joint measurement and I cannot claim it.**
handoff29 recorded 997. This session added **no Python test** — it edited one
number and one provenance comment in `tests/test_check_count_floor.py` — so the
997 → 1144 delta is the peer's NVIDIA work, not mine. Recording it as though it
were a result of this session would be exactly the vouching-for-a-peer error
`intel-handoff27.md` §5a and `3050-handoff29.md` §7 both warn about, one
direction over: there the temptation was to green someone else's red, here it is
to bank someone else's green.

The gate was re-run AFTER this file landed, because a new root `.md` is itself a
subject of `tests/test_documented_paths.py` and every path-shaped token in it is
resolved against the checkout by `tools/check-documented-paths.py` — so the 1144
reading above predates its own subject. The re-run is **exit 0, 1160 tests, OK**,
and the +16 is the peer's again: `tests/test_probe_identity_log_arms.py` was
written at 20:14 and runs **16** tests measured alone. Attributed, not banked.

The Python gate went RED twice during the session and both were mine:

1. `test_every_shipped_file_is_byte_identical_to_the_tree` — my header edit.
   Attributed by mtime (`IntelDisplay.hpp` written seconds earlier) and by the
   failure naming the payload comparison. Fixed by
   `python tools\build-step7-payload.py --skip-gate`.
2. `test_every_md5_the_card_quotes_can_be_accounted_for` — the rebuild I had
   just done made the operator card stale. Fixed by adding the thirty ninth
   image's fingerprints and demoting the thirty eighth's to superseded.

That second red is a small piece of good design worth noticing: the card is
required to still quote the OLD md5, so a reader holding an older note can tell
which image it described. The gate forbids the current image being **absent**,
not the old one being present.

**The thirty ninth image moved NEITHER byte size** — tar stayed 430080, ISO
stayed 507904 — while both md5s changed. `tar` rounds to 10240-byte blocks, so
a change that fits inside the existing padding ships different bytes at an
identical length. That is the fifth image running where a size comparison would
have reported no change.

---

## 7. What this does NOT establish

1. **Nothing here puts a picture on a panel.** This session moved evidence into
   a header. `IntelDisplayProbe` has still never run, and `handoff29` §7 item 1
   is unchanged: not one line of the Intel shell has executed anywhere.
2. **No value in either observed block is asserted to have been read twice.**
   `PLANE_SIZE` is the one constant measured on two separate boots, and it is
   the only reproducibility evidence this track has. The ten words here were
   read once each in one boot. They are on the capture's own safe-to-harvest
   list because the script's double-read subset did not catch them
   disagreeing — which is weaker than being confirmed stable, and the
   difference is not currently expressible anywhere in the tree.
3. **Five constants have no anchor and cannot get one.** `PP_STATUS` and the
   `BLC_PWM` family are almost certainly what four of those offsets are.
   "Almost certainly" is not a citation, and the offsets are what is defensible.
4. **The register gate has nothing to say about this block**, by construction.
   Its silence here is not coverage.
5. **The `intel-handoff29.md` §7 item 3 hole is untouched** — a second MMIO path
   not spelled as a volatile pointer load is still invisible to that gate.

---

## 8. Open list

1. **Scaffold active framebuffer output.** Unchanged, still the highest-value
   item, and still the only one that serves the operator's actual goal — a
   picture on the panel rather than more evidence about it. An `IOFramebuffer` /
   `IOGraphicsDevice` scanout handoff inheriting the mode UEFI already
   programmed. Everything on this track reads; nothing presents. Note that this
   is now the *whole* remaining reason the track exists: the harvest is done, so
   there is no longer an evidence task standing in front of it.
2. ~~**A capture-vs-header equality gate.**~~ **CLOSED in this session — see §9.** Every `IG_OBSERVED_*` value in
   `IntelDisplay.hpp` is transcribed by hand out of
   `igpu-results/tgl-final-capture-2026-08-18-201412.txt`, and **nothing checks
   the transcription**. A Python module in the idiom of
   `tests/test_igpu_mmio_ranges.py` could parse the capture's section tables and
   assert set-equality against the header's observed block, which would also
   redden if somebody edited the capture above its READING NOTES divider — the
   thing `tests/test_capture_integrity.py` guards by hash but only for the two
   filenames its glob matches (`intel-handoff28.md` §3 is why that glob is now
   pinned by a set-equality arm).
3. **The §7 item 3 hole**, inherited from handoff29 unchanged and still that
   session's own: deriving the MMIO set from the base pointer's use rather than
   from the cast's spelling.
4. **Nothing on this track is blocked on hardware.** Unchanged from handoff29
   §8 item 4, and now load-bearing in a new way — with the harvest closed, every
   remaining Intel question is either answered out of the two capture files or
   is not answered at all, and item 1 needs no capture.

---

## 9. §8 item 2, closed in the same session that filed it

`tests/test_capture_vs_header.py`, **10 tests**, and it exists because §1's table
was typed by hand. Nothing in the tree compared a single `IG_OBSERVED_*` value
against the capture it came from, and a transposed nibble is invisible to every
other gate: it compiles, it satisfies every pairwise-distinct arm in
`tests/test_intel_display.cpp`, and it reads as a witnessed value beside four
real ones. The `*_OBSERVED_*` convention was being enforced by **care** rather
than by a gate, which is `handoff17.md` §1's class — a claim described as
measured whose only evidence is that somebody typed it.

**The subject is split in two because the evidence is.** The five anchored words
appear in section 6's named rows (`NAME @0xOFFSET 0xVALUE`); the seven
offset-named ones appear **only** in section 1's raw dword dump. The raw parse
is bounded to section 1 rather than run over the whole file, because section 6
spells its offsets as `@0x…` and a whole-file pair scan would let a section 6
row satisfy a section 1 comparison — two different claims answered by one hit.

Three choices in it are load-bearing:

1. **Only text ABOVE the READING NOTES divider counts.** Everything below is
   host-side analysis added afterwards, so a value found there is not a
   measurement and must not be able to satisfy this gate. Control E moves
   section 6 below the divider and reddens 3.
2. **Every parse refuses on an empty result set** rather than reporting zero
   mismatches. `3050-handoff30.md` §2b's reasoning: a gate reporting nothing
   when its anchor moves is indistinguishable from a green one. Controls C, F
   and J are the three vacuity directions and each raises loudly.
3. **Offsets map to a LIST of readings, never a value.** The capture script read
   a subset twice and seven registers disagreed (§3), so a set of size ≠ 1 is
   the definition of a sample and the arm says so in its own failure message.
   Collapsing the list would hide exactly the thing §3 is about.

| Control | Reddened | Note |
|---|---|---|
| A — anchored `CDCLK_CTL` off by one nibble | **1** | the whole reason the module exists |
| B — `SKL_DFSM` set to all-ones | 2 | transcription **and** refusal arm, correctly |
| C — divider removed | 6 | refuses; cannot separate machine output from prose |
| D — `SFUSE_STRAP` row renamed | 2 | loud, not a silent pass over a smaller set |
| E — section 6 moved below the divider | 3 | those rows stop being machine output |
| F — every `@0x` rewritten | 6 | `ROW_RE` matches nothing; refuses |
| G — **unanchored** `C7204` off by one nibble | **1** | the arm that matters most |
| H — `0x0C8250` dropped from the dump | 2 | shrunken subject set reddens |
| I — the two backlight words made unequal | 1 | the §4 two-constants reasoning has changed |
| J — section 1 heading renamed | 4 | cannot bound its subject; refuses |

A and G are the discriminating pair, and **G is worth more than A**: the anchored
five could in principle be sanity-checked against `reference/i915/`, while the
seven offset-named words have no citable line in this tree at all, so the capture
is the only evidence they will ever have. One wrong nibble in either reddens
exactly one arm.

handoff18 §1e's every-pre-existing-check-green rule does **not** apply here —
this is an all-new module with no pre-existing checks to leave green — so what
the ten controls establish instead is the pair of properties that rule is a proxy
for: each mutation reddens the arm that names its own defect, and each of the
three ways of erasing the subject produces a REFUSAL rather than a pass.

Every mutation ran against **temp copies** with `CAPTURE` and `HEADER`
repointed. The capture is unrepeatable; the live file in `igpu-results/` was
opened read-only and never written, and the control script lives outside the
checkout for the same reason.

**Gate readings after this section:** Python **exit 0, 1183 tests, OK**. The
gate was run five times and every delta is attributed rather than banked —
1144 → 1160 is the peer's `tests/test_probe_identity_log_arms.py` (16 tests,
written 20:14, measured alone), and 1160 → 1166 → 1170 → 1177 → **1183** is this
module growing 6 → 10 → 17 → **23**, each reading taken by running the module
alone before the gate. `phase0-kext/` is **BYTE-IDENTICAL** to what §1 left, so
`build-payload/` was NOT rebuilt and the thirty ninth image is still current —
the payload gate's silence is the proof rather than an assertion.

### 9b. Items 1 and 2 of §9a, closed in the same session that filed them

The module went 10 → 23 tests and the two holes §9a named are gone. Filing them
as open and then closing them one turn later is worth one sentence rather than a
silent overwrite: an item written from the shape of the work just done is a
claim with an age from the moment it is typed, and `3050-handoff29.md` §1 is what
happens when nobody re-measures one.

**Item 1 — the timing/plane block.** Seventeen more constants compared, and the
way they had to be reached is the durable half. Section 2 and section 3 spell
their words as a base plus a delta, but section 1's raw dump also carries them at
their absolute offsets, so those resolve through the SAME parse. The five plane
words do not: `0x07018C` and `0x0701A4` are **absent from section 1 entirely**
and appear only inside section 4's own delta block. So `PLANE` is keyed by delta
with the base READ out of the capture rather than assumed, and a plane that moved
reddens `test_the_plane_base_is_where_the_header_believes` instead of silently
comparing the wrong register (control M). That was measured rather than reasoned:
a first table spelling all five as absolute offsets reddened the presence arm.

**The one real defect this section found was in my own gate, not in the header.**
`DEFINE_RE` ended in `$`, and six timing defines carry an inline `// 1919 / 2079`
comment — so the pattern matched **none of them**, and
`test_no_header_constant_holds_a_refusal_value` had been sweeping a subject set
**six smaller than it appeared to be** while printing green. That is
`intel-handoff28.md` §3's defect one file over: an anchor that quietly narrows its
own subject converts absent coverage into a positive-looking result. It surfaced
only because a wrong table forced the presence arm to enumerate what the regex
actually found. Control Q strips every inline comment and requires the whole
suite to stay **GREEN**, so the pattern cannot become prose-sensitive again.

Two table rows were also mine and wrong from memory rather than from the file:
pipe MISC2 is at **`+0x02C`** and MISC at `+0x030` — the reverse of the order the
names suggest — and the two `_TGL_DPLL0_CFGCR*` words were simply missed, being
ordinary named section 6 rows needing no exemption at all.

**Item 2 — the subject set is now DERIVED.** `test_every_observed_macro_is_compared_or_exempt`
subtracts the union of the five tables plus `EXEMPT` from the header's own
`IG_OBSERVED_*` names and requires the difference to be empty, so a thirty
seventh constant reddens on the commit that adds it **and names itself** (control
R). `intel-handoff26.md` §2a's reasoning, reached for a different family.

`EXEMPT` is the part to be suspicious of, because an exemption list is how a
derived set arm gets talked back into a spelled one. It holds **four** entries and
every one carries the reason it has no single dword to compare against — two DSI
base ADDRESSES, a STRIDE that is a gap between two addresses, and
`IG_OBSERVED_UNPROGRAMMED_2026_08_18`, which is a value PATTERN measured across
transcoders B/C/D and pipes B/C/D on two boots rather than one register's
reading. Pinning that last one to a single offset would misrepresent what was
measured. Two arms stop the list rotting: an exemption naming a macro the header
lost is a REFUSAL (control V), and a macro that is **both** compared and exempt
reddens (control W) — otherwise the set arm is satisfiable without the comparison,
which is this module's own subject one level up.

The three exempted DSI constants are not left unchecked, they are checked as the
LAYOUT claim they actually are: each base must have at least one register dumped
inside its page (control T moves DSI1 to a page nothing was read in), the stride
must equal the arithmetic gap between the two bases (control S), and
`TRANS_DDI_FUNC_CTL` is compared at **BOTH** `0x06B400` and `0x06BC00` — one macro
at two offsets on purpose, because the header's claim is that both pages hold
that word and comparing one page would ratify half the sentence (control U drops
the second and reddens).

**Twenty-three controls, all against temp copies.** Each reddens the arm naming
its own defect; the baseline reddens nothing; the six ways of erasing a subject
set (C, F, J, N, O, plus V) all REFUSE rather than pass. The live capture was
opened read-only throughout and the control script lives outside the checkout.

What §9a items 3 and 4 said is unchanged and still true: this gate proves
transcription, not harvestability — nothing here asserts a pinned value came from
a configuration register rather than a status one — and it still shares
`igpu-results/` with `tests/test_capture_integrity.py` for a different property
with nothing relating the two.

### 9a. What this gate does NOT cover

1. ~~**The timing/plane block above is untouched by it.**~~ **CLOSED — see §9b.** Only §1's twelve
   configuration words are compared. The thirteen `IG_OBSERVED_TRANS_A_*` /
   `PLANE_*` / DSI constants are still hand transcriptions nothing checks, and
   they are the ones an actual mode-set will depend on. That is the cheapest
   remaining work on this track and it is a table extension, not a design.
2. ~~**The subject set is SPELLED, not derived.**~~ **CLOSED — see §9b.** `ANCHORED` and `UNANCHORED` name
   twelve macros; a thirteenth `IG_OBSERVED_*` added later is compared by
   nothing and the gate stays green. `intel-handoff26.md` §2a's derive-the-
   subject-set reasoning is the fix — assert set-equality between the header's
   `IG_OBSERVED_*` names and the union of the two tables — and it is deliberately
   not in this first version, which is item 1's natural companion.
3. **It proves transcription, not harvestability.** Nothing here asserts that a
   value pinned as observed came from a *configuration* register rather than a
   status one. §3's disqualifications remain prose plus the two
   `igReadRefused()` assertions in `tests/test_intel_display.cpp`.
4. **`tests/test_capture_integrity.py` and this module now guard the same file
   for different properties** — that one hashes it, this one reads it — and
   nothing relates the two. A capture replaced wholesale reddens the hash arm
   first, which is the correct order, but by accident rather than by
   construction.

---

## 10. The limit convention was described backwards in five places, and IG_SCAN_ had no set-check at all

This section is a continuation of the same session, working the one remaining
item — scaffolding active framebuffer output. It did not get there, and the
reason is worth more than the item: the measurement taken *before* writing any
scanout code found that the tree's own prose about verdict limits is false, and
that the family a framebuffer handoff would depend on is the only Intel verdict
family with no register-gate coverage.

### 10a. What the scanout layer already had

Measured first, because `intel-handoff29.md` §1's rule is that an item inherited
across handoffs is a claim with an age. "Scaffold scanout" was already largely
done:

- `phase0-kext/IntelScanout.hpp` exists, is dependency-free, and turns an
  inherited plane configuration into the three numbers `IOFramebuffer` needs.
- `tests/test_intel_scanout.cpp` drives it; the C++ gate reports **10** suites.
- `IntelDisplayProbe.cpp` consumes it and publishes `1-Scan`, `1-ScanUsable`
  and `1-ScanExtent`.
- `tests/test_intel_publisher_scan_shape.py` already pins the publication shape,
  including that `1-ScanExtent` is GUARDED by `igScanUsable(` while the other two
  are published on both arms.
- `igScanVerdictName()` landed in `IntelDisplayNames.hpp` earlier the same day,
  with `testScanNames()` wired into `main()`.

So the honest statement of the remaining gap is narrower than the open list said:
the descriptor exists, is tested, is consumed and is shape-gated. What does not
exist is an `IOFramebuffer` subclass consuming the descriptor.

### 10b. Five comments said the convention was inconsistent. It never was.

**This is the finding.** `IntelScanout.hpp` carried a paragraph warning that its
limit is INCLUSIVE and "the OPPOSITE of `IG_MODE_VERDICT_LIMIT` and
`IG_FB_INHERIT_VERDICT_LIMIT`, which are exclusive." That sentence is false, and
it had been copied into four more places.

Measured against the defines rather than against memory:

| family | limit | largest member | |
|---|---|---|---|
| `IG_MODE_` | 6 | `IG_MODE_SRC_MISMATCH` 6 | inclusive |
| `IG_FB_INHERIT_` | 5 | `IG_FB_INHERIT_GEOMETRY` 5 | inclusive |
| `IG_READ_` | 4 | `IG_READ_PAST_MAPPING` 4 | inclusive |
| `IG_SCAN_` | 6 | `IG_SCAN_ERR_OUTSIDE_APERTURE` 6 | inclusive |

Every family spells its bound `// the largest verdict above`, and
`tools/check-registers.py:2440` compares `limit` against `max(codes)` as an
**EQUALITY**, per family — so a family carrying an exclusive limit could not pass
the register gate at all. The convention is uniform and always has been.

Why this is a defect rather than a typo: **it is handoff20 §1 in prose form.**
That defect was `igModeVerdictName()` carrying arms 0..5 against an INCLUSIVE
limit of 6, so `IG_MODE_SRC_MISMATCH` — the verdict naming the byte-swapped
`PLANE_SIZE`/`PIPESRC` shape `handoff15.md` §1 measured on silicon — published
the unknown string with all four gates green. A comment that gets the convention
backwards hands the next author that exact bug: they write `<` over an
inclusive limit, and the family's last arm loses its name silently. The
`IntelScanout.hpp` paragraph even said "That inconsistency is not mine to fix in
this file" — deferring a fix for an inconsistency that does not exist.

Worse, `tests/test_intel_display_names.cpp` claimed the `<=` walk "absorbs" the
exclusive families "by having a LIMIT+1 arm that is genuinely unnamed." Both
halves are false. The walk is `<=` for every family *because every limit is
inclusive*; what the LIMIT+1 assertion pins is the **sentinel**, one past the
last real verdict. That is a green test whose stated reason for being correct is
wrong — the class `3050-handoff26.md` §3 names, where an assertion's message and
its behaviour disagree.

Four sites corrected, each now carrying the measurement rather than the memory:
`phase0-kext/IntelScanout.hpp`, `phase0-kext/IntelDisplayNames.hpp`,
`tests/test_intel_display_names.cpp`, `tests/test_check_count_floor.py`. The
fifth is inside a `>`-quoted superseded image entry in
`OPERATOR-ACTION-step7-build.md` and was **deliberately not rewritten** — that
block is a chronological record, same rule as `intel-handoff28.md` §7 item 5.

### 10c. The largest Intel verdict family had no set-check

The register gate parses `IntelDisplay.hpp`, `IntelFramebuffer.hpp` and
`IntelGpuId.hpp`, and gives seven verdict families a derived set-check —
`IG_MODE_`, `IG_LINK_`, `IG_USABLE_`, `IG_JOIN_`, `IG_READ_`, `IG_SAMPLE_`,
`IG_FB_INHERIT_`. It did **not** parse `IntelScanout.hpp` at all. `IG_SCAN_`, at
seven arms the LARGEST family in the track and the one that decides whether a
firmware-programmed plane can be handed to `IOFramebuffer` at all, was pinned
only by hand-kept literals in `tests/test_intel_scanout.cpp` — so an eighth
refusal reason added without a matching line is a refusal the caller cannot tell
from one it already handles.

**Why it was skipped is the interesting half, and it is `handoff16.md` §1's trap.**
A bare `intel_verdict_set_checks(..., "IG_SCAN_", 7, ...)` line does not work,
because the prefix also covers five non-verdicts: `IG_SCAN_INVALID32`,
`IG_SCAN_INVALID64`, `IG_SCAN_EXTENT_POLICY_CEILING`, `IG_SCAN_SURF_ADDR_MASK`
and `IG_SCAN_SURF_ALIGN_BYTES`. `max(codes)` becomes `0xFFFFFFFFFFFFFFFF` and
the equality fails while being *wrong about the world* — verbatim handoff16's
`IG_PIPE_TRANSCONF`, where a verdict family swept in a register offset and
printed a contiguity error correct about its own arithmetic. So the checker
gained an `exclude` parameter and a `header` label, and two choices in it are
load-bearing:

- The exclusion is applied **BEFORE** `resolve()`, not filtered after it.
  `IG_SCAN_INVALID64` is `0xFFFFFFFFFFFFFFFFull`, and resolving a constant this
  arm is not comparing would let a suffix the parser cannot read fail a whole
  verdict family for a reason having nothing to do with any verdict in it.
- A **stale exclusion is a FAILURE**, not a silent no-op. A name the caller lists
  and the header does not carry means the sweep quietly stopped excluding
  something, and an exclusion list that silently shrinks is how a real verdict
  later slips OUT of the compared set — the direction that reads green while
  covering less. Same reasoning as `EXEMPT`'s stale-name arm in §9b's own gate,
  one tool over.

The five are spelled **by name** rather than filtered by value on purpose. A
value-based rule ("skip anything above the limit") would also skip a real eighth
verdict somebody added above a stale bound — precisely the case this arm exists
to catch.

Register gate **240 → 241**.

### 10d. Three controls, each reddening one arm and naming its own defect

Run against the live tree and reverted immediately; the gate is a pure reader, so
there is no artifact to protect the way §9's capture needed.

| control | result |
|---|---|
| `IG_SCAN_VERDICT_LIMIT` 6 → 7 | 1 failure, naming the constant: *is 7, but the largest verdict is 6 (`IG_SCAN_ERR_OUTSIDE_APERTURE`)* |
| drop `IG_SCAN_INVALID64` from `exclude` | 1 failure: *the largest verdict is 18446744073709551615 (`IG_SCAN_INVALID64`)* — the trap, reproduced on demand |
| add `IG_SCAN_GONE_FROM_HEADER` to `exclude` | 1 failure: *is named as a non-verdict exclusion … but does not exist in IntelScanout.hpp* |

The middle one is the one that matters: it proves the exclusion list is
load-bearing rather than decorative, and it is the only one of the three that
reproduces the reason this family had no set-check for two days. Green restored
at 241/241 after each revert.

### 10e. Three Python readings, three verdicts, and I changed nothing between the last two

Final readings, all four gates:

- Register gate: **241/241**, 0 failed, 0 skipped.
- Syntax gate: exit **0**, zero `FAIL:` hits (read the exit code, not
  `VERDICT: PASS`).
- C++ gate: exit **0**, **10/10** suites.
- Python gate: exit **0**, **1183** tests, **OK**.

That last line is a CORRECTION of what this section first said, and the
correction is worth more than the reading. Across three runs the Python gate
went RED, RED on a completely DIFFERENT pair, then GREEN — and nothing I did
caused the last transition. In order, because the final reading is the least
informative of the three:

1. **Run one, 2 failures.** `test_check_count_floor` (`test_decode` printing
   **148** against a MEASURED entry of 121 — the peer's `NVDecode.hpp` at
   21:14:15 and `test_decode.cpp` at 21:14:22, landed between my two edit
   batches) plus `test_probe_identity_publication`, a negative control over the
   peer's `NVBoot42Readable` publication in `NV3050Probe.cpp` (20:57) against a
   test module untouched since 2026-08-18. The count-floor red cleared itself at
   21:27:33 when the peer landed their own table entry.
2. **Run two, 2 failures, a DIFFERENT pair.** Both in
   `test_operator_step7_doc`: *the card does not mention kext-src.tar's real
   size*, and *the live ISO's md5 must be among the md5s the card prints*. That
   reads exactly like a stale card, and the tempting action is to go and edit the
   card. **Measure first.** `OPERATOR-ACTION-step7-build.md:205`-`:213` already
   quotes `440320`, `518144`, `80586918574534f3709c9ed054f60d72` and
   `a04fea290678d76c8d0fe05a7eb62069` — precisely the live values — and running
   that module alone gives **25 tests, OK**.
3. **Run three, exit 0, 1183 tests, OK.**

What actually happened: the peer rebuilt `build-payload/` at **21:36:00** — the
FORTIETH image, tar 430080 → 440320 and ISO 507904 → 518144, one 10240-byte tar
block each — and run two caught the tree with the payload already rewritten and
the card not yet updated. This is
`mid-write-snapshot-reads-as-false-green` in its inverted direction, which
is the direction that costs you work: for a GATE, a peer's half-landed change
reads as a false **RED** whose failure message names a real file and sounds
correct. Editing the card in run two would have "fixed" a document that was
already right, on the authority of a measurement taken mid-write. **The cheap
discriminator is running the failing module alone**, which re-reads the tree a
second time and takes 17 ms.

**`build-payload/` was never rebuilt by me**, which was right for its original
reason and stays right. At the point I considered it the stale list held **four**
files — `IntelDisplayNames.hpp` and `IntelScanout.hpp` (mine, comment-only)
beside `NV3050Probe.cpp` and `NVDecode.hpp` (the peer's, mid-flight) — so a
rebuild under this session's name would have tarred their half-landed NVIDIA
work in order to green a gate over my prose, exactly what `intel-handoff27.md`
§5a refused. The peer rebuilt it themselves once their own files settled; the
fortieth image and its card entry are theirs, not mine. `handoff18.md` §3's rule,
three times in one session: a gate's verdict in a two-agent tree is a JOINT
measurement — red does not mean you broke it and green does not mean you fixed
it.

### 10f. What is still open

1. **The scanout consumer.** Still the only real item, and now the *only* one:
   `igScanDescribe()` produces the three numbers, `IntelDisplayProbe` publishes
   them, and nothing subclasses `IOFramebuffer` to present them. Everything on
   this track reads; nothing presents.
2. **`IntelScanout.hpp` is parsed for its verdict family only.** The five
   sentinels and masks are now excluded by name from the set-check, and no arm
   checks them against anything else — `IG_SCAN_SURF_ADDR_MASK` in particular is
   a claim about hardware with no i915 cross-check, unlike the offsets in
   `IntelDisplay.hpp`.
3. **Nothing gates the limit convention itself.** `check-registers.py:2440`
   enforces `limit == max(codes)` per family, which is what makes the convention
   uniform — but no test asserts that the *comments* describing it are true, and
   §10b is what happens when they are not. A prose gate here would be the
   `tools/check-documented-paths.py` idiom applied to a numeric claim.
4. `intel-handoff29.md` §7 item 3 / §8 item 3 — derive the MMIO set from the base
   pointer's *use* rather than the cast's spelling, so a second accessor is not
   invisible. Unchanged.
