# handoff1.md — session snapshot, 2026-08-07

Written at the end of the VM-bringup session. This is a **point-in-time record**,
not a replacement for `HANDOFF.md` — read that one first on any resume. This file
exists to capture what changed today, what was gotten wrong today, and the one
question still open when the session ended.

---

## 1. What the project is, restated because it got questioned

| Machine | GPU | Role |
|---|---|---|
| **Laptop** (`kenny-linus`) | **GA107M, RTX 3050 Mobile, `10de:25a2`** | **The only driver target.** Trace source, firmware source, eventual load target. |
| **Desktop** (`KENNYSPC`) | RTX 4060 | Build/analysis box only. **Never a target.** |

Enforced in code, not just prose:

```
phase0-kext/Info.plist    IOPCIMatch = 0x25a210de   exact match, 10de:25a2 only
phase0-kext/NVDecode.hpp  NV_CHIP_ID_GA107 = 0x177  Ampere arch 0x17, impl 0x7
reference/ga107-firmware/                           pulled from the laptop over SSH
```

The macOS VM uses VMware's emulated SVGA adapter and cannot see either physical
GPU. `make` inside that VM cross-builds a kext that only ever loads on the
laptop. Every Ada / AD106 / AD107 mention in the docs is a **warning** (the name
"RTX 3050 Laptop GPU" spans four dies across two architectures), never a target.

`README.md` now carries a "Which machine is which" table so this cannot be
misread again.

---

## 2. Verification state — all green, all re-run this session

```
register constants   102/102   python tools/check-registers.py
test_decode           39/39   tests\build-msvc.bat
test_falcon          165/165   tests\build-msvc.bat
test_riscv_ucode      85/85    tests\build-msvc.bat
kext syntax gate      5/5      tests\check-kext-syntax.bat  (clang 19.1.5)
structors pairing     1/1      python tools/check-structors.py
negative control      4/4      planted defects all diagnosed
python tool tests     97/97    tests\run-python-tests.bat  (was 67 — see note)
```

Nothing here is quoted from memory — every number was produced by a run in this
session.

**Two rows were added on re-audit and are worth flagging, because their absence
read as a smaller test surface than actually exists:**

- `test_riscv_ucode` (85 checks) landed with the GA107 bootloader descriptor
  work. `build-msvc.bat` is therefore **3/3 suites, 289 checks** (39 + 165 + 85),
  not 2/2 and 204. A checklist still expecting 2/2 makes growth look like a
  regression, and the reader's instinct is to distrust the tree rather than the
  doc.
- `tests\run-python-tests.bat` was missing entirely. It is the only gate that
  runs on a bare checkout — CPython, no MSVC or CMake — and covers the
  `tools/dmg2raw.py` mish offsets, the `vm-screenshot.py` logic, and the
  `check-registers.py` / `collapse-sequence.py` parsers. It uses
  `unittest discover`, so **the count is expected to climb**; treat any figure
  here as a floor with a date on it, not a fixed expectation.

  **It already climbed.** The row above read `67/67` when this file was written
  and reads `97/97` on a re-run at 2026-08-07 ~19:05 UTC — +30 checks, with the
  suite reporting `Ran 97 tests ... OK`. The growth is real work landing, not a
  regression or a miscount: the run now also builds an `InstallAssistant` ISO
  and reads it back, so `tools/build-installassistant-iso.py` is inside this
  gate. If a future run reports a number lower than 97, that *is* worth
  investigating; a higher one is not.

---

## 3. New this session

### GA107 firmware, extracted from the laptop

Over SSH to `kenny@192.168.0.180` (key auth, no password needed). The laptop runs
Ubuntu, kernel 7.0.0-29-generic, with `nvidia-595-open` 595.84 installed — so the
exact blobs for this die were already on disk.

| File | Size | Role |
|---|---|---|
| `bootloader-570.144.bin` | 24,684 B | **The image the mmiotrace DMA'd into IMEM** |
| `booter_load-570.144.bin` | 61,304 B | SEC2 Booter Load — raises WPR2, authorises GSP |
| `booter_unload-570.144.bin` | 41,080 B | SEC2 Booter Unload |
| `vbios-ga107.rom` | 65,024 B | VBIOS, `55 aa` valid, `Version 94.07.4E.00.4B` |

All in `reference/ga107-firmware/`. `PHASE4-GSP-BRINGUP.md` §3.3 *inferred* a
~24 KB IMEM payload from 96 × 256 B DMA transfers. The bootloader is 24,684
bytes. The inference was right and the real image is now local.

