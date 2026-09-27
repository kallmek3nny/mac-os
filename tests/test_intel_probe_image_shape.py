#!/usr/bin/env python3
"""Shape gate over IntelGtProbe.cpp's ring-context IMAGE composition.

No C++ suite can execute a line of IntelGtProbe.cpp -- it pulls in IOService,
which does not exist off macOS. So the only observer available for the shell is
a structural one: parse the source, assert the properties, and prove the
assertions can fail by mutating the source IN MEMORY. The live tree is never
written to.

WHY THIS MODULE EXISTS
----------------------
tests/test_intel_gt_image.cpp drives igGtiWriteRingImage() hard, and it proves
the WRITER is correct. It proves nothing whatsoever about the CONSUMER: whether
composeImage() still calls that writer, whether start() still calls
composeImage(), whether publishComposedImage() still publishes the verdict
before the values it qualifies, and whether the no-submission sentence is still
published unconditionally.

intel-handoff37 section 6 item 1 names this as the one composition in
IntelGtProbe.cpp with no structural observer. The three sibling gates --
test_intel_probe_ring_write_shape.py, test_intel_probe_submit_shape.py and
test_intel_probe_blit_shape.py -- cover the other three. This is the fourth and
it closes that item.

The class of defect being gated is the one this project has now met eight times:
a value published into the IORegistry that nobody measured. setProperty() with a
fabricated word is BYTE-IDENTICAL to setProperty() with a measured one. Delete
the igGtiWriteRingImage() call and leave the publisher alone and every one of
the four gates stays green -- IGGtImageDwords would publish init()'s zero as
though composed.

PROPERTIES PINNED
-----------------
1. composeImage() calls igGtiWriteRingImage() and stores into BOTH out-values.
2. start() calls composeImage() and publishComposedImage(), composition first.
3. The verdict key is published FIRST, before the dwords it qualifies.
4. The no-submission sentence is published UNCONDITIONALLY (not inside an if).
5. The image span is sized from IntelGtImage.hpp's own policy constant, never a
   second literal.
6. igGtiWriteRingImage() is called at exactly ONE site in the whole file.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SUBJECT = os.path.join(ROOT, "phase0-kext", "IntelGtProbe.cpp")

COMPOSER = "composeImage"
PUBLISHER = "publishComposedImage"
WRITER = "igGtiWriteRingImage"

VERDICT_KEY = '"IGGtImageVerdict"'
DWORDS_KEY = '"IGGtImageDwords"'

VERDICT_FIELD = "fWriteVerdict"
DWORDS_FIELD = "fImageDwords"
IMAGE_FIELD = "fImage"

DWORDS_POLICY = "IG_GTI_RING_IMAGE_DWORDS_POLICY_12"


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(source):
    """Blank comments, keep string CONTENT, preserve every byte offset.

    Offsets must survive, because every span this module computes is a slice of
    the same string. Comments are replaced space-for-space and newline-for-
    newline rather than removed.

    Stripping is MANDATORY rather than stylistic here: publishComposedImage()'s
    own comment block names IGGtImageVerdict and discusses the no-submission
    sentence in prose, so a gate reading comments would find every key it is
    looking for on a publisher whose setProperty() calls had all been deleted.
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

    Two things this is NOT, both of which were tried and both of which pass a
    guarded publication:

      * BRACE DEPTH. `if (x) setProperty(...);` has no braces at all, so the
        guarded call sits at exactly the same depth as an unconditional one.
        Bareness is not nesting depth.
      * A REGEX OVER THE PRECEDING LINE. handoff25 section 1c is the cost of
        that: a character class excluding only the statement terminator is not
        scoped to the statement.

    So: walk BACK to the previous statement boundary by a paren-balanced scan,
    then read what stands between that boundary and the key. A boundary is a
    `;` at paren depth zero, or either brace -- a `}` met on the way back is the
    END of a preceding block statement, which terminates it just as firmly as a
    semicolon and must not be walked into, or the `if` heading that block would
    be read as this statement's own guard.

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

    # Property 1 -- the composer calls the writer and keeps BOTH out-values.
    if WRITER + "(" not in compose_body:
        failures.append(
            "composeImage() does not call %s(): the image would be published "
            "from init()'s zeroes as though composed" % WRITER
        )
    else:
        if VERDICT_FIELD not in compose_body:
            failures.append(
                "composeImage() does not store into %s: the verdict published "
                "would be the NOT_ATTEMPTED arm on a path that DID attempt"
                % VERDICT_FIELD
            )
        if "&" + DWORDS_FIELD not in compose_body:
            failures.append(
                "composeImage() does not pass &%s to the writer: the dword "
                "count published would be a literal nobody measured"
                % DWORDS_FIELD
            )
        if IMAGE_FIELD not in compose_body:
            failures.append(
                "composeImage() does not hand %s to the writer: the image "
                "would be composed somewhere the publisher cannot see"
                % IMAGE_FIELD
            )

    # Property 2 -- start() drives both halves, composition first.
    compose_call = start_body.find(COMPOSER + "(")
    publish_call = start_body.find(PUBLISHER + "(")
    if compose_call == -1:
        failures.append(
            "start() does not call %s(): nothing composes the image" % COMPOSER
        )
    if publish_call == -1:
        failures.append(
            "start() does not call %s(): the composition reaches no consumer"
            % PUBLISHER
        )
    if compose_call != -1 and publish_call != -1 and compose_call > publish_call:
        failures.append(
            "start() publishes the image BEFORE composing it: every key would "
            "carry init()'s initialiser rather than a measurement"
        )

    # Property 3 -- verdict published FIRST, above the values it qualifies.
    verdict_at = publish_body.find(VERDICT_KEY)
    dwords_at = publish_body.find(DWORDS_KEY)
    if verdict_at == -1:
        failures.append(
            "%s() does not publish %s: an operator cannot tell a refused "
            "composition from a successful one" % (PUBLISHER, VERDICT_KEY)
        )
    if dwords_at == -1:
        failures.append(
            "%s() does not publish %s" % (PUBLISHER, DWORDS_KEY)
        )
    if verdict_at != -1 and dwords_at != -1 and verdict_at > dwords_at:
        failures.append(
            "%s publishes AFTER %s: the verdict must sort above every value it "
            "qualifies" % (VERDICT_KEY, DWORDS_KEY)
        )

    # Property 4 -- the verdict is unconditional, on BOTH arms.
    if verdict_at != -1 and not statement_is_bare(publish_body, verdict_at):
        failures.append(
            "%s is published inside a conditional: an absent key is a question, "
            "not an answer, and is indistinguishable from an older kext"
            % VERDICT_KEY
        )

    # Property 5 -- the span is sized from IntelGtImage.hpp's own constant.
    if DWORDS_POLICY not in compose_body:
        failures.append(
            "composeImage() does not size the image from %s: a second literal "
            "here goes stale silently when the header's policy moves"
            % DWORDS_POLICY
        )

    # Property 6 -- exactly ONE call site in the whole file.
    sites = len(re.findall(re.escape(WRITER) + r"\s*\(", stripped))
    if sites != 1:
        failures.append(
            "%s() is called at %d sites, expected exactly 1: a second composer "
            "would race the first into the same fImage array" % (WRITER, sites)
        )

    return failures


