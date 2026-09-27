#!/usr/bin/env python3
"""
parse-mmiotrace.py -- turn a raw mmiotrace capture into a readable account of
what the working NVIDIA driver does to the GPU during bringup.

Why this exists: the raw log is tens of thousands of lines of hex with no
symbol names. What you actually want to know is "which registers, in what
order, and which of them does my kext already understand". This does that
translation, using the same NVIDIA headers the kext was built from.

The output is the Phase 4 specification: a register-by-register account of the
boot sequence, annotated with what each offset means.

Usage:
    python parse-mmiotrace.py mmio-boot.log
    python parse-mmiotrace.py mmio-boot.log --summary
    python parse-mmiotrace.py mmio-boot.log --first 200
    python parse-mmiotrace.py mmio-boot.log --region gc6

No dependencies. Runs anywhere Python 3 does.
"""

import argparse
import re
import sys
from collections import Counter, OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SWREF = (
    ROOT / "reference" / "open-gpu-kernel-modules"
    / "src" / "common" / "inc" / "swref" / "published"
)

HEADERS = [
    SWREF / "nv_ref.h",
    SWREF / "ampere" / "ga102" / "dev_boot.h",
    SWREF / "ampere" / "ga102" / "dev_gc6_island.h",
    SWREF / "ampere" / "ga102" / "dev_gc6_island_addendum.h",
    SWREF / "ampere" / "ga102" / "dev_falcon_v4.h",
    SWREF / "ampere" / "ga102" / "dev_gsp.h",
    SWREF / "ampere" / "ga102" / "dev_riscv_pri.h",
    SWREF / "ampere" / "ga102" / "dev_fbif_v4.h",
    SWREF / "ampere" / "ga102" / "dev_vm.h",
    SWREF / "ampere" / "ga102" / "dev_ram.h",
    SWREF / "ampere" / "ga102" / "dev_ce_base.h",
]

#
# Coarse address regions. mmiotrace gives absolute offsets into BAR0; these
# ranges tell you which engine a given access belongs to without needing an
# exact symbol match. Sourced from the NV_P* aperture definitions in the
# headers above (e.g. dev_gsp.h:26 -> NV_PGSP 0x113fff:0x110000).
#
REGIONS = [
    # NV_PMC_BOOT_0 (0x0) and NV_PMC_BOOT_42 (0xA00) both live in this first
    # 4 KiB page. An earlier version of this table had a separate "pmc-boot"
    # region at 0x00A000 -- that was a misreading of the 0xA00 offset, and it
    # could never match anything.
    (0x000000, 0x000FFF, "pmc",      "master control / chip identity (BOOT_0, BOOT_42)"),
    (0x001000, 0x001FFF, "pbus",     "bus interface"),
    (0x002000, 0x002FFF, "pfifo",    "command submission FIFO"),
    (0x009000, 0x009FFF, "ptimer",   "timer"),
    (0x010000, 0x01FFFF, "pdisp",    "display engine"),
    (0x020000, 0x02FFFF, "ptherm",   "thermal / power"),
    (0x022000, 0x022FFF, "ppwr",     "power management unit"),
    (0x088000, 0x08FFFF, "pcfg",     "PCI config mirror"),
    (0x100000, 0x10FFFF, "pfb",      "framebuffer / memory controller"),
    (0x110000, 0x113FFF, "pgsp",     "GSP falcon -- Phase 4 lives here"),
    (0x118000, 0x118FFF, "gc6",      "GC6 island scratch -- GFW_BOOT, FB size"),
    (0x120000, 0x12FFFF, "pnvdec",   "video decode"),
    (0x140000, 0x14FFFF, "pnvlink",  "nvlink"),
    (0x400000, 0x4FFFFF, "pgraph",   "graphics engine"),
    (0x600000, 0x6FFFFF, "pcrtc",    "CRTC / scanout"),
    (0x700000, 0x7FFFFF, "pramin",   "instance memory window"),
    (0x840000, 0x843FFF, "psec",     "SEC2 falcon -- boots before GSP"),

    # Not in the vendored GA102 headers, but unambiguous from the trace: the
    # driver reads 0xBB0080/0xBB0084 as a pair in a tight loop, the low word
    # advances monotonically (0x5FE64BE0 -> 0x5FE676A0 -> ...) while the high
    # word stays 0x18C9588E. That is a 64-bit free-running counter being read
    # low-then-high, i.e. PTIMER. Named from behaviour, not from a header --
    # the exact register symbols are NOT confirmed.
    (0xBB0000, 0xBB0FFF, "ptimer2",  "free-running 64-bit counter (inferred from trace)"),
]

