// SPDX-License-Identifier: MIT
// Borrowed INHERITED scanout for IntelGtBringup (t-d0bcef). iggt-bringup=4 only.
//
// The firmware already presents pipe A plane 1 from pages its GGTT maps. This
// file samples that plane once, READS the GGTT PTEs covering PLANE_SURF ..
// PLANE_SURF + extent (it never writes a GGTT PTE), copies their page addresses
// into fresh private-PPGTT leaf tables hung from pd[1..], and lets BCS0 fill
// one rectangle of the presented surface with the fixed Gen12 fill encoder.
//
// No display register is written. The leaf tables are driver-owned; the
// scanout pages stay owned by the firmware and the display engine, so teardown
// clears the PD entries and frees the tables, never the pages they name.
// All range, PTE and rectangle arithmetic lives in IntelGtScanoutMap.hpp.
#include "IntelGtBringup.hpp"
#include "IntelGtBlitter.hpp"
#include "IntelGtScanoutMap.hpp"
#include "IntelFbPresent.hpp"

#include <mach/task.h>
#include <libkern/libkern.h>

#define IGSCANLOG(fmt, ...) IOLog("IntelGtScanout: " fmt "\n", ##__VA_ARGS__)

struct IntelGtScanoutRuntimeState {
    IOBufferMemoryDescriptor *ptPool;   // plan.ptCount contiguous leaf tables
    IOMemoryMap *ptMap;                 // uncached CPU view of those tables
    IODMACommand *ptDMA;
    UInt8 *ptCPU;
    UInt64 ptBus;
    UInt32 ptBytes;
    bool prepared;
    bool dmaBound;
    bool installed;   // pd[plan.firstPdIndex ..] may name the tables at ptBus
    bool unsafe;      // a PD restore was not proven; never free the tables
    bool presentOk;   // the one sample's igPresentVerdict() was IG_PRESENT_OK
    UInt32 width, height, strideBytes, bytesPerPixel;
    IGGtScanoutPlan plan;
    IGGtScanU64 pdEntries[IG_GTSCAN_MAX_PTS_POLICY_32];
};

namespace {
constexpr UInt32 PageBytes = IG_GTSCAN_PAGE_BYTES_POLICY_4096;
constexpr UInt8 IntelDmaAddressBits = 46;
constexpr UInt64 TableAddressMask = 0x00003ffffffff000ULL;
// Gen8+ GGTT PTE window at BAR0 + 8 MiB: the table installGGTT() scans.
constexpr UInt32 GgttTableOffset = 0x800000u;
// Fixed pages of IntelGtBringup's private 32-page pool (IntelGtBringup.cpp).
constexpr UInt32 FenceGpuAddress = 19u * PageBytes;
constexpr UInt32 PdPoolOffset = 22u * PageBytes;
constexpr UInt32 BatchPoolOffset = 24u * PageBytes;
// Pipe A plane 1, the group IntelFbPresentProbe samples (IG_PRESENT_PIPE/PLANE).
constexpr UInt32 ScanoutPipe = 0u;
constexpr UInt32 ScanoutPlane = 0u;
// start()'s one stage-4 rectangle, small and near the top-left corner. All
// ones is white in both XRGB8888 and XRGB2101010, so the colour does not
// depend on the open PLANE_CTL format decode (t-a7929c).
constexpr UInt32 ProbeX = 32u, ProbeY = 32u, ProbeWidth = 128u, ProbeHeight = 128u;
constexpr UInt32 ProbeColor = 0xFFFFFFFFu;

inline void scanoutBarrier()
{
    __asm__ volatile("mfence" ::: "memory");
}

void releaseScanoutStorage(IntelGtScanoutRuntimeState *state)
{
    if (!state) return;
    // Clear each flag BEFORE its release so a re-entrant teardown is a no-op.
    // dmaBound implies ptDMA and prepared implies ptPool, as in cleanup().
    if (state->dmaBound) {
        state->dmaBound=false;
        state->ptDMA->clearMemoryDescriptor();
    }
    OSSafeReleaseNULL(state->ptDMA);
    if (state->ptMap) { state->ptMap->release(); state->ptMap=nullptr; }
    state->ptCPU=nullptr;
    if (state->prepared) {
        state->prepared=false;
        state->ptPool->complete(kIODirectionOutIn);
    }
    OSSafeReleaseNULL(state->ptPool);
    IOFree(state,(vm_size_t)sizeof(*state));
}
}

