// SPDX-License-Identifier: MIT
// Opt-in runtime owner for the staged Intel/3050 sink backing store.
#ifndef INTEL_SINK_CANDIDATE_SERVICE_HPP
#define INTEL_SINK_CANDIDATE_SERVICE_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "IntelGpuId.hpp"
#include "IntelFramebuffer.hpp"
#include "IntelSinkCandidate.hpp"
#include "IntelSinkGgtt.hpp"
#include "IntelSinkGgttTxn.hpp"
#include "IntelSinkPeer.hpp"

// Allocation constraint only. Clearing the low twelve bits requests page
// alignment; no peer-DMA magnitude claim is made from this mask.
#define IG_SINK_CANDIDATE_PHYS_MASK 0xFFFFFFFFFFFFF000ull
#define IG_SINK_CANDIDATE_DISPLAY_WAIT_NS 1000000000ull
#define IG_SINK_CANDIDATE_PEER_WAIT_NS 1000000000ull

class IntelSinkCandidateService : public IOService
{
    OSDeclareDefaultStructors(IntelSinkCandidateService)
    typedef IOService super;

public:
    virtual bool init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice *fPCIDevice;
    IOBufferMemoryDescriptor *fBacking;
    IODMACommand *fIntelDMA;
    IODMACommand *fPeerDMA;
    IOMemoryMap *fBarMap;
    volatile UInt8 *fMMIO;
    UInt64 fBarBytes;
    UInt64 *fSavedPtes;
    UInt64 *fInstalledPtes;
    UInt32 fSavedPteCount;
    UInt32 fInstalledPteCount;
    UInt32 fGgttEntries;
    IGSinkGgttTxn fGgttTxn;
    IGSinkGgttTxnStats fCommitStats;
    IGSinkGgttTxnStats fRestoreStats;
    UInt32 fPrepareVerdict;
    UInt32 fCommitVerdict;
    UInt32 fRestoreVerdict;
    bool fGgttTxnPrepared;
    bool fGgttInstalled;
    bool fQuarantined;
    bool fGgttLifetimePinned;
    bool fPrepared;
    bool fDMABound;
    bool fPeerDMABound;
    bool fOpened;

    static bool readGgttPte(void *context, IGGgttU32 pteIndex, IGGgttU64 *value);
    static bool writeGgttPte(void *context, IGGgttU32 pteIndex, IGGgttU64 value);

    void pinGgttLifetime(void);
    void enterQuarantine(const char *reason);
    void publishGgttTxnDiagnostics(void);
    void releasePeerDma(void);
    bool releaseGgtt(void);
    void releaseBacking(void);
    void publishNotReady(void);
    bool acceleratorPresent(void);
};

#endif // INTEL_SINK_CANDIDATE_SERVICE_HPP
