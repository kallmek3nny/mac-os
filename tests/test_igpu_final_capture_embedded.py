#!/usr/bin/env python3
r"""test_igpu_final_capture_embedded.py -- the embedded Python must be a program.

Why this exists
---------------
``tools/igpu-final-capture.sh`` is a shell file with 295 lines of Python inside
a single quoted heredoc. ``intel-handoff27.md`` section 4 is honest that NO gate
covers the embedded half: both halves were checked BY HAND once (``sh -n`` on
the outside, extract-and ``compile()`` on the inside) and never since.
``intel-handoff27.md`` section 7 item 2 files that gap, and it has been inherited
unchanged through handoffs 28 and 38.

A hand check is a measurement with a date on it. This module is the same two
checks run every time the gate runs, plus the things a hand check does not do.

WHAT MAKES THIS FILE DIFFERENT FROM AN ORDINARY SYNTAX CHECK
------------------------------------------------------------
The script gets ONE run. The laptop it targets is being overwritten with macOS
(``intel-handoff28.md`` section 0), so a ``SyntaxError`` or a ``NameError``
discovered at capture time is not a bug to fix -- it is evidence that no longer
exists. The asymmetry is total: every other script in this tree can be re-run.

So the interesting failure is not "it does not parse". It is:

  * a name used and never bound, which ``compile()`` cannot see because Python
    resolves globals at RUN time -- and the run is the one thing there is no
    second of;
  * the heredoc losing its QUOTING, which turns ``$OUT`` and ``$(basename ...)``
    inside the Python into shell expansions and silently rewrites the program
    before python3 ever sees it;
  * a ``print`` format string whose placeholder count disagrees with its
    argument tuple -- a ``TypeError`` 300 lines into a capture that cannot be
    repeated.

PROPERTIES PINNED
-----------------
1. The heredoc delimiter is QUOTED, so the body reaches python3 verbatim.
2. The extracted body COMPILES.
3. Every global name the body READS is either bound in the body, imported by
   it, or a Python builtin -- checked by walking the AST, not by grepping.
4. Every ``%``-format print has a placeholder count matching its argument
   tuple.
5. The body is the size the docs claim: it is the ONLY heredoc in the file, and
   it is not empty.
6. The shell half parses (``sh -n``) when a shell is available, and SKIPS
   loudly rather than passing quietly when one is not.
"""

import ast
import builtins
import os
import re
import shutil
import subprocess
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SUBJECT = os.path.join(ROOT, "tools", "igpu-final-capture.sh")

DELIM = "PYEOF"

# The heredoc opener, with the delimiter QUOTED. Quoting is the whole property:
# <<'PYEOF' passes the body through untouched, while a bare <<PYEOF makes the
# shell expand $VAR, `cmd` and \ inside what is supposed to be a Python program.
OPEN_RE = re.compile(r"<<\s*(['\"])" + DELIM + r"\1\s*$", re.MULTILINE)

# The unquoted form, matched only so the failure can NAME it.
OPEN_BARE_RE = re.compile(r"<<-?\s*" + DELIM + r"\s*$", re.MULTILINE)


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def heredoc_body(source):
    """Return (body, first_line_number) for the one embedded Python heredoc.

    Refuses rather than returns empty when the anchor has moved. A gate that
    reports nothing when its own subject is gone is indistinguishable from a
    green one, and this subject is a file somebody may reasonably restructure.
    """
    lines = source.splitlines()
    starts = [i for i, line in enumerate(lines)
              if OPEN_RE.search(line) or OPEN_BARE_RE.search(line)]
    if not starts:
        raise AssertionError(
            "REFUSING TO PASS: no %s heredoc in %s. The embedded Python this "
            "module exists to check is not where it was." % (DELIM, SUBJECT))
    if len(starts) != 1:
        raise AssertionError(
            "REFUSING TO PASS: %d %s heredocs in %s, expected exactly 1 -- "
            "this module checks one body and would silently ignore the rest."
            % (len(starts), DELIM, SUBJECT))
    start = starts[0]
    end = None
    for i in range(start + 1, len(lines)):
        if lines[i].strip() == DELIM:
            end = i
            break
    if end is None:
        raise AssertionError(
            "REFUSING TO PASS: %s heredoc opened at line %d and never closed"
            % (DELIM, start + 1))
    return "\n".join(lines[start + 1:end]), start + 2


