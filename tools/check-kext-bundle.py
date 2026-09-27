#!/usr/bin/env python3
"""
check-kext-bundle.py -- the kext's IDENTITY keys, checked against the files that
produce them.

Why this exists
---------------
``tools/check-kext-personalities.py`` covers the matching dictionary: every
``IOPCIMatch`` names a device this project targets, every personality has an
``IOClass``, every ``IOProviderClass`` is ``IOPCIDevice``. That leaves the rest
of ``phase0-kext/Info.plist`` -- the part that decides whether the bundle can be
LOADED at all -- read by nothing.

The defects in that half share one property: they are invisible to every gate
this project has, because none of them is a compile error.

* ``CFBundleExecutable`` names the binary inside ``Contents/MacOS/``. The
  Makefile builds ``$(MACOS_DIR)/$(KEXT_NAME)``. If those two strings ever drift
  apart, ``make`` still succeeds, the bundle still assembles, and ``kmutil``
  refuses it with a message about a missing executable -- naming a file the
  operator can plainly see is present, under a different name.

* ``CFBundleIdentifier`` is what ``kmutil unload -b`` targets, and the Makefile
  spells that identifier a SECOND time in its own ``BUNDLE_ID``. Two independent
  spellings of one string is the classic silent-drift shape: change the plist,
  and ``make unload`` starts unloading nothing at all while reporting success.

* Each personality repeats ``CFBundleIdentifier`` a THIRD time. That copy tells
  IOKit which bundle owns the personality. Get it wrong and the personality is
  registered against a bundle that does not exist, so the class is never
  instantiated. Identical symptom to a wrong ``IOPCIMatch``: silence.

* ``IOClass`` is resolved at match time through the ``OSMetaClass`` registry, by
  STRING. It is not a symbol the linker sees. A typo there links clean and fails
  only on real hardware, at match time, with no diagnostic in the build log.

* ``OSBundleLibraries`` declares the KPIs to link against. Omit the one that
  supplies a class the source actually uses and the failure surfaces as an
  undefined symbol at load time on the target machine -- the single most
  expensive place in this project for a defect to surface, since getting there
  costs a power cycle and physical access.

So every expectation below is DERIVED from a producer file -- the Makefile's
variables, the ``OSDefineMetaClassAndStructors`` calls in the sources, the API
names the sources actually reference -- rather than typed here as a literal. A
second hand-kept copy of a string is the thing being checked for; adding one to
the checker would reproduce the defect inside the gate.

What this does NOT prove
------------------------
Nothing here contacts Apple's SDK. That the *versions* in ``OSBundleLibraries``
exist, and that the KPI names are spelled the way that macOS release spells
them, is unproven until STEP 7 -- see ``OPERATOR-ACTION-step7-build.md``. What
is proven is internal consistency: the plist, the Makefile and the C++ agree
about what this kext is called and what it uses.

Usage
-----
    python tools/check-kext-bundle.py [REPO_ROOT]

Exits 0 and prints a line beginning ``check-kext-bundle: OK`` when every check
passes, 1 and ``check-kext-bundle: FAILED`` otherwise. Tests assert on the
verdict WORD, not only the exit code: a checker that dies before reaching its
verdict also exits non-zero, and the two are not the same evidence.
"""

import glob
import os
import plistlib
import re
import sys

PLIST_REL = "phase0-kext/Info.plist"
MAKEFILE_REL = "phase0-kext/Makefile"
SOURCE_DIR_REL = "phase0-kext"

# A make assignment. `+=` deliberately does not match: appending to CXXFLAGS is
# not an identity definition, and treating it as one would let the last append
# win over the real `:=`.
ASSIGN_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:?=\s*(.*?)\s*$")
MAKE_REF_RE = re.compile(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)")

# OSDefineMetaClassAndStructors(Class, Super) -- the definition that actually
# registers a name with the runtime. OSDeclareDefaultStructors alone is a
# declaration and registers nothing, so IOClass must resolve to the former.
DEFINE_RE = re.compile(
    r"\bOSDefineMetaClassAndStructors\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)")
DECLARE_RE = re.compile(r"\bOSDeclareDefaultStructors\s*\(\s*([A-Za-z_]\w*)\s*\)")

# A bundle identifier macOS will accept: reverse-DNS, no spaces, no underscores.
IDENT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.\-]*$")
# A KPI version. Digits and dots only -- "19.0", not "19.0b1".
VERSION_RE = re.compile(r"^\d+(\.\d+)*$")

