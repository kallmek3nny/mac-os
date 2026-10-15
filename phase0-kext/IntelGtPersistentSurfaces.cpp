// SPDX-License-Identifier: MIT
// Fixed persistent-surface operations for IntelGtBringup.
//
// Userspace never supplies a GPU address or command stream. Opaque handles are
// resolved under the existing synchronous queue lock, converted back through
// the host-tested arena planner, and consumed only by the driver's fixed Gen12
// copy/fill encoders. Every submit still requires fence completion + CSB
// retirement before the queue lock is released.
#include "IntelGtBringup.hpp"
#include "IntelGtBlitter.hpp"
#include "IntelGtSurfaceTxn.hpp"

#include <libkern/libkern.h>

namespace {
constexpr UInt32 SurfacePages = IG_GTS_ARENA_LEAVES_448;
constexpr UInt32 PageBytes = IG_GTS_PAGE_BYTES_POLICY_4096;

// Keep this definition token-for-token aligned with IntelGtBringupSurface.cpp.
// It is the private runtime owned by the same IntelGtBringup instance, not a
// second PCI/accelerator personality.
struct SurfaceRuntimeLayout {
    IOBufferMemoryDescriptor *backing;
    IODMACommand *dma;
    UInt8 *cpu;
    bool dmaBound;
    bool installed;
    bool unsafe;
    UInt32 segmentCount;
    IGGtSurfacePteTxn txn;
    IGSurfaceU64 snapshot[SurfacePages];
    IGSurfaceU64 installedPtes[SurfacePages];
};

inline void surfaceCpuBarrier()
{
    __asm__ volatile("mfence" ::: "memory");
}

IOReturn handleVerdictToIOReturn(IGSurfaceU32 verdict)
{
    switch (verdict) {
        case IG_GTH_OK:
            return kIOReturnSuccess;
        case IG_GTH_ERR_NO_SLOT:
        case IG_GTH_ERR_NO_PAGES:
            return kIOReturnNoMemory;
        case IG_GTH_ERR_CORRUPT:
            return kIOReturnIOError;
        case IG_GTH_ERR_NULL:
        case IG_GTH_ERR_PLAN:
        case IG_GTH_ERR_BAD_HANDLE:
        case IG_GTH_ERR_STALE_HANDLE:
        default:
            return kIOReturnBadArgument;
    }
}

bool resolveHandlePlan(IGGtSurfaceHandleArena *arena,
                       UInt64 handle,
                       IGGtSurfaceHandleView *view,
                       IGGtSurfacePlan *plan,
                       IGSurfaceU32 *verdict)
{
    if (!arena || !view || !plan || !verdict)
        return false;

    *verdict=igGtSurfaceHandleResolve(arena,(IGSurfaceU64)handle,view);
    if (!igGtSurfaceHandleSucceeded(*verdict))
        return false;

    const IGSurfaceU32 planVerdict=igGtSurfacePlan32(
        view->width,view->height,view->pitch,view->firstArenaPage,plan);
    if (!igGtSurfacePlanSucceeded(planVerdict) ||
        plan->pageCount!=view->pageCount ||
        plan->usedBytes!=view->usedBytes ||
        plan->allocationBytes!=(IGSurfaceU32)((IGSurfaceU64)view->pageCount*PageBytes)) {
        *verdict=IG_GTH_ERR_CORRUPT;
        return false;
    }
    return true;
}
}

