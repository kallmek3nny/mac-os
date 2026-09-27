//
//  NVGspStage.hpp
//  Phase 5 -- the PURE arithmetic behind NV3050Probe::loadGspFirmware().
//
//  Like NVDecode.hpp, NVFalcon.hpp, NVRiscvUcode.hpp and NVGspBoot.hpp, this
//  file is DELIBERATELY DEPENDENCY-FREE: no IOKit, no kernel types, no
//  allocation. Every function here is a total function of its arguments, so
//  tests/ compiles the identical file under MSVC and runs it on a machine with
//  no GPU and no macOS.
//
//  WHY THIS FILE EXISTS
//
//  handoff5.md section 7 recorded the gap precisely: loadGspFirmware() lives in
//  NV3050Probe.cpp, which no test binary compiles, so its arithmetic was
//  verified ONLY by clang parsing it against the stub IOKit. Parsing proves the
//  code is well-formed; it proves nothing about whether pad-UP rounds the right
//  way, whether the overflow guard fires, or whether the address checks reject
//  what they should. Those are exactly the mistakes that produce a
//  plausible-looking image the GPU boots into garbage.
//
//  WHAT STAYS IN THE .cpp
//
//  Everything that touches IOKit: inTaskWithPhysicalMask(), prepare(),
//  IODMACommand::withSpecification(), gen64IOVMSegments(), the memcpy, and the
//  lifetime of the two retained objects. This header decides WHAT to copy and
//  WHETHER an address is acceptable; the .cpp does the copying and the asking.
//

#ifndef NVGspStage_hpp
#define NVGspStage_hpp

#include "NVDecode.hpp"
#include "NVRiscvUcode.hpp"
#include "NVGspBoot.hpp"

// ---------------------------------------------------------------------------
// The DMA addressing window, spelled once.
//
// DMATRFBASE carries physical bits 39:8. DMATRFBASE1 is a 9-bit field that
// continues the SHIFTED value, so it carries physical bits 48:40
// (NVFalcon.hpp:217, NVGspBoot.hpp:119-143). The pair can therefore express
// more than 41 bits; 41 is the window THIS DRIVER ALLOWS, and an address above
// it is refused rather than TRUNCATED into a valid-looking one that makes the
// engine read the wrong page WITHOUT FAULTING.
//
// PROVENANCE: 41 is a POLICY CEILING, not a witnessed value. The MMIO trace
// wrote DMATRFBASE1 = 0 because the observed base 0xFFEEE200 sits below 4 GiB,
// so the trace settles nothing about the high half. It was originally derived
// from a reading of BASE1 as physical bits 40:32, which OVERLAPPED the 39:32
// the shifted BASE already carries; that reading was WRONG and was corrected on
// 2026-08-10 (t-53137f). The ceiling was deliberately LEFT at 41 rather than
// widened: under the corrected reading the hardware may accept more, and
// refusing addresses the hardware would have taken is the safe direction --
// admitting one it would truncate is not. Widening this needs hardware
// evidence, not an arithmetic argument. Nothing here is named *_OBSERVED_*
// precisely because none of it came off the wire.
// ---------------------------------------------------------------------------

#define NV_GSP_FW_ADDR_BITS     41

//
// THE NEXT TWO CONSTANTS ARE NOT INTERCHANGEABLE. This is the single most
// likely thing in this file to be broken by a later edit, so it is stated at
// the definitions rather than in a comment somewhere else.
//
// NV_GSP_FW_PHYS_MASK is an ALLOCATION CONSTRAINT handed to
// IOBufferMemoryDescriptor::inTaskWithPhysicalMask(). Its low 8 bits are
// deliberately CLEARED -- that is what forces the 256-byte alignment
// DMATRFBASE's bits-39:8 encoding requires.
//
// NV_GSP_FW_PHYS_LIMIT is a MAGNITUDE CEILING with every low bit SET. It is the
// right thing to compare a returned address against.
//
// Comparing an address against the MASK reads as correct to a reviewer who sees
// "41-bit mask, 41-bit check" and is wrong: it rejects every address in the low
// 256 bytes of an otherwise-valid region. That bug was written and caught here
// on 2026-08-10; nvGspStageAddressOk() below is what keeps it caught.
//
#define NV_GSP_FW_PHYS_MASK     0x000001FFFFFFFF00ULL
#define NV_GSP_FW_PHYS_LIMIT    ((unsigned long long)((1ULL << NV_GSP_FW_ADDR_BITS) - 1ULL))

