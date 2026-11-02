//
//  IntelPlaneProgram.hpp
//  Step (b) of the sink plan: turn a validated sink surface into the plane
//  register values that make the Intel display engine scan it out.
//
//  This is the inverse of the decode helpers in IntelFramebuffer.hpp.
//  Those read what firmware left in the plane; these encode what we want
//  the plane to show. Nothing in here touches MIO. The kext takes an
//  IgPlaneRegs and writes it in igPlaneWriteOffset() order, so the whole encode
//  path is testable on the host.
//  Why this matters for the 3050: on this laptop the panel hangs off the
//  Intel display engine. Whatever the dGPU renders has to land in a
//  GGTT-mapped linear sink and be flipped onto an Intel plane. This header
//  is that flip.
//

#ifndef INTEL_PLANE_PROGRAM_HPP
#define INTEL_PLANE_PROGRAM_HPP

#include "IntelFramebuffer.hpp"

//  ---- stride units ------------------------------
//  The PLANE_STRIDE field counts in units that depend on tiling.
//  IG_FB_STRIDE_UNIT_DERIVED_2026_08_16 (128) was derived from the firmware
//  plane, which is Y-tiled. Our sinks are LINEAR, and a linear plane counts
//  in 64-byte units (i915 skl_plane_stride_mult: DRM_FORMAT_MOD_LINEAR ->
//  64). Reusing 128 for a linear sink would program half the real stride.
//  DOCUMENTED, not yet measured on this hardware.
//
#define IG_PLANE_STRIDE_UNIT_LINEAR_DOCUMENTED  64u

//  Surface base must be page aligned: PLANE_SURF drops the low 12 bits.
#define IG_PLANE_SURF_ALIGN                0x1000u

//  CTL bits we own. Everything else is carried over from the base CTL
//  (usually the firmware value), so pipe-gamma, CSC and friends stay as
//  firmware configured them.
#define IG_PLANE_CTL_OWNED_MASK                                \
    (IG_FB_CTL_ENABLE                                        \
     | (IG_FB_CTL_FORMAT_MASK << IG_FB_CTL_FORMAT_SHIFT)                     \
     | (IG_FB_CTL_TILING_MASK << IG_FB_CTL_TILING_SHIFT)                \
     | IG_FB_CTL_RENDER_DECOMP                                               \
     | IG_FB_CTL_ASYNC_FLIP)

//  ---- verdicts ------------------------------------------------
#define IG_PLANE_OK                     0u
#define IG_PLANE_BAD_FORMAT             1u
#define IG_PLANE_BAD_GEOMETRY           2u
#define IG_PLANE_BAD_STRIDE             3u
#define IG_PLANE_BAD_SURF_ALIGN         4u
#define IG_PLANE_BAD_SURF_RANGE         5u
#define IG_PLANE_NULL_OUT               6u
#define IG_PLANE_VERDICT_LIMIT          6u

struct IgPlaneRegs {
    IGFB32 ctl;
    IGFB32 stride;
    IGFB32 pos;
    IGFB32 size;
    IGFB32 surf;
};

//  Write order. PLANE_SURF goes last: the write to SURF arms the update, and
//  the other registers latch with it at the next vblank. Writing SURF first
//  would scan out one frame with the new base but the old geometry.
//
//  A function, not a static const array: every translation unit that
//  includes this header without walking the order would otherwise carry
//  an unused const table, which -Wunused-const-variable turns into a hard
//  error under the kext's -Werror (check-kext-syntax.bat section 1 caught
//  exactly that). A step past the sequence returns IG_FB_INVALID rather
//  than a real offset, so an off-by-one walk cannot write a register.
#define IG_PLANE_WRITE_STEPS 5u

static inline IGFB32 igPlaneWriteOffset(IGFB32 step)
{
    switch (step) {
    case 0u: return IG_FB_OFF_CTL;
    case 1u: return IG_FB_OFF_STRIDE;
    case 2u: return IG_FB_OFF_POS;
    case 3u: return IG_FB_OFF_SIZE;
    case 4u: return IG_FB_OFF_SURF;
    default: return IG_FB_INVALID;
    }
}

