# handoff16 — four joins, and the one that named itself into a collision

Newest handoff. Read `handoff15.md` for the state this continues from, and
`CLAUDE.md` for the four verification gates.

This session did not touch the NVIDIA path. It is the fourth session on the
Intel track, and it added no new evidence — the 2026-08-16 Tiger Lake capture is
still the only silicon this track has ever seen, and there is still no second
run of that boot. What moved is the *joining* of constants that already existed:
four new decode layers in `phase0-kext/IntelDisplay.hpp`, each answering a
question that individually-correct decoders could not.

All four gates are green as of the last run:

```
python tools/check-registers.py     238/238 passed, 0 failed, 0 skipped
tests\build-msvc.bat                8/8 suites Passed
tests\check-kext-syntax.bat         VERDICT: PASS
tests\run-python-tests.bat          Ran 851 tests — OK
```

Measured per-suite check counts, 2026-08-17, by running each binary directly:

| suite | checks |
|---|---|
| `test_decode` | 64 |
| `test_falcon` | 169 |
| `test_riscv_ucode` | 116 |
| `test_gsp_boot` | 217 |
| `test_gsp_stage` | 181 |
| `test_intel_display` | **354** |
| `test_intel_gpu_id` | 77 |
| `test_intel_framebuffer` | **110** |

`test_intel_display` moved 262 → 354 across this session. `MEASURED` in
`tests/test_check_count_floor.py` was bumped four times to follow it (319, 334,
353, 354); each bump has a dated provenance line.

---

## §1 — the one to read: a verdict family named itself into a collision

The whole-pipe join was originally `IG_PIPE_OK` / `IG_PIPE_LINK` /
`IG_PIPE_TIMING` / `IG_PIPE_PLANE` / `IG_PIPE_VERDICT_LIMIT`. The register gate
groups verdict families **by prefix** — `intel_verdict_set_checks(rep, intel,
prefix, expected, owner)` sweeps every `#define` starting with that string and
asserts distinctness, `OK == 0`, contiguity `0..n-1`, and `VERDICT_LIMIT == max`.

`IG_PIPE_TRANSCONF 0x08u` has existed at `IntelDisplay.hpp:140` since the
first Intel session. It is a **register offset inside the transcoder page**. The
prefix scan swept it in, and the gate reported:

```
FAIL  IG_PIPE_VERDICT_LIMIT  is 3, but the largest verdict is 8
      (IG_PIPE_TRANSCONF) -- a hand-spelled derived number that has fallen
      behind the set it describes
```

That message is *correct about its own arithmetic and wrong about the world*.
Nothing had fallen behind; a register offset had been mistaken for a verdict.
The family is now `IG_USABLE_`, and a comment above the defines records why it
is not `IG_PIPE_`.

**What this means for the next agent.** A prefix-grouped checker is a naming
contract, not just a checker. Before adding a verdict family, grep the header
for that prefix — `grep -n "^#define IG_<PREFIX>" phase0-kext/IntelDisplay.hpp`
— because the failure mode is a message that names a real constant, quotes a
real number, and describes a defect that does not exist. The gate caught this on
the first run, which is the system working; but it caught it by accusing the
wrong line.

**And the rename was incomplete on the first pass.** Editing only the five
`#define`s left `igPipeUsable()`'s three `return IG_PIPE_*` statements behind,
which MSVC caught as four `C2065: undeclared identifier`. A verdict rename
touches three places, not one: the defines, the function body, and the suite.

The collision itself is now pinned in `tests/test_intel_display.cpp`:

```cpp
checkU32("IG_PIPE_TRANSCONF is a register offset, not a verdict",
         IG_PIPE_TRANSCONF, 0x08u);
```

If somebody renames the family back to `IG_PIPE_`, that line is what goes red
first, and its label says what the actual problem is.

---

## §2 — the four layers, and why each one is a join rather than a decoder

Every layer added this session takes decoders that already worked and asserts a
relationship *between* them. That is the shape, because on this silicon the
individual decodes were already right and the half-answers were the hazard.

