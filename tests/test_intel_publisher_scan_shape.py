"""Pin the SHAPE of publishPlane()'s scanout consumption.

Why this module exists
----------------------

`igScanDescribe()` in `phase0-kext/IntelScanout.hpp` had 61 passing checks over
it in `tests/test_intel_scanout.cpp` and occurred NOWHERE outside its own
header -- `intel-handoff26.md` SS1's class, a heavily tested pure function that
passes every suite it has while contributing nothing to the driver.  A suite
proves a function is CORRECT; it says nothing about whether it is CONSULTED.

The 2026-08-19 fix gave it a caller in `IntelDisplayProbe::publishPlane()`.
And then: delete that call and its three `setProperty()` lines, and all four
gates stay green.  The register gate checks constants, the C++ suites cannot
compile a translation unit that reaches IOService, and clang has nothing to say
about a well-formed function that simply stopped calling something.  That is
the hole this tree files after every publisher fix, and this module is it.

What is pinned (small version -- more controls land next)
---------------------------------------------------------

1. `publishPlane()` calls `igScanDescribe(` at all, in CODE and not in prose.
2. All three scan keys are published: `1-Scan`, `1-ScanUsable`, `1-ScanExtent`.
3. `1-ScanExtent` is GUARDED by `igScanUsable(` -- an extent computed over a
   refused surf word is arithmetic over something nobody measured, and
   `setProperty()` with a fabricated number is byte-identical to one with a
   real one.
4. `1-Scan` and `1-ScanUsable` are NOT so guarded -- they are published on BOTH
   paths, which is what makes the extent's ABSENCE an explained absence rather
   than a question (`an-absent-key-is-a-question-not-an-answer`).

Two traps this module is written around
---------------------------------------

COMMENT STRIPPING IS MANDATORY, not stylistic.  The fix's own comment block
names `igScanDescribe`, `igScanUsable` and `setProperty()` in prose -- so a
gate reading raw source would find every identifier it requires on a publisher
whose calls had all been deleted.  `handoff18.md` SS1d in its own file.

THE KEY LITERALS ARE MATCHED WITH THEIR QUOTES.  `1-Scan` is a PREFIX of
`1-ScanUsable` and of `1-ScanExtent`, so a bare substring test for the first
is satisfied by either of the others and would stay green on a publisher that
lost it (`a-substring-test-passes-a-suffixed-rename`).  Every token below is
spelled with both quote characters, which makes the three disjoint.
"""

import os
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SUBJECT = os.path.join(ROOT, "phase0-kext", "IntelDisplayProbe.cpp")

METHOD = "void IntelDisplayProbe::publishPlane(UInt32 pipe)"

DESCRIBE_CALL = "igScanDescribe("
USABLE_CALL = "igScanUsable("
PUBLISH_CALL = "setProperty("

# Spelled with both quote characters on purpose -- see the module docstring.
KEY_VERDICT = '"1-Scan"'
KEY_USABLE = '"1-ScanUsable"'
KEY_EXTENT = '"1-ScanExtent"'


def strip_comments(src):
    """Blank out // and /* */ comments, preserving length and line structure.

    Length is preserved so every offset computed against the stripped text is
    still a valid offset into the original -- line numbers in failure messages
    therefore name real lines in the real file.
    """
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""

        if c == "/" and nxt == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue

        if c == "/" and nxt == "*":
            out.append("  ")
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue

        out.append(c)
        i += 1

    return "".join(out)


def method_body(stripped, signature):
    """Return (start, end) of the brace-balanced body following `signature`.

    Returns None when the signature is absent -- the caller REFUSES in that
    case rather than reporting success, because a gate that finds nothing and
    says nothing is indistinguishable from a green one.
    """
    at = stripped.find(signature)
    if at < 0:
        return None

    open_at = stripped.find("{", at)
    if open_at < 0:
        return None

    depth = 0
    i = open_at
    while i < len(stripped):
        if stripped[i] == "{":
            depth += 1
        elif stripped[i] == "}":
            depth -= 1
            if depth == 0:
                return (open_at, i + 1)
        i += 1
    return None


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def guard_span(body, at):
    """The text from the previous statement boundary up to `at`.

    Bareness is decided by walking BACK to the previous `;`, `{` or `}` rather
    than by a regex over the preceding line -- `handoff25.md` SS1c is what a
    negated character class costs here: a class excluding only the terminator
    runs straight past the `if`'s closing paren and lands on the next
    statement's semicolon.
    """
    start = 0
    for i in range(at - 1, -1, -1):
        if body[i] in ";{}":
            start = i + 1
            break
    return body[start:at]


