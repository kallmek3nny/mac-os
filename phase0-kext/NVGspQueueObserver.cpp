// SPDX-License-Identifier: MIT
// Opt-in live PGSP queue observation. This translation unit contains NO device writes.
#include "NVGspQueueObserver.hpp"
#include "NVGa107PciIdentity.hpp"

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

// Public queue counters are LIVE observations, not historical readiness.
// A failed/restarted observation and stop/free must revoke all counter evidence
// before the BAR mapping or device ownership can be released or replaced.
// These are IORegistry changes only: no PCI/BAR0/GSP writes are performed.
void NVGspQueueObserver::invalidateObservation(void)
{
    setProperty("NVGspQueueObserverRequested",false);
    setProperty("NVGspQueueObserverBarMapped",false);
    setProperty("NVGspQueueObserverMemoryEnabled",false);
    setProperty("NVGspQueueObserverSampledTwiceUnchanged",false);
    setProperty("NVGspQueueTrafficEvidence",false);
    setProperty("NVGspQueueCountersPlausibleWithoutHistory",false);
    setProperty("NVGspQueueObserverVerdict",(UInt64)NV_GSP_QUEUE_OBSERVE_NOT_ATTEMPTED,32);
    setProperty("NVGspQueueObserverVerdictName","not-attempted");

    // Never retain an earlier usable snapshot, register offsets, or stale PCI
    // command metadata in the registry after a stop or a refused new read.
    removeProperty("NVGspQueueObserverQueueIndex");
    removeProperty("NVGspQueueObserverPciCommand");
    removeProperty("NVGspQueueObserverBarLength");
    removeProperty("NVGspQueueObserverRequiredBarOffset");
    removeProperty("NVGspQueueCommandHead");
    removeProperty("NVGspQueueCommandTail");
    removeProperty("NVGspQueueMessageHead");
    removeProperty("NVGspQueueMessageTail");
    removeProperty("NVGspQueueCommandHeadRegister");
    removeProperty("NVGspQueueCommandTailRegister");
    removeProperty("NVGspQueueMessageHeadRegister");
    removeProperty("NVGspQueueMessageTailRegister");
    removeProperty("NVGspQueueMessageCount");
    removeProperty("NVGspQueueCommandUsed");
    removeProperty("NVGspQueueStatusAvailable");
    removeProperty("NVGspQueueSnapshotAllZero");
}

