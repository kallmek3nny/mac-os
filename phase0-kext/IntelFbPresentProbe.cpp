//
//  IntelFbPresentProbe.cpp
//
//  The Gen12 inherited-framebuffer presenter. See IntelFbPresentProbe.hpp for
//  what this class is and is not; the short version is that it adopts a scanout
//  the firmware already programmed, describes it to IOKit, and writes nothing to
//  the GPU -- not one register.
//
//  Every number handed to IOKit below comes out of IntelFbPresent.hpp. There is
//  no shift, no mask, and no arithmetic over a register value anywhere in this
//  file, deliberately: this translation unit reaches IOFramebuffer, so no host
//  suite can compile it, so anything computed HERE is computed where nothing can
//  execute it. IntelDisplayNames.hpp is what that costs when it is ignored.
//
//  The read path is IntelDisplayProbe.cpp's, verbatim in shape: one regRead32()
//  funnel, igReadDecision() deciding, every refusal answering IG_MMIO_INVALID and
//  saying which guard fired. It is duplicated rather than shared because the two
//  classes are separate IOService subclasses with separate BAR0 mappings and
//  separate lifetimes; a shared accessor would need a shared object whose
//  mapping state neither class owns.
//

#include "IntelFbPresentProbe.hpp"

OSDefineMetaClassAndStructors(IntelFbPresentProbe, IOFramebuffer);

#define IGLOG(fmt, ...) IOLog("IntelFbPresentProbe: " fmt "\n", ## __VA_ARGS__)

#define super IOFramebuffer

//
// ---- lifecycle -------------------------------------------------------------
//

bool IntelFbPresentProbe::init(OSDictionary *dict)
{
    if (!super::init(dict))
        return false;

    fPCIDevice      = nullptr;
    fBar0Map        = nullptr;
    fBar0Base       = nullptr;
    fBar0Length     = 0;
    fApertureBytes  = 0;
    fApertureRange  = nullptr;
    fDeviceId       = 0;

    fCtl            = IG_PRESENT_INVALID32;
    fStride         = IG_PRESENT_INVALID32;
    fSize           = IG_PRESENT_INVALID32;
    fSurf           = IG_PRESENT_INVALID32;

    //
    // fPresentVerdict is zeroed here and zero IS IG_PRESENT_OK. That is why
    // fSampleTaken exists and why it is initialised false: a verdict field alone
    // cannot separate "the group was read and accepted" from "the group was never
    // read", and an initialiser zero that reads as the success code is exactly
    // the shape that put three fabricated register words into the NVIDIA track's
    // IORegistry.
    //
    // The four sample words above are initialised to the REFUSAL sentinel rather
    // than to zero for the same asymmetry, one level down: zero is a legal value
    // for every one of them (handoff15 SS2 -- every unprogrammed display page on
    // this silicon reads 0x00000000), so a zero here would be indistinguishable
    // from a register that genuinely read all-clear.
    //
    fPresentVerdict = IG_PRESENT_OK;
    fSampleTaken    = false;

    return true;
}

void IntelFbPresentProbe::free(void)
{
    OSSafeReleaseNULL(fApertureRange);
    unmapBar0();
    OSSafeReleaseNULL(fPCIDevice);

    super::free();
}

