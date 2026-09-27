//
//  IntelGtCmd.hpp
//  Gen12 (Tiger Lake) render-engine command submission -- ring buffer geometry,
//  tail/head wrap arithmetic, MI command word composition, and the refusal
//  verdicts that stand in front of a submission.
//
//  Same rule as IntelDisplay.hpp, IntelFramebuffer.hpp, IntelGpuId.hpp and the
//  five NVIDIA headers: DEPENDENCY-FREE. No includes, no IOKit, no kernel
//  types, no allocation, no sleep. Every function is a total function of its
//  arguments, so tests/test_intel_gt_cmd.cpp compiles this IDENTICAL file under
//  MSVC on a machine with no Intel GPU, and the code path exercised on Windows
//  is the code path that would run on hardware.
//
//  ------------------------------------------------------------------------
//  EVIDENCE CLASSES -- read this before adding a constant
//  ------------------------------------------------------------------------
//
//  This header is the FIRST file in the tree that is not display. That matters
//  for provenance, because reference/i915/ was vendored for the display track
//  and its coverage of the command streamer is thin. Three classes appear
//  below and they are spelled differently on purpose:
//
//    *_I915_*    -- a value with a citable line in reference/i915/. The
//                   comment carries file:line. These are the only values in
//                   this header corroborated by anything in this checkout.
//
//    *_POLICY_*  -- a value this project chose, or took from public Gen12
//                   architecture documentation that is NOT vendored here. It
//                   is NOT a measurement and NOT citable in this tree. A
//                   POLICY value may be wrong on silicon.
//
//    (no *_OBSERVED_* constants exist in this header, and none may be added
//    without an MMIO capture of a running render engine. There is no such
//    capture: igpu-results/ holds display registers only, taken on a machine
//    whose render engine was idle. See intel-handoff28.md section 4d -- a
//    register read on a running engine is a SAMPLE, not a constant.)
//
//  The distinction is the only evidence trail this project has. A POLICY value
//  renamed to *_OBSERVED_* to look better-founded destroys it.
//
//  ------------------------------------------------------------------------
//  WHAT THIS HEADER IS AND IS NOT
//  ------------------------------------------------------------------------
//
//  IS:   the arithmetic and the refusals for handing a batch buffer to the
//        Gen12 render command streamer. Ring geometry, wrap, free space,
//        command word composition, submission admissibility.
//
//  IS NOT: acceleration. Metal on macOS needs an IOAccelerator family plus a
//        userspace driver plugin against private, unversioned Apple
//        interfaces. PLAN.md section 0 calls that the wall. This header is the
//        kernel-side command path underneath such a thing, which is a
//        prerequisite rather than the thing itself. Nothing here draws a
//        triangle, and nothing here has executed on any machine.
//

#ifndef IntelGtCmd_hpp
#define IntelGtCmd_hpp

// ---------------------------------------------------------------------------
// Widths. Same spelling as IntelDisplay.hpp so the two headers can be included
// together in one translation unit without a typedef conflict -- both use the
// identical underlying types, so a redeclaration is legal C++.
// ---------------------------------------------------------------------------

typedef unsigned int IGU32;
typedef unsigned long long IGU64;

// ---------------------------------------------------------------------------
// Engine bases.
//
// CITED. reference/i915/i915_reg.h:289-319 defines the Gen12 engine MMIO
// bases; RENDER_RING_BASE is 0x02000, BLT_RING_BASE 0x22000,
// GEN11_BSD_RING_BASE 0x1c0000, GEN11_VEBOX_RING_BASE 0x1c8000.
//
// The ring registers sit at a fixed stride ABOVE the engine base. i915 spells
// this indirectly rather than as a named offset: i915_reg.h defines
// PRB0_BASE as (0x2030 - 0x30), i.e. the render engine's ring block begins
// 0x30 above RENDER_RING_BASE, and the four ring registers follow at 0x30,
// 0x34, 0x38, 0x3C. Those four offsets are therefore CITED by arithmetic on a
// vendored line rather than by a vendored name.
// ---------------------------------------------------------------------------

