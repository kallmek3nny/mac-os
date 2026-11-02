# QEMU on a full disk — what was measured, and what it buys

**Filed 2026-08-14.** The desktop's `C:` was at 95% and the laptop has ~40 GB
free while dual-booting Windows and Linux. This file records what the macOS
guest actually costs, what the same guest costs under QEMU, and the one
constraint that decides whether the QEMU route can serve the *driver* goal at
all. Every number below came off this machine on the filing date.

---

## 1. Where the space goes

| Artifact | Size | Notes |
|---|---:|---|
| `Sonoma-s001..s026.vmdk` | 33 768 MB | 26 split-sparse extents; 9 hold data, 17 are ~1 MB stubs |
| `564d3ffb-….vmem` | 8 192 MB | exists **only** while the VM is powered on or suspended |
| `BaseSystem.img` | 3 072 MB | the installer volume, `sata0:0` |
| `BaseSystem.dmg` | 844 MB | the source the `.img` was made from |
| **VM directory total, powered off** | **37 909 MB** | ~46 GB with the memory file live |

Against that, what the guest itself says it uses — measured over SSH while it
was running:

```
/dev/disk2s4s1   100Gi    10Gi    80Gi    12%   /
Volume Used Space:  11.3 GB (11262066688 Bytes)
Container Total Space: 107.2 GB
```

**22 506 MB of the VMDK is blocks APFS no longer uses.** VMware's sparse extents
grow and never shrink, and this guest is attached over `sata0` with no discard
path, so nothing ever hands those blocks back.

## 2. What the conversion produced

```
qemu-img convert -p -O qcow2 -c Sonoma.vmdk F:/nv3050-qemu/sonoma-base.qcow2
```

| | |
|---|---|
| convert exit status | 0 |
| result | 26.7 GiB (28 630 122 496 bytes), `compat 1.1`, zlib |
| virtual size | 100 GiB — unchanged, so the guest sees the same disk |
| `qemu-img check` | **No errors**, 32.46% allocated, exit 0 |
| protective MBR | `0x55aa` at offset 510 |
| GPT header at LBA 1 | `EFI PART` |
| QEMU install footprint | 1 207 MB (`C:\Program Files\qemu`, chocolatey `qemu 2026.8.11`, QEMU 11.1.0) |

The MBR/GPT check matters: it is the difference between "a 26.7 GiB file exists"
and "the boot structure survived the format change". `qemu-img check` reading
clean over all 26.7 GiB is the same kind of claim one level down.

**Net on `C:`:** dropping the VMware tree entirely reclaims ~37.9 GB powered
off, ~46 GB counting the memory file. The replacement lives on `F:` (150 GB
free) and costs 26.7 GiB there plus 1.2 GB of QEMU on `C:`.

## 3. The constraint that decides the driver question

The reason to want QEMU here was never disk space — it was that a synthetic
`10de:25a2` PCI device would let macOS build a real `IOPCIDevice` nub and
actually *match* `NV3050Probe` against the `IOPCIMatch` already in
`phase0-kext/Info.plist`, with the kext unmodified.

**Stock QEMU on Windows cannot do that.** Measured by dumping every PCI device's
property list and sweeping it:

| Probe | Result |
|---|---|
| PCI devices enumerated | 128 |
| devices exposing a settable `vendor-id` | **0** |
| ID-ish properties found anywhere | 3 — `e1000e`'s `subsys`/`subsys_ven`, `ati-vga`'s `x-device-id` |
| `vfio-pci` in the Windows binary | absent (0 matches in `-device help`) |
| `ivshmem` | absent |

The sweep carries its own positive control, because a sweep that finds nothing
has to prove it can find something: the same grep over the same dump matched
`addr=` 135 times, `multifunction` 128 times and `romfile` 128 times. So the
zero is a real absence, not a pattern that never fired — the failure mode this
project has hit repeatedly (see the memory note on forbid-greps matching
nothing).

`pci-testdev` and `edu` both have hard-coded IDs; QMP `query-pci` against a
live `q35` confirms what a guest would see:

```
slot 01.0 vendor=0x1234 device=0x11e8   BAR0 memory size=0x100000
slot 02.0 vendor=0x1b36 device=0x0005   BAR0 memory 0x1000, BAR1 io 0x100
```

