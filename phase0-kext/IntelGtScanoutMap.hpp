// SPDX-License-Identifier: MIT
// Dependency-free plan for borrowing the INHERITED pipe A plane 1 scanout into
// IntelGtBringup's private PPGTT, so BCS0 can fill a rectangle of the surface
// the firmware is already presenting. Reached only at iggt-bringup=4 (t-d0bcef).
//
// No IOKit types, MMIO, allocation or sleeps: IntelGtScanout.cpp owns those.
// This layer owns the arithmetic that is easy to get subtly wrong while still
// producing a well-formed GPU address:
//
//   1. Which GGTT PTE indices cover PLANE_SURF .. PLANE_SURF + extent, and that
//      the run fits the GGTT installGGTT() measured without touching the run
//      installGGTT() reserved for the pool. The shell only READS those PTEs.
//   2. How one such PTE decodes to a page address. Linux v6.12 writes a Gen12
//      GGTT PTE as addr | GEN8_PAGE_PRESENT (reference/i915/intel_ggtt.c:281)
//      with addr inside GEN12_GGTT_PTE_ADDR_MASK = GENMASK_ULL(45, 12)
//      (reference/i915/intel_gtt.h:100; PRESENT is bit 0, :152), and
//      display/intel_plane_initial.c:71 recovers the inherited plane's address
//      with that same mask. GEN12_GGTT_PTE_LM (intel_gtt.h:97) names device
//      local memory, which an integrated Tiger Lake does not have, so a PTE
//      carrying it is refused rather than reinterpreted.
//   3. Where the borrowed pages sit in the private PPGTT: whole page tables
//      hung from PD entries 1.., so pd[0] (pool, guard and persistent arena,
//      GPU 0 .. 2 MiB) never shares a table with a borrowed page.
//   4. One requested rectangle as the destination gtEncodeFill32() accepts. A
//      rectangle that is not wholly inside the visible plane is refused, never
//      clipped.
//
// Leaves use the caller's PAT index. IntelGtBringup passes fPatIndex, the
// existing UC entry claimEngine() selected. Linux gives the adopted initial
// framebuffer I915_CACHE_NONE on TGL (display/intel_plane_initial.c:189-190;
// HAS_WT is HAS_EDRAM, i915_drv.h:657-659), TGL_CACHELEVEL maps that to PAT 3
// (i915_pci.c:58), and gt/intel_gtt.c:547 programs PAT 3 as GEN8_PPAT_UC.
// Unvendored paths are drivers/gpu/drm/i915/ at tag v6.12.
#ifndef IntelGtScanoutMap_hpp
#define IntelGtScanoutMap_hpp

#include "IntelGtPpgtt.hpp"

using IGGtScanU32 = unsigned int;
using IGGtScanU64 = unsigned long long;

// Private-PPGTT window. pd[0] is the pool/arena table, so the window starts at
// the next 2 MiB boundary. 32 tables (64 MiB) hold a 3840 x 2400 x 4 plane.
#define IG_GTSCAN_PAGE_BYTES_POLICY_4096                4096u
#define IG_GTSCAN_PT_LEAVES_GEN8_PLUS_512                512u
#define IG_GTSCAN_FIRST_PD_INDEX_POLICY_1                  1u
#define IG_GTSCAN_MAX_PTS_POLICY_32                       32u
#define IG_GTSCAN_MAX_PAGES_POLICY_16384               16384u
#define IG_GTSCAN_GPU_BASE_0x200000               0x00200000u
#define IG_GTSCAN_WINDOW_BYTES_0x4000000          0x04000000u

// The fill encoder's own limits (IntelGtBlitter.cpp fits(), gtEncodeFill32()).
#define IG_GTSCAN_FILL_BYTES_PER_PIXEL_4                   4u
#define IG_GTSCAN_FILL_MAX_EXTENT_16384                16384u
#define IG_GTSCAN_FILL_MAX_PITCH_32767                 32767u

