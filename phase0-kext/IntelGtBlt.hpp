//
//  IntelGtBlt.hpp
//  Gen12 (Tiger Lake) 2D BLITTER command packets -- the first layer in this
//  tree that describes GPU WORK rather than the plumbing that would deliver it.
//
//  Everything below IntelGtCmd.hpp is about DELIVERY: ring geometry, tail/head
//  wrap, execlist descriptors, a logical ring context image. None of it says
//  what the engine would DO. This header composes two packets that do say it --
//  a solid colour fill over a rectangle, and a rectangle copied from one
//  surface to another. That is the smallest honest unit of 2D acceleration
//  there is.
//
//  DEPENDENCY-FREE, same rule as every other header in phase0-kext/. The one
//  #include is a quoted sibling (IntelGtCmd.hpp) for IGU32/IGU64, which is
//  still inside phase0-kext/ and so still parses standalone under
//  -I phase0-kext.
//
//  EVIDENCE, STATED UP FRONT
//  -------------------------
//  This header is the FIRST in the GT stack whose command words are CITED
//  rather than composed from policy. reference/i915/intel_gpu_commands.h was
//  vendored on 2026-08-26 and carries the blitter client, both opcodes, and the
//  colour-depth, write-mask and raster-op families:
//
//    :239  COLOR_BLT_CMD            (2 << 29 | 0x40 << 22 | (5 - 2))
//    :240  XY_COLOR_BLT_CMD         (2 << 29 | 0x50 << 22)
//    :270  XY_SRC_COPY_BLT_CMD      (2 << 29 | 0x53 << 22)
//    :272  BLT_WRITE_A              (2 << 20)
//    :273  BLT_WRITE_RGB            (1 << 20)
//    :274  BLT_WRITE_RGBA           (BLT_WRITE_RGB | BLT_WRITE_A)
//    :275  BLT_DEPTH_8              (0 << 24)
//    :276  BLT_DEPTH_16_565         (1 << 24)
//    :277  BLT_DEPTH_16_1555        (2 << 24)
//    :278  BLT_DEPTH_32             (3 << 24)
//    :280  BLT_ROP_SRC_COPY         (0xcc << 16)
//    :281  BLT_ROP_COLOR_COPY       (0xf0 << 16)
//
//  That is why the constants below carry _I915_ names with a line number. The
//  ones that do NOT -- packet lengths, the DWORD_LENGTH field position, the
//  stride ceiling -- carry _POLICY_ names, and the distinction is not
//  cosmetic. A guess filed beside a measurement destroys the only evidence
//  trail this project has, and this session exists because four MI opcodes
//  spent a day wearing the wrong evidence class.
//
//  THE BLITTER CLIENT IS NOT THE MI CLIENT, AND THE FIELDS DO NOT LINE UP
//  ---------------------------------------------------------------------
//  IntelGtCmd.hpp's igGtMiHeader() puts the opcode at bit 23 with a 6-bit
//  field, because MI_INSTR() does. BLT commands are a DIFFERENT encoding in the
//  same word: client 2 at bit 29, and the opcode at bit **22** with a 7-bit
//  field. 0x50 << 22 and 0x50 << 23 are both well-formed 32-bit words and only
//  one of them is a fill.
//
//  So this header composes its own header word and deliberately does NOT reach
//  for igGtMiHeader(). Reusing it would be the exact shape handoff15 measured
//  on silicon for PLANE_SIZE vs PIPESRC -- one decoder reused for a sibling
//  layout, programming cleanly and meaning something else.
//
//  WHAT THIS DOES NOT ESTABLISH
//  ----------------------------
//  No packet composed here has ever been submitted. There is no ring, no
//  context, no GGTT binding and no engine that has executed a dword of it; the
//  laptop is muxless and PLAN.md termination condition A still holds, so even a
//  perfect blit puts nothing on the internal panel. This is a citable
//  description of two commands, checkable on a machine with no Intel GPU. It is
//  not acceleration, and the distance between the two is a ring nobody has
//  allocated.
//

