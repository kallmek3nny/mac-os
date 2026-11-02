//
//  IntelDisplay.hpp
//  Gen12 (Tiger Lake) display-engine addressing and timing decode.
//
//  Like NVDecode.hpp, NVFalcon.hpp, NVRiscvUcode.hpp, NVGspBoot.hpp and
//  NVGspStage.hpp, this file is DELIBERATELY DEPENDENCY-FREE: no includes, no
//  IOKit, no kernel types, no allocation, no sleep. Every function is a total
//  function of its arguments, so tests/ compiles the IDENTICAL file under MSVC
//  and runs it on a machine with no Intel GPU and no macOS.
//
//  WHY THIS FILE EXISTS
//
//  On 2026-08-15 18:38 UTC a BAR0 mmap on 0000:00:02.0 (8086:9a68, Tiger Lake
//  GT1) succeeded for the first time -- lockdown [none], Secure Boot off, i915
//  bound, panel lit. That capture is the first real Intel register state this
//  project has ever held, and it arrives as raw 32-bit words. Turning a word
//  into a mode is arithmetic, and arithmetic belongs somewhere it can be RUN
//  rather than somewhere it can only be read.
//
//  PROVENANCE OF EVERY NUMBER BELOW
//
//  Every offset, mask and encoding here was read out of the vendored Linux
//  v6.12 sources in reference/i915/ this session, and each is attributed at its
//  definition by file and line. Nothing is inferred from a stride, and nothing
//  is derived from the observed capture -- the capture is the thing these
//  constants will be used to READ, so deriving them from it would make the
//  decode unfalsifiable. See reference/i915/PROVENANCE.md.
//
//  Two groups of constants here are values witnessed on silicon rather than
//  offsets, masks or encodings transcribed from a header:
//
//    IG_PLANE_SIZE_OBSERVED_2026_08_16 -- one word from the first capture,
//    igpu-results/tgl-capture-2026-08-16-022556.txt.
//
//    The IG_OBSERVED_*_2026_08_18 block under VALUES WITNESSED ON SILICON
//    further down -- values from the second and final capture,
//    igpu-results/tgl-final-capture-2026-08-18-201412.txt.
//
//  Each carries its timestamp in its name, per the project rule. They are
//  data the decoders are CHECKED AGAINST, never inputs any of them is derived
//  from, so the paragraph above still holds. Anything else settled from a
//  live dump goes in under the same naming.
//
//  THE THREE HAZARDS THIS FILE IS SHAPED AROUND
//
//  1. TRANSCODER ADDRESSING IS A TABLE, NOT A STRIDE.
//     i915_reg.h defines TRANS_HTOTAL() via _MMIO_TRANS2, which indexes
//     trans_offsets[] -- a per-platform array. Tiger Lake's values live in
//     intel_display_device.c's XE_D_DISPLAY block. A 0x1000 stride happens to
//     reproduce A..D, and then fails: DSI1 sits 0x800 above DSI0, not 0x1000.
//     So igTransBase() is a switch over the table, never base + n * 0x1000.
//
//  2. TRANSCONF IS INDEXED BY TRANSCODER BUT LIVES IN THE PIPE BLOCK.
//     i915_reg.h:1698 defines TRANSCONF() as _MMIO_PIPE2, so it is
//     0x70008 + pipe delta while its argument is a transcoder. One combined
//     translate(base, offset) helper would put it at 0x60008 -- which is
//     _TRANS_HSYNC_A, a REAL register that reads and writes without faulting.
//     That is the Intel-side instance of the hazard nvGspFalconReg() and
//     nvGspRiscvReg() are kept as separate functions for, so the same remedy
//     is used: igTransTimingReg() and igTransConfReg() are separate, and
//     neither can be called with the other's base.
//
//  3. EVERY TIMING FIELD IS STORED AS value - 1.
//     intel_display.c:2741-2758 writes HACTIVE(hdisplay - 1),
//     HTOTAL(htotal - 1), HBLANK_START(... - 1), HSYNC_START(... - 1) and the
//     V equivalents; PIPESRC at 2814 writes width - 1 and height - 1; the
//     readback at 2911 adds 1 back. A decode that omits the +1 is off by
//     exactly one, and a mode still nearly works under that error -- which is
//     what makes it worth pinning in a test rather than assuming.
//
//  WHAT IS DELIBERATELY ABSENT
//
//  CDCLK_CTL. The capture holds a word for it, but the Gen12 divider encoding
//  was never verified against a header this session, so there is no decoder
//  here and no frequency is claimed anywhere. An offset with no verified
//  encoding is not a decode, and filing one next to these would destroy the
//  distinction the rest of the file maintains.
//

#ifndef INTEL_DISPLAY_HPP
#define INTEL_DISPLAY_HPP

//
// Same reasoning as NVDecode.hpp's NVU32: a plain builtin rather than uint32_t,
// because <cstdint> is not available to a -mkernel -fapple-kext translation
// unit and this header must parse identically under both toolchains. unsigned
// int is 32 bits on every ABI this project targets (Apple x86_64 and LLP64
// Windows x64).
//
typedef unsigned int IGU32;

//
// A 64-bit companion, used for exactly one thing: the LENGTH of a BAR mapping.
// unsigned long long is a builtin on both toolchains for the same reason IGU32
// is a plain unsigned int -- <cstdint> is not available to a -mkernel
// -fapple-kext translation unit.
//
// The length is deliberately NOT an IGU32. IOKit hands it over as an
// IOByteCount, which is 64 bits, and narrowing it here would be a silent
// truncation on the one input whose entire job is to bound a dereference. The
// failure is asymmetric: a 4 GiB mapping truncated to 0 refuses every read,
// which is merely wrong and loud, while a length of 0x1_0007_0000 truncated to
// 0x70000 ACCEPTS reads it should refuse -- and that direction scribbles.
//
typedef unsigned long long IGU64;

//
// Returned by every address helper for an input it cannot translate. A helper
// that returned 0 on failure would hand back a VALID MMIO offset -- 0x0 is
// GTHREAD registers on this device -- so the sentinel has to be a value that
// can never be a legal display offset.
//
#define IG_MMIO_INVALID                 0xFFFFFFFFu

//
// The width of a single MMIO access on this engine. It is named rather than
// spelled 4 inline because the mapping bound below needs it: an offset at
// (length - 1) is INSIDE the mapping by any naive `offset < length` test and
// still reads three bytes past the end of it.
//
#define IG_MMIO_ACCESS_BYTES            4u

//
// Transcoder indices. These are ORDINALS INTO THE TABLE, not addresses, and
// they exist so a caller cannot pass a raw offset where an index is wanted.
//
// intel_display_device.c's XE_D_DISPLAY block lists TRANSCODER_A..D plus
// TRANSCODER_DSI_0/1 and does NOT list TRANSCODER_EDP: Gen12 dropped the
// separate eDP transcoder, so the panel on this laptop runs on an ordinary
// transcoder through DDI A. That is why 0x6F000 read as unclaimed in the
// 18:38 capture -- a correct null, not a failed read.
//
#define IG_TRANSCODER_A                 0u
#define IG_TRANSCODER_B                 1u
#define IG_TRANSCODER_C                 2u
#define IG_TRANSCODER_D                 3u
#define IG_TRANSCODER_DSI0              4u
#define IG_TRANSCODER_DSI1              5u
#define IG_TRANSCODER_COUNT             6u

//
// Field offsets WITHIN a transcoder block. reference/i915/i915_reg.h:
//   1073  _TRANS_HTOTAL_A  0x60000
//   1078  _TRANS_HBLANK_A  0x60004
//   1083  _TRANS_HSYNC_A   0x60008
//   1088  _TRANS_VTOTAL_A  0x6000c
//   1093  _TRANS_VBLANK_A  0x60010
//   1098  _TRANS_VSYNC_A   0x60014
//   1104  _PIPEASRC        0x6001c
//
// PIPESRC is in this list rather than with TRANSCONF because i915_reg.h:1147
// defines PIPESRC() via _MMIO_TRANS2 -- despite the _PIPEA name it is indexed
// and addressed like a transcoder register. i915_reg.h:1120 confirms it:
// _PIPEBSRC is 0x6101c, in the TRANSCODER B block, not 0x7101c.
//
#define IG_TRANS_HTOTAL                 0x00u
#define IG_TRANS_HBLANK                 0x04u
#define IG_TRANS_HSYNC                  0x08u
#define IG_TRANS_VTOTAL                 0x0Cu
#define IG_TRANS_VBLANK                 0x10u
#define IG_TRANS_VSYNC                  0x14u
#define IG_TRANS_PIPESRC                0x1Cu

//
// TRANSCONF's offset within the PIPE block. i915_reg.h:1588 _TRANSACONF is
// 0x70008 and PIPE_A_OFFSET is 0x70000, so the delta is 0x08. Kept as its own
// constant rather than reusing IG_TRANS_HSYNC (also 0x08) even though the
// numbers coincide: the coincidence IS hazard 2, and two names make a later
// reader see two registers instead of one.
//
#define IG_PIPE_TRANSCONF               0x08u

//
// i915_reg.h:1589 TRANSCONF_ENABLE       REG_BIT(31)
// i915_reg.h:1591 TRANSCONF_STATE_ENABLE REG_BIT(30)  /* i965+ */
//
// ENABLE is what software asked for; STATE is what the hardware is actually
// doing. They are adjacent bits and they are not the same claim, which is why
// both are decoded separately and why the suite checks each with the other
// clear.
//
#define IG_TRANSCONF_ENABLE             0x80000000u
#define IG_TRANSCONF_STATE_ENABLE       0x40000000u

