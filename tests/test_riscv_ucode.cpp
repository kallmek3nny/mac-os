//
//  test_riscv_ucode.cpp
//  Host-side tests for NVRiscvUcode.hpp -- RM_RISCV_UCODE_DESC parsing.
//
//  Builds and runs under MSVC on Windows. No IOKit, no GPU, no macOS.
//
//  TWO SECTIONS, and the split is deliberate. Everything up to
//  testDescriptorVersion() is SYNTHETIC: every blob is built in memory from the
//  values parsed out of the real firmware, so that part runs identically on a
//  machine without reference/ga107-firmware/ checked out. testRealFirmwareFiles()
//  is the opposite -- it opens the actual files, and SKIPS with a banner and no
//  checks when they are absent, so the portability property above still holds.
//
//  The second section exists because the first one cannot do its job alone.
//  Every synthetic blob comes from buildBlob(), which builds the BOOTLOADER
//  layout, so 134 checks over this header could not discover that the two
//  booters carry a DIFFERENT structure in the same region. A suite that only
//  ever constructs the shape it expects cannot find a second shape.
//
//  The values it synthesises were read from the actual blob pulled off the
//  target laptop (bootloader-570.144.bin, nvidia-595-open 595.84):
//
//    file 24684 bytes, hdrOffset 24, dataOffset 108, dataSize 24576
//    descriptor 84 bytes = 21 u32 fields
//    bootloader 0x5000 + 0x880, param 0x5880 + 0x10
//
//  The bug this file exists to prevent is the variable-length descriptor. The
//  v610 struct has 23 fields; this blob carries 21. A sizeof-based read walks
//  8 bytes into signed firmware and calls the result bIsSmp / bIsPlicEnabled.
//  Nothing faults, and the GSP just never boots.
//

//
// testRealFirmwareFiles() calls fopen(). MSVC's /W4 deprecates it in favour of
// fopen_s, which does not exist off MSVC -- and this suite compiles under GCC
// and Apple clang too. Silencing the warning keeps one portable spelling.
//
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVRiscvUcode.hpp"

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
// Prints the NAME on both arms rather than the integer. A failure here is
// somebody's refusal landing on the wrong code, and "got 3, expected 4" makes
// the reader go and count enumerators.
//
static void checkVerdict(const char *label, NVRiscvUcodeVerdict actual,
                         NVRiscvUcodeVerdict expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-50s %s\n", label, nvRiscvUcodeVerdictName(actual));
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-50s got %s, expected %s\n", label,
                nvRiscvUcodeVerdictName(actual),
                nvRiscvUcodeVerdictName(expected));
}

// ---------------------------------------------------------------------------
// Blob synthesis.
//
// One static buffer, rebuilt per test. Big enough for the real bootloader
// layout; the tests that need a malformed blob shrink the *declared* length
// rather than the allocation, so an out-of-bounds read would land on live
// memory and go undetected -- which is exactly what the bounds checks must
// prevent, and what a shorter allocation would hide behind a crash.
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
// Build the GA107 bootloader layout. `descBytes` is a parameter so the
// short-descriptor cases reuse the same builder.
//
static NVU32 buildBlob(NVU32 descBytes)
{
    std::memset(gBlob, 0xA5, sizeof(gBlob));   // payload = recognisable junk

    NVU32 hdrOff  = NV_RISCV_GA107_BL_HDR_OFFSET;
    NVU32 dataOff = hdrOff + descBytes;
    NVU32 dataSz  = NV_RISCV_GA107_BL_DATA_SIZE;

    put32(NV_RISCV_BIN_HDR_MAGIC_OFFSET,       NV_RISCV_BIN_HDR_MAGIC);
    put32(NV_RISCV_BIN_HDR_VERSION_OFFSET,     NV_RISCV_BIN_HDR_VERSION_1);
    put32(NV_RISCV_BIN_HDR_BIN_SIZE_OFFSET,    24832);
    put32(NV_RISCV_BIN_HDR_HDR_OFFSET_OFFSET,  hdrOff);
    put32(NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET, dataOff);
    put32(NV_RISCV_BIN_HDR_DATA_SIZE_OFFSET,   dataSz);

    std::memset(gBlob + hdrOff, 0, descBytes);

    putDesc(NV_RISCV_DESC_VERSION_OFFSET,          NV_RISCV_DESC_VERSION_5);
    putDesc(NV_RISCV_DESC_BOOTLOADER_OFFSET,       NV_RISCV_GA107_BL_BOOTLOADER_OFFSET);
    putDesc(NV_RISCV_DESC_BOOTLOADER_SIZE,         NV_RISCV_GA107_BL_BOOTLOADER_SIZE);
    putDesc(NV_RISCV_DESC_BOOTLOADER_PARAM_OFFSET, NV_RISCV_GA107_BL_BOOTLOADER_PARAM_OFF);
    putDesc(NV_RISCV_DESC_BOOTLOADER_PARAM_SIZE,   NV_RISCV_GA107_BL_BOOTLOADER_PARAM_SIZE);
    putDesc(NV_RISCV_DESC_FB_RESERVED_SIZE,        dataSz);

    return dataOff + dataSz;   // total length, = 24684 for the real descriptor
}

// ---------------------------------------------------------------------------

//
// The layout arithmetic the whole header rests on. If the synthesised blob
// does not reproduce the real file's geometry then every test below is
// checking a fiction.
//
static void testObservedGeometry(void)
{
    section("observed GA107 bootloader geometry");

    NVU32 len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);

    checkU32("synthesised length matches the real file",
             len, NV_RISCV_GA107_BL_FILE_SIZE);
    checkU32("magic is the NVIDIA vendor ID",
             nvRiscvBinHdrMagic(gBlob), 0x000010DEu);
    checkU32("hdrOffset", nvRiscvBinHdrOffset(gBlob), 24u);
    checkU32("dataOffset", nvRiscvBinDataOffset(gBlob), 108u);
    checkU32("dataSize", nvRiscvBinDataSize(gBlob), 24576u);

    //
    // PINNING the container-header offsets. buildBlob() writes each field
    // THROUGH the same define the assertions above read back, so the two
    // cancel: swap NV_RISCV_BIN_HDR_VERSION_OFFSET with
    // NV_RISCV_BIN_HDR_BIN_SIZE_OFFSET -- or shift the whole block by one u32
    // -- and every check in this file still passes, because the fixture moved
    // with the accessor. version and bin_size are the worst of it: neither has
    // an accessor at all, so nothing reads them back through any path.
    //
    // Reading at BARE LITERAL byte offsets is the separating input. The
    // expected values are the real file's, from the container table in
    // reference/ga107-firmware/README.md:
    //
    //   bin_magic 0x10de  bin_ver 1  bin_size 24832
    //   hdr_off 24        data_off 108  data_size 24576
    //
    // bin_size (24832) is legitimately LARGER than the file on disk (24684):
    // it is the padded image length and the trailing pad is not stored. That
    // is why it is asserted as its own value rather than against len.
    //
    checkU32("byte 0x00 holds bin_magic", nvRiscvRead32(gBlob, 0x00u), 0x000010DEu);
    checkU32("byte 0x04 holds bin_ver 1", nvRiscvRead32(gBlob, 0x04u), 1u);
    checkU32("byte 0x08 holds bin_size 24832",
             nvRiscvRead32(gBlob, 0x08u), 24832u);
    checkU32("byte 0x0C holds hdr_offset 24", nvRiscvRead32(gBlob, 0x0Cu), 24u);
    checkU32("byte 0x10 holds data_offset 108",
             nvRiscvRead32(gBlob, 0x10u), 108u);
    checkU32("byte 0x14 holds data_size 24576",
             nvRiscvRead32(gBlob, 0x14u), 24576u);

    //
    // 108 - 24 = 84. This subtraction IS the fix -- it is where the descriptor
    // length comes from instead of sizeof.
    //
    checkU32("descriptor bytes = dataOffset - hdrOffset",
             nvRiscvDescBytes(gBlob), 84u);
    checkU32("  which is 21 u32 fields", nvRiscvDescFieldCount(gBlob), 21u);

    checkBool("header validates", nvRiscvBinHdrIsValid(gBlob, len), true);
    checkBool("blob is well-formed", nvRiscvUcodeIsWellFormed(gBlob, len), true);
}

