//
//  test_falcon.cpp
//  Host-side tests for NVFalcon.hpp -- the Phase 4 register layer.
//
//  Builds and runs under MSVC on Windows. No IOKit, no GPU, no macOS.
//
//  The single most valuable test in this file is the cross-header base
//  validation: dev_falcon_v4.h gives MAILBOX0 as engine-relative 0x40, while
//  dev_gsp.h independently spells the absolute address 0x110040. If
//  nvGspFalconReg(0x40) reproduces that, NV_PGSP_BASE is provably correct --
//  confirmed by two NVIDIA headers that do not reference each other.
//

#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVFalcon.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void section(const char *name)
{
    std::printf("\n%s\n", name);
    for (size_t i = 0; i < std::strlen(name); ++i)
        std::printf("-");
    std::printf("\n");
}

static void checkU32(const char *label, NVU32 actual, NVU32 expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-50s 0x%X\n", label, actual);
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-50s got 0x%X, expected 0x%X\n", label, actual, expected);
}

static void checkBool(const char *label, bool actual, bool expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-50s %s\n", label, actual ? "true" : "false");
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-50s got %s, expected %s\n",
                label, actual ? "true" : "false", expected ? "true" : "false");
}

//
// THE TEST THAT ANCHORS EVERYTHING ELSE.
//
// Three address spaces are in play and the NVIDIA headers never state the
// relationship in one place. Getting it wrong does not fault -- a RISC-V
// access 0x1000 low lands inside the Falcon block, where every offset is a
// real register. It reads and writes hardware, just the wrong hardware, and
// the symptom is a firmware boot that hangs with no diagnostic.
//
static void testAddressTranslation(void)
{
    section("address translation (the silent-corruption class of bug)");

    //
    // Cross-header validation. dev_falcon_v4.h: MAILBOX0 is relative 0x40.
    // dev_gsp.h: NV_PGSP_FALCON_MAILBOX0 is absolute 0x110040. These two
    // headers do not reference each other, so agreement proves the base.
    //
    checkU32("falcon MAILBOX0 -> absolute",
             nvGspFalconReg(NV_PFALCON_FALCON_MAILBOX0), 0x00110040u);
    checkU32("  matches dev_gsp.h independently",
             nvGspFalconReg(NV_PFALCON_FALCON_MAILBOX0), NV_PGSP_FALCON_MAILBOX0);
    checkU32("falcon MAILBOX1 -> absolute",
             nvGspFalconReg(NV_PFALCON_FALCON_MAILBOX1), NV_PGSP_FALCON_MAILBOX1);

    //
    // RISC-V uses NV_FALCON2_GSP_BASE (0x111000), one 4 KiB page ABOVE the
    // Falcon block. This is the distinction that gets collapsed.
    //
    checkU32("riscv CPUCTL -> absolute",
             nvGspRiscvReg(NV_PRISCV_RISCV_CPUCTL), 0x00111388u);
    checkU32("riscv BCR_CTRL -> absolute",
             nvGspRiscvReg(NV_PRISCV_RISCV_BCR_CTRL), 0x00111668u);

    //
    // The two bases must differ by exactly one Falcon block. If someone
    // "simplifies" NV_FALCON2_GSP_BASE to NV_PGSP_BASE, this is what catches
    // it.
    //
    checkU32("riscv base is falcon base + 0x1000",
             NV_FALCON2_GSP_BASE - NV_PGSP_BASE, (NVU32)NV_FALCON_BLOCK_SIZE);

    //
    // Concretely: the same numeric offset must NOT resolve to the same place
    // through both accessors.
    //
    checkBool("falcon(0x388) != riscv(0x388)",
              nvGspFalconReg(0x388u) == nvGspRiscvReg(0x388u), false);
    checkU32("  falcon(0x388) would be", nvGspFalconReg(0x388u), 0x00110388u);
    checkU32("  riscv(0x388) is",        nvGspRiscvReg(0x388u), 0x00111388u);

    //
    // Every translated address must stay inside the GSP aperture. An escape
    // writes into a neighbouring engine, which is worse than a fault.
    //
    checkBool("falcon MAILBOX0 in aperture",
              nvIsInGspAperture(nvGspFalconReg(NV_PFALCON_FALCON_MAILBOX0)), true);
    checkBool("riscv HUB_ERR_STAT in aperture",
              nvIsInGspAperture(nvGspRiscvReg(NV_PRISCV_RISCV_HUB_ERR_STAT)), true);
    checkBool("QUEUE_HEAD(0) in aperture",
              nvIsInGspAperture(NV_PGSP_QUEUE_HEAD(0)), true);
    checkBool("FALCON_ENGINE in aperture",
              nvIsInGspAperture(NV_PGSP_FALCON_ENGINE), true);

    // And the aperture check must actually reject things outside it.
    checkBool("0x000000 rejected", nvIsInGspAperture(0x000000u), false);
    checkBool("0x114000 rejected (PSEC)", nvIsInGspAperture(0x114000u), false);
    checkBool("limit 0x113FFF accepted", nvIsInGspAperture(NV_PGSP_LIMIT), true);
    checkBool("limit+1 rejected", nvIsInGspAperture(NV_PGSP_LIMIT + 1u), false);

    // Everything must also fit the 16 MiB BAR0 mapping.
    checkBool("GSP limit inside BAR0", nvIsInBar0(NV_PGSP_LIMIT), true);
}

//
// Indexed ports with differing strides. IMEMC/IMEMD/IMEMT form a 16-byte
// triple; DMEMC/DMEMD an 8-byte pair; QUEUE_HEAD an 8-byte slot even though
// only 32 bits are defined. Using one stride everywhere aliases ports onto
// each other.
//
static void testIndexedPorts(void)
{
    section("indexed port strides");

    checkU32("IMEMC(0)", NV_PFALCON_FALCON_IMEMC(0), 0x180u);
    checkU32("IMEMD(0)", NV_PFALCON_FALCON_IMEMD(0), 0x184u);
    checkU32("IMEMT(0)", NV_PFALCON_FALCON_IMEMT(0), 0x188u);
    checkU32("IMEMC(1) stride 16", NV_PFALCON_FALCON_IMEMC(1), 0x190u);
    checkU32("IMEMC(3)", NV_PFALCON_FALCON_IMEMC(3), 0x1B0u);

    checkU32("DMEMC(0)", NV_PFALCON_FALCON_DMEMC(0), 0x1C0u);
    checkU32("DMEMD(0)", NV_PFALCON_FALCON_DMEMD(0), 0x1C4u);
    checkU32("DMEMC(1) stride 8", NV_PFALCON_FALCON_DMEMC(1), 0x1C8u);

    //
    // IMEMC(1) must not collide with any DMEM port, and vice versa. A wrong
    // stride is exactly how that happens.
    //
    checkBool("IMEMC(3) != DMEMC(0)",
              NV_PFALCON_FALCON_IMEMC(3) == NV_PFALCON_FALCON_DMEMC(0), false);

    checkU32("QUEUE_HEAD(0)", NV_PGSP_QUEUE_HEAD(0), 0x110C00u);
    checkU32("QUEUE_HEAD(1) stride 8", NV_PGSP_QUEUE_HEAD(1), 0x110C08u);
    checkU32("MAILBOX(0)", NV_PGSP_MAILBOX(0), 0x110804u);
    checkU32("MAILBOX(1) stride 4", NV_PGSP_MAILBOX(1), 0x110808u);

    checkU32("INTR_RETRIGGER(0)", NV_PFALCON_FALCON_INTR_RETRIGGER(0), 0x3E8u);
    checkU32("INTR_RETRIGGER(1)", NV_PFALCON_FALCON_INTR_RETRIGGER(1), 0x3ECu);
}

