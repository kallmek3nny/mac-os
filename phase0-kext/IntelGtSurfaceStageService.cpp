// SPDX-License-Identifier: MIT
// Allocation-only Tiger Lake persistent-surface runtime staging.
// No BAR mapping, MMIO write, PPGTT mutation, command submission or user ABI.
#include "IntelGtSurfaceStageService.hpp"
#include "IntelGtSurface.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define IGSURFLOG(fmt, ...) IOLog("IntelGtSurfaceStage: " fmt "\n", ##__VA_ARGS__)
OSDefineMetaClassAndStructors(IntelGtSurfaceStageService, IOService);

namespace {
constexpr UInt16 IntelVendor = 0x8086;
constexpr UInt16 TigerLakeDevice = 0x9a68;
constexpr UInt32 SurfacePages = IG_GTS_ARENA_LEAVES_448;
constexpr UInt32 SurfaceBytes = IG_GTS_ARENA_BYTES_0x1C0000;
constexpr UInt32 PageBytes = IG_GTS_PAGE_BYTES_POLICY_4096;
constexpr UInt8 IntelDmaAddressBits = 46;
}

bool IntelGtSurfaceStageService::init(OSDictionary *dict)
{
    if (!IOService::init(dict)) return false;
    fPCI=nullptr;
    fBacking=nullptr;
    fDMA=nullptr;
    fDMABound=false;
    return true;
}

void IntelGtSurfaceStageService::publishState(const char *state)
{
    if (state) setProperty("IGSurfaceStageState",state);
}

bool IntelGtSurfaceStageService::acceleratorPresent(void)
{
    if (!fPCI) return true;
    IORegistryIterator *it=IORegistryIterator::iterateOver(
        fPCI,gIOServicePlane,kIORegistryIterateRecursively);
    if (!it) return true;
    bool found=false;
    while (OSObject *entry=it->getNextObject()) {
        if (entry==this) continue;
        if (entry->metaCast("IOAccelerator") ||
            entry->metaCast("IOGraphicsAccelerator") ||
            entry->metaCast("IOGraphicsAccelerator2")) {
            found=true;
            break;
        }
    }
    it->release();
    return found;
}

void IntelGtSurfaceStageService::releaseResources(void)
{
    // Clear the flag BEFORE the release so a re-entrant teardown is a no-op.
    // fDMABound implies fDMA: it is set only after a successful bind and
    // cleared here before fDMA is dropped.
    if (fDMABound) {
        fDMABound=false;
        fDMA->clearMemoryDescriptor();
    }
    OSSafeReleaseNULL(fDMA);
    OSSafeReleaseNULL(fBacking);
    setProperty("IGSurfaceStageDmaMapped",false);
    setProperty("IGSurfaceStageReady",false);
}

void IntelGtSurfaceStageService::free(void)
{
    releaseResources();
    OSSafeReleaseNULL(fPCI);
    IOService::free();
}

IOService *IntelGtSurfaceStageService::probe(IOService *provider, SInt32 *score)
{
    IOService *result=IOService::probe(provider,score);
    if (!result) return nullptr;
    IOPCIDevice *pci=OSDynamicCast(IOPCIDevice,provider);
    if (!pci) return nullptr;
    return pci->configRead16(kIOPCIConfigVendorID)==IntelVendor &&
           pci->configRead16(kIOPCIConfigDeviceID)==TigerLakeDevice
        ? result : nullptr;
}

