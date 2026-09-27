#!/usr/bin/env python3
"""Shape gate over IntelGtProbe.cpp's ring-write rehearsal.

No C++ suite can execute a line of IntelGtProbe.cpp -- it pulls in IOService,
which does not exist off macOS. So the only observer available for the shell is
a structural one: parse the source, assert the properties, and prove the
assertions can fail by mutating the source IN MEMORY. The live tree is never
written to.

WHY THIS MODULE EXISTS
----------------------
tests/test_intel_gt_ring.cpp drives IntelGtRing.hpp with 331 checks and proves
the WRITER is correct. It cannot prove the SHELL calls it, and it cannot prove
the shell publishes what it got back. intel-handoff26.md SS1 is that finding in
its sharpest form: igSampleVerdict() was a pure header function with 421 checks
over it and no callers, and every suite it had stayed green.

Deleting rehearseRingWrite()'s body, or unguarding the tail publication, or
collapsing the two verdicts into one, leaves all four gates green without this
module.

THE FIVE PROPERTIES
-------------------
1. The rehearsal CALLS the writer, and start() calls the rehearsal. A wiring
   that exists as a method nobody invokes is the exact defect this closes.

2. The batch verdict is published SEPARATELY from the ring verdict. They are
   refusals in two different layers -- a misaligned batch address and a ring
   with no room -- and igGtrWriteBatchStartToRing() takes the batch one as an
   out-param precisely so the caller does not collapse them. A single published
   word would send an operator looking in the wrong layer.

3. IGGtRingTailAfter is published ONLY behind igGtrRingSucceeded(). This is the
   one guarded value and the reason is a property of the writer rather than a
   preference: every refusal arm reports the tail UNCHANGED, so a refused write
   and a write that wrapped exactly back to its starting offset are
   byte-identical in that field. Publishing it unconditionally is a fabricated
   measurement in the shape the NVIDIA track has now found five times.

4. The head and tail fed to the writer are guarded by fSampleTaken. A refused
   read answers IG_MMIO_INVALID (0xFFFFFFFF); handing that in as a byte offset
   makes the writer refuse for BAD_GEOMETRY, and the published verdict would
   then describe a register nobody read rather than the composition.

5. The file still contains NO MMIO write path. The rehearsal stores into host
   memory; if it ever grew an OSWrite* or a RING_TAIL store, the banner in
   IntelGtProbe.hpp and intel-handoff36.md SS1 would both become false.

Design rule, carried from tests/test_intel_probe_mmio_path.py: the subject is
scanned COMMENT- AND STRING-STRIPPED. The fix's own comments name every key and
every guard it adds, so a prose-reading gate would report the property as held
on a file whose guards were all deleted.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SUBJECT = os.path.join(ROOT, "phase0-kext", "IntelGtProbe.cpp")
SUBJECT_HDR = os.path.join(ROOT, "phase0-kext", "IntelGtProbe.hpp")

REHEARSE = "IntelGtProbe::rehearseRingWrite"
PUBLISHER = "IntelGtProbe::publishRingWrite"
START = "IntelGtProbe::start"
INIT = "IntelGtProbe::init"

WRITER_CALL = "igGtrWriteBatchStartToRing"
SUCCEEDED = "igGtrRingSucceeded"

RING_VERDICT_KEY = "IGGtRingVerdict"
BATCH_VERDICT_KEY = "IGGtRingBatchVerdict"
TAIL_KEY = "IGGtRingTailAfter"
ATTEMPTED_KEY = "IGGtRingWriteAttempted"

SAMPLE_FLAG = "fSampleTaken"

# The two fields the writer fills through out-params, and the field the
# rehearsal must consult before trusting a sampled register.
RING_HEAD = "fRingHead"
RING_TAIL = "fRingTail"

SETPROP_RE = re.compile(r"setProperty\s*\(\s*\"([A-Za-z0-9_]+)\"")


def mentions(text, field):
    """True when `field` occurs in `text` as a WHOLE identifier.

    Never a bare `field in text`, and the reason is a live false positive this
    module hit on its first run: fRingTail is a PREFIX of fRingTailAfter, which
    is the writer's out-param and legitimately sits outside the fSampleTaken
    guard. A substring test reported the sampled register as read there and the
    failure message described a defect that was not in the file.

    Same class as the NVIDIA track's substring control (`NAME not in text` is
    satisfied by any longer name NAME prefixes), arrived at from the opposite
    side: there the over-broad match made a control pass vacuously, here it made
    a correct property fail loudly. Both are fixed by matching identifiers as
    identifiers.
    """
    return re.search(r"\b" + re.escape(field) + r"\b(?![A-Za-z0-9_])",
                     text) is not None
OSWRITE_RE = re.compile(r"\bOSWrite[A-Za-z0-9_]*\s*\(")
VOLATILE_STORE_RE = re.compile(
    r"\*\s*\(\s*volatile[^)]*\)[^;=]*=\s*[^=]")


def strip_comments_and_strings(text):
    """Blank comments and string/char literals, preserving every byte offset.

    Each removed character becomes a space; newlines are kept. So len() is
    unchanged and any index into the result indexes the original identically --
    which is what lets line_of() report a real line number for a hit found in
    the stripped text.

    String literals are blanked EXCEPT that setProperty's key needs to survive,
    so this function keeps string CONTENT and blanks only comments. The key
    names are the subject of three properties; blanking them would make this
    gate measure nothing. Comment prose is the thing that must go, because the
    fix's own comments name every key.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            for _ in range(min(2, n - i)):
                out.append(" ")
                i += 1
            continue
        if ch == '"':
            out.append(ch)
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i])
                    i += 1
                out.append(text[i])
                i += 1
            if i < n:
                out.append(text[i])
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def method_span(code, name):
    """[start, end) of a method body, found by a BALANCED brace walk.

    Never a regex over the following lines: handoff25.md SS1c is what that
    costs -- a class excluding only the statement terminator runs past the
    closing paren and lands on the next statement entirely.
    """
    at = code.find(name)
    if at < 0:
        return None
    brace = code.find("{", at)
    if brace < 0:
        return None
    depth = 0
    i = brace
    while i < len(code):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return (brace, i + 1)
        i += 1
    return None


