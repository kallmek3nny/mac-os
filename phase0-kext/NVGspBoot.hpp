//
//  NVGspBoot.hpp
//  Phase 4 -- the GSP bringup state machine for GA107 (RTX 3050 Mobile).
//
//  Like NVDecode.hpp, NVFalcon.hpp and NVRiscvUcode.hpp, this file is
//  DELIBERATELY DEPENDENCY-FREE apart from those two headers: no IOKit, no
//  kernel types, no allocation, no sleeping. Every access to the GPU goes
//  through a caller-supplied read32/write32 callback pair, so tests/ compiles
//  the identical file under MSVC and drives it against a simulated register
//  file with no GPU and no macOS.
//
//  Derived from PHASE4-GSP-BRINGUP.md section 5, which is itself derived from
//  mmio-boot.log (357,495 lines, captured 2026-08-06 22:41 UTC). Every
//  register constant below is a symbol from NVFalcon.hpp -- nothing here
//  open-codes an address, because the two real bugs this project has had were
//  both open-coded constants.
//
//  WHAT THIS DOES NOT DO: it does not load firmware. Steps 3-4 of the spec
//  (DMA the GSP bootloader image into IMEM) are the Phase 5 boundary. The
//  machine reaches that state and STOPS with a distinct result rather than
//  pretending a load happened. Faking it would produce a state machine that
//  passes its own tests and hangs on real silicon.
//

#ifndef NVGspBoot_hpp
#define NVGspBoot_hpp

#include "NVDecode.hpp"
#include "NVFalcon.hpp"


// ===========================================================================
// BCR_CTRL start value.
//
// The trace writes 0x111 to NV_PRISCV_RISCV_BCR_CTRL (absolute 0x111668) to
// release the core. That decomposes as three bits:
//
//   bit 0  VALID       -- NVFalcon.hpp spells this
//   bit 4  CORE_SELECT -- NVFalcon.hpp spells this (1 = RISC-V, not Falcon)
//   bit 8  BRFETCH     -- NVFalcon.hpp spells this too, at
//                         NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_SHIFT, which
//                         NVIDIA's dev_riscv_pri.h publishes as 8:8
//
// Rather than write a bare 0x111 and lose the derivation, every bit is built
// from the named shift it comes from.
//
// CORRECTION (2026-08-10): this comment previously claimed NVFalcon.hpp did
// NOT spell BRFETCH, and defined a local shift on that basis. It does spell
// it, and always did. The local alias is kept so the composition below reads
// uniformly, but it is no longer an unverified literal -- it now aliases the
// Falcon symbol, and tools/check-registers.py's "BRFETCH shift agreement"
// check fails if the two ever diverge.
// ===========================================================================

#define NVGSP_BCR_CTRL_BRFETCH_SHIFT                                          \
    NV_PRISCV_RISCV_BCR_CTRL_BRFETCH_SHIFT

#define NVGSP_BCR_CTRL_START_VALUE                                            \
    ((NVU32)NV_PRISCV_RISCV_BCR_CTRL_VALID_TRUE                               \
         << NV_PRISCV_RISCV_BCR_CTRL_VALID_SHIFT)                             \
    | ((NVU32)1u << NV_PRISCV_RISCV_BCR_CTRL_CORE_SELECT_SHIFT)               \
    | ((NVU32)1u << NVGSP_BCR_CTRL_BRFETCH_SHIFT)


// ===========================================================================
// Observed boot-ROM parameters, straight out of the trace (spec section 3.4).
// ===========================================================================

#define NVGSP_OBSERVED_PARAADDR0                 0x000005A4u
#define NVGSP_OBSERVED_ENGIDMASK                 0x00000400u
#define NVGSP_OBSERVED_UCODE_ID                  0x00000009u


// ===========================================================================
// Phase 5 -- the DMA transfer that pushes a firmware image into IMEM.
//
// The trace (spec section 3.3) writes DMATRFCMD = 0x614 as the trigger for
// every chunk. PHASE4-GSP-BRINGUP.md decodes that value into four fields --
// IMEM=1, WRITE=0, SIZE=6 (256B), CTXDMA=0 -- which produce 0x610, NOT 0x614.
//
// The missing 0x4 is bit 2, the low bit of SEC (3:2), so the observed command
// also carries SEC = 1. That field is absent from the spec's decode table.
// The value below is composed from named NVFalcon.hpp fields INCLUDING SEC so
// it reproduces the observed 0x614 exactly, and tests/test_gsp_boot.cpp pins
// that equality -- if anyone "corrects" this to 0x610 the suite fails and says
// why. Dropping SEC would issue a non-secure transfer into a Falcon that the
// boot ROM is about to signature-check.
// ===========================================================================

#define NVGSP_DMATRFCMD_LOAD_IMEM                                             \
    (((NVU32)1u << NV_PFALCON_FALCON_DMATRFCMD_IMEM_SHIFT)                    \
     | ((NVU32)1u << NV_PFALCON_FALCON_DMATRFCMD_SEC_SHIFT)                   \
     | ((NVU32)NV_PFALCON_FALCON_DMATRFCMD_SIZE_256B                          \
        << NV_PFALCON_FALCON_DMATRFCMD_SIZE_SHIFT))

//
// SIZE_256B means each trigger moves 256 bytes, which is why the trace's
// MOFFS advances by 0x100 per iteration.
//
#define NVGSP_DMA_CHUNK_BYTES                    256u

//
// The image to transfer. The caller owns the buffer, pins it, and hands over
// the PHYSICAL address -- this header never sees a pointer, because a state
// machine that dereferenced host memory could not run against a simulated
// register file.
//
struct NVGspFirmwareImage {
    NVU32 physAddrLo;   // bits 31:0 of the physical base
    NVU32 physAddrHi;   // bits 63:32 of the physical base
    NVU32 sizeBytes;    // must be a whole number of 256-byte chunks
    NVU32 imemOffset;   // destination offset within IMEM, usually 0
    bool  valid;        // false = no image available; Phase 4 stops honestly
};

