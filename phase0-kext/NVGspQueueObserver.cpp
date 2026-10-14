// SPDX-License-Identifier: MIT
// Opt-in live PGSP queue observation. This translation unit contains NO device writes.
#include "NVGspQueueObserver.hpp"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#define NVQLOG(fmt, ...) IOLog("NVGspQueueObserver: " fmt "\n", ##__VA_ARGS__)
#define super IOService
OSDefineMetaClassAndStructors(NVGspQueueObserver, IOService);

bool NVGspQueueObserver::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCI=nullptr;
    fBar0Map=nullptr;
    fBar0Base=nullptr;
    return true;
}

void NVGspQueueObserver::clearResources(void)
{
    fBar0Base=nullptr;
    OSSafeReleaseNULL(fBar0Map);
    OSSafeReleaseNULL(fPCI);
}

void NVGspQueueObserver::free(void)
{
    clearResources();
    super::free();
}

IOService *NVGspQueueObserver::probe(IOService *provider, SInt32 *score)
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

void NVGspQueueObserver::publishState(const char *state)
{
    setProperty("NVGspQueueObserverState",state ? state : "unknown");
}

bool NVGspQueueObserver::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn("nvgsp-queue-observe",&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    setProperty("NVGspQueueObserverRequested",true);
    setProperty("NVGspQueueObserverWritesPossible",false);
    setProperty("NVGspQueueObserverQueueIndex",(UInt64)NV_GSP_RM_QUEUE_INDEX,32);

    fPCI=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCI) {
        publishState("no-pci-provider");
        registerService();
        return true;
    }
    fPCI->retain();

    // Read PCI command state but never change it. The observer must not turn a
    // sleeping/disabled function into an active one merely to collect telemetry.
    const UInt16 command=fPCI->configRead16(kIOPCIConfigCommand);
    const bool memoryEnabled=(command&0x0002u)!=0u; // PCI COMMAND.MEMORY
    setProperty("NVGspQueueObserverPciCommand",(UInt64)command,16);
    setProperty("NVGspQueueObserverMemoryEnabled",memoryEnabled);
    if (!memoryEnabled) {
        publishState("pci-memory-disabled");
        registerService();
        return true;
    }

    IODeviceMemory *bar=fPCI->getDeviceMemoryWithIndex(0);
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

    setProperty("NVGspQueueObserverBarMapped",true);
    setProperty("NVGspQueueObserverBarLength",(UInt64)fBar0Map->getLength(),64);

    NVGspQueueRegisters regs={};
    if (nvGspPlanQueueRegisters(NV_GSP_RM_QUEUE_INDEX,&regs)!=NV_GSP_QUEUE_REGS_OK) {
        publishState("register-plan-failed");
        registerService();
        return true;
    }

    UInt32 maxOffset=regs.commandHead;
    if (regs.commandTail>maxOffset) maxOffset=regs.commandTail;
    if (regs.messageHead>maxOffset) maxOffset=regs.messageHead;
    if (regs.messageTail>maxOffset) maxOffset=regs.messageTail;
    if ((IOByteCount)maxOffset+sizeof(UInt32)>fBar0Map->getLength()) {
        setProperty("NVGspQueueObserverRequiredBarOffset",(UInt64)maxOffset,32);
        publishState("bar0-too-short");
        registerService();
        return true;
    }

    // The four operations below are the only device accesses after mapping and
    // they are all reads. No queue pointer, mailbox, doorbell or shared-memory
    // state is modified by this service.
    const UInt32 commandHead=OSReadLittleInt32(fBar0Base,regs.commandHead);
    const UInt32 commandTail=OSReadLittleInt32(fBar0Base,regs.commandTail);
    const UInt32 messageHead=OSReadLittleInt32(fBar0Base,regs.messageHead);
    const UInt32 messageTail=OSReadLittleInt32(fBar0Base,regs.messageTail);

    setProperty("NVGspQueueCommandHead",(UInt64)commandHead,32);
    setProperty("NVGspQueueCommandTail",(UInt64)commandTail,32);
    setProperty("NVGspQueueMessageHead",(UInt64)messageHead,32);
    setProperty("NVGspQueueMessageTail",(UInt64)messageTail,32);
    setProperty("NVGspQueueCommandHeadRegister",(UInt64)regs.commandHead,32);
    setProperty("NVGspQueueCommandTailRegister",(UInt64)regs.commandTail,32);
    setProperty("NVGspQueueMessageHeadRegister",(UInt64)regs.messageHead,32);
    setProperty("NVGspQueueMessageTailRegister",(UInt64)regs.messageTail,32);

    NVGspQueueSnapshot snapshot={};
    const UInt32 verdict=nvGspClassifyQueueSnapshot(
        NV_GSP_RM_QUEUE_INDEX,commandHead,commandTail,messageHead,messageTail,&snapshot);
    setProperty("NVGspQueueObserverVerdict",(UInt64)verdict,32);
    setProperty("NVGspQueueObserverVerdictName",nvGspQueueObserveVerdictName(verdict));

    if (verdict==NV_GSP_QUEUE_OBSERVE_OK ||
        verdict==NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS) {
        setProperty("NVGspQueueMessageCount",(UInt64)snapshot.messageCount,32);
        setProperty("NVGspQueueCommandUsed",(UInt64)snapshot.commandUsed,32);
        setProperty("NVGspQueueStatusAvailable",(UInt64)snapshot.statusAvailable,32);
        setProperty("NVGspQueueSnapshotAllZero",snapshot.allZero);
        setProperty("NVGspQueueTrafficEvidence",snapshot.anyTrafficEvidence);
        setProperty("NVGspQueueCountersPlausibleWithoutHistory",
                    snapshot.countersPlausibleWithoutHistory);
    }

    publishState(verdict==NV_GSP_QUEUE_OBSERVE_OK ? "snapshot-plausible" :
                 verdict==NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS ? "snapshot-needs-history" :
                 verdict==NV_GSP_QUEUE_OBSERVE_DEAD_READ ? "queue-register-dead-read" :
                 "snapshot-refused");
    registerService();
    NVQLOG("snapshot verdict %s HEAD/TAIL cmd=%08x/%08x msg=%08x/%08x",
           nvGspQueueObserveVerdictName(verdict),
           (unsigned)commandHead,(unsigned)commandTail,
           (unsigned)messageHead,(unsigned)messageTail);
    return true;
}

void NVGspQueueObserver::stop(IOService *provider)
{
    clearResources();
    super::stop(provider);
}
