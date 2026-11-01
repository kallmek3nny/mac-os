"""Pin the SHAPE of the Intel probe's publishers: every publisher that reads
MMIO must judge those reads as a group BEFORE it publishes anything.

Why this module exists
----------------------

`handoff22.md` taught `regRead32()` to refuse a read past the end of the BAR0
mapping and return `IG_MMIO_INVALID`.  `handoff24.md` found that nothing
CONSUMED that refusal -- the sentinel occurred at exactly one site in the whole
of `phase0-kext/IntelDisplayProbe.cpp`, inside `regRead32()` where it is
produced -- so both publishers handed a refused read straight into the decoders
and published the decode.  `setProperty()` with a decode of a refused read is
byte-identical to `setProperty()` with a decode of a register that genuinely
read that value, so an operator could not tell "this block is unprogrammed"
(a fact about the machine, unfixable) from "this block was never read" (a fact
about the driver, fixable).

handoff24 fixed it by sampling each publisher's reads as a GROUP through
`igSampleVerdict()` and publishing the verdict first.  handoff24 §5 item 4 is
the hole that fix left, and it is what this module closes:

    Deleting either `sampled[]` block and its `setProperty()` returns the file
    to its defective state and all four gates stay green.

None of the four gates can see it, by construction.  The register gate checks
CONSTANTS and `IG_MMIO_INVALID` is correct.  The C++ suites cannot link a
translation unit that reaches `IOService`.  `-Werror` has nothing to say about
a deleted call whose result nothing used.  Nothing in the Python gate modelled
the SHAPE of this file.  So the technique is handoff18 §1d's: parse the source,
assert the properties, DERIVE the identifiers rather than spelling them, and
REFUSE rather than pass when the subject cannot be found.

Design rules carried from the two prior shape gates
---------------------------------------------------

* **One `audit()`.**  The live tree and every mutant enter through it and
  nowhere else.  `tests/test_probe_dead_read_publication.py` states the reason:
  a gate whose mutants are checked by different code than its real input is two
  gates, and only one of them has ever been seen to fail.

* **Scan CODE, not prose.**  Comments and string literals are stripped before
  any count.  This is mandatory here rather than tidy: `publishPlane()`'s own
  comment discusses the sampled group by name, `publishTranscoder()`'s comment
  quotes the ten-word join, and `regRead32()`'s comment names
  `IG_MMIO_INVALID`.  A gate reading prose would find the identifiers it exists
  to require and pass on a file whose calls had all been deleted -- handoff18
  §1d's fix comment quoting the defective call is the same trap.

* **Derive the SUBJECT SET, not just the identifier.**  A publisher is a method
  that BOTH reads MMIO and publishes.  `publishIdentity()` publishes four
  properties and reads ZERO registers -- it decodes `fDeviceId`, which came
  from PCI config space rather than from a BAR -- so a blanket "every
  `setProperty()` needs a preceding `igSampleVerdict()`" would redden correct
  shipping code.  That is the failure mode of handoff23 §1b and handoff25 §1c,
  where the failing test was the POSITIVE control.  Deriving the set also means
  a third publisher added later is picked up without editing this file.

* **Balanced-bracket walking, never a negated character class.**  handoff25
  §1c: a class excluding only the terminator is not scoped to the statement --
  it runs past the `if`'s closing paren, the newline and the whole guarded body
  to land on the next statement's semicolon.  Function bodies and call argument
  lists here are found by counting brackets.

* **Refuse rather than pass when the subject is missing.**  `function_body()`
  returns `None`, never `""`, and a body that cannot be found is reported as a
  failure.  That is the failure mode that makes a shape gate vacuous, and it
  has its own negative control below.

What is pinned
--------------

1. Every derived publisher calls `igSampleVerdict()` exactly once.
2. That call precedes every `setProperty()` in the same body -- so the verdict
   sorts FIRST in the IORegistry, above every value it qualifies.
3. The sampled array names EVERY variable the publisher assigned from a
   `regRead32(` in that body.  Ten for `publishTranscoder`, four for
   `publishPlane`, both derived from the assignments rather than spelled.
4. The verdict itself is published, on an unconditional path -- a flag that
   appears only when something went wrong is indistinguishable from an older
   kext that never published it at all.
5. `igSampleVerdict()` is reached only through the array: the count argument is
   derived with `sizeof`, not a literal, so adding a read without extending the
   array cannot leave the count silently correct.
6. At least one publisher exists.  A rule that quantifies over an empty set is
   an `ok` line, not a test.
"""

