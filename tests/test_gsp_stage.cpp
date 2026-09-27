//
//  test_gsp_stage.cpp
//  Host-side tests for NVGspStage.hpp -- the Phase 5 staging arithmetic
//  behind NV3050Probe::loadGspFirmware().
//
//  Builds and runs under MSVC on Windows, GCC/Clang elsewhere. No IOKit, no
//  GPU, no macOS, and deliberately NO FILE I/O: the blob is synthesised in
//  memory using the same GA107 bootloader geometry test_riscv_ucode.cpp uses,
//  so the suite runs on a checkout without reference/ga107-firmware/.
//
//  WHAT THIS SUITE IS FOR
//
//  Before it existed, NVGspStage.hpp was covered only by the clang front-end
//  parse in check-kext-syntax.bat. That proves the file COMPILES; it says
//  nothing about whether the staging arithmetic is right. Every number this
//  header produces is either a length handed to a DMA engine or a physical
//  address handed to the GPU, so "it compiles" is not coverage.
//
//  ONE THING TO KNOW BEFORE READING THE REFUSAL TESTS
//
//  nvGspStagePlan() opens with nvRiscvUcodeIsWellFormed(), and that function
//  ALREADY rejects a zero-length bootloader region and a region outside the
//  payload (NVRiscvUcode.hpp:306-330). So the plan's own
//  ERR_EMPTY_BOOTLOADER / ERR_REGION_OUTSIDE / ERR_REGION_TRUNCATED branches
//  are defence in depth that the public API cannot reach -- a blob that would
//  trip them fails the well-formed gate first and comes back ERR_MALFORMED.
//
//  These tests assert what the API ACTUALLY returns (ERR_MALFORMED), and say
//  so in the label. Asserting the deeper code instead would be asserting a
//  path that does not execute, which is worse than not testing it: it would
//  read as coverage of a branch nothing exercises.
//

#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVGspStage.hpp"

static int gChecks   = 0;
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
        std::printf("  ok    %-52s 0x%X\n", label, actual);
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-52s got 0x%X, expected 0x%X\n",
                label, actual, expected);
}

static void checkBool(const char *label, bool actual, bool expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-52s %s\n", label, actual ? "true" : "false");
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-52s got %s, expected %s\n",
                label, actual ? "true" : "false", expected ? "true" : "false");
}

//
// Result codes are compared by NAME, not by number. A reordered enum would
// keep every numeric comparison passing while changing what the kext refuses
// on, and the name is what nvGspStageResultName() puts in the kernel log.
//
static void checkResult(const char *label,
                        NVGspStageResult actual,
                        NVGspStageResult expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-52s %s\n", label, nvGspStageResultName(actual));
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-52s got %s, expected %s\n",
                label,
                nvGspStageResultName(actual),
                nvGspStageResultName(expected));
}

static void checkU64(const char *label,
                     unsigned long long actual,
                     unsigned long long expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-52s 0x%llX\n", label, actual);
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-52s got 0x%llX, expected 0x%llX\n",
                label, actual, expected);
}

// ---------------------------------------------------------------------------
// Blob synthesis.
//
// Same approach as test_riscv_ucode.cpp: one static buffer at the real GA107
// bootloader file size, rebuilt per test. Tests that need a malformed blob
// shrink the DECLARED length rather than the allocation, so a bounds bug
// reads live memory instead of crashing -- a crash would hide which check
// failed to fire.
// ---------------------------------------------------------------------------

static unsigned char gBlob[NV_RISCV_GA107_BL_FILE_SIZE];

static void put32(NVU32 off, NVU32 value)
{
    gBlob[off + 0] = (unsigned char)(value & 0xFF);
    gBlob[off + 1] = (unsigned char)((value >> 8) & 0xFF);
    gBlob[off + 2] = (unsigned char)((value >> 16) & 0xFF);
    gBlob[off + 3] = (unsigned char)((value >> 24) & 0xFF);
}

static void putDesc(NVU32 fieldOff, NVU32 value)
{
    put32(NV_RISCV_GA107_BL_HDR_OFFSET + fieldOff, value);
}

//
// Build the GA107 bootloader layout. The bootloader region offset and size
// are parameters so the refusal tests can perturb exactly one field and keep
// everything else witnessed-real.
//
static NVU32 buildBlob(NVU32 blOff, NVU32 blSize)
{
    std::memset(gBlob, 0xA5, sizeof(gBlob));   // payload = recognisable junk

    NVU32 hdrOff  = NV_RISCV_GA107_BL_HDR_OFFSET;
    NVU32 dataOff = NV_RISCV_GA107_BL_DATA_OFFSET;
    NVU32 dataSz  = NV_RISCV_GA107_BL_DATA_SIZE;

    put32(NV_RISCV_BIN_HDR_MAGIC_OFFSET,       NV_RISCV_BIN_HDR_MAGIC);
    put32(NV_RISCV_BIN_HDR_VERSION_OFFSET,     NV_RISCV_BIN_HDR_VERSION_1);
    put32(NV_RISCV_BIN_HDR_BIN_SIZE_OFFSET,    NV_RISCV_GA107_BL_FILE_SIZE);
    put32(NV_RISCV_BIN_HDR_HDR_OFFSET_OFFSET,  hdrOff);
    put32(NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET, dataOff);
    put32(NV_RISCV_BIN_HDR_DATA_SIZE_OFFSET,   dataSz);

    //
    // Zero the DESCRIPTOR specifically, leaving the payload as 0xA5 junk.
    //
    // Not cosmetic. nvRiscvUcodeIsWellFormed() walks six optional region
    // (offset, size) pairs and treats a zero size as "absent"; left at 0xA5
    // every one of them names a 0xA5A5A5A5-byte region outside the payload
    // and the whole blob is refused as MALFORMED. Omitting this line is what
    // made the first run of this suite fail its own happy path.
    //
    std::memset(gBlob + hdrOff, 0, NV_RISCV_GA107_BL_DESC_BYTES);

    putDesc(NV_RISCV_DESC_VERSION_OFFSET,          NV_RISCV_DESC_VERSION_5);
    putDesc(NV_RISCV_DESC_BOOTLOADER_OFFSET,       blOff);
    putDesc(NV_RISCV_DESC_BOOTLOADER_SIZE,         blSize);
    putDesc(NV_RISCV_DESC_BOOTLOADER_PARAM_OFFSET, NV_RISCV_GA107_BL_BOOTLOADER_PARAM_OFF);
    putDesc(NV_RISCV_DESC_BOOTLOADER_PARAM_SIZE,   NV_RISCV_GA107_BL_BOOTLOADER_PARAM_SIZE);
    putDesc(NV_RISCV_DESC_FB_RESERVED_SIZE,        dataSz);

    return NV_RISCV_GA107_BL_FILE_SIZE;
}