bool IntelFbPresentProbe::start(IOService *provider)
{
    if (!super::start(provider)) {
        IGLOG("start: super::start() declined");
        return false;
    }

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (fPCIDevice == nullptr) {
        IGLOG("start: provider is not an IOPCIDevice");
        super::stop(provider);
        return false;
    }
    fPCIDevice->retain();

    fDeviceId = fPCIDevice->configRead16(kIOPCIConfigDeviceID);
    IGLOG("start: device id 0x%04x, tiger lake %s",
          fDeviceId, igIsTglDevice(fDeviceId) ? "yes" : "no");

    if (!mapBar0())
        goto fail;

    if (!describeAperture())
        goto fail;

    //
    // THE ONE SAMPLE. Four reads, judged once, and never re-read for the
    // lifetime of the attachment -- see the header for why a re-read is worse
    // than a stale answer.
    //
    if (!samplePlane()) {
        IGLOG("start: refusing to attach -- the inherited plane is not "
              "presentable (%s)", igPresentVerdictName(fPresentVerdict));
        goto fail;
    }

    publishPresentation();

    IGLOG("start: complete -- presenting %ux%u, %u bpp, %u bytes per row",
          igPresentWidth(fCtl, fStride, fSize, fSurf, fApertureBytes),
          igPresentHeight(fCtl, fStride, fSize, fSurf, fApertureBytes),
          igPresentBitsPerPixel(fCtl, fStride, fSize, fSurf, fApertureBytes),
          igPresentBytesPerRow(fCtl, fStride, fSize, fSurf, fApertureBytes));
    return true;

fail:
    OSSafeReleaseNULL(fApertureRange);
    unmapBar0();
    OSSafeReleaseNULL(fPCIDevice);
    super::stop(provider);
    return false;
}

void IntelFbPresentProbe::stop(IOService *provider)
{
    IGLOG("stop");

    OSSafeReleaseNULL(fApertureRange);
    unmapBar0();
    OSSafeReleaseNULL(fPCIDevice);

    super::stop(provider);
}

//
// ---- the two mappings ------------------------------------------------------
//
// They are DIFFERENT BARs and the fields holding their lengths are deliberately
// separate. BAR0 is the MMIO register window this class reads four words out of;
// the aperture is the graphics memory the scanout surface itself lives in.
// igPresentVerdict() takes the APERTURE size, because what it checks is whether
// the surface's byte extent fits the memory holding it -- handing it a register
// window length would compare a framebuffer extent against the size of a
// register file, and the comparison would be arithmetically fine and mean
// nothing.
//

bool IntelFbPresentProbe::mapBar0(void)
{
    IODeviceMemory *bar =
        fPCIDevice->getDeviceMemoryWithIndex(IG_PRESENT_BAR0_INDEX);
    if (bar == nullptr) {
        IGLOG("mapBar0: no device memory at index %d", IG_PRESENT_BAR0_INDEX);
        return false;
    }

    fBar0Map = bar->map();
    if (fBar0Map == nullptr) {
        IGLOG("mapBar0: map() failed");
        return false;
    }

    fBar0Base = (volatile UInt8 *) fBar0Map->getVirtualAddress();
    if (fBar0Base == nullptr) {
        IGLOG("mapBar0: mapping has no virtual address");
        OSSafeReleaseNULL(fBar0Map);
        return false;
    }

    //
    // IOByteCount is 64 bits and fBar0Length is IGU64 for that reason. The
    // narrowing is ASYMMETRIC: 0x1_0000_0000 truncates to 0, which refuses
    // everything and is loudly wrong; 0x1_0007_0000 truncates to 0x70000, which
    // ACCEPTS reads it should refuse. The second direction scribbles.
    //
    IOByteCount length = fBar0Map->getLength();
    fBar0Length = (IGU64) length;

    IGLOG("mapBar0: %llu bytes at %p",
          (unsigned long long) fBar0Length, (void *) fBar0Base);

    //
    // A warning rather than a refusal, same call IntelDisplayProbe makes:
    // declining here would delete the IORegistry node that makes a short BAR
    // inspectable. regRead32()'s IG_READ_PAST_MAPPING arm is where the warning
    // acquires a consequence.
    //
    if (fBar0Length < IG_PRESENT_BAR0_EXPECTED) {
        IGLOG("mapBar0: WARNING BAR0 is %llu bytes, shorter than the 0x%x the "
              "display block needs -- reads past the end are undefined, not "
              "faults",
              (unsigned long long) fBar0Length, IG_PRESENT_BAR0_EXPECTED);
    }

    return true;
}

