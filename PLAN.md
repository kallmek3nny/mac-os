# RTX 3050 macOS Driver — Project Plan

**Status:** exploratory / research project
**Hard constraint:** no hardware purchases
**Honest ceiling:** Phases 0–5 are achievable. Phase 6+ is not, and even a complete driver would not produce a picture on the target laptop. Read "Termination Conditions" before starting.

---

## 0. What is actually being built

A macOS GPU driver is four separate products, not one:

| Layer | What it does | Feasibility |
|---|---|---|
| Kernel driver (kext) | PCI claim, MMIO, firmware boot, memory mgmt, command submission | Hard but **open** — NVIDIA published the kernel-side source |
| Metal userspace driver | Compiles Metal IR → NVIDIA SASS machine code | **Blocked** — Apple's plugin ABI is private and undocumented |
| Display engine driver | Modes, timings, scanout to a physical panel | Hard, and moot on this laptop (see Termination) |
| Power management | Clocks, thermals, sleep/wake | Hard, deferrable |

The kernel half went from "impossible" to "genuinely attemptable" when NVIDIA open-sourced `open-gpu-kernel-modules` (Turing and newer, which includes Ampere/GA107). That code contains the GSP firmware boot sequence, register definitions, and the resource-manager protocol. It is dual MIT/GPLv2 — take the MIT branch and it is legally usable in a closed or permissive macOS kext.

The userspace half has no equivalent. NVIDIA's shader compiler is closed, and Apple's Metal driver plugin interface is private with zero documentation. That is where the project stops.

---

## 1. Software to install — all free

### On the desktop (development host)

| Software | Purpose | Source |
|---|---|---|
| **VMware Workstation Pro** | macOS VM to develop and debug in | Broadcom — free for personal use, account required |
| **OpenCore** + `OSX-KVM`/`macOS-Simple-KVM` unlocker | Boot macOS in the VM | GitHub, free |
| **macOS installer** (Sonoma or Sequoia) | The OS itself | Apple, free |
| **Ghidra** | Disassemble Apple's GPU kexts to recover the private ABI | NSA, free |
| **Git + Python 3** | Build tooling | Free |

### Inside the macOS VM

| Software | Purpose | Source |
|---|---|---|
| **Xcode + Command Line Tools** | Build kexts, `lldb` | Mac App Store, free |
| **Xcode Additional Tools** | Contains **IORegistryExplorer** — essential | developer.apple.com, free account |
| **Kernel Debug Kit (KDK)** | Debug symbols for the kernel; required for two-machine debugging | developer.apple.com, free account |
| **Apple free developer account** | Needed for KDK + Additional Tools downloads | Free (paid $99 account NOT required for this) |

### Source trees to clone (read-only research)

| Repo | Why |
|---|---|
| `NVIDIA/open-gpu-kernel-modules` | **The single most important asset.** GSP boot, Ampere register headers, RM protocol |
| `envytools` | Community NVIDIA hardware documentation, ISA notes |
| Mesa (`src/nouveau`, NVK) | Ongoing open SASS reverse engineering |
| Linux `nouveau` kernel driver | Second reference implementation |
| Apple `XNU` sources | IOKit/IOPCIFamily internals |
| `acidanthera/Lilu` + `WhateverGreen` | How the Hackintosh community patches GPU drivers at runtime |

### For hardware tracing (free alternative to a PCIe analyzer)

| Software | Purpose |
|---|---|
| **Linux live USB** (Ubuntu/Fedora) | Boot the laptop, run the real NVIDIA driver, capture ground truth |
| `nouveau` MMIO trace (`mmiotrace`) | Log every register write the working driver performs — this replaces the $5k bus analyzer |
| `nvidia-smi`, `lspci -vvv` | Enumerate the real card, dump config space |

**What you do NOT need to buy:** the second debug machine is your laptop (you already have two machines — that is exactly the KDP two-machine setup). The PCIe analyzer is replaced by `mmiotrace` on Linux. The $99 Apple developer account is not required.

---

## 2. Access I need from you

