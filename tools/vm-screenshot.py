#!/usr/bin/env python3
"""
vm-screenshot.py -- capture a VMware Workstation guest screen on Windows and
say, in words, what is on it.

Why this exists
---------------
"Is the VM actually booting?" was answered four times in this project by
someone squinting at a window, and once -- 2026-08-07 -- by a screenshot that
turned out to be *the host desktop*, not the guest.

The trap: `Graphics.CopyFromScreen` copies the framebuffer of the **monitor**
over the region where a window happens to sit. Anything overlapping that
region -- a terminal, an editor, a notification -- lands in the PNG instead of
the VM. The capture looks plausible. It is wrong. Five consecutive captures
were taken that way before OCR revealed they contained the operator's own
shell prompt.

The fix is `PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)`. The window is asked
to render *itself* into an offscreen DC, so occlusion, other monitors, and even
a minimized-but-restorable window do not matter.

On top of that: a bare PNG still needs a human to interpret. `--expect` turns
the capture into a pass/fail check by OCR-ing the guest area and looking for
phrases, so "the installer reached the macOS UI" becomes a command that exits
0 or 1 instead of an opinion.

What it does NOT do
-------------------
This does not talk to the guest. `vmrun captureScreen` would, but it requires
`VixVM_LoginInGuest` -- useless before an OS is installed, which is exactly
when you most need to see the screen. Everything here is host-side and works
against a VM sitting at a firmware prompt.

Usage
-----
    python vm-screenshot.py --out shot.png
    python vm-screenshot.py --out shot.png --ocr
    python vm-screenshot.py --expect "Reinstall macOS" --expect "Disk Utility"
    python vm-screenshot.py --window "Sonoma" --out shot.png --crop-chrome
    python vm-screenshot.py --settle 15 --compare motion.png
        (writes motion.frame1.png and motion.frame2.png; motion.png untouched.
         Asks "is the screen moving right now" -- it canNOT attribute a change
         to an action, because it captures both frames itself. For that:
           --out before.png ; <do the action> ; --out after.png
           python tools/png-delta.py before.png after.png --floor <pct>)

Exit status
-----------
    0   capture succeeded (and, with --expect, OCR read every phrase)
    1   capture succeeded but OCR did not READ an --expect phrase
    2   could not capture (no window, PrintWindow failed, not on Windows)

Read exit 1 in ONE DIRECTION. It means OCR failed to read the phrase, not that
the phrase is absent from the guest screen -- Tesseract misreads antialiased
macOS text routinely. Exit 0 is the strong direction: OCR cannot invent a phrase
that is not there. With --ocr and no --expect, a chrome-only capture prints
`result : INCONCLUSIVE` (it printed `FAIL` until 2026-08-07, and that word was
read as "the guest is gone" while the OCR dump directly above it listed macOS
menu items). Open the PNG before concluding anything about the VM.

Requires: Windows, Pillow. OCR (--ocr/--expect) additionally needs pytesseract
and a Tesseract binary; without them those flags fail with a clear message
rather than silently passing.
"""

import argparse
import ctypes
import hashlib
import os
import re
import sys
import time


# --- Win32 -----------------------------------------------------------------
#
# Declared by hand rather than pulling in pywin32: this tool wants to be as
# dependency-light as the rest of tools/, and it needs exactly six calls.

