#!/usr/bin/env python3
"""Shape gate over decodeBoot0()'s log arms -- the sibling of the result-switch gate.

Why this exists
---------------
``3050-handoff33.md`` section 1 moved the four-way part decision out of the
IOKit shell and into ``nvPartVerdict()`` in ``phase0-kext/NVDecode.hpp``, and
taught ``publishIdentity()`` to publish it. What it did NOT do is require the
four LOG arms in ``decodeBoot0()`` to keep agreeing with the four verdicts.

Delete the Ada arm's ``NVLOG`` and every gate stays green:

  * ``tools/check-registers.py`` parses ``#define``s; these are enum members
    and log text.
  * no host suite compiles ``NV3050Probe.cpp`` -- it reaches IOService, which
    does not exist off macOS, so the arms are reachable only by parse.
  * clang has nothing to say about a deleted ``if`` around a well-formed call.
  * ``tests/test_probe_identity_publication.py`` is about the PUBLICATION side.
    It says nothing about the sentence an operator reads in the log.

The Ada arm is the one of the four with an operator action attached: an
'RTX 3050 A' is Ada silicon under a 30-series name and the whole plan
retargets. Losing that sentence turns a retarget into a puzzling boot.

What this deliberately does NOT assert
--------------------------------------
``decodeBoot0()`` has SIX log arms and ``NVPartVerdict`` has FOUR members. The
two extra arms are the dead-read cases -- ``0xFFFFFFFF`` (device not
responding) and ``0x00000000`` (mapping points at nothing) -- and neither has a
verdict counterpart, because ``nvPartVerdict()`` decodes a word that already
read back. A gate demanding a verdict per log arm would redden correct shipping
code. The mapping is therefore ONE-DIRECTIONAL: every verdict must have an arm;
an arm without a verdict is fine.

That direction is still load-bearing, because the verdict set is DERIVED from
the header's own ``typedef enum`` rather than spelled here -- a fifth verdict
added with no log arm reddens on the commit that adds it, naming the member.
"""

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP_PATH = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
HPP_PATH = os.path.join(ROOT, "phase0-kext", "NVDecode.hpp")

#
# Full signatures, never a prefix. "void NV3050Probe::publishIdentity" is a
# PREFIX of publishIdentityUnreadable's signature, so a bare-name anchor finds
# whichever comes first in the file and a vacuity control written that way
# passes without ever running -- measured in 3050-handoff39.md section 9d.
#
SUBJECT_FN = "void NV3050Probe::decodeBoot0(UInt32 value)"
PUBLISHER_FN = "void NV3050Probe::publishIdentity(UInt32 boot0, UInt32 boot42)"
VERDICT_FN = "static inline NVPartVerdict nvPartVerdict(NVU32 boot0)"
VERDICT_NAME_FN = "static inline const char *nvPartVerdictName(NVPartVerdict v)"

VERDICT_ENUM_TAG = "NVPartVerdict"
VERDICT_LIMIT = "NV_PART_VERDICT_LIMIT"

RETARGET_KEY = "NVPartRetargetNote"
ADA_GUARD = "part == NV_PART_ADA"

#
# Each verdict is paired with the condition that selects its log arm and with
# markers that must appear in THAT arm and in no other. Markers are short
# lowercase substrings of the MESSAGE, never a whole wrapped line: anchoring to
# one particular wrap makes the assertion a property of the typography rather
# than of the sentence, so re-flowing a line to fit a column limit would redden
# a gate that has nothing to say about the change.
#
ARM_CONDITIONS = {
    "NV_PART_OTHER_ARCHITECTURE": "if (arch != NV_ARCHITECTURE_GA100) {",
    "NV_PART_ADA":                "if (arch == NV_ARCHITECTURE_AD100) {",
    "NV_PART_TARGET":             "if (impl == NV_IMPLEMENTATION_GA107) {",
}

#
# NV_PART_AMPERE_OTHER has no condition of its own -- it is the else of the
# GA107 test, which is correct: "Ampere but not GA107" is the complement of
# "GA107" within the Ampere branch, and giving it its own `if` would create an
# arm reachable only by a condition the compiler already knows is false.
#
ELSE_OF = {"NV_PART_AMPERE_OTHER": "NV_PART_TARGET"}

