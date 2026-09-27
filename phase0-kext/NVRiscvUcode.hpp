//
//  NVRiscvUcode.hpp
//  RM_RISCV_UCODE_DESC parsing for the GA107 GSP firmware blobs.
//
//  Phase 4. Like NVDecode.hpp and NVFalcon.hpp this is DELIBERATELY
//  DEPENDENCY-FREE: no includes beyond NVDecode.hpp, every type a plain
//  builtin, so tests/ compiles the identical file under MSVC and runs it.
//  Nothing here touches hardware -- it is byte-offset arithmetic over a buffer
//  the caller already has in memory.
//
//  Transcribed from NVIDIA/open-gpu-kernel-modules v610.57.04 (SPDX: MIT):
//    src/nvidia/arch/nvalloc/common/inc/rmRiscvUcode.h
//
//  The observed values below were parsed from the real blob pulled off the
//  target laptop, reference/ga107-firmware/bootloader-570.144.bin
//  (nvidia-595-open 595.84, GA107). They are recorded so the parser can be
//  tested against firmware that actually exists rather than against values
//  this project invented.
//
//  Verified by tests/test_riscv_ucode.cpp.
//

#ifndef NVRiscvUcode_hpp
#define NVRiscvUcode_hpp

#include "NVDecode.hpp"

//
// ===========================================================================
// THE DESCRIPTOR IS VARIABLE-LENGTH, AND ASSUMING OTHERWISE IS SILENT
// ===========================================================================
//
// A firmware blob is three concatenated regions:
//
//   [ nvfw_bin_hdr ][ RM_RISCV_UCODE_DESC ][ payload ]
//   0              hdrOffset              dataOffset
//
// RM_RISCV_UCODE_DESC has grown across driver releases -- fields were appended
// at the end, never reordered. NVIDIA's own extractor (nouveau/
// extract-firmware-nouveau.py) says so outright: "the size of
// RM_RISCV_UCODE_DESC varies from version to version".
//
// So the descriptor's real length is NOT sizeof(the struct you compiled
// against). It is (dataOffset - hdrOffset), computed from the header at
// runtime. On the GA107 bootloader that is 84 bytes = 21 fields, while the
// v610 struct declares 23. The last two fields (bIsSmp, bIsPlicEnabled) are
// NOT PRESENT in this blob.
//
// If you memcpy a 92-byte struct out of an 84-byte descriptor you read 8 bytes
// of payload as though they were flags. Those bytes are signed firmware, so
// they are effectively random -- and they land on bIsSmp / bIsPlicEnabled,
// which change how the RISC-V core is brought up. Nothing faults. The boot
// just hangs. Hence nvRiscvDescFieldCount() and the nvRiscvDescHas*()
// predicates: there is no way to read a field this blob lacks and have it
// return a plausible number by accident.
// ===========================================================================
//

// ---------------------------------------------------------------------------
// nvfw_bin_hdr -- the outer container. Fixed 24 bytes, six u32.
// ---------------------------------------------------------------------------

#define NV_RISCV_BIN_HDR_SIZE                    24

#define NV_RISCV_BIN_HDR_MAGIC_OFFSET            0x00
#define NV_RISCV_BIN_HDR_VERSION_OFFSET          0x04
#define NV_RISCV_BIN_HDR_BIN_SIZE_OFFSET         0x08
#define NV_RISCV_BIN_HDR_HDR_OFFSET_OFFSET       0x0C
#define NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET      0x10
#define NV_RISCV_BIN_HDR_DATA_SIZE_OFFSET        0x14

//
// 0x10DE is NVIDIA's PCI vendor ID reused as a file magic. Anything else means
// the caller handed us a file that is not an nvfw container at all.
//
#define NV_RISCV_BIN_HDR_MAGIC                   0x000010DE
#define NV_RISCV_BIN_HDR_VERSION_1               0x00000001

// ---------------------------------------------------------------------------
// RM_RISCV_UCODE_DESC -- rmRiscvUcode.h:36. All u32, appended-only.
//
// Offsets are relative to the START OF THE DESCRIPTOR, i.e. to hdrOffset in
// the file, NOT to the file start. Add nvRiscvBinHdrOffset() to get a file
// offset.
// ---------------------------------------------------------------------------

