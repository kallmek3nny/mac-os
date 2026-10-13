# Tiger Lake hardware execution backend

`IntelGtBringup.cpp` is a separate, opt-in hardware writer for PCI 8086:9a68. The existing read-only probes are unchanged. It is included in the Makefile and has its own IOKit personality.

## Implemented

- BAR0 uncached mapping, exclusive PCI open and refusal to take over an existing accelerator or configured BCS0.
- GT and render forcewake acquisition with bounded acknowledgement polling.
- Owned 128 KiB DMA allocation resolved through IODMACommand, with actual mapped-address alignment, contiguity and 46-bit range checks.
- Private four-level PPGTT using an existing uncached PAT entry, plus GGTT residency in absent entries only. Present display/scratch mappings are not replaced.
- Gen12 XCS register-state layout, HWS page, ring and legacy 64-bit privileged copy-engine descriptor with software context ID at bit 37.
- A real XY_FAST_COPY_BLT batch copying a 32x32 BGRA surface between owned pages, followed by MI_FLUSH_DW GPU fence write and bounded completion polling.
- Completion is published only after the GPU fence and matching destination data. The CPU does not write the completion cookie.
- MMIO write/readback logging. Pre-submission failures restore owned mappings; once submitted, DMA, forcewake and provider ownership remain pinned until reboot because context-save retirement/reset management is not implemented.

## Explicit experimental enablement

The default boot does not execute this backend. After compiling and installing the new kext on macOS, `iggt-bringup=1` acquires/releases forcewake, `iggt-bringup=2` prepares and restores memory mappings, and `iggt-bringup=3` attempts the hardware copy.

This source has not been compiled against the Apple kernel SDK or executed on the laptop. No test suites were run for this change. Existing boot images do not contain it. Do not describe the current image as acceleration-ready. Stage 3 may hang the GPU; use a recoverable development installation, do not sleep/wake or unload the driver after submission, and remove the argument on the next boot if it times out. No global GPU/display reset is attempted.

## Scope remaining

This is a one-shot GPU execution backend, not desktop acceleration. It does not expose IOAccelerator, a user client, a reusable surface/queue API, Metal, OpenGL or WindowServer integration. Context workarounds, CSB retirement, power management, repeated submissions, and the macOS acceleration interface are still required. No claim of working acceleration is justified before hardware execution and those integrations.

## Source basis

The existing vendored Linux v6.12 files in reference/i915 provide the Gen12 XCS register-state layout (`intel_lrc.c`), HWS/state-page relationship and descriptor fields (`intel_lrc.h`), ring/ELSQ offsets (`intel_engine_regs.h`), commands (`intel_gpu_commands.h`), and GGTT window/encoding (`intel_ggtt.c`, `intel_gtt.h`). These are source references, not evidence that this macOS port works.
