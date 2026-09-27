# handoff2.md — the forward plan, written 2026-08-07 ~19:40 UTC

**Read this first on resume, then `HANDOFF.md` for detail.** `handoff1.md` is the
previous session's point-in-time record and is still accurate; this file is the
*plan*, plus the exact next actions for the moment the macOS install finishes.

Everything here is taken from a tool run in this session or from a doc that
records one. Nothing is quoted from memory.

---

## 1. The goal changed shape this session — read this before anything else

The stated goal is now, in the user's words: **get the 3050 booting macOS first,
reverse-engineer acceleration later, and publish what is learned because it will
help the Hackintosh community.**

That reframe is correct in a way worth stating precisely, because getting it
wrong in either direction wastes months.

### What the reframe does NOT change

**Hardware-accelerated macOS on *this laptop* remains impossible**, and not for
effort reasons. Three independent blockers, any one fatal:

| Blocker | Evidence | Status |
|---|---|---|
| The 3050 has no display outputs | `lspci -nn` → PCI class `0302` (3D controller), not `0300` (VGA). `vgaswitcheroo` absent. `xrandr --listproviders` → `Providers: number : 0` | Confirmed on hardware |
| macOS has no Tiger Lake iGPU driver | Apple never shipped an 11th-gen Intel Mac | Structural |
| Metal needs a private ABI + an IR→SASS compiler | Both undocumented, no public foothold | Structural |

The card renders into a buffer the Intel iGPU scans out. macOS has no Optimus /
PRIME concept, and no driver for the chip that would do the scanning out. So
**neither chip can put a picture on that screen**, independent of driver quality.

Do not let a later session quietly re-open this. It was decided on evidence.

### What the reframe DOES change — and this is the real insight

**The GSP boot sequence is silicon behaviour. It does not care about board-level
display wiring at all.**

So the deliverable is not "a driver for one muxless laptop." It is:

> **How to bring up Ampere GSP under macOS, verified against a real mmiotrace.**

That applies unchanged to every desktop Ampere card — 3060, 3070, 3080, 3090 —
sitting in a Hackintosh build where the display-wiring problem *does not exist*.
Those users have a card with real outputs and no driver at all. This work hands
them the hardest, least-documented layer.

**Framing decision, made this session:** write it up as *"Ampere GSP bringup on
macOS"*, not *"my laptop's driver"*. The evidence already supports the broader
claim, and that is where the leverage is.

### Two working-practice consequences

1. **Write for publication as you go, not after.** `PHASE4-GSP-BRINGUP.md` is
   already close to a publishable artifact. The register sequence *and the trap
   list* are the deliverable — they have value whether or not the final
   milestone lands. Documenting retroactively loses the failure record, which is
   the expensive part and the part nobody else has.
2. **Keep "never touch flash, never touch fuses" absolutely firm.** Once
   strangers follow this writeup on their own cards, this project's safety
   envelope becomes theirs. See §5.

---

## 2. State right now

### The VM — installing, network blocker CLOSED

```
vmrun list          Total running VMs: 1   C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx
vmware.log line 1   2026-08-07T19:40:35Z era, EFI ROM VMW201.00V.24006586.B64.2406042154
lease               192.168.255.129  starts 18:52:35Z  client-hostname "Mac"
VMnet8 counters     12494 in / 73977 out  (were 8562 / 63070 and flat, pre-fix)
```

The user reports the guest is on the black Apple screen finishing the install.

**The "Internet connection is required" blocker was a missing driver, not a
network fault.** The 15.4.1 / 24E263 recovery kernelcache ships **no Intel
ethernet kext at all**, while the VM was configured `ethernet0.virtualDev =
"e1000e"`. `en0` never existed, so no `DHCPDISCOVER` was ever sent. Fix was
`vmxnet3` + a full power cycle. Confirmed by a lease **64 s after** power-on
carrying `client-hostname "Mac"` — a driverless NIC cannot send a DHCP request
with a hostname option. Full record in `VM-BRINGUP.md`.

### Verification gates — all re-run this session, all green

