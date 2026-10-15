// SPDX-License-Identifier: MIT
// Internal runtime state shared by the Tiger Lake persistent-arena owner.
// This header is kernel-private implementation detail and is never part of the
// userspace ABI. It contains CPU/IOMMU/PTE state that must never be published.
#ifndef IntelGtSurfaceRuntime_hpp
#define IntelGtSurfaceRuntime_hpp

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include "IntelGtSurface.hpp"
#include "IntelGtSurfaceTxn.hpp"

#define IG_GT_SURFACE_RUNTIME_PAGES_448 IG_GTS_ARENA_LEAVES_448

struct IntelGtSurfaceRuntimeState {
    IOBufferMemoryDescriptor *backing;
    IODMACommand *dma;
    UInt8 *cpu;
    bool dmaBound;
    bool installed;
    bool unsafe;
    UInt32 segmentCount;
    IGGtSurfacePteTxn txn;
    IGSurfaceU64 snapshot[IG_GT_SURFACE_RUNTIME_PAGES_448];
    IGSurfaceU64 installedPtes[IG_GT_SURFACE_RUNTIME_PAGES_448];
};

#endif // IntelGtSurfaceRuntime_hpp
