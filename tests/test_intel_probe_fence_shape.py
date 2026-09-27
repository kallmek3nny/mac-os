#!/usr/bin/env python3
"""Shape gate over IntelGtProbe.cpp's COMPLETION SIGNAL composition.

No C++ suite can execute a line of IntelGtProbe.cpp -- it pulls in IOService,
which does not exist off macOS. So the only observer available for the shell is
a structural one: parse the source, assert the properties, and prove the
assertions can fail by mutating the source IN MEMORY. The live tree is never
written to.

WHY THIS MODULE EXISTS
----------------------
tests/test_intel_gt_fence.cpp drives igGtfWriteCompletion() and
igGtfCompletionIsOrdered() hard, and it proves the COMPOSERS are correct. It
proves nothing whatsoever about the CONSUMER: whether composeFence() still
calls them, whether start() still calls composeFence(), whether
publishComposedFence() still publishes the attempted flag before every value it
qualifies, and whether the no-submission sentence is still published
unconditionally.

Five sibling gates cover the other compositions in this file --
test_intel_probe_mmio_path.py, test_intel_probe_ring_write_shape.py,
test_intel_probe_blit_shape.py, test_intel_probe_submit_shape.py and
test_intel_probe_image_shape.py. The fence composition landed after all five and
has none. This is the sixth and it closes that gap.

The class of defect being gated is the one this project has now met nine times:
a value published into the IORegistry that nobody measured. setProperty() with a
fabricated word is BYTE-IDENTICAL to setProperty() with a measured one. Delete
the igGtfWriteCompletion() call from composeFence() and leave the publisher
alone and every one of the four gates stays green -- IGGtFenceVerdict would
publish init()'s IG_GTF_FENCE_NOT_ATTEMPTED as though composed, and
IGGtFenceComposedSeqnoAddr would publish a zero read back out of a span nothing
wrote.

THE ORDERING PROPERTY IS THE ONE THAT NEEDS A GATE MOST
-------------------------------------------------------
IntelGtFence.hpp is explicit that a flush AFTER the store retires nothing that
matters: the CPU observes the seqno while render caches still hold unwritten
pixels, so the driver reports "done" over an unfinished frame. Both orderings
compose the identical SET of dwords and the identical total length. That is why
the header exposes igGtfCompletionIsOrdered() as a separate question, and why
fFenceOrdered is published as a key of its own rather than folded into the
verdict -- a composition can be OK and mis-ordered at the same time. A consumer
that stopped consulting the ordering predicate would publish the verdict alone,
and the one property that cannot be seen in a dword dump would silently stop
being checked.

PROPERTIES PINNED
-----------------
1. composeFence() calls igGtfWriteCompletion() and stores its verdict into
   fFenceVerdict, over the driver-owned fFenceSpan.
2. composeFence() consults igGtfCompletionIsOrdered() and stores into
   fFenceOrdered -- the ordering claim is measured, not assumed.
3. start() calls composeFence() and publishComposedFence(), composition first.
4. IGGtFenceAttempted is published FIRST, above every value it qualifies.
5. IGGtFenceAttempted is published UNCONDITIONALLY -- an absent key is a
   question, not an answer.
6. The span is sized from IntelGtFence.hpp's own policy constant by way of
   IG_GTP_FENCE_SPAN_DWORDS, never a second literal.
7. igGtfWriteCompletion() is called at exactly ONE site in the whole file.
8. The file submits NOTHING: no ELSP write, no doorbell, no register store on
   the fence path. The absence is a GATED property rather than a prose claim.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SUBJECT = os.path.join(ROOT, "phase0-kext", "IntelGtProbe.cpp")

COMPOSER = "composeFence"
PUBLISHER = "publishComposedFence"
WRITER = "igGtfWriteCompletion"
ORDER_FN = "igGtfCompletionIsOrdered"

ATTEMPTED_KEY = '"IGGtFenceAttempted"'
VERDICT_KEY = '"IGGtFenceVerdict"'
VERDICT_NAME_KEY = '"IGGtFenceVerdictName"'
ORDERED_KEY = '"IGGtFenceOrdered"'
SIGNALLED_KEY = '"IGGtFenceSignalled"'

ATTEMPTED_FIELD = "fFenceAttempted"
VERDICT_FIELD = "fFenceVerdict"
ORDERED_FIELD = "fFenceOrdered"
SPAN_FIELD = "fFenceSpan"

SPAN_POLICY = "IG_GTP_FENCE_SPAN_DWORDS"

# Names that would mean this driver actually fired something. Spelled here so
# the no-submission sentence in IntelGtProbe.hpp's own banner is a MEASURED
# property rather than a comment nobody checks -- intel-handoff37 section 3
# property 5, arrived at for the fence layer.
SUBMISSION_TOKENS = (
    "igGtxExeclistSqReg",
    "igGtxExeclistControlReg",
    "regWrite32",
)


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(source):
    """Blank comments, keep string CONTENT, preserve every byte offset.

    Offsets must survive, because every span this module computes is a slice of
    the same string. Comments are replaced space-for-space and newline-for-
    newline rather than removed.

    Stripping is MANDATORY rather than stylistic here: composeFence() and
    publishComposedFence() both carry comment blocks naming every key and every
    field below, and the header banner spells igGtfWriteCompletion in prose. A
    gate reading comments would find every token it is looking for on a
    composer whose calls had all been deleted.
    """
    out = []
    i = 0
    n = len(source)
    while i < n:
        ch = source[i]
        if ch == '"':
            j = i + 1
            while j < n:
                if source[j] == "\\":
                    j += 2
                    continue
                if source[j] == '"':
                    j += 1
                    break
                j += 1
            out.append(source[i:j])
            i = j
            continue
        if source.startswith("//", i):
            j = source.find("\n", i)
            if j == -1:
                j = n
            out.append(" " * (j - i))
            i = j
            continue
        if source.startswith("/*", i):
            j = source.find("*/", i + 2)
            j = n if j == -1 else j + 2
            for c in source[i:j]:
                out.append("\n" if c == "\n" else " ")
            i = j
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def method_span(source, qualified_name):
    """Return (start, end) byte offsets of one method BODY, brace-balanced.

    A balanced walk rather than a regex, for handoff25 section 1c's reason: a
    character class excluding only the statement terminator is not scoped to the
    statement, and a body-matching regex runs past the closing brace onto the
    next definition.
    """
    pattern = re.compile(r"IntelGtProbe::" + re.escape(qualified_name) + r"\s*\(")
    match = pattern.search(source)
    if match is None:
        raise AssertionError(
            "REFUSING TO PASS: IntelGtProbe::%s() is not in %s. A gate that "
            "reports nothing when its own subject has moved is "
            "indistinguishable from a green one." % (qualified_name, SUBJECT)
        )
    open_brace = source.find("{", match.end())
    if open_brace == -1:
        raise AssertionError(
            "REFUSING TO PASS: no body brace after IntelGtProbe::%s()"
            % qualified_name
        )
    depth = 0
    i = open_brace
    while i < len(source):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return (open_brace + 1, i)
        i += 1
    raise AssertionError(
        "REFUSING TO PASS: unbalanced braces in IntelGtProbe::%s()"
        % qualified_name
    )


GUARD_WORD_RE = re.compile(r"\b(if|else|for|while|switch|case|do)\b")


def statement_is_bare(body, index):
    """True when the statement containing `index` carries no guard in front of it.

    Two things this is NOT, both of which were tried elsewhere in this tree and
    both of which pass a guarded publication:

      * BRACE DEPTH. `if (x) setProperty(...);` has no braces at all, so the
        guarded call sits at exactly the same depth as an unconditional one.
        Bareness is not nesting depth -- intel-handoff38 section 2.
      * A REGEX OVER THE PRECEDING LINE. handoff25 section 1c is the cost of
        that: a character class excluding only the statement terminator is not
        scoped to the statement.

    So: walk BACK to the previous statement boundary by a paren-balanced scan,
    then read what stands between that boundary and the key. A boundary is a
    `;` at paren depth zero, or either brace -- a `}` met on the way back is the
    END of a preceding block statement, which terminates it just as firmly as a
    semicolon and must not be walked into, or the `if` heading that block would
    be read as this statement's own guard and a correct unconditional
    publication sitting below any guarded block would report as guarded.

    The walk starts INSIDE setProperty()'s argument list, so the first thing it
    meets is that call's own unmatched `(`. Stepping out of an enclosing call is
    expected rather than an imbalance, hence the clamp at zero.
    """
    paren = 0
    boundary = 0
    i = index
    while i > 0:
        i -= 1
        ch = body[i]
        if ch == ")":
            paren += 1
        elif ch == "(":
            if paren > 0:
                paren -= 1
        elif ch == "{" or ch == "}":
            boundary = i + 1
            break
        elif ch == ";" and paren == 0:
            boundary = i + 1
            break
    return GUARD_WORD_RE.search(body[boundary:index]) is None


def audit(source):
    """Return a list of failure strings. Empty list means every property holds.

    One audit shared with the live tree AND with every negative control, so a
    control cannot pass by exercising a different code path than the positive
    one does.
    """
    failures = []
    stripped = strip_comments(source)

    compose_start, compose_end = method_span(stripped, COMPOSER)
    compose_body = stripped[compose_start:compose_end]

    publish_start, publish_end = method_span(stripped, PUBLISHER)
    publish_body = stripped[publish_start:publish_end]

    start_start, start_end = method_span(stripped, "start")
    start_body = stripped[start_start:start_end]

    # Property 1 -- the composer calls the writer, over the driver's own span,
    # and KEEPS the verdict. A call whose verdict is dropped composes the same
    # dwords and publishes NOT_ATTEMPTED over them.
    if WRITER + "(" not in compose_body:
        failures.append(
            "composeFence() does not call %s(): the completion signal would be "
            "published from init()'s zeroes as though composed" % WRITER
        )
    else:
        if VERDICT_FIELD not in compose_body:
            failures.append(
                "composeFence() does not store into %s: the verdict published "
                "would be the NOT_ATTEMPTED arm on a path that DID attempt"
                % VERDICT_FIELD
            )
        if SPAN_FIELD not in compose_body:
            failures.append(
                "composeFence() does not hand %s to the writer: the pair would "
                "be composed somewhere the publisher cannot read back"
                % SPAN_FIELD
            )

    # Property 2 -- the ORDERING claim is measured rather than assumed. This is
    # the property with no other observer: both orderings compose the identical
    # set of dwords and the identical length, so nothing else in this tree can
    # tell a flush-then-store from a store-then-flush.
    if ORDER_FN + "(" not in compose_body:
        failures.append(
            "composeFence() does not consult %s(): a store the CPU observes "
            "BEFORE its flush retires reports done over an unfinished frame, "
            "and no dword dump can show it" % ORDER_FN
        )
    elif ORDERED_FIELD not in compose_body:
        failures.append(
            "composeFence() does not store into %s: the ordering answer would "
            "reach nothing" % ORDERED_FIELD
        )

    # Property 3 -- start() drives both halves, composition first.
    compose_call = start_body.find(COMPOSER + "(")
    publish_call = start_body.find(PUBLISHER + "(")
    if compose_call == -1:
        failures.append(
            "start() does not call %s(): nothing composes the completion signal"
            % COMPOSER
        )
    if publish_call == -1:
        failures.append(
            "start() does not call %s(): the composition reaches no consumer"
            % PUBLISHER
        )
    if compose_call != -1 and publish_call != -1 and compose_call > publish_call:
        failures.append(
            "start() publishes the fence BEFORE composing it: every key would "
            "carry init()'s initialiser rather than a measurement"
        )

    # Property 4 -- the attempted flag sorts above every value it qualifies.
    attempted_at = publish_body.find(ATTEMPTED_KEY)
    if attempted_at == -1:
        failures.append(
            "%s() does not publish %s: an operator cannot tell a composition "
            "that was refused from one that was never reached"
            % (PUBLISHER, ATTEMPTED_KEY)
        )
    for qualified in (VERDICT_KEY, VERDICT_NAME_KEY, ORDERED_KEY):
        at = publish_body.find(qualified)
        if at == -1:
            failures.append(
                "%s() does not publish %s" % (PUBLISHER, qualified)
            )
        elif attempted_at != -1 and attempted_at > at:
            failures.append(
                "%s publishes AFTER %s: the attempted flag must sort above "
                "every value it qualifies" % (ATTEMPTED_KEY, qualified)
            )

    # Property 5 -- the attempted flag is unconditional, on EVERY path.
    if attempted_at != -1 and not statement_is_bare(publish_body, attempted_at):
        failures.append(
            "%s is published inside a conditional: an absent key is a question, "
            "not an answer, and is indistinguishable from an older kext"
            % ATTEMPTED_KEY
        )

    # Property 6 -- the span is sized from the header's own policy constant.
    if SPAN_POLICY not in stripped:
        failures.append(
            "%s does not occur in the file: a second literal for the pair's "
            "dword count goes stale silently when IntelGtFence.hpp's policy "
            "moves, and the span would be composed past its own end"
            % SPAN_POLICY
        )

    # Property 7 -- exactly ONE call site in the whole file.
    sites = len(re.findall(re.escape(WRITER) + r"\s*\(", stripped))
    if sites != 1:
        failures.append(
            "%s() is called at %d sites, expected exactly 1: a second composer "
            "would race the first into the same %s array"
            % (WRITER, sites, SPAN_FIELD)
        )

    # Property 8 -- nothing here fires anything. The banner claims it; this
    # measures it.
    for token in SUBMISSION_TOKENS:
        if token in stripped:
            failures.append(
                "%s occurs in %s: this class composes and publishes, and "
                "submitting is a different act from composing a submission"
                % (token, os.path.basename(SUBJECT))
            )

    return failures


class FenceCompositionShapeTests(unittest.TestCase):
    """The positive controls: every property must hold on the LIVE tree."""

    def setUp(self):
        self.source = read(SUBJECT)
        self.stripped = strip_comments(self.source)

    def test_the_live_tree_satisfies_every_property(self):
        failures = audit(self.source)
        self.assertEqual(failures, [], "\n  ".join([""] + failures))

    def test_the_composer_calls_the_completion_writer(self):
        start, end = method_span(self.stripped, COMPOSER)
        body = self.stripped[start:end]
        self.assertIn(WRITER + "(", body)

    def test_the_composer_measures_the_ordering(self):
        start, end = method_span(self.stripped, COMPOSER)
        body = self.stripped[start:end]
        self.assertIn(ORDER_FN + "(", body)
        self.assertIn(ORDERED_FIELD, body)

    def test_start_calls_the_composer_and_the_publisher(self):
        start, end = method_span(self.stripped, "start")
        body = self.stripped[start:end]
        self.assertIn(COMPOSER + "(", body)
        self.assertIn(PUBLISHER + "(", body)

    def test_the_composition_precedes_its_publication(self):
        start, end = method_span(self.stripped, "start")
        body = self.stripped[start:end]
        self.assertLess(body.find(COMPOSER + "("), body.find(PUBLISHER + "("))

    def test_the_attempted_flag_is_published_first(self):
        start, end = method_span(self.stripped, PUBLISHER)
        body = self.stripped[start:end]
        at = body.find(ATTEMPTED_KEY)
        self.assertNotEqual(at, -1)
        for qualified in (VERDICT_KEY, VERDICT_NAME_KEY, ORDERED_KEY):
            self.assertLess(at, body.find(qualified), qualified)

    def test_the_attempted_flag_is_unconditional(self):
        start, end = method_span(self.stripped, PUBLISHER)
        body = self.stripped[start:end]
        self.assertTrue(statement_is_bare(body, body.find(ATTEMPTED_KEY)))

    def test_the_span_is_sized_from_the_headers_policy(self):
        self.assertIn(SPAN_POLICY, self.stripped)

    def test_the_writer_has_exactly_one_call_site(self):
        sites = re.findall(re.escape(WRITER) + r"\s*\(", self.stripped)
        self.assertEqual(len(sites), 1)

    def test_nothing_in_this_file_submits(self):
        for token in SUBMISSION_TOKENS:
            self.assertNotIn(token, self.stripped, token)

    def test_the_stripper_removes_prose_naming_the_keys(self):
        """Comment stripping is load-bearing, not stylistic.

        composeFence()'s own comment block discusses the writer and the ordering
        predicate by name. If the stripper leaked comments, a composer with
        every call deleted would still satisfy every token search above.
        """
        self.assertIn("//", self.source)
        self.assertEqual(len(self.stripped), len(self.source),
                         "the stripper must preserve every byte offset")
        for line in self.stripped.splitlines():
            self.assertNotIn("//", line)


class RefusalTests(unittest.TestCase):
    """A gate reporting nothing when its anchor moves is indistinguishable from
    a green one. All of these must RAISE rather than pass quietly."""

    def test_a_renamed_composer_refuses(self):
        source = read(SUBJECT).replace("IntelGtProbe::" + COMPOSER,
                                       "IntelGtProbe::composeFenceRenamed")
        with self.assertRaises(AssertionError) as caught:
            audit(source)
        self.assertIn("REFUSING TO PASS", str(caught.exception))

    def test_a_renamed_publisher_refuses(self):
        source = read(SUBJECT).replace("IntelGtProbe::" + PUBLISHER,
                                       "IntelGtProbe::publishComposedFenceX")
        with self.assertRaises(AssertionError) as caught:
            audit(source)
        self.assertIn("REFUSING TO PASS", str(caught.exception))

    def test_a_missing_subject_file_refuses(self):
        with self.assertRaises(AssertionError) as caught:
            audit("int main(void) { return 0; }\n")
        self.assertIn("REFUSING TO PASS", str(caught.exception))


class NegativeControlTests(unittest.TestCase):
    """Each control mutates the source IN MEMORY and asserts WHICH property
    breaks. A control that reddens a property other than its own is not
    isolating anything, and one that reddens nothing is a mutation that matched
    nothing rather than a vacuous subject -- so every control asserts the
    failure TEXT, never merely that the list is non-empty."""

    def setUp(self):
        self.source = read(SUBJECT)

    def mutate(self, old, new, count=1):
        self.assertIn(old, self.source,
                      "mutation anchor absent -- the control would no-op and "
                      "read as proof the property is vacuous")
        self.assertEqual(self.source.count(old), count,
                         "mutation anchor occurs %d times, expected %d -- an "
                         "over-matching anchor reddens more than it means"
                         % (self.source.count(old), count))
        return self.source.replace(old, new)

    def mutate_in_method(self, method, old, new):
        """Replace `old` at its CODE offset inside one method body.

        A whole-file textual replace is wrong for the two function names this
        module cares most about. composeFence()'s own comment block names
        igGtfWriteCompletion five times and igGtfCompletionIsOrdered twice, and
        the header banner names both again -- so a whole-file anchor either
        trips the count guard (measured: 5 and 2) or, worse, mutates prose the
        audit never reads and no-ops while reading as proof the property is
        vacuous.

        strip_comments() preserves every byte offset, which is what makes this
        possible: find the site in the stripped copy, cut it out of the raw one.
        """
        stripped = strip_comments(self.source)
        start, end = method_span(stripped, method)
        at = stripped.find(old, start, end)
        self.assertNotEqual(at, -1,
                            "%s does not occur in %s()'s CODE -- the control "
                            "would no-op" % (old, method))
        self.assertEqual(stripped.count(old, start, end), 1,
                         "%s occurs %d times in %s()'s code, expected 1"
                         % (old, stripped.count(old, start, end), method))
        return self.source[:at] + new + self.source[at + len(old):]

    def test_gutting_the_writer_call_reddens_property_1(self):
        """Deleting the one call site reddens property 1 AND property 7.

        That coupling is honest rather than a leak: the call site and the
        call-site COUNT are two readings of the same fact, so a mutation that
        removes the site must move both. Both are asserted, so a future edit
        that silently decoupled them would redden here.
        """
        mutated = self.mutate_in_method(COMPOSER, WRITER + "(",
                                        "igGtfWriteCompletionGone(")
        failures = audit(mutated)
        self.assertTrue(any("does not call %s()" % WRITER in f
                            for f in failures), failures)
        self.assertTrue(any("called at 0 sites" in f for f in failures),
                        failures)

    def test_dropping_the_ordering_check_reddens_property_2(self):
        mutated = self.mutate_in_method(COMPOSER, ORDER_FN + "(",
                                        "igGtfCompletionIsOrderedGone(")
        failures = audit(mutated)
        self.assertTrue(any("does not consult" in f for f in failures),
                        failures)

    def test_guarding_the_attempted_flag_reddens_property_5(self):
        anchor = "setProperty(" + ATTEMPTED_KEY
        mutated = self.mutate(
            anchor,
            "if (" + VERDICT_FIELD + ") setProperty(" + ATTEMPTED_KEY)
        failures = audit(mutated)
        self.assertTrue(any("inside a conditional" in f for f in failures),
                        failures)

    def test_a_second_writer_call_site_reddens_property_7(self):
        anchor = "void IntelGtProbe::" + PUBLISHER + "(void)\n{"
        mutated = self.mutate(
            anchor,
            "void IntelGtProbe::" + PUBLISHER + "(void)\n{\n    (void) "
            + WRITER + "(nullptr, 0, 0, 0);")
        failures = audit(mutated)
        self.assertTrue(any("called at 2 sites" in f for f in failures),
                        failures)

    def test_a_submission_call_reddens_property_8(self):
        anchor = "void IntelGtProbe::" + PUBLISHER + "(void)\n{"
        mutated = self.mutate(
            anchor,
            "void IntelGtProbe::" + PUBLISHER + "(void)\n{\n    (void) "
            "regWrite32(0, 0);")
        failures = audit(mutated)
        self.assertTrue(any("regWrite32 occurs in" in f for f in failures),
                        failures)

    def test_removing_the_composer_from_start_reddens_property_3(self):
        stripped = strip_comments(self.source)
        start, end = method_span(stripped, "start")
        body = stripped[start:end]
        at = body.find(COMPOSER + "(")
        self.assertNotEqual(at, -1)
        mutated = (self.source[:start + at] + "composeFenceGone(" +
                   self.source[start + at + len(COMPOSER) + 1:])
        failures = audit(mutated)
        self.assertTrue(any("start() does not call %s()" % COMPOSER in f
                            for f in failures), failures)

    def test_publishing_before_composing_reddens_property_3(self):
        """Swap the two calls in start() and nothing else.

        The mutation is done on the byte offsets of the two call LINES inside
        start()'s own body, so it cannot accidentally reorder a sibling
        composition -- the file has five other compose/publish pairs and a
        whole-file replace would move all of them.
        """
        stripped = strip_comments(self.source)
        start, end = method_span(stripped, "start")
        body = stripped[start:end]
        c_at = body.find(COMPOSER + "(")
        p_at = body.find(PUBLISHER + "(")
        self.assertNotEqual(c_at, -1)
        self.assertNotEqual(p_at, -1)
        self.assertLess(c_at, p_at)

        c_line_start = body.rfind("\n", 0, c_at) + 1
        c_line_end = body.find("\n", c_at)
        p_line_start = body.rfind("\n", 0, p_at) + 1
        p_line_end = body.find("\n", p_at)

        c_line = self.source[start + c_line_start:start + c_line_end]
        p_line = self.source[start + p_line_start:start + p_line_end]
        mutated = (self.source[:start + c_line_start] + p_line +
                   self.source[start + c_line_end:start + p_line_start] +
                   c_line + self.source[start + p_line_end:])
        failures = audit(mutated)
        self.assertTrue(any("BEFORE composing" in f for f in failures),
                        failures)

    def test_publishing_the_verdict_above_the_attempted_flag_reddens_property_4(self):
        stripped = strip_comments(self.source)
        start, end = method_span(stripped, PUBLISHER)
        body = stripped[start:end]
        a_at = body.find(ATTEMPTED_KEY)
        v_at = body.find(VERDICT_KEY)
        self.assertNotEqual(a_at, -1)
        self.assertNotEqual(v_at, -1)
        self.assertLess(a_at, v_at)

        a_line_start = body.rfind("\n", 0, a_at) + 1
        a_line_end = body.find("\n", a_at)
        v_line_start = body.rfind("\n", 0, v_at) + 1
        v_line_end = body.find("\n", v_at)

        a_line = self.source[start + a_line_start:start + a_line_end]
        v_line = self.source[start + v_line_start:start + v_line_end]
        mutated = (self.source[:start + a_line_start] + v_line +
                   self.source[start + a_line_end:start + v_line_start] +
                   a_line + self.source[start + v_line_end:])
        failures = audit(mutated)
        self.assertTrue(any("must sort above" in f for f in failures),
                        failures)


if __name__ == "__main__":
    unittest.main(verbosity=2)
