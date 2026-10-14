// SPDX-License-Identifier: MIT
// Dependency-free persistent-surface arena for the Tiger Lake private PPGTT.
#ifndef IntelGtSurface_hpp
#define IntelGtSurface_hpp

#include "IntelGtPpgtt.hpp"
#include "IntelGtCopyPlan.hpp"

using IGSurfaceU32 = unsigned int;
using IGSurfaceU64 = unsigned long long;

#define IG_GTS_PAGE_BYTES_POLICY_4096             4096u
#define IG_GTS_PT_LEAVES_GEN8_PLUS_512             512u
#define IG_GTS_EXISTING_POOL_LEAVES_32              32u
#define IG_GTS_GUARD_FIRST_LEAF_32                  32u
#define IG_GTS_GUARD_LEAVES_32                      32u
#define IG_GTS_ARENA_FIRST_LEAF_64                  64u
#define IG_GTS_ARENA_LEAVES_448                    448u
#define IG_GTS_ARENA_GPU_BASE_0x40000          0x00040000u
#define IG_GTS_ARENA_BYTES_0x1C0000           0x001C0000u
#define IG_GTS_PT_GPU_LIMIT_EXCLUSIVE_0x200000 0x00200000u

#define IG_GTS_PLAN_NOT_ATTEMPTED                    0u
#define IG_GTS_PLAN_OK                               1u
#define IG_GTS_PLAN_NULL_OUT                         2u
#define IG_GTS_PLAN_BAD_GEOMETRY                     3u
#define IG_GTS_PLAN_BAD_PITCH                        4u
#define IG_GTS_PLAN_BAD_ARENA_OFFSET                 5u
#define IG_GTS_PLAN_ARENA_TOO_SMALL                  6u
#define IG_GTS_PLAN_OVERFLOW                         7u

#define IG_GTS_FLAT_NOT_ATTEMPTED                    0u
#define IG_GTS_FLAT_OK                               1u
#define IG_GTS_FLAT_NULL_INPUT                       2u
#define IG_GTS_FLAT_BAD_COUNT                        3u
#define IG_GTS_FLAT_OUTPUT_TOO_SMALL                 4u
#define IG_GTS_FLAT_BAD_SEGMENT                      5u
#define IG_GTS_FLAT_BAD_ADDRESS                      6u
#define IG_GTS_FLAT_PAGE_COUNT_MISMATCH              7u

#define IG_GTS_PTE_NOT_ATTEMPTED                     0u
#define IG_GTS_PTE_OK                                1u
#define IG_GTS_PTE_NULL_INPUT                        2u
#define IG_GTS_PTE_BAD_COUNT                         3u
#define IG_GTS_PTE_OUTPUT_TOO_SMALL                  4u
#define IG_GTS_PTE_BAD_PAT                           5u
#define IG_GTS_PTE_BAD_PAGE                          6u

struct IGGtSurfacePlan {
    IGSurfaceU32 firstLeaf;
    IGSurfaceU32 pageCount;
    IGSurfaceU32 gpuAddress;
    IGSurfaceU32 allocationBytes;
    IGSurfaceU64 usedBytes;
    IGSurfaceU32 width;
    IGSurfaceU32 height;
    IGSurfaceU32 pitch;
};

struct IGGtSurfaceDmaSegment {
    IGSurfaceU64 address;
    IGSurfaceU64 length;
};

static inline bool igGtSurfacePlanSucceeded(IGSurfaceU32 verdict)
{
    return verdict == IG_GTS_PLAN_OK;
}

static inline bool igGtSurfaceFlattenSucceeded(IGSurfaceU32 verdict)
{
    return verdict == IG_GTS_FLAT_OK;
}

static inline bool igGtSurfacePteSucceeded(IGSurfaceU32 verdict)
{
    return verdict == IG_GTS_PTE_OK;
}

static inline IGSurfaceU32 igGtSurfacePlan32(IGSurfaceU32 width,
                                             IGSurfaceU32 height,
                                             IGSurfaceU32 pitch,
                                             IGSurfaceU32 arenaPageOffset,
                                             IGGtSurfacePlan *out)
{
    if (out == 0) return IG_GTS_PLAN_NULL_OUT;
    if (!width || !height || width > 16384u || height > 16384u)
        return IG_GTS_PLAN_BAD_GEOMETRY;
    if (!pitch || pitch > 32767u || (pitch & 3u))
        return IG_GTS_PLAN_BAD_PITCH;

    IGSurfaceU64 used = 0ull;
    if (!igGtCopy32SurfaceBytes(width, height, pitch, &used) || !used)
        return IG_GTS_PLAN_BAD_PITCH;
    if (arenaPageOffset >= IG_GTS_ARENA_LEAVES_448)
        return IG_GTS_PLAN_BAD_ARENA_OFFSET;

    const IGSurfaceU64 rounded =
        (used + (IGSurfaceU64)IG_GTS_PAGE_BYTES_POLICY_4096 - 1ull) &
        ~((IGSurfaceU64)IG_GTS_PAGE_BYTES_POLICY_4096 - 1ull);
    if (!rounded || rounded > 0xffffffffull)
        return IG_GTS_PLAN_OVERFLOW;
    const IGSurfaceU64 pages64 = rounded / IG_GTS_PAGE_BYTES_POLICY_4096;
    if (!pages64 || pages64 > IG_GTS_ARENA_LEAVES_448)
        return IG_GTS_PLAN_ARENA_TOO_SMALL;
    if ((IGSurfaceU64)arenaPageOffset + pages64 > IG_GTS_ARENA_LEAVES_448)
        return IG_GTS_PLAN_ARENA_TOO_SMALL;

    const IGSurfaceU32 firstLeaf = IG_GTS_ARENA_FIRST_LEAF_64 + arenaPageOffset;
    const IGSurfaceU64 gpu = (IGSurfaceU64)firstLeaf * IG_GTS_PAGE_BYTES_POLICY_4096;
    if (firstLeaf < IG_GTS_ARENA_FIRST_LEAF_64 ||
        (IGSurfaceU64)firstLeaf + pages64 > IG_GTS_PT_LEAVES_GEN8_PLUS_512 ||
        gpu < IG_GTS_ARENA_GPU_BASE_0x40000 ||
        gpu + rounded > IG_GTS_PT_GPU_LIMIT_EXCLUSIVE_0x200000)
        return IG_GTS_PLAN_OVERFLOW;

    IGGtSurfacePlan plan = {};
    plan.firstLeaf = firstLeaf;
    plan.pageCount = (IGSurfaceU32)pages64;
    plan.gpuAddress = (IGSurfaceU32)gpu;
    plan.allocationBytes = (IGSurfaceU32)rounded;
    plan.usedBytes = used;
    plan.width = width;
    plan.height = height;
    plan.pitch = pitch;
    *out = plan;
    return IG_GTS_PLAN_OK;
}

