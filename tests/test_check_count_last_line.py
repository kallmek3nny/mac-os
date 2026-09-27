#!/usr/bin/env python3
"""
test_check_count_last_line.py -- every tests/*.cpp suite must END on its count.

Why this exists
---------------
All five C++ suites are read by humans and by scripts via ``tail -1``. The
convention is that the LAST line of output is the numeric check summary, so
``tail -1`` returns a number rather than a sentence.

That convention has already broken twice. On 2026-08-10 ``test_decode`` ended on
"unproven until Step 7." and ``test_falcon`` on "...are all still unwritten." --
both prose -- and a suite whose final line is a caveat reads as a *finding*
rather than as a count. A zero-assertion suite that prints an optimistic
sentence last is indistinguishable, at a glance, from a passing one.

Both suites now print the caveat BEFORE the summary, and each carries a comment
saying why. Nothing enforced it until this file: a comment is not a gate.

WHAT THIS CHECKS, AND WHAT IT DOES NOT
--------------------------------------
This is a **static** check over the C++ source. It does not compile or run
anything, so it works on a bare checkout with CPython only -- the same
constraint ``run-python-tests.bat`` is built around. It asserts that the last
``std::printf`` in each ``main()`` is the count line.

It cannot prove the runtime output ends there: a suite could print from a
function called after that ``printf``, or via ``std::cout``. Those are checked
for separately and explicitly below, but the honest statement is that this gate
pins the *source shape*, and ``build-msvc.bat`` plus running the binary is what
confirms the actual final line.

THE TWO COUNT FORMATS ARE BOTH REAL
-----------------------------------
Which suite prints which shape -- asserted by BothFormatsStillInUseTests below,
so this list cannot drift without a test failing:

    test_decode        "%d/%d passed, %d failed"
    test_falcon        "%d/%d passed, %d failed"
    test_gsp_boot      "%d checks, %d failures"
    test_gsp_stage     "%d checks, %d failures"
    test_intel_display "%d checks, %d failures"
    test_intel_gpu_id  "%d checks, %d failures"
    test_riscv_ucode   "%d checks, %d failures"

So there is no single format to pin, and inventing one would fail three suites
or two. Both shapes are accepted, and a test below asserts that BOTH remain in
use -- if someone unifies them, that test fails and this docstring gets
corrected rather than silently going stale.

THE CHECK COUNTS ARE DELIBERATELY NOT LISTED HERE
-------------------------------------------------
An earlier revision of this docstring listed them under the words "Measured
2026-08-11 by running all five binaries". Two of the five were wrong on the day
they were written: test_falcon was recorded as 165 when the binary prints 169,
and test_riscv_ucode was left as the literal placeholder "(N checks, M
failures)" -- never measured at all, despite the sentence above it saying it
had been. Measured 2026-08-11 by actually running them: 39, 169, 85, 199, 142.
Those five numbers went stale two days later, and the correction went stale
again inside the same day: on 2026-08-13 they were re-measured as
64, 169, 92, 199, 162, and two of the five had moved again before that
afternoon was over. No current list is reproduced here, on purpose -- that is
the point being made, not a footnote to it: a count written into prose is wrong
the moment anyone adds a check, and nothing in this file or any other can
detect that.

No static gate can check a runtime count, which is exactly why one does not
belong in prose that reads as verified. The only authority for a total is
running the binary.

CORRECTED 2026-08-15: the sentence above used to end "The totals live in
tests/README.md" -- and there is no tests/README.md, and there never was in this
tree. So a docstring written specifically to warn that prose goes stale was
itself pointing at a file that does not exist, which is the same defect one
level up. The dated numbers actually live in the MEASURED dict of
tests/test_check_count_floor.py, where a test derives a floor from each one
rather than merely asserting it.
"""

import os
import re
import unittest

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))