#define NV_RISCV_DESC_VERSION_OFFSET             0x00
#define NV_RISCV_DESC_BOOTLOADER_OFFSET          0x04
#define NV_RISCV_DESC_BOOTLOADER_SIZE            0x08
#define NV_RISCV_DESC_BOOTLOADER_PARAM_OFFSET    0x0C
#define NV_RISCV_DESC_BOOTLOADER_PARAM_SIZE      0x10
#define NV_RISCV_DESC_RISCV_ELF_OFFSET           0x14
#define NV_RISCV_DESC_RISCV_ELF_SIZE             0x18
#define NV_RISCV_DESC_APP_VERSION                0x1C
#define NV_RISCV_DESC_MANIFEST_OFFSET            0x20
#define NV_RISCV_DESC_MANIFEST_SIZE              0x24
#define NV_RISCV_DESC_MONITOR_DATA_OFFSET        0x28
#define NV_RISCV_DESC_MONITOR_DATA_SIZE          0x2C
#define NV_RISCV_DESC_MONITOR_CODE_OFFSET        0x30
#define NV_RISCV_DESC_MONITOR_CODE_SIZE          0x34
#define NV_RISCV_DESC_IS_MONITOR_ENABLED         0x38
#define NV_RISCV_DESC_SWBROM_CODE_OFFSET         0x3C
#define NV_RISCV_DESC_SWBROM_CODE_SIZE           0x40
#define NV_RISCV_DESC_SWBROM_DATA_OFFSET         0x44
#define NV_RISCV_DESC_SWBROM_DATA_SIZE           0x48
#define NV_RISCV_DESC_FB_RESERVED_SIZE           0x4C
#define NV_RISCV_DESC_SIGNED_AS_CODE             0x50
// --- everything below is ABSENT from the GA107 570.144 blobs (84-byte desc) --
#define NV_RISCV_DESC_IS_SMP                     0x54
#define NV_RISCV_DESC_IS_PLIC_ENABLED            0x58

//
// Field counts, not byte sizes -- the descriptor is measured in u32 slots and
// comparing counts is what the predicates below do.
//
#define NV_RISCV_DESC_FIELDS_THROUGH_SIGNED_AS_CODE  21
#define NV_RISCV_DESC_FIELDS_THROUGH_PLIC            23

//
// rmRiscvUcode.h:38-42 -- structure version, not driver version.
//   5 = entire RISC-V image signed as "code" in the code section.
//
#define NV_RISCV_DESC_VERSION_5                  0x00000005

// ---------------------------------------------------------------------------
// Observed values -- reference/ga107-firmware/bootloader-570.144.bin,
// parsed 2026-08-07. File length 24684 bytes.
// ---------------------------------------------------------------------------

#define NV_RISCV_GA107_BL_FILE_SIZE              24684
#define NV_RISCV_GA107_BL_HDR_OFFSET             24
#define NV_RISCV_GA107_BL_DATA_OFFSET            108
#define NV_RISCV_GA107_BL_DATA_SIZE              24576
#define NV_RISCV_GA107_BL_DESC_BYTES             84

#define NV_RISCV_GA107_BL_BOOTLOADER_OFFSET      0x00005000
#define NV_RISCV_GA107_BL_BOOTLOADER_SIZE        0x00000880
#define NV_RISCV_GA107_BL_BOOTLOADER_PARAM_OFF   0x00005880
#define NV_RISCV_GA107_BL_BOOTLOADER_PARAM_SIZE  0x00000010

//
// booter_load / booter_unload share the container layout but carry a much
// larger descriptor -- 864 bytes against the bootloader's 84. That is the
// variable-length problem in one line of evidence: three files, same driver
// release, same struct name, two different descriptor sizes.
//
#define NV_RISCV_GA107_BOOTER_HDR_OFFSET         24
#define NV_RISCV_GA107_BOOTER_DATA_OFFSET        888
#define NV_RISCV_GA107_BOOTER_DESC_BYTES         864

