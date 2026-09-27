#!/usr/bin/env python3
r"""read-igpu-mmio.py -- dump bounded DISPLAY-ENGINE register ranges from a live
Intel iGPU, by mmap'ing the device's BAR0 through sysfs.

    sudo python3 read-igpu-mmio.py --pci 0000:00:02.0 --out tgl-display-mmio.txt

Why this exists
---------------
The NVIDIA half of this project is built on a captured MMIO trace, and the
headers keep a hard line between values WITNESSED on hardware (`*_OBSERVED_*`)
and values reasoned out. A Tiger Lake display driver needs the same evidence
base, and nothing in this tree has ever read an Intel register.

This is a read-only dump of a live, working display engine: the panel is lit by
i915 while the dump runs, so every value is a register state that DEMONSTRABLY
drives this exact panel. That is far stronger than a datasheet default.

WHY THE RANGES ARE BOUNDED AND NOT "all of BAR0"
------------------------------------------------
BAR0 is 16 MiB and most of it is the GT (render/media) block, not display.
GT registers are behind FORCEWAKE: reading them from userspace without taking
forcewake returns garbage on a good day, and on some generations logs an
unclaimed-register error or wedges the read. Display registers in the ranges
below are in the display power well, which is ON whenever the panel is on --
so they are both safe to read and meaningful when read.

A blind dump of 16 MiB would therefore produce a large file that is mostly
noise, and would risk the machine for the noise part. The ranges here are the
display engine only.

WHAT A NULL RESULT DOES AND DOES NOT PROVE
------------------------------------------
An all-``0xFFFFFFFF`` range means the read did not reach the device -- wrong
BAR, wrong offset, or a powered-down well. An all-zero range means the same
thing often enough to be untrustworthy. Neither is evidence that a register
"is zero on this hardware".

So the tool reports a REACH line first: how many distinct non-trivial 32-bit
values it saw across everything it read. If that number is small, the mapping
is broken and every value in the dump is vacuous -- do not record any of it as
an observed constant. This is the same guard as ``find-kexts-in-image.py``'s
``scan reach``, for the same reason.
"""

import argparse
import mmap
import os
import struct
import sys

# (base, length, label). Bases are 4 KiB aligned because mmap requires it.
# Labels are descriptive only -- the authority for any individual offset is
# reference/i915/i915_reg.h, never this list.
RANGES = [
    (0x044000, 0x2000, "north display engine interrupts / DE misc"),
    (0x046000, 0x1000, "CDCLK, DPLL control"),
    (0x060000, 0x1000, "transcoder A timings"),
    (0x061000, 0x1000, "transcoder B timings"),
    (0x062000, 0x1000, "transcoder C timings"),
    (0x06C000, 0x1000, "DPCLKA / PLL distribution"),
    (0x06F000, 0x1000, "transcoder EDP timings (pre-Gen12 layout)"),
    (0x070000, 0x1000, "pipe A configuration and universal plane 1"),
    (0x071000, 0x1000, "pipe B configuration and universal plane 1"),
    (0x072000, 0x1000, "pipe C configuration and universal plane 1"),
    (0x0C0000, 0x1000, "south display / port control"),
    (0x0C7000, 0x1000, "panel power sequencing"),
    (0x0C8000, 0x1000, "backlight PWM"),
    (0x160000, 0x2000, "combo PHY A/B"),
    (0x162000, 0x2000, "combo PHY C and DDI aux"),
]

# Values that carry no information: an unmapped read, a cleared register, and
# the two obvious tell-tales of a dead mapping.
TRIVIAL = (0x00000000, 0xFFFFFFFF)


def bar0_size(pci):
    """Return BAR0's length in bytes from sysfs, or None if unreadable."""
    try:
        with open("/sys/bus/pci/devices/%s/resource" % pci) as fh:
            first = fh.readline().split()
    except OSError:
        return None
    if len(first) < 2:
        return None
    try:
        start = int(first[0], 16)
        end = int(first[1], 16)
    except ValueError:
        return None
    if end <= start:
        return None
    return end - start + 1


def dump(pci, out):
    path = "/sys/bus/pci/devices/%s/resource0" % pci
    size = bar0_size(pci)

    lines = []
    lines.append("=== read-igpu-mmio.py ===")
    lines.append("pci    : %s" % pci)
    lines.append("bar0   : %s" % ("%d bytes (0x%X)" % (size, size) if size else "UNKNOWN"))
    lines.append("source : %s" % path)
    lines.append("")

    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError as exc:
        lines.append("FAIL   : cannot open %s: %s" % (path, exc))
        lines.append("         Needs root. Without it nothing below was read.")
        out.write("\n".join(lines) + "\n")
        return 2

    distinct = set()
    total_read = 0

    try:
        for base, length, label in RANGES:
            if size is not None and base + length > size:
                lines.append("SKIP   0x%06X +0x%04X  %s" % (base, length, label))
                lines.append("       (past the end of BAR0 -- not an absence of the register)")
                lines.append("")
                continue

            try:
                mm = mmap.mmap(fd, length, mmap.MAP_SHARED,
                               mmap.PROT_READ, offset=base)
            except (OSError, ValueError) as exc:
                lines.append("FAIL   0x%06X +0x%04X  %s -- mmap: %s"
                             % (base, length, label, exc))
                lines.append("")
                continue

            lines.append("=== 0x%06X .. 0x%06X  %s ==="
                         % (base, base + length - 1, label))
            # Word-at-a-time, deliberately NOT `buf = mm[:length]`.
            #
            # A bulk slice is a memcpy, and glibc's memcpy is free to use 16-
            # or 32-byte vector loads. MMIO wants naturally aligned 32-bit
            # accesses: a wide or split load across device registers can
            # return a word neither half of which was ever simultaneously
            # present, and on read-to-clear or latching registers it can
            # disturb the state it is supposed to be observing. A 4-byte slice
            # lowers to a single aligned dword load.
            try:
                words = []
                for off in range(0, length, 4):
                    words.append(struct.unpack("<I", mm[off:off + 4])[0])
            finally:
                mm.close()

            total_read += length
            for i in range(0, len(words), 4):
                row = words[i:i + 4]
                for w in row:
                    if w not in TRIVIAL:
                        distinct.add(w)
                lines.append("0x%06X: %s"
                             % (base + i * 4,
                                " ".join("%08x" % w for w in row)))
            lines.append("")
    finally:
        os.close(fd)

    # The line that decides whether anything above is evidence.
    header = []
    header.append("REACH  : %d distinct non-trivial 32-bit values across %d bytes read"
                  % (len(distinct), total_read))
    if len(distinct) < 16:
        header.append("VACUOUS: too few distinct values. The mapping did not reach the")
        header.append("         device, or the display power well is down. Do NOT record")
        header.append("         any value below as an observed hardware constant.")
    else:
        header.append("OK     : the mapping reached a live device. Values below are real")
        header.append("         register state from a display that is currently driving")
        header.append("         the panel.")
    header.append("")

    out.write("\n".join(lines[:4] + header + lines[4:]) + "\n")
    return 0 if len(distinct) >= 16 else 1


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pci", default="0000:00:02.0", help="PCI address of the iGPU")
    ap.add_argument("--out", default=None, help="write here instead of stdout")
    args = ap.parse_args(argv)

    if args.out:
        with open(args.out, "w") as fh:
            rc = dump(args.pci, fh)
        sys.stderr.write("wrote %s\n" % args.out)
        return rc
    return dump(args.pci, sys.stdout)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