#ifndef IntelGtBlt_hpp
#define IntelGtBlt_hpp

#include "IntelGtCmd.hpp"

// ---------------------------------------------------------------------------
// 1. The blitter client and the two opcodes.
//
// CITED. Upstream spells each command as a composed word rather than as a bare
// opcode, so the citation checker in tools/check-registers.py reads the
// PARENTHESISED opcode out of the composition and compares that -- see section
// 5 there for why comparing against the composed word would fail while being
// wrong.
// ---------------------------------------------------------------------------

// Bits 31:29 select the client. The blitter is 2.
// intel_gpu_commands.h:23 -- INSTR_BC_CLIENT 0x2, and every BLT command below
// spells it inline as (2 << 29).
#define IG_GTB_CLIENT_I915_2                    2u

// Bits 28:22 are the BLT opcode. SEVEN bits at bit 22, NOT the MI encoding's
// six bits at 23 -- see the header comment for why that difference is the
// whole reason this file composes its own header word.
#define IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50    0x50u  // :240 XY_COLOR_BLT_CMD
#define IG_GTB_OPCODE_XY_SRC_COPY_BLT_I915_0x53 0x53u  // :270 XY_SRC_COPY_BLT_CMD
#define IG_GTB_OPCODE_COLOR_BLT_I915_0x40       0x40u  // :239 COLOR_BLT_CMD

// POLICY, because these are field POSITIONS this project chose to spell as
// separate constants. Their VALUES are checkable against the shifts upstream
// writes inline, and tools/check-registers.py does check them -- an opcode
// correct in value and placed at the wrong bit is the same hang as a wrong
// opcode.
#define IG_GTB_CLIENT_SHIFT_POLICY_29           29u
#define IG_GTB_OPCODE_SHIFT_POLICY_22           22u
#define IG_GTB_OPCODE_MASK_POLICY_0x7F          0x7Fu
#define IG_GTB_LENGTH_MASK_POLICY_0xFF          0xFFu

// ---------------------------------------------------------------------------
// 2. Colour depth, write mask and raster op.
//
// CITED, all three families. Upstream stores these ALREADY SHIFTED into the
// header dword -- BLT_DEPTH_32 is (3 << 24), not 3 -- and they are kept that
// way here rather than un-shifted into a value plus a position.
//
// That choice is deliberate and it is the opposite of the opcode handling one
// section up. An un-shifted depth would need this header to invent a shift
// constant, and a shift this project invented sitting under an _I915_ name is
// precisely the fabricated citation the whole file is written against. Kept
// composed, the constant IS the upstream constant and the checker compares
// whole words.
// ---------------------------------------------------------------------------

#define IG_GTB_DEPTH_8_I915_0x00000000          0x00000000u  // :275 BLT_DEPTH_8
#define IG_GTB_DEPTH_16_565_I915_0x01000000     0x01000000u  // :276 BLT_DEPTH_16_565
#define IG_GTB_DEPTH_16_1555_I915_0x02000000    0x02000000u  // :277 BLT_DEPTH_16_1555
#define IG_GTB_DEPTH_32_I915_0x03000000         0x03000000u  // :278 BLT_DEPTH_32

#define IG_GTB_WRITE_ALPHA_I915_0x00200000      0x00200000u  // :272 BLT_WRITE_A
#define IG_GTB_WRITE_RGB_I915_0x00100000        0x00100000u  // :273 BLT_WRITE_RGB
// :274 BLT_WRITE_RGBA is (BLT_WRITE_RGB | BLT_WRITE_A) upstream. Composed here
// from the two siblings rather than spelled as a third literal, so a corrected
// bit in either half cannot leave this one agreeing with its old self.
#define IG_GTB_WRITE_RGBA_POLICY_COMPOSED \
    (IG_GTB_WRITE_RGB_I915_0x00100000 | IG_GTB_WRITE_ALPHA_I915_0x00200000)

#define IG_GTB_ROP_SRC_COPY_I915_0x00CC0000     0x00CC0000u  // :280 BLT_ROP_SRC_COPY
#define IG_GTB_ROP_COLOR_COPY_I915_0x00F00000   0x00F00000u  // :281 BLT_ROP_COLOR_COPY

