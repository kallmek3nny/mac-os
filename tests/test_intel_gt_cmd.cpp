//
//  test_intel_gt_cmd.cpp
//  Drives phase0-kext/IntelGtCmd.hpp -- Gen12 (Tiger Lake) render-engine command
//  submission: engine bases, ring register composition, ring geometry verdicts,
//  head/tail wrap arithmetic and MI command word composition -- under MSVC on a
//  machine with no Intel GPU. Every function in that header is a total function
//  of its arguments, so there is no fixture, no journal and no register file here.
//
//  TWO RULES, same as tests/test_intel_pixel.cpp and tests/test_intel_scanout.cpp:
//
//    1. Every expected value is a BARE LITERAL. Checking a decode against the
//       same #define the decoder reads makes the constant self-cancelling -- the
//       fixture and the accessor move together and the check cannot fail.
//
//    2. No fixture, no journal, no simulated register file. The header touches no
//       hardware, so a test that builds one would be testing the test.
//
//  WHAT THIS SUITE DOES NOT ESTABLISH: not one line of IntelGtCmd.hpp has ever
//  executed on an Intel GPU. These are arithmetic and composition properties of
//  a pure header. The MI opcodes it composes are POLICY constants -- i915's MI
//  command family lives in gt/ headers that are NOT vendored in reference/i915/ --
//  so a green run here says the composition is self-consistent, never that the
//  words are the words the hardware wants.
//

#include <cstdio>

#include "IntelGtCmd.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %-56s got 0x%08X want 0x%08X\n", label, got, want);
    }
}

static void checkBool(const char *label, bool got, bool want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %-56s got %s want %s\n",
                    label, got ? "true" : "false", want ? "true" : "false");
    }
}

