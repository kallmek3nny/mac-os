// SPDX-License-Identifier: MIT
// Pure GA107/Turing-through-Ada first-cold-boot WPR policy.
//
// This file intentionally contains only the policy pieces delegated by
// NVGspWprMeta.hpp to open RM helpers. It does not place WPR regions and it does
// not touch hardware.
#ifndef NV_GSP_WPR_POLICY_HPP
#define NV_GSP_WPR_POLICY_HPP

using NVGspWprPolicyU32 = unsigned int;
using NVGspWprPolicyU64 = unsigned long long;

#define NV_GSP_WPR_POLICY_NOT_ATTEMPTED       0u
#define NV_GSP_WPR_POLICY_OK                  1u
#define NV_GSP_WPR_POLICY_NULL_INPUT          2u
#define NV_GSP_WPR_POLICY_NULL_OUTPUT         3u
#define NV_GSP_WPR_POLICY_BAD_FRAMEBUFFER     4u
#define NV_GSP_WPR_POLICY_PRESCRUB_TOO_SMALL 5u
#define NV_GSP_WPR_POLICY_OVERFLOW            6u
#define NV_GSP_WPR_POLICY_ZERO_HEAP           7u
#define NV_GSP_WPR_POLICY_LIMIT               8u

#define NV_GSP_WPR_POLICY_KIB                  1024ull
#define NV_GSP_WPR_POLICY_MIB                  (1024ull * 1024ull)
#define NV_GSP_WPR_POLICY_GIB                  (1024ull * 1024ull * 1024ull)

// NVIDIA open RM provenance, v610.57.04:
// - Turing-Ada first normal boot returns no WPR end margin unless a registry
//   override is requested or bootAttempts > 0.
// - TU102 FRTS is hard-coded to 1 MiB.
// - TU102-Ada non-WPR heap HAL returns 1 MiB.
// - GA102/GA107 LibOS3 bare-metal OS carveout is 22 MiB.
// - Turing-Ada RM heap base is 8 MiB.
// - heap growth is 96 KiB per rounded-up GiB of usable FB, then rounded to MiB.
// - client allocation reserve is 48 KiB * 2048 channels = 96 MiB.
// - LibOS3 bare-metal minimum heap is 88 MiB.
// - 280 MiB is the bare-metal REGISTRY-OVERRIDE maximum. It is intentionally
//   not applied to the ordinary calculated path: open RM's normal path clamps
//   only to the pre-scrubbed-space limit passed into _kgspCalculateFwHeapSize().
// - GA107 has no scrubber ucode in this HAL generation, and its pre-scrubbed
//   top-of-FB window is 256 MiB. With nonzero pre/post regions, open RM clamps
//   the calculated heap to what remains in that window, in whole MiB.
#define NV_GSP_GA107_FIRST_BOOT_WPR_END_MARGIN_BYTES  0ull
#define NV_GSP_GA107_FRTS_BYTES                       (1ull * NV_GSP_WPR_POLICY_MIB)
#define NV_GSP_GA107_NON_WPR_HEAP_BYTES               (1ull * NV_GSP_WPR_POLICY_MIB)
#define NV_GSP_GA107_PRESCRUBBED_TOP_FB_BYTES         (256ull * NV_GSP_WPR_POLICY_MIB)
#define NV_GSP_GA107_LIBOS3_OS_CARVEOUT_BYTES         (22ull * NV_GSP_WPR_POLICY_MIB)
#define NV_GSP_GA107_RM_HEAP_BASE_BYTES               (8ull * NV_GSP_WPR_POLICY_MIB)
#define NV_GSP_GA107_HEAP_PER_GIB_BYTES               (96ull * NV_GSP_WPR_POLICY_KIB)
#define NV_GSP_GA107_CLIENT_HEAP_BYTES                ((48ull * NV_GSP_WPR_POLICY_KIB) * 2048ull)
#define NV_GSP_GA107_MIN_WPR_HEAP_BYTES               (88ull * NV_GSP_WPR_POLICY_MIB)
#define NV_GSP_GA107_OVERRIDE_MAX_WPR_HEAP_BYTES      (280ull * NV_GSP_WPR_POLICY_MIB)

