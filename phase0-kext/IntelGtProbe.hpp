//
//  IntelGtProbe.hpp
//  Gen12 (Tiger Lake) render-engine CONSUMER -- the first translation unit in
//  this tree that reaches for the GT stack from inside IOKit.
//
//  READ THIS FIRST: THIS CLASS SUBMITS NOTHING
//  -------------------------------------------
//  It maps BAR0, reads the render engine's ring registers, composes a logical
//  ring context image into memory THIS DRIVER owns, and publishes what it found
//  and what it composed. It writes ZERO registers on the GPU. There is no write
//  path in this class at all, not even a refusing one -- same deliberate
//  asymmetry as IntelDisplayProbe and IntelFbPresentProbe, and for a stronger
//  reason than either of them has.
//
//  The reason is section 1 of intel-handoff35.md: greps of reference/i915/ for
//  LRC, CTX_DESC, RING_BUFFER_START, RING_BUFFER_CTL, BB_PER_CTX and
//  CTX_TIMESTAMP return ZERO matches (measured 2026-08-26). The page LAYOUT the
//  writer composes is this project's choice end to end. Writing a policy-composed
//  context descriptor into a real execlist port is not a bringup step, it is a
//  guess submitted to silicon -- and the failure mode is not a refusal, it is an
//  engine executing from an address nobody chose.
//
//  So the composition happens, is judged, and is PUBLISHED for inspection. The
//  submission is deliberately absent and will stay absent until something in this
//  tree can cite the layout.
//
//  WHAT THIS BUYS, GIVEN THAT
//  --------------------------
//  Five GT headers and sixteen suites, and until this file nothing called any of
//  them from a translation unit that reaches hardware. That gap is what
//  IntelDisplayNames.hpp cost when it was left standing: four verdict-name
//  switches sat inside a shell for weeks with a missing arm publishing the
//  unknown string, green through all four gates, because a pure function written
//  in a shell is a pure function nothing runs.
//
//  This class is the inverse move. Every number it handles comes from a
//  dependency-free header that tests/ already drives under MSVC; this file adds
//  the MAPPING, the READ, the ORDER and the PUBLICATION, and contains no shift,
//  no mask, and no arithmetic over a register value.
//
//  THE RING REGISTERS ARE READ, NOT ASSUMED
//  ----------------------------------------
//  HEAD, TAIL, START and CTL are read out of the render engine's ring block --
//  the one part of the GT stack with a citation, RENDER_RING_BASE at
//  reference/i915/i915_reg.h:289 and PRB0_BASE spelled (0x2030 - 0x30), which is
//  what fixes the ring block ABOVE the engine base rather than at it.
//
//  What comes back on this machine is almost certainly an idle engine, and the
//  class says so rather than deciding: igpu-results/ captured a machine whose
//  render engine was IDLE, so a zero CTL here is the expected reading and is
//  published as a measurement rather than treated as a fault.
//
//  WHY THE SAMPLE IS TAKEN ONCE
//  ----------------------------
//  Same reason IntelFbPresentProbe caches its four plane words and the same
//  measurement behind it -- intel-handoff28 section 4d: two reads of one register
//  inside a single run of tools/igpu-final-capture.sh DISAGREED. A register read
//  on running silicon is a SAMPLE, not a constant. HEAD in particular is a live
//  pointer the engine moves; reading it twice and publishing both would put two
//  descriptions of one ring in the IORegistry with no way to tell which was acted
//  on.
//
//  WHY THE COMPOSED IMAGE LIVES IN A MEMBER ARRAY
//  ----------------------------------------------
//  IntelGtImage.hpp writes into a span the CALLER owns, and the caller here is
//  this class. The span is a fixed member array rather than an IOBufferMemory
//  allocation because nothing is handed to hardware: an allocation would imply a
//  page the engine could reach, and this class deliberately produces no such
//  page. It is sized from igGtiRingImageDwords() rather than from a literal, so
//  the ten-vs-twelve defect intel-handoff35 section 2a records cannot recur here
//  by a second spelling drifting from the first.
//
//  WHAT THIS DOES NOT ESTABLISH
//  ----------------------------
//  It has never been compiled by Apple's clang, never loaded, never matched, and
//  never submitted. PLAN.md termination condition A still holds -- the laptop is
//  muxless. This is not acceleration. It is the first consumer of the
//  acceleration stack, which is a different and much smaller claim.
//

