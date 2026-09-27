#!/usr/bin/env python3
"""
_token-probe.py -- report whether THIS process can actually inject input, and
write the answer to a file.

Why the file
------------
The whole point is to compare a process launched from the agent's shell against
one launched some other way (Task Scheduler, user-typed command). A process
launched by Task Scheduler has no shared stdout with us, so the verdict has to
land somewhere both sides can read.

What it reports
---------------
  * TokenIsRestricted / TokenSandBoxInert / TokenElevationType -- a restricted
    token is the reason SendInput is accepted and discarded.
  * BlockInput(FALSE) ret+err -- ACCESS_DENIED here does NOT prove another
    thread holds the block; a restricted token produces the same error. That
    mistake cost this project two wrong diagnoses on 2026-08-07.
  * A real delivery test: inject an absolute mouse move and check the cursor
    position actually changed. Post-condition only; SendInput's return value is
    not evidence.

Usage
-----
    python _token-probe.py --out probe.txt

Exit status: 0 injection delivered, 1 did not, 2 could not probe.
"""

import argparse
import ctypes
import os
import sys
import time
from ctypes import wintypes

SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN = 76, 77
SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 78, 79
MOUSEEVENTF_MOVE, MOUSEEVENTF_ABSOLUTE = 0x0001, 0x8000
TOKEN_QUERY = 0x0008

# TOKEN_INFORMATION_CLASS, verbatim from winnt.h. Spelled out because getting
# these wrong is SILENT -- GetTokenInformation succeeds and fills the buffer
# for whichever class you actually asked for, under whatever label you printed.
#
# CORRECTED 2026-08-07 (bold-wren-29). This file previously queried 23 as
# "TokenSandBoxInert" (23 is TokenVirtualizationAllowed; SandBoxInert is 15)
# and 21 as "TokenIsRestricted" (21 is TokenHasRestrictions -- near enough in
# meaning, but the name did not exist in the enum). Both return DWORDs, so the
# numbers looked plausible and nothing failed.
#
# A separate ad-hoc probe run the same day made the worse version of this
# mistake: it used 25 for "TokenIsRestricted" and 22 for
# "TokenVirtualizationEnabled". Those are TokenIntegrityLevel and
# TokenAccessInformation -- *structs* whose first field is a pointer -- and it
# printed 3957965056 and 3955797784, the low 32 bits of two pointers, under
# boolean-sounding labels. Query a struct class as a DWORD and you get
# confident nonsense, not an error.
TokenSandBoxInert = 15
TokenElevationType = 18
TokenLinkedToken = 19
TokenElevation = 20
TokenHasRestrictions = 21
TokenVirtualizationAllowed = 23
TokenVirtualizationEnabled = 24
TokenIntegrityLevel = 25
TokenUIAccess = 26

ELEVATION_TYPE = {1: "Default (not a split token)",
                  2: "Full (elevated)",
                  3: "Limited (UAC-filtered)"}

# winnt.h SECURITY_MANDATORY_*_RID
INTEGRITY = {0x0000: "Untrusted", 0x1000: "Low", 0x2000: "Medium",
             0x2100: "Medium Plus", 0x3000: "High", 0x4000: "System",
             0x5000: "Protected"}


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_ulonglong)]


class _IU(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _IU)]


