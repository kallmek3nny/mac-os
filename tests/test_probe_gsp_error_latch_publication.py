#!/usr/bin/env python3
"""Shape gate over NV3050Probe.cpp's publishGspErrorLatches().

Why this exists
---------------
Until 2026-08-19 ``inspectGspEngine()`` read the two RISC-V error-status
registers, ran them through a two-line bool, and wrote the answer to ``NVLOG``
and nowhere else::

    if (nvRiscvHasError(privErr, hubErr))
        NVLOG("GSP: ERROR LATCHES SET -- ...");

Two defects sat in that, and only one of them is about publication.

The bool collapsed FOUR structurally different situations onto one answer: a
priv fault, a hub fault, both, and NEITHER REGISTER DECODING.  ``regRead32()``
reports every refusal it has by returning all-ones, so a BAR0 mapping covering
the Falcon block but stopping before the RISC-V block one 4 KiB page above it
makes both latches read ``0xFFFFFFFF`` -- and the old bool answered TRUE,
reporting a bug in THIS DRIVER as a latched hardware fault.  Reset the engine
versus fix the mapping: opposite actions, one answer.

And three of the five registers in the block (``PRIV_ERR_INFO``,
``PRIV_ERR_ADDR``, ``PRIV_ERR_ADDR_HI``) were read by NOTHING at all, so a
reported fault carried no address -- the actionable half of the diagnosis was
simply absent.

Why no other gate can see this
------------------------------
The same four by-construction blindnesses every module in this family lists:

1. ``tools/check-registers.py`` parses ``#define``s.  Every register here was
   already defined; the defect was that nothing READ three of them.  The
   register gate is correctly unmoved at 240.
2. No C++ suite can link a translation unit reaching ``IOService``, so
   ``publishGspErrorLatches()`` and ``inspectGspEngine()`` are unreachable from
   ``tests/*.cpp`` by construction.  ``tests/test_falcon.cpp`` proves the
   NVRISCV_ERR_* verdict is CORRECT in 239 checks and says nothing whatsoever
   about whether a publisher consumes it.
3. ``-Werror`` has nothing to say about a ``setProperty()`` that was never
   written, nor about a guard deleted from around one that was.
4. A suite proving a value is right says nothing about whether it is USED --
   ``intel-handoff26`` S1's heavily-tested-function-with-no-callers, for the
   fourth time in this tree.

So the shape is gated here instead.  Nine properties, each with a negative
control asserting WHICH property broke -- never mere non-emptiness, because a
control that only checks ``failures != []`` can go green off an unrelated
property and hide a defect inside the gate itself (3050-handoff35 S3a is that
exact near-miss).

All scanning is over COMMENT-STRIPPED source.  This is mandatory rather than
stylistic: the fix's own comment block names every key it publishes and quotes
the defective bool it replaced, so a prose-reading gate would count keys as
published on a publisher whose body had been gutted.
"""

import os
import re
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHELL = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
HEADER = os.path.join(ROOT, "phase0-kext", "NV3050Probe.hpp")
FALCON = os.path.join(ROOT, "phase0-kext", "NVFalcon.hpp")

PUBLISHER = "publishGspErrorLatches"
CONSUMER = "inspectGspEngine"

#
# The three localization registers.  Spelled as the accessor arguments the
# consumer must pass, because that is what was missing: the #defines existed and
# nothing read them.
#
LOCALIZATION_READS = (
    "NV_PRISCV_RISCV_PRIV_ERR_INFO",
    "NV_PRISCV_RISCV_PRIV_ERR_ADDR",
    "NV_PRISCV_RISCV_PRIV_ERR_ADDR_HI",
)

