// SPDX-License-Identifier: MIT
// Opt-in allocation-only GSP boot-argument rehearsal. NO GPU/MMIO writes.
#include "NVGspBootArgsRehearsalService.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define NVBOOTARGSLOG(fmt, ...) IOLog("NVGspBootArgsRehearsal: " fmt "\n", ##__VA_ARGS__)
#define super IOService
OSDefineMetaClassAndStructors(NVGspBootArgsRehearsalService, IOService);

namespace {
static const UInt64 kQueueBackingWaitNs = 2000000000ull;

static bool numberProperty(IOService *service, const char *key, UInt64 &value)
{
    if (!service || !key) return false;
    OSNumber *number=OSDynamicCast(OSNumber,service->getProperty(key));
    if (!number) return false;
    value=number->unsigned64BitValue();
    return true;
}

static bool trueProperty(IOService *service, const char *key)
{
    if (!service || !key) return false;
    OSBoolean *value=OSDynamicCast(OSBoolean,service->getProperty(key));
    return value && value->isTrue();
}

static bool bindOnePage(IOPCIDevice *pci,
                        IOBufferMemoryDescriptor *memory,
                        IODMACommand **outDma,
                        bool *outBound,
                        UInt64 *outAddress)
{
    if (!pci || !memory || !outDma || !outBound || !outAddress) return false;
    *outDma=nullptr;
    *outBound=false;
    *outAddress=0ull;

    IOMapper *mapper=IOMapper::copyMapperForDevice(pci);
    if (!mapper) return false;

    IODMACommand *dma=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        64,
        (UInt64)NV_GSP_CACHED_ARGS_PAGE_BYTES,
        IODMACommand::kMapped,
        (UInt64)NV_GSP_CACHED_ARGS_PAGE_BYTES,
        NV_GSP_QUEUE_PAGE_BYTES,
        mapper);
    mapper->release();
    if (!dma) return false;

    if (dma->setMemoryDescriptor(memory,true)!=kIOReturnSuccess) {
        dma->release();
        return false;
    }

    UInt64 offset=0ull;
    UInt32 count=1u;
    IODMACommand::Segment64 segment={};
    const IOReturn ret=dma->gen64IOVMSegments(&offset,&segment,&count);
    if (ret!=kIOReturnSuccess || count!=1u ||
        offset!=(UInt64)NV_GSP_CACHED_ARGS_PAGE_BYTES ||
        segment.fLength<(UInt64)NV_GSP_CACHED_ARGS_PAGE_BYTES ||
        !segment.fIOVMAddr ||
        (segment.fIOVMAddr&(NV_GSP_QUEUE_PAGE_BYTES-1u))!=0ull) {
        dma->clearMemoryDescriptor();
        dma->release();
        return false;
    }

    *outDma=dma;
    *outBound=true;
    *outAddress=segment.fIOVMAddr;
    return true;
}
}

bool NVGspBootArgsRehearsalService::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCI=nullptr;
    fRmArgs=nullptr;
    fLibosList=nullptr;
    fRmArgsDMA=nullptr;
    fLibosDMA=nullptr;
    fRmArgsBound=false;
    fLibosBound=false;
    return true;
}

void NVGspBootArgsRehearsalService::releaseResources(void)
{
    if (fLibosBound && fLibosDMA) {
        fLibosDMA->clearMemoryDescriptor();
        fLibosBound=false;
    }
    OSSafeReleaseNULL(fLibosDMA);

    if (fRmArgsBound && fRmArgsDMA) {
        fRmArgsDMA->clearMemoryDescriptor();
        fRmArgsBound=false;
    }
    OSSafeReleaseNULL(fRmArgsDMA);

    OSSafeReleaseNULL(fLibosList);
    OSSafeReleaseNULL(fRmArgs);
    OSSafeReleaseNULL(fPCI);
}

void NVGspBootArgsRehearsalService::free(void)
{
    releaseResources();
    super::free();
}

IOService *NVGspBootArgsRehearsalService::probe(IOService *provider, SInt32 *score)
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

void NVGspBootArgsRehearsalService::publishState(const char *state)
{
    setProperty("NVGspBootArgsState",state ? state : "unknown");
}

