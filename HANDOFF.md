# Handoff — 2026-08-06

Everything achievable without a macOS compiler and without a trace capture is
done and verified. This file is the single place to resume from.

---

## 1. The verdict that reframes the project

**The laptop cannot display macOS through either of its GPUs.** Confirmed from
hardware, not inferred:

| Evidence | Result |
|---|---|
| `lspci -nn` | `01:00.0 3D controller [0302] ... [10de:25a2] (rev a1)` |
| PCI class | `0302` (3D controller), **not** `0300` (VGA) — firmware declares no display outputs |
| `vgaswitcheroo` | absent — no mux to switch |
| `xrandr --listproviders` | `Providers: number : 0` |

The 3050 renders into a buffer the Intel iGPU scans out. macOS has no Optimus
concept, and no Tiger Lake framebuffer driver either (Apple never shipped an
11th-gen Intel Mac). **Termination Condition A + B**, both met. See
`HARDWARE.md`.

Phase 7 (Metal/display) was independently impossible anyway — Apple's Metal
driver plugin ABI is private and undocumented, and there is no Metal IR → SASS
compiler.

**What survives:** Phases 2–5 do not involve the display. This is a GPU bringup
project on real hardware, ending at a card that computes and never draws. That
was an informed decision, not a misunderstanding.

---

## 2. Confirmed hardware

| Property | Value |
|---|---|
| Die | **GA107** (GA107M mobile) |
| Architecture | **Ampere**, arch `0x17`, impl `0x7` |
| Device | `10de:25a2` rev `a1` |
| Chip ID | `0x177` |
| Expected `NV_PMC_BOOT_0` | `0x177000xx` |

**Not** an Ada rebadge — the "RTX 3050 A" parts (`0x2822`/`0x28A3`/`0x28E3`) are
AD106/AD107 and would have retargeted everything. `Info.plist` is narrowed to
the exact device: `IOPCIMatch = 0x25a210de`.

---

## 3. Verification state — all green

```
register constants   113/113   (NVDecode + NVFalcon + NVGspBoot + NVGspStage
                               vs 447 NVIDIA defines, 10 headers)
test_decode           39/39
test_falcon          169/169   (was 97; +68 from the mmiotrace analysis, +4 since)
test_riscv_ucode      85 checks, 0 failures
test_gsp_boot        199 checks, 0 failures
test_gsp_stage       142 checks, 0 failures
host decode total    634 checks across five suites
python gate          356 tests, OK
kext syntax gate      5/5 stages, clang 19.1.5 + tests/kextshim/
structors pairing     1/1       (NV3050Probe : IOService)
negative control      4/4 planted defects diagnosed
Info.plist            valid, IOPCIMatch = 0x25a210de
identify-gpu          exit 0
parse-mmiotrace       exit 0
collapse-sequence     exit 0
capture.sh            sh -n clean, 259 lines
```

The 68 new `test_falcon` checks cover the SEC2 aperture, the boot ROM
parameter block, FBIF, and the exact register values observed in
`mmio-boot.log`. They assert against captured hardware behaviour rather than
against constants this project invented — including negative cases, so a
decoder that ignores its input and returns a constant cannot pass.

Reproduce:

```sh
python tools/check-registers.py     # constants vs NVIDIA headers
tests\build-msvc.bat                 # both host suites via ctest
tests\check-kext-syntax.bat          # clang parses the kext against the shim
```

`build-msvc.bat` works as written. An earlier revision of this file claimed it
"drops into an interactive shell" and blamed a missing MSVC edition. That was
wrong: the failure was the *caller*, not the script. Invoking it from Git Bash
as `cmd /c build-msvc.bat` resolves the relative name against the wrong
directory and lands in an interactive `cmd` prompt with no error. Use an
absolute path, or run it from `cmd` directly:

```sh
cmd //c "D:\nerd shit\other projects\mac os\tests\build-msvc.bat"
```

Verified output: cmake configures, Ninja builds, `ctest` runs both suites.

It also used to print a spurious first line:

```
'vswhere.exe' is not recognized as an internal or external command
```

**Fixed.** `VsDevCmd.bat` (line ~180 of the `Visual Studio\18\BuildTools`
install) `pushd`es into the VS Installer directory and then invokes bare
`vswhere.exe`. The script resets `PATH` to a minimal safe value — deliberately,
see the comment at the top of the file — and that reset removed
`%ProgramFiles(x86)%\Microsoft Visual Studio\Installer`, so the bare name did
not resolve. The warning was harmless (`vcvars64.bat` still set the environment
and the `where cl` guard passed immediately after) but read exactly like a build
failure, which is how the "broken wrapper" story started. That one directory is
now on the minimal `PATH`, with a `REM 3.` note explaining why it must stay.

Verified clean from a deleted `tests/build`: cmake detects MSVC 19.51.36247.0,
Ninja builds 4/4, `ctest` reports `100% tests passed, 0 tests failed out of 2`,
no warning line.

---

## 4. What is built

### Kext — `phase0-kext/`

| File | Role |
|---|---|
| `NVDecode.hpp` | Phase 3/3.5 constants + decoders. Dependency-free. |
| `NVFalcon.hpp` | Phase 4 Falcon/RISC-V/GSP/WPR2. Dependency-free. |
| `NV3050Probe.hpp/.cpp` | IOKit shell. **Declares no constants.** |
| `Info.plist` | Exact GA107 match, probe score 100000 |
| `Makefile` | Kernel build flags, `check`/`install`/`load`/`log` targets |

Implemented phases, **all reads, zero writes**:

- **2** — PCI match, `probe()` filters to display-class functions
- **3** — `NV_PMC_BOOT_0`, `NV_PMC_BOOT_42`, chip ID decode
- **3.5** — `GFW_BOOT_PROGRESS` poll for `0xFF`, usable framebuffer size
- **3.75** — GSP engine inspection → `GSPBootable` verdict in the IORegistry

Phase 3.75 reads `HWCFG2`, `HWCFG`, `FALCON_ENGINE`, both `CPUCTL`s,
`BCR_CTRL`, `PRIV_ERR_STAT`, `HUB_ERR_STAT`, and `WPR2_ADDR_HI`. Go/no-go is:
RISC-V capable, not in engine reset, no latched fault, WPR2 down.

### Tools — `tools/`

| Script | Purpose |
|---|---|
| `identify-gpu.py` | Device ID → die/arch, plus expected register values |
| `check-registers.py` | Diffs kext constants against NVIDIA's headers |
| `parse-mmiotrace.py` | Annotates a capture into the Phase 4 spec. Built and tested against `testdata/synthetic-mmio.log`. |
| `collapse-sequence.py` | Folds a parser listing's consecutive same-register runs into `xN` lines. Without it the 206,293-line PGSP listing is unreadable: ~99% is the DMA loop and the queue poll, both single conceptual steps |
| `dma-summary.py` | Reconstructs the falcon DMA transfers from the raw trace: how many bytes went into IMEM vs DMEM, from what physical base, in how many segments. Also detects **lost records** — see the capture-quality note below |
| `vm-screenshot.py` | Windows only. Captures the VMware guest window via `PrintWindow(PW_RENDERFULLCONTENT)`, so occlusion cannot substitute the host desktop for the guest. `--expect` OCRs a phrase into an exit code |
| `vm-type.py` | Windows only. The **write** half of the guest console: synthesises scancode keystrokes and clicks into the VMware window. Needed because the guest has no VMware Tools, no SSH, no VNC — `vmrun runProgramInGuest` is unavailable |
| `_input-block-probe.py` | Decides from **positive evidence** whether host input injection is usable right now. Prefixed `_` because it is a diagnostic, not part of the pipeline |

**Reading `vm-type.py`'s output correctly is not optional — see §5c.** It reports
how many event groups `SendInput` *accepted*, which is not delivery.

### Tests — `tests/`

MSVC-buildable host tests. `NVDecode.hpp` and `NVFalcon.hpp` are
dependency-free precisely so the tests compile the *identical* files the kext
ships — not copies that can drift.

### Docs

`PLAN.md`, `SETUP.md`, `STEPS.md`, `DEVICE-IDS.md`, `HARDWARE.md`,
`PHASE1-GSP-NOTES.md`, plus per-directory READMEs.

`PHASE4-GSP-BRINGUP.md` is new and is the one to read first for Phase 4. It is
the register-by-register account of what the working Linux driver actually did
to this GPU, derived from `mmio-boot.log` rather than from NVIDIA's source.
Where the two disagree, the trace wins — §4 of that document is an assumption
from NVIDIA's code that the capture disproved.

Derived artifacts in the project root, all regenerable:

| File | From |
|---|---|
| `pgsp-listing.txt` | `parse-mmiotrace.py mmio-boot.log --region pgsp --first 0` |
| `pgsp-sequence.txt` | `collapse-sequence.py pgsp-listing.txt` |
| `psec-listing.txt` | `parse-mmiotrace.py mmio-boot.log --region psec --first 0` |
| `psec-sequence.txt` | `collapse-sequence.py psec-listing.txt` |

---

## 5. Bugs found, and why each mattered

Every one of these was silent — wrong in a way that returns a plausible number
rather than failing. That is why the verifier and host tests exist.

