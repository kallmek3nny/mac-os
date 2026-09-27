//
//  test_intel_display_names.cpp -- host-side suite for IntelDisplayNames.hpp.
//
//  Compiles the IDENTICAL header the kext uses, under MSVC, on a machine with no
//  Intel GPU and no macOS.
//
//  WHY THIS SUITE EXISTS AT ALL
//
//  Until IntelDisplayNames.hpp was split out, every function it holds was
//  file-static inside phase0-kext/IntelDisplayProbe.cpp -- a translation unit
//  tests/ cannot compile, because it includes <IOKit/IOService.h>. handoff19 had
//  to record that the four verdict-name switches, igTransName() and the
//  property-key composition carried ZERO runtime coverage and were reachable
//  only by clang's front end. A case arm returning a NEIGHBOURING literal was
//  invisible to all four gates: the parse is happy, the register gate checks
//  constants, and no binary ever called the function.
//
//  That was not hypothetical. igModeVerdictName() had arms for 0 through 5 while
//  IG_MODE_VERDICT_LIMIT is 6, so IG_MODE_SRC_MISMATCH published "unknown".
//  testExhaustive() below is the check that finds that class, and it found this
//  instance.
//
//  THE RULES THIS SUITE OBEYS
//
//  1. Every expected string is a BARE LITERAL. Comparing a name against the
//     same table the name came from cancels out and proves nothing -- the same
//     reason test_intel_framebuffer.cpp pins register values to literals rather
//     than to the #defines that produced them.
//
//  2. Exhaustiveness is walked against each family's own _VERDICT_LIMIT, not
//     against a count typed here. A family that GROWS must redden this suite on
//     the commit that grows it; a count typed here would be edited to match and
//     the growth would pass.
//
//  3. Every refusal path has a POSITIVE control next to it. "igComposeKey
//     refused" is worthless on its own -- a function that refuses everything
//     passes it -- so each refusal is paired with the one-byte-larger buffer
//     that must succeed.
//

#include <cstdio>
#include <cstring>

#include "../phase0-kext/IntelDisplayNames.hpp"

static int gChecks = 0;
static int gFailures = 0;

static void section(const char *name)
{
    std::printf("\n%s\n", name);
    for (const char *p = name; *p != '\0'; ++p)
        std::printf("-");
    std::printf("\n");
}

static void checkStr(const char *label, const char *got, const char *want)
{
    ++gChecks;
    if (got != 0 && want != 0 && std::strcmp(got, want) == 0) {
        std::printf("  ok   %-52s \"%s\"\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got \"%s\" want \"%s\"\n", label,
                    got  != 0 ? got  : "(null)",
                    want != 0 ? want : "(null)");
    }
}

static void checkBool(const char *label, bool got, bool want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s %s\n", label, got ? "true" : "false");
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got %s want %s\n", label,
                    got ? "true" : "false", want ? "true" : "false");
    }
}

static void checkU32(const char *label, IGN32 got, IGN32 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s %u\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got %u want %u\n", label, got, want);
    }
}

//
//  1. EVERY NAME PINNED TO A BARE LITERAL.
//
//  This is the half that catches a case arm returning a neighbouring family
//  member's string -- exhaustiveness below cannot, because "link" is not
//  "unknown" and would pass.
//
static void testLinkNames(void)
{
    section("1a. igLinkVerdictName -- literal per arm");

    checkStr("IG_LINK_OK",        igLinkVerdictName(0u), "ok");
    checkStr("IG_LINK_NO_TIMING", igLinkVerdictName(1u), "no-timing");
    checkStr("IG_LINK_NO_OUTPUT", igLinkVerdictName(2u), "no-output");
    checkStr("IG_LINK_NO_PORT",   igLinkVerdictName(3u), "no-port");
}

