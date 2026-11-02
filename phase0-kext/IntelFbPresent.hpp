//
//  IntelFbPresent.hpp
//  The last purely-arithmetic step on the Gen12 display track: given the four
//  plane words the firmware already programmed and the size of the aperture this
//  driver actually mapped, decide whether an IOFramebuffer subclass may DESCRIBE
//  that surface to macOS -- and if so, with exactly which numbers.
//
//  Same rule as IntelDisplay.hpp, IntelFramebuffer.hpp, IntelScanout.hpp,
//  IntelPixel.hpp, IntelGpuId.hpp and the five NVIDIA headers: DEPENDENCY-FREE of
//  IOKit. No kernel types, no allocation, no sleep. Every function is a total
//  function of its arguments, so tests/test_intel_fb_present.cpp compiles this
//  IDENTICAL file under MSVC and drives it on Windows.
//
//  It is the THIRD header in that family with an #include, after
//  IntelDisplayNames.hpp and IntelPixel.hpp, and for the same reason each of those
//  has one: it is a JOIN over constants its siblings own, so it takes them from
//  their owners rather than respelling them. Every include is a QUOTED sibling
//  inside phase0-kext/, so this header still parses standalone under
//  -I phase0-kext and the syntax gate's section 1 needs no special case --
//  check-header-coverage.py derives its one exemption from the presence of an
//  ANGLE-BRACKET include and so expects this file THERE rather than exempted.
//
//  WHY THIS FILE EXISTS RATHER THAN LIVING IN THE SUBCLASS
//  ------------------------------------------------------
//  An IOFramebuffer subclass cannot be compiled off macOS, so a decision written
//  inside one is a decision nothing runs. That is the whole argument for this
//  tree's split, and IntelDisplayNames.hpp is what it looks like after the
//  argument has already cost a live defect: four verdict-name switches sat in the
//  Intel shell for a month, no host suite could compile them, and
//  igModeVerdictName() was missing its last arm the entire time with all four
//  gates green.
//
//  So the subclass below this header does no arithmetic at all. It reads four
//  registers, calls igPresentVerdict(), and either fills IOKit's structures from
//  the accessors here or declines to attach.
//
//  WHAT THIS FILE DOES NOT DECIDE
//  ------------------------------
//  It does not decode PLANE_CTL (IntelFramebuffer.hpp), does not compute the
//  scanout extent or test aperture containment (IntelScanout.hpp), and does not
//  choose component widths or channel masks (IntelPixel.hpp). Duplicating any of
//  those would give this tree two answers to one question, which is the defect
//  tests/test_igpu_mmio_ranges.py exists to prevent one layer down. Everything
//  here CALLS its owner and joins the results.
//
//  It also does not settle which aperture a PLANE_SURF offset is relative to.
//  IntelScanout.hpp's own note is the governing one: PLANE_SURF holds a
//  graphics-address-space offset, mapping that space is a BAR this track has never
//  mapped, and so the aperture size is a PARAMETER supplied by whoever eventually
//  maps it. A caller with no such mapping passes zero and is refused. Writing a
//  BAR index in here would be inventing evidence.
//

#ifndef INTEL_FB_PRESENT_HPP
#define INTEL_FB_PRESENT_HPP

//
//  IntelDisplay.hpp is included for igReadRefused() and the IG_SAMPLE_ family --
//  the refused-read polarity decision -- rather than for a register offset. This
//  header emits no addresses.
//
#include "IntelDisplay.hpp"
#include "IntelFramebuffer.hpp"
#include "IntelScanout.hpp"
#include "IntelPixel.hpp"

//
//  Local width-explicit typedefs, for the same reason every sibling carries its
//  own: this family cannot borrow UInt32 from IOKit or uint32_t from <cstdint>.
//  Spelled fresh rather than reused from whichever sibling happened to be
//  included first, so a change to one sibling's spelling cannot silently retype
//  this header's whole surface.
//
typedef unsigned int        IGPR32;
typedef unsigned long long  IGPR64;