def bound_names(tree):
    """Every global name the module BINDS: assignment, import, def, class, for.

    Comprehension and function-local bindings are collected too. That is
    deliberately permissive: this walk exists to catch a name that is never
    bound ANYWHERE, which is the defect that costs the one boot. Scope-accurate
    resolution would redden on correct code that shadows, and a gate that
    reddens on correct code gets deleted.
    """
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store):
            names.add(node.id)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                               ast.ClassDef)):
            names.add(node.name)
            for arg in getattr(node, "args", ast.arguments(
                    posonlyargs=[], args=[], vararg=None, kwonlyargs=[],
                    kw_defaults=[], kwarg=None, defaults=[])).args:
                names.add(arg.arg)
            if getattr(node, "args", None) is not None:
                if node.args.vararg is not None:
                    names.add(node.args.vararg.arg)
                if node.args.kwarg is not None:
                    names.add(node.args.kwarg.arg)
                for arg in node.args.posonlyargs + node.args.kwonlyargs:
                    names.add(arg.arg)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                names.add((alias.asname or alias.name).split(".")[0])
        elif isinstance(node, ast.ExceptHandler) and node.name:
            names.add(node.name)
        elif isinstance(node, ast.withitem):
            for sub in ast.walk(node):
                if isinstance(sub, ast.Name) and isinstance(sub.ctx, ast.Store):
                    names.add(sub.id)
    return names


def read_names(tree):
    """Every name the body LOADS."""
    return {node.id for node in ast.walk(tree)
            if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load)}


def unbound_names(body):
    tree = ast.parse(body)
    known = bound_names(tree) | set(dir(builtins))
    return sorted(read_names(tree) - known)


# A %-conversion that consumes an argument. %% is an escaped literal percent and
# consumes nothing, which is why it cannot simply be counted with the rest.
_CONV_RE = re.compile(r"%[-+ #0]*[\d*]*(?:\.[\d*]+)?[hlL]?([diouxXeEfFgGcrsa%])")


def placeholder_count(fmt):
    return sum(1 for m in _CONV_RE.finditer(fmt) if m.group(1) != "%")


def format_mismatches(body):
    """Find `'fmt' % (a, b)` whose placeholder count disagrees with the tuple.

    Only literal-format / literal-tuple pairs are judged. A non-literal right
    operand is skipped rather than guessed at, because a wrong guess here would
    redden correct code and this gate is worth nothing the day somebody deletes
    it for crying wolf.
    """
    problems = []
    tree = ast.parse(body)
    for node in ast.walk(tree):
        if not isinstance(node, ast.BinOp) or not isinstance(node.op, ast.Mod):
            continue
        left = node.left
        if isinstance(left, ast.JoinedStr):
            continue
        # A format string may be written as adjacent literals across a line
        # wrap; ast folds those into one Constant, so nothing extra is needed.
        if not (isinstance(left, ast.Constant) and isinstance(left.value, str)):
            continue
        want = placeholder_count(left.value)
        right = node.right
        if isinstance(right, ast.Tuple):
            have = len(right.elts)
            if any(isinstance(e, ast.Starred) for e in right.elts):
                continue
        elif isinstance(right, ast.Constant) or isinstance(right, ast.Name):
            # One non-tuple operand feeds exactly one placeholder -- unless the
            # name holds a tuple at run time, which is why only the literal and
            # simple-name cases are judged and only when want > 1 is impossible.
            if want <= 1:
                continue
            have = 1
        else:
            continue
        if want != have:
            problems.append(
                "line %d: %d placeholders, %d arguments -- %r"
                % (node.lineno, want, have, left.value[:60]))
    return problems


