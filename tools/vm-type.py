#!/usr/bin/env python3
"""
vm-type.py -- drive a VMware Workstation guest from the host by synthesising
keyboard and mouse input into its window.

Why this exists
---------------
On 2026-08-07 the Sonoma guest reached a macOS desktop with:

  * `vmrun checkToolsState` -> `unknown` (no VMware Tools, so no runProgramInGuest)
  * TCP 22 / 5900 / 548 all closed (no SSH, no VNC, no AFP)
  * ping 192.168.255.129 replying, host ARP populated (the guest IS alive)

STALE -- the first bullet is no longer true, and the reason this tool exists has
changed. Later the same day, after the install finished and the guest mounted
`darwin.iso` itself:

  * `vmrun checkToolsState` -> `running` (exit 0), reproduced at 23:15/16/17Z
    with no new heartbeat timeout in that window. The preserved log
    `vmware-PREBOOT-20260807T2100Z.log` records
    `toolbox-dnd: Version: 12.1.1.12 (build-22484250)` and a live heartbeat.
  * Guest ops still fail, but with `VIX_E_ANON_GUEST_OPERATIONS_PROHIBITED` --
    that is a MISSING `-gu`/`-gp` LOGIN, not absent Tools. `runProgramInGuest`
    is blocked by authentication, and so is `captureScreen`.
  * TCP 22 / 5900 return `Connection refused` (an RST, not a timeout), which
    rules out filtering: the stack answered and nothing is bound to those ports.

So Tools is not the blocker and never was after ~21:00Z; operator credentials
are. Do not plan work off the first bullet list. See `handoff3.md` sections 2-4.

Separately: this tool does NOT currently deliver input. Unelevated `SendInput`
reports `1 event group accepted` while the guest framebuffer delta measures
0.000% against a measured 0.000% idle floor. Read `_vm-type-elevated.cmd`;
elevation requires interactive UAC consent.

There was no way to run a single command inside the guest. `vm-screenshot.py`
can *read* the screen; nothing could *write* to it. This is the write half.

How it works
------------
Win32 `SendInput` posts hardware-level input to the foreground window. VMware
Workstation forwards keyboard/mouse to the guest when its VM view has focus, so
the sequence is always: focus the VM window, click once inside the guest area to
give the VM view keyboard grab, then inject.

`SendInput` (not `PostMessage`/`SendMessage`): VMware's guest view does not
process synthetic WM_KEYDOWN messages -- it reads raw input. Message-posting
looks like it works (the calls succeed) and nothing reaches the guest, which is
the same class of silent-wrong-answer the screenshot tool was written to kill.

Scancodes, not virtual keys
---------------------------
Every key event is sent with KEYEVENTF_SCANCODE and a hardware scancode. The
guest is macOS: it maps scancodes through its own keyboard layout, and the host
virtual-key -> guest character path goes wrong for punctuation. Scancodes are
what a real PS/2 keyboard would put on the wire, so the guest sees exactly what
a physical keypress would produce.

Safety
------
This moves the user's mouse and steals focus for the duration of the injection.
It refuses to run against a minimized window (SW_RESTORE first, same guard as
vm-screenshot.py) and refuses an ambiguous --window pattern rather than typing
into the wrong VM -- typing a shell command into the wrong machine is worse
than any capture bug.

Usage
-----
    python vm-type.py --list
    python vm-type.py --window "Sonoma" --click
    python vm-type.py --window "Sonoma" --text "ifconfig -a" --enter
    python vm-type.py --window "Sonoma" --key cmd+space --text "Terminal" --enter
    python vm-type.py --window "Sonoma" --key esc --delay 0.05

Exit status
-----------
    0   injection completed
    2   could not target the window (none, ambiguous, minimized, not Windows)
    3   an argument was unparseable (unknown key name / untypable character)

Requires: Windows. No third-party packages.
"""

import argparse
import ctypes
import re
import sys
import time
from ctypes import wintypes


