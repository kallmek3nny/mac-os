//
//  IntelDisplayProbe.cpp
//
//  Gen12 (Tiger Lake) display-inheritance probe. Reads registers and writes
//  none -- there is no write path in this file at all. IntelDisplayProbe.hpp
//  explains why that asymmetry with NV3050Probe is deliberate.
//
//  Every decode here delegates to IntelDisplay.hpp, IntelFramebuffer.hpp or
//  IntelGpuId.hpp rather than open-coding a shift and a mask. Those three
//  headers are driven by tests/test_intel_display.cpp,
//  tests/test_intel_framebuffer.cpp and tests/test_intel_gpu_id.cpp under MSVC,
//  so the bit twiddling is verified on a machine with no Intel GPU and no
//  macOS. A shift written inline in this file would leave that reach.
//

#include "IntelDisplayProbe.hpp"

OSDefineMetaClassAndStructors(IntelDisplayProbe, IOService);

#define IGLOG(fmt, ...)  IOLog("IntelDisplayProbe: " fmt "\n", ## __VA_ARGS__)

//
// The verdict names, igTransName() and the property-key composition USED to live
// here as file statics. They are now in phase0-kext/IntelDisplayNames.hpp, driven
// by tests/test_intel_display_names.cpp.
//
// The reason for the move is the reason this whole project splits logic out of
// its IOKit shells: this translation unit includes <IOKit/IOService.h>, so no
// host suite can compile it, so nothing executed a single one of those functions.
// handoff19 had to record that as a gap, and the gap was not theoretical --
// igModeVerdictName() had arms for verdicts 0 through 5 while
// IG_MODE_VERDICT_LIMIT is 6, so IG_MODE_SRC_MISMATCH published the string
// "unknown" into the IORegistry. All four gates were green over that.
//
// If a verdict family grows, add the arm in IntelDisplayNames.hpp; its suite
// walks each family to the family's own _VERDICT_LIMIT and refuses "unknown"
// inside that range, so the omission reddens a host suite rather than surfacing
// as a misreported register on an operator's laptop.
//


bool IntelDisplayProbe::init(OSDictionary *dict)
{
    if (!super::init(dict))
        return false;

    fPCIDevice  = nullptr;
    fBar0Map    = nullptr;
    fBar0Base   = nullptr;
    fBar0Length = 0;
    fDeviceId   = IG_PCI_ID_INVALID;

    return true;
}

void IntelDisplayProbe::free(void)
{
    //
    // free() may run after a failed start(), so every member has to tolerate
    // being already-null. unmapBar0() is idempotent for that reason.
    //
    unmapBar0();
    OSSafeReleaseNULL(fPCIDevice);

    super::free();
}

//
// probe() -- decide whether this is silicon we have a decode story for.
//
// The personality's IOPCIMatch already narrowed the provider to one device ID,
// so re-reading config space here looks redundant. It is not. The plist is data
// an operator can edit, and tools/check-kext-personalities.py runs on the HOST;
// nothing on the target enforces it. Reading the ID and asking IntelGpuId.hpp
// makes the kext's own opinion the thing that decides, and the two disagreeing
// is a log line rather than a silent attach to a die whose display block this
// project has never seen.
//
IOService *IntelDisplayProbe::probe(IOService *provider, SInt32 *score)
{
    IOService *result = super::probe(provider, score);
    if (!result)
        return nullptr;

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        IGLOG("probe: provider is not an IOPCIDevice, declining");
        return nullptr;
    }

    UInt16 vendor = pci->configRead16(kIOPCIConfigVendorID);
    UInt16 device = pci->configRead16(kIOPCIConfigDeviceID);

    if (!igIsSupportedDevice(vendor, device)) {
        IGLOG("probe: %04x:%04x is not a device this track decodes, declining",
              vendor, device);
        return nullptr;
    }

    fDeviceId = device;

    IGLOG("probe: %04x:%04x accepted (GT class %u)",
          vendor, device, igTglGtClass(device));

    return result;
}

bool IntelDisplayProbe::start(IOService *provider)
{
    IGLOG("start");

    if (!super::start(provider))
        return false;

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        IGLOG("start: provider is not an IOPCIDevice");
        super::stop(provider);
        return false;
    }
    fPCIDevice->retain();

    //
    // Memory decode must be on before any BAR read. Without this the aperture
    // reads back all-ones on some platforms and zero on others -- and on THIS
    // silicon zero is also what an unprogrammed display page reads, so a
    // forgotten setMemoryEnable() would be indistinguishable from a panel that
    // firmware never brought up.
    //
    fPCIDevice->setMemoryEnable(true);

    if (!mapBar0())
        goto fail;

    publishIdentity();
    scanTranscoders();
    scanPlanes();
    publishSinkRehearsal();

    registerService();

    IGLOG("start: complete");
    return true;

