# macOS on the RTX 3050 laptop

This folder holds two things: an honest assessment of whether a macOS driver for a mobile RTX 3050 is possible, and a real, buildable starting point for the part that *is* possible.

## Read in this order

| File | What it is |
|---|---|
| **`HANDOFF.md`** | **Read first on any resume.** Current state, all four verification gates (`check-registers`, `build-msvc`, `check-kext-syntax`, `run-python-tests`), every bug found and why each was silent, and the open blockers. |
| **`STEPS.md`** | **Start here.** The seven concrete steps, in order, one at a time. Step 1 is a ten-minute check that may end the project. |
| **`VM-BRINGUP.md`** | The macOS VM, as actually built. Four consecutive power-on failures and the fix for each — none of them produced an error message naming its own cause. Read before touching the VM. **The VM boots and the macOS Recovery GUI is interactive** (confirmed by the user clicking in the console, 2026-08-07). An earlier revision of this line claimed the guest was frozen and told you to run `bcdedit /set hypervisorlaunchtype off` — **that was wrong, do not do it**; see the *false alarm* section in that file for why a static framebuffer and near-zero CPU prove nothing about a Recovery UI. The guest is **macOS 15.4.1 Sequoia, build 24E263**, not Sonoma, despite every filename saying otherwise. |
| **`NETWORK-DIAGNOSIS.md`** | The Recovery *"An Internet connection is required"* wall, localised hop by hop. The host side is **measured and eliminated** (host reaches 8.8.8.8, resolves Apple's endpoints); the guest side is a runbook nobody has executed yet. Read this before touching the network — it is the only doc with the per-hop evidence. |
| **`handoff1.md`** | A **point-in-time** snapshot of the 2026-08-07 VM-bringup session — what changed, what was gotten wrong and corrected, and the one question open at the end. Not a replacement for `HANDOFF.md`; read that first. Useful for the reasoning behind the retractions, which is deliberately preserved. |
| **`PLAN.md`** | The full project plan: what a macOS GPU driver actually consists of, phase-by-phase milestones, and the four termination conditions. Read §4 before investing time. |
| **`SETUP.md`** | Phase 0 in detail. Free software list, macOS VM, SIP/boot-args, two-machine kernel debugging, and the Linux `mmiotrace` capture that replaces a PCIe bus analyzer. |
| **`DEVICE-IDS.md`** | Which die is actually in the laptop. "RTX 3050 Laptop GPU" covers GA107, GA106, AD106, and AD107 across **two architectures** — this is not a formality. |
| **`PHASE1-GSP-NOTES.md`** | The GSP firmware boot sequence, transcribed from NVIDIA's own source with file and line references. Read before planning Phase 4. |
| **`PHASE4-GSP-BRINGUP.md`** | The GSP bringup **specification**, derived from a real `mmiotrace` of the working Linux driver (357,495 lines; region `pgsp`, 206,293 accesses). Every register name resolved against NVIDIA's `swref` headers — nothing guessed. |
| **`PUBLISH-GSP-BRINGUP.md`** | The **publishable** writeup: *"Ampere GSP bringup on macOS"*, standalone, readable without this repo. Same measured sequence as `PHASE4-GSP-BRINGUP.md`, plus the thirteen-item trap list, the safety envelope, and the method rules. Aimed at desktop Ampere (3060–3090) Hackintosh builds, where the muxless-display blocker in §1 does not exist. |
| **`phase0-kext/`** | A working IOKit kext that claims the card, maps BAR0, reads the chip ID, and polls firmware health. Builds with `make` on macOS. |
| **`tools/`** | Dependency-free Python scripts: device-ID lookup, register-constant verifier (113/113 as of 2026-08-11), the `mmiotrace` parser, the sequence collapser, the DMA summariser, plus the VM read/write pair — `vm-screenshot.py` (occlusion-proof capture + OCR) and `vm-type.py` (Win32 `SendInput` scancode injection). See `tools/README.md`; **`vm-type.py` needs an elevated process**, and unelevated runs are accepted-and-discarded rather than failing. For before/after evidence use **`png-delta.py`** on two saved `--out` captures — `vm-screenshot.py --compare` diffs its own two post-action frames, so it cannot attribute a change to an action (inherent, not a bug). Its argument is a **base name**: `--compare NAME.png` writes `NAME.frame1.png` / `NAME.frame2.png` and never touches `NAME.png` — it used to overwrite it, fixed 2026-08-07 and guarded by `tests/test_vm_screenshot.py::DerivedFramePathTests`. |
| **`tests/`** | Host-side tests. The kext's decode logic, buildable **on Windows** with MSVC (five suites, 634 checks — 39 decode + 169 falcon + 85 RISC-V ucode + 199 GSP boot + 142 GSP stage), plus a CPython-only gate (`run-python-tests.bat`, **356 tests**, re-run and green 2026-08-11) covering the `tools/dmg2raw.py` mish offsets, the `vm-screenshot.py` logic, the `vm-type.py` pure helpers (scancode tables, chord parsing, window disambiguation, `--click-at` bounds), and the `check-registers.py` / `collapse-sequence.py` parsers. Catches the class of bug that has already bitten this project twice. |
| **`tests/kextshim/`** | A minimal IOKit/libkern stub tree that lets the clang inside Visual Studio 2022 **parse** the kext on Windows. A parse, not a build — see its `README.md` for exactly what that does and does not prove. |

## The one-paragraph version

The kernel half of this project is genuinely attemptable, because NVIDIA open-sourced `open-gpu-kernel-modules` for Turing and newer — that repo contains the GSP firmware boot sequence and the Ampere register definitions, under a dual MIT/GPLv2 licence. Phases 0 through 5 (enumerate → map registers → boot firmware → allocate memory → submit a command) are hard but real, and finishing them would put you ahead of essentially everyone who has tried.

The userspace half is not attemptable. A GPU is invisible to macOS without a Metal driver, and Apple's Metal driver plugin ABI is private and undocumented — you would be recovering it by disassembling `AMDRadeonX6000.kext`. On top of that you would need a Metal IR → NVIDIA SASS compiler backend for an undocumented ISA.

And for the laptop specifically: it is almost certainly muxless, meaning the 3050 has no wire to the panel — it renders into a buffer that the Intel iGPU scans out. macOS has no Optimus concept, and contains no framebuffer driver for 11th-gen Tiger Lake graphics at all, because Apple never shipped a Mac with one. So even a flawless 3050 driver would have nowhere to put the pixels.

**If the goal is "run macOS,"** the VM path in `SETUP.md` Step 1 gets you a fully working macOS desktop this week, with no GPU acceleration but everything else intact.

**If the goal is "write GPU driver code on real hardware,"** the plan is worth executing and the skeleton is ready to build.

Those are different projects. `PLAN.md` §5 lays out the choice.

## Which machine is which

There are two machines in this project and they have completely different roles.
The desktop's GPU is **not a target** and is never touched by any of this.

| Machine | GPU | Role |
|---|---|---|
| **Laptop** (`kenny-linus`) | **GA107M, RTX 3050 Mobile, `10de:25a2`** | **The only hardware the driver is for.** Source of the `mmiotrace` capture, source of the GA107 firmware blobs, and the eventual load target for the kext. |
| **Desktop** (`KENNYSPC`) | RTX 4060 — **irrelevant** | Build and analysis box only: host tests, the clang syntax gate, the trace parsers, and the macOS VM in which the kext is compiled. |

The macOS VM uses VMware's emulated SVGA adapter and cannot see either physical
GPU. `make` inside that VM cross-builds a kext that will only ever load on the
laptop.

Enforced in code, not just prose:

```
phase0-kext/Info.plist   IOPCIMatch = 0x25a210de   exact match, 10de:25a2 only
phase0-kext/NVDecode.hpp NV_CHIP_ID_GA107 = 0x177  Ampere arch 0x17, impl 0x7
reference/ga107-firmware/                          pulled from the laptop over SSH
```

Every mention of Ada / AD106 / AD107 in these docs is a **warning**, not a
target: "RTX 3050 Laptop GPU" is a marketing name covering four dies across two
architectures, and `DEVICE-IDS.md` exists so a rebadged Ada part would be caught
rather than silently retargeting the work. `lspci` returned `0x25a2`, so it is
Ampere GA107 and the plan stands.

## Nothing here requires a purchase

| Normally needed | Free substitute used here |
|---|---|
| Second debug machine | Your laptop — desktop VM debugs it over KDP |
| PCIe bus analyzer (~$5k) | Linux `mmiotrace` on a live USB, captures every MMIO access the real driver makes |
| $99 Apple developer account | Free Apple ID covers the KDK and Xcode Additional Tools |
| NVIDIA hardware docs | `open-gpu-kernel-modules` register headers + `envytools` |

## What I need from you to go further

From a Linux live USB on the **laptop**. Full walkthrough in `STEPS.md` Step 2 — use that rather than these three lines, because the ordering matters:

1. **Mount debugfs first** — `sudo mount -t debugfs debugfs /sys/kernel/debug`
2. `lspci -nn | grep -i nvidia` — the exact `[10de:xxxx]` device ID
3. `cat /sys/kernel/debug/vgaswitcheroo/switch` — mux or muxless, decides Termination Condition A
4. `xrandr --listproviders` — an independent cross-check on item 3

Step 1 is not optional. An unmounted debugfs makes *every* file under it look absent, including `vgaswitcheroo` — which turns a muxed laptop into a false "muxless" verdict. That is the single most consequential wrong answer this project can produce, and item 4 exists because it cannot lie the same way.

The trace capture (`mmio-boot.log`) is `STEPS.md` Step 3, done in the same live session.
