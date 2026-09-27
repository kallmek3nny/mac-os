# VM-BRINGUP.md — getting the macOS installer to power on

**Written 2026-08-07. Records four consecutive power-on failures on the desktop
host and the fix for each.** Every claim here is taken from `Sonoma.vmx`,
`vmware.log`, `chk.txt`, or the on-disk artifacts — not from memory.

Host: VMware Workstation 17.6.4 build 24832109, Windows 11.
VM directory: `C:\Users\Kentl\macos-vm\sonoma\`.

> **The image is macOS 15.4.1 Sequoia (build 24E263), not Sonoma.** Read from
> the embedded `SystemVersion.plist` in the raw image — see `STEPS.md` line 429.
> The directory, the vmdk, the VM's `displayName` and this file's filename are
> all still *named* Sonoma from before that was known. **The names are wrong and
> renaming them would break the VMX's relative paths for no benefit.** What
> matters downstream: `guestOS = "darwin24-64"` (macOS 15), and guest tools need
> `darwin.iso`, **not** `darwinPre15.iso`.

> **The build number above describes the INSTALLER IMAGE ONLY, and the install
> has since finished.** `24E263` is what the raw recovery image reports and that
> remains correct for the image. It is **not** what is running. `Sonoma.vmx`
> line 236 carries a *guest-originated* write-back:
>
> ```
> guestInfo.detailed.data = "... buildNumber='24G830' distroName='macOS'
>   distroVersion='15.7.9' familyName='Darwin' kernelVersion='24.6.0'"
> ```
>
> That is the installed system reporting its own version through VMware Tools:
> **macOS 15.7.9, build 24G830**, booted to a desktop (confirmed from pixels in
> `artifacts/boot-20260807T2110Z.png`, not inferred). The recovery installer did
> its job and the installed OS superseded it. Anything in this file that reads as
> "the guest is sitting in Recovery" is describing bring-up history, not current
> state — see `handoff3.md` §1.

---

## Why this file exists

None of the four failures produced an error message that named its actual
cause. Each one presents as a *different* problem than it is:

| What you see | What it actually is |
|---|---|
| "VMware Workstation does not support this guest OS" | `vmware-vmx.exe` is unpatched |
| `[msg.Backdoor.OsNotFound]` | the attached image has `capacity=0` |
| EFI shell / no boot device, media clearly attached | the image is presented as a CD, not a disk |
| EFI enumerates but boots nothing | installer and blank target are on swapped ports |

An hour is easy to lose on any of them. Read the failure that matches your
symptom, not the whole file.

---

## Current state

The VM powers on and reaches `poweredOn`. From `vmware.log`:

```
419  Powering on guestOS 'darwin23-64' using the configuration for 'darwin23-64'.
1078 PowerOnTiming: ALL took 511489 us
1119 Transitioned vmx/execState/val to poweredOn
```

That log line is from the `darwin23-64` era. The VMX now carries
`guestOS = "darwin24-64"` (`Sonoma.vmx` line 17) to match the Sequoia image; a
current log will say `darwin24-64` on both halves of line 419.

The EFI ROM comes up as `EFI2.7 64-bit` (line 513) and both SATA disks open
cleanly (lines 528–529).

**Power-on is solved and the guest boots.** The macOS Recovery UI comes up and
responds to input — confirmed by the user interacting with the console on
2026-08-07. The next step is the installer itself (Disk Utility erase, then
Reinstall macOS), which is GUI work inside the guest.

See "The false alarm" below before trusting any headless liveness check on this
VM.

---

## Failure 1 — VMware refuses the guest OS type

**Symptom.** Setting a macOS `guestOS` value (`darwin23-64` at the time, now
`darwin24-64`) and powering on produces a refusal dialog. The macOS guest family is not offered in the New VM wizard at
all.

**Cause.** Stock VMware Workstation deliberately omits macOS guest support on
non-Apple hardware. The check lives in `vmware-vmx.exe` and `vmwarebase.dll`.

**Fix.** Apply DrDonk unlocker 4.2.8 and *verify the patch landed* rather than
assuming it did. `check.exe` must report `Patched (1)` for every binary it
inspects. Current output (`C:\Users\Kentl\macos-vm\chk.txt`):

```
VMware is installed at:  C:\Program Files (x86)\VMware\VMware Workstation\
VMware version:  17.6.4.24832109
Patch Status: Patched (1)
Patch Status: Patched (1)
Patch Status: Patched (1)
```

**Gotcha.** The unlocker patches binaries in place. Any VMware update, repair
install, or auto-update silently reverts all three and Failure 1 returns
looking like a brand-new problem. Re-run `check.exe` first whenever a
previously-working VM starts refusing to boot.

The unlocker also drops the VMware Tools images for macOS into
`..\unlocker428\iso\`. **Use `darwin.iso`, not `darwinPre15.iso`** — the
`Pre15` image is for guests older than macOS 15 and this image is 15.4.1. It is
wired to `sata0:2` with
`startConnected = "FALSE"` — present for later, deliberately not in the boot
path now.

---

## Failure 2 — `msg.Backdoor.OsNotFound` with the .dmg attached

**Symptom.** `BaseSystem.dmg` attached directly. The VM powers on, EFI
initialises, and the guest reports `[msg.Backdoor.OsNotFound]`. Reads exactly
like a boot-order misconfiguration.

**Cause.** It is not a boot-order problem. **VMware cannot read a compressed
UDIF image.** Apple ships `BaseSystem.dmg` as compressed UDIF; VMware attaches
it as a device with `capacity=0`. There is no readable media at all, so EFI
correctly reports that it found no OS.

This is documented at the top of `tools/dmg2raw.py`, which exists precisely
because of this failure.

**Fix.** Decompress the UDIF into a raw image, then present the raw image.

```
python tools/dmg2raw.py BaseSystem.dmg BaseSystem.img
```

Sizes confirm the conversion did real work:

| File | Bytes |
|---|---|
| `BaseSystem.dmg` (compressed UDIF) | 884,317,790 |
| `BaseSystem.img` (raw) | 3,221,110,784 |

**Diagnostic that saves the hour.** If a guest reports `OsNotFound`, grep the
log for the device's reported capacity *before* touching boot order. A
`capacity=0` device is the answer; boot order is a red herring.

---

## Failure 3 — attached as `cdrom-image`, EFI still boots nothing

**Symptom.** With the raw `BaseSystem.img` attached as
`deviceType = "cdrom-image"`, the media is genuinely present and non-zero —
and EFI still finds nothing bootable.

**Cause.** The macOS recovery image is **a GPT disk containing an Apple_HFS
partition, not an ISO9660 filesystem.** A `cdrom-image` device tells EFI to
look for an optical filesystem. It looks, finds no ISO9660, and stops.
Presenting a GPT/HFS+ image as a CD gives EFI nothing it recognises.

**Fix.** Present it as a **disk**, via a hand-written flat descriptor. That is
what `BaseSystem.vmdk` is — a text descriptor pointing at the raw `.img`:

```
createType="monolithicFlat"
RW 6291232 FLAT "BaseSystem.img" 0
```

The extent count is bytes ÷ 512: `3221110784 / 512 = 6291232`. **Get this
wrong and the disk opens but the partition table is truncated or overruns** —
another silent failure. Recompute it if the `.img` is ever regenerated.

The VMX then references the descriptor, not the image:

```
sata0:0.fileName   = "..\sonoma\BaseSystem.vmdk"
sata0:0.deviceType = "disk"
```

---

## Failure 4 — installer and target on swapped ports

**Symptom.** Everything above fixed. The VM powers on, EFI enumerates the
disks — and still boots nothing.

**Cause.** The blank 100 GB install target was on `sata0:0` and the installer
on `sata0:1`. `sata0:0` was a freshly-created empty vmdk: no partition table,
nothing bootable. EFI tried the first port, found an empty disk, and had
nowhere to go.

**Fix.** Swap them. The installer must be first:

| Port | Contents | Device type |
|---|---|---|
| `sata0:0` | `BaseSystem.vmdk` → recovery installer | `disk` |
| `sata0:1` | `Sonoma.vmdk` → blank 100 GB target | `disk` |
| `sata0:2` | `darwin.iso` → VMware Tools | `cdrom-image`, `startConnected = FALSE` |

This history is preserved as a comment block in `Sonoma.vmx` (lines 64–126).
Leave it there — the ordering looks arbitrary without it and is an easy thing
to "tidy" back into a broken state.

---

## Not a failure — the boot works

With Failures 1–4 fixed, the VM powers on with **no boot failure logged at all**:

```
1201  Guest: About to do EFI boot: EFI VMware Virtual SATA Hard Drive (0.0)
1205  Guest: Firmware has transitioned to runtime.
```

EFI finds the blessed volume, hands off, and **macOS Recovery comes up and
responds to input** — confirmed 2026-08-07 by the user clicking around in the
console.

Remaining work is GUI work inside the guest: Disk Utility → erase the 100 GB
target (APFS, GUID Partition Map) → **Reinstall macOS Sequoia** (the Recovery
menu names the image's own version, 15.4.1 — not Sonoma). Do not select the
3 GB device; that is `BaseSystem.vmdk`, the installer you booted from, and
Recovery will erase it without complaint.

---

## The false alarm — how a working guest was declared frozen

This is recorded because the reasoning was wrong in a way that looked rigorous,
and the proposed "fix" would have degraded the host for nothing.

**The claim.** After the clean handoff above, two headless measurements were
taken and read as proof of a hung guest:

| Measurement | Result | What it was taken to mean |
|---|---|---|
| Guest-rect framebuffer diff | **0** changed pixels | display frozen |
| `vmware-vmx` CPU over 20–25 s | **+0.12–0.28 s** | vCPUs halted |
| Target `Sonoma-s0*.vmdk` growth | 13.2 MB, unchanged | nothing installing |

All three numbers are accurate. **None of them supports the conclusion.**

- **macOS Recovery is a completely static screen.** No clock, no spinner, no
  blinking caret, no animation of any kind. A live, idle Recovery UI is
  *byte-identical* between two captures unless the mouse happens to be moving
  during the grab. Zero pixel delta is the expected reading for a healthy guest
  sitting at the menu.
- **An idle guest waiting on user input burns almost no CPU.** ~1% of one core
  is indistinguishable from halted vCPUs at this resolution.
- **The target disk has no reason to grow** before the user starts the install.

**The bad diagnosis that followed.** Three log lines were then assembled into a
root cause:

```
61    IOPL_Init: Hyper-V detected by CPUID
427   Monitor Mode: ULM
1139  Syncing WHP TSCs took 31 us
```

plus host-side `hypervisorlaunchtype Auto`, `HypervisorPresent True`, `VBS
status 2`. The story was that VMware running under WHP bypasses its native
monitor, so the unlocker's SMC patch never executes. It is a *plausible*
mechanism, and every fact in it is true — VMware genuinely is in ULM on this
host. **It is simply not what was happening**, because the guest was never hung.

The proposed remedy was `bcdedit /set hypervisorlaunchtype off` + reboot, which
would have disabled WSL2, Docker Desktop, Windows Sandbox, Hyper-V, and VBS
host-wide. **Do not do this.** Nothing in this project needs it.

**Correct reading of `Monitor Mode: ULM` on this host: benign**, in the same
category as the Hyper-V CPUID line and `Can't set WSS, error 1314`. All three
appear in every log from this machine, including the four runs that failed for
four unrelated reasons.

