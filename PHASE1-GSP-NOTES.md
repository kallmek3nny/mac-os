# Phase 4 reference — the GSP boot sequence, as NVIDIA actually implements it

Everything here is transcribed from `NVIDIA/open-gpu-kernel-modules` v610.57.04,
MIT-licensed, with file and line references so you can read the original rather
than trust this summary. Written now because Phase 4 is the phase that kills
attempts, and knowing its real shape changes how you plan Phases 2–3.

Primary source: `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c`

The `_TU102` suffix is not a mistake. NVIDIA's HAL routes GA10x (including
GA107) to the Turing implementation for GSP bootstrap — the sequence is shared.
That is good news: it means one code path covers Turing and Ampere.

---

## 0. The thing that reframes the whole project

The GPU **boots itself**. There is a signed firmware stack — VBIOS → GFW →
FWSEC → Booter → GSP-RM — that comes up largely on its own, and the host driver's
job is closer to *negotiating with an already-running system* than to bringing
dead silicon to life.

On Ampere, GSP-RM is a **RISC-V** core running NVIDIA's own resource manager as
firmware. Once it is up, most of what the host driver does is send RPCs to it.
That is why the kernel half of this project is attemptable at all: you are not
reimplementing the GPU's management logic, you are implementing the client side
of a message protocol.

It also means the driver cannot skip steps or invent its own order. The firmware
handshake is signature-checked at multiple points.

---

## 1. `kgspPrepareForBootstrap_TU102` — line 425

Pre-flight, before any firmware is pushed:

1. **Wait for GFW boot to complete** (`kgspWaitForGfwBootOk_HAL`, line 1184).
   GFW is the VBIOS-resident firmware that runs before the host is allowed to
   touch the GPU. You poll a scratch register until it reports done.

2. **Check WPR2 is down** (`kgspIsWpr2Up_TU102`, line 1171). WPR2 is a
   write-protected VRAM region. If it is already up, a previous boot left the
   GPU in a state that needs a reset — you cannot proceed.

3. Prepare the FWSEC command that will later establish FRTS.

### The GFW boot poll — your real Phase 3.5 milestone

From `ampere/ga102/dev_gc6_island_addendum.h`:

```c
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT \
            NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(0)
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT_PROGRESS            7:0
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT_PROGRESS_COMPLETED  0xFF
```

So: read `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(0)`, mask bits 7:0, and wait for
`0xFF`.

This is worth adding to the Phase 3 kext as soon as BAR0 reads work, because it
is **the second read-only milestone in the whole project** and it tells you
something BOOT_0 cannot: that the GPU's own firmware came up healthy under
macOS's PCI initialisation. Still zero writes. Still cannot break anything.

A related freebie in the same header:

```c
#define NV_USABLE_FB_SIZE_IN_MB  NV_PGC6_AON_SECURE_SCRATCH_GROUP_42
```

Reading that gives you VRAM size in MB — another pure read that proves you are
talking to live, initialised silicon.

---

## 2. `kgspBootstrap_TU102` — line 522

The actual boot, in order. Each numbered step is a hard dependency on the last.

1. **Scrubber ucode** (line 536) — clears VRAM if needed.

2. **FWSEC → FRTS** (lines 546–557). FWSEC is signed VBIOS-resident firmware.
   Running it establishes FRTS (Firmware Runtime Services) in a
   write-protected region. `kflcnReset_HAL` first, then `kgspExecuteFwsec_HAL`.

3. **Reset the Falcon into RISC-V mode** (line 559, `kflcnResetIntoRiscv_HAL`).
   The GSP engine can run as either a Falcon or a RISC-V core; Ampere GSP-RM
   needs RISC-V.

4. **Program LIBOS boot args into mailbox registers** (line 562). This is how
   you hand the firmware the physical address of your argument structure — the
   mailbox registers are the only channel available before any queue exists.

5. **Booter Load** (line 566, `kgspExecuteBooterLoad_HAL`). Booter is a
   *second* signed ucode whose job is to authenticate and load GSP-RM itself.
   You cannot load GSP-RM directly; Booter does it.

6. **Init RPCs** (line 578, `kgspSendInitRpcs`) — sent *before* the driver's
   GPU object even exists.

