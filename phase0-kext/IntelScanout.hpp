//
//  IntelScanout.hpp
//  Gen12 (Tiger Lake) scanout descriptor -- turning an inherited plane
//  configuration into the three numbers IOFramebuffer needs, or refusing.
//
//  Same rule as IntelDisplay.hpp, IntelFramebuffer.hpp, IntelGpuId.hpp and the
//  five NVIDIA headers: DEPENDENCY-FREE. No includes, no IOKit, no kernel
//  types, no allocation, no sleep. Every function is a total function of its
//  arguments, so tests/test_intel_scanout.cpp compiles this IDENTICAL file
//  under MSVC and drives it on Windows. CLAUDE.md's argument for the split is
//  the whole reason this file exists rather than a block of arithmetic inside
//  the IOKit shell: a pure function written in a shell is a pure function
//  nothing runs, and IntelDisplayNames.hpp is what that looks like after it has
//  already cost a live defect.
//
//  WHERE THIS SITS IN THE TRACK
//  ----------------------------
//  IntelDisplay.hpp answers "where is register X". IntelFramebuffer.hpp answers
//  "what does the word at X mean" and, in igFbInheritable(), "is this plane a
//  layout this driver understands at all". Neither answers the question a
//  framebuffer driver actually has to answer before it presents anything:
//
//      given a plane the firmware already programmed, what is the base
//      address, the byte stride and the byte EXTENT of the pixels, and does
//      that extent fit inside the memory this driver actually mapped?
//
//  That is this file. It is the last purely-arithmetic step before an
//  IOFramebuffer subclass, and it is deliberately landed BEFORE that subclass,
//  because the subclass is unexecutable off macOS and this is not.
//
//  WHAT THIS FILE DOES NOT DECIDE
//  ------------------------------
//  It does not decode PLANE_CTL, it does not convert a PLANE_STRIDE word into
//  bytes, and it does not derive bytes-per-pixel from a format code.
//  IntelFramebuffer.hpp owns all three and check-registers.py checks that
//  ownership. Duplicating any of them here would give this tree two answers to
//  one question, which is the defect tests/test_igpu_mmio_ranges.py exists to
//  prevent one layer down. Everything here takes ALREADY-DECODED values as
//  arguments and joins them.
//
//  It also does not settle WHICH aperture the surface offset is relative to.
//  PLANE_SURF holds a graphics-address-space offset, and mapping that space is
//  a BAR this track has never mapped -- BAR0 is the 16 MiB register block and
//  is the only mapping IntelDisplayProbe has ever made. The aperture size is
//  therefore a PARAMETER here, supplied by whoever eventually maps it, and a
//  caller with no such mapping passes zero and is refused. Writing a BAR index
//  into this header would be inventing evidence; see CLAUDE.md on values this
//  project invented versus values it witnessed.
//

#ifndef INTEL_SCANOUT_HPP
#define INTEL_SCANOUT_HPP

//
//  Local width-explicit typedefs, for the same reason IntelDisplay.hpp and
//  IntelFramebuffer.hpp each carry their own: this header includes nothing, so
//  it cannot borrow UInt32 from IOKit or uint32_t from <cstdint>.
//
typedef unsigned int        IGS32;
typedef unsigned long long  IGS64;

//
//  THE 64-BIT PRODUCT IS THE POINT OF THIS FILE
//  --------------------------------------------
//  bytesPerRow * height is the one multiplication on the scanout path, and in
//  32 bits it WRAPS rather than saturating. A 1920x1080 XRGB_8888 desktop is
//  about 8 MiB and wraps nowhere near 32 bits, so the wrap is not reachable on
//  the panel this track measured -- which is exactly why it needs a guard
//  rather than a comment. A stride word misread by one bit, or a height read
//  out of a register that was never mapped, produces a product that wraps to a
//  SMALL number, and a small extent passes every containment test there is.
//  The failure is then a scanout descriptor that looks conservative and points
//  the display engine past the end of the mapping.
//
//  This is handoff22 SS1b's IGU64 asymmetry in a second place: truncation is not
//  symmetric. Losing the high bits of an extent makes it look SAFER, and the
//  direction that looks safer is the direction that scribbles.
//
#define IG_SCAN_INVALID32               0xFFFFFFFFu
#define IG_SCAN_INVALID64               0xFFFFFFFFFFFFFFFFull

//
//  The largest extent this header will hand out. Not a hardware limit -- it is
//  a POLICY CEILING, in the sense CLAUDE.md reserves that phrase for, and it is
//  named that way so nobody later cites it as a measured maximum. The ceiling
//  is 32 bits because IOFramebuffer's descriptor fields are 32-bit and an
//  extent that does not fit one cannot be described to it truthfully.
//
#define IG_SCAN_EXTENT_POLICY_CEILING   0xFFFFFFFFull

