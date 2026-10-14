// SPDX-License-Identifier: MIT
// Opt-in read-only observation of GA107 cold-boot WPR VGA/VBIOS boundary inputs.
// This translation unit contains NO device writes.
#include "NVGspWprVgaObserver.hpp"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#define NVWPRVGALOG(fmt, ...) IOLog("NVGspWprVgaObserver: " fmt "\n", ##__VA_ARGS__)
#define super IOService
OSDefineMetaClassAndStructors(NVGspWprVgaObserver, IOService);

bool NVGspWprVgaObserver::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCI=nullptr;
    fBar0Map=nullptr;
    fBar0Base=nullptr;
    return true;
}

void NVGspWprVgaObserver::clearResources(void)
{
    fBar0Base=nullptr;
    OSSafeReleaseNULL(fBar0Map);
    OSSafeReleaseNULL(fPCI);
}

void NVGspWprVgaObserver::free(void)
{
    clearResources();
    super::free();
}

IOService *NVGspWprVgaObserver::probe(IOService *provider, SInt32 *score)
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

void NVGspWprVgaObserver::publishState(const char *state)
{
    setProperty("NVGspWprVgaObserverState",state ? state : "unknown");
}

UInt32 NVGspWprVgaObserver::read32(UInt32 offset) const
{
    if (!fBar0Base || !fBar0Map ||
        (IOByteCount)offset + sizeof(UInt32) > fBar0Map->getLength())
        return 0xffffffffu;
    return OSReadLittleInt32(fBar0Base,offset);
}

