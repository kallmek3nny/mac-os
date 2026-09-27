# Phase 4 — GSP bringup specification

**Derived from:** `mmio-boot.log` (357,495 lines, captured 2026-08-06 22:41 UTC),
region `pgsp` (`0x110000`–`0x113FFF`), 206,293 accesses of which 658 are writes.

**Method:** `tools/parse-mmiotrace.py mmio-boot.log --region pgsp --first 0`
produces `pgsp-listing.txt`; `tools/collapse-sequence.py pgsp-listing.txt`
folds consecutive same-register runs into `pgsp-sequence.txt`. Every register
name below was resolved against the NVIDIA GA102 swref headers vendored in
`reference/open-gpu-kernel-modules` — nothing here is guessed.

This document says **what the working Linux driver actually did**, in order.
It is the reimplementation target for `phase0-kext/NV3050Probe.cpp`.

---

## 1. Aperture map

| Range | Symbol | Source |
|---|---|---|
| `0x110000`–`0x113FFF` | `NV_PGSP` | `dev_gsp.h:26` |
| `0x110000` + falcon-relative | `NV_PFALCON_FALCON_*` | `dev_falcon_v4.h` |
| `0x111000` + riscv-relative | `NV_PFALCON2_*` / `NV_PRISCV_RISCV_*` | `dev_falcon_second_pri.h`, `dev_riscv_pri.h` |

`NVFalcon.hpp` already encodes both bases (`NV_PGSP_BASE 0x00110000`,
`NV_FALCON2_GSP_BASE 0x00111000`) and the two helpers `nvGspFalconReg()` /
`nvGspRiscvReg()` compute these offsets, so the kext addresses the same
registers the trace shows.

---

## 2. Previously-unnamed offsets, now resolved

The parser printed these as bare `[PGSP falcon +0xNNN]`. They resolve as:

| Absolute | Relative | Symbol | Header |
|---|---|---|---|
| `0x110C00` | falcon `+0xC00` | `NV_PGSP_QUEUE_HEAD(0)` | `dev_gsp.h` |
| `0x111180` | riscv `+0x180` | `NV_PFALCON2_FALCON_MOD_SEL` | `dev_falcon_second_pri.h` |
| `0x111198` | riscv `+0x198` | `NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID` | `dev_falcon_second_pri.h` |
| `0x11119C` | riscv `+0x19C` | `NV_PFALCON2_FALCON_BROM_ENGIDMASK` | `dev_falcon_second_pri.h` |
| `0x111210` | riscv `+0x210` | `NV_PFALCON2_FALCON_BROM_PARAADDR(0)` | `dev_falcon_second_pri.h` |
| `0x111668` | riscv `+0x668` | `NV_PRISCV_RISCV_BCR_CTRL` | `dev_riscv_pri.h:55` |

`NV_PRISCV_RISCV_BCR_CTRL` is the one that matters most: bit 0 `VALID`,
bit 4 `CORE_SELECT` (0 = falcon, 1 = RISC-V), bit 8 `BRFETCH`. The observed
value `0x111` sets all three — VALID | CORE_SELECT=RISCV | BRFETCH — which is
the explicit "boot this engine as RISC-V, fetch the boot record" command.

---

## 3. The observed sequence

Times are seconds from trace start. `xN` is a collapsed run.

### 3.1 Probe (t ≈ 0.9207)

```
R  0x110100  0x00000010   NV_PGSP_FALCON_CPUCTL      -> HALTED=1, core is stopped
R  0x1100F4  0x000047F7   NV_PFALCON_FALCON_HWCFG2   -> RISCV bit 10 = 1, scrubbing bit 12 = 1
```

`0x47F7` decodes as RISC-V-capable. `nvFalconIsRiscvCapable()` in
`NVFalcon.hpp` returns true for this value — that path is already correct.

### 3.2 Engine reset (t ≈ 0.9210)

```
W  0x1103C0  0x00000001   NV_PGSP_FALCON_ENGINE  RESET = TRUE
W  0x1103C0  0x00000000   NV_PGSP_FALCON_ENGINE  RESET = FALSE
```