#
# Which OSBundleLibraries entry each API family requires. The left-hand pattern
# is matched against the kext sources; if it hits, the library MUST be declared.
#
# Derived-from-source rather than a fixed list on purpose: a fixed list goes
# stale in the safe direction for removals and the UNSAFE direction for
# additions -- start using an IOPCIFamily class without touching the list and
# nothing notices until load time.
#
REQUIRED_LIBRARIES = (
    (re.compile(r"\bIOPCIDevice\b"),
     "com.apple.iokit.IOPCIFamily",
     "IOPCIDevice belongs to IOPCIFamily, which is a separate kext rather than "
     "part of the kernel, so it is never implied by the kpi.* entries"),
    (re.compile(r"\bIOService\b|\bIOMemoryDescriptor\b|\bIOLog\s*\("),
     "com.apple.kpi.iokit",
     "IOService, the memory-descriptor classes and IOLog are all IOKit KPI"),
    (re.compile(r"\bOSDeclareDefaultStructors\b|\bOSDynamicCast\b|\bOSObject\b"),
     "com.apple.kpi.libkern",
     "the OSMetaClass machinery every C++ kext class is built on is libkern"),
)


class Failure(Exception):
    """A condition that stops the run before any check can be evaluated.

    Distinct from an ordinary recorded failure: a missing Info.plist means the
    checks did not run, which must not be reported the same way as checks that
    ran and passed.
    """


def make_variables(text):
    """Every simple assignment in a Makefile, unexpanded."""
    out = {}
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        match = ASSIGN_RE.match(line)
        if match:
            out[match.group(1)] = match.group(2)
    return out


def expand(value, variables, limit=8):
    """Resolve $(FOO) references against `variables`.

    Bounded rather than recursive so a self-referential Makefile cannot hang the
    gate; an unresolved reference is left as-is and fails the identifier check
    loudly instead of silently expanding to an empty string.
    """
    for _ in range(limit):
        expanded = MAKE_REF_RE.sub(
            lambda m: variables.get(m.group(1), m.group(0)), value)
        if expanded == value:
            break
        value = expanded
    return value


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def source_text(root):
    """Every kext source and header, concatenated.

    Headers included: `OSDeclareDefaultStructors` lives in NV3050Probe.hpp while
    its matching define lives in the .cpp, so scanning only one of the two would
    make the IOClass check unable to see the pair it exists to relate.
    """
    parts = []
    for pattern in ("*.cpp", "*.hpp", "*.h"):
        for path in sorted(glob.glob(os.path.join(root, SOURCE_DIR_REL, pattern))):
            parts.append(read_text(path))
    if not parts:
        raise Failure("no sources found under %s/ -- nothing to derive the "
                      "expected IOClass names from" % SOURCE_DIR_REL)
    return "\n".join(parts)


def defined_classes(text):
    """Class names registered with the runtime, and their superclasses."""
    return {m.group(1): m.group(2) for m in DEFINE_RE.finditer(text)}


def declared_classes(text):
    return {m.group(1) for m in DECLARE_RE.finditer(text)}


def load_plist(path):
    try:
        with open(path, "rb") as handle:
            return plistlib.load(handle)
    except FileNotFoundError:
        raise Failure("%s is missing" % path)
    except Exception as exc:                       # plistlib raises several types
        raise Failure("%s does not parse as a property list: %s" % (path, exc))


