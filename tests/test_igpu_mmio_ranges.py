#!/usr/bin/env python3
r"""test_igpu_mmio_ranges.py -- the three MMIO range tables must not drift apart.

Why this exists
---------------
``tools/read-igpu-mmio.py`` and ``tools/read-igpu-mmio.sh`` are twins: same
display-engine ranges, same output format, same REACH guard. The shell one
exists so a capture needs nothing but coreutils; the Python one exists because
it can ``mmap`` a memory BAR, which a shell cannot.

``tools/igpu-capture.sh`` carries a THIRD copy, embedded as a ``python3``
heredoc, because the operator asked for one file and one command and a dumper
sitting in a second file is a dependency that has already failed a run
(2026-08-15 15:50 UTC). That copy is the one that actually ran on the target
laptop, so it is the one whose drift would be least visible.

Three copies of a register map is the setup for a silent divergence. Add a range
to one and the others keep producing a dump that LOOKS complete -- same header,
same REACH line, same OK verdict -- while missing a block entirely. Nothing in
the artifact says which table produced it, so a value "absent from the dump"
would read as absent from the hardware.

This is the same failure this project has already written gates against:
``check-header-coverage.py`` for the hand-written clang list, and
``test_cmake_registration.py`` for the hand-written ctest list. A hand-kept
list's only failure mode is silence.

What this test does NOT check
----------------------------
Whether any offset is CORRECT. That authority is ``reference/i915/i915_reg.h``,
and a wrong offset copied identically into both files passes here. This test
asserts agreement, not truth. ``test_igpu_range_offsets_are_in_the_vendored_header``
below is the one that reaches for truth, and it does so only for the anchors it
can name.

Non-vacuity
-----------
A parser that matches nothing returns two empty lists, and two empty lists are
equal. So every extraction here asserts a FLOOR on what it found before
comparing, and the floors are derived from one dated measurement
(2026-08-15: 15 ranges in each file).
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY_TOOL = ROOT / "tools" / "read-igpu-mmio.py"
SH_TOOL = ROOT / "tools" / "read-igpu-mmio.sh"
EMBED_TOOL = ROOT / "tools" / "igpu-capture.sh"
# The one-boot final capture. NOT a fourth member of the equality above -- it is
# a documented SUPERSET, and the classes at the bottom of this file assert that
# relationship instead. See TheFinalCaptureIsASuperset for why.
FINAL_TOOL = ROOT / "tools" / "igpu-final-capture.sh"
I915_REG = ROOT / "reference" / "i915" / "i915_reg.h"
PLANE_REG = ROOT / "reference" / "i915" / "skl_universal_plane_regs.h"

# Measured 2026-08-15 by parsing all three files. A floor, not an expected
# value -- adding a range should not redden this gate, deleting the table should.
RANGE_FLOOR = 15

# Anchors: (symbol, header, the range base it must fall inside).
# Each is a register this project will actually read, spelled exactly as the
# vendored Intel header spells it.
ANCHORS = (
    ("_TRANS_HTOTAL_A", "i915_reg.h", 0x060000),
    ("_TRANS_VTOTAL_A", "i915_reg.h", 0x060000),
    ("_PIPEASRC", "i915_reg.h", 0x060000),
    ("_TRANSACONF", "i915_reg.h", 0x070000),
    ("CDCLK_CTL", "i915_reg.h", 0x046000),
    ("_PLANE_CTL_1_A", "skl_universal_plane_regs.h", 0x070000),
    ("_PLANE_SURF_1_A", "skl_universal_plane_regs.h", 0x070000),
    ("_TRANS_HTOTAL_B", "i915_reg.h", 0x061000),
)


def _python_list_ranges(path):
    """Extract (base, length) pairs from a ``RANGES = [...]`` Python list.

    Bounded by the list itself rather than by matching tuples anywhere in the
    file. Both files that use this form carry hex in prose above the table --
    igpu-capture.sh especially, whose header block quotes kernel line numbers
    and register addresses -- and an unbounded scan would parse documentation
    and call it a range.
    """
    text = path.read_text(encoding="utf-8")
    block = re.search(r"^RANGES = \[(.*?)^\]", text, re.S | re.M)
    if not block:
        return []
    pairs = re.findall(
        r"\(\s*(0[xX][0-9a-fA-F]+)\s*,\s*(0[xX][0-9a-fA-F]+)\s*,", block.group(1)
    )
    return [(int(b, 16), int(l, 16)) for b, l in pairs]


def py_ranges():
    """Extract (base, length) pairs from the Python tool's RANGES list."""
    return _python_list_ranges(PY_TOOL)