//
// DMATRFBASE holds the physical address shifted right by 8, i.e. bits 39:8.
//
// BASE1 IS A CONTINUATION OF THE SHIFTED VALUE, NOT AN OVERLAY ON THE
// UNSHIFTED ONE. An earlier revision of this file masked the UNSHIFTED high
// word (physHi & 0x1FF), reading BASE1 as physical bits 40:32 and therefore
// OVERLAPPING the 39:32 the shifted BASE already carries. That was wrong, and
// it was invisible below 4 GiB: the observed trace base is 0xFFEEE200, so
// physHi was 0 and both readings emitted the identical pair of writes. The
// divergence starts at exactly 0x1_0000_0000.
//
// Two independent pieces of NVIDIA's own material settle it:
//
//   - reference/.../ampere/ga102/dev_falcon_v4.h defines
//     NV_PFALCON_FALCON_DMATRFBASE1_BASE as 8:0 -- a field of the REGISTER,
//     with no claim about which physical bits land in it.
//   - reference/.../ampere/kernel_gsp_falcon_ga102.c:122-123 writes
//         DMATRFBASE  = NvU64_LO32(srcPhysAddr >> 8)
//         DMATRFBASE1 = NvU64_HI32(srcPhysAddr >> 8) & 0x1FF
//     Both halves come from the SAME shifted quantity. BASE1 therefore carries
//     bits 40:32 OF THE SHIFTED ADDRESS, i.e. physical bits 48:40, and the
//     pair is a plain non-overlapping 64-bit split of (phys >> 8).
//
// So the correct expression shifts before masking. Verified numerically
// against the reference expression: identical for every address below 4 GiB
// (which is why the trace could not catch it), and equal at 0x1_0000_0000,
// 0x1_2345_6700, 2^40 and 2^41-256 where the old form produced BASE1 = 1,
// 1, 0x100 and 0x1FF against the correct 0, 0, 1 and 1.
//
// NOTE ON THE ADDRESSING WINDOW: the comment at NVFalcon.hpp:217 and
// NV_GSP_FW_ADDR_BITS in NVGspStage.hpp describe the pair as reaching 41 bits.
// Under this reading the register pair can express far more, but 41 remains
// the window this driver ALLOWS, and it is the conservative direction -- it
// refuses addresses the hardware might have accepted and never admits one it
// would truncate. Widening it is a separate decision that needs hardware
// evidence, not an arithmetic fix.
//
// Which bits of the UNSHIFTED high word the register pair can actually carry.
//
// nvGspDmaTrfBase() shifts physHi left by 24 into a 32-bit register, so only
// physHi's low 8 bits survive there. nvGspDmaTrfBase1() takes physHi >> 8 and
// masks it with the 9-bit BASE1 field. Together that is 8 + 9 = 17 bits of
// physHi, i.e. physical bits 48:32.
//
// DERIVED, NOT WRITTEN DOWN. The 9 comes from
// NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK rather than from a literal, because a
// literal repeating a field width goes stale silently the moment the field
// definition is corrected. The 0xFF is the 32-bit register minus the 24-bit
// shift and belongs to nvGspDmaTrfBase() above.
//
// This is a REPRESENTABILITY bound, not the policy ceiling. NVGspStage.hpp's
// NV_GSP_FW_ADDR_BITS (41) is stricter and is what the kext's allocation path
// enforces; this header cannot reference it, because NVGspStage.hpp includes
// THIS file and not the other way round. The two are deliberately independent:
// the stage header refuses what this driver ALLOWS, and the check here refuses
// what the REGISTERS CANNOT EXPRESS. An address failing only the stage check is
// a policy refusal; one failing this check would be silently truncated into a
// valid-looking address pointing at the wrong page.
//
#define NVGSP_DMA_PHYSHI_MASK \
    (0xFFu | ((NVU32)NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK << 8))

//
// True when every bit of `physHi` survives the packing into BASE and BASE1.
//
// Spelled as a predicate rather than inlined at the one call site so a test can
// drive it directly with the boundary values -- 0x1FFFF passes and 0x20000 does
// not -- instead of having to reach them through a full state-machine run.
//
static inline bool nvGspDmaPhysHiFits(NVU32 physHi)
{
    return (physHi & ~(NVU32)NVGSP_DMA_PHYSHI_MASK) == 0u;
}

static inline NVU32 nvGspDmaTrfBase(NVU32 physLo, NVU32 physHi)
{
    return (physLo >> 8) | (physHi << 24);
}

//
// `physHi` is the UNSHIFTED high word, matching nvGspDmaTrfBase()'s parameter.
// The >> 8 here is what makes this a continuation of the shifted address
// rather than an overlay on the unshifted one. Do not remove it: without it
// every address at or above 4 GiB programs a BASE1 that overlaps bits BASE
// already carries, and the engine reads the wrong page WITHOUT FAULTING.
//
static inline NVU32 nvGspDmaTrfBase1(NVU32 physHi)
{
    return (physHi >> 8) & (NVU32)NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK;
}


// ===========================================================================
// States and results.
// ===========================================================================

enum NVGspBootState {
    NVGSP_STATE_INIT = 0,      // nothing touched yet
    NVGSP_STATE_PROBE,         // read CPUCTL + HWCFG2, assert preconditions
    NVGSP_STATE_RESET_ASSERT,  // FALCON_ENGINE RESET = 1
    NVGSP_STATE_RESET_DEASSERT,// FALCON_ENGINE RESET = 0
    NVGSP_STATE_SCRUB_WAIT,    // poll HWCFG2 until MEM_SCRUBBING reads 0
    NVGSP_STATE_FIRMWARE_LOAD, // decide: no image -> stop; image -> DMA
    NVGSP_STATE_FBIF_CFG,      // TRANSCFG(0) then CTL: legalise physical DMA
    NVGSP_STATE_DMA_SETUP,     // program DMATRFBASE / DMATRFBASE1
    NVGSP_STATE_DMA_XFER,      // per chunk: MOFFS, FBOFFS, then CMD (trigger)
    NVGSP_STATE_DMA_DRAIN,     // poll DMATRFCMD until IDLE
    NVGSP_STATE_BROM_PARAMS,   // PARAADDR, ENGIDMASK, UCODE_ID, then MOD_SEL
    NVGSP_STATE_CORE_START,    // BCR_CTRL = 0x111
    NVGSP_STATE_ACTIVE_WAIT,   // poll RISC-V CPUCTL for ACTIVE_STAT
    NVGSP_STATE_RUNNING,       // terminal, success
    NVGSP_STATE_STOPPED        // terminal, see result
};

