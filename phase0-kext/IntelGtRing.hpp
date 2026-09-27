//
//  IntelGtRing.hpp
//  Gen12 (Tiger Lake) RING WRITER -- the layer that puts composed dwords into a
//  CIRCULAR buffer, which is the one thing every header beneath it declined to
//  do.
//
//  WHAT WAS MISSING
//  ----------------
//  IntelGtCmd.hpp owns the ring's ARITHMETIC. igGtRingFreeBytes() says how much
//  room there is, igGtRingAdvance() says where the tail lands, and
//  igGtSubmitVerdict() says whether a submission of a given length is
//  admissible at all. Every one of those is a total function of integers and
//  none of them touches memory.
//
//  IntelGtBatch.hpp composes MI_BATCH_BUFFER_START plus its two address dwords
//  into a span -- but into a FLAT span, indexed [0], [1], [2]. That is correct
//  for a batch buffer, which is linear. It is wrong for a ring, which is not.
//
//  So the join was still missing at the last step: a caller holding an
//  admissible verdict and a composed packet had no way to STORE it that
//  respected the wrap. The obvious spelling --
//
//      igGtqWriteBatchStart(&ring[tailDwords], ringDwords - tailDwords, ...)
//
//  -- is not merely wrong, it is wrong in the direction that looks right: it
//  REFUSES with SPAN_TOO_SHORT near the end of the ring, where the correct
//  behaviour is to wrap. A caller reading that refusal would conclude the ring
//  was full when it was empty, and the driver would wedge at the one tail value
//  that needed the wrap most.
//
//  THE THREE THINGS THAT GO WRONG HERE, AND ALL OF THEM EXECUTE
//  -----------------------------------------------------------
//    - A packet SPLIT across the wrap point stores its second half at ring[0].
//      Every dword is well-formed, the header's length field is right, and the
//      streamer executes the two halves as though they were adjacent -- which
//      they are, in ring order. Getting this wrong does not fault; it puts an
//      address dword where an opcode belongs.
//    - A tail advanced past HEAD overwrites commands the engine has not read
//      yet. IntelGtCmd.hpp:312 reserves IG_GT_WRAP_RESERVE_POLICY_0x40 for
//      exactly this reason, and this header consumes that reservation rather
//      than restating it -- igGtRingFreeBytes() is the authority.
//    - A tail left at a byte offset the TAIL REGISTER cannot hold is not a
//      rounding error. IG_GT_SUBMIT_TAIL_UNALIGNED exists one layer down
//      because the register's low bits are not address bits, so this header
//      PADS with MI_NOOP rather than writing an unaligned tail. The pad goes
//      AFTER the payload, where the streamer reads it as a no-op, never before.
//
//  THIS HEADER OWNS NO RING
//  ------------------------
//  Same rule as every dependency-free header in phase0-kext/: no IOKit, no
//  kernel types, no allocation, no sleeping, no MMIO. The ring is a span the
//  CALLER owns and whose length the caller states, and the head is a value the
//  caller READ -- this header never reads a register, so it cannot know whether
//  the head it was handed is a sample from a moving engine. intel-handoff28.md
//  section 4d is why that distinction is written down: a register read on
//  running silicon is a SAMPLE, and a stale head is CONSERVATIVE here (it
//  under-reports free space) which is the safe direction.
//
//  Nothing composed here has ever been submitted to silicon. IntelGtProbe.hpp's
//  banner still holds and IGGtSubmitted is still published false.
//
//  EVIDENCE CLASS
//  --------------
//  Every command word stored here comes from IntelGtCmd.hpp's cited MI opcodes
//  and every geometry decision from its cited ring offsets. What this file adds
//  is PLACEMENT and WRAP, and those are POLICY -- reference/i915/ carries no
//  ring-writer file in this tree. No constant here is _OBSERVED_: igpu-results/
//  captured a machine whose render engine was idle, and an idle engine has no
//  ring traffic to observe.
//
//  THE VERDICT PREFIX IS IG_GTR_ AND THAT IS DELIBERATE
//  ---------------------------------------------------
//  intel_verdict_set_checks() in tools/check-registers.py groups verdict
//  families BY PREFIX. IG_GT_SUBMIT_ is IntelGtCmd.hpp's, IG_GTB_ is the
//  blitter's and IG_GTQ_ is the batch layer's; a family sharing any of them
//  would sweep that header's constants into this one's contiguity check -- the
//  IG_PIPE_TRANSCONF trap from handoff16 section 1, which printed a failure
//  that was correct about its own arithmetic and wrong about the world.
//  IG_GTR_ was checked against the whole tree before it was chosen.
//

