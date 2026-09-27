#!/usr/bin/env python3
"""Shape gate over loadGspFirmware()'s refusal LOG arm -- the fourth member.

Why this exists
---------------
``3050-handoff35.md`` taught ``loadGspFirmware()`` to publish its two staging
verdicts through ``publishGspStageVerdict()``, and
``tests/test_probe_gsp_stage_publication.py`` pins that publication with nine
properties. What no gate covers is the OTHER consumer of the same two verdicts:
the ``NVLOG`` line in the refusal arm at ``NV3050Probe.cpp:1648``.

That is the split the three existing log-arm gates exist for
(``test_probe_gsp_result_log_arms.py``, ``test_probe_identity_log_arms.py``,
``test_probe_boot42_log_arms.py``): a publication gate proves the REGISTRY is
right and says nothing about whether the log still agrees with it. An operator
reading a serial console and an operator reading the IORegistry must not be told
different things about one refusal.

The specific defect this forbids
--------------------------------
The log's ucode half must stay guarded by ``ucodeEvaluated``. ``NVGspStage.hpp``
initialises ``plan.ucode`` to ``NV_RISCV_UCODE_VERDICT_LIMIT`` -- measured, not
remembered -- precisely because zero is ``NV_RISCV_UCODE_OK``, so an unevaluated
field left at its initialiser would report "well-formed" for a blob nobody
validated. ``NV_RISCV_UCODE_VERDICT_LIMIT`` is the one value
``nvRiscvUcodeVerdictName()`` deliberately REFUSES to name.

So a "simplification" that drops the guard and names ``plan.ucode``
unconditionally neither crashes nor reddens any existing gate. It prints a
refusal string for the one case the flag exists to distinguish -- a blob that
failed staging BEFORE the ucode was ever examined -- and the console then
contradicts an IORegistry that got this right. handoff18 section 1d's
fabricated-publication class, arriving at the log instead of the registry.

Why the guard predicate accepts two shapes
------------------------------------------
The first draft of this module went RED on correct shipping code, and the
failing check was its POSITIVE CONTROL -- the fourth session running in this
tree where that is how a first draft fails. It looked for ``if (ucodeEvaluated)
{ ... }`` and the subject uses a TERNARY inside the ``NVLOG`` call. Both shapes
are real guards; a gate that recognises only the one its author pictured reports
a defect in the subject when the defect is in the gate. So ``guard_verdict()``
accepts either, and the negative controls below drive the SAME function the
audit does rather than a second copy of its logic -- a control asserting against
a reimplementation proves the reimplementation, not the gate.

What this does NOT claim
------------------------
It executes nothing. It is structural, over comment- and string-stripped source,
and a future edit keeping every marker while changing what an arm MEANS passes.
It also does not re-assert the publication properties --
``test_probe_gsp_stage_publication.py`` owns those, and two modules with a claim
on one subject is how a disagreement becomes ambiguous.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PROBE = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
STAGE = os.path.join(ROOT, "phase0-kext", "NVGspStage.hpp")

SUBJECT = "NV3050Probe::loadGspFirmware"
FLAG = "ucodeEvaluated"
NAMER = "nvRiscvUcodeVerdictName"
EXPECTED_INIT = "NV_RISCV_UCODE_VERDICT_LIMIT"


def strip_comments(text):
    """Remove // and /* */ comments, and empty every string literal.

    Mandatory rather than stylistic, verbatim the reason
    ``test_probe_gsp_boot_publication.py`` gives: the subject's own comment block
    discusses ``ucodeEvaluated`` by name and quotes both verdict functions, so a
    prose-reading gate would find every token it exists to require on a function
    whose guard had been deleted. The string literals go too -- the log's format
    string carries "ucode" and the fallback carries "not evaluated".
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
        elif c == '"':
            out.append('""')
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def body_of(text, signature):
    """Return the brace-balanced body of the named function, or None.

    A balanced walk rather than a regex, for ``handoff25.md`` section 1c's
    reason: a character class excluding only the statement terminator is not
    scoped to the statement.
    """
    at = text.find(signature)
    if at < 0:
        return None
    open_at = text.find("{", at)
    if open_at < 0:
        return None
    depth = 0
    i = open_at
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_at + 1:i]
        i += 1
    return None


