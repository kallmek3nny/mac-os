//
//  IntelGtContext.hpp
//  Gen12 (Tiger Lake) LOGICAL RING CONTEXT image composition -- the layer
//  directly above IntelGtExec.hpp. That header composes a descriptor NAMING a
//  context address; this one describes what has to be sitting AT that address
//  for the descriptor to mean anything.
//
//  DEPENDENCY-FREE, same rule as every other header in phase0-kext/. The one
//  #include is a quoted sibling (IntelGtExec.hpp) for the IGU32/IGU64 widths,
//  the page constant and the LRCA predicates -- respelling any of those locally
//  would give this tree two answers to one question, which is the defect the
//  sibling-include convention exists to prevent.
//
//  ------------------------------------------------------------------------
//  EVIDENCE: EVERY CONSTANT BELOW IS POLICY. NOT ONE IS CITABLE.
//  ------------------------------------------------------------------------
//
//  Identical measurement to IntelGtExec.hpp's, re-run rather than inherited:
//  reference/i915/ holds seven files and all seven are display. Greps for
//  LRC, CTX_DESC, RING_BUFFER_START, RING_BUFFER_CTL, BB_PER_CTX and
//  CTX_TIMESTAMP across that whole directory return ZERO matches (measured
//  2026-08-26). i915 keeps the LRC register-state layout in gt/ headers that
//  were never vendored here.
//
//  So this header, like its parent, has NO CITED HALF AT ALL. Every dword slot
//  index below is from public Gen architecture documentation and any of them
//  may be wrong on silicon. They are spelled *_POLICY_* without exception, and
//  no *_OBSERVED_* constant may be added: igpu-results/ captured a machine
//  whose render engine was idle, so there is no MMIO evidence of a live context
//  image anywhere in this tree.
//
//  IS NOT ACCELERATION. Metal needs an IOAccelerator family subclass plus a
//  userspace plugin against private, unversioned Apple interfaces; PLAN.md
//  section 0 is the wall. A correctly composed context image is a prerequisite
//  for running shader work, not the thing itself, and nothing here has executed
//  anywhere.
//

#ifndef IntelGtContext_hpp
#define IntelGtContext_hpp

#include "IntelGtExec.hpp"

// ---------------------------------------------------------------------------
// Context image geometry.
//
// POLICY. The image is a run of 4 KiB pages: a per-process hardware status
// page first, then the register-state page the engine restores from, then the
// ring buffer's own pages. The descriptor's LRCA points at the FIRST page, and
// the register state the engine actually reads begins one page in -- an
// off-by-one-page LRCA is the exact hazard this header exists to make
// assertable, because it does not fault. It restores whatever the status page
// happens to hold as though it were register state.
// ---------------------------------------------------------------------------

#define IG_GTC_STATUS_PAGE_BYTES_POLICY_0x1000  0x1000u
#define IG_GTC_STATE_PAGE_BYTES_POLICY_0x1000   0x1000u

// Offset from the LRCA to the register-state page. POLICY, and the single most
// dangerous number in the file for the reason above.
#define IG_GTC_STATE_OFFSET_POLICY_0x1000       0x1000u

// The register-state page opens with a NOOP dword, THEN the load-immediate
// header, then alternating (register offset, value) pairs. The first pair
// therefore begins at dword slot 2, not slot 1 and not slot 0.
//
// CORRECTED 2026-08-26. This file first shipped with the header at slot 0 and
// the first pair at slot 1 -- the layout you get by reasoning about what an LRI
// packet needs rather than by reading what the engine restores. The leading
// NOOP is not decoration: it is part of the image the hardware expects, and
// omitting it slides every subsequent pair down by one dword, so the engine
// reads each register OFFSET out of the previous pair's VALUE slot. That does
// not fault. It programs whatever registers those values happen to name.
#define IG_GTC_NOOP_SLOT_POLICY_0               0u
#define IG_GTC_LRI_HEADER_SLOT_POLICY_1         1u
#define IG_GTC_FIRST_PAIR_SLOT_POLICY_2         2u
#define IG_GTC_SLOT_BYTES_POLICY_4              4u

// A refused byte offset. All-ones here rather than zero, the OPPOSITE of the
// descriptor and PTE sentinels one layer down, and the difference is not an
// inconsistency -- it is the same rule applied to a different kind of answer.
// Zero is a LEGAL slot offset (the leading NOOP lives there), so zero cannot
// mean refusal; in a descriptor word zero is illegal by the hardware's own
// valid bit, so there it can.
#define IG_GTC_OFFSET_INVALID                   0xFFFFFFFFu

