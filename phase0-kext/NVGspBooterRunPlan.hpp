// SPDX-License-Identifier: MIT
// Pure GA107 SEC2 Booter future-execution planner.
//
// Sources of truth:
//   - Linux Nova drivers/gpu/nova-core/firmware/booter.rs
//   - NVIDIA/open-gpu-kernel-modules Booter container extraction/patch policy
//   - the checked-in GA107 570.144 Booter load/unload fixtures
//
// This helper performs no GPU I/O. It does not reset SEC2, load IMEM/DMEM,
// write mailboxes/WPR, start Booter, poll completion, or notify GSP. It only
// proves that already-validated immutable/private inputs compose into the
// minimal semantic contract a later tightly-scoped executor may consume.
#ifndef NV_GSP_BOOTER_RUN_PLAN_HPP
#define NV_GSP_BOOTER_RUN_PLAN_HPP

#include "NVGspBooterFusePatchPolicy.hpp"

#define NV_GSP_BOOTER_RUN_OK                         0u
#define NV_GSP_BOOTER_RUN_ERR_NULL_OUTPUT            1u
#define NV_GSP_BOOTER_RUN_ERR_NULL_SOURCE            2u
#define NV_GSP_BOOTER_RUN_ERR_NULL_OBSERVATION       3u
#define NV_GSP_BOOTER_RUN_ERR_NULL_PRIVATE_IMAGE     4u
#define NV_GSP_BOOTER_RUN_ERR_CONTAINER              5u
#define NV_GSP_BOOTER_RUN_ERR_TARGET_ID              6u
#define NV_GSP_BOOTER_RUN_ERR_PATCH_POLICY           7u
#define NV_GSP_BOOTER_RUN_ERR_PRIVATE_SIZE           8u
#define NV_GSP_BOOTER_RUN_ERR_PRIVATE_OVERLAP        9u
#define NV_GSP_BOOTER_RUN_ERR_PRIVATE_IMAGE         10u
#define NV_GSP_BOOTER_RUN_ERR_LOAD_SEMANTICS        11u
#define NV_GSP_BOOTER_RUN_ERR_LOAD_RANGE            12u
#define NV_GSP_BOOTER_RUN_ERR_WPR_META_DMA          13u
#define NV_GSP_BOOTER_RUN_ERR_INTERNAL              14u

// Both checked-in GA107 570.144 Booter containers carry engine_id == 1.
// Keep this gate local to the exact GA107 planner rather than teaching the
// generic container parser that all future Booter images must use this value.
#define NV_GSP_GA107_BOOTER_HS_ENGINE_ID             1u

struct NVGspBooterRunPlan {
    NVGspBooterHsU32 engineId;
    NVGspBooterHsU32 ucodeId;
    NVGspBooterHsU32 hardwareFuseVersion;
    NVGspBooterHsU32 signatureIndex;
    NVGspBooterHsU32 signatureBytes;
    NVGspBooterHsU32 signaturePatchOffset;

    NVGspBooterHsU32 secureImemSourceOffset;
    NVGspBooterHsU32 secureImemBytes;
    NVGspBooterHsU32 secureImemDestinationOffset;
    NVGspBooterHsU32 dmemSourceOffset;
    NVGspBooterHsU32 dmemBytes;
    NVGspBooterHsU32 dmemDestinationOffset;

    NVGspBooterHsU64 wprMetaDmaAddress;
    NVGspBooterHsU32 mailbox0Input;
    NVGspBooterHsU32 mailbox1Input;
    NVGspBooterHsU32 mailbox0SuccessValue;

    bool resetRequired;
    bool secureImemLoadRequired;
    bool nonSecureImemLoadRequired;
    bool dmemLoadRequired;
    bool mailboxPairRequired;
    bool boundedCompletionPollRequired;
    bool nonzeroMailbox0IsFailure;
    bool valid;
};

static inline bool nvGspBooterRunPlanSucceeded(NVGspBooterHsU32 verdict)
{
    return verdict == NV_GSP_BOOTER_RUN_OK;
}

