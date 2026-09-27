#!/usr/bin/env python
"""png-delta.py -- compare two ALREADY-SAVED PNGs from vm-screenshot.py.

Why this exists
---------------
`vm-screenshot.py --compare REF.png` does NOT compare against REF.png. It
captures BOTH of its frames itself -- frame 1, sleep, frame 2 -- and both land
*after* whatever you did. So it answers "is anything moving right now" and can
never attribute a change to an action. That limit is inherent to the flag and
is still live; it is not a bug that can be fixed.

Its argument is also a base name, not an output file: as of 2026-08-08 the
frames go to `REF.frame1.png` / `REF.frame2.png` and `REF.png` itself is never
written (`derived_frame_path()` in vm-screenshot.py, guarded by
tests/test_vm_screenshot.py::DerivedFramePathTests).

Before that fix it ran `second.save(args.compare)`, overwriting the file you
named, and that cost this project a wrong diagnosis on 2026-08-07: a
pre-injection "baseline" PNG had an mtime 3 seconds NEWER than the "after" PNG
it was supposedly compared to, because --compare had clobbered it. The
reported `changed: yes (guest 0.212%)` was two post-injection frames of an idle
guest, not before-vs-after injection. Any PNG in this repo saved before
2026-08-08 may therefore be a clobbered frame -- md5 it before trusting it.

So: to test whether an injection changed the guest, you need to diff two
files that were each captured by a separate `--out` run, with the injection
between them. That is what this does, and it does nothing else -- it never
captures, so it can never clobber a reference.

Noise floor
-----------
A booted macOS desktop is NOT static: the menu-bar clock ticks, notifications
fade, the cursor blinks. Two captures with no input between them still differ.
Always measure a control pair (capture, wait, capture, no injection) and treat
its guest delta as the floor. A post-injection delta below that floor proves
nothing.

Why byte-identical input is a HARD ERROR (measured 2026-08-07)
-------------------------------------------------------------
Before this check existed, feeding the same bytes twice printed
`delta : guest 0.000%, whole window 0.000%` and exited 0 -- indistinguishable
from a real null measurement. That is not hypothetical. Every one of the eight
supposed before/after frames in `shots/` collapsed to just TWO md5 hashes:

    090566eb0ad301a6633f1c6fa1d910fe  floor-a.png floor-b.png floor-c.png
                                      floor-d.png
    024e9d456fe7ed850ce3dff37ab58743  applemenu-20260807T2015Z.png
                                      mouse-a-20260807T2018Z.png
                                      mouse-b-20260807T2018Z.png
                                      mouse-a2-20260807T2022Z.png
                                      mouse-b2-20260807T2022Z.png

So the "idle floor of 0.000%" and the "mouse injection changed nothing" results
read off those pairs were arithmetic on duplicate inputs, not measurements of
the guest. A duplicate pair is a CAPTURE-PIPELINE failure and it must never be
reportable as evidence about the guest -- hence exit 3, loudly, before any
percentage is printed.

Usage
-----
    python tools/png-delta.py A.png B.png
    python tools/png-delta.py A.png B.png --floor PCT   # PCT from a FRESH control
                                                        # pair whose md5s differ
    python tools/png-delta.py A.png B.png --allow-identical   # escape hatch

Exit status
-----------
    0   delta computed (and, with --floor, delta EXCEEDS the floor)
    1   with --floor: delta is at or below the floor -- inconclusive
    2   could not read a file / sizes differ
    3   BEFORE and AFTER are byte-identical -- not a measurement at all

The geometry and the byte arithmetic are imported from vm-screenshot.py itself,
so the percentages printed here are directly comparable to the ones that tool
prints. They are not a second, subtly different implementation.
"""