//
//  THE VERDICT FAMILY
//  ------------------
//  A verdict rather than a bool, for 3050-handoff32 SS2's reason arrived at for
//  the fifth time in this tree: a bool refusal is satisfied by every refusal
//  reason at once, and these four call for four different actions.
//
//  SAMPLE_REFUSED is a bug in this driver's read path -- a BAR that was never
//  mapped, or an offset outside it. NOT_INHERITABLE is a fact about the plane the
//  firmware programmed (disabled, tiled, compressed, or a layout this track has
//  never named). SCANOUT is a bug in this driver's decode or a mapping too small
//  to hold the pixels. PIXEL is a fact about IOKit's pixel vocabulary and about
//  what this tree has witnessed -- the surface is legible and this driver still
//  will not claim to describe it.
//
//  The LIMIT is INCLUSIVE, like every other verdict family in this track:
//  IG_PRESENT_VERDICT_LIMIT is 4 and IG_PRESENT_ERR_PIXEL is 4. That is the
//  measured convention -- each family spells its bound "the largest verdict
//  above" and EQUALS its largest member, and tools/check-registers.py compares
//  limit against max(codes) as an EQUALITY per family for the two Intel headers
//  it parses. This header is NOT one of those two, so this family gets no
//  set-check from that gate at all and the exhaustiveness assertions in
//  tests/test_intel_fb_present.cpp are the only thing holding the relation.
//  Bound an exhaustiveness loop `<=` the constant below.
//
#define IG_PRESENT_OK                   0u  // the numbers below are usable
#define IG_PRESENT_ERR_SAMPLE_REFUSED   1u  // one of the four words was never read
#define IG_PRESENT_ERR_NOT_INHERITABLE  2u  // the plane is not a layout we inherit
#define IG_PRESENT_ERR_SCANOUT          3u  // base/stride/extent do not describe pixels
#define IG_PRESENT_ERR_PIXEL            4u  // legible, but no honest IOKit pixel type
#define IG_PRESENT_VERDICT_LIMIT        4u  // the largest verdict above

//
//  Refused answers.
//
//  Spelled here rather than borrowed from a sibling even though all three
//  siblings' sentinels hold the same bit pattern, because they are three
//  DIFFERENT claims that happen to coincide -- IG_MMIO_INVALID is "the read was
//  refused", IG_SCAN_INVALID32 is "no scanout answer", IG_PIXEL_INVALID32 is "no
//  pixel answer" -- and the test suite pins all of them equal rather than one
//  header importing another's meaning. Same trick IntelPixel.hpp and
//  IntelScanout.hpp each use for their own.
//
#define IG_PRESENT_INVALID32            0xFFFFFFFFu
#define IG_PRESENT_INVALID64            0xFFFFFFFFFFFFFFFFull

//
//  How many words the plane group carries, and it is a COUNT rather than a
//  literal 4 written at three call sites. igSampleVerdict() judges a read GROUP
//  rather than a field for handoff24 SS1a's reason, and a group whose size is
//  respelled per call site is a group one edit away from being judged over three
//  of its four words.
//
#define IG_PRESENT_PLANE_WORD_COUNT     4u

//
//  Was any word in the plane group a refused read?
//
//  igSampleVerdict() lives in IntelDisplay.hpp and takes an array, which is the
//  right shape for the shell's own sampled[] blocks and the wrong shape for a
//  header that wants to be callable with four named values. This wrapper exists
//  so the four words are named at every call site in this file; it is a
//  DISJUNCTION over igReadRefused() rather than a second copy of the sentinel
//  comparison, so the polarity decision (igReadRefused(0u) is deliberately FALSE,
//  on the evidence of the 2026-08-16 capture reading 0x00000000 out of every
//  unprogrammed page) stays owned by one function.
//
static inline int igPresentSampleRefused(IGPR32 ctl,
                                        IGPR32 stride,
                                        IGPR32 size,
                                        IGPR32 surf)
{
    return igReadRefused(ctl)
        || igReadRefused(stride)
        || igReadRefused(size)
        || igReadRefused(surf);
}