def if_guarded_spans(body, flag):
    """Spans of `body` governed by an ``if`` whose condition mentions `flag`.

    Balanced walks on both the condition parens and the body braces, never a
    negated character class.
    """
    spans = []
    for m in re.finditer(r"\bif\s*\(", body):
        i = m.end()
        depth = 1
        while i < len(body) and depth:
            if body[i] == "(":
                depth += 1
            elif body[i] == ")":
                depth -= 1
            i += 1
        if flag not in body[m.end():i - 1]:
            continue
        j = i
        while j < len(body) and body[j] in " \t\r\n":
            j += 1
        if j < len(body) and body[j] == "{":
            depth = 0
            k = j
            while k < len(body):
                if body[k] == "{":
                    depth += 1
                elif body[k] == "}":
                    depth -= 1
                    if depth == 0:
                        spans.append((j + 1, k))
                        break
                k += 1
        else:
            k = body.find(";", j)
            if k >= 0:
                spans.append((j, k + 1))
    return spans


def statement_before(body, at):
    """Text from the previous statement boundary up to `at`.

    Bareness is decided by walking back to a boundary rather than by a regex
    over the preceding line -- ``handoff25.md`` section 1c is the cost of the
    other choice.
    """
    start = max(body.rfind(";", 0, at),
                body.rfind("{", 0, at),
                body.rfind("}", 0, at)) + 1
    return body[start:at]


def unguarded_namer_calls(body):
    """Offsets of ``nvRiscvUcodeVerdictName(`` calls NOT guarded by the flag.

    Two guard shapes are accepted, because the subject uses one and the obvious
    refactor uses the other:

      * ``if (plan.ucodeEvaluated) { ... namer(...) ... }``
      * ``plan.ucodeEvaluated ? namer(...) : "not evaluated"``

    The ternary arm requires the flag AND the ``?`` to precede the call inside
    one statement, so a call sitting in the FALSE arm -- or a statement that
    merely mentions the flag somewhere after it -- is not counted as guarded.
    """
    spans = if_guarded_spans(body, FLAG)
    bare = []
    for m in re.finditer(r"\b%s\s*\(" % re.escape(NAMER), body):
        at = m.start()
        if any(s <= at < e for s, e in spans):
            continue
        stmt = statement_before(body, at)
        if ternary_guards(stmt):
            continue
        bare.append(at)
    return bare


def ternary_guards(stmt):
    """True when `stmt` leaves the call that follows it in a ternary's TRUE arm.

    The flag preceding a ``?`` is NOT sufficient, and the control that caught
    that is worth keeping in mind: in

        plan.ucodeEvaluated ? "not evaluated" : namer(plan.ucode)

    the flag precedes the ``?`` and the call still runs exactly when the verdict
    was never evaluated. The ``:`` is what separates the arms, so a colon
    between the ``?`` and the call means the call sits in the FALSE arm.

    ``::`` is stepped over rather than counted, so a qualified name in the true
    arm is not misread as an arm break.
    """
    if FLAG not in stmt or "?" not in stmt:
        return False
    q = stmt.rindex("?")
    if stmt.index(FLAG) > q:
        return False
    tail = stmt[q + 1:]
    i = 0
    while i < len(tail):
        if tail[i] == ":":
            if i + 1 < len(tail) and tail[i + 1] == ":":
                i += 2
                continue
            return False
        i += 1
    return True


def guard_verdict(body):
    """The central property, as a list of failure strings.

    Shared by the audit and by every negative control below, deliberately: a
    control asserting against a reimplementation of the predicate proves the
    reimplementation.
    """
    failures = []
    calls = list(re.finditer(r"\b%s\s*\(" % re.escape(NAMER), body))
    if not calls:
        failures.append(
            "P3: %s() is not called at all -- the log no longer names the "
            "ucode half of the refusal" % NAMER)
        return failures
    if FLAG not in body:
        failures.append(
            "P3: %s is never tested -- an unevaluated plan holds %s, which "
            "%s() refuses to name" % (FLAG, EXPECTED_INIT, NAMER))
        return failures
    bare = unguarded_namer_calls(body)
    if bare:
        failures.append(
            "P3: %d of %d %s() call(s) are NOT guarded by %s -- a blob refused "
            "before the ucode was examined would be named from the initialiser "
            "sentinel" % (len(bare), len(calls), NAMER, FLAG))
    return failures


