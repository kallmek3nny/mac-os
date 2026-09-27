//
//  NV3050Probe.cpp
//
//  Phase 2 (enumeration), Phase 3 (identity), Phase 3.5 (firmware health).
//  Reads registers and writes none. See ../PLAN.md.
//
//  Every decode here delegates to NVDecode.hpp rather than open-coding a shift
//  and mask. That header is exercised by tests/test_decode.cpp under MSVC, so
//  the bit twiddling is verified on a machine with no GPU and no macOS -- which
//  is where this project's two real bugs were found.
//

#include "NV3050Probe.hpp"

#define super IOService
OSDefineMetaClassAndStructors(NV3050Probe, IOService);

#define NVLOG(fmt, ...)  IOLog("NV3050Probe: " fmt "\n", ## __VA_ARGS__)

bool NV3050Probe::init(OSDictionary *dict)
{
    if (!super::init(dict))
        return false;

    fPCIDevice = nullptr;
    fBar0Map   = nullptr;
    fBar0Base  = nullptr;

    //
    // Phase 5 staging. Nulled here rather than in start() because free() may run
    // on an instance whose start() never got that far, and releaseGspFirmware()
    // has to be safe to call on a half-constructed object.
    //
    fGspFwBuffer           = nullptr;
    fGspFwDma              = nullptr;
    fGspFwPrepared         = false;
    fGspFwDmaHasDescriptor = false;

    //
    // Gate 1 of two. A stock build cannot turn this on: the only way to reach
    // `true` is to compile with -DPHASE4_ALLOW_WRITES, which is a deliberate
    // act by a human who has read PHASE4-GSP-BRINGUP.md. Everything Phase 4
    // does with the gate closed is a dry rehearsal.
    //
#ifdef PHASE4_ALLOW_WRITES
    fAllowGspWrites = true;
#else
    fAllowGspWrites = false;
#endif

    NVLOG("init (GSP writes %s)", fAllowGspWrites ? "ENABLED" : "disabled");
    return true;
}

void NV3050Probe::free(void)
{
    NVLOG("free");

    //
    // Belt and braces: stop() already released the staging buffer on the normal
    // path, but a start() that failed after loadGspFirmware() never reaches
    // stop(). releaseGspFirmware() is idempotent, so calling it twice is safe
    // and leaking a wired contiguous allocation is not.
    //
    releaseGspFirmware();

    super::free();
}

//
// probe() runs before start() and lets us reject devices we matched on but do
// not actually want. The Info.plist match is vendor-wide (0x10DE), so a machine
// with two NVIDIA functions hands us both. Filter here rather than widening the
// plist.
//
IOService *NV3050Probe::probe(IOService *provider, SInt32 *score)
{
    IOService *result = super::probe(provider, score);
    if (!result)
        return nullptr;

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        NVLOG("probe: provider is not an IOPCIDevice, declining");
        return nullptr;
    }

    UInt16 vendor = pci->configRead16(kIOPCIConfigVendorID);
    UInt16 device = pci->configRead16(kIOPCIConfigDeviceID);
    UInt32 subsys = pci->configRead32(kIOPCIConfigSubSystemVendorID);

    //
    // Config space 0x08 = [rev:8][prog-if:8][subclass:8][baseclass:8].
    // Base class therefore lives at byte offset 0x0B.
    //
    UInt32 classReg  = pci->configRead32(kIOPCIConfigRevisionID);
    UInt8  baseClass = (UInt8)((classReg >> 24) & 0xFF);
    UInt8  subClass  = (UInt8)((classReg >> 16) & 0xFF);

    NVLOG("probe: %04x:%04x subsys 0x%08x class %02x:%02x",
          vendor, device, (unsigned)subsys, baseClass, subClass);

    if (vendor != 0x10DE) {
        NVLOG("probe: not NVIDIA, declining");
        return nullptr;
    }

    //
    // Base class 0x03 = display controller. Ampere boards also expose an HDA
    // audio function (class 0x04) and, on some parts, USB-C controllers
    // (class 0x0C) on the same device. We must claim only the GPU.
    //
    if (baseClass != 0x03) {
        NVLOG("probe: function is not a display controller, declining");
        return nullptr;
    }

    return result;
}

bool NV3050Probe::start(IOService *provider)
{
    UInt32 boot0  = 0;
    UInt32 boot42 = 0;

    if (!super::start(provider)) {
        NVLOG("start: super::start failed");
        return false;
    }

#ifdef PANIC_TEST
    //
    // Phase 0 KDP milestone. Build with `make PANIC_TEST=1`, load, and catch
    // the panic from the debugger machine. A symbolicated backtrace naming
    // NV3050Probe::start means two-machine debugging works and Phase 0 is done.
    // Never ship a build with this defined.
    //
    NVLOG("start: PANIC_TEST build -- deliberately faulting");
    volatile UInt32 *deliberateNull = nullptr;
    *deliberateNull = 0xDEADBEEF;
#endif

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        NVLOG("start: no IOPCIDevice provider");
        goto fail;
    }
    fPCIDevice->retain();

    //
    // Wake the function and enable the memory space decoder. Without this the
    // BAR reads back as all-ones no matter how correct the mapping code is --
    // the single most common reason a first-light register read returns
    // 0xFFFFFFFF.
    //
    fPCIDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    IOSleep(10);

    fPCIDevice->setMemoryEnable(true);
    fPCIDevice->setBusMasterEnable(true);

    NVLOG("start: PCI command register now 0x%04x",
          fPCIDevice->configRead16(kIOPCIConfigCommand));

    if (!mapBar0()) {
        NVLOG("start: BAR0 mapping failed");
        goto fail;
    }

    //
    // Identification is non-fatal. A dead read is itself the useful result at
    // this phase -- we want the node in the IORegistry either way so the
    // failure is inspectable rather than invisible.
    //
    if (readIdentity(&boot0, &boot42))
        publishIdentity(boot0, boot42);
    else
        publishIdentityUnreadable(boot0);

    //
    // Phase 3.5 -- firmware health. Only meaningful if the aperture responded
    // at all, so gate it on the identity read.
    //
    if (!nvLooksLikeDeadRead(boot0)) {
        UInt32 gfwProgress = 0;
        UInt32 gfwElapsed  = 0;
        UInt32 fbSizeMB    = 0;

        //
        // NV_FB_SIZE_NOT_ATTEMPTED is the enum's zero value on purpose, so the
        // !gfwOk path below publishes "nobody looked" rather than inheriting a
        // refusal code it never measured.
        //
        NVFbSizeVerdict fbVerdict = NV_FB_SIZE_NOT_ATTEMPTED;

        NVGfwBootVerdict gfwVerdict = pollGfwBoot(&gfwProgress, &gfwElapsed);

        //
        // An EQUALITY against the accepting verdict rather than a negation of an
        // error arm, for the same reason nvFbSizeVerdict()'s caller uses one:
        // != NV_GFW_BOOT_UNREADABLE is TRUE for NOT_ATTEMPTED and for TIMEOUT,
        // so it would read the framebuffer size off a card that never finished
        // booting.
        //
        if (nvGfwBootVerdictIsOk(gfwVerdict)) {
            NVLOG("GFW_BOOT: progress 0x%02x COMPLETED after %u ms",
                  (unsigned)gfwProgress, (unsigned)gfwElapsed);

            //
            // An EQUALITY against the accepting verdict rather than a truth
            // test on the return, so a verdict added later cannot fall into
            // the accepting arm by being non-zero.
            //
            fbVerdict = readUsableFbSize(&fbSizeMB);

            if (fbVerdict == NV_FB_SIZE_OK) {
                NVLOG("usable framebuffer: %u MB (%u.%u GB)",
                      (unsigned)fbSizeMB,
                      (unsigned)(fbSizeMB / 1024),
                      (unsigned)((fbSizeMB % 1024) * 10 / 1024));
                NVLOG("Phase 3.5 milestone reached -- firmware is up and "
                      "memory is initialised, with zero writes issued.");
            }
        } else {
            NVLOG("GFW_BOOT: did not reach COMPLETED (0x%02x) within %u ms -- "
                  "last progress 0x%02x. Firmware may have stalled, or macOS "
                  "reset the device after the VBIOS ran.",
                  (unsigned)NV_GFW_BOOT_PROGRESS_COMPLETED,
                  (unsigned)NV_GFW_BOOT_POLL_TIMEOUT_MS,
                  (unsigned)gfwProgress);
        }

        publishFirmwareState(gfwVerdict, gfwProgress, fbVerdict, fbSizeMB);

        //
        // Phase 3.75 -- GSP engine inspection. Gated on GFW boot completing,
        // because before that the engine state is not yet meaningful: the
        // VBIOS firmware is still bringing the card up and the GSP registers
        // report transitional values.
        //
        if (nvGfwBootVerdictIsOk(gfwVerdict)) {
            bool gspReady = inspectGspEngine();

            //
            // Phase 4 -- GSP bringup. Gated on the Phase 3.75 go/no-go for the
            // same reason 3.75 is gated on GFW boot: driving a bringup sequence
            // against an engine that already reported NOT bootable would
            // produce a failure whose cause is the precondition, not the
            // sequence, which is the least useful kind of log line.
            //
            if (gspReady)
                attemptGspBoot();
            else
                NVLOG("skipping Phase 4 GSP bringup -- Phase 3.75 reported the "
                      "engine NOT bootable, so a bringup attempt would only "
                      "re-report that precondition");
        } else {
            NVLOG("skipping GSP inspection -- GFW boot did not complete, so "
                  "engine state would not be meaningful");
        }
    }

    registerService();

    NVLOG("start: complete");
    return true;

fail:
    unmapBar0();
    OSSafeReleaseNULL(fPCIDevice);
    super::stop(provider);
    return false;
}

void NV3050Probe::stop(IOService *provider)
{
    NVLOG("stop");

    //
    // Release the firmware staging BEFORE unmapping BAR0. The GPU may still be
    // mid-DMA out of that buffer; the only thing this driver can do about that
    // ordering is complete()/clear() the descriptor while the register aperture
    // is still mapped, so a future halt-the-engine step has somewhere to write.
    //
    releaseGspFirmware();

    unmapBar0();

    if (fPCIDevice) {
        fPCIDevice->setBusMasterEnable(false);
        OSSafeReleaseNULL(fPCIDevice);
    }

    super::stop(provider);
}

#pragma mark - BAR0

