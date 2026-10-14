#!/usr/bin/env python3
from pathlib import Path
import plistlib

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "phase0-kext" / "IntelGtSurfaceBringup.cpp").read_text()
HPP = (ROOT / "phase0-kext" / "IntelGtBringup.hpp").read_text()
DERIVED = (ROOT / "phase0-kext" / "IntelGtSurfaceBringup.hpp").read_text()
MAKE = (ROOT / "phase0-kext" / "Makefile").read_text()
PLIST_PATH = ROOT / "phase0-kext" / "Info.plist"

required_cpp = [
    'PE_parse_boot_argn("iggt-surface-arena"',
    'PE_parse_boot_argn("iggt-bringup"',
    'stage!=3',
    'IntelGtBringup::start(provider)',
    'surfaceExtensionReady()',
    'configRead16(kIOPCIConfigVendorID)!=0x8086',
    'configRead16(kIOPCIConfigDeviceID)!=0x9a68',
    'IOBufferMemoryDescriptor::inTaskWithOptions(',
    'IOMapper::copyMapperForDevice(pci)',
    'IODMACommand::withSpecification(',
    'igGtSurfaceFlattenSegments(',
    'igGtSurfaceBuildPtes(',
    'surfaceLivePatIndex()',
    'igGtSurfacePlanInstall(',
    'pt[leaf]=ptes[i]',
    'if (pt[leaf]!=ptes[i])',
    'pt[leaf]=fSurfaceSavedPtes[i]',
    'if (pt[leaf]!=fSurfaceSavedPtes[i])',
    'igGtSurfaceLeafImageMatches(',
    'IntelGtBringup::stop(provider)',
    'setProperty("GTSurfaceArenaGpuUseReady",false)',
    'setProperty("GTSurfaceArenaRawIovaPublished",false)',
    'rollback-readback-failed-retained',
]
for token in required_cpp:
    assert token in CPP, f"missing persistent-surface safety/runtime shape: {token}"

for token in [
    'write32(',
    'configWrite8(',
    'configWrite16(',
    'configWrite32(',
    'getDeviceMemoryWithIndex(',
    'inTaskWithPhysicalMask',
    'kIOMemoryPhysicallyContiguous',
    'getPhysicalSegment(',
    'GTSurfaceArenaGpuBase',
    'GTSurfaceArenaIova',
    'GTSurfaceArenaDmaAddress',
    'setProperty("GTSurfaceArenaGpuUseReady",true)',
]:
    assert token not in CPP, f"surface extension gained forbidden path/exposure: {token}"

assert 'class IntelGtSurfaceBringup : public IntelGtBringup' in DERIVED
assert 'UInt64 *surfacePrivatePt(void) const' in HPP
assert 'fCPU + 23u * 4096u' in HPP
assert 'surfaceLivePatIndex(void) const' in HPP
assert 'fStage == 3u' in HPP and 'fRetired' in HPP and '!fFaulted' in HPP
assert 'IntelGtSurfaceBringup.cpp' in MAKE

# Base stop must disable the inherited queue before the derived class can tear
# down leaves or the device mapping.
stop = CPP.index('void IntelGtSurfaceBringup::stop')
base_stop = CPP.index('IntelGtBringup::stop(provider)', stop)
restore = CPP.index('restoreSurfaceArena()', base_stop)
release = CPP.index('releaseSurfaceArena()', restore)
assert base_stop < restore < release

# Mapping teardown is forbidden while a leaf could still point at the DMA map.
release_fn = CPP[CPP.index('void IntelGtSurfaceBringup::releaseSurfaceArena'):CPP.index('bool IntelGtSurfaceBringup::restoreSurfaceArena')]
assert 'if (fSurfaceInstalled || fSurfaceRestoreFailed) return;' in release_fn
assert 'clearMemoryDescriptor()' in release_fn

with PLIST_PATH.open('rb') as f:
    plist = plistlib.load(f)
p = plist['IOKitPersonalities']['IntelGtBringup']
assert p['IOClass'] == 'IntelGtSurfaceBringup'
assert p['IOProviderClass'] == 'IOPCIDevice'
assert p['IOPCIMatch'].lower() == '0x9a688086'
assert p['IOMatchCategory'] == 'IntelGtBringup'

print('intel persistent private-PPGTT surface bring-up shape: ok')
