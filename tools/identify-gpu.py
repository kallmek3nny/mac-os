#!/usr/bin/env python3
"""
identify-gpu.py -- resolve an NVIDIA PCI device ID to its die and architecture.

Why this exists: "RTX 3050 Laptop GPU" is a marketing name covering GA107,
GA106, and AD107 across two architectures. Which one you have decides which
register headers and which GSP firmware the driver work targets, so it is not
something to guess at. See ../DEVICE-IDS.md.

Data source is NVIDIA's own shipped table:
  reference/open-gpu-kernel-modules/src/nvidia/generated/g_nv_name_released.h

Usage:
    python identify-gpu.py 25a2
    python identify-gpu.py 10de:25a2
    python identify-gpu.py "$(lspci -nn | grep -i nvidia)"
    python identify-gpu.py --list-3050

Runs on Windows, macOS, or Linux. No dependencies.
"""

import argparse
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Die assignment by device ID range.
#
# NVIDIA's released-name table gives marketing names, not dies, so the die has
# to come from the device ID. Ranges below are from the RM chip HAL tables and
# the published register header directories.
# ---------------------------------------------------------------------------

ARCHITECTURES = {
    0x16: ("Turing", "turing"),
    0x17: ("Ampere", "ampere"),
    0x18: ("Hopper", "hopper"),
    0x19: ("Ada Lovelace", "ada"),
    0x1A: ("Blackwell", "blackwell"),
    0x1B: ("Blackwell 2", "blackwell"),
    0x1C: ("Rubin", None),
    0x1F: ("AMODEL (simulator)", None),
}

# (device_id_low, device_id_high, die, arch_id, impl_id, header_dir)
#
# Ranges verified against g_nv_name_released.h in this repo. Notable checks:
#   0x2507/0x2508/0x2582/0x2584  desktop RTX 3050  -> GA106 / GA107
#   0x2523/0x2563                RTX 3050 Ti Lap.  -> GA106
#   0x25A0-0x25EC                RTX 3050 Laptop   -> GA107
#   0x2822/0x28A3/0x28E3         "RTX 3050 A"      -> AD107 (Ada, not Ampere)
DIE_RANGES = [
    (0x1E00, 0x1E7F, "TU102", 0x16, 0x2, "turing/tu102"),
    (0x1F00, 0x1F7F, "TU106", 0x16, 0x6, "turing/tu102"),
    (0x1F80, 0x1FFF, "TU117", 0x16, 0x7, "turing/tu102"),
    # 0x2200-0x223F is GA102. Includes the A10/A10G datacenter parts
    # (0x2237), which matters because A10G ships a vGPU profile literally
    # named "GeForce RTX 3050" -- see the note in list_3050s().
    (0x2200, 0x223F, "GA102", 0x17, 0x2, "ampere/ga102"),
    (0x2320, 0x237F, "GA103", 0x17, 0x3, "ampere/ga102"),
    (0x2400, 0x24FF, "GA104", 0x17, 0x4, "ampere/ga102"),
    # 0x2500-0x257F is GA106. The desktop RTX 3050 8GB (0x2507/0x2508) and the
    # RTX 3050 Ti Laptop parts (0x2523/0x2563) all live here -- a GA106 die
    # cut down and sold under a 3050 name.
    (0x2500, 0x257F, "GA106", 0x17, 0x6, "ampere/ga102"),
    # 0x2580-0x25FF is GA107. Includes the desktop 3050 6GB (0x2582/0x2584)
    # and the whole 3050/3050 Ti Laptop family (0x25A0-0x25EC).
    (0x2580, 0x25FF, "GA107", 0x17, 0x7, "ampere/ga102"),
    # 0x2600-0x26BF is AD102, including the L40 / L40S / L20 datacenter parts
    # (0x26B5 / 0x26B9 / 0x26BA).
    (0x2600, 0x26BF, "AD102", 0x19, 0x2, "ada/ad102"),
    (0x2700, 0x277F, "AD103", 0x19, 0x3, "ada/ad102"),
    (0x2780, 0x27FF, "AD104", 0x19, 0x4, "ada/ad102"),
    # 0x2800-0x283F is AD106. The "RTX 3050 A Laptop GPU" at 0x2822 lands
    # here -- Ada silicon sold under a 30-series name.
    (0x2800, 0x283F, "AD106", 0x19, 0x6, "ada/ad102"),
    # 0x2840-0x28FF is AD107, including the other two "RTX 3050 A" IDs
    # (0x28A3, 0x28E3). See ../DEVICE-IDS.md.
    (0x2840, 0x28FF, "AD107", 0x19, 0x7, "ada/ad102"),
    # Blackwell consumer/pro. Included only so the tool does not report
    # UNKNOWN for a modern card; irrelevant to this project. NVIDIA ships
    # register headers under blackwell/gb202.
    (0x2B00, 0x2BFF, "GB202", 0x1B, 0x2, "blackwell/gb202"),
]