#
# Registers the Phase 3/3.5 kext already reads. Seeing these in the trace, with
# the same values, is direct cross-validation between the two.
#
KEXT_KNOWN = OrderedDict([
    (0x000000, "NV_PMC_BOOT_0        <- kext Phase 3 reads this"),
    (0x000A00, "NV_PMC_BOOT_42       <- kext Phase 3 reads this"),
    (0x118234, "GFW_BOOT (scratch 05[0]) <- kext Phase 3.5 polls this"),
    (0x1183A4, "NV_USABLE_FB_SIZE_IN_MB  <- kext Phase 3.5 reads this"),
])

#
# mmiotrace text format, one access per line:
#   R 4 0.000000 0 0xf2000000 0x17700000 0
#   W 4 0.000001 0 0xf2000200 0x00000001 0
# fields: op, width, timestamp, pid, phys_addr, value, pc
#
# It also emits MAP / UNMAP lines giving the physical base of each mapped
# region, which is what lets absolute physical addresses be converted back to
# BAR-relative offsets.
#
ACCESS_RE = re.compile(
    r"^\s*(?P<op>[RW])\s+(?P<width>\d+)\s+(?P<ts>[\d.]+)\s+(?P<pid>\d+)\s+"
    r"0x(?P<addr>[0-9a-fA-F]+)\s+0x(?P<val>[0-9a-fA-F]+)"
)
MAP_RE = re.compile(
    r"^\s*MAP\s+(?P<ts>[\d.]+)\s+(?P<pid>\d+)\s+0x(?P<phys>[0-9a-fA-F]+)\s+"
    r"0x(?P<virt>[0-9a-fA-F]+)\s+0x(?P<len>[0-9a-fA-F]+)"
)

DEFINE_RE = re.compile(
    r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(0[xX][0-9A-Fa-f]+)\s*(?:/\*.*)?$"
)


#
# Falcon-relative apertures.
#
# dev_falcon_v4.h defines its registers relative to an engine base, not as
# absolute BAR0 offsets: NV_PFALCON_FALCON_MAILBOX0 is 0x40, HWCFG2 is 0xF4.
# Each engine that embeds a Falcon maps that block at its own base -- GSP at
# 0x110000 (dev_gsp.h:26, NV_PGSP 0x113fff:0x110000), SEC at 0x114000.
#
# Without this, every Falcon access in the trace annotates as blank, which is
# exactly the region Phase 4 cares about most.
#
FALCON_APERTURES = [
    (0x110000, 0x113FFF, "PGSP"),
    (0x840000, 0x843FFF, "PSEC"),
]

FALCON_HEADER = SWREF / "ampere" / "ga102" / "dev_falcon_v4.h"


def _accept_define(name, value, limit):
    """Filter out field/mask defines, keeping only plausible register offsets."""
    if value >= limit or value % 4 != 0:
        return False
    return not name.endswith(("_MASK", "_SHIFT", "_WIDTH", "_VALUE", "_INIT"))


def load_symbols():
    """
    Return (absolute_syms, falcon_syms, missing).

    absolute_syms maps a BAR0 offset to a register name.
    falcon_syms maps an engine-relative offset to a Falcon register name.
    """
    syms = {}
    falcon = {}
    missing = []

    for path in HEADERS:
        if not path.is_file():
            missing.append(path)
            continue

        is_falcon = path == FALCON_HEADER

        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = DEFINE_RE.match(line)
            if not m:
                continue

            name, value = m.group(1), int(m.group(2), 0)

            if is_falcon:
                # Falcon register blocks are 4 KiB, so anything at or above
                # 0x1000 in this header is a field definition, not an offset.
                if _accept_define(name, value, 0x1000):
                    falcon.setdefault(value, name)
                continue

            if not _accept_define(name, value, 0x1000000):
                continue

            # First definition wins so base headers are not shadowed by addenda.
            syms.setdefault(value, name)

    return syms, falcon, missing


