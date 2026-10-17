// SPDX-License-Identifier: MIT
// Provider-owned monotonic GA107 channel readiness observation.
//
// This state machine turns staged, provider-visible channel facts into the
// bounded NVAmpereChannelEvidence consumed by copy publication. It carries no
// raw mapping pointer, GPU address, IOVA, GPFIFO entry, pushbuffer, register
// write, work-submission token value, or provider pointer. It authorizes no
// hardware writes and deliberately has no reset/reuse path in this phase.
#ifndef NV_AMPERE_CHANNEL_RUNTIME_HPP
#define NV_AMPERE_CHANNEL_RUNTIME_HPP

#include "NVAmpereChannel.hpp"

using NVChannelRuntimeU32 = unsigned int;

#define NV_CHANNEL_RUNTIME_OK                    0u
#define NV_CHANNEL_RUNTIME_ERR_NULL              1u
#define NV_CHANNEL_RUNTIME_ERR_STATE             2u
#define NV_CHANNEL_RUNTIME_ERR_ALLOCATION        3u
#define NV_CHANNEL_RUNTIME_ERR_SUBMISSION        4u
#define NV_CHANNEL_RUNTIME_ERR_READINESS         5u
#define NV_CHANNEL_RUNTIME_ERR_NOT_SEALED        6u
#define NV_CHANNEL_RUNTIME_VERDICT_LIMIT         7u

struct NVAmpereChannelRuntime {
    NVAmpereChannelEvidence evidence;
    NVAmpereChannelReadiness readiness;
    bool allocationObserved;
    bool submissionEndpointObserved;
    bool sealed;
    bool runtimeWriteAuthorized;
};

static inline bool nvAmpereChannelRuntimeSucceeded(NVChannelRuntimeU32 verdict)
{
    return verdict == NV_CHANNEL_RUNTIME_OK;
}

static inline const char *nvAmpereChannelRuntimeVerdictName(
    NVChannelRuntimeU32 verdict)
{
    switch (verdict) {
    case NV_CHANNEL_RUNTIME_OK:             return "ok";
    case NV_CHANNEL_RUNTIME_ERR_NULL:       return "null";
    case NV_CHANNEL_RUNTIME_ERR_STATE:      return "state";
    case NV_CHANNEL_RUNTIME_ERR_ALLOCATION: return "allocation";
    case NV_CHANNEL_RUNTIME_ERR_SUBMISSION: return "submission";
    case NV_CHANNEL_RUNTIME_ERR_READINESS:  return "readiness";
    case NV_CHANNEL_RUNTIME_ERR_NOT_SEALED: return "not-sealed";
    default:                                return "unknown";
    }
}

static inline NVChannelRuntimeU32 nvAmpereChannelRuntimeInit(
    NVAmpereChannelRuntime *runtime)
{
    if (!runtime)
        return NV_CHANNEL_RUNTIME_ERR_NULL;

    NVAmpereChannelRuntime staged = {};
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_CHANNEL_RUNTIME_OK;
}

// Record the bounded host mappings returned by a real channel allocation.
// This is intentionally only the allocation half of readiness. Work submission
// presence is observed separately after the CE has actually been bound.
static inline NVChannelRuntimeU32 nvAmpereChannelRuntimeObserveAllocation(
    NVAmpereChannelRuntime *runtime,
    NVChannelRuntimeU32 channelClassNum,
    NVChannelRuntimeU32 numGpFifoEntries,
    bool gpGetMapped,
    bool gpPutMapped,
    bool gpFifoMapped)
{
    if (!runtime)
        return NV_CHANNEL_RUNTIME_ERR_NULL;
    if (runtime->allocationObserved || runtime->sealed)
        return NV_CHANNEL_RUNTIME_ERR_STATE;
    if (channelClassNum != NV_SUBMIT_C56F_CLASS_ID ||
        numGpFifoEntries < 2u ||
        !gpGetMapped || !gpPutMapped || !gpFifoMapped)
        return NV_CHANNEL_RUNTIME_ERR_ALLOCATION;

    NVAmpereChannelRuntime staged = *runtime;
    staged.evidence.channelClassNum = channelClassNum;
    staged.evidence.numGpFifoEntries = numGpFifoEntries;
    staged.evidence.gpGetMapped = gpGetMapped;
    staged.evidence.gpPutMapped = gpPutMapped;
    staged.evidence.gpFifoMapped = gpFifoMapped;
    staged.allocationObserved = true;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_CHANNEL_RUNTIME_OK;
}

