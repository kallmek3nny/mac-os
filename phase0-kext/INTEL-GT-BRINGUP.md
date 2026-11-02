# Experimental Tiger Lake blitter backend

Target: 8086:9a68. The original probes remain read-only. IntelGtBringup is a separate hardware writer, included in the kext Makefile and registered through its own IOKit personality.

## Code implemented

- GT/render forcewake, exclusive provider ownership, cold-BCS0 checks and refusal to take over an existing accelerator.
- Owned DMA allocation with IODMACommand mapped-address checks; private four-level PPGTT; uncached CPU view; GGTT residency only in absent entries.
- Gen12 XCS register-state stream, privileged 64-bit context descriptor, BCS0 ELSQ submission and GPU-written flushing fence.
- A serialized, reusable `copySurface32` kernel API. Batches are not rewritten until the previous GPU fence and expected Gen12 context-switch-away-to-idle CSB event both arrive. Timeout disables the queue and preserves DMA ownership.
- Bounds-checked command encoder in IntelGtBlitter.cpp. It accepts no userspace commands or physical addresses.
- Administrator-only IntelGtBlitUserClient, pointer-free fixed-size surface ABI in IntelGtBlitABI.h, and an iggt-blit macOS client. The v1 user interface is deliberately limited to 1024 bytes per surface. Kernel staging supports up to 4096 bytes. Neither path offers a CPU fallback.

## Build status

IntelGtBlitter.cpp was cross-compiled with Clang for x86_64-apple-darwin24 using -mkernel -fapple-kext, producing a Mach-O x86-64 object at artifacts/gt-build/IntelGtBlitter.o. This compiles the production command encoder, not a test replacement.

The entire kext and userspace client have NOT been compiled against the Apple SDK. Windows has no xcrun/Kernel.framework. The macOS VMware guest was started. SSH initially timed out and VMware Tools lost its heartbeat; the latest attempt reached SSH but was rejected with Permission denied (publickey,password,keyboard-interactive). The previous guest SSH private key is not present in the current host SSH directory, and no guest credentials are configured in the MCP process. A whole-backend host compilation attempt stopped at the missing pexpert/pexpert.h header. No host shim or Mach-O encoder object substitutes for an Apple-SDK kext build. No test suites were run, no kext was loaded, and no GPU execution was attempted.

Once the macOS build guest is accessible, copy the current phase0-kext sources to a writable directory there. The iggt-blit client is a host tool, so its source is `tools/iggt-blit.cpp`, outside the kext directory; `make gt-blit-tool` reads it from a `tools/` directory beside `phase0-kext/`, so copy that file into a sibling `tools/` directory as well. Then run, inside the copied `phase0-kext/`:

```sh
make
make gt-blit-tool
```

The existing boot images contain older sources and have not been rebuilt for this change.

## Experimental hardware enablement

Only after a successful native build and installation: iggt-bringup=1 acquires/releases forcewake; =2 prepares/restores memory; =3 submits the initial owned-surface GPU copy and enables the runtime interface only if it completes and retires. Without the argument, start declines before PCI open/MMIO mapping/writes.

Example userspace operation after the hardware backend is ready:

```sh
sudo ./build/iggt-blit input.bgra output.bgra 16 16 64
```

That requires a 1024-byte BGRA input. Output is written only after the GPU operation succeeds. It is an application-accessible blit operation, not IOAccelerator or Metal support.

DMA, forcewake and the service remain retained after first submission until reboot. A complete reset/teardown and power-management implementation is absent. Do not sleep/wake or unload after submission. Stage 3 is experimental and may hang the GPU. Use a recoverable development installation and remove the boot argument on the next boot if it fails. No global GPU/display reset is attempted.

## Stage 4: borrowed inherited scanout (t-d0bcef)

Goal: the first BCS0-drawn pixels on the real panel. Before this change the private PPGTT mapped only the 32-page pool and the persistent arena, so no blit could reach the surface IntelFbPresentProbe presents. `iggt-bringup=4` runs stage 3 unchanged and adds one rectangle fill into that surface. Stages 1-3 behave exactly as before; the boot argument now accepts 1-4.

### Source findings

Linux v6.12 i915. Unvendored paths are under `drivers/gpu/drm/i915/` at tag v6.12.

- (a) GGTT PTE layout. On Tiger Lake a GGTT PTE is `addr | GEN8_PAGE_PRESENT` (reference/i915/intel_ggtt.c:277-287, `gen8_ggtt_pte_encode`). PRESENT is bit 0 (reference/i915/intel_gtt.h:152) and the address is `GEN12_GGTT_PTE_ADDR_MASK` = `GENMASK_ULL(45, 12)` (intel_gtt.h:100). `GEN12_GGTT_PTE_LM` (intel_gtt.h:97) marks device-local memory, which integrated TGL does not have. Linux finds the inherited plane's pages the same way: it reads the GGTT PTE for PLANE_SURF and masks it (display/intel_plane_initial.c:54-71). So the scanout pages are found by reading the PTEs at BAR0+0x800000 for PLANE_SURF through PLANE_SURF+extent.
- (b) PPGTT mapping of stolen (DSM) pages: yes. i915 adopts the inherited framebuffer as an object in the stolen region (display/intel_plane_initial.c:76, :175). Stolen scatterlists carry DSM DMA addresses (gem/i915_gem_stolen.c:639). A framebuffer object can be exported as a GEM handle; only userptr objects are refused (display/intel_fb.c:1889-1902). execbuffer binds every looked-up object into the context VM, which is the PPGTT (gem/i915_gem_execbuffer.c:888-925, `i915_vma_instance(obj, eb->context->vm)`). The stolen regions' `private` flag (gem/i915_gem_stolen.c:973, :994) only blocks userspace placement at create time (gem/i915_gem_create.c:314); it does not restrict binding. `gen12_pte_encode` builds the PPGTT PTE from the address, PAT and flags only, with no stolen-memory case (gt/gen8_ppgtt.c:61-84).
- Cache selection. TGL has no eDRAM, so `HAS_WT` is false (i915_drv.h:657-659) and the adopted framebuffer gets `I915_CACHE_NONE` (display/intel_plane_initial.c:189-190). `TGL_CACHELEVEL` maps that to PAT index 3 (i915_pci.c:58), and `tgl_setup_private_ppat` programs PAT 3 as `GEN8_PPAT_UC` (gt/intel_gtt.c:547). The borrowed leaves use `fPatIndex`, the UC entry claimEngine() already selected.

