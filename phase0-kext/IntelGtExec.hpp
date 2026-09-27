//
//  IntelGtExec.hpp
//  Gen12 (Tiger Lake) EXECLIST submission and GTT address composition -- the
//  layer directly above IntelGtCmd.hpp. That header describes a ring; this one
//  describes how a ring is handed to the hardware scheduler and how the pages
//  it points at are made visible to the engine.
//
//  DEPENDENCY-FREE, same rule as every other header in phase0-kext/. The one
//  #include is a quoted sibling (IntelGtCmd.hpp) for the engine-base switch and
//  the IGU32/IGU64 widths -- the same shape IntelDisplayNames.hpp already uses,
//  and still standalone-parseable under -I phase0-kext. Duplicating the engine
//  switch here instead would be the copy-paste hazard this project has already
//  paid for twice.
//
//  ------------------------------------------------------------------------
//  EVIDENCE: EVERY CONSTANT BELOW IS POLICY. NOT ONE IS CITABLE.
//  ------------------------------------------------------------------------
//
//  This is not a hedge, it is a measurement. reference/i915/ holds SEVEN files
//  and all seven are display: i915_reg.h, i915_reg_defs.h, i915_pciids.h,
//  intel_de.h, intel_display_reg_defs.h, intel_display_power.h and
//  skl_universal_plane_regs.h. Greps for EXECLIST, ELSP, LRC, CTX_DESC,
//  GEN8_PDE, GEN8_PTE, PPAT and GGTT across that whole directory return ZERO
//  matches (measured 2026-08-26). i915 keeps the execlist port, the context
//  descriptor layout and the page-table entry format in gt/ headers that were
//  never vendored into this checkout.
//
//  So unlike IntelGtCmd.hpp -- where the four engine bases and the ring block
//  ARE cited to i915_reg.h and only the MI opcodes are policy -- this header
//  has no cited half at all. Every offset, every shift and every mask is from
//  public Gen architecture documentation, and any of them may be wrong on
//  silicon. They are spelled *_POLICY_* without exception, and no *_OBSERVED_*
//  constant may be added: igpu-results/ captured a machine whose render engine
//  was idle, so there is no MMIO evidence of a running execlist port anywhere
//  in this tree.
//
//  What a green test suite over this header establishes is that the
//  COMPOSITION is self-consistent and that every refusal is reachable. It does
//  not establish that the hardware wants these words.
//
//  IS NOT ACCELERATION. Metal needs an IOAccelerator family subclass plus a
//  userspace plugin against private, unversioned Apple interfaces; PLAN.md
//  section 0 is the wall. Submitting a context is a prerequisite for running
//  shader work, not the thing itself, and nothing here has executed anywhere.
//

#ifndef IntelGtExec_hpp
#define IntelGtExec_hpp

#include "IntelGtCmd.hpp"

// ---------------------------------------------------------------------------
// Execlist port registers, as offsets from an engine base.
//
// POLICY. Composed through igGtRingReg() so an invalid engine selector refuses
// rather than producing a small plausible offset -- IG_GT_BASE_INVALID + 0x510
// wraps to 0x50F, which is a legal-looking register address.
// ---------------------------------------------------------------------------

#define IG_GTX_EXECLIST_STATUS_POLICY_0x234     0x234u
#define IG_GTX_EXECLIST_SQ_POLICY_0x510         0x510u
#define IG_GTX_EXECLIST_CONTROL_POLICY_0x550    0x550u

// EXECLIST_CONTROL bit 0 is the load bit: writing it tells the scheduler the
// submit queue has been filled. POLICY.
#define IG_GTX_EXECLIST_LOAD_BIT_POLICY_0x1     0x1u

// EXECLIST_STATUS bit 0 clear means the submit queue is free to be written.
// POLICY, and the polarity is the dangerous half: reading it backwards means
// overwriting a queue the scheduler is still consuming.
#define IG_GTX_EXECLIST_BUSY_BIT_POLICY_0x1     0x1u

static inline IGU32 igGtxExeclistStatusReg(IGU32 engine)
{
    return igGtRingReg(engine, IG_GTX_EXECLIST_STATUS_POLICY_0x234);
}

static inline IGU32 igGtxExeclistSqReg(IGU32 engine)
{
    return igGtRingReg(engine, IG_GTX_EXECLIST_SQ_POLICY_0x510);
}

