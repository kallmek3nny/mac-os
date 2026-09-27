//
//  test_intel_gt_exec.cpp
//  Drives phase0-kext/IntelGtExec.hpp -- Gen12 (Tiger Lake) EXECLIST submission
//  and GTT address composition -- under MSVC on a machine with no Intel GPU.
//  Every function in that header is a total function of its arguments, so there
//  is no fixture, no journal and no simulated register file here.
//
//  TWO RULES, same as tests/test_intel_gt_cmd.cpp, tests/test_intel_pixel.cpp
//  and tests/test_intel_scanout.cpp:
//
//    1. Every expected value is a BARE LITERAL. Checking a decode against the
//       same #define the decoder reads makes the constant self-cancelling --
//       the fixture and the accessor move together and the check cannot fail.
//
//    2. No fixture, no journal, no simulated register file. The header touches
//       no hardware, so a test that builds one would be testing the test.
//
//  WHAT THIS SUITE DOES NOT ESTABLISH, AND IT IS MORE THAN USUAL: every single
//  constant in IntelGtExec.hpp is POLICY. reference/i915/ holds seven files and
//  all seven are display -- greps for EXECLIST, ELSP, LRC, CTX_DESC, GEN8_PDE,
//  GEN8_PTE, PPAT and GGTT across that directory return ZERO matches. i915 keeps
//  the execlist port, the context descriptor layout and the page-table entry
//  format in gt/ headers that were never vendored here. So unlike its sibling
//  IntelGtCmd.hpp -- which has a cited half -- this header has none at all, and
//  a green run says the composition is self-consistent and says NOTHING about
//  whether the hardware wants these words.
//

#include <cstdio>

#include "IntelGtExec.hpp"

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

