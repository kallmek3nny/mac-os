// SPDX-License-Identifier: MIT
// Persistent-surface extension for the already-owned Tiger Lake BCS0 backend.
#ifndef IntelGtSurfaceBringup_hpp
#define IntelGtSurfaceBringup_hpp

#include "IntelGtBringup.hpp"
#include "IntelGtSurfaceInstall.hpp"

class IntelGtSurfaceBringup : public IntelGtBringup {
    OSDeclareDefaultStructors(IntelGtSurfaceBringup)
public:
    bool init(OSDictionary *dict = nullptr) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free(void) override;

private:
    IOBufferMemoryDescriptor *fSurfaceBacking;
    IODMACommand *fSurfaceDMA;
    UInt8 *fSurfaceCPU;
    UInt64 fSurfaceSavedPtes[IG_GTS_ARENA_LEAVES_448];
    UInt32 fSurfaceInstallCount;
    bool fSurfaceDMABound;
    bool fSurfaceInstalled;
    bool fSurfaceRestoreFailed;

    bool stageSurfaceArena(void);
    bool restoreSurfaceArena(void);
    void releaseSurfaceArena(void);
    void publishSurfaceState(const char *state);
};

#endif // IntelGtSurfaceBringup_hpp
