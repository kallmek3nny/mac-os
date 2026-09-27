//
//  NVFalcon.hpp
//  Falcon / RISC-V / GSP register layer for GA107.
//
//  Phase 4 foundation. Like NVDecode.hpp this is DELIBERATELY DEPENDENCY-FREE:
//  no includes, every type a plain builtin, so tests/ compiles the identical
//  file under MSVC and runs it. Nothing here touches hardware -- it is address
//  arithmetic and bit-field decoding only.
//
//  Transcribed from NVIDIA/open-gpu-kernel-modules v610.57.04 (SPDX: MIT):
//    src/common/inc/swref/published/ampere/ga102/dev_falcon_v4.h
//    src/common/inc/swref/published/ampere/ga102/dev_riscv_pri.h
//    src/common/inc/swref/published/ampere/ga102/dev_gsp.h
//
//  Verified by tools/check-registers.py and tests/test_falcon.cpp.
//

#ifndef NVFalcon_hpp
#define NVFalcon_hpp

#include "NVDecode.hpp"

//
// ===========================================================================
// THE THREE ADDRESS SPACES, AND WHY GETTING THEM WRONG IS SILENT
// ===========================================================================
//
// A GSP access can be expressed relative to three different bases, and the
// NVIDIA headers use all three without ever saying so in the same file:
//
//   1. dev_falcon_v4.h  -- ENGINE-RELATIVE. MAILBOX0 is 0x40. Every engine
//                          that embeds a Falcon maps this 4 KiB block at its
//                          own base. For GSP that base is 0x110000, so the
//                          absolute address is 0x110040.
//
//   2. dev_riscv_pri.h  -- relative to NV_FALCON2_GSP_BASE, which that header
//                          defines as 0x111000. NOT 0x110000. CPUCTL at 0x388
//                          is therefore absolute 0x111388, one 4 KiB page
//                          above the Falcon block.
//
//   3. dev_gsp.h        -- already ABSOLUTE. NV_PGSP_FALCON_MAILBOX0 is
//                          spelled 0x110040 outright.
//
// Mixing 1 and 2 puts every RISC-V access 0x1000 low, landing inside the
// Falcon block where the offsets are all valid registers. That does not
// fault. It reads and writes real hardware, just the wrong hardware, and the
// symptom is a firmware boot that hangs with no error. Hence the explicit
// nvGspFalconReg() / nvGspRiscvReg() accessors below: there is no way to write
// a bare offset and have it work by accident.
// ===========================================================================
//

//
// dev_gsp.h:26 -- NV_PGSP 0x113fff:0x110000
//
#define NV_PGSP_BASE                             0x00110000
#define NV_PGSP_LIMIT                            0x00113FFF

//
// SEC2 -- dev_sec_pri.h:27, NV_PSEC 0x843fff:0x840000.
//
// This engine was invisible to this project until the mmiotrace was parsed
// correctly: tools/parse-mmiotrace.py originally placed psec at 0x114000
// (immediately after PGSP, which is where you would guess it lives) and
// filed all 25,202 SEC2 accesses as "unmapped". The header is unambiguous.
//
// It matters because SEC2 is part of the same bringup, interleaved with GSP
// rather than separate from it. From the trace: GSP's BCR_CTRL start write
// lands at t=1.119477, and SEC2's falcon reset pulse follows 171 us later at
// t=1.119648. SEC2 then sits with its core halted for ~723 ms while GSP boots,
// and its CPUCTL_ALIAS STARTCPU write at t=1.842825 comes 2 ms AFTER GSP's
// BCR_CTRL is cleared at t=1.840748.
//
// So the dependency runs GSP -> SEC2, not the other way round. A Phase 4
// implementation may bring up GSP alone; it just will not have a booted SEC2
// afterwards, and anything that needs SEC2 must replay this second sequence.
//
#define NV_PSEC_BASE                             0x00840000
#define NV_PSEC_LIMIT                            0x00843FFF

//
// dev_riscv_pri.h:27 -- NV_FALCON2_GSP_BASE
//
#define NV_FALCON2_GSP_BASE                      0x00111000

//
// The FALCON2 aperture sits one 4 KiB block above the engine base. The GSP
// value above is spelled out by dev_riscv_pri.h; SEC2's is not, but the trace
// settles it: the driver writes BCR_CTRL for SEC2 at absolute 0x841668, and
// 0x841668 - 0x668 == 0x841000 == NV_PSEC_BASE + NV_FALCON_BLOCK_SIZE. The
// same +0x1000 relationship holds for GSP (0x111000 - 0x110000), so this is
// the general rule rather than a coincidence of one engine.
//
#define NV_FALCON2_SEC_BASE                      0x00841000

//
// Falcon register blocks are 4 KiB. Anything at or above this in
// dev_falcon_v4.h is a field definition, not an offset.
//
#define NV_FALCON_BLOCK_SIZE                     0x1000


// ---------------------------------------------------------------------------
// Falcon core registers -- dev_falcon_v4.h, ENGINE-RELATIVE offsets.
// ---------------------------------------------------------------------------

#define NV_PFALCON_FALCON_IRQSCLR                0x00000004
#define NV_PFALCON_FALCON_IRQSCLR_HALT_SHIFT     4       // 4:4
#define NV_PFALCON_FALCON_IRQSCLR_HALT_MASK      0x01
#define NV_PFALCON_FALCON_IRQSCLR_SWGEN0_SHIFT   6       // 6:6
#define NV_PFALCON_FALCON_IRQSCLR_SWGEN0_MASK    0x01

#define NV_PFALCON_FALCON_IRQSTAT                0x00000008
#define NV_PFALCON_FALCON_IRQSTAT_HALT_SHIFT     4       // 4:4
#define NV_PFALCON_FALCON_IRQSTAT_HALT_MASK      0x01
#define NV_PFALCON_FALCON_IRQSTAT_SWGEN0_SHIFT   6       // 6:6
#define NV_PFALCON_FALCON_IRQSTAT_SWGEN0_MASK    0x01

