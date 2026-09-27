//
//  IntelPixel.hpp
//  Gen12 plane format -> the fields IOFramebuffer's IOPixelInformation carries,
//  or a refusal.
//
//  Same rule as IntelDisplay.hpp, IntelFramebuffer.hpp, IntelScanout.hpp,
//  IntelGpuId.hpp and the five NVIDIA headers: DEPENDENCY-FREE of IOKit. No
//  kernel types, no allocation, no sleep. Every function is a total function of
//  its arguments, so tests/ compiles this IDENTICAL file under MSVC and drives it
//  on Windows.
//
//  It is the SECOND header in that family with an #include, after
//  IntelDisplayNames.hpp, and for the same reason: it switches on format codes
//  IntelFramebuffer.hpp owns, so it takes them from their owner rather than
//  respelling them. A local copy of IG_FB_FORMAT_XRGB_8888's value would give
//  this tree two answers to one question -- the defect
//  tests/test_igpu_mmio_ranges.py exists to prevent one layer down, and the exact
//  thing the WHAT THIS FILE DOES NOT DECIDE note below forbids. The include is a
//  QUOTED sibling inside phase0-kext/, so this header still parses standalone
//  under -I phase0-kext and the syntax gate's section 1 needs no special case.
//
//  WHERE THIS SITS IN THE TRACK
//  ----------------------------
//  IntelFramebuffer.hpp answers "what does the PLANE_CTL word mean", including
//  igFbFormat() and igFbBytesPerPixel(). IntelScanout.hpp answers "where are the
//  pixels and do they fit in the mapping". Neither answers the question that sits
//  between them and an IOFramebuffer subclass:
//
//      given a format code the firmware already programmed, can this driver
//      DESCRIBE those pixels to IOKit truthfully -- and with which component
//      widths and masks?
//
//  That is this file. It decodes nothing: the format code arrives ALREADY decoded
//  by igFbFormat(), because duplicating that shift here would give this tree two
//  answers to one question, which is the defect tests/test_igpu_mmio_ranges.py
//  exists to prevent one layer down.
//
//  WHY BYTES-PER-PIXEL IS NOT ENOUGH, AND THIS IS THE POINT OF THE FILE
//  -------------------------------------------------------------------
//  igFbBytesPerPixel() returns 4 for BOTH IG_FB_FORMAT_XRGB_8888 and
//  IG_FB_FORMAT_XRGB_2101010. Those two are not interchangeable to IOKit: one is
//  8 bits per component with masks 0x00FF0000/0x0000FF00/0x000000FF, the other is
//  10 bits per component with masks 0x3FF00000/0x000FFC00/0x000003FF. A component
//  width DERIVED from bytes-per-pixel is therefore right for one format and wrong
//  for the other, and the wrong direction is the quiet one -- describing a
//  2101010 surface as 8-8-8 programs cleanly and yields a picture whose colours
//  are wrong rather than a picture that is obviously broken. That is
//  handoff15 SS1's byte-swapped PLANE_SIZE hazard in a second place: the failure
//  mode of a plausible wrong answer is worse than the failure mode of a refusal.
//
//  WHAT THIS FILE REFUSES, AND WHY A REFUSAL IS THE HONEST ANSWER
//  -------------------------------------------------------------
//  Two of the four formats IntelFramebuffer.hpp names are DESCRIBABLE here and
//  two are not. RGB_565 is a real Gen12 format and IOKit's own 16-bit direct
//  pixel type is 5-5-5, so there is no standard macOS pixel description for it
//  that this driver could publish without inventing one; XRGB_16161616F is 64-bit
//  half-float and likewise has no pixel type this tree has ever witnessed a name
//  for. Refusing is not a limitation being papered over -- publishing a
//  fabricated component layout beside measured ones is exactly what CLAUDE.md
//  forbids, and an IOFramebuffer that describes its surface wrongly is worse than
//  one that declines to attach.
//

#ifndef INTEL_PIXEL_HPP
#define INTEL_PIXEL_HPP

#include "IntelFramebuffer.hpp"

//
//  Local width-explicit typedefs, for the same reason every sibling carries its
//  own: this header includes nothing, so it cannot borrow UInt32 from IOKit or
//  uint32_t from <cstdint>.
//
typedef unsigned int        IGP32;