void IntelFbPresentProbe::unmapBar0(void)
{
    fBar0Base   = nullptr;
    fBar0Length = 0;
    OSSafeReleaseNULL(fBar0Map);
}

//
// The aperture is described, never mapped.
//
// getApertureRange() hands IOKit an IODeviceMemory and IOKit maps it itself when
// it wants the pixels; this class has no reason to hold a second virtual mapping
// of a surface it never touches. What it DOES need is the aperture's LENGTH,
// because that is the one input igPresentVerdict() cannot derive from the four
// plane words.
//
// The range is retained for the lifetime of the attachment because
// getApertureRange() may be called more than once and Apple's contract does not
// say the caller releases it.
//
bool IntelFbPresentProbe::describeAperture(void)
{
    IODeviceMemory *range =
        fPCIDevice->getDeviceMemoryWithIndex(IG_PRESENT_APERTURE_INDEX);

    if (range == nullptr) {
        IGLOG("describeAperture: no device memory at index %d -- refusing",
              IG_PRESENT_APERTURE_INDEX);
        return false;
    }

    range->retain();
    fApertureRange = range;

    IOByteCount bytes = range->getLength();
    fApertureBytes = (IGPR64) bytes;

    IGLOG("describeAperture: aperture is %llu bytes",
          (unsigned long long) fApertureBytes);

    //
    // A zero-length aperture is refused HERE rather than left for
    // igPresentVerdict() to catch, because the two refusals mean different
    // things: this one says the OS handed us a BAR with no extent, which is a
    // fact about the machine, while a SCANOUT refusal from the verdict says the
    // surface does not fit an aperture we DID measure. Collapsing them would
    // report a missing BAR as an oversized surface.
    //
    if (fApertureBytes == 0) {
        IGLOG("describeAperture: aperture has zero length -- refusing");
        OSSafeReleaseNULL(fApertureRange);
        return false;
    }

    return true;
}

//
// ---- the read funnel -------------------------------------------------------
//
// Identical in shape to IntelDisplayProbe::regRead32(). The guards live in
// igReadDecision() in IntelDisplay.hpp so a host suite can execute them; their
// ORDER is the part worth keeping honest, and tests/test_intel_display.cpp pins
// the precedence on inputs where two guards are true at once.
//
UInt32 IntelFbPresentProbe::regRead32(UInt32 offset)
{
    switch (igReadDecision(fBar0Base != nullptr, fBar0Length, offset)) {
    case IG_READ_UNMAPPED:
        IGLOG("regRead32(0x%x): BAR0 is not mapped", offset);
        return IG_MMIO_INVALID;

    case IG_READ_SENTINEL_OFFSET:
        IGLOG("regRead32: refusing IG_MMIO_INVALID -- an offset helper declined");
        return IG_MMIO_INVALID;

    case IG_READ_OUT_OF_POLICY:
        IGLOG("regRead32(0x%x): outside the display policy range "
              "0x%x..0x%x -- refusing",
              offset, IG_DISPLAY_POLICY_BASE, IG_DISPLAY_POLICY_LIMIT);
        return IG_MMIO_INVALID;

    case IG_READ_PAST_MAPPING:
        IGLOG("regRead32(0x%x): BAR0 is only %llu bytes -- a %u-byte read here "
              "would land past the end of the mapping, refusing",
              offset, (unsigned long long) fBar0Length, IG_MMIO_ACCESS_BYTES);
        return IG_MMIO_INVALID;

    default:
        break;
    }

    return *(volatile UInt32 *) (fBar0Base + offset);
}

//
// ---- the one sample --------------------------------------------------------
//