def guarded_span(body, predicate):
    """[start, end) of the block guarded by `if (predicate...)`, or None.

    Returns the span of the guarded BODY, so a caller can ask what is inside it
    without a regex deciding where the statement ends.
    """
    at = body.find("if")
    while at >= 0:
        paren = body.find("(", at)
        if paren < 0:
            return None
        depth = 0
        i = paren
        while i < len(body):
            if body[i] == "(":
                depth += 1
            elif body[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        cond = body[paren:i + 1]
        if predicate in cond:
            brace = body.find("{", i)
            if brace < 0:
                return None
            depth = 0
            j = brace
            while j < len(body):
                if body[j] == "{":
                    depth += 1
                elif body[j] == "}":
                    depth -= 1
                    if depth == 0:
                        return (brace, j + 1)
                j += 1
            return None
        at = body.find("if", at + 2)
    return None


def audit(source):
    """Every measurement this module makes, over comment-stripped source.

    Returns a dict. Any anchor that cannot be found becomes a string starting
    with REFUSAL rather than a falsy value, so a moved anchor produces a loud
    failure instead of a gate that passes by measuring nothing.
    """
    code = strip_comments_and_strings(source)
    out = {
        "published": [],
        "tail_publications_total": 0,
        "tail_publications_guarded": 0,
        "oswrites": [],
        "volatile_stores": [],
    }

    reh = method_span(code, REHEARSE)
    if reh is None:
        out["rehearse"] = "REFUSAL: %s not found" % REHEARSE
    else:
        out["rehearse"] = reh

    pub = method_span(code, PUBLISHER)
    if pub is None:
        out["publisher"] = "REFUSAL: %s not found" % PUBLISHER
    else:
        out["publisher"] = pub

    start = method_span(code, START)
    if start is None:
        out["start"] = "REFUSAL: %s not found" % START
    else:
        out["start"] = start

    # ---- property 1: the writer is called, and the caller is called
    if isinstance(reh, tuple):
        body = code[reh[0]:reh[1]]
        out["writer_calls"] = body.count(WRITER_CALL + "(")
        out["consults_sample_flag"] = SAMPLE_FLAG in body
        out["reads_sampled_head"] = mentions(body, RING_HEAD)
        out["reads_sampled_tail"] = mentions(body, RING_TAIL)

        # Property 4: the sampled registers must be reached only inside a
        # block guarded by fSampleTaken. A read outside it is the refusal
        # sentinel being handed in as a byte offset.
        span = guarded_span(body, SAMPLE_FLAG)
        if span is None:
            out["sample_guard"] = "REFUSAL: no if(%s) block in %s" % (
                SAMPLE_FLAG, REHEARSE)
        else:
            guarded = body[span[0]:span[1]]
            outside = body[:span[0]] + body[span[1]:]
            out["sample_guard"] = True
            out["head_inside_guard"] = mentions(guarded, RING_HEAD)
            out["tail_inside_guard"] = mentions(guarded, RING_TAIL)
            out["head_outside_guard"] = mentions(outside, RING_HEAD)
            out["tail_outside_guard"] = mentions(outside, RING_TAIL)
    else:
        out["writer_calls"] = 0
        out["sample_guard"] = reh

    if isinstance(start, tuple):
        sbody = code[start[0]:start[1]]
        out["start_calls_rehearse"] = "rehearseRingWrite" in sbody
        out["start_calls_publisher"] = "publishRingWrite" in sbody
    else:
        out["start_calls_rehearse"] = False
        out["start_calls_publisher"] = False

    # ---- properties 2 and 3: what the publisher publishes, and how
    if isinstance(pub, tuple):
        pbody = code[pub[0]:pub[1]]
        out["published"] = [m.group(1) for m in SETPROP_RE.finditer(pbody)]

        span = guarded_span(pbody, SUCCEEDED)
        if span is None:
            out["success_guard"] = "REFUSAL: no if(%s) block in %s" % (
                SUCCEEDED, PUBLISHER)
            guarded = ""
        else:
            out["success_guard"] = True
            guarded = pbody[span[0]:span[1]]

        for m in SETPROP_RE.finditer(pbody):
            if m.group(1) != TAIL_KEY:
                continue
            out["tail_publications_total"] += 1
            if span is not None and span[0] <= m.start() < span[1]:
                out["tail_publications_guarded"] += 1
        out["guarded_keys"] = [m.group(1) for m in SETPROP_RE.finditer(guarded)]
    else:
        out["success_guard"] = pub
        out["guarded_keys"] = []

    # ---- property 5: still no write path anywhere in the file
    for m in OSWRITE_RE.finditer(code):
        out["oswrites"].append(line_of(code, m.start()))
    for m in VOLATILE_STORE_RE.finditer(code):
        out["volatile_stores"].append(line_of(code, m.start()))

    return out


def live_source():
    with open(SUBJECT, "r", encoding="utf-8") as handle:
        return handle.read()


class RingWriteShapeTests(unittest.TestCase):
    """The five properties plus the controls that prove each can fail."""

    def setUp(self):
        self.source = live_source()
        self.found = audit(self.source)

    # ------------------------------------------------------------- anchors

    def test_the_subject_and_its_anchors_exist(self):
        """Refuse rather than pass vacuously if the anchors have moved."""
        self.assertTrue(os.path.isfile(SUBJECT), "%s is missing" % SUBJECT)
        self.assertTrue(os.path.isfile(SUBJECT_HDR),
                        "%s is missing" % SUBJECT_HDR)
        for key in ("rehearse", "publisher", "start"):
            self.assertNotIsInstance(
                self.found[key], str,
                "audit() could not find the %s anchor: %s. Every other "
                "assertion in this module would then be measuring nothing."
                % (key, self.found[key]))

    def test_the_stripper_preserves_offsets(self):
        """An offset-shifting stripper reports the wrong line for every hit."""
        stripped = strip_comments_and_strings(self.source)
        self.assertEqual(len(self.source), len(stripped))
        self.assertEqual(self.source.count("\n"), stripped.count("\n"))
        self.assertNotIn(
            "every refusal arm of", stripped,
            "comment prose survived stripping, so this gate reads the "
            "documentation of the property instead of the property.")

    # ---------------------------------------------------------- property 1

    def test_the_rehearsal_calls_the_writer_and_start_calls_both(self):
        """Property 1. A header nothing calls is a header nothing runs.

        intel-handoff26.md SS1: igSampleVerdict() had 421 checks over it and no
        callers, and every suite it had stayed green.
        """
        self.assertEqual(
            1, self.found["writer_calls"],
            "expected exactly one %s() call inside %s, found %d"
            % (WRITER_CALL, REHEARSE, self.found["writer_calls"]))
        self.assertTrue(
            self.found["start_calls_rehearse"],
            "start() does not call rehearseRingWrite(). The wiring would then "
            "be a method nobody invokes, which is the defect this module "
            "exists to close rather than the fix for it.")
        self.assertTrue(
            self.found["start_calls_publisher"],
            "start() does not call publishRingWrite(), so the verdict never "
            "reaches the IORegistry and the rehearsal is unobservable.")

    def test_control_a_gutted_rehearsal_reddens(self):
        """The writer call deleted must be reported, not tolerated."""
        mutant = self.source.replace(WRITER_CALL, "igGtrNotCalledAtAll", 1)
        self.assertNotEqual(self.source, mutant, "the mutation applied to nothing")
        found = audit(mutant)
        self.assertEqual(
            0, found["writer_calls"],
            "the writer call was renamed away and this gate still counted it.")

    def test_control_start_not_calling_the_rehearsal_reddens(self):
        """The wiring's own failure mode: a method defined and never invoked."""
        anchor = "    rehearseRingWrite();"
        self.assertEqual(
            1, self.source.count(anchor),
            "mutation anchor %r occurs %d times; the mutation below assumes "
            "exactly one." % (anchor, self.source.count(anchor)))
        mutant = self.source.replace(anchor, "", 1)
        found = audit(mutant)
        self.assertFalse(
            found["start_calls_rehearse"],
            "the call was removed from start() and this gate did not notice.")

    # ---------------------------------------------------------- property 2

    def test_both_verdicts_are_published_separately(self):
        """Property 2. Two layers, two keys.

        igGtrWriteBatchStartToRing() takes the batch verdict as an out-param
        precisely so the caller does not collapse it into the ring verdict. A
        misaligned batch address and a ring with no room are refusals in
        different layers, and an operator handed one word looks in the wrong
        one.
        """
        published = self.found["published"]
        for key in (ATTEMPTED_KEY, RING_VERDICT_KEY, BATCH_VERDICT_KEY):
            self.assertIn(
                key, published,
                "%s is not published by %s. Published keys were %s."
                % (key, PUBLISHER, published))
        self.assertNotEqual(
            RING_VERDICT_KEY, BATCH_VERDICT_KEY,
            "the two verdict keys are the same string, so one overwrites the "
            "other in the registry.")

    def test_the_attempted_flag_is_published_first(self):
        """An absent key is a question, not an answer.

        handoff18 settled this for GSPEngineReadable: a flag that appears only
        when something went wrong is indistinguishable from an older kext that
        never published it. First, so it sorts above what it qualifies.
        """
        published = self.found["published"]
        self.assertTrue(published, "nothing is published at all")
        self.assertEqual(
            ATTEMPTED_KEY, published[0],
            "the first key %s publishes is %r, not %r."
            % (PUBLISHER, published[0], ATTEMPTED_KEY))

    def test_control_a_collapsed_verdict_pair_reddens(self):
        """Publishing the batch verdict under the ring key must be caught."""
        mutant = self.source.replace(
            '"%s"' % BATCH_VERDICT_KEY, '"%s"' % RING_VERDICT_KEY)
        self.assertNotEqual(self.source, mutant, "the mutation applied to nothing")
        found = audit(mutant)
        self.assertNotIn(
            BATCH_VERDICT_KEY, found["published"],
            "the batch key was collapsed onto the ring key and this gate still "
            "reported it published.")

    # ---------------------------------------------------------- property 3

    def test_the_tail_is_published_only_behind_the_success_guard(self):
        """Property 3, and the one guarded value in the publisher.

        Every refusal arm of igGtrWriteBatchStartToRing() reports the tail
        UNCHANGED, so a refused write and a write that wrapped exactly back to
        its starting offset are byte-identical in that field. Unconditional
        publication is a literal that reads as a reading.
        """
        self.assertNotIsInstance(
            self.found["success_guard"], str,
            "the success guard was not found: %s" % (self.found["success_guard"],))
        self.assertEqual(
            1, self.found["tail_publications_total"],
            "expected exactly one %s publication, found %d"
            % (TAIL_KEY, self.found["tail_publications_total"]))
        self.assertEqual(
            1, self.found["tail_publications_guarded"],
            "%s is published outside the if(%s) block. A refused write reports "
            "the tail UNCHANGED, so the published word would be a fabricated "
            "measurement." % (TAIL_KEY, SUCCEEDED))

    def test_the_verdicts_themselves_are_not_inside_the_guard(self):
        """The mirror of property 3, and the direction that goes quiet.

        A verdict published only on the success arm cannot report a failure at
        all -- the registry would carry the tail's absence and nothing saying
        why. Both verdicts must sit OUTSIDE the guard.
        """
        guarded = self.found["guarded_keys"]
        for key in (RING_VERDICT_KEY, BATCH_VERDICT_KEY, ATTEMPTED_KEY):
            self.assertNotIn(
                key, guarded,
                "%s is published inside the success guard, so a refusal "
                "publishes no explanation of itself." % key)
        self.assertEqual(
            [TAIL_KEY], guarded,
            "the success guard publishes %s; only %s belongs behind it."
            % (guarded, TAIL_KEY))

    def test_control_an_unguarded_tail_reddens(self):
        """The fabricated-measurement direction, made to fail."""
        anchor = "    if (igGtrRingSucceeded(fRingVerdict)) {"
        self.assertEqual(
            1, self.source.count(anchor),
            "mutation anchor %r occurs %d times; the mutation below assumes "
            "exactly one." % (anchor, self.source.count(anchor)))
        mutant = self.source.replace(anchor, "    if (true) {", 1)
        found = audit(mutant)
        self.assertIsInstance(
            found["success_guard"], str,
            "the success guard was replaced with if(true) and this gate still "
            "found a guard.")
        self.assertEqual(
            0, found["tail_publications_guarded"],
            "the tail was published unguarded and this gate still counted it "
            "as guarded.")

    # ---------------------------------------------------------- property 4

    def test_the_sampled_registers_are_read_only_behind_the_sample_flag(self):
        """Property 4. A refusal sentinel must not become a byte offset.

        regRead32() answers IG_MMIO_INVALID (0xFFFFFFFF) on every refusal. Fed
        in as a head, the writer refuses for BAD_GEOMETRY -- a verdict about a
        register nobody read, published as though it described the
        composition. fSampleTaken is what separates the two.
        """
        self.assertNotIsInstance(
            self.found["sample_guard"], str,
            "the fSampleTaken guard was not found: %s"
            % (self.found["sample_guard"],))
        self.assertTrue(
            self.found["head_inside_guard"],
            "%s is not read inside the if(%s) block." % (RING_HEAD, SAMPLE_FLAG))
        self.assertTrue(
            self.found["tail_inside_guard"],
            "%s is not read inside the if(%s) block." % (RING_TAIL, SAMPLE_FLAG))
        self.assertFalse(
            self.found["head_outside_guard"],
            "%s is also read OUTSIDE the fSampleTaken guard, so a refused read "
            "reaches the writer as a byte offset." % RING_HEAD)
        self.assertFalse(
            self.found["tail_outside_guard"],
            "%s is also read OUTSIDE the fSampleTaken guard, so a refused read "
            "reaches the writer as a byte offset." % RING_TAIL)

    def test_control_an_unguarded_sampled_read_reddens(self):
        """Hoisting the sampled read above its guard must be caught."""
        anchor = "    if (fSampleTaken) {"
        self.assertEqual(
            1, self.source.count(anchor),
            "mutation anchor %r occurs %d times; the mutation below assumes "
            "exactly one." % (anchor, self.source.count(anchor)))
        mutant = self.source.replace(
            anchor,
            "    headBytes = fRingHead;\n" + anchor,
            1)
        self.assertNotEqual(self.source, mutant, "the mutation applied to nothing")
        found = audit(mutant)
        self.assertTrue(
            found["head_outside_guard"],
            "a read of %s was hoisted above the fSampleTaken guard and this "
            "gate did not report it." % RING_HEAD)

    # ---------------------------------------------------------- property 5

    def test_the_file_still_has_no_write_path(self):
        """Property 5. The banner's claim, made structural.

        The rehearsal stores into host memory. If it ever grew an OSWrite* or a
        volatile store, IntelGtProbe.hpp's banner and intel-handoff36.md SS1
        would both become false, and IGGtSubmitted false would be a lie.
        """
        self.assertEqual(
            [], self.found["oswrites"],
            "OSWrite* call(s) at line(s) %s. This file is documented as having "
            "no write path at all." % self.found["oswrites"])
        self.assertEqual(
            [], self.found["volatile_stores"],
            "volatile store(s) at line(s) %s -- a store through a volatile "
            "pointer in this file is an MMIO write, and the banner says there "
            "are none." % self.found["volatile_stores"])

    def test_control_a_planted_mmio_store_reddens(self):
        """The banner's claim must be falsifiable, or it is prose."""
        anchor = "void IntelGtProbe::publishRingWrite(void)"
        self.assertEqual(
            1, self.source.count(anchor),
            "mutation anchor %r occurs %d times; the mutation below assumes "
            "exactly one." % (anchor, self.source.count(anchor)))
        mutant = self.source.replace(
            anchor,
            "void IntelGtProbe::sneakStore(UInt32 o, UInt32 v)\n"
            "{\n"
            "    *(volatile UInt32 *) (fBar0Base + o) = v;\n"
            "}\n\n" + anchor,
            1)
        self.assertNotEqual(self.source, mutant, "the mutation applied to nothing")
        found = audit(mutant)
        self.assertTrue(
            found["volatile_stores"],
            "an MMIO store was planted in the file and this gate reported none.")


if __name__ == "__main__":
    unittest.main(verbosity=2)