**The lesson, stated as a rule.** There is no reliable *headless* liveness test
for this VM before macOS is installed. `vmrun captureScreen` and every guest RPC
path require VMware Tools, which does not exist until after the install — the
log shows `VIX_E_TOOLS_NOT_RUNNING` and `GuestRpcSendTimedOut` accordingly. A
human interacting with the console is the only ground truth available at this
stage. Screenshot diffing can prove a guest is *alive* (pixels changed); it can
never prove one is *dead*.

**Secondary trap found while chasing this.** An early diff of two captures
reported 642 changed pixels and was read as a live guest. The diff bbox was
`(37,7)–(202,16)` — a 165×9 strip in the VMware **toolbar**. Guest-area delta
was 0. Always crop to the guest rect (in a 1228×933 window capture: `x ≥ 204`,
`y` 77–816) before comparing; the window chrome redraws on its own.

### Closed by positive evidence — 2026-08-07 17:28 UTC

Everything above rests on the user's report plus an argument about what the
numbers *could* mean. It is now settled by measurement in the other direction —
evidence a hung guest could not have produced:

| Evidence | Value |
|---|---|
| Guest-rect framebuffer delta | **62.02%**, bbox `(202,96)–(1209,826)` |
| Frame md5 before → after | `f70bde54…` (3 consecutive frames) → `e9a61ae6…` |
| Log advance after 40 min of silence | `17:22:59Z` sata0:0 COMRESET, `17:28:37Z` sata0:1 (**source log no longer exists** — see the rotation note at the DHCP table below) |
| `sata0:1` descriptor | `DDB: longContentID = b7043146… (was ac16df19…)` |

The 1007×730 changed region sits entirely inside the guest rect — that is the
macOS UI repainting, not chrome. The `longContentID` change on `sata0:1` means
**`Sonoma.vmdk` was written to**: the 100 GB target is being formatted, which is
exactly the Disk Utility step this file says to do next. Those AHCI events are
emitted by `vcpu-1/2/3`; halted vCPUs cannot emit them.

Note the earlier static run was *also* real — three consecutive captures shared
one md5 while the guest was alive throughout. **Static and alive are not
contradictory.** That assumed contradiction was the whole error.

**Why the halt signature was indistinguishable.** `0 of 19 vmware-vmx threads
runnable`, `~1% of one core`, and `0 read/write I/O ops` were read as halted
vCPUs. They are equally the signature of a healthy guest blocked in a WHP
vmexit waiting for input: Windows reports WHP-blocked vCPU threads as
`Wait/UserRequest`, the same state as an idle thread. No extra precision on
those three metrics would have separated the two cases.

**The rule, sharpened.** All four measurements in the bad diagnosis were
*negative* tests — each asked whether something failed to happen, and a negative
result from a negative test is uninformative. To show a guest is dead you need a
**positive test a live guest would pass**: input that provably reaches it, or an
event that must fire on a timer. `tools/vm-screenshot.py --compare` is
one-directional by construction, and its docstring now says so.

**And `--compare` is not the before/after instrument either.** It captures both
frames itself, *after* whatever you did, so it cannot attribute a change to an
action — that limit is inherent and no fix removes it. Its argument is a base
name: `--compare motion.png` writes `motion.frame1.png` and `motion.frame2.png`
and never touches `motion.png`. (It *used* to save frame 2 over the named file,
which is how eight "baselines" in `shots/` were destroyed and how the retracted
floor below came to exist. Fixed 2026-08-07 in `tools/vm-screenshot.py`, guarded
by `tests/test_vm_screenshot.py::DerivedFramePathTests`.) To attribute a
framebuffer change to an action, save the frames yourself and diff them with the
read-only differ:

```sh
python tools/vm-screenshot.py --window "Sonoma - VMware" --out before.png
#   ... perform the action ...
python tools/vm-screenshot.py --window "Sonoma - VMware" --out after.png
python tools/png-delta.py before.png after.png --floor <pct-from-a-fresh-control>
```

