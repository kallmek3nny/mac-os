# Intel Gen12 device IDs — which iGPU is actually in this machine

The Intel analogue of `DEVICE-IDS.md`. Same job, different vendor: attribute PCI
device IDs to a die and an architecture, from a source that is not this project.

**Why this file exists.** `tools/check-kext-personalities.py` refuses to hold its
own accept set. Its `DerivationIsNotHardcoded` tests exist specifically to stop
the accept set becoming a second table nothing keeps true — the checker parses a
markdown table out of a document instead. `DEVICE-IDS.md` is that document for
NVIDIA, and it is NVIDIA-only: every row is a GeForce part and the vendor half is
always `0x10DE`. When the kext grew an Intel personality there was nowhere for the
Intel accept set to come from. This is that document.

It is a document, not evidence. The evidence is cited per row below.

---

## The table

| Marketing name | Device IDs | Die | Architecture |
|---|---|---|---|
| TigerLake-H GT1 (UHD Graphics) | `0x9A60`, `0x9A68`, `0x9A70` | TGL | Gen12 (Xe-LP) |
| TigerLake GT2 (Iris Xe Graphics) | `0x9A40`, `0x9A49`, `0x9A59`, `0x9A78`, `0x9AC0`, `0x9AC9`, `0x9AD9`, `0x9AF8` | TGL | Gen12 (Xe-LP) |

The die column is `TGL` for both rows rather than `TGL-GT1` and `TGL-GT2`, and
that is a deliberate choice with a consequence worth stating plainly.

GT1 and GT2 differ in **execution unit count** — the 3D and compute side. They do
not differ in the display block, which is the only part of the silicon this
project's Intel track touches: same transcoders, same pipes, same DDI registers,
same `PIPESRC`/`PLANE_SIZE` encoding. `phase0-kext/IntelGpuId.hpp` says the same
thing in code — it carries `igIsTglGt1()` and `igIsTglGt2()` for precision, but
its whole-generation predicate `igIsTglDevice()` accepts both, and that predicate
is what an IOKit `probe()` should be asking. A die column that split the two would
make the document disagree with the header for no gain.

---

## Where each half of the table came from

**Both rows: `reference/i915/i915_pciids.h`, vendored at Linux v6.12.** The GT1
row is `INTEL_TGL_GT1_IDS` at line 623–626; the GT2 row is `INTEL_TGL_GT2_IDS` at
line 628–636. That header is Intel's own contributed table in the kernel tree, so
it stands in the same relation to this file as NVIDIA's
`g_nv_name_released.h` does to `DEVICE-IDS.md`: a shipped vendor table rather than
a third-party guess. `reference/i915/PROVENANCE.md` records the fetch and the
byte counts.

**One ID in the GT1 row is also witnessed on the target machine.** `0x9A68` was
read twice, on two different days, by two different mechanisms:

| Reading | Source | What it says |
|---|---|---|
| 2026-08-15 | `HARDWARE.md`, from `lspci -nn` | `8086:9a68`, "TigerLake-H GT1 [UHD Graphics]", rev 01, at `00:02.0` |
| 2026-08-16 | `igpu-results/tgl-capture-2026-08-16-022556.txt` line 3 | `device   0x9a68`, `driver i915`, same BDF |

So the attribution of `0x9A68` specifically to TigerLake-H GT1 rests on three
independent statements that agree: Intel's kernel table, `lspci`'s own PCI ID
database lookup, and the sysfs read on the laptop. The other ten IDs in this
table rest on the vendored kernel header alone. No machine in this project has
ever reported any of them, and this file must not be read as claiming otherwise.

`phase0-kext/IntelGpuId.hpp` defines `IG_DEVICE_OBSERVED_2026_08_16` as `0x9A68`
separately from the GT1 table entry carrying the same value, and never derives one
from the other. That duplication is intentional for exactly the reason this
section is written out: two independent statements that agree are evidence, and
one statement quoted twice is not.

---

## What this file does not establish

The capture behind the 2026-08-16 row came from a single Linux boot of roughly an
hour on an eDP-only laptop, and there is no second run of it. It settles the PCI
identity of the part and nothing else. In particular it does not settle how many
display planes this die exposes, or anything about the DSI pages, because every
unused display page on this silicon reads `0x00000000` rather than all-ones —
read every `ABSENT` in that capture as `UNPROGRAMMED`, not as `does not exist`.

The device IDs here are also not a statement that the kext works on that
silicon. It has never been loaded on it.

---

## How to confirm on a different machine

```sh
lspci -nn | grep -i 'VGA\|Display'
```

You want the bracketed pair, e.g. `[8086:9a68]`. The first half is the vendor
(`0x8086` is Intel, a constant of the world); the second is the device ID to look
up in the table above. If it is not in the table, this project's Intel track does
not target your iGPU, and `phase0-kext/Info.plist` will correctly never match it.
