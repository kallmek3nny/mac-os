#!/usr/bin/env python3
"""Shape gate over NV3050Probe.cpp's identity publication path.

Why this exists
---------------
``readIdentity()`` reads NV_PMC_BOOT_0 first and returns early when the value
looks like a non-decoding read. Until 2026-08-18 that early return did this::

    if (outBoot42)
        *outBoot42 = boot0;
    return false;

-- handing the caller a value for a register it had NOT read. ``start()`` then
discarded the false return entirely and called ``publishIdentity(boot0,
boot42)`` unconditionally, so NV_PMC_BOOT_42 was published into the IORegistry
as a measurement that never happened.

That is invisible from the outside. ``setProperty("NV_PMC_BOOT_42", boot0, 32)``
is BYTE-IDENTICAL in IORegistryExplorer to a register that genuinely returned
that value, so an operator staring at two equal all-ones words cannot tell a
card that answered from an aperture that never did. Same class as handoff18
S1d one level up in ``inspectGspEngine()``.

No existing gate can see it: the register gate checks constants, the C++ suites
cannot link a translation unit that reaches IOService, and clang is perfectly
happy with a fabricated value of the correct type.

Design rule, carried from test_probe_dead_read_publication.py and
test_probe_register_access_refusals.py: the real tree and every mutant go
through ONE audit() function. A gate whose mutants are checked by different
code than its real input is two gates, and only one of them has ever been seen
to fail.

Scanning is done over COMMENT-STRIPPED source. The fix's own comment quotes the
defective assignment it replaced (``*outBoot42 = boot0``), so a gate that
counted prose would find the defect it exists to forbid and redden a pristine
tree. That is handoff18 S1d's lesson, learned the hard way in that module.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CPP = os.path.join(ROOT, "phase0-kext", "NV3050Probe.cpp")

#
# Every anchor below was read out of phase0-kext/NV3050Probe.cpp directly rather
# than guessed from a sibling publisher. publishGspUnreadable() is the same
# SHAPE one level up but does not share a single string with this path, and a
# sentinel guessed from a sibling five hundred lines away is the mistake
# handoff21 S1b records.
#
BOOT42_KEY = "NV_PMC_BOOT_42"
BOOT0_KEY = "NV_PMC_BOOT_0"
READABLE_KEY = "NVIdentityReadable"
DEAD_READ_TEST = "nvLooksLikeDeadRead(boot0)"

#
# Added 2026-08-18 with property 5. decodeBoot42() has always had an arm for
# "BOOT_0 answered but BOOT_42 came back dead" -- it logs the case and returns --
# while publishIdentity() put the word into the IORegistry regardless. These
# three anchors were read out of phase0-kext/NV3050Probe.cpp directly.
#
BOOT42_READABLE_KEY = "NVBoot42Readable"
BOOT42_GUARD = "boot42Readable"
BOOT42_GUARD_DERIVATION = "!nvLooksLikeDeadRead(boot42)"

#
# Added 2026-08-18 with property 6. decodeBoot0() has always detected four
# distinct facts about the part -- a non-Ampere architecture, an Ada part
# specifically (the 'RTX 3050 A' case, where the whole plan retargets), GA107
# confirmed, and an Ampere part that is not GA107 -- and has always written all
# four to the LOG only. publishIdentity() derived a bare NAME from the same
# word, so "Ada" reached the IORegistry byte-identically to "Ampere" with
# nothing saying that one of those two means no constant in this driver applies.
# That is 3050-handoff31.md S1's class again: a diagnosis reaching the log and
# not the consumer.
#
# All four anchors were read out of phase0-kext/NV3050Probe.cpp and
# phase0-kext/NVDecode.hpp directly.
#
PART_VERDICT_KEY = "NVPartVerdict"
PART_TARGET_KEY = "NVPartIsTarget"
PART_IMPL_KEY = "NVImplementation"
PART_VERDICT_DERIVATION = "nvPartVerdict(boot0)"
PART_TARGET_ENUMERATOR = "NV_PART_TARGET"

#
# The equality that decides NVPartIsTarget. Spelled as a pattern rather than a
# literal because the whitespace is not the claim -- but the RELATION is: an
# EQUALITY against the target enumerator, never a negation of one of the error
# arms. A negation of NV_PART_ADA is true for every non-Ada part including a
# Turing one, which would report a part this driver has never seen as the part
# it was written for.
#
PART_TARGET_EQUALITY_RE = re.compile(
    r'setProperty\s*\(\s*"' + PART_TARGET_KEY + r'"\s*,\s*'
    r'part\s*==\s*' + PART_TARGET_ENUMERATOR)

FUNCTIONS = (
    ("start", "NV3050Probe::start("),
    ("readIdentity", "NV3050Probe::readIdentity("),
    ("publishIdentity", "NV3050Probe::publishIdentity("),
    ("publishIdentityUnreadable", "NV3050Probe::publishIdentityUnreadable("),
)

# Assignment through the out-parameter, not a comparison. The defect this gate
# forbids is exactly `*outBoot42 = <anything>` inside the dead-read branch.
BOOT42_STORE_RE = re.compile(r"\*\s*outBoot42\s*=(?!=)")

# setProperty("<key>" ... -- the key is captured so a publication can be
# attributed to the function it lives in.
SETPROP_RE = re.compile(r'setProperty\s*\(\s*"([^"]+)"')


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
    signature_fragment, or None if it cannot be found.

    None rather than "" on every failure path: an empty string satisfies every
    'does not contain' assertion in this file, which is exactly how a shape gate
    goes vacuous.
    """
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


