#!/usr/bin/env python3
"""
test_check_tool_coverage.py -- negative controls for
``tools/check-tool-coverage.py``.

Why this exists
---------------
The checker under test asserts that every gate-invoked ``tools/*.py`` has a test
module. Wiring it into ``check-kext-syntax.bat`` makes it subject to its own
rule, which is the correct outcome and not a joke at its own expense: an audit
that exempts itself is exactly the hand-maintained blind spot it was written to
remove.

The deeper reason is the same one that motivated the checker. Running it once
against the real tree and seeing ``4/4`` proves only that it does not crash. A
body of ``return 0`` prints nothing and exits 0 too; so does a checker whose
regex silently stopped matching ``tools/`` invocations, which would report
vacuous success forever. Every test below except the first two therefore builds a
fixture project that is WRONG in one specific way and asserts the checker
rejects it, naming the tool it should have complained about.

Fixture roots, never the real tree
----------------------------------
The defects here -- a gate invoking an untested tool, a gate invoking a tool that
does not exist -- cannot be planted in the real project without breaking the
gates for every other terminal. Each test builds its own ``tempfile.mkdtemp()``
root with a synthetic ``tools/``, ``tests/`` and gate script, so the suite is
safe to run concurrently and mutates nothing.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHECKER = os.path.join(ROOT, "tools", "check-tool-coverage.py")

#
# How a real suite in this project binds to its subject: the hyphenated name
# cannot be imported, so it appears as a string literal handed to
# spec_from_file_location or subprocess. A comment or docstring mentioning the
# same name is NOT a binding, and the checker must not accept one -- see
# ProseIsNotCoverage below for why that distinction is load-bearing.
#
CODE_BINDING = 'SUBJECT = "check-fixture.py"\n'
COMMENT_ONLY = "# this file talks about check-fixture.py and nothing more\n"
DOCSTRING_ONLY = '"""Prose about check-fixture.py. Tests nothing."""\n'


def run_checker(root):
    """Run the checker against project root `root`; return (code, output)."""
    proc = subprocess.run(
        [sys.executable, CHECKER, root],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=ROOT,
    )
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


class Fixtures(unittest.TestCase):
    """Base class that builds synthetic project roots."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="toolcov-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def make_root(self, tools=(), tests=(), gate=None, claude=None,
                  make_tools_dir=True, make_tests_dir=True):
        """Build a project root.

        tools  -- filenames to create under tools/
        tests  -- {filename: text} to create under tests/
        gate   -- text of tests/fixture-gate.bat, or None to omit it
        claude -- text of CLAUDE.md, or None to omit it
        """
        if make_tools_dir:
            os.makedirs(os.path.join(self.tmp, "tools"), exist_ok=True)
            for name in tools:
                self.write(os.path.join("tools", name), "# fixture tool\n")
        if make_tests_dir:
            os.makedirs(os.path.join(self.tmp, "tests"), exist_ok=True)
            for name, text in dict(tests).items():
                self.write(os.path.join("tests", name), text)
        if gate is not None:
            self.write(os.path.join("tests", "fixture-gate.bat"), gate)
        if claude is not None:
            self.write("CLAUDE.md", claude)
        return self.tmp

    def write(self, relative, text):
        path = os.path.join(self.tmp, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        return path

    def good_root(self):
        """A root where the single gate-invoked tool does have a test module.

        The binding is a CODE string literal, not a comment. That is not
        incidental to the fixture -- the checker requires a code reference, so a
        fixture that bound by comment would be rejected and every negative test
        below would pass for the wrong reason.
        """
        return self.make_root(
            tools=["check-fixture.py"],
            tests={"test_check_fixture.py": CODE_BINDING},
            gate="@echo off\r\npython tools/check-fixture.py\r\n",
        )


class RealTreePasses(Fixtures):

    def test_the_real_project_is_currently_covered(self):
        """If the real tree fails, every negative below is moot."""
        code, out = run_checker(ROOT)
        self.assertEqual(code, 0, out)
        self.assertIn("tool coverage:", out)

    def test_a_synthetic_correct_root_also_passes(self):
        """Proves the fixtures are well-formed, not accidentally broken.

        Without this, a negative test could pass for the wrong reason -- every
        fixture rejected because the harness builds unusable roots, not because
        the checker found the planted defect.
        """
        code, out = run_checker(self.good_root())
        self.assertEqual(code, 0, out)
        self.assertIn("check-fixture.py", out)
        self.assertIn("test_check_fixture.py", out)


class UntestedGateToolIsCaught(Fixtures):

    def test_a_gate_invoked_tool_with_no_test_module_is_rejected(self):
        """The defect this checker exists for -- the check-structors.py case."""
        root = self.make_root(
            tools=["check-fixture.py"],
            tests={"test_something_else.py": "# unrelated\n"},
            gate="@echo off\r\npython tools/check-fixture.py\r\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("check-fixture.py", out)
        self.assertIn("no tests/test_*.py", out)

    def test_a_test_module_not_named_test_prefixed_does_not_count(self):
        """Discovery only collects test_*.py, so only those can count as coverage.

        A helper named ``_negctl_*.py`` mentioning the tool would otherwise read
        as coverage while never running under the gate.
        """
        root = self.make_root(
            tools=["check-fixture.py"],
            tests={"_negctl_fixture.py": CODE_BINDING},
            gate="@echo off\r\npython tools/check-fixture.py\r\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("check-fixture.py", out)


class MissingToolIsCaught(Fixtures):

    def test_a_gate_invoking_a_nonexistent_tool_is_rejected(self):
        """A gate line naming a deleted tool would fail at run time.

        Catching it in the audit matters because the audit is what runs when
        someone is reasoning about the gate rather than executing it.
        """
        root = self.make_root(
            tools=[],
            tests={"test_check_gone.py": 'SUBJECT = "check-gone.py"\n'},
            gate="@echo off\r\npython tools/check-gone.py\r\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("check-gone.py", out)
        self.assertIn("no such file exists", out)


class ExemptionIsDeliberateNotAccidental(Fixtures):

    def test_a_tool_no_gate_invokes_does_not_require_tests(self):
        """The probes and one-shot explorers must not be demanded.

        If this check demanded tests for all 23 non-gate tools it would be noise,
        and noise gets suppressed rather than fixed. The exemption is the reason
        the signal stays credible, so it needs its own control.
        """
        root = self.make_root(
            tools=["check-fixture.py", "_probe-thing.py", "one-shot.py"],
            tests={"test_check_fixture.py": CODE_BINDING},
            gate="@echo off\r\npython tools/check-fixture.py\r\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 0, out)
        self.assertIn("not gate-invoked", out)
        self.assertIn("_probe-thing.py", out)
        self.assertIn("one-shot.py", out)


class BothInvocationFormsAreRead(Fixtures):

    def test_a_backslash_path_is_recognised(self):
        """Gate scripts are cmd.exe; a backslash invocation is legitimate.

        If only forward slashes matched, a tool invoked as tools\\x.py would be
        silently exempt -- the same silence, one character wide.
        """
        root = self.make_root(
            tools=["check-fixture.py"],
            tests={"test_unrelated.py": "# unrelated\n"},
            gate="@echo off\r\npython tools\\check-fixture.py\r\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("check-fixture.py", out)

    def test_claude_md_counts_as_a_gate_source(self):
        """Two of the four real gates are documented in CLAUDE.md, not a .bat.

        check-registers.py is invoked from the CLAUDE.md gate block only. If
        CLAUDE.md were not read, that tool would drop out of the required set
        and the check would quietly cover less than it claims.
        """
        root = self.make_root(
            tools=["check-fixture.py"],
            tests={"test_unrelated.py": "# unrelated\n"},
            gate="@echo off\r\nREM no tool invocations here\r\n",
            claude="```sh\npython tools/check-fixture.py\n```\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("check-fixture.py", out)
        self.assertIn("CLAUDE.md", out)


class VacuousPassIsRefused(Fixtures):

    def test_no_gate_scripts_at_all_is_a_failure(self):
        """With nothing to read, a naive implementation reports success.

        Zero required tools trivially all have tests. That is the shape of the
        bug this whole family of checkers exists to break, so it must fail.
        """
        root = self.make_root(tools=["check-fixture.py"], tests={})
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("no gate scripts found", out)

    def test_gates_that_invoke_no_tool_is_a_failure(self):
        """Gate scripts present but matching nothing -- a broken regex looks like this."""
        root = self.make_root(
            tools=["check-fixture.py"],
            tests={},
            gate="@echo off\r\nREM nothing invoked\r\n",
            claude="no code fence here\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("no tools/*.py invocation", out)


class ProseIsNotCoverage(Fixtures):
    """A docstring or comment naming the tool must not count as a test.

    This is not a hypothetical hardening. The first version of the checker
    matched the tool name anywhere in the file's text, and on its very first run
    against the real tree it reported ``check-registers.py`` as covered by
    ``test_check_tool_coverage.py`` -- this file -- purely because this file's
    own docstrings name it while testing nothing of the sort. Under that reading,
    deleting ``test_check_structors.py`` would have left the gate green on the
    strength of a sentence.
    """

    def test_a_comment_mentioning_the_tool_is_not_coverage(self):
        root = self.make_root(
            tools=["check-fixture.py"],
            tests={"test_prose_only.py": COMMENT_ONLY},
            gate="@echo off\r\npython tools/check-fixture.py\r\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("check-fixture.py", out)
        self.assertIn("no tests/test_*.py", out)

    def test_a_docstring_mentioning_the_tool_is_not_coverage(self):
        root = self.make_root(
            tools=["check-fixture.py"],
            tests={"test_prose_only.py": DOCSTRING_ONLY},
            gate="@echo off\r\npython tools/check-fixture.py\r\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("check-fixture.py", out)
        self.assertIn("no tests/test_*.py", out)

    def test_a_code_literal_in_the_same_file_does_count(self):
        """The paired positive -- otherwise the two above could pass because the
        checker rejects everything, which is the opposite failure.
        """
        root = self.make_root(
            tools=["check-fixture.py"],
            tests={"test_prose_only.py": DOCSTRING_ONLY + COMMENT_ONLY
                   + CODE_BINDING},
            gate="@echo off\r\npython tools/check-fixture.py\r\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 0, out)
        self.assertIn("test_prose_only.py", out)


class UnparseableTestModuleIsReported(Fixtures):

    def test_a_test_module_that_will_not_parse_is_a_failure(self):
        """It cannot count as coverage and will not run under discovery either.

        Silently skipping it would be the worst option: the file sits in tests/
        looking like coverage while contributing nothing anywhere.
        """
        root = self.make_root(
            tools=["check-fixture.py"],
            tests={"test_check_fixture.py": CODE_BINDING,
                   "test_broken.py": "def (this is not python\n"},
            gate="@echo off\r\npython tools/check-fixture.py\r\n",
        )
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("test_broken.py", out)
        self.assertIn("does not parse", out)


class BadInputIsReportedNotCrashed(Fixtures):

    def test_a_missing_tools_directory_is_a_clean_failure(self):
        root = self.make_root(
            tools=[], tests={}, gate="@echo off\r\n", make_tools_dir=False)
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("tools directory not found", out)
        self.assertNotIn("Traceback", out)

    def test_a_missing_tests_directory_is_a_clean_failure(self):
        root = self.make_root(
            tools=["check-fixture.py"], tests={}, make_tests_dir=False)
        code, out = run_checker(root)
        self.assertEqual(code, 1, out)
        self.assertIn("tests directory not found", out)
        self.assertNotIn("Traceback", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
