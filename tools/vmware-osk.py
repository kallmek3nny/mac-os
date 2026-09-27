#!/usr/bin/env python3
"""Recover the AppleSMC OSK from an installed VMware binary, at RUNTIME only.

Why this exists
---------------
`isa-applesmc` has an `osk=` property and QEMU ships no default for it. Apple's
`AppleSMC` kext validates that key, so a QEMU boot without it is expected to fail
late in kernel start. `QEMU-LOWSTORAGE.md` §4 recorded that as an open blocker
and noted that VMware boots the same guest anyway -- `smc.present = "TRUE"` plus
the unlocked `vmware-vmx` supplies the key internally.

This measures whether that internal copy is READABLE on this host, so the blocker
stops being a hypothesis.

What was measured (2026-08-14, VMware Workstation x64 `vmware-vmx.exe`,
21 209 456 bytes)
-----------------------------------------------------------------------
The key is present, and it is **not** one contiguous 64-byte string -- which is
why the first probe looked like a partial hit and could easily have been filed as
"absent":

| Probe | Result |
|---|---|
| first fragment (23 bytes) | 2 occurrences, `0xd1d098` and `0xd24b58` |
| tail fragment (19 bytes) | 2 occurrences, at +85 from each |
| MIDDLE fragment (22 bytes) | **0 occurrences** |

The zero for the middle fragment is real and explains the layout: between the two
halves sit NUL bytes and non-ASCII data, so the key is stored as **two 32-byte
ASCII runs** rather than one 64-byte run. Both runs are byte-identical between the
two occurrences, so reassembly involves no guesswork and no choosing between
candidates.

The sweep carries positive controls, because a null here would otherwise be
useless. All six fire (re-measured 2026-08-15 against the same 21 209 456-byte
binary): `smc.present` 1, `smc.version` 1, `AppleSMC` 3, `darwin` 86,
`guestOS` 18, `firmware` 59. So an absence would have meant absence.

Do not trust the counts in this docstring over the counts the program prints. An
earlier draft of this comment said "7 of 8 controls fired" and `firmware` (57)
against a binary of the same size -- 8 is not the length of CONTROLS and 57 is
not what the sweep returns, so both were prose drifting from the code beside it.
Only the boolean matters to the argument (some control fired, so the scan can see
strings at all); the magnitudes are recorded for provenance and are re-derived on
every run.

Why this file does not contain the key
--------------------------------------
The key is Apple's, read out of software installed on this machine. Committing it
turns a local measurement into redistribution, so the value lives nowhere in this
tree: not in a constant, not in a test fixture, not in a doc. This script reads it
out of the installed binary each time it runs, and the intended use is command
substitution so it never touches disk:

    python tools/qemu-boot-sonoma.py --probe-seconds 90 \
        --osk "$(python tools/vmware-osk.py --print)"

`--print` is deliberately not the default. Bare invocation reports only that a
key was found, its length, and its digest -- enough to prove the channel works
without putting 64 bytes of Apple's string into a terminal transcript that may be
logged.

Usage:
    python tools/vmware-osk.py                 # presence, length, digest
    python tools/vmware-osk.py --print         # the key on stdout, nothing else
    python tools/vmware-osk.py --vmx PATH      # a different vmware-vmx binary

Exit codes: 2 for a malformed invocation, 1 when no key can be recovered
(including "the binary is not there"), 0 only when a key was recovered.
"""

import argparse
import hashlib
import os
import re
import sys

DEFAULT_VMX = r"C:\Program Files (x86)\VMware\VMware Workstation\x64\vmware-vmx.exe"

# Anchors, not the key. Each is a published fragment of the OSK sentence; the
# material BETWEEN them is what this script recovers and never hardcodes.
HEAD_ANCHOR = b"ourhardworkbythesewords"
TAIL_ANCHOR = b"(c)AppleComputerInc"

OSK_LEN = 64
RUN_LEN = 32

# Controls proven to fire against this binary. A sweep that finds no key has to
# demonstrate it could have found one, or the null describes the sweep.
CONTROLS = [b"smc.present", b"smc.version", b"AppleSMC", b"darwin",
            b"guestOS", b"firmware"]


def printable_runs(blob, start, stop):
    """Return the maximal all-printable-ASCII runs inside blob[start:stop]."""
    out = []
    cur = bytearray()
    for ch in blob[start:stop]:
        if 32 <= ch < 127:
            cur.append(ch)
        else:
            if cur:
                out.append(bytes(cur))
            cur = bytearray()
    if cur:
        out.append(bytes(cur))
    return out