IOReturn IntelGtBringup::allocatePersistentSurface32(UInt32 width,
                                                      UInt32 height,
                                                      UInt32 pitch,
                                                      UInt64 *outHandle)
{
    if (!outHandle || !fQueueLock)
        return kIOReturnBadArgument;

    IOLockLock(fQueueLock);
    IOReturn result=kIOReturnNotReady;
    SurfaceRuntimeLayout *state=reinterpret_cast<SurfaceRuntimeLayout *>(fSurfaceRuntime);
    if (fStage==3 && fCompleted && fRetired && !fFaulted && fCPU &&
        state && state->installed && !state->unsafe && state->cpu) {
        IGSurfaceU64 stagedHandle=0ull;
        const IGSurfaceU32 allocVerdict=igGtSurfaceHandleAllocate(
            &fSurfaceHandles,width,height,pitch,&stagedHandle);
        setProperty("GTBringupSurfaceHandleAllocVerdict",(UInt64)allocVerdict,32);
        if (!igGtSurfaceHandleSucceeded(allocVerdict)) {
            result=handleVerdictToIOReturn(allocVerdict);
        } else {
            IGGtSurfaceHandleView view={};
            IGGtSurfacePlan plan={};
            IGSurfaceU32 resolveVerdict=IG_GTH_ERR_CORRUPT;
            if (!resolveHandlePlan(&fSurfaceHandles,stagedHandle,&view,&plan,&resolveVerdict)) {
                setProperty("GTBringupSurfaceHandleResolveVerdict",(UInt64)resolveVerdict,32);
                // Best-effort metadata rollback. No handle has crossed the ABI yet.
                const IGSurfaceU32 rollbackVerdict=
                    igGtSurfaceHandleFree(&fSurfaceHandles,stagedHandle);
                setProperty("GTBringupSurfaceHandleAllocRollbackVerdict",
                            (UInt64)rollbackVerdict,32);
                result=kIOReturnIOError;
            } else {
                // Clear the entire rounded allocation, not merely usedBytes, so
                // padding and pages reused from an older handle cannot leak.
                const size_t cpuOffset=(size_t)view.firstArenaPage*PageBytes;
                bzero(state->cpu+cpuOffset,(size_t)plan.allocationBytes);
                surfaceCpuBarrier();
                *outHandle=(UInt64)stagedHandle;
                setProperty("GTBringupSurfaceHandleZeroedOnAlloc",true);
                result=kIOReturnSuccess;
            }
        }
    }
    IOLockUnlock(fQueueLock);
    return result;
}

IOReturn IntelGtBringup::freePersistentSurface(UInt64 handle)
{
    if (!handle || !fQueueLock)
        return kIOReturnBadArgument;

    IOLockLock(fQueueLock);
    IOReturn result=kIOReturnNotReady;
    SurfaceRuntimeLayout *state=reinterpret_cast<SurfaceRuntimeLayout *>(fSurfaceRuntime);
    if (fStage==3 && fCompleted && fRetired && !fFaulted && fCPU &&
        state && state->installed && !state->unsafe && state->cpu) {
        IGGtSurfaceHandleView view={};
        IGGtSurfacePlan plan={};
        IGSurfaceU32 resolveVerdict=IG_GTH_ERR_CORRUPT;
        if (!resolveHandlePlan(&fSurfaceHandles,handle,&view,&plan,&resolveVerdict)) {
            setProperty("GTBringupSurfaceHandleResolveVerdict",(UInt64)resolveVerdict,32);
            result=handleVerdictToIOReturn(resolveVerdict);
        } else {
            // The queue is retired and the same lock excludes a new submit or
            // allocator reuse. Scrub while pages are still owned by this slot.
            const size_t cpuOffset=(size_t)view.firstArenaPage*PageBytes;
            bzero(state->cpu+cpuOffset,(size_t)plan.allocationBytes);
            surfaceCpuBarrier();
            const IGSurfaceU32 freeVerdict=
                igGtSurfaceHandleFree(&fSurfaceHandles,(IGSurfaceU64)handle);
            setProperty("GTBringupSurfaceHandleFreeVerdict",(UInt64)freeVerdict,32);
            if (igGtSurfaceHandleSucceeded(freeVerdict)) {
                setProperty("GTBringupSurfaceHandleScrubbedOnFree",true);
                result=kIOReturnSuccess;
            } else {
                result=handleVerdictToIOReturn(freeVerdict);
            }
        }
    }
    IOLockUnlock(fQueueLock);
    return result;
}