**Two findings that would each have cost weeks:**

1. **These are `nvfw_bin_hdr` containers, not VBIOS-embedded `FALCON_UCODE_DESC`
   images.** `header_offset` is 24 in all three files, so one parser *looks* like
   it should work — but the bootloader holds `RM_RISCV_UCODE_DESC` v5 there while
   both booters hold `nvfw_hs_header_v2`. Parsing all three alike gave the
   bootloader clean fields and the booters nonsense (`bootloaderOffset =
   0x0000f000`, past EOF). A scan for a V2/V3 descriptor header found **none** in
   any blob; that null result *was* the answer. The `0x3c` = 60 word that looked
   like `FALCON_UCODE_DESC_V2_SIZE_60` is a coincidence — it is `sig_prod_offset`
   in the HS header.
2. **The booters are heavy-secure signed with a signature patch point**
   (`sig_prod_size = 768`, `patch_loc = 828`, `num_sig = 848`). Phase 4 cannot
   load them without performing that patch. No shortcut exists.

**Version gap, unresolved:** blobs are **570.144**; the kext's constants were
transcribed from **610.57.04**. Not blocking yet — the first Phase 4 milestone
(reset, scrubbing poll, `DMATRFBASE`, the 256 B IMEM loop) uses only the
bootloader descriptor and zero RPC structures. It becomes blocking at the Booter
Load / init-RPC stage. See `HANDOFF.md` §8 blocker 3 for the three ways out.

### The VBIOS dump trap

First two attempts returned `dd: IO error` and a 0-byte file. **The cause was not
power management** — `power/control` read `on`, device was in `D0`. It was that
**no driver was bound**: with `driver` empty, the kernel refuses to service reads
of the `rom` attribute even with `rom_enable` set. `sudo modprobe nvidia` first,
then the dump works.

Interacts with `prep-blacklist.sh`: it blacklists `nvidia` so the module does not
autoload during a capture, but an explicit `modprobe nvidia` still works — which
is exactly why the `install ... /bin/true` lines were removed from it.

**Laptop state left behind:** `nvidia` module now loaded (was not before).
`/home/kenny/ga107-fw/` and `/home/kenny/vbios-ga107.rom` exist. A reboot clears
the module; the files are harmless.

### Third verification gate — the kext now parses

clang **19.1.5** ships inside Visual Studio 2022 at
`VC\Tools\Llvm\bin\clang++.exe`. Paired with a hand-written IOKit/libkern stub
tree at `tests/kextshim/`, it parses the kext front-to-back: **clean, zero
diagnostics, under `-Wall -Wextra -Werror`.** First time this code has ever been
through a C++ front end.

Run it: `tests\check-kext-syntax.bat`

Two shim bugs surfaced and were fixed: `dynamic_cast` in `OSDynamicCast` broke
under `-fno-rtti` (real kexts build that way; Apple's version is a metaclass
check, not RTTI), and `IOService::init` needed `using OSObject::init;` to match
Apple's actual overload hiding.

**One planted defect did not fail and was removed rather than left as
decoration.** A missing `OSDefineMetaClassAndStructors` is invisible to
`-fsyntax-only` — the declared ctor/dtor only go unresolved at link time, and
there is nothing to link against here. That defect class moved to
`tools/check-structors.py`. Recorded because a syntax gate that passes everything
is *worse* than no gate: it converts "never compiled" into "believed compiled".

### VM built and booting

