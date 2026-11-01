// SPDX-License-Identifier: MIT
// Opt-in allocation + GA107 IOMMU staging of the full GSP-RM Radix3 image.
// This service writes only newly allocated system memory. It performs NO GPU/MMIO writes.
#include "NVGspRmImageStageService.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define NVRMSTAGELOG(fmt, ...) IOLog("NVGspRmImageStage: " fmt "\n", ##__VA_ARGS__)
#define super IOService
OSDefineMetaClassAndStructors(NVGspRmImageStageService, IOService);

namespace {
static const UInt64 kContainerPolicyMaxBytes = 128ull * 1024ull * 1024ull;
static const UInt64 kStagePolicyMaxBytes = 129ull * 1024ull * 1024ull;
}

bool NVGspRmImageStageService::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCI=nullptr;
    fFirmware=nullptr;
    fBacking=nullptr;
    fDMA=nullptr;
    fDMABound=false;
    return true;
}

OSData *NVGspRmImageStageService::copyRmFirmwareBlob(void)
{
    // Match the existing NV3050Probe ownership discipline: copyProperty()
    // returns a retained object. Check the service/personality first, then the
    // exact GA107 provider so a future loader may inject the property at either
    // level without teaching this staging service a filesystem dependency.
    OSObject *object=copyProperty(NV_GSP_RM_FIRMWARE_PROPERTY_NAME);
    if (!object && fPCI)
        object=fPCI->copyProperty(NV_GSP_RM_FIRMWARE_PROPERTY_NAME);
    if (!object) return nullptr;

    OSData *data=OSDynamicCast(OSData,object);
    if (!data) {
        object->release();
        return nullptr;
    }
    return data; // retained by copyProperty(); released in releaseResources().
}

void NVGspRmImageStageService::releaseResources(void)
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
    OSSafeReleaseNULL(fFirmware);
    OSSafeReleaseNULL(fPCI);
}

void NVGspRmImageStageService::free(void)
{
    releaseResources();
    super::free();
}

IOService *NVGspRmImageStageService::probe(IOService *provider, SInt32 *score)
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

void NVGspRmImageStageService::publishState(const char *state)
{
    setProperty("NVGspRmImageStageState",state ? state : "unknown");
}