1. **Exact laptop model** — I need the real `PCI\VEN_10DE&DEV_xxxx&SUBSYS_xxxx` string, not a guess. GA107 ships as several device IDs with different GSP requirements.
2. ~~**Whether the laptop is muxless**~~ — **ANSWERED, 2026-08-20.** The laptop is
   muxless, confirmed from two independent directions: the operator's own report
   (`3050-handoff27.md` section 1) and the GA107 VBIOS itself, whose Display
   Configuration Block declares ZERO display paths — `entry_count` is `0x00` in a
   DCB that is present, well formed and at the offset its own pointer names. The
   decoder is `tools/parse-dcb.py` and its committed output is `dcb-report.txt`.
   Termination Condition A is therefore SATISFIED rather than open. Nothing is
   needed from you here.
3. **Confirmation you can boot a Linux live USB on the laptop** — the entire tracing strategy depends on it.
4. **Which machine hosts the macOS VM** — the desktop (i5-14400F, 32GB+) is the right answer.

---

## 3. Phased plan

Each phase has a binary pass/fail milestone. Do not proceed until the previous one passes.

### Phase 0 — Environment (realistic: 1–2 days)
- macOS VM boots and is stable
- Xcode builds and loads a hello-world kext with SIP disabled
- Two-machine `lldb` KDP debugging works: desktop VM ↔ laptop
- **Milestone:** deliberately panic your hello-world kext and catch the panic in `lldb` with symbols

### Phase 1 — Reconnaissance (1–2 weeks)
- Ghidra-disassemble `AMDRadeonX6000.kext`, `IOGPUFamily`, `AppleGPUWrangler`
- Document every class the kext must subclass and every method `IOGPUFamily` calls into
- On the laptop: Linux live USB, `lspci -vvv` dump, full `mmiotrace` of NVIDIA driver init
- **Milestone:** a written spec of the IOGPU entry points, and a complete boot-time register trace

### Phase 2 — Enumeration (1–2 weeks)
- Kext with an `IOPCIDevice` match dictionary for the 3050
- Loads, attaches, appears in IORegistryExplorer
- **Milestone:** your driver visible in the IORegistry, attached to the card

### Phase 3 — Register access (days, not weeks)
- Map BAR0, read `NV_PMC_BOOT_0` and `NV_PMC_BOOT_42`
- **Milestone:** chip ID decodes to `0x177` (GA107). First proof of real silicon conversation.
- Revised down from the original 1–3 week estimate: this is one `IOMemoryMap` and two masked reads, and `phase0-kext/` already implements it.

### Phase 3.5 — Firmware health (days) — *added after reading NVIDIA's source*
- Poll `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(0)` bits 7:0 for `0xFF`
- Read `NV_USABLE_FB_SIZE_IN_MB` (scratch group 42)
- **Milestone:** GFW boot reports COMPLETED and the framebuffer size is plausible. This proves the GPU's own firmware came up healthy *under macOS's PCI init*, which `NV_PMC_BOOT_0` cannot tell you.
- Still zero writes. This is the last milestone reachable without ever writing a register, and it is the go/no-go signal for Phase 4. See `PHASE1-GSP-NOTES.md`.

### Phase 3.75 — GSP engine inspection (days) — *implemented*
- Read `HWCFG2` (RISC-V capable? scrubbing done?), `HWCFG` (IMEM size)
- Read `FALCON_ENGINE` (held in reset?), Falcon `CPUCTL`, RISC-V `CPUCTL`, `BCR_CTRL`
- Read `PRIV_ERR_STAT` and `HUB_ERR_STAT` — the only diagnostics GSP has
- **Milestone:** a go/no-go verdict on whether macOS's PCI initialisation leaves the GSP in a bootable state, or resets the engine out from under the VBIOS.
- Still zero writes. This is the last free answer before Phase 4, and it is the cheapest possible test of the assumption the entire plan rests on. Learning the answer from a log line beats learning it from a hang three months into firmware plumbing.
- Address translation goes through `nvGspFalconReg()` / `nvGspRiscvReg()`, never a bare offset — see `PHASE1-GSP-NOTES.md` and the three-address-space hazard in `NVFalcon.hpp`.