// Gen12 GGTT PTE fields, reference/i915/intel_gtt.h:152, :97 and :100.
#define IG_GTSCAN_GGTT_PRESENT_I915_0x1                  0x1ull
#define IG_GTSCAN_GGTT_LM_I915_0x2                       0x2ull
#define IG_GTSCAN_GGTT_ADDR_MASK_I915_45_12  0x00003FFFFFFFF000ull

static_assert(IG_GTSCAN_GPU_BASE_0x200000 ==
              (IG_GTSCAN_FIRST_PD_INDEX_POLICY_1 << IG_GTPG_PD_SHIFT_I915_21),
              "the scanout window must start at its first PD entry");
static_assert(IG_GTSCAN_MAX_PAGES_POLICY_16384 ==
              IG_GTSCAN_MAX_PTS_POLICY_32 * IG_GTSCAN_PT_LEAVES_GEN8_PLUS_512,
              "the page cap must equal the table cap");
static_assert(IG_GTSCAN_WINDOW_BYTES_0x4000000 ==
              IG_GTSCAN_MAX_PAGES_POLICY_16384 * IG_GTSCAN_PAGE_BYTES_POLICY_4096,
              "the window size must equal the page cap");

#define IG_GTSCAN_PLAN_NOT_ATTEMPTED                       0u
#define IG_GTSCAN_PLAN_OK                                  1u
#define IG_GTSCAN_PLAN_NULL_OUT                            2u
#define IG_GTSCAN_PLAN_BAD_BASE                            3u
#define IG_GTSCAN_PLAN_BAD_EXTENT                          4u
#define IG_GTSCAN_PLAN_TOO_LARGE                           5u
#define IG_GTSCAN_PLAN_OUTSIDE_GGTT                        6u
#define IG_GTSCAN_PLAN_OVERLAPS_OWNED                      7u

#define IG_GTSCAN_PTE_NOT_ATTEMPTED                        0u
#define IG_GTSCAN_PTE_OK                                   1u
#define IG_GTSCAN_PTE_NULL_INPUT                           2u
#define IG_GTSCAN_PTE_BAD_COUNT                            3u
#define IG_GTSCAN_PTE_OUTPUT_TOO_SMALL                     4u
#define IG_GTSCAN_PTE_BAD_PAT                              5u
#define IG_GTSCAN_PTE_NOT_PRESENT                          6u
#define IG_GTSCAN_PTE_LOCAL_MEMORY                         7u
#define IG_GTSCAN_PTE_BAD_ADDRESS                          8u

#define IG_GTSCAN_FILL_NOT_ATTEMPTED                       0u
#define IG_GTSCAN_FILL_OK                                  1u
#define IG_GTSCAN_FILL_NULL_INPUT                          2u
#define IG_GTSCAN_FILL_BAD_PLAN                            3u
#define IG_GTSCAN_FILL_BAD_FORMAT                          4u
#define IG_GTSCAN_FILL_BAD_PITCH                           5u
#define IG_GTSCAN_FILL_BAD_SIZE                            6u
#define IG_GTSCAN_FILL_OUTSIDE_PLANE                       7u
#define IG_GTSCAN_FILL_OUTSIDE_WINDOW                      8u

struct IGGtScanoutPlan {
    IGGtScanU32 firstGgttIndex; // GGTT PTE index of the first scanout page
    IGGtScanU32 pageCount;      // PTEs read and private leaves written
    IGGtScanU32 ptCount;        // fresh page tables, one per 512 pages
    IGGtScanU32 firstPdIndex;   // pd[firstPdIndex .. firstPdIndex + ptCount)
    IGGtScanU32 gpuBase;        // private-PPGTT address of PLANE_SURF's page
    IGGtScanU32 mappedBytes;    // pageCount * 4096
    IGGtScanU64 extentBytes;    // the igPresentExtentBytes() answer
};

struct IGGtScanoutFill {
    IGGtScanU32 gpuAddress;     // private-PPGTT address of pixel (x, y)
    IGGtScanU32 bytes;          // gpuAddress .. end of the borrowed window
    IGGtScanU32 pitch;
    IGGtScanU32 width;
    IGGtScanU32 height;
};

