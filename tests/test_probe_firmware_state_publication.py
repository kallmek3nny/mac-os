#!/usr/bin/env python3
"""Shape gate over NV3050Probe.cpp's publishFirmwareState().

Why this exists
---------------
``3050-handoff43.md`` fixed a live defect here: ``NVUsableFramebufferMB`` was
published unconditionally, so a refused read of NV_USABLE_FB_SIZE_IN_MB landed
in the IORegistry as a framebuffer size. ``setProperty()`` with a fabricated
word is byte-identical to ``setProperty()`` with a measured one -- handoff18
section 1d's class, and the eighth occurrence of it.

The fix is one line::

    if (fbVerdict == NV_FB_SIZE_OK)
        setProperty("NVUsableFramebufferMB", (UInt64)fbSizeMB, 32);

and handoff43 section 7 item 2 is that **nothing can see that guard**. Delete
the ``if`` and all four gates stay green, for the four by-construction reasons
this family keeps re-deriving:

  * the register gate checks ``#define`` constants, and the guard is not one;
  * no host C++ suite can compile ``NV3050Probe.cpp`` -- it reaches IOService,
    which does not exist off macOS, so the translation unit is out of reach of
    every suite in tests/ by construction;
  * clang has nothing to say about a well-formed ``setProperty()`` call that
    happens to be unguarded; ``-Werror`` sees no defect;
  * ``nvFbSizeVerdict()`` itself is covered in tests/test_decode.cpp, which is
    the intel-handoff26 section 1 shape -- a heavily tested pure function whose
    caller can ignore its answer and no suite notices.

What is pinned
--------------
1. ``NVUsableFramebufferVerdict`` is published, unconditionally, and is the
   name function's output rather than a hand-spelled string.
2. ``NVUsableFramebufferMeasured`` is published unconditionally and is an
   EQUALITY against NV_FB_SIZE_OK.
3. ``NVUsableFramebufferMB`` is published exactly once, and that publication is
   GUARDED.
4. The guard is an equality against NV_FB_SIZE_OK -- never a negation of one
   error arm, never a magnitude test on the size. The comment in the subject
   explains why, and this property is what stops a later edit from taking the
   tempting form: ``fbVerdict != NV_FB_SIZE_UNREADABLE`` is TRUE for both
   UNPUBLISHED and IMPLAUSIBLE, and ``fbSizeMB > 0`` was true for the refused
   read that caused the original defect.
5. The verdict and the measured flag both sort ABOVE the value they qualify --
   an ABSENT key is a question rather than an answer, so a reader who sees no
   MB key must find the reason beside it rather than infer it.
6. Every enumerator in ``NVFbSizeVerdict`` that the accepting arm is compared
   against is DERIVED from NVDecode.hpp, not spelled here. A renamed accepting
   verdict reddens on the commit that renames it.

Scanning is over COMMENT-STRIPPED source, and that is mandatory rather than
stylistic: the subject's own comment block spells
``fbVerdict != NV_FB_SIZE_UNREADABLE`` and ``fbSizeMB > 0`` as the forms it
refuses, so a prose-reading gate would find the exact tokens this module exists
to forbid and would report the defect on correct code. A test below asserts the
stripper actually removes them.

Refusing rather than passing
----------------------------
If the subject method, the enum, or the name function cannot be found, the
audit returns a REFUSAL string rather than an empty finding list. A gate that
reports nothing when its anchor moves is indistinguishable from a green one --
see tests/test_probe_gsp_boot_publication.py section 2b for the same reasoning.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHELL = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
DECODE = os.path.join(ROOT, "phase0-kext", "NVDecode.hpp")

SUBJECT = "publishFirmwareState"

VERDICT_KEY = "NVUsableFramebufferVerdict"
MEASURED_KEY = "NVUsableFramebufferMeasured"
VALUE_KEY = "NVUsableFramebufferMB"

NAME_FN = "nvFbSizeVerdictName"
ACCEPTING = "NV_FB_SIZE_OK"
ENUM_TYPE = "NVFbSizeVerdict"

#
# The GFW half, added because pollGfwBoot() returned a bare bool whose false had
# TWO preimages -- "this offset is not decoding", a defect in this driver's own
# BAR0 map, and "GFW decoded but never reached 0xFF", a fact about the card. The
# sharper half is the out-param: the unreadable arm wrote *outProgress = 0 before
# returning, and a literal 0 is byte-identical in the IORegistry to a card whose
# GFW genuinely sits at step 0.
#
# So the progress word is now published ONLY behind nvGfwBootProgressWasRead(),
# and this gate is what makes that guard load-bearing: delete it and the C++
# suites, the register gate and clang all stay green, because a deleted `if`
# around a well-formed setProperty() is not a warning.
#
# The readable predicate is deliberately NOT the accepting one. TIMEOUT is a
# FAILURE whose progress word is a real measurement, so an implementation that
# reused nvGfwBootVerdictIsOk() would discard the most useful number an operator
# can have here -- and the two spellings differ on exactly that one input.
#
GFW_VERDICT_KEY = "NVGfwBootVerdict"
GFW_READ_KEY = "NVGfwBootProgressRead"
GFW_OK_KEY = "NVGfwBootOk"
GFW_VALUE_KEY = "NVGfwBootProgress"

GFW_NAME_FN = "nvGfwBootVerdictName"
GFW_OK_FN = "nvGfwBootVerdictIsOk"
GFW_READ_FN = "nvGfwBootProgressWasRead"
GFW_ENUM_TYPE = "NVGfwBootVerdict"
GFW_NOT_ATTEMPTED = "NV_GFW_BOOT_NOT_ATTEMPTED"


def strip_comments(text):
    """Remove // and /* */ comments without disturbing line structure.

    String literals are preserved -- the property KEYS are string literals and
    every property in this module is identified by one.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def method_body(source, name):
    """Return the balanced-brace body of NV3050Probe::<name>, or None.

    A balanced walk rather than a regex: the body contains braces, and a
    non-greedy match to the first '}' stops inside the first nested block.
    """
    anchor = re.search(r"NV3050Probe::" + re.escape(name) + r"\s*\(", source)
    if not anchor:
        return None
    i = source.find("{", anchor.end())
    if i < 0:
        return None
    depth = 0
    start = i
    while i < len(source):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:i]
        i += 1
    return None