#ifndef INTEL_GT_PROBE_HPP
#define INTEL_GT_PROBE_HPP

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>

//
//  IntelDisplay.hpp is included for IG_MMIO_INVALID and IG_MMIO_ACCESS_BYTES
//  ONLY, and neither is a display constant despite the filename: the sentinel is
//  what regRead32() answers on a refusal and the access width is what makes the
//  past-mapping test an overhang test rather than an off-by-three one. Both are
//  properties of MMIO on this device, not of the display block.
//
//  Respelling them here would be the defect this project has already paid for --
//  two answers to one question, agreeing with each other until one moves. The
//  cost of the include is that IG_DISPLAY_* comes with it; nothing in this file
//  touches any of it, because the render ring sits BELOW the display policy
//  range and igReadDecision() would refuse every read this class makes.
//
#include "IntelDisplay.hpp"
#include "IntelGpuId.hpp"
#include "IntelGtCmd.hpp"
#include "IntelGtExec.hpp"
#include "IntelGtContext.hpp"
#include "IntelGtImage.hpp"
#include "IntelGtBatch.hpp"
#include "IntelGtRing.hpp"
#include "IntelGtBlt.hpp"
#include "IntelGtFence.hpp"

//
//  The BAR this class maps, spelled here rather than shared.
//
//  IntelFbPresentProbe.hpp gives the reason and it holds a third time: a BAR
//  index shared between classes is exactly the kind of constant that reads as
//  correct while naming the wrong aperture. This class needs BAR0 only -- it
//  maps no graphics memory, because it hands the engine nothing.
//
#define IG_GTP_BAR0_INDEX           0

//
//  Same warning threshold the two display shells carry, and a warning rather than
//  a refusal for the same reason: declining on a short BAR deletes the IORegistry
//  node that makes a short BAR inspectable. regRead32()'s past-mapping refusal is
//  where the warning acquires a consequence.
//
#define IG_GTP_BAR0_EXPECTED        0x00080000u

//
//  The engine this class reads.
//
//  RENDER, because it is the one with a citation. A constant rather than a scan
//  over all four engines for IntelFbPresentProbe's reason one subsystem over:
//  this class's job is to demonstrate one consumer, and three more idle engines
//  produce three more readings that mean the same thing.
//
#define IG_GTP_ENGINE_RENDER        0u

//
//  The ring geometry this class composes an image FOR, and neither is measured.
//
//  Both carry POLICY in their names because that is what they are: no ring is
//  allocated, so there is no ring base to measure and no size the hardware chose.
//  They exist so the composition has admissible inputs, and the published verdict
//  is a statement about the WRITER given these inputs -- not about a ring.
//
//  A value invented to fill a field, filed next to values witnessed on hardware,
//  destroys the only evidence trail this project has. Hence the names.
//
#define IG_GTP_RING_BYTES_POLICY    0x1000u
#define IG_GTP_RING_BASE_POLICY     0x00200000ull
#define IG_GTP_LRCA_POLICY          0x00100000ull

//
//  THE SUBMISSION REHEARSAL: the software context id this class composes with.
//
//  POLICY, and named so for the same reason IG_GTP_LRCA_POLICY is: no context is
//  registered with anything, so there is no id the hardware or a scheduler chose.
//  It exists so igGtxComposeCtxDescriptor() has an admissible input and the
//  published verdict is a statement about the COMPOSER given that input.
//
//  It is deliberately a value igGtxCtxSwIdOk() ACCEPTS. An id chosen to be
//  refused would make IGGtSubmitReadyVerdict a statement about this constant
//  rather than about the readiness join -- intel-handoff36's reasoning for
//  IG_GTP_BATCH_ADDR_POLICY, one layer up.
//
#define IG_GTP_CTX_SWID_POLICY      0x1u

