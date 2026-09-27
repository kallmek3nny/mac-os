#!/usr/bin/env python3
"""
dma-summary.py -- reconstruct the falcon DMA transfers from a raw mmiotrace.

Why this exists: parse-mmiotrace.py tells you WHICH registers were touched and
collapse-sequence.py makes the order readable, but neither answers the question
that actually blocks Phase 4 -- *how many bytes moved, into IMEM or DMEM, from
what physical base*. That number is what you compare against the firmware blobs
on the laptop to work out which blob the driver was loading.

The model, confirmed against the vendor's own code:

    DMATRFBASE  (+0x110)  physical base >> 8      -- set once per image
    DMATRFBASE1 (+0x128)  high bits of that base
    DMATRFMOFFS (+0x114)  falcon-side offset
    DMATRFFBOFFS(+0x11C)  memory-side offset
    DMATRFCMD   (+0x118)  IMEM/DMEM, direction, size -- the falcon latches HERE

So a transfer is counted on each CMD write, using the offsets last written.

This ordering was originally read backwards. The raw trace prints

    118, 114, 11C, 118, 114, 11C, ...

which reads equally well as (CMD)(offsets) or (offsets)(CMD), and the first
interpretation -- CMD as configuration, the offset write as the trigger -- is
the intuitive one. It is wrong. Two pieces of evidence settle it:

  1. s_dmaTransfer_GA102 in nvidia.ko 595.84 writes 0x114, then 0x11C, then
     0x118 inside its loop body, then tests the loop condition. CMD is last.
  2. The trace's final three writes are 114, 11C, 118 -- it ends on a CMD,
     which only happens if CMD closes each iteration. The very first 118 is a
     pre-loop priming write, which is why there are 173 CMD writes but 174
     FBOFFS writes.

The consequence of the wrong model was an off-by-one: each set of offsets got
paired with the PREVIOUS iteration's CMD, so the IMEM/DMEM split was shifted
by one transfer.

Usage:
    python dma-summary.py mmio-boot.log
    python dma-summary.py mmio-boot.log --engine gsp
    python dma-summary.py mmio-boot.log --transfers      # per-transfer detail
"""

import argparse
import re
import sys

#
# Engine apertures, absolute inside BAR0. PSEC is at 0x840000 -- see
# dev_sec_pri.h:27. parse-mmiotrace.py originally had it at 0x114000 and lost
# an entire falcon boot to the "unmapped" bucket; do not re-derive it by
# adding to the GSP base.
#
ENGINES = {
    "gsp":  0x110000,
    "sec2": 0x840000,
}

# Falcon-relative DMA registers, dev_falcon_v4.h.
REG_DMATRFBASE  = 0x110
REG_DMATRFMOFFS = 0x114
REG_DMATRFCMD   = 0x118
REG_DMATRFFBOFFS = 0x11C
REG_DMATRFBASE1 = 0x128

# DMATRFCMD fields.
CMD_IMEM_SHIFT  = 4
CMD_WRITE_SHIFT = 5
CMD_SIZE_SHIFT  = 8
CMD_SIZE_MASK   = 0x7

# Raw mmiotrace row: op width time id address value pc(?) trailing
ROW = re.compile(
    r"^(?P<op>[RW])\s+\d+\s+"
    r"(?P<time>\d+\.\d+)\s+"
    r"\d+\s+"
    r"(?P<addr>0x[0-9a-fA-F]+)\s+"
    r"(?P<value>0x[0-9a-fA-F]+)"
)


def cmd_size_bytes(cmd):
    """SIZE field is an exponent: 0 -> 4 B, 6 -> 256 B."""
    return 4 << ((cmd >> CMD_SIZE_SHIFT) & CMD_SIZE_MASK)


def cmd_is_imem(cmd):
    return ((cmd >> CMD_IMEM_SHIFT) & 1) != 0


def cmd_is_write(cmd):
    """WRITE set means falcon -> memory. Clear means memory -> falcon (a load)."""
    return ((cmd >> CMD_WRITE_SHIFT) & 1) != 0