enum NVGspBootResult {
    NVGSP_RESULT_INPROGRESS = 0,

    // Terminal, and fine.
    NVGSP_RESULT_RUNNING,

    // Terminal, and expected: we got as far as Phase 4 can go without a
    // firmware image. NOT a failure -- it is the honest end of this phase.
    NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED,

    // Terminal failures.
    NVGSP_RESULT_ERR_NOT_HALTED,
    NVGSP_RESULT_ERR_NOT_RISCV_CAPABLE,
    NVGSP_RESULT_ERR_SCRUB_TIMEOUT,
    NVGSP_RESULT_ERR_ACTIVE_TIMEOUT,
    NVGSP_RESULT_ERR_DMA_TIMEOUT,
    NVGSP_RESULT_ERR_FIRMWARE_INVALID,

    //
    // The RISC-V error latches, SPLIT.
    //
    // nvRiscvErrorVerdict() distinguishes five readings of the PRIV/HUB pair
    // and nvRiscvHasError() is a one-line wrapper that answers `true` for four
    // of them at once. Stopping on that bool reported a boot halted by a
    // REFUSED READ as a latched card fault -- a claim about the CARD made from
    // evidence that is actually a claim about this DRIVER.
    //
    // NV3050Probe.cpp's inspect path already draws this exact line: its
    // NOT-bootable arm splits "the error latches did not decode" out of the
    // fault arm, in those words. The boot path did not, and the two paths read
    // the same two registers on the same card.
    //
    //   ERR_RISCV_FAULT           both halves decoded, at least one latch set.
    //                             A fact about the card; an engine reset is
    //                             the action.
    //   ERR_RISCV_ERR_UNREADABLE  at least one half answered with the refusal
    //                             value. A fact about this driver's mapping,
    //                             and the one somebody can go and fix.
    //
    // The finer three (PRIV, HUB, BOTH) deliberately get NO result codes of
    // their own -- they ride in NVGspBoot::errVerdict and are published beside
    // the two words they decode. A result code answers "what stopped the boot";
    // the verdict answers "which latch". Multiplying result codes for the
    // second question would put a card-fault taxonomy inside an enum whose
    // every other member describes this machine's own progress.
    //
    NVGSP_RESULT_ERR_RISCV_FAULT,
    NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE,

    NVGSP_RESULT_ERR_APERTURE_VIOLATION,
    NVGSP_RESULT_ERR_WRITES_DISABLED,
    NVGSP_RESULT_ERR_NO_IO,
    NVGSP_RESULT_ERR_STEP_BUDGET
};


// ===========================================================================
// I/O plumbing. The machine never touches hardware itself.
// ===========================================================================

typedef NVU32 (*NVGspRead32Fn)(void *ctx, NVU32 bar0Offset);
typedef void  (*NVGspWrite32Fn)(void *ctx, NVU32 bar0Offset, NVU32 value);

struct NVGspBootIo {
    NVGspRead32Fn  read32;
    NVGspWrite32Fn write32;
    void          *ctx;

    //
    // The dry-run gate. When false the machine performs every read and every
    // check but refuses the first write, stopping with ERR_WRITES_DISABLED.
    // NV3050Probe.cpp defaults this to false: the kext has never written to
    // this GPU and that stays true until a human flips it deliberately.
    //
    bool allowWrites;
};

struct NVGspBootParams {
    NVU32 paraAddr0;
    NVU32 engIdMask;
    NVU32 ucodeId;
    NVU32 modSelAlgo;
    NVU32 bcrCtrl;

    NVU32 maxScrubPolls;
    NVU32 maxActivePolls;
    NVU32 maxDmaPolls;

    //
    // The firmware image to push into IMEM. With `valid` false the machine
    // stops at FIRMWARE_NOT_IMPLEMENTED exactly as it did before Phase 5 --
    // that path is unchanged, and is still what a caller with no blob gets.
    //
    NVGspFirmwareImage image;

    //
    // Skip the Phase 5 firmware-load state instead of stopping there. This
    // exists so tests can exercise BROM_PARAMS -> CORE_START -> ACTIVE_WAIT,
    // which are fully specified by the trace. It must never be set on real
    // hardware: starting a core whose IMEM was never filled is exactly the
    // hang this project is trying not to ship.
    //
    bool skipFirmwareLoad;
};

// ===========================================================================
// Which diagnostic words this run actually READ.
// ===========================================================================
//
// NVGspBoot zero-initialises cpuctl, hwcfg2, riscvCpuctl, privErrStat and
// hubErrStat, and each one is assigned ONLY inside the state that reads it. A
// machine that stops at PROBE -- core not halted, say -- never reaches
// ACTIVE_WAIT and never calls nvGspBootCheckRiscvErrors(), so three of those
// five words are still the initialiser's zero when the run ends.
//
// A consumer cannot tell that from a register that genuinely read zero. That
// is handoff18 section 1d's class exactly: a published literal is
// byte-identical to a published measurement, so an operator reading the
// IORegistry sees three RISC-V status words reporting all-clear when nothing
// ever reached for them. The machine therefore records WHICH words it read,
// and a consumer is expected to ask before believing one.
//
// A bitmask rather than five bools because the words are published as a GROUP
// and because one word tells an operator the shape of the run at a glance.
// The bits are an enum rather than #defines on purpose: tools/check-registers.py
// parses #defines out of this header and checks them against NVIDIA's, and
// these are not hardware constants -- they describe this driver's own bookkeeping.
//
enum NVGspBootReadBit {
    NVGSP_READ_CPUCTL        = 0x01,
    NVGSP_READ_HWCFG2        = 0x02,
    NVGSP_READ_RISCV_CPUCTL  = 0x04,
    NVGSP_READ_PRIV_ERR_STAT = 0x08,
    NVGSP_READ_HUB_ERR_STAT  = 0x10
};