```
python tool tests    97/97    tests\run-python-tests.bat   (was 67 — suite grew, see handoff1 §2)
register constants  102/102   python tools/check-registers.py
ctest suites          3/3     tests\build-msvc.bat  (decode, falcon, riscv_ucode)
kext syntax gate      5/5     tests\check-kext-syntax.bat  (clang 19.1.5, incl. 4/4 negative controls)
structors pairing     1/1     python tools/check-structors.py
```

**Carry the syntax gate's own caveat whenever quoting it:** *this is a parse, not
a build.* The kext has been through a C++ front end. It has never been linked and
never been loaded. That distinction is the whole reason §4 step 3 exists.

### Machine roles — restated because this keeps getting misread

| Machine | GPU | Role |
|---|---|---|
| **Laptop** (`kenny-linus`) | GA107M, `10de:25a2` | **The only driver target.** Trace source, firmware source, load target |
| **Desktop** (`KENNYSPC`) | RTX 4060 | Build/analysis box. **Never a target** |
| **macOS VM** (on desktop) | VMware SVGA | Where `make` runs. Sees neither physical GPU |

Enforced in code: `Info.plist` has `IOPCIMatch = 0x25a210de`, exact match.
Every Ada / AD106 / AD107 mention in the docs is a **warning** — "RTX 3050 Laptop
GPU" spans four dies across two architectures — never a target.

---

## 3. What to do the moment the VM boots into macOS

This is the section the user asked for explicitly. Do these in order; each one
gates the next.

### Step 0 — before anything, re-verify the VMX survived

Non-obvious and it already bit this project once. **Power-off rewrites the VMX
from the running instance's memory, unconditionally** — see `VM-BRINGUP.md`
*Failure 5*. It discarded edits made hours earlier by a session that had already
ended, with nobody touching the file.

```sh
grep -nE 'guestOS =|ethernet0.virtualDev' "C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmx"
grep -c '^ethernet0.pciSlotNumber' "C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmx"
```

Expect `darwin24-64`, `vmxnet3`, and count `0`. If `virtualDev` has reverted to
`e1000e`, the network blocker will reappear looking like a brand-new problem.

**Rule: edit the VMX only while the VM is powered off, and re-verify the file
after every power-off.**

> **VERIFIED GREEN 2026-08-07 19:54Z (quiet-wren-9a) — Step 0 has already been
> run against the currently-running VM. Do not redo it unless the VM is
> power-cycled again.**
>
> | Check | Result |
> |---|---|
> | `guestOS` | `darwin24-64` (`Sonoma.vmx:17`) |
> | `ethernet0.virtualDev` | `vmxnet3` (`Sonoma.vmx:167`) |
> | `ethernet0.connectionType` | `nat`, `startConnected = TRUE` |
> | `ethernet0.pciSlotNumber` | count **1**, value `160` (`Sonoma.vmx:228`) |
>
> **The expected count of `0` above is wrong — correct the expectation, not the
> VMX.** VMware re-adds `ethernet0.pciSlotNumber` on every power-on. It did
> *not* pin a stale device: the hypervisor instantiated vmxnet3 anyway
> (`vmware.log:340` `DICT ethernet0.virtualDev = "vmxnet3"`, `:510` `VMXNET: send
> cluster threshold`, `:1102` `VNET: 'ethernet0' enable link state propagation,
> lsp.state = 5`, `:1221` `VMXNET3 use...`), with no e1000/vlance instantiation
> for `ethernet0`. A nonzero count is therefore **not** a failure signal.
>
> The guest driver is bound and transmitting — positive evidence, not absence of
> failure: the host DHCP server issued `192.168.255.129` to
> `00:0c:29:e7:9f:2b` three times (19:26:09, 19:40:13, 19:56:16),
> `client-hostname "Mac"` on the first two, and the host ARP table carries a
> live dynamic entry
> `192.168.255.129 -> 00-0c-29-e7-9f-2b` on the `192.168.255.1` (VMnet8)
> interface. DHCP DISCOVER/REQUEST plus ARP resolution both require a loaded,
> transmitting vmxnet3 driver in the guest.

### Step 1 — finish first boot and confirm networking from inside