// ---------------------------------------------------------------------------
// POLICY <= REPRESENTABLE -- the invariant that makes the two bounds safe.
//
// There are now two independent address bounds in this driver and they mean
// different things:
//
//   NV_GSP_FW_PHYS_LIMIT (here)          POLICY -- what this driver ALLOWS.
//                                        41 bits, a deliberate ceiling, not a
//                                        witnessed hardware width.
//   NVGSP_DMA_PHYSHI_MASK (NVGspBoot)    REPRESENTABILITY -- what the
//                                        DMATRFBASE/BASE1 pair can CARRY.
//                                        17 bits of physHi, i.e. physical
//                                        bits 48:32, so 49 bits in total.
//
// Their relationship is the whole safety argument. It is NOT unasserted -- an
// earlier revision of this comment said it was, and that was wrong.
// tools/check-registers.py::stage_sanity_checks() already fails when
// NV_GSP_FW_ADDR_BITS exceeds the pair's reach, computing that reach from the
// DMATRFBASE/DMATRFBASE1 field widths it resolves out of NVIDIA's published
// header. That check is the stronger one on provenance: its right-hand side
// comes from the vendor, not from this project.
//
// What the predicate below adds is different in two ways. It compares against
// NVGSP_DMA_PHYSHI_MASK -- the constant the kext ACTUALLY packs with -- so it
// also catches the composed mask diverging from the vendor field the Python
// gate reads. And it is drivable from tests/test_gsp_stage.cpp, which runs on a
// bare MSVC build with no Python, so the invariant is not carried by one gate
// alone.
//
// The safe ordering is POLICY <= REPRESENTABLE: this header may
// refuse an address the registers could have carried -- that costs an
// allocation retry and says so -- but it must never ADMIT one the packing would
// silently truncate. A truncated address is well-formed and points at the wrong
// page, and the engine does not fault on it.
//
// If someone widens NV_GSP_FW_ADDR_BITS past 49 on an arithmetic argument, that
// ordering inverts and nvGspStageAddressOk() starts handing
// nvGspDmaTrfBase()/nvGspDmaTrfBase1() addresses they cannot express. The
// predicate below is what catches it, and it is spelled in terms of the OTHER
// header's constant rather than a literal 49 so it tracks the register field
// instead of a number someone typed here.
//
// The direction of the dependency is why this lives here and not in
// NVGspBoot.hpp: that file cannot see NV_GSP_FW_ADDR_BITS, because this one
// includes it and not the other way round.
// ---------------------------------------------------------------------------

//
// The largest physical address the register pair can actually express: 32 bits
// of physLo underneath the 17 bits of physHi that survive the packing.
//
#define NV_GSP_DMA_REPRESENTABLE_LIMIT \
    ((unsigned long long)(((unsigned long long)NVGSP_DMA_PHYSHI_MASK << 32) \
                          | 0xFFFFFFFFULL))

//
// True while the policy ceiling stays inside what the registers can carry.
//
// A predicate rather than a comment so a test can drive it directly, and so the
// failure has a name if NV_GSP_FW_ADDR_BITS is ever widened.
//
static inline bool nvGspStagePolicyFitsRegisters(void)
{
    return NV_GSP_FW_PHYS_LIMIT <= NV_GSP_DMA_REPRESENTABLE_LIMIT;
}

//
// Reasons a stage attempt can be refused. Every one of these is a REFUSAL, not
// a warning: the caller must not hand the GPU an address on any of them.
//
// Spelled as an enum rather than a bool so a test can assert WHICH check fired.
// A bool would let a padding bug pass a test that only asserted "rejected".
//
typedef enum {
    NVGSP_STAGE_OK = 0,

    NVGSP_STAGE_ERR_NO_BLOB,          // null pointer or zero length
    NVGSP_STAGE_ERR_MALFORMED,        // nvRiscvUcodeIsWellFormed() said no
    NVGSP_STAGE_ERR_EMPTY_BOOTLOADER, // descriptor names a zero-length region
    NVGSP_STAGE_ERR_REGION_OUTSIDE,   // region not contained in the payload
    NVGSP_STAGE_ERR_REGION_TRUNCATED, // region runs past the end of the blob
    NVGSP_STAGE_ERR_PAD_OVERFLOW,     // padding to a chunk boundary wraps u32

    NVGSP_STAGE_ERR_ADDR_MISALIGNED,  // returned base is not 256-byte aligned
    NVGSP_STAGE_ERR_ADDR_TOO_HIGH,    // returned base exceeds the 41-bit window
    NVGSP_STAGE_ERR_RANGE_TOO_HIGH    // base fits, base+length does not
} NVGspStageResult;