// ---------------------------------------------------------------------------
// 3. Packet geometry.
//
// POLICY, every one of them, and the names say so. Upstream encodes the packet
// length in the low bits of the header word rather than as a named constant --
// COLOR_BLT_CMD carries (5 - 2) inline at :239 and XY_COLOR_BLT_CMD carries
// nothing at all, leaving the caller to OR a length in.
//
// So the DWORD COUNTS below are this project reading the packet layouts and
// choosing numbers. They are the likeliest thing in this file to be wrong, and
// a wrong length is not a decode error: the engine reads the next packet's
// first dword as this packet's last operand and walks the whole ring out of
// step, which is the same failure the LRI pair-count comment in
// IntelGtImage.hpp describes one layer down.
//
// The convention upstream uses -- and it is visible in that (5 - 2) -- is that
// the length field holds (dwords - 2): the header dword and one more are not
// counted. IG_GTB_LENGTH_BIAS_POLICY_2 is that subtrahend, spelled once.
// ---------------------------------------------------------------------------

#define IG_GTB_LENGTH_BIAS_POLICY_2             2u

// XY_COLOR_BLT: header, BR13 (rop/depth/stride), BR22 (dst top-left),
// BR23 (dst bottom-right), BR09 (dst address low), BR09 high, BR16 (colour).
#define IG_GTB_XY_COLOR_BLT_DWORDS_POLICY_7     7u

// XY_SRC_COPY_BLT: the fill's seven, minus the colour dword, plus src
// top-left, src pitch, and a 64-bit src address.
#define IG_GTB_XY_SRC_COPY_BLT_DWORDS_POLICY_10 10u

// A pitch is a SIGNED 16-bit field in BR13, so the representable ceiling is
// 32767 bytes -- not 65535. A 4K 32-bpp scanline is 15360 bytes and fits; an
// 8K one is 30720 and still fits; anything above this is refused rather than
// truncated, because a truncated pitch composes cleanly and blits a sheared
// picture.
#define IG_GTB_MAX_PITCH_BYTES_POLICY_32767     32767u

// Rectangle coordinates are 16-bit unsigned in BR22/BR23.
#define IG_GTB_MAX_COORD_POLICY_65535           65535u

// ---------------------------------------------------------------------------
// 4. Refusal verdicts.
//
// The prefix is IG_GTB_BLIT_ rather than a bare IG_GTB_, for the reason
// handoff16 section 1 records and IntelGtImage.hpp repeats:
// intel_verdict_set_checks() in tools/check-registers.py groups families BY
// PREFIX, so a bare IG_GTB_ would sweep in every opcode and depth constant
// above and print a contiguity failure that is correct about its own
// arithmetic and wrong about the world.
// ---------------------------------------------------------------------------

#define IG_GTB_BLIT_NOT_ATTEMPTED       0u
#define IG_GTB_BLIT_OK                  1u
#define IG_GTB_BLIT_NULL_SPAN           2u
#define IG_GTB_BLIT_SPAN_TOO_SHORT      3u
#define IG_GTB_BLIT_EMPTY_RECT          4u
#define IG_GTB_BLIT_RECT_INVERTED       5u
#define IG_GTB_BLIT_COORD_TOO_LARGE     6u
#define IG_GTB_BLIT_PITCH_ZERO          7u
#define IG_GTB_BLIT_PITCH_TOO_LARGE     8u
#define IG_GTB_BLIT_ADDR_UNALIGNED      9u
#define IG_GTB_BLIT_LIMIT               10u

static inline bool igGtbBlitSucceeded(IGU32 verdict)
{
    return verdict == IG_GTB_BLIT_OK;
}