class ImageCompositionShapeTests(unittest.TestCase):
    """The positive controls: every property must hold on the LIVE tree."""

    def setUp(self):
        self.source = read(SUBJECT)
        self.stripped = strip_comments(self.source)

    def test_the_live_tree_satisfies_every_property(self):
        failures = audit(self.source)
        self.assertEqual(failures, [], "\n  ".join([""] + failures))

    def test_the_composer_calls_the_image_writer(self):
        start, end = method_span(self.stripped, COMPOSER)
        body = self.stripped[start:end]
        self.assertIn(WRITER + "(", body)

    def test_start_calls_the_composer_and_the_publisher(self):
        start, end = method_span(self.stripped, "start")
        body = self.stripped[start:end]
        self.assertIn(COMPOSER + "(", body)
        self.assertIn(PUBLISHER + "(", body)

    def test_the_composition_precedes_its_publication(self):
        start, end = method_span(self.stripped, "start")
        body = self.stripped[start:end]
        self.assertLess(body.find(COMPOSER + "("), body.find(PUBLISHER + "("))

    def test_the_verdict_is_published_before_the_dwords(self):
        start, end = method_span(self.stripped, PUBLISHER)
        body = self.stripped[start:end]
        self.assertLess(body.find(VERDICT_KEY), body.find(DWORDS_KEY))

    def test_the_verdict_is_unconditional(self):
        start, end = method_span(self.stripped, PUBLISHER)
        body = self.stripped[start:end]
        self.assertTrue(statement_is_bare(body, body.find(VERDICT_KEY)))

    def test_the_span_is_sized_from_the_headers_policy(self):
        start, end = method_span(self.stripped, COMPOSER)
        body = self.stripped[start:end]
        self.assertIn(DWORDS_POLICY, body)

    def test_the_writer_has_exactly_one_call_site(self):
        sites = re.findall(re.escape(WRITER) + r"\s*\(", self.stripped)
        self.assertEqual(len(sites), 1)

    def test_the_stripper_removes_prose_naming_the_keys(self):
        """Comment stripping is load-bearing, not stylistic.

        The publisher's own comment block discusses the verdict in prose. If the
        stripper leaked comments, a publisher with every setProperty() deleted
        would still satisfy every key search above.
        """
        self.assertIn("//", self.source)
        self.assertEqual(len(self.stripped), len(self.source),
                         "the stripper must preserve every byte offset")
        for line in self.stripped.splitlines():
            self.assertNotIn("//", line)


