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
    fCopyPublicationLifetimePinned=false;
    if (nvAmpereChannelRuntimeInit(&fChannelRuntime)!=NV_CHANNEL_RUNTIME_OK)
        return false;
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
    // Deny repeated start while a service, channel, or transaction owns a
    // retained lifetime. A provider stop cannot fabricate retirement proof.
    // Check before super::start(), PCI access, mapper acquisition or registry
    // updates so refused activation leaves all prior ownership untouched.
    if (!nvAmpereProviderRuntimeMayStart(
            fChannelRuntime, fCopyPublicationRuntime,
            fCopyPublicationLifetimePinned, fPCIDevice!=nullptr)) {
        NVMAPLOG("start: retained or uncertain provider lifetime; refusing restart");
        return false;
    }

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
    setProperty("NVPeerMapperCopyPublicationLifetimePinned",false);
    setProperty("NVPeerMapperCopyPublicationState",
                publicationPeerValidated
                    ? "peer-mapping-validated-awaiting-channel"
                    : "peer-mapping-unavailable");

    // The exact provider owns the channel-observation runtime now, but this
    // service has no truthful RM/GSP channel allocation source yet. Keep it
    // explicitly empty until a future observer can supply real allocation and
    // CE-bind facts. Do not manufacture readiness from mapper availability.
    setProperty("NVPeerMapperChannelRuntimeSealed",false);
    setProperty("NVPeerMapperChannelOwnershipUncertain",false);
    setProperty("NVPeerMapperCopyPublicationOwnershipUncertain",false);
    setProperty("NVPeerMapperChannelRuntimeWriteAuthorized",false);
    setProperty("NVPeerMapperChannelRuntimeState",
                "awaiting-real-channel-observer");

    if (mapper) mapper->release();
    registerService();
    NVMAPLOG("device-specific mapper %s; copy publication peer proof %s; channel observer pending",
             available ? "available" : "unavailable",
             publicationPeerValidated ? "bound" : "not-bound");
    return true;
}

void NVPeerMapperProbe::stop(IOService *provider)
{
    // There is deliberately no channel/copy retirement or reuse path in this
    // phase. Once a future observer records channel ownership, or a copy
    // transaction starts, stop() must not silently dismantle provider ownership.
    // Mirror the sink quarantine rule: pin service/device lifetime, still
    // complete IOService stop(), and refuse later dependency release until
    // retirement is proven.
    const bool channelOwnershipObserved=
        fChannelRuntime.allocationObserved ||
        fChannelRuntime.submissionEndpointObserved ||
        fChannelRuntime.sealed;
    if (channelOwnershipObserved || fCopyPublicationRuntime.transactionActive) {
        // Stop is not retirement. Revoke channel readiness and copy-publication
        // progress before pinning the exact-device provider. No pointer, token,
        // IOVA, or command stream is exposed through this read-only seam.
        if (channelOwnershipObserved)
            (void)nvAmpereChannelRuntimeMarkOwnershipUncertain(&fChannelRuntime);
        (void)nvAmpereCopyRuntimeMarkOwnershipUncertain(&fCopyPublicationRuntime);
        setProperty("NVPeerMapperChannelOwnershipUncertain",
                    fChannelRuntime.ownershipUncertain);
        setProperty("NVPeerMapperCopyPublicationOwnershipUncertain",true);
        if (!fCopyPublicationLifetimePinned) {
            retain();
            fCopyPublicationLifetimePinned=true;
            setProperty("NVPeerMapperCopyPublicationLifetimePinned",true);
        }
        setProperty("NVPeerMapperCopyPublicationState",
                    fCopyPublicationRuntime.transactionActive
                        ? "active-transaction-provider-retained"
                        : "channel-ownership-provider-retained");
        setProperty("NVPeerMapperChannelRuntimeState",
                    channelOwnershipObserved
                        ? "ownership-uncertain-provider-retained"
                        : "no-channel-observed-provider-retained");
        setProperty("NVPeerMapperChannelRuntimeWriteAuthorized",false);
        setProperty("NVPeerMapperCopyPublicationRuntimeWriteAuthorized",false);
        NVMAPLOG("stop: GA107 channel/copy ownership remains live; retaining provider ownership");
        super::stop(provider);
        return;
    }

    (void)nvAmpereCopyRuntimeSetPeerMapping(&fCopyPublicationRuntime,false);
    setProperty("NVPeerMapperCopyPublicationLifetimePinned",false);
    OSSafeReleaseNULL(fPCIDevice);
    super::stop(provider);
}
