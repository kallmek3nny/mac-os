//
//  IntelDisplayNames.hpp
//  Verdict names and IORegistry property-key composition for the Gen12 track.
//
//  WHY THIS FILE EXISTS
//  --------------------
//  These functions used to be file-static in IntelDisplayProbe.cpp. That put them
//  in the one Intel-track translation unit tests/ cannot compile -- it includes
//  <IOKit/IOService.h> -- so handoff19 was obliged to record that the four
//  verdict-name switches, igTransName() and the property-key composition had ZERO
//  runtime coverage and were reachable only by clang's front end. A switch arm
//  returning a neighbouring literal was invisible to all four gates.
//
//  It was not a hypothetical. igModeVerdictName() had arms for verdicts 0 through
//  5 while IG_MODE_VERDICT_LIMIT is 6, so IG_MODE_SRC_MISMATCH -- a verdict
//  igModeConsistent() genuinely returns, meaning PIPESRC disagrees with the
//  active region the timing registers describe -- published the string "unknown"
//  into the IORegistry. That is the single most interesting verdict in the family
//  (it is the byte-swapped PLANE_SIZE/PIPESRC confusion handoff15 §1 measured on
//  real silicon) and it was reported as "we have no idea". Moving the switch here
//  and asserting the family exhaustively is what found it.
//
//  DEPENDENCY-FREE, WITH ONE CLARIFICATION
//  ---------------------------------------
//  No IOKit, no kernel types, no allocation, no sleep, no angle-bracket include.
//  It does carry QUOTED includes of its two sibling headers, because the verdict
//  constants it switches on are defined there and this header has no forward
//  declarations available to it.
//
//  That is deliberate and it is what tools/check-header-coverage.py already
//  expects: the exemption from section 1 of tests/check-kext-syntax.bat is
//  computed from the presence of an ANGLE-BRACKET include, i.e. a dependency
//  outside phase0-kext/. A quoted sibling include keeps this file parseable
//  standalone under -I phase0-kext, so it belongs IN section 1 rather than being
//  exempted from it.
//
//  WHY SWITCHES AND NOT STRING TABLES
//  ----------------------------------
//  A table indexed by verdict is correct only while the verdict set stays
//  contiguous from zero -- which tools/check-registers.py does assert -- but a
//  table silently reads PAST ITS END when a family grows, and the value it
//  returns is another verdict's name. A switch cannot do that: the worst it does
//  is fall to `default` and say "unknown".
//
//  "unknown" is honest but it is not free, which is the lesson of the
//  SRC_MISMATCH arm above. The default arms exist because these take an unsigned
//  int rather than an enum, so the compiler cannot prove the switch exhaustive
//  and -Werror cannot help. What covers that gap is
//  tests/test_intel_display_names.cpp, which walks every verdict from 0 to each
//  family's own _VERDICT_LIMIT and REFUSES the string "unknown" inside that
//  range -- so the next verdict added to a family reddens a host suite on the
//  commit that adds it, rather than on an operator's laptop.
//

#ifndef INTEL_DISPLAY_NAMES_HPP
#define INTEL_DISPLAY_NAMES_HPP

#include "IntelDisplay.hpp"
#include "IntelFramebuffer.hpp"
#include "IntelScanout.hpp"
#include "IntelFbPresent.hpp"

//
// Same convention as IGU32 in IntelDisplay.hpp and IGFB32 in
// IntelFramebuffer.hpp: a local typedef rather than UInt32, because UInt32 is an
// IOKit type and naming it here would drag in the dependency this whole split
// exists to avoid. The kext passes UInt32 and the host passes unsigned int; both
// are 32-bit unsigned on every target this project builds for.
//
typedef unsigned int IGN32;

//
// The link verdict out of igTransconfDdiLinked() -- is this transcoder timing,
// connected, and pointed at a port?
//
static inline const char *igLinkVerdictName(IGN32 verdict)
{
    switch (verdict) {
    case IG_LINK_OK:         return "ok";
    case IG_LINK_NO_TIMING:  return "no-timing";
    case IG_LINK_NO_OUTPUT:  return "no-output";
    case IG_LINK_NO_PORT:    return "no-port";
    default:                 return "unknown";
    }
}

//
// The whole-mode consistency verdict out of igModeConsistent().
//
// IG_MODE_SRC_MISMATCH is the arm that was missing. Read the file header before
// deleting or reordering anything here.
//
static inline const char *igModeVerdictName(IGN32 verdict)
{
    switch (verdict) {
    case IG_MODE_OK:            return "ok";
    case IG_MODE_IMPLAUSIBLE:   return "implausible";
    case IG_MODE_BLANK_RANGE:   return "blank-range";
    case IG_MODE_BLANK_OVERLAP: return "blank-overlap";
    case IG_MODE_SYNC_RANGE:    return "sync-range";
    case IG_MODE_SYNC_OUTSIDE:  return "sync-outside";
    case IG_MODE_SRC_MISMATCH:  return "src-mismatch";
    default:                    return "unknown";
    }
}