`png-delta.py` never captures. `--floor` is a **percentage** measured from an
idle control pair, not a file path. This is the positive test; `--compare` is
not.

**The `0.02` floor this file used to hardcode is retracted.** Every before/after
pair in `shots/` was byte-identical — one frame counted twice — so the floors
read off them measured the capture pipeline, not the guest (see the retraction
block in `handoff3.md`). **No valid idle floor exists on record for this guest.**
Measure a fresh control (capture, wait, capture, **no action between**), confirm
the two md5s differ, and use that number. `png-delta.py` now exits **3** on
byte-identical input rather than printing `0.000%`.

---

## Three traps that are not failures yet

### VMX edits do not reach a running VM

Every `.vmx` setting in this file — `guestOS`, the `sata0:*` port assignments,
`sata0:2` pointing at `darwin.iso` — is read **once, at power-on**. Editing the
file while the VM is running changes nothing about the running guest, and
VMware does not warn you. Worse, VMware rewrites parts of the `.vmx` itself
during shutdown, so an edit made mid-session can be silently discarded.

So the sequence is: **shut the guest down, edit, power on.** Not suspend/resume
— a resumed VM restores the saved device state, not the file. If a change you
know you made appears to have had no effect, check that the VM was actually
powered off when you made it before looking anywhere else.

Backups from this session's edits live alongside the file (e.g.
`Sonoma.vmx.bak-20260807`); diff against one to confirm what actually persisted
rather than what you intended to write.

### Windows path case collision

`C:\Users\Kentl\macos-vm\Sonoma` and `...\macos-vm\sonoma` are **the same
directory** — NTFS is case-insensitive. The fetch script created `sonoma`; a
later step created `Sonoma`. `vmrun` reports the VM as `...\sonoma\Sonoma.vmx`.

Consequence: the VMX's relative path `..\sonoma\BaseSystem.vmdk` goes up to
the parent and back into the *same* directory. **It works by accident.** Do not
"simplify" it to a bare filename without testing the change — and be aware that
any tooling treating the two spellings as distinct directories will be wrong.

### Disk usage is misleading

`du` on the VM directory counts the 8 GB `.vmem` and the 3 GB
`BaseSystem.img` alongside the sparse `Sonoma.vmdk`. The directory reads far
larger than the VM's actual footprint. The `.vmem` is a runtime artifact and
disappears on clean shutdown.

---

## Debugging notes for the next failure

**`msg.autoAnswer = "TRUE"` is set** (VMX line 155). Modal dialogs are
auto-dismissed, so a question VMware would normally *ask* you instead becomes a
silent log line. When something fails with no visible dialog, `vmware.log` is
the only record — grep it for `Msg_Post` and `msg.`.

Useful greps against `sonoma\vmware.log`:

```
grep -nE 'Msg_Post|msg\.[a-zA-Z]'        # suppressed dialogs
grep -nE 'capacity|OsNotFound|Backdoor'  # Failure 2 signature
grep -nE 'DISK: OPEN'                    # which images actually opened
grep -nE 'Transitioned vmx/execState'    # did it reach poweredOn
```

Ignore the two `config.ini` "cannot open file" lines at the top of every log
(lines 9–17). A missing per-user `config.ini` is normal and unrelated to any
failure above.

---

## Reproducing from scratch

1. Apply DrDonk unlocker 4.2.8; run `check.exe`; confirm `Patched (1)` ×3.
2. Fetch `BaseSystem.dmg` (+ `.chunklist`) into `macos-vm\sonoma\`.
3. `python tools/dmg2raw.py BaseSystem.dmg BaseSystem.img`.
4. Write `BaseSystem.vmdk` with extent count = `size(BaseSystem.img) / 512`.
5. Create the blank target: `Sonoma.vmdk`, 100 GB.
6. In the VMX: installer on `sata0:0`, target on `sata0:1`, both
   `deviceType = "disk"`; `guestOS = "darwin24-64"`; `firmware = "efi"`;
   `smc.present = "TRUE"`.
7. Power on. Confirm `Transitioned vmx/execState/val to poweredOn` in the log.

Step 7 succeeding means power-on works. It does **not** mean the installer
reaches its UI — that is `t-d3a036`.

---

## Host-side network audit (t-3adea0)

**Run 2026-08-07 ~18:10 UTC on the desktop host, VM powered on.** This section
records what the *host* looks like while Recovery shows "Internet connection is
required". It deliberately separates **proven** facts from **inconclusive**
tests, because three of my first tests turned out to be artifacts of the test
method rather than properties of the VM network.

### Topology, as configured

From `Sonoma.vmx`:

```
ethernet0.present          = "TRUE"
ethernet0.connectionType   = "nat"
ethernet0.virtualDev       = "e1000e"
ethernet0.addressType      = "generated"
ethernet0.startConnected   = "TRUE"
ethernet0.pciSlotNumber    = "160"
ethernet0.generatedAddress = "00:0c:29:e7:9f:2b"
```

`vmware.log` confirms the same values were actually applied at power-on
(the `DICT ethernet0.*` lines), so the VMX is not being overridden.

Host side, NAT is VMnet8:

| Element | Value | Source |
|---|---|---|
| VMnet8 host adapter | `192.168.255.1/24`, up | `ipconfig /all` |
| NAT gateway | `192.168.255.2` | `vmnetnat.conf` `[host] ip` |
| DHCP scope | `192.168.255.128`–`192.168.255.254` | `vmnetdhcp.conf` |
| `option routers` | `192.168.255.2` | `vmnetdhcp.conf` |
| `option domain-name-servers` | `192.168.255.2` | `vmnetdhcp.conf` |

Note both the gateway **and** the DNS forwarder are `.2`, not `.1`. A guest
pointed at `192.168.255.1` for DNS would fail even though everything else works.

### RETRACTED — the lease is from a *previous* boot, not this one

`C:\ProgramData\VMware\vmnetdhcp.leases`:

```
lease 192.168.255.129 {
	starts 5 2026/08/07 16:20:12;
	ends   5 2026/08/07 16:50:12;
	hardware ethernet 00:0c:29:e7:9f:2b;
}
```

I first read this as proof that the running guest had working DHCP. **That was
wrong, and the timestamps are what kill it.**

> **Two provenance corrections to this table, both measured 2026-08-09.**
>
> 1. **The times really are UTC**, so the `(UTC)` header is right. Confirmed
>    two ways: `vmnetdhcp.leases` opens with VMware's own
>    `# All times in this file are in UTC (GMT), not your local timezone`, and
>    every `vmware*.log` line carries an explicit `Z`. The host is
>    America/New_York = UTC−4, so file *mtimes* render four hours earlier —
>    that offset is a display convention, not a discrepancy.
> 2. **The cited log is gone, and an earlier revision of this note guessed
>    wrong about where it went.** VMware keeps only four boot logs
>    (`vmware.log` → `vmware-0` → `-1` → `-2`) and discards the fifth on each
>    power-on. Measured 2026-08-09, the surviving set is:
>
>    | file | first line (UTC) |
>    |---|---|
>    | `vmware.log` | `2026-08-09T01:33:16.516Z` |
>    | `vmware-0.log` | `2026-08-09T01:08:08.812Z` |
>    | `vmware-1.log` | `2026-08-07T22:39:23.419Z` |
>    | `vmware-2.log` | `2026-08-07T18:51:31.356Z` |
>
>    The earliest surviving log line is `18:51:31.356Z`. The `16:36:53Z` boot
>    in the table below, and the `17:22:59Z` / `17:28:37Z` COMRESETs above,
>    **all predate it — they are in no surviving log and cannot be
>    re-verified.** (`vmware-PREBOOT-20260807T2100Z.log` was deliberately
>    preserved but covers `22:39:23.419Z`–`22:43:08.021Z`, not this window.)
>
>    A revision of this note written minutes earlier claimed the boot "is now
>    `vmware-2.log` line 1" and that `vmware-2.log:340` reads
>    `ethernet0.virtualDev = "e1000e"`. Both were inferred from the rotation
>    pattern rather than measured, and both are **false**: `vmware-2.log` L1 is
>    `18:51:31.356Z`, its `Powering on guestOS` is L418 at `18:51:31.641Z`, its
>    L340 reads `vmxnet3`, and its first `sata0:0 Port COMRESET` is L1796 at
>    `19:37:29.530Z`. Rotation is fast enough to invalidate a citation *inside a
>    single session*: the file that was `vmware-0.log` when this file was first
>    edited had become `vmware-2.log` roughly an hour later.
>
>    **Therefore: never cite `vmware.log:<N>` by filename alone.** Record the
>    boot's L1 timestamp alongside it, and copy the quoted lines into the doc —
>    the log itself is not durable evidence.