struct NVGspBoot {
    NVGspBootIo      io;
    NVGspBootParams  params;

    NVGspBootState   state;
    NVGspBootResult  result;

    NVU32 scrubPolls;
    NVU32 activePolls;
    NVU32 dmaPolls;      // polls spent waiting on the CURRENT chunk
    NVU32 dmaPollsTotal; // polls spent across every chunk

    NVU32 chunkCount;    // chunks the image needs
    NVU32 chunksDone;    // chunks confirmed IDLE
    NVU32 dmatrfcmd;     // last DMATRFCMD read back

    // Last values read, kept for diagnostics and for the caller's log.
    //
    // Each is meaningful ONLY when its NVGSP_READ_* bit is set in readMask.
    // Zero here means "never read" just as often as it means "read zero".
    NVU32 cpuctl;
    NVU32 hwcfg2;
    NVU32 riscvCpuctl;
    NVU32 privErrStat;
    NVU32 hubErrStat;

    //
    // The DECODE of the pair above, as an NVRISCV_ERR_* verdict.
    //
    // Meaningful ONLY when BOTH NVGSP_READ_PRIV_ERR_STAT and
    // NVGSP_READ_HUB_ERR_STAT are set in readMask; nvGspBootErrVerdictKnown()
    // is that conjunction and is what a publisher must consult. The
    // initialiser value is NVRISCV_ERR_NONE, which is ALSO the healthy
    // reading, so an unguarded read of this field reports a run that never
    // reached the error check as one that checked and found nothing -- the
    // fabricated-publication class this struct's readMask exists to prevent.
    //
    NVU32 errVerdict;

    // Set of NVGspBootReadBit values this run actually completed a read for.
    NVU32 readMask;

    NVU32 writeCount;
    NVU32 modSelWriteIndex;  // writeCount at the moment MOD_SEL was written
    NVU32 bcrWriteIndex;     // writeCount at the moment BCR_CTRL was written

    NVU32 faultingOffset;    // set on APERTURE_VIOLATION
};

//
// Did this run actually read the word behind `bit`?
//
// A null machine answers false -- the same refuse-rather-than-trust direction
// the rest of this header takes. A caller that cannot see the machine has not
// measured anything, and reporting "read" for it would be the exact fabrication
// this family exists to prevent.
//
static inline bool nvGspBootWasRead(const NVGspBoot *m, NVU32 bit)
{
    return (m != 0) && ((m->readMask & bit) != 0);
}

//
// Is NVGspBoot::errVerdict a MEASUREMENT?
//
// A conjunction over both halves rather than either one, because the verdict
// is a decode of the PAIR: a run that read PRIV_ERR_STAT and then stopped on
// the HUB read leaves errVerdict at its initialiser, and that initialiser is
// NVRISCV_ERR_NONE -- byte-identical to a card that genuinely reported both
// latches clear. The whole readMask family exists because zero is ambiguous;
// this verdict has the same ambiguity one level up, so it gets the same guard.
//
// A null machine answers false, same refuse-rather-than-trust direction as
// nvGspBootWasRead() above.
//
static inline bool nvGspBootErrVerdictKnown(const NVGspBoot *m)
{
    return nvGspBootWasRead(m, NVGSP_READ_PRIV_ERR_STAT)
        && nvGspBootWasRead(m, NVGSP_READ_HUB_ERR_STAT);
}


// ===========================================================================
// Internals.
// ===========================================================================

static inline void nvGspBootStop(NVGspBoot *m, NVGspBootResult r)
{
    m->state  = (r == NVGSP_RESULT_RUNNING) ? NVGSP_STATE_RUNNING
                                            : NVGSP_STATE_STOPPED;
    m->result = r;
}

static inline bool nvGspBootTerminal(const NVGspBoot *m)
{
    return m->state == NVGSP_STATE_RUNNING || m->state == NVGSP_STATE_STOPPED;
}

//
// Every read and every write funnels through these two. The aperture check is
// not paranoia: a wrong base constant here would scribble on an unrelated
// engine, and the whole reason NVFalcon.hpp carries nvIsInGspAperture() is
// that the GSP and SEC2 windows are easy to confuse.
//
static inline bool nvGspBootRead(NVGspBoot *m, NVU32 absolute, NVU32 *out)
{
    if (m->io.read32 == 0) {
        nvGspBootStop(m, NVGSP_RESULT_ERR_NO_IO);
        return false;
    }
    if (!nvIsInGspAperture(absolute)) {
        m->faultingOffset = absolute;
        nvGspBootStop(m, NVGSP_RESULT_ERR_APERTURE_VIOLATION);
        return false;
    }
    *out = m->io.read32(m->io.ctx, absolute);
    return true;
}

static inline bool nvGspBootWrite(NVGspBoot *m, NVU32 absolute, NVU32 value)
{
    if (m->io.write32 == 0) {
        nvGspBootStop(m, NVGSP_RESULT_ERR_NO_IO);
        return false;
    }
    if (!nvIsInGspAperture(absolute)) {
        m->faultingOffset = absolute;
        nvGspBootStop(m, NVGSP_RESULT_ERR_APERTURE_VIOLATION);
        return false;
    }
    if (!m->io.allowWrites) {
        m->faultingOffset = absolute;
        nvGspBootStop(m, NVGSP_RESULT_ERR_WRITES_DISABLED);
        return false;
    }
    m->io.write32(m->io.ctx, absolute, value);
    m->writeCount++;
    return true;
}

