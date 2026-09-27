# Tools

Python scripts, effectively no dependencies, run on Windows/macOS/Linux (one is
Windows-only and says so). Each exists because a specific class of mistake
already cost this project time.

---

## `identify-gpu.py` — which die is actually in the laptop

```sh
python identify-gpu.py 25a2
python identify-gpu.py 10de:25a2
python identify-gpu.py "$(lspci -nn | grep -i nvidia)"
python identify-gpu.py --list-3050
```

"RTX 3050 Laptop GPU" is a marketing name covering **GA107, GA106, AD106, and
AD107** across two architectures. Which one you have decides the register
headers, the GSP firmware, and the SASS ISA. This resolves a PCI device ID to
the real die and prints the exact `NV_PMC_BOOT_0` / `BOOT_42` values the Phase 3
kext should read back.

It also prints the retargeting checklist if you land on an Ada part.

**Caveat it prints for you:** `--list-3050` is a substring match on marketing
names, so it also catches datacenter parts that ship a vGPU profile named after
a GeForce card — `0x2237` is an A10G, not a 3050. Only `0x25xx` and `0x28xx` are
real laptop hardware.

---

## `find-kexts-in-image.py` — what drivers a macOS image actually ships

```sh
python find-kexts-in-image.py BaseSystem.img
python find-kexts-in-image.py BaseSystem.img --filter Intel
python find-kexts-in-image.py BaseSystem.img --filter Graphics
```

A byte scan, not a filesystem walk. It reads the raw image in overlapping
chunks and pulls out every ASCII run matching a kext path or a
`com.apple.driver.*` bundle identifier, so it sees strings inside the prelinked
kernelcache's info plist as well as on-disk names.

It exists because `VM-BRINGUP.md` §"CONFIRMED CAUSE" summarised an Intel-driver
walk in prose without ever writing down the list. The summary turned out to be
accurate — but a remembered impression cannot be re-checked, and the specific
question that mattered later (which *graphics* generations ship) was not
answerable from it.

**The line to read first is `scan reach`.** A compressed kernelcache hides its
strings from a raw scan, so a miss can mean "not present" or "cannot see
anything at all". If the reach count is zero the tool says `VACUOUS` and every
absence below it proves nothing. Non-zero reach is what makes a miss evidence.

The second caveat is scope: a recovery image is a trimmed subset of a full
install, so absence here is strong evidence about the installed volume, never
proof.

---

## `capture-igpu-facts.sh` — the four facts measured from the laptop itself

```sh
sudo sh capture-igpu-facts.sh
```

Runs on an **installed** Ubuntu on the target laptop, read-only. Unlike
`capture.sh` it wants the NVIDIA driver *loaded*, not blacklisted — several of
its checks are meaningless unless both drivers are bound.

It exists because `HARDWARE.md`'s muxless verdict rests on evidence gathered
from a live USB session that reported `Providers: number : 0`. Zero providers
means *neither* GPU registered one, because that session was running on a
generic EFI framebuffer with no accelerated driver bound. A zero produced by a
subsystem that was not running is not a measurement of the mux — it is a
measurement of the live USB.

That matters more than it sounds: Termination Condition A in `PLAN.md` ends the
project, and it currently rests on that absence.

Section 4 is the one to read. It resolves every DRM connector to its owning PCI
device, so the eDP panel's owner is established **positively** rather than by
absence. Section 2b is the control the original run lacked — if `i915` is not
loaded, the script says so and tells you not to read sections 3 and 4 as
hardware facts.

It also captures the iGPU's PCI ID, which no artifact in this tree has ever
recorded, and dumps the 3050 VBIOS while it is there.

---

## `read-igpu-mmio.py` — read a live Tiger Lake display engine

```sh
sudo python3 read-igpu-mmio.py --pci 0000:00:02.0
```

`--out` redirects the dump to a file of your choosing; without it everything
goes to stdout. The filename is not spelled here on purpose — an output name
in a fenced block is indistinguishable, to `test_documented_paths`, from an
input path that must already exist in the checkout, and it fails the gate the
moment the doc is written rather than when anything is actually wrong.

mmaps BAR0 through `/sys/bus/pci/devices/<addr>/resource0` and dumps bounded
display-engine ranges. The panel stays lit while it runs, so every value is a
register state that demonstrably drives this exact screen — which is stronger
evidence than a datasheet default.

**The ranges are bounded on purpose, and "all of BAR0" would be wrong.** BAR0 is
16 MiB and mostly the GT block, which sits behind FORCEWAKE: reading it from
userspace returns garbage, logs unclaimed-register errors, or wedges the read.
The ranges here are display-power-well only, which is on whenever the panel is.

It reads a word at a time rather than slicing the whole range in one go. A bulk
slice is a `memcpy`, and glibc is free to satisfy that with 16- or 32-byte
vector loads; MMIO wants naturally aligned 32-bit accesses, and a wide or split
load can return a word neither half of which was ever simultaneously present.

**Read the `REACH` line first.** An all-`0xFFFFFFFF` range means the read never
reached the device, and an all-zero range means that often enough to be
untrustworthy. If fewer than 16 distinct non-trivial values were seen the tool
prints `VACUOUS` and nothing below it may be recorded as an observed constant —
the same guard, for the same reason, as `find-kexts-in-image.py`'s `scan reach`.

---

## `read-igpu-mmio.sh` — the same dump with no interpreter

```sh
sudo sh read-igpu-mmio.sh --pci 0000:00:02.0
```

Same ranges, same output format, same `REACH` guard as the Python twin, built
from `dd`, `od` and `awk` only. It exists because an interpreter should not be a
precondition for a capture that is otherwise pure coreutils.

**It tries three access paths and names the one that worked.** A shell has no
`mmap`, so it must `read()` a memory BAR, and that is not universally
supported:

1. `read()` on the device's `resource0`. Memory BARs always support `mmap`;
   `read()` support is kernel-version dependent.
2. `read()` on `/dev/mem` at the BAR's physical base. Expected to fail while
   i915 is bound — `CONFIG_IO_STRICT_DEVMEM` refuses ranges a driver has
   claimed — but the attempt is cheap.
3. `intel_reg` from intel-gpu-tools, if installed. It dumps by register *name*
   rather than by address range, so its output is offered as a clearly-labelled
   separate artifact and must not be diffed offset-for-offset against a raw
   range dump.

Which path produced a file is recorded in the `path` line, because a dump from a
path that short-reads is not the same artifact as one from a path that didn't.
Each range's byte count is checked against what was asked for; a short read is
reported as a named FAIL for that range instead of being padded into the dump.

Two details that would otherwise lose data silently: `od` runs with `-v`, since
without it repeated lines collapse to `*` and a page of identical register
values — normal and meaningful — would vanish along with its contribution to
`REACH`. And the byte total is summed from a file rather than a variable set
inside the `while` loop, because that loop is the right-hand side of a pipe and
runs in a subshell where the assignment does not survive.

`tests/test_igpu_mmio_ranges.py` fails if this file's range table, the Python
twin's, and the third copy embedded in `igpu-capture.sh` ever disagree. Hand-kept
copies of a register map have exactly one failure mode and it is silent: add a
range to one, and the others keep printing the same header, the same `REACH`
line and the same `OK` while omitting a block entirely.

---

## `capture-igpu-display.sh` — the whole Tiger Lake capture in one run

```sh
sudo sh capture-igpu-display.sh
```

Read-only, safe on the machine you are looking at. Wraps `read-igpu-mmio.py`
and collects the rest of what a framebuffer driver would have to reproduce:
BAR layout, i915's own debugfs view of the active display, the eDP panel's
modes and EDID, and the DMC firmware the display engine loads. Writes a
timestamped directory and names it at the end.

