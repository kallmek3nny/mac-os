// SPDX-License-Identifier: MIT
// Dependency-free reversible Tiger Lake GGTT transactions for sink surfaces.
//
// This helper owns no memory and performs no MMIO itself. The runtime owner
// supplies exact read/write callbacks and storage for the old/new PTE images.
// Every mutation is followed by exact readback. Failed installs roll back to the
// snapshot; failed restores attempt to reinstall the owned image so backing can
// remain quarantined rather than becoming dangling scanout memory.
#ifndef INTEL_SINK_GGTT_TXN_HPP
#define INTEL_SINK_GGTT_TXN_HPP

#include "IntelSinkGgtt.hpp"

#define IG_SINK_GGTT_TXN_NOT_ATTEMPTED                    0u
#define IG_SINK_GGTT_TXN_OK                               1u
#define IG_SINK_GGTT_TXN_NULL_INPUT                       2u
#define IG_SINK_GGTT_TXN_BAD_RANGE                        3u
#define IG_SINK_GGTT_TXN_SNAPSHOT_TOO_SMALL               4u
#define IG_SINK_GGTT_TXN_BAD_NEW_PTE                      5u
#define IG_SINK_GGTT_TXN_READ_FAILED                      6u
#define IG_SINK_GGTT_TXN_TARGET_OCCUPIED                  7u
#define IG_SINK_GGTT_TXN_STATE_CHANGED                    8u
#define IG_SINK_GGTT_TXN_WRITE_FAILED_ROLLED_BACK         9u
#define IG_SINK_GGTT_TXN_ROLLBACK_FAILED                 10u
#define IG_SINK_GGTT_TXN_RESTORE_STATE_MISMATCH          11u
#define IG_SINK_GGTT_TXN_RESTORE_FAILED_REINSTALLED      12u

typedef bool (*IGSinkGgttTxnReadFn)(void *context,
                                    IGGgttU32 pteIndex,
                                    IGGgttU64 *value);
typedef bool (*IGSinkGgttTxnWriteFn)(void *context,
                                     IGGgttU32 pteIndex,
                                     IGGgttU64 value);

struct IGSinkGgttTxn {
    IGGgttU32 firstPte;
    IGGgttU32 pteCount;
    IGGgttU32 tableEntries;
};

struct IGSinkGgttTxnStats {
    IGGgttU32 writesAttempted;
    IGGgttU32 writesVerified;
    IGGgttU32 recoveryWrites;
    IGGgttU32 failedPte;
};

static inline bool igSinkGgttTxnSucceeded(IGGgttU32 verdict)
{
    return verdict == IG_SINK_GGTT_TXN_OK;
}

static inline const char *igSinkGgttTxnVerdictName(IGGgttU32 verdict)
{
    switch (verdict) {
        case IG_SINK_GGTT_TXN_NOT_ATTEMPTED:               return "not-attempted";
        case IG_SINK_GGTT_TXN_OK:                          return "ok";
        case IG_SINK_GGTT_TXN_NULL_INPUT:                  return "null-input";
        case IG_SINK_GGTT_TXN_BAD_RANGE:                   return "bad-range";
        case IG_SINK_GGTT_TXN_SNAPSHOT_TOO_SMALL:          return "snapshot-too-small";
        case IG_SINK_GGTT_TXN_BAD_NEW_PTE:                 return "bad-new-pte";
        case IG_SINK_GGTT_TXN_READ_FAILED:                 return "read-failed";
        case IG_SINK_GGTT_TXN_TARGET_OCCUPIED:             return "target-occupied";
        case IG_SINK_GGTT_TXN_STATE_CHANGED:               return "state-changed";
        case IG_SINK_GGTT_TXN_WRITE_FAILED_ROLLED_BACK:    return "write-failed-rolled-back";
        case IG_SINK_GGTT_TXN_ROLLBACK_FAILED:             return "rollback-failed";
        case IG_SINK_GGTT_TXN_RESTORE_STATE_MISMATCH:      return "restore-state-mismatch";
        case IG_SINK_GGTT_TXN_RESTORE_FAILED_REINSTALLED:  return "restore-failed-reinstalled";
        default:                                            return "unknown";
    }
}

static inline bool igSinkGgttTxnRangeOk(IGGgttU32 firstPte,
                                        IGGgttU32 pteCount,
                                        IGGgttU32 tableEntries)
{
    if (!pteCount || !tableEntries || firstPte >= tableEntries)
        return false;
    return pteCount <= tableEntries - firstPte;
}