//
//  Is the plane plain linear pixels? (FB-5, t-9348aa)
//
//  igFbInheritable() refuses a disabled plane, render compression (bit 15), a
//  format this tree does not name, a bad stride and a 1x1 page. It never reads
//  the four PLANE_CTL fields below, and each of them puts pixel (x, y)
//  somewhere a linear description does not point:
//
//      field                                 bits   non-zero means
//      PLANE_CTL_TILED_MASK                  12:10  X, Y or Yf/Tile4 tiled
//      PLANE_CTL_FLIP_HORIZONTAL             8      rows scan right to left
//      PLANE_CTL_MEDIA_DECOMPRESSION_ENABLE  4      media-compressed (TGL+)
//      PLANE_CTL_ROTATE_MASK                 1:0    rotated 90, 180 or 270
//
//  Source: reference/i915/skl_universal_plane_regs.h:83-100. Bit 4 means media
//  decompression only on TGL and later; before GLK, bits 5:4 were the alpha
//  mode. This file presents Gen12 planes, so bit 4 has its TGL meaning here.
//
//  The answer has to come from the live word. The 2026-08-16 Linux capture,
//  0x82009000, is Tile Y (bits 12:10 = 4) with render compression on, and the
//  plane macOS inherits need not match it. Before this check, a Tile-Y plane
//  with bit 15 clear passed every test in this file, and GT-1's linear BCS0
//  fill would then have written linear rows into a tiled surface.
//
//  Tiling is read through igFbTiling() and IG_FB_TILING_LINEAR, which
//  IntelFramebuffer.hpp already owns. The other three fields have no decoder
//  there yet, and that file is calm-seal-8e's uncommitted t-a7929c work, so
//  their masks are spelled here for now. That is an exception to "WHAT THIS
//  FILE DOES NOT DECIDE" above. They belong in IntelFramebuffer.hpp beside
//  IG_FB_CTL_RENDER_DECOMP, and should move there once that file is free.
//
//  A plane refused here gets IG_PRESENT_ERR_NOT_INHERITABLE, the same verdict
//  igFbInheritable() produces. Step 2 of THE JOIN gives "not a linear layout"
//  to that verdict, so there is no new verdict number, name arm or limit.
//
#define IG_PRESENT_CTL_FLIP_HORIZONTAL  0x00000100u  // REG_BIT(8), :90
#define IG_PRESENT_CTL_MEDIA_DECOMP     0x00000010u  // REG_BIT(4) TGL+, :91
#define IG_PRESENT_CTL_ROTATE_MASK      0x00000003u  // REG_GENMASK(1, 0), :96

static inline int igPresentLinearLayout(IGPR32 ctl)
{
    if (igFbTiling(ctl) != IG_FB_TILING_LINEAR)
        return 0;
    if ((ctl & IG_PRESENT_CTL_FLIP_HORIZONTAL) != 0u)
        return 0;
    if ((ctl & IG_PRESENT_CTL_MEDIA_DECOMP) != 0u)
        return 0;
    if ((ctl & IG_PRESENT_CTL_ROTATE_MASK) != 0u)
        return 0;
    return 1;
}

//
//  THE JOIN
//  --------
//  One verdict over the whole description, not a flag per field, for
//  handoff24 SS1a's reason arrived at for the fifth time: these values compose ONE
//  reported thing. An IOFramebuffer that published a good width, a good stride,
//  a good bits-per-pixel and one sentinel would be describing a surface that was
//  never measured -- and unlike an IORegistry property, a wrong number here is
//  handed to WindowServer and used to address memory.
//
//  THE ORDER IS THE ASSERTION, and every step is load-bearing:
//
//  1. SAMPLE_REFUSED first. Every later test is arithmetic over these four words,
//     and arithmetic over a refused read is arithmetic over 0xFFFFFFFF. That word
//     has IG_FB_CTL_ENABLE set, so an inheritable-first order reports a plane
//     nobody read as an ENABLED plane and then decodes a format out of it.
//
//  2. NOT_INHERITABLE before the scanout. igFbInheritable() and
//     igPresentLinearLayout() together decide the plane is a layout this
//     driver understands at all: enabled, uncompressed (render or media),
//     linear, unflipped, unrotated, and a format this tree names. Until FB-5
//     only igFbInheritable() ran here, and it never reads the tiling field, so
//     this step promised "linear" without checking it. Every scanout number is
//     a property OF such a plane; a tiled surface has a stride that means
//     something else entirely, so describing its extent is arithmetic over a
//     misread word.
//
//  3. SCANOUT before PIXEL, and this is the one ordering choice worth arguing
//     about, because both orders "work". A scanout refusal is a bug in THIS
//     driver (a decode that went wrong) or a fact about a mapping this driver
//     made too small -- somebody can go and fix either. A pixel refusal is a
//     permanent fact about IOKit's vocabulary: RGB_565 and XRGB_16161616F have no
//     honest macOS pixel description and never will. Reporting the permanent fact
//     over the fixable one buries the actionable half, which is the same reasoning
//     handoff22 SS1b used to put the out-of-policy guard BEFORE the past-mapping
//     one.
//
//  A plane that is bad in two ways at once pins the order, and the suite drives
//  each such pair rather than trusting the prose above.
//
static inline IGPR32 igPresentVerdict(IGPR32 ctl,
                                     IGPR32 stride,
                                     IGPR32 size,
                                     IGPR32 surf,
                                     IGPR64 apertureBytes)
{
    IGPR32 format;

    if (igPresentSampleRefused(ctl, stride, size, surf))
        return IG_PRESENT_ERR_SAMPLE_REFUSED;

    if (igFbInheritable(ctl, stride, size) != IG_FB_INHERIT_OK)
        return IG_PRESENT_ERR_NOT_INHERITABLE;

    if (!igPresentLinearLayout(ctl))
        return IG_PRESENT_ERR_NOT_INHERITABLE;

    format = igFbFormat(ctl);

    if (igScanDescribe(surf,
                       igFbStrideBytes(stride),
                       igFbSizeWidth(size),
                       igFbSizeHeight(size),
                       igFbBytesPerPixel(format),
                       apertureBytes) != IG_SCAN_OK)
        return IG_PRESENT_ERR_SCANOUT;

    if (igPixelDescribe(format) != IG_PIXEL_OK)
        return IG_PRESENT_ERR_PIXEL;

    return IG_PRESENT_OK;
}