def audit():
    """One audit shared with the live tree, as every module in this family does.

    Returns (failures, facts). A REFUSAL rather than silence when the subject
    cannot be found: a gate reporting nothing when its anchor moves is
    indistinguishable from a green one.
    """
    failures = []
    facts = {}

    for path in (PROBE, STAGE):
        if not os.path.isfile(path):
            failures.append("REFUSED: %s is missing" % os.path.basename(path))
            return failures, facts

    with open(PROBE, "r", encoding="utf-8", errors="replace") as fh:
        probe = strip_comments(fh.read())
    with open(STAGE, "r", encoding="utf-8", errors="replace") as fh:
        stage = strip_comments(fh.read())

    body = body_of(probe, SUBJECT)
    if body is None:
        failures.append(
            "REFUSED: %s not found in NV3050Probe.cpp -- the subject moved or "
            "was renamed, so this module measured NOTHING" % SUBJECT)
        return failures, facts
    facts["body_len"] = len(body)

    # ---- P1: the refusal arm logs at all.
    if "NVLOG" not in body:
        failures.append("P1: loadGspFirmware() no longer logs anything")

    # ---- P2: both verdict-name functions are reached from this body.
    for fn in ("nvGspStageResultName", NAMER):
        if fn + "(" not in body.replace(" (", "("):
            failures.append(
                "P2: %s() is not called by loadGspFirmware() -- the log no "
                "longer names that half of the refusal" % fn)

    # ---- P3: every ucode name call is guarded by ucodeEvaluated.
    guard_failures = guard_verdict(body)
    failures.extend(guard_failures)
    if not guard_failures:
        facts["namer_calls"] = len(
            re.findall(r"\b%s\s*\(" % re.escape(NAMER), body))

    # ---- P4: the initialiser sentinel is still the unnameable LIMIT.
    #
    # P3 is load-bearing only while this holds. If a future edit initialises
    # plan.ucode to something nameable, the guard stops mattering and this
    # module would keep passing while protecting nothing -- so the premise is
    # asserted rather than assumed.
    init = re.search(r"\bucode\s*=\s*(NV_RISCV_UCODE_[A-Z_]+)", stage)
    if init is None:
        failures.append(
            "P4: NVGspStage.hpp no longer initialises .ucode to a named "
            "NV_RISCV_UCODE_* value -- P3's premise is unverifiable")
    else:
        facts["ucode_init"] = init.group(1)
        if init.group(1) != EXPECTED_INIT:
            failures.append(
                "P4: .ucode initialises to %s, not %s -- an unevaluated field "
                "is now nameable, so the guard P3 requires protects nothing"
                % (init.group(1), EXPECTED_INIT))

    # ---- P5: the registry is written too, so the log is not the sole consumer.
    #
    # Not a re-assertion of the publication gate's properties; it owns those.
    # This pins only the claim the log arm depends on.
    if "publishGspStageVerdict" not in body:
        failures.append(
            "P5: loadGspFirmware() no longer calls publishGspStageVerdict() -- "
            "the refusal would reach the console and nothing else")

    return failures, facts


class GspStageLogArmTests(unittest.TestCase):
    def test_audit_is_green_on_the_live_tree(self):
        failures, _ = audit()
        self.assertEqual(failures, [], "\n".join(failures))

    def test_subject_is_findable(self):
        _, facts = audit()
        self.assertIn("body_len", facts)
        self.assertGreater(facts["body_len"], 200)

    def test_ucode_initialiser_is_the_limit_sentinel(self):
        _, facts = audit()
        self.assertEqual(facts.get("ucode_init"), EXPECTED_INIT)

    def test_the_namer_is_actually_called(self):
        _, facts = audit()
        self.assertGreaterEqual(facts.get("namer_calls", 0), 1)