**Hazard 6 — whole-mode consistency.** `igAxisConsistent(total, active,
blankStart, blankEnd, syncStart, syncEnd)` takes **already-decoded** numbers, no
`+1` arithmetic inside. That is deliberate: if the function did its own decode, a
convention bug and a comparison bug would be indistinguishable in the failure.
`igModeConsistent()` above it takes **raw** words in MMIO order and runs H, then
V, then PIPESRC last. Verdicts `IG_MODE_*`, 0..6.

The check order inside `igAxisConsistent` is load-bearing: plausibility → blank
range → blank overlap → sync range → sync containment. The blank-overlap test is
`blankStart < active` — a strict `<` against a `>=` bound — because i915
programs `HBLANK_START = hactive - 1`.

**Hazard 7 — plane/raster join.** `igPlaneMatchesPipe(planeWidth, planeHeight,
pipeSrcWord)`, verdicts `IG_JOIN_*` 0..3, degenerate case checked **first**
because a 1×1 plane is what an unprogrammed page decodes to.

**Hazard 8 — `TRANS_DDI_FUNC_CTL`.** Grounded on `i915_reg.h:3741-3747`:
subtracting `igTransBase()` from each of the six DDI_FUNC_CTL addresses gives
`0x400` six times, DSI1 included (`0x6B800 + 0x400 == 0x6BC00`).
`_TRANS_DDI_FUNC_CTL_EDP 0x6F400` is deliberately excluded, matching
`igTransBase()`.

The port field is the trap. Pre-icl is shift **28** / mask **7**; Gen12 is shift
**27** / mask **0xF**. The wrong shift yields *a different valid port*, not
garbage. And the field is **+1 encoded** — `TGL_TRANS_DDI_SELECT_PORT(x) =
((x)+1) << 27` — so a stored **0 means NO PORT**, not port A. `igDdiPortField()`
returns the raw field with the +1 intact and `igDdiPort()` returns
`IG_DDI_PORT_NONE` for zero, split so a test can pin the read separately from
the decode.

**Hazard 9 — transcoder/DDI join.** `igTransconfDdiLinked(transconfWord,
ddiWord)`, verdicts `IG_LINK_*` 0..3. TRANSCONF says a transcoder is *timing*;
TRANS_DDI_FUNC_CTL says its output is *connected*. All four combinations are
reachable and none of them fault. The two middle ones are the dangerous pair —
each looks half-configured, and a driver reading only one register concludes the
display is usable.

**Hazard 10 — the whole-pipe join.** `igPipeUsable()` takes ten raw words and
collapses the three joins to one verdict, `IG_USABLE_*` 0..3. Order is causal:
link → timing → plane. A dead transcoder makes the timing and plane verdicts
describe *stale register contents*, so reporting `IG_USABLE_TIMING` for an
unprogrammed pipe would name a symptom of the pages reading `0x00000000` rather
than the fact that nothing is programmed. Three checks in the suite pin exactly
that — when two or three stages would refuse, the **earliest** must win.

---

## §3 — the placement rule that cost a compile error

The hazard-9 block was first written *above* `igTransconfLive()`'s definition.
These headers have **no includes and therefore no forward declarations**, so a
join placed above any one of the decoders it calls is a compile error rather
than a warning. It was moved below line 490 and a comment now says why it sits
there.

Same rule put `igPipeUsable()` at the **tail of the file**, after
`igPlaneSizeWidth/Height` at 1009/1014 — it is the only function that calls
across all four families.

When adding a join to `IntelDisplay.hpp`: `grep -n "^static inline .*ig<Name>"`
for every callee first, and place the new block below the highest line number.

---

## §4 — the test rasters are 1280×800 on purpose

Do not "fix" this to the captured 1920×1080.

PLANE_SIZE and PIPESRC are **byte-swapped halves of one another** — the
2026-08-16 capture read `0x077F0437` and `0x0437077F` for the same mode on the
same pipe. A decoder that reused one half-order for the other returns a
plausible number with the axes **transposed**, and on a square-ish mode that
still programs cleanly and every test passes.

So the fixtures are 1280×800 @ 1680×831 (`hTot 0x068F04FF`, `hSyn 0x05C7053F`,
`vTot 0x033E031F`, `vSyn 0x03250322`, `src 0x04FF031F`, `psz 0x031F04FF`), with
the captured panel kept as a **second** raster so the measured values stay
exercised. `testPipeUsable()` includes the transpose explicitly: feeding
PIPESRC's own word in as PLANE_SIZE decodes to 800×1280 and must return
`IG_USABLE_PLANE`.

