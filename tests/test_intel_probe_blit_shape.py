#!/usr/bin/env python3
"""Shape gate over IntelGtProbe.cpp's blit composition.

No C++ suite can execute a line of IntelGtProbe.cpp -- it pulls in IOService,
which does not exist off macOS. So the only observer available for the shell is
a structural one: parse the source, assert the properties, and prove the
assertions can fail by mutating the source IN MEMORY. The live tree is never
written to.

WHY THIS MODULE EXISTS
----------------------
tests/test_intel_gt_blt.cpp drives IntelGtBlt.hpp hard and proves the 2D
command packets are composed correctly. It proves nothing whatsoever about
whether anything CALLS them. intel-handoff26 section 1 is that finding --
igSampleVerdict() carried 421 checks and had no callers, and every suite it had
stayed green -- and IntelGtRing.hpp sat in the identical position one session
ago. Delete composeBlit()'s body and all four gates stay green: clang has
nothing to say about a well-formed method nobody calls, and no host suite can
compile the translation unit to notice.

This module is that observer. Five properties, each with a control that reddens
it.

WHAT THIS DOES NOT ASSERT
-------------------------
That the composition is CORRECT. That is test_intel_gt_blt.cpp's job and it
already does it. This module asserts only that the shell reaches the header,
publishes both halves of what comes back, and still contains no MMIO write
path -- the last being the claim IntelGtProbe.hpp's banner makes in prose and
that property 5 makes falsifiable.

THE IDENTIFIER TRAP, INHERITED
------------------------------
test_intel_probe_ring_write_shape.py went red on correct code because
fRingTail is a PREFIX of fRingTailAfter and a bare substring test cannot tell
them apart. The same shape is live here twice over: fBlitSpan/fBlitSpanDwords
and fBlitDwords/fBlitVerdict share prefixes with keys and with each other. So
fields are matched as IDENTIFIERS via mentions(), never as substrings.
"""

import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUBJECT = os.path.join(ROOT, "phase0-kext", "IntelGtProbe.cpp")
SUBJECT_HDR = os.path.join(ROOT, "phase0-kext", "IntelGtProbe.hpp")

COMPOSER = "IntelGtProbe::composeBlit"
PUBLISHER = "IntelGtProbe::publishComposedBlit"
START = "IntelGtProbe::start"

WRITER_CALL = "igGtbWriteColorBlit("
SUCCESS_PRED = "igGtbBlitSucceeded("
NAME_FN = "igGtbBlitVerdictName("

VERDICT_KEY = '"IGGtBlitVerdict"'
NAME_KEY = '"IGGtBlitVerdictName"'
ATTEMPTED_KEY = '"IGGtBlitAttempted"'
DWORDS_KEY = '"IGGtBlitDwords"'
SUBMITTED_KEY = '"IGGtBlitSubmitted"'

