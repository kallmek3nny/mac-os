# `ga107-firmware/` — blobs pulled from the laptop

Extracted 2026-08-07 over SSH from the target laptop (`kenny-linus`, Ubuntu,
kernel 7.0.0-29-generic, `nvidia-595-open` 595.84 installed).

**These are NVIDIA-signed binaries redistributed with the Linux driver. They are
not ours, are not MIT-licensed like `open-gpu-kernel-modules`, and must not be
re-published. They are here as reference input for Phase 4 only.**

## What each file is

| File | Size | Role |
|---|---|---|
| `bootloader-570.144.bin` | 24,684 B | The GSP RISC-V bootloader. **This is the image the mmiotrace DMA'd into IMEM.** |
| `booter_load-570.144.bin` | 61,304 B | SEC2 "Booter Load" — sets up WPR2 and authorises the GSP image |
| `booter_unload-570.144.bin` | 41,080 B | SEC2 "Booter Unload" — tears WPR2 down |
| `vbios-ga107.rom` | 65,024 B | VBIOS dump from PCI config ROM — **truncated**, see § *VBIOS: BIT table and the falcon ucode pointer* |

Source paths on the laptop: `/lib/firmware/nvidia/ga107/gsp/*.zst`
(decompressed with `zstd -d`). The 570.144 revision was chosen over the
535.113.01 sibling because it is the newer of the two shipped for this die.

Also present on the laptop but **not** copied: `/lib/firmware/nvidia/595.84/gsp_ga10x.bin`
at 72,865,776 bytes — the full GSP firmware image. Too large to carry here and
not needed until Phase 4 actually reaches the RM-image stage.

## Verification