static inline const char *nvGspStageResultName(NVGspStageResult r)
{
    switch (r) {
    case NVGSP_STAGE_OK:                    return "OK";
    case NVGSP_STAGE_ERR_NO_BLOB:           return "ERR_NO_BLOB";
    case NVGSP_STAGE_ERR_MALFORMED:         return "ERR_MALFORMED";
    case NVGSP_STAGE_ERR_EMPTY_BOOTLOADER:  return "ERR_EMPTY_BOOTLOADER";
    case NVGSP_STAGE_ERR_REGION_OUTSIDE:    return "ERR_REGION_OUTSIDE";
    case NVGSP_STAGE_ERR_REGION_TRUNCATED:  return "ERR_REGION_TRUNCATED";
    case NVGSP_STAGE_ERR_PAD_OVERFLOW:      return "ERR_PAD_OVERFLOW";
    case NVGSP_STAGE_ERR_ADDR_MISALIGNED:   return "ERR_ADDR_MISALIGNED";
    case NVGSP_STAGE_ERR_ADDR_TOO_HIGH:     return "ERR_ADDR_TOO_HIGH";
    case NVGSP_STAGE_ERR_RANGE_TOO_HIGH:    return "ERR_RANGE_TOO_HIGH";
    }
    return "UNKNOWN";
}

//
// What the caller needs in order to perform the copy: where the bootloader
// bytes start inside the blob, how many there are, and how many bytes the
// destination buffer must be.
//
// `paddedBytes >= sourceBytes` always. The difference is the zero-fill tail --
// the caller must zero the WHOLE buffer, not just the tail, because
// uninitialised kernel memory handed to a DMA engine is an information leak as
// well as a nondeterminism.
//
struct NVGspStagePlan {
    NVU32 sourceOffset;   // byte offset into the blob where the region starts
    NVU32 sourceBytes;    // bytes to copy from the blob
    NVU32 paddedBytes;    // bytes to allocate and zero; a multiple of 256
    bool  valid;

    //
    // The EIGHT-WAY structural verdict, carried out rather than collapsed.
    //
    // nvGspStagePlan() maps this onto NVGspStageResult through the switch
    // below, and that mapping is lossy in both directions that matter: three
    // ucode verdicts land on ERR_MALFORMED and three more land on
    // ERR_REGION_OUTSIDE. A consumer holding only the stage result therefore
    // cannot tell a booter handed in by mistake (ERR_DESC_VERSION) from a
    // truncated file (ERR_DESC_SHORT), and those call for different actions.
    //
    // Carried in the PLAN rather than re-derived by the caller. NV3050Probe.cpp
    // is explicit that nothing in the shell re-derives what this header decides;
    // a second nvRiscvUcodeVerdict() call there would be a second answer that
    // can disagree with the one the driver acted on.
    //
    NVRiscvUcodeVerdict ucode;

    //
    // False until the verdict has actually been computed. Set AFTER the call
    // returns, never before -- the NVGSP_READ_* reasoning in NVGspBoot.hpp.
    //
    // This flag exists because zero is NV_RISCV_UCODE_OK. An unevaluated field
    // left at its initialiser would publish "well-formed" for a blob nobody
    // validated, which is handoff18 section 1d's fabricated publication exactly.
    // `ucode` is initialised to NV_RISCV_UCODE_VERDICT_LIMIT for the same
    // reason: it is a value nvRiscvUcodeVerdictName() refuses to name.
    //
    bool ucodeEvaluated;
};

//
// Round `bytes` UP to a whole number of NVGSP_DMA_CHUNK_BYTES.
//
// UP, not down. The engine moves one chunk per trigger, so a transfer length
// must be a whole number of chunks; truncating the last partial chunk would
// load a bootloader MISSING ITS FINAL INSTRUCTIONS, which is strictly worse
// than loading padding the core never executes.
//
// Returns false without touching *out on overflow. `bytes` is bounded by the
// blob length at every real call site so this cannot realistically trip, but
// the result is a length handed to a DMA engine and a wrapped value would be a
// silent short read rather than a refusal.
//
static inline bool nvGspStagePadUp(NVU32 bytes, NVU32 *out)
{
    if (!out) {
        return false;
    }

    NVU32 rem = bytes % (NVU32)NVGSP_DMA_CHUNK_BYTES;
    if (rem == 0u) {
        *out = bytes;
        return true;
    }

    NVU32 add = (NVU32)NVGSP_DMA_CHUNK_BYTES - rem;
    if (bytes > 0xFFFFFFFFu - add) {
        return false;
    }

    *out = bytes + add;
    return true;
}