#define NV_PFALCON_FALCON_IRQMSET                0x00000010
#define NV_PFALCON_FALCON_IRQMCLR                0x00000014
#define NV_PFALCON_FALCON_IRQMASK                0x00000018
#define NV_PFALCON_FALCON_IRQDEST                0x0000001C

#define NV_PFALCON_FALCON_MAILBOX0               0x00000040
#define NV_PFALCON_FALCON_MAILBOX1               0x00000044

#define NV_PFALCON_FALCON_OS                     0x00000080
#define NV_PFALCON_FALCON_RM                     0x00000084
#define NV_PFALCON_FALCON_DEBUGINFO              0x00000094

//
// HWCFG2 bit 10 says whether this Falcon can run as a RISC-V core. On GA107
// the GSP must, so a zero here means the whole Phase 4 approach is wrong for
// this part -- worth checking before anything is written.
//
#define NV_PFALCON_FALCON_HWCFG2                 0x000000F4
#define NV_PFALCON_FALCON_HWCFG2_RISCV_SHIFT     10      // 10:10
#define NV_PFALCON_FALCON_HWCFG2_RISCV_MASK      0x01
#define NV_PFALCON_FALCON_HWCFG2_RISCV_ENABLE    0x01
#define NV_PFALCON_FALCON_HWCFG2_MEM_SCRUBBING_SHIFT  12  // 12:12
#define NV_PFALCON_FALCON_HWCFG2_MEM_SCRUBBING_MASK   0x01
#define NV_PFALCON_FALCON_HWCFG2_MEM_SCRUBBING_DONE   0x00

#define NV_PFALCON_FALCON_HWCFG                  0x00000108
#define NV_PFALCON_FALCON_HWCFG_IMEM_SIZE_SHIFT  0       // 8:0
#define NV_PFALCON_FALCON_HWCFG_IMEM_SIZE_MASK   0x1FF

#define NV_PFALCON_FALCON_CPUCTL                 0x00000100
#define NV_PFALCON_FALCON_CPUCTL_STARTCPU_SHIFT  1       // 1:1
#define NV_PFALCON_FALCON_CPUCTL_STARTCPU_MASK   0x01
#define NV_PFALCON_FALCON_CPUCTL_HALTED_SHIFT    4       // 4:4
#define NV_PFALCON_FALCON_CPUCTL_HALTED_MASK     0x01
#define NV_PFALCON_FALCON_CPUCTL_ALIAS_EN_SHIFT  6       // 6:6
#define NV_PFALCON_FALCON_CPUCTL_ALIAS_EN_MASK   0x01

#define NV_PFALCON_FALCON_BOOTVEC                0x00000104

#define NV_PFALCON_FALCON_CPUCTL_ALIAS           0x00000130
#define NV_PFALCON_FALCON_CPUCTL_ALIAS_STARTCPU_SHIFT  1  // 1:1
#define NV_PFALCON_FALCON_CPUCTL_ALIAS_STARTCPU_MASK   0x01

//
// DMA engine. Phase 4 uses this to push firmware images into IMEM/DMEM.
//
#define NV_PFALCON_FALCON_DMACTL                 0x0000010C
#define NV_PFALCON_FALCON_DMACTL_REQUIRE_CTX_SHIFT     0  // 0:0
#define NV_PFALCON_FALCON_DMACTL_REQUIRE_CTX_MASK      0x01
#define NV_PFALCON_FALCON_DMACTL_DMEM_SCRUBBING_SHIFT  1  // 1:1
#define NV_PFALCON_FALCON_DMACTL_DMEM_SCRUBBING_MASK   0x01
#define NV_PFALCON_FALCON_DMACTL_IMEM_SCRUBBING_SHIFT  2  // 2:2
#define NV_PFALCON_FALCON_DMACTL_IMEM_SCRUBBING_MASK   0x01

#define NV_PFALCON_FALCON_DMATRFBASE             0x00000110
#define NV_PFALCON_FALCON_DMATRFMOFFS            0x00000114
#define NV_PFALCON_FALCON_DMATRFMOFFS_OFFS_SHIFT 0       // 23:0
#define NV_PFALCON_FALCON_DMATRFMOFFS_OFFS_MASK  0xFFFFFF

//
// DMATRFCMD is the TRIGGER, not configuration. Write MOFFS, then FBOFFS, then
// CMD -- the transfer starts on the CMD write.
//
// This is recorded here because the trace reads the other way round at first
// glance. mmio-boot.log prints 118, 114, 11C, 118, 114, 11C, ... which parses
// equally well as (CMD)(offsets) or (offsets)(CMD), and PHASE4-GSP-BRINGUP.md
// initially claimed the former. Two things settle it: s_dmaTransfer_GA102 in
// the loaded nvidia.ko 595.84 writes 0x114, 0x11C, 0x118 in that order inside
// its loop body, and the trace's last three DMA writes are 114, 11C, 118 --
// it ends on a CMD, which only happens if CMD closes each iteration. The
// leading 118 is a pre-loop priming write.
//
// Getting this backwards does not fault. It produces an off-by-one in which
// transfer each offset pair belongs to, which on real hardware means the last
// block of an image is never moved.
//
#define NV_PFALCON_FALCON_DMATRFCMD              0x00000118
#define NV_PFALCON_FALCON_DMATRFCMD_FULL_SHIFT   0       // 0:0
#define NV_PFALCON_FALCON_DMATRFCMD_FULL_MASK    0x01
#define NV_PFALCON_FALCON_DMATRFCMD_IDLE_SHIFT   1       // 1:1
#define NV_PFALCON_FALCON_DMATRFCMD_IDLE_MASK    0x01
#define NV_PFALCON_FALCON_DMATRFCMD_SEC_SHIFT    2       // 3:2
#define NV_PFALCON_FALCON_DMATRFCMD_SEC_MASK     0x03
#define NV_PFALCON_FALCON_DMATRFCMD_IMEM_SHIFT   4       // 4:4
#define NV_PFALCON_FALCON_DMATRFCMD_IMEM_MASK    0x01
#define NV_PFALCON_FALCON_DMATRFCMD_WRITE_SHIFT  5       // 5:5
#define NV_PFALCON_FALCON_DMATRFCMD_WRITE_MASK   0x01
#define NV_PFALCON_FALCON_DMATRFCMD_SIZE_SHIFT   8       // 10:8
#define NV_PFALCON_FALCON_DMATRFCMD_SIZE_MASK    0x07
#define NV_PFALCON_FALCON_DMATRFCMD_SIZE_256B    0x06
#define NV_PFALCON_FALCON_DMATRFCMD_CTXDMA_SHIFT 12      // 14:12
#define NV_PFALCON_FALCON_DMATRFCMD_CTXDMA_MASK  0x07
#define NV_PFALCON_FALCON_DMATRFCMD_SET_DMTAG_SHIFT  16  // 16:16
#define NV_PFALCON_FALCON_DMATRFCMD_SET_DMTAG_MASK   0x01

