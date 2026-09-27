//
//  IntelGtFence.hpp
//  Gen12 (Tiger Lake) COMPLETION SIGNAL composition -- the layer that answers
//  "how would this driver ever learn that submitted work finished?", which every
//  header beneath it declines to answer.
//
//  WHAT WAS MISSING
//  ----------------
//  IntelGtBlt.hpp composes packets saying what the engine would DO.
//  IntelGtBatch.hpp assembles those into a terminated batch buffer.
//  IntelGtRing.hpp writes an MI_BATCH_BUFFER_START into a circular buffer.
//  IntelGtExec.hpp composes the context descriptor that would hand that ring to
//  the hardware scheduler.
//
//  Join all four and the engine has work and a way to be given it -- and NO way
//  to report that the work is done. A submission whose completion cannot be
//  observed is indistinguishable, from the driver's side, from a submission that
//  hung: both look like "nothing has happened yet", forever. That is not a
//  missing convenience. It is the difference between a pipeline and a write-only
//  hole, and every layer in this tree so far is on the write-only side of it.
//
//  This header composes the two MI commands that close that loop, and NOTHING
//  else:
//
//    MI_FLUSH_DW      -- retire the engine's caches so a subsequent store is
//                        ordered AFTER the work it claims to have completed.
//    MI_STORE_DWORD_IMM -- write a caller-chosen seqno to a caller-chosen
//                        address, which the CPU then polls.
//
//  DEPENDENCY-FREE, same rule as every other header in phase0-kext/. The one
//  #include is a quoted sibling (IntelGtCmd.hpp) for IGU32/IGU64, the MI header
//  composer, and the qword-alignment policy. No IOKit, no kernel types, no
//  allocation, no sleep. Every function is a total function of its arguments.
//
//  THIS HEADER SUBMITS NOTHING AND SIGNALS NOTHING
//  ----------------------------------------------
//  It composes dwords into a span the CALLER owns. There is no register write
//  here, no MMIO offset, no doorbell. Composing a completion signal and firing
//  one are different acts, and only the first is in this file.
//
//  ORDERING IS THE WHOLE POINT, AND IT IS THE ONE THING A DWORD DUMP CANNOT SHOW
//  ----------------------------------------------------------------------------
//  A flush AFTER the store retires nothing that matters: the CPU can observe the
//  seqno while the render caches still hold unwritten pixels, so the driver
//  reports "done" over an unfinished frame and the corruption is timing-
//  dependent. Both orderings compose the identical SET of dwords, the identical
//  total length, and pass every "is this a well-formed MI packet" check. The
//  only thing that separates them is WHICH INDEX each lands at, so
//  igGtfWriteCompletion() is pinned by index and igGtfCompletionIsOrdered()
//  exists to make that assertable by a caller rather than only by a test.
//
//  This is handoff35 section 2's class -- a defect that composes a WELL-FORMED
//  page is not caught by the next thing that reads it.
//
//  CITED vs POLICY, same discipline as IntelGtCmd.hpp
//  --------------------------------------------------
//  Names carrying _I915_ plus a value are read out of reference/i915/ at the
//  line named in the comment. Names carrying _POLICY are this project's own
//  choices and are NOT hardware facts. The distinction is the only evidence
//  trail this project has; do not blur it by filing a guess next to a citation.
//
//  Where a value is composed from a sibling that owns it rather than spelled as
//  a literal, that is deliberate -- handoff37 section 2a. A wrong NAME is a
//  compile error clang points at; a wrong VALUE compiles cleanly and every gate
//  stays green over it.
//
#ifndef IntelGtFence_hpp
#define IntelGtFence_hpp

#include "IntelGtCmd.hpp"

// ---------------------------------------------------------------------------
// MI opcodes, cited
// ---------------------------------------------------------------------------
//
// MI_FLUSH_DW is MI_INSTR(0x26, 1) at intel_gpu_commands.h:164, i.e. opcode
// 0x26 with an excess-length field of 1. Gen8+ widens both the address and the
// immediate to 64 bits, which i915 handles by adding to the length at use sites
// rather than by defining a second opcode; the dword count is therefore a
// POLICY constant below and the OPCODE is the only thing cited here.
//
#define IG_GTF_MI_OPCODE_FLUSH_DW_I915_0x26     0x26u  // :164 MI_FLUSH_DW