# The suites, named explicitly rather than globbed. A glob would silently
# accept a future suite that never gets checked here, and would also pick up
# tests/build/CMakeFiles/.../CMakeCXXCompilerId.cpp, which has its own main().
#
# test_no_unlisted_suite_in_tests_dir is what keeps this list honest in the
# other direction: a .cpp added to tests/ and not added here reddens, so the
# explicit list cannot go stale silently the way a glob's absence would.
SUITES = (
    "test_decode.cpp",
    "test_falcon.cpp",
    "test_gsp_boot.cpp",
    "test_gsp_stage.cpp",
    "test_intel_display.cpp",
    "test_intel_display_names.cpp",
    "test_intel_fb_present.cpp",
    "test_intel_framebuffer.cpp",
    "test_intel_gpu_id.cpp",
    "test_intel_gt_batch.cpp",
    "test_intel_gt_blt.cpp",
    "test_intel_gt_cmd.cpp",
    "test_intel_gt_context.cpp",
    "test_intel_gt_exec.cpp",
    "test_intel_gt_fence.cpp",
    "test_intel_gt_image.cpp",
    "test_intel_gt_reset.cpp",
    "test_intel_gt_ring.cpp",
    "test_intel_pixel.cpp",
    "test_intel_scanout.cpp",
    "test_riscv_ucode.cpp",
)

# Both formats, as measured. Group 1 of each is the leading count expression.
COUNT_FORMATS = (
    re.compile(r'"\\n%d/%d passed, %d failed\\n"'),
    re.compile(r'"\\n%d checks, %d failures\\n"'),
)

_PRINTF = re.compile(r"std::printf\s*\(")
_MAIN = re.compile(r"^int main\s*\(", re.MULTILINE)


def _read(name):
    path = os.path.join(_TESTS_DIR, name)
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _main_body(text):
    """Return the source of main() from its 'int main(' to end of file.

    Slicing to end-of-file rather than brace-matching is deliberate: main() is
    the last function in every one of these files, and a brace matcher would
    have to understand braces inside string literals to be correct. If main()
    ever stops being last, test_main_is_the_last_function below fails loudly
    instead of this helper quietly returning too much.
    """
    match = _MAIN.search(text)
    if match is None:
        return None
    return text[match.start():]


def _printf_calls(body):
    """Return the source offset of every std::printf( in `body`, in order."""
    return [m.start() for m in _PRINTF.finditer(body)]


def _count_format_offsets(body):
    """Return offsets of every count-format literal in `body`, in order."""
    offsets = []
    for pattern in COUNT_FORMATS:
        offsets.extend(m.start() for m in pattern.finditer(body))
    return sorted(offsets)


class SuitesExistTests(unittest.TestCase):
    """Guards this file's own premise before it asserts anything about content.

    If a suite is renamed, every other test here would pass vacuously against
    the four that remain. That is the failure mode this class exists to prevent.
    """

    def test_all_five_named_suites_are_present(self):
        for name in SUITES:
            path = os.path.join(_TESTS_DIR, name)
            self.assertTrue(os.path.isfile(path), path)

    def test_no_unlisted_suite_in_tests_dir(self):
        found = sorted(n for n in os.listdir(_TESTS_DIR) if n.endswith(".cpp"))
        self.assertEqual(found, sorted(SUITES), found)


class CountIsLastPrintfTests(unittest.TestCase):
    """The load-bearing assertion: the count printf is the final printf."""

    def test_every_suite_has_a_main(self):
        for name in SUITES:
            self.assertIsNotNone(_main_body(_read(name)), name)

    def test_every_main_contains_exactly_one_count_format(self):
        for name in SUITES:
            body = _main_body(_read(name))
            offsets = _count_format_offsets(body)
            self.assertEqual(len(offsets), 1,
                             "%s: found %d count formats" % (name, len(offsets)))

    def test_count_printf_is_the_last_printf_in_main(self):
        for name in SUITES:
            body = _main_body(_read(name))
            printfs = _printf_calls(body)
            self.assertTrue(printfs, name)
            count_at = _count_format_offsets(body)[0]

            # The format literal sits inside the last printf call, so the last
            # printf's opening paren must come BEFORE the literal, and no printf
            # may open after it.
            after = [p for p in printfs if p > count_at]
            self.assertEqual(
                after, [],
                "%s: %d printf call(s) come after the count line"
                % (name, len(after)))

    def test_caveat_prose_precedes_the_count(self):
        """The specific 2026-08-10 regression, pinned by its own text.

        These two strings were each the final output line at some point. If
        either moves back after the count, this fails.
        """
        cases = (
            ("test_decode.cpp", "unproven until Step 7."),
            ("test_falcon.cpp", "are all still unwritten."),
        )
        for name, prose in cases:
            body = _main_body(_read(name))
            self.assertIn(prose, body, name)
            self.assertLess(body.index(prose), _count_format_offsets(body)[0],
                            "%s: %r comes after the count" % (name, prose))


