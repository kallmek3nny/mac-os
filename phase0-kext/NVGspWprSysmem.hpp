// SPDX-License-Identifier: MIT
// Pure bridge from the two allocation-only GA107 staging services to the
// system-memory fields consumed by GspFwWprMetaV1.
#ifndef NV_GSP_WPR_SYSMEM_HPP
#define NV_GSP_WPR_SYSMEM_HPP

typedef unsigned long long NVGspWprSysU64;
typedef unsigned int       NVGspWprSysU32;

#define NV_GSP_WPR_SYSMEM_NOT_ATTEMPTED  0u
#define NV_GSP_WPR_SYSMEM_OK             1u
#define NV_GSP_WPR_SYSMEM_NULL_OUTPUT    2u
#define NV_GSP_WPR_SYSMEM_STAGE_MISSING  3u
#define NV_GSP_WPR_SYSMEM_BAD_RADIX3     4u
#define NV_GSP_WPR_SYSMEM_BAD_BOOT       5u
#define NV_GSP_WPR_SYSMEM_BAD_ALIGNMENT  6u
#define NV_GSP_WPR_SYSMEM_BAD_OFFSETS    7u
#define NV_GSP_WPR_SYSMEM_RANGE_OVERFLOW 8u
#define NV_GSP_WPR_SYSMEM_LIMIT          9u

#define NV_GSP_WPR_SYSMEM_PAGE_BYTES     4096ull

struct NVGspWprSysmemInput {
    bool radix3Prepared;
    NVGspWprSysU64 radix3RootGpuAddress;
    NVGspWprSysU64 radix3ImageBytes; // exact .fwimage bytes, not Radix3 allocation bytes

    bool bootPrepared;
    NVGspWprSysU64 bootGpuAddress;
    NVGspWprSysU64 bootImageBytes;   // exact unpadded boot-ucode image bytes
    NVGspWprSysU64 bootCodeOffset;
    NVGspWprSysU64 bootDataOffset;
    NVGspWprSysU64 bootManifestOffset;
};

struct NVGspWprSysmemPlan {
    NVGspWprSysU64 radix3RootGpuAddress;
    NVGspWprSysU64 radix3ImageBytes;
    NVGspWprSysU64 bootGpuAddress;
    NVGspWprSysU64 bootImageBytes;
    NVGspWprSysU64 bootCodeOffset;
    NVGspWprSysU64 bootDataOffset;
    NVGspWprSysU64 bootManifestOffset;
    bool valid;
};

static inline bool nvGspWprSysmemSucceeded(NVGspWprSysU32 verdict)
{
    return verdict == NV_GSP_WPR_SYSMEM_OK;
}

static inline const char *nvGspWprSysmemVerdictName(NVGspWprSysU32 verdict)
{
    switch (verdict) {
        case NV_GSP_WPR_SYSMEM_NOT_ATTEMPTED:  return "not-attempted";
        case NV_GSP_WPR_SYSMEM_OK:             return "ok";
        case NV_GSP_WPR_SYSMEM_NULL_OUTPUT:    return "null-output";
        case NV_GSP_WPR_SYSMEM_STAGE_MISSING:  return "stage-missing";
        case NV_GSP_WPR_SYSMEM_BAD_RADIX3:     return "bad-radix3";
        case NV_GSP_WPR_SYSMEM_BAD_BOOT:       return "bad-boot";
        case NV_GSP_WPR_SYSMEM_BAD_ALIGNMENT:  return "bad-alignment";
        case NV_GSP_WPR_SYSMEM_BAD_OFFSETS:    return "bad-offsets";
        case NV_GSP_WPR_SYSMEM_RANGE_OVERFLOW: return "range-overflow";
        default:                                return "unknown";
    }
}

// The Radix3 address is a root TABLE address. The firmware bytes are reached
// through that hierarchy, so root+imageBytes is NOT a contiguous DMA range and
// must not be validated as one. The boot-ucode address, in contrast, is the
// base of one contiguous GA107 IOVA span and may be range-checked directly.
static inline NVGspWprSysU32 nvGspPlanWprSysmem(
    const NVGspWprSysmemInput *in,
    NVGspWprSysmemPlan *out)
{
    if (!out)
        return NV_GSP_WPR_SYSMEM_NULL_OUTPUT;
    if (!in || !in->radix3Prepared || !in->bootPrepared)
        return NV_GSP_WPR_SYSMEM_STAGE_MISSING;

    if (!in->radix3RootGpuAddress || !in->radix3ImageBytes)
        return NV_GSP_WPR_SYSMEM_BAD_RADIX3;
    if (!in->bootGpuAddress || !in->bootImageBytes)
        return NV_GSP_WPR_SYSMEM_BAD_BOOT;

    if ((in->radix3RootGpuAddress & (NV_GSP_WPR_SYSMEM_PAGE_BYTES - 1ull)) != 0ull ||
        (in->bootGpuAddress & (NV_GSP_WPR_SYSMEM_PAGE_BYTES - 1ull)) != 0ull)
        return NV_GSP_WPR_SYSMEM_BAD_ALIGNMENT;

    // Offset zero is legal. The observed GA107 manifest starts at offset zero.
    if (in->bootCodeOffset >= in->bootImageBytes ||
        in->bootDataOffset >= in->bootImageBytes ||
        in->bootManifestOffset >= in->bootImageBytes)
        return NV_GSP_WPR_SYSMEM_BAD_OFFSETS;

    if (in->bootGpuAddress > ~0ull - (in->bootImageBytes - 1ull))
        return NV_GSP_WPR_SYSMEM_RANGE_OVERFLOW;

    NVGspWprSysmemPlan staged = {};
    staged.radix3RootGpuAddress = in->radix3RootGpuAddress;
    staged.radix3ImageBytes = in->radix3ImageBytes;
    staged.bootGpuAddress = in->bootGpuAddress;
    staged.bootImageBytes = in->bootImageBytes;
    staged.bootCodeOffset = in->bootCodeOffset;
    staged.bootDataOffset = in->bootDataOffset;
    staged.bootManifestOffset = in->bootManifestOffset;
    staged.valid = true;
    *out = staged;
    return NV_GSP_WPR_SYSMEM_OK;
}

#endif // NV_GSP_WPR_SYSMEM_HPP