#define IG_GT_RENDER_RING_BASE_I915_0x02000     0x02000u  // i915_reg.h:289
#define IG_GT_BLT_RING_BASE_I915_0x22000        0x22000u  // i915_reg.h:~300
#define IG_GT_BSD_RING_BASE_I915_0x1C0000       0x1C0000u // i915_reg.h:~310 (GEN11)
#define IG_GT_VEBOX_RING_BASE_I915_0x1C8000     0x1C8000u // i915_reg.h:~319 (GEN11)

// Ring register offsets from the engine base. See the PRB0_BASE note above.
#define IG_GT_RING_TAIL_OFFSET_I915_0x30        0x30u
#define IG_GT_RING_HEAD_OFFSET_I915_0x34        0x34u
#define IG_GT_RING_START_OFFSET_I915_0x38       0x38u
#define IG_GT_RING_CTL_OFFSET_I915_0x3C         0x3Cu

// ---------------------------------------------------------------------------
// Engine selectors. The driver's own numbering, not hardware's. Deliberately
// starting at 0 with a LIMIT, matching the verdict-family idiom in
// IntelDisplay.hpp so an exhaustiveness loop can bound on the limit and redden
// on the commit that grows the family.
// ---------------------------------------------------------------------------

#define IG_GT_ENGINE_RENDER             0u
#define IG_GT_ENGINE_BLT                1u
#define IG_GT_ENGINE_BSD                2u
#define IG_GT_ENGINE_VEBOX              3u
#define IG_GT_ENGINE_LIMIT              4u

// A selector outside the family answers this, never a base of zero -- zero is
// a legal MMIO offset and would be indistinguishable from a real answer.
#define IG_GT_BASE_INVALID              0xFFFFFFFFu

// ---------------------------------------------------------------------------
// Ring geometry policy.
//
// POLICY. The hardware constraint that a ring buffer is 4 KiB-aligned, a whole
// number of pages, and at most 2 MiB is Gen-architecture documentation, not a
// value in this checkout. Nothing in reference/i915/ states it.
//
// The reason a MINIMUM exists at all is the wrap reserve below: a ring smaller
// than the reserve cannot hold a single submission plus its wrap padding, so
// accepting one would produce a ring that is arithmetically valid and unusable.
// ---------------------------------------------------------------------------

#define IG_GT_PAGE_POLICY_0x1000                0x1000u
#define IG_GT_RING_MIN_BYTES_POLICY_0x1000      0x1000u
#define IG_GT_RING_MAX_BYTES_POLICY_0x200000    0x200000u

// Every command word is 4 bytes and the tail pointer is a byte offset that
// must land on a word boundary. CITED by construction: MI command headers in
// i915 are 32-bit words throughout (i915_reg.h MI_* family).
#define IG_GT_CMD_WORD_BYTES_I915_4             4u

// The tail must additionally be 8-byte aligned on Gen12: the command streamer
// fetches in qword pairs, so a tail on an odd word can publish half a command.
// POLICY -- not stated in this checkout.
#define IG_GT_TAIL_ALIGN_POLICY_8               8u

// Bytes at the top of the ring that a submission may never consume, so that
// HEAD == TAIL always means EMPTY and never means FULL. Without a reserve the
// two states are indistinguishable and the engine either spins on an empty
// ring or overruns a full one.
//
// POLICY. The VALUE is this project's choice; the NECESSITY is a property of a
// head/tail ring and is argued rather than cited.
#define IG_GT_WRAP_RESERVE_POLICY_0x40          0x40u

// ---------------------------------------------------------------------------
// MI command opcodes.
//
// CITABLE as of 2026-08-26. The comment that stood here said reference/i915/
// contains MI_ARB_* and MI_MODE but NOT the MI command opcode family, because
// those live in i915's gt/ headers "which were never vendored here". That was
// true when written and is false now: intel_gpu_commands.h was vendored on
// 2026-08-26 alongside intel_lrc_reg.h and intel_engine_regs.h, and it carries
// every opcode this block spells.
//
// The values did not move. All four were correct as guessed; what changed is
// that they stopped being guesses, so the _POLICY_ spelling became a lie in
// the direction this project cares about least but tolerates least -- a value
// with an upstream line marked as one without.
//
// Upstream composes them through MI_INSTR(opcode, flags), which is
// __INSTR(INSTR_MI_CLIENT) | (opcode) << 23 | (flags) -- so the opcode is the
// FIRST argument of the MI_INSTR call on each cited line, and the client and
// the shift below are cited from the same lines.
// ---------------------------------------------------------------------------

