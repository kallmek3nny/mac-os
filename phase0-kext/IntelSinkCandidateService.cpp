// SPDX-License-Identifier: MIT
// Opt-in runtime allocation plus staged Intel GGTT and NVIDIA peer mapping.
#include "IntelSinkCandidateService.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
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
    fPeerDMA=nullptr;
    fBarMap=nullptr;
    fMMIO=nullptr;
    fBarBytes=0;
    fSavedPtes=nullptr;
    fInstalledPtes=nullptr;
    fSavedPteCount=0;
    fInstalledPteCount=0;
    fGgttEntries=0;
    fGgttTxn={};
    fCommitStats={};
    fRestoreStats={};
    fCommitStats.failedPte=0xffffffffu;
    fRestoreStats.failedPte=0xffffffffu;
    fPrepareVerdict=IG_SINK_GGTT_TXN_NOT_ATTEMPTED;
    fCommitVerdict=IG_SINK_GGTT_TXN_NOT_ATTEMPTED;
    fRestoreVerdict=IG_SINK_GGTT_TXN_NOT_ATTEMPTED;
    fGgttTxnPrepared=false;
    fGgttInstalled=false;
    fQuarantined=false;
    fGgttLifetimePinned=false;
    fPrepared=false;
    fDMABound=false;
    fPeerDMABound=false;
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

bool IntelSinkCandidateService::readGgttPte(
    void *context, IGGgttU32 pteIndex, IGGgttU64 *value)
{
    IntelSinkCandidateService *service=
        reinterpret_cast<IntelSinkCandidateService *>(context);
    if (!service || !value || !service->fMMIO ||
        pteIndex>=service->fGgttEntries)
        return false;

    const UInt64 tableOffset=(UInt64)IG_SINK_GGTT_TABLE_BAR0_OFFSET_I915_0x800000;
    const UInt64 entryOffset=tableOffset+(UInt64)pteIndex*sizeof(UInt64);
    if (tableOffset>service->fBarBytes || entryOffset>service->fBarBytes ||
        (UInt64)sizeof(UInt64)>service->fBarBytes-entryOffset)
        return false;

    volatile UInt64 *entry=reinterpret_cast<volatile UInt64 *>(
        service->fMMIO+entryOffset);
    OSSynchronizeIO();
    *value=*entry;
    OSSynchronizeIO();
    return true;
}

bool IntelSinkCandidateService::writeGgttPte(
    void *context, IGGgttU32 pteIndex, IGGgttU64 value)
{
    IntelSinkCandidateService *service=
        reinterpret_cast<IntelSinkCandidateService *>(context);
    if (!service || !service->fMMIO || pteIndex>=service->fGgttEntries)
        return false;

    const UInt64 tableOffset=(UInt64)IG_SINK_GGTT_TABLE_BAR0_OFFSET_I915_0x800000;
    const UInt64 entryOffset=tableOffset+(UInt64)pteIndex*sizeof(UInt64);
    if (tableOffset>service->fBarBytes || entryOffset>service->fBarBytes ||
        (UInt64)sizeof(UInt64)>service->fBarBytes-entryOffset)
        return false;

    volatile UInt64 *entry=reinterpret_cast<volatile UInt64 *>(
        service->fMMIO+entryOffset);
    *entry=value;
    OSSynchronizeIO();
    return true;
}