//
// Read both RISC-V error status registers and stop if either is set.
// Returns false if the machine stopped (either from a fault or from a failed
// read), true if it is clean to continue.
//
static inline bool nvGspBootCheckRiscvErrors(NVGspBoot *m)
{
    if (!nvGspBootRead(m, nvGspRiscvReg(NV_PRISCV_RISCV_PRIV_ERR_STAT),
                       &m->privErrStat)) {
        return false;
    }
    m->readMask |= NVGSP_READ_PRIV_ERR_STAT;

    if (!nvGspBootRead(m, nvGspRiscvReg(NV_PRISCV_RISCV_HUB_ERR_STAT),
                       &m->hubErrStat)) {
        return false;
    }
    m->readMask |= NVGSP_READ_HUB_ERR_STAT;

    //
    // Both halves are in hand, so the verdict is a measurement from here on.
    // Recorded BEFORE any stop, so the stopping path publishes the same decode
    // the continuing path does -- a verdict written only on the clean arm would
    // be absent from exactly the run an operator is reading it for.
    //
    m->errVerdict = nvRiscvErrorVerdict(m->privErrStat, m->hubErrStat);

    //
    // UNREADABLE is tested FIRST and gets its own result code.
    //
    // nvRiscvErrorVerdict() already gives UNREADABLE precedence over a latch in
    // the other half, so this order agrees with the verdict rather than
    // second-guessing it -- but the order is load-bearing here for the reason
    // handoff22 s1b gives: an unreadable half is a bug in THIS driver and a set
    // latch is a fact about the card, so a run that is both is reported as the
    // one somebody can go and fix.
    //
    // An EQUALITY against NVRISCV_ERR_UNREADABLE, never a negation of
    // NVRISCV_ERR_NONE. A sixth verdict added to that family later would be
    // swept into the unreadable arm by a negation and reported as a mapping
    // bug it has nothing to do with.
    //
    if (m->errVerdict == NVRISCV_ERR_UNREADABLE) {
        nvGspBootStop(m, NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE);
        return false;
    }

    if (m->errVerdict != NVRISCV_ERR_NONE) {
        nvGspBootStop(m, NVGSP_RESULT_ERR_RISCV_FAULT);
        return false;
    }
    return true;
}


// ===========================================================================
// Construction.
// ===========================================================================

static inline NVGspBootParams nvGspBootObservedParams(void)
{
    NVGspBootParams p;
    p.paraAddr0        = NVGSP_OBSERVED_PARAADDR0;
    p.engIdMask        = NVGSP_OBSERVED_ENGIDMASK;
    p.ucodeId          = NVGSP_OBSERVED_UCODE_ID;
    p.modSelAlgo       = (NVU32)NV_PFALCON2_FALCON_MOD_SEL_ALGO_RSA3K;
    p.bcrCtrl          = (NVU32)(NVGSP_BCR_CTRL_START_VALUE);

    //
    // The trace shows ~197 ms between BROM setup and core start and ~721 ms
    // for the boot itself, but neither is a fixed delay -- the real driver
    // polls. These budgets are poll counts, not milliseconds, and the caller
    // decides how long a poll takes.
    //
    p.maxScrubPolls    = 1000u;
    p.maxActivePolls   = 100000u;

    //
    // A 256-byte chunk completes in microseconds; a budget of 1000 polls per
    // chunk is generous and still bounds a stuck transfer.
    //
    p.maxDmaPolls      = 1000u;

    //
    // No image by default. A caller that has one fills this in; a caller that
    // does not gets the honest FIRMWARE_NOT_IMPLEMENTED stop.
    //
    p.image.physAddrLo = 0;
    p.image.physAddrHi = 0;
    p.image.sizeBytes  = 0;
    p.image.imemOffset = 0;
    p.image.valid      = false;

    p.skipFirmwareLoad = false;
    return p;
}

static inline void nvGspBootInit(NVGspBoot *m,
                                 NVGspBootIo io,
                                 NVGspBootParams params)
{
    m->io               = io;
    m->params           = params;
    m->state            = NVGSP_STATE_INIT;
    m->result           = NVGSP_RESULT_INPROGRESS;
    m->scrubPolls       = 0;
    m->activePolls      = 0;
    m->dmaPolls         = 0;
    m->dmaPollsTotal    = 0;
    m->chunkCount       = 0;
    m->chunksDone       = 0;
    m->dmatrfcmd        = 0;
    m->cpuctl           = 0;
    m->hwcfg2           = 0;
    m->riscvCpuctl      = 0;
    m->privErrStat      = 0;
    m->hubErrStat       = 0;
    //
    // NVRISCV_ERR_NONE is also the HEALTHY verdict, which is why this field is
    // guarded by nvGspBootErrVerdictKnown() rather than read bare. Initialising
    // it to a sixth "never decoded" value would put a state in this variable
    // that nvRiscvErrorVerdict() can never return, and the name switch in
    // NVFalcon.hpp would have nothing to call it.
    //
    m->errVerdict       = NVRISCV_ERR_NONE;
    m->readMask         = 0;
    m->writeCount       = 0;
    m->modSelWriteIndex = 0;
    m->bcrWriteIndex    = 0;
    m->faultingOffset   = 0;
}


// ===========================================================================
// The state machine. One transition per call, no sleeping, no allocation.
//
// Returns true while there is more to do, false once terminal. The caller
// owns the delay between calls -- in the kext that is IODelay(); in tests it
// is nothing at all.
// ===========================================================================