| Bug | Why it was invisible |
|---|---|
| `NV_PMC_BOOT_0` architecture is **split** across bits 28:24 and bit 8 | Reading only 28:24 gives the *right* answer for Ampere, because bit 8 is zero on every Ampere part |
| GA107 impl is `0x07`, not `0x02` | Docs claimed chip ID `0x172` instead of `0x177` |
| "RTX 3050 A" is **Ada**, and `0x2822` is AD106 while `0x28A3`/`0x28E3` are AD107 | Same marketing name, different architecture |
| Three GSP address spaces | RISC-V base is `0x111000`, one page **above** the Falcon block. Collapse them and every access lands on a *valid* Falcon register — no fault, just a hang |
| WPR2 uses **two different shifts** | Extract with 4, rescale with 12. Using 4 both ways makes 2 GiB read as 8 MiB |
| `MEM_SCRUBBING` is **inverted** | Reads `0` when done. Read it naively and you poll forever on a ready Falcon |
| `parse-mmiotrace.py` bogus `pmc-boot` region at `0x00A000` | Misreading of `BOOT_42`'s `0xA00`. Could never match anything |
| Falcon apertures unresolved in the parser | Every GSP-region access annotated blank — the region Phase 4 cares about most |
| Wrapper ran only `test_decode.exe` | `test_falcon` compiled, linked, and silently never ran. Now uses `ctest` |
| `psec` region placed at `0x114000` | `dev_sec_pri.h:27` says `NV_PSEC` is `0x843FFF:0x840000`. `0x114000` is simply "the next 16 KiB after PGSP", which is where you would guess it lives. Result: **25,003 accesses — an entire second falcon boot — filed as `unmapped` and never analysed.** Nothing failed; the summary just showed a large unattributed bucket |
| WPR2 assumed to be part of GSP bringup | Plausible from NVIDIA's source, false in the capture. `WPR2_ADDR_HI` is read **once** in 14.6 s and returns `0`. The 197 ms "handshake" is 37,328 `CPUCTL` reads — a busy-poll |
| DMA ordering read backwards | `PHASE4-GSP-BRINGUP.md` §3.3 first claimed `DMATRFCMD` is configuration written *before* the offsets. It is the trigger, written *after*. The trace is genuinely ambiguous (`118, 114, 11C, ...` reads both ways); `s_dmaTransfer_GA102` in the loaded `nvidia.ko` settles it. Consequence was an off-by-one in the IMEM/DMEM split |
| Transfer byte totals treated as measurements | The capture is **lossy**. 23,555 of 357,495 lines are damaged (22,756 `UNKNOWN` opcodes, 608 spliced records), and the DMA offset runs have holes of 25–26 blocks that each coincide with exactly one spliced line — on *both* engines, 720 ms apart. `mmiotrace` overran its ring buffer. Totals are floors; offset spans are the better size estimate |
| `ga102/gsp/*.bin.zst` assumed to be the loaded firmware | Those are nouveau's. The running driver is `nvidia-595-open`, which embeds its ucode in `nvidia.ko` as raw-deflate `bindata` archives. An apparent 40,192-byte size match against `booter_unload-570.144` was coincidence |
| XML comment with `--` broke `Info.plist` | XML forbids `--` inside comments; plist parse failed with no line number |

---

## 5c. Driving the guest from the host — and why it needs elevation

**This section exists because `handoff2.md` and `tools/README.md` both carry it
and this file did not.** A detail doc that omits what the plan doc asserts reads
as disagreement, and the next session picks whichever it opens first.

### The situation

The macOS guest reached a desktop with **no channel in**:

| Route | State | Consequence |
|---|---|---|
| `vmrun runProgramInGuest` | `checkToolsState` → `unknown` | no VMware Tools, so no guest program execution |
| SSH (22) | closed | no shell |
| VNC (5900) / AFP (548) | closed | no remote framebuffer, no file share |
| ping / host ARP | **replying** | the guest *is* alive — this is not a hung-VM problem |

`vm-screenshot.py` can *read* the screen. `vm-type.py` is the write half, and it
drives the VMware window with Win32 `SendInput` because VMware's guest view
reads raw input and ignores synthetic `WM_KEYDOWN` — message-posting succeeds
and delivers nothing.

### The trap: `SendInput` reports success while discarding every event

**Unelevated, at Medium integrity with a UAC-filtered token, injection does not
reach the guest and nothing in the return values says so.** Measured
2026-08-07 from three separate processes:

| Measurement | Result | Evidence class |
|---|---|---|
| 20 consecutive sends, VMware window foreground throughout | **20/20 accepted**, `sent=2/2 err=0` every time | **pre-egress only** — `shots/fg-inject.log` states in its own text that this is `NOT proof of delivery -- SendInput reports success even when every event is discarded (Medium IL / UAC-filtered)` |
| `WH_KEYBOARD_LL` hook + one accepted send (`2/2 err=0`) | hook observed **zero events** | host-side, but *falsifying*: see the hook argument below |
| elevated run (High IL, `TokenUIAccess` still `0`) | cursor **physically moved** | operator observation recorded in `tools/_vm-type-elevated.cmd`; **no log file in `shots/` reproduces it** |

A hook fires for *injected* input too (`LLKHF_INJECTED`), so its silence against
an accepted send is positive evidence, not absence of evidence. High integrity
alone was sufficient; `UIAccess = 1` was not required.

**Read the third row narrowly.** "Cursor physically moved" is host cursor
motion, which is host-side post-egress evidence about *Windows*, not about the
guest. No artifact anywhere in `shots/` yet shows a **guest framebuffer change
caused by injected input** — that requires two captures saved around the action
and diffed with `tools/png-delta.py BEFORE.png AFTER.png --floor <pct>`, where
the floor is a **percentage** (the guest delta of an idle control pair), not a
file path. Nothing has produced it. The host→guest input route is
therefore **unproven, not refuted.**

### What the `vm-type-elevated` logs actually say (read line 3, not the filename)

All three exist and **all three were unelevated runs**:

| Artifact | Line 3 | Result | Abort line |
|---|---|---|---|
| `shots/vm-type-elevated.log` | `elevation: NOT ELEVATED` | `DELIVERED=False`, `probe exit: 1` | correct (`at NOT ELEVATED`) |
| `shots/vm-type-elevated-2026-08-07-NOTELEVATED-prefix-abortline-defect.log` | `elevation: NOT ELEVATED` | `sent=1 err=0`, `DELIVERED=False`, `cursor changed : False`, `probe exit: 1` | correct; also states `This run says nothing about elevated behaviour -- the UAC prompt was dismissed` |
| `shots/vm-type-elevated-2100Z-NOTELEVATED.log` | `elevation: NOT ELEVATED` | `sent=1 err=0`, `DELIVERED=False`, `probe exit: 1` | **WRONG** — reads `ABORTED: injection is not delivering even elevated.` |

The 2100Z file's abort line contradicts its own line 3 and **must never be cited
as an elevated result.** In every one of these files the operator dismissed the
UAC prompt, so `probe exit: 0` under elevation has **not yet been observed once**.
When these logs disagree with themselves, the `elevation:` line on line 3 is the
measured condition; the filename and the abort text are both narration.

So `vm-type.py` reports how many event groups `SendInput` **accepted** and says
explicitly that this is not delivery. Same failure shape as `vmrun
connectNamedDevice` returning exit 0 for a device that does not exist (§7b) — an
API's success return is not evidence of effect.

### The route

From an **unelevated** shell, so the UAC prompt can be raised:

```sh
powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath 'D:\nerd shit\other projects\mac os\tools\_vm-type-elevated.cmd' -ArgumentList '--window','Sonoma','--click'"
```

**The path must be ABSOLUTE, and you must pass `-ArgumentList`.** Two ways the
shorter form fails, both *after* the operator has already accepted the prompt:

- `Start-Process -Verb RunAs` launches the child through the shell-execute
  service, which does **not** inherit the launching shell's working directory --
  the child starts in `C:\Windows\System32`. Verified: `ls
  tools/_vm-type-elevated.cmd` from `C:\Windows\System32` exits 2, no such file.
  A relative path therefore reads as "the wrapper is broken" when it is the
  invocation.
- With no arguments the wrapper runs `vm-type.py` with none either, which prints
  `nothing to do` and exits **0** -- a green run that injected nothing.

Then read `shots/vm-type-elevated.log`. The wrapper exists because an elevated
process gets its own console that closes on exit, so stdout is lost exactly when
it matters; it redirects to the log, and it asserts elevation (`net session`)
before trusting anything below that line.

**A human must accept the UAC prompt. No agent can complete this step.**

### Three measurements that look like evidence and are not

Each of these was published as a conclusion during the 2026-08-07 diagnosis and
then retracted. Do not re-derive them:

- **`BlockInput(FALSE)` returning `err=5` (`ACCESS_DENIED`) does not prove a
  foreign thread holds the block.** `BlockInput` requires elevation, so an
  unelevated caller gets `err=5` unconditionally — the reading is unfalsifiable
  from Medium IL. Elevated, the same call returned `err=0`, meaning there was
  never a holder.
- **`GetLastInputInfo` not advancing proves nothing.** That counter is not
  updated by injected input on some Windows builds, so it reads identically for
  delivered and discarded.
- **A hook seeing nothing, without checking what was sent, is a tautology.** One
  run concluded "input is genuinely being discarded" from hook silence while its
  own `SendInput` had returned `0 of 2 err=5` — nothing was ever sent. Always
  print the `SendInput` return alongside the hook count.

Secure desktop was independently ruled out: `OpenInputDesktop` succeeded, the
input desktop is named `Default`, no `consent`/`LogonUI` process was present,
and the screensaver was not running. Integrity levels of `vmware.exe` and the
probe were both `Medium (0x2000)`, so UIPI against a higher-integrity *window*
was also not the cause. The barrier is the caller's own filtered token.

---

## 5b. Remote access — `enable-ssh.sh`

Seven capture attempts have each cost a full round-trip: run script on laptop,
read output on screen, retype or paraphrase it into chat, get a revised script,
copy it back. That loop is the real bottleneck, not the capture itself.

`enable-ssh.sh` collapses it. Run once on the **laptop**:

```sh
sudo sh enable-ssh.sh
```

It installs `openssh-server`, authorises the Windows public key
(`kentl-windows`, ed25519, baked into the script), writes a key-only drop-in at
`/etc/ssh/sshd_config.d/zz-mmiotrace.conf`, validates with `sshd -t`, starts and
enables the service, opens the ufw port if ufw is active, and prints the exact
`ssh user@ip` command for the Windows side.

| Property | Value |
|---|---|
| Auth | Public key only — `PasswordAuthentication no`, `PermitRootLogin no` |
| Exposure | LAN only. Nothing is port-forwarded; the script makes no router changes |
| Idempotent | Yes — the key is appended only if absent, config files are overwritten in place |
| Revoke | `sudo sh enable-ssh.sh --undo` — strips the key, deletes the drop-in, restarts sshd |

**The sudo caveat.** `sudo` over SSH prompts for a password with no TTY to type
it into, which blocks unattended runs of `capture.sh`. The script prints the
NOPASSWD sudoers line rather than installing it, because it is a genuine
security reduction — anyone with a shell as that user becomes root without a
password. Acceptable for a lab laptop on a home network; the user's call, not
the script's.

One detail worth preserving: the script resolves the target user via
`$SUDO_USER` → `logname`, never `$HOME`. Under `sudo`, `$HOME` is `/root`, so
the key would land in `/root/.ssh/authorized_keys` and login as the normal user
would fail with an unhelpful `Permission denied (publickey)`. It also hard-sets
`chmod 700` on `.ssh` and `600` on `authorized_keys`, since sshd silently
ignores the file when permissions are loose and reports the same opaque error.

