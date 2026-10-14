// SPDX-License-Identifier: MIT
// Extend the proven Tiger Lake BCS0 bring-up with one persistent GPU-visible
// system-memory arena. The base class remains the only command submitter.
#include "IntelGtSurfaceBringup.hpp"

#include <IOKit/IOMapper.h>
#include <IOKit/IOLib.h>
#include <mach/task.h>
#include <pexpert/pexpert.h>

#define GTSURFLOG(fmt, ...) IOLog("IntelGtSurfaceBringup: " fmt "\n", ##__VA_ARGS__)
OSDefineMetaClassAndStructors(IntelGtSurfaceBringup, IntelGtBringup);

namespace {
constexpr UInt32 ArenaPages = IG_GTS_ARENA_LEAVES_448;
constexpr UInt32 ArenaBytes = IG_GTS_ARENA_BYTES_0x1C0000;
constexpr UInt32 PageBytes = IG_GTS_PAGE_BYTES_POLICY_4096;
constexpr UInt8 IntelDmaAddressBits = 46;
}

bool IntelGtSurfaceBringup::init(OSDictionary *dict)
{
    if (!IntelGtBringup::init(dict)) return false;
    fSurfaceBacking=nullptr;
    fSurfaceDMA=nullptr;
    fSurfaceCPU=nullptr;
    bzero(fSurfaceSavedPtes,sizeof(fSurfaceSavedPtes));
    fSurfaceInstallCount=0u;
    fSurfaceDMABound=false;
    fSurfaceInstalled=false;
    fSurfaceRestoreFailed=false;
    return true;
}

void IntelGtSurfaceBringup::publishSurfaceState(const char *state)
{
    if (state) setProperty("GTSurfaceArenaState",state);
}

void IntelGtSurfaceBringup::releaseSurfaceArena(void)
{
    // A failed rollback means the private PT may still reference this mapping.
    // Leak it deliberately until reboot rather than manufacture a DMA UAF.
    if (fSurfaceInstalled || fSurfaceRestoreFailed) return;
    if (fSurfaceDMABound && fSurfaceDMA) {
        fSurfaceDMA->clearMemoryDescriptor();
        fSurfaceDMABound=false;
    }
    OSSafeReleaseNULL(fSurfaceDMA);
    OSSafeReleaseNULL(fSurfaceBacking);
    fSurfaceCPU=nullptr;
    setProperty("GTSurfaceArenaDmaMapped",false);
}

bool IntelGtSurfaceBringup::restoreSurfaceArena(void)
{
    if (!fSurfaceInstallCount && !fSurfaceInstalled) return true;
    UInt64 *pt=surfacePrivatePt();
    if (!pt || fSurfaceInstallCount>ArenaPages) {
        fSurfaceRestoreFailed=true;
        setProperty("GTSurfaceArenaRestoreVerified",false);
        publishSurfaceState("rollback-unavailable-retained");
        return false;
    }

    const UInt32 count=fSurfaceInstallCount;
    bool exact=true;
    for (UInt32 i=0u;i<count;++i) {
        const UInt32 leaf=IG_GTS_ARENA_FIRST_LEAF_64+i;
        pt[leaf]=fSurfaceSavedPtes[i];
        __asm__ volatile("mfence" ::: "memory");
        if (pt[leaf]!=fSurfaceSavedPtes[i]) exact=false;
    }
    if (!exact || !igGtSurfaceLeafImageMatches(
            pt,IG_GTS_PT_LEAVES_GEN8_PLUS_512,
            IG_GTS_ARENA_FIRST_LEAF_64,fSurfaceSavedPtes,count)) {
        fSurfaceRestoreFailed=true;
        setProperty("GTSurfaceArenaRestoreVerified",false);
        publishSurfaceState("rollback-readback-failed-retained");
        return false;
    }

    fSurfaceInstallCount=0u;
    fSurfaceInstalled=false;
    setProperty("GTSurfaceArenaInstalled",false);
    setProperty("GTSurfaceArenaRestoreVerified",true);
    publishSurfaceState("rolled-back-exactly");
    return true;
}

