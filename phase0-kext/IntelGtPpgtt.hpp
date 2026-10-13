//
// IntelGtPpgtt.hpp
// Pure Tiger Lake PPGTT + logical-context helpers used by IntelGtBringup.
//
// This file deliberately contains no IOKit types, allocation, MMIO or sleeps.
// The macOS shell owns those.  This layer owns the bits that are easy to get
// subtly wrong while still producing a perfectly well-formed GPU address or
// context image, so the exact same code can run in host tests on Linux/macOS.
//
#ifndef IntelGtPpgtt_hpp
#define IntelGtPpgtt_hpp

#include "IntelGtContext.hpp"

// ---------------------------------------------------------------------------
// Four-level PPGTT geometry.
//
// reference/i915/intel_gtt.h documents the Gen8+ 48-bit layout as:
//   47:39 PML4 | 38:30 PDP | 29:21 PD | 20:12 PT | 11:0 offset
// Tiger Lake uses the same four-level form for the legacy64 context mode.
// ---------------------------------------------------------------------------
#define IG_GTPG_INDEX_MASK_I915_0x1FF          0x1FFu
#define IG_GTPG_PML4_SHIFT_I915_39             39u
#define IG_GTPG_PDP_SHIFT_I915_30              30u
#define IG_GTPG_PD_SHIFT_I915_21               21u
#define IG_GTPG_PT_SHIFT_I915_12               12u
#define IG_GTPG_PAGE_OFFSET_MASK_I915_0xFFF    0xFFFu

static inline IGU32 igGtPpgttPml4Index(IGU64 va)
{
    return (IGU32)((va >> IG_GTPG_PML4_SHIFT_I915_39) & IG_GTPG_INDEX_MASK_I915_0x1FF);
}
static inline IGU32 igGtPpgttPdpIndex(IGU64 va)
{
    return (IGU32)((va >> IG_GTPG_PDP_SHIFT_I915_30) & IG_GTPG_INDEX_MASK_I915_0x1FF);
}
static inline IGU32 igGtPpgttPdIndex(IGU64 va)
{
    return (IGU32)((va >> IG_GTPG_PD_SHIFT_I915_21) & IG_GTPG_INDEX_MASK_I915_0x1FF);
}
static inline IGU32 igGtPpgttPtIndex(IGU64 va)
{
    return (IGU32)((va >> IG_GTPG_PT_SHIFT_I915_12) & IG_GTPG_INDEX_MASK_I915_0x1FF);
}
static inline IGU32 igGtPpgttPageOffset(IGU64 va)
{
    return (IGU32)(va & IG_GTPG_PAGE_OFFSET_MASK_I915_0xFFF);
}

// ---------------------------------------------------------------------------
// Gen12 PPGTT entries.
//
// reference/i915/intel_gtt.h:
//   GEN8_PAGE_PRESENT       bit 0
//   GEN8_PAGE_RW            bit 1
//   GEN12_PPGTT_PTE_PAT0    bit 3
//   GEN12_PPGTT_PTE_PAT1    bit 4
//   GEN12_PPGTT_PTE_PAT2    bit 7
//
// IntelGtBringup deliberately asks IODMACommand for a 46-bit mapping.  The
// address ceiling below is therefore a DRIVER POLICY that matches that DMA
// contract, not a claim that every Gen12 PPGTT implementation has this limit.
// ---------------------------------------------------------------------------
#define IG_GTPG_ENTRY_PRESENT_I915_0x1         0x1ull
#define IG_GTPG_ENTRY_RW_I915_0x2              0x2ull
#define IG_GTPG_PAT0_I915_0x8                  0x8ull
#define IG_GTPG_PAT1_I915_0x10                 0x10ull
#define IG_GTPG_PAT2_I915_0x80                 0x80ull
#define IG_GTPG_PAT_INDEX_COUNT_I915_8         8u
#define IG_GTPG_DMA_ADDR_MASK_POLICY_46        0x00003FFFFFFFF000ull
#define IG_GTPG_ENTRY_INVALID                  0ull
#define IG_GTPG_PAT_INVALID                    0xFFFFFFFFFFFFFFFFull

