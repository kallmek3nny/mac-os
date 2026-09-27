# Ampere GSP bringup on macOS

**What this is:** a register-level account of how NVIDIA's Ampere GSP (GPU System
Processor) is brought out of reset and started as a RISC-V core, derived from an
mmiotrace of the working Linux driver, plus the complete list of traps that cost
real time to find.

**Who it is for:** anyone trying to write a macOS driver for a desktop Ampere
card — 3060, 3070, 3080, 3090. On Turing and later, nothing meaningful happens on
the GPU until GSP is up, and there is no public account of that sequence for
macOS. This is the hardest, least-documented layer, and it is the same silicon
behaviour on every GA10x part.

**What it is not:** a working driver, and not a path to a picture on a screen.
Read §1 before anything else.

**Status:** the register sequence is measured, not inferred. The kext that
replays it has been through a C++ front end and **has never been linked and
never been loaded** — see §7.

---

## 1. Scope, stated honestly and up front

The machine this work was done on is a muxless laptop with a **GA107M**
(`10de:25a2`). On *that* machine, hardware-accelerated macOS is impossible, and
not for effort reasons. Three independent blockers, any one of them fatal:

| Blocker | Evidence |
|---|---|
| The GPU has no display outputs | `lspci -nn` reports PCI class `0302` (3D controller), not `0300` (VGA). `vgaswitcheroo` absent. `xrandr --listproviders` → `Providers: number : 0` |
| macOS has no Tiger Lake iGPU driver | Apple never shipped an 11th-gen Intel Mac |
| Metal needs a private ABI plus an IR→SASS compiler | Both undocumented, no public foothold |

The discrete GPU renders into a buffer the Intel iGPU scans out. macOS has no
Optimus/PRIME concept and no driver for the chip that would do the scanning out,
so **neither chip can put a picture on that laptop's panel**, independent of
driver quality.

**Why this document is still useful to you:** the GSP boot sequence is silicon
behaviour. It does not touch display wiring, display engines, or the mux at all.
Everything in §3–§6 applies unchanged to a desktop Ampere card in a Hackintosh
build — where the display-wiring problem does not exist, the card has real
outputs, and there is currently no driver whatsoever.

Booting GSP is a **correctness milestone**. It is a prerequisite for everything
above it and it is worth nothing by itself.

---

## 2. Safety envelope — non-negotiable

**Never write flash. Never write fuses.** Nothing in this document requires
either, and nothing that follows from it should.

Everything here is MMIO reads and writes to the GSP/SEC2 register apertures
through BAR0, plus DMA out of host memory into falcon IMEM/DMEM. All of it is
lost on power cycle. That property is the whole reason this is safe to iterate
on, and it is the property you give up the moment you touch a non-volatile
surface.

Specifically:

- Do not write the VBIOS. Dump it read-only if you need it (see §6).
- Do not touch `rom_enable` for anything but a read.
- Do not attempt to defeat, patch, or substitute a signature check on a
  non-volatile surface.
- A bricked card is unrecoverable without external hardware, and "I followed a
  writeup" is not a recovery path.

If you are reproducing this on your own card, this envelope is now yours.

---

## 3. The measured bringup sequence

**Source:** `mmio-boot.log`, 357,495 lines, captured 2026-08-06 from a working
`nvidia-595-open` (595.84) driver on a GA107. Region `pgsp`
(`0x110000`–`0x113FFF`), 206,293 accesses of which 658 are writes. Every register
name was resolved against the NVIDIA GA102 `swref` headers in
`open-gpu-kernel-modules` — nothing below is guessed.

Times are seconds from trace start.

### 3.0 Aperture map

| Range | Symbol | Header |
|---|---|---|
| `0x110000`–`0x113FFF` | `NV_PGSP` | `dev_gsp.h:26` |
| `0x110000` + falcon-relative | `NV_PFALCON_FALCON_*` | `dev_falcon_v4.h` |
| `0x111000` + riscv-relative | `NV_PFALCON2_*` / `NV_PRISCV_RISCV_*` | `dev_falcon_second_pri.h`, `dev_riscv_pri.h` |