//
// i915_reg.h:1074-1077 HTOTAL_MASK REG_GENMASK(31,16) / HACTIVE_MASK (15,0)
// i915_reg.h:1105-1108 PIPESRC_WIDTH_MASK (31,16) / PIPESRC_HEIGHT_MASK (15,0)
//
// Every timing register on this engine is the same shape: a 16-bit high half
// and a 16-bit low half, each holding its value minus one.
//
#define IG_TIMING_HI_SHIFT              16u
#define IG_TIMING_HALF_MASK             0xFFFFu

//
// A POLICY BOUND, not a hardware boundary. BAR0 on this device is 16 MiB and
// the display engine does not occupy a documented contiguous window that this
// project has verified. What IS verified is the span of the tables below: the
// lowest entry is transcoder A at 0x60000 and the highest is pipe DSI1 at
// 0x7B800, so the limit is the end of that 4 KiB block. This pair bounds THE
// ADDRESSES THIS HEADER CAN PRODUCE, and says nothing about what the hardware
// decodes.
//
// Note the range is NOT dense: 0x64000..0x6AFFF and 0x74000..0x7AFFF fall
// inside it and belong to DDI/AUX and other blocks this header does not
// address. A bound that a produced address passes is not evidence the address
// is one of ours.
//
// Named the way NV_GSP_FW_ADDR_BITS is named: as a ceiling this project chose,
// so a later reader does not mistake it for a width read off silicon.
//
#define IG_DISPLAY_POLICY_BASE          0x00060000u
#define IG_DISPLAY_POLICY_LIMIT         0x0007BFFFu

static inline bool igIsInDisplayPolicyRange(IGU32 offset)
{
    return offset >= IG_DISPLAY_POLICY_BASE && offset <= IG_DISPLAY_POLICY_LIMIT;
}

//
// THE READ DECISION, EXTRACTED FROM IntelDisplayProbe::regRead32().
//
// regRead32() has three guards in front of its one dereference, and until this
// function existed all three lived inside the IOKit shell -- a translation unit
// no host suite can compile, for the same reason the verdict-name switches were
// unreachable until IntelDisplayNames.hpp was split out (see handoff20 SS1).
// The dereference has to stay there; the DECISION does not, and the decision is
// the part that can be wrong.
//
// WHY THIS IS NOT MERELY igIsInDisplayPolicyRange() AGAIN
// ------------------------------------------------------
// All three guards return the same value. IG_MMIO_INVALID is 0xFFFFFFFF, so it
// is ALSO outside the policy range -- which means the sentinel guard and the
// range guard are distinguishable only by their PRECEDENCE and by the log line
// each prints. Delete the sentinel guard entirely and regRead32() still refuses
// 0xFFFFFFFF, still returns IG_MMIO_INVALID, and still declines to dereference:
// byte-identical behaviour through the return value, and a log line that now
// blames the policy range for an offset helper's refusal. An operator reading
// NVLOG would go looking for a bad constant in the offset tables instead of the
// declining helper that produced the sentinel.
//
// So this returns a REASON rather than a bool. That is what makes the ordering
// assertable at all: tests/test_intel_display.cpp pins that an unmapped BAR
// beats a sentinel offset, and that a sentinel offset beats out-of-policy, on
// inputs where two guards are true at once. A bool cannot express either.
//
// The caller still decides what to do. This says why, not what.
//
// THE FOURTH GUARD: THE POLICY RANGE IS NOT THE MAPPING
// -----------------------------------------------------
// IG_DISPLAY_POLICY_LIMIT (0x7BFFF) bounds THE ADDRESSES THIS HEADER CAN
// PRODUCE. It says nothing whatsoever about how many bytes the OS actually
// mapped, and those are different numbers on purpose:
// IntelDisplayProbe::mapBar0() logs a WARNING when the BAR is shorter than
// IG_BAR0_EXPECTED_LENGTH (0x80000) and then attaches ANYWAY -- deliberately,
// so that a short BAR stays inspectable in the IORegistry instead of vanishing.
//
// Until this parameter existed, that warning was the only consequence there
// was. Every in-policy offset went straight through to the dereference, so on a
// 0x40000 BAR the transcoder A page at 0x60000 was read 0x20000 bytes past the
// end of the mapping. Past the end of an IOMemoryMap is not a fault that stops
// you; it is whatever the next mapping happens to hold, decoded as a transcoder
// register. The warning's own text says "reads past the end are undefined" --
// and nothing acted on it.
//
// The test is `offset + IG_MMIO_ACCESS_BYTES <= mappedLength`, and the width is
// the point: `offset < mappedLength` accepts an offset at length - 1 and reads
// three bytes past the end, which is the same one-sided-containment defect this
// project has already paid for once. The arithmetic is done in IGU64 so the
// addition cannot wrap back inside the bound.
//
// LAST, not first. An out-of-policy offset is a bug in this driver; a short BAR
// is a fact about the machine. An offset that is BOTH should be reported as the
// bug, because that is the one somebody can actually go and fix.
//
#define IG_READ_OK                      0u   // safe to dereference
#define IG_READ_UNMAPPED                1u   // BAR0 was never mapped
#define IG_READ_SENTINEL_OFFSET         2u   // an offset helper declined and said so
#define IG_READ_OUT_OF_POLICY           3u   // a real offset, outside this track's window
#define IG_READ_PAST_MAPPING            4u   // in policy, past the end of what was mapped
#define IG_READ_VERDICT_LIMIT           4u   // the largest verdict above

static inline IGU32 igReadDecision(bool barMapped, IGU64 mappedLength, IGU32 offset)
{
    //
    // Order is the assertion. Unmapped first because with no base address the
    // offset is not yet meaningful; sentinel before range because 0xFFFFFFFF
    // satisfies both and the more specific diagnosis is the useful one; the
    // mapping bound last, for the reason spelled out above.
    //
    if (!barMapped) {
        return IG_READ_UNMAPPED;
    }
    if (offset == IG_MMIO_INVALID) {
        return IG_READ_SENTINEL_OFFSET;
    }
    if (!igIsInDisplayPolicyRange(offset)) {
        return IG_READ_OUT_OF_POLICY;
    }
    if ((IGU64) offset + (IGU64) IG_MMIO_ACCESS_BYTES > mappedLength) {
        return IG_READ_PAST_MAPPING;
    }
    return IG_READ_OK;
}

//
// THE OTHER HALF OF THAT GUARD: A REFUSAL THAT NOBODY CONSUMES IS A ZERO
// ---------------------------------------------------------------------
// igReadDecision() decides whether the dereference may happen, and the shell's
// regRead32() returns IG_MMIO_INVALID when it may not. Until this predicate
// existed, IG_MMIO_INVALID appeared at exactly ONE place in the whole probe --
// inside regRead32() itself. Both publishers took whatever came back and handed
// it straight to igModeConsistent(), igPipeUsable() and igFbInheritable().
//
// That is the same defect handoff18 SS1d records on the NVIDIA side, one level
// down: setProperty() with a decode of a REFUSED read is byte-identical to
// setProperty() with a decode of a register that genuinely read that value. An
// operator reading the IORegistry cannot tell "transcoder B is unprogrammed"
// from "transcoder B was never read because the BAR is too short", and the
// second one is a fact about this driver that somebody could go and fix.
//
// The predicate is deliberately spelled against the sentinel VALUE rather than
// re-deriving the decision, and there is one honest ambiguity in that: a
// register that genuinely reads 0xFFFFFFFF is indistinguishable from a refusal.
// It does not matter here, and the reason is evidence rather than convenience.
// The 2026-08-16 capture read 0x00000000 out of every unprogrammed display page
// on this silicon (handoff15 SS2) -- all-ones is not what an idle page reports,
// it is what a block that is not decoding reports. Both readings say the same
// thing about the only question this probe asks: do NOT inherit a mode from it.
//
static inline bool igReadRefused(IGU32 value)
{
    return value == IG_MMIO_INVALID;
}

//
// A GROUP of reads, judged together.
//
// Judged together because the decoders are joins: igPipeUsable() consumes nine
// registers and returns one verdict, so a single refused read anywhere in that
// group poisons the whole answer rather than one field of it. Publishing eight
// good values and one sentinel would produce a verdict that is arithmetically
// correct over an input that was never measured.
//
// A count of zero, or a null pointer, answers REFUSED rather than OK. A caller
// that lost its array must not be told its reads were fine -- the same reason
// fBar0Length is zeroed on unmap instead of left stale.
//
#define IG_SAMPLE_OK                    0u   // every read in the group landed
#define IG_SAMPLE_REFUSED               1u   // at least one read was refused
#define IG_SAMPLE_VERDICT_LIMIT         1u   // the largest verdict above

static inline IGU32 igSampleVerdict(const IGU32 *values, IGU32 count)
{
    if (!values || count == 0u) {
        return IG_SAMPLE_REFUSED;
    }
    for (IGU32 i = 0u; i < count; i++) {
        if (igReadRefused(values[i])) {
            return IG_SAMPLE_REFUSED;
        }
    }
    return IG_SAMPLE_OK;
}