// Bits 31:29 of a command word select the client; the MI client is 0.
// intel_gpu_commands.h:22 -- INSTR_MI_CLIENT 0x0
#define IG_GT_MI_CLIENT_I915_0                  0u

// Bits 28:23 are the MI opcode, per MI_INSTR at intel_gpu_commands.h:37.
#define IG_GT_MI_OPCODE_NOOP_I915_0x00          0x00u  // :42  MI_NOOP
#define IG_GT_MI_OPCODE_BB_START_I915_0x31      0x31u  // :186 MI_BATCH_BUFFER_START
#define IG_GT_MI_OPCODE_STORE_DWORD_I915_0x20   0x20u  // :135 MI_STORE_DWORD_IMM
#define IG_GT_MI_OPCODE_BB_END_I915_0x0A        0x0Au  // :62  MI_BATCH_BUFFER_END

// ---------------------------------------------------------------------------
// Submission verdicts.
//
// A verdict family rather than a bool, for the reason 3050-handoff34.md
// section 1 measured: `== false` is satisfied by every refusal at once, so a
// bool assertion cannot tell a fix from the bug it fixed.
//
// ZERO IS NOT RETURNED. IG_GT_SUBMIT_NOT_ATTEMPTED is the caller's
// initialiser, so a structure that was never filled in reads as
// NOT_ATTEMPTED rather than as OK. Same convention as the NVGSP_* families.
// ---------------------------------------------------------------------------

#define IG_GT_SUBMIT_NOT_ATTEMPTED      0u  // caller's initialiser, never returned
#define IG_GT_SUBMIT_OK                 1u  // admissible
#define IG_GT_SUBMIT_BAD_ENGINE         2u  // selector outside the engine family
#define IG_GT_SUBMIT_RING_UNALIGNED     3u  // ring base or size not page-aligned
#define IG_GT_SUBMIT_RING_TOO_SMALL     4u  // ring below the wrap reserve
#define IG_GT_SUBMIT_RING_TOO_LARGE     5u  // ring above the policy ceiling
#define IG_GT_SUBMIT_HEAD_OUT_OF_RING   6u  // HEAD read back outside its own ring
#define IG_GT_SUBMIT_TAIL_OUT_OF_RING   7u  // TAIL outside its own ring
#define IG_GT_SUBMIT_TAIL_UNALIGNED     8u  // TAIL not on the qword boundary
#define IG_GT_SUBMIT_EMPTY              9u  // zero bytes offered
#define IG_GT_SUBMIT_UNALIGNED_LENGTH   10u // length not a whole command word
#define IG_GT_SUBMIT_NO_SPACE           11u // will not fit behind the reserve
#define IG_GT_SUBMIT_LIMIT              12u

// ---------------------------------------------------------------------------
// Engine base selection.
//
// A switch, not arithmetic. The four Gen12 engine bases are NOT evenly spaced
// -- render 0x02000, blitter 0x22000, then a jump to 0x1C0000 and 0x1C8000 --
// so a _PICK_EVEN-style stride would produce a plausible wrong base, which is
// the same trap handoff14 section 2 records for transcoders.
// ---------------------------------------------------------------------------

static inline IGU32 igGtEngineBase(IGU32 engine)
{
    switch (engine) {
        case IG_GT_ENGINE_RENDER: return IG_GT_RENDER_RING_BASE_I915_0x02000;
        case IG_GT_ENGINE_BLT:    return IG_GT_BLT_RING_BASE_I915_0x22000;
        case IG_GT_ENGINE_BSD:    return IG_GT_BSD_RING_BASE_I915_0x1C0000;
        case IG_GT_ENGINE_VEBOX:  return IG_GT_VEBOX_RING_BASE_I915_0x1C8000;
        default:                  return IG_GT_BASE_INVALID;
    }
}

// The four ring registers for an engine. Each returns IG_GT_BASE_INVALID for a
// selector outside the family rather than composing an offset from an invalid
// base -- IG_GT_BASE_INVALID + 0x30 is a valid-looking small number and would
// be dereferenced.