Neither is `10de:25a2`, and neither ID can be changed from the command line.

**What follows from that:** the synthetic-device route needs a QEMU that can
present arbitrary IDs — either a patched/rebuilt `qemu-system-x86_64`, or
`vfio-pci` on a Linux host passing through the laptop's real GA107. Both are
real options; neither is "install QEMU and add a flag". Until one exists, QEMU
buys **storage**, not a match.

## 4. The other blocker on the boot path

`isa-applesmc` is present in this binary, with exactly two properties:

```
iobase=<uint32>   (default: 768)
osk=<str>
```

QEMU ships **no default OSK**, and Apple's `AppleSMC` kext validates that key.
So an unattended `--probe-seconds` run without `--osk` is expected to stall in
firmware or panic early. `tools/qemu-boot-sonoma.py` prints that expectation
before it launches, so the resulting failure reads as *the missing key* rather
than as a broken image.

VMware boots this guest without one because `smc.present = "TRUE"` plus the
unlocked `vmware-vmx` supplies it internally. **That internal copy is readable
on this host**, which turns the blocker from a hypothesis into a measurement.
Sweeping the installed 21 209 456-byte `vmware-vmx` binary on 2026-08-14:

| Probe | Result |
|---|---|
| first fragment of the key (23 bytes) | 2 occurrences, `0xd1d098` and `0xd24b58` |
| tail fragment (19 bytes) | 2 occurrences, at +85 from each |
| MIDDLE fragment (22 bytes) | **0 occurrences** |
| controls (`smc.present`, `AppleSMC`, `darwin`, ...) | 6 of 6 fired |

The zero in the middle is the informative row, not a partial hit: NUL and
non-ASCII bytes sit between the halves, so the key is stored as **two 32-byte
ASCII runs** rather than one 64-byte run. A probe expecting one contiguous
string reports "absent" against a binary that plainly has it. Both occurrences
are byte-identical, so reassembly involves no choosing between candidates, and
the controls mean an absence would have meant absence.

`tools/vmware-osk.py` does that reassembly at **runtime only**. The key is
Apple's, read out of software installed on this machine, so committing it would
turn a local measurement into redistribution -- it lives nowhere in this tree,
not in a constant, not in a fixture, not in this sentence. Bare invocation
reports presence, length and a SHA-256 digest and nothing else; `--print` is
opt-in so 64 bytes of Apple's string never land in a terminal transcript by
accident:

```sh
python tools/vmware-osk.py                 # presence, length, digest
python tools/qemu-boot-sonoma.py --probe-seconds 90 \
    --osk "$(python tools/vmware-osk.py --print)"
```

What this does **not** do is move §5. The boot fails before firmware ever asks
the SMC anything, so a recovered key changes nothing until edk2 can read APFS.
It is a blocker retired in advance, not a blocker retired.

## 5. Why the converted image does not boot under stock edk2

This was measured on the filing date, and the answer is not the OSK.

The framebuffer was the wrong instrument. A 1280x800 console dump is downscaled
below the legibility of an 8x16 font by anything that views it, so an EFI shell
full of error text and a blank screen decode to the same unreadable smear.
`tools/qemu-boot-sonoma.py` now adds `-serial file:...` (it has to be added back
explicitly, because `-nodefaults` removes it), and edk2 mirrors its console
there. That channel yields characters:

```
BdsDxe: failed to load Boot0001 "UEFI QEMU HARDDISK QM00013 "
        from PciRoot(0x0)/Pci(0x1,0x0)/Sata(0x0,0xFFFF,0x0): Not Found
BdsDxe: loading Boot0002 "EFI Internal Shell"
```

So the disk was found and no boot image on it was. Reading the partitions
explains why.

| Probe | Result |
|---|---|
| GPT #1 | LBA 40-409639, 200.0 MiB, `c12a7328-...`, EFI System Partition |
| GPT #2 | LBA 409640-209715159, 102200.0 MiB, `7c3457ef-...` (Apple APFS) |
| ESP filesystem | FAT32, OEM `BSD  4.4`, boot signature `0xaa55` |
| ESP root directory slots | **1** |
| that slot | `attr=0x28` (VOLID\|ARCH), size 0 -- a volume label |
| a top-level EFI directory | absent |
| `BOOTX64.EFI` | absent |

