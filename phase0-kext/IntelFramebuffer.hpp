//
//  IntelFramebuffer.hpp
//  Gen12 (Tiger Lake) framebuffer inheritance -- reading an ALREADY-RUNNING
//  display configuration back out of the plane registers.
//
//  Same rule as IntelDisplay.hpp, IntelGpuId.hpp and the five NVIDIA headers:
//  DEPENDENCY-FREE. No includes, no IOKit, no kernel types, no allocation, no
//  sleep. Every function is a total function of its arguments, so
//  tests/test_intel_framebuffer.cpp compiles this IDENTICAL file under MSVC and
//  runs it on a machine with no Intel GPU and no macOS.
//
//  WHY THIS IS A SEPARATE FILE
//
//  IntelDisplay.hpp answers "where is register X" -- it is an ADDRESSING layer,
//  checked against offset #defines in the vendored i915 register headers, and
//  it is owned by another crew terminal. IntelGpuId.hpp answers "is this our
//  silicon". This file answers a third and different question: given the
//  32-bit VALUES those registers currently hold, what framebuffer is the
//  firmware already scanning out? That is field DECODING, not addressing, and
//  its evidence is a mix of vendored field masks and the one live capture.
//
//  WHY FRAMEBUFFER INHERITANCE IS THE RIGHT NEXT LAYER
//
//  A driver that must PROGRAM a mode needs PLL dividers, DPLL/DPCLKA muxing,
//  power-well sequencing, DDI/link training and watermarks -- none of which the
//  one Linux boot could answer, and several of which are unanswerable on an
//  eDP-only machine (see the capture's READING NOTES: Q1/Q2/Q3 are not merely
//  unanswered, they are unanswerable there). A driver that INHERITS a mode
//  needs none of it. The firmware has already trained the link, lit the panel
//  and pointed a plane at memory; the driver reads six registers and learns
//  where the pixels are. That is the shortest real path from "kext loads" to
//  "kext can put a pixel on a screen", and every input it needs was WITNESSED
//  on 2026-08-16 rather than guessed.
//
//  THE ONE OBSERVED REGISTER SET, VERBATIM
//
//  From igpu-results/tgl-capture-2026-08-16-022556.txt, pipe A plane 1 @0x70180
//  on device 0x9A68 with the eDP panel lit at 1920x1080:
//
//      +0x00 CTL     0x82009000
//      +0x08 STRIDE  0x0000003C
//      +0x10 SIZE    0x0437077F
//      +0x1C SURF    0x01058000
//      +0x20 KEYMAX  0xFF000000
//      +0x2C OFFSET  0x01058000   <-- see the hazard note on _PLANE_OFFSET below
//
//  Every constant below whose name carries _OBSERVED_2026_08_16 comes from that
//  block. Every other constant is transcribed from
//  reference/i915/skl_universal_plane_regs.h at Linux v6.12. The distinction is
//  the only evidence trail this project has, and it is preserved per CLAUDE.md.
//

#ifndef INTEL_FRAMEBUFFER_HPP
#define INTEL_FRAMEBUFFER_HPP

//
//  Own typedef rather than reusing IGU32 or IGID32. All three headers are
//  standalone and includable in any order or alone; a shared type would make
//  one of them depend on another and break the dependency-free rule that lets
//  the host suites compile them at all.
//
typedef unsigned int IGFB32;

#define IG_FB_INVALID                   0xFFFFFFFFu

