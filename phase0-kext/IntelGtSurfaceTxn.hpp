// SPDX-License-Identifier: MIT
// Dependency-free reversible PPGTT-leaf transactions for persistent Tiger Lake surfaces.
//
// This helper never allocates memory, touches MMIO, submits GPU work or exposes a
// GPU address.  The caller owns serialization and supplies read/write callbacks.
// Every mutating path uses exact readback and either returns the table to its
// pre-transaction image or reports that rollback itself failed.
#ifndef IntelGtSurfaceTxn_hpp
#define IntelGtSurfaceTxn_hpp

#include "IntelGtSurface.hpp"

#define IG_GTS_TXN_NOT_ATTEMPTED                         0u
#define IG_GTS_TXN_OK                                    1u
#define IG_GTS_TXN_NULL_INPUT                            2u
#define IG_GTS_TXN_BAD_RANGE                             3u
#define IG_GTS_TXN_SNAPSHOT_TOO_SMALL                    4u
#define IG_GTS_TXN_BAD_NEW_PTE                           5u
#define IG_GTS_TXN_READ_FAILED                           6u
#define IG_GTS_TXN_TARGET_OCCUPIED                       7u
#define IG_GTS_TXN_STATE_CHANGED                         8u
#define IG_GTS_TXN_WRITE_FAILED_ROLLED_BACK              9u
#define IG_GTS_TXN_ROLLBACK_FAILED                      10u
#define IG_GTS_TXN_RESTORE_STATE_MISMATCH               11u
#define IG_GTS_TXN_RESTORE_FAILED_REINSTALLED           12u

typedef bool (*IGGtSurfaceTxnReadFn)(void *context,
                                     IGSurfaceU32 leaf,
                                     IGSurfaceU64 *value);
typedef bool (*IGGtSurfaceTxnWriteFn)(void *context,
                                      IGSurfaceU32 leaf,
                                      IGSurfaceU64 value);

struct IGGtSurfacePteTxn {
    IGSurfaceU32 firstLeaf;
    IGSurfaceU32 pageCount;
};

struct IGGtSurfacePteTxnStats {
    IGSurfaceU32 writesAttempted;
    IGSurfaceU32 writesVerified;
    IGSurfaceU32 recoveryWrites;
    IGSurfaceU32 failedLeaf;
};

static inline bool igGtSurfaceTxnSucceeded(IGSurfaceU32 verdict)
{
    return verdict == IG_GTS_TXN_OK;
}

static inline bool igGtSurfaceTxnRangeOk(IGSurfaceU32 firstLeaf,
                                         IGSurfaceU32 pageCount)
{
    if (!pageCount || pageCount > IG_GTS_ARENA_LEAVES_448)
        return false;
    if (firstLeaf < IG_GTS_ARENA_FIRST_LEAF_64 ||
        firstLeaf >= IG_GTS_PT_LEAVES_GEN8_PLUS_512)
        return false;
    const IGSurfaceU64 end = (IGSurfaceU64)firstLeaf + (IGSurfaceU64)pageCount;
    return end <= IG_GTS_PT_LEAVES_GEN8_PLUS_512;
}

static inline bool igGtSurfaceTxnLeafImageOk(IGSurfaceU64 pte)
{
    const IGSurfaceU64 flagMask =
        IG_GTPG_ENTRY_PRESENT_I915_0x1 |
        IG_GTPG_ENTRY_RW_I915_0x2 |
        IG_GTPG_PAT0_I915_0x8 |
        IG_GTPG_PAT1_I915_0x10 |
        IG_GTPG_PAT2_I915_0x80;
    const IGSurfaceU64 allowed = IG_GTPG_DMA_ADDR_MASK_POLICY_46 | flagMask;
    if ((pte & (IG_GTPG_ENTRY_PRESENT_I915_0x1 | IG_GTPG_ENTRY_RW_I915_0x2)) !=
        (IG_GTPG_ENTRY_PRESENT_I915_0x1 | IG_GTPG_ENTRY_RW_I915_0x2))
        return false;
    if (pte & ~allowed) return false;
    return igGtPpgttPageAddressOk(pte & IG_GTPG_DMA_ADDR_MASK_POLICY_46);
}

