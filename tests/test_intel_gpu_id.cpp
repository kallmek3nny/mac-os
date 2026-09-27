//
//  test_intel_gpu_id.cpp
//  Host-side suite for phase0-kext/IntelGpuId.hpp.
//
//  Compiles the IDENTICAL header the kext will use, under MSVC, on a machine
//  with no Intel GPU. Every function in that header is a total function of its
//  arguments -- no fixture, no journal, no register file.
//
//  WHAT EACH SECTION IS ACTUALLY TRYING TO CATCH
//
//  1. Every device ID is pinned to a BARE LITERAL, never to the #define that
//     produced it. This is the single most important property in the file and
//     the reason it is written the tedious way. A row like
//
//         checkU32("GT1[1]", igTglGt1Id(1u), IG_TGL_GT1_ID_1);
//
//     passes under ANY value of IG_TGL_GT1_ID_1, including a typo, because the
//     accessor reads it back through the same #define the expectation is
//     spelled with. The project has already been bitten by exactly this --
//     buildBlob() wrote every field through the #define its accessor read back,
//     so fifteen offsets were undetectable by construction. So every expectation
//     here is the hex from reference/i915/i915_pciids.h typed out again.
//
//  2. The transcription is checked against the vendored header as a SET, not
//     just entry by entry. Entry-by-entry rows catch a wrong digit; they do not
//     catch a DUPLICATED entry (two slots holding 0x9A68, one real ID lost) or
//     a table whose count disagrees with the entries it can actually return.
//     So there are rows for pairwise distinctness and for the count agreeing
//     with where the accessors start refusing.
//
//  3. The observed value and the vendored value are asserted to AGREE, and
//     each is pinned separately first. IG_DEVICE_OBSERVED_2026_08_16 comes from
//     igpu-results/tgl-capture-2026-08-16-022556.txt; IG_TGL_GT1_ID_1 comes
//     from reference/i915/i915_pciids.h. They are two independent readings that
//     happen to describe the same part. Pinning both to bare literals and THEN
//     asserting equality means a mistake in either one goes red -- whereas
//     defining one from the other, or testing only their equality, would let two
//     matching errors cancel.
//
//  4. The match predicate is tested with SEPARATING inputs. A supported device
//     ID under the wrong vendor must be refused, and a wrong device ID under the
//     right vendor must be refused; testing only (Intel, 0x9A68)->true and
//     (0, 0)->false would pass even if the function ignored the vendor entirely.
//
//  5. The all-ones input is a named case rather than an incidental one. An
//     absent PCI function reads 0xFFFFFFFF on both words, so that is the exact
//     input a probe pointed at an empty slot produces -- the most likely wrong
//     input this code will ever see.
//
//  6. Adjacent-value negative controls probe the FAR side of each boundary.
//     Every ID here is checked against value+1 and value-1, because a membership
//     test widened by a mask or a range would still answer correctly on the
//     exact values and wrongly on their neighbours. The project's own
//     cross-checks-probe-the-far-side lesson came from four adjacent-bit checks
//     that all probed the same side and survived a mask widened 0x1 -> 0x3.
//

#include <cstdio>
#include <cstring>

#include "../phase0-kext/IntelGpuId.hpp"

static int gChecks   = 0;
static int gFailures = 0;

static void section(const char *name)
{
    std::printf("\n%s\n", name);
    for (size_t i = 0; i < std::strlen(name); ++i)
        std::printf("-");
    std::printf("\n");
}