static inline bool nvGspBootStep(NVGspBoot *m)
{
    if (nvGspBootTerminal(m)) {
        return false;
    }

    switch (m->state) {

    case NVGSP_STATE_INIT:
        m->state = NVGSP_STATE_PROBE;
        return true;

    //
    // Step 1: the core must be halted and RISC-V capable before we touch it.
    //
    // Observed: CPUCTL 0x110100 = 0x00000010 (HALTED), HWCFG2 0x1100F4 =
    // 0x000047F7. Note 0x47F7 has bit 12 CLEAR, so MEM_SCRUBBING reads 0,
    // which in this register's inverted sense means scrubbing is already
    // DONE. nvFalconScrubbingDone() encodes that inversion.
    //
    case NVGSP_STATE_PROBE:
        if (!nvGspBootRead(m, nvGspFalconReg(NV_PFALCON_FALCON_CPUCTL),
                           &m->cpuctl)) {
            return false;
        }
        m->readMask |= NVGSP_READ_CPUCTL;

        if (!nvFalconIsHalted(m->cpuctl)) {
            nvGspBootStop(m, NVGSP_RESULT_ERR_NOT_HALTED);
            return false;
        }
        if (!nvGspBootRead(m, nvGspFalconReg(NV_PFALCON_FALCON_HWCFG2),
                           &m->hwcfg2)) {
            return false;
        }
        m->readMask |= NVGSP_READ_HWCFG2;

        if (!nvFalconIsRiscvCapable(m->hwcfg2)) {
            nvGspBootStop(m, NVGSP_RESULT_ERR_NOT_RISCV_CAPABLE);
            return false;
        }
        m->state = NVGSP_STATE_RESET_ASSERT;
        return true;

    //
    // Step 2: pulse FALCON_ENGINE reset 1 -> 0, then re-poll scrubbing.
    // Two states rather than one so a test can observe the intermediate
    // write and so the caller can insert a delay between the edges.
    //
    case NVGSP_STATE_RESET_ASSERT:
        if (!nvGspBootWrite(m, nvGspFalconReg(NV_PFALCON_FALCON_ENGINE),
                            (NVU32)1u << NV_PGSP_FALCON_ENGINE_RESET_SHIFT)) {
            return false;
        }
        m->state = NVGSP_STATE_RESET_DEASSERT;
        return true;

    case NVGSP_STATE_RESET_DEASSERT:
        if (!nvGspBootWrite(m, nvGspFalconReg(NV_PFALCON_FALCON_ENGINE), 0u)) {
            return false;
        }
        m->state = NVGSP_STATE_SCRUB_WAIT;
        return true;

    //
    // Reset restarts the memory scrub. Waiting for the wrong sense here means
    // spinning forever on a Falcon that was ready the whole time.
    //
    case NVGSP_STATE_SCRUB_WAIT:
        if (!nvGspBootRead(m, nvGspFalconReg(NV_PFALCON_FALCON_HWCFG2),
                           &m->hwcfg2)) {
            return false;
        }
        if (nvFalconScrubbingDone(m->hwcfg2)) {
            m->state = NVGSP_STATE_FIRMWARE_LOAD;
            return true;
        }
        m->scrubPolls++;
        if (m->scrubPolls >= m->params.maxScrubPolls) {
            nvGspBootStop(m, NVGSP_RESULT_ERR_SCRUB_TIMEOUT);
            return false;
        }
        return true;

    //
    // Steps 3-4: DMA the bootloader image into IMEM via DMATRFBASE /
    // DMATRFMOFFS / DMATRFFBOFFS / DMATRFCMD. That needs a signed firmware
    // blob, an IODMACommand, and a physical address -- all Phase 5. Stopping
    // here with a named result is the whole point: the alternative is a
    // machine that reports success having loaded nothing.
    //
    case NVGSP_STATE_FIRMWARE_LOAD:
        //
        // Three ways out, in priority order.
        //
        // 1. A real image -> do the transfer. This is the Phase 5 path.
        // 2. No image, skipFirmwareLoad set -> TEST ONLY, jump straight to the
        //    BROM parameters. On hardware this releases a core with empty
        //    IMEM; it exists so the post-load states can be exercised at all.
        // 3. No image, no skip -> stop honestly. This is what every caller
        //    without a blob still gets, unchanged from Phase 4.
        //
        if (m->params.image.valid) {
            //
            // The transfer moves whole 256-byte chunks. A size that is not a
            // multiple of that would silently drop its tail, so it is refused
            // rather than rounded -- a short image fails the boot ROM's
            // signature check anyway, and a named error says why.
            //
            if (m->params.image.sizeBytes == 0u
                || (m->params.image.sizeBytes % NVGSP_DMA_CHUNK_BYTES) != 0u) {
                nvGspBootStop(m, NVGSP_RESULT_ERR_FIRMWARE_INVALID);
                return false;
            }

            //
            // DMATRFBASE carries the physical address shifted right by 8, so
            // an address that is not 256-byte aligned cannot be expressed at
            // all -- the low bits are simply not in the register.
            //
            if ((m->params.image.physAddrLo % NVGSP_DMA_CHUNK_BYTES) != 0u) {
                nvGspBootStop(m, NVGSP_RESULT_ERR_FIRMWARE_INVALID);
                return false;
            }

            //
            // The HIGH half has to fit too, and nothing above checked it.
            //
            // physAddrHi is 32 bits wide in NVGspFirmwareImage, but the
            // register pair carries only 17 of them (NVGSP_DMA_PHYSHI_MASK).
            // A high word above that is not rejected by the packing -- it is
            // TRUNCATED by it, and the engine is then handed a well-formed
            // address pointing at the wrong page, which does not fault.
            //
            // This is the same class of bug as the earlier BASE1 overlay: both
            // are invisible below 4 GiB, because every value the trace
            // witnessed had physAddrHi == 0.
            //
            if (!nvGspDmaPhysHiFits(m->params.image.physAddrHi)) {
                nvGspBootStop(m, NVGSP_RESULT_ERR_FIRMWARE_INVALID);
                return false;
            }

            m->chunkCount = m->params.image.sizeBytes / NVGSP_DMA_CHUNK_BYTES;
            m->chunksDone = 0u;
            m->state      = NVGSP_STATE_FBIF_CFG;
            return true;
        }

        if (!m->params.skipFirmwareLoad) {
            nvGspBootStop(m, NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED);
            return false;
        }
        m->state = NVGSP_STATE_BROM_PARAMS;
        return true;

    //
    // Legalise the transfer before programming it. DMATRFBASE carries a raw
    // PHYSICAL address, and a Falcon will only honour that if FBIF says so:
    // TRANSCFG(0) selects the coherent-sysmem target in physical mode, and CTL
    // sets ALLOW_PHYS_NO_CTX so the engine does not demand a context DMA it was
    // never given. Without this step the DMA either faults or moves nothing,
    // and the register trace still shows the CMD write -- which is why the gap
    // survived Phase 4: every write looked right.
    //
    // ORDER MATTERS, and it is the trace's order (PHASE4-GSP-BRINGUP.md 4a):
    // TRANSCFG first, then CTL. Enabling the permission before the target is
    // configured opens a window where a physical transfer is legal against an
    // aperture setting that has not been written yet.
    //
    case NVGSP_STATE_FBIF_CFG:
        if (!nvGspBootWrite(m, nvGspFalconReg(NV_PFALCON_FBIF_TRANSCFG(0)),
                            NV_PFALCON_FBIF_OBSERVED_TRANSCFG0)) {
            return false;
        }
        if (!nvGspBootWrite(m, nvGspFalconReg(NV_PFALCON_FBIF_CTL),
                            NV_PFALCON_FBIF_OBSERVED_CTL)) {
            return false;
        }
        m->state = NVGSP_STATE_DMA_SETUP;
        return true;

    //
    // Program the transfer base once. Both halves are written even when the
    // high word is zero, because the trace writes both and leaving BASE1
    // holding a stale value from an earlier transfer would retarget every
    // chunk that follows.
    //
    case NVGSP_STATE_DMA_SETUP:
        if (!nvGspBootWrite(m, nvGspFalconReg(NV_PFALCON_FALCON_DMATRFBASE),
                            nvGspDmaTrfBase(m->params.image.physAddrLo,
                                            m->params.image.physAddrHi))) {
            return false;
        }
        if (!nvGspBootWrite(m, nvGspFalconReg(NV_PFALCON_FALCON_DMATRFBASE1),
                            nvGspDmaTrfBase1(m->params.image.physAddrHi))) {
            return false;
        }
        m->state = NVGSP_STATE_DMA_XFER;
        return true;

    //
    // One chunk. ORDER MATTERS: MOFFS, then FBOFFS, then CMD -- the transfer
    // starts on the CMD write, so writing CMD first would trigger a transfer
    // using the PREVIOUS chunk's offsets. NVFalcon.hpp:179 records that the
    // trace reads ambiguously here and that the driver disassembly settled it;
    // the journal assertions in tests/test_gsp_boot.cpp pin the order so a
    // future "tidy-up" cannot quietly reverse it.
    //
    case NVGSP_STATE_DMA_XFER:
        {
            NVU32 chunkOff = m->chunksDone * NVGSP_DMA_CHUNK_BYTES;

            if (!nvGspBootWrite(m,
                    nvGspFalconReg(NV_PFALCON_FALCON_DMATRFMOFFS),
                    m->params.image.imemOffset + chunkOff)) {
                return false;
            }
            if (!nvGspBootWrite(m,
                    nvGspFalconReg(NV_PFALCON_FALCON_DMATRFFBOFFS),
                    chunkOff)) {
                return false;
            }
            if (!nvGspBootWrite(m,
                    nvGspFalconReg(NV_PFALCON_FALCON_DMATRFCMD),
                    (NVU32)NVGSP_DMATRFCMD_LOAD_IMEM)) {
                return false;
            }

            m->dmaPolls = 0u;
            m->state    = NVGSP_STATE_DMA_DRAIN;
            return true;
        }

    //
    // Wait for the engine to finish this chunk before programming the next.
    // Without this the offsets for chunk N+1 land while chunk N is still in
    // flight, which is the off-by-one NVFalcon.hpp:191 warns about: it does
    // not fault, it just moves the wrong bytes.
    //
    case NVGSP_STATE_DMA_DRAIN:
        if (!nvGspBootRead(m, nvGspFalconReg(NV_PFALCON_FALCON_DMATRFCMD),
                           &m->dmatrfcmd)) {
            return false;
        }
        if (nvFalconDmaIdle(m->dmatrfcmd)) {
            m->chunksDone++;
            m->state = (m->chunksDone >= m->chunkCount)
                           ? NVGSP_STATE_BROM_PARAMS
                           : NVGSP_STATE_DMA_XFER;
            return true;
        }
        m->dmaPolls++;
        m->dmaPollsTotal++;
        if (m->dmaPolls >= m->params.maxDmaPolls) {
            nvGspBootStop(m, NVGSP_RESULT_ERR_DMA_TIMEOUT);
            return false;
        }
        return true;

    //
    // Step 5: the boot ROM parameter block. ORDER MATTERS. MOD_SEL is the
    // latch; writing it before the other three commits stale parameters and
    // the boot ROM rejects the signature. Trace order, preserved exactly:
    // PARAADDR, ENGIDMASK, CURR_UCODE_ID, MOD_SEL.
    //
    case NVGSP_STATE_BROM_PARAMS:
        if (!nvGspBootWrite(m,
                nvGspRiscvReg(NV_PFALCON2_FALCON_BROM_PARAADDR(0)),
                m->params.paraAddr0)) {
            return false;
        }
        if (!nvGspBootWrite(m,
                nvGspRiscvReg(NV_PFALCON2_FALCON_BROM_ENGIDMASK),
                m->params.engIdMask)) {
            return false;
        }
        if (!nvGspBootWrite(m,
                nvGspRiscvReg(NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID),
                m->params.ucodeId)) {
            return false;
        }
        if (!nvGspBootWrite(m,
                nvGspRiscvReg(NV_PFALCON2_FALCON_MOD_SEL),
                m->params.modSelAlgo)) {
            return false;
        }
        m->modSelWriteIndex = m->writeCount;
        m->state = NVGSP_STATE_CORE_START;
        return true;

    //
    // Step 6: release the core. BCR_CTRL is the only write that actually
    // starts anything; everything before it is setup.
    //
    case NVGSP_STATE_CORE_START:
        if (!nvGspBootWrite(m, nvGspRiscvReg(NV_PRISCV_RISCV_BCR_CTRL),
                            m->params.bcrCtrl)) {
            return false;
        }
        m->bcrWriteIndex = m->writeCount;
        m->state = NVGSP_STATE_ACTIVE_WAIT;
        return true;

    //
    // Step 7: poll RISC-V CPUCTL for ACTIVE_STAT. The trace shows ~721 ms
    // here. Every poll also checks the two error status registers, because a
    // core that faulted during boot will never set ACTIVE_STAT and the
    // timeout alone would not say why.
    //
    case NVGSP_STATE_ACTIVE_WAIT:
        if (!nvGspBootRead(m, nvGspRiscvReg(NV_PRISCV_RISCV_CPUCTL),
                           &m->riscvCpuctl)) {
            return false;
        }
        m->readMask |= NVGSP_READ_RISCV_CPUCTL;

        if (nvRiscvIsActive(m->riscvCpuctl)) {
            nvGspBootStop(m, NVGSP_RESULT_RUNNING);
            return false;
        }
        if (!nvGspBootCheckRiscvErrors(m)) {
            return false;
        }
        m->activePolls++;
        if (m->activePolls >= m->params.maxActivePolls) {
            nvGspBootStop(m, NVGSP_RESULT_ERR_ACTIVE_TIMEOUT);
            return false;
        }
        return true;

    default:
        nvGspBootStop(m, NVGSP_RESULT_ERR_NO_IO);
        return false;
    }
}

