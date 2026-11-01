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

## Remaining acceleration work

Native SDK compile/link fixes; real Tiger Lake execution; relevant context/engine workarounds and CSB errata validation; proper retirement/reset/power management; scalable GPU-resident surface allocation; native macOS acceleration interfaces and userspace driver/compiler support. Staged small blits are not a performance claim and do not accelerate WindowServer, Metal or OpenGL.

## Source basis

Existing vendored Linux v6.12 intel_lrc.c/h, intel_lrc_reg.h, intel_engine_regs.h, intel_gpu_commands.h, intel_execlists_submission.c, intel_ggtt.c and intel_gtt.h provide register/state/descriptor/command/CSB references. Those references do not prove that this macOS port executes correctly.