bool IntelGtBringup::mapScanout(void)
{
    setProperty("GTScanoutMapped",false);
    setProperty("GTScanoutFillCompleted",false);

    // start() calls this only at stage 4, after the cold-engine claim, live PAT
    // discovery, private-PPGTT allocation and installGGTT() have succeeded and
    // BEFORE the first submit, so no context has ever walked pd[1..].
    if (fStage!=4 || !fOpened || !fMMIO || !fCPU || !fGGTTEntries || fScanoutRuntime) {
        setProperty("GTScanoutState","owner-precondition-refused");
        return false;
    }

    // One sample of pipe A plane 1. Reads only; no display register is written.
    const UInt32 ctl=read32(igPlaneReg(ScanoutPipe,ScanoutPlane,IG_PLANE_CTL));
    const UInt32 stride=read32(igPlaneReg(ScanoutPipe,ScanoutPlane,IG_PLANE_STRIDE));
    const UInt32 size=read32(igPlaneReg(ScanoutPipe,ScanoutPlane,IG_PLANE_SIZE));
    const UInt32 surf=read32(igPlaneReg(ScanoutPipe,ScanoutPlane,IG_PLANE_SURF));
    // PLANE_SURF is an offset into the graphics address space the GGTT
    // describes, so that space is the aperture the extent must fit inside.
    const UInt64 aperture=(UInt64)fGGTTEntries*PageBytes;
    const UInt32 presentVerdict=igPresentVerdict(ctl,stride,size,surf,aperture);
    setProperty("GTScanoutPresentVerdict",(UInt64)presentVerdict,32);
    if (presentVerdict!=IG_PRESENT_OK) {
        setProperty("GTScanoutState","present-verdict-refused");
        return false;
    }

    const UInt32 width=igPresentWidth(ctl,stride,size,surf,aperture);
    const UInt32 height=igPresentHeight(ctl,stride,size,surf,aperture);
    const UInt32 strideBytes=igPresentBytesPerRow(ctl,stride,size,surf,aperture);
    const UInt32 bitsPerPixel=igPresentBitsPerPixel(ctl,stride,size,surf,aperture);
    const UInt32 baseOffset=igPresentBaseOffset(ctl,stride,size,surf,aperture);
    const UInt64 extent=igPresentExtentBytes(ctl,stride,size,surf,aperture);
    setProperty("GTScanoutWidth",(UInt64)width,32);
    setProperty("GTScanoutHeight",(UInt64)height,32);
    setProperty("GTScanoutBytesPerRow",(UInt64)strideBytes,32);
    setProperty("GTScanoutBitsPerPixel",(UInt64)bitsPerPixel,32);
    if (bitsPerPixel==IG_PRESENT_INVALID32 || (bitsPerPixel & 7u)!=0u) {
        setProperty("GTScanoutState","pixel-size-refused");
        return false;
    }

    IGGtScanoutPlan plan={};
    const UInt32 planVerdict=igGtScanoutPlanMap(
        baseOffset,extent,fGGTTEntries,fGGTTBase/PageBytes,PoolPages,&plan);
    setProperty("GTScanoutPlanVerdict",(UInt64)planVerdict,32);
    if (!igGtScanoutPlanSucceeded(planVerdict)) {
        setProperty("GTScanoutState","map-plan-refused");
        return false;
    }

    // Refuse here, before anything is allocated, a plane every fill would
    // refuse (for example a 64-bpp format or a stride the BLT cannot walk).
    IGGtScanoutFill geometry={};
    const UInt32 geometryVerdict=igGtScanoutFillRect32(
        &plan,width,height,strideBytes,bitsPerPixel/8u,0u,0u,1u,1u,&geometry);
    setProperty("GTScanoutGeometryVerdict",(UInt64)geometryVerdict,32);
    if (!igGtScanoutFillSucceeded(geometryVerdict)) {
        setProperty("GTScanoutState","fill-geometry-refused");
        return false;
    }

    // The window's PD slots must be exactly as allocateMemory() left them.
    volatile UInt64 *pd=reinterpret_cast<volatile UInt64 *>(fCPU+PdPoolOffset);
    if ((UInt64)plan.firstPdIndex+plan.ptCount>IG_GTSCAN_PT_LEAVES_GEN8_PLUS_512) {
        setProperty("GTScanoutState","pd-range-refused");
        return false;
    }
    for (UInt32 k=0;k<plan.ptCount;++k) {
        if (pd[plan.firstPdIndex+k]!=0ull) {
            setProperty("GTScanoutState","pd-range-occupied");
            return false;
        }
    }

    IntelGtScanoutRuntimeState *state=reinterpret_cast<IntelGtScanoutRuntimeState *>(
        IOMalloc((vm_size_t)sizeof(IntelGtScanoutRuntimeState)));
    if (!state) {
        setProperty("GTScanoutState","state-allocation-failed");
        return false;
    }
    bzero(state,sizeof(*state));
    fScanoutRuntime=state;
    // Only reached past the IG_PRESENT_OK refusal above. scanoutPlaneSize32()
    // still tests it so the GT-1k selector never trusts the call order.
    state->presentOk=(presentVerdict==IG_PRESENT_OK);
    state->plan=plan;
    state->width=width;
    state->height=height;
    state->strideBytes=strideBytes;
    state->bytesPerPixel=bitsPerPixel/8u;
    state->ptBytes=plan.ptCount*PageBytes;

    // Leaf-table storage, allocated and mapped exactly like the pool in
    // allocateMemory(): physically contiguous, uncached, one DMA segment.
    state->ptPool=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionOutIn|kIOMemoryPhysicallyContiguous,state->ptBytes,TableAddressMask);
    if (!state->ptPool || state->ptPool->prepare(kIODirectionOutIn)!=kIOReturnSuccess) {
        setProperty("GTScanoutState","table-allocation-failed");
        releaseScanout();
        return false;
    }
    state->prepared=true;
    state->ptMap=state->ptPool->map(kIOMapAnywhere|kIOMapInhibitCache);
    if (!state->ptMap || state->ptMap->getLength()<state->ptBytes) {
        setProperty("GTScanoutState","table-map-failed");
        releaseScanout();
        return false;
    }
    state->ptCPU=reinterpret_cast<UInt8 *>(state->ptMap->getVirtualAddress());
    if (!state->ptCPU) {
        setProperty("GTScanoutState","table-map-failed");
        releaseScanout();
        return false;
    }
    bzero(state->ptCPU,state->ptBytes);
    state->ptDMA=IODMACommand::withSpecification(&IODMACommand::OutputHost64,
        IntelDmaAddressBits,state->ptBytes,IODMACommand::kMapped,state->ptBytes,PageBytes);
    if (!state->ptDMA ||
        state->ptDMA->setMemoryDescriptor(state->ptPool,true)!=kIOReturnSuccess) {
        setProperty("GTScanoutState","table-dma-bind-failed");
        releaseScanout();
        return false;
    }
    state->dmaBound=true;
    UInt64 dmaOffset=0; UInt32 segmentCount=1; IODMACommand::Segment64 seg={};
    if (state->ptDMA->gen64IOVMSegments(&dmaOffset,&seg,&segmentCount)!=kIOReturnSuccess ||
        segmentCount!=1 || seg.fLength<state->ptBytes || dmaOffset<state->ptBytes ||
        (seg.fIOVMAddr & (PageBytes-1u)) ||
        seg.fIOVMAddr>((1ULL<<IntelDmaAddressBits)-state->ptBytes)) {
        setProperty("GTScanoutState","table-dma-segment-refused");
        releaseScanout();
        return false;
    }
    state->ptBus=seg.fIOVMAddr;

    // READ the GGTT PTEs naming the scanout pages. igGtScanoutPlanMap() proved
    // the run lies inside the table installGGTT() measured and misses the run
    // it reserved for the pool; this loop never stores to the GGTT.
    if ((UInt64)GgttTableOffset+
        ((UInt64)plan.firstGgttIndex+plan.pageCount)*sizeof(UInt64)>fBarBytes) {
        setProperty("GTScanoutState","ggtt-read-outside-bar");
        releaseScanout();
        return false;
    }
    const vm_size_t pteBytes=(vm_size_t)plan.pageCount*sizeof(IGGtScanU64);
    IGGtScanU64 *ggttPtes=reinterpret_cast<IGGtScanU64 *>(IOMalloc(pteBytes));
    if (!ggttPtes) {
        setProperty("GTScanoutState","pte-scratch-allocation-failed");
        releaseScanout();
        return false;
    }
    volatile UInt64 *ggtt=reinterpret_cast<volatile UInt64 *>(fMMIO+GgttTableOffset);
    for (UInt32 i=0;i<plan.pageCount;++i) ggttPtes[i]=ggtt[plan.firstGgttIndex+i];
    scanoutBarrier();
    // fPatIndex is the existing UC entry claimEngine() found; Linux gives the
    // adopted initial framebuffer I915_CACHE_NONE, which is UC on TGL.
    const UInt32 leafVerdict=igGtScanoutBuildLeaves(
        ggttPtes,plan.pageCount,fPatIndex,
        reinterpret_cast<IGGtScanU64 *>(state->ptCPU),
        plan.ptCount*IG_GTSCAN_PT_LEAVES_GEN8_PLUS_512);
    IOFree(ggttPtes,pteBytes);
    setProperty("GTScanoutLeafVerdict",(UInt64)leafVerdict,32);
    if (!igGtScanoutPteSucceeded(leafVerdict)) {
        setProperty("GTScanoutState","ggtt-pte-refused");
        releaseScanout();
        return false;
    }
    scanoutBarrier();

    const UInt32 pdVerdict=igGtScanoutBuildPdEntries(
        &plan,state->ptBus,state->pdEntries,IG_GTSCAN_MAX_PTS_POLICY_32);
    setProperty("GTScanoutPdVerdict",(UInt64)pdVerdict,32);
    if (!igGtScanoutPteSucceeded(pdVerdict)) {
        setProperty("GTScanoutState","pd-entry-refused");
        releaseScanout();
        return false;
    }

    // Hang the tables from pd[1..]. From the first store on, releaseScanout()
    // must clear the whole window; every slot was proven zero above.
    state->installed=true;
    for (UInt32 k=0;k<plan.ptCount;++k) {
        const UInt32 slot=plan.firstPdIndex+k;
        pd[slot]=state->pdEntries[k];
        scanoutBarrier();
        if (pd[slot]!=state->pdEntries[k]) {
            setProperty("GTScanoutState","pd-readback-mismatch");
            releaseScanout();
            return false;
        }
    }

    setProperty("GTScanoutMapped",true);
    setProperty("GTScanoutPageCount",(UInt64)plan.pageCount,32);
    setProperty("GTScanoutTableCount",(UInt64)plan.ptCount,32);
    setProperty("GTScanoutState","inherited-scanout-borrowed-private-pd");
    IGSCANLOG("borrowed %u scanout page(s) through %u private table(s): %ux%u, %u bytes/row",
              plan.pageCount,plan.ptCount,width,height,strideBytes);
    return true;
}