bool NV3050Probe::mapBar0(void)
{
    IODeviceMemory *bar = fPCIDevice->getDeviceMemoryWithIndex(NV_BAR0_INDEX);
    if (!bar) {
        NVLOG("mapBar0: no device memory at index %d", NV_BAR0_INDEX);
        return false;
    }

    //
    // kIOMapInhibitCache is mandatory for MMIO. A cached mapping means writes
    // sit in the cache line and reads return stale data -- the register traffic
    // never reaches the device, and the failure looks like the GPU ignoring you
    // rather than a mapping bug.
    //
    fBar0Map = bar->map(kIOMapInhibitCache);
    if (!fBar0Map) {
        NVLOG("mapBar0: map() failed");
        return false;
    }

    fBar0Base = (volatile UInt8 *)fBar0Map->getVirtualAddress();
    if (!fBar0Base) {
        NVLOG("mapBar0: null virtual address");
        OSSafeReleaseNULL(fBar0Map);
        return false;
    }

    IOByteCount length = fBar0Map->getLength();

    NVLOG("mapBar0: phys 0x%llx virt %p len 0x%llx",
          (unsigned long long)bar->getPhysicalAddress(),
          (void *)fBar0Base,
          (unsigned long long)length);

    if (length < NV_BAR0_EXPECTED_LENGTH) {
        NVLOG("mapBar0: WARNING length 0x%llx is smaller than the expected "
              "0x%x register aperture -- wrong BAR index? The GC6 island "
              "registers Phase 3.5 needs live at ~0x118xxx.",
              (unsigned long long)length, (unsigned)NV_BAR0_EXPECTED_LENGTH);
    }

    return true;
}

void NV3050Probe::unmapBar0(void)
{
    fBar0Base = nullptr;
    OSSafeReleaseNULL(fBar0Map);
}

UInt32 NV3050Probe::regRead32(UInt32 offset)
{
    if (!fBar0Base || !fBar0Map) {
        NVLOG("regRead32: BAR0 not mapped");
        return 0xFFFFFFFF;
    }

    if ((IOByteCount)offset + sizeof(UInt32) > fBar0Map->getLength()) {
        NVLOG("regRead32: offset 0x%x out of range", offset);
        return 0xFFFFFFFF;
    }

    return OSReadLittleInt32(fBar0Base, offset);
}

//
// The ONLY write path in this driver. Three refusals stand in front of the
// store, and each one logs rather than failing silently, because a dropped
// write that nobody notices is indistinguishable from a GPU that ignored it.
//
void NV3050Probe::regWrite32(UInt32 offset, UInt32 value)
{
    if (!fBar0Base || !fBar0Map) {
        NVLOG("regWrite32: BAR0 not mapped -- write 0x%08x -> 0x%x DROPPED",
              (unsigned)value, (unsigned)offset);
        return;
    }

    if ((IOByteCount)offset + sizeof(UInt32) > fBar0Map->getLength()) {
        NVLOG("regWrite32: offset 0x%x out of range -- write 0x%08x DROPPED",
              (unsigned)offset, (unsigned)value);
        return;
    }

    if (!fAllowGspWrites) {
        NVLOG("regWrite32: REFUSED write 0x%08x -> 0x%x. This build has no "
              "-DPHASE4_ALLOW_WRITES, so the driver is read-only by "
              "construction.", (unsigned)value, (unsigned)offset);
        return;
    }

    OSWriteLittleInt32(fBar0Base, offset, value);
}

#pragma mark - Phase 3: identity

bool NV3050Probe::readIdentity(UInt32 *outBoot0, UInt32 *outBoot42)
{
    UInt32 boot0 = regRead32(NV_PMC_BOOT_0);

    if (outBoot0)
        *outBoot0 = boot0;

    decodeBoot0(boot0);

    if (nvLooksLikeDeadRead(boot0)) {
        //
        // BOOT_42 lives at 0xA00 and will not respond either if BOOT_0 did not.
        // Skip it so the log shows one clear failure instead of two.
        //
        // outBoot42 is deliberately left ALONE rather than filled with boot0.
        // Handing back a value this function never read is the defect handoff18
        // §1d fixed one level up in inspectGspEngine(): a fabricated number in
        // the IORegistry is byte-identical to a register that genuinely
        // returned it, so an operator cannot tell a dead aperture from a card
        // reporting 0xFFFFFFFF. The false return is the only honest signal,
        // and publishIdentityUnreadable() is what consumes it.
        //
        NVLOG("readIdentity: skipping NV_PMC_BOOT_42, the aperture is not live");
        return false;
    }

    UInt32 boot42 = regRead32(NV_PMC_BOOT_42);

    if (outBoot42)
        *outBoot42 = boot42;

    decodeBoot42(boot42);
    return true;
}

void NV3050Probe::decodeBoot0(UInt32 value)
{
    if (value == 0xFFFFFFFF) {
        NVLOG("NV_PMC_BOOT_0 = 0xFFFFFFFF -- device not responding. Check: "
              "memory space enabled, device in D0, correct BAR index.");
        return;
    }

    if (value == 0x00000000) {
        NVLOG("NV_PMC_BOOT_0 = 0x00000000 -- mapping points at nothing. "
              "Almost certainly the wrong BAR index.");
        return;
    }

    UInt32 arch  = nvBoot0Architecture(value);
    UInt32 impl  = nvBoot0Implementation(value);
    UInt32 major = nvBoot0MajorRevision(value);
    UInt32 minor = nvBoot0MinorRevision(value);

    NVLOG("NV_PMC_BOOT_0  = 0x%08x  arch=0x%02x (%s) impl=0x%x rev=%u.%u",
          (unsigned)value, (unsigned)arch, nvArchitectureName(arch),
          (unsigned)impl, (unsigned)major, (unsigned)minor);

    if (arch != NV_ARCHITECTURE_GA100) {
        NVLOG("  architecture is not Ampere -- either the wrong device was "
              "claimed, or the read is not landing where you think");

        if (arch == NV_ARCHITECTURE_AD100) {
            NVLOG("  this is an Ada part. If the laptop reports device ID "
                  "0x2822/0x28A3/0x28E3 it is an 'RTX 3050 A' -- Ada silicon "
                  "under a 30-series name. See ../DEVICE-IDS.md; the whole "
                  "plan retargets.");
        }
        return;
    }

    NVLOG("  Ampere confirmed: %s", nvAmpereImplementationName(impl));

    if (impl == NV_IMPLEMENTATION_GA107) {
        NVLOG("  GA107 -- this is the mobile RTX 3050 die. Phase 3 milestone "
              "reached. Compare against your Linux mmiotrace capture.");
    } else {
        NVLOG("  expected GA107 (impl 0x%x) for a mobile RTX 3050",
              NV_IMPLEMENTATION_GA107);
    }
}

void NV3050Probe::decodeBoot42(UInt32 value)
{
    if (nvLooksLikeDeadRead(value)) {
        NVLOG("NV_PMC_BOOT_42 = 0x%08x -- unexpected dead read at 0x%x while "
              "NV_PMC_BOOT_0 responded. Aperture may be shorter than 16 MiB.",
              (unsigned)value, (unsigned)NV_PMC_BOOT_42);
        return;
    }

    UInt32 arch   = nvBoot42Architecture(value);
    UInt32 impl   = nvBoot42Implementation(value);
    UInt32 chipId = nvBoot42ChipId(value);
    UInt32 major  = nvBoot42MajorRevision(value);
    UInt32 minor  = nvBoot42MinorRevision(value);
    UInt32 minorX = nvBoot42MinorExtRevision(value);

    NVLOG("NV_PMC_BOOT_42 = 0x%08x  chipid=0x%03x arch=0x%02x (%s) impl=0x%x "
          "rev=%u.%u.%u",
          (unsigned)value, (unsigned)chipId, (unsigned)arch,
          nvArchitectureName(arch), (unsigned)impl,
          (unsigned)major, (unsigned)minor, (unsigned)minorX);

    if (chipId == NV_CHIP_ID_GA107) {
        NVLOG("  chip ID 0x%03x matches GA107", (unsigned)NV_CHIP_ID_GA107);
        return;
    }

    NVLOG("  chip ID 0x%03x does not match the expected GA107 0x%03x",
          (unsigned)chipId, (unsigned)NV_CHIP_ID_GA107);

    if (chipId == NV_CHIP_ID_GA106) {
        NVLOG("  0x%03x is GA106 -- also sold as 'RTX 3050 Ti Laptop GPU'. "
              "Same architecture, different die and unit counts.",
              (unsigned)NV_CHIP_ID_GA106);
    }
}

#pragma mark - Phase 3.5: firmware health

//
// Poll NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(0) bits 7:0 until they read
// GFW_BOOT_PROGRESS_COMPLETED (0xFF). This mirrors NVIDIA's
// kgspWaitForGfwBootOk_TU102() in
// src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c:1184.
//
// GFW is the VBIOS-resident firmware that runs before the host may touch the
// GPU. NV_PMC_BOOT_0 responds even on a card whose firmware never started, so
// this is the read that actually proves the device is healthy rather than merely
// present.
//
// A poll rather than a single read because the kext can legitimately load while
// GFW is still running, particularly early in boot.
//
NVGfwBootVerdict NV3050Probe::pollGfwBoot(UInt32 *outProgress, UInt32 *outElapsedMs)
{
    UInt32 progress = 0;
    UInt32 elapsed  = 0;

    for (;;) {
        UInt32 raw = regRead32(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT);

        //
        // An all-ones read here while the identity registers responded means
        // the aperture is live but this offset is not decoding -- most likely a
        // short BAR map, since the GC6 island sits at ~0x118xxx.
        //
        if (raw == 0xFFFFFFFF) {
            NVLOG("pollGfwBoot: read 0xFFFFFFFF at 0x%x -- offset not decoding. "
                  "Check the BAR0 length reported above covers it.",
                  (unsigned)NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT);
            //
            // outProgress is deliberately NOT written. It used to be set to a
            // literal 0 here, and a literal 0 is byte-identical in the
            // IORegistry to a card whose GFW genuinely sits at step 0 -- so the
            // one arm that measured NOTHING published the most alarming
            // possible measurement. The caller's initialiser now survives, and
            // nvGfwBootProgressWasRead(NV_GFW_BOOT_UNREADABLE) is false, so the
            // publisher never puts it in the registry at all.
            //
            if (outElapsedMs) *outElapsedMs = elapsed;
            return NV_GFW_BOOT_UNREADABLE;
        }

        progress = nvGfwBootProgress(raw);

        if (nvGfwBootComplete(raw)) {
            if (outProgress)  *outProgress  = progress;
            if (outElapsedMs) *outElapsedMs = elapsed;
            return NV_GFW_BOOT_OK;
        }

        if (elapsed >= NV_GFW_BOOT_POLL_TIMEOUT_MS)
            break;

        //
        // IOSleep rather than IODelay: start() runs on a workloop thread where
        // blocking is permitted, and a 4-second busy-wait would stall boot.
        //
        IOSleep(NV_GFW_BOOT_POLL_INTERVAL_MS);
        elapsed += NV_GFW_BOOT_POLL_INTERVAL_MS;
    }

    //
    // The timeout arm DOES write the progress word: the register decoded, the
    // value is a real measurement of how far GFW got, and it is the single most
    // useful number an operator can have here. That is the whole distinction
    // this verdict exists to carry -- UNREADABLE measured nothing, TIMEOUT
    // measured something and it was not 0xFF.
    //
    if (outProgress)  *outProgress  = progress;
    if (outElapsedMs) *outElapsedMs = elapsed;
    return NV_GFW_BOOT_TIMEOUT;
}