static inline IGU32 igGtxExeclistControlReg(IGU32 engine)
{
    return igGtRingReg(engine, IG_GTX_EXECLIST_CONTROL_POLICY_0x550);
}

// An EQUALITY against the clear state, never a negation. A refused MMIO read
// answers 0xFFFFFFFF, which has the busy bit SET, so a refused read reports
// BUSY -- the safe direction, because busy means "do not write the queue".
static inline bool igGtxExeclistPortBusy(IGU32 statusWord)
{
    return (statusWord & IG_GTX_EXECLIST_BUSY_BIT_POLICY_0x1) != 0u;
}

// ---------------------------------------------------------------------------
// Context descriptor.
//
// POLICY. A 64-bit word naming the logical ring context the engine should run:
// a valid bit, the 4 KiB-aligned context address (LRCA) sitting at its natural
// position in bits 31:12, and a software context id above bit 32.
// ---------------------------------------------------------------------------

#define IG_GTX_CTXDESC_VALID_BIT_POLICY_0x1     0x1u
#define IG_GTX_CTXDESC_LRCA_MASK_POLICY         0xFFFFF000u
#define IG_GTX_CTXDESC_SWID_SHIFT_POLICY_32     32u
#define IG_GTX_CTXDESC_SWID_MASK_POLICY_0x7FF   0x7FFu

// A descriptor that could not be composed. ZERO, deliberately, and this is the
// one sentinel in the tree that is NOT all-ones: bit 0 is the valid bit, so a
// zero descriptor is an INVALID descriptor by the hardware's own definition.
// All-ones would set the valid bit over a garbage address, which is the
// scribble direction -- the same argument IntelGtCmd.hpp makes for refusing a
// ring register rather than composing one off a sentinel base.
#define IG_GTX_CTXDESC_INVALID_POLICY_0         0ull

static inline bool igGtxLrcaAligned(IGU64 lrca)
{
    return (lrca & (IGU64)(IG_GT_PAGE_POLICY_0x1000 - 1u)) == 0ull;
}

// The LRCA field is 20 bits at bit 12, so it can only describe an address
// below 4 GiB. A caller handing in a higher address must be refused rather
// than truncated: a truncated LRCA points at a different, legal-looking
// context page.
static inline bool igGtxLrcaInRange(IGU64 lrca)
{
    return lrca <= (IGU64)IG_GTX_CTXDESC_LRCA_MASK_POLICY;
}

// Masked composition would silently collide two contexts on one id, and two
// contexts sharing an id is a scheduler fault nobody can trace back to here.
static inline bool igGtxContextIdFits(IGU32 contextId)
{
    return contextId <= IG_GTX_CTXDESC_SWID_MASK_POLICY_0x7FF;
}

static inline IGU64 igGtxContextDescriptor(IGU64 lrca, IGU32 contextId)
{
    // Range BEFORE alignment, matching IntelGtCmd.hpp's tail rule: the
    // alignment of an address the field cannot hold is not a meaningful
    // complaint about it.
    if (!igGtxLrcaInRange(lrca)) {
        return IG_GTX_CTXDESC_INVALID_POLICY_0;
    }
    if (!igGtxLrcaAligned(lrca)) {
        return IG_GTX_CTXDESC_INVALID_POLICY_0;
    }
    if (!igGtxContextIdFits(contextId)) {
        return IG_GTX_CTXDESC_INVALID_POLICY_0;
    }

    IGU64 word = (IGU64)IG_GTX_CTXDESC_VALID_BIT_POLICY_0x1;
    word |= (lrca & (IGU64)IG_GTX_CTXDESC_LRCA_MASK_POLICY);
    word |= ((IGU64)(contextId & IG_GTX_CTXDESC_SWID_MASK_POLICY_0x7FF))
            << IG_GTX_CTXDESC_SWID_SHIFT_POLICY_32;
    return word;
}

static inline bool igGtxContextDescriptorValid(IGU64 descriptor)
{
    return (descriptor & (IGU64)IG_GTX_CTXDESC_VALID_BIT_POLICY_0x1) != 0ull;
}