//
// The unperturbed real geometry, used by every positive test.
//
static NVU32 buildRealBlob(void)
{
    return buildBlob(NV_RISCV_GA107_BL_BOOTLOADER_OFFSET,
                     NV_RISCV_GA107_BL_BOOTLOADER_SIZE);
}

// ---------------------------------------------------------------------------
// 1. The addressing window.
//
// DMATRFBASE carries physical bits 39:8 and DMATRFBASE1 continues the SHIFTED
// value with physical bits 48:40 (corrected 2026-08-10, t-53137f), so the pair
// reaches 49 bits and the 41 checked here is this driver's POLICY CEILING, not
// the pair's limit. MASK and LIMIT are two different things and the header
// documents a bug that came from conflating them: the mask has its low 8 bits
// clear for alignment, so using it as a magnitude bound rejects every address
// in the low 256 bytes of a valid region.
// ---------------------------------------------------------------------------

static void testWindowConstants(void)
{
    section("1. addressing window constants");

    checkU32("NV_GSP_FW_ADDR_BITS", (NVU32)NV_GSP_FW_ADDR_BITS, 41u);
    checkU64("NV_GSP_FW_PHYS_MASK", NV_GSP_FW_PHYS_MASK, 0x000001FFFFFFFF00ULL);
    checkU64("NV_GSP_FW_PHYS_LIMIT", NV_GSP_FW_PHYS_LIMIT, 0x000001FFFFFFFFFFULL);

    // LIMIT is (1<<41)-1, so it is all-ones and the mask is NOT.
    checkU64("LIMIT == (1<<ADDR_BITS)-1",
             NV_GSP_FW_PHYS_LIMIT,
             (1ULL << NV_GSP_FW_ADDR_BITS) - 1ULL);

    // The distinction that caused the bug, asserted directly: the mask clears
    // the low 8 bits, the limit does not.
    checkU64("MASK low byte is clear", NV_GSP_FW_PHYS_MASK & 0xFFULL, 0ULL);
    checkU64("LIMIT low byte is set", NV_GSP_FW_PHYS_LIMIT & 0xFFULL, 0xFFULL);
    checkBool("MASK != LIMIT", NV_GSP_FW_PHYS_MASK == NV_GSP_FW_PHYS_LIMIT, false);

    checkU32("NVGSP_DMA_CHUNK_BYTES", (NVU32)NVGSP_DMA_CHUNK_BYTES, 256u);
}

// ---------------------------------------------------------------------------
// 2. nvGspStagePadUp() -- round UP to a whole number of 256-byte chunks.
//
// UP, not down: the engine moves one chunk per trigger, so truncating the
// final partial chunk loads a bootloader missing its last instructions.
// ---------------------------------------------------------------------------

static void testPadUp(void)
{
    section("2. nvGspStagePadUp");

    NVU32 out = 0xDEADBEEFu;

    checkBool("exact multiple: 256 -> ok", nvGspStagePadUp(256u, &out), true);
    checkU32("exact multiple: 256 unchanged", out, 256u);

    out = 0xDEADBEEFu;
    checkBool("zero -> ok", nvGspStagePadUp(0u, &out), true);
    checkU32("zero stays zero", out, 0u);

    // Rounds UP, and the direction is the assertion. 1 -> 256, not 0.
    out = 0xDEADBEEFu;
    checkBool("one byte -> ok", nvGspStagePadUp(1u, &out), true);
    checkU32("one byte rounds UP to 256", out, 256u);

    out = 0xDEADBEEFu;
    checkBool("255 -> ok", nvGspStagePadUp(255u, &out), true);
    checkU32("255 rounds UP to 256", out, 256u);

    out = 0xDEADBEEFu;
    checkBool("257 -> ok", nvGspStagePadUp(257u, &out), true);
    checkU32("257 rounds UP to 512", out, 512u);

    // The real bootloader size: 0x880 = 2176 = 8 chunks + 128 bytes.
    out = 0xDEADBEEFu;
    checkBool("real BL size 0x880 -> ok", nvGspStagePadUp(0x880u, &out), true);
    checkU32("0x880 pads to 0x900", out, 0x900u);
    checkU32("padded is a whole chunk count", out % (NVU32)NVGSP_DMA_CHUNK_BYTES, 0u);

    // Overflow: anything above 0xFFFFFF00 needs padding that wraps u32. The
    // result would be a SHORT length handed to a DMA engine, so it must be a
    // refusal rather than a wrapped value.
    out = 0xDEADBEEFu;
    checkBool("0xFFFFFF00 is exact, no pad needed",
              nvGspStagePadUp(0xFFFFFF00u, &out), true);
    checkU32("0xFFFFFF00 unchanged", out, 0xFFFFFF00u);

    out = 0xDEADBEEFu;
    checkBool("0xFFFFFF01 overflows -> refused",
              nvGspStagePadUp(0xFFFFFF01u, &out), false);
    checkU32("*out untouched on overflow", out, 0xDEADBEEFu);

    out = 0xDEADBEEFu;
    checkBool("0xFFFFFFFF overflows -> refused",
              nvGspStagePadUp(0xFFFFFFFFu, &out), false);
    checkU32("*out untouched on 0xFFFFFFFF", out, 0xDEADBEEFu);

    // A null out pointer is a refusal, not a crash.
    checkBool("null out -> refused", nvGspStagePadUp(256u, nullptr), false);
}