void IntelSinkCandidateService::publishGgttTxnDiagnostics(void)
{
    setProperty("IGSinkCandidateGgttPrepareVerdict",
                igSinkGgttTxnVerdictName(fPrepareVerdict));
    setProperty("IGSinkCandidateGgttPrepareVerdictCode",(UInt64)fPrepareVerdict,32);
    setProperty("IGSinkCandidateGgttCommitVerdict",
                igSinkGgttTxnVerdictName(fCommitVerdict));
    setProperty("IGSinkCandidateGgttCommitVerdictCode",(UInt64)fCommitVerdict,32);
    setProperty("IGSinkCandidateGgttRestoreVerdict",
                igSinkGgttTxnVerdictName(fRestoreVerdict));
    setProperty("IGSinkCandidateGgttRestoreVerdictCode",(UInt64)fRestoreVerdict,32);

    setProperty("IGSinkCandidateGgttCommitWritesAttempted",
                (UInt64)fCommitStats.writesAttempted,32);
    setProperty("IGSinkCandidateGgttCommitWritesVerified",
                (UInt64)fCommitStats.writesVerified,32);
    setProperty("IGSinkCandidateGgttCommitRecoveryWrites",
                (UInt64)fCommitStats.recoveryWrites,32);
    setProperty("IGSinkCandidateGgttCommitFailedPte",
                (UInt64)fCommitStats.failedPte,32);
    setProperty("IGSinkCandidateGgttRestoreWritesAttempted",
                (UInt64)fRestoreStats.writesAttempted,32);
    setProperty("IGSinkCandidateGgttRestoreWritesVerified",
                (UInt64)fRestoreStats.writesVerified,32);
    setProperty("IGSinkCandidateGgttRestoreRecoveryWrites",
                (UInt64)fRestoreStats.recoveryWrites,32);
    setProperty("IGSinkCandidateGgttRestoreFailedPte",
                (UInt64)fRestoreStats.failedPte,32);

    setProperty("IGSinkCandidateQuarantined",fQuarantined);
    setProperty("IGSinkCandidateQuarantineState",fQuarantined);
    setProperty("IGSinkCandidateGgttLifetimePinned",fGgttLifetimePinned);
    setProperty("IGSinkCandidateGgttMappingSafe",fGgttInstalled && !fQuarantined);
}

void IntelSinkCandidateService::pinGgttLifetime(void)
{
    if (fGgttLifetimePinned) return;
    // Hold the service itself across the first GGTT store. That guarantees a
    // failed rollback/restore cannot later free the PCI-open owner object while
    // the BAR, DMA mapping, backing allocation or PTE image is quarantined.
    retain();
    fGgttLifetimePinned=true;
    setProperty("IGSinkCandidateGgttLifetimePinned",true);
}

void IntelSinkCandidateService::enterQuarantine(const char *reason)
{
    pinGgttLifetime();
    fQuarantined=true;
    setProperty("IGSinkCandidateQuarantined",true);
    setProperty("IGSinkCandidateQuarantineState",true);
    setProperty("IGSinkCandidateQuarantineReason",reason ? reason : "unknown");
    setProperty("IGSinkCandidateGgttMapped",false);
    setProperty("IGSinkCandidateGgttMappingSafe",false);
    setProperty("IGSinkCandidateProducerReady",false);
    setProperty("IGSinkCandidateScanoutReady",false);
    setProperty("IGSinkCandidateState","ggtt-quarantined-resource-pin");
    IGSINKLOG("GGTT quarantine entered: %s; retaining backing/DMA/BAR/PCI ownership",
              reason ? reason : "unknown");
}

void IntelSinkCandidateService::releasePeerDma(void)
{
    // Clear the flag BEFORE the release so a re-entrant teardown is a no-op.
    // fPeerDMABound implies fPeerDMA: it is set only after a successful bind
    // and cleared here before fPeerDMA is dropped.
    if (fPeerDMABound) {
        fPeerDMABound=false;
        fPeerDMA->clearMemoryDescriptor();
    }
    OSSafeReleaseNULL(fPeerDMA);
    setProperty("IGSinkCandidatePeerDmaMapped",false);
    setProperty("IGSinkCandidateProducerReady",false);
    setProperty("IGSinkCandidateScanoutReady",false);
}