The ESP is empty. The same walk against the **source** `Sonoma.vmdk`, which
VMware still boots today, returns the identical single VOLID slot -- so this is
the guest's real state, not conversion damage.

Where the bootloader actually lives is settled by naming the APFS volumes.
Both images resolve to the same six, at the same physical blocks:

| oid | block | volume | role | files |
|---:|---:|---|---|---:|
| 1026 | 381918 | `Macintosh HD - Data` | DATA | 92649 |
| 1376 | 401897 | `Preboot` | PREBOOT | 656 |
| 1378 | 382902 | `Recovery` | RECOVERY | 193 |
| 1421 | 386102 | `Macintosh HD` | SYSTEM | 256489 |
| 1423 | 386501 | `Update` | `0x00C0` | 10 |
| 1427 | 385080 | `VM` | VM | 1 |

**On APFS, `boot.efi` lives in the Preboot volume, not on the ESP.** Apple's
firmware reads APFS natively. Stock edk2 has no APFS driver, so it cannot see
Preboot, and it correctly reports `Not Found` on a perfectly intact disk. The
AppleSMC OSK from §4 is a *later* blocker on the same path -- the boot never gets
far enough to reach it.

That reframes the fix. It is not "repair the image": the image is fine, verified
twice by two independent readers. It is "give the firmware a way in" -- an APFS
filesystem driver loaded by OVMF, or a bootloader placed on the ESP. Neither has
been done and neither is claimed here.

### The measurement tools this required

`qemu-img dd` is not usable as a random-access reader on either image. It served
`skip=40` on the qcow2, then refused `skip=409640` on that same file, and refused
`skip=40` on `Sonoma.vmdk`, every time with `cannot skip to specified offset`.
In all three failures **the exit status arriving in the shell was 0 and the output
file was 0 bytes** -- the diagnosis goes to stderr only. A command that reports
success and produces nothing is this project's recurring failure shape, so the
reader was replaced rather than worked around:

- `tools/qcow2-read.py` -- qcow2 including the zlib-compressed clusters `-c`
  produces. Cross-checked against qemu-img: the ESP boot sector it returns is
  byte-identical, and it reproduces `0x55aa` at LBA 0 +510 and `EFI PART` at LBA 1.
- `tools/vmdk-read.py` -- 26-extent `twoGbMaxExtentSparse`, resolving extent then
  grain table then grain. Same two controls pass.
- `tools/apfs-volumes.py` -- names the volumes by walking the container object
  map.

Two bugs in that last one are worth recording, because both produced confident
wrong answers rather than errors:

1. **A linear scan for the `APSB` magic found zero across 512 MiB** while
   counting 141 `NXSB` blocks in the same sweep. The positive control fired, so
   the offset was right and the *method* was wrong -- APFS names volumes by
   virtual object id and resolves them through a B-tree, so a magic scan can
   return a confident zero on a container full of volumes.
2. **Block 0's NXSB is a copy, and it lags.** It carries xid 12589 and
   `nx_omap_oid` 380165; the newest superblock in the checkpoint descriptor area
   carries xid 15491 and `nx_omap_oid` 382546, 2902 transactions newer. Following
   the stale pointer landed on a block whose `o_type` is `0x40000003` -- a
   BLOCKREFTREE node, not `0x4000000b` (OMAP) -- and parsing it as an
   `omap_phys` yielded `om_tree_oid = 78819521836614232`, a block number larger
   than the disk. Nothing raised; the only tell was an absurd magnitude. Had the
   misread bytes decoded to a plausible block number, it would have walked a
   wrong tree and reported nonsense. The reader now scans for the newest
   checkpoint and **checks `o_type` against the type it is parsing as**.

A third near-miss: the volume name offset was first guessed at `0x2C0 + 0x10` and
returned `'ata'` with 24272 files and role `0x0003`. A fragment, not a failure --
`'ata'` reads as a strange-but-real volume name. The correct offset was derived
from the block's own printable runs (`0x110`, `0x140`, `0x170` at 48-byte spacing
is `apfs_modified_by_t`, and `0x2C0` held `Macintosh HD - Data` directly).

## 6. Laptop plan, under 40 GB

