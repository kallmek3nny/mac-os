//
//  test_gsp_boot.cpp
//  Host-side tests for NVGspBoot.hpp -- the Phase 4 GSP bringup state machine.
//
//  Builds and runs under MSVC on Windows, GCC/Clang elsewhere. No IOKit, no
//  GPU, no macOS. The machine talks to a simulated BAR0 through the same
//  read32/write32 callback pair the kext hands it, so what runs here is the
//  identical code path that will run on hardware.
//
//  This is the only test in the suite that exercises a WRITE sequence, which
//  makes it the only place two things can be pinned:
//
//    1. The write ORDER. MOD_SEL is a latch: writing it before the other three
//       BROM parameters commits stale values and the boot ROM rejects the
//       signature. Checking the final register contents would pass either way,
//       so the journal below records the order and asserts on positions.
//
//    2. The dry-run gate. The kext ships read-only, and "read-only" is only
//       meaningful if a refused write provably never reaches the backing
//       store. The fixture counts stores independently of the machine's own
//       counter for exactly that reason.
//
//  Every expected value comes from PHASE4-GSP-BRINGUP.md, which was derived
//  from mmio-boot.log. Nothing here is invented.
//

#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVGspBoot.hpp"

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

//
// 64-bit variant. Needed because the DMATRFBASE1:DMATRFBASE pair is a SPLIT of
// a 64-bit shifted address, and the only assertion that catches an overlapping
// split is one that reassembles the pair and compares it in 64 bits. Comparing
// the halves separately is exactly what let the overlapping form stay green.
//
static void checkU64(const char *label,
                     unsigned long long actual,
                     unsigned long long expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-50s 0x%llX\n", label, actual);
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-50s got 0x%llX, expected 0x%llX\n",
                label, actual, expected);
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

static void checkResult(const char *label,
                       NVGspBootResult actual,
                       NVGspBootResult expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-50s %s\n", label, nvGspBootResultName(actual));
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-50s got %s, expected %s\n", label,
                nvGspBootResultName(actual), nvGspBootResultName(expected));
}


// ===========================================================================
// Absolute addresses, transcribed from the trace in PHASE4-GSP-BRINGUP.md
// sections 3.1-3.6.
//
// These are spelled as literals ON PURPOSE. The machine derives its addresses
// through nvGspFalconReg()/nvGspRiscvReg(); if the fixture derived them the
// same way, a wrong base constant would cancel out and the suite would pass
// while the driver scribbled on the wrong engine. Literals from the trace are
// an independent witness.
// ===========================================================================

#define ABS_FALCON_CPUCTL     0x00110100u
#define ABS_FALCON_HWCFG2     0x001100F4u
#define ABS_FALCON_ENGINE     0x001103C0u
#define ABS_BROM_PARAADDR0    0x00111210u
#define ABS_BROM_ENGIDMASK    0x0011119Cu
#define ABS_BROM_UCODE_ID     0x00111198u
#define ABS_BROM_MOD_SEL      0x00111180u
#define ABS_RISCV_BCR_CTRL    0x00111668u
#define ABS_RISCV_CPUCTL      0x00111388u
#define ABS_RISCV_PRIV_ERR    0x00111500u
#define ABS_RISCV_HUB_ERR     0x00111510u

// Phase 5 DMA block, spec section 3.3.
#define ABS_DMATRFBASE        0x00110110u
#define ABS_DMATRFMOFFS       0x00110114u
#define ABS_DMATRFCMD         0x00110118u
#define ABS_DMATRFFBOFFS      0x0011011Cu
#define ABS_DMATRFBASE1       0x00110128u

//
// FBIF, the block that makes a physical-address DMA legal at all. NVFalcon.hpp
// puts FBIF at falcon-relative 0x600 and CTL at +0x24 within it, and the falcon
// block itself is at 0x00110000 (ABS_FALCON_CPUCTL 0x00110100 for a 0x100
// register). These are written out longhand for the same reason every other
// ABS_ constant is: a translation bug in nvGspFalconReg() must not be able to
// move the expected address along with the actual one.
//
#define ABS_FBIF_TRANSCFG0    0x00110600u
#define ABS_FBIF_CTL          0x00110624u

#define OBS_FBIF_TRANSCFG0    0x00000115u
#define OBS_FBIF_CTL          0x00000190u

//
// The physical base the trace used. DMATRFBASE stores addr >> 8, and the trace
// wrote 0x00FFEEE2, so the address itself was 0xFFEEE200 -- which is where the
// expected base value in the DMA tests comes from.
//
#define OBS_IMAGE_PHYS_LO     0xFFEEE200u
#define OBS_DMATRFBASE_VALUE  0x00FFEEE2u
#define OBS_DMATRFCMD_VALUE   0x00000614u
#define DMA_CHUNK             256u

//
// Observed register values. 0x47F7 is the real HWCFG2 read at t=0.9207: bit 10
// RISCV is SET, and bit 12 MEM_SCRUBBING is CLEAR -- which in that register's
// inverted sense means scrubbing is already DONE. Setting bit 12 is therefore
// how you simulate a scrub that never finishes.
//
#define OBS_CPUCTL_HALTED     0x00000010u
#define OBS_HWCFG2_SCRUB_DONE 0x000047F7u
#define OBS_HWCFG2_SCRUBBING  0x000057F7u
#define RISCV_CPUCTL_ACTIVE   0x00000080u


// ===========================================================================
// The simulated BAR0.
// ===========================================================================

#define FIXTURE_WORDS         (0x00114000u / 4u)
#define FIXTURE_MAX_WRITES    32u

//
// Distinct READ offsets, journalled as a set rather than a sequence.
//
// Writes are journalled in order because several sequences latch on their last
// write. Reads have no such property -- ACTIVE_WAIT polls the same three
// registers until the core comes up, so a read SEQUENCE is dominated by poll
// counts and says nothing. What is worth knowing is the SET of addresses the
// machine touched at all, because that is the only thing that distinguishes
// nvGspRiscvReg() from nvGspFalconReg() at a read site: the relative offset is
// identical either way and only the base differs, by exactly one page.
//
// A full healthy run touches six distinct read addresses, so 24 is loose.
// Overflow is recorded rather than ignored, because a set that silently stopped
// accepting entries would make every "never read" assertion below pass by
// having observed nothing.
//
#define FIXTURE_MAX_READ_OFFSETS 24u

struct WriteRec {
    NVU32 offset;
    NVU32 value;
};

struct Fixture {
    NVU32    regs[FIXTURE_WORDS];

    WriteRec writes[FIXTURE_MAX_WRITES];
    NVU32    writeCount;      // stores that actually LANDED
    NVU32    readCount;

    //
    // The set of distinct offsets ever READ, and whether it filled up. Kept
    // separate from readCount, which counts accesses rather than addresses.
    //
    NVU32    readOffsets[FIXTURE_MAX_READ_OFFSETS];
    NVU32    readOffsetCount;
    bool     readOffsetOverflow;

    bool     apertureViolation;
    bool     journalOverflow;

    //
    // DMA engine simulation. A real Falcon clears IDLE while a transfer is in
    // flight and sets it when the chunk lands; the stored 0x614 has bit 1
    // clear, so without this the drain state would poll a register that never
    // changes and every DMA test would be a timeout test.
    //
    NVU32    dmaDelayPolls;     // non-idle reads to serve before reporting IDLE
    NVU32    dmaReadsSinceCmd;
    bool     dmaNeverIdle;      // simulate a wedged transfer
};

static Fixture gFix;

//
// Both callbacks police the aperture themselves. The machine already checks
// nvIsInGspAperture() before every access, so a violation reaching here means
// that check is broken -- which is precisely the bug worth catching, since a
// RISC-V access 0x1000 low lands on real Falcon registers and corrupts
// silently.
//
static NVU32 fixtureRead32(void *ctx, NVU32 offset)
{
    Fixture *f = (Fixture *)ctx;

    f->readCount++;

    //
    // Record the offset BEFORE the aperture check, so a read that the aperture
    // guard would reject is still visible as an address the machine reached
    // for. Recording it afterwards would hide exactly the mistranslation this
    // is here to detect if it happened to land outside the window.
    //
    {
        bool known = false;
        for (NVU32 i = 0u; i < f->readOffsetCount; ++i) {
            if (f->readOffsets[i] == offset) {
                known = true;
                break;
            }
        }
        if (!known) {
            if (f->readOffsetCount < FIXTURE_MAX_READ_OFFSETS) {
                f->readOffsets[f->readOffsetCount++] = offset;
            } else {
                f->readOffsetOverflow = true;
            }
        }
    }

    if (!nvIsInGspAperture(offset) || (offset / 4u) >= FIXTURE_WORDS) {
        f->apertureViolation = true;
        return 0;
    }

    if (offset == ABS_DMATRFCMD && !f->dmaNeverIdle) {
        NVU32 seen = f->dmaReadsSinceCmd++;
        if (seen >= f->dmaDelayPolls) {
            return f->regs[offset / 4u]
                   | ((NVU32)1u << NV_PFALCON_FALCON_DMATRFCMD_IDLE_SHIFT);
        }
    }

    return f->regs[offset / 4u];
}

static void fixtureWrite32(void *ctx, NVU32 offset, NVU32 value)
{
    Fixture *f = (Fixture *)ctx;

    if (!nvIsInGspAperture(offset) || (offset / 4u) >= FIXTURE_WORDS) {
        f->apertureViolation = true;
        return;
    }

    if (f->writeCount < FIXTURE_MAX_WRITES) {
        f->writes[f->writeCount].offset = offset;
        f->writes[f->writeCount].value  = value;
    } else {
        f->journalOverflow = true;
    }

    //
    // CMD is the trigger, so a write to it restarts this chunk's busy window.
    //
    if (offset == ABS_DMATRFCMD)
        f->dmaReadsSinceCmd = 0u;

    f->writeCount++;
    f->regs[offset / 4u] = value;
}

//
// A valid image of `chunks` 256-byte chunks at the address the trace used.
//
static NVGspFirmwareImage fixtureImage(NVU32 chunks, NVU32 imemOffset)
{
    NVGspFirmwareImage img;
    img.physAddrLo = OBS_IMAGE_PHYS_LO;
    img.physAddrHi = 0u;
    img.sizeBytes  = chunks * DMA_CHUNK;
    img.imemOffset = imemOffset;
    img.valid      = true;
    return img;
}

//
// The same image at an ARBITRARY physical base, including one above 4 GiB.
//
// fixtureImage() above pins physAddrHi = 0 because that is what the trace
// witnessed. That is the correct default, but it means every DMA assertion in
// this file ran in the one region where DMATRFBASE1 is zero -- so nothing here
// ever exercised the high half of the address at all. See testDmaHighAddress().
//
static NVGspFirmwareImage fixtureImageAt(NVU32 physHi,
                                         NVU32 physLo,
                                         NVU32 chunks,
                                         NVU32 imemOffset)
{
    NVGspFirmwareImage img;
    img.physAddrLo = physLo;
    img.physAddrHi = physHi;
    img.sizeBytes  = chunks * DMA_CHUNK;
    img.imemOffset = imemOffset;
    img.valid      = true;
    return img;
}

static void fixtureReset(NVU32 cpuctl, NVU32 hwcfg2, NVU32 riscvCpuctl,
                        NVU32 privErr, NVU32 hubErr)
{
    std::memset(&gFix, 0, sizeof(gFix));

    gFix.regs[ABS_FALCON_CPUCTL / 4u] = cpuctl;
    gFix.regs[ABS_FALCON_HWCFG2 / 4u] = hwcfg2;
    gFix.regs[ABS_RISCV_CPUCTL  / 4u] = riscvCpuctl;
    gFix.regs[ABS_RISCV_PRIV_ERR / 4u] = privErr;
    gFix.regs[ABS_RISCV_HUB_ERR  / 4u] = hubErr;
}

static NVGspBootIo fixtureIo(bool allowWrites)
{
    NVGspBootIo io;
    io.read32      = fixtureRead32;
    io.write32     = fixtureWrite32;
    io.ctx         = &gFix;
    io.allowWrites = allowWrites;
    return io;
}

//
// A healthy GA107 as the trace found it: core halted, RISC-V capable, scrub
// already done, no error latches, and a core that will report ACTIVE once
// released.
//
static void fixtureResetHealthy(void)
{
    fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE,
                 RISCV_CPUCTL_ACTIVE, 0u, 0u);
}

static void checkJournal(const char *label, NVU32 index,
                        NVU32 expectedOffset, NVU32 expectedValue)
{
    gChecks++;

    if (index >= gFix.writeCount || index >= FIXTURE_MAX_WRITES) {
        gFailures++;
        std::printf("  FAIL  %-50s no write at journal index %u (only %u)\n",
                    label, index, gFix.writeCount);
        return;
    }

    const WriteRec *w = &gFix.writes[index];
    if (w->offset == expectedOffset && w->value == expectedValue) {
        std::printf("  ok    %-50s [%u] 0x%X <- 0x%X\n",
                    label, index, w->offset, w->value);
        return;
    }

    gFailures++;
    std::printf("  FAIL  %-50s [%u] got 0x%X <- 0x%X, "
                "expected 0x%X <- 0x%X\n",
                label, index, w->offset, w->value,
                expectedOffset, expectedValue);
}


// ===========================================================================
// 1. Address translation -- the fixture's literals against the machine's
//    derived addresses.
// ===========================================================================

