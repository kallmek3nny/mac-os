#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT / "phase0-kext" / "IntelSinkCandidateService.cpp").read_text()
HPP = (ROOT / "phase0-kext" / "IntelSinkCandidateService.hpp").read_text()

required_header = [
    '#include "IntelSinkGgttTxn.hpp"',
    'UInt64 *fInstalledPtes;',
    'IGSinkGgttTxn fGgttTxn;',
    'IGSinkGgttTxnStats fCommitStats;',
    'IGSinkGgttTxnStats fRestoreStats;',
    'bool fGgttTxnPrepared;',
    'bool fGgttInstalled;',
    'bool fQuarantined;',
    'bool fGgttLifetimePinned;',
    'static bool readGgttPte(',
    'static bool writeGgttPte(',
    'bool releaseGgtt(void);',
]
for token in required_header:
    assert token in HPP, f"missing runtime transaction field/API: {token}"

required_service = [
    '"igsink-ggtt-map"',
    'acceleratorPresent() || !fPCIDevice->open(this)',
    'pteIndex>=service->fGgttEntries',
    'entryOffset>service->fBarBytes',
    '(UInt64)sizeof(UInt64)>service->fBarBytes-entryOffset',
    'OSSynchronizeIO();',
    'igSinkGgttTxnPrepare(',
    'igSinkGgttTxnCommit(',
    'igSinkGgttTxnRestore(',
    'pinGgttLifetime();',
    'retain();',
    'enterQuarantine("ggtt-commit-rollback-unproven")',
    'enterQuarantine("ggtt-restore-unproven")',
    'IGSinkCandidateGgttPrepareVerdict',
    'IGSinkCandidateGgttCommitVerdict',
    'IGSinkCandidateGgttRestoreVerdict',
    'IGSinkCandidateGgttCommitWritesAttempted',
    'IGSinkCandidateGgttCommitWritesVerified',
    'IGSinkCandidateGgttCommitRecoveryWrites',
    'IGSinkCandidateGgttRestoreWritesAttempted',
    'IGSinkCandidateGgttRestoreWritesVerified',
    'IGSinkCandidateGgttRestoreRecoveryWrites',
    'IGSinkCandidateGgttFailedPte',
    'IGSinkCandidateQuarantineState',
    'IGSinkCandidateQuarantineReason',
    'IGSinkCandidateGgttLifetimePinned',
]
for token in required_service:
    assert token in SERVICE, f"missing runtime transaction safety shape: {token}"

# No return to the old manual GGTT save/write/restore loops. All stores are now
# bounded callback stores, and exact readback/rollback/restore lives in the
# dependency-free transaction helper.
for token in [
    'pt[ggttPlan.firstPte+i]=',
    'pt[fMappedFirstPte+i]=',
    'fMappedPteCount=i+1u',
    'fSavedPtes[i]=old',
    'configWrite8(',
    'configWrite16(',
    'configWrite32(',
    'IGSinkCandidatePlaneProgrammed",true',
]:
    assert token not in SERVICE, f"runtime regained forbidden/manual write path: {token}"

# The service retain must be acquired before the first commit store can happen.
commit = SERVICE.index('fCommitVerdict=igSinkGgttTxnCommit(')
pin = SERVICE.rindex('pinGgttLifetime();', 0, commit)
assert pin < commit

# An unproven commit rollback is quarantined directly. It cannot flow into the
# normal GGTT release path.
commit_fail = SERVICE.index('if (!igSinkGgttTxnSucceeded(fCommitVerdict))', commit)
rollback_bad = SERVICE.index('fCommitVerdict==IG_SINK_GGTT_TXN_ROLLBACK_FAILED', commit_fail)
commit_quarantine = SERVICE.index('enterQuarantine("ggtt-commit-rollback-unproven")', rollback_bad)
commit_return = SERVICE.index('return true;', commit_quarantine)
commit_release = SERVICE.index('(void)releaseGgtt();', commit_return)
assert commit_fail < rollback_bad < commit_quarantine < commit_return < commit_release

# Restore failure must quarantine and return before snapshot/DMA/BAR/PCI cleanup.
release_fn = SERVICE.index('bool IntelSinkCandidateService::releaseGgtt')
restore = SERVICE.index('fRestoreVerdict=igSinkGgttTxnRestore(', release_fn)
restore_fail = SERVICE.index('if (!igSinkGgttTxnSucceeded(fRestoreVerdict))', restore)
restore_quarantine = SERVICE.index('enterQuarantine("ggtt-restore-unproven")', restore_fail)
restore_return = SERVICE.index('return false;', restore_quarantine)
free_snapshot = SERVICE.index('IOFree(fSavedPtes', restore_return)
clear_dma = SERVICE.index('fIntelDMA->clearMemoryDescriptor()', free_snapshot)
release_bar = SERVICE.index('OSSafeReleaseNULL(fBarMap)', clear_dma)
close_pci = SERVICE.index('fPCIDevice->close(this)', release_bar)
assert restore < restore_fail < restore_quarantine < restore_return < free_snapshot < clear_dma < release_bar < close_pci

# stop() gates every later dependency release on verified GGTT teardown. The
# quarantine return must happen before peer DMA or backing memory is touched.
stop = SERVICE.index('void IntelSinkCandidateService::stop')
stop_restore = SERVICE.index('const bool ggttReleased=releaseGgtt();', stop)
stop_fail = SERVICE.index('if (!ggttReleased)', stop_restore)
stop_return = SERVICE.index('return;', stop_fail)
peer_release = SERVICE.index('releasePeerDma();', stop_return)
backing_release = SERVICE.index('releaseBacking();', peer_release)
provider_release = SERVICE.index('OSSafeReleaseNULL(fPCIDevice)', backing_release)
assert stop < stop_restore < stop_fail < stop_return < peer_release < backing_release < provider_release

# free() is not allowed to become an emergency restore path. A mapped,
# quarantined, or lifetime-pinned service refuses resource teardown there.
free_fn = SERVICE.index('void IntelSinkCandidateService::free')
free_guard = SERVICE.index('if (fQuarantined || fGgttInstalled || fGgttLifetimePinned)', free_fn)
free_guard_return = SERVICE.index('return;', free_guard)
free_release = SERVICE.index('(void)releaseGgtt();', free_guard_return)
assert free_fn < free_guard < free_guard_return < free_release

# Preserve the existing exact-device/ownership/write opt-ins and keep display
# programming outside this runtime integration milestone. The map boot-arg is
# deliberately checked by exact literal rather than source formatting because
# the call is line-wrapped in the service.
assert 'return igIsSupportedDevice(vendor,device) ? result : nullptr;' in SERVICE
assert 'PE_parse_boot_argn("igsink-candidate"' in SERVICE
assert 'PE_parse_boot_argn(' in SERVICE
assert '"igsink-ggtt-map"' in SERVICE
assert 'IGSinkCandidatePlaneProgrammed",false' in SERVICE

print("intel sink GGTT runtime transaction/quarantine shape: ok")