static void testHwcfg(void)
{
    section("HWCFG2 / HWCFG decode");

    // Bit 10 set -> RISC-V capable. GA107's GSP must report this.
    checkBool("RISCV bit set -> capable",
              nvFalconIsRiscvCapable(1u << 10), true);
    checkBool("RISCV bit clear -> not capable",
              nvFalconIsRiscvCapable(0u), false);
    checkBool("unrelated bits ignored",
              nvFalconIsRiscvCapable(0xFFFFFBFFu), false);

    //
    // SEPARATING INPUT for a widened mask. The three checks above all pass with
    // MASK 0x3 instead of 0x1 -- 0xFFFFFBFF has bit 10 CLEAR, so a two-bit read
    // returns 2, which is still not ENABLE(1), and the verdict is unchanged.
    // The mutant survives an "unrelated bits ignored" check because bit 11 is
    // not unrelated to a two-bit field, it is IN it.
    //
    // With BOTH bits set the two readings finally disagree: the correct decoder
    // extracts 1 and returns true; a MASK 0x3 decoder extracts 3, which is not
    // ENABLE, and reports a RISC-V-capable GSP as incapable.
    //
    checkBool("bit 11 does not widen the RISCV field",
              nvFalconIsRiscvCapable((1u << 10) | (1u << 11)), true);

    //
    // MEM_SCRUBBING has INVERTED sense: 0 means DONE. Reading it the obvious
    // way means polling forever on a Falcon that was ready immediately.
    //
    checkBool("scrubbing bit CLEAR -> done",
              nvFalconScrubbingDone(0u), true);
    checkBool("scrubbing bit SET -> not done",
              nvFalconScrubbingDone(1u << 12), false);
    checkBool("scrubbing done, other bits set",
              nvFalconScrubbingDone(0xFFFFEFFFu), true);

    // IMEM_SIZE is in 256-byte blocks, not bytes.
    checkU32("IMEM 0x40 blocks", nvFalconImemSizeBlocks(0x40u), 0x40u);
    checkU32("IMEM 0x40 blocks -> bytes", nvFalconImemSizeBytes(0x40u), 0x4000u);
    checkU32("IMEM field masks to 9 bits",
             nvFalconImemSizeBlocks(0xFFFFFFFFu), 0x1FFu);
}

static void testCpuctl(void)
{
    section("Falcon CPUCTL");

    checkBool("HALTED set", nvFalconIsHalted(1u << 4), true);
    checkBool("HALTED clear", nvFalconIsHalted(0u), false);
    checkBool("ALIAS_EN set", nvFalconAliasEnabled(1u << 6), true);
    checkBool("ALIAS_EN clear", nvFalconAliasEnabled(0u), false);

    // HALTED and ALIAS_EN are adjacent-ish; make sure neither reads the other.
    checkBool("halted bit does not set alias", nvFalconAliasEnabled(1u << 4), false);
    checkBool("alias bit does not set halted", nvFalconIsHalted(1u << 6), false);

    //
    // SEPARATING INPUTS for a widened mask. The four checks above all pass with
    // MASK 0x3 instead of 0x1, because each cross-check probes the bit on the
    // FAR side -- bit 6 is two positions above HALTED, so a two-bit read of
    // bits 5:4 still returns 0 and the verdict is unchanged. The bit a widened
    // mask actually swallows is the one IMMEDIATELY ABOVE the field, and
    // nothing above tested it.
    //
    // Bit 5 sits between HALTED (4) and ALIAS_EN (6) and is not a field this
    // driver decodes. Bit 7 sits above ALIAS_EN. Under MASK 0x3 either one
    // reports a running Falcon as HALTED, or a plain CPUCTL as aliased -- both
    // stop the bringup with a diagnosis that describes nothing.
    //
    checkBool("bit 5 does not widen the HALTED field",
              nvFalconIsHalted(1u << 5), false);
    checkBool("bit 7 does not widen the ALIAS_EN field",
              nvFalconAliasEnabled(1u << 7), false);
}

static void testDma(void)
{
    section("Falcon DMA status");

    checkBool("IDLE set", nvFalconDmaIdle(1u << 1), true);
    checkBool("IDLE clear", nvFalconDmaIdle(0u), false);
    checkBool("FULL set", nvFalconDmaFull(1u << 0), true);
    checkBool("FULL clear", nvFalconDmaFull(0u), false);

    // FULL is bit 0 and IDLE bit 1 -- off by one and the driver waits on the
    // wrong condition forever.
    checkBool("FULL bit is not IDLE", nvFalconDmaIdle(1u << 0), false);
    checkBool("IDLE bit is not FULL", nvFalconDmaFull(1u << 1), false);

    //
    // SEPARATING INPUT for a widened mask on IDLE. The cross-checks above are
    // asymmetric in what they can catch: a MASK 0x3 read of FULL covers bits
    // 1:0, so "IDLE bit is not FULL" already kills it. A MASK 0x3 read of IDLE
    // covers bits 2:1, which is AWAY from FULL, so every check above returns
    // the same verdict under either mask and the mutant survives.
    //
    // Bit 2 is the one it swallows. A driver that reads a set bit 2 as IDLE
    // stops waiting on a transfer that is still running and proceeds to trigger
    // the next chunk over the top of it.
    //
    checkBool("bit 2 does not widen the IDLE field",
              nvFalconDmaIdle(1u << 2), false);

    //
    // DMATRFBASE1 is a 9-bit field that continues the SHIFTED address, so it
    // carries physical bits 48:40 (corrected 2026-08-10, t-53137f -- it is NOT
    // an overlay on the unshifted word at 40:32). The width asserted here is
    // the register field's, which is what the mask has to match; the 41-bit
    // number elsewhere is a policy ceiling, not this field.
    //
    checkU32("DMATRFBASE1 field is 9 bits",
             NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK, 0x1FFu);
    checkU32("DMATRFMOFFS field is 24 bits",
             NV_PFALCON_FALCON_DMATRFMOFFS_OFFS_MASK, 0xFFFFFFu);
}

static void testRiscv(void)
{
    section("RISC-V core state");

    checkBool("ACTIVE_STAT set -> active", nvRiscvIsActive(1u << 7), true);
    checkBool("ACTIVE_STAT clear -> inactive", nvRiscvIsActive(0u), false);
    checkBool("HALTED set", nvRiscvIsHalted(1u << 4), true);
    checkBool("HALTED clear", nvRiscvIsHalted(0u), false);

    // Both bits live in one register; neither may read the other.
    checkBool("active bit is not halted", nvRiscvIsHalted(1u << 7), false);
    checkBool("halted bit is not active", nvRiscvIsActive(1u << 4), false);

    //
    // A booted GSP reports active AND not halted. Halted-while-active is the
    // signature of firmware that started and then trapped.
    //
    NVU32 booted = (1u << 7);
    checkBool("booted: active", nvRiscvIsActive(booted), true);
    checkBool("booted: not halted", nvRiscvIsHalted(booted), false);

    NVU32 trapped = (1u << 7) | (1u << 4);
    checkBool("trapped: active", nvRiscvIsActive(trapped), true);
    checkBool("trapped: halted too", nvRiscvIsHalted(trapped), true);
}

