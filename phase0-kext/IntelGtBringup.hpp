// Opt-in Tiger Lake BCS0 execution. Not a Metal/IOAccelerator implementation.
#ifndef IntelGtBringup_hpp
#define IntelGtBringup_hpp
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include "IntelGtSurfaceHandles.hpp"

struct IntelGtSurfaceRuntimeState;
struct IntelGtScanoutRuntimeState;

class IntelGtBringup : public IOService {
    OSDeclareDefaultStructors(IntelGtBringup)
public:
    bool init(OSDictionary *dict = nullptr) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free(void) override;
    IOReturn newUserClient(task_t task, void *securityID, UInt32 type,
                          OSDictionary *properties, IOUserClient **handler) override;
    // Kernel-client entry points; staged CPU buffers, synchronous GPU work.
    // They deliberately expose no arbitrary commands, MMIO or GPU addresses.
    IOReturn copySurface32(const void *source, void *destination,
                          UInt32 width, UInt32 height, UInt32 pitch,
                          UInt32 sourceBytes, UInt32 destinationBytes);
    IOReturn fillSurface32(void *destination,
                          UInt32 width, UInt32 height, UInt32 pitch,
                          UInt32 destinationBytes, UInt32 color);

    // Persistent arena entry points. Userspace supplies/receives opaque handles
    // only; resolution to a private-PPGTT address happens under fQueueLock.
    IOReturn allocatePersistentSurface32(UInt32 width, UInt32 height, UInt32 pitch,
                                         UInt64 *outHandle);
    IOReturn freePersistentSurface(UInt64 handle);
    IOReturn fillPersistentSurface32(UInt64 handle, UInt32 color);
    IOReturn copyPersistentSurface32(UInt64 sourceHandle, UInt64 destinationHandle);

    // Inherited-scanout entry point, iggt-bringup=4 only (IntelGtScanout.cpp).
    // Fills one rectangle of the pipe A plane 1 surface the firmware already
    // presents. The caller names pixels, never a GPU address. Synchronous:
    // returns after fence completion and CSB retirement, like the fills above.
    IOReturn fillScanoutRect32(UInt32 x, UInt32 y, UInt32 w, UInt32 h, UInt32 color);
    // Width and height of that sampled plane, for the user client's rectangle
    // check (IntelGtScanoutRect.hpp). kIOReturnNotReady unless iggt-bringup>=4
    // mapped a sample whose igPresentVerdict() was IG_PRESENT_OK (t-619d61).
    IOReturn scanoutPlaneSize32(UInt32 *outWidth, UInt32 *outHeight);
private:
    enum { PoolBytes = 128 * 1024, PoolPages = PoolBytes / 4096 };
    IOLock *fQueueLock;
    IOPCIDevice *fPCI;
    IOMemoryMap *fBarMap, *fPoolMap;
    IOBufferMemoryDescriptor *fPool;
    IODMACommand *fDMA;
    IntelGtSurfaceRuntimeState *fSurfaceRuntime;
    IntelGtScanoutRuntimeState *fScanoutRuntime;
    IGGtSurfaceHandleArena fSurfaceHandles = {};
    volatile UInt8 *fMMIO;
    UInt8 *fCPU;
    UInt64 fBarBytes, fBusAddress, fSavedPtes[PoolPages];
    UInt32 fGGTTBase, fGGTTEntries, fStage, fTrace, fPteCount;
    UInt32 fOldRingMode, fOldMiMode, fOldHws, fPatIndex;
    UInt32 fOldCsbPointer, fCsbHead, fContextId, fSequence, fRingTail;
    bool fRetired, fFaulted;
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
    bool stagePersistentSurface(void);
    bool releasePersistentSurface(void);
    // Borrowed inherited scanout: private leaf tables hung from pd[1..].
    bool mapScanout(void);
    bool drawScanoutProbe(void);
    bool releaseScanout(void);
    bool installGGTT(void);
    bool buildContext(void);
    bool submitCopy(void);
    bool submitBatch(UInt32 sequence);
    bool waitForRetirement(void);
    void restoreGGTT(void);
    void cleanup(void);
    void fail(const char *reason);
};
#endif