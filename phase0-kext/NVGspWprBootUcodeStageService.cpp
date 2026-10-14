// SPDX-License-Identifier: MIT
// Opt-in allocation-only staging of the GSP-RM boot-ucode image used by WPR metadata.
// This service writes only newly allocated system memory. It performs NO GPU/MMIO writes.
#include "NVGspWprBootUcodeStageService.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define NVWPRBOOTLOG(fmt, ...) IOLog("NVGspWprBootUcodeStage: " fmt "\n", ##__VA_ARGS__)
#define super IOService
OSDefineMetaClassAndStructors(NVGspWprBootUcodeStageService, IOService);

namespace {
static const UInt64 kContainerPolicyMaxBytes = 4ull * 1024ull * 1024ull;
static const UInt64 kAllocationPolicyMaxBytes = 4ull * 1024ull * 1024ull;
}

bool NVGspWprBootUcodeStageService::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCI=nullptr;
    fFirmware=nullptr;
    fBacking=nullptr;
    fDMA=nullptr;
    fDMABound=false;
    return true;
}

OSData *NVGspWprBootUcodeStageService::copyBootFirmwareBlob(void)
{
    OSObject *object=copyProperty(NV_GSP_WPR_BOOT_STAGE_PROPERTY);
    if (!object && fPCI)
        object=fPCI->copyProperty(NV_GSP_WPR_BOOT_STAGE_PROPERTY);
    if (!object) return nullptr;

    OSData *data=OSDynamicCast(OSData,object);
    if (!data) {
        object->release();
        return nullptr;
    }
    return data;
}

void NVGspWprBootUcodeStageService::releaseResources(void)
{
    if (fDMABound && fDMA) {
        fDMA->clearMemoryDescriptor();
        fDMABound=false;
    }
    OSSafeReleaseNULL(fDMA);
    OSSafeReleaseNULL(fBacking);
    OSSafeReleaseNULL(fFirmware);
    OSSafeReleaseNULL(fPCI);
}

void NVGspWprBootUcodeStageService::free(void)
{
    releaseResources();
    super::free();
}

IOService *NVGspWprBootUcodeStageService::probe(IOService *provider, SInt32 *score)
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

void NVGspWprBootUcodeStageService::publishState(const char *state)
{
    setProperty("NVGspWprBootUcodeStageState",state ? state : "unknown");
}

