// SPDX-License-Identifier: MIT
// Opt-in allocation-only staging of GA107 cold-boot WPR metadata.
// This service writes only its own newly allocated system-memory page.
// It performs NO BAR0/MMIO, WPR, Booter, mailbox, or GPU-notification write.
#include "NVGspWprMetaStageService.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define NVWPRMETALOG(fmt, ...) IOLog("NVGspWprMetaStage: " fmt "\n", ##__VA_ARGS__)
#define super IOService
OSDefineMetaClassAndStructors(NVGspWprMetaStageService, IOService);

namespace {
static_assert(sizeof(NVGspWprMetaV1)==256u,"GA107 WPR metadata ABI must remain 256 bytes");

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

static IOService *waitOne(const char *className)
{
    OSDictionary *matching=IOService::serviceMatching(className);
    IOService *service=matching
        ? IOService::waitForMatchingService(matching,NV_GSP_WPR_META_STAGE_WAIT_NS)
        : nullptr;
    OSSafeReleaseNULL(matching);
    return service;
}

static bool sameProvider(IOService *service, IOPCIDevice *pci)
{
    return service && pci && service->getProvider()==pci;
}
}

bool NVGspWprMetaStageService::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCI=nullptr;
    fBacking=nullptr;
    fDMA=nullptr;
    fDMABound=false;
    return true;
}

void NVGspWprMetaStageService::releaseResources(void)
{
    if (fDMABound && fDMA) {
        fDMA->clearMemoryDescriptor();
        fDMABound=false;
    }
    OSSafeReleaseNULL(fDMA);
    OSSafeReleaseNULL(fBacking);
    OSSafeReleaseNULL(fPCI);
}

void NVGspWprMetaStageService::free(void)
{
    releaseResources();
    super::free();
}

IOService *NVGspWprMetaStageService::probe(IOService *provider, SInt32 *score)
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

void NVGspWprMetaStageService::publishState(const char *state)
{
    setProperty("NVGspWprMetaStageState",state ? state : "unknown");
}