The laptop is the only machine with a GA107, so it is the only one where a
load attempt could ever mean anything. Numbers, from the same measurements:

| Item | Cost |
|---|---|
| the converted qcow2 | 26.7 GiB |
| QEMU (Linux side, distro package) | ~0.4 GB |
| **total** | **~27 GB of ~40 GB free** |

That fits, but with little room, and the 26.7 GiB figure still contains the
22.5 GB of untrimmed slack from §1. Trimming the guest *before* converting is
the lever worth pulling — reclaiming even half of that slack would put the
image near 15 GB. That has not been done and is not claimed here.

**Host-side reclaim, measured: 0 bytes.** The slack cannot be recovered from the
host without booting the guest. A read-only census
(`tools/qcow2_zero_census.py`, output in `qcow2-census-sonoma-base.log`) decoded
every cluster `sonoma-base.qcow2` stores:

| | |
|---|---|
| guest clusters (64 KiB) | 1 638 400 |
| unallocated (already sparse) | 1 106 602 = 72 522 268 672 bytes |
| allocated | 531 798 (171 144 standard, 360 654 compressed) = 34 851 913 728 bytes of guest data |
| allocated clusters that decode to all zeros | **0** |
| decode errors | 0 |
| all-zero 4 KiB blocks inside non-zero clusters | 81 118 = 332 259 328 bytes; a 64 KiB-cluster qcow2 cannot drop these |
| host bytes in decoded clusters / outside them | 28 567 438 677 / 62 683 819 (metadata, padding) |

A zero-detecting rewrite can only drop clusters that decode to all zeros. There
are none, so its ceiling is 0 bytes, and even a smaller-cluster target could gain
at most 0.31 GiB of guest data. The §1 slack sits inside allocated clusters as
non-zero stale data, not zeros. Whether the original `-c` conversion applied
zero detection does not matter: no all-zero cluster is left for any pass to find.

The candidate `F:/nv3050-qemu/trim-cand-zerodetect.qcow2` agrees. It is
28 630 122 496 bytes, the same as the source, with identical `qemu-img map`
totals (34 851 913 728 data, 72 522 268 672 zero, 0 unallocated), and
`qemu-img check` finds no errors. How it was made is not on record, so the
census is the evidence, not that file.

The only remaining lever is inside the guest: zero the freed blocks, or give the
guest a discard path, and then convert again. That needs a booted guest, and only
the VMware copy boots (§8). It has not been done, so the laptop figure stays at
~27 GB.

## 7. How to use what exists now

```sh
# prove the inputs resolve and the command composes; launches nothing
python tools/qemu-boot-sonoma.py --dry-run

# headless boot attempt: framebuffer dump AND the serial console as text
python tools/qemu-boot-sonoma.py --probe-seconds 90 \
    --screenshot qemu-results/boot-probe.png \
    --serial-log qemu-results/boot-probe-serial.log

# interactive window
python tools/qemu-boot-sonoma.py
```

**Read the serial log first.** A PNG on disk proves a *framebuffer*, never a
booted macOS, and at 1280x800 the console text in it is downscaled below
legibility by anything that views it — so a screen full of firmware errors and a
blank screen look identical. The serial log is the channel that yields
characters. §5 was diagnosed from one line of it after several rounds of trying
to read the pixels.

Once a dump is worth measuring, `tools/png-crop.py` decodes it with all five PNG
filter types (QEMU's encoder uses four of them, so a raw-scanline read reports a
different image), prints the per-band non-black counts, and can cut a row/column
region out at an integer upscale.

To read the disks themselves, without booting anything:

```sh
python tools/qcow2-read.py F:/nv3050-qemu/sonoma-base.qcow2 --info
python tools/apfs-volumes.py --qcow2 F:/nv3050-qemu/sonoma-base.qcow2 --lba 409640
```

Exit codes follow this project's convention: **2** for a malformed invocation,
**1** for a missing required input (reported as a full list, not one per run),
**0** only when the requested action completed.

## 8. What is NOT claimed

- **Measured false, not merely unclaimed: the qcow2 does not boot under stock
  edk2.** §5 has the serial line and the cause. The earlier draft of this section
  said "not claimed that it boots -- that is what the probe measures"; the probe
  has now run, so leaving that sentence standing would read as an open question
  the evidence has already closed.
