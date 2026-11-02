#!/usr/bin/env python3
"""Pin the SHAPE of IntelFbPresentProbe's IOFramebuffer overrides.

Why this module exists
----------------------

`phase0-kext/IntelFbPresentProbe.cpp` is an IOKit shell.  It includes IOKit's
own headers, so no host suite compiles a line of it -- the same by-construction
blindness `intel-handoff26.md` SS1, `3050-handoff30.md` SS1 and
`tests/test_intel_publisher_scan_shape.py` each re-derive.  The consequence
here is concrete and was measured before this file was written: deleting the
`presentable()` guard out of any one of the eight IOFramebuffer overrides
leaves ALL FOUR gates green.

  * the register gate reads `#define`s, and every constant involved is correct;
  * the C++ suites drive `IntelFbPresent.hpp` directly and prove
    `igPresentVerdict()` / `igPresentUsable()` are CORRECT -- they say nothing
    about whether the shell CONSULTS them;
  * `check-kext-syntax.bat` parses the translation unit, and clang has nothing
    to say about a deleted `if` around a well-formed `return`;
  * nothing in the Python gate modelled "every override consults the verdict".

That is exactly the class this project keeps filing: a refusal nobody consumes
is a zero, and an override that answers IOKit without consulting the verdict
reports a mode, a pixel format and an aperture for a plane the driver never
successfully sampled.  `getPixelInformation()` filling an `IOPixelInformation`
out of `IG_PRESENT_INVALID32` words is byte-identical, at the IOKit boundary,
to one filled from four registers that genuinely read.

What is pinned
--------------

1. Every IOFramebuffer override in the subject consults the presentability
   verdict before it answers.  The override SET is DERIVED from the header's
   `APPLE_KEXT_OVERRIDE` declarations, not spelled here -- so a ninth override
   added later with no guard reddens on the commit that adds it, naming the
   method.  `intel-handoff26.md` SS2a's derive-the-subject-set reasoning.
2. `presentable()` is a conjunction of BOTH `fSampleTaken` and an EQUALITY
   against `IG_PRESENT_OK`.  The equality is load-bearing for the reason the
   subject's own comment gives at `:54` -- the verdict field zeroes to
   `IG_PRESENT_OK`, so a verdict test alone is satisfied by a run that never
   sampled anything.  A negation of one error arm (`!= ERR_SAMPLE_REFUSED`)
   would be true for all three other refusals: `3050-handoff32.md` SS3.
3. `fSampleTaken` is set only AFTER the sample succeeds, never before.
4. The verdict and the sampled flag are published FIRST and UNCONDITIONALLY,
   ahead of every value they qualify -- `GSPEngineReadable`'s reasoning, and
   the reason an absent key is a question rather than an answer.
5. Every numeric property published out of the four sampled words sits BEHIND
   the presentable guard.
6. The scan is over COMMENT- AND STRING-STRIPPED source.  Mandatory rather than
   stylistic: the subject's comments discuss `presentable()`, `fSampleTaken`
   and `IG_PRESENT_OK` by name, so a prose-reading gate would report a guard on
   an override whose guard was deleted.

Refusal rather than silence
---------------------------

If the subject file, the header, the verdict family or the override set cannot
be found, every test REFUSES with a message naming what was missing.  A module
that measures nothing when its anchor moves is indistinguishable from a green
one -- `3050-handoff30.md` SS2b.
"""

import os
import re
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SHELL = os.path.join(ROOT, "phase0-kext", "IntelFbPresentProbe.cpp")
SHELL_HDR = os.path.join(ROOT, "phase0-kext", "IntelFbPresentProbe.hpp")
PURE_HDR = os.path.join(ROOT, "phase0-kext", "IntelFbPresent.hpp")

# The shim's IOFramebuffer declaration.  P1's subject set is derived partly from
# HERE rather than only from the subclass's own header, and that is this
# module's own hole being closed (`intel-handoff31.md` SS9 item 1).  A set
# derived from the subclass RATIFIES whatever the subclass happens to declare:
# an override the author forgot to declare is simply absent from the set, so the
# gate reports green over a method it never considered.  What IOFramebuffer
# DEMANDS is not the author's to forget.
BASE_HDR = os.path.join(HERE, "kextshim", "IOKit", "graphics", "IOFramebuffer.h")

CLASS = "IntelFbPresentProbe"
GUARD_FN = "presentable"
SAMPLED_FLAG = "fSampleTaken"
VERDICT_FIELD = "fPresentVerdict"
OK_CONSTANT = "IG_PRESENT_OK"

# The two keys that must be published first and unconditionally.  These are
# spelled because they are an INTERFACE -- the strings an operator greps the
# IORegistry for -- and a gate that derived them from the subject would ratify
# a rename that silently breaks every consumer.
VERDICT_KEY = "IGPresentVerdict"
SAMPLED_KEY = "IGPresentSampled"


#
# ---- reading the tree ------------------------------------------------------
#