//
// NV_USABLE_FB_SIZE_IN_MB == NV_PGC6_AON_SECURE_SCRATCH_GROUP_42, published by
// firmware once memory is initialised. See
// ampere/ga102/dev_gc6_island_addendum.h:33.
//
// Only meaningful after GFW boot completes -- before that the field is not yet
// populated, so callers must gate on pollGfwBoot() succeeding.
//
NVFbSizeVerdict NV3050Probe::readUsableFbSize(UInt32 *outMegabytes)
{
    UInt32 raw = regRead32(NV_USABLE_FB_SIZE_IN_MB);
    UInt32 mb  = raw & NV_USABLE_FB_SIZE_IN_MB_VALUE_MASK;

    NVFbSizeVerdict verdict = nvFbSizeVerdict(raw);

    //
    // The store happens ONLY on the accepting arm, and that ordering is the
    // whole fix. The previous version stored mb before any test, so a refused
    // read left 4294967295 in the caller's variable -- and the value mask is
    // 0xFFFFFFFF, so nothing about that number looks refused. The caller then
    // published it because it was greater than zero.
    //
    if (verdict == NV_FB_SIZE_OK) {
        if (outMegabytes)
            *outMegabytes = mb;
        return verdict;
    }

    switch (verdict) {
        case NV_FB_SIZE_UNREADABLE:
            NVLOG("readUsableFbSize: 0xFFFFFFFF -- offset 0x%x not decoding",
                  (unsigned)NV_USABLE_FB_SIZE_IN_MB);
            break;
        case NV_FB_SIZE_UNPUBLISHED:
            NVLOG("readUsableFbSize: reports 0 MB -- firmware has not published "
                  "the framebuffer size yet");
            break;
        default:
            NVLOG("readUsableFbSize: %u MB is outside the plausible range "
                  "%u-%u MB -- treating as a misread (%s)",
                  (unsigned)mb, (unsigned)NV_FB_SIZE_MIN_MB,
                  (unsigned)NV_FB_SIZE_MAX_MB, nvFbSizeVerdictName(verdict));
            break;
    }

    return verdict;
}

#pragma mark - Phase 3.75: GSP engine inspection

//
// Engine-relative Falcon read. Goes through nvGspFalconReg() rather than an
// open-coded add, so the three-address-space hazard documented at the top of
// NVFalcon.hpp cannot be reintroduced here by accident.
//
UInt32 NV3050Probe::gspFalconRead(UInt32 falconRelative)
{
    UInt32 absolute = nvGspFalconReg(falconRelative);

    if (!nvIsInGspAperture(absolute)) {
        NVLOG("gspFalconRead: 0x%x translates to 0x%x, outside NV_PGSP "
              "[0x%x..0x%x] -- refusing",
              (unsigned)falconRelative, (unsigned)absolute,
              (unsigned)NV_PGSP_BASE, (unsigned)NV_PGSP_LIMIT);
        return 0xFFFFFFFF;
    }

    return regRead32(absolute);
}

//
// RISC-V read. Uses NV_FALCON2_GSP_BASE (0x111000), one 4 KiB page ABOVE the
// Falcon block. Collapsing the two bases would land every access inside the
// Falcon register file, where the offsets are all valid registers -- no fault,
// just silent traffic to the wrong hardware.
//
UInt32 NV3050Probe::gspRiscvRead(UInt32 riscvRelative)
{
    UInt32 absolute = nvGspRiscvReg(riscvRelative);

    if (!nvIsInGspAperture(absolute)) {
        NVLOG("gspRiscvRead: 0x%x translates to 0x%x, outside NV_PGSP "
              "[0x%x..0x%x] -- refusing",
              (unsigned)riscvRelative, (unsigned)absolute,
              (unsigned)NV_PGSP_BASE, (unsigned)NV_PGSP_LIMIT);
        return 0xFFFFFFFF;
    }

    return regRead32(absolute);
}

//
// WPR2 precondition. NVIDIA's kgspPrepareForBootstrap_TU102() calls
// kgspIsWpr2Up_HAL() before pushing any firmware and refuses to proceed if the
// region is already up -- a raised WPR2 means a previous boot left the GPU
// holding a protected region, and the only way forward is a full engine reset.
//
// Returns one of NVWPR2_DOWN / NVWPR2_UP / NVWPR2_UNREADABLE.
//
// It used to return a bare bool meaning "precondition satisfied", and that bool
// was FALSE for two categorically different reasons: WPR2 genuinely UP, or the
// registers not decoding at all. The first is a fact about the machine the
// operator can act on; the second is a bug in THIS driver. See the verdict
// family in NVFalcon.hpp for why they are now separate.
//
// These are framebuffer registers at 0x1FAxxx, deliberately read through
// regRead32() rather than either GSP accessor -- they are nowhere near the GSP
// aperture and routing them through it would (correctly) be refused.
//
UInt32 NV3050Probe::checkWpr2(UInt32 *outAddrLo, UInt32 *outAddrHi)
{
    UInt32 lo = regRead32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    UInt32 hi = regRead32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);

    if (outAddrLo) *outAddrLo = lo;
    if (outAddrHi) *outAddrHi = hi;

    UInt32 verdict = nvWpr2Verdict(lo, hi);

    if (verdict == NVWPR2_UNREADABLE) {
        NVLOG("WPR2: unreadable -- ADDR_LO 0x%08x ADDR_HI 0x%08x at 0x%x/0x%x. "
              "At least one half is all-ones, which is what regRead32() "
              "returns for every refusal it has. These offsets come from "
              "turing/tu102/dev_fb.h; NVIDIA ships no ga102 dev_fb.h. Verify "
              "against the Linux mmiotrace capture.",
              (unsigned)lo, (unsigned)hi,
              (unsigned)NV_PFB_PRI_MMU_WPR2_ADDR_LO,
              (unsigned)NV_PFB_PRI_MMU_WPR2_ADDR_HI);
        return NVWPR2_UNREADABLE;
    }

    if (verdict == NVWPR2_DOWN) {
        NVLOG("WPR2: down (ADDR_HI = 0x%08x) -- firmware boot precondition "
              "satisfied", (unsigned)hi);
        return NVWPR2_DOWN;
    }

    //
    // Print the reconstructed physical range. VAL is a 4 KiB page number, so
    // the scale back up is 12, not the 4 used to extract the field.
    //
    unsigned long long base = nvWpr2AddressLo(lo);
    unsigned long long top  = nvWpr2AddressHi(hi);

    NVLOG("WPR2: UP -- region 0x%llx..0x%llx (ADDR_LO 0x%08x ADDR_HI 0x%08x)",
          base, top, (unsigned)lo, (unsigned)hi);
    NVLOG("      A previous boot left a write-protected region standing. "
          "NVIDIA's own bootstrap refuses to push firmware in this state; "
          "clearing it needs a full engine reset, which is a WRITE and "
          "therefore Phase 4 work.");

    return NVWPR2_UP;
}

