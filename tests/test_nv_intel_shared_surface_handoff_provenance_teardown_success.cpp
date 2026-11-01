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

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static void checkBool(bool cond, const char *what)
{
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::printf("FAIL: %s\n", what);
    }
}

int main()
{
    const NvIntelSharedCopyLifetimePlan copy = retiredCopy();
    const bool copyRetired = copy.valid && copy.completionProven;
    checkBool(copyRetired, "failed to construct retired copy plan");

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

    // These were early returns, which skipped the count line. Each later check
    // still runs only when the state it depends on holds: the planner is only
    // meaningful for a retired copy, and out is only meaningful once accepted.
    bool planned = false;
    if (copyRetired) {
        planned = nvIntelSharedSurfaceHandoffProvenancePlan(
                      &copy, &progress, nullptr, &provenance, &out) ==
                  NV_INTEL_HANDOFF_PROVENANCE_OK;
        checkBool(planned, "ambiguous provenance after peer teardown was refused");
    }

    if (planned) {
        // One assertion per lifetime fact, so a regression names the exact field.
        checkBool(out.valid, "accepted provenance plan is not marked valid");
        checkBool(out.handoff.valid, "accepted provenance plan carries an invalid handoff plan");
        checkBool(out.provenanceUncertain, "omitted completion evidence did not mark provenance uncertain");
        checkBool(out.handoff.phase == NV_INTEL_HANDOFF_PHASE_UNCERTAIN_QUARANTINE,
                  "ambiguous provenance did not enter uncertain quarantine");
        checkBool(out.handoff.lifetimeUncertain, "ambiguous provenance did not mark the handoff lifetime uncertain");
        checkBool(nvIntelSharedSurfaceIdentityEqual(out.handoff.identity, identity),
                  "provenance plan lost the shared-surface identity");
        checkBool(out.handoffEpoch == provenance.handoffEpoch, "provenance plan did not carry the handoff epoch");
        checkBool(out.copySubmissionToken == provenance.copySubmissionToken,
                  "provenance plan did not carry the copy submission token");
        checkBool(out.copySubmissionProvenanceRequired, "published copy submission did not require submission provenance");
        checkBool(out.copySubmissionProvenanceBound, "epoch plus submission token did not bind the copy submission");
        checkBool(out.copyCompletionProvenanceRequired, "proven copy completion did not require completion provenance");
        checkBool(!out.copyCompletionProvenanceBound, "omitted completion evidence was reported as bound");
        checkBool(!out.intelDisplayTransactionProvenanceRequired,
                  "display transaction provenance was required with no display plan");
        checkBool(!out.intelDisplayEvidenceProvenanceRequired,
                  "display evidence provenance was required with no display plan");
        checkBool(!out.handoff.copyRetirementProven, "ambiguous completion evidence still proved copy retirement");
        checkBool(out.handoff.nvidiaMayStillReferenceSurface,
                  "ambiguous completion evidence cleared NVIDIA's possible surface reference");
        checkBool(!out.handoff.peerMappingTeardownMayBeAttempted,
                  "ambiguous provenance still permits a peer teardown attempt");
        checkBool(out.handoff.peerMappingTeardownAttempted, "peer teardown attempt was erased");
        checkBool(out.handoff.peerMappingTeardownFinished, "peer teardown completion was erased");
        checkBool(out.handoff.peerMappingTeardownSucceeded, "successful peer teardown was resurrected as unsuccessful");
        checkBool(!out.handoff.peerMappingMustRemainPinned, "torn-down peer mapping was pinned again");
        checkBool(!out.handoff.peerMappingSlotReuseAuthorized,
                  "peer mapping slot reuse was authorized under ambiguous provenance");
        checkBool(!out.handoff.quarantinePeerMapping, "torn-down peer mapping was quarantined again");
        checkBool(out.handoff.quarantineSharedBacking, "shared backing was not quarantined under ambiguous provenance");
        checkBool(out.handoff.sharedBackingMustRemainPinned, "shared backing was not pinned under ambiguous provenance");
        checkBool(!out.handoff.sharedBackingReleaseAuthorized,
                  "shared backing release was authorized under ambiguous provenance");
        checkBool(!out.handoff.intelHandoffAuthorized, "Intel handoff was authorized under ambiguous provenance");
        checkBool(!out.handoff.intelMappingMayBegin, "Intel mapping may begin under ambiguous provenance");
        checkBool(!out.runtimeWriteAuthorized, "provenance plan authorized runtime writes");
        checkBool(!out.handoff.runtimeWriteAuthorized, "handoff plan authorized runtime writes");
    }

    // Matched control: the same handoff with exactly bound completion evidence
    // is not quarantined, so the quarantine above comes from the omission alone.
    if (copyRetired) {
        NvIntelSharedSurfaceHandoffProvenance matched = provenance;
        matched.copyCompletionEvidence.handoffEpoch = provenance.handoffEpoch;
        matched.copyCompletionEvidence.transactionToken = provenance.copySubmissionToken;

        NvIntelSharedSurfaceHandoffProvenancePlan bound = {};
        const bool boundPlanned =
            nvIntelSharedSurfaceHandoffProvenancePlan(
                &copy, &progress, nullptr, &matched, &bound) ==
            NV_INTEL_HANDOFF_PROVENANCE_OK;
        checkBool(boundPlanned, "exactly bound completion evidence was refused");
        if (boundPlanned) {
            checkBool(!bound.provenanceUncertain, "exactly bound completion evidence was reported uncertain");
            checkBool(bound.copyCompletionProvenanceBound, "exactly bound completion evidence was not reported bound");
            checkBool(bound.handoff.phase == NV_INTEL_HANDOFF_PHASE_NVIDIA_RETIRED,
                      "bound successful teardown did not settle in the NVIDIA-retired phase");
            checkBool(bound.handoff.peerMappingSlotReuseAuthorized,
                      "bound successful teardown did not authorize peer mapping slot reuse");
        }
    }

    // Replay control: a nonzero completion token naming a different submission
    // is refused before the output plan is written.
    if (copyRetired) {
        NvIntelSharedSurfaceHandoffProvenance replay = provenance;
        replay.copyCompletionEvidence.handoffEpoch = provenance.handoffEpoch;
        replay.copyCompletionEvidence.transactionToken = 0x9999aaaabbbbccccull;

        NvIntelSharedSurfaceHandoffProvenancePlan untouched = {};
        untouched.handoffEpoch = 0xfeedfacecafebeefull;
        checkBool(nvIntelSharedSurfaceHandoffProvenancePlan(
                      &copy, &progress, nullptr, &replay, &untouched) ==
                      NV_INTEL_HANDOFF_PROVENANCE_ERR_REPLAY,
                  "conflicting completion token was not refused as replay");
        checkBool(untouched.handoffEpoch == 0xfeedfacecafebeefull, "replay refusal overwrote the output plan");
        checkBool(!untouched.valid, "replay refusal marked the output plan valid");
    }

    if (gFailures == 0)
        std::printf("ambiguous provenance preserves successful teardown and quarantines slot reuse\n");

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