//
//  ---- 1. plane register offsets, relative to the plane's own base ----------
//
//  These are DELTAS from _PLANE_CTL_1_A (0x70180), computed from the vendored
//  literals rather than from the capture's "+0x.." column. The capture's column
//  is a dump stride and happens to agree; the vendored file is the authority.
//
//      _PLANE_CTL_1_A     0x70180   -> +0x00
//      _PLANE_STRIDE_1_A  0x70188   -> +0x08
//      _PLANE_POS_1_A     0x7018c   -> +0x0C
//      _PLANE_SIZE_1_A    0x70190   -> +0x10
//      _PLANE_SURF_1_A    0x7019c   -> +0x1C
//      _PLANE_KEYMAX_1_A  0x701a0   -> +0x20
//      _PLANE_OFFSET_1_A  0x701a4   -> +0x24
//
//  IntelDisplay.hpp already owns igPlaneReg() and IG_PLANE_CTL_BASE, so this
//  file deliberately does NOT compute absolute addresses. It decodes values.
//  Duplicating the addressing arithmetic here would create a second copy that
//  can disagree with the first, and a wrong plane base does not fault -- it
//  reads a real register belonging to another plane.
//
#define IG_FB_OFF_CTL                   0x00u
#define IG_FB_OFF_STRIDE                0x08u
#define IG_FB_OFF_POS                   0x0Cu
#define IG_FB_OFF_SIZE                  0x10u
#define IG_FB_OFF_SURF                  0x1Cu
#define IG_FB_OFF_KEYMAX                0x20u
#define IG_FB_OFF_OFFSET                0x24u

//
//  ---- 2. PLANE_CTL fields --------------------------------------------------
//
//  PLANE_CTL_ENABLE is REG_BIT(31).
//
#define IG_FB_CTL_ENABLE                0x80000000u

//
//  THE FORMAT FIELD IS 5 BITS ON THIS SILICON, NOT 4.
//
//  The vendored header carries BOTH definitions and says why:
//
//      PLANE_CTL_FORMAT_MASK_SKL  REG_GENMASK(27, 24)   pre-icl
//      PLANE_CTL_FORMAT_MASK_ICL  REG_GENMASK(27, 23)   icl+
//
//  Tiger Lake is icl+, so bit 23 is part of the field. Every FORMAT_* value
//  the header spells is REG_FIELD_PREP'd against the SKL mask even for the
//  icl-only codes, which means the numeric codes are stated in 4-bit terms and
//  the 5-bit field simply widens the space below them. Decoding with the SKL
//  mask on TGL silently drops bit 23: an ICL-only format sitting at an odd
//  5-bit code would decode as a DIFFERENT, valid, pre-icl format rather than as
//  garbage -- and a plane decoded as the wrong bpp produces a stride that is
//  wrong by an integer factor, which lands the scanout inside the framebuffer
//  rather than outside it. No fault, just a sheared picture.
//
//  The observed CTL is 0x82009000. (0x82009000 >> 23) & 0x1F == 0x04, and
//  (0x82009000 >> 24) & 0x0F == 0x02. Both are formats the header names
//  (XRGB_8888 is 4, XRGB_2101010 is 2), so this is exactly the case where a
//  wrong mask yields a plausible answer. The panel is a 32-bpp desktop, so 4 is
//  the correct reading and the 5-bit mask is the correct mask.
//
#define IG_FB_CTL_FORMAT_SHIFT          23u
#define IG_FB_CTL_FORMAT_MASK           0x1Fu

//
//  Format codes, transcribed from the vendored REG_FIELD_PREP values. Only the
//  ones this driver can actually consume are named; the YUV and Y-family codes
//  are deliberately absent rather than named-and-unhandled, because a named
//  constant reads as supported.
//
#define IG_FB_FORMAT_XRGB_2101010       2u
#define IG_FB_FORMAT_XRGB_8888          4u
#define IG_FB_FORMAT_XRGB_16161616F     6u
#define IG_FB_FORMAT_RGB_565            14u

//
//  PLANE_CTL_TILED_MASK is REG_GENMASK(12, 10).
//
//  The vendored header defines TILED_YF and TILED_4 to the SAME code 5:
//
//      PLANE_CTL_TILED_YF  REG_FIELD_PREP(PLANE_CTL_TILED_MASK, 5)
//      PLANE_CTL_TILED_4   REG_FIELD_PREP(PLANE_CTL_TILED_MASK, 5)
//
//  That is not a transcription error here -- it is upstream, and it is real:
//  the encoding is reused across generations, so code 5 means YF on SKL-family
//  parts and Tile4 on TGL+. This file names the CODE once and resolves the
//  meaning by generation in a comment rather than defining two names to one
//  value, because two names for one value invites a caller to compare against
//  the wrong one and be right by accident.
//
#define IG_FB_CTL_TILING_SHIFT          10u
#define IG_FB_CTL_TILING_MASK           0x07u

