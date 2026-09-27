# NV3050Probe — Phase 0/2/3 kext skeleton

A minimal, real IOKit driver that claims the RTX 3050 on the PCI bus, maps BAR0, and reads the chip identity register. It does nothing useful to the GPU — that is the point. It is the scaffolding every later phase is built on, and it exercises the three things that must work before anything else can: matching, memory mapping, and MMIO.

## What it does

| Phase | Behaviour | Where to look |
|---|---|---|
| 2 — Enumeration | Matches `pci10de,*`, attaches, publishes properties | IORegistryExplorer |
| 3 — Register access | Maps BAR0, reads `NV_PMC_BOOT_0` and `NV_PMC_BOOT_42`, decodes architecture/implementation/chip ID | `log stream` output |
| 3.5 — Firmware health | Polls `GFW_BOOT_PROGRESS` for `0xFF`, reads usable framebuffer size | `log stream` output |
| 3.75 — GSP engine | Reads `HWCFG2`/`HWCFG`, `FALCON_ENGINE`, both `CPUCTL`s, `BCR_CTRL`, and the two error latches | `log stream` + `GSPBootable` in IORegistry |

Still zero writes. Every register touched is read-only, so nothing here can put the card into a bad state or corrupt anything.

### Why Phase 3.75 is the one that decides the project

Everything before it confirms the card is present and healthy. Phase 3.75 answers the question the whole plan rests on: **does macOS's PCI initialisation leave the GSP in a bootable state, or does it reset the engine out from under the VBIOS?**

If `GSPBootable` comes back true, Phase 4 is viable on this hardware and the firmware push is the next real work. If it comes back false, that is a log line today instead of an unexplained hang three months into firmware plumbing.

The go/no-go is: RISC-V capable, not held in engine reset, and no fault latched before the driver issued a single access. Scrubbing may still be in progress — Phase 4 waits on it.

All reads. `PRIV_ERR_STAT` and `HUB_ERR_STAT` are worth noting specifically: GSP has no console, so those two registers are the *only* diagnostics Phase 4 will ever have. A nonzero value there before we have written anything is worth understanding now.

### Why Phase 3.5 matters more than Phase 3

`NV_PMC_BOOT_0` responds on a card whose firmware never started — it proves the BAR mapping works, not that the GPU is healthy. `GFW_BOOT_PROGRESS == 0xFF` is the read that proves the VBIOS-resident firmware came up **under macOS's PCI initialisation**, which is a genuinely open question: macOS may reset the device after the VBIOS runs, and if it does, the whole GSP path in Phase 4 is dead before it starts. This is the cheapest possible test of that. NVIDIA's own `kgspWaitForGfwBootOk_TU102()` polls the identical field.

`NV_PMC_BOOT_0` is the one register guaranteed readable immediately after PCI enable, with no firmware, no clocks, and no initialisation. If it returns a plausible Ampere ID, you are genuinely talking to silicon. If it returns `0xFFFFFFFF`, the BAR is not mapped or the device is not powered — that distinction is the entire debugging value of this kext.

Expected decode for GA107 (mobile RTX 3050): architecture `0x17` (Ampere), implementation `0x7`, so `NV_PMC_BOOT_0` should read **`0x177000xx`** and `NV_PMC_BOOT_42` should yield composite chip ID **`0x177`**. Full field layout and the constants behind those numbers are in `../DEVICE-IDS.md`.

Two things worth knowing before you compare values:

- The architecture field in `BOOT_0` is **split** across bits 28:24 and bit 8. The decoder here reassembles both, matching `decodePmcBoot0Architecture()` in NVIDIA's `nv_ref.h`. Reading only 28:24 gives the right answer for Ampere by luck.
- If `lspci` reports `0x2822`, `0x28A3`, or `0x28E3`, your card is an **"RTX 3050 A"** — which is **AD107 Ada**, not Ampere, despite the name. Expect arch `0x19` and retarget the whole plan. See `../DEVICE-IDS.md`.

