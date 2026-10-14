// SPDX-License-Identifier: MIT
// Opt-in runtime allocation and staged Intel GGTT mapping for the sink candidate.
#include "IntelSinkCandidateService.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define IGSINKLOG(fmt, ...) IOLog("IntelSinkCandidate: " fmt "\n", ##__VA_ARGS__)
OSDefineMetaClassAndStructors(IntelSinkCandidateService, IOService);

namespace {
static bool numberProperty(IOService *service, const char *key, UInt64 &value)
{
    if (!service || !key) return false;
    OSNumber *number=OSDynamicCast(OSNumber,service->getProperty(key));
    if (!number) return false;
    value=number->unsigned64BitValue();
    return true;
}
}

bool IntelSinkCandidateService::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCIDevice=nullptr;
    fBacking=nullptr;
    fIntelDMA=nullptr;
    fBarMap=nullptr;
    fMMIO=nullptr;
    fBarBytes=0;
    fSavedPtes=nullptr;
    fSavedPteCount=0;
    fMappedPteCount=0;
    fMappedFirstPte=0;
    fPrepared=false;
    fDMABound=false;
    fOpened=false;
    return true;
}

bool IntelSinkCandidateService::acceleratorPresent(void)
{
    if (!fPCIDevice) return true;
    IORegistryIterator *it=IORegistryIterator::iterateOver(
        fPCIDevice,gIOServicePlane,kIORegistryIterateRecursively);
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

void IntelSinkCandidateService::releaseGgtt(void)
{
    if (fMMIO && fSavedPtes && fMappedPteCount) {
        volatile UInt64 *pt=reinterpret_cast<volatile UInt64 *>(
            fMMIO+IG_SINK_GGTT_TABLE_BAR0_OFFSET_I915_0x800000);
        for (UInt32 i=0;i<fMappedPteCount;++i) {
            pt[fMappedFirstPte+i]=fSavedPtes[i];
            OSSynchronizeIO();
        }
        IGSINKLOG("restored %u GGTT PTE(s) at index %u",fMappedPteCount,fMappedFirstPte);
    }
    fMappedPteCount=0;
    fMappedFirstPte=0;

    if (fSavedPtes) {
        IOFree(fSavedPtes,(vm_size_t)fSavedPteCount*sizeof(UInt64));
        fSavedPtes=nullptr;
    }
    fSavedPteCount=0;

    if (fDMABound && fIntelDMA) {
        fIntelDMA->clearMemoryDescriptor();
        fDMABound=false;
    }
    OSSafeReleaseNULL(fIntelDMA);

    OSSafeReleaseNULL(fBarMap);
    fMMIO=nullptr;
    fBarBytes=0;
    if (fOpened && fPCIDevice) {
        fPCIDevice->close(this);
        fOpened=false;
    }

    setProperty("IGSinkCandidateGgttMapped",false);
    setProperty("IGSinkCandidateProducerReady",false);
    setProperty("IGSinkCandidateScanoutReady",false);
}

void IntelSinkCandidateService::releaseBacking(void)
{
    if (fPrepared && fBacking) {
        fBacking->complete(kIODirectionInOut);
        fPrepared=false;
    }
    OSSafeReleaseNULL(fBacking);
}

void IntelSinkCandidateService::publishNotReady(void)
{
    setProperty("IGSinkCandidateBackingAllocated",false);
    setProperty("IGSinkCandidateBackingContiguous",false);
    setProperty("IGSinkCandidateGgttMapRequested",false);
    setProperty("IGSinkCandidateGgttMapped",false);
    setProperty("IGSinkCandidatePeerDmaMapped",false);
    setProperty("IGSinkCandidatePlaneProgrammed",false);
    setProperty("IGSinkCandidateProducerReady",false);
    setProperty("IGSinkCandidateScanoutReady",false);
}

void IntelSinkCandidateService::free(void)
{
    releaseGgtt();
    releaseBacking();
    OSSafeReleaseNULL(fPCIDevice);
    super::free();
}

IOService *IntelSinkCandidateService::probe(IOService *provider, SInt32 *score)
{
    IOService *result=super::probe(provider,score);
    if (!result) return nullptr;
    IOPCIDevice *pci=OSDynamicCast(IOPCIDevice,provider);
    if (!pci) return nullptr;
    const UInt16 vendor=pci->configRead16(kIOPCIConfigVendorID);
    const UInt16 device=pci->configRead16(kIOPCIConfigDeviceID);
    return igIsSupportedDevice(vendor,device) ? result : nullptr;
}

bool IntelSinkCandidateService::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn("igsink-candidate",&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    fPCIDevice=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCIDevice) {
        super::stop(provider);
        return false;
    }
    fPCIDevice->retain();

    int mapValue=0;
    const bool mapRequested=PE_parse_boot_argn(
        "igsink-ggtt-map",&mapValue,sizeof(mapValue)) && mapValue==1;

    setProperty("IGSinkCandidateEnabled",true);
    publishNotReady();
    setProperty("IGSinkCandidateGgttMapRequested",mapRequested);
    setProperty("IGSinkCandidateAddressKind","physical-candidate-not-peer-mapped");

    // Consume the read-only display probe's already-tested rehearsal instead of
    // sampling display registers a third time or reimplementing its reserved-end
    // arithmetic here. A missing probe/plan is diagnostic, not a reason to touch
    // hardware or guess a geometry.
    OSDictionary *matching=IOService::serviceMatching("IntelDisplayProbe");
    IOService *display=matching
        ? IOService::waitForMatchingService(matching,IG_SINK_CANDIDATE_DISPLAY_WAIT_NS)
        : nullptr;
    OSSafeReleaseNULL(matching);

    if (!display) {
        setProperty("IGSinkCandidateState","display-probe-unavailable");
        registerService();
        return true;
    }

    UInt64 planBase=0,planStride=0,planBytes=0,inCtl=0,inSize=0,apertureBytes=0;
    const bool havePlan=
        numberProperty(display,"IGSinkPlanBase",planBase) &&
        numberProperty(display,"IGSinkPlanStride",planStride) &&
        numberProperty(display,"IGSinkPlanBytes",planBytes) &&
        numberProperty(display,"IGSinkRehearsalInCtl",inCtl) &&
        numberProperty(display,"IGSinkRehearsalInSize",inSize) &&
        numberProperty(display,"IGSinkRehearsalInApertureBytes",apertureBytes);
    display->release();

    if (!havePlan || !planBytes || planBase>0xFFFFFFFFull ||
        planStride>0xFFFFFFFFull || planBytes>0xFFFFFFFFull ||
        inCtl>0xFFFFFFFFull || inSize>0xFFFFFFFFull || !apertureBytes) {
        setProperty("IGSinkCandidateState","rehearsal-plan-unavailable");
        registerService();
        return true;
    }

    IgSinkAllocPlan plan;
    plan.base=(IGS32)planBase;
    plan.stride=(IGS32)planStride;
    plan.size=(IGS32)planBytes;
    const IGS32 width=igFbSizeWidth((IGFB32)inSize);
    const IGS32 height=igFbSizeHeight((IGFB32)inSize);
    const IGS32 bytesPerPixel=igFbBytesPerPixel(igFbFormat((IGFB32)inCtl));
    const UInt64 allocationBytes=igSinkGgttRoundUpBytes((UInt64)plan.size);

    setProperty("IGSinkCandidatePlanBase",(UInt64)plan.base,32);
    setProperty("IGSinkCandidatePlanStride",(UInt64)plan.stride,32);
    setProperty("IGSinkCandidatePlanBytes",(UInt64)plan.size,32);
    setProperty("IGSinkCandidateAllocationBytes",allocationBytes,64);
    setProperty("IGSinkCandidateWidth",(UInt64)width,32);
    setProperty("IGSinkCandidateHeight",(UInt64)height,32);
    setProperty("IGSinkCandidateBytesPerPixel",(UInt64)bytesPerPixel,32);
    setProperty("IGSinkCandidateBoundApertureBytes",apertureBytes,64);

    if (!allocationBytes || allocationBytes>0xFFFFFFFFull) {
        setProperty("IGSinkCandidateState","backing-size-refused");
        registerService();
        return true;
    }

    // Physically contiguous backing remains opt-in. Round the allocation to a
    // whole GGTT page so the final PTE never names bytes outside this descriptor.
    fBacking=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut|kIOMemoryPhysicallyContiguous,
        (mach_vm_size_t)allocationBytes,
        (mach_vm_address_t)IG_SINK_CANDIDATE_PHYS_MASK);
    if (!fBacking) {
        setProperty("IGSinkCandidateState","backing-allocation-failed");
        registerService();
        return true;
    }
    setProperty("IGSinkCandidateBackingAllocated",true);

    if (fBacking->prepare(kIODirectionInOut)!=kIOReturnSuccess) {
        setProperty("IGSinkCandidateState","backing-prepare-failed");
        releaseBacking();
        registerService();
        return true;
    }
    fPrepared=true;

    void *bytes=fBacking->getBytesNoCopy();
    if (!bytes || fBacking->getCapacity()<(vm_size_t)allocationBytes) {
        setProperty("IGSinkCandidateState","backing-pointer-failed");
        releaseBacking();
        registerService();
        return true;
    }
    bzero(bytes,(size_t)allocationBytes);

    IOByteCount segmentBytes=0;
    const UInt64 physical=(UInt64)fBacking->getPhysicalSegment(
        0,&segmentBytes,kIOMemoryMapperNone);
    const bool contiguous=physical!=0 && segmentBytes>=(IOByteCount)allocationBytes;
    setProperty("IGSinkCandidatePhysicalAddress",physical,64);
    setProperty("IGSinkCandidatePhysicalSegmentBytes",(UInt64)segmentBytes,64);
    setProperty("IGSinkCandidateBackingContiguous",contiguous);

    IGSinkCandidate candidate;
    IGS32 candidateVerdict=igSinkCandidateBuild(
        &plan,width,height,bytesPerPixel,
        physical,(IGS64)allocationBytes,contiguous,
        apertureBytes,
        0,0,0,
        &candidate);

    setProperty("IGSinkCandidateVerdict",igSinkCandidateVerdictName(candidateVerdict));
    setProperty("IGSinkCandidatePublishVerdict",(UInt64)candidate.publishVerdict,32);
    setProperty("IGSinkCandidateGgttMapped",candidate.ggttMapped!=0);
    setProperty("IGSinkCandidatePeerDmaMapped",candidate.peerDmaMapped!=0);
    setProperty("IGSinkCandidatePlaneProgrammed",candidate.planeProgrammed!=0);
    setProperty("IGSinkCandidateProducerReady",candidate.producerReady!=0);
    setProperty("IGSinkCandidateScanoutReady",candidate.scanoutReady!=0);

    if (!igSinkCandidateSucceeded(candidateVerdict)) {
        setProperty("IGSinkCandidateState","candidate-validation-refused");
        IGSINKLOG("candidate refused: %s publish=%u",
                  igSinkCandidateVerdictName(candidateVerdict),candidate.publishVerdict);
        registerService();
        return true;
    }

    setProperty("IGSinkCandidateDescriptorPhysical",candidate.descriptor.busAddress,64);
    setProperty("IGSinkCandidateDescriptorExtent",candidate.descriptor.extentBytes,64);
    setProperty("IGSinkCandidateDescriptorStride",(UInt64)candidate.descriptor.stride,32);

    if (!mapRequested) {
        setProperty("IGSinkCandidateState","backing-owned-not-mapped");
        IGSINKLOG("owned candidate: phys=%llx bytes=%u ggtt=0x%08x; GGTT mapping not requested",
                  (unsigned long long)physical,plan.size,plan.base);
        registerService();
        return true;
    }

    // Hardware-write stage. Refuse if an accelerator is already attached or if
    // another service owns the PCI provider. This keeps the candidate mapper out
    // of a live graphics driver's GGTT.
    if (acceleratorPresent() || !fPCIDevice->open(this)) {
        setProperty("IGSinkCandidateState","ggtt-owner-unavailable");
        registerService();
        return true;
    }
    fOpened=true;

    IOMemoryDescriptor *bar=fPCIDevice->getDeviceMemoryWithIndex(0);
    if (bar) fBarMap=bar->map(kIOMapAnywhere|kIOMapInhibitCache);
    if (!fBarMap) {
        setProperty("IGSinkCandidateState","ggtt-bar-map-failed");
        releaseGgtt();
        registerService();
        return true;
    }
    fMMIO=reinterpret_cast<volatile UInt8 *>(fBarMap->getVirtualAddress());
    fBarBytes=fBarMap->getLength();

    const UInt16 ggc=fPCIDevice->configRead16(0x50);
    const UInt32 gttBytes=igSinkGgttTableBytesFromGgc((UInt32)ggc);
    const UInt32 ggttEntries=igSinkGgttEntriesFromGgc((UInt32)ggc);
    setProperty("IGSinkCandidateGgttTableBytes",(UInt64)gttBytes,32);
    setProperty("IGSinkCandidateGgttEntries",(UInt64)ggttEntries,32);
    if (!fMMIO || !gttBytes ||
        (UInt64)IG_SINK_GGTT_TABLE_BAR0_OFFSET_I915_0x800000+(UInt64)gttBytes>fBarBytes) {
        setProperty("IGSinkCandidateState","ggtt-table-unavailable");
        releaseGgtt();
        registerService();
        return true;
    }

    // The Intel side gets its own IOVM mapping. This address, not the physical
    // candidate address above, is what goes in the Intel GGTT PTEs.
    fIntelDMA=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,46,(UInt64)allocationBytes,
        IODMACommand::kMapped,(UInt64)allocationBytes,
        IG_SINK_GGTT_PAGE_BYTES_I915_4096);
    if (!fIntelDMA || fIntelDMA->setMemoryDescriptor(fBacking,true)!=kIOReturnSuccess) {
        setProperty("IGSinkCandidateState","intel-dma-bind-failed");
        releaseGgtt();
        registerService();
        return true;
    }
    fDMABound=true;

    UInt64 dmaOffset=0;
    UInt32 segmentCount=1;
    IODMACommand::Segment64 dmaSegment={};
    if (fIntelDMA->gen64IOVMSegments(&dmaOffset,&dmaSegment,&segmentCount)!=kIOReturnSuccess ||
        segmentCount!=1 || dmaOffset<allocationBytes ||
        dmaSegment.fLength<allocationBytes) {
        setProperty("IGSinkCandidateState","intel-dma-segment-failed");
        releaseGgtt();
        registerService();
        return true;
    }
    setProperty("IGSinkCandidateIntelDmaAddress",dmaSegment.fIOVMAddr,64);
    setProperty("IGSinkCandidateIntelDmaSegmentBytes",dmaSegment.fLength,64);

    IGSinkGgttPlan ggttPlan={};
    const UInt32 ggttVerdict=igSinkGgttPlan(
        (UInt64)plan.base,(UInt64)plan.size,dmaSegment.fIOVMAddr,dmaSegment.fLength,
        ggttEntries,&ggttPlan);
    setProperty("IGSinkCandidateGgttPlanVerdict",igSinkGgttPlanVerdictName(ggttVerdict));
    if (!igSinkGgttPlanSucceeded(ggttVerdict)) {
        setProperty("IGSinkCandidateState","ggtt-plan-refused");
        releaseGgtt();
        registerService();
        return true;
    }
    setProperty("IGSinkCandidateGgttFirstPte",(UInt64)ggttPlan.firstPte,32);
    setProperty("IGSinkCandidateGgttPteCount",(UInt64)ggttPlan.pteCount,32);
    setProperty("IGSinkCandidateGgttMappedBytes",ggttPlan.mappedBytes,64);

    const vm_size_t savedBytes=(vm_size_t)ggttPlan.pteCount*sizeof(UInt64);
    fSavedPtes=reinterpret_cast<UInt64 *>(IOMalloc(savedBytes));
    if (!fSavedPtes) {
        setProperty("IGSinkCandidateState","ggtt-save-allocation-failed");
        releaseGgtt();
        registerService();
        return true;
    }
    fSavedPteCount=ggttPlan.pteCount;
    fMappedFirstPte=ggttPlan.firstPte;

    volatile UInt64 *pt=reinterpret_cast<volatile UInt64 *>(
        fMMIO+IG_SINK_GGTT_TABLE_BAR0_OFFSET_I915_0x800000);

    // Read and save the whole target range BEFORE any write. A single present
    // entry refuses the entire operation, so an existing mapping is never
    // partially overwritten.
    for (UInt32 i=0;i<ggttPlan.pteCount;++i) {
        const UInt64 old=pt[ggttPlan.firstPte+i];
        fSavedPtes[i]=old;
        if (igSinkGgttPtePresent(old)) {
            setProperty("IGSinkCandidateGgttOccupiedIndex",(UInt64)(ggttPlan.firstPte+i),32);
            setProperty("IGSinkCandidateState","ggtt-target-occupied");
            releaseGgtt();
            registerService();
            return true;
        }
    }

    bool writeOk=true;
    for (UInt32 i=0;i<ggttPlan.pteCount;++i) {
        const UInt64 pte=igSinkGgttPteForPage(ggttPlan.intelDmaBase,i);
        if (!pte) { writeOk=false; break; }
        pt[ggttPlan.firstPte+i]=pte;
        OSSynchronizeIO();
        fMappedPteCount=i+1u;
        if (pt[ggttPlan.firstPte+i]!=pte) { writeOk=false; break; }
    }
    if (!writeOk || fMappedPteCount!=ggttPlan.pteCount) {
        setProperty("IGSinkCandidateState","ggtt-write-readback-failed");
        releaseGgtt();
        registerService();
        return true;
    }

    // Fresh previously-nonpresent addresses have no valid stale GGTT TLB entry.
    // TGL's uncached GGTT path does not need the pre-Gen11 GFX_FLSH_CNTL write.
    candidateVerdict=igSinkCandidateBuild(
        &plan,width,height,bytesPerPixel,
        physical,(IGS64)allocationBytes,contiguous,
        apertureBytes,
        1,0,0,
        &candidate);
    if (!igSinkCandidateSucceeded(candidateVerdict)) {
        setProperty("IGSinkCandidateState","post-map-candidate-refused");
        releaseGgtt();
        registerService();
        return true;
    }

    setProperty("IGSinkCandidateGgttMapped",true);
    setProperty("IGSinkCandidatePeerDmaMapped",false);
    setProperty("IGSinkCandidatePlaneProgrammed",false);
    setProperty("IGSinkCandidateProducerReady",candidate.producerReady!=0);
    setProperty("IGSinkCandidateScanoutReady",candidate.scanoutReady!=0);
    setProperty("IGSinkCandidateState","ggtt-mapped-peer-not-mapped");
    IGSINKLOG("GGTT mapped %u page(s) at 0x%08x -> Intel IOVM %llx; peer DMA and plane remain disabled",
              ggttPlan.pteCount,plan.base,(unsigned long long)ggttPlan.intelDmaBase);

    registerService();
    return true;
}

void IntelSinkCandidateService::stop(IOService *provider)
{
    releaseGgtt();
    releaseBacking();
    OSSafeReleaseNULL(fPCIDevice);
    super::stop(provider);
}
