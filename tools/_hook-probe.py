#!/usr/bin/env python3
"""
_hook-probe.py -- find out WHERE an injected input event dies.

Why this exists
---------------
On 2026-08-07 `SendInput` from this host reported `sent=1 err=0` and nothing
moved. Four hypotheses were raised and all four were FALSIFIED by measurement:

  1. wrong coordinates        -- real bug, fixed, not the blocker
  2. UIPI / integrity level   -- vmware.exe and python.exe are BOTH medium IL
  3. another thread holds BlockInput(TRUE)
                              -- err=5 also occurs on a UAC-filtered token, so
                                 it was never evidence of a holder
  4. restricted token         -- a Task Scheduler /IT process showed the SAME
                                 TokenIsRestricted=1 and the SAME no-op, and on
                                 an admin account every non-elevated process
                                 carries a filtered token, so the flag is normal
  5. non-interactive window station
                              -- WinSta0, WSF_VISIBLE=1, desktop Default,
                                 OpenInputDesktop succeeds

So the question is no longer "which permission" but "at which layer does the
event disappear". This probe splits that into two independent measurements:

  A. LOW-LEVEL HOOK CHAIN. Install our own WH_MOUSE_LL hook, pump messages,
     then inject. If the hook fires, the event reached the raw input chain and
     something downstream (or the cursor-position layer) dropped it. If the hook
     never fires, the event never got that far -- the call is being intercepted
     at or before user32.

     The hook also reports LLMHF_INJECTED (0x01) and LLMHF_LOWER_IL_INJECTED
     (0x02), which say whether the OS tagged our event as synthetic and as
     coming from a lower integrity level.

  B. IN-PROCESS API PATCHING. Dump the first bytes of user32!SendInput and
     user32!SetCursorPos. An injected DLL that hooks input APIs rewrites the
     prologue with a jmp (0xE9 / 0xFF25 / 0x48B8...FFE0). A clean prologue rules
     that out; a jmp names the culprit via the module list.

The hook must run on a thread with a message pump -- a WH_MOUSE_LL hook is
called by the OS on the installing thread and is silently skipped (and
eventually timed out and removed) if that thread never pumps.

Usage
-----
    python _hook-probe.py

Exit status
-----------
    0   the hook saw our injected event (event reaches the input chain)
    1   the hook never saw it (event dies at or before user32)
    2   could not probe (not Windows / hook could not be installed)
"""

import ctypes
import sys
import time
from ctypes import wintypes

WH_MOUSE_LL = 14
WM_MOUSEMOVE = 0x0200
LLMHF_INJECTED = 0x00000001
LLMHF_LOWER_IL_INJECTED = 0x00000002

SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN = 76, 77
SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 78, 79
MOUSEEVENTF_MOVE, MOUSEEVENTF_ABSOLUTE = 0x0001, 0x8000

PM_REMOVE = 0x0001

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class MSLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("pt", POINT), ("mouseData", wintypes.DWORD),
                ("flags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_ulonglong)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_ulonglong)]


class _IU(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _IU)]


class MSG(ctypes.Structure):
    _fields_ = [("hwnd", wintypes.HWND), ("message", wintypes.UINT),
                ("wParam", ctypes.c_ulonglong), ("lParam", ctypes.c_ulonglong),
                ("time", wintypes.DWORD), ("pt", POINT)]


HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int,
                              ctypes.c_ulonglong, ctypes.c_void_p)

seen = []


def _hook_proc(ncode, wparam, lparam):
    if ncode >= 0:
        info = ctypes.cast(lparam, ctypes.POINTER(MSLLHOOKSTRUCT)).contents
        seen.append({
            "msg": int(wparam),
            "pt": (info.pt.x, info.pt.y),
            "flags": info.flags,
            "injected": bool(info.flags & LLMHF_INJECTED),
            "lower_il": bool(info.flags & LLMHF_LOWER_IL_INJECTED),
        })
    return user32.CallNextHookEx(None, ncode, wparam, lparam)


def pos():
    p = POINT()
    user32.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


def inject_move(x, y):
    vx = user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
    vy = user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
    vw = user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
    vh = user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)
    nx = int((x - vx) * 65535 / max(1, vw - 1))
    ny = int((y - vy) * 65535 / max(1, vh - 1))
    inp = INPUT(type=0, u=_IU(mi=MOUSEINPUT(
        dx=nx, dy=ny, mouseData=0,
        dwFlags=MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
        time=0, dwExtraInfo=0)))
    kernel32.SetLastError(0)
    sent = user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    return sent, kernel32.GetLastError()


def pump(seconds):
    """Pump the thread message queue -- a WH_MOUSE_LL hook is never called on a
    thread that does not pump, so skipping this would fake a negative result."""
    msg = MSG()
    end = time.time() + seconds
    while time.time() < end:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.01)


def prologue(dll, func, n=16):
    """First n bytes of an exported function, as hex."""
    kernel32.GetModuleHandleW.argtypes = (wintypes.LPCWSTR,)
    kernel32.GetModuleHandleW.restype = wintypes.HMODULE
    kernel32.GetProcAddress.argtypes = (wintypes.HMODULE, wintypes.LPCSTR)
    kernel32.GetProcAddress.restype = ctypes.c_void_p
    h = kernel32.GetModuleHandleW(dll)
    if not h:
        return None, "module %s not loaded" % dll
    addr = kernel32.GetProcAddress(h, func.encode())
    if not addr:
        return None, "%s!%s not found" % (dll, func)
    buf = (ctypes.c_ubyte * n).from_address(addr)
    return bytes(buf), None