class HeredocQuotingTests(unittest.TestCase):
    """Property 1. The delimiter must be QUOTED.

    An unquoted heredoc expands $VAR, backticks and backslashes in the body
    before python3 sees it. This body contains `'0x%06X %s' % (o, h(v))` and a
    dozen other constructs the shell would happily chew on, and the damage is
    silent: the script still runs, and it runs a DIFFERENT program.
    """

    def setUp(self):
        self.source = read(SUBJECT)

    def test_the_delimiter_is_quoted(self):
        self.assertIsNotNone(
            OPEN_RE.search(self.source),
            "the %s heredoc delimiter is not quoted -- the shell will expand "
            "the Python body before python3 receives it" % DELIM)

    def test_there_is_exactly_one_embedded_program(self):
        body, _ = heredoc_body(self.source)
        self.assertGreater(len(body.splitlines()), 200,
                           "the embedded program shrank by more than half; "
                           "this module may be checking the wrong body")


class EmbeddedProgramTests(unittest.TestCase):
    """Properties 2, 3 and 4, over the extracted body."""

    def setUp(self):
        self.source = read(SUBJECT)
        self.body, self.first_line = heredoc_body(self.source)

    def test_it_compiles(self):
        try:
            compile(self.body, SUBJECT + " (embedded)", "exec")
        except SyntaxError as exc:
            self.fail("the embedded Python does not compile: line %s: %s"
                      % (exc.lineno, exc.msg))

    def test_every_name_it_reads_is_bound(self):
        """The check compile() cannot make.

        Python resolves globals at RUN time, so a typo'd name is a clean
        compile and a NameError at capture time -- on a machine that gets one
        boot. This is the whole reason the module is worth more than `sh -n`.
        """
        missing = unbound_names(self.body)
        self.assertEqual(
            missing, [],
            "names read but never bound: %s -- each is a NameError on the one "
            "boot this script gets" % ", ".join(missing))

    def test_format_strings_match_their_arguments(self):
        problems = format_mismatches(self.body)
        self.assertEqual(problems, [], "\n  ".join([""] + problems))

    def test_it_reads_and_never_writes(self):
        """The script's own header promises read-only. Pin it.

        A capture that writes to a BAR is not a capture; it is a mode set on a
        panel somebody is looking at. The promise is in the file's first ten
        lines and nothing checked it.
        """
        self.assertIn("mmap.PROT_READ", self.body)
        self.assertIn("os.O_RDONLY", self.body)
        self.assertNotIn("PROT_WRITE", self.body)
        self.assertNotIn("O_RDWR", self.body)
        self.assertNotIn("MAP_POPULATE", self.body)