Section 3 is what makes section 2 interpretable. A raw dump saying the pipe is
1920x1080 is one reading of some offsets; i915's `i915_display_info` saying the
same is an independent path to the same answer. Constants confirmed twice are
the ones worth writing down.

Section 0 refuses to let a bad capture masquerade as data: not root, no driver
bound, or a `modprobe.blacklist` still in `/proc/cmdline` each produce a named
FAIL rather than a directory full of `0xFFFFFFFF` that reads like small numbers.

---

## `igpu-capture.sh` — the same capture as one file and one command

```sh
sudo sh igpu-capture.sh
```

No arguments, no second file to copy, and it tars its own output at the end.
This is the one that actually ran on the target laptop.

It exists because `capture-igpu-display.sh` reads the MMIO dumper from a
separate file, and the 2026-08-15 15:50 UTC run died on exactly that: the
operator copied the orchestrator alone, so the centrepiece artifact was never
produced and the failure only surfaced after five sections of work. A capture
that depends on a second file is a capture that can arrive half-copied. The
dumper is therefore inlined as a `python3` heredoc rather than sourced.

**Python is not optional here, and that is a kernel fact rather than a
preference.** A memory BAR is reachable from userspace only through `mmap` —
`drivers/pci/pci-sysfs.c` wires `read()`/`write()` on sysfs `resourceN` for
`IORESOURCE_IO` BARs only. A shell cannot `mmap`, so the coreutils twin has no
way in on this device no matter which kernel is running. `read-igpu-mmio.sh`
remains useful on hardware that exposes an I/O BAR; it was never going to work
here.

**Section 2 reports kernel lockdown before section 3 reads anything.** On this
laptop every one of the fifteen ranges returned `EPERM` while running as root,
which looks like a permissions problem and is not one:
`security_locked_down(LOCKDOWN_PCI_ACCESS)` is called at the top of the BAR
`mmap` handler, before any mapping is attempted, and the integrity band is
enough to trip it. Root does not bypass it. Ordering the lockdown report first
is what turns that `EPERM` into an attributed result instead of a guess — and
the dumper prints `BLOCKED:` rather than `VACUOUS:` when the EPERM count is
nonzero, because "the kernel refused" and "the mapping reached nothing" are
different findings that would otherwise share a verdict word.

Three dead ends are named in the script itself so they are not re-attempted:
`intel_reg` reaches the BAR through the same sysfs path and hits the same check;
a shell dumper is blocked by the `IORESOURCE_IO` restriction regardless of
lockdown; and `mokutil --disable-validation` turns off shim's MOK validation
while lockdown keys off the firmware Secure Boot state, which that does not
touch. Clearing it needs Secure Boot disabled in firmware and a reboot.

When the MMIO is blocked, section 4's i915 debugfs artifacts still carry the
driver's own decoded view of the active display, which is enough to proceed.
That is a second-hand reading rather than the silicon, so anything derived from
it is marked as such and never as `OBSERVED`.

---

## `igpu-decode.sh` — turn a capture tarball into numbers

```sh
sudo sh igpu-decode.sh
```

Reads nothing from the hardware. It opens the newest capture tarball already on
disk and prints roughly fifty lines that fit in a chat window, so a 48 KB
artifact does not have to be transported off the machine to be useful.

This exists because the capture's terminal output carries only the `REACH` line.
The 2026-08-15 18:38 UTC run succeeded — Secure Boot disabled, lockdown read
`[none]`, and the BAR mmap returned 269 distinct non-trivial values across 73728
bytes — but every actual register value stayed inside the MMIO dump member of
the tarball. Decoding on the machine closes that gap without a second hardware
run, which matters when the capture window was fifteen minutes.

(Deliberately described rather than named: `tests/test_documented_paths.py`
cannot tell a filename that exists only inside a generated tarball from a path
in the checkout, so spelling either member name here reddens the gate. It already
did once, on this very paragraph.)

**The raw 32-bit word is printed beside every decode, and that ordering is the
point.** The word is the observation; the decode is this script's reading of it.
The end of the output re-prints i915's own decoded display-info view from the
same tarball, so two independent paths can be compared. If they disagree, the
word wins and the decode is the bug.

Two encoding facts it depends on, both read out of `reference/i915/` rather than
assumed:

- **Every timing field is stored as value − 1.** `intel_display.c` writes
  `HACTIVE(crtc_hdisplay - 1) | HTOTAL(crtc_htotal - 1)` and adds 1 back on
  readback; `PIPESRC` does the same with width and height. A decode that omits
  the `+1` is off by exactly one — a mode still nearly works under that error,
  which is why it is pinned by a fixture rather than trusted.
- **`TRANSCONF` is a pipe-block register indexed by the transcoder.**
  `i915_reg.h` defines it as `_MMIO_PIPE2`, so it sits at `0x70008` plus the pipe
  delta, not at `0x60008` in the transcoder block. `0x60008` is `_TRANS_HSYNC_A`,
  a real register that reads and writes without faulting — the Intel-side
  instance of the hazard `nvGspFalconReg()` and `nvGspRiscvReg()` are kept
  separate for.

The decoder was probed against a synthetic tarball before it was ever run on the
laptop, including a negative control that patches out the `+1` and requires the
fixture to report `1919x1079` instead of `1920x1080`. Without that control every
assertion could have been satisfied by a fixture that does not separate the right
decode from the wrong one.

---

## `igpu-followup.sh` — the second capture, plus a decode of the first

```sh
sudo sh igpu-followup.sh
```

Two jobs in one command, split because only one of them is perishable.

**Part A reads the live display engine** for ranges the fifteen-range table does
not reach. This is the half that needs Secure Boot off, lockdown `[none]`, i915
bound and the panel lit — the exact state of the 18:38 UTC window and nothing
else. **Part B decodes the tarball that run already produced**, which is not
perishable at all and runs here only because it costs nothing.

What Part A adds, and why those three ranges specifically: `XE_D_DISPLAY` — the
Gen12 entry this silicon uses — sets `trans_offsets[]` to A `0x60000`,
B `0x61000`, C `0x62000`, **D `0x63000`**, and `pipe_offsets[]` to A `0x70000`
through **D `0x73000`**. The original table stops at C, so transcoder D and
pipe D were never dumped and their absence from the artifact is a gap in the
table rather than a fact about the hardware. `0x64000` is added for
`DDI_BUF_CTL` and the AUX channels, which is where the eDP link actually
terminates and which no dumped range covers either.

The gap dump is not compared against the main range table by
`tests/test_igpu_mmio_ranges.py`, and deliberately so — a gap table required to
match the table it fills gaps in would have to be empty.

**A sparse result for transcoder and pipe D is the expected reading, not a
failure.** This laptop drives one eDP panel, so D is unclaimed. The script says
so in its own output rather than letting a near-empty range read as a failed
mapping, and the per-range `FAIL` lines are what distinguish the two.
`TRANSCODER_EDP` at `0x6F000` is the same kind of correct null: `XE_D_DISPLAY`
does not list it, because Gen12 dropped the separate eDP transcoder and the
panel runs on a normal transcoder through DDI A.

Part B is a rewrite of `igpu-decode.sh`'s decoder rather than a copy, so it
carries its own probe against the same fixture and the same negative control. If
the two decoders ever disagree on that fixture, one of them is wrong and the
fixture names the field. The probe selects the Part B heredoc by content rather
than by position — there are two `python3` heredocs in the file, and taking the
first would have silently probed the live-mmap half and passed on an untested
decoder.

---

## `check-registers.py` — verify the kext against NVIDIA's headers

```sh
python check-registers.py
python check-registers.py --verbose
```

Parses every register constant out of `phase0-kext/NVDecode.hpp` and diffs it
against the authoritative definitions in
`reference/open-gpu-kernel-modules/.../swref/published/`. Exit code 0 if
everything matches.