`C:\Users\Kentl\macos-vm\` — VMware Workstation **17.6.4.24832109**, patched by
DrDonk unlocker 4.2.8 (`check.exe`: all four binaries `Patched (1)`, cross-checked
against SHA-256 read directly off disk).

Recovery image fetched via `OSX-KVM`'s `fetch-macOS-v2.py` and verified
byte-exact — all 85 chunklist SHA-256 chunks match. `tools/dmg2raw.py` was
written to convert the compressed UDIF into a raw disk.

**Five power-on failures, five distinct root causes, none of which produced an
error naming itself.** Full writeup with evidence in `VM-BRINGUP.md`; do not
re-derive them.

**The guest is macOS 15.4.1 Sequoia, build 24E263 — not Sonoma**, despite every
filename and the VM's `displayName` saying otherwise. Read from the embedded
`SystemVersion.plist`. Renaming would break the VMX's relative paths for no
benefit. What matters: `guestOS = "darwin24-64"`, and guest tools need
`darwin.iso`, **not** `darwinPre15.iso`.

---

## 4. What I got wrong today, and why it is written down

### The false-alarm freeze — the expensive one

After the clean EFI handoff I took two headless measurements and called the guest
hung:

| Measurement | Result | What I claimed it meant |
|---|---|---|
| Guest-rect framebuffer diff | **0** changed pixels | display frozen |
| `vmware-vmx` CPU over 20–25 s | **+0.12–0.28 s** | vCPUs halted |
| Target vmdk growth | 13.2 MB, unchanged | nothing installing |

Every number is accurate. **None supports the conclusion.** macOS Recovery is a
completely static screen — no clock, no spinner, no caret — so a live idle
Recovery UI is byte-identical between captures. An idle guest waiting on input
burns ~1% of one core. The target disk has no reason to grow before the user
starts the install.

I then assembled `Monitor Mode: ULM` + `Hyper-V detected by CPUID` +
`hypervisorlaunchtype Auto` into a root cause (VMware under WHP bypasses its
native monitor, so the unlocker's SMC patch never executes) and proposed
`bcdedit /set hypervisorlaunchtype off` + reboot. Every *fact* in that chain is
true. **It was not what was happening** — the user clicked around in Recovery and
it responded normally.

That "fix" would have disabled WSL2, Docker Desktop, Windows Sandbox, Hyper-V and
VBS host-wide to solve a problem that did not exist. **Do not do it.**
`Monitor Mode: ULM` on this host is **benign**, same category as the Hyper-V
CPUID line and `Can't set WSS, error 1314` — all three appear in every log from
this machine, including four runs that failed for four unrelated reasons.

**The rule:** screenshot diffing can prove a guest is *alive* (pixels changed);
it can never prove one is *dead*. Before macOS is installed there is **no**
reliable headless liveness test — `vmrun captureScreen` and every guest RPC path
need VMware Tools, which does not exist yet (`VIX_E_TOOLS_NOT_RUNNING`,
`GuestRpcSendTimedOut`). A human at the console is the only ground truth.

### Secondary trap: diffing the whole window

An early comparison reported 642 changed pixels and was read as proof of life.
The diff bbox was `(37,7)–(202,16)` — a 165×9 strip in the VMware **toolbar**.
Guest-area delta was 0. Always crop to the guest rect before comparing; in a
1228×933 window capture that is `x ≥ 204`, `y` 77–816. The chrome redraws on its
own.

### `dmg2raw.py` — a silent 3 GiB of zeroes

The first conversion reported success and wrote a 3 GiB file of pure zeroes.
`bytes decoded: 0` was the only clue, and the per-partition sector ranges printed
*correctly* because those live at mish offsets 8 and 16.

The UDIF checksum field is **136 bytes, not 40**, so the chunk count is at offset
**200** and the chunk array at **204**. Reading them at 104/108 yields a garbage
count of zero-type chunks. Pinned by `tests/test_dmg2raw.py` so it cannot recur.

### Wrong VMware version in the docs

I reported 17.0.0, inferred from the `Workstation-17.0.0` VIX subdirectory name.
The actual binary version is **17.6.4.24832109**, read from `check.exe`. Corrected
in both docs. Small, but it is the same failure mode as everything else here:
inferring instead of measuring.

---

## 5. Open — the one live question

**Recovery reports "An Internet connection is required to install macOS."** at
Step 2 (Reinstall macOS). Diagnosis was in progress when the session ended.

What is **confirmed working**:

```
ethernet0        e1000e, connectionType nat, link state 5, no registration errors
MAC              00:0c:29:e7:9f:2b   (matches ethernet0.generatedAddress)
DHCP lease       192.168.255.129     granted from VMnet8 pool
host services    VMware NAT Service + VMnetDHCP both Running
vmnet8 adapter   Up, host addr 192.168.255.1, NAT gateway 192.168.255.2
vmnat process    running, pid 5740
```

So the guest has a link, an address, and a gateway. Host-side reachability to
every endpoint a Recovery reinstall needs is also fine:

```
osrecovery.apple.com   200
swscan.apple.com       404 (root path; the real sucatalog URL returns 200)
swcdn.apple.com        404 (root path — expected)
swdist.apple.com       404 (root path — expected)
mesu.apple.com/assets/ 403 (expected)
gdmf.apple.com         200 with -k
```

