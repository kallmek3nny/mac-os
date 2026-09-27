//
//  test_decode.cpp
//  Host-side tests for NVDecode.hpp.
//
//  Builds and runs on Windows with MSVC. No IOKit, no GPU, no macOS. It tests
//  the one part of the kext that is pure logic -- and that is exactly the part
//  that has actually been wrong twice in this project.
//
//  Run:  cmake -S tests -B tests/build -G Ninja && cmake --build tests/build
//        tests/build/test_decode.exe
//

#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVDecode.hpp"

static int gChecks = 0;
static int gFailures = 0;
static const char *gSection = "";

static void section(const char *name)
{
    gSection = name;
    std::printf("\n%s\n", name);
    for (size_t i = 0; i < std::strlen(name); ++i)
        std::printf("-");
    std::printf("\n");
}

static void checkU32(const char *label, NVU32 actual, NVU32 expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-46s 0x%X\n", label, actual);
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-46s got 0x%X, expected 0x%X\n", label, actual, expected);
}

static void checkBool(const char *label, bool actual, bool expected)
{
    gChecks++;
    if (actual == expected) {
        std::printf("  ok    %-46s %s\n", label, actual ? "true" : "false");
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-46s got %s, expected %s\n",
                label, actual ? "true" : "false", expected ? "true" : "false");
}

static void checkStr(const char *label, const char *actual, const char *expected)
{
    gChecks++;
    if (std::strcmp(actual, expected) == 0) {
        std::printf("  ok    %-46s \"%s\"\n", label, actual);
        return;
    }
    gFailures++;
    std::printf("  FAIL  %-46s got \"%s\", expected \"%s\"\n",
                label, actual, expected);
}

//
// A realistic GA107 NV_PMC_BOOT_0. Architecture 0x17 occupies bits 28:24 with
// bit 8 clear, implementation 0x7 at 23:20, revision a1 in the low byte.
//
static const NVU32 kBoot0_GA107  = 0x177000A1u;
static const NVU32 kBoot42_GA107 = 0x177010A1u;

static void testBoot0(void)
{
    section("NV_PMC_BOOT_0 decode (GA107)");

    checkU32("architecture", nvBoot0Architecture(kBoot0_GA107), NV_ARCHITECTURE_GA100);
    checkU32("implementation", nvBoot0Implementation(kBoot0_GA107), NV_IMPLEMENTATION_GA107);
    checkU32("major revision", nvBoot0MajorRevision(kBoot0_GA107), 0xA);
    checkU32("minor revision", nvBoot0MinorRevision(kBoot0_GA107), 0x1);
    checkStr("architecture name",
             nvArchitectureName(nvBoot0Architecture(kBoot0_GA107)), "Ampere");
    checkStr("implementation name",
             nvAmpereImplementationName(nvBoot0Implementation(kBoot0_GA107)), "GA107");
}

//
// THE REGRESSION TEST THAT MATTERS.
//
// The first version of this decoder read architecture from bits 28:24 only.
// That gives the right answer for Ampere purely because bit 8 happens to be
// zero -- so the bug was invisible on the exact hardware being targeted, and
// would have surfaced only on a part where bit 8 is set.
//
// Here we synthesise a value with ARCHITECTURE_1 set and confirm the high bit
// is actually folded in. A single-range decoder fails this and passes
// everything else in the file.
//
static void testSplitArchitecture(void)
{
    section("split ARCHITECTURE field (the bug that hid in plain sight)");

    // arch = 0x17 with bit 8 clear -> plain Ampere
    checkU32("bit8 clear: arch 0x17",
             nvBoot0Architecture(0x17000000u), 0x17u);

    // Same low bits, bit 8 SET -> arch must gain 0x20
    checkU32("bit8 set:   arch 0x37",
             nvBoot0Architecture(0x17000100u), 0x37u);

    // Low field all ones, bit 8 set -> 0x3F, the widest representable value
    checkU32("max: low 0x1F + bit8 -> 0x3F",
             nvBoot0Architecture(0x1F000100u), 0x3Fu);

    // The reassembly must not overlap or leave a gap: the mask has to be
    // exactly (1 << WIDTH) - 1, or the high bit lands in the wrong place.
    checkU32("ARCHITECTURE_0_MASK matches WIDTH",
             NV_PMC_BOOT_0_ARCHITECTURE_0_MASK,
             (1u << NV_PMC_BOOT_0_ARCHITECTURE_0_WIDTH) - 1u);

    // BOOT_0 and BOOT_42 must agree about the architecture on the same part.
    checkU32("BOOT_0 and BOOT_42 agree",
             nvBoot0Architecture(kBoot0_GA107),
             nvBoot42Architecture(kBoot42_GA107));
}

static void testBoot42(void)
{
    section("NV_PMC_BOOT_42 decode (GA107)");

    checkU32("architecture", nvBoot42Architecture(kBoot42_GA107), NV_ARCHITECTURE_GA100);
    checkU32("implementation", nvBoot42Implementation(kBoot42_GA107), NV_IMPLEMENTATION_GA107);
    checkU32("chip id", nvBoot42ChipId(kBoot42_GA107), NV_CHIP_ID_GA107);
    checkU32("chip id literal is 0x177", NV_CHIP_ID_GA107, 0x177u);

    //
    // GA106 also sells under a "3050 Ti Laptop" name, so the decoder has to
    // separate 0x176 from 0x177 rather than just recognising "Ampere".
    //
    checkU32("GA106 chip id is 0x176", NV_CHIP_ID_GA106, 0x176u);
    checkBool("GA106 != GA107", NV_CHIP_ID_GA106 == NV_CHIP_ID_GA107, false);
}