// MI_STORE_DWORD_IMM is MI_INSTR(0x20, 1) at intel_gpu_commands.h:135. Same
// situation: the GEN8 form carries a 64-bit address, so the length differs from
// the macro's baked-in 1 and is policy.
//
// Spelled by composition from the sibling that already owns it -- IntelGtCmd.hpp
// cites the identical opcode at its line 175 for the batch-side store. Two
// spellings of one opcode DRIFT (handoff35 section 2a), and the drift is silent
// because both compose well-formed packets.
#define IG_GTF_MI_OPCODE_STORE_DWORD_I915_0x20  IG_GT_MI_OPCODE_STORE_DWORD_I915_0x20

// MI_GLOBAL_GTT is (1<<22) at intel_gpu_commands.h:40. Set, the address operand
// is interpreted through the global GTT; clear, through the context's own PPGTT.
#define IG_GTF_MI_GLOBAL_GTT_I915_BIT22         (1u << 22)

// MI_FLUSH_DW_STORE_INDEX is (1<<21) at intel_gpu_commands.h:166. This driver
// never sets it -- an indexed store writes through the hardware status page,
// which is a page this driver does not own. Defined so the ZERO is deliberate
// rather than an omission nobody noticed.
#define IG_GTF_MI_FLUSH_STORE_INDEX_I915_BIT21  (1u << 21)

// ---------------------------------------------------------------------------
// Packet geometry -- POLICY
// ---------------------------------------------------------------------------
//
// Gen8+ MI_FLUSH_DW: header, address low, address high, immediate low,
// immediate high == 5 dwords. This driver composes the NON-storing form (it
// flushes; the separate MI_STORE_DWORD_IMM does the reporting), which still
// carries the operand dwords as zeroes.
#define IG_GTF_FLUSH_DWORDS_POLICY_5            5u

// Gen8+ MI_STORE_DWORD_IMM: header, address low, address high, value == 4.
#define IG_GTF_STORE_DWORDS_POLICY_4            4u

// The completion pair is both, in that order.
#define IG_GTF_COMPLETION_DWORDS_POLICY \
    (IG_GTF_FLUSH_DWORDS_POLICY_5 + IG_GTF_STORE_DWORDS_POLICY_4)

// Byte length of the pair, composed from the dword count and the word size
// rather than written as a literal. handoff37 section 2a: a literal is CORRECT
// until somebody adds a dword, and then it is silently wrong.
#define IG_GTF_COMPLETION_BYTES_POLICY \
    (IG_GTF_COMPLETION_DWORDS_POLICY * IG_GT_CMD_WORD_BYTES_I915_4)

// The store target must be qword-aligned. intel_gpu_commands.h:16 defines
// alignof_qword as 8 for exactly this class of target; composed from the
// sibling policy that already states the tail alignment for the same reason.
#define IG_GTF_SEQNO_ALIGN_POLICY_8             IG_GT_TAIL_ALIGN_POLICY_8

// A seqno of zero is refused. Not because hardware objects -- it does not --
// but because the memory a driver polls starts at zero, so a completion store
// of zero is byte-identical to a store that never happened. That is handoff18
// section 1d's class aimed at the one word whose whole job is to be distinct
// from "nothing yet".
#define IG_GTF_SEQNO_RESERVED_POLICY_0          0u