import os
import re
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROBE = os.path.join(ROOT, "phase0-kext", "IntelDisplayProbe.cpp")

CLASS_NAME = "IntelDisplayProbe"
READ_CALL = "regRead32"
PUBLISH_CALL = "setProperty"
SAMPLE_CALL = "igSampleVerdict"


# ---------------------------------------------------------------------------
# source handling
# ---------------------------------------------------------------------------


def strip_comments_and_strings(src):
    """Blank out comments and string/char literals, preserving byte offsets.

    Offsets are preserved (each removed character becomes a space, newlines
    kept) so every index computed against the result is also an index into the
    original file -- which is what makes error messages point at real lines.

    Prose is not merely noise here.  The comments in this file NAME the very
    identifiers this gate requires, so counting them would let a file with
    every call deleted pass.
    """
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""

        if c == "/" and nxt == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue

        if c == "/" and nxt == "*":
            out.append("  ")
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue

        if c == '"' or c == "'":
            quote = c
            out.append(" ")
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append(" ")
                i += 1
            continue

        out.append(c)
        i += 1

    return "".join(out)


def line_of(src, index):
    """1-based line number of a byte offset, for failure messages."""
    return src.count("\n", 0, index) + 1


def matching_bracket(src, open_index, opener, closer):
    """Index of the bracket closing the one at `open_index`, or None.

    A balanced walk rather than a negated character class -- handoff25 §1c.
    Callers pass comment- and string-stripped source, so a bracket inside a
    literal cannot skew the depth.
    """
    if open_index is None or open_index >= len(src) or src[open_index] != opener:
        return None
    depth = 0
    for i in range(open_index, len(src)):
        if src[i] == opener:
            depth += 1
        elif src[i] == closer:
            depth -= 1
            if depth == 0:
                return i
    return None


def method_names(src):
    """Every `Type ClassName::name(` definition in the file, in source order."""
    pattern = re.compile(r"\b" + re.escape(CLASS_NAME) + r"::(\w+)\s*\(")
    names = []
    for m in pattern.finditer(src):
        if m.group(1) not in names:
            names.append(m.group(1))
    return names


def function_body(src, name):
    """Body of `ClassName::name`, brace-matched, or None.

    Returns None -- never the empty string -- so a caller cannot mistake
    "could not find the subject" for "the subject contains nothing".  Every
    caller below treats None as a failure rather than a vacuous pass.
    """
    pattern = re.compile(r"\b" + re.escape(CLASS_NAME) + r"::" + re.escape(name) + r"\s*\(")
    m = pattern.search(src)
    if not m:
        return None
    paren = src.find("(", m.end() - 1)
    close_paren = matching_bracket(src, paren, "(", ")")
    if close_paren is None:
        return None
    brace = src.find("{", close_paren)
    if brace < 0:
        return None
    close_brace = matching_bracket(src, brace, "{", "}")
    if close_brace is None:
        return None
    return (brace + 1, close_brace)


def call_sites(body_text, callee):
    """Offsets (relative to body_text) of every `callee(` call.

    The trailing `\\s*\\(` is required so `igSampleVerdictName` cannot be
    counted as `igSampleVerdict`, and the leading word boundary so a member
    named `xregRead32` could not either.
    """
    pattern = re.compile(r"\b" + re.escape(callee) + r"\s*\(")
    return [m.start() for m in pattern.finditer(body_text)]


def read_assignments(body_text):
    """Variables in this body assigned directly from a `regRead32(` call.

    Matches `UInt32 name = regRead32(...)` and a bare `name = regRead32(...)`.
    Deriving the list is the point: a hardcoded list of ten names would pass on
    a publisher that grew an eleventh read and never sampled it.
    """
    pattern = re.compile(
        r"(?:\b\w[\w:*&<>, ]*?\s+)?(\w+)\s*=\s*" + re.escape(READ_CALL) + r"\s*\("
    )
    names = []
    for m in pattern.finditer(body_text):
        if m.group(1) not in names:
            names.append(m.group(1))
    return names


