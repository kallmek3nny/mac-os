#!/usr/bin/env python3
"""Shape gate over IntelGtProbe.cpp's submission REHEARSAL.

No C++ suite can execute a line of IntelGtProbe.cpp -- it pulls in IOService,
which does not exist off macOS. So the only observer available for the shell is
a structural one: parse the source, assert the properties, and prove the
assertions can fail by mutating the source IN MEMORY. The live tree is never
written to.

WHY THIS MODULE EXISTS
----------------------
tests/test_intel_gt_exec.cpp drives IntelGtExec.hpp hard and proves the
context descriptor, the GTT entry and igGtxSubmitReady() are composed
correctly. It proves nothing whatsoever about whether anything CALLS them.
intel-handoff26 section 1 is that finding -- igSampleVerdict() carried 421
checks and had no callers, and every suite it had stayed green. Delete
rehearseSubmit()'s body and all five gates stay green: clang has nothing to say
about a well-formed method nobody calls, and no host suite can compile the
translation unit to notice.

This module is that observer, in the idiom the three siblings established
(test_intel_probe_mmio_path.py, test_intel_probe_ring_write_shape.py,
test_intel_probe_blit_shape.py): one audit() shared with the live tree,
comment-stripped source, negative controls each asserting WHICH property broke.

THE PROPERTY THIS ONE HAS AND THE SIBLINGS DO NOT
-------------------------------------------------
The blit gate asserts there is no MMIO WRITE path. This one asserts something
strictly narrower and, for this subject, sharper: that the two execlist
registers which would SUBMIT -- the submit queue at 0x510 and EXECLIST_CONTROL
at 0x550 -- are never even ADDRESSED. igGtxExeclistSqReg() and
igGtxExeclistControlReg() exist in IntelGtExec.hpp and are tested there; a
shell that composed either offset would be one store away from submission, and
IGGtSubmitted is published false. Only the STATUS register may be read, and
property 5 makes that falsifiable rather than a claim in a comment.

WHAT THIS DOES NOT ASSERT
-------------------------
That the composition is CORRECT. That is tests/test_intel_gt_exec.cpp's job.
This module asserts only that the shell reaches the header, publishes the
verdict on every arm, and still cannot submit.

THE IDENTIFIER TRAP, INHERITED
------------------------------
test_intel_probe_ring_write_shape.py went red on correct code because
fRingTail is a PREFIX of fRingTailAfter and a bare substring test cannot tell
them apart. The same shape is live here: fSubmitRehearsed and
fSubmitReadyVerdict share a prefix with each other, and the key
"IGGtSubmitReadyVerdict" is a PREFIX of "IGGtSubmitReadyVerdictName". Both are
matched as whole tokens -- fields via mentions(), keys with their closing quote
attached -- never as bare substrings.
"""

import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUBJECT = os.path.join(ROOT, "phase0-kext", "IntelGtProbe.cpp")
SUBJECT_HDR = os.path.join(ROOT, "phase0-kext", "IntelGtProbe.hpp")
EXEC_HDR = os.path.join(ROOT, "phase0-kext", "IntelGtExec.hpp")

REHEARSER = "IntelGtProbe::rehearseSubmit"
PUBLISHER = "IntelGtProbe::publishSubmitRehearsal"
INIT = "IntelGtProbe::init"
START = "IntelGtProbe::start"

STATUS_REG_FN = "igGtxExeclistStatusReg("
SQ_REG_FN = "igGtxExeclistSqReg("
CONTROL_REG_FN = "igGtxExeclistControlReg("
DESC_FN = "igGtxContextDescriptor("
READY_FN = "igGtxSubmitReady("
ADMISSIBLE_FN = "igGtxSubmitAdmissible("
NAME_FN = "igGtxReadyVerdictName("
DESC_VALID_FN = "igGtxContextDescriptorValid("

