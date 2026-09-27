# One step at a time

Do these in order. Don't read ahead if it stresses you out — each step is
self-contained and I'll tell you when to move on.

---

> **STATUS 2026-08-07 — Steps 1, 2 and 3 are DONE, and the trace is fully
> analysed. Read this before Step 4.**
>
> **Step 3 captured 2026-08-06 22:41 UTC:** `mmio-boot.log.gz`, 357,495 lines,
> `gzip -t` clean, real `MAP` / `R 4` / `UNMAP` records against BAR0 at
> `0x81000000`. It sits in the project root on the Windows side and in
> `~/Downloads` on the laptop. Postmortem of the eight attempts is in
> `HANDOFF.md` §6.
>
> **Trace analysis is DONE (2026-08-07).** The capture has been parsed,
> collapsed, and written up as `PHASE4-GSP-BRINGUP.md` — the full GSP bringup
> sequence, register by register, every name resolved against the vendored
> NVIDIA headers. Three things came out of it that were not known before:
>
> 1. **A second falcon exists in the trace.** The parser had `psec` at
>    `0x114000`; `dev_sec_pri.h:27` says `0x843FFF:0x840000`. 25,003 accesses
>    — an entire SEC2 boot — were filed as `unmapped`. Fixed, and asserted in
>    `test_falcon.cpp` so the guess cannot come back.
> 2. **The WPR2 assumption was wrong.** `WPR2_ADDR_HI` is read exactly once in
>    14.6 s and returns `0`. The 197 ms gap in GSP bringup is 37,328 `CPUCTL`
>    reads — a busy-poll with a real exit condition, not a handshake.
> 3. **FBIF makes physical-address DMA legal.** SEC2 writes
>    `FBIF_TRANSCFG(0) = 0x115` and `FBIF_CTL = 0x190` before its DMA. That is
>    `ALLOW_PHYS_NO_CTX` — no context DMA object required, so no framebuffer
>    allocator required. This lowers the Phase 5 wall that looked hardest.
>
> `test_falcon` now runs **165 checks, all passing**, every trace-derived
> constant among them. Nothing here needs a macOS compiler; it is all
> host-side and reproducible on the desktop today.
>
> Next action is Step 4 — but note that everything achievable *without* a
> macOS toolchain is now genuinely finished. Step 4 onward is the VM.
>
> The mux check came back **muxless** (`NO VGASWITCHEROO`, and
> `xrandr --listproviders` reported zero providers). Combined with the 3050
> enumerating as PCI class `0302` rather than `0300`, this is conclusive:
> the discrete GPU has no wire to the laptop panel.
>
> **Termination Condition A is met.** macOS will never display through this
> laptop's 3050, regardless of driver quality — and macOS has no Tiger Lake
> framebuffer driver either, so the iGPU cannot scan out for it. See
> `HARDWARE.md` for the full reasoning.
>
> Steps 3 through 7 are still worth doing **if** the goal is GPU driver
> bringup on real hardware. They are wasted effort if the goal was running
> macOS. That decision comes first; `PLAN.md` §5 lays out the two projects.

---

## [x] STEP 1 — Make an Ubuntu USB and boot the laptop from it

**Why first:** this is the step that might end the whole project in ten
minutes. Cheapest possible test. Everything else is wasted effort if this
comes back wrong.

**You need:** a USB stick, 8 GB or bigger, that you don't mind wiping.

1. On the desktop, download Rufus: https://rufus.ie
2. Download Ubuntu Desktop: https://ubuntu.com/download/desktop
   (~6 GB, start it now, it takes a while)

   **Which version?** 24.04 LTS is the safer pick -- two years of "does the
   NVIDIA driver install cleanly on this laptop" traffic behind it. 26.04 LTS
   works too (mmiotrace and debugfs are unchanged), it is just newer, so a
   `modprobe nvidia` hiccup would be a debugging detour unrelated to this
   project. If you already downloaded 26.04, use it -- do not re-download
   6 GB over this. If nvidia fails to load there, we fall back to nouveau,
   which traces fine for our purposes.
3. Open Rufus, pick your USB stick, pick the Ubuntu .iso, click START.
   Accept whatever it asks. Takes ~10 minutes.
4. Plug the USB into the **laptop**. Reboot it and hit the boot menu key
   during startup — usually F12, sometimes F9, F10, or Esc depending on brand.
5. Pick the USB from the list.
6. When Ubuntu loads, choose **"Try Ubuntu"** — NOT install. Nothing touches
   your Windows install.

### How much laptop disk space does this need?

**None. Zero. No partition at all.** The live USB boots entirely from the stick
and runs its filesystem in RAM. Your 100 GB of free space is untouched and
irrelevant to this step.

### Can I run Linux in a VM on the laptop instead?

**No, and this is the one substitution that cannot work.** The entire point of
booting Linux on the laptop is to let the real NVIDIA driver talk to the real
3050 while `mmiotrace` records every register access. A VM guest has no access
to the physical GPU — there is no device there to trace. You would capture an
empty log.

GPU passthrough does not rescue this either: you cannot pass through the GPU
in a muxless laptop, because the host needs it, and on most consumer laptops
the IOMMU grouping will not allow it regardless.

Same answer for WSL2, VirtualBox, VMware, and Hyper-V — all of them are guests
with no path to the physical GPU.

**Native VHD boot is a different case and worth being precise about.** Booting
an OS from a `.vhdx` file on the internal disk *is* real hardware — no
hypervisor, full device access, `mmiotrace` would work fine. The problem is
that native VHD boot is a Windows bootloader feature. Linux has no equivalent;
you would be hand-rolling a loopback root filesystem with a custom initramfs
and GRUB config. That is an afternoon of bootloader work to arrive at exactly
what a live USB gives you in ten minutes with zero risk to the existing boot
configuration. Not impossible, just strictly worse.

### One thing that does need space: where the trace log goes

The live session's filesystem lives in RAM, and an `mmiotrace` capture can run
to hundreds of megabytes. Writing it to the live filesystem risks filling RAM
and killing the session. In Step 3 we will write it to a mounted disk instead
— either your Windows partition or the USB stick. Budget **2 GB** of free
space wherever it lands. That is the only storage this project asks of the
laptop.

**Stop here.** Tell me when you're looking at the Ubuntu desktop.

---