#define NV_PFALCON_FALCON_DMATRFFBOFFS           0x0000011C

//
// DMATRFBASE1 is a 9-bit field (NVIDIA dev_falcon_v4.h: DMATRFBASE1_BASE 8:0).
// It carries bits 40:32 OF THE SHIFTED ADDRESS, i.e. physical bits 48:40 --
// reference/.../ampere/kernel_gsp_falcon_ga102.c:122-123 derives BOTH halves
// from the same (srcPhysAddr >> 8), so the pair is a non-overlapping split of
// the shifted quantity, not an overlay on the unshifted one. An earlier comment
// here read it as physical bits 40:32, which OVERLAPS the 39:32 that the
// shifted BASE already carries; see NVGspBoot.hpp:119-143 for the correction
// and the numeric comparison.
//
// The driver nonetheless ALLOWS only 41 bits (NV_GSP_FW_ADDR_BITS in
// NVGspStage.hpp). That is the conservative direction under this reading -- it
// refuses addresses the hardware might have accepted and never admits one it
// would truncate. IODMACommand must be configured with a matching address width
// or the transfer silently targets the wrong page.
//
#define NV_PFALCON_FALCON_DMATRFBASE1            0x00000128
#define NV_PFALCON_FALCON_DMATRFBASE1_BASE_SHIFT 0       // 8:0
#define NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK  0x1FF

//
// IMEM/DMEM programmed-I/O windows. Indexed ports -- note the differing
// strides: IMEMC/IMEMD/IMEMT are a 16-byte triple, DMEMC/DMEMD an 8-byte pair.
//
#define NV_PFALCON_FALCON_IMEMC(i)               (0x00000180 + (i) * 16)
#define NV_PFALCON_FALCON_IMEMD(i)               (0x00000184 + (i) * 16)
#define NV_PFALCON_FALCON_IMEMT(i)               (0x00000188 + (i) * 16)
#define NV_PFALCON_FALCON_DMEMC(i)               (0x000001C0 + (i) * 8)
#define NV_PFALCON_FALCON_DMEMD(i)               (0x000001C4 + (i) * 8)

#define NV_PFALCON_FALCON_IMEMC_OFFS_SHIFT       2       // 7:2
#define NV_PFALCON_FALCON_IMEMC_OFFS_MASK        0x3F
#define NV_PFALCON_FALCON_IMEMC_BLK_SHIFT        8       // 23:8
#define NV_PFALCON_FALCON_IMEMC_BLK_MASK         0xFFFF
#define NV_PFALCON_FALCON_IMEMC_AINCW_SHIFT      24      // 24:24
#define NV_PFALCON_FALCON_IMEMC_AINCW_MASK       0x01
#define NV_PFALCON_FALCON_IMEMC_SECURE_SHIFT     28      // 28:28
#define NV_PFALCON_FALCON_IMEMC_SECURE_MASK      0x01

#define NV_PFALCON_FALCON_DMEMC_OFFS_SHIFT       2       // 7:2
#define NV_PFALCON_FALCON_DMEMC_OFFS_MASK        0x3F
#define NV_PFALCON_FALCON_DMEMC_BLK_SHIFT        8       // 23:8
#define NV_PFALCON_FALCON_DMEMC_BLK_MASK         0xFFFF
#define NV_PFALCON_FALCON_DMEMC_AINCW_SHIFT      24      // 24:24
#define NV_PFALCON_FALCON_DMEMC_AINCW_MASK       0x01

#define NV_PFALCON_FALCON_INTR_RETRIGGER(i)      (0x000003E8 + (i) * 4)
#define NV_PFALCON_FALCON_INTR_RETRIGGER_SIZE_1  2


// ---------------------------------------------------------------------------
// RISC-V core -- dev_riscv_pri.h, relative to NV_FALCON2_GSP_BASE (0x111000).
// ---------------------------------------------------------------------------

#define NV_PRISCV_RISCV_CPUCTL                   0x00000388
#define NV_PRISCV_RISCV_CPUCTL_HALTED_SHIFT      4       // 4:4
#define NV_PRISCV_RISCV_CPUCTL_HALTED_MASK       0x01
#define NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT_SHIFT 7       // 7:7
#define NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT_MASK  0x01
#define NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT_ACTIVE 0x01

#define NV_PRISCV_RISCV_ICD_CMD                  0x000003D0
#define NV_PRISCV_RISCV_ICD_ADDR0                0x000003D4
#define NV_PRISCV_RISCV_ICD_ADDR1                0x000003D8
#define NV_PRISCV_RISCV_ICD_RDATA0               0x000003E4
#define NV_PRISCV_RISCV_ICD_RDATA1               0x000003E8

#define NV_PRISCV_RISCV_RPC                      0x000003EC

#define NV_PRISCV_RISCV_TRACECTL                 0x00000400
#define NV_PRISCV_RISCV_TRACECTL_FULL_SHIFT      30      // 30:30
#define NV_PRISCV_RISCV_TRACECTL_FULL_MASK       0x01
#define NV_PRISCV_RISCV_TRACE_RDIDX              0x00000404
#define NV_PRISCV_RISCV_TRACE_WTIDX              0x00000408
#define NV_PRISCV_RISCV_TRACEPC_LO               0x0000040C
#define NV_PRISCV_RISCV_TRACEPC_HI               0x00000410