//
//  THE VERDICT FAMILY
//  ------------------
//  A verdict rather than a bool, for 3050-handoff32 SS2's reason: a bool refusal is
//  satisfied by every refusal reason at once, and these two call for different
//  actions. UNKNOWN_FORMAT is a bug in this driver's decode path or a format code
//  Gen12 carries that IntelFramebuffer.hpp has never named. UNDESCRIBABLE is a
//  fact about IOKit's pixel vocabulary and about what this tree has witnessed --
//  the plane is programmed and legible, and this driver still will not claim to
//  describe it.
//
//  The LIMIT is INCLUSIVE, like every other verdict family in this track:
//  IG_PIXEL_VERDICT_LIMIT is 2 and IG_PIXEL_ERR_UNDESCRIBABLE is 2. That is the
//  measured convention (each family's bound is spelled "the largest verdict
//  above" and EQUALS its largest member) and tools/check-registers.py compares
//  limit against max(codes) as an EQUALITY per family, so an exclusive limit
//  could not pass the register gate at all. Bound an exhaustiveness loop `<=`
//  the constant below. An earlier revision of this paragraph's counterpart in
//  IntelScanout.hpp said some families were exclusive; that was false, and it is
//  handoff20 SS1's own defect in prose form -- arms 0..5 against an inclusive
//  limit of 6, so the last verdict published the unknown string with all four
//  gates green.
//
#define IG_PIXEL_OK                     0u  // the description below is usable
#define IG_PIXEL_ERR_UNKNOWN_FORMAT     1u  // not a format code this tree names
#define IG_PIXEL_ERR_UNDESCRIBABLE      2u  // legible, but no honest IOKit type
#define IG_PIXEL_VERDICT_LIMIT          2u  // the largest verdict above

//
//  Component layout, spelled per format rather than derived.
//
//  These are not hardware constants and carry no _OBSERVED_ name: they are what
//  IOKit's IOPixelInformation fields must hold to describe the corresponding
//  Gen12 surface, and they are pinned by the format's own definition rather than
//  by anything read off this silicon. The masks are XRGB -- three components,
//  alpha DELIBERATELY absent, because the X channel of an XRGB plane is not an
//  alpha channel and publishing a fourth component mask would tell IOKit the
//  surface carries transparency information it does not carry.
//
#define IG_PIXEL_BPP_XRGB_8888          32u
#define IG_PIXEL_COMPONENT_BITS_8888    8u
#define IG_PIXEL_MASK_R_8888            0x00FF0000u
#define IG_PIXEL_MASK_G_8888            0x0000FF00u
#define IG_PIXEL_MASK_B_8888            0x000000FFu

#define IG_PIXEL_BPP_XRGB_2101010       32u
#define IG_PIXEL_COMPONENT_BITS_2101010 10u
#define IG_PIXEL_MASK_R_2101010         0x3FF00000u
#define IG_PIXEL_MASK_G_2101010         0x000FFC00u
#define IG_PIXEL_MASK_B_2101010         0x000003FFu

//
//  Every describable format has exactly three components. Spelled once as a
//  constant rather than returned as a literal from two arms, because a per-arm
//  literal is the copy-paste shape 3050-handoff29 SS5's control exists to catch.
//
#define IG_PIXEL_COMPONENT_COUNT        3u

//
//  A refused read, spelled here rather than included, because this header
//  includes nothing. IG_MMIO_INVALID in IntelDisplay.hpp is 0xFFFFFFFF and every
//  refusal arm of IntelDisplayProbe::regRead32() returns it; the two are pinned
//  equal by the test suite instead, the same trick IntelFramebuffer.hpp and
//  IntelScanout.hpp each use for their own sentinel.
//
#define IG_PIXEL_INVALID32              0xFFFFFFFFu

//
//  Is this format code one this tree can describe to IOKit?
//
//  A whitelist by NAME, never a range test. A range would sweep in every future
//  Gen12 format code as describable and hand IOKit a component layout nobody
//  chose -- the fabricated-publication class this project has now met six times.
//
static inline int igPixelDescribable(IGP32 format)
{
    return format == IG_FB_FORMAT_XRGB_2101010
        || format == IG_FB_FORMAT_XRGB_8888;
}

