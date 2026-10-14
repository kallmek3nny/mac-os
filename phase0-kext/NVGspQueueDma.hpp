// SPDX-License-Identifier: MIT
// Pure conversion from device-visible DMA segments to 4 KiB GSP queue pages.
#ifndef NV_GSP_QUEUE_DMA_HPP
#define NV_GSP_QUEUE_DMA_HPP

#include "NVGspQueueMemory.hpp"

using NVGspDmaU32 = unsigned int;
using NVGspDmaU64 = unsigned long long;

#define NV_GSP_QUEUE_DMA_NOT_ATTEMPTED       0u
#define NV_GSP_QUEUE_DMA_OK                  1u
#define NV_GSP_QUEUE_DMA_NULL_LAYOUT         2u
#define NV_GSP_QUEUE_DMA_NULL_SEGMENTS       3u
#define NV_GSP_QUEUE_DMA_NULL_OUTPUT         4u
#define NV_GSP_QUEUE_DMA_BAD_SEGMENT_COUNT   5u
#define NV_GSP_QUEUE_DMA_BAD_SEGMENT         6u
#define NV_GSP_QUEUE_DMA_SHORT_TRANSFER      7u
#define NV_GSP_QUEUE_DMA_OUTPUT_SHORT        8u
#define NV_GSP_QUEUE_DMA_OVERFLOW            9u
#define NV_GSP_QUEUE_DMA_LIMIT               10u

struct NVGspQueueDmaSegment {
    NVGspDmaU64 address;
    NVGspDmaU64 length;
};

struct NVGspQueueDmaPlan {
    NVGspDmaU32 inputSegmentCount;
    NVGspDmaU32 outputPageCount;
    NVGspDmaU64 mappedBytes;
    NVGspDmaU64 firstPageAddress;
    NVGspDmaU64 lastPageAddress;
    bool inputWasSingleContiguousSegment;
};

static inline bool nvGspQueueDmaSucceeded(NVGspDmaU32 verdict)
{
    return verdict == NV_GSP_QUEUE_DMA_OK;
}

static inline const char *nvGspQueueDmaVerdictName(NVGspDmaU32 verdict)
{
    switch (verdict) {
        case NV_GSP_QUEUE_DMA_NOT_ATTEMPTED:     return "not-attempted";
        case NV_GSP_QUEUE_DMA_OK:                return "ok";
        case NV_GSP_QUEUE_DMA_NULL_LAYOUT:       return "null-layout";
        case NV_GSP_QUEUE_DMA_NULL_SEGMENTS:     return "null-segments";
        case NV_GSP_QUEUE_DMA_NULL_OUTPUT:       return "null-output";
        case NV_GSP_QUEUE_DMA_BAD_SEGMENT_COUNT: return "bad-segment-count";
        case NV_GSP_QUEUE_DMA_BAD_SEGMENT:       return "bad-segment";
        case NV_GSP_QUEUE_DMA_SHORT_TRANSFER:    return "short-transfer";
        case NV_GSP_QUEUE_DMA_OUTPUT_SHORT:      return "output-short";
        case NV_GSP_QUEUE_DMA_OVERFLOW:          return "overflow";
        default:                                 return "unknown";
    }
}

// Flatten device-visible DMA segments in descriptor order into the 4 KiB page
// address list GSP-RM stores at the beginning of its shared queue allocation.
// Segments do not need to be physically/IOVA contiguous with each other, but
// every address and segment length must be page aligned. The segment lengths
// must cover EXACTLY layout->sharedBytes: accepting extra bytes would make it
// too easy for a caller to hand GSP a page outside the owned descriptor.
static inline NVGspDmaU32 nvGspFlattenQueueDmaSegments(
    const NVGspQueuePlan *layout,
    const NVGspQueueDmaSegment *segments,
    NVGspDmaU32 segmentCount,
    NVGspDmaU64 *outPageAddress,
    NVGspDmaU32 outPageCapacity,
    NVGspQueueDmaPlan *outPlan)
{
    if (!layout) return NV_GSP_QUEUE_DMA_NULL_LAYOUT;
    if (!segments) return NV_GSP_QUEUE_DMA_NULL_SEGMENTS;
    if (!outPageAddress || !outPlan) return NV_GSP_QUEUE_DMA_NULL_OUTPUT;
    if (!segmentCount) return NV_GSP_QUEUE_DMA_BAD_SEGMENT_COUNT;
    if (outPageCapacity < layout->pageTableEntryCount)
        return NV_GSP_QUEUE_DMA_OUTPUT_SHORT;
    if (!layout->sharedBytes ||
        (layout->sharedBytes & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0u ||
        layout->pageTableEntryCount != (layout->sharedBytes >> NV_GSP_QUEUE_PAGE_SHIFT))
        return NV_GSP_QUEUE_DMA_SHORT_TRANSFER;

    NVGspDmaU64 total = 0ull;
    NVGspDmaU32 pages = 0u;
    for (NVGspDmaU32 s = 0u; s < segmentCount; ++s) {
        const NVGspDmaU64 address = segments[s].address;
        const NVGspDmaU64 length = segments[s].length;
        if (!length ||
            (address & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0ull ||
            (length & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0ull)
            return NV_GSP_QUEUE_DMA_BAD_SEGMENT;
        if (address > ~0ull - (length - NV_GSP_QUEUE_PAGE_BYTES))
            return NV_GSP_QUEUE_DMA_OVERFLOW;
        if (total > ~0ull - length)
            return NV_GSP_QUEUE_DMA_OVERFLOW;
        total += length;
        if (total > layout->sharedBytes)
            return NV_GSP_QUEUE_DMA_SHORT_TRANSFER;

        const NVGspDmaU64 segmentPages = length >> NV_GSP_QUEUE_PAGE_SHIFT;
        if (segmentPages > (NVGspDmaU64)(layout->pageTableEntryCount - pages))
            return NV_GSP_QUEUE_DMA_SHORT_TRANSFER;
        pages += (NVGspDmaU32)segmentPages;
    }

    if (total != layout->sharedBytes || pages != layout->pageTableEntryCount)
        return NV_GSP_QUEUE_DMA_SHORT_TRANSFER;

    // Validation is complete before the caller-visible page array is touched.
    NVGspDmaU32 outIndex = 0u;
    for (NVGspDmaU32 s = 0u; s < segmentCount; ++s) {
        for (NVGspDmaU64 off = 0ull; off < segments[s].length;
             off += NV_GSP_QUEUE_PAGE_BYTES) {
            outPageAddress[outIndex++] = segments[s].address + off;
        }
    }

    NVGspQueueDmaPlan staged = {};
    staged.inputSegmentCount = segmentCount;
    staged.outputPageCount = outIndex;
    staged.mappedBytes = total;
    staged.firstPageAddress = outPageAddress[0];
    staged.lastPageAddress = outPageAddress[outIndex - 1u];
    staged.inputWasSingleContiguousSegment = segmentCount == 1u;
    *outPlan = staged;
    return NV_GSP_QUEUE_DMA_OK;
}

#endif // NV_GSP_QUEUE_DMA_HPP