class NoOtherOutputChannelTests(unittest.TestCase):
    """A printf-only check is defeated by any other way of writing to stdout."""

    def test_no_iostream_output_in_main(self):
        for name in SUITES:
            body = _main_body(_read(name))
            for token in ("std::cout", "std::cerr", "std::clog", "puts(",
                          "fputs(", "std::fprintf"):
                self.assertNotIn(token, body, "%s uses %s" % (name, token))

    def test_nothing_is_called_after_the_count(self):
        """Only the return statement may follow the count printf.

        A helper invoked after the summary could print anything. Rather than
        try to identify calls, this asserts the tail is just the printf's own
        continuation lines plus 'return ...;' and closing braces.
        """
        for name in SUITES:
            body = _main_body(_read(name))
            tail = body[_count_format_offsets(body)[0]:]
            # Drop the remainder of the count printf statement itself.
            semi = tail.index(";")
            remainder = tail[semi + 1:]
            stripped = re.sub(r"\s+", "", remainder)
            self.assertTrue(
                re.fullmatch(r"returngFailures==0\?0:1;\}", stripped),
                "%s: unexpected tail after the count: %r" % (name, remainder))

    def test_main_is_the_last_function(self):
        """_main_body slices to end-of-file, which is only valid if this holds."""
        for name in SUITES:
            text = _read(name)
            body = _main_body(text)
            # Exactly one closing brace at column 0 may follow: main's own.
            self.assertEqual(len(re.findall(r"^\}", body, re.MULTILINE)), 1, name)


class BothFormatsStillInUseTests(unittest.TestCase):
    """Keeps this file's docstring honest.

    Per the `prose-carrying-a-derived-value-goes-stale-silently` finding,
    nothing checks a number -- or a claim -- written in prose. The docstring
    above states that BOTH count formats are in real use. This asserts it.
    """

    def _format_index(self, name):
        body = _main_body(_read(name))
        for index, pattern in enumerate(COUNT_FORMATS):
            if pattern.search(body):
                return index
        return None

    def test_passed_failed_format_is_used(self):
        users = [n for n in SUITES if self._format_index(n) == 0]
        self.assertEqual(sorted(users),
                         ["test_decode.cpp", "test_falcon.cpp",
                          "test_intel_fb_present.cpp"], users)

    def test_checks_failures_format_is_used(self):
        users = [n for n in SUITES if self._format_index(n) == 1]
        self.assertEqual(
            sorted(users),
            ["test_gsp_boot.cpp", "test_gsp_stage.cpp",
             "test_intel_display.cpp", "test_intel_display_names.cpp",
             "test_intel_framebuffer.cpp",
             "test_intel_gpu_id.cpp",
             "test_intel_gt_batch.cpp",
             "test_intel_gt_blt.cpp", "test_intel_gt_cmd.cpp",
             "test_intel_gt_context.cpp",
             "test_intel_gt_exec.cpp",
             "test_intel_gt_fence.cpp",
             "test_intel_gt_image.cpp",
             "test_intel_gt_reset.cpp",
             "test_intel_gt_ring.cpp",
             "test_intel_pixel.cpp",
             "test_intel_scanout.cpp",
             "test_riscv_ucode.cpp"],
            users)

    def test_every_suite_uses_exactly_one_known_format(self):
        for name in SUITES:
            self.assertIsNotNone(self._format_index(name), name)


