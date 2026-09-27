# `Sonoma.vmx` — annotated inventory (t-b8cf72)

**Subject file:** `C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx`
**State when audited:** 2026-08-10 — 10,466 B, 237 lines, md5 `e20b7667bea364e33c04cffb41976839`
**This audit was read-only. The VMX was not edited** (hash re-checked after writing this file: unchanged).

Regenerate the raw evidence with:

```sh
wc -c   "C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmx"
md5sum  "C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmx"
grep -n '^[a-zA-Z_.]'  "C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmx"   # key lines only
grep -n '^# ---'        "C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmx"   # section dividers
```

---

## Read this first: four claims in circulation that this file contradicts

### 1. "The file is 10,251 B" — stale

The `t-b8cf72` card and `handoff3.md:449` both state 10,251 B. **The live file is
10,466 B, 237 lines.** A size mismatch here is not evidence of a clobber — VMware
appends to this file on every power-on, so the size quoted in any document is a
snapshot with an expiry date. Use the key inventory below, not the byte count.

### 2. "`ethernet0.pciSlotNumber` is deliberately ABSENT" — the comment is stale; the key is present and is *not* a clobber

`L161` states the key is deliberately absent. `L228` sets it to `"160"`. Both
facts are real and the configuration is fine.

The comment describes the **authored** state. Hand-pinning slots caused two
power-on crashes (`L30`–`L48`, including `ethernet0.pciSlotNumber = "21"`), so the
authored file pins nothing and lets VMware compute the topology. VMware then did
exactly that and wrote the result back into the tail of the file. The value
`"160"` happens to be the same number that was once hand-pinned for the e1000e
device, which is what makes this trap convincing.

**How to tell a hand-pin from a write-back — there is no divider comment for this.**
The authored region ends at `L195` (`uuid.bios`); `L196` (`uuid.location`) onward
is VMware's tail. That boundary is *positional and unlabelled* — do not go looking
for a `# --- Runtime write-back` header, because none exists. The reliable test is
the key list:

| Test | Result |
|---|---|
| `pciSlotNumber` keys in the file | **9**, all at `L197`–`L204` and `L228` |
| Any `pciSlotNumber` above `L196`? | **No** — the only mentions above are inside comments at `L39`, `L43`, `L161` |

So: nine slot keys, all in the tail, none hand-authored. Nothing to fix.

### 3. "The guest is macOS 15.4.1 Sequoia (24E263)" — that is the *recovery image*, not the running guest

`L6`–`L14` describe the installer media and are correct about it: the
`SystemVersion.plist` embedded in `BaseSystem.img` reports 15.4.1 / 24E263.

The **running guest** is different. `L232` carries VMware's guest-originated
write-back:

```
guestInfo.detailed.data = "architecture='X86' bitness='64' buildNumber='24G830'
  distroName='macOS' distroVersion='15.7.9' familyName='Darwin'
  kernelVersion='24.6.0'"
```

**macOS 15.7.9, build 24G830.** The install completed and the OS updated past the
recovery image. Any document describing the guest as 15.4.1 is describing the ISO.
This key is guest-originated — it can only have been written by tooling running
inside the guest — which makes it the strongest OS-identity evidence in the file.

Also: the directory, `displayName` (`L179`), `nvram` (`L180`), `extendedConfigFile`
(`L181`) and the vmdks all say "Sonoma". **Nothing in this VM is Sonoma.** `L12`
says so explicitly. The names are historical; renaming them buys nothing and
breaks every path in the docs.

### 4. "`cleanShutdown = FALSE` means something broke" — it means the VM is running or was hard-stopped

`L214` is `cleanShutdown = "FALSE"` and `L215` is `softPowerOff = "FALSE"` in the
**live** file. The *clobbered* backup has `cleanShutdown = "TRUE"`. That direction
is counter-intuitive and easy to write down backwards: the older, worse file looks
"cleaner" by this key. It is a power-state marker, not a health marker.

### 5. "`toolsInstallManager.lastInstallError = 21004` means the tools install failed" — it does not

`L233` carries `21004`. Per `handoff3.md:153`–`154`, this was already misread once
as an install failure. **21004 means the guest holds the CD** — the tools ISO is
mounted and in use inside the guest. Combined with `sata0:2.startConnected = TRUE`
(`L137`) and `toolsInstall.origFileName` (`L229`), the consistent reading is
"tools media attached and claimed by the guest", not "installation errored out".