All three blobs are wrapped in the same outer container — `nvfw_bin_hdr`, the
structure `linux-firmware` blobs use (nouveau's `nvfw/fw.h`). **They are not
VBIOS-embedded `FALCON_UCODE_DESC` images**, which matters a great deal for
Phase 4 and is the single most useful thing this extraction established.

```
                        bin_magic   bin_ver  bin_size  hdr_off  data_off  data_size
bootloader-570.144      0x10de      1        24832     24       108       24576
booter_load-570.144     0x10de      1        61440     24       888       60416
booter_unload-570.144   0x10de      1        41216     24       888       40192
```

`bin_size` is the **padded/aligned** image length and is legitimately larger
than the file on disk (24,832 > 24,684) because the trailing pad is not stored.
An early version of the parser flagged this as out-of-range; that check was
wrong, not the data. `data_offset + data_size` does fit inside every file, which
is the bound that actually matters.

### The two blob types use different inner descriptors

This is the trap. `header_offset` is 24 in all three files, so a single parser
looks like it should work — but what sits there differs by blob type:

**`bootloader-570.144.bin` → `RM_RISCV_UCODE_DESC`** (`rmRiscvUcode.h`), version 5:

```
version                5          bootloaderOffset      20480
bootloaderSize         2176       bootloaderParamOffset 22656
bootloaderParamSize    16         riscvElfOffset/Size   0 / 0
manifestSize           2048       monitorDataOffset     2048
monitorDataSize        4096       monitorCodeOffset     6144
monitorCodeSize        10496      bIsMonitorEnabled     1
```

Every offset lands inside the file. `bIsMonitorEnabled = 1` with a 10,496-byte
monitor code region means this image carries a LIBOS monitor, matching the
RISC-V-with-monitor boot path in `kernel_gsp_tu102.c`.

**`booter_load` / `booter_unload` → `nvfw_hs_header_v2`** — heavy-secure signed
images, identical header shape in both:

```
sig_prod_offset  60     sig_prod_size  768    patch_loc       828
patch_sig        832    meta_data_off  836    meta_data_size  12
num_sig          848    header_offset  852    header_size     36
```

These are SEC2 HS ucodes. `sig_prod_size = 768` across `num_sig` entries is the
production signature block; `patch_loc` is where the bootloader patches the
selected signature in. **A driver cannot load these without performing that
signature patch step** — which is the part Phase 4 has no way to shortcut.

### What was wrong before, and why it is recorded

The first parse read `RM_RISCV_UCODE_DESC` at a fixed offset in all three files.
The bootloader came out clean and both booters produced nonsense — fields like
`bootloaderOffset = 0x0000f000` pointing past the file. A follow-up scan for a
`FALCON_UCODE_DESC` V2/V3 header (per the bit definitions in
`kernel_gsp_fwsec.c` lines 129–145) found **none** in the first 256 bytes of any
blob.

That null result was the answer, not a failure: the VBIOS-embedded descriptor
format simply is not what `linux-firmware` ships. The `0x3c` = 60 word that
looked like `FALCON_UCODE_DESC_V2_SIZE_60` is a coincidence — it is
`sig_prod_offset` in the HS header.

Recorded because guessing here is exactly this project's established failure
mode: a plausible-looking wrong offset returns a plausible-looking number.

VBIOS dump identifies as:

```
55 aa                              valid option-ROM signature
"E4736 SKU 10 VGA BIOS"
"Version 94.07.4E.00.4B"
"Copyright (C) 1996-2021 NVIDIA Corp."
"725M47360010"                     board part number
```

**The `55 aa` header and the version string confirm a real dump, not a stub.**
The strings `PLEASE POWER DOWN AND CONNECT THE PCIe POWER CABLE(S)` and
`BIOS Certificate Check Failed!!!` are stock NVIDIA VBIOS error text present in
every image — they are not indications of a problem with this dump.

### VBIOS: BIT table and the falcon ucode pointer

Regenerate everything in this section with:

```sh
python tools/parse-vbios.py reference/ga107-firmware/vbios-ga107.rom
```

The committed output is `vbios-report.txt`; `tests/test_parse_vbios.py` pins the
struct layouts (53 tests).

**The falcon ucode table is not absent from this image — it is off-file.** That
distinction matters: "absent" would mean GA107 does not carry the table in its
VBIOS at all, and Phase 4 would have to source the ucode elsewhere. "Off-file"
means the table exists, and this dump is simply too short to contain it.

What the parser establishes:

| Fact | Value |
|---|---|
| PCI expansion ROM chain | one image at `0x000000`, `last=0x80` — no second image |
| PCIR | `0x000170`, vendor `0x10de`, device `0x25a2` (GA107, RTX 3050 Laptop) |
| PCIR image length | 127 blocks = 65,024 B — exactly the file size |
| BIT header | `0x0001b0`, sig `BIT\0`, header checksum sums to `0x00` (valid) |
| BIT tokens | 17 entries, 6 bytes each |
| Token `0x70` `'p'` `BIT_TOKEN_FALCON_DATA` | pointer bytes `77 7f 07 00` |
| → `FalconUcodeTablePtr` | `0x077f77` = 491,383 |
| ROM size that pointer requires | ≥ 491,389 B (480 KiB) |
| Shortfall against this dump | 426,365 B |

Every *other* BIT token pointer lands inside the file. Token `0x70` is the only
one that does not, and it overshoots by 6.5×. A parser bug would not produce
sixteen in-range pointers and one wildly out-of-range one from the same table
whose header checksum validates.

Corroborating negatives, all from the same run:

* `nvfw_bin_hdr` `binMagic 0x000010de` — absent.
* `NVGI` and `NVDA` container magics — absent.
* No `FALCON`, `FWSEC`, `BOOTER`, `ACR`, `GSP`, `SEC2`, or `PMU` signature
  strings anywhere in the 65,024 bytes.

Those absences are consistent with truncation, not with a stripped image: the
region the pointer names was never captured, so nothing that lives in it can
appear.

**Why the dump is short.** The PCI config-space ROM BAR window exposed 64 KiB;
the physical flash is larger. Re-reading `rom` the same way will return the same
65,024 bytes. Obtaining the falcon table needs a different path — `nvflash`, a
vendor ROM image for board `725M47360010`, or reading the SPI flash directly —
not another `dd` of the `rom` attribute.

## How the dump was obtained, and the trap in it

The first two attempts returned `dd: IO error` and a 0-byte file. The cause was
**not** power management — `power/control` already read `on` and the device was
in `D0`. It was that **no driver was bound**: with `driver` empty, the kernel
refuses to service reads of the `rom` attribute even when `rom_enable` is set.

```sh
sudo modprobe nvidia          # bind first — this is the step that matters
sleep 3
echo 1 | sudo tee /sys/bus/pci/devices/0000:01:00.0/rom_enable   # (attribute is `rom`)
sudo dd if=/sys/bus/pci/devices/0000:01:00.0/rom of=vbios.rom bs=65536
echo 0 | sudo tee /sys/bus/pci/devices/0000:01:00.0/rom
```

Note the interaction with `prep-blacklist.sh`: that script blacklists `nvidia`
so the module does not autoload during a capture. An explicit `modprobe nvidia`
still works (which is exactly why the `install ... /bin/true` lines were removed
from it — see `HANDOFF.md` §5). If a future session finds the ROM unreadable,
check driver binding before suspecting hardware.

## Why these matter to Phase 4

`PHASE4-GSP-BRINGUP.md` §3.3 documents 96 × 256 B DMA transfers pushing a
24 KB image into GSP IMEM. `bootloader-570.144.bin` is 24,684 bytes. That is the
image. Having it locally means the descriptor header can be parsed offline and
the kext's DMA loop can be written against a real structure instead of an
inferred one.