static void testModeNames(void)
{
    section("1b. igModeVerdictName -- literal per arm");

    checkStr("IG_MODE_OK",            igModeVerdictName(0u), "ok");
    checkStr("IG_MODE_IMPLAUSIBLE",   igModeVerdictName(1u), "implausible");
    checkStr("IG_MODE_BLANK_RANGE",   igModeVerdictName(2u), "blank-range");
    checkStr("IG_MODE_BLANK_OVERLAP", igModeVerdictName(3u), "blank-overlap");
    checkStr("IG_MODE_SYNC_RANGE",    igModeVerdictName(4u), "sync-range");
    checkStr("IG_MODE_SYNC_OUTSIDE",  igModeVerdictName(5u), "sync-outside");

    //
    // THE REGRESSION PIN. Verdict 6 is IG_MODE_SRC_MISMATCH: PIPESRC disagrees
    // with the active region the timing registers describe. That is the
    // byte-swapped PLANE_SIZE/PIPESRC shape handoff15 measured on real silicon,
    // and it is the most interesting verdict in the family. Before this suite
    // existed the switch had no arm for it and it published "unknown".
    //
    checkStr("IG_MODE_SRC_MISMATCH",  igModeVerdictName(6u), "src-mismatch");
}

static void testUsableNames(void)
{
    section("1c. igUsableVerdictName -- literal per arm");

    checkStr("IG_USABLE_OK",     igUsableVerdictName(0u), "ok");
    checkStr("IG_USABLE_LINK",   igUsableVerdictName(1u), "link");
    checkStr("IG_USABLE_TIMING", igUsableVerdictName(2u), "timing");
    checkStr("IG_USABLE_PLANE",  igUsableVerdictName(3u), "plane");
}

static void testInheritNames(void)
{
    section("1d. igInheritVerdictName -- literal per arm");

    checkStr("IG_FB_INHERIT_OK",         igInheritVerdictName(0u), "ok");
    checkStr("IG_FB_INHERIT_DISABLED",   igInheritVerdictName(1u), "disabled");
    checkStr("IG_FB_INHERIT_COMPRESSED", igInheritVerdictName(2u), "compressed");
    checkStr("IG_FB_INHERIT_FORMAT",     igInheritVerdictName(3u), "format");
    checkStr("IG_FB_INHERIT_STRIDE",     igInheritVerdictName(4u), "stride");
    checkStr("IG_FB_INHERIT_GEOMETRY",   igInheritVerdictName(5u), "geometry");
}

//
//  2. EXHAUSTIVENESS, WALKED AGAINST THE FAMILY'S OWN LIMIT.
//
//  For each family: every verdict from 0 to _VERDICT_LIMIT inclusive must have a
//  name, and that name must NOT be "unknown". Then LIMIT+1 -- a value no decoder
//  returns -- must BE "unknown", which is the negative control proving the loop
//  above is testing something a wrong answer could fail.
//
//  This is the check that found the missing SRC_MISMATCH arm. It is deliberately
//  written against IG_*_VERDICT_LIMIT rather than a literal count, so adding a
//  seventh mode verdict without a switch arm reddens this suite immediately.
//
//
//  1e. igSampleVerdictName -- the group-read family.
//
//  Two arms, which is the smallest a family can be and still be a family. It is
//  pinned exactly like the other four rather than waved through as "obviously a
//  bool", because the string is what lands in the IORegistry: an operator reads
//  "refused" there, not a 1.
//
static void testSampleNames(void)
{
    section("1e. igSampleVerdictName -- literal per arm");

    checkStr("IG_SAMPLE_OK",      igSampleVerdictName(0u), "ok");
    checkStr("IG_SAMPLE_REFUSED", igSampleVerdictName(1u), "refused");

    //
    //  The two arms must not be the same string. With only two of them a
    //  copy-paste arm is a coin flip, and "has a name" passes either way.
    //
    checkBool("the two arms differ",
              std::strcmp(igSampleVerdictName(0u),
                          igSampleVerdictName(1u)) != 0, true);

    //
    //  "ok" is shared with the other four families on purpose -- one word per
    //  verdict across the whole IORegistry. Pinned so the sharing is a decision
    //  somebody made rather than an accident nobody noticed.
    //
    checkBool("IG_SAMPLE_OK reads the same word as IG_LINK_OK",
              std::strcmp(igSampleVerdictName(0u),
                          igLinkVerdictName(0u)) == 0, true);
}