class EngineState:
    def __init__(self, name, base):
        self.name = name
        self.base = base
        self.cmd = None
        self.moffs = None
        self.fboffs = None
        self.dmabase = None
        self.dmabase1 = None
        self.priming = 0             # CMD writes with no offsets set yet
        self.segment = 0             # bumped on each DMATRFBASE reprogram
        self.transfers = []          # (time, cmd, moffs, fboffs, dmabase, seg)
        self.base_writes = []        # (time, dmabase, dmabase1)

    def note_write(self, time, offset, value):
        if offset == REG_DMATRFMOFFS:
            self.moffs = value
        elif offset == REG_DMATRFFBOFFS:
            self.fboffs = value
        elif offset == REG_DMATRFBASE:
            self.dmabase = value
            self.segment += 1
            self.moffs = None
            self.fboffs = None
            self.base_writes.append((time, value, self.dmabase1))
        elif offset == REG_DMATRFBASE1:
            self.dmabase1 = value
            if self.base_writes:
                t, b, _ = self.base_writes[-1]
                self.base_writes[-1] = (t, b, value)
        elif offset == REG_DMATRFCMD:
            self.cmd = value
            #
            # The latch. Both offsets must already have been written or this
            # is the pre-loop priming write, not a transfer -- counting it
            # would inflate the total by one and attribute bytes to an
            # offset pair that does not exist yet.
            #
            if self.moffs is None or self.fboffs is None:
                self.priming += 1
                return
            self.transfers.append(
                (time, value, self.moffs, self.fboffs, self.dmabase,
                 self.segment))


