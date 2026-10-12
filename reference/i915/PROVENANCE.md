# `reference/i915/` — where these headers came from

Vendored 2026-08-15 from `torvalds/linux`, tag **v6.12**, unmodified. Fetched
over HTTPS from `raw.githubusercontent.com`; each file is byte-identical to the
upstream path below.

| File | Upstream path in v6.12 | Bytes |
|---|---|---|
| `i915_reg.h` | `drivers/gpu/drm/i915/i915_reg.h` | 192,954 |
| `i915_reg_defs.h` | `drivers/gpu/drm/i915/i915_reg_defs.h` | 10,488 |
| `i915_pciids.h` | `include/drm/intel/i915_pciids.h` | 28,456 |
| `skl_universal_plane_regs.h` | `drivers/gpu/drm/i915/display/skl_universal_plane_regs.h` | 22,190 |
| `intel_display_reg_defs.h` | `drivers/gpu/drm/i915/display/intel_display_reg_defs.h` | 1,996 |
| `intel_display_power.h` | `drivers/gpu/drm/i915/display/intel_display_power.h` | 9,502 |
| `intel_de.h` | `drivers/gpu/drm/i915/display/intel_de.h` | 6,503 |
| `intel_display_device.c` | `drivers/gpu/drm/i915/display/intel_display_device.c` | 49,183 |
| `intel_display.c` | `drivers/gpu/drm/i915/display/intel_display.c` | 250,964 |
| `intel_lrc_reg.h` | `drivers/gpu/drm/i915/gt/intel_lrc_reg.h` | 2,746 |
| `intel_engine_regs.h` | `drivers/gpu/drm/i915/gt/intel_engine_regs.h` | 12,482 |
| `intel_gpu_commands.h` | `drivers/gpu/drm/i915/gt/intel_gpu_commands.h` | 20,303 |
| `intel_gt_regs.h` | `drivers/gpu/drm/i915/gt/intel_gt_regs.h` | 65,408 |
| `intel_gtt.h` | `drivers/gpu/drm/i915/gt/intel_gtt.h` | 21,188 |
| `intel_ggtt.c` | `drivers/gpu/drm/i915/gt/intel_ggtt.c` | 42,578 |
| `intel_uncore.h` | `drivers/gpu/drm/i915/intel_uncore.h` | 16,830 |
| `intel_uncore.c` | `drivers/gpu/drm/i915/intel_uncore.c` | 90,545 |
| `intel_execlists_submission.c` | `drivers/gpu/drm/i915/gt/intel_execlists_submission.c` | 127,198 |
| `intel_lrc.c` | `drivers/gpu/drm/i915/gt/intel_lrc.c` | 46,383 |
| `intel_engine_cs.c` | `drivers/gpu/drm/i915/gt/intel_engine_cs.c` | 71,125 |
| `intel_lrc.h` | `drivers/gpu/drm/i915/gt/intel_lrc.h` | 4,046 |

## The three `gt/` headers, vendored 2026-08-26, and what they retire

The first nine files above are all DISPLAY. That was not an oversight at the
time and it became the single measurement shaping six sessions of GT work:
greps of this directory for `LRC`, `CTX_DESC`, `RING_BUFFER_START`,
`RING_BUFFER_CTL`, `BB_PER_CTX` and `CTX_TIMESTAMP` returned **ZERO matches**,
recorded in `intel-handoff35.md` §1 and re-measured in `intel-handoff36.md` §1.
i915 keeps the logical ring context layout under `drivers/gpu/drm/i915/gt/`,
which had never been fetched. Every number in `phase0-kext/IntelGtImage.hpp`
was therefore this project's own choice, spelled `POLICY` for that reason, and
`intel-handoff36.md` §1 is explicit that composing a context descriptor from
policy and writing it to a real execlist port is a guess submitted to silicon
rather than a bringup step.

These three close that gap. They are fetched the same way as the nine above —
HTTPS from `raw.githubusercontent.com`, tag `v6.12`, byte-identical, unmodified.
md5 at vendor time: `intel_lrc_reg.h` `c0e79db0541955adede031f9d088ce27`,
`intel_engine_regs.h` `973fcd212876db8419a5c7eb2bedc7ae`,
`intel_gpu_commands.h` `adba5434b0c0a53d24d1311c6e054d2b`.