//
// Separating inputs for the mask-width and shift mutants that kBoot0_GA107 and
// kBoot42_GA107 cannot tell apart.
//
// The sample values are real silicon, which is exactly why they are weak here:
// a field whose neighbouring bit happens to be 0 on GA107 reads the same
// through a mask one bit too wide, and a field whose top bit happens to be 0
// reads the same through a mask one bit too narrow. Both slips then survive
// every check in this file. The inputs below are synthetic on purpose -- each
// one sets exactly the bit that separates a correct decoder from a wrong one:
//
//   * bit N+1, the bit immediately ABOVE the field, which a WIDENED mask
//     swallows. It is usually undecoded, so it appears in no realistic sample.
//   * the field's own TOP bit, which a NARROWED mask drops.
//
// Three of these decoders -- nvBoot42MinorExtRevision, nvBoot42MinorRevision
// and nvBoot42MajorRevision -- had no call site anywhere in this suite before
// this section existed. They were compiled, never run. The all-field-set input
// pins their shift as well as their width.
//
static void testAdjacentBits(void)
{
    section("adjacent-bit separation (a mask one bit too wide is silent)");

    // --- NV_PMC_BOOT_0 -----------------------------------------------------

    // MINOR_REVISION is 3:0. Bit 4 belongs to MAJOR_REVISION.
    checkU32("bit 4 does not widen minor revision",
             nvBoot0MinorRevision(0x00000010u), 0x0u);
    checkU32("bit 3 IS minor revision's top bit",
             nvBoot0MinorRevision(0x00000008u), 0x8u);

    // MAJOR_REVISION is 7:4. Bit 8 is ARCHITECTURE_1.
    checkU32("bit 8 does not widen major revision",
             nvBoot0MajorRevision(0x00000100u), 0x0u);
    checkU32("bit 7 IS major revision's top bit",
             nvBoot0MajorRevision(0x00000080u), 0x8u);

    // IMPLEMENTATION is 23:20. Bit 23 is its own top bit; a narrowed mask
    // reports GA100 for a GA10x part, which is a plausible-looking wrong
    // answer rather than an obvious one.
    checkU32("bit 23 IS implementation's top bit",
             nvBoot0Implementation(0x00800000u), 0x8u);
    checkU32("bit 24 does not widen implementation",
             nvBoot0Implementation(0x01000000u), 0x0u);

    // ARCHITECTURE_1 is the single bit 8:8, folded in above the 5-bit low
    // half. Widen it to 8:9 and bit 9 arrives as 0x2, landing at 0x40 in the
    // reassembled architecture.
    checkU32("bit 9 does not widen the architecture high bit",
             nvBoot0Architecture(0x00000200u), 0x0u);
    checkU32("bit 8 IS the architecture high bit",
             nvBoot0Architecture(0x00000100u), 0x20u);

    // ARCHITECTURE_0 is 28:24, the low five bits of the reassembled field.
    // BOOT_0 stops at 28 where BOOT_42's architecture runs to 29, so a mask
    // copied across from BOOT_42 swallows bit 29 and reports an architecture
    // 0x20 too high. Neither slip is visible in kBoot0_GA107.
    checkU32("bit 29 does not widen the architecture low half",
             nvBoot0Architecture(0x20000000u), 0x0u);
    checkU32("bit 28 IS the architecture low half's top bit",
             nvBoot0Architecture(0x10000000u), 0x10u);

    // --- NV_PMC_BOOT_42 ----------------------------------------------------

    // MINOR_EXT_REVISION 11:8 -- no call site before this line.
    checkU32("minor ext revision reads its own four bits",
             nvBoot42MinorExtRevision(0x00000F00u), 0xFu);
    checkU32("bit 12 does not widen minor ext revision",
             nvBoot42MinorExtRevision(0x00001000u), 0x0u);

    // MINOR_REVISION 15:12 -- no call site before this line.
    checkU32("boot42 minor revision reads its own four bits",
             nvBoot42MinorRevision(0x0000F000u), 0xFu);
    checkU32("bit 16 does not widen boot42 minor revision",
             nvBoot42MinorRevision(0x00010000u), 0x0u);

    // MAJOR_REVISION 19:16 -- no call site before this line.
    checkU32("boot42 major revision reads its own four bits",
             nvBoot42MajorRevision(0x000F0000u), 0xFu);
    checkU32("bit 20 does not widen boot42 major revision",
             nvBoot42MajorRevision(0x00100000u), 0x0u);

    // IMPLEMENTATION 23:20, same top-bit slip as BOOT_0's. Bit 24 is
    // ARCHITECTURE's low bit, so a widened mask folds part of the
    // architecture into the implementation.
    checkU32("bit 23 IS boot42 implementation's top bit",
             nvBoot42Implementation(0x00800000u), 0x8u);
    checkU32("bit 24 does not widen boot42 implementation",
             nvBoot42Implementation(0x01000000u), 0x0u);

    // ARCHITECTURE 29:24. Bit 30 is undecoded; bit 29 is the field's top bit,
    // and dropping it turns Ampere (0x17) into 0x17 minus 0x20 -- i.e. an
    // architecture this driver would fail to recognise at all.
    checkU32("bit 30 does not widen boot42 architecture",
             nvBoot42Architecture(0x40000000u), 0x0u);
    checkU32("bit 29 IS boot42 architecture's top bit",
             nvBoot42Architecture(0x20000000u), 0x20u);

    // CHIP_ID 29:20 spans both of the above, so it needs its own pair: the
    // composite is what separates GA106 from GA107.
    checkU32("bit 30 does not widen chip id",
             nvBoot42ChipId(0x40000000u), 0x0u);
    checkU32("bit 29 IS chip id's top bit",
             nvBoot42ChipId(0x20000000u), 0x200u);
}

//
// DEVICE-IDS.md and phase0-kext/README.md both print 0x177000xx as the value
// to expect. If the constants ever drift from that, the docs start lying to
// the person staring at a kernel log at 2am. Assemble it from the constants
// and compare.
//
static void testDocumentedValues(void)
{
    section("values printed in the documentation");

    NVU32 assembled =
        ((NV_ARCHITECTURE_GA100 & NV_PMC_BOOT_0_ARCHITECTURE_0_MASK)
            << NV_PMC_BOOT_0_ARCHITECTURE_0_SHIFT) |
        (NV_IMPLEMENTATION_GA107 << NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT);

    checkU32("assembled BOOT_0 base", assembled, 0x17700000u);
    checkU32("matches the GA107 sample", assembled, kBoot0_GA107 & 0xFFFFFF00u);
}

static void testDeadReads(void)
{
    section("dead-read detection");

    checkBool("0xFFFFFFFF is dead", nvLooksLikeDeadRead(0xFFFFFFFFu), true);
    checkBool("0x00000000 is dead", nvLooksLikeDeadRead(0x00000000u), true);
    checkBool("GA107 BOOT_0 is live", nvLooksLikeDeadRead(kBoot0_GA107), false);

    //
    // Only all-ones and all-zeroes are dead. A value that merely LOOKS odd is
    // still real data -- treating it as dead would hide a genuine misread
    // behind a friendlier-sounding error.
    //
    checkBool("0x00000001 is live", nvLooksLikeDeadRead(0x00000001u), false);
    checkBool("0xFFFFFFFE is live", nvLooksLikeDeadRead(0xFFFFFFFEu), false);
}