bool NVGspBootArgsRehearsalService::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn("nvgsp-bootargs-memory",&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    setProperty("NVGspBootArgsRequested",true);
    setProperty("NVGspBootArgsDeviceWritesPossible",false);
    setProperty("NVGspBootArgsMailboxWritten",false);
    setProperty("NVGspBootArgsGspNotified",false);
    setProperty("NVGspBootArgsBootReady",false);
    setProperty("NVGspBootArgsSyntheticOptionalFieldsZero",true);

    fPCI=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCI) {
        publishState("no-pci-provider");
        registerService();
        return true;
    }
    fPCI->retain();

    OSDictionary *matching=IOService::serviceMatching("NVGspQueueBackingService");
    IOService *queue=matching
        ? IOService::waitForMatchingService(matching,kQueueBackingWaitNs)
        : nullptr;
    OSSafeReleaseNULL(matching);
    if (!queue) {
        publishState("queue-backing-unavailable");
        registerService();
        return true;
    }

    UInt64 shared=0,flags=0,cmdOff=0,statusOff=0,hdr=0,minBytes=0,maxBytes=0;
    UInt64 headerAlign=0,elementAlign=0,pteCount=0;
    const bool queueReady=trueProperty(queue,"NVGspQueueBackingPrepared") &&
        numberProperty(queue,"NVGspQueueBackingCandidateSharedMemGpuAddress",shared) &&
        numberProperty(queue,"NVGspQueueBackingInitFlags",flags) &&
        numberProperty(queue,"NVGspQueueBackingInitCommandOffset",cmdOff) &&
        numberProperty(queue,"NVGspQueueBackingInitStatusOffset",statusOff) &&
        numberProperty(queue,"NVGspQueueBackingInitElementHeaderBytes",hdr) &&
        numberProperty(queue,"NVGspQueueBackingInitElementMinBytes",minBytes) &&
        numberProperty(queue,"NVGspQueueBackingInitElementMaxBytes",maxBytes) &&
        numberProperty(queue,"NVGspQueueBackingInitHeaderAlign",headerAlign) &&
        numberProperty(queue,"NVGspQueueBackingInitElementAlignShift",elementAlign) &&
        numberProperty(queue,"NVGspQueueBackingInitPageTableEntryCount",pteCount);
    queue->release();

    if (!queueReady || !shared || flags>~0ull || cmdOff>~0ull || statusOff>~0ull ||
        hdr>0xffffffffull || minBytes>0xffffffffull || maxBytes>0xffffffffull ||
        headerAlign>0xffffffffull || elementAlign>0xffffffffull || pteCount>0xffffffffull) {
        publishState("queue-init-args-unavailable");
        registerService();
        return true;
    }

    NVGspQueueInitArguments queueArgs={};
    queueArgs.flags=flags;
    queueArgs.sharedMemPhysAddr=shared;
    queueArgs.cmdQueueOffset=cmdOff;
    queueArgs.statQueueOffset=statusOff;
    queueArgs.queueElementHdrSize=(UInt32)hdr;
    queueArgs.queueElementSizeMin=(UInt32)minBytes;
    queueArgs.queueElementSizeMax=(UInt32)maxBytes;
    queueArgs.queueHeaderAlign=(UInt32)headerAlign;
    queueArgs.queueElementAlign=(UInt32)elementAlign;
    queueArgs.pageTableEntryCount=(UInt32)pteCount;

    // Open RM uses one 4 KiB contiguous cached sysmem page for RMARGS and one
    // 4 KiB contiguous uncached sysmem page for the LibOS memory-region list.
    // These are private rehearsal allocations; they are never installed into
    // a live GSP bootstrap path by this service.
    fRmArgs=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIODirectionInOut|kIOMemoryPhysicallyContiguous,
        NV_GSP_CACHED_ARGS_PAGE_BYTES,
        NV_GSP_QUEUE_PAGE_BYTES);
    fLibosList=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMapInhibitCache,
        NV_GSP_LIBOS_INIT_LIST_BYTES,
        NV_GSP_QUEUE_PAGE_BYTES);
    if (!fRmArgs || !fLibosList) {
        publishState("bootargs-allocation-failed");
        releaseResources();
        registerService();
        return true;
    }

    UInt8 *rmBytes=reinterpret_cast<UInt8 *>(fRmArgs->getBytesNoCopy());
    UInt8 *listBytes=reinterpret_cast<UInt8 *>(fLibosList->getBytesNoCopy());
    if (!rmBytes || !listBytes ||
        fRmArgs->getCapacity()<NV_GSP_CACHED_ARGS_PAGE_BYTES ||
        fLibosList->getCapacity()<NV_GSP_LIBOS_INIT_LIST_BYTES) {
        publishState("bootargs-cpu-mapping-failed");
        releaseResources();
        registerService();
        return true;
    }

    UInt64 rmGpu=0ull, listGpu=0ull;
    if (!bindOnePage(fPCI,fRmArgs,&fRmArgsDMA,&fRmArgsBound,&rmGpu) ||
        !bindOnePage(fPCI,fLibosList,&fLibosDMA,&fLibosBound,&listGpu)) {
        publishState("bootargs-dma-map-failed");
        releaseResources();
        registerService();
        return true;
    }

    bzero(rmBytes,NV_GSP_CACHED_ARGS_PAGE_BYTES);
    bzero(listBytes,NV_GSP_LIBOS_INIT_LIST_BYTES);

    NVGspCachedArguments *cached=reinterpret_cast<NVGspCachedArguments *>(rmBytes);
    cached->magic=NV_GSP_CACHED_ARGS_MAGIC;
    cached->size=(NVGspArgsU16)sizeof(NVGspCachedArguments);
    cached->flags=0ull;
    cached->gpuInstance=0u;

    NVGspCachedArgsOverlayPlan overlay={};
    const UInt32 overlayVerdict=nvGspApplyCachedArgsQueueOverlay(
        rmBytes,NV_GSP_CACHED_ARGS_PAGE_BYTES,rmGpu,&queueArgs,&overlay);
    setProperty("NVGspBootArgsRmArgsOverlayVerdict",(UInt64)overlayVerdict,32);
    setProperty("NVGspBootArgsRmArgsOverlayVerdictName",nvGspCachedArgsVerdictName(overlayVerdict));
    if (overlayVerdict!=NV_GSP_CACHED_ARGS_OK) {
        publishState("rmargs-overlay-refused");
        releaseResources();
        registerService();
        return true;
    }

    NVGspLibosInitPlan listPlan={};
    const UInt32 listVerdict=nvGspApplyRmArgsRegion(
        reinterpret_cast<NVGspLibosMemoryRegion *>(listBytes),
        NV_GSP_LIBOS_INIT_LIST_BYTES,
        rmGpu,NV_GSP_CACHED_ARGS_PAGE_BYTES,&listPlan);
    setProperty("NVGspBootArgsLibosListVerdict",(UInt64)listVerdict,32);
    setProperty("NVGspBootArgsLibosListVerdictName",nvGspLibosInitVerdictName(listVerdict));
    if (listVerdict!=NV_GSP_LIBOS_INIT_OK) {
        publishState("libos-list-refused");
        releaseResources();
        registerService();
        return true;
    }

    const IOReturn rmSync=fRmArgsDMA->synchronize(kIODirectionOut);
    const IOReturn listSync=fLibosDMA->synchronize(kIODirectionOut);
    setProperty("NVGspBootArgsRmArgsSyncIOReturn",(UInt64)(UInt32)rmSync,32);
    setProperty("NVGspBootArgsLibosSyncIOReturn",(UInt64)(UInt32)listSync,32);
    if (rmSync!=kIOReturnSuccess || listSync!=kIOReturnSuccess) {
        publishState("bootargs-synchronize-failed");
        releaseResources();
        registerService();
        return true;
    }

    NVGspLibosMailboxPlan mailbox={};
    const UInt32 mailboxVerdict=nvGspPlanLibosMailboxWrites(listGpu,&mailbox);
    setProperty("NVGspBootArgsMailboxPlanVerdict",(UInt64)mailboxVerdict,32);
    setProperty("NVGspBootArgsMailboxPlanVerdictName",nvGspLibosMailboxVerdictName(mailboxVerdict));
    if (mailboxVerdict!=NV_GSP_LIBOS_MAILBOX_OK) {
        publishState("mailbox-plan-refused");
        releaseResources();
        registerService();
        return true;
    }

    setProperty("NVGspBootArgsRmArgsGpuAddress",rmGpu,64);
    setProperty("NVGspBootArgsLibosListGpuAddress",listGpu,64);
    setProperty("NVGspBootArgsRmArgsBytes",(UInt64)NV_GSP_CACHED_ARGS_PAGE_BYTES,32);
    setProperty("NVGspBootArgsLibosListBytes",(UInt64)NV_GSP_LIBOS_INIT_LIST_BYTES,32);
    setProperty("NVGspBootArgsRmArgsIndex",(UInt64)listPlan.rmArgsIndex,32);
    setProperty("NVGspBootArgsTerminatorIndex",(UInt64)listPlan.terminatorIndex,32);
    setProperty("NVGspBootArgsMailbox0Register",(UInt64)mailbox.writes[0].reg,32);
    setProperty("NVGspBootArgsMailbox0Value",(UInt64)mailbox.writes[0].value,32);
    setProperty("NVGspBootArgsMailbox1Register",(UInt64)mailbox.writes[1].reg,32);
    setProperty("NVGspBootArgsMailbox1Value",(UInt64)mailbox.writes[1].value,32);
    setProperty("NVGspBootArgsQueueSharedMemGpuAddress",shared,64);
    setProperty("NVGspBootArgsMemoryPrepared",true);
    setProperty("NVGspBootArgsAddressKind","ga107-device-iovm-rehearsal-not-installed");
    publishState("owned-bootargs-rehearsal-gsp-not-notified");
    registerService();

    NVBOOTARGSLOG("prepared RMARGS=%llx LibOS-list=%llx; would write mailbox %08x/%08x, but MMIO remains disabled",
                  (unsigned long long)rmGpu,
                  (unsigned long long)listGpu,
                  mailbox.writes[0].value,mailbox.writes[1].value);
    return true;
}

void NVGspBootArgsRehearsalService::stop(IOService *provider)
{
    releaseResources();
    super::stop(provider);
}
