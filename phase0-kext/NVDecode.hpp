//
//  NVDecode.hpp
//  Pure register constants and bit-field decoding for NVIDIA GPUs.
//
//  DELIBERATELY DEPENDENCY-FREE. No includes, no IOKit, no libc. Every type is
//  a plain builtin. This is what lets the same code be compiled two ways:
//
//    1. Inside the kext, by Apple clang with -mkernel -fapple-kext
//    2. Inside tests/, by MSVC on Windows, where it can actually be RUN
//
//  The second one matters. This project has shipped two wrong register decodes
//  from hand-transcription (the split ARCHITECTURE field in NV_PMC_BOOT_0, and
//  GA107's implementation ID), and both were caught by luck. A kext reading the
//  wrong bits does not crash -- it returns a plausible number and sends you
//  debugging the wrong thing. Anything that can be checked on a machine without
//  a GPU should be.
//
//  Register definitions transcribed from NVIDIA/open-gpu-kernel-modules
//  v610.57.04, src/common/inc/swref/published/ (SPDX: MIT).
//  Verified by tools/check-registers.py.
//

#ifndef NVDecode_hpp
#define NVDecode_hpp

//
// 'unsigned' is 32-bit on every target this code will ever see (LP64 macOS
// x86_64 and LLP64 Windows x64). Using it rather than uint32_t keeps this
// header free of <stdint.h>, which is awkward in kernel builds.
//
typedef unsigned int NVU32;

//
// ---------------------------------------------------------------------------
// NV_PMC_BOOT_0 -- nv_ref.h:108
//
// Readable immediately after PCI memory space is enabled: no clocks, no
// firmware, no init sequence. That is what makes it the correct first read.
//
// The architecture field is SPLIT across two non-adjacent bit ranges:
//
//   ARCHITECTURE_0  28:24  (low 5 bits)
//   ARCHITECTURE_1    8:8  (high bit)
//
//   arch = (ARCHITECTURE_1 << 5) | ARCHITECTURE_0
//
// This mirrors decodePmcBoot0Architecture() in nv_ref.h:213. Reading only
// 28:24 happens to work for Ampere (bit 8 is 0), but is wrong in general and
// would silently misidentify future parts.
// ---------------------------------------------------------------------------
//
#define NV_PMC_BOOT_0                            0x00000000

#define NV_PMC_BOOT_0_MINOR_REVISION_SHIFT       0       // 3:0
#define NV_PMC_BOOT_0_MINOR_REVISION_MASK        0x0F
#define NV_PMC_BOOT_0_MAJOR_REVISION_SHIFT       4       // 7:4
#define NV_PMC_BOOT_0_MAJOR_REVISION_MASK        0x0F
#define NV_PMC_BOOT_0_ARCHITECTURE_1_SHIFT       8       // 8:8
#define NV_PMC_BOOT_0_ARCHITECTURE_1_MASK        0x01
#define NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT       20      // 23:20
#define NV_PMC_BOOT_0_IMPLEMENTATION_MASK        0x0F
#define NV_PMC_BOOT_0_ARCHITECTURE_0_SHIFT       24      // 28:24
#define NV_PMC_BOOT_0_ARCHITECTURE_0_MASK        0x1F
#define NV_PMC_BOOT_0_ARCHITECTURE_0_WIDTH       5

//
// ---------------------------------------------------------------------------
// NV_PMC_BOOT_42 -- nv_ref.h:150
//
// The modern, authoritative chip identity register. BOOT_0 is kept for
// backwards compatibility; NVIDIA's own HAL routing reads BOOT_42. It also
// yields CHIP_ID directly, which is the (architecture << 4 | implementation)
// pair used everywhere else in the RM sources.
// ---------------------------------------------------------------------------
//
#define NV_PMC_BOOT_42                           0x00000A00