//
//  A convenience predicate for the subclass.
//
//  An EQUALITY against IG_PRESENT_OK, never a negation of one error arm.
//  3050-handoff33 SS2 is the cost of the alternative: `!= ERR_SOMETHING` is TRUE
//  for every OTHER refusal, so a description refused for a reason added later
//  would report as usable. A fifth verdict must make this return false without
//  anybody remembering to edit it.
//
static inline int igPresentUsable(IGPR32 verdict)
{
    return verdict == IG_PRESENT_OK;
}

//
//  THE ANSWERS
//  -----------
//  Each accessor takes the WHOLE group and re-derives the verdict before
//  answering, returning the sentinel for any group igPresentVerdict() refuses.
//
//  That is deliberate duplication of work in a function called a handful of times
//  per attach, and it buys the one property this family exists for: a caller who
//  forgets to check the verdict CANNOT publish a fabricated number. 3050-handoff29
//  SS2's finding is that a literal written into a published field is
//  byte-identical to a value that was genuinely measured, and every occurrence of
//  that class in this tree so far has been a caller that had the refusal
//  available and did not consult it -- five times, in two tracks, in code whose
//  own comments explained why it mattered. An accessor that answers only for an
//  accepted group removes the possibility rather than documenting it.
//
//  A zero would be worse than the sentinel and a plausible default worse still:
//  a width of 0 reads as a real answer about an empty surface, and a
//  bits-per-pixel of 32 is exactly what the common case measures.
//

static inline IGPR32 igPresentWidth(IGPR32 ctl,
                                   IGPR32 stride,
                                   IGPR32 size,
                                   IGPR32 surf,
                                   IGPR64 apertureBytes)
{
    if (!igPresentUsable(igPresentVerdict(ctl, stride, size, surf, apertureBytes)))
        return IG_PRESENT_INVALID32;
    return igFbSizeWidth(size);
}

static inline IGPR32 igPresentHeight(IGPR32 ctl,
                                    IGPR32 stride,
                                    IGPR32 size,
                                    IGPR32 surf,
                                    IGPR64 apertureBytes)
{
    if (!igPresentUsable(igPresentVerdict(ctl, stride, size, surf, apertureBytes)))
        return IG_PRESENT_INVALID32;
    return igFbSizeHeight(size);
}

//
//  Bytes per row, which IOKit calls bytesPerRow and this silicon calls stride.
//  Taken from igFbStrideBytes() rather than computed from width * bpp: the
//  hardware's stride is what the display engine actually walks, and a computed
//  minimum is a LOWER BOUND on it. igScanDescribe() has already refused any group
//  where the real stride is below that bound.
//
static inline IGPR32 igPresentBytesPerRow(IGPR32 ctl,
                                         IGPR32 stride,
                                         IGPR32 size,
                                         IGPR32 surf,
                                         IGPR64 apertureBytes)
{
    if (!igPresentUsable(igPresentVerdict(ctl, stride, size, surf, apertureBytes)))
        return IG_PRESENT_INVALID32;
    return igFbStrideBytes(stride);
}

static inline IGPR32 igPresentBitsPerPixel(IGPR32 ctl,
                                          IGPR32 stride,
                                          IGPR32 size,
                                          IGPR32 surf,
                                          IGPR64 apertureBytes)
{
    if (!igPresentUsable(igPresentVerdict(ctl, stride, size, surf, apertureBytes)))
        return IG_PRESENT_INVALID32;
    return igPixelBitsPerPixel(igFbFormat(ctl));
}

static inline IGPR32 igPresentComponentBits(IGPR32 ctl,
                                           IGPR32 stride,
                                           IGPR32 size,
                                           IGPR32 surf,
                                           IGPR64 apertureBytes)
{
    if (!igPresentUsable(igPresentVerdict(ctl, stride, size, surf, apertureBytes)))
        return IG_PRESENT_INVALID32;
    return igPixelComponentBits(igFbFormat(ctl));
}

