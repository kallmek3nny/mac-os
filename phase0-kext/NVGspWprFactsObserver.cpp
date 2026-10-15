// SPDX-License-Identifier: MIT
// Opt-in GA107 WPR runtime-fact observation. This translation unit contains NO device writes.
#include "NVGspWprFactsObserver.hpp"
#include "NVGspBooterFusePatchPolicy.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define NVWPRLOG(fmt, ...) IOLog("NVGspWprFactsObserver: " fmt "\n", ##__VA_ARGS__)
#define super IOService
OSDefineMetaClassAndStructors(NVGspWprFactsObserver, IOService);

namespace {
static const UInt64 kBooterPatchContainerPolicyMaxBytes=4ull*1024ull*1024ull;
static const UInt32 kBooterPatchPrivateAlignment=4096u;
}

bool NVGspWprFactsObserver::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCI=nullptr;
    fBar0Map=nullptr;
    fBar0Base=nullptr;
    return true;
}

void NVGspWprFactsObserver::clearResources(void)
{
    fBar0Base=nullptr;
    OSSafeReleaseNULL(fBar0Map);
    OSSafeReleaseNULL(fPCI);
}

void NVGspWprFactsObserver::free(void)
{
    clearResources();
    super::free();
}

IOService *NVGspWprFactsObserver::probe(IOService *provider, SInt32 *score)
{
    IOService *result=super::probe(provider,score);
    if (!result) return nullptr;

    IOPCIDevice *pci=OSDynamicCast(IOPCIDevice,provider);
    if (!pci) return nullptr;
    const UInt16 vendor=pci->configRead16(kIOPCIConfigVendorID);
    const UInt16 device=pci->configRead16(kIOPCIConfigDeviceID);
    const UInt32 classReg=pci->configRead32(kIOPCIConfigRevisionID);
    const UInt8 baseClass=(UInt8)((classReg>>24)&0xffu);
    if (vendor!=0x10deu || device!=0x25a2u || baseClass!=0x03u)
        return nullptr;
    return result;
}

void NVGspWprFactsObserver::publishState(const char *state)
{
    setProperty("NVGspWprFactsObserverState",state ? state : "unknown");
}