//
// trans_offsets[] for XE_D_DISPLAY, from intel_display_device.c:
//   73  TRANSCODER_A_OFFSET     0x60000
//   74  TRANSCODER_B_OFFSET     0x61000
//   75  TRANSCODER_C_OFFSET     0x62000
//   77  TRANSCODER_D_OFFSET     0x63000
//   79  TRANSCODER_DSI0_OFFSET  0x6b000
//   80  TRANSCODER_DSI1_OFFSET  0x6b800
//
// Line 76 is skipped deliberately: it is CHV_TRANSCODER_C_OFFSET, which is
// ALSO 0x63000. Two different platforms give the same address two different
// meanings, so citing the line above or below D would cite a Cherryview
// constant that happens to hold Tiger Lake's value.
//
// Line 78 defines TRANSCODER_EDP_OFFSET 0x6f000, and XE_D_DISPLAY's
// trans_offsets[] (881-888) does not reference it. That is the source-side
// confirmation of the note above IG_TRANSCODER_A.
//
// A switch rather than an array so this stays a constant expression with no
// storage, and rather than arithmetic because DSI1 - DSI0 is 0x800. Any stride
// formula that reproduces A..D gets DSI1 wrong by 0x800, and 0x6C000 is inside
// a different block -- it would read and write without faulting.
//
static inline IGU32 igTransBase(IGU32 trans)
{
    switch (trans) {
    case IG_TRANSCODER_A:    return 0x60000u;
    case IG_TRANSCODER_B:    return 0x61000u;
    case IG_TRANSCODER_C:    return 0x62000u;
    case IG_TRANSCODER_D:    return 0x63000u;
    case IG_TRANSCODER_DSI0: return 0x6B000u;
    case IG_TRANSCODER_DSI1: return 0x6B800u;
    default:                 return IG_MMIO_INVALID;
    }
}

//
// pipe_offsets[] for XE_D_DISPLAY, from intel_display_device.c:
//   56  PIPE_A_OFFSET     0x70000
//   57  PIPE_B_OFFSET     0x71000
//   58  PIPE_C_OFFSET     0x72000
//   59  PIPE_D_OFFSET     0x73000
//   70  PIPE_DSI0_OFFSET  0x7b000
//   71  PIPE_DSI1_OFFSET  0x7b800
//
// CORRECTED against intel_display_device.c:873-880 before this file was
// tested. The first draft of this comment claimed XE_D_DISPLAY gives the DSI
// transcoders no pipe entry, so igPipeBase() had a strictly smaller domain
// than igTransBase(). That is FALSE: lines 878-879 map TRANSCODER_DSI_0 and
// TRANSCODER_DSI_1 to PIPE_DSI0_OFFSET and PIPE_DSI1_OFFSET. The two
// functions have the SAME domain and remain separate for the reason in
// hazard 2 -- a different TABLE, not a shorter one.
//
// The DSI pair is also where a stride formula breaks a second time: pipe DSI1
// is 0x800 above pipe DSI0, and both are 0xB000 above pipe A rather than the
// 0x1000 that reproduces A..D.
//
static inline IGU32 igPipeBase(IGU32 trans)
{
    switch (trans) {
    case IG_TRANSCODER_A:    return 0x70000u;
    case IG_TRANSCODER_B:    return 0x71000u;
    case IG_TRANSCODER_C:    return 0x72000u;
    case IG_TRANSCODER_D:    return 0x73000u;
    case IG_TRANSCODER_DSI0: return 0x7B000u;
    case IG_TRANSCODER_DSI1: return 0x7B800u;
    default:                 return IG_MMIO_INVALID;
    }
}

//
// True only for the seven field offsets enumerated above. An unrecognised
// offset is refused rather than added to a base, because base + garbage is a
// valid address into a register nobody meant to touch.
//
static inline bool igIsTransTimingField(IGU32 field)
{
    return field == IG_TRANS_HTOTAL || field == IG_TRANS_HBLANK
        || field == IG_TRANS_HSYNC  || field == IG_TRANS_VTOTAL
        || field == IG_TRANS_VBLANK || field == IG_TRANS_VSYNC
        || field == IG_TRANS_PIPESRC;
}

//
// TRANSCODER-BLOCK addresses only. Hazard 2: do not reach TRANSCONF through
// this function. It cannot produce TRANSCONF's address for any input, because
// TRANSCONF is not in igIsTransTimingField()'s list -- the refusal is
// mechanical rather than a comment asking the caller to remember.
//
static inline IGU32 igTransTimingReg(IGU32 trans, IGU32 field)
{
    IGU32 base = igTransBase(trans);

    if (base == IG_MMIO_INVALID || !igIsTransTimingField(field))
        return IG_MMIO_INVALID;

    return base + field;
}

//
// PIPE-BLOCK address, TRANSCODER-indexed. The whole of hazard 2 in one
// function: the argument names a transcoder, the arithmetic uses the pipe
// table. Separate from igTransTimingReg() so that calling the wrong one is a
// compile-time-visible difference in the call site rather than a silent
// 0x10000 error in the result.
//
static inline IGU32 igTransConfReg(IGU32 trans)
{
    IGU32 base = igPipeBase(trans);

    if (base == IG_MMIO_INVALID)
        return IG_MMIO_INVALID;

    return base + IG_PIPE_TRANSCONF;
}

//
// --- TRANS_DDI_FUNC_CTL ----------------------------------------------------
//
// Which PORT a transcoder is feeding, and in which protocol. Inheritance needs
// this: the plane and raster layers describe WHAT is being scanned out, and
// nothing so far says WHERE it goes. A framebuffer inherited onto a transcoder
// whose DDI is disabled is a picture assembled for a port that is dark.
//
// ADDRESSING. i915_reg.h:3741-3747 spells the six bases out one per line:
//
//   _TRANS_DDI_FUNC_CTL_A     0x60400
//   _TRANS_DDI_FUNC_CTL_B     0x61400
//   _TRANS_DDI_FUNC_CTL_C     0x62400
//   _TRANS_DDI_FUNC_CTL_D     0x63400
//   _TRANS_DDI_FUNC_CTL_DSI0  0x6b400
//   _TRANS_DDI_FUNC_CTL_DSI1  0x6bc00
//
// Subtract igTransBase() from each and the answer is 0x400 SIX TIMES, DSI1
// included: 0x6B800 + 0x400 == 0x6BC00. That is worth stating explicitly,
// because the DSI pair is where every other stride formula in this file breaks
// -- igTransBase() and igPipeBase() are both switches precisely because DSI1
// sits 0x800 rather than 0x1000 above DSI0. This offset is a genuine constant
// displacement within an already-irregular table, so it composes with the
// switch instead of replacing it.
//
// _TRANS_DDI_FUNC_CTL_EDP 0x6F400 is deliberately NOT here, for the same reason
// TRANSCODER_EDP_OFFSET is absent from igTransBase(): XE_D_DISPLAY's
// trans_offsets[] does not reference it, so this silicon has no EDP transcoder
// and an address computed for one would be a live register belonging to nothing
// the caller asked for.
//
#define IG_TRANS_DDI_FUNC_CTL           0x400u

//
// HAZARD 8, AND IT IS THE WORST KIND. The port field moved between generations
// and the two readings OVERLAP:
//
//   pre-icl:  TRANS_DDI_PORT_SHIFT     28, mask 7  (i915_reg.h:3752, 3754)
//   tgl:      TGL_TRANS_DDI_PORT_SHIFT 27, mask 0xF (3753, 3755)
//
// A word decoded with the wrong shift yields a DIFFERENT VALID PORT NUMBER, not
// garbage -- the pre-icl reading is exactly the TGL reading shifted down one
// bit, so it names half the right port. Nothing faults; the driver programs a
// panel that is not the lit one and reads back plausible values from it.
//
// AND THE FIELD IS +1 ENCODED. i915_reg.h:3757 is
//
//   TGL_TRANS_DDI_SELECT_PORT(x)  (((x) + 1) << TGL_TRANS_DDI_PORT_SHIFT)
//
// so a stored 1 means port 0, and a stored ZERO means NO PORT SELECTED rather
// than port 0. That distinction is the whole reason igDdiPort() cannot simply
// return the field: on this silicon every unprogrammed display page reads
// 0x00000000 (2026-08-16 capture, READING NOTES), so a decoder that returned
// the raw field would report "port A" for every transcoder that was never
// configured. Reporting a real port for an unconfigured transcoder is the exact
// shape of defect the 1x1-plane refusals exist to prevent.
//
#define IG_DDI_FUNC_ENABLE              0x80000000u
#define IG_DDI_PORT_SHIFT               27u
#define IG_DDI_PORT_FIELD_MASK          0x0Fu
#define IG_DDI_PORT_NONE                0xFFFFFFFFu

//
// MODE SELECT, i915_reg.h:3758-3763. Three bits at 24, and unlike the port
// field it is NOT +1 encoded -- 0 is HDMI, a real mode. So a zero word decodes
// to "HDMI on no port", which only igDdiEnabled() can reject.
//
#define IG_DDI_MODE_SHIFT               24u
#define IG_DDI_MODE_MASK                0x07u
#define IG_DDI_MODE_HDMI                0u
#define IG_DDI_MODE_DVI                 1u
#define IG_DDI_MODE_DP_SST              2u
#define IG_DDI_MODE_DP_MST              3u
#define IG_DDI_MODE_FDI_OR_128B132B     4u

static inline IGU32 igTransDdiReg(IGU32 trans)
{
    IGU32 base = igTransBase(trans);

    if (base == IG_MMIO_INVALID)
        return IG_MMIO_INVALID;

    return base + IG_TRANS_DDI_FUNC_CTL;
}

static inline bool igDdiEnabled(IGU32 word)
{
    return (word & IG_DDI_FUNC_ENABLE) != 0u;
}

//
// The field as stored, +1 encoding intact. Present so a test can pin the raw
// read separately from the decode -- checking only igDdiPort() would leave the
// shift and the -1 cancelling each other out for any single input.
//
static inline IGU32 igDdiPortField(IGU32 word)
{
    return (word >> IG_DDI_PORT_SHIFT) & IG_DDI_PORT_FIELD_MASK;
}

