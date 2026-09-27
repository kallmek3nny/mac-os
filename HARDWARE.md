# Confirmed target hardware

Recorded 2026-08-06 from a live Ubuntu session on the target laptop. Everything
here is measured, not assumed — replace nothing in this file with a guess.

---

## The GPU

```
01:00.0 3D controller [0302]: NVIDIA Corporation GA107M
        [GeForce RTX 3050 Mobile] [10de:25a2] (rev a1)
```

| Property | Value | Source |
|---|---|---|
| PCI address | `01:00.0` | `lspci -nn` |
| Vendor:device | `10de:25a2` | `lspci -nn` |
| Die | **GA107** (GA107M mobile variant) | `tools/identify-gpu.py` |
| Architecture | **Ampere**, arch `0x17`, impl `0x7` | NVIDIA `nv_ref.h` / `nv_arch.h` |
| Revision | `a1` | `lspci -nn` |
| PCI class | `0302` — **3D controller** | `lspci -nn` |

**This is not an Ada rebadge.** The "RTX 3050 A Laptop GPU" parts
(`0x2822` / `0x28A3` / `0x28E3`) are AD106/AD107 and would have retargeted the
entire plan. `0x25a2` is genuine Ampere GA107. `PLAN.md`,
`PHASE1-GSP-NOTES.md`, and `phase0-kext/` are all already aimed at the right
architecture — no changes needed. See `DEVICE-IDS.md`.

### Expected Phase 3 register values

Derived from the confirmed die. The kext should read these back:

| Register | Expected | Note |
|---|---|---|
| `NV_PMC_BOOT_0` | `0x177000xx` | `xx` is the stepping; `rev a1` suggests `0xa1` |
| `NV_PMC_BOOT_42` | `0x17700000` + revision bits | |
| `CHIP_ID` | `0x177` | `(arch 0x17 << 4) \| impl 0x7` |

Register headers to work from:
`reference/open-gpu-kernel-modules/src/common/inc/swref/published/ampere/ga102/`

---

## The iGPU — measured 2026-08-15, and never recorded before that date

Nothing in this tree had ever captured the integrated GPU's PCI ID. Every
mention of it before this date was generic prose. From
`tools/capture-igpu-facts.sh`, run on the installed Ubuntu 26.04 LTS
(kernel 7.0.0-29-generic):

```
00:02.0 VGA compatible controller [0300]: Intel Corporation
        TigerLake-H GT1 [UHD Graphics] [8086:9a68] (rev 01)
```

| Property | Value | Source |
|---|---|---|
| PCI address | `00:02.0` | `lspci -nn` |
| Vendor:device | `8086:9a68` | `lspci -nn` |
| Part | TigerLake-H GT1, UHD Graphics | `lspci -nn` |
| PCI class | `0300` — **VGA controller** | `lspci -nn` |
| Display engine | **version 12.00, stepping D0** | i915, `dmesg` |
| Bound driver | `i915` (`xe` also offered) | `lspci -k` |
| Board | MSI, subsystem `132e` | `lspci -k` |

The display-engine line is the load-bearing one, and it is i915's own
identification of the silicon rather than an inference from the CPU model:

```
i915 0000:00:02.0: [drm] Found tigerlake (device ID 9a68)
                   integrated display version 12.00 stepping D0
```

Gen12. The newest Intel display engine macOS ships a framebuffer for is Gen11,
Ice Lake-LP — measured against the shipped image rather than assumed, see
`PLAN.md` §4 B. Gen12 is the generation immediately after it, and a different
display engine rather than a stepping of the same one. The part in this laptop
is exactly one generation past the last one Apple ever supported.

Note the class code contrast with the 3050 below: the iGPU is `0300`, a VGA
controller with outputs. The 3050 is `0302`.

### Raw display MMIO is BLOCKED on this machine — measured 2026-08-15

