#!/usr/bin/env python3
"""
test_no_duplicate_definitions.py -- a name bound twice in one scope is a silent
overwrite, and the survivor still reports `ok`.

Why this exists
---------------
Python does not object to a name being defined twice in the same scope. The
second definition replaces the first, and the first is simply gone. In a
`unittest` module that produces a specific, quiet failure:

  * Two `class Fixtures(...)` in one file -- only the second is bound, so every
    test method on the first one never runs. The suite does not error. It runs
    a smaller set of tests and prints `OK`.
  * Two `def test_something(self)` in one class -- only the last is bound. The
    earlier assertion is deleted from the run, and the name still appears in
    the verbose listing exactly once, `ok`.

Neither case produces a red anywhere. The only visible symptom is the test
count, and a count that goes DOWN while everything passes is the hardest kind
of regression to notice -- it looks like the suite got faster. This repo has
already been burned by the mirror-image of that: an assertion that could not
fail read as coverage (`tests/test_falcon.cpp`'s adjacent-bit checks), and a
negative control that missed its target line before it ever tested it. A test
that does not exist is the same defect with fewer moving parts.

The dict-literal case is the same mistake in a different shape
--------------------------------------------------------------
A repeated key in a dict literal also keeps the last value and drops the
earlier one, with no warning. That matters here because several gates are
driven by literal tables keyed by tuples -- `tests/test_documented_paths.py`
keys its expected-absent table by `(document, token)` pairs. A pair written
twice would silently discard the first expectation, and the gate would then go
red naming a token its own table appears to cover. Reading the table would not
show the problem; the two entries can be dozens of lines apart.

What this test does NOT claim
-----------------------------
It scans the direct children of each module body and each class body only.
Definitions guarded by `if`/`else` or `try`/`except ImportError` are NOT
examined, because redefining a name across those branches is the normal way to
write a fallback -- exactly one branch runs. That is a deliberate hole, not an
oversight: closing it would require deciding which branches are exclusive,
which is not decidable from the syntax tree.

`@property`/`@x.setter` pairs, `@typing.overload` stacks, and
`@f.register` dispatch are exempt for the same reason -- repeating the name is
the API, not a mistake.

Passing means no repo-owned Python module shadows one of its own definitions.
It says nothing about whether the surviving definitions are correct.

The scan was clean when this was written (2026-08-13), so this lands as a
regression guard rather than a bug fix. That is the point: the failure mode is
invisible at review time, so the check has to be mechanical and it has to live
in the gate that runs on a bare checkout (`tests\\run-python-tests.bat`, which
needs nothing but CPython).

Run:
    python tests/test_no_duplicate_definitions.py
    tests\\run-python-tests.bat        # picked up automatically by discovery
"""

import ast
import os
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     os.pardir))

# `reference/` is vendored NVIDIA source and is not ours to edit -- a finding
# there would be unactionable noise. The rest are generated or transient trees.
SKIP_DIRS = {".git", ".claude-crew", "reference", "build", "__pycache__",
             "shots", "node_modules", "payload-stage", ".venv", "venv"}

DEFINITION_NODES = (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)

# Decorators under which repeating a name is the intended API rather than a
# mistake. `<name>.setter`/`.getter`/`.deleter` and `<name>.register` are
# matched dynamically against the definition's own name, below.
OVERLOAD_DECORATORS = {"overload", "typing.overload"}


def find_python_files():
    """Every repo-owned .py, as (relative_path, absolute_path) pairs."""
    found = []
    for dirpath, dirnames, filenames in os.walk(REPO):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name.lower().endswith(".py"):
                absolute = os.path.join(dirpath, name)
                found.append((os.path.relpath(absolute, REPO).replace(os.sep, "/"),
                              absolute))
    return sorted(found)


def _decorator_strings(node):
    """Dotted names of a definition's decorators, e.g. ['x.setter'].

    Decorators that are not plain names or attribute chains (a call on a
    subscript, say) are skipped rather than guessed at.
    """
    out = []
    for decorator in getattr(node, "decorator_list", []):
        target = decorator.func if isinstance(decorator, ast.Call) else decorator
        parts = []
        while isinstance(target, ast.Attribute):
            parts.append(target.attr)
            target = target.value
        if isinstance(target, ast.Name):
            parts.append(target.id)
            out.append(".".join(reversed(parts)))
    return out


def _is_exempt(node):
    """True when repeating this definition's name is deliberate API."""
    decorators = _decorator_strings(node)
    if any(d in OVERLOAD_DECORATORS for d in decorators):
        return True
    suffixes = (".setter", ".getter", ".deleter", ".register")
    return any(d == node.name + suffix
               for d in decorators for suffix in suffixes)


