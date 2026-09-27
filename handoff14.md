# handoff14 — the Gen12 plane layer landed, and the register gate is blind to all of it

Newest handoff. Read `handoff13.md` for the state this continues from, and
`CLAUDE.md` for the four verification gates.

This session did **not** touch the NVIDIA path. It is the second session on a
new track: a Gen12 (Tiger Lake) Intel display layer, requested explicitly by the
operator after being told the ordering is backwards on difficulty. That decision
stands and is not up for re-litigation — see §6 for the honest distance to a
working driver on either path, because the answer is the thing most likely to be
misread from the green gate output below.

---

## 1. Gate state, measured at the end of this session

All four gates were run. Verdict words, not exit codes:

```
python tools/check-registers.py     114/114 passed, 0 failed, 0 skipped   exit 0
tests\build-msvc.bat                (six suites, counts below)            exit 0
tests\check-kext-syntax.bat         === ALL SYNTAX GATES PASSED ===       exit 0
tests\run-python-tests.bat          Ran 788 tests ... OK                  exit 0
```

The six C++ suites, each run directly and its final line read:

```
test_decode          64/64 passed, 0 failed
test_falcon        169/169 passed, 0 failed
test_riscv_ucode     116 checks, 0 failures
test_gsp_boot        217 checks, 0 failures
test_gsp_stage       181 checks, 0 failures
test_intel_display   205 checks, 0 failures
```

`test_intel_display` was 144 at the start of this session. The 61 new checks are
the plane layer in §2.

## 2. What was added: `IntelDisplay.hpp`'s plane layer

`phase0-kext/IntelDisplay.hpp` grew from 369 to ~530 lines. The addition is
`igPlaneReg()`, the `PLANE_CTL` field decodes, and the raw `PLANE_SIZE`
accessors. Everything is derived from `reference/i915/` at Linux v6.12 with the
line number in the comment — no value came from a dump, and nothing was invented
to make a test pass.

The interesting part is that plane addressing is the **exact inverse** of
transcoder addressing, and both are hazards for opposite reasons:

- **Transcoders must not be computed.** `DSI1 - DSI0` is `0x800` while `D - C`
  is `0x1000`. Any stride formula that reproduces A..D puts DSI1 at `0x6C000`,
  which is a real address in a different block. `igTransBase()` is a switch.
- **Planes genuinely are computed.** `_SKL_PLANE` bottoms out in `_PICK_EVEN`,
  and all five four-corner quads the vendored header spells out
  (CTL/STRIDE/POS/SIZE/KEYVAL) give plane delta `0x100` and pipe delta `0x1000`,
  uniformly. So `igPlaneReg()` multiplies.

The consequence is why this is filed as a hazard rather than a convenience: the
DSI pipes sit at `0x7B000`/`0x7B800`, which are **not** on the `0x1000` ladder
from `0x70180`. A DSI index handed to a plane helper yields `0x74180` — inside
the display block, real, writable, and nothing to do with the plane the caller
asked for. It does not fault. `igPlaneReg()` refuses a DSI index rather than
computing it, and `testPlaneRefusals()` pins that the returned value is the
invalid sentinel and explicitly **not** `0x74180`.

Two things in that layer are deliberately **unsettled** and named as such, in
the `NV_GSP_FW_ADDR_BITS` sense:

- **`IG_PLANE_POLICY_COUNT` is 5, and it is a policy ceiling, not a fact.**
  `intel_display_device.c:1561-1569` branches on `HAS_D12_PLANE_MINIMIZATION`,
  which is not vendored; nor is `enum plane_id`. Sprites plus one primary gives
  5 or 7 and the vendored set does not say which. `PLANE_5`'s existence is
  proven, so 5 is the largest count the evidence supports. If the truth is 7
  this refuses two planes that exist — costs a feature, cannot corrupt
  anything. The reverse would compute addresses for planes that may not be
  there.