bool IntelFbPresentProbe::samplePlane(void)
{
    fCtl    = regRead32(igPlaneReg(IG_PRESENT_PIPE, IG_PRESENT_PLANE,
                                   IG_PLANE_CTL));
    fStride = regRead32(igPlaneReg(IG_PRESENT_PIPE, IG_PRESENT_PLANE,
                                   IG_PLANE_STRIDE));
    fSize   = regRead32(igPlaneReg(IG_PRESENT_PIPE, IG_PRESENT_PLANE,
                                   IG_PLANE_SIZE));
    fSurf   = regRead32(igPlaneReg(IG_PRESENT_PIPE, IG_PRESENT_PLANE,
                                   IG_PLANE_SURF));

    //
    // Set BEFORE the verdict is computed and AFTER all four reads have returned.
    //
    // Before the verdict because the flag's claim is "the four reads happened",
    // which is now true regardless of what they returned -- a refused read is
    // still a read that was attempted, and igPresentVerdict() is what judges the
    // values. After the reads because a flag set first would report a group as
    // sampled on the one path where nothing was.
    //
    fSampleTaken = true;

    fPresentVerdict = igPresentVerdict(fCtl, fStride, fSize, fSurf,
                                       fApertureBytes);

    IGLOG("samplePlane: ctl=0x%08x stride=0x%08x size=0x%08x surf=0x%08x -> %s",
          fCtl, fStride, fSize, fSurf,
          igPresentVerdictName(fPresentVerdict));

    return fPresentVerdict == IG_PRESENT_OK;
}

//
// The single guard every accessor runs first.
//
// It checks BOTH fields, and the redundancy with start()'s refusal is the point:
// this tree's fabricated-publication class has occurred five times, in two
// tracks, and every occurrence was a caller that had a refusal available and did
// not consult it. An accessor that trusts start() to have refused is exactly
// that shape -- correct today, silently wrong the moment somebody adds a path
// that attaches on a partial verdict.
//
// The equality against IG_PRESENT_OK is deliberate rather than a negation of any
// error arm: != IG_PRESENT_ERR_SAMPLE_REFUSED is true for a group refused as
// NOT_INHERITABLE, and would present a compressed surface as a plain one.
//
bool IntelFbPresentProbe::presentable(void) const
{
    return fSampleTaken && fPresentVerdict == IG_PRESENT_OK;
}

//
// ---- what we publish -------------------------------------------------------
//