//
// The port, decoded, or IG_DDI_PORT_NONE. The sentinel is 0xFFFFFFFF rather
// than a small number because every value 0..14 is a port this field can name
// once the +1 comes off, so there is no in-range value left to mean "none".
//
static inline IGU32 igDdiPort(IGU32 word)
{
    IGU32 field = igDdiPortField(word);

    if (field == 0u)
        return IG_DDI_PORT_NONE;

    return field - 1u;
}

static inline IGU32 igDdiMode(IGU32 word)
{
    return (word >> IG_DDI_MODE_SHIFT) & IG_DDI_MODE_MASK;
}

//
// The high half of a timing word, decoded. Hazard 3: the register holds
// value - 1, so this adds 1 back. Returns up to 65536, which is why the return
// type is 32-bit and not 16.
//
static inline IGU32 igTimingHi(IGU32 word)
{
    return ((word >> IG_TIMING_HI_SHIFT) & IG_TIMING_HALF_MASK) + 1u;
}

static inline IGU32 igTimingLo(IGU32 word)
{
    return (word & IG_TIMING_HALF_MASK) + 1u;
}

//
// PIPESRC's two halves. i915_reg.h:1105-1108 spells them WIDTH (31:16) and
// HEIGHT (15:0), and intel_display.c:2814 writes width - 1 / height - 1, so
// they are the same shape as a timing word under different names. Wrappers
// rather than aliases so a caller reading PIPESRC does not have to remember
// which half is which.
//
static inline IGU32 igPipesrcWidth(IGU32 word)
{
    return igTimingHi(word);
}

static inline IGU32 igPipesrcHeight(IGU32 word)
{
    return igTimingLo(word);
}

static inline bool igTransconfEnabled(IGU32 word)
{
    return (word & IG_TRANSCONF_ENABLE) != 0u;
}

static inline bool igTransconfStateEnabled(IGU32 word)
{
    return (word & IG_TRANSCONF_STATE_ENABLE) != 0u;
}

//
// A decoded pair that cannot describe a real raster is worth saying so about,
// because the failure this guards is NOT a fault -- a stale or powered-down
// transcoder returns 0x00000000, which decodes to a perfectly well-formed
// 1x1 mode. total >= active is the property that separates a real timing word
// from a plausible-looking zero.
//
// This is a PLAUSIBILITY test, not a validity test. It cannot tell a correct
// mode from an incorrect one; it can only reject pairs no mode has.
//
static inline bool igTimingPlausible(IGU32 total, IGU32 active)
{
    return active > 1u && total > active && total <= 65536u;
}

//
// Convenience for the common read: is this transcoder actually driving a
// raster right now? Both bits, because ENABLE alone is what software asked
// for and STATE alone is a transcoder mid-transition.
//
static inline bool igTransconfLive(IGU32 word)
{
    return igTransconfEnabled(word) && igTransconfStateEnabled(word);
}

//
// --- the transcoder/DDI join -----------------------------------------------
//
// HAZARD 9. TRANSCONF says whether a transcoder is TIMING; TRANS_DDI_FUNC_CTL
// says whether its output is CONNECTED. The two are separate registers with
// separate enables, and every one of the four combinations is reachable:
//
//   timing + connected    a live output. The only inheritable case.
//   timing + no output    a transcoder generating a raster into nothing.
//   no timing + connected a DDI pointed at a transcoder that is not running.
//   neither               an unprogrammed pipe, which on this silicon reads
//                         0x00000000 in BOTH registers.
//
// None of them fault. The middle two are the dangerous pair on the inheritance
// path: each looks half-configured, and a driver that consults only one of the
// two registers concludes the display is usable. igTransconfLive() answers the
// first half and igDdiEnabled() the second; nothing until now answered both.
//
// THE PORT IS PART OF THE ANSWER, not a detail below it. IG_DDI_PORT_NONE is
// what igDdiPort() returns for a zero field, and a zero field is what an
// unprogrammed page produces -- so a DDI whose ENABLE bit is set but whose port
// field is zero is a real, reachable, non-faulting state that names no output.
// Refusing it here is the same refusal as igFbInheritable()'s 1x1 rejection:
// the decode is well-formed and the configuration is not.
//
// This block sits BELOW igTransconfLive() on purpose. It reads four decoders
// from two different register families, and a header with no includes has no
// forward declarations to fall back on -- placing the join above any one of
// them is a compile error rather than a warning.
//
#define IG_LINK_OK              0u   // timing, connected, and a port named
#define IG_LINK_NO_TIMING       1u   // TRANSCONF is not live
#define IG_LINK_NO_OUTPUT       2u   // TRANS_DDI_FUNC_CTL is disabled
#define IG_LINK_NO_PORT         3u   // enabled, but the port field is zero
#define IG_LINK_VERDICT_LIMIT   3u   // the largest verdict above

static inline IGU32 igTransconfDdiLinked(IGU32 transconfWord, IGU32 ddiWord)
{
    //
    //  TIMING FIRST. A transcoder that is not running makes the DDI state
    //  uninteresting -- the port could be perfectly configured and still have
    //  nothing to carry -- so reporting the DDI verdict there would name a
    //  symptom rather than the cause.
    //
    if (!igTransconfLive(transconfWord))
        return IG_LINK_NO_TIMING;

    if (!igDdiEnabled(ddiWord))
        return IG_LINK_NO_OUTPUT;

    if (igDdiPort(ddiWord) == IG_DDI_PORT_NONE)
        return IG_LINK_NO_PORT;

    return IG_LINK_OK;
}

//
// --- whole-mode consistency ----------------------------------------------
//
// HAZARD 6, and it is the one hazard on this path that NO per-register check
// can see. igTimingPlausible() bounds ONE total/active pair. A raster is six
// words plus PIPESRC, and each of the seven can be individually plausible
// while the SET describes no mode that exists:
//
//   - HBLANK_START can land before the active region ends, so the blanking
//     interval overlaps pixels the plane is still scanning out.
//   - HSYNC can sit outside the blanking interval entirely. Sync inside the
//     active region is not a mode; it is a picture with a bar through it.
//   - PIPESRC's width can disagree with the active width derived from
//     HTOTAL/HBLANK. Both numbers are 1920 on the captured panel, which is
//     exactly why the capture cannot separate the two readings -- see the
//     note in tests/test_intel_display.cpp.
//
// None of these FAULT. Every one of them programs cleanly, and the failure
// mode is a shifted, sheared or torn picture rather than an error the driver
// can read back. On the inheritance path -- reading a running config out
// rather than programming one in -- an inconsistent set means the decode is
// wrong, not the hardware, and that is worth refusing rather than trusting.
//
// A raster's four numbers per axis, all in the DECODED (+1 applied) domain.
// The register order is total, blank, sync; the derivation order is active
// first, because everything else is bounded relative to it.
//
#define IG_MODE_OK              0u   // the set is self-consistent
#define IG_MODE_IMPLAUSIBLE     1u   // a single pair failed igTimingPlausible
#define IG_MODE_BLANK_RANGE     2u   // blank_end <= blank_start, or past total
#define IG_MODE_BLANK_OVERLAP   3u   // blanking starts before active ends
#define IG_MODE_SYNC_RANGE      4u   // sync_end <= sync_start, or past total
#define IG_MODE_SYNC_OUTSIDE    5u   // sync is not contained in the blanking
#define IG_MODE_SRC_MISMATCH    6u   // PIPESRC disagrees with the active region
#define IG_MODE_VERDICT_LIMIT   6u   // the largest verdict above

//
// One axis, checked. All five arguments are in the DECODED domain -- the
// caller has already run igTimingHi()/igTimingLo() over the register words,
// so this function contains no -1/+1 arithmetic at all. That split is
// deliberate: mixing the +1 convention into the consistency logic would make
// every test here also a test of the +1, and a bug in either would be
// attributable to neither.
//
// Order of the checks is load-bearing. The pair bound runs FIRST, because a
// total below its own active makes every subsequent comparison meaningless
// (blank_end <= total would pass against a total that is itself nonsense).
// Containment of sync inside blanking runs LAST, because it is the only check
// that reads two ranges rather than one.
//
static inline IGU32 igAxisConsistent(IGU32 total, IGU32 active,
                                    IGU32 blankStart, IGU32 blankEnd,
                                    IGU32 syncStart, IGU32 syncEnd)
{
    if (!igTimingPlausible(total, active))
        return IG_MODE_IMPLAUSIBLE;

    //
    //  A blanking interval must be a non-empty range that ends inside the
    //  raster. blankEnd == blankStart is not a degenerate blanking period, it
    //  is a decode that produced two copies of one number.
    //
    if (blankEnd <= blankStart || blankEnd > total)
        return IG_MODE_BLANK_RANGE;

    //
    //  Blanking begins at or after the last active pixel. i915 programs
    //  HBLANK_START as hactive - 1, so the decoded blankStart is EQUAL to
    //  active on a normal mode -- the bound is >=, not >. Using > here would
    //  reject every real mode, which is the kind of off-by-one that gets
    //  "fixed" by deleting the check.
    //
    if (blankStart < active)
        return IG_MODE_BLANK_OVERLAP;

    if (syncEnd <= syncStart || syncEnd > total)
        return IG_MODE_SYNC_RANGE;

    //
    //  Sync must live inside the blanking interval. A sync pulse overlapping
    //  active pixels programs cleanly and scans out a bar through the picture.
    //
    if (syncStart < blankStart || syncEnd > blankEnd)
        return IG_MODE_SYNC_OUTSIDE;

    return IG_MODE_OK;
}

