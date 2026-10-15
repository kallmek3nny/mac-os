//
//  IntelDisplayProbe.hpp
//  Gen12 (Tiger Lake) display-inheritance probe for macOS.
//
//  Scope: PCI match, MMIO BAR map, device identification, and READ-ONLY
//  inspection of the display block that firmware already programmed. Nothing is
//  written to the GPU -- there is no write path in this class at all, not even a
//  refusing one. See below for why that is a deliberate asymmetry with
//  NV3050Probe rather than an omission.
//
//  THE SPLIT, RESTATED FOR THIS TRACK
//  ----------------------------------
//  Every register constant, offset computation, bit-field decode and verdict
//  lives in IntelDisplay.hpp, IntelFramebuffer.hpp and IntelGpuId.hpp. Those
//  three headers are dependency-free -- no includes, no IOKit, no kernel types,
//  no allocation, no sleep -- which is what lets tests/test_intel_display.cpp,
//  tests/test_intel_framebuffer.cpp and tests/test_intel_gpu_id.cpp compile the
//  IDENTICAL files under MSVC on a machine with no Intel GPU and no macOS.
//
//  This file is the ONLY Intel-track file that may touch IOService, IOPCIDevice,
//  or an IOMemoryMap. If a decode appears here that is not a call into one of
//  those three headers, the split has been broken and the corresponding logic
//  has left the reach of the host test suites.
//
//  WHY THIS CLASS HAS NO WRITE PATH
//  --------------------------------
//  NV3050Probe carries regWrite32() behind three refusals and a
//  -DPHASE4_ALLOW_WRITES build flag, because the NVIDIA track's whole point is
//  eventually booting GSP firmware, which is a write sequence.
//
//  The Intel track's premise is the opposite one. On the target laptop the
//  firmware has ALREADY brought up the eDP panel before any macOS driver runs:
//  the transcoder is timing, the DDI is connected, the plane is scanning out.
//  The useful thing a first driver can do is READ that state and decide whether
//  it is coherent enough to inherit. A write path here would be a facility with
//  no caller and no test, and the one thing it could do on real hardware is
//  blank a working panel.
//
//  So there is no regWrite32(). Adding one is a decision, not a convenience.
//
//  WHAT THIS DOES NOT ESTABLISH
//  ----------------------------
//  This has never run. It has never been kextloaded, never been matched, and
//  the only Intel silicon this project has ever seen is one Linux boot on
//  2026-08-16 (igpu-results/tgl-capture-2026-08-16-022556.txt), which was a
//  sysfs/MMIO capture rather than anything this class produced. Read every
//  IORegistry property this publishes as "what the decode headers say about the
//  words we read", not as a claim that the inheritance path works.
//

#ifndef INTEL_DISPLAY_PROBE_HPP
#define INTEL_DISPLAY_PROBE_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "IntelGpuId.hpp"
#include "IntelDisplay.hpp"
#include "IntelFramebuffer.hpp"
#include "IntelPlaneLiveObserve.hpp"

//
// The scanout descriptor -- base, stride, byte EXTENT, and whether that extent
// fits the memory this driver actually mapped. Included DIRECTLY even though
// IntelDisplayNames.hpp below also pulls it in for the IG_SCAN_ verdict names:
// a translation unit that uses a header should say so, and handoff19 SS2 is what
// the alternative costs. tests/kextshim/ once declared IODeviceMemory inside
// IOMemoryDescriptor.h and NV3050Probe.cpp reached it transitively for months --
// the shim being FLATTER than Apple's graph accepted a .cpp that omitted an
// include Apple requires, and that first appears on the operator's one build.
//
#include "IntelScanout.hpp"

//
// The verdict names, igTransName() and the IORegistry property-key composition.
//
// This shell composed those keys with snprintf until the names moved into a
// testable header, which is why <libkern/libkern.h> is no longer included here --
// snprintf is a kernel export that IOKit/IOLib.h does not declare, and clang
// caught its absence when this file was first parsed. igComposeKey() replaced it
// for a stronger reason than availability: snprintf TRUNCATES on overflow and
// says so only in a return value, so a buffer sized slightly short publishes a
// key that looks deliberate and is wrong. igComposeKey() refuses and empties the
// buffer instead, and both halves of that are asserted on the host.
//
#include "IntelDisplayNames.hpp"