void IntelFbPresentProbe::publishPresentation(void)
{
    //
    // The verdict and the sampled flag go FIRST and unconditionally, for the
    // reason handoff18 SS1d settled on for GSPEngineReadable: a flag that appears
    // only when something went wrong is indistinguishable from an older kext that
    // never published it. An absent key is a question, not an answer.
    //
    setProperty("IGPresentSampled", fSampleTaken);
    setProperty("IGPresentVerdict", igPresentVerdictName(fPresentVerdict));

    //
    // The four raw words, published because they are the evidence every number
    // below is derived from. An operator who disagrees with a decode needs the
    // input to it, not just the output.
    //
    setProperty("IGPresentPlaneCtl",    (UInt64) fCtl,    32);
    setProperty("IGPresentPlaneStride", (UInt64) fStride, 32);
    setProperty("IGPresentPlaneSize",   (UInt64) fSize,   32);
    setProperty("IGPresentPlaneSurf",   (UInt64) fSurf,   32);

    setProperty("IGPresentApertureBytes", (UInt64) fApertureBytes, 64);

    //
    // Everything below is a DECODE, and every one of them is behind the guard.
    //
    // Not because the accessors would fault -- they each re-derive the verdict
    // and answer the sentinel for a refused group, so they are safe to call. It
    // is because setProperty() with a sentinel is byte-identical to setProperty()
    // with a measurement, and publishing 0xFFFFFFFF as IGPresentWidth would put a
    // number in the IORegistry that looks like a width.
    //
    if (!presentable()) {
        IGLOG("publishPresentation: verdict is %s -- publishing the sample and "
              "no decode of it",
              igPresentVerdictName(fPresentVerdict));
        return;
    }

    setProperty("IGPresentWidth",
                (UInt64) igPresentWidth(fCtl, fStride, fSize, fSurf,
                                        fApertureBytes), 32);
    setProperty("IGPresentHeight",
                (UInt64) igPresentHeight(fCtl, fStride, fSize, fSurf,
                                         fApertureBytes), 32);
    setProperty("IGPresentBytesPerRow",
                (UInt64) igPresentBytesPerRow(fCtl, fStride, fSize, fSurf,
                                              fApertureBytes), 32);
    setProperty("IGPresentBitsPerPixel",
                (UInt64) igPresentBitsPerPixel(fCtl, fStride, fSize, fSurf,
                                               fApertureBytes), 32);
    setProperty("IGPresentComponentBits",
                (UInt64) igPresentComponentBits(fCtl, fStride, fSize, fSurf,
                                                fApertureBytes), 32);
    setProperty("IGPresentComponentCount",
                (UInt64) igPresentComponentCount(fCtl, fStride, fSize, fSurf,
                                                 fApertureBytes), 32);
    setProperty("IGPresentBaseOffset",
                (UInt64) igPresentBaseOffset(fCtl, fStride, fSize, fSurf,
                                             fApertureBytes), 64);
    setProperty("IGPresentExtentBytes",
                (UInt64) igPresentExtentBytes(fCtl, fStride, fSize, fSurf,
                                              fApertureBytes), 64);

    //
    // The refresh rate this class reports, published under a key that says what
    // it is. It is NOT measured -- see IG_PRESENT_REFRESH_UNMEASURED. Deriving a
    // real one needs the transcoder timings and the link clock, and the link
    // clock needs the PLL block at 0x164000 that nothing in this tree decodes.
    //
    setProperty("IGPresentRefreshIsMeasured", false);
}

//
// ---- IOFramebuffer ---------------------------------------------------------
//
// The eight pure virtuals, then the three concrete ones this class overrides.
// Every one of them consults presentable() first. That claim was FALSE between
// this file landing and 2026-08-20 -- getPixelFormatsForDisplayMode() had no
// guard while this line and its own comment both said it did. It is true as
// written now, and tests/test_intel_fb_present_shape.py is what keeps it true:
// the subject set is DERIVED from the APPLE_KEXT_OVERRIDE declarations in the
// header, so a ninth override added later reddens on the commit that adds it
// rather than joining a list nobody updated.
//

IODeviceMemory *IntelFbPresentProbe::getApertureRange(IOPixelAperture aperture)
{
    if (!presentable()) {
        IGLOG("getApertureRange: not presentable (%s) -- refusing",
              igPresentVerdictName(fPresentVerdict));
        return nullptr;
    }

    //
    // Exactly one aperture is described, so every other selector is refused
    // rather than answered with the system one. Answering the base range for an
    // unknown selector would hand IOKit a range that is correct for a different
    // question.
    //
    if (aperture != kIOFBSystemAperture) {
        IGLOG("getApertureRange: aperture %u is not kIOFBSystemAperture -- "
              "refusing", (unsigned) aperture);
        return nullptr;
    }

    if (fApertureRange == nullptr) {
        IGLOG("getApertureRange: no aperture range -- refusing");
        return nullptr;
    }

    //
    // Retained on the way out. Apple's callers release what they are handed, and
    // this class keeps its own reference for the lifetime of the attachment
    // because it may be asked again.
    //
    fApertureRange->retain();
    return fApertureRange;
}

const char *IntelFbPresentProbe::getPixelFormats(void)
{
    //
    // A NUL-NUL terminated list in Apple's format, which is why the literal ends
    // with an explicit \0 -- the string's own terminator closes the one entry and
    // this one closes the list. One entry, because the plane has one format and
    // this driver does not reformat it.
    //
    if (!presentable()) {
        IGLOG("getPixelFormats: not presentable (%s) -- returning an empty list",
              igPresentVerdictName(fPresentVerdict));
        return "\0";
    }

    return IO32BitDirectPixels "\0";
}