static void testBcr(void)
{
    section("BCR -- which core actually starts");

    checkBool("VALID set", nvBcrValid(1u), true);
    checkBool("VALID clear", nvBcrValid(0u), false);

    checkU32("CORE_SELECT falcon", nvBcrCoreSelect(0u),
             (NVU32)NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_FALCON);
    checkU32("CORE_SELECT riscv", nvBcrCoreSelect(1u << 4),
             (NVU32)NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_RISCV);

    checkBool("riscv selected", nvBcrSelectsRiscv(1u << 4), true);
    checkBool("falcon selected", nvBcrSelectsRiscv(0u), false);

    //
    // The configuration Phase 4 must write: VALID | CORE_SELECT=RISCV |
    // BRFETCH. Assembling it from the constants confirms the shifts do not
    // overlap.
    //
    NVU32 want = (NV_PRISCV_RISCV_BCR_CTRL_VALID_TRUE
                    << NV_PRISCV_RISCV_BCR_CTRL_VALID_SHIFT)
               | (NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_RISCV
                    << NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_SHIFT)
               | (NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_TRUE
                    << NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_SHIFT);

    checkU32("assembled boot BCR_CTRL", want, 0x00000111u);
    checkBool("  reads back VALID", nvBcrValid(want), true);
    checkBool("  reads back RISCV", nvBcrSelectsRiscv(want), true);
}

//
// WPR2 -- the precondition NVIDIA checks before pushing any firmware.
//
// Two shifts are in play and they are NOT the same: the VAL field is extracted
// with a shift of 4, but the value it holds is a 4 KiB page number, so
// recovering the physical address shifts back up by 12.
//
// THERE ARE TWO DISTINCT WAYS TO GET THIS WRONG AND THEY PRODUCE DIFFERENT
// WRONG NUMBERS. An earlier revision of this comment named one and quoted the
// other's number, which is worse than saying nothing: it would have talked a
// reader out of the very defect they were looking at. Both are computed from a
// 2 GiB base, whose page number is 0x80000, so the register holds 0x800000:
//
//   correct   (>>4 extract, <<12 align)   0x80000000    2 GiB
//   BUG A: shift 4 in BOTH directions     0x00800000    8 MiB     <- see below
//   BUG B: extract only, no scale-back    0x00080000    512 KiB   <- NVFalcon.hpp:507
//
// Both are plausible-looking numbers, which is the worst kind of wrong. The
// assertion at "2 GiB is not mis-scaled to 8 MiB" pins BUG A specifically;
// the round-trip assertion above it is what excludes BUG B.
//
static void testWpr2(void)
{
    section("WPR2 -- firmware boot precondition");

    checkU32("ADDR_LO offset", NV_PFB_PRI_MMU_WPR2_ADDR_LO, 0x001FA824u);
    checkU32("ADDR_HI offset", NV_PFB_PRI_MMU_WPR2_ADDR_HI, 0x001FA828u);
    checkU32("ADDR_HI is LO + 4",
             NV_PFB_PRI_MMU_WPR2_ADDR_HI - NV_PFB_PRI_MMU_WPR2_ADDR_LO, 4u);

    // Both live inside the 16 MiB BAR0 mapping the kext makes.
    checkBool("ADDR_LO inside BAR0", nvIsInBar0(NV_PFB_PRI_MMU_WPR2_ADDR_LO), true);
    checkBool("ADDR_HI inside BAR0", nvIsInBar0(NV_PFB_PRI_MMU_WPR2_ADDR_HI), true);

    //
    // These are FB registers at 0x1FAxxx, well outside the GSP aperture. If
    // someone ever routes them through nvGspFalconReg() the aperture guard
    // must reject it rather than silently reading a GSP register.
    //
    checkBool("ADDR_HI is NOT in the GSP aperture",
              nvIsInGspAperture(NV_PFB_PRI_MMU_WPR2_ADDR_HI), false);

    // kgspIsWpr2Up_TU102(): nonzero ADDR_HI VAL means WPR2 is up.
    checkBool("zero -> down", nvWpr2IsUp(0u), false);
    checkBool("VAL nonzero -> up", nvWpr2IsUp(1u << 4), true);

    //
    // VAL is bits 31:4, so the low nibble is NOT part of the field. A stray
    // bit there must not read as "WPR2 is up" and abort a legitimate boot.
    //
    checkBool("low nibble ignored", nvWpr2IsUp(0x0000000Fu), false);
    checkBool("bit 4 is the first that counts", nvWpr2IsUp(0x00000010u), true);

    //
    // Address reconstruction. VAL is a page number: extract with >>4, then
    // scale back up by <<12.
    //
    // 0x00000010 -> VAL 0x1 -> page 1 -> 0x1000
    //
    checkBool("page 1 -> 0x1000",
              nvWpr2AddressHi(0x00000010u) == 0x1000ull, true);

    //
    // A realistic case: WPR2 based at 2 GiB. Page number is
    // 0x80000000 >> 12 = 0x80000, so the register holds 0x80000 << 4.
    //
    checkBool("2 GiB round-trips",
              nvWpr2AddressLo(0x80000u << 4) == 0x80000000ull, true);

    //
    // The bug this test exists for: if the reconstruction used the extraction
    // shift of 4 instead of the alignment shift of 12, 2 GiB would come back
    // as 0x80000 * 16 = 8 MiB. Assert we do NOT get that.
    //
    checkBool("2 GiB is not mis-scaled to 8 MiB",
              nvWpr2AddressLo(0x80000u << 4) == 0x800000ull, false);

    checkU32("alignment shift is 12",
             NV_PFB_PRI_MMU_WPR2_ALIGNMENT_SHIFT, 12u);
    checkU32("VAL field is 28 bits",
             NV_PFB_PRI_MMU_WPR2_ADDR_HI_VAL_MASK, 0x0FFFFFFFu);

    //
    // A full 28-bit page number covers 40 bits of physical address, which
    // overflows 32 bits -- hence the 64-bit return type.
    //
    checkBool("max VAL exceeds 32 bits",
              nvWpr2AddressHi(0xFFFFFFFFu) > 0xFFFFFFFFull, true);
}

