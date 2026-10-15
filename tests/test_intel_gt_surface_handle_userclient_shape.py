#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "phase0-kext" / "IntelGtBlitABI.h").read_text()
CLIENT = (ROOT / "phase0-kext" / "IntelGtBlitUserClient.cpp").read_text()
BRINGUP = (ROOT / "phase0-kext" / "IntelGtBringup.hpp").read_text()
PERSIST = (ROOT / "phase0-kext" / "IntelGtPersistentSurfaces.cpp").read_text()
RUNTIME = (ROOT / "phase0-kext" / "IntelGtSurfaceRuntime.hpp").read_text()
SURFACE = (ROOT / "phase0-kext" / "IntelGtBringupSurface.cpp").read_text()
MAKEFILE = (ROOT / "phase0-kext" / "Makefile").read_text()

# The user ABI is fixed and handle-only. No address-like field may be added to
# any persistent-surface request/response without failing this gate.
for token in [
    "IGGT_SURFACE_ALLOC32_SELECTOR 2",
    "IGGT_SURFACE_FREE_SELECTOR 3",
    "IGGT_SURFACE_FILL32_SELECTOR 4",
    "IGGT_SURFACE_COPY32_SELECTOR 5",
    "unsigned long long handle;",
    "unsigned long long sourceHandle;",
    "unsigned long long destinationHandle;",
]:
    assert token in ABI, f"missing persistent ABI token: {token}"

persistent_abi = ABI[ABI.index("struct IgGtSurfaceAlloc32Request") :]
for forbidden in [
    "gpuAddress", "gpuVa", "iova", "dmaAddress", "cpuAddress", "pointer",
    "command", "batch", "pte", "firstArenaPage", "pageCount",
]:
    assert forbidden.lower() not in persistent_abi.lower(), \
        f"persistent ABI leaked address/command detail: {forbidden}"

# Every selector has exact fixed structure sizes and goes only through the
# existing IntelGtBringup owner. No scalar or descriptor ABI is introduced.
assert "args->scalarInputCount" in CLIENT
assert "args->scalarOutputCount" in CLIENT
assert "args->structureInputDescriptor" in CLIENT
assert "args->structureOutputDescriptor" in CLIENT
for token in [
    "sizeof(IgGtSurfaceAlloc32Request)",
    "sizeof(IgGtSurfaceAlloc32Response)",
    "sizeof(IgGtSurfaceFreeRequest)",
    "sizeof(IgGtSurfaceFreeResponse)",
    "sizeof(IgGtSurfaceFill32Request)",
    "sizeof(IgGtSurfaceFill32Response)",
    "sizeof(IgGtSurfaceCopy32Request)",
    "sizeof(IgGtSurfaceCopy32Response)",
    "fOwner->allocatePersistentSurface32(",
    "fOwner->freePersistentSurface(",
    "fOwner->fillPersistentSurface32(",
    "fOwner->copyPersistentSurface32(",
]:
    assert token in CLIENT, f"userclient missing fixed handle dispatch: {token}"

# Persistent operations must stay serialized on the existing queue and must
# resolve handles back through kernel-owned allocator/planner state.
assert PERSIST.count("IOLockLock(fQueueLock)") == 4
assert PERSIST.count("IOLockUnlock(fQueueLock)") == 4
for token in [
    "igGtSurfaceHandleAllocate(",
    "igGtSurfaceHandleResolve(",
    "igGtSurfacePlan32(",
    "igGtSurfaceHandleFree(",
    "gtEncodeFill32(",
    "gtEncodeCopy32(",
    "submitBatch(fSequence)",
    "fRetired && !fFaulted",
    "state->installed && !state->unsafe",
]:
    assert token in PERSIST, f"persistent backend missing safety primitive: {token}"

# Allocation zeroes the complete rounded page run before returning the handle.
alloc = PERSIST[PERSIST.index("allocatePersistentSurface32") : PERSIST.index("freePersistentSurface")]
assert alloc.index("bzero(state->cpu+cpuOffset") < alloc.index("*outHandle=(UInt64)stagedHandle")
assert "plan.allocationBytes" in alloc

# Free scrubs while the slot still owns its pages, then invalidates the handle.
free = PERSIST[PERSIST.index("freePersistentSurface") : PERSIST.index("fillPersistentSurface32")]
assert free.index("bzero(state->cpu+cpuOffset") < free.index("igGtSurfaceHandleFree(")
assert "fRetired && !fFaulted" in free

# The fixed-handle backend itself must not gain PCI/MMIO/config ownership or a
# second hardware-write gate. All writes still flow through submitBatch().
for forbidden in [
    "fPCI->open(", "fPCI->close(", "configWrite", "getDeviceMemoryWithIndex(",
    "write32(", "regWrite32(", "PHASE4_ALLOW_WRITES", "IOMapper::copyMapperForDevice",
]:
    assert forbidden not in PERSIST, f"persistent handle backend gained forbidden path: {forbidden}"

# Runtime state has one shared kernel-private definition. No layout casts.
assert "struct IntelGtSurfaceRuntimeState" in RUNTIME
assert '#include "IntelGtSurfaceRuntime.hpp"' in SURFACE
assert '#include "IntelGtSurfaceRuntime.hpp"' in PERSIST
assert "SurfaceRuntimeLayout" not in PERSIST
assert "reinterpret_cast<SurfaceRuntimeLayout" not in PERSIST

# Preserve the existing service ownership and build integration.
assert "IntelGtSurfaceRuntimeState *fSurfaceRuntime;" in BRINGUP
assert "IGGtSurfaceHandleArena fSurfaceHandles" in BRINGUP
assert "IntelGtPersistentSurfaces.cpp" in MAKEFILE
assert "IntelGtBringupSurface.cpp" in MAKEFILE

print("intel persistent surface handle userclient safety shape: ok")