//
//  PLANE_SURF -- the surface base.
//
//  MEASURED against reference/i915/skl_universal_plane_regs.h: _PLANE_SURF_1_A
//  is 0x7019C and the register's address field is the 4 KiB-aligned upper bits.
//  The low twelve bits are flags rather than address, so a naive read of the
//  whole word yields a base a few bytes off and a picture that tears rather
//  than one that is obviously wrong.
//
#define IG_SCAN_SURF_ADDR_MASK          0xFFFFF000u
#define IG_SCAN_SURF_ALIGN_BYTES        0x1000u

//
//  THE VERDICT FAMILY
//  ------------------
//  A verdict rather than a bool, for the reason 3050-handoff32 SS2 spells out:
//  a bool refusal is satisfied by every refusal reason at once, and these six
//  call for six different actions. NO_SURFACE is a bug in this driver's read
//  path; UNALIGNED is a bug in this driver's decode; OUTSIDE_APERTURE is a fact
//  about how much memory got mapped.
//
//  The LIMIT is INCLUSIVE -- IG_SCAN_VERDICT_LIMIT is 6 and
//  IG_SCAN_ERR_OUTSIDE_APERTURE is 6 -- and so is EVERY other verdict family in
//  this track. That is measured rather than remembered: each family spells its
//  bound "the largest verdict above" and equals its largest member
//  (IG_MODE_VERDICT_LIMIT 6 == IG_MODE_SRC_MISMATCH, IG_FB_INHERIT_VERDICT_LIMIT
//  5 == IG_FB_INHERIT_GEOMETRY, IG_READ_VERDICT_LIMIT 4 ==
//  IG_READ_PAST_MAPPING), and tools/check-registers.py compares limit against
//  max(codes) as an EQUALITY per family, so a family carrying an exclusive limit
//  could not pass the register gate at all.
//
//  An earlier revision of this paragraph said IG_MODE_ and IG_FB_INHERIT_ were
//  exclusive. They never were, and that sentence is the exact shape of the
//  defect it was warning about: handoff20 SS1 is igModeVerdictName() carrying
//  arms 0..5 against an INCLUSIVE limit of 6, so IG_MODE_SRC_MISMATCH published
//  the unknown string with all four gates green. A comment that gets the
//  convention backwards hands the next author the same bug in prose form.
//  Bound an exhaustiveness loop `<=` the constant below, for every family.
//
#define IG_SCAN_OK                      0u  // the descriptor below is usable
#define IG_SCAN_ERR_NO_SURFACE          1u  // the SURF word was never read
#define IG_SCAN_ERR_UNALIGNED           2u  // base is not 4 KiB aligned
#define IG_SCAN_ERR_ZERO_GEOMETRY       3u  // a width, height or row of zero
#define IG_SCAN_ERR_ROW_TOO_SHORT       4u  // the row cannot hold the pixels
#define IG_SCAN_ERR_EXTENT_OVERFLOW     5u  // the extent exceeds the ceiling
#define IG_SCAN_ERR_OUTSIDE_APERTURE    6u  // base + extent runs off the end
#define IG_SCAN_VERDICT_LIMIT           6u  // the largest verdict above

//
//  A refused MMIO read.
//
//  IG_MMIO_INVALID in IntelDisplay.hpp is 0xFFFFFFFF and every refusal arm of
//  IntelDisplayProbe::regRead32() returns it, so an all-ones SURF word is the
//  read never having happened. The value is spelled here rather than included,
//  because this header includes nothing; the two are pinned equal by the test
//  suite instead, which is the same trick IntelFramebuffer.hpp uses for its own
//  IG_FB_INVALID.
//
//  igScanSurfRefused(0u) is deliberately FALSE. Zero is a legal graphics
//  address -- the very base of the aperture -- so treating it as a refusal
//  would refuse a plane that is genuinely programmed there. That is the
//  opposite polarity to a family where zero means unprogrammed, and it is a
//  decision about THIS register rather than a house style.
//
static inline int igScanSurfRefused(IGS32 surf)
{
    return surf == IG_SCAN_INVALID32;
}

//
//  The 4 KiB-aligned base address carried by a PLANE_SURF word.
//
//  Returns the masked address for any input, including a refused one -- the
//  refusal is igScanSurfRefused()'s job and igScanDescribe()'s to act on.
//  Folding the refusal in here would make a masked all-ones address
//  indistinguishable from a real plane at the top of the space.
//
static inline IGS32 igScanSurfaceAddress(IGS32 surf)
{
    return surf & IG_SCAN_SURF_ADDR_MASK;
}

static inline int igScanSurfaceAligned(IGS32 surf)
{
    return (surf & (IG_SCAN_SURF_ALIGN_BYTES - 1u)) == 0u;
}

//
//  The byte extent of the visible pixels: bytesPerRow * height.
//
//  Computed and RETURNED in 64 bits, never narrowed here. A function that
//  returned IGS32 would have already destroyed the evidence its own caller
//  needs to refuse -- the overflow would be a fact about a value that no longer
//  exists. Both operands are widened before the multiply, so the product is
//  exact for every 32-bit input pair.
//
static inline IGS64 igScanExtentBytes(IGS32 bytesPerRow, IGS32 height)
{
    return (IGS64) bytesPerRow * (IGS64) height;
}