//
// Both axes plus PIPESRC, from the RAW register words. This is the caller-
// facing entry point; igAxisConsistent() above is the axis kernel and takes
// decoded numbers.
//
// The six timing words are passed in REGISTER ORDER for each axis -- total,
// blank, sync -- because that is the order they sit at in MMIO (+0x00, +0x04,
// +0x08 for H; +0x0c, +0x10, +0x14 for V) and a caller reading them out of a
// mapped BAR will produce them in exactly that order. Reordering the
// parameters to match the derivation order would invite a caller to hand them
// over in the order they read them and get a silently wrong answer, since all
// six are IGU32 and no swap can be caught by the type system.
//
// PIPESRC is checked LAST and against BOTH axes, because it is the only
// register here that duplicates information the timing words already carry.
// On the captured panel every one of these numbers is 1920 or 1080, so the
// 2026-08-16 capture cannot separate a correct implementation from one that
// compares PIPESRC against the wrong axis. tests/test_intel_display.cpp must
// use a mode whose width and height DIFFER from the captured one to make this
// arm non-vacuous.
//
static inline IGU32 igModeConsistent(IGU32 hTotalWord, IGU32 hBlankWord,
                                     IGU32 hSyncWord,  IGU32 vTotalWord,
                                     IGU32 vBlankWord, IGU32 vSyncWord,
                                     IGU32 pipeSrcWord)
{
    const IGU32 hActive = igTimingLo(hTotalWord);
    const IGU32 vActive = igTimingLo(vTotalWord);

    IGU32 verdict = igAxisConsistent(igTimingHi(hTotalWord), hActive,
                                     igTimingLo(hBlankWord),
                                     igTimingHi(hBlankWord),
                                     igTimingLo(hSyncWord),
                                     igTimingHi(hSyncWord));
    if (verdict != IG_MODE_OK)
        return verdict;

    verdict = igAxisConsistent(igTimingHi(vTotalWord), vActive,
                               igTimingLo(vBlankWord),
                               igTimingHi(vBlankWord),
                               igTimingLo(vSyncWord),
                               igTimingHi(vSyncWord));
    if (verdict != IG_MODE_OK)
        return verdict;

    //
    //  PIPESRC's halves are HI = width, LO = height -- the INVERSE of
    //  PLANE_SIZE, which the 2026-08-16 capture settled by showing the two
    //  registers holding byte-swapped values (0x0437077F vs 0x077F0437) on a
    //  panel where both describe the same 1920x1080. Do not reuse a PLANE_SIZE
    //  decoder here; it transposes the axes and still programs cleanly.
    //
    if (igTimingHi(pipeSrcWord) != hActive || igTimingLo(pipeSrcWord) != vActive)
        return IG_MODE_SRC_MISMATCH;

    return IG_MODE_OK;
}

//
// --- the plane/raster join -------------------------------------------------
//
// HAZARD 7. igModeConsistent() above validates the RASTER; igFbInheritable()
// in IntelFramebuffer.hpp validates the PLANE. Both can return OK on a config
// where the plane is scanning out a different rectangle than the transcoder is
// timing, and NEITHER can see it, by construction: the dependency-free rule
// forbids either header including the other, so no function in this tree has
// both PLANE_SIZE and PIPESRC in scope.
//
// The consequence is not a fault. A plane narrower than the raster scans out
// garbage in the margin; a plane wider than it walks off the end of the
// framebuffer into whatever is mapped next. On the INHERITANCE path -- reading
// a config the firmware already programmed rather than programming one --
// a mismatch means the decode is wrong rather than the hardware, which is
// exactly the case worth refusing.
//
// This function is the join, and it lives here rather than in
// IntelFramebuffer.hpp for one reason: PIPESRC is a TRANSCODER register and
// this header owns transcoder addressing. It takes the plane's width and
// height ALREADY DECODED, so it needs nothing from the other header -- the
// caller runs igFbSizeWidth()/igFbSizeHeight() and hands the numbers over.
// That is the same split as igAxisConsistent(): the +1 convention is applied
// by the caller, so a bug in the convention and a bug in the comparison cannot
// be confused for each other.
//
// DO NOT "simplify" this by decoding pipeSrcWord with a PLANE_SIZE decoder.
// The 2026-08-16 capture showed the two registers holding byte-swapped values
// on the same panel (PIPESRC 0x077F0437, PLANE_SIZE 0x0437077F), so a shared
// decoder transposes the axes and still programs cleanly -- and on that
// panel's own numbers the transpose is invisible, because both halves
// describe the same 1920x1080 rectangle.
//
#define IG_JOIN_OK              0u   // plane rectangle == raster active area
#define IG_JOIN_WIDTH           1u   // widths disagree
#define IG_JOIN_HEIGHT          2u   // heights disagree
#define IG_JOIN_DEGENERATE      3u   // a 1x1 plane, i.e. an unprogrammed page
#define IG_JOIN_VERDICT_LIMIT   3u   // the largest verdict above

static inline IGU32 igPlaneMatchesPipe(IGU32 planeWidth, IGU32 planeHeight,
                                       IGU32 pipeSrcWord)
{
    //
    //  The degenerate case FIRST, and it is not hypothetical. Every unused
    //  display page on this silicon reads 0x00000000 rather than all-ones
    //  (2026-08-16 capture, READING NOTES), and both PLANE_SIZE halves carry
    //  the -1 convention, so an unprogrammed plane decodes to a perfectly
    //  well-formed 1x1. Reporting that as a WIDTH mismatch would be true but
    //  useless -- it names a symptom of a plane that was never programmed at
    //  all.
    //
    if (planeWidth < 2u || planeHeight < 2u)
        return IG_JOIN_DEGENERATE;

    if (planeWidth != igTimingHi(pipeSrcWord))
        return IG_JOIN_WIDTH;

    if (planeHeight != igTimingLo(pipeSrcWord))
        return IG_JOIN_HEIGHT;

    return IG_JOIN_OK;
}

//
// --- the plane layer ------------------------------------------------------
//
// HAZARD 4, and it is the EXACT INVERSE of hazard 1. The transcoders must not
// be addressed arithmetically, because DSI1 - DSI0 is 0x800 while D - C is
// 0x1000. The planes genuinely ARE arithmetic:
//
//   skl_universal_plane_regs.h:11-12
//     #define _SKL_PLANE(pipe, plane, r1a, r1b, r2a, r2b) \
//             _PLANE((plane), _PIPE((pipe), (r1a), (r1b)), _PIPE(...))
//   intel_display_reg_defs.h:18-19
//     #define _PIPE(pipe, a, b)   _PICK_EVEN(pipe, a, b)
//     #define _PLANE(plane, a, b) _PICK_EVEN(plane, a, b)
//   i915_reg_defs.h:213
//     #define _PICK_EVEN(i, a, b) ((a) + (i) * ((b) - (a)))
//
// So the address is base + pipe*(B - A) + plane*(2 - 1), and both strides are
// UNIFORM across every plane register block. Measured from the four-corner
// quads the vendored header spells out, rather than assumed:
//
//   CTL     0x70180  0x70280  0x71180  0x71280   (:31-34)
//   STRIDE  0x70188  0x70288  0x71188  0x71288   (:102-105)
//   POS     0x7018c  0x7028c  0x7118c  0x7128c   (:112-115)
//   SIZE    0x70190  0x70290  0x71190  0x71290   (:124-127)
//   KEYVAL  0x70194  0x70294  0x71194  0x71294   (:136-139)
//
// Every row gives plane delta 0x100 and pipe delta 0x1000. That is why the
// helper below multiplies instead of switching.
//
// THE CONSEQUENCE, which is the reason this is filed as a hazard rather than a
// convenience: the DSI pipes are at 0x7B000 and 0x7B800, which are NOT on the
// 0x1000 ladder that starts at pipe A. Plane arithmetic cannot reach them. A
// DSI transcoder index handed to a plane helper produces
// 0x70180 + 4*0x1000 = 0x74180 -- inside the display block, a real writable
// address, and nothing to do with the plane the caller asked for. It does not
// fault. So igPlaneReg() REFUSES a DSI index rather than computing it, and the
// suite pins that refusal.
//
#define IG_PLANE_CTL_BASE       0x70180u  // _PLANE_CTL_1_A, :31
#define IG_PLANE_PIPE_DELTA     0x1000u   // proven by the _1_B column above
#define IG_PLANE_DELTA          0x100u    // proven by the _2_A column above

//
// Field offsets RELATIVE TO a plane's own CTL register, so the same table
// works for every (pipe, plane) pair. Absolute values are in the comment.
//
#define IG_PLANE_CTL            0x00u  // 0x70180  :31
#define IG_PLANE_STRIDE         0x08u  // 0x70188  :102
#define IG_PLANE_POS            0x0Cu  // 0x7018c  :112
#define IG_PLANE_SIZE           0x10u  // 0x70190  :124
#define IG_PLANE_KEYVAL         0x14u  // 0x70194  :136
#define IG_PLANE_KEYMSK         0x18u  // 0x70198  :144
#define IG_PLANE_SURF           0x1Cu  // 0x7019c  :153
#define IG_PLANE_KEYMAX         0x20u  // 0x701a0  :163
#define IG_PLANE_OFFSET         0x24u  // 0x701a4  :173
#define IG_PLANE_SURFLIVE       0x2Cu  // 0x701ac  :185
#define IG_PLANE_CC_VAL         0x34u  // 0x701b4  :193
#define IG_PLANE_AUX_DIST       0x40u  // 0x701c0  :201
#define IG_PLANE_FIELD_LIMIT    0x40u  // the largest offset above