// Prepare only snapshots the exact old leaf image.  It performs no writes.
// Present leaves are always refused, even inside our private PT arena, so a
// future allocator cannot accidentally steal another persistent mapping.
static inline IGSurfaceU32 igGtSurfaceTxnPrepare(
    IGSurfaceU32 firstLeaf,
    const IGSurfaceU64 *newPtes,
    IGSurfaceU32 pageCount,
    IGSurfaceU64 *snapshot,
    IGSurfaceU32 snapshotCapacity,
    IGGtSurfaceTxnReadFn readFn,
    void *context,
    IGGtSurfacePteTxn *outTxn)
{
    if (newPtes == 0 || snapshot == 0 || readFn == 0 || outTxn == 0)
        return IG_GTS_TXN_NULL_INPUT;
    if (!igGtSurfaceTxnRangeOk(firstLeaf, pageCount))
        return IG_GTS_TXN_BAD_RANGE;
    if (snapshotCapacity < pageCount)
        return IG_GTS_TXN_SNAPSHOT_TOO_SMALL;

    for (IGSurfaceU32 i = 0; i < pageCount; ++i) {
        if (!igGtSurfaceTxnLeafImageOk(newPtes[i]))
            return IG_GTS_TXN_BAD_NEW_PTE;
    }

    for (IGSurfaceU32 i = 0; i < pageCount; ++i) {
        IGSurfaceU64 current = 0ull;
        if (!readFn(context, firstLeaf + i, &current))
            return IG_GTS_TXN_READ_FAILED;
        snapshot[i] = current;
        if (current & IG_GTPG_ENTRY_PRESENT_I915_0x1)
            return IG_GTS_TXN_TARGET_OCCUPIED;
    }

    IGGtSurfacePteTxn txn = {};
    txn.firstLeaf = firstLeaf;
    txn.pageCount = pageCount;
    *outTxn = txn;
    return IG_GTS_TXN_OK;
}

static inline bool igGtSurfaceTxnRecoverRange(
    const IGGtSurfacePteTxn *txn,
    const IGSurfaceU64 *wanted,
    IGSurfaceU32 lastInclusive,
    IGGtSurfaceTxnReadFn readFn,
    IGGtSurfaceTxnWriteFn writeFn,
    void *context,
    IGGtSurfacePteTxnStats *stats)
{
    for (IGSurfaceU32 n = lastInclusive + 1u; n > 0u; --n) {
        const IGSurfaceU32 i = n - 1u;
        const IGSurfaceU32 leaf = txn->firstLeaf + i;
        if (stats) ++stats->recoveryWrites;
        if (!writeFn(context, leaf, wanted[i]))
            return false;
        IGSurfaceU64 back = 0ull;
        if (!readFn(context, leaf, &back) || back != wanted[i])
            return false;
    }
    return true;
}