def recover(path):
    """Return (key, report). `key` is None when nothing could be recovered.

    The report is returned even on failure, because the interesting part of a
    failure here is WHICH controls fired -- that is the difference between "this
    binary has no key" and "this scan cannot see strings in this binary".
    """
    blob = open(path, "rb").read()
    report = {
        "path": path,
        "size": len(blob),
        "controls": [(c.decode("ascii"), blob.count(c)) for c in CONTROLS],
        "head_offsets": [m.start() for m in re.finditer(re.escape(HEAD_ANCHOR), blob)],
        "tail_offsets": [m.start() for m in re.finditer(re.escape(TAIL_ANCHOR), blob)],
        "candidates": [],
    }
    report["controls_fired"] = sum(1 for _n, k in report["controls"] if k)

    for head in report["head_offsets"]:
        # Bound the search window by the tail anchor that follows this head,
        # rather than by a fixed span. Measured here the tail sits at +85, but
        # deriving it means a differently-padded build still resolves instead of
        # silently reading 64 bytes of whatever happens to follow.
        tails = [t for t in report["tail_offsets"] if t > head]
        if not tails:
            continue
        stop = min(tails) + len(TAIL_ANCHOR)
        runs = [r for r in printable_runs(blob, head, stop) if len(r) == RUN_LEN]
        if len(runs) < 2:
            continue
        cand = b"".join(runs[:2])
        if len(cand) != OSK_LEN:
            continue
        if not cand.startswith(HEAD_ANCHOR) or not cand.endswith(TAIL_ANCHOR):
            continue
        report["candidates"].append((head, cand))

    if not report["candidates"]:
        return None, report

    # Every occurrence must agree. Two occurrences that DISAGREED would mean one
    # of them is not the key, and picking the first would be a coin flip filed as
    # a measurement.
    values = set(c for _o, c in report["candidates"])
    report["agree"] = (len(values) == 1)
    if not report["agree"]:
        return None, report
    return report["candidates"][0][1], report


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="vmware-osk.py",
        description="Recover the AppleSMC OSK from an installed vmware-vmx binary.")
    p.add_argument("--vmx", default=DEFAULT_VMX, help="vmware-vmx.exe to read")
    p.add_argument("--print", dest="do_print", action="store_true",
                   help="write the key to stdout and nothing else, for command "
                        "substitution into --osk")
    args = p.parse_args(argv)

    if not os.path.exists(args.vmx):
        sys.stderr.write("FAIL: no such binary: %s\n" % args.vmx)
        return 1

    try:
        key, rep = recover(args.vmx)
    except OSError as exc:
        sys.stderr.write("FAIL: cannot read %s: %s\n" % (args.vmx, exc))
        return 1

    if key is None:
        sys.stderr.write("FAIL: no OSK recovered from %s\n" % args.vmx)
        sys.stderr.write("      controls fired: %d of %d -- %s\n"
                         % (rep["controls_fired"], len(rep["controls"]),
                            ", ".join("%s=%d" % kv for kv in rep["controls"])))
        if rep["controls_fired"] == 0:
            sys.stderr.write("      ZERO controls fired, so this null describes the "
                             "SCAN, not the binary.\n")
        if rep["candidates"] and not rep.get("agree", True):
            sys.stderr.write("      %d candidates found and they DISAGREE; refusing "
                             "to pick one.\n" % len(rep["candidates"]))
        return 1

    if args.do_print:
        sys.stdout.write(key.decode("ascii"))
        return 0

    print("binary          : %s" % rep["path"])
    print("size            : %d bytes" % rep["size"])
    print("controls fired  : %d of %d (%s)"
          % (rep["controls_fired"], len(rep["controls"]),
             ", ".join("%s=%d" % kv for kv in rep["controls"])))
    print("head anchor at  : %s" % [hex(o) for o in rep["head_offsets"]])
    print("tail anchor at  : %s" % [hex(o) for o in rep["tail_offsets"]])
    print("occurrences     : %d, all in agreement: %s"
          % (len(rep["candidates"]), rep.get("agree")))
    print("key length      : %d bytes" % len(key))
    print("key sha256      : %s" % hashlib.sha256(key).hexdigest())
    print()
    print("The VALUE is not printed. Pass --print to emit it on stdout, and use")
    print("command substitution so it never lands on disk:")
    print('  python tools/qemu-boot-sonoma.py --probe-seconds 90 \\')
    print('      --osk "$(python tools/vmware-osk.py --print)"')
    return 0


if __name__ == "__main__":
    sys.exit(main())
