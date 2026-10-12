# intel-handoff44 -- two wrong values in IntelGtExec.hpp, fixed and pinned

Date: 2026-10-12. Follows `intel-handoff43.md` (forcewake layer).

## Goal framing

The operator asked for the hardware path to be coded and gated in advance, so
that one boot yields a full trace with no coding in between. Before building the
opt-in bring-up path on top of `phase0-kext/IntelGtExec.hpp`, that header was
checked against the newly vendored i915 `gt/` files. Two of its values were
wrong. Building a writer on top of them first would have meant the first boot
was spent finding out the descriptor and PTE format were off.

## What landed

1. **PTE address mask: 47:12 -> 45:12.** `intel_gtt.h:100`
   `GEN12_GGTT_PTE_ADDR_MASK` is `GENMASK_ULL(45, 12)` = `0x00003FFFFFFFF000`.
   The old mask was `0x0000FFFFFFFFF000`, so an address with bit 46 or 47 set
   would have gone into the entry unchanged. Renamed
   `IG_GTX_PTE_ADDR_MASK_I915_45_12` / `IG_GTX_PTE_ADDR_CEILING_I915_45_12`.
2. **Software context id: bit 32 -> bit 37.** Tiger Lake is IP 12.0, so the
   `else` branch at `intel_execlists_submission.c:506-515` applies:
   `GEN11_SW_CTX_ID_SHIFT` = 37, width 11 (`intel_lrc.h:103-104`). Renamed
   `IG_GTX_CTXDESC_SWID_SHIFT_I915_37`. Descriptor `(0x1000, id 1)` is now
   `0x0000002000001001` (was `0x0000000100001001`).
3. **`reference/i915/intel_lrc.h` vendored** (v6.12, 4046 B, md5
   `ceead4c4bb890316d7102dcb0008d53c`, fetched twice, same hash). Recorded in
   `reference/i915/PROVENANCE.md`.
4. **`tools/check-registers.py` `intel_gt_exec_checks`**: 14 new checks (PTE
   mask and ceiling, present bit, SWID shift and width, valid bit, LRCA mask,
   the three execlist offsets, the load bit, and three `// file:LINE SYM` line
   citations). Gate 276 -> **290/290**. Negative control: putting both old
   values back gives 288/290; restoring them gives 290/290.
5. `tests/test_intel_gt_exec.cpp` 99 -> **102** checks (three literals updated,
   three new refusals: the old 47:12 ceiling and bit 46). Floor updated.
6. **59th payload image**: ISO md5 `3183e58ebf1c817a80a46521874106a3`
   (927744 B), tar `4c9772c2e7a251038306a1740c4d8f71` (849920 B), built twice
   with the same hash (sha256 prefix `5557aaef40010e51`).

The execlist register offsets (0x234 / 0x510 / 0x550, load bit 0) **match**
upstream. They keep their `_POLICY_` names on purpose, because
`tests/test_intel_probe_submit_shape.py` bans those exact names from
`IntelGtProbe.cpp`.

## Design finding for the bring-up path

`IntelGtProbe.cpp` is locked by five shape gates
(`tests/test_intel_probe_*_shape.py`). They ban `regWrite32`, volatile BAR
stores, and the SQ/CONTROL register names. **That should stay as it is.** The
writing path has to be a separate class (proposed `IntelGtBringup`) with its
own boot-arg opt-in and its own shape gate. It should not loosen the probe's
guarantees.

## Still open (confirm before planning, per CLAUDE.md)

- **Descriptor is still incomplete.** `intel_lrc.c:1517-1531`
  (`lrc_descriptor`) also sets `GEN8_CTX_PRIVILEGE` (bit 8) and an addressing
  mode at bit 3 (`GEN8_CTX_ADDRESSING_MODE_SHIFT`). Our descriptor sets
  neither, so the mode is 0. `intel_lrc.c:1569` also ORs
  `CTX_DESC_FORCE_RESTORE` (bit 2) on first submission. Choosing a mode means
  deciding between PPGTT and GGTT-only. That decision is the next design step.
- **GGTT flush.** On Gen12, `gen8_ggtt_invalidate` (`intel_ggtt.c:213-226`)
  writes `GFX_FLSH_CNTL_GEN6` (0x101008, `intel_gt_regs.h:1478`) only when
  `needs_wc_ggtt_mapping()` is true (`:198-211`, false for ICL+). So no flush
  write is expected, and the GGTT must be mapped uncached.
- Bring-up class, its trace format, boot-arg staging (forcewake / +GGTT /
  +submit) and its shape gate: **not started**.
- `test_the_shell_half_parses` (WSL bash cannot open the D:\ path): still
  waiting on the operator's decision.

Hardware runs are still needed before any of this is known to work. Every
number above is now checked against i915 source. None has been seen on silicon.
