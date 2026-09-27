#!/usr/bin/env python3
r"""test_capture_vs_header.py -- every IG_OBSERVED_* value is a HAND TRANSCRIPTION.

Why this exists
---------------
``phase0-kext/IntelDisplay.hpp`` carries two blocks of ``IG_OBSERVED_*``
constants. Every one of them was typed by a human out of
``igpu-results/tgl-final-capture-2026-08-18-201412.txt``, and until this module
landed **nothing in the tree compared the two**. A transposed nibble in a
constant is invisible: it compiles, it satisfies every pairwise-distinct check
in ``tests/test_intel_display.cpp``, and it reads as a witnessed value beside
four real ones. That is the exact failure this project's ``*_OBSERVED_*``
naming convention exists to prevent, and the convention was being enforced by
care rather than by a gate.

The capture is unrepeatable -- there is no second boot of that silicon -- so
this module NEVER writes to it. It reads, and it reads only the machine's own
output: everything above the READING NOTES divider. Text below that line is
host-side analysis added afterwards, so a value found there is not a
measurement and must not be able to satisfy this gate.

What it pins
------------
1. Each anchored constant equals the dword the capture recorded at its offset.
2. The capture rows this gate depends on all EXIST -- a renamed section or a
   reformatted dump produces a REFUSAL, never a silent pass over an empty set.
3. The header block still exists and still parses.
4. The two all-ones reads are present in the capture and ABSENT from the
   header, so the exclusion recorded in intel-handoff30.md section 3 stays true
   rather than staying merely written down.

Why refusal rather than skip
----------------------------
A gate that reports nothing when its anchor moves is indistinguishable from a
green one -- ``3050-handoff30.md`` section 2b. Every parse here fails loudly on
an empty result set.
"""

import os
import re
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CAPTURE = os.path.join(
    REPO, "igpu-results", "tgl-final-capture-2026-08-18-201412.txt"
)
HEADER = os.path.join(REPO, "phase0-kext", "IntelDisplay.hpp")

# The machine's output ends here. Everything below is host-side prose.
DIVIDER = "READING NOTES added on the host after the run"

# Section 6 rows look like:  "  CDCLK_CTL   @0x046000  0x0038017E"
ROW_RE = re.compile(
    r"^\s{2,}(\S.*?)\s+@0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]{8})\s*$"
)

# NOTE: the trailing part is deliberately NOT anchored with $. Six of the timing
# defines carry an inline "// 1919 / 2079" comment, and an end-anchored pattern
# matched NONE of them -- so this gate's first draft scored a subject set six
# smaller than it appeared to, with test_no_header_constant_holds_a_refusal_value
# silently sweeping a shrunken set. An anchor that quietly narrows its own
# subject is intel-handoff28.md section 3's defect, one file over.
DEFINE_RE = re.compile(
    r"^#define\s+(IG_OBSERVED_[A-Z0-9_]+)\s+0x([0-9A-Fa-f]{8})u",
    re.MULTILINE,
)

# capture row name  ->  (capture offset, header macro)
#
# Only the five ANCHORED constants are in this first table. The panel-power and
# backlight words live in the raw range dumps rather than in section 6's named
# rows and are covered separately.
ANCHORED = {
    "CDCLK_CTL": (0x046000, "IG_OBSERVED_CDCLK_CTL_2026_08_18"),
    "SKL_DFSM": (0x051000, "IG_OBSERVED_SKL_DFSM_2026_08_18"),
    "SOUTH_CHICKEN1": (0x0C2000, "IG_OBSERVED_SOUTH_CHICKEN1_2026_08_18"),
    "SFUSE_STRAP": (0x0C2014, "IG_OBSERVED_SFUSE_STRAP_2026_08_18"),
    "ICL_DPCLKA_CFGCR0": (0x164280, "IG_OBSERVED_DPCLKA_CFGCR0_2026_08_18"),
    # The Gen12 PLL pair. Named rows in section 6 like the five above, so they
    # need no exemption -- they were simply missed by the first table.
    "_TGL_DPLL0_CFGCR0": (0x164284, "IG_OBSERVED_DPLL0_CFGCR0_2026_08_18"),
    "_TGL_DPLL0_CFGCR1": (0x164288, "IG_OBSERVED_DPLL0_CFGCR1_2026_08_18"),
}