static inline bool igSinkGgttTxnInstalledPteOk(IGGgttU64 pte)
{
    if (!igSinkGgttPtePresent(pte))
        return false;
    if (pte & ~(IG_SINK_GGTT_PTE_ADDR_MASK_I915_45_12 |
                IG_SINK_GGTT_PTE_PRESENT_I915_0x1))
        return false;
    return igSinkGgttPageAligned(pte & IG_SINK_GGTT_PTE_ADDR_MASK_I915_45_12);
}

// Snapshot the entire target image before any write. A present target PTE is
// always refused so this service cannot steal another GGTT owner's mapping.
static inline IGGgttU32 igSinkGgttTxnPrepare(
    IGGgttU32 firstPte,
    const IGGgttU64 *installedPtes,
    IGGgttU32 pteCount,
    IGGgttU32 tableEntries,
    IGGgttU64 *snapshot,
    IGGgttU32 snapshotCapacity,
    IGSinkGgttTxnReadFn readFn,
    void *context,
    IGSinkGgttTxn *outTxn)
{
    if (!installedPtes || !snapshot || !readFn || !outTxn)
        return IG_SINK_GGTT_TXN_NULL_INPUT;
    if (!igSinkGgttTxnRangeOk(firstPte,pteCount,tableEntries))
        return IG_SINK_GGTT_TXN_BAD_RANGE;
    if (snapshotCapacity < pteCount)
        return IG_SINK_GGTT_TXN_SNAPSHOT_TOO_SMALL;

    for (IGGgttU32 i=0;i<pteCount;++i) {
        if (!igSinkGgttTxnInstalledPteOk(installedPtes[i]))
            return IG_SINK_GGTT_TXN_BAD_NEW_PTE;
    }

    for (IGGgttU32 i=0;i<pteCount;++i) {
        IGGgttU64 current=0ull;
        if (!readFn(context,firstPte+i,&current))
            return IG_SINK_GGTT_TXN_READ_FAILED;
        snapshot[i]=current;
        if (igSinkGgttPtePresent(current))
            return IG_SINK_GGTT_TXN_TARGET_OCCUPIED;
    }

    IGSinkGgttTxn txn={};
    txn.firstPte=firstPte;
    txn.pteCount=pteCount;
    txn.tableEntries=tableEntries;
    *outTxn=txn;
    return IG_SINK_GGTT_TXN_OK;
}

static inline bool igSinkGgttTxnRecoverRange(
    const IGSinkGgttTxn *txn,
    const IGGgttU64 *wanted,
    IGGgttU32 lastInclusive,
    IGSinkGgttTxnReadFn readFn,
    IGSinkGgttTxnWriteFn writeFn,
    void *context,
    IGSinkGgttTxnStats *stats)
{
    for (IGGgttU32 n=lastInclusive+1u;n>0u;--n) {
        const IGGgttU32 i=n-1u;
        const IGGgttU32 index=txn->firstPte+i;
        if (stats) ++stats->recoveryWrites;
        if (!writeFn(context,index,wanted[i]))
            return false;
        IGGgttU64 back=0ull;
        if (!readFn(context,index,&back) || back!=wanted[i])
            return false;
    }
    return true;
}