#define IG_FB_TILING_LINEAR             0u
#define IG_FB_TILING_X                  1u
#define IG_FB_TILING_Y                  4u
#define IG_FB_TILING_YF_OR_4            5u   // YF pre-tgl, Tile4 on tgl+

//
//  PLANE_CTL_RENDER_DECOMPRESSION_ENABLE is REG_BIT(15), and the observed CTL
//  has it SET (0x82009000 & 0x8000 != 0).
//
//  This matters more than it looks. A render-compressed surface is not a plain
//  linear-in-memory image: the visible pixels are described by a separate CCS
//  (colour control surface) and the main surface holds compressed blocks. A
//  driver that inherits this plane and then writes pixels into SURF as though
//  it were an ordinary framebuffer produces corruption, not a picture --
//  because the display engine keeps decompressing what it finds there.
//
//  So this is a REFUSAL condition for inheritance, not a decoded attribute. It
//  is named, checked, and reported rather than ignored.
//
#define IG_FB_CTL_RENDER_DECOMP         0x00008000u

//
//  PLANE_CTL_ASYNC_FLIP is REG_BIT(9). The observed CTL has bit 12 and bit 15
//  set (0x9000 == bits 12 and 15), so tiling code is 4 -- Tile Y -- and async
//  flip is clear.
//
#define IG_FB_CTL_ASYNC_FLIP            0x00000200u

//
//  ---- 3. PLANE_STRIDE -----------------------------------------------------
//
//  PLANE_STRIDE__MASK is REG_GENMASK(11, 0), so the field is 12 bits.
//
//  THE UNIT IS DERIVED FROM THE CAPTURE, NOT FROM THE VENDORED HEADER.
//
//  The vendored header states the mask and nothing about units. The vendored
//  intel_display.c has no reachable statement of a stride multiplier either --
//  grepped for "/ 64", "<< 6", "* 64", "tile_width" and "stride_alignment";
//  the arithmetic lives in code this tree does not vendor. So the unit is
//  derived from the ONE observation instead, and it is derived rather than
//  guessed:
//
//      panel is 1920x1080, PIPESRC 0x077F0437 decodes +1 to exactly 1920x1080
//      format is XRGB_8888, so 4 bytes per pixel
//      a tight row is 1920 * 4 == 7680 bytes
//      the observed STRIDE field is 0x3C == 60
//      7680 / 60 == 128
//
//  128 bytes per unit, and it divides exactly. That is a single data point, so
//  it is named for what it is: a value DERIVED from one measurement, carrying
//  the date. It is not presented as a hardware constant this project verified
//  in general. A second panel width, or a plane at a different bpp, would
//  either confirm it or kill it -- and there is no second Linux boot to ask.
//
//  The alternative reading worth stating explicitly, since it is the one a
//  reader will suspect: if the unit were 64 bytes, a tight 1920x4 row would
//  need field value 120, not 60, and the observed register would be describing
//  a 960-pixel-wide surface on a 1920-pixel panel. The panel was lit and
//  correct, so 64 is refuted by the same measurement that established 128.
//
#define IG_FB_STRIDE_MASK               0x00000FFFu
#define IG_FB_STRIDE_UNIT_DERIVED_2026_08_16    128u

