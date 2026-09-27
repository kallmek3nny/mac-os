#!/usr/bin/env python3
"""Shape gate over NV3050Probe.cpp's WPR2 publication path.

Why this exists
---------------
``checkWpr2()`` used to return a bare ``bool`` meaning "the firmware boot
precondition is satisfied", and that bool was FALSE for two categorically
different reasons:

  * WPR2 is genuinely UP -- a previous boot left a write-protected region
    standing, and the fix is a full engine reset.
  * The registers did not decode at all -- BAR0 is not mapped, or these
    turing/tu102 offsets are wrong for GA107.

Only the second is a bug in THIS driver, and it is the one somebody can go and
fix. Collapsing them published the first as an answer to the second.

Worse, the caller published ``GSP_WPR2_ADDR_LO`` and ``GSP_WPR2_ADDR_HI``
unconditionally, and the bool tested ADDR_HI ONLY. So a refused ADDR_LO beside a
healthy ADDR_HI reported the precondition SATISFIED and published an all-ones
low word into the IORegistry as though it had been measured.
``setProperty("GSP_WPR2_ADDR_LO", 0xFFFFFFFF, 32)`` is byte-identical to a
register that genuinely holds that value.

That is handoff18 S1d's class for the fourth time, and the third time in this
shell. No existing gate can see it: the register gate checks constants (both
offsets are correct), no C++ suite can link a translation unit that reaches
IOService, and clang is content with a well-typed sentinel.

``tests/test_falcon.cpp`` now drives ``nvWpr2Verdict()`` directly -- that is the
pure side. This module pins the SHELL side, which nothing executes: that the
verdict is consulted, that the address words are published only behind it, and
that the flag goes on both arms.

Design rule, carried from the three sibling probe modules: the real tree and
every mutant go through ONE ``audit()``. A gate whose mutants are checked by
different code than its real input is two gates, and only one of them has ever
been seen to fail.

Scanning is over COMMENT-STRIPPED source. The fix's own comments in both
``checkWpr2()`` and the caller discuss ``GSP_WPR2_ADDR_LO`` and the all-ones
sentinel by name, so a gate that counted prose would find the very thing it
forbids in a pristine tree -- and would equally accept a branch whose real code
had been deleted and replaced by a comment.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CPP = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
HPP = os.path.join(ROOT, "phase0-kext", "NV3050Probe.hpp")

#
# Every anchor below was read out of phase0-kext/NV3050Probe.cpp directly rather
# than guessed from a sibling publisher. publishIdentityUnreadable() and
# publishGspUnreadable() are the same SHAPE but share no string with this path,
# and a sentinel guessed from a sibling is the mistake handoff21 S1b records.
#
ADDR_LO_KEY = "GSP_WPR2_ADDR_LO"
ADDR_HI_KEY = "GSP_WPR2_ADDR_HI"
VERDICT_KEY = "GSPWpr2Verdict"
READABLE_KEY = "GSPWpr2Readable"
DOWN_KEY = "GSPWpr2Down"
REASON_KEY = "GSPWpr2UnreadableReason"

UNREADABLE = "NVWPR2_UNREADABLE"
DOWN = "NVWPR2_DOWN"

FUNCTIONS = (
    ("checkWpr2", "NV3050Probe::checkWpr2("),
    ("inspectGspEngine", "NV3050Probe::inspectGspEngine("),
)

# setProperty("<key>" ... -- the key is captured so a publication can be
# attributed to the block it lives in.
SETPROP_RE = re.compile(r'setProperty\s*\(\s*"([^"]+)"')


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """Blank out C and C++ comments, preserving string literals and offsets."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"' or ch == "'":
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _balanced(text, open_index, opener="{", closer="}"):
    """Index of the closer matching the opener at open_index, or -1.

    Balanced-bracket walk rather than "up to the next terminator": a character
    class that excludes only the terminator is not scoped to the statement, and
    spelling it that way is what reddened handoff25's gate on correct code.
    """
    depth = 0
    i = open_index
    while i < len(text):
        if text[i] == opener:
            depth += 1
        elif text[i] == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def function_body(text, signature_fragment):
    """Brace-balanced body of the function whose signature contains
    signature_fragment, or None.

    None rather than "" on every failure path: an empty string satisfies every
    'does not contain' assertion in this file, which is exactly how a shape gate
    goes vacuous.
    """
    start = text.find(signature_fragment)
    if start < 0:
        return None
    open_brace = text.find("{", start)
    if open_brace < 0:
        return None
    close = _balanced(text, open_brace)
    if close < 0:
        return None
    return text[open_brace : close + 1]