# --- Win32 SendInput ---------------------------------------------------------
#
# Declared by hand for the same reason vm-screenshot.py does it: tools/ stays
# dependency-light, and this needs a handful of calls.

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1

KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_ABSOLUTE = 0x8000

SM_CXSCREEN = 0
SM_CYSCREEN = 1

# Virtual-desktop metrics. MOUSEEVENTF_ABSOLUTE normalises across these, not
# across the primary display -- see _Win32.click.
SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79

ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 \
    else ctypes.c_ulong


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ULONG_PTR)]


class _INPUTunion(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTunion)]


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


# --- scancode tables ---------------------------------------------------------
#
# Set 1 (XT) make codes. The guest receives these as if from a PS/2 keyboard.
# Break codes are the same value with KEYEVENTF_KEYUP.

UNSHIFTED = {
    "a": 0x1E, "b": 0x30, "c": 0x2E, "d": 0x20, "e": 0x12, "f": 0x21,
    "g": 0x22, "h": 0x23, "i": 0x17, "j": 0x24, "k": 0x25, "l": 0x26,
    "m": 0x32, "n": 0x31, "o": 0x18, "p": 0x19, "q": 0x10, "r": 0x13,
    "s": 0x1F, "t": 0x14, "u": 0x16, "v": 0x2F, "w": 0x11, "x": 0x2D,
    "y": 0x15, "z": 0x2C,
    "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06, "6": 0x07,
    "7": 0x08, "8": 0x09, "9": 0x0A, "0": 0x0B,
    "-": 0x0C, "=": 0x0D, "[": 0x1A, "]": 0x1B, "\\": 0x2B, ";": 0x27,
    "'": 0x28, "`": 0x29, ",": 0x33, ".": 0x34, "/": 0x35,
    " ": 0x39, "\t": 0x0F, "\n": 0x1C,
}

# Characters that are the shifted face of a key above.
SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
    "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "-", "+": "=", "{": "[", "}": "]", "|": "\\", ":": ";",
    '"': "'", "~": "`", "<": ",", ">": ".", "?": "/",
}
for _c in "abcdefghijklmnopqrstuvwxyz":
    SHIFTED[_c.upper()] = _c

# Named keys usable with --key. macOS aliases included because the guest is a
# Mac: on VMware's default mapping the host Windows/Super key arrives as Command
# and Alt arrives as Option.
NAMED = {
    "esc": 0x01, "escape": 0x01,
    "enter": 0x1C, "return": 0x1C, "ret": 0x1C,
    "tab": 0x0F, "space": 0x39, "backspace": 0x0E, "bs": 0x0E,
    "delete": 0x53, "del": 0x53,
    "up": 0x48, "down": 0x50, "left": 0x4B, "right": 0x4D,
    "home": 0x47, "end": 0x4F, "pgup": 0x49, "pgdn": 0x51,
    "f1": 0x3B, "f2": 0x3C, "f3": 0x3D, "f4": 0x3E, "f5": 0x3F,
    "f6": 0x40, "f7": 0x41, "f8": 0x42, "f9": 0x43, "f10": 0x44,
    "f11": 0x57, "f12": 0x58,
}

MODIFIERS = {
    "shift": 0x2A,
    "ctrl": 0x1D, "control": 0x1D,
    "alt": 0x38, "option": 0x38, "opt": 0x38,
    "cmd": 0x5B, "command": 0x5B, "win": 0x5B, "super": 0x5B, "meta": 0x5B,
}

# Scancodes that need the E0 prefix -- SendInput expresses that as
# KEYEVENTF_EXTENDEDKEY. Getting this wrong turns Delete into keypad-period and
# the arrow keys into keypad digits.
EXTENDED = {0x53, 0x48, 0x50, 0x4B, 0x4D, 0x47, 0x4F, 0x49, 0x51, 0x5B}


