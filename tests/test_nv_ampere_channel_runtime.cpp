// SPDX-License-Identifier: MIT
#include "NVAmpereChannelRuntime.hpp"

#include <cstdio>

static unsigned gChecks = 0u;
static unsigned gFailures = 0u;

static void check(bool condition, const char *label)
{
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::printf("FAIL: %s\n", label);
    }
}

static bool sameRuntime(const NVAmpereChannelRuntime &a,
                        const NVAmpereChannelRuntime &b)
{
    return a.initialized == b.initialized &&
           a.ownershipUncertain == b.ownershipUncertain &&
           a.evidence.channelClassNum == b.evidence.channelClassNum &&
           a.evidence.numGpFifoEntries == b.evidence.numGpFifoEntries &&
           a.evidence.gpGetMapped == b.evidence.gpGetMapped &&
           a.evidence.gpPutMapped == b.evidence.gpPutMapped &&
           a.evidence.gpFifoMapped == b.evidence.gpFifoMapped &&
           a.evidence.workSubmissionMapped == b.evidence.workSubmissionMapped &&
           a.evidence.workSubmissionTokenReady ==
               b.evidence.workSubmissionTokenReady &&
           a.readiness.allocationReady == b.readiness.allocationReady &&
           a.readiness.ceBoundReady == b.readiness.ceBoundReady &&
           a.readiness.submitReady == b.readiness.submitReady &&
           a.allocationObserved == b.allocationObserved &&
           a.submissionEndpointObserved == b.submissionEndpointObserved &&
           a.sealed == b.sealed &&
           a.runtimeWriteAuthorized == b.runtimeWriteAuthorized;
}

static NVAmpereChannelRuntime allocatedRuntime()
{
    NVAmpereChannelRuntime runtime = {};
    check(nvAmpereChannelRuntimeInit(&runtime) == NV_CHANNEL_RUNTIME_OK,
          "runtime initializes");
    check(nvAmpereChannelRuntimeObserveAllocation(
              &runtime, NV_SUBMIT_C56F_CLASS_ID, 1024u,
              true, true, true) == NV_CHANNEL_RUNTIME_OK,
          "allocation observation accepted");
    return runtime;
}

static NVAmpereChannelRuntime sealedRuntime()
{
    auto runtime = allocatedRuntime();
    check(nvAmpereChannelRuntimeObserveSubmissionEndpoint(
              &runtime, true, true) == NV_CHANNEL_RUNTIME_OK,
          "submission endpoint observation accepted");
    check(nvAmpereChannelRuntimeSeal(&runtime) == NV_CHANNEL_RUNTIME_OK,
          "runtime seals");
    return runtime;
}

static void testInitHasNoAuthority()
{
    std::puts("1. init has no observed facts or write authority");
    NVAmpereChannelRuntime runtime = {};
    check(nvAmpereChannelRuntimeInit(&runtime) == NV_CHANNEL_RUNTIME_OK,
          "init accepted");
    check(!runtime.allocationObserved &&
              !runtime.submissionEndpointObserved && !runtime.sealed,
          "init has no channel observations");
    check(!runtime.runtimeWriteAuthorized,
          "init cannot authorize hardware writes");
    check(!nvAmpereChannelRuntimeReady(runtime),
          "unobserved channel is not ready");
}

static void testAllocationRefusalsAreAtomic()
{
    std::puts("2. allocation observation is exact and atomic");
    NVAmpereChannelRuntime runtime = {};
    nvAmpereChannelRuntimeInit(&runtime);

    const auto initial = runtime;
    check(nvAmpereChannelRuntimeObserveAllocation(
              &runtime, 0u, 1024u, true, true, true) ==
              NV_CHANNEL_RUNTIME_ERR_ALLOCATION,
          "wrong channel class refused");
    check(sameRuntime(runtime, initial), "wrong-class refusal is atomic");

    check(nvAmpereChannelRuntimeObserveAllocation(
              &runtime, NV_SUBMIT_C56F_CLASS_ID, 1u,
              true, true, true) == NV_CHANNEL_RUNTIME_ERR_ALLOCATION,
          "ring without sentinel capacity refused");
    check(sameRuntime(runtime, initial), "small-ring refusal is atomic");

    check(nvAmpereChannelRuntimeObserveAllocation(
              &runtime, NV_SUBMIT_C56F_CLASS_ID, 1024u,
              true, false, true) == NV_CHANNEL_RUNTIME_ERR_ALLOCATION,
          "missing gpPut mapping refused");
    check(sameRuntime(runtime, initial), "mapping refusal is atomic");
}