bool IntelGtBringup::releaseScanout(void)
{
    IntelGtScanoutRuntimeState *state=fScanoutRuntime;
    if (!state) return true;
    if (state->unsafe) return false;

    if (state->installed) {
        if (!fCPU) {
            state->unsafe=true;
            setProperty("GTScanoutReleaseState","restore-pd-unavailable-quarantined");
            return false;
        }
        volatile UInt64 *pd=reinterpret_cast<volatile UInt64 *>(fCPU+PdPoolOffset);
        bool cleared=true;
        for (UInt32 k=0;k<state->plan.ptCount;++k) {
            const UInt32 slot=state->plan.firstPdIndex+k;
            pd[slot]=0ull;
            scanoutBarrier();
            if (pd[slot]!=0ull) cleared=false;
        }
        if (!cleared) {
            // A PD slot may still name these tables. Keep them alive.
            state->unsafe=true;
            setProperty("GTScanoutReleaseState","restore-pd-refused-quarantined");
            setProperty("GTScanoutQuarantined",true);
            return false;
        }
        state->installed=false;
        setProperty("GTScanoutMapped",false);
    }

    releaseScanoutStorage(state);
    fScanoutRuntime=nullptr;
    setProperty("GTScanoutReleaseState","released-private-pd-cleared");
    return true;
}