- **The `-1` convention for `PLANE_SIZE`/`PLANE_POS` is UNPROVEN.** Gen12
  *timing* fields store value − 1, proven for PIPESRC by `intel_display.c:2815`.
  It would be natural to assume `PLANE_SIZE` follows. Nothing in
  `reference/i915/` shows it: the writer is `skl_universal_plane.c`, which is
  not vendored, and grepping the vendored `intel_display.c` for `PLANE_WIDTH` /
  `PLANE_HEIGHT` returns no matches. So the accessors return the RAW field and
  carry `Raw` in the name, and the suite pins that they do **not** add 1 while
  `igTimingLo()` on the same half **does** — the two decodes differ by exactly
  1 on the same fixture word. Do not harmonise them. Vendor
  `skl_universal_plane.c` first, then add a decoded wrapper *beside* the raw
  ones.

One more real finding worth carrying forward: **the format mask diverges by
generation.** `PLANE_CTL_FORMAT_MASK_SKL` is `GENMASK(27,24)`;
`PLANE_CTL_FORMAT_MASK_ICL` is `GENMASK(27,23)`. Gen12 is icl+, so a 4-bit
decode silently misreads every format with bit 23 set — Y210, Y212, Y216 — as a
*different, valid* format. Both decodes are exposed under distinct names and the
suite demonstrates them disagreeing on a real value (`0x00800000`: 1 under icl+,
0 = YUV422 under pre-icl).

## 3. THE OPEN DEFECT, and it is the highest-value item in this handoff

**`tools/check-registers.py` does not know `IntelDisplay.hpp` exists.** Every
single Intel constant in this project is unverified by the one gate whose entire
purpose is catching hand-transcription errors in register constants.

This was measured, not assumed. The gate's own header block is:

```python
KEXT_HEADER    = ROOT / "phase0-kext" / "NVDecode.hpp"
FALCON_HEADER  = ROOT / "phase0-kext" / "NVFalcon.hpp"
GSPBOOT_HEADER = ROOT / "phase0-kext" / "NVGspBoot.hpp"
STAGE_HEADER   = ROOT / "phase0-kext" / "NVGspStage.hpp"
```

and its output confirms the omission by listing exactly those four. There is no
`IntelDisplay` token anywhere in `tools/`. `114/114 passed` is therefore a true
statement about the NVIDIA constants and says **nothing whatever** about the
Intel ones. That is the shape this project has been bitten by repeatedly: a
green gate whose scope is narrower than a reader assumes.

What protects the Intel constants today is only `test_intel_display.cpp`, and it
protects them in a weaker way than the register gate would. The suite asserts
against **bare hand-typed literals** — which is the right construction, and it is
why it catches a wrong stride — but a hand-typed literal and a hand-typed
`#define` can be wrong *together* if the same person misread the same vendored
line twice. The register gate is the thing that compares against the vendored
header rather than against a second transcription.

The work is scoped and I had it mapped before running out of room. The gate
already has everything needed; nothing new has to be invented:

- Add `INTEL_HEADER = ROOT / "phase0-kext" / "IntelDisplay.hpp"` and add
  `reference/i915/i915_reg.h`, `skl_universal_plane_regs.h`,
  `intel_display_reg_defs.h`, `i915_reg_defs.h` and `intel_display_device.c` to
  a **second** source table. Do not append them to `SOURCE_HEADERS`: that table
  is merged first-definition-wins for NVIDIA's swref headers and mixing two
  vendors' namespaces into one dict invites a silent collision, which is the
  exact failure `merge_sources()` reports rather than hides.
- `resolve()` needs `REG_BIT(n)` and `REG_GENMASK(hi,lo)` handling. It already
  strips C integer suffixes and type casts and follows one level of aliasing;
  these two macros are the only new forms Intel's headers use. `REG_GENMASK`
  maps onto the existing `"field"` check kind cleanly — NVIDIA spells a field
  `"HI:LO"` and `resolve_bitrange()` already derives shift and mask from it, so
  the Intel form is a different spelling of a shape the gate handles.