//
// Drive the machine to a terminal state, or give up after maxSteps. The step
// budget is a backstop against a caller passing an enormous poll budget and
// wedging the thread; it is deliberately a separate number from the per-poll
// budgets so a test can distinguish "the GPU never responded" from "the loop
// itself ran away".
//
static inline NVGspBootResult nvGspBootRun(NVGspBoot *m, NVU32 maxSteps)
{
    NVU32 steps = 0;
    while (nvGspBootStep(m)) {
        steps++;
        if (steps >= maxSteps) {
            nvGspBootStop(m, NVGSP_RESULT_ERR_STEP_BUDGET);
            break;
        }
    }
    return m->result;
}

//
// True when the run ended somewhere a caller should treat as "the hardware
// did what the trace says it does". FIRMWARE_NOT_IMPLEMENTED counts: it is
// the designed end of Phase 4, not a fault.
//
static inline bool nvGspBootReachedExpectedEnd(const NVGspBoot *m)
{
    return m->result == NVGSP_RESULT_RUNNING
        || m->result == NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED;
}

static inline const char *nvGspBootResultName(NVGspBootResult r)
{
    switch (r) {
    case NVGSP_RESULT_INPROGRESS:               return "INPROGRESS";
    case NVGSP_RESULT_RUNNING:                  return "RUNNING";
    case NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED: return "FIRMWARE_NOT_IMPLEMENTED";
    case NVGSP_RESULT_ERR_NOT_HALTED:           return "ERR_NOT_HALTED";
    case NVGSP_RESULT_ERR_NOT_RISCV_CAPABLE:    return "ERR_NOT_RISCV_CAPABLE";
    case NVGSP_RESULT_ERR_SCRUB_TIMEOUT:        return "ERR_SCRUB_TIMEOUT";
    case NVGSP_RESULT_ERR_ACTIVE_TIMEOUT:       return "ERR_ACTIVE_TIMEOUT";
    case NVGSP_RESULT_ERR_DMA_TIMEOUT:          return "ERR_DMA_TIMEOUT";
    case NVGSP_RESULT_ERR_FIRMWARE_INVALID:     return "ERR_FIRMWARE_INVALID";
    case NVGSP_RESULT_ERR_RISCV_FAULT:          return "ERR_RISCV_FAULT";
    case NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE: return "ERR_RISCV_ERR_UNREADABLE";
    case NVGSP_RESULT_ERR_APERTURE_VIOLATION:   return "ERR_APERTURE_VIOLATION";
    case NVGSP_RESULT_ERR_WRITES_DISABLED:      return "ERR_WRITES_DISABLED";
    case NVGSP_RESULT_ERR_NO_IO:                return "ERR_NO_IO";
    case NVGSP_RESULT_ERR_STEP_BUDGET:          return "ERR_STEP_BUDGET";
    default:                                    return "UNKNOWN";
    }
}

