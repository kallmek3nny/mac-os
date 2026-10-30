#!/usr/bin/env python3
"""Shape gate over decodeBoot42()'s log arms -- the third member of the family.

Why this exists
---------------
``3050-handoff40.md`` section 1 moved the four-way chip decision out of the
IOKit shell and into ``nvChipVerdict()`` in ``phase0-kext/NVDecode.hpp``, and
taught ``publishIdentity()`` to publish it. What it did NOT do is require the
four LOG arms in ``decodeBoot42()`` to keep agreeing with the four verdicts.

That gap was named in ``3050-handoff40.md`` section 8 item 2 and section 9 item
1, in these words: edit one arm of ``decodeBoot42()`` without touching
``nvChipVerdict()`` and every gate stays green. Measured 2026-08-20 by deleting
the GA106 arm outright -- register gate 240/240, C++ 10 of 10, syntax exit 0,
Python OK. Nothing saw it.

This is deliberately WEAKER than proving the two agree. Proving agreement would
mean executing a function no host suite can compile: ``NV3050Probe.cpp`` reaches
IOKit, so ``decodeBoot42()`` is reachable only by parse. What this module pins
instead is that the log carries ONE arm per verdict the header declares, that
each arm is spelled in that verdict's own idiom, and that the nesting which
makes GA106 a subset of the mismatch case survives. A future fifth verdict added
to the enum reddens on the commit that adds it, naming the verdict.

Same idiom as ``tests/test_probe_identity_log_arms.py`` (decodeBoot0, the
``NVPartVerdict`` family) and ``tests/test_probe_gsp_result_log_arms.py`` (the
result switch). One ``audit()`` shared with the live tree, comment-stripped
source, one positive control and negative controls that each assert WHICH
property broke.

The subject set is DERIVED from the enum rather than spelled here. Spelling four
verdict names in a test makes the test a second copy of the header, and a copy
agrees with itself no matter what the header says -- ``intel-handoff26.md``
section 2a's reasoning, reached for a third family.
"""

import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP_PATH = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
HPP_PATH = os.path.join(ROOT, "phase0-kext", "NVDecode.hpp")

#
# Full signature, never a prefix. decodeBoot42 has no sibling whose name it
# prefixes today, but decodeBoot0 does not either and the identity gate still
# spells the full signature -- the cost of the habit is nothing and the cost of
# breaking it was measured in 3050-handoff35.md section 1, where
# NVGspStageUcode is a strict PREFIX of NVGspStageUcodeEvaluated.
#
SUBJECT_FN = "void NV3050Probe::decodeBoot42(UInt32 value)"
PUBLISHER_FN = "void NV3050Probe::publishIdentity(UInt32 boot0, UInt32 boot42)"
VERDICT_FN = "static inline NVChipVerdict nvChipVerdict(NVU32 boot42)"
VERDICT_NAME_FN = "static inline const char *nvChipVerdictName(NVChipVerdict v)"

VERDICT_ENUM_TAG = "NVChipVerdict"
VERDICT_LIMIT = "NV_CHIP_VERDICT_LIMIT"

SIBLING_KEY = "NVChipSiblingNote"
SIBLING_GUARD = "chip == NV_CHIP_SIBLING_GA106"

