// SPDX-License-Identifier: MIT
// Pure validation for a 3050-side IOMMU mapping of the Intel-owned sink.
#ifndef INTEL_SINK_PEER_HPP
#define INTEL_SINK_PEER_HPP

#include "NVGspStage.hpp"

using IGPeerU32 = unsigned int;
using IGPeerU64 = unsigned long long;

// First peer-mapping policy deliberately reuses the NVIDIA path's existing
// conservative 41-bit DMA window. NVGspStage.hpp documents that 41 is a POLICY
// ceiling, not an observed GA107 maximum. Keeping the first cross-GPU mapping
// inside that already-tested envelope is the safe direction; widening it later
// needs evidence from the copy/render path, not an assumption made here.
#define IG_SINK_PEER_DMA_ADDR_BITS_POLICY NV_GSP_FW_ADDR_BITS
#define IG_SINK_PEER_DMA_LIMIT_POLICY     NV_GSP_FW_PHYS_LIMIT
#define IG_SINK_PEER_PAGE_BYTES           4096u

#define IG_SINK_PEER_NOT_ATTEMPTED        0u
#define IG_SINK_PEER_OK                   1u
#define IG_SINK_PEER_NULL_OUT             2u
#define IG_SINK_PEER_ZERO_SIZE            3u
#define IG_SINK_PEER_IOVM_UNALIGNED       4u
#define IG_SINK_PEER_IOVM_RANGE           5u
#define IG_SINK_PEER_SEGMENT_SHORT        6u
#define IG_SINK_PEER_LIMIT                7u

struct IGSinkPeerMap {
    IGPeerU64 iovmAddress;
    IGPeerU64 segmentBytes;
    IGPeerU64 mappedBytes;
};

static inline void igSinkPeerClear(IGSinkPeerMap *out)
{
    out->iovmAddress=0ull;
    out->segmentBytes=0ull;
    out->mappedBytes=0ull;
}

static inline bool igSinkPeerSucceeded(IGPeerU32 verdict)
{
    return verdict==IG_SINK_PEER_OK;
}

static inline const char *igSinkPeerVerdictName(IGPeerU32 verdict)
{
    switch (verdict) {
        case IG_SINK_PEER_NOT_ATTEMPTED:  return "not-attempted";
        case IG_SINK_PEER_OK:             return "ok";
        case IG_SINK_PEER_NULL_OUT:       return "null-out";
        case IG_SINK_PEER_ZERO_SIZE:      return "zero-size";
        case IG_SINK_PEER_IOVM_UNALIGNED: return "iovm-unaligned";
        case IG_SINK_PEER_IOVM_RANGE:     return "iovm-range";
        case IG_SINK_PEER_SEGMENT_SHORT:  return "segment-short";
        default:                          return "unknown";
    }
}

static inline bool igSinkPeerPageAligned(IGPeerU64 value)
{
    return (value & (IGPeerU64)(IG_SINK_PEER_PAGE_BYTES-1u))==0ull;
}

// Validate one contiguous IOVM segment returned from an IODMACommand built with
// the NVIDIA provider's IOMapper. This does NOT prove any NVIDIA engine can yet
// consume the address; it only proves the mapper produced one contiguous range
// inside the conservative address policy and large enough for the sink.
static inline IGPeerU32 igSinkPeerPlan(IGPeerU64 iovmAddress,
                                      IGPeerU64 segmentBytes,
                                      IGPeerU64 requiredBytes,
                                      IGSinkPeerMap *out)
{
    if (!out) return IG_SINK_PEER_NULL_OUT;
    igSinkPeerClear(out);
    if (!requiredBytes) return IG_SINK_PEER_ZERO_SIZE;
    if (!igSinkPeerPageAligned(iovmAddress))
        return IG_SINK_PEER_IOVM_UNALIGNED;
    if (iovmAddress>IG_SINK_PEER_DMA_LIMIT_POLICY)
        return IG_SINK_PEER_IOVM_RANGE;
    if (requiredBytes-1ull > IG_SINK_PEER_DMA_LIMIT_POLICY-iovmAddress)
        return IG_SINK_PEER_IOVM_RANGE;
    if (segmentBytes<requiredBytes)
        return IG_SINK_PEER_SEGMENT_SHORT;

    out->iovmAddress=iovmAddress;
    out->segmentBytes=segmentBytes;
    out->mappedBytes=requiredBytes;
    return IG_SINK_PEER_OK;
}

#endif // INTEL_SINK_PEER_HPP