//
// NVGfwBootVerdict -- the family added because pollGfwBoot() returned a bare
// bool. `false` was satisfied by BOTH refusals at once: "this offset is not
// decoding" (a defect in this driver's BAR0 map) and "GFW decoded but never
// reached 0xFF" (a fact about the card). Two preimages, and they call for
// different actions.
//
// The sharper half is nvGfwBootProgressWasRead(). The unreadable arm used to
// write *outProgress = 0 before returning, and a literal 0 is byte-identical in
// the IORegistry to a card whose GFW genuinely sits at step 0 -- so the one arm
// that measured NOTHING published the most alarming possible measurement.
//
static void testGfwBootVerdict(void)
{
    section("Phase 3.5 -- GFW boot verdict");

    //
    // NOT_ATTEMPTED must be the enum's ZERO value: it is what a caller's
    // initialiser holds, and "nobody looked" has to be distinguishable from a
    // refusal code the caller never measured.
    //
    checkU32("NOT_ATTEMPTED is the zero value",
             (NVU32)NV_GFW_BOOT_NOT_ATTEMPTED, 0u);

    //
    // Every arm is DISTINCT. A copy-paste arm sharing a sibling's value
    // satisfies every "has a verdict" assertion while collapsing two facts.
    //
    checkBool("OK differs from TIMEOUT",
              NV_GFW_BOOT_OK != NV_GFW_BOOT_TIMEOUT, true);
    checkBool("OK differs from UNREADABLE",
              NV_GFW_BOOT_OK != NV_GFW_BOOT_UNREADABLE, true);
    checkBool("UNREADABLE differs from TIMEOUT",
              NV_GFW_BOOT_UNREADABLE != NV_GFW_BOOT_TIMEOUT, true);
    checkBool("every arm is below the family limit",
              NV_GFW_BOOT_TIMEOUT < NV_GFW_BOOT_VERDICT_LIMIT, true);

    //
    // nvGfwBootVerdictIsOk() is an EQUALITY against the accepting verdict. A
    // `!= UNREADABLE` implementation is TRUE for NOT_ATTEMPTED and for TIMEOUT,
    // so these three checks are what tell the two spellings apart -- the second
    // and third are the separating inputs.
    //
    checkBool("OK is ok",          nvGfwBootVerdictIsOk(NV_GFW_BOOT_OK), true);
    checkBool("TIMEOUT is not ok", nvGfwBootVerdictIsOk(NV_GFW_BOOT_TIMEOUT), false);
    checkBool("UNREADABLE is not ok",
              nvGfwBootVerdictIsOk(NV_GFW_BOOT_UNREADABLE), false);
    checkBool("NOT_ATTEMPTED is not ok",
              nvGfwBootVerdictIsOk(NV_GFW_BOOT_NOT_ATTEMPTED), false);

    //
    // nvGfwBootProgressWasRead() is the predicate a publisher consults before
    // putting the progress word in the IORegistry, and it is NOT the same
    // partition as IsOk(): TIMEOUT is a FAILURE whose progress word is a real
    // measurement. That pair -- TIMEOUT false for IsOk and TRUE here -- is the
    // whole reason both predicates exist, and an implementation that reused
    // IsOk() would discard the most useful number an operator can have.
    //
    checkBool("OK read the progress word",
              nvGfwBootProgressWasRead(NV_GFW_BOOT_OK), true);
    checkBool("TIMEOUT read the progress word",
              nvGfwBootProgressWasRead(NV_GFW_BOOT_TIMEOUT), true);
    checkBool("UNREADABLE read NOTHING",
              nvGfwBootProgressWasRead(NV_GFW_BOOT_UNREADABLE), false);
    checkBool("NOT_ATTEMPTED read NOTHING",
              nvGfwBootProgressWasRead(NV_GFW_BOOT_NOT_ATTEMPTED), false);

    //
    // The two predicates must not be the same function. If somebody implements
    // WasRead() as a call to IsOk(), TIMEOUT is the only input that separates
    // them -- so assert the disagreement directly rather than trusting that the
    // eight checks above happen to cover it.
    //
    checkBool("the two predicates disagree on TIMEOUT",
              nvGfwBootVerdictIsOk(NV_GFW_BOOT_TIMEOUT)
                  != nvGfwBootProgressWasRead(NV_GFW_BOOT_TIMEOUT), true);

    //
    // Every arm returns its OWN name. Per-arm plus pairwise-distinct, because an
    // arm returning a sibling's string satisfies every "has a name" assertion --
    // handoff21 section 1b's lesson, and the sentinel is asserted against what
    // the header actually returns rather than guessed from a sibling switch.
    //
    checkStr("OK is named",
             nvGfwBootVerdictName(NV_GFW_BOOT_OK), "completed");
    checkStr("UNREADABLE names the BAR0 map",
             nvGfwBootVerdictName(NV_GFW_BOOT_UNREADABLE),
             "offset not decoding -- BAR0 map may be short");
    checkStr("TIMEOUT names the timeout",
             nvGfwBootVerdictName(NV_GFW_BOOT_TIMEOUT),
             "timed out before GFW reported completion");
    checkStr("NOT_ATTEMPTED names the identity read",
             nvGfwBootVerdictName(NV_GFW_BOOT_NOT_ATTEMPTED),
             "not attempted -- identity read did not respond");

    //
    // The value one past the family answers the sentinel. This is the tripwire
    // that forces whoever adds a fifth verdict to come back and extend the name
    // switch -- without it a new arm silently publishes the unknown string, which
    // is exactly the defect handoff20 section 1 found in igModeVerdictName().
    //
    checkStr("one past the family is the sentinel",
             nvGfwBootVerdictName((NVGfwBootVerdict)NV_GFW_BOOT_VERDICT_LIMIT),
             "unrecognised GFW boot verdict");

    //
    // Coverage: every value BELOW the family's own limit has a name that is not
    // the sentinel, so an arm DELETED from the switch reddens on the commit that
    // deletes it. Bounded on the limit rather than a hand-written count, so a
    // family that grows reddens rather than silently under-testing.
    //
    {
        NVU32 named = 0;
        for (NVU32 v = 0; v < (NVU32)NV_GFW_BOOT_VERDICT_LIMIT; v++) {
            if (std::strcmp(nvGfwBootVerdictName((NVGfwBootVerdict)v),
                            "unrecognised GFW boot verdict") != 0)
                named++;
        }
        checkU32("every verdict below the limit is named",
                 named, (NVU32)NV_GFW_BOOT_VERDICT_LIMIT);
    }
}

static void testGfwBoot(void)
{
    section("Phase 3.5 -- GFW boot progress");

    checkU32("scratch 05[0] offset",
             NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT, 0x00118234u);
    checkU32("scratch 05[1] stride is 4",
             NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(1), 0x00118238u);
    checkU32("usable FB size offset", NV_USABLE_FB_SIZE_IN_MB, 0x001183A4u);

    checkU32("progress from 0x000000FF", nvGfwBootProgress(0x000000FFu), 0xFFu);
    checkBool("0xFF is complete", nvGfwBootComplete(0x000000FFu), true);
    checkBool("0x0F is not complete", nvGfwBootComplete(0x0000000Fu), false);

    //
    // Upper bits of the scratch register carry unrelated state. The progress
    // field is 7:0 only, so a dirty upper half must not affect the verdict.
    //
    checkBool("upper bits ignored", nvGfwBootComplete(0xDEADBEFFu), true);
    checkU32("progress masks to 8 bits", nvGfwBootProgress(0xFFFFFFFFu), 0xFFu);

    //
    // SEPARATING INPUT for a progress mask widened to 8:0. 0xDEADBEFF has bit 8
    // CLEAR, so the "upper bits ignored" check above reads 0x0FF through a
    // nine-bit mask -- still COMPLETED, verdict unchanged, mutant survives.
    // 0xDEADBFFF sets bit 8: the correct 7:0 read is 0xFF (complete), the
    // widened read is 0x1FF (not complete). That is the only shape of input
    // that tells the two decoders apart.
    //
    checkU32("bit 8 does not widen gfw progress",
             nvGfwBootProgress(0xDEADBFFFu), 0xFFu);
    checkBool("  and the verdict stays complete",
              nvGfwBootComplete(0xDEADBFFFu), true);
    checkU32("bit 7 IS gfw progress's top bit",
             nvGfwBootProgress(0x00000080u), 0x80u);

    //
    // Both GC6 island registers must sit inside the 16 MiB BAR0 aperture, or
    // Phase 3.5 reads run off the end of the mapping.
    //
    checkBool("GFW_BOOT inside BAR0",
              NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT < NV_BAR0_EXPECTED_LENGTH, true);
    checkBool("FB size inside BAR0",
              NV_USABLE_FB_SIZE_IN_MB < NV_BAR0_EXPECTED_LENGTH, true);
}