#
# Each verdict is paired with markers that must occur in ITS log arm and in no
# other. The markers are the subject's own bytes, read out of NV3050Probe.cpp on
# 2026-08-20 -- never a paraphrase, because a paraphrase is a claim about what
# the log says rather than a measurement of it.
#
# NV_CHIP_UNREADABLE's marker is the predicate rather than the prose, because
# the prose ("unexpected dead read") is the one phrase a future edit is most
# likely to reword while leaving the arm correct. nvLooksLikeDeadRead( is what
# the arm IS.
#
ARM_MARKERS = {
    "NV_CHIP_UNREADABLE": ["nvLooksLikeDeadRead(", "Aperture may be shorter"],
    "NV_CHIP_TARGET": ["chipId == NV_CHIP_ID_GA107", "matches GA107"],
    "NV_CHIP_OTHER": ["does not match the expected GA107"],
    "NV_CHIP_SIBLING_GA106": ["chipId == NV_CHIP_ID_GA106", "RTX 3050 Ti"],
}


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """Remove // and /* */ comments.

    Mandatory rather than stylistic, and more so here than in the sibling gate.
    NVDecode.hpp's header block above nvChipVerdict() names all four findings in
    prose -- it says "dead read", "GA107", "GA106" and "'RTX 3050 Ti'" in the
    same words the log arms use -- and NV3050Probe.cpp's own comment above the
    publication block quotes the sibling note. A gate reading prose would report
    all four arms present on a decodeBoot42() whose body had been gutted.

    There is a test below that plants each marker in a comment and requires the
    stripper to remove it.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            if j < 0:
                break
            i = j
            continue
        if two == "/*":
            j = text.find("*/", i + 2)
            if j < 0:
                break
            i = j + 2
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def join_wrapped_literals(text):
    """Join adjacent C string literals so a marker straddling a line wrap is seen.

    The GA106 arm's message is spelled across two source lines:

        NVLOG("  0x%03x is GA106 -- also sold as 'RTX 3050 Ti Laptop GPU'. "
              "Same architecture, different die and unit counts.",

    A marker that happens to land on the seam is ABSENT to a naive substring
    test, and the gate would then report a correct arm as missing -- or, worse,
    a control asserting its absence would pass for the wrong reason. Both live
    markers sit clear of the seam today; joining removes the dependence on that
    staying true.
    """
    return re.sub(r'"\s*\n\s*"', "", text)


def body_of(text, signature):
    """Return the brace-balanced body of the named function, or None.

    A balanced walk rather than a regex. handoff25.md section 1c is the cost of
    the alternative: a character class excluding only the statement terminator
    is not scoped to the statement, and runs past the closing paren of an `if`
    into the next statement entirely.
    """
    start = text.find(signature)
    if start < 0:
        return None
    open_brace = text.find("{", start + len(signature))
    if open_brace < 0:
        return None
    depth = 0
    i = open_brace
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:i]
        i += 1
    return None


def header_verdict_members(hpp_raw):
    """Derive the verdict names from the enum in NVDecode.hpp, or None.

    Returns the members BELOW the limit sentinel, in declaration order. The
    sentinel itself is excluded -- it is a count, not a finding, and requiring a
    log arm for it would redden correct code.
    """
    text = strip_comments(hpp_raw)
    #
    # The body class is [^{}]* rather than a non-greedy .*? with DOTALL, and the
    # difference is not cosmetic -- measured 2026-08-20, when the first draft of
    # this module reddened on a PRISTINE tree naming NV_PART_ADA and five other
    # NV_PART_* verdicts as missing chip log arms.
    #
    # NVDecode.hpp declares NVPartVerdict at :310, ABOVE NVChipVerdict at :385.
    # A DOTALL .*? anchors on the FIRST `typedef enum {` in the file -- the part
    # one -- and then scans forward across that enum's members, nvPartVerdictName()
    # and nvPartVerdict() until it finally reaches `} NVChipVerdict;`. The capture
    # is the WRONG enum's members plus seventy-odd lines of switch bodies, which
    # split on commas into tokens that are not identifiers, so this function
    # returned None and audit() REFUSED on correct shipping code.
    #
    # A class that cannot cross a brace is scoped to one enum body. handoff25.md
    # section 1c is the same lesson one construct over: there a negated class
    # excluding only the statement terminator was not scoped to the statement,
    # here a non-greedy wildcard is not scoped to the enum. In both cases the
    # pattern LOOKS scoped because the delimiter it names is the right one, and
    # in both cases the scoping has to come from the character class.
    #
    # It failed in the LOUD direction, which is the survivable one. A regex that
    # matched the right enum for the wrong reason would have drifted silently.
    #
    m = re.search(
        r"typedef\s+enum\s*\{([^{}]*)\}\s*" + re.escape(VERDICT_ENUM_TAG) + r"\s*;",
        text,
    )
    if m is None:
        return None
    members = []
    for raw in m.group(1).split(","):
        name = raw.split("=")[0].strip()
        if not name:
            continue
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            return None
        if name == VERDICT_LIMIT:
            break
        members.append(name)
    return members or None


def arm_texts(cpp_raw):
    """Return {verdict: [markers found in decodeBoot42's body]}, or None.

    The body is comment-stripped and wrap-joined before any marker is looked
    for, so every hit is code.
    """
    body = body_of(strip_comments(cpp_raw), SUBJECT_FN)
    if body is None:
        return None
    body = join_wrapped_literals(body)
    found = {}
    for verdict, markers in ARM_MARKERS.items():
        found[verdict] = [m for m in markers if m in body]
    return found


def sibling_is_nested(cpp_raw):
    """True when the GA106 test sits INSIDE the general mismatch case.

    GA106 is a SUBSET of "not GA107". Written as two sibling `if`s at the same
    level, whichever comes first wins and the other arm is dead -- the shape
    3050-handoff32.md exists to remove, hit one level up in handoff33 with Ada,
    and this is its third appearance.

    In decodeBoot42() the general mismatch arm is a FALL-THROUGH rather than a
    braced block: the NVLOG fires unconditionally after the GA107 arm returns,
    and the GA106 test follows it. So nesting here means ORDERING -- the GA106
    test must come after the general message, not before it. Measured as index
    order in the comment-stripped body rather than as brace depth, because
    there is no brace to measure.
    """
    body = body_of(strip_comments(cpp_raw), SUBJECT_FN)
    if body is None:
        return None
    body = join_wrapped_literals(body)
    general = body.find("does not match the expected GA107")
    sibling = body.find("chipId == NV_CHIP_ID_GA106")
    if general < 0 or sibling < 0:
        return None
    return general < sibling


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
        return ["NV3050Probe.cpp: `%s` not found -- the subject of this gate is "
                "missing. REFUSING rather than passing." % SUBJECT_FN]

    #
    # Property 1 -- the enum and the marker table describe the SAME set.
    #
    # This is the check that makes a fifth verdict redden on the commit that
    # adds it. Set equality in both directions: a verdict with no marker row is
    # a finding nobody pinned, and a marker row with no verdict is a row that
    # can never be measured against anything.
    #
    declared = set(members)
    tabled = set(ARM_MARKERS)
    for name in sorted(declared - tabled):
        failures.append(
            "%s declares verdict %s and this gate has no marker row for it. A "
            "verdict with no pinned log arm is exactly the gap this module "
            "exists to close -- add its markers, read out of decodeBoot42()."
            % (VERDICT_ENUM_TAG, name))
    for name in sorted(tabled - declared):
        failures.append(
            "this gate has a marker row for %s, which %s does not declare. The "
            "row can never be measured against anything." % (name, VERDICT_ENUM_TAG))

    #
    # Property 2 -- every declared verdict has its arm present in the LOG.
    #
    for name in members:
        markers = ARM_MARKERS.get(name)
        if not markers:
            continue
        missing = [m for m in markers if m not in texts.get(name, [])]
        if missing:
            failures.append(
                "decodeBoot42() has no log arm for %s -- missing %s. The "
                "verdict function still returns it, so the log and the "
                "registry now describe different partitions of the input space."
                % (name, ", ".join(repr(m) for m in missing)))

    #
    # Property 3 -- the GA106 arm stays a SUBSET of the mismatch arm.
    #
    nested = sibling_is_nested(cpp_raw)
    if nested is None:
        failures.append(
            "decodeBoot42(): could not locate both the general mismatch message "
            "and the GA106 test, so the subset relation is unmeasurable.")
    elif not nested:
        failures.append(
            "decodeBoot42(): the GA106 test precedes the general mismatch "
            "message. GA106 is a SUBSET of 'not GA107' -- ordered this way the "
            "operator sees the sibling note without the mismatch line that "
            "gives it its meaning, and the two arms stop matching "
            "nvChipVerdict()'s nesting.")

    #
    # Property 4 -- the verdict family still exists in the header.
    #
    # Without this, gutting NVDecode.hpp and gutting NV3050Probe.cpp TOGETHER is
    # a change every other property in this module agrees with: the enum would
    # be absent, the derived set empty, and the arm loop would iterate zero
    # times. The REFUSAL above catches a missing enum; these catch a missing
    # function beneath a surviving enum.
    #
    hpp = strip_comments(hpp_raw)
    for fn in (VERDICT_FN, VERDICT_NAME_FN):
        if fn not in hpp:
            failures.append(
                "NVDecode.hpp: `%s` is gone. decodeBoot42()'s log arms are "
                "pinned to agree with that function; with it absent they agree "
                "with nothing." % fn)

    #
    # Property 5 -- the publisher still consumes the sibling verdict.
    #
    # The log arm and the registry key are the two halves this family exists to
    # keep in step. Property 2 covers the log; this covers the registry, and it
    # is the half an operator actually reads.
    #
    pub = body_of(strip_comments(cpp_raw), PUBLISHER_FN)
    if pub is None:
        failures.append(
            "NV3050Probe.cpp: `%s` not found -- the consumer of nvChipVerdict() "
            "is missing." % PUBLISHER_FN)
    else:
        if SIBLING_GUARD not in pub:
            failures.append(
                "publishIdentity(): `%s` is gone, so the GA106 near miss reaches "
                "the log and not the registry -- the exact defect "
                "3050-handoff40.md closed." % SIBLING_GUARD)
        if ('"%s"' % SIBLING_KEY) not in pub:
            failures.append(
                "publishIdentity(): the %s key is gone. A GA106 board would "
                "publish NVChipIsTarget false with nothing naming which die it "
                "actually is." % SIBLING_KEY)

    #
    # Properties 6 through 9 are 3050-handoff41.md section 2 items 2, 3, 5 and
    # 6. That section describes them and section 4 lists their negative
    # controls, but this module was last written twelve minutes after
    # handoff41.md and carried none of them -- 3050-handoff44.md section 7 item
    # 4 recorded the symptom as one missing test. Restored 2026-10-30 from that
    # spec and re-measured against decodeBoot42() as it stands.
    #
    # One body for all four, comment-stripped and wrap-joined like arm_texts().
    # Never None here: arm_texts() already found the same signature above.
    #
    body = join_wrapped_literals(body_of(strip_comments(cpp_raw), SUBJECT_FN))

    #
    # Property 6 -- the arm COUNT matches, not just the arm markers.
    #
    # Property 2 alone is satisfied by a body that logs every marker
    # unconditionally in one blob. The dead-read arm and the GA107 arm each end
    # in a bare `return;`; the mismatch arm and its nested GA106 note fall
    # through to the closing brace and own none. Pinned rather than derived:
    # the count is a fact about this body's shape, read out of NV3050Probe.cpp.
    #
    expected_returns = 2
    returns = len(re.findall(r"\breturn\s*;", body))
    if returns != expected_returns:
        failures.append(
            "decodeBoot42(): %d bare `return;` statements, expected %d -- one "
            "ending the dead-read arm and one ending the GA107 arm. The "
            "bare-return count moved, so an arm that should be terminal now "
            "falls through into the next." % (returns, expected_returns))

    #
    # Property 7 -- the dead-read test precedes the general fall-through.
    #
    # The one ordering claim 3050-handoff40.md section 3 measured as
    # load-bearing. An all-ones word decodes to a chip ID that is not GA107, so
    # with the test below the mismatch message a dead read is first logged as a
    # real chip that failed to match. An absent marker is property 2's finding,
    # so this measures only when both are present.
    #
    dead = body.find("nvLooksLikeDeadRead(")
    general = body.find("does not match the expected GA107")
    if dead >= 0 and general >= 0 and dead > general:
        failures.append(
            "decodeBoot42(): the dead-read test follows the general mismatch "
            "message, so an all-ones BOOT_42 is logged as a chip ID mismatch "
            "before it is recognised as a dead read.")

    #
    # Property 8 -- the verdict is defined in the header, never in the shell.
    #
    # Property 4 proves the header still has it; this proves the shell did not
    # grow a second copy. A call such as `= nvChipVerdict(` does not match --
    # only a declarator with the return type directly before the name does.
    #
    if re.search(r"\bNVChipVerdict\s+nvChipVerdict\s*\(", strip_comments(cpp_raw)):
        failures.append(
            "NV3050Probe.cpp defines nvChipVerdict(). The verdict belongs in "
            "the dependency-free NVDecode.hpp, where a host suite can compile "
            "it; a copy in the IOKit shell is a pure function nothing runs.")

    #
    # Property 9 -- decodeBoot42() stays log-only.
    #
    # publishIdentity() is the one publisher of the chip verdict. A
    # setProperty() in the log function makes two publishers of one fact, and
    # nothing keeps them in step.
    #
    if "setProperty(" in body:
        failures.append(
            "decodeBoot42() calls setProperty(). The log function must stay "
            "log-only -- publishIdentity() is the one publisher of the chip "
            "verdict.")

    return failures


def mutate(text, anchor, replacement, occurrences=1):
    """Replace an anchor, refusing unless it occurs exactly `occurrences` times.

    Both directions are asserted. Matching NOTHING prints `ok` silently and the
    control passes without ever testing its property -- the vacuity direction.
    Matching too MUCH raises loudly and looks like the mutation is wrong when in
    fact the precondition is working (intel-handoff26.md section 3).

    3050-handoff40.md section 5b is the reason an anchor here is a single
    statement wherever possible: an anchor spanning two statements is a silent
    claim that nothing ever lands between them, and the next correct edit to the
    subject breaks it.
    """
    count = text.count(anchor)
    if count != occurrences:
        raise AssertionError(
            "mutation anchor occurred %d times, expected %d: %r"
            % (count, occurrences, anchor))
    return text.replace(anchor, replacement)


class Boot42LogArmTests(unittest.TestCase):
    def setUp(self):
        self.cpp = read_text(CPP_PATH)
        self.hpp = read_text(HPP_PATH)

    # ---- positive control -------------------------------------------------

    def test_the_live_tree_passes(self):
        """The shipping tree satisfies every property.

        The one check that shares audit() with the live files rather than with a
        mutant. If this reddens, everything below it is measuring a tree that is
        already broken.
        """
        failures = audit(self.cpp, self.hpp)
        self.assertEqual(failures, [], "\n".join(failures))

    # ---- the derived subject set ------------------------------------------

    def test_the_verdict_set_is_read_out_of_the_header(self):
        """The subject set comes from NVDecode.hpp, not from this file."""
        members = header_verdict_members(self.hpp)
        self.assertIsNotNone(members)
        self.assertEqual(set(members), set(ARM_MARKERS))
        self.assertNotIn(VERDICT_LIMIT, members)

    def test_a_fifth_verdict_with_no_arm_is_caught(self):
        """Adding a verdict to the enum reddens until its log arm is pinned.

        This is the tripwire the module exists for. It is asserted against the
        HEADER rather than the cpp, because that is the commit a future author
        writes first.
        """
        mutant = mutate(
            self.hpp,
            "    NV_CHIP_VERDICT_LIMIT",
            "    NV_CHIP_SIBLING_GA104,\n    NV_CHIP_VERDICT_LIMIT",
        )
        failures = audit(self.cpp, mutant)
        self.assertTrue(
            any("NV_CHIP_SIBLING_GA104" in f and "no marker row" in f
                for f in failures),
            failures)

    def test_a_missing_enum_refuses_rather_than_passes(self):
        """A gate reporting nothing must not be indistinguishable from a green one.

        3050-handoff30.md section 2b: a module that silently measures the empty
        set when its anchor moves is worse than no module, because it reports
        success over a subject it never found.
        """
        mutant = mutate(self.hpp, "} NVChipVerdict;", "} NVChipVerdictRenamed;")
        failures = audit(self.cpp, mutant)
        self.assertTrue(failures)
        self.assertTrue(any("REFUSING" in f for f in failures), failures)

    def test_a_missing_subject_refuses_rather_than_passes(self):
        """Renaming decodeBoot42() refuses rather than measuring the empty set."""
        mutant = mutate(
            self.cpp,
            "void NV3050Probe::decodeBoot42(UInt32 value)",
            "void NV3050Probe::decodeBoot42Renamed(UInt32 value)",
        )
        failures = audit(mutant, self.hpp)
        self.assertTrue(failures)
        self.assertTrue(any("REFUSING" in f for f in failures), failures)

    # ---- one negative control per arm -------------------------------------

    def test_deleting_the_dead_read_arm_is_caught(self):
        mutant = mutate(
            self.cpp,
            "    if (nvLooksLikeDeadRead(value)) {",
            "    if (false) {",
        )
        failures = audit(mutant, self.hpp)
        self.assertTrue(
            any("NV_CHIP_UNREADABLE" in f for f in failures), failures)

    def test_deleting_the_ga107_arm_is_caught(self):
        mutant = mutate(
            self.cpp,
            "    if (chipId == NV_CHIP_ID_GA107) {",
            "    if (chipId == 0u) {",
        )
        failures = audit(mutant, self.hpp)
        self.assertTrue(any("NV_CHIP_TARGET" in f for f in failures), failures)

    def test_deleting_the_general_mismatch_arm_is_caught(self):
        mutant = mutate(
            self.cpp,
            '    NVLOG("  chip ID 0x%03x does not match the expected GA107 0x%03x",\n'
            "          (unsigned)chipId, (unsigned)NV_CHIP_ID_GA107);\n",
            "",
        )
        failures = audit(mutant, self.hpp)
        self.assertTrue(any("NV_CHIP_OTHER" in f for f in failures), failures)

    def test_deleting_the_ga106_arm_is_caught(self):
        """The arm whose deletion left all five gates green on 2026-08-20."""
        mutant = mutate(
            self.cpp,
            "    if (chipId == NV_CHIP_ID_GA106) {",
            "    if (false) {",
        )
        failures = audit(mutant, self.hpp)
        self.assertTrue(
            any("NV_CHIP_SIBLING_GA106" in f for f in failures), failures)

    # ---- the subset relation ----------------------------------------------

    def test_hoisting_the_ga106_test_above_the_mismatch_line_is_caught(self):
        """GA106 before the general message reddens property 3.

        The mutation moves the general NVLOG below the GA106 block rather than
        moving the block, because the block returns -- moving the block above a
        message it must follow is the defect, and this is the smallest edit that
        produces it while leaving both arms present. Property 2 therefore stays
        green and only property 3 reddens, which is what makes this control
        discriminating rather than loud.
        """
        general = (
            '    NVLOG("  chip ID 0x%03x does not match the expected GA107 0x%03x",\n'
            "          (unsigned)chipId, (unsigned)NV_CHIP_ID_GA107);\n"
        )
        sibling = (
            "    if (chipId == NV_CHIP_ID_GA106) {\n"
            '        NVLOG("  0x%03x is GA106 -- also sold as \'RTX 3050 Ti Laptop GPU\'. "\n'
            '              "Same architecture, different die and unit counts.",\n'
            "              (unsigned)NV_CHIP_ID_GA106);\n"
            "    }\n"
        )
        #
        # Both blocks are removed and re-inserted in the opposite order, in ONE
        # edit each, so the result is a genuine swap. The first draft of this
        # control deleted the general message and re-inserted it immediately
        # ABOVE the GA106 `if` -- which is exactly where it already was -- so the
        # mutation was a no-op and the control failed while reporting nothing
        # about its own property. A mutation that changes nothing is the vacuity
        # direction wearing a red hat.
        #
        mutant = mutate(self.cpp, general + "\n" + sibling, sibling + "\n" + general)
        failures = audit(mutant, self.hpp)
        self.assertTrue(
            any("SUBSET" in f for f in failures), failures)

    # ---- the header half ---------------------------------------------------

    def test_gutting_the_verdict_function_is_caught(self):
        mutant = mutate(
            self.hpp,
            "static inline NVChipVerdict nvChipVerdict(NVU32 boot42)",
            "static inline NVChipVerdict nvChipVerdictGutted(NVU32 boot42)",
        )
        failures = audit(self.cpp, mutant)
        self.assertTrue(any("nvChipVerdict(NVU32" in f for f in failures), failures)

    def test_gutting_the_verdict_name_function_is_caught(self):
        mutant = mutate(
            self.hpp,
            "static inline const char *nvChipVerdictName(NVChipVerdict v)",
            "static inline const char *nvChipVerdictNameGutted(NVChipVerdict v)",
        )
        failures = audit(self.cpp, mutant)
        self.assertTrue(
            any("nvChipVerdictName(" in f for f in failures), failures)

    # ---- the publisher half ------------------------------------------------

    def test_deleting_the_sibling_guard_is_caught(self):
        mutant = mutate(
            self.cpp,
            "    if (chip == NV_CHIP_SIBLING_GA106) {",
            "    if (false) {",
        )
        failures = audit(mutant, self.hpp)
        self.assertTrue(
            any(SIBLING_GUARD in f for f in failures), failures)

    def test_deleting_the_sibling_key_is_caught(self):
        mutant = mutate(
            self.cpp,
            'setProperty("NVChipSiblingNote",',
            'setProperty("NVChipSiblingNoteRenamed",',
        )
        failures = audit(mutant, self.hpp)
        self.assertTrue(any(SIBLING_KEY in f for f in failures), failures)

    # ---- properties 6 through 9 -------------------------------------------
    #
    # The controls 3050-handoff41.md section 4 lists for these properties,
    # absent from this module until 2026-10-30 alongside the properties
    # themselves. Each asserts its own property's message AND that nothing else
    # reddened, so no control can pass on another property's failure.
    #

    def test_deleting_the_dead_read_return_is_caught(self):
        """Property 6: a terminal arm that falls through moves the count.

        `return;` alone occurs many times in the file, so the anchor carries
        the dead-read NVLOG's closing line with it. mutate() refuses loudly if
        anything ever lands between the two.
        """
        tail = "              (unsigned)value, (unsigned)NV_PMC_BOOT_42);\n"
        mutant = mutate(self.cpp, tail + "        return;\n", tail)
        failures = audit(mutant, self.hpp)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("bare-return count moved", failures[0])

    def test_hoisting_the_dead_read_test_below_the_mismatch_line_is_caught(self):
        """Property 7: an all-ones word must not be logged as a chip first.

        The whole dead-read block moves below the general mismatch message, one
        edit each way, so every arm stays present and the bare-return count
        stays two -- only the ordering changes.
        """
        dead = (
            "    if (nvLooksLikeDeadRead(value)) {\n"
            '        NVLOG("NV_PMC_BOOT_42 = 0x%08x -- unexpected dead read at 0x%x while "\n'
            '              "NV_PMC_BOOT_0 responded. Aperture may be shorter than 16 MiB.",\n'
            "              (unsigned)value, (unsigned)NV_PMC_BOOT_42);\n"
            "        return;\n"
            "    }\n"
        )
        general = (
            '    NVLOG("  chip ID 0x%03x does not match the expected GA107 0x%03x",\n'
            "          (unsigned)chipId, (unsigned)NV_CHIP_ID_GA107);\n"
        )
        mutant = mutate(self.cpp, dead + "\n", "")
        mutant = mutate(mutant, general, general + "\n" + dead)
        failures = audit(mutant, self.hpp)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("before it is recognised as a dead read", failures[0])

    def test_defining_the_verdict_in_the_shell_is_caught(self):
        """Property 8: a second nvChipVerdict() in the IOKit shell reddens."""
        mutant = mutate(
            self.cpp,
            SUBJECT_FN,
            "NVChipVerdict nvChipVerdict(UInt32 boot42)\n"
            "{\n"
            "    return NV_CHIP_OTHER;\n"
            "}\n"
            "\n" + SUBJECT_FN,
        )
        failures = audit(mutant, self.hpp)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("defines nvChipVerdict()", failures[0])

    def test_a_set_property_in_the_log_function_is_caught(self):
        """Property 9: decodeBoot42() must not become a second publisher.

        Added beside the GA107 NVLOG rather than replacing it, so every arm
        marker survives and only property 9 can redden.
        """
        ga107 = ('        NVLOG("  chip ID 0x%03x matches GA107", '
                 "(unsigned)NV_CHIP_ID_GA107);\n")
        mutant = mutate(
            self.cpp, ga107,
            ga107 + '        setProperty("NVChipIsTarget", true);\n')
        failures = audit(mutant, self.hpp)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("must stay log-only", failures[0])

    # ---- the stripper ------------------------------------------------------

    def test_a_marker_in_a_comment_does_not_count(self):
        """Comment-stripping is load-bearing, not stylistic.

        NVDecode.hpp's own header block names all four findings in prose. This
        plants a marker in a comment inside a GUTTED arm and requires the gate
        to still report the arm missing -- otherwise the prose above the
        function would keep every arm 'present' on an empty body.
        """
        mutant = mutate(
            self.cpp,
            "    if (chipId == NV_CHIP_ID_GA106) {",
            "    // chipId == NV_CHIP_ID_GA106 and 'RTX 3050 Ti'\n"
            "    if (false) {",
        )
        failures = audit(mutant, self.hpp)
        self.assertTrue(
            any("NV_CHIP_SIBLING_GA106" in f for f in failures), failures)

    def test_the_stripper_removes_both_comment_forms(self):
        text = "a // one\nb /* two */ c\n"
        self.assertNotIn("one", strip_comments(text))
        self.assertNotIn("two", strip_comments(text))
        self.assertIn("a", strip_comments(text))
        self.assertIn("c", strip_comments(text))

    def test_wrapped_literals_are_joined(self):
        """A marker straddling a line wrap must still be found.

        a-wrapped-literal-hides-its-own-sentence, one file over.
        """
        text = 'NVLOG("first half "\n      "second half");'
        self.assertIn("first half second half", join_wrapped_literals(text))

    # ---- the mutate() precondition ----------------------------------------

    def test_mutate_refuses_an_anchor_that_matches_nothing(self):
        with self.assertRaises(AssertionError):
            mutate(self.cpp, "this string is not in the subject", "x")

    def test_mutate_refuses_an_anchor_that_matches_too_much(self):
        with self.assertRaises(AssertionError):
            mutate(self.cpp, "    return;\n", "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