static inline const char *nvGspBooterRunPlanVerdictName(NVGspBooterHsU32 verdict)
{
    switch (verdict) {
        case NV_GSP_BOOTER_RUN_OK:                     return "ok";
        case NV_GSP_BOOTER_RUN_ERR_NULL_OUTPUT:        return "null-output";
        case NV_GSP_BOOTER_RUN_ERR_NULL_SOURCE:        return "null-source";
        case NV_GSP_BOOTER_RUN_ERR_NULL_OBSERVATION:   return "null-observation";
        case NV_GSP_BOOTER_RUN_ERR_NULL_PRIVATE_IMAGE: return "null-private-image";
        case NV_GSP_BOOTER_RUN_ERR_CONTAINER:          return "container";
        case NV_GSP_BOOTER_RUN_ERR_TARGET_ID:          return "target-id";
        case NV_GSP_BOOTER_RUN_ERR_PATCH_POLICY:       return "patch-policy";
        case NV_GSP_BOOTER_RUN_ERR_PRIVATE_SIZE:       return "private-size";
        case NV_GSP_BOOTER_RUN_ERR_PRIVATE_OVERLAP:    return "private-overlap";
        case NV_GSP_BOOTER_RUN_ERR_PRIVATE_IMAGE:      return "private-image";
        case NV_GSP_BOOTER_RUN_ERR_LOAD_SEMANTICS:     return "load-semantics";
        case NV_GSP_BOOTER_RUN_ERR_LOAD_RANGE:         return "load-range";
        case NV_GSP_BOOTER_RUN_ERR_WPR_META_DMA:       return "wpr-meta-dma";
        case NV_GSP_BOOTER_RUN_ERR_INTERNAL:           return "internal";
        default:                                       return "unknown";
    }
}

static inline bool nvGspBooterRunPrivateImageMatches(
    const unsigned char *container,
    NVGspBooterHsU64 containerBytes,
    const unsigned char *privateImage,
    NVGspBooterHsU64 privateImageBytes,
    const NVGspBooterPatchPlan *patch)
{
    if (!container || !privateImage || !patch || !patch->valid ||
        privateImageBytes != patch->firmwareBytes ||
        !nvGspBooterHsFits(containerBytes,
                           patch->firmwareSourceOffset,
                           patch->firmwareBytes) ||
        !nvGspBooterHsFits(containerBytes,
                           patch->signatureSourceOffset,
                           patch->signatureBytes) ||
        !nvGspBooterHsRegionFits(patch->firmwareBytes,
                                 patch->patchDestinationOffset,
                                 patch->signatureBytes))
        return false;

    const NVGspBooterHsU64 patchStart = patch->patchDestinationOffset;
    const NVGspBooterHsU64 patchEnd = patchStart + patch->signatureBytes;
    for (NVGspBooterHsU64 i = 0u; i < patch->firmwareBytes; ++i) {
        unsigned char expected =
            container[(NVGspBooterHsU64)patch->firmwareSourceOffset + i];
        if (i >= patchStart && i < patchEnd) {
            expected = container[
                (NVGspBooterHsU64)patch->signatureSourceOffset +
                (i - patchStart)];
        }
        if (privateImage[i] != expected)
            return false;
    }
    return true;
}

