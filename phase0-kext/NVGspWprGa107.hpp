// SPDX-License-Identifier: MIT
// Pure GA107/TU102-through-Ada cold-boot WPR policy derived from NVIDIA open RM.
#ifndef NV_GSP_WPR_GA107_HPP
#define NV_GSP_WPR_GA107_HPP

#include "NVGspWprMeta.hpp"

#define NV_GSP_GA107_WPR_POLICY_NOT_ATTEMPTED       0u
#define NV_GSP_GA107_WPR_POLICY_OK                  1u
#define NV_GSP_GA107_WPR_POLICY_NULL_INPUT          2u
#define NV_GSP_GA107_WPR_POLICY_NULL_OUTPUT         3u
#define NV_GSP_GA107_WPR_POLICY_BAD_FRAMEBUFFER     4u
#define NV_GSP_GA107_WPR_POLICY_BAD_VBIOS_BOUNDARY  5u
#define NV_GSP_GA107_WPR_POLICY_BAD_IMAGE_SIZE      6u
#define NV_GSP_GA107_WPR_POLICY_UNDERFLOW           7u
#define NV_GSP_GA107_WPR_POLICY_OVERFLOW            8u
#define NV_GSP_GA107_WPR_POLICY_BAD_LAYOUT           9u
#define NV_GSP_GA107_WPR_POLICY_LIMIT               10u

// Source-backed GA107 / TU102-through-Ada bare-metal policy.
//
// kernel_gsp_frts_tu102.c: FRTS is 1 MiB when present.
// generated g_kernel_gsp_nvoc.h/c: TU102-through-AD107 non-WPR heap is 1 MiB.
// gsp_fw_heap.h + generated HAL fields for GA107:
//   LibOS3 bare-metal OS carveout 22 MiB
//   architecture base 8 MiB
//   96 KiB per rounded-up GiB of usable FB, aligned to 1 MiB
//   48 KiB * 2048 client-allocation reserve = 96 MiB
//   bare-metal min/max 88/280 MiB
// generated HAL: GA107 pre-scrubbed top-of-FB window is 256 MiB.
// kernel_gsp_tu102.c: first normal cold boot has zero WPR end margin unless an
// explicit registry override asks for APPLY_ALWAYS. This helper models the
// default non-override path only; recovery/retry margin is deliberately a
// different future policy.
#define NV_GSP_GA107_FRTS_BYTES                     0x00100000ull
#define NV_GSP_GA107_NON_WPR_HEAP_BYTES             0x00100000ull
#define NV_GSP_GA107_WPR_META_ALIGNED_BYTES         0x00100000ull
#define NV_GSP_GA107_PRESCRUBBED_TOP_FB_BYTES       0x10000000ull
#define NV_GSP_GA107_COLD_BOOT_WPR_END_MARGIN       0ull

#define NV_GSP_GA107_FW_HEAP_OS_BYTES               0x01600000ull
#define NV_GSP_GA107_FW_HEAP_BASE_BYTES             0x00800000ull
#define NV_GSP_GA107_FW_HEAP_PER_GB_BYTES           0x00018000ull
#define NV_GSP_GA107_FW_HEAP_CLIENT_BYTES           0x06000000ull
#define NV_GSP_GA107_FW_HEAP_MIN_BYTES              0x05800000ull
#define NV_GSP_GA107_FW_HEAP_MAX_BYTES              0x11800000ull
#define NV_GSP_GA107_GIB_BYTES                      0x40000000ull
#define NV_GSP_GA107_MIB_BYTES                      0x00100000ull

struct NVGspGa107WprInput {
    // Runtime facts, NOT constants. `fbSize` must be the usable framebuffer size
    // RM would feed to kgspPopulateWprMeta(), not marketing VRAM capacity.
    NVGspWprU64 fbSize;
    NVGspWprU64 vgaWorkspaceOffset;
    bool mmuLockValid;
    NVGspWprU64 mmuLockLo;

    // Exact unpadded WPR metadata source sizes.
    NVGspWprU64 bootloaderSize;
    NVGspWprU64 radix3ElfSize;
};

