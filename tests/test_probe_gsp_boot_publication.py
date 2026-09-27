#!/usr/bin/env python3
"""Shape gate over NV3050Probe.cpp's publishGspBootResult().

Why this exists
---------------
Until 2026-08-18 ``publishGspBootResult()`` published five diagnostic register
words in five bare ``setProperty()`` calls::

    NVGspBootCpuctl  NVGspBootHwcfg2  NVGspBootRiscvCpuctl
    NVGspBootPrivErrStat  NVGspBootHubErrStat

while ``nvGspBootInit()`` zero-initialises all five and each is assigned ONLY
inside the state that reads it. A run that stops at PROBE therefore put three
initialiser zeros into the IORegistry, and ``setProperty()`` with a literal zero
is byte-identical to ``setProperty()`` with a register that genuinely read
all-clear. handoff18 S1d's class, fifth occurrence, fourth in this file.

The separating case is the one worth remembering: ACTIVE_WAIT stops on
NVGSP_RESULT_RUNNING -- the SUCCESS terminal -- BEFORE
``nvGspBootCheckRiscvErrors()`` is ever called, so the HEALTHY boot is exactly
the path that fabricated two of the five words.

The fix was ``readMask`` plus ``nvGspBootWasRead()``. 3050-handoff29.md S8 item 1
is the hole that fix left: ``publishGspBootResult()`` lives in the IOKit shell,
so **no C++ suite executes a line of it** -- delete the five guards and all four
gates stay green. The C++ suite proves ``readMask`` is correct; nothing proved
the publisher CONSUMES it. This module is that proof, by parse.

What it pins
------------
1. The publisher refuses a null machine before any dereference.
2. ``NVGspBootReadMask`` is published, unconditionally, and FIRST -- above every
   word it qualifies, so its absence cannot be read as an older kext.
3. Every one of the five words is published ONLY inside an
   ``if (nvGspBootWasRead(machine, <bit>))`` guard -- never bare.
4. Each guard's BIT matches the key and the struct field it publishes. This is
   the copy-paste shape: a duplicated bit makes two words share one flag and the
   publisher emits an unmeasured value whenever its twin has been read.
5. The set of bits the publisher guards on EQUALS the set of NVGSP_READ_*
   enumerators in NVGspBoot.hpp. Derived, not spelled -- a sixth bit added to
   the header with no publication reddens here, which is the tripwire.

Scanning is over COMMENT-STRIPPED source. Mandatory, and for the same reason as
``tests/test_probe_dead_read_publication.py``: the fix's own comment block quotes
the defect it repaired and names every key and every field, so a gate reading
prose would pass on a publisher whose guards had all been deleted.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CPP = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")
HPP = os.path.join(ROOT, "phase0-kext", "NVGspBoot.hpp")

PUBLISHER = "void NV3050Probe::publishGspBootResult"
MASK_KEY = "NVGspBootReadMask"
MASK_FIELD = "readMask"
GUARD_FN = "nvGspBootWasRead"

#
# The DECODE of the error-latch pair, added 2026-08-19 beside the five words.
#
# It is not a member of TRIPLES because it is not guarded by ONE bit. Its guard
# is nvGspBootErrVerdictKnown(), the CONJUNCTION of both latch bits, and the
# conjunction is the whole property: the refusal value arrives through a read
# that SUCCEEDED, so no run this machine can produce reads one latch without the
# other and a single-bit guard would be green over every test in the suite.
#
# Guarded at all -- unlike the mask, which is published on every path -- because
# the field's initialiser NVRISCV_ERR_NONE is ALSO the healthy reading. An
# unguarded publication reports a run that stopped at PROBE as one that checked
# both latches and found them clear. Same fabricated-publication class as the
# five words, one level up: the fabricated thing is a DECODE rather than a
# register word.
#
VERDICT_KEY = "NVGspBootErrVerdict"
VERDICT_FIELD = "errVerdict"
VERDICT_GUARD_FN = "nvGspBootErrVerdictKnown"
VERDICT_NAME_FN = "nvRiscvErrorVerdictName"
VERDICT_REQUIRED_BITS = frozenset(("NVGSP_READ_PRIV_ERR_STAT",
                                   "NVGSP_READ_HUB_ERR_STAT"))

#
# The bit / field / key triples were read out of the two subject files directly:
# the BIT and the FIELD from each assignment site in NVGspBoot.hpp, the KEY from
# the setProperty() call in NV3050Probe.cpp. Not guessed from a sibling
# publisher -- handoff21 S1b is what that costs.
#
# The SET of bits is not trusted from this table; property 5 derives it from the
# header's own enum and compares. The table only fixes the pairing.
#
TRIPLES = (
    ("NVGSP_READ_CPUCTL", "cpuctl", "NVGspBootCpuctl"),
    ("NVGSP_READ_HWCFG2", "hwcfg2", "NVGspBootHwcfg2"),
    ("NVGSP_READ_RISCV_CPUCTL", "riscvCpuctl", "NVGspBootRiscvCpuctl"),
    ("NVGSP_READ_PRIV_ERR_STAT", "privErrStat", "NVGspBootPrivErrStat"),
    ("NVGSP_READ_HUB_ERR_STAT", "hubErrStat", "NVGspBootHubErrStat"),
)

# if (nvGspBootWasRead(<obj>, <BIT>))
#     setProperty("<KEY>", (UInt64)<obj>-><field>, 32);
#
# Spelled structurally rather than with a negated character class: a class that
# excludes only the statement terminator runs straight past the if's closing
# paren into the next statement, which is handoff25 S1c.
GUARDED_RE = re.compile(
    r"if\s*\(\s*" + GUARD_FN + r"\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*\)\s*"
    r"setProperty\s*\(\s*\"(\w+)\"\s*,\s*\(\s*UInt64\s*\)\s*(\w+)\s*->\s*(\w+)\s*,"
)

ANY_PROPERTY_RE = re.compile(r"setProperty\s*\(\s*\"(\w+)\"")
NULL_REFUSAL_RE = re.compile(r"^\s*if\s*\(\s*!\s*machine\s*\)\s*return\s*;")
ENUM_BIT_RE = re.compile(r"^\s*(NVGSP_READ_\w+)\s*=", re.MULTILINE)

#
# Captures at every compared point, never a class excluding the terminator --
# handoff25 section 1c is the cost of the other spelling. The guard OBJECT and
# the value OBJECT are separate captures so a guard consulting one machine while
# publishing from another is visible rather than merely well-formed.
#
ENUM_TOKEN_RE = re.compile(r"\bNVGSP_READ_\w+\b")

VERDICT_GUARDED_RE = re.compile(
    r"if\s*\(\s*" + VERDICT_GUARD_FN + r"\s*\(\s*(\w+)\s*\)\s*\)\s*"
    r"setProperty\s*\(\s*\"(\w+)\"\s*,\s*(\w+)\s*\(\s*(\w+)\s*->\s*(\w+)\s*\)\s*\)\s*;"
)


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """Remove C and C++ comments, preserving newlines and string literals.

    Newlines survive so that line-oriented anchors keep working and so that two
    statements separated only by a comment do not become adjacent.
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


