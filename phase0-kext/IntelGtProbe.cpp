//
//  IntelGtProbe.cpp
//
//  The Gen12 render-engine consumer. See IntelGtProbe.hpp for what this class is
//  and is not; the short version is that it maps BAR0, reads the render ring's
//  four registers ONCE, composes a logical ring context image into memory this
//  driver owns, publishes both, and writes NOTHING to the GPU.
//
//  There is no write path in this file. Not a refusing one, not a disabled one,
//  not one behind an #if. IntelGtImage.hpp composes a page whose LAYOUT this
//  project chose end to end -- reference/i915/ has no LRC file at all, measured
//  2026-08-26 -- and a policy-composed context descriptor written into a real
//  execlist port is a guess submitted to silicon. The failure mode is not a
//  refusal, it is an engine executing from an address nobody chose.
//
//  Every number handed to IOKit below comes out of a dependency-free header that
//  tests/ already drives under MSVC. There is no shift, no mask, and no
//  arithmetic over a register value anywhere in this file, deliberately: this
//  translation unit reaches IOKit, so no host suite can compile it, so anything
//  computed HERE is computed where nothing can execute it. IntelDisplayNames.hpp
//  is what that costs when it is ignored.
//
//  The read path is IntelDisplayProbe.cpp's and IntelFbPresentProbe.cpp's,
//  verbatim in shape: one regRead32() funnel, every refusal answering
//  IG_MMIO_INVALID and saying which guard fired. It is duplicated rather than
//  shared for the reason IntelFbPresentProbe.cpp gives -- three separate
//  IOService subclasses with three separate BAR0 mappings and three separate
//  lifetimes, and a shared accessor would need a shared object whose mapping
//  state none of them owns.
//

#include "IntelGtProbe.hpp"

OSDefineMetaClassAndStructors(IntelGtProbe, IOService);

#define IGLOG(fmt, ...) IOLog("IntelGtProbe: " fmt "\n", ## __VA_ARGS__)

#define super IOService

//
// ---- lifecycle -------------------------------------------------------------
//

bool IntelGtProbe::init(OSDictionary *dict)
{
    if (!super::init(dict))
        return false;

    fPCIDevice  = nullptr;
    fBar0Map    = nullptr;
    fBar0Base   = nullptr;
    fBar0Length = 0;
    fDeviceId   = 0;

    //
    // The scratch ring is cleared and both rehearsal verdicts start on their own
    // families' NOT_ATTEMPTED arm. Zero IS that arm in both, and the spelling is
    // the point: an operator reading IGGtRingVerdict 0 is reading a deliberate
    // "no attempt was made", not an initialiser that happens to look like one.
    //
    for (UInt32 i = 0; i < IG_GTP_SCRATCH_RING_DWORDS; i++) {
        fScratchRing[i] = 0;
    }
    fRingVerdict        = IG_GTR_RING_NOT_ATTEMPTED;
    fRingBatchVerdict   = IG_GTQ_BATCH_NOT_ATTEMPTED;
    fRingTailAfter      = 0;
    fRingWriteAttempted = false;

    //
    // The blit span and its verdict, same reasoning one layer over: zero IS the
    // NOT_ATTEMPTED arm and the spelling says so deliberately, and the dword
    // count cannot carry "did this run" because every refusal arm reports zero.
    //
    for (UInt32 i = 0; i < IG_GTP_BLIT_SPAN_DWORDS; i++) {
        fBlitSpan[i] = 0;
    }
    fBlitVerdict   = IG_GTB_BLIT_NOT_ATTEMPTED;
    fBlitDwords    = 0;
    fBlitAttempted = false;

    //
    // The fence span, same shape and same reason as the blit span above.
    //
    // fFenceAttempted is separate from all four reported fields because
    // igGtfWriteCompletion() writes NOTHING on any refusal: a refused
    // composition leaves these zeroes in place, and a zeroed span reads back as
    // not-ordered, address zero and not-retired -- byte-identical to a
    // composition that ran and was refused for a reason worth naming.
    //
    for (UInt32 i = 0; i < IG_GTP_FENCE_SPAN_DWORDS; i++) {
        fFenceSpan[i] = 0;
    }
    fFenceVerdict      = IG_GTF_FENCE_NOT_ATTEMPTED;
    fFenceOrdered      = false;
    fFenceComposedAddr = 0;
    fFenceRetired      = false;
    fFenceAttempted    = false;

    //
    // The four ring words are initialised to the REFUSAL sentinel rather than to
    // zero, and fSampleTaken carries "did the read happen" separately.
    //
    // Zero is a legal reading for every one of them -- igpu-results/ captured a
    // machine whose render engine was IDLE, so a zero CTL is the EXPECTED value
    // here, not a fault. An initialiser zero that reads as a measurement is the
    // exact shape that put three fabricated register words into the NVIDIA
    // track's IORegistry.
    //
    fRingHead    = IG_MMIO_INVALID;
    fRingTail    = IG_MMIO_INVALID;
    fRingStart   = IG_MMIO_INVALID;
    fRingCtl     = IG_MMIO_INVALID;
    fSampleTaken = false;

    for (UInt32 i = 0; i < IG_GTI_RING_IMAGE_DWORDS_POLICY_12; i++)
        fImage[i] = 0;

    //
    // NOT_ATTEMPTED rather than OK, and that asymmetry is the whole reason the
    // writer's verdict family opens with it: IG_GTI_WRITE_OK is 1, so a zeroed
    // verdict field lands on NOT_ATTEMPTED and says truthfully that nothing has
    // been composed yet.
    //
    fImageDwords  = 0;
    fWriteVerdict = IG_GTI_WRITE_NOT_ATTEMPTED;

    //
    // The submission rehearsal, same reasoning as the three families above and
    // one asymmetry worth spelling out. IG_GTX_READY_NOT_ATTEMPTED is 0, so the
    // zeroed verdict lands on the arm that says truthfully that no readiness
    // question was asked yet -- it is NOT the OK arm, and it is not a refusal
    // either, which are three different statements an operator has to be able to
    // tell apart.
    //
    // fCtxDescriptor starts at IG_GTX_CTXDESC_INVALID_POLICY_0, which is the one
    // sentinel in this tree that is ZERO rather than all-ones -- bit 0 is the
    // descriptor's valid bit, so a zero word is an invalid descriptor by the
    // hardware's own definition. That makes the initialiser and the composer's
    // refusal the SAME value here, which is safe only because neither can be
    // mistaken for a measurement: all-ones would instead set the valid bit over a
    // garbage address, and that is the scribble direction.
    //
    // fExeclistStatus starts at IG_MMIO_INVALID for the opposite reason, and the
    // asymmetry is deliberate: zero is a LEGAL status word meaning the port is
    // idle with nothing queued, so an initialiser zero there would be
    // indistinguishable from a register that genuinely read all-clear.
    //
    fExeclistStatus     = IG_MMIO_INVALID;
    fCtxDescriptor      = IG_GTX_CTXDESC_INVALID_POLICY_0;
    fSubmitReadyVerdict = IG_GTX_READY_NOT_ATTEMPTED;
    fSubmitRehearsed    = false;

    return true;
}