// How many (offset, value) pairs the register-state page can hold. POLICY: the
// page is 4 KiB, the LRI header eats one dword, and each pair eats two.
#define IG_GTC_MAX_PAIRS_POLICY_0x1FF           0x1FFu

// ---------------------------------------------------------------------------
// Register-state page addressing.
//
// Every helper below is bounded BEFORE it multiplies. slot * 4 cannot wrap for
// any slot this page can hold, but the bound is written first anyway, because
// the reason a wrap is dangerous does not depend on whether today's constants
// happen to prevent it: a wrapped byte offset is SMALL, 4-byte aligned, and
// inside every containment test there is. The same argument as
// igGtxGttEntryOffset() one layer down.
// ---------------------------------------------------------------------------

static inline IGU32 igGtcSlotCount(void)
{
    return IG_GTC_STATE_PAGE_BYTES_POLICY_0x1000 / IG_GTC_SLOT_BYTES_POLICY_4;
}

static inline bool igGtcSlotInPage(IGU32 slot)
{
    return slot < igGtcSlotCount();
}

// Byte offset of a dword slot from the START OF THE STATE PAGE, not from the
// LRCA. The two differ by IG_GTC_STATE_OFFSET_POLICY_0x1000 and confusing them
// is the off-by-one-page hazard in the header comment, so they are separate
// functions with separate names rather than one function with a flag.
static inline IGU32 igGtcSlotOffset(IGU32 slot)
{
    if (!igGtcSlotInPage(slot)) { return IG_GTC_OFFSET_INVALID; }
    return slot * IG_GTC_SLOT_BYTES_POLICY_4;
}

// Byte offset of a dword slot from the LRCA itself.
static inline IGU32 igGtcSlotOffsetFromLrca(IGU32 slot)
{
    IGU32 inPage = igGtcSlotOffset(slot);
    if (inPage == IG_GTC_OFFSET_INVALID) { return IG_GTC_OFFSET_INVALID; }
    return IG_GTC_STATE_OFFSET_POLICY_0x1000 + inPage;
}

// The address of the register-state page for a context. Refuses for every
// reason the descriptor layer refuses, re-derived through the SAME predicates
// rather than re-tested here -- a second copy of the LRCA rules is a second
// answer to one question.
static inline IGU64 igGtcStatePageAddress(IGU64 lrca)
{
    if (!igGtxLrcaInRange(lrca)) { return 0ull; }
    if (!igGtxLrcaAligned(lrca)) { return 0ull; }
    return lrca + (IGU64)IG_GTC_STATE_OFFSET_POLICY_0x1000;
}

// ---------------------------------------------------------------------------
// The load-immediate header dword.
//
// POLICY. The register-state page opens with an MI_LOAD_REGISTER_IMM whose
// length field says how many (offset, value) pairs follow. The engine restores
// exactly that many pairs, so a header claiming FEWER pairs than the page holds
// leaves the tail slots unrestored -- and unrestored is not a fault, it is
// whatever the engine's registers already contained from the previous context.
// That is the quiet direction and the reason the count is bounded here rather
// than trusted from a caller.
//
// The opcode is composed from IntelGtCmd.hpp's MI family rather than respelled,
// for the same one-answer-to-one-question reason as the includes above.
// ---------------------------------------------------------------------------

// The opcode is defined HERE rather than in IntelGtCmd.hpp because LRI is a
// context-image concern and that header's four opcodes are ring-command ones.
// The length MASK is deliberately NOT respelled -- it is the sibling's
// IG_GT_MI_LENGTH_MASK_POLICY_0xFF, since it is a property of the MI header
// format the sibling owns, not of this page.
#define IG_GTC_MI_OPCODE_LRI_POLICY_0x22        0x22u
#define IG_GTC_LRI_DWORDS_PER_PAIR_POLICY_2     2u

// The MI length field counts dwords BEYOND the header, so a header describing
// N pairs carries 2N - 1. Written as its own constant because the off-by-one
// is the whole content of the field: a header that stored a bare 2N restores
// one dword too many and lands a value into the slot after the last pair.
#define IG_GTC_LRI_LENGTH_EXCESS_POLICY_1       1u
#define IG_GTC_LRI_HEADER_INVALID               0u