struct NVGspGa107WprPolicyInput {
    NVGspWprPolicyU64 usableFbBytes;
    NVGspWprPolicyU64 preWprHeapBytes;
    NVGspWprPolicyU64 postWprHeapBytes;
};

struct NVGspGa107WprPolicyPlan {
    NVGspWprPolicyU64 wprEndMarginBytes;
    NVGspWprPolicyU64 frtsBytes;
    NVGspWprPolicyU64 nonWprHeapRawBytes;
    NVGspWprPolicyU64 usableFbGiB;
    NVGspWprPolicyU64 perFbHeapBytes;
    NVGspWprPolicyU64 calculatedHeapBytes;
    NVGspWprPolicyU64 maxPrescrubbedHeapBytes;
    NVGspWprPolicyU64 wprHeapBytes;
    bool prescrubClampApplied;
    bool valid;
};

static inline bool nvGspGa107WprPolicySucceeded(NVGspWprPolicyU32 verdict)
{
    return verdict == NV_GSP_WPR_POLICY_OK;
}

static inline const char *nvGspGa107WprPolicyVerdictName(NVGspWprPolicyU32 verdict)
{
    switch (verdict) {
        case NV_GSP_WPR_POLICY_NOT_ATTEMPTED:       return "not-attempted";
        case NV_GSP_WPR_POLICY_OK:                  return "ok";
        case NV_GSP_WPR_POLICY_NULL_INPUT:          return "null-input";
        case NV_GSP_WPR_POLICY_NULL_OUTPUT:         return "null-output";
        case NV_GSP_WPR_POLICY_BAD_FRAMEBUFFER:     return "bad-framebuffer";
        case NV_GSP_WPR_POLICY_PRESCRUB_TOO_SMALL: return "prescrub-too-small";
        case NV_GSP_WPR_POLICY_OVERFLOW:            return "overflow";
        case NV_GSP_WPR_POLICY_ZERO_HEAP:           return "zero-heap";
        default:                                     return "unknown";
    }
}

static inline bool nvGspWprPolicyAdd(NVGspWprPolicyU64 a,
                                     NVGspWprPolicyU64 b,
                                     NVGspWprPolicyU64 *out)
{
    if (!out || a > ~0ull - b) return false;
    *out = a + b;
    return true;
}

static inline bool nvGspWprPolicyMul(NVGspWprPolicyU64 a,
                                     NVGspWprPolicyU64 b,
                                     NVGspWprPolicyU64 *out)
{
    if (!out) return false;
    if (a != 0ull && b > ~0ull / a) return false;
    *out = a * b;
    return true;
}

static inline bool nvGspWprPolicyAlignUpMiB(NVGspWprPolicyU64 value,
                                            NVGspWprPolicyU64 *out)
{
    if (!out) return false;
    const NVGspWprPolicyU64 mask = NV_GSP_WPR_POLICY_MIB - 1ull;
    if (value > ~0ull - mask) return false;
    *out = (value + mask) & ~mask;
    return true;
}