IOItemCount IntelFbPresentProbe::getDisplayModeCount(void)
{
    //
    // Zero when the group was refused, and that is the honest answer rather than
    // a defensive one: a framebuffer offering a mode over a surface it could not
    // describe is offering a mode it cannot honour.
    //
    if (!presentable())
        return 0;

    return IG_PRESENT_MODE_COUNT;
}

IOReturn IntelFbPresentProbe::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (allDisplayModes == nullptr)
        return kIOReturnBadArgument;

    if (!presentable())
        return kIOReturnUnsupported;

    allDisplayModes[0] = IG_PRESENT_MODE_ID;
    return kIOReturnSuccess;
}

IOReturn IntelFbPresentProbe::getInformationForDisplayMode(
    IODisplayModeID displayMode,
    IODisplayModeInformation *info)
{
    if (info == nullptr)
        return kIOReturnBadArgument;

    if (!presentable())
        return kIOReturnUnsupported;

    //
    // The one mode is compared against, never assumed. IOKit is not obliged to
    // ask only about modes this class advertised.
    //
    if (displayMode != IG_PRESENT_MODE_ID) {
        IGLOG("getInformationForDisplayMode: mode %d is not the one inherited "
              "mode (%d) -- refusing", (int) displayMode, IG_PRESENT_MODE_ID);
        return kIOReturnBadArgument;
    }

    bzero(info, sizeof(*info));

    info->nominalWidth  = igPresentWidth(fCtl, fStride, fSize, fSurf,
                                         fApertureBytes);
    info->nominalHeight = igPresentHeight(fCtl, fStride, fSize, fSurf,
                                          fApertureBytes);

    //
    // NOT MEASURED. The name of the constant says so and IGPresentRefreshIsMeasured
    // says so in the IORegistry; this field cannot carry the caveat itself
    // because IOKit's struct has nowhere to put one.
    //
    info->refreshRate   = IG_PRESENT_REFRESH_UNMEASURED;

    //
    // maxDepthIndex is the largest legal INDEX, not the count -- so one depth
    // means zero here. Writing the count would advertise a second depth that
    // getPixelInformation() refuses, and IOKit would be holding a depth this
    // class cannot describe.
    //
    info->maxDepthIndex = IG_PRESENT_DEPTH_COUNT - 1;

    return kIOReturnSuccess;
}

UInt64 IntelFbPresentProbe::getPixelFormatsForDisplayMode(
    IODisplayModeID displayMode,
    IOIndex depth)
{
    //
    // Apple's own drivers return 0 here and the value is vestigial -- the real
    // answer comes from getPixelInformation().
    //
    // THE GUARD BELOW IS BEHAVIOURALLY INERT AND IS KEPT ANYWAY. Say that
    // plainly rather than let a future reader infer a refusal that does not
    // happen: the refusal value and the answer are both 0, so no caller can
    // distinguish a guarded run from an unguarded one. Until 2026-08-20 this
    // function had no guard at all and the comment here CLAIMED one, which is
    // the same class of defect as a stale prose constant -- a sentence nothing
    // executes, describing code that does the opposite.
    //
    // The reason to add the guard rather than correct the sentence: it makes
    // "every IOFramebuffer override consults presentable() first" true with NO
    // exemptions, and a rule with no exemptions is gateable. The alternative is
    // an exemption set spelled by hand in tests/test_intel_fb_present_shape.py,
    // and a hand-typed subject set is exactly the anchor that silently narrows
    // (intel-handoff28.md SS3, intel-handoff30.md SS4b). One inert branch is
    // cheaper than a gate that can shrink without reddening.
    //
    (void) displayMode;
    (void) depth;

    if (!presentable())
        return 0;

    return 0;
}