#
# Every key anchor carries its CLOSING QUOTE.  ``GSP_PRIV_ERR_ADDR`` is a strict
# PREFIX of ``GSP_PRIV_ERR_ADDR_HI``, and ``GSPErrorLatchesReadable`` shares its
# first eighteen characters with ``GSPErrorLatchesReason``.  A bare substring
# count of the shorter member returns the sum of both, and the longer key would
# look published when only the shorter one was.
#
# The counts are EXPECTED OCCURRENCES, not mere presence.  Presence cannot see a
# copy-paste that publishes one key twice and another never, and cannot see an
# arm deleted from a two-arm publisher.
#
EXPECTED_KEYS = {
    '"GSPErrorVerdict"': 1,
    '"GSPErrorLatchesReadable"': 1,
    '"GSP_PRIV_ERR_STAT"': 2,      # here, and in publishGspState()
    '"GSP_HUB_ERR_STAT"': 2,       # here, and in publishGspState()
    '"GSPErrorLatchesReason"': 1,
    '"GSP_PRIV_ERR_INFO"': 1,
    '"GSP_PRIV_ERR_ADDR"': 1,
    '"GSP_PRIV_ERR_ADDR_HI"': 1,
}

#
# The three keys that must be published on BOTH arms, i.e. above the UNREADABLE
# early return.  The verdict and the readable flag because their absence would be
# indistinguishable from an older kext (3050-handoff33 S1a); the two status words
# because on the UNREADABLE arm their all-ones value IS the evidence for the
# verdict, and the verdict beside them says how to read it.
#
KEYS_BEFORE_RETURN = (
    '"GSPErrorVerdict"',
    '"GSPErrorLatchesReadable"',
    '"GSP_PRIV_ERR_STAT"',
    '"GSP_HUB_ERR_STAT"',
)