def set_property_calls(body):
    """Every setProperty("KEY", ...) in order, as (key, offset)."""
    return [(m.group(1), m.start())
            for m in re.finditer(r'setProperty\s*\(\s*"([^"]+)"', body)]


def enclosing_depth(body, offset):
    """Brace depth at `offset`, relative to the method body's own top level.

    A statement at the method's top level is depth 0. A statement inside an
    `if` block is depth 1. Bareness is decided by this rather than by reading
    the preceding line, because a guard can sit on its own line above the call
    or share it.
    """
    return body.count("{", 0, offset) - body.count("}", 0, offset)


def guard_controlling(body, offset):
    """The condition text of the `if` controlling the statement at `offset`.

    Handles both the braceless single-statement form the subject uses and a
    braced block. Returns None when the statement is bare.

    Walks BACKWARDS from the statement to the previous statement boundary,
    then requires that what precedes it is an `if (...)`. The condition is
    captured by a balanced-paren walk, never a negated character class --
    handoff25 section 1c is the cost of the latter: a class excluding only `;`
    runs past the if's closing paren and lands on the next statement.
    """
    head = body[:offset]

    # An enclosing braced block: find the '{' that opened our depth.
    if enclosing_depth(body, offset) > 0:
        depth = 0
        i = offset - 1
        while i >= 0:
            if body[i] == "}":
                depth += 1
            elif body[i] == "{":
                if depth == 0:
                    head = body[:i]
                    break
                depth -= 1
            i -= 1

    # Now `head` ends where the controlling construct would sit.
    tail = head.rstrip()
    if not tail.endswith(")"):
        return None

    depth = 0
    i = len(tail) - 1
    while i >= 0:
        if tail[i] == ")":
            depth += 1
        elif tail[i] == "(":
            depth -= 1
            if depth == 0:
                before = tail[:i].rstrip()
                if re.search(r"\bif$", before):
                    return tail[i + 1:len(tail) - 1].strip()
                return None
        i -= 1
    return None


def enum_members(decode_src):
    """Every enumerator of NVFbSizeVerdict, derived from the header."""
    m = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*" + re.escape(ENUM_TYPE),
                  decode_src, re.S)
    if not m:
        return []
    return re.findall(r"\b(NV_FB_SIZE_[A-Z_]+)\b", m.group(1))


def gfw_enum_members(decode_src):
    """Every enumerator of NVGfwBootVerdict, derived from the header.

    Derived rather than spelled here, for the reason
    tests/test_probe_gsp_boot_publication.py section 2 gives: a set written into
    the test cannot notice a fifth verdict added to the header, while a derived
    one reddens on the commit that adds it.
    """
    m = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*" + re.escape(GFW_ENUM_TYPE),
                  decode_src, re.S)
    if not m:
        return []
    return re.findall(r"\b(NV_GFW_BOOT_[A-Z_]+)\b", m.group(1))