`NV_PGSP_BASE = 0x00110000` and `NV_FALCON2_GSP_BASE = 0x00111000` are **exactly
one 4 KiB block apart**. See §5 — collapsing them is the single most dangerous
mistake available here.

### 3.1 Probe (t ≈ 0.9207)

```
R  0x110100  0x00000010   NV_PGSP_FALCON_CPUCTL     -> HALTED=1, core stopped
R  0x1100F4  0x000047F7   NV_PFALCON_FALCON_HWCFG2  -> RISCV bit 10 = 1,
                                                       scrubbing bit 12 = 1
```

`0x47F7` decodes as RISC-V-capable with memory scrubbing complete.

### 3.2 Engine reset (t ≈ 0.9210)

```
W  0x1103C0  0x00000001   NV_PGSP_FALCON_ENGINE  RESET = TRUE
W  0x1103C0  0x00000000   NV_PGSP_FALCON_ENGINE  RESET = FALSE
```

A write-1/write-0 pulse ~21 µs apart. **This is not a self-clearing bit** — the
driver must deassert explicitly. Reset is asserted *before* any DMA setup.

### 3.3 Bootloader DMA into IMEM (t ≈ 0.9212 – 0.9228)

Base programming:

```
W  0x110110  0x00FFEEE2   NV_PGSP_FALCON_DMATRFBASE   physical base >> 8
W  0x110128  0x00000000   NV_PGSP_FALCON_DMATRFBASE1  high bits
```

Then the transfer loop, per iteration:

```
W  0x110114  0x0000NNNN   DMATRFMOFFS   falcon offset, +0x100/iter
W  0x11011C  0x0000NNNN   DMATRFFBOFFS  memory offset, tracks MOFFS exactly
W  0x110118  0x00000614   DMATRFCMD     <- the trigger, written LAST
```

`DMATRFCMD = 0x614` decodes as:

| Field | Bits | Value | Meaning |
|---|---|---|---|
| `IMEM` | 4:4 | 1 | destination is IMEM, not DMEM |
| `WRITE` | 5:5 | 0 | direction is FB → falcon |
| `SIZE` | 10:8 | 6 | `SIZE_256B` |
| `CTXDMA` | 14:12 | 0 | context DMA index 0 |

So: 256-byte reads from memory into IMEM, 174 iterations ≈ 44 KiB of bootloader.
`MOFFS` and `FBOFFS` are written with **identical values** every iteration
(measured delta `0x0` across all segments), confirming a flat 1:1 copy with no
scatter.

**Ordering trap — see §5.1.** `DMATRFCMD` is the trigger and is written *last*.
An earlier revision of this work claimed the opposite and was wrong.

### 3.4 Boot ROM parameters (t ≈ 0.9228)

Four writes, ~4 µs total, **in this exact order**:

```
W  0x111210  0x000005A4   NV_PFALCON2_FALCON_BROM_PARAADDR(0)
W  0x11119C  0x00000400   NV_PFALCON2_FALCON_BROM_ENGIDMASK
W  0x111198  0x00000009   NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID
W  0x111180  0x00000001   NV_PFALCON2_FALCON_MOD_SEL
```

`MOD_SEL = 1` selects signed-ucode boot mode and **must come last** — it is the
latch for the three parameters above it.

`ENGIDMASK = 0x400` (bit 10) and `CURR_UCODE_ID = 9` are what the boot ROM checks
the signature against. **They are not free parameters.**

### 3.5 RISC-V core start (t ≈ 1.1195)

```
W  0x111668  0x00000111   NV_PRISCV_RISCV_BCR_CTRL
```

`0x111` = `VALID` (bit 0) | `CORE_SELECT = RISCV` (bit 4) | `BRFETCH` (bit 8).
This is the actual "go": boot this engine as RISC-V, fetch the boot record.