bool NVGspWprBootUcodeStageService::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn(NV_GSP_WPR_BOOT_STAGE_BOOT_ARG,&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    setProperty("NVGspWprBootUcodeStageRequested",true);
    setProperty("NVGspWprBootUcodeStageDeviceWritesPossible",false);
    setProperty("NVGspWprBootUcodeStageWprWritten",false);
    setProperty("NVGspWprBootUcodeStageBooterExecuted",false);
    setProperty("NVGspWprBootUcodeStageMailboxWritten",false);
    setProperty("NVGspWprBootUcodeStageGspNotified",false);
    setProperty("NVGspWprBootUcodeStageInputProperty",NV_GSP_WPR_BOOT_STAGE_PROPERTY);
    setProperty("NVGspWprBootUcodeStageContainerPolicyMaxBytes",kContainerPolicyMaxBytes,64);
    setProperty("NVGspWprBootUcodeStageAllocationPolicyMaxBytes",kAllocationPolicyMaxBytes,64);

    fPCI=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCI) {
        publishState("no-pci-provider");
        registerService();
        return true;
    }
    fPCI->retain();

    fFirmware=copyBootFirmwareBlob();
    if (!fFirmware) {
        publishState("boot-firmware-property-unavailable");
        registerService();
        return true;
    }

    const UInt64 containerBytes=(UInt64)fFirmware->getLength();
    const UInt8 *container=reinterpret_cast<const UInt8 *>(fFirmware->getBytesNoCopy());
    setProperty("NVGspWprBootUcodeStageContainerBytes",containerBytes,64);
    if (!container || !containerBytes) {
        publishState("boot-firmware-property-empty");
        registerService();
        return true;
    }
    if (containerBytes>kContainerPolicyMaxBytes || containerBytes>0xffffffffull) {
        publishState("boot-firmware-container-policy-refused");
        registerService();
        return true;
    }

    NVGspWprBootUcodePlan plan={};
    const UInt32 planVerdict=nvGspPlanWprBootUcode(
        container,(NVU32)containerBytes,&plan);
    setProperty("NVGspWprBootUcodeStagePlanVerdict",(UInt64)planVerdict,32);
    setProperty("NVGspWprBootUcodeStagePlanVerdictName",nvGspWprBootVerdictName(planVerdict));
    if (!nvGspWprBootSucceeded(planVerdict)) {
        publishState("boot-ucode-plan-refused");
        registerService();
        return true;
    }

    setProperty("NVGspWprBootUcodeStageSourceOffset",(UInt64)plan.sourceOffset,32);
    setProperty("NVGspWprBootUcodeStageSourceBytes",(UInt64)plan.sourceBytes,32);
    setProperty("NVGspWprBootUcodeStageAllocationBytes",(UInt64)plan.allocationBytes,32);
    setProperty("NVGspWprBootUcodeStageMonitorCodeOffset",(UInt64)plan.monitorCodeOffset,32);
    setProperty("NVGspWprBootUcodeStageMonitorDataOffset",(UInt64)plan.monitorDataOffset,32);
    setProperty("NVGspWprBootUcodeStageManifestOffset",(UInt64)plan.manifestOffset,32);

    if (!plan.allocationBytes ||
        (UInt64)plan.allocationBytes>kAllocationPolicyMaxBytes ||
        (UInt64)plan.allocationBytes>(UInt64)(vm_size_t)~(vm_size_t)0) {
        publishState("boot-ucode-allocation-policy-refused");
        registerService();
        return true;
    }

    // Match open RM's boot-ucode memdesc shape: page-aligned, cached sysmem.
    // Do not request physical contiguity. The only contiguity required later is
    // the GA107-visible IOVA, validated after device-specific DMA mapping.
    fBacking=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,kIODirectionInOut,(vm_size_t)plan.allocationBytes,
        (vm_offset_t)NV_GSP_WPR_BOOT_PAGE_BYTES);
    if (!fBacking) {
        publishState("boot-ucode-allocation-failed");
        registerService();
        return true;
    }
    setProperty("NVGspWprBootUcodeStageBackingAllocated",true);
    setProperty("NVGspWprBootUcodeStageBackingCapacity",(UInt64)fBacking->getCapacity(),64);

    UInt8 *stageBytes=reinterpret_cast<UInt8 *>(fBacking->getBytesNoCopy());
    if (!stageBytes || fBacking->getCapacity()<(vm_size_t)plan.allocationBytes) {
        publishState("boot-ucode-cpu-mapping-unavailable");
        releaseResources();
        registerService();
        return true;
    }

    // Zero the entire page-rounded allocation before copying the exact source
    // image. WPR metadata later carries sourceBytes, never allocationBytes, so
    // the zero tail is deterministic backing only and not part of firmware.
    bzero(stageBytes,(vm_size_t)plan.allocationBytes);
    bcopy(container+plan.sourceOffset,stageBytes,(vm_size_t)plan.sourceBytes);

    IOMapper *mapper=IOMapper::copyMapperForDevice(fPCI);
    if (!mapper) {
        publishState("device-mapper-unavailable");
        releaseResources();
        registerService();
        return true;
    }
    setProperty("NVGspWprBootUcodeStageDeviceMapperResolved",true);

    fDMA=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        64,
        (UInt64)plan.allocationBytes,
        IODMACommand::kMapped,
        (UInt64)plan.allocationBytes,
        NV_GSP_WPR_BOOT_PAGE_BYTES,
        mapper);
    mapper->release();

    if (!fDMA || fDMA->setMemoryDescriptor(fBacking,true)!=kIOReturnSuccess) {
        publishState("boot-ucode-dma-bind-failed");
        releaseResources();
        registerService();
        return true;
    }
    fDMABound=true;

    IODMACommand::Segment64 segments[2];
    bzero(segments,sizeof(segments));
    UInt64 dmaOffset=0ull;
    UInt32 segmentCount=2u;
    const IOReturn segRet=fDMA->gen64IOVMSegments(&dmaOffset,segments,&segmentCount);
    setProperty("NVGspWprBootUcodeStageDmaSegmentIOReturn",(UInt64)(UInt32)segRet,32);
    setProperty("NVGspWprBootUcodeStageDmaSegmentCount",(UInt64)segmentCount,32);
    setProperty("NVGspWprBootUcodeStageDmaConsumedBytes",dmaOffset,64);

    if (segRet!=kIOReturnSuccess || dmaOffset!=(UInt64)plan.allocationBytes || segmentCount!=1u) {
        publishState("boot-ucode-iova-not-contiguous");
        releaseResources();
        registerService();
        return true;
    }

    NVGspWprBootDmaPlan dmaPlan={};
    const UInt32 dmaVerdict=nvGspPlanWprBootDma(
        segments[0].fIOVMAddr,
        segments[0].fLength,
        plan.sourceBytes,
        plan.allocationBytes,
        &dmaPlan);
    setProperty("NVGspWprBootUcodeStageDmaVerdict",(UInt64)dmaVerdict,32);
    setProperty("NVGspWprBootUcodeStageDmaVerdictName",nvGspWprBootDmaVerdictName(dmaVerdict));
    if (!nvGspWprBootDmaSucceeded(dmaVerdict)) {
        publishState("boot-ucode-iova-refused");
        releaseResources();
        registerService();
        return true;
    }

    const IOReturn syncRet=fDMA->synchronize(kIODirectionOut);
    setProperty("NVGspWprBootUcodeStageDmaSynchronizeIOReturn",(UInt64)(UInt32)syncRet,32);
    if (syncRet!=kIOReturnSuccess) {
        publishState("boot-ucode-dma-synchronize-failed");
        releaseResources();
        registerService();
        return true;
    }

    setProperty("NVGspWprBootUcodeStageGpuAddress",dmaPlan.gpuAddress,64);
    setProperty("NVGspWprBootUcodeStageWprSizeOfBootloader",dmaPlan.sourceBytes,64);
    setProperty("NVGspWprBootUcodeStageWprBootloaderCodeOffset",(UInt64)plan.monitorCodeOffset,64);
    setProperty("NVGspWprBootUcodeStageWprBootloaderDataOffset",(UInt64)plan.monitorDataOffset,64);
    setProperty("NVGspWprBootUcodeStageWprBootloaderManifestOffset",(UInt64)plan.manifestOffset,64);
    setProperty("NVGspWprBootUcodeStageAddressKind","ga107-device-iovm-wpr-boot-ucode-not-installed");
    setProperty("NVGspWprBootUcodeStagePrepared",true);
    publishState("boot-ucode-staged-wpr-not-written");

    registerService();
    NVWPRBOOTLOG("staged %u-byte WPR boot ucode in %u-byte allocation at GA107 IOVA 0x%llx; WPR not written",
                 (unsigned)plan.sourceBytes,
                 (unsigned)plan.allocationBytes,
                 (unsigned long long)dmaPlan.gpuAddress);
    return true;
}

void NVGspWprBootUcodeStageService::stop(IOService *provider)
{
    releaseResources();
    super::stop(provider);
}
