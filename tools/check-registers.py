#!/usr/bin/env python3
"""
check-registers.py -- verify every register constant in the kext against
NVIDIA's published headers.

Why this exists: this project has already shipped two wrong register decodes
from hand-transcription -- the split ARCHITECTURE field in NV_PMC_BOOT_0, and
GA107's implementation ID. Both were caught by accident. A kext that reads the
wrong offset does not fail loudly; it returns a plausible-looking number and
sends you debugging the wrong thing for a week.

So: parse the authoritative headers, parse the kext header, and diff them.
Run this after any edit to NVDecode.hpp.

Usage:
    python check-registers.py
    python check-registers.py --verbose

Exit code 0 if everything matches, 1 otherwise.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEXT_HEADER = ROOT / "phase0-kext" / "NVDecode.hpp"
FALCON_HEADER = ROOT / "phase0-kext" / "NVFalcon.hpp"
GSPBOOT_HEADER = ROOT / "phase0-kext" / "NVGspBoot.hpp"
STAGE_HEADER = ROOT / "phase0-kext" / "NVGspStage.hpp"

#
# The Intel track. Everything above this line is checked against NVIDIA's
# open-gpu-kernel-modules; IntelDisplay.hpp is checked against the vendored
# Linux i915 headers in reference/i915/, which are that track's only evidence.
#
# Why it is here at all: handoff14 §3 recorded that this gate's header list
# stopped at the four NVIDIA ones, so every Intel constant was unverified by the
# one gate whose entire purpose is catching hand-transcription errors in
# register constants -- and a clean run was a true statement about the NVIDIA
# constants that said nothing about the Intel ones.
#
INTEL_HEADER = ROOT / "phase0-kext" / "IntelDisplay.hpp"

#
# The second Intel header, and a DIFFERENT kind of evidence. IntelDisplay.hpp
# holds register offsets and is checked against #defines in the vendored i915
# register headers. IntelGpuId.hpp holds PCI device IDs, and in the vendored
# tree those are not #defines at all -- they are bare literals inside a
# variadic macro body (see i915_id_list below). Nothing in this gate could
# reach them, so every one of the eleven Tiger Lake IDs was hand-transcribed
# with no gate reading it: exactly the condition handoff14 3 recorded for the
# display header before it was wired in here.
#
INTEL_ID_HEADER = ROOT / "phase0-kext" / "IntelGpuId.hpp"

#
# The third Intel header, and the same defect one more time. IntelDisplay.hpp
# answers "where is register X" and is checked below; IntelFramebuffer.hpp
# answers "what do the VALUES in those registers mean", and until this arm
# existed not one of its ~30 constants was read by this gate. That is worse
# here than it was for the addressing layer, because a wrong FIELD mask does
# not fault: PLANE_CTL_FORMAT_MASK_SKL and _ICL differ by one bit, and the
# observed CTL 0x82009000 decodes to a DIFFERENT VALID FORMAT under each
# (4 = XRGB_8888 vs 2 = XRGB_2101010). A plane decoded at the wrong bpp
# produces a stride wrong by an integer factor, which lands the scanout
# INSIDE the framebuffer and shears the picture rather than faulting.
#
INTEL_FB_HEADER = ROOT / "phase0-kext" / "IntelFramebuffer.hpp"

#
# The fourth Intel header, wired in 2026-08-19, and the same defect a fourth
# time. IntelScanout.hpp turns an inherited plane configuration into the three
# numbers IOFramebuffer needs, and until this arm existed NOT ONE of its
# constants was read by this gate -- including IG_SCAN_, which at seven arms is
# the LARGEST verdict family in the Intel track while all seven smaller families
# had a set-check.
#
# There is a reason this one is not simply another intel_verdict_set_checks()
# line, and it is handoff16 1's IG_PIPE_TRANSCONF trap: the IG_SCAN_ prefix also
# covers IG_SCAN_INVALID32, IG_SCAN_INVALID64, IG_SCAN_EXTENT_POLICY_CEILING,
# IG_SCAN_SURF_ADDR_MASK and IG_SCAN_SURF_ALIGN_BYTES, which are sentinels and
# masks rather than verdicts. A bare prefix sweep would compute max(codes) as
# 0xFFFFFFFFFFFFFFFF and fail the limit == largest equality while being wrong
# about the world -- verbatim the failure handoff16 1 recorded, where a verdict
# family named IG_PIPE_ swept in a register offset and printed a contiguity
# error that was correct about its own arithmetic. The selector below is what
# makes the sweep honest.
#
INTEL_SCAN_HEADER = ROOT / "phase0-kext" / "IntelScanout.hpp"

#
# The fifth Intel header, wired in 2026-08-20. IntelPixel.hpp turns a plane
# format code -- already decoded by igFbFormat() -- into the fields
# IOPixelInformation carries, or a refusal. It is the second header in the
# dependency-free family with an #include, after IntelDisplayNames.hpp, because
# it switches on format codes IntelFramebuffer.hpp owns and takes them from
# their owner rather than respelling them.
#
# Same selector reasoning as the scanout arm above, and the IG_PIXEL_ prefix is
# a WIDER trap than IG_SCAN_ was: it covers IG_PIXEL_INVALID32,
# IG_PIXEL_COMPONENT_COUNT, IG_PIXEL_BPP_XRGB_8888, IG_PIXEL_BPP_XRGB_2101010,
# IG_PIXEL_COMPONENT_BITS_8888, IG_PIXEL_COMPONENT_BITS_2101010 and six
# IG_PIXEL_MASK_* values -- twelve non-verdict constants against three verdicts.
# A bare prefix sweep would compute max(codes) as 0xFFFFFFFF (the sentinel) and
# fail the limit == largest equality while being wrong about the world, which is
# handoff16 1's IG_PIPE_TRANSCONF trap with a worse ratio.
#
INTEL_PIXEL_HEADER = ROOT / "phase0-kext" / "IntelPixel.hpp"

#
# The three GT headers carrying constants with an upstream citation, added
# 2026-08-26 alongside the gt/ vendoring.
#
# Deliberately NOT added to SOURCE_HEADERS. That list drives the whole-header
# set-checks and the define-count print; these three are opened for a handful of
# NAMED constants each, and folding them into the general machinery would put
# ~200 POLICY constants in front of checkers that have nothing to say about
# them. intel_gt_citation_checks() reads them directly instead.
#
GT_CMD_HEADER   = ROOT / "phase0-kext" / "IntelGtCmd.hpp"
GT_CTX_HEADER   = ROOT / "phase0-kext" / "IntelGtContext.hpp"
GT_PROBE_HEADER = ROOT / "phase0-kext" / "IntelGtProbe.hpp"
GT_FW_HEADER = ROOT / "phase0-kext" / "IntelGtForcewake.hpp"

I915 = ROOT / "reference" / "i915"

#
# Deliberately NOT in I915_HEADERS. merge_i915_sources() parses #defines, and
# this file's #defines are macro NAMES with unresolvable variadic bodies --
# merging it would put INTEL_TGL_GT1_IDS in front of resolve() as a symbol
# whose body is a comma-separated list of macro calls. It is read as raw text
# instead.
#
I915_PCIIDS = I915 / "i915_pciids.h"

#
# Kept SEPARATE from SOURCE_HEADERS rather than appended to it. Two reasons,
# and the second is the one that would bite:
#
#   1. merge_sources() reports collisions across its inputs, and the NVIDIA and
#      Intel namespaces have no reason to be compared for collisions.
#   2. i915 spells several of its values with macros that only exist in the
#      i915 world -- REG_BIT(31), REG_GENMASK(27, 23). Merging those into the
#      NVIDIA table would put unresolvable bodies in front of resolve() for
#      every NVIDIA check that walks a composed constant.
#
# So the Intel side gets its own table and its own resolver front end
# (intel_reg_value) that understands REG_BIT and REG_GENMASK.
#
#
# The three gt/ headers below were vendored 2026-08-26 and are added here for a
# reason worth stating, because merely HAVING a file in reference/i915/ buys
# nothing: this gate is the only thing in the tree that compares a kext #define
# against upstream's, so a constant citable to a header the gate does not parse
# is a constant whose citation nobody checks. The comment in the header and the
# value in the header would then be free to disagree, which is the exact shape
# of a fabricated citation -- worse than an uncited value, because it reads as
# evidence.
#
# intel_gpu_commands.h is deliberately NOT here. Its #defines are MI_INSTR(...)
# calls -- MI_NOOP is MI_INSTR(0, 0), MI_LOAD_REGISTER_IMM(x) is
# MI_INSTR(0x22, 2*(x)-1) -- so merging it puts unresolvable macro bodies in
# front of i915_resolve() exactly the way i915_pciids.h would, and a symbol that
# fails to resolve makes its check SKIP, which is a silent pass. The command
# encoding is checked structurally in tests/test_intel_gt_cmd.cpp instead.
#
I915_HEADERS = [
    I915 / "i915_reg.h",
    I915 / "intel_display_device.c",
    I915 / "skl_universal_plane_regs.h",
    I915 / "intel_lrc_reg.h",
    I915 / "intel_engine_regs.h",
]
SWREF = (
    ROOT / "reference" / "open-gpu-kernel-modules"
    / "src" / "common" / "inc" / "swref" / "published"
)

SOURCE_HEADERS = [
    SWREF / "nv_ref.h",
    SWREF / "nv_arch.h",
    SWREF / "ampere" / "ga102" / "dev_boot.h",
    SWREF / "ampere" / "ga102" / "dev_gc6_island.h",
    SWREF / "ampere" / "ga102" / "dev_gc6_island_addendum.h",
    SWREF / "ampere" / "ga102" / "dev_falcon_v4.h",
    SWREF / "ampere" / "ga102" / "dev_falcon_second_pri.h",
    SWREF / "ampere" / "ga102" / "dev_riscv_pri.h",
    SWREF / "ampere" / "ga102" / "dev_gsp.h",
    #
    # NVIDIA ships no dev_fb.h under ampere/ga102, so WPR2 is checked against
    # turing/tu102 -- the same implementation NVIDIA's HAL routes GA10x GSP
    # bootstrap to. hopper/gh100/dev_fb.h carries identical offsets, which is
    # independent corroboration rather than a second copy of one guess.
    #
    SWREF / "turing" / "tu102" / "dev_fb.h",
]

# --------------------------------------------------------------------------
# Checks. Each entry is one claim the kext header makes, paired with the
# NVIDIA symbol that should back it.
#
#   ("offset", KEXT_NAME, NVIDIA_NAME)
#       plain numeric value must match exactly
#
#   ("field", KEXT_SHIFT_NAME, KEXT_MASK_NAME, NVIDIA_NAME)
#       NVIDIA defines the field as "HI:LO"; derive shift=LO and
#       mask=(1 << (HI-LO+1)) - 1 and compare against the kext pair
#
#   ("value", KEXT_NAME, NVIDIA_NAME)
#       a constant such as an architecture or implementation ID
# --------------------------------------------------------------------------
CHECKS = [
    # ---- NV_PMC_BOOT_0 ----
    ("offset", "NV_PMC_BOOT_0", "NV_PMC_BOOT_0"),
    ("field", "NV_PMC_BOOT_0_MINOR_REVISION_SHIFT",
              "NV_PMC_BOOT_0_MINOR_REVISION_MASK",
              "NV_PMC_BOOT_0_MINOR_REVISION"),
    ("field", "NV_PMC_BOOT_0_MAJOR_REVISION_SHIFT",
              "NV_PMC_BOOT_0_MAJOR_REVISION_MASK",
              "NV_PMC_BOOT_0_MAJOR_REVISION"),
    ("field", "NV_PMC_BOOT_0_ARCHITECTURE_1_SHIFT",
              "NV_PMC_BOOT_0_ARCHITECTURE_1_MASK",
              "NV_PMC_BOOT_0_ARCHITECTURE_1"),
    ("field", "NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT",
              "NV_PMC_BOOT_0_IMPLEMENTATION_MASK",
              "NV_PMC_BOOT_0_IMPLEMENTATION"),
    ("field", "NV_PMC_BOOT_0_ARCHITECTURE_0_SHIFT",
              "NV_PMC_BOOT_0_ARCHITECTURE_0_MASK",
              "NV_PMC_BOOT_0_ARCHITECTURE_0"),

    # ---- NV_PMC_BOOT_42 ----
    ("offset", "NV_PMC_BOOT_42", "NV_PMC_BOOT_42"),
    ("field", "NV_PMC_BOOT_42_MINOR_EXT_REVISION_SHIFT",
              "NV_PMC_BOOT_42_MINOR_EXT_REVISION_MASK",
              "NV_PMC_BOOT_42_MINOR_EXTENDED_REVISION"),
    ("field", "NV_PMC_BOOT_42_MINOR_REVISION_SHIFT",
              "NV_PMC_BOOT_42_MINOR_REVISION_MASK",
              "NV_PMC_BOOT_42_MINOR_REVISION"),
    ("field", "NV_PMC_BOOT_42_MAJOR_REVISION_SHIFT",
              "NV_PMC_BOOT_42_MAJOR_REVISION_MASK",
              "NV_PMC_BOOT_42_MAJOR_REVISION"),
    ("field", "NV_PMC_BOOT_42_IMPLEMENTATION_SHIFT",
              "NV_PMC_BOOT_42_IMPLEMENTATION_MASK",
              "NV_PMC_BOOT_42_IMPLEMENTATION"),
    ("field", "NV_PMC_BOOT_42_ARCHITECTURE_SHIFT",
              "NV_PMC_BOOT_42_ARCHITECTURE_MASK",
              "NV_PMC_BOOT_42_ARCHITECTURE"),
    ("field", "NV_PMC_BOOT_42_CHIP_ID_SHIFT",
              "NV_PMC_BOOT_42_CHIP_ID_MASK",
              "NV_PMC_BOOT_42_CHIP_ID"),

    # ---- architecture constants ----
    ("value", "NV_ARCHITECTURE_TU100", "NV_PMC_BOOT_42_ARCHITECTURE_TU100"),
    ("value", "NV_ARCHITECTURE_GA100", "NV_PMC_BOOT_42_ARCHITECTURE_GA100"),
    ("value", "NV_ARCHITECTURE_GH100", "NV_PMC_BOOT_42_ARCHITECTURE_GH100"),
    ("value", "NV_ARCHITECTURE_AD100", "NV_PMC_BOOT_42_ARCHITECTURE_AD100"),
    ("value", "NV_ARCHITECTURE_GB100", "NV_PMC_BOOT_42_ARCHITECTURE_GB100"),
    ("value", "NV_ARCHITECTURE_GB200", "NV_PMC_BOOT_42_ARCHITECTURE_GB200"),
    ("value", "NV_ARCHITECTURE_GR100", "NV_PMC_BOOT_42_ARCHITECTURE_GR100"),

    # ---- Ampere implementation constants ----
    ("value", "NV_IMPLEMENTATION_GA100", "GPU_IMPLEMENTATION_GA100"),
    ("value", "NV_IMPLEMENTATION_GA102", "GPU_IMPLEMENTATION_GA102"),
    ("value", "NV_IMPLEMENTATION_GA103", "GPU_IMPLEMENTATION_GA103"),
    ("value", "NV_IMPLEMENTATION_GA104", "GPU_IMPLEMENTATION_GA104"),
    ("value", "NV_IMPLEMENTATION_GA106", "GPU_IMPLEMENTATION_GA106"),
    ("value", "NV_IMPLEMENTATION_GA107", "GPU_IMPLEMENTATION_GA107"),

    # ---- Phase 3.5 GC6 island ----
    ("offset", "NV_PGC6_AON_SECURE_SCRATCH_GROUP_42",
               "NV_PGC6_AON_SECURE_SCRATCH_GROUP_42"),
    ("field", "NV_GFW_BOOT_PROGRESS_SHIFT",
              "NV_GFW_BOOT_PROGRESS_MASK",
              "NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT_PROGRESS"),
    ("value", "NV_GFW_BOOT_PROGRESS_COMPLETED",
              "NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT_PROGRESS_COMPLETED"),
]

# GROUP_05 is a parameterised macro, so it needs its own check rather than a
# plain numeric comparison.
PARAM_MACRO_CHECKS = [
    ("NV_PGC6_AON_SECURE_SCRATCH_GROUP_05",
     "NV_PGC6_AON_SECURE_SCRATCH_GROUP_05"),
]

# --------------------------------------------------------------------------
# NVFalcon.hpp -- the Phase 4 register layer. Same three check kinds.
#
# These matter more than the NVDecode.hpp set, not less: a wrong Falcon offset
# is a WRITE to the wrong register on live silicon, where a wrong identity
# offset is only a bad read.
# --------------------------------------------------------------------------
FALCON_CHECKS = [
    # ---- base addresses (the three-address-space hazard) ----
    ("offset", "NV_FALCON2_GSP_BASE", "NV_FALCON2_GSP_BASE"),

    # ---- Falcon core, engine-relative (dev_falcon_v4.h) ----
    ("offset", "NV_PFALCON_FALCON_IRQSCLR", "NV_PFALCON_FALCON_IRQSCLR"),
    ("offset", "NV_PFALCON_FALCON_IRQSTAT", "NV_PFALCON_FALCON_IRQSTAT"),
    ("offset", "NV_PFALCON_FALCON_IRQMSET", "NV_PFALCON_FALCON_IRQMSET"),
    ("offset", "NV_PFALCON_FALCON_IRQMCLR", "NV_PFALCON_FALCON_IRQMCLR"),
    ("offset", "NV_PFALCON_FALCON_IRQMASK", "NV_PFALCON_FALCON_IRQMASK"),
    ("offset", "NV_PFALCON_FALCON_IRQDEST", "NV_PFALCON_FALCON_IRQDEST"),
    ("offset", "NV_PFALCON_FALCON_MAILBOX0", "NV_PFALCON_FALCON_MAILBOX0"),
    ("offset", "NV_PFALCON_FALCON_MAILBOX1", "NV_PFALCON_FALCON_MAILBOX1"),
    ("offset", "NV_PFALCON_FALCON_OS", "NV_PFALCON_FALCON_OS"),
    ("offset", "NV_PFALCON_FALCON_HWCFG", "NV_PFALCON_FALCON_HWCFG"),
    ("offset", "NV_PFALCON_FALCON_HWCFG2", "NV_PFALCON_FALCON_HWCFG2"),
    ("offset", "NV_PFALCON_FALCON_CPUCTL", "NV_PFALCON_FALCON_CPUCTL"),
    ("offset", "NV_PFALCON_FALCON_BOOTVEC", "NV_PFALCON_FALCON_BOOTVEC"),
    ("offset", "NV_PFALCON_FALCON_CPUCTL_ALIAS", "NV_PFALCON_FALCON_CPUCTL_ALIAS"),
    ("offset", "NV_PFALCON_FALCON_DMACTL", "NV_PFALCON_FALCON_DMACTL"),
    ("offset", "NV_PFALCON_FALCON_DMATRFBASE", "NV_PFALCON_FALCON_DMATRFBASE"),
    ("offset", "NV_PFALCON_FALCON_DMATRFMOFFS", "NV_PFALCON_FALCON_DMATRFMOFFS"),
    ("offset", "NV_PFALCON_FALCON_DMATRFCMD", "NV_PFALCON_FALCON_DMATRFCMD"),
    ("offset", "NV_PFALCON_FALCON_DMATRFFBOFFS", "NV_PFALCON_FALCON_DMATRFFBOFFS"),
    ("offset", "NV_PFALCON_FALCON_DMATRFBASE1", "NV_PFALCON_FALCON_DMATRFBASE1"),

    # ---- Falcon fields ----
    ("field", "NV_PFALCON_FALCON_HWCFG2_RISCV_SHIFT",
              "NV_PFALCON_FALCON_HWCFG2_RISCV_MASK",
              "NV_PFALCON_FALCON_HWCFG2_RISCV"),
    ("field", "NV_PFALCON_FALCON_HWCFG2_MEM_SCRUBBING_SHIFT",
              "NV_PFALCON_FALCON_HWCFG2_MEM_SCRUBBING_MASK",
              "NV_PFALCON_FALCON_HWCFG2_MEM_SCRUBBING"),
    ("field", "NV_PFALCON_FALCON_HWCFG_IMEM_SIZE_SHIFT",
              "NV_PFALCON_FALCON_HWCFG_IMEM_SIZE_MASK",
              "NV_PFALCON_FALCON_HWCFG_IMEM_SIZE"),
    ("field", "NV_PFALCON_FALCON_CPUCTL_STARTCPU_SHIFT",
              "NV_PFALCON_FALCON_CPUCTL_STARTCPU_MASK",
              "NV_PFALCON_FALCON_CPUCTL_STARTCPU"),
    ("field", "NV_PFALCON_FALCON_CPUCTL_HALTED_SHIFT",
              "NV_PFALCON_FALCON_CPUCTL_HALTED_MASK",
              "NV_PFALCON_FALCON_CPUCTL_HALTED"),
    ("field", "NV_PFALCON_FALCON_CPUCTL_ALIAS_EN_SHIFT",
              "NV_PFALCON_FALCON_CPUCTL_ALIAS_EN_MASK",
              "NV_PFALCON_FALCON_CPUCTL_ALIAS_EN"),
    ("field", "NV_PFALCON_FALCON_DMATRFCMD_FULL_SHIFT",
              "NV_PFALCON_FALCON_DMATRFCMD_FULL_MASK",
              "NV_PFALCON_FALCON_DMATRFCMD_FULL"),
    ("field", "NV_PFALCON_FALCON_DMATRFCMD_IDLE_SHIFT",
              "NV_PFALCON_FALCON_DMATRFCMD_IDLE_MASK",
              "NV_PFALCON_FALCON_DMATRFCMD_IDLE"),
    ("field", "NV_PFALCON_FALCON_DMATRFCMD_IMEM_SHIFT",
              "NV_PFALCON_FALCON_DMATRFCMD_IMEM_MASK",
              "NV_PFALCON_FALCON_DMATRFCMD_IMEM"),
    ("field", "NV_PFALCON_FALCON_DMATRFCMD_WRITE_SHIFT",
              "NV_PFALCON_FALCON_DMATRFCMD_WRITE_MASK",
              "NV_PFALCON_FALCON_DMATRFCMD_WRITE"),
    ("field", "NV_PFALCON_FALCON_DMATRFCMD_SIZE_SHIFT",
              "NV_PFALCON_FALCON_DMATRFCMD_SIZE_MASK",
              "NV_PFALCON_FALCON_DMATRFCMD_SIZE"),
    ("field", "NV_PFALCON_FALCON_DMATRFCMD_CTXDMA_SHIFT",
              "NV_PFALCON_FALCON_DMATRFCMD_CTXDMA_MASK",
              "NV_PFALCON_FALCON_DMATRFCMD_CTXDMA"),
    ("field", "NV_PFALCON_FALCON_DMATRFMOFFS_OFFS_SHIFT",
              "NV_PFALCON_FALCON_DMATRFMOFFS_OFFS_MASK",
              "NV_PFALCON_FALCON_DMATRFMOFFS_OFFS"),
    ("field", "NV_PFALCON_FALCON_DMATRFBASE1_BASE_SHIFT",
              "NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK",
              "NV_PFALCON_FALCON_DMATRFBASE1_BASE"),
    ("field", "NV_PFALCON_FALCON_IMEMC_OFFS_SHIFT",
              "NV_PFALCON_FALCON_IMEMC_OFFS_MASK",
              "NV_PFALCON_FALCON_IMEMC_OFFS"),
    ("field", "NV_PFALCON_FALCON_IMEMC_BLK_SHIFT",
              "NV_PFALCON_FALCON_IMEMC_BLK_MASK",
              "NV_PFALCON_FALCON_IMEMC_BLK"),
    ("field", "NV_PFALCON_FALCON_IMEMC_AINCW_SHIFT",
              "NV_PFALCON_FALCON_IMEMC_AINCW_MASK",
              "NV_PFALCON_FALCON_IMEMC_AINCW"),
    ("field", "NV_PFALCON_FALCON_DMEMC_OFFS_SHIFT",
              "NV_PFALCON_FALCON_DMEMC_OFFS_MASK",
              "NV_PFALCON_FALCON_DMEMC_OFFS"),
    ("field", "NV_PFALCON_FALCON_DMEMC_BLK_SHIFT",
              "NV_PFALCON_FALCON_DMEMC_BLK_MASK",
              "NV_PFALCON_FALCON_DMEMC_BLK"),

    # ---- RISC-V, relative to NV_FALCON2_GSP_BASE (dev_riscv_pri.h) ----
    ("offset", "NV_PRISCV_RISCV_CPUCTL", "NV_PRISCV_RISCV_CPUCTL"),
    ("offset", "NV_PRISCV_RISCV_PRIV_ERR_STAT", "NV_PRISCV_RISCV_PRIV_ERR_STAT"),
    ("offset", "NV_PRISCV_RISCV_HUB_ERR_STAT", "NV_PRISCV_RISCV_HUB_ERR_STAT"),
    ("offset", "NV_PRISCV_RISCV_BCR_CTRL", "NV_PRISCV_RISCV_BCR_CTRL"),

    ("field", "NV_PRISCV_RISCV_CPUCTL_HALTED_SHIFT",
              "NV_PRISCV_RISCV_CPUCTL_HALTED_MASK",
              "NV_PRISCV_RISCV_CPUCTL_HALTED"),
    ("field", "NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT_SHIFT",
              "NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT_MASK",
              "NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT"),
    ("field", "NV_PRISCV_RISCV_BCR_CTRL_VALID_SHIFT",
              "NV_PRISCV_RISCV_BCR_CTRL_VALID_MASK",
              "NV_PRISCV_RISCV_BCR_CTRL_VALID"),
    ("field", "NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_SHIFT",
              "NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_MASK",
              "NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT"),
    ("field", "NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_SHIFT",
              "NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_MASK",
              "NV_PRISCV_RISCV_BCR_CTRL_BRFETCH"),

    # ---- GSP engine, ABSOLUTE (dev_gsp.h) ----
    ("offset", "NV_PGSP_FALCON_MAILBOX0", "NV_PGSP_FALCON_MAILBOX0"),
    ("offset", "NV_PGSP_FALCON_MAILBOX1", "NV_PGSP_FALCON_MAILBOX1"),
    ("offset", "NV_PGSP_FALCON_ENGINE", "NV_PGSP_FALCON_ENGINE"),
    ("field", "NV_PGSP_FALCON_ENGINE_RESET_SHIFT",
              "NV_PGSP_FALCON_ENGINE_RESET_MASK",
              "NV_PGSP_FALCON_ENGINE_RESET"),

    # ---- WPR2, from turing/tu102/dev_fb.h (no ga102 dev_fb.h exists) ----
    ("offset", "NV_PFB_PRI_MMU_WPR2_ADDR_LO", "NV_PFB_PRI_MMU_WPR2_ADDR_LO"),
    ("offset", "NV_PFB_PRI_MMU_WPR2_ADDR_HI", "NV_PFB_PRI_MMU_WPR2_ADDR_HI"),
    ("field", "NV_PFB_PRI_MMU_WPR2_ADDR_LO_VAL_SHIFT",
              "NV_PFB_PRI_MMU_WPR2_ADDR_LO_VAL_MASK",
              "NV_PFB_PRI_MMU_WPR2_ADDR_LO_VAL"),
    ("field", "NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL_SHIFT",
              "NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL_MASK",
              "NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL"),
]

FALCON_PARAM_MACRO_CHECKS = [
    ("NV_PFALCON_FALCON_IMEMC", "NV_PFALCON_FALCON_IMEMC"),
    ("NV_PFALCON_FALCON_IMEMD", "NV_PFALCON_FALCON_IMEMD"),
    ("NV_PFALCON_FALCON_IMEMT", "NV_PFALCON_FALCON_IMEMT"),
    ("NV_PFALCON_FALCON_DMEMC", "NV_PFALCON_FALCON_DMEMC"),
    ("NV_PFALCON_FALCON_DMEMD", "NV_PFALCON_FALCON_DMEMD"),
    ("NV_PFALCON_FALCON_INTR_RETRIGGER", "NV_PFALCON_FALCON_INTR_RETRIGGER"),
    ("NV_PGSP_MAILBOX", "NV_PGSP_MAILBOX"),
    ("NV_PGSP_QUEUE_HEAD", "NV_PGSP_QUEUE_HEAD"),
]

DEFINE_RE = re.compile(
    r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+?)\s*(?:/\*.*)?$"
)
PARAM_DEFINE_RE = re.compile(
    r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\(([A-Za-z_][A-Za-z0-9_]*)\)\s+(.+?)\s*(?:/\*.*)?$"
)
BITRANGE_RE = re.compile(r"^(\d+)\s*:\s*(\d+)$")
NUMERIC_RE = re.compile(r"^0[xX][0-9A-Fa-f]+$|^\d+$")

#
# i915 spells most of its field constants through two macros from
# reference/i915/i915_reg_defs.h rather than as literals:
#
#   #define   PLANE_CTL_ENABLE          REG_BIT(31)
#   #define   PLANE_CTL_FORMAT_MASK_ICL REG_GENMASK(27, 23)
#
# resolve() cannot see through those -- REG_BIT is a parameterised macro, and
# treating `REG_BIT` as a symbolic operand makes the whole body report as
# unparseable. An Intel check whose reference value is unparseable would then
# skip, and a skip is a silent pass, which is the exact failure mode this file
# exists to prevent. So the Intel side gets these two forms decoded directly.
#
# The width suffixes (REG_BIT8/16, REG_GENMASK8/16/64) are accepted because
# they differ only in the C type they force, not in the bits they name.
#
I915_REG_BIT_RE = re.compile(r"^REG_BIT(?:8|16|64)?\s*\(\s*(\d+)\s*\)$")
I915_GENMASK_RE = re.compile(
    r"^REG_GENMASK(?:8|16|64)?\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)$"
)

#
# igTransBase() and igPipeBase() are SWITCHES, not tables of #defines, and that
# is deliberate (IntelDisplay.hpp:204-208: any stride formula that reproduces
# A..D gets DSI1 wrong by 0x800). parse_defines() therefore cannot see a single
# one of those twelve addresses -- the values that matter most on this track are
# invisible to the mechanism the rest of this gate is built on. This pattern is
# how they are read back out of the source text instead.
#
IG_CASE_RE = re.compile(
    r"case\s+IG_TRANSCODER_([A-Z0-9]+)\s*:\s*return\s+"
    r"(0[xX][0-9A-Fa-f]+)[uUlL]*\s*;"
)

#
# NVGspBoot.hpp writes its literals with C integer suffixes (0x000005A4u,
# 256u) because they feed NVU32 expressions. NVIDIA's headers never do, so
# until now resolve() had no reason to cope with one -- and a suffixed literal
# resolved to None, which this script reports as "not found or unparseable"
# rather than as a wrong value. Stripping the suffix is safe: it only affects
# bodies that previously could not be resolved at all.
#
INT_SUFFIX_RE = re.compile(r"(0[xX][0-9A-Fa-f]+|\d+)[uUlL]+")


#
# The Intel track's check table.
#
# Read the KINDS carefully -- three of the four do not compare a kext constant
# to an i915 constant of the same value, because IntelDisplay.hpp deliberately
# stores something DERIVED from i915's number rather than the number itself:
#
#   delta  The kext stores a field's offset RELATIVE to its block base, so one
#          table serves every transcoder and every (pipe, plane) pair. i915
#          stores absolute addresses for the A instance. So the reference value
#          is `iname - ibase`, and a check that compared them directly would
#          fail on every single row.
#   shift  The kext stores a field's LOW BIT NUMBER; i915 stores a shifted mask
#          via REG_GENMASK(hi, lo). Reference value is that mask's lowest set
#          bit.
#   mask   The kext stores a field's mask NORMALISED to bit 0; i915's is still
#          shifted into position. Reference value is `mask >> shift`.
#   value  The two really are the same number.
#
# Getting the kind wrong is not a silent pass -- a delta compared as a value is
# off by the block base and fails loudly. That is the point of spelling the
# relationship out per row rather than hunting for a name that happens to match.
#
INTEL_CHECKS = [
    #
    # Field offsets within a transcoder block, against i915_reg.h's absolute
    # addresses for transcoder A. _PIPEASRC is in this group rather than with
    # TRANSCONF because i915_reg.h:1147 defines PIPESRC() through _MMIO_TRANS2 --
    # it is a transcoder-block register despite the name.
    #
    ("delta", "IG_TRANS_HTOTAL", "_TRANS_HTOTAL_A", "_TRANS_HTOTAL_A"),
    ("delta", "IG_TRANS_HBLANK", "_TRANS_HTOTAL_A", "_TRANS_HBLANK_A"),
    ("delta", "IG_TRANS_HSYNC", "_TRANS_HTOTAL_A", "_TRANS_HSYNC_A"),
    ("delta", "IG_TRANS_VTOTAL", "_TRANS_HTOTAL_A", "_TRANS_VTOTAL_A"),
    ("delta", "IG_TRANS_VBLANK", "_TRANS_HTOTAL_A", "_TRANS_VBLANK_A"),
    ("delta", "IG_TRANS_VSYNC", "_TRANS_HTOTAL_A", "_TRANS_VSYNC_A"),
    ("delta", "IG_TRANS_PIPESRC", "_TRANS_HTOTAL_A", "_PIPEASRC"),

    #
    # TRANSCONF is the project's named hazard 2, and this row is the whole of it
    # in one line: the base is a PIPE offset while the register is indexed by
    # TRANSCODER. i915_reg.h:1698 defines TRANSCONF() via _MMIO_PIPE2, and
    # _TRANSACONF is 0x70008 -- in the pipe block. Subtracting the TRANSCODER
    # base here instead would yield 0x10008 and this row would fail, which is
    # exactly the protection wanted.
    #
    ("delta", "IG_PIPE_TRANSCONF", "PIPE_A_OFFSET", "_TRANSACONF"),

    ("value", "IG_TRANSCONF_ENABLE", "TRANSCONF_ENABLE"),
    ("value", "IG_TRANSCONF_STATE_ENABLE", "TRANSCONF_STATE_ENABLE"),

    #
    # The two halves of every timing register. HTOTAL_MASK is GENMASK(31, 16)
    # and HACTIVE_MASK is GENMASK(15, 0), so the shift and the normalised mask
    # come from opposite halves of the same word on purpose -- checking both
    # against the same mask would leave a swapped pair undetected.
    #
    ("shift", "IG_TIMING_HI_SHIFT", "HTOTAL_MASK"),
    ("mask", "IG_TIMING_HALF_MASK", "HACTIVE_MASK"),

    #
    # The plane layer. Offsets are relative to the plane's own CTL register, so
    # the base is _PLANE_CTL_1_A rather than a pipe base.
    #
    ("value", "IG_PLANE_CTL_BASE", "_PLANE_CTL_1_A"),
    ("delta", "IG_PLANE_CTL", "_PLANE_CTL_1_A", "_PLANE_CTL_1_A"),
    ("delta", "IG_PLANE_STRIDE", "_PLANE_CTL_1_A", "_PLANE_STRIDE_1_A"),
    ("delta", "IG_PLANE_POS", "_PLANE_CTL_1_A", "_PLANE_POS_1_A"),
    ("delta", "IG_PLANE_SIZE", "_PLANE_CTL_1_A", "_PLANE_SIZE_1_A"),
    ("delta", "IG_PLANE_KEYVAL", "_PLANE_CTL_1_A", "_PLANE_KEYVAL_1_A"),
    ("delta", "IG_PLANE_KEYMSK", "_PLANE_CTL_1_A", "_PLANE_KEYMSK_1_A"),
    ("delta", "IG_PLANE_SURF", "_PLANE_CTL_1_A", "_PLANE_SURF_1_A"),
    ("delta", "IG_PLANE_KEYMAX", "_PLANE_CTL_1_A", "_PLANE_KEYMAX_1_A"),
    ("delta", "IG_PLANE_OFFSET", "_PLANE_CTL_1_A", "_PLANE_OFFSET_1_A"),
    ("delta", "IG_PLANE_SURFLIVE", "_PLANE_CTL_1_A", "_PLANE_SURFLIVE_1_A"),
    ("delta", "IG_PLANE_CC_VAL", "_PLANE_CTL_1_A", "_PLANE_CC_VAL_1_A"),
    ("delta", "IG_PLANE_AUX_DIST", "_PLANE_CTL_1_A", "_PLANE_AUX_DIST_1_A"),

    ("value", "IG_PLANE_CTL_ENABLE", "PLANE_CTL_ENABLE"),

    #
    # Gen12 is icl+, so the FIVE-bit mask at shift 23 is the correct one. The
    # four-bit pre-icl pair is checked too, because IntelDisplay.hpp:463-472
    # defines it deliberately so tests/ can show the two disagreeing on Y210 /
    # Y212 / Y216 -- a constant kept for a negative demonstration still has to
    # be the RIGHT wrong value, or the demonstration proves nothing.
    #
    ("shift", "IG_PLANE_FORMAT_SHIFT", "PLANE_CTL_FORMAT_MASK_ICL"),
    ("mask", "IG_PLANE_FORMAT_MASK", "PLANE_CTL_FORMAT_MASK_ICL"),
    ("shift", "IG_PLANE_FORMAT_SHIFT_SKL", "PLANE_CTL_FORMAT_MASK_SKL"),
    ("mask", "IG_PLANE_FORMAT_MASK_SKL", "PLANE_CTL_FORMAT_MASK_SKL"),

    #
    # PLANE_STRIDE__MASK really does carry two underscores upstream
    # (skl_universal_plane_regs.h:109) -- it is not a typo here.
    #
    ("mask", "IG_PLANE_STRIDE_MASK", "PLANE_STRIDE__MASK"),

    #
    # The DDI layer, added 2026-08-17. Only THREE of its constants fit these row
    # kinds, and why is worth recording, because it is the difference between a
    # row that checks something and a row that skips.
    #
    # i915 spells the DDI fields as COMPOSED SHIFTS -- (7 << 24),
    # (0xf << TGL_TRANS_DDI_PORT_SHIFT) -- not as REG_GENMASK. i915_genmask()
    # returns None for every DDI name (measured 2026-08-17 by driving it over all
    # eleven of them), so a "mask" or "shift" row naming any of them would SKIP,
    # and a skip prints as a pass. The rest are derived in intel_ddi_checks().
    #
    ("delta", "IG_TRANS_DDI_FUNC_CTL", "_TRANS_HTOTAL_A", "_TRANS_DDI_FUNC_CTL_A"),
    ("value", "IG_DDI_FUNC_ENABLE", "TRANS_DDI_FUNC_ENABLE"),

    #
    # HAZARD 8 IN ONE ROW, and the i915 name carries the TGL_ prefix on purpose:
    # TRANS_DDI_PORT_SHIFT is 28 (pre-icl), TGL_TRANS_DDI_PORT_SHIFT is 27.
    # Naming the un-prefixed one would pin 28, this row would pass, and every
    # port decode on this silicon would name half the right port.
    #
    ("value", "IG_DDI_PORT_SHIFT", "TGL_TRANS_DDI_PORT_SHIFT"),

    ("shift", "IG_PLANE_POS_Y_SHIFT", "PLANE_POS_Y_MASK"),
    ("mask", "IG_PLANE_POS_X_MASK", "PLANE_POS_X_MASK"),
    ("shift", "IG_PLANE_HEIGHT_SHIFT", "PLANE_HEIGHT_MASK"),
    ("mask", "IG_PLANE_WIDTH_MASK", "PLANE_WIDTH_MASK"),
]


#
# IntelFramebuffer.hpp -- the DECODE layer, checked against the same vendored
# file. Same four row kinds, same dispatcher, deliberately a SEPARATE table:
# these rows resolve names in a different kext header, so one shared table
# would need every row to carry which header it belongs to, and a row that
# named the wrong header would resolve to None and report "not found" rather
# than "looked in the wrong place".
#
# The offset rows are "delta" against _PLANE_CTL_1_A because that is exactly
# what the header claims they are (IntelFramebuffer.hpp:73-79: DELTAS from
# 0x70180, computed from the vendored literals rather than from the capture's
# dump-stride column). The distinction is load-bearing: the capture prints a
# value at +0x2C and the header's prose block labels it OFFSET, but the real
# _PLANE_OFFSET_1_A is 0x701a4, i.e. +0x24. Only the vendored file settles it,
# and IG_FB_OFF_OFFSET is 0x24 here -- so this row is the gate that keeps the
# code correct while the prose block above it names a different address.
#
INTEL_FB_CHECKS = [
    ("delta", "IG_FB_OFF_CTL", "_PLANE_CTL_1_A", "_PLANE_CTL_1_A"),
    ("delta", "IG_FB_OFF_STRIDE", "_PLANE_CTL_1_A", "_PLANE_STRIDE_1_A"),
    ("delta", "IG_FB_OFF_POS", "_PLANE_CTL_1_A", "_PLANE_POS_1_A"),
    ("delta", "IG_FB_OFF_SIZE", "_PLANE_CTL_1_A", "_PLANE_SIZE_1_A"),
    ("delta", "IG_FB_OFF_SURF", "_PLANE_CTL_1_A", "_PLANE_SURF_1_A"),
    ("delta", "IG_FB_OFF_KEYMAX", "_PLANE_CTL_1_A", "_PLANE_KEYMAX_1_A"),
    ("delta", "IG_FB_OFF_OFFSET", "_PLANE_CTL_1_A", "_PLANE_OFFSET_1_A"),

    #
    # Whole-register bit values.
    #
    ("value", "IG_FB_CTL_ENABLE", "PLANE_CTL_ENABLE"),
    ("value", "IG_FB_CTL_RENDER_DECOMP",
              "PLANE_CTL_RENDER_DECOMPRESSION_ENABLE"),
    ("value", "IG_FB_CTL_ASYNC_FLIP", "PLANE_CTL_ASYNC_FLIP"),
    ("value", "IG_FB_SURF_ADDR_MASK", "PLANE_SURF_ADDR_MASK"),
    ("value", "IG_FB_SURF_DECRYPT", "PLANE_SURF_DECRYPT"),

    #
    # The five-bit ICL format field. This is the single most consequential row
    # in the table -- see the note above INTEL_FB_HEADER for why the wrong mask
    # yields a plausible answer rather than garbage.
    #
    ("shift", "IG_FB_CTL_FORMAT_SHIFT", "PLANE_CTL_FORMAT_MASK_ICL"),
    ("mask", "IG_FB_CTL_FORMAT_MASK", "PLANE_CTL_FORMAT_MASK_ICL"),

    ("shift", "IG_FB_CTL_TILING_SHIFT", "PLANE_CTL_TILED_MASK"),
    ("mask", "IG_FB_CTL_TILING_MASK", "PLANE_CTL_TILED_MASK"),

    ("mask", "IG_FB_STRIDE_MASK", "PLANE_STRIDE__MASK"),

    #
    # PLANE_SIZE's halves are INVERTED relative to PIPESRC (handoff15 §1:
    # 0x077F0437 vs 0x0437077F). So width comes from PLANE_WIDTH_MASK at
    # shift 0 and height from PLANE_HEIGHT_MASK at shift 16 -- the opposite
    # assignment to the transcoder path, and checking each against its own
    # vendored mask is what makes a transposed decoder fail here rather than
    # program cleanly and scramble the screen.
    #
    ("shift", "IG_FB_SIZE_WIDTH_SHIFT", "PLANE_WIDTH_MASK"),
    ("mask", "IG_FB_SIZE_HALF_MASK", "PLANE_WIDTH_MASK"),
    ("shift", "IG_FB_SIZE_HEIGHT_SHIFT", "PLANE_HEIGHT_MASK"),

    ("shift", "IG_FB_POS_X_SHIFT", "PLANE_POS_X_MASK"),
    ("mask", "IG_FB_POS_HALF_MASK", "PLANE_POS_X_MASK"),
    ("shift", "IG_FB_POS_Y_SHIFT", "PLANE_POS_Y_MASK"),
]


def strip_int_suffixes(body):
    return INT_SUFFIX_RE.sub(r"\1", body)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*?$", "", text, flags=re.MULTILINE)


def join_continuations(lines):
    """
    Fold backslash-continued #define bodies into one logical line.

    NVGspBoot.hpp composes NVGSP_BCR_CTRL_START_VALUE and
    NVGSP_DMATRFCMD_LOAD_IMEM across four and five lines respectively. Read
    line by line, the body of each is literally a single backslash, which
    parses as a define whose value cannot be resolved -- so the constant is
    reported as unparseable rather than checked, which is the quiet failure
    this whole script exists to prevent.

    NVIDIA's published headers put every define on one line, so folding is a
    no-op for them.
    """
    out, pending = [], None

    for line in lines:
        stripped = line.rstrip()
        continued = stripped.endswith("\\")
        if continued:
            stripped = stripped[:-1].rstrip()

        if pending is None:
            pending = stripped
        else:
            pending += " " + stripped.strip()

        if not continued:
            out.append(pending)
            pending = None

    if pending is not None:
        out.append(pending)

    return out


def parse_defines(path):
    """Return (plain_defines, param_defines) from a C header."""
    plain, param = {}, {}

    if not path.is_file():
        return plain, param

    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()

    for raw in join_continuations(lines):
        m = PARAM_DEFINE_RE.match(raw)
        if m:
            param[m.group(1)] = (m.group(2), strip_comments(m.group(3)).strip())
            continue

        m = DEFINE_RE.match(raw)
        if m:
            name = m.group(1)
            body = strip_comments(m.group(2)).strip()
            if body:
                plain[name] = body

    return plain, param


def merge_sources():
    plain, param, missing = {}, {}, []

    for path in SOURCE_HEADERS:
        if not path.is_file():
            missing.append(path)
            continue
        p, q = parse_defines(path)
        # First definition wins, so an addendum cannot silently shadow a base
        # header. Report collisions instead of hiding them.
        for k, v in p.items():
            plain.setdefault(k, v)
        for k, v in q.items():
            param.setdefault(k, v)

    return plain, param, missing


def resolve(name, table, depth=0):
    """Resolve a define to an int, following one level of aliasing."""
    if depth > 8 or name not in table:
        return None

    body = strip_int_suffixes(table[name].strip())

    if NUMERIC_RE.match(body):
        return int(body, 0)

    # Aliases such as: #define NV_USABLE_FB_SIZE_IN_MB  NV_PGC6_..._GROUP_42
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", body):
        return resolve(body, table, depth + 1)

    # Parenthesised constant arithmetic, e.g. (0x1234 + 0x10)
    if re.fullmatch(r"[\dxXa-fA-F\s()+\-*]+", body):
        try:
            return int(eval(body, {"__builtins__": {}}, {}))  # noqa: S307
        except Exception:
            return None

    #
    # Composed bit expressions, which is how NVGspBoot.hpp builds every value
    # it cares about:
    #
    #   #define NVGSP_DMATRFCMD_LOAD_IMEM \
    #       (((NVU32)1u << NV_PFALCON_FALCON_DMATRFCMD_IMEM_SHIFT) | ...)
    #
    # Three things separate these from the arithmetic above: C casts, shift
    # and bitwise operators, and symbolic operands that must themselves be
    # resolved. Left unhandled, NVGSP_DMATRFCMD_LOAD_IMEM and
    # NVGSP_BCR_CTRL_START_VALUE resolve to None -- and None is reported as
    # "unparseable", not as a wrong value, so the observed 0x614 and 0x111
    # would go unverified while the script still printed a clean score.
    #
    stripped = re.sub(r"\(\s*NVU(?:8|16|32|64)\s*\)", "", body)

    #
    # NVGspStage.hpp does not use the NVU aliases for its addressing window --
    # NV_GSP_FW_PHYS_LIMIT is written with a plain C cast:
    #
    #   #define NV_GSP_FW_PHYS_LIMIT \
    #       ((unsigned long long)((1ULL << NV_GSP_FW_ADDR_BITS) - 1ULL))
    #
    # Without this second strip, `unsigned`, `long` and `long` are each treated
    # as symbolic operands, none resolves, and the whole body reports as
    # "unparseable" -- the same silent-skip failure mode the NVU strip above
    # was added to close. Only type keywords can appear inside these parens, so
    # this can never remove a parenthesised sub-expression.
    #
    stripped = re.sub(
        r"\(\s*(?:unsigned|signed)?\s*"
        r"(?:long\s+long|long|int|short|char)(?:\s+int)?\s*\)",
        "", stripped)

    def substitute(match):
        inner = resolve(match.group(0), table, depth + 1)
        return "None" if inner is None else str(inner)

    #
    # The lookbehind is load-bearing. Without it this pattern matches INSIDE a
    # hex literal: in `0xFF` the `0` is skipped, the scan resumes at `x`, and
    # `xFF` matches [A-Za-z_][A-Za-z0-9_]* as though it were a symbol. resolve()
    # then returns None for it, "None" lands in the expression, and the whole
    # body reports as "unparseable" -- so a composed constant containing a hex
    # literal could never be verified at all.
    #
    # That went unnoticed because every composed expression in the kext until
    # 2026-08-13 used DECIMAL literals for its operands (`(NVU32)1u << SHIFT`,
    # `(1ULL << BITS) - 1ULL`). NVGSP_DMA_PHYSHI_MASK is the first to write one
    # in hex, and it failed as "not found or unparseable in NVGspBoot.hpp" --
    # wording that points at the header rather than at this line.
    #
    # Rejecting a preceding [0-9A-Za-z_] is correct rather than merely narrower:
    # a real C identifier can never be preceded by one of those characters, or
    # it would be part of the same token.
    #
    substituted = re.sub(r"(?<![0-9A-Za-z_])[A-Za-z_][A-Za-z0-9_]*",
                         substitute, stripped)

    if "None" in substituted:
        return None

    if not re.fullmatch(r"[\dxXa-fA-F\s()+\-*|&^<>~]+", substituted):
        return None

    try:
        return int(eval(substituted, {"__builtins__": {}}, {}))  # noqa: S307
    except Exception:
        return None


def resolve_bitrange(name, table):
    """Return (shift, mask) for an 'HI:LO' style NVIDIA field define."""
    if name not in table:
        return None

    m = BITRANGE_RE.match(table[name].strip())
    if not m:
        return None

    hi, lo = int(m.group(1)), int(m.group(2))
    if hi < lo:
        return None

    return lo, (1 << (hi - lo + 1)) - 1


class Report:
    def __init__(self, verbose):
        self.verbose = verbose
        self.ok = 0
        self.failures = []
        self.skipped = []

    def passed(self, label, detail):
        self.ok += 1
        if self.verbose:
            print(f"  ok    {label:<52} {detail}")

    def failed(self, label, detail):
        self.failures.append((label, detail))
        print(f"  FAIL  {label:<52} {detail}")

    def skip(self, label, detail):
        self.skipped.append((label, detail))
        print(f"  skip  {label:<52} {detail}")


def check_offset(rep, kext, src, kname, nname):
    kv = resolve(kname, kext)
    nv = resolve(nname, src)

    if kv is None:
        rep.failed(kname, "not found or unparseable in NVDecode.hpp")
        return
    if nv is None:
        rep.skip(kname, f"{nname} not found in NVIDIA headers")
        return
    if kv != nv:
        rep.failed(kname, f"kext 0x{kv:X} != NVIDIA {nname} 0x{nv:X}")
        return

    rep.passed(kname, f"0x{kv:X}")


def check_value(rep, kext, src, kname, nname):
    kv = resolve(kname, kext)
    nv = resolve(nname, src)

    if kv is None:
        rep.failed(kname, "not found or unparseable in NVDecode.hpp")
        return
    if nv is None:
        rep.skip(kname, f"{nname} not found in NVIDIA headers")
        return
    if kv != nv:
        rep.failed(kname, f"kext 0x{kv:X} != NVIDIA {nname} 0x{nv:X}")
        return

    rep.passed(kname, f"0x{kv:X}")


def check_field(rep, kext, src, kshift, kmask, nname):
    ks = resolve(kshift, kext)
    km = resolve(kmask, kext)
    derived = resolve_bitrange(nname, src)

    label = nname

    if ks is None or km is None:
        rep.failed(label, f"{kshift} / {kmask} missing from NVDecode.hpp")
        return
    if derived is None:
        rep.skip(label, "not found as an 'HI:LO' field in NVIDIA headers")
        return

    shift, mask = derived

    if ks != shift or km != mask:
        rep.failed(
            label,
            f"kext shift={ks} mask=0x{km:X}  vs  NVIDIA shift={shift} mask=0x{mask:X}",
        )
        return

    rep.passed(label, f"shift={shift} mask=0x{mask:X}")


def check_param_macro(rep, kext_param, src_param, kname, nname):
    if kname not in kext_param:
        rep.failed(kname, "parameterised macro missing from NVDecode.hpp")
        return
    if nname not in src_param:
        rep.skip(kname, f"{nname} not found in NVIDIA headers")
        return

    karg, kbody = kext_param[kname]
    narg, nbody = src_param[nname]

    # Evaluate both at several indices rather than comparing text, since the
    # kext deliberately reformats (i)*4 as (i) * 4.
    for i in (0, 1, 5):
        try:
            kv = eval(kbody, {"__builtins__": {}}, {karg: i})  # noqa: S307
            nv = eval(nbody, {"__builtins__": {}}, {narg: i})  # noqa: S307
        except Exception as exc:
            rep.skip(kname, f"could not evaluate: {exc}")
            return

        if kv != nv:
            rep.failed(kname, f"at i={i}: kext 0x{kv:X} != NVIDIA 0x{nv:X}")
            return

    base = eval(kbody, {"__builtins__": {}}, {karg: 0})  # noqa: S307
    rep.passed(kname, f"base 0x{base:X}, stride verified at i=0,1,5")


def sanity_checks(rep, kext):
    """Internal consistency the NVIDIA headers cannot verify for us."""
    arch = resolve("NV_ARCHITECTURE_GA100", kext)
    impl = resolve("NV_IMPLEMENTATION_GA107", kext)

    if arch is None or impl is None:
        rep.failed("NV_CHIP_ID_GA107", "cannot resolve arch/impl constants")
        return

    expected = (arch << 4) | impl

    if expected != 0x177:
        rep.failed(
            "NV_CHIP_ID_GA107",
            f"(0x{arch:X} << 4) | 0x{impl:X} = 0x{expected:X}, expected 0x177",
        )
        return

    rep.passed("NV_CHIP_ID_GA107", f"(0x{arch:X} << 4) | 0x{impl:X} = 0x{expected:X}")

    # The split-architecture reassembly the kext performs in
    # boot0Architecture(). Verify the widths line up so the high bit lands
    # exactly above the low field rather than overlapping or leaving a gap.
    width = resolve("NV_PMC_BOOT_0_ARCHITECTURE_0_WIDTH", kext)
    lo_mask = resolve("NV_PMC_BOOT_0_ARCHITECTURE_0_MASK", kext)

    if width is None or lo_mask is None:
        rep.failed("split-arch reassembly", "width or mask constant missing")
        return

    if lo_mask != (1 << width) - 1:
        rep.failed(
            "split-arch reassembly",
            f"ARCHITECTURE_0_MASK 0x{lo_mask:X} does not match WIDTH {width}",
        )
        return

    rep.passed("split-arch reassembly", f"WIDTH {width} consistent with mask 0x{lo_mask:X}")

    # A GA107 BOOT_0 value assembled from the kext's own constants should be
    # 0x177000xx. This is the number printed in DEVICE-IDS.md and the kext
    # README, so an error here means the docs are lying to you.
    ai_shift = resolve("NV_PMC_BOOT_0_ARCHITECTURE_0_SHIFT", kext)
    im_shift = resolve("NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT", kext)

    if ai_shift is None or im_shift is None:
        rep.failed("expected BOOT_0", "shift constants missing")
        return

    boot0 = ((arch & lo_mask) << ai_shift) | (impl << im_shift)

    if boot0 != 0x17700000:
        rep.failed(
            "expected BOOT_0",
            f"assembled 0x{boot0:08X}, docs claim 0x17700000",
        )
        return

    rep.passed("expected BOOT_0", f"0x{boot0:08X} (matches the documented value)")


def falcon_sanity_checks(rep, table):
    """
    Cross-header invariants for the Phase 4 address spaces.

    NVIDIA's headers never state the relationship between the three GSP bases
    in one place, so these are the checks that catch a collapsed or drifted
    base -- the failure mode that writes real registers at the wrong address
    without ever faulting.
    """
    pgsp_base  = resolve("NV_PGSP_BASE", table)
    pgsp_limit = resolve("NV_PGSP_LIMIT", table)
    falcon2    = resolve("NV_FALCON2_GSP_BASE", table)
    block      = resolve("NV_FALCON_BLOCK_SIZE", table)
    mailbox0   = resolve("NV_PFALCON_FALCON_MAILBOX0", table)
    abs_mbox0  = resolve("NV_PGSP_FALCON_MAILBOX0", table)

    if None in (pgsp_base, pgsp_limit, falcon2, block, mailbox0, abs_mbox0):
        rep.failed("GSP base invariants", "one or more base constants missing")
        return

    #
    # The anchor. dev_falcon_v4.h gives MAILBOX0 as engine-relative 0x40;
    # dev_gsp.h independently spells the absolute address. Agreement proves
    # NV_PGSP_BASE, using two NVIDIA headers that do not reference each other.
    #
    if pgsp_base + mailbox0 != abs_mbox0:
        rep.failed(
            "NV_PGSP_BASE",
            f"0x{pgsp_base:X} + MAILBOX0 0x{mailbox0:X} = "
            f"0x{pgsp_base + mailbox0:X}, but dev_gsp.h says 0x{abs_mbox0:X}",
        )
        return

    rep.passed("NV_PGSP_BASE",
               f"0x{pgsp_base:X} confirmed by two independent NVIDIA headers")

    #
    # RISC-V lives one 4 KiB page above the Falcon block. Collapsing these two
    # bases puts every RISC-V access inside the Falcon register file, where the
    # offsets are all valid registers -- so it corrupts silently.
    #
    if falcon2 - pgsp_base != block:
        rep.failed(
            "NV_FALCON2_GSP_BASE",
            f"0x{falcon2:X} - 0x{pgsp_base:X} = 0x{falcon2 - pgsp_base:X}, "
            f"expected one Falcon block 0x{block:X}",
        )
        return

    rep.passed("NV_FALCON2_GSP_BASE",
               f"0x{falcon2:X} is exactly one 0x{block:X} block above the "
               f"Falcon base")

    #
    # Everything translated through either accessor must land inside NV_PGSP.
    # An escape writes into a neighbouring engine, which is worse than a fault.
    #
    riscv_top = resolve("NV_PRISCV_RISCV_BCR_CTRL", table)

    if riscv_top is None:
        rep.failed("GSP aperture bound", "NV_PRISCV_RISCV_BCR_CTRL missing")
        return

    highest = falcon2 + riscv_top

    if highest > pgsp_limit:
        rep.failed(
            "GSP aperture bound",
            f"highest RISC-V address 0x{highest:X} exceeds "
            f"NV_PGSP_LIMIT 0x{pgsp_limit:X}",
        )
        return

    rep.passed("GSP aperture bound",
               f"highest translated address 0x{highest:X} <= "
               f"limit 0x{pgsp_limit:X}")

    #
    # And the whole aperture must fit inside the 16 MiB BAR0 mapping the kext
    # actually makes.
    #
    bar0_len = resolve("NV_BAR0_EXPECTED_LENGTH", table)

    if bar0_len is None:
        rep.failed("GSP inside BAR0", "NV_BAR0_EXPECTED_LENGTH missing")
        return

    if pgsp_limit >= bar0_len:
        rep.failed(
            "GSP inside BAR0",
            f"NV_PGSP_LIMIT 0x{pgsp_limit:X} is outside the "
            f"0x{bar0_len:X} aperture",
        )
        return

    rep.passed("GSP inside BAR0",
               f"limit 0x{pgsp_limit:X} < aperture 0x{bar0_len:X}")


def gspboot_sanity_checks(rep, table):
    """
    Verify NVGspBoot.hpp's composed constants.

    NVIDIA publishes no symbol for any of these -- they are values witnessed in
    the MMIO trace, assembled in NVGspBoot.hpp out of NVFalcon.hpp field
    positions. So the check is not kext-vs-NVIDIA like everything above it;
    it is composed-value-vs-observed-value, plus cross-header agreement on the
    field positions the composition depends on.

    Until this block existed, NVGspBoot.hpp was parsed by nothing. Its two
    most consequential constants -- the 0x614 DMA trigger and the 0x111 core
    release -- were built from shifts that no gate compared against anything.
    """
    #
    # DMATRFCMD trigger. The trace wrote 0x614. A plain field decode of
    # IMEM=1 / SIZE=6 gives 0x610; the missing 0x4 is bit 2, the low bit of
    # SEC (3:2). NVGspBoot.hpp:76 records that reasoning, and this check is
    # what makes it enforceable: anyone who "corrects" the SEC term to match
    # a naive decode breaks this, not a comment.
    #
    cmd = resolve("NVGSP_DMATRFCMD_LOAD_IMEM", table)
    if cmd is None:
        rep.failed("NVGSP_DMATRFCMD_LOAD_IMEM",
                   "composed value did not resolve")
    elif cmd != 0x614:
        rep.failed("NVGSP_DMATRFCMD_LOAD_IMEM",
                   f"composes to 0x{cmd:X}, trace observed 0x614")
    else:
        rep.passed("NVGSP_DMATRFCMD_LOAD_IMEM",
                   "0x614 (matches the observed trigger)")

    #
    # BCR_CTRL core release. The trace wrote 0x111 = VALID | CORE_SELECT |
    # BRFETCH.
    #
    bcr = resolve("NVGSP_BCR_CTRL_START_VALUE", table)
    observed = resolve("NV_PRISCV_RISCV_BCR_CTRL_OBSERVED_START", table)

    if bcr is None:
        rep.failed("NVGSP_BCR_CTRL_START_VALUE",
                   "composed value did not resolve")
    elif observed is None:
        rep.skip("NVGSP_BCR_CTRL_START_VALUE",
                 "NV_PRISCV_RISCV_BCR_CTRL_OBSERVED_START not in NVFalcon.hpp")
    elif bcr != observed:
        rep.failed("NVGSP_BCR_CTRL_START_VALUE",
                   f"composes to 0x{bcr:X}, NVFalcon.hpp observed "
                   f"0x{observed:X}")
    else:
        rep.passed("NVGSP_BCR_CTRL_START_VALUE",
                   f"0x{bcr:X} (agrees with NVFalcon.hpp)")

    #
    # BRFETCH. NVGspBoot.hpp carries its own NVGSP_BCR_CTRL_BRFETCH_SHIFT
    # with a comment saying NVFalcon.hpp does not spell the bit. NVFalcon.hpp
    # DOES spell it, so the two must agree -- and if they ever diverge, the
    # duplicate is the one that is wrong.
    #
    local_shift = resolve("NVGSP_BCR_CTRL_BRFETCH_SHIFT", table)
    falcon_shift = resolve("NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_SHIFT", table)

    if local_shift is None or falcon_shift is None:
        rep.skip("BRFETCH shift agreement",
                 "one of the two BRFETCH shift constants is missing")
    elif local_shift != falcon_shift:
        rep.failed(
            "BRFETCH shift agreement",
            f"NVGspBoot.hpp says {local_shift}, "
            f"NVFalcon.hpp says {falcon_shift}",
        )
    else:
        rep.passed("BRFETCH shift agreement",
                   f"both headers say bit {local_shift}")

    #
    # The BROM parameter values, against the copies NVFalcon.hpp records
    # independently from the same trace.
    #
    for local, falcon in (
        ("NVGSP_OBSERVED_ENGIDMASK", "NV_PFALCON2_GSP_OBSERVED_ENGIDMASK"),
        ("NVGSP_OBSERVED_UCODE_ID", "NV_PFALCON2_GSP_OBSERVED_UCODE_ID"),
    ):
        lv = resolve(local, table)
        fv = resolve(falcon, table)

        if lv is None:
            rep.failed(local, "not found or unparseable in NVGspBoot.hpp")
        elif fv is None:
            rep.skip(local, f"{falcon} not found in NVFalcon.hpp")
        elif lv != fv:
            rep.failed(local, f"0x{lv:X} != NVFalcon.hpp {falcon} 0x{fv:X}")
        else:
            rep.passed(local, f"0x{lv:X} (agrees with NVFalcon.hpp)")

    #
    # Chunk size. NVGSP_DMA_CHUNK_BYTES is the byte count the state machine
    # advances MOFFS/FBOFFS by per trigger, and DMATRFCMD_SIZE_256B is the
    # field value that makes the engine move that many bytes. They are two
    # spellings of one hardware fact; if they disagree, every chunk after the
    # first targets the wrong offset without faulting.
    #
    chunk = resolve("NVGSP_DMA_CHUNK_BYTES", table)
    size_code = resolve("NV_PFALCON_FALCON_DMATRFCMD_SIZE_256B", table)

    if chunk is None or size_code is None:
        rep.skip("chunk size vs SIZE field",
                 "chunk-size or SIZE_256B constant missing")
    elif chunk != 256:
        rep.failed("chunk size vs SIZE field",
                   f"NVGSP_DMA_CHUNK_BYTES is {chunk}, SIZE_256B moves 256")
    elif size_code != 0x06:
        rep.failed("chunk size vs SIZE field",
                   f"SIZE_256B is 0x{size_code:X}, expected 0x6")
    else:
        rep.passed("chunk size vs SIZE field",
                   "256 bytes, SIZE field 0x6")

    #
    # PARAADDR(0) is a WRITTEN VALUE (0x5A4), not a register offset, and no
    # other header records it. Pin the literal so a typo cannot drift it.
    #
    para = resolve("NVGSP_OBSERVED_PARAADDR0", table)
    if para is None:
        rep.failed("NVGSP_OBSERVED_PARAADDR0",
                   "not found or unparseable in NVGspBoot.hpp")
    elif para != 0x5A4:
        rep.failed("NVGSP_OBSERVED_PARAADDR0",
                   f"0x{para:X}, trace observed 0x5A4")
    else:
        rep.passed("NVGSP_OBSERVED_PARAADDR0",
                   "0x5A4 (matches the trace)")

    #
    # NVGSP_DMA_PHYSHI_MASK -- which bits of the UNSHIFTED high word survive the
    # packing. Added 2026-08-13 alongside nvGspDmaPhysHiFits(), which
    # NVGSP_STATE_FIRMWARE_LOAD refuses on: physAddrHi is 32 bits wide in
    # NVGspFirmwareImage and the register pair carries fewer, so a high word
    # above the mask is TRUNCATED by the packing rather than rejected by it.
    #
    # Two independent derivations of the same quantity, which is the only reason
    # this check can fail:
    #
    #   nvGspDmaTrfBase()  does physHi << 24 into a 32-bit register, so physHi's
    #                      low 8 bits survive there -- the 0xFF term.
    #   nvGspDmaTrfBase1() does (physHi >> 8) & DMATRFBASE1_BASE_MASK, so
    #                      BASE1's width contributes above those 8.
    #
    # The header composes the constant from DMATRFBASE1_BASE_MASK rather than
    # writing a literal, so this is NOT a literal-match tripwire. It recomputes
    # the composition from the field mask resolved out of the table and compares.
    # A widened or narrowed BASE1 field moves both sides together and this still
    # passes; a hand-edited 0xFF term, or a mask that stopped tracking the field,
    # fails here.
    #
    physhi = resolve("NVGSP_DMA_PHYSHI_MASK", table)
    base1_mask = resolve("NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK", table)

    if physhi is None:
        rep.failed("NVGSP_DMA_PHYSHI_MASK",
                   "not found or unparseable in NVGspBoot.hpp")
    elif base1_mask is None:
        rep.skip("NVGSP_DMA_PHYSHI_MASK",
                 "NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK not resolvable -- "
                 "cannot recompute the composition")
    else:
        expect = 0xFF | (base1_mask << 8)
        if physhi != expect:
            rep.failed("NVGSP_DMA_PHYSHI_MASK",
                       f"0x{physhi:X}, expected 0x{expect:X} "
                       f"(0xFF from physHi << 24, plus "
                       f"DMATRFBASE1_BASE_MASK 0x{base1_mask:X} << 8)")
        else:
            rep.passed("NVGSP_DMA_PHYSHI_MASK",
                       f"0x{physhi:X}, {physhi.bit_length()} bits of physHi "
                       f"(8 via BASE + {base1_mask.bit_length()} via BASE1)")


#
# NVGspStage.hpp -- the Phase 5 DMA addressing window.
#
# Nothing here is transcribed from an NVIDIA header, so there is no offset table
# to compare against. What CAN go wrong is the relationship between the three
# constants and the field widths they were derived from:
#
#   - NV_GSP_FW_ADDR_BITS must equal the width DMATRFBASE + DMATRFBASE1 reach.
#   - NV_GSP_FW_PHYS_MASK is an ALLOCATION CONSTRAINT with its low 8 bits
#     CLEARED for 256-byte alignment.
#   - NV_GSP_FW_PHYS_LIMIT is a MAGNITUDE CEILING with every low bit SET.
#
# The mask and the limit differ in exactly their low 8 bits and nowhere else.
# The header says a reviewer who sees "41-bit mask, 41-bit check" will read a
# mask-as-ceiling comparison as correct when it silently rejects every address
# in the low 256 bytes of a valid region. These checks are what keep that
# distinction mechanical rather than a comment.
#
def stage_sanity_checks(rep, table, src):
    bits = resolve("NV_GSP_FW_ADDR_BITS", table)
    mask = resolve("NV_GSP_FW_PHYS_MASK", table)
    limit = resolve("NV_GSP_FW_PHYS_LIMIT", table)
    chunk = resolve("NVGSP_DMA_CHUNK_BYTES", table)

    #
    # WHAT CAN AND CANNOT BE DERIVED HERE -- rewritten 2026-08-10 for t-53137f.
    #
    # The previous form derived 41 from the reading that DMATRFBASE1 carries
    # PHYSICAL bits 40:32, overlapping the 39:32 the shifted BASE already
    # carries. That reading was WRONG. NVIDIA's own driver derives both halves
    # from the SAME shifted quantity:
    #
    #   reference/.../ampere/kernel_gsp_falcon_ga102.c:122-123
    #       DMATRFBASE  = NvU64_LO32(srcPhysAddr >> 8)
    #       DMATRFBASE1 = NvU64_HI32(srcPhysAddr >> 8) & 0x1FF
    #
    # so BASE1 is a CONTINUATION of the shifted value, not an overlay on the
    # unshifted one.
    #
    # With the field widths NVIDIA's header CURRENTLY publishes -- BASE 32 bits,
    # BASE1 9 bits -- that puts BASE1 at physical bits 48:40 and the pair's
    # reach at 49 bits, well above 41. Those three numbers are quoted here only
    # to make the paragraph readable. THE CODE NEVER READS THEM: the check below
    # computes base_bits + base1_bits + 8 from the widths it resolves out of the
    # header, and prints the result under --verbose. If a header revision changes
    # either width, the computed reach moves and this prose goes stale WITHOUT
    # the check failing -- prose is not a tripwire. Trust the printed number
    # over this paragraph.
    #
    # NV_GSP_FW_ADDR_BITS is deliberately still 41. Under the corrected reading
    # it is no longer a DERIVED hardware width -- it is a POLICY CEILING this
    # driver imposes, and it was left where it was rather than widened because
    # refusing an address the hardware would have accepted is safe while
    # admitting one it would truncate is not. Widening it needs hardware
    # evidence.
    #
    # So the honest checks are relational, not an equality against a derivation:
    #
    #   1. The ceiling must not EXCEED what the register pair can express --
    #      otherwise the driver admits addresses the hardware truncates, which
    #      is the exact silent-wrong-page failure the constant exists to stop.
    #   2. The ceiling must exceed what DMATRFBASE reaches ALONE, or BASE1 is
    #      dead weight and the whole pair is pointless.
    #   3. The literal must still match POLICY_BITS below, so a silent edit to
    #      the header fails here rather than passing unnoticed. That is a
    #      tripwire on an intentional value, and it is labelled as one -- not a
    #      derivation dressed up as a check.
    #
    # Both field widths come from NVIDIA's headers via the resolved table, so a
    # transcription error in either one still moves the computed reach.
    #
    # NVIDIA's HI:LO field defines live in the SOURCE table, not the kext one --
    # the kext spells its fields as separate _SHIFT/_MASK pairs. Resolving these
    # from `src` is deliberate: the width must come from NVIDIA's published
    # header, not from a number this project transcribed.
    POLICY_BITS = 41

    base_w = resolve_bitrange("NV_PFALCON_FALCON_DMATRFBASE_BASE", src)
    base1_w = resolve_bitrange("NV_PFALCON_FALCON_DMATRFBASE1_BASE", src)

    hw_reach = None
    base_only = None
    if base_w is not None and base1_w is not None:
        # (shift, mask) -> width in bits
        base_bits = base_w[1].bit_length()
        base1_bits = base1_w[1].bit_length()
        # BASE holds shifted bits (base_bits-1):0, i.e. physical
        # (base_bits + 7):8 -- so BASE alone reaches base_bits + 8 bits.
        base_top = base_bits + 8 - 1
        base_only = base_top + 1
        # BASE1 holds shifted bits (32 + base1_bits - 1):32, i.e. physical
        # (40 + base1_bits - 1):40. The pair reaches one past BASE1's top.
        base1_top = 8 + 32 + base1_bits - 1
        hw_reach = base1_top + 1

    if bits is None:
        rep.failed("NV_GSP_FW_ADDR_BITS", "not found in NVGspStage.hpp")
    elif hw_reach is None:
        rep.skip("NV_GSP_FW_ADDR_BITS",
                 "DMATRFBASE_BASE / DMATRFBASE1_BASE not resolvable -- "
                 "cannot compute the register pair's reach")
    elif bits > hw_reach:
        rep.failed("NV_GSP_FW_ADDR_BITS",
                   f"{bits} EXCEEDS the register pair's reach: DMATRFBASE "
                   f"{base_bits}-bit field (physical {base_top}:8) plus "
                   f"DMATRFBASE1 {base1_bits}-bit field (physical "
                   f"{base1_top}:40) reach {hw_reach}")
    elif bits <= base_only:
        rep.failed("NV_GSP_FW_ADDR_BITS",
                   f"{bits} is within DMATRFBASE alone ({base_only} bits), so "
                   f"DMATRFBASE1 contributes nothing")
    elif bits != POLICY_BITS:
        rep.failed("NV_GSP_FW_ADDR_BITS",
                   f"{bits}, but this file records the policy ceiling as "
                   f"{POLICY_BITS}. The value is intentional, not derived -- "
                   f"change both together and say why in NVGspStage.hpp")
    else:
        rep.passed("NV_GSP_FW_ADDR_BITS",
                   f"{bits}, policy ceiling below the pair's {hw_reach}-bit "
                   f"reach (DMATRFBASE {base_bits}b {base_top}:8 + "
                   f"DMATRFBASE1 {base1_bits}b {base1_top}:40)")

    #
    # The ceiling is (1 << bits) - 1 -- every low bit SET. Recomputed here from
    # the resolved bit count rather than pinned to a literal, so changing
    # NV_GSP_FW_ADDR_BITS moves both together or fails loudly.
    #
    if limit is None:
        rep.failed("NV_GSP_FW_PHYS_LIMIT",
                   "not found or unparseable in NVGspStage.hpp")
    elif bits is not None:
        expect = (1 << bits) - 1
        if limit != expect:
            rep.failed("NV_GSP_FW_PHYS_LIMIT",
                       f"0x{limit:X}, expected 0x{expect:X} "
                       f"((1 << {bits}) - 1)")
        else:
            rep.passed("NV_GSP_FW_PHYS_LIMIT",
                       f"0x{limit:X}, every low bit set")

    #
    # The mask must be the limit with the alignment bits cleared -- NOT a
    # second, independently-written literal. If these two ever stop differing
    # in exactly the chunk-alignment bits, one of them was edited alone.
    #
    if mask is None:
        rep.failed("NV_GSP_FW_PHYS_MASK", "not found in NVGspStage.hpp")
    elif limit is not None and chunk is not None:
        expect = limit & ~(chunk - 1)
        if mask != expect:
            rep.failed("NV_GSP_FW_PHYS_MASK",
                       f"0x{mask:X}, expected 0x{expect:X} "
                       f"(LIMIT with the low {chunk}-byte bits cleared)")
        else:
            rep.passed("NV_GSP_FW_PHYS_MASK",
                       f"0x{mask:X}, low {chunk}-byte alignment bits cleared")

    #
    # And the two must NOT be equal. This is the specific confusion the header
    # documents as having been written and caught on 2026-08-10.
    #
    if mask is not None and limit is not None:
        if mask == limit:
            rep.failed("mask vs limit",
                       "MASK and LIMIT are identical -- an allocation "
                       "constraint has been conflated with a magnitude "
                       "ceiling")
        elif (limit - mask) != (chunk - 1 if chunk else None):
            rep.failed("mask vs limit",
                       f"differ by 0x{limit - mask:X}, expected "
                       f"0x{(chunk - 1) if chunk else 0:X}")
        else:
            rep.passed("mask vs limit",
                       f"differ in exactly the low {chunk} alignment bits")


def merge_i915_sources():
    """
    Parse the vendored i915 headers into one table.

    Deliberately NOT folded into merge_sources() -- see the note above
    I915_HEADERS. First definition wins here as well, so an addendum cannot
    silently shadow a base header.
    """
    plain, missing = {}, []

    for path in I915_HEADERS:
        if not path.is_file():
            missing.append(path)
            continue
        p, _ = parse_defines(path)
        for k, v in p.items():
            plain.setdefault(k, v)

    return plain, missing


def i915_resolve(name, table):
    """
    Resolve an i915 constant to an int.

    REG_BIT / REG_GENMASK are decoded here rather than handed to resolve(),
    which would read `REG_BIT` as a symbolic operand, fail to find it, and
    report the whole body as unparseable. An unparseable reference makes a
    check SKIP, and a skip is a silent pass.
    """
    if name not in table:
        return None

    body = strip_int_suffixes(table[name].strip())

    m = I915_REG_BIT_RE.match(body)
    if m:
        return 1 << int(m.group(1))

    m = I915_GENMASK_RE.match(body)
    if m:
        hi, lo = int(m.group(1)), int(m.group(2))
        if hi < lo:
            return None
        return ((1 << (hi - lo + 1)) - 1) << lo

    return resolve(name, table)


def i915_genmask(name, table):
    """Return (shift, bit-0-normalised mask) for an i915 REG_GENMASK define."""
    if name not in table:
        return None

    m = I915_GENMASK_RE.match(strip_int_suffixes(table[name].strip()))
    if not m:
        return None

    hi, lo = int(m.group(1)), int(m.group(2))
    if hi < lo:
        return None

    return lo, (1 << (hi - lo + 1)) - 1


def ig_switch_cases(text, func):
    """
    Return {ordinal: value} for the case arms inside `func`'s body.

    Brace-matched from the opening brace of the definition rather than
    regexed over the whole file, because igTransBase() and igPipeBase() use
    the IDENTICAL case labels -- a file-wide scan would merge the two sets
    and silently check each address against the wrong block.
    """
    anchor = re.search(r"\b" + re.escape(func) + r"\s*\([^)]*\)\s*\{", text)
    if not anchor:
        return None

    start = text.rindex("{", anchor.start(), anchor.end())
    depth = 0
    body = None

    for j in range(start, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                body = text[start:j]
                break

    if body is None:
        return None

    return {m.group(1): int(m.group(2), 16)
            for m in IG_CASE_RE.finditer(strip_comments(body))}


#
# The twelve switch bases, paired with the i915 constant each one must equal.
# These are the values that matter most on this track and the ones no #define
# exists for, so nothing else in this gate can reach them.
#
IG_TRANS_BASE_EXPECT = [
    ("A", "TRANSCODER_A_OFFSET"),
    ("B", "TRANSCODER_B_OFFSET"),
    ("C", "TRANSCODER_C_OFFSET"),
    ("D", "TRANSCODER_D_OFFSET"),
    ("DSI0", "TRANSCODER_DSI0_OFFSET"),
    ("DSI1", "TRANSCODER_DSI1_OFFSET"),
]

IG_PIPE_BASE_EXPECT = [
    ("A", "PIPE_A_OFFSET"),
    ("B", "PIPE_B_OFFSET"),
    ("C", "PIPE_C_OFFSET"),
    ("D", "PIPE_D_OFFSET"),
    ("DSI0", "PIPE_DSI0_OFFSET"),
    ("DSI1", "PIPE_DSI1_OFFSET"),
]


def run_intel_checks(rep, kext, src):
    #
    # Both of these would otherwise turn every row below into a skip, and the
    # gate would stay green while checking nothing at all.
    #
    if not kext:
        rep.failed("IntelDisplay.hpp",
                   "no #defines parsed -- every Intel check would skip, "
                   "and a skip is a silent pass")
        return
    if not src:
        rep.failed("reference/i915",
                   "no vendored i915 headers parsed -- every Intel check "
                   "would skip, and a skip is a silent pass")
        return

    for row in INTEL_CHECKS:
        kind = row[0]

        if kind == "delta":
            _, kname, ibase, iname = row
            kv = resolve(kname, kext)
            base = i915_resolve(ibase, src)
            absolute = i915_resolve(iname, src)

            if kv is None:
                rep.failed(kname, "not found or unparseable in IntelDisplay.hpp")
                continue
            if base is None or absolute is None:
                gone = ibase if base is None else iname
                rep.skip(kname, f"{gone} not found in vendored i915 headers")
                continue

            want = absolute - base
            if kv != want:
                rep.failed(kname,
                           f"0x{kv:X} != {iname} - {ibase} "
                           f"(0x{absolute:X} - 0x{base:X} = 0x{want:X})")
            else:
                rep.passed(kname, f"0x{kv:X} == {iname} - {ibase}")

        elif kind == "value":
            _, kname, iname = row
            kv = resolve(kname, kext)
            nv = i915_resolve(iname, src)

            if kv is None:
                rep.failed(kname, "not found or unparseable in IntelDisplay.hpp")
                continue
            if nv is None:
                rep.skip(kname, f"{iname} not found in vendored i915 headers")
                continue

            if kv != nv:
                rep.failed(kname, f"0x{kv:X} != {iname} 0x{nv:X}")
            else:
                rep.passed(kname, f"0x{kv:X} == {iname}")

        elif kind in ("shift", "mask"):
            _, kname, iname = row
            kv = resolve(kname, kext)
            pair = i915_genmask(iname, src)

            if kv is None:
                rep.failed(kname, "not found or unparseable in IntelDisplay.hpp")
                continue
            if pair is None:
                rep.skip(kname,
                         f"{iname} not found, or not a REG_GENMASK, in the "
                         f"vendored i915 headers")
                continue

            shift, mask = pair
            want = shift if kind == "shift" else mask
            what = "low bit" if kind == "shift" else "mask normalised to bit 0"

            if kv != want:
                rep.failed(kname,
                           f"0x{kv:X} != {iname} {what} 0x{want:X}")
            else:
                rep.passed(kname, f"0x{kv:X} == {iname} {what}")

        else:
            rep.failed(str(row), f"unknown INTEL_CHECKS kind {kind!r}")


def run_intel_fb_checks(rep, kext, src):
    #
    # The same two refusals as run_intel_checks(), and for the same reason: an
    # empty table on either side turns every row below into a skip, and a skip
    # prints as a pass. A rename or a moved file is the realistic way this
    # happens, and the failure mode is a gate that stays green while reading
    # nothing at all.
    #
    if not kext:
        rep.failed("IntelFramebuffer.hpp",
                   "no #defines parsed -- every framebuffer check would "
                   "skip, and a skip is a silent pass")
        return
    if not src:
        rep.failed("reference/i915",
                   "no vendored i915 headers parsed -- every framebuffer "
                   "check would skip, and a skip is a silent pass")
        return

    for row in INTEL_FB_CHECKS:
        kind = row[0]

        if kind == "delta":
            _, kname, ibase, iname = row
            kv = resolve(kname, kext)
            base = i915_resolve(ibase, src)
            absolute = i915_resolve(iname, src)

            if kv is None:
                rep.failed(kname,
                           "not found or unparseable in IntelFramebuffer.hpp")
                continue
            if base is None or absolute is None:
                gone = ibase if base is None else iname
                rep.skip(kname, f"{gone} not found in vendored i915 headers")
                continue

            want = absolute - base
            if kv != want:
                rep.failed(kname,
                           f"0x{kv:X} != {iname} - {ibase} "
                           f"(0x{absolute:X} - 0x{base:X} = 0x{want:X})")
            else:
                rep.passed(kname, f"0x{kv:X} == {iname} - {ibase}")

        elif kind == "value":
            _, kname, iname = row
            kv = resolve(kname, kext)
            nv = i915_resolve(iname, src)

            if kv is None:
                rep.failed(kname,
                           "not found or unparseable in IntelFramebuffer.hpp")
                continue
            if nv is None:
                rep.skip(kname, f"{iname} not found in vendored i915 headers")
                continue

            if kv != nv:
                rep.failed(kname, f"0x{kv:X} != {iname} 0x{nv:X}")
            else:
                rep.passed(kname, f"0x{kv:X} == {iname}")

        elif kind in ("shift", "mask"):
            _, kname, iname = row
            kv = resolve(kname, kext)
            pair = i915_genmask(iname, src)

            if kv is None:
                rep.failed(kname,
                           "not found or unparseable in IntelFramebuffer.hpp")
                continue
            if pair is None:
                rep.skip(kname,
                         f"{iname} not found, or not a REG_GENMASK, in the "
                         f"vendored i915 headers")
                continue

            shift, mask = pair
            want = shift if kind == "shift" else mask
            what = "low bit" if kind == "shift" else "mask normalised to bit 0"

            if kv != want:
                rep.failed(kname,
                           f"0x{kv:X} != {iname} {what} 0x{want:X}")
            else:
                rep.passed(kname, f"0x{kv:X} == {iname} {what}")

        else:
            rep.failed(str(row), f"unknown INTEL_FB_CHECKS kind {kind!r}")


def intel_fb_sanity_checks(rep, kext, src):
    """
    The claims IntelFramebuffer.hpp makes that no single row can express.

    Every one of these is a RELATIONSHIP between two constants, or between a
    constant and a number witnessed on the target laptop. A per-row comparison
    against a vendored mask cannot see any of them: each side is individually
    correct and the pair is still wrong.
    """
    if not kext:
        return

    #
    # src arrives as None or {} when the vendored tree is missing or truncated,
    # and i915_genmask() indexes it directly -- None raises TypeError from
    # inside the gate, and a traceback reads as a BROKEN GATE rather than as a
    # missing corpus. Normalised here, then refused loudly below rather than
    # skipped: run_intel_fb_checks() has already failed on the same condition,
    # and per the paragraph-wide-negation finding a skip prints as a pass.
    #
    if not src:
        src = {}

    #
    # 1. The five-bit ICL field really must DIFFER from the four-bit SKL one.
    #
    # This is the row that catches the whole class of "decoded with the wrong
    # mask". If someone widens or narrows the header's field to match the SKL
    # reading, the shift/mask rows above go red -- but only while the vendored
    # file still spells both. Pinning the DISAGREEMENT is what survives an
    # upstream cleanup that deletes one of the two names.
    #
    icl = i915_genmask("PLANE_CTL_FORMAT_MASK_ICL", src)
    skl = i915_genmask("PLANE_CTL_FORMAT_MASK_SKL", src)
    if icl is None or skl is None:
        rep.failed("IG_FB_CTL_FORMAT_MASK",
                   "one of the two vendored format masks is missing, so the "
                   "icl-vs-skl disagreement cannot be checked")
    elif icl == skl:
        rep.failed("IG_FB_CTL_FORMAT_MASK",
                   "the vendored ICL and SKL format masks are identical "
                   f"({icl}), so this header's five-bit choice is untested")
    else:
        rep.passed("IG_FB_CTL_FORMAT_MASK",
                   f"icl {icl} differs from skl {skl} as the header claims")

    #
    # 2. The observed CTL must decode DIFFERENTLY under the two masks.
    #
    # 0x82009000 is not an arbitrary example -- it is the one live capture, and
    # it is precisely the case where a wrong mask yields a plausible answer
    # rather than garbage: 4 (XRGB_8888) under the correct five-bit reading,
    # 2 (XRGB_2101010) under the four-bit one. Both are formats the vendored
    # header names. A driver that picked the wrong one would compute 4 bytes
    # per pixel either way here, but the two codes diverge on other planes.
    #
    ctl = resolve("IG_FB_CTL_OBSERVED_2026_08_16", kext)
    shift = resolve("IG_FB_CTL_FORMAT_SHIFT", kext)
    mask = resolve("IG_FB_CTL_FORMAT_MASK", kext)

    if ctl is None or shift is None or mask is None:
        rep.failed("IG_FB_CTL_OBSERVED_2026_08_16",
                   "the observed CTL or its format field constants do not "
                   "resolve, so the capture is checked against nothing")
    else:
        got = (ctl >> shift) & mask
        xrgb8888 = resolve("IG_FB_FORMAT_XRGB_8888", kext)
        if xrgb8888 is None:
            rep.failed("IG_FB_FORMAT_XRGB_8888", "does not resolve")
        elif got != xrgb8888:
            rep.failed("IG_FB_CTL_OBSERVED_2026_08_16",
                       f"decodes to format {got} under this header's field, "
                       f"but the eDP panel was a 32-bpp desktop, which is "
                       f"XRGB_8888 = {xrgb8888}")
        else:
            rep.passed("IG_FB_CTL_OBSERVED_2026_08_16",
                       f"decodes to XRGB_8888 ({got}) on the observed capture")

        if skl is not None:
            skl_shift, skl_mask = skl
            wrong = (ctl >> skl_shift) & skl_mask
            if wrong == got:
                rep.failed("IG_FB_CTL_FORMAT_SHIFT",
                           "the observed CTL decodes identically under the "
                           "four-bit SKL mask, so this capture cannot "
                           "separate the correct reading from the wrong one")
            else:
                rep.passed("IG_FB_CTL_FORMAT_SHIFT",
                           f"observed CTL separates the readings: {got} "
                           f"icl vs {wrong} skl")

    #
    # 3. The derived stride unit must reproduce the panel that was LIT.
    #
    # IntelFramebuffer.hpp:191-219 derives 128 bytes/unit from exactly one
    # measurement, and says so. This re-derives it from the other four pinned
    # numbers rather than trusting the comment: a tight 1920-pixel row at four
    # bytes per pixel is 7680 bytes, the observed field is 0x3C = 60, and
    # 7680 / 60 == 128. Nothing else in the tree checks that arithmetic, and
    # per prose-carrying-a-derived-value-goes-stale-silently, a number written
    # in a comment is checked by nothing.
    #
    stride_raw = resolve("IG_FB_STRIDE_OBSERVED_2026_08_16", kext)
    stride_mask = resolve("IG_FB_STRIDE_MASK", kext)
    unit = resolve("IG_FB_STRIDE_UNIT_DERIVED_2026_08_16", kext)
    width = resolve("IG_FB_PANEL_WIDTH_OBSERVED_2026_08_16", kext)

    if None in (stride_raw, stride_mask, unit, width):
        rep.failed("IG_FB_STRIDE_UNIT_DERIVED_2026_08_16",
                   "the stride derivation's inputs do not all resolve, so "
                   "the 128-byte unit rests on the comment alone")
    else:
        field = stride_raw & stride_mask
        if field == 0:
            rep.failed("IG_FB_STRIDE_OBSERVED_2026_08_16",
                       "the observed stride field masks to zero")
        elif field * unit != width * 4:
            rep.failed("IG_FB_STRIDE_UNIT_DERIVED_2026_08_16",
                       f"{field} units * {unit} = {field * unit} bytes, but a "
                       f"tight {width}-pixel row at 4 bpp is {width * 4}")
        else:
            rep.passed("IG_FB_STRIDE_UNIT_DERIVED_2026_08_16",
                       f"{field} * {unit} == {width} * 4 == {width * 4} bytes")

    #
    # 3b. IG_FB_GGTT_POLICY_LIMIT must be derivable from the SURF field itself.
    #
    # Added 2026-08-17 alongside igFbSurfaceRangeOk(). The ceiling is spelled
    # 0x100000000ull by hand, and the header's own comment says what it means:
    # NOT a measured GGTT size -- nothing in this tree measures that -- but the
    # largest address PLANE_SURF's page field can NAME. That is a claim about
    # IG_FB_SURF_ADDR_MASK, so it can be recomputed from it, and per
    # prose-carrying-a-derived-value-goes-stale-silently a derivation that lives
    # only in a comment is checked by nothing.
    #
    # The page size is derived from the mask rather than typed: the low bits the
    # mask clears ARE the page, so ~mask + 1 is 4096 without a second
    # hand-spelled number to go stale. Then limit == mask + page exactly.
    #
    # Both EDGES matter, per containment-is-one-sided. A limit too HIGH accepts
    # ranges the field cannot name; too LOW rejects the topmost page the field
    # CAN name. Equality is the only correct relation, so it is asserted as
    # equality rather than as a bound.
    #
    ggtt_limit = resolve("IG_FB_GGTT_POLICY_LIMIT", kext)
    surf_mask = resolve("IG_FB_SURF_ADDR_MASK", kext)

    if ggtt_limit is None or surf_mask is None:
        rep.failed("IG_FB_GGTT_POLICY_LIMIT",
                   "the ceiling or IG_FB_SURF_ADDR_MASK does not resolve, so "
                   "the 2^32 bound rests on the comment alone")
    else:
        page = (~surf_mask & 0xFFFFFFFF) + 1
        derived = surf_mask + page
        if page != 4096:
            rep.failed("IG_FB_SURF_ADDR_MASK",
                       f"clears {page} low bytes rather than a 4096-byte page, "
                       f"so PLANE_SURF is not being read as a page address")
        elif ggtt_limit != derived:
            rep.failed("IG_FB_GGTT_POLICY_LIMIT",
                       f"is 0x{ggtt_limit:X}, but the top page 0x{surf_mask:X} "
                       f"plus one {page}-byte page is 0x{derived:X} -- the "
                       f"ceiling no longer equals what the field can name")
        else:
            rep.passed("IG_FB_GGTT_POLICY_LIMIT",
                       f"0x{ggtt_limit:X} == top page 0x{surf_mask:X} + "
                       f"{page} bytes")

    #
    # 4. The observed SIZE must decode to the panel the connector advertised.
    #
    # Two independent readings of the same fact (handoff15 §1): the capture's
    # connector line says 1920x1080, and PLANE_SIZE says 0x0437077F. Both
    # halves store value - 1. Checking them against each other is the only
    # thing in the tree that can catch a transposed decoder, because
    # PLANE_SIZE's halves are INVERTED relative to PIPESRC and a transposed
    # 1080x1920 is a perfectly valid register write.
    #
    size = resolve("IG_FB_SIZE_OBSERVED_2026_08_16", kext)
    half = resolve("IG_FB_SIZE_HALF_MASK", kext)
    wshift = resolve("IG_FB_SIZE_WIDTH_SHIFT", kext)
    hshift = resolve("IG_FB_SIZE_HEIGHT_SHIFT", kext)
    height = resolve("IG_FB_PANEL_HEIGHT_OBSERVED_2026_08_16", kext)

    if None in (size, half, wshift, hshift, width, height):
        rep.failed("IG_FB_SIZE_OBSERVED_2026_08_16",
                   "the PLANE_SIZE decode's constants do not all resolve")
    else:
        got_w = ((size >> wshift) & half) + 1
        got_h = ((size >> hshift) & half) + 1
        if (got_w, got_h) == (height, width):
            rep.failed("IG_FB_SIZE_OBSERVED_2026_08_16",
                       f"decodes TRANSPOSED to {got_w}x{got_h}; PLANE_SIZE's "
                       f"halves are inverted relative to PIPESRC, and a "
                       f"transposed surface programs cleanly")
        elif (got_w, got_h) != (width, height):
            rep.failed("IG_FB_SIZE_OBSERVED_2026_08_16",
                       f"decodes to {got_w}x{got_h}, but the eDP connector "
                       f"advertised {width}x{height}")
        else:
            rep.passed("IG_FB_SIZE_OBSERVED_2026_08_16",
                       f"decodes to {got_w}x{got_h}, matching the panel")

    #
    # 5. The observed CTL really does have render decompression SET.
    #
    # This is the header's most counterintuitive claim and the one a reader is
    # most likely to "correct": the ONE live capture is an inheritance REFUSAL
    # case, not an acceptance case. If this ever silently became false, every
    # test built around the refusal would still pass while testing the wrong
    # branch.
    #
    decomp = resolve("IG_FB_CTL_RENDER_DECOMP", kext)
    if ctl is None or decomp is None:
        rep.failed("IG_FB_CTL_RENDER_DECOMP",
                   "does not resolve alongside the observed CTL")
    elif not (ctl & decomp):
        rep.failed("IG_FB_CTL_RENDER_DECOMP",
                   f"0x{decomp:X} is CLEAR in the observed CTL 0x{ctl:X}; the "
                   f"header states the one live capture is compressed, and "
                   f"the inheritance verdict depends on it")
    else:
        rep.passed("IG_FB_CTL_RENDER_DECOMP",
                   f"set in the observed CTL 0x{ctl:X} -- the live capture is "
                   f"an inheritance refusal case")

    #
    # 6. The SURF address mask and the flag bit must not overlap.
    #
    # PLANE_SURF_ADDR_MASK is REG_GENMASK(31, 12) and DECRYPT is REG_BIT(2).
    # If a transcription error widened the mask downward it would swallow the
    # flags, and igFbSurfaceOffset() would return an address with flag bits
    # baked in -- a 4 KiB-misaligned GGTT offset that still looks plausible.
    #
    addr = resolve("IG_FB_SURF_ADDR_MASK", kext)
    decrypt = resolve("IG_FB_SURF_DECRYPT", kext)
    if addr is None or decrypt is None:
        rep.failed("IG_FB_SURF_ADDR_MASK",
                   "the SURF constants do not both resolve")
    elif addr & decrypt:
        rep.failed("IG_FB_SURF_ADDR_MASK",
                   f"0x{addr:X} overlaps DECRYPT 0x{decrypt:X}, so the "
                   f"decoded surface address would carry flag bits")
    else:
        rep.passed("IG_FB_SURF_ADDR_MASK",
                   f"0x{addr:X} is disjoint from DECRYPT 0x{decrypt:X}")

    #
    # 7. The four named format codes must be DISTINCT.
    #
    # igFbBytesPerPixel() is a switch over these. Two codes colliding is a
    # duplicate-case compile error in C++ -- but only once a suite compiles the
    # header, and this gate runs on a bare checkout where nothing does.
    #
    fmt_names = ("IG_FB_FORMAT_XRGB_2101010", "IG_FB_FORMAT_XRGB_8888",
                 "IG_FB_FORMAT_XRGB_16161616F", "IG_FB_FORMAT_RGB_565")
    fmts = {n: resolve(n, kext) for n in fmt_names}
    if any(v is None for v in fmts.values()):
        missing = [n for n, v in fmts.items() if v is None]
        rep.failed("IG_FB_FORMAT_*",
                   f"{', '.join(missing)} do not resolve")
    elif len(set(fmts.values())) != len(fmt_names):
        rep.failed("IG_FB_FORMAT_*",
                   f"the four named format codes are not distinct: {fmts}")
    else:
        rep.passed("IG_FB_FORMAT_*",
                   f"{len(fmt_names)} named codes, all distinct")

    #
    # 8. Every named format code must FIT the five-bit field.
    #
    # A code wider than the mask can never be produced by igFbFormat(), so its
    # case arm in igFbBytesPerPixel() would be unreachable -- silently, with no
    # warning from any compiler, and the format would fall through to the 0
    # default and be refused as unreadable.
    #
    if mask is not None and all(v is not None for v in fmts.values()):
        toobig = {n: v for n, v in fmts.items() if v > mask}
        if toobig:
            rep.failed("IG_FB_FORMAT_*",
                       f"{toobig} exceed the field mask 0x{mask:X}, so those "
                       f"case arms are unreachable")
        else:
            rep.passed("IG_FB_FORMAT_*",
                       f"all four codes fit the 0x{mask:X} field")

    #
    # 9. The tiling codes must be distinct and fit their three-bit field.
    #
    # Same property one field over. YF_OR_4 is deliberately ONE name for the
    # one code 5 that upstream spells twice (TILED_YF and TILED_4 are both
    # REG_FIELD_PREP(..., 5)), so four distinct names for four distinct codes
    # is the correct expectation here, not five.
    #
    tile_names = ("IG_FB_TILING_LINEAR", "IG_FB_TILING_X",
                  "IG_FB_TILING_Y", "IG_FB_TILING_YF_OR_4")
    tiles = {n: resolve(n, kext) for n in tile_names}
    tmask = resolve("IG_FB_CTL_TILING_MASK", kext)
    if any(v is None for v in tiles.values()) or tmask is None:
        rep.failed("IG_FB_TILING_*", "the tiling constants do not all resolve")
    elif len(set(tiles.values())) != len(tile_names):
        rep.failed("IG_FB_TILING_*",
                   f"the tiling codes are not distinct: {tiles}")
    elif any(v > tmask for v in tiles.values()):
        rep.failed("IG_FB_TILING_*",
                   f"a tiling code exceeds the 0x{tmask:X} field: {tiles}")
    else:
        rep.passed("IG_FB_TILING_*",
                   f"{len(tile_names)} distinct codes, all inside 0x{tmask:X}")

    #
    # 10. The six inheritance verdicts must be distinct.
    #
    # igFbInheritable() returns these, and the whole reason they are an
    # enumeration rather than a bool is that a caller must be able to tell
    # "wait" from "refuse permanently" from "fall back to a mode set". Two
    # colliding values silently merge two of those responses.
    #
    verdict_names = ("IG_FB_INHERIT_OK", "IG_FB_INHERIT_DISABLED",
                     "IG_FB_INHERIT_COMPRESSED", "IG_FB_INHERIT_FORMAT",
                     "IG_FB_INHERIT_STRIDE", "IG_FB_INHERIT_GEOMETRY")
    verdicts = {n: resolve(n, kext) for n in verdict_names}
    if any(v is None for v in verdicts.values()):
        missing = [n for n, v in verdicts.items() if v is None]
        rep.failed("IG_FB_INHERIT_*", f"{', '.join(missing)} do not resolve")
    elif len(set(verdicts.values())) != len(verdict_names):
        rep.failed("IG_FB_INHERIT_*",
                   f"the verdict codes are not distinct: {verdicts}")
    elif verdicts["IG_FB_INHERIT_OK"] != 0:
        rep.failed("IG_FB_INHERIT_OK",
                   f"is {verdicts['IG_FB_INHERIT_OK']}, not 0; every other "
                   f"verdict is a refusal reason, so OK must be the zero")
    else:
        rep.passed("IG_FB_INHERIT_*",
                   f"{len(verdict_names)} distinct verdicts, OK == 0")


def intel_cross_header_checks(rep, intel, intel_fb):
    """
    The plane field offsets are spelled TWICE in this tree, and until now
    nothing compared the two copies.

    IntelDisplay.hpp owns ADDRESSING and names them IG_PLANE_*:

        IG_PLANE_CTL 0x00, IG_PLANE_STRIDE 0x08, IG_PLANE_POS 0x0C,
        IG_PLANE_SIZE 0x10, IG_PLANE_SURF 0x1C, IG_PLANE_KEYMAX 0x20,
        IG_PLANE_OFFSET 0x24

    IntelFramebuffer.hpp owns DECODING and names the same seven IG_FB_OFF_*.
    That duplication is deliberate -- the dependency-free rule forbids one
    header including the other, so the second copy is the only way both files
    can stand alone. But a deliberate duplicate is still a duplicate, and this
    is the exact shape of `self-cancelling-constants-are-untestable` one level
    up: each side is individually checked against the vendored i915 literals by
    its own row table, so BOTH can be individually green while disagreeing with
    each other.

    WHY A DISAGREEMENT HERE IS INVISIBLE EVERYWHERE ELSE

    igPlaneReg() computes WHERE to read from IntelDisplay's table. igFbFormat()
    and friends decode WHAT was read, assuming it came from the offset in
    IntelFramebuffer's table. If the two disagree by one row, the driver reads
    PLANE_KEYVAL and decodes it as PLANE_SIZE. Both registers exist, both are
    readable, neither faults -- and the decoded geometry is a plausible number.
    The C++ suites cannot see it either: test_intel_display.cpp drives the
    addressing header and test_intel_framebuffer.cpp drives the decode header,
    and neither includes the other, by construction.

    The overlap set is DERIVED by stripping the two prefixes rather than typed,
    so a new field added to either side joins the comparison by itself.
    """
    #
    # Both refusals up front, and both are failed() rather than skip(), because
    # a skip prints as a pass. A rename on either side is the realistic way this
    # goes quiet, and the failure mode is a comparison of nothing against
    # nothing reported as agreement.
    #
    if not intel:
        rep.failed("IG_PLANE_* vs IG_FB_OFF_*",
                   "IntelDisplay.hpp parsed no #defines -- the addressing side "
                   "of the cross-header comparison is empty")
        return
    if not intel_fb:
        rep.failed("IG_PLANE_* vs IG_FB_OFF_*",
                   "IntelFramebuffer.hpp parsed no #defines -- the decode side "
                   "of the cross-header comparison is empty")
        return

    #
    # BOTH SIDES GO THROUGH resolve(). parse_defines() returns the raw #define
    # BODY as a str, so comparing intel[n] to intel_fb[n] directly is a TEXTUAL
    # comparison wearing a numeric one's clothes. It passed today only because
    # the two headers happen to spell the same offsets the same way -- change
    # one side to 0x0 or 0 or 0x00 and this would report a disagreement that
    # does not exist, while a real disagreement spelled identically-but-wrong
    # is not what it would ever catch. resolve() also strips the u suffix and
    # follows one level of aliasing, neither of which string equality does.
    #
    addressing = {n[len("IG_PLANE_"):]: resolve(n, intel)
                  for n in intel
                  if n.startswith("IG_PLANE_")}
    decoding = {n[len("IG_FB_OFF_"):]: resolve(n, intel_fb)
                for n in intel_fb
                if n.startswith("IG_FB_OFF_")}

    #
    # IG_PLANE_CTL_BASE, IG_PLANE_PIPE_DELTA, IG_PLANE_DELTA and
    # IG_PLANE_FIELD_LIMIT are addressing constants, not field offsets. They
    # have no IG_FB_OFF_ counterpart by design, so they drop out of the
    # intersection rather than being excluded by a hand-kept list.
    #
    shared = sorted(set(addressing) & set(decoding))

    #
    # A floor on the overlap, because an intersection of zero compares nothing
    # and every loop below would be vacuous. Seven is what the two headers
    # share today (CTL, STRIDE, POS, SIZE, SURF, KEYMAX, OFFSET); the check is
    # >= rather than == so adding a field to BOTH sides does not red the gate.
    #
    if len(shared) < 7:
        rep.failed("IG_PLANE_* vs IG_FB_OFF_*",
                   f"only {len(shared)} field name(s) in common "
                   f"({shared}) -- expected at least 7, so a prefix or a "
                   f"field name changed and this comparison went vacuous")
        return

    disagree = {}
    for name in shared:
        a = addressing[name]
        d = decoding[name]
        if a is None or d is None:
            rep.failed(f"IG_PLANE_{name} vs IG_FB_OFF_{name}",
                       "one side did not resolve to a number")
            continue
        if a != d:
            disagree[name] = (a, d)

    if disagree:
        for name, (a, d) in sorted(disagree.items()):
            rep.failed(f"IG_PLANE_{name} vs IG_FB_OFF_{name}",
                       f"addressing says 0x{a:02X}, decode says 0x{d:02X} -- "
                       f"the driver would read one register and decode it as "
                       f"another; neither faults")
    else:
        rep.passed("IG_PLANE_* vs IG_FB_OFF_*",
                   f"{len(shared)} field offsets agree across both headers "
                   f"({', '.join(shared)})")

    #
    # Every shared offset must also sit inside the addressing header's own
    # declared field window. IG_PLANE_FIELD_LIMIT is what igPlaneReg()'s
    # bounds logic is written against, so a decode offset beyond it names a
    # register the addressing layer will not produce an address for -- an
    # unreachable decoder rather than a wrong one.
    #
    limit = addressing.get("FIELD_LIMIT")
    if limit is None:
        rep.failed("IG_PLANE_FIELD_LIMIT",
                   "absent, so no bound exists to check the decode offsets "
                   "against")
    else:
        beyond = {n: decoding[n] for n in shared if decoding[n] > limit}
        if beyond:
            rep.failed("IG_FB_OFF_* within IG_PLANE_FIELD_LIMIT",
                       f"{beyond} exceed 0x{limit:02X}")
        else:
            rep.passed("IG_FB_OFF_* within IG_PLANE_FIELD_LIMIT",
                       f"all {len(shared)} inside 0x{limit:02X}")


def intel_ddi_checks(rep, kext, src):
    """
    The six DDI constants no INTEL_CHECKS row can express.

    i915 spells the DDI fields as composed shifts rather than REG_GENMASK, so
    i915_genmask() returns None for every one of them and a "mask"/"shift" row
    would SKIP -- which prints as a pass. Three constants did fit the row kinds
    (the +0x400 delta, FUNC_ENABLE, and the TGL port shift); these are the rest,
    and each is DERIVED from the vendored literal rather than compared to a
    second hand-typed copy of it.

    The port field mask is the interesting one. IG_DDI_PORT_FIELD_MASK is 0x0F
    -- the mask AFTER the shift comes off -- while i915 stores the mask already
    positioned, (0xf << 27). So the derivation is a shift-down, and getting it
    backwards would produce 0x78000000, which is not 0x0F by any reading, so the
    comparison cannot accidentally agree.

    HAZARD 8 IS ALSO ASSERTED HERE, as a NON-equality. The pre-icl shift (28)
    and the Gen12 shift (27) both exist in the vendored header, and a row
    pinning IG_DDI_PORT_SHIFT to the wrong one would pass. So this refuses if
    the two i915 shifts are equal -- if a future vendored version unified them,
    the separation this header is built around would be gone and the check
    guarding it would be silently vacuous.
    """
    if not kext or not src:
        rep.failed("IG_DDI_*",
                   "IntelDisplay.hpp or reference/i915 parsed empty -- every "
                   "DDI derivation below would skip, and a skip is a pass")
        return

    tgl_shift = i915_resolve("TGL_TRANS_DDI_PORT_SHIFT", src)
    old_shift = i915_resolve("TRANS_DDI_PORT_SHIFT", src)
    tgl_mask = i915_resolve("TGL_TRANS_DDI_PORT_MASK", src)

    if tgl_shift is None or old_shift is None or tgl_mask is None:
        rep.failed("TGL_TRANS_DDI_PORT_*",
                   "the vendored port shift or mask does not resolve, so "
                   "nothing grounds the Gen12 port field")
        return

    #
    # The non-vacuity guard for the whole hazard: if upstream ever makes these
    # equal, pinning IG_DDI_PORT_SHIFT to either one stops being a real choice
    # and the row that pins it stops being evidence.
    #
    if tgl_shift == old_shift:
        rep.failed("TGL_TRANS_DDI_PORT_SHIFT",
                   f"equals TRANS_DDI_PORT_SHIFT ({tgl_shift}) in the vendored "
                   f"headers -- the pre-icl/Gen12 split this header is built "
                   f"around is gone, so the row pinning it proves nothing")
    else:
        rep.passed("TGL vs pre-icl port shift",
                   f"{tgl_shift} vs {old_shift}, still distinct")

    #
    # THE FIELD MASK IS DERIVED THROUGH THE KEXT'S OWN SHIFT, not through the
    # vendored one, and that is a CORRECTION rather than a stylistic choice.
    #
    # It first read `tgl_mask >> tgl_shift` -- both operands from the vendored
    # table. A mutation probe on 2026-08-17 set IG_DDI_PORT_SHIFT to the pre-icl
    # 28 and this function stayed green on all nine checks: the shift the DRIVER
    # will actually use appeared nowhere in the derivation. The INTEL_CHECKS
    # "value" row does pin it, so the gate as a whole was not blind -- but a
    # function whose subject is the port field must not be silent about the one
    # constant that positions it, or a future reordering of the row table takes
    # the only evidence away without reddening anything.
    #
    # Routing the shift-down through the kext's number makes the mask and the
    # shift ONE claim: a wrong shift now moves the derived mask and fails here as
    # well as in the row above.
    #
    kext_shift = resolve("IG_DDI_PORT_SHIFT", kext)
    if kext_shift is None:
        rep.failed("IG_DDI_PORT_SHIFT", "not found in IntelDisplay.hpp")
        return
    if kext_shift != tgl_shift:
        rep.failed("IG_DDI_PORT_SHIFT",
                   f"is {kext_shift}, but Gen12 puts the port field at "
                   f"{tgl_shift} (TGL_TRANS_DDI_PORT_SHIFT). {old_shift} is the "
                   f"PRE-ICL shift, and a word read with it yields a different "
                   f"VALID port number rather than garbage")
        return

    field_mask = resolve("IG_DDI_PORT_FIELD_MASK", kext)
    derived_field = tgl_mask >> kext_shift

    if field_mask is None:
        rep.failed("IG_DDI_PORT_FIELD_MASK", "not found in IntelDisplay.hpp")
    elif field_mask != derived_field:
        rep.failed("IG_DDI_PORT_FIELD_MASK",
                   f"is 0x{field_mask:X}, but TGL_TRANS_DDI_PORT_MASK "
                   f"0x{tgl_mask:X} shifted down by {tgl_shift} is "
                   f"0x{derived_field:X}")
    else:
        rep.passed("IG_DDI_PORT_FIELD_MASK",
                   f"0x{field_mask:X} == 0x{tgl_mask:X} >> {tgl_shift}")

    #
    # MODE SELECT. (7 << 24), so the low set bit is the shift and the value
    # shifted down is the mask. Both are derived from the ONE vendored literal,
    # which is what keeps the pair consistent -- a shift and a mask taken from
    # different sources can each be right and still not describe one field.
    #
    mode_mask_raw = i915_resolve("TRANS_DDI_MODE_SELECT_MASK", src)
    if mode_mask_raw is None or mode_mask_raw == 0:
        rep.failed("TRANS_DDI_MODE_SELECT_MASK",
                   "absent or zero in the vendored headers")
        return

    derived_mode_shift = (mode_mask_raw & -mode_mask_raw).bit_length() - 1
    derived_mode_mask = mode_mask_raw >> derived_mode_shift

    for name, want, what in (
            ("IG_DDI_MODE_SHIFT", derived_mode_shift, "low set bit"),
            ("IG_DDI_MODE_MASK", derived_mode_mask, "normalised to bit 0")):
        got = resolve(name, kext)
        if got is None:
            rep.failed(name, "not found in IntelDisplay.hpp")
        elif got != want:
            rep.failed(name,
                       f"is 0x{got:X}, but TRANS_DDI_MODE_SELECT_MASK "
                       f"0x{mode_mask_raw:X} gives {want} as its {what}")
        else:
            rep.passed(name, f"{want}, the {what} of 0x{mode_mask_raw:X}")

    #
    # The five mode CODES. i915 stores them already positioned, so each is
    # shifted down by the derived shift rather than by a typed 24 -- if the
    # shift above were wrong every one of these would move with it, which is
    # the point: the codes and the shift are one claim, not six.
    #
    for kname, iname in (
            ("IG_DDI_MODE_HDMI", "TRANS_DDI_MODE_SELECT_HDMI"),
            ("IG_DDI_MODE_DVI", "TRANS_DDI_MODE_SELECT_DVI"),
            ("IG_DDI_MODE_DP_SST", "TRANS_DDI_MODE_SELECT_DP_SST"),
            ("IG_DDI_MODE_DP_MST", "TRANS_DDI_MODE_SELECT_DP_MST"),
            ("IG_DDI_MODE_FDI_OR_128B132B",
             "TRANS_DDI_MODE_SELECT_FDI_OR_128B132B")):
        positioned = i915_resolve(iname, src)
        got = resolve(kname, kext)

        if positioned is None:
            rep.failed(iname, "absent from the vendored headers")
            continue
        if got is None:
            rep.failed(kname, "not found in IntelDisplay.hpp")
            continue

        want = positioned >> derived_mode_shift
        if got != want:
            rep.failed(kname,
                       f"is {got}, but {iname} 0x{positioned:X} shifted down "
                       f"by {derived_mode_shift} is {want}")
        else:
            rep.passed(kname, f"{got} == 0x{positioned:X} >> "
                              f"{derived_mode_shift}")


def intel_verdict_set_checks(rep, intel, prefix, expected, owner,
                             exclude=(), header="IntelDisplay.hpp"):
    """
    A verdict ENUM, checked as a SET derived from the header.

    Called once per family: IG_MODE_* (igModeConsistent, 7 verdicts) and
    IG_JOIN_* (igPlaneMatchesPipe, 4). Parameterised rather than copied,
    because the second copy of a checker is where the two drift -- and a
    drifted checker is green on the family it still describes.

    ``prefix`` includes its trailing underscore. ``expected`` is the number of
    verdicts the function returns, EXCLUDING the VERDICT_LIMIT bound. ``owner``
    names the C++ function, for the failure text only.

    tests/test_intel_display.cpp already pins each verdict to a bare literal,
    and that is the stronger check for the codes that exist today. It cannot be
    the whole story, because its list is HAND-KEPT: add an eighth verdict to
    IntelDisplay.hpp, forget the corresponding checkU32 line, and every gate in
    this tree stays green while the new code is pinned by nothing. This function
    reads the set out of the file, so an unpinned addition is impossible by
    construction rather than by remembering.

    Three properties, and each has a distinct failure:

      - DISTINCT. Two verdicts sharing a number makes two different refusal
        reasons indistinguishable to the caller. igModeConsistent() returns the
        verdict verbatim, so a collision does not fault or even misbehave -- it
        reports the wrong reason for a real refusal, which is the kind of defect
        that survives until someone debugs the wrong half of a raster.

      - OK IS ZERO. Every call site is `if (verdict != IG_MODE_OK)`. A non-zero
        OK does not break that idiom, but it does break the one C convention a
        reader will assume without checking, and igAxisConsistent()'s early
        returns would then be returning a falsy value for a FAILURE.

      - VERDICT_LIMIT IS THE MAXIMUM. It is a DERIVED number spelled by hand.
        Nothing recomputes it, so adding a verdict above it leaves a bound that
        silently excludes the newest case -- the same one-sided-containment
        shape that let IG_DISPLAY_POLICY_LIMIT be widened with no failure.
    """
    verdicts = {}
    seen_excluded = set()
    for name in intel:
        if not name.startswith(prefix):
            continue
        #
        # An excluded name is recorded and SKIPPED BEFORE resolve(), not
        # filtered out after it. IG_SCAN_INVALID64 is 0xFFFFFFFFFFFFFFFFull,
        # and resolving a constant this arm is not comparing would let a
        # suffix this parser cannot read fail a whole verdict family for a
        # reason that has nothing to do with any verdict in it.
        #
        if name in exclude:
            seen_excluded.add(name)
            continue
        value = resolve(name, intel)
        if value is None:
            rep.failed(name, "does not resolve to an integer")
            return
        verdicts[name] = value

    #
    # A stale exclusion is an excuse for nothing. A name the caller lists and
    # the header does not carry means the sweep quietly stopped excluding
    # something, and an exclusion list that silently shrinks is how a real
    # verdict later slips OUT of the compared set -- the direction that reads
    # green while covering less. handoff16 1's IG_PIPE_TRANSCONF trap is why
    # the list exists at all: IG_SCAN_ also covers five sentinels and masks, so
    # a bare prefix sweep computes max(codes) as 0xFFFFFFFFFFFFFFFF and fails
    # the limit == largest equality while being wrong about the world.
    #
    for name in sorted(set(exclude) - seen_excluded):
        rep.failed(name,
                   f"is named as a non-verdict exclusion for {prefix}* but "
                   f"does not exist in {header} -- a stale exclusion silently "
                   f"shrinks the set this arm compares")
        return

    limit_name = prefix + "VERDICT_LIMIT"
    codes = {n: v for n, v in verdicts.items() if n != limit_name}

    #
    # A refusal, never a skip. A skip prints as a pass, and "the verdicts are
    # gone" must not read the same as "the verdicts are fine".
    #
    if len(codes) < expected:
        rep.failed(f"{prefix}* verdicts",
                   f"only {len(codes)} found in {header} -- "
                   f"{owner} returns {expected} distinct verdicts, so this "
                   f"comparison is examining a header that is not the one the "
                   f"driver compiles")
        return

    seen = {}
    for name, value in sorted(codes.items()):
        if value in seen:
            rep.failed(name,
                       f"0x{value:X} collides with {seen[value]} -- two "
                       f"refusal reasons the caller cannot tell apart")
            return
        seen[value] = name

    ok_name = prefix + "OK"
    if codes.get(ok_name) != 0:
        rep.failed(ok_name,
                   f"is {codes.get(ok_name)}, not 0 -- every call site spells "
                   f"success as a zero")
        return

    if limit_name not in verdicts:
        rep.failed(limit_name, "absent -- nothing bounds the verdict range")
        return

    largest = max(codes.values())
    if verdicts[limit_name] != largest:
        rep.failed(limit_name,
                   f"is {verdicts[limit_name]}, but the largest verdict is "
                   f"{largest} ({seen[largest]}) -- a hand-spelled derived "
                   f"number that has fallen behind the set it describes")
        return

    expected = set(range(len(codes)))
    if set(codes.values()) != expected:
        rep.failed(f"{prefix}* numbering",
                   f"{sorted(codes.values())} is not 0..{len(codes) - 1} -- "
                   f"a gap means the verdicts cannot be used as a table index")
        return

    rep.passed(f"{prefix}* verdicts",
               f"{len(codes)} distinct, 0..{largest}, {ok_name} is 0, "
               f"{limit_name} == {largest}")


def intel_sanity_checks(rep, kext, src, text):
    if text is None:
        rep.failed("IntelDisplay.hpp",
                   "not found -- the twelve switch bases are unverified")
        return
    if not src:
        return

    #
    # The twelve addresses parse_defines() cannot see.
    #
    for func, expect in (("igTransBase", IG_TRANS_BASE_EXPECT),
                         ("igPipeBase", IG_PIPE_BASE_EXPECT)):
        cases = ig_switch_cases(text, func)

        if cases is None:
            rep.failed(func,
                       "function body not found in IntelDisplay.hpp -- its "
                       "six bases are unverified")
            continue

        if len(cases) != len(expect):
            rep.failed(f"{func} arm count",
                       f"{len(cases)} case arms decoded, expected "
                       f"{len(expect)} -- an arm was added, removed, or is "
                       f"spelled in a form IG_CASE_RE cannot read")

        for ordinal, iname in expect:
            label = f"{func}(IG_TRANSCODER_{ordinal})"

            if ordinal not in cases:
                rep.failed(label, "no case arm found")
                continue

            want = i915_resolve(iname, src)
            if want is None:
                rep.skip(label, f"{iname} not found in vendored i915 headers")
                continue

            got = cases[ordinal]
            if got != want:
                rep.failed(label, f"0x{got:X} != {iname} 0x{want:X}")
            else:
                rep.passed(label, f"0x{got:X} == {iname}")

    #
    # Hazard 1, stated as an assertion rather than a comment: the transcoder
    # ladder is NOT uniform, so no stride formula can replace the switch. If
    # these two ever become equal, someone has changed the vendored evidence
    # and the switch could be "simplified" into a silently-wrong arithmetic
    # helper -- DSI1 would land at 0x6C000, a real address in another block
    # that reads and writes without faulting.
    #
    tc = i915_resolve("TRANSCODER_C_OFFSET", src)
    td = i915_resolve("TRANSCODER_D_OFFSET", src)
    t0 = i915_resolve("TRANSCODER_DSI0_OFFSET", src)
    t1 = i915_resolve("TRANSCODER_DSI1_OFFSET", src)

    if None in (tc, td, t0, t1):
        rep.skip("transcoder ladder is not uniform",
                 "a TRANSCODER_*_OFFSET is missing from the vendored headers")
    else:
        ab, dsi = td - tc, t1 - t0
        if ab == dsi:
            rep.failed("transcoder ladder is not uniform",
                       f"D-C and DSI1-DSI0 are both 0x{ab:X} -- the stated "
                       f"reason igTransBase() is a switch no longer holds")
        else:
            rep.passed("transcoder ladder is not uniform",
                       f"D-C 0x{ab:X} vs DSI1-DSI0 0x{dsi:X}")

    #
    # Plane addressing is the exact inverse: it genuinely IS arithmetic
    # (_PICK_EVEN). Both strides are therefore checked as differences between
    # two vendored constants, which is the form the INTEL_CHECKS kinds cannot
    # express.
    #
    for kname, hi, lo in (
        ("IG_PLANE_PIPE_DELTA", "PIPE_B_OFFSET", "PIPE_A_OFFSET"),
        ("IG_PLANE_DELTA", "_PLANE_CTL_2_A", "_PLANE_CTL_1_A"),
    ):
        kv = resolve(kname, kext)
        a, b = i915_resolve(hi, src), i915_resolve(lo, src)

        if kv is None:
            rep.failed(kname, "not found or unparseable in IntelDisplay.hpp")
        elif a is None or b is None:
            rep.skip(kname, f"{hi if a is None else lo} not found upstream")
        elif kv != a - b:
            rep.failed(kname,
                       f"0x{kv:X} != {hi} - {lo} (0x{a - b:X})")
        else:
            rep.passed(kname, f"0x{kv:X} == {hi} - {lo}")

    #
    # IG_DISPLAY_POLICY_BASE/LIMIT is a POLICY bound, not a hardware width, and
    # the header says so. What can still be checked is that it actually
    # brackets every base the code will hand to it -- the window is documented
    # as deliberately NOT dense, so containment is the only true property.
    #
    winlo = resolve("IG_DISPLAY_POLICY_BASE", kext)
    winhi = resolve("IG_DISPLAY_POLICY_LIMIT", kext)

    bases = {}
    for func in ("igTransBase", "igPipeBase"):
        for ordinal, value in (ig_switch_cases(text, func) or {}).items():
            bases[f"{func}({ordinal})"] = value

    if winlo is None or winhi is None:
        rep.failed("IG_DISPLAY_POLICY window",
                   "BASE or LIMIT not found in IntelDisplay.hpp")
    elif winhi <= winlo:
        rep.failed("IG_DISPLAY_POLICY window",
                   f"LIMIT 0x{winhi:X} is not above BASE 0x{winlo:X}")
    elif not bases:
        rep.failed("IG_DISPLAY_POLICY window",
                   "no switch bases decoded -- containment would be vacuous")
    else:
        outside = [f"{k}=0x{v:X}" for k, v in bases.items()
                   if not winlo <= v <= winhi]
        if outside:
            rep.failed("IG_DISPLAY_POLICY window",
                       f"outside 0x{winlo:X}..0x{winhi:X}: {', '.join(outside)}")
        else:
            rep.passed("IG_DISPLAY_POLICY window",
                       f"0x{winlo:X}..0x{winhi:X} brackets all twelve "
                       f"block bases")

    #
    # Containment ALONE is a one-sided property, and the weak side is silent:
    # widening LIMIT by any amount leaves every base still inside, so the upper
    # edge could drift arbitrarily far into unrelated MMIO and this gate would
    # stay green. Measured 2026-08-16 by in-memory mutation -- LIMIT 0x7BFFF ->
    # 0x7C000 produced NO failure at all before these two rows existed.
    #
    # So both edges are also pinned to the vendored evidence:
    #
    #   BASE  == the lowest of the twelve block bases (transcoder A, 0x60000)
    #   LIMIT == the highest of the twelve (pipe DSI1, 0x7B800) plus that
    #            block's extent, minus one
    #
    # The extent is derived from DSI1 - DSI0 rather than written as 0x800,
    # because 0x800 as a literal here would be a second, independently-typed
    # copy of the exact number hazard 1 is about.
    #
    # This is NOT a claim that the window is dense -- the header is explicit
    # that it is not, and the gaps between blocks stay gaps. It is a claim
    # about where the two EDGES sit, which is checkable and was not checked.
    #
    if bases and winlo is not None and winhi is not None:
        want_lo = min(bases.values())
        top = max(bases.values())

        if winlo != want_lo:
            rep.failed("IG_DISPLAY_POLICY_BASE",
                       f"0x{winlo:X} != the lowest block base 0x{want_lo:X} "
                       f"-- the window's lower edge has drifted off it")
        else:
            rep.passed("IG_DISPLAY_POLICY_BASE",
                       f"0x{winlo:X} == the lowest block base")

        d0 = i915_resolve("PIPE_DSI0_OFFSET", src)
        d1 = i915_resolve("PIPE_DSI1_OFFSET", src)

        if d0 is None or d1 is None:
            rep.skip("IG_DISPLAY_POLICY_LIMIT",
                     "PIPE_DSI0/1_OFFSET not found in the vendored i915 "
                     "headers -- the block extent cannot be derived")
        elif d1 <= d0:
            rep.failed("IG_DISPLAY_POLICY_LIMIT",
                       f"PIPE_DSI1_OFFSET 0x{d1:X} is not above "
                       f"PIPE_DSI0_OFFSET 0x{d0:X}")
        else:
            extent = d1 - d0
            want_hi = top + extent - 1
            if winhi != want_hi:
                rep.failed("IG_DISPLAY_POLICY_LIMIT",
                           f"0x{winhi:X} != 0x{want_hi:X} (highest base "
                           f"0x{top:X} + extent 0x{extent:X} - 1) -- the "
                           f"window's upper edge is not tight")
            else:
                rep.passed("IG_DISPLAY_POLICY_LIMIT",
                           f"0x{winhi:X} == highest base 0x{top:X} + "
                           f"extent 0x{extent:X} - 1")


#
# ---- IntelGpuId.hpp -----------------------------------------------------
#
# The vendored PCI IDs are NOT #defines. i915_pciids.h spells them as bare
# literals inside variadic macro bodies:
#
#     #define INTEL_TGL_GT1_IDS(MACRO__, ...) \
#         MACRO__(0x9A60, ## __VA_ARGS__), \
#         MACRO__(0x9A68, ## __VA_ARGS__), \
#         MACRO__(0x9A70, ## __VA_ARGS__)
#
# parse_defines() and i915_resolve() both key on a NAME, and there is no name
# here to key on -- the value is a positional argument to a macro that is never
# expanded in this tree. So this extractor walks the macro BODY by line
# continuation and pulls the ordered list of literals out of it.
#
# Ordered, not a set: IntelGpuId.hpp's accessors are index-addressed
# (igTglGt1Id(1u) must be the SECOND entry), so a check that only compared
# membership would pass on a table whose entries were permuted -- and a
# permuted table still classifies every device correctly while returning the
# wrong slot to anything that iterates.
#
_ID_MACRO_RE_TMPL = r"#define\s+%s\s*\([^)]*\)((?:.*\\\r?\n)*.*)"
_ID_LITERAL_RE = re.compile(r"MACRO__\s*\(\s*(0[xX][0-9A-Fa-f]+)")


def i915_id_list(name, text):
    """The ordered PCI IDs inside one INTEL_*_IDS macro body, or None.

    Returns None -- never an empty list -- when the macro is absent or its body
    yields no literals. An empty list would read as "a real table that happens
    to be empty" and every zip() against it would silently check nothing, which
    is the precise failure mode this gate exists to prevent.
    """
    if not text:
        return None

    match = re.search(_ID_MACRO_RE_TMPL % re.escape(name), text)
    if match is None:
        return None

    ids = [int(v, 16) for v in _ID_LITERAL_RE.findall(match.group(1))]
    return ids or None


#
# Which kext #define must equal which slot of which vendored macro. Spelled as
# (macro, index, kext name) triples rather than derived from a naming
# convention, because a convention-driven loop cannot tell a MISSING entry from
# a table that is simply shorter -- and the count is checked separately below
# against the same source.
#
IG_ID_EXPECT = [
    ("INTEL_TGL_GT1_IDS", 0, "IG_TGL_GT1_ID_0"),
    ("INTEL_TGL_GT1_IDS", 1, "IG_TGL_GT1_ID_1"),
    ("INTEL_TGL_GT1_IDS", 2, "IG_TGL_GT1_ID_2"),
    ("INTEL_TGL_GT2_IDS", 0, "IG_TGL_GT2_ID_0"),
    ("INTEL_TGL_GT2_IDS", 1, "IG_TGL_GT2_ID_1"),
    ("INTEL_TGL_GT2_IDS", 2, "IG_TGL_GT2_ID_2"),
    ("INTEL_TGL_GT2_IDS", 3, "IG_TGL_GT2_ID_3"),
    ("INTEL_TGL_GT2_IDS", 4, "IG_TGL_GT2_ID_4"),
    ("INTEL_TGL_GT2_IDS", 5, "IG_TGL_GT2_ID_5"),
    ("INTEL_TGL_GT2_IDS", 6, "IG_TGL_GT2_ID_6"),
    ("INTEL_TGL_GT2_IDS", 7, "IG_TGL_GT2_ID_7"),
]


def run_intel_id_checks(rep, kext, pciids_text):
    #
    # Same refusal as run_intel_checks(). A missing header or a missing
    # vendored file would turn every row below into a skip, and a gate that
    # skips everything reports the same green as a gate that checked
    # everything.
    #
    if not kext:
        rep.failed("IntelGpuId.hpp",
                   "no #defines parsed -- every ID check would skip, "
                   "and a skip is a silent pass")
        return
    if not pciids_text:
        rep.failed("reference/i915/i915_pciids.h",
                   "not readable -- the PCI IDs have no evidence to be "
                   "checked against")
        return

    tables = {}
    for macro in ("INTEL_TGL_GT1_IDS", "INTEL_TGL_GT2_IDS"):
        ids = i915_id_list(macro, pciids_text)
        if ids is None:
            rep.failed(macro,
                       "not found in reference/i915/i915_pciids.h, or its "
                       "body yielded no literals -- the extractor cannot "
                       "see the table it is meant to check against")
        tables[macro] = ids

    #
    # 1. Every transcribed ID against the vendored literal at the SAME INDEX.
    #
    for macro, index, kname in IG_ID_EXPECT:
        ids = tables.get(macro)
        if ids is None:
            rep.skip(kname, f"{macro} unavailable")
            continue
        if index >= len(ids):
            rep.failed(kname,
                       f"{macro} has only {len(ids)} entries, so slot "
                       f"{index} does not exist in the vendored table")
            continue

        #
        # resolve(), not kext.get(). parse_defines() stores the raw BODY text,
        # so IG_TGL_GT1_ID_0 comes back as the string "0x9A60u" -- which
        # compares unequal to every int and formats as a TypeError. resolve()
        # strips the integer suffix and evaluates.
        #
        got = resolve(kname, kext)
        if got is None:
            rep.failed(kname, "not defined in IntelGpuId.hpp, or its body "
                              "did not resolve to an integer")
            continue

        want = ids[index]
        if got != want:
            rep.failed(kname,
                       f"0x{got:X} != {macro}[{index}] 0x{want:X}")
        else:
            rep.passed(kname, f"0x{got:X} == {macro}[{index}]")

    #
    # 2. The COUNTS, against the length of the vendored tables. Row-by-row
    #    checks above cannot see a table that grew upstream: every listed slot
    #    would still match while the driver silently ignored the new part.
    #    This is the check that goes red when i915 adds an ID.
    #
    for macro, kname in (("INTEL_TGL_GT1_IDS", "IG_TGL_GT1_ID_COUNT"),
                         ("INTEL_TGL_GT2_IDS", "IG_TGL_GT2_ID_COUNT")):
        ids = tables.get(macro)
        got = resolve(kname, kext)
        if ids is None or got is None:
            rep.skip(kname, "table or constant unavailable")
            continue
        if got != len(ids):
            rep.failed(kname,
                       f"{got} != {len(ids)}, the number of IDs in {macro} "
                       f"-- an upstream entry is unreachable from this driver")
        else:
            rep.passed(kname, f"{got} == len({macro})")

    #
    # 3. The union count. IG_TGL_ID_COUNT is spelled as a literal 11 in the
    #    header rather than as the sum, so this is a real check: if a table
    #    grows and its own count follows but the union count does not,
    #    igTglId() stops returning the tail and nothing else notices.
    #
    gt1 = tables.get("INTEL_TGL_GT1_IDS")
    gt2 = tables.get("INTEL_TGL_GT2_IDS")
    union = resolve("IG_TGL_ID_COUNT", kext)
    if gt1 is None or gt2 is None or union is None:
        rep.skip("IG_TGL_ID_COUNT", "tables or constant unavailable")
    elif union != len(gt1) + len(gt2):
        rep.failed("IG_TGL_ID_COUNT",
                   f"{union} != {len(gt1)} + {len(gt2)} = "
                   f"{len(gt1) + len(gt2)}")
    else:
        rep.passed("IG_TGL_ID_COUNT",
                   f"{union} == len(GT1) + len(GT2)")

    #
    # 4. The vendor ID, against INTEL_VGA_DEVICE's own first field. It is a
    #    bare 0x8086 in the macro body, so it is unreachable by name the same
    #    way the device IDs are.
    #
    vendor = resolve("IG_PCI_VENDOR_INTEL", kext)
    vm = re.search(r"#define\s+INTEL_VGA_DEVICE\s*\([^)]*\)\s*\{\s*\\\r?\n"
                   r"\s*(0[xX][0-9A-Fa-f]+)", pciids_text)
    if vendor is None or vm is None:
        rep.skip("IG_PCI_VENDOR_INTEL",
                 "INTEL_VGA_DEVICE's vendor field not located")
    else:
        want = int(vm.group(1), 16)
        if vendor != want:
            rep.failed("IG_PCI_VENDOR_INTEL",
                       f"0x{vendor:X} != INTEL_VGA_DEVICE's 0x{want:X}")
        else:
            rep.passed("IG_PCI_VENDOR_INTEL",
                       f"0x{vendor:X} == INTEL_VGA_DEVICE's vendor field")

    #
    # 5. The one OBSERVED value, against the vendored table -- and pinned as a
    #    MEMBERSHIP claim rather than as equality with a particular slot. The
    #    capture read 0x9A68 off the target laptop; the table is Linux's. They
    #    are two independent readings, and this is the row that goes red if
    #    either one is wrong. Kept separate from IG_TGL_GT1_ID_1's own row on
    #    purpose: checking only their equality would let two matching errors
    #    cancel.
    #
    observed = resolve("IG_DEVICE_OBSERVED_2026_08_16", kext)
    if observed is None or gt1 is None or gt2 is None:
        rep.skip("IG_DEVICE_OBSERVED_2026_08_16", "table or constant unavailable")
    elif observed not in (gt1 + gt2):
        rep.failed("IG_DEVICE_OBSERVED_2026_08_16",
                   f"0x{observed:X} is not in the vendored Tiger Lake tables "
                   f"-- either the transcription is wrong or the captured "
                   f"machine is not what the capture says it is")
    else:
        rep.passed("IG_DEVICE_OBSERVED_2026_08_16",
                   f"0x{observed:X} is in INTEL_TGL_*_IDS")

    #
    # 6. The tables as SETS. A duplicated entry is invisible to every row
    #    above: each slot still matches its own expectation while one real
    #    device is silently missing from the driver's world.
    #
    if gt1 is not None and gt2 is not None:
        both = gt1 + gt2
        dupes = sorted({v for v in both if both.count(v) > 1})
        if dupes:
            rep.failed("INTEL_TGL_IDS distinctness",
                       "the vendored tables contain duplicate IDs: "
                       + ", ".join(f"0x{v:X}" for v in dupes))
        else:
            rep.passed("INTEL_TGL_IDS distinctness",
                       f"{len(both)} IDs, all distinct")


#
#  The GT citation checks, added 2026-08-26 with the three gt/ headers.
#
#  These exist because merging a header into I915_HEADERS buys NOTHING on its
#  own. The four ring offsets in IntelGtCmd.hpp and the first-pair offset in
#  IntelGtProbe.hpp carry _I915_ in their names and a line number in a trailing
#  comment, and until this function ran, nothing in the tree compared either
#  half against upstream. A name that CLAIMS a citation over a value nobody
#  checked is worse than an uncited value, because it reads as evidence.
#
#  Both vendored subjects are parsed as RAW TEXT rather than through
#  merge_i915_sources(), and that is the load-bearing choice here.
#  RING_TAIL(base) is a function-like macro whose body is _MMIO((base) + 0x30);
#  resolve() cannot evaluate it, an unresolvable symbol makes its check SKIP,
#  and a skip is a silent pass. Same reasoning that keeps i915_pciids.h out of
#  the merge list.
#
#  Every anchor below FAILS when it is not found. It never skips. A gate that
#  reports nothing when its anchor moves is indistinguishable from a green one,
#  which is the vacuity direction this project has paid for repeatedly.
#

#  RING_TAIL(base)  _MMIO((base) + 0x30)
_ENGINE_REG_RE = (r"#define\s+{}\(base\)\s+_MMIO\(\(base\)\s*\+\s*"
                  r"(0[xX][0-9a-fA-F]+)\)")

#  CTX_RING_HEAD  (0x04 + 1)
_LRC_SLOT_RE = r"#define\s+{}\s+\((0[xX][0-9a-fA-F]+)\s*\+\s*1\)"


def _gt_find_hex(rep, label, pattern, name, text, filename):
    """Extract one hex value from vendored text, or FAIL. Never silent."""
    if text is None:
        rep.failed(label, f"{filename} not found -- this check measured "
                          f"NOTHING, which is not the same as passing")
        return None

    m = re.search(pattern.format(name), text)
    if not m:
        rep.failed(label, f"{name} not found in {filename} -- upstream "
                          f"renamed it, or the spelling this pattern assumes "
                          f"changed; the citation is now unverifiable")
        return None

    return int(m.group(1), 16)


def intel_gt_forcewake_checks(rep):
    """
    IntelGtForcewake.hpp against reference/i915/intel_gt_regs.h and
    intel_uncore.c, added 2026-10-12.

    Three kinds of claim, all FAIL (never skip) on a missing file:
      1. each cited register / bit value equals upstream's,
      2. each `// file:LINE NAME` citation comment points at a line that
         actually contains NAME (so a re-vendor that shifts lines reddens),
      3. the 43-row range table equals __gen12_fw_ranges[] row for row,
         including the row COUNT -- a dropped row would otherwise silently
         widen its neighbour.
    """
    def _read(p):
        return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else None

    hdr = _read(GT_FW_HEADER)
    regs = _read(I915 / "intel_gt_regs.h")
    unc = _read(I915 / "intel_uncore.c")
    for text, name in ((hdr, GT_FW_HEADER.name), (regs, "intel_gt_regs.h"),
                       (unc, "intel_uncore.c")):
        if text is None:
            rep.failed("forcewake", f"{name} not found -- these checks measured "
                                    f"NOTHING, which is not the same as passing")
            return

    def hdr_hex(name):
        m = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+)u" % name, hdr)
        return int(m.group(1), 16) if m else None

    def hdr_bit(name):
        m = re.search(r"#define\s+%s\s+\(1u\s*<<\s*(\d+)\)" % name, hdr)
        return int(m.group(1)) if m else None

    # 1. values
    for kext, up in (("IG_GTW_REQ_RENDER_I915_0xA278", "FORCEWAKE_RENDER_GEN9"),
                     ("IG_GTW_ACK_RENDER_I915_0xD84", "FORCEWAKE_ACK_RENDER_GEN9"),
                     ("IG_GTW_REQ_GT_I915_0xA188", "FORCEWAKE_GT_GEN9"),
                     ("IG_GTW_ACK_GT_I915_0x130044", "FORCEWAKE_ACK_GT_GEN9")):
        label = f"{kext} vs {up}"
        m = re.search(r"#define\s+%s\s+_MMIO\((0x[0-9a-fA-F]+)\)" % up, regs)
        got = hdr_hex(kext)
        if not m:
            rep.failed(label, f"{up} not found in intel_gt_regs.h")
        elif got is None:
            rep.failed(label, f"{kext} not defined in {GT_FW_HEADER.name}")
        elif got != int(m.group(1), 16):
            rep.failed(label, f"kext 0x{got:X}, upstream {m.group(1)}")
        else:
            rep.passed(label, f"0x{got:X}")

    for kext, up in (("IG_GTW_KERNEL_I915_BIT0", "FORCEWAKE_KERNEL"),
                     ("IG_GTW_KERNEL_FALLBACK_I915_BIT15", "FORCEWAKE_KERNEL_FALLBACK")):
        label = f"{kext} vs {up}"
        m = re.search(r"#define\s+%s\s+BIT\((\d+)\)" % up, regs)
        got = hdr_bit(kext)
        if not m or got is None or got != int(m.group(1)):
            rep.failed(label, f"kext bit {got}, upstream "
                              f"{m.group(1) if m else 'not found'}")
        else:
            rep.passed(label, f"BIT({got})")

    label = "IG_GTW_RESET_BITS_GEN12_I915_0xEFFF vs fw_domain_reset()"
    m = re.search(r">=\s*12\)\s*\n\s*fw_clear\(d,\s*(0x[0-9a-fA-F]+)\)", unc)
    got = hdr_hex("IG_GTW_RESET_BITS_GEN12_I915_0xEFFF")
    if not m or got is None or got != int(m.group(1), 16):
        rep.failed(label, f"kext {got}, upstream {m.group(1) if m else 'not found'}")
    else:
        rep.passed(label, f"0x{got:X}")

    label = "IG_GTW_ACK_TIMEOUT_MS_I915_50 vs FORCEWAKE_ACK_TIMEOUT_MS"
    m = re.search(r"#define\s+FORCEWAKE_ACK_TIMEOUT_MS\s+(\d+)", unc)
    m2 = re.search(r"#define\s+IG_GTW_ACK_TIMEOUT_MS_I915_50\s+(\d+)u", hdr)
    if not m or not m2 or m.group(1) != m2.group(1):
        rep.failed(label, "timeout differs or not found")
    else:
        rep.passed(label, f"{m.group(1)} ms")

    # 2. line citations
    files = {"intel_gt_regs.h": regs.splitlines(), "intel_uncore.c": unc.splitlines()}
    for fname, line, sym in re.findall(
            r"//\s*(intel_gt_regs\.h|intel_uncore\.c):(\d+)\s+([A-Z_][A-Z0-9_]+)", hdr):
        lines = files[fname]
        n = int(line)
        label = f"citation {fname}:{line} {sym}"
        if n < 1 or n > len(lines) or sym not in lines[n - 1]:
            rep.failed(label, "cited line does not contain the symbol")
        else:
            rep.passed(label, "line matches")

    # 3. the range table
    label = "kIgGtwGen12Ranges vs __gen12_fw_ranges[]"
    body = re.search(r"__gen12_fw_ranges\[\]\s*=\s*\{(.*?)\n\};", unc, re.S)
    if not body:
        rep.failed(label, "__gen12_fw_ranges[] not found in intel_uncore.c")
        return
    def _dom(s):
        if s == "0":
            return "NONE"
        if s.startswith("FORCEWAKE_MEDIA"):
            return "MEDIA"
        return {"FORCEWAKE_RENDER": "RENDER", "FORCEWAKE_GT": "GT"}.get(s, "?" + s)
    up = [(int(a, 16), int(b, 16), _dom(d)) for a, b, d in re.findall(
        r"GEN_FW_RANGE\((0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(\w+)\)", body.group(1))]
    ours = [(int(a, 16), int(b, 16), d) for a, b, d in re.findall(
        r"IG_GTW_ROW\((0x[0-9A-Fa-f]+)u,\s*(0x[0-9A-Fa-f]+)u,\s*IG_GTW_DOMAIN_(\w+)\)", hdr)]
    if len(up) != len(ours):
        rep.failed(label, f"row count: kext {len(ours)}, upstream {len(up)}")
    else:
        bad = [i for i, (x, y) in enumerate(zip(ours, up)) if x != y]
        if bad:
            i = bad[0]
            rep.failed(label, f"{len(bad)} rows differ; first is row {i}: "
                              f"kext {ours[i]}, upstream {up[i]}")
        else:
            rep.passed(label, f"{len(up)} rows identical")


def intel_gt_citation_checks(rep, gt_cmd, gt_probe, gt_ctx,
                             engine_text, lrc_text, cmds_text):
    """
    Every _I915_ constant in the GT stack, against the header it cites.

    gt_cmd / gt_probe / gt_ctx are parsed #define tables. engine_text,
    lrc_text and cmds_text are the RAW text of
    reference/i915/intel_engine_regs.h, reference/i915/intel_lrc_reg.h and
    reference/i915/intel_gpu_commands.h.
    """

    #
    #  1. The four ring offsets from the engine base.
    #
    #  These were CITED BY ARITHMETIC before the vendoring: i915_reg.h:289
    #  spells PRB0_BASE as (0x2030 - 0x30), so the ring block sits 0x30 above
    #  RENDER_RING_BASE and the four registers follow at 0x30/0x34/0x38/0x3C.
    #  That derivation was correct and it was still a derivation over a
    #  subtraction in one line of a display header.
    #
    #  intel_engine_regs.h names all four directly, so the arithmetic and the
    #  name can now be checked against EACH OTHER. Two independent derivations
    #  agreeing is a measurement; one derivation transcribed is a copy.
    #
    for kext_name, i915_name in (
        ("IG_GT_RING_TAIL_OFFSET_I915_0x30",  "RING_TAIL"),
        ("IG_GT_RING_HEAD_OFFSET_I915_0x34",  "RING_HEAD"),
        ("IG_GT_RING_START_OFFSET_I915_0x38", "RING_START"),
        ("IG_GT_RING_CTL_OFFSET_I915_0x3C",   "RING_CTL"),
    ):
        label = f"{kext_name} vs {i915_name}()"
        want = _gt_find_hex(rep, label, _ENGINE_REG_RE, i915_name,
                            engine_text, "intel_engine_regs.h")
        if want is None:
            continue

        got = resolve(kext_name, gt_cmd)
        if got is None:
            rep.failed(label, f"{kext_name} is not defined in "
                              f"{GT_CMD_HEADER.name}, or does not resolve")
        elif got != want:
            rep.failed(label, f"kext says 0x{got:X}, intel_engine_regs.h "
                              f"says 0x{want:X}")
        else:
            rep.passed(label, f"0x{got:X}")

    #
    #  2. The first LRI pair's register offset.
    #
    #  This is the one that changed STATUS rather than value. It was
    #  IG_GTP_FIRST_PAIR_UNCITED until 2026-08-26, because reference/i915/ held
    #  no LRC or engine header at all and this tree could not say what the
    #  slot 2/3 pair holds. RING_CONTEXT_CONTROL(base) is (base) + 0x244, and
    #  the guessed value was right.
    #
    #  A guess that turns out correct is STILL A GUESS until something
    #  independent agrees with it. This check is that something, and it is also
    #  what stops the two from drifting apart later.
    #
    label = "IG_GTP_FIRST_PAIR_I915_0x244 vs RING_CONTEXT_CONTROL()"
    want = _gt_find_hex(rep, label, _ENGINE_REG_RE, "RING_CONTEXT_CONTROL",
                        engine_text, "intel_engine_regs.h")
    if want is not None:
        got = resolve("IG_GTP_FIRST_PAIR_I915_0x244", gt_probe)
        if got is None:
            rep.failed(label, "constant not defined in "
                              f"{GT_PROBE_HEADER.name} -- was it renamed "
                              "back to the UNCITED spelling?")
        elif got != want:
            rep.failed(label, f"kext says 0x{got:X}, intel_engine_regs.h "
                              f"says 0x{want:X}")
        else:
            rep.passed(label, f"0x{got:X}")

    #
    #  3. The register-state page slot layout.
    #
    #  READ THE `+ 1` CAREFULLY, because it is the whole reason this check is
    #  worth writing rather than the reason it is easy. intel_lrc_reg.h spells
    #  CTX_RING_HEAD as (0x04 + 1), and what that names is the index of the
    #  VALUE dword. The OFFSET dword sits at 0x04 and the pair occupies slots
    #  4/5.
    #
    #  Somebody reading that header quickly pins slot 5 as HEAD, which is off
    #  by one for every pair in the image and walks the whole thing one dword
    #  out of step -- an image that composes cleanly, passes every arithmetic
    #  suite, and describes a different ring. So the comparison below is
    #  against the 0xNN inside the parentheses, never against 0xNN + 1.
    #
    #  These constants are named POLICY on the kext side and they STAY named
    #  POLICY. The slot layout is still this project's choice; upstream
    #  agreeing with a choice made independently is corroboration, and
    #  renaming them _I915_ would claim the layout was read off the header,
    #  which would erase the one thing that makes the agreement informative.
    #
    for kext_name, i915_name in (
        ("IG_GTC_FIRST_PAIR_SLOT_POLICY_2",   "CTX_CONTEXT_CONTROL"),
        ("IG_GTC_SLOT_RING_HEAD_POLICY_0x04", "CTX_RING_HEAD"),
        ("IG_GTC_SLOT_RING_TAIL_POLICY_0x06", "CTX_RING_TAIL"),
        ("IG_GTC_SLOT_RING_START_POLICY_0x08", "CTX_RING_START"),
        ("IG_GTC_SLOT_RING_CTL_POLICY_0x0A",  "CTX_RING_CTL"),
    ):
        label = f"{kext_name} vs {i915_name}"
        want = _gt_find_hex(rep, label, _LRC_SLOT_RE, i915_name,
                            lrc_text, "intel_lrc_reg.h")
        if want is None:
            continue

        got = resolve(kext_name, gt_ctx)
        if got is None:
            rep.failed(label, f"{kext_name} is not defined in "
                              f"{GT_CTX_HEADER.name}, or does not resolve")
        elif got != want:
            rep.failed(label, f"kext puts the OFFSET dword at slot {got}, "
                              f"intel_lrc_reg.h puts it at {want} "
                              f"(it spells the VALUE dword {want + 1})")
        else:
            rep.passed(label, f"slot {got}")

    #
    #  4. The pair stride, which is what makes section 3 a LAYOUT check rather
    #     than five unrelated point checks.
    #
    #  Upstream's four ring pairs sit at 0x04, 0x06, 0x08, 0x0A -- stride two,
    #  because each pair is an offset dword followed by a value dword. If a
    #  future edit moved all five kext slots by the same amount, every check
    #  above would redden, which is fine. But if somebody changed the SLOT
    #  constants to the value-dword spelling (5, 7, 9, 11), the stride would be
    #  unchanged and section 3 would catch it only because it compares absolute
    #  slots. This arm pins the relation itself so the intent is recorded
    #  rather than implied.
    #
    label = "LRC ring pair stride"
    slots = []
    for i915_name in ("CTX_RING_HEAD", "CTX_RING_TAIL",
                      "CTX_RING_START", "CTX_RING_CTL"):
        v = _gt_find_hex(rep, label, _LRC_SLOT_RE, i915_name,
                         lrc_text, "intel_lrc_reg.h")
        if v is None:
            slots = None
            break
        slots.append(v)

    if slots:
        strides = [b - a for a, b in zip(slots, slots[1:])]
        if set(strides) != {2}:
            rep.failed(label, f"expected every pair two dwords apart, "
                              f"upstream gives strides {strides} -- the "
                              f"offset/value pairing this image assumes does "
                              f"not hold in intel_lrc_reg.h")
        else:
            rep.passed(label, "HEAD/TAIL/START/CTL two dwords apart")

    #
    #  4. The MI command opcodes, against intel_gpu_commands.h.
    #
    #  These were _POLICY_ until 2026-08-27 on the grounds, spelled in
    #  IntelGtCmd.hpp's own opcode block, that the MI command family "lives in
    #  i915's gt/ headers, which were never vendored here". That was true when
    #  written and became false on 2026-08-26, when intel_gpu_commands.h was
    #  vendored alongside intel_lrc_reg.h and intel_engine_regs.h. All four
    #  values were correct as guessed; what changed is that they stopped being
    #  guesses.
    #
    #  Upstream composes every one of them through
    #
    #      MI_INSTR(opcode, flags) = __INSTR(INSTR_MI_CLIENT) | (opcode) << 23
    #                                | (flags)
    #
    #  so the opcode is the FIRST argument of the MI_INSTR call, and the value
    #  compared below is that argument alone -- never the composed word. A
    #  checker comparing against the composed word would be comparing a header
    #  constant that deliberately holds only the opcode field against a value
    #  carrying the client bits too, and would fail while being wrong.
    #
    #  MI_NOOP is spelled MI_INSTR(0, 0) upstream -- a bare 0, not 0x00 -- so
    #  the pattern accepts a decimal or hex first argument and int(_, 0) reads
    #  either. Spelling it hex-only would fail on a correct citation.
    #
    for kext_name, up_name in (
        ("IG_GT_MI_OPCODE_NOOP_I915_0x00",        "MI_NOOP"),
        ("IG_GT_MI_OPCODE_BB_START_I915_0x31",    "MI_BATCH_BUFFER_START"),
        ("IG_GT_MI_OPCODE_STORE_DWORD_I915_0x20", "MI_STORE_DWORD_IMM"),
        ("IG_GT_MI_OPCODE_BB_END_I915_0x0A",      "MI_BATCH_BUFFER_END"),
    ):
        label = f"{kext_name} vs intel_gpu_commands.h {up_name}"

        if cmds_text is None:
            rep.failed(label, "reference/i915/intel_gpu_commands.h is absent, "
                              "so this citation cannot be checked")
            continue

        #  MI_BATCH_BUFFER_START  MI_INSTR(0x31, 0)
        #  MI_BATCH_BUFFER_START_GEN8 is a DIFFERENT name sharing this prefix,
        #  so the name is anchored on both sides -- \b after the name would
        #  still match the _GEN8 spelling's prefix under some engines, and the
        #  required whitespace before MI_INSTR is what actually separates them.
        pat = (r"#define\s+" + re.escape(up_name) +
               r"\s+MI_INSTR\(\s*(0[xX][0-9a-fA-F]+|\d+)\s*,")
        m = re.search(pat, cmds_text)
        if m is None:
            rep.failed(label, f"intel_gpu_commands.h no longer spells "
                              f"{up_name} as MI_INSTR(opcode, flags); the "
                              f"citation is now unverifiable")
            continue

        upstream = int(m.group(1), 0)
        #  resolve(), never a bare .get(): parse_defines() returns the #define
        #  BODY as TEXT, so "0x31" != 0x31 and a raw comparison fails on a
        #  correct citation. Sections 1-3 all go through resolve() for this
        #  reason and the first draft of this section did not.
        ours = resolve(kext_name, gt_cmd)
        if ours is None:
            rep.failed(label, f"{kext_name} is not defined in "
                              f"{GT_CMD_HEADER.name}, or does not resolve")
        elif ours != upstream:
            rep.failed(label, f"we say {ours:#04x}, {up_name} is "
                              f"{upstream:#04x}")
        else:
            rep.passed(label, f"opcode {upstream:#04x}")

    #
    #  The MI client field. Upstream keeps it as a plain #define rather than
    #  inside MI_INSTR, and __INSTR() shifts it by INSTR_CLIENT_SHIFT.
    #
    label = "IG_GT_MI_CLIENT_I915_0 vs intel_gpu_commands.h INSTR_MI_CLIENT"
    if cmds_text is None:
        rep.failed(label, "reference/i915/intel_gpu_commands.h is absent, "
                          "so this citation cannot be checked")
    else:
        m = re.search(r"#define\s+INSTR_MI_CLIENT\s+(0[xX][0-9a-fA-F]+|\d+)",
                      cmds_text)
        if m is None:
            rep.failed(label, "intel_gpu_commands.h no longer defines "
                              "INSTR_MI_CLIENT; the citation is now "
                              "unverifiable")
        else:
            upstream = int(m.group(1), 0)
            ours = resolve("IG_GT_MI_CLIENT_I915_0", gt_cmd)
            if ours is None:
                rep.failed(label, "IG_GT_MI_CLIENT_I915_0 is not defined in "
                                  f"{GT_CMD_HEADER.name}, or does not resolve")
            elif ours != upstream:
                rep.failed(label, f"we say {ours}, INSTR_MI_CLIENT is "
                                  f"{upstream}")
            else:
                rep.passed(label, f"MI client {upstream}")

    #
    #  The two shifts the composition uses. These stay POLICY-named on the kext
    #  side because they are field POSITIONS this project chose to spell as
    #  separate constants, but their values are checkable: upstream writes the
    #  opcode shift as a literal 23 inside MI_INSTR and the client shift as
    #  INSTR_CLIENT_SHIFT. Checking them here is what makes the opcode checks
    #  above load-bearing -- a correct opcode placed at the wrong bit is the
    #  same hang as a wrong opcode, and section 4 alone cannot see it.
    #
    label = "IG_GT_MI_CLIENT_SHIFT_POLICY_29 vs INSTR_CLIENT_SHIFT"
    if cmds_text is None:
        rep.failed(label, "reference/i915/intel_gpu_commands.h is absent, "
                          "so this citation cannot be checked")
    else:
        m = re.search(r"#define\s+INSTR_CLIENT_SHIFT\s+(\d+)", cmds_text)
        if m is None:
            rep.failed(label, "intel_gpu_commands.h no longer defines "
                              "INSTR_CLIENT_SHIFT")
        else:
            upstream = int(m.group(1))
            ours = resolve("IG_GT_MI_CLIENT_SHIFT_POLICY_29", gt_cmd)
            if ours is None:
                rep.failed(label, "IG_GT_MI_CLIENT_SHIFT_POLICY_29 is not "
                                  f"defined in {GT_CMD_HEADER.name}, or does "
                                  f"not resolve")
            elif ours != upstream:
                rep.failed(label, f"we say {ours}, INSTR_CLIENT_SHIFT is "
                                  f"{upstream}")
            else:
                rep.passed(label, f"client shift {upstream}")

    label = "IG_GT_MI_OPCODE_SHIFT_POLICY_23 vs MI_INSTR opcode placement"
    if cmds_text is None:
        rep.failed(label, "reference/i915/intel_gpu_commands.h is absent, "
                          "so this citation cannot be checked")
    else:
        #  (__INSTR(INSTR_MI_CLIENT) | (opcode) << 23 | (flags))
        m = re.search(r"#define\s+MI_INSTR\(opcode,\s*flags\)[^\n]*\n?[^\n]*?"
                      r"\(opcode\)\s*<<\s*(\d+)", cmds_text)
        if m is None:
            rep.failed(label, "could not read the opcode shift out of "
                              "MI_INSTR in intel_gpu_commands.h")
        else:
            upstream = int(m.group(1))
            ours = resolve("IG_GT_MI_OPCODE_SHIFT_POLICY_23", gt_cmd)
            if ours is None:
                rep.failed(label, "IG_GT_MI_OPCODE_SHIFT_POLICY_23 is not "
                                  f"defined in {GT_CMD_HEADER.name}, or does "
                                  f"not resolve")
            elif ours != upstream:
                rep.failed(label, f"we say {ours}, MI_INSTR shifts the opcode "
                                  f"by {upstream}")
            else:
                rep.passed(label, f"opcode shift {upstream}")


def main():
    ap = argparse.ArgumentParser(
        description="Verify kext register constants against NVIDIA's "
                    "open-gpu-kernel-modules and the vendored i915 headers."
    )
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="print passing checks too")
    args = ap.parse_args()

    if not KEXT_HEADER.is_file():
        print(f"error: {KEXT_HEADER} not found", file=sys.stderr)
        return 1

    src, src_param, missing = merge_sources()

    if missing:
        print("warning: some NVIDIA headers were not found -- "
              "related checks will be skipped:")
        for p in missing:
            print(f"  {p.relative_to(ROOT)}")
        print()

    if not src:
        print("error: no NVIDIA headers parsed. Clone the reference repo:",
              file=sys.stderr)
        print("  git clone --depth 1 "
              "https://github.com/NVIDIA/open-gpu-kernel-modules", file=sys.stderr)
        return 1

    kext, kext_param = parse_defines(KEXT_HEADER)

    #
    # NVFalcon.hpp includes NVDecode.hpp, so its own constants are parsed
    # separately and then merged over the base set. Merging (rather than
    # checking in isolation) means a Falcon check can reference a constant
    # defined in NVDecode.hpp -- e.g. NV_BAR0_EXPECTED_LENGTH.
    #
    falcon, falcon_param = parse_defines(FALCON_HEADER)

    #
    # NVGspBoot.hpp includes both of the above and composes its constants out
    # of their field positions, so it is merged last -- a composed value can
    # only resolve when the shifts it references are already in the table.
    #
    gspboot, gspboot_param = parse_defines(GSPBOOT_HEADER)

    #
    # NVGspStage.hpp includes all three and derives its addressing window from
    # their field widths, so it merges last for the same reason NVGspBoot.hpp
    # merges before it.
    #
    stage, stage_param = parse_defines(STAGE_HEADER)

    #
    # The Intel track. Parsed into its OWN table rather than merged into the
    # NVIDIA one: IntelDisplay.hpp includes none of those headers and composes
    # nothing out of them, so merging would only build a namespace in which a
    # collision carries no information.
    #
    intel, _intel_param = parse_defines(INTEL_HEADER)

    #
    # The decode layer, parsed into its own table for the same reason: it
    # includes nothing and composes nothing out of IntelDisplay.hpp, so a
    # shared namespace would only create collisions that carry no information.
    #
    intel_fb, _intel_fb_param = parse_defines(INTEL_FB_HEADER)

    #
    # The scanout layer, parsed into its own table for the same reason as the two
    # above: IntelScanout.hpp includes nothing and composes nothing out of
    # either, so a shared namespace would only create collisions that carry no
    # information.
    #
    intel_scan, _intel_scan_param = parse_defines(INTEL_SCAN_HEADER)

    #
    # The pixel layer. Its own table too, and here the reason is sharper than for
    # the three above: IntelPixel.hpp DOES include IntelFramebuffer.hpp, so a
    # merged namespace would hold each IG_FB_FORMAT_* twice and a collision would
    # mean nothing. Parsing it alone keeps the IG_PIXEL_ family the only thing
    # this table describes.
    #
    intel_pixel, _intel_pixel_param = parse_defines(INTEL_PIXEL_HEADER)

    #
    # The raw text as well as the defines, because igTransBase() and
    # igPipeBase() are switches -- their twelve addresses are not #defines and
    # parse_defines() cannot see one of them.
    #
    intel_text = (INTEL_HEADER.read_text(encoding="utf-8", errors="replace")
                  if INTEL_HEADER.is_file() else None)

    #
    # The second Intel header. Its own table again, for the same reason: it
    # includes nothing and composes nothing out of the display constants, so a
    # shared namespace would only manufacture collisions that carry no
    # information.
    #
    intel_id, _intel_id_param = parse_defines(INTEL_ID_HEADER)

    #
    # i915_pciids.h as RAW TEXT rather than through merge_i915_sources(). The
    # PCI IDs are positional arguments inside variadic macro bodies, so there
    # is no name for parse_defines() to key on -- see i915_id_list().
    #
    pciids_text = (I915_PCIIDS.read_text(encoding="utf-8", errors="replace")
                   if I915_PCIIDS.is_file() else None)

    #
    # The three GT headers, each parsed into its OWN table.
    #
    # Same reasoning as the four Intel display tables above and it holds here
    # too: IntelGtProbe.hpp includes IntelGtCmd.hpp and IntelGtContext.hpp, so a
    # merged namespace would hold each IG_GT_ constant twice and a collision
    # would carry no information. Keeping them apart also means a failure names
    # the file the constant actually lives in, which is the whole point of the
    # citation checks.
    #
    gt_cmd,   _gt_cmd_param   = parse_defines(GT_CMD_HEADER)
    gt_ctx,   _gt_ctx_param   = parse_defines(GT_CTX_HEADER)
    gt_probe, _gt_probe_param = parse_defines(GT_PROBE_HEADER)

    #
    # The two vendored gt/ headers as RAW TEXT, for the reason spelled out above
    # intel_gt_citation_checks(): RING_TAIL(base) and its siblings are
    # function-like macros wrapping _MMIO(), which resolve() cannot evaluate. An
    # unresolvable symbol makes a check SKIP, and a skip in a citation checker
    # is a silent pass over the exact claim the checker exists to test.
    #
    # Read here rather than inside the checker so a missing file shows up on the
    # banner below, where a reader actually looks, rather than only inside one
    # failure line in a wall of ok rows.
    #
    lrc_path    = I915 / "intel_lrc_reg.h"
    engine_path = I915 / "intel_engine_regs.h"
    cmds_path   = I915 / "intel_gpu_commands.h"

    lrc_text = (lrc_path.read_text(encoding="utf-8", errors="replace")
                if lrc_path.is_file() else None)
    engine_text = (engine_path.read_text(encoding="utf-8", errors="replace")
                   if engine_path.is_file() else None)
    #
    # The THIRD gt/ header, added 2026-08-27 with the MI opcode citations. It was
    # vendored on 2026-08-26 alongside the other two and then read by nothing for
    # a day, while IntelGtCmd.hpp's opcode block still said the MI command family
    # "was never vendored here". A vendored file no checker opens is evidence the
    # tree does not actually hold.
    #
    cmds_text = (cmds_path.read_text(encoding="utf-8", errors="replace")
                 if cmds_path.is_file() else None)

    i915_src, i915_missing = merge_i915_sources()

    if i915_missing:
        print("warning: some vendored i915 headers were not found -- "
              "related Intel checks will be skipped:")
        for p in i915_missing:
            print(f"  {p.relative_to(ROOT)}")
        print()

    merged = dict(kext)
    merged.update(falcon)
    merged.update(gspboot)
    merged.update(stage)
    merged_param = dict(kext_param)
    merged_param.update(falcon_param)
    merged_param.update(gspboot_param)
    merged_param.update(stage_param)

    print(f"kext headers  : {KEXT_HEADER.name} "
          f"({len(kext)} defines, {len(kext_param)} macros)")

    if falcon:
        print(f"                {FALCON_HEADER.name} "
              f"({len(falcon)} defines, {len(falcon_param)} macros)")
    else:
        print(f"                {FALCON_HEADER.name} NOT FOUND -- "
              f"Phase 4 checks will all fail")

    if gspboot:
        print(f"                {GSPBOOT_HEADER.name} "
              f"({len(gspboot)} defines, {len(gspboot_param)} macros)")
    else:
        print(f"                {GSPBOOT_HEADER.name} NOT FOUND -- "
              f"Phase 5 checks will all be skipped")

    #
    # The banner is the only place a reader learns WHICH headers were actually
    # parsed. Leaving NVGspStage.hpp off it while its checks run would make a
    # clean score look like it covered three headers when it covered four --
    # and, worse, a missing NVGspStage.hpp would silently drop the addressing
    # window checks with nothing on screen to say so.
    #
    if stage:
        print(f"                {STAGE_HEADER.name} "
              f"({len(stage)} defines, {len(stage_param)} macros)")
    else:
        print(f"                {STAGE_HEADER.name} NOT FOUND -- "
              f"Phase 5 addressing-window checks will all fail")

    #
    # Same reasoning as the NVGspStage.hpp line above, and it bites harder
    # here: until this change the banner named four headers, IntelDisplay.hpp
    # was checked by nothing, and a clean score read as coverage of a file this
    # gate had never opened.
    #
    if intel:
        print(f"                {INTEL_HEADER.name} "
              f"({len(intel)} defines)")
    else:
        print(f"                {INTEL_HEADER.name} NOT FOUND -- "
              f"Intel display checks will all fail")

    #
    # Printed for the same reason as the line above it. A header that silently
    # stopped parsing would otherwise leave run_intel_fb_checks() to report one
    # failure line about an empty table, in a wall of 200+ ok rows, and the
    # banner is where a reader actually looks to see WHICH files were read.
    #
    if intel_fb:
        print(f"                {INTEL_FB_HEADER.name} "
              f"({len(intel_fb)} defines)")
    else:
        print(f"                {INTEL_FB_HEADER.name} NOT FOUND -- "
              f"Intel framebuffer checks will all fail")

    #
    # The three GT headers on the banner, same reasoning as every line above and
    # one reason of their own: these are the first kext headers this gate opens
    # for a NAMED handful of constants rather than for a whole-header set-check,
    # so their absence would drop nine checks with nothing on screen to say the
    # score fell. A gate that quietly measures less is the failure mode this
    # whole file is arranged against.
    #
    if gt_cmd:
        print(f"                {GT_CMD_HEADER.name} "
              f"({len(gt_cmd)} defines, cited constants only)")
    else:
        print(f"                {GT_CMD_HEADER.name} NOT FOUND -- "
              f"GT ring citation checks will all fail")

    if gt_ctx:
        print(f"                {GT_CTX_HEADER.name} "
              f"({len(gt_ctx)} defines, slot layout only)")
    else:
        print(f"                {GT_CTX_HEADER.name} NOT FOUND -- "
              f"GT slot layout checks will all fail")

    if gt_probe:
        print(f"                {GT_PROBE_HEADER.name} "
              f"({len(gt_probe)} defines, first-pair offset only)")
    else:
        print(f"                {GT_PROBE_HEADER.name} NOT FOUND -- "
              f"the first-pair citation check will fail")

    print(f"NVIDIA headers: {len(src)} defines, {len(src_param)} macros "
          f"from {len(SOURCE_HEADERS) - len(missing)} files")
    print(f"i915 headers  : {len(i915_src)} defines "
          f"from {len(I915_HEADERS) - len(i915_missing)} files")

    #
    # The three gt/ headers are named SEPARATELY from the count above, because
    # the count above says how many defines merged and says nothing about which
    # files they came from -- and these three are read as raw text as well, a
    # path the merge count cannot describe at all.
    #
    # intel_gpu_commands.h was vendored 2026-08-26 and named here on 2026-08-27,
    # with the MI opcode citations. Between those two dates this line printed a
    # complete-looking list of the gt/ evidence while omitting a third of it,
    # which is the direction that reads as coverage rather than as a gap.
    #
    print(f"gt/ citations : intel_lrc_reg.h "
          f"{'read' if lrc_text else 'NOT FOUND'}, "
          f"intel_engine_regs.h "
          f"{'read' if engine_text else 'NOT FOUND'}, "
          f"intel_gpu_commands.h "
          f"{'read' if cmds_text else 'NOT FOUND'}")
    print()

    rep = Report(args.verbose)

    def run_checks(checks, table):
        for check in checks:
            kind = check[0]
            if kind == "offset":
                check_offset(rep, table, src, check[1], check[2])
            elif kind == "value":
                check_value(rep, table, src, check[1], check[2])
            elif kind == "field":
                check_field(rep, table, src, check[1], check[2], check[3])

    if args.verbose:
        print("NVDecode.hpp -- Phase 3 / 3.5 identity and firmware health")
        print()

    run_checks(CHECKS, kext)

    for kname, nname in PARAM_MACRO_CHECKS:
        check_param_macro(rep, kext_param, src_param, kname, nname)

    sanity_checks(rep, kext)

    if args.verbose:
        print()
        print("NVFalcon.hpp -- Phase 4 Falcon / RISC-V / GSP")
        print()

    run_checks(FALCON_CHECKS, merged)

    for kname, nname in FALCON_PARAM_MACRO_CHECKS:
        check_param_macro(rep, merged_param, src_param, kname, nname)

    falcon_sanity_checks(rep, merged)

    if args.verbose:
        print()
        print("NVGspBoot.hpp -- Phase 5 composed constants")
        print()

    gspboot_sanity_checks(rep, merged)

    if args.verbose:
        print()
        print("NVGspStage.hpp -- Phase 5 DMA addressing window")
        print()

    stage_sanity_checks(rep, merged, src)

    if args.verbose:
        print()
        print("IntelDisplay.hpp -- Gen12 transcoder, pipe and plane addressing")
        print()

    run_intel_checks(rep, intel, i915_src)
    intel_sanity_checks(rep, intel, i915_src, intel_text)

    if args.verbose:
        print()
        print("IntelFramebuffer.hpp -- Gen12 plane register field decoding")
        print()

    run_intel_fb_checks(rep, intel_fb, i915_src)
    intel_fb_sanity_checks(rep, intel_fb, i915_src)

    #
    # Runs AFTER both Intel headers have been parsed, because it is the only
    # check in this script that reads two kext headers at once. Neither
    # header's own row table can see a disagreement between them: both sides
    # are individually correct against the vendored i915 literals and still
    # wrong as a pair.
    #
    intel_cross_header_checks(rep, intel, intel_fb)

    #
    # The IG_MODE_* verdict set, read out of the header rather than from a
    # hand-kept list. tests/test_intel_display.cpp pins each verdict that
    # exists today; this catches the EIGHTH one somebody adds without a
    # matching checkU32 line.
    #
    #
    # The DDI constants the row table cannot express, derived from the vendored
    # composed shifts rather than compared to a second typed copy.
    #
    intel_ddi_checks(rep, intel, i915_src)

    #
    # The GT citations, added 2026-08-26 with the three gt/ headers.
    #
    # Called UNCONDITIONALLY rather than under an `if gt_cmd:` guard, and that
    # is deliberate. _gt_find_hex() FAILS on a missing vendored file instead of
    # returning early, so a guard here would convert nine failures into nine
    # ABSENCES -- the score would simply fall, with nothing on screen saying a
    # claim stopped being checked. That is the vacuity direction this checker
    # was written against, reintroduced at its own call site.
    #
    intel_gt_citation_checks(rep, gt_cmd, gt_probe, gt_ctx,
                             engine_text, lrc_text, cmds_text)
    intel_gt_forcewake_checks(rep)

    intel_verdict_set_checks(rep, intel, "IG_MODE_", 7, "igModeConsistent()")
    intel_verdict_set_checks(rep, intel, "IG_LINK_", 4, "igTransconfDdiLinked()")
    intel_verdict_set_checks(rep, intel, "IG_USABLE_", 4, "igPipeUsable()")
    intel_verdict_set_checks(rep, intel, "IG_JOIN_", 4, "igPlaneMatchesPipe()")

    #
    # The read-guard family, extracted from IntelDisplayProbe::regRead32() on
    # 2026-08-17 so that something could execute it. Listed here for the same
    # reason as the four above: test_intel_display.cpp pins each verdict to a
    # bare literal from a HAND-KEPT list, so a fifth reason added without a
    # matching checkU32 line would be pinned by nothing.
    #
    intel_verdict_set_checks(rep, intel, "IG_READ_", 5, "igReadDecision()")

    #
    # The group-read family, added 2026-08-18 alongside igSampleVerdict(). Two
    # arms is the smallest a family can be and still be a family, and it is
    # listed here for exactly that reason: two is where somebody is most likely
    # to add a third (a PARTIAL, say) without pinning it, because two looks like
    # a bool that grew a name.
    #
    intel_verdict_set_checks(rep, intel, "IG_SAMPLE_", 2, "igSampleVerdict()")

    #
    # The scanout family, wired 2026-08-19, and the LARGEST in the Intel track at
    # seven arms -- while being the LAST to get a set-check. igScanDescribe() is
    # what decides whether a firmware-programmed plane can be handed to
    # IOFramebuffer at all, so an eighth refusal reason added without a pin is a
    # refusal the caller cannot tell from one it already handles.
    #
    # This is the one call that needs an exclusion list, because IG_SCAN_ is the
    # only Intel verdict prefix that also covers sentinels and masks. Spelling
    # the five by name rather than filtering on value is deliberate: a
    # value-based rule ("skip anything above the limit") would ALSO skip a real
    # eighth verdict somebody added above the stale bound, which is precisely
    # the case this arm exists to catch.
    #
    intel_verdict_set_checks(
        rep, intel_scan, "IG_SCAN_", 7, "igScanDescribe()",
        exclude=("IG_SCAN_INVALID32", "IG_SCAN_INVALID64",
                 "IG_SCAN_EXTENT_POLICY_CEILING", "IG_SCAN_SURF_ADDR_MASK",
                 "IG_SCAN_SURF_ALIGN_BYTES"),
        header="IntelScanout.hpp")

    #
    # The pixel family, wired 2026-08-20. Three arms -- OK, UNKNOWN_FORMAT,
    # UNDESCRIBABLE -- and it is the SMALLEST verdict family in the Intel track
    # while needing the LONGEST exclusion list, twelve names against three
    # verdicts. That ratio is the whole reason the exclude parameter exists:
    # IG_PIXEL_ covers a sentinel (0xFFFFFFFF), a component count, two
    # bits-per-pixel values, two component widths and six channel masks, so a
    # bare prefix sweep computes max(codes) as the sentinel and fails the
    # limit == largest equality while being wrong about the world.
    #
    # Spelled by name rather than filtered on value, same as the scanout arm: a
    # rule of the form "skip anything above the limit" would also skip a real
    # fourth verdict added above a stale bound, which is the case this arm
    # exists to catch.
    #
    # The family is small on purpose and the two refusals are not
    # interchangeable. UNKNOWN_FORMAT is a hole in THIS driver's decode or a
    # Gen12 code IntelFramebuffer.hpp has never named; UNDESCRIBABLE is a fact
    # about IOKit's pixel vocabulary -- RGB_565 against a 5-5-5 direct type, and
    # a 64-bit half-float with no pixel type this tree has witnessed a name for.
    # Only the first is something somebody can go and fix, which is why
    # igPixelDescribe() decides it first.
    #
    intel_verdict_set_checks(
        rep, intel_pixel, "IG_PIXEL_", 3, "igPixelDescribe()",
        exclude=("IG_PIXEL_INVALID32", "IG_PIXEL_COMPONENT_COUNT",
                 "IG_PIXEL_BPP_XRGB_8888", "IG_PIXEL_BPP_XRGB_2101010",
                 "IG_PIXEL_COMPONENT_BITS_8888",
                 "IG_PIXEL_COMPONENT_BITS_2101010",
                 "IG_PIXEL_MASK_R_8888", "IG_PIXEL_MASK_G_8888",
                 "IG_PIXEL_MASK_B_8888",
                 "IG_PIXEL_MASK_R_2101010", "IG_PIXEL_MASK_G_2101010",
                 "IG_PIXEL_MASK_B_2101010"),
        header="IntelPixel.hpp")

    #
    # The third verdict family, and the one that lives in the OTHER header --
    # so it is passed intel_fb, not intel. It gained its VERDICT_LIMIT only in
    # order to be checkable here; the six verdicts themselves predate it.
    #
    intel_verdict_set_checks(rep, intel_fb, "IG_FB_INHERIT_", 6,
                             "igFbInheritable()")

    if args.verbose:
        print()
        print("IntelGpuId.hpp -- Gen12 PCI device identification")
        print()

    run_intel_id_checks(rep, intel_id, pciids_text)

    total = rep.ok + len(rep.failures) + len(rep.skipped)
    print()
    print(f"{rep.ok}/{total} passed, {len(rep.failures)} failed, "
          f"{len(rep.skipped)} skipped")

    if rep.skipped and not rep.failures:
        print()
        print("Skipped checks are symbols this script could not locate in the")
        print("vendored headers -- either the name differs or the header is not")
        print("in SOURCE_HEADERS / I915_HEADERS. Each one is an unverified")
        print("constant, so chase them down rather than treating a clean run")
        print("as full coverage.")

    return 1 if rep.failures else 0


if __name__ == "__main__":
    sys.exit(main())
