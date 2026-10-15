#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SURFACE = (ROOT / "phase0-kext" / "IntelGtBringupSurface.cpp").read_text()
BRINGUP = (ROOT / "phase0-kext" / "IntelGtBringup.cpp").read_text()
HPP = (ROOT / "phase0-kext" / "IntelGtBringup.hpp").read_text()
MAKEFILE = (ROOT / "phase0-kext" / "Makefile").read_text()

required_surface = [
    'PE_parse_boot_argn("iggt-surface-install"',
    'IOBufferMemoryDescriptor::inTaskWithOptions(',
    'IOMapper::copyMapperForDevice(fPCI)',
    'IODMACommand::withSpecification(',
    'IntelDmaAddressBits = 46',
    'igGtSurfaceFlattenSegments(',
    'igGtSurfaceBuildPtes(',
    'pages,SurfacePages,fPatIndex,state->installedPtes',
    'igGtSurfaceTxnPrepare(',
    'IG_GTS_ARENA_FIRST_LEAF_64',
    'igGtSurfaceTxnCommit(',
    'igGtSurfaceTxnRestore(',
    'state->snapshot',
    'state->installedPtes',
    'state->unsafe=true;',
    'fQuarantined=true;',
    'GTBringupSurfaceRawGpuAddressPublished",false',
    'GTBringupSurfaceRawIovaPublished",false',
    'constexpr UInt32 PrivatePtPoolOffset = 23u * PageBytes;',
]
for token in required_surface:
    assert token in SURFACE, f"missing integrated surface safety shape: {token}"

# The runtime extension inherits PCI ownership from IntelGtBringup. It must not
# grow its own provider-open/MMIO/config-write path or publish a usable address.
for token in [
    'fPCI->open(',
    'fPCI->close(',
    'getDeviceMemoryWithIndex(',
    'configWrite8(',
    'configWrite16(',
    'configWrite32(',
    'fMMIO',
    'regWrite32(',
    'PHASE4_ALLOW_WRITES',
    'GTBringupSurfaceGpuAddress',
    'GTBringupSurfaceIova',
    'GTBringupSurfaceRawGpuAddressPublished",true',
    'GTBringupSurfaceRawIovaPublished",true',
]:
    assert token not in SURFACE, f"integrated surface runtime gained forbidden path: {token}"

assert 'IntelGtSurfaceRuntimeState *fSurfaceRuntime;' in HPP
assert 'bool stagePersistentSurface(void);' in HPP
assert 'bool releasePersistentSurface(void);' in HPP
assert 'IntelGtBringupSurface.cpp' in MAKEFILE

# Pin the surface writer to the exact PT page used by allocateMemory(). If the
# pool layout ever moves, this test forces both sides to be reviewed together.
assert 'Pt = 23 * 4096;' in BRINGUP
assert 'PrivatePtPoolOffset = 23u * PageBytes;' in SURFACE

# The extra install gate is reached only after exact-device/exclusive-provider
# checks, cold BCS0 claim, PAT discovery and private-root allocation. Teardown
# restores those private leaves before the root/DMA pool can disappear.
start = BRINGUP.index('bool IntelGtBringup::start')
claim = BRINGUP.index('claimEngine()', start)
allocate = BRINGUP.index('allocateMemory()', claim)
stage = BRINGUP.index('stagePersistentSurface()', allocate)
ggtt = BRINGUP.index('installGGTT()', stage)
assert claim < allocate < stage < ggtt

cleanup = BRINGUP.index('void IntelGtBringup::cleanup')
restore_surface = BRINGUP.index('releasePersistentSurface()', cleanup)
restore_ggtt = BRINGUP.index('restoreGGTT();', restore_surface)
clear_pool_dma = BRINGUP.index('fDMA->clearMemoryDescriptor()', restore_ggtt)
assert cleanup < restore_surface < restore_ggtt < clear_pool_dma

# The old no-boot-arg and accelerator ownership refusal remain in the live path.
assert 'PE_parse_boot_argn("iggt-bringup"' in BRINGUP
assert 'accelerator/provider already owned; no hardware writes' in BRINGUP
assert 'not the supported 8086:9a68 Tiger Lake device' in BRINGUP
assert 'BCS0 already configured/busy; no takeover or reset attempted' in BRINGUP

print("intel integrated persistent surface install shape: ok")
