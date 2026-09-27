#!/usr/bin/env python3
"""
check-tool-coverage.py -- assert that every tools/*.py a verification gate
depends on has a test module in tests/, and that the mapping is derived from the
filesystem rather than remembered.

Why this exists
---------------
This is the same shape of hole that ``check-header-coverage.py`` closed one
level down. The gates invoke Python checkers -- ``check-registers.py``,
``check-structors.py``, ``check-header-coverage.py`` -- and those checkers are
*evidence producers*: when one is wrong it does not crash, it prints a
plausible, confidently-formatted, wrong verdict and the gate goes green.

A gate-invoked checker with no tests is therefore worse than an untested
ordinary script. It is a green light nobody has proven can turn red. That was
not hypothetical either: ``check-structors.py`` sat in section 4 of
``check-kext-syntax.bat`` with **zero** test coverage while three of its four
sibling tools had suites.

What counts as "a gate depends on it"
-------------------------------------
Derived, not listed. Every ``tools/<name>.py`` invocation found in:

  * any ``tests/*.bat`` gate script, and
  * the fenced gate block in ``CLAUDE.md``

is a gate dependency. Nothing here hardcodes a tool name, so wiring a new
checker into a gate immediately makes this check demand tests for it -- which is
the entire point. Tools that no gate invokes (the ``_``-prefixed probes,
one-shot explorers, operator utilities) are deliberately NOT required to have
tests; requiring it would make this check noise, and noise gets suppressed.

What counts as "has a test module"
----------------------------------
A file ``tests/test_*.py`` that mentions the tool's filename in **code** --
not in a comment and not in a docstring.

The filename is the binding because the hyphenated tool names cannot be
imported: every existing suite loads its subject by path
(``spec_from_file_location``) or runs it as a subprocess, and in both cases the
filename appears verbatim as a string literal.

Prose mentions are excluded because they produced a false positive on the very
first run of this check. ``tests/test_check_tool_coverage.py`` names
``check-registers.py`` and ``check-structors.py`` in its own docstrings while
testing neither, and that alone was enough to report both as covered. Worse than
cosmetic: deleting ``test_check_structors.py`` would then have left this check
green on the strength of a sentence. A docstring is not a test.

It still does NOT prove the suite is thorough -- see "What this does not prove".

Usage
-----
    python tools/check-tool-coverage.py [project-root]

The optional root exists so tests can point this at a fixture tree. Exits 0 on
success, 1 on any failure.

What this does not prove
------------------------
That a tool is *well* tested. A suite of one trivial assertion satisfies this
check. It proves only that a gate-invoked evidence producer is not completely
unexercised -- the difference between "thinly covered" and "nothing anywhere
would notice if this returned the wrong answer".
"""

import ast
import re
import sys
from pathlib import Path

#
# tools/<name>.py as invoked from a batch file or documented in CLAUDE.md.
# Both forward and backslash, since gate scripts are cmd.exe and CLAUDE.md is
# not consistent about it.
#
GATE_INVOKE_RE = re.compile(r"tools[/\\]([A-Za-z0-9_.-]+\.py)")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def code_string_literals(path: Path):
    """Every string literal in `path` that is NOT a docstring.

    Comments never reach here at all -- ``ast`` discards them -- and module,
    class and function docstrings are removed explicitly. What is left is the
    strings the code actually uses: the ``os.path.join(..., "check-x.py")`` and
    ``spec_from_file_location`` arguments by which every suite in this project
    binds to its hyphenated subject.

    Returns None when the file cannot be parsed, so the caller can decide what
    an unparseable test module means rather than silently treating it as empty
    (which would read as "mentions nothing" and fail for the wrong reason).
    """
    try:
        tree = ast.parse(read_text(path))
    except SyntaxError:
        return None

    docstrings = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef,
                            ast.FunctionDef, ast.AsyncFunctionDef)):
            body = getattr(node, "body", None)
            if not body:
                continue
            first = body[0]
            if isinstance(first, ast.Expr) and \
                    isinstance(first.value, ast.Constant) and \
                    isinstance(first.value.value, str):
                docstrings.add(id(first.value))

    out = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if id(node) in docstrings:
                continue
            out.append(node.value)
    return out


def gate_scripts(root: Path) -> list:
    """Every gate script whose tool invocations count as a dependency."""
    found = sorted((root / "tests").glob("*.bat")) if (root / "tests").is_dir() else []
    claude = root / "CLAUDE.md"
    if claude.is_file():
        found.append(claude)
    return found


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else \
        Path(__file__).resolve().parent.parent

    tools_dir = root / "tools"
    tests_dir = root / "tests"

    if not tools_dir.is_dir():
        print(f"FAIL: tools directory not found: {tools_dir}")
        return 1
    if not tests_dir.is_dir():
        print(f"FAIL: tests directory not found: {tests_dir}")
        return 1

    gates = gate_scripts(root)
    if not gates:
        print(f"FAIL: no gate scripts found under {tests_dir} and no CLAUDE.md "
              f"-- with nothing to read, this check would vacuously pass.")
        return 1

    # tool filename -> sorted list of gate files that invoke it
    required = {}
    for gate in gates:
        for name in GATE_INVOKE_RE.findall(read_text(gate)):
            required.setdefault(name, set()).add(gate.name)

    if not required:
        print(f"FAIL: no tools/*.py invocation found in any of "
              f"{', '.join(g.name for g in gates)} -- either the gates stopped "
              f"using their checkers, or this pattern no longer matches them.")
        return 1

    # tool filename -> sorted list of test modules that reference it in CODE
    test_modules = sorted(tests_dir.glob("test_*.py"))
    mentions = {}
    unparseable = []
    for module in test_modules:
        literals = code_string_literals(module)
        if literals is None:
            unparseable.append(module.name)
            continue
        for name in sorted(required):
            if any(name in literal for literal in literals):
                mentions.setdefault(name, set()).add(module.name)

    failures = []

    #
    # A test module that will not parse cannot be counted either way, and it is
    # not going to run under discovery either. Say so rather than letting it
    # sit in the tests directory looking like coverage.
    #
    for name in unparseable:
        failures.append(
            f"{name}: does not parse as Python, so it cannot count as coverage "
            f"for anything and will not run under unittest discovery."
        )

    for name in sorted(required):
        invoked_by = ", ".join(sorted(required[name]))

        if not (tools_dir / name).is_file():
            failures.append(
                f"{name}: invoked by {invoked_by}, but no such file exists in "
                f"tools/. The gate would fail at run time; it should not have "
                f"taken a run to find out."
            )
            continue

        if name not in mentions:
            failures.append(
                f"{name}: invoked by {invoked_by}, but no tests/test_*.py "
                f"mentions it. A gate-invoked checker with no tests is a green "
                f"light nobody has proven can turn red."
            )

    if failures:
        for line in failures:
            print(f"FAIL: {line}")
        return 1

    print(f"tool coverage: {len(required)}/{len(required)} gate-invoked tools "
          f"have a test module")
    for name in sorted(required):
        print(f"  {name}")
        print(f"      invoked by {', '.join(sorted(required[name]))}")
        print(f"      covered by {', '.join(sorted(mentions[name]))}")

    untested = sorted(
        p.name for p in tools_dir.glob("*.py") if p.name not in required
    )
    print(f"\nnot gate-invoked, tests not required ({len(untested)}): "
          f"{', '.join(untested) if untested else 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
