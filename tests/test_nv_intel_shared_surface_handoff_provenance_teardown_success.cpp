// SPDX-License-Identifier: MIT
#include "NvIntelSharedSurfaceHandoffProvenance.hpp"

#include <cstdio>

static NvIntelSharedCopyLifetimePlan retiredCopy()
{
    NvIntelSharedCopyProgress p = {};
    p.peerMappingValidated = true;
    p.channelSubmitReady = true;
    p.copySubmissionPublished = true;
    p.trackingFencePublishedAfterCopy = true;
    p.boundedCompletionWaitUsed = true;
    p.boundedCompletionWaitFinished = true;
    p.completionPayloadReadSucceeded = true;
    p.completedBeforeSubmission = 200ull;
    p.submittedTrackingValue = 201ull;
    p.observedCompletionPayload = 201u;
    p.maxCredibleCompletionJump = 8ull;

    NvIntelSharedCopyLifetimePlan out = {};
    if (nvIntelSharedCopyLifetimePlan(p, &out) != NV_INTEL_COPY_OK)
        return {};
    return out;
}

int main()
{
    const NvIntelSharedCopyLifetimePlan copy = retiredCopy();
    if (!copy.valid || !copy.completionProven) {
        std::puts("failed to construct retired copy plan");
        return 1;
    }

    NvIntelSharedSurfaceIdentity identity = {};
    identity.allocationToken = 0x5566778899aabbccull;
    identity.contentGeneration = 31ull;

    NvIntelSharedSurfaceHandoffProgress progress = {};
    progress.sharedSurfaceIdentity = identity;
    progress.copyCompletionIdentity = identity;
    progress.intelTargetIdentity = identity;
    progress.peerMappingTeardownAttempted = true;
    progress.peerMappingTeardownFinished = true;
    progress.peerMappingTeardownSucceeded = true;

    NvIntelSharedSurfaceHandoffProvenance provenance = {};
    provenance.handoffEpoch = 0x1111222233334444ull;
    provenance.copySubmissionToken = 0x2222333344445555ull;
    // Deliberately omit copyCompletionEvidence. Completion provenance is
    // therefore ambiguous even though the base lifetime planner saw retirement.

    NvIntelSharedSurfaceHandoffProvenancePlan out = {};
    if (nvIntelSharedSurfaceHandoffProvenancePlan(
            &copy, &progress, nullptr, &provenance, &out) !=
        NV_INTEL_HANDOFF_PROVENANCE_OK) {
        std::puts("ambiguous provenance after peer teardown was refused");
        return 1;
    }

    if (!out.provenanceUncertain ||
        out.handoff.phase != NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE ||
        out.handoff.copyRetirementProven ||
        !out.handoff.nvidiaMayStillReferenceSurface ||
        !out.handoff.peerMappingTeardownAttempted ||
        !out.handoff.peerMappingTeardownFinished ||
        !out.handoff.peerMappingTeardownSucceeded ||
        out.handoff.peerMappingMustRemainPinned ||
        out.handoff.peerMappingSlotReuseAuthorized ||
        out.handoff.quarantinePeerMapping ||
        !out.handoff.quarantineSharedBacking ||
        !out.handoff.sharedBackingMustRemainPinned ||
        out.handoff.sharedBackingReleaseAuthorized ||
        out.handoff.intelHandoffAuthorized ||
        out.handoff.intelMappingMayBegin ||
        out.runtimeWriteAuthorized ||
        out.handoff.runtimeWriteAuthorized) {
        std::puts("successful peer teardown was resurrected or its slot was reused under ambiguous provenance");
        return 1;
    }

    std::puts("ambiguous provenance preserves successful teardown and quarantines slot reuse");
    return 0;
}
