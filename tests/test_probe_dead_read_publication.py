#!/usr/bin/env python3
"""
test_probe_dead_read_publication.py -- the dead-read path in
``phase0-kext/NV3050Probe.cpp`` must publish only what it actually read.

Why this exists
---------------
``inspectGspEngine()`` reads HWCFG2 first and returns early when the value
looks like a non-decoding read. Until 2026-08-17 that early return called
``publishGspState(hwcfg2, 0, 0, 0, 0, 0, 0, false)`` -- handing literal zeros
for six registers it had not read.

That is invisible in every existing gate and it is invisible on the operator's
screen, which is the whole problem. ``setProperty`` with a literal 0 produces an
IORegistry entry byte-identical to one that really did read zero. GSP_BCR_CTRL
equal to zero is a plausible measurement rather than an obviously-bogus
sentinel, so an operator in IORegistryExplorer could not tell a powered-down
engine from an engine reporting all clear -- two conclusions that send them to
opposite ends of PLAN.md. ``GSPImemBytes`` was worse than absent: it was
``nvFalconImemSizeBytes(0)``, a value DERIVED from a register nobody read,
published as a byte count.

No other gate can see this class. The register gate checks constants, the C++
suites cannot link the IOKit shell at all, and the clang front end is perfectly
happy with a fabricated argument -- ``0`` is a valid ``UInt32``. So the shape is
what gets pinned, the way ``tools/check-prepare-balance.py`` pins a shape no
gate could otherwise reach.

Every assertion here runs through one ``audit()`` function so the negative
controls exercise the identical predicates as the positive control. A gate
whose mutants are checked by different code than its real input is two gates,
and only one of them has been seen to fail.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CPP = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
HPP = os.path.join(ROOT, "phase0-kext", "NV3050Probe.hpp")

#
# The six registers the dead-read path never reads. HWCFG2 is deliberately NOT
# in this list -- it is the one value that path did measure, and publishing it
# is the point.
#
UNREAD_REGISTER_KEYS = (
    "GSP_HWCFG",
    "GSP_FALCON_CPUCTL",
    "GSP_RISCV_CPUCTL",
    "GSP_BCR_CTRL",
    "GSP_PRIV_ERR_STAT",
    "GSP_HUB_ERR_STAT",
)

#
# Values decoded from a register rather than read from one. Both are forbidden
# on the dead-read path for the same reason as the raw registers, and
# GSPRiscvCapable is the subtler of the two: it decodes hwcfg2, which the path
# DID read -- but read as a non-decoding value, so a bit-field pulled out of it
# is noise wearing the shape of a capability flag.
#
UNREAD_DERIVED_KEYS = (
    "GSPImemBytes",
    "GSPRiscvCapable",
)


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


def strip_comments(text):
    """Return ``text`` with C/C++ comments blanked and string literals intact.

    This module scans for CALLS, and the comment above
    ``publishGspUnreadable()`` quotes the exact defective call it replaced --
    ``publishGspState(hwcfg2, 0, 0, 0, 0, 0, 0, false)``. Scanning raw text made
    the fix's own explanation look like the bug: a false RED on a pristine tree,
    and the worst kind, because the only way to green it would have been to
    delete the sentence that records why the gate exists.

    Comments are replaced by spaces rather than removed so byte offsets survive
    and every ``find()`` still lines up with the original text. String literals
    are left alone because ``published_keys()`` reads the property names out of
    them.
    """
    out = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]

        if char in "\"'":
            quote = char
            out.append(char)
            index += 1
            while index < length:
                out.append(text[index])
                if text[index] == "\\" and index + 1 < length:
                    out.append(text[index + 1])
                    index += 2
                    continue
                if text[index] == quote:
                    index += 1
                    break
                index += 1
            continue

        if char == "/" and index + 1 < length and text[index + 1] == "/":
            while index < length and text[index] != "\n":
                out.append(" ")
                index += 1
            continue

        if char == "/" and index + 1 < length and text[index + 1] == "*":
            while index + 1 < length and not (text[index] == "*"
                                              and text[index + 1] == "/"):
                out.append("\n" if text[index] == "\n" else " ")
                index += 1
            out.append("  ")
            index += 2
            continue

        out.append(char)
        index += 1

    return "".join(out)


def _balanced(text, start, opener, closer):
    """Return text from ``start`` (which must be ``opener``) to its match."""
    if text[start] != opener:
        raise ValueError("start is not %r" % opener)
    depth = 0
    for index in range(start, len(text)):
        char = text[index]
        if char == opener:
            depth += 1
        elif char == closer:
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise ValueError("unbalanced %r from offset %d" % (opener, start))


def function_body(text, signature_fragment):
    """Body of the function whose signature contains ``signature_fragment``.

    Returns None rather than raising, because "the function is gone" is a
    finding this module has to report rather than an internal error -- a mutant
    that deletes a function must produce a named failure, not a traceback.
    """
    index = text.find(signature_fragment)
    if index < 0:
        return None
    brace = text.find("{", index)
    if brace < 0:
        return None
    try:
        return _balanced(text, brace, "{", "}")
    except ValueError:
        return None


def call_argument_lists(text, callee):
    """Every argument list passed to ``callee``, as raw strings."""
    out = []
    for match in re.finditer(r"\b%s\s*\(" % re.escape(callee), text):
        paren = text.index("(", match.start())
        try:
            out.append(_balanced(text, paren, "(", ")"))
        except ValueError:
            continue
    return out


def split_arguments(arglist):
    """Split a ``(a, b, c)`` string into its depth-zero arguments."""
    inner = arglist[1:-1]
    args = []
    depth = 0
    current = ""
    for char in inner:
        if char in "([":
            depth += 1
        elif char in ")]":
            depth -= 1
        if char == "," and depth == 0:
            args.append(current.strip())
            current = ""
        else:
            current += char
    if current.strip():
        args.append(current.strip())
    return args


def published_keys(body):
    """The literal keys a function body hands to setProperty."""
    return re.findall(r'setProperty\(\s*"([A-Za-z0-9_]+)"', body)


def audit(cpp, hpp):
    """Return a list of human-readable failures. Empty means the shape holds.

    Kept as one function on purpose: the real tree and every mutant below go
    through these exact predicates.
    """
    #
    # Code only. Prose about a defect is not the defect, and this file's own
    # explanatory comments quote both the old bad call and the key names it
    # forbids.
    #
    cpp = strip_comments(cpp)
    hpp = strip_comments(hpp)

    failures = []

    # ---------------------------------------------------------------- 1
    if "publishGspUnreadable" not in hpp:
        failures.append(
            "NV3050Probe.hpp does not declare publishGspUnreadable -- the "
            "dead-read path has no dedicated publisher, so it must be "
            "reusing publishGspState with fabricated arguments")

    inspect = function_body(cpp, "NV3050Probe::inspectGspEngine")
    if inspect is None:
        failures.append("inspectGspEngine() not found in NV3050Probe.cpp")
        return failures

    # ---------------------------------------------------------------- 2
    guard = inspect.find("nvLooksLikeDeadRead")
    if guard < 0:
        failures.append(
            "inspectGspEngine() no longer guards on nvLooksLikeDeadRead -- "
            "the dead-read path this module exists to police is gone")
        branch = ""
    else:
        brace = inspect.find("{", guard)
        try:
            branch = _balanced(inspect, brace, "{", "}") if brace >= 0 else ""
        except ValueError:
            branch = ""

    if branch:
        if "publishGspUnreadable" not in branch:
            failures.append(
                "the nvLooksLikeDeadRead branch does not call "
                "publishGspUnreadable")
        if "publishGspState" in branch:
            failures.append(
                "the nvLooksLikeDeadRead branch calls publishGspState, which "
                "takes seven register values it cannot have read")

    # ---------------------------------------------------------------- 3
    unreadable = function_body(cpp, "NV3050Probe::publishGspUnreadable")
    if unreadable is None:
        failures.append("publishGspUnreadable() not defined in NV3050Probe.cpp")
    else:
        keys = published_keys(unreadable)
        if "GSP_HWCFG2" not in keys:
            failures.append(
                "publishGspUnreadable does not publish GSP_HWCFG2, the one "
                "register that path actually read -- dropping it makes the "
                "dead read itself uninspectable")
        for key in UNREAD_REGISTER_KEYS:
            if key in keys:
                failures.append(
                    "publishGspUnreadable publishes %s, a register the "
                    "dead-read path never read" % key)
        for key in UNREAD_DERIVED_KEYS:
            if key in keys:
                failures.append(
                    "publishGspUnreadable publishes %s, which is DERIVED from "
                    "a register the dead-read path never read" % key)
        if "GSPEngineReadable" not in keys:
            failures.append(
                "publishGspUnreadable does not publish GSPEngineReadable")
        elif not re.search(r'setProperty\(\s*"GSPEngineReadable"\s*,\s*false',
                           unreadable):
            failures.append(
                "publishGspUnreadable publishes GSPEngineReadable but not as "
                "false")
        if "NVGspUnreadableReason" not in keys:
            failures.append(
                "publishGspUnreadable does not publish "
                "NVGspUnreadableReason -- an absent key is a question the "
                "operator has to ask, and this is the answer")

    # ---------------------------------------------------------------- 4
    state = function_body(cpp, "NV3050Probe::publishGspState")
    if state is None:
        failures.append("publishGspState() not defined in NV3050Probe.cpp")
    else:
        if not re.search(r'setProperty\(\s*"GSPEngineReadable"\s*,\s*true',
                         state):
            failures.append(
                "publishGspState does not publish GSPEngineReadable as true "
                "-- if the key appears only on the failure path, its absence "
                "cannot be told from a kext that predates it")
        for key in UNREAD_REGISTER_KEYS:
            if key not in published_keys(state):
                failures.append(
                    "publishGspState no longer publishes %s -- the measured "
                    "path is the one place these belong" % key)

    # ---------------------------------------------------------------- 5
    for arglist in call_argument_lists(cpp, "publishGspState"):
        args = split_arguments(arglist)
        if len(args) < 2:
            continue
        bare_zero = [i for i, arg in enumerate(args) if arg in ("0", "0u", "0U")]
        if bare_zero:
            failures.append(
                "a publishGspState call passes a bare literal zero at "
                "argument %s -- a register value nobody read is "
                "indistinguishable in the IORegistry from one that read zero"
                % ", ".join(str(i) for i in bare_zero))

    return failures


def mutate(text, old, new, occurrences=1):
    """Replace ``old`` with ``new``, asserting the anchor matched as expected.

    A mutation that matches nothing produces a green test that examined a
    pristine file. Counting first is what makes the negative controls real.
    """
    found = text.count(old)
    if found != occurrences:
        raise AssertionError(
            "mutation anchor matched %d times, expected %d:\n%r"
            % (found, occurrences, old))
    return text.replace(old, new)


class DeadReadPublicationTests(unittest.TestCase):
    """Positive control over the real tree."""

    def setUp(self):
        self.cpp = read_text(CPP)
        self.hpp = read_text(HPP)

    def test_sources_are_present_and_non_trivial(self):
        # A reader that silently returned "" would make every check below pass
        # by examining nothing.
        self.assertGreater(len(self.cpp), 10000, CPP)
        self.assertGreater(len(self.hpp), 1000, HPP)
        self.assertIn("NV3050Probe::inspectGspEngine", self.cpp)

    def test_live_tree_publishes_only_what_it_read(self):
        failures = audit(self.cpp, self.hpp)
        self.assertEqual(failures, [], "\n  " + "\n  ".join(failures))

    def test_dead_read_branch_calls_the_dedicated_publisher(self):
        inspect = function_body(self.cpp, "NV3050Probe::inspectGspEngine")
        self.assertIsNotNone(inspect)
        brace = inspect.find("{", inspect.find("nvLooksLikeDeadRead"))
        branch = _balanced(inspect, brace, "{", "}")
        self.assertIn("publishGspUnreadable", branch)
        self.assertNotIn("publishGspState", branch)

    def test_both_paths_publish_the_readability_flag(self):
        state = function_body(self.cpp, "NV3050Probe::publishGspState")
        unreadable = function_body(self.cpp, "NV3050Probe::publishGspUnreadable")
        self.assertIn("GSPEngineReadable", published_keys(state))
        self.assertIn("GSPEngineReadable", published_keys(unreadable))


class NegativeControlTests(unittest.TestCase):
    """Each mutant must be caught, and caught for the RIGHT reason.

    Asserting merely that ``audit()`` returned something would pass on a mutant
    that reddened for an unrelated cause, which is the failure mode a bare
    ``assertTrue(failures)`` cannot see.
    """

    def setUp(self):
        self.cpp = read_text(CPP)
        self.hpp = read_text(HPP)
        # Sanity: the unmutated pair is clean, so every failure below is the
        # mutation's doing.
        self.assertEqual(audit(self.cpp, self.hpp), [])

    def _reasons(self, cpp=None, hpp=None):
        return "\n".join(audit(cpp or self.cpp, hpp or self.hpp))

    def test_reverting_to_publishGspState_with_zeros_is_caught(self):
        # The exact code this session replaced.
        cpp = mutate(self.cpp,
                     "publishGspUnreadable(hwcfg2);",
                     "publishGspState(hwcfg2, 0, 0, 0, 0, 0, 0, false);")
        reasons = self._reasons(cpp=cpp)
        self.assertIn("branch calls publishGspState", reasons)
        self.assertIn("bare literal zero", reasons)

    def test_publishing_an_unread_register_is_caught(self):
        cpp = mutate(self.cpp,
                     'setProperty("GSPBootable", false);',
                     'setProperty("GSP_BCR_CTRL", 0, 32);\n'
                     '    setProperty("GSPBootable", false);')
        reasons = self._reasons(cpp=cpp)
        self.assertIn("publishes GSP_BCR_CTRL", reasons)
        self.assertIn("never read", reasons)

    def test_publishing_a_derived_value_is_caught(self):
        cpp = mutate(self.cpp,
                     'setProperty("GSPEngineReadable", false);',
                     'setProperty("GSPImemBytes", (UInt64)0, 32);\n'
                     '    setProperty("GSPEngineReadable", false);')
        reasons = self._reasons(cpp=cpp)
        self.assertIn("publishes GSPImemBytes", reasons)
        self.assertIn("DERIVED", reasons)

    def test_dropping_the_flag_from_the_measured_path_is_caught(self):
        cpp = mutate(self.cpp,
                     'setProperty("GSPEngineReadable", true);',
                     '/* removed */')
        reasons = self._reasons(cpp=cpp)
        self.assertIn("does not publish GSPEngineReadable as true", reasons)

    def test_flipping_the_flag_on_the_dead_read_path_is_caught(self):
        cpp = mutate(self.cpp,
                     'setProperty("GSPEngineReadable", false);',
                     'setProperty("GSPEngineReadable", true);')
        reasons = self._reasons(cpp=cpp)
        self.assertIn("not as false", reasons)

    def test_dropping_the_measured_register_is_caught(self):
        cpp = mutate(self.cpp,
                     'setProperty("GSP_HWCFG2", hwcfg2, 32);',
                     '/* removed */')
        reasons = self._reasons(cpp=cpp)
        self.assertIn("does not publish GSP_HWCFG2", reasons)

    def test_dropping_the_reason_string_is_caught(self):
        cpp = mutate(self.cpp, 'setProperty("NVGspUnreadableReason",',
                     'setProperty("NVGspUnreadableReasonRenamed",')
        reasons = self._reasons(cpp=cpp)
        self.assertIn("NVGspUnreadableReason", reasons)

    def test_removing_the_declaration_is_caught(self):
        hpp = mutate(self.hpp,
                     "void   publishGspUnreadable(UInt32 hwcfg2);",
                     "/* removed */")
        reasons = self._reasons(hpp=hpp)
        self.assertIn("does not declare publishGspUnreadable", reasons)

    def test_removing_the_guard_entirely_is_caught(self):
        cpp = mutate(self.cpp, "nvLooksLikeDeadRead(hwcfg2)",
                     "false /* was nvLooksLikeDeadRead */", occurrences=1)
        reasons = self._reasons(cpp=cpp)
        self.assertIn("no longer guards on nvLooksLikeDeadRead", reasons)


class HelperTests(unittest.TestCase):
    """The extractors themselves, because a parser that returns nothing makes
    every predicate above vacuous."""

    def test_function_body_is_balanced(self):
        body = function_body(read_text(CPP), "NV3050Probe::publishGspUnreadable")
        self.assertIsNotNone(body)
        self.assertTrue(body.startswith("{"))
        self.assertTrue(body.endswith("}"))
        self.assertEqual(body.count("{"), body.count("}"))

    def test_function_body_returns_none_for_a_missing_function(self):
        self.assertIsNone(function_body(read_text(CPP), "NV3050Probe::nope"))

    def test_split_arguments_respects_nesting(self):
        self.assertEqual(
            split_arguments("(a, (UInt64)f(b, c), 0)"),
            ["a", "(UInt64)f(b, c)", "0"])

    def test_published_keys_finds_the_measured_path_registers(self):
        state = function_body(read_text(CPP), "NV3050Probe::publishGspState")
        keys = published_keys(state)
        for key in UNREAD_REGISTER_KEYS:
            self.assertIn(key, keys)

    def test_mutate_refuses_an_anchor_that_matches_nothing(self):
        with self.assertRaises(AssertionError):
            mutate("abc", "zzz", "yyy")


if __name__ == "__main__":
    unittest.main(verbosity=2)
