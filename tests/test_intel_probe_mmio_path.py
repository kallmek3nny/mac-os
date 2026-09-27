#!/usr/bin/env python3
"""Shape gate over IntelDisplayProbe.cpp's MMIO path.

No C++ suite can execute a line of IntelDisplayProbe.cpp -- it pulls in
IOService, which does not exist off macOS. So the only observer available for
the shell is a structural one: parse the source, assert the properties, and
prove the assertions can fail by mutating the source IN MEMORY. The live tree
is never written to.

Design rule, carried from tests/test_probe_register_access_refusals.py: the
real tree and every mutant go through ONE audit(), so a mutant cannot pass by
taking a different code path than the live check.

What this module pins
---------------------
Property 1: the file contains EXACTLY ONE dereference through a volatile
pointer, and it is inside IntelDisplayProbe::regRead32.

Property 2: that dereference is DOMINATED by the igReadDecision() switch. The
shape is a fall-through rather than an IG_READ_OK arm -- every refusal arm
returns IG_MMIO_INVALID and the load sits BELOW the switch's closing brace,
reached only when `default: break;` runs. So the assertion is an offset
relation (deref after switch end) plus a refusal-per-arm check, not a search
for a case label that does not exist.

Property 3: every verdict in the family below its own limit has an arm. The
loop bounds on IG_READ_VERDICT_LIMIT read out of IntelDisplay.hpp, never a
hand-counted total -- handoff20 SS1 is the cost: igModeVerdictName() had arms
0..5 while its family's limit was 6, IG_MODE_SRC_MISMATCH published the unknown
string, and all four gates were green over it. MEASURED 2026-08-19: this
family's limit is 4u and the comment beside it says "the largest verdict
above", so it is INCLUSIVE. A loop written for the exclusive convention would
redden a pristine tree.

Property 4: the sole dereference is the operand of a RETURN, so it cannot be a
store. Combined with zero OSWrite* calls, that is the structural form of the
file's own header claim that there is no write path in it at all.

Property 5: fBar0Base and fBar0Length are assigned ONLY in init(), mapBar0()
and unmapBar0(), and both are zeroed in init() and unmapBar0(). A stale bound
must refuse rather than trust -- handoff22 SS1 is why the length exists at all.

Why that is the property worth pinning. intel-handoff26.md SS6 item 4 names
the hole every sibling gate in this family still has: they spell their subject
against ``regRead32(`` BY NAME, so a second MMIO accessor added later is not
considered a publisher at all and the gate reports NOTHING rather than
failing. That is the vacuity direction. This module therefore derives the
DEREFERENCE set structurally -- a load through a volatile-qualified pointer --
rather than the ACCESSOR set by name, so a second accessor reddens on the
commit that adds it.

Why comment stripping is mandatory rather than stylistic
--------------------------------------------------------
regRead32()'s own comment block discusses IG_MMIO_INVALID, the guard order and
the dereference by name, and IntelDisplay.hpp's prose quotes the cast. A gate
reading prose would pass on a function whose guards were all deleted. The
stripper blanks comments and string literals while PRESERVING BYTE OFFSETS --
each removed character becomes a space and newlines are kept -- so every index
into the stripped text is also an index into the original.

Why a cast is not a dereference
-------------------------------
MEASURED 2026-08-19: ``volatile`` occurs exactly twice in the subject.
mapBar0() casts getVirtualAddress() to ``(volatile UInt8 *)`` and STORES it;
regRead32() writes ``*(volatile UInt32 *) (fBar0Base + offset)`` and LOADS
through it. Only the second is an MMIO access. The two are told apart by the
``*`` immediately preceding the cast's open paren, so DEREF_RE requires it --
matching the cast alone would count the store site and make the count 2 on a
correct tree.

Refusal rather than silence
---------------------------
If the subject file, the method, or the dereference cannot be found, audit()
reports a REFUSAL string. A gate that reports nothing when its anchor moves is
indistinguishable from a green one.
"""
import os
import re
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, os.pardir))

SUBJECT = os.path.join(_ROOT, "phase0-kext", "IntelDisplayProbe.cpp")