//
// Validate `blob` and work out what to copy. No allocation, no I/O.
//
// The order of the checks is load-bearing and matches the .cpp's:
//
//   1. Structural validation FIRST, before any arithmetic on the contents. The
//      blob comes from outside the kext and a truncated or hostile one must
//      produce a refusal, not an out-of-bounds read in kernel space.
//   2. Region offsets are relative to dataOffset -- NOT to the file start and
//      NOT to the descriptor. Getting that wrong reads the DESCRIPTOR BYTES as
//      firmware, which is a plausible-looking image that boots nothing.
//   3. The file-offset re-check is not redundant with step 2. Step 2 asks
//      whether the region fits the payload the descriptor DECLARES; this asks
//      whether it fits the bytes actually present.
//
// *outPlan is left invalid on every refusal, so a caller that ignores the
// return value still cannot build a copy from it.
//
static inline NVGspStageResult nvGspStagePlan(const unsigned char *blob,
                                              NVU32 blobLen,
                                              NVGspStagePlan *outPlan)
{
    if (!outPlan) {
        return NVGSP_STAGE_ERR_NO_BLOB;
    }

    outPlan->sourceOffset   = 0;
    outPlan->sourceBytes    = 0;
    outPlan->paddedBytes    = 0;
    outPlan->valid          = false;
    outPlan->ucode          = NV_RISCV_UCODE_VERDICT_LIMIT;
    outPlan->ucodeEvaluated = false;

    if (!blob || blobLen == 0u) {
        return NVGSP_STAGE_ERR_NO_BLOB;
    }

    //
    // The structural verdict, MAPPED rather than collapsed.
    //
    // Until 2026-08-18 this was `if (!nvRiscvUcodeIsWellFormed(...))` returning
    // ERR_MALFORMED, and the two arms below it -- ERR_EMPTY_BOOTLOADER and
    // ERR_REGION_OUTSIDE -- re-read the SAME two descriptor fields the
    // well-formed gate had already refused on. Both were therefore provably
    // unreachable, and tests/test_gsp_stage.cpp said so in its own comments
    // while asserting MALFORMED for cases the enum has precise names for.
    //
    // Two dead enum arms are not a cosmetic problem. nvGspStageResultName() is
    // what NV3050Probe::loadGspFirmware() puts in its refusal log, so every
    // structurally bad blob produced the same word regardless of what was
    // actually wrong with it.
    //
    // The mapping is exhaustive over NVRiscvUcodeVerdict with no `default:`
    // arm, so a code added to that enum fails to compile here rather than
    // silently landing on ERR_MALFORMED -- handoff20 section 1's lesson, where a
    // `default:` swallowed a verdict and all four gates stayed green.
    //
    NVRiscvUcodeVerdict uv = nvRiscvUcodeVerdict(blob, blobLen);

    //
    // Recorded BEFORE the switch, so every refusal arm below carries the
    // specific verdict out with it rather than only the collapsed stage code.
    // Set after the call RETURNS, never before -- a flag set first would report
    // a verdict as computed on the one path where nothing was.
    //
    outPlan->ucode          = uv;
    outPlan->ucodeEvaluated = true;

    switch (uv) {
    case NV_RISCV_UCODE_OK:
        break;

    case NV_RISCV_UCODE_ERR_HEADER:
    case NV_RISCV_UCODE_ERR_DESC_SHORT:
    case NV_RISCV_UCODE_ERR_DESC_VERSION:
        return NVGSP_STAGE_ERR_MALFORMED;

    case NV_RISCV_UCODE_ERR_BL_EMPTY:
        return NVGSP_STAGE_ERR_EMPTY_BOOTLOADER;

    case NV_RISCV_UCODE_ERR_BL_OUTSIDE:
    case NV_RISCV_UCODE_ERR_PARAM_OUTSIDE:
    case NV_RISCV_UCODE_ERR_REGION_OUTSIDE:
        return NVGSP_STAGE_ERR_REGION_OUTSIDE;

    case NV_RISCV_UCODE_VERDICT_LIMIT:
        return NVGSP_STAGE_ERR_MALFORMED;
    }

    //
    // Past the verdict, the bootloader region is guaranteed non-empty and
    // contained in the payload. The two refusals that used to stand here were
    // re-tests of exactly that guarantee and are gone rather than kept as
    // defence in depth -- an unreachable guard is indistinguishable from a
    // deleted one, which is the whole reason this function needed changing.
    //
    NVU32 blOff  = nvRiscvDescField(blob, NV_RISCV_DESC_BOOTLOADER_OFFSET, 0);
    NVU32 blSize = nvRiscvDescField(blob, NV_RISCV_DESC_BOOTLOADER_SIZE, 0);

    NVU32 fileOff = nvRiscvRegionFileOffset(blob, blOff);
    if (!nvRiscvFits(blobLen, fileOff, blSize)) {
        return NVGSP_STAGE_ERR_REGION_TRUNCATED;
    }

    NVU32 padded = 0;
    if (!nvGspStagePadUp(blSize, &padded)) {
        return NVGSP_STAGE_ERR_PAD_OVERFLOW;
    }

    outPlan->sourceOffset = fileOff;
    outPlan->sourceBytes  = blSize;
    outPlan->paddedBytes  = padded;
    outPlan->valid        = true;

    return NVGSP_STAGE_OK;
}