> **HARD BLOCKED 2026-08-07 ~20:25Z — there is no working path into the guest
> right now. This is not a retryable failure.** Every route was measured, not
> assumed:
>
> | Route | State | Evidence |
> |---|---|---|
> | SSH (22) | closed | `Test-NetConnection` → `TcpTestSucceeded False` |
> | Screen Sharing (5900) | closed | same |
> | Kerberos (88) | closed | same |
> | VMware Tools `runProgramInGuest` | absent | `vmrun checkToolsState` → `unknown`, exit 127 |
> | `vmrun captureScreen` | refused | `VixVM_LoginInGuest` required, exit 127 |
> | Host input injection (`vm-type.py`), unelevated | not delivering | `tools/_input-block-probe.py`, exit 1 |
> | Host input injection (`vm-type.py`), **elevated** | **WORKS** | `tools/_token-probe.py` as admin, `DELIVERED: True` |
>
> The last row is the new one and it is the one that closes the loop, because
> UI injection was the fallback for everything above it. What is measured:
>
> ```
> block: held_by_other=True  (BlockInput(F) ret=False err=5)
> inject (1972,423) -> (2012,423)  sent=1 err=0  lastinput_advanced=False  DELIVERED=False
> VERDICT: injection is NOT delivering
> ```
>
> **CAUSE CORRECTED 2026-08-07 ~20:35Z — this is an integrity barrier, not a
> stuck block.** This passage previously read "some thread on the host holds
> `BlockInput(TRUE)` … reproduced independently by two terminals in two separate
> processes, which rules out a per-process UIPI artifact." **Both halves were
> wrong.**
>
> `tools/_token-probe.py`, once its `TOKEN_INFORMATION_CLASS` constants were
> fixed, reports for the agent shell's Python (pid 688):
>
> ```
> TokenElevationType  : 3  (Limited (UAC-filtered))
> TokenIntegrityLevel : 0x2000  (Medium)
> TokenHasRestrictions: 1
> TokenUIAccess       : 0
> TokenElevation      : 0
> ```
>
> A Medium-integrity, UAC-filtered token with `UIAccess = 0` is denied
> `BlockInput` and cannot `SendInput` into a higher-integrity window **by
> design**. `ERROR_ACCESS_DENIED` (5) is the documented return for that posture,
> not evidence of a foreign `BlockInput(TRUE)` holder. VMware Workstation's
> window being higher-integrity than us is sufficient on its own.
>
> The "two independent processes" argument was worthless: both terminals launch
> from the same agent shell and carry the **same** restricted token. That is one
> sample taken twice, so their agreement said nothing about the host.
>
> The wrong constants are worth recording, because the failure was silent.
> `_token-probe.py` queried 23 as `TokenSandBoxInert` (23 is
> `TokenVirtualizationAllowed`; `SandBoxInert` is 15) and 21 as
> `TokenIsRestricted` (21 is `TokenHasRestrictions`). A separate ad-hoc probe
> used 25 and 22 — `TokenIntegrityLevel` and `TokenAccessInformation`, both
> **structs** whose first field is a pointer — and printed `3957965056` and
> `3955797784`, the low 32 bits of two pointers, under boolean-sounding labels.
> `GetTokenInformation` returns success for a struct class read as a DWORD, so
> nothing errors.
>
> **`SendInput` returned `sent=1 err=0` while delivering nothing.** Do not read
> `vm-type.py`'s acceptance line as evidence the guest received anything — it
> means only that `SendInput` accepted the events. Same failure shape as
> `vmrun connectNamedDevice` returning exit 0 for a nonexistent device. Run
> `python tools/_input-block-probe.py` first and believe *it*.
>
> The tool used to print `result : injected` outright. **That line was removed
> on 2026-08-07** — it now prints how many event groups `SendInput` accepted
> plus an explicit "NOT proof of delivery" statement. Corroborated from a third
> process: 20/20 consecutive sends accepted (`sent=2/2 err=0`) with the VMware
> window foreground throughout, while a `WH_KEYBOARD_LL` hook in the same
> process observed **zero** of them.
>
> **The guest is healthy — this is a host problem.** `vmrun list` shows the VM
> running, the macOS console is visible in the window (Finder menu bar and an
> open Finder window, confirmed from the pixels), and the vmxnet3 link is
> renewing DHCP. **Visibility is not reachability.** Do not "fix" the VM.
>
> **PARTIALLY UNBLOCKED 2026-08-07 ~20:40Z — elevation lifts the integrity
> barrier; guest keystroke delivery is still UNPROVEN.**
>
> > **Scope correction ~21:00Z.** This heading previously read "UNBLOCKED —
> > elevation fixes it, measured not inferred." **That over-claimed.** What is
> > measured is that an elevated process can move the *cursor*
> > (`SetCursorPos`) where a Medium-integrity one cannot. Every elevated
> > `vm-type.py` run carried `--dry-run`, which injects nothing by definition,
> > so its `exit 0` proved only that argument parsing and `--out` work — never
> > that a keystroke reached the guest. "An elevated process can move the
> > cursor" and "`vm-type.py` can type into the guest" are different claims;
> > only the first is supported. Closing the second requires an elevated,
> > **non-dry-run** send whose effect is observed *inside the guest*.
>
> `_token-probe.py` relaunched through `Start-Process -Verb RunAs` (pid 64748):
>
> ```
> TokenElevationType  : 2  (Full (elevated))
> TokenIntegrityLevel : 0x3000  (High)
> TokenHasRestrictions: 0
> TokenUIAccess       : 0
> SetCursorPos     : ret=True err=0  -> (893, 590)
> DELIVERED        : True
> VERDICT          : injection WORKS from this process
> ```
>
> The cursor physically moved — that is *cursor* delivery, not keystroke
> delivery to the guest. Two details finish off the earlier misdiagnosis of
> **cause**:
> `TokenUIAccess` is **still 0** in the working run, so High integrity alone was
> enough and `UIAccess = 1` was never needed; and `BlockInput(FALSE)` returned
> `err=0` when elevated versus `err=5` at Medium — no `ERROR_ACCESS_DENIED`
> means there was no foreign `BlockInput(TRUE)` to be denied against. The stuck
> block did not exist.
>
> **So a host reboot was never the fix, and `t-ce4863` is obsolete.** The route
> to *attempt* is: relaunch `tools/vm-type.py` elevated via `Start-Process
> -Verb RunAs` — then confirm the effect inside the guest before believing it. Elevated children share no stdout with the launching shell, so
> pass `--out <path>` when you need the result back. **`--out` did not exist
> until 2026-08-07** — it was documented here before it was implemented, and has
> since been added and verified.
>
> The exact invocation. **`--dry-run` is present deliberately** — this was
> verified only as far as "parsed, targeted, wrote its report" (exit 0, file
> written, `type   : 'echo hi'`). Nothing was injected, so this run is **not**
> evidence of guest delivery:
>
> ```powershell
> Start-Process -FilePath 'C:\Python314\python.exe' `
>   -ArgumentList 'tools\vm-type.py','--window','Sonoma',`
>                 '--text','"echo hi"','--dry-run',`
>                 '--out','C:\Users\Kentl\macos-vm\sonoma\out.txt' `
>   -WorkingDirectory 'D:\nerd shit\other projects\mac os' -Verb RunAs
> ```
>
> **Quoting trap — note the inner quotes on `"echo hi"`.**
> `Start-Process -ArgumentList` joins its elements with spaces and does *not*
> quote them, so an element containing a space is re-split by the child. Three
> elevated runs differing only in `--text`:
>
> | `--text` element | Result |
> |---|---|
> | `'echo hi'` | exit 2, **no `--out` file at all** |
> | `'echohi'` | exit 0, file written |
> | `'"echo hi"'` | exit 0, file written, text intact |
>
> `echo hi` reaches the child as `--text echo` plus a stray positional `hi`;
> argparse rejects it and exits 2 **before** `--out` is wired up. So **exit 2
> with no `--out` file is an argument-parsing failure, not a window-targeting
> failure** — a real targeting failure happens after the tee is installed and
> does write the file. Do not go looking for a missing window.
>
> `DELIVERED=False` remains true for **unelevated** processes. What changed is
> *why*, and therefore what fixes it.