bool IntelSinkCandidateService::releaseGgtt(void)
{
    // Once quarantine is entered, never retry writes during teardown. The exact
    // hardware image is no longer proven safe to mutate, so every dependency is
    // intentionally retained until reboot.
    if (fQuarantined) return false;

    if (fGgttInstalled) {
        if (!fGgttTxnPrepared || !fSavedPtes || !fInstalledPtes ||
            fSavedPteCount!=fGgttTxn.pteCount ||
            fInstalledPteCount!=fGgttTxn.pteCount) {
            enterQuarantine("ggtt-restore-metadata-missing");
            return false;
        }

        fRestoreStats={};
        fRestoreStats.failedPte=0xffffffffu;
        fRestoreVerdict=igSinkGgttTxnRestore(
            &fGgttTxn,
            fInstalledPtes,
            fSavedPtes,
            fSavedPteCount,
            &IntelSinkCandidateService::readGgttPte,
            &IntelSinkCandidateService::writeGgttPte,
            this,
            &fRestoreStats);
        publishGgttTxnDiagnostics();
        if (!igSinkGgttTxnSucceeded(fRestoreVerdict)) {
            setProperty("IGSinkCandidateGgttFailedPte",
                        (UInt64)fRestoreStats.failedPte,32);
            enterQuarantine("ggtt-restore-unproven");
            return false;
        }

        fGgttInstalled=false;
        setProperty("IGSinkCandidateGgttMapped",false);
        setProperty("IGSinkCandidateGgttMappingSafe",false);
        IGSINKLOG("verified restore of %u GGTT PTE(s) at index %u",
                  fGgttTxn.pteCount,fGgttTxn.firstPte);
    }

    if (fSavedPtes) {
        IOFree(fSavedPtes,(vm_size_t)fSavedPteCount*sizeof(UInt64));
        fSavedPtes=nullptr;
    }
    fSavedPteCount=0;
    if (fInstalledPtes) {
        IOFree(fInstalledPtes,(vm_size_t)fInstalledPteCount*sizeof(UInt64));
        fInstalledPtes=nullptr;
    }
    fInstalledPteCount=0;
    fGgttTxn={};
    fGgttTxnPrepared=false;
    fGgttEntries=0;

    // Clear the flag BEFORE the release so a re-entrant teardown is a no-op.
    // fDMABound implies fIntelDMA: it is set only after a successful bind and
    // cleared here before fIntelDMA is dropped.
    if (fDMABound) {
        fDMABound=false;
        fIntelDMA->clearMemoryDescriptor();
    }
    OSSafeReleaseNULL(fIntelDMA);

    OSSafeReleaseNULL(fBarMap);
    fMMIO=nullptr;
    fBarBytes=0;
    if (fPCIDevice) {
        if (fOpened) {
            fOpened=false;
            fPCIDevice->close(this);
        }
    }

    setProperty("IGSinkCandidateGgttMapped",false);
    setProperty("IGSinkCandidateGgttMappingSafe",false);
    setProperty("IGSinkCandidateProducerReady",false);
    setProperty("IGSinkCandidateScanoutReady",false);
    return true;
}

void IntelSinkCandidateService::releaseBacking(void)
{
    // Clear the flag BEFORE the release so a re-entrant teardown is a no-op.
    // fPrepared implies fBacking: it is set only after a successful prepare()
    // and cleared here before fBacking is dropped.
    if (fPrepared) {
        fPrepared=false;
        fBacking->complete(kIODirectionInOut);
    }
    OSSafeReleaseNULL(fBacking);
}

void IntelSinkCandidateService::publishNotReady(void)
{
    setProperty("IGSinkCandidateBackingAllocated",false);
    setProperty("IGSinkCandidateBackingContiguous",false);
    setProperty("IGSinkCandidateGgttMapRequested",false);
    setProperty("IGSinkCandidatePeerMapRequested",false);
    setProperty("IGSinkCandidateGgttMapped",false);
    setProperty("IGSinkCandidatePeerDmaMapped",false);
    setProperty("IGSinkCandidatePlaneProgrammed",false);
    setProperty("IGSinkCandidateProducerReady",false);
    setProperty("IGSinkCandidateScanoutReady",false);
    publishGgttTxnDiagnostics();
}