# Read 0xFFFFFFFF -- the block is not decoding. A refusal is not a reading, so
# neither of these may appear in the header.
REFUSED_ROWS = {
    "PCH_DREF_CONTROL": 0x0C6200,
    "_TRANS_DP_CTL_A": 0x0E0300,
}

# The other seven harvested words have no i915 anchor in this checkout --
# intel_pps_regs.h and intel_dp_aux_regs.h were never vendored -- so they are
# named by OFFSET and they appear only in SECTION 1's raw dword dump, never in
# section 6's named rows.
UNANCHORED = {
    0x0C7200: "IG_OBSERVED_PANEL_PWR_C7200_2026_08_18",
    0x0C7204: "IG_OBSERVED_PANEL_PWR_C7204_2026_08_18",
    0x0C7208: "IG_OBSERVED_PANEL_PWR_C7208_2026_08_18",
    0x0C720C: "IG_OBSERVED_PANEL_PWR_C720C_2026_08_18",
    0x0C8250: "IG_OBSERVED_BACKLIGHT_C8250_2026_08_18",
    0x0C8254: "IG_OBSERVED_BACKLIGHT_C8254_2026_08_18",
    0x0C8258: "IG_OBSERVED_BACKLIGHT_C8258_2026_08_18",
}

# The timing/plane block from the FIRST observed section. Section 2 and section
# 4 spell these as a base plus a delta ("trans A @0x60000" then "+0x000"), and
# section 1 dumps the same words at their absolute offsets -- so they resolve
# through the same raw parse as UNANCHORED above rather than needing a second
# two-level parser. The absolute offsets below are the SUM the capture itself
# states, never arithmetic this gate invents.
#
# 9a item 1: these are the constants an actual mode-set will depend on, and
# until this table landed nothing compared one of them to anything.
TIMING = {
    # transcoder A timing block, section 2, base 0x60000
    0x060000: "IG_OBSERVED_TRANS_A_HTOTAL_2026_08_18",
    0x060004: "IG_OBSERVED_TRANS_A_HBLANK_2026_08_18",
    0x060008: "IG_OBSERVED_TRANS_A_HSYNC_2026_08_18",
    0x06000C: "IG_OBSERVED_TRANS_A_VTOTAL_2026_08_18",
    0x060010: "IG_OBSERVED_TRANS_A_VBLANK_2026_08_18",
    0x060014: "IG_OBSERVED_TRANS_A_VSYNC_2026_08_18",
    0x06001C: "IG_OBSERVED_TRANS_A_PIPESRC_2026_08_18",
    0x060400: "IG_OBSERVED_TRANS_A_DDI_FUNC_CTL_2026_08_18",
    0x060410: "IG_OBSERVED_TRANS_A_MSA_MISC_2026_08_18",
    # pipe A configuration, section 3. MISC2 is at +0x02C and MISC at +0x030 --
    # the capture's own spelling, and the reverse of the order the names suggest.
    0x070008: "IG_OBSERVED_PIPE_A_TRANSCONF_2026_08_18",
    0x07002C: "IG_OBSERVED_PIPE_A_MISC2_2026_08_18",
    0x070030: "IG_OBSERVED_PIPE_A_MISC_2026_08_18",
}