void NVGspWprFactsObserver::stageBooterPrivateImage(
    const NVGspBooterFuseObservation &observation)
{
    setProperty("NVGspBooterPatchStagePrepared",false);
    setProperty("NVGspBooterPatchStageDeviceWritesPossible",false);
    setProperty("NVGspBooterPatchStageDmaMapped",false);
    setProperty("NVGspBooterPatchStageWprWritten",false);
    setProperty("NVGspBooterPatchStageMailboxWritten",false);
    setProperty("NVGspBooterPatchStageBooterExecuted",false);
    setProperty("NVGspBooterPatchStageGspExecuted",false);
    setProperty("NVGspBooterPatchStageSourceWritten",false);
    setProperty("NVGspBooterPatchStagePrivateAddressPublished",false);
    setProperty("NVGspBooterPatchStageInputProperty",NV_GSP_BOOTER_LOAD_FIRMWARE_PROPERTY);
    setProperty("NVGspBooterPatchStageContainerPolicyMaxBytes",
                kBooterPatchContainerPolicyMaxBytes,64);

    OSObject *object=copyProperty(NV_GSP_BOOTER_LOAD_FIRMWARE_PROPERTY);
    if (!object && fPCI)
        object=fPCI->copyProperty(NV_GSP_BOOTER_LOAD_FIRMWARE_PROPERTY);
    if (!object) {
        setProperty("NVGspBooterPatchStageState","booter-load-firmware-unavailable");
        return;
    }

    OSData *firmware=OSDynamicCast(OSData,object);
    if (!firmware) {
        object->release();
        setProperty("NVGspBooterPatchStageState","booter-load-firmware-wrong-type");
        return;
    }

    const UInt64 containerBytes=(UInt64)firmware->getLength();
    const UInt8 *container=reinterpret_cast<const UInt8 *>(firmware->getBytesNoCopy());
    setProperty("NVGspBooterPatchStageContainerBytes",containerBytes,64);
    if (!container || !containerBytes ||
        containerBytes>kBooterPatchContainerPolicyMaxBytes ||
        containerBytes>0xffffffffull) {
        setProperty("NVGspBooterPatchStageState","booter-load-firmware-policy-refused");
        firmware->release();
        return;
    }

    NVGspBooterFusePatchPlan plan={};
    const UInt32 policyVerdict=nvGspPlanObservedBooterPatch(
        container,(NVGspBooterHsU64)containerBytes,&observation,&plan);
    setProperty("NVGspBooterPatchStagePolicyVerdict",(UInt64)policyVerdict,32);
    setProperty("NVGspBooterPatchStagePolicyVerdictName",
                nvGspBooterFusePatchVerdictName(policyVerdict));
    if (!nvGspBooterFusePatchSucceeded(policyVerdict) || !plan.valid) {
        setProperty("NVGspBooterPatchStageState","observed-fuse-patch-policy-refused");
        firmware->release();
        return;
    }

    setProperty("NVGspBooterPatchStageObservedUcodeId",(UInt64)plan.observation.ucodeId,32);
    setProperty("NVGspBooterPatchStageObservedFuseVersion",
                (UInt64)plan.observation.fuseVersion,32);
    setProperty("NVGspBooterPatchStageParsedUcodeId",(UInt64)plan.patch.ucodeId,32);
    setProperty("NVGspBooterPatchStageSignatureIndex",(UInt64)plan.patch.signatureIndex,32);
    setProperty("NVGspBooterPatchStageSignatureBytes",(UInt64)plan.patch.signatureBytes,32);
    setProperty("NVGspBooterPatchStagePatchOffset",
                (UInt64)plan.patch.patchDestinationOffset,32);
    setProperty("NVGspBooterPatchStageFirmwareBytes",(UInt64)plan.patch.firmwareBytes,32);

    if (!plan.patch.firmwareBytes ||
        (UInt64)plan.patch.firmwareBytes>kBooterPatchContainerPolicyMaxBytes ||
        (UInt64)plan.patch.firmwareBytes>(UInt64)(vm_size_t)~(vm_size_t)0) {
        setProperty("NVGspBooterPatchStageState","private-image-size-refused");
        firmware->release();
        return;
    }

    IOBufferMemoryDescriptor *privateImage=IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,kIODirectionInOut,(vm_size_t)plan.patch.firmwareBytes,
        (vm_offset_t)kBooterPatchPrivateAlignment);
    if (!privateImage) {
        setProperty("NVGspBooterPatchStageState","private-image-allocation-failed");
        firmware->release();
        return;
    }

    UInt8 *privateBytes=reinterpret_cast<UInt8 *>(privateImage->getBytesNoCopy());
    if (!privateBytes || privateImage->getCapacity()<(vm_size_t)plan.patch.firmwareBytes) {
        setProperty("NVGspBooterPatchStageState","private-image-cpu-map-failed");
        privateImage->release();
        firmware->release();
        return;
    }

    bzero(privateBytes,(vm_size_t)plan.patch.firmwareBytes);
    NVGspBooterFusePatchPlan built={};
    const UInt32 buildVerdict=nvGspBuildObservedBooterPrivateImage(
        container,(NVGspBooterHsU64)containerBytes,&observation,
        privateBytes,(NVGspBooterHsU64)plan.patch.firmwareBytes,&built);
    setProperty("NVGspBooterPatchStageBuildVerdict",(UInt64)buildVerdict,32);
    setProperty("NVGspBooterPatchStageBuildVerdictName",
                nvGspBooterFusePatchVerdictName(buildVerdict));
    if (!nvGspBooterFusePatchSucceeded(buildVerdict) || !built.valid) {
        bzero(privateBytes,(vm_size_t)plan.patch.firmwareBytes);
        setProperty("NVGspBooterPatchStageState","private-image-build-refused");
        privateImage->release();
        firmware->release();
        return;
    }

    // This milestone proves only the fuse-to-production-signature decision and
    // private-copy mutation. Deliberately erase and release the copy here: it is
    // never DMA mapped, installed into WPR, exposed as an address, or executed.
    setProperty("NVGspBooterPatchStagePrepared",true);
    setProperty("NVGspBooterPatchStagePrivateImageTransient",true);
    setProperty("NVGspBooterPatchStageState","private-image-patched-not-installed");
    NVWPRLOG("Booter private patch rehearsal ucode=%u fuse=%u sigIndex=%u bytes=%u; image not installed",
             (unsigned)built.observation.ucodeId,
             (unsigned)built.observation.fuseVersion,
             (unsigned)built.patch.signatureIndex,
             (unsigned)built.patch.firmwareBytes);

    bzero(privateBytes,(vm_size_t)plan.patch.firmwareBytes);
    privateImage->release();
    firmware->release();
}

