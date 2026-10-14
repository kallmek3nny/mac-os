#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "phase0-kext" / "IntelGtSurfaceStageService.cpp").read_text()
HPP = (ROOT / "phase0-kext" / "IntelGtSurfaceStageService.hpp").read_text()

required = [
    'PE_parse_boot_argn("iggt-surface-stage"',
    'constexpr UInt16 IntelVendor = 0x8086;',
    'constexpr UInt16 TigerLakeDevice = 0x9a68;',
    'IOBufferMemoryDescriptor::inTaskWithOptions(',
    'IOMapper::copyMapperForDevice(fPCI)',
    'IODMACommand::withSpecification(',
    'constexpr UInt8 IntelDmaAddressBits = 46;',
    'igGtSurfacePlan32(',
    'igGtSurfaceFlattenSegments(',
    'acceleratorPresent()',
    'setProperty("IGSurfaceStagePpgttMutated",false)',
    'setProperty("IGSurfaceStageMmioMapped",false)',
    'setProperty("IGSurfaceStageRawIovaPublished",false)',
    'setProperty("IGSurfaceStageReady",true)',
]
for token in required:
    assert token in CPP, f"missing required safety/runtime shape: {token}"

for token in [
    'inTaskWithPhysicalMask',
    'kIOMemoryPhysicallyContiguous',
    'getPhysicalSegment(',
    'getDeviceMemoryWithIndex(',
    'configWrite8(',
    'configWrite16(',
    'configWrite32(',
    'fPCI->open(',
    'volatile UInt32 *',
    'volatile UInt64 *',
    'write32(',
    'regWrite32(',
    'IGSurfaceStageDmaAddress',
    'IGSurfaceStageIova',
]:
    assert token not in CPP, f"allocation-only stage gained forbidden path: {token}"

assert 'IOBufferMemoryDescriptor *fBacking;' in HPP
assert 'IODMACommand *fDMA;' in HPP
assert 'IOMemoryMap' not in HPP

# The mapper stays bound after start() succeeds and is torn down only by the
# service lifecycle, which is the persistence contract for this staging step.
ready = CPP.index('setProperty("IGSurfaceStageReady",true)')
cleanup = CPP.index('void IntelGtSurfaceStageService::stop')
assert ready < cleanup

print("intel persistent surface runtime stage shape: ok")