**Read `intel_lrc_reg.h`'s slot numbering carefully, because the `+ 1` is the
whole point.** `CTX_RING_HEAD` is spelled `(0x04 + 1)`, and what that names is
the index of the **value** dword — so the OFFSET dword sits at slot `0x04` and
the pair occupies 4/5. Read that way the file gives HEAD at 4/5, TAIL at 6/7,
START at 8/9 and CTL at 10/11, which is **exactly the layout
`IntelGtImage.hpp` chose as policy**. The project's guess is corroborated by
upstream rather than corrected by it; that is a much better outcome than
agreement would have been if the layout had been read off these files in the
first place, because the two derivations are independent.

`CTX_CONTEXT_CONTROL (0x02 + 1)` is the other half. The slot 2/3 pair had no
citation in this tree at all, which is why `phase0-kext/IntelGtProbe.hpp` fills
it from a constant that was named `IG_GTP_FIRST_PAIR_UNCITED` until this
vendoring retired the name (it is `IG_GTP_FIRST_PAIR_I915_0x244` now), and why
`intel-handoff36.md` §6 item 5 calls it a hole in the design rather than a
solved problem. `intel_engine_regs.h` supplies the offset that belongs there —
`RING_CONTEXT_CONTROL(base)` at `(base) + 0x244` — which matches the value the
probe was already passing. Same shape as the ring block: guessed, then
corroborated.

`intel_gpu_commands.h` is vendored alongside them for `MI_LOAD_REGISTER_IMM`,
whose encoding `IntelGtCmd.hpp` composes from policy, and for `MI_NOOP` — the
zero word at slot 0 that the image writer emits.

The caveat below applies to these with more force than to the display files,
not less: no register in any of the three has been WITNESSED on this machine.
`igpu-results/` captured a render engine that was IDLE. A constant citable to
these headers is `I915`, never `OBSERVED`.

The two `.c` files above are `.c` rather than `.h`, vendored 2026-08-15 for two
facts that live only in code and cannot be read off a header:

- **Transcoder addressing on Gen12 is a per-platform TABLE, not arithmetic.**
  `i915_reg.h` defines `TRANS_HTOTAL()` in terms of `_MMIO_TRANS2`, which indexes
  `trans_offsets[]` — a field filled in per platform. `intel_display_device.c`'s
  `XE_D_DISPLAY` block is where Tiger Lake's actual values are:
  A `0x60000`, B `0x61000`, C `0x62000`, D `0x63000`. The uniform `0x1000`
  spacing is a property of that table, not a rule, and the same block shows
  Gen12 does NOT list `TRANSCODER_EDP` at all.
- **Every timing field is stored as value − 1.** `intel_display.c:2741-2743`
  writes `HACTIVE(crtc_hdisplay - 1) | HTOTAL(crtc_htotal - 1)`, and the readback
  at 2911 adds 1 back. `PIPESRC` at 2814 does the same with `width - 1` and
  `height - 1`. A decode that omits the `+1` is off by exactly one, which a mode
  still nearly works under — the reason it is worth pinning rather than assuming.

`TRANSCONF` is the trap those two files together expose: `i915_reg.h:1698`
defines it as `_MMIO_PIPE2`, so it lives in the PIPE block (`0x70008` + pipe
delta) while being indexed by the TRANSCODER. A single
`translate(base, offset)` helper would place it at `0x60008`, which is
`_TRANS_HSYNC_A` — a real register that reads and writes without faulting. This
is the Intel-side instance of the same hazard `nvGspFalconReg()` /
`nvGspRiscvReg()` are kept separate for.

Licence is SPDX `MIT` on the display files and `MIT` on `i915_reg.h`; the
headers carry their own notices and those are left intact.

## The eight hardware-path files, vendored 2026-10-12

The twelve files above can describe what the GT would execute; none of them
describes how to make it executable. Greps of this tree on 2026-10-12 for
`FORCEWAKE`, `GuC`, GGTT PTE writes and interrupt handling returned **zero**
matches in `phase0-kext/`, and `reference/i915/` held no file that defines a
forcewake register at all. These eight close that: power-well wake
(`intel_gt_regs.h`, `intel_uncore.[ch]`), the GGTT page-table format
(`intel_gtt.h`, `intel_ggtt.c`), and execlist submission
(`intel_execlists_submission.c`, `intel_lrc.c`, `intel_engine_cs.c`).

