//
//  IntelGtBatch.hpp
//  Gen12 (Tiger Lake) BATCH BUFFER assembly -- the join between work and
//  delivery, and the first header in this tree that is about neither one.
//
//  WHAT WAS MISSING
//  ----------------
//  IntelGtBlt.hpp composes packets that say what the engine would DO.
//  IntelGtCmd.hpp composes the ring geometry and the MI commands that would
//  DELIVER them, including MI_BATCH_BUFFER_START and its two address dwords.
//  Nothing joined the two. A caller holding a composed blit packet had no
//  admissible way to ask "where does this go, what terminates it, and what do I
//  put in the ring so the engine finds it".
//
//  That gap is not cosmetic. The three questions have wrong answers that all
//  produce a well-formed, executable, catastrophic result:
//
//    - A batch buffer with no MI_BATCH_BUFFER_END does not stop. The command
//      streamer keeps fetching past the last packet the caller wrote and
//      executes whatever the allocation happened to contain as commands. That
//      is the single worst failure mode available on this path, and it is the
//      one an unterminated buffer produces EVERY time rather than
//      intermittently.
//    - A batch address the low bits of MI_BATCH_BUFFER_START's first dword
//      cannot hold does not misfetch, it sets FLAGS -- IntelGtCmd.hpp:439 says
//      so, which is why its splitters refuse rather than truncate. This header
//      consumes that refusal rather than restating the alignment rule.
//    - A ring tail advanced by a count from a REFUSED compose walks the tail
//      into dwords the engine executes as commands. Every writer here therefore
//      writes its out-param on every path, refusals included, exactly as
//      IntelGtBlt.hpp does.
//
//  THIS HEADER SUBMITS NOTHING AND ALLOCATES NOTHING
//  ------------------------------------------------
//  Same rule as every dependency-free header in phase0-kext/: no IOKit, no
//  kernel types, no allocation, no sleeping. Every writer stores into a span the
//  CALLER owns and whose length the caller states. There is no ring here, no
//  GGTT binding, and no engine -- IntelGtProbe.hpp's banner still holds, and
//  nothing composed here has ever been submitted to silicon.
//
//  EVIDENCE CLASS
//  --------------
//  Every command word this header emits comes from IntelGtCmd.hpp's cited MI
//  opcodes. What this file adds is ORDER, TERMINATION and ADMISSIBILITY, and
//  those are POLICY -- reference/i915/ carries no batch-assembly file in this
//  tree, so the names say POLICY and mean it. No constant here is _OBSERVED_,
//  because igpu-results/ captured a machine whose render engine was idle and
//  there is nothing about batch assembly to observe on an idle engine.
//
//  THE VERDICT PREFIX IS IG_GTQ_ AND THAT IS DELIBERATE
//  ---------------------------------------------------
//  intel_verdict_set_checks() in tools/check-registers.py groups verdict
//  families BY PREFIX. IG_GTB_ is the blitter's, and a family sharing it would
//  sweep the blitter's constants into this one's contiguity check -- the
//  IG_PIPE_TRANSCONF trap from handoff16 section 1, which printed a failure that
//  was correct about its own arithmetic and wrong about the world.
//

#ifndef IntelGtBatch_hpp
#define IntelGtBatch_hpp

#include "IntelGtCmd.hpp"
#include "IntelGtBlt.hpp"

// ---------------------------------------------------------------------------
// Verdicts.
//
// Contiguous from zero to the family's own LIMIT. Zero is NOT_ATTEMPTED -- the
// caller's initialiser -- and is never returned, so an out-param nobody wrote
// cannot read as a verdict somebody measured.
// ---------------------------------------------------------------------------

#define IG_GTQ_BATCH_NOT_ATTEMPTED      0u
#define IG_GTQ_BATCH_OK                 1u
#define IG_GTQ_BATCH_NULL_SPAN          2u
#define IG_GTQ_BATCH_SPAN_TOO_SHORT     3u
#define IG_GTQ_BATCH_ADDR_UNALIGNED     4u
#define IG_GTQ_BATCH_EMPTY              5u
#define IG_GTQ_BATCH_UNTERMINATED       6u
#define IG_GTQ_BATCH_TOO_LARGE          7u
#define IG_GTQ_BATCH_LIMIT              8u

// Admissibility is an EQUALITY against OK, never a negation of one error arm.
// "!= UNTERMINATED" is true for six other refusals and would report a batch
// nobody could assemble as ready to dispatch.
static inline bool igGtqBatchSucceeded(IGU32 verdict)
{
    return verdict == IG_GTQ_BATCH_OK;
}

static inline const char *igGtqBatchVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GTQ_BATCH_NOT_ATTEMPTED:  return "not attempted";
        case IG_GTQ_BATCH_OK:             return "ok";
        case IG_GTQ_BATCH_NULL_SPAN:      return "null batch span";
        case IG_GTQ_BATCH_SPAN_TOO_SHORT: return "batch span too short";
        case IG_GTQ_BATCH_ADDR_UNALIGNED: return "batch address not 4-byte aligned";
        case IG_GTQ_BATCH_EMPTY:          return "empty batch";
        case IG_GTQ_BATCH_UNTERMINATED:   return "batch has no MI_BATCH_BUFFER_END";
        case IG_GTQ_BATCH_TOO_LARGE:      return "batch above ceiling";
        default:                          return "unknown batch verdict";
    }
}