//
// The whole-pipe join out of igPipeUsable(): could this pipe be inherited at all?
//
static inline const char *igUsableVerdictName(IGN32 verdict)
{
    switch (verdict) {
    case IG_USABLE_OK:      return "ok";
    case IG_USABLE_LINK:    return "link";
    case IG_USABLE_TIMING:  return "timing";
    case IG_USABLE_PLANE:   return "plane";
    default:                return "unknown";
    }
}

//
// The scanout-descriptor verdict out of igScanDescribe().
//
// THE LIMIT FOR THIS FAMILY IS INCLUSIVE. IG_SCAN_VERDICT_LIMIT is 6 and
// IG_SCAN_ERR_OUTSIDE_APERTURE is 6 -- and so is every other verdict family in
// this track, measured 2026-08-19 rather than remembered: each bound is spelled
// "the largest verdict above" and EQUALS its largest member, and
// tools/check-registers.py enforces limit == max(codes) per family, so an
// exclusive limit could not pass the register gate. An earlier revision of this
// paragraph called IG_MODE_ and IG_FB_INHERIT_ exclusive; that was false, and it
// is the defect it was warning about, in prose. handoff20 SS1 is
// igModeVerdictName() carrying arms 0..5 against an INCLUSIVE limit of 6, so
// IG_MODE_SRC_MISMATCH published "unknown" with all four gates green. Getting
// the convention backwards in the OTHER direction is just as available -- an
// exhaustiveness loop written `< IG_SCAN_VERDICT_LIMIT` never reaches
// OUTSIDE_APERTURE, which is the one verdict in this family that is a fact
// about how much memory got mapped rather than a bug in this driver.
//
static inline const char *igScanVerdictName(IGN32 verdict)
{
    switch (verdict) {
    case IG_SCAN_OK:                   return "ok";
    case IG_SCAN_ERR_NO_SURFACE:       return "no-surface";
    case IG_SCAN_ERR_UNALIGNED:        return "unaligned";
    case IG_SCAN_ERR_ZERO_GEOMETRY:    return "zero-geometry";
    case IG_SCAN_ERR_ROW_TOO_SHORT:    return "row-too-short";
    case IG_SCAN_ERR_EXTENT_OVERFLOW:  return "extent-overflow";
    case IG_SCAN_ERR_OUTSIDE_APERTURE: return "outside-aperture";
    default:                           break;
    }
    return "unknown";
}

//
// The plane verdict out of igFbInheritable().
//
static inline const char *igInheritVerdictName(IGN32 verdict)
{
    switch (verdict) {
    case IG_FB_INHERIT_OK:         return "ok";
    case IG_FB_INHERIT_DISABLED:   return "disabled";
    case IG_FB_INHERIT_COMPRESSED: return "compressed";
    case IG_FB_INHERIT_FORMAT:     return "format";
    case IG_FB_INHERIT_STRIDE:     return "stride";
    case IG_FB_INHERIT_GEOMETRY:   return "geometry";
    default:                       return "unknown";
    }
}

//
// The group-read verdict out of igSampleVerdict().
//
// Two arms, and it is still a named family rather than a bool for the reason
// igReadDecision() is: the string is what lands in the IORegistry, and "refused"
// there is a different claim from an absent property or a zero. A bool published
// as 0/1 would need a reader who knows which way round it goes.
//
// "ok" matches the other four families on purpose -- an operator scanning the
// IORegistry reads one word for every verdict this driver publishes.
//
static inline const char *igSampleVerdictName(IGN32 verdict)
{
    switch (verdict) {
    case IG_SAMPLE_OK:      return "ok";
    case IG_SAMPLE_REFUSED: return "refused";
    default:                return "unknown";
    }
}

//
// The presentation verdict out of igPresentVerdict() in IntelFbPresent.hpp.
//
// Five arms and a bound of IG_PRESENT_VERDICT_LIMIT, which that header spells as
// an EQUALITY against its largest member -- the same convention every other
// family here follows. This one carries a risk the others do not:
// tools/check-registers.py parses only IntelDisplay.hpp and IntelFramebuffer.hpp,
// so IG_PRESENT_* gets no set-check from the register gate at all, and
// tests/test_intel_display_names.cpp's own exhaustiveness loop over
// IG_PRESENT_VERDICT_LIMIT is the only thing that reddens when this switch loses
// an arm. That is precisely how igModeVerdictName() shipped with a missing arm
// publishing "unknown" into the IORegistry, green through all four gates.
//
// The names say what was refused rather than what to do about it, matching the
// other five families: "sample" is this driver failing to read the group,
// "inheritable" is the plane refusing adoption, "scanout" is the surface not
// fitting the aperture, and "pixel" is a format with no describable layout.
//
static inline const char *igPresentVerdictName(IGN32 verdict)
{
    switch (verdict) {
    case IG_PRESENT_OK:                  return "ok";
    case IG_PRESENT_ERR_SAMPLE_REFUSED:  return "sample";
    case IG_PRESENT_ERR_NOT_INHERITABLE: return "inheritable";
    case IG_PRESENT_ERR_SCANOUT:         return "scanout";
    case IG_PRESENT_ERR_PIXEL:           return "pixel";
    default:                             return "unknown";
    }
}