static inline bool igGtcPairCountFits(IGU32 pairs)
{
    if (pairs == 0u) { return false; }
    if (pairs > IG_GTC_MAX_PAIRS_POLICY_0x1FF) { return false; }
    // The header's own length field is 8 bits wide, so a pair count the PAGE
    // could hold may still be one the HEADER cannot express. Both bounds are
    // tested, because either alone accepts a count the other refuses.
    if (((pairs * IG_GTC_LRI_DWORDS_PER_PAIR_POLICY_2)
         - IG_GTC_LRI_LENGTH_EXCESS_POLICY_1)
        > IG_GT_MI_LENGTH_MASK_POLICY_0xFF) {
        return false;
    }
    return true;
}

// Refuses to ZERO. Zero is a safe refusal for a header dword specifically
// because the opcode field is non-zero in every LRI header, so no valid header
// can collide with it -- the same argument as the descriptor's valid bit one
// layer down, reached through a different field.
static inline IGU32 igGtcLriHeader(IGU32 pairs)
{
    if (!igGtcPairCountFits(pairs)) { return IG_GTC_LRI_HEADER_INVALID; }
    return igGtMiHeader(IG_GTC_MI_OPCODE_LRI_POLICY_0x22,
                        (pairs * IG_GTC_LRI_DWORDS_PER_PAIR_POLICY_2)
                        - IG_GTC_LRI_LENGTH_EXCESS_POLICY_1);
}

// Recovers the pair count a header claims. Answers zero for anything whose
// opcode is not LRI, so a caller cannot read a pair count out of a NOOP or out
// of the refusal sentinel and act on it.
static inline IGU32 igGtcLriHeaderPairs(IGU32 header)
{
    IGU32 opcode = (header >> IG_GT_MI_OPCODE_SHIFT_POLICY_23)
                   & IG_GT_MI_OPCODE_MASK_POLICY_0x3F;
    if (opcode != IG_GTC_MI_OPCODE_LRI_POLICY_0x22) { return 0u; }

    IGU32 length = header & IG_GT_MI_LENGTH_MASK_POLICY_0xFF;
    IGU32 dwords = length + IG_GTC_LRI_LENGTH_EXCESS_POLICY_1;
    // An ODD dword count cannot be a whole number of pairs, so it describes a
    // header nobody in this tree composed. Refusing is the only honest answer;
    // rounding would report a pair count the hardware will not restore.
    if ((dwords % IG_GTC_LRI_DWORDS_PER_PAIR_POLICY_2) != 0u) { return 0u; }
    return dwords / IG_GTC_LRI_DWORDS_PER_PAIR_POLICY_2;
}

// ---------------------------------------------------------------------------
// The ring-state pairs.
//
// POLICY, every slot index. These four registers are what make a restored
// context point at a ring: where the ring lives, how big it is, and where the
// engine should resume inside it. The engine restores them from FIXED slots in
// the register-state page -- the slot index IS the meaning, so a pair written
// to the wrong slot programs a different register with a perfectly valid value.
// That is why each accessor is separate and each is pinned to a bare literal in
// the suite rather than checked against the #define it reads.
//
// The register OFFSETS are re-derived through IntelGtCmd.hpp's ring-register
// composers rather than respelled, so a ring block that moves in the sibling
// moves here by itself.
// ---------------------------------------------------------------------------

// CORRECTED 2026-08-26, together with the leading NOOP above and for the same
// reason: these four were 0x02/0x04/0x06/0x08, which is what you get by placing
// the LRI header at slot 0. With the NOOP occupying slot 0 and the header at
// slot 1, the first pair's OFFSET dword is slot 2 and its VALUE dword is slot 3,
// so the four ring registers land one pair higher throughout.
//
// These are the OFFSET slots -- the dword that NAMES the register -- and the
// value the engine restores into it sits at slot + 1. Corrected in the same
// breath as the numbers above, because the first version of this comment said
// the opposite while carrying these same four values, and a comment that
// disagrees with its own constants is worse than no comment: it tells the next
// reader the pairs are one dword wide.
//
// igGtcPairValueSlot() below is the only sanctioned way to get from one to the
// other. A hand-written + 1 at a call site is the same off-by-one this whole
// section exists to prevent, just moved somewhere no gate is looking.
#define IG_GTC_SLOT_RING_HEAD_POLICY_0x04       0x04u
#define IG_GTC_SLOT_RING_TAIL_POLICY_0x06       0x06u
#define IG_GTC_SLOT_RING_START_POLICY_0x08      0x08u
#define IG_GTC_SLOT_RING_CTL_POLICY_0x0A        0x0Au

