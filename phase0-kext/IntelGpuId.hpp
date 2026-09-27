//
//  IntelGpuId.hpp
//  Gen12 (Tiger Lake) PCI device identification.
//
//  Same rule as IntelDisplay.hpp and the five NVIDIA headers: DEPENDENCY-FREE.
//  No includes, no IOKit, no kernel types, no allocation, no sleep. Every
//  function is a total function of its arguments, so tests/test_intel_gpu_id.cpp
//  compiles this IDENTICAL file under MSVC and runs it on a machine with no
//  Intel GPU and no macOS.
//
//  WHY THIS IS A SEPARATE FILE FROM IntelDisplay.hpp
//
//  IntelDisplay.hpp answers "given a transcoder or a plane, what offset do I
//  touch, and what does the word there mean". It says nothing about WHICH
//  device those offsets are valid for -- it is addressing arithmetic that would
//  happily compute a Gen12 plane offset on a Gen9 part and hand back a number
//  that is wrong in a way no decode can detect. This file is the gate in front
//  of that: it answers "is the part in this PCI slot one whose display engine
//  IntelDisplay.hpp actually describes".
//
//  That split matters because the two have different EVIDENCE. The offsets come
//  from vendored register headers; the device list comes from a vendored PCI ID
//  table. Mixing them in one file makes it easy to cite the wrong provenance for
//  a constant, which is the failure mode this project keeps rediscovering.
//
//  WHERE THE NUMBERS COME FROM
//
//  Every device ID below is transcribed from reference/i915/i915_pciids.h at
//  Linux v6.12 -- the INTEL_TGL_GT1_IDS and INTEL_TGL_GT2_IDS macros. Those
//  macros are the kernel's own definition of "this part is Tiger Lake", so the
//  list here is a transcription rather than a judgement. Nothing was added to
//  it because a machine happened to have it, and nothing was dropped from it
//  because no machine here has it.
//
//  ONE constant is a value witnessed on silicon rather than transcribed:
//  IG_DEVICE_OBSERVED_2026_08_16. It is 0x9A68, read from
//  igpu-results/tgl-capture-2026-08-16-022556.txt, which is the only artifact in
//  this project produced on the target laptop. It is deliberately defined
//  SEPARATELY from the GT1 table entry that carries the same value, and
//  igTglGt1Id() is never used to define it. Two independent statements that
//  agree are evidence; one statement quoted twice is not, and a test that read
//  the observed value back out of the vendor table would pass under any value at
//  all. tests/test_intel_gpu_id.cpp pins both to bare literals and then asserts
//  they agree -- so if either the transcription or the capture reading is wrong,
//  a row goes red rather than the two errors cancelling.
//
//  WHAT THIS FILE DOES NOT CLAIM
//
//  - It does not claim the eleven IDs are the complete set of Tiger Lake parts
//    in the world. It claims they are the complete set the vendored header
//    spells at v6.12, which is a checkable statement.
//  - It does not claim the display engine is identical across all eleven. GT1
//    and GT2 differ in execution units, not in the display block, but this file
//    keeps the two classes separable (igTglGtClass) rather than flattening them,
//    because a caller that later needs the distinction should not have to
//    re-derive it from a list this file already knows.
//  - It says nothing about the NVIDIA part. The target laptop has both; only
//    one of them has a panel wired to it.
//

#ifndef INTEL_GPU_ID_HPP
#define INTEL_GPU_ID_HPP

//
//  Deliberately NOT named IGU32. IntelDisplay.hpp already spells that, and a
//  second file redefining it works only by accident of the two being the same
//  underlying type. A distinct name keeps these two headers independently
//  includable in either order and in isolation.
//
typedef unsigned int IGID32;

//
//  Returned by every table accessor for an out-of-range index. 0xFFFFFFFF is
//  not a possible PCI vendor or device ID -- both are 16 bits -- so it can never
//  collide with a real answer. A PCI config read of an absent function also
//  returns all-ones, which makes this the same sentinel the hardware itself
//  uses.
//
#define IG_PCI_ID_INVALID               0xFFFFFFFFu

//
//  PCI vendor ID. Intel Corporation.
//
//  This is checked SEPARATELY from the device ID and both must pass. Device IDs
//  are only unique within a vendor, so a device-ID-only match is a real bug
//  rather than a pedantic one: 0x9A68 under some other vendor is some other
//  part, and programming Gen12 display offsets into it writes to whatever
//  happens to live at 0x60000 in its BAR.
//
#define IG_PCI_VENDOR_INTEL             0x8086u

