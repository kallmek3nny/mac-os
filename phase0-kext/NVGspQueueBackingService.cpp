// SPDX-License-Identifier: MIT
// Opt-in allocation + GA107 DMA mapping for GSP-RM queues.
// This service writes only memory it allocated. It performs NO GPU/MMIO writes.
#include "NVGspQueueBackingService.hpp"
#include "NVGa107PciIdentity.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define NVQMEMLOG(fmt, ...) IOLog("NVGspQueueBacking: " fmt "\n", ##__VA_ARGS__)
#define super IOService
OSDefineMetaClassAndStructors(NVGspQueueBackingService, IOService);

bool NVGspQueueBackingService::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCI=nullptr;
    fBacking=nullptr;
    fDMA=nullptr;
    fDMABound=false;
    return true;
}

void NVGspQueueBackingService::releaseResources(void)
{
    // Revoke public evidence BEFORE unbinding the DMA descriptor or dropping
    // provider ownership. These registry facts are about the live mapping and
    // queue buffer; they are not safe historical readiness receipts.
    setProperty("NVGspQueueBackingRequested",false);
    setProperty("NVGspQueueBackingPrepared",false);
    setProperty("NVGspQueueBackingDmaBound",false);
    setProperty("NVGspQueueBackingDeviceMapperResolved",false);
    setProperty("NVGspQueueBackingAllocated",false);
    setProperty("NVGspQueueBackingDeviceWritesPossible",false);
    setProperty("NVGspQueueBackingGspNotified",false);
    setProperty("NVGspQueueBackingPgspPointersWritten",false);
    setProperty("NVGspQueueBackingCachedArgsInstalled",false);
    setProperty("NVGspQueueBackingStatusQueueInitializedByHost",false);

    // Under resource pressure, even setProperty(false) may be unable to
    // replace an existing true OSBoolean in the IORegistry. Explicitly remove
    // every live readiness flag before clearing provider-owned DMA memory.
    // Absence is NOT hardware readiness; clients must require a live true
    // property from the current owner. Keep the previous false attempts for
    // diagnostics but do not depend on their success for resource revocation.
    removeProperty("NVGspQueueBackingRequested");
    removeProperty("NVGspQueueBackingPrepared");
    removeProperty("NVGspQueueBackingDmaBound");
    removeProperty("NVGspQueueBackingDeviceMapperResolved");
    removeProperty("NVGspQueueBackingAllocated");
    removeProperty("NVGspQueueBackingDeviceWritesPossible");
    removeProperty("NVGspQueueBackingGspNotified");
    removeProperty("NVGspQueueBackingPgspPointersWritten");
    removeProperty("NVGspQueueBackingCachedArgsInstalled");
    removeProperty("NVGspQueueBackingStatusQueueInitializedByHost");

    // A stale capacity, prior IOVA mapping length, successful sync verdict, or
    // composed init argument must not be read as evidence for a later provider
    // activation. Keep only NVGspQueueBackingState for failure diagnostics.
    removeProperty("NVGspQueueBackingLayoutVerdict");
    removeProperty("NVGspQueueBackingSharedBytes");
    removeProperty("NVGspQueueBackingPageCount");
    removeProperty("NVGspQueueBackingPageTableBytes");
    removeProperty("NVGspQueueBackingCommandOffset");
    removeProperty("NVGspQueueBackingStatusOffset");
    removeProperty("NVGspQueueBackingCapacity");
    removeProperty("NVGspQueueBackingDmaSegmentIOReturn");
    removeProperty("NVGspQueueBackingDmaSegmentCount");
    removeProperty("NVGspQueueBackingDmaConsumedBytes");
    removeProperty("NVGspQueueBackingDmaVerdict");
    removeProperty("NVGspQueueBackingDmaVerdictName");
    removeProperty("NVGspQueueBackingMappedBytes");
    removeProperty("NVGspQueueBackingSingleDmaSegment");
    removeProperty("NVGspQueueBackingMemoryVerdict");
    removeProperty("NVGspQueueBackingMemoryVerdictName");
    removeProperty("NVGspQueueBackingPageTableVerdict");
    removeProperty("NVGspQueueBackingPageTableEntriesWritten");
    removeProperty("NVGspQueueBackingTxHeaderVerdict");
    removeProperty("NVGspQueueBackingDmaSynchronizeIOReturn");
    removeProperty("NVGspQueueBackingInitFlags");
    removeProperty("NVGspQueueBackingInitCommandOffset");
    removeProperty("NVGspQueueBackingInitStatusOffset");
    removeProperty("NVGspQueueBackingInitElementHeaderBytes");
    removeProperty("NVGspQueueBackingInitElementMinBytes");
    removeProperty("NVGspQueueBackingInitElementMaxBytes");
    removeProperty("NVGspQueueBackingInitHeaderAlign");
    removeProperty("NVGspQueueBackingInitElementAlignShift");
    removeProperty("NVGspQueueBackingInitPageTableEntryCount");
    removeProperty("NVGspQueueBackingAddressKind");
    if (fDMABound && fDMA) {
        fDMA->clearMemoryDescriptor();
        fDMABound=false;
    }
    OSSafeReleaseNULL(fDMA);
    OSSafeReleaseNULL(fBacking);
    OSSafeReleaseNULL(fPCI);
}