// ---------------------------------------------------------------------------
// Verdicts
// ---------------------------------------------------------------------------
//
// A bool is satisfied by every refusal at once (handoff34 section 1), and these
// refusals are not interchangeable: a null span is a bug in the caller, an
// unaligned address is a bug in whoever allocated the target, and a reserved
// seqno is a bug in the sequence counter. Those call for three different fixes.
#define IG_GTF_FENCE_NOT_ATTEMPTED      0u  // caller's initialiser, never returned
#define IG_GTF_FENCE_OK                 1u  // composed
#define IG_GTF_FENCE_NULL_SPAN          2u  // destination pointer was null
#define IG_GTF_FENCE_SPAN_TOO_SHORT     3u  // span cannot hold the pair
#define IG_GTF_FENCE_ADDR_UNALIGNED     4u  // seqno address not qword-aligned
#define IG_GTF_FENCE_ADDR_ZERO          5u  // seqno address is zero
#define IG_GTF_FENCE_SEQNO_RESERVED     6u  // seqno is the reserved value
#define IG_GTF_FENCE_LIMIT              7u

// EQUALITY against OK, never a negation of one error arm. handoff33 section 2:
// a `!= SOME_ERROR` form reports every other refusal as success.
static inline bool igGtfFenceSucceeded(IGU32 verdict)
{
    return verdict == IG_GTF_FENCE_OK;
}

// Bounds on the family's own _LIMIT rather than a typed count, so a family that
// grows reddens on the commit that grows it -- handoff20 section 1, where
// igModeVerdictName() had arms 0..5 against a limit of 6 and published the
// unknown string for the one verdict that mattered most.
static inline const char *igGtfFenceVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GTF_FENCE_NOT_ATTEMPTED:  return "not-attempted";
        case IG_GTF_FENCE_OK:             return "ok";
        case IG_GTF_FENCE_NULL_SPAN:      return "null-span";
        case IG_GTF_FENCE_SPAN_TOO_SHORT: return "span-too-short";
        case IG_GTF_FENCE_ADDR_UNALIGNED: return "addr-unaligned";
        case IG_GTF_FENCE_ADDR_ZERO:      return "addr-zero";
        case IG_GTF_FENCE_SEQNO_RESERVED: return "seqno-reserved";
        default:                          return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Address predicates
// ---------------------------------------------------------------------------
//
// Alignment and zero are tested SEPARATELY and zero is tested FIRST, because
// zero trivially satisfies alignment -- a containment-first order would make
// IG_GTF_FENCE_ADDR_ZERO unreachable. That is handoff32 section 3's dead-arm
// shape, one subsystem over.
static inline bool igGtfSeqnoAddressAligned(IGU64 address)
{
    return (address % (IGU64)IG_GTF_SEQNO_ALIGN_POLICY_8) == 0u;
}

static inline bool igGtfSeqnoValueOk(IGU32 seqno)
{
    return seqno != IG_GTF_SEQNO_RESERVED_POLICY_0;
}

// Split from the composer so a caller can ask the question without a span, and
// so the ordering of the two address tests is assertable on its own.
static inline IGU32 igGtfSeqnoTargetVerdict(IGU64 address, IGU32 seqno)
{
    if (address == 0u) {
        return IG_GTF_FENCE_ADDR_ZERO;
    }
    if (!igGtfSeqnoAddressAligned(address)) {
        return IG_GTF_FENCE_ADDR_UNALIGNED;
    }
    if (!igGtfSeqnoValueOk(seqno)) {
        return IG_GTF_FENCE_SEQNO_RESERVED;
    }
    return IG_GTF_FENCE_OK;
}

// ---------------------------------------------------------------------------
// Header composition
// ---------------------------------------------------------------------------
//
// Both headers go through igGtMiHeader() rather than being spelled, so the
// client field and the two shifts have exactly one definition in this tree.
//
// The length field of an MI command is EXCESS -- dwords minus two. Composed by
// subtraction from the dword-count policy above rather than written as a
// literal, for handoff37 section 2a's reason: the literal is right until the
// packet grows.
#define IG_GTF_MI_EXCESS_BIAS_POLICY_2            2u

static inline IGU32 igGtfFlushHeader(void)
{
    return igGtMiHeader(IG_GTF_MI_OPCODE_FLUSH_DW_I915_0x26,
                        IG_GTF_FLUSH_DWORDS_POLICY_5 - IG_GTF_MI_EXCESS_BIAS_POLICY_2);
}