## [ ] STEP 2 — Identify the card and answer the mux question

Once you're at the Ubuntu desktop, open a terminal (Ctrl+Alt+T) and run these
in order. Copy the output of each back to me.

### 2a. Mount debugfs FIRST

```sh
sudo mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
sudo ls /sys/kernel/debug/ | head
```

Note `debugfs` appears **twice** in the mount line: once as the filesystem type
after `-t`, once as the dummy device name. Omitting the second one makes mount
fail silently.

Note also the `sudo` on the `ls`. `/sys/kernel/debug` is `0700 root:root`, so an
unprivileged listing returns "Permission denied" even when the mount succeeded.
That is normal. Every access below needs `sudo` for the same reason.

**This must come before 2c.** On a live session `/sys/kernel/debug` is often
not mounted, and an unmounted debugfs makes *every* file under it look absent
— including `vgaswitcheroo`. Skipping this step turns a muxed laptop into a
false "muxless" verdict, which is the single most consequential wrong answer
this project can get.

(If debugfs was already mounted, the command prints an error to /dev/null and
still says `mounted`. That is fine.)

### 2b. Which die is actually in the laptop

```sh
lspci -nn | grep -i nvidia
```

You want the bracketed pair, e.g. `[10de:25a2]`. Paste me the whole line and
I'll run it through `tools/identify-gpu.py` to confirm GA107 vs. the Ada
rebadge. See `DEVICE-IDS.md` for why this is not a formality.

### 2c. The mux question — Termination Condition A

```sh
sudo sh -c 'cat /sys/kernel/debug/vgaswitcheroo/switch 2>/dev/null || echo "NO VGASWITCHEROO"'
```

The `sudo sh -c` wrapper matters. An earlier version of this command ran `ls`
without `sudo`, which fails on the `0700` permissions and falls through to the
`||` branch — printing `NO VGASWITCHEROO` and fabricating a muxless verdict out
of a permissions error. `sudo` on the outer shell means the fallback only fires
when the file genuinely is not there.

- Output showing `switch` contents → the laptop is **muxed**. The 3050 can
  drive the panel directly. Project continues.
- `NO VGASWITCHEROO` → **muxless**, assuming 2a succeeded. The 3050 has no
  wire to the panel, and no driver quality fixes that. See `PLAN.md` §4.

**Prior expectation:** `lspci` already reported the 3050 as PCI class `0302`
(3D controller) rather than `0300` (VGA controller), which means firmware is
declaring it has no display outputs. That is the textbook muxless signature, so
expect `NO VGASWITCHEROO` here. See `HARDWARE.md`.

### 2d. Cross-check the mux answer

```sh
xrandr --listproviders
```

Two providers where only the Intel one has `outputs`, and the NVIDIA one shows
`Sink Offload` / `Source Output`, is the classic muxless signature. This is a
second, independent read on 2c — worth having, because 2c can lie if debugfs
isn't mounted and 2d cannot.

**Stop here.** Paste me 2b, 2c, and 2d.

---

## [x] STEP 3 — Capture the register trace  ✅ DONE 2026-08-06 22:41 UTC