// Flatten IOMMU segments into page addresses. Validation is a complete first
// pass, so caller output is untouched on every refusal and no large temporary
// array is required on the future kernel stack.
static inline IGSurfaceU32 igGtSurfaceFlattenSegments(
    const IGGtSurfaceDmaSegment *segments,
    IGSurfaceU32 segmentCount,
    IGSurfaceU32 expectedPages,
    IGSurfaceU64 *outPages,
    IGSurfaceU32 outputCapacity)
{
    if (segments == 0 || outPages == 0) return IG_GTS_FLAT_NULL_INPUT;
    if (!segmentCount || !expectedPages || expectedPages > IG_GTS_ARENA_LEAVES_448 ||
        segmentCount > expectedPages)
        return IG_GTS_FLAT_BAD_COUNT;
    if (outputCapacity < expectedPages) return IG_GTS_FLAT_OUTPUT_TOO_SMALL;

    IGSurfaceU64 totalPages = 0ull;
    for (IGSurfaceU32 i = 0; i < segmentCount; ++i) {
        const IGSurfaceU64 address = segments[i].address;
        const IGSurfaceU64 length = segments[i].length;
        if (!length || (address & (IG_GTS_PAGE_BYTES_POLICY_4096 - 1u)) ||
            (length & (IG_GTS_PAGE_BYTES_POLICY_4096 - 1u)))
            return IG_GTS_FLAT_BAD_SEGMENT;
        const IGSurfaceU64 pages = length / IG_GTS_PAGE_BYTES_POLICY_4096;
        if (!pages || totalPages + pages < totalPages)
            return IG_GTS_FLAT_BAD_SEGMENT;
        const IGSurfaceU64 last = address + (pages - 1ull) * IG_GTS_PAGE_BYTES_POLICY_4096;
        if (last < address || !igGtPpgttPageAddressOk(address) ||
            !igGtPpgttPageAddressOk(last))
            return IG_GTS_FLAT_BAD_ADDRESS;
        totalPages += pages;
        if (totalPages > expectedPages) return IG_GTS_FLAT_PAGE_COUNT_MISMATCH;
    }
    if (totalPages != expectedPages) return IG_GTS_FLAT_PAGE_COUNT_MISMATCH;

    IGSurfaceU32 outIndex = 0u;
    for (IGSurfaceU32 i = 0; i < segmentCount; ++i) {
        const IGSurfaceU64 pages = segments[i].length / IG_GTS_PAGE_BYTES_POLICY_4096;
        for (IGSurfaceU64 page = 0; page < pages; ++page) {
            outPages[outIndex++] = segments[i].address + page * IG_GTS_PAGE_BYTES_POLICY_4096;
        }
    }
    return IG_GTS_FLAT_OK;
}

// Convert already-flattened Intel-domain page addresses into Gen12 PPGTT leaf
// entries. As above, the full input is validated before caller output changes.
static inline IGSurfaceU32 igGtSurfaceBuildPtes(const IGSurfaceU64 *pages,
                                                IGSurfaceU32 pageCount,
                                                IGSurfaceU32 patIndex,
                                                IGSurfaceU64 *outPtes,
                                                IGSurfaceU32 outputCapacity)
{
    if (pages == 0 || outPtes == 0) return IG_GTS_PTE_NULL_INPUT;
    if (!pageCount || pageCount > IG_GTS_ARENA_LEAVES_448)
        return IG_GTS_PTE_BAD_COUNT;
    if (outputCapacity < pageCount) return IG_GTS_PTE_OUTPUT_TOO_SMALL;
    if (!igGtPpgttPatIndexOk(patIndex)) return IG_GTS_PTE_BAD_PAT;

    for (IGSurfaceU32 i = 0; i < pageCount; ++i) {
        if (!igGtPpgttPageAddressOk(pages[i]) || !igGtPpgttLeafEntry(pages[i], patIndex))
            return IG_GTS_PTE_BAD_PAGE;
    }
    for (IGSurfaceU32 i = 0; i < pageCount; ++i)
        outPtes[i] = igGtPpgttLeafEntry(pages[i], patIndex);
    return IG_GTS_PTE_OK;
}

#endif // IntelGtSurface_hpp