ARM_MARKERS = {
    "NV_PART_OTHER_ARCHITECTURE": ("architecture is not ampere",
                                   "not landing where you think"),
    "NV_PART_ADA":                ("this is an ada part",
                                   "the whole plan retargets"),
    "NV_PART_TARGET":             ("mobile rtx 3050 die",
                                   "phase 3 milestone"),
    "NV_PART_AMPERE_OTHER":       ("expected ga107",),
}


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """Remove // and /* */ comments.

    Mandatory rather than stylistic. decodeBoot0()'s own neighbours discuss the
    Ada case and the retarget in prose, and NVDecode.hpp's header block above
    nvPartVerdict() names all four findings in words -- so a gate reading prose
    would report an arm as present on a function whose NVLOG was deleted.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            out.append(text[i:j])
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def join_wrapped_literals(text):
    """Concatenate adjacent string literals split across a line break.

    A marker straddling the wrap is ABSENT from raw source that plainly
    contains the sentence, because the compiler is what joins them. Only a
    close-quote at end of line against an open-quote at start of the next is
    joined; a comma or paren between them is left alone.
    """
    return re.sub(r'"[ \t]*\n[ \t]*"', "", text)


def brace_span(text, open_index):
    """Balanced walk from the '{' at open_index. Returns (body_start, body_end)."""
    depth = 0
    i = open_index
    n = len(text)
    while i < n:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return open_index + 1, i
        i += 1
    return None


def function_body(text, signature):
    at = text.find(signature)
    if at < 0:
        return None
    open_index = text.find("{", at + len(signature))
    if open_index < 0:
        return None
    span = brace_span(text, open_index)
    if span is None:
        return None
    return text[span[0]:span[1]]


def if_arm(body, condition):
    """Return (start, end, else_start, else_end) for the `if` opening `condition`.

    Offsets are into `body`. The else members are None when there is no else.
    """
    at = body.find(condition)
    if at < 0:
        return None
    open_index = body.find("{", at)
    if open_index < 0:
        return None
    span = brace_span(body, open_index)
    if span is None:
        return None
    start, end = span

    tail = body[end + 1:]
    match = re.match(r"\s*else\s*\{", tail)
    if match is None:
        return start, end, None, None
    else_open = end + 1 + match.end() - 1
    else_span = brace_span(body, else_open)
    if else_span is None:
        return start, end, None, None
    return start, end, else_span[0], else_span[1]


def header_verdict_members(hpp_raw):
    """Derive the verdict set from NVDecode.hpp's own typedef enum.

    Spelling the four names here would make a fifth verdict invisible to this
    gate: the log arm it lacks would never be looked for.
    """
    hpp = strip_comments(hpp_raw)
    close = hpp.find("} " + VERDICT_ENUM_TAG + ";")
    if close < 0:
        return None
    open_at = hpp.rfind("typedef enum {", 0, close)
    if open_at < 0:
        return None
    block = hpp[open_at:close]
    members = re.findall(r"\b(NV_PART_\w+)\b", block)
    return frozenset(m for m in members if m != VERDICT_LIMIT)


def arm_texts(cpp_raw):
    """Map verdict -> the log text of its arm, with nested arms subtracted.

    The Ada arm sits INSIDE the not-Ampere arm, so its text must be removed
    from the enclosing one or the two are not disjoint by construction and the
    swap check below could never fail.
    """
    cpp = join_wrapped_literals(strip_comments(cpp_raw))
    body = function_body(cpp, SUBJECT_FN)
    if body is None:
        return None

    spans = {}
    for verdict, condition in ARM_CONDITIONS.items():
        found = if_arm(body, condition)
        if found is None:
            continue
        spans[verdict] = (found[0], found[1])
        for child, parent in ELSE_OF.items():
            if parent == verdict and found[2] is not None:
                spans[child] = (found[2], found[3])

    texts = {}
    for verdict, (start, end) in spans.items():
        text = body[start:end]
        for other, (o_start, o_end) in spans.items():
            if other == verdict:
                continue
            if start <= o_start and o_end <= end:
                text = text.replace(body[o_start:o_end], "")
        texts[verdict] = text.lower()
    return texts


def ada_is_nested_in_not_ampere(cpp_raw):
    """Is the Ada test inside the not-Ampere block?

    NV_PART_ADA is a SUBSET of "not Ampere". Hoisting the Ada test out to sit
    beside the general one leaves both true for an Ada part, so which sentence
    an operator reads depends on statement order rather than on the decision --
    the dead-arm shape 3050-handoff33.md section 2 exists to remove.
    """
    cpp = join_wrapped_literals(strip_comments(cpp_raw))
    body = function_body(cpp, SUBJECT_FN)
    if body is None:
        return None
    outer = if_arm(body, ARM_CONDITIONS["NV_PART_OTHER_ARCHITECTURE"])
    if outer is None:
        return None
    inner_at = body.find(ARM_CONDITIONS["NV_PART_ADA"])
    if inner_at < 0:
        return None
    return outer[0] <= inner_at < outer[1]


def audit(cpp_raw, hpp_raw):
    failures = []

    members = header_verdict_members(hpp_raw)
    if members is None:
        return ["NVDecode.hpp: no `typedef enum { ... } %s;` block found -- "
                "this gate derives its subject set from that enum and cannot "
                "measure anything without it. REFUSING rather than passing."
                % VERDICT_ENUM_TAG]

    texts = arm_texts(cpp_raw)
    if texts is None:
        return ["NV3050Probe.cpp: no `%s` found -- the subject function this "
                "gate reads is gone or renamed. REFUSING rather than passing, "
                "because a gate that reports nothing when its anchor moves is "
                "indistinguishable from a green one." % SUBJECT_FN]

    # Property 1 -- every verdict in the header has a log arm.
    mapped = frozenset(ARM_MARKERS)
    for member in sorted(members - mapped):
        failures.append(
            "%s is a member of %s and this gate has no log arm mapped to it -- "
            "a verdict added to the header with no sentence in decodeBoot0() "
            "leaves the operator reading nothing about the case that fired."
            % (member, VERDICT_ENUM_TAG))
    for member in sorted(mapped - members):
        failures.append(
            "this gate maps a log arm to %s, which is no longer a member of "
            "%s -- the pairing is stale." % (member, VERDICT_ENUM_TAG))

    # Property 2 -- each arm exists and carries its own markers.
    for verdict in sorted(mapped & members):
        text = texts.get(verdict)
        if text is None:
            failures.append(
                "decodeBoot0: no log arm for %s -- that case now falls through "
                "silently, and for NV_PART_ADA specifically the lost sentence "
                "is the one with an operator action attached." % verdict)
            continue
        for marker in ARM_MARKERS[verdict]:
            if marker not in text:
                failures.append(
                    "decodeBoot0: the %s arm no longer says %r -- the arm is "
                    "present but no longer reports what it is for."
                    % (verdict, marker))

    # Property 3 -- markers are DISJOINT. Presence alone passes a body swap.
    for verdict in sorted(mapped & members):
        text = texts.get(verdict)
        if text is None:
            continue
        for other in sorted(mapped & members):
            if other == verdict:
                continue
            for marker in ARM_MARKERS[other]:
                if marker in text:
                    failures.append(
                        "decodeBoot0: the %s arm says %r, which belongs to the "
                        "%s arm -- the two arms report different findings and "
                        "this one is now claiming the other's."
                        % (verdict, marker, other))

    # Property 4 -- the Ada test is nested inside the not-Ampere test.
    nested = ada_is_nested_in_not_ampere(cpp_raw)
    if nested is None:
        failures.append(
            "decodeBoot0: could not locate both the not-Ampere test and the "
            "Ada test -- REFUSING rather than passing.")
    elif not nested:
        failures.append(
            "decodeBoot0: the Ada test is not nested inside the not-Ampere "
            "test. NV_PART_ADA is a SUBSET of 'not Ampere', so hoisting it out "
            "makes which sentence an operator reads depend on statement order "
            "rather than on the decision.")

    # Property 5 -- nvPartVerdictName() names every member.
    hpp = strip_comments(hpp_raw)
    name_body = function_body(hpp, VERDICT_NAME_FN)
    if name_body is None:
        failures.append(
            "NVDecode.hpp: no `%s` -- REFUSING rather than passing."
            % VERDICT_NAME_FN)
    else:
        named = frozenset(re.findall(r"case\s+(NV_PART_\w+)\s*:", name_body))
        for member in sorted(members - named):
            failures.append(
                "nvPartVerdictName: no arm for %s, so it renders as the "
                "default string and reaches the IORegistry as an unnamed "
                "verdict." % member)

    # Property 6 -- the Ada finding reaches the registry, not only the log.
    pub_body = function_body(strip_comments(cpp_raw), PUBLISHER_FN)
    if pub_body is None:
        failures.append(
            "NV3050Probe.cpp: no `%s` -- REFUSING rather than passing."
            % PUBLISHER_FN)
    else:
        if ADA_GUARD not in pub_body:
            failures.append(
                "publishIdentity: no `%s` guard -- the Ada retarget note is "
                "the log arm's consumer, and without it the finding is "
                "log-only again." % ADA_GUARD)
        if RETARGET_KEY not in pub_body:
            failures.append(
                "publishIdentity: %s is not published -- decodeBoot0's Ada "
                "sentence would once more reach the log and not the registry."
                % RETARGET_KEY)

    return failures


def mutate(text, old, new, occurrences=1):
    """Replace an anchor, asserting the match count in BOTH directions.

    Matching nothing prints ok silently -- the mutation is a no-op and the
    control reports a vacuous test as working. Matching too much raises loudly
    and looks like the mutation is wrong when the precondition is working.
    """
    count = text.count(old)
    if count != occurrences:
        raise AssertionError(
            "mutation anchor matched %d times, expected %d: %r"
            % (count, occurrences, old[:80]))
    return text.replace(old, new)


class DecodeBoot0LogArmTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.cpp = read_text(CPP_PATH)
        cls.hpp = read_text(HPP_PATH)

    # -- positive control ---------------------------------------------------

    def test_the_live_tree_passes(self):
        self.assertEqual(audit(self.cpp, self.hpp), [])

    # -- the stripper does its job ------------------------------------------

    def test_comment_stripping_removes_prose_naming_the_findings(self):
        #
        # NVDecode.hpp's block above nvPartVerdict() names all four findings in
        # words. If the stripper missed it, every marker check could pass on a
        # decodeBoot0() whose NVLOGs were deleted.
        #
        stripped = strip_comments(self.hpp)
        self.assertIn("an Ada part specifically", self.hpp)
        self.assertNotIn("an Ada part specifically", stripped)

    def test_wrapped_literals_are_joined_before_matching(self):
        #
        # The Ada message wraps mid-sentence, so "the whole plan retargets"
        # exists only after adjacent literals are concatenated.
        #
        raw = strip_comments(self.cpp)
        joined = join_wrapped_literals(raw)
        self.assertNotIn("the whole \"\n", joined)
        self.assertIn("the whole plan retargets", joined)

    # -- the subject set is derived, not spelled ----------------------------

    def test_the_verdict_set_comes_from_the_header(self):
        members = header_verdict_members(self.hpp)
        self.assertIsNotNone(members)
        self.assertEqual(members, frozenset(ARM_MARKERS))
        self.assertNotIn(VERDICT_LIMIT, members)

    def test_a_fifth_verdict_with_no_log_arm_reddens(self):
        broken = mutate(
            self.hpp,
            "    NV_PART_VERDICT_LIMIT",
            "    NV_PART_TURING,\n    NV_PART_VERDICT_LIMIT")
        failures = audit(self.cpp, broken)
        self.assertTrue(any("NV_PART_TURING" in f for f in failures), failures)

    # -- property 2: an arm that stops reporting what it is for -------------

    def test_the_ada_arm_losing_its_retarget_sentence_reddens(self):
        #
        # The sharpest of the four to lose. An 'RTX 3050 A' is Ada silicon under
        # a 30-series name; without this sentence the operator sees an
        # architecture mismatch and no statement of what to do about it.
        #
        broken = mutate(self.cpp,
                        '                  "plan retargets.");',
                        '                  "boot continues.");')
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NV_PART_ADA" in f and "retargets" in f for f in failures),
            failures)

    def test_the_not_ampere_arm_losing_its_diagnosis_reddens(self):
        broken = mutate(
            self.cpp,
            '              "claimed, or the read is not landing where you '
            'think");',
            '              "claimed.");')
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NV_PART_OTHER_ARCHITECTURE" in f for f in failures), failures)

    def test_the_ampere_other_arm_losing_its_sentence_reddens(self):
        #
        # This arm is the `else` of the GA107 test rather than an `if` of its
        # own, so it is found through ELSE_OF. A control that only ever mutated
        # `if`-shaped arms would leave that path unproven.
        #
        broken = mutate(
            self.cpp,
            '        NVLOG("  expected GA107 (impl 0x%x) for a mobile RTX '
            '3050",',
            '        NVLOG("  a different part (impl 0x%x)",')
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NV_PART_AMPERE_OTHER" in f for f in failures), failures)

    # -- property 3: presence alone passes a swap ---------------------------

    def test_an_arm_claiming_another_arms_finding_reddens(self):
        #
        # Every marker still present SOMEWHERE in the function, and the target
        # arm keeps its own -- so a presence-only gate stays green while the
        # GA107 sentence now also announces a plan retarget.
        #
        broken = mutate(
            self.cpp,
            '              "reached. Compare against your Linux mmiotrace '
            'capture.");',
            '              "reached. the whole plan retargets.");')
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("belongs to the" in f and "NV_PART_TARGET" in f
                for f in failures), failures)

    # -- property 4: the subset relation ------------------------------------

    def test_hoisting_the_ada_test_out_of_the_not_ampere_test_reddens(self):
        #
        # NV_PART_ADA is a SUBSET of "not Ampere". As a sibling AFTER the
        # general test the arm sits below a `return` and cannot run at all --
        # the dead-arm shape 3050-handoff33.md section 2 exists to remove,
        # reintroduced one function over.
        #
        broken = mutate(
            self.cpp,
            '        if (arch == NV_ARCHITECTURE_AD100) {\n'
            '            NVLOG("  this is an Ada part. If the laptop reports '
            'device ID "\n'
            '                  "0x2822/0x28A3/0x28E3 it is an \'RTX 3050 A\' '
            '-- Ada silicon "\n'
            '                  "under a 30-series name. See ../DEVICE-IDS.md; '
            'the whole "\n'
            '                  "plan retargets.");\n'
            '        }\n'
            '        return;\n'
            '    }\n',
            '        return;\n'
            '    }\n'
            '\n'
            '    if (arch == NV_ARCHITECTURE_AD100) {\n'
            '        NVLOG("  this is an Ada part. If the laptop reports '
            'device ID "\n'
            '              "0x2822/0x28A3/0x28E3 it is an \'RTX 3050 A\' -- '
            'Ada silicon "\n'
            '              "under a 30-series name. See ../DEVICE-IDS.md; the '
            'whole "\n'
            '              "plan retargets.");\n'
            '    }\n')
        failures = audit(broken, self.hpp)
        self.assertTrue(any("SUBSET" in f for f in failures), failures)

    # -- property 5: the verdict must still have a name ---------------------

    def test_a_verdict_losing_its_name_arm_reddens(self):
        broken = mutate(
            self.hpp,
            '        case NV_PART_ADA:                return "Ada -- not '
            'Ampere, plan retargets";',
            '        case NV_PART_VERDICT_LIMIT:      return "Ada -- not '
            'Ampere, plan retargets";')
        failures = audit(self.cpp, broken)
        self.assertTrue(
            any("nvPartVerdictName" in f and "NV_PART_ADA" in f
                for f in failures), failures)

    # -- property 6: the finding must reach the registry too ----------------

    def test_dropping_the_ada_publication_guard_reddens(self):
        broken = mutate(self.cpp,
                        "    if (part == NV_PART_ADA) {",
                        "    if (part == NV_PART_TARGET) {")
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(ADA_GUARD in f for f in failures), failures)

    def test_dropping_the_retarget_note_key_reddens(self):
        broken = mutate(self.cpp,
                        'setProperty("NVPartRetargetNote",',
                        'setProperty("NVPartRetarget",')
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any(RETARGET_KEY in f for f in failures), failures)

    # -- vacuity: a gate that cannot find its subject must REFUSE -----------

    def test_a_renamed_subject_function_refuses_rather_than_passes(self):
        #
        # Renamed to a name the anchor is NOT a prefix of. A SUFFIXED rename --
        # decodeBoot0Impl -- still CONTAINS the old name, so find() locates it
        # and the control passes for the wrong reason, reporting a vacuity arm
        # as working when it never ran. Measured in 3050-handoff39.md 9d.
        #
        broken = mutate(self.cpp,
                        "void NV3050Probe::decodeBoot0(UInt32 value)",
                        "void NV3050Probe::readPartIdentity(UInt32 value)")
        failures = audit(broken, self.hpp)
        self.assertTrue(any("REFUSING" in f for f in failures), failures)

    def test_a_renamed_publisher_refuses_rather_than_passes(self):
        broken = mutate(
            self.cpp,
            "void NV3050Probe::publishIdentity(UInt32 boot0, UInt32 boot42)",
            "void NV3050Probe::emitIdentity(UInt32 boot0, UInt32 boot42)")
        failures = audit(broken, self.hpp)
        self.assertTrue(any("REFUSING" in f for f in failures), failures)

    def test_a_renamed_verdict_enum_refuses_rather_than_passes(self):
        broken = mutate(self.hpp,
                        "} NVPartVerdict;",
                        "} NVPartKind;")
        failures = audit(self.cpp, broken)
        self.assertTrue(any("REFUSING" in f for f in failures), failures)


if __name__ == "__main__":
    unittest.main(verbosity=2)