//
// A transcoder's name, for log lines and property keys.
//
// Property keys carry the LETTER rather than the index, because "IGTranscoder2"
// is ambiguous between "the third transcoder" and "transcoder C" depending on
// where the reader starts counting.
//
// DSI0 and DSI1 are named here even though IG_TRANS_SCAN_COUNT stops at four and
// the probe never reaches them. A name function that returned "?" for a
// transcoder that exists would make a future scan-count change misreport rather
// than fail, and the two DSI pages do exist on this die -- they are excluded from
// the scan because they are off the 0x1000 plane ladder, not because they are
// absent.
//
static inline const char *igTransName(IGN32 trans)
{
    switch (trans) {
    case IG_TRANSCODER_A:    return "A";
    case IG_TRANSCODER_B:    return "B";
    case IG_TRANSCODER_C:    return "C";
    case IG_TRANSCODER_D:    return "D";
    case IG_TRANSCODER_DSI0: return "DSI0";
    case IG_TRANSCODER_DSI1: return "DSI1";
    default:                 return "?";
    }
}

//
// The longest key this composer is ever asked for, plus its NUL. "IGTranscoder"
// is 12 characters, the longest unit name is "DSI0" at 4, and the longest suffix
// is "-Inherit" at 8: 24 with the terminator. Callers size their buffers from
// this rather than from a number they counted by hand.
//
#define IG_KEY_BUFFER_BYTES         32u

//
// Compose an IORegistry property key: prefix + unit + suffix.
//
// This replaced snprintf, and the reason is not that snprintf was unavailable --
// it is a kernel export, and tests/kextshim/libkern/libkern.h exists precisely
// because IOKit/IOLib.h does not declare it. The reason is that snprintf
// TRUNCATES on overflow and reports the truncation only in a return value, so a
// caller that sized a buffer slightly short publishes "IGTranscoderDSI0-Inher"
// -- a key that looks deliberate, sorts next to the real one, and is wrong. It
// also cannot be tested from tests/, because a kernel-side snprintf is not the
// host's.
//
// This refuses instead, and on refusal it writes an EMPTY string rather than a
// partial one, so a caller that ignores the return value publishes a visibly
// broken key instead of a plausible one. Both halves of that are asserted in
// tests/test_intel_display_names.cpp.
//
// It is a total function of its arguments: no allocation, no locale, no format
// string, and nothing to get wrong about a %s paired with a %u.
//
static inline bool igComposeKey(char *out, IGN32 outSize,
                               const char *prefix,
                               const char *unit,
                               const char *suffix)
{
    if (out == 0 || outSize == 0)
        return false;

    out[0] = '\0';

    if (prefix == 0 || unit == 0 || suffix == 0)
        return false;

    const char *parts[3];
    parts[0] = prefix;
    parts[1] = unit;
    parts[2] = suffix;

    IGN32 used = 0;

    for (IGN32 p = 0; p < 3u; p++) {
        const char *s = parts[p];

        for (IGN32 i = 0; s[i] != '\0'; i++) {
            //
            // used + 1 rather than used, because the NUL has to fit as well.
            // Getting this edge wrong in the lenient direction is a one-byte
            // overrun of a stack buffer in the kernel, so the test suite pins
            // the exact-fit case and the one-byte-short case either side of it.
            //
            if (used + 1u >= outSize) {
                out[0] = '\0';
                return false;
            }

            out[used++] = s[i];
        }
    }

    out[used] = '\0';
    return true;
}

//
// The two keys the probe actually composes. These exist so the PREFIX is written
// once: a probe that published "IGTranscoderA-Link" in one place and
// "IGTransA-Link" in another would produce an IORegistry an operator cannot
// grep, and neither spelling would be wrong on its own.
//
static inline bool igTranscoderKey(char *out, IGN32 outSize,
                                   IGN32 trans, const char *suffix)
{
    return igComposeKey(out, outSize, "IGTranscoder", igTransName(trans), suffix);
}

static inline bool igPlaneKey(char *out, IGN32 outSize,
                              IGN32 pipe, const char *suffix)
{
    return igComposeKey(out, outSize, "IGPlane", igTransName(pipe), suffix);
}

#endif // INTEL_DISPLAY_NAMES_HPP