//
// THE TEST THIS FILE EXISTS FOR.
//
// bIsSmp and bIsPlicEnabled are at descriptor offsets 0x54 and 0x58. The
// descriptor is 84 (0x54) bytes long, so 0x54 is one byte PAST the end -- the
// classic off-by-one boundary. Reading it yields payload.
//
static void testAbsentFields(void)
{
    section("variable-length descriptor (the silent-boot-hang class of bug)");

    NVU32 len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);

    checkBool("bSignedAsCode (0x50) IS present",
              nvRiscvDescHasField(gBlob, NV_RISCV_DESC_SIGNED_AS_CODE), true);
    checkBool("bIsSmp (0x54) is NOT present",
              nvRiscvDescHasField(gBlob, NV_RISCV_DESC_IS_SMP), false);
    checkBool("bIsPlicEnabled (0x58) is NOT present",
              nvRiscvDescHasField(gBlob, NV_RISCV_DESC_IS_PLIC_ENABLED), false);
    checkBool("the SMP-flags predicate agrees",
              nvRiscvDescHasSmpFlags(gBlob), false);

    //
    // 0x50 + 4 == 84 exactly. The last field must be readable, and the byte
    // after it must not be -- a >= vs > slip in nvRiscvDescHasField() shows up
    // here and nowhere else.
    //
    checkU32("last field ends exactly at the descriptor end",
             NV_RISCV_DESC_SIGNED_AS_CODE + 4u, nvRiscvDescBytes(gBlob));

    //
    // The payload is 0xA5 filler, so an unguarded read of bIsSmp would return
    // 0xA5A5A5A5 -- nonzero, i.e. "SMP enabled". Assert we get the fallback
    // instead. This is the whole failure in one assertion.
    //
    checkU32("absent bIsSmp returns the fallback, not payload",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_IS_SMP, 0u), 0u);
    checkU32("absent bIsPlicEnabled returns the fallback",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_IS_PLIC_ENABLED, 0u), 0u);
    checkU32("  and the fallback is honoured, not hardcoded to 0",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_IS_SMP, 0xDEADu), 0xDEADu);

    //
    // What the unguarded read would have produced. Asserted explicitly so the
    // hazard is visible in the test output rather than described in a comment.
    //
    checkU32("raw bytes at that offset really are payload junk",
             nvRiscvRead32(gBlob, nvRiscvBinHdrOffset(gBlob)
                                      + NV_RISCV_DESC_IS_SMP), 0xA5A5A5A5u);

    //
    // A longer descriptor must expose the fields. Same code, 92-byte blob:
    // the predicate flips. This proves the guard keys off the blob, not off a
    // constant that happens to match GA107.
    //
    len = buildBlob(92);
    putDesc(NV_RISCV_DESC_IS_SMP, 1u);
    putDesc(NV_RISCV_DESC_IS_PLIC_ENABLED, 1u);

    checkU32("92-byte descriptor is 23 fields", nvRiscvDescFieldCount(gBlob), 23u);
    checkBool("bIsSmp now present", nvRiscvDescHasSmpFlags(gBlob), true);
    checkU32("and reads the real value, not the fallback",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_IS_SMP, 0u), 1u);
    checkBool("still well-formed", nvRiscvUcodeIsWellFormed(gBlob, len), true);

    //
    // SEPARATING INPUT for nvRiscvDescHasSmpFlags(). It keys off
    // NV_RISCV_DESC_IS_PLIC_ENABLED (0x58), the HIGHER of the two flags, so a
    // blob that has one flag but not the other is what tells it apart from the
    // mutant that keys off NV_RISCV_DESC_IS_SMP (0x54). The 84-byte blob has
    // NEITHER field (both predicates false) and the 92-byte blob has BOTH
    // (both true) -- every assertion above passes unchanged under the mutant.
    //
    // 88 bytes is the one length that separates them: 0x54 + 4 == 88 fits,
    // 0x58 + 4 == 92 does not. Reading bIsPlicEnabled here would return 0xA5
    // payload as "PLIC enabled", which is the silent-boot-hang failure this
    // file exists for -- reached by a blob one field shorter than the one the
    // existing checks use.
    //
    len = buildBlob(88);
    putDesc(NV_RISCV_DESC_IS_SMP, 1u);

    checkU32("88-byte descriptor is 22 fields", nvRiscvDescFieldCount(gBlob), 22u);
    checkBool("  bIsSmp (0x54) IS present at 22 fields",
              nvRiscvDescHasField(gBlob, NV_RISCV_DESC_IS_SMP), true);
    checkBool("  bIsPlicEnabled (0x58) is still NOT present",
              nvRiscvDescHasField(gBlob, NV_RISCV_DESC_IS_PLIC_ENABLED), false);
    checkBool("  so the SMP-flags predicate must be false, not true",
              nvRiscvDescHasSmpFlags(gBlob), false);
    checkU32("  bIsSmp reads its real value",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_IS_SMP, 0u), 1u);
    checkU32("  bIsPlicEnabled falls back instead of reading payload",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_IS_PLIC_ENABLED, 0xDEADu),
             0xDEADu);
    checkBool("  and 88 bytes is still well-formed",
              nvRiscvUcodeIsWellFormed(gBlob, len), true);
}