class RefusalTests(unittest.TestCase):
    """A gate that passes by measuring nothing is worse than no gate."""

    def test_a_renamed_subject_refuses_rather_than_passing(self):
        failures = []
        if body_of("void somethingElse(void) { return; }", SUBJECT) is None:
            failures.append("REFUSED")
        self.assertEqual(failures, ["REFUSED"])

    def test_a_body_with_no_namer_call_reddens(self):
        self.assertTrue(guard_verdict("NVLOG(\"\"); return false;"))


class StripperTests(unittest.TestCase):
    """The stripper is load-bearing, so it is tested rather than trusted."""

    def test_line_comment_is_removed(self):
        self.assertNotIn(FLAG, strip_comments("int x; // ucodeEvaluated\n"))

    def test_block_comment_is_removed(self):
        self.assertNotIn(FLAG, strip_comments("/* ucodeEvaluated */ int x;"))

    def test_string_literal_is_emptied(self):
        self.assertNotIn(FLAG, strip_comments('NVLOG("ucodeEvaluated");'))

    def test_code_survives(self):
        self.assertIn(FLAG, strip_comments("if (plan.ucodeEvaluated) { }"))


class NegativeControls(unittest.TestCase):
    """Each control drives guard_verdict() -- the function the audit uses.

    handoff18 section 1e's every-pre-existing-check-green rule does not apply to
    an all-new module. What these establish instead is that the central property
    has mutations that redden it, and that both real guard shapes are accepted
    so the gate cannot fail the way its own first draft did.
    """

    TERNARY = """
        publishGspStageVerdict(planned, &plan);
        if (planned != NVGSP_STAGE_OK) {
            NVLOG("", (unsigned)blobLen, nvGspStageResultName(planned),
                  plan.ucodeEvaluated ? nvRiscvUcodeVerdictName(plan.ucode)
                                      : "");
            return false;
        }
    """

    IF_BLOCK = """
        publishGspStageVerdict(planned, &plan);
        if (plan.ucodeEvaluated) {
            NVLOG("", nvRiscvUcodeVerdictName(plan.ucode));
        }
    """

    def test_positive_control_ternary_is_clean(self):
        self.assertEqual(guard_verdict(strip_comments(self.TERNARY)), [])

    def test_positive_control_if_block_is_clean(self):
        self.assertEqual(guard_verdict(strip_comments(self.IF_BLOCK)), [])

    def test_dropping_the_ternary_guard_reddens(self):
        mutated = self.TERNARY.replace(
            'plan.ucodeEvaluated ? nvRiscvUcodeVerdictName(plan.ucode)\n'
            '                                      : ""',
            'nvRiscvUcodeVerdictName(plan.ucode)')
        self.assertNotEqual(mutated, self.TERNARY)
        self.assertTrue(guard_verdict(strip_comments(mutated)))

    def test_dropping_the_if_guard_reddens(self):
        mutated = self.IF_BLOCK.replace("if (plan.ucodeEvaluated) {", "{")
        self.assertNotEqual(mutated, self.IF_BLOCK)
        self.assertTrue(guard_verdict(strip_comments(mutated)))

    def test_hoisting_the_call_out_of_the_guard_reddens(self):
        mutated = """
            const char *u = nvRiscvUcodeVerdictName(plan.ucode);
            if (plan.ucodeEvaluated) { NVLOG("", u); }
        """
        self.assertTrue(guard_verdict(strip_comments(mutated)))

    def test_a_second_bare_call_beside_a_guarded_one_reddens(self):
        # The copy-paste shape: one call correctly guarded, a later one bare.
        # A predicate testing only "is the flag present" would stay green here.
        mutated = self.TERNARY + '\n        NVLOG("", nvRiscvUcodeVerdictName(plan.ucode));'
        self.assertTrue(guard_verdict(strip_comments(mutated)))

    def test_a_call_in_the_false_arm_reddens(self):
        # The flag precedes the call, but the call sits AFTER the colon -- so it
        # runs exactly when the verdict was never evaluated.
        mutated = """
            NVLOG("", plan.ucodeEvaluated ? ""
                                          : nvRiscvUcodeVerdictName(plan.ucode));
        """
        self.assertTrue(guard_verdict(strip_comments(mutated)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