### Code

- IntelGtScanoutMap.hpp is the dependency-free arithmetic: no IOKit types, no MMIO. It is registered in tests/check-kext-syntax.bat. `igGtScanoutPlanMap` turns PLANE_SURF and the extent into a GGTT index run. That run must fit inside `fGGTTEntries` and must not overlap the pool run installGGTT() reserved. `igGtScanoutGgttPage` decodes one read-back PTE and refuses entries that are not present, are LM, or name address 0. `igGtScanoutBuildLeaves` and `igGtScanoutBuildPdEntries` produce the private entries. `igGtScanoutFillRect32` turns a pixel rectangle into a `gtEncodeFill32` destination. It refuses any rectangle that is not wholly inside the visible plane rather than clipping it. The window starts at GPU 0x200000 (pd[1..32]) and holds up to 16384 pages (64 MiB, enough for 3840x2400x4). pd[0] holds the pool, guard and persistent arena and never shares a table with a borrowed page.
- IntelGtScanout.cpp holds the IntelGtBringup members.
  - `mapScanout()` runs at stage 4 only, after the engine claim, PAT discovery, PPGTT allocation and installGGTT(), and before the first submit, so no context has walked pd[1..] yet. It samples pipe A plane 1 once (reads of PLANE_CTL, STRIDE, SIZE and SURF only). It refuses unless `igPresentVerdict()` is `IG_PRESENT_OK`, which also refuses tiled and compressed planes (IntelFbPresent.hpp:172-175). It checks pixel size and fill geometry before allocating anything and requires the pd[1..] slots to be zero. Then it allocates contiguous leaf tables (uncached CPU view, bound through IODMACommand, 46-bit), reads the GGTT PTEs, builds the leaves and installs the PD entries with read-back. Each verdict and state is published as a `GTScanout*` property.
  - `fillScanoutRect32(x, y, w, h, color)` runs under `fQueueLock`. It requires stage 4, a completed and retired self-test, no fault, and an installed mapping. It encodes with `gtEncodeFill32` and submits through `submitBatch()`, the same fence and CSB-retirement wait that fillPersistentSurface32() uses. Callers name pixels, never GPU addresses. It is not exposed through the user client.
- start(). At stage 4 `mapScanout()` runs before `submitCopy()`. If it refuses, nothing is submitted: the driver cleans up like stage 2 and stays registered so `GTScanoutState` can be read. After the self-test copy retires, `drawScanoutProbe()` fills 128x128 at (32,32) with 0xFFFFFFFF and publishes `GTScanoutFillCompleted`. 0xFFFFFFFF is white in both XRGB8888 and XRGB2101010, so the result does not depend on the open PLANE_CTL format decode (t-a7929c).
- cleanup(). `releaseScanout()` runs first, before releasePersistentSurface() and before the PPGTT pages are released. It clears pd[1..] with read-back and frees only the driver-owned tables, never the scanout pages. If a clear cannot be proven, the tables are kept, the driver quarantines (`GTScanoutQuarantined`, `GTBringupScanoutQuarantined`) and DMA is retained.
- Unchanged: there are no display register writes and no GGTT PTE writes (the GGTT is only read), and the IntelGtProbe.cpp write bans are untouched.

### Status

The Visual Studio 2022 clang syntax probe against tests/kextshim gives these results. IntelGtScanout.cpp has 0 errors. IntelGtBringup.cpp has the same 5 errors as the pre-change baseline. IntelGtPersistentSurfaces.cpp has 0 errors. The IntelGtScanoutMap.hpp header check passes. Nothing has been built against the Apple SDK, loaded, or run on hardware. Stage 4 writes visible pixels into the live scanout. It carries the same GPU-hang risk and the same no-sleep and no-unload limits as stage 3.

## Remaining acceleration work

Native SDK compile/link fixes; real Tiger Lake execution; relevant context/engine workarounds and CSB errata validation; proper retirement/reset/power management; scalable GPU-resident surface allocation; native macOS acceleration interfaces and userspace driver/compiler support. Staged small blits are not a performance claim and do not accelerate WindowServer, Metal or OpenGL.

## Source basis

Existing vendored Linux v6.12 intel_lrc.c/h, intel_lrc_reg.h, intel_engine_regs.h, intel_gpu_commands.h, intel_execlists_submission.c, intel_ggtt.c and intel_gtt.h provide register/state/descriptor/command/CSB references. Those references do not prove that this macOS port executes correctly.