def embed_ranges():
    """Extract the copy embedded in igpu-capture.sh's python3 heredoc.

    Same ``RANGES = [...]`` shape as the standalone Python tool, because inside
    the heredoc it IS Python. The heredoc body is unindented, so the ``^``
    anchors in the shared parser hold.
    """
    return _python_list_ranges(EMBED_TOOL)


def final_ranges():
    """Extract the superset table from tools/igpu-final-capture.sh.

    Same ``RANGES = [...]`` shape and the same shared parser, for the same
    reason embed_ranges() can use it: inside the python3 heredoc it IS Python,
    and the heredoc body is unindented so the ``^`` anchors hold.

    That file's header block quotes far more hex in prose than any of the other
    three -- a whole citation table of register offsets and header line numbers
    -- so being bounded by the list rather than scanning the file matters more
    here than anywhere else in this module.
    """
    return _python_list_ranges(FINAL_TOOL)


def sh_ranges():
    """Extract (base, length) pairs from the shell tool's heredoc table.

    Bounded by the BEGIN-RANGES / END-RANGES markers rather than by matching
    hex anywhere in the file -- the file is full of hex in prose, and a
    whole-file scan would pick up documentation and call it a range.
    """
    text = SH_TOOL.read_text(encoding="utf-8")
    block = re.search(r"# BEGIN-RANGES(.*?)# END-RANGES", text, re.S)
    if not block:
        return []
    out = []
    for line in block.group(1).splitlines():
        m = re.match(r"\s*(0[xX][0-9a-fA-F]+)\s+(0[xX][0-9a-fA-F]+)\s+\S", line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2), 16)))
    return out


def header_offset(symbol, header):
    """Return the numeric value of `#define <symbol> <hex>`, or None."""
    path = ROOT / "reference" / "i915" / header
    if not path.is_file():
        return None
    pat = re.compile(
        r"^#define\s+" + re.escape(symbol) + r"\s+(?:_MMIO\()?(0[xX][0-9a-fA-F]+)",
        re.M,
    )
    m = pat.search(path.read_text(encoding="utf-8", errors="replace"))
    return int(m.group(1), 16) if m else None


class AllThreeToolsExist(unittest.TestCase):
    def test_the_python_twin_exists(self):
        """A missing file would make every extraction below vacuously empty."""
        self.assertTrue(PY_TOOL.is_file(), "%s is missing" % PY_TOOL)

    def test_the_shell_twin_exists(self):
        """The shell twin is the one that needs no interpreter."""
        self.assertTrue(SH_TOOL.is_file(), "%s is missing" % SH_TOOL)

    def test_the_one_file_capture_exists(self):
        """The copy that actually ran on the laptop, embedded as a heredoc."""
        self.assertTrue(EMBED_TOOL.is_file(), "%s is missing" % EMBED_TOOL)