class ShellHalfTests(unittest.TestCase):
    """Property 6. The outer shell must parse -- and must SKIP loudly."""

    def test_the_shell_half_parses(self):
        shell = shutil.which("sh") or shutil.which("bash")
        if shell is None:
            self.skipTest("no sh/bash on this host -- the shell half is "
                          "UNCHECKED on this run, not proven")
        proc = subprocess.run([shell, "-n", SUBJECT],
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
        self.assertEqual(proc.returncode, 0,
                         proc.stdout.decode("utf-8", "replace"))


class NegativeControlTests(unittest.TestCase):
    """Each control mutates the source IN MEMORY and asserts WHICH property
    breaks. The live tree is never written to.

    Every control asserts the failure TEXT rather than merely that something
    raised: a control that reddens a property other than its own is not
    isolating anything."""

    def setUp(self):
        self.source = read(SUBJECT)
        self.body, _ = heredoc_body(self.source)

    def mutate(self, old, new, count=1):
        self.assertIn(old, self.source,
                      "mutation anchor absent -- the control would no-op and "
                      "read as proof the property is vacuous")
        self.assertEqual(self.source.count(old), count,
                         "mutation anchor occurs %d times, expected %d -- an "
                         "over-matching anchor reddens more than it means"
                         % (self.source.count(old), count))
        return self.source.replace(old, new)

    def test_unquoting_the_delimiter_reddens_property_1(self):
        mutated = self.mutate("<<'PYEOF'", "<<PYEOF")
        self.assertIsNone(OPEN_RE.search(mutated))
        # The body must still be FOUND, or this control would be passing by
        # breaking extraction rather than by breaking quoting.
        body, _ = heredoc_body(mutated)
        self.assertGreater(len(body.splitlines()), 200)

    def test_a_broken_statement_reddens_property_2(self):
        mutated = self.body.replace("total_live = 0", "total_live = = 0", 1)
        self.assertNotEqual(mutated, self.body)
        with self.assertRaises(SyntaxError):
            compile(mutated, "<mutated>", "exec")

    def test_a_typod_name_reddens_property_3(self):
        """The control that justifies the module.

        `verdict_null` -> `verdict_nul` compiles CLEANLY and raises NameError
        only when the branch executes -- which, on this script, is during the
        one capture.
        """
        mutated = self.body.replace("state = 'LIVE' if words else verdict_null",
                                    "state = 'LIVE' if words else verdict_nul",
                                    1)
        self.assertNotEqual(mutated, self.body, "anchor did not match")
        compile(mutated, "<mutated>", "exec")  # still compiles -- that is the point
        self.assertIn("verdict_nul", unbound_names(mutated))

    def test_a_dropped_format_argument_reddens_property_4(self):
        mutated = self.body.replace(
            "print('BAR0 file size 0x%X, mapped 0x%X' % (size, span))",
            "print('BAR0 file size 0x%X, mapped 0x%X' % (size,))", 1)
        self.assertNotEqual(mutated, self.body, "anchor did not match")
        problems = format_mismatches(mutated)
        self.assertTrue(any("2 placeholders, 1 arguments" in p
                            for p in problems), problems)

    def test_a_write_mapping_reddens_the_readonly_property(self):
        mutated = self.body.replace("mmap.PROT_READ",
                                    "mmap.PROT_READ | mmap.PROT_WRITE", 1)
        self.assertNotEqual(mutated, self.body)
        self.assertIn("PROT_WRITE", mutated)

    def test_a_second_heredoc_refuses(self):
        mutated = self.source + "\npython3 - <<'PYEOF'\nprint(1)\nPYEOF\n"
        with self.assertRaises(AssertionError) as caught:
            heredoc_body(mutated)
        self.assertIn("REFUSING TO PASS", str(caught.exception))

    def test_a_missing_heredoc_refuses(self):
        mutated = self.source.replace("<<'PYEOF'", "<<'OTHEREOF'")
        with self.assertRaises(AssertionError) as caught:
            heredoc_body(mutated)
        self.assertIn("REFUSING TO PASS", str(caught.exception))

    def test_an_unclosed_heredoc_refuses(self):
        lines = self.source.splitlines()
        for i, line in enumerate(lines):
            if line.strip() == DELIM and i > 0:
                lines[i] = "# " + line
                break
        with self.assertRaises(AssertionError) as caught:
            heredoc_body("\n".join(lines))
        self.assertIn("REFUSING TO PASS", str(caught.exception))


class PredicateTests(unittest.TestCase):
    """The helpers get their own controls, because a predicate that cannot fail
    turns every test above into an `ok` line."""

    def test_placeholder_count_ignores_escaped_percent(self):
        self.assertEqual(placeholder_count("100%% done: %s"), 1)
        self.assertEqual(placeholder_count("0x%06X +0x%04X  %-44s %s"), 4)
        self.assertEqual(placeholder_count("no placeholders here"), 0)

    def test_placeholder_count_reads_width_and_precision(self):
        self.assertEqual(placeholder_count("%-13s +0x%03X  %s%s"), 4)
        self.assertEqual(placeholder_count("%.2f"), 1)

    def test_unbound_names_finds_a_planted_typo(self):
        self.assertEqual(unbound_names("a = 1\nprint(a)\n"), [])
        self.assertEqual(unbound_names("a = 1\nprint(b)\n"), ["b"])

    def test_unbound_names_accepts_imports_and_builtins(self):
        self.assertEqual(unbound_names("import os\nprint(len(os.sep))\n"), [])

    def test_format_mismatches_is_quiet_on_correct_code(self):
        self.assertEqual(format_mismatches("print('%s %s' % (1, 2))\n"), [])
        self.assertEqual(
            len(format_mismatches("print('%s %s' % (1,))\n")), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