void NVGspQueueObserver::free(void)
{
    invalidateObservation();
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
    // Never reactivate a service with a live or partially-retained resource.
    if (fPCI || fBar0Map || fBar0Base) return false;

    int enabled=0;
    if (!PE_parse_boot_argn("nvgsp-queue-observe",&enabled,sizeof(enabled)) || enabled!=1)
        return false;

    // Probe is not lasting authority: re-check the actual start provider
    // BEFORE IOService activation, BAR0 mapping, or DMA allocation.
    // No PCI writes, bus-master enabling, or device memory access here.
    IOPCIDevice *candidate=OSDynamicCast(IOPCIDevice,provider);
    if (!candidate) return false;
    const UInt16 vendor=candidate->configRead16(kIOPCIConfigVendorID);
    const UInt16 device=candidate->configRead16(kIOPCIConfigDeviceID);
    const UInt32 classReg=candidate->configRead32(kIOPCIConfigRevisionID);
    if (!nvGa107PciIdentityEligible(vendor,device,classReg))
        return false;
    if (!super::start(provider)) return false;

    // Never carry previously published counters into this activation, even
    // when PCI memory is off, BAR0 is absent, or the next pair is unstable.
    invalidateObservation();
    setProperty("NVGspQueueObserverRequested",true);
    setProperty("NVGspQueueObserverWritesPossible",false);
    setProperty("NVGspQueueObserverQueueIndex",(UInt64)NV_GSP_RM_QUEUE_INDEX,32);

    // This exact-device candidate passed eligibility before super::start().
    fPCI=candidate;
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

    // A single four-register read may span an active GSP queue update.
    // Take exactly two bounded samples, with NO retry loop and NO MMIO writes.
    // Equal sets reduce mixed-epoch readings; they do not prove hardware
    // atomicity, channel allocation, or successful GSP queue processing.
    NVGspQueueCounterSample first = {};
    first.commandHead=OSReadLittleInt32(fBar0Base,regs.commandHead);
    first.commandTail=OSReadLittleInt32(fBar0Base,regs.commandTail);
    first.messageHead=OSReadLittleInt32(fBar0Base,regs.messageHead);
    first.messageTail=OSReadLittleInt32(fBar0Base,regs.messageTail);

    NVGspQueueCounterSample second = {};
    second.commandHead=OSReadLittleInt32(fBar0Base,regs.commandHead);
    second.commandTail=OSReadLittleInt32(fBar0Base,regs.commandTail);
    second.messageHead=OSReadLittleInt32(fBar0Base,regs.messageHead);
    second.messageTail=OSReadLittleInt32(fBar0Base,regs.messageTail);

    NVGspQueueSnapshot snapshot={};
    const UInt32 verdict=nvGspClassifyStableQueueSnapshot(
        NV_GSP_RM_QUEUE_INDEX,&first,&second,&snapshot);
    setProperty("NVGspQueueObserverVerdict",(UInt64)verdict,32);
    setProperty("NVGspQueueObserverVerdictName",nvGspQueueObserveVerdictName(verdict));
    setProperty("NVGspQueueObserverSampledTwiceUnchanged",
                snapshot.sampledTwiceUnchanged);

    if (verdict==NV_GSP_QUEUE_OBSERVE_CHANGED_DURING_READ) {
        // Publishing one half of an inconsistent sample would manufacture a
        // plausible, cross-epoch set of queue counters. Retain no such data.
        publishState("snapshot-changed-during-read");
        registerService();
        NVQLOG("queue register counters changed during bounded observation");
        return true;
    }

    // Two identical dead BAR reads are NOT usable evidence either. Never
    // publish 0xffffffff as counters after a dead-read/refused classification.
    // The shared helper also forbids one-shot (not double-sampled) evidence.
    if (!nvGspQueueSnapshotCanPublish(verdict,&snapshot)) {
        publishState(verdict==NV_GSP_QUEUE_OBSERVE_DEAD_READ
                         ? "queue-register-dead-read" : "snapshot-refused");
        registerService();
        return true;
    }

    // Publish counter values only after the paired reads passed consistency.
    setProperty("NVGspQueueCommandHead",(UInt64)second.commandHead,32);
    setProperty("NVGspQueueCommandTail",(UInt64)second.commandTail,32);
    setProperty("NVGspQueueMessageHead",(UInt64)second.messageHead,32);
    setProperty("NVGspQueueMessageTail",(UInt64)second.messageTail,32);
    setProperty("NVGspQueueCommandHeadRegister",(UInt64)regs.commandHead,32);
    setProperty("NVGspQueueCommandTailRegister",(UInt64)regs.commandTail,32);
    setProperty("NVGspQueueMessageHeadRegister",(UInt64)regs.messageHead,32);
    setProperty("NVGspQueueMessageTailRegister",(UInt64)regs.messageTail,32);

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
    NVQLOG("double-read snapshot verdict %s HEAD/TAIL cmd=%08x/%08x msg=%08x/%08x",
           nvGspQueueObserveVerdictName(verdict),
           (unsigned)second.commandHead,(unsigned)second.commandTail,
           (unsigned)second.messageHead,(unsigned)second.messageTail);
    return true;
}

void NVGspQueueObserver::stop(IOService *provider)
{
    // Revoke all public snapshot evidence before unmapping BAR0 and releasing
    // provider ownership. stop() must not look like live queue traffic.
    invalidateObservation();
    publishState("stopped-no-live-snapshot");
    clearResources();
    super::stop(provider);
}