Note the ~197 ms gap before this write. It is **not a delay** — see §4.

### 3.6 Steady state (t ≈ 1.84 onward)

```
W  0x111668  0x00000000   BCR_CTRL cleared
W  0x111210  0x00001F10   BROM_PARAADDR(0)
W  0x110C00  0x00000000   NV_PGSP_QUEUE_HEAD(0)   x~94, ongoing
```

Once GSP runs, `0x110C00` is the command-queue doorbell. The 94 writes are
message submissions; the 200,000+ reads in this region are the RM↔GSP message
loop polling for responses. **That polling is not bringup** — it is the runtime
protocol, and it is out of scope here.

---

## 4. The gaps are polls, not delays

| Gap | Duration | What the driver was doing |
|---|---|---|
| 0.9228 → 1.1195 | ~197 ms | busy-polling `CPUCTL` |
| 1.1195 → 1.8407 | ~721 ms | GSP firmware booting; driver polling elsewhere |

Counting every access in the first window:

| Register | Accesses in the gap |
|---|---|
| `0x110100` `NV_PGSP_FALCON_CPUCTL` | 37,328 reads, **all returning `0x0`** |
| `0x1103C0` `FALCON_ENGINE` | 24 |
| `0x1100F4` `HWCFG2` | 13 |
| everything else | 14 |

The gap is one busy-poll on `CPUCTL`. **Do not implement a fixed sleep here** —
you have a real exit condition to wait on.

**A hypothesis that was wrong, recorded so you do not repeat it:** an earlier
draft guessed this window was a WPR2 setup handshake. It is not.
`NV_PFB_PRI_MMU_WPR2_ADDR_HI` (`0x1FA828`) is read **exactly once in the entire
14.6 s capture**, at t=0.9208 during probe, and it returns `0x0`. WPR2 is not
part of this bringup.

---

## 5. The trap list

This section is the other half of the deliverable. The register sequence above
is reproducible; the traps below are what the sequence costs to *get right*, and
they are not written down anywhere else.

### 5.1 `DMATRFCMD` is the trigger and comes LAST

The raw trace prints `118, 114, 11C, 118, 114, 11C, ...`, which reads equally
well as `(CMD)(offsets)` or `(offsets)(CMD)`. Two independent pieces of evidence
settle it:

1. `s_dmaTransfer_GA102` in the loaded `nvidia.ko` (595.84, disassembled) writes
   `+0x114`, then `+0x11C`, then `+0x118` inside the loop body, then tests the
   loop condition. **CMD is last.**
2. The trace's final three DMA writes are `114, 11C, 118` — it *ends* on a CMD,
   which only happens if CMD closes each iteration. The leading `118` is a
   pre-loop priming write, which is why there are 173 CMD writes against 174
   FBOFFS writes.

**Cost of getting it wrong:** an off-by-one. Each offset pair gets attributed to
the previous iteration's CMD, shifting the IMEM/DMEM split by one transfer.
Nothing errors.

### 5.2 `NV_FALCON2_GSP_BASE` sits exactly one 4 KiB block above `NV_PGSP_BASE`

`0x110000` and `0x111000`. Collapse them — or compute a RISC-V offset from the
Falcon base — and **every RISC-V access lands inside the Falcon register file,
where every offset is a *valid register***.

This does not fault. It does not return garbage. It reads and writes real
hardware at the wrong address, and the symptom is a firmware boot that hangs
with **no diagnostic at all**.

This is the single most dangerous mistake in the whole sequence, precisely
because the failure is silent. Prove your two bases independently before
trusting either:

- `dev_falcon_v4.h` gives `MAILBOX0` as engine-relative `0x40`.
- `dev_gsp.h` independently spells the absolute `0x110040`.
- Those two headers do not reference each other, so their agreement *proves* the
  base rather than assuming it.

### 5.3 The firmware version gap

The firmware blobs in hand are **570.144**. The register constants were
transcribed from **610.57.04** headers.