//
//  ---- 4. PLANE_SIZE and PLANE_POS -----------------------------------------
//
//  PLANE_POS carries Y in REG_GENMASK(31, 16) and X in REG_GENMASK(15, 0).
//  PLANE_SIZE has no field masks in the vendored header at all -- the header
//  defines the register and stops.
//
//  THE HALVES ARE INVERTED RELATIVE TO PIPESRC, AND THIS HAS ALREADY BITTEN.
//
//  handoff15 1 recorded it from this capture: PIPESRC reads 0x077F0437 and
//  PLANE_SIZE reads 0x0437077F. Same 1920x1080, byte-swapped halves. So a
//  single decoder reused for both TRANSPOSES the axes and still programs
//  cleanly -- a 1080-wide, 1920-tall surface on a 1920x1080 panel is a valid
//  register write that produces a scrambled screen and no error anywhere.
//
//  Hence: PIPESRC is horizontal-in-high-half; PLANE_SIZE is
//  horizontal-in-LOW-half. Both store value - 1. This file decodes PLANE_SIZE
//  only, and names the shift constants explicitly rather than sharing a helper
//  with the transcoder path.
//
#define IG_FB_SIZE_WIDTH_SHIFT          0u
#define IG_FB_SIZE_HEIGHT_SHIFT         16u
#define IG_FB_SIZE_HALF_MASK            0xFFFFu

#define IG_FB_POS_X_SHIFT               0u
#define IG_FB_POS_Y_SHIFT               16u
#define IG_FB_POS_HALF_MASK             0xFFFFu

//
//  ---- 5. PLANE_SURF -------------------------------------------------------
//
//  PLANE_SURF_ADDR_MASK is REG_GENMASK(31, 12), so the surface address is
//  4 KiB aligned and the low 12 bits are flags (PLANE_SURF_DECRYPT is
//  REG_BIT(2)).
//
//  The address is a GGTT offset, NOT a physical address and NOT a CPU virtual
//  one. Reading 0x01058000 and handing it to anything that expects a physical
//  address produces a read of unrelated memory. Named here so the type of the
//  number is part of the record.
//
#define IG_FB_SURF_ADDR_MASK            0xFFFFF000u
#define IG_FB_SURF_DECRYPT              0x00000004u

//
//  ---- 6. the observed register block --------------------------------------
//
//  Pinned as bare literals so the host suite can assert each one against a
//  retyped copy and neither can quietly drift. Per the
//  self-cancelling-constants finding, the suite must NOT check a decode against
//  the same #define the decode reads.
//
#define IG_FB_CTL_OBSERVED_2026_08_16       0x82009000u
#define IG_FB_STRIDE_OBSERVED_2026_08_16    0x0000003Cu
#define IG_FB_SIZE_OBSERVED_2026_08_16      0x0437077Fu
#define IG_FB_SURF_OBSERVED_2026_08_16      0x01058000u
#define IG_FB_KEYMAX_OBSERVED_2026_08_16    0xFF000000u

//
//  What the panel actually was, from the same capture's connector section
//  ("card1-eDP-1 connected modes=[1920x1080...]") and from Q4's PIPESRC line.
//  Two independent readings of the same fact, pinned separately on purpose.
//
#define IG_FB_PANEL_WIDTH_OBSERVED_2026_08_16   1920u
#define IG_FB_PANEL_HEIGHT_OBSERVED_2026_08_16  1080u


//
//  ---- 7. decoders ---------------------------------------------------------
//

static inline bool igFbPlaneEnabled(IGFB32 ctl)
{
    return (ctl & IG_FB_CTL_ENABLE) != 0u;
}

static inline IGFB32 igFbFormat(IGFB32 ctl)
{
    return (ctl >> IG_FB_CTL_FORMAT_SHIFT) & IG_FB_CTL_FORMAT_MASK;
}

static inline IGFB32 igFbTiling(IGFB32 ctl)
{
    return (ctl >> IG_FB_CTL_TILING_SHIFT) & IG_FB_CTL_TILING_MASK;
}

static inline bool igFbRenderCompressed(IGFB32 ctl)
{
    return (ctl & IG_FB_CTL_RENDER_DECOMP) != 0u;
}

static inline bool igFbAsyncFlip(IGFB32 ctl)
{
    return (ctl & IG_FB_CTL_ASYNC_FLIP) != 0u;
}

