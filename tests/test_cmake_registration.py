#!/usr/bin/env python3
"""
test_cmake_registration.py -- every tests/*.cpp must be registered with ctest.

Why this exists
---------------
``tests/CMakeLists.txt`` registers each C++ suite by hand, one
``add_kext_test(name source)`` call per suite. Nothing checks that the list is
complete. A suite that is written, committed and never registered is never
built and never run -- and ``ctest`` reports ``5/5 tests passed`` while doing
it, because five is all it was ever told about.

That is the same failure shape as the one
``tests/test_check_count_last_line.py`` was written for: a green number that
describes the harness rather than the code. It has already happened in this
repo's history in the adjacent form -- ``check-kext-syntax.bat`` printed a clean
pass while three of five headers went unparsed standalone, and its
``sources scanned : 7`` line came from a different scan entirely (handoff8 §1).
Two numbers, one screen, neither one saying what a reader assumed.

WHAT THIS CHECKS, AND WHAT IT DOES NOT
--------------------------------------
A **static** read of ``tests/CMakeLists.txt`` and a directory listing. No CMake,
no compiler, no ctest -- so it runs on a bare checkout with CPython only, which
is the constraint ``run-python-tests.bat`` is built around.

It proves the registration list is complete and self-consistent. It does **not**
prove the suites build, link, or pass; ``tests\build-msvc.bat`` is what does
that. A green run here means "ctest has been told about every suite", not
"every suite works".

THE NAME/SOURCE PAIRING IS THE SUBTLE ONE
-----------------------------------------
``add_kext_test(test_gsp_stage test_gsp_boot.cpp)`` -- a copy-paste that updates
the target name but not the source -- registers a *sixth* target that compiles
the *fifth* file. ctest then reports six passing tests, the new suite has never
been compiled, and the count went **up**. So the pairing is asserted, not just
the presence of both halves.
"""

import importlib.util
import os
import re
import unittest

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_CMAKELISTS = os.path.join(_TESTS_DIR, "CMakeLists.txt")

# Deliberately not anchored to the start of the line, because the call is
# indented nowhere today but a future edit may indent it -- and an anchored
# pattern silently matching fewer keys is a mistake this project has already
# made once, with '^[a-zA-Z_.]*pciSlotNumber' finding 2 of 9 VMX keys.
_ADD_TEST = re.compile(r"add_kext_test\s*\(\s*([^\s)]+)\s+([^\s)]+)\s*\)")


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _strip_comments(text):
    """Drop CMake ``#`` comments, line by line.

    A commented-out registration must not count as a registration. This file
    contains no quoted ``#``, and a test below asserts that stays true rather
    than leaving the simplification unstated.
    """
    out = []
    for line in text.splitlines():
        hash_at = line.find("#")
        out.append(line if hash_at < 0 else line[:hash_at])
    return "\n".join(out)


def registrations():
    """Return [(target_name, source_file)] in file order, comments excluded."""
    return _ADD_TEST.findall(_strip_comments(_read(_CMAKELISTS)))


def cpp_sources():
    """Return sorted tests/*.cpp basenames.

    Only the tests directory itself -- ``tests/build/CMakeFiles/.../
    CMakeCXXCompilerId.cpp`` is CMake's own probe file, has its own ``main()``,
    and is not a suite. os.listdir does not recurse, which is why it is used
    here in place of a glob.
    """
    return sorted(n for n in os.listdir(_TESTS_DIR) if n.endswith(".cpp"))