//
//  Seven arms, and the largest verdict family in the Intel track.
//  IG_SCAN_VERDICT_LIMIT is 6 and IG_SCAN_ERR_OUTSIDE_APERTURE is 6 -- the limit
//  is INCLUSIVE, and so is every other family's. That is measured 2026-08-19
//  rather than remembered: each bound is spelled "the largest verdict above" and
//  EQUALS its largest member (IG_MODE_VERDICT_LIMIT 6 == IG_MODE_SRC_MISMATCH,
//  IG_FB_INHERIT_VERDICT_LIMIT 5 == IG_FB_INHERIT_GEOMETRY, IG_READ_VERDICT_LIMIT
//  4 == IG_READ_PAST_MAPPING), and tools/check-registers.py:2440 compares limit
//  against max(codes) as an EQUALITY per family, so an exclusive limit could not
//  pass the register gate at all.
//
//  An earlier revision of this comment called IG_MODE_ and IG_FB_INHERIT_
//  exclusive and said the `<=` walk below "absorbs" that via a genuinely unnamed
//  LIMIT+1 arm. Both halves were false. The walk is `<=` for every family because
//  every limit is inclusive; what the LIMIT+1 assertion pins is the SENTINEL, one
//  past the last real verdict. A comment that gets the convention backwards is
//  handoff20 SS1 in prose form -- that defect WAS igModeVerdictName() carrying
//  arms 0..5 against an inclusive limit of 6, publishing "unknown" for
//  IG_MODE_SRC_MISMATCH with all four gates green. A `<` written from habit here
//  would stop before OUTSIDE_APERTURE: the one verdict in this family that is a
//  fact about how much memory got mapped rather than a bug in this driver, and
//  therefore the one an operator most needs to read as a word.
//
static void testScanNames(void)
{
    section("1f. igScanVerdictName -- literal per arm");

    checkStr("IG_SCAN_OK",                   igScanVerdictName(0u), "ok");
    checkStr("IG_SCAN_ERR_NO_SURFACE",       igScanVerdictName(1u), "no-surface");
    checkStr("IG_SCAN_ERR_UNALIGNED",        igScanVerdictName(2u), "unaligned");
    checkStr("IG_SCAN_ERR_ZERO_GEOMETRY",    igScanVerdictName(3u), "zero-geometry");
    checkStr("IG_SCAN_ERR_ROW_TOO_SHORT",    igScanVerdictName(4u), "row-too-short");
    checkStr("IG_SCAN_ERR_EXTENT_OVERFLOW",  igScanVerdictName(5u), "extent-overflow");
    checkStr("IG_SCAN_ERR_OUTSIDE_APERTURE", igScanVerdictName(6u), "outside-aperture");

    //
    //  The inclusive limit pinned as an EQUALITY against the last real verdict
    //  rather than left implicit in the walk. If somebody later renumbers the
    //  family or flips the convention to match the two exclusive ones, this
    //  reddens on the commit that does it rather than on the boot that publishes
    //  a number nobody can read.
    //
    checkStr("the limit itself names a real verdict",
             igScanVerdictName(IG_SCAN_VERDICT_LIMIT), "outside-aperture");

    //
    //  Same cross-family agreement check as the sample block: "ok" is one word in
    //  this tree, not six spellings of one idea.
    //
    checkBool("IG_SCAN_OK reads the same word as IG_LINK_OK",
              std::strcmp(igScanVerdictName(0u),
                          igLinkVerdictName(0u)) == 0, true);
}

