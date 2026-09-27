//
//  IntelFbPresentProbe.hpp
//  Gen12 (Tiger Lake) INHERITED framebuffer, presented to IOKit.
//
//  Scope: an IOFramebuffer subclass that adopts a scanout the firmware already
//  programmed and describes it to IOKit. It writes nothing to the GPU -- not one
//  register -- for the same reason IntelDisplayProbe does not: this driver has
//  never run, and a write path that has never executed on silicon is a way to
//  brick a display, not a feature.
//
//  WHAT THIS CLASS IS AND IS NOT
//  -----------------------------
//  IS:     a read-only presenter. It reads four plane words once, asks
//          IntelFbPresent.hpp whether that group can be presented, and answers
//          IOKit's accessors out of the verdict.
//  IS NOT: a mode setter. setDisplayMode() accepts exactly the one mode it found
//          and refuses every other. enableController() does not enable anything;
//          it reports whether the inherited plane was presentable.
//
//  THE SPLIT, RESTATED FOR THIS FILE
//  ---------------------------------
//  Every number this class hands IOKit comes from IntelFbPresent.hpp, which is
//  dependency-free and driven by tests/test_intel_fb_present.cpp under MSVC. This
//  file contains no shift, no mask, and no arithmetic over a register value. That
//  is not tidiness: no host suite can compile this translation unit, because
//  IOFramebuffer's declarations do not exist off macOS, so anything computed HERE
//  is computed where nothing can execute it. IntelDisplayNames.hpp is what that
//  costs when it is ignored -- four verdict-name switches sat in a shell for weeks
//  with a missing arm publishing the unknown string, green through all four gates.
//
//  WHY THE VERDICT IS CACHED RATHER THAN RE-READ
//  ---------------------------------------------
//  fPresentVerdict, fCtl, fStride, fSize and fSurf are captured ONCE in start()
//  and never re-read. IOKit calls the accessors below repeatedly and in an order
//  this class does not control, and handoff28 SS4d is the reason that matters: a
//  register read on a running display is a SAMPLE, not a constant -- two reads of
//  the same register inside one run of tools/igpu-final-capture.sh disagreed. If
//  getPixelInformation() re-read PLANE_CTL it could return a different answer than
//  getInformationForDisplayMode() did a moment earlier, and IOKit would be holding
//  two descriptions of one surface with no way to tell which it acted on.
//
//  So the group is sampled once, judged once, and every accessor answers out of
//  that one sample. A stale answer is a known quantity; two disagreeing answers
//  are not.
//
//  WHY EVERY ACCESSOR CONSULTS THE VERDICT ANYWAY
//  ---------------------------------------------
//  fPresentVerdict is checked in every accessor even though start() refuses to
//  attach when it is not IG_PRESENT_OK. That looks redundant and is not: this
//  tree's fabricated-publication class has occurred five times, in two tracks,
//  and EVERY occurrence was a caller that had a refusal available and did not
//  consult it. An accessor that trusts start() to have refused is exactly that
//  shape -- correct today, and silently wrong the moment somebody adds a path
//  that attaches on a partial verdict.
//
//  WHAT THIS DOES NOT ESTABLISH
//  ----------------------------
//  It has never been compiled by Apple's clang, never loaded, and never matched.
//  PLAN.md termination condition A holds -- the laptop is muxless, so all
//  graphics route through the Intel chip and a picture on the internal panel is
//  not what this buys. What it buys is that the decode layer finally has a
//  consumer with the right shape.
//

#ifndef INTEL_FB_PRESENT_PROBE_HPP
#define INTEL_FB_PRESENT_PROBE_HPP

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>

#include "IntelDisplay.hpp"
#include "IntelFramebuffer.hpp"
#include "IntelScanout.hpp"
#include "IntelPixel.hpp"
#include "IntelFbPresent.hpp"
#include "IntelGpuId.hpp"
#include "IntelDisplayNames.hpp"

//
//  The one display mode this class offers.
//
//  IOKit requires at least one IODisplayModeID and requires it to be non-zero --
//  zero is kIODisplayModeIDInvalid in Apple's header. The value itself is
//  arbitrary and this class never interprets it; what matters is that
//  setDisplayMode() compares against it rather than accepting anything.
//
#define IG_PRESENT_MODE_ID          1

//
//  The depth index this class offers, and the count.
//
//  ONE depth, because the plane has one format and this driver does not reformat
//  it. A driver offering two depths over a surface it cannot reprogram would be
//  describing a capability it does not have.
//
#define IG_PRESENT_DEPTH_INDEX      0
#define IG_PRESENT_DEPTH_COUNT      1
#define IG_PRESENT_MODE_COUNT       1

//
//  The pipe and plane this class presents.
//
//  Pipe A plane 1, which is the only pipe the 2026-08-16 capture found programmed
//  and the only one an eDP-only machine can have programmed. handoff15 SS2 is why
//  this is a CONSTANT rather than a scan: every unused display page on this
//  silicon reads 0x00000000, so a scan would find three more pipes decoding to a
//  1x1 plane and igFbInheritable() would refuse each of them as GEOMETRY -- which
//  is correct, and which produces three refusals per boot that mean nothing.
//
//  IntelDisplayProbe DOES scan, because its job is to report what is there. This
//  class's job is to present ONE surface, so it names the one.
//
#define IG_PRESENT_PIPE             0u
#define IG_PRESENT_PLANE            0u

//
//  The refresh rate reported to IOKit, in 16.16 fixed point.
//
//  60 << 16. THIS IS NOT MEASURED, and the name says so. Deriving a real refresh
//  rate needs the transcoder timings and the link clock, and the link clock needs
//  the PLL block at 0x164000 that the final capture dumped but nothing in this
//  tree decodes yet. A value invented to fill a field, filed next to values
//  witnessed on hardware, destroys the only evidence trail this project has --
//  so this one carries UNMEASURED in its name and the field is documented as a
//  placeholder rather than a measurement.
//
#define IG_PRESENT_REFRESH_UNMEASURED   0x003C0000u