// ---------------------------------------------------------------------------
// Accessors.
//
// Every read is bounds-checked against a caller-supplied length. A GSP
// firmware blob arrives from a file the kext does not control; a truncated or
// hostile one must produce a refusal, not an out-of-bounds read in kernel
// space.
// ---------------------------------------------------------------------------

//
// Little-endian u32 load. Spelled byte-by-byte rather than as a cast because
// the descriptor is not guaranteed aligned relative to the caller's buffer,
// and an unaligned NVU32* deref is UB even on x86.
//
static inline NVU32 nvRiscvRead32(const unsigned char *buf, NVU32 off)
{
    return (NVU32)buf[off]
         | ((NVU32)buf[off + 1] << 8)
         | ((NVU32)buf[off + 2] << 16)
         | ((NVU32)buf[off + 3] << 24);
}

static inline bool nvRiscvFits(NVU32 len, NVU32 off, NVU32 size)
{
    // Overflow-safe: never computes off + size.
    return off <= len && size <= (len - off);
}

static inline NVU32 nvRiscvBinHdrMagic(const unsigned char *buf)
{
    return nvRiscvRead32(buf, NV_RISCV_BIN_HDR_MAGIC_OFFSET);
}

static inline NVU32 nvRiscvBinHdrOffset(const unsigned char *buf)
{
    return nvRiscvRead32(buf, NV_RISCV_BIN_HDR_HDR_OFFSET_OFFSET);
}

static inline NVU32 nvRiscvBinDataOffset(const unsigned char *buf)
{
    return nvRiscvRead32(buf, NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET);
}

static inline NVU32 nvRiscvBinDataSize(const unsigned char *buf)
{
    return nvRiscvRead32(buf, NV_RISCV_BIN_HDR_DATA_SIZE_OFFSET);
}

//
// True only if the container header itself is readable and internally
// consistent. Call this before ANYTHING else -- every accessor below trusts
// hdrOffset and dataOffset, and those come out of the file.
//
static inline bool nvRiscvBinHdrIsValid(const unsigned char *buf, NVU32 len)
{
    if (len < NV_RISCV_BIN_HDR_SIZE) {
        return false;
    }
    if (nvRiscvBinHdrMagic(buf) != NV_RISCV_BIN_HDR_MAGIC) {
        return false;
    }

    NVU32 hdrOff  = nvRiscvBinHdrOffset(buf);
    NVU32 dataOff = nvRiscvBinDataOffset(buf);
    NVU32 dataSz  = nvRiscvBinDataSize(buf);

    // The descriptor sits between the two offsets, so the order matters and a
    // zero-length descriptor is not a thing.
    if (hdrOff < NV_RISCV_BIN_HDR_SIZE || dataOff <= hdrOff) {
        return false;
    }
    // Descriptor must be a whole number of u32 slots.
    if (((dataOff - hdrOff) & 3u) != 0) {
        return false;
    }
    // Payload must actually be in the file.
    return nvRiscvFits(len, dataOff, dataSz);
}

//
// Descriptor length in bytes / in u32 fields, computed from the header rather
// than from any compiled-in sizeof.
//
static inline NVU32 nvRiscvDescBytes(const unsigned char *buf)
{
    return nvRiscvBinDataOffset(buf) - nvRiscvBinHdrOffset(buf);
}

static inline NVU32 nvRiscvDescFieldCount(const unsigned char *buf)
{
    return nvRiscvDescBytes(buf) / 4;
}

//
// Is the field at descriptor-relative byte offset `fieldOff` actually present
// in THIS blob? This is the guard that stops a 23-field read against a
// 21-field descriptor.
//
static inline bool nvRiscvDescHasField(const unsigned char *buf, NVU32 fieldOff)
{
    return (fieldOff + 4) <= nvRiscvDescBytes(buf);
}

static inline bool nvRiscvDescHasSmpFlags(const unsigned char *buf)
{
    return nvRiscvDescHasField(buf, NV_RISCV_DESC_IS_PLIC_ENABLED);
}