def audit():
    """Return a list of findings. Empty list means the shape is correct.

    A REFUSAL entry means the audit could not locate its own subject, which is
    a failure of the gate rather than a pass.
    """
    findings = []

    with open(SHELL, "r", encoding="utf-8") as fh:
        raw = fh.read()
    with open(DECODE, "r", encoding="utf-8") as fh:
        decode_raw = fh.read()

    src = strip_comments(raw)
    decode = strip_comments(decode_raw)

    body = method_body(src, SUBJECT)
    if body is None:
        return ["REFUSAL: cannot find NV3050Probe::%s() in %s"
                % (SUBJECT, os.path.basename(SHELL))]

    members = enum_members(decode)
    if not members:
        return ["REFUSAL: cannot derive %s enumerators from %s"
                % (ENUM_TYPE, os.path.basename(DECODE))]
    if ACCEPTING not in members:
        return ["REFUSAL: accepting verdict %s is not an enumerator of %s"
                % (ACCEPTING, ENUM_TYPE)]

    if not re.search(r"\b" + re.escape(NAME_FN) + r"\s*\(", decode):
        return ["REFUSAL: cannot find %s() in %s"
                % (NAME_FN, os.path.basename(DECODE))]

    calls = set_property_calls(body)
    keys = [k for k, _ in calls]
    at = {}
    for k, off in calls:
        at.setdefault(k, []).append(off)

    # Property 1 -- the verdict is published, unconditionally, through the
    # name function rather than a hand-spelled string.
    if VERDICT_KEY not in at:
        findings.append("%s is never published" % VERDICT_KEY)
    else:
        if len(at[VERDICT_KEY]) != 1:
            findings.append("%s is published %d times, expected once"
                            % (VERDICT_KEY, len(at[VERDICT_KEY])))
        off = at[VERDICT_KEY][0]
        if guard_controlling(body, off) is not None:
            findings.append("%s is GUARDED -- it must be published on every "
                            "path, because an absent key is a question rather "
                            "than an answer" % VERDICT_KEY)
        stmt = body[off:body.find(";", off)]
        if NAME_FN not in stmt:
            findings.append("%s does not publish %s()'s output -- a "
                            "hand-spelled string cannot follow the enum"
                            % (VERDICT_KEY, NAME_FN))

    # Property 2 -- the measured flag is unconditional and is an EQUALITY.
    if MEASURED_KEY not in at:
        findings.append("%s is never published" % MEASURED_KEY)
    else:
        off = at[MEASURED_KEY][0]
        if guard_controlling(body, off) is not None:
            findings.append("%s is GUARDED -- it must be published on every "
                            "path" % MEASURED_KEY)
        stmt = body[off:body.find(";", off)]
        if not re.search(r"==\s*" + re.escape(ACCEPTING), stmt):
            findings.append("%s is not an equality against %s -- a negation "
                            "of one error arm is TRUE for the others"
                            % (MEASURED_KEY, ACCEPTING))

    # Properties 3 and 4 -- the value is published exactly once and GUARDED,
    # and the guard is an equality against the accepting verdict.
    if VALUE_KEY not in at:
        findings.append("%s is never published" % VALUE_KEY)
    else:
        if len(at[VALUE_KEY]) != 1:
            findings.append("%s is published %d times, expected once -- a "
                            "second unguarded copy restores the defect"
                            % (VALUE_KEY, len(at[VALUE_KEY])))
        off = at[VALUE_KEY][0]
        guard = guard_controlling(body, off)
        if guard is None:
            findings.append("%s is published UNGUARDED -- a refused read "
                            "lands in the IORegistry as a framebuffer size"
                            % VALUE_KEY)
        else:
            if not re.search(r"==\s*" + re.escape(ACCEPTING), guard):
                findings.append(
                    "%s's guard `%s` is not an equality against %s"
                    % (VALUE_KEY, guard, ACCEPTING))
            if "!=" in guard:
                findings.append(
                    "%s's guard `%s` negates an error arm -- true for every "
                    "OTHER refusal" % (VALUE_KEY, guard))
            if re.search(r"fbSizeMB\s*[<>]", guard):
                findings.append(
                    "%s's guard `%s` is a magnitude test on the size -- a "
                    "refused read masked to 4294967295 passes it"
                    % (VALUE_KEY, guard))

    # Property 5 -- the qualifiers sort above the value they qualify.
    if VERDICT_KEY in at and VALUE_KEY in at:
        if keys.index(VERDICT_KEY) > keys.index(VALUE_KEY):
            findings.append("%s is published AFTER %s"
                            % (VERDICT_KEY, VALUE_KEY))
    if MEASURED_KEY in at and VALUE_KEY in at:
        if keys.index(MEASURED_KEY) > keys.index(VALUE_KEY):
            findings.append("%s is published AFTER %s"
                            % (MEASURED_KEY, VALUE_KEY))

    findings.extend(audit_gfw(body, decode, calls, keys, at))

    return findings