class _Win32:
    """Confines ctypes.windll to one place, mirroring vm-screenshot.py."""

    def __init__(self):
        if sys.platform != "win32":
            raise RuntimeError("vm-type.py only works on Windows "
                               "(VMware Workstation host)")
        self.user32 = ctypes.windll.user32
        self.kernel32 = ctypes.windll.kernel32
        self.user32.SetProcessDPIAware()
        self.user32.SendInput.argtypes = (wintypes.UINT,
                                          ctypes.POINTER(INPUT), ctypes.c_int)
        self.user32.SendInput.restype = wintypes.UINT

        # focus() attaches to the foreground thread's input queue. Declaring
        # these matters on 64-bit: GetWindowThreadProcessId returns a DWORD
        # thread id, and without a restype ctypes truncates it to a signed
        # int, so AttachThreadInput would be handed a bogus tid and simply
        # refuse -- silently, since it reports failure by returning false.
        self.user32.GetForegroundWindow.restype = wintypes.HWND
        self.user32.GetWindowThreadProcessId.argtypes = (
            wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
        self.user32.GetWindowThreadProcessId.restype = wintypes.DWORD
        self.user32.AttachThreadInput.argtypes = (wintypes.DWORD,
                                                  wintypes.DWORD,
                                                  wintypes.BOOL)
        self.user32.AttachThreadInput.restype = wintypes.BOOL
        self.kernel32.GetCurrentThreadId.restype = wintypes.DWORD
        self.user32.SetForegroundWindow.argtypes = (wintypes.HWND,)
        self.user32.BringWindowToTop.argtypes = (wintypes.HWND,)

    # -- window plumbing (same contract as vm-screenshot.py) --

    def find_windows(self):
        found = []
        proto = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                   ctypes.c_void_p)

        def cb(hwnd, _lparam):
            if not self.user32.IsWindowVisible(hwnd):
                return True
            n = self.user32.GetWindowTextLengthW(hwnd)
            if n <= 0:
                return True
            buf = ctypes.create_unicode_buffer(n + 1)
            self.user32.GetWindowTextW(hwnd, buf, n + 1)
            found.append((hwnd, buf.value))
            return True

        self.user32.EnumWindows(proto(cb), 0)
        return found

    def window_rect(self, hwnd):
        r = RECT()
        if not self.user32.GetWindowRect(hwnd, ctypes.byref(r)):
            raise RuntimeError("GetWindowRect failed for hwnd %s" % hwnd)
        return r

    def is_minimized(self, hwnd):
        return bool(self.user32.IsIconic(hwnd))

    def restore(self, hwnd):
        self.user32.ShowWindow(hwnd, 9)          # SW_RESTORE

    def focus(self, hwnd):
        """Bring `hwnd` to the foreground.

        SetForegroundWindow is refused by Windows when the calling process is
        not itself foreground, and it signals that by returning false rather
        than raising. The caller must verify with `foreground()` -- injecting
        into whatever window actually has focus is how you type a shell command
        into the operator's own terminal.

        A plain SetForegroundWindow is exactly what failed on 2026-08-07 while
        the operator's editor held focus:

            error: could not bring the VM window to the foreground
            (foreground hwnd is 36900544, wanted 67505164)

        `tools/_force-foreground-probe.py` measured five escalations against
        that live refusal. Only **attach** was cleanly demonstrated: it ran
        while a foreign window (the editor, hwnd 36900544) genuinely held the
        foreground and moved it to the VM. The probe's other three "successes"
        -- plain, minimize-trick, switch -- each started their trial with the
        VM window *already* foreground, so they prove nothing; taking what you
        already hold is unfalsifiable. They are UNTESTED, not disproven.

        So: attach to the current foreground thread's input queue, which makes
        Windows treat us as entitled to set the foreground, then set and
        detach. Falls back to the plain call when we already are foreground or
        the attach is refused. Still returns nothing -- the caller verifies.
        """
        self.user32.ShowWindow(hwnd, 9)          # SW_RESTORE

        fg = self.user32.GetForegroundWindow()
        fg_tid = self.user32.GetWindowThreadProcessId(fg, None) if fg else 0
        me = self.kernel32.GetCurrentThreadId()

        attached = False
        if fg_tid and fg_tid != me:
            attached = bool(self.user32.AttachThreadInput(me, fg_tid, True))
        try:
            self.user32.BringWindowToTop(hwnd)
            self.user32.SetForegroundWindow(hwnd)
        finally:
            if attached:
                self.user32.AttachThreadInput(me, fg_tid, False)

    def foreground(self):
        return self.user32.GetForegroundWindow()

    # -- input injection --

    def _send(self, *inputs):
        n = len(inputs)
        arr = (INPUT * n)(*inputs)
        sent = self.user32.SendInput(n, arr, ctypes.sizeof(INPUT))
        if sent != n:
            err = ctypes.get_last_error()
            raise RuntimeError("SendInput sent %d/%d events (error %s)"
                               % (sent, n, err))

    def key_event(self, scan, keyup=False):
        flags = KEYEVENTF_SCANCODE
        if scan in EXTENDED:
            flags |= KEYEVENTF_EXTENDEDKEY
        if keyup:
            flags |= KEYEVENTF_KEYUP
        return INPUT(type=INPUT_KEYBOARD,
                     u=_INPUTunion(ki=KEYBDINPUT(wVk=0, wScan=scan,
                                                 dwFlags=flags, time=0,
                                                 dwExtraInfo=0)))

    def tap(self, scan, hold=0.0):
        self._send(self.key_event(scan))
        if hold:
            time.sleep(hold)
        self._send(self.key_event(scan, keyup=True))

    def chord(self, mod_scans, scan, hold=0.02):
        """Press modifiers, tap `scan`, release modifiers in reverse order."""
        for m in mod_scans:
            self._send(self.key_event(m))
        time.sleep(hold)
        self.tap(scan)
        time.sleep(hold)
        for m in reversed(mod_scans):
            self._send(self.key_event(m, keyup=True))

    def guest_view(self, hwnd):
        """Find the child HWND that actually renders the guest framebuffer.

        The VMware frame window is not the guest surface. Under it sit a
        toolbar (`VMUICmdbar`), a status bar, and the guest view itself --
        class `VMware.GuestWindow`, which in turn hosts `MKSEmbedded`. On
        2026-08-07 the frame was 1228x881 at 1300,85 while the guest view was
        1024x768 at 1502,176: a fixed `left+2, top+60` chrome offset misses the
        guest surface by ~200x90 px, and every click lands on VMware's own
        chrome or the host desktop. Clicks that miss are silent -- the tool
        reports `injected` and the framebuffer never changes, which is exactly
        the silent-wrong-answer failure vm-screenshot.py was written to kill.

        Returns the guest view RECT, or None if no guest child is present
        (e.g. the VM is powered off and the frame shows the library view).
        """
        hit = []
        proto = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                   ctypes.c_void_p)

        def cb(child, _lparam):
            cls = ctypes.create_unicode_buffer(256)
            self.user32.GetClassNameW(child, cls, 256)
            name = cls.value or ""
            if "GuestWindow" in name or "MKS" in name:
                r = RECT()
                self.user32.GetWindowRect(child, ctypes.byref(r))
                if (r.right - r.left) > 1 and (r.bottom - r.top) > 1:
                    hit.append((child, name, r))
            return True

        self.user32.EnumChildWindows(hwnd, proto(cb), 0)
        if not hit:
            return None
        # Several candidates nest (GuestWindow contains MKSEmbedded contains
        # MKSWindow#0) and they share the same rectangle. Take the largest --
        # the outermost one -- so the rect is the full guest surface.
        hit.sort(key=lambda t: (t[2].right - t[2].left) * (t[2].bottom - t[2].top),
                 reverse=True)
        return hit[0]

    def click(self, x, y):
        """Absolute left-click at screen pixel (x, y).

        MOUSEEVENTF_ABSOLUTE coordinates are 0..65535, and on a multi-monitor
        host they span the **virtual desktop**, not the primary screen. This
        originally normalised against SM_CXSCREEN/SM_CYSCREEN; on a host whose
        virtual desktop is larger than (or offset from) the primary display
        that sends the cursor somewhere else entirely -- asking for 1914,545
        put the pointer at 1442,1144. Normalise against SM_XVIRTUALSCREEN and
        friends, subtracting the virtual origin, which can be negative when a
        monitor sits left of or above the primary.
        """
        vx = self.user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
        vy = self.user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
        sw = self.user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
        sh = self.user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)
        nx = int((x - vx) * 65535 / max(1, sw - 1))
        ny = int((y - vy) * 65535 / max(1, sh - 1))
        move = INPUT(type=INPUT_MOUSE,
                     u=_INPUTunion(mi=MOUSEINPUT(
                         dx=nx, dy=ny, mouseData=0,
                         dwFlags=MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                         time=0, dwExtraInfo=0)))
        down = INPUT(type=INPUT_MOUSE,
                     u=_INPUTunion(mi=MOUSEINPUT(
                         dx=nx, dy=ny, mouseData=0,
                         dwFlags=MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE,
                         time=0, dwExtraInfo=0)))
        up = INPUT(type=INPUT_MOUSE,
                   u=_INPUTunion(mi=MOUSEINPUT(
                       dx=nx, dy=ny, mouseData=0,
                       dwFlags=MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE,
                       time=0, dwExtraInfo=0)))
        self._send(move)
        time.sleep(0.05)
        self._send(down)
        time.sleep(0.05)
        self._send(up)