class ExtractionIsNotVacuous(unittest.TestCase):
    """Two empty lists compare equal, so prove each parser found something."""

    def test_python_table_meets_the_floor(self):
        got = py_ranges()
        self.assertGreaterEqual(
            len(got),
            RANGE_FLOOR,
            "parsed only %d ranges from read-igpu-mmio.py; the RANGES list was "
            "reformatted or removed, so the comparison below would be vacuous"
            % len(got),
        )

    def test_shell_table_meets_the_floor(self):
        got = sh_ranges()
        self.assertGreaterEqual(
            len(got),
            RANGE_FLOOR,
            "parsed only %d ranges from read-igpu-mmio.sh; the BEGIN-RANGES / "
            "END-RANGES markers or the heredoc changed shape" % len(got),
        )

    def test_embedded_table_meets_the_floor(self):
        got = embed_ranges()
        self.assertGreaterEqual(
            len(got),
            RANGE_FLOOR,
            "parsed only %d ranges from igpu-capture.sh; the embedded RANGES "
            "list was reformatted, indented, or removed from the heredoc, so "
            "the comparison below would be vacuous" % len(got),
        )

    def test_the_embedded_scan_is_bounded_to_the_list(self):
        """Negative control for the shared Python-list parser.

        igpu-capture.sh's header block quotes kernel line numbers and register
        addresses in prose. If the parser were matching hex tuples anywhere in
        the file, deleting the list would still return ranges and the floor
        above would pass on documentation.
        """
        text = EMBED_TOOL.read_text(encoding="utf-8")
        gutted = re.sub(r"^RANGES = \[.*?^\]", "RANGES = []", text, flags=re.S | re.M)
        self.assertNotEqual(gutted, text, "the RANGES list was not found to gut")
        block = re.search(r"^RANGES = \[(.*?)^\]", gutted, re.S | re.M)
        found = re.findall(
            r"\(\s*(0[xX][0-9a-fA-F]+)\s*,\s*(0[xX][0-9a-fA-F]+)\s*,",
            block.group(1) if block else "",
        )
        self.assertEqual(found, [])

    def test_a_wrong_marker_name_finds_nothing(self):
        """Negative control: the bounded scan really is bounded.

        If this found ranges, ``sh_ranges`` would be matching hex anywhere in
        the file rather than inside the table, and the floor above would pass
        on prose.
        """
        text = SH_TOOL.read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"# BEGIN-RANGE5(.*?)# END-RANGE5", text, re.S))


class TablesAgree(unittest.TestCase):
    def test_the_two_tables_are_identical_in_order(self):
        """Same ranges, same order. Order matters: the dumps are diffed."""
        self.assertEqual(
            py_ranges(),
            sh_ranges(),
            "read-igpu-mmio.py and read-igpu-mmio.sh disagree about which "
            "display ranges to dump. Whichever produced an artifact, the other "
            "would silently omit a block while still printing OK.",
        )

    def test_the_embedded_table_matches_the_standalone_python_one(self):
        """igpu-capture.sh's heredoc copy is the one that ran on the laptop.

        Asserted against the standalone .py rather than the .sh because the two
        are literally the same source shape, so a diff here is a real edit
        rather than a formatting difference between two spellings of a table.
        """
        self.assertEqual(
            embed_ranges(),
            py_ranges(),
            "igpu-capture.sh's embedded RANGES disagrees with "
            "read-igpu-mmio.py. The embedded one is what produced the artifacts "
            "in hand, so a range missing there is a hole in the evidence that "
            "no REACH line reports.",
        )

    def test_all_three_tables_agree(self):
        """Stated once directly, so a failure names the three-way property."""
        tables = {"py": py_ranges(), "sh": sh_ranges(), "embedded": embed_ranges()}
        distinct = {tuple(v) for v in tables.values()}
        self.assertEqual(
            len(distinct),
            1,
            "the three MMIO range tables are not all equal: %s"
            % {k: len(v) for k, v in tables.items()},
        )