**Run this after any edit to `NVDecode.hpp`.** That is the only file that should
ever declare a register — `NV3050Probe.hpp` is the IOKit shell and carries no
constants of its own, so there is exactly one place for a transcription error to
hide, and it is checked. This project has already shipped
two wrong register decodes from hand-transcription — the split `ARCHITECTURE`
field in `NV_PMC_BOOT_0`, and GA107's implementation ID. Both were caught by
accident. A kext reading the wrong offset does not fail loudly; it returns a
plausible number and sends you debugging the wrong thing for a week.

Current status: **113/113 passing, 0 skipped** — measured 2026-08-11 by running the tool. Constants come from `NVDecode.hpp` (Phase 3/3.5), `NVFalcon.hpp` (Phase 4 Falcon/RISC-V/GSP/WPR2), `NVGspBoot.hpp` and `NVGspStage.hpp`, checked against 447 definitions across 10 NVIDIA headers.

The tool prints how many defines it *parsed* per header — 53, 177, 7 and 3 — which is not the same number as the 113 it *checks*, because a kext constant is only checked when NVIDIA publishes a counterpart. An earlier revision of this line read "102/102 ... 53 from `NVDecode.hpp` and 146 from `NVFalcon.hpp`": that predates the two GSP headers, and its per-header split is not a figure this tool reports. Run it rather than quoting it.

One of those 10 is `turing/tu102/dev_fb.h` rather than an Ampere header, because **NVIDIA ships no `dev_fb.h` under `ampere/ga102`**. WPR2 is taken from TU102 — the same implementation NVIDIA's own HAL routes GA10x GSP bootstrap to — and corroborated by `hopper/gh100/dev_fb.h`, which carries identical offsets. Two architectures agreeing is evidence; one header would have been a guess.

Beyond per-constant comparison it enforces cross-header invariants NVIDIA never states in one place:

- **`NV_PGSP_BASE` is proven, not assumed.** `dev_falcon_v4.h` gives `MAILBOX0` as engine-relative `0x40`; `dev_gsp.h` independently spells the absolute `0x110040`. Those two headers do not reference each other, so agreement proves the base.
- **`NV_FALCON2_GSP_BASE` sits exactly one 4 KiB block above.** Collapsing the two bases puts every RISC-V access inside the Falcon register file — where every offset is a *valid register*. That does not fault. It reads and writes real hardware at the wrong address, and the symptom is a firmware boot that hangs with no diagnostic.
- **Every translated address stays inside the GSP aperture**, and the aperture fits the 16 MiB BAR0 mapping the kext actually makes.

Its companion is `tests/` — see below. `check-registers.py` proves the constants
match NVIDIA's headers; `tests/test_decode.cpp` proves the code that *uses*
those constants extracts the right bits. Neither catches the other's failures.

It checks three kinds of claim:

- **offsets** — numeric address must match exactly
- **fields** — NVIDIA writes `HI:LO`; the script derives shift and mask and
  compares against the kext's `_SHIFT` / `_MASK` pair
- **values** — architecture and implementation constants

Plus internal sanity checks NVIDIA's headers cannot verify for us: that
`(arch << 4) | impl` really is `0x177` for GA107, that the split-architecture
reassembly widths line up, and that the assembled `BOOT_0` equals the
`0x17700000` printed in the docs.

---

## `parse-mmiotrace.py` — turn the Step 3 capture into a specification

```sh
python parse-mmiotrace.py mmio-boot.log              # summary by engine
python parse-mmiotrace.py mmio-boot.log --first 200  # ordered listing
python parse-mmiotrace.py mmio-boot.log --region pgsp  # just the GSP falcon
```

The raw capture is tens of thousands of lines of hex with no symbol names. This
translates it using the same NVIDIA headers the kext was built from, so you get
a register-by-register account of what the working Linux driver does during
bringup. **That listing is the Phase 4 specification.**

What it does:

- Infers the BAR0 physical base from the trace's `MAP` records (the 16 MiB
  region is the register aperture on all Turing+ parts) and converts absolute
  addresses back to BAR-relative offsets
- Buckets accesses into engine regions — `pgsp` is where Phase 4 lives, `gc6`
  holds `GFW_BOOT` and the framebuffer size
- Resolves **Falcon-relative** offsets. `dev_falcon_v4.h` defines registers
  relative to an engine base, so `0x1100F4` is PGSP (`0x110000`) + `HWCFG2`
  (`0xF4`). Without this every Falcon access annotates blank — exactly the
  region that matters most
- Cross-checks the four registers the kext already reads, printing the values
  the Linux driver saw. **Those are ground truth.** If the kext reads something
  different from the same offset, the kext is wrong

### Testing without hardware

`testdata/synthetic-mmio.log` is a hand-written fixture matching the kernel's
text format. It is not a real capture — it exists so the parser could be
exercised before the laptop was available. It has caught two real defects: a
bogus `pmc-boot` region at `0x00A000` (a misreading of `BOOT_42`'s `0xA00`
offset, which could never have matched anything), and the missing Falcon
aperture resolution.

```sh
python parse-mmiotrace.py testdata/synthetic-mmio.log
```

Expected: BAR0 base `0xF2000000`, 23 accesses, all four kext registers found,
and PGSP accesses naming `MAILBOX0`, `CPUCTL`, `CPUCTL_ALIAS`, `HWCFG2`.

---

## `vm-screenshot.py` — prove what the VM is *actually* showing

**Windows only** (it drives the Win32 window APIs against VMware Workstation).
Needs Pillow; `--ocr`/`--expect` additionally need `pytesseract` plus a
Tesseract binary.

```sh
python vm-screenshot.py --list
python vm-screenshot.py --window "Sonoma - VMware" --out shot.png --crop-chrome
python vm-screenshot.py --window "Sonoma - VMware" --expect "Reinstall macOS" --expect "Disk Utility"
python vm-screenshot.py --window "Sonoma - VMware" --settle 8 --compare motion.png
```

On 2026-08-07, "does the Sonoma installer reach the macOS UI?" was answered
five times in a row with screenshots of **the host desktop**. The captures
looked real — right size, right window title, plausible dark pixels — and were
only caught when OCR read back the operator's own shell prompt.

The cause is `Graphics.CopyFromScreen`: it copies the *monitor* framebuffer
over the region where a window happens to sit, so anything overlapping that
region ends up in the PNG. This tool uses
`PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)` instead — the window renders
*itself* into an offscreen DC, and occlusion stops mattering.

Why the other obvious option doesn't work: `vmrun captureScreen` talks to the
guest and needs `VixVM_LoginInGuest`, which is useless before an OS is
installed — precisely when you most need to see the screen. Everything here is
host-side and works against a VM sitting at a firmware prompt.

What it adds on top of a PNG:

- `--expect PHRASE` (repeatable) OCRs the guest area and exits **0** if OCR read
  every phrase, **1** if it did not read one of them, **2** if the capture itself
  failed. Read exit **1** in one direction only: it means *OCR did not read the
  phrase*, not *the phrase is not on the guest screen*. Tesseract misreads
  antialiased macOS text routinely, so exit 1 is evidence, never proof, and the
  tool's own output now says so on the line after the verdict. Exit **0** is the
  strong direction — OCR cannot invent a phrase that is not there.
- `--ocr` prints the OCR dump and, when VMware chrome strings match but no macOS
  marker does, ends with **`result : INCONCLUSIVE`**. It used to print
  `result : FAIL`, which on 2026-08-07 was read as "the guest is gone" while the
  dump printed twenty lines above it listed macOS Recovery menu items — the
  phrase table simply lacked those spellings. The verdict is a heuristic over the
  dump, and the dump is the evidence: **open the PNG before concluding anything
  about the VM.**
- `--crop-chrome` trims VMware's toolbar and status bar. Under-cropping feeds
  VMware's *own* menu text to OCR and produces convincing nonsense about the
  guest.