# --- pure helpers (unit-testable without Windows) ----------------------------

def resolve_click_at(spec, rect):
    """Turn a ``--click-at X,Y`` spec into absolute screen coordinates.

    `spec` is guest-relative -- (0,0) is the top-left of the guest framebuffer,
    not of the VMware window -- so the caller must pass the *view* rect, not the
    window rect. Returns (screen_x, screen_y); raises ValueError with a message
    fit for stderr on bad input.

    Out-of-range is an error, not a clamp. A clamped click still lands
    somewhere, silently, and "somewhere" on this host means VMware's own
    toolbar or another application -- the exact class of silent wrong action
    the ambiguous-window guard exists to prevent. Refusing costs one rerun;
    clicking host chrome costs a wrong diagnosis.
    """
    parts = spec.split(",")
    if len(parts) != 2:
        raise ValueError("--click-at wants X,Y integers")
    try:
        gx, gy = (int(p.strip()) for p in parts)
    except ValueError:
        raise ValueError("--click-at wants X,Y integers")
    w = rect.right - rect.left
    h = rect.bottom - rect.top
    if not (0 <= gx < w and 0 <= gy < h):
        raise ValueError("--click-at %d,%d is outside the %dx%d guest "
                         "surface -- that click would land on host chrome"
                         % (gx, gy, w, h))
    return (rect.left + gx, rect.top + gy)