static inline bool igGtScanoutPlanSucceeded(IGGtScanU32 verdict)
{
    return verdict == IG_GTSCAN_PLAN_OK;
}

static inline bool igGtScanoutPteSucceeded(IGGtScanU32 verdict)
{
    return verdict == IG_GTSCAN_PTE_OK;
}

static inline bool igGtScanoutFillSucceeded(IGGtScanU32 verdict)
{
    return verdict == IG_GTSCAN_FILL_OK;
}

// baseOffset and extentBytes are igPresentBaseOffset()/igPresentExtentBytes()
// for a group igPresentVerdict() accepted. ggttEntries is installGGTT()'s
// fGGTTEntries; owned* is the pool run it reserved (fGGTTBase / 4096, PoolPages).
// The caller's output is untouched on every refusal.
static inline IGGtScanU32 igGtScanoutPlanMap(IGGtScanU32 baseOffset,
                                             IGGtScanU64 extentBytes,
                                             IGGtScanU32 ggttEntries,
                                             IGGtScanU32 ownedFirstIndex,
                                             IGGtScanU32 ownedCount,
                                             IGGtScanoutPlan *out)
{
    if (out == 0) return IG_GTSCAN_PLAN_NULL_OUT;
    if ((baseOffset & (IG_GTSCAN_PAGE_BYTES_POLICY_4096 - 1u)) != 0u)
        return IG_GTSCAN_PLAN_BAD_BASE;
    if (extentBytes == 0ull) return IG_GTSCAN_PLAN_BAD_EXTENT;
    if (extentBytes > (IGGtScanU64)IG_GTSCAN_WINDOW_BYTES_0x4000000)
        return IG_GTSCAN_PLAN_TOO_LARGE;

    const IGGtScanU64 page = IG_GTSCAN_PAGE_BYTES_POLICY_4096;
    const IGGtScanU64 pages = (extentBytes + page - 1ull) / page;
    const IGGtScanU64 pts = (pages + IG_GTSCAN_PT_LEAVES_GEN8_PLUS_512 - 1ull) /
                            IG_GTSCAN_PT_LEAVES_GEN8_PLUS_512;
    if (pages == 0ull || pages > IG_GTSCAN_MAX_PAGES_POLICY_16384 ||
        pts == 0ull || pts > IG_GTSCAN_MAX_PTS_POLICY_32)
        return IG_GTSCAN_PLAN_TOO_LARGE;

    const IGGtScanU64 first = (IGGtScanU64)baseOffset / page;
    if (ggttEntries == 0u || first + pages > (IGGtScanU64)ggttEntries)
        return IG_GTSCAN_PLAN_OUTSIDE_GGTT;
    if (ownedCount != 0u &&
        first < (IGGtScanU64)ownedFirstIndex + ownedCount &&
        (IGGtScanU64)ownedFirstIndex < first + pages)
        return IG_GTSCAN_PLAN_OVERLAPS_OWNED;

    IGGtScanoutPlan plan = {};
    plan.firstGgttIndex = (IGGtScanU32)first;
    plan.pageCount = (IGGtScanU32)pages;
    plan.ptCount = (IGGtScanU32)pts;
    plan.firstPdIndex = IG_GTSCAN_FIRST_PD_INDEX_POLICY_1;
    plan.gpuBase = IG_GTSCAN_GPU_BASE_0x200000;
    plan.mappedBytes = (IGGtScanU32)(pages * page);
    plan.extentBytes = extentBytes;
    *out = plan;
    return IG_GTSCAN_PLAN_OK;
}