7. **Program `NV_PFALCON_FALCON_OS`** with the ucode's app version (line 588).

8. **Confirm RISC-V is active** (line 592, `kflcnIsRiscvActive_HAL`). If this
   fails you get `NV_ERR_NOT_READY` and the boot is over.

9. **Link the status queue** (line 611, `GspStatusQueueInit`) — shared-memory
   ring buffers in system memory for host ↔ GSP messaging.

10. **Wait for RM init done** (line 614, `kgspWaitForRmInitDone`).

At that point GSP-RM is running and the RPC channel is live.

---

## 3. What this means for the macOS port — the honest read

### Genuinely helped by the open source

The *sequence* is documented, the register definitions are published, and the
error paths tell you what failure looks like at each step. You are not
reverse-engineering the order of operations. That is the single biggest reason
Phase 4 moved from "fantasy" to "hard but real".

### The signed-firmware problem

Steps 2 and 5 execute **signed** ucode: FWSEC from the VBIOS, and Booter plus
GSP-RM from NVIDIA's redistributable firmware package. You cannot modify them
and you cannot substitute your own. Concretely:

- FWSEC is read out of the laptop's own VBIOS — available, not a blocker.
- **Booter and GSP-RM firmware blobs ship with NVIDIA's Linux driver package,**
  under a redistribution licence that permits use with NVIDIA hardware. You
  extract them from the `.run` installer. Also not a blocker, but note the
  blobs are **version-locked to the driver**: the GSP-RM image and the kernel
  driver must be from the same release, because the RPC structures change.
  This is why `open-gpu-kernel-modules` is tagged per driver version.

So the firmware is obtainable. What you're doing is porting NVIDIA's host-side
client to IOKit.

### What actually has to be rewritten for macOS

| NVIDIA / Linux primitive | macOS / IOKit equivalent | Difficulty |
|---|---|---|
| `GPU_REG_RD32` / `WR32` | `IOMemoryMap` + `OSReadLittleInt32` | Trivial — already done in the Phase 3 kext |
| `memdesc*` (memory descriptors) | `IOBufferMemoryDescriptor`, `IODMACommand` | Moderate; the abstractions map reasonably |
| DMA address translation | `IODMACommand` with a 40-bit address filter | Moderate; NVIDIA's own address-width constraints apply |
| `portMem*` allocators | `IOMalloc` / `IOFree` | Trivial |
| Linux `nv-pci.c` attach | `IOPCIDevice` (already in the skeleton) | Done |
| `nvoc` generated object model | Hand-written C++, or port the generator | **Large.** This is the bulk of the work |
| Radix-3 page tables for firmware images | Manual construction over IOKit memory | Substantial |

The `nvoc` object model is the honest bulk of it. NVIDIA generates thousands of
lines of C from an internal object-orientation preprocessor. You either port a
subset by hand or write a translator.

---

## 4. Revised phase estimates, now grounded in source

These replace the guesses in `PLAN.md` §3.

| Phase | Was | Now | Why it changed |
|---|---|---|---|
| 3 — register reads | 1–3 weeks | **Days** | It is one `IOMemoryMap` and a masked read. The skeleton already does it. |
| **3.5 — GFW boot poll + VRAM size** | not planned | **Days** | New. Pure reads, high signal, proves firmware health under macOS. |
| 4 — GSP firmware boot | 1–6 months | **3–12 months** | Revised *up*. The sequence is documented but there are ~10 dependent steps, two signed-ucode executions, and a shared-memory queue protocol. |
| 5 — memory + submission | 3–12 months | **6–18 months** | Revised up. Radix-3 page tables and the RPC layer are more than "allocate and DMA". |

Phase 3.5 is the addition worth acting on. It is cheap, safe, and it is the last
milestone reachable without writing a single register.

---

## 5. Next concrete step

Extend the Phase 3 kext to poll `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(0)` for
`GFW_BOOT_PROGRESS == 0xFF` and read `NV_USABLE_FB_SIZE_IN_MB`. Both are reads.
That lands Phase 3.5 in the same load as Phase 3, and gives four independent
confirmations that the card is alive and healthy before anything is ever
written.

I need the `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05` and `_GROUP_42` base offsets
from `dev_gc6_island.h` to do that — next step is reading them out of the
header.