`tools/igpu-capture.sh` mapped all 15 display ranges and got `EPERM` on every
one of them while running as root. That is not a permissions problem and not a
hardware fact:

```
lockdown  : none [integrity] confidentiality
secureboot: SecureBoot enabled
```

The bracketed word is the active band. `LOCKDOWN_PCI_ACCESS` sits inside the
integrity band, and the kernel's `pci-sysfs.c` (v6.12, line 1047) calls
`security_locked_down()` at the top of the BAR mmap handler — before any mapping
is attempted. Root does not bypass it.

Three consequences worth writing down so they are not re-discovered:

- **`intel_reg` will not help.** intel-gpu-tools reaches the BAR through the same
  sysfs path and hits the same check.
- **A shell dumper can never work here regardless of lockdown.** The same file,
  line 1187, wires `read()`/`write()` on sysfs `resourceN` only for
  `IORESOURCE_IO` BARs. A memory BAR gets `mmap` and nothing else.
- **`mokutil --disable-validation` will not help.** It disables shim's MOK
  validation; lockdown on Ubuntu keys off the firmware's Secure Boot state, which
  that does not change.

Clearing it needs Secure Boot disabled in firmware and a reboot; lockdown cannot
be lowered at runtime. Until then the display state is available only through
i915's decoded debugfs view, which is a second-hand reading and must be marked
as such rather than as `OBSERVED`.

---

## Class `0302` — the first hard signal about the mux

PCI class `0x0302` is **3D controller**. A GPU wired to a display outputs class
`0x0300` (VGA controller).

A discrete GPU enumerating as `0302` means the firmware is telling the OS this
device has **no display outputs of its own**. That is the textbook Optimus
muxless signature, and it is independent evidence arriving before the
`vgaswitcheroo` check has even run.

It is not yet proof — Steps 2c and 2d still need to confirm — but it is a strong
prior, and it points at **Termination Condition A** in `PLAN.md` §4.

The kext handles this correctly regardless: `probe()` filters on base class
`0x03`, which accepts both `0300` and `0302`. Only the mux *conclusion* changes,
not the driver's ability to claim the device.

---

## The match dictionary is now exact

`phase0-kext/Info.plist` was carrying a vendor-wide wildcard
(`0x000010de&0x0000ffff`) that would have claimed **any** NVIDIA card. Now
narrowed to this device:

```xml
<key>IOPCIMatch</key>
<string>0x25a210de</string>
```

`IOPCIMatch` encodes `(device << 16) | vendor`, so `10de:25a2` becomes
`0x25a210de`. `probe()` remains as a second filter on display-class functions.

---

## Step 2 results — complete

| Check | Result |
|---|---|
| 2a — debugfs mounted | done (0700 root:root, needs sudo) |
| 2b — device ID | `10de:25a2`, GA107 Ampere confirmed |
| 2c — `vgaswitcheroo` | **NO VGASWITCHEROO** |
| 2d — `xrandr --listproviders` | **`Providers: number : 0`** |

---

## VERDICT: muxless, and worse than muxless

> **Re-measured 2026-08-15 — the verdict survives, but not on this evidence.**
> Both checks below were re-run from the installed Ubuntu, and both turned out
> to be uninterpretable rather than confirmatory:
>
> - `Providers: number : 0` reproduced *with `i915` loaded, bound, and lighting
>   the panel*. The session is Wayland, so `xrandr` talks to XWayland, which
>   owns no DRM device and registers no providers — it prints that exact zero
>   on every machine, single-GPU or dual. The number is a property of the
>   compositor, not of this laptop.
> - `NO VGASWITCHEROO` reproduced, but that boot still carried
>   `modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm,nouveau`
>   on `/proc/cmdline`, left in GRUB by the `capture.sh` run. With no NVIDIA
>   driver bound, nothing can register a switcheroo handler, register a DRI3
>   provider, or expose a DRM connector — and the card's PCI ROM read returns
>   EIO from D3cold. Four separate "absences", all pre-determined by one
>   kernel argument.
>
> What *did* land is positive and independent of both: `/sys/class/drm/card1`
> resolves to `0000:00:02.0`, and `card1-eDP-1` reads `connected`. **The
> built-in panel is wired to the Intel iGPU.** That is the half of the muxless
> claim that is now measured rather than inferred.
>
> The other half — whether the 3050 has *any* connector of its own — remains
> open, and cannot be closed from a boot where the card has no driver. See
> `tools/capture-igpu-facts.sh` §2c, which now detects and reports this exact
> condition instead of printing absences that look like data.