void IntelSinkCandidateService::free(void)
{
    // A lifetime pin makes this branch unreachable in normal IOKit teardown.
    // Keep the guard anyway: free() must never become a last-chance unverified
    // restore path that dismantles resources behind a live/uncertain PTE image.
    if (fQuarantined || fGgttInstalled || fGgttLifetimePinned) {
        IGSINKLOG("refusing unsafe free-time resource teardown");
        super::free();
        return;
    }

    (void)releaseGgtt();
    releasePeerDma();
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
    int peerValue=0;
    const bool peerRequested=PE_parse_boot_argn(
        "igsink-peer-map",&peerValue,sizeof(peerValue)) && peerValue==1;

    setProperty("IGSinkCandidateEnabled",true);
    publishNotReady();
    setProperty("IGSinkCandidateGgttMapRequested",mapRequested);
    setProperty("IGSinkCandidatePeerMapRequested",peerRequested);
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
        setProperty("IGSinkCandidateState",peerRequested
            ? "peer-map-requires-ggtt-map" : "backing-owned-not-mapped");
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
        (void)releaseGgtt();
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
        (void)releaseGgtt();
        registerService();
        return true;
    }
    fGgttEntries=ggttEntries;

    // The Intel side gets its own IOVM mapping. This address, not the physical
    // candidate address above, is what goes in the Intel GGTT PTEs.
    fIntelDMA=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,46,(UInt64)allocationBytes,
        IODMACommand::kMapped,(UInt64)allocationBytes,
        IG_SINK_GGTT_PAGE_BYTES_I915_4096);
    if (!fIntelDMA || fIntelDMA->setMemoryDescriptor(fBacking,true)!=kIOReturnSuccess) {
        setProperty("IGSinkCandidateState","intel-dma-bind-failed");
        (void)releaseGgtt();
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
        (void)releaseGgtt();
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
        (void)releaseGgtt();
        registerService();
        return true;
    }
    setProperty("IGSinkCandidateGgttFirstPte",(UInt64)ggttPlan.firstPte,32);
    setProperty("IGSinkCandidateGgttPteCount",(UInt64)ggttPlan.pteCount,32);
    setProperty("IGSinkCandidateGgttMappedBytes",ggttPlan.mappedBytes,64);

    const vm_size_t pteBytes=(vm_size_t)ggttPlan.pteCount*sizeof(UInt64);
    fSavedPtes=reinterpret_cast<UInt64 *>(IOMalloc(pteBytes));
    fInstalledPtes=reinterpret_cast<UInt64 *>(IOMalloc(pteBytes));
    if (!fSavedPtes || !fInstalledPtes) {
        setProperty("IGSinkCandidateState","ggtt-image-allocation-failed");
        (void)releaseGgtt();
        registerService();
        return true;
    }
    fSavedPteCount=ggttPlan.pteCount;
    fInstalledPteCount=ggttPlan.pteCount;
    bzero(fSavedPtes,(size_t)pteBytes);
    bzero(fInstalledPtes,(size_t)pteBytes);

    for (UInt32 i=0;i<ggttPlan.pteCount;++i) {
        const UInt64 pte=igSinkGgttPteForPage(ggttPlan.intelDmaBase,i);
        if (!pte) {
            setProperty("IGSinkCandidateState","ggtt-installed-image-invalid");
            (void)releaseGgtt();
            registerService();
            return true;
        }
        fInstalledPtes[i]=pte;
    }

    fPrepareVerdict=igSinkGgttTxnPrepare(
        ggttPlan.firstPte,
        fInstalledPtes,
        ggttPlan.pteCount,
        ggttEntries,
        fSavedPtes,
        fSavedPteCount,
        &IntelSinkCandidateService::readGgttPte,
        this,
        &fGgttTxn);
    publishGgttTxnDiagnostics();
    if (!igSinkGgttTxnSucceeded(fPrepareVerdict)) {
        if (fPrepareVerdict==IG_SINK_GGTT_TXN_TARGET_OCCUPIED) {
            for (UInt32 i=0;i<fSavedPteCount;++i) {
                if (igSinkGgttPtePresent(fSavedPtes[i])) {
                    setProperty("IGSinkCandidateGgttOccupiedIndex",
                                (UInt64)(ggttPlan.firstPte+i),32);
                    break;
                }
            }
        }
        setProperty("IGSinkCandidateState","ggtt-transaction-prepare-refused");
        (void)releaseGgtt();
        registerService();
        return true;
    }
    fGgttTxnPrepared=true;

    // Pin the service before the first possible store. Safe rollback can later
    // release resources normally, but an unproven rollback/restore keeps this
    // owner object and every mapping dependency alive until reboot.
    pinGgttLifetime();
    fCommitStats={};
    fCommitStats.failedPte=0xffffffffu;
    fCommitVerdict=igSinkGgttTxnCommit(
        &fGgttTxn,
        fInstalledPtes,
        fSavedPtes,
        fSavedPteCount,
        &IntelSinkCandidateService::readGgttPte,
        &IntelSinkCandidateService::writeGgttPte,
        this,
        &fCommitStats);
    publishGgttTxnDiagnostics();
    if (!igSinkGgttTxnSucceeded(fCommitVerdict)) {
        setProperty("IGSinkCandidateGgttFailedPte",
                    (UInt64)fCommitStats.failedPte,32);
        if (fCommitVerdict==IG_SINK_GGTT_TXN_ROLLBACK_FAILED) {
            enterQuarantine("ggtt-commit-rollback-unproven");
            registerService();
            return true;
        }
        setProperty("IGSinkCandidateState",
                    fCommitVerdict==IG_SINK_GGTT_TXN_WRITE_FAILED_ROLLED_BACK
                        ? "ggtt-commit-failed-rolled-back"
                        : "ggtt-commit-refused");
        (void)releaseGgtt();
        registerService();
        return true;
    }
    fGgttInstalled=true;
    publishGgttTxnDiagnostics();

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
        if (!releaseGgtt()) {
            registerService();
            return true;
        }
        registerService();
        return true;
    }

    setProperty("IGSinkCandidateGgttMapped",true);
    setProperty("IGSinkCandidatePeerDmaMapped",false);
    setProperty("IGSinkCandidatePlaneProgrammed",false);
    setProperty("IGSinkCandidateProducerReady",candidate.producerReady!=0);
    setProperty("IGSinkCandidateScanoutReady",candidate.scanoutReady!=0);
    publishGgttTxnDiagnostics();

    if (!peerRequested) {
        setProperty("IGSinkCandidateState","ggtt-mapped-peer-not-requested");
        IGSINKLOG("GGTT mapped %u page(s) at 0x%08x -> Intel IOVM %llx; peer DMA not requested",
                  ggttPlan.pteCount,plan.base,(unsigned long long)ggttPlan.intelDmaBase);
        registerService();
        return true;
    }

    // Find the separately matched NVIDIA-side capability service. Its provider
    // is the exact GA107M PCI function. We resolve a fresh device-specific
    // mapper here rather than sharing a pointer across services.
    OSDictionary *peerMatching=IOService::serviceMatching("NVPeerMapperProbe");
    IOService *peerProbe=peerMatching
        ? IOService::waitForMatchingService(peerMatching,IG_SINK_CANDIDATE_PEER_WAIT_NS)
        : nullptr;
    OSSafeReleaseNULL(peerMatching);
    if (!peerProbe) {
        setProperty("IGSinkCandidateState","peer-probe-unavailable");
        registerService();
        return true;
    }

    IOPCIDevice *peerPci=OSDynamicCast(IOPCIDevice,peerProbe->getProvider());
    if (!peerPci) {
        peerProbe->release();
        setProperty("IGSinkCandidateState","peer-provider-unavailable");
        registerService();
        return true;
    }

    IOMapper *peerMapper=IOMapper::copyMapperForDevice(peerPci);
    peerProbe->release();
    if (!peerMapper) {
        setProperty("IGSinkCandidateState","peer-mapper-unavailable");
        registerService();
        return true;
    }

    fPeerDMA=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        (UInt8)IG_SINK_PEER_DMA_ADDR_BITS_POLICY,
        (UInt64)allocationBytes,
        IODMACommand::kMapped,
        (UInt64)allocationBytes,
        IG_SINK_PEER_PAGE_BYTES,
        peerMapper);
    peerMapper->release();

    if (!fPeerDMA || fPeerDMA->setMemoryDescriptor(fBacking,true)!=kIOReturnSuccess) {
        setProperty("IGSinkCandidateState","peer-dma-bind-failed");
        releasePeerDma();
        registerService();
        return true;
    }
    fPeerDMABound=true;

    UInt64 peerOffset=0;
    UInt32 peerSegmentCount=1;
    IODMACommand::Segment64 peerSegment={};
    if (fPeerDMA->gen64IOVMSegments(&peerOffset,&peerSegment,&peerSegmentCount)!=kIOReturnSuccess ||
        peerSegmentCount!=1 || peerOffset<allocationBytes) {
        setProperty("IGSinkCandidateState","peer-dma-segment-failed");
        releasePeerDma();
        registerService();
        return true;
    }

    IGSinkPeerMap peerPlan={};
    const UInt32 peerVerdict=igSinkPeerPlan(
        peerSegment.fIOVMAddr,peerSegment.fLength,allocationBytes,&peerPlan);
    setProperty("IGSinkCandidatePeerMapVerdict",igSinkPeerVerdictName(peerVerdict));
    if (!igSinkPeerSucceeded(peerVerdict)) {
        setProperty("IGSinkCandidateState","peer-dma-plan-refused");
        releasePeerDma();
        registerService();
        return true;
    }

    setProperty("IGSinkCandidatePeerDmaAddress",peerPlan.iovmAddress,64);
    setProperty("IGSinkCandidatePeerDmaSegmentBytes",peerPlan.segmentBytes,64);
    setProperty("IGSinkCandidatePeerDmaMappedBytes",peerPlan.mappedBytes,64);
    setProperty("IGSinkCandidateAddressKind","peer-iovm-ready-physical-candidate-retained");

    candidateVerdict=igSinkCandidateBuild(
        &plan,width,height,bytesPerPixel,
        physical,(IGS64)allocationBytes,contiguous,
        apertureBytes,
        1,1,0,
        &candidate);
    if (!igSinkCandidateSucceeded(candidateVerdict)) {
        setProperty("IGSinkCandidateState","post-peer-candidate-refused");
        releasePeerDma();
        registerService();
        return true;
    }

    setProperty("IGSinkCandidatePeerDmaMapped",true);
    setProperty("IGSinkCandidatePlaneProgrammed",false);
    setProperty("IGSinkCandidateProducerReady",candidate.producerReady!=0);
    setProperty("IGSinkCandidateScanoutReady",candidate.scanoutReady!=0);
    setProperty("IGSinkCandidateState","ggtt-and-peer-mapped-plane-not-programmed");
    IGSINKLOG("shared sink mapped: Intel GGTT 0x%08x -> %llx, NVIDIA IOVM %llx; plane still disabled",
              plan.base,
              (unsigned long long)ggttPlan.intelDmaBase,
              (unsigned long long)peerPlan.iovmAddress);

    registerService();
    return true;
}

void IntelSinkCandidateService::stop(IOService *provider)
{
    // GGTT ownership is the first teardown gate. If exact restoration is not
    // proven, keep peer DMA, Intel DMA, BAR mapping, backing pages, snapshots and
    // the PCI-open owner alive. No later release is allowed to fall through.
    const bool ggttReleased=releaseGgtt();
    if (!ggttReleased) {
        IGSINKLOG("stop leaving quarantined sink resources pinned until reboot");
        super::stop(provider);
        return;
    }

    releasePeerDma();
    releaseBacking();
    OSSafeReleaseNULL(fPCIDevice);

    const bool dropLifetimePin=fGgttLifetimePinned;
    fGgttLifetimePinned=false;
    setProperty("IGSinkCandidateGgttLifetimePinned",false);
    super::stop(provider);

    // Last operation: dropping the extra safety retain may invoke free().
    if (dropLifetimePin) release();
}
