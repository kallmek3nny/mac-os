// SPDX-License-Identifier: MIT
// Pure transaction checks for installing the persistent-surface arena into the
// already-owned Tiger Lake private PPGTT. Runtime writes stay in
// IntelGtBringup; this layer makes the ownership/range/PTE rules host-testable.
#ifndef IntelGtSurfaceInstall_hpp
#define IntelGtSurfaceInstall_hpp

#include "IntelGtSurface.hpp"

#define IG_GTSI_NOT_ATTEMPTED          0u
#define IG_GTSI_OK                     1u
#define IG_GTSI_NULL_INPUT             2u
#define IG_GTSI_BAD_PT_GEOMETRY        3u
#define IG_GTSI_BAD_COUNT              4u
#define IG_GTSI_WINDOW_OCCUPIED        5u
#define IG_GTSI_BAD_NEW_PTE            6u

struct IGGtSurfaceInstallPlan {
    IGSurfaceU32 firstLeaf;
    IGSurfaceU32 leafCount;
    IGSurfaceU32 gpuBase;
    IGSurfaceU32 bytes;
};

static inline bool igGtSurfaceInstallSucceeded(IGSurfaceU32 verdict)
{
    return verdict == IG_GTSI_OK;
}

static inline bool igGtSurfaceInstallPteValid(IGSurfaceU64 pte)
{
    const IGSurfaceU64 address = pte & IG_GTPG_DMA_ADDR_MASK_POLICY_46;
    if (!igGtPpgttPageAddressOk(address)) return false;
    for (IGSurfaceU32 pat = 0u; pat < IG_GTPG_PAT_INDEX_COUNT_I915_8; ++pat) {
        if (igGtPpgttLeafEntry(address, pat) == pte) return true;
    }
    return false;
}

// The runtime milestone maps the whole reserved arena at once. Requiring the
// exact 448-leaf span keeps this transaction distinct from future handle-level
// suballocation and makes it impossible to overwrite the pool or guard leaves.
// Caller output is untouched on every refusal.
static inline IGSurfaceU32 igGtSurfacePlanInstall(
    const IGSurfaceU64 *pt,
    IGSurfaceU32 ptLeaves,
    const IGSurfaceU64 *newPtes,
    IGSurfaceU32 newPteCount,
    IGGtSurfaceInstallPlan *out)
{
    if (!pt || !newPtes || !out) return IG_GTSI_NULL_INPUT;
    if (ptLeaves != IG_GTS_PT_LEAVES_GEN8_PLUS_512)
        return IG_GTSI_BAD_PT_GEOMETRY;
    if (newPteCount != IG_GTS_ARENA_LEAVES_448)
        return IG_GTSI_BAD_COUNT;

    for (IGSurfaceU32 i = 0u; i < IG_GTS_ARENA_LEAVES_448; ++i) {
        if (pt[IG_GTS_ARENA_FIRST_LEAF_64 + i] != IG_GTPG_ENTRY_INVALID)
            return IG_GTSI_WINDOW_OCCUPIED;
        if (!igGtSurfaceInstallPteValid(newPtes[i]))
            return IG_GTSI_BAD_NEW_PTE;
    }

    IGGtSurfaceInstallPlan plan = {};
    plan.firstLeaf = IG_GTS_ARENA_FIRST_LEAF_64;
    plan.leafCount = IG_GTS_ARENA_LEAVES_448;
    plan.gpuBase = IG_GTS_ARENA_GPU_BASE_0x40000;
    plan.bytes = IG_GTS_ARENA_BYTES_0x1C0000;
    *out = plan;
    return IG_GTSI_OK;
}

static inline bool igGtSurfaceLeafImageMatches(
    const IGSurfaceU64 *pt,
    IGSurfaceU32 ptLeaves,
    IGSurfaceU32 firstLeaf,
    const IGSurfaceU64 *expected,
    IGSurfaceU32 count)
{
    if (!pt || !expected || ptLeaves != IG_GTS_PT_LEAVES_GEN8_PLUS_512 ||
        firstLeaf < IG_GTS_ARENA_FIRST_LEAF_64 ||
        count > IG_GTS_ARENA_LEAVES_448 ||
        (IGSurfaceU64)firstLeaf + count > ptLeaves)
        return false;
    for (IGSurfaceU32 i = 0u; i < count; ++i) {
        if (pt[firstLeaf + i] != expected[i]) return false;
    }
    return true;
}

#endif // IntelGtSurfaceInstall_hpp