// ---------------------------------------------------------------------------
// Geometry.
//
// The ceiling is a POLICY number and is here so a caller cannot hand in a dword
// count that overflows the byte arithmetic below it. A batch of 2^30 dwords is
// 4 GiB and multiplying it by four wraps -- the ceiling is what keeps
// igGtqBatchBytes() from returning a small number for an enormous batch, which
// is the direction that under-reserves ring space rather than over-reserving it.
// ---------------------------------------------------------------------------

#define IG_GTQ_MAX_BATCH_DWORDS_POLICY  0x00100000u  /* 1 Mi dwords == 4 MiB */

// Batch buffers are dispatched on a qword boundary by the same convention that
// makes IG_GT_SUBMIT_TAIL_UNALIGNED a refusal one layer down. A terminator that
// left an odd dword count would compose a batch whose end the ring cannot point
// at cleanly, so the terminator PADS.
#define IG_GTQ_ALIGN_DWORDS_POLICY_2    2u

// How many dwords the terminator will consume for a batch currently holding
// usedDwords. Exposed rather than inlined at the call site so a caller can size
// its allocation BEFORE composing anything -- asking after the fact means
// discovering the buffer is one dword short with packets already in it.
static inline IGU32 igGtqTerminatorDwords(IGU32 usedDwords)
{
    // MI_BATCH_BUFFER_END is one dword. If that lands the total on an odd
    // count, one MI_NOOP follows it. The NOOP is AFTER the end, not before:
    // padding ahead of the terminator would be executed, padding behind it
    // never is.
    if (((usedDwords + 1u) % IG_GTQ_ALIGN_DWORDS_POLICY_2) == 0u) {
        return 1u;
    }
    return 2u;
}

static inline IGU32 igGtqBatchBytes(IGU32 dwords)
{
    if (dwords > IG_GTQ_MAX_BATCH_DWORDS_POLICY) {
        return 0u;
    }
    return dwords * IG_GT_CMD_WORD_BYTES_I915_4;
}

// ---------------------------------------------------------------------------
// Termination.
// ---------------------------------------------------------------------------

// Append MI_BATCH_BUFFER_END (plus one MI_NOOP when padding is needed) to a
// batch that already holds usedDwords of packets.
//
// ARM ORDER, and each step is pinned by an input bad in two ways at once:
//   1. NULL span      -- before anything else is inspected.
//   2. Span too short -- writing is what scribbles; every other complaint is
//                        merely wrong, so the span wins.
//   3. Empty batch    -- a batch with no packets is refused rather than
//                        terminated. A bare MI_BATCH_BUFFER_END is a valid
//                        buffer that does nothing, and dispatching it burns a
//                        ring slot and an execlist port on a no-op the caller
//                        did not intend.
//   4. Too large      -- checked last, because it is the one arm that is a fact
//                        about the caller's allocation rather than a bug in the
//                        composition. Report the fixable thing first.
static inline IGU32 igGtqWriteTerminator(IGU32 *dwords, IGU32 dwordCount,
                                         IGU32 usedDwords,
                                         IGU32 *outDwordsWritten)
{
    IGU32 need;

    if (outDwordsWritten != 0) { *outDwordsWritten = 0u; }

    if (dwords == 0) { return IG_GTQ_BATCH_NULL_SPAN; }

    need = igGtqTerminatorDwords(usedDwords);

    // The span must hold what is already there PLUS the terminator. Testing
    // only the terminator's own length would accept a span the packets already
    // overflowed.
    if (dwordCount < usedDwords || (dwordCount - usedDwords) < need) {
        return IG_GTQ_BATCH_SPAN_TOO_SHORT;
    }

    if (usedDwords == 0u) { return IG_GTQ_BATCH_EMPTY; }

    if ((usedDwords + need) > IG_GTQ_MAX_BATCH_DWORDS_POLICY) {
        return IG_GTQ_BATCH_TOO_LARGE;
    }

    dwords[usedDwords] = igGtMiBatchBufferEnd();
    if (need == 2u) {
        dwords[usedDwords + 1u] = igGtMiNoop();
    }

    if (outDwordsWritten != 0) { *outDwordsWritten = need; }
    return IG_GTQ_BATCH_OK;
}

// Is a composed batch actually terminated? Read rather than assumed, because
// the caller who composed it and the caller who dispatches it need not be the
// same code -- and the failure mode of dispatching an unterminated batch is the
// streamer running off the end of the allocation.
//
// This checks the LAST MEANINGFUL dword rather than scanning: a BB_END earlier
// in the buffer terminates it too, but everything after that point is dead, and
// a batch whose tail is dead is a composition bug this predicate should not
// bless. Trailing MI_NOOPs are skipped because the terminator itself emits one.
static inline bool igGtqBatchIsTerminated(const IGU32 *dwords, IGU32 usedDwords)
{
    IGU32 i;

    if (dwords == 0 || usedDwords == 0u) { return false; }

    i = usedDwords;
    while (i > 0u && dwords[i - 1u] == igGtMiNoop()) {
        i--;
    }

    if (i == 0u) { return false; }
    return dwords[i - 1u] == igGtMiBatchBufferEnd();
}