| Event | Time (UTC) | Source |
|---|---|---|
| Lease starts | **16:20:12** | `vmnetdhcp.leases` |
| Lease ends | **16:50:12** | `vmnetdhcp.leases` |
| **This VM instance powered on** | **16:36:53Z** | `vmware.log` line 1 / `Powering on guestOS`, **as read on 2026-08-07; that log has since been rotated away** |
| Leases file last written | **16:20** (12:20 EDT) | `ls -la` mtime |
| Audit ran | 18:09–18:24 | `date -u` |

The lease was issued **16 minutes before the currently-running VM booted**, and
the leases file has **not been written since 16:20 UTC** — so no DHCP
transaction has occurred at any point during this VM's uptime. The lease belongs
to an earlier boot of the same VM (same MAC, persistent because
`ethernet0.addressType = "generated"` is stable across boots).

What *is* still solid: the runtime MAC really is `00:0c:29:e7:9f:2b`, but the
proof is `vmware.log` line 690, `Ethernet0 MAC Address: 00:0c:29:e7:9f:2b`, and
**not** the on-disk VMX (see the config-drift section below for why the VMX is
not a valid description of the running VM).

So the correct statement, **as of this audit (2026-08-07 ~18:24 UTC, the e1000e
session)**, was: the guest had never been observed obtaining a DHCP lease. "No
DHCP" went back on the suspect list as the *leading* suspect.

> **Since resolved — do not read this paragraph as current state.** After the
> vmxnet3 swap and a power cycle at 18:51:31Z, the guest obtained a lease at
> **18:52:35Z** with `client-hostname "Mac"`. See *Acceptance test — PASSED
> 2026-08-07 18:52:35 UTC* below. This section is preserved as the diagnostic
> record that led there, not as a description of the running VM.

Supporting but weaker signal — VMnet8 byte counters were sampled three times,
30 s apart:

```
18:20:17Z   1500  MediaSense 1   BytesIn 8562   BytesOut 63070
18:20:48Z   1500  MediaSense 1   BytesIn 8562   BytesOut 63070
18:21:18Z   1500  MediaSense 1   BytesIn 8562   BytesOut 63070
```

Flat across 90 s. That is consistent with a guest that is not even retrying
DHCP, but **absence of counter movement is not proof** of a dead NIC — same
one-directional-evidence rule as the frozen-guest false alarm. Treat it as
corroborating, never as the finding itself.

Also note `arp -a -N 192.168.255.1` shows only `192.168.255.254`
(`00-50-56-e4-43-4c`, a VMware `00:50:56` host OUI — the vmnet8 pseudo-endpoint).
The guest MAC `00:0c:29:e7:9f:2b` is **absent from the host ARP table**, and a
ping sweep of `192.168.255.128–135` returned all timeouts. The sweep is
inconclusive on its own for the VPN reason described below, but the missing ARP
entry is independent of that artifact and points the same way.

### Config drift — the on-disk VMX is not the running VM

While checking the lease timing I found the on-disk `Sonoma.vmx` and the running
instance disagree about the guest OS type:

| Where | Value | Evidence |
|---|---|---|
| `Sonoma.vmx` on disk | `guestOS = "darwin24-64"` | line 17 of the VMX |
| Applied at power-on | `guestOS = "darwin23-64"` | `vmware.log:305` `DICT guestOS`, `vmware.log:419` `Powering on guestOS 'darwin23-64' using the configuration for 'darwin23-64'` |

The timestamps explain it and there is no mystery:

```
VM powered on      2026-08-07 16:36:53 UTC   (vmware.log line 1)
Sonoma.vmx edited  2026-08-07 16:59    UTC   (mtime, 12:59 EDT)
```

The VMX was edited **22 minutes after** this instance booted. VMware reads the
VMX once at power-on, so the `darwin24-64` correction on disk has never been
applied to the process that is currently running. This is the trap already
recorded above in *"VMX edits do not reach a running VM"* — hit again, this time
while auditing rather than while configuring.

**Consequence for anyone reading this audit:** do not quote the on-disk VMX as a
description of the running VM. Quote `vmware.log`'s `DICT` block, which is the
configuration that was actually applied. The networking values in the topology
table above survive this test — `vmware.log:338–342` shows `ethernet0.present`,
`connectionType = "nat"`, `virtualDev = "e1000e"` and `pciSlotNumber = "160"`
were all applied as written, and line 690 confirms the runtime MAC. But that is
because they were checked against the log, not because the VMX agreed.

**Consequence for the fix:** whatever the guest-side walk concludes, the guest
OS type change and any `ethernet0` remedy (MTU clamp, `connectionType` change)
require a **full power cycle**, not a reboot of the guest and not a live VMX
edit.

### Proven: the host services and the host's own path are healthy

```
VMnetDHCP            RUNNING   (vmnetdhcp.exe pid 5660)
VMware NAT Service   RUNNING   (vmnat.exe     pid 5740)
ping 1.1.1.1              2/2 replies, ~26 ms avg
nslookup gdmf.apple.com   -> 17.171.47.23 (via gdmf.v.aaplimg.com)
ping 192.168.255.1        2/2 replies, 0 ms
```

So the host itself has working DNS and working internet, and both VMware
networking services are alive.

### The complication: a VPN owns the default route

```
Publish  Type      Met  Prefix        Idx  Gateway/Interface
No       Manual    0    0.0.0.0/0      18  192.168.0.1        <- Wi-Fi
No       Manual    0    0.0.0.0/0      60  10.64.0.1          <- Mullvad tunnel
```

Mullvad is **Connected** (relay `us-qas-wg-101`, WireGuard), and its tunnel
interface holds the winning `0.0.0.0/0`. Relevant settings:

| Setting | Value |
|---|---|
| Lockdown mode ("block when disconnected") | **off** |
| LAN sharing | **allow** |
| Split tunneling | on — only two Google NearbyShare exes excluded |
| Custom DNS / content blocking | none |
| Tunnel MTU | **1380** (VMnet8 is 1500) |

`vmnat.exe` is **not** in the split-tunnel exclusion list, so NAT-forwarded guest
traffic egresses through the tunnel.

### Inconclusive — do not cite these as evidence

Three tests failed and I initially read them as a VMnet8 block. Controls show
they are artifacts:

| Test | Result | Why it proves nothing |
|---|---|---|
| `ping -S 192.168.255.1 1.1.1.1` | General failure | Control: `ping -S 192.168.0.237` (Wi-Fi) and `ping -S 192.168.56.1` (VirtualBox) **also** fail, while `ping -S 10.148.93.182` (tunnel) succeeds 2/2. Every non-tunnel source fails, because the tunnel drops non-tunnel-sourced packets. Property of the VPN, not of VMnet8. |
| `ping 192.168.255.2` | 3/3 timeout | `vmnat` does not reliably answer host-originated ICMP. Expected even on a healthy NAT. |
| `nslookup gdmf.apple.com 192.168.255.2` | 4× timeout | `vmnat`'s DNS forwarder serves guest-originated queries; a host-originated one timing out is not a fault signal. |

