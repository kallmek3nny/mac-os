// SPDX-License-Identifier: MIT
// Pure NVIDIA SEC2 Booter production-signature selection and private-copy patching.
//
// Source of truth:
//   NVIDIA/open-gpu-kernel-modules
//   src/nvidia/src/kernel/gpu/gsp/kernel_gsp_booter.c
//   s_patchBooterUcodeSignature()
//
// NVIDIA selects signature 0 for a one-signature image. For a multi-signature
// image it reads the hardware SEC2 ucode fuse version, rejects values larger
// than numSigs-1, then selects:
//
//     sigIndex = numSigs - 1 - fuseVer
//
// The production driver copies the firmware image into writable private memory
// before patching that selected signature at patchLoc. This helper mirrors that
// policy without reading a fuse register or touching SEC2, BAR0, WPR, mailboxes
// or any GPU state. The hardware fuse value is an explicit caller input.
#ifndef NV_GSP_BOOTER_PATCH_HPP
#define NV_GSP_BOOTER_PATCH_HPP

#include "NVGspBooterHs.hpp"

#define NV_GSP_BOOTER_PATCH_OK                     0u
#define NV_GSP_BOOTER_PATCH_ERR_NULL_OUTPUT        1u
#define NV_GSP_BOOTER_PATCH_ERR_CONTAINER          2u
#define NV_GSP_BOOTER_PATCH_ERR_FUSE_RANGE         3u
#define NV_GSP_BOOTER_PATCH_ERR_SIGNATURE_RANGE    4u
#define NV_GSP_BOOTER_PATCH_ERR_DEST_RANGE         5u
#define NV_GSP_BOOTER_PATCH_ERR_NULL_DEST          6u
#define NV_GSP_BOOTER_PATCH_ERR_DEST_TOO_SMALL     7u
#define NV_GSP_BOOTER_PATCH_ERR_SOURCE_OVERLAP     8u

// Clang/GCC provide this builtin integer type without libc/stdint.h, keeping
// the helper suitable for both host tests and kernel C++ translation units.
#if defined(__UINTPTR_TYPE__)
typedef __UINTPTR_TYPE__ NVGspBooterPatchPtr;
#elif defined(_MSC_VER) && defined(_WIN64)
// cl.exe host-test builds have no __UINTPTR_TYPE__; use the same pointer-sized
// unsigned types vcruntime.h uses for uintptr_t.
typedef unsigned __int64 NVGspBooterPatchPtr;
#elif defined(_MSC_VER)
typedef unsigned int NVGspBooterPatchPtr;
#else
#error "NVGspBooterPatch.hpp: no pointer-sized unsigned integer type for this compiler"
#endif

struct NVGspBooterPatchPlan {
    NVGspBooterHsU32 hardwareFuseVersion;
    NVGspBooterHsU32 sourceMetadataFuseVersion;
    NVGspBooterHsU32 signatureIndex;
    NVGspBooterHsU32 signatureSourceOffset;
    NVGspBooterHsU32 signatureBytes;
    NVGspBooterHsU32 firmwareSourceOffset;
    NVGspBooterHsU32 firmwareBytes;
    NVGspBooterHsU32 patchDestinationOffset;
    NVGspBooterHsU32 ucodeId;
    bool valid;
};

static inline bool nvGspBooterPatchSucceeded(NVGspBooterHsU32 verdict)
{
    return verdict == NV_GSP_BOOTER_PATCH_OK;
}

static inline const char *nvGspBooterPatchVerdictName(NVGspBooterHsU32 verdict)
{
    switch (verdict) {
        case NV_GSP_BOOTER_PATCH_OK:                  return "ok";
        case NV_GSP_BOOTER_PATCH_ERR_NULL_OUTPUT:     return "null-output";
        case NV_GSP_BOOTER_PATCH_ERR_CONTAINER:       return "container";
        case NV_GSP_BOOTER_PATCH_ERR_FUSE_RANGE:      return "fuse-range";
        case NV_GSP_BOOTER_PATCH_ERR_SIGNATURE_RANGE: return "signature-range";
        case NV_GSP_BOOTER_PATCH_ERR_DEST_RANGE:      return "destination-range";
        case NV_GSP_BOOTER_PATCH_ERR_NULL_DEST:       return "null-destination";
        case NV_GSP_BOOTER_PATCH_ERR_DEST_TOO_SMALL:  return "destination-too-small";
        case NV_GSP_BOOTER_PATCH_ERR_SOURCE_OVERLAP:  return "source-overlap";
        default:                                      return "unknown";
    }
}

static inline bool nvGspBooterPatchRangesOverlap(const unsigned char *source,
                                                  NVGspBooterHsU64 sourceBytes,
                                                  const unsigned char *dest,
                                                  NVGspBooterHsU64 destBytes)
{
    const NVGspBooterPatchPtr s = (NVGspBooterPatchPtr)source;
    const NVGspBooterPatchPtr d = (NVGspBooterPatchPtr)dest;
    const NVGspBooterPatchPtr se = s + (NVGspBooterPatchPtr)sourceBytes;
    const NVGspBooterPatchPtr de = d + (NVGspBooterPatchPtr)destBytes;

    // Treat integer-address overflow as overlap/refusal. The helper never needs
    // to accept a wrapped address interval.
    if (se < s || de < d)
        return true;
    return d < se && s < de;
}

