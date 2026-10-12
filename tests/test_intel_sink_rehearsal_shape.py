#!/usr/bin/env python3
"""Pin the SHAPE of IntelDisplayProbe::publishSinkRehearsal().

Why this module exists
----------------------

`phase0-kext/IntelSinkRehearse.hpp` is pure and `tests/test_intel_sink_rehearse.cpp`
proves it CORRECT. That proof says nothing about whether the IOKit shell that
calls it CONSULTS the verdict before it publishes the composition. The shell
includes IOKit's headers, so no host suite compiles it -- the same by-construction
blindness `tests/test_intel_fb_present_shape.py` was written for. Every
property below was a deletion or a move that left the C++ suites, the register
gate and the syntax gate green.

What is pinned
--------------

P1. `igSinkRehearse()` is called EXACTLY ONCE, and before any `setProperty()`.
    A second call is two compositions behind one verdict; a property published
    ahead of the call publishes a word nothing has composed yet.
P2. `start()` calls `publishSinkRehearsal()` exactly once, AFTER `scanPlanes()`
    and BEFORE `registerService()`: a client matching on the registered service
    must find the keys already there.
P3. The first three keys are the sample verdict, the rehearsal verdict and
    `IGSinkRehearsalWroteRegisters`, all at brace depth 0 (unconditional), and
    the last one is the literal `false`. An absent key is a question, not an
    answer (handoff18 SS1d); a computed "wrote" flag is a claim the shell cannot
    back, because the shell has no write path at all (P5).
P4. Every composed key -- `IGSinkPlan*`, `IGSinkPlane*`,
    `IGSinkRehearsalReservedEnd` -- and every `setProperty()` whose arguments
    read `.plan`, `.regs` or `.reservedEnd` sits AFTER the guard, and the guard
    is the conjunction of two equalities:
        if (!(<sample> == IG_SAMPLE_OK && igSinkRehearseUsable(<verdict>)))
    with a `return` in its block. A NOT_REACHED word published as a base is
    byte-identical at the IORegistry to a planned one; a negation of one error
    arm is true for every other refusal (3050-handoff32 SS3).
P5. No write path: the method body never names `fBar0Base`, `volatile`,
    `igPlaneWriteOffset` or any `regWrite*`, and the pure header names neither
    `volatile` nor `regWrite`. A rehearsal that can write is not a rehearsal.

The composed key SET is spelled, not derived: the keys are an INTERFACE (the
strings an operator greps the IORegistry for), and a set derived from the subject
would ratify a rename. The scan is over comment-stripped source with string
literals masked, because the subject's own comments discuss the guard by name.

Refusal rather than silence
---------------------------

If the subject file, either method body, or the guard cannot be found, the
audit REFUSES with a message naming what was missing (3050-handoff30 SS2b).
"""

import os
import re
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SHELL = os.path.join(ROOT, "phase0-kext", "IntelDisplayProbe.cpp")
PURE_HDR = os.path.join(ROOT, "phase0-kext", "IntelSinkRehearse.hpp")

CLASS = "IntelDisplayProbe"
METHOD = "publishSinkRehearsal"
COMPOSER = "igSinkRehearse"

# The interface. Spelled on purpose -- see the module docstring.
FIRST_KEYS = (
    "IGSinkRehearsalSample",
    "IGSinkRehearsalVerdict",
    "IGSinkRehearsalWroteRegisters",
)
WROTE_KEY = "IGSinkRehearsalWroteRegisters"
COMPOSED_KEYS = (
    "IGSinkRehearsalReservedEnd",
    "IGSinkPlanBase",
    "IGSinkPlanStride",
    "IGSinkPlanBytes",
    "IGSinkPlaneCtl",
    "IGSinkPlaneStride",
    "IGSinkPlanePos",
    "IGSinkPlaneSize",
    "IGSinkPlaneSurf",
)
COMPOSED_KEY_RE = re.compile(r"^(IGSinkPlan|IGSinkPlane|IGSinkRehearsalReservedEnd$)")
COMPOSED_ARG_RE = re.compile(r"\.\s*(plan|regs|reservedEnd)\b")

GUARD_RE = re.compile(
    r"\bif\s*\(\s*!\s*\(\s*(\w+)\s*==\s*IG_SAMPLE_OK\s*&&\s*"
    r"igSinkRehearseUsable\s*\(\s*(\w+)\s*\)\s*\)\s*\)\s*\{"
)

