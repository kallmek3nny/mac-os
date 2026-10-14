// SPDX-License-Identifier: MIT
// Pure Tiger Lake GGTT mapping arithmetic for the runtime-owned sink backing.
#ifndef INTEL_SINK_GGTT_HPP
#define INTEL_SINK_GGTT_HPP

using IGGgttU32 = unsigned int;
using IGGgttU64 = unsigned long long;

// Linux v6.12 vendored in-tree:
//   reference/i915/intel_gtt.h: GEN12_GGTT_PTE_ADDR_MASK = GENMASK_ULL(45,12)
//   reference/i915/intel_gtt.h: GEN8_PAGE_PRESENT = BIT_ULL(0)
//   reference/i915/intel_ggtt.c: gen8_ggtt_pte_encode() = addr | PRESENT for
//   system-memory pages (PTE_LM is not set, PAT index is ignored on this path).
#define IG_SINK_GGTT_PAGE_BYTES_I915_4096          4096u
#define IG_SINK_GGTT_PTE_BYTES_I915_8              8u
#define IG_SINK_GGTT_PTE_PRESENT_I915_0x1          0x1ull
#define IG_SINK_GGTT_PTE_ADDR_MASK_I915_45_12      0x00003FFFFFFFF000ull
#define IG_SINK_GGTT_ADDR_CEILING_I915_45_12       0x00003FFFFFFFFFFFull
#define IG_SINK_GGTT_TABLE_BAR0_OFFSET_I915_0x800000 0x00800000u

#define IG_SINK_GGTT_PLAN_NOT_ATTEMPTED             0u
#define IG_SINK_GGTT_PLAN_OK                        1u
#define IG_SINK_GGTT_PLAN_NULL_OUT                  2u
#define IG_SINK_GGTT_PLAN_ZERO_SIZE                 3u
#define IG_SINK_GGTT_PLAN_GGTT_UNALIGNED            4u
#define IG_SINK_GGTT_PLAN_DMA_UNALIGNED             5u
#define IG_SINK_GGTT_PLAN_DMA_RANGE                 6u
#define IG_SINK_GGTT_PLAN_DMA_SPAN_SHORT            7u
#define IG_SINK_GGTT_PLAN_GGTT_RANGE                8u
#define IG_SINK_GGTT_PLAN_LIMIT                     9u

struct IGSinkGgttPlan {
    IGGgttU32 firstPte;
    IGGgttU32 pteCount;
    IGGgttU64 mappedBytes;
    IGGgttU64 ggttBase;
    IGGgttU64 intelDmaBase;
};

static inline void igSinkGgttPlanClear(IGSinkGgttPlan *out)
{
    out->firstPte=0u;
    out->pteCount=0u;
    out->mappedBytes=0ull;
    out->ggttBase=0ull;
    out->intelDmaBase=0ull;
}

static inline bool igSinkGgttPlanSucceeded(IGGgttU32 verdict)
{
    return verdict==IG_SINK_GGTT_PLAN_OK;
}

static inline const char *igSinkGgttPlanVerdictName(IGGgttU32 verdict)
{
    switch (verdict) {
        case IG_SINK_GGTT_PLAN_NOT_ATTEMPTED:  return "not-attempted";
        case IG_SINK_GGTT_PLAN_OK:             return "ok";
        case IG_SINK_GGTT_PLAN_NULL_OUT:       return "null-out";
        case IG_SINK_GGTT_PLAN_ZERO_SIZE:      return "zero-size";
        case IG_SINK_GGTT_PLAN_GGTT_UNALIGNED: return "ggtt-unaligned";
        case IG_SINK_GGTT_PLAN_DMA_UNALIGNED:  return "dma-unaligned";
        case IG_SINK_GGTT_PLAN_DMA_RANGE:      return "dma-range";
        case IG_SINK_GGTT_PLAN_DMA_SPAN_SHORT: return "dma-span-short";
        case IG_SINK_GGTT_PLAN_GGTT_RANGE:     return "ggtt-range";
        default:                               return "unknown";
    }
}

static inline bool igSinkGgttPageAligned(IGGgttU64 value)
{
    return (value & (IGGgttU64)(IG_SINK_GGTT_PAGE_BYTES_I915_4096-1u))==0ull;
}