### Phase 4 — GSP firmware boot (3–12 months, revised up)
- Port the boot sequence from `open-gpu-kernel-modules` to IOKit primitives
- Falcon/Riscv bootloader, firmware upload, handshake, heartbeat
- **Milestone:** GSP responds. This is the phase that historically kills attempts, and the phase most helped by NVIDIA's open source.

### Phase 5 — Memory + command submission (3–12 months)
- VRAM allocator, GPU page tables, DMA a pattern and read it back
- Push a trivial command buffer, see the fence signal
- **Milestone:** the GPU executes something you told it to and reports completion

### Phase 6 — Compute (blocked)
Requires hand-assembling SASS. Possible in principle, but you are now writing machine code for an undocumented ISA with no assembler.

### Phase 7 — Metal (not achievable)
Requires implementing Apple's private Metal driver plugin ABI **and** a full Metal IR → SASS compiler backend. No public documentation exists for either side. This is the wall.

---

## 4. Termination conditions

The project ends — regardless of code quality — at whichever of these you hit first:

**A. Muxless laptop — SATISFIED, confirmed 2026-08-20.** The 3050 has no wire to
the panel. It renders into a buffer and the Intel iGPU scans it out. macOS has no
Optimus concept. Even a flawless 3050 driver produces no image.

This is no longer a prediction to check in the BIOS. It is confirmed from two
independent directions: the operator's report that all graphics route through the
Intel chip (`3050-handoff27.md` section 1), and the GA107's own VBIOS, whose
Display Configuration Block declares ZERO display paths — `entry_count` is `0x00`
in a DCB that is present, well formed, version `0x41`, and sitting exactly where
the pointer at ROM offset `0x36` says it sits, with its connector sub-table empty
while the CCB, GPIO and I2C siblings are populated. The decoder is
`tools/parse-dcb.py`, its committed output is `dcb-report.txt`, and the ROM is
`reference/ga107-firmware/vbios-ga107.rom`. The firmware direction is the stronger
of the two: a driver cannot invent a path the board does not carry, and this DCB
describes no path of any kind — not even the HDMI bypass `3050-handoff27.md`
named.

Work on the NVIDIA track from here is Phase 4/5 engineering with no route to the
internal panel. That is a bounded, deliberate scope, not an unfinished display
path.

**B. No Tiger Lake framebuffer.** Apple never shipped a Mac with an 11th-gen Intel chip, so macOS contains no Iris Xe / UHD framebuffer driver. Not buggy — absent. The iGPU cannot scan out either.

Measured 2026-08-15 rather than asserted. `tools/find-kexts-in-image.py` scanned
the 15.4.1 Sequoia recovery image and recovered 430 kext paths and 491 bundle
identifiers — the scan is demonstrably not blind, so an absence in it means
something. Every Intel graphics framebuffer it ships is:

```
AppleIntelGMAHD / AppleIntelGMAHDFB    Arrandale-era GMA
AppleIntelKBLGraphicsFramebuffer       Kaby Lake, 7th gen, Gen9
AppleIntelCFLGraphicsFramebuffer       Coffee Lake, 8th/9th gen, Gen9
AppleIntelICLLPGraphicsFramebuffer     Ice Lake-LP, 10th gen, Gen11
```

There is no TGL bundle of any kind. Ice Lake is the newest display engine macOS
knows how to drive, and Tiger Lake is the generation directly after it — Gen12,
a different display engine, not a stepping. The one Metal-capable Intel driver
in the image is `AppleIntelHD5000GraphicsMTLDriver`, i.e. Haswell Gen7.5 from
2013. Full unfiltered inventory in `intel-kext-inventory.txt` -- 430 paths and
491 bundle identifiers, re-measured 2026-08-19 against
`C:/Users/Kentl/macos-vm/sonoma/BaseSystem.img`. Tiger Lake / Gen12 / Iris Xe is
**zero matches across all 430**, which is what makes this absence citable rather
than suggestive. (An earlier version of that file listed only 16 of the 430 it
had reached, and an absence measured against an excerpt is not an absence.)