//
// Field offsets against rmRiscvUcode.h:36. The struct is append-only, so an
// offset error is a permanent misread of every blob, not a version skew.
//
static void testFieldOffsets(void)
{
    section("descriptor field offsets (rmRiscvUcode.h:36)");

    //
    // CONTAINER offsets first (nvfw_bin_hdr). These were the three the
    // separating-input sweep found undefended: buildBlob() writes version and
    // bin_size THROUGH the same define an assertion would read them back with,
    // so the fixture and the accessor cancel and a swapped or shifted define
    // still passes every check. version and bin_size have no accessor at all,
    // so no other path reads them either.
    //
    // Comparing the define against a bare literal is what breaks the
    // cancellation: the literal cannot move when the define does. Values are
    // the fixed six-u32 layout in NVRiscvUcode.hpp:59.
    //
    checkU32("bin_magic offset", NV_RISCV_BIN_HDR_MAGIC_OFFSET, 0x00u);
    checkU32("bin_ver offset", NV_RISCV_BIN_HDR_VERSION_OFFSET, 0x04u);
    checkU32("bin_size offset", NV_RISCV_BIN_HDR_BIN_SIZE_OFFSET, 0x08u);
    checkU32("hdr_offset offset", NV_RISCV_BIN_HDR_HDR_OFFSET_OFFSET, 0x0Cu);
    checkU32("data_offset offset", NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET, 0x10u);
    checkU32("data_size offset", NV_RISCV_BIN_HDR_DATA_SIZE_OFFSET, 0x14u);
    checkU32("six u32 spans exactly the 24-byte container",
             NV_RISCV_BIN_HDR_DATA_SIZE_OFFSET + 4u, NV_RISCV_BIN_HDR_SIZE);

    //
    // The FULL 0x00..0x58 sequence, every define against a bare literal.
    //
    // This list used to skip the size fields -- riscvElfSize, manifestSize,
    // monitorDataSize, monitorCodeSize, swbromCodeSize, swbromDataOffset,
    // swbromDataSize -- on the reasoning that the offset fields around them
    // were pinned. That does not hold: the gap between two pinned offsets is
    // exactly where a define can move undetected, because the fixture writes
    // the field through the same define the accessor reads it back with, so
    // the two cancel. A skipped entry is a hole, not a covered neighbour.
    //
    // Being an unbroken 4-byte progression is itself the assertion. Any
    // single define swapped, shifted, or duplicated breaks the sequence here
    // even when it breaks nothing else in the file.
    //
    checkU32("version", NV_RISCV_DESC_VERSION_OFFSET, 0x00u);
    checkU32("bootloaderOffset", NV_RISCV_DESC_BOOTLOADER_OFFSET, 0x04u);
    checkU32("bootloaderSize", NV_RISCV_DESC_BOOTLOADER_SIZE, 0x08u);
    checkU32("bootloaderParamOffset", NV_RISCV_DESC_BOOTLOADER_PARAM_OFFSET, 0x0Cu);
    checkU32("bootloaderParamSize", NV_RISCV_DESC_BOOTLOADER_PARAM_SIZE, 0x10u);
    checkU32("riscvElfOffset", NV_RISCV_DESC_RISCV_ELF_OFFSET, 0x14u);
    checkU32("riscvElfSize", NV_RISCV_DESC_RISCV_ELF_SIZE, 0x18u);
    checkU32("appVersion", NV_RISCV_DESC_APP_VERSION, 0x1Cu);
    checkU32("manifestOffset", NV_RISCV_DESC_MANIFEST_OFFSET, 0x20u);
    checkU32("manifestSize", NV_RISCV_DESC_MANIFEST_SIZE, 0x24u);
    checkU32("monitorDataOffset", NV_RISCV_DESC_MONITOR_DATA_OFFSET, 0x28u);
    checkU32("monitorDataSize", NV_RISCV_DESC_MONITOR_DATA_SIZE, 0x2Cu);
    checkU32("monitorCodeOffset", NV_RISCV_DESC_MONITOR_CODE_OFFSET, 0x30u);
    checkU32("monitorCodeSize", NV_RISCV_DESC_MONITOR_CODE_SIZE, 0x34u);
    checkU32("bIsMonitorEnabled", NV_RISCV_DESC_IS_MONITOR_ENABLED, 0x38u);
    checkU32("swbromCodeOffset", NV_RISCV_DESC_SWBROM_CODE_OFFSET, 0x3Cu);
    checkU32("swbromCodeSize", NV_RISCV_DESC_SWBROM_CODE_SIZE, 0x40u);
    checkU32("swbromDataOffset", NV_RISCV_DESC_SWBROM_DATA_OFFSET, 0x44u);
    checkU32("swbromDataSize", NV_RISCV_DESC_SWBROM_DATA_SIZE, 0x48u);
    checkU32("fbReservedSize", NV_RISCV_DESC_FB_RESERVED_SIZE, 0x4Cu);
    checkU32("bSignedAsCode", NV_RISCV_DESC_SIGNED_AS_CODE, 0x50u);
    checkU32("bIsSmp", NV_RISCV_DESC_IS_SMP, 0x54u);
    checkU32("bIsPlicEnabled", NV_RISCV_DESC_IS_PLIC_ENABLED, 0x58u);

    //
    // The two field COUNTS are derived from those offsets, so pin the
    // relationship rather than the numbers: a count that drifts from the
    // offset it is supposed to describe is what silently mis-sizes a
    // descriptor.
    //
    checkU32("21 fields reaches exactly through bSignedAsCode",
             (NV_RISCV_DESC_SIGNED_AS_CODE / 4u) + 1u,
             NV_RISCV_DESC_FIELDS_THROUGH_SIGNED_AS_CODE);
    checkU32("23 fields reaches exactly through bIsPlicEnabled",
             (NV_RISCV_DESC_IS_PLIC_ENABLED / 4u) + 1u,
             NV_RISCV_DESC_FIELDS_THROUGH_PLIC);

    // Every field is a u32 slot; a gap or overlap means a transcription error.
    checkU32("21 fields spans exactly 84 bytes",
             NV_RISCV_DESC_FIELDS_THROUGH_SIGNED_AS_CODE * 4u, 84u);
    checkU32("23 fields spans exactly 92 bytes",
             NV_RISCV_DESC_FIELDS_THROUGH_PLIC * 4u, 92u);
    checkU32("the container header is 24 bytes = 6 u32",
             NV_RISCV_BIN_HDR_SIZE, 6u * 4u);

    //
    // The descriptor's observed values, read back through the accessors rather
    // than compared as constants.
    //
    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    checkU32("version field reads 5 (sign-whole-image-as-code)",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_VERSION_OFFSET, 0u),
             NV_RISCV_DESC_VERSION_5);
    checkU32("bootloaderOffset reads 0x5000",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_BOOTLOADER_OFFSET, 0u), 0x5000u);
    checkU32("bootloaderSize reads 0x880",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_BOOTLOADER_SIZE, 0u), 0x880u);

    //
    // The param block starts exactly where the bootloader ends. Adjacency is
    // not guaranteed by the format, but it holds on this blob and a change
    // would mean the firmware layout moved under us.
    //
    checkU32("param block begins where the bootloader ends",
             NV_RISCV_GA107_BL_BOOTLOADER_OFFSET + NV_RISCV_GA107_BL_BOOTLOADER_SIZE,
             NV_RISCV_GA107_BL_BOOTLOADER_PARAM_OFF);
}