bool IntelGtSurfaceBringup::stageSurfaceArena(void)
{
    if (!surfaceExtensionReady() || fSurfaceBacking || fSurfaceDMA ||
        fSurfaceInstalled || fSurfaceInstallCount || fSurfaceRestoreFailed) {
        publishSurfaceState("preconditions-refused");
        return false;
    }

    IOPCIDevice *pci=surfaceOwnedProvider();
    if (!pci || pci->configRead16(kIOPCIConfigVendorID)!=0x8086 ||
        pci->configRead16(kIOPCIConfigDeviceID)!=0x9a68) {
        publishSurfaceState("exact-device-refused");
        return false;
    }

    // Noncontiguous by construction: no physical-contiguous option and no
    // physical-segment walk. The exact device IOMapper owns the IOVA mapping.
    fSurfaceBacking=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,kIODirectionInOut,(vm_size_t)ArenaBytes,(vm_offset_t)PageBytes);
    if (!fSurfaceBacking || fSurfaceBacking->getCapacity()<(vm_size_t)ArenaBytes) {
        publishSurfaceState("backing-allocation-failed");
        releaseSurfaceArena();
        return false;
    }
    fSurfaceCPU=reinterpret_cast<UInt8 *>(fSurfaceBacking->getBytesNoCopy());
    if (!fSurfaceCPU) {
        publishSurfaceState("backing-cpu-map-failed");
        releaseSurfaceArena();
        return false;
    }
    bzero(fSurfaceCPU,ArenaBytes);

    IOMapper *mapper=IOMapper::copyMapperForDevice(pci);
    if (!mapper) {
        publishSurfaceState("intel-device-mapper-unavailable");
        releaseSurfaceArena();
        return false;
    }
    fSurfaceDMA=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        IntelDmaAddressBits,
        (UInt64)PageBytes,
        IODMACommand::kMapped,
        (UInt64)ArenaBytes,
        PageBytes,
        mapper);
    mapper->release();
    if (!fSurfaceDMA ||
        fSurfaceDMA->setMemoryDescriptor(fSurfaceBacking,true)!=kIOReturnSuccess) {
        publishSurfaceState("intel-dma-bind-failed");
        releaseSurfaceArena();
        return false;
    }
    fSurfaceDMABound=true;

    const vm_size_t dmaBytes=(vm_size_t)ArenaPages*sizeof(IODMACommand::Segment64);
    const vm_size_t segmentBytes=(vm_size_t)ArenaPages*sizeof(IGGtSurfaceDmaSegment);
    const vm_size_t pageBytes=(vm_size_t)ArenaPages*sizeof(IGSurfaceU64);
    IODMACommand::Segment64 *dma=
        reinterpret_cast<IODMACommand::Segment64 *>(IOMalloc(dmaBytes));
    IGGtSurfaceDmaSegment *segments=
        reinterpret_cast<IGGtSurfaceDmaSegment *>(IOMalloc(segmentBytes));
    IGSurfaceU64 *pages=reinterpret_cast<IGSurfaceU64 *>(IOMalloc(pageBytes));
    IGSurfaceU64 *ptes=reinterpret_cast<IGSurfaceU64 *>(IOMalloc(pageBytes));
    if (!dma || !segments || !pages || !ptes) {
        if (dma) IOFree(dma,dmaBytes);
        if (segments) IOFree(segments,segmentBytes);
        if (pages) IOFree(pages,pageBytes);
        if (ptes) IOFree(ptes,pageBytes);
        publishSurfaceState("scratch-allocation-failed");
        releaseSurfaceArena();
        return false;
    }
    bzero(dma,dmaBytes);
    bzero(segments,segmentBytes);
    bzero(pages,pageBytes);
    bzero(ptes,pageBytes);

    bool ok=false;
    UInt64 dmaOffset=0ull;
    UInt32 segmentCount=ArenaPages;
    const IOReturn segmentResult=
        fSurfaceDMA->gen64IOVMSegments(&dmaOffset,dma,&segmentCount);
    setProperty("GTSurfaceArenaDmaSegmentCount",(UInt64)segmentCount,32);
    setProperty("GTSurfaceArenaDmaConsumedBytes",dmaOffset,64);
    if (segmentResult==kIOReturnSuccess &&
        dmaOffset==(UInt64)ArenaBytes && segmentCount>0u && segmentCount<=ArenaPages) {
        for (UInt32 i=0u;i<segmentCount;++i) {
            segments[i].address=dma[i].fIOVMAddr;
            segments[i].length=dma[i].fLength;
        }
        const UInt32 flat=igGtSurfaceFlattenSegments(
            segments,segmentCount,ArenaPages,pages,ArenaPages);
        const UInt32 pte=igGtSurfaceFlattenSucceeded(flat)
            ? igGtSurfaceBuildPtes(
                pages,ArenaPages,surfaceLivePatIndex(),ptes,ArenaPages)
            : IG_GTS_PTE_NOT_ATTEMPTED;
        setProperty("GTSurfaceArenaFlattenVerdict",(UInt64)flat,32);
        setProperty("GTSurfaceArenaPteVerdict",(UInt64)pte,32);

        UInt64 *pt=surfacePrivatePt();
        IGGtSurfaceInstallPlan install={};
        const UInt32 installVerdict=
            igGtSurfacePteSucceeded(pte) && pt
                ? igGtSurfacePlanInstall(
                    pt,IG_GTS_PT_LEAVES_GEN8_PLUS_512,
                    ptes,ArenaPages,&install)
                : IG_GTSI_NOT_ATTEMPTED;
        setProperty("GTSurfaceArenaInstallVerdict",(UInt64)installVerdict,32);

        if (igGtSurfaceInstallSucceeded(installVerdict) &&
            install.firstLeaf==IG_GTS_ARENA_FIRST_LEAF_64 &&
            install.leafCount==ArenaPages &&
            install.gpuBase==IG_GTS_ARENA_GPU_BASE_0x40000 &&
            install.bytes==ArenaBytes) {
            for (UInt32 i=0u;i<ArenaPages;++i)
                fSurfaceSavedPtes[i]=pt[IG_GTS_ARENA_FIRST_LEAF_64+i];

            ok=true;
            for (UInt32 i=0u;i<ArenaPages;++i) {
                const UInt32 leaf=IG_GTS_ARENA_FIRST_LEAF_64+i;
                pt[leaf]=ptes[i];
                __asm__ volatile("mfence" ::: "memory");
                fSurfaceInstallCount=i+1u;
                if (pt[leaf]!=ptes[i]) {
                    ok=false;
                    break;
                }
            }
            if (ok && !igGtSurfaceLeafImageMatches(
                    pt,IG_GTS_PT_LEAVES_GEN8_PLUS_512,
                    IG_GTS_ARENA_FIRST_LEAF_64,ptes,ArenaPages))
                ok=false;
        }
    }

    IOFree(ptes,pageBytes);
    IOFree(pages,pageBytes);
    IOFree(segments,segmentBytes);
    IOFree(dma,dmaBytes);

    if (!ok) {
        if (fSurfaceInstallCount && !restoreSurfaceArena()) {
            surfaceMarkFault("persistent surface PPGTT rollback readback failed; mapping retained");
            return false;
        }
        publishSurfaceState("install-refused-or-readback-failed");
        releaseSurfaceArena();
        return false;
    }

    fSurfaceInstalled=true;
    setProperty("GTSurfaceArenaDmaMapped",true);
    setProperty("GTSurfaceArenaInstalled",true);
    setProperty("GTSurfaceArenaRestoreVerified",false);
    setProperty("GTSurfaceArenaBytes",(UInt64)ArenaBytes,32);
    setProperty("GTSurfaceArenaLeaves",(UInt64)ArenaPages,32);
    setProperty("GTSurfaceArenaPatIndex",(UInt64)surfaceLivePatIndex(),32);
    // The context has run before these new PTEs existed. Do not claim the arena
    // usable by BCS until a later source-backed PPGTT/TLB invalidation stage.
    setProperty("GTSurfaceArenaGpuUseReady",false);
    publishSurfaceState("installed-awaiting-tlb-invalidation");
    GTSURFLOG("installed leaves 64..511 with exact readback; GPU use still gated");
    return true;
}