// Reparse the container on every request rather than trusting a caller-supplied
// stale plan. Atomic refusal: *out is written only after every source and
// destination bound required by NVIDIA's selection rule has been proven.
static inline NVGspBooterHsU32 nvGspPlanBooterPatch(
    const unsigned char *container,
    NVGspBooterHsU64 containerBytes,
    NVGspBooterHsU32 hardwareFuseVersion,
    NVGspBooterPatchPlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_PATCH_ERR_NULL_OUTPUT;

    NVGspBooterHsPlan hs = {};
    if (!nvGspBooterHsSucceeded(
            nvGspPlanBooterHs(container,containerBytes,&hs)) || !hs.valid)
        return NV_GSP_BOOTER_PATCH_ERR_CONTAINER;

    NVGspBooterHsU32 signatureIndex = 0u;
    if (hs.signatureCount > 1u) {
        if (hardwareFuseVersion > hs.signatureCount - 1u)
            return NV_GSP_BOOTER_PATCH_ERR_FUSE_RANGE;
        signatureIndex = hs.signatureCount - 1u - hardwareFuseVersion;
    }

    const NVGspBooterHsU64 signatureSource =
        (NVGspBooterHsU64)hs.signatureOffset +
        (NVGspBooterHsU64)signatureIndex * hs.signatureStride;
    if (!nvGspBooterHsFits(containerBytes,signatureSource,hs.signatureStride) ||
        signatureSource > 0xffffffffull)
        return NV_GSP_BOOTER_PATCH_ERR_SIGNATURE_RANGE;

    // Mirror s_patchBooterUcodeSignature()'s strict image-size condition:
    //   imageSize > sigDestOffset
    //   imageSize - sigDestOffset > sigSize
    // The second relation is intentionally strict rather than >= because that
    // is what the production NVIDIA source checks before portMemCopy().
    if (hs.patchLocation >= hs.firmwareBytes ||
        (NVGspBooterHsU64)hs.firmwareBytes - hs.patchLocation <= hs.signatureStride)
        return NV_GSP_BOOTER_PATCH_ERR_DEST_RANGE;
    if (!nvGspBooterHsFits(containerBytes,hs.firmwareOffset,hs.firmwareBytes))
        return NV_GSP_BOOTER_PATCH_ERR_DEST_RANGE;

    NVGspBooterPatchPlan staged = {};
    staged.hardwareFuseVersion = hardwareFuseVersion;
    staged.sourceMetadataFuseVersion = hs.fuseVersion;
    staged.signatureIndex = signatureIndex;
    staged.signatureSourceOffset = (NVGspBooterHsU32)signatureSource;
    staged.signatureBytes = hs.signatureStride;
    staged.firmwareSourceOffset = hs.firmwareOffset;
    staged.firmwareBytes = hs.firmwareBytes;
    staged.patchDestinationOffset = hs.patchLocation;
    staged.ucodeId = hs.ucodeId;
    staged.valid = true;
    *out = staged;
    return NV_GSP_BOOTER_PATCH_OK;
}

// Build the writable image NVIDIA's runtime expects: copy ONLY the validated
// firmware payload into caller-owned private storage, then patch the selected
// production signature into that copy. Full validation, capacity checks and
// source/destination overlap checks happen before the first destination byte is
// changed, so ordinary refusal is atomic and the source container is immutable.
static inline NVGspBooterHsU32 nvGspBuildBooterPrivateImage(
    const unsigned char *container,
    NVGspBooterHsU64 containerBytes,
    NVGspBooterHsU32 hardwareFuseVersion,
    unsigned char *privateImage,
    NVGspBooterHsU64 privateCapacity,
    NVGspBooterPatchPlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_PATCH_ERR_NULL_OUTPUT;

    NVGspBooterPatchPlan plan = {};
    const NVGspBooterHsU32 verdict = nvGspPlanBooterPatch(
        container,containerBytes,hardwareFuseVersion,&plan);
    if (!nvGspBooterPatchSucceeded(verdict))
        return verdict;
    if (!privateImage)
        return NV_GSP_BOOTER_PATCH_ERR_NULL_DEST;
    if (privateCapacity < plan.firmwareBytes)
        return NV_GSP_BOOTER_PATCH_ERR_DEST_TOO_SMALL;
    if (nvGspBooterPatchRangesOverlap(
            container,containerBytes,privateImage,plan.firmwareBytes))
        return NV_GSP_BOOTER_PATCH_ERR_SOURCE_OVERLAP;

    for (NVGspBooterHsU32 i=0u; i<plan.firmwareBytes; ++i)
        privateImage[i]=container[plan.firmwareSourceOffset+i];
    for (NVGspBooterHsU32 i=0u; i<plan.signatureBytes; ++i)
        privateImage[plan.patchDestinationOffset+i]=
            container[plan.signatureSourceOffset+i];

    *out=plan;
    return NV_GSP_BOOTER_PATCH_OK;
}

#endif // NV_GSP_BOOTER_PATCH_HPP