Two timing words were hand-computed wrong during this session and caught by the
suite: `0x07FF` is **2048**, not 1920, and `0x043F` is **1088**, not 1080. The
captured-panel raster is now `0x0897077F` / `0x080307D7` / `0x04640437` /
`0x0440043B` / PIPESRC `0x077F0437`.

---

## §5 — gate work, and one dead assertion removed

`tools/check-registers.py` grew from 219 to 238 checks.

**`intel_verdict_set_checks()`** is the original `intel_mode_verdict_checks()`
parameterised. It is now wired four times — `IG_MODE_` (7), `IG_JOIN_` (4),
`IG_LINK_` (4), `IG_USABLE_` (4) — plus `IG_FB_INHERIT_` (6) against
`IntelFramebuffer.hpp`. The `expected` floor **`rep.failed()`s**, never skips: a
`skip` prints as a pass, so a family that silently shrank had to be a red.

**`intel_ddi_checks(rep, kext, src)`** derives the six DDI constants the row
table cannot express. Important discovery, measured 2026-08-17: **i915's DDI
fields are composed shifts, not `REG_GENMASK`** — `i915_genmask()` returns
`None` for all eleven DDI names, so ordinary `"mask"`/`"shift"` rows would have
**silently skipped**. It also carries a non-vacuity guard that fails if upstream
ever unifies `TGL_TRANS_DDI_PORT_SHIFT` with `TRANS_DDI_PORT_SHIFT`, and it
derives the field mask through the **kext's own** shift so a mutation on either
side reddens.

**`intel_cross_header_checks()`** had a real bug: the `a != d` comparison was
**string equality** on raw `#define` bodies, so `0x00u` and `0x0` would have
falsely reported disagreement. Both sides now route through `resolve()`. It
surfaced as a `TypeError` on `f"0x{limit:02X}"`.

**Section 3b** recomputes `IG_FB_GGTT_POLICY_LIMIT` from
`IG_FB_SURF_ADDR_MASK`: `page = (~mask & 0xFFFFFFFF) + 1`, then `limit == mask +
page` as **equality**, plus a `page != 4096` guard.

**`IntelFramebuffer.hpp`** gained `igFbSurfaceRangeOk()`, mirroring the NVIDIA
`nvGspStageRangeOk()` fix — the base was bounded and `base + bytes` was not.
`IG_FB_GGTT_POLICY_LIMIT 0x100000000ull` is documented as a **POLICY CEILING**
like `NV_GSP_FW_ADDR_BITS`, not a measured GGTT size: it is the largest address
`PLANE_SURF`'s 20-bit page field can name. A dead `height != 0u` conjunct was
also removed from `igFbSurfaceBytes()` — `igFbSizeHeight()` returns 1..65536, so
the false branch was unreachable by construction, and a comment says so rather
than leaving a reader to re-derive it.

One test expectation of mine was wrong and the code was right:
`igFbSurfaceRangeOk(0u, 0xFFFFFFFFu)` is legitimately **true** — `0 + 4294967295`
is one byte below 2³². Replaced with a genuinely separating input,
`igFbSurfaceRangeOk(0x00001000u, 0xFFFFFFFFu)`, where the 32-bit sum wraps to
`0xFFF`.

---

## §6 — non-vacuity probes, all in %TEMP%

Five in-memory mutation probes were written this session, all currently at
**0 mutants survived**:

| probe | cases | what it mutates |
|---|---|---|
| `probe-cross-header.py` | 11 | the two address/offset dicts |
| `probe-mode-verdicts.py` | 27 | both verdict families + cross-family leakage |
| `probe-ggtt-limit.py` | 11 | label-matched, arm lives in a multi-arm function |
| `probe-ddi-resolve.py` | survey | what the resolvers can actually evaluate |
| `probe-ddi-checks.py` | ~24 | **both** the kext and the vendored i915 table |
| `probe-link-verdicts.py` | 11 | the `IG_LINK_` family, incl. total absence |