//
//  THE JOIN
//  --------
//  One verdict over the whole description, not a flag per field, for
//  handoff24 SS1a's reason arrived at for the fourth time in this tree: these
//  values compose ONE reported thing. A caller that published a good bits-per-
//  pixel, a good component count and one sentinel would be describing a surface
//  that was never measured.
//
//  THE ORDER IS THE ASSERTION:
//
//  1. UNKNOWN_FORMAT first. Every later test is a property OF a format this file
//     recognises, and a code nothing names has no component width to be wrong
//     about. Testing describability first would report an unnamed code as merely
//     undescribable, which is a fact about IOKit rather than about this driver's
//     decode -- and only the second is something somebody can go and fix.
//
//  2. UNDESCRIBABLE after it, and only for codes this tree does name. RGB_565 and
//     XRGB_16161616F land here: legible, programmed, and refused on purpose.
//
static inline IGP32 igPixelDescribe(IGP32 format)
{
    if (format != IG_FB_FORMAT_XRGB_2101010
     && format != IG_FB_FORMAT_XRGB_8888
     && format != IG_FB_FORMAT_XRGB_16161616F
     && format != IG_FB_FORMAT_RGB_565)
        return IG_PIXEL_ERR_UNKNOWN_FORMAT;

    if (!igPixelDescribable(format))
        return IG_PIXEL_ERR_UNDESCRIBABLE;

    return IG_PIXEL_OK;
}

//
//  The three IOPixelInformation numbers, for a format igPixelDescribe() accepted.
//
//  Each returns IG_PIXEL_INVALID32 for any format it cannot describe, rather than
//  a plausible default. A zero would be worse than the sentinel and a 32 would be
//  worse still: 3050-handoff29 SS2's finding is that a literal written into a
//  published field is byte-identical to a value that was genuinely measured, so
//  the refusal has to be a value no real answer can take.
//
static inline IGP32 igPixelBitsPerPixel(IGP32 format)
{
    switch (format) {
    case IG_FB_FORMAT_XRGB_8888:     return IG_PIXEL_BPP_XRGB_8888;
    case IG_FB_FORMAT_XRGB_2101010:  return IG_PIXEL_BPP_XRGB_2101010;
    default:                         return IG_PIXEL_INVALID32;
    }
}

static inline IGP32 igPixelComponentBits(IGP32 format)
{
    switch (format) {
    case IG_FB_FORMAT_XRGB_8888:     return IG_PIXEL_COMPONENT_BITS_8888;
    case IG_FB_FORMAT_XRGB_2101010:  return IG_PIXEL_COMPONENT_BITS_2101010;
    default:                         return IG_PIXEL_INVALID32;
    }
}

static inline IGP32 igPixelComponentCount(IGP32 format)
{
    if (!igPixelDescribable(format))
        return IG_PIXEL_INVALID32;
    return IG_PIXEL_COMPONENT_COUNT;
}

//
//  The three channel masks. Returned through out-params rather than as a struct,
//  because a struct in a dependency-free header is a layout this tree would then
//  have to keep matching across two compilers for no gain.
//
//  A refused format writes the SENTINEL into all three, never leaving them
//  untouched. An untouched out-param carries whatever the caller had there, which
//  is the initialiser-zero shape 3050-handoff29 SS2 measured: a zero mask is
//  indistinguishable from a channel that is genuinely absent.
//
static inline IGP32 igPixelMasks(IGP32 format, IGP32 *r, IGP32 *g, IGP32 *b)
{
    if (r == 0 || g == 0 || b == 0)
        return IG_PIXEL_ERR_UNKNOWN_FORMAT;

    *r = IG_PIXEL_INVALID32;
    *g = IG_PIXEL_INVALID32;
    *b = IG_PIXEL_INVALID32;

    IGP32 verdict = igPixelDescribe(format);
    if (verdict != IG_PIXEL_OK)
        return verdict;

    if (format == IG_FB_FORMAT_XRGB_8888) {
        *r = IG_PIXEL_MASK_R_8888;
        *g = IG_PIXEL_MASK_G_8888;
        *b = IG_PIXEL_MASK_B_8888;
    } else {
        *r = IG_PIXEL_MASK_R_2101010;
        *g = IG_PIXEL_MASK_G_2101010;
        *b = IG_PIXEL_MASK_B_2101010;
    }

    return IG_PIXEL_OK;
}

//
//  A convenience predicate for the eventual IOFramebuffer subclass.
//
//  An EQUALITY against IG_PIXEL_OK, never a negation of one error arm.
//  3050-handoff33 SS2 is the cost of the alternative: `!= ERR_SOMETHING` is TRUE
//  for every OTHER refusal, so a description refused for a reason added later
//  would report as usable. A third verdict must make this return false without
//  anybody remembering to edit it.
//
static inline int igPixelUsable(IGP32 verdict)
{
    return verdict == IG_PIXEL_OK;
}

#endif  // INTEL_PIXEL_HPP