A write-1 / write-0 pulse, ~21 µs apart. Not a self-clearing bit — the driver
must deassert explicitly. Reset is asserted **before** any DMA setup.

### 3.3 Bootloader DMA into IMEM (t ≈ 0.9212 – 0.9228)

```
W  0x110110  0x00FFEEE2   NV_PGSP_FALCON_DMATRFBASE   physical base >> 8
W  0x110128  0x00000000   NV_PGSP_FALCON_DMATRFBASE1  high bits
```

then the transfer loop:

```
W  0x110114  0x0000NNNN   NV_PGSP_FALCON_DMATRFMOFFS  (falcon offset, +0x100/iter)
W  0x11011C  0x0000NNNN   NV_PGSP_FALCON_DMATRFFBOFFS (memory offset, tracks MOFFS exactly)
W  0x110118  0x00000614   NV_PGSP_FALCON_DMATRFCMD    <- the trigger
```

`DMATRFCMD = 0x614` decodes against the field definitions in `NVFalcon.hpp`:

| Field | Bits | Value | Meaning |
|---|---|---|---|
| `IMEM` | 4:4 | 1 | destination is IMEM, not DMEM |
| `WRITE` | 5:5 | 0 | direction is FB → falcon |
| `SIZE` | 10:8 | 6 | `SIZE_256B` |
| `CTXDMA` | 14:12 | 0 | context DMA index 0 |

So: 256-byte reads from framebuffer into IMEM, 174 iterations ≈ 44 KiB of
bootloader. `MOFFS` and `FBOFFS` advance in lockstep by `0x100`, confirming a
flat 1:1 copy with no scatter.

**Ordering note that will bite an implementer — and did bite this document.**
An earlier revision of §3.3 claimed the opposite: that `DMATRFCMD` is written
*before* the offsets and is configuration rather than a trigger. That was
wrong, and it is worth recording why the mistake was easy.

The raw trace prints

```
118, 114, 11C, 118, 114, 11C, ...
```

which reads equally well as `(CMD)(offsets)` or `(offsets)(CMD)`. Two pieces
of evidence settle it, both obtained after the fact:

1. `s_dmaTransfer_GA102` in the loaded `nvidia.ko` (595.84, open kernel module,
   disassembled on the laptop) writes `+0x114`, then `+0x11C`, then `+0x118`
   inside the loop body, then tests the loop condition. **CMD is last.**
2. The trace's final three DMA writes are `114, 11C, 118` — it *ends* on a
   CMD, which only happens if CMD closes each iteration. The leading `118` is
   a pre-loop priming write, which is why there are 173 CMD writes against 174
   FBOFFS writes.

The practical consequence of the wrong model was an off-by-one: each offset
pair got attributed to the *previous* iteration's CMD, shifting the IMEM/DMEM
split by one transfer. `tools/dma-summary.py` implements the corrected model
and counts a transfer on each CMD write.

`MOFFS` and `FBOFFS` are written with **identical values** on every iteration
(the measured `FBOFFS - MOFFS` delta is `0x0` across all segments), confirming
a flat 1:1 copy with no scatter.

### 3.4 Boot ROM parameters (t ≈ 0.9228)

```
W  0x111210  0x000005A4   NV_PFALCON2_FALCON_BROM_PARAADDR(0)
W  0x11119C  0x00000400   NV_PFALCON2_FALCON_BROM_ENGIDMASK
W  0x111198  0x00000009   NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID
W  0x111180  0x00000001   NV_PFALCON2_FALCON_MOD_SEL
```

Four writes, ~4 µs total, in this exact order. `MOD_SEL = 1` selects the
signed-ucode boot mode and must come **last** — it is the latch for the three
parameters above it.

`ENGIDMASK = 0x400` is bit 10, and `CURR_UCODE_ID = 9` identifies the GSP
booter ucode. These two values are what the boot ROM checks the signature
against; they are not free parameters.

### 3.5 RISC-V core start (t ≈ 1.1195)

```
W  0x111668  0x00000111   NV_PRISCV_RISCV_BCR_CTRL
```

`0x111` = `VALID | CORE_SELECT_RISCV | BRFETCH`. This is the actual "go".
Note the ~197 ms gap between the BROM parameter writes and this — the driver
waited on something in between (see §4).