// ---------------------------------------------------------------------------
// 3. nvGspStageAddressOk() -- the two hardware constraints on a base the
//    mapper ACTUALLY returned.
//
// Two separate checks on purpose, and the enum is what proves they stayed
// separate: an alignment failure and a magnitude failure must not collapse
// into one "rejected" bool.
// ---------------------------------------------------------------------------

static void testAddressOk(void)
{
    section("3. nvGspStageAddressOk");

    checkResult("zero is aligned and in range",
                nvGspStageAddressOk(0ULL), NVGSP_STAGE_OK);
    checkResult("256 -- one chunk in",
                nvGspStageAddressOk(256ULL), NVGSP_STAGE_OK);
    checkResult("4 KiB page base",
                nvGspStageAddressOk(0x1000ULL), NVGSP_STAGE_OK);

    // Above 4 GiB, which is the whole reason DMATRFBASE1 exists.
    checkResult("above 4 GiB, aligned",
                nvGspStageAddressOk(0x1'0000'0000ULL), NVGSP_STAGE_OK);

    // The exact top of the 41-bit window. LIMIT itself is 0x1FF_FFFF_FFFF,
    // which is NOT 256-aligned, so the highest ACCEPTABLE base is that value
    // with its low byte cleared.
    checkResult("LIMIT itself is misaligned",
                nvGspStageAddressOk(NV_GSP_FW_PHYS_LIMIT),
                NVGSP_STAGE_ERR_ADDR_MISALIGNED);
    checkResult("highest aligned base in window",
                nvGspStageAddressOk(NV_GSP_FW_PHYS_LIMIT & ~0xFFULL),
                NVGSP_STAGE_OK);

    // One chunk past the window, still aligned: must fail on MAGNITUDE, not
    // alignment. If this ever reports MISALIGNED the two checks have been
    // reordered or merged.
    checkResult("first aligned base above the window",
                nvGspStageAddressOk((NV_GSP_FW_PHYS_LIMIT & ~0xFFULL) + 256ULL),
                NVGSP_STAGE_ERR_ADDR_TOO_HIGH);
    checkResult("1 << 41 is too high",
                nvGspStageAddressOk(1ULL << NV_GSP_FW_ADDR_BITS),
                NVGSP_STAGE_ERR_ADDR_TOO_HIGH);

    // Misalignment is checked FIRST, so a base that is both misaligned and
    // too high reports MISALIGNED.
    checkResult("misaligned AND too high -> MISALIGNED wins",
                nvGspStageAddressOk((1ULL << NV_GSP_FW_ADDR_BITS) + 1ULL),
                NVGSP_STAGE_ERR_ADDR_MISALIGNED);

    checkResult("off by one byte",
                nvGspStageAddressOk(1ULL), NVGSP_STAGE_ERR_ADDR_MISALIGNED);
    checkResult("255 -- one short of a chunk",
                nvGspStageAddressOk(255ULL), NVGSP_STAGE_ERR_ADDR_MISALIGNED);
    checkResult("128 -- half a chunk",
                nvGspStageAddressOk(128ULL), NVGSP_STAGE_ERR_ADDR_MISALIGNED);

    //
    // The documented bug, asserted as a regression test rather than described
    // in a comment: an address in the low 256 bytes of a valid region passes
    // the LIMIT check. Comparing against MASK instead would reject it.
    //
    checkBool("low-region address is NOT rejected by LIMIT",
              0x100ULL <= NV_GSP_FW_PHYS_LIMIT, true);
    checkBool("...but IS lost by a MASK comparison",
              (0x80ULL & NV_GSP_FW_PHYS_MASK) == 0x80ULL, false);
}

// ---------------------------------------------------------------------------
// 3b. nvGspStageRangeOk() -- the BASE plus the LENGTH.
//
// nvGspStageAddressOk() takes one address and no length, so it cannot see an
// image whose start fits the 41-bit window and whose tail does not. The engine
// is programmed with a base and driven by a length, so that image would make it
// walk past the addressable region.
//
// The separating inputs are the ones where base alone passes: a base at or just
// below the ceiling. A base far from the ceiling passes under BOTH the correct
// reading and a broken one that ignores lengthBytes entirely, so those cases
// prove nothing on their own and are here only as the negative control.
//
// The overflow case is the one that matters most. A naive implementation writes
// `physBase + lengthBytes <= LIMIT`; with a length near 2^64 that sum WRAPS to
// a small number and the check passes. The assertion below fails against that
// implementation and passes against the subtraction-on-the-ceiling form.
// ---------------------------------------------------------------------------