static void testFbSize(void)
{
    section("framebuffer size plausibility");

    checkBool("4096 MB (4 GB 3050) plausible", nvFbSizeIsPlausible(4096u), true);
    checkBool("6144 MB (6 GB 3050) plausible", nvFbSizeIsPlausible(6144u), true);
    checkBool("0 MB rejected", nvFbSizeIsPlausible(0u), false);
    checkBool("128 MB rejected", nvFbSizeIsPlausible(128u), false);
    checkBool("0xFFFFFFFF rejected", nvFbSizeIsPlausible(0xFFFFFFFFu), false);
}

//
// nvFbSizeVerdict() exists because nvFbSizeIsPlausible() above is a BOOL, and
// `false` is satisfied by all three refusals at once -- the class recorded in
// 3050-handoff34.md section 1 one subsystem over. The shell consumed that bool
// and then decided whether to PUBLISH from the MAGNITUDE of the megabyte count,
// which is the defect this family removes: the value mask is 0xFFFFFFFF, so a
// refused read masks to 4294967295, and 4294967295 is greater than zero.
//
// Four properties:
//
//   1. Every arm is REACHED by a value that lands only on it. An unreachable
//      arm is indistinguishable from a deleted one (handoff32 section 2a).
//   2. ORDER is pinned by the values satisfying TWO arms at once. 0xFFFFFFFF is
//      both unreadable AND implausible; 0 is both unpublished AND implausible.
//      Those checks are the whole argument for the header's arm order, and a
//      reordered subject reddens exactly them.
//   3. Every verdict below the family's own LIMIT has its OWN name, pairwise
//      distinct -- a copy-paste arm returning a sibling's string satisfies
//      every has-a-name assertion (handoff21 section 1).
//   4. NOT_ATTEMPTED is the ZERO value and nvFbSizeVerdict() NEVER returns it.
//      It is the caller's initialiser, so an unassigned verdict reads as "not
//      measured" rather than as a good size.
//
static void testFbSizeVerdict(void)
{
    section("framebuffer size verdict");

    checkU32("NOT_ATTEMPTED is the zero value",
             (NVU32)NV_FB_SIZE_NOT_ATTEMPTED, 0u);
    checkU32("verdict family has five members",
             (NVU32)NV_FB_SIZE_VERDICT_LIMIT, 5u);

    //
    // Property 1 -- every arm reached.
    //
    checkU32("4096 MB measured",
             (NVU32)nvFbSizeVerdict(4096u), (NVU32)NV_FB_SIZE_OK);
    checkU32("6144 MB measured",
             (NVU32)nvFbSizeVerdict(6144u), (NVU32)NV_FB_SIZE_OK);
    checkU32("all-ones is UNREADABLE",
             (NVU32)nvFbSizeVerdict(0xFFFFFFFFu), (NVU32)NV_FB_SIZE_UNREADABLE);
    checkU32("zero is UNPUBLISHED",
             (NVU32)nvFbSizeVerdict(0u), (NVU32)NV_FB_SIZE_UNPUBLISHED);
    checkU32("128 MB is IMPLAUSIBLE",
             (NVU32)nvFbSizeVerdict(128u), (NVU32)NV_FB_SIZE_IMPLAUSIBLE);
    checkU32("1 TB is IMPLAUSIBLE",
             (NVU32)nvFbSizeVerdict(1024u * 1024u), (NVU32)NV_FB_SIZE_IMPLAUSIBLE);

    //
    // Property 2 -- the two order pins. Both of these values satisfy the
    // implausible arm as well, so an implausibility-first subject returns
    // IMPLAUSIBLE for both and these two checks are the only ones that move.
    //
    checkBool("all-ones is ALSO implausible, so its arm must run first",
              nvFbSizeIsPlausible(0xFFFFFFFFu), false);
    checkBool("zero is ALSO implausible, so its arm must run first",
              nvFbSizeIsPlausible(0u), false);

    //
    // Property 3 -- every verdict below the LIMIT has its own name, and the
    // value one past the family answers the sentinel. That last check is the
    // tripwire forcing whoever adds a sixth verdict to extend the switch.
    //
    const char *seen[NV_FB_SIZE_VERDICT_LIMIT];
    const char *sentinel = nvFbSizeVerdictName((NVFbSizeVerdict)NV_FB_SIZE_VERDICT_LIMIT);

    checkBool("one past the family answers the sentinel",
              std::strcmp(sentinel, "invalid verdict") == 0, true);

    for (NVU32 v = 0; v < (NVU32)NV_FB_SIZE_VERDICT_LIMIT; ++v) {
        seen[v] = nvFbSizeVerdictName((NVFbSizeVerdict)v);
        checkBool("verdict is named, not the sentinel",
                  std::strcmp(seen[v], sentinel) != 0, true);
    }

    for (NVU32 a = 0; a < (NVU32)NV_FB_SIZE_VERDICT_LIMIT; ++a)
        for (NVU32 b = a + 1; b < (NVU32)NV_FB_SIZE_VERDICT_LIMIT; ++b)
            checkBool("verdict names are pairwise distinct",
                      std::strcmp(seen[a], seen[b]) != 0, true);

    //
    // Property 4 -- NOT_ATTEMPTED is never produced by the decoder. It is the
    // caller's initialiser and nothing else. A subject that returned it for
    // some raw word would let "nobody looked" and "the read was refused" become
    // the same published string.
    //
    const NVU32 probes[] = { 0u, 1u, 255u, 256u, 4095u, 4096u, 6144u,
                             131072u, 131073u, 0x7FFFFFFFu, 0xFFFFFFFEu,
                             0xFFFFFFFFu };

    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i)
        checkBool("decoder never returns NOT_ATTEMPTED",
                  nvFbSizeVerdict(probes[i]) != NV_FB_SIZE_NOT_ATTEMPTED, true);
}