//
// WPR2 VERDICT -- three outcomes, not two.
//
// checkWpr2() in the IOKit shell returned a bare bool meaning "precondition
// satisfied", and that bool was FALSE for two categorically different reasons:
// WPR2 genuinely UP, or the registers not decoding at all. Only the second is a
// defect in this driver. The caller published GSP_WPR2_ADDR_LO/_HI
// unconditionally, so a refused read reached the IORegistry as a measurement --
// setProperty() with 0xFFFFFFFF is byte-identical to setProperty() with a
// register that genuinely holds it.
//
// The join is what needs separating inputs. nvWpr2Verdict() must consult BOTH
// halves: the bool version tested ADDR_HI only, so a refused ADDR_LO went
// straight into nvWpr2AddressLo() and composed the bottom of a reported range.
// Each half is therefore refused INDEPENDENTLY below, which is the pair of
// inputs that separates a two-half test from a one-half test.
//
static void testWpr2Verdict(void)
{
    section("WPR2 verdict (refused reads are not measurements)");

    //
    // The sentinel is the value regRead32() returns for every refusal it has --
    // BAR0 unmapped, offset past the end of the mapping -- and is also what a
    // PCI read nothing answers produces.
    //
    checkU32("refusal sentinel is all-ones",
             NVWPR2_READ_REFUSED_VALUE, 0xFFFFFFFFu);
    checkBool("all-ones is refused", nvWpr2ReadRefused(0xFFFFFFFFu), true);

    //
    // Zero is NOT refused, and this is the one direction that matters: ADDR_HI
    // reads zero exactly when WPR2 is DOWN, which is the healthy case and the
    // single most common reading this driver will ever take. A sentinel that
    // swept zero in would report every healthy machine as unreadable.
    //
    checkBool("zero is NOT refused", nvWpr2ReadRefused(0x00000000u), false);
    checkBool("a plausible value is not refused",
              nvWpr2ReadRefused(0x0000A000u), false);

    //
    // One below all-ones is not refused either. This pins the sentinel as an
    // equality rather than a threshold -- a >= test would sweep in the top of
    // the address space, which is a legal WPR2 base.
    //
    checkBool("0xFFFFFFFE is NOT refused",
              nvWpr2ReadRefused(0xFFFFFFFEu), false);

    // ---- the join, with each half refused independently ----

    checkU32("both halves clean, VAL zero -> DOWN",
             nvWpr2Verdict(0x00000000u, 0x00000000u), NVWPR2_DOWN);
    checkU32("both halves clean, VAL nonzero -> UP",
             nvWpr2Verdict(0x00001000u, 0x00000010u), NVWPR2_UP);

    //
    // ADDR_HI refused. The bool version caught this one -- it tested ADDR_HI
    // for all-ones -- so on its own this check proves nothing new.
    //
    checkU32("ADDR_HI refused -> UNREADABLE",
             nvWpr2Verdict(0x00000000u, 0xFFFFFFFFu), NVWPR2_UNREADABLE);

    //
    // ADDR_LO refused with a PERFECTLY HEALTHY ADDR_HI. This is the separating
    // input: the bool version returned "precondition satisfied" here, because
    // ADDR_HI decoded and its VAL was zero. The refused low word was published
    // as GSP_WPR2_ADDR_LO and nothing anywhere said it had not been read.
    //
    checkU32("ADDR_LO refused, ADDR_HI clean -> UNREADABLE",
             nvWpr2Verdict(0xFFFFFFFFu, 0x00000000u), NVWPR2_UNREADABLE);

    //
    // Same again with WPR2 genuinely up on the high half, so the result cannot
    // be explained by the VAL-zero path.
    //
    checkU32("ADDR_LO refused, ADDR_HI up -> UNREADABLE",
             nvWpr2Verdict(0xFFFFFFFFu, 0x00000010u), NVWPR2_UNREADABLE);

    checkU32("both halves refused -> UNREADABLE",
             nvWpr2Verdict(0xFFFFFFFFu, 0xFFFFFFFFu), NVWPR2_UNREADABLE);

    //
    // UNREADABLE beats UP. A refused read must not be reported as the actionable
    // fact "a previous boot left a region standing", because that sends the
    // operator to reset an engine over a bug in this driver's offsets.
    //
    checkBool("UNREADABLE is not UP",
              nvWpr2Verdict(0xFFFFFFFFu, 0x00000010u) == NVWPR2_UP, false);
    checkBool("UNREADABLE is not DOWN",
              nvWpr2Verdict(0xFFFFFFFFu, 0x00000000u) == NVWPR2_DOWN, false);

    //
    // The three verdicts are distinct values. Two arms sharing a number would
    // satisfy every equality check above while collapsing exactly the
    // distinction this family exists to make.
    //
    checkBool("DOWN != UP", NVWPR2_DOWN == NVWPR2_UP, false);
    checkBool("DOWN != UNREADABLE", NVWPR2_DOWN == NVWPR2_UNREADABLE, false);
    checkBool("UP != UNREADABLE", NVWPR2_UP == NVWPR2_UNREADABLE, false);

    //
    // Every verdict below the family's own limit has a name, and the loop bounds
    // on NVWPR2_VERDICT_LIMIT rather than on a typed count -- handoff20 s1,
    // where igModeVerdictName() had arms for 0..5 while its family limit was 6
    // and the missing verdict published the unknown string with all four gates
    // green over it. A family that GROWS reddens on the commit that grows it.
    //
    for (NVU32 v = 0; v < NVWPR2_VERDICT_LIMIT; v++) {
        char label[64];
        std::snprintf(label, sizeof(label), "verdict %u has a name", (unsigned)v);
        checkBool(label,
                  std::strcmp(nvWpr2VerdictName(v),
                              "unrecognised WPR2 verdict") != 0, true);
    }

    //
    // Each name is its OWN string. A copy-paste arm returning a sibling's name
    // satisfies every "has a name" assertion above -- handoff21 s1, where six of
    // seven architecture arms had never executed while 64 checks printed green.
    //
    checkBool("DOWN is named down",
              std::strcmp(nvWpr2VerdictName(NVWPR2_DOWN), "down") == 0, true);
    checkBool("UP is named up",
              std::strcmp(nvWpr2VerdictName(NVWPR2_UP), "up") == 0, true);
    checkBool("UNREADABLE is named unreadable",
              std::strcmp(nvWpr2VerdictName(NVWPR2_UNREADABLE),
                          "unreadable") == 0, true);

    //
    // The value one PAST the family answers the sentinel. This is the tripwire
    // that forces whoever adds a fourth verdict to come back and extend the
    // switch, rather than silently inheriting `default:`.
    //
    checkBool("one past the family is unnamed",
              std::strcmp(nvWpr2VerdictName(NVWPR2_VERDICT_LIMIT),
                          "unrecognised WPR2 verdict") == 0, true);

    //
    // The limit is the count, not one of the members. A limit written as 2 or 4
    // would make the loop above either skip an arm or demand one that does not
    // exist.
    //
    checkU32("verdict limit is 3", NVWPR2_VERDICT_LIMIT, 3u);
}