// The store addresses global-GTT space, so bit 22 is SET. A PPGTT store would
// clear it and write into the context's private address space, which this
// driver does not build page tables for.
static inline IGU32 igGtfStoreHeader(void)
{
    return igGtMiHeader(IG_GTF_MI_OPCODE_STORE_DWORD_I915_0x20,
                        IG_GTF_STORE_DWORDS_POLICY_4 - IG_GTF_MI_EXCESS_BIAS_POLICY_2)
           | IG_GTF_MI_GLOBAL_GTT_I915_BIT22;
}

// Address halves. Reused from IntelGtCmd.hpp's batch-address split rather than
// re-derived, because two spellings of one 64-bit split drift the same way two
// spellings of a length do.
static inline IGU32 igGtfAddressLow(IGU64 address)
{
    return igGtBatchAddressLow(address);
}

static inline IGU32 igGtfAddressHigh(IGU64 address)
{
    return igGtBatchAddressHigh(address);
}

// ---------------------------------------------------------------------------
// Index layout -- named, because the ordering IS the property
// ---------------------------------------------------------------------------
//
// These are the indices igGtfWriteCompletion() writes at, relative to the start
// of the pair. They are named rather than left as arithmetic inside the writer
// so that a test, a caller, and the writer all refer to ONE definition of the
// layout. A test that recomputed them independently would agree with a writer
// that had drifted.
#define IG_GTF_IDX_FLUSH_HEADER_POLICY          0u
#define IG_GTF_IDX_FLUSH_ADDR_LO_POLICY         1u
#define IG_GTF_IDX_FLUSH_ADDR_HI_POLICY         2u
#define IG_GTF_IDX_FLUSH_IMM_LO_POLICY          3u
#define IG_GTF_IDX_FLUSH_IMM_HI_POLICY          4u
#define IG_GTF_IDX_STORE_HEADER_POLICY          5u
#define IG_GTF_IDX_STORE_ADDR_LO_POLICY         6u
#define IG_GTF_IDX_STORE_ADDR_HI_POLICY         7u
#define IG_GTF_IDX_STORE_VALUE_POLICY           8u

// ---------------------------------------------------------------------------
// The composer
// ---------------------------------------------------------------------------
//
// Writes the flush/store pair into `dwords`, returns a verdict, and writes
// NOTHING on any refusal -- a partially composed packet is worse than none,
// because it is well-formed for its first few dwords and the engine would
// execute it.
//
// The flush's own operand dwords are written as explicit zeroes rather than
// left alone. Leaving them would publish whatever the caller's buffer held, and
// stale ring memory decodes as a plausible address.
static inline IGU32 igGtfWriteCompletion(IGU32 *dwords, IGU32 dwordCount,
                                         IGU64 seqnoAddress, IGU32 seqnoValue)
{
    IGU32 targetVerdict;

    if (dwords == 0) {
        return IG_GTF_FENCE_NULL_SPAN;
    }
    if (dwordCount < IG_GTF_COMPLETION_DWORDS_POLICY) {
        return IG_GTF_FENCE_SPAN_TOO_SHORT;
    }

    targetVerdict = igGtfSeqnoTargetVerdict(seqnoAddress, seqnoValue);
    if (targetVerdict != IG_GTF_FENCE_OK) {
        return targetVerdict;
    }

    dwords[IG_GTF_IDX_FLUSH_HEADER_POLICY]  = igGtfFlushHeader();
    dwords[IG_GTF_IDX_FLUSH_ADDR_LO_POLICY] = 0u;
    dwords[IG_GTF_IDX_FLUSH_ADDR_HI_POLICY] = 0u;
    dwords[IG_GTF_IDX_FLUSH_IMM_LO_POLICY]  = 0u;
    dwords[IG_GTF_IDX_FLUSH_IMM_HI_POLICY]  = 0u;

    dwords[IG_GTF_IDX_STORE_HEADER_POLICY]  = igGtfStoreHeader();
    dwords[IG_GTF_IDX_STORE_ADDR_LO_POLICY] = igGtfAddressLow(seqnoAddress);
    dwords[IG_GTF_IDX_STORE_ADDR_HI_POLICY] = igGtfAddressHigh(seqnoAddress);
    dwords[IG_GTF_IDX_STORE_VALUE_POLICY]   = seqnoValue;

    return IG_GTF_FENCE_OK;
}