//
// Region offsets are PAYLOAD-relative. Treating them as file-relative reads
// 108 bytes early -- i.e. reads the descriptor as though it were firmware.
//
static void testRegionAddressing(void)
{
    section("region offsets are payload-relative, not file-relative");

    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);

    NVU32 blOff  = NV_RISCV_GA107_BL_BOOTLOADER_OFFSET;
    NVU32 blSize = NV_RISCV_GA107_BL_BOOTLOADER_SIZE;

    checkBool("bootloader region is inside the payload",
              nvRiscvRegionInPayload(gBlob, blOff, blSize), true);
    checkU32("its file offset is dataOffset + regionOffset",
             nvRiscvRegionFileOffset(gBlob, blOff), 108u + 0x5000u);
    checkBool("which is NOT the raw region offset",
              nvRiscvRegionFileOffset(gBlob, blOff) == blOff, false);
    checkU32("  the difference is exactly the header+descriptor",
             nvRiscvRegionFileOffset(gBlob, blOff) - blOff, 108u);

    // Param block, same rule.
    checkBool("param region inside the payload",
              nvRiscvRegionInPayload(gBlob, NV_RISCV_GA107_BL_BOOTLOADER_PARAM_OFF,
                                     NV_RISCV_GA107_BL_BOOTLOADER_PARAM_SIZE), true);

    //
    // A region ending exactly at the payload end is legal; one byte more is
    // not. Boundary conditions are where these checks actually get tested.
    //
    NVU32 dataSz = NV_RISCV_GA107_BL_DATA_SIZE;
    checkBool("region ending exactly at payload end is accepted",
              nvRiscvRegionInPayload(gBlob, dataSz - 16u, 16u), true);
    checkBool("one byte past the end is rejected",
              nvRiscvRegionInPayload(gBlob, dataSz - 16u, 17u), false);
    checkBool("zero-size at the very end is accepted",
              nvRiscvRegionInPayload(gBlob, dataSz, 0u), true);
    checkBool("offset past the end is rejected even at size 0",
              nvRiscvRegionInPayload(gBlob, dataSz + 1u, 0u), false);

    //
    // Overflow. off + size wraps to a small number on 32-bit arithmetic, so a
    // naive `off + size <= len` accepts a region that covers the entire address
    // space. nvRiscvFits() never performs that addition.
    //
    checkBool("0xFFFFFFF0 + 0x20 does not wrap into acceptance",
              nvRiscvRegionInPayload(gBlob, 0xFFFFFFF0u, 0x20u), false);
    checkBool("huge size rejected",
              nvRiscvRegionInPayload(gBlob, 0u, 0xFFFFFFFFu), false);
}

//
// Malformed input. The kext reads these blobs out of a file it does not
// control, in kernel space. Every rejection here is a rejection instead of an
// out-of-bounds read.
//
static void testMalformed(void)
{
    section("malformed blobs must be refused, not parsed");

    NVU32 len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);

    // Truncated below the container header.
    checkBool("length 0 rejected", nvRiscvBinHdrIsValid(gBlob, 0u), false);
    checkBool("length 23 rejected (one short of the header)",
              nvRiscvBinHdrIsValid(gBlob, 23u), false);
    checkBool("length 24 is enough for the header itself, but payload is absent",
              nvRiscvBinHdrIsValid(gBlob, 24u), false);

    // Payload declared longer than the file.
    checkBool("declared payload past EOF rejected",
              nvRiscvBinHdrIsValid(gBlob, len - 1u), false);
    checkBool("exact length accepted", nvRiscvBinHdrIsValid(gBlob, len), true);

    // Wrong magic -- a file that is not an nvfw container at all.
    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    put32(NV_RISCV_BIN_HDR_MAGIC_OFFSET, 0x10DE0000u);
    checkBool("byte-swapped magic rejected", nvRiscvBinHdrIsValid(gBlob, len), false);
    put32(NV_RISCV_BIN_HDR_MAGIC_OFFSET, 0u);
    checkBool("zero magic rejected", nvRiscvBinHdrIsValid(gBlob, len), false);

    // dataOffset <= hdrOffset means a zero or negative-length descriptor.
    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    put32(NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET, NV_RISCV_GA107_BL_HDR_OFFSET);
    checkBool("zero-length descriptor rejected", nvRiscvBinHdrIsValid(gBlob, len), false);

    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    put32(NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET, 8u);
    checkBool("dataOffset before hdrOffset rejected",
              nvRiscvBinHdrIsValid(gBlob, len), false);

    // hdrOffset inside the container header would overlap the header itself.
    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    put32(NV_RISCV_BIN_HDR_HDR_OFFSET_OFFSET, 4u);
    checkBool("descriptor overlapping the container header rejected",
              nvRiscvBinHdrIsValid(gBlob, len), false);

    // Descriptor not a whole number of u32 slots.
    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    put32(NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET, NV_RISCV_GA107_BL_DATA_OFFSET + 2u);
    checkBool("non-multiple-of-4 descriptor rejected",
              nvRiscvBinHdrIsValid(gBlob, len), false);

    //
    // A descriptor too short to carry the fields Phase 4 needs. The header
    // itself is valid, so only nvRiscvUcodeIsWellFormed() catches this -- the
    // two checks are not interchangeable.
    //
    NVU32 shortLen = buildBlob(40u);   // 10 fields
    checkBool("40-byte descriptor: container header still valid",
              nvRiscvBinHdrIsValid(gBlob, shortLen), true);
    checkU32("  10 fields", nvRiscvDescFieldCount(gBlob), 10u);
    checkBool("  but not well-formed for Phase 4",
              nvRiscvUcodeIsWellFormed(gBlob, shortLen), false);

    //
    // A region that runs off the end of the payload. Header valid, descriptor
    // long enough, values lying.
    //
    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_BOOTLOADER_SIZE, NV_RISCV_GA107_BL_DATA_SIZE);
    checkBool("bootloader region overrunning the payload rejected",
              nvRiscvUcodeIsWellFormed(gBlob, len), false);

    // A zero-size bootloader is not a usable image either.
    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_BOOTLOADER_SIZE, 0u);
    checkBool("zero-size bootloader rejected",
              nvRiscvUcodeIsWellFormed(gBlob, len), false);

    //
    // Zero-size optional regions are normal on this blob and must NOT be
    // treated as errors -- riscvElf, manifest, monitor and swbrom are all zero
    // in the real bootloader. Rejecting them would reject the actual firmware.
    //
    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    checkU32("real blob has manifestSize 0",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_MANIFEST_SIZE, 0u), 0u);
    checkU32("real blob has riscvElfSize 0",
             nvRiscvDescField(gBlob, NV_RISCV_DESC_RISCV_ELF_SIZE, 0u), 0u);
    checkBool("  and is still well-formed",
              nvRiscvUcodeIsWellFormed(gBlob, len), true);

    //
    // But a non-zero optional region that overruns is still an error -- and
    // that has to be asserted for EVERY row of nvRiscvUcodeIsWellFormed()'s
    // pairs[] table, not only the one that happened to get written here first.
    //
    // The reason is the `size != 0 &&` short-circuit in that loop. The real
    // bootloader leaves all six optional sizes at zero, and buildBlob()
    // reproduces it faithfully, so every row is skipped before
    // nvRiscvRegionInPayload() is ever called. A row DELETED from pairs[] is
    // therefore indistinguishable from a row present in it: the previous
    // single manifest case left five of the six rows unreachable, and
    // duplicating row 2 over row 6 kept the whole suite green.
    //
    // Each row also carries a zero-size positive control, because an overrun
    // check that reddened for some unrelated reason -- a field offset past the
    // end of an 84-byte descriptor, say -- would look identical to a working
    // bound. The control writes the same bogus offset and pins that the row is
    // ignored once its size returns to zero, which attributes the rejection to
    // the size field rather than to the offset write.
    //
    static const struct {
        const char *name;
        NVU32       offsetField;
        NVU32       sizeField;
    } optionalRegions[] = {
        { "riscvElf",    NV_RISCV_DESC_RISCV_ELF_OFFSET,    NV_RISCV_DESC_RISCV_ELF_SIZE    },
        { "manifest",    NV_RISCV_DESC_MANIFEST_OFFSET,     NV_RISCV_DESC_MANIFEST_SIZE     },
        { "monitorData", NV_RISCV_DESC_MONITOR_DATA_OFFSET, NV_RISCV_DESC_MONITOR_DATA_SIZE },
        { "monitorCode", NV_RISCV_DESC_MONITOR_CODE_OFFSET, NV_RISCV_DESC_MONITOR_CODE_SIZE },
        { "swbromCode",  NV_RISCV_DESC_SWBROM_CODE_OFFSET,  NV_RISCV_DESC_SWBROM_CODE_SIZE  },
        { "swbromData",  NV_RISCV_DESC_SWBROM_DATA_OFFSET,  NV_RISCV_DESC_SWBROM_DATA_SIZE  },
    };

    const unsigned regionCount =
        (unsigned)(sizeof(optionalRegions) / sizeof(optionalRegions[0]));
    checkU32("pairs[] has six optional rows", regionCount, 6u);

    for (unsigned i = 0; i < regionCount; ++i) {
        char label[96];

        //
        // The size field must land inside the 84-byte descriptor or the write
        // is a no-op and the check below passes against nothing. swbromData's
        // size sits at 0x48, so 0x48 + 4 = 0x4C <= 0x54 -- every row is
        // reachable, but pin it rather than reason about it.
        //
        std::snprintf(label, sizeof(label), "%s size field is inside the descriptor",
                      optionalRegions[i].name);
        checkBool(label,
                  optionalRegions[i].sizeField + 4u <= NV_RISCV_GA107_BL_DESC_BYTES,
                  true);

        len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
        putDesc(optionalRegions[i].offsetField, NV_RISCV_GA107_BL_DATA_SIZE - 4u);
        putDesc(optionalRegions[i].sizeField, 64u);
        std::snprintf(label, sizeof(label), "  overrunning %s rejected",
                      optionalRegions[i].name);
        checkBool(label, nvRiscvUcodeIsWellFormed(gBlob, len), false);

        putDesc(optionalRegions[i].sizeField, 0u);
        std::snprintf(label, sizeof(label), "  zero-size %s ignores the same offset",
                      optionalRegions[i].name);
        checkBool(label, nvRiscvUcodeIsWellFormed(gBlob, len), true);
    }
}