bool IntelGtSurfaceBringup::start(IOService *provider)
{
    int surfaceGate=0;
    const bool surfaceRequested=
        PE_parse_boot_argn("iggt-surface-arena",&surfaceGate,sizeof(surfaceGate));
    if (surfaceRequested && surfaceGate!=1) return false;

    if (surfaceRequested) {
        int stage=0;
        if (!PE_parse_boot_argn("iggt-bringup",&stage,sizeof(stage)) || stage!=3)
            return false;
    }

    if (!IntelGtBringup::start(provider)) return false;
    setProperty("GTSurfaceArenaRequested",surfaceRequested);
    setProperty("GTSurfaceArenaInstalled",false);
    setProperty("GTSurfaceArenaGpuUseReady",false);
    setProperty("GTSurfaceArenaRawIovaPublished",false);
    if (!surfaceRequested) {
        publishSurfaceState("gate-disabled-base-behavior");
        return true;
    }

    IOLock *lock=surfaceQueueLock();
    if (!lock) {
        publishSurfaceState("queue-lock-unavailable");
        return true;
    }
    IOLockLock(lock);
    const bool ready=surfaceExtensionReady();
    const bool staged=ready && stageSurfaceArena();
    IOLockUnlock(lock);
    if (!ready) publishSurfaceState("base-stage3-not-ready");
    else if (!staged && !fSurfaceRestoreFailed)
        publishSurfaceState("surface-stage-not-ready");
    return true;
}

void IntelGtSurfaceBringup::stop(IOService *provider)
{
    // Base stop acquires the queue lock and marks the submit path faulted first.
    // Only after submissions are disabled is it safe to remove arena PTEs.
    IntelGtBringup::stop(provider);
    if (fSurfaceInstalled || fSurfaceInstallCount) {
        if (!restoreSurfaceArena()) {
            surfaceMarkFault("persistent surface rollback failed during stop; DMA retained");
            return;
        }
    }
    releaseSurfaceArena();
}

void IntelGtSurfaceBringup::free(void)
{
    // Defensive path for abnormal teardown where stop() was skipped. Disable
    // the inherited queue under its own lock before touching the private PT.
    if ((fSurfaceInstalled || fSurfaceInstallCount) && !fSurfaceRestoreFailed) {
        IOLock *lock=surfaceQueueLock();
        if (lock) {
            IOLockLock(lock);
            surfaceMarkFault("free while persistent surface installed; disabling queue");
            const bool restored=restoreSurfaceArena();
            IOLockUnlock(lock);
            if (!restored) {
                fSurfaceRestoreFailed=true;
            }
        } else {
            fSurfaceRestoreFailed=true;
        }
    }
    releaseSurfaceArena();
    IntelGtBringup::free();
}