#define NV_PMC_BOOT_42_MINOR_EXT_REVISION_SHIFT  8       // 11:8
#define NV_PMC_BOOT_42_MINOR_EXT_REVISION_MASK   0x0F
#define NV_PMC_BOOT_42_MINOR_REVISION_SHIFT      12      // 15:12
#define NV_PMC_BOOT_42_MINOR_REVISION_MASK       0x0F
#define NV_PMC_BOOT_42_MAJOR_REVISION_SHIFT      16      // 19:16
#define NV_PMC_BOOT_42_MAJOR_REVISION_MASK       0x0F
#define NV_PMC_BOOT_42_IMPLEMENTATION_SHIFT      20      // 23:20
#define NV_PMC_BOOT_42_IMPLEMENTATION_MASK       0x0F
#define NV_PMC_BOOT_42_ARCHITECTURE_SHIFT        24      // 29:24
#define NV_PMC_BOOT_42_ARCHITECTURE_MASK         0x3F
#define NV_PMC_BOOT_42_CHIP_ID_SHIFT             20      // 29:20
#define NV_PMC_BOOT_42_CHIP_ID_MASK              0x3FF

//
// Architecture IDs -- nv_ref.h:130
//
#define NV_ARCHITECTURE_TU100                    0x16    // Turing
#define NV_ARCHITECTURE_GA100                    0x17    // Ampere
#define NV_ARCHITECTURE_GH100                    0x18    // Hopper
#define NV_ARCHITECTURE_AD100                    0x19    // Ada
#define NV_ARCHITECTURE_GB100                    0x1A    // Blackwell
#define NV_ARCHITECTURE_GB200                    0x1B
#define NV_ARCHITECTURE_GR100                    0x1C

//
// Implementation IDs within Ampere -- nv_arch.h:100 / ctrl2080mc.h:116.
// The mobile RTX 3050 is GA107.
//
#define NV_IMPLEMENTATION_GA100                  0x00
#define NV_IMPLEMENTATION_GA102                  0x02
#define NV_IMPLEMENTATION_GA103                  0x03
#define NV_IMPLEMENTATION_GA104                  0x04
#define NV_IMPLEMENTATION_GA106                  0x06
#define NV_IMPLEMENTATION_GA107                  0x07

//
// Composite chip ID = (architecture << 4) | implementation, matching
// NV_PMC_BOOT_42_CHIP_ID_* in nv_ref.h:188.
//
#define NV_CHIP_ID(arch, impl)                   (((arch) << 4) | (impl))
#define NV_CHIP_ID_GA107                         NV_CHIP_ID(NV_ARCHITECTURE_GA100, NV_IMPLEMENTATION_GA107)  // 0x177
#define NV_CHIP_ID_GA106                         NV_CHIP_ID(NV_ARCHITECTURE_GA100, NV_IMPLEMENTATION_GA106)  // 0x176

//
// ---------------------------------------------------------------------------
// Phase 3.5 -- GC6 island AON secure scratch registers.
//
// ampere/ga102/dev_gc6_island.h:33
//   NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(i) = 0x00118234 + i*4
// ampere/ga102/dev_gc6_island_addendum.h:31
//   GFW_BOOT       = GROUP_05(0)
//   GFW_BOOT_PROGRESS            7:0
//   GFW_BOOT_PROGRESS_COMPLETED  0xFF
//
// GFW is the VBIOS-resident firmware that runs before the host is permitted to
// touch the GPU. NVIDIA's own kgspWaitForGfwBootOk_TU102() polls exactly this
// field. Reading 0xFF proves the card's firmware came up healthy under macOS's
// PCI initialisation -- something NV_PMC_BOOT_0 cannot tell you, since BOOT_0
// responds even on a GPU whose firmware never started.
// ---------------------------------------------------------------------------
//
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(i)   (0x00118234 + (i) * 4)
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT \
            NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(0)

#define NV_GFW_BOOT_PROGRESS_SHIFT               0       // 7:0
#define NV_GFW_BOOT_PROGRESS_MASK                0xFF
#define NV_GFW_BOOT_PROGRESS_COMPLETED           0xFF