static void testAddressAgreement(void)
{
    section("address agreement (trace literals vs derived addresses)");

    checkU32("falcon CPUCTL",
             nvGspFalconReg(NV_PFALCON_FALCON_CPUCTL), ABS_FALCON_CPUCTL);
    checkU32("falcon HWCFG2",
             nvGspFalconReg(NV_PFALCON_FALCON_HWCFG2), ABS_FALCON_HWCFG2);
    checkU32("falcon ENGINE (reset)",
             nvGspFalconReg(NV_PFALCON_FALCON_ENGINE), ABS_FALCON_ENGINE);

    checkU32("brom PARAADDR(0)",
             nvGspRiscvReg(NV_PFALCON2_FALCON_BROM_PARAADDR(0)),
             ABS_BROM_PARAADDR0);
    checkU32("brom ENGIDMASK",
             nvGspRiscvReg(NV_PFALCON2_FALCON_BROM_ENGIDMASK),
             ABS_BROM_ENGIDMASK);
    checkU32("brom CURR_UCODE_ID",
             nvGspRiscvReg(NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID),
             ABS_BROM_UCODE_ID);
    checkU32("brom MOD_SEL",
             nvGspRiscvReg(NV_PFALCON2_FALCON_MOD_SEL), ABS_BROM_MOD_SEL);

    checkU32("riscv BCR_CTRL",
             nvGspRiscvReg(NV_PRISCV_RISCV_BCR_CTRL), ABS_RISCV_BCR_CTRL);
    checkU32("riscv CPUCTL",
             nvGspRiscvReg(NV_PRISCV_RISCV_CPUCTL), ABS_RISCV_CPUCTL);
    checkU32("riscv PRIV_ERR_STAT",
             nvGspRiscvReg(NV_PRISCV_RISCV_PRIV_ERR_STAT), ABS_RISCV_PRIV_ERR);
    checkU32("riscv HUB_ERR_STAT",
             nvGspRiscvReg(NV_PRISCV_RISCV_HUB_ERR_STAT), ABS_RISCV_HUB_ERR);

    //
    // The Falcon CPUCTL and the RISC-V CPUCTL are DIFFERENT registers that
    // share a name. Conflating them is the single easiest way to poll the
    // wrong one forever.
    //
    checkBool("falcon and riscv CPUCTL are distinct",
              ABS_FALCON_CPUCTL != ABS_RISCV_CPUCTL, true);

    checkBool("every trace address is in the GSP aperture",
              nvIsInGspAperture(ABS_FALCON_CPUCTL)
              && nvIsInGspAperture(ABS_FALCON_HWCFG2)
              && nvIsInGspAperture(ABS_FALCON_ENGINE)
              && nvIsInGspAperture(ABS_BROM_PARAADDR0)
              && nvIsInGspAperture(ABS_BROM_ENGIDMASK)
              && nvIsInGspAperture(ABS_BROM_UCODE_ID)
              && nvIsInGspAperture(ABS_BROM_MOD_SEL)
              && nvIsInGspAperture(ABS_RISCV_BCR_CTRL)
              && nvIsInGspAperture(ABS_RISCV_CPUCTL)
              && nvIsInGspAperture(ABS_RISCV_PRIV_ERR)
              && nvIsInGspAperture(ABS_RISCV_HUB_ERR), true);

    //
    // The scrubbing sense, stated as a test rather than a comment. 0x47F7 is
    // the value the hardware actually returned.
    //
    checkBool("observed HWCFG2 0x47F7 means scrub DONE",
              nvFalconScrubbingDone(OBS_HWCFG2_SCRUB_DONE), true);
    checkBool("bit 12 SET means scrub still RUNNING",
              nvFalconScrubbingDone(OBS_HWCFG2_SCRUBBING), false);
    checkBool("both values are still RISC-V capable",
              nvFalconIsRiscvCapable(OBS_HWCFG2_SCRUB_DONE)
              && nvFalconIsRiscvCapable(OBS_HWCFG2_SCRUBBING), true);
}


// ===========================================================================
// 2. Observed parameters.
// ===========================================================================

static void testObservedParams(void)
{
    section("observed boot parameters (spec section 3.4)");

    NVGspBootParams p = nvGspBootObservedParams();

    checkU32("PARAADDR(0)",      p.paraAddr0,  0x000005A4u);
    checkU32("ENGIDMASK",        p.engIdMask,  0x00000400u);
    checkU32("CURR_UCODE_ID",    p.ucodeId,    0x00000009u);
    checkU32("MOD_SEL algorithm", p.modSelAlgo,
             (NVU32)NV_PFALCON2_FALCON_MOD_SEL_ALGO_RSA3K);

    //
    // 0x111 is what the trace writes. It decomposes as VALID | CORE_SELECT |
    // BRFETCH, and the header builds it from the two bits NVFalcon.hpp names.
    //
    checkU32("BCR_CTRL start value", p.bcrCtrl, 0x00000111u);

    checkBool("firmware load NOT skipped by default",
              p.skipFirmwareLoad, false);
    checkBool("poll budgets are nonzero",
              p.maxScrubPolls > 0u && p.maxActivePolls > 0u, true);
}


// ===========================================================================
// 3. The dry-run gate. This is the test that lets the kext be called
//    read-only in writing.
// ===========================================================================

static void testDryRunGate(void)
{
    section("dry-run gate (allowWrites = false)");

    fixtureResetHealthy();

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(false), nvGspBootObservedParams());

    NVGspBootResult r = nvGspBootRun(&m, 1000u);

    checkResult("result", r, NVGSP_RESULT_ERR_WRITES_DISABLED);
    checkU32("machine write counter", m.writeCount, 0u);

    //
    // The independent witness. m.writeCount is the machine's own bookkeeping;
    // gFix.writeCount counts stores that actually reached the backing store.
    // Both must be zero, and only the second one proves nothing escaped.
    //
    checkU32("stores that reached the fixture", gFix.writeCount, 0u);

    //
    // The first write it WOULD have issued is the reset assert. Reporting the
    // exact offset is what makes the dry run actionable rather than just a
    // refusal.
    //
    checkU32("reported first blocked write", m.faultingOffset,
             ABS_FALCON_ENGINE);

    checkBool("reads still happened", gFix.readCount > 0u, true);
    checkBool("not treated as an expected end",
              nvGspBootReachedExpectedEnd(&m), false);
    checkBool("no aperture violation", gFix.apertureViolation, false);
}


// ===========================================================================
// 4. The default path: reach the Phase 5 boundary and stop honestly.
// ===========================================================================

static void testFirmwareBoundary(void)
{
    section("phase 5 boundary (default params, writes allowed)");

    fixtureResetHealthy();

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());

    NVGspBootResult r = nvGspBootRun(&m, 1000u);

    checkResult("result", r, NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED);
    checkU32("state", (NVU32)m.state, (NVU32)NVGSP_STATE_STOPPED);

    //
    // Exactly the two reset-pulse writes. If this number grows, something
    // started issuing BROM parameters without a firmware image behind them --
    // which would start a core with empty IMEM.
    //
    checkU32("writes issued", m.writeCount, 2u);
    checkU32("stores reaching the fixture", gFix.writeCount, 2u);

    checkJournal("reset assert",   0u, ABS_FALCON_ENGINE, 1u);
    checkJournal("reset deassert", 1u, ABS_FALCON_ENGINE, 0u);

    //
    // Not a failure. Stopping here is the designed end of Phase 4, and the
    // caller must be able to tell that apart from a fault.
    //
    checkBool("counts as an expected end",
              nvGspBootReachedExpectedEnd(&m), true);

    checkBool("no BROM parameter was written",
              gFix.regs[ABS_BROM_MOD_SEL / 4u] == 0u
              && gFix.regs[ABS_RISCV_BCR_CTRL / 4u] == 0u, true);

    checkBool("no aperture violation", gFix.apertureViolation, false);
    checkBool("scrub was already done, so zero polls",
              m.scrubPolls == 0u, true);
}


// ===========================================================================
// 5. The full sequence, and the ORDER within it.
// ===========================================================================

static void testFullSequenceOrder(void)
{
    section("full sequence and write order (skipFirmwareLoad = true)");

    fixtureResetHealthy();

    NVGspBootParams p = nvGspBootObservedParams();

    //
    // TEST-ONLY. On hardware this would release a RISC-V core whose IMEM was
    // never filled. It exists so the states past the Phase 5 boundary --
    // which the trace fully specifies -- can be exercised at all.
    //
    p.skipFirmwareLoad = true;

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    NVGspBootResult r = nvGspBootRun(&m, 1000u);

    checkResult("result", r, NVGSP_RESULT_RUNNING);
    checkU32("state", (NVU32)m.state, (NVU32)NVGSP_STATE_RUNNING);
    checkU32("writes issued", m.writeCount, 7u);
    checkU32("stores reaching the fixture", gFix.writeCount, 7u);
    checkBool("journal did not overflow", gFix.journalOverflow, false);

    //
    // THE ORDER TEST. Checking final register contents would pass even if
    // MOD_SEL went first, so the journal positions are the assertion.
    //
    checkJournal("1 reset assert",   0u, ABS_FALCON_ENGINE,  1u);
    checkJournal("2 reset deassert", 1u, ABS_FALCON_ENGINE,  0u);
    checkJournal("3 PARAADDR(0)",    2u, ABS_BROM_PARAADDR0, 0x000005A4u);
    checkJournal("4 ENGIDMASK",      3u, ABS_BROM_ENGIDMASK, 0x00000400u);
    checkJournal("5 CURR_UCODE_ID",  4u, ABS_BROM_UCODE_ID,  0x00000009u);
    checkJournal("6 MOD_SEL (LATCH -- must be last)",
                 5u, ABS_BROM_MOD_SEL,
                 (NVU32)NV_PFALCON2_FALCON_MOD_SEL_ALGO_RSA3K);
    checkJournal("7 BCR_CTRL (starts the core)",
                 6u, ABS_RISCV_BCR_CTRL, 0x00000111u);

    //
    // The machine's own record of where the latch landed, cross-checked
    // against the journal. These indices are 1-based write counts, so MOD_SEL
    // at journal[5] is writeCount 6.
    //
    checkU32("modSelWriteIndex", m.modSelWriteIndex, 6u);
    checkU32("bcrWriteIndex",    m.bcrWriteIndex,    7u);
    checkBool("MOD_SEL precedes BCR_CTRL",
              m.modSelWriteIndex < m.bcrWriteIndex, true);

    checkBool("expected end", nvGspBootReachedExpectedEnd(&m), true);
    checkBool("no aperture violation", gFix.apertureViolation, false);
    checkU32("riscv CPUCTL as read", m.riscvCpuctl, RISCV_CPUCTL_ACTIVE);
}


// ===========================================================================
// 6. Preconditions.
// ===========================================================================

static void testPreconditions(void)
{
    section("preconditions refuse before any write");

    // Core not halted -- something else already started it.
    {
        fixtureReset(0u, OBS_HWCFG2_SCRUB_DONE, RISCV_CPUCTL_ACTIVE, 0u, 0u);

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());
        NVGspBootResult r = nvGspBootRun(&m, 1000u);

        checkResult("not halted", r, NVGSP_RESULT_ERR_NOT_HALTED);
        checkU32("  no writes issued", gFix.writeCount, 0u);
        checkBool("  not an expected end",
                  nvGspBootReachedExpectedEnd(&m), false);
    }

    // Not RISC-V capable -- bit 10 clear.
    {
        fixtureReset(OBS_CPUCTL_HALTED,
                     OBS_HWCFG2_SCRUB_DONE & ~0x400u,
                     RISCV_CPUCTL_ACTIVE, 0u, 0u);

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());
        NVGspBootResult r = nvGspBootRun(&m, 1000u);

        checkResult("not riscv capable", r,
                    NVGSP_RESULT_ERR_NOT_RISCV_CAPABLE);
        checkU32("  no writes issued", gFix.writeCount, 0u);
    }

    // No I/O callbacks at all.
    {
        fixtureResetHealthy();

        NVGspBootIo io = fixtureIo(true);
        io.read32 = 0;

        NVGspBoot m;
        nvGspBootInit(&m, io, nvGspBootObservedParams());
        checkResult("null read callback", nvGspBootRun(&m, 1000u),
                    NVGSP_RESULT_ERR_NO_IO);
    }

    {
        fixtureResetHealthy();

        NVGspBootIo io = fixtureIo(true);
        io.write32 = 0;

        NVGspBoot m;
        nvGspBootInit(&m, io, nvGspBootObservedParams());
        checkResult("null write callback", nvGspBootRun(&m, 1000u),
                    NVGSP_RESULT_ERR_NO_IO);
        checkU32("  nothing landed", gFix.writeCount, 0u);
    }
}


// ===========================================================================
// 6b. The aperture guard, which was the one terminal result no test reached.
//
// Measured 2026-08-17 by set-differencing the NVGSP_RESULT_* names spelled in
// NVGspBoot.hpp against those spelled in this file: fourteen in the header,
// thirteen here, and the missing one was ERR_APERTURE_VIOLATION. It is produced
// at NVGspBoot.hpp:365 (read) and :380 (write), so BOTH guards existed and
// NEITHER was exercised -- deleting either `if (!nvIsInGspAperture(absolute))`
// block left this suite green.
//
// That guard is not decorative. NVFalcon.hpp's whole reason for carrying
// nvIsInGspAperture() is that the RISC-V block sits exactly one 4 KiB page above
// the Falcon block, so an address 0x1000 low is a VALID address into the wrong
// engine: it does not fault, it reads and writes real hardware silently. The
// fixture callbacks police the aperture themselves and set gFix.apertureViolation
// if one ever reaches them, which is what lets the checks below distinguish
// "the machine refused" from "the machine passed it through and the fixture
// caught it" -- two outcomes that produce the same absence of corruption here
// and completely different behaviour on silicon.
//
// The machine cannot be steered outside the aperture through its public API --
// every address it computes comes from nvGspFalconReg()/nvGspRiscvReg() over
// constants. So these drive nvGspBootRead()/nvGspBootWrite() directly, which is
// the same thing testPreconditions() does when it nulls a callback.
// ===========================================================================