class RangesAreMappable(unittest.TestCase):
    def test_every_base_is_page_aligned(self):
        """mmap and dd both seek in whole pages; a stray base shifts the dump.

        An unaligned base does not error -- mmap rejects it, but dd would
        silently round the skip and label the output with the wrong offset,
        which is worse.
        """
        for base, _ in py_ranges():
            self.assertEqual(base % 4096, 0, "base 0x%06X is not page aligned" % base)

    def test_every_length_is_a_whole_number_of_pages(self):
        """dd counts in 4096-byte blocks, so a partial page is truncated."""
        for base, length in py_ranges():
            self.assertEqual(
                length % 4096,
                0,
                "length 0x%04X at base 0x%06X is not a multiple of 4096"
                % (length, base),
            )

    def test_no_two_ranges_overlap(self):
        """An overlap would dump the same register twice under two labels."""
        spans = sorted(py_ranges())
        for (b1, l1), (b2, _) in zip(spans, spans[1:]):
            self.assertLessEqual(
                b1 + l1,
                b2,
                "range at 0x%06X +0x%04X overlaps the next at 0x%06X"
                % (b1, l1, b2),
            )


class AnchorsResolveInTheVendoredHeaders(unittest.TestCase):
    """The only tests here that reach past agreement toward correctness."""

    def test_the_vendored_headers_are_present(self):
        """Without these, every anchor lookup returns None and skips."""
        self.assertTrue(I915_REG.is_file(), "%s is missing" % I915_REG)
        self.assertTrue(PLANE_REG.is_file(), "%s is missing" % PLANE_REG)

    def test_every_anchor_is_found_in_its_header(self):
        """A symbol that does not resolve makes the next test vacuous."""
        for symbol, header, _ in ANCHORS:
            self.assertIsNotNone(
                header_offset(symbol, header),
                "%s not found in reference/i915/%s -- either the symbol was "
                "renamed upstream or the re-vendor changed its spelling"
                % (symbol, header),
            )

    def test_every_anchor_falls_inside_the_range_that_claims_it(self):
        """The offsets this project will read must be inside a dumped range.

        This is what makes the range table evidence rather than a guess: each
        base is asserted against Intel's own definition of a register that
        lives there.
        """
        table = py_ranges()
        for symbol, header, want_base in ANCHORS:
            off = header_offset(symbol, header)
            if off is None:
                continue  # reported by the test above
            match = [(b, l) for b, l in table if b == want_base]
            self.assertEqual(
                len(match),
                1,
                "no single range based at 0x%06X for %s" % (want_base, symbol),
            )
            base, length = match[0]
            self.assertTrue(
                base <= off < base + length,
                "%s = 0x%06X is outside its claimed range 0x%06X +0x%04X"
                % (symbol, off, base, length),
            )


class TheShellToolGuardsItsOutput(unittest.TestCase):
    """The REACH guard is the whole reason a null dump is not read as data."""

    def test_it_reports_reach_and_can_say_vacuous(self):
        text = SH_TOOL.read_text(encoding="utf-8")
        self.assertIn("REACH  :", text)
        self.assertIn("VACUOUS:", text)

    def test_od_keeps_repeated_lines(self):
        """``od`` without -v collapses repeats to ``*`` and eats real content.

        A page of identical register values is normal and meaningful; losing it
        would shrink the dump and the REACH count with no error anywhere.
        """
        text = SH_TOOL.read_text(encoding="utf-8")
        self.assertRegex(text, r"od -An -tx4 -v")

    def test_it_names_which_access_path_produced_the_dump(self):
        """A dump from a path that short-reads is not the same artifact."""
        self.assertIn('path   : $METHOD', SH_TOOL.read_text(encoding="utf-8"))


