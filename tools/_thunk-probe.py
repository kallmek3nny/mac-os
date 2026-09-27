#!/usr/bin/env python3
"""
_thunk-probe.py -- decide whether user32's input exports are actually HOOKED,
and test the one input path whose prologue is clean.

Why this exists
---------------
_hook-probe.py reported this on 2026-08-07:

    SendInput      ff 25 42 b6 03 00 ...   indirect jmp (ff 25)  <-- HOOKED
    SetCursorPos   ff 25 7a b4 03 00 ...   indirect jmp (ff 25)  <-- HOOKED
    BlockInput     ff 25 12 ce 03 00 ...   indirect jmp (ff 25)  <-- HOOKED
    mouse_event    4c 8b dc 48 83 ec 58 ...  clean prologue

and Windhawk was found injected into the process. That looks damning and may
still be wrong: on Windows 11 many user32 exports are *legitimately* `ff 25`
indirect thunks that forward into win32u.dll or into user32's own body. A
bare-prologue pattern match cannot tell a detour from a forwarder -- calling it
"HOOKED" on the byte pattern alone is exactly the kind of confident-wrong answer
this project keeps having to retract.

The test that actually decides it: `ff 25 <rel32>` reads a pointer from
(rip + rel32). FOLLOW that pointer and ask which module owns the destination.

  * destination inside user32.dll / win32u.dll / ntdll.dll -> normal thunk,
    NOT a hook
  * destination inside windhawk.dll or any other foreign module -> a real detour,
    and the module name is the culprit

It also tests `mouse_event`, whose prologue is clean. mouse_event reaches the
same kernel input path as SendInput by a different export. If mouse_event moves
the cursor while SendInput does not, the block is per-export (so: a hook). If
neither moves it, the block is below both, and no user-mode unhooking would
help.

Usage
-----
    python _thunk-probe.py

Exit status
-----------
    0   some input path delivered (cursor observably moved)
    1   no input path delivered
    2   could not probe (not Windows)
"""

import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000
SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN = 76, 77
SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 78, 79


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


def _decl():
    kernel32.GetModuleHandleW.argtypes = (wintypes.LPCWSTR,)
    kernel32.GetModuleHandleW.restype = wintypes.HMODULE
    kernel32.GetProcAddress.argtypes = (wintypes.HMODULE, wintypes.LPCSTR)
    kernel32.GetProcAddress.restype = ctypes.c_void_p
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    psapi.EnumProcessModules.argtypes = (wintypes.HANDLE,
                                         ctypes.POINTER(wintypes.HMODULE),
                                         wintypes.DWORD,
                                         ctypes.POINTER(wintypes.DWORD))
    psapi.EnumProcessModules.restype = wintypes.BOOL
    psapi.GetModuleFileNameExW.argtypes = (wintypes.HANDLE, wintypes.HMODULE,
                                           wintypes.LPWSTR, wintypes.DWORD)
    psapi.GetModuleFileNameExW.restype = wintypes.DWORD
    psapi.GetModuleInformation.argtypes = (wintypes.HANDLE, wintypes.HMODULE,
                                           ctypes.c_void_p, wintypes.DWORD)
    psapi.GetModuleInformation.restype = wintypes.BOOL
    user32.GetCursorPos.argtypes = (ctypes.POINTER(POINT),)
    user32.mouse_event.argtypes = (wintypes.DWORD, wintypes.DWORD,
                                   wintypes.DWORD, wintypes.DWORD,
                                   ctypes.c_ulonglong)


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p),
                ("SizeOfImage", wintypes.DWORD),
                ("EntryPoint", ctypes.c_void_p)]


def module_map():
    """[(base, end, path)] for every module in this process."""
    h = kernel32.GetCurrentProcess()
    arr = (wintypes.HMODULE * 1024)()
    need = wintypes.DWORD()
    out = []
    if not psapi.EnumProcessModules(h, arr, ctypes.sizeof(arr),
                                    ctypes.byref(need)):
        return out
    count = need.value // ctypes.sizeof(wintypes.HMODULE)
    buf = ctypes.create_unicode_buffer(1024)
    for i in range(count):
        mi = MODULEINFO()
        if not psapi.GetModuleInformation(h, arr[i], ctypes.byref(mi),
                                          ctypes.sizeof(mi)):
            continue
        psapi.GetModuleFileNameExW(h, arr[i], buf, 1024)
        base = mi.lpBaseOfDll or 0
        out.append((base, base + mi.SizeOfImage, buf.value))
    return out


def owner(addr, mods):
    for base, end, path in mods:
        if base <= addr < end:
            return path
    return None