- `--compare` takes a second frame after `--settle` and reports whether the
  **guest region** changed. It prints the whole-window figure too, because those
  two numbers disagreeing is itself the finding: on 2026-08-07 a 20 s comparison
  reported `0.055%` changed, and every one of those pixels was a 165×9 strip at
  `y=7` — VMware's own title bar. Diffing the whole window would have read as
  proof of life.

  **`--compare` cannot attribute a change to an action.** Both frames it diffs
  are captured by this invocation, *after* whatever action you just performed,
  so it answers only "is anything moving *right now*". That limit is inherent —
  no fix removes it. For before/after evidence use two `--out` captures and
  `png-delta.py` below.

  Its argument is a **base name**, not an output file: `--compare motion.png`
  writes `motion.frame1.png` and `motion.frame2.png`, and `motion.png` itself is
  never written. Before 2026-08-07 it saved frame 2 straight over the named file,
  which destroyed four saved baselines in `shots/` and produced the retracted
  `0.000%` idle floor. Fixed in `vm-screenshot.py` (`derived_frame_path`) and
  guarded by `tests/test_vm_screenshot.py::DerivedFramePathTests`, which greps
  the tool's source so the old call cannot come back silently.

  **Read the direction of this test carefully.** A changed guest region proves
  the guest is *alive*. An unchanged one proves nothing: macOS Recovery has no
  clock, no spinner and no animation, so an idle **live** Recovery menu is
  pixel-identical frame to frame. On 2026-08-07 a `0.000%` guest delta was read
  as a hung guest, blamed on `Monitor Mode: ULM`, and nearly answered with
  `bcdedit /set hypervisorlaunchtype off` — a host-wide security downgrade for a
  problem that did not exist. The guest was interactive the whole time. See the
  *false alarm* section of `../VM-BRINGUP.md`.
- An ambiguous `--window` pattern is an **error**, not a guess. Silently
  capturing the wrong VM is the same class of mistake as capturing the host.
- **A minimized window is refused, not captured.** `PrintWindow` does *not*
  fail on an iconic window — it succeeds and hands back a taskbar thumbnail.
  On 2026-08-07 that produced a `160×28` PNG reported as a successful capture,
  with OCR returning zero lines: a result that says nothing about the guest but
  reads like it does. The tool now un-minimizes first (`SW_RESTORE`, then a
  repaint pause), re-checks, and refuses anything below a `400×300` floor.
  `--no-restore` skips the un-minimize and fails outright — use it when moving
  the user's windows would be rude.

### Testing without a VM

`tests/test_vm_screenshot.py` covers the pure logic — phrase matching, chrome
cropping, window selection, argument parsing, and the minimized-window guard
(driven through a stub that reports iconic state and hands back pixel bytes).
The raw Win32 calls are not faked; they are six documented API calls, and a
mock would only test the mock.

```sh
tests\run-python-tests.bat
```

---

## `vm-type.py` — the write half of the guest console

**Windows only** (Win32 `SendInput` against the VMware Workstation window). No
third-party packages.

```sh
python vm-type.py --list
python vm-type.py --window "Sonoma" --click
python vm-type.py --window "Sonoma" --text "ifconfig -a" --enter
python vm-type.py --window "Sonoma" --click-at "12,8"
python vm-type.py --window "Sonoma" --key Q --dry-run
```

**Do not use `--key cmd+space`.** `cmd` is the host left-Win key (scancode
`0x5B`), and `Win+Space` is swallowed by the Windows shell as the
input-language hotkey before VMware ever sees it — so Spotlight never opens,
and `vm-type.py` still reports the events as *accepted*. The same trap applies to
`Win+E/R/D/L/Tab`. Reach the guest by clicking a known screen position
(`--click-at`) or by typing into an already-focused field instead.

`vm-screenshot.py` can *read* the guest screen; nothing could *write* to it.
On 2026-08-07 the Sonoma guest reached a macOS desktop with `vmrun
checkToolsState` reporting `unknown` (no VMware Tools, so no
`runProgramInGuest`) and TCP 22/5900/548 all closed — alive, reachable by
ping, and completely undrivable. This is the other half.

Why `SendInput` and not `PostMessage`: VMware's guest view reads raw input and
ignores synthetic `WM_KEYDOWN`. Message-posting *looks* like it works — every
call succeeds — and nothing reaches the guest. Same class of silent wrong
answer `vm-screenshot.py` was written to kill.

Why scancodes and not virtual keys: the guest is macOS and maps scancodes
through its own layout. Sending virtual keys routes punctuation through the
*host* layout and mangles it. Every event here carries `KEYEVENTF_SCANCODE`
and a set 1 make code — what a real PS/2 keyboard would put on the wire.

Safety properties, each one a failure that already happened somewhere:

- **An ambiguous `--window` pattern is refused.** Typing a shell command into
  the wrong VM is worse than any capture bug.
- **A minimized window is refused** (`SW_RESTORE` first), same guard as
  `vm-screenshot.py`.
- **`--dry-run` resolves and prints the full ordered sequence and injects
  nothing.** Use it before anything destructive.
- Exit **2** = could not target the window, **3** = unparseable argument
  (unknown key name, untypable character). An untypable character is an error,
  not a silent substitution.

### Two coordinate bugs, both fixed 2026-08-07 — do not reintroduce them

Both produced *silent* misses: the tool reported the events as accepted, the
click landed somewhere in host space, and the guest framebuffer never changed.
They
are independent of the elevation problem below and were confirmed by
measurement, not reasoning.

**1. Absolute mouse coordinates normalise across the VIRTUAL desktop, not the
primary screen.** `MOUSEEVENTF_ABSOLUTE` maps `0..65535` over
`SM_XVIRTUALSCREEN` / `SM_CXVIRTUALSCREEN` and friends. The original code
divided by `SM_CXSCREEN`/`SM_CYSCREEN`; asking for `1914,545` put the pointer
at `1442,1144`. Subtract the virtual origin too — it is negative when a monitor
sits left of or above the primary.

**2. The guest surface is a CHILD window, not the frame.** `vm-type.py` now
resolves it with `_Win32.guest_view()`, which walks `EnumChildWindows` for class
`VMware.GuestWindow` / `MKSEmbedded` and takes the largest match. On
2026-08-07 the frame was **1228x881 at 1300,85** while the guest view was
**1024x768 at 1502,176** — a fixed `left+2, top+60` chrome offset misses the
guest surface by roughly 200x90 px. `--click-at X,Y` is therefore
guest-relative and is *rejected* (exit 3) when it falls outside the resolved
guest rectangle, rather than clicking host chrome. If no guest child exists
(VM powered off, frame showing the library tab) the tool refuses with exit 2
instead of guessing an offset.

### BLOCKED 2026-08-07 ~20:25Z — injection is not delivering on this host

**`vm-type.py` currently cannot reach the guest, and it will not tell you so.**
Measured independently by two separate terminals (`bold-wren-e3`, then
`bold-wren-29` from a different process) with `tools/_input-block-probe.py`:

```
block: held_by_other=True  (BlockInput(F) ret=False err=5 ACCESS_DENIED)
inject (1972,423) -> (2012,423)  sent=1 err=0  lastinput_advanced=False  DELIVERED=False
VERDICT: injection is NOT delivering                     (exit 1)
```

**CAUSE CORRECTED 2026-08-07 ~20:35Z — this is an integrity barrier, not a
stuck block.** The paragraph that stood here previously said "some thread on the
host holds `BlockInput(TRUE)`, and only that thread can release it", and cited
"two independent processes seeing the identical result" as ruling out a
per-process artifact. **Both claims were wrong.**

`tools/_token-probe.py`, after its `TOKEN_INFORMATION_CLASS` constants were
fixed, reports for the agent shell's Python (pid 688):

```
TokenElevationType  : 3  (Limited (UAC-filtered))
TokenIntegrityLevel : 0x2000  (Medium)
TokenHasRestrictions: 1
TokenUIAccess       : 0
TokenElevation      : 0
```

