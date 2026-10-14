// SPDX-License-Identifier: MIT
// Pure NVIDIA GSP-RM RPC framing for production non-CC queues. No queue writes.
#ifndef NV_GSP_RPC_FRAME_HPP
#define NV_GSP_RPC_FRAME_HPP

#include "NVGspQueuePlan.hpp"

using NVGspRpcU32 = unsigned int;
using NVGspRpcU64 = unsigned long long;

// message_queue_priv.h / mctp_format.h / nvdm_format.h / rpc_headers.h
#define NV_GSP_RPC_MCTP_MAGIC                     0x4d435450u
#define NV_GSP_RPC_MCTP_SINGLE_HEADER             0xc0000001u
#define NV_GSP_RPC_NVDM_TYPE_RM_RPC               0x25u
#define NV_GSP_RPC_NVDM_VENDOR_HEADER_RM_RPC      0x2510de7eu
#define NV_GSP_RPC_HEADER_VERSION                 0x03000000u
#define NV_GSP_RPC_SIGNATURE_VALID                0x43505256u
#define NV_GSP_RPC_RESULT_PENDING                 0xffffffffu
#define NV_GSP_RPC_UNION_INIT                     0x00000000u
#define NV_GSP_RPC_HEADER_DWORDS                  8u
#define NV_GSP_RPC_HEADER_BYTES                   (NV_GSP_RPC_HEADER_DWORDS * 4u)
#define NV_GSP_RPC_OUTER_NO_CC_DWORDS             6u
#define NV_GSP_RPC_OUTER_NO_CC_BYTES              (NV_GSP_RPC_OUTER_NO_CC_DWORDS * 4u)
#define NV_GSP_RPC_PREFIX_DWORDS                  (NV_GSP_RPC_OUTER_NO_CC_DWORDS + NV_GSP_RPC_HEADER_DWORDS)
#define NV_GSP_RPC_PREFIX_BYTES                   (NV_GSP_RPC_PREFIX_DWORDS * 4u)
#define NV_GSP_RPC_MAX_PARAM_BYTES_NO_CC          (NV_GSP_QUEUE_ELEMENT_MAX_BYTES - NV_GSP_RPC_PREFIX_BYTES)

#define NV_GSP_RPC_FRAME_NOT_ATTEMPTED            0u
#define NV_GSP_RPC_FRAME_OK                       1u
#define NV_GSP_RPC_FRAME_NULL_OUTPUT              2u
#define NV_GSP_RPC_FRAME_PARAMS_TOO_LARGE         3u
#define NV_GSP_RPC_FRAME_OVERFLOW                 4u
#define NV_GSP_RPC_FRAME_LIMIT                    5u

struct NVGspRpcFramePlan {
    NVGspRpcU32 prefix[NV_GSP_RPC_PREFIX_DWORDS];
    NVGspRpcU32 prefixBytes;
    NVGspRpcU32 parameterBytes;
    NVGspRpcU32 rpcBytes;
    NVGspRpcU32 queueMessageBytes;
};

static inline bool nvGspRpcFrameSucceeded(NVGspRpcU32 verdict)
{
    return verdict==NV_GSP_RPC_FRAME_OK;
}

static inline const char *nvGspRpcFrameVerdictName(NVGspRpcU32 verdict)
{
    switch (verdict) {
        case NV_GSP_RPC_FRAME_NOT_ATTEMPTED:    return "not-attempted";
        case NV_GSP_RPC_FRAME_OK:               return "ok";
        case NV_GSP_RPC_FRAME_NULL_OUTPUT:      return "null-output";
        case NV_GSP_RPC_FRAME_PARAMS_TOO_LARGE: return "params-too-large";
        case NV_GSP_RPC_FRAME_OVERFLOW:         return "overflow";
        default:                                return "unknown";
    }
}

// Build the fixed prefix for one current-production non-confidential RM-RPC
// command queue element. The caller appends exactly `parameterBytes` bytes after
// this prefix. NVIDIA's send path then wraps/queues the complete message; this
// helper performs neither action.
//
// Source-backed nesting:
//   GSP_MSG_QUEUE_ELEMENT(no CC)       24 bytes
//     mctpMagic
//     mctpPayloadSize = 24 + rpcBytes
//     mctpHeader      = SOM=1,EOM=1,SEID/DEID/SEQ=0,version=1
//     nvdmHeader      = vendor PCI, NVIDIA 0x10de, RM_RPC type 0x25
//     nvdmPayloadSize = rpcBytes
//     reserved        = 0
//   rpc_message_header_v03_00          32 bytes
//     version 3.0, valid signature, pending results, caller function/sequence
//
// `sequence` is supplied by the caller because KernelGsp owns/increments that
// counter. A numeric zero is valid, including the first RPC and natural wrap.
static inline NVGspRpcU32 nvGspBuildRmRpcFrameNoCc(NVGspRpcU32 function,
                                                   NVGspRpcU32 sequence,
                                                   NVGspRpcU32 parameterBytes,
                                                   NVGspRpcFramePlan *out)
{
    if (!out) return NV_GSP_RPC_FRAME_NULL_OUTPUT;
    if (parameterBytes>NV_GSP_RPC_MAX_PARAM_BYTES_NO_CC)
        return NV_GSP_RPC_FRAME_PARAMS_TOO_LARGE;

    const NVGspRpcU64 rpcBytes=(NVGspRpcU64)NV_GSP_RPC_HEADER_BYTES+parameterBytes;
    const NVGspRpcU64 queueBytes=(NVGspRpcU64)NV_GSP_RPC_OUTER_NO_CC_BYTES+rpcBytes;
    if (rpcBytes>0xffffffffull || queueBytes>0xffffffffull ||
        queueBytes>NV_GSP_QUEUE_ELEMENT_MAX_BYTES)
        return NV_GSP_RPC_FRAME_OVERFLOW;

    NVGspRpcFramePlan staged={};
    staged.prefix[0]=NV_GSP_RPC_MCTP_MAGIC;
    staged.prefix[1]=(NVGspRpcU32)queueBytes;
    staged.prefix[2]=NV_GSP_RPC_MCTP_SINGLE_HEADER;
    staged.prefix[3]=NV_GSP_RPC_NVDM_VENDOR_HEADER_RM_RPC;
    staged.prefix[4]=(NVGspRpcU32)rpcBytes;
    staged.prefix[5]=0u;

    staged.prefix[6]=NV_GSP_RPC_HEADER_VERSION;
    staged.prefix[7]=NV_GSP_RPC_SIGNATURE_VALID;
    staged.prefix[8]=(NVGspRpcU32)rpcBytes;
    staged.prefix[9]=function;
    staged.prefix[10]=NV_GSP_RPC_RESULT_PENDING;
    staged.prefix[11]=NV_GSP_RPC_RESULT_PENDING;
    staged.prefix[12]=sequence;
    staged.prefix[13]=NV_GSP_RPC_UNION_INIT;

    staged.prefixBytes=NV_GSP_RPC_PREFIX_BYTES;
    staged.parameterBytes=parameterBytes;
    staged.rpcBytes=(NVGspRpcU32)rpcBytes;
    staged.queueMessageBytes=(NVGspRpcU32)queueBytes;
    *out=staged;
    return NV_GSP_RPC_FRAME_OK;
}

#endif // NV_GSP_RPC_FRAME_HPP