def report():
    u = ctypes.windll.user32
    k = ctypes.windll.kernel32
    a = ctypes.windll.advapi32
    u.SetProcessDPIAware()
    u.BlockInput.argtypes = (wintypes.BOOL,)
    u.BlockInput.restype = wintypes.BOOL
    u.SetCursorPos.argtypes = (ctypes.c_int, ctypes.c_int)
    u.SetCursorPos.restype = wintypes.BOOL
    a.OpenProcessToken.argtypes = (wintypes.HANDLE, wintypes.DWORD,
                                   ctypes.POINTER(wintypes.HANDLE))
    a.OpenProcessToken.restype = wintypes.BOOL
    a.GetTokenInformation.argtypes = (wintypes.HANDLE, ctypes.c_int,
                                      ctypes.c_void_p, wintypes.DWORD,
                                      ctypes.POINTER(wintypes.DWORD))
    a.GetTokenInformation.restype = wintypes.BOOL

    lines = []
    lines.append("pid              : %d" % os.getpid())
    lines.append("exe              : %s" % sys.executable)

    tok = wintypes.HANDLE()
    if a.OpenProcessToken(k.GetCurrentProcess(), TOKEN_QUERY, ctypes.byref(tok)):
        # DWORD-valued classes only. Do NOT add TokenIntegrityLevel (25) or
        # TokenAccessInformation (22) to this loop -- they are structs, and
        # reading four bytes off a struct yields the low half of a pointer,
        # printed as a plausible-looking integer with no error raised.
        for label, cls in (("TokenHasRestrictions", TokenHasRestrictions),
                           ("TokenSandBoxInert", TokenSandBoxInert),
                           ("TokenUIAccess", TokenUIAccess),
                           ("TokenElevation", TokenElevation),
                           ("TokenElevationType", TokenElevationType)):
            val = wintypes.DWORD(0)
            need = wintypes.DWORD()
            ok = a.GetTokenInformation(tok, cls, ctypes.byref(val),
                                       ctypes.sizeof(val), ctypes.byref(need))
            decoded = ""
            if ok and label == "TokenElevationType":
                decoded = "  (%s)" % ELEVATION_TYPE.get(val.value,
                                                        "unrecognised")
            lines.append("%-20s: %s%s%s"
                         % (label, val.value if ok else "?",
                            "" if ok else " (err %d)" % k.GetLastError(),
                            decoded))

        # Integrity level: TOKEN_MANDATORY_LABEL, whose first field is a SID
        # pointer. Parsed properly rather than truncated to a DWORD.
        a.GetSidSubAuthorityCount.argtypes = (ctypes.c_void_p,)
        a.GetSidSubAuthorityCount.restype = ctypes.POINTER(ctypes.c_ubyte)
        a.GetSidSubAuthority.argtypes = (ctypes.c_void_p, wintypes.DWORD)
        a.GetSidSubAuthority.restype = ctypes.POINTER(wintypes.DWORD)
        need = wintypes.DWORD(0)
        a.GetTokenInformation(tok, TokenIntegrityLevel, None, 0,
                              ctypes.byref(need))
        buf = ctypes.create_string_buffer(max(need.value, 8))
        k.SetLastError(0)
        if a.GetTokenInformation(tok, TokenIntegrityLevel, buf,
                                 ctypes.sizeof(buf), ctypes.byref(need)):
            psize = ctypes.sizeof(ctypes.c_void_p)
            sid = ctypes.c_void_p(int.from_bytes(buf.raw[:psize], "little"))
            cnt = a.GetSidSubAuthorityCount(sid)
            if cnt:
                rid = a.GetSidSubAuthority(sid, cnt[0] - 1)[0]
                lines.append("%-20s: 0x%04x  (%s)"
                             % ("TokenIntegrityLevel", rid,
                                INTEGRITY.get(rid, "unrecognised")))
            else:
                lines.append("%-20s: ? (bad SID)" % "TokenIntegrityLevel")
        else:
            lines.append("%-20s: ? (err %d)"
                         % ("TokenIntegrityLevel", k.GetLastError()))
    else:
        lines.append("token            : OpenProcessToken failed %d"
                     % k.GetLastError())

    k.SetLastError(0)
    bret = bool(u.BlockInput(False))
    berr = k.GetLastError()
    lines.append("BlockInput(FALSE): ret=%s err=%d  "
                 "(err 5 on a restricted token is NOT proof another thread "
                 "holds the block)" % (bret, berr))

    def pos():
        p = POINT()
        u.GetCursorPos(ctypes.byref(p))
        return (p.x, p.y)

    start = pos()
    tx = start[0] - 60 if start[0] > 120 else start[0] + 60
    vx = u.GetSystemMetrics(SM_XVIRTUALSCREEN)
    vy = u.GetSystemMetrics(SM_YVIRTUALSCREEN)
    vw = u.GetSystemMetrics(SM_CXVIRTUALSCREEN)
    vh = u.GetSystemMetrics(SM_CYVIRTUALSCREEN)
    nx = int((tx - vx) * 65535 / max(1, vw - 1))
    ny = int((start[1] - vy) * 65535 / max(1, vh - 1))
    inp = INPUT(type=0, u=_IU(mi=MOUSEINPUT(
        dx=nx, dy=ny, mouseData=0,
        dwFlags=MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
        time=0, dwExtraInfo=0)))
    k.SetLastError(0)
    sent = u.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    serr = k.GetLastError()
    time.sleep(0.35)
    after = pos()
    delivered = after != start

    k.SetLastError(0)
    scp = bool(u.SetCursorPos(tx, start[1]))
    scperr = k.GetLastError()
    time.sleep(0.25)
    after2 = pos()

    lines.append("SendInput        : sent=%d err=%d  %s -> %s"
                 % (sent, serr, start, after))
    lines.append("SetCursorPos     : ret=%s err=%d  -> %s"
                 % (scp, scperr, after2))
    delivered = delivered or after2 != start
    lines.append("DELIVERED        : %s" % delivered)
    lines.append("VERDICT          : injection %s from this process"
                 % ("WORKS" if delivered else "does NOT work"))
    return lines, delivered


def main(argv=None):
    ap = argparse.ArgumentParser(description="Report injection capability.")
    ap.add_argument("--out", help="also write the report to this path")
    args = ap.parse_args(argv)
    if sys.platform != "win32":
        print("error: Windows only", file=sys.stderr)
        return 2
    lines, delivered = report()
    text = "\n".join(lines)
    print(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    return 0 if delivered else 1


if __name__ == "__main__":
    sys.exit(main())
