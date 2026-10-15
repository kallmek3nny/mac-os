// SPDX-License-Identifier: MIT
// Persistent Tiger Lake surface backing owned by IntelGtBringup.
//
// This file deliberately lives under the already-exclusive IntelGtBringup PCI
// owner instead of attaching a second service to the GPU.  The extra
// iggt-surface-install=1 boot argument is required before any arena PTE changes.
// The only page-table writes are to leaves 64..511 of Bringup's private PT;
// every write is snapshotted, read back, and reversibly restored through the
// host-tested IntelGtSurfaceTxn helper.  No GPU address is exposed to userspace.
#include "IntelGtBringup.hpp"
#include "IntelGtSurfaceRuntime.hpp"

#include <IOKit/IOMapper.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define IGSURFRTLOG(fmt, ...) IOLog("IntelGtBringupSurface: " fmt "\n", ##__VA_ARGS__)

namespace {
constexpr UInt32 SurfacePages = IG_GTS_ARENA_LEAVES_448;
constexpr UInt32 SurfaceBytes = IG_GTS_ARENA_BYTES_0x1C0000;
constexpr UInt32 PageBytes = IG_GTS_PAGE_BYTES_POLICY_4096;
constexpr UInt8 IntelDmaAddressBits = 46;
// IntelGtBringup's owned pool layout reserves page 23 for its sole private PT.
// Keep this value pinned by the runtime-shape test before either layout moves.
constexpr UInt32 PrivatePtPoolOffset = 23u * PageBytes;

inline void ptBarrier()
{
    __asm__ volatile("mfence" ::: "memory");
}

struct PrivatePtAccess {
    volatile IGSurfaceU64 *pt;
};

bool readPrivatePt(void *opaque, IGSurfaceU32 leaf, IGSurfaceU64 *value)
{
    PrivatePtAccess *access=reinterpret_cast<PrivatePtAccess *>(opaque);
    if (!access || !access->pt || !value || leaf>=IG_GTS_PT_LEAVES_GEN8_PLUS_512)
        return false;
    ptBarrier();
    *value=access->pt[leaf];
    ptBarrier();
    return true;
}

bool writePrivatePt(void *opaque, IGSurfaceU32 leaf, IGSurfaceU64 value)
{
    PrivatePtAccess *access=reinterpret_cast<PrivatePtAccess *>(opaque);
    if (!access || !access->pt || leaf>=IG_GTS_PT_LEAVES_GEN8_PLUS_512)
        return false;
    access->pt[leaf]=value;
    ptBarrier();
    return true;
}

void releaseRuntimeStorage(IntelGtSurfaceRuntimeState *state)
{
    if (!state) return;
    if (state->dmaBound && state->dma) {
        state->dma->clearMemoryDescriptor();
        state->dmaBound=false;
    }
    OSSafeReleaseNULL(state->dma);
    OSSafeReleaseNULL(state->backing);
    IOFree(state,(vm_size_t)sizeof(*state));
}
}