This is **not** blocking for the probe/reset/DMA milestones — those registers are
stable across the range. It **becomes blocking** at Booter Load and init-RPC,
where the RM↔GSP message structures are version-coupled.

If you are reproducing this: match your header version to your blob version, or
accept that everything past `BCR_CTRL` is unverified.

### 5.4 The booters are heavy-secure signed, with a signature patch point

`booter_load` and `booter_unload` are `nvfw_hs_header_v2` images — heavy-secure
SEC2 ucodes:

```
sig_prod_offset  60     sig_prod_size  768    patch_loc       828
patch_sig        832    meta_data_off  836    meta_data_size  12
num_sig          848    header_offset  852    header_size     36
```

`sig_prod_size = 768` across `num_sig` entries is the production signature block;
`patch_loc` is where the bootloader patches the *selected* signature in.

**A driver cannot load these without performing that signature patch step.**
There is no shortcut, and per §2 there is no attempt to defeat the check.

### 5.5 Two blob types, one `header_offset`, different inner descriptors

`header_offset` is 24 in all three blobs, so a single parser *looks* like it
should work. What sits there differs by type:

| Blob | Inner descriptor |
|---|---|
| `bootloader-570.144.bin` | `RM_RISCV_UCODE_DESC` (`rmRiscvUcode.h`), version 5 |
| `booter_load` / `booter_unload` | `nvfw_hs_header_v2` (heavy-secure) |

Parsing `RM_RISCV_UCODE_DESC` at a fixed offset in all three yields a clean
bootloader and **plausible-looking nonsense** for both booters — e.g.
`bootloaderOffset = 0x0000f000`, pointing past end of file.

Related coincidence that will mislead you: the `0x3c` = 60 word that looks like
`FALCON_UCODE_DESC_V2_SIZE_60` is actually `sig_prod_offset` in the HS header. A
scan for a real `FALCON_UCODE_DESC` V2/V3 header found **none** in the first 256
bytes of any blob — the VBIOS-embedded descriptor format simply is not what
`linux-firmware` ships. That null result was the answer, not a failure.

### 5.6 `bin_size` is legitimately larger than the file on disk

```
                    bin_magic  bin_ver  bin_size  hdr_off  data_off  data_size
bootloader-570.144  0x10de     1        24832     24       108       24576
booter_load         0x10de     1        61440     24       888       60416
booter_unload       0x10de     1        41216     24       888       40192
```

`bootloader` is 24,684 bytes on disk but declares `bin_size = 24832`, because
`bin_size` is the **padded/aligned** length and the trailing pad is not stored. A
parser that range-checks against `bin_size` will flag valid data as corrupt. The
bound that actually matters is `data_offset + data_size`, which fits inside every
file.

### 5.7 SEC2 is not where you would guess, and guessing hid an entire engine

`NV_PSEC` is `0x840000`–`0x843FFF` (`dev_sec_pri.h:27`) — **not** `0x114000`
immediately above PGSP, which is the natural guess.

Cost of the guess: **25,003 accesses — an entire second falcon boot — were filed
as "unmapped" and never analysed.**

### 5.8 The dependency runs GSP → SEC2, not the reverse

| t (s) | Event |
|---|---|
| 1.119477 | GSP `BCR_CTRL` ← `0x111` (start RISC-V) |
| 1.119648 | SEC2 `FALCON_ENGINE` reset pulse 1 → 0, **171 µs later** |
| 1.119695 | SEC2 DMA base programmed, IMEM load begins |
| 1.840748 | GSP `BCR_CTRL` ← `0x0` (GSP up) |
| 1.842825 | SEC2 `CPUCTL_ALIAS` ← `0x2` (`STARTCPU`), **2 ms after** |

You may bring up GSP alone. You simply will not have a booted SEC2 afterwards.

### 5.9 Capability is not the boot mode actually used