void IntelGtProbe::free(void)
{
    unmapBar0();
    OSSafeReleaseNULL(fPCIDevice);

    super::free();
}

bool IntelGtProbe::start(IOService *provider)
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

    if (!mapBar0()) {
        OSSafeReleaseNULL(fPCIDevice);
        super::stop(provider);
        return false;
    }

    //
    // THE ONE SAMPLE, and the class ATTACHES REGARDLESS of what it returns.
    //
    // That is the opposite call from IntelFbPresentProbe, which refuses to attach
    // on a bad plane -- and the difference is what the two classes claim. That one
    // promises IOKit a presentable framebuffer, so attaching over a surface it
    // cannot describe would be a promise it cannot honour. This one promises
    // nothing to anybody: it is an inspection point. Refusing here would delete
    // the IORegistry node carrying the reading that says WHY it refused.
    //
    (void) sampleRing();

    composeImage();
    rehearseRingWrite();
    composeBlit();
    rehearseSubmit();
    composeFence();

    publishRingSample();
    publishComposedImage();
    publishRingWrite();
    publishComposedBlit();
    publishSubmitRehearsal();
    publishComposedFence();

    registerService();

    IGLOG("start: complete -- read-only, nothing was submitted to the engine");
    return true;
}

void IntelGtProbe::stop(IOService *provider)
{
    IGLOG("stop");

    unmapBar0();
    OSSafeReleaseNULL(fPCIDevice);

    super::stop(provider);
}

//
// ---- the mapping -----------------------------------------------------------
//