## File layout

| File | What it is |
|---|---|
| `NVDecode.hpp` | **Every register constant and bit-field decode.** Dependency-free — no includes, all plain builtins — so `tests/` compiles the identical file under MSVC and actually runs it. This is the only file that should ever declare a register. |
| `NV3050Probe.hpp/.cpp` | The IOKit shell: PCI match, BAR0 map, logging, IORegistry publication. Declares no constants of its own. |
| `Info.plist` | Match dictionary and probe score. |
| `Makefile` | Kernel build flags, install/load/log/check targets. |

The split exists so the bit twiddling that ships is the same code the tests
exercise, not a copy that can drift. Verified two ways:
`tools/check-registers.py` (113/113 — constants match NVIDIA's headers),
`tests/build-msvc.bat` (634/634 across five suites — the decoders extract the
right bits), and
`tests/check-kext-syntax.bat` (clang parses this IOKit shell clean against the
stub tree in `tests/kextshim/` — a parse, not a build). None of the three
catches the other's failures.

## Before it will load

1. SIP disabled and `kext-dev-mode=1` — see `../SETUP.md` Step 2
2. Set your real device ID in `Info.plist` → `IOPCIMatch`. The shipped value is `0x000010de&0x0000ffff` — a masked wildcard that matches **any** NVIDIA card in the machine. Narrow it to the exact device as soon as you have the real ID from `lspci -nn`, e.g. `0x25a210de` for `10de:25a2`. Until then, `probe()` is what stops the kext claiming the wrong function.

## Build

```sh
make                 # produces NV3050Probe.kext
make install         # copies to /Library/Extensions, fixes ownership
make load
make log             # watch it in real time
make unload
```

Panic-test build for the Phase 0 KDP milestone:

```sh
make clean && make PANIC_TEST=1 && make install load
```

That deliberately dereferences null in `start()`. Catch it in `lldb` from the debugger machine with a symbolicated backtrace showing `NV3050Probe::start` and Phase 0 is complete.

## Reading the output

```sh
sudo dmesg | grep NV3050
# or
log stream --predicate 'sender == "NV3050Probe"' --level debug
```

Good:
```
NV3050Probe: probe: 10de:25a2 subsys 0x........ class 03:00
NV3050Probe: start: PCI command register now 0x0007
NV3050Probe: mapBar0: phys 0x........ virt 0x........ len 0x1000000
NV3050Probe: NV_PMC_BOOT_0  = 0x177000a1  arch=0x17 (Ampere) impl=0x7 rev=10.1
NV3050Probe:   Ampere confirmed: GA107
NV3050Probe:   GA107 -- this is the mobile RTX 3050 die. Phase 3 milestone reached.
NV3050Probe: NV_PMC_BOOT_42 = 0x177010a1  chipid=0x177 arch=0x17 (Ampere) impl=0x7 rev=1.0.10
NV3050Probe:   chip ID 0x177 matches GA107
```

The revision digits will differ by stepping — `arch`, `impl`, and `chipid` are the fields that must match.

Bad, and what each means:
- `0xFFFFFFFF` — BAR unmapped, device in D3, or bus master not enabled
- `0x00000000` — mapping succeeded but points at nothing; wrong BAR index
- no match at all — another driver claimed the device first, or the match dictionary is wrong. Check `IOProbeScore`.

## The competing-driver problem

On a real Mac-like system, `IOPCIFamily` will bind a generic driver to any unclaimed device. Your `IOProbeScore` of 100000 in `Info.plist` outbids it. If something else still wins, `ioreg -p IOService -w0 -l | grep -A5 pci10de` shows you who.

## Honest scope

This compiles and loads. It reads one register. Everything past that — GSP firmware boot, page tables, command submission — is Phase 4+, and is where the real difficulty starts. See `../PLAN.md` §4 for the conditions under which none of it produces a picture on your laptop regardless.
