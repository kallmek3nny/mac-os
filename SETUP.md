# Phase 0 — Environment Setup

Everything here is free and none of it needs the laptop. Work top to bottom; each step has a check you can actually verify before moving on.

---

## Step 1 — macOS VM on the desktop

Host: i5-14400F, 32GB+ RAM recommended, 100GB+ free disk.

1. Install **VMware Workstation Pro** (free for personal use, Broadcom account required).
2. Apply the **macOS unlocker** — VMware hides the macOS guest type by default. `github.com/DrDonk/unlocker`. Run it with VMware fully closed and its services stopped.
3. Get a macOS installer. Easiest path without owning a Mac: **`OSX-KVM`'s `fetch-macOS-v2.py`** pulls the official recovery image straight from Apple's servers. The original intent was **Sonoma (14)** — Sequoia and Tahoe tightened kext loading further, and Sonoma is the last comfortable release for third-party kext work.

   > **What the VM in use actually runs is macOS 15.4.1 Sequoia, build 24E263.** `fetch-macOS-v2.py` defaults to `os_type='latest'`, which returns the newest recovery image for the board-id regardless of what you meant to pick. Confirmed 2026-08-07 from the `SystemVersion.plist` embedded in the raw image — the string "Install macOS Sonoma" appears **zero** times in it. **Check the fetched image rather than trusting the picker**, and if you land on 15.x, expect the kext-loading friction this step was written to avoid. Downstream: the KDK and Xcode must match **15.4.1 / 24E263**, and guest tools need `darwin.iso`, not `darwinPre15.iso`. See `VM-BRINGUP.md`.
4. VM settings that matter:
   - **4 vCPU (2 cores × 2 sockets), 8 GB RAM** — this is what the working VM
     actually runs (`Sonoma.vmx`: `numvcpus = "4"`, `cpuid.coresPerSocket = "2"`,
     `memsize = "8192"`), verified 2026-08-07 on a 16-logical-CPU / 32 GB host.
     An earlier draft of this step said "8+ vCPU, 16GB+ RAM"; nothing was ever
     built or tested at those numbers. More is fine if you have it, but do not
     treat 8/16 as a requirement — the installer boots and runs at 4/8.
   - Firmware: **UEFI**, Secure Boot **off**
   - `guestOS` must match the image you actually fetched. The VM in use is
     `darwin24-64` (macOS 15), not `darwin23-64` (macOS 14) — see the Sequoia
     note above.
   - Add `smc.version = "0"` to the `.vmx` file or macOS will refuse to boot
5. Boot the recovery image, use Disk Utility to format the virtual disk as APFS, install.

**Check:** macOS desktop boots, and `uname -a` in Terminal reports your version.

> The VM has **no GPU passthrough**. That is fine — Phases 1–2 are pure code and disassembly. Real hardware only matters from Phase 3 on, and that happens on the laptop.

---

## Step 2 — Unlock the VM for kext development

A stock macOS install will not load your unsigned kext. Boot to Recovery (in the VM: hold ⌘R equivalent, or boot the recovery partition) and open Terminal:

```sh
csrutil disable
csrutil authenticated-root disable
```

Back in the booted system:

```sh
sudo nvram boot-args="-v keepsyms=1 debug=0x144 kext-dev-mode=1"
sudo reboot
```

What those do:
- `-v` — verbose boot, so you see panics instead of a sad Mac
- `keepsyms=1` — keeps kernel symbols so panic logs are readable
- `debug=0x144` — `DB_NMI | DB_KPRT | DB_ARP`: wait for a debugger on panic instead of rebooting, print to the debug port

**Check:** `csrutil status` reports disabled, and `nvram boot-args` shows your string.

---

## Step 3 — Toolchain

Inside the VM:

1. **Xcode** from the Mac App Store, then `xcode-select --install` for the CLI tools.
2. **Xcode Additional Tools** — `developer.apple.com/download/all/?q=additional%20tools`. Free Apple ID. You want **IORegistryExplorer** out of this; you will live in it.
3. **Kernel Debug Kit** matching your exact macOS build — same download page, search "Kernel Debug Kit". The build number must match `sw_vers -buildVersion` exactly or `lldb` will refuse the symbols.

**Check:** `sw_vers -buildVersion` matches the KDK you downloaded, and IORegistryExplorer opens and shows a tree.

---

## Step 4 — Two-machine kernel debugging