def call_arguments(body_text, call_offset, callee):
    """Top-level comma-separated argument texts of the call at `call_offset`.

    Split at depth 1 only, so `igSampleVerdict(sampled, sizeof(a)/sizeof(a[0]))`
    yields two arguments rather than four.
    """
    paren = body_text.find("(", call_offset + len(callee) - 1)
    close = matching_bracket(body_text, paren, "(", ")")
    if close is None:
        return None
    inner = body_text[paren + 1 : close]
    args = []
    depth = 0
    current = []
    for ch in inner:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
            continue
        current.append(ch)
    tail = "".join(current).strip()
    if tail or args:
        args.append(tail)
    return args


def array_initializer(body_text, array_name):
    """Text inside the braces of `... array_name[] = { ... };`, or None."""
    pattern = re.compile(r"\b" + re.escape(array_name) + r"\s*\[\s*\]\s*=\s*\{")
    m = pattern.search(body_text)
    if not m:
        return None
    brace = body_text.rfind("{", m.start(), m.end())
    close = matching_bracket(body_text, brace, "{", "}")
    if close is None:
        return None
    return body_text[brace + 1 : close]


def identifiers(text):
    return set(re.findall(r"\b[A-Za-z_]\w*\b", text))


# ---------------------------------------------------------------------------
# the audit -- the live tree and every mutant come through here
# ---------------------------------------------------------------------------


def audit(source):
    """Return a list of failure strings; empty means the shape is intact.

    Every property is checked against comment- and string-stripped source.
    Failure strings name WHICH property broke and where, so the negative
    controls below can assert on the reason rather than on a bare boolean.
    """
    failures = []
    code = strip_comments_and_strings(source)

    names = method_names(code)
    if not names:
        return ["no %s:: methods found -- subject not located" % CLASS_NAME]

    publishers = []
    for name in names:
        span = function_body(code, name)
        if span is None:
            failures.append("body not found for %s::%s" % (CLASS_NAME, name))
            continue
        body = code[span[0] : span[1]]
        reads = call_sites(body, READ_CALL)
        publishes = call_sites(body, PUBLISH_CALL)
        if reads and publishes:
            publishers.append((name, span, body))

    if not publishers:
        failures.append(
            "no publisher found: no %s:: method both calls %s() and %s()"
            % (CLASS_NAME, READ_CALL, PUBLISH_CALL)
        )
        return failures

    for name, span, body in publishers:
        base = span[0]

        samples = call_sites(body, SAMPLE_CALL)
        if not samples:
            failures.append(
                "%s reads MMIO and publishes but never calls %s()" % (name, SAMPLE_CALL)
            )
            continue
        if len(samples) != 1:
            failures.append(
                "%s calls %s() %d times -- the group verdict must be one call"
                % (name, SAMPLE_CALL, len(samples))
            )
            continue

        sample_at = samples[0]
        publishes = call_sites(body, PUBLISH_CALL)
        first_publish = min(publishes)
        if sample_at > first_publish:
            failures.append(
                "%s publishes at line %d before sampling at line %d -- the "
                "verdict must come first"
                % (
                    name,
                    line_of(source, base + first_publish),
                    line_of(source, base + sample_at),
                )
            )

        args = call_arguments(body, sample_at, SAMPLE_CALL)
        if not args or len(args) != 2:
            failures.append(
                "%s: %s() call does not take two arguments" % (name, SAMPLE_CALL)
            )
            continue

        array_name = args[0].strip()
        init = array_initializer(body, array_name)
        if init is None:
            failures.append(
                "%s: sampled array %r has no brace initializer" % (name, array_name)
            )
            continue

        sampled_names = identifiers(init)
        read_names = read_assignments(body)
        if not read_names:
            failures.append(
                "%s calls %s() but no variable is assigned from it"
                % (name, READ_CALL)
            )
            continue

        missing = [v for v in read_names if v not in sampled_names]
        if missing:
            failures.append(
                "%s reads %s but does not sample it: %s"
                % (name, ", ".join(missing), array_name)
            )

        read_calls = call_sites(body, READ_CALL)
        if len(read_names) != len(read_calls):
            failures.append(
                "%s makes %d %s() calls but only %d land in named variables -- "
                "an unnamed read cannot be sampled"
                % (name, len(read_calls), READ_CALL, len(read_names))
            )

        count_arg = args[1]
        if "sizeof" not in count_arg:
            failures.append(
                "%s: %s() count argument %r is not derived with sizeof -- a "
                "literal count stays correct when a read is added"
                % (name, SAMPLE_CALL, count_arg.strip())
            )
        elif array_name not in count_arg:
            failures.append(
                "%s: %s() count argument does not size %r"
                % (name, SAMPLE_CALL, array_name)
            )

        sample_var = None
        assign = re.search(
            r"(?:\b\w[\w ]*?\s+)?(\w+)\s*=\s*" + re.escape(SAMPLE_CALL) + r"\s*\(", body
        )
        if assign:
            sample_var = assign.group(1)
        if sample_var is None:
            failures.append(
                "%s: %s() result is discarded -- a verdict nobody publishes is "
                "a verdict nobody reads" % (name, SAMPLE_CALL)
            )
            continue

        published = False
        for offset in publishes:
            pub_args = call_arguments(body, offset, PUBLISH_CALL)
            if not pub_args:
                continue
            if any(sample_var in identifiers(a) for a in pub_args[1:]):
                published = True
                break
        if not published:
            failures.append(
                "%s samples into %r but never publishes it" % (name, sample_var)
            )

    return failures