#
# The three keys that must NOT be published on the UNREADABLE arm.  Nothing on
# that path measured them, and a refusal value published as a word is
# byte-identical to a measured one (handoff18 S1d).
#
KEYS_AFTER_RETURN = (
    '"GSP_PRIV_ERR_INFO"',
    '"GSP_PRIV_ERR_ADDR"',
    '"GSP_PRIV_ERR_ADDR_HI"',
)


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """Remove // and /* */ comments, preserving string literals.

    Mandatory here rather than stylistic.  ``publishGspErrorLatches()``'s own
    comment block names GSPErrorVerdict, GSPErrorLatchesReadable, PRIV_ERR_STAT,
    HUB_ERR_STAT and handoff18 s1d by name, and quotes the bool it replaced.  A
    gate reading prose would find every key it exists to require on a publisher
    whose body was deleted.
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
                if text[i] == "\\":
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def body_of(text, name):
    """Return the brace-balanced body of the method whose name is `name`.

    A balanced walk rather than a regex.  handoff25 S1c is the cost of the
    alternative: a character class excluding only the statement terminator runs
    past an ``if``'s closing paren, the newline, and the whole guarded body to
    land on the next statement's semicolon.
    """
    match = re.search(r"NV3050Probe::" + re.escape(name) + r"\s*\(", text)
    if match is None:
        return None
    open_brace = text.find("{", match.end())
    if open_brace < 0:
        return None
    depth = 0
    i = open_brace
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"' or ch == "'":
            quote = ch
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:i]
        i += 1
    return None


def brace_depth(body, index):
    """Nesting depth of `index` within `body`, counting from its start.

    Statement level and nesting level are two different questions and only the
    second one means unguarded.  A call nested inside an ``if`` block is preceded
    by that block's opening ``{``, which IS a statement boundary -- so a
    walk-back-to-the-previous-terminator test reports such a call as bare, which
    is exactly the mutation an unconditional-ness property exists to catch
    (3050-handoff35 S3a).

    ``body_of()`` returns the text INSIDE the function's outer braces, so an
    unconditional statement in that body sits at depth 0 and anything guarded
    sits at 1 or deeper.

    String literals are skipped, or a ``{`` inside an NVLOG format string would
    shift every depth after it.
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


def guard_span(body, index):
    """Brace span of the block opening at or after `index`.

    Returns (start, end) indices of the block's contents, or None.  Used to ask
    what the UNREADABLE arm publishes: a count over the whole body is satisfied
    by keys sitting anywhere, including below an early return on an arm that
    publishes nothing at all.
    """
    open_brace = body.find("{", index)
    if open_brace < 0:
        return None
    depth = 0
    i = open_brace
    n = len(body)
    while i < n:
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
            if depth == 0:
                return (open_brace + 1, i)
        i += 1
    return None


def audit(shell_text, header_text, falcon_text):
    """Return a list of failure strings.  Empty means every property holds.

    ONE audit, shared between the live tree and every mutant.  A separate
    mutation-only checker is a second implementation that can agree with the
    first while both are wrong.

    Refuses rather than passing when the subject cannot be found.  A gate that
    reports nothing when its anchor moves is indistinguishable from a green one,
    which is the vacuity direction and the worse of the two.
    """
    failures = []

    shell = strip_comments(shell_text)
    header = strip_comments(header_text)
    falcon = strip_comments(falcon_text)

    #
    # Property 1 -- the publisher is DECLARED in the header.  A definition with
    # no declaration does not compile, so this is cheap; it is here because it is
    # also the anchor everything below depends on, and its absence must produce a
    # named refusal rather than eight confusing failures.
    #
    #
    # Matched as a CALL-SHAPED token rather than a bare substring.  A bare `in`
    # test is satisfied by any name this one is a PREFIX of, so a declaration
    # renamed to publishGspErrorLatchesXX would still contain it and the check
    # would pass on a header that declares nothing this file defines.
    #
    if re.search(re.escape(PUBLISHER) + r"\s*\(", header) is None:
        failures.append(
            "property 1: NV3050Probe.hpp does not declare " + PUBLISHER + "()")

    pub_body = body_of(shell, PUBLISHER)
    if pub_body is None:
        failures.append(
            "REFUSAL: cannot find NV3050Probe::" + PUBLISHER + "()'s body in "
            "NV3050Probe.cpp -- this gate measured NOTHING about properties "
            "2-7 rather than passing them")

    cons_body = body_of(shell, CONSUMER)
    if cons_body is None:
        failures.append(
            "REFUSAL: cannot find NV3050Probe::" + CONSUMER + "()'s body in "
            "NV3050Probe.cpp -- this gate measured NOTHING about properties "
            "8-9 rather than passing them")

    #
    # Property 2 -- the verdict family exists in NVFalcon.hpp and the publisher
    # renders it through the family's OWN name function.
    #
    # Derived from the header rather than spelled here, in the sense that the
    # five arms must all be present: a family that lost an arm reddens on the
    # commit that loses it, rather than silently publishing a sentinel string.
    #
    # Matched as a #define with a trailing WORD BOUNDARY, never a bare
    # substring.  Every arm here is a strict PREFIX of a name somebody could
    # rename it to -- NVRISCV_ERR_UNREADABLE is a substring of
    # NVRISCV_ERR_UNREADABLE_RENAMED, and NVRISCV_ERR_NONE of anything suffixed
    # -- so a bare `in` test stays green on exactly the deletion this property
    # exists to catch.  Underscore is a word character, so \b refuses the
    # suffixed spelling rather than accepting it.
    #
    for arm in ("NVRISCV_ERR_NONE", "NVRISCV_ERR_PRIV", "NVRISCV_ERR_HUB",
                "NVRISCV_ERR_BOTH", "NVRISCV_ERR_UNREADABLE",
                "NVRISCV_ERR_VERDICT_LIMIT"):
        if re.search(r"#define\s+" + re.escape(arm) + r"\b", falcon) is None:
            failures.append(
                "property 2: NVFalcon.hpp does not define " + arm)

    if pub_body is not None:
        if "nvRiscvErrorVerdictName(" not in pub_body:
            failures.append(
                "property 2: " + PUBLISHER + "() does not render the verdict "
                "through nvRiscvErrorVerdictName() -- a bare number in the "
                "IORegistry is a number an operator has to decode by reading "
                "this source")

    #
    # Property 3 -- every expected key is published, the expected number of
    # times, across the whole file.
    #
    for key, expected in sorted(EXPECTED_KEYS.items()):
        found = shell.count(key)
        if found != expected:
            failures.append(
                "property 3: key " + key + " occurs " + str(found) +
                " time(s) in NV3050Probe.cpp, expected " + str(expected))

    if pub_body is not None:
        #
        # Property 4 -- the readable flag is a NEGATED EQUALITY against the one
        # UNREADABLE arm, never a disjunction over the fault arms.
        #
        # A disjunction (`== PRIV || == HUB || == BOTH`) is FALSE for a sixth
        # verdict added later, so it would report an unmeasured latch as
        # readable.  Same reasoning as handoff32 S3's `!= ERR_HEADER`, handoff33
        # S2's NVPartIsTarget, and handoff35 S2's NVGspStageOk -- general enough
        # by now that it should be the default assumption for any boolean
        # derived from an enum.
        #
        flag = re.search(
            r'setProperty\(\s*"GSPErrorLatchesReadable"\s*,\s*'
            r'([^;]*?)\)\s*;', pub_body, re.S)
        if flag is None:
            failures.append(
                "property 4: cannot find the GSPErrorLatchesReadable "
                "setProperty() call to inspect its expression")
        else:
            expr = " ".join(flag.group(1).split())
            if expr != "errVerdict != NVRISCV_ERR_UNREADABLE":
                failures.append(
                    "property 4: GSPErrorLatchesReadable is published as \"" +
                    expr + "\", expected the negated equality "
                    "\"errVerdict != NVRISCV_ERR_UNREADABLE\"")

        #
        # Property 5 -- the UNREADABLE arm returns EARLY, and the four
        # both-arms keys are published ABOVE it.
        #
        # A count over the whole body (property 3) is satisfied by keys sitting
        # anywhere, including entirely below the early return -- which publishes
        # nothing at all on the arm an operator holding a refused read is
        # actually looking at.
        #
        guard = re.search(
            r"if\s*\(\s*errVerdict\s*==\s*NVRISCV_ERR_UNREADABLE\s*\)",
            pub_body)
        if guard is None:
            failures.append(
                "property 5: " + PUBLISHER + "() has no "
                "`if (errVerdict == NVRISCV_ERR_UNREADABLE)` arm -- the "
                "localization words would then be published on a path that "
                "never read them")
        else:
            for key in KEYS_BEFORE_RETURN:
                at = pub_body.find(key)
                if at < 0:
                    failures.append(
                        "property 5: " + key + " is not published in " +
                        PUBLISHER + "() at all")
                elif at > guard.start():
                    failures.append(
                        "property 5: " + key + " is published BELOW the "
                        "UNREADABLE arm, so it is absent on that path -- an "
                        "absent key is a question rather than an answer")

            #
            # Property 6 -- the UNREADABLE arm publishes the reason and
            # NOTHING ELSE, and returns.
            #
            span = guard_span(pub_body, guard.end())
            if span is None:
                failures.append(
                    "property 6: cannot find the UNREADABLE arm's brace span")
            else:
                arm = pub_body[span[0]:span[1]]
                #
                # A STATEMENT, not the identifier.  The reason string inside
                # this very arm says "regRead32() returns for every refusal",
                # so a substring test for "return" is satisfied by the arm's
                # own prose and stays green after the return is deleted -- the
                # same class as `a-token-presence-check-is-not-a-return-check`.
                #
                if re.search(r"\breturn\s*;", arm) is None:
                    failures.append(
                        "property 6: the UNREADABLE arm does not return, so "
                        "execution falls through and publishes the three "
                        "localization words nothing on that path read")
                if arm.count("setProperty(") != 1:
                    failures.append(
                        "property 6: the UNREADABLE arm makes " +
                        str(arm.count("setProperty(")) + " setProperty() "
                        "call(s), expected exactly 1 (the reason string)")
                if '"GSPErrorLatchesReason"' not in arm:
                    failures.append(
                        "property 6: the UNREADABLE arm does not publish "
                        "GSPErrorLatchesReason, so the refusal carries no "
                        "explanation an operator can read")
                for key in KEYS_AFTER_RETURN:
                    if key in arm:
                        failures.append(
                            "property 6: " + key + " is published INSIDE the "
                            "UNREADABLE arm -- nothing on that path measured "
                            "it, and a refusal value published as a word is "
                            "byte-identical to a measured one")

            #
            # Property 7 -- the three localization words are published BELOW
            # the arm, i.e. only on the readable path.
            #
            for key in KEYS_AFTER_RETURN:
                at = pub_body.find(key)
                if at < 0:
                    failures.append(
                        "property 7: " + key + " is not published in " +
                        PUBLISHER + "() at all -- a reported fault would carry "
                        "no address")
                elif at < guard.start():
                    failures.append(
                        "property 7: " + key + " is published ABOVE the "
                        "UNREADABLE arm, so it lands on a path that never "
                        "read it")

    if cons_body is not None:
        #
        # Property 8 -- the consumer READS all three localization registers.
        #
        # This is the half no publication check can see: until 2026-08-19 these
        # three #defines occurred at exactly one place each in the whole tree,
        # their own definition line.  A publisher can be perfectly shaped around
        # values nobody ever read.
        #
        for reg in LOCALIZATION_READS:
            if reg not in cons_body:
                failures.append(
                    "property 8: " + CONSUMER + "() does not read " + reg +
                    " -- a reported fault would name no access and no address")

        #
        # Property 9 -- the verdict is derived ONCE and the publisher call is
        # UNCONDITIONAL.
        #
        # Depth, not bareness.  A call moved inside the fault arm still
        # publishes on the path somebody debugging a fault reads, which is
        # exactly why it looks harmless -- and it makes the keys' ABSENCE mean
        # two different things: latches clear, or an older kext.
        #
        derivations = re.findall(r"nvRiscvErrorVerdict\s*\(", cons_body)
        if len(derivations) != 1:
            failures.append(
                "property 9: " + CONSUMER + "() calls nvRiscvErrorVerdict() " +
                str(len(derivations)) + " time(s), expected exactly 1 -- "
                "deriving a predicate more than once is how it ends up "
                "disagreeing with itself")

        calls = [m.start() for m in
                 re.finditer(re.escape(PUBLISHER) + r"\s*\(", cons_body)]
        if len(calls) != 1:
            failures.append(
                "property 9: " + CONSUMER + "() calls " + PUBLISHER + "() " +
                str(len(calls)) + " time(s), expected exactly 1")
        else:
            depth = brace_depth(cons_body, calls[0])
            if depth != 0:
                failures.append(
                    "property 9: the " + PUBLISHER + "() call sits at brace "
                    "depth " + str(depth) + ", expected 0 -- it is nested "
                    "inside a conditional, so the keys are absent on some "
                    "paths and their absence means two different things")

            #
            # And it precedes the three-arm log block that consumes the same
            # verdict.  Published BEFORE anything acts on the verdict, so the
            # keys are present whatever happens next.
            #
            first_arm = cons_body.find("if (errVerdict == NVRISCV_ERR_UNREADABLE)")
            if first_arm < 0:
                failures.append(
                    "property 9: cannot find the UNREADABLE log arm in " +
                    CONSUMER + "() to order the publisher call against")
            elif calls[0] > first_arm:
                failures.append(
                    "property 9: the " + PUBLISHER + "() call comes AFTER the "
                    "verdict is acted on, so an early return added between "
                    "them would publish nothing")

    return failures


def mutate(text, old, new, count=1):
    """Replace `old` exactly `count` times, or raise.

    A mutation that matches nothing prints ok, and one that matches more than it
    means raises loudly and looks like the mutation is wrong when the
    precondition is working (intel-handoff26 S3).  Asserting the occurrence
    COUNT is what separates the two.
    """
    found = text.count(old)
    if found != count:
        raise AssertionError(
            "mutation anchor occurs " + str(found) + " time(s), expected " +
            str(count) + ": " + repr(old[:70]))
    return text.replace(old, new, count)


def only(failures, needle):
    """True when at least one failure mentions `needle`."""
    return any(needle in f for f in failures)


class ErrorLatchPublicationTests(unittest.TestCase):
    """The live tree, plus one control per property."""

    @classmethod
    def setUpClass(cls):
        cls.shell = read(SHELL)
        cls.header = read(HEADER)
        cls.falcon = read(FALCON)

    # ---------------------------------------------------------------- positive

    def test_the_live_tree_holds_every_property(self):
        """The positive control.  Everything below is only meaningful if this
        passes on the tree as committed."""
        failures = audit(self.shell, self.header, self.falcon)
        self.assertEqual([], failures, "\n  " + "\n  ".join(failures))

    def test_the_comment_stripper_removes_the_fixs_own_prose(self):
        """Comment stripping is load-bearing, not cosmetic.

        The publisher's comment block names GSPErrorVerdict and
        GSPErrorLatchesReadable in prose.  If the stripper failed, property 3
        would count those mentions as publications.
        """
        stripped = strip_comments(self.shell)
        self.assertIn("an absent key is a question", self.shell)
        self.assertNotIn("an absent key is a question", stripped)
        #
        # And the string literals it must NOT remove survive.
        #
        self.assertIn('"GSPErrorVerdict"', stripped)

    # ---------------------------------------------------------------- controls

    def test_control_header_declaration_removed(self):
        header = mutate(self.header, PUBLISHER, "publishGspErrorLatchesXX")
        failures = audit(self.shell, header, self.falcon)
        self.assertTrue(only(failures, "property 1"), failures)

    def test_control_publisher_renamed_refuses_rather_than_passes(self):
        shell = mutate(
            self.shell,
            "void NV3050Probe::publishGspErrorLatches(",
            "void NV3050Probe::publishGspErrorLatchesRenamed(")
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "REFUSAL"), failures)

    def test_control_verdict_arm_deleted_from_the_family(self):
        falcon = mutate(
            self.falcon, "#define NVRISCV_ERR_UNREADABLE",
            "#define NVRISCV_ERR_UNREADABLE_RENAMED")
        failures = audit(self.shell, self.header, falcon)
        self.assertTrue(only(failures, "property 2"), failures)

    def test_control_verdict_published_as_a_bare_number(self):
        shell = mutate(
            self.shell,
            'setProperty("GSPErrorVerdict", nvRiscvErrorVerdictName(errVerdict));',
            'setProperty("GSPErrorVerdict", (UInt64)errVerdict, 32);')
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "property 2"), failures)

    def test_control_a_localization_key_published_twice(self):
        """The copy-paste shape.  Presence cannot see it; a COUNT can."""
        shell = mutate(
            self.shell,
            'setProperty("GSP_PRIV_ERR_ADDR_HI", privErrAddrHi, 32);',
            'setProperty("GSP_PRIV_ERR_ADDR", privErrAddrHi, 32);')
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "property 3"), failures)

    def test_control_readable_flag_as_a_disjunction(self):
        """The form that is FALSE for a verdict added later."""
        shell = mutate(
            self.shell,
            "errVerdict != NVRISCV_ERR_UNREADABLE);",
            "errVerdict == NVRISCV_ERR_NONE\n"
            "                || errVerdict == NVRISCV_ERR_PRIV\n"
            "                || errVerdict == NVRISCV_ERR_HUB\n"
            "                || errVerdict == NVRISCV_ERR_BOTH);")
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "property 4"), failures)

    def test_control_status_words_moved_below_the_unreadable_arm(self):
        """They satisfy property 3's counts either way, and vanish from the one
        arm where their value is the evidence."""
        shell = mutate(
            self.shell,
            '    setProperty("GSP_PRIV_ERR_STAT", privErr, 32);\n'
            '    setProperty("GSP_HUB_ERR_STAT",  hubErr,  32);\n\n'
            "    if (errVerdict == NVRISCV_ERR_UNREADABLE) {",
            "    if (errVerdict == NVRISCV_ERR_UNREADABLE) {")
        shell = mutate(
            shell,
            '    setProperty("GSP_PRIV_ERR_INFO",    privErrInfo,   32);',
            '    setProperty("GSP_PRIV_ERR_STAT", privErr, 32);\n'
            '    setProperty("GSP_HUB_ERR_STAT",  hubErr,  32);\n'
            '    setProperty("GSP_PRIV_ERR_INFO",    privErrInfo,   32);')
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "property 5"), failures)

    def test_control_unreadable_arm_falls_through(self):
        """Deleting the return publishes three words nothing on that path read."""
        shell = mutate(
            self.shell,
            '"consulted and are NOT published");\n        return;\n    }',
            '"consulted and are NOT published");\n    }')
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "property 6"), failures)

    def test_control_localization_word_hoisted_above_the_arm(self):
        shell = mutate(
            self.shell,
            '    setProperty("GSP_PRIV_ERR_STAT", privErr, 32);',
            '    setProperty("GSP_PRIV_ERR_STAT", privErr, 32);\n'
            '    setProperty("GSP_PRIV_ERR_ADDR", privErrAddr, 32);')
        shell = mutate(
            shell,
            '    setProperty("GSP_PRIV_ERR_ADDR",    privErrAddr,   32);\n',
            "")
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "property 7"), failures)

    def test_control_a_localization_read_deleted(self):
        """The pre-2026-08-19 state for one of the three registers."""
        shell = mutate(
            self.shell,
            "gspRiscvRead(NV_PRISCV_RISCV_PRIV_ERR_ADDR_HI)",
            "gspRiscvRead(NV_PRISCV_RISCV_PRIV_ERR_ADDR)")
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "property 8"), failures)

    def test_control_publisher_call_guarded_by_the_fault_arm(self):
        """Depth, not bareness.  3050-handoff35 S3a is the near-miss that makes
        this control worth spelling: a walk-back-to-the-previous-terminator test
        calls this call BARE, because the enclosing block's `{` is itself a
        statement boundary."""
        shell = mutate(
            self.shell,
            "    publishGspErrorLatches(errVerdict, privErr, hubErr,\n"
            "                           privErrInfo, privErrAddr, privErrAddrHi);\n\n"
            "    if (errVerdict == NVRISCV_ERR_UNREADABLE) {",
            "    if (errVerdict == NVRISCV_ERR_UNREADABLE) {\n"
            "        publishGspErrorLatches(errVerdict, privErr, hubErr,\n"
            "                               privErrInfo, privErrAddr, privErrAddrHi);")
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "property 9"), failures)

    def test_control_verdict_derived_twice(self):
        #
        # Anchored on the preceding line as well.  The `} else if (errVerdict !=
        # NVRISCV_ERR_NONE) {` line occurs TWICE in this file -- once in the
        # three-arm log block and once in the not-bootable chain -- and an
        # anchor matching both is intel-handoff26 S3's over-match direction.
        #
        shell = mutate(
            self.shell,
            '              (unsigned)privErr, (unsigned)hubErr);\n'
            "    } else if (errVerdict != NVRISCV_ERR_NONE) {",
            '              (unsigned)privErr, (unsigned)hubErr);\n'
            "    } else if (nvRiscvErrorVerdict(privErr, hubErr) "
            "!= NVRISCV_ERR_NONE) {")
        failures = audit(shell, self.header, self.falcon)
        self.assertTrue(only(failures, "property 9"), failures)


if __name__ == "__main__":
    unittest.main(verbosity=2)