//
// ampere/ga102/dev_gc6_island.h:34 + dev_gc6_island_addendum.h:33
//
// Usable framebuffer size in megabytes, published by firmware into scratch
// group 42. A plausible value (4096 for a 4 GB mobile 3050) is a second
// independent confirmation that firmware ran and initialised memory.
//
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_42      0x001183A4
#define NV_USABLE_FB_SIZE_IN_MB                  NV_PGC6_AON_SECURE_SCRATCH_GROUP_42
#define NV_USABLE_FB_SIZE_IN_MB_VALUE_MASK       0xFFFFFFFF

//
// Plausibility bounds for the framebuffer size. A mobile 3050 ships 4 GB or
// 6 GB; anything outside this window means the field was misread rather than
// the card having an exotic configuration.
//
#define NV_FB_SIZE_MIN_MB                        256
#define NV_FB_SIZE_MAX_MB                        (128 * 1024)

//
// GFW boot can legitimately still be in progress when the kext loads early in
// boot. 4 seconds at 10 ms intervals is well past what real hardware needs.
//
#define NV_GFW_BOOT_POLL_INTERVAL_MS             10
#define NV_GFW_BOOT_POLL_TIMEOUT_MS              4000

//
// BAR0 is the 16 MiB register aperture on all Turing+ parts. Note the GC6
// island registers above live at ~0x118xxx, comfortably inside it -- if the
// aperture maps short, those reads fail before the identity reads do.
//
#define NV_BAR0_INDEX                            0
#define NV_BAR0_EXPECTED_LENGTH                  0x01000000


// ===========================================================================
// Decoding. All pure functions of a register value -- no hardware, no state.
// ===========================================================================

//
// An all-ones read means the device did not respond (BAR unmapped, device in
// D3, memory space decoder off). An all-zeroes read means the mapping
// succeeded but points at nothing, which in practice is the wrong BAR index.
// Both are failures, but they point at different bugs.
//
static inline bool nvLooksLikeDeadRead(NVU32 value)
{
    return (value == 0xFFFFFFFFu) || (value == 0x00000000u);
}

//
// nv_ref.h:213 decodePmcBoot0Architecture(). Reassembles the split field.
//
static inline NVU32 nvBoot0Architecture(NVU32 boot0)
{
    NVU32 lo = (boot0 >> NV_PMC_BOOT_0_ARCHITECTURE_0_SHIFT) & NV_PMC_BOOT_0_ARCHITECTURE_0_MASK;
    NVU32 hi = (boot0 >> NV_PMC_BOOT_0_ARCHITECTURE_1_SHIFT) & NV_PMC_BOOT_0_ARCHITECTURE_1_MASK;

    return (hi << NV_PMC_BOOT_0_ARCHITECTURE_0_WIDTH) | lo;
}

static inline NVU32 nvBoot0Implementation(NVU32 boot0)
{
    return (boot0 >> NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT) & NV_PMC_BOOT_0_IMPLEMENTATION_MASK;
}

static inline NVU32 nvBoot0MajorRevision(NVU32 boot0)
{
    return (boot0 >> NV_PMC_BOOT_0_MAJOR_REVISION_SHIFT) & NV_PMC_BOOT_0_MAJOR_REVISION_MASK;
}

static inline NVU32 nvBoot0MinorRevision(NVU32 boot0)
{
    return (boot0 >> NV_PMC_BOOT_0_MINOR_REVISION_SHIFT) & NV_PMC_BOOT_0_MINOR_REVISION_MASK;
}

static inline NVU32 nvBoot42Architecture(NVU32 boot42)
{
    return (boot42 >> NV_PMC_BOOT_42_ARCHITECTURE_SHIFT) & NV_PMC_BOOT_42_ARCHITECTURE_MASK;
}

static inline NVU32 nvBoot42Implementation(NVU32 boot42)
{
    return (boot42 >> NV_PMC_BOOT_42_IMPLEMENTATION_SHIFT) & NV_PMC_BOOT_42_IMPLEMENTATION_MASK;
}