# The five plane words are keyed by DELTA from the plane base rather than by
# absolute offset, because section 1's raw dump does not reach two of them --
# 0x07018C and 0x0701A4 are absent from it and appear ONLY in section 4's own
# base-plus-delta block. Measured, not assumed: a table spelling them as
# absolute offsets reddened the presence arm rather than passing over them.
PLANE_BASE_EXPECTED = 0x070180
PLANE = {
    0x00: "IG_OBSERVED_PLANE_1A_CTL_2026_08_18",
    0x08: "IG_OBSERVED_PLANE_1A_STRIDE_2026_08_18",
    0x0C: "IG_OBSERVED_PLANE_1A_POS_2026_08_18",
    0x10: "IG_OBSERVED_PLANE_1A_SIZE_2026_08_18",
    0x24: "IG_OBSERVED_PLANE_1A_OFFSET_2026_08_18",
}

# TRANS_DDI_FUNC_CTL in BOTH DSI pages read the same non-trivial 0x00030000,
# which is the whole of the stride evidence (§4 of intel-handoff28.md). One macro
# is compared at TWO offsets on purpose: the header's claim is that both pages
# hold it, so comparing one page would ratify half the sentence.
DSI_CTL_OFFSETS = (0x06B400, 0x06BC00)
DSI_CTL_MACRO = "IG_OBSERVED_TRANS_DSI_DDI_FUNC_CTL_2026_08_18"

# 9a item 2: the subject set must be DERIVED, not spelled. Every IG_OBSERVED_*
# macro in the header is either compared against a dumped dword by one of the
# tables above, or it is listed HERE with the reason it cannot be. A macro in
# neither reddens the set-equality arm on the commit that adds it.
#
# These four are not transcriptions of a register READING, so there is no dword
# to compare them to:
EXEMPT = {
    "IG_OBSERVED_TRANS_DSI0_BASE_2026_08_18":
        "an ADDRESS this project derived from the capture's page layout, not a "
        "dword the machine printed. Its evidence is that registers were dumped "
        "inside the page, which test_both_dsi_pages_were_dumped asserts.",
    "IG_OBSERVED_TRANS_DSI1_BASE_2026_08_18":
        "an ADDRESS, same as DSI0 above.",
    "IG_OBSERVED_TRANS_DSI_STRIDE_2026_08_18":
        "a SPACING between two addresses, checked as arithmetic over the two "
        "base macros by test_the_dsi_stride_is_the_gap_between_the_bases "
        "rather than against any single reading.",
    "IG_OBSERVED_UNPROGRAMMED_2026_08_18":
        "a value PATTERN measured across transcoders B/C/D and pipes B/C/D on "
        "two boots, not one register's reading. Pinning it to a single offset "
        "would misrepresent what was measured.",
}

SECTION4_START = "==== SECTION 4"
SECTION4_END = "==== SECTION 5"

PLANE_BASE_RE = re.compile(r"plane\s+1\s+@0x([0-9A-Fa-f]{5,6})")
DELTA_RE = re.compile(r"\+0x([0-9A-Fa-f]{2,3})\s+0x([0-9A-Fa-f]{8})")

# Raw dump pairs:  "      0x0C7200 0x80000008  0x0C7204 0x00000067"
RAW_PAIR_RE = re.compile(r"0x([0-9A-Fa-f]{5,8})\s+0x([0-9A-Fa-f]{8})")

# The raw parse is bounded to section 1 deliberately. Section 6 spells its
# offsets as "@0x...", so a whole-file pair scan would let a section 6 row
# satisfy a section 1 comparison -- two different claims answered by one hit.
SECTION1_START = "==== SECTION 1"
SECTION1_END = "==== SECTION 2"


