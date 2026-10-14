// SPDX-License-Identifier: MIT
// Pure GA107-domain DMA contract for the WPR boot-ucode staging image.
#ifndef NV_GSP_WPR_BOOT_DMA_HPP
#define NV_GSP_WPR_BOOT_DMA_HPP

#define NV_GSP_WPR_BOOT_DMA_NOT_ATTEMPTED    0u
#define NV_GSP_WPR_BOOT_DMA_OK               1u
#define NV_GSP_WPR_BOOT_DMA_NULL_OUTPUT      2u
#define NV_GSP_WPR_BOOT_DMA_BAD_ALLOCATION   3u
#define NV_GSP_WPR_BOOT_DMA_BAD_SEGMENT      4u
#define NV_GSP_WPR_BOOT_DMA_BAD_ALIGNMENT    5u
#define NV_GSP_WPR_BOOT_DMA_LENGTH_MISMATCH  6u
#define NV_GSP_WPR_BOOT_DMA_RANGE_OVERFLOW   7u
#define NV_GSP_WPR_BOOT_DMA_LIMIT            8u

#define NV_GSP_WPR_BOOT_DMA_PAGE_BYTES       4096ull

typedef unsigned long long NVGspWprBootDmaU64;
typedef unsigned int       NVGspWprBootDmaU32;

struct NVGspWprBootDmaPlan {
    NVGspWprBootDmaU64 gpuAddress;
    NVGspWprBootDmaU64 sourceBytes;
    NVGspWprBootDmaU64 allocationBytes;
    bool valid;
};

static inline bool nvGspWprBootDmaSucceeded(NVGspWprBootDmaU32 verdict)
{
    return verdict == NV_GSP_WPR_BOOT_DMA_OK;
}

static inline const char *nvGspWprBootDmaVerdictName(NVGspWprBootDmaU32 verdict)
{
    switch (verdict) {
        case NV_GSP_WPR_BOOT_DMA_NOT_ATTEMPTED:   return "not-attempted";
        case NV_GSP_WPR_BOOT_DMA_OK:              return "ok";
        case NV_GSP_WPR_BOOT_DMA_NULL_OUTPUT:     return "null-output";
        case NV_GSP_WPR_BOOT_DMA_BAD_ALLOCATION:  return "bad-allocation";
        case NV_GSP_WPR_BOOT_DMA_BAD_SEGMENT:     return "bad-segment";
        case NV_GSP_WPR_BOOT_DMA_BAD_ALIGNMENT:   return "bad-alignment";
        case NV_GSP_WPR_BOOT_DMA_LENGTH_MISMATCH: return "length-mismatch";
        case NV_GSP_WPR_BOOT_DMA_RANGE_OVERFLOW:  return "range-overflow";
        default:                                  return "unknown";
    }
}

// WPR metadata carries exactly ONE sysmemAddrOfBootloader + one size. There is
// no scatter list in GspFwWprMetaV1 for this object, so the macOS staging shell
// must only publish a DMA address when GA107 sees the whole allocation through
// one contiguous IOVM segment. Physical pages may remain noncontiguous behind
// the device mapper; this contract is only about the device-visible IOVA.
//
// No 41-bit Phase-5 policy is inherited here. That ceiling belongs to the
// Falcon DMATRFBASE transfer path. The WPR pointer is an AT_GPU/device-domain
// address in open RM and gets a separate proof before any Booter execution.
static inline NVGspWprBootDmaU32 nvGspPlanWprBootDma(
    NVGspWprBootDmaU64 segmentAddress,
    NVGspWprBootDmaU64 segmentBytes,
    NVGspWprBootDmaU64 sourceBytes,
    NVGspWprBootDmaU64 allocationBytes,
    NVGspWprBootDmaPlan *out)
{
    if (!out)
        return NV_GSP_WPR_BOOT_DMA_NULL_OUTPUT;

    if (!sourceBytes || !allocationBytes || sourceBytes > allocationBytes ||
        (allocationBytes & (NV_GSP_WPR_BOOT_DMA_PAGE_BYTES - 1ull)) != 0ull)
        return NV_GSP_WPR_BOOT_DMA_BAD_ALLOCATION;

    if (!segmentAddress || !segmentBytes)
        return NV_GSP_WPR_BOOT_DMA_BAD_SEGMENT;

    if ((segmentAddress & (NV_GSP_WPR_BOOT_DMA_PAGE_BYTES - 1ull)) != 0ull)
        return NV_GSP_WPR_BOOT_DMA_BAD_ALIGNMENT;

    // IODMACommand is bound to exactly allocationBytes. A different length is
    // surprising enough to refuse rather than silently widening/narrowing the
    // address contract Booter will later receive.
    if (segmentBytes != allocationBytes)
        return NV_GSP_WPR_BOOT_DMA_LENGTH_MISMATCH;

    // Prove both the allocation and the exact unpadded firmware source range
    // are representable without wrapping the 64-bit device address.
    if (segmentAddress > ~0ull - (allocationBytes - 1ull) ||
        segmentAddress > ~0ull - (sourceBytes - 1ull))
        return NV_GSP_WPR_BOOT_DMA_RANGE_OVERFLOW;

    NVGspWprBootDmaPlan staged = {};
    staged.gpuAddress = segmentAddress;
    staged.sourceBytes = sourceBytes;
    staged.allocationBytes = allocationBytes;
    staged.valid = true;
    *out = staged;
    return NV_GSP_WPR_BOOT_DMA_OK;
}

#endif // NV_GSP_WPR_BOOT_DMA_HPP