**Prime suspect: Mullvad VPN.** Status `Connected`, relay `us-qas-wg-101`, and it
holds the **default route at interface metric 1**. LAN sharing is `allow` and
lockdown mode is `off`, but *NAT-forwarded guest traffic is not LAN traffic* — it
is forwarded traffic with a foreign source, which is exactly the shape a VPN
client's firewall drops. Split tunnelling is on but excludes only two Google
NearbyShare binaries.

**`gdmf.apple.com` fails TLS from the host** with
`schannel: SEC_E_UNTRUSTED_ROOT (0x80090325)` and succeeds with `-k`. That is
worth understanding rather than waving away — it is consistent with a curl /
schannel cert-store quirk, but also with interception. It was **not** resolved.

### Next steps, in order

1. **Test guest→internet directly.** Everything so far tests the *host*. In the
   guest's Recovery Terminal (Utilities → Terminal):
   ```sh
   ifconfig en0 | head -5          # confirm 192.168.255.129
   route -n get default            # confirm gateway 192.168.255.2
   ping -c 3 192.168.255.2         # NAT gateway reachable?
   ping -c 3 1.1.1.1               # forwarding works?
   nslookup swscan.apple.com       # DNS proxy works?
   curl -sI https://osrecovery.apple.com/ | head -3
   ```
   Whichever of those first fails localises it precisely. Do not guess before
   running them — that is the mistake this session already made once.
2. **If ping to `1.1.1.1` fails but the gateway answers:** Mullvad is dropping
   forwarded traffic. Disconnect Mullvad temporarily and retry, or add
   `vmnat.exe` to its split-tunnel exclusions.
3. **If DNS fails but IP ping works:** the NAT DNS proxy is inheriting Mullvad's
   `10.64.0.1` resolver. Set explicit DNS in the guest, or pin
   `[dns] policy`/nameservers in `C:\ProgramData\VMware\vmnetnat.conf`.
4. **Check the guest clock.** `tools.syncTime = "FALSE"` and no persistent
   `Sonoma.nvram`. A wrong RTC breaks TLS and macOS reports it as "no internet".
   `date` in the Recovery Terminal settles it.

### Worth knowing

The guest is a **recovery** image — "Reinstall macOS" downloads the full OS from
Apple, so a working connection is genuinely required. This is not a spurious
error.

---

## 6. Traps that will each cost an hour

- **`Sonoma\` and `sonoma\` are the same directory.** Windows is
  case-insensitive; the fetch script created `sonoma`, the VM dir was created as
  `Sonoma`. `vmrun` reports the vmx under `sonoma\`. The VMX's relative
  `..\sonoma\BaseSystem.vmdk` resolves *out and back into the same folder* — it
  works **by accident**. Test before simplifying it.
- **`du` on the VM directory is not a progress signal.** It counts the 8 GB
  `.vmem`, the 3 GB `.img`, and the 0.9 GB `.dmg`. Watch `Sonoma-s0*.vmdk` extent
  sizes instead.
- **`fetch-macOS-v2.py --action verify` crashes on Windows** with
  `[WinError 6] The handle is invalid`. Bug in the script's verify path, not a bad
  download. Verify the chunklist directly.
- **Never hand-pin `pciSlotNumber`.** Declare the bridges and let VMware compute
  the map. It chose `ethernet0 = 160`, `usb = 32`, `usb_xhci = 192` — values no
  human would guess. Hand-pinning `21` crashed the VM twice.
- **The unlocker is reverted by any VMware update, repair, or auto-update.** All
  three binaries silently revert and Failure 1 returns looking brand new. Re-run
  `check.exe` first whenever a previously-working VM refuses to boot.
- **Do not touch `C:\Program Files (x86)\VMware\`** except via `relock.exe`.

---

## 7. Where each thing lives

```
D:\nerd shit\other projects\mac os\        project root
  HANDOFF.md                               read first on resume
  VM-BRINGUP.md                            5 power-on failures + the false alarm
  PHASE4-GSP-BRINGUP.md                    trace-derived GSP bringup spec
  reference\ga107-firmware\                laptop blobs + README
  tests\kextshim\                          IOKit stubs for the clang gate
  tools\dmg2raw.py                         UDIF -> raw converter
  tools\check-structors.py                 metaclass pairing checker
  .claude-crew\plan.md                     crew plan, teams, rules

C:\Users\Kentl\macos-vm\                   VM lives here (C: has ~192 GB free)
  sonoma\  (== Sonoma\)                    vmx, vmdk, BaseSystem.{dmg,img,vmdk}
  unlocker428\windows\                     unlock.exe, relock.exe, check.exe
  unlocker428\iso\darwin.iso               guest tools for macOS 15+
  grab-vm-screen.ps1                       host-side console capture
  analyze-screen.py                        numeric screenshot description