Do not treat this key as a fault signal, and do not act on it by detaching the
ISO.

---

## The authored / runtime boundary

| Region | Lines | Owner |
|---|---|---|
| Authored configuration | `L1`–`L195` | Humans. Ends at `uuid.bios`. |
| VMware write-back tail | `L196`–`L237` | VMware. Starts at `uuid.location`. |

**There is no comment marking this boundary.** The section dividers in the file
stop at `# --- Misc` (`L183`). When diffing this VMX against a backup, diff only
`L1`–`L195`; differences below are expected and meaningless.

---

## Section map (dividers as they actually appear)

| Line | Divider |
|---|---|
| 5 | `# --- Guest OS` |
| 21 | `# --- Firmware` |
| 25 | `# --- CPU / memory` |
| 30 | `# --- PCI topology` |
| 69 | `# --- Disk layout` |
| 101 | `# --- Installer media` |
| 130 | `# --- Guest tools ISO` |
| 139 | `# --- Network` |
| 183 | `# --- Misc` |

---

## Authored configuration (`L1`–`L195`)

| Line | Key | Value | Note |
|---|---|---|---|
| 1–3 | `.encoding`, `config.version`, `virtualHW.version` | `windows-1252`, `8`, `21` | |
| 17 | `guestOS` | `darwin24-64` | macOS 15 family. The clobbered backup had `darwin23-64`; current value is the corrected one. |
| 18–19 | `smc.present`, `smc.version` | `TRUE`, `0` | Required for macOS. |
| 23 | `firmware` | `efi` | |
| 26–28 | `numvcpus`, `cpuid.coresPerSocket`, `memsize` | `4`, `2`, `8192` | |
| 51–66 | `pciBridge{0,4,5,6,7}.present`, `.virtualDev = pcieRootPort`, `.functions = 8` | | Bridges declared, **no slots pinned**. This is the fix for the crashes at `L30`–`L48`. |
| 134–137 | `sata0:2.present`, `.fileName`, `.deviceType`, `.startConnected` | `TRUE`, `C:\Program Files (x86)\VMware\VMware Workstation\darwin.iso`, `cdrom-image`, `TRUE` | Correct ISO: `darwinPre15.iso` is for pre-macOS-15 guests and this guest is 15.x. Note the path is now **absolute** — both backups used the relative `..\unlocker428\iso\...` form. |
| 165–169 | `ethernet0.present`, `.connectionType`, `.virtualDev`, `.addressType`, `.startConnected` | `TRUE`, `nat`, **`vmxnet3`**, `generated`, `TRUE` | `vmxnet3`, not `e1000e`. Both backups still say `e1000e`. |
| 179–181 | `displayName`, `nvram`, `extendedConfigFile` | `Sonoma`, `Sonoma.nvram`, `Sonoma.vmxf` | Historical names — see §3. |
| 184–189 | `tools.syncTime`, `powerType.*`, `msg.autoAnswer` | `FALSE`, all `soft`, `TRUE` | |
| 190–195 | `vhv.enable`, `virtualHW.productCompatibility`, `vmxstats.filename`, `numa.autosize.*`, `uuid.bios` | | `uuid.bios` at `L195` is the last authored line. |

### The PCI slot rule, stated once

`L30`–`L48` records two power-on crashes caused by hand-pinned `pciSlotNumber`
values. Slots 16–22 are reserved for the bridges themselves, so pinning a device
into that range collides. The resolution: declare the bridges, pin nothing.

**Do not add a `pciSlotNumber` key above `L196`.**

---

## VMware write-back tail (`L196`–`L237`) — do not hand-edit, do not diff