static inline const char *nvGspBootStateName(NVGspBootState s)
{
    switch (s) {
    case NVGSP_STATE_INIT:           return "INIT";
    case NVGSP_STATE_PROBE:          return "PROBE";
    case NVGSP_STATE_RESET_ASSERT:   return "RESET_ASSERT";
    case NVGSP_STATE_RESET_DEASSERT: return "RESET_DEASSERT";
    case NVGSP_STATE_SCRUB_WAIT:     return "SCRUB_WAIT";
    case NVGSP_STATE_FIRMWARE_LOAD:  return "FIRMWARE_LOAD";
    case NVGSP_STATE_FBIF_CFG:       return "FBIF_CFG";
    case NVGSP_STATE_DMA_SETUP:      return "DMA_SETUP";
    case NVGSP_STATE_DMA_XFER:       return "DMA_XFER";
    case NVGSP_STATE_DMA_DRAIN:      return "DMA_DRAIN";
    case NVGSP_STATE_BROM_PARAMS:    return "BROM_PARAMS";
    case NVGSP_STATE_CORE_START:     return "CORE_START";
    case NVGSP_STATE_ACTIVE_WAIT:    return "ACTIVE_WAIT";
    case NVGSP_STATE_RUNNING:        return "RUNNING";
    case NVGSP_STATE_STOPPED:        return "STOPPED";
    default:                         return "UNKNOWN";
    }
}

#endif /* NVGspBoot_hpp */