//
//  The first LRI pair's register offset, supplied by this class as the caller.
//
//  THIS CONSTANT WAS CALLED IG_GTP_FIRST_PAIR_UNCITED UNTIL 2026-08-26 and the
//  rename is the whole story. The old name was accurate: the header said this
//  tree cannot cite what that pair holds because reference/i915/ had no LRC file
//  at all, so 0x244 was a value with no evidence behind it sitting one slot away
//  from four ring offsets that were citable.
//
//  reference/i915/ now carries drivers/gpu/drm/i915/gt/intel_engine_regs.h, and
//  RING_CONTEXT_CONTROL(base) is _MMIO((base) + 0x244) at :181. The pair is
//  CTX_CONTEXT_CONTROL, spelled (0x02 + 1) in intel_lrc_reg.h -- the + 1 naming
//  the VALUE dword, so the OFFSET dword is slot 0x02 and the pair occupies 2/3,
//  which is where IntelGtImage.hpp puts it.
//
//  So the value did not change. What changed is that it stopped being a guess,
//  and it is worth being precise about which of those two facts is the finding:
//  a guess that turns out correct is still a guess until something independent
//  agrees with it, and the reason to rename rather than to quietly keep 0x244 is
//  that the old name would go on claiming an absence that no longer exists.
//
#define IG_GTP_FIRST_PAIR_I915_0x244    0x244u
#define IG_GTP_FIRST_PAIR_VALUE         0x00000000u

//
//  THE RING-WRITE REHEARSAL: where the composed dwords would land.
//
//  IG_GTP_SCRATCH_RING_DWORDS is the length of a ring-shaped buffer this class
//  owns outright. It is not the ring the engine reads and it is not mapped to
//  anything the GPU can see -- it is host memory, sized to IG_GTP_RING_BYTES_POLICY
//  so the writer is driven at the same geometry the composed image describes.
//
//  The value is spelled as the byte policy divided by the cited word size rather
//  than as a second literal, for the reason intel-handoff35 section 2a paid for
//  once already: two spellings of one length drift, and the drift is invisible
//  because both are plausible.
//
#define IG_GTP_SCRATCH_RING_DWORDS  (IG_GTP_RING_BYTES_POLICY / IG_GT_CMD_WORD_BYTES_I915_4)

//
//  The batch address handed to the dispatch composer.
//
//  POLICY, and named so. No batch buffer is allocated, so there is no address to
//  measure -- this exists so the dispatch has an admissible input and the
//  published verdict is a statement about the WRITER given that input. It is
//  deliberately 4-byte aligned so igGtBatchAddressOk() accepts it: an address
//  chosen to be REFUSED would make IGGtRingBatchVerdict a statement about this
//  constant rather than about the composition, which is the vacuity direction.
//
#define IG_GTP_BATCH_ADDR_POLICY    0x00300000ull

//
//  The byte length the submit rehearsal claims was written into the ring.
//
//  COMPOSED, not spelled. rehearseRingWrite() emits exactly one MI_BATCH_BUFFER_START
//  packet through igGtrWriteBatchDispatch(), so the length the submit verdict
//  must reason about is that packet's own dword count times the cited command
//  word size -- never a literal 12. This is IG_GTP_SCRATCH_RING_DWORDS' reason
//  at line 203 applied one layer up, and intel-handoff35 section 2a is what two
//  spellings of one length cost: they drift, and the drift is invisible because
//  both readings are plausible. A packet that grows a dword moves this with it.
//
#define IG_GTP_SUBMIT_LENGTH_BYTES_POLICY \
    (IG_GT_MI_BB_START_DWORDS_POLICY_3 * IG_GT_CMD_WORD_BYTES_I915_4)