bool NVGspWprVgaObserver::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn(NV_GSP_WPR_VGA_OBSERVER_BOOT_ARG,&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    setProperty("NVGspWprVgaObserverRequested",true);
    setProperty("NVGspWprVgaObserverWritesPossible",false);
    setProperty("NVGspWprVgaObserverMmuLockValid",false);
    setProperty("NVGspWprVgaObserverMmuLockSource","ga107-no-op-hal");

    fPCI=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCI) {
        publishState("no-pci-provider");
        registerService();
        return true;
    }
    fPCI->retain();

    // Observation must never wake or reconfigure the device. If PCI memory
    // decoding is off, report that fact and stop instead of enabling it.
    const UInt16 command=fPCI->configRead16(kIOPCIConfigCommand);
    const bool memoryEnabled=(command&0x0002u)!=0u;
    setProperty("NVGspWprVgaObserverPciCommand",(UInt64)command,16);
    setProperty("NVGspWprVgaObserverMemoryEnabled",memoryEnabled);
    if (!memoryEnabled) {
        publishState("pci-memory-disabled");
        registerService();
        return true;
    }

    IODeviceMemory *bar=fPCI->getDeviceMemoryWithIndex(NV_BAR0_INDEX);
    if (!bar) {
        publishState("bar0-unavailable");
        registerService();
        return true;
    }
    fBar0Map=bar->map(kIOMapInhibitCache);
    if (!fBar0Map) {
        publishState("bar0-map-failed");
        registerService();
        return true;
    }
    fBar0Base=(volatile UInt8 *)fBar0Map->getVirtualAddress();
    if (!fBar0Base) {
        publishState("bar0-null-address");
        registerService();
        return true;
    }

    setProperty("NVGspWprVgaObserverBarMapped",true);
    setProperty("NVGspWprVgaObserverBarLength",(UInt64)fBar0Map->getLength(),64);

    UInt32 maxOffset=NV_GSP_GA107_DISPLAY_FUSE_REGISTER;
    if (NV_GSP_GA107_VGA_WORKSPACE_REGISTER>maxOffset)
        maxOffset=NV_GSP_GA107_VGA_WORKSPACE_REGISTER;
    if (NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT>maxOffset)
        maxOffset=NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT;
    if (NV_USABLE_FB_SIZE_IN_MB>maxOffset)
        maxOffset=NV_USABLE_FB_SIZE_IN_MB;
    if ((IOByteCount)maxOffset+sizeof(UInt32)>fBar0Map->getLength()) {
        setProperty("NVGspWprVgaObserverRequiredBarOffset",(UInt64)maxOffset,32);
        publishState("bar0-too-short");
        registerService();
        return true;
    }

    // The firmware-published framebuffer field is only meaningful after GFW
    // reached COMPLETED. This observer performs one bounded snapshot read, not a
    // poll loop. If firmware is still booting, the operator can retry on a later
    // boot rather than this service blocking start().
    const UInt32 gfwRaw=read32(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT);
    if (gfwRaw==0xffffffffu) {
        publishState("gfw-progress-dead-read");
        registerService();
        return true;
    }
    const UInt32 gfwProgress=nvGfwBootProgress(gfwRaw);
    setProperty("NVGspWprVgaObserverGfwProgress",(UInt64)gfwProgress,32);
    if (gfwProgress!=NV_GFW_BOOT_PROGRESS_COMPLETED) {
        publishState("gfw-not-complete");
        registerService();
        return true;
    }

    const UInt32 fbRaw=read32(NV_USABLE_FB_SIZE_IN_MB);
    const NVFbSizeVerdict fbVerdict=nvFbSizeVerdict(fbRaw);
    setProperty("NVGspWprVgaObserverFramebufferVerdict",nvFbSizeVerdictName(fbVerdict));
    setProperty("NVGspWprVgaObserverFramebufferMeasured",fbVerdict==NV_FB_SIZE_OK);
    if (fbVerdict!=NV_FB_SIZE_OK) {
        publishState("usable-framebuffer-refused");
        registerService();
        return true;
    }

    const UInt32 fbSizeMB=fbRaw&NV_USABLE_FB_SIZE_IN_MB_VALUE_MASK;
    const UInt64 fbBytes=((UInt64)fbSizeMB)<<20;
    setProperty("NVGspWprVgaObserverFramebufferMB",(UInt64)fbSizeMB,32);
    setProperty("NVGspWprVgaObserverFramebufferBytes",fbBytes,64);

    // Source order matters: read the display fuse first, and only consult the
    // VGA-workspace register if this GA107 actually has display enabled. Open
    // RM makes the same branch, so a stale/unmapped workspace word on a
    // displayless device can never affect the cold-boot boundary.
    const UInt32 fuseRaw=read32(NV_GSP_GA107_DISPLAY_FUSE_REGISTER);
    UInt32 workspaceRaw=0u;
    if (fuseRaw!=0xffffffffu && nvGspGa107DisplaySupported(fuseRaw))
        workspaceRaw=read32(NV_GSP_GA107_VGA_WORKSPACE_REGISTER);

    NVGspGa107VgaPlan plan={};
    const UInt32 verdict=nvGspPlanGa107VgaWorkspace(
        fbBytes,fuseRaw,workspaceRaw,&plan);
    setProperty("NVGspWprVgaObserverVerdict",(UInt64)verdict,32);
    setProperty("NVGspWprVgaObserverVerdictName",nvGspGa107VgaVerdictName(verdict));
    setProperty("NVGspWprVgaObserverReady",nvGspGa107VgaSucceeded(verdict));

    // Raw measurements are only published when they are not the dead-read
    // sentinel. Derived offsets are published only behind the accepting verdict.
    if (fuseRaw!=0xffffffffu)
        setProperty("NVGspWprVgaObserverDisplayFuseRaw",(UInt64)fuseRaw,32);
    if (fuseRaw!=0xffffffffu && nvGspGa107DisplaySupported(fuseRaw) &&
        workspaceRaw!=0xffffffffu)
        setProperty("NVGspWprVgaObserverWorkspaceRaw",(UInt64)workspaceRaw,32);

    if (!nvGspGa107VgaSucceeded(verdict)) {
        publishState(verdict==NV_GSP_WPR_VGA_DEAD_FUSE_READ ? "display-fuse-dead-read" :
                     verdict==NV_GSP_WPR_VGA_DEAD_WORKSPACE_READ ? "workspace-dead-read" :
                     "vga-workspace-refused");
        registerService();
        return true;
    }

    setProperty("NVGspWprVgaObserverDisplaySupported",plan.displaySupported);
    setProperty("NVGspWprVgaObserverWorkspaceRegisterValid",plan.workspaceRegisterValid);
    setProperty("NVGspWprVgaObserverUsedPraminFallback",plan.usedPraminFallback);
    setProperty("NVGspWprVgaObserverRelocatedToTail",plan.relocatedToTail);
    if (plan.workspaceRegisterValid)
        setProperty("NVGspWprVgaObserverDecodedWorkspaceOffset",plan.decodedWorkspaceOffset,64);
    setProperty("NVGspWprVgaObserverWorkspaceOffset",plan.workspaceOffset,64);
    setProperty("NVGspWprVgaObserverWorkspaceBytes",plan.workspaceBytes,64);
    setProperty("NVGspWprVgaObserverVbiosReservedOffset",plan.vbiosReservedOffset,64);
    setProperty("NVGspWprVgaObserverAddressKind","framebuffer-byte-offset-read-only-not-installed");

    publishState("wpr-vga-boundary-observed");
    registerService();
    NVWPRVGALOG("fb=%u MiB display=%u workspace=0x%llx bytes=0x%llx vbiosReserved=0x%llx; no GPU writes",
                (unsigned)fbSizeMB,
                (unsigned)plan.displaySupported,
                (unsigned long long)plan.workspaceOffset,
                (unsigned long long)plan.workspaceBytes,
                (unsigned long long)plan.vbiosReservedOffset);
    return true;
}

void NVGspWprVgaObserver::stop(IOService *provider)
{
    clearResources();
    super::stop(provider);
}