### 3.6 Steady state (t ≈ 1.84 onward)

```
W  0x111668  0x00000000   NV_PRISCV_RISCV_BCR_CTRL   BCR cleared
W  0x111210  0x00001F10   NV_PFALCON2_FALCON_BROM_PARAADDR(0)
W  0x110C00  0x00000000   NV_PGSP_QUEUE_HEAD(0)      x~94, ongoing
```

Once GSP is running, `0x110C00` becomes the command-queue doorbell. The 94
writes are message submissions; the 200,000+ reads in this region are the
RM↔GSP message loop polling for responses. **That polling is not part of
bringup** — it is the runtime protocol and belongs to Phase 5.

---

## 4. The gaps, and what they mean

| Gap | Duration | What the driver was doing |
|---|---|---|
| 0.9228 → 1.1195 | ~197 ms | waiting between BROM setup and core start |
| 1.1195 → 1.8407 | ~721 ms | GSP firmware booting; driver polling elsewhere |

Neither gap is a fixed delay — a real implementation polls.

**The WPR2 hypothesis was wrong, and the trace disproves it.** An earlier draft
of this document guessed the 197 ms gap was a WPR2 setup handshake. Counting
every access in that window says otherwise:

| Register | Accesses in the gap |
|---|---|
| `0x110100` `NV_PGSP_FALCON_CPUCTL` | 37,328 reads, all returning `0x0` |
| `0x1103C0` `FALCON_ENGINE` | 24 |
| `0x1100F4` `HWCFG2` | 13 |
| everything else | 14 |

The gap is one busy-poll on `CPUCTL`, nothing more. `NV_PFB_PRI_MMU_WPR2_ADDR_HI`
(`0x1FA828`) is read **exactly once in the entire 14.6 s capture**, at t=0.9208
during the probe phase, and it returns `0x0`. WPR2 is not part of this bringup.

That matters for implementation: the poll is on `CPUCTL`, so a kext replaying
this sequence has a real exit condition it can wait on, not an opaque delay it
has to guess at.

---

## 4a. SEC2 — the second falcon

This engine was invisible to the project until the parser's region table was
corrected. `tools/parse-mmiotrace.py` had `psec` at `0x114000` (immediately
above PGSP, which is where you would guess it lives); `dev_sec_pri.h:27` says
`NV_PSEC` is `0x843FFF:0x840000`. The consequence of the guess was that
**25,003 accesses — an entire second falcon boot — were filed as "unmapped"**
and never analysed. Fixed in the region table and asserted in `test_falcon.cpp`
so it cannot be reintroduced silently.

### Ordering: GSP first, then SEC2

| t (s) | Event |
|---|---|
| 1.119477 | GSP `BCR_CTRL` ← `0x111` (start RISC-V) |
| 1.119648 | SEC2 `FALCON_ENGINE` reset pulse 1 → 0, **171 µs later** |
| 1.119695 | SEC2 DMA base programmed, IMEM load begins |
| 1.840748 | GSP `BCR_CTRL` ← `0x0` (cleared, GSP up) |
| 1.842825 | SEC2 `CPUCTL_ALIAS` ← `0x2` (`STARTCPU`), **2 ms after** |

The dependency runs **GSP → SEC2**, not the reverse. A Phase 4 implementation
may bring up GSP alone; it simply will not have a booted SEC2 afterwards.

### The SEC2 control writes, in order

```
0x8403C0  0x1 -> 0x0   FALCON_ENGINE     reset pulse
0x841668  0x0          BCR_CTRL          cleared before use
0x840084  0xB77000A1   FALCON_RM         scratch; the value is the PMC boot ID
0x840624  0x00000190   FBIF_CTL          ALLOW_PHYS_NO_CTX (bit 7)
0x84010C  0x00000000   FALCON_DMACTL     idle
0x840600  0x00000115   FBIF_TRANSCFG(0)  TARGET=COHERENT_SYSMEM, MEM_TYPE=PHYSICAL
0x840110  0x00FFED00   DMATRFBASE
0x840128  0x00000000   DMATRFBASE1
   ... 63 x 256 B IMEM transfers ...
0x840110  0x00FFED8A   DMATRFBASE        rebased for the DMEM pass
0x840128  0x00000000   DMATRFBASE1
   ... DMEM transfers ...
0x840130  0x00000002   CPUCTL_ALIAS      STARTCPU
```