//
//  THE BLIT COMPOSITION: a rectangle this class describes but nobody draws.
//
//  IntelGtBlt.hpp is the first header in this tree that says what the engine
//  would DO rather than how a packet would reach it, and until this file it had
//  the same standing IntelGtRing.hpp had a session ago: a suite over it, 2D
//  command packets composed correctly, and NO CALLER. intel-handoff26 section 1
//  is that finding and it holds a third time -- a pure function written in a
//  header is a pure function nothing runs until a translation unit that reaches
//  hardware calls it.
//
//  Every number below is POLICY and named so. No framebuffer is allocated, so
//  there is no destination address to measure and no pitch the hardware chose;
//  the rectangle is not on anybody's screen. They exist so the composition has
//  admissible inputs and the published verdict is a statement about the WRITER
//  given those inputs -- never about a surface.
//
//  The destination is 4-byte aligned so igGtbSurfaceVerdict() ACCEPTS it, for
//  IG_GTP_BATCH_ADDR_POLICY's reason one header over: an address chosen to be
//  REFUSED would make IGGtBlitVerdict a statement about this constant rather
//  than about the composition, which is the vacuity direction.
//
//  The rectangle is deliberately non-square and its origin non-zero, so a
//  composer that transposed the axes or dropped the origin produces different
//  dwords rather than the same ones. handoff15 section 1 measured that exact
//  transposition on Intel silicon -- PLANE_SIZE carries its halves INVERTED
//  relative to PIPESRC -- and a 64x64 rectangle at the origin cannot tell the
//  two apart.
//
#define IG_GTP_BLIT_DST_ADDR_POLICY     0x00400000ull
#define IG_GTP_BLIT_DST_PITCH_POLICY    4096u
#define IG_GTP_BLIT_LEFT_POLICY         16u
#define IG_GTP_BLIT_TOP_POLICY          32u
#define IG_GTP_BLIT_RIGHT_POLICY        144u
#define IG_GTP_BLIT_BOTTOM_POLICY       96u
#define IG_GTP_BLIT_COLOR_POLICY        0x00000000u

//
//  The colour depth is COMPOSED from IntelGtBlt.hpp's own constant rather than
//  spelled as a second literal. That header's line 110 records exactly why:
//  the value is a PRE-SHIFTED header dword -- BLT_DEPTH_32 is (3 << 24), not 3
//  -- so a hand-written 3 here would compose a packet claiming 8bpp while
//  reading as thirty-two to anybody skimming this file, and both spellings are
//  plausible. Same reason IG_GTP_BLIT_SPAN_DWORDS is composed below.
//
#define IG_GTP_BLIT_DEPTH_POLICY        IG_GTB_DEPTH_32_I915_0x03000000

//
//  The span the fill is composed into, sized from the header's own packet
//  length rather than from a literal 7.
//
//  Same reason IG_GTP_SCRATCH_RING_DWORDS is composed rather than spelled:
//  intel-handoff35 section 2a paid once already for two spellings of one length
//  drifting apart, and the drift is invisible because both are plausible.
//
#define IG_GTP_BLIT_SPAN_DWORDS     IG_GTB_XY_COLOR_BLT_DWORDS_POLICY_7

//
//  ---- the completion fence -------------------------------------------------
//
//  Every constant above describes work the engine would DO or a way to hand it
//  over. None describes how this driver would learn the work FINISHED, and
//  IntelGtFence.hpp exists because a submission whose completion cannot be
//  observed is indistinguishable, from this side, from one that hung.
//
//  The seqno target lives at a POLICY address in the same invented space as
//  IG_GTP_BLIT_DST_ADDR_POLICY and IG_GTP_BATCH_ADDR_POLICY, and it is chosen to
//  be ACCEPTED -- qword-aligned and non-zero -- for the reason those two are: an
//  address picked to be REFUSED would make IGGtFenceVerdict a statement about
//  this constant rather than about the composition, which is the vacuity
//  direction.
//
#define IG_GTP_SEQNO_ADDR_POLICY        0x00500000ull