static inline IGU32 igGtRingReg(IGU32 engine, IGU32 ringOffset)
{
    IGU32 base = igGtEngineBase(engine);
    if (base == IG_GT_BASE_INVALID) {
        return IG_GT_BASE_INVALID;
    }
    return base + ringOffset;
}

static inline IGU32 igGtRingTailReg(IGU32 engine)
{
    return igGtRingReg(engine, IG_GT_RING_TAIL_OFFSET_I915_0x30);
}

static inline IGU32 igGtRingHeadReg(IGU32 engine)
{
    return igGtRingReg(engine, IG_GT_RING_HEAD_OFFSET_I915_0x34);
}

static inline IGU32 igGtRingStartReg(IGU32 engine)
{
    return igGtRingReg(engine, IG_GT_RING_START_OFFSET_I915_0x38);
}

static inline IGU32 igGtRingCtlReg(IGU32 engine)
{
    return igGtRingReg(engine, IG_GT_RING_CTL_OFFSET_I915_0x3C);
}

// ---------------------------------------------------------------------------
// Ring geometry.
// ---------------------------------------------------------------------------

static inline bool igGtIsPageAligned(IGU64 value)
{
    return (value & (IGU64)(IG_GT_PAGE_POLICY_0x1000 - 1u)) == 0ull;
}

// A ring size is admissible if it is page-aligned, at least the minimum, and
// at most the ceiling. Tested as a SIZE rather than folded into the submission
// check so a test can pin each bound independently.
static inline IGU32 igGtRingSizeVerdict(IGU32 sizeBytes)
{
    if (!igGtIsPageAligned((IGU64)sizeBytes)) {
        return IG_GT_SUBMIT_RING_UNALIGNED;
    }
    // Emptiness before the bound: a zero size trivially satisfies "below the
    // minimum" and a minimum-first order would make the alignment arm
    // unreachable for it. Zero IS page-aligned, so it reaches here.
    if (sizeBytes < IG_GT_RING_MIN_BYTES_POLICY_0x1000) {
        return IG_GT_SUBMIT_RING_TOO_SMALL;
    }
    if (sizeBytes > IG_GT_RING_MAX_BYTES_POLICY_0x200000) {
        return IG_GT_SUBMIT_RING_TOO_LARGE;
    }
    return IG_GT_SUBMIT_OK;
}

// Bytes available for a submission, given a head and tail already known to be
// inside a ring of ringBytes.
//
// The reserve is subtracted, so the answer is what a submission may CONSUME
// rather than the raw gap. Returning the raw gap and expecting every caller to
// subtract is the shape that produces a full ring reported as empty.
//
// Wrap: when tail is at or behind head the free run is head - tail; when tail
// is ahead of head the run continues to the end of the ring and around.
static inline IGU32 igGtRingFreeBytes(IGU32 headBytes, IGU32 tailBytes, IGU32 ringBytes)
{
    if (ringBytes <= IG_GT_WRAP_RESERVE_POLICY_0x40) {
        return 0u;
    }
    if (headBytes >= ringBytes || tailBytes >= ringBytes) {
        return 0u;
    }

    IGU32 gap;
    if (tailBytes < headBytes) {
        gap = headBytes - tailBytes;
    } else {
        gap = (ringBytes - tailBytes) + headBytes;
    }

    if (gap <= IG_GT_WRAP_RESERVE_POLICY_0x40) {
        return 0u;
    }
    return gap - IG_GT_WRAP_RESERVE_POLICY_0x40;
}

// Where the tail lands after consuming lengthBytes, wrapping inside the ring.
// Returns the ring size itself for an out-of-ring input rather than a modular
// answer -- a wrapped result computed from a bad input is indistinguishable
// from a good one.
static inline IGU32 igGtRingAdvance(IGU32 tailBytes, IGU32 lengthBytes, IGU32 ringBytes)
{
    if (ringBytes == 0u || tailBytes >= ringBytes) {
        return ringBytes;
    }
    IGU64 sum = (IGU64)tailBytes + (IGU64)lengthBytes;
    return (IGU32)(sum % (IGU64)ringBytes);
}