VERDICT_FIELD = "fBlitVerdict"
DWORDS_FIELD = "fBlitDwords"
ATTEMPTED_FIELD = "fBlitAttempted"
SPAN_FIELD = "fBlitSpan"


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(source):
    """Blank comments, keep string CONTENT, preserve every byte offset.

    Offset preservation matters because every span this module computes is a
    slice index into the ORIGINAL text.

    Strings are kept because the setProperty key names are the subject of three
    of the five properties. Comments must go because this file's own comment
    block names every key it publishes and quotes the guard it wraps -- a gate
    reading prose would report a gutted publisher as correct, which is
    3050-handoff30 section 2a's finding.
    """
    out = list(source)
    i = 0
    n = len(source)
    while i < n:
        c = source[i]
        if c == '"':
            i += 1
            while i < n:
                if source[i] == "\\":
                    i += 2
                    continue
                if source[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and source[i + 1] == "/":
            while i < n and source[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if c == "/" and i + 1 < n and source[i + 1] == "*":
            while i < n and not (source[i] == "*" and i + 1 < n
                                 and source[i + 1] == "/"):
                if source[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                out[i + 1] = " "
                i += 2
            continue
        i += 1
    return "".join(out)


def mentions(text, field):
    """True when `field` occurs as a whole identifier.

    NOT a substring test. fBlitSpan is a prefix of nothing in the tree today,
    but fRingTail was a prefix of fRingTailAfter and that cost a red run on
    correct shipping code -- the ring module's own docstring records it. A
    trailing-character check is what separates the two.
    """
    return re.search(r"\b" + re.escape(field) + r"\b(?![A-Za-z0-9_])",
                     text) is not None


def method_span(source, qualified_name):
    """Byte span of one method body, found by a BALANCED BRACE WALK.

    Never a regex. handoff25 section 1c is the cost of the alternative: a
    character class excluding only the statement terminator runs straight past
    an if's closing paren, the newline and the whole guarded body, and lands on
    the NEXT statement's semicolon.
    """
    at = source.find(qualified_name)
    if at < 0:
        return None
    open_at = source.find("{", at)
    if open_at < 0:
        return None
    depth = 0
    i = open_at
    while i < len(source):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return (open_at, i + 1)
        i += 1
    return None


def guarded_span(source, predicate, start_at):
    """Byte span of the block guarded by `if (predicate...)`, brace-walked.

    Returns the body span only. The condition itself is deliberately excluded,
    because the verdict field appears INSIDE the condition and property 3 is
    about what the BODY contains.
    """
    at = source.find("if (" + predicate, start_at)
    if at < 0:
        return None
    paren = source.find("(", at)
    depth = 0
    i = paren
    while i < len(source):
        if source[i] == "(":
            depth += 1
        elif source[i] == ")":
            depth -= 1
            if depth == 0:
                break
        i += 1
    open_at = source.find("{", i)
    if open_at < 0:
        return None
    depth = 0
    j = open_at
    while j < len(source):
        if source[j] == "{":
            depth += 1
        elif source[j] == "}":
            depth -= 1
            if depth == 0:
                return (open_at, j + 1)
        j += 1
    return None


def audit(source):
    """Read the five properties out of one source string.

    Every missing anchor answers with a REFUSAL string rather than a falsy
    value, so a moved anchor fails LOUDLY. A gate that reports nothing when its
    subject moves is indistinguishable from a green one -- 3050-handoff30
    section 2b -- and that is the vacuity direction.
    """
    out = {}
    text = strip_comments(source)

    span = method_span(text, COMPOSER)
    if span is None:
        out["composer"] = "REFUSAL: %s not found" % COMPOSER
    else:
        out["composer"] = text[span[0]:span[1]]

    span = method_span(text, PUBLISHER)
    if span is None:
        out["publisher"] = "REFUSAL: %s not found" % PUBLISHER
    else:
        out["publisher"] = text[span[0]:span[1]]

    span = method_span(text, START)
    if span is None:
        out["start"] = "REFUSAL: %s not found" % START
    else:
        out["start"] = text[span[0]:span[1]]

    if isinstance(out["publisher"], str) and \
            out["publisher"].startswith("REFUSAL"):
        out["guarded"] = out["publisher"]
        out["unguarded"] = out["publisher"]
    else:
        body = out["publisher"]
        inner = guarded_span(body, SUCCESS_PRED, 0)
        if inner is None:
            out["guarded"] = "REFUSAL: no if(%s) block in %s" % (
                SUCCESS_PRED, PUBLISHER)
            out["unguarded"] = body
        else:
            out["guarded"] = body[inner[0]:inner[1]]
            out["unguarded"] = body[:inner[0]] + body[inner[1]:]

    out["whole"] = text
    return out


class BlitCompositionShapeTests(unittest.TestCase):
    """The wiring properties. Each has a control in the class below."""

    def setUp(self):
        self.source = read(SUBJECT)
        self.header = read(SUBJECT_HDR)
        self.a = audit(self.source)

    # -- property 1: the shell actually calls the header ------------------

    def test_the_composer_calls_the_2d_writer(self):
        body = self.a["composer"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertEqual(body.count(WRITER_CALL), 1,
                         "composeBlit() must call %s exactly once; "
                         "a header nothing calls is a header nothing runs"
                         % WRITER_CALL)

    def test_start_calls_the_composer_and_the_publisher(self):
        body = self.a["start"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn("composeBlit();", body,
                      "start() must call composeBlit()")
        self.assertIn("publishComposedBlit();", body,
                      "start() must call publishComposedBlit()")

    def test_the_composition_precedes_its_publication(self):
        body = self.a["start"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertLess(body.find("composeBlit();"),
                        body.find("publishComposedBlit();"),
                        "publishing before composing publishes the "
                        "initialiser, not the verdict")

    # -- property 2: the verdict is published, and named ------------------

    def test_the_verdict_and_its_name_are_both_published(self):
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn(VERDICT_KEY, body)
        self.assertIn(NAME_KEY, body)
        self.assertIn(NAME_FN, body,
                      "the name must come from the header's own switch, "
                      "never a string composed here")

    def test_the_attempted_flag_is_published_first(self):
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn(ATTEMPTED_KEY, body)
        self.assertLess(body.find(ATTEMPTED_KEY), body.find(VERDICT_KEY),
                        "the flag saying the composition ran must sort "
                        "above every value it qualifies")

    def test_the_verdict_is_published_outside_the_success_guard(self):
        """A verdict published only on success cannot report a failure."""
        outside = self.a["unguarded"]
        self.assertFalse(outside.startswith("REFUSAL"), outside)
        self.assertIn(VERDICT_KEY, outside)
        self.assertIn(NAME_KEY, outside)
        self.assertIn(ATTEMPTED_KEY, outside)

    # -- property 3: the dword count is guarded ---------------------------

    def test_the_dword_count_is_published_only_behind_success(self):
        inside = self.a["guarded"]
        outside = self.a["unguarded"]
        self.assertFalse(inside.startswith("REFUSAL"), inside)
        self.assertIn(DWORDS_KEY, inside,
                      "IGGtBlitDwords must sit behind %s" % SUCCESS_PRED)
        self.assertNotIn(DWORDS_KEY, outside,
                         "a refusal zeroes the count before inspecting any "
                         "argument, so an unguarded publication is "
                         "byte-identical to a measured zero")

    def test_the_guard_is_the_headers_own_predicate(self):
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn(SUCCESS_PRED + VERDICT_FIELD, body,
                      "admissibility must be the header's equality against "
                      "OK, never a negation of one error arm")

    # -- property 4: the count field is only ever set by the writer -------

    def test_the_dword_count_is_written_only_by_the_composer(self):
        """fBlitDwords must be assigned in init() and by the writer's out-param.

        A hand-computed count filed beside a composer's report is the
        fabricated-publication class: an operator cannot tell which one they
        are reading.
        """
        text = self.a["whole"]
        assigns = re.findall(r"\b" + DWORDS_FIELD + r"\b(?![A-Za-z0-9_])\s*=",
                             text)
        self.assertEqual(len(assigns), 1,
                         "fBlitDwords must have exactly one direct "
                         "assignment (the init() zero); the composer must "
                         "reach it only through the writer's out-param")
        self.assertTrue(mentions(self.a["composer"], "&" + DWORDS_FIELD)
                        or ("&" + DWORDS_FIELD) in self.a["composer"],
                        "the composer must pass &fBlitDwords to the writer")

    # -- property 5: still no MMIO write path -----------------------------

    def test_the_shell_still_has_no_mmio_write_path(self):
        """IntelGtProbe.hpp's banner claims this in prose. This makes it fail.

        Composing a drawing command and submitting one are different acts. The
        whole argument for this class is that it does only the first, and that
        claim is worth exactly what an observer can falsify.
        """
        text = self.a["whole"]
        self.assertNotIn("OSWriteLittleInt32", text)
        self.assertNotIn("OSWriteLittleInt64", text)
        self.assertNotIn("OSWriteBigInt32", text)
        self.assertIsNone(
            re.search(r"\*\s*\(\s*volatile\s+UInt(32|64)\s*\*\s*\)[^;]*=",
                      text),
            "a volatile store through the BAR mapping is a submission path; "
            "IGGtBlitSubmitted is published false and must stay true to that")

    def test_the_blit_is_published_as_unsubmitted(self):
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn(SUBMITTED_KEY, body)
        self.assertIn('setProperty("IGGtBlitSubmitted", false)', body)

    # -- the span is sized from the header, not a literal ------------------

    def test_the_span_length_is_composed_not_spelled(self):
        """intel-handoff35 section 2a: two spellings of one length drift."""
        self.assertIn("#define IG_GTP_BLIT_SPAN_DWORDS", self.header)
        line = [l for l in self.header.splitlines()
                if l.startswith("#define IG_GTP_BLIT_SPAN_DWORDS")]
        self.assertEqual(len(line), 1)
        self.assertIn("IG_GTB_XY_COLOR_BLT_DWORDS_POLICY_7", line[0],
                      "the span must be sized from IntelGtBlt.hpp's own "
                      "packet length, never a second literal 7")


class NegativeControlTests(unittest.TestCase):
    """Each control mutates the source IN MEMORY and asserts WHICH property
    breaks. A control that reddens a property other than its own is not
    isolating anything -- intel-handoff26 section 3 is that reading.
    """

    def setUp(self):
        self.source = read(SUBJECT)

    def test_control_renaming_the_writer_call_breaks_property_one(self):
        mutated = self.source.replace(WRITER_CALL, "igGtbNotTheWriter(")
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertEqual(a["composer"].count(WRITER_CALL), 0)

    def test_control_deleting_the_start_call_breaks_property_one(self):
        mutated = self.source.replace("    composeBlit();\n", "", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertNotIn("composeBlit();", a["start"])

    def test_control_publishing_before_composing_breaks_the_order(self):
        mutated = self.source.replace("    composeBlit();\n", "", 1)
        mutated = mutated.replace(
            "    publishComposedBlit();",
            "    publishComposedBlit();\n    composeBlit();", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertGreater(a["start"].find("composeBlit();"),
                           a["start"].find("publishComposedBlit();"))

    def test_control_unguarding_the_count_breaks_property_three(self):
        mutated = self.source.replace(
            "    if (igGtbBlitSucceeded(fBlitVerdict)) {", "    if (true) {", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertNotIn(SUCCESS_PRED + VERDICT_FIELD, a["publisher"])

    def test_control_collapsing_the_name_key_breaks_property_two(self):
        mutated = self.source.replace(NAME_KEY, VERDICT_KEY, 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertNotIn(NAME_KEY, a["publisher"])

    def test_control_a_hand_written_count_breaks_property_four(self):
        mutated = self.source.replace(
            "    if (!igGtbBlitSucceeded(fBlitVerdict)) {",
            "    fBlitDwords = 7;\n"
            "    if (!igGtbBlitSucceeded(fBlitVerdict)) {", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        text = audit(mutated)["whole"]
        assigns = re.findall(r"\b" + DWORDS_FIELD + r"\b(?![A-Za-z0-9_])\s*=",
                             text)
        self.assertEqual(len(assigns), 2)

    def test_control_a_volatile_store_breaks_property_five(self):
        mutated = self.source.replace(
            "void IntelGtProbe::publishComposedBlit(void)",
            "void IntelGtProbe::submitBlit(UInt32 o, UInt32 v)\n"
            "{\n"
            "    *(volatile UInt32 *) (fBar0Base + o) = v;\n"
            "}\n"
            "\n"
            "void IntelGtProbe::publishComposedBlit(void)", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        text = audit(mutated)["whole"]
        self.assertIsNotNone(
            re.search(r"\*\s*\(\s*volatile\s+UInt(32|64)\s*\*\s*\)[^;]*=",
                      text))

    def test_control_a_moved_anchor_refuses_loudly(self):
        """A gate reporting nothing when its subject moves reads as green."""
        mutated = self.source.replace(COMPOSER, "IntelGtProbe::composeSomething")
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertTrue(a["composer"].startswith("REFUSAL"), a["composer"])

    def test_control_the_stripper_removes_prose_but_keeps_keys(self):
        """Comment stripping is mandatory here, not stylistic.

        The publisher's own comment block names IGGtBlitDwords and quotes the
        guard it sits behind, so a prose-reading gate would report a gutted
        publisher as correct.
        """
        sample = ('// IGGtBlitDwords is published behind the guard\n'
                  'setProperty("IGGtBlitDwords", x);\n')
        stripped = strip_comments(sample)
        self.assertEqual(len(stripped), len(sample),
                         "the stripper must preserve every byte offset")
        self.assertEqual(stripped.count("IGGtBlitDwords"), 1,
                         "the comment mention must be blanked and the key kept")


if __name__ == "__main__":
    unittest.main()