//
// Read one descriptor field. Returns `fallback` when the field is absent from
// this blob -- callers must pass the value that is CORRECT when the firmware
// predates the field, not merely zero out of habit. For bIsSmp / bIsPlicEnabled
// the pre-field behaviour is single-hart, no PLIC, so false is right there.
//
static inline NVU32 nvRiscvDescField(const unsigned char *buf,
                                     NVU32 fieldOff,
                                     NVU32 fallback)
{
    if (!nvRiscvDescHasField(buf, fieldOff)) {
        return fallback;
    }
    return nvRiscvRead32(buf, nvRiscvBinHdrOffset(buf) + fieldOff);
}

//
// Structure version of THIS descriptor. Field 0, so it is present in every
// descriptor long enough to exist at all -- but read it through the same
// presence-checked accessor as everything else rather than reaching for offset
// 0 directly, because a zero-length descriptor is a shape the container header
// can legally describe and offset 0 would then read the payload's first word.
//
// The fallback is deliberately NOT 5. A blob whose descriptor is too short to
// hold its own version is not "probably the version we know"; it is a blob
// whose type we failed to establish, and the value that must flow onward is one
// that no consumer accepts.
//
#define NV_RISCV_DESC_VERSION_UNKNOWN            0xFFFFFFFFu

static inline NVU32 nvRiscvDescVersion(const unsigned char *buf)
{
    return nvRiscvDescField(buf, NV_RISCV_DESC_VERSION_OFFSET,
                            NV_RISCV_DESC_VERSION_UNKNOWN);
}

//
// A descriptor region is an (offset, size) pair naming a slice of the PAYLOAD,
// so its offsets are relative to dataOffset -- not to the descriptor, and not
// to the file. Getting that wrong reads the descriptor bytes as firmware.
//
static inline bool nvRiscvRegionInPayload(const unsigned char *buf,
                                          NVU32 regionOff,
                                          NVU32 regionSize)
{
    NVU32 dataSz = nvRiscvBinDataSize(buf);
    return nvRiscvFits(dataSz, regionOff, regionSize);
}

//
// File offset of a payload-relative region, for a caller that wants to point
// a DMA descriptor at it. Only meaningful once nvRiscvRegionInPayload() has
// said yes.
//
static inline NVU32 nvRiscvRegionFileOffset(const unsigned char *buf,
                                            NVU32 regionOff)
{
    return nvRiscvBinDataOffset(buf) + regionOff;
}

//
// Full structural validation: header sane, descriptor a known-plausible
// length, and every region the descriptor names contained in the payload.
//
// This does NOT verify signatures. The blobs are NVIDIA-signed and this
// project cannot check that; a blob passing here is well-formed, not trusted.
//
// WHY A VERDICT RATHER THAN A BOOL
//
// Until 2026-08-18 this was a bare `bool` with six distinct ways to say no and
// one word to say them in. nvGspStagePlan() mapped every one of them onto
// NVGSP_STAGE_ERR_MALFORMED and NV3050Probe::loadGspFirmware() put that single
// word into a log line, so an operator holding a refused blob could not tell a
// truncated file from a version mismatch from a region running off the end of
// the payload. Those call for three different actions: re-extract the file,
// use a different firmware, and file a bug against this parser.
//
// The distinction is not hypothetical. 3050-handoff27 measured that both
// booters in reference/ga107-firmware/ carry an nvfw_hs_header_v2 where the
// bootloader carries an RM_RISCV_UCODE_DESC, so handing this function a booter
// now refuses at NV_RISCV_UCODE_ERR_DESC_VERSION -- a DIAGNOSIS ("that is the
// wrong kind of file") rather than the mystery "malformed" was.
//
// Spelled as an enum rather than #defines on purpose. tools/check-registers.py
// parses this header's #defines against NVIDIA's published headers, and these
// codes are this driver's own bookkeeping rather than hardware -- the same
// reasoning as NVGSP_READ_* in NVGspBoot.hpp, and why the register gate is
// correctly unmoved by this change.
//
typedef enum {
    NV_RISCV_UCODE_OK = 0,

    NV_RISCV_UCODE_ERR_HEADER,         // container header short, bad magic, or
                                       // internally inconsistent
    NV_RISCV_UCODE_ERR_DESC_SHORT,     // descriptor lacks the fields Phase 4 needs
    NV_RISCV_UCODE_ERR_DESC_VERSION,   // descriptor version is not 5
    NV_RISCV_UCODE_ERR_BL_EMPTY,       // bootloader region is zero-length
    NV_RISCV_UCODE_ERR_BL_OUTSIDE,     // bootloader region escapes the payload
    NV_RISCV_UCODE_ERR_PARAM_OUTSIDE,  // bootloader-param region escapes it
    NV_RISCV_UCODE_ERR_REGION_OUTSIDE, // an optional region escapes it

    NV_RISCV_UCODE_VERDICT_LIMIT
} NVRiscvUcodeVerdict;