def dead_read_branch(body):
    """The brace-matched body of readIdentity()'s `if (nvLooksLikeDeadRead(...))`
    block, or None if the test or its brace cannot be located."""
    at = body.find(DEAD_READ_TEST)
    if at < 0:
        return None
    brace = body.find("{", at)
    if brace < 0:
        return None
    close = _balanced(body, brace)
    if close < 0:
        return None
    return body[brace : close + 1]


def guarded_span(body, condition):
    """Half-open span of the block guarded by ``if (<condition>) {``, or None.

    Structural: the opening brace is located and matched by a balanced walk.
    NOT a character class excluding the statement terminator -- handoff25 S1c
    records what that costs, because such a class runs straight past the if's
    closing paren and the whole guarded body to land on the next statement.
    """
    marker = re.search(r"if\s*\(\s*%s\s*\)\s*\{" % re.escape(condition), body)
    if marker is None:
        return None
    brace = body.index("{", marker.start())
    end = _balanced(body, brace)
    if end < 0:
        return None
    return (brace, end)


def setprop_sites(body, key):
    """Start offsets of every setProperty() call on `key` within `body`."""
    pattern = r'setProperty\s*\(\s*"%s"' % re.escape(key)
    return [m.start() for m in re.finditer(pattern, body)]


def statement_is_bare(body, at):
    """True when the statement starting at `at` is not the body of an if/else.

    Decided by walking BACK to the previous statement boundary -- a ``;``, ``{``
    or ``}`` -- and reading what sits between it and `at`. That span is the only
    place a guard can hide: an ``if (...)`` with no brace puts its condition
    there, and a braced ``if`` puts the ``{`` immediately before it with the
    condition just above.

    Deliberately NOT a regex over the preceding line. handoff25 S1c is the cost
    of a character class that excludes only the statement terminator: it runs
    past the if's closing paren, the newline and the whole guarded body to land
    on the next statement's semicolon, and then reports a guarded call as bare.

    A boundary of ``{`` needs one more step, because it is ambiguous -- it opens
    a function body (bare) or an if/else block (guarded). The text before that
    brace decides it.
    """
    i = at - 1
    while i >= 0 and body[i] not in ";{}":
        i -= 1

    if i < 0:
        return True

    boundary = body[i]
    between = body[i + 1:at]

    if "if" in between or "else" in between:
        return False

    if boundary != "{":
        return True

    #
    # The boundary is an opening brace. Walk back over the whitespace before it
    # and read the head: a ``)`` there belongs to an ``if``/``for``/``while``
    # condition, so this statement is the first in a guarded block.
    #
    j = i - 1
    while j >= 0 and body[j].isspace():
        j -= 1

    if j < 0:
        return True

    if body[j] != ")":
        # ``else {`` has no parenthesis; catch it by name.
        head = body[max(0, j - 8):j + 1]
        return "else" not in head

    open_paren = _balanced_back(body, j, "(", ")")
    if open_paren < 0:
        return True

    k = open_paren - 1
    while k >= 0 and body[k].isspace():
        k -= 1

    head = body[max(0, k - 8):k + 1]
    return not (head.endswith("if") or head.endswith("else if"))


