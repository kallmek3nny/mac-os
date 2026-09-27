//
//  NV3050Probe.hpp
//  Phase 0/2/3/3.5 skeleton driver for GA107 (RTX 3050 Mobile) on macOS.
//
//  Scope: PCI match, BAR0 map, chip identification, and firmware-health reads.
//  Nothing is written to the GPU. Everything past this is Phase 4+ (GSP
//  firmware boot) and is not here.
//
//  All register constants and bit-field decoding live in NVDecode.hpp, which is
//  deliberately dependency-free so the same code compiles and RUNS under MSVC
//  in tests/. This file holds only the IOKit shell around it. Do not
//  re-declare a register here -- if it is missing, add it to NVDecode.hpp so
//  tools/check-registers.py and tests/test_decode.cpp both cover it.
//

#ifndef NV3050Probe_hpp
#define NV3050Probe_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>

#include "NVDecode.hpp"
#include "NVFalcon.hpp"
#include "NVGspBoot.hpp"
#include "NVRiscvUcode.hpp"
#include "NVGspStage.hpp"

//
// Phase 5 -- IORegistry key the GSP firmware blob arrives under.
//
// The blob is injected as a property (by the personality dictionary, or by a
// userspace tool that sets it on the matched service) rather than read from a
// file inside start(). A kext reading the filesystem at match time has no
// ordering guarantee that the volume is mounted, and the GA107 GSP binaries are
// NVIDIA-signed and must not be redistributed inside this bundle.
//
#define kNVGspFirmwareProperty  "NVGspFirmware"

//
// NV_GSP_FW_ADDR_BITS / NV_GSP_FW_PHYS_MASK / NV_GSP_FW_PHYS_LIMIT used to be
// defined here. They now live in NVGspStage.hpp, next to the functions that
// enforce them, so the mask-vs-limit distinction cannot drift away from the
// code that depends on it -- and so tests/ can compile the same constants that
// the kext uses.
//

class NV3050Probe : public IOService
{
    OSDeclareDefaultStructors(NV3050Probe);

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
    // Phase 5 -- staging for the GSP bootloader image.
    //
    // The buffer must outlive the state machine run: DMATRFBASE hands the GPU a
    // physical address and the engine reads it asynchronously, so freeing at the
    // end of attemptGspBoot() would race the transfer. Both objects are released
    // in releaseGspFirmware(), which stop() and free() both call.
    //
    // fGspFwDma exists rather than a bare getPhysicalSegment() call because the
    // transfer is constrained to 41 bits -- DMATRFBASE carries physical bits
    // 39:8 and DMATRFBASE1 continues the SHIFTED value with physical bits 48:40
    // (NVGspBoot.hpp:119-143), so 41 is this driver's policy ceiling rather
    // than the pair's limit. IODMACommand::withSpecification() is where that
    // width is expressed; a buffer above 2 TiB would otherwise be truncated into
    // a valid-looking address and read silently wrong memory.
    //
    IOBufferMemoryDescriptor *fGspFwBuffer;
    IODMACommand             *fGspFwDma;

    //
    // True only between a prepare() that returned kIOReturnSuccess and its
    // matching complete().
    //
    // releaseGspFirmware() runs from every one of loadGspFirmware()'s failure
    // paths, and one of those paths is "prepare() ITSELF failed" -- so an
    // unconditional complete() there is a complete() with no prepare behind it.
    // IOMemoryDescriptor carries a wire count, so that decrements a counter
    // below its own floor: the damage lands in kernel VM bookkeeping rather than
    // in anything this driver would print, which is why nothing here would have
    // reported it.
    //
    bool                      fGspFwPrepared;

    //
    // True only between a setMemoryDescriptor() that returned kIOReturnSuccess
    // and its matching clearMemoryDescriptor().
    //
    // The same shape as fGspFwPrepared, one function name away, and it was
    // missed by the audit that found that one -- because the teardown DOES have
    // a guard here, just the wrong one. `if (fGspFwDma)` asks whether the
    // COMMAND OBJECT exists, which is true from the moment
    // withSpecification() returns. It says nothing about whether the object
    // ever took ownership of a descriptor, and loadGspFirmware() calls the
    // teardown from the path where setMemoryDescriptor() ITSELF failed -- an
    // object that exists and holds nothing.
    //
    // Whether Apple's clearMemoryDescriptor() tolerates that is not knowable
    // from this tree: tests/kextshim/IOKit/IODMACommand.h only DECLARES it, and
    // no header here carries the implementation. So the flag is not a claim
    // that the unguarded call was harmful -- it is a refusal to depend on an
    // answer this project cannot check. One bool makes the code correct
    // without needing to know.
    //
    bool                      fGspFwDmaHasDescriptor;