//
// The two name switches in NVDecode.hpp are the only string-producing functions
// in the NVIDIA decode layer, and until 2026-08-17 this suite called each of
// them EXACTLY ONCE: nvArchitectureName(NV_ARCHITECTURE_GA100) in testBoot0()
// and nvAmpereImplementationName(NV_IMPLEMENTATION_GA107) in
// testSplitArchitecture(). Six of seven architecture arms and five of six
// implementation arms had therefore never EXECUTED, so an arm returning a
// sibling's string -- or missing entirely and falling through to `default` --
// was indistinguishable from correct code.
//
// That is the shape handoff20 found on the Intel side one level down, and the
// reason a `default` arm is honest and useless: it answers, plausibly, for the
// exact value the switch was supposed to name. Both switches here are complete
// as written (verified 2026-08-17 by set-difference: 7 defines against 7 arms,
// 6 against 6). This section is what makes that stay true.
//
// Three properties, because per-arm pinning alone is not enough:
//
//   1. Every arm returns its OWN string. A copy-paste arm returning a sibling's
//      name passes any "is not unknown" test, so the names must be pinned
//      individually AND asserted pairwise distinct.
//   2. Every value in the family's contiguous range is named. This catches an
//      arm DELETED later without this test having to list the family, which is
//      the failure mode of a hand-written table: a table edited to match the
//      code cannot audit the code.
//   3. The value one past the range answers "unknown". This is the tripwire.
//      An eighth architecture cannot be given a switch arm without reddening
//      this check, which forces whoever adds it to come here and extend the
//      bound in property 2 -- otherwise property 2 would silently stop covering
//      the newest arm, which is precisely how the Intel defect survived.
//
// The GAPS are pinned for the same reason. NVIDIA defines no Ampere
// implementation 0x01 or 0x05, so those must answer "unknown"; spelling that
// out is what stops property 2's loop from being "simplified" into a straight
// range walk over 0x00..0x07, which would report a failure that is wrong about
// the world.
//
static void testNameSwitches(void)
{
    section("architecture and implementation name switches");

    //
    // The two switches do NOT share a sentinel: nvArchitectureName() answers
    // "unknown" (NVDecode.hpp:278) and nvAmpereImplementationName() answers
    // "unrecognised Ampere part" (:291). I got this wrong on the first write of
    // this section and four assertions went RED against correct shipping code,
    // which is worth recording because the FIFTH site was a real vacuity bug
    // rather than a cosmetic one: property 2's loop below compared each defined
    // implementation against "unknown", a string that switch can never return,
    // so it counted 6 named arms no matter what -- including for an arm DELETED
    // from the switch. A sentinel guessed from a sibling function makes a
    // coverage loop that cannot fail.
    //
    // Bound to named constants so the same mistake cannot recur silently, and
    // asserted DISTINCT so nobody "tidies" the two switches into one sentinel
    // without noticing that this section's two loops depend on which is which.
    //
    const char *const kArchSentinel = "unknown";
    const char *const kImplSentinel = "unrecognised Ampere part";

    checkBool("the two switches use different sentinels",
              std::strcmp(kArchSentinel, kImplSentinel) != 0, true);
    checkStr("the architecture sentinel is what the header returns",
             nvArchitectureName(0x00u), kArchSentinel);
    checkStr("the implementation sentinel is what the header returns",
             nvAmpereImplementationName(0x01u), kImplSentinel);

    // Property 1, architectures -- every arm, by its own constant.
    checkStr("0x16 is Turing",
             nvArchitectureName(NV_ARCHITECTURE_TU100), "Turing");
    checkStr("0x17 is Ampere",
             nvArchitectureName(NV_ARCHITECTURE_GA100), "Ampere");
    checkStr("0x18 is Hopper",
             nvArchitectureName(NV_ARCHITECTURE_GH100), "Hopper");
    checkStr("0x19 is Ada",
             nvArchitectureName(NV_ARCHITECTURE_AD100), "Ada");
    checkStr("0x1A is Blackwell",
             nvArchitectureName(NV_ARCHITECTURE_GB100), "Blackwell");
    checkStr("0x1B is Blackwell 2",
             nvArchitectureName(NV_ARCHITECTURE_GB200), "Blackwell 2");
    checkStr("0x1C is Rubin",
             nvArchitectureName(NV_ARCHITECTURE_GR100), "Rubin");

    // Property 1, implementations.
    checkStr("impl 0x00 is GA100",
             nvAmpereImplementationName(NV_IMPLEMENTATION_GA100), "GA100");
    checkStr("impl 0x02 is GA102",
             nvAmpereImplementationName(NV_IMPLEMENTATION_GA102), "GA102");
    checkStr("impl 0x03 is GA103",
             nvAmpereImplementationName(NV_IMPLEMENTATION_GA103), "GA103");
    checkStr("impl 0x04 is GA104",
             nvAmpereImplementationName(NV_IMPLEMENTATION_GA104), "GA104");
    checkStr("impl 0x06 is GA106",
             nvAmpereImplementationName(NV_IMPLEMENTATION_GA106), "GA106");
    checkStr("impl 0x07 is GA107 (the target part)",
             nvAmpereImplementationName(NV_IMPLEMENTATION_GA107), "GA107");

    //
    // Property 1's separating half. GA106 and GA107 differ by one character and
    // sit adjacent in the switch, which is the single most likely copy-paste
    // pair in this header -- and mixing them up would mis-name the target part
    // in a kernel log while every "has a name" check stayed green.
    //
    {
        const char *archNames[7];
        archNames[0] = nvArchitectureName(NV_ARCHITECTURE_TU100);
        archNames[1] = nvArchitectureName(NV_ARCHITECTURE_GA100);
        archNames[2] = nvArchitectureName(NV_ARCHITECTURE_GH100);
        archNames[3] = nvArchitectureName(NV_ARCHITECTURE_AD100);
        archNames[4] = nvArchitectureName(NV_ARCHITECTURE_GB100);
        archNames[5] = nvArchitectureName(NV_ARCHITECTURE_GB200);
        archNames[6] = nvArchitectureName(NV_ARCHITECTURE_GR100);

        int archCollisions = 0;
        for (int i = 0; i < 7; ++i) {
            for (int j = i + 1; j < 7; ++j) {
                if (std::strcmp(archNames[i], archNames[j]) == 0) { archCollisions++; }
            }
        }
        checkU32("the seven architecture names are pairwise distinct",
                 (NVU32)archCollisions, 0u);

        const char *implNames[6];
        implNames[0] = nvAmpereImplementationName(NV_IMPLEMENTATION_GA100);
        implNames[1] = nvAmpereImplementationName(NV_IMPLEMENTATION_GA102);
        implNames[2] = nvAmpereImplementationName(NV_IMPLEMENTATION_GA103);
        implNames[3] = nvAmpereImplementationName(NV_IMPLEMENTATION_GA104);
        implNames[4] = nvAmpereImplementationName(NV_IMPLEMENTATION_GA106);
        implNames[5] = nvAmpereImplementationName(NV_IMPLEMENTATION_GA107);

        int implCollisions = 0;
        for (int i = 0; i < 6; ++i) {
            for (int j = i + 1; j < 6; ++j) {
                if (std::strcmp(implNames[i], implNames[j]) == 0) { implCollisions++; }
            }
        }
        checkU32("the six implementation names are pairwise distinct",
                 (NVU32)implCollisions, 0u);
    }

    //
    // Property 2, architectures. The family is contiguous 0x16..0x1C, so the
    // loop is derived from the family's own endpoint constants rather than from
    // a count typed here. Assert the span too, so a constant moved out of the
    // range cannot shrink the loop silently.
    //
    checkU32("the architecture family spans seven values",
             (NVU32)(NV_ARCHITECTURE_GR100 - NV_ARCHITECTURE_TU100 + 1), 7u);
    {
        NVU32 named = 0u;
        for (NVU32 a = NV_ARCHITECTURE_TU100; a <= NV_ARCHITECTURE_GR100; ++a) {
            if (std::strcmp(nvArchitectureName(a), "unknown") != 0) { named++; }
        }
        checkU32("every architecture in the range has a name", named, 7u);
    }

    //
    // Property 2, implementations. NOT a range walk: 0x01 and 0x05 are genuine
    // holes in NVIDIA's numbering, so the defined values are visited by name.
    //
    {
        NVU32 named = 0u;
        const NVU32 defined[6] = {
            NV_IMPLEMENTATION_GA100, NV_IMPLEMENTATION_GA102,
            NV_IMPLEMENTATION_GA103, NV_IMPLEMENTATION_GA104,
            NV_IMPLEMENTATION_GA106, NV_IMPLEMENTATION_GA107,
        };
        for (int i = 0; i < 6; ++i) {
            if (std::strcmp(nvAmpereImplementationName(defined[i]),
                            kImplSentinel) != 0) {
                named++;
            }
        }
        checkU32("every defined Ampere implementation has a name", named, 6u);
    }

    //
    // Property 3 -- the tripwires, on both sides of each range. 0x15 is Volta,
    // which this header deliberately does not carry; 0x1D is whatever NVIDIA
    // ships next.
    //
    checkStr("0x15 (below Turing) is unknown",
             nvArchitectureName(NV_ARCHITECTURE_TU100 - 1u), "unknown");
    checkStr("0x1D (above Rubin) is unknown",
             nvArchitectureName(NV_ARCHITECTURE_GR100 + 1u), "unknown");
    checkStr("impl 0x08 (above GA107) is unrecognised",
             nvAmpereImplementationName(NV_IMPLEMENTATION_GA107 + 1u), kImplSentinel);

    // The two holes NVIDIA leaves inside the implementation range.
    checkStr("impl 0x01 is a hole, not a name",
             nvAmpereImplementationName(0x01u), kImplSentinel);
    checkStr("impl 0x05 is a hole, not a name",
             nvAmpereImplementationName(0x05u), kImplSentinel);

    //
    // Wildly out-of-range inputs, including the dead-read pattern. A register
    // that reads all-ones must not name an architecture: nvLooksLikeDeadRead()
    // is what catches that, but if a name switch answered for 0xFFFFFFFF the
    // log line would look authoritative anyway.
    //
    checkStr("0x00 is unknown", nvArchitectureName(0x00u), "unknown");
    checkStr("0xFFFFFFFF is unknown", nvArchitectureName(0xFFFFFFFFu), "unknown");
    checkStr("impl 0xFFFFFFFF is unrecognised",
             nvAmpereImplementationName(0xFFFFFFFFu), kImplSentinel);

    //
    // Finally, the join: the real GA107 boot0 word must route through BOTH
    // switches to the target part's two names. The per-arm checks above pin the
    // switches; this pins that the decoders feeding them pick the right arms,
    // which is the only thing an operator actually reads in a kernel log.
    //
    checkStr("real GA107 boot0 names Ampere",
             nvArchitectureName(nvBoot0Architecture(kBoot0_GA107)), "Ampere");
    checkStr("real GA107 boot0 names GA107",
             nvAmpereImplementationName(nvBoot0Implementation(kBoot0_GA107)), "GA107");
    checkStr("real GA107 boot42 names Ampere",
             nvArchitectureName(nvBoot42Architecture(kBoot42_GA107)), "Ampere");
}