static inline const char *igGtbBlitVerdictName(IGU32 verdict)
{
    switch (verdict) {
    case IG_GTB_BLIT_NOT_ATTEMPTED:   return "not attempted";
    case IG_GTB_BLIT_OK:              return "ok";
    case IG_GTB_BLIT_NULL_SPAN:       return "null span";
    case IG_GTB_BLIT_SPAN_TOO_SHORT:  return "span shorter than the packet";
    case IG_GTB_BLIT_EMPTY_RECT:      return "rectangle has no area";
    case IG_GTB_BLIT_RECT_INVERTED:   return "rectangle bottom-right precedes top-left";
    case IG_GTB_BLIT_COORD_TOO_LARGE: return "coordinate exceeds the 16-bit field";
    case IG_GTB_BLIT_PITCH_ZERO:      return "pitch is zero";
    case IG_GTB_BLIT_PITCH_TOO_LARGE: return "pitch exceeds the signed 16-bit field";
    case IG_GTB_BLIT_ADDR_UNALIGNED:  return "surface address is not dword aligned";
    default:                          return "unknown blit verdict";
    }
}

// ---------------------------------------------------------------------------
// 5. Header word composition.
//
// Its own function rather than igGtMiHeader(), for the reason in the file
// header: the BLT opcode field is seven bits at 22 and the MI one is six bits
// at 23. Both compose a plausible word from the same inputs.
//
// The masks are applied rather than trusted. An opcode above 0x7F would
// otherwise overflow into the client field and compose a command belonging to
// a different engine entirely.
// ---------------------------------------------------------------------------

static inline IGU32 igGtbHeader(IGU32 opcode, IGU32 lengthField, IGU32 flags)
{
    return (IG_GTB_CLIENT_I915_2 << IG_GTB_CLIENT_SHIFT_POLICY_29)
         | ((opcode & IG_GTB_OPCODE_MASK_POLICY_0x7F)
                << IG_GTB_OPCODE_SHIFT_POLICY_22)
         | flags
         | (lengthField & IG_GTB_LENGTH_MASK_POLICY_0xFF);
}

// The biased length upstream's (5 - 2) spells inline. Returns 0 for a packet
// shorter than the bias rather than underflowing to 0xFFFFFFFE -- an unsigned
// underflow here composes a header whose length field is 0xFE.
static inline IGU32 igGtbLengthField(IGU32 packetDwords)
{
    if (packetDwords < IG_GTB_LENGTH_BIAS_POLICY_2) { return 0u; }
    return packetDwords - IG_GTB_LENGTH_BIAS_POLICY_2;
}

// ---------------------------------------------------------------------------
// 6. Rectangle and surface admissibility.
//
// Split out from the writers so both packets judge their geometry through ONE
// function. Two copies of this test would agree with each other until one moved.
//
// ORDER is load-bearing, same discipline as igGtiWriteRingImage():
//
//   INVERTED before EMPTY. A rectangle whose right edge precedes its left is a
//   caller who swapped two arguments; reporting "no area" sends them to look at
//   a size that was never the problem. Both are true of an inverted rectangle
//   and only one of them is actionable.
//
//   COORD_TOO_LARGE last of the three, because it is a fact about the FIELD
//   rather than about the rectangle, and a rectangle that is also inverted is
//   the more urgent complaint.
// ---------------------------------------------------------------------------

static inline IGU32 igGtbRectVerdict(IGU32 left, IGU32 top,
                                     IGU32 right, IGU32 bottom)
{
    if (right < left || bottom < top) { return IG_GTB_BLIT_RECT_INVERTED; }
    if (right == left || bottom == top) { return IG_GTB_BLIT_EMPTY_RECT; }
    if (right > IG_GTB_MAX_COORD_POLICY_65535 ||
        bottom > IG_GTB_MAX_COORD_POLICY_65535) {
        return IG_GTB_BLIT_COORD_TOO_LARGE;
    }
    return IG_GTB_BLIT_OK;
}

// ZERO is refused rather than treated as "unset". Zero is the caller's
// initialiser, and a zero pitch blits every row of the rectangle over the top
// of the first one -- which completes without faulting.
static inline IGU32 igGtbPitchVerdict(IGU32 pitchBytes)
{
    if (pitchBytes == 0u) { return IG_GTB_BLIT_PITCH_ZERO; }
    if (pitchBytes > IG_GTB_MAX_PITCH_BYTES_POLICY_32767) {
        return IG_GTB_BLIT_PITCH_TOO_LARGE;
    }
    return IG_GTB_BLIT_OK;
}