def code_occurrences(src, token):
    """Offsets of `token` in CODE only, valid as offsets into `src` itself.

    strip_comments() preserves LENGTH, so an index found in the stripped text
    is the same index in the original.  That is what lets a mutation target the
    real call rather than the comment block three lines above it that names the
    same identifier.

    This helper exists because the first draft of this module's own negative
    control reddened NOTHING: `src.replace("igScanDescribe(", ..., 1)` hit the
    fix's own comment, the stripper then blanked the mutation, and the live call
    survived untouched -- so a gate that works reported a control that does not.
    That is `a-mutation-anchor-can-match-more-than-it-means` in its quiet
    direction: over-matching inside a comment does not raise, it silently
    mutates nothing and prints a plausible green.
    """
    stripped = strip_comments(src)
    out = []
    at = stripped.find(token)
    while at >= 0:
        out.append(at)
        at = stripped.find(token, at + 1)
    return out


def replace_code_occurrence(src, token, replacement, which=0):
    """Splice `replacement` over the `which`-th CODE occurrence of `token`.

    Returns `src` unchanged when there is no such occurrence, which every
    caller below asserts against -- a mutation that matched nothing must fail
    loudly rather than be mistaken for a subject that tolerates it.
    """
    spots = code_occurrences(src, token)
    if which >= len(spots):
        return src
    at = spots[which]
    return src[:at] + replacement + src[at + len(token):]


def audit(source_text=None):
    """Return a list of failure strings.  Empty means the shape holds.

    Shared with the live tree by default so every control below drives the
    IDENTICAL code path the positive control drives.
    """
    if source_text is None:
        with open(SUBJECT, "r", encoding="utf-8") as handle:
            source_text = handle.read()

    failures = []
    stripped = strip_comments(source_text)

    span = method_body(stripped, METHOD)
    if span is None:
        return [
            "REFUSED: cannot find %r in %s -- the subject moved or was "
            "renamed, and this gate measured nothing" % (METHOD, SUBJECT)
        ]

    body = stripped[span[0]:span[1]]
    base = span[0]

    # 1. The call exists, in code.
    if DESCRIBE_CALL not in body:
        failures.append(
            "publishPlane() never calls %s -- igScanDescribe() is back to "
            "being a tested function with no callers" % DESCRIBE_CALL
        )

    # 2/3/4. The three keys, and how each is guarded.
    for token, must_be_guarded in (
        (KEY_VERDICT, False),
        (KEY_USABLE, False),
        (KEY_EXTENT, True),
    ):
        at = body.find(token)
        if at < 0:
            failures.append(
                "publishPlane() never publishes %s -- the scanout verdict "
                "does not reach the IORegistry" % token
            )
            continue

        guard = guard_span(body, at)
        guarded = USABLE_CALL in guard

        if must_be_guarded and not guarded:
            failures.append(
                "%s at line %d is published WITHOUT an %s guard -- an extent "
                "over a refused surf word is arithmetic over something "
                "nobody measured"
                % (token, line_of(stripped, base + at), USABLE_CALL)
            )
        elif not must_be_guarded and guarded:
            failures.append(
                "%s at line %d is guarded by %s -- it must be published on "
                "BOTH paths, or the extent's absence stops being an "
                "explained absence"
                % (token, line_of(stripped, base + at), USABLE_CALL)
            )

        if PUBLISH_CALL not in body[at:at + 400]:
            failures.append(
                "%s at line %d composes a key that nothing publishes"
                % (token, line_of(stripped, base + at))
            )

    return failures


class LiveTreeTests(unittest.TestCase):
    """The positive control: the shipping publisher satisfies the shape."""

    def test_the_live_publisher_consumes_the_scanout_descriptor(self):
        failures = audit()
        self.assertEqual([], failures, "\n".join(failures))

    def test_the_subject_file_exists(self):
        self.assertTrue(os.path.isfile(SUBJECT), SUBJECT)