void NVGspQueueBackingService::free(void)
{
    releaseResources();
    super::free();
}

IOService *NVGspQueueBackingService::probe(IOService *provider, SInt32 *score)
{
    IOService *result=super::probe(provider,score);
    if (!result) return nullptr;
    IOPCIDevice *pci=OSDynamicCast(IOPCIDevice,provider);
    if (!pci) return nullptr;
    const UInt16 vendor=pci->configRead16(kIOPCIConfigVendorID);
    const UInt16 device=pci->configRead16(kIOPCIConfigDeviceID);
    const UInt32 classReg=pci->configRead32(kIOPCIConfigRevisionID);
    const UInt8 baseClass=(UInt8)((classReg>>24)&0xffu);
    return (vendor==0x10deu && device==0x25a2u && baseClass==0x03u) ? result : nullptr;
}

void NVGspQueueBackingService::publishState(const char *state)
{
    setProperty("NVGspQueueBackingState",state ? state : "unknown");
}

bool NVGspQueueBackingService::start(IOService *provider)
{
    // Never reactivate a service with a live or partially-retained resource.
    if (fPCI || fBacking || fDMA || fDMABound) return false;

    int enabled=0;
    if (!PE_parse_boot_argn("nvgsp-queue-memory",&enabled,sizeof(enabled)) || enabled!=1)
        return false;

    // Probe is not lasting authority: re-check the actual start provider
    // BEFORE IOService activation, BAR0 mapping, or DMA allocation.
    // No PCI writes, bus-master enabling, or device memory access here.
    IOPCIDevice *candidate=OSDynamicCast(IOPCIDevice,provider);
    if (!candidate) return false;
    const UInt16 vendor=candidate->configRead16(kIOPCIConfigVendorID);
    const UInt16 device=candidate->configRead16(kIOPCIConfigDeviceID);
    const UInt32 classReg=candidate->configRead32(kIOPCIConfigRevisionID);
    if (!nvGa107PciIdentityEligible(vendor,device,classReg))
        return false;
    if (!super::start(provider)) return false;

    // Start is permitted only with no retained resources. Revoke all prior
    // activation metadata even when this attempt will fail before DMA setup.
    releaseResources();
    publishState("preparing-provider-owned-memory");
    // A requested-live state must be published successfully or this activation
    // may be mistaken for a complete candidate while its registry is partial.
    if (!setProperty("NVGspQueueBackingRequested",true)) {
        publishState("requested-evidence-publication-failed");
        releaseResources();
        registerService();
        return true;
    }
    setProperty("NVGspQueueBackingPrepared",false);
    setProperty("NVGspQueueBackingDmaBound",false);
    setProperty("NVGspQueueBackingDeviceMapperResolved",false);
    setProperty("NVGspQueueBackingAllocated",false);
    setProperty("NVGspQueueBackingDeviceWritesPossible",false);
    setProperty("NVGspQueueBackingGspNotified",false);
    setProperty("NVGspQueueBackingPgspPointersWritten",false);
    setProperty("NVGspQueueBackingCachedArgsInstalled",false);
    setProperty("NVGspQueueBackingStatusQueueInitializedByHost",false);

    // This exact-device candidate passed eligibility before super::start().
    fPCI=candidate;
    fPCI->retain();

    NVGspQueuePlan layout={};
    const UInt32 layoutVerdict=nvGspBuildQueuePlan(true,false,0u,&layout);
    setProperty("NVGspQueueBackingLayoutVerdict",(UInt64)layoutVerdict,32);
    if (layoutVerdict!=NV_GSP_QUEUE_PLAN_OK ||
        layout.pageTableEntryCount!=NV_GSP_QUEUE_NORMAL_MAX_PAGES) {
        publishState("layout-refused");
        registerService();
        return true;
    }

    // Geometry must have been exported in its entirety before a real DMA
    // allocation can later be advertised as prepared.
    const bool layoutPublished =
        setProperty("NVGspQueueBackingSharedBytes",(UInt64)layout.sharedBytes,32) &&
        setProperty("NVGspQueueBackingPageCount",(UInt64)layout.pageTableEntryCount,32) &&
        setProperty("NVGspQueueBackingPageTableBytes",(UInt64)layout.pageTableBytes,32) &&
        setProperty("NVGspQueueBackingCommandOffset",(UInt64)layout.commandQueueOffset,32) &&
        setProperty("NVGspQueueBackingStatusOffset",(UInt64)layout.statusQueueOffset,32);
    if (!layoutPublished) {
        publishState("queue-layout-publication-failed");
        releaseResources();
        registerService();
        return true;
    }

    // Kernel allocations are wired by default unless kIOMemoryPageable is
    // requested. Do NOT request physical contiguity: NVIDIA open RM explicitly
    // supports a scatter of 4 KiB system-memory pages via the table in page 0.
    fBacking=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,kIODirectionInOut,(vm_size_t)layout.sharedBytes,
        (vm_offset_t)NV_GSP_QUEUE_PAGE_BYTES);
    if (!fBacking) {
        publishState("allocation-failed");
        registerService();
        return true;
    }
    if (!setProperty("NVGspQueueBackingAllocated",true) ||
        !setProperty("NVGspQueueBackingCapacity",(UInt64)fBacking->getCapacity(),64)) {
        publishState("allocation-evidence-publication-failed");
        releaseResources();
        registerService();
        return true;
    }

    void *cpuBytes=fBacking->getBytesNoCopy();
    if (!cpuBytes || fBacking->getCapacity()<(vm_size_t)layout.sharedBytes) {
        publishState("cpu-mapping-unavailable");
        releaseResources();
        registerService();
        return true;
    }

    IOMapper *mapper=IOMapper::copyMapperForDevice(fPCI);
    if (!mapper) {
        publishState("device-mapper-unavailable");
        releaseResources();
        registerService();
        return true;
    }
    if (!setProperty("NVGspQueueBackingDeviceMapperResolved",true)) {
        mapper->release();
        publishState("mapper-evidence-publication-failed");
        releaseResources();
        registerService();
        return true;
    }

    // No guessed GA107 queue-address ceiling is imposed at this preparation
    // stage. 64 means the OutputHost64 representation is not artificially
    // truncated; a future hardware-notification stage must prove any actual
    // GSP addressing limit before installing these arguments.
    fDMA=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        64,
        (UInt64)NV_GSP_QUEUE_PAGE_BYTES,
        IODMACommand::kMapped,
        (UInt64)layout.sharedBytes,
        NV_GSP_QUEUE_PAGE_BYTES,
        mapper);
    mapper->release();

    if (!fDMA || fDMA->setMemoryDescriptor(fBacking,true)!=kIOReturnSuccess) {
        publishState("dma-bind-failed");
        releaseResources();
        registerService();
        return true;
    }
    fDMABound=true;
    if (!setProperty("NVGspQueueBackingDmaBound",true)) {
        publishState("dma-bind-evidence-publication-failed");
        releaseResources();
        registerService();
        return true;
    }

    const vm_size_t segBytes=(vm_size_t)layout.pageTableEntryCount*sizeof(IODMACommand::Segment64);
    IODMACommand::Segment64 *ioSeg=reinterpret_cast<IODMACommand::Segment64 *>(IOMalloc(segBytes));
    NVGspQueueDmaSegment *pureSeg=reinterpret_cast<NVGspQueueDmaSegment *>(IOMalloc(
        (vm_size_t)layout.pageTableEntryCount*sizeof(NVGspQueueDmaSegment)));
    NVGspDmaU64 *pageAddress=reinterpret_cast<NVGspDmaU64 *>(IOMalloc(
        (vm_size_t)layout.pageTableEntryCount*sizeof(NVGspDmaU64)));

    if (!ioSeg || !pureSeg || !pageAddress) {
        if (ioSeg) IOFree(ioSeg,segBytes);
        if (pureSeg) IOFree(pureSeg,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspQueueDmaSegment));
        if (pageAddress) IOFree(pageAddress,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspDmaU64));
        publishState("scratch-allocation-failed");
        releaseResources();
        registerService();
        return true;
    }
    bzero(ioSeg,segBytes);
    bzero(pureSeg,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspQueueDmaSegment));
    bzero(pageAddress,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspDmaU64));

    UInt64 dmaOffset=0ull;
    UInt32 segmentCount=layout.pageTableEntryCount;
    const IOReturn segRet=fDMA->gen64IOVMSegments(&dmaOffset,ioSeg,&segmentCount);
    setProperty("NVGspQueueBackingDmaSegmentIOReturn",(UInt64)(UInt32)segRet,32);
    setProperty("NVGspQueueBackingDmaSegmentCount",(UInt64)segmentCount,32);
    setProperty("NVGspQueueBackingDmaConsumedBytes",dmaOffset,64);

    bool segmentsUsable=(segRet==kIOReturnSuccess &&
                         dmaOffset==(UInt64)layout.sharedBytes &&
                         segmentCount>0u &&
                         segmentCount<=layout.pageTableEntryCount);
    if (segmentsUsable) {
        for (UInt32 i=0;i<segmentCount;++i) {
            pureSeg[i].address=ioSeg[i].fIOVMAddr;
            pureSeg[i].length=ioSeg[i].fLength;
        }
    }

    NVGspQueueDmaPlan dmaPlan={};
    UInt32 dmaVerdict=NV_GSP_QUEUE_DMA_BAD_SEGMENT_COUNT;
    if (segmentsUsable) {
        dmaVerdict=nvGspFlattenQueueDmaSegments(
            &layout,pureSeg,segmentCount,pageAddress,
            layout.pageTableEntryCount,&dmaPlan);
    }
    setProperty("NVGspQueueBackingDmaVerdict",(UInt64)dmaVerdict,32);
    setProperty("NVGspQueueBackingDmaVerdictName",nvGspQueueDmaVerdictName(dmaVerdict));

    if (!segmentsUsable || dmaVerdict!=NV_GSP_QUEUE_DMA_OK) {
        IOFree(ioSeg,segBytes);
        IOFree(pureSeg,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspQueueDmaSegment));
        IOFree(pageAddress,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspDmaU64));
        publishState("dma-segments-refused");
        releaseResources();
        registerService();
        return true;
    }

    // IOVAs are private to the provider-owned DMA plan. No raw GPU address
    // or queue-argument address is exported through the IORegistry.
    if (!setProperty("NVGspQueueBackingMappedBytes",dmaPlan.mappedBytes,64) ||
        !setProperty("NVGspQueueBackingSingleDmaSegment",
                     dmaPlan.inputWasSingleContiguousSegment)) {
        IOFree(ioSeg,segBytes);
        IOFree(pureSeg,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspQueueDmaSegment));
        IOFree(pageAddress,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspDmaU64));
        publishState("mapped-evidence-publication-failed");
        releaseResources();
        registerService();
        return true;
    }

    NVGspQueueMemoryPlan memoryPlan={};
    const UInt32 memoryVerdict=nvGspBuildQueueMemoryPlan(
        &layout,false,pageAddress,layout.pageTableEntryCount,&memoryPlan);
    setProperty("NVGspQueueBackingMemoryVerdict",(UInt64)memoryVerdict,32);
    setProperty("NVGspQueueBackingMemoryVerdictName",nvGspQueueMemoryVerdictName(memoryVerdict));
    if (memoryVerdict!=NV_GSP_QUEUE_MEMORY_OK) {
        IOFree(ioSeg,segBytes);
        IOFree(pureSeg,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspQueueDmaSegment));
        IOFree(pageAddress,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspDmaU64));
        publishState("memory-plan-refused");
        releaseResources();
        registerService();
        return true;
    }

    // From here onward every CPU store targets this service's brand-new buffer.
    // Nothing in BAR0, PGSP, PCI config space or pre-existing firmware memory is
    // modified. GSP has not been told this allocation exists.
    bzero(cpuBytes,(size_t)layout.sharedBytes);
    UInt32 tableWritten=0u;
    const UInt32 tableVerdict=nvGspWriteQueuePageTable(
        &layout,pageAddress,layout.pageTableEntryCount,
        reinterpret_cast<NVGspMemU64 *>(cpuBytes),
        layout.pageTableBytes/(UInt32)sizeof(NVGspMemU64),&tableWritten);
    setProperty("NVGspQueueBackingPageTableVerdict",(UInt64)tableVerdict,32);
    setProperty("NVGspQueueBackingPageTableEntriesWritten",(UInt64)tableWritten,32);

    NVGspMsgqTxHeaderPlan txHeader={};
    const UInt32 txVerdict=nvGspBuildMsgqTxHeader(layout.commandQueueBytes,&txHeader);
    setProperty("NVGspQueueBackingTxHeaderVerdict",(UInt64)txVerdict,32);
    if (tableVerdict!=NV_GSP_QUEUE_MEMORY_OK ||
        tableWritten!=layout.pageTableEntryCount ||
        txVerdict!=NV_GSP_QUEUE_TX_OK) {
        IOFree(ioSeg,segBytes);
        IOFree(pureSeg,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspQueueDmaSegment));
        IOFree(pageAddress,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspDmaU64));
        publishState("owned-memory-compose-failed");
        releaseResources();
        registerService();
        return true;
    }

    UInt32 *commandHeader=reinterpret_cast<UInt32 *>(
        reinterpret_cast<UInt8 *>(cpuBytes)+layout.commandQueueOffset);
    for (UInt32 i=0;i<NV_GSP_MSGQ_TX_HEADER_DWORDS;++i)
        commandHeader[i]=txHeader.dwords[i];

    // If the DMA command had to use any private/bounce storage, synchronize the
    // CPU-authored table/header into the exact memory represented by the IOVAs.
    const IOReturn syncRet=fDMA->synchronize(kIODirectionOut);
    setProperty("NVGspQueueBackingDmaSynchronizeIOReturn",(UInt64)(UInt32)syncRet,32);

    IOFree(ioSeg,segBytes);
    IOFree(pureSeg,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspQueueDmaSegment));
    IOFree(pageAddress,(vm_size_t)layout.pageTableEntryCount*sizeof(NVGspDmaU64));

    if (syncRet!=kIOReturnSuccess) {
        publishState("dma-synchronize-failed");
        releaseResources();
        registerService();
        return true;
    }

    // The exact PCI owner may disappear or be substituted while an IODMACommand
    // is bound and CPU-authored queue memory is synchronized. Validate its
    // identity again before publishing provider-owned "prepared" evidence.
    // PCI reads only; no MMIO, firmware notification or queue submission.
    const UInt16 currentVendor=fPCI->configRead16(kIOPCIConfigVendorID);
    const UInt16 currentDevice=fPCI->configRead16(kIOPCIConfigDeviceID);
    const UInt32 currentClassReg=fPCI->configRead32(kIOPCIConfigRevisionID);
    if (!nvGa107PciIdentityStableAcrossStage(
            vendor,device,classReg,currentVendor,currentDevice,currentClassReg)) {
        publishState("pci-provider-changed-before-prepared");
        releaseResources();
        registerService();
        return true;
    }

    // The candidate address remains private until a separately proven GSP
    // initialization/notification boundary exists.
    // Publication is a fail-closed evidence boundary, not a best-effort log.
    // Publish the address classification before the readiness bit and revoke
    // every live property if ANY required geometry field cannot be recorded.
    // Evaluate each write even after an earlier failure to aid diagnostics;
    // releaseResources() still withdraws the entire partial snapshot.
    bool published = true;
    published = setProperty("NVGspQueueBackingInitFlags",memoryPlan.initArgs.flags,64) && published;
    published = setProperty("NVGspQueueBackingInitCommandOffset",memoryPlan.initArgs.cmdQueueOffset,64) && published;
    published = setProperty("NVGspQueueBackingInitStatusOffset",memoryPlan.initArgs.statQueueOffset,64) && published;
    published = setProperty("NVGspQueueBackingInitElementHeaderBytes",(UInt64)memoryPlan.initArgs.queueElementHdrSize,32) && published;
    published = setProperty("NVGspQueueBackingInitElementMinBytes",(UInt64)memoryPlan.initArgs.queueElementSizeMin,32) && published;
    published = setProperty("NVGspQueueBackingInitElementMaxBytes",(UInt64)memoryPlan.initArgs.queueElementSizeMax,32) && published;
    published = setProperty("NVGspQueueBackingInitHeaderAlign",(UInt64)memoryPlan.initArgs.queueHeaderAlign,32) && published;
    published = setProperty("NVGspQueueBackingInitElementAlignShift",(UInt64)memoryPlan.initArgs.queueElementAlign,32) && published;
    published = setProperty("NVGspQueueBackingInitPageTableEntryCount",(UInt64)memoryPlan.initArgs.pageTableEntryCount,32) && published;
    published = setProperty("NVGspQueueBackingAddressKind","ga107-device-iovm-candidate-not-installed") && published;
    if (!published || !setProperty("NVGspQueueBackingPrepared",true)) {
        publishState("prepared-evidence-publication-failed");
        releaseResources();
        registerService();
        return true;
    }
    publishState("owned-memory-prepared-gsp-not-notified");
    registerService();

    NVQMEMLOG("prepared %u shared page(s); GSP not notified",
              layout.pageTableEntryCount);
    return true;
}

void NVGspQueueBackingService::stop(IOService *provider)
{
    releaseResources();
    publishState("stopped-backing-released");
    super::stop(provider);
}
