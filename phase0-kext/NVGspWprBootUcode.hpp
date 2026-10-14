// SPDX-License-Identifier: MIT
// Pure plan for the GA107 GSP-RM boot-ucode image consumed by WPR metadata.
#ifndef NV_GSP_WPR_BOOT_UCODE_HPP
#define NV_GSP_WPR_BOOT_UCODE_HPP

#include "NVRiscvUcode.hpp"

#define NV_GSP_WPR_BOOT_NOT_ATTEMPTED  0u
#define NV_GSP_WPR_BOOT_OK             1u
#define NV_GSP_WPR_BOOT_NULL_OUTPUT    2u
#define NV_GSP_WPR_BOOT_BAD_UCODE      3u
#define NV_GSP_WPR_BOOT_BAD_IMAGE      4u
#define NV_GSP_WPR_BOOT_BAD_OFFSETS    5u
#define NV_GSP_WPR_BOOT_SIZE_OVERFLOW  6u
#define NV_GSP_WPR_BOOT_LIMIT          7u

#define NV_GSP_WPR_BOOT_PAGE_BYTES     4096u

struct NVGspWprBootUcodePlan {
    // File span copied into the dedicated GSP-RM boot-ucode sysmem allocation.
    NVU32 sourceOffset;
    NVU32 sourceBytes;

    // Open RM allocates the memdesc page-rounded but records the unrounded
    // source size in GspFwWprMetaV1::sizeOfBootloader.
    NVU32 allocationBytes;

    // Offsets are relative to the boot-ucode image base, exactly as copied by
    // kernel_gsp_tu102.c into GspFwWprMetaV1.
    NVU32 monitorCodeOffset;
    NVU32 monitorDataOffset;
    NVU32 manifestOffset;

    NVRiscvUcodeVerdict ucodeVerdict;
    bool valid;
};

static inline bool nvGspWprBootSucceeded(NVU32 verdict)
{
    return verdict == NV_GSP_WPR_BOOT_OK;
}

static inline const char *nvGspWprBootVerdictName(NVU32 verdict)
{
    switch (verdict) {
        case NV_GSP_WPR_BOOT_NOT_ATTEMPTED: return "not-attempted";
        case NV_GSP_WPR_BOOT_OK:            return "ok";
        case NV_GSP_WPR_BOOT_NULL_OUTPUT:   return "null-output";
        case NV_GSP_WPR_BOOT_BAD_UCODE:     return "bad-ucode";
        case NV_GSP_WPR_BOOT_BAD_IMAGE:     return "bad-image";
        case NV_GSP_WPR_BOOT_BAD_OFFSETS:   return "bad-offsets";
        case NV_GSP_WPR_BOOT_SIZE_OVERFLOW: return "size-overflow";
        default:                            return "unknown";
    }
}

static inline bool nvGspWprBootRoundPage(NVU32 bytes, NVU32 *out)
{
    if (!out || bytes == 0u)
        return false;

    const NVU32 mask = NV_GSP_WPR_BOOT_PAGE_BYTES - 1u;
    if (bytes > 0xffffffffu - mask)
        return false;

    *out = (bytes + mask) & ~mask;
    return true;
}

// Build the source-backed plan from the combined nvfw bootloader container.
//
// The current Phase-5 IMEM loader intentionally stages only the descriptor's
// small `bootloaderOffset/bootloaderSize` slice. WPR metadata does NOT point at
// that slice. Open RM keeps a separate GSP-RM boot-ucode image allocation and
// copies monitorCodeOffset/monitorDataOffset/manifestOffset relative to that
// image's base. For the combined nvfw container used by this project, that
// image is the complete payload at [dataOffset, dataOffset + dataSize).
//
// The function validates everything into a local value and only commits to
// caller memory on success. Every refusal therefore leaves *out untouched.
static inline NVU32 nvGspPlanWprBootUcode(const unsigned char *blob,
                                          NVU32 blobLen,
                                          NVGspWprBootUcodePlan *out)
{
    if (!out)
        return NV_GSP_WPR_BOOT_NULL_OUTPUT;

    NVGspWprBootUcodePlan staged = {};
    staged.ucodeVerdict = NV_RISCV_UCODE_VERDICT_LIMIT;
    staged.valid = false;

    NVRiscvUcodeVerdict uv = nvRiscvUcodeVerdict(blob, blobLen);
    staged.ucodeVerdict = uv;
    if (uv != NV_RISCV_UCODE_OK)
        return NV_GSP_WPR_BOOT_BAD_UCODE;

    const NVU32 sourceOffset = nvRiscvBinDataOffset(blob);
    const NVU32 sourceBytes = nvRiscvBinDataSize(blob);
    if (sourceBytes == 0u || !nvRiscvFits(blobLen, sourceOffset, sourceBytes))
        return NV_GSP_WPR_BOOT_BAD_IMAGE;

    NVU32 allocationBytes = 0u;
    if (!nvGspWprBootRoundPage(sourceBytes, &allocationBytes))
        return NV_GSP_WPR_BOOT_SIZE_OVERFLOW;

    const NVU32 codeOff = nvRiscvDescField(blob,
                                            NV_RISCV_DESC_MONITOR_CODE_OFFSET,
                                            0xffffffffu);
    const NVU32 dataOff = nvRiscvDescField(blob,
                                            NV_RISCV_DESC_MONITOR_DATA_OFFSET,
                                            0xffffffffu);
    const NVU32 manifestOff = nvRiscvDescField(blob,
                                                NV_RISCV_DESC_MANIFEST_OFFSET,
                                                0xffffffffu);

    // WPR's V1 builder requires each offset to name a byte inside the staged
    // image. This is intentionally stricter than nvRiscvUcodeVerdict(), which
    // permits an arbitrary offset when an optional region's size is zero.
    if (codeOff >= sourceBytes || dataOff >= sourceBytes || manifestOff >= sourceBytes)
        return NV_GSP_WPR_BOOT_BAD_OFFSETS;

    staged.sourceOffset = sourceOffset;
    staged.sourceBytes = sourceBytes;
    staged.allocationBytes = allocationBytes;
    staged.monitorCodeOffset = codeOff;
    staged.monitorDataOffset = dataOff;
    staged.manifestOffset = manifestOff;
    staged.ucodeVerdict = uv;
    staged.valid = true;

    *out = staged;
    return NV_GSP_WPR_BOOT_OK;
}

#endif // NV_GSP_WPR_BOOT_UCODE_HPP