PW_RENDERFULLCONTENT = 0x00000002


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class _Win32:
    """Thin wrapper so the import of ctypes.windll is confined to one place."""

    def __init__(self):
        if sys.platform != "win32":
            raise RuntimeError("vm-screenshot.py only works on Windows "
                               "(VMware Workstation host)")
        self.user32 = ctypes.windll.user32
        self.gdi32 = ctypes.windll.gdi32
        self.user32.SetProcessDPIAware()

    def find_windows(self):
        """Every visible top-level window, as (hwnd, title)."""
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
        return r.right - r.left, r.bottom - r.top

    def is_minimized(self, hwnd):
        """True if the window is iconic (minimized to the taskbar).

        This matters more than it looks. `PrintWindow` does **not** fail on a
        minimized window -- it succeeds and hands back a taskbar-thumbnail-
        sized stub. On 2026-08-07 that produced a 160x28 PNG that the tool
        reported as a successful capture; OCR returned zero lines and the
        result was nearly read as a statement about the guest.
        """
        return bool(self.user32.IsIconic(hwnd))

    def restore(self, hwnd):
        """Un-minimize `hwnd` (SW_RESTORE) without stealing focus order."""
        self.user32.ShowWindow(hwnd, 9)          # SW_RESTORE

    def print_window(self, hwnd, width, height):
        """Render hwnd into an offscreen bitmap; return raw BGRA bytes.

        Occlusion-proof: the window draws itself, so whatever is stacked on
        top of it on the physical desktop is irrelevant.
        """
        src = self.user32.GetWindowDC(hwnd)
        if not src:
            raise RuntimeError("GetWindowDC failed")
        try:
            mem = self.gdi32.CreateCompatibleDC(src)
            bmp = self.gdi32.CreateCompatibleBitmap(src, width, height)
            old = self.gdi32.SelectObject(mem, bmp)
            try:
                ok = self.user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)
                if not ok:
                    # Some windows refuse PW_RENDERFULLCONTENT but accept 0.
                    ok = self.user32.PrintWindow(hwnd, mem, 0)
                if not ok:
                    raise RuntimeError("PrintWindow returned false")
                return self._bits(mem, bmp, width, height)
            finally:
                self.gdi32.SelectObject(mem, old)
                self.gdi32.DeleteObject(bmp)
                self.gdi32.DeleteDC(mem)
        finally:
            self.user32.ReleaseDC(hwnd, src)

    def _bits(self, hdc, hbmp, width, height):
        class BITMAPINFOHEADER(ctypes.Structure):
            _fields_ = [("biSize", ctypes.c_uint32),
                        ("biWidth", ctypes.c_int32),
                        ("biHeight", ctypes.c_int32),
                        ("biPlanes", ctypes.c_uint16),
                        ("biBitCount", ctypes.c_uint16),
                        ("biCompression", ctypes.c_uint32),
                        ("biSizeImage", ctypes.c_uint32),
                        ("biXPelsPerMeter", ctypes.c_int32),
                        ("biYPelsPerMeter", ctypes.c_int32),
                        ("biClrUsed", ctypes.c_uint32),
                        ("biClrImportant", ctypes.c_uint32)]

        hdr = BITMAPINFOHEADER()
        hdr.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        hdr.biWidth = width
        hdr.biHeight = -height          # negative => top-down rows
        hdr.biPlanes = 1
        hdr.biBitCount = 32
        hdr.biCompression = 0           # BI_RGB

        buf = ctypes.create_string_buffer(width * height * 4)
        got = self.gdi32.GetDIBits(hdc, hbmp, 0, height, buf,
                                   ctypes.byref(hdr), 0)
        if got == 0:
            raise RuntimeError("GetDIBits failed")
        return buf.raw


# --- capture ---------------------------------------------------------------

def pick_window(win32, pattern):
    """Choose the VMware window whose title matches `pattern`.

    Matching is case-insensitive substring. Ambiguity is an error rather than
    a guess -- capturing the wrong VM silently is the exact failure mode this
    tool exists to prevent.
    """
    rx = re.compile(re.escape(pattern), re.I)
    hits = [(h, t) for h, t in win32.find_windows() if rx.search(t)]
    if not hits:
        titles = [t for _, t in win32.find_windows() if "vmware" in t.lower()]
        hint = ("  VMware windows currently open:\n    "
                + "\n    ".join(titles)) if titles else \
               "  No VMware windows are open at all -- is the VM running?"
        raise RuntimeError("no window title matches %r\n%s" % (pattern, hint))
    if len(hits) > 1:
        listing = "\n    ".join("%s (hwnd %s)" % (t, h) for h, t in hits)
        raise RuntimeError("%d windows match %r -- narrow it with --window:"
                           "\n    %s" % (len(hits), pattern, listing))
    return hits[0]


MIN_CAPTURE_WIDTH = 400
MIN_CAPTURE_HEIGHT = 300