import argparse
import hashlib
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_vm_screenshot():
    """Import vm-screenshot.py by path -- the hyphen makes it unimportable."""
    path = os.path.join(HERE, "vm-screenshot.py")
    spec = importlib.util.spec_from_file_location("vm_screenshot", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load %s" % path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main(argv=None):
    p = argparse.ArgumentParser(
        description="Diff two saved vm-screenshot PNGs (never captures).")
    p.add_argument("before", metavar="BEFORE.png")
    p.add_argument("after", metavar="AFTER.png")
    p.add_argument("--floor", type=float, default=None, metavar="PCT",
                   help="idle-noise guest delta from a control pair; exit 1 "
                        "unless this run exceeds it")
    p.add_argument("--allow-identical", action="store_true",
                   help="proceed even when BEFORE and AFTER are byte-identical "
                        "(normally exit 3; only for testing this tool itself)")
    args = p.parse_args(argv)

    try:
        vms = load_vm_screenshot()
    except Exception as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    # Identity check FIRST, before any percentage can be printed. A duplicate
    # capture is a failure of the capture pipeline, not a null result about the
    # guest, and the two are indistinguishable once you only look at 0.000%.
    try:
        with open(args.before, "rb") as fh:
            before_md5 = hashlib.md5(fh.read()).hexdigest()
        with open(args.after, "rb") as fh:
            after_md5 = hashlib.md5(fh.read()).hexdigest()
    except OSError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    if before_md5 == after_md5:
        if not args.allow_identical:
            print("before : %s" % args.before)
            print("after  : %s" % args.after)
            print("md5    : %s (BOTH FILES)" % before_md5)
            print("")
            print("error: BEFORE and AFTER are BYTE-IDENTICAL. This is not a "
                  "measurement.", file=sys.stderr)
            print("       A 0.000% delta here would be arithmetic on one "
                  "frame counted twice,", file=sys.stderr)
            print("       not evidence that the guest did not change. Fix the "
                  "capture instead:", file=sys.stderr)
            print("         * did vm-screenshot.py actually run twice, with "
                  "the action between?", file=sys.stderr)
            print("         * is this a pre-2026-08-08 PNG that --compare "
                  "clobbered? (that overwrite is fixed now --", file=sys.stderr)
            print("           see derived_frame_path() in "
                  "vm-screenshot.py -- but old files may still be "
                  "clobbered frames)", file=sys.stderr)
            print("         * did a copy/rename produce two names for one "
                  "capture?", file=sys.stderr)
            print("       Re-capture, verify the md5s DIFFER, then diff "
                  "again.", file=sys.stderr)
            return 3
        print("warning: identical inputs, continuing only because "
              "--allow-identical was passed", file=sys.stderr)

    from PIL import Image
    try:
        before = Image.open(args.before)
        before.load()
        after = Image.open(args.after)
        after.load()
    except Exception as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    if before.size != after.size:
        print("error: sizes differ %s vs %s -- window was resized, "
              "delta is meaningless" % (before.size, after.size),
              file=sys.stderr)
        return 2

    w, h = before.size
    # Same box vm-screenshot.py:513 uses, so the numbers line up. The whole
    # window is a trap: VMware's own title bar and the host clock live there.
    guest_box = (2, 60, max(3, w - 2), max(61, h - 20))

    guest_pct = vms.frame_delta(before, after, guest_box)
    window_pct = vms.frame_delta(before, after)

    print("before : %s" % args.before)
    print("after  : %s" % args.after)
    print("size   : %dx%d   guest box %s" % (w, h, guest_box))

    if guest_pct is None:
        print("delta  : unknown")
        return 2

    print("delta  : guest %.3f%%, whole window %.3f%%" % (guest_pct, window_pct))

    if args.floor is None:
        return 0

    if guest_pct > args.floor:
        print("verdict: %.3f%% EXCEEDS idle floor %.3f%% -- change is real"
              % (guest_pct, args.floor))
        return 0

    print("verdict: %.3f%% is at/below idle floor %.3f%% -- INCONCLUSIVE, "
          "this is what an untouched desktop does on its own"
          % (guest_pct, args.floor))
    return 1


if __name__ == "__main__":
    sys.exit(main())