//
//  The two BAR indices this class uses, and they are DIFFERENT BARs.
//
//  BAR0 is the MMIO register window the four plane words are read out of --
//  spelled here rather than taken from IntelDisplayProbe.hpp for that file's own
//  stated reason: a BAR index shared between classes is exactly the kind of
//  constant that reads as correct while naming the wrong aperture.
//
//  BAR2 is the graphics-memory aperture the scanout surface itself lives in, and
//  it is what getApertureRange() hands IOKit. Its LENGTH is the one input
//  igPresentVerdict() cannot derive from the plane words, which is why this class
//  needs it at all -- it never maps it.
//
#define IG_PRESENT_BAR0_INDEX       0
#define IG_PRESENT_APERTURE_INDEX   2

//
//  Same warning threshold IntelDisplayProbe.hpp carries, and a warning for the
//  same reason: declining on a short BAR would delete the IORegistry node that
//  makes a short BAR inspectable. regRead32()'s IG_READ_PAST_MAPPING arm is where
//  the warning acquires a consequence.
//
#define IG_PRESENT_BAR0_EXPECTED    0x00080000u

class IntelFbPresentProbe : public IOFramebuffer
{
    OSDeclareDefaultStructors(IntelFbPresentProbe)

public:
    virtual bool       init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void       free(void) APPLE_KEXT_OVERRIDE;
    virtual bool       start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void       stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    //
    //  The eight pure virtuals IOFramebuffer demands. Every one of them consults
    //  fPresentVerdict before answering; see the header comment for why that is
    //  not redundant with start()'s refusal.
    //
    virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE;
    virtual const char     *getPixelFormats(void) APPLE_KEXT_OVERRIDE;
    virtual IOItemCount     getDisplayModeCount(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn        getDisplayModes(IODisplayModeID *allDisplayModes) APPLE_KEXT_OVERRIDE;
    virtual IOReturn        getInformationForDisplayMode(IODisplayModeID displayMode,
                                                         IODisplayModeInformation *info) APPLE_KEXT_OVERRIDE;
    virtual UInt64          getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                                          IOIndex depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn        getPixelInformation(IODisplayModeID displayMode,
                                                IOIndex depth,
                                                IOPixelAperture aperture,
                                                IOPixelInformation *pixelInfo) APPLE_KEXT_OVERRIDE;
    virtual IOReturn        getCurrentDisplayMode(IODisplayModeID *displayMode,
                                                  IOIndex *depth) APPLE_KEXT_OVERRIDE;

    virtual IOReturn        setDisplayMode(IODisplayModeID displayMode,
                                           IOIndex depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn        enableController(void) APPLE_KEXT_OVERRIDE;
    virtual bool            isConsoleDevice(void) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice        *fPCIDevice;
    IOMemoryMap        *fBar0Map;
    volatile UInt8     *fBar0Base;

    //
    //  The mapped length, as an IGU64.
    //
    //  Same reasoning as IntelDisplayProbe's own fBar0Length and it is not
    //  optional here: IOByteCount is 64 bits, and narrowing it to 32 is
    //  ASYMMETRIC. 0x1_0000_0000 truncates to 0, which refuses everything and is
    //  loudly wrong; 0x1_0007_0000 truncates to 0x70000, which ACCEPTS reads it
    //  should refuse. The second direction scribbles.
    //
    IGU64               fBar0Length;

    //
    //  The aperture length in bytes, kept separately from fBar0Length.
    //
    //  These are DIFFERENT NUMBERS and conflating them is the defect this field
    //  exists to prevent. fBar0Length is how much MMIO register space the OS
    //  mapped -- BAR0. The aperture is the graphics memory the scanout surface
    //  lives in, reached through a different BAR entirely. igPresentVerdict()
    //  takes the APERTURE size, because what it checks is whether the surface's
    //  byte extent fits the memory holding it; handing it a register-window
    //  length would compare a framebuffer extent against the size of a register
    //  file.
    //
    IGPR64              fApertureBytes;

    //
    //  The aperture range handed to IOKit, retained for the lifetime of the
    //  attachment because getApertureRange() may be called more than once and
    //  Apple's contract does not say the caller releases it.
    //
    IODeviceMemory     *fApertureRange;

    UInt32              fDeviceId;

    //
    //  THE ONE SAMPLE. Read once in start(), never re-read.
    //
    UInt32              fCtl;
    UInt32              fStride;
    UInt32              fSize;
    UInt32              fSurf;

    //
    //  The verdict over that sample, and the flag saying the sample HAPPENED.
    //
    //  Two fields rather than one, because a verdict field alone cannot separate
    //  "the group was read and refused" from "the group was never read" --
    //  init() zeroes fPresentVerdict and zero IS IG_PRESENT_OK. An initialiser
    //  zero that reads as the success code is the exact shape that put three
    //  fabricated register words into the NVIDIA track's IORegistry, so
    //  fSampleTaken is initialised false and set true only after the four reads
    //  return.
    //
    UInt32              fPresentVerdict;
    bool                fSampleTaken;

    bool     mapBar0(void);
    void     unmapBar0(void);
    bool     describeAperture(void);
    UInt32   regRead32(UInt32 offset);

    bool     samplePlane(void);
    void     publishPresentation(void);

    //
    //  The single guard every accessor runs first. True only when the sample was
    //  taken AND the verdict says the group is presentable.
    //
    bool     presentable(void) const;
};

#endif // INTEL_FB_PRESENT_PROBE_HPP