//
//  NOT zero, deliberately: zero is IG_GTF_SEQNO_RESERVED_POLICY_0, and a store
//  of zero is byte-identical to a store that never happened.
//
//  Also NOT one. igGtfNextSeqno() returns 1 when it skips the reserved value on
//  wrap, so a composer that ignored its argument entirely and emitted that
//  wrap-skip constant would look correct against a 1.
//
#define IG_GTP_SEQNO_VALUE_POLICY       0x00000101u

//
//  The span the pair is composed into, sized from the header's own dword count
//  rather than from a literal 9 -- intel-handoff35 section 2a's reason, same as
//  IG_GTP_BLIT_SPAN_DWORDS above and IG_GTP_SCRATCH_RING_DWORDS.
//
#define IG_GTP_FENCE_SPAN_DWORDS        IG_GTF_COMPLETION_DWORDS_POLICY

//
//  What the CPU would read out of the fence target BEFORE the engine ever ran.
//  Composed from the reserved constant rather than spelled as 0, because that is
//  what the value MEANS here: the memory a driver polls starts at zero.
//
//  igGtfSeqnoRetired() must answer NOT-RETIRED over it, and that answer is
//  published rather than assumed -- "the fence has not fired" and "the poll
//  arithmetic is inverted" produce the same silence otherwise.
//
#define IG_GTP_SEQNO_OBSERVED_POLICY    IG_GTF_SEQNO_RESERVED_POLICY_0

class IntelGtProbe : public IOService
{
    OSDeclareDefaultStructors(IntelGtProbe)

public:
    virtual bool init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice        *fPCIDevice;
    IOMemoryMap        *fBar0Map;
    volatile UInt8     *fBar0Base;

    //
    //  The mapped length as an IGU64, never narrowed to 32 bits.
    //
    //  IntelFbPresentProbe.hpp states the asymmetry and it is the same here:
    //  0x1_0000_0000 truncates to 0 and refuses everything, which is loud;
    //  0x1_0007_0000 truncates to 0x70000 and ACCEPTS reads it should refuse,
    //  which scribbles.
    //
    IGU64               fBar0Length;

    UInt32              fDeviceId;

    //
    //  THE ONE SAMPLE of the render engine's ring block. Read once, never
    //  re-read, for the reason in the header comment.
    //
    UInt32              fRingHead;
    UInt32              fRingTail;
    UInt32              fRingStart;
    UInt32              fRingCtl;

    //
    //  The flag saying the sample HAPPENED, kept separate from the values.
    //
    //  A zero HEAD is a perfectly ordinary reading on an idle engine, so the
    //  values themselves cannot carry "was this read". init() zeroes all four and
    //  an initialiser zero that reads as a measurement is the exact shape that
    //  put three fabricated register words into the NVIDIA track's IORegistry.
    //
    bool                fSampleTaken;

    //
    //  The composed image and the writer's verdict over it.
    //
    //  Sized from the header's own accessor rather than a literal -- see the
    //  header comment for why a second spelling of that length is the defect
    //  intel-handoff35 section 2a already paid for once.
    //
    UInt32              fImage[IG_GTI_RING_IMAGE_DWORDS_POLICY_12];
    UInt32              fImageDwords;
    UInt32              fWriteVerdict;

    //
    //  The submission rehearsal: the composed context descriptor and the
    //  readiness verdict over it.
    //
    //  fSubmitReadyVerdict is the join igGtxSubmitReady() returns; it is NOT a
    //  bool, because the readiness family names each refusal distinctly and a
    //  bool is satisfied by every refusal at once. fCtxDescriptor is the 64-bit
    //  word that WOULD be written to the execlist queue if this class had a write
    //  path, and it does not: nothing stores it to MMIO.
    //
    //  fSubmitRehearsed is published on both arms for the reason every other
    //  attempted-flag in this file is -- an absent key is a question, not an
    //  answer.
    //
    //
    //  The execlist port's status word, sampled ONCE in rehearseSubmit().
    //
    //  It starts at IG_MMIO_INVALID rather than zero for the reason the four ring
    //  words do: zero is a legal status word meaning "port idle, nothing queued",
    //  so an initialiser zero reaching the IORegistry would be indistinguishable
    //  from a register that genuinely read all-clear.
    //
    UInt32              fExeclistStatus;