def pick_window(windows, pattern):
    """Choose the window whose title matches `pattern`.

    Ambiguity is an error, never a guess. vm-screenshot.py refuses to *capture*
    the wrong VM; typing into the wrong VM is strictly worse, so the same rule
    applies here.
    """
    rx = re.compile(re.escape(pattern), re.I)
    hits = [(h, t) for h, t in windows if rx.search(t)]
    if not hits:
        titles = [t for _, t in windows if "vmware" in t.lower()]
        hint = ("  VMware windows currently open:\n    "
                + "\n    ".join(titles)) if titles else \
               "  No VMware windows are open at all -- is the VM running?"
        raise RuntimeError("no window title matches %r\n%s" % (pattern, hint))
    if len(hits) > 1:
        listing = "\n    ".join("%s (hwnd %s)" % (t, h) for h, t in hits)
        raise RuntimeError("%d windows match %r -- narrow it with --window:"
                           "\n    %s" % (len(hits), pattern, listing))
    return hits[0]


def char_to_scan(ch):
    """Return (scancode, needs_shift) for `ch`, or raise for untypable input.

    Raising beats silently dropping a character: a shell command that arrives
    with one character missing runs and does the wrong thing.
    """
    if ch in UNSHIFTED:
        return UNSHIFTED[ch], False
    if ch in SHIFTED:
        return UNSHIFTED[SHIFTED[ch]], True
    raise ValueError("cannot type %r with a US scancode keyboard "
                     "(non-ASCII or unmapped)" % ch)