//
// The error-status block. When GSP boot fails, PRIV_ERR_STAT and HUB_ERR_STAT
// are the only diagnostics available -- there is no console. Reading these on
// every failure path is the difference between a debuggable bringup and
// guessing.
//
#define NV_PRISCV_RISCV_PRIV_ERR_STAT            0x00000500
#define NV_PRISCV_RISCV_PRIV_ERR_INFO            0x00000504
#define NV_PRISCV_RISCV_PRIV_ERR_ADDR            0x00000508
#define NV_PRISCV_RISCV_PRIV_ERR_ADDR_HI         0x0000050C
#define NV_PRISCV_RISCV_HUB_ERR_STAT             0x00000510

#define NV_PRISCV_RISCV_IRQMASK                  0x00000528
#define NV_PRISCV_RISCV_IRQDEST                  0x0000052C
#define NV_PRISCV_RISCV_IRQDELEG                 0x00000534

//
// BCR = Boot Control Register. CORE_SELECT chooses Falcon or RISC-V, and
// VALID reports whether the boot configuration was accepted. This is the
// register that decides which core actually starts.
//
#define NV_PRISCV_RISCV_BCR_CTRL                 0x00000668
#define NV_PRISCV_RISCV_BCR_CTRL_VALID_SHIFT     0       // 0:0
#define NV_PRISCV_RISCV_BCR_CTRL_VALID_MASK      0x01
#define NV_PRISCV_RISCV_BCR_CTRL_VALID_TRUE      0x01
#define NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_SHIFT   4   // 4:4
#define NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_MASK    0x01
#define NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_FALCON  0x00
#define NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_RISCV   0x01
#define NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_SHIFT   8       // 8:8
#define NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_MASK    0x01
#define NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_TRUE    0x01

//
// The exact value the working Linux driver writes to start the GSP RISC-V
// core, observed in mmio-boot.log at t=1.119477 (see PHASE4-GSP-BRINGUP.md
// section 3.5). VALID | CORE_SELECT_RISCV | BRFETCH == 0x111. This is a
// captured fact, not a derivation, and the test asserts the three fields
// really do compose to it -- if a shift above is ever mistyped, that check
// fails immediately rather than at bringup time on hardware.
//
#define NV_PRISCV_RISCV_BCR_CTRL_OBSERVED_START  0x00000111


// ---------------------------------------------------------------------------
// Boot ROM parameter block -- dev_falcon_second_pri.h, relative to the
// engine's FALCON2 base (NV_FALCON2_GSP_BASE for GSP, NV_FALCON2_SEC_BASE
// for SEC2).
//
// These four registers are written immediately before BCR_CTRL and they are
// ORDER-SENSITIVE: MOD_SEL is the latch. Writing it before the other three
// commits stale parameters and the boot ROM rejects the signature with no
// diagnostic beyond a core that never leaves halt. The observed order in
// mmio-boot.log is PARAADDR, ENGIDMASK, CURR_UCODE_ID, MOD_SEL.
// ---------------------------------------------------------------------------

#define NV_PFALCON2_FALCON_MOD_SEL                   0x00000180
#define NV_PFALCON2_FALCON_MOD_SEL_ALGO_SHIFT        0       // 7:0
#define NV_PFALCON2_FALCON_MOD_SEL_ALGO_MASK         0xFF
#define NV_PFALCON2_FALCON_MOD_SEL_ALGO_RSA3K        0x01

#define NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID        0x00000198
#define NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID_SHIFT  0       // 7:0
#define NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID_MASK   0xFF

#define NV_PFALCON2_FALCON_BROM_ENGIDMASK            0x0000019C

#define NV_PFALCON2_FALCON_BROM_PARAADDR(i)          (0x00000210 + (i) * 4)
#define NV_PFALCON2_FALCON_BROM_PARAADDR_SIZE_1      4

//
// Values captured from the working driver's GSP boot (section 3.4 of
// PHASE4-GSP-BRINGUP.md). ENGIDMASK 0x400 is bit 10; UCODE_ID 9 identifies
// the GSP booter. Both are checked by the boot ROM against the firmware
// signature, so they are fixed inputs rather than tunables.
//
#define NV_PFALCON2_GSP_OBSERVED_ENGIDMASK           0x00000400
#define NV_PFALCON2_GSP_OBSERVED_UCODE_ID            0x00000009


// ---------------------------------------------------------------------------
// FBIF -- dev_fbif_v4.h. The header spells these relative to the FBIF block,
// which itself sits at engine-relative 0x600.
//
// The trace pins the base down: SEC2 writes 0x840600 and 0x840624, and
// 0x840624 - 0x840600 == 0x24 == NV_PFALCON_FBIF_CTL's offset within FBIF.
// Two registers, one known stride, one consistent answer -- FBIF is at +0x600.
//
// This block is what makes a physical-address DMA legal. Without it the
// falcon requires a context DMA, which a kext with no framebuffer allocator
// cannot construct. It is therefore the most directly reusable part of the
// SEC2 sequence.
// ---------------------------------------------------------------------------

#define NV_PFALCON_FBIF_BASE                     0x00000600

#define NV_PFALCON_FBIF_TRANSCFG(i)              (NV_PFALCON_FBIF_BASE + (i) * 4)
#define NV_PFALCON_FBIF_TRANSCFG_SIZE_1          8
#define NV_PFALCON_FBIF_TRANSCFG_TARGET_SHIFT    0       // 1:0
#define NV_PFALCON_FBIF_TRANSCFG_TARGET_MASK     0x03
#define NV_PFALCON_FBIF_TRANSCFG_TARGET_LOCAL_FB 0x00
#define NV_PFALCON_FBIF_TRANSCFG_TARGET_COHERENT_SYSMEM 0x01
#define NV_PFALCON_FBIF_TRANSCFG_MEM_TYPE_SHIFT  2       // 2:2
#define NV_PFALCON_FBIF_TRANSCFG_MEM_TYPE_MASK   0x01
#define NV_PFALCON_FBIF_TRANSCFG_MEM_TYPE_PHYSICAL 0x01