def _balanced_back(text, close_index, opener="(", closer=")"):
    """Index of the opener matching the closer at close_index, or -1."""
    depth = 0
    i = close_index
    while i >= 0:
        if text[i] == closer:
            depth += 1
        elif text[i] == opener:
            depth -= 1
            if depth == 0:
                return i
        i -= 1
    return -1


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

    # ---- property 1: the dead-read branch invents no BOOT_42 measurement ----
    read = bodies["readIdentity"]
    if read is not None:
        branch = dead_read_branch(read)
        if branch is None:
            failures.append(
                "readIdentity: no brace-matched %r branch -- the dead-read early "
                "return is what the rest of this gate is about" % DEAD_READ_TEST)
        else:
            stores = BOOT42_STORE_RE.findall(branch)
            if stores:
                failures.append(
                    "readIdentity: the dead-read branch STORES through outBoot42 "
                    "(%d assignment(s)). BOOT_42 is never read on that path, so "
                    "any value handed back is fabricated and indistinguishable "
                    "in the IORegistry from a real read" % len(stores))

            if "return false;" not in branch:
                failures.append(
                    "readIdentity: the dead-read branch does not `return false;` "
                    "-- the false return is the ONLY signal the caller has")

        # The live path must still fill it, or the out-parameter is pointless.
        if BOOT42_STORE_RE.search(read) is None:
            failures.append(
                "readIdentity: nothing ever stores through outBoot42 -- the live "
                "path must still publish a real BOOT_42")

    # ---- property 2: start() BRANCHES on readIdentity()'s return value ------
    start = bodies["start"]
    if start is not None:
        if "if (readIdentity(" not in start:
            failures.append(
                "start: readIdentity()'s return value is not tested. Discarding "
                "it publishes the dead-read path through publishIdentity()")

        if "publishIdentityUnreadable(" not in start:
            failures.append(
                "start: never calls publishIdentityUnreadable() -- there is no "
                "honest publication path for a dead aperture")

        if "publishIdentity(" not in start:
            failures.append("start: never calls publishIdentity()")

    # ---- property 3: the unreadable publisher fabricates no BOOT_42 --------
    unread = bodies["publishIdentityUnreadable"]
    if unread is not None:
        keys = SETPROP_RE.findall(unread)

        if BOOT42_KEY in keys:
            failures.append(
                "publishIdentityUnreadable: publishes %r. That register was "
                "never read on this path; the key must be ABSENT, because an "
                "absent key is a question and a fabricated key is an answer "
                "the operator will believe" % BOOT42_KEY)

        if BOOT0_KEY not in keys:
            failures.append(
                "publishIdentityUnreadable: does not publish %r -- it is the ONE "
                "real measurement this path has" % BOOT0_KEY)

        if "%s\", false" % READABLE_KEY not in unread:
            failures.append(
                "publishIdentityUnreadable: does not publish %s false"
                % READABLE_KEY)

        #
        # No architecture or implementation name may be derived here. Those are
        # functions of a value that is not a chip ID on this path, and
        # nvArchitectureName() answers its sentinel for anything -- a plausible
        # string over a register nobody read.
        #
        for derived in ("nvArchitectureName", "nvAmpereImplementationName",
                        "nvBoot0Architecture", "nvBoot0Implementation"):
            if derived in unread:
                failures.append(
                    "publishIdentityUnreadable: calls %s() -- a name DERIVED "
                    "from a value that is not a chip ID reads as a measurement"
                    % derived)

    # ---- property 4: the readable flag is published on BOTH paths ----------
    #
    # A flag published only on the failure path means its absence is ambiguous
    # between "healthy" and "older kext". handoff18 S1d made the same call for
    # GSPEngineReadable and for the same reason.
    #
    good = bodies["publishIdentity"]
    if good is not None:
        keys = SETPROP_RE.findall(good)
        if READABLE_KEY not in keys:
            failures.append(
                "publishIdentity: does not publish %s -- published on only one "
                "path, the flag's ABSENCE cannot be told from a healthy read"
                % READABLE_KEY)
        if "%s\", true" % READABLE_KEY not in good:
            failures.append(
                "publishIdentity: does not publish %s true" % READABLE_KEY)

        if BOOT42_KEY not in keys:
            failures.append(
                "publishIdentity: does not publish %r -- the live path is the "
                "one that HAS a real BOOT_42" % BOOT42_KEY)

    # ---- property 5: BOOT_42 is published only when it DECODED ------------
    #
    # Added 2026-08-18. decodeBoot42() has always had an arm for "BOOT_0
    # answered but BOOT_42 came back all-ones or all-zeroes" -- it logs the
    # case and returns -- while publishIdentity() put the word into the
    # IORegistry regardless. setProperty() with a dead read is byte-identical
    # to setProperty() with a register that genuinely holds that value, so an
    # operator reading NV_PMC_BOOT_42 = 0xFFFFFFFF could not tell a short
    # aperture from a chip reporting all-ones. handoff18 S1d's class, sixth
    # occurrence.
    #
    # readIdentity() deliberately still returns true on that path: BOOT_0
    # decoded, so the architecture and implementation names ARE measurements
    # and routing to publishIdentityUnreadable() would discard them. The split
    # belongs at publication.
    #
    if good is not None:
        if BOOT42_GUARD_DERIVATION not in good:
            failures.append(
                "publishIdentity: %s is not derived from %s -- the guard must "
                "be the dead-read predicate itself, not a value reconstructed "
                "some other way" % (BOOT42_GUARD, BOOT42_GUARD_DERIVATION))

        span = guarded_span(good, BOOT42_GUARD)
        if span is None:
            failures.append(
                "publishIdentity: no 'if (%s) {' block -- %r is published "
                "unconditionally, so a dead read reaches the IORegistry as a "
                "measurement" % (BOOT42_GUARD, BOOT42_KEY))
        else:
            lo, hi = span
            for site in setprop_sites(good, BOOT42_KEY):
                if not (lo <= site < hi):
                    failures.append(
                        "publishIdentity: publishes %r outside the %s guard"
                        % (BOOT42_KEY, BOOT42_GUARD))

            flag_sites = setprop_sites(good, BOOT42_READABLE_KEY)
            if not flag_sites:
                failures.append(
                    "publishIdentity: does not publish %s -- without it a "
                    "missing %r cannot be told from an older kext"
                    % (BOOT42_READABLE_KEY, BOOT42_KEY))
            else:
                for site in flag_sites:
                    if lo <= site < hi:
                        failures.append(
                            "publishIdentity: publishes %s inside the guard -- "
                            "the flag must be published on BOTH arms"
                            % BOOT42_READABLE_KEY)
                if flag_sites[0] > lo:
                    failures.append(
                        "publishIdentity: publishes %s after the guarded block "
                        "-- it must sort above the word it qualifies"
                        % BOOT42_READABLE_KEY)

    #
    # The flag goes on the unreadable arm too. BOOT_42 is not merely dead
    # there, it was never read at all -- strictly less readable -- so a reader
    # must not have to distinguish an absent flag from an older kext.
    #
    if unread is not None:
        if '%s", false' % BOOT42_READABLE_KEY not in unread:
            failures.append(
                "publishIdentityUnreadable: does not publish %s false -- "
                "BOOT_42 was never read on this path" % BOOT42_READABLE_KEY)

    #
    # Whole-file: NV_PMC_BOOT_42 is published from exactly ONE function. A
    # second publication site anywhere is a site standing behind none of the
    # checks above, and no other gate in this tree could see one.
    #
    n = len(re.findall(r'setProperty\s*\(\s*"%s"' % re.escape(BOOT42_KEY), src))
    if n != 1:
        failures.append(
            "%s: published at %d sites in the file, expected 1 -- "
            "publishIdentity() is the only path that read it" % (BOOT42_KEY, n))

    # ---- property 6: the part verdict reaches the registry -----------------
    #
    # decodeBoot0() detects four facts about the part and writes all four to the
    # log. Until 2026-08-18 none of them reached a consumer: publishIdentity()
    # published NVArchitecture as a bare NAME, so "Ada" landed in the IORegistry
    # byte-identically to "Ampere" with nothing saying the whole plan retargets.
    #
    # No other gate can see this. The register gate reads constants; the C++
    # suites cannot compile a translation unit that reaches IOService; and a
    # deleted setProperty() is well-formed C++.
    #
    if good is not None:
        keys = SETPROP_RE.findall(good)

        if PART_VERDICT_KEY not in keys:
            failures.append(
                "publishIdentity: does not publish %r -- decodeBoot0()'s four "
                "findings reach the log and stop there" % PART_VERDICT_KEY)

        if PART_TARGET_KEY not in keys:
            failures.append(
                "publishIdentity: does not publish %r -- a NAME does not say "
                "whether the measured part is the one this driver was written "
                "for" % PART_TARGET_KEY)

        if PART_VERDICT_DERIVATION not in good:
            failures.append(
                "publishIdentity: does not derive the verdict as %s -- a "
                "hand-rolled architecture test here is a second copy of a "
                "decision NVDecode.hpp already owns" % PART_VERDICT_DERIVATION)

        #
        # The relation is an EQUALITY against the target enumerator. A negation
        # of an error arm is true for every part that is not that arm, so a
        # Turing part would publish NVPartIsTarget true.
        #
        if not PART_TARGET_EQUALITY_RE.search(good):
            failures.append(
                "publishIdentity: %s is not an equality against %s -- a "
                "negation of an error arm reports an unknown part as the "
                "target part" % (PART_TARGET_KEY, PART_TARGET_ENUMERATOR))

        #
        # NVImplementation must be published UNCONDITIONALLY. It used to sit
        # behind an `if (arch == NV_ARCHITECTURE_GA100)`, so a non-Ampere part
        # left the key ABSENT -- and an absence is a question rather than an
        # answer: it reads identically to an older kext that never published the
        # key at all. The bareness is decided by a balanced-brace walk from the
        # previous statement boundary, never by a character class excluding the
        # statement terminator (handoff25 S1c).
        #
        impl_at = good.find('setProperty("%s"' % PART_IMPL_KEY)
        if impl_at < 0:
            failures.append(
                "publishIdentity: does not publish %r on any arm"
                % PART_IMPL_KEY)
        elif not statement_is_bare(good, impl_at):
            failures.append(
                "publishIdentity: publishes %s inside a guard -- a non-Ampere "
                "part then leaves the key absent, which reads identically to "
                "an older kext" % PART_IMPL_KEY)

        #
        # The verdict sorts ABOVE every value it qualifies, same reasoning as
        # NVBoot42Readable and GSPEngineReadable before it.
        #
        arch_at = good.find('setProperty("NVArchitecture"')
        verdict_at = good.find('setProperty("%s"' % PART_VERDICT_KEY)
        if arch_at >= 0 and verdict_at >= 0 and verdict_at > arch_at:
            failures.append(
                "publishIdentity: publishes %s after NVArchitecture -- the "
                "verdict must sort above the names it qualifies"
                % PART_VERDICT_KEY)

    return failures