def parse_key(spec):
    """Parse 'cmd+shift+g' into ([mod scancodes], key scancode).

    Case matters on the *final* part and only there. Modifier names and named
    keys are matched case-insensitively ('CMD+Enter' works), but a single
    character face is passed to char_to_scan verbatim, so '--key Q' means
    shift+q and '--key q' means q. Lowercasing the whole spec -- which this
    function used to do -- made the shifted branch below unreachable for
    letters: every '--key Q' silently typed 'q', and nothing in the output
    said so.
    """
    parts = [p.strip() for p in spec.split("+") if p.strip()]
    if not parts:
        raise ValueError("empty --key spec")
    *mods, key = parts
    mod_scans = []
    for m in mods:
        m = m.lower()
        if m not in MODIFIERS:
            raise ValueError("unknown modifier %r (known: %s)"
                             % (m, ", ".join(sorted(set(MODIFIERS)))))
        mod_scans.append(MODIFIERS[m])
    if key.lower() in NAMED:
        return mod_scans, NAMED[key.lower()]
    if len(key) == 1:
        scan, shifted = char_to_scan(key)
        if shifted and MODIFIERS["shift"] not in mod_scans:
            mod_scans.append(MODIFIERS["shift"])
        return mod_scans, scan
    raise ValueError("unknown key %r (known names: %s)"
                     % (key, ", ".join(sorted(NAMED))))


def guest_center(rect, top_chrome=60, bottom_chrome=20):
    """Screen pixel at the middle of the guest area of a VMware window.

    Same chrome offsets as vm-screenshot.py's crop_chrome: clicking in the
    toolbar would hit a VMware button instead of giving the guest focus.
    """
    x = (rect.left + rect.right) // 2
    y = (rect.top + top_chrome + rect.bottom - bottom_chrome) // 2
    return x, y


# --- main --------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        description="Inject keystrokes and clicks into a VMware guest window.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="exit 0 = injected, 2 = could not target a window, "
               "3 = bad key/text argument")
    p.add_argument("--window", default="VMware Workstation",
                   help="substring of the window title "
                        "(default: 'VMware Workstation')")
    p.add_argument("--text", action="append", default=[],
                   help="type this literal text; repeatable, in order with "
                        "--key")
    p.add_argument("--key", action="append", default=[], metavar="SPEC",
                   help="press a key or chord, e.g. 'enter', 'esc', "
                        "'cmd+space', 'ctrl+c'; repeatable")
    p.add_argument("--enter", action="store_true",
                   help="press Return after everything else")
    p.add_argument("--click", action="store_true",
                   help="click the centre of the guest area first (gives the "
                        "VM view keyboard grab)")
    p.add_argument("--click-at", metavar="X,Y",
                   help="click at these guest-relative pixels instead of the "
                        "centre")
    p.add_argument("--delay", type=float, default=0.012, metavar="SEC",
                   help="pause between keystrokes (default 0.012)")
    p.add_argument("--settle", type=float, default=0.4, metavar="SEC",
                   help="pause after focusing/clicking before typing "
                        "(default 0.4)")
    p.add_argument("--dry-run", action="store_true",
                   help="resolve the window and parse every action, but "
                        "inject nothing")
    p.add_argument("--out", metavar="PATH",
                   help="also write this run's report to PATH. REQUIRED when "
                        "launched elevated: an elevated child shares no stdout "
                        "with the launching shell, so Start-Process -Verb "
                        "RunAs returns only a pid and every line printed here "
                        "is lost. Added 2026-08-07 after tools/README.md and "
                        "handoff2.md documented this flag before it existed.")
    p.add_argument("--list", action="store_true",
                   help="list visible window titles and exit")
    return p