// A surface base is a GGTT address and the blitter reads it as dword-aligned.
// An unaligned base is not rounded here: rounding it would blit correctly
// formed pixels to an address the caller did not name.
static inline IGU32 igGtbSurfaceVerdict(IGU64 address, IGU32 pitchBytes)
{
    const IGU32 pitch = igGtbPitchVerdict(pitchBytes);
    if (pitch != IG_GTB_BLIT_OK) { return pitch; }
    if ((address & 0x3ull) != 0ull) { return IG_GTB_BLIT_ADDR_UNALIGNED; }
    return IG_GTB_BLIT_OK;
}

// ---------------------------------------------------------------------------
// 7. XY_COLOR_BLT -- fill a rectangle with one colour.
//
// The two rules IntelGtImage.hpp states apply verbatim, and for the same
// reasons:
//
//   1. NOTHING IS WRITTEN UNTIL THE WHOLE PACKET IS ADMISSIBLE. A refusal that
//      has already written four dwords leaves a partial command in the ring,
//      and a partial command is not ignored -- it is executed as whatever it
//      happens to encode.
//
//   2. THE OUT-PARAM COUNT IS WRITTEN ON EVERY PATH. An untouched out-param
//      carries the caller's initialiser, so a caller who pre-loaded it with the
//      count they expected cannot tell a refusal from a success.
// ---------------------------------------------------------------------------

static inline IGU32 igGtbWriteColorBlit(IGU32 *dwords,
                                        IGU32 dwordCount,
                                        IGU64 dstAddress,
                                        IGU32 dstPitchBytes,
                                        IGU32 left, IGU32 top,
                                        IGU32 right, IGU32 bottom,
                                        IGU32 color,
                                        IGU32 depthFlag,
                                        IGU32 *outDwordsWritten)
{
    if (outDwordsWritten != 0) { *outDwordsWritten = 0u; }

    if (dwords == 0) { return IG_GTB_BLIT_NULL_SPAN; }
    if (dwordCount < IG_GTB_XY_COLOR_BLT_DWORDS_POLICY_7) {
        return IG_GTB_BLIT_SPAN_TOO_SHORT;
    }

    const IGU32 surface = igGtbSurfaceVerdict(dstAddress, dstPitchBytes);
    if (surface != IG_GTB_BLIT_OK) { return surface; }

    const IGU32 rect = igGtbRectVerdict(left, top, right, bottom);
    if (rect != IG_GTB_BLIT_OK) { return rect; }

    // BR13: raster op, colour depth, and the destination pitch in the low 16
    // bits. COLOR_COPY rather than SRC_COPY -- a fill has no source, and
    // SRC_COPY here would raster against a surface the packet never names.
    dwords[0] = igGtbHeader(IG_GTB_OPCODE_XY_COLOR_BLT_I915_0x50,
                            igGtbLengthField(IG_GTB_XY_COLOR_BLT_DWORDS_POLICY_7),
                            IG_GTB_WRITE_RGBA_POLICY_COMPOSED);
    dwords[1] = IG_GTB_ROP_COLOR_COPY_I915_0x00F00000
              | depthFlag
              | (dstPitchBytes & 0xFFFFu);

    // BR22/BR23: top-left and bottom-right, each packed Y in the high half and
    // X in the low half. The Y-major packing is not a choice -- it is what the
    // XY_ in the opcode name means, and reversing it composes a valid packet
    // that fills a transposed rectangle.
    dwords[2] = (top << 16) | (left & 0xFFFFu);
    dwords[3] = (bottom << 16) | (right & 0xFFFFu);

    dwords[4] = (IGU32)(dstAddress & 0xFFFFFFFFull);
    dwords[5] = (IGU32)((dstAddress >> 32) & 0xFFFFFFFFull);
    dwords[6] = color;

    if (outDwordsWritten != 0) {
        *outDwordsWritten = IG_GTB_XY_COLOR_BLT_DWORDS_POLICY_7;
    }
    return IG_GTB_BLIT_OK;
}