static inline NVU32 nvBoot42ChipId(NVU32 boot42)
{
    return (boot42 >> NV_PMC_BOOT_42_CHIP_ID_SHIFT) & NV_PMC_BOOT_42_CHIP_ID_MASK;
}

static inline NVU32 nvBoot42MajorRevision(NVU32 boot42)
{
    return (boot42 >> NV_PMC_BOOT_42_MAJOR_REVISION_SHIFT) & NV_PMC_BOOT_42_MAJOR_REVISION_MASK;
}

static inline NVU32 nvBoot42MinorRevision(NVU32 boot42)
{
    return (boot42 >> NV_PMC_BOOT_42_MINOR_REVISION_SHIFT) & NV_PMC_BOOT_42_MINOR_REVISION_MASK;
}

static inline NVU32 nvBoot42MinorExtRevision(NVU32 boot42)
{
    return (boot42 >> NV_PMC_BOOT_42_MINOR_EXT_REVISION_SHIFT) & NV_PMC_BOOT_42_MINOR_EXT_REVISION_MASK;
}

static inline NVU32 nvGfwBootProgress(NVU32 scratch05_0)
{
    return (scratch05_0 >> NV_GFW_BOOT_PROGRESS_SHIFT) & NV_GFW_BOOT_PROGRESS_MASK;
}

static inline bool nvGfwBootComplete(NVU32 scratch05_0)
{
    return nvGfwBootProgress(scratch05_0) == NV_GFW_BOOT_PROGRESS_COMPLETED;
}

static inline bool nvFbSizeIsPlausible(NVU32 megabytes)
{
    return (megabytes >= NV_FB_SIZE_MIN_MB) && (megabytes <= NV_FB_SIZE_MAX_MB);
}

//
// Verdict over ONE read of NV_USABLE_FB_SIZE_IN_MB.
//
// The framebuffer size is a MEASUREMENT, so the driver has to be able to say
// "not measured" as precisely as it says "4096 MB". Four things can be true of
// that word and only one of them is a size:
//
//   * the read was never attempted, because GFW boot did not complete;
//   * the offset did not decode and regRead32() answered all-ones;
//   * firmware has not published the field yet, so it reads zero;
//   * it decodes to a size no mobile 3050 ships, so it is a misread.
//
// Why this is a verdict rather than the bool the caller used to get:
// NV_USABLE_FB_SIZE_IN_MB_VALUE_MASK is 0xFFFFFFFF, so a REFUSED read masks to
// 4294967295 rather than to something obviously wrong -- and 4294967295 is
// greater than zero. A publisher testing the MAGNITUDE of the size to decide
// whether to publish it therefore publishes a refusal as a measurement, and
// setProperty() with a fabricated word is byte-identical in the IORegistry to
// setProperty() with a real one. That is handoff18 s1d's class, and the bool
// could not carry the distinction because `false` is satisfied by all three
// refusals at once.
//
// NV_FB_SIZE_NOT_ATTEMPTED is deliberately the ZERO value, the same convention
// NVChipVerdict uses: a verdict variable that was never assigned then reads as
// "not measured" rather than as a good size. nvFbSizeVerdict() never returns it
// -- it is the caller's initialiser, and it is what the !gfwOk path publishes,
// because "nobody looked" and "the offset did not decode" are different facts
// and only the second is a defect somebody can act on.
//
// GFW boot poll verdict.
//
// Why this is a verdict rather than the bool the caller used to get:
// pollGfwBoot() refuses for two categorically DIFFERENT reasons and returned
// one false for both. NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(0) reading all-ones
// while the identity registers responded means the aperture is live and THIS
// OFFSET is not decoding -- a defect in this driver's own BAR0 map, the one
// somebody can go and fix. A progress field that decoded and simply never
// reached 0xFF inside the timeout is a fact about the CARD instead.
//
// The sharper half is the out-param. The refusal arm wrote *outProgress = 0
// before returning, and a literal zero is byte-identical in the IORegistry to a
// card whose GFW genuinely sits at step 0 -- handoff18 section 1d's
// fabricated-publication class. So the progress word may only be published
// behind nvGfwBootProgressWasRead(), and the refusal arm writes nothing at all.
//
// NV_GFW_BOOT_NOT_ATTEMPTED is the enum's zero value on purpose, so a caller's
// initialiser reads as "nobody looked" rather than as a measurement.
//
typedef enum {
    NV_GFW_BOOT_NOT_ATTEMPTED = 0,
    NV_GFW_BOOT_OK,
    NV_GFW_BOOT_UNREADABLE,
    NV_GFW_BOOT_TIMEOUT,

    NV_GFW_BOOT_VERDICT_LIMIT
} NVGfwBootVerdict;

