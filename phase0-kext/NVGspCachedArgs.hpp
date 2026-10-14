// SPDX-License-Identifier: MIT
// Pure GSP-RM cached-argument queue overlay + LibOS RMARGS region contract.
#ifndef NV_GSP_CACHED_ARGS_HPP
#define NV_GSP_CACHED_ARGS_HPP

#include "NVGspQueueMemory.hpp"

using NVGspArgsU8  = unsigned char;
using NVGspArgsU16 = unsigned short;
using NVGspArgsU32 = unsigned int;
using NVGspArgsU64 = unsigned long long;

#define NV_GSP_CACHED_ARGS_PAGE_BYTES             4096u
#define NV_GSP_CACHED_ARGS_MAGIC                  0x20505347u
#define NV_GSP_CACHED_ARGS_QUEUE_OFFSET           16u
#define NV_GSP_CACHED_ARGS_QUEUE_BYTES            64u
#define NV_GSP_CACHED_ARGS_CURRENT_BYTES          184u

#define NV_GSP_LIBOS_REGION_CONTIGUOUS             1u
#define NV_GSP_LIBOS_REGION_LOC_SYSMEM             1u
#define NV_GSP_LIBOS_RMARGS_ID                     0x0000524d41524753ull
#define NV_GSP_LIBOS_REGION_BYTES                  32u

#define NV_GSP_CACHED_ARGS_NOT_ATTEMPTED           0u
#define NV_GSP_CACHED_ARGS_OK                      1u
#define NV_GSP_CACHED_ARGS_NULL_PAGE               2u
#define NV_GSP_CACHED_ARGS_NULL_QUEUE              3u
#define NV_GSP_CACHED_ARGS_NULL_OUTPUT             4u
#define NV_GSP_CACHED_ARGS_PAGE_TOO_SHORT          5u
#define NV_GSP_CACHED_ARGS_BAD_MAGIC               6u
#define NV_GSP_CACHED_ARGS_BAD_SIZE                7u
#define NV_GSP_CACHED_ARGS_BAD_RMARGS_ADDRESS      8u
#define NV_GSP_CACHED_ARGS_LIMIT                   9u

struct NVGspSrInitArguments {
    NVGspArgsU32 oldLevel;
    NVGspArgsU32 flags;
    NVGspArgsU8 reserved[8];
};

struct NVGspPairArguments {
    NVGspArgsU64 pa;
    NVGspArgsU64 size;
};

// Mirrors the current ABI-stable GSP_ARGUMENTS_CACHED layout. Runtime code
// must not synthesize all of this structure: several fields are populated by
// other RM subsystems. The structure exists here to pin offsets/size and to
// validate that the queue overlay remains at bytes 16..79.
struct NVGspCachedArguments {
    NVGspArgsU32 magic;
    NVGspArgsU16 size;
    NVGspArgsU8 reserved[2];
    NVGspArgsU64 flags;
    NVGspQueueInitArguments messageQueueInitArguments;
    NVGspSrInitArguments srInitArguments;
    NVGspArgsU8 reserved2[4];
    NVGspArgsU32 gpuInstance;
    NVGspPairArguments profilerArgs;
    NVGspPairArguments sysmemHeapArgs;
    NVGspPairArguments rmStateMonitorBufferArgs;
    NVGspPairArguments bindataArgs; // `pa` is source field `radix3` here.
    NVGspPairArguments vbiosOverrideArgs;
};

// Explicitly padded form of LibosMemoryRegionInitArgument. NVIDIA's source
// fields occupy 26 bytes and natural 8-byte alignment rounds sizeof to 32.
struct NVGspLibosMemoryRegion {
    NVGspArgsU64 id8;
    NVGspArgsU64 pa;
    NVGspArgsU64 size;
    NVGspArgsU8 kind;
    NVGspArgsU8 loc;
    NVGspArgsU8 padding[6];
};

struct NVGspCachedArgsOverlayPlan {
    NVGspArgsU32 cachedArgsSize;
    NVGspArgsU32 queueOffset;
    NVGspArgsU32 queueBytes;
    NVGspArgsU32 preservedPrefixBytes;
    NVGspArgsU32 preservedSuffixOffset;
    NVGspLibosMemoryRegion rmArgsRegion;
    bool fullFenceAfterLibosRegionList;
};

static inline bool nvGspCachedArgsSucceeded(NVGspArgsU32 verdict)
{
    return verdict == NV_GSP_CACHED_ARGS_OK;
}

static inline const char *nvGspCachedArgsVerdictName(NVGspArgsU32 verdict)
{
    switch (verdict) {
        case NV_GSP_CACHED_ARGS_NOT_ATTEMPTED:      return "not-attempted";
        case NV_GSP_CACHED_ARGS_OK:                 return "ok";
        case NV_GSP_CACHED_ARGS_NULL_PAGE:          return "null-page";
        case NV_GSP_CACHED_ARGS_NULL_QUEUE:         return "null-queue";
        case NV_GSP_CACHED_ARGS_NULL_OUTPUT:        return "null-output";
        case NV_GSP_CACHED_ARGS_PAGE_TOO_SHORT:     return "page-too-short";
        case NV_GSP_CACHED_ARGS_BAD_MAGIC:          return "bad-magic";
        case NV_GSP_CACHED_ARGS_BAD_SIZE:           return "bad-size";
        case NV_GSP_CACHED_ARGS_BAD_RMARGS_ADDRESS: return "bad-rmargs-address";
        default:                                     return "unknown";
    }
}