// ---------------------------------------------------------------------------
// Submission admissibility.
//
// ARM ORDER IS LOAD-BEARING and it is the reverse of the intuitive one in one
// place: the ring's own geometry is checked BEFORE the head/tail read back
// from it, because a head outside a ring whose size is itself nonsense should
// be reported as the bad ring -- the thing a driver author can fix -- rather
// than as a bad head.
// ---------------------------------------------------------------------------

static inline IGU32 igGtSubmitVerdict(IGU32 engine,
                                      IGU32 ringBytes,
                                      IGU32 headBytes,
                                      IGU32 tailBytes,
                                      IGU32 lengthBytes)
{
    if (igGtEngineBase(engine) == IG_GT_BASE_INVALID) {
        return IG_GT_SUBMIT_BAD_ENGINE;
    }

    IGU32 sizeVerdict = igGtRingSizeVerdict(ringBytes);
    if (sizeVerdict != IG_GT_SUBMIT_OK) {
        return sizeVerdict;
    }

    if (headBytes >= ringBytes) {
        return IG_GT_SUBMIT_HEAD_OUT_OF_RING;
    }
    if (tailBytes >= ringBytes) {
        return IG_GT_SUBMIT_TAIL_OUT_OF_RING;
    }
    if ((tailBytes % IG_GT_TAIL_ALIGN_POLICY_8) != 0u) {
        return IG_GT_SUBMIT_TAIL_UNALIGNED;
    }

    // Emptiness FIRST, then the word-multiple test. A zero length is trivially
    // a whole number of command words, so a multiple-first order would make
    // IG_GT_SUBMIT_EMPTY unreachable -- the dead-arm shape 3050-handoff32.md
    // section 3 exists to remove.
    if (lengthBytes == 0u) {
        return IG_GT_SUBMIT_EMPTY;
    }
    if ((lengthBytes % IG_GT_CMD_WORD_BYTES_I915_4) != 0u) {
        return IG_GT_SUBMIT_UNALIGNED_LENGTH;
    }

    if (lengthBytes > igGtRingFreeBytes(headBytes, tailBytes, ringBytes)) {
        return IG_GT_SUBMIT_NO_SPACE;
    }

    return IG_GT_SUBMIT_OK;
}

// An EQUALITY against OK, never a negation of an error arm. `!= BAD_ENGINE` is
// true for every other refusal and would report a ring overrun as admissible
// -- 3050-handoff32.md section 3's reasoning, one subsystem over.
static inline bool igGtSubmitAdmissible(IGU32 verdict)
{
    return verdict == IG_GT_SUBMIT_OK;
}

// ---------------------------------------------------------------------------
// MI command word composition.
//
// POLICY encodings -- see the opcode block above. Composition is separated
// from the opcodes so a test can pin the BIT PLACEMENT independently of the
// opcode values, which is the half that stays correct if an opcode turns out
// wrong on silicon.
// ---------------------------------------------------------------------------

#define IG_GT_MI_CLIENT_SHIFT_POLICY_29         29u
#define IG_GT_MI_OPCODE_SHIFT_POLICY_23         23u
#define IG_GT_MI_OPCODE_MASK_POLICY_0x3F        0x3Fu
#define IG_GT_MI_LENGTH_MASK_POLICY_0xFF        0xFFu

// Command header word. dwordLength is the excess length field: the number of
// dwords AFTER the header, which is what the streamer expects, and it is
// masked rather than checked because an over-long value must not silently
// corrupt the opcode field above it.
static inline IGU32 igGtMiHeader(IGU32 opcode, IGU32 dwordLength)
{
    IGU32 word = (IG_GT_MI_CLIENT_I915_0 << IG_GT_MI_CLIENT_SHIFT_POLICY_29);
    word |= (opcode & IG_GT_MI_OPCODE_MASK_POLICY_0x3F) << IG_GT_MI_OPCODE_SHIFT_POLICY_23;
    word |= (dwordLength & IG_GT_MI_LENGTH_MASK_POLICY_0xFF);
    return word;
}

static inline IGU32 igGtMiNoop(void)
{
    return igGtMiHeader(IG_GT_MI_OPCODE_NOOP_I915_0x00, 0u);
}