def audit_gfw(body, decode, calls, keys, at):
    """The GFW half of the same subject. Same helpers, same body, same idiom.

    Kept as a function over an already-parsed body rather than a second module,
    because a second module would duplicate the audit() every member of this
    family deliberately shares with the live tree -- the reasoning in
    3050-handoff31.md section 3 for extending rather than adding.
    """
    findings = []

    members = gfw_enum_members(decode)
    if not members:
        return ["REFUSAL: cannot derive %s enumerators from %s"
                % (GFW_ENUM_TYPE, os.path.basename(DECODE))]
    if GFW_NOT_ATTEMPTED not in members:
        return ["REFUSAL: %s is not an enumerator of %s"
                % (GFW_NOT_ATTEMPTED, GFW_ENUM_TYPE)]

    for fn in (GFW_NAME_FN, GFW_OK_FN, GFW_READ_FN):
        if not re.search(r"\b" + re.escape(fn) + r"\s*\(", decode):
            return ["REFUSAL: cannot find %s() in %s"
                    % (fn, os.path.basename(DECODE))]

    # Property 6 -- the verdict is published, unconditionally, through the name
    # function. Same reasoning as property 1 one register over.
    if GFW_VERDICT_KEY not in at:
        findings.append("%s is never published" % GFW_VERDICT_KEY)
    else:
        if len(at[GFW_VERDICT_KEY]) != 1:
            findings.append("%s is published %d times, expected once"
                            % (GFW_VERDICT_KEY, len(at[GFW_VERDICT_KEY])))
        off = at[GFW_VERDICT_KEY][0]
        if guard_controlling(body, off) is not None:
            findings.append("%s is GUARDED -- it must be published on every "
                            "path, because an absent key is a question rather "
                            "than an answer" % GFW_VERDICT_KEY)
        stmt = body[off:body.find(";", off)]
        if GFW_NAME_FN not in stmt:
            findings.append("%s does not publish %s()'s output -- a "
                            "hand-spelled string cannot follow the enum"
                            % (GFW_VERDICT_KEY, GFW_NAME_FN))

    # Property 7 -- the readable flag is unconditional and is the READ predicate,
    # never the accepting one. This is the load-bearing distinction: TIMEOUT is a
    # FAILURE whose progress word is a real measurement, so publishing
    # nvGfwBootVerdictIsOk() here would report a measured word as unmeasured.
    if GFW_READ_KEY not in at:
        findings.append("%s is never published" % GFW_READ_KEY)
    else:
        off = at[GFW_READ_KEY][0]
        if guard_controlling(body, off) is not None:
            findings.append("%s is GUARDED -- it must be published on every "
                            "path" % GFW_READ_KEY)
        stmt = body[off:body.find(";", off)]
        if GFW_READ_FN not in stmt:
            findings.append("%s does not publish %s()'s output -- %s() is a "
                            "DIFFERENT partition and reports TIMEOUT's real "
                            "measurement as unmeasured"
                            % (GFW_READ_KEY, GFW_READ_FN, GFW_OK_FN))

    # Property 8 -- the health bool is unconditional and is the ACCEPTING
    # predicate. Here the polarity is the other way round: publishing the read
    # predicate would call a timed-out card healthy.
    if GFW_OK_KEY not in at:
        findings.append("%s is never published" % GFW_OK_KEY)
    else:
        off = at[GFW_OK_KEY][0]
        if guard_controlling(body, off) is not None:
            findings.append("%s is GUARDED -- it must be published on every "
                            "path" % GFW_OK_KEY)
        stmt = body[off:body.find(";", off)]
        if GFW_OK_FN not in stmt:
            findings.append("%s does not publish %s()'s output -- %s() is TRUE "
                            "for TIMEOUT and would call a card that never "
                            "finished booting healthy"
                            % (GFW_OK_KEY, GFW_OK_FN, GFW_READ_FN))

    # Properties 9 and 10 -- the progress word is published exactly once and
    # GUARDED, and the guard is the READ predicate. An unguarded copy publishes
    # the caller's initialiser 0 on the UNREADABLE arm, which is byte-identical
    # in the IORegistry to a card whose GFW genuinely sits at step 0.
    if GFW_VALUE_KEY not in at:
        findings.append("%s is never published" % GFW_VALUE_KEY)
    else:
        if len(at[GFW_VALUE_KEY]) != 1:
            findings.append("%s is published %d times, expected once -- a "
                            "second unguarded copy restores the defect"
                            % (GFW_VALUE_KEY, len(at[GFW_VALUE_KEY])))
        off = at[GFW_VALUE_KEY][0]
        guard = guard_controlling(body, off)
        if guard is None:
            findings.append("%s is published UNGUARDED -- on the UNREADABLE arm "
                            "pollGfwBoot() writes nothing, so this is the "
                            "caller's initialiser 0 and is byte-identical to a "
                            "card sitting at step 0" % GFW_VALUE_KEY)
        else:
            if GFW_READ_FN not in guard:
                findings.append("%s's guard `%s` does not consult %s()"
                                % (GFW_VALUE_KEY, guard, GFW_READ_FN))
            if GFW_OK_FN in guard:
                findings.append("%s's guard `%s` consults %s() -- that DISCARDS "
                                "the timeout progress word, which is a real "
                                "measurement and the most useful number here"
                                % (GFW_VALUE_KEY, guard, GFW_OK_FN))
            if "!" in guard.replace("!=", ""):
                findings.append("%s's guard `%s` is a NEGATION -- the guard must "
                                "be the positive read predicate"
                                % (GFW_VALUE_KEY, guard))

    # Property 11 -- the two GFW predicates are not the same call. If somebody
    # spells both keys with one function the pair collapses, and the collapse is
    # invisible to every other property here.
    if GFW_READ_KEY in at and GFW_OK_KEY in at:
        read_stmt = body[at[GFW_READ_KEY][0]:body.find(";", at[GFW_READ_KEY][0])]
        ok_stmt = body[at[GFW_OK_KEY][0]:body.find(";", at[GFW_OK_KEY][0])]
        read_fns = set(re.findall(r"\bnvGfwBoot[A-Za-z]*\s*\(", read_stmt))
        ok_fns = set(re.findall(r"\bnvGfwBoot[A-Za-z]*\s*\(", ok_stmt))
        if read_fns and read_fns == ok_fns:
            findings.append("%s and %s are published through the SAME "
                            "predicate -- they partition the verdicts "
                            "differently and TIMEOUT is the separating input"
                            % (GFW_READ_KEY, GFW_OK_KEY))

    # Property 12 -- the qualifiers sort above the value they qualify.
    for qualifier in (GFW_VERDICT_KEY, GFW_READ_KEY):
        if qualifier in at and GFW_VALUE_KEY in at:
            if keys.index(qualifier) > keys.index(GFW_VALUE_KEY):
                findings.append("%s is published AFTER %s"
                                % (qualifier, GFW_VALUE_KEY))

    return findings


