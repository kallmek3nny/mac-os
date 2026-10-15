// SPDX-License-Identifier: MIT
// Opt-in GA107 WPR runtime-fact observation. This translation unit contains NO device writes.
#include "NVGspWprFactsObserver.hpp"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#define NVWPRLOG(fmt, ...) IOLog("NVGspWprFactsObserver: " fmt "\n", ##__VA_ARGS__)
#define super IOService
OSDefineMetaClassAndStructors(NVGspWprFactsObserver, IOService);

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

bool NVGspWprFactsObserver::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn("nvgsp-wpr-facts-observe",&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    setProperty("NVGspWprFactsObserverRequested",true);
    setProperty("NVGspWprFactsObserverWritesPossible",false);
    setProperty("NVGspWprFactsObserverMmuLockReadRequired",false);
    setProperty("NVGspBooterFuseObserverRequested",true);
    setProperty("NVGspBooterFuseObserverWritesPossible",false);
    setProperty("NVGspBooterFuseObserved",false);

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

    // ---------------------------------------------------------------------
    // GA107 SEC2 Booter fuse observation.
    //
    // Read identity first and let the pure planner decide whether this exact
    // 10de:25a2 provider is actually GA107 silicon and which one SEC2 fuse
    // register belongs to the Booter ucode ID. The runtime shell never repeats
    // the register arithmetic and never falls back to a hard-coded offset.
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
                    // 0xffffffff is this driver's refusal sentinel, not a
                    // measured fuse value an operator should mistake for data.
                    setProperty("NVGspBooterFuseObserverState",
                                nvGspBooterFuseVerdictName(fuseVerdict));
                }
            }
        }
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
    // state first and refuse rather than polling inside start(). A later boot or
    // operator retry can observe the completed state without this service ever
    // blocking indefinitely.
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
    // All are reads from source-backed GA107/TU102-through-Ada registers.
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