// ---------------------------------------------------------------------------
// 8. XY_SRC_COPY_BLT -- copy a rectangle between two surfaces.
//
// BOTH surfaces are judged, and the source is judged with the same function as
// the destination. A source pitch of zero reads one scanline repeatedly and
// produces a smeared but complete-looking copy, which is the quiet failure
// direction this whole family of guards exists for.
// ---------------------------------------------------------------------------

static inline IGU32 igGtbWriteCopyBlit(IGU32 *dwords,
                                       IGU32 dwordCount,
                                       IGU64 dstAddress,
                                       IGU32 dstPitchBytes,
                                       IGU64 srcAddress,
                                       IGU32 srcPitchBytes,
                                       IGU32 left, IGU32 top,
                                       IGU32 right, IGU32 bottom,
                                       IGU32 srcLeft, IGU32 srcTop,
                                       IGU32 depthFlag,
                                       IGU32 *outDwordsWritten)
{
    if (outDwordsWritten != 0) { *outDwordsWritten = 0u; }

    if (dwords == 0) { return IG_GTB_BLIT_NULL_SPAN; }
    if (dwordCount < IG_GTB_XY_SRC_COPY_BLT_DWORDS_POLICY_10) {
        return IG_GTB_BLIT_SPAN_TOO_SHORT;
    }

    // Destination first, because a caller debugging a copy looks at where it
    // landed before where it came from.
    const IGU32 dstSurface = igGtbSurfaceVerdict(dstAddress, dstPitchBytes);
    if (dstSurface != IG_GTB_BLIT_OK) { return dstSurface; }

    const IGU32 srcSurface = igGtbSurfaceVerdict(srcAddress, srcPitchBytes);
    if (srcSurface != IG_GTB_BLIT_OK) { return srcSurface; }

    const IGU32 rect = igGtbRectVerdict(left, top, right, bottom);
    if (rect != IG_GTB_BLIT_OK) { return rect; }

    // The source origin is a point rather than a rectangle -- the copy's extent
    // comes from the destination rectangle -- so it is bounded against the
    // coordinate field directly rather than through igGtbRectVerdict().
    if (srcLeft > IG_GTB_MAX_COORD_POLICY_65535 ||
        srcTop > IG_GTB_MAX_COORD_POLICY_65535) {
        return IG_GTB_BLIT_COORD_TOO_LARGE;
    }

    dwords[0] = igGtbHeader(IG_GTB_OPCODE_XY_SRC_COPY_BLT_I915_0x53,
                            igGtbLengthField(IG_GTB_XY_SRC_COPY_BLT_DWORDS_POLICY_10),
                            IG_GTB_WRITE_RGBA_POLICY_COMPOSED);
    dwords[1] = IG_GTB_ROP_SRC_COPY_I915_0x00CC0000
              | depthFlag
              | (dstPitchBytes & 0xFFFFu);

    dwords[2] = (top << 16) | (left & 0xFFFFu);
    dwords[3] = (bottom << 16) | (right & 0xFFFFu);

    dwords[4] = (IGU32)(dstAddress & 0xFFFFFFFFull);
    dwords[5] = (IGU32)((dstAddress >> 32) & 0xFFFFFFFFull);

    dwords[6] = (srcTop << 16) | (srcLeft & 0xFFFFu);
    dwords[7] = srcPitchBytes & 0xFFFFu;

    dwords[8] = (IGU32)(srcAddress & 0xFFFFFFFFull);
    dwords[9] = (IGU32)((srcAddress >> 32) & 0xFFFFFFFFull);

    if (outDwordsWritten != 0) {
        *outDwordsWritten = IG_GTB_XY_SRC_COPY_BLT_DWORDS_POLICY_10;
    }
    return IG_GTB_BLIT_OK;
}

#endif // IntelGtBlt_hpp