IOReturn IntelFbPresentProbe::getPixelInformation(IODisplayModeID displayMode,
                                                  IOIndex depth,
                                                  IOPixelAperture aperture,
                                                  IOPixelInformation *pixelInfo)
{
    if (pixelInfo == nullptr)
        return kIOReturnBadArgument;

    if (!presentable())
        return kIOReturnUnsupported;

    if (displayMode != IG_PRESENT_MODE_ID) {
        IGLOG("getPixelInformation: mode %d is not the one inherited mode (%d) "
              "-- refusing", (int) displayMode, IG_PRESENT_MODE_ID);
        return kIOReturnBadArgument;
    }

    if (depth != IG_PRESENT_DEPTH_INDEX) {
        IGLOG("getPixelInformation: depth index %u is not the one depth (%u) "
              "-- refusing", (unsigned) depth, IG_PRESENT_DEPTH_INDEX);
        return kIOReturnBadArgument;
    }

    if (aperture != kIOFBSystemAperture) {
        IGLOG("getPixelInformation: aperture %u is not kIOFBSystemAperture "
              "-- refusing", (unsigned) aperture);
        return kIOReturnBadArgument;
    }

    bzero(pixelInfo, sizeof(*pixelInfo));

    pixelInfo->bytesPerRow      = igPresentBytesPerRow(fCtl, fStride, fSize,
                                                       fSurf, fApertureBytes);
    pixelInfo->bitsPerPixel     = igPresentBitsPerPixel(fCtl, fStride, fSize,
                                                        fSurf, fApertureBytes);
    pixelInfo->bitsPerComponent = igPresentComponentBits(fCtl, fStride, fSize,
                                                         fSurf, fApertureBytes);
    pixelInfo->componentCount   = igPresentComponentCount(fCtl, fStride, fSize,
                                                          fSurf, fApertureBytes);

    pixelInfo->activeWidth      = igPresentWidth(fCtl, fStride, fSize, fSurf,
                                                 fApertureBytes);
    pixelInfo->activeHeight     = igPresentHeight(fCtl, fStride, fSize, fSurf,
                                                  fApertureBytes);

    pixelInfo->pixelType        = kIORGBDirectPixels;

    //
    // bytesPerPlane is zero for a packed format, and this is one -- there is no
    // second plane. Writing the surface extent here would describe a planar
    // layout that does not exist.
    //
    pixelInfo->bytesPerPlane    = 0;

    //
    // The three component masks, taken from IntelPixel.hpp through
    // IntelFbPresent.hpp rather than spelled here. igPresentMasks() writes the
    // refusal sentinel into all three out-params BEFORE deciding, so a refused
    // group cannot leave a stale or partial mask behind -- and its return value
    // is checked rather than assumed, because presentable() being true is a claim
    // about the GROUP and this is a separate call that can refuse on its own
    // out-params.
    //
    UInt32 rMask = IG_PRESENT_INVALID32;
    UInt32 gMask = IG_PRESENT_INVALID32;
    UInt32 bMask = IG_PRESENT_INVALID32;

    if (!igPresentMasks(fCtl, fStride, fSize, fSurf, fApertureBytes,
                        &rMask, &gMask, &bMask)) {
        IGLOG("getPixelInformation: igPresentMasks() declined -- refusing "
              "rather than publishing a partial pixel description");
        return kIOReturnUnsupported;
    }

    pixelInfo->componentMasks[0] = rMask;
    pixelInfo->componentMasks[1] = gMask;
    pixelInfo->componentMasks[2] = bMask;

    //
    // strlcpy rather than strcpy, and the destination size comes from sizeof the
    // FIELD rather than from a counted number. IOPixelEncoding is a fixed 32-byte
    // array and IO32BitDirectPixels is exactly 32 characters plus a terminator,
    // so this is the one place in the file where a one-byte mistake would
    // truncate a format string into a different, plausible one.
    //
    strlcpy((char *) pixelInfo->pixelFormat, IO32BitDirectPixels,
            sizeof(pixelInfo->pixelFormat));

    return kIOReturnSuccess;
}