static void testApertureGuard(void)
{
    section("aperture guard refuses outside NV_PGSP_BASE..NV_PGSP_LIMIT");

    //
    // The two edges first, as plain predicate calls. Containment is a one-sided
    // property: a guard that only ever sees inside addresses is satisfied by a
    // window widened in either direction, so both edges are pinned AND the
    // first address past each is pinned as refused.
    //
    checkBool("base is inside",       nvIsInGspAperture(NV_PGSP_BASE), true);
    checkBool("limit is inside",      nvIsInGspAperture(NV_PGSP_LIMIT), true);
    checkBool("base-1 is outside",    nvIsInGspAperture(NV_PGSP_BASE - 1u), false);
    checkBool("limit+1 is outside",   nvIsInGspAperture(NV_PGSP_LIMIT + 1u), false);

    //
    // A READ below the window. 0x0010F000 is one page below NV_PGSP_BASE, which
    // is the realistic shape of the bug -- a base constant short by a page --
    // rather than a wild pointer.
    //
    {
        fixtureResetHealthy();

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());

        NVU32 value = 0xDEADBEEFu;
        bool  ok    = nvGspBootRead(&m, NV_PGSP_BASE - 0x1000u, &value);

        checkBool("read below window refused", ok, false);
        checkResult("  result", m.result,
                    NVGSP_RESULT_ERR_APERTURE_VIOLATION);
        checkU32("  faulting offset recorded", m.faultingOffset,
                 NV_PGSP_BASE - 0x1000u);
        checkU32("  state is STOPPED", (NVU32)m.state,
                 (NVU32)NVGSP_STATE_STOPPED);

        //
        // The guard must fire BEFORE the callback, not after. If it fired after,
        // the fixture would have seen the access and every check above would
        // still pass -- on hardware that is a real read of a real Falcon
        // register. readCount is the separating observation.
        //
        checkU32("  callback never invoked", gFix.readCount, 0u);
        checkBool("  fixture saw no violation", gFix.apertureViolation, false);

        //
        // Terminal, and NOT the expected end. A violation reported as a
        // legitimate stopping point would read as success in the kext's
        // published properties.
        //
        checkBool("  not an expected end",
                  nvGspBootReachedExpectedEnd(&m), false);
    }

    //
    // A WRITE above the window, and the write guard is a separate block in the
    // header from the read guard, so it needs its own case. Deleting one and
    // keeping the other must not leave this suite green.
    //
    {
        fixtureResetHealthy();

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());

        bool ok = nvGspBootWrite(&m, NV_PGSP_LIMIT + 1u, 0x1u);

        checkBool("write above window refused", ok, false);
        checkResult("  result", m.result,
                    NVGSP_RESULT_ERR_APERTURE_VIOLATION);
        checkU32("  faulting offset recorded", m.faultingOffset,
                 NV_PGSP_LIMIT + 1u);
        checkU32("  nothing landed", gFix.writeCount, 0u);
        checkBool("  fixture saw no violation", gFix.apertureViolation, false);
        checkU32("  machine's own write count", m.writeCount, 0u);
    }

    //
    // Precedence. A machine with no write callback AND an out-of-window address
    // has two reasons to refuse, and the header checks io.write32 first. Pinning
    // which one wins keeps the two failures from being silently interchangeable
    // -- ERR_NO_IO says the driver wired itself wrong, APERTURE_VIOLATION says a
    // register constant is wrong, and they send a reader to different files.
    //
    {
        fixtureResetHealthy();

        NVGspBootIo io = fixtureIo(true);
        io.write32 = 0;

        NVGspBoot m;
        nvGspBootInit(&m, io, nvGspBootObservedParams());

        checkBool("null callback beats bad address",
                  nvGspBootWrite(&m, NV_PGSP_LIMIT + 1u, 0x1u), false);
        checkResult("  result is NO_IO, not APERTURE", m.result,
                    NVGSP_RESULT_ERR_NO_IO);
    }

    //
    // The writes-disabled gate sits BELOW the aperture check in the header, so a
    // dry-run machine handed a bad address must still report the violation. If
    // this returned ERR_WRITES_DISABLED, a wrong base constant would be
    // invisible on every dry run -- which is the only way this code has ever
    // been executed against real silicon.
    //
    {
        fixtureResetHealthy();

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(false), nvGspBootObservedParams());

        checkBool("dry run still refuses bad address",
                  nvGspBootWrite(&m, NV_PGSP_BASE - 1u, 0x1u), false);
        checkResult("  aperture beats writes-disabled", m.result,
                    NVGSP_RESULT_ERR_APERTURE_VIOLATION);
        checkU32("  faulting offset recorded", m.faultingOffset,
                 NV_PGSP_BASE - 1u);
    }

    //
    // Negative control. The same calls with an address INSIDE the window must
    // succeed and must reach the fixture -- otherwise every check above would
    // pass against a guard that refuses everything, which is a different bug
    // wearing the same green.
    //
    {
        fixtureResetHealthy();

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());

        NVU32 value = 0u;
        bool  ok    = nvGspBootRead(&m, ABS_FALCON_CPUCTL, &value);

        checkBool("in-window read allowed", ok, true);
        checkU32("  callback WAS invoked", gFix.readCount, 1u);
        checkU32("  value came from the fixture", value, OBS_CPUCTL_HALTED);
        checkResult("  machine still running", m.result,
                    NVGSP_RESULT_INPROGRESS);

        checkBool("in-window write allowed",
                  nvGspBootWrite(&m, ABS_FALCON_ENGINE, 1u), true);
        checkU32("  write landed", gFix.writeCount, 1u);
        checkJournal("  journal records it", 0u, ABS_FALCON_ENGINE, 1u);
    }
}


// ===========================================================================
// 7. Timeouts, and the priority between them.
// ===========================================================================

static void testTimeouts(void)
{
    section("timeouts and fault priority");

    // Scrub never completes: bit 12 stays SET.
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUBBING,
                     RISCV_CPUCTL_ACTIVE, 0u, 0u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.maxScrubPolls = 7u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);
        NVGspBootResult r = nvGspBootRun(&m, 1000u);

        checkResult("scrub timeout", r, NVGSP_RESULT_ERR_SCRUB_TIMEOUT);
        checkU32("  polled exactly the budget", m.scrubPolls, 7u);
        checkU32("  reset pulse still issued", m.writeCount, 2u);
        checkU32("  last HWCFG2 retained", m.hwcfg2, OBS_HWCFG2_SCRUBBING);
        checkBool("  not an expected end",
                  nvGspBootReachedExpectedEnd(&m), false);
    }

    // Core released but never reports ACTIVE, and no error latch fires.
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u, 0u, 0u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);
        NVGspBootResult r = nvGspBootRun(&m, 1000u);

        checkResult("active timeout", r, NVGSP_RESULT_ERR_ACTIVE_TIMEOUT);
        checkU32("  polled exactly the budget", m.activePolls, 5u);
        checkU32("  full write sequence still issued", m.writeCount, 7u);
    }

    //
    // A fault must WIN over the timeout. Reporting ERR_ACTIVE_TIMEOUT when
    // PRIV_ERR_STAT is nonzero would hide the only diagnostic GSP has.
    //
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u,
                     0x00000001u, 0u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);
        NVGspBootResult r = nvGspBootRun(&m, 1000u);

        checkResult("priv error beats timeout", r,
                    NVGSP_RESULT_ERR_RISCV_FAULT);
        checkU32("  PRIV_ERR_STAT surfaced", m.privErrStat, 0x00000001u);
        checkU32("  detected on the first poll", m.activePolls, 0u);
    }

    // Same for the hub latch, which has a different cause.
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u,
                     0u, 0x00000002u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("hub error beats timeout", nvGspBootRun(&m, 1000u),
                    NVGSP_RESULT_ERR_RISCV_FAULT);
        checkU32("  HUB_ERR_STAT surfaced", m.hubErrStat, 0x00000002u);
    }

    //
    // An already-active core short-circuits: no fault check is needed because
    // ACTIVE_STAT is checked first.
    //
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE,
                     RISCV_CPUCTL_ACTIVE, 0x00000001u, 0u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("active wins over a stale latch",
                    nvGspBootRun(&m, 1000u), NVGSP_RESULT_RUNNING);
    }
}


// ===========================================================================
// 7b. The RISC-V error latches, SPLIT into a fault and an unreadable pair.
// ===========================================================================
//
// nvGspBootCheckRiscvErrors() used to stop on nvRiscvHasError(), which is a
// one-line wrapper answering `true` for FOUR of nvRiscvErrorVerdict()'s five
// codes at once. A boot halted because a half did not DECODE was therefore
// reported as ERR_RISCV_FAULT -- a claim about the card, made from evidence
// that is a claim about this driver's mapping.
//
// Section 7 above already drove the fault arm and could not see this: every
// one of its latches is a small nonzero value, and a refused read and a set
// latch both satisfy `!= NVRISCV_ERR_NONE`. That is a bool with four preimages
// in exactly the sense 3050-handoff34.md s1 records, one subsystem over.
//
// So the checks below come in PAIRS at the same construction: for each half,
// one run where that half is REFUSED and one where it carries a genuine latch,
// and the two must land on different result codes. A test that only ever
// refused would be satisfied by a machine that reports UNREADABLE for every
// nonzero reading.
//
static void testRiscvErrorSplit(void)
{
    section("7b. RISC-V error latches: fault vs. unreadable");

    //
    // Every run here has to REACH the error check, which sits below the ACTIVE
    // test in ACTIVE_WAIT -- a core reporting ACTIVE stops on RUNNING first and
    // never reads either latch. riscvCpuctl is 0 throughout for that reason,
    // and skipFirmwareLoad keeps the DMA path out of the way (3050-handoff29
    // s4a: the DMA route plus the default poll budget trips the step backstop
    // before the fixture's own budget, and the result then describes the
    // FIXTURE rather than the simulated hardware).
    //
    const NVU32 REFUSED = 0xFFFFFFFFu;

    // ---- PRIV: refused, then genuinely latched. --------------------------
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u,
                     REFUSED, 0u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("PRIV refused stops UNREADABLE, not FAULT",
                    nvGspBootRun(&m, 1000u),
                    NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE);
        checkU32("  and the verdict says so",
                 m.errVerdict, NVRISCV_ERR_UNREADABLE);
        checkBool("  both halves were read, so it is a measurement",
                  nvGspBootErrVerdictKnown(&m), true);
    }
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u,
                     0x00000001u, 0u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("PRIV latched stops FAULT",
                    nvGspBootRun(&m, 1000u), NVGSP_RESULT_ERR_RISCV_FAULT);
        checkU32("  and the verdict names the PRIV half",
                 m.errVerdict, NVRISCV_ERR_PRIV);
    }

    // ---- HUB: the same pair, so neither half is pinned by the other. -----
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u,
                     0u, REFUSED);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("HUB refused stops UNREADABLE, not FAULT",
                    nvGspBootRun(&m, 1000u),
                    NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE);
        checkU32("  and the verdict says so",
                 m.errVerdict, NVRISCV_ERR_UNREADABLE);
    }
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u,
                     0u, 0x00000002u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("HUB latched stops FAULT",
                    nvGspBootRun(&m, 1000u), NVGSP_RESULT_ERR_RISCV_FAULT);
        checkU32("  and the verdict names the HUB half",
                 m.errVerdict, NVRISCV_ERR_HUB);
    }

    //
    // Both latched. Distinct from either single case, and the reason the finer
    // three verdicts ride in errVerdict rather than in the result enum: this
    // run and the two above are all ERR_RISCV_FAULT, and only the verdict
    // separates them.
    //
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u,
                     0x00000001u, 0x00000002u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("both latched is still FAULT",
                    nvGspBootRun(&m, 1000u), NVGSP_RESULT_ERR_RISCV_FAULT);
        checkU32("  and the verdict distinguishes it from either half alone",
                 m.errVerdict, NVRISCV_ERR_BOTH);
    }

    //
    // PRECEDENCE, and this is the discriminating check in the section.
    //
    // A refused HUB read beside a GENUINE PRIV latch. Both readings are true at
    // once, and the old bool answered the same `true` either way. The half that
    // did not decode wins, because an unreadable register is a bug in this
    // driver and a set latch is a fact about the card -- handoff22 s1b's rule:
    // report the one somebody can go and fix. A machine that reported FAULT
    // here would be right about the latch and wrong about what to do next.
    //
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u,
                     0x00000001u, REFUSED);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("an unreadable half BEATS a real latch in the other",
                    nvGspBootRun(&m, 1000u),
                    NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE);
        checkU32("  and the verdict agrees with the result code",
                 m.errVerdict, NVRISCV_ERR_UNREADABLE);
        checkU32("  while the latched word is still carried for the log",
                 m.privErrStat, 0x00000001u);
    }

    //
    // The two runs where the verdict is NOT a measurement, which is the whole
    // reason nvGspBootErrVerdictKnown() exists. NVRISCV_ERR_NONE is the
    // initialiser AND the healthy reading, so a publisher reading the field
    // bare would report both of these as "checked both latches, found them
    // clear".
    //
    // The second one is the trap 3050-handoff29 s2a names: the run that
    // fabricates is the SUCCESSFUL one. ACTIVE_WAIT stops on RUNNING before the
    // error check, so a healthy boot never reads either latch.
    //
    {
        // cpuctl 0 = the HALTED bit clear, which is how section 6 spells a
        // core somebody else already started.
        fixtureReset(0u, OBS_HWCFG2_SCRUB_DONE, 0u, 0u, 0u);

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());

        checkResult("a run stopping at PROBE never reaches the latches",
                    nvGspBootRun(&m, 1000u), NVGSP_RESULT_ERR_NOT_HALTED);
        checkBool("  so the verdict is NOT known",
                  nvGspBootErrVerdictKnown(&m), false);
        checkU32("  even though the field reads as the healthy value",
                 m.errVerdict, NVRISCV_ERR_NONE);
    }
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE,
                     RISCV_CPUCTL_ACTIVE, 0u, 0u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("a HEALTHY boot is the one that stops before the latches",
                    nvGspBootRun(&m, 1000u), NVGSP_RESULT_RUNNING);
        checkBool("  so its verdict is NOT known either",
                  nvGspBootErrVerdictKnown(&m), false);
        checkBool("  and neither latch bit is set in the mask",
                  nvGspBootWasRead(&m, NVGSP_READ_PRIV_ERR_STAT)
                  || nvGspBootWasRead(&m, NVGSP_READ_HUB_ERR_STAT), false);
    }

    //
    // A null machine answers false, same direction as nvGspBootWasRead().
    //
    checkBool("a null machine has not measured a verdict",
              nvGspBootErrVerdictKnown(0), false);

    //
    // The CONJUNCTION, driven over readMask directly.
    //
    // Every run above that reads PRIV also reads HUB -- the refusal VALUE comes
    // back through a read that SUCCEEDED, so both bits land together and no
    // run in this file separates the two halves. A nvGspBootErrVerdictKnown()
    // written as a single-bit test would therefore pass every check above it,
    // which is the vacuity direction: the predicate would be measuring nothing
    // it claims to measure while printing ok.
    //
    // So the mask is set by hand. This is the one place in the section that
    // drives the predicate as a pure function rather than through a run.
    //
    {
        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());

        m.readMask = NVGSP_READ_PRIV_ERR_STAT;
        checkBool("PRIV alone is not a verdict",
                  nvGspBootErrVerdictKnown(&m), false);

        m.readMask = NVGSP_READ_HUB_ERR_STAT;
        checkBool("HUB alone is not a verdict either",
                  nvGspBootErrVerdictKnown(&m), false);

        m.readMask = NVGSP_READ_PRIV_ERR_STAT | NVGSP_READ_HUB_ERR_STAT;
        checkBool("both together are",
                  nvGspBootErrVerdictKnown(&m), true);

        //
        // And the far-side bit: every OTHER bit set, both of these clear, must
        // still answer false. A predicate written as a nonzero test over the
        // whole mask would pass the three checks above and fail this one.
        //
        m.readMask = (NVU32)~(NVGSP_READ_PRIV_ERR_STAT
                              | NVGSP_READ_HUB_ERR_STAT);
        checkBool("every other bit set is still not a verdict",
                  nvGspBootErrVerdictKnown(&m), false);
    }

    //
    // The new result code needs its own NAME. A code added to the enum without
    // an arm in the switch falls to `default:` and publishes "UNKNOWN", which
    // is handoff20 s1's defect exactly: honest, and useless.
    //
    checkBool("the new result code has its own name",
              std::strcmp(nvGspBootResultName(
                              NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE),
                          "ERR_RISCV_ERR_UNREADABLE") == 0, true);
    checkBool("and it is distinct from the fault it was split out of",
              std::strcmp(nvGspBootResultName(
                              NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE),
                          nvGspBootResultName(
                              NVGSP_RESULT_ERR_RISCV_FAULT)) != 0, true);
}