#define NV_PFALCON_FBIF_CTL                      (NV_PFALCON_FBIF_BASE + 0x24)
#define NV_PFALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX_SHIFT 7    // 7:7
#define NV_PFALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX_MASK  0x01
#define NV_PFALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX_ALLOW 0x01

//
// Values SEC2 was observed writing at t=1.119686 / t=1.119691:
//
//   TRANSCFG(0) <- 0x115   TARGET=COHERENT_SYSMEM, MEM_TYPE=PHYSICAL
//   CTL         <- 0x190   ALLOW_PHYS_NO_CTX=ALLOW (bit 7)
//
// Both are recorded so the decoders can be tested against real values rather
// than against values this project invented.
//
#define NV_PFALCON_FBIF_OBSERVED_TRANSCFG0       0x00000115
#define NV_PFALCON_FBIF_OBSERVED_CTL             0x00000190

static inline bool nvFbifTargetsSysmem(NVU32 transcfg)
{
    return ((transcfg >> NV_PFALCON_FBIF_TRANSCFG_TARGET_SHIFT)
            & NV_PFALCON_FBIF_TRANSCFG_TARGET_MASK)
           == NV_PFALCON_FBIF_TRANSCFG_TARGET_COHERENT_SYSMEM;
}

static inline bool nvFbifIsPhysical(NVU32 transcfg)
{
    return ((transcfg >> NV_PFALCON_FBIF_TRANSCFG_MEM_TYPE_SHIFT)
            & NV_PFALCON_FBIF_TRANSCFG_MEM_TYPE_MASK)
           == NV_PFALCON_FBIF_TRANSCFG_MEM_TYPE_PHYSICAL;
}

static inline bool nvFbifAllowsPhysNoCtx(NVU32 fbifctl)
{
    return ((fbifctl >> NV_PFALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX_SHIFT)
            & NV_PFALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX_MASK) != 0;
}


// ---------------------------------------------------------------------------
// GSP engine -- dev_gsp.h. These are ABSOLUTE BAR0 offsets already.
// ---------------------------------------------------------------------------

#define NV_PGSP_FALCON_MAILBOX0                  0x00110040
#define NV_PGSP_FALCON_MAILBOX1                  0x00110044

//
// Engine-level reset, distinct from the Falcon CPUCTL. This is the big hammer
// that returns GSP to a known state, and Phase 4 must assert and release it
// before any firmware push.
//
//
// FALCON_ENGINE is spelled absolutely by dev_gsp.h, but the same register
// exists on every falcon at engine-relative 0x3C0 -- dev_sec_pri.h:28 gives
// NV_PSEC_FALCON_ENGINE as 0x008403C0, which is NV_PSEC_BASE + 0x3C0. The
// relative form is what the SEC2 accessor needs; the test asserts the two
// spellings agree for GSP, so the relative constant cannot drift.
//
#define NV_PFALCON_FALCON_ENGINE                 0x000003C0

#define NV_PGSP_FALCON_ENGINE                    0x001103C0
#define NV_PGSP_FALCON_ENGINE_RESET_SHIFT        0       // 0:0
#define NV_PGSP_FALCON_ENGINE_RESET_MASK         0x01
#define NV_PGSP_FALCON_ENGINE_RESET_TRUE         0x01
#define NV_PGSP_FALCON_ENGINE_RESET_FALSE        0x00

#define NV_PGSP_MAILBOX(i)                       (0x00110804 + (i) * 4)
#define NV_PGSP_MAILBOX_SIZE_1                   4

//
// Note the 8-byte stride -- each queue head is a 64-bit slot even though only
// the low 32 bits are defined as ADDRESS. Using a 4-byte stride here would
// alias every other queue onto the wrong one.
//
#define NV_PGSP_QUEUE_HEAD(i)                    (0x00110C00 + (i) * 8)
#define NV_PGSP_QUEUE_HEAD_SIZE_1                8


// ---------------------------------------------------------------------------
// WPR2 -- write-protected region 2, dev_fb.h.
//
// NVIDIA ships no dev_fb.h under ampere/ga102. The definitions below come from
// turing/tu102/dev_fb.h, corroborated by hopper/gh100/dev_fb.h -- two headers
// from different architectures agreeing on the same offsets, which is the same
// kind of independent confirmation used to prove NV_PGSP_BASE. GA10x is safe
// to treat as TU102 here because NVIDIA's own HAL routes GA10x GSP bootstrap
// to the _TU102 implementation (kernel_gsp_tu102.c).
//
// Why Phase 4 cares: kgspPrepareForBootstrap_TU102() calls kgspIsWpr2Up_HAL()
// BEFORE pushing any firmware, and refuses to proceed if WPR2 is already up.
// A raised WPR2 means a previous boot left the GPU holding a protected region,
// and the only way forward is a full engine reset. Reading it costs nothing
// and it is a genuine precondition, not a nicety.
//
// ADDR_HI reads ZERO when WPR2 is down. That is the entire test --
// kgspIsWpr2Up_TU102() is just DRF_VAL(_VAL, ADDR_HI) != 0.
// ---------------------------------------------------------------------------

#define NV_PFB_PRI_MMU_WPR2_ADDR_LO              0x001FA824
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO_VAL_SHIFT    4       // 31:4
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO_VAL_MASK     0x0FFFFFFF

#define NV_PFB_PRI_MMU_WPR2_ADDR_HI              0x001FA828
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL_SHIFT    4       // 31:4
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL_MASK     0x0FFFFFFF

//
// dev_fb.h gives ALIGNMENT as 0x0C for both halves: the stored VAL is the
// physical address shifted right by 12, i.e. a 4 KiB page number. Forgetting
// this shift makes a 2 GiB WPR base look like 512 KiB.
//
#define NV_PFB_PRI_MMU_WPR2_ALIGNMENT_SHIFT      12