static void testExhaustive(void)
{
    section("2. exhaustive over each family's own _VERDICT_LIMIT");

    for (IGN32 v = 0; v <= IG_LINK_VERDICT_LIMIT; v++) {
        char label[64];
        std::snprintf(label, sizeof(label), "link verdict %u is named", v);
        checkBool(label, std::strcmp(igLinkVerdictName(v), "unknown") != 0, true);
    }
    checkStr("link above limit is unknown",
             igLinkVerdictName(IG_LINK_VERDICT_LIMIT + 1u), "unknown");

    for (IGN32 v = 0; v <= IG_MODE_VERDICT_LIMIT; v++) {
        char label[64];
        std::snprintf(label, sizeof(label), "mode verdict %u is named", v);
        checkBool(label, std::strcmp(igModeVerdictName(v), "unknown") != 0, true);
    }
    checkStr("mode above limit is unknown",
             igModeVerdictName(IG_MODE_VERDICT_LIMIT + 1u), "unknown");

    for (IGN32 v = 0; v <= IG_USABLE_VERDICT_LIMIT; v++) {
        char label[64];
        std::snprintf(label, sizeof(label), "usable verdict %u is named", v);
        checkBool(label, std::strcmp(igUsableVerdictName(v), "unknown") != 0, true);
    }
    checkStr("usable above limit is unknown",
             igUsableVerdictName(IG_USABLE_VERDICT_LIMIT + 1u), "unknown");

    for (IGN32 v = 0; v <= IG_FB_INHERIT_VERDICT_LIMIT; v++) {
        char label[64];
        std::snprintf(label, sizeof(label), "inherit verdict %u is named", v);
        checkBool(label, std::strcmp(igInheritVerdictName(v), "unknown") != 0, true);
    }
    checkStr("inherit above limit is unknown",
             igInheritVerdictName(IG_FB_INHERIT_VERDICT_LIMIT + 1u), "unknown");

    //
    // A far-out value as well as LIMIT+1. IG_MMIO_INVALID is the shape a
    // register read returns when the offset was refused, and it is exactly the
    // value most likely to reach a name function by accident.
    //
    for (IGN32 v = 0; v <= IG_SAMPLE_VERDICT_LIMIT; v++) {
        char label[64];
        std::snprintf(label, sizeof(label), "sample verdict %u is named", v);
        checkBool(label, std::strcmp(igSampleVerdictName(v), "unknown") != 0, true);
    }
    checkStr("sample above limit is unknown",
             igSampleVerdictName(IG_SAMPLE_VERDICT_LIMIT + 1u), "unknown");

    //
    // The scan family's limit is INCLUSIVE, so `<=` walks 0..6 and every one of
    // the seven must be named. LIMIT+1 is 7, the first value outside the family.
    //
    for (IGN32 v = 0; v <= IG_SCAN_VERDICT_LIMIT; v++) {
        char label[64];
        std::snprintf(label, sizeof(label), "scan verdict %u is named", v);
        checkBool(label, std::strcmp(igScanVerdictName(v), "unknown") != 0, true);
    }
    checkStr("scan above limit is unknown",
             igScanVerdictName(IG_SCAN_VERDICT_LIMIT + 1u), "unknown");

    //
    // A far-out value as well as LIMIT+1. IG_MMIO_INVALID is the shape a
    // register read returns when the offset was refused, and it is exactly the
    // value most likely to reach a name function by accident.
    //
    checkStr("link 0xFFFFFFFF is unknown",    igLinkVerdictName(0xFFFFFFFFu), "unknown");
    checkStr("mode 0xFFFFFFFF is unknown",    igModeVerdictName(0xFFFFFFFFu), "unknown");
    checkStr("usable 0xFFFFFFFF is unknown",  igUsableVerdictName(0xFFFFFFFFu), "unknown");
    checkStr("inherit 0xFFFFFFFF is unknown", igInheritVerdictName(0xFFFFFFFFu), "unknown");
    checkStr("sample 0xFFFFFFFF is unknown",  igSampleVerdictName(0xFFFFFFFFu), "unknown");
    checkStr("scan 0xFFFFFFFF is unknown",    igScanVerdictName(0xFFFFFFFFu), "unknown");
}