//
// The booters carry a 864-byte descriptor against the bootloader's 84 -- same
// driver release, same struct name. This is the evidence that the length must
// come from the header, recorded as an assertion so it cannot be forgotten.
//
static void testBooterLayout(void)
{
    section("booter blobs carry a different descriptor length");

    checkU32("bootloader descriptor", NV_RISCV_GA107_BL_DESC_BYTES, 84u);
    checkU32("booter descriptor", NV_RISCV_GA107_BOOTER_DESC_BYTES, 864u);
    checkBool("they differ",
              NV_RISCV_GA107_BL_DESC_BYTES == NV_RISCV_GA107_BOOTER_DESC_BYTES, false);

    checkU32("booter dataOffset = hdrOffset + descriptor",
             NV_RISCV_GA107_BOOTER_HDR_OFFSET + NV_RISCV_GA107_BOOTER_DESC_BYTES,
             NV_RISCV_GA107_BOOTER_DATA_OFFSET);
    checkU32("bootloader dataOffset = hdrOffset + descriptor",
             NV_RISCV_GA107_BL_HDR_OFFSET + NV_RISCV_GA107_BL_DESC_BYTES,
             NV_RISCV_GA107_BL_DATA_OFFSET);

    //
    // 864 bytes is 216 fields -- far more than the 23 the struct declares. The
    // surplus is trailing signature material, which is precisely why a
    // sizeof-based reader must not assume descriptor length equals struct size
    // in EITHER direction.
    //
    checkU32("booter descriptor is 216 u32 slots",
             NV_RISCV_GA107_BOOTER_DESC_BYTES / 4u, 216u);
    checkBool("which exceeds the declared struct",
              (NV_RISCV_GA107_BOOTER_DESC_BYTES / 4u)
                  > NV_RISCV_DESC_FIELDS_THROUGH_PLIC, true);
}

//
// The descriptor VERSION gate, added 2026-08-18.
//
// Measured against all three files in reference/ga107-firmware/: only
// bootloader-570.144.bin carries an RM_RISCV_UCODE_DESC in the region between
// hdrOffset and dataOffset. Both booters carry an nvfw_hs_header_v2 there --
// same container magic, same hdrOffset of 24 -- whose first word is 60 (0x3C),
// its own `header_size`, not a structure version.
//
// The separating input is the one this section exists for: a blob whose
// container header is valid, whose descriptor is long enough, and whose
// optional rows are ALL ZERO -- so every pre-existing check in this file passes
// it -- differing from the bootloader only in the version word. Before the
// gate, nvRiscvUcodeIsWellFormed() returned true on exactly that shape.
//
static void testDescriptorVersion(void)
{
    section("descriptor version establishes the blob type");

    NVU32 len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);

    checkU32("the real bootloader is version 5",
             nvRiscvDescVersion(gBlob), NV_RISCV_DESC_VERSION_5);
    checkBool("and is well-formed",
              nvRiscvUcodeIsWellFormed(gBlob, len), true);

    //
    // THE SEPARATING INPUT. Only the version word differs from the check
    // directly above, and every optional row is still zero, so nothing else in
    // this suite distinguishes the two.
    //
    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_VERSION_OFFSET, 60u);
    checkU32("an HS header's header_size reads as version 60",
             nvRiscvDescVersion(gBlob), 60u);
    checkBool("and is rejected",
              nvRiscvUcodeIsWellFormed(gBlob, len), false);

    //
    // An EQUALITY, not a floor. Version 6 would append fields the way 5 did and
    // would otherwise parse; it is rejected on purpose, so that whoever meets
    // one extends the gate deliberately rather than discovering it worked.
    //
    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_VERSION_OFFSET, 6u);
    checkBool("a HIGHER version is rejected too, not accepted as a superset",
              nvRiscvUcodeIsWellFormed(gBlob, len), false);

    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_VERSION_OFFSET, 4u);
    checkBool("and so is a lower one",
              nvRiscvUcodeIsWellFormed(gBlob, len), false);

    buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_VERSION_OFFSET, 0u);
    checkBool("zero is not a free pass",
              nvRiscvUcodeIsWellFormed(gBlob, len), false);

    //
    // The fallback. A descriptor too short to hold its own version must not
    // report the version we happen to know -- it must report a value no
    // consumer accepts. Read through the same presence-checked accessor as
    // every other field, so offset 0 cannot reach the payload's first word.
    //
    buildBlob(0u);
    checkU32("a zero-length descriptor reports UNKNOWN, not 5",
             nvRiscvDescVersion(gBlob), NV_RISCV_DESC_VERSION_UNKNOWN);
    checkBool("and UNKNOWN differs from the version we accept",
              NV_RISCV_DESC_VERSION_UNKNOWN == NV_RISCV_DESC_VERSION_5, false);

    //
    // Precedence: the length test runs BEFORE the version test, so a short
    // descriptor is refused for being short rather than for its version word.
    // Both refuse, but only one of them is a claim somebody can act on.
    //
    NVU32 shortLen = buildBlob(NV_RISCV_DESC_VERSION_OFFSET + 4u);
    checkBool("a descriptor holding ONLY a valid version is still refused",
              nvRiscvUcodeIsWellFormed(gBlob, shortLen), false);
}