// ===========================================================================
// Address translation. Pure arithmetic, no hardware.
// ===========================================================================

//
// Convert an engine-relative Falcon offset (dev_falcon_v4.h) to an absolute
// BAR0 offset for the GSP engine.
//
static inline NVU32 nvGspFalconReg(NVU32 falconRelative)
{
    return NV_PGSP_BASE + falconRelative;
}

//
// Convert a RISC-V offset (dev_riscv_pri.h) to absolute. Note this uses
// NV_FALCON2_GSP_BASE (0x111000), one page ABOVE the Falcon block -- see the
// address-space commentary at the top of this file.
//
static inline NVU32 nvGspRiscvReg(NVU32 riscvRelative)
{
    return NV_FALCON2_GSP_BASE + riscvRelative;
}

//
// Every absolute GSP access must land inside NV_PGSP. A translation bug that
// escapes the aperture writes into a neighbouring engine, which is far worse
// than faulting.
//
static inline bool nvIsInGspAperture(NVU32 absolute)
{
    return (absolute >= NV_PGSP_BASE) && (absolute <= NV_PGSP_LIMIT);
}

//
// SEC2 equivalents. Separate functions rather than a base parameter, for the
// same reason nvGspFalconReg() exists: a single translate(base, offset) helper
// makes it possible to pass the GSP base with a SEC2 offset and get a valid
// address into the wrong engine, which is exactly the failure mode this file
// is structured to prevent.
//
static inline NVU32 nvSecFalconReg(NVU32 falconRelative)
{
    return NV_PSEC_BASE + falconRelative;
}

static inline NVU32 nvSecRiscvReg(NVU32 riscvRelative)
{
    return NV_FALCON2_SEC_BASE + riscvRelative;
}

static inline bool nvIsInSecAperture(NVU32 absolute)
{
    return (absolute >= NV_PSEC_BASE) && (absolute <= NV_PSEC_LIMIT);
}

//
// The two apertures must not overlap. Cheap to assert, and the whole reason
// the psec region was misfiled for so long was an address-range assumption
// nobody checked against the headers.
//
static inline bool nvGspAndSecDisjoint(void)
{
    return (NV_PGSP_LIMIT < NV_PSEC_BASE) || (NV_PSEC_LIMIT < NV_PGSP_BASE);
}

static inline bool nvIsInBar0(NVU32 absolute)
{
    return absolute < (NVU32)NV_BAR0_EXPECTED_LENGTH;
}


// ===========================================================================
// Field decoding. Pure functions of a register value.
// ===========================================================================

static inline bool nvFalconIsRiscvCapable(NVU32 hwcfg2)
{
    return ((hwcfg2 >> NV_PFALCON_FALCON_HWCFG2_RISCV_SHIFT)
            & NV_PFALCON_FALCON_HWCFG2_RISCV_MASK)
           == NV_PFALCON_FALCON_HWCFG2_RISCV_ENABLE;
}

//
// MEM_SCRUBBING reads 0 when scrubbing is DONE. Inverted sense, and getting it
// backwards means waiting forever on a Falcon that was ready immediately.
//
static inline bool nvFalconScrubbingDone(NVU32 hwcfg2)
{
    return ((hwcfg2 >> NV_PFALCON_FALCON_HWCFG2_MEM_SCRUBBING_SHIFT)
            & NV_PFALCON_FALCON_HWCFG2_MEM_SCRUBBING_MASK)
           == NV_PFALCON_FALCON_HWCFG2_MEM_SCRUBBING_DONE;
}

//
// HWCFG_IMEM_SIZE is expressed in 256-byte blocks, not bytes.
//
static inline NVU32 nvFalconImemSizeBlocks(NVU32 hwcfg)
{
    return (hwcfg >> NV_PFALCON_FALCON_HWCFG_IMEM_SIZE_SHIFT)
           & NV_PFALCON_FALCON_HWCFG_IMEM_SIZE_MASK;
}

static inline NVU32 nvFalconImemSizeBytes(NVU32 hwcfg)
{
    return nvFalconImemSizeBlocks(hwcfg) * 256u;
}

static inline bool nvFalconIsHalted(NVU32 cpuctl)
{
    return ((cpuctl >> NV_PFALCON_FALCON_CPUCTL_HALTED_SHIFT)
            & NV_PFALCON_FALCON_CPUCTL_HALTED_MASK) != 0;
}

static inline bool nvFalconAliasEnabled(NVU32 cpuctl)
{
    return ((cpuctl >> NV_PFALCON_FALCON_CPUCTL_ALIAS_EN_SHIFT)
            & NV_PFALCON_FALCON_CPUCTL_ALIAS_EN_MASK) != 0;
}

static inline bool nvFalconDmaIdle(NVU32 dmatrfcmd)
{
    return ((dmatrfcmd >> NV_PFALCON_FALCON_DMATRFCMD_IDLE_SHIFT)
            & NV_PFALCON_FALCON_DMATRFCMD_IDLE_MASK) != 0;
}

static inline bool nvFalconDmaFull(NVU32 dmatrfcmd)
{
    return ((dmatrfcmd >> NV_PFALCON_FALCON_DMATRFCMD_FULL_SHIFT)
            & NV_PFALCON_FALCON_DMATRFCMD_FULL_MASK) != 0;
}

static inline bool nvRiscvIsActive(NVU32 riscvCpuctl)
{
    return ((riscvCpuctl >> NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT_SHIFT)
            & NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT_MASK)
           == NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT_ACTIVE;
}

static inline bool nvRiscvIsHalted(NVU32 riscvCpuctl)
{
    return ((riscvCpuctl >> NV_PRISCV_RISCV_CPUCTL_HALTED_SHIFT)
            & NV_PRISCV_RISCV_CPUCTL_HALTED_MASK) != 0;
}

static inline bool nvBcrValid(NVU32 bcrCtrl)
{
    return ((bcrCtrl >> NV_PRISCV_RISCV_BCR_CTRL_VALID_SHIFT)
            & NV_PRISCV_RISCV_BCR_CTRL_VALID_MASK)
           == NV_PRISCV_RISCV_BCR_CTRL_VALID_TRUE;
}