# Keys carry their closing quote so IGGtSubmitReadyVerdict cannot match inside
# IGGtSubmitReadyVerdictName -- the prefix trap this file's docstring records.
REHEARSED_KEY = '"IGGtSubmitRehearsed"'
VERDICT_KEY = '"IGGtSubmitReadyVerdict"'
VERDICT_NAME_KEY = '"IGGtSubmitReadyVerdictName"'
STATUS_KEY = '"IGGtExeclistStatus"'
DESCRIPTOR_KEY = '"IGGtCtxDescriptor"'
DESC_VALID_KEY = '"IGGtCtxDescriptorValid"'
LRCA_KEY = '"IGGtCtxLrca"'
SWID_KEY = '"IGGtCtxSwId"'
SUBMITTED_KEY = '"IGGtSubmitted"'
WRITTEN_KEY = '"IGGtExeclistWritten"'

VERDICT_FIELD = "fSubmitReadyVerdict"
REHEARSED_FIELD = "fSubmitRehearsed"
STATUS_FIELD = "fExeclistStatus"
DESCRIPTOR_FIELD = "fCtxDescriptor"

LRCA_CONST = "IG_GTP_LRCA_POLICY"
SWID_CONST = "IG_GTP_CTX_SWID_POLICY"


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(source):
    """Blank comments, keep string CONTENT, preserve every byte offset.

    Offset preservation matters because every span this module computes is a
    slice index into the ORIGINAL text.

    Strings are kept because the setProperty key names are the subject of four
    of the six properties. Comments must go because this file's own comment
    block names every key it publishes, names both registers it refuses to
    touch, and quotes the guard it wraps -- a gate reading prose would report a
    gutted publisher as correct, which is 3050-handoff30 section 2a's finding.
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

    NOT a substring test. fRingTail was a prefix of fRingTailAfter and that
    cost a red run on correct shipping code -- the ring module's own docstring
    records it. fSubmitRehearsed and fSubmitReadyVerdict are the live pair here.
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


def call_args(text, call):
    """The argument text of the first `call`, by a balanced PAREN walk.

    A regex up to the first `)` would stop inside igGtxExeclistStatusReg(...)
    when that call is nested in another, and igGtxSubmitReady()'s own argument
    list spans seven lines. Returns None when the call is absent.
    """
    at = text.find(call)
    if at < 0:
        return None
    open_at = at + len(call) - 1
    depth = 0
    i = open_at
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_at + 1:i]
        i += 1
    return None


def audit(source):
    """Read the six properties out of one source string.

    Every missing anchor answers with a REFUSAL string rather than a falsy
    value, so a moved anchor fails LOUDLY. A gate that reports nothing when its
    subject moves is indistinguishable from a green one -- 3050-handoff30
    section 2b -- and that is the vacuity direction.
    """
    out = {}
    text = strip_comments(source)

    for key, anchor in (("rehearser", REHEARSER),
                        ("publisher", PUBLISHER),
                        ("init", INIT),
                        ("start", START)):
        span = method_span(text, anchor)
        if span is None:
            out[key] = "REFUSAL: %s not found" % anchor
        else:
            out[key] = text[span[0]:span[1]]

    out["whole"] = text
    return out


class SubmitRehearsalShapeTests(unittest.TestCase):
    """The wiring properties. Each has a control in the class below."""

    def setUp(self):
        self.source = read(SUBJECT)
        self.header = read(SUBJECT_HDR)
        self.a = audit(self.source)

    # -- property 1: the shell actually calls the header -------------------

    def test_the_rehearser_calls_the_three_header_functions(self):
        body = self.a["rehearser"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        for fn in (DESC_FN, READY_FN):
            self.assertEqual(body.count(fn), 1,
                             "rehearseSubmit() must call %s exactly once; "
                             "a header nothing calls is a header nothing runs"
                             % fn)

    def test_start_calls_the_rehearser_and_the_publisher(self):
        body = self.a["start"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn("rehearseSubmit();", body,
                      "start() must call rehearseSubmit()")
        self.assertIn("publishSubmitRehearsal();", body,
                      "start() must call publishSubmitRehearsal()")

    def test_the_rehearsal_precedes_its_publication(self):
        body = self.a["start"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertLess(body.find("rehearseSubmit();"),
                        body.find("publishSubmitRehearsal();"),
                        "publishing before rehearsing publishes the "
                        "initialiser, not the verdict")

    # -- property 2: the verdict is published, named, and unguarded --------

    def test_the_verdict_and_its_name_are_both_published(self):
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn(VERDICT_KEY, body)
        self.assertIn(VERDICT_NAME_KEY, body)
        self.assertIn(NAME_FN, body,
                      "the name must come from the header's own switch, "
                      "never a string composed here")

    def test_the_rehearsed_flag_is_published_first(self):
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn(REHEARSED_KEY, body)
        self.assertLess(body.find(REHEARSED_KEY), body.find(VERDICT_KEY),
                        "the flag saying the rehearsal ran must sort above "
                        "every value it qualifies")

    def test_the_publisher_has_no_conditional_at_all(self):
        """Every key on every arm, so an absence cannot be a question.

        The blit publisher guards its dword count deliberately, because a
        refusal ZEROES that count and a zero is indistinguishable from a
        measured zero. Nothing here has that shape: the verdict family is
        contiguous and never-attempted is a NAMED member of it, and the
        descriptor's refusal is a self-describing sentinel. So the correct
        structure is no guard whatsoever, and this asserts it directly rather
        than testing each key for unguardedness one at a time.
        """
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIsNone(re.search(r"\bif\s*\(", body),
                          "publishSubmitRehearsal() must publish every key "
                          "unconditionally; an absent key is a question, not "
                          "an answer")

    def test_every_published_key_is_present(self):
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        for key in (REHEARSED_KEY, VERDICT_KEY, VERDICT_NAME_KEY, STATUS_KEY,
                    DESCRIPTOR_KEY, DESC_VALID_KEY, LRCA_KEY, SWID_KEY,
                    SUBMITTED_KEY, WRITTEN_KEY):
            self.assertIn(key, body, "%s must be published" % key)

    # -- property 3: admissibility is the header's own equality ------------

    def test_the_guard_is_the_headers_own_predicate(self):
        body = self.a["rehearser"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn(ADMISSIBLE_FN + VERDICT_FIELD, body,
                      "admissibility must be the header's equality against "
                      "OK, never a negation of one error arm -- a negation is "
                      "true for every refusal added after it was written")

    def test_the_descriptor_validity_comes_from_the_header(self):
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn(DESC_VALID_FN + DESCRIPTOR_FIELD, body,
                      "validity must be the header's valid-bit test, never a "
                      "comparison against zero composed here")

    # -- property 4: the join judges what the descriptor was composed from -

    def test_the_join_and_the_descriptor_share_their_context(self):
        """A verdict over a different LRCA describes a page nothing composed.

        The rehearser's own comment makes this claim in prose. This makes it
        fail: both calls' argument lists are walked by balanced parens and the
        two policy constants must appear as whole identifiers in each.
        """
        body = self.a["rehearser"]
        self.assertFalse(body.startswith("REFUSAL"), body)

        desc_args = call_args(body, DESC_FN)
        ready_args = call_args(body, READY_FN)
        self.assertIsNotNone(desc_args, "no %s call in rehearseSubmit()" % DESC_FN)
        self.assertIsNotNone(ready_args, "no %s call in rehearseSubmit()" % READY_FN)

        for const in (LRCA_CONST, SWID_CONST):
            self.assertTrue(mentions(desc_args, const),
                            "%s must compose from %s" % (DESC_FN, const))
            self.assertTrue(mentions(ready_args, const),
                            "%s must judge the same %s" % (READY_FN, const))

    def test_the_join_is_handed_the_sampled_status_word(self):
        body = self.a["rehearser"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        ready_args = call_args(body, READY_FN)
        self.assertIsNotNone(ready_args)
        self.assertTrue(mentions(ready_args, STATUS_FIELD),
                        "the port-busy arm is only reachable if the sampled "
                        "status word is what the join is handed")

    def test_the_status_word_is_sampled_before_the_join(self):
        body = self.a["rehearser"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertLess(body.find(STATUS_REG_FN), body.find(READY_FN),
                        "judging before sampling judges the initialiser")

    # -- property 5: the two SUBMITTING registers are never addressed ------

    def test_the_shell_never_addresses_the_submit_queue(self):
        """IntelGtProbe.hpp's banner claims this in prose. This makes it fail.

        Reading a status word and submitting a context are different acts. The
        submit queue at 0x510 and EXECLIST_CONTROL at 0x550 are the two
        registers that would perform the second, and this class must not even
        COMPOSE their offsets -- a composed offset is one store from a
        submission, and IGGtSubmitted is published false.
        """
        text = self.a["whole"]
        self.assertNotIn(SQ_REG_FN, text)
        self.assertNotIn(CONTROL_REG_FN, text)
        self.assertNotIn("IG_GTX_EXECLIST_SQ_POLICY", text)
        self.assertNotIn("IG_GTX_EXECLIST_CONTROL_POLICY", text)
        self.assertNotIn("IG_GTX_EXECLIST_LOAD_BIT_POLICY", text)

    def test_the_shell_still_has_no_mmio_write_path(self):
        text = self.a["whole"]
        self.assertNotIn("OSWriteLittleInt32", text)
        self.assertNotIn("OSWriteLittleInt64", text)
        self.assertNotIn("OSWriteBigInt32", text)
        self.assertIsNone(
            re.search(r"\*\s*\(\s*volatile\s+UInt(32|64)\s*\*\s*\)[^;]*=",
                      text),
            "a volatile store through the BAR mapping is a submission path; "
            "IGGtSubmitted is published false and must stay true to that")

    def test_both_standing_denials_are_published_false(self):
        body = self.a["publisher"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn('setProperty("IGGtSubmitted", false)', body)
        self.assertIn('setProperty("IGGtExeclistWritten", false)', body)

    # -- property 6: the three members are initialised, not assumed --------

    def test_the_three_members_are_initialised_in_init(self):
        """An unset member published as a verdict is a fabricated measurement.

        NOT_ATTEMPTED is a NAMED member of the family precisely so this state
        is reportable; it is worth nothing if init() never puts it there.
        """
        body = self.a["init"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        for field, value in ((VERDICT_FIELD, "IG_GTX_READY_NOT_ATTEMPTED"),
                             (REHEARSED_FIELD, "false"),
                             (DESCRIPTOR_FIELD, "IG_GTX_CTXDESC_INVALID_POLICY_0"),
                             (STATUS_FIELD, "IG_MMIO_INVALID")):
            found = re.search(r"\b" + re.escape(field) +
                              r"\b(?![A-Za-z0-9_])\s*=\s*([A-Za-z0-9_]+)\s*;",
                              body)
            self.assertIsNotNone(found, "%s must be initialised in init()" % field)
            self.assertEqual(found.group(1), value,
                             "%s must initialise to %s" % (field, value))

    def test_the_rehearsed_flag_is_set_by_the_rehearser(self):
        body = self.a["rehearser"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertIn(REHEARSED_FIELD + " = true;", body,
                      "the flag must be set by the method it describes")

    def test_the_verdict_field_is_written_only_by_the_join(self):
        """fSubmitReadyVerdict must be assigned in init() and by the join.

        A hand-computed verdict filed beside a header's report is the
        fabricated-publication class: an operator cannot tell which one they
        are reading.
        """
        text = self.a["whole"]
        assigns = re.findall(r"\b" + VERDICT_FIELD + r"\b(?![A-Za-z0-9_])\s*=",
                             text)
        self.assertEqual(len(assigns), 2,
                         "fSubmitReadyVerdict must have exactly two "
                         "assignments: the init() sentinel and the join's "
                         "return")
        self.assertIn(VERDICT_FIELD + " = " + READY_FN, self.a["rehearser"],
                      "the second assignment must be the header's join")

    # -- the context id is sized from the header, not spelled twice --------

    def test_the_context_id_is_a_named_policy_constant(self):
        """intel-handoff35 section 2a: two spellings of one value drift."""
        line = [l for l in self.header.splitlines()
                if l.startswith("#define " + SWID_CONST)]
        self.assertEqual(len(line), 1,
                         "%s must be defined exactly once" % SWID_CONST)
        body = self.a["rehearser"]
        self.assertFalse(body.startswith("REFUSAL"), body)
        self.assertEqual(body.count(SWID_CONST), 2,
                         "the rehearser must spell the context id by NAME in "
                         "both the compose and the judge, never as a literal")


class NegativeControlTests(unittest.TestCase):
    """Each control mutates the source IN MEMORY and asserts WHICH property
    breaks. A control that reddens a property other than its own is not
    isolating anything -- intel-handoff26 section 3 is that reading.
    """

    def setUp(self):
        self.source = read(SUBJECT)

    def test_control_renaming_the_join_breaks_property_one(self):
        mutated = self.source.replace(READY_FN, "igGtxNotTheJoin(")
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertEqual(a["rehearser"].count(READY_FN), 0)

    def test_control_deleting_the_start_call_breaks_property_one(self):
        mutated = self.source.replace("    rehearseSubmit();\n", "", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertNotIn("rehearseSubmit();", a["start"])

    def test_control_guarding_the_verdict_breaks_property_two(self):
        """A verdict published only on success cannot report a failure."""
        mutated = self.source.replace(
            '    setProperty("IGGtSubmitReadyVerdict",     (UInt64) fSubmitReadyVerdict, 32);',
            '    if (igGtxSubmitAdmissible(fSubmitReadyVerdict)) {\n'
            '        setProperty("IGGtSubmitReadyVerdict", (UInt64) fSubmitReadyVerdict, 32);\n'
            '    }', 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertIsNotNone(re.search(r"\bif\s*\(", a["publisher"]))

    def test_control_collapsing_the_name_key_breaks_property_two(self):
        """The prefix trap: the bare key must not match inside the Name key."""
        mutated = self.source.replace(VERDICT_NAME_KEY, VERDICT_KEY, 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertNotIn(VERDICT_NAME_KEY, a["publisher"])
        self.assertIn(VERDICT_KEY, a["publisher"],
                      "the surviving bare key must still be found; a control "
                      "that reddens both is not isolating the collapse")

    def test_control_negating_one_error_arm_breaks_property_three(self):
        mutated = self.source.replace(
            "    if (!igGtxSubmitAdmissible(fSubmitReadyVerdict)) {",
            "    if (fSubmitReadyVerdict != IG_GTX_READY_PORT_BUSY) {", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertNotIn(ADMISSIBLE_FN + VERDICT_FIELD, a["rehearser"])

    def test_control_judging_a_different_lrca_breaks_property_four(self):
        """The sharpest one: a verdict about a page nothing composed.

        The mutation is inside the JOIN's argument list only, so the descriptor
        still composes from the policy LRCA and only the shared-context claim
        reddens.
        """
        mutated = self.source.replace(
            "    fSubmitReadyVerdict = igGtxSubmitReady(IG_GTP_ENGINE_RENDER,\n"
            "                                           IG_GTP_LRCA_POLICY,",
            "    fSubmitReadyVerdict = igGtxSubmitReady(IG_GTP_ENGINE_RENDER,\n"
            "                                           0x2000ull,", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        ready_args = call_args(a["rehearser"], READY_FN)
        desc_args = call_args(a["rehearser"], DESC_FN)
        self.assertFalse(mentions(ready_args, LRCA_CONST))
        self.assertTrue(mentions(desc_args, LRCA_CONST),
                        "the composer must be untouched; a control reddening "
                        "both halves does not isolate the disagreement")

    def test_control_a_constant_status_breaks_property_four(self):
        mutated = self.source.replace(
            "                                           fExeclistStatus);",
            "                                           0u);", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        ready_args = call_args(a["rehearser"], READY_FN)
        self.assertFalse(mentions(ready_args, STATUS_FIELD))

    def test_control_addressing_the_submit_queue_breaks_property_five(self):
        mutated = self.source.replace(
            "    fExeclistStatus = regRead32(igGtxExeclistStatusReg(IG_GTP_ENGINE_RENDER));",
            "    fExeclistStatus = regRead32(igGtxExeclistStatusReg(IG_GTP_ENGINE_RENDER));\n"
            "    UInt32 sq = igGtxExeclistSqReg(IG_GTP_ENGINE_RENDER);\n"
            "    IGLOG(\"sq %08x\", (unsigned) sq);", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        text = audit(mutated)["whole"]
        self.assertIn(SQ_REG_FN, text)

    def test_control_a_volatile_store_breaks_property_five(self):
        mutated = self.source.replace(
            "void IntelGtProbe::publishSubmitRehearsal(void)",
            "void IntelGtProbe::submitContext(UInt32 o, UInt32 v)\n"
            "{\n"
            "    *(volatile UInt32 *) (fBar0Base + o) = v;\n"
            "}\n"
            "\n"
            "void IntelGtProbe::publishSubmitRehearsal(void)", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        text = audit(mutated)["whole"]
        self.assertIsNotNone(
            re.search(r"\*\s*\(\s*volatile\s+UInt(32|64)\s*\*\s*\)[^;]*=",
                      text))

    def test_control_a_hand_written_verdict_breaks_property_six(self):
        mutated = self.source.replace(
            "    if (!igGtxSubmitAdmissible(fSubmitReadyVerdict)) {",
            "    fSubmitReadyVerdict = IG_GTX_READY_OK;\n"
            "    if (!igGtxSubmitAdmissible(fSubmitReadyVerdict)) {", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        text = audit(mutated)["whole"]
        assigns = re.findall(r"\b" + VERDICT_FIELD + r"\b(?![A-Za-z0-9_])\s*=",
                             text)
        self.assertEqual(len(assigns), 3)

    def test_control_an_uninitialised_verdict_breaks_property_six(self):
        mutated = self.source.replace(
            "    fSubmitReadyVerdict = IG_GTX_READY_NOT_ATTEMPTED;\n", "", 1)
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertIsNone(
            re.search(r"\b" + VERDICT_FIELD + r"\b(?![A-Za-z0-9_])\s*=\s*"
                      r"([A-Za-z0-9_]+)\s*;", a["init"]))

    def test_control_a_moved_anchor_refuses_loudly(self):
        """A gate reporting nothing when its subject moves reads as green."""
        mutated = self.source.replace(REHEARSER, "IntelGtProbe::rehearseSomething")
        self.assertNotEqual(mutated, self.source, "mutation matched nothing")
        a = audit(mutated)
        self.assertTrue(a["rehearser"].startswith("REFUSAL"), a["rehearser"])

    def test_control_the_stripper_removes_prose_but_keeps_keys(self):
        """Comment stripping is mandatory here, not stylistic.

        The rehearser's own comment block names both registers this module
        forbids and quotes the guard it wraps, so a prose-reading gate would
        report a gutted rehearsal as correct.
        """
        sample = ('// igGtxExeclistSqReg is never called from this file\n'
                  'setProperty("IGGtSubmitted", false);\n')
        stripped = strip_comments(sample)
        self.assertEqual(len(stripped), len(sample),
                         "the stripper must preserve every byte offset")
        self.assertEqual(stripped.count("igGtxExeclistSqReg"), 0,
                         "the comment mention must be blanked")
        self.assertEqual(stripped.count("IGGtSubmitted"), 1,
                         "the published key must survive")


if __name__ == "__main__":
    unittest.main()
