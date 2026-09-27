#!/usr/bin/env python3
"""
test_check_negative_controls.py -- negative controls for
``tools/check-negative-controls.py``.

Why this exists
---------------
The checker under test asserts that the ``#if defined(NVNEG_*)`` blocks in
``tests/kextshim/negative-control.cpp`` and the ``for %%M in (...)`` list in
section 11 of ``tests/check-kext-syntax.bat`` name the same set. A checker for
a drift-prone hand-typed list is itself worth nothing unless something proves
it can go red, so every case below feeds it an input that SHOULD fail and
asserts the verdict word changes.

The subject is loaded by file path rather than imported by name: the tool's
filename is hyphenated and is not a legal Python identifier. That is also what
binds this module to it for ``tools/check-tool-coverage.py`` -- the literal
string "check-negative-controls.py" appears below in code, not in prose.
"""

import importlib.util
import io
import os
import subprocess
import sys
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHECKER = os.path.join(ROOT, "tools", "check-negative-controls.py")
CONTROL_CPP = os.path.join(ROOT, "tests", "kextshim", "negative-control.cpp")
GATE_BAT = os.path.join(ROOT, "tests", "check-kext-syntax.bat")


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def load_checker():
    spec = importlib.util.spec_from_file_location("nv_check_negative_controls", CHECKER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run_main(mod):
    """Call main() and return (exit_code, stdout)."""
    buf = io.StringIO()
    with redirect_stdout(buf):
        code = mod.main()
    return code, buf.getvalue()


class RealTree(unittest.TestCase):
    """The checker against the actual repository. This is the positive control:
    without it, every case below would still pass if the checker rejected
    everything unconditionally."""

    def test_real_tree_passes(self):
        proc = subprocess.run(
            [sys.executable, CHECKER],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
        )
        self.assertIn("VERDICT: PASS", proc.stdout)
        self.assertNotIn("VERDICT: FAIL", proc.stdout)
        self.assertEqual(0, proc.returncode)

    def test_the_two_files_agree_on_a_nonempty_set(self):
        mod = load_checker()
        defined = mod.defined_macros(read(CONTROL_CPP))
        enumerated = mod.enumerated_macros(read(GATE_BAT))
        self.assertIsNotNone(enumerated)
        self.assertTrue(defined, "negative-control.cpp defines no NVNEG_ blocks")
        self.assertEqual(defined, enumerated)

    def test_bmd_controls_are_present(self):
        """IOBufferMemoryDescriptor had zero controls until 2026-08-14. Pin the
        four that closed that hole, so removing one reddens something."""
        mod = load_checker()
        defined = mod.defined_macros(read(CONTROL_CPP))
        for name in (
            "NVNEG_BMD_TYPO_MEMBER",
            "NVNEG_BMD_ARITY",
            "NVNEG_BMD_VOID_PTR",
            "NVNEG_BMD_CONST_PREPARE",
        ):
            self.assertIn(name, defined)


class DefinedMacros(unittest.TestCase):
    def test_finds_guarded_blocks(self):
        mod = load_checker()
        text = "#if defined(NVNEG_ALPHA)\nint a;\n#endif\n"
        self.assertEqual({"NVNEG_ALPHA"}, mod.defined_macros(text))

    def test_tolerates_whitespace_variants(self):
        mod = load_checker()
        text = "  #if   defined ( NVNEG_SPACED )\n#endif\n"
        self.assertEqual({"NVNEG_SPACED"}, mod.defined_macros(text))

    def test_ignores_macro_names_in_comments(self):
        """negative-control.cpp discusses NVNEG_MISSING_STRUCTORS in prose on
        purpose. Counting a comment mention as a definition would demand the
        gate enumerate a block that does not exist -- the opposite defect."""
        mod = load_checker()
        text = (
            "// NVNEG_MISSING_STRUCTORS was here and has been REMOVED.\n"
            "/* see also NVNEG_GHOST */\n"
            "#if defined(NVNEG_REAL)\n#endif\n"
        )
        self.assertEqual({"NVNEG_REAL"}, mod.defined_macros(text))

    def test_ifdef_spelling_is_not_accepted(self):
        """Documents a DELIBERATE limit rather than an accident. The file uses
        `#if defined(...)` throughout; if a future block uses `#ifdef`, this
        test fails and forces the regex to be widened on purpose instead of a
        block hiding from the audit."""
        mod = load_checker()
        self.assertEqual(set(), mod.defined_macros("#ifdef NVNEG_SNEAKY\n#endif\n"))


class EnumeratedMacros(unittest.TestCase):
    def test_reads_the_loop_list(self):
        mod = load_checker()
        text = "for %%M in (NVNEG_A NVNEG_B NVNEG_C) do (\n"
        self.assertEqual({"NVNEG_A", "NVNEG_B", "NVNEG_C"}, mod.enumerated_macros(text))

    def test_missing_loop_returns_none_not_empty(self):
        """None and set() must stay distinguishable. An empty set compares as
        'the gate enumerates nothing', which is a real state; a missing loop
        means the checker no longer understands the file and must refuse."""
        mod = load_checker()
        self.assertIsNone(mod.enumerated_macros("echo hello\r\n"))

    def test_does_not_match_an_unrelated_for_loop(self):
        """check-kext-syntax.bat contains other for-loops. Matching one of them
        would compare against an unrelated list and still print a verdict."""
        mod = load_checker()
        text = (
            "for %%F in (a.hpp b.hpp) do (\n"
            "  echo %%F\n"
            ")\n"
            "for %%M in (NVNEG_ONLY) do (\n"
        )
        self.assertEqual({"NVNEG_ONLY"}, mod.enumerated_macros(text))


class Mutations(unittest.TestCase):
    """Each case points the checker at synthetic file contents by overriding its
    module-level paths, so the real tree is never written to."""

    def _run_with(self, cpp_text, bat_text):
        import tempfile

        mod = load_checker()
        with tempfile.TemporaryDirectory() as d:
            cpp = os.path.join(d, "negative-control.cpp")
            bat = os.path.join(d, "check-kext-syntax.bat")
            with open(cpp, "w", encoding="utf-8") as fh:
                fh.write(cpp_text)
            with open(bat, "w", encoding="utf-8") as fh:
                fh.write(bat_text)
            mod.CONTROL_CPP = cpp
            mod.GATE_BAT = bat
            return run_main(mod)

    def test_defined_but_not_enumerated_fails(self):
        code, out = self._run_with(
            "#if defined(NVNEG_A)\n#endif\n#if defined(NVNEG_ORPHAN)\n#endif\n",
            "for %%M in (NVNEG_A) do (\n",
        )
        self.assertIn("VERDICT: FAIL", out)
        self.assertIn("NVNEG_ORPHAN", out)
        self.assertIn("defined but never run", out)
        self.assertEqual(1, code)

    def test_enumerated_but_not_defined_fails(self):
        """The damaging direction: clang compiles the bare baseline, exits 0,
        and section 11 prints 'ok NVNEG_GONE diagnosed'."""
        code, out = self._run_with(
            "#if defined(NVNEG_A)\n#endif\n",
            "for %%M in (NVNEG_A NVNEG_GONE) do (\n",
        )
        self.assertIn("VERDICT: FAIL", out)
        self.assertIn("NVNEG_GONE", out)
        self.assertIn("enumerated but not defined", out)
        self.assertEqual(1, code)

    def test_both_directions_reported_together(self):
        code, out = self._run_with(
            "#if defined(NVNEG_ONLY_CPP)\n#endif\n",
            "for %%M in (NVNEG_ONLY_BAT) do (\n",
        )
        self.assertIn("NVNEG_ONLY_CPP", out)
        self.assertIn("NVNEG_ONLY_BAT", out)
        self.assertIn("VERDICT: FAIL", out)
        self.assertEqual(1, code)

    def test_missing_loop_fails_rather_than_passing(self):
        code, out = self._run_with(
            "#if defined(NVNEG_A)\n#endif\n",
            "echo section 11 was rewritten\n",
        )
        self.assertIn("VERDICT: FAIL", out)
        self.assertIn("no `for %%M in (...)` loop found", out)
        self.assertEqual(1, code)

    def test_empty_both_sides_is_a_fail_not_a_vacuous_pass(self):
        """Both sets empty compares equal. Without the vacuity guard that would
        print PASS and mean 'there are no negative controls at all'."""
        code, out = self._run_with("// nothing here\n", "for %%M in () do (\n")
        self.assertIn("VERDICT: FAIL", out)
        self.assertIn("nothing was compared", out)
        self.assertEqual(1, code)

    def test_matching_nonempty_sets_pass(self):
        """Positive control for this class: the mutations above must fail for
        the reason claimed, not because the harness always fails."""
        code, out = self._run_with(
            "#if defined(NVNEG_A)\n#endif\n#if defined(NVNEG_B)\n#endif\n",
            "for %%M in (NVNEG_A NVNEG_B) do (\n",
        )
        self.assertIn("VERDICT: PASS", out)
        self.assertNotIn("VERDICT: FAIL", out)
        self.assertEqual(0, code)


class GateWiring(unittest.TestCase):
    def test_gate_invokes_the_checker_and_asserts_the_verdict_word(self):
        """A checker no gate runs is documentation. Assert both that the gate
        calls it and that it greps the verdict rather than trusting errorlevel."""
        bat = read(GATE_BAT)
        self.assertIn("check-negative-controls.py", bat)
        self.assertIn("VERDICT: PASS", bat)


if __name__ == "__main__":
    unittest.main()