def capture(win32, hwnd, restore=True):
    """PrintWindow `hwnd` into a Pillow image.

    Refuses to return a minimized-window stub. `PrintWindow` succeeds on an
    iconic window and yields a taskbar thumbnail -- a plausible-looking PNG
    that contains nothing of the guest. With `restore=True` the window is
    un-minimized first; the size floor below is the backstop for any other
    route to a degenerate frame.
    """
    from PIL import Image

    if win32.is_minimized(hwnd):
        if not restore:
            raise RuntimeError(
                "window is minimized -- PrintWindow would return a taskbar "
                "thumbnail, not the guest. Restore it or drop --no-restore")
        win32.restore(hwnd)
        time.sleep(1.0)                 # let the window repaint at full size
        if win32.is_minimized(hwnd):
            raise RuntimeError("window stayed minimized after SW_RESTORE")

    width, height = win32.window_rect(hwnd)
    if width <= 0 or height <= 0:
        raise RuntimeError("window has zero size (%dx%d) -- minimized?"
                           % (width, height))
    if width < MIN_CAPTURE_WIDTH or height < MIN_CAPTURE_HEIGHT:
        raise RuntimeError(
            "window is %dx%d, below the %dx%d floor -- this is a thumbnail "
            "or a collapsed window, not a guest framebuffer. Refusing to "
            "return it as a capture"
            % (width, height, MIN_CAPTURE_WIDTH, MIN_CAPTURE_HEIGHT))

    raw = win32.print_window(hwnd, width, height)
    img = Image.frombuffer("RGBA", (width, height), raw, "raw", "BGRA", 0, 1)
    return img.convert("RGB")


def crop_chrome(img, top=60, bottom=20, side=2):
    """Trim the VMware toolbar/status bar so OCR sees only the guest.

    The offsets are deliberately generous. Cutting a few guest pixels is
    harmless; leaving the host toolbar in feeds VMware's own menu text to OCR
    and produces convincing nonsense.
    """
    w, h = img.size
    box = (side, top, max(side + 1, w - side), max(top + 1, h - bottom))
    return img.crop(box)


# --- OCR -------------------------------------------------------------------

def load_tesseract():
    """Import pytesseract and locate the binary, or explain what's missing."""
    try:
        import pytesseract
    except ImportError:
        raise RuntimeError("--ocr/--expect need pytesseract: pip install "
                           "pytesseract (and install Tesseract-OCR itself)")

    if not pytesseract.pytesseract.tesseract_cmd or \
            not os.path.exists(pytesseract.pytesseract.tesseract_cmd):
        for cand in (r"C:\Program Files\Tesseract-OCR\tesseract.exe",
                     r"C:\Program Files (x86)\Tesseract-OCR\tesseract.exe"):
            if os.path.exists(cand):
                pytesseract.pytesseract.tesseract_cmd = cand
                break
    return pytesseract


def ocr_text(img, upscale=2):
    """OCR `img`, upscaled -- guest text at 1024x768 is small for Tesseract."""
    pytesseract = load_tesseract()
    grey = img.convert("L")
    if upscale > 1:
        grey = grey.resize((grey.size[0] * upscale, grey.size[1] * upscale))
    try:
        return pytesseract.image_to_string(grey)
    except Exception as exc:                      # tesseract missing/broken
        raise RuntimeError("Tesseract failed: %s" % exc)


def derived_frame_path(base, tag):
    """Return `base` with `tag` inserted before the extension.

    `--compare REF.png` used to write its second frame straight over `REF.png`,
    destroying any baseline the operator had saved there and making the mtimes
    lie about which frame came first. Both frames now land on derived paths --
    REF.frame1.png and REF.frame2.png -- and the named file is never touched.

        derived_frame_path("ref.png", "frame1") -> "ref.frame1.png"
        derived_frame_path("ref", "frame2")     -> "ref.frame2.png"
    """
    root, ext = os.path.splitext(base)
    return "%s.%s%s" % (root, tag, ext if ext else ".png")


def frame_delta(first, second, region=None):
    """Fraction of differing bytes between two frames, as a percentage.

    `region` is an optional (left, top, right, bottom) box applied to both
    frames before comparison. Comparing the *whole window* is a trap: on
    2026-08-07 a 20 s comparison reported 0.055% changed, and that 0.055% was
    a 165x9 strip at y=7 -- VMware's own title bar, not the guest at all.

    Read the result in ONE DIRECTION. A nonzero guest delta proves the guest
    is alive. A zero guest delta proves nothing: macOS Recovery has no clock,
    spinner or caret, so a healthy guest parked at the menu is byte-identical
    frame to frame. On the same day, a 0.000% guest delta here was read as a
    freeze; the guest was interactive throughout.

    Returns None if the frames differ in size (the window was resized, so no
    honest per-pixel comparison exists).
    """
    if first.size != second.size:
        return None
    if region is not None:
        first = first.crop(region)
        second = second.crop(region)
    a, b = first.tobytes(), second.tobytes()
    differing = sum(1 for x, y in zip(a, b) if x != y)
    return 100.0 * differing / len(a)