```

Laptop: `ssh kenny@192.168.0.180`, key auth, no password required.

---

## 8. Immediate next action

> **SUPERSEDED 2026-08-07 ~18:40 UTC — see §9 below.** The hop walk described
> here is no longer the next action. The failure was localised host-side without
> the guest console, and the answer is that no hop after the first one is
> reachable. Read §9 instead of acting on this section.

Run the six commands in §5 step 1 from the guest's Recovery Terminal. That
localises the network failure to one hop instead of a theory. Everything else in
the project is green and waiting on the install.

---

## 9. The open question from §5 is CLOSED — root cause found

**The Recovery "Internet connection is required" wall was not a network problem
in any of the senses §5 proposed.** It was a missing driver. `en0` never existed.

### The finding

`ethernet0.virtualDev = "e1000e"` presents the guest an Intel 82574L-class NIC.
The macOS 15.4.1 (24E263) recovery kernelcache inside `BaseSystem.img` contains
**no Intel ethernet kext of any kind.** The complete prelinked ethernet/network
bundle list:

```
AppleUSBEthernet.kext          AppleUSBEthernetHost.kext
AppleUSBNetworking.kext        IONetworkingFamily.kext
IOUserEthernet.kext
IONetworkingFamily.kext/Contents/PlugIns/
    AppleBCM5701Ethernet.kext
    AppleEthernetAquantiaAqtion.kext
    AppleEthernetAquantiaAqtionFirmware.kext
    AppleVmxnet3Ethernet.kext
    IOEthernetAVBController.kext
    mDNSOffloadUserClient.kext