def live_source():
    with open(PROBE, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def mutate(source, anchor, replacement, expected_count=1):
    """Replace `anchor` with `replacement`, asserting its occurrence COUNT.

    A mutation whose anchor has drifted must raise rather than silently match
    nothing -- a mutation that matches nothing prints `ok`, and an `ok` from a
    no-op mutation is the loudest possible false green.
    """
    found = source.count(anchor)
    if found != expected_count:
        raise AssertionError(
            "mutation anchor occurs %d times, expected %d: %r"
            % (found, expected_count, anchor[:80])
        )
    return source.replace(anchor, replacement)


# ---------------------------------------------------------------------------
# positive control
# ---------------------------------------------------------------------------


def derived_publishers(source):
    """Every method audit() treats as a publisher, in source order.

    The same derivation audit() performs -- a body that calls both READ_CALL
    and PUBLISH_CALL -- exposed once so the positive control and the
    missing-body control assert on one definition rather than two copies.
    """
    code = strip_comments_and_strings(source)
    derived = []
    for name in method_names(code):
        span = function_body(code, name)
        if span is None:
            continue
        body = code[span[0] : span[1]]
        if call_sites(body, READ_CALL) and call_sites(body, PUBLISH_CALL):
            derived.append(name)
    return derived


class TestLiveTree(unittest.TestCase):
    def test_probe_source_exists(self):
        self.assertTrue(
            os.path.isfile(PROBE), "subject not found: %s" % PROBE
        )

    def test_live_tree_passes_the_audit(self):
        failures = audit(live_source())
        self.assertEqual(
            [], failures, "shipping code failed the shape audit:\n  " + "\n  ".join(failures)
        )

    def test_both_known_publishers_are_derived(self):
        """The derived set must actually contain the real publishers.

        Without this, a bug that derived an EMPTY set for the wrong reason
        would still let every quantified rule above pass vacuously.

        publishPlaneLiveObservation is pinned as well.  It reaches the rules by
        derivation, not by name -- which is how this gate caught it publishing
        ten unsampled reads (3050-handoff44) -- and pinning it here means a
        rename or a refactor that hides its reads cannot quietly drop it out
        of scope.
        """
        derived = derived_publishers(live_source())
        self.assertIn("publishTranscoder", derived)
        self.assertIn("publishPlane", derived)
        self.assertIn("publishPlaneLiveObservation", derived)

    def test_publish_identity_is_not_a_publisher(self):
        """publishIdentity publishes and reads NO registers, so it is out of scope.

        This is the assertion that keeps the rule honest.  A blanket "every
        setProperty needs a preceding igSampleVerdict" would redden correct
        shipping code here -- fDeviceId comes from PCI config space, not from a
        BAR, so there is no refusal to consume.
        """
        code = strip_comments_and_strings(live_source())
        span = function_body(code, "publishIdentity")
        self.assertIsNotNone(span, "publishIdentity body not found")
        body = code[span[0] : span[1]]
        self.assertEqual([], call_sites(body, READ_CALL))
        self.assertNotEqual([], call_sites(body, PUBLISH_CALL))

    def test_stripping_preserves_offsets_and_removes_prose(self):
        """Two properties the whole module rests on, measured not assumed.

        Offset preservation is what lets a span found in stripped code be
        applied to the raw bytes, which every mutation below relies on.

        The first version of this test asserted that `igSampleVerdict` appears
        in the subject's COMMENTS as well as its code, and went red on a
        pristine tree: the count is 4 either way, because the comments discuss
        the group in words without spelling the callee.  That is handoff25
        §1c-ii exactly -- an assertion about comment wording I remembered
        writing rather than wording on disk.  The durable property is not which
        identifiers the current comments happen to contain; it is that no
        comment or literal survives stripping at all, and that a comment naming
        the call cannot satisfy the gate.  The second half of that is a
        negative control below rather than a fact about today's file.
        """
        source = live_source()
        code = strip_comments_and_strings(source)
        self.assertEqual(len(source), len(code), "stripping changed byte offsets")
        self.assertEqual(
            source.count("\n"), code.count("\n"), "stripping changed the line count"
        )
        self.assertNotIn("//", code, "line-comment text survived stripping")
        self.assertNotIn('"', code, "string literal survived stripping")
        self.assertIn("//", source, "subject has no comments -- stripping unproven")
        self.assertIn('"', source, "subject has no literals -- stripping unproven")


# ---------------------------------------------------------------------------
# negative controls -- each asserts WHICH property broke
# ---------------------------------------------------------------------------


class TestNegativeControls(unittest.TestCase):
    def assertFailsWith(self, failures, needle):
        self.assertTrue(failures, "mutation was not caught at all")
        joined = "\n".join(failures)
        self.assertIn(needle, joined, "caught, but for the wrong reason:\n" + joined)

    def test_deleting_the_transcoder_sample_is_caught(self):
        """handoff24 §5 item 4, verbatim: the defect this module exists to stop."""
        source = live_source()
        code = strip_comments_and_strings(source)
        span = function_body(code, "publishTranscoder")
        body = source[span[0] : span[1]]
        m = re.search(r"\w+\s+sample\s*=\s*igSampleVerdict\s*\(", body)
        self.assertIsNotNone(m, "could not locate the transcoder sample call")
        paren = body.find("(", m.end() - 1)
        close = matching_bracket(body, paren, "(", ")")
        semi = body.find(";", close)
        mutated = source[: span[0]] + body[: m.start()] + body[semi + 1 :] + source[span[1] :]
        self.assertFailsWith(audit(mutated), "never calls %s()" % SAMPLE_CALL)

    def test_deleting_the_live_observer_sample_is_caught(self):
        """The 3050-handoff44 defect restored: ten reads published unsampled.

        publishPlaneLiveObservation reaches the audit by derivation, so this
        proves the gate still reddens on the exact shape it caught.  Its
        verdict local is readSample (sample already names the snapshot), so
        the transcoder control's `sample =` pattern cannot find it; this
        control has its own locator and asserts that it matched.
        """
        source = live_source()
        code = strip_comments_and_strings(source)
        span = function_body(code, "publishPlaneLiveObservation")
        self.assertIsNotNone(span, "publishPlaneLiveObservation body not found")

        code_body = code[span[0] : span[1]]
        raw_body = source[span[0] : span[1]]

        m = re.search(r"\w+\s+readSample\s*=\s*igSampleVerdict\s*\(", code_body)
        self.assertIsNotNone(m, "could not locate the live observer sample call")
        paren = m.end() - 1
        close = matching_bracket(code_body, paren, "(", ")")
        self.assertIsNotNone(close, "live observer sample call is unbalanced")
        semi = code_body.find(";", close)
        self.assertNotEqual(semi, -1, "live observer sample call is unterminated")

        mutated = (
            source[: span[0]]
            + raw_body[: m.start()]
            + raw_body[semi + 1 :]
            + source[span[1] :]
        )
        self.assertFailsWith(audit(mutated), "never calls %s()" % SAMPLE_CALL)

    def test_dropping_a_read_from_the_sampled_array_is_caught(self):
        """One word left out of the group is the per-field defect handoff24 rejected.

        Mutated STRUCTURALLY, inside the brace span of the sampled[] array,
        rather than by text.  The first version replaced the literal text
        `vTotal, vBlank, vSync, src, planeSize` and raised on its own occurrence
        count: that run of names appears TWICE in the file, once in the array
        and once in the igPipeUsable() join it feeds.  `mutate()` refusing on a
        count of 2 is the precondition working -- a mutation that matches more
        than it meant to is as worthless as one that matches nothing.
        """
        source = live_source()
        code = strip_comments_and_strings(source)
        span = function_body(code, "publishTranscoder")
        self.assertIsNotNone(span, "publishTranscoder body not found")

        code_body = code[span[0] : span[1]]
        raw_body = source[span[0] : span[1]]

        m = re.search(r"\bsampled\s*\[\s*\]\s*=\s*\{", code_body)
        self.assertIsNotNone(m, "sampled[] initializer not found")
        brace = code_body.rfind("{", m.start(), m.end())
        close = matching_bracket(code_body, brace, "{", "}")
        self.assertIsNotNone(close, "sampled[] initializer is unbalanced")

        init = raw_body[brace + 1 : close]
        self.assertIn("planeSize", init, "planeSize is not in the transcoder group")
        trimmed = init.replace(", planeSize", "", 1)
        self.assertNotEqual(init, trimmed, "trim matched nothing inside the array")

        mutated_body = raw_body[: brace + 1] + trimmed + raw_body[close:]
        mutated = source[: span[0]] + mutated_body + source[span[1] :]
        self.assertFailsWith(audit(mutated), "does not sample it")

    def test_a_comment_naming_the_call_does_not_satisfy_the_gate(self):
        """handoff18 §1d's trap, planted deliberately: prose is not code.

        There the fix's own comment quoted the defective call, so a structural
        gate that counted prose reddened a pristine tree.  The inverse is the
        danger here: comment the sampler OUT and leave its text on the line,
        and a prose-reading gate sees the identifier it requires while the
        driver publishes decodes of reads nobody judged.
        """
        source = live_source()
        code = strip_comments_and_strings(source)
        span = function_body(code, "publishTranscoder")
        self.assertIsNotNone(span, "publishTranscoder body not found")

        code_body = code[span[0] : span[1]]
        raw_body = source[span[0] : span[1]]

        m = re.search(r"\w+\s+sample\s*=\s*" + re.escape(SAMPLE_CALL) + r"\s*\(", code_body)
        self.assertIsNotNone(m, "could not locate the transcoder sample call")
        paren = code_body.find("(", m.end() - 1)
        close = matching_bracket(code_body, paren, "(", ")")
        self.assertIsNotNone(close, "sample call is unbalanced")
        semi = code_body.find(";", close)
        self.assertGreater(semi, 0, "sample statement has no terminator")

        commented = "// " + " ".join(raw_body[m.start() : semi + 1].split()) + "\n    "
        mutated_body = raw_body[: m.start()] + commented + raw_body[semi + 1 :]
        mutated = source[: span[0]] + mutated_body + source[span[1] :]

        self.assertIn(SAMPLE_CALL, mutated, "mutation did not leave the name in prose")
        self.assertFailsWith(audit(mutated), "never calls %s()" % SAMPLE_CALL)

    def test_dropping_surf_from_the_plane_group_is_caught(self):
        """SURF is deliberately inside the plane group -- handoff24 §1b.

        A refused SURF decodes to an offset of 0xFFFFFFC0-ish and a range
        verdict computed over an address nobody read.
        """
        source = live_source()
        mutated = mutate(source, "ctl, stride, size, surf", "ctl, stride, size")
        self.assertFailsWith(audit(mutated), "surf")

    def test_publishing_before_sampling_is_caught(self):
        """The verdict must sort ABOVE the values it qualifies."""
        source = live_source()
        code = strip_comments_and_strings(source)
        span = function_body(code, "publishPlane")
        body = source[span[0] : span[1]]
        m = re.search(r"\w+\s+sample\s*=\s*igSampleVerdict\s*\(", body)
        self.assertIsNotNone(m)
        paren = body.find("(", m.end() - 1)
        close = matching_bracket(body, paren, "(", ")")
        semi = body.find(";", close)
        stmt = body[m.start() : semi + 1]
        without = body[: m.start()] + body[semi + 1 :]
        moved = without.rstrip()
        if moved.endswith("}"):
            moved = moved[:-1]
        moved = without + "\n    " + stmt + "\n"
        mutated = source[: span[0]] + moved + source[span[1] :]
        failures = audit(mutated)
        self.assertTrue(failures, "reordering was not caught")
        self.assertTrue(
            any("verdict must come first" in f for f in failures),
            "caught, but for the wrong reason:\n" + "\n".join(failures),
        )

    def test_a_literal_count_is_caught(self):
        """A literal count stays correct when an eleventh read is added."""
        source = live_source()
        mutated = re.sub(
            r"\(UInt32\)\s*\(sizeof\(sampled\)\s*/\s*sizeof\(sampled\[0\]\)\)",
            "10u",
            source,
            count=1,
        )
        self.assertNotEqual(source, mutated, "count mutation matched nothing")
        self.assertFailsWith(audit(mutated), "not derived with sizeof")

    def test_discarding_the_verdict_is_caught(self):
        """A verdict nobody publishes is a verdict nobody reads."""
        source = live_source()
        code = strip_comments_and_strings(source)
        span = function_body(code, "publishPlane")
        body = source[span[0] : span[1]]
        m = re.search(r"(\w+\s+sample\s*=\s*)igSampleVerdict\s*\(", body)
        self.assertIsNotNone(m)
        mutated_body = body[: m.start(1)] + body[m.end(1) :]
        mutated = source[: span[0]] + mutated_body + source[span[1] :]
        failures = audit(mutated)
        self.assertTrue(failures, "discarded verdict was not caught")

    def test_not_publishing_the_verdict_is_caught(self):
        """handoff18 §1d: an absent key is indistinguishable from an older kext."""
        source = live_source()
        mutated = re.sub(
            r"setProperty\(([^,\n]*Sample[^,\n]*),\s*\(UInt64\)\s*sample[^;]*;",
            "",
            source,
            count=1,
        )
        if mutated == source:
            mutated = re.sub(
                r"[^\n]*setProperty[^\n]*\bsample\b[^\n]*\n", "", source, count=1
            )
        self.assertNotEqual(source, mutated, "publish mutation matched nothing")
        self.assertFailsWith(audit(mutated), "never publishes it")

    def test_a_second_sample_call_is_caught(self):
        """Two verdicts in one body is a per-field flag wearing a group's name."""
        source = live_source()
        mutated = mutate(
            source,
            "    UInt32 sample = igSampleVerdict(sampled,",
            "    UInt32 extra = igSampleVerdict(sampled, 1u);\n"
            "    (void) extra;\n"
            "    UInt32 sample = igSampleVerdict(sampled,",
            expected_count=2,
        )
        self.assertFailsWith(audit(mutated), "the group verdict must be one call")

    def test_an_unnamed_read_is_caught(self):
        """A read whose value never lands in a variable cannot be sampled."""
        source = live_source()
        code = strip_comments_and_strings(source)
        span = function_body(code, "publishPlane")
        body = source[span[0] : span[1]]
        anchor = "UInt32 surf   = regRead32("
        self.assertIn(anchor, body, "plane read anchor drifted")
        mutated_body = body.replace(
            anchor, "(void) regRead32(igPlaneReg(pipe, 0u, IG_PLANE_CTL));\n    UInt32 surf   = regRead32(", 1
        )
        mutated = source[: span[0]] + mutated_body + source[span[1] :]
        self.assertFailsWith(audit(mutated), "land in named variables")

    def test_a_missing_body_refuses_rather_than_passes(self):
        """The failure mode that makes a shape gate vacuous.

        The anchor carries the opening parenthesis because the bare
        `IntelDisplayProbe::publishPlane` is also a prefix of
        `IntelDisplayProbe::publishPlaneLiveObservation`.  mutate() refused the
        bare form on a count of 2 (3050-handoff44), which is its precondition
        working; with the parenthesis it names exactly one definition.

        The old `failures or True` assertion could never fail.  What a rename
        must not do is drop the method out of scope: publishers are derived
        from what their bodies call, not from their names, so the renamed
        method must still be derived and the old name must be gone.
        """
        source = live_source()
        mutated = mutate(
            source,
            "IntelDisplayProbe::publishPlane(",
            "IntelDisplayProbe::publishPlaneRenamed(",
        )
        derived = derived_publishers(mutated)
        self.assertIn(
            "publishPlaneRenamed",
            derived,
            "renaming must not silently drop the subject",
        )
        self.assertNotIn(
            "publishPlane",
            derived,
            "the rename matched nothing: the old name is still derived",
        )
        code = strip_comments_and_strings(mutated)
        self.assertIsNone(
            function_body(code, "publishPlane"),
            "function_body must return None, never an empty body",
        )

    def test_an_empty_subject_refuses(self):
        """A file with no methods must fail, not pass over nothing."""
        failures = audit("// nothing here at all\n")
        self.assertFailsWith(failures, "subject not located")

    def test_a_subject_with_no_publisher_refuses(self):
        """A rule quantified over an empty set is an ok line, not a test."""
        source = """
void IntelDisplayProbe::doNothing(void)
{
    return;
}
"""
        self.assertFailsWith(audit(source), "no publisher found")


if __name__ == "__main__":
    unittest.main()