//
// The little-endian loader itself. It is spelled byte-by-byte to avoid an
// unaligned deref, so it needs its own coverage -- a byte-order slip here
// corrupts every value above at once.
//
static void testRead32(void)
{
    section("unaligned little-endian load");

    std::memset(gBlob, 0, sizeof(gBlob));
    gBlob[0] = 0x78; gBlob[1] = 0x56; gBlob[2] = 0x34; gBlob[3] = 0x12;
    checkU32("aligned load is little-endian", nvRiscvRead32(gBlob, 0), 0x12345678u);

    // Same bytes at an odd offset -- the case a NVU32* cast would make UB.
    gBlob[5] = 0x78; gBlob[6] = 0x56; gBlob[7] = 0x34; gBlob[8] = 0x12;
    checkU32("unaligned load agrees", nvRiscvRead32(gBlob, 5), 0x12345678u);

    // High bit set -- catches a signed-char promotion sign-extending the load.
    gBlob[12] = 0x00; gBlob[13] = 0x00; gBlob[14] = 0x00; gBlob[15] = 0x80;
    checkU32("high bit does not sign-extend", nvRiscvRead32(gBlob, 12), 0x80000000u);

    gBlob[16] = 0xFF; gBlob[17] = 0xFF; gBlob[18] = 0xFF; gBlob[19] = 0xFF;
    checkU32("all ones", nvRiscvRead32(gBlob, 16), 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// THE FILE-DRIVEN SECTION.
//
// Everything above this line is synthetic, and that is exactly why the booter
// defect survived 134 green checks: buildBlob() builds the BOOTLOADER layout,
// so no synthetic input could ever carry an nvfw_hs_header_v2. This section
// opens the real files.
//
// It SKIPS -- banner, zero checks, no failure -- when the directory is absent,
// so a bare checkout still runs the suite. That means the counts differ between
// a full tree and a bare one, which is intended: the skip banner is the record
// of which reading you are looking at.
//
// Paths are relative to the SOURCE directory, injected by CMake as
// NV_FIRMWARE_DIR. A relative path from the binary's cwd would depend on where
// ctest was invoked from, and a wrong path would SKIP rather than fail -- the
// vacuity direction, and the worst possible failure mode for this section.
// ---------------------------------------------------------------------------

#ifndef NV_FIRMWARE_DIR
#define NV_FIRMWARE_DIR ""
#endif

//
// Read a whole file into `out`. Returns the byte count, or 0 when the file is
// not there. 0 means ABSENT and nothing else: every real blob here is thousands
// of bytes, and a genuinely empty file would fail the container header test
// anyway rather than pass silently.
//
static NVU32 loadFile(const char *name, unsigned char *out, NVU32 cap)
{
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s", NV_FIRMWARE_DIR, name);

    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        return 0;
    }

    size_t got = std::fread(out, 1, (size_t)cap, f);
    std::fclose(f);
    return (NVU32)got;
}

//
// 64 KiB covers the largest of the three (booter_load, 61304 bytes) with room
// left. A file that fills the buffer exactly would be indistinguishable from a
// truncated read, so the size assertions below are equalities against the
// measured lengths rather than floors.
//
static unsigned char gFile[65536];

static void testRealFirmwareFiles(void)
{
    section("the REAL firmware files, parsed from disk");

    NVU32 blLen = loadFile("bootloader-570.144.bin", gFile, sizeof(gFile));
    if (blLen == 0) {
        std::printf("  SKIPPED -- reference/ga107-firmware/ is not in this"
                    " checkout.\n");
        std::printf("  The synthetic sections above still ran. This section"
                    " adds NO checks.\n");
        return;
    }

    // ---- bootloader: the one blob that really does carry a descriptor ----

    checkU32("bootloader file length", blLen, NV_RISCV_GA107_BL_FILE_SIZE);
    checkU32("bootloader container magic",
             nvRiscvBinHdrMagic(gFile), NV_RISCV_BIN_HDR_MAGIC);
    checkBool("bootloader container header is valid",
              nvRiscvBinHdrIsValid(gFile, blLen), true);
    checkU32("bootloader hdrOffset",
             nvRiscvBinHdrOffset(gFile), NV_RISCV_GA107_BL_HDR_OFFSET);
    checkU32("bootloader dataOffset",
             nvRiscvBinDataOffset(gFile), NV_RISCV_GA107_BL_DATA_OFFSET);
    checkU32("bootloader dataSize",
             nvRiscvBinDataSize(gFile), NV_RISCV_GA107_BL_DATA_SIZE);
    checkU32("bootloader descriptor is 84 bytes",
             nvRiscvDescBytes(gFile), NV_RISCV_GA107_BL_DESC_BYTES);
    checkU32("bootloader descriptor is 21 fields",
             nvRiscvDescFieldCount(gFile),
             NV_RISCV_DESC_FIELDS_THROUGH_SIGNED_AS_CODE);
    checkBool("bootloader lacks bIsSmp / bIsPlicEnabled",
              nvRiscvDescHasSmpFlags(gFile), false);

    //
    // The version word, read off the real file rather than off a word I typed.
    // This is the check that makes the whole section worth its cost.
    //
    checkU32("bootloader descriptor version is 5",
             nvRiscvDescVersion(gFile), NV_RISCV_DESC_VERSION_5);

    checkU32("bootloader bootloaderOffset",
             nvRiscvDescField(gFile, NV_RISCV_DESC_BOOTLOADER_OFFSET, 0),
             NV_RISCV_GA107_BL_BOOTLOADER_OFFSET);
    checkU32("bootloader bootloaderSize",
             nvRiscvDescField(gFile, NV_RISCV_DESC_BOOTLOADER_SIZE, 0),
             NV_RISCV_GA107_BL_BOOTLOADER_SIZE);
    checkU32("bootloader paramOffset",
             nvRiscvDescField(gFile, NV_RISCV_DESC_BOOTLOADER_PARAM_OFFSET, 0),
             NV_RISCV_GA107_BL_BOOTLOADER_PARAM_OFF);
    checkU32("bootloader paramSize",
             nvRiscvDescField(gFile, NV_RISCV_DESC_BOOTLOADER_PARAM_SIZE, 0),
             NV_RISCV_GA107_BL_BOOTLOADER_PARAM_SIZE);

    //
    // MEASURED 2026-08-18, and it CONTRADICTS what this suite assumed.
    // handoff18 recorded that the real bootloader leaves all six optional rows
    // zero, and buildBlob() reproduces that. Three of them are NOT zero:
    //
    //   manifest   off 0x00000000 size 0x00000800
    //   monData    off 0x00000800 size 0x00001000
    //   monCode    off 0x00001800 size 0x00002900
    //
    // and bIsMonitorEnabled is 1. Only elf and the two swbrom rows are zero.
    // That is why these are pinned here: the synthetic fixture's all-zero rows
    // are a shape the real firmware does not have, so the loop generalised in
    // handoff18 was exercising a blob strictly simpler than the one that ships.
    //
    checkU32("real manifestSize is NOT zero",
             nvRiscvDescField(gFile, NV_RISCV_DESC_MANIFEST_SIZE, 0), 0x00000800u);
    checkU32("real monitorDataOffset",
             nvRiscvDescField(gFile, NV_RISCV_DESC_MONITOR_DATA_OFFSET, 0), 0x00000800u);
    checkU32("real monitorDataSize",
             nvRiscvDescField(gFile, NV_RISCV_DESC_MONITOR_DATA_SIZE, 0), 0x00001000u);
    checkU32("real monitorCodeOffset",
             nvRiscvDescField(gFile, NV_RISCV_DESC_MONITOR_CODE_OFFSET, 0), 0x00001800u);
    checkU32("real monitorCodeSize",
             nvRiscvDescField(gFile, NV_RISCV_DESC_MONITOR_CODE_SIZE, 0), 0x00002900u);
    checkU32("real bIsMonitorEnabled is set",
             nvRiscvDescField(gFile, NV_RISCV_DESC_IS_MONITOR_ENABLED, 0), 1u);
    checkU32("real elfSize is zero",
             nvRiscvDescField(gFile, NV_RISCV_DESC_RISCV_ELF_SIZE, 0), 0u);
    checkU32("real swbromCodeSize is zero",
             nvRiscvDescField(gFile, NV_RISCV_DESC_SWBROM_CODE_SIZE, 0), 0u);

    //
    // Asserted against the VERDICT rather than the bool. "Accepted" is one
    // value of an eight-valued family, so `true` is satisfiable by a wrapper
    // that stopped consulting the verdict at all; NV_RISCV_UCODE_OK is not.
    // The bool is kept immediately below because it is what the shell calls,
    // and the pair is what pins the wrapper to its own subject.
    //
    checkVerdict("THE REAL BOOTLOADER IS ACCEPTED",
                 nvRiscvUcodeVerdict(gFile, blLen), NV_RISCV_UCODE_OK);
    checkBool("  and the wrapper agrees on the real file",
              nvRiscvUcodeIsWellFormed(gFile, blLen), true);

    // ---- the two booters: same container, a different structure inside ----

    static const struct {
        const char *name;
        NVU32       fileLen;
        NVU32       dataSize;
    } booters[2] = {
        { "booter_load-570.144.bin",   61304u, 60416u },
        { "booter_unload-570.144.bin", 41080u, 40192u },
    };

    for (int i = 0; i < 2; i++) {
        char label[160];
        NVU32 len = loadFile(booters[i].name, gFile, sizeof(gFile));

        std::snprintf(label, sizeof(label), "%s file length", booters[i].name);
        checkU32(label, len, booters[i].fileLen);

        //
        // The container header is VALID on both booters -- same magic, same
        // hdrOffset, payload in the file. That is the whole trap: every test
        // this suite had before today passes on these blobs.
        //
        std::snprintf(label, sizeof(label), "%s container header is valid",
                      booters[i].name);
        checkBool(label, nvRiscvBinHdrIsValid(gFile, len), true);

        std::snprintf(label, sizeof(label), "%s hdrOffset is 24, same as the"
                      " bootloader", booters[i].name);
        checkU32(label, nvRiscvBinHdrOffset(gFile),
                 NV_RISCV_GA107_BOOTER_HDR_OFFSET);

        std::snprintf(label, sizeof(label), "%s dataOffset", booters[i].name);
        checkU32(label, nvRiscvBinDataOffset(gFile),
                 NV_RISCV_GA107_BOOTER_DATA_OFFSET);

        std::snprintf(label, sizeof(label), "%s dataSize", booters[i].name);
        checkU32(label, nvRiscvBinDataSize(gFile), booters[i].dataSize);

        std::snprintf(label, sizeof(label), "%s middle region is 864 bytes",
                      booters[i].name);
        checkU32(label, nvRiscvDescBytes(gFile),
                 NV_RISCV_GA107_BOOTER_DESC_BYTES);

        //
        // 216 "fields" -- which is the tell. RM_RISCV_UCODE_DESC has 23 in the
        // newest driver this project has read. A 216-field descriptor is not a
        // descriptor.
        //
        std::snprintf(label, sizeof(label), "%s middle region is 216 u32 slots",
                      booters[i].name);
        checkU32(label, nvRiscvDescFieldCount(gFile), 216u);

        //
        // THE SEPARATING MEASUREMENT. 60 is 0x3C, which is nvfw_hs_header_v2's
        // own header_size field. Read as a structure version it is nonsense,
        // and reading it as one is exactly what this file did until 2026-08-18.
        //
        std::snprintf(label, sizeof(label), "%s word 0 is 60 -- an HS header's"
                      " header_size, not a version", booters[i].name);
        checkU32(label, nvRiscvDescVersion(gFile), 60u);

        std::snprintf(label, sizeof(label), "%s version differs from 5",
                      booters[i].name);
        checkBool(label, nvRiscvDescVersion(gFile) == NV_RISCV_DESC_VERSION_5,
                  false);

        //
        // The refusal, driven by the real bytes. Before the version gate this
        // returned false too -- but three optional-region rows deep, because
        // signature bytes at +0x24 happened to decode to 0x1F3B3C30 which does
        // not fit the payload. Nothing constrains a future signature to hold an
        // out-of-range word there.
        //
        //
        // Asserted against the VERDICT, not the bool. `false` is satisfied by
        // ALL SEVEN refusal codes, so the pre-2026-08-18 behaviour -- refusing
        // three optional-region rows deep on a signature word that happened to
        // be out of range -- satisfies it identically to refusing because the
        // descriptor version is not 5. Only the code separates the two, and
        // only one of them is a fact about the FILE rather than a coincidence
        // of its signature bytes. This turns 3050-handoff27 §2's measurement
        // into a standing assertion.
        //
        std::snprintf(label, sizeof(label), "%s IS REFUSED for a VERSION that"
                      " is not 5", booters[i].name);
        checkVerdict(label, nvRiscvUcodeVerdict(gFile, len),
                     NV_RISCV_UCODE_ERR_DESC_VERSION);

        std::snprintf(label, sizeof(label), "  and the wrapper refuses %s",
                      booters[i].name);
        checkBool(label, nvRiscvUcodeIsWellFormed(gFile, len), false);
    }

    //
    // The three files together are the claim: one container format, two
    // structures inside it, and the descriptor length alone does not separate
    // them -- 84 against 864 is a difference the ORIGINAL suite already knew
    // about and still could not use, because it never built the second shape.
    //
    checkBool("bootloader and booter descriptor lengths differ",
              NV_RISCV_GA107_BL_DESC_BYTES == NV_RISCV_GA107_BOOTER_DESC_BYTES,
              false);
}

//
// nvRiscvUcodeVerdict() -- every refusal reaches the caller under its own name.
//
// Until 2026-08-18 this header answered six distinct structural failures with
// one `false`, nvGspStagePlan() mapped that onto ERR_MALFORMED, and
// NV3050Probe::loadGspFirmware() logged the single word. A truncated file, a
// booter handed in by mistake, and a descriptor pointing off the end of its own
// payload were indistinguishable to the one person who could act on them.
//
// Each case below is a SEPARATING input: it differs from the valid blob in one
// field and must land on one code. Asserting only "refused" would pass with
// every arm returning the same thing, which is the state this section exists to
// make impossible -- an assertion that cannot fail is not coverage.
//
static void testUcodeVerdict(void)
{
    section("every structural refusal has its own verdict");

    NVU32 len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    checkVerdict("the real bootloader layout is OK",
                 nvRiscvUcodeVerdict(gBlob, len), NV_RISCV_UCODE_OK);
    checkBool("  and the wrapper agrees",
              nvRiscvUcodeIsWellFormed(gBlob, len), true);

    // Container header. Both roads into it, because a length test and a magic
    // test are different failures that happen to share a code.
    checkVerdict("length 0 -> ERR_HEADER",
                 nvRiscvUcodeVerdict(gBlob, 0u), NV_RISCV_UCODE_ERR_HEADER);

    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    put32(NV_RISCV_BIN_HDR_MAGIC_OFFSET, 0xDEADBEEFu);
    checkVerdict("wrong container magic -> ERR_HEADER",
                 nvRiscvUcodeVerdict(gBlob, len), NV_RISCV_UCODE_ERR_HEADER);

    // Descriptor present but shorter than the fields Phase 4 reads.
    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    put32(NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET,
          NV_RISCV_GA107_BL_HDR_OFFSET + 16u);
    checkVerdict("16-byte descriptor -> ERR_DESC_SHORT",
                 nvRiscvUcodeVerdict(gBlob, len), NV_RISCV_UCODE_ERR_DESC_SHORT);

    //
    // Version. This is the code that names the booter case 3050-handoff27
    // measured -- an nvfw_hs_header_v2 sitting where the descriptor belongs -- so
    // it is the difference between "malformed" and "that is the wrong kind of
    // file".
    //
    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_VERSION_OFFSET, NV_RISCV_DESC_VERSION_5 + 1u);
    checkVerdict("descriptor version 6 -> ERR_DESC_VERSION",
                 nvRiscvUcodeVerdict(gBlob, len),
                 NV_RISCV_UCODE_ERR_DESC_VERSION);

    // Bootloader region: empty and escaping are different facts.
    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_BOOTLOADER_SIZE, 0u);
    checkVerdict("zero-length bootloader -> ERR_BL_EMPTY",
                 nvRiscvUcodeVerdict(gBlob, len), NV_RISCV_UCODE_ERR_BL_EMPTY);

    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_BOOTLOADER_OFFSET, NV_RISCV_GA107_BL_DATA_SIZE);
    checkVerdict("bootloader starts at the payload end -> ERR_BL_OUTSIDE",
                 nvRiscvUcodeVerdict(gBlob, len), NV_RISCV_UCODE_ERR_BL_OUTSIDE);

    //
    // THE ORDER, pinned rather than assumed. A zero-length region trivially
    // satisfies containment, so a verdict that tested containment first would
    // make ERR_BL_EMPTY unreachable -- the same dead-arm shape this whole change
    // exists to remove, reintroduced one level down. This blob is BOTH empty and
    // out of the payload; only the emptiness-first order returns BL_EMPTY.
    //
    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_BOOTLOADER_OFFSET, NV_RISCV_GA107_BL_DATA_SIZE);
    putDesc(NV_RISCV_DESC_BOOTLOADER_SIZE, 0u);
    checkVerdict("empty AND outside -> ERR_BL_EMPTY (emptiness tested first)",
                 nvRiscvUcodeVerdict(gBlob, len), NV_RISCV_UCODE_ERR_BL_EMPTY);

    // Param region, distinct from the bootloader region that precedes it.
    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_BOOTLOADER_PARAM_OFFSET,
            NV_RISCV_GA107_BL_DATA_SIZE - 4u);
    checkVerdict("param region escapes the payload -> ERR_PARAM_OUTSIDE",
                 nvRiscvUcodeVerdict(gBlob, len),
                 NV_RISCV_UCODE_ERR_PARAM_OUTSIDE);

    // One of the six optional rows, which share a code between them.
    len = buildBlob(NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_MANIFEST_OFFSET, NV_RISCV_GA107_BL_DATA_SIZE - 4u);
    putDesc(NV_RISCV_DESC_MANIFEST_SIZE, 64u);
    checkVerdict("optional region escapes the payload -> ERR_REGION_OUTSIDE",
                 nvRiscvUcodeVerdict(gBlob, len),
                 NV_RISCV_UCODE_ERR_REGION_OUTSIDE);
    checkBool("  and the wrapper still says no",
              nvRiscvUcodeIsWellFormed(gBlob, len), false);

    //
    // Name coverage, bounded on the family's own limit rather than on a typed
    // count -- handoff20 section 1, where igModeVerdictName() lost an arm to
    // `default:` and all four gates stayed green. A code added to the enum with
    // no arm here reddens on the commit that adds it.
    //
    // The sentinel is READ OUT OF THE SUBJECT rather than spelled, because a
    // sentinel guessed from a sibling function is a comparison against a string
    // the subject can never return, and every arm then counts as named.
    //
    const char *sentinel =
        nvRiscvUcodeVerdictName((NVRiscvUcodeVerdict)NV_RISCV_UCODE_VERDICT_LIMIT);
    checkBool("the sentinel is non-empty", sentinel[0] != '\0', true);

    for (unsigned v = 0; v < (unsigned)NV_RISCV_UCODE_VERDICT_LIMIT; ++v) {
        char label[96];
        const char *name = nvRiscvUcodeVerdictName((NVRiscvUcodeVerdict)v);

        std::snprintf(label, sizeof(label), "verdict %u has its own name", v);
        checkBool(label, std::strcmp(name, sentinel) != 0 && name[0] != '\0',
                  true);
    }

    //
    // Pairwise distinct. A copy-paste arm returning a sibling's string passes
    // every "has a name" assertion above while telling the operator the wrong
    // thing about their file.
    //
    unsigned collisions = 0;
    for (unsigned a = 0; a < (unsigned)NV_RISCV_UCODE_VERDICT_LIMIT; ++a) {
        for (unsigned b = a + 1; b < (unsigned)NV_RISCV_UCODE_VERDICT_LIMIT; ++b) {
            if (std::strcmp(nvRiscvUcodeVerdictName((NVRiscvUcodeVerdict)a),
                            nvRiscvUcodeVerdictName((NVRiscvUcodeVerdict)b)) == 0)
                collisions++;
        }
    }
    checkU32("no two verdicts share a name", collisions, 0u);

    //
    // The wrapper is an EQUALITY against OK. Pinned across the whole family
    // rather than at one point: a `!= ERR_HEADER` form would call a
    // version-mismatched booter well-formed.
    //
    checkU32("the family has eight codes plus the limit",
             (NVU32)NV_RISCV_UCODE_VERDICT_LIMIT, 8u);
}

int main(void)
{
    std::printf("NVRiscvUcode.hpp -- RM_RISCV_UCODE_DESC parsing tests\n");
    std::printf("=====================================================\n");

    testObservedGeometry();
    testAbsentFields();
    testFieldOffsets();
    testRegionAddressing();
    testMalformed();
    testBooterLayout();
    testDescriptorVersion();
    testUcodeVerdict();
    testRead32();
    testRealFirmwareFiles();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