def _scan_body(body, scope, findings):
    seen = {}
    for node in body:
        if isinstance(node, ast.ClassDef):
            _scan_body(node.body, "%s.%s" % (scope, node.name), findings)
        if not isinstance(node, DEFINITION_NODES):
            continue
        if _is_exempt(node):
            continue
        seen.setdefault(node.name, []).append(node.lineno)
    for name, lines in sorted(seen.items()):
        if len(lines) > 1:
            findings.append((scope, name, lines))


def duplicate_definitions(source):
    """[(scope, name, [linenos])] for names bound twice in the same scope.

    `scope` is "module" or a dotted class path such as "module.Fixtures".
    """
    findings = []
    _scan_body(ast.parse(source).body, "module", findings)
    return findings


def duplicate_dict_keys(source):
    """[(repr_of_key, [linenos])] for literal dict keys written twice.

    Keys are compared by VALUE, the way Python compares them -- so `1` and
    `True` collide here exactly as they would at runtime. Computed keys (a
    name, an f-string, a call) are skipped: their value is not knowable from
    the syntax tree, and guessing would produce false reds.
    """
    findings = []
    for node in ast.walk(ast.parse(source)):
        if not isinstance(node, ast.Dict):
            continue
        seen = {}
        for key in node.keys:
            if key is None:          # `{**other}` carries a None key slot
                continue
            try:
                value = ast.literal_eval(key)
                hash(value)
            except (ValueError, SyntaxError, TypeError, MemoryError,
                    RecursionError):
                continue
            seen.setdefault(value, []).append(getattr(key, "lineno",
                                                      node.lineno))
        for value, lines in seen.items():
            if len(lines) > 1:
                findings.append((repr(value), sorted(lines)))
    return findings


class NoModuleShadowsItsOwnDefinitions(unittest.TestCase):

    def test_the_walk_finds_this_module_and_a_tool(self):
        # A walk that silently finds nothing makes every assertion below
        # vacuously true. Asserting the SHAPE of the result -- that the file
        # doing the asserting is in it, and that the scan reaches outside
        # tests/ -- rather than a count, which would just be a magnitude that
        # goes stale on the next file added.
        discovered = [relative for relative, _absolute in find_python_files()]
        self.assertIn("tests/test_no_duplicate_definitions.py", discovered,
                      "the walk did not find the file it is defined in -- "
                      "REPO or SKIP_DIRS is wrong and this suite is checking "
                      "nothing")
        self.assertTrue([r for r in discovered if r.startswith("tools/")],
                        "the walk found no tools/ module, so the scan is "
                        "confined to tests/ and half the repo is unchecked")

    def test_no_module_binds_a_name_twice_in_one_scope(self):
        offenders = []
        for relative, absolute in find_python_files():
            with open(absolute, "rb") as fh:
                source = fh.read()
            try:
                findings = duplicate_definitions(source)
            except SyntaxError as exc:
                offenders.append("%s: will not parse (%s)" % (relative, exc))
                continue
            for scope, name, lines in findings:
                offenders.append("%s: %r defined %d times in %s at lines %s"
                                 % (relative, name, len(lines), scope,
                                    ", ".join(str(n) for n in lines)))
        self.assertEqual(
            [], offenders,
            "name(s) bound twice in one scope:\n  %s\n"
            "Only the LAST definition survives; the earlier one is discarded "
            "at import. If these are test methods or a TestCase subclass, "
            "those tests are not running and the suite is still green -- the "
            "only symptom is a test count that quietly went down. Rename one, "
            "or delete the dead one deliberately."
            % "\n  ".join(offenders))

    def test_no_dict_literal_repeats_a_key(self):
        offenders = []
        for relative, absolute in find_python_files():
            with open(absolute, "rb") as fh:
                source = fh.read()
            try:
                findings = duplicate_dict_keys(source)
            except SyntaxError:
                continue      # already reported by the test above
            for key, lines in findings:
                offenders.append("%s: key %s repeated at lines %s"
                                 % (relative, key,
                                    ", ".join(str(n) for n in lines)))
        self.assertEqual(
            [], offenders,
            "dict literal(s) with a repeated key:\n  %s\n"
            "The last value wins and the earlier entry is dropped silently. "
            "Where a literal table drives a gate -- an expected-absent list, a "
            "register map -- that dropped entry becomes a failure blamed on "
            "the thing the table looks like it already covers."
            % "\n  ".join(offenders))