//
//  3. NO TWO VERDICTS IN ONE FAMILY SHARE A NAME.
//
//  Exhaustiveness accepts a switch where two arms return the same literal, and
//  that is precisely the copy-paste mistake a table of similar strings invites.
//  Two verdicts publishing the same string is indistinguishable in the
//  IORegistry from the decoder never producing one of them.
//
static void testNoDuplicateNames(void)
{
    section("3. names are unique within each family");

    for (IGN32 a = 0; a <= IG_MODE_VERDICT_LIMIT; a++) {
        for (IGN32 b = a + 1u; b <= IG_MODE_VERDICT_LIMIT; b++) {
            char label[80];
            std::snprintf(label, sizeof(label), "mode %u vs %u differ", a, b);
            checkBool(label,
                      std::strcmp(igModeVerdictName(a),
                                  igModeVerdictName(b)) != 0, true);
        }
    }

    for (IGN32 a = 0; a <= IG_FB_INHERIT_VERDICT_LIMIT; a++) {
        for (IGN32 b = a + 1u; b <= IG_FB_INHERIT_VERDICT_LIMIT; b++) {
            char label[80];
            std::snprintf(label, sizeof(label), "inherit %u vs %u differ", a, b);
            checkBool(label,
                      std::strcmp(igInheritVerdictName(a),
                                  igInheritVerdictName(b)) != 0, true);
        }
    }

    //
    // The scan family is the longest of the six and its arms are the most
    // similar-looking -- "row-too-short" beside "extent-overflow" beside
    // "outside-aperture", all three describing a size that does not fit. Those
    // are exactly the three a copy-paste arm would collapse, and the three whose
    // collapse an operator could not detect from the IORegistry.
    //
    for (IGN32 a = 0; a <= IG_SCAN_VERDICT_LIMIT; a++) {
        for (IGN32 b = a + 1u; b <= IG_SCAN_VERDICT_LIMIT; b++) {
            char label[80];
            std::snprintf(label, sizeof(label), "scan %u vs %u differ", a, b);
            checkBool(label,
                      std::strcmp(igScanVerdictName(a),
                                  igScanVerdictName(b)) != 0, true);
        }
    }

    for (IGN32 a = 0; a <= IG_LINK_VERDICT_LIMIT; a++) {
        for (IGN32 b = a + 1u; b <= IG_LINK_VERDICT_LIMIT; b++) {
            char label[80];
            std::snprintf(label, sizeof(label), "link %u vs %u differ", a, b);
            checkBool(label,
                      std::strcmp(igLinkVerdictName(a),
                                  igLinkVerdictName(b)) != 0, true);
        }
    }

    for (IGN32 a = 0; a <= IG_USABLE_VERDICT_LIMIT; a++) {
        for (IGN32 b = a + 1u; b <= IG_USABLE_VERDICT_LIMIT; b++) {
            char label[80];
            std::snprintf(label, sizeof(label), "usable %u vs %u differ", a, b);
            checkBool(label,
                      std::strcmp(igUsableVerdictName(a),
                                  igUsableVerdictName(b)) != 0, true);
        }
    }

    for (IGN32 a = 0; a <= IG_SAMPLE_VERDICT_LIMIT; a++) {
        for (IGN32 b = a + 1u; b <= IG_SAMPLE_VERDICT_LIMIT; b++) {
            char label[80];
            std::snprintf(label, sizeof(label), "sample %u vs %u differ", a, b);
            checkBool(label,
                      std::strcmp(igSampleVerdictName(a),
                                  igSampleVerdictName(b)) != 0, true);
        }
    }
}

//
//  4. TRANSCODER NAMES.
//
//  Pinned to letters, not to indices, and DSI0/DSI1 are named even though
//  IG_TRANS_SCAN_COUNT stops at four. The probe excludes DSI because those pages
//  are off the 0x1000 plane ladder, not because they are absent -- so a name
//  function that returned "?" for them would make a future scan-count change
//  misreport instead of failing.
//
static void testTransNames(void)
{
    section("4. igTransName");

    checkStr("transcoder 0", igTransName(0u), "A");
    checkStr("transcoder 1", igTransName(1u), "B");
    checkStr("transcoder 2", igTransName(2u), "C");
    checkStr("transcoder 3", igTransName(3u), "D");
    checkStr("transcoder 4", igTransName(4u), "DSI0");
    checkStr("transcoder 5", igTransName(5u), "DSI1");

    //
    // Negative control, and the loop below proves the six above are not all
    // reachable by accident: every index inside IG_TRANSCODER_COUNT must have a
    // name, and the first index outside it must not.
    //
    checkStr("transcoder 6 refused", igTransName(6u), "?");
    checkStr("transcoder 0xFFFFFFFF refused", igTransName(0xFFFFFFFFu), "?");

    for (IGN32 t = 0; t < IG_TRANSCODER_COUNT; t++) {
        char label[64];
        std::snprintf(label, sizeof(label), "transcoder %u inside COUNT is named", t);
        checkBool(label, std::strcmp(igTransName(t), "?") != 0, true);
    }
    checkStr("first index outside COUNT refused",
             igTransName(IG_TRANSCODER_COUNT), "?");

    //
    // Uniqueness, for the same reason as the verdict families: two transcoders
    // sharing a letter would publish one unit's verdicts over another's.
    //
    for (IGN32 a = 0; a < IG_TRANSCODER_COUNT; a++) {
        for (IGN32 b = a + 1u; b < IG_TRANSCODER_COUNT; b++) {
            char label[80];
            std::snprintf(label, sizeof(label), "transcoder %u vs %u differ", a, b);
            checkBool(label,
                      std::strcmp(igTransName(a), igTransName(b)) != 0, true);
        }
    }
}