//
//  Bytes per pixel for a format code, or 0 for anything this driver cannot
//  consume.
//
//  Zero rather than a guess, and zero rather than a "reasonable default" of 4.
//  Every caller below multiplies by this, so a wrong default silently produces
//  a wrong stride and a wrong surface size for a YUV plane -- which is the
//  failure that lands a scanout inside the framebuffer and shears the picture
//  instead of faulting.
//
static inline IGFB32 igFbBytesPerPixel(IGFB32 format)
{
    switch (format) {
    case IG_FB_FORMAT_RGB_565:        return 2u;
    case IG_FB_FORMAT_XRGB_8888:      return 4u;
    case IG_FB_FORMAT_XRGB_2101010:   return 4u;
    case IG_FB_FORMAT_XRGB_16161616F: return 8u;
    default:                          return 0u;
    }
}

//
//  PLANE_SIZE decode. Both halves store value - 1, so both are +1'd.
//
//  A raw register of 0x00000000 therefore decodes to 1x1 rather than 0x0. That
//  is the honest reading of the convention, and it is why callers must consult
//  igFbPlaneEnabled() rather than treating a small size as "no plane": every
//  unused display page on this silicon reads 0x00000000 (capture READING
//  NOTES), so an unprogrammed plane decodes as a legitimate-looking 1x1.
//
static inline IGFB32 igFbSizeWidth(IGFB32 size)
{
    return ((size >> IG_FB_SIZE_WIDTH_SHIFT) & IG_FB_SIZE_HALF_MASK) + 1u;
}

static inline IGFB32 igFbSizeHeight(IGFB32 size)
{
    return ((size >> IG_FB_SIZE_HEIGHT_SHIFT) & IG_FB_SIZE_HALF_MASK) + 1u;
}

//
//  PLANE_POS decode. POS does NOT use the value-1 convention -- a plane at the
//  top-left corner sits at 0,0 and a -1 encoding could not express it. So these
//  are raw field reads, deliberately asymmetric with the SIZE decoders above,
//  and that asymmetry is the point rather than an oversight.
//
static inline IGFB32 igFbPosX(IGFB32 pos)
{
    return (pos >> IG_FB_POS_X_SHIFT) & IG_FB_POS_HALF_MASK;
}

static inline IGFB32 igFbPosY(IGFB32 pos)
{
    return (pos >> IG_FB_POS_Y_SHIFT) & IG_FB_POS_HALF_MASK;
}

//
//  Stride in BYTES, from the raw register.
//
static inline IGFB32 igFbStrideBytes(IGFB32 stride)
{
    return (stride & IG_FB_STRIDE_MASK)
           * IG_FB_STRIDE_UNIT_DERIVED_2026_08_16;
}

//
//  The GGTT offset of the surface. Low 12 bits masked off -- they are flags.
//
static inline IGFB32 igFbSurfaceOffset(IGFB32 surf)
{
    return surf & IG_FB_SURF_ADDR_MASK;
}

static inline bool igFbSurfaceDecrypt(IGFB32 surf)
{
    return (surf & IG_FB_SURF_DECRYPT) != 0u;
}