fail:
    unmapBar0();
    OSSafeReleaseNULL(fPCIDevice);
    super::stop(provider);
    return false;
}

void IntelDisplayProbe::stop(IOService *provider)
{
    IGLOG("stop");

    unmapBar0();
    OSSafeReleaseNULL(fPCIDevice);

    super::stop(provider);
}

//
// ---- BAR0 -----------------------------------------------------------------
//

bool IntelDisplayProbe::mapBar0(void)
{
    IODeviceMemory *bar = fPCIDevice->getDeviceMemoryWithIndex(IG_BAR0_INDEX);
    if (!bar) {
        IGLOG("mapBar0: no device memory at index %d", IG_BAR0_INDEX);
        return false;
    }

    fBar0Map = bar->map();
    if (!fBar0Map) {
        IGLOG("mapBar0: map() failed");
        return false;
    }

    fBar0Base = (volatile UInt8 *) fBar0Map->getVirtualAddress();
    if (!fBar0Base) {
        IGLOG("mapBar0: mapped with a null virtual address");
        OSSafeReleaseNULL(fBar0Map);
        return false;
    }

    IOByteCount length = fBar0Map->getLength();

    //
    // Recorded, not merely logged. Until this assignment existed the warning
    // below was the ONLY consequence of a short BAR -- regRead32() bounded
    // offsets against the compile-time policy range and never learned how many
    // bytes were really mapped, so on a short BAR an in-policy offset read past
    // the end of the mapping. IOByteCount is 64 bits and IGU64 is 64 bits, so
    // nothing is narrowed on the way in.
    //
    fBar0Length = (IGU64) length;

    IGLOG("mapBar0: %llu bytes at %p",
          (unsigned long long) length, (void *) fBar0Base);

    //
    // A warning, not a refusal. See IG_BAR0_EXPECTED_LENGTH: declining here
    // would delete the IORegistry node that makes a short BAR inspectable.
    //
    if (length < IG_BAR0_EXPECTED_LENGTH) {
        IGLOG("mapBar0: WARNING BAR0 is %llu bytes, shorter than the 0x%x the "
              "display block needs -- reads past the end are undefined, not "
              "faults",
              (unsigned long long) length, IG_BAR0_EXPECTED_LENGTH);
    }

    return true;
}

void IntelDisplayProbe::unmapBar0(void)
{
    fBar0Base   = nullptr;
    fBar0Length = 0;
    OSSafeReleaseNULL(fBar0Map);
}