static inline const char *nvGfwBootVerdictName(NVGfwBootVerdict v)
{
    switch (v) {
        case NV_GFW_BOOT_NOT_ATTEMPTED: return "not attempted -- identity read did not respond";
        case NV_GFW_BOOT_OK:            return "completed";
        case NV_GFW_BOOT_UNREADABLE:    return "offset not decoding -- BAR0 map may be short";
        case NV_GFW_BOOT_TIMEOUT:       return "timed out before GFW reported completion";
        default:                        return "unrecognised GFW boot verdict";
    }
}

//
// An EQUALITY against the accepting verdict, never a negation of an error arm.
// A != NV_GFW_BOOT_UNREADABLE form is TRUE for NOT_ATTEMPTED and for TIMEOUT,
// so it would report a card that never finished booting as healthy.
//
static inline bool nvGfwBootVerdictIsOk(NVGfwBootVerdict v)
{
    return v == NV_GFW_BOOT_OK;
}

//
// Whether the verdict implies the progress field was actually READ. OK and
// TIMEOUT both decoded a real word out of the register; UNREADABLE and
// NOT_ATTEMPTED did not, and on those two arms the progress out-param holds an
// initialiser rather than a measurement. This is the predicate a publisher must
// consult before putting the progress word in the IORegistry -- the same shape
// as nvGspBootWasRead() one subsystem over.
//
static inline bool nvGfwBootProgressWasRead(NVGfwBootVerdict v)
{
    return v == NV_GFW_BOOT_OK || v == NV_GFW_BOOT_TIMEOUT;
}

//
typedef enum {
    NV_FB_SIZE_NOT_ATTEMPTED = 0,
    NV_FB_SIZE_OK,
    NV_FB_SIZE_UNREADABLE,
    NV_FB_SIZE_UNPUBLISHED,
    NV_FB_SIZE_IMPLAUSIBLE,

    NV_FB_SIZE_VERDICT_LIMIT
} NVFbSizeVerdict;

static inline const char *nvFbSizeVerdictName(NVFbSizeVerdict v)
{
    switch (v) {
        case NV_FB_SIZE_NOT_ATTEMPTED: return "not attempted -- GFW boot did not complete";
        case NV_FB_SIZE_OK:            return "measured";
        case NV_FB_SIZE_UNREADABLE:    return "unreadable -- the offset did not decode";
        case NV_FB_SIZE_UNPUBLISHED:   return "not published by firmware yet";
        case NV_FB_SIZE_IMPLAUSIBLE:   return "implausible -- treated as a misread";
        default:                       return "invalid verdict";
    }
}

