#!/usr/bin/env python3
"""
check-header-coverage.py -- assert that section 1 of tests/check-kext-syntax.bat
parses EVERY dependency-free header in phase0-kext/, and that anything it skips
is skipped for a reason that is checkable rather than remembered.

Why this exists
---------------
Section 1 of the gate is a hand-written list of clang invocations, one line per
header. A hand-written list has exactly one failure mode, and it is silent: add
a new header to phase0-kext/ and forget the line, and the gate still prints
all-ok. It does not print "5 of 6" -- it prints ok five times, because it has no
idea a sixth header exists. That is how this gate spent time parsing two of five
headers while reporting success.

The fix is not to hand-maintain the list more carefully. It is to derive the
expected set from the filesystem and compare.

The exemption is DERIVED, not listed
------------------------------------
One header genuinely cannot be parsed standalone: NV3050Probe.hpp includes
<IOKit/...>, which needs -I tests/kextshim and the full %FLAGS%, not %HDRFLAGS%.
Hardcoding the name "NV3050Probe.hpp" here would just move the stale list from
the .bat to the .py.

So the exemption is computed: a header is exempt from section 1 if and only if
it has an angle-bracket #include, i.e. it depends on something outside
phase0-kext/. That is the same property the project's architecture split is
built on (see CLAUDE.md: the logic headers include no IOKit and no kernel
types). If someone adds IOKit to NVGspBoot.hpp, this check stops demanding a
standalone parse for it -- and section 2 becomes the thing that must cover it.

An exempt header is not unchecked
---------------------------------
Being skipped by section 1 is only acceptable if something else parses it. This
script requires every exempt header to be reachable from a .cpp that section 2
compiles, via a quoted #include. NV3050Probe.cpp includes "NV3050Probe.hpp", so
the IOKit header is covered with the shim on the include path. A header that is
neither parsed standalone nor pulled in by a compiled source is reported as a
failure, because nothing anywhere would catch a syntax error in it.

Usage
-----
    python tools/check-header-coverage.py [path-to-check-kext-syntax.bat]

The optional path exists so tests can point this at a fixture gate file -- a
checker with no negative control is a checker nobody has proven can fail.
Exits 0 on success, 1 on any failure.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEXT_DIR = ROOT / "phase0-kext"
DEFAULT_GATE = ROOT / "tests" / "check-kext-syntax.bat"

#
# The clang line in section 1 looks like:
#
#     "!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVDecode.hpp
#
# Match on %HDRFLAGS% specifically. Section 2 uses %FLAGS% and compiles a .cpp,
# so it cannot be confused with a standalone header parse, and a REM line
# mentioning a header name will not match either.
#
STANDALONE_RE = re.compile(
    r"%HDRFLAGS%.*?phase0-kext[/\\]([A-Za-z0-9_]+\.hpp)"
)

#
# Section 2 compiles sources with %FLAGS%. Same shape, .cpp instead.
#
COMPILED_SRC_RE = re.compile(
    r"%FLAGS%\s+phase0-kext[/\\]([A-Za-z0-9_]+\.cpp)"
)

ANGLE_INCLUDE_RE = re.compile(r"^\s*#\s*include\s*<", re.MULTILINE)
QUOTED_INCLUDE_RE = re.compile(r"^\s*#\s*include\s*\"([^\"]+)\"", re.MULTILINE)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def is_self_contained(header: Path) -> bool:
    """True when the header pulls in nothing from outside phase0-kext/."""
    return ANGLE_INCLUDE_RE.search(read_text(header)) is None


def quoted_includes(path: Path) -> list:
    return QUOTED_INCLUDE_RE.findall(read_text(path))


def reachable_headers(source: Path) -> set:
    """Headers reachable from `source` through quoted includes, transitively."""
    seen = set()
    pending = [source]
    while pending:
        current = pending.pop()
        for name in quoted_includes(current):
            leaf = Path(name).name
            if leaf in seen:
                continue
            candidate = KEXT_DIR / leaf
            if not candidate.is_file():
                continue
            seen.add(leaf)
            pending.append(candidate)
    return seen


def main() -> int:
    gate_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_GATE

    if not KEXT_DIR.is_dir():
        print(f"FAIL: kext directory not found: {KEXT_DIR}")
        return 1
    if not gate_path.is_file():
        print(f"FAIL: gate script not found: {gate_path}")
        return 1

    headers = sorted(p.name for p in KEXT_DIR.glob("*.hpp"))
    if not headers:
        print(f"FAIL: no headers found under {KEXT_DIR}")
        return 1

    gate_text = read_text(gate_path)
    parsed = set(STANDALONE_RE.findall(gate_text))
    compiled_sources = sorted(set(COMPILED_SRC_RE.findall(gate_text)))

    #
    # Everything section 2 compiles, plus everything those sources include,
    # transitively. This is what makes an exemption legitimate.
    #
    covered_by_compile = set()
    for src_name in compiled_sources:
        src = KEXT_DIR / src_name
        if src.is_file():
            covered_by_compile |= reachable_headers(src)

    failures = []

    for name in headers:
        header = KEXT_DIR / name
        self_contained = is_self_contained(header)

        if self_contained:
            if name not in parsed:
                failures.append(
                    f"{name}: dependency-free (no angle-bracket include) but "
                    f"section 1 never parses it standalone. Add a "
                    f'"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/{name} line.'
                )
            continue

        # Not self-contained: standalone parse is not expected, but SOMETHING
        # must still parse it.
        if name in parsed:
            failures.append(
                f"{name}: has an angle-bracket include, so it cannot parse "
                f"standalone under %HDRFLAGS% (no shim on the include path), "
                f"yet section 1 tries to."
            )
        elif name not in covered_by_compile:
            failures.append(
                f"{name}: parsed by nothing. It is not dependency-free, so "
                f"section 1 skips it, and no source compiled in section 2 "
                f"includes it. A syntax error here would go unnoticed."
            )

    #
    # The reverse direction: a line naming a header that no longer exists is a
    # gate that silently tests less than it claims.
    #
    for name in sorted(parsed):
        if name not in headers:
            failures.append(
                f"{name}: section 1 parses it, but no such file exists in "
                f"{KEXT_DIR.name}/. Stale line."
            )

    if failures:
        for line in failures:
            print(f"FAIL: {line}")
        return 1

    exempt = [n for n in headers if n not in parsed]
    print(f"header coverage: {len(parsed)}/{len(headers)} parsed standalone")
    for name in sorted(parsed):
        print(f"  standalone  {name}")
    for name in exempt:
        print(f"  via compile {name} (angle-bracket include; "
              f"reached from {', '.join(compiled_sources) or 'nothing'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