static void checkU32(const char *label, IGID32 got, IGID32 want)
{
    ++gChecks;
    if (got == want) {
        std::printf("  ok   %-52s 0x%08X\n", label, got);
    } else {
        ++gFailures;
        std::printf("  FAIL %-52s got 0x%08X want 0x%08X\n", label, got, want);
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


//
//  ---- 1. the transcription, against bare literals ----------------------------
//
//  Every expectation below is the hex as it appears in
//  reference/i915/i915_pciids.h lines 623-636, retyped. Do NOT rewrite these to
//  use the IG_TGL_* names -- that is precisely what makes the check vacuous.
//
static void testGt1Table(void)
{
    section("1. INTEL_TGL_GT1_IDS transcription (bare literals)");

    checkU32("GT1 slot 0 is 0x9A60", igTglGt1Id(0u), 0x9A60u);
    checkU32("GT1 slot 1 is 0x9A68", igTglGt1Id(1u), 0x9A68u);
    checkU32("GT1 slot 2 is 0x9A70", igTglGt1Id(2u), 0x9A70u);

    //
    //  The #defines themselves, also against bare literals. The accessor rows
    //  above would survive an accessor that ignored its tables entirely and
    //  returned three hardcoded values, so these pin the constants a caller
    //  might use directly.
    //
    checkU32("IG_TGL_GT1_ID_0 define", IG_TGL_GT1_ID_0, 0x9A60u);
    checkU32("IG_TGL_GT1_ID_1 define", IG_TGL_GT1_ID_1, 0x9A68u);
    checkU32("IG_TGL_GT1_ID_2 define", IG_TGL_GT1_ID_2, 0x9A70u);

    checkU32("the GT1 list is three long", IG_TGL_GT1_ID_COUNT, 3u);

    //
    //  Refusal at the edge, not merely somewhere past it. Index 3 is the first
    //  invalid one; a bound written as <= would pass an index-99 test and fail
    //  only here.
    //
    checkU32("GT1 slot 3 refuses", igTglGt1Id(3u), IG_PCI_ID_INVALID);
    checkU32("GT1 slot 0xFFFFFFFF refuses", igTglGt1Id(0xFFFFFFFFu),
             IG_PCI_ID_INVALID);
}

static void testGt2Table(void)
{
    section("2. INTEL_TGL_GT2_IDS transcription (bare literals)");

    checkU32("GT2 slot 0 is 0x9A40", igTglGt2Id(0u), 0x9A40u);
    checkU32("GT2 slot 1 is 0x9A49", igTglGt2Id(1u), 0x9A49u);
    checkU32("GT2 slot 2 is 0x9A59", igTglGt2Id(2u), 0x9A59u);
    checkU32("GT2 slot 3 is 0x9A78", igTglGt2Id(3u), 0x9A78u);
    checkU32("GT2 slot 4 is 0x9AC0", igTglGt2Id(4u), 0x9AC0u);
    checkU32("GT2 slot 5 is 0x9AC9", igTglGt2Id(5u), 0x9AC9u);
    checkU32("GT2 slot 6 is 0x9AD9", igTglGt2Id(6u), 0x9AD9u);
    checkU32("GT2 slot 7 is 0x9AF8", igTglGt2Id(7u), 0x9AF8u);

    checkU32("IG_TGL_GT2_ID_0 define", IG_TGL_GT2_ID_0, 0x9A40u);
    checkU32("IG_TGL_GT2_ID_7 define", IG_TGL_GT2_ID_7, 0x9AF8u);

    checkU32("the GT2 list is eight long", IG_TGL_GT2_ID_COUNT, 8u);
    checkU32("GT2 slot 8 refuses", igTglGt2Id(8u), IG_PCI_ID_INVALID);
}


//
//  ---- 2. the tables as SETS ---------------------------------------------------
//
static void testTableShape(void)
{
    section("3. the union, and the tables as sets rather than slots");

    //
    //  IG_TGL_ID_COUNT is spelled as a literal 11 in the header rather than as
    //  GT1_COUNT + GT2_COUNT, so this row is a real check rather than an
    //  identity. If a table grows and its own count follows but the union count
    //  does not, igTglId() silently stops returning the tail.
    //
    checkU32("the union count is eleven", IG_TGL_ID_COUNT, 11u);
    checkBool("and it equals GT1 + GT2",
              IG_TGL_GT1_ID_COUNT + IG_TGL_GT2_ID_COUNT == IG_TGL_ID_COUNT,
              true);

    //
    //  INTEL_TGL_IDS order: the whole GT1 list, then the whole GT2 list.
    //
    checkU32("union slot 0 is the first GT1 entry", igTglId(0u), 0x9A60u);
    checkU32("union slot 2 is the last GT1 entry", igTglId(2u), 0x9A70u);
    checkU32("union slot 3 is the first GT2 entry", igTglId(3u), 0x9A40u);
    checkU32("union slot 10 is the last GT2 entry", igTglId(10u), 0x9AF8u);
    checkU32("union slot 11 refuses", igTglId(11u), IG_PCI_ID_INVALID);

    //
    //  Pairwise distinctness. An entry-by-entry suite cannot see a duplicate:
    //  if slot 4 and slot 5 both held 0x9AC0, every individual row would still
    //  be checked against its own expectation and one real device would just be
    //  missing from the driver's world.
    //
    unsigned duplicates = 0u;
    for (IGID32 i = 0u; i < IG_TGL_ID_COUNT; ++i) {
        for (IGID32 j = i + 1u; j < IG_TGL_ID_COUNT; ++j) {
            if (igTglId(i) == igTglId(j))
                ++duplicates;
        }
    }
    checkU32("no two union slots hold the same ID", duplicates, 0u);

    //
    //  Every slot the count promises must actually return something, and
    //  nothing beyond it may. Counted rather than spot-checked, so this stays
    //  true if the tables grow.
    //
    unsigned valid = 0u;
    for (IGID32 i = 0u; i < IG_TGL_ID_COUNT; ++i) {
        if (igTglId(i) != IG_PCI_ID_INVALID)
            ++valid;
    }
    checkU32("every promised slot returns an ID", valid, IG_TGL_ID_COUNT);

    //
    //  Both lists are 16-bit PCI device IDs. A transcription that dropped or
    //  gained a digit would very likely leave this range, and nothing else here
    //  would notice a 0x99A68.
    //
    unsigned oversized = 0u;
    for (IGID32 i = 0u; i < IG_TGL_ID_COUNT; ++i) {
        if (igTglId(i) > 0xFFFFu)
            ++oversized;
    }
    checkU32("every ID fits in 16 bits", oversized, 0u);

    //
    //  Intel's Tiger Lake block is 0x9Axx throughout. This is a WEAKER claim
    //  than the per-entry rows and is here only to catch a whole entry pasted
    //  from the wrong macro -- the JSL list one screen up in the same vendored
    //  header is 0x4Exx, which this would catch and the count rows would not.
    //
    unsigned outOfFamily = 0u;
    for (IGID32 i = 0u; i < IG_TGL_ID_COUNT; ++i) {
        if ((igTglId(i) & 0xFF00u) != 0x9A00u)
            ++outOfFamily;
    }
    checkU32("every ID is in the 0x9Axx family", outOfFamily, 0u);
}


//
//  ---- 3. observed vs vendored -------------------------------------------------
//
static void testObservedAgreesWithVendor(void)
{
    section("4. the capture and the vendored table, pinned separately");

    //
    //  Two independent readings. Left column: what
    //  igpu-results/tgl-capture-2026-08-16-022556.txt printed on the target
    //  laptop ("device   0x9a68"). Right column: what
    //  reference/i915/i915_pciids.h spells at line 625. Each is pinned to a
    //  bare literal FIRST so that a mistake in either one is visible on its own
    //  row -- and only then are they asserted to agree.
    //
    checkU32("the capture read 0x9A68", IG_DEVICE_OBSERVED_2026_08_16, 0x9A68u);
    checkU32("the vendored GT1 table spells 0x9A68", IG_TGL_GT1_ID_1, 0x9A68u);
    checkBool("so the two independent readings agree",
              IG_DEVICE_OBSERVED_2026_08_16 == IG_TGL_GT1_ID_1, true);

    //
    //  And the part the capture saw must actually be one this driver matches.
    //  If that ever goes red, either the transcription is wrong or the machine
    //  is not what the capture says it is.
    //
    checkBool("the observed part is a device we claim to support",
              igIsTglDevice(IG_DEVICE_OBSERVED_2026_08_16), true);
    checkU32("and it classifies as GT1",
             igTglGtClass(IG_DEVICE_OBSERVED_2026_08_16), IG_TGL_GT_1);

    //
    //  The observed BDF, also against bare literals. sysfs printed
    //  0000:00:02.0.
    //
    checkU32("observed bus is 0", IG_IGD_BUS_OBSERVED_2026_08_16, 0x00u);
    checkU32("observed device is 2", IG_IGD_DEVICE_OBSERVED_2026_08_16, 0x02u);
    checkU32("observed function is 0", IG_IGD_FUNCTION_OBSERVED_2026_08_16, 0x00u);

    checkBool("00:02.0 is the location the capture recorded",
              igIsObservedIgdLocation(0x00u, 0x02u, 0x00u), true);

    //
    //  One separating input per field, so the predicate cannot be a constant
    //  true and cannot be ignoring a field. 00:02.1 in particular is a real
    //  address on real machines, not an absurd one.
    //
    checkBool("a different bus is not it",
              igIsObservedIgdLocation(0x01u, 0x02u, 0x00u), false);
    checkBool("a different device is not it",
              igIsObservedIgdLocation(0x00u, 0x03u, 0x00u), false);
    checkBool("a different function is not it",
              igIsObservedIgdLocation(0x00u, 0x02u, 0x01u), false);
}


//
//  ---- 4. membership, with far-side neighbours --------------------------------
//
static void testMembership(void)
{
    section("5. membership, and the neighbours of every entry");

    checkBool("0x9A60 is GT1", igIsTglGt1(0x9A60u), true);
    checkBool("0x9A68 is GT1", igIsTglGt1(0x9A68u), true);
    checkBool("0x9A70 is GT1", igIsTglGt1(0x9A70u), true);
    checkBool("0x9A40 is NOT GT1", igIsTglGt1(0x9A40u), false);

    checkBool("0x9A40 is GT2", igIsTglGt2(0x9A40u), true);
    checkBool("0x9AF8 is GT2", igIsTglGt2(0x9AF8u), true);
    checkBool("0x9A68 is NOT GT2", igIsTglGt2(0x9A68u), false);

    //
    //  The two classes must not overlap. Both membership functions being true
    //  for one ID would make igTglGtClass()'s answer depend on the order of its
    //  two ifs -- a silent, order-dependent classification.
    //
    unsigned inBoth = 0u;
    for (IGID32 i = 0u; i < IG_TGL_ID_COUNT; ++i) {
        if (igIsTglGt1(igTglId(i)) && igIsTglGt2(igTglId(i)))
            ++inBoth;
    }
    checkU32("no ID is in both GT classes", inBoth, 0u);

    //
    //  Every table entry answers yes, and every entry's IMMEDIATE NEIGHBOURS on
    //  BOTH sides answer no. A membership test written as a range or a masked
    //  compare -- (device & 0xFFF0) == 0x9A60, say -- answers correctly on all
    //  eleven exact values and wrongly on 0x9A61..0x9A67. Probing only one side
    //  of each value is how four green cross-checks once survived a mask
    //  widened from 0x1 to 0x3.
    //
    //  Skipped where a neighbour is itself a real entry: 0x9A48+1 == 0x9A49 is
    //  in the table, so asserting it false would be asserting a defect.
    //
    unsigned unrecognised = 0u;
    for (IGID32 i = 0u; i < IG_TGL_ID_COUNT; ++i) {
        if (!igIsTglDevice(igTglId(i)))
            ++unrecognised;
    }
    checkU32("every table entry is recognised", unrecognised, 0u);

    unsigned strayHits = 0u;
    for (IGID32 i = 0u; i < IG_TGL_ID_COUNT; ++i) {
        IGID32 id = igTglId(i);

        IGID32 above = id + 1u;
        IGID32 below = id - 1u;

        bool aboveIsReal = false;
        bool belowIsReal = false;
        for (IGID32 j = 0u; j < IG_TGL_ID_COUNT; ++j) {
            if (igTglId(j) == above) aboveIsReal = true;
            if (igTglId(j) == below) belowIsReal = true;
        }

        if (!aboveIsReal && igIsTglDevice(above))
            ++strayHits;
        if (!belowIsReal && igIsTglDevice(below))
            ++strayHits;
    }
    checkU32("no non-entry neighbour is recognised", strayHits, 0u);

    //
    //  A handful of the neighbours spelled out, so the loop above is not the
    //  only thing standing between a widened match and a green run. If the loop
    //  itself is ever broken, these still go red.
    //
    checkBool("0x9A5F is not a TGL device", igIsTglDevice(0x9A5Fu), false);
    checkBool("0x9A61 is not a TGL device", igIsTglDevice(0x9A61u), false);
    checkBool("0x9A67 is not a TGL device", igIsTglDevice(0x9A67u), false);
    checkBool("0x9A69 is not a TGL device", igIsTglDevice(0x9A69u), false);
    checkBool("0x9A71 is not a TGL device", igIsTglDevice(0x9A71u), false);
    checkBool("0x9AF9 is not a TGL device", igIsTglDevice(0x9AF9u), false);

    //
    //  0x4E55 is a Jasper Lake ID from INTEL_JSL_IDS, seven lines above the TGL
    //  block in the same vendored header. It is the exact wrong-paste this
    //  file is most likely to suffer.
    //
    checkBool("a Jasper Lake ID is not a TGL device",
              igIsTglDevice(0x4E55u), false);

    checkU32("an unknown ID has no GT class", igTglGtClass(0x1234u),
             IG_TGL_GT_NONE);
    checkU32("IG_TGL_GT_NONE is zero", IG_TGL_GT_NONE, 0u);
    checkU32("IG_TGL_GT_1 is one", IG_TGL_GT_1, 1u);
    checkU32("IG_TGL_GT_2 is two", IG_TGL_GT_2, 2u);
}


//
//  ---- 5. the match predicate --------------------------------------------------
//
static void testMatchPredicate(void)
{
    section("6. igIsSupportedDevice: vendor AND device");

    checkU32("IG_PCI_VENDOR_INTEL is 0x8086", IG_PCI_VENDOR_INTEL, 0x8086u);
    checkU32("IG_PCI_ID_INVALID is all ones", IG_PCI_ID_INVALID, 0xFFFFFFFFu);

    checkBool("Intel + the observed part matches",
              igIsSupportedDevice(0x8086u, 0x9A68u), true);
    checkBool("Intel + a GT2 part matches",
              igIsSupportedDevice(0x8086u, 0x9AF8u), true);

    //
    //  The separating pair. Both of these would pass a function that checked
    //  only one of its two arguments, and exactly one of them would pass a
    //  function that checked only the other.
    //
    checkBool("NVIDIA's vendor ID with a TGL device ID is refused",
              igIsSupportedDevice(0x10DEu, 0x9A68u), false);
    checkBool("Intel's vendor ID with a non-TGL device ID is refused",
              igIsSupportedDevice(0x8086u, 0x4E55u), false);

    //
    //  The empty-slot read. Both words all-ones is what PCI config space
    //  returns for a function that is not there, so this is the single most
    //  likely wrong input in production -- not an edge case.
    //
    checkBool("an absent function (all ones on both words) is refused",
              igIsSupportedDevice(IG_PCI_ID_INVALID, IG_PCI_ID_INVALID), false);
    checkBool("all-ones vendor alone is refused",
              igIsSupportedDevice(IG_PCI_ID_INVALID, 0x9A68u), false);
    checkBool("all-ones device alone is refused",
              igIsSupportedDevice(0x8086u, IG_PCI_ID_INVALID), false);

    checkBool("a zeroed read is refused",
              igIsSupportedDevice(0x0000u, 0x0000u), false);

    //
    //  And the predicate must accept every entry in the table under the right
    //  vendor -- otherwise a device could be in the list and still never match.
    //
    unsigned unmatched = 0u;
    for (IGID32 i = 0u; i < IG_TGL_ID_COUNT; ++i) {
        if (!igIsSupportedDevice(IG_PCI_VENDOR_INTEL, igTglId(i)))
            ++unmatched;
    }
    checkU32("every table entry matches under Intel's vendor", unmatched, 0u);

    //
    //  ...and none of them under any other vendor. 0x1002 is AMD, 0x10DE is
    //  NVIDIA -- the two other vendors whose parts turn up in this laptop's
    //  class of machine.
    //
    unsigned wrongVendorHits = 0u;
    for (IGID32 i = 0u; i < IG_TGL_ID_COUNT; ++i) {
        if (igIsSupportedDevice(0x10DEu, igTglId(i)))
            ++wrongVendorHits;
        if (igIsSupportedDevice(0x1002u, igTglId(i)))
            ++wrongVendorHits;
    }
    checkU32("no table entry matches under another vendor", wrongVendorHits, 0u);
}


int main(void)
{
    std::printf("IntelGpuId.hpp -- Gen12 device identification\n");
    std::printf("IDs from reference/i915/i915_pciids.h at Linux v6.12.\n");
    std::printf("One observed value, from igpu-results/tgl-capture-2026-08-16-022556.txt.\n");

    testGt1Table();
    testGt2Table();
    testTableShape();
    testObservedAgreesWithVendor();
    testMembership();
    testMatchPredicate();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