// Commit revalidates the complete snapshot before touching a leaf.  A write or
// readback failure triggers reverse-order rollback, including the failed leaf
// because a callback may report failure after a partial store.
static inline IGSurfaceU32 igGtSurfaceTxnCommit(
    const IGGtSurfacePteTxn *txn,
    const IGSurfaceU64 *newPtes,
    const IGSurfaceU64 *snapshot,
    IGSurfaceU32 snapshotCapacity,
    IGGtSurfaceTxnReadFn readFn,
    IGGtSurfaceTxnWriteFn writeFn,
    void *context,
    IGGtSurfacePteTxnStats *outStats)
{
    if (txn == 0 || newPtes == 0 || snapshot == 0 || readFn == 0 || writeFn == 0)
        return IG_GTS_TXN_NULL_INPUT;
    if (!igGtSurfaceTxnRangeOk(txn->firstLeaf, txn->pageCount))
        return IG_GTS_TXN_BAD_RANGE;
    if (snapshotCapacity < txn->pageCount)
        return IG_GTS_TXN_SNAPSHOT_TOO_SMALL;
    for (IGSurfaceU32 i = 0; i < txn->pageCount; ++i) {
        if (!igGtSurfaceTxnLeafImageOk(newPtes[i]))
            return IG_GTS_TXN_BAD_NEW_PTE;
        IGSurfaceU64 current = 0ull;
        if (!readFn(context, txn->firstLeaf + i, &current))
            return IG_GTS_TXN_READ_FAILED;
        if (current != snapshot[i])
            return IG_GTS_TXN_STATE_CHANGED;
    }

    IGGtSurfacePteTxnStats stats = {};
    stats.failedLeaf = 0xffffffffu;
    for (IGSurfaceU32 i = 0; i < txn->pageCount; ++i) {
        const IGSurfaceU32 leaf = txn->firstLeaf + i;
        ++stats.writesAttempted;
        bool wrote = writeFn(context, leaf, newPtes[i]);
        IGSurfaceU64 back = 0ull;
        bool verified = wrote && readFn(context, leaf, &back) && back == newPtes[i];
        if (!verified) {
            stats.failedLeaf = leaf;
            bool recovered = igGtSurfaceTxnRecoverRange(
                txn, snapshot, i, readFn, writeFn, context, &stats);
            if (outStats) *outStats = stats;
            return recovered ? IG_GTS_TXN_WRITE_FAILED_ROLLED_BACK
                             : IG_GTS_TXN_ROLLBACK_FAILED;
        }
        ++stats.writesVerified;
    }
    if (outStats) *outStats = stats;
    return IG_GTS_TXN_OK;
}

// Teardown first proves every target leaf still equals our installed image.
// On a restore failure it tries to reinstall that image so the caller can keep
// the backing quarantined without leaving a half-restored address space.
static inline IGSurfaceU32 igGtSurfaceTxnRestore(
    const IGGtSurfacePteTxn *txn,
    const IGSurfaceU64 *installedPtes,
    const IGSurfaceU64 *snapshot,
    IGSurfaceU32 snapshotCapacity,
    IGGtSurfaceTxnReadFn readFn,
    IGGtSurfaceTxnWriteFn writeFn,
    void *context,
    IGGtSurfacePteTxnStats *outStats)
{
    if (txn == 0 || installedPtes == 0 || snapshot == 0 || readFn == 0 || writeFn == 0)
        return IG_GTS_TXN_NULL_INPUT;
    if (!igGtSurfaceTxnRangeOk(txn->firstLeaf, txn->pageCount))
        return IG_GTS_TXN_BAD_RANGE;
    if (snapshotCapacity < txn->pageCount)
        return IG_GTS_TXN_SNAPSHOT_TOO_SMALL;

    for (IGSurfaceU32 i = 0; i < txn->pageCount; ++i) {
        if (!igGtSurfaceTxnLeafImageOk(installedPtes[i]))
            return IG_GTS_TXN_BAD_NEW_PTE;
        IGSurfaceU64 current = 0ull;
        if (!readFn(context, txn->firstLeaf + i, &current))
            return IG_GTS_TXN_READ_FAILED;
        if (current != installedPtes[i])
            return IG_GTS_TXN_RESTORE_STATE_MISMATCH;
    }

    IGGtSurfacePteTxnStats stats = {};
    stats.failedLeaf = 0xffffffffu;
    for (IGSurfaceU32 i = 0; i < txn->pageCount; ++i) {
        const IGSurfaceU32 leaf = txn->firstLeaf + i;
        ++stats.writesAttempted;
        bool wrote = writeFn(context, leaf, snapshot[i]);
        IGSurfaceU64 back = 0ull;
        bool verified = wrote && readFn(context, leaf, &back) && back == snapshot[i];
        if (!verified) {
            stats.failedLeaf = leaf;
            bool recovered = igGtSurfaceTxnRecoverRange(
                txn, installedPtes, i, readFn, writeFn, context, &stats);
            if (outStats) *outStats = stats;
            return recovered ? IG_GTS_TXN_RESTORE_FAILED_REINSTALLED
                             : IG_GTS_TXN_ROLLBACK_FAILED;
        }
        ++stats.writesVerified;
    }
    if (outStats) *outStats = stats;
    return IG_GTS_TXN_OK;
}

#endif // IntelGtSurfaceTxn_hpp
