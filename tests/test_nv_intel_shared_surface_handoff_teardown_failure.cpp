// SPDX-License-Identifier: MIT
#include "NvIntelSharedSurfaceHandoff.hpp"

#include <cstdio>

static NvIntelSharedCopyLifetimePlan retiredCopy()
{
    NvIntelSharedCopyProgress progress = {};
    progress.peerMappingValidated = true;
    progress.channelSubmitReady = true;
    progress.copySubmissionPublished = true;
    progress.trackingFencePublishedAfterCopy = true;
    progress.boundedCompletionWaitUsed = true;
    progress.boundedCompletionWaitFinished = true;
    progress.completionPayloadReadSucceeded = true;
    progress.completedBeforeSubmission = 40ull;
    progress.submittedTrackingValue = 41ull;
    progress.observedCompletionPayload = 41u;
    progress.maxCredibleCompletionJump = 8ull;

    NvIntelSharedCopyLifetimePlan out = {};
    if (nvIntelSharedCopyLifetimePlan(progress, &out) != NV_INTEL_COPY_OK)
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
    identity.allocationToken = 0x8899aabbccddeeffull;
    identity.contentGeneration = 22ull;

    NvIntelSharedSurfaceHandoffProgress progress = {};
    progress.sharedSurfaceIdentity = identity;
    progress.copyCompletionIdentity = identity;
    progress.intelTargetIdentity = identity;
    progress.peerMappingTeardownAttempted = true;
    progress.peerMappingTeardownFinished = true;
    progress.peerMappingTeardownSucceeded = false;

    NvIntelSharedSurfaceHandoffPlan out = {};
    if (nvIntelSharedSurfaceHandoffPlan(&copy, &progress, nullptr, &out) !=
        NV_INTEL_HANDOFF_OK) {
        std::puts("failed peer teardown plan was refused instead of quarantined");
        return 1;
    }

    if (out.phase != NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE ||
        !out.lifetimeUncertain || out.completionUncertain ||
        !out.quarantineSharedBacking || !out.quarantinePeerMapping ||
        out.quarantineIntelMapping || !out.peerMappingMustRemainPinned ||
        out.peerMappingSlotReuseAuthorized ||
        !out.sharedBackingMustRemainPinned ||
        out.sharedBackingReleaseAuthorized ||
        out.runtimeWriteAuthorized) {
        std::puts("failed peer teardown did not produce conservative lifetime quarantine");
        return 1;
    }

    std::puts("failed peer teardown lifetime quarantine ok");
    return 0;
}