    //
    // Extract the bootloader region from `blob`, stage it in a 41-bit-addressable
    // contiguous buffer, and fill *outImage with the physical base and length the
    // state machine needs. Returns false with *outImage left invalid on any
    // malformed blob, allocation failure, or address-width violation.
    //
    bool   loadGspFirmware(const unsigned char *blob, UInt32 blobLen,
                           NVGspFirmwareImage *outImage);

    //
    // Publish nvGspStagePlan()'s refusal, and the EIGHT-WAY ucode verdict
    // underneath it, into the IORegistry.
    //
    // Until 2026-08-19 both died in NVLOG. loadGspFirmware() returned a bare
    // bool and logged one word, so an operator holding a refused blob could
    // read the registry and find NOTHING about why -- the same class as
    // 3050-handoff31 (a diagnosis reaching the log and not the consumer) and
    // 3050-handoff33, one subsystem over.
    //
    // Called with the plan nvGspStagePlan() filled, on BOTH arms and BEFORE
    // the refusal is acted on, so the keys are present whatever happens next.
    // `plan` may be null only defensively; the one caller always has one.
    //
    void   publishGspStageVerdict(NVGspStageResult planned,
                                  const NVGspStagePlan *plan);
    void   releaseGspFirmware(void);

    //
    // Retained OSData holding the GSP firmware blob, or nullptr when none is
    // present. The blob is a property rather than a file read: a kext reading
    // from the filesystem at start() time is a layering and timing hazard, and
    // this project has no signed blob to ship. Caller releases.
    //
    OSData *copyGspFirmwareBlob(void);

    bool   mapBar0(void);
    void   unmapBar0(void);
    UInt32 regRead32(UInt32 offset);

    //
    // The ONLY function in this driver that writes to the GPU. It refuses
    // unless fAllowGspWrites is set, so the call is safe to reach from a
    // stock build -- it just logs and returns.
    //
    void   regWrite32(UInt32 offset, UInt32 value);

    // Phase 3 -- identity.
    bool   readIdentity(UInt32 *outBoot0, UInt32 *outBoot42);
    void   decodeBoot0(UInt32 value);
    void   decodeBoot42(UInt32 value);
    void   publishIdentity(UInt32 boot0, UInt32 boot42);

    //
    // The dead-read path publishes through here instead. It takes ONLY BOOT_0,
    // because on that path BOOT_42 was never read -- readIdentity() returns
    // false without touching outBoot42. Publishing a fabricated NV_PMC_BOOT_42
    // would be byte-identical in the IORegistry to a register that genuinely
    // returned that value, which is the class handoff18 s1d fixed one level up
    // in inspectGspEngine(). An absent key is a question the operator has to
    // ask; a fabricated key is an answer they will believe.
    //
    void   publishIdentityUnreadable(UInt32 boot0);

    // Phase 3.5 -- firmware health, still read-only.
    //
    // Returns the VERDICT rather than a bool. The bool collapsed two
    // categorically different refusals into one false -- "this offset is not
    // decoding", which is a defect in this driver's BAR0 map, and "GFW decoded
    // but never reached 0xFF", which is a fact about the card. Worse, the
    // unreadable arm wrote *outProgress = 0 before returning, and a literal 0 is
    // byte-identical in the IORegistry to a card whose GFW genuinely sits at
    // step 0. outProgress is now written ONLY on the two arms that read a real
    // word, and callers must consult nvGfwBootProgressWasRead() before
    // publishing it. See NVDecode.hpp's NVGfwBootVerdict comment.
    //
    NVGfwBootVerdict pollGfwBoot(UInt32 *outProgress, UInt32 *outElapsedMs);
    //
    // Returns the VERDICT rather than a bool, and writes the megabyte count
    // only on NV_FB_SIZE_OK. The bool could not carry the distinction: the
    // value mask is 0xFFFFFFFF, so a refused read masks to 4294967295, which
    // is greater than zero and therefore looked like a size to a publisher
    // testing magnitude. See NVDecode.hpp's NVFbSizeVerdict comment.
    //
    NVFbSizeVerdict readUsableFbSize(UInt32 *outMegabytes);
    void   publishFirmwareState(NVGfwBootVerdict gfwVerdict, UInt32 gfwProgress,
                                NVFbSizeVerdict fbVerdict, UInt32 fbSizeMB);