//
// Every MMIO read in this file goes through here, and every offset is checked
// against igIsInDisplayPolicyRange() first.
//
// That bound is a POLICY range covering the display block this track decodes,
// not the whole BAR. An offset outside it is a bug in this file rather than a
// hardware condition -- there is no register this class has any business reading
// elsewhere -- so it returns IG_MMIO_INVALID and says so, rather than reading a
// real register in an engine nothing here understands.
//
// IG_MMIO_INVALID is 0xFFFFFFFF, which is also what a dead aperture reads. The
// two are distinguished by the log line, deliberately: a caller that cannot tell
// them apart should not be silently choosing between them.
//
UInt32 IntelDisplayProbe::regRead32(UInt32 offset)
{
    //
    // The three guards that used to be spelled out here are now
    // igReadDecision() in IntelDisplay.hpp, and the reason for the move is the
    // one handoff20 SS1 is about: a predicate written inside this file is a
    // predicate no suite can execute. Their ORDER is the part worth keeping
    // honest -- IG_MMIO_INVALID is also outside the policy range, so deleting
    // the sentinel arm changes nothing about what this function RETURNS and only
    // corrupts the reason it logs. tests/test_intel_display.cpp pins the
    // precedence on inputs where two guards are true at once.
    //
    // Every arm still returns IG_MMIO_INVALID, unchanged. That value is also
    // what a dead aperture reads, and the two remain distinguished by the log
    // line rather than by the return value -- deliberately, because a caller
    // that cannot tell them apart should not be silently choosing between them.
    //
    switch (igReadDecision(fBar0Base != nullptr, fBar0Length, offset)) {
    case IG_READ_UNMAPPED:
        IGLOG("regRead32(0x%x): BAR0 is not mapped", offset);
        return IG_MMIO_INVALID;

    case IG_READ_SENTINEL_OFFSET:
        //
        // An offset computed by igTransTimingReg() or igPlaneReg() that came
        // back as their refusal sentinel. Reading it would be reading whatever
        // sits at the very top of the BAR.
        //
        IGLOG("regRead32: refusing IG_MMIO_INVALID -- an offset helper declined");
        return IG_MMIO_INVALID;

    case IG_READ_OUT_OF_POLICY:
        IGLOG("regRead32(0x%x): outside the display policy range "
              "0x%x..0x%x -- refusing",
              offset, IG_DISPLAY_POLICY_BASE, IG_DISPLAY_POLICY_LIMIT);
        return IG_MMIO_INVALID;

    case IG_READ_PAST_MAPPING:
        //
        // A legal display offset, and the BAR is genuinely mapped -- it is just
        // SHORTER than the display block needs. mapBar0() warned about this and
        // attached anyway, on purpose; this is where that warning finally has a
        // consequence other than a log line.
        //
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
// ---- what we publish -------------------------------------------------------
//

void IntelDisplayProbe::publishIdentity(void)
{
    setProperty("IGDeviceID", (UInt64) fDeviceId, 32);
    setProperty("IGGtClass", (UInt64) igTglGtClass(fDeviceId), 32);
    setProperty("IGIsTigerLake", igIsTglDevice(fDeviceId));

    //
    // The one device ID this project has ever seen on real silicon, published
    // next to the one we actually matched so an operator can tell "the observed
    // part" from "a part the table also lists". Everything except 0x9A68 rests
    // on the vendored i915 table alone -- see INTEL-DEVICE-IDS.md.
    //
    setProperty("IGDeviceIsTheObservedPart",
                fDeviceId == IG_DEVICE_OBSERVED_2026_08_16);
}

//
// Walk every transcoder rather than assuming A.
//
// The 2026-08-16 capture read 0x00000000 out of transcoders B/C/D, and the
// capture script printed ABSENT for them. That was the script's word, not the
// silicon's: unused display pages on this die read zero rather than all-ones, so
// "absent" and "never programmed" are indistinguishable from a register read.
// Transcoders B, C and D certainly exist. A driver that hardcoded A because of
// that capture would be right on this laptop and silently wrong elsewhere.
//
// DSI0/DSI1 are excluded from the scan -- IG_TRANS_SCAN_COUNT is 4, not
// IG_TRANSCODER_COUNT. igPlaneReg() refuses any pipe above IG_TRANSCODER_D
// because DSI is off the 0x1000 plane ladder entirely, so scanning DSI here
// would produce transcoder verdicts with no plane to join them against. That is
// handoff15 Q1/Q2, and it is unanswerable on an eDP-only machine rather than
// merely unanswered.
//
void IntelDisplayProbe::scanTranscoders(void)
{
    for (UInt32 trans = 0; trans < IG_TRANS_SCAN_COUNT; trans++)
        publishTranscoder(trans);
}

void IntelDisplayProbe::publishTranscoder(UInt32 trans)
{
    UInt32 conf   = regRead32(igTransConfReg(trans));
    UInt32 ddi    = regRead32(igTransDdiReg(trans));

    UInt32 hTotal = regRead32(igTransTimingReg(trans, IG_TRANS_HTOTAL));
    UInt32 hBlank = regRead32(igTransTimingReg(trans, IG_TRANS_HBLANK));
    UInt32 hSync  = regRead32(igTransTimingReg(trans, IG_TRANS_HSYNC));
    UInt32 vTotal = regRead32(igTransTimingReg(trans, IG_TRANS_VTOTAL));
    UInt32 vBlank = regRead32(igTransTimingReg(trans, IG_TRANS_VBLANK));
    UInt32 vSync  = regRead32(igTransTimingReg(trans, IG_TRANS_VSYNC));
    UInt32 src    = regRead32(igTransTimingReg(trans, IG_TRANS_PIPESRC));

    //
    // PLANE_SIZE for plane 1 of this pipe, which is the word igPipeUsable()
    // joins the raster against.
    //
    // PLANE_SIZE and PIPESRC are BYTE-SWAPPED halves of one another -- the
    // capture read 0x077F0437 and 0x0437077F for the same mode on the same
    // pipe. Each has its own decoder for exactly that reason, and reusing one
    // for the other returns a plausible number with the axes TRANSPOSED. The
    // join below is what catches that; it is not caught by any single read.
    //
    UInt32 planeSize = regRead32(igPlaneReg(trans, 0u, IG_PLANE_SIZE));

    //
    // The ten reads above, judged as ONE group before any of them is decoded.
    //
    // As a group because the decoders are joins: igPipeUsable() consumes nine of
    // these words and returns a single verdict, so one refused read anywhere in
    // the set poisons the whole answer rather than one field of it. Until this
    // existed, IG_MMIO_INVALID appeared at exactly one place in this file --
    // inside regRead32() -- and every refusal flowed straight into the decoders
    // below. setProperty() with a decode of 0xFFFFFFFF is byte-identical to
    // setProperty() with a decode of a register that genuinely read all-ones, so
    // the IORegistry could not distinguish "this transcoder is unprogrammed"
    // from "this transcoder was never read". The second is a fact about this
    // driver that somebody can go and fix.
    //
    const UInt32 sampled[] = { conf, ddi, hTotal, hBlank, hSync,
                               vTotal, vBlank, vSync, src, planeSize };
    UInt32 sample = igSampleVerdict(sampled,
                                   (UInt32) (sizeof(sampled) / sizeof(sampled[0])));

    UInt32 link   = igTransconfDdiLinked(conf, ddi);
    UInt32 mode   = igModeConsistent(hTotal, hBlank, hSync,
                                     vTotal, vBlank, vSync, src);
    UInt32 usable = igPipeUsable(conf, ddi, hTotal, hBlank, hSync,
                                 vTotal, vBlank, vSync, src, planeSize);

    IGLOG("transcoder %s: link=%s mode=%s usable=%s "
          "active=%ux%u port=%u",
          igTransName(trans),
          igLinkVerdictName(link),
          igModeVerdictName(mode),
          igUsableVerdictName(usable),
          igPipesrcWidth(src), igPipesrcHeight(src),
          igDdiPort(ddi));

    //
    // Property keys carry the transcoder letter rather than an index, because
    // "IGTranscoder2" would be ambiguous between the third transcoder and
    // transcoder C depending on whether the reader counts from zero.
    //
    // Every key goes through igTranscoderKey(), which refuses rather than
    // truncating and writes an empty string on refusal. A key that did not fit is
    // therefore visibly empty in the IORegistry instead of being a shortened
    // string that sorts next to the real one. The buffer is sized from
    // IG_KEY_BUFFER_BYTES rather than a number counted by hand, and
    // test_intel_display_names.cpp composes every real prefix/unit/suffix triple
    // at that size so a suffix added here cannot overflow it undetected.
    //
    char key[IG_KEY_BUFFER_BYTES];

    //
    // Published on BOTH paths, never conditionally, for the reason handoff18
    // SS1d settled on for GSPEngineReadable: a flag that appears only when
    // something went wrong is indistinguishable from an older kext that did not
    // publish it at all. "ok" is a measurement; an absent key is not.
    //
    // It is published FIRST so it sorts above the values it qualifies -- every
    // key below this one is a decode of the ten words this verdict is about.
    //
    if (igTranscoderKey(key, sizeof(key), trans, "-Sample"))
        setProperty(key, igSampleVerdictName(sample));

    if (igTranscoderKey(key, sizeof(key), trans, "-Link"))
        setProperty(key, igLinkVerdictName(link));

    if (igTranscoderKey(key, sizeof(key), trans, "-Mode"))
        setProperty(key, igModeVerdictName(mode));

    if (igTranscoderKey(key, sizeof(key), trans, "-Usable"))
        setProperty(key, igUsableVerdictName(usable));

    if (igTranscoderKey(key, sizeof(key), trans, "-Width"))
        setProperty(key, (UInt64) igPipesrcWidth(src), 32);

    if (igTranscoderKey(key, sizeof(key), trans, "-Height"))
        setProperty(key, (UInt64) igPipesrcHeight(src), 32);
}

//
// The plane side: is what firmware left in plane 1 of each pipe something this
// driver could inherit rather than reprogram?
//
// igFbInheritable() answers that from three words and nothing else, so this
// function is a read loop and a log. The verdict ORDER inside it is load-bearing
// (compression before format, because a compressed XRGB_8888 plane has a format
// this driver understands and a layout it does not) and lives in the header
// where a test can drive it.
//
void IntelDisplayProbe::scanPlanes(void)
{
    for (UInt32 pipe = 0; pipe < IG_TRANS_SCAN_COUNT; pipe++) {
        publishPlane(pipe);
        publishPlaneLiveObservation(pipe);
    }
}

void IntelDisplayProbe::publishPlane(UInt32 pipe)
{
    UInt32 ctl    = regRead32(igPlaneReg(pipe, 0u, IG_PLANE_CTL));
    UInt32 stride = regRead32(igPlaneReg(pipe, 0u, IG_PLANE_STRIDE));
    UInt32 size   = regRead32(igPlaneReg(pipe, 0u, IG_PLANE_SIZE));
    UInt32 surf   = regRead32(igPlaneReg(pipe, 0u, IG_PLANE_SURF));

    //
    // The four reads above, judged as ONE group before any of them is decoded --
    // the same reason publishTranscoder() does it, on a smaller set.
    // igFbInheritable() joins three of these words into a single verdict, so a
    // refusal anywhere in the group poisons the answer rather than one field.
    //
    // SURF is in the group even though it feeds only the offset and the range
    // check. A refused SURF decodes to an offset of 0xFFFFFFC0-ish and a range
    // verdict computed over an address nobody read, and that is exactly the
    // published-zero shape this guard exists to stop.
    //
    const UInt32 sampled[] = { ctl, stride, size, surf };
    UInt32 sample = igSampleVerdict(sampled,
                                   (UInt32) (sizeof(sampled) / sizeof(sampled[0])));

    UInt32 verdict = igFbInheritable(ctl, stride, size);
    UInt32 bytes   = igFbSurfaceBytes(stride, size);

    //
    // The range check bounds base + bytes, not just the base. A base inside the
    // aperture whose EXTENT runs past the end is the silent-truncation shape
    // this project has already been bitten by on the NVIDIA staging path
    // (nvGspStageRangeOk), so it is checked here the same way.
    //
    // igFbSurfaceBytes() returns 0 for anything it cannot describe, and
    // igFbSurfaceRangeOk() refuses a zero extent, so a broken decode reports
    // range=no rather than a range of nothing that reads as fine.
    //
    bool rangeOk = igFbSurfaceRangeOk(igFbSurfaceOffset(surf), bytes);

    //
    // ---- the SCANOUT descriptor ------------------------------------------
    //
    // igFbInheritable() answers "is this plane a layout this driver
    // understands". rangeOk answers "does base + bytes stay inside the address
    // space PLANE_SURF's field can express". Neither answers the question an
    // IOFramebuffer subclass has to answer before it presents anything: given
    // the base, the stride and the height, is the byte EXTENT describable in the
    // 32-bit fields IOFramebuffer carries, and does it fit?
    //
    // Until this call landed, igScanDescribe() had 61 passing checks over it and
    // NO CALLER anywhere outside its own header. intel-handoff26 SS1 is that
    // exact class one subsystem over -- a heavily tested pure function passes
    // every suite it has while contributing nothing to the driver, because a
    // suite proves the function is CORRECT and says nothing about whether it is
    // CONSULTED.
    //
    // WHY THE ARGUMENTS ARE THESE ARGUMENTS
    //
    // Every value handed over is ALREADY DECODED by IntelFramebuffer.hpp.
    // IntelScanout.hpp refuses to decode PLANE_CTL, refuses to convert a stride
    // word to bytes, and refuses to derive bytes-per-pixel from a format code --
    // it says so in its own header and check-registers.py enforces the
    // ownership. Re-deriving any of the three here would give this tree two
    // answers to one question, which is the defect tests/test_igpu_mmio_ranges.py
    // exists to prevent one layer down.
    //
    // THE APERTURE ARGUMENT IS THE ONE THAT COULD FABRICATE EVIDENCE
    //
    // PLANE_SURF holds a GGTT offset, and this driver has never mapped the
    // graphics address space -- BAR0 is the 16 MiB register block and is the
    // only mapping mapBar0() has ever made. There is no measured aperture size
    // to pass, and IntelScanout.hpp is explicit that writing a BAR index into it
    // would be inventing evidence.
    //
    // So the bound passed is IG_FB_GGTT_POLICY_LIMIT, and it is honest for one
    // specific reason: it is not a claim about how much memory exists. It is the
    // ceiling of what PLANE_SURF's own 32-bit address field can EXPRESS, already
    // documented as a POLICY CEILING in IntelFramebuffer.hpp and already the
    // bound rangeOk uses. A descriptor refused against it is refused for a fact
    // about the register, not a guess about the machine. A descriptor ACCEPTED
    // against it has been checked for extent overflow and for expressibility --
    // and has NOT been checked against a real mapping, because no such mapping
    // exists to check against. That limit is written into the header and is why
    // the key below is named Scan rather than anything promising more.
    //
    UInt32 scan = igScanDescribe(surf,
                                 igFbStrideBytes(stride),
                                 igFbSizeWidth(size),
                                 igFbSizeHeight(size),
                                 igFbBytesPerPixel(igFbFormat(ctl)),
                                 IG_FB_GGTT_POLICY_LIMIT);

    IGLOG("plane %s.1: inherit=%s %ux%u bpp=%u stride=%u bytes=%u "
          "surf=0x%08x range=%s scan=%s%s",
          igTransName(pipe),
          igInheritVerdictName(verdict),
          igFbSizeWidth(size), igFbSizeHeight(size),
          igFbBytesPerPixel(igFbFormat(ctl)),
          igFbStrideBytes(stride),
          bytes,
          igFbSurfaceOffset(surf),
          rangeOk ? "ok" : "no",
          igScanVerdictName(scan),
          igFbSurfaceDecrypt(surf) ? " DECRYPT" : "");

    //
    // The "1" in each key is plane 1, the primary plane -- the only one this scan
    // reads. It is part of the suffix rather than composed from a plane index
    // because nothing here iterates planes; a second plane would need its own
    // loop and its own justification, not a formatted number.
    //
    char key[IG_KEY_BUFFER_BYTES];

    //
    // Published on both paths and first, for the reasons publishTranscoder()
    // gives above: an absent key is not a measurement, and this verdict
    // qualifies every key below it.
    //
    if (igPlaneKey(key, sizeof(key), pipe, "1-Sample"))
        setProperty(key, igSampleVerdictName(sample));

    if (igPlaneKey(key, sizeof(key), pipe, "1-Inherit"))
        setProperty(key, igInheritVerdictName(verdict));

    if (igPlaneKey(key, sizeof(key), pipe, "1-Width"))
        setProperty(key, (UInt64) igFbSizeWidth(size), 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-Height"))
        setProperty(key, (UInt64) igFbSizeHeight(size), 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-StrideBytes"))
        setProperty(key, (UInt64) igFbStrideBytes(stride), 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-SurfaceBytes"))
        setProperty(key, (UInt64) bytes, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-SurfaceOffset"))
        setProperty(key, (UInt64) igFbSurfaceOffset(surf), 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-RangeOk"))
        setProperty(key, rangeOk);

    //
    // The scanout verdict as a WORD, in the idiom of the five families beside
    // it. A bare number here would be the thing igScanVerdictName() was written
    // to prevent: six refusal reasons that call for six different actions,
    // published as an integer an operator has to go and look up.
    //
    if (igPlaneKey(key, sizeof(key), pipe, "1-Scan"))
        setProperty(key, igScanVerdictName(scan));

    //
    // igScanUsable() is an EQUALITY against IG_SCAN_OK, never a negation of one
    // error arm. 3050-handoff33 SS2 is the cost of the alternative: a
    // `!= ERR_SOMETHING` form is TRUE for every OTHER refusal, so a descriptor
    // refused for a reason added later would publish as usable. Published as its
    // own key rather than left for a reader to infer from the word above,
    // because the go/no-go is what a framebuffer subclass consumes and an
    // inference is not a measurement.
    //
    if (igPlaneKey(key, sizeof(key), pipe, "1-ScanUsable"))
        setProperty(key, igScanUsable(scan) != 0);

    //
    // The 64-bit extent, published only when the descriptor is usable.
    //
    // This is the asymmetry IntelScanout.hpp exists for. bytesPerRow * height
    // WRAPS in 32 bits rather than saturating, and a wrapped extent is SMALLER
    // than the truth -- so truncation looks conservative while pointing the
    // display engine past the end of the mapping. igScanExtentBytes() returns
    // 64 bits and igScanDescribe() has already refused anything that does not
    // fit IG_SCAN_EXTENT_POLICY_CEILING, so reaching this line means the value
    // is describable in the 32-bit field IOFramebuffer carries.
    //
    // Guarded rather than published unconditionally: an extent computed over a
    // refused surf word or a zero geometry is arithmetic over something nobody
    // measured, and setProperty() with a fabricated number is byte-identical to
    // setProperty() with a real one. The verdict and the usable flag above are
    // published on BOTH paths, so the absence of this key is explained by a key
    // that is present rather than being a question.
    //
    if (igScanUsable(scan) &&
        igPlaneKey(key, sizeof(key), pipe, "1-ScanExtent")) {
        setProperty(key,
                    (UInt64) igScanExtentBytes(igFbStrideBytes(stride),
                                               igFbSizeHeight(size)),
                    64);
    }
}

//
// Second, independently bracketed primary-plane sample. publishPlane() above is
// intentionally left untouched: this method answers a different question --
// whether the programmed surface was still the live surface for one coherent
// read window. SURFLIVE is sampled first and last around every other word so a
// flip that lands mid-snapshot becomes an explicit refusal rather than a mixed
// record. igPlaneLiveObserve() owns the exact raw SURF equality check and all
// semantic gates. This shell only performs bounded reads and publishes evidence.
//
// There is no write accessor here and this method performs no writes. The
// WroteRegisters property is unconditional so absence can never be mistaken for
// proof that a future revision did not program the plane.
//
void IntelDisplayProbe::publishPlaneLiveObservation(UInt32 pipe)
{
    IgPlaneLiveSample sample = {};
    sample.deviceId = fDeviceId;
    sample.pipe = pipe;

    sample.surfLiveBefore = regRead32(igPlaneReg(pipe,
                                                IG_PLANE_LIVE_PRIMARY_PLANE,
                                                IG_PLANE_SURFLIVE));
    sample.transConf = regRead32(igTransConfReg(pipe));
    sample.pipeSrc = regRead32(igTransTimingReg(pipe, IG_TRANS_PIPESRC));
    sample.ctl = regRead32(igPlaneReg(pipe,
                                     IG_PLANE_LIVE_PRIMARY_PLANE,
                                     IG_PLANE_CTL));
    sample.stride = regRead32(igPlaneReg(pipe,
                                        IG_PLANE_LIVE_PRIMARY_PLANE,
                                        IG_PLANE_STRIDE));
    sample.pos = regRead32(igPlaneReg(pipe,
                                     IG_PLANE_LIVE_PRIMARY_PLANE,
                                     IG_PLANE_POS));
    sample.size = regRead32(igPlaneReg(pipe,
                                      IG_PLANE_LIVE_PRIMARY_PLANE,
                                      IG_PLANE_SIZE));
    sample.surf = regRead32(igPlaneReg(pipe,
                                      IG_PLANE_LIVE_PRIMARY_PLANE,
                                      IG_PLANE_SURF));
    sample.offset = regRead32(igPlaneReg(pipe,
                                        IG_PLANE_LIVE_PRIMARY_PLANE,
                                        IG_PLANE_OFFSET));
    sample.surfLiveAfter = regRead32(igPlaneReg(pipe,
                                               IG_PLANE_LIVE_PRIMARY_PLANE,
                                               IG_PLANE_SURFLIVE));

    IgPlaneLiveObservation observed = {};
    UInt32 verdict = igPlaneLiveObserve(&sample, &observed);

    IGLOG("plane %s.1 live: verdict=%s before=0x%08x after=0x%08x surf=0x%08x",
          igTransName(pipe),
          igPlaneLiveVerdictName(verdict),
          sample.surfLiveBefore,
          sample.surfLiveAfter,
          sample.surf);

    char key[IG_KEY_BUFFER_BYTES];

    // The verdict, zero-write fact and raw words are published on every path.
    // That makes every refusal inspectable without decoding data the pure helper
    // explicitly declined to trust.
    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveObserve"))
        setProperty(key, igPlaneLiveVerdictName(verdict));

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveWroteRegisters"))
        setProperty(key, false);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveBefore"))
        setProperty(key, (UInt64) sample.surfLiveBefore, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveAfter"))
        setProperty(key, (UInt64) sample.surfLiveAfter, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveTransConf"))
        setProperty(key, (UInt64) sample.transConf, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LivePipeSrc"))
        setProperty(key, (UInt64) sample.pipeSrc, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveCtl"))
        setProperty(key, (UInt64) sample.ctl, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveStride"))
        setProperty(key, (UInt64) sample.stride, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LivePos"))
        setProperty(key, (UInt64) sample.pos, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveSize"))
        setProperty(key, (UInt64) sample.size, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveSurf"))
        setProperty(key, (UInt64) sample.surf, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveOffset"))
        setProperty(key, (UInt64) sample.offset, 32);

    if (!igPlaneLiveSucceeded(verdict))
        return;

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveWidth"))
        setProperty(key, (UInt64) observed.width, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveHeight"))
        setProperty(key, (UInt64) observed.height, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveStrideBytes"))
        setProperty(key, (UInt64) observed.strideBytes, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveSurfaceWord"))
        setProperty(key, (UInt64) observed.liveSurfaceWord, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveSurfaceOffset"))
        setProperty(key, (UInt64) observed.programmedSurfaceOffset, 32);

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveInherit"))
        setProperty(key, igInheritVerdictName(observed.inheritVerdict));

    if (igPlaneKey(key, sizeof(key), pipe, "1-LiveCoherent"))
        setProperty(key, true);
}

//
// ---- the 3050 sink rehearsal -----------------------------------------------
//
// Step (b) of the sink plan, composed and PUBLISHED, never written. Five reads
// of pipe A plane 1, one call into IntelSinkRehearse.hpp, and the answer laid
// out in the IORegistry so an operator can check the plan against the live
// plane before any code that writes a plane register exists.
//
// It writes NOTHING. There is no write accessor in this class and this method
// does not acquire one: igPlaneWriteOffset() is never walked here, and
// IGSinkRehearsalWroteRegisters is published false on every path so its
// ABSENCE can never be read as "wrote". tests/test_intel_sink_rehearsal_shape.py
// pins all of that structurally, because no host suite can compile this file.
//
// The five words are a SECOND sample of the plane publishPlane() just read.
// That is deliberate: the rehearsal publishes its own inputs beside its own
// verdict, so the evidence for every composed word sits next to it and nobody
// has to assume two samples of a running display agreed (handoff28 SS4d).
//
void IntelDisplayProbe::publishSinkRehearsal(void)
{
    //
    // The locals carry an in- prefix on purpose: they are the INPUTS of the
    // rehearsal, published as IGSinkRehearsalIn*, and the prefix keeps them
    // textually distinct from scanPlanes()'s ctl/stride/size/surf group, which
    // test_intel_publisher_sample_shape.py mutates by exact anchor.
    //
    UInt32 inCtl     = regRead32(igPlaneReg(IG_SINK_REHEARSE_PIPE, 0u, IG_PLANE_CTL));
    UInt32 inStride  = regRead32(igPlaneReg(IG_SINK_REHEARSE_PIPE, 0u, IG_PLANE_STRIDE));
    UInt32 inSize    = regRead32(igPlaneReg(IG_SINK_REHEARSE_PIPE, 0u, IG_PLANE_SIZE));
    UInt32 inSurf    = regRead32(igPlaneReg(IG_SINK_REHEARSE_PIPE, 0u, IG_PLANE_SURF));
    UInt32 inAuxDist = regRead32(igPlaneReg(IG_SINK_REHEARSE_PIPE, 0u, IG_PLANE_AUX_DIST));

    const UInt32 sampled[] = { inCtl, inStride, inSize, inSurf, inAuxDist };
    UInt32 inSample = igSampleVerdict(sampled,
                                   (UInt32) (sizeof(sampled) / sizeof(sampled[0])));

    //
    // The bound. getDeviceMemoryWithIndex() hands back the provider's own
    // range, unretained, and only its length is taken. A missing BAR measures
    // as zero, which igSinkRehearse() refuses as NO_BOUND rather than planning
    // against an aperture nobody measured.
    //
    IODeviceMemory *aperture =
        fPCIDevice->getDeviceMemoryWithIndex(IG_SINK_REHEARSE_APERTURE_INDEX);
    IGU64 apertureBytes = aperture ? (IGU64) aperture->getLength() : 0;

    IgSinkRehearsal rehearsal;
    UInt32 verdict = igSinkRehearse(inCtl, inStride, inSize, inSurf, inAuxDist,
                                    apertureBytes, &rehearsal);

    IGLOG("sink rehearsal: sample=%s verdict=%s alloc=%u plane=%u "
          "aperture=%llu base=0x%08x",
          igSampleVerdictName(inSample),
          igSinkRehearseVerdictName(verdict),
          rehearsal.allocResult, rehearsal.planeVerdict,
          (unsigned long long) apertureBytes, rehearsal.plan.base);

    //
    // FIRST and unconditional: the sample verdict, the rehearsal verdict, and
    // the statement that nothing was written. An absent key is a question, not
    // an answer (handoff18 SS1d).
    //
    setProperty("IGSinkRehearsalSample", igSampleVerdictName(inSample));
    setProperty("IGSinkRehearsalVerdict", igSinkRehearseVerdictName(verdict));
    setProperty("IGSinkRehearsalWroteRegisters", false);

    //
    // The inputs, on every path: they are the evidence every composed word
    // below is derived from, and a refusal is only checkable against them.
    //
    setProperty("IGSinkRehearsalInCtl",     (UInt64) inCtl,     32);
    setProperty("IGSinkRehearsalInStride",  (UInt64) inStride,  32);
    setProperty("IGSinkRehearsalInSize",    (UInt64) inSize,    32);
    setProperty("IGSinkRehearsalInSurf",    (UInt64) inSurf,    32);
    setProperty("IGSinkRehearsalInAuxDist", (UInt64) inAuxDist, 32);
    setProperty("IGSinkRehearsalInApertureBytes", (UInt64) apertureBytes, 64);

    //
    // The stage results, on every path, as NUMBERS: IG_SINK_REHEARSE_NOT_REACHED
    // (all-ones) for a stage the rehearsal never got to, never a zero that
    // would read as IG_SINK_ALLOC_OK.
    //
    setProperty("IGSinkRehearsalAllocResult",  (UInt64) rehearsal.allocResult,  32);
    setProperty("IGSinkRehearsalPlaneVerdict", (UInt64) rehearsal.planeVerdict, 32);

    //
    // Everything below is COMPOSED, and every one of them is behind the guard:
    // setProperty() with a NOT_REACHED word is byte-identical to setProperty()
    // with a planned base. The guard is a conjunction of two equalities, never a
    // negation of one error arm.
    //
    if (!(inSample == IG_SAMPLE_OK && igSinkRehearseUsable(verdict))) {
        IGLOG("sink rehearsal: publishing the inputs and no composition");
        return;
    }

    setProperty("IGSinkRehearsalReservedEnd", (UInt64) rehearsal.reservedEnd, 64);
    setProperty("IGSinkPlanBase",   (UInt64) rehearsal.plan.base,   32);
    setProperty("IGSinkPlanStride", (UInt64) rehearsal.plan.stride, 32);
    setProperty("IGSinkPlanBytes",  (UInt64) rehearsal.plan.size,   32);
    setProperty("IGSinkPlaneCtl",    (UInt64) rehearsal.regs.ctl,    32);
    setProperty("IGSinkPlaneStride", (UInt64) rehearsal.regs.stride, 32);
    setProperty("IGSinkPlanePos",    (UInt64) rehearsal.regs.pos,    32);
    setProperty("IGSinkPlaneSize",   (UInt64) rehearsal.regs.size,   32);
    setProperty("IGSinkPlaneSurf",   (UInt64) rehearsal.regs.surf,   32);
}