static inline bool igGtPpgttPageAddressOk(IGU64 address)
{
    if ((address & (IGU64)IG_GTPG_PAGE_OFFSET_MASK_I915_0xFFF) != 0ull) {
        return false;
    }
    return address <= IG_GTPG_DMA_ADDR_MASK_POLICY_46;
}

static inline bool igGtPpgttPatIndexOk(IGU32 patIndex)
{
    return patIndex < IG_GTPG_PAT_INDEX_COUNT_I915_8;
}

static inline IGU64 igGtPpgttPatBits(IGU32 patIndex)
{
    if (!igGtPpgttPatIndexOk(patIndex)) {
        return IG_GTPG_PAT_INVALID;
    }
    IGU64 bits = 0ull;
    if ((patIndex & 1u) != 0u) { bits |= IG_GTPG_PAT0_I915_0x8; }
    if ((patIndex & 2u) != 0u) { bits |= IG_GTPG_PAT1_I915_0x10; }
    if ((patIndex & 4u) != 0u) { bits |= IG_GTPG_PAT2_I915_0x80; }
    return bits;
}

static inline IGU64 igGtPpgttTableEntry(IGU64 childPageAddress)
{
    if (!igGtPpgttPageAddressOk(childPageAddress)) {
        return IG_GTPG_ENTRY_INVALID;
    }
    return childPageAddress | IG_GTPG_ENTRY_PRESENT_I915_0x1 |
           IG_GTPG_ENTRY_RW_I915_0x2;
}

static inline IGU64 igGtPpgttLeafEntry(IGU64 pageAddress, IGU32 patIndex)
{
    if (!igGtPpgttPageAddressOk(pageAddress)) {
        return IG_GTPG_ENTRY_INVALID;
    }
    IGU64 pat = igGtPpgttPatBits(patIndex);
    if (pat == IG_GTPG_PAT_INVALID) {
        return IG_GTPG_ENTRY_INVALID;
    }
    return pageAddress | IG_GTPG_ENTRY_PRESENT_I915_0x1 |
           IG_GTPG_ENTRY_RW_I915_0x2 | pat;
}

// ---------------------------------------------------------------------------
// Gen12 logical-context PPGTT block.
//
// reference/i915/intel_lrc.c::gen12_xcs_offsets and gen12_rcs_offsets have the
// same second block: five NOP dwords, a posted 9-register LRI, then offsets
// 0x3a8, 0x28c, 0x288, 0x284, 0x280, 0x27c, 0x278, 0x274, 0x270.
// reference/i915/intel_lrc_reg.h fixes the VALUE slots for PDP3..PDP0 at
// 0x25,0x27,0x29,0x2b,0x2d,0x2f,0x31,0x33 respectively.
//
// For a four-level VM, Linux's init_ppgtt_regs() writes only PDP0 with the PML4
// DMA address; PDP1..3 are ignored.  For legacy32 it writes all four PDP roots.
// This writer mirrors that distinction and refuses before touching caller
// memory if any input is invalid.
// ---------------------------------------------------------------------------
#define IG_GTPG_LRC_NOP_FIRST_SLOT_I915_0x1C   0x1Cu
#define IG_GTPG_LRC_NOP_COUNT_I915_5           5u
#define IG_GTPG_LRC_LRI_SLOT_I915_0x21         0x21u
#define IG_GTPG_LRC_FIRST_PAIR_SLOT_I915_0x22  0x22u
#define IG_GTPG_LRC_PAIR_COUNT_I915_9          9u
#define IG_GTPG_LRC_REQUIRED_DWORDS_I915_0x34  0x34u
#define IG_GTPG_LRC_BLOCK_DWORDS_I915_24       24u