# The method that is allowed to dereference. Spelled with the class qualifier
# because the definition in the .cpp carries it and the .hpp declaration does
# not -- matching the bare name would find the declaration in a header this
# module does not read, and would find nothing here.
ACCESSOR = "IntelDisplayProbe::regRead32"

# A LOAD through a volatile pointer: a '*' applied to a volatile-qualified
# pointer cast. The leading '*' is what separates the load at :289 from the
# STORE of the cast pointer in mapBar0(); see the module docstring.
DEREF_RE = re.compile(r"\*\s*\(\s*volatile\b[^;{}]*?\*\s*\)")

# The verdict family lives in the dependency-free header rather than in the
# shell, so the exhaustiveness loop reads it from there. Deriving the case set
# instead of spelling it means a sixth verdict added to the header with no arm
# in the switch reddens on the commit that adds it, naming the verdict.
DISPLAY_HDR = os.path.join(_ROOT, "phase0-kext", "IntelDisplay.hpp")

VERDICT_DEF_RE = re.compile(r"^#define\s+(IG_READ_[A-Z0-9_]+)\s+(\d+)u", re.M)
LIMIT_NAME = "IG_READ_VERDICT_LIMIT"
OK_NAME = "IG_READ_OK"

CASE_RE = re.compile(r"\bcase\s+(IG_READ_[A-Z0-9_]+)\s*:")
DEFAULT_RE = re.compile(r"\bdefault\s*:")
SWITCH_RE = re.compile(r"\bswitch\s*\(\s*igReadDecision\s*\(")

# Any of the OSWrite* family is an MMIO store this file must not contain.
OSWRITE_RE = re.compile(r"\bOSWrite[A-Za-z0-9_]*\s*\(")

# An assignment, not a comparison -- the negative lookahead keeps `==` out.
FIELD_ASSIGN_RE = re.compile(r"\bfBar0(?:Base|Length)\s*=(?!=)")

# The three methods permitted to touch the base and the length. mapBar0() is
# the only one that assigns a non-zero value; the other two zero them, so a
# stale bound refuses rather than trusts.
FIELD_OWNERS = (
    "IntelDisplayProbe::init",
    "IntelDisplayProbe::mapBar0",
    "IntelDisplayProbe::unmapBar0",
)
ZEROING_OWNERS = ("IntelDisplayProbe::init", "IntelDisplayProbe::unmapBar0")


