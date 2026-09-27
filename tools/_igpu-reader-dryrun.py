#!/usr/bin/env python3
"""_igpu-reader-dryrun.py -- drive igpu-plane-dsi.sh's embedded reader against a
SYNTHETIC BAR0 image, on a Windows machine with no Intel GPU.

    python tools/_igpu-reader-dryrun.py 0     # PIPESRC raw == active
    python tools/_igpu-reader-dryrun.py 1     # PIPESRC raw == active + 1
    python tools/_igpu-reader-dryrun.py dead  # control fails: nulls go INCONCLUSIVE
    python tools/_igpu-reader-dryrun.py zero  # unused pages read 0x00000000, not all-ones

WHY THIS EXISTS
---------------
`sh -n` and `py_compile` prove the file PARSES. They say nothing about whether
the cross-examination can reach a DISAGREEING verdict. This project has been
burned repeatedly by checks that were green because they matched nothing, so the
reader is driven here against inputs that differ in exactly the quantities Q3/Q4
measure, plus one arm where the positive control is dead. If every arm prints
the same thing, the verdicts are decoration.

The fake `mmap` module is needed because Windows CPython's mmap.mmap has no
MAP_SHARED/PROT_READ parameters. Nothing on the Linux path changes.
"""
import os
import re
import struct
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))

# The script under test. IGPU_DRYRUN_SCRIPT overrides it so a caller can drive a
# MUTATED copy without touching the real file -- which is how
# tests/test_igpu_dryrun_arms.py proves its assertions are not vacuous: it reverts
# dead() to the pre-2026-08-16 form in a temp copy and requires the zero arm to
# change its answer. Without the override the only way to test that is editing the
# live script, and a concurrent gate run would then read a half-mutated file --
# the false-RED failure mode this tree already has a memory note about.
SCRIPT = os.environ.get("IGPU_DRYRUN_SCRIPT") or os.path.join(HERE, "igpu-plane-dsi.sh")
SIZE = 0x100000          # the reader maps min(filesize, 0x100000)


def extract_reader(path):
    src = open(path, "r", encoding="utf-8", newline="").read()
    m = re.search(r"<<'PYEOF'\n(.*?)\nPYEOF\n", src, re.S)
    if not m:
        raise SystemExit("FATAL: no PYEOF heredoc in %s" % path)
    return m.group(1)


def build_bar(arm):
    buf = bytearray(b"\xFF" * SIZE)      # unclaimed reads all-ones, like real MMIO

    def w(off, val):
        struct.pack_into("<I", buf, off, val & 0xFFFFFFFF)

    if arm == "dead":
        return buf                       # no live transcoder -> control must fail

    if arm == "zero":
        # MEASURED 2026-08-16 on 8086:9a68: an unused display page on real Tiger
        # Lake reads 0x00000000, NOT all-ones. The 0xFF fill above is what the
        # author EXPECTED hardware to do, which is why the original dead() --
        # None/0xFFFFFFFF only -- shipped and reported existing transcoders
        # B/C/D as "ABSENT". This arm reproduces the real silicon so that bug
        # cannot come back silently.
        buf = bytearray(SIZE)

    delta = 1 if arm == "1" else 0
    # Transcoder A, 1920x1080. Timing registers store value - 1.
    hact, htot, vact, vtot = 1919, 2159, 1079, 1109
    w(0x60000, (htot << 16) | hact)                      # HTOTAL
    w(0x6000C, (vtot << 16) | vact)                      # VTOTAL
    w(0x6001C, ((hact + delta) << 16) | (vact + delta))  # PIPESRC

    w(0x6C000, 0x00001234)               # DPCLKA-ish, not timings

    w(0x70008, 0x80000000)                               # pipe A TRANSCONF
    w(0x70180, 0x80000000 | (0x04 << 23))                # plane 1 CTL enabled
    w(0x70188, 0x00000280)                               # plane 1 STRIDE
    w(0x70280, 0x00000000)                               # plane 2 present, off
    return buf


class FakeMap:
    def __init__(self, buf, length):
        self._b = bytes(buf[:length])

    def __len__(self):
        return len(self._b)

    def __getitem__(self, k):
        return self._b[k]

    def close(self):
        pass


def main():
    arm = sys.argv[1] if len(sys.argv) > 1 else "0"
    buf = build_bar(arm)

    # PID in the name: two arms running concurrently (the test module drives five
    # runs, and other agents run gates at the same time) would otherwise share one
    # path, and one run's os.remove() would make the other read a missing file.
    img = os.path.join(HERE, "_dryrun-bar0-%d.bin" % os.getpid())
    with open(img, "wb") as fh:
        fh.write(buf)

    fake = types.ModuleType("mmap")
    fake.MAP_SHARED = 1
    fake.PROT_READ = 1
    fake.mmap = lambda fd, length, *a, **k: FakeMap(buf, length)
    sys.modules["mmap"] = fake

    sys.argv = ["-", img]
    try:
        exec(compile(extract_reader(SCRIPT), "<embedded reader>", "exec"),
             {"__name__": "__main__"})
    finally:
        os.remove(img)


if __name__ == "__main__":
    main()