// Mirrors the normal, non-vGPU, no-registry-override GA107 first-cold-boot
// policy. The caller supplies the pre/post sizes already computed by TU102's
// placement math. When both are zero, open RM deliberately disables the
// pre-scrubbed-space clamp; recovery-margin code relies on that exact path.
// When either is nonzero, this function refuses an impossible 256 MiB window
// instead of reproducing unsigned subtraction underflow.
static inline NVGspWprPolicyU32 nvGspPlanGa107FirstBootWprPolicy(
    const NVGspGa107WprPolicyInput *in,
    NVGspGa107WprPolicyPlan *out)
{
    if (!in) return NV_GSP_WPR_POLICY_NULL_INPUT;
    if (!out) return NV_GSP_WPR_POLICY_NULL_OUTPUT;
    if (!in->usableFbBytes) return NV_GSP_WPR_POLICY_BAD_FRAMEBUFFER;

    NVGspWprPolicyU64 prePost = 0ull;
    if (!nvGspWprPolicyAdd(in->preWprHeapBytes, in->postWprHeapBytes, &prePost))
        return NV_GSP_WPR_POLICY_OVERFLOW;
    const bool applyPrescrubClamp = prePost != 0ull;
    if (applyPrescrubClamp && prePost >= NV_GSP_GA107_PRESCRUBBED_TOP_FB_BYTES)
        return NV_GSP_WPR_POLICY_PRESCRUB_TOO_SMALL;

    const NVGspWprPolicyU64 fbGiB =
        (in->usableFbBytes / NV_GSP_WPR_POLICY_GIB) +
        ((in->usableFbBytes % NV_GSP_WPR_POLICY_GIB) ? 1ull : 0ull);

    NVGspWprPolicyU64 perFbRaw = 0ull;
    NVGspWprPolicyU64 perFbAligned = 0ull;
    if (!nvGspWprPolicyMul(fbGiB, NV_GSP_GA107_HEAP_PER_GIB_BYTES, &perFbRaw) ||
        !nvGspWprPolicyAlignUpMiB(perFbRaw, &perFbAligned))
        return NV_GSP_WPR_POLICY_OVERFLOW;

    NVGspWprPolicyU64 heap = NV_GSP_GA107_LIBOS3_OS_CARVEOUT_BYTES;
    if (!nvGspWprPolicyAdd(heap, NV_GSP_GA107_RM_HEAP_BASE_BYTES, &heap) ||
        !nvGspWprPolicyAdd(heap, perFbAligned, &heap) ||
        !nvGspWprPolicyAdd(heap, NV_GSP_GA107_CLIENT_HEAP_BYTES, &heap))
        return NV_GSP_WPR_POLICY_OVERFLOW;

    if (heap < NV_GSP_GA107_MIN_WPR_HEAP_BYTES)
        heap = NV_GSP_GA107_MIN_WPR_HEAP_BYTES;
    const NVGspWprPolicyU64 calculatedHeap = heap;

    NVGspWprPolicyU64 maxPrescrubbedHeap = ~0ull;
    if (applyPrescrubClamp) {
        const NVGspWprPolicyU64 prescrubRemaining =
            NV_GSP_GA107_PRESCRUBBED_TOP_FB_BYTES - prePost;
        maxPrescrubbedHeap =
            (prescrubRemaining / NV_GSP_WPR_POLICY_MIB) * NV_GSP_WPR_POLICY_MIB;
        if (heap > maxPrescrubbedHeap)
            heap = maxPrescrubbedHeap;
        if (!heap)
            return NV_GSP_WPR_POLICY_ZERO_HEAP;
    }

    NVGspGa107WprPolicyPlan staged = {};
    staged.wprEndMarginBytes = NV_GSP_GA107_FIRST_BOOT_WPR_END_MARGIN_BYTES;
    staged.frtsBytes = NV_GSP_GA107_FRTS_BYTES;
    staged.nonWprHeapRawBytes = NV_GSP_GA107_NON_WPR_HEAP_BYTES;
    staged.usableFbGiB = fbGiB;
    staged.perFbHeapBytes = perFbAligned;
    staged.calculatedHeapBytes = calculatedHeap;
    staged.maxPrescrubbedHeapBytes = maxPrescrubbedHeap;
    staged.wprHeapBytes = heap;
    staged.prescrubClampApplied = applyPrescrubClamp;
    staged.valid = true;
    *out = staged;
    return NV_GSP_WPR_POLICY_OK;
}

#endif // NV_GSP_WPR_POLICY_HPP