SEC2's `HWCFG2` reads `0x77F7` before reset and `0x67F7` after — bit 10 set in
both, so SEC2 *is* RISC-V-capable. It is nonetheless started through
`CPUCTL_ALIAS.STARTCPU` (the Falcon path), **not** through `BCR_CTRL`.

### 5.10 FBIF lets you skip the context-DMA object entirely

This is the most valuable single find in the trace, because it removes a wall
that looked structural.

```
0x840624  0x00000190   FBIF_CTL         ALLOW_PHYS_NO_CTX (bit 7)
0x840600  0x00000115   FBIF_TRANSCFG(0) TARGET=COHERENT_SYSMEM, MEM_TYPE=PHYSICAL
```

Together these tell the falcon to accept a **raw physical address** for DMA
instead of requiring a context DMA object. Constructing a context DMA needs a
framebuffer allocator a bare kext does not have — which is why the DMA steps
looked unimplementable. They are not: `IOMemoryDescriptor` +
`getPhysicalAddress()` is sufficient, **provided FBIF is programmed first**.

Header note: `dev_fbif_v4.h` spells these relative to the FBIF block. The trace
pins the block to engine-relative `+0x600`, because `0x840624 - 0x840600 = 0x24`,
which is `FBIF_CTL`'s documented offset within FBIF.

### 5.11 The capture is lossy — byte totals are floors, not measurements

`MOFFS` advances by exactly one transfer size per iteration, so within a segment
the offsets should be contiguous. They are not:

| Engine | Gap | Blocks lost | Corrupt lines in interval |
|---|---|---|---|
| GSP | `0x2C00`–`0x4600` | 25 | 1 |
| GSP | `0x6F00`–`0x8900` | 25 | 1 |
| GSP | `0xA100`–`0xBB00` | 25 | 1 |
| SEC2 | `0x300`–`0x1E00` | 26 | 1 |
| SEC2 | `0x3600`–… | 26 | 1 |

Three lines of evidence say this is the tracer, not the driver:

1. **Every gap coincides with a corrupt line** — a spliced record where two
   mmiotrace entries were written over each other
   (`W 4 60.958803W 4 60.958933 36 0x8111011c ...`).
2. **The pattern repeats on two unrelated engines.** GSP and SEC2 load 720 ms
   apart and share nothing but the tracer, yet both lose runs of 25–26 blocks at
   regular intervals.
3. **A driver would not do this.** Skipping 6.4 KiB out of the middle of a signed
   image, three times, at even spacing, would fail signature verification. The
   firmware booted.

Overall: 23,555 damaged lines out of 357,495 (22,756 `UNKNOWN`-opcode, 608
spliced) — a ring-buffer overrun; `mmiotrace` could not drain fast enough during
the DMA bursts.

**If you re-capture, raise `buffer_size_kb`.** And treat transfer counts and byte
totals as floors. Offset *spans* are the better size estimate, since the first
and last transfer of each segment did survive.

### 5.12 Only one firmware size match is clean — do not over-read the others

| Observed | Bytes | Verdict |
|---|---|---|
| SEC2 IMEM span | 35,072 | **exact** — `BooterLoad` header field `[6] = 0x8900` |
| GSP seg1 DMEM span | 2,048 | exact — `GspRmBoot` desc `[9]`/`[10] = 0x0800` |
| GSP seg0 IMEM span | 49,152 | 2× the inflated `GspRmBoot` (24,576) — **not a match** |
| GSP seg2 IMEM span | 16,384 | no match |
| GSP seg3 DMEM span | 2,560 | no match |
| SEC2 DMEM span | 23,296 | no match |

So **SEC2 runs the GSP booter**, confirmed from two independent directions: the
engine identity from `dev_sec_pri.h`, and the image size from the module's own
bindata. The GSP rows should **not** be treated as identified — `2×` is not a
match, and a single round number like `0x0800` could be coincidence.

