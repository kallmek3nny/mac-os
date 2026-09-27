#!/usr/bin/env python3
"""Shape gate over NV3050Probe.cpp's publishGspStageVerdict().

Why this exists
---------------
Until 2026-08-19 ``loadGspFirmware()`` returned a bare ``bool`` and wrote its
refusal to ``NVLOG`` and nowhere else::

    NVLOG("GSP firmware: refusing %u-byte blob -- %s", ..., name(planned));
    return false;

``nvGspStagePlan()`` had, by then, already computed TWO independent verdicts:
the six-way ``NVGspStageResult`` and the EIGHT-way ``NVRiscvUcodeVerdict``
underneath it, the latter carried out of the plan deliberately because the
stage mapping is lossy (three ucode verdicts land on ``ERR_MALFORMED`` and
three more on ``ERR_REGION_OUTSIDE`` -- 3050-handoff32 S2).  Both died in the
log.  An operator holding a refused blob could read the whole IORegistry and
find nothing at all about why the firmware was refused.

That is the class 3050-handoff31 S1 and 3050-handoff33 S1 each closed one
subsystem over: *a diagnosis that reaches the log and not the consumer*.  The
log line is on a console nobody is reading, on a machine whose firmware just
failed to boot; the registry is the artifact that survives.

Why no existing gate can see it
-------------------------------
The same four by-construction blindnesses every module in this family lists:

  * the register gate parses ``#define``s, and this path adds none;
  * no C++ suite can link a translation unit that reaches ``IOService``, so
    ``publishGspStageVerdict()`` is unreachable from ``tests/*.cpp``;
  * ``-Werror`` has nothing to say about a well-formed function nobody calls,
    nor about a ``setProperty()`` that was never written;
  * ``tests/test_gsp_stage.cpp`` has 181 checks over ``nvGspStagePlan()`` and
    proves the verdict is CORRECT.  Nothing whatsoever proved a consumer
    reads it.  Delete the ``publishGspStageVerdict()`` call and all four
    gates stay green.

So this module pins the SHELL side, which nothing executes.

Design rules, carried from the four sibling probe modules
---------------------------------------------------------
The real tree and every mutant go through ONE ``audit()``.  A gate whose
mutants are checked by different code than its real input is two gates, and
only one of them has ever been seen to fail.

Scanning is over COMMENT-STRIPPED source.  Both the publisher's own comment
block and the call site's name every key and both verdict types in prose, so a
gate reading comments would report the keys as published on a publisher whose
``setProperty()`` calls had all been deleted -- and that is not hypothetical
here, because the comments were written in the same edit as the code.

``audit()`` REFUSES rather than passing when it cannot find its subject.  A
module that reports nothing when its anchor moves is indistinguishable from a
green one, which is the vacuity direction and the worse of the two.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CPP = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
HPP = os.path.join(ROOT, "phase0-kext", "NV3050Probe.hpp")

#
# Every anchor below was read out of phase0-kext/NV3050Probe.cpp and
# phase0-kext/NVGspStage.hpp directly rather than guessed from a sibling
# publisher.  A sentinel guessed from a sibling five lines away is the mistake
# handoff21 S1b records, and handoff33 S5a is four more of the same shape.
#
PUBLISHER = "NV3050Probe::publishGspStageVerdict("
CONSUMER = "NV3050Probe::loadGspFirmware("

VERDICT_KEY = "NVGspStageVerdict"
OK_KEY = "NVGspStageOk"
EVALUATED_KEY = "NVGspStageUcodeEvaluated"
UCODE_KEY = "NVGspStageUcode"

#
# NOTE the closing quote in every anchor below.  UCODE_KEY is a strict PREFIX
# of EVALUATED_KEY, so a bare substring count of the shorter one returns four
# where the answer is two, and the two arms would look published when only the
# evaluated flag was.
#
STAGE_OK = "NVGSP_STAGE_OK"
NOT_EVALUATED = '"not evaluated"'
UCODE_NAME_FN = "nvRiscvUcodeVerdictName("
EVALUATED_FIELD = "ucodeEvaluated"


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """Remove // and /* */ comments, preserving string literals.

    Character-by-character rather than a regex: a regex that strips ``//`` to
    end-of-line eats the ``//`` inside a URL in a string literal, and one that
    strips ``/* */`` non-greedily across a file will happily consume real code
    between a ``*/`` inside a string and the next one.

    Length is NOT preserved -- nothing downstream indexes the original text.
    """
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
                if text[i] == "\\":
                    if i + 1 < n:
                        out.append(text[i + 1])
                        i += 2
                        continue
                elif text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
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
    'does not contain' assertion in this file, so a moved anchor would read as
    a clean tree.
    """
    start = text.find(signature_fragment)
    if start < 0:
        return None
    brace = text.find("{", start)
    if brace < 0:
        return None
    end = _balanced(text, brace)
    if end < 0:
        return None
    return text[brace : end + 1]