//
// Read the GSP engine state Phase 4 will later have to drive, and report
// whether a firmware boot could even be attempted.
//
// Every access here is a read. The value of doing this before Phase 4 is that
// it answers, cheaply and safely, the one question the whole plan rests on:
// does macOS's PCI initialisation leave the GSP in a bootable state, or does
// it reset the engine out from under the VBIOS? If the answer is bad, it is
// far better to learn it from a log line than from a hang three months into
// firmware plumbing.
//
bool NV3050Probe::inspectGspEngine(void)
{
    UInt32 hwcfg2  = gspFalconRead(NV_PFALCON_FALCON_HWCFG2);

    if (nvLooksLikeDeadRead(hwcfg2)) {
        NVLOG("GSP: HWCFG2 = 0x%08x -- the GSP falcon block at 0x%x is not "
              "decoding. Either the engine is powered down or BAR0 maps short.",
              (unsigned)hwcfg2,
              (unsigned)nvGspFalconReg(NV_PFALCON_FALCON_HWCFG2));
        publishGspUnreadable(hwcfg2);
        return false;
    }

    UInt32 hwcfg        = gspFalconRead(NV_PFALCON_FALCON_HWCFG);
    UInt32 falconCpuctl = gspFalconRead(NV_PFALCON_FALCON_CPUCTL);
    UInt32 riscvCpuctl  = gspRiscvRead(NV_PRISCV_RISCV_CPUCTL);
    UInt32 bcrCtrl      = gspRiscvRead(NV_PRISCV_RISCV_BCR_CTRL);
    UInt32 privErr      = gspRiscvRead(NV_PRISCV_RISCV_PRIV_ERR_STAT);
    UInt32 hubErr       = gspRiscvRead(NV_PRISCV_RISCV_HUB_ERR_STAT);

    //
    // The three registers that LOCALIZE a priv fault. Until 2026-08-19 nothing
    // in this tree read any of them -- NV_PRISCV_RISCV_PRIV_ERR_INFO,
    // _PRIV_ERR_ADDR and _PRIV_ERR_ADDR_HI occurred at exactly one place each,
    // their own #define line in NVFalcon.hpp, and that was the whole extent of
    // their existence.
    //
    // Found by set-difference over the error-status block rather than by
    // reading code, which is 3050-handoff18 s1's method: five registers
    // defined, two consumed. PRIV_ERR_STAT says a privileged access was
    // refused; these three say WHICH access and at WHAT address. GSP has no
    // console, so this block is the only diagnostic Phase 4 will ever have, and
    // "something was refused" without an address is most of a bug report with
    // the actionable half missing.
    //
    UInt32 privErrInfo   = gspRiscvRead(NV_PRISCV_RISCV_PRIV_ERR_INFO);
    UInt32 privErrAddr   = gspRiscvRead(NV_PRISCV_RISCV_PRIV_ERR_ADDR);
    UInt32 privErrAddrHi = gspRiscvRead(NV_PRISCV_RISCV_PRIV_ERR_ADDR_HI);
    UInt32 engine       = regRead32(NV_PGSP_FALCON_ENGINE);

    bool riscvCapable = nvFalconIsRiscvCapable(hwcfg2);
    bool scrubDone    = nvFalconScrubbingDone(hwcfg2);
    bool inReset      = nvGspEngineInReset(engine);

    NVLOG("GSP: HWCFG2 = 0x%08x  riscv=%s scrubbing=%s",
          (unsigned)hwcfg2,
          riscvCapable ? "capable" : "NOT CAPABLE",
          scrubDone ? "done" : "in progress");

    NVLOG("GSP: HWCFG  = 0x%08x  IMEM %u blocks (%u bytes)",
          (unsigned)hwcfg,
          (unsigned)nvFalconImemSizeBlocks(hwcfg),
          (unsigned)nvFalconImemSizeBytes(hwcfg));

    NVLOG("GSP: FALCON_ENGINE = 0x%08x  reset=%s",
          (unsigned)engine, inReset ? "ASSERTED" : "released");

    NVLOG("GSP: falcon CPUCTL = 0x%08x  halted=%s alias_en=%s",
          (unsigned)falconCpuctl,
          nvFalconIsHalted(falconCpuctl) ? "yes" : "no",
          nvFalconAliasEnabled(falconCpuctl) ? "yes" : "no");

    NVLOG("GSP: riscv  CPUCTL = 0x%08x  active=%s halted=%s",
          (unsigned)riscvCpuctl,
          nvRiscvIsActive(riscvCpuctl) ? "yes" : "no",
          nvRiscvIsHalted(riscvCpuctl) ? "yes" : "no");

    NVLOG("GSP: BCR_CTRL = 0x%08x  valid=%s core=%s",
          (unsigned)bcrCtrl,
          nvBcrValid(bcrCtrl) ? "yes" : "no",
          nvBcrSelectsRiscv(bcrCtrl) ? "RISC-V" : "Falcon");

    //
    // Derived ONCE, here, and used by the log block, the publisher and the
    // go/no-go join below. Deriving it three times is how a predicate ends up
    // disagreeing with itself -- the same argument nvGspFalconReg() /
    // nvGspRiscvReg() are two functions for.
    //
    UInt32 errVerdict = nvRiscvErrorVerdict(privErr, hubErr);

    publishGspErrorLatches(errVerdict, privErr, hubErr,
                           privErrInfo, privErrAddr, privErrAddrHi);

    if (errVerdict == NVRISCV_ERR_UNREADABLE) {
        //
        // NOT an error report. The old bool answered TRUE here and this branch
        // said "ERROR LATCHES SET", naming a fault on a card nothing has
        // written to -- when what actually happened is that one of these two
        // reads was REFUSED. regRead32() reports every refusal by returning
        // all-ones, so a BAR0 mapping that covers the Falcon block and stops
        // before the RISC-V block one 4 KiB page above it produces exactly
        // this. Reset the engine versus fix this kext: opposite actions.
        //
        NVLOG("GSP: error latches UNREADABLE -- PRIV_ERR_STAT = 0x%08x  "
              "HUB_ERR_STAT = 0x%08x. At least one read all-ones, which is "
              "what regRead32() returns for every refusal it has. This is a "
              "fault in THIS DRIVER's mapping, not a latched hardware error.",
              (unsigned)privErr, (unsigned)hubErr);
    } else if (errVerdict != NVRISCV_ERR_NONE) {
        NVLOG("GSP: ERROR LATCHES SET (%s) -- PRIV_ERR_STAT = 0x%08x  "
              "HUB_ERR_STAT = 0x%08x",
              nvRiscvErrorVerdictName(errVerdict),
              (unsigned)privErr, (unsigned)hubErr);
        //
        // The three registers that say WHICH access and at WHAT address. Until
        // 2026-08-19 nothing in this tree read any of them, so every fault this
        // driver could ever have reported was "something was refused" with the
        // actionable half missing.
        //
        NVLOG("     PRIV_ERR_INFO = 0x%08x  PRIV_ERR_ADDR = 0x%08x  "
              "PRIV_ERR_ADDR_HI = 0x%08x",
              (unsigned)privErrInfo, (unsigned)privErrAddr,
              (unsigned)privErrAddrHi);
        NVLOG("     There is no console on GSP. This block is the only "
              "diagnostic Phase 4 will have, so a nonzero value here before we "
              "have written anything is worth understanding now.");
    } else {
        NVLOG("GSP: error latches clear (PRIV_ERR_STAT and HUB_ERR_STAT both 0)");
    }

    //
    // WPR2 precondition, mirroring kgspPrepareForBootstrap_TU102().
    //
    UInt32 wpr2Lo = 0;
    UInt32 wpr2Hi = 0;
    UInt32 wpr2Verdict = checkWpr2(&wpr2Lo, &wpr2Hi);

    bool wpr2Readable = (wpr2Verdict != NVWPR2_UNREADABLE);
    bool wpr2Down     = (wpr2Verdict == NVWPR2_DOWN);

    //
    // The verdict goes FIRST and on BOTH paths, so it sorts above every value
    // it qualifies in IORegistryExplorer and so its absence cannot be mistaken
    // for a healthy read on an older kext.
    //
    setProperty("GSPWpr2Verdict", nvWpr2VerdictName(wpr2Verdict));
    setProperty("GSPWpr2Readable", wpr2Readable);

    if (wpr2Readable) {
        setProperty("GSP_WPR2_ADDR_LO", wpr2Lo, 32);
        setProperty("GSP_WPR2_ADDR_HI", wpr2Hi, 32);
        setProperty("GSPWpr2Down", wpr2Down);
    } else {
        //
        // Neither address word is published. Both would be all-ones or an
        // arbitrary word next to one, and setProperty() with a refused read is
        // byte-identical to setProperty() with a register that genuinely holds
        // that value -- so publishing them would answer a question this driver
        // never asked the hardware.
        //
        setProperty("GSPWpr2UnreadableReason",
                    "At least one of NV_PFB_PRI_MMU_WPR2_ADDR_LO/_HI read "
                    "all-ones, which is what regRead32() returns for every "
                    "refusal it has. No WPR2 address was published and the "
                    "boot precondition is reported as NOT satisfied.");
    }

    //
    // Go/no-go. GA107's GSP must be able to run as RISC-V, must not be held in
    // engine reset, must not have latched a fault before we touched it, and
    // WPR2 must be down. Scrubbing is allowed to still be in progress --
    // Phase 4 waits on it.
    //
    // wpr2Down is false when the verdict is UNREADABLE as well as when WPR2 is
    // genuinely up, which is the safe direction: a precondition this driver
    // could not measure must not read as satisfied.
    //
    // The error term is an EQUALITY against NVRISCV_ERR_NONE, never a
    // disjunction over the three fault arms: that form is FALSE for UNREADABLE
    // and would send a precondition nobody measured through as satisfied.
    //
    bool ready = riscvCapable
              && !inReset
              && (errVerdict == NVRISCV_ERR_NONE)
              && wpr2Down;

    if (ready) {
        NVLOG("GSP: engine is in a bootable state. Phase 4 is viable on this "
              "hardware under macOS -- the firmware push is the next real "
              "work, and the first thing in this project that writes.");
    } else if (!riscvCapable) {
        NVLOG("GSP: NOT bootable -- HWCFG2 says this falcon cannot run as "
              "RISC-V. GA107's GSP should report capable, so either the read "
              "is landing somewhere unexpected or this is not the GSP engine.");
    } else if (inReset) {
        NVLOG("GSP: NOT bootable -- the engine is held in reset. Phase 4 will "
              "have to release it, which is a WRITE and therefore out of "
              "scope for this phase.");
    } else if (errVerdict == NVRISCV_ERR_UNREADABLE) {
        //
        // Split out from the fault arm below. That arm used to catch this case
        // and told the operator a fault had been latched, which is a claim
        // about the CARD made on evidence that is actually a claim about this
        // driver -- handoff22 s1b's ordering rule: report the bug somebody can
        // go and fix.
        //
        NVLOG("GSP: NOT bootable -- the error latches did not decode, so the "
              "fault precondition could not be measured. Not the same thing as "
              "a fault: check the BAR0 mapping length first.");
    } else if (errVerdict != NVRISCV_ERR_NONE) {
        NVLOG("GSP: NOT bootable -- a fault was latched before this driver "
              "issued any access at all (%s).",
              nvRiscvErrorVerdictName(errVerdict));
    } else {
        NVLOG("GSP: NOT bootable -- WPR2 is not down, or its address registers "
              "did not decode. See GSPWpr2Verdict.");
    }

    publishGspState(hwcfg2, hwcfg, falconCpuctl, riscvCpuctl, bcrCtrl,
                    privErr, hubErr, ready);

    return ready;
}

//
// Publish the RISC-V error-latch verdict, and the five register words behind it.
//
// GSPErrorVerdict and GSPErrorLatchesReadable go FIRST and on BOTH arms, so the
// verdict sorts above every value it qualifies and the flag's ABSENCE can never
// be confused with an older kext that did not publish it. That is the fourth
// family after GSPEngineReadable, NVIdentityReadable and NVBoot42Readable, and
// the reasoning is 3050-handoff33 s1a's: an absent key is a question rather than
// an answer.
//
// The three LOCALIZATION words are published only behind the readable arm.
// PRIV_ERR_STAT and HUB_ERR_STAT themselves are published on both arms and
// deliberately so -- on the UNREADABLE arm their all-ones value is the EVIDENCE
// for the verdict, and the verdict beside them says how to read it. The other
// three were never consulted on that path, and a word nobody read is
// byte-identical to a word that read that value (handoff18 s1d).
//
void NV3050Probe::publishGspErrorLatches(UInt32 errVerdict,
                                         UInt32 privErr, UInt32 hubErr,
                                         UInt32 privErrInfo,
                                         UInt32 privErrAddr,
                                         UInt32 privErrAddrHi)
{
    setProperty("GSPErrorVerdict", nvRiscvErrorVerdictName(errVerdict));

    //
    // An EQUALITY against the one unreadable arm, negated. Never a disjunction
    // over the three fault arms -- that form is false for a verdict added
    // later, and would report an unmeasured latch as readable.
    //
    setProperty("GSPErrorLatchesReadable",
                errVerdict != NVRISCV_ERR_UNREADABLE);

    setProperty("GSP_PRIV_ERR_STAT", privErr, 32);
    setProperty("GSP_HUB_ERR_STAT",  hubErr,  32);

    if (errVerdict == NVRISCV_ERR_UNREADABLE) {
        setProperty("GSPErrorLatchesReason",
                    "at least one of PRIV_ERR_STAT/HUB_ERR_STAT read "
                    "0xFFFFFFFF, which is what regRead32() returns for every "
                    "refusal it has; the localization registers were not "
                    "consulted and are NOT published");
        return;
    }

    setProperty("GSP_PRIV_ERR_INFO",    privErrInfo,   32);
    setProperty("GSP_PRIV_ERR_ADDR",    privErrAddr,   32);
    setProperty("GSP_PRIV_ERR_ADDR_HI", privErrAddrHi, 32);
}