def strip_comments_and_strings(src):
    """Remove // and /* */ comments and the CONTENTS of string literals.

    String contents go too, not just comments.  The shell logs
    'not presentable (%s)' inside several overrides, so a scan over source that
    kept string bodies would find the guard's own name inside a log line in a
    method whose guard had been deleted.  Quotes are kept as empty "" so the
    surrounding syntax still parses as calls and arguments.
    """
    out = []
    i = 0
    n = len(src)
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
        elif c == '"':
            out.append('""')
            i += 1
            while i < n:
                if src[i] == "\\":
                    i += 2
                    continue
                if src[i] == '"':
                    i += 1
                    break
                i += 1
        elif c == "'":
            out.append("' '")
            i += 1
            while i < n:
                if src[i] == "\\":
                    i += 2
                    continue
                if src[i] == "'":
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def body_of(src, method):
    """Return the BODY of Class::method, brace-balanced, or None.

    A balanced walk rather than a regex, for `handoff25.md` SS1c's reason: a
    negated character class excluding only the statement terminator is not
    scoped to the statement, and runs straight past a closing paren into the
    next one.  The body EXCLUDES its own outermost braces, so a call written
    directly in the method sits at depth 0 -- `bareness-is-not-nesting-depth`.
    """
    pat = re.compile(
        r"\b" + re.escape(CLASS) + r"::" + re.escape(method) + r"\s*\("
    )
    m = pat.search(src)
    if m is None:
        return None
    # Walk to the opening brace of the definition, refusing a declaration.
    i = m.end()
    depth = 1
    while i < len(src) and depth > 0:
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
        i += 1
    while i < len(src) and src[i] not in "{;":
        i += 1
    if i >= len(src) or src[i] == ";":
        return None
    start = i + 1
    depth = 1
    j = start
    while j < len(src) and depth > 0:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
        j += 1
    if depth != 0:
        return None
    return src[start:j - 1]


def declared_overrides(header_src):
    """Derive the override set from the header's APPLE_KEXT_OVERRIDE lines.

    Deriving rather than spelling is the whole point: a ninth override added
    later is swept in with no edit to this file.  init/free/start/stop are
    EXCLUDED -- they are lifecycle methods that RUN the sample rather than
    answer a question about it, so requiring a guard in them would redden
    correct code.  That exclusion is itself asserted below, so it cannot be
    widened silently.
    """
    names = []
    for m in re.finditer(
        r"virtual\s+[A-Za-z_][A-Za-z0-9_ *&:<>]*?\b([A-Za-z_][A-Za-z0-9_]*)"
        r"\s*\([^;{]*\)[^;{]*APPLE_KEXT_OVERRIDE",
        header_src,
    ):
        names.append(m.group(1))
    return names