# Strings rendered by VMware Workstation's own UI (the Library sidebar, the VM
# inventory) or by the Windows shell behind it.
#
# CRITICAL: their presence does NOT mean the guest is hidden. Workstation docks
# the Library sidebar *beside* the guest console, and the Windows taskbar sits
# below the window -- so a perfectly good guest frame routinely contains all of
# these. This list is only half of the test; see `classify_frame`.
CHROME_MARKERS = (
    "my computer",
    "type here to search",
    "windows 11 x64",
)

# Strings that only the macOS guest can draw. The Finder menu bar is the
# strongest single signal: it is present on every macOS screen that is not the
# bare installer progress bar, and no part of VMware's or Windows' UI renders
# "Finder File Edit View Go Window Help".
#
# Kept as fragments rather than the whole bar because OCR of a 12px menu bar
# drops and mangles words freely -- 2026-08-07 the same bar read once as
# "ringer rue cat view vo wingow neip" and once as
# "Finder File Edit View Go Window Help".
GUEST_MARKERS = (
    "finder",
    "icloud drive",
    "macos base system",
    "applications",
    "recents",
    "go window help",
    "utilities",
    "reinstall macos",
    "disk utility",
)


def _hay(text):
    """Whitespace-collapsed, lowercased haystack.

    Same normalisation as `check_expected`, for the same reason: OCR mangles
    spacing far more often than it mangles letters.
    """
    return re.sub(r"\s+", " ", text).lower()


def classify_frame(text):
    """Return (chrome_markers, guest_markers) found in OCR `text`.

    Read the pair, never either half alone. This function exists because the
    obvious version of it was wrong, and the wrong version was actively
    dangerous.

    2026-08-07, measured: a capture of 'Sonoma - VMware Workstation' OCR'd as
    'Library', 'My Computer', 'Windows 11 x64', 'Type here to search' -- and
    ALSO as 'Finder File Edit View Go Window Help', 'Recents', 'Applications',
    'iCloud Drive', 'macOS Base System'. The guest console was frontmost the
    whole time. Workstation simply docks its Library sidebar next to the
    console, and the Windows taskbar shows below the window, so chrome strings
    and guest strings coexist in a completely healthy frame.

    A first version of this check failed the capture on chrome strings alone.
    That is a false positive, and the failure mode it creates is worse than the
    one it was written to prevent: it tells a caller the guest is unreachable
    when it is right there, which invites someone to "fix" a working setup.

    So the only sound conclusion is the conjunction:

      chrome AND NOT guest  -> good evidence the console is not showing
      guest (with or without chrome) -> the guest IS rendering
      neither -> unknown; OCR may have failed entirely. Prove it another way.

    Even the first case is one-directional, exactly like `frame_delta`: it is
    evidence, not proof, because OCR can fail to read guest text that is
    genuinely on screen. Pair it with --expect for a positive assertion.
    """
    hay = _hay(text)
    return ([m for m in CHROME_MARKERS if m in hay],
            [m for m in GUEST_MARKERS if m in hay])


def check_expected(text, phrases):
    """Return (found, missing) for `phrases`, matched case-insensitively.

    OCR mangles punctuation and spacing far more often than letters, so match
    on a whitespace-collapsed, lowercased haystack.
    """
    hay = re.sub(r"\s+", " ", text).lower()
    found, missing = [], []
    for p in phrases:
        needle = re.sub(r"\s+", " ", p).strip().lower()
        (found if needle in hay else missing).append(p)
    return found, missing