void NV3050Probe::publishGspState(UInt32 hwcfg2, UInt32 hwcfg,
                                  UInt32 falconCpuctl, UInt32 riscvCpuctl,
                                  UInt32 bcrCtrl, UInt32 privErr,
                                  UInt32 hubErr, bool ready)
{
    setProperty("GSP_HWCFG2",         hwcfg2,       32);
    setProperty("GSP_HWCFG",          hwcfg,        32);
    setProperty("GSP_FALCON_CPUCTL",  falconCpuctl, 32);
    setProperty("GSP_RISCV_CPUCTL",   riscvCpuctl,  32);
    setProperty("GSP_BCR_CTRL",       bcrCtrl,      32);
    setProperty("GSP_PRIV_ERR_STAT",  privErr,      32);
    setProperty("GSP_HUB_ERR_STAT",   hubErr,       32);

    setProperty("GSPRiscvCapable", nvFalconIsRiscvCapable(hwcfg2));
    setProperty("GSPImemBytes", (UInt64)nvFalconImemSizeBytes(hwcfg), 32);
    setProperty("GSPBootable", ready);

    //
    // Published on BOTH paths on purpose. publishGspUnreadable() sets this
    // false, so if the key were absent here an operator would have to tell
    // "this kext reads the engine" from "this kext predates the key" by its
    // absence, which is exactly the inference the dead-read fix exists to
    // remove. Every GSP_* value above is a real read when this is true.
    //
    setProperty("GSPEngineReadable", true);

    if (ready)
        setProperty("NVPhase", "3.75 - GSP engine inspected and bootable, "
                               "no writes issued");
}

//
// The dead-read path. HWCFG2 came back looking like a non-decoding read, so
// inspectGspEngine() returned before issuing the other six reads.
//
// This used to call publishGspState(hwcfg2, 0, 0, 0, 0, 0, 0, false), and that
// was wrong in the quietest possible way: setProperty() with a literal 0 for a
// register nobody read produces an IORegistry entry BYTE-IDENTICAL to one that
// really did read zero. GSP_BCR_CTRL = 0 is a plausible measurement, not an
// obviously-bogus sentinel, so an operator reading IORegistryExplorer would
// have had no way to tell a powered-down engine from an engine reporting all
// clear. The two conclusions send them to opposite ends of the plan.
//
// GSPImemBytes was worse than merely absent: it was nvFalconImemSizeBytes(0),
// a DERIVED value computed from a register that was never read, published as a
// byte count.
//
// So publish only what was actually measured, and say plainly why the rest is
// missing. An absent key is a question the operator has to ask; a fabricated
// key is an answer they will believe.
//
void NV3050Probe::publishGspUnreadable(UInt32 hwcfg2)
{
    setProperty("GSP_HWCFG2", hwcfg2, 32);
    setProperty("GSPBootable", false);
    setProperty("GSPEngineReadable", false);

    setProperty("NVGspUnreadableReason",
                "HWCFG2 did not decode -- engine powered down or BAR0 short. "
                "The other GSP registers were never read and are therefore "
                "not published.");
}

#pragma mark - IORegistry publication

//
// Mirror what we learned into the IORegistry so it is visible in
// IORegistryExplorer without trawling the log.
//
void NV3050Probe::publishIdentity(UInt32 boot0, UInt32 boot42)
{
    setProperty("NV_PMC_BOOT_0",  boot0,  32);

    //
    // BOOT_42 is published ONLY if it decoded. decodeBoot42() already knows
    // this case exists -- BOOT_0 answered, BOOT_42 came back all-ones or
    // all-zeroes, the aperture is shorter than 16 MiB -- and it logs the case
    // and returns. Until 2026-08-18 the word went into the IORegistry anyway,
    // and setProperty() with a dead read is byte-identical to setProperty()
    // with a register that genuinely holds that value. handoff18 section 1d's
    // class, sixth occurrence.
    //
    // readIdentity() deliberately still returns true here: BOOT_0 decoded, so
    // the architecture and implementation names below are real measurements
    // and routing to publishIdentityUnreadable() would throw them away. The
    // honest split is at publication, not at the read.
    //
    // The flag goes on BOTH publication arms so its absence cannot be read as
    // an older kext -- same reasoning as GSPEngineReadable and
    // NVIdentityReadable -- and FIRST, so it sorts above the word it qualifies.
    //
    bool boot42Readable = !nvLooksLikeDeadRead(boot42);

    setProperty("NVBoot42Readable", boot42Readable);

    //
    // The chip verdict, published on BOTH arms and ahead of the word it
    // qualifies -- verbatim NVPartVerdict's reasoning one register over.
    //
    // decodeBoot42() has always detected all four of these cases and written
    // them to the log only. The registry carried NV_PMC_BOOT_42 as a raw word
    // and NVChipIdExpected as a statement of what this driver wants, and never
    // compared them -- so "is this the die the driver was written for?" was
    // left to whoever happened to be reading. GA106 is the case that matters:
    // it is the 'RTX 3050 Ti Laptop GPU', same architecture, different die and
    // different unit counts, and it is the near miss most likely to be taken
    // for the target part.
    //
    // NVChipIsTarget is an EQUALITY against NV_CHIP_TARGET, never a negation of
    // an error arm: != NV_CHIP_SIBLING_GA106 is true for a dead read and for
    // every chip ID this driver has never seen, which would report an
    // unmeasured register as the die the driver was written for.
    //
    NVChipVerdict chip = nvChipVerdict(boot42);

    setProperty("NVChipVerdict", nvChipVerdictName(chip));
    setProperty("NVChipIsTarget", chip == NV_CHIP_TARGET);

    if (chip == NV_CHIP_SIBLING_GA106) {
        setProperty("NVChipSiblingNote",
                    "Chip ID is GA106, also sold as 'RTX 3050 Ti Laptop GPU'. "
                    "Same Ampere architecture as the target GA107, but a "
                    "different die with different unit counts. Every register "
                    "offset in this driver was derived for GA107.");
    }

    if (boot42Readable) {
        setProperty("NV_PMC_BOOT_42", boot42, 32);
    } else {
        setProperty("NVBoot42UnreadableReason",
                    "NV_PMC_BOOT_42 read all-ones or all-zeroes while "
                    "NV_PMC_BOOT_0 decoded. The word is not published because "
                    "a dead read is indistinguishable from a register that "
                    "genuinely holds that value. The BAR0 aperture may be "
                    "shorter than 16 MiB.");
    }

    UInt32 arch = nvBoot0Architecture(boot0);
    UInt32 impl = nvBoot0Implementation(boot0);

    //
    // The part verdict is published FIRST and on every arm, ahead of every
    // value it qualifies -- same reasoning as NVBoot42Readable above and as
    // GSPEngineReadable / NVIdentityReadable / NVGspBootReadMask before it.
    //
    // Until 2026-08-18 this block published NVArchitecture as a bare NAME and
    // nothing else. decodeBoot0() already detected all four cases and wrote
    // them to the log; none of them reached here. So an Ada part published
    // "Ada" and an Ampere part published "Ampere", byte-identically as far as
    // an operator reading IORegistryExplorer is concerned, with no statement
    // that one of those two means the whole plan retargets. That is
    // 3050-handoff31.md s1's class again -- a diagnosis reaching the log and
    // not the consumer.
    //
    // NVPartIsTarget is an EQUALITY against NV_PART_TARGET, never a negation
    // of one of the error arms: a negation of NV_PART_ADA is true for every
    // non-Ada part including a Turing one, which would report a part this
    // driver has never seen as the part it was written for.
    //
    NVPartVerdict part = nvPartVerdict(boot0);

    setProperty("NVPartVerdict", nvPartVerdictName(part));
    setProperty("NVPartIsTarget", part == NV_PART_TARGET);

    setProperty("NVArchitecture", nvArchitectureName(arch));

    //
    // NVImplementation is published on EVERY arm now. It used to be guarded by
    // the Ampere test, so a non-Ampere part left the key ABSENT -- and an
    // absence is a question rather than an answer: it reads identically to an
    // older kext that never published the key at all. The name function
    // answers "unrecognised Ampere part" for a non-Ampere implementation
    // number, which is a statement, and NVPartVerdict above says why.
    //
    setProperty("NVImplementation", nvAmpereImplementationName(impl));

    if (part == NV_PART_ADA) {
        setProperty("NVPartRetargetNote",
                    "Ada silicon. If the laptop reports device ID "
                    "0x2822/0x28A3/0x28E3 this is an 'RTX 3050 A' -- Ada under "
                    "a 30-series name. See DEVICE-IDS.md; the whole plan "
                    "retargets and no constant in this driver applies.");
    }

    setProperty("NVChipIdExpected", (UInt64)NV_CHIP_ID_GA107, 32);

    if (fPCIDevice) {
        setProperty("NVVendorID",  fPCIDevice->configRead16(kIOPCIConfigVendorID), 16);
        setProperty("NVDeviceID",  fPCIDevice->configRead16(kIOPCIConfigDeviceID), 16);
        setProperty("NVSubsystem", fPCIDevice->configRead32(kIOPCIConfigSubSystemVendorID), 32);
    }

    setProperty("NVPhase", "3 - register reads only, no GPU initialisation");
    setProperty("NVIdentityReadable", true);
}