def _ordered_actions(argv, args):
    """Interleave --text and --key in the order they appeared on the CLI.

    argparse keeps two independent lists, which loses the ordering between
    them -- and `--key cmd+space --text Terminal` means something completely
    different from the reverse. Re-derive the order from argv.
    """
    actions = []
    texts = list(args.text)
    keys = list(args.key)
    for tok in argv:
        if tok == "--text" and texts:
            actions.append(("text", texts.pop(0)))
        elif tok == "--key" and keys:
            actions.append(("key", keys.pop(0)))
    # Anything argparse saw that argv scanning missed (e.g. --text=foo form).
    for t in texts:
        actions.append(("text", t))
    for k in keys:
        actions.append(("key", k))
    return actions


class _Tee:
    """Write to a stream and also keep a copy.

    Exists so `--out` captures *everything* this tool prints, including the
    stderr `error:` lines and the delivery caveat, without touching the two
    dozen individual print() calls (and without a later edit silently missing
    one).
    """

    def __init__(self, stream, sink):
        self._stream = stream
        self._sink = sink

    def write(self, data):
        self._sink.append(data)
        return self._stream.write(data)

    def flush(self):
        self._stream.flush()

    def isatty(self):
        return getattr(self._stream, "isatty", lambda: False)()


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    args = build_parser().parse_args(argv)

    if not args.out:
        return _run(args, argv)

    # The whole point of --out is the elevated case, where this process has no
    # stdout the launcher can read. So the exit code goes INTO the file too --
    # `Start-Process -Verb RunAs` surfaces a pid, and a caller who only has the
    # file would otherwise have no way to tell success from exit 2.
    captured = []
    real_out, real_err = sys.stdout, sys.stderr
    sys.stdout = _Tee(real_out, captured)
    sys.stderr = _Tee(real_err, captured)
    code = 1
    try:
        code = _run(args, argv)
    finally:
        sys.stdout, sys.stderr = real_out, real_err
        captured.append("exit   : %d\n" % code)
        try:
            with open(args.out, "w", encoding="utf-8") as fh:
                fh.write("".join(captured))
        except OSError as exc:
            print("warn   : could not write --out %s: %s" % (args.out, exc),
                  file=sys.stderr)
    return code