bool IntelGtSurfaceStageService::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn("iggt-surface-stage",&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!IOService::start(provider)) return false;

    fPCI=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCI) {
        IOService::stop(provider);
        return false;
    }
    fPCI->retain();

    setProperty("IGSurfaceStageEnabled",true);
    setProperty("IGSurfaceStageReady",false);
    setProperty("IGSurfaceStageDmaMapped",false);
    setProperty("IGSurfaceStagePpgttMutated",false);
    setProperty("IGSurfaceStageMmioMapped",false);
    setProperty("IGSurfaceStageRawIovaPublished",false);
    setProperty("IGSurfaceStageContiguityRequired",false);
    setProperty("IGSurfaceStageAllocationBytes",(UInt64)SurfaceBytes,32);
    setProperty("IGSurfaceStagePageCount",(UInt64)SurfacePages,32);

    if (fPCI->configRead16(kIOPCIConfigVendorID)!=IntelVendor ||
        fPCI->configRead16(kIOPCIConfigDeviceID)!=TigerLakeDevice) {
        publishState("exact-device-refused");
        registerService();
        return true;
    }

    // Allocation-only means ownership rules still matter. Do not create a new
    // device mapping underneath an already-attached graphics accelerator.
    if (acceleratorPresent()) {
        publishState("accelerator-present-no-stage");
        registerService();
        return true;
    }

    IGGtSurfacePlan arenaPlan={};
    const UInt32 planVerdict=igGtSurfacePlan32(
        1024u,448u,4096u,0u,&arenaPlan);
    setProperty("IGSurfaceStagePlanVerdict",(UInt64)planVerdict,32);
    if (!igGtSurfacePlanSucceeded(planVerdict) ||
        arenaPlan.pageCount!=SurfacePages ||
        arenaPlan.allocationBytes!=SurfaceBytes) {
        publishState("arena-plan-refused");
        registerService();
        return true;
    }

    // Fresh, private, page-aligned system memory. Deliberately do NOT request
    // physical contiguity; the whole point of this stage is to prove the Intel
    // device mapper can back the private PPGTT arena with scattered pages.
    fBacking=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,kIODirectionInOut,(vm_size_t)SurfaceBytes,(vm_offset_t)PageBytes);
    if (!fBacking || fBacking->getCapacity()<(vm_size_t)SurfaceBytes) {
        publishState("backing-allocation-failed");
        releaseResources();
        registerService();
        return true;
    }
    setProperty("IGSurfaceStageBackingAllocated",true);

    UInt8 *bytes=reinterpret_cast<UInt8 *>(fBacking->getBytesNoCopy());
    if (!bytes) {
        publishState("backing-cpu-map-failed");
        releaseResources();
        registerService();
        return true;
    }
    bzero(bytes,SurfaceBytes);

    IOMapper *mapper=IOMapper::copyMapperForDevice(fPCI);
    if (!mapper) {
        publishState("intel-device-mapper-unavailable");
        releaseResources();
        registerService();
        return true;
    }
    setProperty("IGSurfaceStageDeviceMapperResolved",true);

    // Cap every DMA segment at one page. The planner still accepts coalesced
    // page-multiple segments, but page-sized segments make runtime evidence
    // maximally explicit and bound scratch storage to exactly 448 entries.
    fDMA=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        IntelDmaAddressBits,
        (UInt64)PageBytes,
        IODMACommand::kMapped,
        (UInt64)SurfaceBytes,
        PageBytes,
        mapper);
    mapper->release();

    if (!fDMA || fDMA->setMemoryDescriptor(fBacking,true)!=kIOReturnSuccess) {
        publishState("intel-dma-bind-failed");
        releaseResources();
        registerService();
        return true;
    }
    fDMABound=true;

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
        publishState("dma-scratch-allocation-failed");
        releaseResources();
        registerService();
        return true;
    }
    bzero(dmaSeg,dmaSegBytes);
    bzero(normalized,normalizedBytes);
    bzero(pages,pageListBytes);

    UInt64 dmaOffset=0ull;
    UInt32 segmentCount=SurfacePages;
    const IOReturn segRet=fDMA->gen64IOVMSegments(&dmaOffset,dmaSeg,&segmentCount);
    setProperty("IGSurfaceStageDmaSegmentIOReturn",(UInt64)(UInt32)segRet,32);
    setProperty("IGSurfaceStageDmaSegmentCount",(UInt64)segmentCount,32);
    setProperty("IGSurfaceStageDmaConsumedBytes",dmaOffset,64);

    bool segmentShapeOk=(segRet==kIOReturnSuccess &&
                         dmaOffset==(UInt64)SurfaceBytes &&
                         segmentCount>0u && segmentCount<=SurfacePages);
    if (segmentShapeOk) {
        for (UInt32 i=0u;i<segmentCount;++i) {
            normalized[i].address=dmaSeg[i].fIOVMAddr;
            normalized[i].length=dmaSeg[i].fLength;
        }
    }

    UInt32 flattenVerdict=IG_GTS_FLAT_BAD_COUNT;
    if (segmentShapeOk) {
        flattenVerdict=igGtSurfaceFlattenSegments(
            normalized,segmentCount,SurfacePages,pages,SurfacePages);
    }
    setProperty("IGSurfaceStageFlattenVerdict",(UInt64)flattenVerdict,32);

    IOFree(pages,pageListBytes);
    IOFree(normalized,normalizedBytes);
    IOFree(dmaSeg,dmaSegBytes);

    if (!segmentShapeOk || !igGtSurfaceFlattenSucceeded(flattenVerdict)) {
        publishState("intel-dma-page-flatten-refused");
        releaseResources();
        registerService();
        return true;
    }

    // Keep the owned backing + device mapping alive for the next branch, where
    // handle lifetime and reversible arena-leaf installation can be added. This
    // branch intentionally has no code path capable of changing a PPGTT entry.
    setProperty("IGSurfaceStageDmaMapped",true);
    setProperty("IGSurfaceStageReady",true);
    publishState("owned-noncontiguous-intel-iommu-pages-ready");
    IGSURFLOG("staged %u pages (%u bytes) in %u Intel-domain segment(s); no PPGTT write",
              SurfacePages,SurfaceBytes,segmentCount);
    registerService();
    return true;
}

void IntelGtSurfaceStageService::stop(IOService *provider)
{
    releaseResources();
    OSSafeReleaseNULL(fPCI);
    IOService::stop(provider);
}