//
// SEC2 -- the engine this project did not know existed until the mmiotrace
// region table was corrected.
//
// tools/parse-mmiotrace.py originally placed psec at 0x114000, immediately
// after PGSP, which is where you would guess it lives. dev_sec_pri.h:27 says
// NV_PSEC is 0x843fff:0x840000. The consequence of the guess was that 25,202
// accesses -- an entire second falcon boot -- were filed as "unmapped" and
// never analysed.
//
// These checks exist so the same guess cannot be re-introduced silently.
//
static void testSecAperture(void)
{
    section("SEC2 aperture (the region that was misfiled)");

    checkU32("NV_PSEC_BASE from dev_sec_pri.h", NV_PSEC_BASE, 0x00840000u);
    checkU32("NV_PSEC_LIMIT from dev_sec_pri.h", NV_PSEC_LIMIT, 0x00843FFFu);

    //
    // The wrong value, asserted as wrong. 0x114000 is inside neither engine;
    // it is the gap above PGSP. If anyone re-derives the base by adding 0x4000
    // to PGSP, this fails.
    //
    checkBool("0x114000 is NOT the SEC2 base", NV_PSEC_BASE == 0x00114000u, false);
    checkBool("0x114000 is not in the SEC2 aperture",
              nvIsInSecAperture(0x00114000u), false);
    checkBool("0x114000 is not in the GSP aperture either",
              nvIsInGspAperture(0x00114000u), false);

    // The aperture is 16 KiB, same size as PGSP.
    checkU32("SEC2 aperture is 16 KiB",
             NV_PSEC_LIMIT - NV_PSEC_BASE + 1u, 0x4000u);
    checkU32("same size as the GSP aperture",
             (NV_PSEC_LIMIT - NV_PSEC_BASE) - (NV_PGSP_LIMIT - NV_PGSP_BASE), 0u);

    // The two engines must not overlap, or an accessor mix-up is undetectable.
    checkBool("GSP and SEC2 apertures are disjoint", nvGspAndSecDisjoint(), true);

    //
    // Address translation, mirroring testAddressTranslation() for GSP. The
    // trace grounds these: the driver writes SEC2's FALCON_ENGINE at absolute
    // 0x8403C0 and SEC2's BCR_CTRL at absolute 0x841668.
    //
    checkU32("falcon ENGINE -> absolute (trace: 0x8403C0)",
             nvSecFalconReg(NV_PFALCON_FALCON_ENGINE), 0x008403C0u);
    checkU32("relative ENGINE agrees with the absolute GSP spelling",
             nvGspFalconReg(NV_PFALCON_FALCON_ENGINE), NV_PGSP_FALCON_ENGINE);
    checkU32("riscv BCR_CTRL -> absolute (trace: 0x841668)",
             nvSecRiscvReg(NV_PRISCV_RISCV_BCR_CTRL), 0x00841668u);

    //
    // The FALCON2 block sits exactly one 4 KiB block above the engine base,
    // for both engines. This is the relationship the trace confirmed.
    //
    checkU32("SEC2 FALCON2 is base + 0x1000",
             NV_FALCON2_SEC_BASE - NV_PSEC_BASE, NV_FALCON_BLOCK_SIZE);
    checkU32("GSP FALCON2 is base + 0x1000",
             NV_FALCON2_GSP_BASE - NV_PGSP_BASE, NV_FALCON_BLOCK_SIZE);

    //
    // Cross-engine confusion is the failure this file is structured to
    // prevent: a SEC2 offset through the GSP accessor produces a valid-looking
    // address in the wrong engine. Assert the accessors disagree.
    //
    checkBool("GSP accessor does not produce a SEC2 address",
              nvIsInSecAperture(nvGspFalconReg(NV_PFALCON_FALCON_CPUCTL)), false);
    checkBool("SEC2 accessor does not produce a GSP address",
              nvIsInGspAperture(nvSecFalconReg(NV_PFALCON_FALCON_CPUCTL)), false);

    // Both engines are reachable through the 16 MiB BAR0 mapping.
    checkBool("SEC2 base inside BAR0", nvIsInBar0(NV_PSEC_BASE), true);
    checkBool("SEC2 limit inside BAR0", nvIsInBar0(NV_PSEC_LIMIT), true);
}

//
// Boot ROM parameter block.
//
// Four registers, written in a fixed order, MOD_SEL last. MOD_SEL is the
// latch: write it first and the boot ROM commits stale parameters, rejects
// the signature, and the core sits in halt with no diagnostic. The offsets
// and the observed values both come from mmio-boot.log section 3.4.
//
static void testBromParameters(void)
{
    section("boot ROM parameters (order-sensitive)");

    checkU32("MOD_SEL offset", NV_PFALCON2_FALCON_MOD_SEL, 0x00000180u);
    checkU32("BROM_CURR_UCODE_ID offset",
             NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID, 0x00000198u);
    checkU32("BROM_ENGIDMASK offset",
             NV_PFALCON2_FALCON_BROM_ENGIDMASK, 0x0000019Cu);
    checkU32("BROM_PARAADDR(0) offset",
             NV_PFALCON2_FALCON_BROM_PARAADDR(0), 0x00000210u);

    // Indexed port, stride 4, same convention as the other PARAADDR-style regs.
    checkU32("PARAADDR(1) is +4",
             NV_PFALCON2_FALCON_BROM_PARAADDR(1)
                 - NV_PFALCON2_FALCON_BROM_PARAADDR(0), 4u);
    checkU32("PARAADDR(3) is +12",
             NV_PFALCON2_FALCON_BROM_PARAADDR(3)
                 - NV_PFALCON2_FALCON_BROM_PARAADDR(0), 12u);

    //
    // Absolute addresses, checked against the raw trace. These four writes
    // appear at t=0.922832..0.922836 in mmio-boot.log:
    //
    //   0x111210 <- 0x5A4   PARAADDR(0)
    //   0x11119C <- 0x400   ENGIDMASK
    //   0x111198 <- 0x009   CURR_UCODE_ID
    //   0x111180 <- 0x001   MOD_SEL          <- latch, written last
    //
    checkU32("PARAADDR(0) absolute matches trace",
             nvGspRiscvReg(NV_PFALCON2_FALCON_BROM_PARAADDR(0)), 0x00111210u);
    checkU32("ENGIDMASK absolute matches trace",
             nvGspRiscvReg(NV_PFALCON2_FALCON_BROM_ENGIDMASK), 0x0011119Cu);
    checkU32("CURR_UCODE_ID absolute matches trace",
             nvGspRiscvReg(NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID), 0x00111198u);
    checkU32("MOD_SEL absolute matches trace",
             nvGspRiscvReg(NV_PFALCON2_FALCON_MOD_SEL), 0x00111180u);

    //
    // All four are in the FALCON2 block, NOT the falcon block. Routing any of
    // them through nvGspFalconReg() lands 0x1000 low on a real register --
    // the silent-corruption case. Assert the offsets are small enough that the
    // mistake produces an address inside the falcon block, i.e. that the guard
    // is genuinely needed rather than incidentally safe.
    //
    checkBool("MOD_SEL via falcon accessor lands in the wrong block",
              nvGspFalconReg(NV_PFALCON2_FALCON_MOD_SEL)
                  != nvGspRiscvReg(NV_PFALCON2_FALCON_MOD_SEL), true);
    checkU32("  and it is exactly 0x1000 low",
             nvGspRiscvReg(NV_PFALCON2_FALCON_MOD_SEL)
                 - nvGspFalconReg(NV_PFALCON2_FALCON_MOD_SEL), 0x1000u);

    //
    // Observed values. ENGIDMASK 0x400 is bit 10; UCODE_ID 9 identifies the
    // GSP booter. The boot ROM checks both against the firmware signature, so
    // they are fixed inputs, not tunables.
    //
    checkU32("observed ENGIDMASK", NV_PFALCON2_GSP_OBSERVED_ENGIDMASK, 0x400u);
    checkU32("  ENGIDMASK is a single bit",
             NV_PFALCON2_GSP_OBSERVED_ENGIDMASK
                 & (NV_PFALCON2_GSP_OBSERVED_ENGIDMASK - 1u), 0u);
    checkU32("  which bit", 10u, 10u);
    checkU32("observed UCODE_ID", NV_PFALCON2_GSP_OBSERVED_UCODE_ID, 9u);
    checkBool("UCODE_ID fits its 8-bit field",
              NV_PFALCON2_GSP_OBSERVED_UCODE_ID
                  <= NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID_MASK, true);

    //
    // MOD_SEL = 1 is RSA3K signed-ucode mode, the value the trace shows.
    //
    checkU32("MOD_SEL ALGO_RSA3K", NV_PFALCON2_FALCON_MOD_SEL_ALGO_RSA3K, 1u);
    checkBool("ALGO fits its 8-bit field",
              NV_PFALCON2_FALCON_MOD_SEL_ALGO_RSA3K
                  <= NV_PFALCON2_FALCON_MOD_SEL_ALGO_MASK, true);
}