#ifndef IntelGtRing_hpp
#define IntelGtRing_hpp

#include "IntelGtCmd.hpp"
#include "IntelGtBatch.hpp"

// ---------------------------------------------------------------------------
// Verdicts.
//
// Contiguous from zero to the family's own LIMIT. Zero is NOT_ATTEMPTED -- the
// caller's initialiser -- and is never returned, so an out-param nobody wrote
// cannot read as a verdict somebody measured.
// ---------------------------------------------------------------------------

#define IG_GTR_RING_NOT_ATTEMPTED       0u
#define IG_GTR_RING_OK                  1u
#define IG_GTR_RING_NULL_SPAN           2u
#define IG_GTR_RING_BAD_GEOMETRY        3u
#define IG_GTR_RING_BAD_SPAN            4u
#define IG_GTR_RING_NOT_ADMISSIBLE      5u
#define IG_GTR_RING_EMPTY_PAYLOAD       6u
#define IG_GTR_RING_PAYLOAD_TOO_LONG    7u
#define IG_GTR_RING_LIMIT               8u

// Admissibility is an EQUALITY against OK, never a negation of one error arm.
// "!= NOT_ADMISSIBLE" is true for six other refusals and would report a write
// nobody could perform as one that landed.
static inline bool igGtrRingSucceeded(IGU32 verdict)
{
    return verdict == IG_GTR_RING_OK;
}

static inline const char *igGtrRingVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GTR_RING_NOT_ATTEMPTED:    return "not attempted";
        case IG_GTR_RING_OK:               return "ok";
        case IG_GTR_RING_NULL_SPAN:        return "null ring span";
        case IG_GTR_RING_BAD_GEOMETRY:     return "ring geometry refused";
        case IG_GTR_RING_BAD_SPAN:         return "ring span length unusable";
        case IG_GTR_RING_NOT_ADMISSIBLE:   return "submission not admissible";
        case IG_GTR_RING_EMPTY_PAYLOAD:    return "empty payload";
        case IG_GTR_RING_PAYLOAD_TOO_LONG: return "payload above ceiling";
        default:                           return "unknown ring verdict";
    }
}

// ---------------------------------------------------------------------------
// Mapping IntelGtCmd.hpp's refusal onto this family.
//
// An exhaustive SWITCH with no default:, deliberately. A hand-written list of
// the arms that mean "geometry" is satisfied by every arm it forgot -- and the
// direction it forgets in is the dangerous one, because an unlisted arm falls
// through to whatever the last clause says rather than failing to build. A
// tenth IG_GT_SUBMIT_ verdict added one layer down must not compile until
// somebody decides which of this header's two refusals it means.
//
// This is IntelGtBlt.hpp / handoff32 section 3's reasoning: an exhaustive switch
// with no default: turns a silent misclassification into a build error.
//
// NOT_ATTEMPTED and OK are listed and both answer BAD_GEOMETRY, because reaching
// this function with either is impossible -- the caller tests admissibility
// first, and NOT_ATTEMPTED is never returned by anything. An arm that cannot be
// reached still needs a value that is safe if the impossible happens, and
// refusing is that value.
// ---------------------------------------------------------------------------