The lesson matches the one already recorded in *"The false alarm — how a working
guest was declared frozen"*: **a negative result from an untested method is not
evidence.** Run the control first.

### What this leaves for the guest-side walk (t-ab1317)

Steps 1–2 (`ifconfig -a`, `ipconfig getpacket en0`) are expected to **pass** with
`192.168.255.129`. The live search space therefore starts at step 3:

1. `netstat -rn` — is there a default route, and is it `192.168.255.2`?
2. `ping -c3 192.168.255.2` — L3 to the NAT gateway.
3. `ping -c3 1.1.1.1` — routed off-subnet.
4. `dig +short apple.com` / `nslookup apple.com 192.168.255.2` — DNS alone.
5. `date` — a wrong guest clock breaks TLS and looks identical to no-network.
6. `curl -sSv https://gdmf.apple.com/v2/pmv 2>&1 | head -20` — TLS/HTTP.
7. `ping -c3 -s 1400 1.1.1.1` — **MTU probe.** The tunnel is 1380 and VMnet8 is
   1500. If small pings work but TLS or `curl` hangs mid-transfer, this
   mismatch is the prime suspect, and the fix is clamping guest MTU (or
   `ethernet0` MTU) below 1380.

Record the **first** step that fails; that single hop is the diagnosis. Do not
assert a cause an earlier passing step did not establish.

---

## CONFIRMED CAUSE — the recovery kernelcache has no e1000e driver (t-c14570)

**Run 2026-08-07 ~18:35 UTC, host-side only, VM untouched.** This is positive
evidence, not an absence argument: the recovery kernelcache's prelinked bundle
list was *enumerated*, and the Intel driver is not in it.

`Sonoma.vmx` / `vmware.log:340` both set `ethernet0.virtualDev = "e1000e"`, so the
guest is presented an **Intel 82574L-class PCI NIC**. Every ethernet-capable kext
prelinked into `BaseSystem.img` (15.4.1 Sequoia, build 24E263):

```
/System/Library/Extensions/AppleUSBEthernet.kext
/System/Library/Extensions/AppleUSBEthernetHost.kext
/System/Library/Extensions/AppleUSBNetworking.kext
/System/Library/Extensions/IONetworkingFamily.kext
/System/Library/Extensions/IONetworkingFamily.kext/Contents/PlugIns/AppleBCM5701Ethernet.kext
/System/Library/Extensions/IONetworkingFamily.kext/Contents/PlugIns/AppleEthernetAquantiaAqtion.kext
/System/Library/Extensions/IONetworkingFamily.kext/Contents/PlugIns/AppleEthernetAquantiaAqtionFirmware.kext
/System/Library/Extensions/IONetworkingFamily.kext/Contents/PlugIns/AppleVmxnet3Ethernet.kext
/System/Library/Extensions/IONetworkingFamily.kext/Contents/PlugIns/IOEthernetAVBController.kext
/System/Library/Extensions/IONetworkingFamily.kext/Contents/PlugIns/mDNSOffloadUserClient.kext
/System/Library/Extensions/IOUserEthernet.kext
```

That is the complete list. Broadcom 5701, Aquantia Aqtion, **VMware vmxnet3**,
USB ethernet, AVB — and **no Intel ethernet driver of any kind.** Cross-checking
every prelinked path containing "Intel" returns only graphics framebuffers, LPSS
I2C/SPI/UART, MCE reporter, PCH PMC and adaptive clocking — zero networking.

So: the VM hands the guest an Intel NIC, and the recovery environment has no
driver that can claim it. `en0` never comes up, no DHCPDISCOVER is ever sent, and
Recovery reports "Internet connection is required". That single fact explains
**all four** host-side observations at once — no lease this boot, guest MAC absent
from host ARP, VMnet8 counters flat, and Apple endpoints reachable from the host.

### The controls, because a grep over a 3 GB image is easy to fool

| Control | Expected | Result |
|---|---|---|
| `AppleUSBEHCI`, `IOPCIFamily`, `AppleAHCIPort` | present if image is readable | all **PRESENT** |
| `Sequoia`, `24E263` | present, confirms which image | both **PRESENT** |
| `ZzQqXxNotARealKextName123`, `AppleFakeEthernetDriver` | absent, proves no false positives | both **absent** |

Two string hits looked like counter-evidence and are not:

- **`Intel8254` PRESENT** — the matches are
  `com.SmallTree.iokit.Intel8254x 3.4.32/3.4.22/3.4.24`, third-party SmallTree
  bundle IDs sitting inside the **`OSKextExcludeList`** blob (surrounding keys are
  SHA-1 hashes mapping to `com.Greatdy.*`, `com.zg.driver.*`,
  `com.digidesign.*`, `com.microsoft.Virtual*`). That is a *blocklist of drivers
  macOS refuses to load*, not a driver that ships.
- **`e1000` / `E1000` PRESENT** — the matches are `_fiveToThe1000`,
  `_powersOf10_Float`, `GCC_except_table1000`, i.e. symbol and exception-table
  names. Coincidental substrings, not a driver.

`AppleVmxnet3Ethernet` by contrast resolves to a real prelinked bundle:
`_PrelinkBundlePath = /System/Library/Extensions/IONetworkingFamily.kext/Contents/PlugIns/AppleVmxnet3Ethernet.kext`,
with an `_InfoPlistDigest` and an `_PrelinkExecutableLoadAddr` — a loaded kext,
not a string.

### The fix

Switch the virtual NIC to the device the recovery environment *does* have a
driver for:

```
ethernet0.virtualDev = "vmxnet3"     # was "e1000e"
```

This also matches OSX-KVM's own choice of vmxnet3 for macOS guests, and it is why
that choice exists.

**The driver will actually bind — verified, not assumed.** Existing as a bundle is
not the same as matching the device. `AppleVmxnet3Ethernet.kext` declares:

```
<key>IOPCIPrimaryMatch</key>
<string>0x07b015ad&0xffffffff</string>
```

`15ad` is the VMware vendor ID and `07b0` is the vmxnet3 device ID, so the match
string covers exactly the device VMware will present. There is no equivalent to
check on the e1000e side, because no Intel ethernet kext exists to match against.

### Two hazards in applying the swap

**Delete `ethernet0.pciSlotNumber`, do not edit it.** The VMX currently pins
`ethernet0.pciSlotNumber = "160"`, a value chosen for an e1000e device. This file
already records **two** power-on crashes caused by that exact field being wrong
for the device — `[msg.pci.noslotavail]`, failed PCI registration, then a null
dereference to `0xc0000005` (see the PCI topology comment block in `Sonoma.vmx`
and *Failure 1* above). Changing `virtualDev` while keeping a slot pinned for the
old device invites a third. Remove the line; VMware assigns and writes back a
correct one at power-on. `ethernet0.generatedAddress` can stay — the MAC is not
device-dependent, only the slot is.

**The VMX's own Network comment is now false and must be corrected in the same
edit.** It reads:

> `e1000e is the only NIC macOS has a built-in driver for. vmxnet3 needs VMware`
> `Tools, which is not installed until after the OS is up.`

That is backwards for macOS 15, and believing it is what produced this blocker.
The recovery kernelcache ships `AppleVmxnet3Ethernet.kext` prelinked and ships no
Intel ethernet kext at all. The "vmxnet3 needs Tools" rule is true for Windows
and Linux guests; Apple bundles the driver. Left in place, the comment will talk
the next reader into swapping back.