A Medium-integrity, UAC-filtered token with `UIAccess = 0` is denied
`BlockInput` and cannot `SendInput` into a higher-integrity window **by
design**. `ERROR_ACCESS_DENIED` is the documented return for that posture — it
is *not* evidence that another thread holds the block. VMware Workstation's
window being higher-integrity than us is sufficient on its own to explain
non-delivery.

The "two independent processes" argument was worthless: both terminals launch
from the same agent shell and therefore carry the *same* restricted token. That
is one sample taken twice, not two independent samples, so their agreement said
nothing about the host.

**LIKELY REMEDY, PARTIALLY VERIFIED 2026-08-07 ~20:40Z — run elevated.**

> **Scope correction, ~21:00Z.** This heading previously read "REMEDY PROVEN —
> run elevated. Injection works." **That over-claimed.** What was measured is
> that an elevated process can move the *cursor* (`SetCursorPos`), via
> `_token-probe.py`. Every elevated `vm-type.py` run carried `--dry-run`, which
> injects nothing by definition, so its `exit 0` proved only that argument
> parsing and `--out` work — **not** that a keystroke ever reached the guest.
> "An elevated process can move the cursor" and "`vm-type.py` can type into the
> guest" are different claims; only the first is supported.
>
> Guest keystroke delivery remains **unproven**. Closing that requires an
> elevated, non-dry-run send whose effect is observed *inside the guest*.
>
> **Instrument difference, ~21:05Z.** The elevated evidence below comes from
> `_token-probe.py`, which proves delivery with `SetCursorPos ret=True`.
> `_input-block-probe.py` proves delivery with `SendInput` plus an
> `on_target=True` post-condition, and **`vm-type.py` injects with
> `SendInput`.** Those are different APIs with different gates, so the elevated
> run below does *not* exercise the path `vm-type.py` uses. When quoting an
> elevated success, always name which API produced it.
>
> Corollary for artifacts: an elevated log is only citable for `vm-type.py` if
> its text contains an `on_target=` field — that field is what proves the
> current probe build ran rather than an older one. As of 21:05Z **no artifact
> anywhere in `shots/` contains `on_target=True`.** The three `DELIVERED` lines
> on disk are `on_target=False` in `shots/vm-type-elevated.log` and
> `DELIVERED : False` in `token-shell.txt:10` and `token-sched.txt:10`.

**`shots/vm-type-elevated.log` is a scratch buffer, not evidence.**
`_vm-type-elevated.cmd` truncates it on every run — deliberately, so a stale
success line can never be mistaken for a fresh one. The cost is that a run
destroys the previous log. A 21:00Z log was quoted line-by-line in `#vm-input`
and then overwritten minutes later by a verification run; it survives only
because it was copied first, as `shots/vm-type-elevated-2100Z-NOTELEVATED.log`.
**Copy the log under a dated name before re-running if anyone is arguing from
its contents.**

**Abort-line defect, fixed and verified 2026-08-07 ~21:05Z.** On probe failure
this file used to print the unconditional string `ABORTED: injection is not
delivering even elevated` — including into the 21:00Z log whose own line 3 read
`NOT ELEVATED` (the UAC prompt had been dismissed). A reader trusting the abort
line would have concluded elevation was ruled out by a run that never tested it.
The `net session` check now stores its result in `%ELEV%` and the abort block
reports that measured value plus the correct next action. This is the same class
of error as reading `SendInput`'s return value as delivery: asserting a
condition instead of measuring it.

The same `_token-probe.py` relaunched via `Start-Process
-Verb RunAs` (pid 64748) reports:

```
TokenElevationType  : 2  (Full (elevated))
TokenIntegrityLevel : 0x3000  (High)
TokenHasRestrictions: 0
TokenUIAccess       : 0
SetCursorPos     : ret=True err=0  -> (893, 590)
DELIVERED        : True
VERDICT          : injection WORKS from this process
```

The cursor physically moved — that is cursor delivery, not keystroke delivery to
the guest. Two further details settle the earlier *misdiagnosis of cause*:

- **`TokenUIAccess` is still `0` in the working run.** High integrity alone was
  sufficient; `UIAccess = 1` is not required.
- **`BlockInput(FALSE)` returned `False` with `err=0`** when elevated, versus
  `err=5` at Medium. No `ERROR_ACCESS_DENIED` means there was never a foreign
  `BlockInput(TRUE)` to be denied against — the original "stuck block held by an
  unknown thread" reading is refuted directly, not just explained away.

So a host reboot was the wrong recommendation and would have changed nothing.
To *attempt* driving the guest, relaunch elevated — and verify the effect inside
the guest before believing it:

```powershell
Start-Process -FilePath 'C:\Python314\python.exe' `
  -ArgumentList 'tools\vm-type.py','--window','Sonoma','--text','...' `
  -WorkingDirectory 'D:\nerd shit\other projects\mac os' -Verb RunAs
```

Elevated child processes share no stdout with the launching shell, so pass
`--out <path>` when you need to read the result back. `--out` was added
2026-08-07 — this README documented the flag before it existed.

**Quoting trap, measured 2026-08-07.** `Start-Process -ArgumentList` joins its
elements with spaces and does **not** quote them, so any argument containing a
space is split by the child's own command-line parser. Three elevated runs, same
window, differing only in the `--text` value:

| `--text` element | Result |
|---|---|
| `'echo hi'` | exit 2, **no `--out` file written** |
| `'echohi'` | exit 0, file written, `type   : 'echohi'` |
| `'"echo hi"'` | exit 0, file written, `type   : 'echo hi'` |

`echo hi` arrives at the child as `--text echo` plus a stray positional `hi`;
argparse rejects it and calls `sys.exit(2)` **before** `--out` is wired up, so
the run leaves no file at all.

That asymmetry is the diagnostic: **exit 2 with no `--out` file is an
argument-parsing failure, not a window-targeting failure.** A genuine
window-targeting exit 2 happens after the tee is installed and *does* write the
file. Do not go hunting for a missing window when the file is simply absent.

Quote embedded spaces explicitly:

```powershell
-ArgumentList 'tools\vm-type.py','--window','Sonoma','--text','"echo hi"',`
              '--out','C:\Users\Kentl\macos-vm\sonoma\out.txt'