static inline IGPR32 igPresentComponentCount(IGPR32 ctl,
                                            IGPR32 stride,
                                            IGPR32 size,
                                            IGPR32 surf,
                                            IGPR64 apertureBytes)
{
    if (!igPresentUsable(igPresentVerdict(ctl, stride, size, surf, apertureBytes)))
        return IG_PRESENT_INVALID32;
    return igPixelComponentCount(igFbFormat(ctl));
}

//
//  The 4 KiB-aligned graphics-address-space offset of the first pixel.
//
//  This is NOT a CPU address and NOT a BAR offset. It is what PLANE_SURF holds,
//  and turning it into something dereferenceable needs a mapping of the graphics
//  address space that this track has never made. The subclass therefore reports
//  it and does not follow it.
//
static inline IGPR32 igPresentBaseOffset(IGPR32 ctl,
                                        IGPR32 stride,
                                        IGPR32 size,
                                        IGPR32 surf,
                                        IGPR64 apertureBytes)
{
    if (!igPresentUsable(igPresentVerdict(ctl, stride, size, surf, apertureBytes)))
        return IG_PRESENT_INVALID32;
    return igScanSurfaceAddress(surf);
}

//
//  The byte extent of the visible pixels, in 64 bits and never narrowed here.
//
//  IntelScanout.hpp's argument is the governing one: bytesPerRow * height wraps
//  in 32 bits rather than saturating, and truncation is ASYMMETRIC -- losing the
//  high bits makes an extent look SAFER, and the direction that looks safer is
//  the direction that scribbles. igScanDescribe() has already refused any group
//  whose extent exceeds IG_SCAN_EXTENT_POLICY_CEILING or runs off the end of the
//  aperture, so a value returned here fits a 32-bit IOKit field -- but it is
//  returned wide anyway, because the narrowing belongs at the publication site
//  where somebody can see it happen.
//
static inline IGPR64 igPresentExtentBytes(IGPR32 ctl,
                                         IGPR32 stride,
                                         IGPR32 size,
                                         IGPR32 surf,
                                         IGPR64 apertureBytes)
{
    if (!igPresentUsable(igPresentVerdict(ctl, stride, size, surf, apertureBytes)))
        return IG_PRESENT_INVALID64;
    return igScanExtentBytes(igFbStrideBytes(stride), igFbSizeHeight(size));
}

//
//  The three channel masks, through out-params rather than a struct, for
//  IntelPixel.hpp's reason: a struct in a dependency-free header is a layout this
//  tree would then have to keep matching across two compilers for no gain.
//
//  A refused group writes the SENTINEL into all three, never leaving them
//  untouched. An untouched out-param carries whatever the caller had there, which
//  is the initialiser-zero shape 3050-handoff29 SS2 measured -- and a zero mask is
//  indistinguishable from a channel that is genuinely absent.
//
static inline IGPR32 igPresentMasks(IGPR32 ctl,
                                    IGPR32 stride,
                                    IGPR32 size,
                                    IGPR32 surf,
                                    IGPR64 apertureBytes,
                                    IGPR32 *r,
                                    IGPR32 *g,
                                    IGPR32 *b)
{
    IGPR32 verdict;

    //
    //  A null out-param answers SAMPLE_REFUSED rather than OK, and that is the
    //  same refuse-rather-than-trust direction igReadDecision() takes for a null
    //  base and igScanWithinAperture() takes for a zero aperture. It is NOT the
    //  most descriptive verdict available -- a null pointer is a caller bug
    //  rather than a refused read -- and it is chosen anyway because inventing a
    //  sixth verdict for a condition no hardware can produce would put a code in
    //  the family that the exhaustiveness loops cannot reach from any register
    //  word.
    //
    if (r == 0 || g == 0 || b == 0)
        return IG_PRESENT_ERR_SAMPLE_REFUSED;

    *r = IG_PRESENT_INVALID32;
    *g = IG_PRESENT_INVALID32;
    *b = IG_PRESENT_INVALID32;

    verdict = igPresentVerdict(ctl, stride, size, surf, apertureBytes);
    if (!igPresentUsable(verdict))
        return verdict;

    //
    //  Delegated rather than switched on again here. igPixelMasks() owns the
    //  per-format layout and writes its own sentinel into all three on refusal,
    //  so this is a forward of both the values and the polarity.
    //
    if (igPixelMasks(igFbFormat(ctl), r, g, b) != IG_PIXEL_OK) {
        *r = IG_PRESENT_INVALID32;
        *g = IG_PRESENT_INVALID32;
        *b = IG_PRESENT_INVALID32;
        return IG_PRESENT_ERR_PIXEL;
    }

    return IG_PRESENT_OK;
}

#endif  // INTEL_FB_PRESENT_HPP