// ===========================================================================
// 8. The step-budget backstop, and per-step driving.
// ===========================================================================

static void testStepDriving(void)
{
    section("step budget and single-stepping");

    //
    // The runaway-loop backstop, distinct from the poll budgets so a caller
    // can tell "the GPU never responded" from "the loop itself ran away".
    //
    {
        fixtureResetHealthy();

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());

        checkResult("step budget trips", nvGspBootRun(&m, 3u),
                    NVGSP_RESULT_ERR_STEP_BUDGET);
        checkU32("  stopped, not running", (NVU32)m.state,
                 (NVU32)NVGSP_STATE_STOPPED);
    }

    //
    // The kext drives one transition per call so it can sleep in between.
    // This walks the exact state order that loop will see.
    //
    {
        fixtureResetHealthy();

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkU32("state before any step", (NVU32)m.state,
                 (NVU32)NVGSP_STATE_INIT);

        const NVGspBootState expected[] = {
            NVGSP_STATE_PROBE,
            NVGSP_STATE_RESET_ASSERT,
            NVGSP_STATE_RESET_DEASSERT,
            NVGSP_STATE_SCRUB_WAIT,
            NVGSP_STATE_FIRMWARE_LOAD,
            NVGSP_STATE_BROM_PARAMS,
            NVGSP_STATE_CORE_START,
            NVGSP_STATE_ACTIVE_WAIT
        };
        const NVU32 expectedCount =
            (NVU32)(sizeof(expected) / sizeof(expected[0]));

        bool orderHeld = true;
        NVU32 taken = 0u;

        for (NVU32 i = 0u; i < expectedCount; ++i) {
            if (!nvGspBootStep(&m)) {
                orderHeld = false;
                break;
            }
            taken++;
            if (m.state != expected[i]) {
                orderHeld = false;
                std::printf("        at step %u: state %s, expected %s\n",
                            i, nvGspBootStateName(m.state),
                            nvGspBootStateName(expected[i]));
                break;
            }
        }

        checkU32("transitions taken", taken, expectedCount);
        checkBool("state order matches the spec sequence", orderHeld, true);

        // One more step lands on the terminal state and reports done.
        checkBool("final step reports terminal", nvGspBootStep(&m), false);
        checkResult("  and the result", m.result, NVGSP_RESULT_RUNNING);
        checkBool("  further steps are inert", nvGspBootStep(&m), false);
    }
}


// ===========================================================================
// 9. Naming, so a failure log is readable.
// ===========================================================================

static void testNames(void)
{
    section("diagnostic names");

    checkBool("RUNNING result name",
              std::strcmp(nvGspBootResultName(NVGSP_RESULT_RUNNING),
                          "RUNNING") == 0, true);
    checkBool("FIRMWARE_NOT_IMPLEMENTED result name",
              std::strcmp(
                  nvGspBootResultName(NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED),
                  "FIRMWARE_NOT_IMPLEMENTED") == 0, true);
    checkBool("WRITES_DISABLED result name",
              std::strcmp(
                  nvGspBootResultName(NVGSP_RESULT_ERR_WRITES_DISABLED),
                  "ERR_WRITES_DISABLED") == 0, true);
    checkBool("BROM_PARAMS state name",
              std::strcmp(nvGspBootStateName(NVGSP_STATE_BROM_PARAMS),
                          "BROM_PARAMS") == 0, true);

    //
    // No state or result may print UNKNOWN -- an unnamed enum member in a log
    // is a diagnostic that says nothing.
    //
    bool allNamed = true;
    for (int s = NVGSP_STATE_INIT; s <= NVGSP_STATE_STOPPED; ++s) {
        if (std::strcmp(nvGspBootStateName((NVGspBootState)s),
                        "UNKNOWN") == 0) {
            allNamed = false;
            std::printf("        state %d prints UNKNOWN\n", s);
        }
    }
    for (int r = NVGSP_RESULT_INPROGRESS; r <= NVGSP_RESULT_ERR_STEP_BUDGET;
         ++r) {
        if (std::strcmp(nvGspBootResultName((NVGspBootResult)r),
                        "UNKNOWN") == 0) {
            allNamed = false;
            std::printf("        result %d prints UNKNOWN\n", r);
        }
    }
    checkBool("every state and result has a name", allNamed, true);
}


// ===========================================================================
// 10. Phase 5: the DMA transfer itself.
//
// These are the tests handoff5.md section 6 asked for. The fixture's read32
// simulates the engine: after a write to ABS_DMATRFCMD it serves
// dmaDelayPolls non-idle reads and then sets IDLE, so a drain that never
// polls would hang here rather than pass.
// ===========================================================================

//
// One chunk, end to end. The whole point is the JOURNAL, not the final
// register contents: FBIF before DMATRFBASE, and MOFFS/FBOFFS before CMD.
// Checking final values would pass even if CMD were written first, which on
// hardware triggers a transfer using the previous chunk's offsets.
//
static void testDmaSingleChunk(void)
{
    section("DMA: one chunk, full write order");

    fixtureResetHealthy();

    NVGspBootParams p = nvGspBootObservedParams();
    p.image = fixtureImage(1u, 0x100u);

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    NVGspBootResult r = nvGspBootRun(&m, 1000u);

    checkResult("result", r, NVGSP_RESULT_RUNNING);
    checkU32("state", (NVU32)m.state, (NVU32)NVGSP_STATE_RUNNING);
    checkU32("chunks planned", m.chunkCount, 1u);
    checkU32("chunks completed", m.chunksDone, 1u);

    //
    // 2 reset + 2 FBIF + 2 DMA base + 3 per chunk + 4 BROM + 1 BCR = 14.
    //
    checkU32("writes issued", m.writeCount, 14u);
    checkU32("stores reaching the fixture", gFix.writeCount, 14u);
    checkBool("journal did not overflow", gFix.journalOverflow, false);
    checkBool("no aperture violation", gFix.apertureViolation, false);

    checkJournal("1 reset assert",    0u, ABS_FALCON_ENGINE,  1u);
    checkJournal("2 reset deassert",  1u, ABS_FALCON_ENGINE,  0u);

    //
    // FBIF must precede the transfer. TRANSCFG selects the aperture, CTL
    // grants physical-no-context permission; granting permission first opens
    // a window where a physical DMA is legal against an unconfigured target.
    //
    checkJournal("3 FBIF TRANSCFG(0) (aperture first)",
                 2u, ABS_FBIF_TRANSCFG0, OBS_FBIF_TRANSCFG0);
    checkJournal("4 FBIF CTL (permission second)",
                 3u, ABS_FBIF_CTL, OBS_FBIF_CTL);

    checkJournal("5 DMATRFBASE (physLo >> 8)",
                 4u, ABS_DMATRFBASE, OBS_DMATRFBASE_VALUE);
    checkJournal("6 DMATRFBASE1 (written even when zero)",
                 5u, ABS_DMATRFBASE1, 0u);

    checkJournal("7 DMATRFMOFFS (imem offset)",
                 6u, ABS_DMATRFMOFFS, 0x100u);
    checkJournal("8 DMATRFFBOFFS (chunk offset)",
                 7u, ABS_DMATRFFBOFFS, 0u);
    checkJournal("9 DMATRFCMD (TRIGGER -- must be last)",
                 8u, ABS_DMATRFCMD, OBS_DMATRFCMD_VALUE);

    checkJournal("10 PARAADDR(0)",   9u,  ABS_BROM_PARAADDR0, p.paraAddr0);
    checkJournal("11 ENGIDMASK",     10u, ABS_BROM_ENGIDMASK, p.engIdMask);
    checkJournal("12 CURR_UCODE_ID", 11u, ABS_BROM_UCODE_ID,  p.ucodeId);
    checkJournal("13 MOD_SEL (latch, after DMA)",
                 12u, ABS_BROM_MOD_SEL, p.modSelAlgo);
    checkJournal("14 BCR_CTRL (starts the core)",
                 13u, ABS_RISCV_BCR_CTRL, p.bcrCtrl);
}


//
// THE HIGH HALF OF THE ADDRESS. This test exists because nothing else in this
// file exercised it.
//
// Every other DMA test here uses fixtureImage(), which pins physAddrHi = 0
// because that is what the trace witnessed (OBS_IMAGE_PHYS_LO 0xFFEEE200 sits
// below 4 GiB). The consequence is that DMATRFBASE1 was only ever asserted as
// zero -- the "written even when zero" check in testDmaSingleChunk passes for
// a machine that computes BASE1 correctly AND for one that never computes it
// at all. The high half of nvGspDmaTrfBase() was in the same position: with
// physHi = 0 the `| (physHi << 24)` term vanishes, so a dropped or wrongly
// shifted term could not be seen.
//
// The values below are asserted as ABSOLUTE NUMBERS worked out by hand from
// the header's documented reading, not by re-running the header's own
// arithmetic. Calling nvGspDmaTrfBase() to compute the expected value would
// make this test agree with the implementation by construction.
//
//   physical base = 0x0000_0001_2345_6700   (physHi = 1, physLo = 0x23456700)
//
//   DMATRFBASE  = (physLo >> 8) | (physHi << 24)
//               = 0x00234567  |  0x01000000
//               = 0x01234567                    -- physical bits 39:8
//
//   DMATRFBASE1 = (physHi >> 8) & 0x1FF = 0x000
//
// READ THIS BEFORE "FIXING" THE BASE1 EXPECTATION. BASE1 continues the
// SHIFTED address rather than overlaying the unshifted one: the mask is
// applied AFTER the >> 8, matching NVIDIA's own driver at
// reference/.../kernel_gsp_falcon_ga102.c:123, `NvU64_HI32(addr >> 8) & 0x1FF`.
// So BASE1 carries physical bit 40 upward, and physHi = 1 contributes NOTHING
// to it -- the whole of physHi's low 8 bits is already in BASE via << 24.
//
// The earlier form masked the unshifted high word (`physHi & 0x1FF`), which
// overlapped bits BASE already carried and would print 0x001 here. It was
// corrected in NVGspBoot.hpp (see the derivation at NVGspBoot.hpp:137-151);
// the two forms are identical for every address below 4 GiB, which is exactly
// why the MMIO trace could not distinguish them and why this test exists.
//
// These assertions pin the corrected arithmetic at three addresses chosen so
// that no single mistake passes all three: one where the high word reaches
// BASE only, one where it reaches BASE1 only, and one where it reaches both.
//
static void testDmaHighAddress(void)
{
    section("DMA: physical base above 4 GiB (BASE1 nonzero)");

    fixtureResetHealthy();

    NVGspBootParams p = nvGspBootObservedParams();
    p.image = fixtureImageAt(0x1u, 0x23456700u, 1u, 0x100u);

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    NVGspBootResult r = nvGspBootRun(&m, 1000u);

    checkResult("result", r, NVGSP_RESULT_RUNNING);
    checkU32("state", (NVU32)m.state, (NVU32)NVGSP_STATE_RUNNING);
    checkU32("chunks completed", m.chunksDone, 1u);

    // Same 14-write shape as the single-chunk case: a high address must not
    // change the sequence, only the values.
    checkU32("writes issued", m.writeCount, 14u);
    checkBool("journal did not overflow", gFix.journalOverflow, false);
    checkBool("no aperture violation", gFix.apertureViolation, false);

    //
    // The high half reaches BASE. 0x01234567 differs from the low-only value
    // 0x00234567 in exactly the bits `physHi << 24` contributes, so a dropped
    // high term fails here and nowhere else in this file.
    //
    checkJournal("DMATRFBASE carries the high half",
                 4u, ABS_DMATRFBASE, 0x01234567u);

    //
    // ...and does NOT bleed into BASE1. This is the assertion that fails under
    // the superseded `physHi & 0x1FF` form, which produced 0x001 here while
    // BASE was already carrying the same bits.
    //
    checkJournal("DMATRFBASE1 stays zero when physHi fits in BASE",
                 5u, ABS_DMATRFBASE1, 0x000u);

    // The rest of the sequence is unchanged by the address.
    checkJournal("DMATRFMOFFS",  6u, ABS_DMATRFMOFFS,  0x100u);
    checkJournal("DMATRFFBOFFS", 7u, ABS_DMATRFFBOFFS, 0u);
    checkJournal("DMATRFCMD still last",
                 8u, ABS_DMATRFCMD, OBS_DMATRFCMD_VALUE);
    checkJournal("BCR_CTRL still last overall",
                 13u, ABS_RISCV_BCR_CTRL, p.bcrCtrl);

    //
    // A second base, chosen so BASE1 uses more than its lowest bit and so the
    // two halves cannot be swapped without failing. physHi = 0x1FF is the top
    // of the 9-bit field.
    //
    fixtureResetHealthy();

    NVGspBootParams p2 = nvGspBootObservedParams();
    p2.image = fixtureImageAt(0x1FFu, 0xFFFFFF00u, 1u, 0u);

    NVGspBoot m2;
    nvGspBootInit(&m2, fixtureIo(true), p2);
    checkResult("top-of-field base runs",
                nvGspBootRun(&m2, 1000u), NVGSP_RESULT_RUNNING);

    //   DMATRFBASE  = (0xFFFFFF00 >> 8) | (0x1FF << 24)
    //               = 0x00FFFFFF | 0xFF000000   (0x1FF << 24 truncates to u32)
    //               = 0xFFFFFFFF
    checkJournal("DMATRFBASE at the top of the field",
                 4u, ABS_DMATRFBASE, 0xFFFFFFFFu);

    //   DMATRFBASE1 = (0x1FF >> 8) & 0x1FF = 0x001
    //
    // One, not 0x1FF. physHi's low 8 bits are spent in BASE; only its bit 8 --
    // physical bit 40 -- survives into BASE1, which is precisely what makes
    // the pair 41 bits rather than 49 under this reading.
    checkJournal("DMATRFBASE1 at the top of the field",
                 5u, ABS_DMATRFBASE1, 0x001u);

    //
    // Third case: exactly 2^40, where the high word reaches BASE1 and NOTHING
    // reaches BASE. physHi = 0x100 shifted left by 24 overflows a u32 to zero,
    // so BASE is 0 and the entire address lives in BASE1. A form that dropped
    // the >> 8 would write BASE1 = 0x100 here; one that dropped BASE1's write
    // entirely would target physical address 0.
    //
    fixtureResetHealthy();

    NVGspBootParams p3 = nvGspBootObservedParams();
    p3.image = fixtureImageAt(0x100u, 0x00000000u, 1u, 0u);

    NVGspBoot m3;
    nvGspBootInit(&m3, fixtureIo(true), p3);
    checkResult("2^40 base runs",
                nvGspBootRun(&m3, 1000u), NVGSP_RESULT_RUNNING);

    checkJournal("DMATRFBASE is zero at 2^40",
                 4u, ABS_DMATRFBASE, 0x00000000u);
    checkJournal("DMATRFBASE1 carries physical bit 40 alone",
                 5u, ABS_DMATRFBASE1, 0x001u);

    //
    // The truncation above is worth stating plainly rather than leaving in the
    // arithmetic: `physHi << 24` on a 32-bit value keeps only physHi's low 8
    // bits, so bit 8 of physHi (physical bit 40) reaches DMATRFBASE1 and
    // NOTHING ELSE. That is consistent with BASE topping out at physical 39
    // and is the whole reason the pair is described as 41 bits.
    //
    checkU32("physHi bit 8 survives only via BASE1",
             (NVU32)((0x1FFu << 24) & 0xFFFFFFFFu), 0xFF000000u);
}