bool IntelGtBringup::stagePersistentSurface(void)
{
    setProperty("GTBringupSurfaceInstallEnabled",false);
    setProperty("GTBringupSurfaceReady",false);
    setProperty("GTBringupSurfacePpgttInstalled",false);
    setProperty("GTBringupSurfaceRawGpuAddressPublished",false);
    setProperty("GTBringupSurfaceRawIovaPublished",false);
    setProperty("GTBringupSurfaceContiguityRequired",false);

    int enabled=0;
    if (!PE_parse_boot_argn("iggt-surface-install",&enabled,sizeof(enabled)) || enabled!=1) {
        setProperty("GTBringupSurfaceState","install-gate-disabled");
        return true;
    }
    setProperty("GTBringupSurfaceInstallEnabled",true);

    // This method is called only after exact-device/provider ownership,
    // accelerator refusal, cold-engine claim, live PAT discovery, and private
    // PPGTT allocation have already succeeded in IntelGtBringup::start().
    if (!fPCI || !fOpened || !fCPU || fSurfaceRuntime) {
        setProperty("GTBringupSurfaceState","owner-precondition-refused");
        return false;
    }

    IGGtSurfacePlan arenaPlan={};
    const UInt32 planVerdict=igGtSurfacePlan32(1024u,448u,4096u,0u,&arenaPlan);
    setProperty("GTBringupSurfacePlanVerdict",(UInt64)planVerdict,32);
    if (!igGtSurfacePlanSucceeded(planVerdict) ||
        arenaPlan.firstLeaf!=IG_GTS_ARENA_FIRST_LEAF_64 ||
        arenaPlan.pageCount!=SurfacePages ||
        arenaPlan.allocationBytes!=SurfaceBytes) {
        setProperty("GTBringupSurfaceState","arena-plan-refused");
        return false;
    }

    IntelGtSurfaceRuntimeState *state=reinterpret_cast<IntelGtSurfaceRuntimeState *>(
        IOMalloc((vm_size_t)sizeof(IntelGtSurfaceRuntimeState)));
    if (!state) {
        setProperty("GTBringupSurfaceState","state-allocation-failed");
        return false;
    }
    bzero(state,sizeof(*state));
    fSurfaceRuntime=state;

    // Fresh page-aligned system memory with no physical-contiguity request.
    state->backing=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,kIODirectionInOut,(vm_size_t)SurfaceBytes,(vm_offset_t)PageBytes);
    if (!state->backing || state->backing->getCapacity()<(vm_size_t)SurfaceBytes) {
        setProperty("GTBringupSurfaceState","backing-allocation-failed");
        releasePersistentSurface();
        return false;
    }
    state->cpu=reinterpret_cast<UInt8 *>(state->backing->getBytesNoCopy());
    if (!state->cpu) {
        setProperty("GTBringupSurfaceState","backing-cpu-map-failed");
        releasePersistentSurface();
        return false;
    }
    bzero(state->cpu,SurfaceBytes);

    IOMapper *mapper=IOMapper::copyMapperForDevice(fPCI);
    if (!mapper) {
        setProperty("GTBringupSurfaceState","intel-device-mapper-unavailable");
        releasePersistentSurface();
        return false;
    }
    state->dma=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        IntelDmaAddressBits,
        (UInt64)PageBytes,
        IODMACommand::kMapped,
        (UInt64)SurfaceBytes,
        PageBytes,
        mapper);
    mapper->release();
    if (!state->dma ||
        state->dma->setMemoryDescriptor(state->backing,true)!=kIOReturnSuccess) {
        setProperty("GTBringupSurfaceState","intel-dma-bind-failed");
        releasePersistentSurface();
        return false;
    }
    state->dmaBound=true;

    const vm_size_t dmaSegBytes=(vm_size_t)SurfacePages*sizeof(IODMACommand::Segment64);
    const vm_size_t normalizedBytes=(vm_size_t)SurfacePages*sizeof(IGGtSurfaceDmaSegment);
    const vm_size_t pageListBytes=(vm_size_t)SurfacePages*sizeof(IGSurfaceU64);
    IODMACommand::Segment64 *dmaSeg=
        reinterpret_cast<IODMACommand::Segment64 *>(IOMalloc(dmaSegBytes));
    IGGtSurfaceDmaSegment *normalized=
        reinterpret_cast<IGGtSurfaceDmaSegment *>(IOMalloc(normalizedBytes));
    IGSurfaceU64 *pages=reinterpret_cast<IGSurfaceU64 *>(IOMalloc(pageListBytes));
    if (!dmaSeg || !normalized || !pages) {
        if (dmaSeg) IOFree(dmaSeg,dmaSegBytes);
        if (normalized) IOFree(normalized,normalizedBytes);
        if (pages) IOFree(pages,pageListBytes);
        setProperty("GTBringupSurfaceState","dma-scratch-allocation-failed");
        releasePersistentSurface();
        return false;
    }
    bzero(dmaSeg,dmaSegBytes);
    bzero(normalized,normalizedBytes);
    bzero(pages,pageListBytes);

    UInt64 dmaOffset=0ull;
    UInt32 segmentCount=SurfacePages;
    const IOReturn segRet=state->dma->gen64IOVMSegments(
        &dmaOffset,dmaSeg,&segmentCount);
    state->segmentCount=segmentCount;
    setProperty("GTBringupSurfaceDmaSegmentIOReturn",(UInt64)(UInt32)segRet,32);
    setProperty("GTBringupSurfaceDmaSegmentCount",(UInt64)segmentCount,32);
    setProperty("GTBringupSurfaceDmaConsumedBytes",dmaOffset,64);

    bool segmentShapeOk=(segRet==kIOReturnSuccess &&
                         dmaOffset==(UInt64)SurfaceBytes &&
                         segmentCount>0u && segmentCount<=SurfacePages);
    if (segmentShapeOk) {
        for (UInt32 i=0;i<segmentCount;++i) {
            normalized[i].address=dmaSeg[i].fIOVMAddr;
            normalized[i].length=dmaSeg[i].fLength;
        }
    }

    UInt32 flattenVerdict=IG_GTS_FLAT_BAD_COUNT;
    if (segmentShapeOk) {
        flattenVerdict=igGtSurfaceFlattenSegments(
            normalized,segmentCount,SurfacePages,pages,SurfacePages);
    }
    setProperty("GTBringupSurfaceFlattenVerdict",(UInt64)flattenVerdict,32);

    UInt32 pteVerdict=IG_GTS_PTE_NOT_ATTEMPTED;
    if (segmentShapeOk && igGtSurfaceFlattenSucceeded(flattenVerdict)) {
        // fPatIndex came from the live TGL PAT table in claimEngine(); this path
        // never invents or rewrites a PAT entry.
        pteVerdict=igGtSurfaceBuildPtes(
            pages,SurfacePages,fPatIndex,state->installedPtes,SurfacePages);
    }
    setProperty("GTBringupSurfacePteVerdict",(UInt64)pteVerdict,32);

    IOFree(pages,pageListBytes);
    IOFree(normalized,normalizedBytes);
    IOFree(dmaSeg,dmaSegBytes);

    if (!segmentShapeOk || !igGtSurfaceFlattenSucceeded(flattenVerdict) ||
        !igGtSurfacePteSucceeded(pteVerdict)) {
        setProperty("GTBringupSurfaceState","intel-dma-pte-plan-refused");
        releasePersistentSurface();
        return false;
    }

    PrivatePtAccess access={
        reinterpret_cast<volatile IGSurfaceU64 *>(fCPU+PrivatePtPoolOffset)
    };
    const UInt32 prepareVerdict=igGtSurfaceTxnPrepare(
        IG_GTS_ARENA_FIRST_LEAF_64,
        state->installedPtes,
        SurfacePages,
        state->snapshot,
        SurfacePages,
        readPrivatePt,
        &access,
        &state->txn);
    setProperty("GTBringupSurfaceTxnPrepareVerdict",(UInt64)prepareVerdict,32);
    if (!igGtSurfaceTxnSucceeded(prepareVerdict)) {
        setProperty("GTBringupSurfaceState","private-pt-prepare-refused");
        releasePersistentSurface();
        return false;
    }

    IGGtSurfacePteTxnStats stats={};
    const UInt32 commitVerdict=igGtSurfaceTxnCommit(
        &state->txn,
        state->installedPtes,
        state->snapshot,
        SurfacePages,
        readPrivatePt,
        writePrivatePt,
        &access,
        &stats);
    setProperty("GTBringupSurfaceTxnCommitVerdict",(UInt64)commitVerdict,32);
    setProperty("GTBringupSurfaceTxnWritesVerified",(UInt64)stats.writesVerified,32);
    setProperty("GTBringupSurfaceTxnRecoveryWrites",(UInt64)stats.recoveryWrites,32);

    if (!igGtSurfaceTxnSucceeded(commitVerdict)) {
        if (commitVerdict==IG_GTS_TXN_ROLLBACK_FAILED) {
            // We cannot prove the private PT returned to its old image. Keep
            // both page-table storage and backing alive rather than freeing
            // memory an uncertain PTE could still name.
            state->unsafe=true;
            fQuarantined=true;
            setProperty("GTBringupSurfaceState","install-rollback-failed-quarantined");
            setProperty("GTBringupSurfaceQuarantined",true);
            return false;
        }
        setProperty("GTBringupSurfaceState","private-pt-install-refused-rolled-back");
        releasePersistentSurface();
        return false;
    }

    state->installed=true;
    setProperty("GTBringupSurfacePpgttInstalled",true);
    setProperty("GTBringupSurfaceReady",true);
    setProperty("GTBringupSurfaceState","owned-intel-iommu-pages-installed-private-pt");
    IGSURFRTLOG("installed %u private PPGTT leaves from %u Intel-domain segment(s)",
                SurfacePages,segmentCount);
    return true;
}