```

Broadcom, Aquantia, **VMware vmxnet3**, USB, AVB — no Intel. Every prelinked
path containing "Intel" is graphics framebuffer, LPSS I2C/SPI/UART, MCE reporter
or PCH PMC. Zero networking.

One fact explains **all four** independent host-side observations at once:

| Observation | Why the missing driver explains it |
|---|---|
| No DHCP lease at any point this boot | No `en0`, so no DHCPDISCOVER is ever sent |
| Guest MAC absent from host ARP table | No interface to answer ARP with |
| VMnet8 byte counters flat across 90 s | Guest emits nothing onto the wire |
| Apple endpoints reachable from host | Nothing upstream was ever broken |

This also **retires §5's "confirmed working" block**, which is the part of this
file most likely to mislead: `DHCP lease 192.168.255.129` was real but belonged
to an *earlier boot* (see the retraction table below), and `link state 5, no
registration errors` describes the *emulated device* registering on the virtual
PCI bus — not a guest driver attaching to it. A device can register perfectly and
still have nothing claim it.

### Why this is positive evidence, not an absence argument

The kernelcache's prelinked bundle list was **enumerated**, not searched for a
missing name. Controls were run both directions: `AppleUSBEHCI`, `IOPCIFamily`,
`AppleAHCIPort`, `Sequoia`, `24E263` all present (image reads correctly); two
invented kext names both absent (no false positives).

Two string hits looked like counter-evidence and were disproved:

- `Intel8254` → `com.SmallTree.iokit.Intel8254x` living **inside the
  `OSKextExcludeList`** blob — a blocklist of drivers macOS refuses to load.
  Neighbouring keys are SHA-1 hashes for `com.Greatdy.*`, `com.zg.driver.*`,
  `com.digidesign.*`.
- `e1000` / `E1000` → `_fiveToThe1000`, `_powersOf10_Float`,
  `GCC_except_table1000`. Symbol names.

`AppleVmxnet3Ethernet` by contrast resolves to a real bundle with a
`_PrelinkBundlePath`, `_InfoPlistDigest` and `_PrelinkExecutableLoadAddr` — and,
decisively, `IOPCIPrimaryMatch = 0x07b015ad&0xffffffff`. `15ad` is the VMware
vendor ID, `07b0` the vmxnet3 device ID. The driver in the recovery image matches
the exact device VMware will present. Existence alone would not have been enough.

### The fix and its hazards

```
ethernet0.virtualDev = "vmxnet3"     # was "e1000e"
```

1. **Delete `ethernet0.pciSlotNumber` entirely.** It is pinned to `"160"`, chosen
   for an e1000e device. §6 already carries the rule — *never hand-pin
   `pciSlotNumber`, hand-pinning `21` crashed the VM twice* — and it applies here:
   a slot chosen for the old device is a hand-pin for the new one. Remove the
   line; VMware assigns and writes back a correct value at power-on.
   `ethernet0.generatedAddress` can stay — the MAC is not device-dependent.
2. **Full power cycle, not a guest reboot.** VMware reads the VMX once at
   power-on.
3. **`Sonoma.vmx:140-141` is false and must be corrected in the same edit.** It
   reads verbatim: `e1000e is the only NIC macOS has a built-in driver for.`
   `vmxnet3 needs VMware Tools, which is not installed until after the OS is up.`
   That belief is what produced this blocker. The Tools rule is true for Windows
   and Linux guests; Apple bundles the vmxnet3 driver. Left in place it will talk
   the next reader into swapping back.

### Acceptance test — falsifiable both ways

After power-on, take the timestamp from `vmware.log` line 1, then check
`C:\ProgramData\VMware\vmnetdhcp.leases` for a lease whose `starts` timestamp is
**later** than that power-on time.

- New lease → confirmed, blocker closed.
- `en0` exists but still no lease → **this hypothesis is dead.** Mullvad returns
  to the front, because the tunnel genuinely does own the default route at metric
  1 with MTU 1380 against VMnet8's 1500.

A one-command guest-side confirmation is also available and worth taking if the
console is already open: `ifconfig -a`. No `en0` at all confirms this from inside.
Steps 2–6 of §5 are unreachable until `en0` exists, so do not spend console time
on them.

#### Three traps that will read as success and are not

Each of these produces a signal that looks like the fix landed. A pre-swap
baseline was captured at `2026-08-07T18:41:48Z` —
`C:\Users\Kentl\macos-vm\sonoma\BASELINE-preswap-20260807T1841Z.txt` — with
md5s to diff against: VMX `1b823842…`, leases `7636c96e…`, vmware.log
`db5f3376…`, nvram `087f3c8d…`.

**1. A lease for this VM's MAC already exists.** `vmnetdhcp.leases` holds
`192.168.255.129` against `00:0c:29:e7:9f:2b` — this VM — with
`starts 16:20:12`, `ends 16:50:12`. Power-on was `16:36:53`, so it belongs to an
*earlier* boot and expired hours ago. Grepping the file for the VM's MAC after
the swap therefore proves nothing. Worse, the MAC cannot disambiguate:
`ethernet0.generatedAddress` is pinned in the VMX, so a vmxnet3 vNIC comes up on
the *same* `00:0c:29:e7:9f:2b`. **Only a `starts` timestamp later than the new
power-on counts.**

**2. `VMXNET` already appears in the log on an e1000e boot.** `vmware.log:511`
of the current, pre-swap, e1000e instance reads
`VMXNET: send cluster threshold is 80, size = 2 recalcInterval is 20000 ticks`.
That is generic backend chatter emitted regardless of `virtualDev`. Grepping for
`vmxnet` and finding a hit is not evidence. Check
`ethernet0.virtualDev = "vmxnet3"` in the VMX and a **new** power-on line.

**3. A soft stop exits 0 without stopping anything.** `powerType.powerOff` is
`soft`, and a soft stop is delivered *through VMware Tools*. Tools are not
installed — `sata0:2` is `startConnected = "FALSE"` by design, and the recovery
environment could not use them anyway. `vmrun stop` will return exit 0 while the
guest keeps running. Use `vmrun stop <vmx> hard`, then confirm the VM is gone
from `vmrun list` before assuming the cycle happened.

One more, cheap to check: the running instance booted as `darwin23-64`
(`vmware.log:419`, both halves of the line) because it predates the VMX
correction. A post-swap log **must** say `darwin24-64` on both halves, or no
power cycle occurred and every other reading is from the old instance.

### Config drift found along the way

The on-disk VMX says `guestOS = "darwin24-64"` but the running instance was
powered on with `darwin23-64` (`vmware.log:419`). The VMX was edited at 16:59 UTC,
**22 minutes after** the 16:36:53 UTC power-on. **Never quote the on-disk VMX as a
description of the running VM — quote `vmware.log`'s `DICT` block.** The same
power cycle lands both corrections. Note this makes §3's `guestOS = "darwin24-64"`
true of the *file* but not yet of any VM that has actually run.

### Retracted this session, recorded so it is not re-derived

| Claim | Status |
|---|---|
| "Guest already has a lease, so no-NIC and no-DHCP are eliminated" (and §5's `DHCP lease 192.168.255.129 granted`) | **Wrong.** Lease starts 16:20:12Z, VM powered on 16:36:53Z — it predates this boot by 16 min and belongs to an earlier one (MAC persists via `addressType = "generated"`). The leases file has not been written since 16:20Z. |
| `ping -S 192.168.255.1 1.1.1.1` fails → VMnet8 is blocked | **Inconclusive.** Controls: Wi-Fi and VirtualBox source addresses also fail; only the tunnel address succeeds. Artifact of the VPN owning the default route. |
| `ping 192.168.255.2` / `nslookup` via `.2` time out → NAT is broken | **Inconclusive.** `vmnat` does not reliably answer host-originated ICMP or DNS. |
| Mullvad egress block is the leading cause (§5 "prime suspect") | **Eliminated**, not merely demoted. An egress block requires frames to leave the guest. Flat `BytesIn` and a missing ARP entry are *pre-egress* — ARP and DHCPDISCOVER are L2 broadcasts on VMnet8 that never touch the tunnel or the default route. Still a live *second-order* suspect once `en0` exists. |

Still genuinely unresolved from §5: the `gdmf.apple.com`
`SEC_E_UNTRUSTED_ROOT` TLS failure from the host, and the guest clock
(`tools.syncTime = "FALSE"`, no persistent `Sonoma.nvram`). Neither can be
reached from inside the guest until `en0` exists; both stay open.

### Method lesson

The distinction that cracked this was **pre-egress vs post-egress evidence**.
Interface byte counters and the host ARP table discriminate between "guest is not
talking" and "guest is talking but blocked"; no host-side reachability test can
make that distinction. Reaching for them first would have skipped the entire
Mullvad detour — and §5's whole "confirmed working" block is host-side
reachability, which is why it read as reassuring while the guest was mute.

Corollary, consistent with the frozen-guest false alarm in §4: the flat counters
**corroborate** but never prove — absence of traffic is one-directional evidence.
The finding rests on the kext enumeration, which is positive.

### The clobber trap — hit while applying the fix

The first attempt at the swap was destroyed by a trap that is the **inverse** of
the config drift above, and it is worth more than the fix itself.

Known rule: *VMX edits do not reach a running VM.* Unrecorded half: **at
power-off VMware writes its in-memory config back over the file**, discarding
whatever is on disk.

**CORRECTED after the operator reported the actual sequence.** This first read as
"VMware overwrites edits made while it was running", which implies the trap needs
someone editing concurrently. It does not. `quiet-ibis-f5` **never edited the VMX
at all** — back up at 14:35, soft-stop, hard-stop at 14:41, then read. The edits
that were destroyed had been written at **12:59**, hours earlier by a previous
session, and had sat untouched on disk ever since. They were still discarded.

So the accurate statement is worse: **power-off rewrites the VMX from the running
instance's memory unconditionally.** Any on-disk edit made after the last
power-on is lost, however long ago, and regardless of whether anyone touched the
file during the session.

**The hard stop is what flushed it.** The soft stop did nothing — Recovery has no
VMware Tools to receive the ACPI request, so `vmrun stop ... soft` returned exit 0
while PID 30264 stayed alive. Only the hard stop ended the process, and the
write-back followed it. A hard stop *looks* like it should discard in-memory
state; instead it is precisely what commits that state to disk.

Observed 2026-08-07 ~18:41 UTC, after the VM was powered off:

| Line | Clobbered | Should have been |
|---|---|---|
| `guestOS` | `"darwin23-64"` | `"darwin24-64"` |
| `ethernet0.virtualDev` | `"e1000e"` | `"vmxnet3"` |
| `ethernet0.pciSlotNumber` | `"160"` still pinned | removed |
| `sata0:2.fileName` | `darwinPre15.iso` | `darwin.iso` |

The file shrank **8629 → 7844 B**, **209 → 198 lines**, **95 → 84 comment
lines** — including the entire block explaining why `darwin24-64` is correct.
Every restored value equals what the *running instance* held (`vmware.log:305`,
`:419`, and the `DICT ethernet0.*` block).

Proof of mechanism rather than inference from timestamps: VMware **added** keys
no hand edit produces — `cleanShutdown = "TRUE"` (the live file had `FALSE`),
`softPowerOff = "FALSE"`, four `vmotion.checkpoint*`/`svga` keys, and
`uuid.location`. `cleanShutdown = "TRUE"` is written by the shutdown path, which
dates the write to power-off.

> **Operating rule, both directions: edit the VMX only while the VM is powered
> off, and re-verify the file after every power-off.** A running VM ignores your
> edit, and its eventual power-off overwrites the file from memory — including
> edits you made before it ever booted. Never assume an edit survived a VM
> session; `grep` or hash it again afterwards.

A second, smaller rule from the same incident: **when auditing a file another
agent is actively repairing, hash it — do not `diff` it.** During the repair,
`diff` reported the files identical (exit 0) while `md5sum` and `wc` in the *same
command batch* showed them plainly different. The repair landed between the two
reads. Two reads in one batch are not atomic, and `diff` is only a point-in-time
answer.

Recovery, because a backup predated the session:

```
Sonoma.vmx.bak-CLOBBERED-20260807T1841Z    7844 B   the regression, preserved
Sonoma.vmx.bak-pre-vmxnet3                 8629 B   md5 8f92072d…  restored from
Sonoma.vmx                                10047 B   md5 6f42d826…  restored + fixed
```

The clobbered file was kept deliberately — a regression that leaves no artifact
is one the next person re-derives.

### Applied state — verified on disk 2026-08-07 ~18:47 UTC

The fix **is in the file**, applied with the VM powered off and no
`vmware-vmx.exe` running. Verified by `grep` after the edits, not assumed:

```
guestOS                    = "darwin24-64"
ethernet0.virtualDev       = "vmxnet3"
ethernet0.connectionType   = "nat"
ethernet0.generatedAddress = "00:0c:29:e7:9f:2b"   kept — MAC is not device-dependent
ethernet0.pciSlotNumber      ABSENT (grep -c = 0) — VMware writes a correct slot at power-on
sata0:2.fileName           = "..\unlocker428\iso\darwin.iso"
```

The false Network comment is replaced with the measured finding, the
`IOPCIPrimaryMatch` proof, an explicit *do not swap this back*, and a note
saying why `pciSlotNumber` is absent — so neither can be helpfully re-added.

**The power-on has since happened and the test passed — see below.**

### Acceptance test — PASSED 2026-08-07 18:52:35 UTC — BLOCKER CLOSED

Stated before the power-on, falsifiable both ways, and it passed:

```
power-on   2026-08-07T18:51:31.356Z   vmware.log line 1, pid=22944
lease      starts 5 2026/08/07 18:52:35   ends 19:22:35
           hardware ethernet 00:0c:29:e7:9f:2b
           client-hostname "Mac"