//
// The 3050 sink rehearsal (intel-handoff42): one sample of the firmware plane
// in, a sink plan and five plane words out, NONE of them written. Every number
// is composed in IntelSinkRehearse.hpp, which tests/test_intel_sink_rehearse.cpp
// executes on the host; this class only reads, calls, and publishes.
//
#include "IntelSinkRehearse.hpp"

//
// BAR0 is the MMIO/GTT aperture on Gen12, and BAR2 is the stolen-memory
// aperture. Every register this class reads is a BAR0 offset.
//
// This is NOT the same index convention as the NVIDIA side by coincidence --
// both happen to be 0 -- so it is spelled here rather than shared, because a
// constant shared between two vendors' address spaces is exactly the kind of
// thing that reads as correct while naming the wrong aperture.
//
#define IG_BAR0_INDEX               0

//
// The display block this track decodes ends at IG_DISPLAY_POLICY_LIMIT
// (IntelDisplay.hpp). A BAR shorter than that cannot carry the transcoder and
// plane pages, and the read path would return garbage rather than faulting.
//
// This is a WARNING threshold, not a refusal: a short BAR is worth saying out
// loud, but declining to attach would delete the IORegistry node that makes the
// problem inspectable in the first place.
//
#define IG_BAR0_EXPECTED_LENGTH     0x00080000u

//
// Transcoder/pipe A is the only one the target laptop's eDP panel uses, and
// the 2026-08-16 capture read 0x00000000 out of every other transcoder page.
// Read that as UNPROGRAMMED rather than ABSENT: unused display pages on this
// silicon read as zero, not all-ones, so "transcoder B is absent" is a claim
// this project cannot make from that capture.
//
// The scan below therefore walks ALL FOUR transcoders and reports each one's
// verdict, rather than assuming A. A driver that hardcoded A would be right on
// this machine and silently wrong on any other.
//
#define IG_TRANS_SCAN_COUNT         4u

//
// The pipe the sink rehearsal samples, and the BAR whose LENGTH bounds the plan.
//
// Pipe A for IntelFbPresentProbe.hpp's reason: it is the only pipe an eDP-only
// machine can have programmed, and a rehearsal per pipe would publish three
// PLANE_DISABLED refusals per boot that mean nothing.
//
// BAR2 is the graphics aperture. Only its LENGTH is read, never mapped: it is
// the one bound on the sink this class can measure rather than assume, and a
// sink inside it is reachable through it -- which is what a peer write from the
// 3050 would target. It is NOT the GGTT size (intel-handoff41 section 5 item
// 2), and the published keys say Aperture, not Ggtt.
//
#define IG_SINK_REHEARSE_PIPE           IG_TRANSCODER_A
#define IG_SINK_REHEARSE_APERTURE_INDEX 2

class IntelDisplayProbe : public IOService
{
    OSDeclareDefaultStructors(IntelDisplayProbe)

    typedef IOService super;

public:
    virtual bool       init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void       free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool       start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void       stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice        *fPCIDevice;
    IOMemoryMap        *fBar0Map;
    volatile UInt8     *fBar0Base;

    //
    // The LENGTH of that mapping, in bytes, and the fourth input to
    // igReadDecision(). It is not derivable from anything else this class
    // holds: IG_BAR0_EXPECTED_LENGTH is what the display block NEEDS and
    // IG_DISPLAY_POLICY_LIMIT is the largest offset the decode headers can
    // PRODUCE -- neither is what the OS actually mapped, and mapBar0() attaches
    // to a short BAR on purpose.
    //
    // Zero when unmapped, which is the safe direction: igReadDecision() then
    // refuses every offset rather than trusting a stale bound.
    //
    IGU64               fBar0Length;

    //
    // Cached from config space in probe() so start() does not re-read it. The
    // value is only ever passed to IntelGpuId.hpp predicates; nothing here
    // decides anything from a device ID by open-coded comparison.
    //
    UInt32              fDeviceId;

    bool   mapBar0(void);
    void   unmapBar0(void);
    UInt32 regRead32(UInt32 offset);

    void   publishIdentity(void);
    void   scanTranscoders(void);
    void   scanPlanes(void);

    void   publishTranscoder(UInt32 trans);
    void   publishPlane(UInt32 pipe);
    void   publishPlaneLiveObservation(UInt32 pipe);

    void   publishSinkRehearsal(void);
};

#endif // INTEL_DISPLAY_PROBE_HPP