### Why the FBIF block is the most valuable find here

`FBIF_TRANSCFG(0) = 0x115` sets `TARGET = COHERENT_SYSMEM` and
`MEM_TYPE = PHYSICAL`; `FBIF_CTL = 0x190` sets `ALLOW_PHYS_NO_CTX`. Together
they tell the falcon to accept a **raw physical address** for DMA instead of
requiring a context DMA object.

That is directly relevant to the Phase 5 boundary named in §5. The reason
steps 3–4 of the GSP sequence looked unimplementable is that constructing a
context DMA needs a framebuffer allocator the kext does not have. This
sequence shows the hardware does not require one — `IOMemoryDescriptor` +
`getPhysicalAddress()` is sufficient, provided FBIF is programmed first.

The firmware-image problem in §6 is unchanged. This lowers one wall, not both.

Header note: `dev_fbif_v4.h` spells these relative to the FBIF block. The trace
pins the block itself to engine-relative `+0x600`, because `0x840624 - 0x840600`
is `0x24`, which is `FBIF_CTL`'s documented offset within FBIF.

### SEC2 `HWCFG2`

Read `0x77F7` before its reset and `0x67F7` after. Both have bit 10 set, so
SEC2 is RISC-V-capable too — but it is started through `CPUCTL_ALIAS.STARTCPU`
(the Falcon path), not through `BCR_CTRL`. Capability is not the same as the
boot mode actually used.

---

## 4b. Which firmware image was being loaded

The laptop still has the exact driver that produced this capture
(`nvidia-595-open`, module `nvidia.ko`, 19 MB), so the transfer sizes can be
checked against the firmware the driver actually ships rather than guessed at.

**The `/lib/firmware/nvidia/ga102/gsp/*.bin.zst` blobs are the wrong ones.**
Those belong to nouveau. The loaded driver is the NVIDIA open kernel module,
which carries its ucode *embedded in the module* as `bindata` archives —
raw-deflate payloads reachable through `kgspBinArchive*_GA102_*` symbols. An
earlier size match against `booter_unload-570.144` (40,192 B vs an observed
40,192 B) was coincidence and is discarded.

Inflating the real payloads:

| bindata symbol | compressed | inflated |
|---|---|---|
| `GspRmBoot_GA102 UCODE_IMAGE_PROD` | 17,934 | 24,576 |
| `BooterLoadUcode_GA102 IMAGE_PROD` | 34,775 | 60,416 |
| `BooterUnloadUcode_GA102 IMAGE_PROD` | — | 40,192 |

### The one clean match

| Observed | Bytes | Matches |
|---|---|---|
| SEC2 IMEM span | 35,072 | **exact** — `BooterLoad` header field `[6] = 0x8900` |
| GSP seg0 IMEM span | 49,152 | 2 × the inflated `GspRmBoot` image (24,576) |
| GSP seg1 DMEM span | 2,048 | exact — `GspRmBoot` desc `[9]`/`[10] = 0x0800` |
| GSP seg2 IMEM span | 16,384 | no match |
| GSP seg3 DMEM span | 2,560 | no match |
| SEC2 DMEM span | 23,296 | no match |

So **SEC2 is running the GSP booter**, which is what its name has said all
along and is now confirmed from two independent directions: the engine identity
from `dev_sec_pri.h`, and the image size from the module's own bindata.

The GSP-side matches are weaker and should not be over-read. `2×` is not a
match; the DMEM `0x0800` agreement is a single round number and could be
coincidence. Do not treat the GSP rows as identified.

### Why the GSP figures cannot be tightened from this capture

Because **the capture lost records.** See §4c.

---

## 4c. The capture is lossy, and the totals are floors

This was missed on the first pass and it invalidates any exact byte accounting.

`MOFFS` advances by exactly one transfer size per iteration, so within a
segment the offsets should form a contiguous run. They do not:

| Engine | Gap | Blocks lost | Corrupt lines in that interval |
|---|---|---|---|
| GSP | `0x2C00`–`0x4600` | 25 | 1 |
| GSP | `0x6F00`–`0x8900` | 25 | 1 |
| GSP | `0xA100`–`0xBB00` | 25 | 1 |
| SEC2 | `0x300`–`0x1E00` | 26 | 1 |
| SEC2 | `0x3600`–… | 26 | 1 |

Three independent lines of evidence say this is the tracer, not the driver:

1. **Every gap coincides with a corrupt line.** Each interval contains exactly
   one spliced record — a line where two mmiotrace entries were written over
   each other (`W 4 60.958803W 4 60.958933 36 0x8111011c ...`).
2. **The pattern repeats on two unrelated engines.** GSP and SEC2 load 720 ms
   apart and share nothing but the tracer, yet both lose runs of 25–26 blocks
   at regular intervals.
3. **A driver would not do this.** Skipping 6.4 KiB out of the middle of a
   signed image, three times, at even spacing, would fail signature
   verification. The firmware booted.

The capture also contains 22,756 `UNKNOWN`-opcode lines and 608 spliced
records overall — 23,555 damaged lines out of 357,495. This is a ring-buffer
overrun: `mmiotrace` could not drain fast enough during the DMA bursts.

**Consequences for anyone using these numbers:**

- Transfer *counts* and *byte totals* are **floors**, not measurements.
- The *offset spans* are the better estimate of image size, since the first and
  last transfer of each segment did survive.
- `tools/dma-summary.py` now reports gaps explicitly and refuses to present the
  totals as complete.
- If exact sizes ever matter, re-capture with a larger trace buffer
  (`buffer_size_kb`) or accept the spans.

---

## 5. What the kext must implement

In order, as a state machine. Each step has an observable exit condition:

1. Read `CPUCTL`, assert `HALTED`. Read `HWCFG2`, assert RISC-V capable and
   scrubbing complete (`nvFalconScrubbingDone()`).
2. Pulse `FALCON_ENGINE` reset 1 → 0. Re-poll `HWCFG2` scrubbing.
3. Program `DMATRFBASE` / `DMATRFBASE1` from the bootloader's physical address.
4. Loop the 256 B IMEM transfers; between iterations poll `DMATRFCMD.IDLE`
   (`nvFalconDmaIdle()`) rather than free-running as the trace does.
5. Write the four BROM registers in the order of §3.4, `MOD_SEL` last.
6. Write `BCR_CTRL = 0x111`.
7. Poll `NV_PRISCV_RISCV_CPUCTL.ACTIVE_STAT` (`nvRiscvIsActive()`) until set,
   with a timeout. On timeout, read `PRIV_ERR_STAT` / `HUB_ERR_STAT`
   (`nvRiscvHasError()`) and report.

Steps 1, 2, 5, 6 and 7 are decode-complete in `NVFalcon.hpp` today. Steps 3
and 4 need a DMA descriptor the kext cannot construct without a framebuffer
allocator — that is the Phase 5 boundary, and it is a real one.

---

## 6. Open items

- ~~**WPR2 handshake unconfirmed.**~~ **Closed.** See §4 — the gap is a `CPUCTL`
  poll, and WPR2 is read once, returning zero. The hypothesis was wrong.
- **`0xBB0000` is unidentified.** 702 accesses, read as a `0x80`/`0x84` pair in
  a tight loop; the low word advances monotonically while the high word stays
  `0x18C9588E`. That is a 64-bit free-running counter read low-then-high, i.e.
  a timer. No GA102 header in `reference/` defines this aperture, so the
  parser labels it `ptimer2` and the name is **inferred from behaviour, not
  confirmed**.
- **Signed firmware is not in hand.** Steps 3–4 move a signed GSP booter image
  the boot ROM will verify. Nothing in this project can produce or substitute
  that image; the Linux driver loads it from `/lib/firmware`.
- **This still produces no picture.** Per `HANDOFF.md` §1, the GA107 in this
  laptop is PCI class `0302` with no display engine wired out. Booting GSP is
  a correctness milestone, not a path to a framebuffer.