```

64 seconds after power-on — not the stale `16:20:12Z` leftover. Leases file mtime
12:20 → 14:52, md5 → `75b9cc9d…`.

The swap reached the **running** instance this time (`vmware.log:305` `guestOS =
"darwin24-64"`, `:340` `ethernet0.virtualDev = "vmxnet3"`, `:418` powering on
`darwin24-64`, `:687` MAC `00:0c:29:e7:9f:2b`).

All four observations that convicted `e1000e` have flipped:

| Under e1000e | Now under vmxnet3 |
|---|---|
| No lease this boot | Lease `192.168.255.129`, `starts` 18:52:35Z |
| Guest MAC absent from host ARP | Present as `192.168.255.129` **dynamic** |
| VMnet8 counters flat across 90 s | `8562/63070` → `10102/67925` (+1540 in) |
| Only the host reached Apple | The guest is on the wire |

`client-hostname "Mac"` is the strongest single detail: the guest sent a DHCP
request **carrying a hostname option**, so a macOS DHCP client genuinely ran. A
driverless NIC cannot produce that.

**The falsifier did not trigger.** The committed kill condition — *“`en0` exists
but still no lease → hypothesis dead, Mullvad back to the front”* — did not
occur. Mullvad stays **eliminated as the cause**, retained only as a second-order
suspect (tunnel MTU 1380 vs VMnet8 1500) if large transfers fail mid-download.

**PCI slot.** VMware wrote `ethernet0.pciSlotNumber = "160"` back at log line 228
— the same number as the deleted pin, but chosen *for a vmxnet3 device*, and the
VM powered on with no `[msg.pci.noslotavail]` and no `0xc0000005`. Deleting the
pin was still correct; an identical value does not mean it was harmless.

### Where it is written

`VM-BRINGUP.md` → `## Host-side network audit (t-3adea0)` (topology, proven vs
inconclusive, config drift), `## CONFIRMED CAUSE — the recovery kernelcache
has no e1000e driver (t-c14570)` (enumeration, controls, fix, hazards), and
`### Failure 5 — a running VM clobbers on-disk VMX edits at shutdown`.