bool NVGspWprMetaStageService::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn(NV_GSP_WPR_META_STAGE_BOOT_ARG,&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    setProperty("NVGspWprMetaStageRequested",true);
    setProperty("NVGspWprMetaStageDeviceWritesPossible",false);
    setProperty("NVGspWprMetaStageWprWritten",false);
    setProperty("NVGspWprMetaStageBooterExecuted",false);
    setProperty("NVGspWprMetaStageMailboxWritten",false);
    setProperty("NVGspWprMetaStageGspNotified",false);
    setProperty("NVGspWprMetaStageMetadataVerified",false);
    setProperty("NVGspWprMetaStageBackingBytes",(UInt64)NV_GSP_WPR_META_STAGE_PAGE_BYTES,64);
    setProperty("NVGspWprMetaStageDependencyWaitNs",(UInt64)NV_GSP_WPR_META_STAGE_WAIT_NS,64);
    setProperty("NVGspWprMetaStageOptionalSignatureZero",true);
    setProperty("NVGspWprMetaStageOptionalCrashQueueZero",true);
    setProperty("NVGspWprMetaStageOptionalFlagsZero",true);

    fPCI=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCI) {
        publishState("no-pci-provider");
        registerService();
        return true;
    }
    fPCI->retain();

    // All dependency discovery is bounded. These services are independently
    // opt-in and remain the owners of their DMA mappings; this service only
    // snapshots their published, already-validated values.
    IOService *radix=waitOne("NVGspRmImageStageService");
    if (!radix) {
        publishState("radix3-stage-unavailable");
        registerService();
        return true;
    }
    IOService *boot=waitOne("NVGspWprBootUcodeStageService");
    if (!boot) {
        radix->release();
        publishState("boot-ucode-stage-unavailable");
        registerService();
        return true;
    }
    IOService *factsService=waitOne("NVGspWprFactsObserver");
    if (!factsService) {
        boot->release();
        radix->release();
        publishState("runtime-facts-unavailable");
        registerService();
        return true;
    }

    if (!sameProvider(radix,fPCI) || !sameProvider(boot,fPCI) || !sameProvider(factsService,fPCI)) {
        factsService->release();
        boot->release();
        radix->release();
        publishState("dependency-provider-mismatch");
        registerService();
        return true;
    }

    UInt64 radixRoot=0ull,radixImageBytes=0ull;
    UInt64 bootAddress=0ull,bootImageBytes=0ull;
    UInt64 bootCodeOffset=0ull,bootDataOffset=0ull,bootManifestOffset=0ull;
    UInt64 usableFbRaw=0ull,displayFuseRaw=0ull,vgaWorkspaceRaw=0ull;

    const bool dependenciesReady=
        trueProperty(radix,"NVGspRmImageStagePrepared") &&
        numberProperty(radix,"NVGspRmImageStageRootGpuAddress",radixRoot) &&
        numberProperty(radix,"NVGspRmImageStageFwImageBytes",radixImageBytes) &&
        trueProperty(boot,"NVGspWprBootUcodeStagePrepared") &&
        numberProperty(boot,"NVGspWprBootUcodeStageGpuAddress",bootAddress) &&
        numberProperty(boot,"NVGspWprBootUcodeStageWprSizeOfBootloader",bootImageBytes) &&
        numberProperty(boot,"NVGspWprBootUcodeStageWprBootloaderCodeOffset",bootCodeOffset) &&
        numberProperty(boot,"NVGspWprBootUcodeStageWprBootloaderDataOffset",bootDataOffset) &&
        numberProperty(boot,"NVGspWprBootUcodeStageWprBootloaderManifestOffset",bootManifestOffset) &&
        numberProperty(factsService,"NVGspWprFactsUsableFbMbRaw",usableFbRaw) &&
        numberProperty(factsService,"NVGspWprFactsDisplayFuseRaw",displayFuseRaw) &&
        numberProperty(factsService,"NVGspWprFactsVgaWorkspaceRaw",vgaWorkspaceRaw);

    factsService->release();
    boot->release();
    radix->release();

    if (!dependenciesReady || usableFbRaw>0xffffffffull ||
        displayFuseRaw>0xffffffffull || vgaWorkspaceRaw>0xffffffffull) {
        publishState("dependency-properties-incomplete");
        registerService();
        return true;
    }

    NVGspWprSysmemInput sysmemInput={};
    sysmemInput.radix3Prepared=true;
    sysmemInput.radix3RootGpuAddress=radixRoot;
    sysmemInput.radix3ImageBytes=radixImageBytes;
    sysmemInput.bootPrepared=true;
    sysmemInput.bootGpuAddress=bootAddress;
    sysmemInput.bootImageBytes=bootImageBytes;
    sysmemInput.bootCodeOffset=bootCodeOffset;
    sysmemInput.bootDataOffset=bootDataOffset;
    sysmemInput.bootManifestOffset=bootManifestOffset;

    NVGspWprSysmemPlan sysmem={};
    const UInt32 sysmemVerdict=nvGspPlanWprSysmem(&sysmemInput,&sysmem);
    setProperty("NVGspWprMetaStageSysmemVerdict",(UInt64)sysmemVerdict,32);
    setProperty("NVGspWprMetaStageSysmemVerdictName",nvGspWprSysmemVerdictName(sysmemVerdict));
    if (!nvGspWprSysmemSucceeded(sysmemVerdict)) {
        publishState("sysmem-bridge-refused");
        registerService();
        return true;
    }

    NVGspGa107ObservedRegs observed={};
    observed.usableFbMb=(UInt32)usableFbRaw;
    observed.displayFuse=(UInt32)displayFuseRaw;
    observed.vgaWorkspace=(UInt32)vgaWorkspaceRaw;
    NVGspGa107RuntimeFacts facts={};
    const UInt32 factsVerdict=nvGspDecodeGa107WprFacts(&observed,&facts);
    setProperty("NVGspWprMetaStageFactsVerdict",(UInt64)factsVerdict,32);
    setProperty("NVGspWprMetaStageFactsVerdictName",nvGspGa107FactsVerdictName(factsVerdict));
    if (!nvGspGa107FactsSucceeded(factsVerdict)) {
        publishState("runtime-facts-refused");
        registerService();
        return true;
    }

    NVGspWprCandidateInput candidateInput={};
    candidateInput.factsPrepared=true;
    candidateInput.facts=facts;
    candidateInput.sysmemPrepared=true;
    candidateInput.sysmem=sysmem;
    // Optional cold-boot fields are intentionally explicit zero pairs here.
    candidateInput.signatureGpuAddress=0ull;
    candidateInput.signatureBytes=0ull;
    candidateInput.crashQueueGpuAddress=0ull;
    candidateInput.crashQueueBytes=0ull;
    candidateInput.vfPartitionCount=0u;
    candidateInput.flags=0u;
    candidateInput.pagingConfig=0u;

    NVGspWprCandidatePlan candidate={};
    const UInt32 candidateVerdict=nvGspPlanGa107WprCandidate(&candidateInput,&candidate);
    setProperty("NVGspWprMetaStageCandidateVerdict",(UInt64)candidateVerdict,32);
    setProperty("NVGspWprMetaStageCandidateVerdictName",nvGspWprCandidateVerdictName(candidateVerdict));
    if (!nvGspWprCandidateSucceeded(candidateVerdict) || !candidate.valid ||
        candidate.metadata.verified!=0ull) {
        publishState("metadata-candidate-refused");
        registerService();
        return true;
    }

    // Fresh private memory only. Nothing below aliases WPR/VRAM or BAR0.
    fBacking=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,kIODirectionInOut,(vm_size_t)NV_GSP_WPR_META_STAGE_PAGE_BYTES,
        (vm_offset_t)NV_GSP_WPR_META_STAGE_PAGE_BYTES);
    if (!fBacking) {
        publishState("metadata-backing-allocation-failed");
        registerService();
        return true;
    }
    UInt8 *bytes=reinterpret_cast<UInt8 *>(fBacking->getBytesNoCopy());
    if (!bytes || fBacking->getCapacity()<NV_GSP_WPR_META_STAGE_PAGE_BYTES) {
        publishState("metadata-cpu-mapping-unavailable");
        releaseResources();
        registerService();
        return true;
    }
    bzero(bytes,NV_GSP_WPR_META_STAGE_PAGE_BYTES);
    bcopy(&candidate.metadata,bytes,sizeof(candidate.metadata));

    IOMapper *mapper=IOMapper::copyMapperForDevice(fPCI);
    if (!mapper) {
        publishState("device-mapper-unavailable");
        releaseResources();
        registerService();
        return true;
    }
    fDMA=IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        64,
        (UInt64)NV_GSP_WPR_META_STAGE_PAGE_BYTES,
        IODMACommand::kMapped,
        (UInt64)NV_GSP_WPR_META_STAGE_PAGE_BYTES,
        NV_GSP_WPR_META_STAGE_PAGE_BYTES,
        mapper);
    mapper->release();
    if (!fDMA || fDMA->setMemoryDescriptor(fBacking,true)!=kIOReturnSuccess) {
        publishState("metadata-dma-bind-failed");
        releaseResources();
        registerService();
        return true;
    }
    fDMABound=true;

    UInt64 dmaOffset=0ull;
    UInt32 segmentCount=1u;
    IODMACommand::Segment64 segment={};
    const IOReturn segRet=fDMA->gen64IOVMSegments(&dmaOffset,&segment,&segmentCount);
    setProperty("NVGspWprMetaStageDmaSegmentIOReturn",(UInt64)(UInt32)segRet,32);
    if (segRet!=kIOReturnSuccess || segmentCount!=1u ||
        dmaOffset!=(UInt64)NV_GSP_WPR_META_STAGE_PAGE_BYTES ||
        segment.fLength<(UInt64)NV_GSP_WPR_META_STAGE_PAGE_BYTES ||
        !segment.fIOVMAddr ||
        (segment.fIOVMAddr&(NV_GSP_WPR_META_STAGE_PAGE_BYTES-1u))!=0ull) {
        publishState("metadata-iova-refused");
        releaseResources();
        registerService();
        return true;
    }

    const IOReturn syncRet=fDMA->synchronize(kIODirectionOut);
    setProperty("NVGspWprMetaStageDmaSynchronizeIOReturn",(UInt64)(UInt32)syncRet,32);
    if (syncRet!=kIOReturnSuccess) {
        publishState("metadata-dma-synchronize-failed");
        releaseResources();
        registerService();
        return true;
    }

    setProperty("NVGspWprMetaStageGpuAddress",segment.fIOVMAddr,64);
    setProperty("NVGspWprMetaStageMetadataBytes",(UInt64)sizeof(candidate.metadata),64);
    setProperty("NVGspWprMetaStageFramebufferBytes",(UInt64)facts.fbSizeBytes,64);
    setProperty("NVGspWprMetaStageWprStart",(UInt64)candidate.metadata.gspFwWprStart,64);
    setProperty("NVGspWprMetaStageWprEnd",(UInt64)candidate.metadata.gspFwWprEnd,64);
    setProperty("NVGspWprMetaStageVerifiedValue",(UInt64)candidate.metadata.verified,64);
    setProperty("NVGspWprMetaStageAddressKind","ga107-device-iovm-wpr-metadata-not-installed");
    setProperty("NVGspWprMetaStagePrepared",true);
    publishState("metadata-staged-wpr-not-written");

    registerService();
    NVWPRMETALOG("staged %u-byte cold-boot metadata at GA107 IOVA 0x%llx; WPR/Booter untouched",
                 (unsigned)sizeof(candidate.metadata),
                 (unsigned long long)segment.fIOVMAddr);
    return true;
}

void NVGspWprMetaStageService::stop(IOService *provider)
{
    releaseResources();
    super::stop(provider);
}