bool IntelGtProbe::mapBar0(void)
{
    IODeviceMemory *bar =
        fPCIDevice->getDeviceMemoryWithIndex(IG_GTP_BAR0_INDEX);
    if (bar == nullptr) {
        IGLOG("mapBar0: no device memory at index %d", IG_GTP_BAR0_INDEX);
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
    // A warning rather than a refusal, the same call both display shells make:
    // declining here would delete the IORegistry node that makes a short BAR
    // inspectable. regRead32()'s past-mapping refusal is where the warning
    // acquires a consequence.
    //
    if (fBar0Length < IG_GTP_BAR0_EXPECTED) {
        IGLOG("mapBar0: WARNING BAR0 is %llu bytes, shorter than the 0x%x the "
              "GT block needs -- reads past the end are undefined, not faults",
              (unsigned long long) fBar0Length, IG_GTP_BAR0_EXPECTED);
    }

    return true;
}

void IntelGtProbe::unmapBar0(void)
{
    fBar0Base   = nullptr;
    fBar0Length = 0;
    OSSafeReleaseNULL(fBar0Map);
}

//
// ---- the read funnel -------------------------------------------------------
//
// Identical in shape to the two display shells, with ONE difference that matters
// and is deliberate: the policy-range guard is NOT applied here.
//
// igReadDecision() bounds offsets against IG_DISPLAY_POLICY_BASE..LIMIT, and the
// render ring lives at 0x2030 and below -- underneath that range entirely. Passing
// a GT offset through the display decision would refuse every read this class
// makes, correctly by its own arithmetic and wrongly about the world. So the two
// guards that DO apply are spelled here: unmapped, and past-the-mapping.
//
// The past-mapping test is offset + IG_MMIO_ACCESS_BYTES <= fBar0Length, never
// offset < fBar0Length, because an offset at length - 1 is inside the mapping by
// any naive test and still reads three bytes past the end of it.
//
UInt32 IntelGtProbe::regRead32(UInt32 offset)
{
    if (fBar0Base == nullptr) {
        IGLOG("regRead32(0x%x): BAR0 is not mapped", offset);
        return IG_MMIO_INVALID;
    }

    if (offset == IG_MMIO_INVALID) {
        IGLOG("regRead32: refusing IG_MMIO_INVALID -- an offset helper declined");
        return IG_MMIO_INVALID;
    }

    if ((IGU64) offset + (IGU64) IG_MMIO_ACCESS_BYTES > fBar0Length) {
        IGLOG("regRead32(0x%x): BAR0 is only %llu bytes -- a %u-byte read here "
              "would land past the end of the mapping, refusing",
              offset, (unsigned long long) fBar0Length, IG_MMIO_ACCESS_BYTES);
        return IG_MMIO_INVALID;
    }

    return *(volatile UInt32 *) (fBar0Base + offset);
}

//
// ---- the one sample --------------------------------------------------------
//
// Four reads of the render engine's ring block, taken once and never re-read.
//
// intel-handoff28 section 4d is the measurement behind that: two reads of one
// register inside a single capture run DISAGREED. A register read on running
// silicon is a SAMPLE, not a constant, and HEAD in particular is a live pointer
// the engine moves. Reading it twice and publishing both would put two
// descriptions of one ring in the IORegistry with no way to tell which was acted
// on.
//
// The four offsets come from igGtcRingReg(), which is where the citation lives --
// RENDER_RING_BASE at reference/i915/i915_reg.h:289, with PRB0_BASE spelled
// (0x2030 - 0x30), the subtraction being what fixes the ring block ABOVE the
// engine base rather than at it.
//
bool IntelGtProbe::sampleRing(void)
{
    //
    //  The four purpose-built accessors rather than igGtRingReg() plus an offset
    //  constant, and that is not a style choice. The sibling owns which offset is
    //  HEAD; spelling the offset here would give this tree two answers to one
    //  question, and a changed offset would move only one of the two. Same reason
    //  the image writer calls igGtMiNoop() instead of respelling the opcode.
    //
    fRingHead  = regRead32(igGtRingHeadReg(IG_GTP_ENGINE_RENDER));
    fRingTail  = regRead32(igGtRingTailReg(IG_GTP_ENGINE_RENDER));
    fRingStart = regRead32(igGtRingStartReg(IG_GTP_ENGINE_RENDER));
    fRingCtl   = regRead32(igGtRingCtlReg(IG_GTP_ENGINE_RENDER));

    //
    // Set AFTER all four reads have returned. A flag set first would report a
    // group as sampled on the one path where nothing was.
    //
    // The flag's claim is "the four reads were attempted and returned", which is
    // true regardless of WHAT they returned -- a refused read is still a read that
    // happened, and the sentinel in the value is what says it was refused.
    //
    fSampleTaken = true;

    IGLOG("sampleRing: head=0x%08x tail=0x%08x start=0x%08x ctl=0x%08x",
          fRingHead, fRingTail, fRingStart, fRingCtl);

    //
    // An idle engine is the EXPECTED reading on this machine and is reported as a
    // measurement rather than treated as a fault -- igpu-results/ captured a
    // render engine that was idle. The return value says whether all four reads
    // landed, nothing more; start() ignores it on purpose.
    //
    return fRingHead  != IG_MMIO_INVALID &&
           fRingTail  != IG_MMIO_INVALID &&
           fRingStart != IG_MMIO_INVALID &&
           fRingCtl   != IG_MMIO_INVALID;
}

//
// ---- the composition -------------------------------------------------------
//
// The image is composed into fImage, a fixed member array. Nothing is handed to
// hardware, so there is no IOBufferMemoryDescriptor here: an allocation would
// imply a page the engine could reach, and this class deliberately produces no
// such page.
//
// HEAD and TAIL come from the SAMPLE rather than from a policy value, so the
// composed image describes the ring position this machine actually reported. The
// base and the size are policy and say so in their names -- no ring is allocated,
// so there is no ring base to measure and no size the hardware chose.
//
void IntelGtProbe::composeImage(void)
{
    fWriteVerdict = igGtiWriteRingImage(fImage,
                                        IG_GTI_RING_IMAGE_DWORDS_POLICY_12,
                                        IG_GTP_LRCA_POLICY,
                                        IG_GTP_RING_BYTES_POLICY,
                                        IG_GTP_RING_BASE_POLICY,
                                        fRingHead,
                                        fRingTail,
                                        IG_GTP_FIRST_PAIR_I915_0x244,
                                        IG_GTP_FIRST_PAIR_VALUE,
                                        &fImageDwords);

    IGLOG("composeImage: verdict %u, %u dwords written",
          fWriteVerdict, fImageDwords);

    if (fWriteVerdict != IG_GTI_WRITE_OK) {
        IGLOG("composeImage: the writer refused -- the span is untouched and "
              "nothing below will publish a dword of it");
    }
}

//
// ---- what we publish -------------------------------------------------------
//

//
// ---- the ring-write rehearsal ----------------------------------------------
//
// This is the first place in the tree where IntelGtRing.hpp is CALLED from a
// translation unit that would call it for real. Everything it writes lands in
// fScratchRing, which is host memory this driver owns: no MMIO store follows,
// RING_TAIL is never written, and IGGtSubmitted is still published false.
//
// The head is the SAMPLED one when the read happened, and IG_GTP_RING_BYTES_POLICY
// minus one word otherwise. That second case matters: a refused read answers
// IG_MMIO_INVALID (0xFFFFFFFF), and handing that in as a head byte offset would
// make the writer refuse for BAD_GEOMETRY -- a verdict about a register nobody
// read, published as though it described the composition. fSampleTaken is what
// separates the two, exactly as it does for publishRingSample().
//
void IntelGtProbe::rehearseRingWrite(void)
{
    //
    // Head and tail are reduced into the ring's own span before use. The sampled
    // registers hold BYTE offsets whose top bits carry status on real silicon,
    // and a head past the end of a 4 KiB ring is not a geometry this scratch
    // buffer has -- masking is what keeps the rehearsal a statement about the
    // WRITER rather than about a register field this class does not decode.
    //
    UInt32 headBytes = 0;
    UInt32 tailBytes = 0;

    if (fSampleTaken) {
        headBytes = fRingHead % IG_GTP_RING_BYTES_POLICY;
        tailBytes = fRingTail % IG_GTP_RING_BYTES_POLICY;
    }

    //
    // The tail must be word-aligned before the writer sees it: igGtRingAdvance()
    // and the submit verdict both reason in whole dwords, and an unaligned start
    // would be refused for a reason that describes this call site rather than
    // the composition it is rehearsing.
    //
    headBytes &= ~(IG_GT_CMD_WORD_BYTES_I915_4 - 1u);
    tailBytes &= ~(IG_GT_CMD_WORD_BYTES_I915_4 - 1u);

    fRingWriteAttempted = true;

    fRingVerdict = igGtrWriteBatchStartToRing(fScratchRing,
                                              IG_GTP_SCRATCH_RING_DWORDS,
                                              IG_GTP_ENGINE_RENDER,
                                              headBytes,
                                              tailBytes,
                                              IG_GTP_BATCH_ADDR_POLICY,
                                              &fRingBatchVerdict,
                                              &fRingTailAfter);

    if (!igGtrRingSucceeded(fRingVerdict)) {
        IGLOG("rehearseRingWrite: ring writer refused (%u), batch verdict %u",
              (unsigned) fRingVerdict, (unsigned) fRingBatchVerdict);
    }
}

void IntelGtProbe::publishRingSample(void)
{
    //
    // The flag goes FIRST and unconditionally, for the reason handoff18 settled
    // on for GSPEngineReadable: a flag that appears only when something went
    // wrong is indistinguishable from an older kext that never published it. An
    // absent key is a question, not an answer.
    //
    setProperty("IGGtRingSampled", fSampleTaken);
    setProperty("IGGtEngine", (UInt64) IG_GTP_ENGINE_RENDER, 32);

    //
    // The four raw words, published whatever they hold. A refused read publishes
    // its sentinel HERE and that is correct -- these keys are the sample, and the
    // sample is what it is. What must never happen is a DECODE of a sentinel
    // published as though it were a measurement, which is why nothing below
    // decodes them.
    //
    setProperty("IGGtRingHead",  (UInt64) fRingHead,  32);
    setProperty("IGGtRingTail",  (UInt64) fRingTail,  32);
    setProperty("IGGtRingStart", (UInt64) fRingStart, 32);
    setProperty("IGGtRingCtl",   (UInt64) fRingCtl,   32);

    setProperty("IGGtBar0Bytes", (UInt64) fBar0Length, 64);
}

//
// The rehearsal's own publication, kept SEPARATE from publishComposedImage()
// rather than folded into it. The composed image and the ring write are two
// independent measurements that happen to run in sequence: the image describes a
// context page, the ring verdict describes a store into a circular buffer, and a
// single key covering both would report one as the other's failure.
//
//
//  composeBlit() -- ask IntelGtBlt.hpp to compose one XY_COLOR_BLT packet into
//  a span this driver owns.
//
//  NOTHING IS SUBMITTED. fBlitSpan is host memory, it is not the scratch ring,
//  no MMIO store follows this call, and RING_TAIL is never touched. What this
//  buys is the thing a pure header cannot have: the 2D command writer is now
//  CALLED from the translation unit that would call it for real, with its
//  verdict published where an operator can read it.
//
//  Every input is a policy constant from the header rather than a register
//  value, and deliberately so. rehearseRingWrite() above may legitimately feed
//  the ring writer a SAMPLE, because head and tail are ring geometry the engine
//  owns; a destination surface is not. There is no framebuffer here to measure,
//  so a measured input would have to be invented, and a value invented to fill a
//  field then filed next to values witnessed on hardware destroys the only
//  evidence trail this project has.
//
void IntelGtProbe::composeBlit(void)
{
    fBlitAttempted = true;

    fBlitVerdict = igGtbWriteColorBlit(fBlitSpan,
                                       IG_GTP_BLIT_SPAN_DWORDS,
                                       IG_GTP_BLIT_DST_ADDR_POLICY,
                                       IG_GTP_BLIT_DST_PITCH_POLICY,
                                       IG_GTP_BLIT_LEFT_POLICY,
                                       IG_GTP_BLIT_TOP_POLICY,
                                       IG_GTP_BLIT_RIGHT_POLICY,
                                       IG_GTP_BLIT_BOTTOM_POLICY,
                                       IG_GTP_BLIT_COLOR_POLICY,
                                       IG_GTP_BLIT_DEPTH_POLICY,
                                       &fBlitDwords);

    if (!igGtbBlitSucceeded(fBlitVerdict)) {
        IGLOG("composeBlit: 2D writer refused (%u)", (unsigned) fBlitVerdict);
    }
}

//
//  publishComposedBlit() -- the verdict, the count, and the inputs it was given.
//
//  Kept separate from publishComposedImage() and publishRingWrite() for the
//  reason those two are separate from each other: the image describes a context
//  page, the ring verdict describes a store into a circular buffer, and this
//  describes a drawing command. One key covering two of them reports one as the
//  other's failure.
//
//  The inputs are published because a verdict without them is unfalsifiable --
//  an operator reading "ok" cannot tell which rectangle was admissible.
//
void IntelGtProbe::publishComposedBlit(void)
{
    setProperty("IGGtBlitAttempted", fBlitAttempted);

    setProperty("IGGtBlitVerdict",     (UInt64) fBlitVerdict, 32);
    setProperty("IGGtBlitVerdictName", igGtbBlitVerdictName(fBlitVerdict));

    //
    //  The count is published ONLY behind the success arm.
    //
    //  igGtbWriteColorBlit() zeroes outDwordsWritten before inspecting any
    //  argument, so a refusal and a composition that never ran both report zero
    //  -- and setProperty() with a refusal's zero is byte-identical to
    //  setProperty() with a measured zero. Publishing it unconditionally is the
    //  fabricated-publication class this tree has now paid for six times.
    //
    if (igGtbBlitSucceeded(fBlitVerdict)) {
        setProperty("IGGtBlitDwords", (UInt64) fBlitDwords, 32);
    }

    setProperty("IGGtBlitSpanDwords", (UInt64) IG_GTP_BLIT_SPAN_DWORDS, 32);
    setProperty("IGGtBlitDstAddr",    (UInt64) IG_GTP_BLIT_DST_ADDR_POLICY, 64);
    setProperty("IGGtBlitDstPitch",   (UInt64) IG_GTP_BLIT_DST_PITCH_POLICY, 32);
    setProperty("IGGtBlitRectLeft",   (UInt64) IG_GTP_BLIT_LEFT_POLICY, 32);
    setProperty("IGGtBlitRectTop",    (UInt64) IG_GTP_BLIT_TOP_POLICY, 32);
    setProperty("IGGtBlitRectRight",  (UInt64) IG_GTP_BLIT_RIGHT_POLICY, 32);
    setProperty("IGGtBlitRectBottom", (UInt64) IG_GTP_BLIT_BOTTOM_POLICY, 32);

    setProperty("IGGtBlitWroteToHostMemoryOnly", true);
    setProperty("IGGtBlitSubmitted", false);
}

//
//  rehearseSubmit() -- ask IntelGtExec.hpp whether the context this class has
//  composed COULD be handed to the render engine's execlist port, and compose
//  the descriptor word that would carry it.
//
//  NOTHING IS SUBMITTED. No execlist queue register is written, EXECLIST_CONTROL
//  is never touched, and fCtxDescriptor is a UInt64 sitting in host memory this
//  driver owns. The whole of the write side of submission is absent from this
//  file, not disabled in it.
//
//  WHY THE STATUS WORD IS READ AND THE OTHER TWO ARE NOT
//  -----------------------------------------------------
//  igGtxSubmitReady() needs to know whether the port is busy, and that is a fact
//  about the machine rather than a policy this driver may choose -- so
//  EXECLIST_STATUS is READ, once, through the same funnel as the four ring words.
//  EXECLIST_SQ and EXECLIST_CONTROL are the two WRITE registers of the family and
//  are deliberately not touched at all: reading them would establish nothing the
//  readiness join consumes, and every offset this file reaches for should be one
//  a consumer can point at a use for.
//
//  A refused read answers IG_MMIO_INVALID, whose busy bit is SET, so a refusal
//  reports the port BUSY and the join refuses. That is the safe direction and it
//  is the header's own documented polarity (IntelGtExec.hpp, igGtxExeclistPortBusy)
//  rather than a property this call site invents -- but it is also the reason the
//  raw status word is published beside the verdict below: a verdict of BUSY over
//  a sentinel and a verdict of BUSY over a genuinely busy engine are two different
//  facts, and only the raw word separates them.
//
void IntelGtProbe::rehearseSubmit(void)
{
    fSubmitRehearsed = true;

    fExeclistStatus = regRead32(igGtxExeclistStatusReg(IG_GTP_ENGINE_RENDER));

    //
    // The descriptor is composed FIRST and its result kept whatever it is.
    // igGtxContextDescriptor() answers IG_GTX_CTXDESC_INVALID_POLICY_0 when its
    // inputs are inadmissible, and that sentinel is what gets published -- never a
    // zero standing in for it. Note that here the sentinel IS zero, uniquely in
    // this tree, because bit 0 is the descriptor's valid bit: a zero word is an
    // invalid descriptor by the hardware's own definition rather than by this
    // driver's convention, which is why the header chose it over all-ones.
    //
    fCtxDescriptor = igGtxContextDescriptor(IG_GTP_LRCA_POLICY,
                                            IG_GTP_CTX_SWID_POLICY);

    //
    // The join runs over the same LRCA and context id the descriptor was composed
    // from, the ring geometry this class rehearsed the writer at, and the sampled
    // status word. Handing it a different LRCA than composeImage() used would make
    // the verdict a statement about a context page nothing in this class ever
    // composed.
    //
    // The two ring offsets are RE-REDUCED here rather than taken from a member,
    // because rehearseRingWrite()'s reduced head is a local of that method and
    // fRingTailAfter is the tail the writer PRODUCED. Recomputing the head with
    // the identical expression keeps one spelling of the reduction rule in this
    // file per use site rather than two disagreeing ones -- and the sample guard
    // is the same one rehearseRingWrite() applies, for the same reason: a refused
    // read answers IG_MMIO_INVALID, and handing that in as a byte offset would
    // make the verdict a complaint about a sentinel rather than about geometry.
    //
    UInt32 headBytes = fSampleTaken ? (fRingHead % IG_GTP_RING_BYTES_POLICY) : 0u;
    headBytes &= ~(IG_GT_CMD_WORD_BYTES_I915_4 - 1u);

    fSubmitReadyVerdict = igGtxSubmitReady(IG_GTP_ENGINE_RENDER,
                                           IG_GTP_LRCA_POLICY,
                                           IG_GTP_CTX_SWID_POLICY,
                                           IG_GTP_RING_BYTES_POLICY,
                                           headBytes,
                                           fRingTailAfter,
                                           IG_GTP_SUBMIT_LENGTH_BYTES_POLICY,
                                           fExeclistStatus);

    if (!igGtxSubmitAdmissible(fSubmitReadyVerdict)) {
        IGLOG("rehearseSubmit: not ready (%u), status %08x, descriptor %llx",
              (unsigned) fSubmitReadyVerdict,
              (unsigned) fExeclistStatus,
              (unsigned long long) fCtxDescriptor);
    }
}

//
//  publishSubmitRehearsal() -- the readiness verdict, the descriptor it judged,
//  and the status word both were derived from.
//
//  Kept separate from the other four publishers for the reason each of those is
//  separate from the others: this describes whether a context could be SUBMITTED,
//  which is a different question from whether a page, a ring store or a drawing
//  command was composed correctly. One key covering two reports one as the
//  other's failure.
//
void IntelGtProbe::publishSubmitRehearsal(void)
{
    //
    // The attempted flag goes FIRST and unconditionally, same reasoning as every
    // other flag in this file: an absent key is a question, not an answer.
    //
    setProperty("IGGtSubmitRehearsed", fSubmitRehearsed);

    setProperty("IGGtSubmitReadyVerdict",     (UInt64) fSubmitReadyVerdict, 32);
    setProperty("IGGtSubmitReadyVerdictName", igGtxReadyVerdictName(fSubmitReadyVerdict));

    //
    // The raw status word is published whatever it holds, and nothing here
    // decodes it. A refused read publishes its sentinel HERE and that is correct:
    // this key IS the sample. What must never happen is a decode of a sentinel
    // published as though it were a measurement.
    //
    setProperty("IGGtExeclistStatus", (UInt64) fExeclistStatus, 32);

    //
    // The descriptor is published unconditionally and that is deliberate, unlike
    // IGGtBlitDwords above which is behind its success arm. The difference is
    // that a refusing composer returns a SENTINEL rather than a zero, so the
    // failure case is self-describing -- there is no value here that a reader
    // could mistake for a measurement that never happened.
    //
    setProperty("IGGtCtxDescriptor", fCtxDescriptor, 64);
    setProperty("IGGtCtxDescriptorValid", igGtxContextDescriptorValid(fCtxDescriptor));

    setProperty("IGGtCtxLrca", (UInt64) IG_GTP_LRCA_POLICY, 64);
    setProperty("IGGtCtxSwId", (UInt64) IG_GTP_CTX_SWID_POLICY, 32);

    //
    // The two standing denials, in the idiom the blit publisher established.
    // They are published rather than left to a reader's inference because the
    // whole class of question this driver exists to answer is "did anything
    // reach the hardware", and silence answers it ambiguously.
    //
    setProperty("IGGtSubmitted", false);
    setProperty("IGGtExeclistWritten", false);
}

void IntelGtProbe::publishRingWrite(void)
{
    //
    // The attempted flag goes FIRST and unconditionally, for the reason
    // handoff18 settled on for GSPEngineReadable and publishRingSample() repeats
    // above: a key that appears only on one arm is indistinguishable from an
    // older kext that never published it. An absent key is a question, not an
    // answer.
    //
    setProperty("IGGtRingWriteAttempted", fRingWriteAttempted);

    //
    // Both verdicts on BOTH arms, so the pair sorts above every value it
    // qualifies. The batch verdict is published SEPARATELY rather than collapsed
    // into the ring one, which is the whole reason igGtrWriteBatchStartToRing()
    // takes it as an out-param: a batch address whose low bits were wrong and a
    // ring with no room are refusals in two different layers, and an operator
    // handed one word would go looking in the wrong one.
    //
    setProperty("IGGtRingVerdict",      (UInt64) fRingVerdict,      32);
    setProperty("IGGtRingBatchVerdict", (UInt64) fRingBatchVerdict, 32);

    //
    // The names, so the registry is readable without the header open. Published
    // on both arms for the same reason the numbers are.
    //
    setProperty("IGGtRingVerdictName",      igGtrRingVerdictName(fRingVerdict));
    setProperty("IGGtRingBatchVerdictName", igGtqBatchVerdictName(fRingBatchVerdict));

    //
    // The geometry the rehearsal ran at, so the verdict above is interpretable.
    // These are POLICY constants rather than measurements and the key names carry
    // no measurement idiom; IG_GTP_SCRATCH_RING_DWORDS is published in dwords
    // because that is the unit the writer takes.
    //
    setProperty("IGGtRingScratchDwords", (UInt64) IG_GTP_SCRATCH_RING_DWORDS, 32);
    setProperty("IGGtRingBatchAddr",     (UInt64) IG_GTP_BATCH_ADDR_POLICY,   64);

    //
    // The resulting tail ONLY behind the success arm.
    //
    // This is the one value in this publisher that must be guarded, and the
    // reason is a property of the writer rather than a preference: every refusal
    // arm of igGtrWriteBatchStartToRing() reports the tail UNCHANGED, so a
    // refused write and a write that wrapped exactly back to its starting offset
    // put byte-identical values in this field. Publishing it unconditionally
    // would be a fabricated measurement in the shape the NVIDIA track has now
    // found five times -- a literal that reads as a reading.
    //
    if (igGtrRingSucceeded(fRingVerdict)) {
        setProperty("IGGtRingTailAfter", (UInt64) fRingTailAfter, 32);
    }

    //
    // THE SENTENCE THIS METHOD MUST NOT WEAKEN. The rehearsal wrote into host
    // memory this driver owns; no MMIO store followed it, RING_TAIL was never
    // touched, and nothing the GPU can reach points at fScratchRing. The banner
    // in IntelGtProbe.hpp and intel-handoff36 section 1 both still hold.
    //
    setProperty("IGGtRingWroteToHostMemoryOnly", true);
}

void IntelGtProbe::publishComposedImage(void)
{
    //
    // The verdict FIRST and on BOTH arms, so it sorts above every value it
    // qualifies and so its absence cannot be mistaken for an older kext.
    //
    setProperty("IGGtImageVerdict", (UInt64) fWriteVerdict, 32);
    setProperty("IGGtImageDwords",  (UInt64) fImageDwords,  32);

    //
    // THE SENTENCE THIS CLASS EXISTS TO PUBLISH. Unconditional, on every path,
    // and a constant rather than a derived value: no code path in this file can
    // make it false, because there is no write path in this file to make it
    // false.
    //
    setProperty("IGGtSubmitted", false);
    setProperty("IGGtImageLayoutIsPolicy", true);

    if (fWriteVerdict != IG_GTI_WRITE_OK) {
        IGLOG("publishComposedImage: verdict %u -- publishing the verdict and no "
              "dword of the span", fWriteVerdict);
        return;
    }

    //
    // The composed dwords, published as DATA rather than as twelve numbered keys.
    //
    // Twelve keys would invite reading slot N as "the value of register N", and
    // the slots are not registers -- they are an offset/value pair layout this
    // project chose. One blob makes an inspector go to IntelGtContext.hpp for the
    // map, which is where the map actually is.
    //
    setProperty("IGGtImage", (void *) fImage,
                (unsigned int) (fImageDwords * sizeof(UInt32)));
}

//
//  composeFence() -- the completion signal, composed and never fired.
//
//  WHAT THIS CLOSES
//  ----------------
//  Every other composition in this file describes work the engine would DO or a
//  way to hand that work over. None of them describes how this driver would
//  learn the work FINISHED. A submission whose completion cannot be observed is
//  indistinguishable, from the driver's side, from a submission that hung: both
//  look like "nothing has happened yet", forever.
//
//  IntelGtFence.hpp composes the flush/store pair that closes that loop. This is
//  its first and only caller.
//
//  NOTHING IS FIRED. fFenceSpan is host memory this driver owns, no MMIO store
//  follows the composition, IG_GTP_SEQNO_ADDR_POLICY names an address in an
//  invented space nothing on the GPU can reach, and no doorbell exists in this
//  file to ring. Composing a completion signal and firing one are different acts
//  and only the first is here -- the same claim IntelGtFence.hpp's own banner
//  makes, restated where it can be checked.
//
//  WHY THE ORDER IS READ BACK RATHER THAN TRUSTED
//  ----------------------------------------------
//  igGtfWriteCompletion() returns IG_GTF_FENCE_OK over either ordering. Flush
//  first and store first compose the identical SET of dwords, the identical
//  count and the identical byte length, and both are well-formed MI packets. The
//  only thing separating them is WHICH INDEX each header lands at, and a store
//  the CPU can observe before its flush retires reports "done" over render
//  caches still holding unwritten pixels -- a wrong frame whose wrongness is
//  timing-dependent.
//
//  So the verdict is not enough on its own, and igGtfCompletionIsOrdered() is
//  consulted separately and published separately. That is the whole reason that
//  predicate is caller-facing rather than a test's private assertion.
//
void IntelGtProbe::composeFence(void)
{
    //
    // Set BEFORE the composer runs, never after.
    //
    // Every other field this method assigns is a REPORT about a composition, and
    // igGtfWriteCompletion() writes nothing at all on a refusal -- so a flag set
    // afterwards would be indistinguishable from init()'s zero on exactly the
    // path where the distinction matters. Same reasoning as fBlitAttempted and
    // fRingWriteAttempted, and the same reasoning the NVIDIA track's readMask
    // bits are set only after their read returns, arrived at from the other side:
    // there the flag says a value was MEASURED, here it says a composition was
    // ATTEMPTED, and each is set at the point that makes the claim true.
    //
    fFenceAttempted = true;

    fFenceVerdict = igGtfWriteCompletion(fFenceSpan,
                                         IG_GTP_FENCE_SPAN_DWORDS,
                                         IG_GTP_SEQNO_ADDR_POLICY,
                                         IG_GTP_SEQNO_VALUE_POLICY);

    if (!igGtfFenceSucceeded(fFenceVerdict)) {
        //
        // EQUALITY against OK through the header's own predicate, never a
        // negation of one error arm: a `!= IG_GTF_FENCE_NULL_SPAN` form reports
        // every other refusal as success, which is handoff33 section 2's shape.
        //
        IGLOG("composeFence: refused -- verdict %u (%s), nothing was composed",
              fFenceVerdict, igGtfFenceVerdictName(fFenceVerdict));
        return;
    }

    //
    // Read BACK out of the span, not restated from the inputs.
    //
    // Restating IG_GTP_SEQNO_ADDR_POLICY here would publish the constant that
    // went in and call it the address the engine would store to, which is a
    // fabricated publication in the sense handoff18 section 1d names: it looks
    // like a measurement of the composition and is a measurement of nothing.
    // igGtfComposedSeqnoAddress() rejoins the two halves the composer actually
    // wrote, so a swapped or truncated half shows up as a value differing from
    // the policy constant an operator can compare it against.
    //
    fFenceOrdered      = igGtfCompletionIsOrdered(fFenceSpan,
                                                  IG_GTP_FENCE_SPAN_DWORDS);
    fFenceComposedAddr = igGtfComposedSeqnoAddress(fFenceSpan,
                                                   IG_GTP_FENCE_SPAN_DWORDS);

    //
    // The CPU-side arithmetic, exercised over the pre-run observation.
    //
    // IG_GTP_SEQNO_OBSERVED_POLICY is the reserved value and nothing has run, so
    // this must be FALSE. Asking the question is the point: "the fence has not
    // fired" and "the poll compare is inverted" produce the same silence on a
    // machine, and only the second is a bug in this driver.
    //
    fFenceRetired = igGtfSeqnoRetired(IG_GTP_SEQNO_OBSERVED_POLICY,
                                      IG_GTP_SEQNO_VALUE_POLICY);

    IGLOG("composeFence: composed %u dwords, ordered %d, addr 0x%llx, "
          "retired-over-pre-run %d",
          IG_GTP_FENCE_SPAN_DWORDS, fFenceOrdered ? 1 : 0,
          (unsigned long long) fFenceComposedAddr, fFenceRetired ? 1 : 0);
}

//
//  publishComposedFence() -- the verdict, the ORDER, and the rejoined address.
//
//  Kept separate from the four publishers above for the reason they are separate
//  from each other: one key covering two compositions reports one as the other's
//  failure.
//
//  IGGtFenceAttempted goes out FIRST and on EVERY path, verbatim the reasoning
//  behind GSPEngineReadable, NVIdentityReadable and IGGtBlitAttempted. An ABSENT
//  key is a question rather than an answer -- indistinguishable from an older
//  kext that never published it -- and every field below is a report about a
//  composition that may not have run.
//
void IntelGtProbe::publishComposedFence(void)
{
    setProperty("IGGtFenceAttempted", fFenceAttempted);
    setProperty("IGGtFenceVerdict", (unsigned long long) fFenceVerdict, 32);
    setProperty("IGGtFenceVerdictName", igGtfFenceVerdictName(fFenceVerdict));

    //
    // The address the seqno WOULD be stored to, as policy, published beside the
    // rejoined one below so the two can be compared rather than trusted.
    //
    setProperty("IGGtFenceSeqnoAddrPolicy",
                (unsigned long long) IG_GTP_SEQNO_ADDR_POLICY, 64);
    setProperty("IGGtFenceSeqnoValuePolicy",
                (unsigned long long) IG_GTP_SEQNO_VALUE_POLICY, 32);

    //
    // The sentence this method exists to publish, and it is a CONSTANT rather
    // than a derived value: no code path in this file can make it false, because
    // there is no MMIO store and no doorbell in this file to make it false.
    //
    setProperty("IGGtFenceSignalled", false);

    if (!igGtfFenceSucceeded(fFenceVerdict)) {
        //
        // Guarded, because igGtfWriteCompletion() wrote NOTHING. Publishing the
        // three fields below over an untouched span would put init()'s zeroes
        // into the IORegistry as an ordering claim, an address and a poll answer
        // -- three values nobody composed, reading exactly like three that were
        // composed. That is the class this project has now filed nine times.
        //
        IGLOG("publishComposedFence: verdict %u -- publishing the verdict and no "
              "property of the span", fFenceVerdict);
        return;
    }

    setProperty("IGGtFenceOrdered", fFenceOrdered);
    setProperty("IGGtFenceComposedSeqnoAddr",
                (unsigned long long) fFenceComposedAddr, 64);

    //
    // The poll answer over the pre-run observation, published WITH the value it
    // was asked about. The answer alone is unreadable -- false could mean the
    // arithmetic works or that somebody passed the wrong observation.
    //
    setProperty("IGGtFenceSeqnoObserved",
                (unsigned long long) IG_GTP_SEQNO_OBSERVED_POLICY, 32);
    setProperty("IGGtFenceSeqnoRetired", fFenceRetired);

    //
    // The composed dwords as one blob, same reasoning as IGGtImage above: nine
    // numbered keys would invite reading slot N as a register, and the slots are
    // an MI packet layout IntelGtFence.hpp owns the map for.
    //
    setProperty("IGGtFence", (void *) fFenceSpan,
                (unsigned int) IG_GTF_COMPLETION_BYTES_POLICY);
}