class LiveTreeTests(unittest.TestCase):
    """The positive control: the shipping tree must satisfy every property."""

    def test_live_tree_is_clean(self):
        findings = audit()
        self.assertEqual([], findings,
                         "publishFirmwareState() shape defects: %s" % findings)

    def test_subject_is_findable(self):
        with open(SHELL, "r", encoding="utf-8") as fh:
            src = strip_comments(fh.read())
        self.assertIsNotNone(method_body(src, SUBJECT))

    def test_accepting_verdict_is_derived_not_spelled(self):
        with open(DECODE, "r", encoding="utf-8") as fh:
            decode = strip_comments(fh.read())
        members = enum_members(decode)
        self.assertIn(ACCEPTING, members)
        self.assertIn("NV_FB_SIZE_NOT_ATTEMPTED", members)
        self.assertGreaterEqual(len(members), 4, members)

    def test_not_attempted_is_the_zero_value(self):
        # The caller leaves fbVerdict at its initialiser on the !gfwOk path,
        # so the zero value must be a refusal rather than an accept.
        with open(DECODE, "r", encoding="utf-8") as fh:
            decode = strip_comments(fh.read())
        self.assertNotEqual(ACCEPTING, enum_members(decode)[0])


class StripperTests(unittest.TestCase):
    """The stripper must remove the forms the subject's comment spells."""

    def test_comment_forms_are_removed(self):
        with open(SHELL, "r", encoding="utf-8") as fh:
            raw = fh.read()
        self.assertIn("!= NV_FB_SIZE_UNREADABLE", raw,
                      "the subject's comment no longer spells the refused "
                      "form; this test's premise has moved")
        body = method_body(strip_comments(raw), SUBJECT)
        self.assertNotIn("NV_FB_SIZE_UNREADABLE", body)
        self.assertNotIn("fbSizeMB > 0", body)

    def test_string_literals_survive(self):
        stripped = strip_comments('setProperty("A/*B*/C", x); // gone')
        self.assertIn('"A/*B*/C"', stripped)
        self.assertNotIn("gone", stripped)

    def test_block_comment_removed(self):
        self.assertNotIn("x", strip_comments("/* x */"))


class GuardDetectionTests(unittest.TestCase):
    """guard_controlling() must not report a guard that is not there."""

    def test_bare_call_has_no_guard(self):
        body = '\n    setProperty("K", v);\n'
        off = set_property_calls(body)[0][1]
        self.assertIsNone(guard_controlling(body, off))

    def test_braceless_guard_is_found(self):
        body = '\n    if (v == NV_FB_SIZE_OK)\n        setProperty("K", v);\n'
        off = set_property_calls(body)[0][1]
        self.assertEqual("v == NV_FB_SIZE_OK", guard_controlling(body, off))

    def test_braced_guard_is_found(self):
        body = '\n    if (v == NV_FB_SIZE_OK) {\n        setProperty("K", v);\n    }\n'
        off = set_property_calls(body)[0][1]
        self.assertEqual("v == NV_FB_SIZE_OK", guard_controlling(body, off))

    def test_preceding_call_is_not_a_guard(self):
        # handoff25 section 1c: a statement after another statement that ends
        # in ')' must not read as guarded.
        body = '\n    other(a);\n    setProperty("K", v);\n'
        off = set_property_calls(body)[0][1]
        self.assertIsNone(guard_controlling(body, off))

    def test_nested_condition_is_captured_whole(self):
        body = '\n    if (f(a) == NV_FB_SIZE_OK)\n        setProperty("K", v);\n'
        off = set_property_calls(body)[0][1]
        self.assertEqual("f(a) == NV_FB_SIZE_OK",
                         guard_controlling(body, off))