static void testAllocationThenEndpointOrdering()
{
    std::puts("3. endpoint observation cannot precede allocation");
    NVAmpereChannelRuntime runtime = {};
    nvAmpereChannelRuntimeInit(&runtime);
    const auto initial = runtime;

    check(nvAmpereChannelRuntimeObserveSubmissionEndpoint(
              &runtime, true, true) == NV_CHANNEL_RUNTIME_ERR_STATE,
          "submission endpoint before allocation refused");
    check(sameRuntime(runtime, initial), "early endpoint refusal is atomic");

    runtime = allocatedRuntime();
    check(runtime.allocationObserved &&
              runtime.evidence.channelClassNum == NV_SUBMIT_C56F_CLASS_ID &&
              runtime.evidence.numGpFifoEntries == 1024u,
          "allocation facts are retained");
    check(!runtime.evidence.workSubmissionMapped &&
              !runtime.evidence.workSubmissionTokenReady,
          "allocation does not fabricate submission endpoint");

    const auto allocated = runtime;
    check(nvAmpereChannelRuntimeObserveAllocation(
              &runtime, NV_SUBMIT_C56F_CLASS_ID, 1024u,
              true, true, true) == NV_CHANNEL_RUNTIME_ERR_STATE,
          "allocation cannot be replayed");
    check(sameRuntime(runtime, allocated), "allocation replay is atomic");
}

static void testSubmissionObservationIsPresenceOnly()
{
    std::puts("4. submission observation carries presence only");
    auto runtime = allocatedRuntime();
    const auto before = runtime;

    check(nvAmpereChannelRuntimeObserveSubmissionEndpoint(
              &runtime, false, true) == NV_CHANNEL_RUNTIME_ERR_SUBMISSION,
          "missing submission mapping refused");
    check(sameRuntime(runtime, before), "mapping refusal is atomic");
    check(nvAmpereChannelRuntimeObserveSubmissionEndpoint(
              &runtime, true, false) == NV_CHANNEL_RUNTIME_ERR_SUBMISSION,
          "missing token readiness refused");
    check(sameRuntime(runtime, before), "token-readiness refusal is atomic");

    check(nvAmpereChannelRuntimeObserveSubmissionEndpoint(
              &runtime, true, true) == NV_CHANNEL_RUNTIME_OK,
          "presence observation accepted");
    check(runtime.submissionEndpointObserved &&
              runtime.evidence.workSubmissionMapped &&
              runtime.evidence.workSubmissionTokenReady,
          "only bounded endpoint readiness is retained");
    check(!runtime.runtimeWriteAuthorized,
          "endpoint observation grants no write authority");
}