static void testRangeOk(void)
{
    section("3b. nvGspStageRangeOk");

    // Negative control: nowhere near the ceiling, so this passes under every
    // reading of the length term. It is here to prove the function is not
    // simply refusing everything.
    checkResult("low base, small length -> OK",
                nvGspStageRangeOk(0x1000ULL, 0x10000ULL), NVGSP_STAGE_OK);

    // Exactly flush with the ceiling: the last byte of the image is the last
    // addressable byte. Off-by-one in either direction changes this verdict,
    // which is what makes it a separating input.
    const unsigned long long kSpan = 0x10000ULL;
    const unsigned long long kFlushBase =
        (NV_GSP_FW_PHYS_LIMIT + 1ull) - kSpan;
    checkResult("image ends on the last addressable byte -> OK",
                nvGspStageRangeOk(kFlushBase, kSpan), NVGSP_STAGE_OK);

    // One chunk further up with the same length: the base still passes
    // nvGspStageAddressOk(), the RANGE does not. This is the bug the function
    // exists for, and it is the case a base-only check reports as fine.
    checkResult("base still legal, tail is not -> RANGE_TOO_HIGH",
                nvGspStageRangeOk(kFlushBase + NVGSP_DMA_CHUNK_BYTES, kSpan),
                NVGSP_STAGE_ERR_RANGE_TOO_HIGH);
    checkResult("...and that same base alone is accepted",
                nvGspStageAddressOk(kFlushBase + NVGSP_DMA_CHUNK_BYTES),
                NVGSP_STAGE_OK);

    // One byte too long from a flush base. Distinguishes <= from <.
    checkResult("one byte past the end -> RANGE_TOO_HIGH",
                nvGspStageRangeOk(kFlushBase, kSpan + 1ull),
                NVGSP_STAGE_ERR_RANGE_TOO_HIGH);

    // Wrap-around. `physBase + lengthBytes` overflows to a small value here, so
    // an addition-based check would return OK.
    checkResult("length that wraps 64 bits -> RANGE_TOO_HIGH",
                nvGspStageRangeOk(0x100ULL, 0xFFFFFFFFFFFFFF00ULL),
                NVGSP_STAGE_ERR_RANGE_TOO_HIGH);

    // The base-level codes must be FORWARDED, not collapsed into a range code.
    // A caller logs this name, and a misalignment reported as RANGE_TOO_HIGH
    // would send the reader looking at the wrong constraint.
    checkResult("misaligned base forwards MISALIGNED",
                nvGspStageRangeOk(0x101ULL, 0x100ULL),
                NVGSP_STAGE_ERR_ADDR_MISALIGNED);
    checkResult("base above the window forwards ADDR_TOO_HIGH",
                nvGspStageRangeOk(NV_GSP_FW_PHYS_LIMIT + 1ull, 0x100ULL),
                NVGSP_STAGE_ERR_ADDR_TOO_HIGH);

    // Zero length is accepted deliberately -- an empty image is refused by
    // nvGspStagePlan() before any address exists, and refusing it again here
    // would report a length fault as a range fault.
    checkResult("zero length -> OK (plan owns the empty case)",
                nvGspStageRangeOk(0x1000ULL, 0ull), NVGSP_STAGE_OK);

    // The real GA107 padded length from a plausible base, as the .cpp uses it.
    checkResult("GA107 padded length from a low base -> OK",
                nvGspStageRangeOk(0x40000000ULL,
                                  (unsigned long long)NV_RISCV_GA107_BL_DATA_SIZE),
                NVGSP_STAGE_OK);
}

// ---------------------------------------------------------------------------
// 4. nvGspStageFillImage() -- split a validated base into the hi/lo pair.
//
// A physAddrHi that dropped the shift would look correct for every address
// below 4 GiB, so the above-4-GiB cases are the ones that matter.
// ---------------------------------------------------------------------------