bool NVGspRmImageStageService::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn(NV_GSP_RM_STAGE_BOOT_ARG,&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    setProperty("NVGspRmImageStageRequested",true);
    setProperty("NVGspRmImageStageDeviceWritesPossible",false);
    setProperty("NVGspRmImageStageWprWritten",false);
    setProperty("NVGspRmImageStageBooterExecuted",false);
    setProperty("NVGspRmImageStageMailboxWritten",false);
    setProperty("NVGspRmImageStageGspNotified",false);
    setProperty("NVGspRmImageStageContainerPolicyMaxBytes",kContainerPolicyMaxBytes,64);
    setProperty("NVGspRmImageStageAllocationPolicyMaxBytes",kStagePolicyMaxBytes,64);
    setProperty("NVGspRmImageStageInputProperty",NV_GSP_RM_FIRMWARE_PROPERTY_NAME);

    fPCI=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCI) {
        publishState("no-pci-provider");
        registerService();
        return true;
    }
    fPCI->retain();

    fFirmware=copyRmFirmwareBlob();
    if (!fFirmware) {
        publishState("rm-firmware-property-unavailable");
        registerService();
        return true;
    }

    const UInt64 containerBytes=(UInt64)fFirmware->getLength();
    const UInt8 *container=reinterpret_cast<const UInt8 *>(fFirmware->getBytesNoCopy());
    setProperty("NVGspRmImageStageContainerBytes",containerBytes,64);
    if (!container || !containerBytes) {
        publishState("rm-firmware-property-empty");
        registerService();
        return true;
    }
    if (containerBytes>kContainerPolicyMaxBytes) {
        publishState("rm-firmware-container-policy-refused");
        registerService();
        return true;
    }

    NVGspRmImagePlan plan={};
    const UInt32 planVerdict=nvGspPlanRmFirmwareImage(container,containerBytes,&plan);
    setProperty("NVGspRmImageStagePlanVerdict",(UInt64)planVerdict,32);
    setProperty("NVGspRmImageStagePlanVerdictName",nvGspRmImageVerdictName(planVerdict));
    if (planVerdict!=NV_GSP_RM_IMAGE_OK) {
        publishState("rm-firmware-plan-refused");
        registerService();
        return true;
    }

    setProperty("NVGspRmImageStageElfClass",(UInt64)plan.imageSection.elfClass,32);
    setProperty("NVGspRmImageStageSectionIndex",(UInt64)plan.imageSection.sectionIndex,32);
    setProperty("NVGspRmImageStageFwImageOffset",plan.imageSection.fileOffset,64);
    setProperty("NVGspRmImageStageFwImageBytes",plan.imageSection.fileBytes,64);
    setProperty("NVGspRmImageStageAllocationBytes",plan.stageAllocationBytes,64);
    setProperty("NVGspRmImageStageAllocationPages",plan.stageAllocationPages,64);
    setProperty("NVGspRmImageStageTableBytes",plan.radix3.tableBytes,64);
    setProperty("NVGspRmImageStageTablePages",plan.radix3.tablePages,64);
    setProperty("NVGspRmImageStageDataPages",plan.radix3.dataPages,64);
    setProperty("NVGspRmImageStageNoncontiguous",plan.stageNoncontiguous);
    setProperty("NVGspRmImageStageCached",plan.stageCached);

    if (!plan.stageAllocationBytes ||
        plan.stageAllocationBytes>kStagePolicyMaxBytes ||
        plan.stageAllocationPages>0xffffffffull ||
        plan.stageAllocationBytes>(UInt64)(vm_size_t)~(vm_size_t)0) {
        publishState("radix3-allocation-policy-refused");
        registerService();
        return true;
    }

    // open RM uses 4 KiB-aligned, NONCONTIGUOUS, cached system memory for this
    // Radix3 descriptor. Kernel allocations are wired and normally cached by
    // default, so request alignment but deliberately do not request physical
    // contiguity or an inhibited-cache mapping.
    fBacking=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,kIODirectionInOut,(vm_size_t)plan.stageAllocationBytes,
        (vm_offset_t)NV_GSP_RM_STAGE_PAGE_BYTES);
    if (!fBacking) {
        publishState("radix3-allocation-failed");
        registerService();
        return true;
    }
    setProperty("NVGspRmImageStageBackingAllocated",true);
    setProperty("NVGspRmImageStageBackingCapacity",(UInt64)fBacking->getCapacity(),64);

    UInt8 *stageBytes=reinterpret_cast<UInt8 *>(fBacking->getBytesNoCopy());
    if (!stageBytes || fBacking->getCapacity()<(vm_size_t)plan.stageAllocationBytes) {
        publishState("radix3-cpu-mapping-unavailable");
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
    setProperty("NVGspRmImageStageDeviceMapperResolved",true);

    // 64 address bits here means only "do not truncate the staging IOVA". It
    // is not a claim about GA107 GSP's eventual address-width contract. No GPU
    // consumes this address in this service.
    fDMA=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        64,
        (UInt64)NV_GSP_RM_STAGE_PAGE_BYTES,
        IODMACommand::kMapped,
        plan.stageAllocationBytes,
        NV_GSP_RM_STAGE_PAGE_BYTES,
        mapper);
    mapper->release();

    if (!fDMA || fDMA->setMemoryDescriptor(fBacking,true)!=kIOReturnSuccess) {
        publishState("radix3-dma-bind-failed");
        releaseResources();
        registerService();
        return true;
    }
    fDMABound=true;

    const UInt32 pageCapacity=(UInt32)plan.stageAllocationPages;
    const vm_size_t ioSegBytes=(vm_size_t)pageCapacity*sizeof(IODMACommand::Segment64);
    const vm_size_t pageListBytes=(vm_size_t)pageCapacity*sizeof(NVGspRadixU64);
    IODMACommand::Segment64 *ioSeg=reinterpret_cast<IODMACommand::Segment64 *>(IOMalloc(ioSegBytes));
    NVGspRadixU64 *pageAddress=reinterpret_cast<NVGspRadixU64 *>(IOMalloc(pageListBytes));
    if (!ioSeg || !pageAddress) {
        if (ioSeg) IOFree(ioSeg,ioSegBytes);
        if (pageAddress) IOFree(pageAddress,pageListBytes);
        publishState("radix3-scratch-allocation-failed");
        releaseResources();
        registerService();
        return true;
    }
    bzero(ioSeg,ioSegBytes);
    bzero(pageAddress,pageListBytes);

    UInt64 dmaOffset=0ull;
    UInt32 segmentCount=pageCapacity;
    const IOReturn segRet=fDMA->gen64IOVMSegments(&dmaOffset,ioSeg,&segmentCount);
    setProperty("NVGspRmImageStageDmaSegmentIOReturn",(UInt64)(UInt32)segRet,32);
    setProperty("NVGspRmImageStageDmaSegmentCount",(UInt64)segmentCount,32);
    setProperty("NVGspRmImageStageDmaConsumedBytes",dmaOffset,64);

    bool segmentShapeOk=(segRet==kIOReturnSuccess &&
                         dmaOffset==plan.stageAllocationBytes &&
                         segmentCount>0u && segmentCount<=pageCapacity);
    UInt32 pageIndex=0u;
    if (segmentShapeOk) {
        for (UInt32 i=0u;i<segmentCount && segmentShapeOk;++i) {
            const UInt64 address=ioSeg[i].fIOVMAddr;
            const UInt64 length=ioSeg[i].fLength;
            if (!address || !length ||
                (address & (NV_GSP_RM_STAGE_PAGE_BYTES-1u))!=0ull ||
                (length & (NV_GSP_RM_STAGE_PAGE_BYTES-1u))!=0ull) {
                segmentShapeOk=false;
                break;
            }
            const UInt64 pages=length/NV_GSP_RM_STAGE_PAGE_BYTES;
            if (pages>(UInt64)(pageCapacity-pageIndex)) {
                segmentShapeOk=false;
                break;
            }
            for (UInt64 p=0ull;p<pages;++p)
                pageAddress[pageIndex++]=address+p*NV_GSP_RM_STAGE_PAGE_BYTES;
        }
    }
    if (!segmentShapeOk || pageIndex!=pageCapacity) {
        IOFree(ioSeg,ioSegBytes);
        IOFree(pageAddress,pageListBytes);
        publishState("radix3-dma-segments-refused");
        releaseResources();
        registerService();
        return true;
    }

    UInt64 imageBytesWritten=0ull;
    const UInt32 imageVerdict=nvGspBuildEmbeddedRadix3Image(
        &plan.radix3,
        pageAddress,
        plan.stageAllocationPages,
        container+plan.imageSection.fileOffset,
        plan.imageSection.fileBytes,
        stageBytes,
        plan.stageAllocationBytes,
        &imageBytesWritten);
    setProperty("NVGspRmImageStageBuildVerdict",(UInt64)imageVerdict,32);
    setProperty("NVGspRmImageStageBuildVerdictName",nvGspRadix3VerdictName(imageVerdict));
    setProperty("NVGspRmImageStageImageBytesWritten",imageBytesWritten,64);
    if (imageVerdict!=NV_GSP_RADIX3_OK || imageBytesWritten!=plan.stageAllocationBytes) {
        IOFree(ioSeg,ioSegBytes);
        IOFree(pageAddress,pageListBytes);
        publishState("radix3-image-build-refused");
        releaseResources();
        registerService();
        return true;
    }

    // Flush CPU-authored tables + firmware bytes into the exact DMA view whose
    // page addresses were written into the Radix3 hierarchy.
    const IOReturn syncRet=fDMA->synchronize(kIODirectionOut);
    setProperty("NVGspRmImageStageDmaSynchronizeIOReturn",(UInt64)(UInt32)syncRet,32);
    if (syncRet!=kIOReturnSuccess) {
        IOFree(ioSeg,ioSegBytes);
        IOFree(pageAddress,pageListBytes);
        publishState("radix3-dma-synchronize-failed");
        releaseResources();
        registerService();
        return true;
    }

    const UInt64 rootIova=pageAddress[0];
    setProperty("NVGspRmImageStageRootGpuAddress",rootIova,64);
    setProperty("NVGspRmImageStageAddressKind","ga107-device-iovm-radix3-root-not-installed");
    setProperty("NVGspRmImageStagePrepared",true);
    publishState("radix3-staged-gsp-not-notified");

    IOFree(ioSeg,ioSegBytes);
    IOFree(pageAddress,pageListBytes);
    registerService();
    NVRMSTAGELOG("staged .fwimage=%llu bytes into %llu-page Radix3 root=%llx; GPU not notified",
                 (unsigned long long)plan.imageSection.fileBytes,
                 (unsigned long long)plan.stageAllocationPages,
                 (unsigned long long)rootIova);
    return true;
}

void NVGspRmImageStageService::stop(IOService *provider)
{
    releaseResources();
    super::stop(provider);
}