// Decode one GGTT PTE read back from the table installGGTT() already scans.
// The caller's output is untouched on every refusal.
static inline IGGtScanU32 igGtScanoutGgttPage(IGGtScanU64 pte, IGGtScanU64 *outAddress)
{
    if (outAddress == 0) return IG_GTSCAN_PTE_NULL_INPUT;
    if ((pte & IG_GTSCAN_GGTT_PRESENT_I915_0x1) == 0ull)
        return IG_GTSCAN_PTE_NOT_PRESENT;
    if ((pte & IG_GTSCAN_GGTT_LM_I915_0x2) != 0ull)
        return IG_GTSCAN_PTE_LOCAL_MEMORY;
    const IGGtScanU64 address = pte & IG_GTSCAN_GGTT_ADDR_MASK_I915_45_12;
    // A present PTE naming address 0 describes no surface this driver would
    // write, so it is refused rather than mapped.
    if (address == 0ull || !igGtPpgttPageAddressOk(address))
        return IG_GTSCAN_PTE_BAD_ADDRESS;
    *outAddress = address;
    return IG_GTSCAN_PTE_OK;
}

// Convert the GGTT PTEs read for one plan into private leaves. Leaf i lands at
// flat index i of the caller's contiguous page-table storage, i.e. table
// i / 512, slot i % 512. Every input is validated before output changes.
static inline IGGtScanU32 igGtScanoutBuildLeaves(const IGGtScanU64 *ggttPtes,
                                                 IGGtScanU32 pageCount,
                                                 IGGtScanU32 patIndex,
                                                 IGGtScanU64 *outLeaves,
                                                 IGGtScanU32 outputCapacity)
{
    if (ggttPtes == 0 || outLeaves == 0) return IG_GTSCAN_PTE_NULL_INPUT;
    if (pageCount == 0u || pageCount > IG_GTSCAN_MAX_PAGES_POLICY_16384)
        return IG_GTSCAN_PTE_BAD_COUNT;
    if (outputCapacity < pageCount) return IG_GTSCAN_PTE_OUTPUT_TOO_SMALL;
    if (!igGtPpgttPatIndexOk(patIndex)) return IG_GTSCAN_PTE_BAD_PAT;

    for (IGGtScanU32 i = 0u; i < pageCount; ++i) {
        IGGtScanU64 address = 0ull;
        const IGGtScanU32 verdict = igGtScanoutGgttPage(ggttPtes[i], &address);
        if (!igGtScanoutPteSucceeded(verdict)) return verdict;
        if (igGtPpgttLeafEntry(address, patIndex) == IG_GTPG_ENTRY_INVALID)
            return IG_GTSCAN_PTE_BAD_ADDRESS;
    }
    for (IGGtScanU32 i = 0u; i < pageCount; ++i) {
        IGGtScanU64 address = 0ull;
        (void)igGtScanoutGgttPage(ggttPtes[i], &address);
        outLeaves[i] = igGtPpgttLeafEntry(address, patIndex);
    }
    return IG_GTSCAN_PTE_OK;
}

// PD entries for plan->ptCount physically contiguous page tables starting at
// ptBusBase. outEntries[k] belongs in pd[plan->firstPdIndex + k].
static inline IGGtScanU32 igGtScanoutBuildPdEntries(const IGGtScanoutPlan *plan,
                                                    IGGtScanU64 ptBusBase,
                                                    IGGtScanU64 *outEntries,
                                                    IGGtScanU32 outputCapacity)
{
    if (plan == 0 || outEntries == 0) return IG_GTSCAN_PTE_NULL_INPUT;
    if (plan->ptCount == 0u || plan->ptCount > IG_GTSCAN_MAX_PTS_POLICY_32 ||
        plan->firstPdIndex != IG_GTSCAN_FIRST_PD_INDEX_POLICY_1)
        return IG_GTSCAN_PTE_BAD_COUNT;
    if (outputCapacity < plan->ptCount) return IG_GTSCAN_PTE_OUTPUT_TOO_SMALL;

    const IGGtScanU64 page = IG_GTSCAN_PAGE_BYTES_POLICY_4096;
    const IGGtScanU64 last = ptBusBase + (IGGtScanU64)(plan->ptCount - 1u) * page;
    if (ptBusBase == 0ull || last < ptBusBase ||
        !igGtPpgttPageAddressOk(ptBusBase) || !igGtPpgttPageAddressOk(last))
        return IG_GTSCAN_PTE_BAD_ADDRESS;
    for (IGGtScanU32 k = 0u; k < plan->ptCount; ++k)
        outEntries[k] = igGtPpgttTableEntry(ptBusBase + (IGGtScanU64)k * page);
    return IG_GTSCAN_PTE_OK;
}