Complete Setup Assistant. Then in Terminal:

```sh
ifconfig -a          # en0 must exist with a 192.168.255.x address
netstat -rn | head   # default route via 192.168.255.2
curl -sI https://apple.com | head -1
```

The gateway **and** the DNS forwarder are both `192.168.255.2`, not `.1`.

If small requests work but large transfers stall mid-stream, the suspect is the
Mullvad tunnel's **MTU 1380** against VMnet8's **1500**. That is *eliminated as
the cause of the old blocker* but retained as a live second-order suspect. Probe
it with `ping -c3 -s 1400 1.1.1.1` before theorising.

### Step 2 — install the toolchain

- **Xcode** + Command Line Tools (`xcode-select --install`)
- **The KDK** (Kernel Debug Kit) matching the guest build — `24E263`. Free Apple
  developer account; the \$99 program is **not** required for local development.
- Install VMware Tools from `sata0:2` (`darwin.iso`, **not** `darwinPre15.iso` —
  that one is for guests older than macOS 15).

> **Measured 2026-08-07 (quiet-wren-9a): until VMware Tools is installed *and*
> guest credentials are configured, `vmrun captureScreen` fails with `Anonymous
> guest operations are not allowed on this virtual machine. You must call
> VixVM_LoginInGuest before performing guest operations` (exit 127), both with
> and without `-T ws`.** No agent can screenshot the framebuffer before this
> step completes — read the VM window directly, and use host-side evidence
> (vmdk mtime/size deltas, `vmware.log`, DHCP leases, host ARP,
> `vmware-vmx` CPU time) to judge guest liveness in the meantime. Per
> `VM-BRINGUP.md`, absence of change is never proof of a hang; only positive
> evidence counts.