```

**The trap: `SendInput` returned `sent=1 err=0` while delivering nothing.**
Confirmed a third time on 2026-08-07 by `calm-crane-9a` from a third process,
by two measurements that do not share a failure mode with `_input-block-probe.py`:

| Test | Method | Result |
|---|---|---|
| 20 consecutive sends | inert `VK_LSHIFT` down/up, 0.4 s apart, `Sonoma - VMware Workstation` foreground throughout | **20/20 accepted**, `sent=2/2 err=0` every time |
| same-process observation | `WH_KEYBOARD_LL` hook installed, then one accepted send (`2/2 err=0`, first attempt) | hook observed **zero events** |

An accepted send that never appears in the raw input path is the post-condition
signature of the integrity barrier above: at Medium the call is accepted and
discarded; elevated, it delivers. A `WH_KEYBOARD_LL` hook fires for *injected*
input too (`LLKHF_INJECTED`), so its silence is positive evidence, not absence.

So `vm-type.py` no longer prints `result : injected`. It prints how many event
groups `SendInput` **accepted**, followed by an explicit statement that this is
not delivery. Same failure shape as `vmrun connectNamedDevice` returning exit 0
for a device that does not exist. An API's success return is not evidence of
effect.

**Two measurements that look like evidence here and are not — do not cite
either:**

- **`GetLastInputInfo` not advancing.** That counter is not updated by injected
  input on some Windows builds, so it cannot distinguish delivered from
  discarded. It reads identically in both cases. (`_input-block-probe.py`'s
  `lastinput_advanced=False` field is this measurement — its `DELIVERED=False`
  verdict happened to be right, but not because of that field.)
- **A hook seeing nothing, without checking what was sent.** One run here
  concluded "input is genuinely being discarded" from hook silence while its own
  send had returned `0 of 2 err=5` — nothing was ever sent, so the silence was a
  tautology. **Always print the `SendInput` return alongside the hook count**;
  the conclusion is only valid for a send that was accepted.

Run `python tools/_input-block-probe.py` before trusting any injection, and
`python tools/_token-probe.py` to see whether *this* process is the reason.
While the first says `NOT delivering`, try an elevated shell before concluding
anything about the host. Note this is orthogonal to guest health: the macOS
console is visible and the VM is running — **visibility is not reachability.**

**A note on reading `SetCursorPos`:** the corrected probe run recorded
`ret=False err=0` — a failure return with *no* error code set. Do not read its
`GetLastError()` as meaningful.

### The probe asserts the landing point, not just "something moved"

`_input-block-probe.py` originally scored delivery as `after != start`. That is
the weak form of the same error as reading `SendInput`'s return value: both are
satisfied by outcomes in which the thing you wanted never happened. It shipped a
real false positive on 2026-08-07 — an **unelevated** run printed
`DELIVERED=True` for `target=(1985,682)`, `landed=(1985,718)`. The Y drift is the
tell: an *absolute* injected move cannot land off-axis. The operator had touched
the physical mouse inside the ~0.3 s sample window, and that one reading nearly
overturned the (correct) elevation finding.

Delivery now requires landing **on the target**, within 1 px per axis — absolute
coordinates are quantised through a `0..65535` normalisation, so the exact pixel
can be off by one on some virtual-desktop sizes. The probe reports three states,
not two:

| Output | Meaning |
|---|---|
| `DELIVERED=True` | landed on target — injection works |
| `DELIVERED=False`, cursor unmoved | injection is being discarded |
| `PHYSICAL INPUT DETECTED` | moved but *not* to the target — sample contaminated |

The third case is scored as neither pass nor fail. If you see it, stop touching
the mouse and re-run. A single pass is still not evidence: take two or three
agreeing runs before acting on either verdict. Verified by three consecutive
unelevated passes reporting `on_target=False`, `DELIVERED=False`, exit 1.

**The on-target rule is strict in one direction only — know which.** It cannot
pass a run that did not deliver: the cursor must sit within 1 px of the exact
requested target, and nothing but a delivered absolute move puts it there. It
*can* fail a run that did deliver — if the operator's hand moves the pointer
between the injection and the 0.3 s sample, a genuinely delivered move is scored
`PHYSICAL INPUT DETECTED` and discarded. So `DELIVERED=True` is trustworthy on a
single run; `DELIVERED=False` is only trustworthy across several agreeing runs
with the mouse untouched. Do not invert that: a fresh false *negative* is the
cheap failure here, and reading one as "injection is broken" is how the
elevation finding almost got overturned twice.

**`--key` case is significant on the final part only.** `--key Q` is shift+q;
`--key q` is q. Modifier names and named keys stay case-insensitive, so
`CMD+Enter` works. This mattered: `parse_key` originally lowercased the *whole*
spec, which made its own shifted-face branch unreachable for letters. `--key Q`
resolved to plain `q`, reported `key : Q (0 modifiers)`, and typed the wrong
character into the guest with exit 0 — no feedback channel exists to catch it.
Fixed 2026-08-07 and pinned by test.

### Testing without a VM

`tests/test_vm_type.py` — **59 tests** — covers everything between the command
line and `SendInput`:

- `parse_key`, including the case regression above and the double-shift guard
- `char_to_scan`, and scancode-table consistency (no collisions, no dangling
  shifted faces, arrows carry the E0 prefix)
- `_ordered_actions`, which preserves the CLI interleaving of `--key` and
  `--text` — pressing both keys *then* typing sends the text somewhere else
- `guest_center`, which must clear VMware's chrome strip
- `pick_window` — **ambiguity must raise, never guess.** Also pins that the
  pattern is escaped before compiling, so it is a literal substring match and
  not a regex (`S.noma` does not match `Sonoma`)
- `resolve_click_at` — **out of bounds must raise, never clamp.** Bounds are
  half-open: on a 1000-wide surface, `999` is the last valid x and `1000` is
  rejected

The Win32 layer is not faked; a mock would only test the mock.

`resolve_click_at` was **extracted from `main()` on 2026-08-07 to make that
last line testable.** The bounds check was inline and coupled to a live window
rect, so nothing could reach it. A guard nothing exercises is a guard nobody
can trust. `main()` now catches its `ValueError` and keeps the same exit 3.

Why raising beats clamping: a clamped click still lands somewhere, silently,
and *somewhere* on this host is VMware's own toolbar or another application.
Refusing costs one rerun; clicking host chrome costs a wrong diagnosis.

```sh
tests\run-python-tests.bat
```

Note: `python -m unittest tests.test_vm_type` fails — there is no
`tests/__init__.py`. Use discovery, as `run-python-tests.bat` does:
`python -m unittest discover -s tests`.

---

## `png-delta.py` — the before/after instrument, and the only one

`vm-screenshot.py --compare` cannot answer "did this action change the guest?"
It captures **both** frames itself, after the action — an inherent limit of the
flag, not a bug. (It also used to overwrite the file you named; that *was* a bug
and is fixed — the frames now go to `NAME.frame1.png` / `NAME.frame2.png`.)
`png-delta.py` never captures anything; it only reads two PNGs you already
saved.

```sh
python tools/vm-screenshot.py --window "Sonoma - VMware" --out before.png
#   ... perform the action ...
python tools/vm-screenshot.py --window "Sonoma - VMware" --out after.png
python tools/png-delta.py before.png after.png --floor <pct-from-a-fresh-control>
```

Geometry and byte arithmetic are **imported from `vm-screenshot.py` itself**, so
the percentages are directly comparable to the ones that tool prints, not a
second near-identical implementation.

| Exit | Meaning |
|---|---|
| 0 | delta computed; with `--floor`, it **exceeds** the floor |
| 1 | with `--floor`: delta is at or below the floor — **inconclusive** |
| 2 | could not read a file, or the two sizes differ |
| 3 | BEFORE and AFTER are **byte-identical** — not a measurement at all |

### Exit 3 exists because the duplicate-capture failure already happened

Added 2026-08-07 after a measured incident. Feeding the same bytes twice used to
print `delta : guest 0.000%, whole window 0.000%` and **exit 0** —
indistinguishable from a real null result. `md5sum` over the eight supposed
before/after frames in `shots/` collapsed to **two** hashes:

```
090566eb0ad301a6633f1c6fa1d910fe  floor-a.png floor-b.png floor-c.png floor-d.png
                                  (all four, 185840 bytes)
024e9d456fe7ed850ce3dff37ab58743  mouse-a-20260807T2018Z.png  mouse-b-20260807T2018Z.png
                                  mouse-a2-20260807T2022Z.png mouse-b2-20260807T2022Z.png
                                  applemenu-20260807T2015Z.png  (all five, 82501 bytes)