static inline NVU32 nvBcrCoreSelect(NVU32 bcrCtrl)
{
    return (bcrCtrl >> NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_SHIFT)
           & NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_MASK;
}

static inline bool nvBcrSelectsRiscv(NVU32 bcrCtrl)
{
    return nvBcrCoreSelect(bcrCtrl) == NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_RISCV;
}

//
// kgspIsWpr2Up_TU102(), kernel_gsp_tu102.c:1171. The entire implementation is
// "is the ADDR_HI VAL field nonzero", so this is a faithful port rather than a
// simplification.
//
static inline bool nvWpr2IsUp(NVU32 wpr2AddrHi)
{
    return ((wpr2AddrHi >> NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL_SHIFT)
            & NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL_MASK) != 0;
}

//
// Recover the physical address a WPR2 half describes. VAL is a 4 KiB page
// number, so the shift back up is 12 -- not the 4 used to extract the field.
// Returned as 64-bit because a 28-bit page number covers 40 bits of address
// and would overflow a NVU32.
//
static inline unsigned long long nvWpr2Address(NVU32 wpr2Half,
                                               NVU32 valShift,
                                               NVU32 valMask)
{
    unsigned long long page = (unsigned long long)((wpr2Half >> valShift) & valMask);
    return page << NV_PFB_PRI_MMU_WPR2_ALIGNMENT_SHIFT;
}

static inline unsigned long long nvWpr2AddressLo(NVU32 wpr2AddrLo)
{
    return nvWpr2Address(wpr2AddrLo,
                         NV_PFB_PRI_MMU_WPR2_ADDR_LO_VAL_SHIFT,
                         NV_PFB_PRI_MMU_WPR2_ADDR_LO_VAL_MASK);
}

static inline unsigned long long nvWpr2AddressHi(NVU32 wpr2AddrHi)
{
    return nvWpr2Address(wpr2AddrHi,
                         NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL_SHIFT,
                         NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL_MASK);
}

//
// WPR2 read verdict. THREE outcomes, not two.
//
// checkWpr2() in the IOKit shell returned a bare bool meaning "is the
// precondition satisfied", and it returned FALSE for two categorically
// different reasons: WPR2 is genuinely UP, or the registers did not decode at
// all. Those are not the same fact about the machine. "A previous boot left a
// protected region, reset the engine" is something an operator can act on;
// "BAR0 is not mapped, or these turing/tu102 offsets are wrong for GA107" is a
// bug in THIS driver. Collapsing them publishes the first as an answer to the
// second, and only the second is a thing somebody can go and fix.
//
// This judges the read GROUP rather than each word, for the reason handoff24
// gives one track over: nvWpr2AddressLo() and nvWpr2AddressHi() are decoded
// together and reported as ONE range, so a per-word flag would publish one
// good half, one sentinel, and a range arithmetically correct over a word that
// was never measured.
//
#define NVWPR2_DOWN          0u   /* both halves read; VAL is zero        */
#define NVWPR2_UP            1u   /* both halves read; VAL is nonzero     */
#define NVWPR2_UNREADABLE    2u   /* at least one half did not decode     */
#define NVWPR2_VERDICT_LIMIT 3u

//
// regRead32() reports every refusal it has -- BAR0 unmapped, offset past the
// end of the mapping -- by returning all-ones, and so does a PCI read that
// nothing answers.
//
// The honest ambiguity, written down rather than fixed: a WPR2 half that
// GENUINELY holds 0xFFFFFFFF is reported UNREADABLE. That is the safe
// direction and it is not a new judgement -- all-ones in ADDR_HI decodes to
// VAL 0x0FFFFFFF, the maximum page number, so believing it would publish a
// WPR2 based at the top of a 40-bit address space as though it had been
// measured. The shipping shell has treated that value as not-decoding since
// the first version of checkWpr2(); this only extends the same test to
// ADDR_LO, which had none.
//
#define NVWPR2_READ_REFUSED_VALUE 0xFFFFFFFFu

static inline bool nvWpr2ReadRefused(NVU32 wpr2Half)
{
    return wpr2Half == NVWPR2_READ_REFUSED_VALUE;
}

//
// The join. BOTH halves are tested, which the bool version did not do: it
// checked ADDR_HI only, so a refused ADDR_LO went straight into
// nvWpr2AddressLo() and composed the bottom of a reported range.
//
static inline NVU32 nvWpr2Verdict(NVU32 wpr2AddrLo, NVU32 wpr2AddrHi)
{
    if (nvWpr2ReadRefused(wpr2AddrLo) || nvWpr2ReadRefused(wpr2AddrHi))
        return NVWPR2_UNREADABLE;

    return nvWpr2IsUp(wpr2AddrHi) ? NVWPR2_UP : NVWPR2_DOWN;
}

//
// A name per verdict, for the IORegistry and the log. The exhaustiveness loop
// in tests/test_falcon.cpp bounds on NVWPR2_VERDICT_LIMIT rather than on a
// typed count, so a family that GROWS reddens on the commit that grows it --
// handoff20 s1, where igModeVerdictName() lost an arm to `default:` and all
// four gates stayed green over it.
//
static inline const char *nvWpr2VerdictName(NVU32 verdict)
{
    switch (verdict) {
    case NVWPR2_DOWN:       return "down";
    case NVWPR2_UP:         return "up";
    case NVWPR2_UNREADABLE: return "unreadable";
    default:                return "unrecognised WPR2 verdict";
    }
}

static inline bool nvGspEngineInReset(NVU32 engineReg)
{
    return ((engineReg >> NV_PGSP_FALCON_ENGINE_RESET_SHIFT)
            & NV_PGSP_FALCON_ENGINE_RESET_MASK)
           == NV_PGSP_FALCON_ENGINE_RESET_TRUE;
}