//
// Three chunks. The assertion that matters is that MOFFS and FBOFFS ADVANCE
// by 0x100 per chunk while BASE/BASE1 are programmed exactly once. A machine
// that reprogrammed the base per chunk would still pass a final-value check;
// a machine that failed to advance the offsets would move the same 256 bytes
// three times and pass a "did it finish" check.
//
static void testDmaMultiChunk(void)
{
    section("DMA: three chunks, offsets advance, base programmed once");

    fixtureResetHealthy();

    NVGspBootParams p = nvGspBootObservedParams();
    p.image = fixtureImage(3u, 0x200u);

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    NVGspBootResult r = nvGspBootRun(&m, 1000u);

    checkResult("result", r, NVGSP_RESULT_RUNNING);
    checkU32("chunks planned", m.chunkCount, 3u);
    checkU32("chunks completed", m.chunksDone, 3u);

    //
    // 2 reset + 2 FBIF + 2 base + 3*3 chunk + 4 BROM + 1 BCR = 20.
    //
    checkU32("writes issued", m.writeCount, 20u);
    checkBool("journal did not overflow", gFix.journalOverflow, false);

    // Base written once, before the first chunk, and never again.
    checkJournal("DMATRFBASE once at index 4",
                 4u, ABS_DMATRFBASE, OBS_DMATRFBASE_VALUE);
    checkJournal("DMATRFBASE1 once at index 5",
                 5u, ABS_DMATRFBASE1, 0u);

    // Chunk 0: imem 0x200, fb 0x000.
    checkJournal("chunk 0 MOFFS",  6u,  ABS_DMATRFMOFFS,  0x200u);
    checkJournal("chunk 0 FBOFFS", 7u,  ABS_DMATRFFBOFFS, 0x000u);
    checkJournal("chunk 0 CMD",    8u,  ABS_DMATRFCMD,    OBS_DMATRFCMD_VALUE);

    // Chunk 1: both advance by one chunk.
    checkJournal("chunk 1 MOFFS advanced",  9u,  ABS_DMATRFMOFFS,  0x300u);
    checkJournal("chunk 1 FBOFFS advanced", 10u, ABS_DMATRFFBOFFS, 0x100u);
    checkJournal("chunk 1 CMD",             11u, ABS_DMATRFCMD,
                 OBS_DMATRFCMD_VALUE);

    // Chunk 2.
    checkJournal("chunk 2 MOFFS advanced",  12u, ABS_DMATRFMOFFS,  0x400u);
    checkJournal("chunk 2 FBOFFS advanced", 13u, ABS_DMATRFFBOFFS, 0x200u);
    checkJournal("chunk 2 CMD",             14u, ABS_DMATRFCMD,
                 OBS_DMATRFCMD_VALUE);

    // BROM only after the last chunk drained.
    checkJournal("PARAADDR(0) after the last chunk",
                 15u, ABS_BROM_PARAADDR0, p.paraAddr0);
    checkJournal("BCR_CTRL last", 19u, ABS_RISCV_BCR_CTRL, p.bcrCtrl);
}


//
// The engine does not finish instantly. With dmaDelayPolls the fixture serves
// non-idle reads before setting IDLE, so this exercises the drain loop rather
// than a transfer that happened to be done on first read.
//
// ===========================================================================
// DMA: the XFER/DRAIN loop as a STATE SEQUENCE, not just as register effects.
//
// Second finding from the same set-difference that produced testApertureGuard().
// NVGspBoot.hpp spells fifteen NVGSP_STATE_* names; this suite spelled thirteen.
// The two it never spelled were NVGSP_STATE_DMA_XFER and NVGSP_STATE_DMA_DRAIN.
//
// Unlike the aperture case that absence was NOT total coverage loss -- both
// states are reached at runtime by testDmaSingleChunk/MultiChunk/DrainPolls, and
// nvGspBootStateName()'s arms for both are covered by testNames()'s range loop.
// What no test did was observe the machine WHILE it was in them. Every DMA test
// calls nvGspBootRun() and then inspects the journal and the counters after the
// fact, so all of them assert the loop's EFFECTS. testStepDriving(), the one
// test that pins states as an ordered sequence, sets skipFirmwareLoad -- so the
// sequence it pins jumps FIRMWARE_LOAD straight to BROM_PARAMS and never enters
// the DMA path at all.
//
// That leaves a specific thing unasserted: the SHAPE of the per-chunk loop.
// A machine that programmed both chunks from DMA_XFER without ever passing
// through DMA_DRAIN between them would produce the same journal, the same
// chunksDone, and the same final state -- and on silicon would land chunk N+1's
// offsets while chunk N was still in flight, which is exactly the off-by-one
// NVFalcon.hpp:191 warns about and which does not fault, it just moves the wrong
// bytes. Effects-after-the-fact cannot tell those apart. A state sequence can.
// ===========================================================================