#define IG_GTPG_PDP3_UDW_SLOT_I915_0x25        0x25u
#define IG_GTPG_PDP3_LDW_SLOT_I915_0x27        0x27u
#define IG_GTPG_PDP2_UDW_SLOT_I915_0x29        0x29u
#define IG_GTPG_PDP2_LDW_SLOT_I915_0x2B        0x2Bu
#define IG_GTPG_PDP1_UDW_SLOT_I915_0x2D        0x2Du
#define IG_GTPG_PDP1_LDW_SLOT_I915_0x2F        0x2Fu
#define IG_GTPG_PDP0_UDW_SLOT_I915_0x31        0x31u
#define IG_GTPG_PDP0_LDW_SLOT_I915_0x33        0x33u

// intel_gpu_commands.h: MI_LRI_LRM_CS_MMIO bit 19, MI_LRI_FORCE_POSTED bit 12.
#define IG_GTPG_LRI_LRM_CS_MMIO_I915_0x80000   0x00080000u
#define IG_GTPG_LRI_FORCE_POSTED_I915_0x1000   0x00001000u

#define IG_GTPG_ROOT_NOT_ATTEMPTED              0u
#define IG_GTPG_ROOT_OK                         1u
#define IG_GTPG_ROOT_NULL_SPAN                  2u
#define IG_GTPG_ROOT_SPAN_TOO_SHORT             3u
#define IG_GTPG_ROOT_BAD_ENGINE                 4u
#define IG_GTPG_ROOT_BAD_MODE                   5u
#define IG_GTPG_ROOT_BAD_COUNT                  6u
#define IG_GTPG_ROOT_BAD_ADDRESS                7u
#define IG_GTPG_ROOT_LIMIT                      7u

static inline bool igGtPpgttRootSucceeded(IGU32 verdict)
{
    return verdict == IG_GTPG_ROOT_OK;
}

static inline const char *igGtPpgttRootVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_GTPG_ROOT_NOT_ATTEMPTED:  return "not attempted";
    case IG_GTPG_ROOT_OK:             return "ok";
    case IG_GTPG_ROOT_NULL_SPAN:      return "null context span";
    case IG_GTPG_ROOT_SPAN_TOO_SHORT: return "context span too short";
    case IG_GTPG_ROOT_BAD_ENGINE:     return "invalid engine";
    case IG_GTPG_ROOT_BAD_MODE:       return "unsupported context address mode";
    case IG_GTPG_ROOT_BAD_COUNT:      return "wrong page-table root count";
    case IG_GTPG_ROOT_BAD_ADDRESS:    return "invalid page-table root address";
    default:                          return "unknown ppgtt root verdict";
    }
}

static inline IGU32 igGtPpgttSecondBlockRegOffset(IGU32 pair)
{
    switch (pair) {
    case 0u: return 0x3A8u;
    case 1u: return 0x28Cu;
    case 2u: return 0x288u;
    case 3u: return 0x284u;
    case 4u: return 0x280u;
    case 5u: return 0x27Cu;
    case 6u: return 0x278u;
    case 7u: return 0x274u;
    case 8u: return 0x270u;
    default: return IG_GTC_OFFSET_INVALID;
    }
}

static inline IGU32 igGtPpgttLriHeader(void)
{
    return igGtcLriHeader(IG_GTPG_LRC_PAIR_COUNT_I915_9) |
           IG_GTPG_LRI_LRM_CS_MMIO_I915_0x80000 |
           IG_GTPG_LRI_FORCE_POSTED_I915_0x1000;
}

static inline void igGtPpgttStoreRoot(IGU32 *dwords,
                                      IGU32 udwSlot,
                                      IGU32 ldwSlot,
                                      IGU64 address)
{
    dwords[udwSlot] = (IGU32)(address >> 32);
    dwords[ldwSlot] = (IGU32)(address & 0xFFFFFFFFull);
}