Same fetch as the rest: HTTPS from `raw.githubusercontent.com`, tag `v6.12`,
byte-identical, unmodified. md5 at vendor time:

- `intel_gt_regs.h` `abe340379c4cf30376e465701f01d38a`
- `intel_gtt.h` `22201b32bafdb1d5ddd4699aa91b4deb`
- `intel_ggtt.c` `5fd9e39eda585b6311d95de32aa81f04`
- `intel_uncore.h` `b222462944c10c102f0e0b4cb2c2bec8`
- `intel_uncore.c` `c288543091fdfdea71f27004a914070b`
- `intel_execlists_submission.c` `18d6a8e9be7c04a9c40450b406ca019d`
- `intel_lrc.c` `3750c37cc6cd8a5b62cce7aaea49fc2a`
- `intel_engine_cs.c` `7678f8a9805f8d474cfa0482314be84e`

First consumer: `phase0-kext/IntelGtForcewake.hpp`, whose register and bit
constants and whose 43-row copy of `__gen12_fw_ranges[]`
(`intel_uncore.c:1337-1436`) are re-derived from these files by
`tools/check-registers.py` (`intel_gt_forcewake_checks`).

The `.c` files are larger than anything vendored before, and they are here
for the PROTOCOL, not for constants: `intel_uncore.c:294-310`
(`fw_domains_get_normal`) is the acquire order this tree follows, and
`:203-248` is the fallback toggle it deliberately does not implement yet.

## `intel_lrc.h`, vendored 2026-10-12, and the two bugs it exposed

Same fetch, tag `v6.12`, byte-identical; fetched twice, md5 both times
`ceead4c4bb890316d7102dcb0008d53c`. Vendored because `intel_lrc.c:1517-1531`
(`lrc_descriptor`) uses the context-descriptor bit names but `intel_lrc.c` does
not define them -- `intel_lrc.h:90-114` does.

Checking `phase0-kext/IntelGtExec.hpp` against this file and `intel_gtt.h`
found two wrong values, both fixed the same day and both now pinned by
`tools/check-registers.py` (`intel_gt_exec_checks`):

- PTE address mask was bits 47:12; `intel_gtt.h:100` `GEN12_GGTT_PTE_ADDR_MASK`
  is `GENMASK_ULL(45, 12)`.
- the software context id sat at bit 32; Tiger Lake (IP 12.0, below 12.55)
  uses `intel_lrc.h:103` `GEN11_SW_CTX_ID_SHIFT` = 37, width 11.

Still NOT implemented and recorded as open (intel-handoff44): `lrc_descriptor`
also sets `GEN8_CTX_PRIVILEGE` (bit 8) and an addressing mode at bit 3; our
descriptor sets neither.

## Why this directory exists

The same reason `reference/open-gpu-kernel-modules/` exists. This project's one
standing rule about constants is that a value invented to make a test pass,
filed next to a value witnessed on hardware, destroys the only evidence trail
there is. For the NVIDIA half, `tools/check-registers.py` enforces that by
diffing the kext headers against NVIDIA's own shipped definitions. The Intel
half needs the same anchor, and Intel's `i915` headers are the authoritative
public source for Gen12 display register offsets.

## The one fact already settled from these files

`i915_pciids.h:623-626` defines `INTEL_TGL_GT1_IDS` as `0x9A60`, **`0x9A68`**,
`0x9A70`. The laptop's iGPU is `8086:9a68` (`HARDWARE.md`), so it is Tiger Lake
**GT1** — from Intel's own ID table rather than inferred from the CPU model.
That agrees with i915's independent runtime identification in `dmesg`:
`Found tigerlake (device ID 9a68) integrated display version 12.00 stepping D0`.

Two independent sources, same answer. That is the standard the rest of the
Intel constants have to meet.

## A caveat that will matter later

These are *Linux's* register definitions, not Intel's Programmer's Reference
Manual. They are extremely well tested — they drive the panel on this exact
laptop right now — but they are one project's reading of the hardware. Where a
value can also be checked against a live register dump
(`tools/read-igpu-mmio.py`), check it. A constant confirmed by both a header
and an observed value from the running display engine is worth far more than
either alone.

Upstream moves. Anything derived from these files should name the file and the
symbol, so a later re-vendor can be diffed rather than re-read.