FORBIDDEN_IN_BODY = (
    (re.compile(r"\bfBar0Base\b"), "fBar0Base"),
    (re.compile(r"\bvolatile\b"), "volatile"),
    (re.compile(r"\bigPlaneWriteOffset\b"), "igPlaneWriteOffset"),
    (re.compile(r"\bregWrite\w*"), "regWrite*"),
)
FORBIDDEN_IN_HDR = (
    (re.compile(r"\bvolatile\b"), "volatile"),
    (re.compile(r"\bregWrite\w*"), "regWrite*"),
)


class Refusal(AssertionError):
    """The audit could not find what it measures. Never a pass."""


#
# ---- reading the tree ------------------------------------------------------
#

def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def strip_comments(src):
    """Drop // and /* */ comments; keep string literals VERBATIM.

    Comments inside string literals are not comments, so the walk tracks
    string state. Newlines inside block comments are kept so line numbers in
    messages stay meaningful.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        elif c in "\"'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                i += 1
                if out[-1] == q:
                    break
        else:
            out.append(c)
            i += 1
    return "".join(out)


def mask_strings(code):
    """Same length as `code`, string/char literal CONTENTS replaced by spaces.

    Positions in the masked text are positions in `code`, so a key read out of
    `code` and a token found in the mask can be ordered against each other.
    """
    out = list(code)
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        if c in "\"'":
            q = c
            i += 1
            while i < n and code[i] != q:
                if code[i] == "\\" and i + 1 < n:
                    out[i] = out[i + 1] = " "
                    i += 2
                    continue
                if code[i] != "\n":
                    out[i] = " "
                i += 1
            i += 1
        else:
            i += 1
    return "".join(out)


def body_span(masked, method):
    """(start, end) of the BODY of CLASS::method in `masked`, braces excluded.

    A balanced walk, not a regex (handoff25 SS1c). A declaration-only match
    (a `;` before any `{`) is skipped, so a forward reference is never taken
    for the definition.
    """
    pat = re.compile(r"\b" + re.escape(CLASS) + r"::" + re.escape(method) + r"\s*\(")
    for m in pat.finditer(masked):
        j = m.end()
        depth = 1
        while j < len(masked) and depth:
            if masked[j] == "(":
                depth += 1
            elif masked[j] == ")":
                depth -= 1
            j += 1
        k = j
        while k < len(masked) and masked[k] not in "{;":
            k += 1
        if k >= len(masked) or masked[k] == ";":
            continue
        start = k + 1
        depth = 1
        j = start
        while j < len(masked) and depth:
            if masked[j] == "{":
                depth += 1
            elif masked[j] == "}":
                depth -= 1
            j += 1
        if depth:
            return None
        return (start, j - 1)
    return None


def depth_at(masked, start, pos):
    """Brace depth at `pos`, counted from a body that starts at `start`."""
    d = 0
    for ch in masked[start:pos]:
        if ch == "{":
            d += 1
        elif ch == "}":
            d -= 1
    return d


def call_args(masked, open_paren):
    """Index one past the ')' matching the '(' at `open_paren`."""
    d = 0
    j = open_paren
    while j < len(masked):
        if masked[j] == "(":
            d += 1
        elif masked[j] == ")":
            d -= 1
            if d == 0:
                return j + 1
        j += 1
    return None


SETPROP_RE = re.compile(r"\bsetProperty\s*\(")
KEY_RE = re.compile(r'\s*"([^"\\]*)"\s*,')


def setproperty_calls(code, masked, start, end):
    """[(pos, depth, key or None, args_text)] for every setProperty in a body."""
    calls = []
    for m in SETPROP_RE.finditer(masked, start, end):
        paren = m.end() - 1
        close = call_args(masked, paren)
        if close is None:
            raise Refusal("unbalanced setProperty( at offset %d" % m.start())
        args = code[paren + 1:close - 1]
        km = KEY_RE.match(args)
        calls.append((m.start(), depth_at(masked, start, m.start()),
                      km.group(1) if km else None, args))
    return calls


#
# ---- the audit -------------------------------------------------------------
#

def audit(shell_src=None, hdr_src=None):
    """Return a list of failure strings. Raises Refusal when a subject is missing."""
    if shell_src is None:
        if not os.path.isfile(SHELL):
            raise Refusal("subject missing: %s" % SHELL)
        shell_src = read(SHELL)
    if hdr_src is None:
        if not os.path.isfile(PURE_HDR):
            raise Refusal("pure header missing: %s" % PURE_HDR)
        hdr_src = read(PURE_HDR)

    code = strip_comments(shell_src)
    masked = mask_strings(code)
    fails = []

    span = body_span(masked, METHOD)
    if span is None:
        raise Refusal("%s::%s() definition not found" % (CLASS, METHOD))
    b0, b1 = span
    start_span = body_span(masked, "start")
    if start_span is None:
        raise Refusal("%s::start() definition not found" % CLASS)

    calls = setproperty_calls(code, masked, b0, b1)
    if not calls:
        raise Refusal("%s() publishes nothing -- no setProperty() found" % METHOD)

    # P1 -------------------------------------------------------------------
    composes = [m.start() for m in
                re.finditer(r"\b" + COMPOSER + r"\s*\(", masked[b0:b1])]
    if len(composes) != 1:
        fails.append("P1: %s() called %d times in %s(), expected exactly 1"
                     % (COMPOSER, len(composes), METHOD))
    if composes and calls[0][0] < b0 + composes[0]:
        fails.append("P1: setProperty(%r) precedes the %s() call"
                     % (calls[0][2], COMPOSER))

    # P2 -------------------------------------------------------------------
    s0, s1 = start_span
    sbody = masked[s0:s1]
    pubs = [m.start() for m in re.finditer(r"\b" + METHOD + r"\s*\(\s*\)", sbody)]
    scans = [m.start() for m in re.finditer(r"\bscanPlanes\s*\(", sbody)]
    regs = [m.start() for m in re.finditer(r"\bregisterService\s*\(", sbody)]
    if len(pubs) != 1:
        fails.append("P2: start() calls %s() %d times, expected exactly 1"
                     % (METHOD, len(pubs)))
    elif not scans or not regs:
        fails.append("P2: start() lacks scanPlanes() or registerService() to order against")
    else:
        if pubs[0] < scans[0]:
            fails.append("P2: %s() runs before scanPlanes()" % METHOD)
        if pubs[0] > regs[0]:
            fails.append("P2: %s() runs after registerService()" % METHOD)

    # P3 -------------------------------------------------------------------
    keyed = [c for c in calls if c[2] is not None]
    first = [c[2] for c in keyed[:len(FIRST_KEYS)]]
    if tuple(first) != FIRST_KEYS:
        fails.append("P3: first keys are %r, expected %r" % (first, list(FIRST_KEYS)))
    for key in FIRST_KEYS:
        hits = [c for c in calls if c[2] == key]
        if len(hits) != 1:
            fails.append("P3: %s published %d times, expected exactly 1" % (key, len(hits)))
            continue
        if hits[0][1] != 0:
            fails.append("P3: %s is conditional (brace depth %d)" % (key, hits[0][1]))
        if key == WROTE_KEY:
            value = hits[0][3].split(",", 1)[1].strip() if "," in hits[0][3] else ""
            if value != "false":
                fails.append("P3: %s value is %r, expected the literal false" % (key, value))

    # P4 -------------------------------------------------------------------
    guards = list(GUARD_RE.finditer(masked, b0, b1))
    if len(guards) != 1:
        fails.append("P4: expected exactly one guard of the form "
                     "if (!(<sample> == IG_SAMPLE_OK && igSinkRehearseUsable(<verdict>))) {, found %d"
                     % len(guards))
    else:
        g = guards[0]
        if depth_at(masked, b0, g.start()) != 0:
            fails.append("P4: the guard is itself nested (not at depth 0)")
        gclose = g.end()
        d = 1
        while gclose < b1 and d:
            if masked[gclose] == "{":
                d += 1
            elif masked[gclose] == "}":
                d -= 1
            gclose += 1
        if not re.search(r"\breturn\b", masked[g.end():gclose]):
            fails.append("P4: the guard's block does not return")
        sample_var, verdict_var = g.group(1), g.group(2)
        if not re.search(r"\b" + re.escape(sample_var) + r"\s*=\s*igSampleVerdict\s*\(",
                         masked[b0:g.start()]):
            fails.append("P4: guard tests %r, which is not assigned from igSampleVerdict()"
                         % sample_var)
        if not re.search(r"\b" + re.escape(verdict_var) + r"\s*=\s*" + COMPOSER + r"\s*\(",
                         masked[b0:g.start()]):
            fails.append("P4: guard tests %r, which is not assigned from %s()"
                         % (verdict_var, COMPOSER))
        for pos, _depth, key, args in calls:
            composed = (key is not None and COMPOSED_KEY_RE.match(key)) or \
                COMPOSED_ARG_RE.search(mask_strings(args))
            if composed and pos < g.end():
                fails.append("P4: %s published ahead of the guard" % (key or args.strip()))
    present = set(c[2] for c in calls)
    for key in COMPOSED_KEYS:
        if key not in present:
            fails.append("P4: composed key %s is never published" % key)

    # P5 -------------------------------------------------------------------
    body = masked[b0:b1]
    for rx, name in FORBIDDEN_IN_BODY:
        if rx.search(body):
            fails.append("P5: %s() names %s -- a write path" % (METHOD, name))
    hdr = mask_strings(strip_comments(hdr_src))
    for rx, name in FORBIDDEN_IN_HDR:
        if rx.search(hdr):
            fails.append("P5: IntelSinkRehearse.hpp names %s" % name)

    return fails


#
# ---- tests -----------------------------------------------------------------
#

def mutate(src, old, new):
    count = src.count(old)
    if count != 1:
        raise AssertionError("mutation anchor occurs %d times, expected 1: %r" % (count, old))
    return src.replace(old, new)


class LiveTreeTests(unittest.TestCase):

    def test_live_tree_passes(self):
        self.assertEqual([], audit())

    def test_stripper_keeps_keys_and_drops_prose(self):
        code = strip_comments('a(); // setProperty("X", 1);\nsetProperty("Y", 2);')
        self.assertNotIn('"X"', code)
        self.assertIn('"Y"', code)

    def test_mask_preserves_length(self):
        s = 'f("a{b}", \'}\');'
        self.assertEqual(len(s), len(mask_strings(s)))
        self.assertNotIn("{", mask_strings(s))


class NegativeControlTests(unittest.TestCase):

    def setUp(self):
        self.src = read(SHELL)
        self.hdr = read(PURE_HDR)

    def _fails_with(self, mutated, needle, hdr=None):
        fails = audit(mutated, self.hdr if hdr is None else hdr)
        self.assertTrue(any(needle in f for f in fails),
                        "expected a failure containing %r, got %r" % (needle, fails))

    def test_second_compose_call_reddens_P1(self):
        m = mutate(self.src, "    IgSinkRehearsal rehearsal;\n",
                   "    IgSinkRehearsal rehearsal;\n"
                   "    (void) igSinkRehearse(0, 0, 0, 0, 0, 0, &rehearsal);\n")
        self._fails_with(m, "P1: igSinkRehearse() called 2 times")

    def test_property_ahead_of_the_compose_reddens_P1(self):
        m = mutate(self.src, "    IgSinkRehearsal rehearsal;\n",
                   '    setProperty("IGSinkRehearsalEarly", true);\n'
                   "    IgSinkRehearsal rehearsal;\n")
        self._fails_with(m, "precedes the igSinkRehearse() call")

    def test_dropped_call_in_start_reddens_P2(self):
        m = mutate(self.src, "    publishSinkRehearsal();\n", "")
        self._fails_with(m, "P2: start() calls publishSinkRehearsal() 0 times")

    def test_call_after_register_service_reddens_P2(self):
        m = mutate(self.src, "    publishSinkRehearsal();\n", "")
        m = mutate(m, "    registerService();\n",
                   "    registerService();\n    publishSinkRehearsal();\n")
        self._fails_with(m, "runs after registerService()")

    def test_call_before_scan_reddens_P2(self):
        m = mutate(self.src, "    scanPlanes();\n    publishSinkRehearsal();\n",
                   "    publishSinkRehearsal();\n    scanPlanes();\n")
        self._fails_with(m, "runs before scanPlanes()")

    def test_computed_wrote_flag_reddens_P3(self):
        m = mutate(self.src, '"IGSinkRehearsalWroteRegisters", false)',
                   '"IGSinkRehearsalWroteRegisters", fWrote)')
        self._fails_with(m, "expected the literal false")

    def test_conditional_verdict_reddens_P3(self):
        old = '    setProperty("IGSinkRehearsalVerdict", igSinkRehearseVerdictName(verdict));\n'
        m = mutate(self.src, old, "    if (inSample == IG_SAMPLE_OK) {\n" + old + "    }\n")
        self._fails_with(m, "P3: IGSinkRehearsalVerdict is conditional")

    def test_input_ahead_of_the_verdicts_reddens_P3(self):
        old = '    setProperty("IGSinkRehearsalSample",'
        m = mutate(self.src, old,
                   '    setProperty("IGSinkRehearsalInCtl", (UInt64) inCtl, 32);\n' + old)
        self._fails_with(m, "P3: first keys are")

    def test_composed_key_ahead_of_the_guard_reddens_P4(self):
        m = mutate(self.src, "    if (!(inSample == IG_SAMPLE_OK",
                   '    setProperty("IGSinkPlanBase", (UInt64) 0, 32);\n'
                   "    if (!(inSample == IG_SAMPLE_OK")
        self._fails_with(m, "P4: IGSinkPlanBase published ahead of the guard")

    def test_composed_value_under_another_key_reddens_P4(self):
        m = mutate(self.src, "    if (!(inSample == IG_SAMPLE_OK",
                   '    setProperty("IGSinkRehearsalBase", (UInt64) rehearsal.plan.base, 32);\n'
                   "    if (!(inSample == IG_SAMPLE_OK")
        self._fails_with(m, "IGSinkRehearsalBase published ahead of the guard")

    def test_guard_as_one_negated_arm_reddens_P4(self):
        m = mutate(self.src,
                   "if (!(inSample == IG_SAMPLE_OK && igSinkRehearseUsable(verdict))) {",
                   "if (verdict == IG_SINK_REHEARSE_ALLOC_REFUSED) {")
        self._fails_with(m, "P4: expected exactly one guard")

    def test_guard_losing_the_sample_conjunct_reddens_P4(self):
        m = mutate(self.src,
                   "if (!(inSample == IG_SAMPLE_OK && igSinkRehearseUsable(verdict))) {",
                   "if (!igSinkRehearseUsable(verdict)) {")
        self._fails_with(m, "P4: expected exactly one guard")

    def test_guard_without_return_reddens_P4(self):
        m = mutate(self.src,
                   '        IGLOG("sink rehearsal: publishing the inputs and no composition");\n'
                   "        return;\n",
                   '        IGLOG("sink rehearsal: publishing the inputs and no composition");\n')
        self._fails_with(m, "P4: the guard's block does not return")

    def test_dropped_composed_key_reddens_P4(self):
        m = mutate(self.src,
                   '    setProperty("IGSinkPlanePos",    (UInt64) rehearsal.regs.pos,    32);\n', "")
        self._fails_with(m, "P4: composed key IGSinkPlanePos is never published")

    def test_register_write_reddens_P5(self):
        m = mutate(self.src, "    IgSinkRehearsal rehearsal;\n",
                   "    IgSinkRehearsal rehearsal;\n    regWrite32(0, 0);\n")
        self._fails_with(m, "names regWrite*")

    def test_volatile_store_reddens_P5(self):
        m = mutate(self.src, "    IgSinkRehearsal rehearsal;\n",
                   "    IgSinkRehearsal rehearsal;\n"
                   "    *(volatile UInt32 *) (fBar0Base + 0) = 0;\n")
        self._fails_with(m, "names fBar0Base")
        self._fails_with(m, "names volatile")

    def test_volatile_in_the_pure_header_reddens_P5(self):
        self._fails_with(self.src, "IntelSinkRehearse.hpp names volatile",
                         hdr=self.hdr + "\nstatic volatile int igSinkPoke;\n")

    def test_commented_early_publish_stays_green(self):
        m = mutate(self.src, "    if (!(inSample == IG_SAMPLE_OK",
                   '    // setProperty("IGSinkPlanBase", (UInt64) rehearsal.plan.base, 32);\n'
                   "    if (!(inSample == IG_SAMPLE_OK")
        self.assertEqual([], audit(m, self.hdr))

    def test_renamed_subject_refuses(self):
        m = self.src.replace("IntelDisplayProbe::publishSinkRehearsal(",
                             "IntelDisplayProbe::publishSinkRehearsalV2(")
        with self.assertRaises(Refusal):
            audit(m, self.hdr)

    def test_missing_start_refuses(self):
        m = self.src.replace("IntelDisplayProbe::start(", "IntelDisplayProbe::begin(")
        with self.assertRaises(Refusal):
            audit(m, self.hdr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
