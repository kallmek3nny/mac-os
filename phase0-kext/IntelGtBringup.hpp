// Opt-in Tiger Lake BCS0 execution. Not a Metal/IOAccelerator implementation.
#ifndef IntelGtBringup_hpp
#define IntelGtBringup_hpp
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
class IntelGtBringup : public IOService {
    OSDeclareDefaultStructors(IntelGtBringup)
public:
    bool init(OSDictionary *dict = nullptr) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free(void) override;
private:
    enum { PoolBytes = 128 * 1024, PoolPages = PoolBytes / 4096 };
    IOPCIDevice *fPCI;
    IOMemoryMap *fBarMap, *fPoolMap;
    IOBufferMemoryDescriptor *fPool;
    IODMACommand *fDMA;
    volatile UInt8 *fMMIO;
    UInt8 *fCPU;
    UInt64 fBarBytes, fBusAddress, fSavedPtes[PoolPages];
    UInt32 fGGTTBase, fGGTTEntries, fStage, fTrace, fPteCount;
    UInt32 fOldRingMode, fOldMiMode, fOldHws, fPatIndex;
    UInt16 fOldPCICommand;
    bool fOpened, fPrepared, fDMABound, fPCIChanged;
    bool fRenderWake, fGTWake, fEngineChanged;
    bool fSubmissionPending, fCompleted, fQuarantined;
    UInt32 read32(UInt32 offset);
    bool write32(UInt32 offset, UInt32 value);
    bool wait32(UInt32 offset, UInt32 mask, UInt32 wanted, UInt32 polls);
    bool acquireWake(UInt32 request, UInt32 ack, bool &held);
    void releaseWake(void);
    bool claimEngine(void);
    bool allocateMemory(void);
    bool installGGTT(void);
    bool buildContext(void);
    bool submitCopy(void);
    void restoreGGTT(void);
    void cleanup(void);
    void fail(const char *reason);
};
#endif