static inline int igScanExtentFits(IGS64 extent)
{
    return extent != 0ull && extent <= IG_SCAN_EXTENT_POLICY_CEILING;
}

//
//  The minimum row length a given geometry needs.
//
//  Also 64-bit, and for the identical reason: width * bytesPerPixel is a second
//  multiplication of two 32-bit values, and a wrapped minimum is SMALLER than
//  the truth, so a row that is far too short compares as adequate.
//
static inline IGS64 igScanMinimumRowBytes(IGS32 width, IGS32 bytesPerPixel)
{
    return (IGS64) width * (IGS64) bytesPerPixel;
}

//
//  Does [base, base + extent) lie inside an aperture of apertureBytes?
//
//  The test is `base + extent <= apertureBytes` computed in 64 bits, never
//  `base < apertureBytes`. handoff22 SS1b measured why one layer down: an offset
//  one byte inside a mapping is inside it by any naive test and still reads
//  past the end. Here the overhang is a whole framebuffer rather than three
//  bytes.
//
//  An aperture of zero refuses everything, which is the intended answer for a
//  caller that has not mapped one. Refuse rather than trust is the same
//  direction IntelDisplayProbe::unmapBar0() zeroes fBar0Length for.
//
static inline int igScanWithinAperture(IGS32 base, IGS64 extent, IGS64 apertureBytes)
{
    if (apertureBytes == 0ull)
        return 0;
    return ((IGS64) base + extent) <= apertureBytes;
}

//
//  THE JOIN
//  --------
//  One verdict over the whole descriptor, not a flag per field. The reason is
//  handoff24 SS1a's, arrived at for the third time in this tree: these values
//  compose ONE reported thing. A per-field flag would report a good base, a
//  good stride, and one sentinel, and a caller that published four of the five
//  would be describing a framebuffer that was never measured.
//
//  THE ORDER IS THE ASSERTION, and each step below is load-bearing:
//
//  1. NO_SURFACE first. Every later test is arithmetic over the surf word, and
//     arithmetic over a refused read is arithmetic over 0xFFFFFFFF. Masking it
//     yields 0xFFFFF000, which is 4 KiB aligned -- so an alignment-first order
//     reports a refused read as a well-aligned plane.
//
//  2. UNALIGNED before the geometry. Alignment is a property of the one word
//     already in hand; the geometry needs three more, any of which may be a
//     refused read this function was not handed the means to detect.
//
//  3. ZERO_GEOMETRY before ROW_TOO_SHORT, and this is the arm most likely to be
//     "simplified" away. A zero WIDTH makes the minimum row length zero, and
//     every bytesPerRow satisfies `>= 0`, so a containment-first order reports
//     a zero-width plane as having an adequate row and then produces a zero
//     extent. That is 3050-handoff32 SS3's empty-before-contained reasoning
//     exactly: an empty thing trivially satisfies a containment test, so the
//     emptiness arm must run first or it is unreachable.
//
//  4. EXTENT_OVERFLOW before OUTSIDE_APERTURE, and this is the reason the file
//     exists. A wrapped extent is a SMALL extent, and a small extent fits
//     inside any aperture. Testing containment first makes the overflow arm
//     unreachable for precisely the inputs that need it.
//
//  A blob that is bad in two ways at once pins the order, and the suite drives
//  each such pair rather than trusting the prose above.
//
static inline IGS32 igScanDescribe(IGS32 surf,
                                   IGS32 bytesPerRow,
                                   IGS32 width,
                                   IGS32 height,
                                   IGS32 bytesPerPixel,
                                   IGS64 apertureBytes)
{
    IGS64 extent;

    if (igScanSurfRefused(surf))
        return IG_SCAN_ERR_NO_SURFACE;

    if (!igScanSurfaceAligned(surf))
        return IG_SCAN_ERR_UNALIGNED;

    if (width == 0u || height == 0u || bytesPerRow == 0u || bytesPerPixel == 0u)
        return IG_SCAN_ERR_ZERO_GEOMETRY;

    if ((IGS64) bytesPerRow < igScanMinimumRowBytes(width, bytesPerPixel))
        return IG_SCAN_ERR_ROW_TOO_SHORT;

    extent = igScanExtentBytes(bytesPerRow, height);
    if (!igScanExtentFits(extent))
        return IG_SCAN_ERR_EXTENT_OVERFLOW;

    if (!igScanWithinAperture(igScanSurfaceAddress(surf), extent, apertureBytes))
        return IG_SCAN_ERR_OUTSIDE_APERTURE;

    return IG_SCAN_OK;
}

//
//  A convenience predicate for the eventual IOFramebuffer subclass.
//
//  An EQUALITY against IG_SCAN_OK, never a negation of one error arm. 3050-
//  handoff33 SS2 is the cost of the alternative: `!= ERR_SOMETHING` is TRUE for
//  every OTHER refusal, so a descriptor refused for a reason added later would
//  report as usable. A seventh verdict must make this function return false
//  without anybody remembering to edit it.
//
static inline int igScanUsable(IGS32 verdict)
{
    return verdict == IG_SCAN_OK;
}

#endif  // INTEL_SCANOUT_HPP