> **Captured. 357,495 lines.** `mmio-boot.log.gz` (1,712,423 bytes, `gzip -t`
> clean) and `capture-summary.txt` are in the project root on the Windows side
> and in `~/Downloads` on the laptop. The run reported `card is unclaimed`,
> `mmiotrace active, tracing_on=1`, `driver : nvidia`, and a bound driver on
> `01:00.0`. The log opens with `VERSION 20070824` and real `MAP` / `R 4` /
> `UNMAP` records against BAR0 at `0x81000000` — a genuine cold init.
>
> **What finally worked:** the blacklist alone was never enough.
> `gpu-manager.service` (Ubuntu's `ubuntu-drivers-common`) calls
> `/sbin/modprobe nvidia` by exact module name, which ignores
> `modprobe.blacklist=` entirely. `prep-blacklist.sh` now disables it along with
> the five `nvidia-*` units. Full postmortem in `HANDOFF.md` §6.
>
> Nothing below needs running again. Next step is `tools/parse-mmiotrace.py`
> against the log.

### 3-zero. Optional but strongly recommended: turn on SSH first

Every capture attempt so far has cost a full round-trip — run the script, read
the output on the laptop screen, retype it into chat, copy a revised script
back. Seven attempts, seven round-trips. Doing this once removes that loop
entirely:

```sh
sudo sh enable-ssh.sh
```

It installs `openssh-server`, authorises the Windows key, allows key-only login
(no passwords, no root), starts the service, and prints the exact
`ssh user@ip` command to run on the Windows side. Safe to re-run. Revoke with
`sudo sh enable-ssh.sh --undo` when the project is finished.

It also prints — but deliberately does **not** install — the NOPASSWD sudoers
line. Without it, `sudo sh capture.sh` over SSH stalls waiting for a password
prompt that has no terminal to appear in. Installing it is a real security
reduction and is your call, not the script's.

### 3-one. Run the capture

**Just run `capture.sh`.** Everything below the reference heading is what the
script does internally; you do not need to type any of it.

First, the one-time prerequisite — the driver must not be loaded, and the only
safe way to get there is a blacklisted boot:

```sh
sudo sh prep-blacklist.sh
sudo reboot
```

Do **not** run `prep-blacklist.sh --undo` first. It overwrites its own config
every run, so there is nothing to clear, and `--undo` removes the blacklist —
which puts you straight back to a booted-with-driver machine. Then:

```sh
cd ~/Downloads                       # or wherever this folder is
sudo sh capture.sh
```

The script prints PASS or FAIL at every step and writes `mmio-boot.log.gz` plus
`capture-summary.txt` next to itself — i.e. straight onto the Windows disk, not
into the live session's RAM filesystem. Reboot to Windows and both files are
already there.

If any line says FAIL, stop and send me `capture-summary.txt`. A partial capture
looks like a real one, which is why the script refuses to finish quietly.

---

### Reference: what the script does, step by step

Only needed if something fails and you want to run it by hand. Stay in the same
live session — do not reboot in between.

This capture is the free replacement for a PCIe bus analyzer. It records every
MMIO read and write the working NVIDIA driver performs while bringing the card
up, in order. That log becomes the Phase 4 specification.

### 3a. Find somewhere real to write the log

The live session's filesystem is in RAM. A trace can run to hundreds of MB, and
filling RAM kills the session. Write to a real disk.

```sh
lsblk -f
```

Find your Windows partition (type `ntfs`) or the USB stick, then mount it:

```sh
sudo mkdir -p /mnt/trace
sudo mount /dev/nvme0n1p3 /mnt/trace    # <-- replace with YOUR device
df -h /mnt/trace
```

You want 2 GB+ free on whatever `df` reports. If the NTFS mount comes up
read-only, use the USB stick instead — it is already mounted somewhere under
`/media/ubuntu/`.

### 3b. Make sure debugfs is mounted

```sh
sudo mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
ls /sys/kernel/debug/tracing/current_tracer && echo "debugfs OK"
```

If `debugfs OK` does not print, stop and tell me — nothing below will work.

### 3c. Confirm mmiotrace exists in this kernel

```sh
cat /sys/kernel/debug/tracing/available_tracers
```

`mmiotrace` must appear in that list. It is compiled into stock Ubuntu kernels,
but confirm rather than assume — if it is absent the capture silently records
nothing and you find out tomorrow instead of now.

### 3d. Confirm the card is unclaimed — do NOT unload the driver

```sh
lsmod | grep -E 'nvidia|nouveau' || echo "no NVIDIA driver loaded -- good"
```

The trace has to capture the driver's *initialisation*, so the card must start
unclaimed. **Do not run `modprobe -r` to get there, and do not stop the display
manager.** On this muxless laptop both hard-lock the machine — `nvidia_drm`
owns KMS and nothing is left to hand the console back to. Confirmed the hard
way three times; see the failure table in `HANDOFF.md`.

The only route that works is to boot with the modules blacklisted so they are
never loaded at all. `prep-blacklist.sh` installs that blacklist to disk, so
there is nothing to type at the GRUB menu:

```sh
sudo sh prep-blacklist.sh
sudo reboot
```

The desktop comes up on the Intel iGPU; the 3050 sits untouched. Confirm with
the `lsmod` line above, then run `sudo sh capture.sh`.

When the whole project is finished, put the driver back:

```sh
sudo sh prep-blacklist.sh --undo
sudo reboot
```

This also produces a *better* trace: it is the one genuine cold init the
firmware performs per power cycle, so nothing gets skipped mid-sequence.

<details>
<summary>Manual alternative, if you would rather not write to the installed system</summary>

1. Reboot. Hold **Shift** (BIOS) or tap **Esc** (UEFI) for the GRUB menu.
2. Highlight the Ubuntu entry, press **`e`**.
3. Append to the line starting `linux`:
   ```
   modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm,nouveau
   ```
4. **Ctrl+X** to boot. The edit is not saved, so it has to be retyped on every
   boot — which is exactly why the helper script exists.

</details>

### 3e. Trace, load, stop

Run this as one block:

```sh
echo mmiotrace | sudo tee /sys/kernel/debug/tracing/current_tracer
sudo sh -c 'cat /sys/kernel/debug/tracing/trace_pipe > /mnt/trace/mmio-boot.log' &
sleep 2
sudo modprobe nvidia || sudo modprobe nouveau
sleep 15
sudo pkill -f trace_pipe
echo nop | sudo tee /sys/kernel/debug/tracing/current_tracer
sync
ls -lh /mnt/trace/mmio-boot.log
```

`modprobe nvidia` may fail on a live session if the proprietary driver is not
installed — that is what the `|| sudo modprobe nouveau` fallback is for.
Nouveau traces fine for our purposes; it just will not exercise the GSP path as
thoroughly.

### 3f. Sanity-check the capture BEFORE rebooting

```sh
wc -l /mnt/trace/mmio-boot.log
head -20 /mnt/trace/mmio-boot.log
grep -c 'MMIO' /mnt/trace/mmio-boot.log
```

A real capture is tens of thousands of lines minimum. Under a few hundred means
something did not attach, and redoing it while you are still booted costs
minutes instead of another whole USB session.

Then unmount cleanly so Windows does not complain:

```sh
sudo umount /mnt/trace
```

**Stop here.** Copy `mmio-boot.log` into the project folder on the desktop and
tell me the line count. I will write the parser for it while you move on to
Step 4.

---

## [x] STEP 4 — Install VMware on the desktop

> **ALREADY DONE — verified 2026-08-07.** VMware Workstation **17.6.4.24832109**
> is installed at `C:\Program Files (x86)\VMware\VMware Workstation\`, and
> `vmrun.exe` (version 1.17.0, build-24832109) works. No Broadcom download
> needed. Item 1 below is satisfied.
>
> **Item 3 (recovery image) is also DONE** — `C:\Users\Kentl\macos-vm\sonoma\BaseSystem.dmg`,
> 884,317,790 bytes, all 85 chunklist SHA-256 chunks verified byte-exact. Note
> that `fetch-macOS-v2.py --action verify` crashes on Windows with
> `[WinError 6] The handle is invalid`; that is a bug in the script's verify
> path, not a bad image. Verify the chunklist directly instead.
>
> **What the image actually is: macOS 15.4.1 Sequoia, build 24E263 — not
> Sonoma.** Read from the embedded `SystemVersion.plist` in the raw image; the
> string "Install macOS Sonoma" appears **zero** times in it. The `sonoma\`
> directory, the vmdk and the VM's `displayName` are still *named* Sonoma and
> are deliberately left that way so nothing's paths break — they are filenames,
> not version claims. Downstream this means the KDK and Xcode must match
> **15.4.1 / 24E263**, and guest tools need `darwin.iso`, not `darwinPre15.iso`.
>
> **Item 2 (unlocker) is DONE — it was run, and it is still REQUIRED.**
> v4.2.8 from `C:\Users\Kentl\macos-vm\unlocker428\` has patched
> `vmware-vmx.exe` / `vmwarebase.dll`; `check.exe` must report `Patched (1)`.
> This is not optional and it is not only about the guest-OS picker in the New
> VM wizard: an unpatched `vmware-vmx.exe` **refuses to power on** a VM whose
> `.vmx` sets a `darwin*` guest, however that `.vmx` was authored.
> Hand-writing the file skips the wizard, not the patch.
>
> **It reverts on any VMware update.** If a VM that used to power on suddenly
> will not, re-run `check.exe` before debugging anything else. See
> `VM-BRINGUP.md` failure 1.
>
> **Laptop is NOT a viable host for this VM.** Checked 2026-08-07: i5-11400H
> with VT-x and `/dev/kvm` present, 15 GB RAM — both fine — but only ~40 GB free
> on `/`, a single Ubuntu root with no second partition. Xcode alone is ~40 GB.
> The VM must live on the desktop.
>
> **The desktop's real blocker is disk, not software.** `C:` 191 GB free (90%
> used), `D:` 86 GB free (91% used). Budget ~120 GB for the guest + Xcode + KDK.
> An external USB 3 drive is an acceptable home for the VM if freeing space
> in-place is impractical.

This one has no dependency on the laptop, so you can do it any time — even
while the Ubuntu ISO is still downloading. It is the longest step in wall-clock
time and the least demanding in attention.

1. **VMware Workstation Pro** — free for personal use, Broadcom account
   required. https://www.broadcom.com/ → search "Workstation Pro" → downloads.
   The account signup is tedious but free.
2. Install it, then **fully close it** and stop its services. The next step
   fails if anything VMware-related is running.
3. **DrDonk's unlocker** — https://github.com/DrDonk/unlocker → Releases →
   download the Windows zip. Extract, right-click `win-install.cmd`, Run as
   administrator.

   VMware hides the macOS guest type by default; this is what reveals it.
4. Reopen VMware. Click New Virtual Machine. If **Apple Mac OS X** appears in
   the guest OS list, the unlocker worked. If it does not, the unlocker ran
   while VMware was still resident — close everything and run it again.

**Check:** "Apple Mac OS X" is selectable as a guest OS.

> **Step 4 above (the New Virtual Machine wizard) was not used** — the `.vmx`
> was hand-written instead, with `guestOS` set directly. It was authored as
> `darwin23-64` (macOS 14) and **corrected to `darwin24-64` on 2026-08-07** once
> the guest was confirmed to be macOS 15.4.1 Sequoia; backup at
> `Sonoma.vmx.bak-20260807`. That
> skips the wizard, **not** the unlocker: step 3 is still mandatory, because an
> unpatched `vmware-vmx.exe` refuses to power on a `darwin*` guest no matter how
> the `.vmx` was produced. `VM-BRINGUP.md` has the working `.vmx` in full, with
> the PCI-bridge and SATA-port notes that took two crashes each to find.

---

## [~] STEP 5 — Get macOS installed in the VM

> **PARTLY DONE — 2026-08-07. Read `VM-BRINGUP.md` before touching this step;
> it records four consecutive power-on failures and the fix for each, none of
> which produced an error message that named the real cause.**
>
> **Done:** the recovery image is fetched and verified
> (`BaseSystem.dmg`, 884,317,790 bytes, all 85 chunklist chunks byte-exact);
> it is converted to raw (`BaseSystem.img`, 3,221,110,784 bytes) because VMware
> cannot read a *compressed* UDIF and silently presents it as `capacity=0`;
> the `.vmx` is hand-written with `guestOS = "darwin24-64"` (macOS 15 — it was
> `darwin23-64` until the guest was identified; needs the unlocker patch in
> place either way, see Step 4); and the VM **renders the macOS Recovery GUI**,
> verified 2026-08-07 by screen capture: the Recovery/File/Edit/Utilities menu
> bar, "Reinstall macOS Sequoia", Disk Utility, Safari and Continue. The
> language picker was on screen earlier in the same boot.
>
> **The guest is live and interactive.** The user clicked around in Recovery and
> it responds normally.
>
> **RETRACTED 2026-08-07 — an earlier revision of this step said the guest
> FREEZES here, blamed `Monitor Mode: ULM`, and told you to run
> `bcdedit /set hypervisorlaunchtype off` and disable Memory Integrity / Core
> Isolation. Do not do that.** It would have disabled Windows security features
> to fix a problem that does not exist. The reasoning is recorded below because
> it was *wrong in an inviting way* and will be re-derived by the next person
> otherwise:
>
> - **"0 changed pixels in the guest rect over 8 s"** is exactly what an idle
>   **live** Recovery menu looks like. macOS Recovery has no clock, no spinner
>   and no animation, so idle-but-running is pixel-identical frame to frame.
>   A static framebuffer is not evidence of a halted guest; it is evidence of a
>   static screen, which is the normal state of that screen.
> - **`Monitor Mode: ULM` is not an error.** It is what VMware logs whenever the
>   Windows hypervisor is present, which is the default on Windows 11. It
>   correlates with nothing here.
>
> Both signals were measured correctly. Neither supported the conclusion drawn
> from them. **The only way to answer "is the guest running?" is to interact
> with it** — click something and see if it responds. Pixel diffs answer a
> different, narrower question.
>
> **Check the VM's state with a command, not with your eyes.** On 2026-08-07
> "does the installer reach the macOS UI?" was answered five times in a row with
> screenshots that turned out to be *the host desktop* — right window size,
> right title, plausible dark pixels, caught only when OCR read back the
> operator's own shell prompt. `tools/vm-screenshot.py` uses `PrintWindow`
> rather than `CopyFromScreen`, so occlusion cannot contaminate the capture:
>
> ```sh
> python tools/vm-screenshot.py --window "Sonoma - VMware" \
>     --expect "Reinstall macOS" --expect "Disk Utility"   # exit 0 = on screen
> python tools/vm-screenshot.py --window "Sonoma - VMware" \
>     --settle 8 --compare motion.png                      # frame-to-frame diff
> ```
>
> **`--compare` is not the before/after instrument.** It captures *both* frames
> itself, *after* the action, so it structurally cannot attribute a change to
> anything you did — it answers "is anything moving right now", nothing else.
> Its argument is a **base name**: the frames land on `motion.frame1.png` and
> `motion.frame2.png`, and `motion.png` itself is never written. (Until
> 2026-08-07 it saved frame 2 straight over the file you named, destroying any
> baseline parked there; that is fixed in `tools/vm-screenshot.py` and guarded
> by `tests/test_vm_screenshot.py::DerivedFramePathTests`. The no-attribution
> limit below is *not* fixed — it is inherent.) To show that an action changed
> the guest framebuffer, save two captures around the action and diff them with
> the read-only differ:
>
> ```sh
> python tools/vm-screenshot.py --window "Sonoma - VMware" --out before.png
> #   ... perform the action (inject input, click, advance the boot step) ...
> python tools/vm-screenshot.py --window "Sonoma - VMware" --out after.png
> python tools/png-delta.py before.png after.png --floor <pct-from-a-fresh-control>
> ```
>
> `png-delta.py` never captures. `--floor` is a **percentage** — the guest delta
> measured from an idle control pair — and the run exits non-zero unless it
> exceeds that floor. Byte-identical inputs exit 3 rather than reporting 0%.
>
> **Do not paste a floor value from this repo's history.** The `0.02` /
> `0.000%` floors previously quoted here came from control pairs that were
> byte-identical (one frame counted twice — see the retraction block in
> `handoff3.md`), so **no valid idle floor exists on record for this guest.**
> Measure a fresh control — capture, wait, capture, **no action between** —
> confirm the two md5s differ, and use that number.
>
> The first form turns "it reached the UI" into an exit code, and it is the one
> worth trusting. **`--compare` does not answer "is the guest alive"** — that
> was the mistake made here on 2026-08-07. An idle live screen and a halted one
> produce the same diff. Use it to detect *motion*, and treat the absence of
> motion as no information at all.
>
> **`--compare` diffs the guest box, not the whole window** (since 2026-08-07),
> printing both figures — `guest 0.000%, whole window 0.055%` — with a note when
> the only moving pixels are VMware's chrome. That fix is still correct and
> worth having: it stops chrome ticks reading as guest activity. It just does
> not turn a static frame into proof of anything.
>
> It reports `changed: unknown (window was resized between frames)` if the
> window changed size mid-run; that is no result at all — re-run without
> touching the window.
>
> **The guest is macOS 15.4.1 Sequoia (build 24E263), not Sonoma** — read from
> the image's own `SystemVersion.plist`. Directory and vmdk names still say
> "sonoma"; those are filenames, left alone on purpose.
>
> **Not done:** nothing is installed. Erasing the target disk to APFS and
> running the installer needs a human at the GUI — `vmrun` cannot drive it, and
> the guest is sitting at Recovery waiting for exactly that. Do not mark this
> step complete until macOS actually boots from the target disk.
>
> **The next action, precisely:** in Recovery → Disk Utility, turn on
> **View → Show All Devices**, select the **~100 GB** device (*not* the ~3 GB
> one — that is `BaseSystem.vmdk`, the installer you are booted from), erase it
> as **APFS** with scheme **GUID Partition Map**, quit, then *Reinstall macOS
> Sequoia*. Item 4 below has the reasoning.
>
> **The four failures, in order** (detail in `VM-BRINGUP.md`): missing
> `smc.version`; a compressed dmg attached directly (`capacity=0` →
> `msg.Backdoor.OsNotFound`); installer and blank target disks on swapped SATA
> ports so EFI enumerated but booted nothing; and a Windows path case collision
> (`...\macos-vm\Sonoma` and `...\macos-vm\sonoma` are the **same** directory —
> the relative `.vmx` paths work only by accident, so do not "tidy" them into
> plain filenames without retesting).

### If Recovery says "An Internet connection is required"

**This is where the project currently sits.** *Reinstall macOS Sequoia* in
Recovery is a **network installer**: the 3 GB `BaseSystem.img` you booted from
contains the recovery environment, **not** the ~15 GB of OS payload. Pressing
Continue makes the guest fetch that payload from Apple, and if the guest has no
working route to the internet it stops there.

> **On this VM the erase already happened — do not redo it.** An earlier
> revision of this section said the failure lands "before Disk Utility, before
> any disk is touched." That is the *general* shape of the wall, and it is wrong
> here: Disk Utility ran successfully on 2026-08-07 (13:28–13:29) and the
> **~100 GB target is already erased and APFS-formatted**, before the network
> wall was reached. Item 4 below is therefore **done** on this VM. Re-erasing
> costs nothing but time; assuming it still needs doing wastes a session
> looking for a step that is already complete.

So there are two separate problems, and it is worth being clear which one you
are solving:

| Problem | Fix |
|---|---|
| The guest has no network | Diagnose it — start with `ifconfig -a`, below |
| You do not *want* to depend on Apple's servers | The offline path below |

The network fix is the better outcome and is being worked on separately. The
offline path exists so that a network failure is a **detour, not a wall**.

#### CONFIRMED CAUSE — the recovery image has no Intel ethernet driver

**Established 2026-08-07 from `BaseSystem.img`, host-side, without touching the
VM.** The 15.4.1 / 24E263 recovery kernelcache contains **no Intel ethernet
kext of any kind**, while the VM is configured with
`ethernet0.virtualDev = "e1000e"`. So `en0` never comes up, no `DHCPDISCOVER`
is ever sent, and Recovery reports "An Internet connection is required".

This is an enumeration, not an absence argument. The complete prelinked
ethernet/network kext list is `AppleUSBEthernet`, `AppleUSBEthernetHost`,
`AppleUSBNetworking`, `IONetworkingFamily` and its PlugIns
`AppleBCM5701Ethernet`, `AppleEthernetAquantiaAqtion` (+`Firmware`),
**`AppleVmxnet3Ethernet`**, `IOEthernetAVBController`,
`mDNSOffloadUserClient`, plus `IOUserEthernet`. Every prelinked path containing
"Intel" is graphics / LPSS / MCE / PCH — **zero networking**. Controls were run
both ways: `AppleUSBEHCI`, `IOPCIFamily`, `AppleAHCIPort`, `Sequoia`, `24E263`
all present; two invented kext names absent.

**`vmxnet3` will actually bind.** `AppleVmxnet3Ethernet` declares
`IOPCIPrimaryMatch = 0x07b015ad&0xffffffff` — `15ad` is VMware's vendor ID,
`07b0` the vmxnet3 device ID. The driver in this exact image matches the exact
device VMware presents; it is not merely that the kext exists.

The four host-side observations that pointed here, each killing a *different*
popular explanation:

| Observed | Rules out |
|---|---|
| No DHCP lease written this boot (the lease on file predates power-on) | "DHCP handed out a bad address" |
| Guest MAC `00:0c:29:e7:9f:2b` never appears in the host ARP table | "routing/NAT is misconfigured" |
| VMnet8 byte counters flat across three 30-second samples | "packets leave but come back wrong" |
| Every Apple endpoint reachable from the host through the same tunnel | "something upstream blocks Apple" |

Routing, DNS, clock skew and MTU all require a guest that is **already
talking**. These four describe a guest emitting nothing.

#### Applying the fix — four steps, two of them hazards

1. **Set `ethernet0.virtualDev = "vmxnet3"`** (was `e1000e`).
2. **Delete the `ethernet0.pciSlotNumber` line outright.** That value was chosen
   for an e1000e device, and the VMX's own comment block records **two**
   power-on crashes (`0xc0000005`, `[msg.pci.noslotavail]`) caused by this exact
   field being wrong for the device. Let VMware reassign it — it writes a
   correct value back at power-on. Leave `generatedAddress` /
   `generatedAddressOffset` alone; only the slot is device-dependent.
3. **Fix the Network comment in the VMX — it is backwards, and it is what caused
   this blocker.** It reads "e1000e is the only NIC macOS has a built-in driver
   for. vmxnet3 needs VMware Tools". For macOS 15 the opposite holds: the
   recovery kernelcache ships `AppleVmxnet3Ethernet` prelinked and ships no
   Intel ethernet kext at all. vmxnet3 needs Tools on Windows and Linux guests,
   not on macOS — Apple bundles the driver. Replace the comment or the next
   reader swaps back.
4. **Full power cycle, not a guest reboot.** The `.vmx` is read once at power-on
   (`VM-BRINGUP.md` § Three traps). Land the `guestOS = "darwin24-64"`
   correction in the same cycle — the running instance is still `darwin23-64`
   even though the file says otherwise.

**Acceptance test, falsifiable both ways.** Note the power-on timestamp from
`vmware.log` line 1, then check `C:/ProgramData/VMware/vmnetdhcp.leases` for a
lease whose `starts` timestamp is **later** than that power-on. A new lease
closes this. **If `en0` appears and there is still no lease, this diagnosis is
dead** and Mullvad's kill-switch returns to the front.

> **STATE 2026-08-07 19:41Z: the fix is APPLIED, RUNNING, AND PASSED. All four
> criteria are MET.** Supersedes two earlier notes here — one saying the VM had
> never been restarted, one saying only the outcome was untested. Both were
> accurate when written and were overtaken: first by the power cycle, then by
> the acceptance test below.
>
> The VM was power-cycled and the swap reached the running instance. This is a
> **new** `vmware.log` — line 1 is `2026-08-07T18:51:31.356Z`, `pid=22944`, not
> the old `16:36:53Z` / `pid=30264`:
>
> ```
> 305  DICT              guestOS = "darwin24-64"
> 340  DICT ethernet0.virtualDev = "vmxnet3"
> 418  Powering on guestOS 'darwin24-64' using the configuration for 'darwin24-64'
> 687  Ethernet0 MAC Address: 00:0c:29:e7:9f:2b
> ```
>
> Criterion 1 holds and the edit is live in the **running** instance — `vmrun
> list` still reports this `Sonoma.vmx` up under `pid=22944`.
>
> **Criteria 2–4 are closed by positive, post-egress evidence** — the guest
> emitting frames, not the absence of a failure:
>
> | Criterion | Result |
> |---|---|
> | Applied NIC is `vmxnet3` | yes — `DICT ethernet0.virtualDev`, new log |
> | Fresh lease after power-on | **yes** — `starts 18:52:35`, 64 s after `18:51:31` |
> | Guest identifies itself | **yes** — lease carries `client-hostname "Mac"` |
> | Guest on the wire, sustained | **yes** — host ARP `192.168.255.129` → `00-0c-29-e7-9f-2b` dynamic |
>
> **The stack stayed live, not just booted.** `vmnetdhcp.leases` holds *five*
> successive leases to `00:0c:29:e7:9f:2b` — 18:52:35Z, 19:07:34Z, 19:26:09Z,
> 19:40:13Z, 19:41:17Z. The 19:07 lease carried a two-hour term and the guest
> re-DHCPed at 19:26 anyway, so `en0` was bound and renewing across ~50 minutes.
> A single boot-time lease could be a fluke; a renewal chain cannot.
>
> The no-result-vs-negative-result caution that governed this check is now
> discharged, and it is worth recording *why the check was sound*: an unchanged
> `vmnetdhcp.leases` would have proven nothing, because the operator drives the
> installer GUI by hand. Only the **positive** artifacts settle it — a lease with
> `starts` later than 18:51:31Z, and a host ARP entry the host could only have
> learned from real frames on VMnet8. Both landed. Under `e1000e` neither ever
> did: no `en0`, no `DHCPDISCOVER`, no ARP entry across 87 minutes.
>
> **The falsifier did not trigger.** The stated kill condition was *“`en0` exists
> but still no lease → this diagnosis is dead and Mullvad returns to the front”*.
> The lease landed, so Mullvad stays eliminated **as the cause of this failure**
> and is retained only as a second-order suspect (tunnel MTU 1380 vs VMnet8 1500)
> for large transfers during the download.
>
> **PCI slot.** VMware wrote `ethernet0.pciSlotNumber = "160"` back at line 228
> — the same number as the deleted pin, but chosen *for a vmxnet3 device*. The
> VM powered on with no `[msg.pci.noslotavail]` and no `0xc0000005`. Deleting
> the pin was still correct; an identical value does not mean it was harmless.

**Mullvad is eliminated as the cause of *this* failure — by measurement, not
argument.** An egress block predicts frames that leave the guest and die
upstream, but ARP and `DHCPDISCOVER` are L2 broadcasts on VMnet8 that never
touch the tunnel or the default route. A VPN cannot produce a flat `BytesIn`
counter or a missing ARP entry; both are *pre-egress*. It stays on the list as a
**post-swap** suspect and becomes live again the moment `en0` exists, because
the tunnel does own the default route at metric 1 with MTU 1380.

**Do not disconnect Mullvad to test it now** — it changes host state, buys
nothing diagnostically, and if the swap then worked nobody could say which
change did it.

Per-hop evidence is in `NETWORK-DIAGNOSIS.md`; the full kext enumeration is in
`VM-BRINGUP.md`.

#### The offline path, and its one hard precondition

**Precondition, non-negotiable: the offline installer must match the recovery
environment — macOS 15.4.1, build 24E263.** The recovery image was confirmed as
24E263 by reading `SystemVersion.plist` out of the raw image; a full installer
for any other build will refuse or produce a system that does not match what
every other doc here describes. Getting this wrong swaps one blocker for a worse
one, because the failure surfaces late.

**Verdict from the host-side viability check (2026-08-07): the path is viable,
but it needs a ~15 GB download nobody has made yet, and the published recipe is
written for Ventura and must be retargeted.** What follows separates what was
actually proven from what is still assumed.

#### Three things that are settled, and save you the wrong first move

**1. There is no full installer on disk, and re-running the fetcher will not
produce one.** `macos-vm` (depth 4) and `Downloads` were searched for
`InstallAssistant*`, `Install*macOS*`, `*.pkg`, `*.sparseimage` and
`SharedSupport*` — zero hits. Only the recovery set is present:
`BaseSystem.dmg` (884,317,790 B), `BaseSystem.img` (3,221,110,784 B),
`BaseSystem.chunklist` (3,352 B, `CNKL`, 85 chunks).

`fetch-macOS-v2.py` **cannot** fetch a full installer. It only POSTs to
`osrecovery.apple.com/InstallationPayload/RecoveryImage`; its own source points
at `corpnewt/gibMacOS` for full installers. "Just re-run the fetcher" is the
obvious first idea and it is wrong.

**2. The recovery image is not secretly an installer.** Searching the raw image
finds `InstallAssistant` (221 hits), `SharedSupport.dmg` (4) and
`Install macOS Sequoia.app` (4) — which looks promising and is not. Dumping the
context around all three shows they are **code and plist references**: a literal
`/Applications/Install macOS Sequoia.app` beside `com.apple.InstallAssistant`,
and a launch plist with `BundlePath` `/Install macOS Sequoia.app` and
`-RunInOSIEnv`. That is the recovery environment *describing an installer it
expects to find*, not carrying one. 3 GB cannot hold a 15 GB `SharedSupport`
anyway — the arithmetic alone settles it.

**3. The ISO step is solved, and there is a tool for it.** `run_offline.md`
calls for `mkisofs`. This host has **none** of `mkisofs`, `genisoimage`,
`xorriso` or `oscdimg` — the Windows ADK installed here ships only DISM and
Licensing, with no `Oscdimg` directory. `tools/build-installassistant-iso.py`
replaces that step with `pycdlib` (1.20.0, verified by a real build-and-readback):

```sh
python tools/build-installassistant-iso.py --self-test
python tools/build-installassistant-iso.py InstallAssistant.pkg -o InstallAssistant.iso
python tools/build-installassistant-iso.py --verify InstallAssistant.iso
```

> **Use the tool rather than calling `pycdlib` by hand.** It sets the volume
> identifier to exactly `InstallAssistant` — `run_offline.sh` hardcodes
> `/Volumes/InstallAssistant`, so any other label mounts somewhere the script
> will not look, and that is a silent failure. `--verify` reads the ISO back and
> checks the label; `--self-test` proves the toolchain works before you spend
> ~15 GB of download on it.
>
> Minor trap if you do work with `pycdlib` directly: it exposes no
> `__version__`; use `pip show pycdlib`. `tools/README.md` has the full section.

#### The path

1. **Download `InstallAssistant.pkg` for Sequoia 15.4.1 / 24E263** on the host,
   via `corpnewt/gibMacOS` — **not** `fetch-macOS-v2.py`. ~15 GB. **This has not
   been done.**
2. **Wrap it in an ISO** —
   `python tools/build-installassistant-iso.py InstallAssistant.pkg -o InstallAssistant.iso`.
   Run `--self-test` first; it proves the toolchain before the download, not
   after.
3. **Retarget `scripts/run_offline.sh` before running it.** It hardcodes
   `Install macOS Ventura.app` **twice**; both occurrences must become
   `Install macOS Sequoia.app` or it fails as written. What it does: copies
   `InstallAssistant.pkg` to `Contents/SharedSupport/SharedSupport.dmg` inside
   the `.app`, then runs `InstallAssistant_springboard` directly.
4. **Attach the ISO** as an additional `sata0:*` device in the `.vmx`.
5. **In Recovery, erase the target first** — Disk Utility,
   `View → Show All Devices`, the **~100 GB** device, APFS / GUID (item 4 below)
   — then run the installer from the attached volume instead of pressing
   *Reinstall macOS*.

> **Steps 1 and 3 are UNVERIFIED.** Nobody has downloaded the ~15 GB package or
> run the retargeted script in the guest. What was proven is the **tooling and
> the shape of the fix**, not the fix. Steps 2 and 4–5 rest on things that were
> actually executed (a real `pycdlib` build-and-readback, and the disk
> instructions already in this file).
>
> **Remember the power-cycle rule** before adding the disk at step 4: `.vmx`
> settings are read once, at power-on. Shut the guest down, edit, power on —
> editing a running VM changes nothing and VMware may discard the edit during
> shutdown. See `VM-BRINGUP.md` § Three traps.
>
> Steps 1–2 below are the original hand-written route and are superseded by the
> above; step 3's `smc.version = "0"` line is still required and still correct.

1. Grab the installer. Easiest path without owning a Mac is `OSX-KVM`'s
   fetcher, which pulls the official recovery image straight from Apple:

   ```sh
   git clone --depth 1 https://github.com/kholia/OSX-KVM
   cd OSX-KVM
   python fetch-macOS-v2.py
   ```

   The original intent was **Sonoma (14)** — the last release comfortable for
   third-party kext work, since later versions tightened kext loading further
   for no benefit to us.

   > **What was actually fetched is macOS 15.4.1 Sequoia, build 24E263.**
   > Confirmed 2026-08-07 from the embedded `SystemVersion.plist`, not from the
   > menu selection. Recovery offers "Reinstall macOS Sequoia". If you re-fetch
   > and genuinely want 14, check the resulting image rather than trusting the
   > picker — and expect the kext-loading friction this step was written to
   > avoid.

2. New VM in VMware, guest type **Apple Mac OS X**, version macOS 14.
   (The VM in use skipped this wizard entirely — the `.vmx` was hand-written
   with `guestOS = "darwin24-64"`, matching the macOS 15.4.1 guest that was
   actually fetched. See `VM-BRINGUP.md`.)

3. Settings that matter:
   - **4 vCPU (2 cores × 2 sockets), 8 GB RAM** — what the working VM actually
     runs (`Sonoma.vmx`: `numvcpus = "4"`, `cpuid.coresPerSocket = "2"`,
     `memsize = "8192"`), verified 2026-08-07. An earlier draft said "8+ vCPU,
     16 GB+ RAM"; nothing was ever built or tested at those numbers. More is
     fine on a 16-CPU / 32 GB host, but 4/8 is what boots and runs.
   - 100 GB+ disk
   - Firmware **UEFI**, Secure Boot **off**
   - Close VMware, open the VM's `.vmx` file in Notepad, add this line:
     ```
     smc.version = "0"
     ```
     Without it macOS refuses to boot. Save, reopen VMware.

4. Boot the recovery image → Disk Utility → erase the **target** disk as
   **APFS** → quit → Install macOS.

   > **Pick the disk by size, and check twice.** Two devices are attached and
   > Disk Utility will happily erase either:
   >
   > | Size | What it is | Do |
   > |---|---|---|
   > | **~3 GB** | `BaseSystem.vmdk` — the recovery image you booted from | **leave alone** |
   > | **~100 GB** | the blank target `.vmdk` | **erase this one** |
   >
   > Erasing the 3 GB device destroys the installer you are currently running
   > from. Recovery does not stop you, and the failure surfaces later as an
   > install that cannot find its source.
   >
   > In Disk Utility, set **View → Show All Devices** first. Without it the
   > sidebar lists *volumes*, not devices, and erasing a volume leaves the old
   > partition scheme in place. Select the top-level ~100 GB **device**, then
   > Erase with:
   >
   > - **Format:** APFS
   > - **Scheme:** GUID Partition Map
   >
   > Scheme only appears when a device is selected — if you cannot see it, you
   > have a volume selected and are about to do the wrong thing.

**Check:** macOS desktop boots and `uname -a` in Terminal reports Darwin.

> No GPU passthrough here, and that is fine. Steps 6–7 are code and
> disassembly. Real hardware only matters from Phase 4 onward, and that
> happens on the laptop.

---

## [ ] STEP 6 — Unlock the VM and install the toolchain

### 6a. Disable SIP

A stock macOS install will not load an unsigned kext. Reboot the VM into
Recovery, open Terminal from the Utilities menu:

```sh
csrutil disable
csrutil authenticated-root disable
```

Back in the booted system:

```sh
sudo nvram boot-args="-v keepsyms=1 debug=0x144 kext-dev-mode=1"
sudo reboot
```

**Check:** `csrutil status` reports disabled.

### 6b. Toolchain

1. **Xcode** from the Mac App Store, then `xcode-select --install`.
2. **Xcode Additional Tools** — developer.apple.com/download/all, search
   "Additional Tools". Free Apple ID. You want **IORegistryExplorer** out of
   this; you will live in it.
3. **Kernel Debug Kit** — same page, search "Kernel Debug Kit". The build
   number must match `sw_vers -buildVersion` **exactly** or lldb rejects the
   symbols.

The $99 developer account is not required for any of this.

**Check:** `sw_vers -buildVersion` matches the KDK you downloaded.

---

## [ ] STEP 7 — First build of the kext

Copy `phase0-kext/` into the VM, then:

```sh
cd phase0-kext
make check      # preflight: SDK, SIP state, boot-args, KDK
make
```

**The `make` half of this step is DONE as of 2026-08-14.** It ran in the macOS
VMware guest and came back `make exit status: 0` with the bundle PRESENT, under
Apple clang version 17.0.0 (clang-1700.0.13.5) on macOS 15.7.9 (24G830). The log
is `step7-results/step7-build-SUCCESS.log`. So the two things that paragraph
named as unproven are now proven: the kernel build flags work, and the real Apple
KPI signatures do match the stubs in `tests/kextshim/` closely enough to compile
and link.

The checkbox above stays unticked because the *milestone* below — deliberately
panic, catch it in the debugger — has not been attempted. Building is not
loading.

The useful finding is what stopped the first attempt, because it was none of the
risks this step anticipated. A fresh macOS install ships **no compiler at all**:
`xcode-select` errored, `Kernel.framework` was ABSENT, and `make` exited 1 having
printed only the installer prompt. That run is kept as
`step7-results/step7-build.log`; the contrast between it and the success log is
the evidence. The missing step was `xcode-select --install`, and no amount of
perfecting how the source got into the guest would have supplied it.

For the record, what the parse gate had already ruled out before any of this:
as of 2026-08-07 `tests\check-kext-syntax.bat` runs the clang inside Visual
Studio 2022 over the kext against a stub IOKit tree and comes back clean, so
typos, bad overrides, wrong argument counts and format-string mismatches were
never candidates.

Then the Phase 0 milestone — deliberately panic and catch it:

```sh
make clean && make PANIC_TEST=1 && make install load
```

A symbolicated backtrace naming `NV3050Probe::start` from the debugger means
two-machine debugging works and Phase 0 is genuinely done.

**Note:** the VM has no NVIDIA card, so the kext will build and load but never
match anything. That is expected. Matching is tested on the laptop later.

---

## Where things actually stand

Done and verified on the desktop, needing nothing from you:

- `PLAN.md` — phases, revised estimates, termination conditions
- `DEVICE-IDS.md` — which die is which, corrected twice against NVIDIA's table
- `PHASE1-GSP-NOTES.md` — GSP boot sequence transcribed from real source
- `phase0-kext/` — Phase 2 + 3 + 3.5, all register constants verified
- `tools/identify-gpu.py` — resolves any NVIDIA device ID to its die
- `tools/check-registers.py` — 113/113 constants match NVIDIA's headers (measured 2026-08-11; this line said 102/102 before `NVGspBoot.hpp` and `NVGspStage.hpp` were covered)
- `tools/check-structors.py` — metaclass declare/define pairing, 1/1
- `tools/parse-mmiotrace.py` — ready for your Step 3 log
- `tests/kextshim/` — clang parses the kext clean on Windows, 5/5 stages

Unproven: the kext has never been **loaded**. It parses on Windows and, as of
2026-08-14, it **builds and links** against Apple's real KPI in the macOS guest
(`step7-results/step7-build-SUCCESS.log`). What is left of Step 7 is the Phase 0
milestone above — `kextload` plus a caught panic — which needs SIP lowered and a
reboot and has not been attempted.