def strip_comments_and_strings(text):
    """Blank comments and string/char literals, preserving every byte offset.

    Each removed character becomes a space; newlines are kept. So len() is
    unchanged and any index into the result indexes the original too, which is
    what lets a match found here be reported against a real line number.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif ch == "/" and nxt == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            while i < n and i < n and text[i:i + 2] == "*/":
                out[i] = out[i + 1] = " "
                i += 2
        elif ch in "\"'":
            quote = ch
            out[i] = " "
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    out[i] = " "
                    i += 1
                if i < n and text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def method_span(code, qualified_name):
    """Return (start, end) byte offsets of a method BODY, braces included.

    Found by a balanced-brace walk from the first '{' after the name, never by
    a line regex. handoff25.md SS1c is the cost of the alternative: a character
    class excluding only the statement terminator is not scoped to the
    statement, and runs past a closing paren into the next one.
    """
    at = code.find(qualified_name)
    if at < 0:
        return None
    open_at = code.find("{", at)
    if open_at < 0:
        return None
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return (open_at, i + 1)
    return None


def line_of(code, offset):
    return code.count("\n", 0, offset) + 1


def switch_span(code, span):
    """Return ((start, end) or None, total_switch_count) for the decision switch.

    The controlling expression is skipped by a balanced-PAREN walk and the body
    by a balanced-BRACE walk. Both matter: igReadDecision() takes three
    arguments, one of which contains a comparison, and the arms contain braces
    of their own inside the IGLOG calls' argument lists.
    """
    hits = [m.start() for m in SWITCH_RE.finditer(code)]
    inside = [h for h in hits if span[0] <= h < span[1]]
    if len(hits) != 1 or len(inside) != 1:
        return None, len(hits)

    at = inside[0]
    paren = code.find("(", at)
    depth, close = 0, -1
    for i in range(paren, span[1]):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                close = i
                break
    if close < 0:
        return None, len(hits)

    open_at = code.find("{", close)
    if open_at < 0 or open_at >= span[1]:
        return None, len(hits)

    depth = 0
    for i in range(open_at, span[1]):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return (open_at, i + 1), len(hits)
    return None, len(hits)


def statement_text(code, offset):
    """The statement containing `offset`, back to the previous boundary.

    Walked back to the nearest `;`, `{`, `}` or `:` rather than to the start of
    the line, because handoff25 SS1c is what a line- or class-based scope costs:
    a regex excluding only the terminator is not scoped to the statement.
    """
    start = max(code.rfind(";", 0, offset),
                code.rfind("{", 0, offset),
                code.rfind("}", 0, offset),
                code.rfind(":", 0, offset))
    return code[start + 1:offset + 1]


def read_verdicts(header):
    """Return (name -> value for the arms, limit) from the header's defines.

    The limit is popped out of the family: it is a bound on the others, not a
    verdict a switch can have an arm for.
    """
    found = {m.group(1): int(m.group(2))
             for m in VERDICT_DEF_RE.finditer(header)}
    limit = found.pop(LIMIT_NAME, None)
    return found, limit


def arm_spans(code, sw_span):
    """Ordered [(label, start, end)] for each arm of the switch body.

    An arm runs from its own label to the next label, so a `return` belonging
    to the arm below it cannot be miscredited upwards. `default` is included
    under that name because it is a real arm of this switch -- it is the one
    that falls through to the load.
    """
    marks = [(m.start(), m.group(1)) for m in CASE_RE.finditer(code)
             if sw_span[0] <= m.start() < sw_span[1]]
    marks += [(m.start(), "default") for m in DEFAULT_RE.finditer(code)
              if sw_span[0] <= m.start() < sw_span[1]]
    marks.sort()

    out = []
    for i, (at, label) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else sw_span[1] - 1
        out.append((label, at, end))
    return out


def audit(source, header):
    """Return a findings dict over one subject's source text.

    Every key is either a measurement or a REFUSAL string. Nothing here
    asserts; the tests do, so that a mutant and the live tree are measured by
    the identical code.
    """
    code = strip_comments_and_strings(source)
    hdr = strip_comments_and_strings(header)
    out = {
        "accessor": None,
        "derefs_total": None,
        "derefs_inside": None,
        "derefs_outside": [],
        "switch": None,
        "switch_count": 0,
        "deref_after_switch": None,
        "deref_statement": None,
        "arms": {},
        "arms_without_refusal": [],
        "verdicts": {},
        "limit": None,
        "unhandled_verdicts": [],
        "oswrites": [],
        "field_assign_owners": {},
        "stray_field_assigns": [],
        "zeroing": {},
    }

    # ---- the verdict family, read from the header rather than spelled here
    verdicts, limit = read_verdicts(hdr)
    out["verdicts"] = verdicts
    out["limit"] = limit

    span = method_span(code, ACCESSOR)
    if span is None:
        out["accessor"] = "REFUSAL: %s not found" % ACCESSOR
        return out
    out["accessor"] = span

    # ---- property 1: the dereference set
    hits = [m.start() for m in DEREF_RE.finditer(code)]
    out["derefs_total"] = len(hits)
    out["derefs_inside"] = sum(1 for h in hits if span[0] <= h < span[1])
    out["derefs_outside"] = [line_of(code, h) for h in hits
                             if not (span[0] <= h < span[1])]

    # ---- property 4: the load is the operand of a return, so it is not a store
    inside = [h for h in hits if span[0] <= h < span[1]]
    if len(inside) == 1:
        out["deref_statement"] = statement_text(code, inside[0]).strip()
    out["oswrites"] = [line_of(code, m.start())
                       for m in OSWRITE_RE.finditer(code)]

    # ---- property 2: domination by the igReadDecision switch
    sw, sw_total = switch_span(code, span)
    out["switch_count"] = sw_total
    if sw is None:
        out["switch"] = "REFUSAL: igReadDecision switch not found in %s" % ACCESSOR
        return out
    out["switch"] = sw
    if len(inside) == 1:
        out["deref_after_switch"] = inside[0] >= sw[1]

    arms = arm_spans(code, sw)
    out["arms"] = {label: line_of(code, at) for label, at, _ in arms}
    for label, at, end in arms:
        if label == "default":
            continue
        body = code[at:end]
        if "return" not in body or "IG_MMIO_INVALID" not in body:
            out["arms_without_refusal"].append(label)

    # ---- property 3: exhaustiveness, bounded on the family's OWN limit
    #
    # IG_READ_OK is deliberately NOT expected as a case label: it is the
    # fall-through, handled by `default: break;` dropping to the load. Every
    # other verdict from 0 through the INCLUSIVE limit must have an arm.
    by_value = {}
    for name, value in verdicts.items():
        by_value.setdefault(value, []).append(name)
    if limit is not None:
        ok_value = verdicts.get(OK_NAME)
        for value in range(0, limit + 1):
            names = by_value.get(value, [])
            if not names:
                out["unhandled_verdicts"].append("value %d has no name" % value)
                continue
            if value == ok_value:
                continue
            if not any(name in out["arms"] for name in names):
                out["unhandled_verdicts"].append(names[0])

    # ---- property 5: who may touch the base and the length
    owners = {}
    for name in FIELD_OWNERS:
        owner_span = method_span(code, name)
        if owner_span is not None:
            owners[name] = owner_span
    out["field_assign_owners"] = {name: 0 for name in owners}

    for m in FIELD_ASSIGN_RE.finditer(code):
        home = None
        for name, owner_span in owners.items():
            if owner_span[0] <= m.start() < owner_span[1]:
                home = name
                break
        if home is None:
            out["stray_field_assigns"].append(line_of(code, m.start()))
        else:
            out["field_assign_owners"][home] += 1

    for name in ZEROING_OWNERS:
        owner_span = owners.get(name)
        if owner_span is None:
            out["zeroing"][name] = "REFUSAL: %s not found" % name
            continue
        body = code[owner_span[0]:owner_span[1]]
        base_zero = re.search(r"fBar0Base\s*=\s*(?:nullptr|NULL|0)\s*;", body)
        len_zero = re.search(r"fBar0Length\s*=\s*0\s*;", body)
        out["zeroing"][name] = bool(base_zero) and bool(len_zero)

    return out


def live_source():
    with open(SUBJECT, "r", encoding="utf-8") as handle:
        return handle.read()


def live_header():
    with open(DISPLAY_HDR, "r", encoding="utf-8") as handle:
        return handle.read()


class MmioPathTests(unittest.TestCase):
    """Property 1 plus the controls that prove it can fail."""

    def setUp(self):
        self.source = live_source()
        self.header = live_header()
        self.found = audit(self.source, self.header)

    def test_the_subject_and_its_accessor_exist(self):
        """Refuse rather than pass vacuously if the anchors have moved."""
        self.assertTrue(os.path.isfile(SUBJECT), "%s is missing" % SUBJECT)
        self.assertNotIsInstance(
            self.found["accessor"], str,
            "audit() could not find %s. Every other assertion in this module "
            "would then be measuring nothing." % ACCESSOR)

    def test_the_stripper_preserves_offsets(self):
        """An offset-shifting stripper reports the wrong line for every hit."""
        stripped = strip_comments_and_strings(self.source)
        self.assertEqual(len(self.source), len(stripped))
        self.assertEqual(self.source.count("\n"), stripped.count("\n"))
        self.assertNotIn(
            "IG_MMIO_INVALID is 0xFFFFFFFF", stripped,
            "comment prose survived stripping, so this gate reads the "
            "documentation of the property instead of the property.")

    def test_exactly_one_dereference_and_it_is_in_the_accessor(self):
        """Property 1. The whole MMIO surface is one load, inside one method."""
        self.assertEqual(
            1, self.found["derefs_total"],
            "expected exactly one volatile dereference in %s, found %d"
            % (os.path.basename(SUBJECT), self.found["derefs_total"]))
        self.assertEqual(
            [], self.found["derefs_outside"],
            "MMIO dereference outside %s at line(s) %s -- the bounds and "
            "policy guards live in that method, so a load anywhere else is "
            "unguarded by construction."
            % (ACCESSOR, self.found["derefs_outside"]))
        self.assertEqual(1, self.found["derefs_inside"])

    # ------------------------------------------------------- negative controls

    def test_control_a_second_dereference_elsewhere_reddens(self):
        """The vacuity direction this module exists to close.

        A second accessor added later must REDDEN rather than be silently
        excluded from the subject set -- intel-handoff26.md SS6 item 4.
        """
        anchor = "void IntelDisplayProbe::free"
        self.assertEqual(
            1, self.source.count(anchor),
            "mutation anchor %r occurs %d times; the mutation below assumes "
            "exactly one." % (anchor, self.source.count(anchor)))
        mutant = self.source.replace(
            anchor,
            "UInt32 IntelDisplayProbe::sneak(UInt32 o)\n"
            "{\n"
            "    return *(volatile UInt32 *) (fBar0Base + o);\n"
            "}\n\n" + anchor,
            1)
        self.assertNotEqual(self.source, mutant, "the mutation applied to nothing")

        found = audit(mutant, self.header)
        self.assertEqual(2, found["derefs_total"])
        self.assertTrue(
            found["derefs_outside"],
            "a dereference outside %s was not reported, so this gate cannot "
            "see a second MMIO accessor." % ACCESSOR)

    def test_control_a_renamed_accessor_refuses(self):
        """A moved anchor must produce a REFUSAL, never silence."""
        mutant = self.source.replace(ACCESSOR, "IntelDisplayProbe::regReadXX")
        self.assertNotEqual(self.source, mutant, "the mutation applied to nothing")
        found = audit(mutant, self.header)
        self.assertIsInstance(
            found["accessor"], str,
            "the accessor was renamed and audit() did not refuse.")
        self.assertIn("REFUSAL", found["accessor"])

    # ------------------------------------------------------------ property 2

    def test_the_dereference_is_dominated_by_the_decision_switch(self):
        """Property 2. The load is reachable only past every refusal arm.

        The shape is a FALL-THROUGH, not an IG_READ_OK arm: each refusal arm
        returns IG_MMIO_INVALID and `default: break;` drops to the load below
        the switch's closing brace. So the claim is an offset relation plus a
        refusal-per-arm check -- searching for a `case IG_READ_OK:` that does
        not exist would redden a pristine tree.
        """
        self.assertNotIsInstance(
            self.found["switch"], str,
            "the igReadDecision switch was not found: %s"
            % (self.found["switch"],))
        self.assertEqual(
            1, self.found["switch_count"],
            "expected exactly one igReadDecision switch in the file, found %d"
            % self.found["switch_count"])
        self.assertTrue(
            self.found["deref_after_switch"],
            "the MMIO load is not below the decision switch, so it is not "
            "dominated by the guards -- an unguarded read is exactly what "
            "handoff22 SS1 measured going 0x20000 bytes past a mapping.")
        self.assertEqual(
            [], self.found["arms_without_refusal"],
            "switch arm(s) %s do not return IG_MMIO_INVALID. An arm that falls "
            "through reaches the load with its own refusal ignored."
            % self.found["arms_without_refusal"])

    # ------------------------------------------------------------ property 3

    def test_every_verdict_below_the_family_limit_has_an_arm(self):
        """Property 3, bounded on the family's own INCLUSIVE limit.

        handoff20 SS1: igModeVerdictName() had arms 0..5 while its limit was 6,
        and all four gates were green while IG_MODE_SRC_MISMATCH published the
        unknown string. The limit is read from IntelDisplay.hpp so a sixth
        verdict added there with no arm here reddens on that commit.
        """
        self.assertIsNotNone(
            self.found["limit"],
            "%s was not found in IntelDisplay.hpp; the exhaustiveness loop "
            "would then bound on nothing." % LIMIT_NAME)
        self.assertIn(OK_NAME, self.found["verdicts"])
        self.assertEqual(
            [], self.found["unhandled_verdicts"],
            "verdict(s) %s have no arm in the switch. A verdict with no arm "
            "falls to `default` and reaches the dereference."
            % self.found["unhandled_verdicts"])
        self.assertNotIn(
            OK_NAME, self.found["arms"],
            "%s has an explicit case arm now. That is not wrong in itself, but "
            "this gate's domination proof assumes the fall-through shape and "
            "must be re-derived." % OK_NAME)

    # ------------------------------------------------------------ property 4

    def test_the_sole_access_is_a_load_and_there_is_no_write_path(self):
        """Property 4. The file's header claim, made structural.

        A dereference that is the operand of a `return` cannot be the target of
        an assignment. Together with zero OSWrite* calls that is the whole
        no-write-path claim, rather than prose asserting it.
        """
        self.assertIsNotNone(self.found["deref_statement"])
        self.assertTrue(
            self.found["deref_statement"].lstrip().startswith("return"),
            "the MMIO dereference is not the operand of a return; its "
            "statement reads %r, which may be a STORE to hardware."
            % self.found["deref_statement"])
        self.assertEqual(
            [], self.found["oswrites"],
            "OSWrite* call(s) at line(s) %s. This file is documented as having "
            "no write path at all." % self.found["oswrites"])

    # ------------------------------------------------------------ property 5

    def test_the_base_and_length_are_owned_by_three_methods(self):
        """Property 5. A stale bound must refuse rather than trust.

        fBar0Length exists because handoff22 SS1 found the policy range is not
        the mapping. It is only load-bearing if nothing outside mapBar0() can
        set it and both zeroing paths actually zero it.
        """
        self.assertEqual(
            [], self.found["stray_field_assigns"],
            "fBar0Base/fBar0Length assigned outside %s at line(s) %s"
            % (list(FIELD_OWNERS), self.found["stray_field_assigns"]))
        for name in FIELD_OWNERS:
            self.assertIn(name, self.found["field_assign_owners"],
                          "%s was not found in the subject" % name)
            self.assertGreater(
                self.found["field_assign_owners"][name], 0,
                "%s assigns neither field; the ownership set is stale" % name)
        for name in ZEROING_OWNERS:
            self.assertIs(
                True, self.found["zeroing"][name],
                "%s does not zero BOTH fBar0Base and fBar0Length, so a stale "
                "bound would be trusted after unmap." % name)

    # ------------------------------------------------------- negative controls

    def test_control_hoisting_the_load_above_the_switch_reddens(self):
        """Property 2's control: an undominated load must be reported."""
        anchor = "    switch (igReadDecision("
        self.assertEqual(
            1, self.source.count(anchor),
            "mutation anchor %r occurs %d times"
            % (anchor, self.source.count(anchor)))
        mutant = self.source.replace(
            anchor,
            "    return *(volatile UInt32 *) (fBar0Base + offset);\n" + anchor,
            1)
        found = audit(mutant, self.header)
        self.assertEqual(2, found["derefs_total"])
        self.assertEqual(
            2, found["derefs_inside"],
            "the hoisted load landed outside the accessor; this control is "
            "then testing property 1 rather than property 2.")

    def test_control_an_arm_that_stops_refusing_reddens(self):
        """Property 2's other control: a break-instead-of-return arm."""
        anchor = "    case IG_READ_UNMAPPED:"
        self.assertEqual(1, self.source.count(anchor))
        at = self.source.index(anchor)
        nxt = self.source.index("    case IG_READ_SENTINEL_OFFSET:", at)
        arm = self.source[at:nxt]
        self.assertIn("return IG_MMIO_INVALID;", arm)
        mutant = (self.source[:at]
                  + arm.replace("return IG_MMIO_INVALID;", "break;")
                  + self.source[nxt:])
        found = audit(mutant, self.header)
        self.assertEqual(
            ["IG_READ_UNMAPPED"], found["arms_without_refusal"],
            "an arm that stopped refusing was not reported")

    def test_control_a_deleted_arm_reddens_naming_the_verdict(self):
        """Property 3's control, and it must name WHICH verdict lost its arm."""
        anchor = "    case IG_READ_PAST_MAPPING:"
        self.assertEqual(1, self.source.count(anchor))
        mutant = self.source.replace(anchor, "    case 99u:", 1)
        found = audit(mutant, self.header)
        self.assertIn(
            "IG_READ_PAST_MAPPING", found["unhandled_verdicts"],
            "a deleted arm was not reported by name; got %s"
            % found["unhandled_verdicts"])

    def test_control_a_sixth_verdict_with_no_arm_reddens(self):
        """Property 3's forward control: the limit is read, not hand-counted.

        This mutates the HEADER, not the subject -- the point is that growing
        the family reddens the gate without anybody editing this module.
        """
        anchor = "#define IG_READ_VERDICT_LIMIT           4u"
        self.assertEqual(
            1, self.header.count(anchor),
            "header anchor %r occurs %d times"
            % (anchor, self.header.count(anchor)))
        mutant_hdr = self.header.replace(
            anchor,
            "#define IG_READ_TORN_READ               5u\n"
            "#define IG_READ_VERDICT_LIMIT           5u",
            1)
        found = audit(self.source, mutant_hdr)
        self.assertEqual(5, found["limit"])
        self.assertIn(
            "IG_READ_TORN_READ", found["unhandled_verdicts"],
            "a verdict added to the header with no arm in the switch was not "
            "reported, so this gate is blind to the family growing.")

    def test_control_a_store_through_the_pointer_reddens(self):
        """Property 4's control: turn the load into a write."""
        anchor = "    return *(volatile UInt32 *) (fBar0Base + offset);"
        self.assertEqual(
            1, self.source.count(anchor),
            "mutation anchor %r occurs %d times"
            % (anchor, self.source.count(anchor)))
        mutant = self.source.replace(
            anchor, "    *(volatile UInt32 *) (fBar0Base + offset) = 0u;", 1)
        found = audit(mutant, self.header)
        self.assertEqual(1, found["derefs_total"])
        self.assertFalse(
            found["deref_statement"].lstrip().startswith("return"),
            "a store through the MMIO pointer still read as a load")

    def test_control_a_stray_length_assignment_reddens(self):
        """Property 5's control: a fourth writer of the bound."""
        anchor = "void IntelDisplayProbe::free"
        self.assertEqual(1, self.source.count(anchor))
        mutant = self.source.replace(
            anchor,
            "void IntelDisplayProbe::widen(void)\n"
            "{\n"
            "    fBar0Length = 0x80000;\n"
            "}\n\n" + anchor,
            1)
        found = audit(mutant, self.header)
        self.assertTrue(
            found["stray_field_assigns"],
            "an assignment to fBar0Length outside the three owning methods "
            "was not reported.")

    def test_control_unmap_that_stops_zeroing_reddens(self):
        """Property 5's other control: the stale-bound-trusted case."""
        at = self.source.index("IntelDisplayProbe::unmapBar0")
        tail = self.source[at:]
        anchor = "fBar0Length = 0;"
        self.assertIn(anchor, tail)
        cut = at + tail.index(anchor)
        mutant = (self.source[:cut]
                  + "/* left stale */"
                  + self.source[cut + len(anchor):])
        found = audit(mutant, self.header)
        self.assertIs(
            False, found["zeroing"]["IntelDisplayProbe::unmapBar0"],
            "unmapBar0() stopped zeroing the length and the gate did not "
            "notice; every later read would trust a bound for a mapping that "
            "no longer exists.")

    def test_control_a_cast_alone_is_not_counted(self):
        """mapBar0()'s store of the cast pointer must not read as a load.

        If DEREF_RE dropped its leading '*', the correct tree would count two
        and property 1 would redden on pristine source.
        """
        self.assertEqual(
            [], DEREF_RE.findall("fBar0Base = (volatile UInt8 *) p->get();"))
        self.assertEqual(
            1, len(DEREF_RE.findall(
                "return *(volatile UInt32 *) (fBar0Base + offset);")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