struct NVGspGa107HeapPlan {
    NVGspWprU64 roundedFbGiB;
    NVGspWprU64 formulaBytes;
    NVGspWprU64 preWprBytes;
    NVGspWprU64 postWprBytes;
    NVGspWprU64 maxPrescrubbedHeapBytes;
    NVGspWprU64 heapBytes;
};

struct NVGspGa107WprPlan {
    NVGspWprU64 vbiosReservedOffset;
    NVGspWprU64 wprEndMargin;
    NVGspGa107HeapPlan heap;
    NVGspWprLayoutInput layoutInput;
    NVGspWprLayoutPlan layout;
};

static inline bool nvGspGa107WprPolicySucceeded(NVGspWprU32 verdict)
{
    return verdict == NV_GSP_GA107_WPR_POLICY_OK;
}

static inline const char *nvGspGa107WprPolicyVerdictName(NVGspWprU32 verdict)
{
    switch (verdict) {
        case NV_GSP_GA107_WPR_POLICY_NOT_ATTEMPTED:      return "not-attempted";
        case NV_GSP_GA107_WPR_POLICY_OK:                 return "ok";
        case NV_GSP_GA107_WPR_POLICY_NULL_INPUT:         return "null-input";
        case NV_GSP_GA107_WPR_POLICY_NULL_OUTPUT:        return "null-output";
        case NV_GSP_GA107_WPR_POLICY_BAD_FRAMEBUFFER:    return "bad-framebuffer";
        case NV_GSP_GA107_WPR_POLICY_BAD_VBIOS_BOUNDARY: return "bad-vbios-boundary";
        case NV_GSP_GA107_WPR_POLICY_BAD_IMAGE_SIZE:     return "bad-image-size";
        case NV_GSP_GA107_WPR_POLICY_UNDERFLOW:          return "underflow";
        case NV_GSP_GA107_WPR_POLICY_OVERFLOW:           return "overflow";
        case NV_GSP_GA107_WPR_POLICY_BAD_LAYOUT:         return "bad-layout";
        default:                                         return "unknown";
    }
}

static inline NVGspWprU32 nvGspGa107RoundFbGiB(NVGspWprU64 fbSize,
                                               NVGspWprU64 *outGiB)
{
    if (!outGiB || !fbSize)
        return NV_GSP_GA107_WPR_POLICY_BAD_FRAMEBUFFER;
    if (fbSize > ~0ull - (NV_GSP_GA107_GIB_BYTES - 1ull))
        return NV_GSP_GA107_WPR_POLICY_OVERFLOW;
    const NVGspWprU64 rounded =
        (fbSize + NV_GSP_GA107_GIB_BYTES - 1ull) / NV_GSP_GA107_GIB_BYTES;
    if (!rounded)
        return NV_GSP_GA107_WPR_POLICY_OVERFLOW;
    *outGiB = rounded;
    return NV_GSP_GA107_WPR_POLICY_OK;
}