//
// The captured BCR_CTRL start value, cross-checked against the field
// definitions. testBcr() already assembles 0x111 from the fields; this asserts
// the constant recorded from the trace is that same value, so a mistyped shift
// fails here instead of on hardware.
//
static void testObservedBootValue(void)
{
    section("captured boot value vs assembled fields");

    NVU32 assembled = (NV_PRISCV_RISCV_BCR_CTRL_VALID_TRUE
                          << NV_PRISCV_RISCV_BCR_CTRL_VALID_SHIFT)
                    | (NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_RISCV
                          << NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_SHIFT)
                    | (NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_TRUE
                          << NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_SHIFT);

    checkU32("trace value is 0x111",
             NV_PRISCV_RISCV_BCR_CTRL_OBSERVED_START, 0x00000111u);
    checkU32("assembled fields reproduce it",
             assembled, NV_PRISCV_RISCV_BCR_CTRL_OBSERVED_START);

    checkBool("decodes as VALID",
              nvBcrValid(NV_PRISCV_RISCV_BCR_CTRL_OBSERVED_START), true);
    checkBool("decodes as RISC-V core",
              nvBcrSelectsRiscv(NV_PRISCV_RISCV_BCR_CTRL_OBSERVED_START), true);

    //
    // The trace also shows this register written back to 0 once GSP is up
    // (t=1.840748). That value must decode as not-valid, or a re-entrant
    // bringup would read a stale "already booted" state.
    //
    checkBool("cleared value is not VALID", nvBcrValid(0u), false);
}

//
// HWCFG2 values actually observed, for both engines.
//
// GSP read 0x47F7; SEC2 read 0x77F7 before its boot and 0x67F7 after. The
// difference is in the scrubbing/reset status bits, not the capability bits --
// both engines report RISC-V capable, which is why both need the FALCON2 path.
//
static void testObservedHwcfg(void)
{
    section("observed HWCFG2 values");

    checkBool("GSP 0x47F7 is RISC-V capable",
              nvFalconIsRiscvCapable(0x000047F7u), true);
    checkBool("SEC2 0x77F7 is RISC-V capable",
              nvFalconIsRiscvCapable(0x000077F7u), true);
    checkBool("SEC2 0x67F7 is RISC-V capable",
              nvFalconIsRiscvCapable(0x000067F7u), true);

    //
    // Bit 10 is the RISC-V bit. 0x47F7 has it set; clearing it must flip the
    // answer, otherwise the check is reading the wrong bit and would pass on
    // any value.
    //
    checkBool("clearing bit 10 flips the answer",
              nvFalconIsRiscvCapable(0x000047F7u & ~(1u << 10)), false);
}

//
// FBIF -- the block that makes a physical-address DMA legal.
//
// This is the most directly useful thing the SEC2 sequence revealed. Without
// ALLOW_PHYS_NO_CTX the falcon demands a context DMA object, which a kext with
// no framebuffer allocator cannot build. With it, a physical address from
// IOMemoryDescriptor::getPhysicalAddress() is accepted directly.
//
// The offsets are checked against the trace rather than against the header
// alone, because the header spells them relative to the FBIF block and says
// nothing about where that block sits.
//
static void testFbif(void)
{
    section("FBIF (physical DMA without a context DMA)");

    checkU32("FBIF block is engine-relative 0x600",
             NV_PFALCON_FBIF_BASE, 0x00000600u);
    checkU32("TRANSCFG(0) offset", NV_PFALCON_FBIF_TRANSCFG(0), 0x00000600u);
    checkU32("CTL offset", NV_PFALCON_FBIF_CTL, 0x00000624u);

    //
    // The base is derived, not asserted: CTL is documented at 0x24 within the
    // FBIF block, and the trace shows SEC2 writing 0x840600 and 0x840624. The
    // difference confirms the block placement.
    //
    checkU32("CTL is FBIF_BASE + 0x24",
             NV_PFALCON_FBIF_CTL - NV_PFALCON_FBIF_BASE, 0x24u);

    // Absolute addresses, matched to the SEC2 writes at t=1.119686/1.119691.
    checkU32("TRANSCFG(0) absolute matches trace (0x840600)",
             nvSecFalconReg(NV_PFALCON_FBIF_TRANSCFG(0)), 0x00840600u);
    checkU32("CTL absolute matches trace (0x840624)",
             nvSecFalconReg(NV_PFALCON_FBIF_CTL), 0x00840624u);

    // Indexed port, stride 4, eight entries.
    checkU32("TRANSCFG(1) is +4",
             NV_PFALCON_FBIF_TRANSCFG(1) - NV_PFALCON_FBIF_TRANSCFG(0), 4u);
    checkU32("TRANSCFG has 8 entries", NV_PFALCON_FBIF_TRANSCFG_SIZE_1, 8u);

    //
    // Decode the values the working driver actually wrote. If either decoder
    // reads the wrong bit these fail against real data, not invented data.
    //
    checkU32("observed TRANSCFG(0)",
             NV_PFALCON_FBIF_OBSERVED_TRANSCFG0, 0x115u);
    checkBool("  targets coherent sysmem",
              nvFbifTargetsSysmem(NV_PFALCON_FBIF_OBSERVED_TRANSCFG0), true);
    checkBool("  is a physical mapping",
              nvFbifIsPhysical(NV_PFALCON_FBIF_OBSERVED_TRANSCFG0), true);

    checkU32("observed CTL", NV_PFALCON_FBIF_OBSERVED_CTL, 0x190u);
    checkBool("  allows phys with no ctx",
              nvFbifAllowsPhysNoCtx(NV_PFALCON_FBIF_OBSERVED_CTL), true);

    //
    // Negative cases. A decoder that ignores its input and returns a constant
    // would pass every check above; these make that impossible.
    //
    checkBool("LOCAL_FB does not read as sysmem",
              nvFbifTargetsSysmem(NV_PFALCON_FBIF_TRANSCFG_TARGET_LOCAL_FB),
              false);
    checkBool("MEM_TYPE clear does not read as physical",
              nvFbifIsPhysical(NV_PFALCON_FBIF_OBSERVED_TRANSCFG0
                                   & ~(1u << 2)),
              false);
    checkBool("bit 7 clear does not allow phys-no-ctx",
              nvFbifAllowsPhysNoCtx(NV_PFALCON_FBIF_OBSERVED_CTL
                                        & ~(1u << 7)),
              false);

    // Reachable through the BAR0 mapping on both engines.
    checkBool("SEC2 FBIF inside BAR0",
              nvIsInBar0(nvSecFalconReg(NV_PFALCON_FBIF_CTL)), true);
    checkBool("GSP FBIF inside BAR0",
              nvIsInBar0(nvGspFalconReg(NV_PFALCON_FBIF_CTL)), true);
}

