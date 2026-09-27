#!/usr/bin/env python3
"""Shape gate over attemptGspBoot()'s result switch -- the LOG half.

Why this exists
---------------
``3050-handoff39.md`` section 3 split one result code into two.
``nvGspBootCheckRiscvErrors()`` used to stop on ``nvRiscvHasError()``, a bool
spelled ``nvRiscvErrorVerdict(priv, hub) != NVRISCV_ERR_NONE`` -- so ``true``
had FOUR preimages and a boot halted because one half answered with the
``0xFFFFFFFF`` refusal value was reported as a latched card fault. The fix gives
the refusal its own code::

    if (m->errVerdict == NVRISCV_ERR_UNREADABLE) {
        nvGspBootStop(m, NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE);
        return false;
    }
    if (nvRiscvErrorVerdictHasFault(m->errVerdict)) {
        nvGspBootStop(m, NVGSP_RESULT_ERR_RISCV_FAULT);
        return false;
    }

Section 8 of that handoff closed the PUBLICATION half: the guarded
``NVGspBootErrVerdict`` key is pinned by
``tests/test_probe_gsp_boot_publication.py`` property 6, guard and conjunction
both. This module is the half section 8 did NOT close, carried as section 9
item 2.

The gap
-------
``attemptGspBoot()`` renders each terminal result to ``NVLOG`` through a
``switch (machine.result)``, and the two RISC-V arms are where the whole point
of the split lives:

* the FAULT arm says the latches are a fact about the CARD and an engine reset
  is the action;
* the UNREADABLE arm says the stop happened WITHOUT a fault having been
  established and names THIS DRIVER as the place to look.

Nothing requires those arms to keep saying that. Delete either one and the
result falls through to ``default:``, which prints a generic line and exits
zero on every gate:

* the register gate parses ``#define``s and these are enum members;
* both C++ suites and the syntax gate cannot reach it -- the switch lives in
  ``NV3050Probe.cpp``, an IOKit translation unit no host suite compiles, and
  clang has nothing to say about a ``case`` that is merely absent;
* the Python gates model publication, never log rendering.

So an edit that swaps the two arms' bodies, or merges them back into one,
restores the exact confusion section 3 removed -- with all five gates green.
That is ``3050-handoff30.md`` section 1's shape a third time: a distinction
that exists in the code and has no observer.

What is deliberately NOT asserted
---------------------------------
**Not** "every stop code has an arm." Measured before writing: fourteen codes
reach ``nvGspBootStop()`` and the switch carries nine arms, so five --
``ERR_DMA_TIMEOUT``, ``ERR_FIRMWARE_INVALID``, ``ERR_NOT_RISCV_CAPABLE``,
``ERR_NO_IO``, ``ERR_STEP_BUDGET`` -- legitimately fall to ``default:``. A
coverage gate spelled that way reddens correct shipping code, and its failing
test would be the POSITIVE CONTROL. Third session running where the first
draft would have made that mistake; the set-difference was run first, which is
the only reason it was not made here.

The subject is therefore the PAIR the split created, not the family.

What IS asserted
----------------
1. Both arms exist, exactly once each.
2. Each arm's own log text carries the reading its result code means -- the
   fault arm claims the CARD, the refusal arm claims THIS DRIVER -- and the two
   readings are asserted DISJOINT, so swapping the bodies reddens.
3. The fault arm renders the verdict through ``nvRiscvErrorVerdictName()``.
   The verdict is what distinguishes PRIV from HUB from BOTH, and those are the
   three preimages the fault code still legitimately covers.
4. The header's two stop sites and the shell's two arms are the SAME set, and
   that set is DERIVED from the header rather than spelled here -- a third
   RISC-V result code added later with no arm reddens on the commit that adds
   it.
5. The switch still has a ``default:``. Without one, deleting an arm is a
   compile error on macOS and this gate would be redundant; with one, deletion
   is silent, which is why every assertion above is needed. Asserting it keeps
   the module honest about its own premise.

Scanning is over COMMENT-STRIPPED source. The switch's own comment block
explains the CARD/DRIVER distinction in those words, so a prose-reading gate
would pass a switch whose arms had been merged -- verbatim
``3050-handoff30.md`` section 2a's reason, one file over.

A gate that cannot find its subject REFUSES rather than passing: reporting
nothing is indistinguishable from reporting green.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CPP_PATH = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
HPP_PATH = os.path.join(ROOT, "phase0-kext", "NVGspBoot.hpp")

SUBJECT_FN = "bool NV3050Probe::attemptGspBoot"
CHECK_FN = "bool nvGspBootCheckRiscvErrors"

FAULT_CODE = "NVGSP_RESULT_ERR_RISCV_FAULT"
UNREADABLE_CODE = "NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE"

VERDICT_NAME_FN = "nvRiscvErrorVerdictName"

#
# The two readings, as the arms themselves spell them.
#
# Held as lowercase substrings of the arm's own log text rather than as whole
# sentences: the wording is prose and will be edited, but a fault arm that stops
# claiming the CARD -- or a refusal arm that stops naming THIS DRIVER -- is the
# defect this module exists to catch.
#
# Each marker is asserted PRESENT in its own arm and ABSENT from its sibling.
# Presence alone passes a switch whose two bodies were swapped, because both
# markers would still occur somewhere in the switch.
#
FAULT_MARKERS = ("about the card", "engine reset")
UNREADABLE_MARKERS = ("without a fault", "this driver")

STOP_RE = re.compile(r"nvGspBootStop\s*\(\s*\w+\s*,\s*(NVGSP_RESULT_\w+)\s*\)")
RISCV_CODE_RE = re.compile(r"^NVGSP_RESULT_ERR_RISCV_\w*$")


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """Remove C and C++ comments, preserving newlines and string literals.

    Newlines survive so that line-oriented anchors keep working and so that two
    statements separated only by a comment do not become adjacent. String
    literals are preserved because the log text IS the subject here.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == '"' or ch == "'":
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and nxt == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def join_wrapped_literals(text):
    """Concatenate adjacent string literals split across a line break.

    The subject is the SENTENCE an operator reads, and the compiler forms it by
    concatenating adjacent literals::

        NVLOG("... an engine "
              "reset is the action.", ...)

    A gate scanning raw source sees ``an engine "`` and ``"reset is`` as
    separate text, so a marker straddling the wrap is absent from a file that
    plainly contains the sentence -- and, worse, re-wrapping the line to fit a
    column limit would redden a gate that had nothing to say about the change.
    Joining first makes every assertion below a property of the message rather
    than of its typography.

    Only a close-quote at end of line against an open-quote at start of the
    next is joined; a comma, paren or anything else between them is left alone.
    """
    return re.sub(r'"[ \t]*\n[ \t]*"', "", text)