//  Returns the PLANE_STRIDE field for a linear stride in bytes, or
//  IG_FB_INVALID if the stride is zero, not a whole unit, or too wide for
//  the field.
static inline IGFB32 igPlaneEncodeLinearStride(IGFB32 strideBytes)
{
    if (strideBytes == 0u)
        return IG_FB_INVALID;
    if ((strideBytes % IG_PLANE_STRIDE_UNIT_LINEAR_DOCUMENTED) != 0u)
        return IG_FB_INVALID;
    IGFB32 units = strideBytes / IG_PLANE_STRIDE_UNIT_LINEAR_DOCUMENTED;
    if (units > IG_FB_STRIDE_MASK)
        return IG_FB_INVALID;
    return units;
}

//  PLANE_SIZE stores (height-1)<<16 | (width-1). This is the inverse of
//  igFbSizeWidth/igFbSizeHeight.
static inline IGFB32 igPlaneEncodeSize(IGFB32 width, IGFB32 height)
{
    if (width == 0u || height == 0u)
        return IG_FB_INVALID;
    if (width - 1u > IG_FB_SIZE_HALF_MASK || height - 1u > IG_FB_SIZE_HALF_MASK)
        return IG_FB_INVALID;
    return ((width - 1u) << IG_FB_SIZE_WIDTH_SHIFT)
           | ((height - 1u) << IG_FB_SIZE_HEIGHT_SHIFT);
}

static inline IGFB32 igPlaneEncodePos(IGFB32 x, IGFB32 y)
{
    return ((x & IG_FB_POS_HALF_MASK) << IG_FB_POS_X_SHIFT)
           | ((y & IG_FB_POS_HALF_MASK) << IG_FB_POS_Y_SHIFT);
}

//  Build the registers for a linear, uncompressed, sync-flipped plane that
//  shows `surf` (a GGTT offset) at the top-left of the pipe.
//  `baseCtl` supplies the non-owned CTL bits. On failure `out` is left
//  untouched.
static inline IGFB32 igPlaneBuildLinear(IGFB32 baseCtl, IGFB32 format,
                                        IGFB32 surf, IGFB32 width,
                                IGFB32 height, IGFB32 strideBytes,
                                IgPlaneRegs *out)
{
    if (out == 0)
        return IG_PLANE_NULL_OUT;

    IGFB32 bpp = igFbBytesPerPixel(format);
    if (bpp == 0u)
        return IG_PLANE_BAD_FORMAT;

    IGFB32 size = igPlaneEncodeSize(width, height);
    if (size == IG_FB_INVALID)
        return IG_PLANE_BAD_GEOMETRY;

    IGFB32 stride = igPlaneEncodeLinearStride(strideBytes);
    if (stride == IG_FB_INVALID)
        return IG_PLANE_BAD_STRIDE;
    //  A row must fit in the stride. width <= 65536 and bpp <= 8, so the
    //  product cannot overflow 32 bits.
    if (width * bpp > strideBytes)
        return IG_PLANE_BAD_STRIDE;

    if ((surf & (IG_PLANE_SURF_ALIGN - 1u)) != 0u)
        return IG_PLANE_BAD_SURF_ALIGN;

    //  strideBytes <= 0xFFF*64 and height <= 65536, so this fits in 32 bits
    //  only for modest heights. Reject rather than wrap.
    if (strideBytes > (IG_FB_INVALID / height))
        return IG_PLANE_BAD_SURF_RANGE;
    if (!igFbSurfaceRangeOk(surf, strideBytes * height))
        return IG_PLANE_BAD_SURF_RANGE;

    //  igFbFormatCtlBits() puts the 4-bit code at bit 24 with bit 23 clear.
    //  The old (format << IG_FB_CTL_FORMAT_SHIFT) wrote XRGB_8888 as
    //  XRGB_2101010 and RGB_565 as P016 (t-a7929c).
    out->ctl = (baseCtl & ~IG_PLANE_CTL_OWNED_MASK)
               | IG_FB_CTL_ENABLE
               | igFbFormatCtlBits(format)
               | (IG_FB_TILING_LINEAR << IG_FB_CTL_TILING_SHIFT);
    out->stride = stride;
    out->pos    = igPlaneEncodePos(0u, 0u);
    out->size   = size;
    out->surf   = surf & IG_FB_SURF_ADDR_MASK;
    return IG_PLANE_OK;
}

#endif  // INTEL_PLANE_PROGRAM_HPP