Snapshot the VM here. This is the first state worth being able to return to.

### Step 3 — build the kext for real. **This is the milestone.**

```sh
cd phase0-kext && make
```

Every gate to date is a parse or a host-side test. This is the **first actual
compile and link**, and it is the first thing that can fail in ways
`-fsyntax-only` structurally cannot see — unresolved symbols, missing
`OSDefineMetaClassAndStructors`, KPI mismatches, linkage against the wrong SDK.

Expect failures here and treat them as information, not setbacks. `NVDecode.hpp`,
`NVFalcon.hpp` and `NVRiscvUcode.hpp` are deliberately dependency-free so the
host tests compile the *identical* files the kext ships — but nothing has ever
linked them against real IOKit.

**Do not report the kext as "builds" until `make` exits 0 and produces a bundle.**

### Step 4 — two-machine KDP debugging

Desktop VM ↔ laptop. Milestone: deliberately panic a hello-world kext and catch
it in `lldb` with symbols resolving.

This is worth the setup cost *before* Phase 4, not after. Phase 4 is the first
phase that writes to hardware; the failure mode is a hang or a panic, and
without KDP you are debugging by reboot.

### Step 5 — load `phase0-kext` on the laptop. **This is the pivotal step.**

Everything built so far *predicts* what those registers will read. **Nothing has
verified it.** Step 5 converts the whole project from "believed correct" to
"observed correct", and it is the cheapest possible test of the assumption
everything else rests on: that macOS's PCI initialisation leaves the GSP in a
bootable state rather than resetting the engine out from under the VBIOS.

Expected readings (all reads, zero writes — this cannot brick anything):

| Register | Expected |
|---|---|
| `NV_PMC_BOOT_0` | `0x177000xx` (arch `0x17`, impl `0x7`) |
| `NV_PMC_BOOT_42` | GA107 decode |
| `GFW_BOOT_PROGRESS` | polls to `0xFF` |
| `GSPBootable` verdict | RISC-V capable, not in engine reset, no latched fault, WPR2 down |

**If any of these disagree with the prediction, stop and write it down before
proceeding.** A disagreement here invalidates assumptions Phase 4 is built on,
and it is far cheaper to find at step 5 than at step 6.

### Step 6 — Phase 4 milestone 1

Only after step 5 is green. See §4.

---

## 4. Phase 4 — the first thing that writes

Read `PHASE4-GSP-BRINGUP.md` before touching this. It is a register-by-register
account of what the working Linux driver actually did **to this die**, derived
from `mmio-boot.log` rather than from reading NVIDIA's source. **Where the two
disagree, the trace wins** — §4 of that document is an assumption from NVIDIA's
code that the capture disproved.

### The advantage most attempts do not have

1. **A trace of real silicon.** Not documentation, not interpreted source — a
   recording of every register access in order, from this exact GPU.
2. **The actual firmware blobs**, pulled over SSH from the laptop's
   `nvidia-595-open` 595.84 install:

| File | Size | Role |
|---|---|---|
| `bootloader-570.144.bin` | 24,684 B | The image the trace DMA'd into IMEM |
| `booter_load-570.144.bin` | 61,304 B | SEC2 Booter Load — raises WPR2, authorises GSP |
| `booter_unload-570.144.bin` | 41,080 B | SEC2 Booter Unload |
| `vbios-ga107.rom` | 65,024 B | VBIOS, `Version 94.07.4E.00.4B` |

`PHASE4-GSP-BRINGUP.md` §3.3 *inferred* a ~24 KB IMEM payload from 96 × 256 B DMA
transfers. The bootloader is 24,684 bytes. The inference was right and the real
image is now local.

### Milestones — each falsifiable on its own

Do not proceed past a step you cannot observe succeeding.

1. **Engine reset + scrubbing poll.** Write `FALCON_ENGINE` reset, poll `HWCFG2`
   for scrubbing complete. Uses reads already validated, plus one write.
2. **`DMATRFBASE` + the 256 B IMEM loop.** DMA the bootloader in 96 transfers,
   exactly as the trace shows. **This is the first real milestone** — it needs
   only the bootloader descriptor and *zero* RPC structures, which is why the
   version gap below does not block it.
3. **Boot ROM parameter block** (§3.4), then **RISC-V core start** (§3.5).
4. **SEC2 Booter Load.** Ordering is **GSP first, then SEC2** (§4a).
5. **Init RPC / heartbeat.** GSP responds.

### Three things that will bite

**The version gap.** Blobs are **570.144**; the kext's constants were transcribed
from **610.57.04**. Not blocking for milestones 1–2. Becomes blocking at Booter
Load / init-RPC. Three ways out are in `HANDOFF.md` §8 blocker 3.

**The booters are heavy-secure signed with a signature patch point** —
`sig_prod_size = 768`, `patch_loc = 828`, `num_sig = 848`. Phase 4 cannot load
them without performing that patch. No shortcut exists.

**The three blobs look like one format and are two.** All are `nvfw_bin_hdr`
containers, **not** VBIOS-embedded `FALCON_UCODE_DESC` images. `header_offset` is
`24` in all three, so a single parser *looks* like it should work. Verified this
session by re-parsing all three:

```
bootloader     magic=0x10de ver=1  hdr_off=24  data_off=108  desc= 84 B =  21 fields  -> RM_RISCV_UCODE_DESC v5
booter_load    magic=0x10de ver=1  hdr_off=24  data_off=888  desc=864 B = 216 fields  -> nvfw_hs_header_v2
booter_unload  magic=0x10de ver=1  hdr_off=24  data_off=888  desc=864 B = 216 fields  -> nvfw_hs_header_v2
```

Parse all three alike and the bootloader yields clean fields while the booters
yield plausible nonsense (`bootloaderOffset = 0x0000f000`, past EOF). A scan for
a V2/V3 descriptor header found **none** in any blob — that null result *was* the
answer. The `0x3c` = 60 word that looks like `FALCON_UCODE_DESC_V2_SIZE_60` is a
coincidence; it is `sig_prod_offset` in the HS header.

Bootloader descriptor, confirmed by dump this session: `version = 5`,
`bootloaderOffset = 0x5000`, `bootloaderSize = 0x880` (2176),
`bootloaderParamOffset = 0x5880`, `bootloaderParamSize = 16`.

---

## 5. Safety envelope — do not widen it

This matters more now that the work is intended for publication. Whatever line
this project draws becomes the line strangers follow on their own cards.

**Safe — and this covers Phases 4 and 5 essentially in full:**

- MMIO reads. Cannot brick anything.
- Loading firmware into IMEM and starting the GSP. Volatile — power-cycle clears
  it. Worst case is a wedged GSP or a host panic.
- Engine resets, DMA, command submission. All volatile.

**Never, and not in this plan:**

- **Writing the VBIOS flash.** This is what permanently bricks cards.
- **Fuses / OTP.** One-way, unrecoverable.
- **Power and clock registers.** Pushing voltage or clocks past what the VRM can
  follow can damage silicon. This is why Phase 6 power management sits off the
  critical path.

The realistic hazard is a **soft brick during a firmware experiment** — GSP
wedged, card unresponsive until a **full power cycle** (actual power off; some
state survives a warm reset). Annoying, recoverable.