def _balanced(text, open_index, opener="{", closer="}"):
    """Index just past the closer matching the opener at open_index."""
    depth = 0
    i = open_index
    while i < len(text):
        if text[i] == opener:
            depth += 1
        elif text[i] == closer:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return -1


def function_body(text, signature_fragment):
    """Body of the function whose signature contains the fragment, or None."""
    start = text.find(signature_fragment)
    if start < 0:
        return None
    brace = text.find("{", start)
    if brace < 0:
        return None
    end = _balanced(text, brace)
    if end < 0:
        return None
    return text[brace + 1:end - 1]


def header_riscv_stop_codes(hpp_raw):
    """The RISC-V error codes nvGspBootCheckRiscvErrors() actually stops on.

    DERIVED from the header's own stop sites rather than spelled here, for
    test_probe_gsp_boot_publication property 5's reason: a third RISC-V code
    added to that function with no matching switch arm must redden on the
    commit that adds it, naming the code.

    Returns None when the function cannot be located -- refuse rather than pass.
    """
    hpp = strip_comments(hpp_raw)
    body = function_body(hpp, CHECK_FN)
    if body is None:
        return None
    return frozenset(code for code in STOP_RE.findall(body)
                     if RISCV_CODE_RE.match(code))


def arm_bodies(cpp_raw):
    """Map result code -> that case arm's text, for every arm in the switch.

    An arm runs from its own ``case <CODE>:`` to the next ``case``/``default``
    at the same level. Found by scanning the subject function's body rather
    than the whole file, so an unrelated switch elsewhere cannot contribute.

    Returns None when the subject function cannot be located.
    """
    cpp = join_wrapped_literals(strip_comments(cpp_raw))
    body = function_body(cpp, SUBJECT_FN)
    if body is None:
        return None

    labels = []
    for match in re.finditer(r"\bcase\s+(NVGSP_RESULT_\w+)\s*:|\bdefault\s*:",
                             body):
        labels.append((match.start(), match.end(), match.group(1)))

    arms = {}
    for index, (_, end, code) in enumerate(labels):
        if code is None:
            continue
        stop = labels[index + 1][0] if index + 1 < len(labels) else len(body)
        arms.setdefault(code, []).append(body[end:stop])
    return arms