// Compose only the source-backed semantic execution contract for GA107 Booter.
// Atomic refusal: *out is assigned only after every immutable-source,
// observed-fuse, private-image, load-region and WPR-address check succeeds.
static inline NVGspBooterHsU32 nvGspPlanGa107BooterRun(
    const unsigned char *container,
    NVGspBooterHsU64 containerBytes,
    const NVGspBooterFuseObservation *observation,
    const unsigned char *privateImage,
    NVGspBooterHsU64 privateImageBytes,
    NVGspBooterHsU64 wprMetaDmaAddress,
    NVGspBooterRunPlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_RUN_ERR_NULL_OUTPUT;
    if (!container || !containerBytes)
        return NV_GSP_BOOTER_RUN_ERR_NULL_SOURCE;
    if (!observation)
        return NV_GSP_BOOTER_RUN_ERR_NULL_OBSERVATION;
    if (!privateImage)
        return NV_GSP_BOOTER_RUN_ERR_NULL_PRIVATE_IMAGE;

    // Reparse the immutable source rather than accepting caller-supplied
    // offsets/lengths. The generic parser already proves every declared region
    // fits the immutable firmware payload.
    NVGspBooterHsPlan hs = {};
    if (!nvGspBooterHsSucceeded(
            nvGspPlanBooterHs(container,containerBytes,&hs)) || !hs.valid)
        return NV_GSP_BOOTER_RUN_ERR_CONTAINER;

    // This planner is intentionally exact-artifact/target scoped. Future
    // firmware carrying different IDs must earn a new source-backed policy.
    if (hs.engineId != NV_GSP_GA107_BOOTER_HS_ENGINE_ID ||
        hs.ucodeId != NV_GSP_GA107_BOOTER_HS_UCODE_ID)
        return NV_GSP_BOOTER_RUN_ERR_TARGET_ID;

    // Re-derive NVIDIA's production-signature selection from the canonical
    // hardware observation. Never trust the private image's contents as proof
    // of which signature should have been selected.
    NVGspBooterFusePatchPlan policy = {};
    const NVGspBooterHsU32 policyVerdict = nvGspPlanObservedBooterPatch(
        container,containerBytes,observation,&policy);
    if (!nvGspBooterFusePatchSucceeded(policyVerdict) || !policy.valid ||
        !policy.patch.valid)
        return NV_GSP_BOOTER_RUN_ERR_PATCH_POLICY;

    // Defensive consistency check against accidental drift between the generic
    // parser and the observed-fuse patch policy.
    if (policy.patch.ucodeId != hs.ucodeId ||
        policy.patch.firmwareSourceOffset != hs.firmwareOffset ||
        policy.patch.firmwareBytes != hs.firmwareBytes ||
        policy.patch.patchDestinationOffset != hs.patchLocation ||
        policy.patch.signatureBytes != hs.signatureStride ||
        policy.observation.ucodeId != hs.ucodeId)
        return NV_GSP_BOOTER_RUN_ERR_INTERNAL;

    if (privateImageBytes != hs.firmwareBytes)
        return NV_GSP_BOOTER_RUN_ERR_PRIVATE_SIZE;
    if (nvGspBooterPatchRangesOverlap(
            container,containerBytes,privateImage,privateImageBytes))
        return NV_GSP_BOOTER_RUN_ERR_PRIVATE_OVERLAP;

    // GA102+ Nova semantics: app0_code is the only IMEM load and is secure,
    // with destination 0. There is no separate non-secure IMEM load. DMEM is
    // sourced from os_data and also targets destination 0. The checked-in
    // source-derived GA107 images carry an ignored 0..app0_code prefix and no
    // app-data payload; refuse shapes that would require inventing semantics.
    if (hs.osCodeOffset != 0u ||
        hs.osCodeBytes != hs.appCodeOffset ||
        hs.appDataOffset != hs.appCodeOffset ||
        hs.appDataBytes != 0u ||
        hs.appCodeBytes == 0u ||
        hs.osDataBytes == 0u)
        return NV_GSP_BOOTER_RUN_ERR_LOAD_SEMANTICS;

    // privateImageBytes == hs.firmwareBytes is proven above, so pass the
    // 32-bit field. The helper takes NVGspBooterHsU32, and narrowing the
    // 64-bit parameter raised MSVC C4244 on all three calls. Same verdict.
    if (!nvGspBooterHsRegionFits(hs.firmwareBytes,
                                 hs.appCodeOffset,hs.appCodeBytes) ||
        !nvGspBooterHsRegionFits(hs.firmwareBytes,
                                 hs.osDataOffset,hs.osDataBytes) ||
        !nvGspBooterHsRegionFits(hs.firmwareBytes,
                                 hs.patchLocation,hs.signatureStride))
        return NV_GSP_BOOTER_RUN_ERR_LOAD_RANGE;

    // Prove that the caller-owned image is exactly the immutable firmware
    // payload with only the one selected production signature substituted.
    if (!nvGspBooterRunPrivateImageMatches(
            container,containerBytes,privateImage,privateImageBytes,
            &policy.patch))
        return NV_GSP_BOOTER_RUN_ERR_PRIVATE_IMAGE;

    // The future executor will write these two semantic words to the sourced
    // SEC2 mailbox pair. A u64 input makes the split exact and avoids any
    // implicit narrower address-domain assumption.
    if (wprMetaDmaAddress == 0u)
        return NV_GSP_BOOTER_RUN_ERR_WPR_META_DMA;

    NVGspBooterRunPlan staged = {};
    staged.engineId = hs.engineId;
    staged.ucodeId = hs.ucodeId;
    staged.hardwareFuseVersion = policy.observation.fuseVersion;
    staged.signatureIndex = policy.patch.signatureIndex;
    staged.signatureBytes = policy.patch.signatureBytes;
    staged.signaturePatchOffset = policy.patch.patchDestinationOffset;
    staged.secureImemSourceOffset = hs.appCodeOffset;
    staged.secureImemBytes = hs.appCodeBytes;
    staged.secureImemDestinationOffset = 0u;
    staged.dmemSourceOffset = hs.osDataOffset;
    staged.dmemBytes = hs.osDataBytes;
    staged.dmemDestinationOffset = 0u;
    staged.wprMetaDmaAddress = wprMetaDmaAddress;
    staged.mailbox0Input = (NVGspBooterHsU32)(wprMetaDmaAddress & 0xffffffffull);
    staged.mailbox1Input = (NVGspBooterHsU32)(wprMetaDmaAddress >> 32);
    staged.mailbox0SuccessValue = 0u;
    staged.resetRequired = true;
    staged.secureImemLoadRequired = true;
    staged.nonSecureImemLoadRequired = false;
    staged.dmemLoadRequired = true;
    staged.mailboxPairRequired = true;
    staged.boundedCompletionPollRequired = true;
    staged.nonzeroMailbox0IsFailure = true;
    staged.valid = true;
    *out = staged;
    return NV_GSP_BOOTER_RUN_OK;
}

#endif // NV_GSP_BOOTER_RUN_PLAN_HPP