This is the piece that makes Phases 3–5 survivable. Every mistake in firmware boot or DMA is a hang or a panic, and without KDP you are rebooting blind.

- **Target** (the machine that panics): the laptop, once it is running macOS. Until then, the VM can be its own target for practice.
- **Debugger** (the machine running `lldb`): the desktop VM.

On the target:

```sh
sudo nvram boot-args="-v keepsyms=1 debug=0x144 _panicd_ip=<debugger-ip>"
```

On the debugger, when the target panics:

```sh
lldb
(lldb) settings set target.load-kexts false
(lldb) kdp-remote <target-ip>
```

**Milestone for Phase 0:** deliberately panic the skeleton kext (there is a `-DPANIC_TEST` build flag for exactly this), catch it in `lldb`, and get a symbolicated backtrace showing your own function name. When you can do that, Phase 0 is done.

---

## Step 5 — Linux tracing USB (do this early)

This is the free replacement for a multi-thousand-dollar PCIe bus analyzer, and the single most valuable data source in the whole project.

1. Write an **Ubuntu 24.04** live USB (Rufus on Windows).
2. Boot the **laptop** from it. Enable "try Ubuntu", no install needed.
3. Capture ground truth:

```sh
# exact device identity - I need this string
lspci -nn | grep -i nvidia
lspci -vvv -s <bus:dev.fn> > lspci-3050.txt

# every register the real driver touches during init
#
# Do NOT do this by hand, and NEVER run `modprobe -r nvidia` or
# `systemctl stop gdm3` on this muxless laptop -- both hard-lock it.
#
# Step 1: install the blacklist to disk, once, then reboot. The desktop
#         comes back up on the Intel iGPU with the 3050 unclaimed.

sudo sh prep-blacklist.sh
sudo reboot

# Step 2: after the reboot, run the capture.

sudo sh capture.sh

# When the whole project is finished, put the driver back:

sudo sh prep-blacklist.sh --undo && sudo reboot
```

`capture.sh` (in this folder) does the whole capture with a PASS/FAIL at every
step and refuses to run if a driver is still loaded. `prep-blacklist.sh` is the
one-time prerequisite that makes it pass. See `STEPS.md` step 3.

`mmio-boot.log` is a complete, ordered log of every MMIO read and write the working driver performs to bring the card up. That log *is* the Phase 3–4 specification. Save it — put it in this folder and I will work through it with you.

4. While you are in there, check the mux question:

```sh
xrandr --listproviders
cat /sys/kernel/debug/vgaswitcheroo/switch 2>/dev/null || echo "muxless"
```

If `vgaswitcheroo` does not exist, the laptop is muxless and **Termination Condition A** applies. Worth knowing on day one rather than month six.

---

## Step 6 — Reference sources

Clone these on the desktop (Windows side is fine, they are read-only research):

```sh
git clone --depth 1 https://github.com/NVIDIA/open-gpu-kernel-modules
git clone --depth 1 https://github.com/envytools/envytools
git clone --depth 1 https://github.com/acidanthera/Lilu
git clone --depth 1 https://github.com/acidanthera/WhateverGreen
git clone --depth 1 https://gitlab.freedesktop.org/mesa/mesa
git clone --depth 1 https://github.com/apple-oss-distributions/xnu
```

The first one carries the project. Start with:
- `src/nvidia/generated/g_*_nvoc.c` — the resource manager object model
- `src/common/inc/swref/published/ampere/ga102/dev_*.h` — **Ampere register definitions**
- `src/nvidia/src/kernel/gpu/gsp/` — **the GSP boot sequence**
- `kernel-open/nvidia/nv-pci.c` — PCI attach and BAR mapping, the direct analogue of what the kext must do

GA107 (mobile 3050) is close enough to GA102 that the register headers apply; differences are mostly in unit counts, not register layout.

---

## Step 7 — Build the skeleton

```sh
cd phase0-kext
make
sudo make install
sudo kmutil load -p /Library/Extensions/NV3050Probe.kext
```

See `phase0-kext/README.md` for what it does and what to look for.

---

## What I need back from you

1. `lspci -nn` line for the 3050 — the exact `[10de:xxxx]` device ID
2. `vgaswitcheroo` result — mux or muxless
3. `mmio-boot.log` in this folder
4. Confirmation the VM boots and `csrutil status` is disabled

With 1–3 in hand, Phase 1 stops being guesswork.