// The value dword for a pair whose offset dword is at `slot`. Refuses with the
// all-ones sentinel when the offset slot is out of the page, when it is below
// the first pair (slots 0 and 1 are the NOOP and the LRI header, so neither
// begins a pair), or when the value would fall off the end of the page -- an
// offset dword in the last slot describes a pair the page cannot hold.
static inline IGU32 igGtcPairValueSlot(IGU32 offsetSlot)
{
    if (!igGtcSlotInPage(offsetSlot)) { return IG_GTC_OFFSET_INVALID; }
    if (offsetSlot < IG_GTC_FIRST_PAIR_SLOT_POLICY_2) { return IG_GTC_OFFSET_INVALID; }
    if (!igGtcSlotInPage(offsetSlot + 1u)) { return IG_GTC_OFFSET_INVALID; }
    return offsetSlot + 1u;
}

// The four ring-state slots are pairwise distinct and all inside the page. A
// copy-paste slot constant satisfies every "has a slot" assertion, so this is
// asserted rather than assumed.
#define IG_GTC_RING_STATE_PAIRS_POLICY_4        4u

// RING_BUFFER_CTL packs the ring length as (pages - 1) in its high bits and
// carries the enable bit at bit 0. The minus-one is the same value-1 convention
// the display side uses for timings, and it has the same hazard: a length
// written WITHOUT the minus-one describes a ring one page longer than the one
// that was allocated, and the engine will happily fetch off the end of it.
#define IG_GTC_RING_CTL_ENABLE_BIT_POLICY_0x1   0x1u
#define IG_GTC_RING_CTL_LEN_SHIFT_POLICY_12     12u
#define IG_GTC_RING_CTL_LEN_MASK_POLICY_0x1FF   0x1FFu
#define IG_GTC_RING_CTL_INVALID                 0u

// The page size is the sibling's ring MINIMUM, which is one page by policy.
// Named locally so the arithmetic below reads as pages rather than as a
// minimum, but DERIVED rather than respelled -- a 4096 typed here would be a
// second answer to the question IntelGtCmd.hpp already answers.
#define IG_GTC_PAGE_BYTES_POLICY (IG_GT_RING_MIN_BYTES_POLICY_0x1000)

static inline IGU32 igGtcRingBufferControl(IGU32 ringBytes)
{
    // Geometry is judged by the SIBLING that owns it. A second copy of the ring
    // rules here would let the two disagree, and the disagreement would be
    // silent -- this composer would program a ring IntelGtCmd.hpp refuses.
    if (igGtRingSizeVerdict(ringBytes) != IG_GT_SUBMIT_OK) {
        return IG_GTC_RING_CTL_INVALID;
    }

    IGU32 pages = ringBytes / IG_GTC_PAGE_BYTES_POLICY;
    if (pages == 0u) { return IG_GTC_RING_CTL_INVALID; }
    if ((pages - 1u) > IG_GTC_RING_CTL_LEN_MASK_POLICY_0x1FF) {
        return IG_GTC_RING_CTL_INVALID;
    }

    IGU32 word = ((pages - 1u) & IG_GTC_RING_CTL_LEN_MASK_POLICY_0x1FF)
                 << IG_GTC_RING_CTL_LEN_SHIFT_POLICY_12;
    word |= IG_GTC_RING_CTL_ENABLE_BIT_POLICY_0x1;
    return word;
}

// Recovers the ring size a control word describes. Answers zero for a word with
// the enable bit CLEAR, because the length field of a disabled ring is not a
// measurement of anything -- reading it back would report a ring size for a ring
// the engine is not using.
static inline IGU32 igGtcRingControlBytes(IGU32 ctlWord)
{
    if ((ctlWord & IG_GTC_RING_CTL_ENABLE_BIT_POLICY_0x1) == 0u) { return 0u; }
    IGU32 pages = ((ctlWord >> IG_GTC_RING_CTL_LEN_SHIFT_POLICY_12)
                   & IG_GTC_RING_CTL_LEN_MASK_POLICY_0x1FF) + 1u;
    return pages * IG_GTC_PAGE_BYTES_POLICY;
}

