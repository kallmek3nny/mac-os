#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Enforce private GA107 GSP address transport and read-only public evidence."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
queue = (root / "phase0-kext/NVGspQueueBackingService.cpp").read_text()
boot = (root / "phase0-kext/NVGspBootArgsRehearsalService.cpp").read_text()
header = (root / "phase0-kext/NVGspQueueBackingService.hpp").read_text()

for key in (
    "NVGspQueueBackingGpuPageFirst",
    "NVGspQueueBackingGpuPageLast",
    "NVGspQueueBackingCandidateSharedMemGpuAddress",
):
    assert f'setProperty("{key}"' not in queue, key

for key in (
    "NVGspBootArgsRmArgsGpuAddress",
    "NVGspBootArgsLibosListGpuAddress",
    "NVGspBootArgsQueueSharedMemGpuAddress",
    "NVGspBootArgsMailbox0Value",
    "NVGspBootArgsMailbox1Value",
):
    assert f'setProperty("{key}"' not in boot, key

assert 'copyPreparedInitArgs(NVGspQueueInitArguments *out) const' in header
assert "nvGspCopyPrivateQueueHandoff(" in queue
assert "fPreparedInitArgs=memoryPlan.initArgs;" in queue
assert "fPrepared=false;" in queue and "fPreparedInitArgs={};" in queue
assert 'OSDynamicCast(NVGspQueueBackingService,queue)' in boot
assert "backing->copyPreparedInitArgs(&queueArgs)" in boot
assert 'numberProperty(queue,' not in boot
assert "queue->release();" in boot
assert 'NVGspQueueBackingGspNotified",false' in queue
assert 'NVGspBootArgsGspNotified",false' in boot
assert "OSReadLittleInt32" not in queue
assert "OSWriteLittleInt32" not in queue
assert 'vendor==0x10deu && device==0x25a2u && baseClass==0x03u' in queue
assert 'vendor==0x10deu && device==0x25a2u && baseClass==0x03u' in boot
assert "memoryPlan.initArgs.sharedMemPhysAddr);" not in queue
assert "(unsigned long long)rmGpu" not in boot
assert "(unsigned long long)listGpu" not in boot
assert "mailbox.writes[0].value," not in boot
assert "mailbox.writes[1].value," not in boot
print("private GA107 GSP queue address boundary: source checks passed")