bool IntelGtBringup::releasePersistentSurface(void)
{
    IntelGtSurfaceRuntimeState *state=fSurfaceRuntime;
    if (!state) return true;
    if (state->unsafe) return false;

    if (state->installed) {
        if (!fCPU) {
            state->unsafe=true;
            setProperty("GTBringupSurfaceState","restore-pt-unavailable-quarantined");
            return false;
        }
        PrivatePtAccess access={
            reinterpret_cast<volatile IGSurfaceU64 *>(fCPU+PrivatePtPoolOffset)
        };
        IGGtSurfacePteTxnStats stats={};
        const UInt32 restoreVerdict=igGtSurfaceTxnRestore(
            &state->txn,
            state->installedPtes,
            state->snapshot,
            SurfacePages,
            readPrivatePt,
            writePrivatePt,
            &access,
            &stats);
        setProperty("GTBringupSurfaceTxnRestoreVerdict",(UInt64)restoreVerdict,32);
        setProperty("GTBringupSurfaceTxnRestoreWritesVerified",(UInt64)stats.writesVerified,32);
        setProperty("GTBringupSurfaceTxnRestoreRecoveryWrites",(UInt64)stats.recoveryWrites,32);
        if (!igGtSurfaceTxnSucceeded(restoreVerdict)) {
            // RESTORE_FAILED_REINSTALLED deliberately leaves the owned image in
            // place; STATE_MISMATCH means ownership was lost. Both require the
            // backing + private root to stay alive until an operator can inspect.
            state->unsafe=true;
            setProperty("GTBringupSurfaceState","restore-refused-quarantined");
            setProperty("GTBringupSurfaceQuarantined",true);
            return false;
        }
        state->installed=false;
        setProperty("GTBringupSurfacePpgttInstalled",false);
    }

    releaseRuntimeStorage(state);
    fSurfaceRuntime=nullptr;
    setProperty("GTBringupSurfaceReady",false);
    setProperty("GTBringupSurfaceState","released-exact-old-private-pt-image");
    return true;
}
