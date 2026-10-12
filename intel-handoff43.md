# intel-handoff43 -- toward iGPU acceleration: the hardware path, step 1 (forcewake)

Date: 2026-10-12. Previous: `intel-handoff42.md` (sink step (b) rehearsal).

## 0. The goal, stated honestly

The operator asked for "acceleration on the iGPU". Two different things hide in
that word:

1. **macOS-visible acceleration** (Metal / Quartz Extreme / WindowServer
   compositing on the GPU). That needs an Apple user-space GPU driver for Tiger
   Lake that Apple never shipped. A kext alone cannot provide it. Out of reach.
2. **This kext making the Gen12 GT execute work** -- e.g. the blitter filling
   or copying a buffer. That is reachable, and it is what the GT stack in this
   tree (Cmd, Batch, Blt, Image, Context, Ring, Exec, Fence, Reset, Probe) has
   been composing in host memory for ten headers without ever delivering.

This handoff is about (2). The milestone is: **one XY_COLOR_BLT executed by the
BCS engine, its fill observed in memory, its fence seqno observed in the status
page.**

## 1. What was missing (grep-measured 2026-10-12, `phase0-kext/`)

| Prerequisite | hits before | status after this session |
|---|---|---|
| forcewake | 0 | **IntelGtForcewake.hpp + 188-check suite** |
| GGTT PTE writes | 0 | composed only (IntelGtExec.hpp, POLICY) -- see section 3 |
| execlist submit (ELSQ + EL_CTRL_LOAD) | composed only | unchanged |
| GuC | 0 | not needed: execlists path (i915 still supports it on TGL) |
| interrupts | 0 | not needed for a polled first submission |

## 2. What landed

- **Vendored 8 files** into `reference/i915/` (v6.12, byte-identical; table and
  md5s in `reference/i915/PROVENANCE.md`): `intel_gt_regs.h`,
  `intel_uncore.[ch]`, `intel_gtt.h`, `intel_ggtt.c`,
  `intel_execlists_submission.c`, `intel_lrc.c`, `intel_engine_cs.c`.
- **`phase0-kext/IntelGtForcewake.hpp`** -- pure, no MMIO:
  - cited request/ack registers for the RENDER (0xA278 / 0xD84) and GT
    (0xA188 / 0x130044) wells, FORCEWAKE_KERNEL bit 0, fallback bit 15;
  - masked-write composers: acquire `0x00010001`, release `0x00010000`,
    gen12 reset `0xEFFF0000`;
  - a 43-row copy of `__gen12_fw_ranges[]` and `igGtwDomainForOffset()`
    (RCS 0x2000 -> RENDER, BCS 0x22000 -> GT, media -> refused);
  - a step-wise acquire sequence (`IGGtwSeq`) following
    `fw_domains_get_normal()`: wait ack clear -> write request -> wait ack set.
    The shell supplies reads/writes/delays; the header says what to do next.
  - **all-ones is judged before the ack bit**: 0xFFFFFFFF has FORCEWAKE_KERNEL
    set, so a dead BAR would otherwise read as "well awake".
- **`tests/test_intel_gt_forcewake.cpp`**: 188 checks, 0 failures.
- **`tools/check-registers.py`** `intel_gt_forcewake_checks()`: 16 new checks
  (values, line citations, and the range table row-for-row). 260 -> 276.
- Registered in CMake, the syntax gate, the count floor and the last-line
  roster. 58th payload image (card updated).

### Negative controls run this session
- Flipping one table row (0x8140-0x815F RENDER -> GT): check-registers 275/276,
  names the row.
- Deleting the all-ones arm of `igGtwAckVerdict()`: suite 4 failures, including
  "dead after write -> mmio-dead, not held" (it reported HELD).

### Deliberately not implemented
The FORCEWAKE_KERNEL_FALLBACK toggle (`intel_uncore.c:203-248`). A missed ack
is a refusal (`no-ack` / `stale-ack`), never a write into a sleeping well.
Add it as its own verdict only once hardware shows the normal path missing.

## 3. A real finding for the next step: the GGTT address mask is too wide

`IntelGtExec.hpp` composes PTEs with `IG_GTX_PTE_ADDR_MASK_POLICY =
0x0000FFFFFFFFF000` (bits 47:12). Upstream says `GEN12_GGTT_PTE_ADDR_MASK` is
`GENMASK_ULL(45, 12)` (`intel_gtt.h:100`) = `0x00003FFFFFFFF000`. Also
`gen8_ggtt_pte_encode()` (`intel_ggtt.c:277-287`) sets only `GEN8_PAGE_PRESENT`
(plus LM, which is dGPU-only) -- no RW bit for GGTT. And the GGTT itself lives
at **GTTMMADR (BAR0) + 8 MiB** on gen8+ (`intel_ggtt.c:1137-1146`,
`gen6_gttadr_offset()` = 16 MiB / 2).

The execlist POLICY constants in `IntelGtExec.hpp` are now all citable and
all AGREE with upstream: SQ +0x510 (`intel_engine_regs.h:232`), CONTROL +0x550
(`:234`), EL_CTRL_LOAD bit 0 (`:235`), STATUS +0x234 (`:179`). Submission order
is `execlists_submit_ports()` (`intel_execlists_submission.c:925-941`): write
every port's descriptor low/high to SQ, then EL_CTRL_LOAD.

## 4. Next steps, in order

1. Retire IntelGtExec.hpp's POLICY names for the five values above into `_I915_`
   citations with check-registers coverage; narrow the PTE mask to 45:12 (and
   update test_intel_gt_exec accordingly).
2. Shell: in `IntelGtProbe.cpp`, behind an explicit boot-arg (proposed
   `iggtwake=1`), run the forcewake sequence on the GT domain, publish
   `IGGtForcewakeVerdict` / `IGGtForcewakeLastAck`, and release. This is the
   first GT write this tree would ever make, and it is benign: i915 does it
   thousands of times per second.
3. GGTT: pick a free GGTT page above the firmware framebuffer
   (IntelSinkAlloc.hpp already computes such a placement), write ONE PTE via
   BAR0+8MiB, read it back, invalidate (GFX_FLSH_CNTL_GEN6).
4. Map ring + context + batch + target pages; submit the existing blit through
   ELSQ + EL_CTRL_LOAD on BCS; poll the status-page seqno with
   IntelGtReset.hpp's budget; read the target page back.

Every step from 2 on needs a hardware boot to mean anything.

## 5. Open questions for the operator

- Is the laptop still available to boot? `intel-handoff39.md` said the machine
  that could run the capture script was gone. Without it, steps 2-4 can be
  written and gated but never observed.
- `test_the_shell_half_parses` still fails on this host (WSL bash cannot open
  a `D:\` path). Skip on Windows, or fix the path translation?