- Not claimed: that the image is *broken*. The opposite is measured twice by two
  independent readers — protective MBR, GPT, FAT32 ESP and a 99.80 GiB APFS
  container with all six volumes resolving to the same physical blocks in the
  converted image and in the VMware source. The obstacle is in the firmware's
  filesystem support, not the disk.
- **Superseded by §9: an APFS driver was added, and the boot got further and still
  failed.** The earlier text here read "not claimed that adding an APFS driver or
  an ESP bootloader would make it boot -- that is the indicated next step, not a
  result". The driver has now been loaded and `boot.efi` has been launched, so
  that sentence would read as an open question the evidence has closed. The OSK
  of §4 is still untested: the failure is in the booter, before the kernel.
- Not claimed: that QEMU can present `10de:25a2`. Measured false for the stock
  Windows binary, §3.
- Not claimed: that the VMware tree is safe to delete. **It is now measurably the
  wrong move**: the VMware copy is the only guest anything has booted, and the
  QEMU copy is known not to boot as it stands. Delete it after a successful QEMU
  boot, not before.
- Not claimed: that any of this moves `PLAN.md`'s termination conditions. Those
  are about pixels on a panel. This is about whether the kext can ever be
  *loaded and matched*, which is a separate and strictly smaller question.

## 9. The APFS driver loaded, boot.efi ran, and the boot stops inside Apple's booter

**Filed 2026-08-15.** §5 named the indicated next step as "give the firmware a way
in". That has now been done, non-destructively, and the boot got measurably
further and still failed. The failure moved out of edk2 and into Apple's own
booter. Every number and every quoted line below came off a serial log in
`qemu-results/` on the filing date.

### 9.1 The delivery channel, and why nothing was written to the image

The driver is handed to the firmware through QEMU's vvfat driver, which
synthesises a FAT volume out of a host directory. The macOS qcow2 is never
opened for write, so a failed experiment costs one rerun rather than a 26.7 GiB
reconversion, and the VMware source stays the only thing anything has booted.

Three spellings protect the host directory and only one of them attaches:

| Spelling | Result |
|---|---|
| `readonly=on` + `ide-hd` | dies at device setup, `Block node is read-only`, qemu exits BEFORE firmware runs -- 0-byte serial log |
| `fat:rw:` | attaches, but vvfat's write path is documented as able to corrupt the host directory it serves |
| `snapshot=on` | attaches; qemu layers a temporary overlay, every write lands in a temp file discarded at exit |

`snapshot=on` is what `tools/qemu-boot-sonoma.py --vvfat` uses. An `ide-hd` is a
hard disk and qemu will not build one over a read-only node, which is why the
first attempt produced no log at all rather than a boot failure.

### 9.2 What the driver is, established by parsing it rather than by its name

| Probe | Result |
|---|---|
| size | 713 784 bytes |
| first two bytes / PE signature | `MZ` / `PE\0\0` |
| machine | `0x8664` (x86_64) |
| optional header magic | `0x020b` (PE32+) |
| subsystem | **11 -- `EFI_BOOT_SERVICE_DRIVER`** |
| provenance strings | `com.apple.xbs/Sources/apfs_efi_osx/authapfs/...`, `APFSLCKDH9`, `APFSVOL H` |

Subsystem 11 is the row that matters: it is a filesystem *driver*, not an
application, so `load` is the correct verb for it. This is Apple's own APFS EFI
driver -- exactly the thing §5 names as missing from stock edk2.

### 9.3 A single BEL byte, and why only the serial log could see it

The first run reported `LOAD-RETURNED` and loaded nothing. The `.nsh` script had
been authored through a Bash heredoc, which ate the `\a` of the driver's path and
left a literal BEL (`0x07`) at byte `0x14A` -- so the shell was handed a volume
prefix followed by an unprintable, and the load could never have resolved. It is
invisible in a framebuffer dump and invisible in a terminal that renders control
bytes as nothing.

Every `.nsh` in this experiment is now written by a tool that does not interpret
escapes, and each one is checked before a probe is spent on it: byte count, zero
BEL, and CR count equal to LF count so every line ending is CRLF.

### 9.4 What boot.efi actually does

