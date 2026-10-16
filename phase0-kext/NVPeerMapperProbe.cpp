// SPDX-License-Identifier: MIT
// Read-only proof that the GA107 provider has a device-specific IOMapper.
#include "NVPeerMapperProbe.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <pexpert/pexpert.h>

#define NVMAPLOG(fmt, ...) IOLog("NVPeerMapperProbe: " fmt "\n", ##__VA_ARGS__)
OSDefineMetaClassAndStructors(NVPeerMapperProbe, IOService);

bool NVPeerMapperProbe::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCIDevice=nullptr;
    return nvAmpereCopyRuntimeInit(&fCopyPublicationRuntime)==
           NV_COPY_RUNTIME_OK;
}

void NVPeerMapperProbe::free(void)
{
    OSSafeReleaseNULL(fPCIDevice);
    super::free();
}

IOService *NVPeerMapperProbe::probe(IOService *provider, SInt32 *score)
{
    IOService *result=super::probe(provider,score);
    if (!result) return nullptr;

    IOPCIDevice *pci=OSDynamicCast(IOPCIDevice,provider);
    if (!pci) return nullptr;
    const UInt16 vendor=pci->configRead16(kIOPCIConfigVendorID);
    const UInt16 device=pci->configRead16(kIOPCIConfigDeviceID);
    const UInt32 classReg=pci->configRead32(kIOPCIConfigRevisionID);
    const UInt8 baseClass=(UInt8)((classReg>>24)&0xffu);

    // Exact target laptop GPU: NVIDIA GA107M RTX 3050 Mobile, display class.
    if (vendor!=0x10de || device!=0x25a2 || baseClass!=0x03)
        return nullptr;
    return result;
}

bool NVPeerMapperProbe::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn("igsink-peer-map",&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    fPCIDevice=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCIDevice) {
        super::stop(provider);
        return false;
    }
    fPCIDevice->retain();

    // Re-check exact ownership at start(), not only probe(). Future submission
    // state must never inherit a provider that changed identity between match
    // and activation or was instantiated outside the expected personality.
    const UInt16 vendor=fPCIDevice->configRead16(kIOPCIConfigVendorID);
    const UInt16 device=fPCIDevice->configRead16(kIOPCIConfigDeviceID);
    const UInt32 classReg=fPCIDevice->configRead32(kIOPCIConfigRevisionID);
    const UInt8 baseClass=(UInt8)((classReg>>24)&0xffu);
    if (vendor!=0x10de || device!=0x25a2 || baseClass!=0x03) {
        NVMAPLOG("start: exact GA107M provider ownership check failed");
        OSSafeReleaseNULL(fPCIDevice);
        super::stop(provider);
        return false;
    }

    setProperty("NVPeerMapperRequested",true);
    setProperty("NVPeerMapperTargetVendor",(UInt64)0x10de,16);
    setProperty("NVPeerMapperTargetDevice",(UInt64)0x25a2,16);
    setProperty("NVPeerMapperIommuParentProperty",fPCIDevice->getProperty("iommu-parent")!=nullptr);

    // Apple XNU's device-specific DMA path uses copyMapperForDevice(device),
    // then passes the returned mapper into IODMACommand::withSpecification().
    // This probe intentionally stops one step earlier: resolve the mapper and
    // publish whether it exists, but create no DMA mapping and issue no GPU I/O.
    IOMapper *mapper=IOMapper::copyMapperForDevice(fPCIDevice);
    const bool available=mapper!=nullptr;

    // Bind real provider evidence into the same private state a future channel
    // executor will advance. The mapping fact is no longer accepted from an
    // arbitrary final publication snapshot at this seam. No mapper pointer,
    // IOVA, token, command stream, or publication ordinal is exported.
    NVCopyRuntimeU32 publicationVerdict=NV_COPY_RUNTIME_OK;
    if (available)
        publicationVerdict=nvAmpereCopyRuntimeSetPeerMapping(
            &fCopyPublicationRuntime,true);

    const bool publicationPeerValidated=
        available && publicationVerdict==NV_COPY_RUNTIME_OK &&
        fCopyPublicationRuntime.peerMappingValidated;

    setProperty("NVPeerMapperAvailable",available);
    setProperty("NVPeerMapperState",available ? "device-mapper-available" : "device-mapper-unavailable");
    setProperty("NVPeerMapperCopyPublicationPeerValidated",publicationPeerValidated);
    setProperty("NVPeerMapperCopyPublicationRuntimeWriteAuthorized",false);
    setProperty("NVPeerMapperCopyPublicationState",
                publicationPeerValidated
                    ? "peer-mapping-validated-awaiting-channel"
                    : "peer-mapping-unavailable");

    if (mapper) mapper->release();
    registerService();
    NVMAPLOG("device-specific mapper %s; copy publication peer proof %s",
             available ? "available" : "unavailable",
             publicationPeerValidated ? "bound" : "not-bound");
    return true;
}

void NVPeerMapperProbe::stop(IOService *provider)
{
    // There is deliberately no transaction-reuse/retirement path in this
    // phase. If a future executor starts a transaction, stop() must not silently
    // dismantle provider ownership underneath it. Preserve the service/device
    // lifetime instead of pretending retirement happened.
    if (fCopyPublicationRuntime.transactionActive) {
        setProperty("NVPeerMapperCopyPublicationState",
                    "active-transaction-provider-retained");
        setProperty("NVPeerMapperCopyPublicationRuntimeWriteAuthorized",false);
        NVMAPLOG("stop: active copy publication transaction; retaining provider ownership");
        retain();
        return;
    }

    (void)nvAmpereCopyRuntimeSetPeerMapping(&fCopyPublicationRuntime,false);
    OSSafeReleaseNULL(fPCIDevice);
    super::stop(provider);
}