def check(root):
    """Every check. Returns (failures, checks_run)."""
    failures = []
    run = 0

    plist_path = os.path.join(root, PLIST_REL)
    make_path = os.path.join(root, MAKEFILE_REL)

    plist = load_plist(plist_path)
    if not isinstance(plist, dict):
        raise Failure("%s parses but its root is %s, not a dictionary"
                      % (PLIST_REL, type(plist).__name__))

    if not os.path.isfile(make_path):
        raise Failure("%s is missing -- the expected bundle identity is derived "
                      "from it, so there is nothing to compare against"
                      % MAKEFILE_REL)

    variables = make_variables(read_text(make_path))
    kext_name = expand(variables.get("KEXT_NAME", ""), variables)
    bundle_id = expand(variables.get("BUNDLE_ID", ""), variables)

    if not kext_name:
        raise Failure("%s defines no KEXT_NAME" % MAKEFILE_REL)
    if not bundle_id:
        raise Failure("%s defines no BUNDLE_ID" % MAKEFILE_REL)

    sources = source_text(root)
    defines = defined_classes(sources)
    declares = declared_classes(sources)

    # -- 1. the executable name the Makefile builds --------------------------
    run += 1
    executable = plist.get("CFBundleExecutable")
    if executable != kext_name:
        failures.append(
            "CFBundleExecutable is %r but the Makefile builds %r "
            "(KEXT_NAME) -- the bundle would carry a binary under a name "
            "nothing in Contents/Info.plist points at, and kmutil rejects that "
            "as a missing executable" % (executable, kext_name))

    # -- 2. the identifier, spelled twice ------------------------------------
    run += 1
    identifier = plist.get("CFBundleIdentifier")
    if identifier != bundle_id:
        failures.append(
            "CFBundleIdentifier is %r but the Makefile's BUNDLE_ID is %r -- "
            "`make unload` targets BUNDLE_ID, so a drift here makes unload a "
            "no-op that still exits 0" % (identifier, bundle_id))

    run += 1
    if not isinstance(identifier, str) or not IDENT_RE.match(identifier or ""):
        failures.append(
            "CFBundleIdentifier %r is not a usable bundle identifier -- it must "
            "be reverse-DNS with no spaces or underscores" % (identifier,))

    # -- 3. the package type -------------------------------------------------
    run += 1
    package_type = plist.get("CFBundlePackageType")
    if package_type != "KEXT":
        failures.append(
            "CFBundlePackageType is %r, not 'KEXT' -- the bundle would be "
            "rejected before any personality is even read" % (package_type,))

    # -- 4. the version keys -------------------------------------------------
    for key in ("CFBundleVersion", "CFBundleShortVersionString"):
        run += 1
        value = plist.get(key)
        if not isinstance(value, str) or not VERSION_RE.match(value or ""):
            failures.append(
                "%s is %r, which is not a dotted numeric version -- kmutil "
                "parses this and a bundle it cannot version cannot be staged"
                % (key, value))

    # -- 5. OSBundleLibraries ------------------------------------------------
    run += 1
    libraries = plist.get("OSBundleLibraries")
    if not isinstance(libraries, dict) or not libraries:
        failures.append(
            "OSBundleLibraries is %r -- a C++ kext that declares no libraries "
            "links against nothing and cannot resolve even OSObject"
            % (libraries,))
        libraries = {}

    for name, version in sorted(libraries.items()):
        run += 1
        if not name.startswith("com.apple."):
            failures.append(
                "OSBundleLibraries key %r does not name an Apple KPI or kext -- "
                "nothing would supply it at load time" % (name,))
        run += 1
        if not isinstance(version, str) or not VERSION_RE.match(version or ""):
            failures.append(
                "OSBundleLibraries[%r] is %r, not a dotted numeric version"
                % (name, version))

    for pattern, library, why in REQUIRED_LIBRARIES:
        if not pattern.search(sources):
            continue
        run += 1
        if library not in libraries:
            failures.append(
                "the sources match %s but OSBundleLibraries does not declare "
                "%s -- %s. The failure surfaces as an undefined symbol at load "
                "time on the target machine, not at build time"
                % (pattern.pattern, library, why))

    # The kpi.* libraries are versioned with the kernel as a set. Declaring two
    # different versions among them asks for a kernel that never shipped, so
    # this is a consistency check between the entries themselves -- no version
    # number is typed here, because a typed one would be a third hand-kept copy
    # of exactly the kind this module exists to eliminate.
    kpi_versions = {v for k, v in libraries.items()
                    if k.startswith("com.apple.kpi.")}
    if libraries:
        run += 1
        if len(kpi_versions) > 1:
            failures.append(
                "the com.apple.kpi.* entries declare %d different versions (%s) "
                "-- the kernel KPIs are versioned together, so a mixed set "
                "names a kernel that does not exist"
                % (len(kpi_versions), ", ".join(sorted(kpi_versions))))

    # -- 6. the personalities' identity keys ---------------------------------
    personalities = plist.get("IOKitPersonalities")
    run += 1
    if not isinstance(personalities, dict) or not personalities:
        failures.append(
            "IOKitPersonalities is %r -- a kext with no personality loads and "
            "is never matched against anything, which reads exactly like a "
            "driver that found no GPU" % (personalities,))
        personalities = {}

    for name, body in sorted(personalities.items()):
        run += 1
        if not isinstance(body, dict):
            failures.append("personality %r is %s, not a dictionary"
                            % (name, type(body).__name__))
            continue

        run += 1
        owner = body.get("CFBundleIdentifier")
        if owner != identifier:
            failures.append(
                "personality %r says CFBundleIdentifier %r but the bundle's own "
                "is %r -- IOKit would attribute the personality to a bundle "
                "that does not exist and never instantiate the class"
                % (name, owner, identifier))

        io_class = body.get("IOClass")
        run += 1
        if io_class not in defines:
            extra = ""
            if io_class in declares:
                extra = (" -- it is declared with OSDeclareDefaultStructors but "
                         "has no OSDefineMetaClassAndStructors, so nothing "
                         "registers it with the runtime")
            failures.append(
                "personality %r names IOClass %r, which no "
                "OSDefineMetaClassAndStructors in %s/ defines%s. IOClass is "
                "resolved by STRING through the OSMetaClass registry, so this "
                "links clean and fails at match time on hardware"
                % (name, io_class, SOURCE_DIR_REL, extra))

    return failures, run


def main(argv):
    root = argv[1] if len(argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))

    try:
        failures, run = check(root)
    except Failure as exc:
        print("check-kext-bundle: FAILED -- %s" % exc)
        return 1

    for text in failures:
        print("  %s" % text)

    if failures:
        print("check-kext-bundle: FAILED -- %d of %d checks failed"
              % (len(failures), run))
        return 1

    print("check-kext-bundle: OK -- %d checks, bundle identity agrees with "
          "%s and %s/" % (run, MAKEFILE_REL, SOURCE_DIR_REL))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