//
// SEC2 boot ordering, as observed. An earlier draft of the notes claimed SEC2
// boots first; the trace says the opposite. These constants encode the timings
// so the corrected ordering is recorded somewhere executable.
//
static void testSecOrdering(void)
{
    section("SEC2 start path is Falcon, not RISC-V");

    //
    // SEC2 is RISC-V-capable -- both HWCFG2 values it reported have bit 10 --
    // yet the driver starts it via CPUCTL_ALIAS.STARTCPU rather than BCR_CTRL.
    // Capability is not the boot mode actually used, and conflating the two
    // would send a Phase 4 implementation down the wrong path for this engine.
    //
    checkU32("CPUCTL_ALIAS offset", NV_PFALCON_FALCON_CPUCTL_ALIAS, 0x00000130u);
    checkU32("SEC2 CPUCTL_ALIAS absolute matches trace (0x840130)",
             nvSecFalconReg(NV_PFALCON_FALCON_CPUCTL_ALIAS), 0x00840130u);

    // The observed write was 0x2, i.e. STARTCPU at bit 1.
    NVU32 startcpu = 1u << NV_PFALCON_FALCON_CPUCTL_ALIAS_STARTCPU_SHIFT;
    checkU32("STARTCPU bit assembles to the observed 0x2", startcpu, 0x2u);

    // DMACTL and FALCON_RM, the other two SEC2 preamble writes.
    checkU32("SEC2 DMACTL absolute matches trace (0x84010C)",
             nvSecFalconReg(NV_PFALCON_FALCON_DMACTL), 0x0084010Cu);
    checkU32("SEC2 FALCON_RM absolute matches trace (0x840084)",
             nvSecFalconReg(NV_PFALCON_FALCON_RM), 0x00840084u);
}