NAME_TABLE = (
    Path(__file__).resolve().parent.parent
    / "reference"
    / "open-gpu-kernel-modules"
    / "src"
    / "nvidia"
    / "generated"
    / "g_nv_name_released.h"
)

ENTRY_RE = re.compile(
    r"\{\s*0x([0-9A-Fa-f]{4})\s*,\s*0x([0-9A-Fa-f]{4})\s*,"
    r"\s*0x([0-9A-Fa-f]{4})\s*,\s*\"([^\"]*)\"\s*\}"
)


def parse_name_table(path):
    """Return list of (device_id, subsys_device, subsys_vendor, name)."""
    if not path.is_file():
        return None

    entries = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for m in ENTRY_RE.finditer(text):
        entries.append(
            (int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), m.group(4))
        )
    return entries


def die_for(device_id):
    for low, high, die, arch, impl, headers in DIE_RANGES:
        if low <= device_id <= high:
            return die, arch, impl, headers
    return None, None, None, None


def parse_device_id(text):
    """Accept '25a2', '0x25a2', '10de:25a2', or a full lspci -nn line."""
    m = re.search(r"10de:([0-9A-Fa-f]{4})", text, re.IGNORECASE)
    if m:
        return int(m.group(1), 16)

    cleaned = text.strip().lower().removeprefix("0x")
    if re.fullmatch(r"[0-9a-f]{4}", cleaned):
        return int(cleaned, 16)

    return None


def expected_boot_values(arch, impl):
    """Compute the NV_PMC_BOOT_0 / BOOT_42 values the Phase 3 kext should read."""
    # BOOT_0: arch low 5 bits at 28:24, arch bit 5 at 8:8, impl at 23:20.
    arch_lo = arch & 0x1F
    arch_hi = (arch >> 5) & 0x01
    boot0 = (arch_lo << 24) | (arch_hi << 8) | (impl << 20)

    # BOOT_42: arch at 29:24, impl at 23:20. CHIP_ID is the 29:20 composite.
    boot42 = (arch << 24) | (impl << 20)
    chip_id = (arch << 4) | impl

    return boot0, boot42, chip_id