// Reproduce open RM's GA107 bare-metal LibOS3 FW heap formula, including the
// pre-scrubbed top-of-FB clamp. `gspFwOffset` is independently computable from
// the top-down placement before heap size is known, exactly as TU102 does.
static inline NVGspWprU32 nvGspGa107PlanFwHeap(NVGspWprU64 fbSize,
                                              NVGspWprU64 gspFwOffset,
                                              NVGspGa107HeapPlan *out)
{
    if (!out)
        return NV_GSP_GA107_WPR_POLICY_NULL_OUTPUT;
    if (!fbSize || !gspFwOffset || gspFwOffset >= fbSize)
        return NV_GSP_GA107_WPR_POLICY_BAD_FRAMEBUFFER;

    NVGspGa107HeapPlan staged = {};
    NVGspWprU32 verdict = nvGspGa107RoundFbGiB(fbSize, &staged.roundedFbGiB);
    if (!nvGspGa107WprPolicySucceeded(verdict))
        return verdict;

    if (staged.roundedFbGiB > (~0ull / NV_GSP_GA107_FW_HEAP_PER_GB_BYTES))
        return NV_GSP_GA107_WPR_POLICY_OVERFLOW;
    NVGspWprU64 perGb = staged.roundedFbGiB * NV_GSP_GA107_FW_HEAP_PER_GB_BYTES;
    perGb = nvGspWprAlignUp(perGb, NV_GSP_GA107_MIB_BYTES);
    if (!perGb)
        return NV_GSP_GA107_WPR_POLICY_OVERFLOW;

    const NVGspWprU64 fixed = NV_GSP_GA107_FW_HEAP_OS_BYTES +
                              NV_GSP_GA107_FW_HEAP_BASE_BYTES +
                              NV_GSP_GA107_FW_HEAP_CLIENT_BYTES;
    if (fixed > ~0ull - perGb)
        return NV_GSP_GA107_WPR_POLICY_OVERFLOW;
    staged.formulaBytes = fixed + perGb;
    if (staged.formulaBytes < NV_GSP_GA107_FW_HEAP_MIN_BYTES)
        staged.formulaBytes = NV_GSP_GA107_FW_HEAP_MIN_BYTES;
    if (staged.formulaBytes > NV_GSP_GA107_FW_HEAP_MAX_BYTES)
        staged.formulaBytes = NV_GSP_GA107_FW_HEAP_MAX_BYTES;

    staged.preWprBytes = NV_GSP_GA107_WPR_META_ALIGNED_BYTES +
                         NV_GSP_GA107_NON_WPR_HEAP_BYTES;
    staged.postWprBytes = nvGspWprAlignUp(fbSize - gspFwOffset,
                                         NV_GSP_GA107_MIB_BYTES);
    if (!staged.postWprBytes)
        return NV_GSP_GA107_WPR_POLICY_OVERFLOW;

    if (staged.preWprBytes > NV_GSP_GA107_PRESCRUBBED_TOP_FB_BYTES ||
        staged.postWprBytes > NV_GSP_GA107_PRESCRUBBED_TOP_FB_BYTES - staged.preWprBytes)
        return NV_GSP_GA107_WPR_POLICY_UNDERFLOW;

    staged.maxPrescrubbedHeapBytes =
        NV_GSP_GA107_PRESCRUBBED_TOP_FB_BYTES - staged.preWprBytes - staged.postWprBytes;
    // Open RM floors the maximum to whole MiB before clamping the heap.
    staged.maxPrescrubbedHeapBytes =
        (staged.maxPrescrubbedHeapBytes / NV_GSP_GA107_MIB_BYTES) * NV_GSP_GA107_MIB_BYTES;
    if (!staged.maxPrescrubbedHeapBytes)
        return NV_GSP_GA107_WPR_POLICY_UNDERFLOW;

    staged.heapBytes = staged.formulaBytes;
    if (staged.heapBytes > staged.maxPrescrubbedHeapBytes)
        staged.heapBytes = staged.maxPrescrubbedHeapBytes;
    if (!staged.heapBytes)
        return NV_GSP_GA107_WPR_POLICY_UNDERFLOW;

    *out = staged;
    return NV_GSP_GA107_WPR_POLICY_OK;
}

