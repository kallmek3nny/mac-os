// SPDX-License-Identifier: MIT
// Rectangle bound check for the GT-1k inherited-scanout fill selector (t-619d61).
//
// Dependency-free on purpose: no IOKit, libkern or SDK include, so the check
// compiles and can be tested on any host. IntelGtBlitUserClient calls it with
// the plane width and height that the stage-4 sample accepted (IG_PRESENT_OK),
// before IntelGtBringup::fillScanoutRect32 runs. A rectangle outside the plane
// is refused, never clipped, as in IntelGtScanoutMap.hpp.
#ifndef IntelGtScanoutRect_hpp
#define IntelGtScanoutRect_hpp

// Verdicts. Only IG_SCANOUT_RECT_OK lets the fill go ahead.
#define IG_SCANOUT_RECT_OK        0u
#define IG_SCANOUT_RECT_EMPTY     1u  // w or h is zero
#define IG_SCANOUT_RECT_NO_PLANE  2u  // sampled width or height is zero
#define IG_SCANOUT_RECT_OVERFLOW  3u  // x + w or y + h would wrap 32 bits
#define IG_SCANOUT_RECT_OUTSIDE   4u  // x + w > width or y + h > height

// x + w must not exceed planeWidth and y + h must not exceed planeHeight.
// Each sum is tested for 32-bit wrap before it is formed, so no addition here
// can overflow.
inline unsigned int igScanoutRectVerdict(unsigned int planeWidth, unsigned int planeHeight,
                                         unsigned int x, unsigned int y,
                                         unsigned int w, unsigned int h)
{
    if (!w || !h) return IG_SCANOUT_RECT_EMPTY;
    if (!planeWidth || !planeHeight) return IG_SCANOUT_RECT_NO_PLANE;
    if (x > 0xFFFFFFFFu - w || y > 0xFFFFFFFFu - h) return IG_SCANOUT_RECT_OVERFLOW;
    if (x + w > planeWidth || y + h > planeHeight) return IG_SCANOUT_RECT_OUTSIDE;
    return IG_SCANOUT_RECT_OK;
}

inline bool igScanoutRectInside(unsigned int verdict)
{
    return verdict == IG_SCANOUT_RECT_OK;
}

#endif