//
// Arm ORDER is load-bearing and is the dead-arm shape handoff32 s3 and
// handoff33 s2 each removed one subsystem over:
//
//   * all-ones is tested FIRST, because 0xFFFFFFFF is ALSO implausible. An
//     implausibility-first order makes NV_FB_SIZE_UNREADABLE unreachable, and
//     an unreachable arm is indistinguishable from a deleted one.
//   * zero is tested before plausibility for the same reason: zero is below
//     NV_FB_SIZE_MIN_MB, so it satisfies the implausible arm too, and
//     "firmware has not published it yet" is a normal early-boot state rather
//     than a misread.
//
static inline NVFbSizeVerdict nvFbSizeVerdict(NVU32 raw)
{
    NVU32 megabytes = raw & NV_USABLE_FB_SIZE_IN_MB_VALUE_MASK;

    if (raw == 0xFFFFFFFFu)
        return NV_FB_SIZE_UNREADABLE;

    if (megabytes == 0)
        return NV_FB_SIZE_UNPUBLISHED;

    if (!nvFbSizeIsPlausible(megabytes))
        return NV_FB_SIZE_IMPLAUSIBLE;

    return NV_FB_SIZE_OK;
}

static inline const char *nvArchitectureName(NVU32 arch)
{
    switch (arch) {
        case NV_ARCHITECTURE_TU100: return "Turing";
        case NV_ARCHITECTURE_GA100: return "Ampere";
        case NV_ARCHITECTURE_GH100: return "Hopper";
        case NV_ARCHITECTURE_AD100: return "Ada";
        case NV_ARCHITECTURE_GB100: return "Blackwell";
        case NV_ARCHITECTURE_GB200: return "Blackwell 2";
        case NV_ARCHITECTURE_GR100: return "Rubin";
        default:                    return "unknown";
    }
}

static inline const char *nvAmpereImplementationName(NVU32 impl)
{
    switch (impl) {
        case NV_IMPLEMENTATION_GA100: return "GA100";
        case NV_IMPLEMENTATION_GA102: return "GA102";
        case NV_IMPLEMENTATION_GA103: return "GA103";
        case NV_IMPLEMENTATION_GA104: return "GA104";
        case NV_IMPLEMENTATION_GA106: return "GA106";
        case NV_IMPLEMENTATION_GA107: return "GA107";
        default:                      return "unrecognised Ampere part";
    }
}

//
// The four findings decodeBoot0() logs, as a value a caller can act on.
//
// decodeBoot0() has always detected these cases and has always written them to
// the LOG only: a non-Ampere architecture, an Ada part specifically (the
// 'RTX 3050 A' case, where the whole plan retargets), GA107 confirmed, and an
// Ampere part that is not GA107. A log line is not a consumable answer --
// publishIdentity() published a NAME derived from the same word and nothing
// saying whether the part is the one this driver was written for, so "Ada"
// reached the IORegistry byte-identically to "Ampere".
//
// Order is load-bearing. NV_PART_ADA is a SUBSET of "not Ampere", so a version
// testing the general case first would make the Ada arm unreachable -- and that
// is the one finding of the four with an operator action attached to it.
//
typedef enum {
    NV_PART_TARGET = 0,         // Ampere GA107 -- the mobile RTX 3050 die
    NV_PART_AMPERE_OTHER,       // Ampere, but not GA107
    NV_PART_ADA,                // Ada silicon; see DEVICE-IDS.md, plan retargets
    NV_PART_OTHER_ARCHITECTURE, // neither Ampere nor Ada

    NV_PART_VERDICT_LIMIT
} NVPartVerdict;

static inline const char *nvPartVerdictName(NVPartVerdict v)
{
    switch (v) {
        case NV_PART_TARGET:             return "GA107 -- target part";
        case NV_PART_AMPERE_OTHER:       return "Ampere, not GA107";
        case NV_PART_ADA:                return "Ada -- not Ampere, plan retargets";
        case NV_PART_OTHER_ARCHITECTURE: return "not Ampere";
        default:                         return "unrecognised part verdict";
    }
}

static inline NVPartVerdict nvPartVerdict(NVU32 boot0)
{
    NVU32 arch = nvBoot0Architecture(boot0);
    NVU32 impl = nvBoot0Implementation(boot0);

    if (arch == NV_ARCHITECTURE_GA100) {
        return (impl == NV_IMPLEMENTATION_GA107) ? NV_PART_TARGET
                                                 : NV_PART_AMPERE_OTHER;
    }

    if (arch == NV_ARCHITECTURE_AD100) {
        return NV_PART_ADA;
    }

    return NV_PART_OTHER_ARCHITECTURE;
}