//
// Every verdict below the family's own limit must have a name. The loop in
// tests/test_riscv_ucode.cpp bounds on NV_RISCV_UCODE_VERDICT_LIMIT rather than
// on a typed count, so a code added to the enum with no arm here reddens on the
// commit that adds it -- handoff20 section 1's igModeVerdictName() lesson,
// where a `default:` arm swallowed a verdict and all four gates stayed green.
//
static inline const char *nvRiscvUcodeVerdictName(NVRiscvUcodeVerdict v)
{
    switch (v) {
    case NV_RISCV_UCODE_OK:                 return "well-formed";
    case NV_RISCV_UCODE_ERR_HEADER:         return "container header invalid";
    case NV_RISCV_UCODE_ERR_DESC_SHORT:     return "descriptor too short";
    case NV_RISCV_UCODE_ERR_DESC_VERSION:   return "descriptor version not 5";
    case NV_RISCV_UCODE_ERR_BL_EMPTY:       return "bootloader region empty";
    case NV_RISCV_UCODE_ERR_BL_OUTSIDE:     return "bootloader region outside payload";
    case NV_RISCV_UCODE_ERR_PARAM_OUTSIDE:  return "bootloader-param region outside payload";
    case NV_RISCV_UCODE_ERR_REGION_OUTSIDE: return "optional region outside payload";
    default:                                return "unrecognised ucode verdict";
    }
}