//
// The dead-read counterpart of publishIdentity().
//
// readIdentity() returns false without ever issuing the BOOT_42 read, and it
// deliberately leaves outBoot42 alone rather than filling it with boot0. So
// there is no BOOT_42 measurement to publish here, and this function does not
// invent one: NV_PMC_BOOT_42 is ABSENT from the IORegistry on this path.
//
// That absence is the whole point. setProperty("NV_PMC_BOOT_42", boot0, 32) is
// byte-identical in IORegistryExplorer to a register that genuinely returned
// that value, so an operator staring at two equal all-ones words cannot tell a
// card that answered from an aperture that never did. Same class as handoff18
// s1d one level up in inspectGspEngine(), and the same remedy: publish the one
// real measurement, say plainly why the rest is missing.
//
// NVIdentityReadable is published on BOTH paths so its absence means "older
// kext" rather than "healthy", which is what a flag published on only the
// failure path would mean.
//
void NV3050Probe::publishIdentityUnreadable(UInt32 boot0)
{
    setProperty("NV_PMC_BOOT_0", boot0, 32);
    setProperty("NVIdentityReadable", false);

    //
    // Published on this arm too, so a reader never has to distinguish "the
    // flag is missing because BOOT_42 was never reached" from "the flag is
    // missing because this is an older kext". BOOT_42 was not read on this
    // path at all, which is strictly less readable than a dead read.
    //
    setProperty("NVBoot42Readable", false);

    setProperty("NVIdentityUnreadableReason",
                "NV_PMC_BOOT_0 did not decode -- all-ones or all-zeroes. "
                "NV_PMC_BOOT_42 was never read and is therefore not published, "
                "and no architecture or implementation name was derived from "
                "a value that is not a chip ID.");

    setProperty("NVChipIdExpected", (UInt64)NV_CHIP_ID_GA107, 32);

    if (fPCIDevice) {
        setProperty("NVVendorID",  fPCIDevice->configRead16(kIOPCIConfigVendorID), 16);
        setProperty("NVDeviceID",  fPCIDevice->configRead16(kIOPCIConfigDeviceID), 16);
    }

    setProperty("NVPhase", "3 - identity read FAILED, aperture not live");
}

void NV3050Probe::publishFirmwareState(NVGfwBootVerdict gfwVerdict, UInt32 gfwProgress,
                                       NVFbSizeVerdict fbVerdict, UInt32 fbSizeMB)
{
    //
    // The verdict and the readable flag go FIRST and on EVERY path, verbatim the
    // reasoning behind GSPEngineReadable, NVIdentityReadable, NVBoot42Readable
    // and NVGspBootReadMask: an ABSENT key is a question rather than an answer,
    // indistinguishable from an older kext that never published it.
    //
    setProperty("NVGfwBootVerdict", nvGfwBootVerdictName(gfwVerdict));
    setProperty("NVGfwBootProgressRead", nvGfwBootProgressWasRead(gfwVerdict));

    //
    // An EQUALITY against the accepting verdict, never a negation of one error
    // arm: `gfwVerdict != NV_GFW_BOOT_UNREADABLE` is true for NOT_ATTEMPTED and
    // for TIMEOUT, so it would report a card that never finished booting as
    // healthy.
    //
    setProperty("NVGfwBootOk", nvGfwBootVerdictIsOk(gfwVerdict));

    //
    // The progress word is published ONLY when the verdict says the register
    // actually decoded. On the UNREADABLE arm pollGfwBoot() measured nothing and
    // writes nothing, so this word would be the caller's initialiser -- and a
    // literal 0 is byte-identical in the IORegistry to a card whose GFW
    // genuinely sits at step 0. That is handoff18 section 1d's class, and it is
    // the defect this signature change exists to close.
    //
    if (nvGfwBootProgressWasRead(gfwVerdict))
        setProperty("NVGfwBootProgress", (UInt64)gfwProgress, 32);

    //
    // The verdict is published FIRST and on EVERY path, verbatim the reasoning
    // behind GSPEngineReadable, NVIdentityReadable, NVBoot42Readable and
    // NVGspBootReadMask: an ABSENT key is a question rather than an answer,
    // indistinguishable from an older kext that never published it.
    //
    setProperty("NVUsableFramebufferVerdict", nvFbSizeVerdictName(fbVerdict));
    setProperty("NVUsableFramebufferMeasured", fbVerdict == NV_FB_SIZE_OK);

    //
    // An EQUALITY against the accepting verdict, never a magnitude test and
    // never a negation of one error arm: `fbVerdict != NV_FB_SIZE_UNREADABLE`
    // is true for UNPUBLISHED and IMPLAUSIBLE, and `fbSizeMB > 0` was true for
    // a refused read masked to 4294967295. Both publish a word nobody measured.
    //
    if (fbVerdict == NV_FB_SIZE_OK)
        setProperty("NVUsableFramebufferMB", (UInt64)fbSizeMB, 32);

    setProperty("NVPhase", nvGfwBootVerdictIsOk(gfwVerdict)
                ? "3.5 - identity + firmware health confirmed, no writes issued"
                : "3 - identity read, firmware health NOT confirmed");
}

#pragma mark - Phase 4: GSP bringup

//
// Trampolines. NVGspBoot.hpp knows nothing about IOKit; it calls these two
// through function pointers with `this` as the opaque context.
//
// Both take an ABSOLUTE BAR0 offset -- the state machine has already done the
// aperture arithmetic via nvGspFalconReg()/nvGspRiscvReg() and has already
// checked the result against nvIsInGspAperture(). These functions deliberately
// do NOT re-derive an address; doing so twice in two places is how a base
// constant ends up disagreeing with itself.
//
NVU32 NV3050Probe::gspBootRead32(void *ctx, NVU32 bar0Offset)
{
    NV3050Probe *self = (NV3050Probe *)ctx;
    if (!self)
        return 0;
    return (NVU32)self->regRead32((UInt32)bar0Offset);
}

void NV3050Probe::gspBootWrite32(void *ctx, NVU32 bar0Offset, NVU32 value)
{
    NV3050Probe *self = (NV3050Probe *)ctx;
    if (!self)
        return;
    self->regWrite32((UInt32)bar0Offset, (UInt32)value);
}

//
// Drive the Phase 4 state machine against real silicon.
//
// The sequence itself is in NVGspBoot.hpp and is exercised on a host compiler
// in tests/, so what happens here is only: build the I/O struct, spin the
// machine, sleep between polls, and report. No register knowledge lives in
// this function -- if you find yourself adding an offset here, it belongs in
// the header instead.
//
// With fAllowGspWrites false (every stock build) this reaches the first write,
// stops with ERR_WRITES_DISABLED, and reports which write it would have
// issued. That is a useful result, not a failure: it proves the preconditions
// hold and names the exact next action.
//
bool NV3050Probe::attemptGspBoot(void)
{
    NVGspBootIo io;
    io.read32      = &NV3050Probe::gspBootRead32;
    io.write32     = &NV3050Probe::gspBootWrite32;
    io.ctx         = this;
    io.allowWrites = fAllowGspWrites;

    NVGspBootParams params = nvGspBootObservedParams();

    //
    // Poll budgets are counted in transitions, and this loop spends
    // NV_GFW_BOOT_POLL_INTERVAL_MS (10 ms) per transition. The trace's ~721 ms
    // boot window therefore needs ~73 polls; 400 gives five times that
    // headroom and bounds the wait at 4 s -- the same ceiling pollGfwBoot()
    // already accepts via NV_GFW_BOOT_POLL_TIMEOUT_MS.
    //
    params.maxActivePolls = 400;
    params.maxScrubPolls  = 100;

    //
    // skipFirmwareLoad stays FALSE on hardware, always. Setting it would start
    // a RISC-V core whose IMEM was never filled -- the exact hang this project
    // exists to avoid. It is a test-only escape hatch.
    //
    params.skipFirmwareLoad = false;

    //
    // Phase 5 -- stage the bootloader image, if one was supplied.
    //
    // nvGspBootObservedParams() leaves params.image.valid false, and that is a
    // legitimate outcome: with no blob the machine stops at
    // FIRMWARE_NOT_IMPLEMENTED exactly as it did in Phase 4. A failure to stage
    // is therefore NOT a reason to abort the run -- the dry rehearsal is still
    // worth doing, and stopping honestly at FIRMWARE_NOT_IMPLEMENTED reports
    // more than refusing to start.
    //
    OSData *blob = copyGspFirmwareBlob();
    if (blob) {
        NVGspFirmwareImage image;
        image.physAddrLo = 0;
        image.physAddrHi = 0;
        image.sizeBytes  = 0;
        image.imemOffset = 0;
        image.valid      = false;

        if (loadGspFirmware((const unsigned char *)blob->getBytesNoCopy(),
                            (UInt32)blob->getLength(),
                            &image)) {
            params.image = image;
            NVLOG("Phase 5: staged %u bytes at phys 0x%08x_%08x, IMEM offset 0x%x",
                  (unsigned)image.sizeBytes,
                  (unsigned)image.physAddrHi,
                  (unsigned)image.physAddrLo,
                  (unsigned)image.imemOffset);
        } else {
            NVLOG("Phase 5: firmware staging FAILED -- continuing without an "
                  "image. The run will stop at FIRMWARE_NOT_IMPLEMENTED.");
        }

        OSSafeReleaseNULL(blob);
    } else {
        NVLOG("Phase 5: no NVGspFirmware property present -- no image to load.");
    }

    NVGspBoot machine;
    nvGspBootInit(&machine, io, params);

    NVLOG("Phase 4: starting GSP bringup (writes %s)",
          fAllowGspWrites ? "ENABLED" : "DISABLED -- dry rehearsal");

    //
    // IOSleep, NOT IODelay. pollGfwBoot() at line ~508 already settled this
    // question for this file: start() runs on a workloop thread where blocking
    // is permitted, and a multi-second busy-wait stalls boot. An IODelay per
    // transition across a large poll budget is precisely that busy-wait, so it
    // would have contradicted the reasoning ten lines up in the same file.
    //
    // The same interval constant is reused rather than a new number, so there
    // is one polling cadence in this driver instead of two.
    //
    UInt32 steps = 0;
    while (nvGspBootStep(&machine)) {
        IOSleep(NV_GFW_BOOT_POLL_INTERVAL_MS);

        //
        // Backstop only. The machine's own budgets bound it at roughly
        // maxScrubPolls + maxActivePolls + a handful of one-shot transitions,
        // so this can only fire if those budgets were mis-set.
        //
        if (++steps >= 1000) {
            NVLOG("Phase 4: step budget exhausted -- aborting the loop rather "
                  "than spinning in start()");
            break;
        }
    }

    NVLOG("Phase 4: %s (state %s) after %u transitions, %u writes issued",
          nvGspBootResultName(machine.result),
          nvGspBootStateName(machine.state),
          (unsigned)steps, (unsigned)machine.writeCount);

    switch (machine.result) {
    case NVGSP_RESULT_ERR_WRITES_DISABLED:
        NVLOG("     Dry rehearsal complete. Preconditions held and the first "
              "write it would have issued targets BAR0 offset 0x%x. Rebuild "
              "with -DPHASE4_ALLOW_WRITES to let it land.",
              (unsigned)machine.faultingOffset);
        break;

    case NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED:
        NVLOG("     Reached the Phase 5 boundary: reset and scrub completed, "
              "and the next step is DMAing a signed GSP image into IMEM, "
              "which this driver does not have. This is the designed end of "
              "Phase 4.");
        break;

    case NVGSP_RESULT_ERR_NOT_HALTED:
        NVLOG("     CPUCTL = 0x%08x -- the core was NOT halted. Something "
              "already started it, so resetting it here would be destroying "
              "state rather than initialising it.",
              (unsigned)machine.cpuctl);
        break;

    case NVGSP_RESULT_ERR_SCRUB_TIMEOUT:
        NVLOG("     HWCFG2 = 0x%08x after %u polls -- MEM_SCRUBBING never read "
              "back 0. Remember the sense is inverted: 0 means DONE.",
              (unsigned)machine.hwcfg2, (unsigned)machine.scrubPolls);
        break;

    case NVGSP_RESULT_ERR_RISCV_FAULT:
        NVLOG("     PRIV_ERR_STAT = 0x%08x  HUB_ERR_STAT = 0x%08x  verdict %s. "
              "The core faulted during boot; these two registers are the only "
              "diagnostics GSP has. This is a fact about the CARD -- an engine "
              "reset is the action.",
              (unsigned)machine.privErrStat, (unsigned)machine.hubErrStat,
              nvRiscvErrorVerdictName(machine.errVerdict));
        break;

    //
    // Split out of the fault arm above, and the same split inspectGspEngine()
    // already draws on its own NOT-bootable path. A refused read reported as a
    // latched fault sends an operator looking for a card problem over evidence
    // that is actually a claim about this driver's BAR mapping.
    //
    case NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE:
        NVLOG("     PRIV_ERR_STAT = 0x%08x  HUB_ERR_STAT = 0x%08x -- at least "
              "one half answered with the refusal value, so the boot was "
              "stopped WITHOUT a fault having been established. That is a bug "
              "in THIS driver (mapping or offset), not a fault in the card.",
              (unsigned)machine.privErrStat, (unsigned)machine.hubErrStat);
        break;

    case NVGSP_RESULT_ERR_ACTIVE_TIMEOUT:
        NVLOG("     RISCV CPUCTL = 0x%08x after %u polls -- ACTIVE_STAT never "
              "set and neither error latch fired. The core was released and "
              "then said nothing.",
              (unsigned)machine.riscvCpuctl, (unsigned)machine.activePolls);
        break;

    case NVGSP_RESULT_ERR_APERTURE_VIOLATION:
        NVLOG("     BUG: the machine tried to touch BAR0 offset 0x%x, which is "
              "outside the GSP aperture. A base constant is wrong -- do not "
              "load this build again until tests/ reproduces it.",
              (unsigned)machine.faultingOffset);
        break;

    case NVGSP_RESULT_RUNNING:
        NVLOG("     GSP is RUNNING. RISCV CPUCTL = 0x%08x.",
              (unsigned)machine.riscvCpuctl);
        break;

    default:
        break;
    }

    publishGspBootResult(&machine);

    return nvGspBootReachedExpectedEnd(&machine);
}