IOReturn IntelGtBringup::fillPersistentSurface32(UInt64 handle, UInt32 color)
{
    if (!handle || !fQueueLock)
        return kIOReturnBadArgument;

    IOLockLock(fQueueLock);
    IOReturn result=kIOReturnNotReady;
    SurfaceRuntimeLayout *state=reinterpret_cast<SurfaceRuntimeLayout *>(fSurfaceRuntime);
    if (fStage==3 && fCompleted && fRetired && !fFaulted && fCPU &&
        state && state->installed && !state->unsafe && state->cpu) {
        IGGtSurfaceHandleView view={};
        IGGtSurfacePlan plan={};
        IGSurfaceU32 resolveVerdict=IG_GTH_ERR_CORRUPT;
        if (!resolveHandlePlan(&fSurfaceHandles,handle,&view,&plan,&resolveVerdict)) {
            setProperty("GTBringupSurfaceHandleResolveVerdict",(UInt64)resolveVerdict,32);
            result=handleVerdictToIOReturn(resolveVerdict);
        } else {
            const GTBlitSurface32 dest={plan.gpuAddress,plan.allocationBytes,plan.pitch};
            if (++fSequence==0u) ++fSequence;
            if (!gtEncodeFill32(reinterpret_cast<UInt32 *>(fCPU+24u*4096u),16,
                                dest,plan.width,plan.height,color,
                                19u*4096u,fSequence)) {
                result=kIOReturnBadArgument;
            } else if (!submitBatch(fSequence)) {
                fFaulted=true;
                result=kIOReturnIOError;
            } else {
                setProperty("GTBringupPersistentFillCompleted",true);
                result=kIOReturnSuccess;
            }
        }
    }
    IOLockUnlock(fQueueLock);
    return result;
}

IOReturn IntelGtBringup::copyPersistentSurface32(UInt64 sourceHandle,
                                                  UInt64 destinationHandle)
{
    if (!sourceHandle || !destinationHandle || sourceHandle==destinationHandle || !fQueueLock)
        return kIOReturnBadArgument;

    IOLockLock(fQueueLock);
    IOReturn result=kIOReturnNotReady;
    SurfaceRuntimeLayout *state=reinterpret_cast<SurfaceRuntimeLayout *>(fSurfaceRuntime);
    if (fStage==3 && fCompleted && fRetired && !fFaulted && fCPU &&
        state && state->installed && !state->unsafe && state->cpu) {
        IGGtSurfaceHandleView srcView={}, dstView={};
        IGGtSurfacePlan srcPlan={}, dstPlan={};
        IGSurfaceU32 srcVerdict=IG_GTH_ERR_CORRUPT;
        IGSurfaceU32 dstVerdict=IG_GTH_ERR_CORRUPT;
        if (!resolveHandlePlan(&fSurfaceHandles,sourceHandle,&srcView,&srcPlan,&srcVerdict)) {
            setProperty("GTBringupSurfaceHandleResolveVerdict",(UInt64)srcVerdict,32);
            result=handleVerdictToIOReturn(srcVerdict);
        } else if (!resolveHandlePlan(&fSurfaceHandles,destinationHandle,
                                      &dstView,&dstPlan,&dstVerdict)) {
            setProperty("GTBringupSurfaceHandleResolveVerdict",(UInt64)dstVerdict,32);
            result=handleVerdictToIOReturn(dstVerdict);
        } else if (srcPlan.width!=dstPlan.width || srcPlan.height!=dstPlan.height ||
                   srcPlan.pitch!=dstPlan.pitch || srcPlan.usedBytes!=dstPlan.usedBytes) {
            result=kIOReturnBadArgument;
        } else {
            const UInt64 srcEnd=(UInt64)srcPlan.gpuAddress+srcPlan.allocationBytes;
            const UInt64 dstEnd=(UInt64)dstPlan.gpuAddress+dstPlan.allocationBytes;
            if ((UInt64)srcPlan.gpuAddress<dstEnd && (UInt64)dstPlan.gpuAddress<srcEnd) {
                // Distinct live allocator slots may never overlap. Treat this as
                // metadata corruption rather than asking the BLT encoder to cope.
                result=kIOReturnIOError;
            } else {
                const GTBlitSurface32 src={srcPlan.gpuAddress,srcPlan.allocationBytes,srcPlan.pitch};
                const GTBlitSurface32 dst={dstPlan.gpuAddress,dstPlan.allocationBytes,dstPlan.pitch};
                if (++fSequence==0u) ++fSequence;
                if (!gtEncodeCopy32(reinterpret_cast<UInt32 *>(fCPU+24u*4096u),16,
                                    src,dst,srcPlan.width,srcPlan.height,
                                    19u*4096u,fSequence)) {
                    result=kIOReturnBadArgument;
                } else if (!submitBatch(fSequence)) {
                    fFaulted=true;
                    result=kIOReturnIOError;
                } else {
                    setProperty("GTBringupPersistentCopyCompleted",true);
                    result=kIOReturnSuccess;
                }
            }
        }
    }
    IOLockUnlock(fQueueLock);
    return result;
}