//
//  ---- INTEL_TGL_GT1_IDS, reference/i915/i915_pciids.h ------------------------
//
//  Three entries, in the order the vendored header spells them.
//
#define IG_TGL_GT1_ID_0                 0x9A60u
#define IG_TGL_GT1_ID_1                 0x9A68u
#define IG_TGL_GT1_ID_2                 0x9A70u
#define IG_TGL_GT1_ID_COUNT             3u

//
//  ---- INTEL_TGL_GT2_IDS, reference/i915/i915_pciids.h ------------------------
//
//  Eight entries, in the order the vendored header spells them.
//
#define IG_TGL_GT2_ID_0                 0x9A40u
#define IG_TGL_GT2_ID_1                 0x9A49u
#define IG_TGL_GT2_ID_2                 0x9A59u
#define IG_TGL_GT2_ID_3                 0x9A78u
#define IG_TGL_GT2_ID_4                 0x9AC0u
#define IG_TGL_GT2_ID_5                 0x9AC9u
#define IG_TGL_GT2_ID_6                 0x9AD9u
#define IG_TGL_GT2_ID_7                 0x9AF8u
#define IG_TGL_GT2_ID_COUNT             8u

//
//  INTEL_TGL_IDS is the concatenation of the two, so this is 3 + 8. Spelled as
//  a literal 11 rather than as the sum, on purpose: if a table entry is ever
//  added and its COUNT bumped without the union count following, the sum form
//  would silently stay correct and the mismatch would surface only as a missing
//  device at match time. The literal makes tests/test_intel_gpu_id.cpp able to
//  assert the sum against it.
//
#define IG_TGL_ID_COUNT                 11u

//
//  GT class. NOT a bitfield and not an index -- 0 means "this device is not
//  Tiger Lake at all", which is a distinct answer from "it is Tiger Lake and I
//  do not know which GT". There is no third state, because the vendored header
//  has exactly two TGL macros.
//
#define IG_TGL_GT_NONE                  0u
#define IG_TGL_GT_1                     1u
#define IG_TGL_GT_2                     2u

//
//  ---- witnessed on the target laptop ----------------------------------------
//
//  igpu-results/tgl-capture-2026-08-16-022556.txt, header block, 20260816-023053
//  UTC:
//
//      pci      /sys/bus/pci/devices/0000:00:02.0
//      device   0x9a68
//      driver   i915
//
//  That boot cannot be repeated -- the machine is not running Linux any more --
//  so these four values are the entire first-hand record of what the part is
//  and where it sits.
//
//  The device ID duplicates IG_TGL_GT1_ID_1's value by coincidence of being the
//  same part; it does NOT duplicate its provenance. See the header comment.
//
#define IG_DEVICE_OBSERVED_2026_08_16   0x9A68u

//
//  0000:00:02.0 -- domain 0, bus 0, device 2, function 0. This is the
//  conventional location of an Intel integrated display controller and the
//  capture confirms it on this specific machine. Kept as three separate values
//  rather than one packed word because the packing convention differs between
//  Linux sysfs, IOKit and PCI config space, and a packed constant would silently
//  pick one.
//
#define IG_IGD_BUS_OBSERVED_2026_08_16      0x00u
#define IG_IGD_DEVICE_OBSERVED_2026_08_16   0x02u
#define IG_IGD_FUNCTION_OBSERVED_2026_08_16 0x00u


//
//  ---- table accessors --------------------------------------------------------
//
//  Switches rather than arithmetic. The IDs are not a stride -- 0x9A60, 0x9A68,
//  0x9A70 looks like a stride of 8 right up until the GT2 list, where 0x9A40,
//  0x9A49, 0x9A59 is not a stride of anything. Any formula fitted to part of
//  this list produces plausible wrong IDs for the rest, which is exactly the
//  defect IntelDisplay.hpp's transcoder switch exists to avoid on the addressing
//  side.
//

static inline IGID32 igTglGt1Id(IGID32 index)
{
    switch (index) {
    case 0u: return IG_TGL_GT1_ID_0;
    case 1u: return IG_TGL_GT1_ID_1;
    case 2u: return IG_TGL_GT1_ID_2;
    default: return IG_PCI_ID_INVALID;
    }
}