void NV3050Probe::publishGspBootResult(const NVGspBoot *machine)
{
    if (!machine)
        return;

    setProperty("NVGspBootResult", nvGspBootResultName(machine->result));
    setProperty("NVGspBootState",  nvGspBootStateName(machine->state));

    setProperty("NVGspBootWrites",      (UInt64)machine->writeCount,  32);
    setProperty("NVGspBootScrubPolls",  (UInt64)machine->scrubPolls,  32);
    setProperty("NVGspBootActivePolls", (UInt64)machine->activePolls, 32);

    //
    // The five diagnostic words, each published ONLY if this run actually read
    // it. NVGspBoot zero-initialises all five and assigns each one inside the
    // state that reads it, so a run that stops at PROBE never touches
    // riscvCpuctl, privErrStat or hubErrStat -- and publishing the zero would
    // be byte-identical to publishing a register that genuinely read clear.
    // handoff18 section 1d's class: a fabricated word an operator cannot tell
    // from a measured one.
    //
    // The MASK is published on every path and FIRST, so its absence cannot be
    // confused with an older kext and so it sorts above the words it qualifies.
    // Same reasoning as GSPEngineReadable and NVIdentityReadable.
    //
    setProperty("NVGspBootReadMask", (UInt64)machine->readMask, 32);

    if (nvGspBootWasRead(machine, NVGSP_READ_CPUCTL))
        setProperty("NVGspBootCpuctl",      (UInt64)machine->cpuctl,      32);
    if (nvGspBootWasRead(machine, NVGSP_READ_HWCFG2))
        setProperty("NVGspBootHwcfg2",      (UInt64)machine->hwcfg2,      32);
    if (nvGspBootWasRead(machine, NVGSP_READ_RISCV_CPUCTL))
        setProperty("NVGspBootRiscvCpuctl", (UInt64)machine->riscvCpuctl, 32);
    if (nvGspBootWasRead(machine, NVGSP_READ_PRIV_ERR_STAT))
        setProperty("NVGspBootPrivErrStat", (UInt64)machine->privErrStat, 32);
    if (nvGspBootWasRead(machine, NVGSP_READ_HUB_ERR_STAT))
        setProperty("NVGspBootHubErrStat",  (UInt64)machine->hubErrStat,  32);

    //
    // The DECODE of that pair, guarded by the conjunction of both bits rather
    // than by either one. nvGspBootErrVerdictKnown() is that conjunction and
    // lives beside the mask in NVGspBoot.hpp, so the guard cannot drift from
    // the field it protects.
    //
    // Guarded rather than published on every path, unlike the mask above,
    // because the initialiser value NVRISCV_ERR_NONE is ALSO the healthy
    // reading -- publishing it unconditionally would tell an operator that a
    // run which stopped at PROBE had checked both latches and found them
    // clear. That is the same fabricated-publication class the five words
    // above are guarded against, one level up: here the fabricated thing is a
    // DECODE rather than a register word.
    //
    if (nvGspBootErrVerdictKnown(machine))
        setProperty("NVGspBootErrVerdict",
                    nvRiscvErrorVerdictName(machine->errVerdict));

    if (machine->faultingOffset)
        setProperty("NVGspBootFaultingOffset",
                    (UInt64)machine->faultingOffset, 32);

    setProperty("NVGspWritesAllowed", fAllowGspWrites);

    setProperty("NVPhase", fAllowGspWrites
                ? "4 - GSP bringup attempted with writes ENABLED"
                : "4 - GSP bringup dry rehearsal, still zero writes issued");
}

#pragma mark - Phase 5 firmware staging

//
// Fetch the GSP firmware blob from the IORegistry.
//
// A property rather than a file read, for two reasons. First, a kext reading
// from the filesystem during start() is a layering and timing hazard: the
// driver may match before the volume holding the blob is mounted, and there is
// no ordering guarantee that would make that deterministic. Second, this
// project has no signed blob to ship -- the GA107 GSP binaries are
// NVIDIA-signed, redistributed with the Linux driver, and must not be
// re-published (reference/ga107-firmware/README.md). Keeping the blob outside
// the bundle means the kext is committable and the firmware is not.
//
// copyProperty() rather than getProperty() so the returned object carries a
// reference the caller owns. getProperty() hands back a borrowed pointer that
// the registry may drop underneath a DMA that is still in flight.
//
// Returns nullptr when no blob is present, which is the normal case and is not
// an error: attemptGspBoot() then leaves params.image.valid false and the state
// machine stops honestly at FIRMWARE_NOT_IMPLEMENTED.
//
OSData *NV3050Probe::copyGspFirmwareBlob(void)
{
    OSObject *prop = copyProperty(kNVGspFirmwareProperty);
    if (!prop)
        return nullptr;

    OSData *blob = OSDynamicCast(OSData, prop);
    if (!blob) {
        //
        // Present but the wrong type. Say so rather than failing silently --
        // a plist typo is otherwise indistinguishable from no blob at all.
        //
        NVLOG("GSP firmware: property \"%s\" exists but is not OSData -- ignoring",
              kNVGspFirmwareProperty);
        prop->release();
        return nullptr;
    }

    return blob;
}

//
// Publish the staging verdict. Two keys carry the STAGE result and six-way
// refusal; two more carry the eight-way ucode verdict beneath it.
//
// Both pairs are published on EVERY arm. An absent key is a question rather
// than an answer -- indistinguishable from an older kext that never published
// it -- so the ucode key carries the string "not evaluated" when the plan
// bailed before reaching nvRiscvUcodeVerdict(), rather than being omitted.
// That literal is spelled here rather than taken from
// nvRiscvUcodeVerdictName(NV_RISCV_UCODE_VERDICT_LIMIT), because a sentinel
// guessed out of a sibling function is a value this file cannot vouch for.
//
// NVGspStageOk is an EQUALITY against NVGSP_STAGE_OK, never a negation of a
// particular error arm: `planned != NVGSP_STAGE_ERR_NO_BLOB` is true for the
// five other refusals and would report a refused blob as staged.
//
void NV3050Probe::publishGspStageVerdict(NVGspStageResult planned,
                                         const NVGspStagePlan *plan)
{
    setProperty("NVGspStageVerdict", nvGspStageResultName(planned));
    setProperty("NVGspStageOk", planned == NVGSP_STAGE_OK);

    if (!plan) {
        setProperty("NVGspStageUcodeEvaluated", false);
        setProperty("NVGspStageUcode", "not evaluated");
        return;
    }

    setProperty("NVGspStageUcodeEvaluated", plan->ucodeEvaluated);
    setProperty("NVGspStageUcode",
                plan->ucodeEvaluated ? nvRiscvUcodeVerdictName(plan->ucode)
                                     : "not evaluated");
}