def brace_depth(body, index):
    """Nesting depth of `index` inside `body`, where 1 is the function's own
    top level.

    Needed because statement_is_bare() below CANNOT answer "is this guarded":
    a call nested inside an `if` block is preceded by that block's `{`, which
    is a statement boundary, so bareness reports True on exactly the mutation
    property 3 exists to catch.  Depth and statement level are two different
    questions and the first is the one that means unguarded.

    Braces inside string literals are skipped -- a literal '{' in a log format
    string would otherwise shift every depth after it.
    """
    depth = 0
    i = 0
    n = len(body)
    while i < n and i < index:
        ch = body[i]
        if ch == '"' or ch == "'":
            quote = ch
            i += 1
            while i < n:
                if body[i] == "\\":
                    i += 2
                    continue
                if body[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        i += 1
    return depth


def statement_is_bare(body, index):
    """True when the statement containing `index` is at statement level.

    Walks BACK to the previous statement boundary and requires only whitespace
    between it and the token.  A regex over the preceding line cannot answer
    this: the guard may be two lines up, and a comment may sit between.
    """
    i = index - 1
    while i >= 0:
        ch = body[i]
        if ch in ";{}":
            return True
        if not ch.isspace():
            return False
        i -= 1
    return True


def audit(cpp_text, hpp_text):
    """Return a list of property failures; empty list means the tree is clean.

    Every failure names WHICH property broke, so a negative control asserts a
    specific string rather than mere non-emptiness -- otherwise a mutation that
    breaks property 2 passes a test written for property 5.
    """
    failures = []

    cpp = strip_comments(cpp_text)
    hpp = strip_comments(hpp_text)

    publisher = function_body(cpp, PUBLISHER)
    consumer = function_body(cpp, CONSUMER)

    #
    # Refusal, not silence.  If either subject cannot be found the rest of this
    # function would pass by measuring nothing.
    #
    if publisher is None:
        failures.append(
            "REFUSED: no function body found for %s -- the publisher was "
            "renamed or removed, and every property below would pass "
            "vacuously" % PUBLISHER
        )
    if consumer is None:
        failures.append(
            "REFUSED: no function body found for %s -- the consumer was "
            "renamed or removed" % CONSUMER
        )
    if publisher is None or consumer is None:
        return failures

    #
    # Property 1: the publisher is DECLARED in the shell header.
    #
    # A definition with no declaration does not compile, so this is really a
    # tripwire for the header and the implementation drifting apart under a
    # rename that touched only one of them.
    #
    if "publishGspStageVerdict" not in hpp:
        failures.append(
            "property 1: NV3050Probe.hpp does not declare "
            "publishGspStageVerdict"
        )

    #
    # Property 2: loadGspFirmware() calls the publisher exactly once.
    #
    calls = consumer.count("publishGspStageVerdict(")
    if calls != 1:
        failures.append(
            "property 2: loadGspFirmware() calls publishGspStageVerdict() "
            "%d times, expected exactly 1" % calls
        )

    #
    # Property 3: that call is UNCONDITIONAL.
    #
    # This is the whole point of the fix.  A call moved inside the refusal arm
    # still logs and still publishes on the path an operator debugging a
    # refusal would look at -- and makes the keys' ABSENCE mean two different
    # things, which is 3050-handoff33 S1a's "an absent key is a question".
    #
    if calls == 1:
        at = consumer.find("publishGspStageVerdict(")
        depth = brace_depth(consumer, at)
        if depth != 1:
            failures.append(
                "property 3: the publishGspStageVerdict() call in "
                "loadGspFirmware() sits at brace depth %d, not 1 -- it is "
                "nested inside a block, so one arm publishes nothing" % depth
            )
        elif not statement_is_bare(consumer, at):
            failures.append(
                "property 3: the publishGspStageVerdict() call in "
                "loadGspFirmware() is not a bare statement -- it hangs off "
                "another expression rather than standing alone"
            )

        #
        # Property 4: it precedes the refusal test it describes.
        #
        # Publishing after `return false` is not publishing at all, and a
        # bare-statement check alone cannot tell the difference.
        #
        refusal = consumer.find("planned != " + STAGE_OK)
        if refusal < 0:
            failures.append(
                "property 4: loadGspFirmware() no longer tests "
                "'planned != %s' -- the anchor this ordering check needs is "
                "gone" % STAGE_OK
            )
        elif at > refusal:
            failures.append(
                "property 4: publishGspStageVerdict() is called AFTER the "
                "'planned != %s' refusal, so the refusal path returns before "
                "publishing" % STAGE_OK
            )

    #
    # Property 5: all four keys are published, and the two ucode keys on BOTH
    # arms of the publisher.
    #
    # The null-plan arm returns early, so each ucode key needs its own
    # setProperty() on each side.  Counting is how a deleted arm is caught: a
    # single occurrence compiles, runs, and leaves the key absent on the arm
    # that was deleted.
    #
    expected_counts = (
        (VERDICT_KEY, 1),
        (OK_KEY, 1),
        (EVALUATED_KEY, 2),
        (UCODE_KEY, 2),
    )
    for key, want in expected_counts:
        anchor = 'setProperty("%s"' % key
        got = publisher.count(anchor)
        if got != want:
            failures.append(
                "property 5: publishGspStageVerdict() publishes %s %d "
                "time(s), expected %d" % (key, got, want)
            )

    #
    # Property 6: NVGspStageOk is an EQUALITY against NVGSP_STAGE_OK.
    #
    # Never a negation of a particular error arm.  `planned != ERR_NO_BLOB` is
    # TRUE for the five other refusals and would report a refused blob as
    # staged -- 3050-handoff32 S3's `!= ERR_HEADER` reasoning, and
    # 3050-handoff33 S2's NVPartIsTarget, in a third subsystem.
    #
    ok_at = publisher.find('setProperty("%s"' % OK_KEY)
    if ok_at >= 0:
        paren = publisher.find("(", ok_at)
        close = _balanced(publisher, paren, "(", ")")
        argument = publisher[paren : close + 1] if close > 0 else ""
        if ("planned == " + STAGE_OK) not in argument:
            failures.append(
                "property 6: %s is not published as an equality against %s "
                "-- a negation of one error arm is true for the others"
                % (OK_KEY, STAGE_OK)
            )

    #
    # Property 7: the ucode NAME is only taken when the verdict was evaluated.
    #
    # nvGspStagePlan() initialises plan.ucode to NV_RISCV_UCODE_VERDICT_LIMIT
    # and sets ucodeEvaluated only after nvRiscvUcodeVerdict() RETURNS, so an
    # unguarded name lookup on the no-blob path renders the sentinel -- a value
    # nobody measured, published in the same shape as one that was.
    #
    for at in [m.start() for m in re.finditer(re.escape(UCODE_NAME_FN),
                                              publisher)]:
        window = publisher[max(0, at - 240) : at]
        if EVALUATED_FIELD not in window:
            failures.append(
                "property 7: a %s call in publishGspStageVerdict() is not "
                "guarded by %s, so an unevaluated verdict is rendered as a "
                "name" % (UCODE_NAME_FN, EVALUATED_FIELD)
            )

    #
    # Property 8: the unevaluated case publishes a literal, not a sentinel
    # name.
    #
    # Both arms need it: the null-plan arm and the not-evaluated branch.
    # Spelling the string here rather than deriving it from
    # nvRiscvUcodeVerdictName(NV_RISCV_UCODE_VERDICT_LIMIT) is deliberate --
    # that function's default arm is a value this file cannot vouch for, and
    # comparing against a value the subject can never produce is handoff21
    # S1b's mistake.
    #
    literals = publisher.count(NOT_EVALUATED)
    if literals != 2:
        failures.append(
            "property 8: publishGspStageVerdict() spells %s %d time(s), "
            "expected 2 (the null-plan arm and the not-evaluated branch)"
            % (NOT_EVALUATED, literals)
        )

    #
    # Property 9: the null-plan arm publishes before it returns.
    #
    # An early `return` with nothing above it is the absent-key failure mode
    # again, and property 5's counts cannot see it: two setProperty() calls
    # placed both BELOW the return satisfy every count in this file while
    # publishing nothing on that arm.
    #
    guard = re.search(r"if\s*\(\s*!\s*plan\s*\)", publisher)
    if guard is None:
        failures.append(
            "property 9: publishGspStageVerdict() has no '!plan' guard -- a "
            "null plan would be dereferenced"
        )
    else:
        brace = publisher.find("{", guard.end())
        end = _balanced(publisher, brace) if brace >= 0 else -1
        arm = publisher[brace : end + 1] if end > 0 else ""
        if arm.count("setProperty(") != 2:
            failures.append(
                "property 9: the '!plan' arm of publishGspStageVerdict() "
                "makes %d setProperty() call(s), expected 2 -- an arm that "
                "returns without publishing leaves the keys absent"
                % arm.count("setProperty(")
            )

    return failures


def mutate(text, old, new, count=1):
    """Replace `old` exactly `count` times, or raise.

    A mutation that matches nothing prints ok, and one that matches more than
    it means raises loudly and looks like the mutation is wrong when the
    precondition is working (intel-handoff26 S3).  Asserting the occurrence
    COUNT is what separates the two.
    """
    found = text.count(old)
    if found != count:
        raise AssertionError(
            "mutation anchor %r occurs %d times, expected %d"
            % (old, found, count)
        )
    return text.replace(old, new)


class GspStagePublicationTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.cpp = read_text(CPP)
        cls.hpp = read_text(HPP)

    #
    # Positive control.
    #
    def test_live_tree_passes(self):
        """The shipping shell satisfies every property."""
        self.assertEqual(audit(self.cpp, self.hpp), [])

    #
    # Non-vacuity of the module itself.
    #
    def test_a_renamed_publisher_refuses_rather_than_passing(self):
        broken = mutate(
            self.cpp,
            "NV3050Probe::publishGspStageVerdict(NVGspStageResult planned,",
            "NV3050Probe::publishStageThing(NVGspStageResult planned,",
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("REFUSED:") for f in failures),
            "a renamed publisher must REFUSE, not report a clean tree: %r"
            % failures,
        )

    def test_a_renamed_consumer_refuses_rather_than_passing(self):
        broken = mutate(
            self.cpp,
            "bool NV3050Probe::loadGspFirmware(",
            "bool NV3050Probe::loadGspFirmwareOld(",
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("REFUSED:") for f in failures),
            "a renamed consumer must REFUSE: %r" % failures,
        )

    def test_comments_are_stripped_before_scanning(self):
        """The keys named in prose must not count as publications.

        The publisher's own comment block discusses the keys, so a gate that
        read comments would pass a publisher whose setProperty() calls were
        all deleted.
        """
        stripped = strip_comments(self.cpp)
        self.assertNotIn("An absent key is a question", stripped)
        self.assertIn('setProperty("%s"' % VERDICT_KEY, stripped)

    #
    # Negative controls -- one per property, each asserting WHICH broke.
    #
    def test_removing_the_header_declaration_is_caught(self):
        broken_hpp = mutate(
            self.hpp,
            "void   publishGspStageVerdict(NVGspStageResult planned,",
            "void   publishStageThingDecl(NVGspStageResult planned,",
        )
        failures = audit(self.cpp, broken_hpp)
        self.assertTrue(
            any(f.startswith("property 1:") for f in failures), failures
        )

    def test_deleting_the_call_is_caught(self):
        broken = mutate(
            self.cpp,
            "    publishGspStageVerdict(planned, &plan);\n",
            "",
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("property 2:") for f in failures), failures
        )

    def test_guarding_the_call_behind_the_refusal_is_caught(self):
        """The defect this fix removes, reintroduced one level in.

        A call moved inside the refusal arm still publishes on the path an
        operator debugging a refusal reads, which is exactly why it looks
        harmless.
        """
        broken = mutate(
            self.cpp,
            "    publishGspStageVerdict(planned, &plan);\n\n"
            "    if (planned != NVGSP_STAGE_OK) {\n",
            "    if (planned != NVGSP_STAGE_OK) {\n"
            "        publishGspStageVerdict(planned, &plan);\n",
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("property 3:") for f in failures), failures
        )

    def test_publishing_after_the_refusal_returns_is_caught(self):
        broken = mutate(
            self.cpp,
            "    publishGspStageVerdict(planned, &plan);\n\n"
            "    if (planned != NVGSP_STAGE_OK) {\n",
            "    if (planned != NVGSP_STAGE_OK) {\n",
        )
        broken = mutate(
            broken,
            "    UInt32 fileOff = (UInt32)plan.sourceOffset;",
            "    publishGspStageVerdict(planned, &plan);\n"
            "    UInt32 fileOff = (UInt32)plan.sourceOffset;",
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("property 4:") for f in failures), failures
        )

    def test_dropping_the_verdict_key_is_caught(self):
        broken = mutate(
            self.cpp,
            'setProperty("NVGspStageVerdict", nvGspStageResultName(planned));',
            "(void)planned;",
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("property 5:") and VERDICT_KEY in f
                for f in failures),
            failures,
        )

    def test_dropping_one_arm_of_the_ucode_key_is_caught(self):
        """The copy-paste shape: one arm publishes, the other silently does not.

        This is why property 5 counts rather than merely checking presence.
        """
        broken = mutate(
            self.cpp,
            '        setProperty("NVGspStageUcode", "not evaluated");\n',
            "",
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("property 5:") and "NVGspStageUcode " in f
                for f in failures),
            failures,
        )

    def test_a_negated_ok_flag_is_caught(self):
        """`!= ERR_NO_BLOB` is TRUE for the five other refusals."""
        broken = mutate(
            self.cpp,
            'setProperty("NVGspStageOk", planned == NVGSP_STAGE_OK);',
            'setProperty("NVGspStageOk", '
            "planned != NVGSP_STAGE_ERR_NO_BLOB);",
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("property 6:") for f in failures), failures
        )

    def test_an_unguarded_ucode_name_lookup_is_caught(self):
        """An unevaluated verdict is the sentinel, and naming it invents a fact."""
        broken = mutate(
            self.cpp,
            "    setProperty(\"NVGspStageUcode\",\n"
            "                plan->ucodeEvaluated "
            "? nvRiscvUcodeVerdictName(plan->ucode)\n"
            "                                     : \"not evaluated\");",
            "    setProperty(\"NVGspStageUcode\",\n"
            "                nvRiscvUcodeVerdictName(plan->ucode));",
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("property 7:") or f.startswith("property 8:")
                for f in failures),
            failures,
        )

    def test_emptying_the_null_plan_arm_is_caught(self):
        """Two calls moved below the return satisfy every count and publish nothing."""
        broken = mutate(
            self.cpp,
            "    if (!plan) {\n"
            '        setProperty("NVGspStageUcodeEvaluated", false);\n'
            '        setProperty("NVGspStageUcode", "not evaluated");\n'
            "        return;\n"
            "    }\n",
            "    if (!plan) {\n"
            "        return;\n"
            "    }\n"
            '    setProperty("NVGspStageUcodeEvaluated", false);\n'
            '    setProperty("NVGspStageUcode", "not evaluated");\n',
        )
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(f.startswith("property 9:") for f in failures), failures
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