def region_for(offset):
    for lo, hi, short, desc in REGIONS:
        if lo <= offset <= hi:
            return short, desc
    return "?", "unmapped in this tool's region table"


def parse_trace(path):
    """Return (accesses, maps, stats). Accesses are BAR-relative where possible."""
    accesses = []
    maps = []
    skipped = 0
    total_lines = 0

    with path.open(encoding="utf-8", errors="replace") as fh:
        for line in fh:
            total_lines += 1

            if line.lstrip().startswith("#"):
                continue

            m = MAP_RE.match(line)
            if m:
                maps.append(
                    (int(m.group("phys"), 16), int(m.group("len"), 16), float(m.group("ts")))
                )
                continue

            m = ACCESS_RE.match(line)
            if not m:
                if line.strip():
                    skipped += 1
                continue

            accesses.append({
                "op": m.group("op"),
                "width": int(m.group("width")),
                "ts": float(m.group("ts")),
                "addr": int(m.group("addr"), 16),
                "val": int(m.group("val"), 16),
            })

    return accesses, maps, {"total_lines": total_lines, "unparsed": skipped}


def infer_bar_base(accesses, maps):
    """
    mmiotrace logs absolute physical addresses. BAR0 base is whichever mapped
    region is 16 MiB -- the register aperture on all Turing+ parts.
    """
    for phys, length, _ts in maps:
        if length == 0x1000000:
            return phys, "MAP record with a 16 MiB length"

    if maps:
        phys, length, _ts = max(maps, key=lambda m: m[1])
        return phys, f"largest MAP record (0x{length:X} bytes)"

    if not accesses:
        return None, "no accesses and no MAP records"

    #
    # Fallback: no MAP lines survived. BAR bases are at least 16 MiB aligned,
    # so round the lowest observed address down.
    #
    lowest = min(a["addr"] for a in accesses)
    return lowest & ~0xFFFFFF, "inferred by aligning the lowest address (no MAP records)"


def annotate(offset, syms, falcon=None):
    if offset in KEXT_KNOWN:
        return KEXT_KNOWN[offset]
    if offset in syms:
        return syms[offset]

    #
    # Fall back to a Falcon-relative lookup. dev_falcon_v4.h offsets are
    # relative to the owning engine's base, so 0x1100F4 is PGSP (0x110000)
    # plus HWCFG2 (0xF4).
    #
    if falcon:
        for lo, hi, engine in FALCON_APERTURES:
            if lo <= offset <= hi:
                rel = offset - lo
                if rel in falcon:
                    # Rewrite the generic PFALCON prefix to name the engine.
                    name = falcon[rel]
                    suffix = name.replace("NV_PFALCON_FALCON_", "", 1)
                    return f"NV_{engine}_FALCON_{suffix}  (+0x{rel:03X})"
                return f"[{engine} falcon +0x{rel:03X}]"

    return ""


def print_header(path, stats, base, base_why, syms, falcon, missing):
    print(f"trace          : {path}")
    print(f"lines          : {stats['total_lines']:,} ({stats['unparsed']:,} unparsed)")
    if base is not None:
        print(f"BAR0 base      : 0x{base:X}  ({base_why})")
    else:
        print(f"BAR0 base      : unknown -- {base_why}")
    print(f"symbols loaded : {len(syms):,} absolute, "
          f"{len(falcon):,} falcon-relative")

    if missing:
        print()
        print("warning: some NVIDIA headers not found, annotations will be sparse:")
        for p in missing:
            print(f"  {p.name}")
    print()