class DetectorItself(unittest.TestCase):
    """The measurement must be right, or the two tests above are decoration.

    Every case here is driven from a source string rather than a file: the
    repo scan is clean by construction today, so without these the detector
    could return `[]` unconditionally and nothing would notice.
    """

    def test_two_methods_with_the_same_name_are_reported(self):
        findings = duplicate_definitions(
            "class T:\n"
            "    def test_a(self):\n"
            "        pass\n"
            "    def test_b(self):\n"
            "        pass\n"
            "    def test_a(self):\n"
            "        pass\n")
        self.assertEqual([("module.T", "test_a", [2, 6])], findings)

    def test_two_module_level_classes_with_the_same_name_are_reported(self):
        findings = duplicate_definitions(
            "class Fixtures:\n"
            "    pass\n"
            "class Other:\n"
            "    pass\n"
            "class Fixtures:\n"
            "    pass\n")
        self.assertEqual([("module", "Fixtures", [1, 5])], findings)

    def test_a_nested_class_is_scanned_and_named(self):
        findings = duplicate_definitions(
            "class Outer:\n"
            "    class Inner:\n"
            "        def go(self):\n"
            "            pass\n"
            "        def go(self):\n"
            "            pass\n")
        self.assertEqual([("module.Outer.Inner", "go", [3, 5])], findings)

    def test_the_same_name_in_two_different_classes_is_fine(self):
        # The overwrite is per-scope. Two TestCases can and should both define
        # `setUp`; flagging that would make the gate unusable.
        self.assertEqual([], duplicate_definitions(
            "class A:\n"
            "    def setUp(self):\n"
            "        pass\n"
            "class B:\n"
            "    def setUp(self):\n"
            "        pass\n"))

    def test_a_property_setter_is_not_a_duplicate(self):
        self.assertEqual([], duplicate_definitions(
            "class T:\n"
            "    @property\n"
            "    def size(self):\n"
            "        return 1\n"
            "    @size.setter\n"
            "    def size(self, v):\n"
            "        pass\n"))

    def test_a_setter_named_for_a_DIFFERENT_property_is_still_a_duplicate(self):
        # The exemption matches `<own name>.setter`, not any `.setter` at all.
        # A copy-paste that leaves the decorator pointing at the neighbouring
        # property is precisely the mistake worth catching, and a blanket
        # "ends with .setter" rule would wave it through.
        findings = duplicate_definitions(
            "class T:\n"
            "    @property\n"
            "    def size(self):\n"
            "        return 1\n"
            "    @other.setter\n"
            "    def size(self, v):\n"
            "        pass\n")
        self.assertEqual([("module.T", "size", [3, 6])], findings)

    def test_typing_overload_is_not_a_duplicate(self):
        self.assertEqual([], duplicate_definitions(
            "import typing\n"
            "@typing.overload\n"
            "def f(x: int):\n"
            "    ...\n"
            "@typing.overload\n"
            "def f(x: str):\n"
            "    ...\n"
            "def f(x):\n"
            "    return x\n"))

    def test_a_conditional_fallback_definition_is_not_reported(self):
        # Documented hole: exactly one branch binds the name at runtime, so
        # this is correct code and must not be flagged.
        self.assertEqual([], duplicate_definitions(
            "try:\n"
            "    from fast import loads\n"
            "except ImportError:\n"
            "    def loads(s):\n"
            "        return s\n"))

    def test_distinct_names_produce_nothing(self):
        self.assertEqual([], duplicate_definitions(
            "def a():\n"
            "    pass\n"
            "def b():\n"
            "    pass\n"))

    def test_a_repeated_dict_key_is_reported(self):
        self.assertEqual([("'x'", [1, 3])], duplicate_dict_keys(
            "D = {'x': 1,\n"
            "     'y': 2,\n"
            "     'x': 3}\n"))

    def test_a_repeated_tuple_key_is_reported(self):
        # The shape the doc-path tables actually use: (document, token).
        findings = duplicate_dict_keys(
            "D = {('a.md', 'p.hpp'): 1,\n"
            "     ('b.md', 'p.hpp'): 2,\n"
            "     ('a.md', 'p.hpp'): 3}\n")
        self.assertEqual([("('a.md', 'p.hpp')", [1, 3])], findings)

    def test_distinct_tuple_keys_produce_nothing(self):
        self.assertEqual([], duplicate_dict_keys(
            "D = {('a.md', 'p.hpp'): 1,\n"
            "     ('a.md', 'q.hpp'): 2}\n"))

    def test_a_computed_key_is_ignored_rather_than_guessed_at(self):
        # Two calls could return the same value or different ones; the syntax
        # tree cannot say. Skipping is the only answer that never false-reds.
        self.assertEqual([], duplicate_dict_keys(
            "D = {name(): 1,\n"
            "     name(): 2}\n"))

    def test_a_dict_unpacking_slot_does_not_crash_the_scan(self):
        self.assertEqual([], duplicate_dict_keys("D = {**other, 'x': 1}\n"))

    def test_a_nested_dict_is_scanned_too(self):
        self.assertEqual([("'k'", [1, 1])], duplicate_dict_keys(
            "D = {'outer': {'k': 1, 'k': 2}}\n"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
