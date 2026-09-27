# RTX 3050 laptop device IDs — which die is actually in your machine

Transcribed from `NVIDIA/open-gpu-kernel-modules` v610.57.04,
`src/nvidia/generated/g_nv_name_released.h`. This is NVIDIA's own shipped
table, so it is authoritative.

**Why this file exists:** "RTX 3050 laptop" is a marketing name covering at
least three different silicon dies across two architectures. The driver work
is completely different depending on which one you have. Do not guess.

---

## The trap

| Marketing name | Device IDs | Die | Architecture |
|---|---|---|---|
| GeForce RTX 3050 Laptop GPU | `0x25A2`, `0x25A5`, `0x25E2`, `0x25E5` | GA107 | **Ampere** (arch `0x17`) |
| GeForce RTX 3050 4GB Laptop GPU | `0x25AB` | GA107 | Ampere |
| GeForce RTX 3050 6GB Laptop GPU | `0x25AC`, `0x25EC` | GA107 | Ampere |
| GeForce RTX 3050 Ti Laptop GPU | `0x25A0`, `0x25E0` | GA107 | Ampere |
| GeForce RTX 3050 Ti Laptop GPU | `0x2523`, `0x2563` | GA106 | Ampere |
| GeForce RTX 3050 (desktop, 8GB) | `0x2507`, `0x2508` | GA106 | Ampere |
| GeForce RTX 3050 (desktop, 6GB) | `0x2582`, `0x2584` | GA107 | Ampere |
| **GeForce RTX 3050 A Laptop GPU** | `0x2822` | **AD106** | **Ada** (arch `0x19`) |
| **GeForce RTX 3050 A Laptop GPU** | `0x28A3`, `0x28E3` | **AD107** | **Ada** (arch `0x19`) |

The last two rows are the ones that matter. The **"3050 A"** parts — sold in
2024+ laptops — are **Ada Lovelace**, not Ampere. Same product name, different
architecture, different GSP firmware, different SASS ISA. Note they are not
even all the same die: `0x2822` is AD106, the other two are AD107. If your
laptop reports one of those IDs, every reference in `PLAN.md` to Ampere/GA107
needs re-targeting, and the register headers to read are
`swref/published/ada/ad102/` rather than `ampere/ga102/`.

### One more naming trap

A substring search for "3050" in NVIDIA's table also returns `0x2237` (an
**A10G**, GA102 datacenter part), `0x26B5`/`0x26B8`/`0x26B9`/`0x26BA`
(**L40 / L40S / L20**, AD102), and `0x2BB5`/`0x2BB9` (**RTX PRO 6000**,
GB202). Those are datacenter GPUs that ship a *vGPU profile* named after a
GeForce card. They are not RTX 3050 hardware. Only `0x25xx` and `0x28xx` IDs
are real laptop 3050 silicon.

An 11th-gen Intel laptop is almost certainly 2021–2022 vintage, which points
to GA107 — but confirm it rather than assuming.

---

## How to confirm

From the Linux live USB on the laptop:

```sh
lspci -nn | grep -i nvidia
```

You want the bracketed pair, e.g. `[10de:25a2]`. The second half is the device
ID to look up in the table above.

---

## Expected register values for GA107

Once the Phase 3 kext reads BAR0, these are the values to expect. Derived from
`src/common/inc/swref/published/nv_ref.h`.

### Field layout

`NV_PMC_BOOT_0` (offset `0x0`):

| Bits | Field | GA107 value |
|---|---|---|
| 28:24 | ARCHITECTURE_0 (low 5 bits) | `0x17` |
| 8:8 | ARCHITECTURE_1 (high bit) | `0` |
| 23:20 | IMPLEMENTATION | `0x7` |
| 7:4 | MAJOR_REVISION | varies by stepping |
| 3:0 | MINOR_REVISION | varies by stepping |

Architecture is **split across two non-adjacent ranges** —
`arch = (bit8 << 5) | bits28:24`. See `decodePmcBoot0Architecture()` in
`nv_ref.h`. Reading only 28:24 happens to give the right answer for Ampere
because bit 8 is zero, but it is wrong in general.

So `NV_PMC_BOOT_0` should read **`0x177000xx`**, where `xx` is the stepping.

`NV_PMC_BOOT_42` (offset `0xA00`) — the modern authoritative identity register,
and what NVIDIA's own HAL routing uses:

| Bits | Field | GA107 value |
|---|---|---|
| 29:24 | ARCHITECTURE | `0x17` |
| 23:20 | IMPLEMENTATION | `0x7` |
| 29:20 | CHIP_ID (composite) | **`0x177`** |
| 19:16 | MAJOR_REVISION | varies |
| 15:12 | MINOR_REVISION | varies |
| 11:8 | MINOR_EXT_REVISION | varies |

### Architecture constants (`nv_ref.h`)

| Arch | Value |
|---|---|
| Turing (TU100) | `0x16` |
| **Ampere (GA100)** | **`0x17`** |
| Hopper (GH100) | `0x18` |
| **Ada (AD100)** | **`0x19`** |
| Blackwell (GB100) | `0x1A` |
| Blackwell 2 (GB200) | `0x1B` |
| Rubin (GR100) | `0x1C` |
| AMODEL (simulator) | `0x1F` |

### Ampere implementation constants (`nv_arch.h`)

| Impl | Value | Composite chip ID |
|---|---|---|
| GA100 | `0x00` | `0x170` |
| GA102 | `0x02` | `0x172` |
| GA103 | `0x03` | `0x173` |
| GA104 | `0x04` | `0x174` |
| GA106 | `0x06` | `0x176` |
| **GA107** | **`0x07`** | **`0x177`** |

---

## Register headers to use

GA107 has no dedicated header directory. NVIDIA ships Ampere register
definitions under `ga102` and `ga100`, and GA107 uses the GA10x layout — the
differences from GA102 are unit *counts* (SM/TPC/GPC), not register *offsets*.

```
reference/open-gpu-kernel-modules/src/common/inc/swref/published/
  nv_ref.h                    <- BOOT_0 / BOOT_42, architecture constants
  nv_arch.h                   <- implementation constants
  ampere/ga102/dev_boot.h     <- boot register detail
  ampere/ga102/dev_gsp.h      <- GSP mailbox registers (Phase 4)
  ampere/ga102/dev_falcon_v4.h
  ampere/ga102/dev_riscv_pri.h <- GSP is a RISC-V core on Ampere
  ampere/ga102/dev_fbif_v4.h  <- framebuffer interface (Phase 5)
  ampere/ga102/dev_ram.h
  ampere/ga102/dev_vm.h       <- page tables (Phase 5)
```

If the laptop turns out to be AD107, substitute `ada/ad102/` throughout.