With the driver loaded from the vvfat volume and `connect -r` run, six new
filesystem handles appear. Preboot and the System volume both resolve, and the
booter is at the volume group's `CoreServices` directory inside Preboot -- 723 512
bytes, `boot.efi 583~2317 (Official), built 2025-07-20`.

It **launches and returns to the shell**:

```
#[EB.CFG.DEV|!] Err(0xE) <- RT.GV booter-strict-xmlparser 7C436110-AB2A-4BBB-A880-FE41995C9F82
#[EB|GFWF!] Err(0xE) <- 5
#[EB|GFWF!] Err(0xE) <- 33
#[EB|GFWF!] Err(0xE) <- 23
           Err(0xE) <- EB.G.ICM
#[EB|WL] 0 0 0x01 0x03  41 0x0E
#[EB|STOP] 0x1
```

`0xE` is `EFI_NOT_FOUND`. The `EB.G.ICM` line is reported through `EB.B.MN` in the
first run and through `EB.ERR.SBF` in later ones; the origin token and the status
are identical in both, which is why the prefix is left out of the quote above.

The **System volume's own copy of boot.efi fails identically**. That localises the
fault away from the Preboot copy, and it is the reason no effort has gone into
re-blessing or repairing that tree.

What `EB.G.ICM` expands to is **not known**. It is an abbreviation in Apple's log
with no public expansion, and every guess at it below is labelled as one.

### 9.5 Objects that exist, so are not the missing thing at the obvious path

| Object | State |
|---|---|
| `BootKernelExtensions.kc` in Preboot's kernel-collections directory | **present**, 66 125 824 bytes |
| the OS cryptex disk image under Preboot's `cryptex1` tree | **present**, 2 155 872 256 bytes, beside an app image, a build manifest, 19 apticket `.im4m` files and both root-hash files |
| the 36-byte root-uuid file beside boot.efi | **present** |
| the CoreStorage wipekey plist under Preboot's caches | **present** |
| the pre-Big-Sur prelinked-kernel directory | **EMPTY**, 0 files -- genuinely absent, but Sonoma does not use it |

The cryptex tree was dumped because "ICM names a cryptex manifest" was a plausible
reading of the abbreviation. It is populated, so that reading is not supported by
an absence -- the guess neither confirmed nor usefully failed.

One unattributed diagnostic from the driver, once per run, during a directory
listing: `FileOpen:2688: EFI cannot open compressed files.` followed by
`FileOpen:2735: Status 800000000000000f` (`0x0F` = `EFI_ACCESS_DENIED`). **Which
file that was has never been established.** Later scripts fence each listing
between markers so a future occurrence lands between two known labels instead of
floating loose in 40 KB of log.

### 9.6 Three configuration channels, each refuted with a within-run control

Every experiment below runs a **bare launch first, in the same boot**, so a null
cannot be confused with a run that differed for some other reason. The comparison
is over the booter's own `#[EB...]` lines, timestamp-normalised.

| Channel | How it was proven delivered | Result |
|---|---|---|
| shell command-line arguments | -- | 30 EB lines, **byte-identical** to bare; the booter's own config table still printed `default` for every row |
| NVRAM `boot-args` under the Apple vendor GUID | `dmpstore` before/after: before held only `efi-boot-failure-reason-ca`, after held both written variables with correct ASCII bytes | 30 EB lines, **byte-identical** to bare |
| NVRAM `log-level`, three encodings | `dmpstore` after each write printed the stored bytes | all launches **timestamp-normalised identical**; the only differing lines were `LOG:INIT` and `LOG:EXIT` |

The `log-level` row was re-run on 2026-08-15 with both encodings that the shell
actually accepts, and the result stands -- but the run is worth reading for how
it stands rather than for the null itself. `dmpstore` before any write in that
boot printed `log-level` already present at eight bytes
`21 00 00 00 00 00 00 00`, left there by the previous run: `OVMF_VARS.fd`
persists, and the script's four `setvar ... -guid G` clears displayed those
variables instead of deleting them (§9.8). So the arm labelled BARE was running
the payload the third arm writes, and the three arms were never three arms.