def follow(dll, func, mods):
    """Resolve an export and, if it is an `ff 25` thunk, follow it once.

    Returns a dict describing the export, the thunk destination, and which
    module owns each -- the only evidence that separates a detour from a normal
    forwarder.
    """
    h = kernel32.GetModuleHandleW(dll)
    addr = kernel32.GetProcAddress(h, func.encode()) if h else None
    if not addr:
        return {"name": func, "error": "not resolved"}
    raw = bytes((ctypes.c_ubyte * 8).from_address(addr))
    info = {"name": func, "addr": addr, "bytes": raw.hex(" "),
            "addr_module": owner(addr, mods)}
    if raw[0] == 0xFF and raw[1] == 0x25:
        rel = int.from_bytes(raw[2:6], "little", signed=True)
        slot = addr + 6 + rel
        info["slot"] = slot
        info["slot_module"] = owner(slot, mods)
        try:
            dest = int.from_bytes(
                bytes((ctypes.c_ubyte * 8).from_address(slot)), "little")
            info["dest"] = dest
            info["dest_module"] = owner(dest, mods)
        except Exception as exc:                      # unreadable slot
            info["dest_error"] = str(exc)
    return info


def pos():
    p = POINT()
    user32.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


def norm(x, y):
    vx = user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
    vy = user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
    vw = user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
    vh = user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)
    return (int((x - vx) * 65535 / max(1, vw - 1)),
            int((y - vy) * 65535 / max(1, vh - 1)))


def try_sendinput(x, y):
    nx, ny = norm(x, y)
    inp = INPUT(type=0, u=_IU(mi=MOUSEINPUT(
        dx=nx, dy=ny, mouseData=0,
        dwFlags=MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
        time=0, dwExtraInfo=0)))
    kernel32.SetLastError(0)
    sent = user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    return "sent=%d err=%d" % (sent, kernel32.GetLastError())


def try_mouse_event(x, y):
    nx, ny = norm(x, y)
    kernel32.SetLastError(0)
    user32.mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, nx, ny, 0, 0)
    return "err=%d" % kernel32.GetLastError()


def try_setcursorpos(x, y):
    user32.SetCursorPos.argtypes = (ctypes.c_int, ctypes.c_int)
    user32.SetCursorPos.restype = wintypes.BOOL
    kernel32.SetLastError(0)
    r = bool(user32.SetCursorPos(x, y))
    return "ret=%s err=%d" % (r, kernel32.GetLastError())


def main():
    if sys.platform != "win32":
        print("error: Windows only", file=sys.stderr)
        return 2
    _decl()
    user32.SetProcessDPIAware()
    mods = module_map()

    print("=== is the ff25 a HOOK or a normal thunk? ===")
    print("    (follow the indirect jmp and name the module that owns the "
          "destination)")
    for fn in ("SendInput", "SetCursorPos", "BlockInput", "mouse_event"):
        i = follow("user32.dll", fn, mods)
        if "error" in i:
            print("  %-13s %s" % (fn, i["error"]))
            continue
        print("  %-13s at 0x%X in %s"
              % (fn, i["addr"], short(i.get("addr_module"))))
        print("                bytes %s" % i["bytes"])
        if "dest" in i:
            dm = i.get("dest_module")
            verdict = classify(dm)
            print("                thunk slot 0x%X in %s"
                  % (i["slot"], short(i.get("slot_module"))))
            print("                -> dest 0x%X in %s   %s"
                  % (i["dest"], short(dm), verdict))
        elif "dest_error" in i:
            print("                slot unreadable: %s" % i["dest_error"])
        else:
            print("                direct code, no thunk to follow")

    print("=== does ANY input path deliver? ===")
    delivered = False
    for label, fn in (("SendInput", try_sendinput),
                      ("mouse_event", try_mouse_event),
                      ("SetCursorPos", try_setcursorpos)):
        start = pos()
        target = (start[0] - 80 if start[0] > 160 else start[0] + 80, start[1])
        detail = fn(*target)
        time.sleep(0.35)
        after = pos()
        moved = after != start
        delivered = delivered or moved
        print("  %-13s %s -> %s   %s   DELIVERED=%s"
              % (label, start, after, detail, moved))
        if moved:
            fn(*start)
            time.sleep(0.2)

    print("---")
    print("VERDICT: %s"
          % ("at least one input path DELIVERS" if delivered
             else "NO user-mode input path delivers from this process"))
    return 0 if delivered else 1


def short(path):
    return path.rsplit("\\", 1)[-1] if path else "<unmapped>"


def classify(dest_module):
    if not dest_module:
        return "<-- destination not in any loaded module (SUSPICIOUS)"
    low = dest_module.lower()
    if any(s in low for s in ("\\user32.dll", "\\win32u.dll", "\\ntdll.dll",
                              "\\kernelbase.dll", "\\kernel32.dll")):
        return "= NORMAL THUNK, not a hook"
    return "<-- REAL DETOUR into %s" % short(dest_module)


if __name__ == "__main__":
    sys.exit(main())
