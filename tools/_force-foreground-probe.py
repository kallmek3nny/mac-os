#!/usr/bin/env python3
"""_force-foreground-probe.py -- can this process take the foreground?

WHY THIS EXISTS (2026-08-07)
----------------------------
`vm-type.py` refused to inject because `SetForegroundWindow` returned without
putting the VM window in front:

    error: could not bring the VM window to the foreground
    (foreground hwnd is 36900544, wanted 67505164)

hwnd 36900544 is the operator's editor (Antigravity IDE, pid 49572). Windows
refuses foreground changes from a process that is not itself foreground and
has no recent input -- SetForegroundWindow signals that by returning false,
not by raising. Refusing to inject there was the correct call: the keystrokes
would have gone into the editor.

This probe measures which escalation, if any, actually works. It reports the
POST-CONDITION (GetForegroundWindow == target) after each strategy, never the
API return value. Strategies, cheapest first:

  1. plain           SetForegroundWindow
  2. altkey          synthetic ALT tap first -- the documented unlock: the
                     foreground lock yields to a process that just produced
                     input
  3. attach          AttachThreadInput to the current foreground thread, so
                     we share its input queue and inherit its right to set
                     foreground, then set + detach
  4. minimize-trick  ShowWindow(SW_MINIMIZE) then SW_RESTORE on the target
  5. switch          SwitchToThisWindow(hwnd, TRUE)

Each strategy is verified independently and the foreground is handed back to
whatever held it at start, so running this does not leave the operator's
desktop rearranged.

Exit 0 if at least one strategy took the foreground, 1 if none did.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

user32.GetForegroundWindow.restype = wt.HWND
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.SwitchToThisWindow.argtypes = [wt.HWND, wt.BOOL]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
user32.AttachThreadInput.restype = wt.BOOL
user32.IsIconic.argtypes = [wt.HWND]
user32.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM), wt.LPARAM]

SW_MINIMIZE = 6
SW_RESTORE = 9
SW_SHOW = 5

VK_MENU = 0x12
KEYEVENTF_KEYUP = 0x0002
INPUT_KEYBOARD = 1


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class _IU(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _IU)]


def _safe(s):
    """Window titles are arbitrary Unicode; this console is cp1252. A Braille
    spinner glyph in some other app's title crashed an earlier run of this probe
    inside print(), losing the measurement to an encoding error. Never let a
    title decide whether the probe reports."""
    if s is None:
        return "None"
    return repr(str(s).encode("ascii", "backslashreplace").decode("ascii"))


def title(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, buf, 512)
    return buf.value


def pid_of(hwnd):
    p = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
    return p.value


def find_window(substr):
    hits = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            t = title(hwnd)
            if substr.lower() in t.lower():
                hits.append((hwnd, t))
        return True

    user32.EnumWindows(cb, 0)
    return hits


def alt_tap():
    """Produce real input from this process to release the foreground lock."""
    down = INPUT(type=INPUT_KEYBOARD)
    down.u.ki = KEYBDINPUT(wVk=VK_MENU, wScan=0, dwFlags=0, time=0, dwExtraInfo=None)
    up = INPUT(type=INPUT_KEYBOARD)
    up.u.ki = KEYBDINPUT(wVk=VK_MENU, wScan=0, dwFlags=KEYEVENTF_KEYUP, time=0,
                         dwExtraInfo=None)
    arr = (INPUT * 2)(down, up)
    return user32.SendInput(2, arr, ctypes.sizeof(INPUT))


# -- strategies: each returns a short note; the caller measures the result --

def s_plain(hwnd):
    user32.ShowWindow(hwnd, SW_RESTORE)
    r = user32.SetForegroundWindow(hwnd)
    return "SetForegroundWindow ret=%s" % bool(r)


def s_altkey(hwnd):
    sent = alt_tap()
    time.sleep(0.05)
    user32.ShowWindow(hwnd, SW_RESTORE)
    r = user32.SetForegroundWindow(hwnd)
    return "alt sent=%s SetForegroundWindow ret=%s" % (sent, bool(r))


def s_attach(hwnd):
    fg = user32.GetForegroundWindow()
    fg_tid = user32.GetWindowThreadProcessId(fg, None)
    me = kernel32.GetCurrentThreadId()
    attached = user32.AttachThreadInput(me, fg_tid, True) if fg_tid != me else False
    try:
        user32.ShowWindow(hwnd, SW_RESTORE)
        user32.BringWindowToTop(hwnd)
        r = user32.SetForegroundWindow(hwnd)
    finally:
        if attached:
            user32.AttachThreadInput(me, fg_tid, False)
    return "attach=%s fg_tid=%s SetForegroundWindow ret=%s" % (bool(attached), fg_tid, bool(r))


def s_minimize_trick(hwnd):
    user32.ShowWindow(hwnd, SW_MINIMIZE)
    time.sleep(0.15)
    user32.ShowWindow(hwnd, SW_RESTORE)
    time.sleep(0.15)
    r = user32.SetForegroundWindow(hwnd)
    return "minimize/restore SetForegroundWindow ret=%s" % bool(r)


def s_switch(hwnd):
    user32.SwitchToThisWindow(hwnd, True)
    return "SwitchToThisWindow called"


STRATEGIES = [
    ("plain", s_plain),
    ("altkey", s_altkey),
    ("attach", s_attach),
    ("minimize-trick", s_minimize_trick),
    ("switch", s_switch),
]


def pick_decoy(hwnd):
    """A visible window that is NOT the target, to hold the foreground between
    trials. Without one there is no precondition to steal from."""
    for cand, ctitle in find_window(""):
        if cand != hwnd and ctitle.strip() and not user32.IsIconic(cand):
            return cand, ctitle
    return None, None


def normalize(hwnd):
    """Force the target back to a NON-minimized state and confirm it.

    Why this is not optional: a minimized window follows different foreground
    rules than a restored one. An earlier revision of this probe left the VM
    window minimized (the unstick path below minimizes it), and the very next
    run measured a minimized target and reported the OPPOSITE verdict for the
    `plain` strategy. Every trial must start from the same window state or the
    run is measuring its own leftovers.
    """
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, SW_RESTORE)
        time.sleep(0.25)
    return not bool(user32.IsIconic(hwnd))


def establish_precondition(hwnd, decoy):
    """Put the foreground on `decoy` and CONFIRM it, leaving the target
    restored. Returns True only on positive evidence that some OTHER window
    holds the foreground while the target is non-minimized.

    This is the whole point of the rewrite: if the target is already in front,
    SetForegroundWindow trivially returns True and the post-check trivially
    passes, so the trial measures nothing. A trial is only meaningful when a
    DIFFERENT window demonstrably held the foreground immediately before it --
    and when the target is in the same state every time.
    """
    if not decoy:
        return False
    normalize(hwnd)
    user32.SetForegroundWindow(decoy)
    time.sleep(0.3)
    if user32.GetForegroundWindow() == decoy and not user32.IsIconic(hwnd):
        return True
    # Second attempt: unstick by minimizing the target, then IMMEDIATELY
    # restore it. The restore is mandatory -- skipping it is exactly the leak
    # that corrupted the previous run.
    user32.ShowWindow(hwnd, SW_MINIMIZE)
    time.sleep(0.3)
    user32.SetForegroundWindow(decoy)
    time.sleep(0.2)
    user32.ShowWindow(hwnd, SW_RESTORE)
    time.sleep(0.25)
    user32.SetForegroundWindow(decoy)
    time.sleep(0.3)
    return user32.GetForegroundWindow() == decoy and not user32.IsIconic(hwnd)


def main():
    substr = sys.argv[1] if len(sys.argv) > 1 else "Sonoma"
    hits = find_window(substr)
    if not hits:
        print("error: no visible window matching %r" % substr, file=sys.stderr)
        return 3
    hwnd, wtitle = hits[0]

    original_fg = user32.GetForegroundWindow()
    decoy, dtitle = pick_decoy(hwnd)
    print("target      : hwnd %s %s (pid %s)" % (hwnd, _safe(wtitle), pid_of(hwnd)))
    print("foreground  : hwnd %s %s (pid %s)"
          % (original_fg, _safe(title(original_fg)), pid_of(original_fg)))
    print("iconic      : %s" % bool(user32.IsIconic(hwnd)))
    print("decoy       : hwnd %s %s (pid %s)"
          % (decoy, _safe(dtitle), pid_of(decoy) if decoy else None))
    if original_fg == hwnd:
        print("note        : target was ALREADY foreground at startup -- every")
        print("              trial below forces the decoy in front first, so a")
        print("              pass means a real steal, not a no-op.")
    print("---")

    if not decoy:
        print("---")
        print("WORKING     : INVALID (no decoy window available; precondition")
        print("              'some other window holds the foreground' could not")
        print("              be established, so no trial can prove a steal)")
        return 2

    winners = []
    invalid = []
    for name, fn in STRATEGIES:
        # A trial only means something if a DIFFERENT window demonstrably held
        # the foreground first, with the target in a known (non-minimized)
        # state. No confirmed precondition -> report INVALID, never a pass.
        if not establish_precondition(hwnd, decoy):
            invalid.append(name)
            print("%-15s %-58s -> PRECONDITION_FAILED TOOK_FOREGROUND=INVALID"
                  % (name, "could not park fg on decoy with target restored"))
            continue
        before = user32.GetForegroundWindow()
        icon_before = bool(user32.IsIconic(hwnd))
        note = fn(hwnd)
        time.sleep(0.35)
        got = user32.GetForegroundWindow()
        ok = got == hwnd
        if ok:
            winners.append(name)
        print("%-15s %-58s -> from=%s iconic_pre=%s fg=%s TOOK_FOREGROUND=%s"
              % (name, note, before, icon_before, got, ok))
        # Strategies mutate window state (minimize-trick minimizes). Undo it
        # now so trial N+1 does not inherit trial N's leftovers.
        normalize(hwnd)

    # Hand the desktop back the way we found it: state FIRST, then foreground.
    # SetForegroundWindow alone does not un-minimize, which is how the previous
    # revision poisoned the following run.
    normalize(hwnd)
    if original_fg:
        user32.SetForegroundWindow(original_fg)
        time.sleep(0.2)
    left_iconic = bool(user32.IsIconic(hwnd))

    print("---")
    print("WORKING     : %s" % (", ".join(winners) if winners else "NONE"))
    if invalid:
        print("INVALID     : %s (no precondition -- these tested nothing)"
              % ", ".join(invalid))
    print("EXIT_STATE  : target iconic=%s (must be False, else the next run is"
          " contaminated)" % left_iconic)
    if left_iconic:
        print("warning: target left MINIMIZED -- treat the next run as suspect",
              file=sys.stderr)
    if not winners:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