// Exact open-RM `_kgspGenerateInitArgId()` spelling: consume at most eight
// bytes and shift each ASCII byte into a big-endian 64-bit tag.
static inline NVGspArgsU64 nvGspGenerateInitArgId(const char *name)
{
    if (!name) return 0ull;
    NVGspArgsU64 id = 0ull;
    for (NVGspArgsU32 i = 0u; i < 8u; ++i) {
        const NVGspArgsU8 c = (NVGspArgsU8)*name++;
        if (c == 0u) break;
        id = (id << 8) | (NVGspArgsU64)c;
    }
    return id;
}

static inline NVGspArgsU16 nvGspReadLe16(const NVGspArgsU8 *p)
{
    return (NVGspArgsU16)((NVGspArgsU16)p[0] | ((NVGspArgsU16)p[1] << 8));
}

static inline NVGspArgsU32 nvGspReadLe32(const NVGspArgsU8 *p)
{
    return (NVGspArgsU32)p[0] |
           ((NVGspArgsU32)p[1] << 8) |
           ((NVGspArgsU32)p[2] << 16) |
           ((NVGspArgsU32)p[3] << 24);
}

// Validate an existing RMARGS page before allowing a queue-only overlay.
// GSP_ARGUMENTS_CACHED is ABI-stable and may grow by appending fields, so the
// validator accepts a size >= the queue field's end and <= the 4 KiB page
// rather than requiring today's 184-byte exact total.
static inline NVGspArgsU32 nvGspPlanCachedArgsQueueOverlay(
    const NVGspArgsU8 *rmArgsPage,
    NVGspArgsU32 pageBytes,
    NVGspArgsU64 rmArgsGpuAddress,
    NVGspCachedArgsOverlayPlan *out)
{
    if (!rmArgsPage) return NV_GSP_CACHED_ARGS_NULL_PAGE;
    if (!out) return NV_GSP_CACHED_ARGS_NULL_OUTPUT;
    if (pageBytes < NV_GSP_CACHED_ARGS_PAGE_BYTES)
        return NV_GSP_CACHED_ARGS_PAGE_TOO_SHORT;

    const NVGspArgsU32 magic = nvGspReadLe32(rmArgsPage);
    const NVGspArgsU16 size = nvGspReadLe16(rmArgsPage + 4u);
    if (magic != NV_GSP_CACHED_ARGS_MAGIC)
        return NV_GSP_CACHED_ARGS_BAD_MAGIC;
    if ((NVGspArgsU32)size < NV_GSP_CACHED_ARGS_QUEUE_OFFSET + NV_GSP_CACHED_ARGS_QUEUE_BYTES ||
        (NVGspArgsU32)size > NV_GSP_CACHED_ARGS_PAGE_BYTES)
        return NV_GSP_CACHED_ARGS_BAD_SIZE;
    if ((rmArgsGpuAddress & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0ull)
        return NV_GSP_CACHED_ARGS_BAD_RMARGS_ADDRESS;

    NVGspCachedArgsOverlayPlan staged = {};
    staged.cachedArgsSize = (NVGspArgsU32)size;
    staged.queueOffset = NV_GSP_CACHED_ARGS_QUEUE_OFFSET;
    staged.queueBytes = NV_GSP_CACHED_ARGS_QUEUE_BYTES;
    staged.preservedPrefixBytes = NV_GSP_CACHED_ARGS_QUEUE_OFFSET;
    staged.preservedSuffixOffset = NV_GSP_CACHED_ARGS_QUEUE_OFFSET + NV_GSP_CACHED_ARGS_QUEUE_BYTES;
    staged.rmArgsRegion.id8 = nvGspGenerateInitArgId("RMARGS");
    staged.rmArgsRegion.pa = rmArgsGpuAddress;
    staged.rmArgsRegion.size = NV_GSP_CACHED_ARGS_PAGE_BYTES;
    staged.rmArgsRegion.kind = NV_GSP_LIBOS_REGION_CONTIGUOUS;
    staged.rmArgsRegion.loc = NV_GSP_LIBOS_REGION_LOC_SYSMEM;
    staged.fullFenceAfterLibosRegionList = true;
    *out = staged;
    return NV_GSP_CACHED_ARGS_OK;
}

// Apply ONLY the MESSAGE_QUEUE_INIT_ARGUMENTS bytes after the entire page has
// been validated. Everything else in RMARGS is preserved byte-for-byte.
static inline NVGspArgsU32 nvGspApplyCachedArgsQueueOverlay(
    NVGspArgsU8 *rmArgsPage,
    NVGspArgsU32 pageBytes,
    NVGspArgsU64 rmArgsGpuAddress,
    const NVGspQueueInitArguments *queueArgs,
    NVGspCachedArgsOverlayPlan *outPlan)
{
    if (!rmArgsPage) return NV_GSP_CACHED_ARGS_NULL_PAGE;
    if (!queueArgs) return NV_GSP_CACHED_ARGS_NULL_QUEUE;
    if (!outPlan) return NV_GSP_CACHED_ARGS_NULL_OUTPUT;

    NVGspCachedArgsOverlayPlan staged = {};
    const NVGspArgsU32 verdict = nvGspPlanCachedArgsQueueOverlay(
        rmArgsPage, pageBytes, rmArgsGpuAddress, &staged);
    if (verdict != NV_GSP_CACHED_ARGS_OK)
        return verdict;

    const NVGspArgsU8 *src = reinterpret_cast<const NVGspArgsU8 *>(queueArgs);
    for (NVGspArgsU32 i = 0u; i < NV_GSP_CACHED_ARGS_QUEUE_BYTES; ++i)
        rmArgsPage[NV_GSP_CACHED_ARGS_QUEUE_OFFSET + i] = src[i];
    *outPlan = staged;
    return NV_GSP_CACHED_ARGS_OK;
}

#endif // NV_GSP_CACHED_ARGS_HPP