static inline IGU32 igGtMiBatchBufferEnd(void)
{
    return igGtMiHeader(IG_GT_MI_OPCODE_BB_END_I915_0x0A, 0u);
}

// MI_BATCH_BUFFER_START with a 64-bit address: header plus two address dwords,
// so the excess dword length is 1 (the field counts total dwords minus 2).
#define IG_GT_MI_BB_START_DWORDS_POLICY_3       3u
#define IG_GT_MI_BB_START_EXCESS_POLICY_1       1u

static inline IGU32 igGtMiBatchBufferStart(void)
{
    return igGtMiHeader(IG_GT_MI_OPCODE_BB_START_I915_0x31,
                        IG_GT_MI_BB_START_EXCESS_POLICY_1);
}

// The two address dwords. A batch buffer address must be 4-byte aligned; the
// low two bits of the first dword are field space in the real encoding, so an
// unaligned address does not merely misfetch, it sets flags. The splitters
// therefore refuse rather than truncate.
#define IG_GT_BB_ADDR_ALIGN_POLICY_4            4u

static inline bool igGtBatchAddressOk(IGU64 address)
{
    return (address % (IGU64)IG_GT_BB_ADDR_ALIGN_POLICY_4) == 0ull;
}

static inline IGU32 igGtBatchAddressLow(IGU64 address)
{
    if (!igGtBatchAddressOk(address)) {
        return 0u;
    }
    return (IGU32)(address & 0xFFFFFFFFull);
}

static inline IGU32 igGtBatchAddressHigh(IGU64 address)
{
    if (!igGtBatchAddressOk(address)) {
        return 0u;
    }
    return (IGU32)((address >> 32) & 0xFFFFFFFFull);
}

// Total bytes a batch-buffer-start submission consumes in the ring, so a
// caller can ask igGtSubmitVerdict() about it before writing anything.
static inline IGU32 igGtBatchStartBytes(void)
{
    return IG_GT_MI_BB_START_DWORDS_POLICY_3 * IG_GT_CMD_WORD_BYTES_I915_4;
}

// ---------------------------------------------------------------------------
// Verdict names. Kept HERE rather than in IntelDisplayNames.hpp because that
// file includes IntelDisplay.hpp and IntelFramebuffer.hpp for the constants it
// switches on, and adding a third include would couple the display name file
// to the command path. The arm-per-verdict exhaustiveness rule from
// handoff20.md section 1 applies: every verdict below the family's own LIMIT
// must have an arm, and a family that grows reddens on the commit that grows
// it.
// ---------------------------------------------------------------------------

static inline const char *igGtSubmitVerdictName(IGU32 verdict)
{
    switch (verdict) {
        case IG_GT_SUBMIT_NOT_ATTEMPTED:    return "not attempted";
        case IG_GT_SUBMIT_OK:               return "ok";
        case IG_GT_SUBMIT_BAD_ENGINE:       return "bad engine selector";
        case IG_GT_SUBMIT_RING_UNALIGNED:   return "ring not page aligned";
        case IG_GT_SUBMIT_RING_TOO_SMALL:   return "ring below minimum";
        case IG_GT_SUBMIT_RING_TOO_LARGE:   return "ring above ceiling";
        case IG_GT_SUBMIT_HEAD_OUT_OF_RING: return "head outside ring";
        case IG_GT_SUBMIT_TAIL_OUT_OF_RING: return "tail outside ring";
        case IG_GT_SUBMIT_TAIL_UNALIGNED:   return "tail not qword aligned";
        case IG_GT_SUBMIT_EMPTY:            return "empty submission";
        case IG_GT_SUBMIT_UNALIGNED_LENGTH: return "length not a command word multiple";
        case IG_GT_SUBMIT_NO_SPACE:         return "insufficient ring space";
        default:                            return "unknown submit verdict";
    }
}

static inline const char *igGtEngineName(IGU32 engine)
{
    switch (engine) {
        case IG_GT_ENGINE_RENDER: return "render";
        case IG_GT_ENGINE_BLT:    return "blitter";
        case IG_GT_ENGINE_BSD:    return "bsd";
        case IG_GT_ENGINE_VEBOX:  return "vebox";
        default:                  return "unknown engine";
    }
}

#endif /* IntelGtCmd_hpp */