class TheFinalCaptureIsASuperset(unittest.TestCase):
    r"""``tools/igpu-final-capture.sh`` extends the canonical table, never forks it.

    Why this class is not four more lines in ``TheThreeTablesAgree``
    ---------------------------------------------------------------
    The three tools above are held IDENTICAL on purpose: they answer the same
    question three ways, so any difference between them is a bug in one of them.
    The final capture answers a strictly larger question -- it is the single
    one-boot run after which Linux is never available again, so it also reads
    transcoder/pipe D, the DSI pages, the Gen12 PLL block at 0x164000, and the
    PCH fuse and DPLL pages that no predecessor script ever touched.

    Folding it into the equality would force one of two bad outcomes: either the
    three canonical tools grow ranges they have no reason to read, or the final
    capture loses the pages that are its entire reason for existing. So the
    relationship asserted here is CONTAINMENT, in one direction only.

    What that buys: adding a range to the canonical three without adding it here
    reddens this class. That is the drift the module exists to catch, preserved
    across the asymmetry rather than abandoned at it.
    """

    def test_the_final_capture_exists(self):
        """A missing file makes every extraction below vacuously empty."""
        self.assertTrue(FINAL_TOOL.is_file(), "%s is missing" % FINAL_TOOL)

    def test_its_table_parses_and_is_not_empty(self):
        """An empty list is a superset of nothing and contains nothing.

        Without this, a renamed list or a reindented heredoc would silently
        turn every containment assertion below into a loop over zero items.
        """
        self.assertGreaterEqual(
            len(final_ranges()),
            RANGE_FLOOR,
            "parsed %d ranges from %s; it must carry at least the %d canonical "
            "rows, so a count below the floor means the parser missed the table "
            "rather than that the table shrank"
            % (len(final_ranges()), FINAL_TOOL.name, RANGE_FLOOR),
        )

    def test_every_canonical_range_survives_verbatim(self):
        """Base AND length, unchanged. This is the containment claim itself.

        Comparing bases alone would pass a row that kept 0x044000 but shrank its
        length from 0x2000 to 0x1000 -- which would drop half the north display
        engine interrupt block from the one capture that gets to read it.
        """
        canonical = set(py_ranges())
        final = set(final_ranges())
        missing = sorted(canonical - final)
        self.assertEqual(
            missing,
            [],
            "%s is missing canonical ranges %s -- if a range was added to the "
            "three canonical tools, add it here too"
            % (
                FINAL_TOOL.name,
                ", ".join("0x%06X +0x%04X" % (b, l) for b, l in missing),
            ),
        )

    def test_it_is_a_strict_superset(self):
        """If it carried nothing extra it should have joined the equality.

        This is the anti-degeneracy check for the class: containment alone is
        satisfied by an exact copy, and an exact copy would mean the file's
        stated reason for existing is false.
        """
        extra = set(final_ranges()) - set(py_ranges())
        self.assertNotEqual(
            extra,
            set(),
            "%s carries no range the canonical three lack; either it gained "
            "nothing (in which case it should be one of the three) or the "
            "parser read the wrong list" % FINAL_TOOL.name,
        )

    def test_its_bases_are_page_aligned(self):
        """mmap rejects an unaligned base outright, so the run dies at range 1."""
        for base, _ in final_ranges():
            self.assertEqual(
                base % 4096, 0, "base 0x%06X is not page aligned" % base
            )

    def test_its_lengths_are_whole_pages(self):
        for base, length in final_ranges():
            self.assertEqual(
                length % 4096,
                0,
                "length 0x%04X at base 0x%06X is not a multiple of 4096"
                % (length, base),
            )

    def test_none_of_its_ranges_overlap(self):
        """An overlap dumps one register twice under two labels.

        Worse here than in the canonical table: this artifact is the only one
        that will ever exist, so a duplicated block is read as corroboration.
        """
        spans = sorted(final_ranges())
        for (b1, l1), (b2, _) in zip(spans, spans[1:]):
            self.assertLessEqual(
                b1 + l1,
                b2,
                "range at 0x%06X +0x%04X overlaps the next at 0x%06X"
                % (b1, l1, b2),
            )

    def test_every_anchor_falls_inside_one_of_its_ranges(self):
        """Each symbol this project reads must be inside a range it dumps.

        Asserted against the final table independently rather than inferred
        from containment: containment says the canonical rows are present, and
        an anchor could still be added later that resolves in neither table.
        """
        table = final_ranges()
        for symbol, header, _ in ANCHORS:
            off = header_offset(symbol, header)
            if off is None:
                continue  # reported by AnchorsResolveInTheVendoredHeaders
            covering = [(b, l) for b, l in table if b <= off < b + l]
            self.assertEqual(
                len(covering),
                1,
                "%s = 0x%06X is covered by %d ranges in %s, want exactly 1"
                % (symbol, off, len(covering), FINAL_TOOL.name),
            )