//
// The four findings decodeBoot42() logs, as a value a caller can act on.
//
// This is nvPartVerdict()'s sibling one register over, and it exists for the
// same reason. decodeBoot42() has always detected four cases and has always
// written all four to the LOG only: an unexpected dead read (BOOT_0 answered,
// BOOT_42 did not), the composite chip ID matching GA107, the chip ID not
// matching, and the not-matching value being GA106 SPECIFICALLY -- the
// 'RTX 3050 Ti Laptop GPU', same architecture but a different die with
// different unit counts, which is the near miss most likely to be mistaken for
// the target part. publishIdentity() published NV_PMC_BOOT_42 as a raw word and
// NVChipIdExpected as a statement of what this driver wants, and never compared
// the two -- so the IORegistry left "is this the die the driver was written
// for?" to whoever happened to be reading it.
//
// Order is load-bearing in two places, and only the second is a SUBSET
// relation -- the distinction was measured rather than assumed:
//
//   * The dead-read test runs before the general FALL-THROUGH. A dead read is
//     not a special kind of mismatch, but it decodes as one: the CHIP_ID field
//     is ten bits, so all-ones yields 0x3FF and all-zeroes yields 0x000, and
//     neither is a chip ID this tree names. Drop the dead-read test and both
//     land on NV_CHIP_OTHER -- "wrong die", a statement about silicon, made
//     over a register nobody successfully read.
//
//     It is worth being exact about what this does NOT depend on, because a
//     mutation measured 2026-08-19 reddened nothing: moving the dead-read test
//     below the two EQUALITY arms is behaviour-preserving, because 0x3FF and
//     0x000 cannot equal GA107 (0x177) or GA106 (0x176). Only the position
//     relative to the fall-through is load-bearing, and the suite pins the
//     non-collision so a future chip ID landing on either value reddens.
//   * GA106 is a SUBSET of "not GA107", so the sibling test must sit INSIDE the
//     mismatch case rather than beside it. Beside it, whichever test is written
//     first wins and the other arm is dead.
//
// NV_CHIP_UNREADABLE is deliberately the ZERO value, so a verdict variable that
// was never assigned reads as "not measured" rather than as the target part.
//
typedef enum {
    NV_CHIP_UNREADABLE = 0,     // dead read -- no chip ID was measured
    NV_CHIP_TARGET,             // composite chip ID is GA107
    NV_CHIP_SIBLING_GA106,      // GA106 -- 'RTX 3050 Ti', a different die
    NV_CHIP_OTHER,              // some other chip ID entirely
    NV_CHIP_VERDICT_LIMIT
} NVChipVerdict;

static inline const char *nvChipVerdictName(NVChipVerdict v)
{
    switch (v) {
        case NV_CHIP_UNREADABLE:    return "unreadable -- BOOT_42 was a dead read";
        case NV_CHIP_TARGET:        return "GA107 -- target die";
        case NV_CHIP_SIBLING_GA106: return "GA106 -- 'RTX 3050 Ti', not this die";
        case NV_CHIP_OTHER:         return "chip ID does not match GA107";
        default:                    return "unrecognised chip verdict";
    }
}

static inline NVChipVerdict nvChipVerdict(NVU32 boot42)
{
    if (nvLooksLikeDeadRead(boot42)) {
        return NV_CHIP_UNREADABLE;
    }

    NVU32 chipId = nvBoot42ChipId(boot42);

    if (chipId == NV_CHIP_ID_GA107) {
        return NV_CHIP_TARGET;
    }

    if (chipId == NV_CHIP_ID_GA106) {
        return NV_CHIP_SIBLING_GA106;
    }

    return NV_CHIP_OTHER;
}

#endif /* NVDecode_hpp */