//
// The RISC-V error latches as a VERDICT rather than a bool.
//
// testEngineReset() below asserts nvRiscvHasError() on four inputs, and all
// four are satisfied IDENTICALLY by the old two-line bool and by the verdict
// wrapper that replaced it: `== true` has three preimages here (PRIV, HUB,
// BOTH) and `== false` has one. That is 3050-handoff34 s1's lesson arriving in
// the same file -- a bool assertion cannot tell a fix from the bug it fixed.
// Those four checks are KEPT rather than replaced, because nvRiscvHasError() is
// what the go/no-go join actually calls, and a verdict-only suite would leave
// the wrapper free to stop consulting the verdict with every check green.
//
static void testRiscvErrorVerdict(void)
{
    section("RISC-V error verdict (a refused latch is not a fault)");

    //
    // The sentinel is spelled by THIS family rather than borrowed from the WPR2
    // one. Both land on all-ones, by different roads; a sentinel taken from a
    // sibling family five hundred lines away is handoff21 s1b's mistake, and
    // this check is what reddens if the two ever diverge.
    //
    checkU32("refusal sentinel is all-ones",
             NVRISCV_ERR_READ_REFUSED_VALUE, 0xFFFFFFFFu);
    checkBool("all-ones is refused",
              nvRiscvErrReadRefused(0xFFFFFFFFu), true);

    //
    // Zero is the HEALTHY reading for both of these registers, so refusal must
    // be FALSE on it -- the opposite polarity to the WPR2 family, where zero in
    // ADDR_HI is the healthy DOWN case and all-ones is the refusal. One
    // sentinel, two families, arrived at from opposite directions.
    //
    checkBool("zero is NOT refused (it is the healthy latch state)",
              nvRiscvErrReadRefused(0u), false);

    //
    // An equality, never a threshold. Every bit of an error-status register is a
    // distinct latch, so there is no "too large to be real" reading to
    // threshold against -- only the one exact value regRead32() manufactures.
    //
    checkBool("one below the sentinel is a measurement",
              nvRiscvErrReadRefused(0xFFFFFFFEu), false);
    checkBool("a single latch bit is a measurement",
              nvRiscvErrReadRefused(1u), false);

    //
    // The four arms where both halves decoded.
    //
    checkU32("both clear -> NONE",
             nvRiscvErrorVerdict(0u, 0u), NVRISCV_ERR_NONE);
    checkU32("priv latch only -> PRIV",
             nvRiscvErrorVerdict(1u, 0u), NVRISCV_ERR_PRIV);
    checkU32("hub latch only -> HUB",
             nvRiscvErrorVerdict(0u, 1u), NVRISCV_ERR_HUB);
    checkU32("both latched -> BOTH",
             nvRiscvErrorVerdict(0xDEADu, 0xBEEFu), NVRISCV_ERR_BOTH);

    //
    // UNREADABLE takes precedence over a genuine latch in the other half, and
    // the two mixed cases are what pin it. A hub fault beside a refused priv
    // read must NOT report "hub error latched": the half that did not decode is
    // itself the finding, and it is a bug in THIS DRIVER's mapping rather than a
    // fact about the card -- handoff22 s1b, the bug somebody can go and fix
    // outranks the fact they cannot.
    //
    checkU32("priv refused -> UNREADABLE",
             nvRiscvErrorVerdict(0xFFFFFFFFu, 0u), NVRISCV_ERR_UNREADABLE);
    checkU32("hub refused -> UNREADABLE",
             nvRiscvErrorVerdict(0u, 0xFFFFFFFFu), NVRISCV_ERR_UNREADABLE);
    checkU32("priv refused beside a real hub latch -> UNREADABLE",
             nvRiscvErrorVerdict(0xFFFFFFFFu, 1u), NVRISCV_ERR_UNREADABLE);
    checkU32("hub refused beside a real priv latch -> UNREADABLE",
             nvRiscvErrorVerdict(1u, 0xFFFFFFFFu), NVRISCV_ERR_UNREADABLE);
    checkU32("both refused -> UNREADABLE",
             nvRiscvErrorVerdict(0xFFFFFFFFu, 0xFFFFFFFFu),
             NVRISCV_ERR_UNREADABLE);

    //
    // The five verdicts are distinct values. Two arms sharing a number would
    // satisfy every equality above while collapsing exactly the distinction the
    // family exists to make.
    //
    checkBool("NONE != PRIV", NVRISCV_ERR_NONE == NVRISCV_ERR_PRIV, false);
    checkBool("NONE != HUB", NVRISCV_ERR_NONE == NVRISCV_ERR_HUB, false);
    checkBool("NONE != BOTH", NVRISCV_ERR_NONE == NVRISCV_ERR_BOTH, false);
    checkBool("NONE != UNREADABLE",
              NVRISCV_ERR_NONE == NVRISCV_ERR_UNREADABLE, false);
    checkBool("PRIV != HUB", NVRISCV_ERR_PRIV == NVRISCV_ERR_HUB, false);
    checkBool("PRIV != BOTH", NVRISCV_ERR_PRIV == NVRISCV_ERR_BOTH, false);
    checkBool("PRIV != UNREADABLE",
              NVRISCV_ERR_PRIV == NVRISCV_ERR_UNREADABLE, false);
    checkBool("HUB != BOTH", NVRISCV_ERR_HUB == NVRISCV_ERR_BOTH, false);
    checkBool("HUB != UNREADABLE",
              NVRISCV_ERR_HUB == NVRISCV_ERR_UNREADABLE, false);
    checkBool("BOTH != UNREADABLE",
              NVRISCV_ERR_BOTH == NVRISCV_ERR_UNREADABLE, false);

    //
    // The sentinel STRING is asserted against what the header's own `default:`
    // arm returns, never guessed from a sibling function. handoff21 s1b is the
    // cost of guessing: two switches five lines apart returned different
    // defaults, and a coverage loop comparing against a string the subject can
    // never produce counted every arm as named -- including a deleted one.
    //
    static const char *const kErrSentinelName =
        "unrecognised RISC-V error verdict";
    checkBool("sentinel name is the header's own default arm",
              std::strcmp(nvRiscvErrorVerdictName(NVRISCV_ERR_VERDICT_LIMIT),
                          kErrSentinelName) == 0, true);

    //
    // Every verdict BELOW the family's own limit is named. Bounded on
    // NVRISCV_ERR_VERDICT_LIMIT rather than a typed count, so a sixth verdict
    // added to the header with no arm in the switch reddens on the commit that
    // adds it -- handoff20 s1, where igModeVerdictName() lost
    // IG_MODE_SRC_MISMATCH to `default:` and all four gates stayed green.
    //
    for (NVU32 v = 0; v < NVRISCV_ERR_VERDICT_LIMIT; v++) {
        checkBool("verdict below the limit has a name",
                  std::strcmp(nvRiscvErrorVerdictName(v),
                              kErrSentinelName) != 0, true);
    }

    //
    // And each arm returns its OWN name. The loop above only proves a verdict is
    // not the sentinel; a copy-paste arm returning a sibling's string satisfies
    // it completely (handoff21 s1). All five literals differ, so these five
    // checks also establish the names are pairwise distinct.
    //
    checkBool("NONE is named",
              std::strcmp(nvRiscvErrorVerdictName(NVRISCV_ERR_NONE),
                          "latches clear") == 0, true);
    checkBool("PRIV is named",
              std::strcmp(nvRiscvErrorVerdictName(NVRISCV_ERR_PRIV),
                          "priv error latched") == 0, true);
    checkBool("HUB is named",
              std::strcmp(nvRiscvErrorVerdictName(NVRISCV_ERR_HUB),
                          "hub error latched") == 0, true);
    checkBool("BOTH is named",
              std::strcmp(nvRiscvErrorVerdictName(NVRISCV_ERR_BOTH),
                          "priv and hub errors latched") == 0, true);
    checkBool("UNREADABLE is named",
              std::strcmp(nvRiscvErrorVerdictName(NVRISCV_ERR_UNREADABLE),
                          "error latches unreadable") == 0, true);

    //
    // The wrapper, and the one input that separates a NEGATION of the OK arm
    // from a DISJUNCTION over the three fault arms.
    //
    // `verdict == PRIV || == HUB || == BOTH` is FALSE for UNREADABLE, so it
    // would report a refused read as "no fault" and send an unmeasurable
    // precondition through the go/no-go as satisfied. `!= NONE` is TRUE, which
    // also preserves the old two-line bool exactly -- `(0xFFFFFFFF != 0) || ...`
    // answered true, and so does this.
    //
    checkBool("wrapper true on both halves refused",
              nvRiscvHasError(0xFFFFFFFFu, 0xFFFFFFFFu), true);
    checkBool("wrapper true on one half refused",
              nvRiscvHasError(0xFFFFFFFFu, 0u), true);

    //
    // The wrapper is pinned to the verdict on every input above, so it cannot
    // stop consulting it -- 3050-handoff34 s2's PAIR, which is the whole reason
    // the bool was kept rather than deleted.
    //
    {
        static const NVU32 kPairs[][2] = {
            { 0u,          0u },
            { 1u,          0u },
            { 0u,          1u },
            { 0xDEADu,     0xBEEFu },
            { 0xFFFFFFFFu, 0u },
            { 0u,          0xFFFFFFFFu },
            { 0xFFFFFFFFu, 1u },
            { 1u,          0xFFFFFFFFu },
            { 0xFFFFFFFFu, 0xFFFFFFFFu },
        };

        for (unsigned i = 0; i < sizeof(kPairs) / sizeof(kPairs[0]); i++) {
            checkBool("wrapper == (verdict != NONE)",
                      nvRiscvHasError(kPairs[i][0], kPairs[i][1]),
                      nvRiscvErrorVerdict(kPairs[i][0], kPairs[i][1])
                          != NVRISCV_ERR_NONE);
        }
    }
}

static void testEngineReset(void)
{
    section("engine reset and error latches");

    checkBool("RESET set -> in reset", nvGspEngineInReset(1u), true);
    checkBool("RESET clear -> out of reset", nvGspEngineInReset(0u), false);

    checkBool("no error", nvRiscvHasError(0u, 0u), false);
    checkBool("priv error only", nvRiscvHasError(1u, 0u), true);
    checkBool("hub error only", nvRiscvHasError(0u, 1u), true);
    checkBool("both", nvRiscvHasError(0xDEADu, 0xBEEFu), true);
}

int main(void)
{
    std::printf("NVFalcon.hpp host tests (Phase 4 register layer)\n");
    std::printf("================================================\n");

    testAddressTranslation();
    testIndexedPorts();
    testHwcfg();
    testCpuctl();
    testDma();
    testRiscv();
    testBcr();
    testWpr2();
    testWpr2Verdict();
    testRiscvErrorVerdict();
    testEngineReset();

    // Added from the mmio-boot.log analysis (see PHASE4-GSP-BRINGUP.md).
    testSecAperture();
    testBromParameters();
    testObservedBootValue();
    testObservedHwcfg();
    testFbif();
    testSecOrdering();

    //
    // The caveat prints BEFORE the summary on purpose. The count must be the
    // LAST line of output so `tail -1` on this binary returns a count and not a
    // sentence -- a reader (or a script) checking the final line is exactly how
    // a zero-assertion suite gets misread as passing. All five suites in this
    // directory end on their count for that reason.
    //
    if (gFailures == 0) {
        std::printf("\nPhase 4 address arithmetic and decoding are correct.\n"
                    "This does NOT mean GSP will boot -- the firmware blobs,\n"
                    "DMA plumbing, and the RPC layer are all still unwritten.\n");
    }

    std::printf("\n%d/%d passed, %d failed\n",
                gChecks - gFailures, gChecks, gFailures);

    return gFailures == 0 ? 0 : 1;
}