//
//  5. KEY COMPOSITION.
//
//  igComposeKey() replaced snprintf. The point of the replacement is that
//  snprintf TRUNCATES and reports it only in a return value, so a caller that
//  sized a buffer slightly short publishes "IGTranscoderDSI0-Inher" -- a key
//  that looks deliberate, sorts next to the real one, and is wrong. This
//  refuses, and on refusal writes an EMPTY string rather than a partial one.
//
static void testComposeKey(void)
{
    section("5. igComposeKey");

    char buf[IG_KEY_BUFFER_BYTES];

    checkBool("compose succeeds",
              igComposeKey(buf, sizeof(buf), "IGTranscoder", "A", "-Link"), true);
    checkStr("composed key", buf, "IGTranscoderA-Link");

    checkBool("compose succeeds (DSI0)",
              igComposeKey(buf, sizeof(buf), "IGTranscoder", "DSI0", "-Height"), true);
    checkStr("composed key (DSI0)", buf, "IGTranscoderDSI0-Height");

    //
    // THE EXACT-FIT PAIR. "ABC" is three characters, so four bytes is exactly
    // enough and three is one short. Getting this edge wrong in the lenient
    // direction is a one-byte overrun of a stack buffer in the kernel, so both
    // sides are pinned and the successful side is the positive control that
    // stops "it refused" from passing vacuously.
    //
    char tight[4];
    checkBool("exact fit succeeds",
              igComposeKey(tight, sizeof(tight), "A", "B", "C"), true);
    checkStr("exact fit content", tight, "ABC");

    char short1[3];
    checkBool("one byte short refuses",
              igComposeKey(short1, sizeof(short1), "A", "B", "C"), false);
    checkStr("refusal writes empty, not partial", short1, "");

    //
    // A refusal must not leave the caller's PREVIOUS key in place either -- that
    // would republish the last unit's key under this unit's value, which is
    // worse than an empty one because it is plausible.
    //
    char reused[8];
    checkBool("first compose into small buffer",
              igComposeKey(reused, sizeof(reused), "IG", "A", "-Lnk"), true);
    checkStr("first content", reused, "IGA-Lnk");
    checkBool("second compose overflows",
              igComposeKey(reused, sizeof(reused), "IGTranscoder", "A", "-Link"), false);
    checkStr("overflow cleared the stale key", reused, "");

    //
    // Degenerate arguments. A zero-size buffer cannot even hold a NUL, so it is
    // refused without being touched; null parts are refused after the buffer has
    // been emptied, so a caller that ignores the result still publishes nothing
    // rather than whatever was on the stack.
    //
    checkBool("zero size refuses",
              igComposeKey(buf, 0u, "IG", "A", "-Link"), false);
    checkBool("null out refuses",
              igComposeKey(0, sizeof(buf), "IG", "A", "-Link"), false);

    buf[0] = 'X';
    checkBool("null prefix refuses",
              igComposeKey(buf, sizeof(buf), 0, "A", "-Link"), false);
    checkStr("null prefix emptied the buffer", buf, "");

    buf[0] = 'X';
    checkBool("null unit refuses",
              igComposeKey(buf, sizeof(buf), "IG", 0, "-Link"), false);
    checkStr("null unit emptied the buffer", buf, "");

    buf[0] = 'X';
    checkBool("null suffix refuses",
              igComposeKey(buf, sizeof(buf), "IG", "A", 0), false);
    checkStr("null suffix emptied the buffer", buf, "");

    //
    // Empty parts are legal, not degenerate: they compose to the concatenation
    // of the rest. This matters because it separates "refused" from "produced
    // nothing" -- a composer that treated "" as an error would refuse a key it
    // could have built.
    //
    checkBool("empty suffix succeeds",
              igComposeKey(buf, sizeof(buf), "IGTranscoder", "A", ""), true);
    checkStr("empty suffix content", buf, "IGTranscoderA");
}