    UInt64              fCtxDescriptor;
    UInt32              fSubmitReadyVerdict;
    bool                fSubmitRehearsed;

    //
    //  THE SCRATCH RING and the two verdicts the rehearsal produces.
    //
    //  This buffer is host memory this driver owns. Nothing the GPU can reach
    //  points at it, no MMIO store follows a write into it, and RING_TAIL is
    //  never touched -- IntelGtProbe.hpp's banner and intel-handoff36 section 1
    //  both still hold, and IGGtSubmitted is still published false.
    //
    //  What it buys is the thing a pure header cannot have: the ring writer is
    //  now CALLED from the translation unit that would call it for real, at the
    //  geometry the composed image describes, with its verdict published where
    //  an operator can read it. A header nothing calls is a header nothing runs
    //  -- intel-handoff26 section 1 is that finding, and the whole reason this
    //  wiring exists.
    //
    UInt32              fScratchRing[IG_GTP_SCRATCH_RING_DWORDS];

    //
    //  Both verdicts are initialised to their families' NOT_ATTEMPTED arm rather
    //  than to zero by accident. Zero IS that arm in both families, which is the
    //  point: the spelling says the value is a deliberate "no attempt was made"
    //  rather than an initialiser that happens to read as one.
    //
    UInt32              fRingVerdict;
    UInt32              fRingBatchVerdict;

    //
    //  Where the rehearsal's tail landed, and the flag saying the write ran.
    //
    //  The tail cannot carry "did this happen" on its own: igGtrWriteBatchStartToRing()
    //  reports the tail UNCHANGED on every refusal arm, so a refused write and a
    //  write that wrapped exactly back to its start are byte-identical in this
    //  field. Same shape as fSampleTaken above, same reason.
    //
    UInt32              fRingTailAfter;
    bool                fRingWriteAttempted;

    //
    //  THE COMPOSED BLIT and the writer's verdict over it.
    //
    //  fBlitSpan is host memory this driver owns outright. It is not mapped to
    //  anything the GPU can see, nothing points at it, and the packet it holds
    //  is never appended to any ring -- rehearseRingWrite() writes a batch-start
    //  dword into fScratchRing and this array is not that buffer. Composing a
    //  drawing command and submitting one are different acts and this class does
    //  only the first.
    //
    //  Sized from IntelGtBlt.hpp's own packet length, never a literal 7.
    //
    UInt32              fBlitSpan[IG_GTP_BLIT_SPAN_DWORDS];

    //
    //  The verdict, initialised to its family's NOT_ATTEMPTED arm rather than
    //  to zero by accident. Zero IS that arm, which is the point: the spelling
    //  says a deliberate "no attempt was made" rather than an initialiser that
    //  happens to read as one.
    //
    UInt32              fBlitVerdict;

    //
    //  How many dwords the composer reports it wrote, and the flag saying the
    //  composition ran.
    //
    //  The count cannot carry "did this happen" on its own: igGtbWriteColorBlit()
    //  zeroes outDwordsWritten before inspecting a single argument, so every
    //  refusal arm reports zero and a composition that was never attempted
    //  reports zero identically. Same shape as fSampleTaken and
    //  fRingWriteAttempted above, same reason.
    //
    UInt32              fBlitDwords;
    bool                fBlitAttempted;