// ---------------------------------------------------------------------------
// Dispatch: the ring dwords that point the engine at a batch.
// ---------------------------------------------------------------------------

// Compose MI_BATCH_BUFFER_START plus its two address dwords into a RING span.
//
// The alignment refusal is DELEGATED to igGtBatchAddressOk() rather than
// restated here. Two spellings of one rule agree with each other until one of
// them moves, and this project has already paid for that -- the splitters in
// IntelGtCmd.hpp are the authority on what an address dword can hold.
static inline IGU32 igGtqWriteBatchStart(IGU32 *dwords, IGU32 dwordCount,
                                         IGU64 batchAddress,
                                         IGU32 *outDwordsWritten)
{
    if (outDwordsWritten != 0) { *outDwordsWritten = 0u; }

    if (dwords == 0) { return IG_GTQ_BATCH_NULL_SPAN; }

    // Span before address, same reasoning as the terminator: a short span is
    // what would store past the end of the caller's array.
    if (dwordCount < IG_GT_MI_BB_START_DWORDS_POLICY_3) {
        return IG_GTQ_BATCH_SPAN_TOO_SHORT;
    }

    if (!igGtBatchAddressOk(batchAddress)) {
        return IG_GTQ_BATCH_ADDR_UNALIGNED;
    }

    dwords[0] = igGtMiBatchBufferStart();
    dwords[1] = igGtBatchAddressLow(batchAddress);
    dwords[2] = igGtBatchAddressHigh(batchAddress);

    if (outDwordsWritten != 0) {
        *outDwordsWritten = IG_GT_MI_BB_START_DWORDS_POLICY_3;
    }
    return IG_GTQ_BATCH_OK;
}

// ---------------------------------------------------------------------------
// The whole join, in one call.
//
// Compose a solid-fill blit into a batch buffer and terminate it. This is the
// first function in this tree that produces a DISPATCHABLE unit of GPU work
// from a description of what the work is.
//
// The blitter's refusal is PROPAGATED rather than collapsed. A caller handed
// IG_GTQ_BATCH_UNTERMINATED for a rectangle whose pitch was zero would go
// looking in the wrong layer entirely, so the blit verdict is returned through
// an out-param and the batch verdict says only what the BATCH did.
// ---------------------------------------------------------------------------

static inline IGU32 igGtqBuildFillBatch(IGU32 *dwords, IGU32 dwordCount,
                                        IGU64 dstAddress, IGU32 dstPitchBytes,
                                        IGU32 left, IGU32 top,
                                        IGU32 right, IGU32 bottom,
                                        IGU32 color, IGU32 depthFlag,
                                        IGU32 *outBlitVerdict,
                                        IGU32 *outDwordsWritten)
{
    IGU32 blitVerdict;
    IGU32 blitWritten = 0u;
    IGU32 termWritten = 0u;
    IGU32 termVerdict;

    // Both out-params on EVERY path, including the ones that return before a
    // single argument is inspected.
    if (outBlitVerdict != 0)   { *outBlitVerdict = IG_GTB_BLIT_NOT_ATTEMPTED; }
    if (outDwordsWritten != 0) { *outDwordsWritten = 0u; }

    if (dwords == 0) { return IG_GTQ_BATCH_NULL_SPAN; }

    blitVerdict = igGtbWriteColorBlit(dwords, dwordCount,
                                      dstAddress, dstPitchBytes,
                                      left, top, right, bottom,
                                      color, depthFlag,
                                      &blitWritten);

    if (outBlitVerdict != 0) { *outBlitVerdict = blitVerdict; }

    if (!igGtbBlitSucceeded(blitVerdict)) {
        // The blit refused, so nothing was written and nothing is terminated.
        // The batch verdict is SPAN_TOO_SHORT only when that is what the blit
        // actually said; otherwise the batch itself is EMPTY, which is true --
        // no packet reached it.
        return IG_GTQ_BATCH_EMPTY;
    }

    termVerdict = igGtqWriteTerminator(dwords, dwordCount,
                                       blitWritten, &termWritten);

    if (!igGtqBatchSucceeded(termVerdict)) {
        // The packet is in the buffer but the terminator did not fit. Report
        // ZERO dwords written rather than the packet's count: a caller that
        // advanced a ring tail by the packet length here would dispatch a batch
        // with no MI_BATCH_BUFFER_END in it, which is the exact failure this
        // header exists to prevent.
        return termVerdict;
    }

    if (outDwordsWritten != 0) {
        *outDwordsWritten = blitWritten + termWritten;
    }
    return IG_GTQ_BATCH_OK;
}

#endif /* IntelGtBatch_hpp */