//
// Check a physical base the MAPPER ACTUALLY RETURNED against the two hardware
// constraints. The mask passed to the allocator is a REQUEST; this is the
// ANSWER, and the answer is what the GPU will be handed.
//
// Two separate tests on purpose:
//
//   - alignment, via a modulo against the chunk size
//   - magnitude, via NV_GSP_FW_PHYS_LIMIT
//
// Never fold these into one comparison against NV_GSP_FW_PHYS_MASK. See the
// comment at the constants.
//
static inline NVGspStageResult nvGspStageAddressOk(unsigned long long physBase)
{
    if ((physBase % (unsigned long long)NVGSP_DMA_CHUNK_BYTES) != 0ull) {
        return NVGSP_STAGE_ERR_ADDR_MISALIGNED;
    }

    if (physBase > NV_GSP_FW_PHYS_LIMIT) {
        return NVGSP_STAGE_ERR_ADDR_TOO_HIGH;
    }

    return NVGSP_STAGE_OK;
}

//
// Check the WHOLE staged image, not just where it starts.
//
// nvGspStageAddressOk() bounds the BASE alone. A chunk-aligned base a few
// hundred bytes below NV_GSP_FW_PHYS_LIMIT passes it while the TAIL of the
// image sits above the window -- and DMATRFBASE/DMATRFBASE1 are programmed
// with a base and the transfer is driven by a LENGTH, so the engine would walk
// off the end of the addressable region. Nothing above caught that, because
// every existing check takes one address and no length.
//
// The comparison is written as a subtraction on the CEILING, not an addition
// on the base. `physBase + lengthBytes` can wrap a 64-bit unsigned and produce
// a small number that passes a naive `<= LIMIT` test -- the exact shape of
// failure this function exists to refuse. physBase <= NV_GSP_FW_PHYS_LIMIT is
// already established by the call above, so (LIMIT + 1) - physBase cannot
// underflow.
//
// A lengthBytes of 0 is accepted here. That is deliberate and is NOT a claim
// that a zero-length image is loadable: nvGspStagePlan() refuses an empty
// bootloader before any address exists, and duplicating that refusal here
// would report a length fault as a RANGE fault.
//
static inline NVGspStageResult nvGspStageRangeOk(unsigned long long physBase,
                                                 unsigned long long lengthBytes)
{
    NVGspStageResult baseOk = nvGspStageAddressOk(physBase);
    if (baseOk != NVGSP_STAGE_OK) {
        return baseOk;
    }

    if (lengthBytes > (NV_GSP_FW_PHYS_LIMIT + 1ull) - physBase) {
        return NVGSP_STAGE_ERR_RANGE_TOO_HIGH;
    }

    return NVGSP_STAGE_OK;
}

//
// Split a validated physical base into the two halves NVGspFirmwareImage wants.
//
// Spelled here rather than at the call site so the shift and mask are covered
// by a test. A physAddrHi that dropped the shift would look right for every
// address below 4 GiB and silently target page zero above it.
//
static inline void nvGspStageFillImage(NVGspFirmwareImage *img,
                                       unsigned long long  physBase,
                                       NVU32               paddedBytes,
                                       NVU32               imemOffset)
{
    if (!img) {
        return;
    }

    img->physAddrLo = (NVU32)(physBase & 0xFFFFFFFFull);
    img->physAddrHi = (NVU32)(physBase >> 32);
    img->sizeBytes  = paddedBytes;
    img->imemOffset = imemOffset;
    img->valid      = true;
}

#endif /* NVGspStage_hpp */