Related: the `/lib/firmware/nvidia/ga102/gsp/*.bin.zst` blobs are **nouveau's**,
not the open kernel module's. The NVIDIA open module carries its ucode *embedded*
as `bindata` archives — raw-deflate payloads reachable through
`kgspBinArchive*_GA102_*` symbols. An early size match against
`booter_unload-570.144` (40,192 B vs an observed 40,192 B) was coincidence and
was discarded.

### 5.13 `0xBB0000` is inferred, not confirmed

702 accesses, read as a `0x80`/`0x84` pair in a tight loop; the low word advances
monotonically while the high word stays `0x18C9588E`. That is a 64-bit
free-running counter read low-then-high — i.e. a timer. **No GA102 header defines
this aperture.** The name is inferred from behaviour. Treat it as unidentified.

---

## 6. Reading the VBIOS — bind the driver first

Not part of bringup, but it blocks anyone trying to reproduce the setup, and the
error it produces points at the wrong cause.

The first two dump attempts returned `dd: IO error` and a 0-byte file. The cause
was **not** power management — `power/control` already read `on` and the device
was in `D0`. It was that **no driver was bound**: with `driver` empty, the kernel
refuses to service reads of the `rom` attribute even when `rom_enable` is set.

```sh
sudo modprobe nvidia          # bind first -- this is the step that matters
sleep 3
echo 1 | sudo tee /sys/bus/pci/devices/0000:01:00.0/rom_enable
sudo dd if=/sys/bus/pci/devices/0000:01:00.0/rom of=vbios.rom bs=65536
echo 0 | sudo tee /sys/bus/pci/devices/0000:01:00.0/rom
```

A valid dump starts `55 aa` and carries a version string. Note that
`PLEASE POWER DOWN AND CONNECT THE PCIe POWER CABLE(S)` and
`BIOS Certificate Check Failed!!!` are **stock NVIDIA error text present in every
image** — they are not signs of a bad dump.

If you blacklist `nvidia` so it does not autoload during a capture, an explicit
`modprobe nvidia` must still work — do not add `install ... /bin/true` lines,
or the ROM becomes unreadable for reasons that look like hardware failure.

---

## 7. What a kext must implement, and where the wall is

As a state machine. Every step has an **observable exit condition** — no step
needs a guessed delay:

1. Read `CPUCTL`, assert `HALTED`. Read `HWCFG2`, assert RISC-V capable and
   scrubbing complete.
2. Pulse `FALCON_ENGINE` reset 1 → 0. Re-poll `HWCFG2` scrubbing.
3. Program `DMATRFBASE` / `DMATRFBASE1` from the bootloader's physical address.
4. Loop the 256 B IMEM transfers; between iterations poll `DMATRFCMD.IDLE`
   rather than free-running as the trace does.
5. Write the four BROM registers in the order of §3.4, `MOD_SEL` **last**.
6. Write `BCR_CTRL = 0x111`.
7. Poll `NV_PRISCV_RISCV_CPUCTL.ACTIVE_STAT` until set, with a timeout. On
   timeout, read `PRIV_ERR_STAT` / `HUB_ERR_STAT` and report.

**Steps 1, 2, 5, 6, 7 are decode-complete.** Steps 3 and 4 need a DMA descriptor
— and per §5.10 that is achievable with `IOMemoryDescriptor` +
`getPhysicalAddress()` once FBIF is programmed, which is *less* of a wall than it
first appeared.

### The wall that is real

**Signed firmware is not substitutable.** Steps 3–4 move a signed GSP booter
image the boot ROM will verify against `ENGIDMASK` and `CURR_UCODE_ID`. Nothing
in this work can produce or replace that image — the Linux driver loads it from
`/lib/firmware`, and it is NVIDIA-signed, redistributed-with-the-driver binary,
not MIT-licensed like `open-gpu-kernel-modules`. **Do not re-publish the blobs.**

### The honest status of the code

The kext has passed a **syntax gate**, not a build:

- 5/5 clang parse checks (including 4/4 negative controls)
- 113/113 register constants verified against NVIDIA's own headers
- 5/5 C++ unit-test suites, 634 checks total (decode 39, falcon 169,
  riscv_ucode 85, gsp_boot 199, gsp_stage 142)

