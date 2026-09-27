#!/usr/bin/env python3
"""
_input-block-probe.py -- decide, from positive evidence only, whether host
input injection is usable right now.

Why this exists
---------------
On 2026-08-07 vm-type.py printed `result : injected` while nothing reached the
guest. Three wrong diagnoses were reached in a row before this probe existed:

  1. "the coordinates are off"      -- they were, but that was not the blocker
  2. "UIPI / integrity level"       -- ruled out: vmware.exe and python.exe are
                                       BOTH medium IL
  3. "the physical cursor is frozen, so input is dead host-wide"
                                    -- FALSIFIED. Six samples over three
                                       seconds showed (1869,548) every time,
                                       and the very next probe run found the
                                       cursor at (335,448). A short static
                                       window is an IDLE cursor, not a blocked
                                       one. This is the same one-directional
                                       -evidence error as reading an unchanged
                                       framebuffer as a hung VM.

So this probe reports two independent things and never conflates them:

  * BLOCK STATE  -- BlockInput(FALSE) returning FALSE with GetLastError == 5
    (ACCESS_DENIED) means another thread holds BlockInput(TRUE); only that
    thread can release it. Any other result means we are not blocked that way.

  * DELIVERY     -- whether an injected event actually had an effect, measured
    by a post-condition (cursor position changed / GetLastInputInfo advanced),
    never by SendInput's return value. SendInput returning sent=1 only means
    the events were accepted into the API.

Usage
-----
    python _input-block-probe.py            # one pass
    python _input-block-probe.py --watch 12 # sample for ~12s, catch a transient

Exit status
-----------
    0   injection is DELIVERING (safe for vm-type.py)
    1   injection is NOT delivering
    2   could not probe (not Windows)
"""

import argparse
import ctypes
import sys
import time
from ctypes import wintypes

ERROR_ACCESS_DENIED = 5

SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_ulonglong)]


class _IU(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _IU)]


class Probe:
    def __init__(self):
        if sys.platform != "win32":
            raise RuntimeError("Windows only")
        self.u = ctypes.windll.user32
        self.k = ctypes.windll.kernel32
        self.u.SetProcessDPIAware()
        self.u.BlockInput.argtypes = (wintypes.BOOL,)
        self.u.BlockInput.restype = wintypes.BOOL
        self.u.SetCursorPos.argtypes = (ctypes.c_int, ctypes.c_int)
        self.u.SetCursorPos.restype = wintypes.BOOL

    def pos(self):
        p = POINT()
        self.u.GetCursorPos(ctypes.byref(p))
        return (p.x, p.y)

    def last_input(self):
        li = LASTINPUTINFO()
        li.cbSize = ctypes.sizeof(LASTINPUTINFO)
        self.u.GetLastInputInfo(ctypes.byref(li))
        return li.dwTime

    def block_state(self):
        """Return (held_by_other, ret, err)."""
        self.k.SetLastError(0)
        ret = bool(self.u.BlockInput(False))
        err = self.k.GetLastError()
        return (not ret and err == ERROR_ACCESS_DENIED), ret, err

    def inject_move(self, x, y):
        """Absolute move normalised across the VIRTUAL desktop, not the primary
        display -- the primary-screen form silently lands elsewhere on a
        multi-monitor host."""
        vx = self.u.GetSystemMetrics(SM_XVIRTUALSCREEN)
        vy = self.u.GetSystemMetrics(SM_YVIRTUALSCREEN)
        vw = self.u.GetSystemMetrics(SM_CXVIRTUALSCREEN)
        vh = self.u.GetSystemMetrics(SM_CYVIRTUALSCREEN)
        nx = int((x - vx) * 65535 / max(1, vw - 1))
        ny = int((y - vy) * 65535 / max(1, vh - 1))
        inp = INPUT(type=0, u=_IU(mi=MOUSEINPUT(
            dx=nx, dy=ny, mouseData=0,
            dwFlags=MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
            time=0, dwExtraInfo=0)))
        self.k.SetLastError(0)
        sent = self.u.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        return sent, self.k.GetLastError()

    def delivery_test(self):
        """Inject a move to a target 40px away and check the POST-CONDITION.

        Delivery means the cursor landed ON THE TARGET, not merely that it
        moved. That distinction is not pedantry -- it is a defect this probe
        actually shipped. On 2026-08-07 an unelevated run reported
        DELIVERED=True for `(1929,682) -> (1985,718)` against a target of
        `(1985,682)`: the operator moved the physical mouse inside the ~0.3 s
        sample window, the position differed, and `after != start` scored it as
        a pass. It nearly overturned the (correct) elevation finding on one
        reading. An absolute injected move cannot drift in Y -- if the landing
        point is not the target, something else moved the pointer.

        Tolerance is 1 px per axis: absolute coordinates are quantised through
        a 0..65535 normalisation, so the exact pixel can be off by one on some
        virtual-desktop sizes. Anything beyond that is physical input.

        The cursor is restored afterwards so the probe does not leave the
        operator's pointer parked somewhere odd.

        THE RESIDUAL LIMITATION -- read this before treating one run as final.
        The fix above traded a false POSITIVE for a false NEGATIVE, and the
        false negative is still here. `delivered = on_target` and
        `physical = moved and not on_target`, so if injection IS working and the
        operator moves the physical mouse inside the same ~0.3 s window, the
        landing point will not be the target and the run scores
        `physical_input=True, DELIVERED=False`. Working injection, reported as
        broken.

        That trade is deliberate and correct for this project: a false "not
        delivering" costs one rerun, a false "delivering" costs a wrong
        diagnosis and already nearly overturned the elevation finding. So it is
        recorded, not fixed.

        The consequence is operational: **a single NOT-delivering pass taken
        while somebody is using the mouse is not conclusive.** Run at least
        three passes (or `--watch`) and look for agreement. The three-pass habit
        is load-bearing, not ceremony.

        How to tell the two apart in the output: on genuine non-delivery the
        cursor does not move at all, so `after == start` and
        `landed - target` is exactly the +/-40 px offset the target was built
        from. Under concurrent physical input `after` is somewhere unrelated to
        both. (Reading that +40 as a wrong-axis bug is itself a documented
        mistake -- it was made and retracted on 2026-08-07.)
        """
        start = self.pos()
        li0 = self.last_input()
        target = (start[0] - 40 if start[0] > 100 else start[0] + 40, start[1])
        sent, err = self.inject_move(*target)
        time.sleep(0.3)
        after = self.pos()
        li1 = self.last_input()
        on_target = (abs(after[0] - target[0]) <= 1
                     and abs(after[1] - target[1]) <= 1)
        moved = after != start
        # Moved, but not to where we aimed -> the operator's hand, not us.
        physical = moved and not on_target
        delivered = on_target
        if moved:
            self.inject_move(*start)
        return {
            "start": start, "target": target, "after": after,
            "sent": sent, "err": err,
            "lastinput_advanced": li1 != li0,
            "moved": moved,
            "on_target": on_target,
            "physical_input": physical,
            "delivered": delivered,
        }