//
//  6. THE TWO KEYS THE PROBE ACTUALLY PUBLISHES.
//
//  Pinned as whole literal strings rather than assembled here, because
//  assembling the expectation from the same prefix the function uses is the
//  cancelling-out mistake rule 1 exists to prevent. If the prefix ever changes,
//  an operator's grep breaks and these are the checks that say so.
//
static void testUnitKeys(void)
{
    section("6. igTranscoderKey / igPlaneKey");

    char buf[IG_KEY_BUFFER_BYTES];

    checkBool("transcoder key A",
              igTranscoderKey(buf, sizeof(buf), 0u, "-Link"), true);
    checkStr("transcoder key A content", buf, "IGTranscoderA-Link");

    checkBool("transcoder key D",
              igTranscoderKey(buf, sizeof(buf), 3u, "-Usable"), true);
    checkStr("transcoder key D content", buf, "IGTranscoderD-Usable");

    checkBool("plane key A",
              igPlaneKey(buf, sizeof(buf), 0u, "1-Inherit"), true);
    checkStr("plane key A content", buf, "IGPlaneA1-Inherit");

    checkBool("plane key C",
              igPlaneKey(buf, sizeof(buf), 2u, "1-Height"), true);
    checkStr("plane key C content", buf, "IGPlaneC1-Height");

    //
    // A key built for a transcoder the name function refuses must not silently
    // become "IGTranscoder?-Link". It composes -- "?" is a string like any other
    // -- and the point of pinning it is that the '?' is VISIBLE in the
    // IORegistry rather than being an index an operator would read as real.
    //
    checkBool("out-of-range unit still composes",
              igTranscoderKey(buf, sizeof(buf), 99u, "-Link"), true);
    checkStr("out-of-range unit is visibly wrong", buf, "IGTranscoder?-Link");

    //
    // THE BUFFER SIZE IS SUFFICIENT FOR EVERY REAL KEY. IG_KEY_BUFFER_BYTES is
    // the constant callers size their stack buffers from, so a suffix added to
    // the probe that overflows it has to fail HERE rather than in the kernel.
    // The longest unit name is DSI0 even though the scan stops at D, because the
    // scan count is a policy this suite must not assume.
    //
    static const char *suffixes[] = {
        "-Link", "-Mode", "-Usable", "-Port", "-Width", "-Height",
    };
    static const char *planeSuffixes[] = {
        "1-Inherit", "1-Width", "1-Height", "1-Stride", "1-Bytes", "1-Range",
    };

    for (IGN32 t = 0; t < IG_TRANSCODER_COUNT; t++) {
        for (IGN32 s = 0; s < 6u; s++) {
            char label[96];
            std::snprintf(label, sizeof(label), "transcoder %u %s fits",
                          t, suffixes[s]);
            checkBool(label,
                      igTranscoderKey(buf, sizeof(buf), t, suffixes[s]), true);
        }
        for (IGN32 s = 0; s < 6u; s++) {
            char label[96];
            std::snprintf(label, sizeof(label), "plane %u %s fits",
                          t, planeSuffixes[s]);
            checkBool(label,
                      igPlaneKey(buf, sizeof(buf), t, planeSuffixes[s]), true);
        }
    }

    //
    // And the negative control for that: one byte less than the longest real key
    // needs must refuse, so "every key fits" is a measurement rather than a
    // statement about a buffer nothing could overflow.
    //
    checkBool("longest key composes at the declared size",
              igTranscoderKey(buf, IG_KEY_BUFFER_BYTES, 4u, "-Height"), true);
    checkStr("longest key content", buf, "IGTranscoderDSI0-Height");
    checkU32("longest key length", (IGN32) std::strlen(buf), 23u);
    checkBool("longest key refused one byte short",
              igTranscoderKey(buf, 23u, 4u, "-Height"), false);
}

int main(void)
{
    testLinkNames();
    testModeNames();
    testUsableNames();
    testInheritNames();
    testSampleNames();
    testScanNames();
    testExhaustive();
    testNoDuplicateNames();
    testTransNames();
    testComposeKey();
    testUnitKeys();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
