// SPDX-License-Identifier: MIT
// Bounded GA107M PCI ownership witness for read-only PGSP queue observation.
// No IOKit, GPU I/O, register writes, or hardware-read authority.
#ifndef NV_GSP_QUEUE_LIVE_EVIDENCE_HPP
#define NV_GSP_QUEUE_LIVE_EVIDENCE_HPP

#include "NVGa107PciIdentity.hpp"
#include "NVGspQueueObserve.hpp"

struct NVGspQueuePciSample {
    NVGspObsU32 vendor;
    NVGspObsU32 device;
    NVGspObsU32 classRevision;
    NVGspObsU32 command;
};

// A stable two-pass BAR0 sample is not valid evidence if the PCI function or
// MEMORY enable changed while the provider was reading it. This is a bounded
// before/after read-only consistency guard, not an atomic device transaction,
// a GSP-ready signal, or authorization to submit anything to the device.
static inline bool nvGspQueueLiveEvidenceCanPublish(
    const NVGspQueuePciSample *before,
    const NVGspQueuePciSample *after,
    NVGspObsU32 verdict,
    const NVGspQueueSnapshot *snapshot)
{
    if (!before || !after ||
        !nvGspQueueSnapshotCanPublish(verdict, snapshot))
        return false;
    if (!nvGa107PciIdentityEligible(before->vendor, before->device,
                                    before->classRevision) ||
        !nvGa107PciIdentityEligible(after->vendor, after->device,
                                    after->classRevision))
        return false;
    // MEMORY enable must be present at both observation boundaries. Refuse
    // any command-word drift, including bus mastering or other status changes:
    // neither a BAR aperture nor DMA/queue ownership may be inferred through
    // a PCI configuration transition.
    return (before->command & 0x0002u) != 0u &&
           before->vendor == after->vendor &&
           before->device == after->device &&
           before->classRevision == after->classRevision &&
           before->command == after->command;
}

#endif // NV_GSP_QUEUE_LIVE_EVIDENCE_HPP