They are **not in the working tree** and must not be. A probe that plants
defects in the tree makes a concurrent gate run report named FAILs with real
tracebacks and no defect. Every probe mutates **parsed dicts in memory** and
asserts the **verdict word** (`pass` / `fail` / `silent`), never an exit code.

`probe-ddi-checks.py` is the one worth reading if you add a derivation. It
mutates the vendored table as well as the kext, because a derivation that only
reddens when the kext moves is really just comparing a literal to itself.

---

## §7 — the payload treadmill: nineteen rebuilds

`tests/test_line_endings.py`'s sibling
`test_every_shipped_file_is_byte_identical_to_the_tree` compares
`build-payload/kext-src.tar` byte-for-byte against the live tree. **Every edit to
a `phase0-kext/` header reddens the Python gate.** The fix is always:

```sh
python tools/build-step7-payload.py --skip-gate
```

then update the md5s in `OPERATOR-ACTION-step7-build.md`. That happened **six
times this session** (rebuilds 14 through 19).

Current image, after the NINETEENTH rebuild, measured 2026-08-17:

| artifact | bytes | md5 |
|---|---|---|
| `step7-payload.iso` | 364544 | `bb6197dec2cb5a121879deb7e4c47a32` |
| `kext-src.tar` | 286720 | `17ea88f705ad119daf2ca70a7da7d7ba` |
| `guest-build-kext.sh` | 12677 | `599b51eb31500c9ef9ff4b1f2dee0476` |

**Read the md5, not the size.** The eighteenth and nineteenth rebuilds both held
both sizes fixed while both md5s moved — tar pads to a 10240-byte boundary, so
content can change with size constant. The card's cause paragraphs for rebuilds
12-19 deliberately contrast the runs where sizes held against the runs where
they grew, which is the argument for why the size line is only a weak
corroborator.

---

## §8 — what is still open, honestly

**Nothing here has run on Intel hardware.** The kext compiled and linked under
Apple clang 17.0.0 on 2026-08-14 (`step7-results/step7-build-SUCCESS.log`), but
it was never `kextload`ed, the guest has no NVIDIA card, and the Intel track has
**no IOKit shell at all** — `IntelDisplay.hpp` and `IntelFramebuffer.hpp` are
pure logic driven only by `tests/*.cpp` under MSVC. There is no
`IntelDisplay.cpp`, no matching dictionary, no `IOService` subclass.

The highest-value open items, in order:

1. **An IOKit shell for the Intel track.** Everything decode-side now exists and
   is gated; nothing consumes it. This is the first item that would make the
   Intel work a *driver* rather than a library.
2. **The DSI pages and the `0x800` stride (handoff15 Q1/Q2), and plane count
   (Q3), remain unanswerable** on an eDP-only machine. They are not merely
   unanswered. Do not file work that assumes a second capture is available.
3. **Watermarks / `PLANE_WM`.** The plane can be sized and addressed and still
   not scan out. Nothing in this tree touches watermarks, and they are the next
   real hazard on the inheritance path.
4. **`IG_FB_GGTT_POLICY_LIMIT` is a policy ceiling, not a measured GGTT size.**
   If a second capture ever happens, this is one of the cheapest things to
   settle.

**Read every `ABSENT` in the 2026-08-16 capture as `UNPROGRAMMED`.** Unused
display pages on this silicon read `0x00000000`, not all-ones, which made the
capture script print `ABSENT` for transcoders B/C/D that certainly exist.
Nothing above the READING NOTES divider in
`igpu-results/tgl-capture-2026-08-16-022556.txt` may be edited.

---

## §9 — three habits that paid off, for whoever is next

**Small edits, one per turn.** A large `Write` followed by incremental `Edit`s
beat trying to land a whole layer at once — every compile error this session was
localised to the block just added.

**Assert the verdict word, never the exit code.** This applies to probes, to
gate runs, and to pipelines: `$?` after a pipe is the *last* command's status.
Gate runs here redirect to a `%TEMP%` log, `echo EXIT=$?`, then grep the verdict
lines.

**Pin both edges of every bound.** Containment is one-sided — loosening a limit
in the permissive direction reddens nothing, because every input stays inside.
Where a bound is derivable, assert it as **equality** and mutate outward as well
as inward.
