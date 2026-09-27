#!/usr/bin/env python3
"""
collapse-sequence.py -- turn a parse-mmiotrace.py region listing into the
ordered bringup sequence a driver author actually has to reimplement.

Why this exists: the PGSP listing is 206,293 lines, and ~99% of it is two
things repeating -- the IMEM/DMEM DMA transfer loop and the queue-head poll.
Both are single conceptual steps. Reading the raw listing to find the boot
control writes is hopeless; this collapses consecutive identical-register
runs into one line with a repeat count, so the actual sequence is visible.

Usage:
    python collapse-sequence.py pgsp-listing.txt              # writes only
    python collapse-sequence.py pgsp-listing.txt --with-reads  # reads too
    python collapse-sequence.py pgsp-listing.txt --gap 0.001   # mark stalls

Output columns:
    time      first timestamp of the run
    op        R or W
    offset    BAR0 offset
    xN        how many consecutive accesses collapsed into this line
    value     the value, or "first..last" when it varied across the run
    register  symbol name from the listing
"""

import argparse
import re
import sys

# parse-mmiotrace.py listing rows look like:
#   0.921174  W   0x110118  0x00000614  pgsp      NV_PGSP_FALCON_DMATRFCMD  (+0x118)
ROW = re.compile(
    r"^\s+(?P<time>\d+\.\d+)\s+"
    r"(?P<op>[RW])\s+"
    r"(?P<offset>0x[0-9A-Fa-f]+)\s+"
    r"(?P<value>0x[0-9A-Fa-f]+)\s+"
    r"(?P<region>\S+)\s+"
    r"(?P<register>.*?)\s*$"
)


class Run:
    """A maximal stretch of consecutive accesses to the same register+op."""

    __slots__ = ("time", "op", "offset", "register", "count",
                 "first_value", "last_value", "values", "end_time")

    def __init__(self, row):
        self.time = float(row["time"])
        self.end_time = self.time
        self.op = row["op"]
        self.offset = row["offset"].upper().replace("0X", "0x")
        self.register = row["register"]
        self.count = 1
        self.first_value = row["value"]
        self.last_value = row["value"]
        self.values = {row["value"]}

    def extend(self, row):
        self.count += 1
        self.last_value = row["value"]
        self.end_time = float(row["time"])
        if len(self.values) < 8:
            self.values.add(row["value"])

    def matches(self, row):
        return (row["op"] == self.op
                and row["offset"].upper().replace("0X", "0x") == self.offset)

    def value_text(self):
        if len(self.values) == 1:
            return self.first_value
        return f"{self.first_value}..{self.last_value}"

    def duration(self):
        return self.end_time - self.time


def collapse(lines, want_reads):
    runs = []
    current = None
    skipped = 0
    for line in lines:
        m = ROW.match(line)
        if not m:
            skipped += 1
            continue
        row = m.groupdict()
        if row["op"] == "R" and not want_reads:
            continue
        if current is not None and current.matches(row):
            current.extend(row)
        else:
            current = Run(row)
            runs.append(current)
    return runs, skipped


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("listing", help="output of parse-mmiotrace.py")
    ap.add_argument("--with-reads", action="store_true",
                    help="include reads (polling loops) as well as writes")
    ap.add_argument("--gap", type=float, default=0.0,
                    help="insert a marker when more than GAP seconds pass "
                         "between runs; 0 disables (default 0)")
    ap.add_argument("--min-count", type=int, default=1,
                    help="only print runs of at least this length")
    args = ap.parse_args(argv)

    try:
        with open(args.listing, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError as exc:
        print(f"cannot read {args.listing}: {exc}", file=sys.stderr)
        return 1

    runs, skipped = collapse(lines, args.with_reads)
    if not runs:
        print("no listing rows matched -- is this a parse-mmiotrace.py listing?",
              file=sys.stderr)
        return 1

    total = sum(r.count for r in runs)
    print(f"listing        : {args.listing}")
    print(f"accesses       : {total:,} ({'reads+writes' if args.with_reads else 'writes only'})")
    print(f"collapsed runs : {len(runs):,}")
    print(f"header/other   : {skipped:,} lines ignored")
    print()
    print(f"{'time':>10}  {'op':2} {'offset':10} {'xN':>7}  {'value':22} register")
    print("-" * 100)

    prev_end = None
    for run in runs:
        if args.gap and prev_end is not None and run.time - prev_end > args.gap:
            print(f"{'':>10}  .. {run.time - prev_end * 1:.6f}s gap "
                  f"({(run.time - prev_end) * 1000:.1f} ms) ..")
        if run.count >= args.min_count:
            xn = f"x{run.count}" if run.count > 1 else ""
            dur = f"  [{run.duration() * 1000:.1f} ms]" if run.count > 1 else ""
            print(f"{run.time:10.6f}  {run.op:2} {run.offset:10} {xn:>7}  "
                  f"{run.value_text():22} {run.register}{dur}")
        prev_end = run.end_time

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