def header_read_bits(hpp_raw):
    """The NVGSP_READ_* enumerator names declared in NVGspBoot.hpp."""
    return set(ENUM_BIT_RE.findall(strip_comments(hpp_raw)))


def verdict_guard_bits(hpp_raw):
    """Return the NVGSP_READ_* bits nvGspBootErrVerdictKnown() actually reads.

    Derived from the header rather than spelled here, for the reason property 5
    derives its bit set: a guard weakened to one bit must redden on the commit
    that weakens it, and a table in this file would have to be edited to notice.
    Returns None when the function cannot be located -- refuse rather than pass.
    """
    hpp = strip_comments(hpp_raw)
    body = function_body(hpp, "bool " + VERDICT_GUARD_FN)
    if body is None:
        return None
    return frozenset(ENUM_TOKEN_RE.findall(body))


def audit(cpp_raw, hpp_raw):
    """Return a list of failure strings; empty means the shape holds.

    Shared with the live tree -- every test in this module drives this one
    function, so a control that passes is a control that exercised the same code
    the positive control does.
    """
    failures = []

    cpp = strip_comments(cpp_raw)
    body = function_body(cpp, PUBLISHER)

    if body is None:
        # Refuse rather than pass. A gate that cannot find its subject reports
        # nothing, and reporting nothing is indistinguishable from green.
        return ["publishGspBootResult: subject not found in %s -- the gate could "
                "not locate the publisher, so it has measured nothing" % CPP]

    # ---- property 1: a null machine is refused before any dereference -------
    if NULL_REFUSAL_RE.search(body) is None:
        failures.append(
            "publishGspBootResult: does not refuse a null machine -- the mask "
            "publication dereferences machine unconditionally")
    else:
        refusal_end = NULL_REFUSAL_RE.search(body).end()
        first_property = ANY_PROPERTY_RE.search(body)
        if first_property is not None and first_property.start() < refusal_end:
            failures.append(
                "publishGspBootResult: publishes before the null-machine "
                "refusal")

    # ---- property 2: the mask is published, unconditionally, and FIRST ------
    mask_calls = [m for m in ANY_PROPERTY_RE.finditer(body)
                  if m.group(1) == MASK_KEY]
    if not mask_calls:
        failures.append(
            "publishGspBootResult: %s is never published -- its absence would "
            "then be indistinguishable from an older kext" % MASK_KEY)
    elif len(mask_calls) > 1:
        failures.append(
            "publishGspBootResult: %s is published %d times; expected exactly "
            "one unconditional call" % (MASK_KEY, len(mask_calls)))

    guarded = list(GUARDED_RE.finditer(body))
    guarded_spans = [(m.start(), m.end()) for m in guarded]

    def inside_a_guard(index):
        return any(lo <= index < hi for lo, hi in guarded_spans)

    if mask_calls:
        mask_at = mask_calls[0].start()

        if inside_a_guard(mask_at):
            failures.append(
                "publishGspBootResult: %s is published behind a guard -- it "
                "must be published unconditionally, on every path" % MASK_KEY)
        else:
            # Walk back to the start of this statement and refuse any branch.
            cut = max(body.rfind(";", 0, mask_at),
                      body.rfind("{", 0, mask_at),
                      body.rfind("}", 0, mask_at))
            prefix = body[cut + 1:mask_at]
            if re.search(r"\b(if|else|while|for)\b", prefix):
                failures.append(
                    "publishGspBootResult: %s is published conditionally -- it "
                    "must be published unconditionally, on every path"
                    % MASK_KEY)

        # The verdict is qualified by the mask exactly as the five words are --
        # nvGspBootErrVerdictKnown() reads two of its bits -- so it belongs in
        # the FIRST check's subject set rather than only in property 6.
        word_keys = set(key for _, _, key in TRIPLES) | {VERDICT_KEY}
        word_positions = [m.start() for m in ANY_PROPERTY_RE.finditer(body)
                          if m.group(1) in word_keys]
        if word_positions and mask_at > min(word_positions):
            failures.append(
                "publishGspBootResult: %s is not published FIRST -- it must "
                "sort above every word it qualifies" % MASK_KEY)

    # ---- property 3: no diagnostic word is published bare -------------------
    for bit, field, key in TRIPLES:
        occurrences = [m for m in ANY_PROPERTY_RE.finditer(body)
                       if m.group(1) == key]
        if not occurrences:
            failures.append(
                "publishGspBootResult: %s is never published" % key)
            continue
        for occurrence in occurrences:
            if not inside_a_guard(occurrence.start()):
                failures.append(
                    "publishGspBootResult: %s is published without a "
                    "%s() guard -- an unread word reaches the IORegistry as an "
                    "initialiser zero" % (key, GUARD_FN))

    # ---- property 4: bit, key and field agree in every guard ----------------
    by_key = {}
    for match in guarded:
        guard_obj, bit, key, value_obj, field = match.groups()
        by_key.setdefault(key, []).append((bit, field, guard_obj, value_obj))

    for bit, field, key in TRIPLES:
        found = by_key.get(key)
        if not found:
            # Already reported as bare or missing by property 3.
            continue
        for got_bit, got_field, guard_obj, value_obj in found:
            if got_bit != bit:
                failures.append(
                    "publishGspBootResult: %s is guarded by the wrong bit -- "
                    "got %s, expected %s" % (key, got_bit, bit))
            if got_field != field:
                failures.append(
                    "publishGspBootResult: %s publishes the wrong field -- "
                    "got %s, expected %s" % (key, got_field, field))
            if guard_obj != value_obj:
                failures.append(
                    "publishGspBootResult: %s guards on %s but publishes from "
                    "%s" % (key, guard_obj, value_obj))

    guarded_bits = [m.group(2) for m in guarded]
    duplicates = sorted(set(b for b in guarded_bits
                            if guarded_bits.count(b) > 1))
    for bit in duplicates:
        failures.append(
            "publishGspBootResult: %s guards more than one word -- two words "
            "sharing one flag publishes an unmeasured value whenever its twin "
            "has been read" % bit)

    # ---- property 5: the guarded bit set EQUALS the header's own enum -------
    declared = header_read_bits(hpp_raw)
    if not declared:
        failures.append(
            "NVGspBoot.hpp: no NVGSP_READ_* enumerators found -- the gate could "
            "not derive the bit set, so it has measured nothing")
    else:
        used = set(guarded_bits)
        for missing in sorted(declared - used):
            failures.append(
                "publishGspBootResult: %s is declared in NVGspBoot.hpp but "
                "guards nothing here -- a bit with no publication" % missing)
        for extra in sorted(used - declared):
            failures.append(
                "publishGspBootResult: guards on %s, which NVGspBoot.hpp does "
                "not declare" % extra)

    # ---- property 6: the DECODE is guarded by the CONJUNCTION of both bits ---
    #
    # Deleting this guard leaves properties 1-5 fully green: the verdict is not
    # a member of TRIPLES, so nothing above this line has an opinion about it.
    # That hole is why this property exists -- it is the one the fix that added
    # NVGspBootErrVerdict left behind on the day it landed.
    #
    verdict_publications = [m for m in ANY_PROPERTY_RE.finditer(body)
                            if m.group(1) == VERDICT_KEY]
    if not verdict_publications:
        failures.append(
            "publishGspBootResult: %s is never published -- the decode of the "
            "error-latch pair reaches the log and not the IORegistry"
            % VERDICT_KEY)
    elif len(verdict_publications) > 1:
        failures.append(
            "publishGspBootResult: %s is published %d times -- a second "
            "publication can be the unguarded one"
            % (VERDICT_KEY, len(verdict_publications)))
    else:
        found = VERDICT_GUARDED_RE.findall(body)
        if not found:
            failures.append(
                "publishGspBootResult: %s is published without an "
                "%s() guard -- its initialiser NVRISCV_ERR_NONE is also the "
                "HEALTHY reading, so an unguarded publication reports a run "
                "that never reached the error check as one that checked and "
                "found nothing" % (VERDICT_KEY, VERDICT_GUARD_FN))
        else:
            for guard_obj, got_key, name_fn, value_obj, got_field in found:
                if got_key != VERDICT_KEY:
                    failures.append(
                        "publishGspBootResult: the %s() guard protects %s, not "
                        "%s" % (VERDICT_GUARD_FN, got_key, VERDICT_KEY))
                if name_fn != VERDICT_NAME_FN:
                    failures.append(
                        "publishGspBootResult: %s is rendered by %s, not %s -- "
                        "a verdict published as a bare number is a number "
                        "nobody can read" % (VERDICT_KEY, name_fn,
                                             VERDICT_NAME_FN))
                if got_field != VERDICT_FIELD:
                    failures.append(
                        "publishGspBootResult: %s publishes the wrong field -- "
                        "got %s, expected %s"
                        % (VERDICT_KEY, got_field, VERDICT_FIELD))
                if guard_obj != value_obj:
                    failures.append(
                        "publishGspBootResult: %s guards on %s but publishes "
                        "from %s" % (VERDICT_KEY, guard_obj, value_obj))

    #
    # And the guard itself must still be the CONJUNCTION. This half is read out
    # of NVGspBoot.hpp, because a publisher calling a correctly-named guard that
    # has been weakened to one bit is byte-identical here to a correct one -- and
    # the machine cannot separate the two bits at runtime, so no C++ test in the
    # suite reddens on that weakening either.
    #
    guard_bits = verdict_guard_bits(hpp_raw)
    if guard_bits is None:
        failures.append(
            "NVGspBoot.hpp: %s() not found -- the gate could not read the "
            "guard, so it has measured nothing" % VERDICT_GUARD_FN)
    elif guard_bits != set(VERDICT_REQUIRED_BITS):
        failures.append(
            "NVGspBoot.hpp: %s() consults %s, expected %s -- the verdict is a "
            "decode of BOTH latches, so either bit alone leaves one half at its "
            "initialiser"
            % (VERDICT_GUARD_FN,
               ", ".join(sorted(guard_bits)) or "nothing",
               ", ".join(sorted(VERDICT_REQUIRED_BITS))))

    return failures