def audit(cpp_raw, hpp_raw):
    """Return a list of failure strings; empty means the shape holds."""
    failures = []

    arms = arm_bodies(cpp_raw)
    if arms is None:
        return ["NV3050Probe.cpp: %s() not found -- the gate could not read "
                "the switch, so it has measured nothing" % SUBJECT_FN]

    stop_codes = header_riscv_stop_codes(hpp_raw)
    if stop_codes is None:
        failures.append(
            "NVGspBoot.hpp: %s() not found -- the gate could not derive the "
            "RISC-V result codes, so it has measured nothing" % CHECK_FN)
    elif stop_codes != {FAULT_CODE, UNREADABLE_CODE}:
        failures.append(
            "NVGspBoot.hpp: %s() stops on %s, this gate knows how to check %s "
            "-- a RISC-V result code was added or removed and its log arm "
            "needs a reading of its own"
            % (CHECK_FN, sorted(stop_codes),
               sorted([FAULT_CODE, UNREADABLE_CODE])))

    # ---- property 1: both arms exist, exactly once ------------------------

    for code in (FAULT_CODE, UNREADABLE_CODE):
        found = arms.get(code, [])
        if not found:
            failures.append(
                "attemptGspBoot: no `case %s:` arm -- that result now falls to "
                "default: and prints a generic line, which is the collapse "
                "handoff39 section 3 split apart" % code)
        elif len(found) > 1:
            failures.append(
                "attemptGspBoot: %d `case %s:` arms -- a second arm is "
                "unreachable and can disagree with the first"
                % (len(found), code))

    # ---- property 2: each arm carries its OWN reading, disjointly ---------

    readings = (
        (FAULT_CODE, FAULT_MARKERS, UNREADABLE_MARKERS,
         "a fact about the CARD"),
        (UNREADABLE_CODE, UNREADABLE_MARKERS, FAULT_MARKERS,
         "a bug in THIS DRIVER"),
    )

    for code, own, foreign, description in readings:
        found = arms.get(code, [])
        if len(found) != 1:
            continue
        text = found[0].lower()

        for marker in own:
            if marker not in text:
                failures.append(
                    "attemptGspBoot: the %s arm no longer says %r -- its whole "
                    "job is reporting %s, and an operator acts on that sentence"
                    % (code, marker, description))

        for marker in foreign:
            if marker in text:
                failures.append(
                    "attemptGspBoot: the %s arm says %r, which belongs to its "
                    "sibling -- the two arms report opposite things and this "
                    "one is now claiming the other's" % (code, marker))

    # ---- property 3: the fault arm renders the verdict --------------------

    fault = arms.get(FAULT_CODE, [])
    if len(fault) == 1 and VERDICT_NAME_FN not in fault[0]:
        failures.append(
            "attemptGspBoot: the %s arm does not call %s() -- FAULT still "
            "covers PRIV, HUB and BOTH, and the verdict is the only thing that "
            "tells those three apart" % (FAULT_CODE, VERDICT_NAME_FN))

    # ---- property 5: the switch still has a default -----------------------
    #
    # This module's premise. With a default:, deleting an arm is silent and
    # every assertion above is load-bearing. Without one, the compiler would
    # catch a deleted arm and this gate would be redundant -- worth knowing
    # either way, so the premise is asserted rather than assumed.
    #
    cpp_body = function_body(strip_comments(cpp_raw), SUBJECT_FN)
    if cpp_body is not None and not re.search(r"\bdefault\s*:", cpp_body):
        failures.append(
            "attemptGspBoot: the result switch has no default: -- this gate "
            "assumes a deleted arm is silent; without a default the compiler "
            "catches it and these checks describe the wrong world")

    return failures


def mutate(text, old, new, occurrences=1):
    """Replace and assert the anchor matched exactly as many times as expected.

    A mutation matching nothing prints ok; one matching more than it means
    raises loudly and looks like the mutation is wrong when the precondition is
    working. Both directions are caught here.
    """
    count = text.count(old)
    if count != occurrences:
        raise AssertionError(
            "mutation anchor matched %d times, expected %d: %r"
            % (count, occurrences, old[:80]))
    return text.replace(old, new)


class GspResultLogArmTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.cpp = read_text(CPP_PATH)
        cls.hpp = read_text(HPP_PATH)

    # ---- positive control -------------------------------------------------

    def test_live_tree_passes(self):
        failures = audit(self.cpp, self.hpp)
        self.assertEqual([], failures, "\n".join(failures))

    # ---- the subject is actually found ------------------------------------

    def test_both_arms_are_located(self):
        arms = arm_bodies(self.cpp)
        self.assertIsNotNone(arms)
        self.assertIn(FAULT_CODE, arms)
        self.assertIn(UNREADABLE_CODE, arms)

    def test_the_stop_code_set_is_derived_from_the_header(self):
        codes = header_riscv_stop_codes(self.hpp)
        self.assertEqual({FAULT_CODE, UNREADABLE_CODE}, set(codes))

    # ---- negative controls, each naming WHICH property broke --------------

    def test_deleting_the_fault_arm_is_caught(self):
        broken = mutate(self.cpp,
                        "    case NVGSP_RESULT_ERR_RISCV_FAULT:",
                        "    case NVGSP_RESULT_ERR_DMA_TIMEOUT:")
        failures = audit(broken, self.hpp)
        self.assertTrue(any("no `case %s:` arm" % FAULT_CODE in f
                            for f in failures), failures)

    def test_deleting_the_unreadable_arm_is_caught(self):
        broken = mutate(self.cpp,
                        "    case NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE:",
                        "    case NVGSP_RESULT_ERR_STEP_BUDGET:")
        failures = audit(broken, self.hpp)
        self.assertTrue(any("no `case %s:` arm" % UNREADABLE_CODE in f
                            for f in failures), failures)

    def test_the_fault_arm_losing_its_card_reading_is_caught(self):
        broken = mutate(
            self.cpp,
            '              "diagnostics GSP has. This is a fact about the '
            'CARD -- an engine "\n'
            '              "reset is the action.",',
            '              "diagnostics GSP has. The boot did not finish.",')
        failures = audit(broken, self.hpp)
        self.assertTrue(any("no longer says" in f and FAULT_CODE in f
                            for f in failures), failures)

    def test_the_unreadable_arm_losing_its_driver_reading_is_caught(self):
        broken = mutate(
            self.cpp,
            '              "stopped WITHOUT a fault having been established. '
            'That is a bug "\n'
            '              "in THIS driver (mapping or offset), not a fault '
            'in the card.",',
            '              "stopped early. Something is wrong.",')
        failures = audit(broken, self.hpp)
        self.assertTrue(any("no longer says" in f and UNREADABLE_CODE in f
                            for f in failures), failures)

    def test_the_unreadable_arm_claiming_a_card_fault_is_caught(self):
        """The defect handoff39 section 3 removed, restored in the log only.

        This is the sharp one: the result CODES stay split and the guarded
        publication stays correct, so property 6 of the publication gate is
        green -- while the sentence an operator actually reads sends them
        looking for a card problem over a mapping bug.
        """
        broken = mutate(
            self.cpp,
            '              "in THIS driver (mapping or offset), not a fault '
            'in the card.",',
            '              "in THIS driver (mapping or offset). This is a '
            'fact about the "\n'
            '              "CARD -- an engine reset is the action.",')
        failures = audit(broken, self.hpp)
        self.assertTrue(any("belongs to its sibling" in f and
                            UNREADABLE_CODE in f for f in failures), failures)

    def test_the_fault_arm_dropping_the_verdict_is_caught(self):
        broken = mutate(self.cpp,
                        "              nvRiscvErrorVerdictName(machine.errVerdict));",
                        "              \"faulted\");")
        failures = audit(broken, self.hpp)
        self.assertTrue(any("does not call %s()" % VERDICT_NAME_FN in f
                            for f in failures), failures)

    def test_a_third_riscv_stop_code_reddens(self):
        """The derived-set arm: a new code with no arm must be named.

        Spelled as a third stop site in the header rather than as a change to
        this module, which is the whole point of deriving the set.
        """
        broken = mutate(self.hpp,
                        "        nvGspBootStop(m, NVGSP_RESULT_ERR_RISCV_FAULT);",
                        "        nvGspBootStop(m, NVGSP_RESULT_ERR_RISCV_WEDGED);\n"
                        "        nvGspBootStop(m, NVGSP_RESULT_ERR_RISCV_FAULT);")
        failures = audit(self.cpp, broken)
        self.assertTrue(any("a RISC-V result code was added" in f
                            for f in failures), failures)

    # ---- vacuity controls: a gate that cannot find its subject REFUSES ----

    def test_a_renamed_subject_function_refuses_rather_than_passes(self):
        #
        # Renamed to a name the fragment is NOT a prefix of. A SUFFIXED rename
        # -- attemptGspBootRenamed -- still CONTAINS "attemptGspBoot", so
        # find() locates it and the control passes for the wrong reason,
        # reporting a vacuity arm as working when it never ran.
        #
        broken = mutate(self.cpp,
                        "bool NV3050Probe::attemptGspBoot(void)",
                        "bool NV3050Probe::beginGspBringup(void)")
        failures = audit(broken, self.hpp)
        self.assertTrue(any("not found" in f and "measured nothing" in f
                            for f in failures), failures)

    def test_a_renamed_check_function_refuses_rather_than_passes(self):
        broken = mutate(self.hpp,
                        "static inline bool nvGspBootCheckRiscvErrors(NVGspBoot *m)",
                        "static inline bool nvGspBootScanRiscvLatches(NVGspBoot *m)")
        failures = audit(self.cpp, broken)
        self.assertTrue(any("not found" in f and "measured nothing" in f
                            for f in failures), failures)


if __name__ == "__main__":
    unittest.main()