def _run(args, argv):
    try:
        win32 = _Win32()
    except RuntimeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    if args.list:
        # Window titles are arbitrary user text (browser tabs, chat clients) and
        # routinely contain characters the Windows console codepage cannot
        # encode. A UnicodeEncodeError here would kill the one command whose
        # entire job is telling the operator what --window to pass, so titles
        # are degraded to the stream's encoding rather than allowed to raise.
        enc = getattr(sys.stdout, "encoding", None) or "ascii"
        for hwnd, title in win32.find_windows():
            safe = title.encode(enc, "replace").decode(enc, "replace")
            print("%-12s %s" % (hwnd, safe))
        return 0

    try:
        hwnd, title = pick_window(win32.find_windows(), args.window)
    except RuntimeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    print("window : %s (hwnd %s)" % (title, hwnd))

    actions = []
    for kind, value in _ordered_actions(argv, args):
        if kind == "text":
            try:
                for ch in value:
                    char_to_scan(ch)
            except ValueError as exc:
                print("error: %s" % exc, file=sys.stderr)
                return 3
            actions.append(("text", value))
        else:
            try:
                mods, scan = parse_key(value)
            except ValueError as exc:
                print("error: %s" % exc, file=sys.stderr)
                return 3
            actions.append(("key", (value, mods, scan)))
    if args.enter:
        actions.append(("key", ("enter", [], NAMED["enter"])))

    click_xy = None
    if args.click or args.click_at:
        if win32.is_minimized(hwnd):
            win32.restore(hwnd)
            time.sleep(1.0)
            if win32.is_minimized(hwnd):
                print("error: window stayed minimized after SW_RESTORE",
                      file=sys.stderr)
                return 2
        # Resolve the guest framebuffer child window rather than guessing a
        # chrome offset from the frame. A guessed offset put every click ~200x90
        # px off the guest surface on 2026-08-07, silently.
        view = win32.guest_view(hwnd)
        if view is None:
            print("error: no VMware.GuestWindow/MKS child under hwnd %s -- the "
                  "VM view is not showing a guest framebuffer (powered off, or "
                  "the frame is on the library/home tab). Refusing to click at "
                  "a guessed offset." % hwnd, file=sys.stderr)
            return 2
        vhwnd, vcls, rect = view
        print("guest  : %s %dx%d at %d,%d (hwnd %s)"
              % (vcls, rect.right - rect.left, rect.bottom - rect.top,
                 rect.left, rect.top, vhwnd))
        if args.click_at:
            try:
                click_xy = resolve_click_at(args.click_at, rect)
            except ValueError as exc:
                print("error: %s" % exc, file=sys.stderr)
                return 3
        else:
            click_xy = guest_center(rect)
        print("click  : screen %d,%d" % click_xy)

    for kind, value in actions:
        if kind == "text":
            print("type   : %r" % value)
        else:
            name, mods, _scan = value
            print("key    : %s (%d modifier%s)"
                  % (name, len(mods), "" if len(mods) == 1 else "s"))

    if args.dry_run:
        print("dry-run: nothing injected")
        return 0

    if not actions and click_xy is None:
        print("nothing to do -- pass --text, --key, --enter or --click")
        return 0

    if win32.is_minimized(hwnd):
        win32.restore(hwnd)
        time.sleep(1.0)
    win32.focus(hwnd)
    time.sleep(0.25)

    fg = win32.foreground()
    if fg != hwnd:
        print("error: could not bring the VM window to the foreground "
              "(foreground hwnd is %s, wanted %s). Refusing to inject -- "
              "the keystrokes would land in whatever window has focus."
              % (fg, hwnd), file=sys.stderr)
        return 2

    if click_xy is not None:
        win32.click(*click_xy)
    time.sleep(args.settle)

    for kind, value in actions:
        if kind == "text":
            for ch in value:
                scan, shifted = char_to_scan(ch)
                if shifted:
                    win32.chord([MODIFIERS["shift"]], scan)
                else:
                    win32.tap(scan)
                time.sleep(args.delay)
        else:
            _name, mods, scan = value
            if mods:
                win32.chord(mods, scan)
            else:
                win32.tap(scan)
            time.sleep(max(args.delay, 0.08))

    # NOT "result : injected". Measured 2026-08-07: at Medium integrity with a
    # UAC-filtered token, SendInput returns success for every event and the OS
    # discards all of them. 20 consecutive sends were accepted (2/2, err=0)
    # with the VMware window foreground, and a WH_KEYBOARD_LL hook in the same
    # process observed ZERO of them. `_send` above raises only when the OS
    # *refuses* the call, which is a different and rarer failure.
    #
    # So the return value proves the call was accepted, nothing more. Claiming
    # "injected" here is the same class of silent-wrong-answer vm-screenshot.py
    # was written to kill -- and it already cost this project three wrong
    # diagnoses. See tools/README.md on the integrity barrier and
    # tools/_input-block-probe.py for the post-condition check.
    print("result : %d event group(s) accepted by SendInput" % len(actions))
    print("         NOT proof of delivery -- SendInput reports success even")
    print("         when every event is discarded (Medium IL / UAC-filtered).")
    print("         Verify by post-condition: vm-screenshot.py --compare, or")
    print("         re-run this from an ELEVATED terminal.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
