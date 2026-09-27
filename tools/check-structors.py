#!/usr/bin/env python3
"""
check-structors.py -- assert every OSDeclareDefaultStructors has a matching
OSDefineMetaClassAndStructors, and that the superclass named there is the one
the class actually derives from.

Why this is a separate script instead of a compiler check:

    tests/kextshim/ lets clang parse the kext with -fsyntax-only on Windows.
    That catches typos, bad overrides, and format-string mismatches. It cannot
    catch a missing OSDefineMetaClassAndStructors, because the declared ctor
    and dtor only go unresolved at LINK time, and there is nothing to link
    against here -- a kext links against Apple's kernel KPI.

    On macOS the failure mode is worse than a link error would suggest: the
    class ends up with no metaclass registration, so IOKit never instantiates
    the driver and the kext loads while doing nothing. Silent, exactly like the
    two register-decode bugs this project already shipped.

Usage:
    python tools/check-structors.py [kext-dir]

The optional directory argument exists so tests can point this at a fixture
tree containing deliberately-broken sources. A checker with no negative control
is a checker nobody has proven can fail -- running it once against the real
kext and seeing exit 0 proves only that it does not crash, which a body of
``return 0`` would also do.

Exit code 0 on success, 1 on any mismatch.
"""

import re
import sys
from pathlib import Path

KEXT_DIR = Path(__file__).resolve().parent.parent / "phase0-kext"

DECLARE_RE = re.compile(r"OSDeclare(?:Default|Abstract)Structors\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")
DEFINE_RE = re.compile(
    r"OSDefineMetaClassAndStructors\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)"
)
# `class Foo : public Bar` -- captures the declared base so the metaclass
# superclass argument can be cross-checked against it.
CLASS_RE = re.compile(
    r"class\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*public\s+([A-Za-z_][A-Za-z0-9_]*)"
)


def main() -> int:
    kext_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else KEXT_DIR

    if not kext_dir.is_dir():
        print(f"FAIL: kext directory not found: {kext_dir}")
        return 1

    sources = sorted(
        p for p in kext_dir.iterdir()
        if p.suffix in (".cpp", ".hpp", ".h", ".cc")
    )
    if not sources:
        print(f"FAIL: no sources found under {kext_dir}")
        return 1

    declared = {}   # class name -> file that declared it
    defined = {}    # class name -> (file, superclass named in the macro)
    bases = {}      # class name -> base class from its `class X : public Y`

    for path in sources:
        text = path.read_text(encoding="utf-8", errors="replace")

        for name in DECLARE_RE.findall(text):
            if name in declared:
                print(f"FAIL: {name} declared structors twice "
                      f"({declared[name]} and {path.name})")
                return 1
            declared[name] = path.name

        for name, super_name in DEFINE_RE.findall(text):
            if name in defined:
                print(f"FAIL: {name} defines structors twice "
                      f"({defined[name][0]} and {path.name}) -- "
                      f"duplicate metaclass registration")
                return 1
            defined[name] = (path.name, super_name)

        for name, base in CLASS_RE.findall(text):
            bases[name] = base

    failures = []

    for name, decl_file in sorted(declared.items()):
        if name not in defined:
            failures.append(
                f"{name}: OSDeclareDefaultStructors in {decl_file}, but no "
                f"OSDefineMetaClassAndStructors anywhere. On macOS this builds "
                f"and loads, then never instantiates."
            )
            continue

        def_file, super_named = defined[name]
        actual_base = bases.get(name)
        if actual_base is not None and actual_base != super_named:
            failures.append(
                f"{name}: derives from {actual_base} but "
                f"OSDefineMetaClassAndStructors in {def_file} names "
                f"{super_named}."
            )

    for name, (def_file, _super) in sorted(defined.items()):
        if name not in declared:
            failures.append(
                f"{name}: OSDefineMetaClassAndStructors in {def_file} with no "
                f"matching OSDeclareDefaultStructors in the class body."
            )

    print(f"sources scanned : {len(sources)} "
          f"({', '.join(p.name for p in sources)})")
    print(f"structors pairs : {len(declared)} declared, {len(defined)} defined")
    for name in sorted(declared):
        if name in defined:
            print(f"  ok   {name} : {defined[name][1]}")

    if failures:
        print()
        for line in failures:
            print(f"  FAIL {line}")
        print(f"\n{len(failures)} failed")
        return 1

    print(f"\n{len(declared)}/{len(declared)} passed, 0 failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