static void testDmaStateSequence(void)
{
    section("DMA: the XFER/DRAIN loop is walked as a state sequence");

    fixtureResetHealthy();

    NVGspBootParams p = nvGspBootObservedParams();
    p.image = fixtureImage(2u, 0u);

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    checkU32("state before any step", (NVU32)m.state, (NVU32)NVGSP_STATE_INIT);

    //
    // Two chunks, so the loop-back edge is exercised once. One chunk would walk
    // XFER then DRAIN then leave, which proves the states exist but not that
    // DRAIN returns to XFER rather than falling through.
    //
    const NVGspBootState expected[] = {
        NVGSP_STATE_PROBE,
        NVGSP_STATE_RESET_ASSERT,
        NVGSP_STATE_RESET_DEASSERT,
        NVGSP_STATE_SCRUB_WAIT,
        NVGSP_STATE_FIRMWARE_LOAD,
        NVGSP_STATE_FBIF_CFG,
        NVGSP_STATE_DMA_SETUP,
        NVGSP_STATE_DMA_XFER,     // chunk 0
        NVGSP_STATE_DMA_DRAIN,
        NVGSP_STATE_DMA_XFER,     // chunk 1, reached only via the DRAIN edge
        NVGSP_STATE_DMA_DRAIN,
        NVGSP_STATE_BROM_PARAMS,
        NVGSP_STATE_CORE_START,
        NVGSP_STATE_ACTIVE_WAIT
    };
    const NVU32 expectedCount =
        (NVU32)(sizeof(expected) / sizeof(expected[0]));

    bool  orderHeld = true;
    NVU32 taken     = 0u;

    //
    // chunksDone AT THE MOMENT each DMA_XFER is entered. The counter is bumped
    // by DRAIN, not by XFER, so these two readings are what prove the second
    // chunk was programmed after the first one drained rather than beside it.
    //
    NVU32 xferChunk[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    NVU32 xferSeen     = 0u;

    for (NVU32 i = 0u; i < expectedCount; ++i) {
        if (!nvGspBootStep(&m)) {
            orderHeld = false;
            std::printf("       step %u refused early, state %s\n",
                        i, nvGspBootStateName(m.state));
            break;
        }
        taken++;

        if (m.state != expected[i]) {
            orderHeld = false;
            std::printf("       step %u: got %s, expected %s\n",
                        i, nvGspBootStateName(m.state),
                        nvGspBootStateName(expected[i]));
            break;
        }

        if (m.state == NVGSP_STATE_DMA_XFER && xferSeen < 4u) {
            xferChunk[xferSeen++] = m.chunksDone;
        }
    }

    checkBool("state order held through the DMA loop", orderHeld, true);
    checkU32("  steps taken", taken, expectedCount);

    //
    // The states were entered, by name. This is the assertion the suite was
    // missing outright -- everything else about the DMA path was measured after
    // nvGspBootRun() had already returned.
    //
    checkU32("  DMA_XFER entered twice", xferSeen, 2u);
    checkU32("  first XFER saw chunksDone 0", xferChunk[0], 0u);
    checkU32("  second XFER saw chunksDone 1", xferChunk[1], 1u);

    //
    // And the loop terminated by counting, not by running out of steps.
    //
    checkU32("  chunkCount planned", m.chunkCount, 2u);
    checkU32("  chunksDone after the loop", m.chunksDone, 2u);

    //
    // Negative control on the sequence itself: a ONE-chunk image must NOT enter
    // DMA_XFER a second time. Without this, an expectation list that happened to
    // match a machine which re-entered XFER spuriously would still be green,
    // because the list above only says what it saw and not what it must not.
    //
    {
        fixtureResetHealthy();

        NVGspBootParams p1 = nvGspBootObservedParams();
        p1.image = fixtureImage(1u, 0u);

        NVGspBoot m1;
        nvGspBootInit(&m1, fixtureIo(true), p1);

        NVU32 xferCount  = 0u;
        NVU32 drainCount = 0u;

        for (NVU32 i = 0u; i < 64u; ++i) {
            if (!nvGspBootStep(&m1)) {
                break;
            }
            if (m1.state == NVGSP_STATE_DMA_XFER)  { xferCount++; }
            if (m1.state == NVGSP_STATE_DMA_DRAIN) { drainCount++; }
        }

        checkU32("one chunk enters XFER once", xferCount, 1u);
        checkU32("  and DRAIN once", drainCount, 1u);
        checkU32("  chunksDone", m1.chunksDone, 1u);
        checkResult("  reached RUNNING", m1.result, NVGSP_RESULT_RUNNING);
    }
}


static void testDmaDrainPolls(void)
{
    section("DMA: drain actually polls before IDLE");

    fixtureResetHealthy();
    gFix.dmaDelayPolls = 4u;

    NVGspBootParams p = nvGspBootObservedParams();
    p.image = fixtureImage(2u, 0u);

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    NVGspBootResult r = nvGspBootRun(&m, 1000u);

    checkResult("result", r, NVGSP_RESULT_RUNNING);
    checkU32("chunks completed", m.chunksDone, 2u);

    //
    // 4 non-idle reads per chunk, two chunks. dmaPolls resets per chunk, so
    // only the running total records the work.
    //
    checkU32("total non-idle drain polls", m.dmaPollsTotal, 8u);
    checkBool("per-chunk poll counter stayed under budget",
              m.dmaPolls < p.maxDmaPolls, true);
}


//
// A wedged engine. Without a bounded drain this is an infinite loop on
// hardware, so the named timeout is the whole assertion.
//
static void testDmaWedged(void)
{
    section("DMA: wedged transfer times out with a named result");

    fixtureResetHealthy();
    gFix.dmaNeverIdle = true;

    NVGspBootParams p = nvGspBootObservedParams();
    p.image        = fixtureImage(1u, 0u);
    p.maxDmaPolls  = 8u;

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    NVGspBootResult r = nvGspBootRun(&m, 1000u);

    checkResult("result", r, NVGSP_RESULT_ERR_DMA_TIMEOUT);
    checkU32("state", (NVU32)m.state, (NVU32)NVGSP_STATE_STOPPED);
    checkU32("stopped at the budget, not past it", m.dmaPolls, 8u);
    checkU32("no chunk was ever completed", m.chunksDone, 0u);

    //
    // It got as far as arming the transfer -- the CMD write is what proves
    // the timeout is the ENGINE not responding, not the machine refusing to
    // start.
    //
    checkJournal("CMD was written before the wait",
                 8u, ABS_DMATRFCMD, OBS_DMATRFCMD_VALUE);
}


//
// BROM_PARAMS as a STATE SEQUENCE, which is the complement of the journal
// indices testFullSequenceOrder() already pins on the same four writes.
//
// Those two assertions are not redundant and neither subsumes the other:
//
//   A journal cannot see a missing state transition. If BROM_PARAMS were
//   entered twice, or entered from the wrong predecessor, or if CORE_START
//   were skipped so that ACTIVE_WAIT ran against an unstarted core, the four
//   writes would still land at the same four indices carrying the same four
//   values. checkJournal() would be green on all of it.
//
//   A state sequence cannot see a reordered write. MOD_SEL is the LATCH --
//   NVGspBoot.hpp:750 says so plainly -- so writing it before the other three
//   commits stale parameters and the boot ROM rejects the signature. The walk
//   through BROM_PARAMS is identical either way, one step in and one step out.
//
// So this section asserts the walk, and re-asserts the order INSIDE the one
// step that performs it. The second part is what a per-run journal check does
// not give you: testFullSequenceOrder() indexes into the journal of a whole
// run, where indices 4..7 happen to be the BROM writes because of everything
// that came before. Here the four writes are bounded to a SINGLE step, so the
// claim is "this state emitted exactly these four, in this order" rather than
// "the run as a whole eventually contained them".
//
// skipFirmwareLoad is used deliberately. NVGspBoot.hpp:276 says that flag
// exists so tests can reach BROM_PARAMS -> CORE_START -> ACTIVE_WAIT without
// the DMA path; testDmaStateSequence() already covers the other entry edge
// (DMA_DRAIN on the last chunk), and both edges landing on the same successor
// is asserted below rather than assumed.
//
static void testBromParamsStateSequence(void)
{
    section("BROM_PARAMS: the parameter latch as a state sequence");

    //
    // Part 1 -- the walk. One step in, one step out, and the successor is
    // CORE_START rather than ACTIVE_WAIT.
    //
    {
        fixtureResetHealthy();

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        //
        // Walk to the state rather than assuming a step count. A machine that
        // grew or lost a state ahead of BROM_PARAMS would otherwise shift the
        // index and make every assertion below describe the wrong step.
        //
        bool  arrived   = false;
        NVU32 stepsUsed = 0u;

        for (NVU32 i = 0u; i < 64u; ++i) {
            if (m.state == NVGSP_STATE_BROM_PARAMS) {
                arrived   = true;
                stepsUsed = i;
                break;
            }
            if (!nvGspBootStep(&m)) {
                break;
            }
        }

        checkBool("BROM_PARAMS is reached with skipFirmwareLoad", arrived, true);
        checkBool("it is reached without exhausting the walk",
                  stepsUsed < 64u, true);

        //
        // The predecessor edge. NVGspBoot.hpp:642 routes FIRMWARE_LOAD straight
        // here when the flag is set; if that arm ever fell through to FBIF_CFG
        // instead, the run would still reach BROM_PARAMS by the DMA route and
        // this test would pass while exercising a path it did not name.
        //
        checkU32("no DMA write happened on the way in", gFix.writeCount, 2u);

        NVU32 beforeWrites = gFix.writeCount;

        checkBool("the BROM_PARAMS step itself succeeds",
                  nvGspBootStep(&m), true);

        checkU32("one step emitted exactly four writes",
                 gFix.writeCount - beforeWrites, 4u);

        checkU32("BROM_PARAMS advances to CORE_START",
                 (NVU32)m.state, (NVU32)NVGSP_STATE_CORE_START);

        //
        // Part 2 -- the order, bounded to that one step. beforeWrites is the
        // base rather than a literal 2, so the four indices stay correct if a
        // write is ever added ahead of this state.
        //
        // MOD_SEL is asserted LAST and the other three are asserted at fixed
        // offsets from the same base, which is what makes a swap detectable:
        // moving MOD_SEL to the front leaves the same four (offset, value)
        // pairs in the journal and only the ORDER differs.
        //
        checkJournal("1st: PARAADDR(0)",
                     beforeWrites + 0u, ABS_BROM_PARAADDR0, p.paraAddr0);
        checkJournal("2nd: ENGIDMASK",
                     beforeWrites + 1u, ABS_BROM_ENGIDMASK, p.engIdMask);
        checkJournal("3rd: CURR_UCODE_ID",
                     beforeWrites + 2u, ABS_BROM_UCODE_ID, p.ucodeId);
        checkJournal("4th: MOD_SEL, the latch, LAST",
                     beforeWrites + 3u, ABS_BROM_MOD_SEL, p.modSelAlgo);

        //
        // All four are RISC-V-block addresses. NVGspBoot.hpp writes them through
        // nvGspRiscvReg(); the falcon block sits exactly one 4 KiB page below,
        // so a site that reached for nvGspFalconReg() by mistake would produce a
        // VALID address into the wrong engine. The ABS_ literals above are
        // transcribed from the trace rather than derived, so this comparison is
        // an independent witness rather than the same arithmetic twice.
        //
        checkBool("all four land in the RISC-V page, not the Falcon one",
                  (ABS_BROM_PARAADDR0 & 0xFFFFF000u) ==
                      (ABS_BROM_MOD_SEL & 0xFFFFF000u) &&
                  (ABS_BROM_PARAADDR0 & 0xFFFFF000u) !=
                      (ABS_FALCON_CPUCTL & 0xFFFFF000u),
                  true);
    }

    //
    // Part 3 -- the state is entered EXACTLY ONCE. A machine that re-entered it
    // would re-latch MOD_SEL after the boot ROM had already consumed the block,
    // and would produce a journal that merely looks longer -- indistinguishable
    // from a longer image unless the entries are counted per state.
    //
    {
        fixtureResetHealthy();

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        NVU32 bromCount  = 0u;
        NVU32 coreCount  = 0u;

        for (NVU32 i = 0u; i < 64u; ++i) {
            if (!nvGspBootStep(&m)) {
                break;
            }
            if (m.state == NVGSP_STATE_BROM_PARAMS) { bromCount++; }
            if (m.state == NVGSP_STATE_CORE_START)  { coreCount++; }
        }

        checkU32("BROM_PARAMS is entered exactly once", bromCount, 1u);
        checkU32("CORE_START is entered exactly once",  coreCount, 1u);
    }

    //
    // Part 4 -- the negative control. If the assertions above could not tell a
    // walk apart from a different walk, they would be pinning nothing. Driving
    // the SAME machine with skipFirmwareLoad clear must reach BROM_PARAMS by a
    // demonstrably different route: through the DMA states, with more than two
    // writes behind it by the time it arrives.
    //
    {
        fixtureResetHealthy();

        NVGspBootParams p = nvGspBootObservedParams();
        p.image = fixtureImage(1u, 0u);

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        bool  arrived   = false;
        bool  sawDma    = false;
        NVU32 atArrival = 0u;

        for (NVU32 i = 0u; i < 64u; ++i) {
            if (m.state == NVGSP_STATE_DMA_XFER ||
                m.state == NVGSP_STATE_DMA_DRAIN) {
                sawDma = true;
            }
            if (m.state == NVGSP_STATE_BROM_PARAMS) {
                arrived   = true;
                atArrival = gFix.writeCount;
                break;
            }
            if (!nvGspBootStep(&m)) {
                break;
            }
        }

        checkBool("without the flag, BROM_PARAMS is still reached",
                  arrived, true);
        checkBool("and it is reached THROUGH the DMA states", sawDma, true);
        checkBool("so the DMA route really did more writes than the skip route",
                  atArrival > 2u, true);

        //
        // Both entry edges land on the same successor. This is the claim that
        // makes the skip-flag route a legitimate stand-in for the real one:
        // if DMA_DRAIN's exit and FIRMWARE_LOAD's exit diverged after
        // BROM_PARAMS, Part 1 would be describing a path hardware never takes.
        //
        checkBool("the BROM_PARAMS step succeeds on this route too",
                  nvGspBootStep(&m), true);
        checkU32("and it advances to CORE_START from here as well",
                 (NVU32)m.state, (NVU32)NVGSP_STATE_CORE_START);
    }
}


//
// Did the machine ever read this exact absolute address?
//
static bool fixtureWasRead(NVU32 offset)
{
    for (NVU32 i = 0u; i < gFix.readOffsetCount; ++i) {
        if (gFix.readOffsets[i] == offset) {
            return true;
        }
    }
    return false;
}

//
// The gap between the two engine blocks. NV_PGSP_BASE is 0x110000 and
// NV_FALCON2_GSP_BASE is 0x111000, so a site that reached for the wrong
// translator produces an address exactly this far from the right one -- in
// whichever direction it got wrong. Spelled as a literal for the same reason
// every ABS_ constant is: if this were derived from the two bases, a bug that
// moved a base would move the expected wrong address along with it.
//
#define ENGINE_PAGE_DELTA 0x1000u

//
// Every READ site chose the right translator.
//
// This is the read-side counterpart of testAddressAgreement(), and it exists
// because those two things are not the same claim. testAddressAgreement() pins
// nvGspFalconReg() and nvGspRiscvReg() against trace literals -- it proves each
// FUNCTION is correct. It says nothing about whether a given access site CALLED
// the correct one, and CLAUDE.md is explicit that this is the project's most
// dangerous class: the RISC-V block sits exactly one 4 KiB page above the Falcon
// block, so the wrong translator yields a VALID address into the wrong engine.
// It does not fault. It reads and writes real hardware silently.
//
// For WRITES that is already covered, and covered strongly: checkJournal()
// asserts an absolute offset per write, so nvGspFalconReg() at a RISC-V write
// site would produce an address 0x1000 off and redden immediately.
//
// For READS there was nothing. fixtureRead32() bumped readCount and discarded
// the address, so the only observable was "a read happened". A read site 0x1000
// low lands on a Falcon register that exists, returns whatever the fixture has
// there (zero, in a fresh reset), and the machine proceeds on that value. That
// is invisible in every existing check in this suite.
//
// So the fixture now records the SET of offsets read, and this section asserts
// two things per site: the right address WAS read, and the mistranslated one was
// NOT. The second half is the load-bearing one -- "CPUCTL was read" would stay
// green on a machine that read both.
//
static void testReadTranslators(void)
{
    section("every READ site chose the right engine translator");

    //
    // This needs TWO runs, and that is a fact about the state machine rather
    // than a convenience. ACTIVE_WAIT reads RISC-V CPUCTL first and stops on
    // NVGSP_RESULT_RUNNING at NVGspBoot.hpp:805 the moment the core reports
    // ACTIVE -- which is BEFORE nvGspBootCheckRiscvErrors() at :808. So a
    // healthy boot can never reach the two error-status read sites, and any run
    // that does reach them is by construction one where the core never went
    // active. A single run cannot cover all six sites, and the first version of
    // this section asserted that it did: it demanded six addresses from a
    // healthy run, got four, and was wrong about the machine rather than the
    // other way round.
    //
    // Run A also carries the precedence claim as a positive statement: ACTIVE
    // beats the error check, so the error registers must be ABSENT here.
    //
    NVU32 runACount = 0u;

    //
    // RUN A -- healthy, one-chunk image, so the DMA path is entered. Covers
    // falcon CPUCTL (:514), falcon HWCFG2 (:522 and :558 re-read), falcon
    // DMATRFCMD (:730) and RISC-V CPUCTL (:800). Four distinct addresses.
    //
    {
        fixtureResetHealthy();

        NVGspBootParams p = nvGspBootObservedParams();
        p.image = fixtureImage(1u, 0u);

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        NVGspBootResult r = nvGspBootRun(&m, 1000u);

        //
        // RUNNING is the SUCCESS terminal for this machine -- the core came up.
        // Asserting r != RUNNING would be asserting the boot failed, which is
        // exactly the mistake the first draft of this section made.
        //
        checkResult("run A: a healthy boot ends RUNNING", r,
                    NVGSP_RESULT_RUNNING);
        checkBool("  and that is an expected end",
                  nvGspBootReachedExpectedEnd(&m), true);

        //
        // Non-vacuity first. Every assertion below is of the form "this address
        // is in the set" or "this one is not", and both are trivially satisfied
        // by an EMPTY set -- so the set has to be shown non-empty and
        // un-truncated before any of it means anything.
        //
        checkBool("  reads actually happened", gFix.readCount > 0u, true);
        checkBool("  the offset set is non-empty",
                  gFix.readOffsetCount > 0u, true);
        checkBool("  the offset set did not overflow",
                  gFix.readOffsetOverflow, false);

        //
        // The three Falcon-block read sites. HWCFG2 is read from two different
        // states and is one address, which is why this is a SET and not a
        // sequence.
        //
        checkBool("  falcon CPUCTL read at its falcon address",
                  fixtureWasRead(ABS_FALCON_CPUCTL), true);
        checkBool("    and NOT one page up, in the RISC-V block",
                  fixtureWasRead(ABS_FALCON_CPUCTL + ENGINE_PAGE_DELTA), false);

        checkBool("  falcon HWCFG2 read at its falcon address",
                  fixtureWasRead(ABS_FALCON_HWCFG2), true);
        checkBool("    and NOT one page up, in the RISC-V block",
                  fixtureWasRead(ABS_FALCON_HWCFG2 + ENGINE_PAGE_DELTA), false);

        checkBool("  falcon DMATRFCMD read at its falcon address",
                  fixtureWasRead(ABS_DMATRFCMD), true);
        checkBool("    and NOT one page up, in the RISC-V block",
                  fixtureWasRead(ABS_DMATRFCMD + ENGINE_PAGE_DELTA), false);

        //
        // The RISC-V CPUCTL poll. This is the one the aperture guard cannot
        // help with either: one page DOWN from it is still inside
        // NV_PGSP_BASE..NV_PGSP_LIMIT, so nvIsInGspAperture() is perfectly
        // happy with the wrong answer.
        //
        checkBool("  RISC-V CPUCTL read at its RISC-V address",
                  fixtureWasRead(ABS_RISCV_CPUCTL), true);
        checkBool("    and NOT one page down, in the Falcon block",
                  fixtureWasRead(ABS_RISCV_CPUCTL - ENGINE_PAGE_DELTA), false);

        //
        // ACTIVE beats the error check. If these two ever appear in a healthy
        // run, :805 stopped returning before :808 -- which would mean every
        // successful boot pays two extra MMIO reads it does not need, and would
        // also mean run B below is no longer testing what it claims to.
        //
        checkBool("  ACTIVE wins: PRIV_ERR_STAT never read",
                  fixtureWasRead(ABS_RISCV_PRIV_ERR), false);
        checkBool("  ACTIVE wins: HUB_ERR_STAT never read",
                  fixtureWasRead(ABS_RISCV_HUB_ERR), false);

        //
        // Exactly those four and nothing else. This is what turns the per-site
        // checks into a statement about the whole machine: a NEW read site
        // added later, at whatever address, fails here even though every
        // assertion above still passes.
        //
        // If this fires after adding a read, the fix is to name the new address
        // above, not to raise the number.
        //
        checkU32("  exactly four distinct addresses were read",
                 gFix.readOffsetCount, 4u);

        //
        // The helper's own negative control. Every "NOT read" assertion above
        // is a false negative if fixtureWasRead() simply cannot find anything,
        // and its positive counterparts would not catch that -- a linear scan
        // that returned true for the first entry and false otherwise would
        // satisfy a surprising amount of the above. So: an address in the
        // aperture this machine has no reason to touch must report absent, and
        // one that was WRITTEN but never read must ALSO report absent, which is
        // what proves the read set is not accidentally the write journal.
        //
        checkBool("  an untouched aperture address reports absent",
                  fixtureWasRead(0x00113FFCu), false);
        checkBool("  MOD_SEL was written, and is not in the READ set",
                  fixtureWasRead(ABS_BROM_MOD_SEL), false);
        checkBool("  BCR_CTRL was written, and is not in the READ set",
                  fixtureWasRead(ABS_RISCV_BCR_CTRL), false);

        runACount = gFix.readOffsetCount;
    }

    //
    // RUN B -- the core is released but never reports ACTIVE and no error latch
    // fires, so ACTIVE_WAIT falls through :805 into
    // nvGspBootCheckRiscvErrors() and the two error-status sites at :400 and
    // :404 are finally reached. skipFirmwareLoad keeps the DMA path out of it,
    // which is itself asserted below -- five distinct addresses, not five plus
    // DMATRFCMD.
    //
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u, 0u, 0u);

        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        NVGspBootResult r = nvGspBootRun(&m, 1000u);

        checkResult("run B: core never goes ACTIVE", r,
                    NVGSP_RESULT_ERR_ACTIVE_TIMEOUT);
        checkBool("  reads actually happened", gFix.readCount > 0u, true);
        checkBool("  the offset set did not overflow",
                  gFix.readOffsetOverflow, false);

        //
        // The two sites this whole second run exists to reach. Both go through
        // nvGspRiscvReg(), and one page DOWN from either lands in the Falcon
        // block on a live card -- a read that returns a plausible value from a
        // register belonging to the wrong engine.
        //
        checkBool("  PRIV_ERR_STAT read at its RISC-V address",
                  fixtureWasRead(ABS_RISCV_PRIV_ERR), true);
        checkBool("    and NOT one page down, in the Falcon block",
                  fixtureWasRead(ABS_RISCV_PRIV_ERR - ENGINE_PAGE_DELTA),
                  false);

        checkBool("  HUB_ERR_STAT read at its RISC-V address",
                  fixtureWasRead(ABS_RISCV_HUB_ERR), true);
        checkBool("    and NOT one page down, in the Falcon block",
                  fixtureWasRead(ABS_RISCV_HUB_ERR - ENGINE_PAGE_DELTA),
                  false);

        checkBool("  RISC-V CPUCTL read at its RISC-V address",
                  fixtureWasRead(ABS_RISCV_CPUCTL), true);
        checkBool("    and NOT one page down, in the Falcon block",
                  fixtureWasRead(ABS_RISCV_CPUCTL - ENGINE_PAGE_DELTA), false);

        checkBool("  falcon CPUCTL read at its falcon address",
                  fixtureWasRead(ABS_FALCON_CPUCTL), true);
        checkBool("    and NOT one page up, in the RISC-V block",
                  fixtureWasRead(ABS_FALCON_CPUCTL + ENGINE_PAGE_DELTA), false);

        checkBool("  falcon HWCFG2 read at its falcon address",
                  fixtureWasRead(ABS_FALCON_HWCFG2), true);
        checkBool("    and NOT one page up, in the RISC-V block",
                  fixtureWasRead(ABS_FALCON_HWCFG2 + ENGINE_PAGE_DELTA), false);

        //
        // The DMA read site must be ABSENT here. Without this the run could be
        // silently taking the DMA path and the count below would be wrong for a
        // reason no assertion names.
        //
        checkBool("  DMATRFCMD absent: skipFirmwareLoad really skipped DMA",
                  fixtureWasRead(ABS_DMATRFCMD), false);

        checkU32("  exactly five distinct addresses were read",
                 gFix.readOffsetCount, 5u);

        checkBool("  MOD_SEL was written, and is not in the READ set",
                  fixtureWasRead(ABS_BROM_MOD_SEL), false);

        //
        // The union over both runs is the machine's whole read-site inventory:
        // six addresses, with three (falcon CPUCTL, falcon HWCFG2, RISC-V
        // CPUCTL) shared. Spelled as arithmetic over the two measured counts so
        // that a read site added to EITHER path breaks it, rather than only the
        // path whose count happens to be checked.
        //
        checkU32("the two runs cover six distinct read sites",
                 runACount + gFix.readOffsetCount - 3u, 6u);
    }

    //
    // And the two blocks really are one page apart, which is the premise the
    // whole section rests on. Derived from the trace literals rather than from
    // the bases the machine uses, so it is an independent witness: if this were
    // false, every "one page up/down" assertion above would be probing an
    // address that means nothing.
    //
    checkU32("the RISC-V block is one page above the Falcon block",
             (ABS_RISCV_CPUCTL & 0xFFFFF000u) -
                 (ABS_FALCON_CPUCTL & 0xFFFFF000u),
             ENGINE_PAGE_DELTA);
}