IOReturn IntelFbPresentProbe::getCurrentDisplayMode(IODisplayModeID *displayMode,
                                                    IOIndex *depth)
{
    if (displayMode == nullptr || depth == nullptr)
        return kIOReturnBadArgument;

    if (!presentable())
        return kIOReturnUnsupported;

    *displayMode = IG_PRESENT_MODE_ID;
    *depth       = IG_PRESENT_DEPTH_INDEX;

    return kIOReturnSuccess;
}

//
// setDisplayMode() accepts exactly the mode it found and refuses every other.
//
// This is not a mode setter and the refusal is the whole point: accepting a mode
// this class cannot program would report success over a display that did not
// change, which is worse than declining -- IOKit would then describe the panel
// with numbers that no longer match the scanout.
//
IOReturn IntelFbPresentProbe::setDisplayMode(IODisplayModeID displayMode,
                                             IOIndex depth)
{
    if (!presentable())
        return kIOReturnUnsupported;

    if (displayMode != IG_PRESENT_MODE_ID || depth != IG_PRESENT_DEPTH_INDEX) {
        IGLOG("setDisplayMode: refusing mode %d depth %u -- this driver "
              "inherits one mode (%d/%u) and programs nothing",
              (int) displayMode, (unsigned) depth,
              IG_PRESENT_MODE_ID, IG_PRESENT_DEPTH_INDEX);
        return kIOReturnUnsupported;
    }

    //
    // Success, and nothing happened. That is accurate: the requested mode is the
    // mode already programmed, so there is no work to do and no register to
    // write.
    //
    return kIOReturnSuccess;
}

//
// enableController() does not enable anything.
//
// It reports whether the inherited plane was presentable, which is the only
// honest thing a class with no write path can say here. Returning success
// unconditionally -- which is what a stub would do -- would tell IOKit a
// controller was brought up when nothing was touched.
//
IOReturn IntelFbPresentProbe::enableController(void)
{
    if (!presentable()) {
        IGLOG("enableController: the inherited plane is not presentable (%s) "
              "-- this driver has no write path, so there is nothing to enable",
              igPresentVerdictName(fPresentVerdict));
        return kIOReturnUnsupported;
    }

    IGLOG("enableController: the inherited plane is presentable -- adopting it "
          "without programming anything");
    return kIOReturnSuccess;
}

//
// False, deliberately.
//
// Claiming the console would make this the framebuffer the kernel draws panics
// and early boot text through, on a driver that has never run on silicon. The
// surface is inherited, so whatever the firmware set up is already on screen;
// there is nothing to gain and a boot to lose.
//
// THE GUARD BELOW IS BEHAVIOURALLY INERT, and it is here on purpose.
//
// Both arms answer false, so no caller can distinguish a guarded run from an
// unguarded one -- exactly the situation getPixelFormatsForDisplayMode() is in,
// where the refusal value and the answer are both 0. The reason to write it
// anyway is the same reason: it makes "every IOFramebuffer override consults
// presentable() first" true with NO exemptions, and a rule with no exemptions
// is gateable. The alternative is a hand-typed exemption set inside
// tests/test_intel_fb_present_shape.py, which is the anchor shape that silently
// narrows its own subject and reports green over a smaller set --
// intel-handoff28.md SS3 and intel-handoff30.md SS4b are both that defect.
//
// One inert branch is cheaper than a gate that can shrink without reddening.
// It also does NOT weaken the policy above: this method answers false whether
// or not the plane is presentable, and the guard must never be read as a hint
// that a presentable plane would claim the console.
//
bool IntelFbPresentProbe::isConsoleDevice(void)
{
    if (!presentable())
        return false;

    return false;
}