def report_gaps(state):
    """
    Report transfers the capture LOST, not transfers the driver skipped.

    MOFFS advances by exactly one transfer size per iteration, so within a
    segment the set of offsets should be a contiguous run. It is not.

    Group by (segment, destination) -- NOT by segment alone. IMEM and DMEM
    each restart MOFFS at their own base, so lumping them together invents
    gaps that are really just the two ranges overlapping.

    What survives that correction is still real, and its shape is the whole
    argument: the losses come in runs of 25-26 blocks, evenly spaced, and the
    SAME pattern appears on GSP and on SEC2 -- two engines whose loads are
    720 ms apart and share nothing but the tracer. A driver does not skip
    6.4 KiB out of the middle of a signed image three times at regular
    intervals, and it certainly does not do so identically on two unrelated
    engines. This is the mmiotrace ring buffer overrunning while the consumer
    falls behind, the same defect that produced the 608 spliced records and
    the 22,756 UNKNOWN-opcode lines elsewhere in this capture.

    This matters because the byte totals above are therefore a FLOOR. The
    image the driver actually moved is the offset span, not the sum of the
    transfers that survived to disk.
    """
    groups = {}
    for _t, cmd, moffs, _fb, _base, seg in state.transfers:
        if moffs is None:
            continue
        key = (seg, "IMEM" if cmd_is_imem(cmd) else "DMEM")
        groups.setdefault(key, []).append((moffs, cmd_size_bytes(cmd)))

    total_lost = 0
    lines = []
    for key in sorted(groups):
        entries = groups[key]
        offs = sorted({o for o, _ in entries})
        step = min(s for _, s in entries)
        span = offs[-1] - offs[0] + step
        expected = span // step
        missing = expected - len(offs)
        total_lost += missing * step
        gaps = [((a + step), (b - step), (b - a) // step - 1)
                for a, b in zip(offs, offs[1:]) if b - a > step]
        lines.append((key, offs[0], offs[-1], span, len(offs), missing, gaps))

    if not lines:
        return

    print("  segments       :")
    for (seg, dest), lo, hi, span, seen, missing, gaps in lines:
        flag = "" if missing == 0 else f"   *** {missing} block(s) MISSING"
        print(f"    [{seg}] {dest}  MOFFS 0x{lo:X}..0x{hi:X}  span {span:,} B  "
              f"seen {seen}{flag}")
        for g0, g1, n in gaps:
            print(f"          gap 0x{g0:X}..0x{g1:X}  ({n} blocks, "
                  f"{n * 256:,} B)")

    if total_lost:
        span_total = sum(l[3] for l in lines)
        print()
        print(f"  *** {total_lost:,} bytes are missing from the CAPTURE, not "
              f"from the transfer.")
        print(f"  *** mmiotrace dropped records under load (see the 608 "
              f"spliced lines in this")
        print(f"  *** trace). Treat the transfer totals as a floor; the image "
              f"size implied by")
        print(f"  *** contiguous offset coverage is {span_total:,} bytes.")


def summarise(state):
    imem = [t for t in state.transfers if cmd_is_imem(t[1])]
    dmem = [t for t in state.transfers if not cmd_is_imem(t[1])]

    imem_bytes = sum(cmd_size_bytes(t[1]) for t in imem)
    dmem_bytes = sum(cmd_size_bytes(t[1]) for t in dmem)

    print(f"\n{state.name} -- falcon at 0x{state.base:06X}")
    print("-" * 72)

    if not state.transfers:
        print("  no DMA transfers observed")
        return

    print(f"  transfers      : {len(state.transfers)}")
    print(f"    into IMEM    : {len(imem):4d}  ({imem_bytes:,} bytes)")
    print(f"    into DMEM    : {len(dmem):4d}  ({dmem_bytes:,} bytes)")
    print(f"    total        : {imem_bytes + dmem_bytes:,} bytes")

    if state.priming:
        print(f"  priming writes : {state.priming}  "
              f"(CMD written before any offset -- not a transfer)")

    report_gaps(state)

    directions = {cmd_is_write(t[1]) for t in state.transfers}
    if directions == {False}:
        print("  direction      : memory -> falcon (load) on every transfer")
    elif directions == {True}:
        print("  direction      : falcon -> memory on every transfer")
    else:
        print("  direction      : MIXED -- inspect with --transfers")

    sizes = sorted({cmd_size_bytes(t[1]) for t in state.transfers})
    print(f"  transfer sizes : {', '.join(str(s) + ' B' for s in sizes)}")

    moffs = [t[2] for t in state.transfers if t[2] is not None]
    if moffs:
        print(f"  MOFFS range    : 0x{min(moffs):X} .. 0x{max(moffs):X}")

    print(f"  base programmed: {len(state.base_writes)} time(s)")
    for t, b, b1 in state.base_writes:
        if b is None:
            continue
        phys = (b << 8) | ((b1 or 0) << 40)
        print(f"    t={t:9.6f}  DMATRFBASE=0x{b:08X}  "
              f"-> physical 0x{phys:012X}")


def print_transfers(state):
    print(f"\n{state.name} -- per-transfer detail")
    print(f"{'time':>10}  {'cmd':>8}  {'dest':5} {'size':>6}  "
          f"{'moffs':>8}  {'fboffs':>8}")
    print("-" * 60)
    for time, cmd, moffs, fboffs, _, _seg in state.transfers:
        print(f"{time:10.6f}  0x{cmd:06X}  "
              f"{'IMEM' if cmd_is_imem(cmd) else 'DMEM':5} "
              f"{cmd_size_bytes(cmd):5d}B  "
              f"0x{(moffs or 0):06X}  0x{fboffs:06X}")


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", help="raw mmio-boot.log")
    ap.add_argument("--engine", choices=sorted(ENGINES),
                    help="restrict to one engine (default: all)")
    ap.add_argument("--bar0", default="0x81000000",
                    help="BAR0 base from the MAP record (default 0x81000000)")
    ap.add_argument("--transfers", action="store_true",
                    help="print every transfer, not just the summary")
    args = ap.parse_args(argv)

    bar0 = int(args.bar0, 16)
    wanted = [args.engine] if args.engine else sorted(ENGINES)
    states = {n: EngineState(n, ENGINES[n]) for n in wanted}

    try:
        fh = open(args.trace, "r", encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"cannot read {args.trace}: {exc}", file=sys.stderr)
        return 1

    rows = 0
    with fh:
        for line in fh:
            m = ROW.match(line)
            if not m or m.group("op") != "W":
                continue
            rows += 1
            addr = int(m.group("addr"), 16)
            if addr < bar0:
                continue
            offset = addr - bar0
            for state in states.values():
                rel = offset - state.base
                if 0 <= rel < 0x4000:
                    state.note_write(float(m.group("time")),
                                     rel, int(m.group("value"), 16))
                    break

    print(f"trace          : {args.trace}")
    print(f"BAR0 base      : 0x{bar0:08X}")
    print(f"writes scanned : {rows:,}")

    for name in wanted:
        summarise(states[name])
        if args.transfers:
            print_transfers(states[name])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