//
// nvPartVerdict() -- the four findings decodeBoot0() used to log and throw away.
//
// Until 2026-08-18 all four lived inside decodeBoot0(), a method on the IOKit
// shell, so no host suite could compile them and nothing had ever EXECUTED one.
// They are now a pure function of boot0 and this section is what makes them a
// claim rather than a log line.
//
// The separating-input rule matters more here than usual: a verdict family whose
// arms all return the same value satisfies every "it refused" assertion, so each
// of the four codes needs an input NO other code can produce.
//
// The ORDER pin is the sharp one. NV_PART_ADA is a subset of "not Ampere", so a
// version testing the general case first makes the Ada arm unreachable -- the
// dead-arm shape 3050-handoff32.md exists to remove, reintroduced one level
// over, and the one finding of the four with an operator action attached.
//
static void testPartVerdict(void)
{
    section("part verdict");

    //
    // A boot0 word carrying an arbitrary architecture and implementation, built
    // through the same shifts the decoders read so the test cannot pass by
    // agreeing with itself about a layout.
    //
    // Architecture is SPLIT across two fields in BOOT_0 -- that is what
    // testSplitArchitecture() covers -- so composing it here by hand is the
    // only way to reach an arch the real part never reports.
    //
    struct Compose {
        static NVU32 boot0(NVU32 arch, NVU32 impl)
        {
            //
            // The low half's WIDTH is derived from its own mask rather than
            // spelled as 5. The header defines no _BITS constant for it, and a
            // hand-written 5 here would be a second, unchecked copy of a field
            // layout the decoder already owns -- so widening the mask would
            // leave this composer silently building the old shape.
            //
            NVU32 loBits = 0;
            for (NVU32 m = NV_PMC_BOOT_0_ARCHITECTURE_0_MASK; m != 0; m >>= 1) {
                loBits++;
            }

            NVU32 lo = arch & NV_PMC_BOOT_0_ARCHITECTURE_0_MASK;
            NVU32 hi = (arch >> loBits) & NV_PMC_BOOT_0_ARCHITECTURE_1_MASK;

            return (lo << NV_PMC_BOOT_0_ARCHITECTURE_0_SHIFT)
                 | (hi << NV_PMC_BOOT_0_ARCHITECTURE_1_SHIFT)
                 | ((impl & NV_PMC_BOOT_0_IMPLEMENTATION_MASK)
                    << NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT);
        }
    };

    //
    // Before anything else: the composer must reproduce the real word's two
    // fields. A composer that is wrong makes every check below a test of two
    // agreeing mistakes.
    //
    checkBool("composer reproduces GA107 architecture",
          nvBoot0Architecture(Compose::boot0(NV_ARCHITECTURE_GA100,
                                             NV_IMPLEMENTATION_GA107))
              == NV_ARCHITECTURE_GA100, true);
    checkBool("composer reproduces GA107 implementation",
          nvBoot0Implementation(Compose::boot0(NV_ARCHITECTURE_GA100,
                                               NV_IMPLEMENTATION_GA107))
              == NV_IMPLEMENTATION_GA107, true);

    // ---- a separating input for each of the four verdicts -----------------

    checkBool("real GA107 boot0 is the target part",
          nvPartVerdict(kBoot0_GA107) == NV_PART_TARGET, true);

    checkBool("composed GA107 is the target part",
          nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_GA100,
                                       NV_IMPLEMENTATION_GA107))
              == NV_PART_TARGET, true);

    checkBool("GA106 is Ampere but not the target",
          nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_GA100,
                                       NV_IMPLEMENTATION_GA106))
              == NV_PART_AMPERE_OTHER, true);

    checkBool("GA102 is Ampere but not the target",
          nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_GA100,
                                       NV_IMPLEMENTATION_GA102))
              == NV_PART_AMPERE_OTHER, true);

    checkBool("Ada is its own verdict",
          nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_AD100, 0x02u))
              == NV_PART_ADA, true);

    checkBool("Turing is neither Ampere nor Ada",
          nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_TU100,
                                       NV_IMPLEMENTATION_GA107))
              == NV_PART_OTHER_ARCHITECTURE, true);

    checkBool("Hopper is neither Ampere nor Ada",
          nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_GH100, 0x00u))
              == NV_PART_OTHER_ARCHITECTURE, true);

    checkBool("Blackwell is neither Ampere nor Ada",
          nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_GB100, 0x00u))
              == NV_PART_OTHER_ARCHITECTURE, true);

    //
    // The ORDER pin. An Ada part carrying GA107's implementation number is BOTH
    // "not Ampere" and "Ada", and only the Ada-specific test running before the
    // general one returns NV_PART_ADA. Reverse the two tests in the header and
    // this is the check that reddens; without it, the Ada arm could be deleted
    // and every other check here would stay green.
    //
    checkBool("Ada carrying GA107's impl number is still Ada",
          nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_AD100,
                                       NV_IMPLEMENTATION_GA107))
              == NV_PART_ADA, true);

    //
    // The implementation number is IGNORED once the architecture is not Ampere.
    // Pinned because the obvious wrong refactor -- one join over both fields --
    // would make an Ada part's verdict depend on a field that means something
    // different on Ada silicon.
    //
    checkBool("Ada verdict does not depend on the implementation field",
          nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_AD100, 0x00u))
              == nvPartVerdict(Compose::boot0(NV_ARCHITECTURE_AD100, 0x0Fu)), true);

    //
    // A dead read must NOT name the target part. All-ones decodes to an
    // architecture of 0x3F and all-zeroes to 0x00; neither is Ampere, so both
    // must land on the general arm. nvLooksLikeDeadRead() is what actually
    // catches these, but a verdict answering TARGET for them would make the
    // IORegistry say the target part was found on a machine that answered
    // nothing.
    //
    checkBool("all-ones is not the target part",
          nvPartVerdict(0xFFFFFFFFu) != NV_PART_TARGET, true);
    checkBool("all-zeroes is not the target part",
          nvPartVerdict(0x00000000u) != NV_PART_TARGET, true);

    // ---- every verdict below the family's own limit has a name ------------

    //
    // Bounded on NV_PART_VERDICT_LIMIT rather than on a typed count, so a fifth
    // verdict added to the enum with no name arm reddens on the commit that adds
    // it. The sentinel is READ OUT OF THE SUBJECT rather than spelled --
    // handoff21 S1b is the reason: a sentinel guessed from a sibling function is
    // a comparison against a string this switch can never return, and every arm
    // then counts as named unconditionally.
    //
    const char *kSentinel =
        nvPartVerdictName((NVPartVerdict)NV_PART_VERDICT_LIMIT);

    checkBool("the limit itself answers a sentinel", kSentinel[0] != '\0', true);

    for (NVU32 v = 0; v < (NVU32)NV_PART_VERDICT_LIMIT; v++) {
        char label[96];
        std::snprintf(label, sizeof(label), "verdict %u has its own name",
                      (unsigned)v);
        checkBool(label, std::strcmp(nvPartVerdictName((NVPartVerdict)v),
                                 kSentinel) != 0, true);
    }

    //
    // Pairwise distinct. A copy-paste arm returning a sibling's string passes
    // every "has a name" check above -- and "Ampere, not GA107" beside
    // "GA107 -- target part" is exactly the pair an operator must be able to
    // tell apart.
    //
    for (NVU32 a = 0; a < (NVU32)NV_PART_VERDICT_LIMIT; a++) {
        for (NVU32 b = a + 1; b < (NVU32)NV_PART_VERDICT_LIMIT; b++) {
            char label[96];
            std::snprintf(label, sizeof(label), "verdict %u and %u differ",
                          (unsigned)a, (unsigned)b);
            checkBool(label, std::strcmp(nvPartVerdictName((NVPartVerdict)a),
                                     nvPartVerdictName((NVPartVerdict)b)) != 0, true);
        }
    }
}