//
// Image validation. Each of these is refused BEFORE FBIF is touched, so the
// journal length is the assertion: only the two reset writes happened.
//
static void testDmaImageValidation(void)
{
    section("DMA: invalid images are refused before any DMA write");

    struct BadCase {
        const char *name;
        NVU32       sizeBytes;
        NVU32       physAddrLo;
        NVU32       physAddrHi;
    };

    //
    // The physAddrHi cases are the ones a base-only reading cannot see. The
    // register pair carries 17 bits of the high word (NVGSP_DMA_PHYSHI_MASK);
    // anything above that is TRUNCATED by the packing rather than rejected by
    // it, so without a refusal here the engine gets a well-formed address
    // pointing at the wrong page. 0x20000 is one bit past the window and
    // 0x1FFFF (the last legal value) is asserted as ACCEPTED further below --
    // the pair is what separates a correct mask from a wider or narrower one.
    //
    static const BadCase bad[] = {
        { "size 0",                    0u,           OBS_IMAGE_PHYS_LO,     0u },
        { "size not a whole chunk",    DMA_CHUNK + 1u, OBS_IMAGE_PHYS_LO,   0u },
        { "size under one chunk",      64u,          OBS_IMAGE_PHYS_LO,     0u },
        { "phys base not 256-aligned", DMA_CHUNK,    OBS_IMAGE_PHYS_LO + 1u, 0u },
        { "physHi one bit past the window",
                                       DMA_CHUNK,    OBS_IMAGE_PHYS_LO,     0x20000u },
        { "physHi with only the top bit set",
                                       DMA_CHUNK,    OBS_IMAGE_PHYS_LO,     0x80000000u },
        { "physHi all ones",           DMA_CHUNK,    OBS_IMAGE_PHYS_LO,     0xFFFFFFFFu },
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        fixtureResetHealthy();

        NVGspBootParams p = nvGspBootObservedParams();
        p.image            = fixtureImage(1u, 0u);
        p.image.sizeBytes  = bad[i].sizeBytes;
        p.image.physAddrLo = bad[i].physAddrLo;
        p.image.physAddrHi = bad[i].physAddrHi;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        NVGspBootResult r = nvGspBootRun(&m, 1000u);

        checkResult(bad[i].name, r, NVGSP_RESULT_ERR_FIRMWARE_INVALID);
        checkU32("  refused before FBIF (only the 2 reset writes)",
                 gFix.writeCount, 2u);
        checkU32("  no chunks planned", m.chunkCount, 0u);
    }

    //
    // THE FAR SIDE OF THE SAME BOUNDARY. Every case above is a refusal, and a
    // check that only ever asserts refusal passes for a mask of ZERO -- a
    // machine that rejected every nonzero physAddrHi would look identical.
    //
    // 0x1FFFF is the last value the register pair can carry, so this and the
    // 0x20000 refusal above are separating inputs: widening the mask by one bit
    // breaks the refusal, narrowing it by one bit breaks this acceptance.
    // Neither assertion alone can tell a correct mask from a wrong one.
    //
    fixtureResetHealthy();
    {
        NVGspBootParams p = nvGspBootObservedParams();
        p.image            = fixtureImage(1u, 0u);
        p.image.physAddrHi = 0x1FFFFu;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);

        checkResult("physHi at the top of the window is ACCEPTED",
                    nvGspBootRun(&m, 1000u), NVGSP_RESULT_RUNNING);
        checkU32("  the transfer ran", m.chunksDone, 1u);
    }

    //
    // The predicate driven directly, so the boundary is pinned without having
    // to reach it through a full state-machine run. A test that can only see
    // the bound through nvGspBootRun() cannot say WHICH bit failed.
    //
    checkU32("NVGSP_DMA_PHYSHI_MASK is 17 bits",
             (NVU32)NVGSP_DMA_PHYSHI_MASK, 0x1FFFFu);
    checkBool("physHi 0 fits",       nvGspDmaPhysHiFits(0u), true);
    checkBool("physHi 0xFF fits",    nvGspDmaPhysHiFits(0xFFu), true);
    checkBool("physHi 0x100 fits",   nvGspDmaPhysHiFits(0x100u), true);
    checkBool("physHi 0x1FFFF fits", nvGspDmaPhysHiFits(0x1FFFFu), true);
    checkBool("physHi 0x20000 does NOT fit",
              nvGspDmaPhysHiFits(0x20000u), false);
    checkBool("physHi 0x20000 differs from 0x1FFFF in exactly the bit above the field",
              (0x20000u ^ 0x1FFFFu) == 0x3FFFFu, true);
}


//
// skipFirmwareLoad is a TEST-ONLY door. A valid image must take priority over
// it, or a stray skip in production would silently release a core with empty
// IMEM while every test still looked green.
//
static void testDmaImageBeatsSkip(void)
{
    section("DMA: a real image outranks skipFirmwareLoad");

    fixtureResetHealthy();

    NVGspBootParams p = nvGspBootObservedParams();
    p.image            = fixtureImage(1u, 0u);
    p.skipFirmwareLoad = true;

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    checkResult("result", nvGspBootRun(&m, 1000u), NVGSP_RESULT_RUNNING);
    checkU32("the transfer still happened", m.chunksDone, 1u);
    checkJournal("FBIF was still programmed",
                 2u, ABS_FBIF_TRANSCFG0, OBS_FBIF_TRANSCFG0);
}


//
// The dry-run gate has to hold on the DMA path too. With an image present the
// machine now has far more to write, and the first refusal must still be the
// FIRST write -- not the first DMA write.
//
static void testDmaRespectsDryRun(void)
{
    section("DMA: dry-run gate still refuses the first write");

    fixtureResetHealthy();

    NVGspBootParams p = nvGspBootObservedParams();
    p.image = fixtureImage(4u, 0u);

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(false), p);

    checkResult("result", nvGspBootRun(&m, 1000u),
                NVGSP_RESULT_ERR_WRITES_DISABLED);
    checkU32("nothing reached the fixture", gFix.writeCount, 0u);
    checkU32("no chunk started", m.chunksDone, 0u);
}


// ===========================================================================
// 9. Phase 5 -- the DMA transfer into IMEM.
//
// handoff5.md section 6 lists these cases. Each one pins a value or an order
// that a plausible-looking refactor could silently change: DMATRFBASE is a
// SHIFTED address, the per-chunk writes have a mandatory order because CMD is
// the trigger, and BASE1 is written even when the high word is zero.
// ===========================================================================

static void testDmaTransfer(void)
{
    section("phase 5 DMA transfer");

    //
    // 1. The command word itself. 0x610 is what the four documented fields
    //    produce; the trace wrote 0x614. Bit 2 is the low bit of SEC.
    //
    checkU32("DMATRFCMD_LOAD_IMEM == observed 0x614",
             (NVU32)NVGSP_DMATRFCMD_LOAD_IMEM, OBS_DMATRFCMD_VALUE);

    //
    // 2. DMATRFBASE stores addr >> 8. 0xFFEEE200 >> 8 == 0x00FFEEE2.
    //
    checkU32("DMATRFBASE is physLo >> 8",
             nvGspDmaTrfBase(OBS_IMAGE_PHYS_LO, 0u), OBS_DMATRFBASE_VALUE);
    checkU32("DMATRFBASE1 saturates its 9-bit field",
             nvGspDmaTrfBase1(0xFFFFFFFFu),
             (NVU32)NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK);
    checkU32("DMATRFBASE1 is zero for a 32-bit address",
             nvGspDmaTrfBase1(0u), 0u);

    //
    // 3. BASE1 IS A CONTINUATION OF THE SHIFTED ADDRESS, NOT AN OVERLAY ON THE
    //    UNSHIFTED ONE. The three assertions above do not distinguish the two
    //    readings: 0xFFFFFFFF >> 8 & 0x1FF and 0xFFFFFFFF & 0x1FF are BOTH
    //    0x1FF, and 0 is 0 either way. So the whole group stayed green while
    //    the helper carried the overlapping form (NVGspBoot.hpp:119-143).
    //
    //    These are the values that separate them. physHi is the UNSHIFTED high
    //    word, so the correct helper shifts it down by 8 first:
    //
    //      physHi        correct  overlapping (WRONG)
    //      0x00000001    0        1          <- exactly 4 GiB
    //      0x00000100    1        0
    //      0x000001FF    1        0x1FF
    //
    //    Every one of these is 256-byte aligned and inside no aperture check,
    //    so nothing else in the suite would notice.
    //
    checkU32("DMATRFBASE1 at exactly 4 GiB is 0, not 1",
             nvGspDmaTrfBase1(0x00000001u), 0u);
    checkU32("DMATRFBASE1 carries shifted bit 40, i.e. physHi bit 8",
             nvGspDmaTrfBase1(0x00000100u), 1u);
    checkU32("DMATRFBASE1 ignores physHi bits 7:0 entirely",
             nvGspDmaTrfBase1(0x000001FFu), 1u);

    //
    // 4. The pair must be a NON-OVERLAPPING split of (phys >> 8). Reassemble
    //    it and compare against the shift done in 64-bit arithmetic -- that is
    //    the property, and it is what the overlapping form violated. 2^40 is
    //    the smallest probe where BASE1 must be non-zero.
    //
    {
        const unsigned long long addr = 1ULL << 40;
        NVU32 lo   = (NVU32)(addr & 0xFFFFFFFFull);
        NVU32 hi   = (NVU32)(addr >> 32);
        unsigned long long back =
            ((unsigned long long)nvGspDmaTrfBase1(hi) << 32) |
            (unsigned long long)nvGspDmaTrfBase(lo, hi);
        checkU64("BASE1:BASE reassembles to phys >> 8 at 2^40",
                 back, addr >> 8);
    }
}