static void checkU64(const char *label, IGU64 got, IGU64 want)
{
    gChecks++;
    if (got != want) {
        gFailures++;
        std::printf("FAIL %-56s got 0x%016llX want 0x%016llX\n",
                    label,
                    (unsigned long long)got,
                    (unsigned long long)want);
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
    std::printf("IntelGtExec.hpp -- Gen12 execlist submission and GTT entries\n");
    std::printf("EVERY constant here is POLICY: i915 gt/ headers are not vendored.\n");
    std::printf("A green run proves composition, never that the hardware agrees.\n");

    // -- Execlist port register composition ---------------------------------
    // Bare literals: engine base plus the policy offset. A composer that added
    // the offset to a sentinel base rather than refusing would land on 0x50F,
    // a legal-looking low register.
    checkU32("execlistStatus(RENDER)", igGtxExeclistStatusReg(0u), 0x02234u);
    checkU32("execlistSq(RENDER)", igGtxExeclistSqReg(0u), 0x02510u);
    checkU32("execlistControl(RENDER)", igGtxExeclistControlReg(0u), 0x02550u);

    checkU32("execlistStatus(BLT)", igGtxExeclistStatusReg(1u), 0x22234u);
    checkU32("execlistSq(BSD)", igGtxExeclistSqReg(2u), 0x1C0510u);
    checkU32("execlistControl(VEBOX)", igGtxExeclistControlReg(3u), 0x1C8550u);

    checkU32("execlistStatus(LIMIT) refuses", igGtxExeclistStatusReg(4u), 0xFFFFFFFFu);
    checkU32("execlistSq(LIMIT) refuses", igGtxExeclistSqReg(4u), 0xFFFFFFFFu);
    checkU32("execlistControl(LIMIT) refuses", igGtxExeclistControlReg(4u), 0xFFFFFFFFu);
    checkU32("execlistSq(all-ones) refuses", igGtxExeclistSqReg(0xFFFFFFFFu), 0xFFFFFFFFu);

    // The three ports are pairwise distinct -- a copy-paste composer returning
    // a sibling's offset satisfies every "has an address" assertion.
    checkBool("status != sq",
              igGtxExeclistStatusReg(0u) == igGtxExeclistSqReg(0u), false);
    checkBool("status != control",
              igGtxExeclistStatusReg(0u) == igGtxExeclistControlReg(0u), false);
    checkBool("sq != control",
              igGtxExeclistSqReg(0u) == igGtxExeclistControlReg(0u), false);

    // -- Port busy polarity --------------------------------------------------
    // The dangerous half of this header. Reading the bit backwards means
    // overwriting a submit queue the scheduler is still consuming.
    checkBool("portBusy(0)", igGtxExeclistPortBusy(0u), false);
    checkBool("portBusy(1)", igGtxExeclistPortBusy(1u), true);
    checkBool("portBusy(0x2)", igGtxExeclistPortBusy(0x2u), false);
    checkBool("portBusy(0x3)", igGtxExeclistPortBusy(0x3u), true);
    // A REFUSED MMIO read is 0xFFFFFFFF and must report BUSY -- the safe
    // direction, because busy means "do not write the queue". A predicate
    // testing for the clear state would call a refused read idle.
    checkBool("portBusy(refused read)", igGtxExeclistPortBusy(0xFFFFFFFFu), true);

    // -- Context descriptor composition --------------------------------------
    // Bare literals throughout: valid bit 0, LRCA at its natural position in
    // bits 31:12, software id above bit 32.
    checkU64("ctxDesc(0x1000, 1)", igGtxContextDescriptor(0x1000ull, 1u),
             0x0000000100001001ull);
    checkU64("ctxDesc(0, 0)", igGtxContextDescriptor(0ull, 0u),
             0x0000000000000001ull);
    checkU64("ctxDesc(0xFFFFF000, 0x7FF)",
             igGtxContextDescriptor(0xFFFFF000ull, 0x7FFu),
             0x000007FFFFFFF001ull);

    // Every refusal answers ZERO rather than all-ones, because bit 0 is the
    // valid bit -- all-ones would be a VALID descriptor over a garbage address.
    checkU64("ctxDesc refuses unaligned", igGtxContextDescriptor(0x1001ull, 1u), 0ull);
    checkU64("ctxDesc refuses 0x800", igGtxContextDescriptor(0x800ull, 1u), 0ull);
    checkU64("ctxDesc refuses above LRCA field",
             igGtxContextDescriptor(0x100000000ull, 1u), 0ull);
    checkU64("ctxDesc refuses id 0x800",
             igGtxContextDescriptor(0x1000ull, 0x800u), 0ull);

    // ARM ORDER, pinned by an address bad in TWO ways at once: above the field
    // AND unaligned. Both refuse to zero, so the order is not observable from
    // the descriptor -- it is observable from the predicates, which is why both
    // are pinned separately below.
    checkBool("lrcaInRange(0x100000004)", igGtxLrcaInRange(0x100000004ull), false);
    checkBool("lrcaAligned(0x100000004)", igGtxLrcaAligned(0x100000004ull), false);
    checkU64("ctxDesc refuses range+align",
             igGtxContextDescriptor(0x100000004ull, 1u), 0ull);

    checkBool("lrcaInRange(0xFFFFF000)", igGtxLrcaInRange(0xFFFFF000ull), true);
    checkBool("lrcaInRange(0xFFFFF001)", igGtxLrcaInRange(0xFFFFF001ull), false);
    checkBool("lrcaAligned(0)", igGtxLrcaAligned(0ull), true);
    checkBool("lrcaAligned(0xFFF)", igGtxLrcaAligned(0xFFFull), false);
    // Above 32 bits: a helper truncating to IGU32 would call this aligned.
    checkBool("lrcaAligned(0x1_0000_0800)", igGtxLrcaAligned(0x100000800ull), false);
    checkBool("lrcaAligned(0x1_0000_0000)", igGtxLrcaAligned(0x100000000ull), true);

    checkBool("contextIdFits(0)", igGtxContextIdFits(0u), true);
    checkBool("contextIdFits(0x7FF)", igGtxContextIdFits(0x7FFu), true);
    checkBool("contextIdFits(0x800)", igGtxContextIdFits(0x800u), false);
    checkBool("contextIdFits(all-ones)", igGtxContextIdFits(0xFFFFFFFFu), false);

    checkBool("descValid(composed)",
              igGtxContextDescriptorValid(igGtxContextDescriptor(0x1000ull, 1u)), true);
    checkBool("descValid(0)", igGtxContextDescriptorValid(0ull), false);

    // -- Descriptor halves ----------------------------------------------------
    // Both halves answer zero for an invalid descriptor rather than splitting
    // garbage, so a caller ignoring the validity flag writes an all-zero
    // (invalid) queue entry instead of a plausible one.
    {
        IGU64 desc = igGtxContextDescriptor(0x1000ull, 1u);
        checkU32("descLow(0x1000, 1)", igGtxDescriptorLow(desc), 0x00001001u);
        checkU32("descHigh(0x1000, 1)", igGtxDescriptorHigh(desc), 0x00000001u);
    }
    checkU32("descLow refuses invalid", igGtxDescriptorLow(0ull), 0u);
    checkU32("descHigh refuses invalid", igGtxDescriptorHigh(0ull), 0u);
    // An all-ones word HAS the valid bit set, so the halves must split it --
    // this pins that the refusal is keyed on the valid bit and not on a
    // hand-rolled sentinel comparison.
    checkU32("descLow(all-ones) splits", igGtxDescriptorLow(0xFFFFFFFFFFFFFFFFull),
             0xFFFFFFFFu);
    checkU32("descHigh(all-ones) splits", igGtxDescriptorHigh(0xFFFFFFFFFFFFFFFFull),
             0xFFFFFFFFu);

    // -- Page-table entries ---------------------------------------------------
    checkU64("pte(0x1000, present)", igGtxPageTableEntry(0x1000ull, true),
             0x0000000000001001ull);
    checkU64("pte(0x1_0000_0000, present)",
             igGtxPageTableEntry(0x100000000ull, true), 0x0000000100000001ull);
    // Absent answers ZERO, not the address with a clear bit: bit 0 is the
    // present bit, so zero is NOT PRESENT by the hardware's own definition.
    checkU64("pte(0x1000, absent)", igGtxPageTableEntry(0x1000ull, false), 0ull);
    checkU64("pte refuses unaligned", igGtxPageTableEntry(0x1001ull, true), 0ull);
    checkU64("pte refuses 0x800", igGtxPageTableEntry(0x800ull, true), 0ull);
    // Above the 36-bit page-frame field.
    checkU64("pte refuses above ceiling",
             igGtxPageTableEntry(0x0001000000000000ull, true), 0ull);
    checkU64("pte accepts the ceiling",
             igGtxPageTableEntry(0x0000FFFFFFFFF000ull, true),
             0x0000FFFFFFFFF001ull);

    checkBool("pageAddressOk(0)", igGtxPageAddressOk(0ull), true);
    checkBool("pageAddressOk(0x1000)", igGtxPageAddressOk(0x1000ull), true);
    checkBool("pageAddressOk(0xFFF)", igGtxPageAddressOk(0xFFFull), false);
    checkBool("pageAddressOk(0x1_0000_0800)", igGtxPageAddressOk(0x100000800ull), false);
    checkBool("pageAddressOk(ceiling)",
              igGtxPageAddressOk(0x0000FFFFFFFFF000ull), true);
    checkBool("pageAddressOk(above ceiling)",
              igGtxPageAddressOk(0x0001000000000000ull), false);

    checkBool("pteIsPresent(composed)",
              igGtxPteIsPresent(igGtxPageTableEntry(0x1000ull, true)), true);
    checkBool("pteIsPresent(0)", igGtxPteIsPresent(0ull), false);
    checkBool("pteIsPresent(0x1000)", igGtxPteIsPresent(0x1000ull), false);

    // An address recovered from a NON-PRESENT entry is a number nobody
    // programmed, and it looks exactly like one somebody did -- so the accessor
    // answers zero rather than masking the bits it was handed.
    checkU64("pteAddress(present)",
             igGtxPteAddress(igGtxPageTableEntry(0x1000ull, true)), 0x1000ull);
    checkU64("pteAddress(absent) refuses", igGtxPteAddress(0x1000ull), 0ull);
    checkU64("pteAddress(0) refuses", igGtxPteAddress(0ull), 0ull);

    // -- GTT entry offsets ----------------------------------------------------
    checkU32("gttOffset(0)", igGtxGttEntryOffset(0u), 0u);
    checkU32("gttOffset(1)", igGtxGttEntryOffset(1u), 8u);
    checkU32("gttOffset(0xFFFFF)", igGtxGttEntryOffset(0xFFFFFu), 0x7FFFF8u);
    checkU32("gttOffset(LIMIT) refuses", igGtxGttEntryOffset(0x100000u), 0xFFFFFFFFu);
    // The index is bounded BEFORE the multiply. 0x20000000 * 8 wraps to 0 in 32
    // bits, and a wrapped offset is small, aligned and entirely plausible.
    checkU32("gttOffset(0x20000000) refuses",
             igGtxGttEntryOffset(0x20000000u), 0xFFFFFFFFu);
    checkU32("gttOffset(all-ones) refuses",
             igGtxGttEntryOffset(0xFFFFFFFFu), 0xFFFFFFFFu);

    // -- Submission readiness -------------------------------------------------
    // The happy case FIRST, so every refusal below is a refusal of something
    // that would otherwise be accepted.
    checkU32("ready ok",
             igGtxSubmitReady(0u, 0x1000ull, 1u, 0x1000u, 0u, 0u, 0x40u, 0u), 1u);
    checkBool("admissible(OK)", igGtxSubmitAdmissible(1u), true);
    // Admissibility is an EQUALITY against OK, never a negation of one error:
    // a negation is true for every OTHER refusal, including later additions.
    checkBool("admissible(NOT_ATTEMPTED)", igGtxSubmitAdmissible(0u), false);
    checkBool("admissible(BAD_ENGINE)", igGtxSubmitAdmissible(2u), false);
    checkBool("admissible(PORT_BUSY)", igGtxSubmitAdmissible(7u), false);
    checkBool("admissible(LIMIT)", igGtxSubmitAdmissible(8u), false);

    checkU32("ready bad engine",
             igGtxSubmitReady(4u, 0x1000ull, 1u, 0x1000u, 0u, 0u, 0x40u, 0u), 2u);
    checkU32("ready lrca range",
             igGtxSubmitReady(0u, 0x100000000ull, 1u, 0x1000u, 0u, 0u, 0x40u, 0u), 3u);
    checkU32("ready lrca unaligned",
             igGtxSubmitReady(0u, 0x1004ull, 1u, 0x1000u, 0u, 0u, 0x40u, 0u), 4u);
    checkU32("ready context id",
             igGtxSubmitReady(0u, 0x1000ull, 0x800u, 0x1000u, 0u, 0u, 0x40u, 0u), 5u);
    checkU32("ready ring refused",
             igGtxSubmitReady(0u, 0x1000ull, 1u, 0x1800u, 0u, 0u, 0x40u, 0u), 6u);
    checkU32("ready port busy",
             igGtxSubmitReady(0u, 0x1000ull, 1u, 0x1000u, 0u, 0u, 0x40u, 1u), 7u);
    // A refused port-status READ is all-ones, which has the busy bit set, so it
    // must land on PORT_BUSY rather than being read as idle.
    checkU32("ready port status refused",
             igGtxSubmitReady(0u, 0x1000ull, 1u, 0x1000u, 0u, 0u, 0x40u, 0xFFFFFFFFu),
             7u);

    // ARM ORDER, pinned by inputs bad in TWO ways at once. Without these, a
    // reordered chain returns a verdict that is true about the input and names
    // the wrong defect -- and the wrong defect is the one nobody can act on.
    //
    // A bad engine beside a nonsense ring reports the ENGINE.
    checkU32("order: engine beats ring",
             igGtxSubmitReady(4u, 0x1000ull, 1u, 0x1800u, 0u, 0u, 0x40u, 0u), 2u);
    // An LRCA above the field that is ALSO unaligned reports the RANGE: the
    // alignment of an address the field cannot hold is not a meaningful
    // complaint about it.
    checkU32("order: lrca range beats align",
             igGtxSubmitReady(0u, 0x100000004ull, 1u, 0x1000u, 0u, 0u, 0x40u, 0u), 3u);
    // A bad context id beside a nonsense ring reports the CONTEXT ID: the
    // context is judged whole before the ring it would run from.
    checkU32("order: context id beats ring",
             igGtxSubmitReady(0u, 0x1000ull, 0x800u, 0x1800u, 0u, 0u, 0x40u, 0u), 5u);
    // THE LOAD-BEARING ONE: a refused ring beside a BUSY port reports the RING.
    // A ring geometry defect is a bug in this driver; a busy port is a fact
    // about the machine one millisecond from now. Report the one somebody can
    // go and fix -- handoff22 section 1b's rule applied to a transient.
    checkU32("order: ring beats port busy",
             igGtxSubmitReady(0u, 0x1000ull, 1u, 0x1800u, 0u, 0u, 0x40u, 1u), 6u);
    // And the engine outranks both.
    checkU32("order: engine beats port busy",
             igGtxSubmitReady(4u, 0x1000ull, 1u, 0x1000u, 0u, 0u, 0x40u, 1u), 2u);

    // -- Verdict names --------------------------------------------------------
    // Every verdict BELOW the family's own limit has a name, and the names are
    // pairwise distinct -- a copy-paste arm returning a sibling's string passes
    // every "has a name" assertion. The loop bounds on the limit rather than a
    // typed count, so a family that grows reddens on the commit that grows it.
    for (IGU32 v = 0u; v < IG_GTX_READY_LIMIT; v++) {
        gChecks++;
        const char *name = igGtxReadyVerdictName(v);
        if (name == nullptr || name[0] == '\0') {
            gFailures++;
            std::printf("FAIL verdict %u has no name\n", v);
            continue;
        }
        for (IGU32 w = 0u; w < v; w++) {
            if (igGtxReadyVerdictName(w) == name) {
                gFailures++;
                std::printf("FAIL verdict %u shares a name with %u\n", v, w);
                break;
            }
        }
    }
    // The value one past the family answers the sentinel, so a verdict added to
    // the header with no arm in the switch is caught here rather than published.
    {
        const char *past = igGtxReadyVerdictName(IG_GTX_READY_LIMIT);
        checkBool("verdictName(LIMIT) is the sentinel",
                  past == igGtxReadyVerdictName(IG_GTX_READY_LIMIT + 1u), true);
        checkBool("verdictName(LIMIT) != verdictName(OK)",
                  past == igGtxReadyVerdictName(IG_GTX_READY_OK), false);
    }

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