Measured 2026-08-11 by running the gates and each binary. An earlier revision
of this list said `102/102` and `3/3 ... (decode, falcon, riscv_ucode)`, which
predates `NVGspBoot.hpp`/`NVGspStage.hpp` coverage and the two GSP suites.

**It has never been linked and never been loaded.** Constants matching headers
proves transcription; unit tests prove bit extraction. Neither proves the thing
runs. Do not read those numbers as more than they are — that habit is what §5 is
made of.

---

## 8. Method rules, if you are doing this kind of work

These are the generalisable part, and they are cheaper to inherit than to earn.

**Absence is one-directional.** Flat counters, an unchanged file, a static
screenshot — these corroborate, they never prove. A cursor unchanged across six
samples in three seconds is an *idle* cursor, not a blocked one, exactly as an
unchanged framebuffer is not a hung machine.

**An API's success return is not evidence of effect.** `SendInput` returning
`sent=1` means the events were accepted, not delivered. `vmrun
connectNamedDevice` returns exit 0 for a device that does not exist. Assert the
specific expected end-state instead.

**"Something changed" is the weak form of the same error.** A post-condition test
is only as good as its specificity. Assert the *exact* expected end-state, and
give contaminated samples their own outcome rather than folding them into pass or
fail.

**A byte pattern is not a conclusion.** Three `user32` exports beginning `ff 25`
looked like inline hooks; following each pointer showed all three landing in
`win32u.dll` — normal forwarders. Resolve to a named object.

**An UNTESTED hypothesis is not a FALSIFIED one.** This one cost the most. A
discriminating experiment that *could not be run* was silently recorded as one
that came back negative, and the true cause sat on the "falsified" list for hours.
When you cannot run the deciding test, write down **UNTESTED**, say so out loud,
and keep it on the list.

**A plausible-looking wrong offset returns a plausible-looking number.** This is
the project's signature failure mode and the reason §5.2, §5.5 and §5.7 exist.
Hardware does not fault when you read the wrong valid register.

---

## 9. Reproducing this

What you need:

- An Ampere card and a Linux install with a working NVIDIA driver, to capture
  from. The trace here is `nvidia-595-open` 595.84.
- `mmiotrace` with a **large** `buffer_size_kb` — see §5.11.
- NVIDIA's `open-gpu-kernel-modules` for the `swref` headers. Resolve every
  register name against them; do not name a register from behaviour.

A caution on header availability: NVIDIA ships **no `dev_fb.h` under
`ampere/ga102`**. WPR2 offsets have to come from `turing/tu102/dev_fb.h` — the
same implementation NVIDIA's own HAL routes GA10x GSP bootstrap to — and are
corroborated by `hopper/gh100/dev_fb.h`, which carries identical offsets. Two
architectures agreeing is evidence; one header would have been a guess.

A caution on the die: **"RTX 3050" is a marketing name spanning GA107, GA106,
AD106 and AD107 — four dies across two architectures.** Resolve your actual PCI
device ID to a die before assuming anything here applies. Ada parts need
retargeting, not reuse.

---

## 10. TODO

Marked rather than invented. These are genuinely open:

- **Booter Load / init-RPC sequence.** Blocked on the §5.3 version gap and the
  §5.4 signature patch step. Not traced here.
- **RM↔GSP message protocol.** The 200,000+ `QUEUE_HEAD` polling accesses in
  §3.6 are the runtime protocol and were not decoded.
- **`0xBB0000` identity.** See §5.13 — behaviour says timer, no header confirms.
- **GSP-side firmware image identification.** See §5.12 — cannot be tightened
  from a lossy capture.
- **Desktop Ampere validation.** Every register here is from a GA107. The
  sequence should be identical on GA102/GA104, but that is an expectation, not a
  measurement. **Nobody has run this on a card with display outputs.** If you
  do, that result is the most valuable thing anyone can add to this document.