    //
    //  THE COMPOSED COMPLETION FENCE and everything the composition reports.
    //
    //  fFenceSpan is host memory this driver owns outright, exactly like
    //  fBlitSpan and fScratchRing above. The flush/store pair it holds is never
    //  appended to any ring, no MMIO store follows it, and the seqno address it
    //  names is a POLICY constant in an invented space -- nothing the GPU can
    //  reach points at either the span or the target. Composing a completion
    //  signal and FIRING one are different acts and this class does only the
    //  first, which is IntelGtFence.hpp's own banner restated at its one caller.
    //
    //  Sized from IG_GTP_FENCE_SPAN_DWORDS, which is the header's own dword
    //  count rather than a literal 9.
    //
    UInt32              fFenceSpan[IG_GTP_FENCE_SPAN_DWORDS];

    //
    //  The verdict, initialised to its family's NOT_ATTEMPTED arm rather than to
    //  zero by accident. Zero IS that arm, same as fBlitVerdict and fRingVerdict:
    //  the spelling says a deliberate "no attempt was made".
    //
    UInt32              fFenceVerdict;

    //
    //  Whether the composed span reads back with the flush BEFORE the store.
    //
    //  This is the one property the verdict cannot carry. Both orderings compose
    //  the identical SET of dwords, the identical count, the identical byte
    //  length, and igGtfWriteCompletion() returns IG_GTF_FENCE_OK over either --
    //  only the INDEX each header lands at separates them, and a store observed
    //  before its flush reports "done" over caches still holding unwritten
    //  pixels. IntelGtFence.hpp's ordering section is that argument; this member
    //  is the reason igGtfCompletionIsOrdered() exists as a caller-facing
    //  predicate rather than only as a test's assertion.
    //
    bool                fFenceOrdered;

    //
    //  The seqno address read BACK out of the composed span, not the constant
    //  that went in.
    //
    //  igGtfComposedSeqnoAddress() rejoins the two halves, which is a thing a
    //  dword dump cannot show: the high half sits three dwords from the low one
    //  and reads as an unrelated word. Publishing the rejoined value lets an
    //  operator compare it against IG_GTP_SEQNO_ADDR_POLICY and see a swapped or
    //  truncated half, rather than reading two plausible dwords and inferring
    //  nothing.
    //
    UInt64              fFenceComposedAddr;

    //
    //  What igGtfSeqnoRetired() answers over the pre-run observation.
    //
    //  It must be FALSE: IG_GTP_SEQNO_OBSERVED_POLICY is the reserved value and
    //  nothing has run. Published rather than assumed, because a fence that has
    //  not fired and poll arithmetic that is inverted produce the same silence,
    //  and only one of them is a bug in this driver.
    //
    bool                fFenceRetired;

    //
    //  The flag saying the composition ran at all.
    //
    //  None of the four fields above can carry it. igGtfWriteCompletion() writes
    //  NOTHING on any refusal, so a refused composition leaves fFenceSpan as
    //  init() left it -- and init()'s zeroes make fFenceOrdered false,
    //  fFenceComposedAddr zero and fFenceRetired false, which is byte-identical
    //  to a composition that ran and was refused. Same shape as fSampleTaken,
    //  fRingWriteAttempted and fBlitAttempted above, same reason.
    //
    bool                fFenceAttempted;

    //
    //  THE RECOVERY JUDGEMENT and everything the two judgements report.
    //
    //  This layer is the first one in this class whose subject is a register
    //  WRITE rather than a composition in host memory, and that difference is the
    //  reason every member below is a VERDICT or a composed OFFSET and not one
    //  byte of state the hardware would see. IntelGtReset.hpp composes and
    //  JUDGES: it says which offset a reset request would be written to and what
    //  value it would carry, and it reads a supplied register word to decide
    //  whether the engine reported itself ready. Nothing in this file writes
    //  either offset, because nothing in this file has ever submitted anything to
    //  an engine that could need recovering.
    //
    //  The wait verdict, initialised to its family's NOT_ATTEMPTED arm rather
    //  than to zero by accident -- same spelling as the four verdicts above, and
    //  the family it belongs to is the one whose whole existence is that a bool
    //  cannot carry this answer. igGtfSeqnoPassed() returning false means "not
    //  yet" and means "never", and those two call for opposite next actions:
    //  keep polling, or stop the engine and recover it.
    //
    UInt32              fWaitVerdict;