//
// The RISC-V error-latch verdict. FIVE outcomes, not a bool.
//
// The bool this replaces was `(privErrStat != 0) || (hubErrStat != 0)`, and its
// own comment said "a hub error and a priv error have different causes and both
// must be surfaced" -- while returning one word that surfaced neither. Four
// structurally different situations collapsed onto `true`:
//
//   * a priv error alone   -- an access this driver made was refused by the
//                             engine's privilege check
//   * a hub error alone    -- the memory hub rejected a transaction
//   * both latches set     -- usually one fault cascading
//   * NEITHER register decoded -- and this one is not an error at all
//
// The last is the reason this is a verdict rather than a tidier bool.
// regRead32() reports every refusal it has by returning all-ones, so a BAR0
// mapping that covers the Falcon block but stops before the RISC-V block one
// 4 KiB page above it makes BOTH latches read 0xFFFFFFFF -- and the old bool
// answered TRUE. An operator was told the engine had latched a fault when what
// actually happened is that this driver read past the end of its own mapping.
// Those call for opposite actions: reset the engine, versus fix this kext.
//
// Same class as the WPR2 verdict (3050-handoff26 s1) one register block over,
// and the same reason handoff22 s1b puts the driver's own bug ahead of a fact
// about the machine: the bug is the one somebody can go and fix.
//
#define NVRISCV_ERR_NONE          0u  /* both decoded; both latches clear   */
#define NVRISCV_ERR_PRIV          1u  /* both decoded; PRIV_ERR_STAT set    */
#define NVRISCV_ERR_HUB           2u  /* both decoded; HUB_ERR_STAT set     */
#define NVRISCV_ERR_BOTH          3u  /* both decoded; both latches set     */
#define NVRISCV_ERR_UNREADABLE    4u  /* at least one half did not decode   */
#define NVRISCV_ERR_VERDICT_LIMIT 5u

//
// The refusal sentinel, spelled for THIS family rather than borrowed from the
// WPR2 one. Both land on all-ones, but by different roads, and a sentinel taken
// from a sibling family five hundred lines away is handoff21 s1b's mistake --
// the two can diverge and this comparison would not know.
//
#define NVRISCV_ERR_READ_REFUSED_VALUE 0xFFFFFFFFu

//
// An equality, never a threshold. Every bit of an error-status register is a
// distinct latch, so there is no "too large to be real" reading to threshold
// against -- only the one exact value regRead32() manufactures on refusal.
//
// nvRiscvErrReadRefused(0u) is deliberately FALSE: zero is the HEALTHY case for
// both of these registers, which is the opposite polarity to the WPR2 family,
// where zero in ADDR_HI is the healthy DOWN reading and all-ones is refusal.
// Two families, same sentinel, arrived at from opposite directions.
//
static inline bool nvRiscvErrReadRefused(NVU32 errStat)
{
    return errStat == NVRISCV_ERR_READ_REFUSED_VALUE;
}

//
// The honest ambiguity, written down rather than fixed: a latch GENUINELY
// holding 0xFFFFFFFF is reported UNREADABLE.
//
// That is the safe direction and it is not a close call. All-ones means every
// error type in the register latched at once, on a card this driver has never
// written to; UNREADABLE says "go and find out", while naming a specific fault
// would invent a diagnosis. Fixing it properly needs the mapping length, which
// would couple this dependency-free header to the shell's BAR state -- the same
// judgement igSampleVerdict() makes on the Intel track (handoff24 s1a).
//
// UNREADABLE takes PRECEDENCE over a genuine latch in the other half. The
// consumer is a go/no-go, and "one of the only two diagnostics GSP has did not
// decode" is itself the finding: nothing is lost, because both words are
// published beside this verdict either way.
//
static inline NVU32 nvRiscvErrorVerdict(NVU32 privErrStat, NVU32 hubErrStat)
{
    if (nvRiscvErrReadRefused(privErrStat)
        || nvRiscvErrReadRefused(hubErrStat)) {
        return NVRISCV_ERR_UNREADABLE;
    }

    if (privErrStat != 0 && hubErrStat != 0)
        return NVRISCV_ERR_BOTH;
    if (privErrStat != 0)
        return NVRISCV_ERR_PRIV;
    if (hubErrStat != 0)
        return NVRISCV_ERR_HUB;

    return NVRISCV_ERR_NONE;
}

//
// Bounded on the family's own _VERDICT_LIMIT rather than a typed count, so a
// family that GROWS reddens on the commit that grows it -- handoff20 s1, where
// igModeVerdictName() lost an arm to `default:` and all four gates stayed green.
//
static inline const char *nvRiscvErrorVerdictName(NVU32 verdict)
{
    switch (verdict) {
    case NVRISCV_ERR_NONE:       return "latches clear";
    case NVRISCV_ERR_PRIV:       return "priv error latched";
    case NVRISCV_ERR_HUB:        return "hub error latched";
    case NVRISCV_ERR_BOTH:       return "priv and hub errors latched";
    case NVRISCV_ERR_UNREADABLE: return "error latches unreadable";
    default:                     return "unrecognised RISC-V error verdict";
    }
}

//
// Any nonzero error latch is a real fault. KEPT as a one-line wrapper over the
// verdict rather than deleted, so every existing caller and every existing
// assertion keeps working and this refactor cannot be the thing that changes a
// go/no-go decision (3050-handoff32 s3's reasoning, one subsystem over).
//
// A NEGATION of the single OK arm, never a disjunction over the three fault
// arms. `verdict == PRIV || verdict == HUB || verdict == BOTH` is FALSE for
// UNREADABLE, so it would report a refused read as "no fault" and send an
// unmeasurable precondition through the go/no-go as satisfied. `!= NONE` has
// exactly one preimage on its false side, which is the whole point.
//
// It also preserves the old bool's behaviour on all-ones EXACTLY -- true, so
// the boot is still refused. What changes is that somebody reading the registry
// can now tell WHY.
//
static inline bool nvRiscvHasError(NVU32 privErrStat, NVU32 hubErrStat)
{
    return nvRiscvErrorVerdict(privErrStat, hubErrStat) != NVRISCV_ERR_NONE;
}

#endif /* NVFalcon_hpp */