- The checks themselves, with both sides' exact names confirmed this session:

  | kext | vendored | value |
  |---|---|---|
  | `IG_PLANE_CTL_BASE` | `_PLANE_CTL_1_A` | `0x70180` |
  | `IG_PLANE_DELTA` | `_PLANE_CTL_2_A` − `_PLANE_CTL_1_A` | `0x100` |
  | `IG_PLANE_PIPE_DELTA` | `_PLANE_CTL_1_B` − `_PLANE_CTL_1_A` | `0x1000` |
  | `IG_PLANE_CTL_ENABLE` | `PLANE_CTL_ENABLE` | `REG_BIT(31)` |
  | format shift/mask | `PLANE_CTL_FORMAT_MASK_ICL` | `GENMASK(27,23)` |
  | pre-icl shift/mask | `PLANE_CTL_FORMAT_MASK_SKL` | `GENMASK(27,24)` |
  | `IG_PLANE_POS_*` | `PLANE_POS_Y_MASK` / `PLANE_POS_X_MASK` | `31:16` / `15:0` |
  | `IG_PLANE_WIDTH_MASK` etc. | `PLANE_WIDTH_MASK` / `PLANE_HEIGHT_MASK` | `15:0` / `31:16` |
  | `IG_PIPE_TRANSCONF` | `_TRANSACONF` − `PIPE_A_OFFSET` | `0x08` |
  | timing masks | `HTOTAL_MASK` / `HACTIVE_MASK` | `31:16` / `15:0` |
  | transcoder bases | `_TRANS_HTOTAL_A/B/DSI0/DSI1` | `0x60000` `0x61000` `0x6B000` `0x6B800` |
  | pipe bases | `PIPE_A..D_OFFSET`, `PIPE_DSI0/1_OFFSET` | `0x70000`..`0x73000`, `0x7B000`/`0x7B800` |

  The eleven relative field offsets (`IG_PLANE_STRIDE` `0x08` through
  `IG_PLANE_AUX_DIST` `0x40`) each want a check of the form *vendored absolute
  minus `_PLANE_CTL_1_A`*, so the expected side is computed from the vendored
  header rather than restated.
- `PIPE_*_OFFSET` and `_TRANSACONF` live in `intel_display_device.c` and
  `i915_reg.h` respectively, so the source table has to include a `.c` file.
  `parse_defines()` does not care about the extension.

One trap, in the `self-cancelling-constants-are-untestable` family: do **not**
write the Intel checks so the expected side resolves through the kext's own
`#define`. The gate's existing `check_offset` / `check_field` already resolve the
two sides from two different tables, which is the property that matters. Keep it.

## 4. Also open, smaller

- **`IG_PLANE_FIELD_LIMIT` admits gaps.** `igPlaneReg()` accepts any aligned
  offset up to `0x40`, and `0x28` and `0x30` are gaps in the vendored register
  list. The suite pins that they pass rather than pretending otherwise — the
  guard is a bounds check, not a register whitelist, and the honest limit is
  recorded so nobody over-reads it. A whitelist would be strictly better and is
  a contained change.
- **Handoff13 §8 items are all still open**: diffing Apple's real KPI
  signatures against `tests/kextshim/` (highest value on the NVIDIA path, now
  possible for the first time), the three compiler warnings, retiring the four
  CD-delivery artifacts, and `tools/guest-build-kext.sh` silently ignoring its
  documented `--payload-root`.

## 5. Housekeeping done this session

- `tests/test_check_count_floor.py` — `MEASURED["test_intel_display"]` 144 →
  205. The superseded 144 is left standing with its own dated paragraph. That is
  the **fourth** time a number in this file described as measured went stale,
  and the second time inside a single day.
- `tests/test_check_count_last_line.py` — its docstring pointed at
  `tests/README.md` for the recorded totals. That file does not exist and never
  did in this tree. A docstring written specifically to warn that prose goes
  stale was itself pointing at a nonexistent file. Corrected in place, with the
  correction dated rather than the sentence quietly deleted.
- `build-payload/` rebuilt, which is **mandatory after any `phase0-kext/`
  edit** — `build-payload/kext-src.tar` is compared byte-for-byte against the
  live tree by the Python gate, so skipping the rebuild reddens it with a
  failure that names your header. Current image md5
  `a4f1524f18664a6a113357c41d360e57`, 303104 bytes, carrying `kext-src.tar`
  (225280 bytes, md5 `8c073e68dd3fbd0c14102e3de1aa3310`) and
  `guest-build-kext.sh` (12677 bytes) under vol ident `PAYLOAD`.
