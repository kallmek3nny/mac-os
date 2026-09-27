#!/usr/bin/env python3
"""
check-negative-controls.py -- assert that every NVNEG_* block defined in
tests/kextshim/negative-control.cpp is actually enumerated by section 11 of
tests/check-kext-syntax.bat, and that section 11 enumerates nothing that does
not exist.

Why this exists
---------------
Section 11 is a hand-typed macro list inside a ``for %%M in (...)`` loop. A
hand-typed list has one failure mode and it is silent in BOTH directions:

1. **Defined but not enumerated.** Add a ``#if defined(NVNEG_NEW)`` block to
   negative-control.cpp, forget the batch file, and the control never runs.
   The gate still prints a wall of ``ok`` lines and still exits 0. The shim is
   then claimed to catch a defect that nothing has ever checked it catches.

2. **Enumerated but not defined.** Delete or rename a block and leave the name
   in the loop. clang is handed ``-DNVNEG_GONE`` against a file that contains
   no such block, compiles the baseline, exits 0 -- and section 11 reports
   ``ok   NVNEG_GONE diagnosed``. That line is a lie in the most damaging
   possible form: a passing control that tests nothing.

Direction 2 is not hypothetical. ``NVNEG_MISSING_STRUCTORS`` was removed from
negative-control.cpp because ``-fsyntax-only`` never links, so it could not
fail. Had its name been left in the loop, the gate would have gone on printing
``ok   NVNEG_MISSING_STRUCTORS diagnosed`` forever.

What binds the two sides
------------------------
Both sets are DERIVED from the files at run time -- the ``#if defined(...)``
lines on one side, the ``for %%M in (...)`` list on the other. Neither is typed
into this checker. A list retyped here would be a third copy to drift.

Macro names appearing in COMMENTS are deliberately not counted. The header
comment of negative-control.cpp names several macros in prose, and the removed
``NVNEG_MISSING_STRUCTORS`` is still discussed there on purpose. Only an actual
``#if defined(NVNEG_...)`` preprocessor line defines a block.

Verdict
-------
Prints ``VERDICT: PASS`` or ``VERDICT: FAIL`` on the last line and exits 0 or 1
respectively. Callers should assert the verdict WORD -- an exit code alone
cannot distinguish "checked and clean" from "found nothing to check", which is
the precise failure this file exists to prevent.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CONTROL_CPP = os.path.join(ROOT, "tests", "kextshim", "negative-control.cpp")
GATE_BAT = os.path.join(ROOT, "tests", "check-kext-syntax.bat")

#
# `#if defined(NVNEG_X)` only. Not `#ifdef`, because the file does not use it
# and silently accepting a spelling the file never uses would let a block hide.
# If a future block uses #ifdef, this regex must be widened DELIBERATELY, and
# test_check_negative_controls.py has a case that fails until it is.
#
DEFINE_RE = re.compile(r"^\s*#if\s+defined\s*\(\s*(NVNEG_[A-Z0-9_]+)\s*\)", re.M)

#
# The `for %%M in (...)` loop. Anchored on `%%M` rather than on `for`, because
# check-kext-syntax.bat contains other for-loops and matching the wrong one
# would compare against an unrelated list and still produce a confident verdict.
#
LOOP_RE = re.compile(r"for\s+%%M\s+in\s*\(([^)]*)\)", re.I)

MACRO_TOKEN_RE = re.compile(r"NVNEG_[A-Z0-9_]+")


def defined_macros(text):
    """Macro names that guard a real block in negative-control.cpp."""
    return set(DEFINE_RE.findall(text))


def enumerated_macros(text):
    """Macro names section 11 of the gate actually iterates over.

    Returns None -- distinct from an empty set -- when the loop cannot be
    found at all. An empty set would compare as "the gate enumerates nothing",
    which is a legitimate (if alarming) state; a missing loop means this
    checker no longer understands the file it is auditing and must refuse
    rather than render a verdict.
    """
    m = LOOP_RE.search(text)
    if m is None:
        return None
    return set(MACRO_TOKEN_RE.findall(m.group(1)))


def main():
    problems = []

    try:
        with open(CONTROL_CPP, "r", encoding="utf-8") as fh:
            cpp = fh.read()
    except OSError as exc:
        print("cannot read %s: %s" % (CONTROL_CPP, exc))
        print("VERDICT: FAIL")
        return 1

    try:
        with open(GATE_BAT, "r", encoding="utf-8") as fh:
            bat = fh.read()
    except OSError as exc:
        print("cannot read %s: %s" % (GATE_BAT, exc))
        print("VERDICT: FAIL")
        return 1

    defined = defined_macros(cpp)
    enumerated = enumerated_macros(bat)

    if enumerated is None:
        # NOT %-formatted: `%%M` is how the token is spelled inside a .bat
        # file, and `"...%%M..." % x` would collapse it to `%M`, printing a
        # construct cmd.exe has no such thing as.
        print("no `for %%M in (...)` loop found in " + os.path.basename(GATE_BAT))
        print("  section 11 has been restructured; this checker no longer")
        print("  understands it and refuses to render a verdict.")
        print("VERDICT: FAIL")
        return 1

    #
    # Vacuity guard. If BOTH sides are empty the comparison below succeeds
    # trivially, and a green line would mean "the negative controls are all
    # accounted for" when the truth is that there are none. Report the
    # substantive comparison first and this only when nothing else explains
    # the silence.
    #
    for name in sorted(defined - enumerated):
        problems.append(
            "defined but never run: %s guards a block in negative-control.cpp "
            "that section 11 does not enumerate" % name
        )

    for name in sorted(enumerated - defined):
        problems.append(
            "enumerated but not defined: section 11 passes -D%s to clang, but "
            "negative-control.cpp has no `#if defined(%s)` block -- that "
            "control reports ok while testing nothing" % (name, name)
        )

    if problems:
        for p in problems:
            print("  " + p)
        print("VERDICT: FAIL")
        return 1

    if not defined:
        print("no NVNEG_* blocks found in %s" % os.path.basename(CONTROL_CPP))
        print("  nothing was compared; the negative-control suite is empty.")
        print("VERDICT: FAIL")
        return 1

    print("  %d negative control(s), defined set == enumerated set" % len(defined))
    for name in sorted(defined):
        print("    %s" % name)
    print("VERDICT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
