#!/usr/bin/env python3
"""Shape gate over NV3050Probe.cpp's register access refusals.

No C++ suite can execute a line of NV3050Probe.cpp -- it pulls in IOService,
which does not exist off macOS. So the only observer available for the shell
is a structural one: parse the source, assert the properties, and prove the
assertions can fail by mutating the source in memory.

Design rule, carried from test_probe_dead_read_publication.py: the real tree
and every mutant go through ONE audit() function. A gate whose mutants are
checked by different code than its real input is two gates, and only one of
them has ever been seen to fail.

Scanning is done over COMMENT-STRIPPED source. regWrite32()'s own comment
says "Three refusals stand in front of the store", and a gate that counted
prose would pass on a function whose guards had all been deleted.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CPP = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")

# The value both refusals in regRead32() return. Read from the subject's own
# body at :330 and :336 rather than guessed from a sibling accessor -- two
# functions five lines apart do not necessarily agree on a sentinel.
#
# Held as an INT, and every found literal is parsed to an int before comparing.
# The first version of this gate compared spellings via value.upper(), which
# uppercases the 0x prefix too -- "0XFFFFFFFF" never equals "0xFFFFFFFF", so
# the positive control reddened against correct shipping code. A gate that
# compares renderings is measuring the renderer.
READ_REFUSAL_VALUE = 0xFFFFFFFF

#
# Every anchor below was read out of phase0-kext/NV3050Probe.cpp directly
# rather than taken from a sibling function or from a comment. Both roads have
# already produced a check that could not fail in this tree: a sentinel guessed
# from the switch five lines away, and a "three refusals" claim that lives only
# in prose. regWrite32()'s comment at :342 does say "Three refusals" -- the
# count below is measured from the guards at :348, :354 and :360.
#
UNMAPPED_GUARD = "if (!fBar0Base || !fBar0Map) {"
WRITE_GATE = "if (!fAllowGspWrites) {"
READ_STORE = "OSReadLittleInt32"
WRITE_STORE = "OSWriteLittleInt32"
CACHE_FLAG = "bar->map(kIOMapInhibitCache)"
SHORT_BAR_TEST = "if (length < NV_BAR0_EXPECTED_LENGTH) {"
IFDEF_OPEN = "#ifdef PHASE4_ALLOW_WRITES"

# Assignment, not comparison, and deliberately not matching the USE at :953
# (`io.allowWrites = fAllowGspWrites;`) where the identifier is on the right.
ASSIGN_RE = re.compile(r"\bfAllowGspWrites\s*=(?!=)")

# The whole condition of any `if` that consults the mapping length, CAPTURED so
# the width term can be inspected rather than merely matched.
BOUNDS_RE = re.compile(r"if\s*\(([^;{]*?fBar0Map->getLength\(\))\s*\)\s*\{")

BARE_RETURN_RE = re.compile(r"\breturn\s*;")

FUNCTIONS = (
    ("regRead32", "NV3050Probe::regRead32("),
    ("regWrite32", "NV3050Probe::regWrite32("),
    ("mapBar0", "NV3050Probe::mapBar0("),
    ("init", "NV3050Probe::init("),
)


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """Blank out C and C++ comments, preserving string literals and offsets."""
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
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def function_body(text, signature_fragment):
    """Return the brace-balanced body of the function whose signature contains
    signature_fragment, or None if it cannot be found."""
    start = text.find(signature_fragment)
    if start < 0:
        return None
    open_brace = text.find("{", start)
    if open_brace < 0:
        return None
    depth = 0
    i = open_brace
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace : i + 1]
        i += 1
    return None


def _balanced(text, open_index, opener="{", closer="}"):
    """Index of the closer matching the opener at open_index, or -1."""
    depth = 0
    i = open_index
    while i < len(text):
        if text[i] == opener:
            depth += 1
        elif text[i] == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _accessor_failures(name, body, store):
    """The guards regRead32() and regWrite32() share: mapped, in range, and a
    single dereference standing BEHIND both of them."""
    out = []

    guards = body.count(UNMAPPED_GUARD)
    if guards != 1:
        out.append("%s: expected exactly one unmapped-BAR guard %r, found %d"
                   % (name, UNMAPPED_GUARD, guards))

    bounds = BOUNDS_RE.search(body)
    if bounds is None:
        out.append("%s: no bounds guard compares an offset against getLength()"
                   % name)
    elif "sizeof(UInt32)" not in bounds.group(1):
        out.append(
            "%s: bounds guard %r carries no sizeof(UInt32) term -- an offset at "
            "length-1 passes every naive test and the access still runs three "
            "bytes past the end of the mapping" % (name, bounds.group(1).strip()))

    stores = len(re.findall(r"\b%s\s*\(" % store, body))
    if stores != 1:
        out.append("%s: expected exactly one %s, found %d" % (name, store, stores))
    else:
        at = body.index(store)
        if bounds is not None and bounds.start() > at:
            out.append("%s: %s runs BEFORE the bounds guard" % (name, store))
        if UNMAPPED_GUARD in body and body.index(UNMAPPED_GUARD) > at:
            out.append("%s: %s runs BEFORE the unmapped-BAR guard" % (name, store))

    return out


def audit(cpp_raw):
    """Return a list of failure reasons. Empty list means every property holds.

    Reasons are strings so negative controls can assert WHICH property broke,
    not merely that something did.
    """
    failures = []
    src = strip_comments(cpp_raw)

    bodies = {}
    for name, fragment in FUNCTIONS:
        body = function_body(src, fragment)
        if body is None:
            failures.append("%s: body not found (signature %r)" % (name, fragment))
        bodies[name] = body

    #
    # Whole-file: the two guarded accessors are the ONLY MMIO path in the shell.
    # A second OSWriteLittleInt32 anywhere is a store standing behind none of the
    # refusals, and no other gate in this tree would see it.
    #
    for store in (READ_STORE, WRITE_STORE):
        n = len(re.findall(r"\b%s\s*\(" % store, src))
        if n != 1:
            failures.append(
                "%s: occurs %d times in the file, expected 1 -- the guarded "
                "accessors are the only permitted MMIO path" % (store, n))

    # ---- property 1: regRead32 refuses twice, then dereferences once --------
    read = bodies["regRead32"]
    if read is not None:
        failures.extend(_accessor_failures("regRead32", read, READ_STORE))

        refusals = re.findall(r"return\s+(0x[0-9A-Fa-f]+)\s*;", read)
        if len(refusals) != 2:
            failures.append("regRead32: expected 2 sentinel returns, found %d"
                            % len(refusals))
        for value in refusals:
            if int(value, 16) != READ_REFUSAL_VALUE:
                failures.append("regRead32: refusal returns %s, not 0x%08X"
                                % (value, READ_REFUSAL_VALUE))

    # ---- property 2: regWrite32 refuses three times, then stores once -------
    write = bodies["regWrite32"]
    if write is not None:
        failures.extend(_accessor_failures("regWrite32", write, WRITE_STORE))

        n = len(BARE_RETURN_RE.findall(write))
        if n != 3:
            failures.append(
                "regWrite32: expected 3 bare returns (unmapped, out of range, "
                "build gate), found %d" % n)

        if write.count(WRITE_GATE) != 1:
            failures.append(
                "regWrite32: missing the %r build gate -- without it a stock "
                "read-only build can store to the GPU" % WRITE_GATE)
        elif WRITE_STORE in write and write.index(WRITE_GATE) > write.index(WRITE_STORE):
            failures.append("regWrite32: the build gate runs AFTER the store")

    # ---- property 3: a stock build physically cannot open the write gate ----
    assigns = len(ASSIGN_RE.findall(src))
    if assigns != 2:
        failures.append(
            "fAllowGspWrites: assigned %d times in the file, expected 2. Both "
            "must sit inside init()'s %s block, or a stock build can reach true"
            % (assigns, IFDEF_OPEN))

    init = bodies["init"]
    if init is not None:
        in_init = len(ASSIGN_RE.findall(init))
        if in_init != assigns:
            failures.append(
                "fAllowGspWrites: %d of %d assignments sit outside init()"
                % (assigns - in_init, assigns))

        start = init.find(IFDEF_OPEN)
        end = init.find("#endif", start) if start >= 0 else -1
        if start < 0:
            failures.append("init: no %r around the write gate" % IFDEF_OPEN)
        elif end < 0:
            failures.append("init: %r is never closed by #endif" % IFDEF_OPEN)
        else:
            region = init[start:end]
            inside = len(ASSIGN_RE.findall(region))
            if inside != 2:
                failures.append(
                    "fAllowGspWrites: %d assignments sit inside the %s block, "
                    "expected 2" % (inside, IFDEF_OPEN))
            arms = region.split("#else")
            if "fAllowGspWrites = true;" not in arms[0]:
                failures.append(
                    "init: the #ifdef arm does not set fAllowGspWrites true")
            if len(arms) < 2 or "fAllowGspWrites = false;" not in arms[1]:
                failures.append(
                    "init: the #else arm does not set fAllowGspWrites false")

    # ---- property 4: mapBar0 WARNS about a short BAR, it does not refuse ----
    mapped = bodies["mapBar0"]
    if mapped is not None:
        if mapped.count(CACHE_FLAG) != 1:
            failures.append(
                "mapBar0: expected exactly one %r -- a cached MMIO mapping "
                "leaves writes in the cache line and returns stale reads, which "
                "looks like a GPU ignoring you rather than a mapping bug"
                % CACHE_FLAG)

        if mapped.count(SHORT_BAR_TEST) != 1:
            failures.append("mapBar0: no %r comparison" % SHORT_BAR_TEST)
        else:
            brace = mapped.index(SHORT_BAR_TEST) + len(SHORT_BAR_TEST) - 1
            close = _balanced(mapped, brace)
            branch = mapped[brace : close + 1] if close > 0 else ""
            if re.search(r"\breturn\b", branch):
                failures.append(
                    "mapBar0: the short-BAR branch returns. It is a WARNING by "
                    "design -- declining to attach deletes the IORegistry node "
                    "that makes a short BAR inspectable in the first place")

        if not mapped.rstrip().rstrip("}").rstrip().endswith("return true;"):
            failures.append("mapBar0: does not end in `return true;`")

    return failures


def mutate(text, old, new, occurrences=1):
    if text.count(old) != occurrences:
        raise AssertionError(
            "mutation anchor %r occurs %d times, expected %d"
            % (old, text.count(old), occurrences)
        )
    return text.replace(old, new)


class RegisterAccessRefusalTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.cpp = read_text(CPP)

    def test_live_tree_passes(self):
        """Positive control: the shipping shell satisfies every property."""
        self.assertEqual(audit(self.cpp), [])

    def test_dropping_sizeof_term_is_caught(self):
        """offset < getLength() lets an offset at length-1 read 3 bytes past
        the end of the mapping. The sizeof term is the whole guard."""
        mutant = mutate(
            self.cpp,
            "(IOByteCount)offset + sizeof(UInt32) > fBar0Map->getLength()",
            "(IOByteCount)offset > fBar0Map->getLength()",
            occurrences=2,
        )
        reasons = audit(mutant)
        hits = [r for r in reasons if "carries no sizeof(UInt32) term" in r]
        # Both accessors carry the guard, so both must redden. An assertion
        # satisfied by one of two would pass on a half-applied mutation.
        self.assertEqual(
            len(hits), 2,
            "expected both accessors to redden on the width term, got %r"
            % (reasons,),
        )

    def test_deleting_unmapped_guard_is_caught(self):
        mutant = mutate(
            self.cpp,
            "if (!fBar0Base || !fBar0Map) {",
            "if (false) {",
            occurrences=2,
        )
        reasons = audit(mutant)
        hits = [r for r in reasons if "unmapped-BAR guard" in r]
        self.assertEqual(
            len(hits), 2,
            "expected both accessors to redden on the mapped guard, got %r"
            % (reasons,),
        )

    def test_second_mmio_store_is_caught(self):
        """A store outside the guarded accessor stands behind none of the three
        refusals. Nothing else in this tree can see one."""
        mutant = mutate(
            self.cpp,
            "OSWriteLittleInt32(fBar0Base, offset, value);",
            "OSWriteLittleInt32(fBar0Base, offset, value);\n    "
            "OSWriteLittleInt32(fBar0Base, offset + 4, value);",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("only permitted MMIO path" in r for r in reasons),
            "expected the whole-file MMIO-path check to redden, got %r" % (reasons,),
        )

    def test_deleting_the_write_build_gate_is_caught(self):
        """Without it a stock build with no -DPHASE4_ALLOW_WRITES can store to
        the GPU, which is the one thing this driver promises it cannot do."""
        mutant = mutate(self.cpp, WRITE_GATE, "if (false) {")
        reasons = audit(mutant)
        self.assertTrue(
            any("build gate" in r for r in reasons),
            "expected the build-gate check to redden, got %r" % (reasons,),
        )

    def test_else_arm_enabling_writes_is_caught(self):
        """The #else arm is what a stock build compiles. Flipping it to true
        leaves the assignment COUNT at two, so only an arm-aware check sees it."""
        mutant = mutate(self.cpp, "fAllowGspWrites = false;",
                        "fAllowGspWrites = true;")
        reasons = audit(mutant)
        self.assertTrue(
            any("#else arm" in r for r in reasons),
            "expected the #else arm check to redden, got %r" % (reasons,),
        )

    def test_assignment_outside_init_is_caught(self):
        mutant = mutate(
            self.cpp,
            "IOByteCount length = fBar0Map->getLength();",
            "fAllowGspWrites = true;\n    "
            "IOByteCount length = fBar0Map->getLength();",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("sit outside init()" in r for r in reasons),
            "expected an outside-init failure, got %r" % (reasons,),
        )

    def test_short_bar_branch_returning_is_caught(self):
        """handoff22 §1 is the Intel version of this exact call, and the reason
        it is a WARNING: declining to attach deletes the IORegistry node that
        makes a short BAR inspectable in the first place."""
        mutant = mutate(self.cpp, SHORT_BAR_TEST, SHORT_BAR_TEST + " return false;")
        reasons = audit(mutant)
        self.assertTrue(
            any("short-BAR branch returns" in r for r in reasons),
            "expected the short-BAR check to redden, got %r" % (reasons,),
        )

    def test_cached_mapping_is_caught(self):
        mutant = mutate(self.cpp, CACHE_FLAG, "bar->map(0)")
        reasons = audit(mutant)
        self.assertTrue(
            any("cached MMIO mapping" in r for r in reasons),
            "expected the cache-flag check to redden, got %r" % (reasons,),
        )

    def test_dereference_before_the_guards_is_caught(self):
        """Two mutations, because moving the read must not also duplicate it --
        a second store would redden the COUNT check and prove nothing about
        ordering."""
        mutant = mutate(
            self.cpp,
            "    if (!fBar0Base || !fBar0Map) {\n"
            "        NVLOG(\"regRead32: BAR0 not mapped\");\n"
            "        return 0xFFFFFFFF;\n"
            "    }",
            "    UInt32 early = OSReadLittleInt32(fBar0Base, offset);\n"
            "    if (!fBar0Base || !fBar0Map) {\n"
            "        NVLOG(\"regRead32: BAR0 not mapped\");\n"
            "        return 0xFFFFFFFF;\n"
            "    }",
        )
        mutant = mutate(mutant, "    return OSReadLittleInt32(fBar0Base, offset);",
                        "    return early;")
        reasons = audit(mutant)
        self.assertTrue(
            any("runs BEFORE the unmapped-BAR guard" in r for r in reasons),
            "expected an ordering failure, got %r" % (reasons,),
        )
        self.assertTrue(
            any("runs BEFORE the bounds guard" in r for r in reasons),
            "expected an ordering failure against the bounds guard, got %r"
            % (reasons,),
        )

    def test_audit_refuses_when_a_subject_is_absent(self):
        """A gate that cannot find its subject must REFUSE, never pass over an
        empty string. This is the failure mode that makes a shape gate vacuous."""
        mutant = mutate(self.cpp, "NV3050Probe::regWrite32(",
                        "NV3050Probe::regWriteThirtyTwo(")
        reasons = audit(mutant)
        self.assertTrue(
            any(r.startswith("regWrite32: body not found") for r in reasons),
            "expected a refusal, got %r" % (reasons,),
        )


if __name__ == "__main__":
    unittest.main()