def _sibling_gate():
    """Load test_check_count_last_line.py by path, not by module name.

    ``tests/`` has no ``__init__.py`` and ``site-packages/tests/`` shadows the
    name, so an ``import tests.x`` form fails with an error that blames the
    wrong file. Loading by path sidesteps both.
    """
    path = os.path.join(_TESTS_DIR, "test_check_count_last_line.py")
    spec = importlib.util.spec_from_file_location("_sibling_count_gate", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PremiseTests(unittest.TestCase):
    """Guards this file's own assumptions before it asserts anything else."""

    def test_cmakelists_exists(self):
        self.assertTrue(os.path.isfile(_CMAKELISTS), _CMAKELISTS)

    def test_at_least_one_registration_is_found(self):
        """A regex that matches nothing would make every later test vacuous."""
        self.assertTrue(registrations())

    def test_no_quoted_hash_in_cmakelists(self):
        """_strip_comments treats every '#' as a comment; this pins that."""
        for line in _read(_CMAKELISTS).splitlines():
            if "#" in line and '"' in line:
                self.fail("quoted text and '#' on one line: %r" % line)

    def test_at_least_one_cpp_source_is_found(self):
        self.assertTrue(cpp_sources())


class RegistrationCompleteTests(unittest.TestCase):
    """The load-bearing assertion: no suite is left out, none is invented."""

    def test_every_cpp_source_is_registered(self):
        registered = {source for _name, source in registrations()}
        missing = [s for s in cpp_sources() if s not in registered]
        self.assertEqual(
            missing, [],
            "unregistered suite(s) -- ctest will never run these: %s" % missing)

    def test_every_registered_source_exists(self):
        present = set(cpp_sources())
        phantom = [s for _n, s in registrations() if s not in present]
        self.assertEqual(phantom, [],
                         "registered but absent from tests/: %s" % phantom)

    def test_no_source_is_registered_twice(self):
        sources = [s for _n, s in registrations()]
        dupes = sorted({s for s in sources if sources.count(s) > 1})
        self.assertEqual(dupes, [], "registered more than once: %s" % dupes)

    def test_no_target_name_is_reused(self):
        names = [n for n, _s in registrations()]
        dupes = sorted({n for n in names if names.count(n) > 1})
        self.assertEqual(dupes, [], "duplicate target name(s): %s" % dupes)


class NameMatchesSourceTests(unittest.TestCase):
    """``add_kext_test(test_gsp_stage test_gsp_boot.cpp)`` must not pass.

    Both halves are individually valid there -- the name is unused, the source
    exists -- so only the pairing catches it. Left unchecked, ctest reports one
    more passing test than before while the new suite has never been compiled.
    """

    def test_target_name_is_the_source_stem(self):
        for name, source in registrations():
            self.assertEqual(name, os.path.splitext(source)[0],
                             "target %r does not match source %r" % (name, source))


class CrossGateAgreementTests(unittest.TestCase):
    """Keeps the two static gates from drifting apart.

    ``test_check_count_last_line.py`` names the five suites explicitly rather
    than globbing, on purpose. That list and this registration list describe the
    same set, so adding a sixth suite must update BOTH -- and if it updates only
    one, this fails rather than one gate quietly checking four files and the
    other five.
    """

    def test_count_gate_list_matches_the_directory(self):
        self.assertEqual(sorted(_sibling_gate().SUITES), cpp_sources())

    def test_count_gate_list_matches_cmake_registration(self):
        registered = sorted(s for _n, s in registrations())
        self.assertEqual(sorted(_sibling_gate().SUITES), registered)


class CtestPlumbingTests(unittest.TestCase):
    """The helper only counts if it actually wires each target into ctest."""

    def test_enable_testing_is_called(self):
        self.assertIn("enable_testing()", _strip_comments(_read(_CMAKELISTS)))

    def test_helper_calls_add_test(self):
        body = _strip_comments(_read(_CMAKELISTS))
        self.assertIn("add_test(", body)

    def test_helper_calls_add_executable(self):
        body = _strip_comments(_read(_CMAKELISTS))
        self.assertIn("add_executable(", body)

    def test_kext_headers_are_on_the_include_path(self):
        """The whole point is compiling the SAME headers the kext compiles."""
        body = _strip_comments(_read(_CMAKELISTS))
        self.assertIn("phase0-kext", body)


if __name__ == "__main__":
    unittest.main()