Additional constraint specific to this target: the laptop's GPU is soldered.
There is no second card and no swap path. That argues for treating the two rules
above as hard limits rather than guidelines.

---

## 6. The ceiling, stated once so it is not re-litigated

| Phase | Status |
|---|---|
| 0–3.75 | **Done**, verified host-side. Not yet verified on silicon (step 5) |
| 4 — GSP firmware boot | **Next.** Trace + blobs in hand |
| 5 — memory + command submission | Reachable. **The honest end of the project** |
| 6 — compute | Requires hand-assembling SASS for an ISA with no public assembler. Possible, painful |
| 7 — Metal / display | **Wall.** Private plugin ABI + an IR→SASS compiler backend, both undocumented |

Phase 5's milestone: *the GPU executes something you told it to and reports
completion.* A card that computes and never draws.

Also a fixed clock on the platform: **macOS Tahoe (26) is Apple's final Intel
release.** And the kext will need rebuilding against each major macOS release —
the register constants and GSP sequence survive unchanged (they describe NVIDIA
silicon), but the IOKit KPI does not. Develop on Sequoia because that is what is
installed and it is a known quantity; treat 26 as a later port, not a design
target.

---

## 7. Method rules earned the hard way — apply these

These cost real time this session. They are cheap to inherit.

**Pre-egress vs post-egress evidence.** To tell "guest is not talking" from
"guest is talking but blocked", use interface byte counters and the host ARP
table. No host-side reachability test can make that distinction — they all pass
while the guest is mute, which reads as reassuring and sends you hunting
downstream. This is what eventually cracked the network blocker after a long
detour into a VPN theory.

**Run the control before believing a negative.** Three tests were reported as
evidence and then retracted: `ping -S 192.168.255.1` failed — but so did *every*
non-tunnel source address, because a VPN owned the default route. `vmnat` does
not reliably answer host-originated ICMP or DNS either. A negative result from an
untested method is not evidence.

**An UNTESTED hypothesis is not a FALSIFIED one — and this one cost the most.**
The input-injection blocker was diagnosed wrong four times in a row. The fourth
was the expensive one: elevation was listed among six "falsified" causes, but it
had never actually been tested. The discriminating experiment (`schtasks /RL
HIGHEST`) was **denied** because the agent shell is not admin, and
"could not run the test" silently became "the test came back negative." That
gap was then broadcast to the crew as a settled conclusion: *"the failure is
below user32, no user-mode fix reaches it."* Elevation was the true cause. When
a discriminating experiment cannot be run, record it as **UNTESTED**, say so out
loud, and keep it on the list. Corollary: the operator running one elevated
command beat four rounds of agent probing — when permission blocks the decisive
experiment, ask the human to run it.

**A byte pattern is not a conclusion.** `user32!SendInput`, `SetCursorPos` and
`BlockInput` all begin `ff 25` (indirect jmp), which was reported as
`<-- HOOKED` and broadcast. Following each pointer to its destination showed all
three land in **win32u.dll** — normal forwarders present on every Windows 11
box. Resolve to a named object or a positive post-condition; a pattern match
that *could* mean something is not evidence that it does.

**Absence is one-directional.** Flat counters, an unchanged file, a static
screenshot — these corroborate, they never prove. Ground findings in positive
evidence. The e1000e diagnosis rests on *enumerating* the kernelcache's prelinked
kext list, not on failing to find a name. A cursor frozen across six samples in
three seconds was read as "host input is dead"; the next probe found it moved.
Idle is not blocked, exactly as an unchanged framebuffer is not a hung VM.

**An unchanged artifact is only meaningful against a recorded before-state and a
known event.** "The lease file didn't change" means nothing if no boot happened.

**Hash, don't diff, when another agent may be writing.** During the VMX repair,
`diff` reported two files identical while `md5sum` and `wc` *in the same command
batch* showed them plainly different. Two reads in one batch are not atomic.

**Timestamps beat plausibility.** A DHCP lease that looked like proof the guest
had networking turned out to predate the running VM's power-on by 16 minutes.
Check when a thing happened before deciding what it means.

---

## 8. Where everything lives