static void testSealAndEvidenceBoundary()
{
    std::puts("5. only a sealed provider runtime exports evidence");
    NVAmpereChannelRuntime runtime = {};
    nvAmpereChannelRuntimeInit(&runtime);
    const auto before = runtime;

    check(nvAmpereChannelRuntimeSeal(&runtime) ==
              NV_CHANNEL_RUNTIME_ERR_READINESS,
          "empty runtime cannot seal");
    check(sameRuntime(runtime, before), "early seal refusal is atomic");

    runtime = allocatedRuntime();
    const auto allocated = runtime;
    check(nvAmpereChannelRuntimeSeal(&runtime) ==
              NV_CHANNEL_RUNTIME_ERR_READINESS,
          "allocation alone cannot seal");
    check(sameRuntime(runtime, allocated), "half-ready seal refusal is atomic");

    runtime = sealedRuntime();
    check(nvAmpereChannelRuntimeReady(runtime),
          "sealed runtime reports ready");
    check(runtime.readiness.allocationReady && runtime.readiness.ceBoundReady &&
              runtime.readiness.submitReady,
          "existing channel validator accepted sealed snapshot");

    NVAmpereChannelEvidence evidence = {};
    check(nvAmpereChannelRuntimeCopyEvidence(runtime, &evidence) ==
              NV_CHANNEL_RUNTIME_OK,
          "sealed runtime exports bounded evidence");
    check(evidence.channelClassNum == NV_SUBMIT_C56F_CLASS_ID &&
              evidence.numGpFifoEntries == 1024u &&
              evidence.gpGetMapped && evidence.gpPutMapped &&
              evidence.gpFifoMapped && evidence.workSubmissionMapped &&
              evidence.workSubmissionTokenReady,
          "export preserves validated presence facts");
}

static void testUnsealedExportIsAtomic()
{
    std::puts("6. unsealed runtime cannot export a channel snapshot");
    auto runtime = allocatedRuntime();
    NVAmpereChannelEvidence out = {};
    out.channelClassNum = 0xdeadbeefu;
    out.numGpFifoEntries = 77u;
    const auto before = out;

    check(nvAmpereChannelRuntimeCopyEvidence(runtime, &out) ==
              NV_CHANNEL_RUNTIME_ERR_NOT_SEALED,
          "unsealed evidence export refused");
    check(out.channelClassNum == before.channelClassNum &&
              out.numGpFifoEntries == before.numGpFifoEntries,
          "refused export leaves caller output unchanged");
}

static void testSealedStateCannotMutate()
{
    std::puts("7. sealed channel state is immutable");
    auto runtime = sealedRuntime();
    const auto before = runtime;

    check(nvAmpereChannelRuntimeObserveAllocation(
              &runtime, NV_SUBMIT_C56F_CLASS_ID, 1024u,
              true, true, true) == NV_CHANNEL_RUNTIME_ERR_STATE,
          "sealed allocation mutation refused");
    check(sameRuntime(runtime, before), "sealed allocation refusal is atomic");
    check(nvAmpereChannelRuntimeObserveSubmissionEndpoint(
              &runtime, true, true) == NV_CHANNEL_RUNTIME_ERR_STATE,
          "sealed endpoint mutation refused");
    check(sameRuntime(runtime, before), "sealed endpoint refusal is atomic");
    check(nvAmpereChannelRuntimeSeal(&runtime) == NV_CHANNEL_RUNTIME_ERR_STATE,
          "seal cannot be replayed");
    check(sameRuntime(runtime, before), "re-seal refusal is atomic");
}


static void testInitIsOneShot()
{
    std::puts("8. uninitialized access and reinitialization are refused atomically");
    NVAmpereChannelRuntime state = {};
    const auto empty = state;
    check(nvAmpereChannelRuntimeObserveAllocation(&state, NV_SUBMIT_C56F_CLASS_ID,
              1024u, true, true, true) == NV_CHANNEL_RUNTIME_ERR_UNINITIALIZED,
          "cannot observe allocation before init");
    check(nvAmpereChannelRuntimeObserveSubmissionEndpoint(&state, true, true) ==
              NV_CHANNEL_RUNTIME_ERR_UNINITIALIZED,
          "cannot observe submission before init");
    check(nvAmpereChannelRuntimeSeal(&state) == NV_CHANNEL_RUNTIME_ERR_UNINITIALIZED,
          "cannot seal before init");
    check(!nvAmpereChannelRuntimeReady(state) && sameRuntime(state, empty),
          "uninitialized state remains empty");

    check(nvAmpereChannelRuntimeInit(&state) == NV_CHANNEL_RUNTIME_OK,
          "initialization accepted once");
    const auto initialized = state;
    check(nvAmpereChannelRuntimeInit(&state) ==
              NV_CHANNEL_RUNTIME_ERR_REINITIALIZATION &&
              sameRuntime(state, initialized),
          "second init refused without changing state");

    check(nvAmpereChannelRuntimeObserveAllocation(&state, NV_SUBMIT_C56F_CLASS_ID,
              1024u, true, true, true) == NV_CHANNEL_RUNTIME_OK,
          "channel allocation observed");
    const auto allocated = state;
    check(nvAmpereChannelRuntimeInit(&state) ==
              NV_CHANNEL_RUNTIME_ERR_REINITIALIZATION &&
              sameRuntime(state, allocated),
          "cannot reset channel allocation ownership");

    check(nvAmpereChannelRuntimeObserveSubmissionEndpoint(&state, true, true) ==
              NV_CHANNEL_RUNTIME_OK &&
              nvAmpereChannelRuntimeSeal(&state) == NV_CHANNEL_RUNTIME_OK,
          "channel sealed");
    const auto sealed = state;
    check(nvAmpereChannelRuntimeInit(&state) ==
              NV_CHANNEL_RUNTIME_ERR_REINITIALIZATION &&
              sameRuntime(state, sealed) && nvAmpereChannelRuntimeReady(state),
          "cannot erase sealed ownership");
}