//
//  ---- 7b. the surface RANGE ------------------------------------------------
//
//  igFbSurfaceOffset() bounds nothing and igFbSurfaceBytes() bounds nothing,
//  and until now nothing put the two together. That is the identical defect
//  this tree already found and fixed once on the NVIDIA side:
//  nvGspStageAddressOk() bounded only the BASE, so `base + paddedBytes` was
//  unbounded and a base that passed could still describe a range running off
//  the end of the aperture. The fix there was nvGspStageRangeOk(); this is the
//  same fix on the Intel side, and it is being written from the memory of that
//  bug rather than from a new discovery.
//
//  Why a range check and not just a base check. On inheritance the driver maps
//  what the firmware already programmed. If the decoded extent overruns the
//  GGTT, the mapping either fails late or -- worse -- succeeds against whatever
//  else is mapped above it, and the display engine keeps scanning out of the
//  original range regardless. So the failure is not a fault the caller can
//  observe; it is a picture assembled from someone else's memory.
//
//  IG_FB_GGTT_LIMIT IS A POLICY CEILING, NOT A HARDWARE WIDTH.
//
//  It is deliberately named and documented the same way NV_GSP_FW_ADDR_BITS is,
//  because the honest statement about it is the same. Nothing in this tree
//  measures the GGTT size on this silicon: the 2026-08-16 capture read plane
//  registers, not GTT configuration, and reference/i915/ vendors register
//  headers plus intel_display.c -- the GGTT sizing arithmetic lives in files
//  this tree does not vendor. 4 GiB is the largest offset PLANE_SURF's 20-bit
//  page field can name at all (0xFFFFF000 + 4096 == 2^32), so a ceiling at 2^32
//  is the tightest bound derivable from the REGISTER rather than from the GTT.
//
//  That makes it a true statement about what the field can express and NOT a
//  claim about how much GGTT exists. A real driver on real silicon must read
//  the actual GTT size and tighten this; the name says CEILING so that whoever
//  does is not misled into thinking it was measured.
//
//  The comparison is done in 64-bit for the same reason the NVIDIA fix was:
//  offset + bytes is up to ~2^32 + 2^35, and computing it in 32 bits WRAPS to a
//  small plausible number that passes any bound. A wrapped sum is the failure
//  mode this function exists to prevent, so it must not be the arithmetic the
//  function itself uses.
//
#define IG_FB_GGTT_POLICY_LIMIT     0x100000000ull

static inline bool igFbSurfaceRangeOk(IGFB32 surf, IGFB32 bytes)
{
    //
    //  A zero extent is a refusal, not an empty success. igFbSurfaceBytes()
    //  returns 0 for every input it cannot describe -- a zero stride, or a
    //  product that would overflow -- so accepting 0 here would launder that
    //  refusal into an OK range of nothing, and a caller checking only this
    //  function would map nothing and believe it inherited a framebuffer.
    //
    if (bytes == 0u)
        return false;

    //
    //  64-bit throughout. Neither operand is widened by the other, so the cast
    //  is on the FIRST term and not on the sum -- casting the sum would be the
    //  bug, since the wrap would already have happened.
    //
    return ((unsigned long long)igFbSurfaceOffset(surf) + (unsigned long long)bytes)
           <= IG_FB_GGTT_POLICY_LIMIT;
}


//
//  ---- 8. the inheritance verdict ------------------------------------------
//
//  The whole point of the file: can this plane's configuration be adopted as
//  it stands?
//
//  Stated as an enumerated REASON rather than a bool. A bool collapses "this
//  plane is off", "this plane is compressed" and "this format is one we cannot
//  read" into one answer, and those three demand completely different responses
//  from a driver -- wait, refuse permanently, or fall back to a mode set. A
//  single false would send all three down the same path.
//
#define IG_FB_INHERIT_OK                0u
#define IG_FB_INHERIT_DISABLED          1u
#define IG_FB_INHERIT_COMPRESSED        2u
#define IG_FB_INHERIT_FORMAT            3u
#define IG_FB_INHERIT_STRIDE            4u
#define IG_FB_INHERIT_GEOMETRY          5u

//
//  The largest verdict above, and the reason it is being added AFTER the six it
//  bounds rather than alongside them.
//
//  IntelDisplay.hpp's IG_MODE_* and IG_JOIN_* families each carry a
//  VERDICT_LIMIT, and tools/check-registers.py derives three properties from it
//  that no C++ test can: that the set is distinct, that OK is zero, and that the
//  limit still equals the largest member. That last one is the one a hand-kept
//  test list cannot supply -- add a seventh verdict here without a matching
//  checkU32 line in tests/test_intel_framebuffer.cpp and every gate in this tree
//  stays green while the new code is pinned by nothing at all.
//
//  This family had six verdicts and no bound, so it was the one family the
//  parameterised checker could not be pointed at. Nothing was wrong with the six
//  values; what was missing was the derived number that makes a SEVENTH one
//  impossible to add unnoticed.
//
#define IG_FB_INHERIT_VERDICT_LIMIT     5u