//
// HOW MANY PLANES A GEN12 PIPE HAS IS **NOT SETTLED** BY THE VENDORED SET.
//
// intel_display_device.c:1561-1569 branches on a predicate whose definition is
// not vendored:
//
//   if (DISPLAY_VER(i915) >= 13 || HAS_D12_PLANE_MINIMIZATION(i915))
//           num_sprites[pipe] = 4;
//   else if (DISPLAY_VER(i915) >= 11)
//           num_sprites[pipe] = 6;
//
// Tiger Lake is DISPLAY_VER 12, so it takes one branch or the other depending
// on HAS_D12_PLANE_MINIMIZATION, and neither that macro nor `enum plane_id`
// (intel_display_types.h) is in reference/i915/. Sprites + 1 primary gives 5 or
// 7; this file does not know which.
//
// What IS proven is that PLANE_5 exists as an index --
// skl_universal_plane_regs.h:21 and i915_reg.h:2538 both name it. So 5 planes
// per pipe is the largest count the vendored evidence supports, and that is
// what this constant is: a POLICY CEILING in the same sense as
// NV_GSP_FW_ADDR_BITS, not a derived hardware width. If the real count is 7,
// this refuses two planes that exist -- which costs a feature and cannot
// corrupt anything. Reversing it would compute addresses for planes that may
// not be there.
//
#define IG_PLANE_POLICY_COUNT   5u

//
// PLANE_CTL fields. Gen12 is icl+, which matters for exactly one of these.
//
#define IG_PLANE_CTL_ENABLE     0x80000000u  // REG_BIT(31),        :38
#define IG_PLANE_FORMAT_SHIFT   23u          // GENMASK(27,23) low, :49
#define IG_PLANE_FORMAT_MASK    0x1Fu        // 5 bits, icl+

//
// The pre-icl mask is GENMASK(27,24) -- FOUR bits at shift 24 (:48). The header
// comment at :43-47 spells out that the shift-24 format values still decode
// correctly on icl+ *provided bit 23 is zero*. It is not zero for Y210 (1),
// Y212 (3) and Y216 (5), so a Gen12 decode using the 4-bit mask silently
// misreads every one of those as a different, valid format. Both are defined so
// the suite can show the two disagreeing on a real value.
//
#define IG_PLANE_FORMAT_SHIFT_SKL   24u
#define IG_PLANE_FORMAT_MASK_SKL    0x0Fu

#define IG_PLANE_STRIDE_MASK    0x0FFFu      // GENMASK(11,0),  :109
#define IG_PLANE_POS_Y_SHIFT    16u          // GENMASK(31,16), :118
#define IG_PLANE_POS_X_MASK     0xFFFFu      // GENMASK(15,0),  :121
#define IG_PLANE_HEIGHT_SHIFT   16u          // GENMASK(31,16), :131
#define IG_PLANE_WIDTH_MASK     0xFFFFu      // GENMASK(15,0),  :133
#define IG_PLANE_HALF_MASK      0xFFFFu

//
// A plane register's address, or IG_MMIO_INVALID.
//
// Refuses, in order: a pipe index the arithmetic cannot express (DSI0, DSI1,
// and anything past them), a plane index above the policy ceiling, and a field
// offset that is not one of the twelve above. The last of those is the weakest
// of the three -- it bounds the offset and its alignment rather than checking
// membership -- so the suite pins the specific gaps (0x28, 0x30) that a real
// block has and this test lets through as ALIGNED-but-undefined.
//
static inline IGU32 igPlaneReg(IGU32 pipe, IGU32 plane, IGU32 field)
{
    if (pipe > IG_TRANSCODER_D) {
        return IG_MMIO_INVALID;   // DSI is off the 0x1000 ladder entirely
    }
    if (plane >= IG_PLANE_POLICY_COUNT) {
        return IG_MMIO_INVALID;
    }
    if (field > IG_PLANE_FIELD_LIMIT || (field & 0x3u) != 0u) {
        return IG_MMIO_INVALID;
    }
    return IG_PLANE_CTL_BASE
         + pipe  * IG_PLANE_PIPE_DELTA
         + plane * IG_PLANE_DELTA
         + field;
}

static inline bool igPlaneEnabled(IGU32 word)
{
    return (word & IG_PLANE_CTL_ENABLE) != 0u;
}

//
// The icl+ 5-bit decode. This is the one Gen12 wants.
//
static inline IGU32 igPlaneFormat(IGU32 word)
{
    return (word >> IG_PLANE_FORMAT_SHIFT) & IG_PLANE_FORMAT_MASK;
}

//
// The pre-icl 4-bit decode, present ONLY so the suite can demonstrate it
// disagreeing. Do not call it from the kext.
//
static inline IGU32 igPlaneFormatSkl(IGU32 word)
{
    return (word >> IG_PLANE_FORMAT_SHIFT_SKL) & IG_PLANE_FORMAT_MASK_SKL;
}

static inline IGU32 igPlaneStride(IGU32 word)
{
    return word & IG_PLANE_STRIDE_MASK;
}

static inline IGU32 igPlanePosX(IGU32 word)
{
    return word & IG_PLANE_POS_X_MASK;
}

static inline IGU32 igPlanePosY(IGU32 word)
{
    return (word >> IG_PLANE_POS_Y_SHIFT) & IG_PLANE_HALF_MASK;
}

//
// HAZARD 5, AND IT IS NOW SETTLED -- BY MEASUREMENT, NOT BY A VENDORED HEADER.
//
// These still return the RAW field, unchanged. The `Raw` in the name still means
// what it meant: no +1 is applied. Two decoded wrappers were ADDED below rather
// than changing what these return, because callers written against the raw
// behaviour must not silently start getting a different number.
//
// The history, kept because the reason the answer was unknown matters:
// every TIMING register on this engine stores value - 1 (hazard 3), and PIPESRC
// does too -- intel_display.c:2814 writes width - 1 / height - 1, which is why
// igPipesrcWidth() adds it back. It was natural to assume PLANE_SIZE followed
// suit, and nothing in reference/i915/ showed it: the writer is
// skl_universal_plane.c, which is NOT vendored, and a grep of the vendored
// intel_display.c for PLANE_WIDTH / PLANE_HEIGHT returns no matches. So the
// convention was recorded as UNKNOWN rather than assumed.
//
// IG_PLANE_SIZE_OBSERVED_2026_08_16 -- 2026-08-16 02:30:53 UTC, Tiger Lake mobile
// 8086:9a68 at 0000:00:02.0, i915 bound, eDP-1 lit at 1920x1080 (the panel's
// only advertised mode). PLANE_SIZE for pipe A plane 1, read at
// IG_PLANE_CTL_BASE + IG_PLANE_SIZE = 0x70190:
//
//     0x0437077F   width  field (15:0)  = 0x077F = 1919
//                  height field (31:16) = 0x0437 = 1079
//
// 1919 + 1 == 1920 and 1079 + 1 == 1080, so PLANE_SIZE stores value - 1 on both
// halves, exactly as the timing registers do. The full capture is
// igpu-results/tgl-capture-2026-08-16-022556.txt.
//
// WHAT THIS ONE SAMPLE DOES AND DOES NOT ESTABLISH. It settles the ENCODING:
// a register holding 1919 on a 1920-wide surface cannot be storing the width
// directly, and the alternative readings (raw width, or width - 1 on only one
// half) are all excluded by this single word because the two halves disagree
// with each other in the same direction as the mode's two axes. It does NOT
// establish behaviour for scaled or rotated planes, for a plane narrower than
// its pipe, or for any format other than the observed one -- raw 5-bit field 4,
// which is code 2, XRGB_2101010 -- one unscaled full-screen desktop plane is
// the only configuration witnessed.
//
// CORRECTED 2026-11-01 (t-a7929c). The line above said "the observed 4
// (XRGB8888)". igPlaneFormat() returns the raw field, and 4 is right as a raw
// field, but the codes are 4-bit at bit 24
// (reference/i915/skl_universal_plane_regs.h:43-60), so field 4 is code 2 and
// XRGB_8888 is field 8. igFbFormat() is the decode that returns codes.
//
// The field ORDER is inverted between the two registers, and this is the pair
// most likely to produce a silently valid wrong answer:
//
//     PIPESRC    0x077F0437  width  in 31:16, height in 15:0
//     PLANE_SIZE 0x0437077F  height in 31:16, width  in 15:0
//
// Both were read in the same capture, on the same pipe, encoding the same
// 1920x1080. They are byte-swapped halves of one another. Reusing one
// decoder for both returns a plausible number with the axes transposed, which
// on a non-square mode is a wrong surface that still programs cleanly.
//
// The observed word itself, so that every paragraph above cites a name this
// file actually SPELLS rather than a label that exists only in prose. A cited
// name with no definition reads as provenance and is not; that is the one
// asset this project has. It is the only _OBSERVED_ constant from the
// 2026-08-16 capture; the IG_OBSERVED_*_2026_08_18 block further down holds
// the values from the second and final capture.
//
#define IG_PLANE_SIZE_OBSERVED_2026_08_16   0x0437077Fu

static inline IGU32 igPlaneSizeWidthRaw(IGU32 word)
{
    return word & IG_PLANE_WIDTH_MASK;
}

static inline IGU32 igPlaneSizeHeightRaw(IGU32 word)
{
    return (word >> IG_PLANE_HEIGHT_SHIFT) & IG_PLANE_HALF_MASK;
}

