#!/usr/bin/env python3
"""
test_check_header_coverage.py -- negative controls for
``tools/check-header-coverage.py``.

Why this exists
---------------
check-header-coverage.py exists because a hand-written list in a batch file
fails SILENTLY -- it prints ok for what it knows about and says nothing about
what it does not. A checker written to catch a silent failure, and then never
proven capable of failing itself, has simply moved the silence one level up.
Running it once against the real gate and seeing exit 0 proves only that it does
not crash; a script whose body was ``return 0`` would pass that same test.

So every test below except the first constructs a gate file that is WRONG in one
specific way and asserts the checker rejects it, naming the header it should
have complained about. That is the whole point of the optional path argument on
the checker's command line.

No fixture here writes into phase0-kext/. The headers are read from the real
tree deliberately -- the checker's job is to compare a gate script against the
headers that actually exist, so faking the headers would test a different
function than the one that runs in the gate.
"""

import os
import subprocess
import sys
import tempfile
import shutil
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHECKER = os.path.join(ROOT, "tools", "check-header-coverage.py")
REAL_GATE = os.path.join(HERE, "check-kext-syntax.bat")
KEXT_DIR = os.path.join(ROOT, "phase0-kext")

#
# The line shapes the checker matches. Kept here rather than imported so that a
# change to the checker's regex has to be reflected in two places by a human --
# a test that derives its input from the code under test cannot contradict it.
#
STANDALONE_LINE = '"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/{name}\n'
COMPILE_LINE = '"!CLANG!" %FLAGS% phase0-kext/{name}\n'


def self_contained_headers():
    """Headers with no angle-bracket include -- the ones section 1 must parse."""
    out = []
    for name in sorted(os.listdir(KEXT_DIR)):
        if not name.endswith(".hpp"):
            continue
        with open(os.path.join(KEXT_DIR, name), encoding="utf-8",
                  errors="replace") as handle:
            text = handle.read()
        angle = any(line.lstrip().startswith("#include <")
                    for line in text.splitlines())
        if not angle:
            out.append(name)
    return out


def dependent_headers():
    """Headers WITH an angle-bracket include -- exempt from standalone parse."""
    every = [n for n in sorted(os.listdir(KEXT_DIR)) if n.endswith(".hpp")]
    return [n for n in every if n not in self_contained_headers()]


def run_checker(gate_path):
    """Run the checker on `gate_path`; return (exit_code, combined_output)."""
    proc = subprocess.run(
        [sys.executable, CHECKER, gate_path],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=ROOT,
    )
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


class Fixtures(unittest.TestCase):
    """Base class providing a scratch directory for synthetic gate files."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="hdrcov-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def write_gate(self, body):
        path = os.path.join(self.tmp, "fixture-gate.bat")
        with open(path, "w", encoding="utf-8", newline="\r\n") as handle:
            handle.write(body)
        return path

    def kext_sources(self):
        """Every .cpp in phase0-kext/ -- section 3 of the real gate compiles all.

        Derived from the directory rather than typed. A dependent header's
        exemption is "some compiled source pulls it in", and WHICH source that
        is depends on which IOKit shells exist. This used to spell
        NV3050Probe.cpp as a literal, which made the fixture wrong the moment a
        second shell landed: IntelDisplayProbe.hpp was then reached by nothing,
        and the "a correct gate passes" control went red against a checker that
        was right. The failure looked like a checker bug and was a fixture bug.
        """
        return sorted(n for n in os.listdir(KEXT_DIR) if n.endswith(".cpp"))

    def full_gate(self, omit=(), extra=""):
        """A gate that is correct except for whatever the caller changes."""
        body = "@echo off\n"
        for name in self_contained_headers():
            if name in omit:
                continue
            body += STANDALONE_LINE.format(name=name)
        for name in self.kext_sources():
            body += COMPILE_LINE.format(name=name)
        body += extra
        return self.write_gate(body)


class RealGatePasses(Fixtures):

    def test_the_checked_in_gate_is_currently_correct(self):
        """The real gate must pass -- otherwise every negative below is moot."""
        code, out = run_checker(REAL_GATE)
        self.assertEqual(code, 0, out)
        self.assertIn("header coverage:", out)

    def test_a_synthetic_correct_gate_also_passes(self):
        """Proves the fixtures themselves are well-formed, not accidentally bad.

        Without this, a negative test could pass for the wrong reason -- every
        fixture rejected because the harness builds malformed gates, not
        because the checker found the planted defect.
        """
        code, out = run_checker(self.full_gate())
        self.assertEqual(code, 0, out)


class MissingLineIsCaught(Fixtures):

    def test_every_self_contained_header_is_individually_required(self):
        """Drop one header's line at a time; each omission must be caught.

        Looping over all of them rather than spot-checking one matters: a
        checker that hardcoded a single name would pass a one-header test.
        """
        headers = self_contained_headers()
        self.assertGreater(len(headers), 1, "need >1 header for this to mean anything")
        for name in headers:
            with self.subTest(omitted=name):
                code, out = run_checker(self.full_gate(omit=(name,)))
                self.assertEqual(code, 1, out)
                self.assertIn(name, out)
                self.assertIn("never parses it standalone", out)

    def test_an_empty_gate_fails_rather_than_reporting_zero_of_zero(self):
        """A gate with no clang lines is the maximal version of the bug."""
        code, out = run_checker(self.write_gate("@echo off\n"))
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL", out)


class StaleLineIsCaught(Fixtures):

    def test_a_line_naming_a_deleted_header_is_reported(self):
        """The reverse direction: the gate claims coverage it does not have."""
        extra = STANDALONE_LINE.format(name="NVDoesNotExist.hpp")
        code, out = run_checker(self.full_gate(extra=extra))
        self.assertEqual(code, 1, out)
        self.assertIn("NVDoesNotExist.hpp", out)
        self.assertIn("Stale line", out)


class ExemptionIsDerivedNotAssumed(Fixtures):

    def test_a_dependent_header_parsed_standalone_is_rejected(self):
        """%HDRFLAGS% has no shim on its include path, so this cannot work.

        The gate would fail loudly at clang anyway -- but it should be caught
        by the audit too, because the audit is what runs when someone is
        reasoning about the list rather than executing it.
        """
        dependent = dependent_headers()
        self.assertTrue(dependent, "expected at least one IOKit-dependent header")
        for name in dependent:
            with self.subTest(header=name):
                extra = STANDALONE_LINE.format(name=name)
                code, out = run_checker(self.full_gate(extra=extra))
                self.assertEqual(code, 1, out)
                self.assertIn(name, out)
                self.assertIn("cannot parse", out)

    def test_an_exempt_header_covered_by_nothing_is_rejected(self):
        """Skipping standalone is only legitimate if a compiled source pulls it in.

        Here the %FLAGS% line is removed, so NV3050Probe.hpp is parsed by
        neither section. That is the case the exemption must NOT silently
        permit -- it is exactly the silence this checker exists to break.
        """
        body = "@echo off\n"
        for name in self_contained_headers():
            body += STANDALONE_LINE.format(name=name)
        code, out = run_checker(self.write_gate(body))
        self.assertEqual(code, 1, out)
        self.assertIn("parsed by nothing", out)
        for name in dependent_headers():
            self.assertIn(name, out)


class BadInputIsReportedNotCrashed(Fixtures):

    def test_a_missing_gate_file_is_a_clean_failure(self):
        missing = os.path.join(self.tmp, "no-such-gate.bat")
        code, out = run_checker(missing)
        self.assertEqual(code, 1, out)
        self.assertIn("gate script not found", out)
        self.assertNotIn("Traceback", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