class NegativeControlTests(unittest.TestCase):
    """Each control mutates the SUBJECT and asserts WHICH property broke.

    Every control runs against a synthetic body rather than the live file, so
    the tree is never mutated. The properties under test are implemented as
    module-level functions over a body string, which is what makes that
    possible -- and is why audit() and these controls share the same helpers.
    """

    GOOD = '\n'.join([
        '    setProperty("NVGfwBootOk", gfwOk);',
        '    setProperty("NVUsableFramebufferVerdict", nvFbSizeVerdictName(fbVerdict));',
        '    setProperty("NVUsableFramebufferMeasured", fbVerdict == NV_FB_SIZE_OK);',
        '    if (fbVerdict == NV_FB_SIZE_OK)',
        '        setProperty("NVUsableFramebufferMB", (UInt64)fbSizeMB, 32);',
        '',
    ])

    def properties(self, body):
        """Re-run the audit's property checks against a synthetic body."""
        calls = set_property_calls(body)
        keys = [k for k, _ in calls]
        at = {}
        for k, off in calls:
            at.setdefault(k, []).append(off)
        broken = []

        if VALUE_KEY in at:
            off = at[VALUE_KEY][0]
            guard = guard_controlling(body, off)
            if guard is None:
                broken.append("value-unguarded")
            else:
                if not re.search(r"==\s*" + re.escape(ACCEPTING), guard):
                    broken.append("guard-not-equality")
                if "!=" in guard:
                    broken.append("guard-negates")
                if re.search(r"fbSizeMB\s*[<>]", guard):
                    broken.append("guard-magnitude")
            if len(at[VALUE_KEY]) != 1:
                broken.append("value-duplicated")
        else:
            broken.append("value-missing")

        if VERDICT_KEY not in at:
            broken.append("verdict-missing")
        else:
            if guard_controlling(body, at[VERDICT_KEY][0]) is not None:
                broken.append("verdict-guarded")
            stmt = body[at[VERDICT_KEY][0]:body.find(";", at[VERDICT_KEY][0])]
            if NAME_FN not in stmt:
                broken.append("verdict-hand-spelled")

        if MEASURED_KEY not in at:
            broken.append("measured-missing")
        elif guard_controlling(body, at[MEASURED_KEY][0]) is not None:
            broken.append("measured-guarded")

        if VERDICT_KEY in at and VALUE_KEY in at:
            if keys.index(VERDICT_KEY) > keys.index(VALUE_KEY):
                broken.append("verdict-sorts-below")
        if MEASURED_KEY in at and VALUE_KEY in at:
            if keys.index(MEASURED_KEY) > keys.index(VALUE_KEY):
                broken.append("measured-sorts-below")

        return broken

    def test_positive_control(self):
        self.assertEqual([], self.properties(self.GOOD))

    def test_deleting_the_guard_is_caught(self):
        # This is handoff43's own defect, restored.
        bad = self.GOOD.replace(
            '    if (fbVerdict == NV_FB_SIZE_OK)\n', '')
        self.assertEqual(["value-unguarded"], self.properties(bad))

    def test_negated_error_arm_guard_is_caught(self):
        bad = self.GOOD.replace("fbVerdict == NV_FB_SIZE_OK)\n",
                                "fbVerdict != NV_FB_SIZE_UNREADABLE)\n")
        broken = self.properties(bad)
        self.assertIn("guard-not-equality", broken)
        self.assertIn("guard-negates", broken)

    def test_magnitude_guard_is_caught(self):
        bad = self.GOOD.replace("fbVerdict == NV_FB_SIZE_OK)\n",
                                "fbSizeMB > 0)\n")
        broken = self.properties(bad)
        self.assertIn("guard-magnitude", broken)
        self.assertIn("guard-not-equality", broken)

    def test_second_unguarded_copy_is_caught(self):
        bad = self.GOOD + \
            '    setProperty("NVUsableFramebufferMB", (UInt64)fbSizeMB, 32);\n'
        self.assertIn("value-duplicated", self.properties(bad))

    def test_guarding_the_verdict_is_caught(self):
        bad = self.GOOD.replace(
            '    setProperty("NVUsableFramebufferVerdict", nvFbSizeVerdictName(fbVerdict));',
            '    if (fbVerdict == NV_FB_SIZE_OK)\n'
            '        setProperty("NVUsableFramebufferVerdict", nvFbSizeVerdictName(fbVerdict));')
        self.assertEqual(["verdict-guarded"], self.properties(bad))

    def test_guarding_the_measured_flag_is_caught(self):
        bad = self.GOOD.replace(
            '    setProperty("NVUsableFramebufferMeasured", fbVerdict == NV_FB_SIZE_OK);',
            '    if (gfwOk)\n'
            '        setProperty("NVUsableFramebufferMeasured", fbVerdict == NV_FB_SIZE_OK);')
        self.assertEqual(["measured-guarded"], self.properties(bad))

    def test_hand_spelled_verdict_string_is_caught(self):
        bad = self.GOOD.replace("nvFbSizeVerdictName(fbVerdict)", '"ok"')
        self.assertEqual(["verdict-hand-spelled"], self.properties(bad))

    def test_deleting_the_verdict_is_caught(self):
        bad = self.GOOD.replace(
            '    setProperty("NVUsableFramebufferVerdict", nvFbSizeVerdictName(fbVerdict));\n',
            '')
        self.assertEqual(["verdict-missing"], self.properties(bad))

    def test_deleting_the_measured_flag_is_caught(self):
        bad = self.GOOD.replace(
            '    setProperty("NVUsableFramebufferMeasured", fbVerdict == NV_FB_SIZE_OK);\n',
            '')
        self.assertEqual(["measured-missing"], self.properties(bad))

    def test_verdict_sorting_below_the_value_is_caught(self):
        bad = '\n'.join([
            '    if (fbVerdict == NV_FB_SIZE_OK)',
            '        setProperty("NVUsableFramebufferMB", (UInt64)fbSizeMB, 32);',
            '    setProperty("NVUsableFramebufferVerdict", nvFbSizeVerdictName(fbVerdict));',
            '    setProperty("NVUsableFramebufferMeasured", fbVerdict == NV_FB_SIZE_OK);',
            '',
        ])
        broken = self.properties(bad)
        self.assertIn("verdict-sorts-below", broken)
        self.assertIn("measured-sorts-below", broken)