def looks_hooked(b):
    """Detect the common inline-hook prologues.

    A clean x64 export usually starts with the hotpatch pad `4c 8b d1` /
    `48 83 ec` / `mov edi,edi`. A detour rewrites it with a near jmp (e9),
    an indirect jmp (ff 25), or a mov rax,imm64 + jmp rax (48 b8 ... ff e0).
    """
    if not b:
        return False, "no bytes"
    if b[0] == 0xE9:
        return True, "near jmp (e9)"
    if b[0] == 0xFF and b[1] == 0x25:
        return True, "indirect jmp (ff 25)"
    if b[0] == 0x48 and b[1] == 0xB8 and b[10:12] == b"\xff\xe0":
        return True, "mov rax,imm64 + jmp rax"
    if b[0] == 0x68:
        return True, "push imm32 + ret"
    return False, "clean prologue"


def modules():
    """Loaded modules outside System32/SysWOW64/the Python install.

    An input-filtering DLL injected into every process is the classic cause of
    'SendInput succeeds and does nothing', and it shows up here.
    """
    # Every one of these argtypes is load-bearing. Without them ctypes defaults
    # HMODULE to c_int and a 64-bit module base raises
    # 'OverflowError: int too long to convert' -- which is what killed this
    # function on its first run, AFTER the useful half of the probe had already
    # printed. Declare the prototypes; do not rely on ctypes guessing.
    psapi.EnumProcessModules.argtypes = (wintypes.HANDLE,
                                         ctypes.POINTER(wintypes.HMODULE),
                                         wintypes.DWORD,
                                         ctypes.POINTER(wintypes.DWORD))
    psapi.EnumProcessModules.restype = wintypes.BOOL
    psapi.GetModuleFileNameExW.argtypes = (wintypes.HANDLE, wintypes.HMODULE,
                                           wintypes.LPWSTR, wintypes.DWORD)
    psapi.GetModuleFileNameExW.restype = wintypes.DWORD
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    h = kernel32.GetCurrentProcess()
    arr = (wintypes.HMODULE * 1024)()
    need = wintypes.DWORD()
    if not psapi.EnumProcessModules(h, arr, ctypes.sizeof(arr),
                                    ctypes.byref(need)):
        return []
    count = need.value // ctypes.sizeof(wintypes.HMODULE)
    out = []
    buf = ctypes.create_unicode_buffer(1024)
    for i in range(count):
        if psapi.GetModuleFileNameExW(h, arr[i], buf, 1024):
            p = buf.value
            low = p.lower()
            if ("\\windows\\system32\\" in low
                    or "\\windows\\syswow64\\" in low
                    or "\\windows\\winsxs\\" in low
                    or "\\python3" in low):
                continue
            out.append(p)
    return out


def main():
    if sys.platform != "win32":
        print("error: Windows only", file=sys.stderr)
        return 2
    user32.SetProcessDPIAware()
    user32.SetWindowsHookExW.argtypes = (ctypes.c_int, HOOKPROC,
                                         wintypes.HINSTANCE, wintypes.DWORD)
    user32.SetWindowsHookExW.restype = ctypes.c_void_p
    user32.CallNextHookEx.argtypes = (ctypes.c_void_p, ctypes.c_int,
                                      ctypes.c_ulonglong, ctypes.c_void_p)
    user32.CallNextHookEx.restype = ctypes.c_ssize_t

    print("=== B. in-process API patching ===")
    for dll, fn in (("user32.dll", "SendInput"),
                    ("user32.dll", "SetCursorPos"),
                    ("user32.dll", "BlockInput"),
                    ("user32.dll", "mouse_event")):
        b, err = prologue(dll, fn)
        if err:
            print("  %-14s %s" % (fn, err))
            continue
        hooked, why = looks_hooked(b)
        print("  %-14s %s   %s%s"
              % (fn, b.hex(" "), why, "  <-- HOOKED" if hooked else ""))

    mods = modules()
    print("  foreign modules in-process: %d" % len(mods))
    for p in mods[:20]:
        print("    %s" % p)

    print("=== A. low-level hook chain ===")
    cb = HOOKPROC(_hook_proc)
    kernel32.SetLastError(0)
    hhook = user32.SetWindowsHookExW(WH_MOUSE_LL, cb, None, 0)
    if not hhook:
        print("  SetWindowsHookEx(WH_MOUSE_LL) FAILED err=%d"
              % kernel32.GetLastError(), file=sys.stderr)
        return 2
    print("  hook installed: %s" % hhook)

    pump(0.5)
    baseline = len(seen)
    start = pos()
    target = (start[0] - 70 if start[0] > 140 else start[0] + 70, start[1])
    sent, err = inject_move(*target)
    pump(1.2)
    after = pos()

    fired = seen[baseline:]
    print("  inject %s -> requested %s" % (start, target))
    print("  SendInput sent=%d err=%d" % (sent, err))
    print("  cursor after: %s   moved=%s" % (after, after != start))
    print("  hook callbacks during injection: %d" % len(fired))
    for ev in fired[:6]:
        print("    msg=0x%04X pt=%s flags=0x%X injected=%s lower_il=%s"
              % (ev["msg"], ev["pt"], ev["flags"], ev["injected"],
                 ev["lower_il"]))

    user32.UnhookWindowsHookEx(hhook)

    print("---")
    if fired:
        print("VERDICT: the injected event DID enter the low-level input chain.")
        print("         The call is not being swallowed at user32; the event is")
        print("         dropped downstream (another LL hook returning nonzero,")
        print("         or the cursor layer refusing the move).")
        return 0
    print("VERDICT: the injected event NEVER reached the low-level hook chain,")
    print("         even though SendInput reported success. The event dies at")
    print("         or before user32 for this process.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