def run(watch_seconds):
    p = Probe()
    deadline = time.time() + max(0.0, watch_seconds)
    positions = []
    verdict = False
    first = True

    while first or time.time() < deadline:
        first = False
        held, ret, err = p.block_state()
        d = p.delivery_test()
        positions.append(d["start"])
        print("block: held_by_other=%-5s (BlockInput(F) ret=%s err=%d)"
              % (held, ret, err))
        print("  inject target=%s -> landed=%s   sent=%d err=%d  "
              "lastinput_advanced=%s  on_target=%s  DELIVERED=%s"
              % (d["target"], d["after"], d["sent"], d["err"],
                 d["lastinput_advanced"], d["on_target"], d["delivered"]))
        if d["physical_input"]:
            # Not a pass and not a fail -- the sample is contaminated. Saying
            # so out loud beats scoring it either way.
            print("  PHYSICAL INPUT DETECTED: the cursor moved but did NOT "
                  "land on the target, so something outside this probe moved "
                  "it. Sample discarded; keep your hands off the mouse.")
        if d["delivered"]:
            verdict = True
            break
        if time.time() < deadline:
            time.sleep(1.0)

    moved_on_its_own = len(set(positions)) > 1
    print("---")
    print("cursor samples      : %s" % (positions,))
    print("cursor changed      : %s  (physical input observed = %s)"
          % (moved_on_its_own, moved_on_its_own))
    print("VERDICT             : injection is %s"
          % ("DELIVERING" if verdict else "NOT delivering"))
    if not verdict:
        print("  Do not read vm-type.py's 'N event group(s) accepted by "
              "SendInput' line as delivery while this says NOT delivering -- "
              "SendInput accepting events is not delivery. (It printed "
              "'result : injected' before 2026-08-07; that claim was removed "
              "because it was measurably false at Medium integrity.)")
    return 0 if verdict else 1


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Report whether host input injection actually delivers.")
    ap.add_argument("--watch", type=float, default=0.0, metavar="SEC",
                    help="keep sampling for SEC seconds until delivery is "
                         "observed (default: single pass)")
    args = ap.parse_args(argv)
    try:
        return run(args.watch)
    except RuntimeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