```

Every "pair" was one frame counted twice, so the "`0.000%` idle floor" and the
"mouse injection changed nothing" results read off them were arithmetic on
duplicate inputs and said **nothing about the guest**. A failure of the *capture
pipeline* was wearing the costume of a fact about the *guest* — the same error
class as reading `SendInput`'s "accepted" as delivery. The tool now hashes both
inputs **before** computing anything and refuses, loudly, with a
capture-pipeline checklist.

`--allow-identical` overrides the refusal. It is for testing this tool itself;
using it to get a number out of a duplicate pair reintroduces the exact defect.

**A `--floor` value is only meaningful if the control pair that produced it also
had differing md5s.** There is currently **no valid idle floor on record** for
this guest — the previous one was retracted (see the retraction block in
`handoff3.md`). Measure a fresh control before quoting a floor.

**Do not trust the `Z` stamps in `shots/` filenames.**
`mouse-a-20260807T2018Z.png` has mtime **20:02:48Z**. Trust mtime and md5, never
the name.

---

## `build-installassistant-iso.py` — the offline-install escape hatch

Needs `pycdlib` (`pip install pycdlib`). Runs anywhere; nothing Windows-specific.

```sh
python build-installassistant-iso.py --self-test
python build-installassistant-iso.py InstallAssistant.pkg -o InstallAssistant.iso
python build-installassistant-iso.py --verify InstallAssistant.iso
```

**`Reinstall macOS Sequoia` is a *network* installer.** The 3 GB
`BaseSystem.img` on `sata0:0` is the recovery *environment*, not the ~15 GB
payload — so a Recovery session without working egress stops at "An Internet
connection is required" and cannot proceed no matter how healthy the VM is.
OSX-KVM's `run_offline.md` documents the way around it: attach a second CD
carrying the full installer package.

> **Before reaching for this, read `../NETWORK-DIAGNOSIS.md`.** The egress
> failure on this VM has a *confirmed* cause — the 15.4.1 / 24E263 recovery
> kernelcache carries no Intel ethernet kext, so `ethernet0.virtualDev =
> "e1000e"` never binds and `en0` never comes up. The fix is a one-line VMX
> change to `vmxnet3`. This script is the escape hatch for when you do not want
> to depend on Apple's servers at all — not the first response to a Recovery
> network failure.

That recipe calls for `mkisofs`, and **this host has no ISO writer at all** —
no `mkisofs`, `genisoimage`, `xorriso` or `oscdimg`; the Windows ADK here ships
only DISM and Licensing, with no `Oscdimg` directory. `pycdlib` is pure Python
and produces an image the recovery environment mounts identically. This script
is that substitute.

Two details that fail *inside the guest*, after a reboot, if you get them wrong:

- **The volume identifier must be exactly `InstallAssistant`.** macOS mounts by
  volume identifier and `run_offline.sh` hardcodes `/Volumes/InstallAssistant`.
  Anything else and the `cp` fails against a path that looks correct in the log.
  That string is the default; `--vol-ident` warns loudly if you override it, and
  `--verify` refuses an image carrying the wrong one.
- **The upstream recipe is Ventura-specific.** It copies
  `/Install macOS Ventura.app` twice by literal path; this project's recovery is
  Sequoia 15.4.1 (24E263). The script writes a retargeted `run_offline.sh` onto
  the ISO (`--app` to change the target, `--no-script` to omit it) so the rename
  is done rather than remembered.

Worth reading the emitted script once: the package is copied *into* the
installer app as `Contents/SharedSupport/SharedSupport.dmg` — renamed, not
extracted — and then `InstallAssistant_springboard` is run directly. That is
what makes it skip the network fetch.

Other guards: the package is **streamed** from disk, not read into memory (a
real payload is ~15 GB); anything under 8 GiB is rejected as a placeholder or
truncated download unless you pass `--allow-small-pkg`; and every build is
read back before it reports success, comparing the on-ISO size against the file
on disk.

### Testing without the payload

`--self-test` builds and reads back a throwaway two-file ISO in a temp
directory, then removes it — it proves the `pycdlib` path works on this host
without the ~15 GB download. `tests/test_build_installassistant_iso.py` covers
the script rendering, the volume identifier, the size guard, and the build/
read-back round trip.

> `pycdlib` exposes **no `__version__`** — `import pycdlib; pycdlib.__version__`
> raises `AttributeError`. Use `pip show pycdlib` to check the version.

---

## `_hfs-catalog-walk.py` — read an Apple disk image without mounting it

Runs anywhere; pure `struct`, no dependencies, opens the image read-only.

```sh
python _hfs-catalog-walk.py                     # defaults to darwin.iso
python _hfs-catalog-walk.py path\to\image.iso
```

Leading underscore because this is a one-question diagnostic, not part of the
bringup path — same convention as `_input-block-probe.py`.

It exists because `darwin.iso` (the VMware Tools image from DrDonk unlocker
4.2.8) **is not ISO9660 despite the extension.** Block 0 carries `0x4552`
(`'ER'`), an Apple Partition Map driver descriptor, which is why
`sata0:2.deviceType = "cdrom-image"` fails with `sense key 0x5 asc 0x24` and the
guest ejects the tray. The fix under test presents it as a plain disk instead
(`DarwinTools.vmdk`, `deviceType = "disk"`). See `../VM-BRINGUP.md` Failure 5.

**The question it answers is narrower than "is this image valid".** "The
partition map parses" only proves the container is Apple-shaped. Whether the
guest gets a *mountable volume*, and whether the installer is *inside it*, are
two further positive claims — so this walks the HFS+ volume header and the
catalog B-tree and names the files. On `darwin.iso` that is `'H+'` v4,
`1637 × 2048 == 3352576` bytes matching the partition exactly, `freeBlocks 0`,
and a root directory containing `Install VMware Tools.app` and
`VMware Tools.pkg`.

Exit status: `0` names at least one file, `1` parsed but recovered no names,
`2` not an APM/HFS+ image at all.

### The offset that made a broken read look like a real answer

The first version printed `catalog totalNodes : 0` — which reads as *"the image
has no catalog"* and would have wrongly discredited a correct fix. It was the
reader. `BTHeaderRec` does not start at the node base; a B-tree node opens with
a 14-byte `BTNodeDescriptor` and the header record follows at `+14`:

| Field | Node base offset |
|---|---|
| `nodeSize` | +32 |
| `maxKeyLength` | +34 |
| **`totalNodes`** | **+36** |
| `freeNodes` | +40 |

Reading `totalNodes` at +40 returns `freeNodes`, and on a full read-only
installer image `freeNodes` is legitimately `0`. **That is the whole trap: the
wrong offset returned a plausible number instead of obvious garbage,** so
nothing in the output flagged it. Same failure class as the retracted TEST E/F
in `../HANDOFF.md` — a reading that cannot distinguish the two outcomes is not
evidence.

The arithmetic that caught it is now printed beside the field on every run:
`logicalSize / nodeSize` (`49152 / 4096 == 12`) must equal `totalNodes`. A
mismatch is a parser bug, not a finding.

Two other deliberate choices, both for the same reason:

- **Every node in the fork is scanned, not `firstLeafNode → fLink`.** A broken
  forward link would silently truncate the listing, and a short listing must not
  be mistaken for a short volume.
- **`node_offset` walks the extent list** instead of assuming one contiguous
  run, so a fragmented catalog is read correctly rather than read wrongly.

Thread records (recordType 3 and 4) are skipped; they repeat names and would
double every entry.

---

## `qcow2-read.py` — random access into a qcow2, because `qemu-img dd` would not

```sh
python qcow2-read.py F:/nv3050-qemu/sonoma-base.qcow2 --info
python qcow2-read.py F:/nv3050-qemu/sonoma-base.qcow2 --lba 1 --length 16 --hex
python qcow2-read.py F:/nv3050-qemu/sonoma-base.qcow2 --lba 40 --length 512 --out ESP.bin
```

`qemu-img dd` served `skip=40` on this image and then refused `skip=409640` on
the same file with `cannot skip to specified offset`. Both times **the exit
status arriving in the shell was 0 and the output file was 0 bytes** — the
diagnosis goes to stderr only. A command that reports success and produces
nothing is this project's recurring failure shape, so the reader was replaced
rather than worked around.

The image was written by `qemu-img convert -c`, so most clusters are raw DEFLATE
streams rather than plain data. A reader that handles only the uncompressed L2
descriptor returns plausible garbage for those clusters instead of failing, so
the compressed branch is implemented and every read reports how many clusters of
each kind it touched. The compressed descriptor's offset/size boundary moves
with `cluster_bits`, and the stream starts mid-sector — reading
`nb_csectors * 512` from the stored offset overruns into the next cluster's
stream.

Cross-checked against ground truth before it was trusted: `0x55aa` at LBA 0
+510, `EFI PART` at LBA 1, and an ESP boot sector byte-identical to the one
`qemu-img` itself extracted.

---

## `vmdk-read.py` — the same, for the 26-extent VMware source

```sh
python vmdk-read.py C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmdk --info
python vmdk-read.py C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmdk --lba 40 --length 512 --out ESP.bin
```

Same `cannot skip to specified offset` failure, same silent exit 0. This one
exists for a specific question: the converted guest's EFI System Partition holds
nothing but a volume label, and the only way to tell real guest state from
conversion damage is to read the same LBA out of the image VMware still boots.

Pass the **descriptor**, not an extent. `Sonoma.vmdk` is text listing 26 `SPARSE`
extents, each with its own `KDMV` header, grain directory and grain tables, so a
guest LBA resolves in three steps — extent, then grain table, then grain. A
reader that skips the extent step silently returns data from the wrong 2 GB of
the disk. Unallocated grains read as zeros, which is correct and is why every
read reports the mapped/unmapped split: a range that reads as zeros because
nothing is mapped there must not be confused with a range that was read and
found empty.

---

## `apfs-volumes.py` — name the volumes, by walking the object map

```sh
python apfs-volumes.py --qcow2 F:/nv3050-qemu/sonoma-base.qcow2 --lba 409640
python apfs-volumes.py --vmdk  C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmdk --lba 409640
```

Written to answer one question: stock edk2 reports `Not Found` for the boot
entry on a disk whose partition table, filesystem and container all read clean.
If a volume with role `PREBOOT` exists, that is the answer — on APFS macOS's
`boot.efi` lives in Preboot, not on the ESP, and firmware with no APFS driver
cannot see it.

Two bugs on the way to that answer, both of which produced confident wrong
answers rather than errors:

- **A linear scan for the `APSB` magic found zero across 512 MiB** while counting
  141 `NXSB` blocks in the same sweep. The positive control fired, so the offset
  was right and the *method* was wrong: APFS names volumes by virtual object id
  and resolves them through a B-tree, so a magic scan returns a confident zero on
  a container full of volumes.
- **Block 0's container superblock is a copy, and it lags.** Measured here: xid
  12589 against xid 15491 for the newest checkpoint, 2902 transactions apart.
  Following the stale pointer landed on a B-tree node and parsed it as an object
  map, yielding a block number larger than the disk. Nothing raised — the only
  tell was an absurd magnitude. So the newest checkpoint is scanned for, and
  every block's `o_type` is checked against the type it is being parsed as.

The volume-name offset was derived from the block's own printable runs rather
than guessed; the first guess returned `'ata'`, a fragment that reads perfectly
well as a strange-but-real volume name.

---

## `vmware-osk.py` — recover the AppleSMC OSK from installed VMware, at runtime

```sh
python vmware-osk.py                 # presence, length, sha256 -- not the value
python vmware-osk.py --print         # the key on stdout, nothing else
python vmware-osk.py --vmx PATH      # a different vmware-vmx binary
```

`isa-applesmc` has an `osk=` property and QEMU ships no default for it, so
`QEMU-LOWSTORAGE.md` §4 recorded the missing key as an open blocker on the boot
path. VMware boots the same guest anyway, because `smc.present = "TRUE"` plus the
unlocked `vmware-vmx` supplies the key internally. This measures whether that
internal copy is readable here, so the blocker stops being a hypothesis.

It is, and the layout is why a first probe looked like a partial hit: the head
fragment matched twice and the tail fragment matched twice at a fixed distance,
but the **middle** fragment matched zero times. NUL and non-ASCII bytes sit
between the halves, so the key is stored as two 32-byte ASCII runs rather than
one 64-byte run, and a probe expecting one contiguous string reports absent
against a binary that plainly has it. Reassembly is bounded by the tail anchor
that follows each head rather than by a fixed span, so a differently-padded build
still resolves instead of silently reading 64 bytes of whatever comes next. Every
occurrence must agree; two that disagreed would mean one is not the key, and
picking the first would be a coin flip filed as a measurement.

The sweep carries six positive controls and all six fire, so an absence here
would have meant absence rather than a scan that cannot see strings. Read the
count off the program, not off this paragraph -- an earlier draft of this section
said "eight positive controls and seven fired" because it was copied from a
docstring rather than from a run, and nothing in this repo checks a number in
prose.

**The value is not in this repository.** The key is Apple's, read out of software
installed on this machine, so committing it would turn a local measurement into
redistribution — it is not in a constant, not in a fixture, not in this sentence.
Bare invocation prints only presence, length and a digest, which is enough to
prove the channel works without putting 64 bytes of Apple's string into a
terminal transcript that may be logged. `--print` is opt-in, for command
substitution so the value never touches disk:

```sh
python qemu-boot-sonoma.py --probe-seconds 90 --osk "$(python vmware-osk.py --print)"
```

This retires a blocker in advance rather than unblocking anything today, but the
reason has changed and the earlier one is now measured false. That paragraph used
to end "the key changes nothing until edk2 can read APFS". edk2 *can* read APFS,
as of `QEMU-LOWSTORAGE.md` §9 — Apple's own `apfs.efi` loads from a vvfat volume,
`connect -r` brings up the Preboot and System handles, and `boot.efi` launches and
prints its own version banner over serial. The OSK is still untested for a
different reason: the boot now stops *inside* Apple's booter, before a kernel
exists to ask the SMC for anything. So the key remains unexercised, and this
section is not evidence that it works — only that it is available.

---

## `qemu-boot-sonoma.py` — boot the QEMU macOS image and capture what it prints

```sh
python qemu-boot-sonoma.py --probe-seconds 90 \
    --osk "$(python vmware-osk.py --print)"

