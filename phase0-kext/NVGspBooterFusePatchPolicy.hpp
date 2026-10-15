// SPDX-License-Identifier: MIT
// Pure bridge between the read-only GA107 SEC2 Booter fuse observer and the
// already-source-backed production-signature patch planner.
//
// The purpose of this layer is deliberately narrow: a runtime caller may not
// turn a naked integer called "fuseVersion" into a signature choice. It must
// carry the complete observation produced by NVGspBooterFuse.hpp, and this
// helper re-validates that observation before the immutable firmware container
// is parsed and a private-copy patch plan is accepted.
#ifndef NV_GSP_BOOTER_FUSE_PATCH_POLICY_HPP
#define NV_GSP_BOOTER_FUSE_PATCH_POLICY_HPP

#include "NVGspBooterFuse.hpp"
#include "NVGspBooterPatch.hpp"

#define NV_GSP_BOOTER_FUSE_PATCH_OK                         0u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_OUTPUT            1u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_OBSERVATION       2u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_OBSERVATION            3u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_UCODE_MISMATCH         4u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_CONTAINER              5u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_FUSE_RANGE             6u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_SIGNATURE_RANGE        7u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_DEST_RANGE             8u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_DEST              9u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_DEST_TOO_SMALL        10u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_SOURCE_OVERLAP        11u
#define NV_GSP_BOOTER_FUSE_PATCH_ERR_PATCH_INTERNAL        12u

struct NVGspBooterFusePatchPlan {
    NVGspBooterFuseObservation observation;
    NVGspBooterPatchPlan patch;
    bool valid;
};

static inline bool nvGspBooterFusePatchSucceeded(NVGspBooterHsU32 verdict)
{
    return verdict == NV_GSP_BOOTER_FUSE_PATCH_OK;
}

static inline const char *nvGspBooterFusePatchVerdictName(NVGspBooterHsU32 verdict)
{
    switch (verdict) {
        case NV_GSP_BOOTER_FUSE_PATCH_OK:                  return "ok";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_OUTPUT:     return "null-output";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_OBSERVATION:return "null-observation";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_OBSERVATION:     return "observation";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_UCODE_MISMATCH:  return "ucode-mismatch";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_CONTAINER:       return "container";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_FUSE_RANGE:      return "fuse-range";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_SIGNATURE_RANGE: return "signature-range";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_DEST_RANGE:      return "destination-range";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_DEST:       return "null-destination";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_DEST_TOO_SMALL:  return "destination-too-small";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_SOURCE_OVERLAP:  return "source-overlap";
        case NV_GSP_BOOTER_FUSE_PATCH_ERR_PATCH_INTERNAL:  return "patch-internal";
        default:                                           return "unknown";
    }
}

static inline NVGspBooterHsU32 nvGspBooterFusePatchMapPlannerVerdict(
    NVGspBooterHsU32 verdict)
{
    switch (verdict) {
        case NV_GSP_BOOTER_PATCH_ERR_CONTAINER:
            return NV_GSP_BOOTER_FUSE_PATCH_ERR_CONTAINER;
        case NV_GSP_BOOTER_PATCH_ERR_FUSE_RANGE:
            return NV_GSP_BOOTER_FUSE_PATCH_ERR_FUSE_RANGE;
        case NV_GSP_BOOTER_PATCH_ERR_SIGNATURE_RANGE:
            return NV_GSP_BOOTER_FUSE_PATCH_ERR_SIGNATURE_RANGE;
        case NV_GSP_BOOTER_PATCH_ERR_DEST_RANGE:
            return NV_GSP_BOOTER_FUSE_PATCH_ERR_DEST_RANGE;
        case NV_GSP_BOOTER_PATCH_ERR_NULL_DEST:
            return NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_DEST;
        case NV_GSP_BOOTER_PATCH_ERR_DEST_TOO_SMALL:
            return NV_GSP_BOOTER_FUSE_PATCH_ERR_DEST_TOO_SMALL;
        case NV_GSP_BOOTER_PATCH_ERR_SOURCE_OVERLAP:
            return NV_GSP_BOOTER_FUSE_PATCH_ERR_SOURCE_OVERLAP;
        default:
            return NV_GSP_BOOTER_FUSE_PATCH_ERR_PATCH_INTERNAL;
    }
}