The caveat that keeps this honest: this is the *recovery* image, which is a
trimmed subset of a full install. It is strong evidence, not proof, that the
installed system volume also lacks TGL.

**B is now confirmed a second way, and the first way was arguing the weaker
point.** A missing kext NAME leaves open the possibility that scanout does not
need a per-generation driver at all — and that premise was never tested until
2026-08-19. Two things could have falsified it, and both were measured:

*A generic framebuffer does ship.* The image carries `IOBootFramebuffer`, an
`IOFramebuffer` subclass with a real mode/aperture/gamma vtable (108
occurrences), alongside `IOFramebuffer` 2035, `IOGraphicsFamily` 95 and
`IONDRVFramebuffer` 195. So the assumption that macOS has no
generation-independent scanout path was wrong. It cannot help, because it has
**no IOKit personality** — zero of those 108 occurrences sit within 4000 bytes of
an `IOProviderClass` key, so it is instantiated by code inside IOGraphicsFamily
rather than matched, and nothing can point it at a PCI device. Corroborated
against controls proving IOGraphicsFamily personalities *are* visible to the same
scan.

*The real gate is a device-id allow-list.* Apple's framebuffer personalities
carry both `IOPCIClassMatch` (`0x03000000&0xff000000` — any display controller)
and `IOPCIPrimaryMatch` (an explicit list of `<device><vendor>` words). IOKit
requires both when both are present, so the class match is not the gate.
`tools/find-display-matches.py` reads those dictionaries out of the prelinked XML
and `display-match-report.txt` is its committed output: **203 distinct device ids,
unfiltered, and `0x9A688086` — the Tiger Lake GT2 id measured on the laptop
itself — is absent from every one of them**, while both positive controls
(`0x3E9B8086` Coffee Lake, `0x8A528086` Ice Lake-LP) are present in the same
parse. The Intel graphics ids stop at Ice Lake-LP, the generation immediately
before Tiger Lake.

So condition B is not "no kext is called Tiger Lake". It is that the allow-lists
inside the kexts that *do* ship exclude this silicon **by number**, and the one
generic framebuffer that exists can never be aimed at it. Full reasoning in
`3050-handoff37.md` section 8; the gate is `tests/test_find_display_matches.py`.

The known workaround, stated so it is not mistaken for a plan: inject a fake
device id so an Ice Lake personality attaches. That makes a driver *bind*; it
does not make the driver correct. Gen11 and Gen12 are different display engines,
so an ICL driver programming TGL registers is this project's own measured
`PLANE_SIZE` / `PIPESRC` transposition one level up — it programs cleanly and
yields a plausible wrong picture, which is worse than a blank screen because it
looks like progress.

**C. Metal ABI is private.** Even on a desktop with a monitor wired directly to the card, without a Metal driver the OS treats the GPU as a non-graphics device. WindowServer will not use it.

**D. Intel macOS is ending.** macOS Tahoe (26) is Apple's final Intel release. The target platform has a expiry date measured in a year or two.

A + B together mean the laptop specifically cannot display macOS through either of its GPUs, independent of driver quality.

---

## 5. What this plan is actually good for

Phases 0–5 are a legitimate, genuinely impressive systems-programming project. You would learn IOKit, PCI, DMA, firmware bringup, and kernel debugging on real hardware, and NVIDIA's open source makes Phase 4 a real possibility rather than a fantasy. Finishing Phase 5 would put you ahead of almost everyone who has attempted this.

What it will not do is boot macOS on that laptop. If the goal is "run macOS," the VM path gets you there this week. If the goal is "write GPU driver code," this plan is worth executing and I will work through every phase with you.

Those are two different projects. Pick deliberately.

---

## 6. Next action

Answer the four items in section 2. Phase 0 can start immediately after that — none of it requires the laptop.