//
// Decoded PLANE_SIZE, in pixels. Added 2026-08-16 on the strength of
// IG_PLANE_SIZE_OBSERVED_2026_08_16 above; the Raw pair is deliberately left
// alone. Like igPipesrcWidth() these return up to 65536, which is why the
// return type is IGU32 and not a 16-bit type -- a field of all ones decodes to
// 65536 and would wrap to 0 in 16 bits.
//
// These are the INVERSE-ORDER counterparts of igPipesrcWidth/Height: width is
// the LOW half here and the HIGH half there. Do not fold the two pairs.
//
static inline IGU32 igPlaneSizeWidth(IGU32 word)
{
    return igPlaneSizeWidthRaw(word) + 1u;
}

static inline IGU32 igPlaneSizeHeight(IGU32 word)
{
    return igPlaneSizeHeightRaw(word) + 1u;
}

//
// --- the whole-pipe join ----------------------------------------------------
//
// HAZARD 10. Three joins now exist and each answers one third of the question
// a driver actually asks before inheriting a display:
//
//   igTransconfDdiLinked()  is this transcoder running, and does it name a port
//   igModeConsistent()      do its six timing words describe one raster
//   igPlaneMatchesPipe()    does the scanned-out surface match that raster
//
// Any one of them passing in isolation is a half-answer, and the half-answers
// are individually convincing. A pipe can carry a perfectly self-consistent
// raster to no port at all; a live port can carry a raster whose plane is a
// different size. Neither faults, and neither is visible to a caller that
// checked only the other.
//
// ORDER IS CAUSAL, not arbitrary. The link is checked first because a dead
// transcoder makes the timing and plane verdicts describe stale register
// contents rather than a running configuration -- reporting IG_PIPE_TIMING for
// an unprogrammed pipe would name a symptom of the pages reading 0x00000000
// rather than the fact that nothing is programmed. Timing precedes the plane
// for the same reason: igPlaneMatchesPipe() compares against PIPESRC, so a
// PIPESRC that disagrees with its own transcoder makes the plane verdict
// meaningless whichever way it lands.
//
// This collapses the three sub-verdicts to a single code deliberately. The
// caller that wants the detail calls the three directly; this one answers the
// only question the inheritance path has, which is whether to trust what the
// firmware left behind.
//
// NOT named IG_PIPE_. That prefix is already taken by IG_PIPE_TRANSCONF (0x08,
// a register offset within the transcoder page), and the register gate groups
// verdict families BY PREFIX -- so an IG_PIPE_ verdict set would have been
// measured against a 0x08 register offset and reported a contiguity failure
// naming a constant that has nothing to do with this function. Caught by the
// gate on the first run, 2026-08-17.
//
#define IG_USABLE_OK              0u   // link, timing and plane all agree
#define IG_USABLE_LINK            1u   // igTransconfDdiLinked() refused
#define IG_USABLE_TIMING          2u   // igModeConsistent() refused
#define IG_USABLE_PLANE           3u   // igPlaneMatchesPipe() refused
#define IG_USABLE_VERDICT_LIMIT   3u   // the largest verdict above

static inline IGU32 igPipeUsable(IGU32 transconfWord, IGU32 ddiWord,
                                 IGU32 hTotalWord, IGU32 hBlankWord,
                                 IGU32 hSyncWord, IGU32 vTotalWord,
                                 IGU32 vBlankWord, IGU32 vSyncWord,
                                 IGU32 pipeSrcWord, IGU32 planeSizeWord)
{
    if (igTransconfDdiLinked(transconfWord, ddiWord) != IG_LINK_OK)
        return IG_USABLE_LINK;

    if (igModeConsistent(hTotalWord, hBlankWord, hSyncWord,
                         vTotalWord, vBlankWord, vSyncWord,
                         pipeSrcWord) != IG_MODE_OK)
        return IG_USABLE_TIMING;

    //
    //  PLANE_SIZE's halves are INVERTED relative to PIPESRC -- width low,
    //  height high, where PIPESRC is the reverse (2026-08-16 capture:
    //  0x077F0437 against 0x0437077F for the same 1920x1080). Decoding it here
    //  through igPlaneSizeWidth/Height rather than reusing the PIPESRC pair is
    //  the whole reason this call is not a two-liner the caller could inline.
    //
    if (igPlaneMatchesPipe(igPlaneSizeWidth(planeSizeWord),
                           igPlaneSizeHeight(planeSizeWord),
                           pipeSrcWord) != IG_JOIN_OK)
        return IG_USABLE_PLANE;

    return IG_USABLE_OK;
}


//
//  ============================================================================
//  VALUES WITNESSED ON SILICON -- 2026-08-18 20:14:12 UTC
//  ============================================================================
//
//  Source: igpu-results/tgl-final-capture-2026-08-18-201412.txt, the SECOND
//  and FINAL boot of Linux on the target Tiger Lake laptop. 718 live dwords
//  across 25 ranges. There will never be a third capture -- that machine is
//  being overwritten with macOS -- so every constant below is the only evidence
//  of its kind this project will ever hold. See intel-handoff28.md.
//
//  These are DATA, not inputs. Same rule as IG_PLANE_SIZE_OBSERVED_2026_08_16
//  above: the decoders are CHECKED AGAINST these words by
//  tests/test_intel_display.cpp, and no decoder derives a mask, shift or offset
//  from any of them. Deriving the decode from the capture would make the decode
//  unfalsifiable -- it would agree with the silicon by construction rather than
//  by being right.
//
//  WHY THE PREFIX IS IG_OBSERVED_ AND NOT IG_TRANS_ / IG_PIPE_ / IG_PLANE_
//
//  handoff16 section 1: tools/check-registers.py groups constants BY PREFIX for
//  its set and contiguity checks, so a new family sharing a prefix with an
//  existing one sweeps that one in and reports a failure that is correct about
//  its own arithmetic and wrong about the world. IG_PIPE_ did exactly that to
//  IG_PIPE_TRANSCONF. A fresh prefix that occurs nowhere else in the tree is
//  grouped with nothing, and these are measurements rather than a verdict enum,
//  so being outside every family is the correct place for them.
//
//  WHAT IS DELIBERATELY ABSENT, AND WHY -- read this before adding a row
//
//  A register read on a running display is a SAMPLE, not a constant. The
//  capture read several ranges twice inside one run and the two reads
//  DISAGREED, clustering in two places:
//
//    0x064000  DDI_BUF_CTL_A. Its bit 7 is DDI_BUF_IS_IDLE
//              (reference/i915/i915_reg.h:3869) -- a live status bit that the
//              hardware owns and changes underneath the reader. The word read
//              0x80000087 at one moment and is not that word by nature.
//    ~0x70240  the watermark and allocation band around _PLANE_WM_1_A_0
//              (reference/i915/skl_universal_plane_regs.h:315). These track
//              memory pressure, so they are a reading of a moment.
//
//  Also absent on purpose:
//
//    PLANE_SURF (read 0x01A7C000) is the scanout base in the GGTT. It changes
//    on every pageflip, so pinning it would file the address of one frame
//    beside the geometry of the panel and both would look equally permanent.
//    PLANE_SURFLIVE is the same objection one level worse.
//
//  Only CONFIGURATION registers -- words the driver wrote and the hardware
//  holds until it is written again -- appear below. Anything a future session
//  adds here has to survive that test first.
//

//
//  Transcoder A at 0x60000, driving the lit internal eDP panel at 1920x1080.
//  This block is the POSITIVE CONTROL for the whole capture: it decodes to the
//  panel that was visibly on at the time, so a reader that got these words is a
//  reader that reached real registers, and every 0x00000000 elsewhere in the
//  file means UNPROGRAMMED rather than NOT READ.
//
//  All six timing words store value - 1 on BOTH halves (hazard 3), confirmed
//  here for the second time on a second independent boot.
//
#define IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18        0x081F077Fu  // 1919 / 2079
#define IG_OBSERVED_TRANS_A_HBLANK_2026_08_18        0x081F077Fu  // 1919 / 2079
#define IG_OBSERVED_TRANS_A_HSYNC_2026_08_18         0x081B07EBu  // 2027 / 2075
#define IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18        0x049B0437u  // 1079 / 1179
#define IG_OBSERVED_TRANS_A_VBLANK_2026_08_18        0x049B0437u  // 1079 / 1179
#define IG_OBSERVED_TRANS_A_VSYNC_2026_08_18         0x043F043Au  // 1082 / 1087

//
//  PIPESRC, and the answer to Q4. Raw 1919x1079, +1 == 1920x1080 == the size
//  the panel advertises. The +1 convention was first measured 2026-08-16; this
//  is the same value from a separate boot, which is what upgrades it from one
//  reading to a repeated measurement.
//
#define IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18       0x077F0437u

//
//  TRANS_DDI_FUNC_CTL. Decodes with the constants above it in this file:
//    bit 31          set             -- IG_DDI_FUNC_ENABLE, i915_reg.h:3750
//    (word >> 24) & 7  == 2          -- IG_DDI_MODE_DP_SST, i915_reg.h:3761
//    (word >> 27) & 0xF == 1         -- a named port, via IG_DDI_PORT_SHIFT
//
//  The port shift is the TGL one (27), not the pre-icl 28. Hazard 8: reading
//  this word with shift 28 yields a different port number that is still inside
//  the field's range, so it names the wrong port rather than failing.
//
#define IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18  0x8A000006u
#define IG_OBSERVED_TRANS_A_MSA_MISC_2026_08_18      0x00000021u