// ---------------------------------------------------------------------------
// Whole-image verdict.
//
// The join. Everything above answers one question about one field; this answers
// whether an image is fit to be named by a descriptor and handed to the engine.
//
// ZERO IS NOT RETURNED. IG_GTC_IMAGE_NOT_ATTEMPTED is the caller's initialiser,
// so a structure nobody filled in reads as NOT_ATTEMPTED rather than as OK --
// same convention as IG_GT_SUBMIT_* and the NVGSP_* families.
//
// The prefix is IG_GTC_IMAGE_ rather than IG_GTC_, deliberately.
// intel_verdict_set_checks() in tools/check-registers.py groups families BY
// PREFIX, so a family named IG_GTC_ would sweep in every slot index and page
// constant above it and print a contiguity failure that is correct about its
// own arithmetic and wrong about the world -- handoff16 section 1's lesson,
// which cost that session a rename.
// ---------------------------------------------------------------------------

#define IG_GTC_IMAGE_NOT_ATTEMPTED      0u  // caller's initialiser, never returned
#define IG_GTC_IMAGE_OK                 1u  // fit to be named by a descriptor
#define IG_GTC_IMAGE_LRCA_RANGE         2u  // LRCA above the descriptor's field
#define IG_GTC_IMAGE_LRCA_UNALIGNED     3u  // LRCA not on a page boundary
#define IG_GTC_IMAGE_PAIR_COUNT         4u  // pair count the page or header refuses
#define IG_GTC_IMAGE_RING_REFUSED       5u  // ring geometry the sibling refuses
#define IG_GTC_IMAGE_RING_UNMAPPED      6u  // ring base not a usable GTT page
#define IG_GTC_IMAGE_LIMIT              7u

static inline IGU32 igGtcImageVerdict(IGU64 lrca,
                                      IGU32 pairs,
                                      IGU32 ringBytes,
                                      IGU64 ringBase)
{
    // ARM ORDER is load-bearing at every step and each claim is pinned in the
    // suite by an input bad in TWO ways at once. LRCA range before LRCA
    // alignment, because the alignment of an address the descriptor field
    // cannot hold is not a meaningful complaint about it -- the same ordering
    // rule as igGtxSubmitReady() one layer down, applied to the same pair.
    if (!igGtxLrcaInRange(lrca)) { return IG_GTC_IMAGE_LRCA_RANGE; }
    if (!igGtxLrcaAligned(lrca)) { return IG_GTC_IMAGE_LRCA_UNALIGNED; }

    // The IMAGE is judged whole before the ring it would run, because a bad
    // pair count is a defect in the image this driver composed while a ring is
    // a separate allocation -- report the one that is provably ours.
    if (!igGtcPairCountFits(pairs)) { return IG_GTC_IMAGE_PAIR_COUNT; }

    // Ring GEOMETRY before ring MAPPING: a ring whose size is not legal cannot
    // be usefully described as mapped or unmapped, and a mapping-first order
    // would report the address of a ring nobody could have allocated.
    if (igGtRingSizeVerdict(ringBytes) != IG_GT_SUBMIT_OK) {
        return IG_GTC_IMAGE_RING_REFUSED;
    }
    if (!igGtxPageAddressOk(ringBase)) { return IG_GTC_IMAGE_RING_UNMAPPED; }

    return IG_GTC_IMAGE_OK;
}

// An EQUALITY against OK, never a negation of one error arm. A negation is true
// for every OTHER refusal including ones added later, so !=RING_REFUSED would
// call an unaligned LRCA admissible -- 3050-handoff32 section 3's reasoning,
// two subsystems over.
static inline bool igGtcImageAdmissible(IGU32 verdict)
{
    return verdict == IG_GTC_IMAGE_OK;
}

static inline const char *igGtcImageVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_GTC_IMAGE_NOT_ATTEMPTED:  return "context image not attempted";
    case IG_GTC_IMAGE_OK:             return "context image admissible";
    case IG_GTC_IMAGE_LRCA_RANGE:     return "LRCA above the descriptor field";
    case IG_GTC_IMAGE_LRCA_UNALIGNED: return "LRCA not page aligned";
    case IG_GTC_IMAGE_PAIR_COUNT:     return "register pair count refused";
    case IG_GTC_IMAGE_RING_REFUSED:   return "ring geometry refused";
    case IG_GTC_IMAGE_RING_UNMAPPED:  return "ring base not a usable page";
    default:                          return "unknown context image verdict";
    }
}

#endif // IntelGtContext_hpp
