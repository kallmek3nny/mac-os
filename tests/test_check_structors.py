#!/usr/bin/env python3
"""
test_check_structors.py -- negative controls for ``tools/check-structors.py``.

Why this exists
---------------
``check-structors.py`` runs as section 4 of ``tests/check-kext-syntax.bat`` and
had **zero** test coverage until now -- the gap that
``tools/check-tool-coverage.py`` was written to detect, and the first thing it
found.

That gap mattered more than an ordinary untested script, because this checker is
an *evidence producer*. When it is wrong it does not crash; it prints
``1/1 passed, 0 failed`` and the gate goes green. Seeing it exit 0 against the
real kext proves only that it does not crash -- a body of ``return 0`` passes
that same test, and so does a checker whose regexes silently match nothing.

So every test below except the first two builds a fixture kext tree that is
WRONG in one specific way and asserts the checker rejects it, naming the class it
should have complained about. That is what the optional directory argument on the
checker's command line is for.

Why fixture trees rather than the real one
-----------------------------------------
The defects under test -- a missing metaclass registration, a superclass
mismatch, a duplicate declaration -- cannot be planted in ``phase0-kext/``
without breaking the tree for every other terminal. Each test writes its own
``tempfile.mkdtemp()`` directory instead, so the suite is safe to run
concurrently and never mutates the project.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHECKER = os.path.join(ROOT, "tools", "check-structors.py")
REAL_KEXT = os.path.join(ROOT, "phase0-kext")

#
# A minimal well-formed pair: declaration in the header, definition in the
# source, and the superclass named in the metaclass macro matching the one the
# class actually derives from.
#
GOOD_HPP = """\
class NVFixtureProbe : public IOService
{
    OSDeclareDefaultStructors(NVFixtureProbe)
public:
    virtual bool start(IOService *provider);
};
"""

GOOD_CPP = """\
#include "NVFixtureProbe.hpp"

OSDefineMetaClassAndStructors(NVFixtureProbe, IOService)

bool NVFixtureProbe::start(IOService *provider)
{
    return true;
}
"""


def run_checker(kext_dir):
    """Run the checker against `kext_dir`; return (exit_code, combined_output)."""
    proc = subprocess.run(
        [sys.executable, CHECKER, kext_dir],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=ROOT,
    )
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


class Fixtures(unittest.TestCase):
    """Base class providing a scratch kext tree per test."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="structors-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def write(self, name, text):
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        return path

    def good_tree(self):
        self.write("NVFixtureProbe.hpp", GOOD_HPP)
        self.write("NVFixtureProbe.cpp", GOOD_CPP)
        return self.tmp


class RealKextPasses(Fixtures):

    def test_the_real_kext_is_currently_correct(self):
        """If the real tree fails, every negative below is moot."""
        code, out = run_checker(REAL_KEXT)
        self.assertEqual(code, 0, out)
        self.assertIn("NV3050Probe : IOService", out)

    def test_a_synthetic_correct_tree_also_passes(self):
        """Proves the fixtures are well-formed, not accidentally broken.

        Without this, every negative test could be passing for the wrong
        reason -- rejected because the harness emits malformed C++, not because
        the checker found the planted defect.
        """
        code, out = run_checker(self.good_tree())
        self.assertEqual(code, 0, out)
        self.assertIn("NVFixtureProbe : IOService", out)
        self.assertIn("1/1 passed", out)


class MissingDefinitionIsCaught(Fixtures):

    def test_declared_but_never_defined(self):
        """The defect the checker exists for: builds and loads, never instantiates.

        clang cannot catch this -- the code is valid C++ either way. Nothing but
        this textual check stands between the project and a kext that loads and
        silently does nothing.
        """
        self.write("NVFixtureProbe.hpp", GOOD_HPP)
        self.write("NVFixtureProbe.cpp",
                   GOOD_CPP.replace(
                       "OSDefineMetaClassAndStructors(NVFixtureProbe, IOService)",
                       "// metaclass registration deliberately absent"))
        code, out = run_checker(self.tmp)
        self.assertEqual(code, 1, out)
        self.assertIn("NVFixtureProbe", out)
        self.assertIn("no\nOSDefineMetaClassAndStructors".replace("\n", " "), out)