static inline IGGgttU64 igSinkGgttRoundUpBytes(IGGgttU64 bytes)
{
    if (!bytes) return 0ull;
    const IGGgttU64 add=(IGGgttU64)IG_SINK_GGTT_PAGE_BYTES_I915_4096-1ull;
    if (bytes > ~0ull-add) return 0ull;
    return (bytes+add) & ~add;
}

static inline bool igSinkGgttPtePresent(IGGgttU64 pte)
{
    return (pte & IG_SINK_GGTT_PTE_PRESENT_I915_0x1)!=0ull;
}

static inline IGGgttU64 igSinkGgttPteForPage(IGGgttU64 intelDmaBase,
                                             IGGgttU32 pageIndex)
{
    if (!igSinkGgttPageAligned(intelDmaBase)) return 0ull;
    const IGGgttU64 offset=(IGGgttU64)pageIndex*(IGGgttU64)IG_SINK_GGTT_PAGE_BYTES_I915_4096;
    if (offset > IG_SINK_GGTT_ADDR_CEILING_I915_45_12-intelDmaBase) return 0ull;
    const IGGgttU64 addr=intelDmaBase+offset;
    if (addr & ~IG_SINK_GGTT_PTE_ADDR_MASK_I915_45_12) return 0ull;
    return addr | IG_SINK_GGTT_PTE_PRESENT_I915_0x1;
}

// Builds the exact page range a caller may consider mapping. The caller must
// still prove every target PTE is currently non-present before doing any write.
// `dmaSpanBytes` is the contiguous Intel IOVM segment length, not the physical
// candidate address published for the future 3050 side.
static inline IGGgttU32 igSinkGgttPlan(IGGgttU64 ggttBase,
                                       IGGgttU64 surfaceBytes,
                                       IGGgttU64 intelDmaBase,
                                       IGGgttU64 dmaSpanBytes,
                                       IGGgttU32 ggttEntries,
                                       IGSinkGgttPlan *out)
{
    if (out==0) return IG_SINK_GGTT_PLAN_NULL_OUT;
    igSinkGgttPlanClear(out);
    if (!surfaceBytes) return IG_SINK_GGTT_PLAN_ZERO_SIZE;
    if (!igSinkGgttPageAligned(ggttBase)) return IG_SINK_GGTT_PLAN_GGTT_UNALIGNED;
    if (!igSinkGgttPageAligned(intelDmaBase)) return IG_SINK_GGTT_PLAN_DMA_UNALIGNED;
    if (intelDmaBase & ~IG_SINK_GGTT_PTE_ADDR_MASK_I915_45_12)
        return IG_SINK_GGTT_PLAN_DMA_RANGE;

    const IGGgttU64 mapped=igSinkGgttRoundUpBytes(surfaceBytes);
    if (!mapped) return IG_SINK_GGTT_PLAN_DMA_RANGE;
    if (mapped>dmaSpanBytes) return IG_SINK_GGTT_PLAN_DMA_SPAN_SHORT;
    if ((mapped-1ull)>IG_SINK_GGTT_ADDR_CEILING_I915_45_12-intelDmaBase)
        return IG_SINK_GGTT_PLAN_DMA_RANGE;

    const IGGgttU64 first64=ggttBase/(IGGgttU64)IG_SINK_GGTT_PAGE_BYTES_I915_4096;
    const IGGgttU64 count64=mapped/(IGGgttU64)IG_SINK_GGTT_PAGE_BYTES_I915_4096;
    if (!ggttEntries || first64>=(IGGgttU64)ggttEntries ||
        !count64 || count64>(IGGgttU64)ggttEntries-first64 ||
        first64>0xFFFFFFFFull || count64>0xFFFFFFFFull)
        return IG_SINK_GGTT_PLAN_GGTT_RANGE;

    out->firstPte=(IGGgttU32)first64;
    out->pteCount=(IGGgttU32)count64;
    out->mappedBytes=mapped;
    out->ggttBase=ggttBase;
    out->intelDmaBase=intelDmaBase;
    return IG_SINK_GGTT_PLAN_OK;
}

#endif // INTEL_SINK_GGTT_HPP