static void testChipVerdict(void)
{
    section("chip verdict");

    //
    // A BOOT_42 word carrying an arbitrary composite chip ID, built through the
    // same shift the decoder reads. Composing through the subject's own #define
    // is only safe because the FIRST check below anchors the composer against
    // the real GA107 word -- otherwise the fixture and the accessor cancel and
    // every check below tests two copies of one belief.
    //
    struct Compose {
        static NVU32 boot42(NVU32 chipId)
        {
            return (chipId & NV_PMC_BOOT_42_CHIP_ID_MASK)
                   << NV_PMC_BOOT_42_CHIP_ID_SHIFT;
        }
    };

    checkU32("composer reproduces the real GA107 chip ID",
             nvBoot42ChipId(Compose::boot42(NV_CHIP_ID_GA107)),
             nvBoot42ChipId(kBoot42_GA107));

    // ---- the four findings decodeBoot42() logs ----------------------------

    checkBool("GA107 is the target die",
          nvChipVerdict(Compose::boot42(NV_CHIP_ID_GA107))
              == NV_CHIP_TARGET, true);

    checkBool("the real GA107 word is the target die",
          nvChipVerdict(kBoot42_GA107) == NV_CHIP_TARGET, true);

    checkBool("GA106 is the named sibling",
          nvChipVerdict(Compose::boot42(NV_CHIP_ID_GA106))
              == NV_CHIP_SIBLING_GA106, true);

    //
    // A chip ID that is neither. 0x1FF is not an architecture/implementation
    // pair this tree names, which is the point -- the general arm exists for
    // silicon nobody has enumerated.
    //
    checkBool("an unenumerated chip ID lands on the general arm",
          nvChipVerdict(Compose::boot42(0x1FFu)) == NV_CHIP_OTHER, true);

    // ---- the two SUBSET order pins ---------------------------------------

    //
    // GA106 is a SUBSET of "not GA107". If the sibling test sat BESIDE the
    // mismatch test rather than inside it, whichever was written first would
    // win and the other arm would be dead -- the shape 3050-handoff33.md S2
    // removed from nvPartVerdict(), one register over. A GA106 part answering
    // OTHER is that regression.
    //
    checkBool("GA106 is not swallowed by the general mismatch arm",
          nvChipVerdict(Compose::boot42(NV_CHIP_ID_GA106))
              != NV_CHIP_OTHER, true);

    //
    // The dead-read test must run before the general FALL-THROUGH, and these are
    // the checks that prove it. A dead read is not a special kind of mismatch,
    // but it DECODES as one: CHIP_ID is ten bits, so all-ones yields 0x3FF and
    // all-zeroes yields 0x000. Delete the dead-read test and both land on
    // NV_CHIP_OTHER -- "wrong die", a statement about silicon -- over a register
    // nobody successfully read.
    //
    // Be exact about what this does NOT depend on. Hoisting the dead-read test
    // below the two EQUALITY arms was measured on 2026-08-19 and reddened ZERO
    // checks, correctly: 0x3FF and 0x000 cannot equal GA107 (0x177) or GA106
    // (0x176), so those arms can never steal a dead read. The non-collision is
    // pinned below rather than left as arithmetic in a comment, because it is
    // the fact the ordering claim rests on -- a named die landing on either
    // value would make the equality order load-bearing overnight.
    //
    checkBool("all-ones is unreadable, not a wrong die",
          nvChipVerdict(0xFFFFFFFFu) == NV_CHIP_UNREADABLE, true);
    checkBool("all-zeroes is unreadable, not a wrong die",
          nvChipVerdict(0x00000000u) == NV_CHIP_UNREADABLE, true);

    //
    // And neither dead read may name the target die. This is the same claim
    // testPartVerdict() makes about BOOT_0 and it is worth making twice: the
    // IORegistry saying the target die was found on a machine that answered
    // nothing is the exact defect NVBoot42Readable exists to prevent.
    //
    checkBool("all-ones is not the target die",
          nvChipVerdict(0xFFFFFFFFu) != NV_CHIP_TARGET, true);
    checkBool("all-zeroes is not the target die",
          nvChipVerdict(0x00000000u) != NV_CHIP_TARGET, true);

    //
    // The non-collision the ordering claim above RESTS ON. If a named die's
    // chip ID ever equalled 0x3FF or 0x000, an equality arm could steal a dead
    // read and its position relative to the dead-read test would become
    // load-bearing overnight -- so this is asserted rather than left as
    // arithmetic in a comment. Read through nvBoot42ChipId() rather than
    // spelled as 0x3FF, so a change to CHIP_ID's shift or width is carried
    // into the claim instead of silently invalidating it.
    //
    checkBool("all-ones chip ID is not GA107",
          nvBoot42ChipId(0xFFFFFFFFu) != (NVU32)NV_CHIP_ID_GA107, true);
    checkBool("all-ones chip ID is not GA106",
          nvBoot42ChipId(0xFFFFFFFFu) != (NVU32)NV_CHIP_ID_GA106, true);
    checkBool("all-zeroes chip ID is not GA107",
          nvBoot42ChipId(0x00000000u) != (NVU32)NV_CHIP_ID_GA107, true);
    checkBool("all-zeroes chip ID is not GA106",
          nvBoot42ChipId(0x00000000u) != (NVU32)NV_CHIP_ID_GA106, true);

    //
    // NV_CHIP_UNREADABLE is the ZERO value on purpose, so a verdict variable
    // that was never assigned reads as "not measured" rather than as the target
    // part. Pinned because renumbering the enum for tidiness would silently
    // make an uninitialised verdict claim GA107.
    //
    checkU32("unreadable is the zero value", (NVU32)NV_CHIP_UNREADABLE, 0u);

    // ---- every verdict below the family's own limit has a name ------------

    //
    // Bounded on NV_CHIP_VERDICT_LIMIT rather than a typed count, and the
    // sentinel is READ OUT OF THE SUBJECT rather than spelled -- handoff21 S1b:
    // a sentinel guessed from a sibling function is a comparison against a
    // string this switch can never return, and every arm then counts as named
    // unconditionally. nvPartVerdictName() is that sibling, five lines up, and
    // its default string differs from this one.
    //
    const char *kSentinel =
        nvChipVerdictName((NVChipVerdict)NV_CHIP_VERDICT_LIMIT);

    checkBool("the limit itself answers a sentinel", kSentinel[0] != '\0', true);

    for (NVU32 v = 0; v < (NVU32)NV_CHIP_VERDICT_LIMIT; v++) {
        char label[96];
        std::snprintf(label, sizeof(label), "chip verdict %u has its own name",
                      (unsigned)v);
        checkBool(label, std::strcmp(nvChipVerdictName((NVChipVerdict)v),
                                 kSentinel) != 0, true);
    }

    //
    // Pairwise distinct. A copy-paste arm returning a sibling's string passes
    // every "has a name" check above -- and the GA107 and GA106 strings are the
    // pair an operator most needs to tell apart, since they differ by one digit
    // and describe two dies sold under names one character apart.
    //
    for (NVU32 a = 0; a < (NVU32)NV_CHIP_VERDICT_LIMIT; a++) {
        for (NVU32 b = a + 1; b < (NVU32)NV_CHIP_VERDICT_LIMIT; b++) {
            char label[96];
            std::snprintf(label, sizeof(label), "chip verdict %u and %u differ",
                          (unsigned)a, (unsigned)b);
            checkBool(label, std::strcmp(nvChipVerdictName((NVChipVerdict)a),
                                     nvChipVerdictName((NVChipVerdict)b)) != 0, true);
        }
    }

    //
    // The two families must not share a sentinel. nvPartVerdictName() and
    // nvChipVerdictName() sit adjacent in the header and are the two functions
    // most likely to be "unified" by a later edit; if they returned the same
    // default string, a coverage loop written against either one would count
    // the other's arms as named. handoff21 S1b is what that costs.
    //
    checkBool("the two verdict families have distinct sentinels",
          std::strcmp(kSentinel,
                      nvPartVerdictName((NVPartVerdict)NV_PART_VERDICT_LIMIT)) != 0,
          true);
}

int main(void)
{
    std::printf("NVDecode.hpp host tests\n");
    std::printf("=======================\n");

    testBoot0();
    testSplitArchitecture();
    testBoot42();
    testAdjacentBits();
    testDocumentedValues();
    testDeadReads();
    testGfwBootVerdict();
    testGfwBoot();
    testFbSize();
    testFbSizeVerdict();
    testNameSwitches();
    testPartVerdict();
    testChipVerdict();

    //
    // The caveat prints BEFORE the summary on purpose. The count must be the
    // LAST line of output so `tail -1` on this binary returns a count and not a
    // sentence -- a reader (or a script) checking the final line is exactly how
    // a zero-assertion suite gets misread as passing. All five suites in this
    // directory end on their count for that reason.
    //
    if (gFailures == 0) {
        std::printf("\nDecode logic is correct. This does NOT mean the kext\n"
                    "builds -- IOKit and the kernel build flags are still\n"
                    "unproven until Step 7.\n");
    }

    std::printf("\n%d/%d passed, %d failed\n",
                gChecks - gFailures, gChecks, gFailures);

    return gFailures == 0 ? 0 : 1;
}