def mutate(text, old, new, occurrences=1):
    """Replace old with new, asserting how many times it was found.

    A mutation that matches nothing is a no-op, and a no-op negative control
    passes for the wrong reason -- so the count is checked, not assumed.
    """
    count = text.count(old)
    if count != occurrences:
        raise AssertionError(
            "mutation anchor occurs %d times, expected %d: %r"
            % (count, occurrences, old[:80]))
    return text.replace(old, new)


MASK_LINE = '    setProperty("NVGspBootReadMask", (UInt64)machine->readMask, 32);\n'

CPUCTL_PAIR = (
    "    if (nvGspBootWasRead(machine, NVGSP_READ_CPUCTL))\n"
    '        setProperty("NVGspBootCpuctl",      (UInt64)machine->cpuctl,      32);\n'
)

HWCFG2_PAIR = (
    "    if (nvGspBootWasRead(machine, NVGSP_READ_HWCFG2))\n"
    '        setProperty("NVGspBootHwcfg2",      (UInt64)machine->hwcfg2,      32);\n'
)

RISCV_PAIR = (
    "    if (nvGspBootWasRead(machine, NVGSP_READ_RISCV_CPUCTL))\n"
    '        setProperty("NVGspBootRiscvCpuctl", (UInt64)machine->riscvCpuctl, 32);\n'
)