def report(device_id, entries):
    die, arch, impl, headers = die_for(device_id)

    print(f"device ID        : 10de:{device_id:04x}")

    names = []
    if entries:
        names = sorted({n for did, _, _, n in entries if did == device_id})

    if names:
        print(f"marketing name(s): {names[0]}")
        for extra in names[1:]:
            print(f"                   {extra}")
    elif entries is None:
        print("marketing name(s): (name table not found -- clone the reference repo)")
    else:
        print("marketing name(s): not in NVIDIA's released table")

    if die is None:
        print("die              : UNKNOWN -- outside the ranges this tool knows")
        print()
        print("This ID is not in a range mapped here. Check the register headers")
        print("under reference/open-gpu-kernel-modules/src/common/inc/swref/published/")
        return 2

    arch_name, arch_dir = ARCHITECTURES.get(arch, ("unknown", None))
    boot0, boot42, chip_id = expected_boot_values(arch, impl)

    print(f"die              : {die}")
    print(f"architecture     : {arch_name} (arch 0x{arch:02x}, impl 0x{impl:x})")
    print()
    print("Expected Phase 3 kext output:")
    print(f"  NV_PMC_BOOT_0  = 0x{boot0:08x} | 0x000000xx   (xx = stepping revision)")
    print(f"  NV_PMC_BOOT_42 = 0x{boot42:08x} | revision bits")
    print(f"  CHIP_ID        = 0x{chip_id:03x}")
    print()
    print("Register headers to work from:")
    print(f"  src/common/inc/swref/published/{headers}/")
    print()

    if arch == 0x17:
        print("VERDICT: Ampere. PLAN.md and the phase0-kext are already targeted")
        print("         at this architecture. No retargeting needed.")
        return 0

    if arch == 0x19:
        print("VERDICT: Ada Lovelace, NOT Ampere -- despite any '30-series' name.")
        print("         This is the 'RTX 3050 A' rebadge. Everything in PLAN.md that")
        print(f"         says Ampere/GA107 must be retargeted to {die}:")
        print(f"           - register headers  -> {headers}/")
        print("           - GSP firmware      -> different blob, different version")
        print("           - SASS ISA          -> different instruction encoding")
        print("         The phase0-kext still works (it decodes any architecture),")
        print("         but its GA107 expectation messages will not match.")
        return 0

    print(f"VERDICT: {arch_name}. Outside the scope PLAN.md was written for.")
    return 0


def list_3050s(entries):
    if not entries:
        print("Name table not found. Clone the reference repo first:", file=sys.stderr)
        print("  git clone --depth 1 "
              "https://github.com/NVIDIA/open-gpu-kernel-modules", file=sys.stderr)
        return 1

    matches = {}
    for did, _, _, name in entries:
        if "3050" in name:
            matches.setdefault(did, set()).add(name)

    if not matches:
        print("No 3050 entries found in the table.")
        return 1

    print(f"{'devid':<8} {'die':<8} {'arch':<14} name")
    print("-" * 72)

    for did in sorted(matches):
        die, arch, _, _ = die_for(did)
        arch_name = ARCHITECTURES.get(arch, ("?", None))[0] if arch else "?"
        flag = "  <-- NOT Ampere" if arch == 0x19 else ""
        for name in sorted(matches[did]):
            print(f"{did:04x}     {die or '?':<8} {arch_name:<14} {name}{flag}")

    print()
    print("Rows flagged NOT Ampere are Ada dies sold under a 30-series name.")
    print()
    print("Caveat: this is a substring match on the marketing name, so it also")
    print("catches datacenter parts that ship a vGPU profile named after a")
    print("GeForce card -- e.g. 0x2237 is an A10G (GA102), not a real 3050.")
    print("Only the 0x25xx and 0x28xx rows are actual laptop 3050 hardware.")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Resolve an NVIDIA PCI device ID to its die and architecture."
    )
    ap.add_argument(
        "device",
        nargs="?",
        help="device ID ('25a2', '10de:25a2') or a full 'lspci -nn' line",
    )
    ap.add_argument(
        "--list-3050",
        action="store_true",
        help="list every device sold as an RTX 3050 and which die it really is",
    )
    args = ap.parse_args()

    entries = parse_name_table(NAME_TABLE)

    if args.list_3050:
        return list_3050s(entries)

    if not args.device:
        ap.print_help()
        return 1

    device_id = parse_device_id(args.device)
    if device_id is None:
        print(f"Could not find a device ID in: {args.device!r}", file=sys.stderr)
        print("Expected something like '25a2', '10de:25a2', or an lspci -nn line.",
              file=sys.stderr)
        return 1

    return report(device_id, entries)


if __name__ == "__main__":
    sys.exit(main())