- `OPERATOR-ACTION-step7-build.md` gained its sixth rebuild block. Two
  tar-changing rebuilds inside one day is the thing worth recording: every byte
  size and md5 quoted in that doc goes stale the moment anything under
  `phase0-kext/` is edited, and the only things that will say so are
  `tests/test_guest_build_kext.py` and `tests/test_operator_step7_doc.py`.

## 6. How close either driver actually is

This section exists because §1 is six green lines and a reader could take that
for progress toward a picture on a screen. It is not, on either path.

### The Intel iGPU driver: roughly 5% of a display driver, and 0% of a pixel

What exists is **read-only decode**. `IntelDisplay.hpp` can compute the address
of a transcoder timing register or a plane register, and can take a 32-bit word
read from one and tell you what it means. That is genuinely the correct first
layer, it is the layer this project is good at, and 205 host-side checks stand
behind it.

What does not exist, at all:

- **Anything that writes.** There is no modeset sequence — no enable ordering,
  no wait-for-vblank, no register-write journal like `NVGspBoot.hpp` has.
- **Clocks.** DPLL programming, the combo PHY, DDI initialisation. On Gen12
  this is the bulk of the work and none of it is even mapped.
- **Power wells.** Gen12 gates display power domains; touching a transcoder
  before its well is up reads back garbage or hangs.
- **Output detection.** No GMBUS/DDC, no EDID parse, no DP AUX, no link
  training. The driver has no way to learn what is plugged in or what mode it
  supports.
- **Memory.** No GGTT programming, no stolen-memory or framebuffer allocation.
  There is nothing for a plane's `SURF` register to point at.
- **An IOKit shell.** `NV3050Probe.cpp` matches NVIDIA PCI IDs. There is no
  Intel probe, no `IOFramebuffer` subclass, no `IOService` match dictionary for
  Tiger Lake.

And the structural problem, which no amount of the above fixes: **Apple never
shipped Tiger Lake.** The last Intel Macs are Ice Lake / Comet Lake era, so
there is no `AppleIntelFramebuffer` for TGL to lean on, patch, or imitate — the
usual hackintosh route (spoof a device ID at a shipping Apple driver) has no
target. There is also no vendor open-source equivalent of
`open-gpu-kernel-modules` to derive from; `reference/i915/` is Linux, which is
informative about registers and silent about macOS driver shape.

Best honest estimate: the register-decode foundation is real and reusable, and
it is a small single-digit fraction of a display driver. Nothing currently in
the tree could light a panel.

### The 3050 driver: at its planned ceiling, and the ceiling is not a picture

`PLAN.md` is explicit that Phases 0–5 are the honest ceiling, and the tree is
essentially there. What is real: identity and firmware-health decode, the
Falcon/RISC-V register layer with per-engine address translation, RM ucode
descriptor parsing, the GSP bringup state machine driven against a simulated
BAR0 with write-**order** assertions, and the Phase 5 staging arithmetic. As of
handoff13 the kext also compiles and links under Apple clang 17.0.0 with
`-mkernel -fapple-kext` against the real `Kernel.framework`.

What has never happened: it was never `kextload`ed (needs SIP lowered and a
reboot), never matched against hardware, never executed a single MMIO access
against a real GA107. Beyond Phase 5 there is no channel/command submission, no
memory manager, no display path, and no answer to firmware signing on macOS.

And the ceiling itself: even a *complete* NVIDIA driver would not put a picture
on the target laptop's screen. The panel is wired to the Intel iGPU; the 3050 is
a muxless render-only device. That is the reason `PLAN.md` bounds the scope where
it does, and it is unchanged by anything in this handoff.

### What that means for ordering

The operator was told, and reaffirmed anyway, that doing the Intel driver first
is the harder of the two and does not make the NVIDIA one easier. That decision
is recorded, not reopened. The one thing worth saying plainly to whoever picks
this up: the shortest path to *anything visible* is not in this repo — it is a
machine Apple shipped a display driver for. Everything here is a register-level
exercise with real evidence discipline, and it is worth continuing on those
terms rather than on the expectation of a screen.

## 7. Start here

Wire `IntelDisplay.hpp` into `tools/check-registers.py` per §3. It is the only
item where the gap between what the gates *say* and what they *cover* is
currently load-bearing, the vendored evidence is all present, and the exact
symbol names on both sides are in the table above.