static void testOwnershipUncertaintyQuarantinesChannel()
{
    std::puts("9. channel loss is terminal without erasing ownership history");
    NVAmpereChannelRuntime runtime = {};
    const auto empty = runtime;
    check(nvAmpereChannelRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_CHANNEL_RUNTIME_ERR_UNINITIALIZED &&
              sameRuntime(runtime, empty), "uninitialized loss refuses atomically");
    check(nvAmpereChannelRuntimeInit(&runtime) == NV_CHANNEL_RUNTIME_OK,
          "init accepted");
    const auto clean = runtime;
    check(nvAmpereChannelRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_CHANNEL_RUNTIME_ERR_STATE &&
              sameRuntime(runtime, clean), "empty runtime cannot invent ownership");

    check(nvAmpereChannelRuntimeObserveAllocation(&runtime,
              NV_SUBMIT_C56F_CLASS_ID, 1024u, true, true, true) ==
              NV_CHANNEL_RUNTIME_OK, "allocation observed");
    check(nvAmpereChannelRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_CHANNEL_RUNTIME_OK, "partially observed ownership lost");
    const auto lost = runtime;
    check(runtime.allocationObserved && runtime.ownershipUncertain &&
              !nvAmpereChannelRuntimeReady(runtime), "retained but not ready");
    check(nvAmpereChannelRuntimeObserveSubmissionEndpoint(&runtime, true, true) ==
              NV_CHANNEL_RUNTIME_ERR_OWNERSHIP_UNCERTAIN,
          "lost owner cannot acquire submission readiness");
    check(nvAmpereChannelRuntimeSeal(&runtime) ==
              NV_CHANNEL_RUNTIME_ERR_OWNERSHIP_UNCERTAIN,
          "lost owner cannot seal");
    check(nvAmpereChannelRuntimeObserveAllocation(&runtime,
              NV_SUBMIT_C56F_CLASS_ID, 1024u, true, true, true) ==
              NV_CHANNEL_RUNTIME_ERR_OWNERSHIP_UNCERTAIN,
          "cannot reobserve lost allocation");
    check(nvAmpereChannelRuntimeInit(&runtime) ==
              NV_CHANNEL_RUNTIME_ERR_REINITIALIZATION,
          "cannot erase loss via init");
    check(nvAmpereChannelRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_CHANNEL_RUNTIME_OK && sameRuntime(runtime, lost),
          "loss observation is idempotent and atomic");

    runtime = sealedRuntime();
    NVAmpereChannelEvidence out = {};
    out.channelClassNum = 0xdeadbeefu;
    const auto sealed = runtime;
    check(nvAmpereChannelRuntimeMarkOwnershipUncertain(&runtime) ==
              NV_CHANNEL_RUNTIME_OK, "sealed owner can become uncertain");
    check(runtime.sealed && runtime.allocationObserved &&
              runtime.submissionEndpointObserved &&
              runtime.evidence.channelClassNum == sealed.evidence.channelClassNum &&
              !nvAmpereChannelRuntimeReady(runtime),
          "sealed history retained, readiness revoked");
    check(nvAmpereChannelRuntimeCopyEvidence(runtime, &out) ==
              NV_CHANNEL_RUNTIME_ERR_OWNERSHIP_UNCERTAIN &&
              out.channelClassNum == 0xdeadbeefu,
          "copy refuses without clobbering caller output");
}