python qemu-boot-sonoma.py --probe-seconds 220 \
    --vvfat F:/nv3050-qemu/vvfat \
    --screenshot qemu-results/apfs-cryptex.png \
    --serial-log qemu-results/apfs-cryptex-serial.log \
    --osk "$(python vmware-osk.py --print)"
```

The first form is the bare boot. The second is the one that got a picture of
Apple's booter running: `--vvfat DIR` attaches a host directory as a FAT volume,
which is how `apfs.efi` and a `startup.nsh` reach the firmware without writing a
single byte into the disk image.

**Read the serial log, not the screenshot.** edk2 mirrors its console to serial,
and `boot.efi` writes its `#[EB|...]` diagnostics there and nowhere else. The
framebuffer shows a cursor and a few lines; the log shows the version banner, the
config table and the failure. `--screenshot` is worth passing anyway because a
0-byte serial log plus a framebuffer that never painted distinguishes "qemu exited
before firmware ran" from "firmware ran and said nothing".

**`--vvfat` attaches with `snapshot=on`, deliberately.** The other two spellings
both look right and are both wrong. `readonly=on` makes an `ide-hd` refuse the
node — qemu dies at device setup with `Block node is read-only`, *before* firmware
runs, so the failure arrives as an empty log rather than as an error you can read
in context. `fat:rw:` is documented as able to corrupt the host directory it
serves, so it is never used here. `snapshot=on` gives the guest a writable overlay
that is discarded on exit: the host directory is safe and the volume still mounts.

**The exit status of a probe does not survive a pipe.** These runs are long enough
that the output usually gets piped through a filter — redacting the OSK, say — and
`$?` after a pipeline is the *last* command's status, not python's. Write the
status to a file inside the braces before the pipe and read the file back.
`QEMU-LOWSTORAGE.md` §9 carries the full measurement this workflow produced,
including the three things it refuted and the one it proved.