// The two dwords written to the submit queue, low first. Both answer zero for
// an invalid descriptor rather than splitting garbage, so a caller that
// ignores the validity flag writes an all-zero (invalid) queue entry instead
// of a plausible one.
static inline IGU32 igGtxDescriptorLow(IGU64 descriptor)
{
    if (!igGtxContextDescriptorValid(descriptor)) {
        return 0u;
    }
    return (IGU32)(descriptor & 0xFFFFFFFFull);
}

static inline IGU32 igGtxDescriptorHigh(IGU64 descriptor)
{
    if (!igGtxContextDescriptorValid(descriptor)) {
        return 0u;
    }
    return (IGU32)((descriptor >> 32) & 0xFFFFFFFFull);
}

// ---------------------------------------------------------------------------
// GTT / page-table entries.
//
// POLICY, entirely. A page-table entry is a 64-bit word holding a 4 KiB-aligned
// physical page frame plus a present bit. The engine reads memory THROUGH this
// table, so a wrong entry does not fault -- it reads somebody else's page.
// ---------------------------------------------------------------------------

#define IG_GTX_PTE_PRESENT_BIT_POLICY_0x1       0x1u
#define IG_GTX_PTE_ADDR_MASK_POLICY             0x0000FFFFFFFFF000ull
#define IG_GTX_GTT_ENTRY_BYTES_POLICY_8         8u

// The largest physical address the entry can describe: the mask is 36 bits of
// page frame at bit 12, so the ceiling is the mask itself.
#define IG_GTX_PTE_ADDR_CEILING_POLICY          0x0000FFFFFFFFF000ull

// A refused entry is ZERO, not all-ones, for IG_GTX_CTXDESC_INVALID_POLICY_0's
// reason one level down: bit 0 is the present bit, so zero is NOT PRESENT by
// the hardware's own definition while all-ones is a present entry pointing at
// the top of the address space.
#define IG_GTX_PTE_INVALID_POLICY_0             0ull

// One million entries -- 4 GiB of 4 KiB pages, an 8 MiB table. POLICY, and a
// ceiling rather than a measurement: nothing in this tree has read the real
// GTT size out of GMCH graphics control.
#define IG_GTX_GTT_MAX_ENTRIES_POLICY_0x100000  0x100000u
#define IG_GTX_GTT_OFFSET_INVALID               0xFFFFFFFFu

static inline bool igGtxPageAddressOk(IGU64 physAddr)
{
    if ((physAddr & (IGU64)(IG_GT_PAGE_POLICY_0x1000 - 1u)) != 0ull) {
        return false;
    }
    return physAddr <= IG_GTX_PTE_ADDR_CEILING_POLICY;
}

static inline IGU64 igGtxPageTableEntry(IGU64 physAddr, bool present)
{
    if (!igGtxPageAddressOk(physAddr)) {
        return IG_GTX_PTE_INVALID_POLICY_0;
    }
    if (!present) {
        return IG_GTX_PTE_INVALID_POLICY_0;
    }
    return (physAddr & IG_GTX_PTE_ADDR_MASK_POLICY)
           | (IGU64)IG_GTX_PTE_PRESENT_BIT_POLICY_0x1;
}

static inline bool igGtxPteIsPresent(IGU64 entry)
{
    return (entry & (IGU64)IG_GTX_PTE_PRESENT_BIT_POLICY_0x1) != 0ull;
}

// Answers zero for an absent entry rather than the masked bits of whatever was
// handed in: an address recovered from a non-present entry is a number nobody
// programmed, and it looks exactly like one somebody did.
static inline IGU64 igGtxPteAddress(IGU64 entry)
{
    if (!igGtxPteIsPresent(entry)) {
        return 0ull;
    }
    return entry & IG_GTX_PTE_ADDR_MASK_POLICY;
}

// The index is bounded BEFORE the multiply, never after. index * 8 in 32 bits
// wraps for an index above 0x1FFFFFFF, and a wrapped offset is small, aligned
// and entirely plausible -- the same truncation direction handoff22 section 1b
// records for a BAR length.
static inline IGU32 igGtxGttEntryOffset(IGU32 pageIndex)
{
    if (pageIndex >= IG_GTX_GTT_MAX_ENTRIES_POLICY_0x100000) {
        return IG_GTX_GTT_OFFSET_INVALID;
    }
    return pageIndex * IG_GTX_GTT_ENTRY_BYTES_POLICY_8;
}