| Line | Key | Value |
|---|---|---|
| 196 | `uuid.location` | `56 4d 3f fb …` — first tail line |
| 197–201 | `pciBridge{0,4,5,6,7}.pciSlotNumber` | `17`, `21`, `22`, `23`, `24` |
| 202–204 | `usb`, `usb_xhci`, `sata0` `.pciSlotNumber` | `32`, `192`, `33` |
| 205 | `sata0:0.redo` | `""` |
| 206–210 | `svga.vramSize`, `vmotion.checkpoint*`, `vmotion.svga.*` | `268435456`, `134217728`, … |
| 211 | `ethernet0.generatedAddress` | **`00:0c:29:e7:9f:2b`** — matches the observed DHCP lease MAC (`handoff3.md:190`). This is the anchor tying the VMX to the lease. |
| 212 | `ethernet0.generatedAddressOffset` | `0` |
| 213 | `monitor.phys_bits_used` | `45` |
| 214–215 | `cleanShutdown`, `softPowerOff` | `FALSE`, `FALSE` — see §4 |
| 216–225 | `usb_xhci:{6,7}.*` | hub descriptors (`speed`, `present`, `deviceType = hub`, `port`, `parent = -1`) |
| 226 | `toolsInstallManager.updateCounter` | `17` |
| 227 | `sata0:1.redo` | `""` |
| 228 | `ethernet0.pciSlotNumber` | `160` — the trap in §2. Legitimate write-back. |
| 229–231 | `toolsInstall.origFileName`, `.origType`, `.origClientDevice` | `..\unlocker428\iso\darwin.iso`, `cdrom-image`, `FALSE` — the **relative** path the ISO was originally attached from, before `sata0:2.fileName` (`L135`) became absolute |
| 232 | `guestInfo.detailed.data` | `…24G830… 15.7.9…` — guest-originated, see §3 |
| 233 | `toolsInstallManager.lastInstallError` | `21004` — see §5 |
| 234–237 | `usb_xhci:4.*` | HID device (`deviceType = hid`, `port = 4`, `parent = -1`) |

---

## Backup files in the same directory

| File | Size | Lines | md5 | What it is |
|---|---|---|---|---|
| `Sonoma.vmx` | 10,466 | 237 | `e20b7667…` | **Live, current, correct.** |
| `Sonoma.vmx.bak-20260807` | 8,629 | 209 | `8f92072d…` | Pre-vmxnet3. |
| `Sonoma.vmx.bak-pre-vmxnet3` | 8,629 | 209 | `8f92072d…` | **Byte-identical to the row above** — same md5. Two names, one snapshot. |
| `Sonoma.vmx.bak-CLOBBERED-20260807T1841Z` | 7,844 | 198 | `1b823842…` | The clobbered state, kept as evidence. |

Both backups already contain `uuid.location`, `pciBridge0.pciSlotNumber` and
`ethernet0.pciSlotNumber`, and **neither contains `guestInfo.detailed.data`** —
consistent with both predating the completed install.

### What the clobber actually changed (clobbered → live, non-comment keys)

```
cleanShutdown            TRUE             -> FALSE      (power-state marker, not damage)
ethernet0.virtualDev     e1000e           -> vmxnet3
guestOS                  darwin23-64      -> darwin24-64
sata0:2.fileName         ..\...\darwinPre15.iso -> C:\Program Files (x86)\...\darwin.iso
guestInfo.detailed.data  (absent)         -> present (24G830 / 15.7.9)
```

The clobber had reverted the guest OS string, the NIC model, and the tools ISO —
three deliberate corrections. **All three are correct in the live file. The
clobber is repaired; this table is a historical record, not an open issue.**

### Pre-vmxnet3 → live

```
ethernet0.virtualDev     e1000e   -> vmxnet3
sata0:2.fileName         relative -> absolute path
sata0:2.startConnected   FALSE    -> TRUE
guestInfo.detailed.data  (absent) -> present
```

---

## Path resolution caveat

Relative paths in this VMX (`..\sonoma\BaseSystem.vmdk`) resolve to the parent
directory and back into the same directory. They work — but only because Windows
treats `Sonoma` and `sonoma` as the same directory. **Do not "simplify" them to
bare filenames without testing;** the case collision is what makes the current
form work. The `sata0:2` ISO path has already been converted to an absolute path,
which is why it is immune to this.

---

## Checklist for the next person touching this file

1. **Hash first.** If the md5 is not `e20b7667bea364e33c04cffb41976839`, something
   changed since 2026-08-10 — find out what before editing.
2. **Diff only `L1`–`L195`.** The boundary is unlabelled: authored ends at
   `uuid.bios` (`L195`), VMware's tail begins at `uuid.location` (`L196`).
3. **Never add a `pciSlotNumber` above `L196`.** All 9 existing ones are VMware's.
4. **Never use file size as an integrity signal.** It grows on power-on.
5. **Read `guestInfo.detailed.data` (`L232`) for the guest OS**, not the comment
   block at `L6`–`L14` — that block is about the installer media.
6. `cleanShutdown = FALSE` is a power-state marker, not a fault.