def readable_arms(body):
    """Return (then_arm, else_arm) of the `if (wpr2Readable)` statement, or
    (None, None) if it or either brace cannot be located.

    Both arms are brace-matched. The point of separating them is that the
    address words must live in the THEN arm and nowhere else -- a publication
    hoisted above the `if` satisfies every 'is published somewhere' assertion
    while restoring the exact defect.
    """
    at = body.find("if (wpr2Readable)")
    if at < 0:
        return (None, None)

    then_open = body.find("{", at)
    if then_open < 0:
        return (None, None)
    then_close = _balanced(body, then_open)
    if then_close < 0:
        return (None, None)
    then_arm = body[then_open : then_close + 1]

    rest = body[then_close + 1 :]
    else_at = rest.find("else")
    if else_at < 0:
        return (then_arm, None)
    else_open = rest.find("{", else_at)
    if else_open < 0:
        return (then_arm, None)
    else_close = _balanced(rest, else_open)
    if else_close < 0:
        return (then_arm, None)
    return (then_arm, rest[else_open : else_close + 1])


def audit(cpp_raw, hpp_raw):
    """Return a list of failure reasons. Empty list means every property holds.

    Reasons are strings so negative controls can assert WHICH property broke,
    not merely that something did.
    """
    failures = []
    src = strip_comments(cpp_raw)
    hdr = strip_comments(hpp_raw)

    bodies = {}
    for name, fragment in FUNCTIONS:
        body = function_body(src, fragment)
        if body is None:
            failures.append("%s: body not found (signature %r)" % (name, fragment))
        bodies[name] = body

    # ---- property 1: checkWpr2() returns a VERDICT, not a bool -------------
    #
    # A bool cannot carry three outcomes. The declaration is checked as well as
    # the definition, because a header still promising `bool` would compile
    # (implicit conversion) and silently collapse UNREADABLE into UP.
    #
    if "UInt32 NV3050Probe::checkWpr2(" not in src:
        failures.append(
            "checkWpr2: definition does not return UInt32 -- a bool cannot "
            "distinguish UNREADABLE from UP, which is the whole point")

    if re.search(r"UInt32\s+checkWpr2\s*\(", hdr) is None:
        failures.append(
            "NV3050Probe.hpp: checkWpr2 is not declared returning UInt32")

    check = bodies["checkWpr2"]
    if check is not None:
        if "nvWpr2Verdict(" not in check:
            failures.append(
                "checkWpr2: does not call nvWpr2Verdict() -- the join is what "
                "consults BOTH halves; the old bool tested ADDR_HI only, so a "
                "refused ADDR_LO went straight into the reported range")

        #
        # A RETURN of each verdict, not a mention of it. Presence alone is
        # satisfied by the `if (verdict == NVWPR2_UNREADABLE)` comparison three
        # lines above the return, so a body whose return had been gutted to
        # `return false;` still contained the token and the check stayed green.
        # That is an assertion whose message and whose behaviour disagreed --
        # the class this whole module exists to catch, one level up.
        #
        for verdict in (UNREADABLE, DOWN, "NVWPR2_UP"):
            if re.search(r"\breturn\s+%s\s*;" % re.escape(verdict),
                         check) is None:
                failures.append(
                    "checkWpr2: never returns %s -- an arm that cannot be "
                    "reached is a verdict the caller can never see" % verdict)

        if re.search(r"\breturn\s+(true|false)\s*;", check):
            failures.append(
                "checkWpr2: still returns a bare true/false -- that is the "
                "two-outcome signal this change exists to replace")

    # ---- property 2: the caller consults the verdict before publishing -----
    inspect = bodies["inspectGspEngine"]
    if inspect is not None:
        if "checkWpr2(" not in inspect:
            failures.append("inspectGspEngine: never calls checkWpr2()")

        if UNREADABLE not in inspect:
            failures.append(
                "inspectGspEngine: never tests for %s -- the refusal is "
                "produced and nothing consumes it" % UNREADABLE)

        then_arm, else_arm = readable_arms(inspect)

        if then_arm is None:
            failures.append(
                "inspectGspEngine: no brace-matched `if (wpr2Readable)` -- the "
                "address words must be published behind the verdict, not "
                "beside it")
        else:
            then_keys = SETPROP_RE.findall(then_arm)
            for key in (ADDR_LO_KEY, ADDR_HI_KEY, DOWN_KEY):
                if key not in then_keys:
                    failures.append(
                        "inspectGspEngine: %r is not published inside the "
                        "readable arm -- the healthy path must still publish "
                        "its real measurements" % key)

        if else_arm is None:
            failures.append(
                "inspectGspEngine: no brace-matched `else` arm -- the "
                "unreadable path must say so rather than publish nothing at "
                "all, because silence reads as an older kext")
        else:
            else_keys = SETPROP_RE.findall(else_arm)
            for key in (ADDR_LO_KEY, ADDR_HI_KEY, DOWN_KEY):
                if key in else_keys:
                    failures.append(
                        "inspectGspEngine: the UNREADABLE arm publishes %r. "
                        "That word is regRead32()'s refusal value, and "
                        "setProperty() with a refused read is byte-identical "
                        "to setProperty() with a register that genuinely "
                        "holds it" % key)

            if REASON_KEY not in else_keys:
                failures.append(
                    "inspectGspEngine: the UNREADABLE arm does not publish %r "
                    "-- an absent key is a question, but an unexplained "
                    "absence is not even that" % REASON_KEY)

        #
        # Whole-function counts. The address words must be published from
        # exactly ONE site each: a second publication hoisted above the `if`
        # stands behind neither arm and restores the defect while every
        # per-arm check above stays green.
        #
        for key in (ADDR_LO_KEY, ADDR_HI_KEY):
            n = len(re.findall(
                r'setProperty\s*\(\s*"%s"' % re.escape(key), inspect))
            if n != 1:
                failures.append(
                    "inspectGspEngine: %r published at %d sites, expected 1 -- "
                    "a site outside the readable arm is a site behind no "
                    "verdict" % (key, n))

        # ---- property 3: the verdict and flag go on BOTH paths ------------
        #
        # Published once, unconditionally, ABOVE the branch. handoff18 S1d's
        # rule for GSPEngineReadable and handoff25's for NVIdentityReadable: a
        # flag published only where things went wrong is indistinguishable from
        # an older kext that never published it, and absence reads as healthy.
        #
        for key in (VERDICT_KEY, READABLE_KEY):
            n = len(re.findall(
                r'setProperty\s*\(\s*"%s"' % re.escape(key), inspect))
            if n != 1:
                failures.append(
                    "inspectGspEngine: %r published at %d sites, expected "
                    "exactly 1 unconditional site" % (key, n))

            if then_arm is not None and key in SETPROP_RE.findall(then_arm):
                failures.append(
                    "inspectGspEngine: %r is published INSIDE the readable "
                    "arm -- on the unreadable path it would then be absent, "
                    "and absence cannot be told from an older kext" % key)

            if else_arm is not None and key in SETPROP_RE.findall(else_arm):
                failures.append(
                    "inspectGspEngine: %r is published INSIDE the unreadable "
                    "arm -- a flag published only where things went wrong "
                    "reads as healthy by its absence" % key)

        if "nvWpr2VerdictName(" not in inspect:
            failures.append(
                "inspectGspEngine: does not publish the verdict NAME -- a bare "
                "integer in the IORegistry needs this file to decode")

        # ---- property 4: an unmeasured precondition is not "satisfied" ----
        #
        # wpr2Down must be an equality against DOWN, never a negation of UP.
        # `verdict != NVWPR2_UP` is true for UNREADABLE, which would report a
        # precondition this driver could not measure as satisfied and send the
        # go/no-go straight through it.
        #
        if re.search(r"wpr2Down\s*=\s*\(?\s*wpr2Verdict\s*==\s*%s" % DOWN,
                     inspect) is None:
            failures.append(
                "inspectGspEngine: wpr2Down is not `wpr2Verdict == %s`. "
                "Anything written as a negation of UP is TRUE for "
                "UNREADABLE, so a precondition that was never measured would "
                "read as satisfied" % DOWN)

        if "wpr2Down" not in inspect.split("bool ready")[-1]:
            failures.append(
                "inspectGspEngine: the go/no-go does not consume wpr2Down")

    return failures