// ---------------------------------------------------------------------------
// Submission readiness.
//
// The join: can THIS context be handed to THIS engine's execlist port right
// now. Contiguous family, zero is the caller's initialiser and is never
// returned, and every arm below the limit has a distinct name.
//
// Prefix is IG_GTX_READY_ rather than IG_GTX_ or IG_GTX_EXEC_ on purpose --
// handoff16 section 1 records a verdict family whose prefix swept in a register
// offset and printed a contiguity failure that was correct about its own
// arithmetic and wrong about the world. IG_GTX_EXEC_ would sweep the three
// IG_GTX_EXECLIST_ registers above.
// ---------------------------------------------------------------------------

#define IG_GTX_READY_NOT_ATTEMPTED      0u  // caller's initialiser, never returned
#define IG_GTX_READY_OK                 1u  // admissible
#define IG_GTX_READY_BAD_ENGINE         2u  // selector outside the engine family
#define IG_GTX_READY_LRCA_RANGE         3u  // context address above the LRCA field
#define IG_GTX_READY_LRCA_UNALIGNED     4u  // context address not 4 KiB aligned
#define IG_GTX_READY_CONTEXT_ID         5u  // software id above the 11-bit field
#define IG_GTX_READY_RING_REFUSED       6u  // igGtSubmitVerdict() said no
#define IG_GTX_READY_PORT_BUSY          7u  // scheduler still consuming the queue
#define IG_GTX_READY_LIMIT              8u

// ARM ORDER, and the last two placements are the load-bearing ones.
//
// The ring is judged BEFORE the port, because a ring geometry defect is a bug
// in this driver while a busy port is a fact about the machine one millisecond
// from now -- report the one somebody can go and fix. That is handoff22
// section 1b's ordering rule (an out-of-policy offset outranks a short BAR)
// applied to a transient rather than to a mapping.
//
// LRCA RANGE is judged before LRCA ALIGNMENT, matching igGtSubmitVerdict()'s
// tail rule and igGtxContextDescriptor()'s own order: the alignment of an
// address the field cannot hold is not a meaningful complaint about it.
static inline IGU32 igGtxSubmitReady(IGU32 engine,
                                     IGU64 lrca,
                                     IGU32 contextId,
                                     IGU32 ringBytes,
                                     IGU32 head,
                                     IGU32 tail,
                                     IGU32 lengthBytes,
                                     IGU32 portStatus)
{
    if (igGtEngineBase(engine) == IG_GT_BASE_INVALID) {
        return IG_GTX_READY_BAD_ENGINE;
    }
    if (!igGtxLrcaInRange(lrca)) {
        return IG_GTX_READY_LRCA_RANGE;
    }
    if (!igGtxLrcaAligned(lrca)) {
        return IG_GTX_READY_LRCA_UNALIGNED;
    }
    if (!igGtxContextIdFits(contextId)) {
        return IG_GTX_READY_CONTEXT_ID;
    }
    if (igGtSubmitVerdict(engine, ringBytes, head, tail, lengthBytes)
        != IG_GT_SUBMIT_OK) {
        return IG_GTX_READY_RING_REFUSED;
    }
    if (igGtxExeclistPortBusy(portStatus)) {
        return IG_GTX_READY_PORT_BUSY;
    }
    return IG_GTX_READY_OK;
}

// An EQUALITY against OK, never a negation of one error: a negation is true for
// every OTHER refusal, including ones added after this line was written.
static inline bool igGtxSubmitAdmissible(IGU32 verdict)
{
    return verdict == IG_GTX_READY_OK;
}

static inline const char *igGtxReadyVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GTX_READY_NOT_ATTEMPTED:  return "not attempted";
        case IG_GTX_READY_OK:             return "ok";
        case IG_GTX_READY_BAD_ENGINE:     return "engine selector out of family";
        case IG_GTX_READY_LRCA_RANGE:     return "context address above the LRCA field";
        case IG_GTX_READY_LRCA_UNALIGNED: return "context address not page aligned";
        case IG_GTX_READY_CONTEXT_ID:     return "software context id too large";
        case IG_GTX_READY_RING_REFUSED:   return "ring geometry refused";
        case IG_GTX_READY_PORT_BUSY:      return "execlist port busy";
        default:                          return "unknown submission verdict";
    }
}

#endif // IntelGtExec_hpp