static inline IGFB32 igFbInheritable(IGFB32 ctl, IGFB32 stride, IGFB32 size)
{
    if (!igFbPlaneEnabled(ctl))
        return IG_FB_INHERIT_DISABLED;

    //
    //  Checked BEFORE the format, deliberately. A compressed XRGB_8888 plane
    //  has a format this driver understands and a layout it does not, so a
    //  format-first order would return OK for it.
    //
    if (igFbRenderCompressed(ctl))
        return IG_FB_INHERIT_COMPRESSED;

    IGFB32 bpp = igFbBytesPerPixel(igFbFormat(ctl));
    if (bpp == 0u)
        return IG_FB_INHERIT_FORMAT;

    IGFB32 bytes = igFbStrideBytes(stride);
    if (bytes == 0u)
        return IG_FB_INHERIT_STRIDE;

    IGFB32 width  = igFbSizeWidth(size);
    IGFB32 height = igFbSizeHeight(size);

    //
    //  A 1x1 plane is what an UNPROGRAMMED page decodes to, because the whole
    //  page reads zero and both halves are +1'd. Refusing it here is what stops
    //  a driver from "inheriting" a transcoder that was never programmed --
    //  which on this silicon is indistinguishable from a programmed one by any
    //  single register read.
    //
    if (width < 2u || height < 2u)
        return IG_FB_INHERIT_GEOMETRY;

    //
    //  The row must FIT. If the stride is narrower than the visible row, the
    //  display engine walks into the next row's pixels every scanline -- a
    //  sheared image, not a fault. This is the check that makes the derived
    //  128-byte unit load-bearing rather than decorative.
    //
    if (bytes < width * bpp)
        return IG_FB_INHERIT_STRIDE;

    return IG_FB_INHERIT_OK;
}

//
//  Total bytes the surface occupies, for a bounds check against the aperture.
//
//  stride * height, NOT width * bpp * height. The two differ by the stride
//  padding, and the smaller of them is the wrong one to bound with: the display
//  engine reads a full stride per scanline, so a mapping sized from the visible
//  width falls short of what hardware actually touches on the last row.
//
//  Returns 0 when the inputs cannot describe a surface, so a caller that
//  forgets to check the verdict first gets a zero-size mapping rather than a
//  plausible wrong one.
//
static inline IGFB32 igFbSurfaceBytes(IGFB32 stride, IGFB32 size)
{
    IGFB32 bytes  = igFbStrideBytes(stride);
    IGFB32 height = igFbSizeHeight(size);

    if (bytes == 0u)
        return 0u;

    //
    //  Overflow refusal rather than a wrapped product. 12-bit stride field *
    //  128 is at most 0x7FF80, and height is at most 65536, so the product can
    //  reach ~2^35 -- it does NOT fit in 32 bits, and a wrapped value is a
    //  small plausible number that would pass any aperture bound.
    //
    //  THE DIVISOR CANNOT BE ZERO, so there is no guard on it.
    //
    //  This line used to read `if (height != 0u && bytes > ...)`. That conjunct
    //  was DEAD TEXT: igFbSizeHeight() returns ((size >> 16) & 0xFFFF) + 1u,
    //  whose range is 1..65536, so no 32-bit input can make it 0. A guard whose
    //  false branch is unreachable by construction is worse than no guard --
    //  it reads as defensive, no test can drive it, and it invites a later
    //  reader to believe a zero height is a case this file handles. It is not
    //  a case that exists. If igFbSizeHeight() ever stops being +1'd, this
    //  comment is the thing that has to be revisited along with it.
    //
    if (bytes > (IG_FB_INVALID / height))
        return 0u;

    return bytes * height;
}

#endif  // INTEL_FRAMEBUFFER_HPP