HUB_PAIR = (
    "    if (nvGspBootWasRead(machine, NVGSP_READ_HUB_ERR_STAT))\n"
    '        setProperty("NVGspBootHubErrStat",  (UInt64)machine->hubErrStat,  32);\n'
)

NULL_REFUSAL = "    if (!machine)\n        return;\n"


class GspBootPublicationTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.cpp = read_text(CPP)
        cls.hpp = read_text(HPP)

    # ---- the subject is present and parseable ------------------------------

    def test_the_publisher_is_found(self):
        body = function_body(strip_comments(self.cpp), PUBLISHER)
        self.assertIsNotNone(
            body, "publishGspBootResult() not found -- the anchor moved")

    def test_the_header_declares_five_read_bits(self):
        declared = header_read_bits(self.hpp)
        self.assertEqual(
            declared, set(bit for bit, _, _ in TRIPLES),
            "the NVGSP_READ_* family changed; the triple table needs the same "
            "edit this gate is here to force")

    def test_comment_stripping_removes_the_fixs_own_prose(self):
        # The repair comment names every key and every field. If prose survived,
        # a publisher with all five guards deleted would still look correct.
        body = function_body(strip_comments(self.cpp), PUBLISHER)
        self.assertNotIn("handoff18 section 1d", body)
        self.assertNotIn("byte-identical", body)

    # ---- positive control ---------------------------------------------------

    def test_live_tree_passes(self):
        failures = audit(self.cpp, self.hpp)
        self.assertEqual([], failures, "\n".join(failures))

    # ---- negative controls, each naming WHICH property broke ----------------

    def test_a_bare_word_is_caught(self):
        broken = mutate(
            self.cpp, CPUCTL_PAIR,
            '    setProperty("NVGspBootCpuctl", (UInt64)machine->cpuctl, 32);\n')
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NVGspBootCpuctl is published without" in f for f in failures),
            failures)

    def test_deleting_every_guard_is_caught(self):
        broken = self.cpp
        for bit, field, key in TRIPLES:
            broken = re.sub(
                r"if \(nvGspBootWasRead\(machine, %s\)\)\n *" % bit, "", broken)
        failures = audit(broken, self.hpp)
        for _, _, key in TRIPLES:
            self.assertTrue(
                any(("%s is published without" % key) in f for f in failures),
                "%s not reported: %s" % (key, failures))

    # ---- property 6 controls: the verdict decode --------------------------
    #
    # These are the ones that matter for this extension. Every control above
    # stays green under all four of them, because the verdict is not a member of
    # TRIPLES -- which is precisely the hole this property closes.
    #
    def test_deleting_the_verdict_guard_is_caught(self):
        # The exact regression the fix that added NVGspBootErrVerdict left
        # available: an unguarded publication reports a run that stopped at
        # PROBE as one that checked both latches and found them clear.
        broken = mutate(
            self.cpp,
            "    if (nvGspBootErrVerdictKnown(machine))\n"
            "        setProperty(\"NVGspBootErrVerdict\",\n",
            "    setProperty(\"NVGspBootErrVerdict\",\n")
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NVGspBootErrVerdict is published without an" in f
                for f in failures),
            failures)

    def test_deleting_the_verdict_publication_is_caught(self):
        # The other direction: the decode stays in the log and never reaches
        # the IORegistry at all, which is the class this whole family exists
        # to catch and reads as a green publisher.
        broken = mutate(
            self.cpp,
            "    if (nvGspBootErrVerdictKnown(machine))\n"
            "        setProperty(\"NVGspBootErrVerdict\",\n"
            "                    nvRiscvErrorVerdictName(machine->errVerdict));\n",
            "")
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NVGspBootErrVerdict is never published" in f
                for f in failures),
            failures)

    def test_a_verdict_published_as_a_bare_number_is_caught(self):
        # nvRiscvErrorVerdictName() is what makes the key readable. A raw
        # UInt64 publishes 0x4 where UNREADABLE belongs -- correct arithmetic,
        # and unreadable by the one person the key exists for.
        broken = mutate(
            self.cpp,
            "                    nvRiscvErrorVerdictName(machine->errVerdict));",
            "                    nvGspBootResultName(machine->errVerdict));")
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("is rendered by nvGspBootResultName" in f for f in failures),
            failures)

    def test_weakening_the_conjunction_to_one_bit_is_caught(self):
        # The half no C++ test in the suite can reach: the refusal VALUE
        # arrives through a read that SUCCEEDED, so no run this machine can
        # produce reads one latch without the other. A single-bit guard is
        # green over every check in test_gsp_boot.cpp.
        broken_hpp = mutate(
            self.hpp,
            "    return nvGspBootWasRead(m, NVGSP_READ_PRIV_ERR_STAT)\n"
            "        && nvGspBootWasRead(m, NVGSP_READ_HUB_ERR_STAT);\n",
            "    return nvGspBootWasRead(m, NVGSP_READ_PRIV_ERR_STAT);\n")
        failures = audit(self.cpp, broken_hpp)
        self.assertTrue(
            any("consults NVGSP_READ_PRIV_ERR_STAT, expected" in f
                for f in failures),
            failures)

    def test_a_renamed_verdict_guard_refuses_rather_than_passes(self):
        # A gate that cannot find its subject must say so. Renaming the guard
        # in the header alone leaves the publisher calling a function that no
        # longer exists -- the gate must not read that as satisfied.
        broken_hpp = mutate(
            self.hpp,
            "static inline bool nvGspBootErrVerdictKnown(const NVGspBoot *m)",
            "static inline bool nvGspBootVerdictIsKnown(const NVGspBoot *m)")
        failures = audit(self.cpp, broken_hpp)
        self.assertTrue(
            any("nvGspBootErrVerdictKnown() not found" in f for f in failures),
            failures)

    def test_a_duplicated_bit_is_caught(self):
        # The copy-paste shape: HWCFG2's guard left spelling CPUCTL's bit.
        broken = mutate(
            self.cpp,
            "    if (nvGspBootWasRead(machine, NVGSP_READ_HWCFG2))\n",
            "    if (nvGspBootWasRead(machine, NVGSP_READ_CPUCTL))\n")
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NVGspBootHwcfg2 is guarded by the wrong bit" in f
                for f in failures), failures)
        self.assertTrue(
            any("NVGSP_READ_CPUCTL guards more than one word" in f
                for f in failures), failures)
        self.assertTrue(
            any("NVGSP_READ_HWCFG2 is declared" in f for f in failures),
            failures)

    def test_publishing_the_wrong_field_is_caught(self):
        broken = mutate(
            self.cpp,
            '        setProperty("NVGspBootHubErrStat",  (UInt64)machine->hubErrStat,  32);',
            '        setProperty("NVGspBootHubErrStat",  (UInt64)machine->privErrStat,  32);')
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NVGspBootHubErrStat publishes the wrong field" in f
                for f in failures), failures)

    def test_a_missing_mask_is_caught(self):
        broken = mutate(self.cpp, MASK_LINE, "")
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NVGspBootReadMask is never published" in f for f in failures),
            failures)

    def test_a_guarded_mask_is_caught(self):
        broken = mutate(
            self.cpp, MASK_LINE,
            "    if (machine->readMask)\n" + MASK_LINE)
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NVGspBootReadMask is published conditionally" in f
                for f in failures), failures)

    def test_a_mask_published_last_is_caught(self):
        broken = mutate(self.cpp, MASK_LINE, "")
        broken = mutate(broken, HUB_PAIR, HUB_PAIR + MASK_LINE)
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NVGspBootReadMask is not published FIRST" in f
                for f in failures), failures)

    def test_a_deleted_word_is_caught(self):
        broken = mutate(self.cpp, RISCV_PAIR, "")
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("NVGspBootRiscvCpuctl is never published" in f
                for f in failures), failures)
        self.assertTrue(
            any("NVGSP_READ_RISCV_CPUCTL is declared" in f for f in failures),
            failures)

    def test_a_dropped_null_refusal_is_caught(self):
        broken = mutate(self.cpp, NULL_REFUSAL, "")
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("does not refuse a null machine" in f for f in failures),
            failures)

    def test_a_bit_declared_but_never_published_is_caught(self):
        # The tripwire: a sixth bit lands in the header and nothing publishes it.
        broken_hpp = mutate(
            self.hpp,
            "    NVGSP_READ_HUB_ERR_STAT  = 0x10\n",
            "    NVGSP_READ_HUB_ERR_STAT  = 0x10,\n"
            "    NVGSP_READ_INVENTED      = 0x20\n")
        failures = audit(self.cpp, broken_hpp)
        self.assertTrue(
            any("NVGSP_READ_INVENTED is declared" in f for f in failures),
            failures)

    def test_a_missing_subject_refuses_rather_than_passes(self):
        broken = mutate(self.cpp, PUBLISHER, "void NV3050Probe::renamedAway")
        failures = audit(broken, self.hpp)
        self.assertTrue(
            any("subject not found" in f for f in failures), failures)

    def test_a_missing_enum_refuses_rather_than_passes(self):
        failures = audit(self.cpp, "// nothing here\n")
        self.assertTrue(
            any("no NVGSP_READ_* enumerators found" in f for f in failures),
            failures)


if __name__ == "__main__":
    unittest.main(verbosity=2)