def cmd_summary(accesses, base, syms, falcon):
    reads = sum(1 for a in accesses if a["op"] == "R")
    writes = len(accesses) - reads

    print(f"accesses       : {len(accesses):,}  ({reads:,} reads, {writes:,} writes)")

    if not accesses:
        print()
        print("No MMIO accesses parsed. Either the tracer never attached, or the")
        print("driver failed to load. Re-run Step 3e while still booted.")
        return

    span = max(a["ts"] for a in accesses) - min(a["ts"] for a in accesses)
    print(f"time span      : {span:.3f} s")
    print()

    region_counts = Counter()
    region_writes = Counter()

    for a in accesses:
        off = a["addr"] - base if base is not None else a["addr"]
        short, _desc = region_for(off)
        region_counts[short] += 1
        if a["op"] == "W":
            region_writes[short] += 1

    print("Accesses by engine region:")
    print()
    print(f"  {'region':<10} {'total':>9} {'writes':>9}   what it is")
    print("  " + "-" * 68)

    for short, count in region_counts.most_common():
        desc = next((d for _lo, _hi, s, d in REGIONS if s == short),
                    "unmapped in this tool's region table")
        print(f"  {short:<10} {count:>9,} {region_writes[short]:>9,}   {desc}")

    print()
    print("Registers the kext already understands:")
    print()

    found_any = False
    for off, label in KEXT_KNOWN.items():
        hits = [a for a in accesses
                if (a["addr"] - base if base is not None else a["addr"]) == off]
        if not hits:
            print(f"  0x{off:06X}  NOT SEEN   {label.split('<-')[0].strip()}")
            continue

        found_any = True
        vals = Counter(a["val"] for a in hits)
        common = ", ".join(f"0x{v:08X}" for v, _ in vals.most_common(3))
        print(f"  0x{off:06X}  {len(hits):>4} hits  {label.split('<-')[0].strip()}")
        print(f"              values: {common}")

    if found_any:
        print()
        print("Those values are the ground truth to compare your kext's log against.")
        print("If the kext reads something different from the same offset, the kext")
        print("is wrong -- the Linux driver is known-working on this hardware.")

    print()
    top = Counter()
    for a in accesses:
        off = a["addr"] - base if base is not None else a["addr"]
        top[off] += 1

    print("Most-touched registers:")
    print()
    for off, count in top.most_common(15):
        name = annotate(off, syms, falcon)
        short, _ = region_for(off)
        print(f"  0x{off:06X}  {count:>7,}  [{short}] {name}")


def cmd_listing(accesses, base, syms, falcon, limit, region_filter):
    shown = 0

    print(f"{'time':>10}  op  {'offset':>8}  {'value':>10}  region    register")
    print("-" * 88)

    t0 = accesses[0]["ts"] if accesses else 0.0

    for a in accesses:
        off = a["addr"] - base if base is not None else a["addr"]
        short, _desc = region_for(off)

        if region_filter and short != region_filter:
            continue

        name = annotate(off, syms, falcon)
        rel = a["ts"] - t0

        print(f"{rel:>10.6f}  {a['op']}   0x{off:06X}  0x{a['val']:08X}  "
              f"{short:<9} {name}")

        shown += 1
        if limit and shown >= limit:
            print()
            print(f"... stopped at {limit}. Use --first 0 for the whole listing.")
            break

    if shown == 0:
        print()
        print(f"No accesses matched region '{region_filter}'.")
        print("Valid regions: " + ", ".join(sorted({s for _lo, _hi, s, _d in REGIONS})))


def main():
    ap = argparse.ArgumentParser(
        description="Parse an mmiotrace capture into an annotated register log."
    )
    ap.add_argument("trace", help="path to mmio-boot.log from Step 3")
    ap.add_argument("--summary", action="store_true",
                    help="statistics by engine region (default if no other mode)")
    ap.add_argument("--first", type=int, default=None, metavar="N",
                    help="print the first N accesses in order; 0 for all")
    ap.add_argument("--region", default=None, metavar="NAME",
                    help="restrict the listing to one region, e.g. pgsp or gc6")
    args = ap.parse_args()

    path = Path(args.trace)
    if not path.is_file():
        print(f"error: {path} not found", file=sys.stderr)
        return 1

    syms, falcon, missing = load_symbols()
    accesses, maps, stats = parse_trace(path)
    base, base_why = infer_bar_base(accesses, maps)

    print_header(path, stats, base, base_why, syms, falcon, missing)

    if args.first is not None or args.region:
        limit = None if args.first == 0 else (args.first or 100)
        cmd_listing(accesses, base, syms, falcon, limit, args.region)
    else:
        cmd_summary(accesses, base, syms, falcon)

    return 0


if __name__ == "__main__":
    sys.exit(main())
