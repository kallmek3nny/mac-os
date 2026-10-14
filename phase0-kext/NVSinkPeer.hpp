// SPDX-License-Identifier: MIT
// Pure validation for the GA107-side IOVM assigned to the shared sink backing.
#ifndef NV_SINK_PEER_HPP
#define NV_SINK_PEER_HPP

using NVPeerU32 = unsigned int;
using NVPeerU64 = unsigned long long;

// Conservative policy, deliberately no wider than the already exercised GA107
// GSP staging path (NV_GSP_FW_ADDR_BITS = 41). This is NOT a claim that general
// graphics engines are limited to 41 bits; it is the strongest address policy
// this driver has already handed to NVIDIA hardware through IODMACommand.
#define NV_SINK_PEER_ADDR_BITS_POLICY_41       41u
#define NV_SINK_PEER_ADDR_LIMIT_POLICY         ((1ull<<NV_SINK_PEER_ADDR_BITS_POLICY_41)-1ull)
#define NV_SINK_PEER_ALIGN_POLICY_4096         4096u

#define NV_SINK_PEER_NOT_ATTEMPTED             0u
#define NV_SINK_PEER_OK                        1u
#define NV_SINK_PEER_ZERO_BYTES                2u
#define NV_SINK_PEER_ADDR_ZERO                 3u
#define NV_SINK_PEER_ADDR_UNALIGNED            4u
#define NV_SINK_PEER_ADDR_RANGE                5u
#define NV_SINK_PEER_SEGMENT_SHORT             6u
#define NV_SINK_PEER_OFFSET_SHORT              7u
#define NV_SINK_PEER_LIMIT                     8u

static inline bool nvSinkPeerSucceeded(NVPeerU32 verdict)
{
    return verdict==NV_SINK_PEER_OK;
}

static inline const char *nvSinkPeerVerdictName(NVPeerU32 verdict)
{
    switch (verdict) {
        case NV_SINK_PEER_NOT_ATTEMPTED: return "not-attempted";
        case NV_SINK_PEER_OK:            return "ok";
        case NV_SINK_PEER_ZERO_BYTES:    return "zero-bytes";
        case NV_SINK_PEER_ADDR_ZERO:     return "address-zero";
        case NV_SINK_PEER_ADDR_UNALIGNED:return "address-unaligned";
        case NV_SINK_PEER_ADDR_RANGE:    return "address-range";
        case NV_SINK_PEER_SEGMENT_SHORT: return "segment-short";
        case NV_SINK_PEER_OFFSET_SHORT:  return "offset-short";
        default:                         return "unknown";
    }
}

static inline NVPeerU32 nvSinkPeerValidate(NVPeerU64 iovmAddress,
                                           NVPeerU64 segmentBytes,
                                           NVPeerU64 consumedOffset,
                                           NVPeerU64 requiredBytes)
{
    if (!requiredBytes) return NV_SINK_PEER_ZERO_BYTES;
    if (!iovmAddress) return NV_SINK_PEER_ADDR_ZERO;
    if ((iovmAddress & (NVPeerU64)(NV_SINK_PEER_ALIGN_POLICY_4096-1u))!=0ull)
        return NV_SINK_PEER_ADDR_UNALIGNED;
    if (iovmAddress>NV_SINK_PEER_ADDR_LIMIT_POLICY ||
        (requiredBytes-1ull)>NV_SINK_PEER_ADDR_LIMIT_POLICY-iovmAddress)
        return NV_SINK_PEER_ADDR_RANGE;
    if (segmentBytes<requiredBytes) return NV_SINK_PEER_SEGMENT_SHORT;
    if (consumedOffset<requiredBytes) return NV_SINK_PEER_OFFSET_SHORT;
    return NV_SINK_PEER_OK;
}

#endif // NV_SINK_PEER_HPP