---

## 6. DONE — the trace capture

**Captured 2026-08-06 22:41 UTC, attempt 8. 357,495 lines.**

| Artifact | Location | Detail |
|---|---|---|
| `mmio-boot.log.gz` | project root (Windows) + `~/Downloads` (laptop) | 1,712,423 bytes, `gzip -t` clean, 357,495 lines |
| `capture-summary.txt` | project root (Windows) | Full PASS/FAIL transcript of the run |

The run reported `PASS : card is unclaimed`, `PASS : mmiotrace active,
tracing_on=1`, `driver : nvidia`, and a bound driver on `01:00.0`. The log opens
with `VERSION 20070824` followed by real `MAP` / `R 4` / `UNMAP` records against
BAR0 at `0x81000000` — a genuine cold init, not a partial or empty trace.

`tools/parse-mmiotrace.py` is the next step; this log is its input.

### What finally made it work — attempt 8

The blacklist was never the whole answer. Even with the full
`modprobe.blacklist=` string on `/proc/cmdline` **and** `nvidia-powerd`
disabled, `nvidia` still came up with refcount 32 and bound the card.

The culprit was **`gpu-manager.service`** (Ubuntu's `ubuntu-drivers-common`).
It runs before `display-manager.service` and calls `/sbin/modprobe` *directly,
by exact module name*, to probe the discrete GPU. `modprobe.blacklist=` only
suppresses alias- and udev-driven loading, so an explicit modprobe sails
straight through it. Journal timings from the failing boot:

| Time | Event |
|---|---|
| 3.99s | `gpu-manager.service` starts |
| 4.35s | `nvidia` module appears |
| 4.95s | `gpu-manager.service` finishes |

It also left `/run/u-d-c-nvidia-was-loaded` and
`/run/u-d-c-nvidia-drm-was-loaded` behind as receipts — those marker files are
the fastest way to confirm this specific cause on a future machine.

`prep-blacklist.sh` now disables `gpu-manager` alongside `nvidia-powerd`,
`nvidia-hibernate`, `nvidia-resume`, `nvidia-suspend`, and
`nvidia-suspend-then-hibernate`. `--undo` re-enables all six. After that change
plus a reboot: `NO_NVIDIA_LOADED`, `NO_DRIVER_BOUND`, no `u-d-c` markers — and
the capture succeeded on the first try.

**Lesson worth keeping:** on Ubuntu, blacklisting a GPU driver is necessary but
not sufficient. Anything that calls `modprobe <exact-name>` ignores a blacklist
entirely, so the services that do so must be disabled as well.

### Remote execution made this tractable

Attempt 8 was the first run driven entirely over SSH from the Windows machine:
inspect state, edit the script locally, `scp` it across, run it, reboot, verify,
re-run, and `scp` the results back — no copy-pasting terminal output. The seven
previous attempts each cost a full manual round-trip. See §5b.

Lid suspend was disabled via `/etc/systemd/logind.conf.d/zz-mmiotrace-lid.conf`
(`HandleLidSwitch=ignore` and friends) so the laptop stays reachable with the
lid shut. Delete that file and restart `systemd-logind` to restore normal
behaviour.

### Prior attempts — the failure record

Seven attempts, seven distinct failures, before the capture succeeded:

| Attempt | Outcome |
|---|---|
| 1 | Ran from live-USB `~/Downloads` (tmpfs). Driver wouldn't unload — `nvidia_drm` refcount 7 |
| 2 | Script stopped the display manager from inside the desktop and **killed its own terminal**. Truncated summary, no PASS/FAIL |
| 3 | Manual `sudo systemctl stop gdm3` → **entire system locked up**, required REISUB |
| 4 | 2026-08-06. Ran without blacklisting. The script's own `modprobe -r` at line 108 fired **before** the guard that was supposed to prevent exactly this. Graphical run died; re-run from Ctrl+Alt+F3 **hard-locked the laptop** during the unload |
| 5 | 2026-08-06 20:12 UTC. Re-run with the fixed guard. Reached the guard, printed refcounts (`nvidia_drm` 7, `nvidia` 69), exited 1 **without touching anything**. Not a crash — the guard working as designed |
| 6 | 2026-08-06 20:34 UTC. First run past the guard, on a blacklisted boot. `PASS : card is unclaimed`, `driver : nvidia`, **`lines : 0`**. Tracer attached, driver "loaded", zero MMIO recorded |
| 7 | 2026-08-06 20:53 UTC. Followed a bad instruction of mine — `prep-blacklist.sh --undo` "to reset" — which *removed* the blacklist, so the next boot loaded the driver normally and the guard fired again. `nvidia` refcount 70. No harm done, but a wasted reboot |

### Attempt 4 was a script bug, not user error

The unclaimed-card guard existed, but an unconditional
`modprobe -r nvidia_drm nvidia_modeset nvidia_uvm nvidia nouveau` sat ~13 lines
*above* it. The guard could only ever see the state left behind by the unload it
was meant to prevent, so on a machine with the driver loaded the script did the
dangerous thing first and checked afterwards. Being on a TTY did not help, as
predicted below.

**Fixed 2026-08-06:** the `modprobe -r` line is deleted. The script now never
unloads a module and never touches the display manager — it only *checks*
`lsmod` and exits 1 with the blacklist instructions if anything is loaded. The
dead fallback block that used to suggest `systemctl isolate multi-user.target`
is removed too. `sh -n` clean. `STEPS.md` step 3d and `SETUP.md` step 5 carried
the same bad `modprobe -r` advice and were corrected in the same pass.

**Verified 2026-08-06, attempt 5:** re-run on the installed system with the
driver still loaded. The script reached the guard, printed
`FAIL : the driver is held open by this desktop session`, listed the refcounts
(`nvidia_drm` 7, `nvidia` 69), and exited 1 **without touching anything**. No
lock-up. The guard now behaves as documented; the blacklist boot is the only
remaining step.

Also hardened in the same pass: the in-trace `modprobe nvidia` now confirms via
`lsmod` that the module actually inserted, and falls back to
`modprobe --ignore-install nvidia` if it did not. `modprobe.blacklist=` only
suppresses automatic/alias-driven loading, so an explicit modprobe by exact
module name still works -- but the fallback removes any doubt that the boot
blacklist could silently no-op the one load the tracer exists to capture.

### Root cause

On a muxless NVIDIA laptop, stopping `gdm3` releases the display while
`nvidia_drm` still owns KMS. There is no mux to hand the panel back through and
no console driver left to take over, so the machine hangs. **Being on a TTY does
not help — the TTY is one of the things that dies.** That advice was wrong and
is now removed from the script; line 130 is a warning telling you not to do it.

### The only supported route

Blacklist at boot so the driver is never claimed. Nothing needs tearing down,
and it yields a **strictly better** trace — a genuine cold init, the one and only
initialisation the firmware ever performs per power cycle. Unload-and-reload can
skip steps that only happen once, leaving holes exactly where Phase 4 needs the
sequence complete.

`prep-blacklist.sh` installs the blacklist to disk so there is nothing to type
at the GRUB menu. Copy both scripts to the laptop, then:

```sh
sudo sh prep-blacklist.sh
sudo reboot
```

The desktop comes up on the Intel iGPU; the 3050 sits unclaimed. Confirm with
`lsmod | grep -E 'nvidia|nouveau'` (expect no output), then:

```sh
sudo sh capture.sh
```

When the whole project is finished, restore the driver:

```sh
sudo sh prep-blacklist.sh --undo && sudo reboot
```

**What `prep-blacklist.sh` writes** — two files, both removed by `--undo`:

| Path | Purpose |
|---|---|
| `/etc/modprobe.d/zz-mmiotrace-blacklist.conf` | `blacklist` lines for all five modules — stops udev/alias-driven loads once userspace is up, while leaving an explicit `modprobe nvidia` fully working |
| `/etc/default/grub.d/zz-mmiotrace-blacklist.cfg` | Appends `modprobe.blacklist=…` to the kernel cmdline — covers initramfs loads, before `/etc/modprobe.d` is readable |

It then runs `update-initramfs -u` and `update-grub`, and confirms the string
landed in `/boot/grub/grub.cfg`. Using `grub.d/` rather than editing
`/etc/default/grub` leaves the user's own `GRUB_CMDLINE_LINUX_DEFAULT`
untouched, so the entire change is two deletable files.

Neither mechanism blocks the explicit `modprobe nvidia` that `capture.sh`
issues inside the trace window — blacklisting only suppresses automatic loading
by alias. **Do not add `install <mod> /bin/true` to that conf file.** It reads
like a stronger blacklist, but it makes every `modprobe` of those modules run
`/bin/true` and exit 0 without loading anything — including `capture.sh`'s. See
the attempt-6 postmortem below for what that cost.

### Attempt 6 postmortem — the zero-line capture

The run looked healthy right up to the line count: writability, filesystem,
GA107 identity, debugfs, mmiotrace, unclaimed card — all PASS. Then
`driver : nvidia` followed by `lines : 0`.

**Root cause: `install <mod> /bin/true` in `prep-blacklist.sh`.** The first
version of that helper wrote both `blacklist <mod>` *and*
`install <mod> /bin/true` for all five modules, on the theory that belt and
braces beats either alone. It does not. `install ... /bin/true` replaces the
load action for that module with a command that does nothing and exits 0, and
it applies to **every** `modprobe`, including the deliberate one `capture.sh`
fires inside the trace window. So:

1. `modprobe nvidia` ran `/bin/true`, exited 0, loaded nothing.
2. The `lsmod` verification correctly caught the no-op and fell through to the
   `insmod $(modinfo -n nvidia)` fallback.
3. `insmod` loads exactly one `.ko` by path and resolves **no dependencies**,
   so bare `nvidia.ko` went in without `nvidia_modeset` or `nvidia_drm`.
4. Bare `nvidia.ko` never binds `01:00.0`, never calls `ioremap` on the BAR, so
   mmiotrace had nothing marked to watch. Zero lines — and `DRIVER=nvidia` was
   reported, because the module *was* technically resident.

The insmod fallback masked the failure instead of exposing it. It turned a
loud "modprobe did nothing" into a quiet "driver loaded, trace empty".

**Fixes applied 2026-08-06:**

| Where | Change |
|---|---|
| `prep-blacklist.sh` | `install ... /bin/true` lines **removed**. Only `blacklist` lines are written now — those suppress udev/alias loading while leaving an explicit `modprobe nvidia` fully functional, dependencies included. A comment block in the file explains why they must not be re-added |
| `capture.sh` trace block | Resets `current_tracer=nop`, `tracing_on=1`, and clears `trace` before starting. A stale `tracing_on=0` from a prior run silently yields an empty capture with no other symptom |
| `capture.sh` trace block | `buffer_size_kb=131072`. The default per-CPU ring buffer overflows under mmiotrace and drops the earliest init writes — the ones Phase 4 needs most |
| `capture.sh` trace block | Verifies `current_tracer` actually reads back as `mmiotrace`, and that the `trace_pipe` reader survived its first two seconds |
| `capture.sh` post-trace | Appends `$TR/trace` to the log after killing the reader, so whatever was still in the ring buffer is not discarded |
| `capture.sh` post-trace | New `bound :` line from `lspci -k -s 01:00.0`. Module *resident* ≠ module *bound to the card*; only binding produces MMIO. WARNs explicitly when nothing is bound |
| `capture.sh` load block | The `insmod $(modinfo -n nvidia)` fallback is **replaced by `modprobe --ignore-install nvidia`**. Both bypass an `install` directive, but `insmod` resolves no dependencies and was the proximate cause of the silent empty trace. `--ignore-install` keeps the full chain, and now prints a `note :` when it fires so a stale conf file is visible rather than masked |

Anyone re-running this just needs the new `prep-blacklist.sh` and a reboot. It
overwrites its own config files in place, so the old conf with the `install`
lines is replaced, not merged. **Do not run `--undo` first.**

### Attempt 7 postmortem — `--undo` is not a reset

I told the user to run `prep-blacklist.sh --undo` before re-running it, to clear
the bad conf file. That advice was wrong. `--undo` deletes the blacklist, so the
next boot loads the NVIDIA driver normally — which put the machine back in the
exact state the guard exists to reject. The guard fired correctly (`nvidia`
refcount 70) and nothing was damaged, but a reboot was wasted.

`--undo` has exactly one purpose: restoring the driver when the project is
finished. It is never a step in the capture workflow.

**Fixes applied:**

| Where | Change |
|---|---|
| `prep-blacklist.sh` header | States the script is safe to re-run, that it overwrites its own files, and that `--undo` must not be used as a reset |
| `prep-blacklist.sh` header | Spells out that the reboot is mandatory — the script only writes config, so the resident driver stays loaded until restart |
| `prep-blacklist.sh --undo` | Prints a `NOTE :` after undoing, warning that the blacklist is now gone and that re-running without `--undo` was the intended move |
| `capture.sh` graphical FAIL branch | New three-way boot-state diagnostic (see below) |

The diagnostic distinguishes the three reasons the driver can still be loaded,
so the next failure names its own cause instead of repeating generic advice:

| Condition | Message |
|---|---|
| `modprobe.blacklist=nvidia` in `/proc/cmdline` | Blacklist active yet driver loaded — something loads it explicitly; check `/etc/modprobe.d` and login units |
| Conf file on disk, not on cmdline | Installed but not picked up — **you have not rebooted**; just reboot, do not `--undo` |
| No conf file | Nothing installed — run the helper; if you ran `--undo`, that removed it |

<details>
<summary>Manual alternative — one-time GRUB edit, nothing written to disk</summary>

1. Reboot. Hold **Shift** (BIOS) or press **Esc** (UEFI) for GRUB.
2. Highlight the Ubuntu entry, press **`e`**.
3. Append to the line starting `linux`:
   ```
   modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm,nouveau
   ```
4. **Ctrl+X** to boot, then `sudo sh capture.sh`.

The edit is not saved, so it must be retyped on every boot — which is why the
helper exists.

</details>

Outputs land next to the script: `mmio-boot.log.gz` and
`capture-summary.txt`. Send the summary; `parse-mmiotrace.py` is ready for the
log.

### Script safety guards

- Writability check before the 20-second capture, not after
- tmpfs/overlay refusal (live-USB only; no-ops on installed Ubuntu)
- Refuses to stop any display manager, from any session type
- `trap '' HUP` — completes and writes the summary even if the terminal vanishes
- Rejects captures under 1000 lines, because a partial trace looks exactly like
  a real one

---

## 7. CLOSED 2026-08-14 — the kext parses on Windows AND builds under Apple clang

**Read this first; the section below it was written while the build half was
still open.** The kext was built in the macOS guest on 2026-08-14: `make exit
status: 0`, bundle PRESENT, Apple clang version 17.0.0 (clang-1700.0.13.5),
macOS 15.7.9 (24G830). Log: `step7-results/step7-build-SUCCESS.log`.

The finding is not "it built". It is **what stopped it from building the first
time**, and it was nothing this section predicted. The first attempt reached the
guest exactly as designed, unpacked, and ran `make` — and produced nothing,
because a fresh macOS install ships no compiler. `xcode-select` errored,
`Kernel.framework` was ABSENT, and `make` exited 1 having emitted only the
installer-prompt notice. That failed run is kept as
`step7-results/step7-build.log`; the contrast between the two logs is the
evidence. The missing step was `xcode-select --install`, which no amount of
perfecting the source-delivery channel would have supplied.

Still unproven, and not narrowed by this at all: nothing was loaded. No
`kextload`, no match, no panic test. The Phase 0 milestone in `STEPS.md` STEP 7
remains open.

### The original section, as written before the build


No C++ compiler for Apple *targets* exists on the desktop. MSVC 14.51 + CMake
4.0.3 + Ninja build the host tests, but a kext needs Apple clang with
`-mkernel -fapple-kext` and the Kernel framework headers.

**What changed 2026-08-07:** clang 19.1.5 was found shipping inside Visual
Studio 2022 at `VC\Tools\Llvm\bin\clang++.exe`. Paired with a minimal
IOKit/libkern stub tree in `tests/kextshim/`, it parses the kext front-to-back.
The first time this code has ever been through a C++ front end: **clean, zero
diagnostics, under `-Wall -Wextra -Werror`.**

```
tests\check-kext-syntax.bat      # 5 stages, all green
```

| Now proven | How |
|---|---|
| No typos in IOKit member names or argument counts | clang parse |
| Every `APPLE_KEXT_OVERRIDE` really overrides something | `override` expansion |
| Every `IOLog` format specifier matches its argument width | `format(printf)` attribute |
| `OSDynamicCast` targets are related types | `static_cast` in the shim |
| `OSDeclareDefaultStructors` ↔ `OSDefineMetaClassAndStructors` pairing, and the superclass named matches the real base | `tools/check-structors.py` |
| Both dependency-free headers parse standalone | clang parse |

**Still unproven, and the reason Step 7 is not cancelled:** the shim is not
Apple's SDK — real KPI signatures may be wider or differ outright. **Nothing
links** (a kext links against Apple's kernel KPI, absent here). Nothing says the
kext loads, matches, or runs. Every flag in the Makefile remains untested.
Expect the first real `make` to fail; the output is fixable.

One planted defect in the negative control did **not** fail and was removed
rather than left as decoration: a missing `OSDefineMetaClassAndStructors` is
invisible to `-fsyntax-only`, because the declared ctor/dtor only go unresolved
at link time. That defect class moved to `tools/check-structors.py`. This is
recorded because a syntax gate that passes everything is worse than no gate — it
converts "never compiled" into "believed compiled".

That is `STEPS.md` Steps 4–7 — VMware Workstation Pro (free), DrDonk's
unlocker, a recovery image via `OSX-KVM`'s `fetch-macOS-v2.py`, then Xcode +
Additional Tools + a build-matched KDK. Free Apple ID only. All on the desktop;
the laptop is not involved.

**The image that came down is macOS 15.4.1 Sequoia (build 24E263), not Sonoma**
— confirmed 2026-08-07 from the embedded `SystemVersion.plist`. "Build-matched
KDK" therefore means matched to **24E263**, not to a 14.x build.

**Step 4 is already satisfied — verified 2026-08-07.** VMware Workstation
**17.6.4.24832109** is installed at `C:\Program Files (x86)\VMware\VMware
Workstation\`, and `vmrun.exe` 1.17.0 (build-24832109) responds. The version was
read from the unlocker's own `check.exe`, not inferred from the install path — an
earlier note in this file said 17.0.0, which came from the `Workstation-17.0.0`
VIX subdirectory name and was wrong. No Broadcom download is needed.
Because `vmrun` works, VM creation, power control, and headless driving can all
be done from the command line — the wizard does not have to be used.

**The laptop cannot host this VM.** Checked 2026-08-07: i5-11400H, VT-x present,
`/dev/kvm` present, 15 GB RAM — all fine — but a single Ubuntu root with only
~40 GB free and no second partition. Xcode alone is ~40 GB. Do not revisit this.

**Disk is no longer blocking.** As of 2026-08-07 the user cleared space: `C:` now
has **204 GB free** (218,536,812,544 bytes), `D:` still 86 GB. Budget ~120 GB for
the guest + Xcode + KDK, so `C:` is sufficient with headroom. **Put the VM on `C:`,
not `D:`** — 86 GB would run out once Xcode is installed inside the guest. An
external USB 3 drive remains a fallback but is no longer needed.

**Three items genuinely require the user** and cannot be done over SSH or by
`vmrun`: (1) DrDonk's `unlocker` run elevated — it patches the signed
`vmware-vmx.exe` in place, reversible with `relock.exe`; (2) clicking through the
macOS installer GUI (Disk Utility erase, then install); (3) an Apple ID login for
Xcode and the KDK. Everything else in Steps 4–7 can be driven from here.

**Update 2026-08-07 — item (1) was done, and it remains load-bearing.** The
unlocker was run and `check.exe` reports `Patched (1)`. An earlier revision of
this file claimed the patch was unnecessary because the `.vmx` was hand-written;
that was wrong, and it is the kind of wrong that costs a day. The patch is not
about the New VM wizard's guest-OS list — an **unpatched `vmware-vmx.exe`
refuses to power on any `darwin*` guest**, however the `.vmx` was authored. It
also reverts on any VMware update, so `check.exe` is the first thing to re-run
when a previously-working VM stops powering on.

Items (2) and (3) still stand. **`VM-BRINGUP.md` is the authority on the VM** —
it records four consecutive power-on failures and the fix for each, none of
which named its own cause in any error message.

---

## 7b. DONE — GA107 firmware extracted from the laptop

**2026-08-07, over SSH to `kenny@kenny-linus` (192.168.0.180), key auth.** The
laptop runs Ubuntu, kernel 7.0.0-29-generic, with `nvidia-595-open` 595.84
installed — which means the exact firmware blobs for this die were already on
disk.

Pulled into `reference/ga107-firmware/` (see its `README.md` for full detail):

| File | Size | Role |
|---|---|---|
| `bootloader-570.144.bin` | 24,684 B | **The image the mmiotrace DMA'd into IMEM** |
| `booter_load-570.144.bin` | 61,304 B | SEC2 Booter Load — raises WPR2, authorises GSP |
| `booter_unload-570.144.bin` | 41,080 B | SEC2 Booter Unload |
| `vbios-ga107.rom` | 65,024 B | VBIOS, `55 aa` valid, `Version 94.07.4E.00.4B` |

### Why this is the most useful thing since the trace

`PHASE4-GSP-BRINGUP.md` §3.3 *inferred* a ~24 KB IMEM payload from 96 × 256 B DMA
transfers. `bootloader-570.144.bin` is 24,684 bytes. **The inference was right,
and the actual image is now local** — so the kext's DMA loop can be written
against a parsed descriptor instead of a guessed one.

### Two findings that would have cost weeks each

**1. These are `nvfw_bin_hdr` containers, not VBIOS-embedded
`FALCON_UCODE_DESC` images.** `header_offset` is 24 in all three files, so one
parser *looks* like it should work — but the bootloader holds
`RM_RISCV_UCODE_DESC` v5 there while both booters hold `nvfw_hs_header_v2`.
Parsing all three the same way gave the bootloader clean fields and the booters
nonsense (`bootloaderOffset = 0x0000f000`, past end of file). A scan for a V2/V3
descriptor header found **none** in any blob; that null result *was* the answer.
The `0x3c` = 60 word that looked like `FALCON_UCODE_DESC_V2_SIZE_60` is a
coincidence — it is `sig_prod_offset` in the HS header.

**2. The booters are heavy-secure signed and carry a signature patch point**
(`sig_prod_size = 768`, `patch_loc = 828`, `num_sig = 848`). Phase 4 cannot load
them without performing that patch step. There is no shortcut around it.

Bootloader descriptor, for the record — every offset lands inside the file:

```
version 5   bootloaderOffset 20480   bootloaderSize 2176
            bootloaderParamOffset 22656   bootloaderParamSize 16
            riscvElfOffset/Size 0/0   manifestSize 2048
            monitorDataOffset 2048   monitorDataSize 4096
            monitorCodeOffset 6144   monitorCodeSize 10496
            bIsMonitorEnabled 1
```

`bIsMonitorEnabled = 1` with a 10,496-byte monitor code region means this image
carries a LIBOS monitor, matching the RISC-V-with-monitor boot path in
`kernel_gsp_tu102.c`.

### The VBIOS dump trap

First two attempts returned `dd: IO error` and a 0-byte file. **The cause was not
power management** — `power/control` read `on` and the device was in `D0`. It was
that **no driver was bound**: with `driver` empty, the kernel refuses to service
reads of the `rom` attribute even with `rom_enable` set. `sudo modprobe nvidia`
first, then the dump works.

This interacts with `prep-blacklist.sh`: it blacklists `nvidia` so the module
does not autoload during a capture, but an explicit `modprobe nvidia` still works
— which is precisely why the `install ... /bin/true` lines were removed from it
(§5). If a future session finds the ROM unreadable, check driver binding before
suspecting hardware.

**State left on the laptop:** the `nvidia` module is now loaded (it was not
before). `/home/kenny/ga107-fw/` and `/home/kenny/vbios-ga107.rom` exist. A
reboot clears the module; the files are harmless to leave or delete.

**Not copied:** `/lib/firmware/nvidia/595.84/gsp_ga10x.bin`, 72,865,776 bytes —
the full GSP RM image. Too large to carry here and not needed until Phase 4
reaches the RM-image stage. Still on the laptop if wanted.

### This partly resolves a §8 blocker

§8 blocker 3 asked for "GSP-RM and Booter firmware blobs extracted from NVIDIA's
`.run` installer, version-locked to v610.57.04." The booters and bootloader are
now in hand at **570.144**, not 610.57.04. That version gap is real and matters —
see the note added to §8 below.

---

## 8. Phase 4 — what it needs and where to start

The firmware push is the **first thing in this project that writes**.

### Blockers

1. **The `mmiotrace` capture** — ground truth for the boot sequence. Phase 4
   should be written against the real register order, not against a reading of
   NVIDIA's source.
2. **The macOS VM** — so `make` can run.

   **PARTLY RESOLVED 2026-08-07 — the VM boots to a live, interactive macOS
   Recovery; nothing is installed in it yet.**
   `.vmx` hand-written with `guestOS = "darwin24-64"`
   (macOS 15 — authored as `darwin23-64` and corrected 2026-08-07 once the guest
   was identified; backup at `Sonoma.vmx.bak-20260807`. **`.vmx` settings are
   read once, at power-on — shut the guest down before editing, then power on.**
   Editing a running VM changes nothing, VMware does not warn you, and it
   rewrites parts of the file during shutdown, so a mid-session edit can be
   silently discarded. Suspend/resume does not count: a resumed VM restores
   saved device state, not the file. Diff against the `.bak-` copy to see what
   actually persisted. An **unpatched**
   `vmware-vmx.exe` refuses to power on either value — the unlocker is
   load-bearing, see §7a), `BaseSystem.dmg` converted to raw
   (`BaseSystem.img`, 3,221,110,784 bytes) because VMware presents a
   *compressed* UDIF as `capacity=0`. The VM reaches the **macOS Recovery menu**
   — verified by screen capture, not inferred from `poweredOn`.

   **And it is live.** The user clicked around in Recovery and it responds
   normally. Nothing about the host needs changing.

   **RETRACTED 2026-08-07 — do not act on the previous revision of this
   section.** It stated that the guest froze at Recovery, named
   `Monitor Mode: ULM` as the root cause, and instructed the reader to run
   `bcdedit /set hypervisorlaunchtype off` and disable Memory Integrity / Core
   Isolation. **That would have disabled Windows security features to fix a
   problem that does not exist.** The two signals behind it were measured
   correctly and supported neither conclusion:

   - **0 changed pixels in the guest rect over 8 s.** This is exactly what an
     idle *live* Recovery menu looks like. macOS Recovery has no clock, no
     spinner and no animation, so idle-but-running is pixel-identical frame to
     frame. A static framebuffer is evidence of a static screen, nothing more.
   - **`Monitor Mode: ULM` in `vmware.log`.** Not an error. VMware logs it
     whenever the Windows hypervisor is present, which is the default on
     Windows 11. It correlates with nothing here.

   The general lesson, kept because it was expensive: **a pixel diff cannot
   answer "is the guest running?"** Interaction can. Click something and see
   whether it responds.

   The `--compare` guest-box fix (2026-08-07) is still correct and worth
   keeping — it stops VMware's own chrome ticks from reading as guest activity,
   printing `guest 0.000%, whole window 0.055%` so the two are distinguishable.
   It just does not make a static frame mean anything.

   **Use `tools/vm-screenshot.py` to check *what is on screen*, not whether the
   guest is alive.** `--expect "Reinstall macOS"` makes "it reached the UI" an
   exit code, and that is the part worth trusting: five host-desktop screenshots
   were mistaken for guest captures on 2026-08-07 before OCR caught the
   operator's own shell prompt in one. `PrintWindow` (not `CopyFromScreen`) is
   what makes the capture occlusion-proof.

   **`--compare` does not answer "is the guest running?"** — that inference is
   what had to be retracted above. It detects *motion*; absence of motion is no
   information, because an idle Recovery screen has none. Since 2026-08-07 it
   diffs the **guest box** rather than the whole window and prints both figures
   (`guest 0.000%, whole window 0.055%`), which correctly stops VMware's chrome
   ticks from reading as guest activity — a real improvement, and still not a
   liveness test.

   **`--compare` is also not the before/after instrument.** It captures both
   frames itself, after the action, so it cannot attribute a change to that
   action. This limit is **inherent** — it is what the flag does, not a bug.
   For "did this action change the guest framebuffer?", save two captures with
   `--out` around the action and diff them with
   `tools/png-delta.py BEFORE.png AFTER.png --floor <pct>`, which never
   captures anything itself.

   The *separate* clobber defect is **fixed** (2026-08-07): `--compare REF.png`
   used to save frame 2 over `REF.png`, destroying any baseline parked there and
   making the mtimes claim the baseline was written after its own after-frame —
   that is where the retracted `0.000%` floor came from. The argument is now a
   **base name**: frames go to `REF.frame1.png` / `REF.frame2.png` and `REF.png`
   is never written. Guarded by
   `tests/test_vm_screenshot.py::DerivedFramePathTests`, which greps the tool's
   source so the old call cannot return silently.

   **Confirm the two md5s differ first.** As of 2026-08-07 `png-delta.py` does
   that for you and exits **3** on byte-identical input, because every
   before/after pair in `shots/` turned out to be one frame counted twice and
   the resulting `0.000%` deltas were arithmetic, not measurements. That also
   means the `<pct>` floor is only meaningful if the control pair behind it had
   differing md5s — the old `0.000%` floor is retracted, so measure a fresh
   control before quoting one.

   **The recovery image is macOS 15.4.1 Sequoia, build 24E263 — not Sonoma.**
   Read out of the embedded `SystemVersion.plist` in the raw image. The
   directory, `displayName`, nvram and vmdk are all still *named* "Sonoma";
   those are filenames and are deliberately left alone so paths do not break.
   Only the OS-version claims were wrong. This matters downstream: the KDK and
   Xcode must match **15.4.1 / 24E263**, and guest tools need `darwin.iso`, not
   `darwinPre15.iso`.

   **Booted is still not installed.** Nothing has been erased, installed, or
   compiled. This blocker is not cleared until `make` actually runs.
   `VM-BRINGUP.md` records the four power-on failures and their fixes; read it
   before touching the VM.

   **Next action, and it needs a human at the GUI** — `vmrun` cannot drive
   Recovery. In Disk Utility, turn on **View → Show All Devices** first (without
   it the sidebar lists volumes, not devices, and erasing a volume leaves the
   old partition scheme in place), then select the **~100 GB** device and erase
   it as **APFS** / **GUID Partition Map**. **Not the ~3 GB device** — that is
   `BaseSystem.vmdk`, the installer currently running; Recovery will erase it
   without complaint and the failure only surfaces later as an install that
   cannot find its source. Then *Reinstall macOS Sequoia*. `STEPS.md` Step 5
   item 4 has the same table.

   **RESOLVED 2026-08-07 18:52:35Z — the payload fetch is no longer blocked.**
   *Reinstall macOS Sequoia* is a **network installer**: the 3 GB
   `BaseSystem.img` carries the recovery environment, not the ~15 GB OS payload,
   so Continue makes the guest fetch from Apple. For the whole `e1000e` session
   the guest had no working route and Recovery stopped at *"An Internet
   connection is required"*.

   The cause was a **missing driver, not a network fault**: the 15.4.1 (24E263)
   recovery kernelcache ships no Intel ethernet kext, so the `e1000e` device had
   nothing to claim it and `en0` never existed. Swapping
   `ethernet0.virtualDev` to `vmxnet3` and power-cycling fixed it. Evidence:

   ```
   power-on   2026-08-07T18:51:31.356Z   vmware.log line 1, pid=22944
   applied    :305 guestOS = "darwin24-64"   :340 ethernet0.virtualDev = "vmxnet3"
   lease      starts 5 2026/08/07 18:52:35   ends 19:22:35
              hardware ethernet 00:0c:29:e7:9f:2b   client-hostname "Mac"
   ```

   The lease is **64 s after** power-on, and `client-hostname "Mac"` means a
   macOS DHCP client genuinely ran — impossible with no driver bound. Guest MAC
   is now present in the host ARP table, and `quiet-ibis-f5` measured
   host→guest ping 3/3 at TTL 64. Full writeup in `VM-BRINGUP.md` →
   *CONFIRMED CAUSE — the recovery kernelcache has no e1000e driver* and
   *Acceptance test — PASSED*.

   Remaining second-order suspect, **not** a current failure: the Mullvad tunnel
   owns the host default route at metric 1 with MTU 1380 against VMnet8's 1500.
   If the ~15 GB download stalls mid-transfer rather than failing to start, that
   mismatch is the first thing to test.

   **The erase above is already DONE on this VM — do not redo it.** An earlier
   revision of this section said the wall lands "before Disk Utility, before any
   disk is touched" and that "nothing has been erased." **Both were wrong.**
   Disk Utility ran successfully on 2026-08-07 (13:28–13:29) and the **~100 GB
   target is erased and APFS-formatted**; the network wall came *after* that.
   The disk instructions above are still correct as instructions — they are just
   already satisfied here. Treating them as pending wastes a session hunting for
   a completed step.

   Two independent routes out, and they solve different problems:

   - **Fix the guest network** — the better outcome. Being localised to one hop
     rather than guessed; see `VM-BRINGUP.md` § Recovery network diagnosis.

     **CONFIRMED CAUSE, 2026-08-07: the 15.4.1 / 24E263 recovery kernelcache
     contains no Intel ethernet driver at all, while the VM is configured with
     `ethernet0.virtualDev = "e1000e"`. `en0` never comes up, no `DHCPDISCOVER`
     is ever sent, and Recovery reports "An Internet connection is required".**
     Established host-side from `BaseSystem.img`, without touching the VM.

     **The fix is `ethernet0.virtualDev = "vmxnet3"`** — see *Applying the fix*
     below, which has two hazards worth reading before the edit.

     Four independent host-side observations pointed here first. **All four were
     measured during the `e1000e` session (2026-08-07 ~18:09–18:24 UTC) and all
     four have since flipped — read them as the diagnostic record, not as a
     description of the running VM:**

     1. **No DHCP lease this boot.** `vmnetdhcp.leases` held a lease for
        `192.168.255.129` / `00:0c:29:e7:9f:2b` starting 16:20:12 UTC — which
        *predated* the 16:36:53 UTC power-on, and the file had not been written
        since. The lease was a leftover, not evidence of that session.
     2. **The guest MAC was absent from the host ARP table.** Only the `.254`
        pseudo-endpoint appeared; `00:0c:29:e7:9f:2b` never did.
     3. **VMnet8 byte counters were dead flat** — 8562 in / 63070 out, unchanged
        across three 30-second samples.
     4. **Nothing upstream was blocking Apple** — every endpoint the installer
        needs was reachable from the host through the same tunnel.

     Read together these ruled out the routing, DNS, clock and MTU explanations,
     *because every one of those requires a guest that is already talking*. They
     described a guest emitting nothing.

     **After the `vmxnet3` swap and the 18:51:31Z power cycle, each one
     reversed — which is what converts the diagnosis into proof by remedy:**

     | Under `e1000e` | Now under `vmxnet3` |
     |---|---|
     | No lease this boot | Lease `192.168.255.129`, `starts` 18:52:35Z, `client-hostname "Mac"` |
     | Guest MAC absent from ARP | Present as `192.168.255.129` **dynamic** |
     | Counters flat across 90 s | `8562/63070` → `10430/70094` |
     | Only the host reached Apple | The guest is on the wire |

     **The evidence is an enumeration, not an absence argument.** The complete
     prelinked ethernet/network kext list in `BaseSystem.img` is
     `AppleUSBEthernet`, `AppleUSBEthernetHost`, `AppleUSBNetworking`,
     `IONetworkingFamily` and its PlugIns `AppleBCM5701Ethernet`,
     `AppleEthernetAquantiaAqtion` (+`Firmware`), **`AppleVmxnet3Ethernet`**,
     `IOEthernetAVBController`, `mDNSOffloadUserClient`, plus `IOUserEthernet`.
     That is the whole list. Every prelinked path containing "Intel" is
     graphics / LPSS / MCE / PCH — **zero networking**. Meanwhile
     `vmware.log:340` shows the guest is handed an Intel 82574L-class NIC that
     nothing in that environment can claim.

     Controls were run both ways: `AppleUSBEHCI`, `IOPCIFamily`,
     `AppleAHCIPort`, `Sequoia` and `24E263` all **present** (the image reads
     fine), and two invented kext names came back **absent** (no false
     positives). Two apparent Intel hits were disproved — `Intel8254` is
     `com.SmallTree.iokit.Intel8254x` sitting *inside* the `OSKextExcludeList`
     blocklist, and `e1000` / `E1000` are the symbols `_fiveToThe1000` and
     `GCC_except_table1000`.

     **`vmxnet3` will actually bind, which is the part that matters.**
     `AppleVmxnet3Ethernet` declares `IOPCIPrimaryMatch = 0x07b015ad&0xffffffff`
     — `15ad` is VMware's vendor ID and `07b0` the vmxnet3 device ID, so the
     driver in this exact recovery image matches the exact device VMware will
     present. It is not merely that the kext exists.

     #### Applying the fix — two hazards, verified in the VMX

     1. **Change `ethernet0.virtualDev` from `e1000e` to `vmxnet3`.**
     2. **Delete the `ethernet0.pciSlotNumber = 160` line outright.** That value
        was chosen for an e1000e device, and the VMX's own comment block records
        **two** power-on crashes (`0xc0000005`, `[msg.pci.noslotavail]`) caused
        by this exact field being wrong for the device. Let VMware reassign it;
        it writes a correct value back at power-on. Leave
        `generatedAddress` / `generatedAddressOffset` alone — the MAC can stay,
        only the slot is device-dependent.
     3. **Fix the Network comment in the VMX — it is backwards and it is what
        caused this blocker.** It reads "e1000e is the only NIC macOS has a
        built-in driver for. vmxnet3 needs VMware Tools". For macOS 15 the
        opposite is true: the recovery kernelcache ships `AppleVmxnet3Ethernet`
        prelinked and ships no Intel ethernet kext of any kind. vmxnet3 needs
        Tools on Windows and Linux guests, not on macOS — Apple bundles the
        driver. Replace the comment, or the next reader swaps back.
     4. **Full power cycle, not a guest reboot** — the VMX is read once at
        power-on (`VM-BRINGUP.md` § Three traps). Land the `guestOS`
        `darwin24-64` correction in the same cycle; the running instance is
        still `darwin23-64` even though the file says otherwise.

     **Acceptance test, falsifiable both ways:** note the power-on timestamp
     from `vmware.log` line 1, then check
     `C:/ProgramData/VMware/vmnetdhcp.leases` for a lease whose `starts`
     timestamp is **later** than that power-on. A new lease closes this. **If
     `en0` appears and there is still no lease, this diagnosis is dead and
     Mullvad returns to the front** — state that plainly rather than softening
     it.

     > **STATE 2026-08-07 18:57Z: the fix is APPLIED, RUNNING, AND THE OUTCOME
     > IS MEASURED — the guest is on the wire.** Supersedes two earlier notes
     > here: one saying the VM had never been restarted, and one saying only the
     > outcome remained untested. Both were accurate when written and were
     > overtaken — first by the power cycle, then by the acceptance test in the
     > UPDATE block below. **The only part still open is hop 4** (does
     > `vmnat.exe` forward to Apple), not whether the NIC works.
     >
     > The VM was power-cycled and the swap reached the running instance. This
     > is a **new** `vmware.log` — line 1 is `2026-08-07T18:51:31.356Z`,
     > `pid=22944`, not the old `16:36:53Z` / `pid=30264`:
     >
     > ```
     > 305  DICT              guestOS = "darwin24-64"
     > 340  DICT ethernet0.virtualDev = "vmxnet3"
     > 418  Powering on guestOS 'darwin24-64' using the configuration for 'darwin24-64'
     > 687  Ethernet0 MAC Address: 00:0c:29:e7:9f:2b
     > ```
     >
     > So criterion 1 holds **and** the edit is live in the running VM.
     >
     > **UPDATE 2026-08-07 18:57Z — criteria 2 and 3 are now MET. The diagnosis
     > is confirmed by remedy, not by argument** (`t-09d0fd`):
     >
     > | Criterion | Result |
     > |---|---|
     > | Applied NIC is `vmxnet3` | yes — `DICT ethernet0.virtualDev`, new log |
     > | Fresh lease after power-on | **yes** — `starts 18:52:35`, 64 s after `18:51:31` |
     > | Guest identifies itself | **yes** — lease carries `client-hostname "Mac"`; the stale one had none |
     > | Guest reachable on vmnet8 | **yes** — host → `ping 192.168.255.129` 3/3, 0% loss, TTL 64 |
     >
     > Under `e1000e` that same guest was silent for 87 minutes across a full
     > boot — no ARP entry, `Destination host unreachable`. The guest is now
     > emitting frames, which is precisely what the no-driver hypothesis
     > predicted and the egress hypotheses did not.
     >
     > **EXTENDED 2026-08-07 19:41Z — the stack is not merely up, it is
     > *sustained* (`t-24b954`).** The 18:57Z table rested on a single
     > boot-time lease, which a one-shot DHCP fluke could also produce.
     > `vmnetdhcp.leases` now holds **five** successive leases to
     > `00:0c:29:e7:9f:2b` — 18:52:35Z, 19:07:34Z, 19:26:09Z, 19:40:13Z,
     > 19:41:17Z. The 19:07 lease carried a two-hour term and the guest
     > re-DHCPed at 19:26 regardless, so `en0` stayed bound and renewing across
     > ~50 minutes. A renewal chain cannot be a boot-time artifact. Host
     > `arp -a` independently still shows `192.168.255.129 → 00-0c-29-e7-9f-2b
     > dynamic`, learned only from real frames on VMnet8, and `vmrun list`
     > confirms this same `pid=22944` instance is the one still running.
     >
     > **The timestamp is the test, not the lease's existence.** The MAC does
     > not change across a NIC swap, so "a lease exists for `00:0c:29:e7:9f:2b`"
     > passes on the *stale* record (which started `16:20:12`, sixteen minutes
     > **before** the previous `16:36:53` boot). Always compare `starts`
     > against the power-on line in the current `vmware.log`.
     >
     > **Criterion 4 — whether Recovery actually proceeds — remains open.** The
     > guest has an address and answers ICMP on the LAN; nothing yet proves
     > `vmnat.exe` forwards it to Apple. That needs someone at the console, and
     > it is the hop where Mullvad and the MTU finding below become live again.
     >
     > **PCI slot, worth recording.** VMware wrote `ethernet0.pciSlotNumber =
     > "160"` back at line 228 of the new log — the *same number* as the pin
     > that was deleted. But VMware chose it **for a vmxnet3 device**, and the
     > VM powered on with no `[msg.pci.noslotavail]` and no `0xc0000005`.
     > Deleting the pinned line was still correct: an identical value does not
     > mean the pin was harmless, it means VMware happened to pick the same slot
     > for a different device.
     >
     > **Do not "fix" the apparent contradiction in the VMX.** `Sonoma.vmx:161`
     > comments that `ethernet0.pciSlotNumber` is deliberately ABSENT while
     > `Sonoma.vmx:228` sets it to `160`. That reads like a stale pin someone
     > forgot to delete. It is not. The power-on `DICT` dump enumerates every
     > key the VMX supplied at `18:51:31.641Z` (`vmware.log:338-342`, `364-378`)
     > and contains **no** `ethernet0.pciSlotNumber` — it genuinely was absent
     > on disk at boot, and VMware wrote the value back afterward. The VMX
     > mtime is later than the power-on, which corroborates. Deleting the line
     > again is harmless (VMware re-derives it); *hand-editing it to a chosen
     > number is what caused the original crashes* — see `Sonoma.vmx:39`.

     **Mullvad is eliminated as the cause of *this* failure, and it was not a
     bad hypothesis.** `NETWORK-DIAGNOSIS.md` proposed that Mullvad's
     kill-switch drops `vmnat.exe` egress — correctly labelled unproven, with
     the right one-step test attached. It is ruled out by measurement, not by
     argument: an egress block predicts frames that *leave the guest* and die
     upstream, but ARP and `DHCPDISCOVER` are **L2 broadcasts on VMnet8 that
     never touch the tunnel or the default route**. A VPN cannot produce a flat
     `BytesIn` counter or a missing ARP entry, because both are *pre-egress*.

     | Hypothesis | Hop | Guest emits frames? | Status |
     |---|---|---|---|
     | **No NIC driver** (`e1000e` unsupported in 15.4.1 Recovery) | 1 | no | **confirmed** |
     | **Mullvad blocks `vmnat.exe`** | 3–4 | yes, they die on the host | eliminated *for now* — second-order |

     Mullvad stays on the list as a **post-swap** suspect and becomes live again
     the moment `en0` exists, because the tunnel genuinely does own the default
     route at metric 1 with MTU 1380. The MTU finding below sits in the same
     tier for the same reason.

     **Do not disconnect Mullvad to test it now.** It changes host state, buys
     nothing diagnostically, and if the swap then worked nobody could say which
     change did it.

     **Method lesson worth keeping:** this resolved without touching the VPN or
     the console because byte counters and the ARP table are *pre-egress*
     evidence — they discriminate between "guest not talking" and "guest talking
     but blocked", a distinction no host-side reachability test can make.

     Per-hop evidence lives in `NETWORK-DIAGNOSIS.md`; the full kext enumeration
     is in `VM-BRINGUP.md` § *CONFIRMED CAUSE — the recovery kernelcache has no
     e1000e driver*.

     **Host-side measurement, 2026-08-07 — a real MTU mismatch that is *not*
     established as the cause, and is subordinate to the above.** A Mullvad
     WireGuard tunnel owns the default
     route (`10.64.0.1`, ifIndex 60, ifMetric 1) and beats Wi-Fi
     (`192.168.0.1`, ifMetric 35). The tunnel's MTU is **1380**; VMware's
     VMnet8 host adapter is **1500**. A guest that sizes packets for 1500 and
     egresses through a 1380 tunnel will have large packets dropped if PMTU
     discovery does not work through it — which is exactly the shape of a TLS
     handshake that opens and then stalls.

     **Do not write this up as the root cause.** Apple's endpoints were reached
     from the host *through the same tunnel*, so the tunnel is not simply
     blocking them. The mismatch is a prime suspect with a plausible mechanism
     and no proof. What settles it is the guest-side hop walk
     (`ifconfig -a` → `ipconfig getpacket en0` → `netstat -rn` → ping router →
     ping `17.253.144.10` → DNS → `curl` an Apple endpoint), which localises
     the failure to one hop instead of guessing. `VM-BRINGUP.md`
     § Recovery network diagnosis carries the walk and the full measurements.
   - **Offline install** — attach a full installer as an additional disk and run
     it from there instead of pressing Continue. **Hard precondition: build
     24E263**, matching the recovery environment; any other build swaps this
     blocker for a later, worse one.

     Host-side viability was checked on 2026-08-07 and the path is **viable but
     not yet executed**. Three findings save the wrong first move:

     - **No full installer exists on disk, and re-running the fetcher will not
       produce one.** `fetch-macOS-v2.py` only POSTs to
       `osrecovery.apple.com/InstallationPayload/RecoveryImage`; its own source
       points at `corpnewt/gibMacOS` for full installers. "Just re-run the
       fetcher" is the obvious first idea and it is wrong.
     - **The recovery image is not secretly an installer.** It contains
       `InstallAssistant` (221 hits), `SharedSupport.dmg` (4) and
       `Install macOS Sequoia.app` (4) — all **code and plist references**, the
       recovery environment describing an installer it expects to find. 3 GB
       cannot hold a 15 GB `SharedSupport` regardless.
     - **The ISO step is solved, and there is a tool for it.** No `mkisofs` /
       `genisoimage` / `xorriso` / `oscdimg` on this host (the ADK here ships
       only DISM and Licensing), but **`pycdlib` 1.20.0 works** — verified by a
       real build-and-readback. Use
       **`tools/build-installassistant-iso.py`** rather than calling `pycdlib`
       by hand: it sets the volume identifier to exactly `InstallAssistant`,
       which `run_offline.sh` hardcodes as `/Volumes/InstallAssistant`, and any
       other label is a **silent** failure. `--self-test` proves the toolchain
       before you spend ~15 GB of download on it; `--verify` reads a built ISO
       back and checks the label. See `tools/README.md`.

     **Still unverified:** the ~15 GB `InstallAssistant.pkg` download for
     24E263, and `scripts/run_offline.sh`, which hardcodes
     `Install macOS Ventura.app` **twice** and must be retargeted to Sequoia or
     it fails as written. `STEPS.md` Step 5 § *If Recovery says "An Internet
     connection is required"* has the full five-step path and marks which steps
     rest on executed evidence.
3. **GSP-RM and Booter firmware blobs** — version-locked to whichever
   `open-gpu-kernel-modules` release the kext's register/RPC transcription came
   from. The RPC structures change between releases; a mismatched pair will not
   boot.

   **PARTLY RESOLVED 2026-08-07 (§7b), with a version gap that must be closed.**
   `reference/ga107-firmware/` now holds the GA107 bootloader and both SEC2
   booters — but at **570.144**, pulled from the laptop's `nvidia-595-open`
   install. The kext's constants were transcribed from **v610.57.04**. Those are
   not the same release, and this is exactly the mismatch this blocker warns
   about.

   Three ways out, in order of preference:

   - **Match the source to the blobs.** Re-verify `NVFalcon.hpp` against the
     570.144 headers instead of 610.57.04. `tools/check-registers.py` already
     does this mechanically — point it at a 570.144 checkout and see what breaks.
     Register offsets rarely move within an architecture; the RPC *structures*
     are where drift bites, and Phase 4's first milestone (bootloader DMA into
     IMEM) predates any RPC traffic.
   - **Match the blobs to the source.** Fetch the 610.57.04 `.run` installer and
     extract its blobs. This needs a download the laptop can do.
   - **Neither, for now.** The immediate Phase 4 work — reset, scrubbing poll,
     `DMATRFBASE` programming, the 256 B IMEM loop — uses the bootloader's
     descriptor and no RPC structures at all. The 570.144 bootloader is
     sufficient to write and test that much. The gap becomes blocking at the
     Booter Load / init-RPC stage, not before.

   **Do not treat the blobs as version-matched.** The one thing that would make
   this silently wrong is assuming they are.

### The sequence, from `kernel_gsp_tu102.c`

NVIDIA's HAL routes GA10x GSP bootstrap to the `_TU102` implementation — one
code path covers Turing and Ampere. Full detail in `PHASE1-GSP-NOTES.md`.

`kgspPrepareForBootstrap_TU102` (line 425): wait for GFW boot → confirm WPR2 is
down → prepare the FWSEC command. **Phase 3.75 already reads all three
preconditions.**

`kgspBootstrap_TU102` (line 522): scrubber → FWSEC/FRTS → reset into RISC-V →
LIBOS boot args into mailboxes → Booter Load → init RPCs → program `FALCON_OS`
→ confirm RISC-V active → link status queue → wait for RM init done.

### Honest estimates

| Phase | Estimate |
|---|---|
| 4 — GSP firmware boot | 3–12 months |
| 5 — memory + command submission | 6–18 months |
| 6 — compute | blocked on undocumented SASS |
| 7 — Metal | not achievable — private ABI |

The bulk of Phase 5 is NVIDIA's `nvoc` object model: thousands of generated C
lines from an internal OO preprocessor. Port a subset by hand, or write a
translator.

---

## 9. Resume checklist

- [x] Run `python tools/check-registers.py` — expect **113/113**, 0 failed, 0 skipped *(measured 2026-08-11. The previous revision of this line expected **102/102**, which predates the `NVGspStage.hpp` constants — a run reporting 113 against that expectation reads as a discrepancy when it is growth. Same rule as the two lines below: a HIGHER count is growth, a LOWER one is a regression, and the question to ask is which header stopped being scanned)*
- [x] Run `tests\build-msvc.bat` — expect **5/5 suites, 634 checks** (39 decode + 169 falcon + 85 RISC-V ucode + 199 GSP boot + 142 GSP stage) *(measured 2026-08-11 by running all five binaries directly, which is the only authority for a runtime count. The previous revision of this line expected 3/3 and 289 (39 + 165 + 85): that predates `test_gsp_boot` and `test_gsp_stage` entirely, and `test_falcon` has itself grown from 165 checks to 169 since. **Treat any figure here as a floor with a date on it — a HIGHER count is growth, a LOWER one is a regression, and the question to ask is which suite stopped being discovered.** An earlier revision still expected 2/2 and 204)*
- [x] Run `tests\check-kext-syntax.bat` — expect 5/5 stages, exit 0 *(new gate 2026-08-07: clang 19.1.5 parses the kext clean; 4/4 planted defects diagnosed; see `tests/kextshim/README.md`)*
- [x] Run `tests\run-python-tests.bat` — expect **356 tests**, `OK` *(measured 2026-08-11, twice consecutively. An earlier stamp made this same day said **348**; a peer landed 8 more tests within the session, which is exactly why a figure here is a dated floor and not a target. The revision before that expected **156**, which now reads as a false regression report. Gate first added 2026-08-07: `unittest discover -s tests`, so the count grows as suites land — it was 12 when the gate was written, 67 mid-day, 97 after that, and **156** once `tests/test_vm_type.py` landed (59 tests). **A lower number is a regression, not a stale expectation — check which suite stopped being discovered.** Covers `tools/dmg2raw.py` mish offsets (the header fields at 8/16 were right while the chunk count at 200 and chunk array at 204 were not, which is why that bug was silent), the `vm-screenshot.py` logic, the `vm-type.py` pure helpers (scancode tables, `parse_key` case handling, `pick_window` ambiguity, `resolve_click_at` bounds), and the `check-registers.py` / `collapse-sequence.py` parsers. CPython only, no MSVC/CMake needed — the one gate that runs on a bare checkout)*
- [x] GA107 firmware extracted from the laptop — `reference/ga107-firmware/` *(2026-08-07, §7b. Laptop reachable at `kenny@192.168.0.180`, key auth, no password needed)*
- [~] Phase 5 kext-side firmware staging — **landed 2026-08-10, covered by ZERO tests.** `copyGspFirmwareBlob()`, `loadGspFirmware()` and `releaseGspFirmware()` now exist in `NV3050Probe.cpp`; `handoff5.md` §7 previously said this was "not started at all" and that section has been rewritten. All four gates re-run green after the change (syntax 5/5, `ctest` 4/4, python `EXIT=0`, line-endings 9). **What that does NOT mean:** the DMA states still have no test, `fixtureImage` in `tests/test_gsp_boot.cpp` is still unreferenced, nothing has been built with Apple clang, and no blob is injected anywhere — with no `NVGspFirmware` property present the machine still stops at `FIRMWARE_NOT_IMPLEMENTED`, which is the unchanged normal path. Two constants there are **not interchangeable**: `NV_GSP_FW_PHYS_MASK` is an allocation constraint whose low 8 bits are cleared to force 256-byte alignment, `NV_GSP_FW_PHYS_LIMIT` is the `(1<<41)-1` ceiling — comparing a returned address against the mask reads as correct and is wrong
- [ ] Read `PHASE4-GSP-BRINGUP.md` before touching `NV3050Probe.cpp`
- [ ] Read `HARDWARE.md` § verdict before proposing display work
- [ ] Capture the trace via the **blacklist route only** (§6)
- [x] Step 4 — VMware Workstation 17.6.4.24832109 + `vmrun` 1.17.0 already installed *(verified 2026-08-07 via unlocker `check.exe`; laptop ruled out as host — only ~40 GB free)*
- [x] Free ~120 GB on the desktop — done 2026-08-07, `C:` has 204 GB free. Host the VM on `C:`, not `D:` (86 GB)
- [x] Run DrDonk's `unlocker` elevated — **DONE 2026-08-07, and it stays required.** v4.2.8, staged at `C:\Users\Kentl\macos-vm\unlocker428\windows\`. `check.exe` now reports all four VMware binaries `Patch Status: Patched (1)` against VMware 17.6.4.24832109. An unpatched `vmware-vmx.exe` refuses to power on a `darwin*` guest regardless of how the `.vmx` was authored, and the patch reverts on any VMware update — re-run `check.exe` first when a working VM stops powering on. Undo with `relock.exe` in the same folder. Guest-tools ISOs are in `..\iso\` — the image in use is macOS 15.4.1, so use **`darwin.iso`** (`darwinPre15.iso` is for 14 and earlier)
- [x] Fetch the recovery image via `OSX-KVM`'s `fetch-macOS-v2.py` — done 2026-08-07. `C:\Users\Kentl\macos-vm\sonoma\BaseSystem.dmg`, 884,317,790 bytes, **all 85 chunklist SHA-256 chunks verified byte-exact**. **What it actually contains is macOS 15.4.1 Sequoia (build 24E263)**, not Sonoma — read from the embedded `SystemVersion.plist`; the marketing string "Install macOS Sonoma" appears **zero** times in the image. The `sonoma\` directory name is now just a filename
- [~] Build the macOS VM (`STEPS.md` 5–6) — **boots to a live, interactive macOS Recovery; nothing installed yet.** 2026-08-07: `.vmx` hand-written, and the VM reaches the **macOS Recovery menu** (screen capture — Recovery/File/Edit/Utilities menu bar, "Reinstall macOS Sequoia", Disk Utility, Safari); the user clicked around in it and it responds normally. **RETRACTED: an earlier revision of this line said the guest froze, blamed `Monitor Mode: ULM`, and told you to run `bcdedit /set hypervisorlaunchtype off` plus disable Memory Integrity / Core Isolation — do not do that.** 0 changed pixels over 8 s is what an *idle live* Recovery menu looks like (no clock, no spinner, no animation), and `Monitor Mode: ULM` is normal on Windows 11, not an error. `BaseSystem.dmg` had to be converted to raw first (`BaseSystem.img`, 3,221,110,784 bytes) — VMware cannot read a *compressed* UDIF and silently presents it as `capacity=0` → `msg.Backdoor.OsNotFound`. The image is **macOS 15.4.1 Sequoia, build 24E263**, not Sonoma. **The erase is already DONE** — Disk Utility ran 13:28–13:29 and the ~100 GB target is APFS-formatted; do not redo it. The Recovery blocker *was* the payload fetch: it stopped at *"An Internet connection is required"* because the 15.4.1 recovery kernelcache carries no Intel ethernet kext and `ethernet0.virtualDev = "e1000e"` never binds, so `en0` never existed and no `DHCPDISCOVER` was ever sent. **RESOLVED 2026-08-07 — an earlier revision of this line said `vmxnet3` was "edited on disk but UNTESTED"; that is no longer true and acting on it would mean a needless power cycle.** The VM was power-cycled, the guest bound `en0`, and the install proceeded past Recovery to a **macOS Finder desktop**. Verified by positive, guest-originated evidence only — a host-side absence proves nothing here:

  | Check | Result |
  |---|---|
  | `vmnetdhcp.leases` | `192.168.255.129`, `hardware ethernet 00:0c:29:e7:9f:2b`, `client-hostname "Mac"` — a driverless NIC cannot send a hostname option |
  | lease chain | five successive leases 18:52Z→19:41Z, so `en0` stayed bound and *renewing*, not a one-shot boot artifact |
  | hostname change | `Mac` → `kennys-Mac` at 19:56Z — the generic Recovery hostname became the installed system's |
  | `arp -a` on VMnet8 | host learned `00-0c-29-e7-9f-2b` from a guest-originated frame |
  | `ping -n 4` | 4/4, `TTL=64` — replies *generated* by the guest, not forwarded |
  | `Get-NetNeighbor` | `State : Reachable` on `VMware Network Adapter VMnet8` |

  Two host measurements are **unavailable on this machine** and their zeroes are artifacts, not evidence: `Get-NetAdapterStatistics` has no `MSFT_NetAdapterStatisticsSettingData` for either VMnet adapter, and `Win32_PerfRawData_Tcpip_NetworkInterface` does not enumerate them at all. Use lease + ARP + neighbor state. `arp -d` also needs elevation and exits `0` when it fails. Still unproven: **routed/NAT egress to the internet** — every check above is host↔guest on the VMnet8 segment. Read `VM-BRINGUP.md` (Failures 1–5 and the acceptance section) before touching this
- [ ] First `make` — expect failures, they are fixable
- [ ] Load on the laptop, read `GSPBootable` in IORegistryExplorer
- [ ] If `GSPBootable` is true, Phase 4 is viable and the firmware push begins

### Two rules that were earned the hard way

**Never declare a register outside `NVDecode.hpp` or `NVFalcon.hpp`.** One place
for a transcription error to hide, and it is checked.

**Never route a GSP access through a bare offset.** Use `nvGspFalconReg()` or
`nvGspRiscvReg()`. The three-address-space hazard is documented at the top of
`NVFalcon.hpp`, and it does not fault when you get it wrong.