class RefusalTests(unittest.TestCase):
    """A gate reporting nothing when its anchor moves is indistinguishable from
    a green one. Both of these must RAISE rather than pass quietly."""

    def test_a_renamed_composer_refuses(self):
        source = read(SUBJECT).replace("IntelGtProbe::" + COMPOSER,
                                       "IntelGtProbe::composeImageRenamed")
        with self.assertRaises(AssertionError) as caught:
            audit(source)
        self.assertIn("REFUSING TO PASS", str(caught.exception))

    def test_a_renamed_publisher_refuses(self):
        source = read(SUBJECT).replace("IntelGtProbe::" + PUBLISHER,
                                       "IntelGtProbe::publishComposedImageX")
        with self.assertRaises(AssertionError) as caught:
            audit(source)
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

    def test_gutting_the_writer_call_reddens_property_1(self):
        mutated = self.mutate(WRITER + "(" + IMAGE_FIELD,
                              "igGtiWriteRingImageGone(" + IMAGE_FIELD)
        failures = audit(mutated)
        self.assertTrue(any("does not call" in f for f in failures), failures)

    def test_dropping_the_dwords_out_param_reddens_property_1(self):
        mutated = self.mutate("&" + DWORDS_FIELD, "nullptr")
        failures = audit(mutated)
        self.assertTrue(any(DWORDS_FIELD in f and "out" not in f.lower() or
                            "&" + DWORDS_FIELD in f for f in failures),
                        failures)

    def test_swapping_the_publication_order_reddens_property_3(self):
        start, end = method_span(strip_comments(self.source), PUBLISHER)
        stripped = strip_comments(self.source)
        body = stripped[start:end]
        v_line_start = body.rfind("\n", 0, body.find(VERDICT_KEY)) + 1
        v_line_end = body.find("\n", body.find(VERDICT_KEY))
        d_line_start = body.rfind("\n", 0, body.find(DWORDS_KEY)) + 1
        d_line_end = body.find("\n", body.find(DWORDS_KEY))
        v_line = self.source[start + v_line_start:start + v_line_end]
        d_line = self.source[start + d_line_start:start + d_line_end]
        mutated = (self.source[:start + v_line_start] + d_line +
                   self.source[start + v_line_end:start + d_line_start] +
                   v_line + self.source[start + d_line_end:])
        failures = audit(mutated)
        self.assertTrue(any("sort above" in f for f in failures), failures)

    def test_guarding_the_verdict_reddens_property_4(self):
        anchor = 'setProperty("IGGtImageVerdict"'
        mutated = self.mutate(anchor,
                              'if (fImageDwords) setProperty("IGGtImageVerdict"')
        failures = audit(mutated)
        self.assertTrue(any("inside a conditional" in f for f in failures),
                        failures)

    def test_a_second_literal_span_reddens_property_5(self):
        mutated = self.mutate(DWORDS_POLICY + ",", "12u,")
        failures = audit(mutated)
        self.assertTrue(any(DWORDS_POLICY in f for f in failures), failures)

    def test_a_second_call_site_reddens_property_6(self):
        anchor = "void IntelGtProbe::" + PUBLISHER + "(void)\n{"
        mutated = self.mutate(
            anchor,
            "void IntelGtProbe::" + PUBLISHER + "(void)\n{\n    (void) "
            + WRITER + "(nullptr, 0, 0, 0, 0, 0, 0, 0, 0, nullptr);")
        failures = audit(mutated)
        self.assertTrue(any("called at 2 sites" in f for f in failures),
                        failures)

    def test_removing_the_composer_from_start_reddens_property_2(self):
        start, end = method_span(strip_comments(self.source), "start")
        stripped = strip_comments(self.source)
        body = stripped[start:end]
        at = body.find(COMPOSER + "(")
        self.assertNotEqual(at, -1)
        mutated = (self.source[:start + at] + "composeImageGone(" +
                   self.source[start + at + len(COMPOSER) + 1:])
        failures = audit(mutated)
        self.assertTrue(any("start() does not call" in f for f in failures),
                        failures)


if __name__ == "__main__":
    unittest.main(verbosity=2)