def mutate(text, old, new, occurrences=1):
    """Replace `old` with `new`, refusing unless it occurs exactly `occurrences`
    times. A mutation whose anchor has drifted must RAISE rather than silently
    match nothing and print ok."""
    if text.count(old) != occurrences:
        raise AssertionError(
            "mutation anchor %r occurs %d times, expected %d"
            % (old, text.count(old), occurrences)
        )
    return text.replace(old, new)


class IdentityPublicationTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.cpp = read_text(CPP)

    def test_live_tree_passes(self):
        """Positive control: the shipping shell satisfies every property."""
        self.assertEqual(audit(self.cpp), [])

    def test_restoring_the_fabricated_boot42_is_caught(self):
        """This is the exact defect, restored. `*outBoot42 = boot0` hands the
        caller a value for a register the function never issued a read for."""
        mutant = mutate(
            self.cpp,
            "        NVLOG(\"readIdentity: skipping NV_PMC_BOOT_42, the aperture is not live\");\n"
            "        return false;",
            "        NVLOG(\"readIdentity: skipping NV_PMC_BOOT_42, the aperture is not live\");\n"
            "        if (outBoot42)\n"
            "            *outBoot42 = boot0;\n"
            "        return false;",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("STORES through outBoot42" in r for r in reasons),
            "expected the dead-read store check to redden, got %r" % (reasons,),
        )

    def test_discarding_the_return_value_is_caught(self):
        """The other half of the original defect: even with readIdentity() honest,
        a caller that ignores the bool publishes the dead-read path as a
        measurement."""
        mutant = mutate(
            self.cpp,
            "    if (readIdentity(&boot0, &boot42))\n"
            "        publishIdentity(boot0, boot42);\n"
            "    else\n"
            "        publishIdentityUnreadable(boot0);",
            "    readIdentity(&boot0, &boot42);\n"
            "    publishIdentity(boot0, boot42);",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("return value is not tested" in r for r in reasons),
            "expected the start() branch check to redden, got %r" % (reasons,),
        )
        self.assertTrue(
            any("never calls publishIdentityUnreadable" in r for r in reasons),
            "expected the unreadable-path check to redden too, got %r" % (reasons,),
        )

    def test_dead_read_branch_returning_true_is_caught(self):
        mutant = mutate(
            self.cpp,
            "        NVLOG(\"readIdentity: skipping NV_PMC_BOOT_42, the aperture is not live\");\n"
            "        return false;",
            "        NVLOG(\"readIdentity: skipping NV_PMC_BOOT_42, the aperture is not live\");\n"
            "        return true;",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("does not `return false;`" in r for r in reasons),
            "expected the false-return check to redden, got %r" % (reasons,),
        )

    def test_publishing_boot42_from_the_unreadable_path_is_caught(self):
        """The subtlest form of the defect: readIdentity() is honest, start()
        branches correctly, and the unreadable publisher fabricates the key
        anyway."""
        mutant = mutate(
            self.cpp,
            '    setProperty("NV_PMC_BOOT_0", boot0, 32);\n'
            '    setProperty("NVIdentityReadable", false);',
            '    setProperty("NV_PMC_BOOT_0", boot0, 32);\n'
            '    setProperty("NV_PMC_BOOT_42", boot0, 32);\n'
            '    setProperty("NVIdentityReadable", false);',
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("publishes 'NV_PMC_BOOT_42'" in r or "publishes \"NV_PMC_BOOT_42\"" in r
                for r in reasons),
            "expected the fabricated-key check to redden, got %r" % (reasons,),
        )
        self.assertTrue(
            any("published at 2 sites" in r for r in reasons),
            "expected the whole-file site count to redden too, got %r" % (reasons,),
        )

    def test_deriving_a_name_on_the_unreadable_path_is_caught(self):
        """nvArchitectureName() answers its sentinel for ANY input, so a name
        derived from a non-chip-ID reads as a measurement rather than as the
        refusal it is."""
        mutant = mutate(
            self.cpp,
            '    setProperty("NVChipIdExpected", (UInt64)NV_CHIP_ID_GA107, 32);\n'
            "\n"
            "    if (fPCIDevice) {\n"
            "        setProperty(\"NVVendorID\",  fPCIDevice->configRead16(kIOPCIConfigVendorID), 16);\n"
            "        setProperty(\"NVDeviceID\",  fPCIDevice->configRead16(kIOPCIConfigDeviceID), 16);\n"
            "    }\n"
            "\n"
            '    setProperty("NVPhase", "3 - identity read FAILED, aperture not live");',
            '    setProperty("NVChipIdExpected", (UInt64)NV_CHIP_ID_GA107, 32);\n'
            '    setProperty("NVArchitecture", nvArchitectureName(nvBoot0Architecture(boot0)));\n'
            "\n"
            "    if (fPCIDevice) {\n"
            "        setProperty(\"NVVendorID\",  fPCIDevice->configRead16(kIOPCIConfigVendorID), 16);\n"
            "        setProperty(\"NVDeviceID\",  fPCIDevice->configRead16(kIOPCIConfigDeviceID), 16);\n"
            "    }\n"
            "\n"
            '    setProperty("NVPhase", "3 - identity read FAILED, aperture not live");',
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("calls nvArchitectureName()" in r for r in reasons),
            "expected the derived-name check to redden, got %r" % (reasons,),
        )

    def test_dropping_boot0_from_the_unreadable_path_is_caught(self):
        """Publishing NOTHING is the opposite failure and equally wrong: BOOT_0
        is the one real measurement that path has."""
        mutant = mutate(
            self.cpp,
            '    setProperty("NV_PMC_BOOT_0", boot0, 32);\n'
            '    setProperty("NVIdentityReadable", false);',
            '    setProperty("NVIdentityReadable", false);',
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("does not publish 'NV_PMC_BOOT_0'" in r or
                'does not publish "NV_PMC_BOOT_0"' in r for r in reasons),
            "expected the BOOT_0 check to redden, got %r" % (reasons,),
        )

    def test_flag_on_only_the_failure_path_is_caught(self):
        """handoff18 S1d's rule: a flag published on one path only means its
        absence cannot be told from a healthy read on an older kext."""
        mutant = mutate(
            self.cpp,
            '    setProperty("NVPhase", "3 - register reads only, no GPU initialisation");\n'
            '    setProperty("NVIdentityReadable", true);',
            '    setProperty("NVPhase", "3 - register reads only, no GPU initialisation");',
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("published on only one " in r for r in reasons),
            "expected the both-paths flag check to redden, got %r" % (reasons,),
        )

    def test_live_path_losing_boot42_is_caught(self):
        """The over-correction: refusing to publish BOOT_42 even where it WAS
        read throws away a real measurement."""
        mutant = mutate(
            self.cpp,
            '    setProperty("NV_PMC_BOOT_42", boot42, 32);\n',
            "",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("published at 0 sites" in r for r in reasons),
            "expected the whole-file site count to redden, got %r" % (reasons,),
        )

    def test_publishing_boot42_unguarded_is_caught(self):
        """The defect itself: BOOT_42 published on the live path regardless of
        whether it decoded. decodeBoot42() already knows that case exists and
        logs it; publishing the word anyway puts a dead read into the
        IORegistry byte-identical to a register that genuinely holds it."""
        mutant = mutate(
            self.cpp,
            "    if (boot42Readable) {\n"
            '        setProperty("NV_PMC_BOOT_42", boot42, 32);\n'
            "    } else {",
            '    setProperty("NV_PMC_BOOT_42", boot42, 32);\n'
            "    if (!boot42Readable) {",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("no 'if (boot42Readable) {' block" in r for r in reasons),
            "expected the guard check to redden, got %r" % (reasons,),
        )

    def test_a_guard_not_derived_from_the_predicate_is_caught(self):
        """The guard must BE the dead-read predicate. A hand-rolled equality
        against all-ones alone accepts the all-zeroes half of a dead read,
        which is the case that means the mapping points at nothing."""
        mutant = mutate(
            self.cpp,
            "    bool boot42Readable = !nvLooksLikeDeadRead(boot42);",
            "    bool boot42Readable = (boot42 != 0xFFFFFFFF);",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("is not derived from" in r for r in reasons),
            "expected the derivation check to redden, got %r" % (reasons,),
        )

    def test_the_readable_flag_inside_the_guard_is_caught(self):
        """A flag published only where the word WAS readable is always true, so
        it carries no information at all -- and its absence then reads as an
        older kext rather than as a dead read.

        The mutation is TWO independent edits rather than one anchor spanning
        both statements. Until 2026-08-19 it anchored on the flag publication
        and the guard being ADJACENT, and the chip verdict block landing between
        them made that anchor occur ZERO times -- so mutate() raised and this
        control failed for a reason with nothing to do with its own property.
        An anchor spanning two statements is a silent claim about everything
        between them, and the next correct edit to the subject breaks it.
        """
        mutant = mutate(
            self.cpp,
            '    setProperty("NVBoot42Readable", boot42Readable);\n',
            "",
        )
        mutant = mutate(
            mutant,
            "    if (boot42Readable) {\n",
            "    if (boot42Readable) {\n"
            '        setProperty("NVBoot42Readable", boot42Readable);\n',
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("inside the guard" in r for r in reasons),
            "expected the both-arms check to redden, got %r" % (reasons,),
        )

    def test_the_flag_missing_from_the_unreadable_arm_is_caught(self):
        """BOOT_42 is not merely dead on that path, it was never read at all --
        strictly less readable -- so the flag belongs there too."""
        mutant = mutate(
            self.cpp,
            '    setProperty("NVBoot42Readable", false);\n',
            "",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("does not publish NVBoot42Readable false" in r
                for r in reasons),
            "expected the unreadable-arm check to redden, got %r" % (reasons,),
        )

    # ---- property 6 negative controls -----------------------------------
    #
    # Each of these is a shape somebody could plausibly land, and each reddens
    # ONLY its own property. The first two are the defect itself, restored.
    #

    def test_deleting_the_part_verdict_is_caught(self):
        """This is the defect, restored. decodeBoot0() still detects all four
        cases and still logs them; nothing reaches the registry, so an Ada part
        publishes \"Ada\" byte-identically to an Ampere part publishing
        \"Ampere\" and nothing says the whole plan retargets."""
        mutant = mutate(
            self.cpp,
            '    setProperty("NVPartVerdict", nvPartVerdictName(part));\n',
            "",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("does not publish 'NVPartVerdict'" in r for r in reasons),
            "expected the verdict-publication check to redden, got %r"
            % (reasons,),
        )

    def test_deleting_the_target_flag_is_caught(self):
        """A NAME is not an answer to \"is this the part the driver was written
        for\". Losing the boolean leaves an operator to know NVIDIA's
        implementation numbering by heart."""
        mutant = mutate(
            self.cpp,
            '    setProperty("NVPartIsTarget", part == NV_PART_TARGET);\n',
            "",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("does not publish 'NVPartIsTarget'" in r for r in reasons),
            "expected the target-flag check to redden, got %r" % (reasons,),
        )

    def test_a_negated_error_arm_is_not_an_equality(self):
        """The sharp one. `part != NV_PART_ADA` is TRUE for every non-Ada part,
        including a Turing one this driver has never seen -- so the registry
        would report an unknown part as the target part. The relation must be an
        EQUALITY against NV_PART_TARGET, never a negation of an error arm.

        Same family as 3050-handoff32.md S3's `!= ERR_HEADER` reasoning one
        level over: a comparison against the wrong pole of a verdict family
        answers 'fine' for everything the family does not specifically name.
        """
        mutant = mutate(
            self.cpp,
            '    setProperty("NVPartIsTarget", part == NV_PART_TARGET);',
            '    setProperty("NVPartIsTarget", part != NV_PART_ADA);',
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("is not an equality against" in r for r in reasons),
            "expected the equality check to redden, got %r" % (reasons,),
        )

    def test_a_hand_rolled_architecture_test_is_caught(self):
        """Deriving the verdict here rather than calling nvPartVerdict() is a
        second copy of a decision NVDecode.hpp owns and test_decode.cpp covers.
        A copy drifts silently: the header could grow a fifth verdict and this
        publisher would never learn of it."""
        mutant = mutate(
            self.cpp,
            "    NVPartVerdict part = nvPartVerdict(boot0);",
            "    NVPartVerdict part = (arch == NV_ARCHITECTURE_GA100 &&\n"
            "                          impl == NV_IMPLEMENTATION_GA107)\n"
            "                             ? NV_PART_TARGET\n"
            "                             : NV_PART_OTHER_ARCHITECTURE;",
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("does not derive the verdict as" in r for r in reasons),
            "expected the derivation check to redden, got %r" % (reasons,),
        )

    def test_reguarding_the_implementation_key_is_caught(self):
        """The shape this fix removed, restored. Guarding NVImplementation on
        the Ampere test leaves the key ABSENT for a non-Ampere part -- and an
        absence is a question rather than an answer: it reads identically to an
        older kext that never published the key at all.

        This is also what exercises statement_is_bare(). A regex over the
        preceding line would not distinguish these two shapes.
        """
        mutant = mutate(
            self.cpp,
            '    setProperty("NVImplementation", nvAmpereImplementationName(impl));',
            "    if (arch == NV_ARCHITECTURE_GA100)\n"
            '        setProperty("NVImplementation", nvAmpereImplementationName(impl));',
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("inside a guard" in r for r in reasons),
            "expected the bareness check to redden, got %r" % (reasons,),
        )

    def test_the_verdict_sorting_below_the_names_is_caught(self):
        """A verdict published after the values it qualifies is still correct and
        still useless at the moment it matters: an operator scrolling
        IORegistryExplorer reads NVArchitecture first and has already formed a
        belief. Same reasoning as NVBoot42Readable and GSPEngineReadable."""
        mutant = mutate(
            self.cpp,
            '    setProperty("NVPartVerdict", nvPartVerdictName(part));\n'
            '    setProperty("NVPartIsTarget", part == NV_PART_TARGET);\n'
            "\n"
            '    setProperty("NVArchitecture", nvArchitectureName(arch));',
            '    setProperty("NVArchitecture", nvArchitectureName(arch));\n'
            "\n"
            '    setProperty("NVPartVerdict", nvPartVerdictName(part));\n'
            '    setProperty("NVPartIsTarget", part == NV_PART_TARGET);',
        )
        reasons = audit(mutant)
        self.assertTrue(
            any("after NVArchitecture" in r for r in reasons),
            "expected the ordering check to redden, got %r" % (reasons,),
        )

    def test_audit_refuses_when_a_subject_is_absent(self):
        """A gate that cannot find its subject must REFUSE, never pass over an
        empty string. This is the failure mode that makes a shape gate vacuous:
        every 'does not contain' assertion is satisfied by ""."""
        mutant = mutate(
            self.cpp,
            "NV3050Probe::publishIdentityUnreadable(",
            "NV3050Probe::publishIdentityCannotBeRead(",
            occurrences=1,
        )
        reasons = audit(mutant)
        self.assertTrue(
            any(r.startswith("publishIdentityUnreadable: body not found")
                for r in reasons),
            "expected a refusal, got %r" % (reasons,),
        )

    def test_comment_prose_alone_does_not_satisfy_the_gate(self):
        """The dead-read branch's own comment DISCUSSES outBoot42 by name -- it
        has to, because the whole point of that branch is what it does NOT do
        with the out-parameter. A gate reading prose would therefore find the
        identifier it exists to forbid in a PRISTINE tree, and would equally
        accept a branch whose real code had been deleted and replaced by a
        comment. Both directions are why audit() scans stripped source.

        Named as a property rather than as an incidental fact: if somebody
        rewrites that comment so it no longer mentions outBoot42, this test goes
        red and the answer is to delete it, not to weaken strip_comments().
        """
        raw_branch = dead_read_branch(
            function_body(self.cpp, "NV3050Probe::readIdentity("))
        self.assertIsNotNone(raw_branch)
        self.assertIn(
            "outBoot42", raw_branch,
            "the dead-read branch's comment should still explain what it "
            "deliberately does not do with outBoot42")

        stripped = strip_comments(self.cpp)
        read = function_body(stripped, "NV3050Probe::readIdentity(")
        self.assertIsNotNone(read)
        branch = dead_read_branch(read)
        self.assertIsNotNone(branch)
        self.assertNotIn(
            "outBoot42", branch,
            "after comment stripping the dead-read branch should mention "
            "outBoot42 nowhere -- prose is not code")


if __name__ == "__main__":
    unittest.main()