static void testFillImage(void)
{
    section("4. nvGspStageFillImage");

    NVGspFirmwareImage img;
    std::memset(&img, 0xEE, sizeof(img));
    img.valid = false;

    nvGspStageFillImage(&img, 0x1000ULL, 0x900u, 0u);
    checkU32("below 4 GiB: lo", img.physAddrLo, 0x1000u);
    checkU32("below 4 GiB: hi is zero", img.physAddrHi, 0u);
    checkU32("sizeBytes carried through", img.sizeBytes, 0x900u);
    checkU32("imemOffset carried through", img.imemOffset, 0u);
    checkBool("valid set", img.valid, true);

    // Exactly 4 GiB: lo wraps to zero and hi becomes 1. A dropped shift would
    // leave hi at 0 here and target page zero.
    std::memset(&img, 0xEE, sizeof(img));
    nvGspStageFillImage(&img, 0x1'0000'0000ULL, 256u, 0u);
    checkU32("at 4 GiB: lo is zero", img.physAddrLo, 0u);
    checkU32("at 4 GiB: hi is one", img.physAddrHi, 1u);

    // A base with distinct halves, so a swapped assignment cannot pass.
    std::memset(&img, 0xEE, sizeof(img));
    nvGspStageFillImage(&img, 0x0000'01AB'CDEF'1200ULL, 0x900u, 0x400u);
    checkU32("split base: lo", img.physAddrLo, 0xCDEF1200u);
    checkU32("split base: hi", img.physAddrHi, 0x000001ABu);
    checkU32("nonzero imemOffset", img.imemOffset, 0x400u);

    // Top of the window.
    std::memset(&img, 0xEE, sizeof(img));
    nvGspStageFillImage(&img, NV_GSP_FW_PHYS_LIMIT & ~0xFFULL, 256u, 0u);
    checkU32("window top: lo", img.physAddrLo, 0xFFFFFF00u);
    checkU32("window top: hi", img.physAddrHi, 0x1FFu);

    // Recombining the halves must give back the base exactly.
    unsigned long long rebuilt =
        ((unsigned long long)img.physAddrHi << 32) | img.physAddrLo;
    checkU64("hi/lo recombine to the base",
             rebuilt, NV_GSP_FW_PHYS_LIMIT & ~0xFFULL);

    // A null image pointer is a no-op, not a crash.
    nvGspStageFillImage(nullptr, 0x1000ULL, 256u, 0u);
    checkBool("null img did not crash", true, true);
}

// ---------------------------------------------------------------------------
// 5. nvGspStagePlan() -- the happy path against the real GA107 geometry.
//
// Region offsets are relative to dataOffset, NOT to the file start. Getting
// that wrong reads the descriptor bytes as firmware -- a plausible-looking
// image that boots nothing -- so the file offset is asserted as an absolute
// number, not as "whatever the header computed".
// ---------------------------------------------------------------------------

static void testPlanHappyPath(void)
{
    section("5. nvGspStagePlan -- real GA107 bootloader");

    NVU32 len = buildRealBlob();

    NVGspStagePlan plan;
    std::memset(&plan, 0xEE, sizeof(plan));

    checkResult("real blob is accepted",
                nvGspStagePlan(gBlob, len, &plan), NVGSP_STAGE_OK);
    checkBool("plan marked valid", plan.valid, true);

    // dataOffset(108) + regionOffset(0x5000) = 108 + 20480 = 20588 = 0x506C.
    checkU32("sourceOffset is dataOffset + regionOffset",
             plan.sourceOffset,
             (NVU32)(NV_RISCV_GA107_BL_DATA_OFFSET
                     + NV_RISCV_GA107_BL_BOOTLOADER_OFFSET));
    checkU32("sourceOffset absolute value", plan.sourceOffset, 20588u);

    // The region offset alone would be 0x5000. Asserting they differ is what
    // catches a plan that forgot to add dataOffset.
    checkBool("sourceOffset != bare region offset",
              plan.sourceOffset == NV_RISCV_GA107_BL_BOOTLOADER_OFFSET, false);

    checkU32("sourceBytes is the declared region size",
             plan.sourceBytes, (NVU32)NV_RISCV_GA107_BL_BOOTLOADER_SIZE);
    checkU32("sourceBytes absolute value", plan.sourceBytes, 0x880u);

    checkU32("paddedBytes rounds 0x880 up to 0x900", plan.paddedBytes, 0x900u);
    checkBool("paddedBytes >= sourceBytes",
              plan.paddedBytes >= plan.sourceBytes, true);
    checkU32("paddedBytes is a whole chunk count",
             plan.paddedBytes % (NVU32)NVGSP_DMA_CHUNK_BYTES, 0u);

    // The whole padded copy must stay inside the file, not just the source
    // bytes -- the caller allocates paddedBytes and copies sourceBytes, so a
    // read of the padded length off the blob would run past the end.
    checkBool("source region fits the file",
              plan.sourceOffset + plan.sourceBytes <= len, true);

    // Zero-fill tail: 0x900 - 0x880 = 0x80 bytes the caller must zero.
    checkU32("zero-fill tail is 0x80",
             plan.paddedBytes - plan.sourceBytes, 0x80u);
}

// ---------------------------------------------------------------------------
// 6. nvGspStagePlan() -- refusals.
//
// See the header comment at the top of this file: nvRiscvUcodeIsWellFormed()
// runs FIRST and already rejects an empty or out-of-payload bootloader
// region, so those blobs come back ERR_MALFORMED rather than reaching the
// plan's own more specific codes. Each label below says which code the API
// actually produces and why.
// ---------------------------------------------------------------------------

static void testPlanRefusals(void)
{
    section("6. nvGspStagePlan -- refusals");

    NVGspStagePlan plan;
    NVU32 len = buildRealBlob();

    // Null and empty inputs.
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("null blob",
                nvGspStagePlan(nullptr, len, &plan), NVGSP_STAGE_ERR_NO_BLOB);
    checkBool("plan invalidated on null blob", plan.valid, false);

    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("zero length",
                nvGspStagePlan(gBlob, 0u, &plan), NVGSP_STAGE_ERR_NO_BLOB);
    checkBool("plan invalidated on zero length", plan.valid, false);

    checkResult("null out-plan",
                nvGspStagePlan(gBlob, len, nullptr), NVGSP_STAGE_ERR_NO_BLOB);

    // Every refusal must leave the plan fields zeroed, not merely invalid --
    // a caller that ignores the return value must not be able to build a copy
    // out of leftover numbers.
    std::memset(&plan, 0xEE, sizeof(plan));
    (void)nvGspStagePlan(nullptr, len, &plan);
    checkU32("sourceOffset zeroed on refusal", plan.sourceOffset, 0u);
    checkU32("sourceBytes zeroed on refusal", plan.sourceBytes, 0u);
    checkU32("paddedBytes zeroed on refusal", plan.paddedBytes, 0u);

    // Truncated file: the declared length is shortened so the payload no
    // longer fits. nvRiscvBinHdrIsValid() catches it.
    len = buildRealBlob();
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("declared length shorter than the payload -> MALFORMED",
                nvGspStagePlan(gBlob, NV_RISCV_BIN_HDR_SIZE + 4u, &plan),
                NVGSP_STAGE_ERR_MALFORMED);
    checkBool("plan invalid on truncated file", plan.valid, false);

    // Bad magic.
    len = buildRealBlob();
    put32(NV_RISCV_BIN_HDR_MAGIC_OFFSET, 0xDEADBEEFu);
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("wrong container magic -> MALFORMED",
                nvGspStagePlan(gBlob, len, &plan), NVGSP_STAGE_ERR_MALFORMED);

    // Descriptor too short to carry the fields Phase 4 relies on.
    len = buildRealBlob();
    put32(NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET,
          NV_RISCV_GA107_BL_HDR_OFFSET + 16u);
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("16-byte descriptor -> MALFORMED",
                nvGspStagePlan(gBlob, len, &plan), NVGSP_STAGE_ERR_MALFORMED);

    //
    // Zero-length bootloader region. Until 2026-08-18 this asserted MALFORMED
    // and this comment explained that ERR_EMPTY_BOOTLOADER was a branch that
    // does not execute -- the suite documented its own subject's dead arm and
    // asserted the collapse rather than the fact. nvRiscvUcodeVerdict() now
    // distinguishes the cases and nvGspStagePlan() maps them, so the precise
    // code is both reachable and the one asserted.
    //
    len = buildBlob(NV_RISCV_GA107_BL_BOOTLOADER_OFFSET, 0u);
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("empty bootloader -> EMPTY_BOOTLOADER",
                nvGspStagePlan(gBlob, len, &plan),
                NVGSP_STAGE_ERR_EMPTY_BOOTLOADER);
    checkBool("plan invalid on empty bootloader", plan.valid, false);

    //
    // Region outside the payload. Same story, same fix: the containment test
    // lives in the verdict, and its answer now arrives here under its own name
    // rather than as MALFORMED.
    //
    // These three are the separating inputs for EMPTY vs OUTSIDE. An empty
    // region trivially satisfies containment, so a verdict that tested
    // containment first would report OUTSIDE for the case above and these four
    // assertions would agree with each other while being wrong about which
    // fact was measured.
    //
    len = buildBlob(NV_RISCV_GA107_BL_DATA_SIZE, 0x880u);
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("region starts at end of payload -> REGION_OUTSIDE",
                nvGspStagePlan(gBlob, len, &plan),
                NVGSP_STAGE_ERR_REGION_OUTSIDE);

    len = buildBlob(NV_RISCV_GA107_BL_DATA_SIZE - 0x100u, 0x880u);
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("region runs past the payload end -> REGION_OUTSIDE",
                nvGspStagePlan(gBlob, len, &plan),
                NVGSP_STAGE_ERR_REGION_OUTSIDE);

    len = buildBlob(0xFFFFFF00u, 0x880u);
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("region offset near u32 max -> REGION_OUTSIDE",
                nvGspStagePlan(gBlob, len, &plan),
                NVGSP_STAGE_ERR_REGION_OUTSIDE);

    // A region at the very end of the payload, exactly fitting, IS accepted.
    // This is the boundary the overflow-safe nvRiscvFits() has to get right.
    len = buildBlob(NV_RISCV_GA107_BL_DATA_SIZE - 0x880u, 0x880u);
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("region exactly filling the payload tail is accepted",
                nvGspStagePlan(gBlob, len, &plan), NVGSP_STAGE_OK);
    checkU32("tail region sourceOffset",
             plan.sourceOffset,
             (NVU32)(NV_RISCV_GA107_BL_DATA_OFFSET
                     + NV_RISCV_GA107_BL_DATA_SIZE - 0x880u));

    // Restore the real blob so no later test inherits a perturbed one.
    (void)buildRealBlob();
}

// ---------------------------------------------------------------------------
// 7. Plan and address checks composed, the way the .cpp uses them.
//
// The plan produces a LENGTH; the address check validates a BASE the mapper
// returned. They are independent, and the composition is what the kext does.
// ---------------------------------------------------------------------------

static void testPlanThenImage(void)
{
    section("7. plan -> address check -> image");

    NVU32 len = buildRealBlob();

    NVGspStagePlan plan;
    std::memset(&plan, 0xEE, sizeof(plan));
    checkResult("plan ok", nvGspStagePlan(gBlob, len, &plan), NVGSP_STAGE_OK);

    // A plausible mapper answer above 4 GiB, chunk-aligned.
    const unsigned long long base = 0x0000'0001'2345'6700ULL;
    checkResult("mapper base accepted",
                nvGspStageAddressOk(base), NVGSP_STAGE_OK);

    NVGspFirmwareImage img;
    std::memset(&img, 0xEE, sizeof(img));
    nvGspStageFillImage(&img, base, plan.paddedBytes, 0u);

    checkU32("image lo", img.physAddrLo, 0x23456700u);
    checkU32("image hi", img.physAddrHi, 0x1u);
    checkU32("image size is the PADDED length", img.sizeBytes, 0x900u);
    checkBool("image size is not the raw length",
              img.sizeBytes == plan.sourceBytes, false);
    checkU32("image size is a whole chunk count",
             img.sizeBytes % (NVU32)NVGSP_DMA_CHUNK_BYTES, 0u);
    checkBool("image valid", img.valid, true);

    // A mapper answer that is misaligned must be refused BEFORE it reaches
    // fillImage -- that is the ordering the .cpp depends on.
    checkResult("misaligned mapper base refused",
                nvGspStageAddressOk(base + 1ULL),
                NVGSP_STAGE_ERR_ADDR_MISALIGNED);
}

// ---------------------------------------------------------------------------
// 8. nvGspStageResultName() -- every code has a distinct, non-empty name.
//
// The name is what lands in the kernel log, and a missing switch arm would
// silently print "UNKNOWN" for a real refusal.
// ---------------------------------------------------------------------------

static void testResultNames(void)
{
    section("8. nvGspStageResultName");

    const NVGspStageResult all[] = {
        NVGSP_STAGE_OK,
        NVGSP_STAGE_ERR_NO_BLOB,
        NVGSP_STAGE_ERR_MALFORMED,
        NVGSP_STAGE_ERR_EMPTY_BOOTLOADER,
        NVGSP_STAGE_ERR_REGION_OUTSIDE,
        NVGSP_STAGE_ERR_REGION_TRUNCATED,
        NVGSP_STAGE_ERR_PAD_OVERFLOW,
        NVGSP_STAGE_ERR_ADDR_MISALIGNED,
        NVGSP_STAGE_ERR_ADDR_TOO_HIGH,
        NVGSP_STAGE_ERR_RANGE_TOO_HIGH,
    };
    const int n = (int)(sizeof(all) / sizeof(all[0]));

    for (int i = 0; i < n; ++i) {
        const char *name = nvGspStageResultName(all[i]);
        gChecks++;
        if (name != nullptr && name[0] != '\0'
            && std::strcmp(name, "UNKNOWN") != 0) {
            std::printf("  ok    %-52s %s\n", "named", name);
        } else {
            gFailures++;
            std::printf("  FAIL  %-52s code %d has no name\n",
                        "named", (int)all[i]);
        }
    }

    // Distinctness: two codes sharing a string would make the log ambiguous.
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            gChecks++;
            if (std::strcmp(nvGspStageResultName(all[i]),
                            nvGspStageResultName(all[j])) != 0) {
                continue;
            }
            gFailures++;
            std::printf("  FAIL  %-52s %s\n",
                        "duplicate name", nvGspStageResultName(all[i]));
        }
    }
    std::printf("  ok    %-52s %d codes, all distinct\n", "names distinct", n);

    // An out-of-range value falls through to UNKNOWN rather than reading off
    // the end of a table.
    checkBool("out-of-range code -> UNKNOWN",
              std::strcmp(nvGspStageResultName((NVGspStageResult)999),
                          "UNKNOWN") == 0,
              true);
}