def demanded_pure_virtuals(base_hdr_src):
    """Derive the methods IOFramebuffer DEMANDS a concrete subclass define.

    Split on the statement terminator and keep the `virtual` declarations whose
    own tail is `= 0`.  One regex over the whole file instead OVER-MATCHES: a
    lazy run before `= 0` crosses the OSDeclareDefaultStructors line and every
    intervening declaration to reach the first `= 0` in the file, which is an
    enumerator (`kIOFBNoError = 0`) rather than a pure virtual.  That is
    `intel-handoff26.md` SS3's over-match direction, and it was measured here
    rather than reasoned about -- a throwaway probe using that shape reported
    seventeen names where eight are pure.

    Pure-ness is decided per chunk, so a declaration made CONCRETE in the shim
    LEAVES this set.  That is the direction that shrinks a subject set silently,
    which is why the floor below refuses rather than passes.

    PURE-NESS IS DECIDED ON THE SUFFIX AFTER THE PARAMETER LIST, not on the
    chunk's tail, and that is `intel-handoff32.md` SS6 item 3 closed -- though
    not for the reason that item gives.  It predicted the hole was a `= 0`
    written across a line break.  MEASURED, that is not a hole at all: Python's
    `\\s` already matches a newline, so every wrapped spelling was already
    found, and a whitespace-collapsing "fix" changes the output on no input.

    The real hole is any TOKEN standing after the `= 0`:

        virtual IOReturn getPixelInformation(...) = 0 APPLE_KEXT_PURE;
        virtual IOReturn getCurrentDisplayMode(...) = 0 __attribute__((x));

    Both are pure virtuals, and both leave a chunk whose tail is not `= 0`, so
    both DROPPED OUT of the demanded set -- P0 then stops asking about the
    method and reports green over one it never considered, which is exactly the
    defect SS1a measured for the declared half, arrived at from the other side.
    It shrinks the set rather than failing, so the floor at 5 is the only thing
    that would ever notice, and the floor does not pin 8.

    Walking the balanced parameter list and testing only what FOLLOWS it closes
    that, and keeps the one thing the tail test got right: a default argument's
    own `= 0` sits INSIDE the parens, so a concrete `f(int x = 0);` is still
    correctly not pure.
    """
    names = []
    for chunk in base_hdr_src.split(";"):
        if "virtual" not in chunk:
            continue
        tail = chunk[chunk.index("virtual"):]
        m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", tail)
        if not m:
            continue
        i = m.end() - 1
        depth = 0
        while i < len(tail):
            if tail[i] == "(":
                depth += 1
            elif tail[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if depth != 0 or i >= len(tail):
            continue
        suffix = re.sub(r"\s+", " ", tail[i + 1:])
        if re.search(r"(^| )=\s*0\b", suffix):
            names.append(m.group(1))
    return names


LIFECYCLE = ("init", "free", "start", "stop")


#
# The four words the group sample captures.  Spelled from the header's own
# field names; a decode is recognised by the fact that it CONSUMES them, never
# by the key it publishes under.
#
SAMPLE_FIELDS = ("fCtl", "fStride", "fSize", "fSurf")


def setproperty_calls(body):
    """Yield (offset, key, argument_text) for each setProperty("key", ...).

    The argument text is taken by a balanced-paren walk from the call's opening
    paren, so a nested call in the value position is captured whole.  A regex
    stopping at the first `)` would truncate
    `setProperty("IGPresentWidth", igPresentWidth(fCtl, ...), 32)` at the inner
    call's paren and lose every field name it consumes -- and losing them is the
    silent direction: the value would then look like a bare word rather than a
    decode, and P5 would wave it through.
    """
    for m in re.finditer(r'setProperty\(\s*"([^"]+)"', body):
        open_paren = body.index("(", m.start())
        depth = 1
        i = open_paren + 1
        while i < len(body) and depth > 0:
            if body[i] == "(":
                depth += 1
            elif body[i] == ")":
                depth -= 1
            i += 1
        yield (m.start(), m.group(1), body[open_paren + 1:i - 1])


def is_decode_of_the_sample(args):
    """True when a published value is COMPUTED from the sampled words.

    Structural rather than a key list, and that is the load-bearing choice.  A
    typed set of key names is the anchor shape intel-handoff28.md SS3 records:
    it silently narrows, so a ninth published decode joins a list nobody
    updates and P5 reports green over a smaller set.  Deriving instead means a
    new `setProperty("IGPresentFoo", igPresentFoo(fCtl, ...))` is policed on the
    commit that adds it.

    The four RAW words are deliberately NOT decodes.  They are published ahead
    of the guard on purpose -- they are the evidence every decode below is
    derived from, and an operator reading a refused run needs to see the words
    that produced the refusal.  A bare `fCtl` in the value position is a raw
    word; `igPresentWidth(fCtl, ...)` is a decode.  The distinction is whether a
    FUNCTION consumes them.
    """
    if not re.search(r"\big[A-Z][A-Za-z0-9_]*\s*\(", args):
        return False
    return any(re.search(r"\b" + f + r"\b", args) for f in SAMPLE_FIELDS)


def audit(shell_src=None, shell_hdr_src=None, pure_hdr_src=None,
          base_hdr_src=None):
    """Return (failures, facts).

    One audit shared with the live tree, so every negative control below runs
    against the SAME code path the positive control does.  Sources may be
    injected; anything omitted is read from disk.
    """
    failures = []
    facts = {}

    if shell_src is None:
        if not os.path.isfile(SHELL):
            return (["REFUSED: subject not found: %s" % SHELL], facts)
        shell_src = read(SHELL)
    if shell_hdr_src is None:
        if not os.path.isfile(SHELL_HDR):
            return (["REFUSED: header not found: %s" % SHELL_HDR], facts)
        shell_hdr_src = read(SHELL_HDR)
    if pure_hdr_src is None:
        if not os.path.isfile(PURE_HDR):
            return (["REFUSED: pure header not found: %s" % PURE_HDR], facts)
        pure_hdr_src = read(PURE_HDR)
    if base_hdr_src is None:
        if not os.path.isfile(BASE_HDR):
            return (["REFUSED: base header not found: %s" % BASE_HDR], facts)
        base_hdr_src = read(BASE_HDR)

    code = strip_comments_and_strings(shell_src)
    hdr = strip_comments_and_strings(shell_hdr_src)
    pure = strip_comments_and_strings(pure_hdr_src)
    base = strip_comments_and_strings(base_hdr_src)

    facts["code"] = code

    # The verdict family must exist in the pure header, or nothing below means
    # anything.  Refuse rather than pass.
    if ("#define " + OK_CONSTANT) not in pure:
        return (["REFUSED: %s not defined in %s" % (OK_CONSTANT, PURE_HDR)],
                facts)

    overrides = declared_overrides(hdr)
    facts["overrides"] = overrides
    if not overrides:
        return (["REFUSED: no APPLE_KEXT_OVERRIDE declarations found in %s"
                 % SHELL_HDR], facts)

    declared = [n for n in overrides if n not in LIFECYCLE]
    facts["declared"] = declared

    demanded = demanded_pure_virtuals(base)
    facts["demanded"] = demanded
    if len(demanded) < 5:
        return (["REFUSED: only %d pure virtuals derived from %s (%s) -- a "
                 "gate whose subject set collapses reports green over an "
                 "unmeasured subject, which is worse than no gate"
                 % (len(demanded), BASE_HDR, ", ".join(demanded) or "none")],
                facts)

    #
    # Property 0 -- every pure virtual IOFramebuffer demands is DECLARED as an
    # override by the subclass.
    #
    # This arm is why the demanded set is read at all.  Under a purely
    # header-derived subject set, deleting a declaration REMOVED the method from
    # the set, so P1 stopped asking about it and the gate stayed green -- an
    # absence of coverage reported as a positive result.
    #
    for name in demanded:
        if name not in overrides:
            failures.append(
                "P0: IOFramebuffer demands %s() and %s declares no override "
                "for it -- a subject set derived from the subclass alone would "
                "drop it silently rather than redden" % (name, SHELL_HDR))

    #
    # The subject set is the UNION, never the demanded set alone.  Repointing P1
    # at the pure virtuals only would NARROW it: measured 2026-08-20, the shim
    # demands 8 while the header declares 11 answering overrides, so
    # setDisplayMode, enableController and isConsoleDevice -- concrete in the
    # base, overridden here, and every one of them an answer about the sample --
    # would fall out of coverage entirely.  That is `intel-handoff28.md` SS3a's
    # self-narrowing gate arrived at from the other side, and the strictness of
    # the containment is asserted below so a future edit cannot collapse the
    # union back onto either half while staying green.
    #
    answering = list(declared)
    for name in demanded:
        if name not in answering:
            answering.append(name)
    facts["answering"] = answering
    if len(answering) < 5:
        return (["REFUSED: only %d answering overrides derived (%s) -- the "
                 "declaration pattern has stopped matching"
                 % (len(answering), ", ".join(answering) or "none")], facts)

    #
    # Property 1 -- every answering override consults the verdict.
    #
    for name in answering:
        body = body_of(code, name)
        if body is None:
            failures.append("P1: no definition found for %s::%s"
                            % (CLASS, name))
            continue
        if (GUARD_FN + "(") not in body:
            failures.append(
                "P1: %s::%s answers IOKit without calling %s() -- it would "
                "describe a plane that was never successfully sampled"
                % (CLASS, name, GUARD_FN))

    #
    # Property 2 -- presentable() is a conjunction of the flag AND an equality.
    #
    guard = body_of(code, GUARD_FN)
    if guard is None:
        failures.append("P2: no definition found for %s::%s"
                        % (CLASS, GUARD_FN))
    else:
        facts["guard"] = guard
        if SAMPLED_FLAG not in guard:
            failures.append(
                "P2: %s() does not consult %s -- the verdict field zeroes to "
                "%s, so a verdict test alone passes a run that never sampled"
                % (GUARD_FN, SAMPLED_FLAG, OK_CONSTANT))
        if "&&" not in guard:
            failures.append(
                "P2: %s() is not a conjunction -- both the sampled flag and "
                "the verdict must hold" % GUARD_FN)
        if not re.search(re.escape(VERDICT_FIELD) + r"\s*==\s*"
                         + re.escape(OK_CONSTANT), guard):
            failures.append(
                "P2: %s() does not test %s == %s -- a negation of one error "
                "arm is TRUE for every other refusal"
                % (GUARD_FN, VERDICT_FIELD, OK_CONSTANT))
        if re.search(re.escape(VERDICT_FIELD) + r"\s*!=", guard):
            failures.append(
                "P2: %s() tests %s with != -- an inequality against one arm "
                "admits the other refusals" % (GUARD_FN, VERDICT_FIELD))

    #
    # Property 3 -- the sampled flag is set only after the sample succeeds.
    #
    sampler = body_of(code, "samplePlane")
    if sampler is None:
        failures.append("P3: no definition found for %s::samplePlane" % CLASS)
    else:
        facts["sampler"] = sampler
        sets = [m for m in re.finditer(
            re.escape(SAMPLED_FLAG) + r"\s*=\s*true", sampler)]
        if len(sets) != 1:
            failures.append(
                "P3: samplePlane() sets %s = true %d times, expected exactly "
                "one" % (SAMPLED_FLAG, len(sets)))
        else:
            before = sampler[:sets[0].start()]
            if "regRead32(" not in before and "fCtl" not in before:
                failures.append(
                    "P3: samplePlane() sets %s = true before reading anything "
                    "-- the flag would report a sample that never happened"
                    % SAMPLED_FLAG)

    #
    # Property 4 -- verdict and sampled flag published FIRST, unconditionally.
    #
    pub = body_of(code, "publishPresentation")
    if pub is None:
        failures.append("P4: no definition found for %s::publishPresentation"
                        % CLASS)
    else:
        facts["publisher"] = pub
        #
        # The stripper emptied every string literal, so the key NAMES are gone
        # from `pub` -- it is deliberately useless for this property.  Recover
        # the key order from the UNSTRIPPED body instead: property keys are an
        # interface an operator greps the IORegistry for, so they have to be
        # compared as themselves rather than as a shape.  Prose cannot forge a
        # match here, because the pattern requires the literal to sit inside a
        # setProperty( call.
        #
        raw_pub = body_of(shell_src, "publishPresentation")
        if raw_pub is None:
            failures.append("P4: publishPresentation() body unreadable")
        else:
            keys = re.findall(r'setProperty\(\s*"([^"]+)"', raw_pub)
            facts["keys"] = keys
            if not keys:
                failures.append(
                    "P4: publishPresentation() publishes nothing -- the "
                    "setProperty pattern has stopped matching")
            else:
                if VERDICT_KEY not in keys:
                    failures.append("P4: %s is never published" % VERDICT_KEY)
                if SAMPLED_KEY not in keys:
                    failures.append("P4: %s is never published" % SAMPLED_KEY)
                if VERDICT_KEY in keys and SAMPLED_KEY in keys:
                    if keys.index(VERDICT_KEY) > 1 or \
                            keys.index(SAMPLED_KEY) > 1:
                        failures.append(
                            "P4: %s and %s are not the first two properties "
                            "published (order was %s) -- a verdict must sort "
                            "above every value it qualifies"
                            % (VERDICT_KEY, SAMPLED_KEY, ", ".join(keys[:4])))
            # Unconditional: the two calls must sit at brace depth 0 of the
            # publisher body, i.e. inside no if.
            for key in (VERDICT_KEY, SAMPLED_KEY):
                m = re.search(r'setProperty\(\s*"' + re.escape(key) + r'"',
                              raw_pub)
                if m is None:
                    continue
                depth = 0
                for ch in raw_pub[:m.start()]:
                    if ch == "{":
                        depth += 1
                    elif ch == "}":
                        depth -= 1
                if depth != 0:
                    failures.append(
                        "P4: %s is published inside a conditional (brace "
                        "depth %d) -- its ABSENCE would then be "
                        "indistinguishable from an older kext" % (key, depth))

    #
    # Property 5 -- every value DERIVED from the four sampled words is published
    # only behind the guard.
    #
    # This is the property the whole module exists for.  A width, a stride or an
    # aperture extent computed out of IG_PRESENT_INVALID32 and published as
    # IGPresentWidth is byte-identical, in the IORegistry, to one computed out of
    # four registers that genuinely read -- `a-refusal-nobody-consumes-is-a-zero`
    # at the publication boundary.  The two flag keys are the exception BY
    # DESIGN: they are what makes the distinction visible at all, so they must
    # sit ahead of the guard rather than behind it (property 4).
    #
    if pub is not None:
        raw_pub = body_of(shell_src, "publishPresentation")
        if raw_pub is not None:
            gm = re.search(r"if\s*\(\s*!\s*" + re.escape(GUARD_FN)
                           + r"\s*\(\s*\)\s*\)", raw_pub)
            if gm is None:
                failures.append(
                    "P5: publishPresentation() has no !%s() guard at all -- "
                    "every derived value it publishes would be computed from "
                    "words that may never have been read" % GUARD_FN)
            else:
                decodes = 0
                for at, key, args in setproperty_calls(raw_pub):
                    if not is_decode_of_the_sample(args):
                        continue
                    decodes += 1
                    if at < gm.start():
                        failures.append(
                            "P5: %s is a DECODE of the sampled words and is "
                            "published BEFORE the !%s() guard -- a number "
                            "derived from IG_PRESENT_INVALID32 is "
                            "indistinguishable, in the IORegistry, from one "
                            "derived from four registers that read"
                            % (key, GUARD_FN))
                facts["decode_count"] = decodes
                if decodes == 0:
                    failures.append(
                        "P5: no decode publications recognised at all -- the "
                        "predicate has stopped matching, so this property is "
                        "passing over an empty set")

    return (failures, facts)


#
# ---- the positive control --------------------------------------------------
#

class LiveTreeTests(unittest.TestCase):

    def test_live_tree_passes(self):
        failures, facts = audit()
        self.assertEqual(
            [], failures,
            "the live tree should satisfy every property; got:\n  "
            + "\n  ".join(failures))

    def test_override_set_is_derived_and_nonempty(self):
        _, facts = audit()
        self.assertIn("answering", facts)
        self.assertGreaterEqual(
            len(facts["answering"]), 5,
            "the answering-override set is derived from the header; a small "
            "set means the declaration pattern stopped matching")

    def test_demanded_set_is_derived_and_nonempty(self):
        """The pure-virtual set comes off the shim, not out of this file."""
        _, facts = audit()
        self.assertIn("demanded", facts)
        self.assertGreaterEqual(
            len(facts["demanded"]), 5,
            "IOFramebuffer's pure virtuals are parsed from the shim; a small "
            "set means the declaration split stopped matching")
        self.assertIn(
            "getApertureRange", facts["demanded"],
            "getApertureRange is pure in Apple's header and pure in the shim; "
            "its absence means the parse landed somewhere else")

    def test_answering_is_the_union_of_both_sets(self):
        """Set EQUALITY, so a future edit cannot collapse onto either half.

        A containment assertion alone is satisfied by the union, by the declared
        set, and by the demanded set, so it would ratify exactly the narrowing
        this arm exists to forbid.
        """
        _, facts = audit()
        self.assertEqual(
            set(facts["declared"]) | set(facts["demanded"]),
            set(facts["answering"]),
            "P1's subject set must be the union; anything else drops methods "
            "one of the two sources knows about")

    def test_demanded_is_a_strict_subset_of_declared(self):
        """Anti-degeneracy: prove the union is broader than the demanded half.

        If the two sets happened to be EQUAL in this tree, the union assertion
        above would hold under a P1 pointed at the demanded set alone, and the
        narrowing would go unnoticed until somebody added a non-pure override.
        Measured 2026-08-20: 8 demanded, 11 declared.
        """
        _, facts = audit()
        demanded = set(facts["demanded"])
        declared = set(facts["declared"])
        self.assertTrue(
            demanded < declared,
            "expected the demanded set to be a STRICT subset of the declared "
            "answering set; demanded=%s declared=%s"
            % (sorted(demanded), sorted(declared)))

    def test_overridden_concrete_base_methods_are_covered(self):
        """The three methods that exist only because of the union.

        setDisplayMode, enableController and isConsoleDevice are CONCRETE in
        IOFramebuffer and overridden here, and each answers a question about the
        sample.  A P1 derived from the pure virtuals alone would not ask about
        any of them.
        """
        _, facts = audit()
        for name in ("setDisplayMode", "enableController", "isConsoleDevice"):
            self.assertNotIn(
                name, facts["demanded"],
                "%s is concrete in the shim; if it became pure this test is "
                "the wrong shape rather than the code" % name)
            self.assertIn(
                name, facts["answering"],
                "%s is overridden and answers about the sample, so it must be "
                "in P1's subject set" % name)

    def test_lifecycle_methods_are_excluded(self):
        _, facts = audit()
        for name in LIFECYCLE:
            self.assertNotIn(
                name, facts.get("answering", []),
                "%s runs the sample rather than answering a question about "
                "it; requiring a guard there reddens correct code" % name)

    def test_stripper_removes_prose_mentions(self):
        src = 'void f(void) { /* presentable() and fSampleTaken */ int x; }'
        out = strip_comments_and_strings(src)
        self.assertNotIn("presentable", out)
        self.assertNotIn(SAMPLED_FLAG, out)

    def test_stripper_removes_string_bodies(self):
        src = 'void f(void) { IGLOG("not presentable (%s)"); }'
        out = strip_comments_and_strings(src)
        self.assertNotIn("presentable", out)


#
# ---- negative controls -----------------------------------------------------
#
# Each mutates ONE thing and asserts WHICH property broke.  A control that
# merely asserts "something failed" cannot tell a working gate from one that
# reddens on everything.
#

class NegativeControlTests(unittest.TestCase):

    def setUp(self):
        self.shell = read(SHELL)
        self.hdr = read(SHELL_HDR)
        self.pure = read(PURE_HDR)
        self.base = read(BASE_HDR)
        pre, _ = audit(self.shell, self.hdr, self.pure, self.base)
        self.assertEqual([], pre, "controls need a green starting point")

    def _fails_with(self, mutated_shell, needle, hdr=None, pure=None,
                    base=None):
        failures, _ = audit(mutated_shell, hdr or self.hdr, pure or self.pure,
                            base or self.base)
        self.assertTrue(
            any(needle in f for f in failures),
            "expected a failure containing %r; got:\n  %s"
            % (needle, "\n  ".join(failures) or "NOTHING (vacuous)"))

    def test_deleted_guard_in_an_override_reddens_P1(self):
        #
        # The mutation must land INSIDE getPixelInformation, not at the first
        # match in the file.  The first `if (!presentable())` in this shell is
        # publishPresentation()'s, which P1 deliberately does not police -- so a
        # file-wide anchor mutates a method outside the subject set and the
        # control reads as a vacuous gate when the gate is right.  Same class as
        # `a-mutation-anchor-can-match-more-than-it-means`, in the direction
        # where the anchor matches something real and WRONG.
        #
        start = self.shell.find(CLASS + "::getPixelInformation")
        self.assertNotEqual(start, -1, "mutation anchor matched nothing")
        m = re.compile(r"if\s*\(\s*!\s*presentable\s*\(\s*\)\s*\)").search(
            self.shell, start)
        self.assertIsNotNone(
            m, "mutation anchor matched nothing -- a mutation that matches "
               "nothing prints ok")
        mutated = self.shell[:m.start()] + "if (false)" + self.shell[m.end():]
        failures, _ = audit(mutated, self.hdr, self.pure)
        self.assertTrue(
            any("P1:" in f and "getPixelInformation" in f for f in failures),
            "expected P1 to name getPixelInformation; got:\n  %s"
            % ("\n  ".join(failures) or "NOTHING (vacuous)"))

    def test_decode_published_ahead_of_the_guard_reddens_P5(self):
        #
        # Hoist one decode above the guard.  This is the defect P5 exists for:
        # a width derived from IG_PRESENT_INVALID32 published as IGPresentWidth
        # is byte-identical, in the IORegistry, to a width derived from four
        # registers that genuinely read.
        #
        #
        # The mutation must land INSIDE publishPresentation(), the only method
        # P5 polices.  sliceScanout() now holds the first `if (!presentable())`
        # in the file, so a file-wide anchor hoists the decode into a method P5
        # never reads and the control prints vacuous while the gate is right --
        # the trap test_deleted_guard_in_an_override_reddens_P1 documents above.
        # The search starts at the DEFINITION, so a prose mention of the name
        # cannot move it ahead of sliceScanout(), and it must not leave the
        # method body.
        #
        anchor = '    if (!presentable()) {'
        defn = re.compile(r"^void\s+" + CLASS + r"::publishPresentation\s*\(",
                          re.M).search(self.shell)
        self.assertIsNotNone(defn, "mutation anchor matched nothing -- "
                                   "publishPresentation() definition not found")
        at = self.shell.find(anchor, defn.end())
        self.assertNotEqual(at, -1, "mutation anchor moved -- a mutation that "
                                    "matches nothing prints ok")
        self.assertIsNone(
            re.search(r"^\}", self.shell[defn.end():at], re.M),
            "mutation anchor left publishPresentation() -- the first guard "
            "after its definition belongs to another method")
        hoisted = ('    setProperty("IGPresentWidth",\n'
                   '                (UInt64) igPresentWidth(fCtl, fStride, '
                   'fSize, fSurf,\n'
                   '                                        fApertureBytes), '
                   '32);\n')
        mutated = self.shell[:at] + hoisted + self.shell[at:]
        self._fails_with(mutated, "P5:")

    def test_guard_losing_the_sampled_flag_reddens_P2(self):
        mutated = self.shell.replace(
            "return fSampleTaken && fPresentVerdict == IG_PRESENT_OK;",
            "return fPresentVerdict == IG_PRESENT_OK;")
        self.assertNotEqual(mutated, self.shell, "mutation anchor moved")
        self._fails_with(mutated, "P2:")

    def test_guard_becoming_an_inequality_reddens_P2(self):
        mutated = self.shell.replace(
            "fPresentVerdict == IG_PRESENT_OK",
            "fPresentVerdict != IG_PRESENT_ERR_SAMPLE_REFUSED")
        self.assertNotEqual(mutated, self.shell, "mutation anchor moved")
        self._fails_with(mutated, "P2:")

    def test_deleted_override_declaration_reddens_P0(self):
        #
        # THE control for this session's fix, and the measurement that shows the
        # old shape was vacuous here.  Delete getPixelInformation's declaration
        # from the subclass header and leave the DEFINITION in place.
        #
        # Under the old header-only derivation the name simply left the subject
        # set, so P1 stopped asking about it and every check stayed green -- an
        # absence of coverage reported as a positive result, which is
        # `intel-handoff28.md` SS3a's self-narrowing gate one module over.  The
        # demanded set is not the author's to forget, so P0 reddens and names it.
        #
        m = re.compile(
            r"[ \t]*virtual[^;]*\bgetPixelInformation\b[^;]*;[ \t]*\r?\n").search(
                self.hdr)
        self.assertIsNotNone(
            m, "mutation anchor matched nothing -- a mutation that matches "
               "nothing prints ok")
        mutated_hdr = self.hdr[:m.start()] + self.hdr[m.end():]
        self.assertNotIn("getPixelInformation", mutated_hdr.split("class ")[-1]
                         .split("};")[0],
                         "the declaration survived the cut")
        failures, facts = audit(self.shell, mutated_hdr, self.pure, self.base)
        self.assertTrue(
            any("P0:" in f and "getPixelInformation" in f for f in failures),
            "expected P0 to name getPixelInformation; got:\n  %s"
            % ("\n  ".join(failures) or "NOTHING (vacuous)"))
        #
        # And the method must STILL be in P1's subject set, because the union
        # picks it up off the base header.  A P0 that reddened while the method
        # fell out of P1 would trade one silent narrowing for another.
        #
        self.assertIn("getPixelInformation", facts["answering"],
                      "the union must keep a demanded method the subclass "
                      "header no longer declares")

    def test_base_header_with_no_pure_virtuals_refuses(self):
        #
        # A gate reporting nothing when its anchor moves is indistinguishable
        # from a green one -- `3050-handoff30.md` SS2b.  Make every pure virtual
        # concrete and the demanded set collapses; the audit must REFUSE rather
        # than fall back on the subclass's own declarations.
        #
        mutated_base = re.sub(r"\)\s*=\s*0\s*;", ");", self.base)
        self.assertNotEqual(mutated_base, self.base, "mutation anchor moved")
        failures, _ = audit(self.shell, self.hdr, self.pure, mutated_base)
        self.assertTrue(
            any("REFUSED" in f and "pure virtuals" in f for f in failures),
            "expected a REFUSED naming the collapsed pure-virtual set; got:\n  %s"
            % ("\n  ".join(failures) or "NOTHING (vacuous)"))

    def test_pure_virtual_with_a_trailing_macro_stays_demanded(self):
        #
        # `intel-handoff32.md` SS6 item 3 predicted this module's remaining hole
        # was a `= 0` written across a LINE BREAK.  MEASURED, that is not a hole
        # at all: Python's whitespace class already matches a newline, so every
        # wrapped spelling was found by the old tail test and a whitespace-
        # collapsing "fix" changes the output on no input whatsoever.
        #
        # The real hole is any TOKEN standing AFTER the `= 0`.  Apple spells
        # pure virtuals `= 0 APPLE_KEXT_PURE;` in several graphics headers, and
        # an attribute lands in the same place.  Under a tail test the chunk no
        # longer ENDS in `= 0`, so the method dropped OUT of the demanded set,
        # P0 stopped asking about it, and the gate reported green over a method
        # it never considered -- SS1a's defect arrived at from the other side.
        #
        # The assertion is set EQUALITY, not non-emptiness: a parser that
        # answered pure for every declaration would also keep the method.
        #
        before = demanded_pure_virtuals(self.base)
        self.assertIn("getPixelInformation", before,
                      "precondition: the method is demanded before mutation")

        mutated_base, n = re.subn(r"\)(\s*)=(\s*)0(\s*);",
                                  r")\1=\2 0 APPLE_KEXT_PURE;", self.base)
        self.assertNotEqual(
            n, 0, "mutation anchor matched nothing -- a mutation that matches "
                  "nothing prints ok")

        after = demanded_pure_virtuals(mutated_base)
        self.assertEqual(
            sorted(after), sorted(before),
            "a trailing macro after `= 0` must not shrink the demanded set; "
            "lost: %s" % sorted(set(before) - set(after)))

        #
        # And the whole audit must stay GREEN over the mutated base header.  A
        # demanded set that survived the parse but failed to reach P0 would
        # satisfy the equality above and still leave the gate blind.
        #
        failures, _ = audit(self.shell, self.hdr, self.pure, mutated_base)
        self.assertEqual(
            failures, [],
            "a trailing macro is a spelling change, not a defect; got:\n  %s"
            % "\n  ".join(failures))

    def test_default_argument_zero_is_not_a_pure_virtual(self):
        #
        # The anti-degeneracy half of the test above.  A default argument
        # carries its own `= 0` INSIDE the parameter list:
        #
        #     virtual IOReturn setAttribute(IOSelect a, uintptr_t v = 0);
        #
        # A parser that widened far enough to accept a trailing macro by
        # searching the chunk for `= 0` ANYWHERE would call that concrete
        # method pure, and P0 would then demand a declaration the subclass has
        # no reason to carry -- a gate reddening correct shipping code, which
        # is `intel-handoff26.md` SS2a's trap.  Walking the balanced parameter
        # list is what buys both halves at once.
        #
        concrete = "    virtual IOReturn zzzConcrete(IOSelect a, int v = 0);\n"
        pure = "    virtual IOReturn zzzPure(IOSelect a, int v = 0) = 0;\n"

        names = demanded_pure_virtuals(concrete)
        self.assertNotIn("zzzConcrete", names,
                         "a default argument's own `= 0` must not make a "
                         "concrete declaration look pure")

        names = demanded_pure_virtuals(pure)
        self.assertIn("zzzPure", names,
                      "a pure virtual whose LAST parameter has a default "
                      "argument is still pure")

    def test_renamed_subject_refuses_rather_than_passes(self):
        mutated = self.shell.replace(CLASS + "::", "SomethingElse::")
        failures, _ = audit(mutated, self.hdr, self.pure)
        self.assertTrue(
            failures,
            "a renamed subject must REFUSE loudly; silence is "
            "indistinguishable from green")

    def test_missing_verdict_family_refuses(self):
        failures, _ = audit(self.shell, self.hdr,
                            self.pure.replace("#define " + OK_CONSTANT,
                                              "#define IG_SOMETHING_ELSE"))
        self.assertTrue(any("REFUSED" in f for f in failures),
                        "a missing verdict family must refuse")

    def test_empty_override_set_refuses(self):
        failures, _ = audit(self.shell,
                            self.hdr.replace("APPLE_KEXT_OVERRIDE", "/*x*/"),
                            self.pure)
        self.assertTrue(any("REFUSED" in f for f in failures),
                        "an unmatched declaration pattern must refuse")


if __name__ == "__main__":
    unittest.main(verbosity=2)