bool NVGspWprFactsObserver::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn("nvgsp-wpr-facts-observe",&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    int patchStageEnabled=0;
    const bool patchStageRequested=
        PE_parse_boot_argn(NV_GSP_BOOTER_PATCH_STAGE_BOOT_ARG,
                           &patchStageEnabled,sizeof(patchStageEnabled)) &&
        patchStageEnabled==1;

    setProperty("NVGspWprFactsObserverRequested",true);
    setProperty("NVGspWprFactsObserverWritesPossible",false);
    setProperty("NVGspWprFactsObserverMmuLockReadRequired",false);
    setProperty("NVGspBooterFuseObserverRequested",true);
    setProperty("NVGspBooterFuseObserverWritesPossible",false);
    setProperty("NVGspBooterFuseObserved",false);
    setProperty("NVGspBooterPatchStageRequested",patchStageRequested);
    setProperty("NVGspBooterPatchStagePrepared",false);
    setProperty("NVGspBooterPatchStageDeviceWritesPossible",false);
    setProperty("NVGspBooterPatchStageState",
                patchStageRequested ? "awaiting-fuse-observation" : "stage-gate-disabled");

    fPCI=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCI) {
        publishState("no-pci-provider");
        registerService();
        return true;
    }
    fPCI->retain();

    const UInt16 vendor=fPCI->configRead16(kIOPCIConfigVendorID);
    const UInt16 device=fPCI->configRead16(kIOPCIConfigDeviceID);
    setProperty("NVGspBooterFusePciVendor",(UInt64)vendor,16);
    setProperty("NVGspBooterFusePciDevice",(UInt64)device,16);

    // Observe PCI command state without changing it. This observer never opens
    // the device, enables memory decoding, enables bus mastering or claims GPU
    // ownership merely to collect telemetry.
    const UInt16 command=fPCI->configRead16(kIOPCIConfigCommand);
    const bool memoryEnabled=(command&0x0002u)!=0u;
    setProperty("NVGspWprFactsPciCommand",(UInt64)command,16);
    setProperty("NVGspWprFactsMemoryEnabled",memoryEnabled);
    if (!memoryEnabled) {
        setProperty("NVGspBooterFuseObserverState","pci-memory-disabled");
        publishState("pci-memory-disabled");
        registerService();
        return true;
    }

    IODeviceMemory *bar=fPCI->getDeviceMemoryWithIndex(0);
    if (!bar) {
        setProperty("NVGspBooterFuseObserverState","bar0-unavailable");
        publishState("bar0-unavailable");
        registerService();
        return true;
    }

    fBar0Map=bar->map(kIOMapInhibitCache);
    if (!fBar0Map) {
        setProperty("NVGspBooterFuseObserverState","bar0-map-failed");
        publishState("bar0-map-failed");
        registerService();
        return true;
    }
    fBar0Base=(volatile UInt8 *)fBar0Map->getVirtualAddress();
    if (!fBar0Base) {
        setProperty("NVGspBooterFuseObserverState","bar0-null-address");
        publishState("bar0-null-address");
        registerService();
        return true;
    }

    setProperty("NVGspWprFactsBarMapped",true);
    setProperty("NVGspWprFactsBarLength",(UInt64)fBar0Map->getLength(),64);

    NVGspBooterFuseObservation observedBooterFuse={};
    bool observedBooterFuseValid=false;

    // ---------------------------------------------------------------------
    // GA107 SEC2 Booter fuse observation.
    // ---------------------------------------------------------------------
    if ((IOByteCount)NV_PMC_BOOT_42+sizeof(UInt32)>fBar0Map->getLength()) {
        setProperty("NVGspBooterFuseObserverState","identity-bar-too-short");
        setProperty("NVGspBooterFusePlanVerdict",
                    (UInt64)NV_GSP_BOOTER_FUSE_NOT_ATTEMPTED,32);
        setProperty("NVGspBooterFusePlanVerdictName",
                    nvGspBooterFuseVerdictName(NV_GSP_BOOTER_FUSE_NOT_ATTEMPTED));
    }
    else {
        const UInt32 boot0=OSReadLittleInt32(fBar0Base,NV_PMC_BOOT_0);
        const UInt32 boot42=OSReadLittleInt32(fBar0Base,NV_PMC_BOOT_42);
        NVGspBooterFuseReadPlan fusePlan={};
        const UInt32 planVerdict=nvGspPlanGa107BooterFuseRead(
            (NVU32)vendor,(NVU32)device,boot0,boot42,
            NV_GSP_GA107_BOOTER_HS_UCODE_ID,&fusePlan);

        setProperty("NVGspBooterFusePlanVerdict",(UInt64)planVerdict,32);
        setProperty("NVGspBooterFusePlanVerdictName",
                    nvGspBooterFuseVerdictName(planVerdict));

        if (!nvGspBooterFuseSucceeded(planVerdict)) {
            setProperty("NVGspBooterFuseObserverState",
                        nvGspBooterFuseVerdictName(planVerdict));
        }
        else {
            setProperty("NVGspBooterFuseUcodeId",(UInt64)fusePlan.ucodeId,32);
            setProperty("NVGspBooterFuseRegister",(UInt64)fusePlan.registerOffset,32);

            if ((IOByteCount)fusePlan.registerOffset+sizeof(UInt32)>
                fBar0Map->getLength()) {
                setProperty("NVGspBooterFuseObserverState","fuse-bar-too-short");
                setProperty("NVGspBooterFuseVerdict",
                            (UInt64)NV_GSP_BOOTER_FUSE_NOT_ATTEMPTED,32);
                setProperty("NVGspBooterFuseVerdictName",
                            nvGspBooterFuseVerdictName(NV_GSP_BOOTER_FUSE_NOT_ATTEMPTED));
            }
            else {
                const UInt32 rawFuse=OSReadLittleInt32(
                    fBar0Base,fusePlan.registerOffset);
                NVGspBooterFuseObservation observed={};
                const UInt32 fuseVerdict=nvGspDecodeGa107BooterFuse(
                    &fusePlan,rawFuse,&observed);

                setProperty("NVGspBooterFuseVerdict",(UInt64)fuseVerdict,32);
                setProperty("NVGspBooterFuseVerdictName",
                            nvGspBooterFuseVerdictName(fuseVerdict));

                if (nvGspBooterFuseSucceeded(fuseVerdict)) {
                    observedBooterFuse=observed;
                    observedBooterFuseValid=true;
                    setProperty("NVGspBooterFuseRaw",(UInt64)observed.rawFuse,32);
                    setProperty("NVGspBooterFuseVersion",(UInt64)observed.fuseVersion,32);
                    setProperty("NVGspBooterFuseObserved",true);
                    setProperty("NVGspBooterFuseObserverState","observed");
                    NVWPRLOG("SEC2 Booter fuse ucode=%u reg=0x%08x raw=0x%08x version=%u",
                             (unsigned)observed.ucodeId,
                             (unsigned)observed.registerOffset,
                             (unsigned)observed.rawFuse,
                             (unsigned)observed.fuseVersion);
                }
                else {
                    // Deliberately do not publish rawFuse on the dead-read arm.
                    setProperty("NVGspBooterFuseObserverState",
                                nvGspBooterFuseVerdictName(fuseVerdict));
                }
            }
        }
    }

    // Signature patch rehearsal is deliberately independent of the WPR-facts
    // verdict below. It can fail/decline without poisoning framebuffer facts,
    // and it has no device-visible side effect.
    if (patchStageRequested) {
        if (observedBooterFuseValid)
            stageBooterPrivateImage(observedBooterFuse);
        else
            setProperty("NVGspBooterPatchStageState","fuse-observation-unavailable");
    }

    UInt32 maxOffset=NV_GSP_GA107_USABLE_FB_MB_REG;
    if (NV_GSP_GA107_DISPLAY_FUSE_REG>maxOffset) maxOffset=NV_GSP_GA107_DISPLAY_FUSE_REG;
    if (NV_GSP_GA107_VGA_WORKSPACE_REG>maxOffset) maxOffset=NV_GSP_GA107_VGA_WORKSPACE_REG;
    if (NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT>maxOffset)
        maxOffset=NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT;
    if ((IOByteCount)maxOffset+sizeof(UInt32)>fBar0Map->getLength()) {
        setProperty("NVGspWprFactsRequiredBarOffset",(UInt64)maxOffset,32);
        publishState("bar0-too-short");
        registerService();
        return true;
    }

    // Usable-FB is a firmware-published fact. Take one bounded snapshot of GFW
    // state first and refuse rather than polling inside start().
    const UInt32 gfwRaw=OSReadLittleInt32(
        fBar0Base,NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT);
    if (gfwRaw==0xffffffffu) {
        publishState("gfw-progress-dead-read");
        registerService();
        return true;
    }
    const UInt32 gfwProgress=nvGfwBootProgress(gfwRaw);
    setProperty("NVGspWprFactsGfwProgress",(UInt64)gfwProgress,32);
    if (!nvGfwBootComplete(gfwRaw)) {
        publishState("gfw-not-complete");
        registerService();
        return true;
    }

    // These are the only three WPR-fact device accesses after the GFW snapshot.
    NVGspGa107ObservedRegs regs={};
    regs.usableFbMb=OSReadLittleInt32(fBar0Base,NV_GSP_GA107_USABLE_FB_MB_REG);
    regs.displayFuse=OSReadLittleInt32(fBar0Base,NV_GSP_GA107_DISPLAY_FUSE_REG);
    regs.vgaWorkspace=OSReadLittleInt32(fBar0Base,NV_GSP_GA107_VGA_WORKSPACE_REG);

    setProperty("NVGspWprFactsUsableFbMbRegister",(UInt64)NV_GSP_GA107_USABLE_FB_MB_REG,32);
    setProperty("NVGspWprFactsDisplayFuseRegister",(UInt64)NV_GSP_GA107_DISPLAY_FUSE_REG,32);
    setProperty("NVGspWprFactsVgaWorkspaceRegister",(UInt64)NV_GSP_GA107_VGA_WORKSPACE_REG,32);
    setProperty("NVGspWprFactsUsableFbMbRaw",(UInt64)regs.usableFbMb,32);
    setProperty("NVGspWprFactsDisplayFuseRaw",(UInt64)regs.displayFuse,32);
    setProperty("NVGspWprFactsVgaWorkspaceRaw",(UInt64)regs.vgaWorkspace,32);

    NVGspGa107RuntimeFacts facts={};
    const UInt32 verdict=nvGspDecodeGa107WprFacts(&regs,&facts);
    setProperty("NVGspWprFactsVerdict",(UInt64)verdict,32);
    setProperty("NVGspWprFactsVerdictName",nvGspGa107FactsVerdictName(verdict));

    if (nvGspGa107FactsSucceeded(verdict)) {
        setProperty("NVGspWprFactsUsableFbBytes",(UInt64)facts.fbSizeBytes,64);
        setProperty("NVGspWprFactsDisplaySupported",facts.displaySupported);
        setProperty("NVGspWprFactsVgaWorkspaceRegisterValid",facts.vgaWorkspaceRegisterValid);
        setProperty("NVGspWprFactsVgaWorkspaceRegisterOffset",(UInt64)facts.vgaWorkspaceRegisterOffset,64);
        setProperty("NVGspWprFactsVgaWorkspacePolicyOffset",(UInt64)facts.vgaWorkspacePolicyOffset,64);
        setProperty("NVGspWprFactsVbiosReservedOffset",(UInt64)facts.vgaWorkspacePolicyOffset,64);
        setProperty("NVGspWprFactsVgaWorkspaceBytes",(UInt64)(facts.fbSizeBytes-facts.vgaWorkspacePolicyOffset),64);
        setProperty("NVGspWprFactsMmuLockValid",facts.mmuLockValid);
        setProperty("NVGspWprFactsMmuLockLo",(UInt64)facts.mmuLockLo,64);
        setProperty("NVGspWprFactsAddressKind","framebuffer-byte-offset-read-only-not-installed");
        publishState("facts-plausible");
    }
    else {
        publishState(verdict==NV_GSP_GA107_FACTS_DEAD_READ ? "register-dead-read" :
                     verdict==NV_GSP_GA107_FACTS_BAD_FRAMEBUFFER ? "framebuffer-refused" :
                     verdict==NV_GSP_GA107_FACTS_BAD_WORKSPACE ? "workspace-refused" :
                     "facts-refused");
    }

    registerService();
    NVWPRLOG("facts verdict %s fbMB=%08x fuse=%08x workspace=%08x",
             nvGspGa107FactsVerdictName(verdict),
             (unsigned)regs.usableFbMb,
             (unsigned)regs.displayFuse,
             (unsigned)regs.vgaWorkspace);
    return true;
}

void NVGspWprFactsObserver::stop(IOService *provider)
{
    clearResources();
    super::stop(provider);
}