IOReturn IntelGtBringup::fillScanoutRect32(UInt32 x, UInt32 y, UInt32 w, UInt32 h,
                                           UInt32 color)
{
    if (!w || !h || !fQueueLock)
        return kIOReturnBadArgument;

    IOLockLock(fQueueLock);
    IOReturn result=kIOReturnNotReady;
    IntelGtScanoutRuntimeState *state=fScanoutRuntime;
    if (fStage==4 && fCompleted && fRetired && !fFaulted && fCPU &&
        state && state->installed && !state->unsafe) {
        IGGtScanoutFill fill={};
        const UInt32 fillVerdict=igGtScanoutFillRect32(
            &state->plan,state->width,state->height,state->strideBytes,
            state->bytesPerPixel,x,y,w,h,&fill);
        setProperty("GTScanoutFillVerdict",(UInt64)fillVerdict,32);
        if (!igGtScanoutFillSucceeded(fillVerdict)) {
            result=kIOReturnBadArgument;
        } else {
            const GTBlitSurface32 dest={fill.gpuAddress,fill.bytes,fill.pitch};
            if (++fSequence==0u) ++fSequence;
            if (!gtEncodeFill32(reinterpret_cast<UInt32 *>(fCPU+BatchPoolOffset),16,
                                dest,fill.width,fill.height,color,
                                FenceGpuAddress,fSequence)) {
                result=kIOReturnBadArgument;
            } else if (!submitBatch(fSequence)) {
                fFaulted=true;
                result=kIOReturnIOError;
            } else {
                result=kIOReturnSuccess;
            }
        }
    }
    IOLockUnlock(fQueueLock);
    return result;
}