static inline IGID32 igTglGt2Id(IGID32 index)
{
    switch (index) {
    case 0u: return IG_TGL_GT2_ID_0;
    case 1u: return IG_TGL_GT2_ID_1;
    case 2u: return IG_TGL_GT2_ID_2;
    case 3u: return IG_TGL_GT2_ID_3;
    case 4u: return IG_TGL_GT2_ID_4;
    case 5u: return IG_TGL_GT2_ID_5;
    case 6u: return IG_TGL_GT2_ID_6;
    case 7u: return IG_TGL_GT2_ID_7;
    default: return IG_PCI_ID_INVALID;
    }
}

//
//  INTEL_TGL_IDS order: the whole GT1 list, then the whole GT2 list. A caller
//  enumerating 0..IG_TGL_ID_COUNT-1 sees every supported part exactly once.
//
static inline IGID32 igTglId(IGID32 index)
{
    if (index < IG_TGL_GT1_ID_COUNT)
        return igTglGt1Id(index);
    if (index < IG_TGL_ID_COUNT)
        return igTglGt2Id(index - IG_TGL_GT1_ID_COUNT);
    return IG_PCI_ID_INVALID;
}


//
//  ---- membership -------------------------------------------------------------
//

static inline bool igIsTglGt1(IGID32 device)
{
    return device == IG_TGL_GT1_ID_0
        || device == IG_TGL_GT1_ID_1
        || device == IG_TGL_GT1_ID_2;
}

static inline bool igIsTglGt2(IGID32 device)
{
    return device == IG_TGL_GT2_ID_0
        || device == IG_TGL_GT2_ID_1
        || device == IG_TGL_GT2_ID_2
        || device == IG_TGL_GT2_ID_3
        || device == IG_TGL_GT2_ID_4
        || device == IG_TGL_GT2_ID_5
        || device == IG_TGL_GT2_ID_6
        || device == IG_TGL_GT2_ID_7;
}

static inline bool igIsTglDevice(IGID32 device)
{
    return igIsTglGt1(device) || igIsTglGt2(device);
}

//
//  IG_TGL_GT_NONE for anything not in either list. Callers that only want a
//  yes/no should use igIsTglDevice(); this exists so a caller that needs the
//  class does not re-implement the two lists to get it.
//
static inline IGID32 igTglGtClass(IGID32 device)
{
    if (igIsTglGt1(device))
        return IG_TGL_GT_1;
    if (igIsTglGt2(device))
        return IG_TGL_GT_2;
    return IG_TGL_GT_NONE;
}


//
//  ---- the actual match predicate ---------------------------------------------
//
//  The one function an IOKit probe should call. Vendor AND device, never device
//  alone.
//
//  It also refuses IG_PCI_ID_INVALID explicitly rather than relying on it simply
//  not being in the tables. An absent PCI function reads all-ones on both words,
//  so (0xFFFFFFFF, 0xFFFFFFFF) is the exact input a probe sees when it is
//  pointed at a slot with nothing in it -- the single most likely wrong input
//  this function will ever receive, and the one whose rejection should not be
//  incidental.
//
static inline bool igIsSupportedDevice(IGID32 vendor, IGID32 device)
{
    if (vendor == IG_PCI_ID_INVALID || device == IG_PCI_ID_INVALID)
        return false;
    if (vendor != IG_PCI_VENDOR_INTEL)
        return false;
    return igIsTglDevice(device);
}

//
//  Whether a PCI location is the one the capture recorded. Not a match
//  requirement -- a caller must NOT refuse a Tiger Lake part because it turned
//  up at a different BDF, since 00:02.0 is a convention rather than an
//  architectural guarantee. This exists so a probe can log "the part is where we
//  saw it" or "the part moved", which is a diagnostic, and so that the three
//  observed values are load-bearing rather than decorative.
//
static inline bool igIsObservedIgdLocation(IGID32 bus, IGID32 device, IGID32 function)
{
    return bus      == IG_IGD_BUS_OBSERVED_2026_08_16
        && device   == IG_IGD_DEVICE_OBSERVED_2026_08_16
        && function == IG_IGD_FUNCTION_OBSERVED_2026_08_16;
}

#endif  // INTEL_GPU_ID_HPP
