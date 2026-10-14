// SPDX-License-Identifier: MIT
// Pure composition of the full GSP-RM ELF container into Radix3 staging geometry.
#ifndef NV_GSP_RM_IMAGE_HPP
#define NV_GSP_RM_IMAGE_HPP

#include "NVGspFwContainer.hpp"
#include "NVGspRadix3.hpp"

using NVGspRmU32 = unsigned int;
using NVGspRmU64 = unsigned long long;

// Keep the full GSP-RM container namespace deliberately separate from the
// existing small RISC-V bootloader property consumed by NVGspStage.hpp.
#define NV_GSP_BOOTLOADER_PROPERTY_NAME        "NVGspFirmware"
#define NV_GSP_RM_FIRMWARE_PROPERTY_NAME       "NVGspRmFirmware"
#define NV_GSP_RM_STAGE_BOOT_ARG               "nvgsp-rm-image-stage"

// open RM kgspCreateRadix3() allocates the firmware Radix3 descriptor as
// 4 KiB-aligned, noncontiguous, cached system memory.
#define NV_GSP_RM_STAGE_PAGE_BYTES             4096u
#define NV_GSP_RM_STAGE_NONCONTIGUOUS          1u
#define NV_GSP_RM_STAGE_CACHED                 1u

#define NV_GSP_RM_IMAGE_NOT_ATTEMPTED          0u
#define NV_GSP_RM_IMAGE_OK                     1u
#define NV_GSP_RM_IMAGE_NULL_BLOB              2u
#define NV_GSP_RM_IMAGE_NULL_OUTPUT            3u
#define NV_GSP_RM_IMAGE_CONTAINER_REFUSED      4u
#define NV_GSP_RM_IMAGE_RADIX3_REFUSED         5u
#define NV_GSP_RM_IMAGE_LIMIT                  6u

struct NVGspRmImagePlan {
    NVGspRmU64 containerBytes;
    NVGspFwImageSection imageSection;
    NVGspRadix3Plan radix3;
    NVGspRmU64 imageEndOffset;
    NVGspRmU64 stageAllocationBytes;
    NVGspRmU64 stageAllocationPages;
    NVGspRmU32 containerVerdict;
    NVGspRmU32 radix3Verdict;
    NVGspRmU32 stagePageBytes;
    bool stageNoncontiguous;
    bool stageCached;
};

static inline bool nvGspRmImageSucceeded(NVGspRmU32 verdict)
{
    return verdict == NV_GSP_RM_IMAGE_OK;
}

static inline const char *nvGspRmImageVerdictName(NVGspRmU32 verdict)
{
    switch (verdict) {
        case NV_GSP_RM_IMAGE_NOT_ATTEMPTED:     return "not-attempted";
        case NV_GSP_RM_IMAGE_OK:                return "ok";
        case NV_GSP_RM_IMAGE_NULL_BLOB:         return "null-blob";
        case NV_GSP_RM_IMAGE_NULL_OUTPUT:       return "null-output";
        case NV_GSP_RM_IMAGE_CONTAINER_REFUSED: return "container-refused";
        case NV_GSP_RM_IMAGE_RADIX3_REFUSED:    return "radix3-refused";
        default:                                return "unknown";
    }
}

// Compose the same two source-backed steps open RM performs before creating the
// pGspUCodeRadix3Descriptor:
//   1. select `.fwimage` from the full GSP firmware ELF container;
//   2. size the embedded-data Radix3 descriptor for exactly that section.
//
// No blob bytes are copied and no memory is allocated here. The caller can use
// imageSection.fileOffset/fileBytes as the only legal source range for a future
// runtime-owned staging allocation. The existing `NVGspFirmware` bootloader
// property is intentionally outside this API.
static inline NVGspRmU32 nvGspPlanRmFirmwareImage(const unsigned char *blob,
                                                  NVGspRmU64 blobBytes,
                                                  NVGspRmImagePlan *out)
{
    if (!blob) return NV_GSP_RM_IMAGE_NULL_BLOB;
    if (!out) return NV_GSP_RM_IMAGE_NULL_OUTPUT;

    NVGspRmImagePlan staged = {};
    staged.containerBytes = blobBytes;
    staged.stagePageBytes = NV_GSP_RM_STAGE_PAGE_BYTES;
    staged.stageNoncontiguous = NV_GSP_RM_STAGE_NONCONTIGUOUS != 0u;
    staged.stageCached = NV_GSP_RM_STAGE_CACHED != 0u;

    staged.containerVerdict = nvGspFindFwImageSection(
        blob, blobBytes, &staged.imageSection);
    if (staged.containerVerdict != NV_GSP_FW_CONTAINER_OK)
        return NV_GSP_RM_IMAGE_CONTAINER_REFUSED;

    // nvGspFindFwImageSection already proves this addition is within blobBytes,
    // so it cannot overflow. Keep the derived end in the final plan so runtime
    // code never needs to re-derive the source span.
    staged.imageEndOffset = staged.imageSection.fileOffset + staged.imageSection.fileBytes;

    staged.radix3Verdict = nvGspPlanEmbeddedRadix3(
        staged.imageSection.fileBytes, &staged.radix3);
    if (staged.radix3Verdict != NV_GSP_RADIX3_OK)
        return NV_GSP_RM_IMAGE_RADIX3_REFUSED;

    staged.stageAllocationBytes = staged.radix3.allocationBytes;
    staged.stageAllocationPages = staged.radix3.allocationPages;
    *out = staged;
    return NV_GSP_RM_IMAGE_OK;
}

#endif // NV_GSP_RM_IMAGE_HPP