static void checkInvalidSealedSnapshot(const NVAmpereChannelRuntime &runtime,
                                       const char *label)
{
    NVAmpereChannelEvidence out = {};
    out.channelClassNum = 0xdeadbeefu;
    out.numGpFifoEntries = 77u;
    const auto before = runtime;
    check(!nvAmpereChannelRuntimeReady(runtime), label);
    check(nvAmpereChannelRuntimeCopyEvidence(runtime, &out) ==
              NV_CHANNEL_RUNTIME_ERR_INCONSISTENT_SNAPSHOT,
          "inconsistent sealed evidence cannot be exported");
    check(out.channelClassNum == 0xdeadbeefu &&
              out.numGpFifoEntries == 77u,
          "rejected export leaves caller output unchanged");
    check(sameRuntime(runtime, before), "readiness check never mutates runtime");
}

static void testSealedEvidenceRevalidatedAtExport()
{
    std::puts("10. seal-time readiness must be revalidated before export");
    auto runtime = sealedRuntime();
    check(nvAmpereChannelRuntimeReady(runtime),
          "untouched sealed channel remains ready");
    runtime.evidence.channelClassNum = 0u;
    checkInvalidSealedSnapshot(runtime, "changed channel class rejected");

    runtime = sealedRuntime();
    runtime.evidence.numGpFifoEntries = 1u;
    checkInvalidSealedSnapshot(runtime, "sentinel-capacity loss rejected");

    runtime = sealedRuntime();
    runtime.evidence.gpGetMapped = false;
    checkInvalidSealedSnapshot(runtime, "gpGet mapping loss rejected");

    runtime = sealedRuntime();
    runtime.evidence.gpPutMapped = false;
    checkInvalidSealedSnapshot(runtime, "gpPut mapping loss rejected");

    runtime = sealedRuntime();
    runtime.evidence.gpFifoMapped = false;
    checkInvalidSealedSnapshot(runtime, "GPFIFO mapping loss rejected");

    runtime = sealedRuntime();
    runtime.evidence.workSubmissionMapped = false;
    checkInvalidSealedSnapshot(runtime, "submission mapping loss rejected");

    runtime = sealedRuntime();
    runtime.evidence.workSubmissionTokenReady = false;
    checkInvalidSealedSnapshot(runtime, "submission token loss rejected");

    runtime = sealedRuntime();
    runtime.readiness.ceBoundReady = false;
    checkInvalidSealedSnapshot(runtime, "cached readiness mismatch rejected");

    runtime = sealedRuntime();
    runtime.runtimeWriteAuthorized = true;
    checkInvalidSealedSnapshot(runtime, "write-authorization mutation rejected");

    runtime = sealedRuntime();
    runtime.submissionEndpointObserved = false;
    checkInvalidSealedSnapshot(runtime, "missing observation ownership rejected");
}

int main()
{
    testInitHasNoAuthority();
    testAllocationRefusalsAreAtomic();
    testAllocationThenEndpointOrdering();
    testSubmissionObservationIsPresenceOnly();
    testSealAndEvidenceBoundary();
    testUnsealedExportIsAtomic();
    testSealedStateCannotMutate();
    testInitIsOneShot();
    testOwnershipUncertaintyQuarantinesChannel();
    testSealedEvidenceRevalidatedAtExport();

    std::printf("%u checks, %u failures\n", gChecks, gFailures);
    return gFailures == 0u ? 0 : 1;
}