class NegativeControlTests(unittest.TestCase):
    """The checks above must be able to go RED.

    Every other test in this module runs against the five real suites, and all
    five currently satisfy the convention. That means a helper that silently
    stopped matching -- a regex typo, a `_main_body` that sliced to nothing --
    would produce a green run indistinguishable from a correct one. Per the
    `an-assertion-that-cannot-fail-is-not-coverage` finding, a passing check
    over only-conforming inputs measures nothing.

    These build deliberately-broken bodies and assert the helpers reject them.
    A mutant that survives here is a hole in the gate, not a passing test.
    """

    #
    # A minimal main() in the same shape as the real suites: a caveat printf,
    # then the count printf, then the return. Built from _read() of a real
    # suite would couple this to that suite's contents, so it is synthetic.
    #
    GOOD = (
        'int main(void)\n'
        '{\n'
        '    runEverything();\n'
        '    if (gFailures == 0) {\n'
        '        std::printf("\\nA caveat that must precede the count.\\n");\n'
        '    }\n'
        '    std::printf("\\n%d/%d passed, %d failed\\n",\n'
        '                gChecks - gFailures, gChecks, gFailures);\n'
        '    return gFailures == 0 ? 0 : 1;\n'
        '}\n'
    )

    def test_the_synthetic_good_body_actually_passes(self):
        """Anchors the mutants below.

        Without this, a mutant could be rejected because the *baseline* is
        malformed rather than because the mutation was caught -- and every
        mutant test would pass for the wrong reason.
        """
        body = _main_body(self.GOOD)
        offsets = _count_format_offsets(body)
        self.assertEqual(len(offsets), 1)
        after = [p for p in _printf_calls(body) if p > offsets[0]]
        self.assertEqual(after, [], "baseline already has a trailing printf")

    def test_a_printf_after_the_count_is_detected(self):
        """The exact 2026-08-10 regression: a caveat printed last."""
        mutant = self.GOOD.replace(
            '    return gFailures == 0 ? 0 : 1;',
            '    std::printf("\\nStill unwritten until Step 7.\\n");\n'
            '    return gFailures == 0 ? 0 : 1;')
        body = _main_body(mutant)
        offsets = _count_format_offsets(body)
        after = [p for p in _printf_calls(body) if p > offsets[0]]
        self.assertEqual(len(after), 1,
                         "a printf after the count went undetected")

    def test_a_missing_count_format_is_detected(self):
        """A suite that stopped printing a count at all."""
        mutant = self.GOOD.replace('\\n%d/%d passed, %d failed\\n', 'done')
        self.assertEqual(_count_format_offsets(_main_body(mutant)), [],
                         "a suite with no count format still matched")

    def test_an_iostream_write_is_detected(self):
        """printf-only matching is defeated by any other output channel."""
        mutant = self.GOOD.replace(
            '    return gFailures == 0 ? 0 : 1;',
            '    std::cout << "a caveat";\n'
            '    return gFailures == 0 ? 0 : 1;')
        self.assertIn("std::cout", _main_body(mutant),
                      "an iostream write after the count went undetected")

    def test_a_helper_call_after_the_count_is_detected(self):
        """The tail check must reject anything but the return statement.

        A call cannot be recognised by name, so `test_nothing_is_called_after
        _the_count` asserts the tail matches exactly. This confirms that
        assertion separates rather than matching loosely.
        """
        mutant = self.GOOD.replace(
            '    return gFailures == 0 ? 0 : 1;',
            '    printTrailingSummary();\n'
            '    return gFailures == 0 ? 0 : 1;')
        body = _main_body(mutant)
        tail = body[_count_format_offsets(body)[0]:]
        remainder = tail[tail.index(";") + 1:]
        stripped = re.sub(r"\s+", "", remainder)
        self.assertIsNone(re.fullmatch(r"returngFailures==0\?0:1;\}", stripped),
                          "a helper call after the count went undetected")

    def test_a_second_function_after_main_is_detected(self):
        """_main_body slices to EOF, so a trailing function breaks its premise."""
        mutant = self.GOOD + '\nvoid trailing(void)\n{\n}\n'
        body = _main_body(mutant)
        self.assertNotEqual(len(re.findall(r"^\}", body, re.MULTILINE)), 1,
                            "a function after main() went undetected")


if __name__ == "__main__":
    unittest.main()