// roots[] is PDP0..PDP3 for legacy32 and a single PML4 address for legacy64.
static inline IGU32 igGtPpgttWriteLrcRootBlock(IGU32 *dwords,
                                               IGU32 dwordCount,
                                               IGU32 engine,
                                               IGU32 addressMode,
                                               const IGU64 *roots,
                                               IGU32 rootCount,
                                               IGU32 *outDwordsWritten)
{
    if (outDwordsWritten != 0) { *outDwordsWritten = 0u; }
    if (dwords == 0 || roots == 0) { return IG_GTPG_ROOT_NULL_SPAN; }
    if (dwordCount < IG_GTPG_LRC_REQUIRED_DWORDS_I915_0x34) {
        return IG_GTPG_ROOT_SPAN_TOO_SHORT;
    }

    IGU32 base = igGtEngineBase(engine);
    if (base == IG_GT_BASE_INVALID) { return IG_GTPG_ROOT_BAD_ENGINE; }
    if (!igGtxContextAddressModeSupported(addressMode)) {
        return IG_GTPG_ROOT_BAD_MODE;
    }

    IGU32 wantedRoots = (addressMode == IG_GTX_CTXDESC_LEGACY64_I915_3) ? 1u : 4u;
    if (rootCount != wantedRoots) { return IG_GTPG_ROOT_BAD_COUNT; }
    for (IGU32 i = 0; i < rootCount; ++i) {
        if (!igGtPpgttPageAddressOk(roots[i])) { return IG_GTPG_ROOT_BAD_ADDRESS; }
    }

    // Validation is complete. Nothing above this point writes caller memory.
    for (IGU32 i = 0; i < IG_GTPG_LRC_NOP_COUNT_I915_5; ++i) {
        dwords[IG_GTPG_LRC_NOP_FIRST_SLOT_I915_0x1C + i] = igGtMiNoop();
    }
    dwords[IG_GTPG_LRC_LRI_SLOT_I915_0x21] = igGtPpgttLriHeader();

    for (IGU32 pair = 0; pair < IG_GTPG_LRC_PAIR_COUNT_I915_9; ++pair) {
        IGU32 offsetSlot = IG_GTPG_LRC_FIRST_PAIR_SLOT_I915_0x22 + pair * 2u;
        dwords[offsetSlot] = base + igGtPpgttSecondBlockRegOffset(pair);
        dwords[offsetSlot + 1u] = 0u;
    }

    if (addressMode == IG_GTX_CTXDESC_LEGACY64_I915_3) {
        // Linux ASSIGN_CTX_PML4 stores the PML4 DMA address in PDP0 only.
        igGtPpgttStoreRoot(dwords,
                           IG_GTPG_PDP0_UDW_SLOT_I915_0x31,
                           IG_GTPG_PDP0_LDW_SLOT_I915_0x33,
                           roots[0]);
    } else {
        // Linux ASSIGN_CTX_PDP(ppgtt, regs, n), n = 3..0.
        igGtPpgttStoreRoot(dwords, IG_GTPG_PDP0_UDW_SLOT_I915_0x31,
                           IG_GTPG_PDP0_LDW_SLOT_I915_0x33, roots[0]);
        igGtPpgttStoreRoot(dwords, IG_GTPG_PDP1_UDW_SLOT_I915_0x2D,
                           IG_GTPG_PDP1_LDW_SLOT_I915_0x2F, roots[1]);
        igGtPpgttStoreRoot(dwords, IG_GTPG_PDP2_UDW_SLOT_I915_0x29,
                           IG_GTPG_PDP2_LDW_SLOT_I915_0x2B, roots[2]);
        igGtPpgttStoreRoot(dwords, IG_GTPG_PDP3_UDW_SLOT_I915_0x25,
                           IG_GTPG_PDP3_LDW_SLOT_I915_0x27, roots[3]);
    }

    if (outDwordsWritten != 0) {
        *outDwordsWritten = IG_GTPG_LRC_BLOCK_DWORDS_I915_24;
    }
    return IG_GTPG_ROOT_OK;
}

#endif // IntelGtPpgtt_hpp