| File | What it is |
|---|---|
| `handoff2.md` | **This file.** The forward plan and the post-boot sequence |
| `HANDOFF.md` | The detailed resume document. Phase 4 blockers in §8 |
| `handoff1.md` | Previous session's point-in-time record. Still accurate |
| `PHASE4-GSP-BRINGUP.md` | **Read before Phase 4.** Register-by-register, from the trace |
| `VM-BRINGUP.md` | Five power-on failures, the e1000e root cause, the VMX clobber trap |
| `NETWORK-DIAGNOSIS.md` | The network hop chain, now resolved |
| `HARDWARE.md` | Termination conditions A and B, with evidence |
| `PLAN.md` | Original framing. Phase table still valid |
| `STEPS.md` / `SETUP.md` | Operational procedure |
| `reference/ga107-firmware/` | The four blobs, plus a README on what each is |

---

## 9. Immediate next action

**Superseded 2026-08-07 ~20:55Z. The install is finished and step 0 is done.**

The wait is over. Verified this session:

- `vmrun list` → the VM is running; VMX intact (`guestOS = darwin24-64`,
  `ethernet0.virtualDev = vmxnet3`, one `pciSlotNumber` line, 10080 bytes).
- **Setup Assistant COMPLETED.** The DHCP `client-hostname` on
  `192.168.255.129` changed `Mac` → `kennys-Mac` at 19:56:16Z and the lease
  duration jumped 30 min → 2 h — the installer environment's short lease gave
  way to the installed system's. Read the history from
  `vmnetdhcp.leases~`, not the live file: it self-compacted to a single record.
- The guest is at a macOS **Finder desktop**, and it is alive by positive
  evidence — ping replies 3/3, host ARP holds `00-0c-29-e7-9f-2b`, and the
  guest menu-bar clock advanced 12:50PM → 12:55PM → 1:00PM across real host
  intervals.

So start at **§3 Step 1** (the in-guest network checks), and reach the guest
like this:

1. Run `python tools/_input-block-probe.py` first. Exit 1 means injection is not
   delivering, and `vm-type.py`'s "accepted by SendInput" count tells you
   nothing about whether the guest saw anything.
2. Relaunch `tools/vm-type.py` **elevated** — unelevated agent shells cannot
   inject; see §3's elevation note. Use the wrapper
   `tools/_vm-type-elevated.cmd`, **not** a bare `Start-Process -Verb RunAs`
   on `python`: an elevated child is a new process with its own console and
   shares no stdout with the launching shell, so a direct launch injects and
   shows you nothing. `vm-type.py` now has an `--out PATH` flag for exactly
   this (added 2026-08-07, after both this file and `tools/README.md` had
   documented it before it existed — the docs led the code, and an earlier
   revision of this paragraph then wrongly "corrected" them by asserting the
   flag did not exist). Either mechanism works: pass `--out` yourself, or use
   the wrapper, which redirects from *inside* the elevated process into
   `shots/vm-type-elevated.log`, truncated each run so a stale success line
   cannot be mistaken for a fresh one. Before injecting it asserts elevation
   actually happened (`net session`) — a dismissed UAC prompt otherwise yields a
   no-op that still prints an accepted-events count — and gates on
   `tools/_input-block-probe.py`, aborting unless an injected mouse move
   produced an observed cursor move.

   ```sh
   powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath 'D:\nerd shit\other projects\mac os\tools\_vm-type-elevated.cmd' -ArgumentList '--window','Sonoma','--click'"
   cat "D:/nerd shit/other projects/mac os/shots/vm-type-elevated.log"
   ```

   This needs the **operator** to accept the UAC prompt. Agent shells cannot
   self-elevate: `IsUserAnAdmin` is False and `schtasks /RL HIGHEST` is denied.
3. Do not use `--key cmd+space`. `cmd` is the host left-Win key and the Windows
   shell swallows `Win+Space` as the input-language hotkey, so Spotlight never
   opens while the tool still reports success. Use `--click-at` (guest-relative)
   or type into an already-focused field.

The first thing worth doing inside the guest is **enabling Remote Login**
(`sudo systemsetup -setremotelogin on`). It converts every later step from
screenshot-and-inject into a real shell, and it is the cheapest step in the
list. Do not skip §3 step 5 — it is the only one that converts prediction into
observation.