// Revalidate the full snapshot immediately before the first store. A write or
// readback failure rolls back the failed PTE too, because a callback may fail
// after a partial store.
static inline IGGgttU32 igSinkGgttTxnCommit(
    const IGSinkGgttTxn *txn,
    const IGGgttU64 *installedPtes,
    const IGGgttU64 *snapshot,
    IGGgttU32 snapshotCapacity,
    IGSinkGgttTxnReadFn readFn,
    IGSinkGgttTxnWriteFn writeFn,
    void *context,
    IGSinkGgttTxnStats *outStats)
{
    if (!txn || !installedPtes || !snapshot || !readFn || !writeFn)
        return IG_SINK_GGTT_TXN_NULL_INPUT;
    if (!igSinkGgttTxnRangeOk(txn->firstPte,txn->pteCount,txn->tableEntries))
        return IG_SINK_GGTT_TXN_BAD_RANGE;
    if (snapshotCapacity < txn->pteCount)
        return IG_SINK_GGTT_TXN_SNAPSHOT_TOO_SMALL;

    for (IGGgttU32 i=0;i<txn->pteCount;++i) {
        if (!igSinkGgttTxnInstalledPteOk(installedPtes[i]))
            return IG_SINK_GGTT_TXN_BAD_NEW_PTE;
        IGGgttU64 current=0ull;
        if (!readFn(context,txn->firstPte+i,&current))
            return IG_SINK_GGTT_TXN_READ_FAILED;
        if (current!=snapshot[i])
            return IG_SINK_GGTT_TXN_STATE_CHANGED;
    }

    IGSinkGgttTxnStats stats={};
    stats.failedPte=0xffffffffu;
    for (IGGgttU32 i=0;i<txn->pteCount;++i) {
        const IGGgttU32 index=txn->firstPte+i;
        ++stats.writesAttempted;
        const bool wrote=writeFn(context,index,installedPtes[i]);
        IGGgttU64 back=0ull;
        const bool verified=wrote && readFn(context,index,&back) &&
                            back==installedPtes[i];
        if (!verified) {
            stats.failedPte=index;
            const bool recovered=igSinkGgttTxnRecoverRange(
                txn,snapshot,i,readFn,writeFn,context,&stats);
            if (outStats) *outStats=stats;
            return recovered ? IG_SINK_GGTT_TXN_WRITE_FAILED_ROLLED_BACK
                             : IG_SINK_GGTT_TXN_ROLLBACK_FAILED;
        }
        ++stats.writesVerified;
    }
    if (outStats) *outStats=stats;
    return IG_SINK_GGTT_TXN_OK;
}

// Teardown first proves the complete target image is still exactly ours. If an
// old-PTE restore fails, reinstall the owned image over the attempted prefix so
// the caller can keep the backing/DMA/BAR state quarantined safely.
static inline IGGgttU32 igSinkGgttTxnRestore(
    const IGSinkGgttTxn *txn,
    const IGGgttU64 *installedPtes,
    const IGGgttU64 *snapshot,
    IGGgttU32 snapshotCapacity,
    IGSinkGgttTxnReadFn readFn,
    IGSinkGgttTxnWriteFn writeFn,
    void *context,
    IGSinkGgttTxnStats *outStats)
{
    if (!txn || !installedPtes || !snapshot || !readFn || !writeFn)
        return IG_SINK_GGTT_TXN_NULL_INPUT;
    if (!igSinkGgttTxnRangeOk(txn->firstPte,txn->pteCount,txn->tableEntries))
        return IG_SINK_GGTT_TXN_BAD_RANGE;
    if (snapshotCapacity < txn->pteCount)
        return IG_SINK_GGTT_TXN_SNAPSHOT_TOO_SMALL;

    for (IGGgttU32 i=0;i<txn->pteCount;++i) {
        if (!igSinkGgttTxnInstalledPteOk(installedPtes[i]))
            return IG_SINK_GGTT_TXN_BAD_NEW_PTE;
        IGGgttU64 current=0ull;
        if (!readFn(context,txn->firstPte+i,&current))
            return IG_SINK_GGTT_TXN_READ_FAILED;
        if (current!=installedPtes[i])
            return IG_SINK_GGTT_TXN_RESTORE_STATE_MISMATCH;
    }

    IGSinkGgttTxnStats stats={};
    stats.failedPte=0xffffffffu;
    for (IGGgttU32 i=0;i<txn->pteCount;++i) {
        const IGGgttU32 index=txn->firstPte+i;
        ++stats.writesAttempted;
        const bool wrote=writeFn(context,index,snapshot[i]);
        IGGgttU64 back=0ull;
        const bool verified=wrote && readFn(context,index,&back) && back==snapshot[i];
        if (!verified) {
            stats.failedPte=index;
            const bool recovered=igSinkGgttTxnRecoverRange(
                txn,installedPtes,i,readFn,writeFn,context,&stats);
            if (outStats) *outStats=stats;
            return recovered ? IG_SINK_GGTT_TXN_RESTORE_FAILED_REINSTALLED
                             : IG_SINK_GGTT_TXN_ROLLBACK_FAILED;
        }
        ++stats.writesVerified;
    }
    if (outStats) *outStats=stats;
    return IG_SINK_GGTT_TXN_OK;
}

#endif // INTEL_SINK_GGTT_TXN_HPP