// Re-derive every observation field that influences signature selection. This
// deliberately does not trust `valid`, `registerOffset`, or `fuseVersion` in
// isolation. A dead BAR sentinel can therefore never be smuggled through as
// version 32, and an observation for another SEC2 ucode cannot select a Booter
// signature merely because its numeric version happens to fit.
static inline bool nvGspBooterFuseObservationIsCanonical(
    const NVGspBooterFuseObservation *observation)
{
    if (!observation || !observation->valid ||
        observation->ucodeId < NV_GSP_GA107_SEC2_UCODE_FIRST_ID ||
        observation->ucodeId > NV_GSP_GA107_SEC2_UCODE_LAST_ID ||
        observation->rawFuse == 0xffffffffu)
        return false;

    const NVGspBooterHsU32 expectedRegister =
        NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG +
        (observation->ucodeId - NV_GSP_GA107_SEC2_UCODE_FIRST_ID) *
        NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE;
    if (observation->registerOffset != expectedRegister)
        return false;

    return observation->fuseVersion ==
        nvGspBooterFuseVersionFromMask(observation->rawFuse);
}

// Atomic policy planner. `*out` is untouched unless the complete observed fact
// is canonical, the Booter container itself parses, NVIDIA's production
// signature selection accepts the observed version, and the parsed HS ucode ID
// is exactly the ID whose fuse register was observed.
static inline NVGspBooterHsU32 nvGspPlanObservedBooterPatch(
    const unsigned char *container,
    NVGspBooterHsU64 containerBytes,
    const NVGspBooterFuseObservation *observation,
    NVGspBooterFusePatchPlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_OUTPUT;
    if (!observation)
        return NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_OBSERVATION;
    if (!nvGspBooterFuseObservationIsCanonical(observation))
        return NV_GSP_BOOTER_FUSE_PATCH_ERR_OBSERVATION;

    NVGspBooterPatchPlan patch = {};
    const NVGspBooterHsU32 patchVerdict = nvGspPlanBooterPatch(
        container,containerBytes,observation->fuseVersion,&patch);
    if (!nvGspBooterPatchSucceeded(patchVerdict))
        return nvGspBooterFusePatchMapPlannerVerdict(patchVerdict);
    if (!patch.valid || patch.ucodeId != observation->ucodeId)
        return NV_GSP_BOOTER_FUSE_PATCH_ERR_UCODE_MISMATCH;

    NVGspBooterFusePatchPlan staged = {};
    staged.observation = *observation;
    staged.patch = patch;
    staged.valid = true;
    *out = staged;
    return NV_GSP_BOOTER_FUSE_PATCH_OK;
}

// Build only into caller-owned writable storage. The source container remains
// immutable. Policy validation happens before the existing patch helper gets a
// destination pointer, so a wrong/stale/non-Booter fuse observation cannot
// cause even a private-memory mutation.
static inline NVGspBooterHsU32 nvGspBuildObservedBooterPrivateImage(
    const unsigned char *container,
    NVGspBooterHsU64 containerBytes,
    const NVGspBooterFuseObservation *observation,
    unsigned char *privateImage,
    NVGspBooterHsU64 privateCapacity,
    NVGspBooterFusePatchPlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_FUSE_PATCH_ERR_NULL_OUTPUT;

    NVGspBooterFusePatchPlan policy = {};
    const NVGspBooterHsU32 policyVerdict = nvGspPlanObservedBooterPatch(
        container,containerBytes,observation,&policy);
    if (!nvGspBooterFusePatchSucceeded(policyVerdict))
        return policyVerdict;

    NVGspBooterPatchPlan built = {};
    const NVGspBooterHsU32 patchVerdict = nvGspBuildBooterPrivateImage(
        container,containerBytes,policy.observation.fuseVersion,
        privateImage,privateCapacity,&built);
    if (!nvGspBooterPatchSucceeded(patchVerdict))
        return nvGspBooterFusePatchMapPlannerVerdict(patchVerdict);

    // Source is immutable for runtime callers; this is a defensive consistency
    // check against accidental helper drift rather than a post-write recovery
    // mechanism. There is no device-visible state in this function.
    if (!built.valid || built.ucodeId != policy.patch.ucodeId ||
        built.signatureIndex != policy.patch.signatureIndex ||
        built.signatureSourceOffset != policy.patch.signatureSourceOffset ||
        built.signatureBytes != policy.patch.signatureBytes ||
        built.firmwareSourceOffset != policy.patch.firmwareSourceOffset ||
        built.firmwareBytes != policy.patch.firmwareBytes ||
        built.patchDestinationOffset != policy.patch.patchDestinationOffset)
        return NV_GSP_BOOTER_FUSE_PATCH_ERR_PATCH_INTERNAL;

    *out = policy;
    return NV_GSP_BOOTER_FUSE_PATCH_OK;
}

#endif // NV_GSP_BOOTER_FUSE_PATCH_POLICY_HPP