# --- main ------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        description="Occlusion-proof VMware guest screenshot + OCR check.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="exit 0 = ok, 1 = OCR did not READ an --expect phrase (not "
               "proof it is absent -- open the PNG), 2 = could not capture")
    p.add_argument("--window", default="VMware Workstation",
                   help="substring of the window title "
                        "(default: 'VMware Workstation')")
    p.add_argument("--out", help="write the capture to this PNG")
    p.add_argument("--crop-chrome", action="store_true",
                   help="trim VMware toolbar/status bar from the saved PNG")
    p.add_argument("--ocr", action="store_true",
                   help="print the OCR'd guest text")
    p.add_argument("--expect", action="append", default=[], metavar="PHRASE",
                   help="require PHRASE on screen; repeatable; implies --ocr")
    p.add_argument("--settle", type=float, default=0.0, metavar="SEC",
                   help="wait SEC before capturing")
    p.add_argument("--compare", metavar="PNG",
                   help="capture a SECOND frame after --settle and report "
                        "whether the screen changed between the two -- both "
                        "frames are captured by this run, so this answers "
                        "'is anything moving right now', NOT 'did my action "
                        "change the screen'. PNG is a base name only: the "
                        "frames are written to PNG.frame1.png / PNG.frame2.png "
                        "and PNG itself is never overwritten. For before/after "
                        "evidence use two --out captures and tools/png-delta.py")
    p.add_argument("--no-restore", action="store_true",
                   help="do not un-minimize the window; fail instead. A "
                        "minimized window yields a taskbar thumbnail, never "
                        "the guest")
    p.add_argument("--allow-chrome", action="store_true",
                   help="do not fail when the capture shows VMware's own UI "
                        "(Library pane) instead of the guest console")
    p.add_argument("--list", action="store_true",
                   help="list visible window titles and exit")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)

    try:
        win32 = _Win32()
    except RuntimeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    if args.list:
        # Window titles routinely contain characters outside the console's
        # code page (cp1252 on this host). Printing them raw raised
        # UnicodeEncodeError and aborted the listing part-way, hiding every
        # window after the offending one -- including, potentially, the VM.
        enc = getattr(sys.stdout, "encoding", None) or "ascii"
        for hwnd, title in win32.find_windows():
            safe = title.encode(enc, "replace").decode(enc, "replace")
            print("%-12s %s" % (hwnd, safe))
        return 0

    try:
        from PIL import Image                                   # noqa: F401
    except ImportError:
        print("error: Pillow is required: pip install pillow", file=sys.stderr)
        return 2

    try:
        hwnd, title = pick_window(win32, args.window)
    except RuntimeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    print("window : %s (hwnd %s)" % (title, hwnd))

    if args.settle and not args.compare:
        time.sleep(args.settle)

    try:
        shot = capture(win32, hwnd, restore=not args.no_restore)
    except RuntimeError as exc:
        print("error: capture failed: %s" % exc, file=sys.stderr)
        return 2

    print("size   : %dx%d" % shot.size)

    guest = crop_chrome(shot)

    if args.out:
        (guest if args.crop_chrome else shot).save(args.out)
        digest = hashlib.md5(open(args.out, "rb").read()).hexdigest()
        print("saved  : %s (md5 %s)" % (args.out, digest))

    if args.compare:
        # Both frames of this comparison are captured HERE, after whatever
        # action the operator already performed. It answers "is anything
        # moving right now", never "did my action change the screen" -- for
        # that, save two --out captures around the action and diff them with
        # tools/png-delta.py.
        #
        # Historical defect (fixed): this used to save the second frame
        # straight to the path named by --compare, overwriting the file the
        # operator named. Anyone who passed a saved baseline lost it, and the
        # mtimes then "proved" the baseline was written after the after-frame.
        # Both frames now go to derived paths; the named file is never written.
        # tests/test_vm_screenshot.py greps this file for the old call to make
        # sure it cannot come back -- do not quote it literally here.
        frame1_path = derived_frame_path(args.compare, "frame1")
        frame2_path = derived_frame_path(args.compare, "frame2")
        if os.path.exists(args.compare):
            print("note   : %s already exists and is NOT overwritten -- "
                  "--compare captures both of its own frames" % args.compare)

        settle = args.settle if args.settle else 10.0
        time.sleep(settle)
        try:
            second = capture(win32, hwnd, restore=not args.no_restore)
        except RuntimeError as exc:
            print("error: second capture failed: %s" % exc, file=sys.stderr)
            return 2

        shot.save(frame1_path)
        second.save(frame2_path)
        print("saved  : %s (frame 1 of 2)" % frame1_path)
        print("saved  : %s (frame 2 of 2, %.1f s later)"
              % (frame2_path, settle))

        w, h = shot.size
        guest_box = (2, 60, max(3, w - 2), max(61, h - 20))
        guest_pct = frame_delta(shot, second, guest_box)
        window_pct = frame_delta(shot, second)

        if guest_pct is None:
            print("changed: unknown (window was resized between frames)")
        else:
            # The guest figure is the verdict. The window figure is printed
            # alongside it only so a nonzero-but-chrome-only delta is visible
            # as chrome rather than mistaken for the guest running.
            print("changed: %s (guest %.3f%%, whole window %.3f%%)"
                  % ("yes" if guest_pct > 0.01 else
                     "no -- guest framebuffer is static",
                     guest_pct, window_pct))
            if guest_pct <= 0.01 < window_pct:
                print("note   : the only moving pixels are VMware's own "
                      "chrome -- this is not proof the guest is alive")

    if not (args.ocr or args.expect):
        return 0

    try:
        text = ocr_text(guest)
    except RuntimeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    lines = [l.strip() for l in text.splitlines() if l.strip()]
    if args.ocr or not args.expect:
        print("--- OCR (%d lines) ---" % len(lines))
        for line in lines:
            print("  %s" % line)

    chrome, guest = classify_frame(text)
    if chrome:
        print("chrome : %s" % ", ".join(repr(m) for m in chrome))
    if guest:
        print("guest  : %s" % ", ".join(repr(m) for m in guest))

    # Only the conjunction is meaningful. Chrome strings alone are normal --
    # the Library sidebar docks beside a perfectly visible guest console.
    if chrome and not guest:
        if not args.allow_chrome:
            # Deliberately NOT the word FAIL on its own line. On 2026-08-07 an
            # operator read "result : FAIL" as "the capture failed / the guest
            # is gone" while the OCR dump printed twenty lines above it listed
            # macOS Recovery menu items -- the phrase table simply lacked those
            # spellings. The verdict is about THIS HEURISTIC's markers, never
            # about the guest, so it says so in the same string.
            print("result : INCONCLUSIVE -- no marker from this tool's macOS "
                  "phrase table matched, and")
            print("         VMware chrome strings did. That is a statement "
                  "about the phrase table,")
            print("         not about the guest: OCR routinely misses guest "
                  "text that is really on")
            print("         screen, and the table only knows the spellings "
                  "someone added to it.")
            print("         READ THE PNG BEFORE CONCLUDING ANYTHING. The OCR "
                  "dump above is the")
            print("         evidence; this line is only a heuristic over it, "
                  "and it has been wrong.")
            print("         The window is real and full-size, so this is not "
                  "a minimized-window stub;")
            print("         the likeliest benign cause is that the guest "
                  "console tab is not frontmost.")
            print("         Click the guest tab in VMware Workstation, then "
                  "re-capture.")
            print("         DO NOT run tools/vm-type.py against this window "
                  "in this state -- it targets")
            print("         the same hwnd, so keystrokes would land in "
                  "VMware's menus, not the guest.")
            print("         Pass --allow-chrome to capture anyway.")
            return 2
        print("warn   : chrome without guest strings -- guest may not be "
              "frontmost (--allow-chrome set)")

    if not args.expect:
        return 0

    found, missing = check_expected(text, args.expect)
    for phrase in found:
        print("FOUND  : %s" % phrase)
    for phrase in missing:
        print("MISSING: %s" % phrase)
    if missing:
        # FAIL here is narrower than it looks, and saying so costs one line.
        # It means "OCR did not read these phrases", not "these phrases are not
        # on the guest screen" -- Tesseract misreads antialiased macOS text
        # routinely. The OCR dump above is the evidence; this is a string match
        # over it.
        print("result : FAIL (%d/%d phrases read by OCR)"
              % (len(found), len(args.expect)))
        print("         This means OCR did not READ the missing phrases, not "
              "that they are absent")
        print("         from the guest screen. Open the PNG before concluding "
              "anything about the VM.")
        return 1
    print("result : PASS (%d/%d phrases found)"
          % (len(found), len(args.expect)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