class MissingDeclarationIsCaught(Fixtures):

    def test_defined_but_never_declared(self):
        """The reverse direction -- a registration with no class-body declaration."""
        self.write("NVFixtureProbe.hpp",
                   GOOD_HPP.replace(
                       "    OSDeclareDefaultStructors(NVFixtureProbe)\n", ""))
        self.write("NVFixtureProbe.cpp", GOOD_CPP)
        code, out = run_checker(self.tmp)
        self.assertEqual(code, 1, out)
        self.assertIn("NVFixtureProbe", out)
        self.assertIn("matching OSDeclareDefaultStructors", out)


class SuperclassMismatchIsCaught(Fixtures):

    def test_metaclass_names_a_different_superclass(self):
        """The cross-check that a pairing count alone would miss.

        A checker that only counted declarations against definitions would pass
        this: one of each. Naming the wrong superclass in the metaclass macro is
        a distinct defect, so it needs its own control.
        """
        self.write("NVFixtureProbe.hpp", GOOD_HPP)
        self.write("NVFixtureProbe.cpp",
                   GOOD_CPP.replace(
                       "OSDefineMetaClassAndStructors(NVFixtureProbe, IOService)",
                       "OSDefineMetaClassAndStructors(NVFixtureProbe, IOUserClient)"))
        code, out = run_checker(self.tmp)
        self.assertEqual(code, 1, out)
        self.assertIn("derives from IOService", out)
        self.assertIn("IOUserClient", out)


class DuplicatesAreCaught(Fixtures):

    def test_declared_twice_in_two_files(self):
        """Two declarations of one class -- the header included twice, copy-pasted."""
        self.good_tree()
        self.write("NVFixtureProbeCopy.hpp", GOOD_HPP)
        code, out = run_checker(self.tmp)
        self.assertEqual(code, 1, out)
        self.assertIn("declared structors twice", out)

    def test_defined_twice_in_two_files(self):
        """Duplicate metaclass registration -- distinct from a duplicate declaration."""
        self.good_tree()
        self.write("NVFixtureProbeSecond.cpp", GOOD_CPP)
        code, out = run_checker(self.tmp)
        self.assertEqual(code, 1, out)
        self.assertIn("defines structors twice", out)
        self.assertIn("duplicate metaclass registration", out)


class AbstractStructorsAreRecognised(Fixtures):

    def test_osdeclareabstractstructors_is_also_required_to_pair(self):
        """The DECLARE regex accepts Default and Abstract; prove Abstract is real.

        An alternation branch no test exercises is an assumption, not a feature.
        """
        self.write("NVFixtureProbe.hpp",
                   GOOD_HPP.replace("OSDeclareDefaultStructors",
                                    "OSDeclareAbstractStructors"))
        self.write("NVFixtureProbe.cpp",
                   GOOD_CPP.replace(
                       "OSDefineMetaClassAndStructors(NVFixtureProbe, IOService)",
                       "// no registration"))
        code, out = run_checker(self.tmp)
        self.assertEqual(code, 1, out)
        self.assertIn("NVFixtureProbe", out)


class BadInputIsReportedNotCrashed(Fixtures):

    def test_a_missing_directory_is_a_clean_failure(self):
        missing = os.path.join(self.tmp, "no-such-dir")
        code, out = run_checker(missing)
        self.assertEqual(code, 1, out)
        self.assertIn("kext directory not found", out)
        self.assertNotIn("Traceback", out)

    def test_an_empty_directory_fails_rather_than_reporting_zero_of_zero(self):
        """An empty tree must not read as success.

        This is the failure mode that made the header-coverage gap invisible:
        finding nothing and printing a pass. A checker that scanned no sources
        has not verified anything.
        """
        code, out = run_checker(self.tmp)
        self.assertEqual(code, 1, out)
        self.assertIn("no sources found", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