Three constraints on applying it, all of them already-recorded traps:

1. **Full power cycle required.** VMware reads the VMX once at power-on. A
   running-VM edit does nothing — see the config-drift section above, where the
   `darwin24-64` correction never reached the running process.
2. **Fix `guestOS` in the same cycle.** The on-disk VMX says `darwin24-64` but the
   running instance is `darwin23-64`. One power cycle can land both.
3. **The 100 GB target is already erased and APFS-formatted** (per
   `quiet-yak-f0`'s host-side proof), so a power cycle costs the Recovery session,
   not the disk work.

### Three readings that will look like success and are not

A pre-swap baseline was captured `2026-08-07T18:41:48Z` —
`BASELINE-preswap-20260807T1841Z.txt`, alongside this VM — carrying md5s to diff
against: VMX `1b823842…`, leases `7636c96e…`, `vmware.log` `db5f3376…`, nvram
`087f3c8d…`.

**A lease for this VM's MAC already exists.** `vmnetdhcp.leases` holds
`192.168.255.129` against `00:0c:29:e7:9f:2b` — this VM — with
`starts 16:20:12`, `ends 16:50:12`. Power-on was `16:36:53`, so that record is
from an *earlier* boot and expired hours ago. Finding the VM's MAC in the lease
file after the swap proves nothing, and the MAC cannot disambiguate either:
`ethernet0.generatedAddress` is pinned in the VMX, so a vmxnet3 vNIC comes up on
the *same* address. **Only a `starts` timestamp later than the new power-on
counts.**

**`VMXNET` already appears in the log on an e1000e boot.** Line 511 of the
pre-swap instance reads `VMXNET: send cluster threshold is 80, size = 2
recalcInterval is 20000 ticks`. That is backend chatter emitted regardless of
`virtualDev`. Grepping for `vmxnet` and getting a hit is not evidence — read
`ethernet0.virtualDev` in the VMX and look for a **new** power-on line.

**A soft stop exits 0 without stopping anything.** `powerType.powerOff` is
`soft`, and a soft stop is delivered *through VMware Tools*. Tools are not
installed — `sata0:2` is `startConnected = "FALSE"` by design, and the recovery
environment could not use them anyway. `vmrun stop` returns exit 0 while the
guest keeps running. Use `vmrun stop <vmx> hard` and confirm the VM has left
`vmrun list` before believing the cycle happened.

Cheapest check of the lot: the pre-swap instance booted as `darwin23-64`
(line 419, both halves). A post-swap log **must** read `darwin24-64` on both
halves, or no power cycle occurred and every other reading is from the old
process.

### Failure 5 — a running VM clobbers on-disk VMX edits at shutdown

**Hit 2026-08-07 ~18:41 UTC, while applying the fix above.** This is the exact
inverse of the config-drift trap and it cost the first attempt at the swap.

The rule already recorded is *VMX edits do not reach a running VM* — VMware reads
the file once at power-on. The unrecorded half is worse: **at power-off VMware
writes its in-memory config back over the file**, discarding whatever is on disk.

**CORRECTED after the operator reported the actual sequence.** This section first
read as "VMware overwrites edits made while it was running", which implies the
trap needs someone to be editing concurrently. It does not. The operator
(`quiet-ibis-f5`) **never edited the VMX at all** — the sequence was: back up at
14:35, soft-stop, hard-stop at 14:41, then read. The edits that were destroyed
had been written at **12:59**, hours earlier, by a previous session, and had sat
untouched on disk ever since. They were still discarded.

So the accurate statement is stronger and worse: **power-off rewrites the VMX
from the running instance's memory unconditionally.** Any on-disk edit made after
the last power-on is lost, no matter how long ago, and no matter that nobody
touched the file during the session.

**The hard stop is what flushed it.** The soft stop did nothing — Recovery has no
VMware Tools to receive the ACPI request, so `vmrun stop ... soft` returned exit
0 while PID 30264 stayed alive. Only the hard stop actually ended the process,
and the write-back followed it. This is the counterintuitive part worth
remembering: a hard stop *looks* like it should discard in-memory state, and
instead it is precisely what commits that state to disk.

Observed state after the VM was powered off:

| Line | Clobbered file | Should have been |
|---|---|---|
| `guestOS` | `"darwin23-64"` | `"darwin24-64"` |
| `ethernet0.virtualDev` | `"e1000e"` | `"vmxnet3"` |
| `ethernet0.pciSlotNumber` | `"160"` still pinned | removed |
| `sata0:2.fileName` | `darwinPre15.iso` | `darwin.iso` |

The file shrank from **8629 B / 209 lines / 95 comment lines** to **7844 B / 198
lines / 84 comment lines** — 785 bytes and 11 comment lines gone, including the
entire block explaining why `darwin24-64` is correct. The `darwinPre15.iso`
comment came back reading "correct for Sonoma" against a 15.4.1 Sequoia image.

Every restored value is exactly what the *running instance* held — `vmware.log:305`
`DICT guestOS = "darwin23-64"`, `vmware.log:419`, and the `DICT ethernet0.*`
block. That is the proof of mechanism, not an inference from timestamps alone.
The smoking gun is the set of keys VMware **added** on its way out, which no hand
edit would produce:

```
cleanShutdown = "TRUE"                        # live file had FALSE
softPowerOff  = "FALSE"
vmotion.checkpointFBSize = "134217728"
vmotion.checkpointSVGAPrimarySize = "268435456"
vmotion.svga.mobMaxSize = "268435456"
vmotion.svga.graphicsMemoryKB = "262144"
uuid.location = "56 4d 3f fb d1 1c ed d5-26 5a 22 58 98 e7 9f 2b"
```

`cleanShutdown = "TRUE"` is written by the shutdown path. Its presence dates the
write to power-off, and dates it *after* any edit made while the VM was up.

**Consequence — the operating rule, both directions:**

> Edit the VMX **only while the VM is powered off, and re-verify the file after
> every power-off.** A running VM ignores your edit, and its eventual power-off
> overwrites the file from memory — including edits you made before it ever
> booted. Never assume an edit survived a VM session; `grep` or hash it again
> afterwards.

A second, smaller rule came out of the same incident: **when auditing a file
another agent is actively repairing, hash it — do not `diff` it.** During the
repair, `diff` reported the files identical (exit 0) while `md5sum` and `wc` in
the *same command batch* showed them plainly different. The repair landed between
the two reads. Two reads in one batch are not atomic, and `diff` is only a
point-in-time answer.

**Recovery, for the record.** Nothing was lost because a backup predated the
session:

```
Sonoma.vmx.bak-CLOBBERED-20260807T1841Z   7844 B   the regression, preserved
Sonoma.vmx.bak-pre-vmxnet3                8629 B   md5 8f92072d… restored from
Sonoma.vmx                               10047 B   md5 6f42d826… restored + fixed
```

The clobbered file was kept rather than deleted — a regression that leaves no
artifact is one the next person re-derives.

### Applied state — verified on disk 2026-08-07 ~18:47 UTC

The fix is **in the file**, with the VM powered off and no `vmware-vmx.exe`
running. Verified by `grep` after the edits, not assumed:

```
guestOS                    = "darwin24-64"
ethernet0.virtualDev       = "vmxnet3"
ethernet0.connectionType   = "nat"
ethernet0.generatedAddress = "00:0c:29:e7:9f:2b"     kept — MAC is not device-dependent
ethernet0.pciSlotNumber      ABSENT (grep -c = 0)   — VMware writes a correct slot at power-on
sata0:2.fileName           = "..\unlocker428\iso\darwin.iso"
```

The false Network comment is replaced with the measured finding, the
`IOPCIPrimaryMatch` proof, an explicit *do not swap this back*, and a note saying
why `pciSlotNumber` is absent — so neither can be helpfully re-added.

**The only remaining step was the power-on — it has since happened and passed.**
See *Acceptance test — PASSED* below.

### Acceptance test — PASSED 2026-08-07 18:52:35 UTC

The test was stated before the power-on and is falsifiable both ways: a lease
whose `starts` timestamp is **later** than the new power-on time. It passed.

```
power-on   2026-08-07T18:51:31.356Z   vmware.log line 1, pid=22944
lease      starts 5 2026/08/07 18:52:35   ends 19:22:35
           hardware ethernet 00:0c:29:e7:9f:2b
           client-hostname "Mac"
```

64 seconds after power-on — not the stale `16:20:12Z` leftover from a previous
boot. The leases file moved from mtime 12:20 to 14:52 and its md5 changed to
`75b9cc9d…`.

The swap reached the **running** instance, which the previous attempt never did:

```
305  DICT              guestOS = "darwin24-64"
340  DICT ethernet0.virtualDev = "vmxnet3"
418  Powering on guestOS 'darwin24-64' using the configuration for 'darwin24-64'
687  Ethernet0 MAC Address: 00:0c:29:e7:9f:2b
```

**All four original observations flipped.** Each was independent evidence for the
diagnosis; each now reads the opposite way:

| Observation under e1000e | Now under vmxnet3 |
|---|---|
| No lease at any point this boot | Lease `192.168.255.129`, `starts` 18:52:35Z |
| Guest MAC absent from host ARP | `192.168.255.129` `00-0c-29-e7-9f-2b` **dynamic**, present |
| VMnet8 counters flat across 90 s | `8562/63070` → `10102/67925` (**+1540** BytesIn) |
| Apple endpoints reachable from host only | Guest itself now on the wire |

The detail that makes it airtight is `client-hostname "Mac"`. The guest sent a
DHCP request **carrying a hostname option** — a macOS DHCP client genuinely ran.
A NIC with no driver bound cannot produce that; it is positive evidence of the
driver attaching, not merely of packets appearing.

**The falsifier did not trigger.** The stated kill condition was *“`en0` exists
but still no lease → this diagnosis is dead and Mullvad returns to the front”*. A
lease landed 64 s after power-on, so Mullvad stays **eliminated as the cause**
and is retained only as a second-order suspect (tunnel MTU 1380 vs VMnet8 1500)
should large transfers fail during the download.

**PCI slot, recorded because the value is misleading.** VMware wrote
`ethernet0.pciSlotNumber = "160"` back at line 228 — the *same number* as the pin
that was deleted. But VMware chose it *for a vmxnet3 device*, and the VM powered
on with no `[msg.pci.noslotavail]` and no `0xc0000005`. Deleting the pin was
still correct: an identical value does not mean the pin was harmless, only that
VMware happened to pick the same slot for a different device.

### Independent re-verification — 2026-08-07, same boot, ~1 h later

Re-run against the **still-running** instance (`vmrun list` → 1 VM,
`C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx`) to confirm the pass was not a
one-shot artifact of the first 64 seconds. All four checks are *positive*
evidence — each requires the guest to have transmitted.

| Check | Result | What it proves |
|---|---|---|
| `vmnetdhcp.leases` | `192.168.255.129`, `hardware ethernet 00:0c:29:e7:9f:2b`, `client-hostname "Mac"` | guest DHCP client ran and sent a hostname option |
| `arp -a` on `192.168.255.1` (VMnet8) | `192.168.255.129  00-0c-29-e7-9f-2b  dynamic` | host learned the MAC from a guest-originated frame |
| `ping -n 4 192.168.255.129` | 4/4 replies, 0 % loss, `<1 ms`, `TTL=64` | guest IP stack **generated** replies (TTL 64 = originated, not forwarded) |
| `Get-NetNeighbor 192.168.255.129` | `State : Reachable`, `InterfaceAlias : VMware Network Adapter VMnet8` | Windows confirmed bidirectional reachability inside the neighbor reachable timer |

The link came up cleanly in `vmware.log` and stayed up — no re-negotiation or
drop in the 8 `VNET`/`vmxnet` lines for this boot:

```
 340  DICT      ethernet0.virtualDev = "vmxnet3"
1102  VNET: 'ethernet0' enable link state propagation, lsp.state = 5
1103  VNET: MACVNetConnectToNetwork 'ethernet0' lsp.state = 4
1104  VNET: MACVNetConnectToNetwork 'Ethernet0' notify available.
1130  VNET: MACVNetLinkStateEventHandler: 'ethernet0' state from 4 to 6.
1156  VNET: MACVNetLinkStateTimerHandler: 'ethernet0' state from 6 to 1.
```

**Two measurement methods that do *not* work on this host — recorded so the next
person does not burn time on them:**

- `Get-NetAdapterStatistics` fails for both VMnet adapters:
  *"No MSFT_NetAdapterStatisticsSettingData objects found with property 'Name'
  equal to 'VMware Network Adapter VMnet8'"*. Piping `Get-NetAdapter` into it
  errors the same way; querying by wildcard returns empty properties and a
  before/after delta of `0` that is an **artifact of empty fields, not of zero
  traffic**. `Get-NetAdapter` itself does list both VMnet8 and VMnet1 as `Up` at
  100 Mbps, so the adapters exist — only the statistics provider is missing.
- `Win32_PerfRawData_Tcpip_NetworkInterface` enumerates only
  `Realtek PCIe GbE Family Controller`, `Realtek 8821CE Wireless LAN`, and
  `Teredo Tunneling Pseudo-Interface`. The VMware virtual adapters are not
  registered with that perf provider, and `Get-Counter -ListSet "Network
  Interface"` returns no `VMnet8` instance path.

So the `+1540 BytesIn` figure in the table above is **not reproducible through
those cmdlets** — treat host vNIC byte counters as unavailable on this machine
and use lease + ARP + neighbor-state instead.

#### Sustained-link evidence, and a trap in the lease file (19:56Z)

**`vmnetdhcp.leases` self-compacts. Reading it alone will mislead you.** At
`19:56:16Z` the VMware DHCP server rewrote
`C:\ProgramData\VMware\vmnetdhcp.leases` down to a **single** record for
`192.168.255.129` and moved the accumulated history to
`C:\ProgramData\VMware\vmnetdhcp.leases~`. Both files carry mtime
`15:56:16` local, within a millisecond of each other. A later reader who checks
only the live file sees one lease and can wrongly conclude the renewal history
below was never observed. It is an append-then-compact store, so **absence of an
old lease in the live file is not evidence the lease never existed** — always
read the `~` backup alongside it.

The backup preserves all six leases to `00:0c:29:e7:9f:2b`:

```
starts 2026/08/07 18:52:35   client-hostname "Mac"
starts 2026/08/07 19:07:34   client-hostname "Mac"
starts 2026/08/07 19:26:09   client-hostname "Mac"
starts 2026/08/07 19:40:13   client-hostname "Mac"
starts 2026/08/07 19:41:17   client-hostname "Mac"
starts 2026/08/07 19:56:16   client-hostname "kennys-Mac"   ends 21:56:16
```

**This is a renewal chain, not a boot-time fluke.** The `19:07` lease carried a
two-hour term and the guest re-DHCPed at `19:26` regardless, so `en0` stayed
bound and renewing across roughly 64 minutes. A single lease 64 seconds after
power-on is consistent with a one-shot DHCP artifact; five successive renewals
are not.

**The hostname changed from `Mac` to `kennys-Mac` at 19:56:16Z — the install
progressed past Recovery.** `Mac` is the generic hostname the macOS Recovery
ramdisk announces. A guest only advertises a personalized name once a real
system identity exists, i.e. Setup Assistant or the installed system named the
machine after the user. The lease term also lengthened to a full two hours.
Because the MAC is unchanged and `vmrun list` still reports the same
`Sonoma.vmx` (`pid=22944`) up, **the vmxnet3 link survived the
Recovery-to-installed-system transition with no intervention.**

Re-confirmed at the time of writing: `vmrun list` shows the VM running and
`arp -a` still shows `192.168.255.129  00-0c-29-e7-9f-2b  dynamic`.

- `arp -d 192.168.255.129` (the flush-and-force-re-resolve test) fails
  unelevated: *"The requested operation requires elevation"*, and `arp` still
  exits `0`, so the failure is silent unless stderr is read. `Get-NetNeighbor`
  reporting `Reachable` is the unelevated substitute — that state is only set by
  a confirmed round trip, so it carries the same positive signal.

### What this does *not* settle

MTU (tunnel 1380 vs VMnet8 1500) and the guest clock remain untested — they
cannot be tested until `en0` exists. They stay on the list as *second-order*
suspects for after the NIC swap, not as competing explanations for the current
failure.

Nothing here tests **routed** egress. Every check above is host↔guest on the
VMnet8 segment; the guest reaching the internet through NAT is a separate claim
and is still unproven.

---

## Failure 5 — `darwin.iso` will not mount as a CD (VMware Tools blocked)

**Symptom.** With the guest at a macOS desktop and no VMware Tools, the obvious
next step is to connect `sata0:2` (`darwin.iso`) and run the installer.
`vmrun connectNamedDevice ... sata0:2` returns **exit 0**. The guest attaches
the device — and then immediately ejects the tray. No Tools installer volume
ever appears.

**Cause.** `darwin.iso` is **not an ISO9660 image despite the extension.** Read
from the file, not assumed:

| Offset | Bytes | Meaning |
|---|---|---|
| `0x0000` | `45 52` (`'ER'`) | Apple Partition Map block0 |
| `0x0002` | `08 00` | block size 2048 |
| `0x8000` | `00` | **where a `CD001` primary volume descriptor would be** |

The partition map is self-consistent and describes an Apple disk:

```
part 0  'Apple'                  Apple_partition_map   start=1   blocks=15
part 1  'DiscRecording 9.0.3d5'  Apple_HFS             start=16  blocks=1637
```

and the volume at `16 * 2048 + 1024` carries the HFS Plus signature `H+`.
`(16 + 1637) * 2048 = 3385344` — exactly the file size, so the map ends at EOF.

A `cdrom-image` device tells the guest to look for an optical filesystem. There
isn't one, so the read fails at the SCSI layer. From `vmware.log`:

```
SCSI-CD sata0:2: READ CD failed. sense key 0x5 asc 0x24 ascq 0x0
```

Sense key `0x5` / asc `0x24` is ILLEGAL REQUEST — invalid field in CDB. **This
is Failure 3 again**, one device along: an APM/HFS+ image presented as a CD
gives the guest nothing it recognises.

The host reaches the same conclusion independently — `Mount-DiskImage` on
`darwin.iso` returns *"The file or directory is corrupted and unreadable."* Both
images in `unlocker428\iso\` are this format, not just `darwin.iso`:

| File | Bytes | block0 | Apple_HFS partition |
|---|---|---|---|
| `darwin.iso` | 3,385,344 | `'ER'` | `start=16 blocks=1637` |
| `darwinPre15.iso` | 3,006,464 | `'ER'` | present |

**Fix (written, not yet applied — see below).** Present it as a **disk**, via a
flat descriptor, exactly as `BaseSystem.vmdk` does for the installer.
`DarwinTools.vmdk`:

```
createType="monolithicFlat"
RW 6612 FLAT "..\unlocker428\iso\darwin.iso" 0
```

Extent is bytes ÷ 512: `3385344 / 512 = 6612`, and `3385344 mod 512 == 0`, so
the image is exactly sector-aligned. `vmware-vdiskmanager -R` validates the
descriptor (exit 0). The VMX then carries a fourth SATA device:

```
sata0:3.fileName   = "DarwinTools.vmdk"
sata0:3.deviceType = "disk"
```

`sata0:2` is deliberately left `present` with `startConnected = "FALSE"` so the
broken presentation stays visible in the file rather than being silently
deleted and re-attempted later.

> **`block0`'s own `blockcount` field reads `1694892032` — 3.4 TB — which is
> garbage for a 3.4 MB file.** The partition map entries are the authoritative
> ones and they end exactly at EOF. Do not "fix" the block count; it is not
> what the guest reads.

### The fix is corroborated by the filesystem, not just the partition map

"The partition map parses" would only prove the *container* is Apple-shaped. It
says nothing about whether the guest gets a **mountable volume**, and nothing
about whether the Tools installer is actually inside. Both are positive claims,
so both got their own measurement — `tools/_hfs-catalog-walk.py` reads the HFS+
volume and walks its catalog B-tree out of the raw bytes:

```
partition map (2 entries)
  1. Apple                    Apple_partition_map   start=1   sectors=63
  2. DiscRecording 9.0.3d5    Apple_HFS             start=64  sectors=6548

signature   : b'H+' version 4      allocation block : 2048 bytes
totalBlocks : 1637 -> 3352576      freeBlocks       : 0
partition   : 3352576 bytes        volume fits      : True

catalog B-tree: nodeSize 4096, totalNodes 12, treeDepth 2, leafRecords 218
nodes scanned : 12 of 12           catalog records  : 109 file/folder entries
```

Those sector figures are the same partition map as the 2048-byte-block listing
above, in 512-byte units: `16 * 2048 == 64 * 512` and
`1637 * 2048 == 6548 * 512 == 3352576`. Two independent parses of the same map
agreeing in different units is the point.

`freeBlocks = 0` and a volume that exactly fills its partition are what a
shipped read-only installer image should look like. And the volume root
(`parentID 2`) names the payload:

```
Install VMware Tools.app
Uninstall VMware Tools.app
VMware Tools.pkg
.VolumeIcon.icns   .DS_Store   .hiddendir
```

So the `sata0:3` disk presentation will hand the guest a real HFS+ volume with
the installer on it. That is now measured, not assumed — the only thing still
missing is the power cycle that makes the device exist.

> **A parser bug nearly turned this into a false negative — recorded because the
> wrong answer looked right.** The first walk printed `catalog totalNodes : 0`,
> which would read as "the image has no catalog." It was the reader, not the
> image: `BTHeaderRec` starts at **+14** inside a B-tree node, after the 14-byte
> `BTNodeDescriptor`, so `totalNodes` is at node base **+36** and node base +40
> is `freeNodes`. On a full read-only image `freeNodes` is legitimately `0`, so
> the wrong offset returned a plausible number instead of obvious garbage. Same
> failure class as the retracted TEST E/F elsewhere in this project: a reading
> that cannot distinguish the two outcomes is not evidence. The arithmetic check
> that caught it — `49152 / 4096 == 12` — is now printed next to the field on
> every run.

### Why this is not closed yet

**The running VM was powered on before the VMX edit, so the new device does not
exist in its live configuration.** `vmrun connectNamedDevice ... sata0:3`
returns exit 0 and `vmware.log` says what actually happened:

```
Attempting to connect nonexistent device "sata0:3".
```

Only a power cycle applies the edit — and that discards the live macOS session
the crew is using. Left for whoever owns that call.

**The diagnostic worth keeping: `vmrun connectNamedDevice` returns exit 0 for a
device that does not exist.** It reported success twice here while doing
nothing at all — once for the CD that ejected itself, once for a device absent
from the running config. Its exit code is not evidence. Read `vmware.log`.