int main(void)
{
    std::printf("IntelGtCmd.hpp -- Gen12 render-engine command submission\n");
    std::printf("Engine bases and ring offsets cited to reference/i915/i915_reg.h.\n");
    std::printf("MI opcodes are POLICY -- i915 gt/ headers are not vendored here.\n");

    // -- Engine bases -----------------------------------------------------
    // Each arm returns its OWN base, and the four are pairwise distinct: a
    // copy-paste arm returning a sibling's base satisfies every "has a base"
    // assertion. Bare literals from reference/i915/i915_reg.h:289 and the
    // GEN11 block; a stride would land 0x1C0000 wrong.
    checkU32("engineBase(RENDER)", igGtEngineBase(0u), 0x02000u);
    checkU32("engineBase(BLT)", igGtEngineBase(1u), 0x22000u);
    checkU32("engineBase(BSD)", igGtEngineBase(2u), 0x1C0000u);
    checkU32("engineBase(VEBOX)", igGtEngineBase(3u), 0x1C8000u);

    checkBool("render != blt", igGtEngineBase(0u) == igGtEngineBase(1u), false);
    checkBool("render != bsd", igGtEngineBase(0u) == igGtEngineBase(2u), false);
    checkBool("render != vebox", igGtEngineBase(0u) == igGtEngineBase(3u), false);
    checkBool("blt != bsd", igGtEngineBase(1u) == igGtEngineBase(2u), false);
    checkBool("blt != vebox", igGtEngineBase(1u) == igGtEngineBase(3u), false);
    checkBool("bsd != vebox", igGtEngineBase(2u) == igGtEngineBase(3u), false);

    // The value one past the family answers the sentinel -- the tripwire that
    // forces whoever adds a fifth engine to come back and extend the switch.
    checkU32("engineBase(LIMIT) refuses", igGtEngineBase(4u), 0xFFFFFFFFu);
    checkU32("engineBase(5) refuses", igGtEngineBase(5u), 0xFFFFFFFFu);
    checkU32("engineBase(all-ones) refuses", igGtEngineBase(0xFFFFFFFFu), 0xFFFFFFFFu);

    // -- Ring register composition ----------------------------------------
    // PRB0_BASE is spelled (0x2030 - 0x30) in i915_reg.h, which is why the ring
    // block begins +0x30 above the engine base rather than at it.
    checkU32("ringTail(RENDER)", igGtRingTailReg(0u), 0x02030u);
    checkU32("ringHead(RENDER)", igGtRingHeadReg(0u), 0x02034u);
    checkU32("ringStart(RENDER)", igGtRingStartReg(0u), 0x02038u);
    checkU32("ringCtl(RENDER)", igGtRingCtlReg(0u), 0x0203Cu);

    checkU32("ringTail(BLT)", igGtRingTailReg(1u), 0x22030u);
    checkU32("ringHead(BLT)", igGtRingHeadReg(1u), 0x22034u);
    checkU32("ringStart(BSD)", igGtRingStartReg(2u), 0x1C0038u);
    checkU32("ringCtl(VEBOX)", igGtRingCtlReg(3u), 0x1C803Cu);

    // A composer must REFUSE off an invalid base rather than add 0x30 to the
    // sentinel: 0xFFFFFFFF + 0x30 wraps to 0x2F, a plausible low register.
    checkU32("ringTail(LIMIT) refuses", igGtRingTailReg(4u), 0xFFFFFFFFu);
    checkU32("ringHead(LIMIT) refuses", igGtRingHeadReg(4u), 0xFFFFFFFFu);
    checkU32("ringStart(LIMIT) refuses", igGtRingStartReg(4u), 0xFFFFFFFFu);
    checkU32("ringCtl(LIMIT) refuses", igGtRingCtlReg(4u), 0xFFFFFFFFu);
    checkU32("ringReg(LIMIT,0x30) refuses", igGtRingReg(4u, 0x30u), 0xFFFFFFFFu);

    // -- Page alignment ----------------------------------------------------
    checkBool("pageAligned(0)", igGtIsPageAligned(0ull), true);
    checkBool("pageAligned(0x1000)", igGtIsPageAligned(0x1000ull), true);
    checkBool("pageAligned(0x1)", igGtIsPageAligned(0x1ull), false);
    checkBool("pageAligned(0xFFF)", igGtIsPageAligned(0xFFFull), false);
    checkBool("pageAligned(0x1001)", igGtIsPageAligned(0x1001ull), false);
    // Above 32 bits: a helper that truncated to IGU32 would call this aligned.
    checkBool("pageAligned(0x1_0000_0800)", igGtIsPageAligned(0x100000800ull), false);
    checkBool("pageAligned(0x1_0000_0000)", igGtIsPageAligned(0x100000000ull), true);

    // -- Ring size verdicts ------------------------------------------------
    // Each bound is pinned SEPARATELY, and the arm order is load-bearing: zero
    // is page-aligned, so the alignment test must run FIRST for zero to reach
    // the too-small arm rather than being refused as unaligned.
    checkU32("ringSize(0x1000) ok", igGtRingSizeVerdict(0x1000u), 1u);
    checkU32("ringSize(0x4000) ok", igGtRingSizeVerdict(0x4000u), 1u);
    checkU32("ringSize(0x200000) ok", igGtRingSizeVerdict(0x200000u), 1u);
    checkU32("ringSize(0x800) unaligned", igGtRingSizeVerdict(0x800u), 3u);
    checkU32("ringSize(0x1001) unaligned", igGtRingSizeVerdict(0x1001u), 3u);
    checkU32("ringSize(0) too small", igGtRingSizeVerdict(0u), 4u);
    checkU32("ringSize(0x201000) too large", igGtRingSizeVerdict(0x201000u), 5u);

    // -- Free space --------------------------------------------------------
    // HEAD == TAIL is EMPTY, never FULL, and the wrap reserve is subtracted so
    // a submission can never advance TAIL onto HEAD.
    checkU32("free(head==tail==0)", igGtRingFreeBytes(0u, 0u, 0x1000u), 0x1000u - 0x40u);
    checkU32("free(head==tail==0x800)", igGtRingFreeBytes(0x800u, 0x800u, 0x1000u),
             0x1000u - 0x40u);
    // TAIL ahead of HEAD: the gap behind TAIL plus the gap in front of HEAD.
    checkU32("free(head 0, tail 0x100)", igGtRingFreeBytes(0u, 0x100u, 0x1000u),
             0x1000u - 0x100u - 0x40u);
    // HEAD ahead of TAIL: only the span between them, minus the reserve.
    checkU32("free(head 0x800, tail 0x100)", igGtRingFreeBytes(0x800u, 0x100u, 0x1000u),
             0x800u - 0x100u - 0x40u);
    // Out-of-ring inputs answer zero rather than an underflowed huge number.
    checkU32("free(head out of ring)", igGtRingFreeBytes(0x2000u, 0u, 0x1000u), 0u);
    checkU32("free(tail out of ring)", igGtRingFreeBytes(0u, 0x2000u, 0x1000u), 0u);
    checkU32("free(ring 0)", igGtRingFreeBytes(0u, 0u, 0u), 0u);

    // -- Tail advance ------------------------------------------------------
    checkU32("advance(0 + 0x40, ring 0x1000)", igGtRingAdvance(0u, 0x40u, 0x1000u), 0x40u);
    checkU32("advance wraps to 0", igGtRingAdvance(0xFC0u, 0x40u, 0x1000u), 0u);
    checkU32("advance wraps past 0", igGtRingAdvance(0xFC0u, 0x80u, 0x1000u), 0x40u);
    // An out-of-ring tail returns the ring size, NOT a modular answer: a wrapped
    // result from a bad input is indistinguishable from a good one.
    checkU32("advance(tail out of ring)", igGtRingAdvance(0x2000u, 0x40u, 0x1000u), 0x1000u);
    checkU32("advance(ring 0)", igGtRingAdvance(0u, 0x40u, 0u), 0u);

    // -- Submission verdicts -----------------------------------------------
    // The happy case first, so every refusal below is a refusal of something
    // that would otherwise be accepted.
    checkU32("submit ok", igGtSubmitVerdict(0u, 0x1000u, 0u, 0u, 0x40u), 1u);
    checkBool("admissible(OK)", igGtSubmitAdmissible(1u), true);
    // Admissibility is an EQUALITY against OK, never a negation of one error:
    // a negation is true for every OTHER refusal, including ones added later.
    checkBool("admissible(NOT_ATTEMPTED)", igGtSubmitAdmissible(0u), false);
    checkBool("admissible(BAD_ENGINE)", igGtSubmitAdmissible(2u), false);
    checkBool("admissible(NO_SPACE)", igGtSubmitAdmissible(11u), false);
    checkBool("admissible(LIMIT)", igGtSubmitAdmissible(12u), false);

    checkU32("submit bad engine", igGtSubmitVerdict(4u, 0x1000u, 0u, 0u, 0x40u), 2u);
    checkU32("submit ring unaligned", igGtSubmitVerdict(0u, 0x1800u, 0u, 0u, 0x40u), 3u);
    checkU32("submit ring too small", igGtSubmitVerdict(0u, 0u, 0u, 0u, 0x40u), 4u);
    checkU32("submit ring too large", igGtSubmitVerdict(0u, 0x201000u, 0u, 0u, 0x40u), 5u);
    checkU32("submit head out of ring", igGtSubmitVerdict(0u, 0x1000u, 0x1000u, 0u, 0x40u), 6u);
    checkU32("submit tail out of ring", igGtSubmitVerdict(0u, 0x1000u, 0u, 0x1000u, 0x40u), 7u);
    checkU32("submit tail unaligned", igGtSubmitVerdict(0u, 0x1000u, 0u, 0x4u, 0x40u), 8u);
    checkU32("submit empty", igGtSubmitVerdict(0u, 0x1000u, 0u, 0u, 0u), 9u);
    checkU32("submit unaligned length", igGtSubmitVerdict(0u, 0x1000u, 0u, 0u, 0x42u), 10u);
    checkU32("submit no space", igGtSubmitVerdict(0u, 0x1000u, 0u, 0u, 0x1000u), 11u);

    // ARM ORDER, pinned by inputs that are bad in two ways at once. Without
    // these, a reordered chain returns a verdict that is true about the input
    // and names the wrong defect -- and the wrong defect is the one nobody can
    // act on.
    //
    // A bad engine beside a nonsense ring reports the ENGINE.
    checkU32("order: engine beats ring", igGtSubmitVerdict(4u, 0x1800u, 0u, 0u, 0x40u), 2u);
    // A head outside a ring that is itself unaligned reports the RING: the ring
    // is the thing an author can fix, and the head is only "out" relative to a
    // size that was never legal.
    checkU32("order: ring beats head", igGtSubmitVerdict(0u, 0x1800u, 0x9000u, 0u, 0x40u), 3u);
    checkU32("order: ring beats tail", igGtSubmitVerdict(0u, 0u, 0u, 0x9000u, 0x40u), 4u);
    // An unaligned tail that is ALSO out of the ring reports out-of-ring first:
    // alignment of a value outside the buffer is not a meaningful complaint.
    checkU32("order: tail range beats tail align",
             igGtSubmitVerdict(0u, 0x1000u, 0u, 0x1004u, 0x40u), 7u);
    // Zero length reports EMPTY, not UNALIGNED_LENGTH. Zero is trivially a
    // multiple of the command word, so a multiple-first order makes the EMPTY
    // arm unreachable -- the dead-arm shape 3050-handoff32 section 3 exists to
    // remove, reintroduced one subsystem over.
    checkU32("order: empty beats length align",
             igGtSubmitVerdict(0u, 0x1000u, 0u, 0u, 0u), 9u);
    // A length that is both unaligned and larger than the ring reports the
    // alignment: it is a defect in the caller's command stream, while no-space
    // is a fact about how full the ring happens to be right now.
    checkU32("order: length align beats no space",
             igGtSubmitVerdict(0u, 0x1000u, 0u, 0u, 0x4002u), 10u);

    // -- MI command words ---------------------------------------------------
    // POLICY constants: i915's MI command family lives in gt/ headers that are
    // not vendored in reference/i915/, so these pin the COMPOSITION -- client 0
    // in bits 31:29, opcode in bits 28:23, length in the low byte -- and say
    // nothing about whether the hardware wants these opcodes.
    checkU32("miHeader(0x00, 0)", igGtMiHeader(0x00u, 0u), 0x00000000u);
    checkU32("miHeader(0x0A, 0)", igGtMiHeader(0x0Au, 0u), 0x05000000u);
    checkU32("miHeader(0x31, 1)", igGtMiHeader(0x31u, 1u), 0x18800001u);
    checkU32("miHeader(0x20, 2)", igGtMiHeader(0x20u, 2u), 0x10000002u);
    // The opcode is masked to 6 bits and the length to 8, so an over-wide
    // argument cannot bleed into the client field above it.
    checkU32("miHeader opcode masked", igGtMiHeader(0x7Fu, 0u), 0x1F800000u);
    checkU32("miHeader length masked", igGtMiHeader(0x00u, 0x1FFu), 0x000000FFu);

    checkU32("miNoop", igGtMiNoop(), 0x00000000u);
    checkU32("miBatchBufferEnd", igGtMiBatchBufferEnd(), 0x05000000u);
    checkU32("miBatchBufferStart", igGtMiBatchBufferStart(), 0x18800001u);
    // NOOP and BB_END must not compose to the same word -- a shared value would
    // make a ring padded with NOOPs terminate wherever the padding began.
    checkBool("noop != bbEnd", igGtMiNoop() == igGtMiBatchBufferEnd(), false);
    checkBool("noop != bbStart", igGtMiNoop() == igGtMiBatchBufferStart(), false);
    checkBool("bbEnd != bbStart",
              igGtMiBatchBufferEnd() == igGtMiBatchBufferStart(), false);

    // BB_START is three dwords: the header plus a 64-bit address.
    checkU32("batchStartBytes", igGtBatchStartBytes(), 12u);

    // -- Batch buffer address ----------------------------------------------
    checkBool("batchAddrOk(0)", igGtBatchAddressOk(0ull), true);
    checkBool("batchAddrOk(0x4)", igGtBatchAddressOk(0x4ull), true);
    checkBool("batchAddrOk(0x1)", igGtBatchAddressOk(0x1ull), false);
    checkBool("batchAddrOk(0x2)", igGtBatchAddressOk(0x2ull), false);
    checkBool("batchAddrOk(0x3)", igGtBatchAddressOk(0x3ull), false);
    checkBool("batchAddrOk(0x1_0000_0004)", igGtBatchAddressOk(0x100000004ull), true);
    checkBool("batchAddrOk(0x1_0000_0002)", igGtBatchAddressOk(0x100000002ull), false);

    // The halves REFUSE an unaligned address rather than truncating it, because
    // the low bits are field space in the real encoding -- a truncated address
    // is a legal-looking word pointing two bytes away from the batch.
    checkU32("batchLow(0x1_2345_6780)", igGtBatchAddressLow(0x123456780ull), 0x23456780u);
    checkU32("batchHigh(0x1_2345_6780)", igGtBatchAddressHigh(0x123456780ull), 0x00000001u);
    checkU32("batchLow(0)", igGtBatchAddressLow(0ull), 0u);
    checkU32("batchHigh(0)", igGtBatchAddressHigh(0ull), 0u);
    checkU32("batchLow refuses unaligned", igGtBatchAddressLow(0x123456782ull), 0u);
    checkU32("batchHigh refuses unaligned", igGtBatchAddressHigh(0x123456782ull), 0u);

    // -- Verdict and engine names -------------------------------------------
    // Every verdict BELOW the family's own limit has a name, and the names are
    // pairwise distinct -- a copy-paste arm returning a sibling's string passes
    // every "has a name" assertion. The loop bounds on the limit rather than a
    // typed count, so a family that grows reddens on the commit that grows it.
    for (IGU32 v = 0u; v < IG_GT_SUBMIT_LIMIT; v++) {
        gChecks++;
        const char *name = igGtSubmitVerdictName(v);
        if (name == nullptr || name[0] == '\0') {
            gFailures++;
            std::printf("FAIL verdict %u has no name\n", v);
            continue;
        }
        for (IGU32 w = 0u; w < v; w++) {
            if (igGtSubmitVerdictName(w) == name) {
                gFailures++;
                std::printf("FAIL verdict %u shares a name with %u\n", v, w);
                break;
            }
        }
    }
    // The value one past the family answers the sentinel, so a verdict added to
    // the header with no arm in the switch is caught here rather than published.
    {
        const char *past = igGtSubmitVerdictName(IG_GT_SUBMIT_LIMIT);
        checkBool("verdictName(LIMIT) is the sentinel",
                  past == igGtSubmitVerdictName(IG_GT_SUBMIT_LIMIT + 1u), true);
        checkBool("verdictName(LIMIT) != verdictName(OK)",
                  past == igGtSubmitVerdictName(IG_GT_SUBMIT_OK), false);
    }

    for (IGU32 e = 0u; e < IG_GT_ENGINE_LIMIT; e++) {
        gChecks++;
        const char *name = igGtEngineName(e);
        if (name == nullptr || name[0] == '\0') {
            gFailures++;
            std::printf("FAIL engine %u has no name\n", e);
            continue;
        }
        for (IGU32 f = 0u; f < e; f++) {
            if (igGtEngineName(f) == name) {
                gFailures++;
                std::printf("FAIL engine %u shares a name with %u\n", e, f);
                break;
            }
        }
    }
    checkBool("engineName(LIMIT) is the sentinel",
              igGtEngineName(IG_GT_ENGINE_LIMIT) == igGtEngineName(IG_GT_ENGINE_LIMIT + 1u),
              true);
    checkBool("engineName(LIMIT) != engineName(RENDER)",
              igGtEngineName(IG_GT_ENGINE_LIMIT) == igGtEngineName(0u), false);

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