def machine_output():
    """The capture text ABOVE the divider, i.e. the silicon's own words."""
    with open(CAPTURE, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    cut = text.find(DIVIDER)
    if cut < 0:
        raise AssertionError(
            "REFUSAL: the READING NOTES divider is missing from %s, so this "
            "gate cannot tell machine output from host analysis." % CAPTURE
        )
    return text[:cut]


def named_rows():
    """Map every ``NAME @0xOFF 0xVALUE`` row above the divider to (off, value).

    A name appearing twice with DIFFERENT values is a live status bit sampled
    twice, not a constant, so the duplicate is kept rather than collapsed.
    """
    rows = {}
    for line in machine_output().splitlines():
        match = ROW_RE.match(line)
        if not match:
            continue
        name, offset, value = match.groups()
        rows.setdefault(name.strip(), []).append(
            (int(offset, 16), int(value, 16))
        )
    if not rows:
        raise AssertionError(
            "REFUSAL: parsed ZERO named rows out of %s. The dump format "
            "changed and this gate is measuring nothing." % CAPTURE
        )
    return rows


def raw_dwords():
    """Every ``0xOFFSET 0xVALUE`` pair inside SECTION 1, above the divider.

    Returns offset -> LIST of readings rather than a single value, because the
    capture script read a subset of registers twice in the one run and seven of
    them disagreed. An offset read twice with two answers is a sample, not a
    constant, and collapsing the list here would hide exactly that.
    """
    text = machine_output()
    start = text.find(SECTION1_START)
    end = text.find(SECTION1_END, start + 1) if start >= 0 else -1
    if start < 0 or end < 0:
        raise AssertionError(
            "REFUSAL: could not bound SECTION 1 in %s, so every raw-dump "
            "comparison below would measure nothing." % CAPTURE
        )
    dwords = {}
    for offset, value in RAW_PAIR_RE.findall(text[start:end]):
        dwords.setdefault(int(offset, 16), []).append(int(value, 16))
    if not dwords:
        raise AssertionError(
            "REFUSAL: parsed ZERO dwords out of SECTION 1 of %s." % CAPTURE
        )
    return dwords


def plane_dwords():
    """Plane 1's base and its ``+0xDD 0xVALUE`` block from SECTION 4.

    Returns ``(base, {delta: [readings]})``. The base is READ rather than
    assumed, so a capture whose plane 1 moved is a loud failure instead of a
    comparison against the wrong register.
    """
    text = machine_output()
    start = text.find(SECTION4_START)
    end = text.find(SECTION4_END, start + 1) if start >= 0 else -1
    if start < 0 or end < 0:
        raise AssertionError(
            "REFUSAL: could not bound SECTION 4 in %s, so every plane "
            "comparison below would measure nothing." % CAPTURE
        )
    block = text[start:end]
    bases = PLANE_BASE_RE.findall(block)
    if not bases:
        raise AssertionError(
            "REFUSAL: SECTION 4 no longer states a base for plane 1, so this "
            "gate cannot resolve a delta to a register."
        )
    base = int(bases[0], 16)
    deltas = {}
    for delta, value in DELTA_RE.findall(block):
        deltas.setdefault(int(delta, 16), []).append(int(value, 16))
    if not deltas:
        raise AssertionError(
            "REFUSAL: parsed ZERO deltas out of SECTION 4 of %s." % CAPTURE
        )
    return base, deltas


def header_defines():
    with open(HEADER, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    found = {
        name: int(value, 16) for name, value in DEFINE_RE.findall(text)
    }
    if not found:
        raise AssertionError(
            "REFUSAL: parsed ZERO IG_OBSERVED_* defines out of %s." % HEADER
        )
    return found


class AnchorTests(unittest.TestCase):
    """The gate must have a subject before it can have a verdict."""

    def test_the_capture_file_exists(self):
        self.assertTrue(
            os.path.isfile(CAPTURE),
            "the 2026-08-18 capture is one of two irreplaceable artifacts "
            "and it is missing",
        )

    def test_every_row_this_gate_depends_on_is_present(self):
        rows = named_rows()
        wanted = set(ANCHORED) | set(REFUSED_ROWS)
        missing = sorted(wanted - set(rows))
        self.assertEqual(
            [],
            missing,
            "REFUSAL: these capture rows vanished, so the comparisons below "
            "would pass by measuring nothing: %s" % missing,
        )

    def test_every_header_macro_this_gate_names_is_present(self):
        defines = header_defines()
        missing = sorted(
            macro for _, macro in ANCHORED.values() if macro not in defines
        )
        self.assertEqual([], missing, "header macros missing: %s" % missing)


class TranscriptionTests(unittest.TestCase):
    """Each anchored constant against the dword the machine actually printed."""

    def test_each_anchored_value_matches_the_capture(self):
        rows = named_rows()
        defines = header_defines()
        for name, (offset, macro) in sorted(ANCHORED.items()):
            with self.subTest(register=name):
                readings = rows[name]
                self.assertEqual(
                    1,
                    len(readings),
                    "%s was read %d times; a register read more than once is "
                    "a SAMPLE unless every reading agrees"
                    % (name, len(readings)),
                )
                got_offset, got_value = readings[0]
                self.assertEqual(
                    offset,
                    got_offset,
                    "%s sits at 0x%06X in the capture, not 0x%06X -- this "
                    "gate is comparing the wrong register"
                    % (name, got_offset, offset),
                )
                self.assertEqual(
                    got_value,
                    defines[macro],
                    "%s: capture says 0x%08X, %s says 0x%08X"
                    % (name, got_value, macro, defines[macro]),
                )


class UnanchoredTranscriptionTests(unittest.TestCase):
    """The seven offset-named words against SECTION 1's raw dump.

    These are the constants with no citable i915 line, so the capture is the
    ONLY evidence they have. A gate over them is worth more than a gate over the
    anchored five, not less.
    """

    def test_every_offset_this_gate_depends_on_was_dumped(self):
        dumped = raw_dwords()
        missing = sorted(
            "0x%06X" % offset for offset in UNANCHORED if offset not in dumped
        )
        self.assertEqual(
            [],
            missing,
            "REFUSAL: these offsets are no longer in SECTION 1, so the "
            "comparisons below would pass over an empty set: %s" % missing,
        )

    def test_every_unanchored_macro_is_present(self):
        defines = header_defines()
        missing = sorted(m for m in UNANCHORED.values() if m not in defines)
        self.assertEqual([], missing, "header macros missing: %s" % missing)

    def test_each_unanchored_value_matches_the_capture(self):
        dumped = raw_dwords()
        defines = header_defines()
        for offset, macro in sorted(UNANCHORED.items()):
            with self.subTest(offset="0x%06X" % offset):
                readings = set(dumped[offset])
                self.assertEqual(
                    1,
                    len(readings),
                    "0x%06X was read %d times and the readings DISAGREE (%s) "
                    "-- it is a live status bit, not configuration, and must "
                    "not be pinned as %s"
                    % (
                        offset,
                        len(dumped[offset]),
                        sorted("0x%08X" % v for v in readings),
                        macro,
                    ),
                )
                got = readings.pop()
                self.assertEqual(
                    got,
                    defines[macro],
                    "0x%06X: capture says 0x%08X, %s says 0x%08X"
                    % (offset, got, macro, defines[macro]),
                )

    def test_every_timing_offset_was_dumped(self):
        dumped = raw_dwords()
        missing = sorted(
            "0x%06X" % offset for offset in TIMING if offset not in dumped
        )
        self.assertEqual(
            [],
            missing,
            "REFUSAL: these timing/plane offsets are no longer in SECTION 1, "
            "so the comparisons below would pass over an empty set: %s"
            % missing,
        )

    def test_every_timing_macro_is_present(self):
        defines = header_defines()
        missing = sorted(m for m in TIMING.values() if m not in defines)
        self.assertEqual([], missing, "header macros missing: %s" % missing)

    def test_each_timing_value_matches_the_capture(self):
        dumped = raw_dwords()
        defines = header_defines()
        for offset, macro in sorted(TIMING.items()):
            with self.subTest(offset="0x%06X" % offset):
                readings = set(dumped[offset])
                self.assertEqual(
                    1,
                    len(readings),
                    "0x%06X was read %d times and the readings DISAGREE (%s) "
                    "-- it is a sample, not a constant, and must not be "
                    "pinned as %s"
                    % (
                        offset,
                        len(dumped[offset]),
                        sorted("0x%08X" % v for v in readings),
                        macro,
                    ),
                )
                got = readings.pop()
                self.assertEqual(
                    got,
                    defines[macro],
                    "0x%06X: capture says 0x%08X, %s says 0x%08X"
                    % (offset, got, macro, defines[macro]),
                )

    def test_the_plane_base_is_where_the_header_believes(self):
        base, _ = plane_dwords()
        self.assertEqual(
            PLANE_BASE_EXPECTED,
            base,
            "SECTION 4 puts plane 1 at 0x%06X, not 0x%06X -- every delta "
            "below would resolve to the wrong register"
            % (base, PLANE_BASE_EXPECTED),
        )

    def test_every_plane_delta_was_dumped(self):
        _, deltas = plane_dwords()
        missing = sorted(
            "+0x%02X" % delta for delta in PLANE if delta not in deltas
        )
        self.assertEqual(
            [],
            missing,
            "REFUSAL: these plane deltas are no longer in SECTION 4, so the "
            "comparisons below would pass over an empty set: %s" % missing,
        )

    def test_every_plane_macro_is_present(self):
        defines = header_defines()
        missing = sorted(m for m in PLANE.values() if m not in defines)
        self.assertEqual([], missing, "header macros missing: %s" % missing)

    def test_each_plane_value_matches_the_capture(self):
        base, deltas = plane_dwords()
        defines = header_defines()
        for delta, macro in sorted(PLANE.items()):
            with self.subTest(register="0x%06X" % (base + delta)):
                readings = set(deltas[delta])
                self.assertEqual(
                    1,
                    len(readings),
                    "+0x%02X was read %d times and the readings DISAGREE "
                    "(%s) -- it is a sample, not a constant, and must not be "
                    "pinned as %s"
                    % (
                        delta,
                        len(deltas[delta]),
                        sorted("0x%08X" % v for v in readings),
                        macro,
                    ),
                )
                got = readings.pop()
                self.assertEqual(
                    got,
                    defines[macro],
                    "0x%06X (+0x%02X): capture says 0x%08X, %s says 0x%08X"
                    % (base + delta, delta, got, macro, defines[macro]),
                )

    def test_the_two_equal_backlight_words_are_equal_in_the_capture(self):
        dumped = raw_dwords()
        self.assertEqual(
            set(dumped[0x0C8254]),
            set(dumped[0x0C8258]),
            "the header keeps these as TWO constants because the equality is "
            "a fact about this panel rather than a rule; if the capture no "
            "longer shows them equal, that reasoning has changed",
        )


class DsiTests(unittest.TestCase):
    """The DSI block's evidence is a LAYOUT, so it is checked as one.

    intel-handoff28.md section 4 is careful about what the capture establishes
    here: two pages at a fixed spacing holding identical non-trivial reset state.
    That is weaker than reading a programmed mode out of both and far stronger
    than a stride this project invented. The arms below pin exactly that claim
    and no more.
    """

    def test_both_dsi_pages_were_dumped(self):
        dumped = raw_dwords()
        defines = header_defines()
        for macro in (
            "IG_OBSERVED_TRANS_DSI0_BASE_2026_08_18",
            "IG_OBSERVED_TRANS_DSI1_BASE_2026_08_18",
        ):
            with self.subTest(macro=macro):
                base = defines[macro]
                inside = [off for off in dumped if base <= off < base + 0x800]
                self.assertTrue(
                    inside,
                    "%s claims a DSI page at 0x%06X, and SECTION 1 dumped NO "
                    "register inside it -- the address has no evidence"
                    % (macro, base),
                )

    def test_the_dsi_stride_is_the_gap_between_the_bases(self):
        defines = header_defines()
        gap = (
            defines["IG_OBSERVED_TRANS_DSI1_BASE_2026_08_18"]
            - defines["IG_OBSERVED_TRANS_DSI0_BASE_2026_08_18"]
        )
        self.assertEqual(
            defines["IG_OBSERVED_TRANS_DSI_STRIDE_2026_08_18"],
            gap,
            "the stride must be the arithmetic gap between the two bases; a "
            "stride that disagrees with its own operands is invented",
        )

    def test_both_dsi_pages_hold_the_same_ddi_func_ctl(self):
        dumped = raw_dwords()
        defines = header_defines()
        for offset in DSI_CTL_OFFSETS:
            with self.subTest(offset="0x%06X" % offset):
                self.assertIn(
                    offset,
                    dumped,
                    "REFUSAL: 0x%06X is no longer in SECTION 1, so the "
                    "identical-reset-state claim rests on one page" % offset,
                )
                readings = set(dumped[offset])
                self.assertEqual(
                    1,
                    len(readings),
                    "0x%06X was read %d times and DISAGREES (%s)"
                    % (
                        offset,
                        len(dumped[offset]),
                        sorted("0x%08X" % v for v in readings),
                    ),
                )
                self.assertEqual(
                    readings.pop(),
                    defines[DSI_CTL_MACRO],
                    "0x%06X disagrees with %s -- the stride evidence is that "
                    "BOTH pages hold this word" % (offset, DSI_CTL_MACRO),
                )


class SubjectSetTests(unittest.TestCase):
    """9a item 2: the subject set is DERIVED from the header, never spelled.

    A table that names its own subjects passes silently over a constant added
    later -- intel-handoff26.md section 2a is the reasoning, and section 3 of
    intel-handoff28.md is what a silently narrowed subject set costs. The arm
    below fails on the commit that adds an uncovered IG_OBSERVED_* macro, and
    names it.
    """

    def test_every_observed_macro_is_compared_or_exempt(self):
        covered = (
            {macro for _, macro in ANCHORED.values()}
            | set(UNANCHORED.values())
            | set(TIMING.values())
            | set(PLANE.values())
            | {DSI_CTL_MACRO}
        )
        accounted = covered | set(EXEMPT)
        orphans = sorted(set(header_defines()) - accounted)
        self.assertEqual(
            [],
            orphans,
            "these IG_OBSERVED_* constants are compared against NOTHING. Add "
            "each to a table above, or to EXEMPT with the reason it has no "
            "single dword to compare against: %s" % orphans,
        )

    def test_no_exemption_names_a_macro_the_header_lost(self):
        stale = sorted(set(EXEMPT) - set(header_defines()))
        self.assertEqual(
            [],
            stale,
            "REFUSAL: these exemptions name macros that no longer exist, so "
            "they are excusing nothing: %s" % stale,
        )

    def test_no_macro_is_both_compared_and_exempt(self):
        covered = (
            {macro for _, macro in ANCHORED.values()}
            | set(UNANCHORED.values())
            | set(TIMING.values())
            | set(PLANE.values())
            | {DSI_CTL_MACRO}
        )
        both = sorted(covered & set(EXEMPT))
        self.assertEqual(
            [],
            both,
            "an exemption for a macro that IS compared makes the set-equality "
            "arm satisfiable without the comparison: %s" % both,
        )


class ExclusionTests(unittest.TestCase):
    """A refusal is not a reading, and the header must keep saying so."""

    def test_the_refused_rows_really_did_read_all_ones(self):
        rows = named_rows()
        for name, offset in sorted(REFUSED_ROWS.items()):
            with self.subTest(register=name):
                readings = rows[name]
                self.assertIn(
                    (offset, 0xFFFFFFFF),
                    readings,
                    "%s no longer reads 0xFFFFFFFF, so the reason it is "
                    "excluded from the header has changed" % name,
                )

    def test_no_header_constant_holds_a_refusal_value(self):
        for macro, value in sorted(header_defines().items()):
            with self.subTest(macro=macro):
                self.assertNotEqual(
                    0xFFFFFFFF,
                    value,
                    "%s pins an all-ones read. On this silicon that means the "
                    "block was not decoding -- it is a refusal, not a "
                    "measurement." % macro,
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)