static inline IGU32 igGtrClassifySubmit(IGU32 submitVerdict)
{
    switch (submitVerdict) {
        // The ring, the engine, or the caller's head/tail pair is wrong. These
        // are facts about the geometry the caller handed in, fixable before any
        // dword is composed.
        case IG_GT_SUBMIT_BAD_ENGINE:
        case IG_GT_SUBMIT_RING_UNALIGNED:
        case IG_GT_SUBMIT_RING_TOO_SMALL:
        case IG_GT_SUBMIT_RING_TOO_LARGE:
        case IG_GT_SUBMIT_HEAD_OUT_OF_RING:
        case IG_GT_SUBMIT_TAIL_OUT_OF_RING:
        case IG_GT_SUBMIT_TAIL_UNALIGNED:
        case IG_GT_SUBMIT_NOT_ATTEMPTED:
        case IG_GT_SUBMIT_OK:
            return IG_GTR_RING_BAD_GEOMETRY;

        // The geometry is fine and this particular submission does not fit or
        // is malformed. A caller seeing this can retry after the engine has
        // consumed some ring; a caller seeing BAD_GEOMETRY cannot.
        case IG_GT_SUBMIT_EMPTY:
        case IG_GT_SUBMIT_UNALIGNED_LENGTH:
        case IG_GT_SUBMIT_NO_SPACE:
            return IG_GTR_RING_NOT_ADMISSIBLE;
    }

    // Unreachable while the switch above is exhaustive. Present because C++
    // requires a return on every path, and refusing is the safe answer.
    return IG_GTR_RING_BAD_GEOMETRY;
}

// ---------------------------------------------------------------------------
// Geometry.
// ---------------------------------------------------------------------------

// A payload ceiling, for the same reason IntelGtBatch.hpp carries one: it keeps
// the dword-to-byte multiplication below from wrapping. A payload larger than
// the largest legal ring can never be admissible anyway, so this refuses early
// and by name rather than late and by arithmetic.
#define IG_GTR_MAX_PAYLOAD_DWORDS_POLICY  0x00040000u  /* 256 Ki dwords == 1 MiB */

static inline IGU32 igGtrPayloadBytes(IGU32 dwords)
{
    if (dwords > IG_GTR_MAX_PAYLOAD_DWORDS_POLICY) {
        return 0u;
    }
    return dwords * IG_GT_CMD_WORD_BYTES_I915_4;
}

// How many MI_NOOPs must follow a payload of payloadDwords, starting at
// tailBytes, so the resulting tail satisfies IG_GT_SUBMIT_TAIL_UNALIGNED's
// rule one layer down.
//
// Exposed rather than inlined at the call site so a caller can size its
// reservation BEFORE writing anything -- asking after the fact means
// discovering the ring is one dword short with a half-written packet in it,
// which is the state this whole header exists to make unreachable.
static inline IGU32 igGtrPadDwords(IGU32 tailBytes, IGU32 payloadDwords)
{
    IGU32 endBytes;
    IGU32 remainder;

    if (payloadDwords > IG_GTR_MAX_PAYLOAD_DWORDS_POLICY) {
        return 0u;
    }

    endBytes = tailBytes + (payloadDwords * IG_GT_CMD_WORD_BYTES_I915_4);
    remainder = endBytes % IG_GT_TAIL_ALIGN_POLICY_8;
    if (remainder == 0u) {
        return 0u;
    }

    // The alignment is a byte count and a dword is four bytes, so the shortfall
    // is always a whole number of dwords. Dividing rather than looping keeps
    // this a total function with no bound to get wrong.
    return (IG_GT_TAIL_ALIGN_POLICY_8 - remainder) / IG_GT_CMD_WORD_BYTES_I915_4;
}

// Total dwords a payload will consume once padded. This is what the caller must
// pass to igGtSubmitVerdict() as a BYTE count -- not the bare payload -- because
// a submission that fits only without its pad does not fit.
static inline IGU32 igGtrTotalDwords(IGU32 tailBytes, IGU32 payloadDwords)
{
    if (payloadDwords > IG_GTR_MAX_PAYLOAD_DWORDS_POLICY) {
        return 0u;
    }
    return payloadDwords + igGtrPadDwords(tailBytes, payloadDwords);
}

// ---------------------------------------------------------------------------
// The wrapping store.
//
// This is the whole point of the header and it is four lines of body under
// forty of refusals, which is the correct ratio for a function whose failure
// mode is executing an address as an opcode.
// ---------------------------------------------------------------------------