// ---------------------------------------------------------------------------
// 10. POLICY <= REPRESENTABLE -- the relationship between the two bounds.
//
// NV_GSP_FW_PHYS_LIMIT is a POLICY ceiling (41 bits, this driver's choice).
// NVGSP_DMA_PHYSHI_MASK is a REPRESENTABILITY bound (17 bits of physHi, so 49
// bits with physLo underneath). Their order is also checked by
// tools/check-registers.py::stage_sanity_checks(), which derives the pair's
// reach from NVIDIA's published field widths -- better provenance, but it needs
// Python. This section pins the same invariant against the constant the kext
// actually packs with, from a suite that runs on a bare MSVC build.
//
// nvGspStagePolicyFitsRegisters() is a comparison between two compile-time
// constants, so calling it can only ever return true on the current header --
// driving it alone would be an assertion that cannot fail. The checks below
// therefore also feed the comparison a SEPARATING input: the limit a widened
// NV_GSP_FW_ADDR_BITS would produce, which must come out ABOVE the representable
// limit. That is what proves the predicate discriminates rather than merely
// agreeing with itself.
// ---------------------------------------------------------------------------

static void testPolicyFitsRegisters(void)
{
    section("10. policy ceiling vs register representability");

    // The representable limit must be built from the register field, not a
    // literal 49. 0xFF | (0x1FF << 8) == 0x1FFFF is 17 bits of physHi.
    checkU32("NVGSP_DMA_PHYSHI_MASK", (NVU32)NVGSP_DMA_PHYSHI_MASK, 0x1FFFFu);
    checkU64("NV_GSP_DMA_REPRESENTABLE_LIMIT",
             NV_GSP_DMA_REPRESENTABLE_LIMIT, 0x0001FFFFFFFFFFFFULL);

    // 49 bits, spelled independently of the mask above.
    checkU64("REPRESENTABLE_LIMIT == (1<<49)-1",
             NV_GSP_DMA_REPRESENTABLE_LIMIT, (1ULL << 49) - 1ULL);

    // The invariant itself.
    checkBool("nvGspStagePolicyFitsRegisters()",
              nvGspStagePolicyFitsRegisters(), true);
    checkBool("POLICY_LIMIT <= REPRESENTABLE_LIMIT",
              NV_GSP_FW_PHYS_LIMIT <= NV_GSP_DMA_REPRESENTABLE_LIMIT, true);

    // Strict, not merely <=: the policy ceiling is 8 bits below the pair's
    // reach today. Equality would be legal but would mean the margin is gone,
    // and that is worth noticing rather than passing silently.
    checkBool("POLICY_LIMIT < REPRESENTABLE_LIMIT",
              NV_GSP_FW_PHYS_LIMIT < NV_GSP_DMA_REPRESENTABLE_LIMIT, true);

    // -- separating inputs: the comparison must be able to come out false --
    //
    // A ceiling of exactly 49 bits is still admissible (equal, not above).
    checkBool("a 49-bit ceiling would still fit",
              ((1ULL << 49) - 1ULL) <= NV_GSP_DMA_REPRESENTABLE_LIMIT, true);

    // 50 bits does NOT fit, and this is the case that would fire if someone
    // widened NV_GSP_FW_ADDR_BITS on an arithmetic argument.
    checkBool("a 50-bit ceiling would NOT fit",
              ((1ULL << 50) - 1ULL) <= NV_GSP_DMA_REPRESENTABLE_LIMIT, false);

    // And the one bit above the pair's reach is the boundary that separates
    // them, so the bound is not off by one in either direction.
    checkBool("REPRESENTABLE_LIMIT + 1 does NOT fit",
              (NV_GSP_DMA_REPRESENTABLE_LIMIT + 1ULL)
                  <= NV_GSP_DMA_REPRESENTABLE_LIMIT, false);

    // The two bounds are genuinely different numbers. If a later edit ever
    // makes the stage header's limit equal the register limit, every check
    // above except the strict one still passes, so pin the distance.
    checkU32("ADDR_BITS is 8 below the pair's 49",
             (NVU32)(49u - (NVU32)NV_GSP_FW_ADDR_BITS), 8u);

    // Finally: an address the POLICY refuses can still be representable. That
    // asymmetry is the point of keeping the two checks separate -- refusing
    // something the hardware would have taken is safe; the reverse is not.
    {
        const unsigned long long justAbovePolicy = NV_GSP_FW_PHYS_LIMIT + 1ULL;
        checkBool("just above POLICY is refused by the stage check",
                  justAbovePolicy <= NV_GSP_FW_PHYS_LIMIT, false);
        checkBool("...yet the registers could have carried it",
                  justAbovePolicy <= NV_GSP_DMA_REPRESENTABLE_LIMIT, true);
    }

    // -- and now through the PREDICATE, not through the two limits --
    //
    // Everything above compares NV_GSP_FW_PHYS_LIMIT against
    // NV_GSP_DMA_REPRESENTABLE_LIMIT: two integers, one `<`. That is a
    // statement about two constants. It is NOT a statement about the code the
    // DMA path actually runs, which is nvGspDmaPhysHiFits() -- the predicate
    // NVGSP_STATE_FIRMWARE_LOAD consults before packing BASE/BASE1.
    //
    // The two can disagree. NV_GSP_DMA_REPRESENTABLE_LIMIT is assembled here
    // from a bit count; nvGspDmaPhysHiFits() masks with
    // NVGSP_DMA_PHYSHI_MASK, which is assembled from NVIDIA's published BASE1
    // field width. A mistake in either assembly leaves the `<` above green
    // while the predicate refuses -- or admits -- a different set of addresses
    // than the limit says it should. handoff9 §2 is exactly that failure mode
    // one level down: the pair TRUNCATES above 17 bits rather than faulting,
    // so an address wrongly admitted here is a silent overlapping DMA, not an
    // error.
    //
    // So drive the predicate with the high word of the policy ceiling itself.
    // If the driver's own maximum permitted address cannot be packed, every
    // legal allocation at the top of the window programs the wrong BASE1.
    {
        const NVU32 policyHi = (NVU32)(NV_GSP_FW_PHYS_LIMIT >> 32);

        // 41-bit ceiling -> high word 0x1FF. Spelled as a literal as well as
        // derived, because deriving it from the same NV_GSP_FW_ADDR_BITS the
        // predicate's margin was computed from would make the check agree with
        // itself no matter which value is wrong.
        checkU32("policy ceiling's high word", policyHi, 0x1FFu);

        checkBool("the POLICY ceiling's high word packs into BASE/BASE1",
                  nvGspDmaPhysHiFits(policyHi), true);

        // Separating inputs. Without these the check above is one that has only
        // ever seen a value 8 bits inside the boundary -- a mask widened or
        // narrowed by several bits would still admit 0x1FF, so a green result
        // there says nothing about where the predicate's edge actually is.
        checkBool("physHi 0x1FFFF (17 bits, the last that fits) is accepted",
                  nvGspDmaPhysHiFits(0x1FFFFu), true);
        checkBool("physHi 0x20000 (bit 17) is refused",
                  nvGspDmaPhysHiFits(0x20000u), false);

        // The far side of the boundary pair. An off-by-one in the mask moves
        // the edge by one bit and both checks above still read correctly under
        // the wrong reading if only one of them is present.
        checkBool("physHi 0x10000 (bit 16, inside) is accepted",
                  nvGspDmaPhysHiFits(0x10000u), true);
        checkBool("physHi 0xFFFFFFFF (every bit) is refused",
                  nvGspDmaPhysHiFits(0xFFFFFFFFu), false);

        // And the tie-back: the predicate's edge and the limit constant must
        // describe the same address. REPRESENTABLE_LIMIT's high word is the
        // largest physHi that fits, by construction -- if these two ever
        // disagree, one of the two assemblies is wrong and the `<` comparison
        // above cannot tell you which.
        const NVU32 limitHi = (NVU32)(NV_GSP_DMA_REPRESENTABLE_LIMIT >> 32);
        checkBool("REPRESENTABLE_LIMIT's high word is the predicate's maximum",
                  nvGspDmaPhysHiFits(limitHi) && !nvGspDmaPhysHiFits(limitHi + 1u),
                  true);
    }
}

int main(void)
{
    std::printf("NVGspStage.hpp -- Phase 5 staging arithmetic tests\n");
    std::printf("==================================================\n");

    testWindowConstants();
    testPadUp();
    testAddressOk();
    testRangeOk();
    testFillImage();
    testPlanHappyPath();
    testPlanRefusals();
    testPlanThenImage();
    testResultNames();
    testPolicyFitsRegisters();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