//
// Validate `blob`, copy its bootloader region into a physically contiguous,
// 41-bit-addressable, 256-byte-aligned kernel buffer, and fill *outImage with
// the physical base the state machine will program into DMATRFBASE.
//
// Every early return leaves *outImage invalid, so a caller that ignores the
// return value still cannot hand the GPU a half-built descriptor.
//
bool NV3050Probe::loadGspFirmware(const unsigned char *blob, UInt32 blobLen,
                                  NVGspFirmwareImage *outImage)
{
    if (!outImage)
        return false;

    //
    // Set the failed state FIRST. Everything below can bail, and `valid` false
    // is the one field that must be correct on every path out of here.
    //
    outImage->physAddrLo = 0;
    outImage->physAddrHi = 0;
    outImage->sizeBytes  = 0;
    outImage->imemOffset = 0;
    outImage->valid      = false;

    //
    // ALL of the validation and the padding arithmetic lives in NVGspStage.hpp,
    // which is dependency-free and is exercised by tests/test_gsp_stage.cpp on
    // a host compiler. Nothing below re-derives any of it -- that header is the
    // single place where "is this blob usable, and how many bytes do we need"
    // is decided, so the answer this driver acts on is the same answer the
    // tests check.
    //
    NVGspStagePlan     plan;
    NVGspStageResult   planned = nvGspStagePlan(blob, (NVU32)blobLen, &plan);

    //
    // Publish BEFORE acting on the refusal, and on BOTH arms. The log line
    // below reaches a console nobody is reading on a machine that failed to
    // boot firmware; the IORegistry is what an operator can actually inspect
    // afterwards. Publishing only inside the refusal arm would make the keys'
    // ABSENCE mean two different things -- staged fine, or an older kext.
    //
    // nvGspStagePlan() fills *outPlan on every path it can take with a
    // non-null out-param, including its own early returns, so `plan` is
    // initialised here whatever `planned` says.
    //
    publishGspStageVerdict(planned, &plan);

    if (planned != NVGSP_STAGE_OK) {
        NVLOG("GSP firmware: refusing %u-byte blob -- %s (ucode: %s)",
              (unsigned)blobLen, nvGspStageResultName(planned),
              plan.ucodeEvaluated ? nvRiscvUcodeVerdictName(plan.ucode)
                                  : "not evaluated");
        return false;
    }

    UInt32 fileOff = (UInt32)plan.sourceOffset;
    UInt32 blSize  = (UInt32)plan.sourceBytes;
    UInt32 padded  = (UInt32)plan.paddedBytes;

    //
    // Release any previous staging before allocating a new one. Idempotent, so
    // a retried boot attempt does not leak a contiguous allocation -- which is
    // the scarcest thing this driver asks the kernel for.
    //
    releaseGspFirmware();

    //
    // The 41-bit constraint is the whole reason this uses a physical mask and
    // an IODMACommand rather than a plain allocation. DMATRFBASE carries
    // physical bits 39:8 and DMATRFBASE1 continues the SHIFTED value with
    // physical bits 48:40 (NVFalcon.hpp:217, NVGspBoot.hpp:119-143), so the
    // register pair reaches further than 41 bits. 41 is the ceiling this driver
    // ALLOWS: an address above it is refused up front rather than TRUNCATED
    // into a valid-looking one that would make the engine read the wrong memory
    // without faulting. Constrain the allocation and the truncation cannot
    // happen.
    //
    // kIOMemoryPhysicallyContiguous because the engine is handed ONE base
    // address and a length; it does not walk a scatter list.
    //
    fGspFwBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionOut | kIOMemoryPhysicallyContiguous,
        (mach_vm_address_t)padded,
        (mach_vm_address_t)NV_GSP_FW_PHYS_MASK);

    if (!fGspFwBuffer) {
        NVLOG("GSP firmware: could not allocate %u bytes of 41-bit-addressable "
              "contiguous memory", (unsigned)padded);
        return false;
    }

    if (fGspFwBuffer->prepare(kIODirectionOut) != kIOReturnSuccess) {
        NVLOG("GSP firmware: prepare() failed -- pages not wired, refusing to "
              "hand the GPU an address");
        releaseGspFirmware();
        return false;
    }

    //
    // Set IMMEDIATELY after the success check and before anything that can bail.
    // Every failure path below calls releaseGspFirmware(), which now only calls
    // complete() when this is true -- so a missed assignment here would leak the
    // wired pages instead of double-completing them. Both directions are wrong;
    // the flag has to track prepare() exactly, not approximately.
    //
    fGspFwPrepared = true;

    void *dst = fGspFwBuffer->getBytesNoCopy();
    if (!dst || fGspFwBuffer->getCapacity() < (IOByteCount)padded) {
        NVLOG("GSP firmware: buffer smaller than requested (%u bytes)",
              (unsigned)padded);
        releaseGspFirmware();
        return false;
    }

    //
    // Zero the whole buffer before the copy so the padding tail is defined.
    // Uninitialised kernel memory handed to a DMA engine is an information
    // leak as well as a nondeterminism.
    //
    memset(dst, 0, padded);
    memcpy(dst, blob + fileOff, blSize);

    //
    // Resolve the physical address through IODMACommand rather than
    // getPhysicalSegment(), because withSpecification()'s numAddressBits is
    // where the 41-bit width is actually enforced against the mapper.
    //
    //
    // kMapped rather than a bare 0: mappingOptions is an unscoped enum in
    // Apple's header, and C++ will not implicitly convert int to one. The
    // literal 0 compiled here only because tests/kextshim/ had declared the
    // parameter UInt32 -- see tests/kextshim/SIGNATURES.md.
    //
    fGspFwDma = IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        NV_GSP_FW_ADDR_BITS,
        (UInt64)padded,
        IODMACommand::kMapped,
        (UInt64)padded,
        (UInt32)NVGSP_DMA_CHUNK_BYTES);

    if (!fGspFwDma) {
        NVLOG("GSP firmware: could not create a %u-bit IODMACommand",
              (unsigned)NV_GSP_FW_ADDR_BITS);
        releaseGspFirmware();
        return false;
    }

    if (fGspFwDma->setMemoryDescriptor(fGspFwBuffer, true) != kIOReturnSuccess) {
        NVLOG("GSP firmware: setMemoryDescriptor() failed");
        releaseGspFirmware();
        return false;
    }

    //
    // Set IMMEDIATELY after the success check, for the same reason as
    // fGspFwPrepared: every failure path below calls releaseGspFirmware(), and
    // the teardown now only clears the descriptor when this is true. A bail-out
    // between the two would leave the command holding a descriptor that nothing
    // ever tells it to let go of -- the LEAK direction, opposite to the
    // double-release the flag exists to prevent.
    //
    fGspFwDmaHasDescriptor = true;

    UInt64                   offset      = 0;
    IODMACommand::Segment64  seg;
    UInt32                   numSegments = 1;

    seg.fIOVMAddr = 0;
    seg.fLength   = 0;

    if (fGspFwDma->gen64IOVMSegments(&offset, &seg, &numSegments)
            != kIOReturnSuccess
        || numSegments != 1) {
        //
        // More than one segment means the allocation was not contiguous after
        // all. The engine takes a single base, so a scattered buffer cannot be
        // described to it -- stop rather than program the first fragment and
        // let the GPU read whatever follows it.
        //
        NVLOG("GSP firmware: expected 1 contiguous DMA segment, got %u",
              (unsigned)numSegments);
        releaseGspFirmware();
        return false;
    }

    if (seg.fLength < (UInt64)padded) {
        NVLOG("GSP firmware: segment covers %llu of %u bytes",
              (unsigned long long)seg.fLength, (unsigned)padded);
        releaseGspFirmware();
        return false;
    }

    //
    // Re-check the hardware constraints against the address actually handed
    // back, not against the mask that was requested. The mask is a request;
    // this is the answer.
    //
    // Both checks live in nvGspStageAddressOk() so the alignment test and the
    // magnitude test stay two separate tests. Folding them into one comparison
    // against NV_GSP_FW_PHYS_MASK reads as correct and is wrong -- the mask has
    // its low 8 bits cleared to force alignment, so it rejects every address in
    // the low 256 bytes of an otherwise-valid region.
    //
    // Checked as a RANGE, not as a base. The engine is handed this base and a
    // length, so a base that fits the window while base+padded does not would
    // make it read past the addressable region -- and nvGspStageAddressOk(),
    // which takes no length, cannot see that. nvGspStageRangeOk() performs the
    // alignment and base checks first and forwards their codes unchanged, so
    // the log below still names the specific constraint that fired.
    NVGspStageResult addrOk = nvGspStageRangeOk((unsigned long long)seg.fIOVMAddr,
                                               (unsigned long long)padded);
    if (addrOk != NVGSP_STAGE_OK) {
        NVLOG("GSP firmware: physical range 0x%llx + %u bytes rejected -- %s",
              (unsigned long long)seg.fIOVMAddr,
              (unsigned)padded,
              nvGspStageResultName(addrOk));
        releaseGspFirmware();
        return false;
    }

    nvGspStageFillImage(outImage,
                        (unsigned long long)seg.fIOVMAddr,
                        (NVU32)padded,
                        0);

    NVLOG("GSP firmware: staged %u bytes (bootloader %u + %u pad) at phys "
          "0x%llx -- IMEM offset 0",
          (unsigned)padded, (unsigned)blSize,
          (unsigned)(padded - blSize),
          (unsigned long long)seg.fIOVMAddr);

    return true;
}

//
// Tear down the staging buffer. Idempotent by construction: every pointer is
// nulled as it is released, so loadGspFirmware()'s failure paths, stop(), and
// free() can all call this in any order and any number of times.
//
// Order matters. The IODMACommand must let go of the descriptor BEFORE the
// descriptor is completed and released, or the mapping is torn down under an
// object that still references it.
//
void NV3050Probe::releaseGspFirmware(void)
{
    if (fGspFwDma) {
        //
        // Guarded on whether a descriptor was ever TAKEN, not on whether the
        // command object exists. `if (fGspFwDma)` was already true here on the
        // path where setMemoryDescriptor() returned non-success, so it guarded
        // nothing that mattered. Cleared BEFORE the call so a second entry into
        // this function is a no-op rather than a second release.
        //
        if (fGspFwDmaHasDescriptor) {
            fGspFwDmaHasDescriptor = false;
            fGspFwDma->clearMemoryDescriptor(true);
        }
        OSSafeReleaseNULL(fGspFwDma);
    }

    if (fGspFwBuffer) {
        //
        // Guarded rather than unconditional. loadGspFirmware() calls this from
        // the path where prepare() itself returned non-success, and a
        // complete() with no successful prepare behind it unbalances the
        // descriptor's wire count. The flag is cleared BEFORE the release so a
        // second call to this function is a no-op rather than a second
        // complete() on the same preparation.
        //
        if (fGspFwPrepared) {
            fGspFwPrepared = false;
            fGspFwBuffer->complete(kIODirectionOut);
        }
        OSSafeReleaseNULL(fGspFwBuffer);
    }
}