IOReturn IntelGtBringup::scanoutPlaneSize32(UInt32 *outWidth, UInt32 *outHeight)
{
    if (!outWidth || !outHeight || !fQueueLock)
        return kIOReturnBadArgument;
    *outWidth=0u;
    *outHeight=0u;

    // Same lock and state as fillScanoutRect32(), which re-checks the rectangle
    // against these numbers. 4 is the highest iggt-bringup value start()
    // accepts (IntelGtBringup.cpp), so >=4 is the stage-4 gate. Reads only.
    IOLockLock(fQueueLock);
    IOReturn result=kIOReturnNotReady;
    const IntelGtScanoutRuntimeState *state=fScanoutRuntime;
    if (fStage>=4 && state && state->presentOk && state->installed && !state->unsafe) {
        *outWidth=state->width;
        *outHeight=state->height;
        result=kIOReturnSuccess;
    }
    IOLockUnlock(fQueueLock);
    return result;
}

bool IntelGtBringup::drawScanoutProbe(void)
{
    // start() calls this once at stage 4, after submitCopy() has completed and
    // retired the self-test copy. A refusal leaves the panel untouched.
    const IOReturn ret=fillScanoutRect32(ProbeX,ProbeY,ProbeWidth,ProbeHeight,ProbeColor);
    setProperty("GTScanoutFillIOReturn",(UInt64)(UInt32)ret,32);
    const bool done=(ret==kIOReturnSuccess);
    setProperty("GTScanoutFillCompleted",done);
    if (done) {
        IGSCANLOG("BCS0 filled %ux%u at (%u,%u) of the inherited scanout; fence and CSB retired",
                  ProbeWidth,ProbeHeight,ProbeX,ProbeY);
    }
    return done;
}