    //
    //  The reset handshake verdict, same spelling and same reason. Separate from
    //  the wait verdict because the two answer different questions over different
    //  budgets -- one waits on submitted work whose length this driver does not
    //  know, the other on a hardware handshake that either lands promptly or is
    //  not going to land at all.
    //
    UInt32              fResetVerdict;

    //
    //  Whether the wait verdict is one that would send this driver into recovery.
    //
    //  Published rather than left to be derived by whoever reads the registry,
    //  because the derivation is the one place in this layer where a negation is
    //  wrong: !igGtrWaitSucceeded() is TRUE for PENDING, and resetting an engine
    //  with work legitimately still in flight is the single action this layer
    //  exists to prevent. igGtrWaitDemandsRecovery() is an EQUALITY SET over the
    //  two arms that mean it, and publishing its answer puts that distinction in
    //  the registry rather than in a reader's head.
    //
    bool                fWaitDemandsRecovery;

    //
    //  The two absolute MMIO offsets a recovery would have used, composed for the
    //  engine this class samples.
    //
    //  Composed rather than spelled, and published so they can be COMPARED: an
    //  unrecognised engine base answers IG_GT_BASE_INVALID rather than a
    //  plausible offset into whatever block happens to live there, so a reader
    //  can tell a refusal from an address. That refuse-rather-than-compose
    //  direction is the same one every accessor in this tree takes, and the
    //  reason it matters here is the NVIDIA side's measured precedent -- the
    //  RISC-V block sits one page above the Falcon block, so a wrong base reads
    //  and writes real hardware silently rather than faulting.
    //
    UInt32              fResetCtlOffset;
    UInt32              fMiModeOffset;

    //
    //  The two values those writes would carry, read back out of the header's own
    //  composers rather than restated from the cited bit constants.
    //
    //  Restating IG_GTR_RESET_REQUEST_I915_BIT0 here would publish the constant
    //  that went in and call it the value a reset request would carry -- a
    //  fabricated publication in handoff18 section 1d's sense, looking like a
    //  measurement of the composition and measuring nothing.
    //
    UInt32              fResetRequestValue;
    UInt32              fStopRingValue;

    //
    //  The RESET_CTL word this judgement was made over.
    //
    //  Initialised to the REFUSAL sentinel rather than to zero, same reasoning as
    //  the four ring words: zero is a LEGAL reading of RESET_CTL meaning the
    //  engine reports neither ready-to-reset nor a catastrophic error, which is
    //  exactly what an idle healthy engine reports. An initialiser zero there
    //  would be indistinguishable from a register that genuinely read all-clear.
    //
    UInt32              fResetCtlWord;

    //
    //  The flag saying the judgement ran at all.
    //
    //  None of the fields above can carry it. Every refusal arm of both verdict
    //  functions returns a NAMED verdict rather than touching the offsets or the
    //  values, so a judgement refused for BAD_ENGINE leaves fResetCtlOffset and
    //  fMiModeOffset as init() left them -- and init()'s zeroes are
    //  indistinguishable from a judgement that was never asked for. Same shape as
    //  fSampleTaken, fRingWriteAttempted, fBlitAttempted and fFenceAttempted
    //  above, same reason.
    //
    bool                fResetAttempted;

    bool     mapBar0(void);
    void     unmapBar0(void);
    UInt32   regRead32(UInt32 offset);

    bool     sampleRing(void);
    void     composeImage(void);
    void     rehearseRingWrite(void);
    void     composeBlit(void);
    void     rehearseSubmit(void);
    void     composeFence(void);
    void     judgeRecovery(void);

    void     publishRingSample(void);
    void     publishComposedImage(void);
    void     publishRingWrite(void);
    void     publishComposedBlit(void);
    void     publishSubmitRehearsal(void);
    void     publishComposedFence(void);
    void     publishRecoveryJudgement(void);
};

#endif // INTEL_GT_PROBE_HPP