// ---------------------------------------------------------------------------
// The ordering predicate
// ---------------------------------------------------------------------------
//
// Reads a composed span back and answers whether the flush precedes the store.
// This exists because the two orderings are indistinguishable by every other
// property: same dwords, same count, same byte length, both well-formed.
//
// It compares against the composers rather than against re-spelled literals, so
// a change to either header word moves the subject and the predicate together
// -- which is the point. This predicate does not certify that the words are
// CORRECT; it certifies that they are in the order this header claims to write.
// Those are different claims and only the second is checkable without hardware.
static inline bool igGtfCompletionIsOrdered(const IGU32 *dwords, IGU32 dwordCount)
{
    if (dwords == 0) {
        return false;
    }
    if (dwordCount < IG_GTF_COMPLETION_DWORDS_POLICY) {
        return false;
    }
    if (dwords[IG_GTF_IDX_FLUSH_HEADER_POLICY] != igGtfFlushHeader()) {
        return false;
    }
    if (dwords[IG_GTF_IDX_STORE_HEADER_POLICY] != igGtfStoreHeader()) {
        return false;
    }
    return true;
}

// Round-trips the address halves back into one 64-bit value. A caller with a
// composed span can therefore check that the address the engine would store to
// is the address it asked for, which a dword dump cannot show -- the high half
// sits three dwords from the low one and reads as an unrelated word.
static inline IGU64 igGtfComposedSeqnoAddress(const IGU32 *dwords, IGU32 dwordCount)
{
    if (dwords == 0 || dwordCount < IG_GTF_COMPLETION_DWORDS_POLICY) {
        return 0u;
    }
    return ((IGU64)dwords[IG_GTF_IDX_STORE_ADDR_HI_POLICY] << 32)
           | (IGU64)dwords[IG_GTF_IDX_STORE_ADDR_LO_POLICY];
}

// ---------------------------------------------------------------------------
// Completion observation -- the CPU side
// ---------------------------------------------------------------------------
//
// The seqno the engine stores is polled by the driver. This is the arithmetic
// for deciding whether a given seqno has retired, and it is a WRAPPING compare
// rather than a plain `>=`.
//
// A plain `>=` is correct until the 32-bit counter wraps, at which point every
// outstanding submission reports as complete AT ONCE -- the driver frees
// buffers the engine is still reading, and the failure is a use-after-free four
// billion submissions into a session rather than an obvious bug on day one.
// The signed-difference form is the standard fix and is correct across the
// wrap as long as the two values are within 2^31 of each other, which they are
// by construction: a driver cannot have 2 Gi submissions in flight.
static inline bool igGtfSeqnoPassed(IGU32 observed, IGU32 target)
{
    // Unsigned subtraction wraps defined; the cast reads the result as signed.
    return (int)(observed - target) >= 0;
}

// A target of zero is the reserved value and never legitimately awaited, so
// asking whether it passed is a caller bug rather than a question with an
// answer. Reported as NOT passed, which is the safe direction: the alternative
// frees memory on a seqno nobody ever stored.
static inline bool igGtfSeqnoRetired(IGU32 observed, IGU32 target)
{
    if (!igGtfSeqnoValueOk(target)) {
        return false;
    }
    return igGtfSeqnoPassed(observed, target);
}

// Advances a sequence counter, skipping the reserved value on wrap. Without
// this, one submission in every 2^32 is handed the seqno that means "nothing
// has happened yet" and its completion is permanently unobservable.
static inline IGU32 igGtfNextSeqno(IGU32 current)
{
    IGU32 next = current + 1u;
    if (next == IG_GTF_SEQNO_RESERVED_POLICY_0) {
        next = IG_GTF_SEQNO_RESERVED_POLICY_0 + 1u;
    }
    return next;
}

#endif /* IntelGtFence_hpp */