// Store payloadDwords of composed dwords into a ring span at a dword offset,
// wrapping at the end of the ring. Pads to the tail alignment with MI_NOOP.
//
// ARM ORDER, and each step is pinned by an input bad in two ways at once:
//   1. NULL spans     -- either one, before anything else is inspected.
//   2. Geometry       -- delegated WHOLE to igGtSubmitVerdict(). This header
//                        does not restate the size, head, tail or alignment
//                        rules; two spellings of one rule agree until one of
//                        them moves, and this project has already paid for
//                        that (handoff36 section 2a).
//   3. Span mismatch  -- the caller's dword count must describe the SAME ring
//                        the byte geometry describes. A span shorter than the
//                        ring wraps early and scribbles; a span longer than it
//                        leaves the far end unreachable, which is merely
//                        wasteful, but both mean the caller has two ideas of
//                        one ring and only one of them can be right.
//   4. Payload        -- emptiness FIRST, then the ceiling, for the dead-arm
//                        reason IntelGtCmd.hpp:366 spells out: a zero-length
//                        payload trivially satisfies the ceiling, so a
//                        ceiling-first order makes EMPTY_PAYLOAD unreachable.
//
// outTailBytes is written on EVERY path, refusals included -- verbatim
// IntelGtBatch.hpp's rule, and for the same reason: a ring tail advanced by a
// count from a REFUSED store walks the tail into dwords the engine executes.
// On a refusal it is written with the tail UNCHANGED, which is the value that
// makes a caller ignoring the verdict harmless rather than catastrophic.
static inline IGU32 igGtrWriteWrapped(IGU32 *ring, IGU32 ringDwords,
                                      const IGU32 *payload, IGU32 payloadDwords,
                                      IGU32 engine,
                                      IGU32 headBytes, IGU32 tailBytes,
                                      IGU32 *outTailBytes)
{
    IGU32 ringBytes;
    IGU32 padDwords;
    IGU32 totalDwords;
    IGU32 submitVerdict;
    IGU32 cursor;
    IGU32 i;

    // The tail is reported unchanged before a single argument is inspected, so
    // even the NULL-span arm leaves the caller with a usable value.
    if (outTailBytes != 0) { *outTailBytes = tailBytes; }

    if (ring == 0 || payload == 0) { return IG_GTR_RING_NULL_SPAN; }

    // Payload shape before geometry: an empty or over-long payload is a bug in
    // the composition, while a bad ring is a fact about the caller's
    // allocation. Report the fixable thing first -- IntelGtBatch.hpp's arm
    // order, arrived at for the same reason.
    if (payloadDwords == 0u) { return IG_GTR_RING_EMPTY_PAYLOAD; }
    if (payloadDwords > IG_GTR_MAX_PAYLOAD_DWORDS_POLICY) {
        return IG_GTR_RING_PAYLOAD_TOO_LONG;
    }

    // The span and the geometry must describe one ring. Checked BEFORE the
    // submit verdict because a mismatch makes every byte answer that verdict
    // gives meaningless as a statement about this span.
    ringBytes = ringDwords * IG_GT_CMD_WORD_BYTES_I915_4;
    if (ringDwords == 0u ||
        (ringDwords > (0xFFFFFFFFu / IG_GT_CMD_WORD_BYTES_I915_4))) {
        return IG_GTR_RING_BAD_SPAN;
    }

    padDwords = igGtrPadDwords(tailBytes, payloadDwords);
    totalDwords = payloadDwords + padDwords;

    // Geometry and admissibility in ONE call, against the PADDED length. Asking
    // about the bare payload would accept a submission whose pad does not fit,
    // and the pad is not optional -- an unaligned tail is refused by the layer
    // that owns the register.
    submitVerdict = igGtSubmitVerdict(engine, ringBytes, headBytes, tailBytes,
                                      totalDwords * IG_GT_CMD_WORD_BYTES_I915_4);

    if (!igGtSubmitAdmissible(submitVerdict)) {
        return igGtrClassifySubmit(submitVerdict);
    }

    // Everything below this line is a store. The tail is a BYTE offset and the
    // span is indexed in DWORDS, and the division is exact because
    // IG_GT_SUBMIT_TAIL_UNALIGNED already refused every tail that is not a
    // multiple of eight -- which is a multiple of four.
    cursor = tailBytes / IG_GT_CMD_WORD_BYTES_I915_4;

    for (i = 0u; i < payloadDwords; i++) {
        ring[cursor] = payload[i];
        cursor++;
        if (cursor == ringDwords) { cursor = 0u; }
    }

    // The pad goes AFTER the payload. Padding ahead of it would be executed as
    // commands preceding the packet, which is harmless for a NOOP and would
    // stop being harmless the moment somebody padded with anything else.
    for (i = 0u; i < padDwords; i++) {
        ring[cursor] = igGtMiNoop();
        cursor++;
        if (cursor == ringDwords) { cursor = 0u; }
    }

    // The new tail comes from IntelGtCmd.hpp's own advance rather than from the
    // cursor. The two agree by construction here, and deriving it from the
    // cursor would make this header a second authority on wrap arithmetic --
    // the exact duplication the geometry delegation above avoids.
    if (outTailBytes != 0) {
        *outTailBytes = igGtRingAdvance(tailBytes,
                                        totalDwords * IG_GT_CMD_WORD_BYTES_I915_4,
                                        ringBytes);
    }
    return IG_GTR_RING_OK;
}