//
//  Pipe A configuration. TRANSCONF carries BOTH live bits:
//    bit 31  TRANSCONF_ENABLE        i915_reg.h:1589
//    bit 30  TRANSCONF_STATE_ENABLE  i915_reg.h:1591
//  which is what makes 0xC0000024 the word igTransconfDdiLinked() is written
//  to accept, rather than one of them alone.
//
#define IG_OBSERVED_PIPE_A_TRANSCONF_2026_08_18      0xC0000024u
#define IG_OBSERVED_PIPE_A_MISC_2026_08_18           0x00800100u  // _PIPE_MISC_A,  :1708
#define IG_OBSERVED_PIPE_A_MISC2_2026_08_18          0x00808000u  // _PIPE_MISC2_A, :1739

//
//  Plane 1 on pipe A at 0x70180 -- the one live plane slot on this machine.
//  CTL 0x82009000 decodes as:
//    bit 31            set    -- PLANE_CTL_ENABLE, skl_universal_plane_regs.h:38
//    (word >> 23) & 0x1F == 4 -- the icl+ 5-bit format field, :49
//    (word >> 10) & 7    == 4 -- PLANE_CTL_TILED_Y, :86
//
//  STRIDE is pinned as the RAW word and as its masked field only. The byte
//  stride depends on the tiling mode's unit size, and that conversion is not
//  in any vendored header -- writing one here would be an invented number filed
//  beside measured ones, which is the single thing this section may not do.
//
#define IG_OBSERVED_PLANE_1A_CTL_2026_08_18          0x82009000u
#define IG_OBSERVED_PLANE_1A_STRIDE_2026_08_18       0x0000003Cu  // & 0xFFF == 60
#define IG_OBSERVED_PLANE_1A_POS_2026_08_18          0x00000000u  // origin
#define IG_OBSERVED_PLANE_1A_SIZE_2026_08_18         0x0437077Fu  // == the 08-16 word
#define IG_OBSERVED_PLANE_1A_OFFSET_2026_08_18       0x00000000u

//
//  The Gen12 PLL block at 0x164000, dumped for the first time in this capture.
//  Kept as raw words rather than decoded: DPLL_CFGCR0's DCO fraction/integer
//  split (i915_reg.h:4270-4273) and CFGCR1's Q/K/P dividers (:4279+) are
//  vendored, but nothing in this tree computes a pixel clock from them, so a
//  decoded value here would be a number with no consumer and no check.
//
#define IG_OBSERVED_DPLL0_CFGCR0_2026_08_18          0x00E001A5u
#define IG_OBSERVED_DPLL0_CFGCR1_2026_08_18          0x00000048u

//
//  Q1/Q2: THE DSI 0x800 STRIDE, WITNESSED.
//
//  Nothing on this laptop will ever program a DSI transcoder -- it is eDP-only
//  -- so no DSI block will ever hold a mode, and handoff15 section 2 recorded
//  the stride as unanswerable. That was wrong about the METHOD rather than
//  about the machine.
//
//  Both pages exist and both are LIVE. Every timing word in each reads
//  0x00000000, and TRANS_DDI_FUNC_CTL in each reads the SAME non-trivial
//  0x00030000. Two pages at a fixed spacing holding identical non-trivial
//  reset state is the signature of two instances of one block at that stride.
//
//  That is weaker than reading a programmed mode out of both, and it is far
//  stronger than a stride this project invented. Do not upgrade this sentence
//  when citing it: what is witnessed is a RESET-STATE pattern at a spacing, not
//  two configured transcoders.
//
#define IG_OBSERVED_TRANS_DSI0_BASE_2026_08_18       0x0006B000u
#define IG_OBSERVED_TRANS_DSI1_BASE_2026_08_18       0x0006B800u
#define IG_OBSERVED_TRANS_DSI_STRIDE_2026_08_18      0x00000800u
#define IG_OBSERVED_TRANS_DSI_DDI_FUNC_CTL_2026_08_18 0x00030000u

//
//  What an unprogrammed page reads on THIS silicon. Not all-ones -- measured
//  2026-08-16 and again 2026-08-18 across transcoders B, C and D and pipes B,
//  C and D, all of which certainly exist. This is why igReadRefused() answers
//  FALSE for zero: on this hardware zero is a legitimate reading of a real
//  register, and 0xFFFFFFFF is what a block that is not decoding reports.
//
#define IG_OBSERVED_UNPROGRAMMED_2026_08_18          0x00000000u


//
//  ============================================================================
//  CONFIGURATION STATE FROM THE SAME BOOT -- clocks, fuses, panel power
//  ============================================================================
//
//  Same capture, same boot, same prohibition as the block above:
//  igpu-results/tgl-final-capture-2026-08-18-201412.txt, 2026-08-18 20:14:12
//  UTC, and there will never be a third one.
//
//  Every word below is on the capture's own WHAT IS SAFE TO HARVEST list, and
//  that list is a measurement rather than an opinion: the script read a subset
//  of registers TWICE in the one run, seven of them disagreed between the two
//  reads, and the harvest list is what survived. So the rule this block obeys
//  is the capture's own -- A REGISTER READ ON A RUNNING DISPLAY IS A SAMPLE,
//  NOT A CONSTANT, and only configuration state may be named *_OBSERVED_*.
//
//  THE EXCLUSIONS ARE THE LOAD-BEARING PART OF THIS BLOCK. Three section 6
//  reads look exactly as harvestable as the ten below and are not here:
//
//    _DDI_BUF_CTL_A (0x064000) read 0x80000087 in one pass and 0x8000008F in
//    the other. Bit 3 is DDI_BUF_IS_IDLE, a live status bit, so the two
//    readings are the same register in two states rather than a bad read --
//    and a value pinned here would be whichever pass happened to land first.
//    This is the one register in the capture that proves the rule, because it
//    is a DDI control register and reads like configuration until you have two
//    samples of it.
//
//    PCH_DREF_CONTROL (0x0C6200) and _TRANS_DP_CTL_A (0x0E0300) both read
//    0xFFFFFFFF, which on this silicon means the block is not decoding at all
//    (IG_OBSERVED_UNPROGRAMMED_2026_08_18 above is what a page that IS
//    decoding and merely unprogrammed reads). Filing all-ones as a measured
//    value would file a refusal as a reading.
//
//  NAMING. The first five carry their i915 names because those names have a
//  citable line in this checkout. The last five do NOT: intel_pps_regs.h and
//  intel_dp_aux_regs.h were never vendored, so PP_STATUS, PP_CONTROL and the
//  BLC_PWM family have no anchor here (four greps, zero hits -- see
//  intel-handoff27.md section 6 item 5). Inventing those names would put a
//  fabricated citation beside five real ones, so those constants are named by
//  their OFFSET and the descriptive words come from the capture's own range
//  table rather than from me.
//
//  ANCHORS, one per line, all in reference/i915/i915_reg.h:
//
//    CDCLK_CTL          _MMIO(0x46000)    :4060
//    SKL_DFSM           _MMIO(0x51000)    :2858
//    SOUTH_CHICKEN1     _MMIO(0xc2000)    :3358
//    SFUSE_STRAP        _MMIO(0xc2014)    :4391
//    ICL_DPCLKA_CFGCR0  _MMIO(0x164280)   :4158
//
#define IG_OBSERVED_CDCLK_CTL_2026_08_18             0x0038017Eu
#define IG_OBSERVED_SKL_DFSM_2026_08_18              0x800C1240u
#define IG_OBSERVED_SOUTH_CHICKEN1_2026_08_18        0x00000901u
#define IG_OBSERVED_SFUSE_STRAP_2026_08_18           0x00000000u
#define IG_OBSERVED_DPCLKA_CFGCR0_2026_08_18         0x01E17800u

//
//  Panel power sequencing, capture range 0x0C7000 +0x1000, LIVE. Four dwords
//  at 0x0C7200..0x0C720C, each read once. Unanchored -- see NAMING above.
//
#define IG_OBSERVED_PANEL_PWR_C7200_2026_08_18       0x80000008u
#define IG_OBSERVED_PANEL_PWR_C7204_2026_08_18       0x00000067u
#define IG_OBSERVED_PANEL_PWR_C7208_2026_08_18       0x07D00001u
#define IG_OBSERVED_PANEL_PWR_C720C_2026_08_18       0x01F40001u

//
//  Backlight PWM, capture range 0x0C8000 +0x1000, LIVE. Three dwords at
//  0x0C8250..0x0C8258, each read once. Unanchored -- see NAMING above. The
//  two 0x00017700 words are equal in the capture and that equality is
//  PRESERVED here rather than collapsed to one constant, because they are two
//  registers that happened to hold the same value on this panel and a single
//  constant would assert they must.
//
#define IG_OBSERVED_BACKLIGHT_C8250_2026_08_18       0x80000000u
#define IG_OBSERVED_BACKLIGHT_C8254_2026_08_18       0x00017700u
#define IG_OBSERVED_BACKLIGHT_C8258_2026_08_18       0x00017700u

//
//  The CDCLK decimal frequency field. The MASK is anchored
//  (CDCLK_FREQ_DECIMAL_MASK = 0x7ff, i915_reg.h:4082) so the extraction is a
//  citation; the kHz relation behind it is NOT vendored in this checkout and
//  is therefore deliberately NOT encoded here. This function returns the RAW
//  field, and whoever converts it to a frequency must bring an anchor for the
//  conversion rather than inherit one from this comment.
//
#define IG_CDCLK_FREQ_DECIMAL_MASK                   0x000007FFu

static inline IGU32 igCdclkFreqDecimal(IGU32 cdclkCtl)
{
    return cdclkCtl & IG_CDCLK_FREQ_DECIMAL_MASK;
}

#endif // INTEL_DISPLAY_HPP