    //
    // Phase 3.75 -- GSP engine inspection. Reads the Falcon and RISC-V state
    // that Phase 4 will later have to drive, and reports whether the engine is
    // in a condition where a firmware boot could even be attempted. Still zero
    // writes; this is the go/no-go gate before anything is ever written.
    //
    UInt32 gspFalconRead(UInt32 falconRelative);
    UInt32 gspRiscvRead(UInt32 riscvRelative);
    bool   inspectGspEngine(void);
    //
    // Returns an NVWPR2_* verdict, not a bool. A bool collapsed "WPR2 is up"
    // and "the registers did not decode" into one false, and only the second
    // is a defect somebody can go and fix. The out-parameters carry whatever
    // was read, INCLUDING the all-ones refusal value -- the caller must
    // consult the verdict before publishing either word.
    //
    UInt32 checkWpr2(UInt32 *outAddrLo, UInt32 *outAddrHi);
    void   publishGspState(UInt32 hwcfg2, UInt32 hwcfg, UInt32 falconCpuctl,
                           UInt32 riscvCpuctl, UInt32 bcrCtrl,
                           UInt32 privErr, UInt32 hubErr, bool ready);

    //
    // The dead-read path publishes through here instead. It takes ONLY the one
    // register that was actually read, because on that path nothing else was.
    // Handing publishGspState() zeros for the other six would put fabricated
    // values in the IORegistry that no reader can tell from measured ones.
    //
    void   publishGspUnreadable(UInt32 hwcfg2);

    //
    // The RISC-V error latches, as a VERDICT plus the five register words.
    //
    // Until 2026-08-19 nvRiscvHasError() collapsed four structurally different
    // situations onto one bool -- priv fault, hub fault, both, and NEITHER
    // REGISTER DECODING -- and the last is a bug in this driver rather than a
    // fact about the card. Three of the five registers in the block
    // (PRIV_ERR_INFO, PRIV_ERR_ADDR, PRIV_ERR_ADDR_HI) were read by nothing at
    // all, so a reported fault carried no address.
    //
    // The verdict and the readable flag are published FIRST and on BOTH arms,
    // and the three localization words only behind the readable arm: on the
    // UNREADABLE arm nothing in this block was measured, and a refusal value
    // published as a word is byte-identical to a measured one.
    //
    void   publishGspErrorLatches(UInt32 errVerdict,
                                 UInt32 privErr, UInt32 hubErr,
                                 UInt32 privErrInfo, UInt32 privErrAddr,
                                 UInt32 privErrAddrHi);

    //
    // Phase 4 -- GSP bringup. The state machine itself lives in NVGspBoot.hpp
    // and is driven on a host compiler in tests/; everything below is only the
    // glue that hands that machine a real BAR0.
    //
    // TWO independent gates stand between this code and a write landing on
    // the GPU, because this project has never written to the card and the
    // first write must be a deliberate human decision, not a default:
    //
    //   Gate 1  fAllowGspWrites is false unless the kext was built with
    //           -DPHASE4_ALLOW_WRITES. A stock build physically cannot set it.
    //   Gate 2  That flag is carried into NVGspBootIo::allowWrites, so the
    //           machine refuses its own first write and stops with
    //           ERR_WRITES_DISABLED instead of continuing blind.
    //
    // With both gates closed the run is a full dry rehearsal: every read, every
    // precondition check, and an exact report of which write it WOULD have
    // issued first.
    //
    bool   fAllowGspWrites;

    //
    // Trampolines matching NVGspRead32Fn / NVGspWrite32Fn. Typed with NVU32
    // rather than UInt32 so the signatures match those typedefs exactly and no
    // implicit conversion is relied on.
    //
    static NVU32 gspBootRead32(void *ctx, NVU32 bar0Offset);
    static void  gspBootWrite32(void *ctx, NVU32 bar0Offset, NVU32 value);

    bool   attemptGspBoot(void);
    void   publishGspBootResult(const NVGspBoot *machine);
};

#endif /* NV3050Probe_hpp */