// One rectangle of the visible plane as the destination gtEncodeFill32()
// accepts. planeWidth, planeHeight and strideBytes are igPresentWidth(),
// igPresentHeight() and igPresentBytesPerRow(); bytesPerPixel is
// igPresentBitsPerPixel() / 8, all for the group the plan came from.
static inline IGGtScanU32 igGtScanoutFillRect32(const IGGtScanoutPlan *plan,
                                                IGGtScanU32 planeWidth,
                                                IGGtScanU32 planeHeight,
                                                IGGtScanU32 strideBytes,
                                                IGGtScanU32 bytesPerPixel,
                                                IGGtScanU32 x,
                                                IGGtScanU32 y,
                                                IGGtScanU32 width,
                                                IGGtScanU32 height,
                                                IGGtScanoutFill *out)
{
    if (plan == 0 || out == 0) return IG_GTSCAN_FILL_NULL_INPUT;
    if (plan->pageCount == 0u || plan->pageCount > IG_GTSCAN_MAX_PAGES_POLICY_16384 ||
        plan->gpuBase != IG_GTSCAN_GPU_BASE_0x200000 ||
        plan->mappedBytes != plan->pageCount * IG_GTSCAN_PAGE_BYTES_POLICY_4096 ||
        plan->extentBytes == 0ull || plan->extentBytes > plan->mappedBytes)
        return IG_GTSCAN_FILL_BAD_PLAN;
    if (bytesPerPixel != IG_GTSCAN_FILL_BYTES_PER_PIXEL_4)
        return IG_GTSCAN_FILL_BAD_FORMAT;
    if (strideBytes == 0u || strideBytes > IG_GTSCAN_FILL_MAX_PITCH_32767 ||
        (strideBytes & 3u) != 0u ||
        (IGGtScanU64)planeWidth * bytesPerPixel > strideBytes)
        return IG_GTSCAN_FILL_BAD_PITCH;
    if (width == 0u || height == 0u ||
        width > IG_GTSCAN_FILL_MAX_EXTENT_16384 ||
        height > IG_GTSCAN_FILL_MAX_EXTENT_16384)
        return IG_GTSCAN_FILL_BAD_SIZE;
    if (planeWidth == 0u || planeHeight == 0u ||
        (IGGtScanU64)x + width > planeWidth ||
        (IGGtScanU64)y + height > planeHeight)
        return IG_GTSCAN_FILL_OUTSIDE_PLANE;

    const IGGtScanU64 visible = (IGGtScanU64)(planeHeight - 1u) * strideBytes +
                                (IGGtScanU64)planeWidth * bytesPerPixel;
    if (visible > plan->extentBytes) return IG_GTSCAN_FILL_BAD_PLAN;

    const IGGtScanU64 offset = (IGGtScanU64)y * strideBytes +
                               (IGGtScanU64)x * bytesPerPixel;
    const IGGtScanU64 used = (IGGtScanU64)(height - 1u) * strideBytes +
                             (IGGtScanU64)width * bytesPerPixel;
    if (offset + used > plan->extentBytes)
        return IG_GTSCAN_FILL_OUTSIDE_WINDOW;

    IGGtScanoutFill fill = {};
    fill.gpuAddress = plan->gpuBase + (IGGtScanU32)offset;
    fill.bytes = plan->mappedBytes - (IGGtScanU32)offset;
    fill.pitch = strideBytes;
    fill.width = width;
    fill.height = height;
    *out = fill;
    return IG_GTSCAN_FILL_OK;
}

#endif // IntelGtScanoutMap_hpp