class NegativeControlTests(unittest.TestCase):
    """Each control breaks exactly one property and names which."""

    def setUp(self):
        with open(SUBJECT, "r", encoding="utf-8") as handle:
            self.src = handle.read()
        self.assertEqual([], audit(self.src), "fixture is not a pristine tree")

    def test_deleting_the_describe_call_reddens(self):
        """The property this whole module exists for.

        Mutated at a CODE offset, not by a bare replace -- see
        code_occurrences().  A bare replace hits the comment first and the
        stripper then erases the mutation, which is how this control passed
        vacuously on its first run.
        """
        mutated = replace_code_occurrence(self.src, DESCRIBE_CALL, "igScanNothing(")
        self.assertNotEqual(self.src, mutated, "mutation matched nothing")
        failures = audit(mutated)
        self.assertTrue(
            any("never calls" in f for f in failures), failures
        )

    def test_the_comment_naming_the_call_does_not_satisfy_the_gate(self):
        """Prose is not code, and here the prose is genuinely present.

        `publishPlane()`'s own comment block discusses `igScanDescribe()` by
        name.  Comment the call out and that prose remains, so a gate reading
        raw source finds the identifier it requires on a publisher that no
        longer consults the descriptor at all -- `handoff18.md` SS1d inverted.
        """
        mutated = replace_code_occurrence(
            self.src, DESCRIBE_CALL, "/* " + DESCRIBE_CALL + " */ igScanNothing("
        )
        self.assertNotEqual(self.src, mutated, "mutation matched nothing")
        self.assertIn(DESCRIBE_CALL, mutated, "the prose must survive")
        failures = audit(mutated)
        self.assertTrue(
            any("never calls" in f for f in failures), failures
        )

    def test_dropping_the_verdict_key_reddens_naming_that_key(self):
        mutated = replace_code_occurrence(self.src, KEY_VERDICT, '"1-ScanVerdictX"')
        self.assertNotEqual(self.src, mutated, "mutation matched nothing")
        failures = audit(mutated)
        self.assertTrue(
            any("never publishes" in f and KEY_VERDICT in f for f in failures),
            failures,
        )

    def test_dropping_the_usable_key_reddens_naming_that_key(self):
        mutated = replace_code_occurrence(self.src, KEY_USABLE, '"1-ScanUsableX"')
        self.assertNotEqual(self.src, mutated, "mutation matched nothing")
        failures = audit(mutated)
        self.assertTrue(
            any("never publishes" in f and KEY_USABLE in f for f in failures),
            failures,
        )

    def test_dropping_the_extent_key_reddens_naming_that_key(self):
        mutated = replace_code_occurrence(self.src, KEY_EXTENT, '"1-ScanExtentX"')
        self.assertNotEqual(self.src, mutated, "mutation matched nothing")
        failures = audit(mutated)
        self.assertTrue(
            any("never publishes" in f and KEY_EXTENT in f for f in failures),
            failures,
        )

    def test_the_three_key_controls_are_disjoint(self):
        """`1-Scan` is a PREFIX of both other keys.

        A bare substring test for the shortest would be satisfied by either
        longer one, so a publisher that lost `1-Scan` entirely would stay
        green (`a-substring-test-passes-a-suffixed-rename`).  Renaming the
        verdict key must therefore redden EXACTLY one key, not three.
        """
        mutated = replace_code_occurrence(self.src, KEY_VERDICT, '"1-ScanVerdictX"')
        failures = [f for f in audit(mutated) if "never publishes" in f]
        self.assertEqual(1, len(failures), failures)

    def test_guarding_the_verdict_key_reddens(self):
        """The inverse property: the verdict must reach BOTH paths.

        Guard it behind igScanUsable() and a refused read publishes nothing at
        all -- and an absent key is a question rather than an answer,
        indistinguishable from an older kext that never published it.
        """
        mutated = replace_code_occurrence(
            self.src,
            "if (igPlaneKey(key, sizeof(key), pipe, " + KEY_VERDICT + "))",
            "if (igScanUsable(scan) && igPlaneKey(key, sizeof(key), pipe, "
            + KEY_VERDICT
            + "))",
        )
        self.assertNotEqual(self.src, mutated, "mutation matched nothing")
        failures = audit(mutated)
        self.assertTrue(
            any("must be published on" in f for f in failures), failures
        )

    def test_unguarding_the_extent_reddens(self):
        mutated = self.src.replace(
            "if (igScanUsable(scan) &&\n        igPlaneKey(key, sizeof(key), pipe, \"1-ScanExtent\"))",
            "if (igPlaneKey(key, sizeof(key), pipe, \"1-ScanExtent\"))",
            1,
        )
        self.assertNotEqual(self.src, mutated, "mutation matched nothing")
        failures = audit(mutated)
        self.assertTrue(
            any("WITHOUT an" in f for f in failures), failures
        )

    def test_a_renamed_subject_refuses_rather_than_passing(self):
        mutated = self.src.replace(METHOD, METHOD.replace("publishPlane", "publishPlaneOld"), 1)
        self.assertNotEqual(self.src, mutated, "mutation matched nothing")
        failures = audit(mutated)
        self.assertTrue(
            any(f.startswith("REFUSED:") for f in failures), failures
        )


if __name__ == "__main__":
    unittest.main()