static inline NVRiscvUcodeVerdict nvRiscvUcodeVerdict(const unsigned char *buf,
                                                      NVU32 len)
{
    if (!nvRiscvBinHdrIsValid(buf, len)) {
        return NV_RISCV_UCODE_ERR_HEADER;
    }

    // Everything through bSignedAsCode must exist -- a descriptor shorter than
    // that predates the fields Phase 4 relies on and is not usable here.
    if (nvRiscvDescFieldCount(buf) < NV_RISCV_DESC_FIELDS_THROUGH_SIGNED_AS_CODE) {
        return NV_RISCV_UCODE_ERR_DESC_SHORT;
    }

    //
    // THE BLOB'S TYPE, ESTABLISHED RATHER THAN ASSUMED.
    //
    // Measured 2026-08-18 against all three files in reference/ga107-firmware/:
    // only bootloader-570.144.bin carries an RM_RISCV_UCODE_DESC here. Both
    // booters carry an nvfw_hs_header_v2 instead -- same container header, same
    // hdrOffset of 24, a region between hdrOffset and dataOffset that this
    // function was reading as a descriptor. Its first word is 60 (0x3C), which
    // is that header's own `header_size` field and not a structure version at
    // all; the plausible-looking prefix ends at exactly byte 36, after which the
    // words are signature material.
    //
    // Without this test a booter was still REJECTED -- but by accident, three
    // optional-region rows deep, because the signature bytes at +0x24 happened
    // to decode to a size of 0x1F3B3C30 which does not fit the payload. That is
    // a rejection this project cannot rely on: nothing constrains a future
    // signature to hold an out-of-range word at that offset, and the failure it
    // guards is not a parse error but a DMA of an HS signature block into IMEM
    // as though it were bootloader code.
    //
    // An EQUALITY, not a floor. A version 6 descriptor would append fields
    // exactly as 5 appended them and would very likely parse -- but "very
    // likely" is the wrong standard for a buffer that becomes instructions on a
    // core with no MMU. Whoever meets a version 6 blob extends this test
    // deliberately, which is the point of it being here.
    //
    if (nvRiscvDescVersion(buf) != NV_RISCV_DESC_VERSION_5) {
        return NV_RISCV_UCODE_ERR_DESC_VERSION;
    }

    NVU32 blOff  = nvRiscvDescField(buf, NV_RISCV_DESC_BOOTLOADER_OFFSET, 0);
    NVU32 blSize = nvRiscvDescField(buf, NV_RISCV_DESC_BOOTLOADER_SIZE, 0);

    //
    // SPLIT, where the bool version had one `||`. The two halves are different
    // facts about the file: a zero-length bootloader is a blob that names no
    // code to load, while a region escaping the payload is a blob whose
    // descriptor disagrees with its own dataSize. Collapsing them was exactly
    // the loss this verdict exists to undo, and the order matters -- an empty
    // region trivially "fits", so the emptiness test must come first or
    // NV_RISCV_UCODE_ERR_BL_EMPTY would be unreachable.
    //
    if (blSize == 0) {
        return NV_RISCV_UCODE_ERR_BL_EMPTY;
    }

    if (!nvRiscvRegionInPayload(buf, blOff, blSize)) {
        return NV_RISCV_UCODE_ERR_BL_OUTSIDE;
    }

    NVU32 pOff  = nvRiscvDescField(buf, NV_RISCV_DESC_BOOTLOADER_PARAM_OFFSET, 0);
    NVU32 pSize = nvRiscvDescField(buf, NV_RISCV_DESC_BOOTLOADER_PARAM_SIZE, 0);
    if (!nvRiscvRegionInPayload(buf, pOff, pSize)) {
        return NV_RISCV_UCODE_ERR_PARAM_OUTSIDE;
    }

    //
    // Manifest / monitor / swbrom regions are legitimately zero on this blob.
    // A zero-size region is "absent", not "invalid" -- only a non-zero region
    // that runs off the end of the payload is a failure.
    //
    NVU32 pairs[6][2] = {
        { NV_RISCV_DESC_RISCV_ELF_OFFSET,    NV_RISCV_DESC_RISCV_ELF_SIZE },
        { NV_RISCV_DESC_MANIFEST_OFFSET,     NV_RISCV_DESC_MANIFEST_SIZE },
        { NV_RISCV_DESC_MONITOR_DATA_OFFSET, NV_RISCV_DESC_MONITOR_DATA_SIZE },
        { NV_RISCV_DESC_MONITOR_CODE_OFFSET, NV_RISCV_DESC_MONITOR_CODE_SIZE },
        { NV_RISCV_DESC_SWBROM_CODE_OFFSET,  NV_RISCV_DESC_SWBROM_CODE_SIZE },
        { NV_RISCV_DESC_SWBROM_DATA_OFFSET,  NV_RISCV_DESC_SWBROM_DATA_SIZE },
    };

    for (int i = 0; i < 6; i++) {
        NVU32 off  = nvRiscvDescField(buf, pairs[i][0], 0);
        NVU32 size = nvRiscvDescField(buf, pairs[i][1], 0);
        if (size != 0 && !nvRiscvRegionInPayload(buf, off, size)) {
            return NV_RISCV_UCODE_ERR_REGION_OUTSIDE;
        }
    }

    return NV_RISCV_UCODE_OK;
}

//
// The original predicate, kept as a one-line wrapper.
//
// Kept rather than replaced because "is this blob usable at all" is a real
// question with a real answer, and every existing caller and test asks exactly
// that. Deleting it would have meant editing nvGspStagePlan() and every
// assertion in tests/test_riscv_ucode.cpp in the same commit that introduced
// the verdict, which is how a widening turns into a rewrite nobody can review.
//
// The relation is an EQUALITY against NV_RISCV_UCODE_OK, never a comparison
// against any error code. A `!= NV_RISCV_UCODE_ERR_HEADER` form would call a
// version-mismatched booter well-formed, which is the exact blob 3050-handoff27
// found being read as a descriptor.
//
static inline bool nvRiscvUcodeIsWellFormed(const unsigned char *buf, NVU32 len)
{
    return nvRiscvUcodeVerdict(buf, len) == NV_RISCV_UCODE_OK;
}

#endif /* NVRiscvUcode_hpp */