class GfwNegativeControlTests(unittest.TestCase):
    """Controls for the GFW half. Same shape as NegativeControlTests above.

    Every control runs against a synthetic body, so the tree is never mutated.
    These are all-new properties, so 3050-handoff18.md section 1e's
    every-pre-existing-check-green rule does NOT apply -- what the controls
    establish instead is that each property reddens on its OWN mutation and that
    the positive control is clean, which is the strongest statement available for
    a module with no prior checks over its subject.
    """

    GOOD = '\n'.join([
        '    setProperty("NVGfwBootVerdict", nvGfwBootVerdictName(gfwVerdict));',
        '    setProperty("NVGfwBootProgressRead", nvGfwBootProgressWasRead(gfwVerdict));',
        '    setProperty("NVGfwBootOk", nvGfwBootVerdictIsOk(gfwVerdict));',
        '    if (nvGfwBootProgressWasRead(gfwVerdict))',
        '        setProperty("NVGfwBootProgress", (UInt64)gfwProgress, 32);',
        '',
    ])

    DECODE = '\n'.join([
        'typedef enum {',
        '    NV_GFW_BOOT_NOT_ATTEMPTED = 0,',
        '    NV_GFW_BOOT_OK,',
        '    NV_GFW_BOOT_UNREADABLE,',
        '    NV_GFW_BOOT_TIMEOUT,',
        '    NV_GFW_BOOT_VERDICT_LIMIT',
        '} NVGfwBootVerdict;',
        'static inline const char *nvGfwBootVerdictName(NVGfwBootVerdict v) { return ""; }',
        'static inline bool nvGfwBootVerdictIsOk(NVGfwBootVerdict v) { return false; }',
        'static inline bool nvGfwBootProgressWasRead(NVGfwBootVerdict v) { return false; }',
        '',
    ])

    def findings(self, body, decode=None):
        """Run the REAL audit_gfw() against a synthetic body.

        Deliberately the live function rather than a re-implementation: a
        re-implemented copy of the properties would drift from the one the gate
        actually runs, and then every control here would be measuring the copy.
        """
        calls = set_property_calls(body)
        keys = [k for k, _ in calls]
        at = {}
        for k, off in calls:
            at.setdefault(k, []).append(off)
        return audit_gfw(body, self.DECODE if decode is None else decode,
                         calls, keys, at)

    def test_positive_control(self):
        self.assertEqual([], self.findings(self.GOOD))

    def test_deleting_the_progress_guard_is_caught(self):
        # The defect this session fixed, restored: the progress word published
        # on every path, so the UNREADABLE arm ships the initialiser 0.
        bad = self.GOOD.replace(
            '    if (nvGfwBootProgressWasRead(gfwVerdict))\n', '')
        found = self.findings(bad)
        self.assertEqual(1, len(found), found)
        self.assertIn("UNGUARDED", found[0])

    def test_guarding_the_progress_word_with_isok_is_caught(self):
        # The tempting wrong fix: it discards TIMEOUT's progress word, which is
        # a real measurement and the single most useful number on that arm.
        bad = self.GOOD.replace(
            '    if (nvGfwBootProgressWasRead(gfwVerdict))',
            '    if (nvGfwBootVerdictIsOk(gfwVerdict))')
        found = self.findings(bad)
        self.assertTrue(any("does not consult" in f for f in found), found)
        self.assertTrue(any("DISCARDS" in f for f in found), found)

    def test_negated_guard_is_caught(self):
        bad = self.GOOD.replace(
            'if (nvGfwBootProgressWasRead(gfwVerdict))',
            'if (!nvGfwBootProgressWasRead(gfwVerdict))')
        found = self.findings(bad)
        self.assertTrue(any("NEGATION" in f for f in found), found)

    def test_second_unguarded_copy_is_caught(self):
        bad = self.GOOD + \
            '    setProperty("NVGfwBootProgress", (UInt64)gfwProgress, 32);\n'
        found = self.findings(bad)
        self.assertTrue(any("expected once" in f for f in found), found)

    def test_guarding_the_verdict_is_caught(self):
        bad = self.GOOD.replace(
            '    setProperty("NVGfwBootVerdict", nvGfwBootVerdictName(gfwVerdict));',
            '    if (nvGfwBootVerdictIsOk(gfwVerdict))\n'
            '        setProperty("NVGfwBootVerdict", nvGfwBootVerdictName(gfwVerdict));')
        found = self.findings(bad)
        self.assertEqual(1, len(found), found)
        self.assertIn("GUARDED", found[0])

    def test_hand_spelled_verdict_string_is_caught(self):
        bad = self.GOOD.replace('nvGfwBootVerdictName(gfwVerdict)', '"completed"')
        found = self.findings(bad)
        self.assertEqual(1, len(found), found)
        self.assertIn("hand-spelled", found[0])

    def test_deleting_the_readable_flag_is_caught(self):
        bad = self.GOOD.replace(
            '    setProperty("NVGfwBootProgressRead", nvGfwBootProgressWasRead(gfwVerdict));\n',
            '')
        found = self.findings(bad)
        self.assertEqual(1, len(found), found)
        self.assertIn("never published", found[0])

    def test_readable_flag_published_through_isok_is_caught(self):
        # The polarity error one way round: it reports TIMEOUT's real measurement
        # as unmeasured, so the publisher would suppress a word it did read.
        bad = self.GOOD.replace(
            'setProperty("NVGfwBootProgressRead", nvGfwBootProgressWasRead(gfwVerdict))',
            'setProperty("NVGfwBootProgressRead", nvGfwBootVerdictIsOk(gfwVerdict))')
        found = self.findings(bad)
        self.assertTrue(any("DIFFERENT partition" in f for f in found), found)

    def test_health_bool_published_through_wasread_is_caught(self):
        # The polarity error the OTHER way round, and the dangerous direction: it
        # calls a card that never finished booting healthy.
        bad = self.GOOD.replace(
            'setProperty("NVGfwBootOk", nvGfwBootVerdictIsOk(gfwVerdict))',
            'setProperty("NVGfwBootOk", nvGfwBootProgressWasRead(gfwVerdict))')
        found = self.findings(bad)
        self.assertTrue(any("never finished booting healthy" in f for f in found),
                        found)

    def test_collapsing_both_predicates_is_caught(self):
        # Both qualifier keys spelled with ONE predicate. Property 11 exists for
        # this alone -- the collapse is invisible to every other property, since
        # each key individually still publishes a nvGfwBoot* call.
        bad = self.GOOD.replace(
            'setProperty("NVGfwBootOk", nvGfwBootVerdictIsOk(gfwVerdict))',
            'setProperty("NVGfwBootOk", nvGfwBootProgressWasRead(gfwVerdict))')
        found = self.findings(bad)
        self.assertTrue(any("SAME " in f for f in found), found)

    def test_verdict_sorting_below_the_value_is_caught(self):
        bad = '\n'.join([
            '    if (nvGfwBootProgressWasRead(gfwVerdict))',
            '        setProperty("NVGfwBootProgress", (UInt64)gfwProgress, 32);',
            '    setProperty("NVGfwBootVerdict", nvGfwBootVerdictName(gfwVerdict));',
            '    setProperty("NVGfwBootProgressRead", nvGfwBootProgressWasRead(gfwVerdict));',
            '    setProperty("NVGfwBootOk", nvGfwBootVerdictIsOk(gfwVerdict));',
            '',
        ])
        found = self.findings(bad)
        self.assertTrue(any("published AFTER" in f for f in found), found)

    def test_missing_enum_refuses(self):
        # A gate reporting NOTHING when its anchor moves is indistinguishable
        # from a green one, so the absence of the enum must REFUSE.
        found = self.findings(self.GOOD, decode="/* no enum here */")
        self.assertEqual(1, len(found), found)
        self.assertIn("REFUSAL", found[0])

    def test_missing_predicate_refuses(self):
        decode = self.DECODE.replace(
            'static inline bool nvGfwBootProgressWasRead(NVGfwBootVerdict v) { return false; }\n',
            '')
        found = self.findings(self.GOOD, decode=decode)
        self.assertEqual(1, len(found), found)
        self.assertIn("REFUSAL", found[0])


class RefusalTests(unittest.TestCase):
    """The audit must REFUSE rather than pass when its anchors move."""

    def test_missing_subject_refuses(self):
        src = strip_comments("void NV3050Probe::somethingElse() { }")
        self.assertIsNone(method_body(src, SUBJECT))

    def test_enumless_header_yields_no_members(self):
        self.assertEqual([], enum_members("typedef enum { A } Unrelated;"))

    def test_renamed_subject_is_not_matched_by_prefix(self):
        # A substring test would pass a suffixed rename; the anchor requires
        # the open paren, so publishFirmwareStateV2 does not match.
        src = strip_comments("void NV3050Probe::publishFirmwareStateV2(int a) { }")
        self.assertIsNone(method_body(src, SUBJECT))


if __name__ == "__main__":
    unittest.main(verbosity=2)