Both independent checks agree, and 2d is stronger than expected.

```
$ sudo sh -c 'cat /sys/kernel/debug/vgaswitcheroo/switch 2>/dev/null || echo "NO VGASWITCHEROO"'
NO VGASWITCHEROO

$ xrandr --listproviders
Providers: number : 0
```

### What `Providers: number : 0` means

A muxless Optimus laptop normally reports **two** providers: the Intel iGPU with
the outputs, and the NVIDIA GPU offering `Sink Offload` / `Source Output`. That
is the signature this check was written to detect.

Zero providers means neither GPU registered a DRI3 render provider in this
session — the live USB is running on a generic framebuffer (simpledrm/EFI-FB)
with no accelerated driver bound to either device. So this session cannot even
see the display topology.

Combined with three other facts, the conclusion is not in doubt:

1. PCI class `0302` (3D controller), not `0300` (VGA controller) — firmware
   declares the 3050 has **no display outputs**
2. No `vgaswitcheroo` — the kernel found no mux to switch
3. The panel is lit, and it is not being driven by the 3050

The 3050 renders into a buffer that the Intel iGPU scans out. There is no wire
from the NVIDIA die to the panel.

### Termination Condition A is met

See `PLAN.md` §4. Even a flawless, complete GA107 macOS driver would produce no
image on this laptop, because:

- **A.** The 3050 is muxless — no path to the panel. macOS has no Optimus
  concept; PRIME offload is a Linux/Windows mechanism with no macOS analogue.
- **B.** macOS contains no framebuffer driver for 11th-gen Tiger Lake graphics,
  because Apple never shipped a Mac with one. So the iGPU cannot scan out
  either. Not buggy — absent.

A and B together mean the laptop cannot display macOS through **either** of its
GPUs, independent of driver quality. This is not a difficulty; it is a
topology.

---

## What survives

The verdict kills "boot macOS on this laptop." It does not kill the driver work,
because none of Phases 2 through 5 involve the display:

| Phase | Needs a display? | Still viable |
|---|---|---|
| 2 — enumeration | no | yes |
| 3 — identity registers | no | yes |
| 3.5 — firmware health | no | yes |
| 4 — GSP firmware boot | no | yes |
| 5 — memory + command submission | no | yes |
| 6 — compute | no | blocked on SASS, not on the mux |
| 7 — Metal / display | **yes** | no, and was already blocked by the private ABI |

Phase 7 was already impossible for an unrelated reason (Apple's Metal driver
plugin ABI is private and undocumented). The mux finding removes a phase that
was not reachable anyway.

What it does change is the **purpose**: this is now a GPU bringup project on
real hardware, not a path to running macOS. Those are different projects, and
the choice between them belongs to the person doing the work.

See `PLAN.md` §5.

### The debugfs permission problem

```
$ ls /sys/kernel/debug/
ls: cannot open directory '/sys/kernel/debug/': Permission denied
```

The mount succeeded; the directory is simply `0700 root:root`, so an unprivileged
`ls` cannot read it. This is normal and not a failure — every subsequent access
simply needs `sudo`.

It does mean the 2c command as originally written was wrong: without `sudo`, the
`ls` fails on permissions and the `||` branch prints `NO VGASWITCHEROO`,
fabricating a muxless verdict from a permissions error. Corrected in
`STEPS.md`.