class TheFinalCaptureKeepsItsCaptureDiscipline(unittest.TestCase):
    r"""The properties that make its output evidence rather than a hex dump.

    Each of these was learned from a run that went wrong, and none of them is
    visible in the range table.
    """

    def test_it_has_no_companion_file(self):
        """The dumper is embedded, because the 2026-08-15 15:50 UTC run failed
        for exactly one reason: a second file was not copied alongside the
        first. The heredoc is what makes 'one file, one command' true.
        """
        text = FINAL_TOOL.read_text(encoding="utf-8")
        self.assertIn("<<'PYEOF'", text)
        self.assertIn("PYEOF", text)

    def test_it_reads_a_positive_control_before_reporting_absences(self):
        """An unclaimed page and a failed mmap are indistinguishable.

        Transcoder A's HTOTAL is driving the lit panel during the run, so a dead
        control means the mapping broke -- and every null must then downgrade to
        INCONCLUSIVE rather than being read as a fact about the hardware.
        """
        text = FINAL_TOOL.read_text(encoding="utf-8")
        self.assertIn("CONTROL", text)
        self.assertIn("INCONCLUSIVE", text)
        self.assertIn("READER_OK", text)

    def test_zero_counts_as_dead(self):
        """MEASURED 2026-08-16 on 8086:9a68 (handoff15 sec.2): an unused display
        page on this silicon reads 0x00000000, not all-ones. A dead() that tests
        only for all-ones reports transcoders that certainly exist as ABSENT.
        """
        text = FINAL_TOOL.read_text(encoding="utf-8")
        self.assertIn("TRIVIAL = (0x00000000, 0xFFFFFFFF)", text)

    def test_its_verdict_word_does_not_overclaim(self):
        """One read cannot separate a decoded-but-idle block from a missing one,
        so the word must carry both readings.
        """
        text = FINAL_TOOL.read_text(encoding="utf-8")
        self.assertIn("UNPROGRAMMED-OR-ABSENT", text)
        self.assertNotIn("= 'ABSENT'", text)

    def test_it_opens_the_bar_read_only(self):
        """The panel is lit for the whole run. A write through BAR0 while the
        display engine is scanning out is the one way this script could damage
        the evidence it exists to collect -- or the machine.
        """
        text = FINAL_TOOL.read_text(encoding="utf-8")
        self.assertIn("os.O_RDONLY", text)
        self.assertIn("mmap.PROT_READ", text)
        self.assertNotIn("O_RDWR", text)
        self.assertNotIn("PROT_WRITE", text)

    def test_it_binds_and_unbinds_nothing(self):
        """Unbinding i915 blanks the panel, and a blanked panel means every
        timing register afterwards describes a display that stopped.
        """
        text = FINAL_TOOL.read_text(encoding="utf-8")
        for forbidden in ("driver/unbind", "driver/bind", "modprobe -r", "rmmod"):
            self.assertNotIn(
                forbidden,
                text,
                "%s would disturb the lit panel the capture depends on"
                % forbidden,
            )

    def test_it_records_the_perishable_provenance(self):
        """lockdown state, kernel cmdline and driver binding decide whether the
        run was even permitted to read BAR0. Without them a null dump cannot be
        attributed, and there is no second boot to re-attribute it from.
        """
        text = FINAL_TOOL.read_text(encoding="utf-8")
        for token in ("lockdown", "/proc/cmdline", "resource0", "connector"):
            self.assertIn(token, text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