//
// t-b3dbb9. The card asked for an FBIF programming step between SCRUB_WAIT and
// DMA_SETUP. The step ALREADY EXISTS in NVGspBoot.hpp -- what was missing was
// anything that would notice if it were removed. That is what this is.
//
// The journal tests above pin the two VALUES and their order. This one pins the
// STATE ITSELF, by single-stepping and recording the sequence: a refactor that
// folded the FBIF writes into DMA_SETUP would keep every journal assertion
// green while deleting the state, and only this test would fail.
//
static void testFbifStateOrdering(void)
{
    section("FBIF: the state exists and sits before DMA_SETUP");

    fixtureResetHealthy();

    NVGspBootParams p = nvGspBootObservedParams();
    p.image = fixtureImage(1u, 0u);

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    //
    // Walk until the machine leaves FBIF_CFG, recording where FBIF and
    // DMA_SETUP were first seen. Indices, not booleans: "both happened" is
    // not the claim -- "FBIF happened FIRST" is.
    //
    int fbifAt    = -1;
    int dmaSetupAt = -1;
    int scrubAt    = -1;
    int step       = 0;

    while (nvGspBootStep(&m) && step < 200) {
        if (m.state == NVGSP_STATE_SCRUB_WAIT && scrubAt < 0)
            scrubAt = step;
        if (m.state == NVGSP_STATE_FBIF_CFG && fbifAt < 0)
            fbifAt = step;
        if (m.state == NVGSP_STATE_DMA_SETUP && dmaSetupAt < 0)
            dmaSetupAt = step;
        step++;
    }

    checkBool("FBIF_CFG was entered at all", fbifAt >= 0, true);
    checkBool("DMA_SETUP was entered at all", dmaSetupAt >= 0, true);
    checkBool("FBIF_CFG strictly precedes DMA_SETUP",
              fbifAt >= 0 && dmaSetupAt >= 0 && fbifAt < dmaSetupAt, true);
    checkBool("FBIF_CFG comes after SCRUB_WAIT",
              scrubAt >= 0 && fbifAt > scrubAt, true);

    //
    // A state with no name is a state that cannot be reported from a panic
    // log, which is the only place this machine's state is ever read on
    // hardware.
    //
    checkBool("FBIF_CFG has a diagnostic name",
              std::strcmp(nvGspBootStateName(NVGSP_STATE_FBIF_CFG),
                          "FBIF_CFG") == 0, true);

    checkResult("and the run still completes", m.result, NVGSP_RESULT_RUNNING);
}


//
// The other half of t-b3dbb9: a write that FAILS inside FBIF_CFG must stop the
// machine, not be skipped past. FBIF is the step that makes a physical-address
// DMA legal, so a half-programmed FBIF is worse than none -- CTL could grant
// ALLOW_PHYS_NO_CTX while TRANSCFG still points at the wrong aperture.
//
// The fault is injected by clearing io.write32 once the machine has ARRIVED at
// FBIF_CFG but before that state executes. nvGspBootWrite() checks the callback
// first, so the very first FBIF write takes the ERR_NO_IO path.
//
static void testFbifWriteFaultPropagates(void)
{
    section("FBIF: a failed write inside FBIF_CFG stops the machine");

    fixtureResetHealthy();

    NVGspBootParams p = nvGspBootObservedParams();
    p.image = fixtureImage(1u, 0u);

    NVGspBoot m;
    nvGspBootInit(&m, fixtureIo(true), p);

    //
    // Advance until FBIF_CFG is the state about to run. Bounded so a machine
    // that never reaches it fails the check below instead of spinning.
    //
    int  step    = 0;
    bool arrived = false;
    while (step < 200) {
        if (m.state == NVGSP_STATE_FBIF_CFG) {
            arrived = true;
            break;
        }
        if (!nvGspBootStep(&m))
            break;
        step++;
    }

    checkBool("reached FBIF_CFG before injecting the fault", arrived, true);

    NVU32 beforeFault = gFix.writeCount;
    checkU32("only the two reset writes so far", beforeFault, 2u);

    // Injecting the fault: no write callback at all.
    m.io.write32 = 0;

    nvGspBootStep(&m);

    checkResult("stopped with a named IO error", m.result, NVGSP_RESULT_ERR_NO_IO);
    checkU32("state", (NVU32)m.state, (NVU32)NVGSP_STATE_STOPPED);

    //
    // The assertion that matters: NOTHING landed. Not TRANSCFG alone, not CTL
    // alone. A machine that ignored the failed write and fell through to
    // DMA_SETUP would show more stores here.
    //
    checkU32("no FBIF store reached the fixture", gFix.writeCount, beforeFault);
    checkU32("and no DMA was ever armed", m.chunksDone, 0u);
}


//
// Which diagnostic words a run actually READ.
//
// NVGspBoot zero-initialises cpuctl, hwcfg2, riscvCpuctl, privErrStat and
// hubErrStat, and each is assigned only inside the state that reads it. Until
// readMask existed, NV3050Probe::publishGspBootResult() published all five
// unconditionally -- so a run that stopped at PROBE put three literal zeros
// into the IORegistry, byte-identical to three registers that genuinely read
// clear. handoff18 section 1d's class.
//
// The separating case is run A below, and it is NOT the obvious one. A HEALTHY
// boot never reads the error pair at all: ACTIVE_WAIT reads RISCV_CPUCTL, sees
// ACTIVE, and stops on NVGSP_RESULT_RUNNING before nvGspBootCheckRiscvErrors()
// is ever called. So the successful path is exactly the path that used to
// fabricate two words -- and it is the path an operator is most likely to be
// reading when they trust them.
//
static void testReadMask(void)
{
    section("readMask: only words this run actually read");

    //
    // A machine the caller cannot see has measured nothing. Refuse rather than
    // trust, the same direction as the rest of the header.
    //
    checkBool("a null machine reports nothing read",
              nvGspBootWasRead(0, NVGSP_READ_CPUCTL), false);

    {
        fixtureResetHealthy();

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());

        checkU32("init leaves the mask empty", m.readMask, 0u);
        checkBool("and no bit answers before a single step",
                  nvGspBootWasRead(&m, NVGSP_READ_CPUCTL), false);
    }

    //
    // The five bits must be DISTINCT. A copy-paste duplicate would make two
    // words share one flag, and the publisher would emit an unmeasured value
    // whenever its twin had been read.
    //
    {
        NVU32 all = (NVU32)NVGSP_READ_CPUCTL
                  | (NVU32)NVGSP_READ_HWCFG2
                  | (NVU32)NVGSP_READ_RISCV_CPUCTL
                  | (NVU32)NVGSP_READ_PRIV_ERR_STAT
                  | (NVU32)NVGSP_READ_HUB_ERR_STAT;

        checkU32("the five bits are distinct and disjoint", all, 0x1Fu);
    }

    //
    // Run A -- the healthy boot. Three words read, and the error pair NOT.
    //
    {
        fixtureResetHealthy();

        NVGspBootParams p = nvGspBootObservedParams();
        p.image = fixtureImage(1u, 0u);

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);
        nvGspBootRun(&m, 1000u);

        checkResult("healthy run reaches RUNNING", m.result, NVGSP_RESULT_RUNNING);

        checkBool("A: CPUCTL was read",
                  nvGspBootWasRead(&m, NVGSP_READ_CPUCTL), true);
        checkBool("A: HWCFG2 was read",
                  nvGspBootWasRead(&m, NVGSP_READ_HWCFG2), true);
        checkBool("A: RISCV_CPUCTL was read",
                  nvGspBootWasRead(&m, NVGSP_READ_RISCV_CPUCTL), true);

        //
        // These two are the whole point. A healthy boot stops on ACTIVE before
        // the error check runs, so privErrStat and hubErrStat are still the
        // initialiser's zero -- and were published as measurements.
        //
        checkBool("A: PRIV_ERR_STAT was NOT read",
                  nvGspBootWasRead(&m, NVGSP_READ_PRIV_ERR_STAT), false);
        checkBool("A: HUB_ERR_STAT was NOT read",
                  nvGspBootWasRead(&m, NVGSP_READ_HUB_ERR_STAT), false);

        checkU32("A: mask is exactly the three", m.readMask, 0x07u);
        checkU32("A: and the unread words are still zero", m.privErrStat, 0u);
    }

    //
    // Run B -- a core released that never reports ACTIVE. This is the only
    // route that reaches the error pair, so it is the only run whose mask is
    // full. Pinning both runs is what makes the mask a measurement rather than
    // a constant.
    //
    {
        fixtureReset(OBS_CPUCTL_HALTED, OBS_HWCFG2_SCRUB_DONE, 0u, 0u, 0u);

        //
        // skipFirmwareLoad and a SMALL active budget, matching testTimeouts()
        // and run B of testReadTranslators(). With the DMA path in and the
        // default budget of 200 active polls, the 1000-step backstop trips
        // first and the result is ERR_STEP_BUDGET -- which measures the
        // harness rather than the machine.
        //
        NVGspBootParams p = nvGspBootObservedParams();
        p.skipFirmwareLoad = true;
        p.maxActivePolls   = 5u;

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), p);
        nvGspBootRun(&m, 1000u);

        checkResult("a core that never activates times out",
                    m.result, NVGSP_RESULT_ERR_ACTIVE_TIMEOUT);

        checkBool("B: PRIV_ERR_STAT was read",
                  nvGspBootWasRead(&m, NVGSP_READ_PRIV_ERR_STAT), true);
        checkBool("B: HUB_ERR_STAT was read",
                  nvGspBootWasRead(&m, NVGSP_READ_HUB_ERR_STAT), true);
        checkU32("B: mask is all five", m.readMask, 0x1Fu);
    }

    //
    // Run C -- stopped at the first precondition. ONE word read, four not.
    // This is the shape that made the old publisher worst: four fabricated
    // status words beside one real one, all rendered identically.
    //
    {
        fixtureReset(0u, OBS_HWCFG2_SCRUB_DONE, RISCV_CPUCTL_ACTIVE, 0u, 0u);

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());
        nvGspBootRun(&m, 1000u);

        checkResult("a core that is not halted stops at PROBE",
                    m.result, NVGSP_RESULT_ERR_NOT_HALTED);

        checkBool("C: CPUCTL was read",
                  nvGspBootWasRead(&m, NVGSP_READ_CPUCTL), true);
        checkBool("C: HWCFG2 was NOT read",
                  nvGspBootWasRead(&m, NVGSP_READ_HWCFG2), false);
        checkBool("C: RISCV_CPUCTL was NOT read",
                  nvGspBootWasRead(&m, NVGSP_READ_RISCV_CPUCTL), false);
        checkU32("C: mask is CPUCTL alone", m.readMask, 0x01u);
    }

    //
    // Run D -- halted but not RISC-V capable. TWO words, and it separates the
    // CPUCTL bit from the HWCFG2 bit: a mask that set both at the first read
    // would pass run C and fail here, and one that set neither until the state
    // completed would pass run A and fail both C and D.
    //
    {
        fixtureReset(OBS_CPUCTL_HALTED, 0u, RISCV_CPUCTL_ACTIVE, 0u, 0u);

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());
        nvGspBootRun(&m, 1000u);

        checkResult("a non-RISC-V core stops after HWCFG2",
                    m.result, NVGSP_RESULT_ERR_NOT_RISCV_CAPABLE);

        checkBool("D: CPUCTL was read",
                  nvGspBootWasRead(&m, NVGSP_READ_CPUCTL), true);
        checkBool("D: HWCFG2 was read",
                  nvGspBootWasRead(&m, NVGSP_READ_HWCFG2), true);
        checkBool("D: RISCV_CPUCTL was NOT read",
                  nvGspBootWasRead(&m, NVGSP_READ_RISCV_CPUCTL), false);
        checkU32("D: mask is the two falcon words", m.readMask, 0x03u);
    }

    //
    // A failed READ must not set the bit. This is the direction that matters
    // most: nvGspBootRead() returning false means no value landed, so a mask
    // set before the check would report a word as measured on the exact path
    // where nothing was measured at all.
    //
    {
        fixtureResetHealthy();

        NVGspBoot m;
        nvGspBootInit(&m, fixtureIo(true), nvGspBootObservedParams());
        m.io.read32 = 0;

        nvGspBootRun(&m, 1000u);

        checkResult("a machine with no read callback stops",
                    m.result, NVGSP_RESULT_ERR_NO_IO);
        checkU32("and the mask stayed empty", m.readMask, 0u);
    }
}


int main(void)
{
    std::printf("NVGspBoot.hpp -- Phase 4 GSP bringup state machine\n");
    std::printf("==================================================\n");

    testAddressAgreement();
    testObservedParams();
    testDryRunGate();
    testFirmwareBoundary();
    testFullSequenceOrder();
    testPreconditions();
    testApertureGuard();
    testTimeouts();
    testRiscvErrorSplit();
    testStepDriving();
    testBromParamsStateSequence();
    testReadTranslators();
    testReadMask();
    testNames();
    testDmaSingleChunk();
    testDmaHighAddress();
    testDmaMultiChunk();
    testDmaStateSequence();
    testDmaDrainPolls();
    testDmaWedged();
    testDmaImageValidation();
    testDmaImageBeatsSkip();
    testDmaRespectsDryRun();
    testFbifStateOrdering();
    testFbifWriteFaultPropagates();
    testDmaTransfer();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