What survives that is still a refutation, because the arms differ in the right
place by the store's own dump rather than by intent: the second arm's `dmpstore`
read back `DataSize = 0x01` / `21`, the third `DataSize = 0x08` /
`21 00 00 00 00 00 00 00`. One byte and eight bytes are not the same value, and
all three launches printed the identical config row --
`#[EB|CFG:ARG] log-level 0x0000000000000001 (0x0000000000000001 & 0x0000000000000021) default`
at log lines 164, 246 and 331. A row that reads `default` while the variable
demonstrably holds `0x21` is the booter declining to consult the name, not a
write that failed to land.

The same boot also settles which names this booter asks for at all:
`booter-strict-xmlparser` is the **only** one. Grepping every `RT.GV` and
`CFG:DEV` line across all three launches returns three occurrences, all of that
one name. `log-level` never appears on a `RT.GV` line, which is the mechanism
behind its null and is a stronger statement than "it changed nothing".

So the configuration channel is not the lever. The failing *operation* is.

### 9.7 The one positive result, which overturned how the first null was read

The bare launch logs its own failed variable read:

```
#[EB.CFG.DEV|!] Err(0xE) <- RT.GV booter-strict-xmlparser 7C436110-...
#[EB|CFG:DEV] r5 0x0 0x0
```

`RT.GV` is `GetVariable` at runtime, and the name it asks for is
`booter-strict-xmlparser` -- an **individually named variable**, not a token inside
`boot-args`. Writing that exact name as one ASCII byte changed the log:

```
#[EB.CFG.DEV|VAR] booter-strict-xmlparser 7C436110-... 48
#[EB|CFG:DEV] r5 0x0 0x2
```

**NVRAM is read.** The `boot-args` null earlier in §9.6 means this booter never
*asks* for `boot-args`, not that it ignores NVRAM -- a different conclusion from
the one that null first looked like, and the reason that row is stated as "the
variable is not read" rather than "the channel is dead".

The printed `48` also settles the **encoding**: the variable held one byte `0x30`,
and the booter printed 48, which is that byte's value in decimal. It takes the
value of the byte, not the digit the byte spells -- so `="0x21"` asks for 48, which
is why a `log-level` write that demonstrably landed still left the row reading
`default`. Retried as a single byte `0x21` and as a contiguous little-endian
UInt64, `log-level` still changed nothing, so the extra verbosity bit its own mask
advertises is either not honoured or not reached before the failure.

### 9.8 Three ways a run measured nothing and printed a clean result

Each was caught by a control, not by an error:

- `setvar NAME -guid G` with no value **displays** the variable; it does not delete
  it. The "cleared" dump printed every variable still present, so a run intended
  to start from a clean NVRAM store started from the previous run's tail state.
- `=21 00 00 00 00 00 00 00` is rejected with `setvar: Invalid argument - '00'` --
  the spaces make each pair a separate argument. That write never happened, so
  that run's third launch silently repeated its second, and the pair compared
  equal for a reason that had nothing to do with the booter.
- A **within-run bare control is only bare if the clear worked**, and on this
  firmware the clear never does. Combined with the first bullet, the two compose
  into a control that reads as a control, prints as a control, and is the
  experiment: the 2026-08-15 `log-level` run opened with the previous run's
  eight-byte value already in the store, so its first launch was its third
  launch. Nothing errored. The only artifact that showed it was the `dmpstore`
  placed *before* the first write.

The second one is the more dangerous shape: a run that measures nothing while
printing a clean, plausible, *equal* comparison. The third is the same shape
aimed at the control rather than at the treatment, which is worse, because a
contaminated control silently converts every null in the run into a tautology.

### 9.9 What is NOT claimed in §9

- Not claimed: that the guest boots. It does not. `boot.efi` returns to the EFI
  shell with `EFI_NOT_FOUND` every time.
- Not claimed: that `EB.G.ICM` is understood. It is an unexpanded abbreviation.
  The cryptex reading was tested and neither confirmed nor refuted.
- Not claimed: that the AppleSMC OSK of §4 works. **It is still untested** -- the
  failure is in the booter, before the kernel exists to validate a key. §4 was a
  blocker retired in advance and it is still that.
- Not claimed: that any of this required or produced a change to the macOS image.
  Nothing was written to it. The VMware tree remains the only guest anything has
  booted and remains the wrong thing to delete.
- Not claimed: that this moves `PLAN.md`. It does not; see §8.