// Record only the presence of the provider work-submission endpoint and token.
// No pointer or token value crosses this boundary. A negative/incomplete
// observation is a refusal and leaves the provider-owned state unchanged.
static inline NVChannelRuntimeU32
nvAmpereChannelRuntimeObserveSubmissionEndpoint(
    NVAmpereChannelRuntime *runtime,
    bool workSubmissionMapped,
    bool workSubmissionTokenReady)
{
    if (!runtime)
        return NV_CHANNEL_RUNTIME_ERR_NULL;
    if (!runtime->allocationObserved || runtime->submissionEndpointObserved ||
        runtime->sealed)
        return NV_CHANNEL_RUNTIME_ERR_STATE;
    if (!workSubmissionMapped || !workSubmissionTokenReady)
        return NV_CHANNEL_RUNTIME_ERR_SUBMISSION;

    NVAmpereChannelRuntime staged = *runtime;
    staged.evidence.workSubmissionMapped = workSubmissionMapped;
    staged.evidence.workSubmissionTokenReady = workSubmissionTokenReady;
    staged.submissionEndpointObserved = true;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_CHANNEL_RUNTIME_OK;
}

// Seal the channel snapshot exactly once. The existing source-backed channel
// validator remains authoritative; the runtime merely controls when a complete
// snapshot may cross into the copy-publication state machine.
static inline NVChannelRuntimeU32 nvAmpereChannelRuntimeSeal(
    NVAmpereChannelRuntime *runtime)
{
    if (!runtime)
        return NV_CHANNEL_RUNTIME_ERR_NULL;
    if (runtime->sealed)
        return NV_CHANNEL_RUNTIME_ERR_STATE;
    if (!runtime->allocationObserved || !runtime->submissionEndpointObserved)
        return NV_CHANNEL_RUNTIME_ERR_READINESS;

    NVAmpereChannelReadiness readiness = {};
    if (nvAmpereEvaluateChannel(runtime->evidence, &readiness) !=
            NV_CHANNEL_READY_OK ||
        !readiness.allocationReady || !readiness.ceBoundReady ||
        !readiness.submitReady)
        return NV_CHANNEL_RUNTIME_ERR_READINESS;

    NVAmpereChannelRuntime staged = *runtime;
    staged.readiness = readiness;
    staged.sealed = true;
    staged.runtimeWriteAuthorized = false;
    *runtime = staged;
    return NV_CHANNEL_RUNTIME_OK;
}

static inline bool nvAmpereChannelRuntimeReady(
    const NVAmpereChannelRuntime &runtime)
{
    return runtime.sealed && runtime.allocationObserved &&
           runtime.submissionEndpointObserved &&
           runtime.readiness.allocationReady &&
           runtime.readiness.ceBoundReady && runtime.readiness.submitReady &&
           !runtime.runtimeWriteAuthorized;
}

// Export only the bounded presence evidence after the provider-owned runtime is
// sealed. Refusal is atomic with respect to the caller's output object.
static inline NVChannelRuntimeU32 nvAmpereChannelRuntimeCopyEvidence(
    const NVAmpereChannelRuntime &runtime,
    NVAmpereChannelEvidence *out)
{
    if (!out)
        return NV_CHANNEL_RUNTIME_ERR_NULL;
    if (!nvAmpereChannelRuntimeReady(runtime))
        return NV_CHANNEL_RUNTIME_ERR_NOT_SEALED;

    *out = runtime.evidence;
    return NV_CHANNEL_RUNTIME_OK;
}

#endif // NV_AMPERE_CHANNEL_RUNTIME_HPP