// ---------------------------------------------------------------------------
// The whole join, in one call.
//
// Point the ring at a batch buffer: compose MI_BATCH_BUFFER_START plus its two
// address dwords and store them into the ring with the wrap respected. This is
// the first function in this tree that takes a batch address and leaves the
// ring in the state the engine would need to find it.
//
// It still submits nothing. Writing the resulting tail to RING_TAIL is an MMIO
// store this header cannot make and IntelGtProbe.cpp deliberately does not
// contain -- intel-handoff36.md section 1 is why, and nothing in it has changed.
//
// The batch layer's refusal is PROPAGATED rather than collapsed. A caller handed
// IG_GTR_RING_NOT_ADMISSIBLE for an address whose low bits were wrong would go
// looking in the wrong layer entirely, so the batch verdict is returned through
// an out-param and the ring verdict says only what the RING did.
// ---------------------------------------------------------------------------

static inline IGU32 igGtrWriteBatchStartToRing(IGU32 *ring, IGU32 ringDwords,
                                               IGU32 engine,
                                               IGU32 headBytes, IGU32 tailBytes,
                                               IGU64 batchAddress,
                                               IGU32 *outBatchVerdict,
                                               IGU32 *outTailBytes)
{
    IGU32 packet[IG_GT_MI_BB_START_DWORDS_POLICY_3];
    IGU32 batchVerdict;
    IGU32 written = 0u;

    // Both out-params on EVERY path, including the ones that return before a
    // single argument is inspected.
    if (outBatchVerdict != 0) { *outBatchVerdict = IG_GTQ_BATCH_NOT_ATTEMPTED; }
    if (outTailBytes != 0)    { *outTailBytes = tailBytes; }

    // Composed into a LOCAL span first, then stored. Composing straight into
    // the ring is what the flat-span spelling in this header's banner gets
    // wrong: igGtqWriteBatchStart() indexes [0], [1], [2] and cannot wrap, so
    // a tail two dwords from the end would either refuse a ring that is empty
    // or store past the end of the span. Three dwords of stack removes the
    // question entirely.
    batchVerdict = igGtqWriteBatchStart(packet,
                                        IG_GT_MI_BB_START_DWORDS_POLICY_3,
                                        batchAddress, &written);

    if (outBatchVerdict != 0) { *outBatchVerdict = batchVerdict; }

    if (!igGtqBatchSucceeded(batchVerdict)) {
        // The packet was never composed, so there is nothing to store and the
        // tail must not move. The ring verdict is EMPTY_PAYLOAD, which is true:
        // no dword reached it.
        return IG_GTR_RING_EMPTY_PAYLOAD;
    }

    return igGtrWriteWrapped(ring, ringDwords, packet, written,
                             engine, headBytes, tailBytes, outTailBytes);
}

#endif /* IntelGtRing_hpp */
