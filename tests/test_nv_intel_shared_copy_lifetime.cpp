// SPDX-License-Identifier: MIT
#include "NvIntelSharedCopyLifetime.hpp"

#include <cstdio>
#include <cstring>

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static void checkTrue(bool cond, const char *what)
{
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::printf("FAIL: %s\n", what);
    }
}

static NvIntelSharedCopyProgress peerReady()
{
    NvIntelSharedCopyProgress p = {};
    p.peerMappingValidated = true;
    p.channelSubmitReady = true;
    return p;
}

static NvIntelSharedCopyProgress copyPublished()
{
    auto p = peerReady();
    p.copySubmissionPublished = true;
    return p;
}

static NvIntelSharedCopyProgress fencePublished(NVIntelCopyU64 completed,
                                                NVIntelCopyU64 target,
                                                NVIntelCopyU64 maxJump)
{
    auto p = copyPublished();
    p.trackingFencePublishedAfterCopy = true;
    p.completedBeforeSubmission = completed;
    p.submittedTrackingValue = target;
    p.maxCredibleCompletionJump = maxJump;
    return p;
}

static void poison(NvIntelSharedCopyLifetimePlan *out)
{
    std::memset(out, 0xA5, sizeof(*out));
}

static bool sameBytes(const NvIntelSharedCopyLifetimePlan &a,
                      const NvIntelSharedCopyLifetimePlan &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static void expectRefusal(const NvIntelSharedCopyProgress &progress,
                          NVIntelCopyU32 expected,
                          const char *what)
{
    NvIntelSharedCopyLifetimePlan out = {};
    poison(&out);
    const auto before = out;
    const auto verdict = nvIntelSharedCopyLifetimePlan(progress, &out);
    checkTrue(verdict == expected, what);
    checkTrue(sameBytes(out, before), "refusal leaves output untouched");
}

static void checkAlwaysPure(const NvIntelSharedCopyLifetimePlan &p)
{
    checkTrue(!p.runtimeWriteAuthorized,
          "lifetime planner never authorizes runtime writes");
    if (p.intelHandoffAuthorized) {
        checkTrue(p.completionProven,
              "intel handoff requires proven nvidia completion");
        checkTrue(p.intelGgttMappingMayBegin,
              "proven handoff admits intel ggtt mapping phase");
        checkTrue(!p.nvidiaMayReferenceSink,
              "retired copy no longer owns nvidia sink reference");
    }
}

static void testPrePeerAndPeerReady()
{
    std::puts("1. no-submit phases never invent intel handoff");

    NvIntelSharedCopyProgress progress = {};
    NvIntelSharedCopyLifetimePlan out = {};
    checkTrue(nvIntelSharedCopyLifetimePlan(progress, &out) == NV_INTEL_COPY_OK,
          "pre-peer state accepted");
    checkTrue(out.valid && out.phase == NV_INTEL_COPY_PHASE_PRE_PEER,
          "pre-peer phase classified");
    checkTrue(!out.sinkBackingMustRemainPinned &&
              !out.peerMappingMustRemainPinned,
          "no peer map creates no nvidia lifetime");
    checkTrue(!out.intelHandoffAuthorized &&
              !out.intelGgttMappingMayBegin,
          "pre-peer state does not claim copy completion");
    checkTrue(!out.peerMappingTeardownMayBeAttempted,
          "no peer map means no peer teardown");
    checkAlwaysPure(out);

    progress = peerReady();
    checkTrue(nvIntelSharedCopyLifetimePlan(progress, &out) == NV_INTEL_COPY_OK,
          "validated peer-ready state accepted");
    checkTrue(out.phase == NV_INTEL_COPY_PHASE_PEER_READY,
          "peer-ready phase classified");
    checkTrue(out.sinkBackingMustRemainPinned,
          "validated peer map pins sink backing");
    checkTrue(!out.nvidiaMayReferenceSink &&
              !out.peerMappingMustRemainPinned,
          "before submission gpu does not own a copy reference");
    checkTrue(out.peerMappingTeardownMayBeAttempted &&
              out.peerMappingTeardownRequiredBeforeNvidiaIovaReuse,
          "unused peer map can be torn down before iova reuse");
    checkTrue(!out.intelHandoffAuthorized,
          "peer mapping alone is not nvidia copy completion");
    checkAlwaysPure(out);
}

static void testPublishedCopyPinsUntilTrackingFence()
{
    std::puts("2. published copy pins sink and peer map until tracking fence");

    auto progress = copyPublished();
    NvIntelSharedCopyLifetimePlan out = {};
    checkTrue(nvIntelSharedCopyLifetimePlan(progress, &out) == NV_INTEL_COPY_OK,
          "published copy without fence accepted as in-flight state");
    checkTrue(out.phase == NV_INTEL_COPY_PHASE_COPY_UNTRACKED,
          "untracked copy phase classified");
    checkTrue(out.nvidiaMayReferenceSink &&
              out.sinkBackingMustRemainPinned &&
              out.peerMappingMustRemainPinned,
          "in-flight copy pins all nvidia-visible sink resources");
    checkTrue(out.trackingFencePublicationRequired,
          "copy publication requires ordered tracking fence");
    checkTrue(!out.peerMappingTeardownMayBeAttempted &&
              !out.intelHandoffAuthorized,
          "untracked copy permits neither teardown nor intel handoff");
    checkTrue(out.quarantineOnAbortOrUncertainCompletion,
          "abort after untracked submission retains resources");
    checkAlwaysPure(out);
}

static void testAwaitingCompletionRejectsStaleProgress()
{
    std::puts("3. ordered fence below target keeps nvidia ownership live");

    auto progress = fencePublished(10ull, 12ull, 8ull);
    progress.boundedCompletionWaitUsed = true;
    progress.completionPayloadReadSucceeded = true;
    progress.observedCompletionPayload = 11u;

    NvIntelSharedCopyLifetimePlan out = {};
    checkTrue(nvIntelSharedCopyLifetimePlan(progress, &out) == NV_INTEL_COPY_OK,
          "below-target completion observation accepted as pending");
    checkTrue(out.phase == NV_INTEL_COPY_PHASE_AWAIT_COMPLETION,
          "below-target fence remains awaiting completion");
    checkTrue(out.completionEvidencePresent && !out.completionProven,
          "below-target payload is evidence but not retirement proof");
    checkTrue(out.observedCompletedValue == 11ull,
          "extended completion value retained");
    checkTrue(out.nvidiaMayReferenceSink &&
              out.peerMappingMustRemainPinned &&
              out.sinkBackingMustRemainPinned,
          "below-target completion preserves nvidia lifetime");
    checkTrue(!out.intelHandoffAuthorized &&
              !out.peerMappingTeardownMayBeAttempted,
          "stale completion cannot hand sink to intel");
    checkAlwaysPure(out);
}

static void testExactCompletionAuthorizesHandoff()
{
    std::puts("4. ordered tracking completion is the nvidia-to-intel handoff boundary");

    auto progress = fencePublished(100ull, 101ull, 8ull);
    progress.boundedCompletionWaitUsed = true;
    progress.boundedCompletionWaitFinished = true;
    progress.completionPayloadReadSucceeded = true;
    progress.observedCompletionPayload = 101u;

    NvIntelSharedCopyLifetimePlan out = {};
    checkTrue(nvIntelSharedCopyLifetimePlan(progress, &out) == NV_INTEL_COPY_OK,
          "target completion accepted");
    checkTrue(out.phase == NV_INTEL_COPY_PHASE_RETIRED_FOR_INTEL,
          "retired-for-intel phase classified");
    checkTrue(out.completionProven && out.observedCompletedValue == 101ull,
          "target tracking value proves ordered retirement");
    checkTrue(!out.nvidiaMayReferenceSink &&
              !out.peerMappingMustRemainPinned,
          "retired copy releases nvidia reference lifetime");
    checkTrue(out.sinkBackingMustRemainPinned,
          "sink remains pinned across ownership handoff");
    checkTrue(out.intelHandoffAuthorized && out.intelGgttMappingMayBegin,
          "only proven nvidia retirement admits intel mapping");
    checkTrue(out.peerMappingTeardownMayBeAttempted &&
              out.peerMappingTeardownRequiredBeforeNvidiaIovaReuse,
          "retired peer mapping may be torn down before iova reuse");
    checkTrue(!out.quarantineOnAbortOrUncertainCompletion,
          "proven completion clears uncertainty policy");
    checkAlwaysPure(out);
}

static void testWrapAwareCompletion()
{
    std::puts("5. 32-bit semaphore wrap preserves 64-bit retirement ordering");

    auto progress = fencePublished(0x00000000ffffffffull,
                                   0x0000000100000000ull,
                                   4ull);
    progress.boundedCompletionWaitUsed = true;
    progress.boundedCompletionWaitFinished = true;
    progress.completionPayloadReadSucceeded = true;
    progress.observedCompletionPayload = 0u;

    NvIntelSharedCopyLifetimePlan out = {};
    checkTrue(nvIntelSharedCopyLifetimePlan(progress, &out) == NV_INTEL_COPY_OK,
          "wrapped tracking payload accepted");
    checkTrue(out.completionProven &&
              out.observedCompletedValue == 0x0000000100000000ull,
          "wrap extends to exact next 64-bit completion value");
    checkTrue(out.phase == NV_INTEL_COPY_PHASE_RETIRED_FOR_INTEL &&
              out.intelHandoffAuthorized,
          "wrapped but ordered completion authorizes handoff");
    checkAlwaysPure(out);
}

static void testTimeoutQuarantinesUntilFenceProof()
{
    std::puts("6. bounded timeout below target quarantines both shared resources");

    auto progress = fencePublished(200ull, 202ull, 8ull);
    progress.boundedCompletionWaitUsed = true;
    progress.boundedCompletionWaitFinished = true;
    progress.completionPayloadReadSucceeded = true;
    progress.completionWaitTimedOut = true;
    progress.observedCompletionPayload = 201u;

    NvIntelSharedCopyLifetimePlan out = {};
    checkTrue(nvIntelSharedCopyLifetimePlan(progress, &out) == NV_INTEL_COPY_OK,
          "timed-out below-target observation accepted conservatively");
    checkTrue(out.phase == NV_INTEL_COPY_PHASE_UNCERTAIN_QUARANTINE,
          "timeout enters uncertain quarantine");
    checkTrue(out.completionUncertain &&
              out.quarantineSinkBacking && out.quarantinePeerMapping,
          "timeout quarantines sink backing and peer mapping");
    checkTrue(out.nvidiaMayReferenceSink &&
              out.peerMappingMustRemainPinned,
          "timeout cannot retire possible nvidia reference");
    checkTrue(!out.intelHandoffAuthorized &&
              !out.peerMappingTeardownMayBeAttempted,
          "timeout cannot hand off or teardown peer mapping");
    checkAlwaysPure(out);

    // Unlike a mailbox heuristic, the ordered tracking semaphore itself is the
    // retirement primitive. A later exact observation can therefore resolve a
    // prior bounded-wait timeout without pretending the timeout was success.
    progress.observedCompletionPayload = 202u;
    checkTrue(nvIntelSharedCopyLifetimePlan(progress, &out) == NV_INTEL_COPY_OK,
          "late exact tracking completion accepted");
    checkTrue(out.phase == NV_INTEL_COPY_PHASE_RETIRED_FOR_INTEL &&
              out.completionProven && !out.completionUncertain,
          "exact fence proof resolves earlier timing uncertainty");
    checkTrue(out.intelHandoffAuthorized &&
              !out.quarantineSinkBacking && !out.quarantinePeerMapping,
          "resolved tracking proof enables safe handoff");
    checkAlwaysPure(out);
}

static void testContradictionsRefuseAtomically()
{
    std::puts("7. impossible ordering and forged counter evidence refuse atomically");

    auto p = copyPublished();
    p.peerMappingValidated = false;
    expectRefusal(p, NV_INTEL_COPY_ERR_SEQUENCE,
                  "copy without peer mapping refused");

    p = copyPublished();
    p.channelSubmitReady = false;
    expectRefusal(p, NV_INTEL_COPY_ERR_CHANNEL,
                  "copy without submit-ready channel refused");

    p = peerReady();
    p.trackingFencePublishedAfterCopy = true;
    expectRefusal(p, NV_INTEL_COPY_ERR_SEQUENCE,
                  "tracking fence without copy refused");

    p = copyPublished();
    p.boundedCompletionWaitUsed = true;
    expectRefusal(p, NV_INTEL_COPY_ERR_SEQUENCE,
                  "completion wait before tracking fence refused");

    p = fencePublished(10ull, 11ull, 8ull);
    p.boundedCompletionWaitFinished = true;
    expectRefusal(p, NV_INTEL_COPY_ERR_SEQUENCE,
                  "finished wait without started wait refused");

    p = fencePublished(10ull, 11ull, 8ull);
    p.completionPayloadReadSucceeded = true;
    expectRefusal(p, NV_INTEL_COPY_ERR_SEQUENCE,
                  "completion payload without bounded wait refused");

    p = fencePublished(10ull, 11ull, 8ull);
    p.boundedCompletionWaitUsed = true;
    p.completionWaitTimedOut = true;
    expectRefusal(p, NV_INTEL_COPY_ERR_SEQUENCE,
                  "timeout before bounded wait finishes refused");

    p = fencePublished(10ull, 10ull, 8ull);
    expectRefusal(p, NV_INTEL_COPY_ERR_TRACKING_TARGET,
                  "non-new tracking target refused");

    p = fencePublished(10ull, 20ull, 8ull);
    expectRefusal(p, NV_INTEL_COPY_ERR_TRACKING_TARGET,
                  "tracking target outside credible queue jump refused");

    p = fencePublished(10ull, 11ull, 8ull);
    p.boundedCompletionWaitUsed = true;
    p.completionPayloadReadSucceeded = true;
    p.observedCompletionPayload = 100u;
    expectRefusal(p, NV_INTEL_COPY_ERR_COMPLETION_EVIDENCE,
                  "implausibly large observed completion jump refused");

    p = fencePublished(0xfffffffffffffffeull,
                       0xffffffffffffffffull,
                       4ull);
    p.boundedCompletionWaitUsed = true;
    p.completionPayloadReadSucceeded = true;
    p.observedCompletionPayload = 0u;
    expectRefusal(p, NV_INTEL_COPY_ERR_COMPLETION_EVIDENCE,
                  "64-bit completion counter overflow refused");

    checkTrue(nvIntelSharedCopyLifetimePlan(peerReady(), nullptr) ==
              NV_INTEL_COPY_ERR_NULL_OUTPUT,
          "null output refused");
}

int main()
{
    testPrePeerAndPeerReady();
    testPublishedCopyPinsUntilTrackingFence();
    testAwaitingCompletionRejectsStaleProgress();
    testExactCompletionAuthorizesHandoff();
    testWrapAwareCompletion();
    testTimeoutQuarantinesUntilFenceProof();
    testContradictionsRefuseAtomically();

    std::printf("\n%d checks, %d failures\n", static_cast<int>(gChecks), static_cast<int>(gFailures));
    return gFailures == 0 ? 0 : 1;
}