// Compose the source-backed GA107 policy with the existing TU102 placement
// planner. Only fbSize / VGA workspace / MMU-lock facts remain runtime inputs.
static inline NVGspWprU32 nvGspPlanGa107ColdBootWpr(const NVGspGa107WprInput *in,
                                                    NVGspGa107WprPlan *out)
{
    if (!in) return NV_GSP_GA107_WPR_POLICY_NULL_INPUT;
    if (!out) return NV_GSP_GA107_WPR_POLICY_NULL_OUTPUT;
    if (!in->fbSize) return NV_GSP_GA107_WPR_POLICY_BAD_FRAMEBUFFER;
    if (!in->vgaWorkspaceOffset || in->vgaWorkspaceOffset > in->fbSize)
        return NV_GSP_GA107_WPR_POLICY_BAD_VBIOS_BOUNDARY;
    if (!in->bootloaderSize || !in->radix3ElfSize)
        return NV_GSP_GA107_WPR_POLICY_BAD_IMAGE_SIZE;

    NVGspGa107WprPlan staged = {};
    staged.vbiosReservedOffset = in->vgaWorkspaceOffset;
    if (in->mmuLockValid) {
        if (!in->mmuLockLo || in->mmuLockLo > in->fbSize)
            return NV_GSP_GA107_WPR_POLICY_BAD_VBIOS_BOUNDARY;
        if (in->mmuLockLo < staged.vbiosReservedOffset)
            staged.vbiosReservedOffset = in->mmuLockLo;
    }
    staged.wprEndMargin = NV_GSP_GA107_COLD_BOOT_WPR_END_MARGIN;

    // Reproduce just the top-down portion needed to calculate postWprHeapSize.
    const NVGspWprU64 wprEnd = nvGspWprAlignDown(
        staged.vbiosReservedOffset, NV_GSP_WPR_ALIGNMENT_BYTES);
    if (!wprEnd || NV_GSP_GA107_FRTS_BYTES > wprEnd)
        return NV_GSP_GA107_WPR_POLICY_UNDERFLOW;
    const NVGspWprU64 frtsOffset = wprEnd - NV_GSP_GA107_FRTS_BYTES;
    if (in->bootloaderSize > frtsOffset)
        return NV_GSP_GA107_WPR_POLICY_UNDERFLOW;
    const NVGspWprU64 bootBinOffset = nvGspWprAlignDown(
        frtsOffset - in->bootloaderSize, NV_GSP_WPR_BOOT_ALIGNMENT_BYTES);
    if (!bootBinOffset || in->radix3ElfSize > bootBinOffset)
        return NV_GSP_GA107_WPR_POLICY_UNDERFLOW;
    const NVGspWprU64 gspFwOffset = nvGspWprAlignDown(
        bootBinOffset - in->radix3ElfSize, NV_GSP_WPR_ELF_ALIGNMENT_BYTES);
    if (!gspFwOffset || gspFwOffset >= in->fbSize)
        return NV_GSP_GA107_WPR_POLICY_BAD_LAYOUT;

    NVGspWprU32 heapVerdict = nvGspGa107PlanFwHeap(in->fbSize, gspFwOffset,
                                                   &staged.heap);
    if (!nvGspGa107WprPolicySucceeded(heapVerdict))
        return heapVerdict;

    staged.layoutInput.fbSize = in->fbSize;
    staged.layoutInput.vbiosReservedOffset = staged.vbiosReservedOffset;
    staged.layoutInput.wprEndMargin = staged.wprEndMargin;
    staged.layoutInput.frtsSize = NV_GSP_GA107_FRTS_BYTES;
    staged.layoutInput.bootloaderSize = in->bootloaderSize;
    staged.layoutInput.radix3ElfSize = in->radix3ElfSize;
    staged.layoutInput.nonWprHeapRawBytes = NV_GSP_GA107_NON_WPR_HEAP_BYTES;
    staged.layoutInput.wprHeapBytes = staged.heap.heapBytes;

    const NVGspWprU32 layoutVerdict =
        nvGspPlanWprLayout(&staged.layoutInput, &staged.layout);
    if (!nvGspWprPlanSucceeded(layoutVerdict))
        return NV_GSP_GA107_WPR_POLICY_BAD_LAYOUT;

    // The independent top-down rehearsal and the generic full planner must
    // agree. This guards future drift between the two helpers.
    if (staged.layout.gspFwOffset != gspFwOffset ||
        staged.layout.gspFwWprEnd != wprEnd ||
        staged.layout.frtsOffset != frtsOffset ||
        staged.layout.bootBinOffset != bootBinOffset)
        return NV_GSP_GA107_WPR_POLICY_BAD_LAYOUT;

    *out = staged;
    return NV_GSP_GA107_WPR_POLICY_OK;
}

#endif // NV_GSP_WPR_GA107_HPP