def mutate(text, old, new, occurrences=1):
    """Replace `old` with `new`, refusing unless it occurs exactly `occurrences`
    times. A mutation whose anchor has drifted must RAISE rather than silently
    match nothing and print ok."""
    if text.count(old) != occurrences:
        raise AssertionError(
            "mutation anchor %r occurs %d times, expected %d"
            % (old, text.count(old), occurrences)
        )
    return text.replace(old, new)


class Wpr2PublicationTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.cpp = read_text(CPP)
        cls.hpp = read_text(HPP)

    def test_live_tree_passes(self):
        """Positive control: the shipping shell satisfies every property."""
        self.assertEqual(audit(self.cpp, self.hpp), [])

    def test_publishing_the_address_words_unconditionally_is_caught(self):
        """This is the exact defect, restored: both words published beside the
        verdict rather than behind it, so a refused read reaches the
        IORegistry as a measurement."""
        mutant = mutate(
            self.cpp,
            '    if (wpr2Readable) {\n'
            '        setProperty("GSP_WPR2_ADDR_LO", wpr2Lo, 32);\n'
            '        setProperty("GSP_WPR2_ADDR_HI", wpr2Hi, 32);\n'
            '        setProperty("GSPWpr2Down", wpr2Down);\n'
            '    } else {',
            '    setProperty("GSP_WPR2_ADDR_LO", wpr2Lo, 32);\n'
            '    setProperty("GSP_WPR2_ADDR_HI", wpr2Hi, 32);\n'
            '    setProperty("GSPWpr2Down", wpr2Down);\n'
            '    if (wpr2Readable) {\n'
            '    } else {',
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any("is not published inside the readable arm" in r for r in reasons),
            "expected the readable-arm check to redden, got %r" % (reasons,),
        )

    def test_a_second_hoisted_publication_site_is_caught(self):
        """The subtler form: the readable arm is left intact and correct, and a
        SECOND publication is added above the branch. Every per-arm check stays
        green while the defect is fully restored."""
        mutant = mutate(
            self.cpp,
            '    setProperty("GSPWpr2Verdict", nvWpr2VerdictName(wpr2Verdict));',
            '    setProperty("GSP_WPR2_ADDR_LO", wpr2Lo, 32);\n'
            '    setProperty("GSPWpr2Verdict", nvWpr2VerdictName(wpr2Verdict));',
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any("published at 2 sites" in r for r in reasons),
            "expected the site-count check to redden, got %r" % (reasons,),
        )

    def test_publishing_an_address_word_on_the_unreadable_arm_is_caught(self):
        mutant = mutate(
            self.cpp,
            '        setProperty("GSPWpr2UnreadableReason",',
            '        setProperty("GSP_WPR2_ADDR_HI", wpr2Hi, 32);\n'
            '        setProperty("GSPWpr2UnreadableReason",',
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any("the UNREADABLE arm publishes" in r for r in reasons),
            "expected the unreadable-arm check to redden, got %r" % (reasons,),
        )

    def test_down_written_as_a_negation_of_up_is_caught(self):
        """`!= NVWPR2_UP` is TRUE for UNREADABLE, so a precondition that was
        never measured would report as satisfied and the go/no-go would pass
        straight through it. This mutation changes nothing else."""
        mutant = mutate(
            self.cpp,
            "    bool wpr2Down     = (wpr2Verdict == NVWPR2_DOWN);",
            "    bool wpr2Down     = (wpr2Verdict != NVWPR2_UP);",
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any("is not `wpr2Verdict == NVWPR2_DOWN`" in r for r in reasons),
            "expected the wpr2Down check to redden, got %r" % (reasons,),
        )

    def test_the_flag_moving_inside_the_readable_arm_is_caught(self):
        """A flag published only on the healthy path is absent on the failure
        path, and its absence is indistinguishable from an older kext."""
        mutant = mutate(
            self.cpp,
            '    setProperty("GSPWpr2Readable", wpr2Readable);\n'
            "\n"
            "    if (wpr2Readable) {\n",
            "    if (wpr2Readable) {\n"
            '        setProperty("GSPWpr2Readable", wpr2Readable);\n',
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any("is published INSIDE the readable arm" in r for r in reasons),
            "expected the both-paths flag check to redden, got %r" % (reasons,),
        )

    def test_dropping_the_unreadable_reason_is_caught(self):
        mutant = mutate(
            self.cpp,
            '        setProperty("GSPWpr2UnreadableReason",',
            '        (void)0; NVLOG("unreadable"); if (false) setProperty("GSPWpr2Skipped",',
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any("does not publish 'GSPWpr2UnreadableReason'" in r or
                'does not publish "GSPWpr2UnreadableReason"' in r
                for r in reasons),
            "expected the reason-string check to redden, got %r" % (reasons,),
        )

    def test_checkWpr2_returning_a_bool_again_is_caught(self):
        """The signature is the carrier of the third outcome. A bool return
        compiles fine and silently collapses UNREADABLE into one of the other
        two."""
        mutant = mutate(
            self.cpp,
            "UInt32 NV3050Probe::checkWpr2(",
            "bool NV3050Probe::checkWpr2(",
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any("definition does not return UInt32" in r for r in reasons),
            "expected the return-type check to redden, got %r" % (reasons,),
        )

    def test_a_bare_boolean_return_inside_checkWpr2_is_caught(self):
        mutant = mutate(
            self.cpp,
            "        return NVWPR2_UNREADABLE;",
            "        return false;",
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any("still returns a bare true/false" in r for r in reasons),
            "expected the bare-bool check to redden, got %r" % (reasons,),
        )
        self.assertTrue(
            any("never returns NVWPR2_UNREADABLE" in r for r in reasons),
            "expected the reachable-arm check to redden too, got %r" % (reasons,),
        )

    def test_checkWpr2_skipping_the_join_is_caught(self):
        """nvWpr2Verdict() is what consults BOTH halves. Testing ADDR_HI alone
        is the original bug: a refused ADDR_LO beside a healthy ADDR_HI reads
        as a satisfied precondition."""
        mutant = mutate(
            self.cpp,
            "    UInt32 verdict = nvWpr2Verdict(lo, hi);",
            "    UInt32 verdict = nvWpr2IsUp(hi) ? NVWPR2_UP : NVWPR2_DOWN;",
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any("does not call nvWpr2Verdict()" in r for r in reasons),
            "expected the join check to redden, got %r" % (reasons,),
        )

    def test_audit_refuses_when_a_subject_is_absent(self):
        """A gate that cannot find its subject must REFUSE, never pass over an
        empty string. This is the failure mode that makes a shape gate vacuous:
        every 'does not contain' assertion is satisfied by ""."""
        mutant = mutate(
            self.cpp,
            "UInt32 NV3050Probe::checkWpr2(",
            "UInt32 NV3050Probe::checkWpr2Region(",
        )
        reasons = audit(mutant, self.hpp)
        self.assertTrue(
            any(r.startswith("checkWpr2: body not found") for r in reasons),
            "expected a refusal, got %r" % (reasons,),
        )

    def test_comment_prose_alone_does_not_satisfy_the_gate(self):
        """Both the fix's comments name GSP_WPR2_ADDR_LO and the all-ones
        sentinel -- they have to, because the point of the unreadable arm is
        what it does NOT publish. A gate reading prose would find the key it
        exists to forbid in a PRISTINE tree, and would equally accept an arm
        whose real code had been deleted.

        Named as a property rather than as an incidental fact: if somebody
        rewrites those comments, this test goes red and the answer is to delete
        it, not to weaken strip_comments().
        """
        raw_inspect = function_body(self.cpp, "NV3050Probe::inspectGspEngine(")
        self.assertIsNotNone(raw_inspect)
        _, raw_else = readable_arms(raw_inspect)
        self.assertIsNotNone(raw_else)
        self.assertIn(
            "byte-identical", raw_else,
            "the unreadable arm's comment should still explain why neither "
            "address word is published")

        stripped = strip_comments(self.cpp)
        inspect = function_body(stripped, "NV3050Probe::inspectGspEngine(")
        self.assertIsNotNone(inspect)
        _, else_arm = readable_arms(inspect)
        self.assertIsNotNone(else_arm)
        self.assertNotIn(
            ADDR_LO_KEY, else_arm,
            "after comment stripping the unreadable arm should mention "
            "%s nowhere -- prose is not code" % ADDR_LO_KEY)


if __name__ == "__main__":
    unittest.main()
