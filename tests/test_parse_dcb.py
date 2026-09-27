#!/usr/bin/env python3
"""test_parse_dcb.py -- gate over tools/parse-dcb.py.

What this pins, and why the emphasis is where it is
---------------------------------------------------
The subject's headline finding is an ABSENCE: the GA107 VBIOS declares zero
display paths. An absence is the one result a decoder cannot report honestly on
its own evidence, because landing one field off, landing on the wrong table, and
reading a truncated dump all produce the identical output. So the bulk of this
module is not "does the happy path decode" -- it is that each arm of
`corroborate_absence()` is individually FALSIFIABLE, and that a failed arm flips
the verdict from AUTHORED to UNPROVEN.

That is the direction that matters. A corroboration function whose arms cannot
fail is an `ok` line dressed as proof, and it would ratify a misread as the
board's own statement -- the exact class recorded in
`an-assertion-that-cannot-fail-is-not-coverage` and
`a-vacuity-refusal-must-not-preempt-a-diagnosis`.

The synthetic images are built here rather than carved out of the real ROM, for
the reason 3050-handoff27 section 2b item 5 records: a fixture that only ever
constructs the shape it expects cannot discover that a second shape exists. So
both are driven -- synthetic images for the arms and the refusals, and the real
65024-byte file for the finding itself, in a class that SKIPS with a banner when
`reference/ga107-firmware/` is absent.
"""

import importlib.util
import os
import struct
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(ROOT, "tools", "parse-dcb.py")
REAL_ROM = os.path.join(ROOT, "reference", "ga107-firmware", "vbios-ga107.rom")
REPORT = os.path.join(ROOT, "dcb-report.txt")


def load_tool():
    """Load the hyphenated tool by path; a normal import cannot name it."""
    spec = importlib.util.spec_from_file_location("parse_dcb_under_test", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


dcb = load_tool()


# ---------------------------------------------------------------------------
# Image construction
# ---------------------------------------------------------------------------

IMAGE_SIZE = 4096
DCB_BASE = 0x0800
SUB_CCB = 0x0900
SUB_GPIO = 0x0A00
SUB_I2C = 0x0B00
SUB_CONN = 0x0C00

# The connector sub-table's own layout, as build_subtable() writes it below.
# Named rather than spelled inline because a populated fixture has to seek to
# row N, and a hand-written 5 or 4 in that arithmetic is a second copy of a
# layout that would then silently disagree with the builder.
CONN_HEADER_SIZE = 5
CONN_ENTRY_SIZE = 4


def build_subtable(version, header_size, count, entry_size, fill=0x11):
    """A DCB sub-table: the four-byte quartet, then count*entry_size bytes."""
    body = bytearray()
    body += bytes([version, header_size, count, entry_size])
    body += bytes(header_size - 4)
    body += bytes([fill]) * (count * entry_size)
    return bytes(body)


def build_image(entries=(), entry_count=None, size=IMAGE_SIZE,
                signature=None, dcb_base=DCB_BASE, header_size=None,
                entry_size=8, version=0x41, ccb_count=15, gpio_count=36,
                i2c_count=32, conn_count=0, conn_types=None, dcb_pointer=None,
                image_length=None, last_image=True):
    """Build a minimal but structurally valid VBIOS carrying one DCB.

    Every knob here exists because some negative control needs to move exactly
    one thing. Defaults produce an image whose absence corroboration passes all
    seven arms, so a control's failure is attributable to the knob it moved.
    """
    if signature is None:
        signature = dcb.DCB_SIGNATURE
    if header_size is None:
        header_size = dcb.DCB_HEADER_41_SIZE
    if dcb_pointer is None:
        dcb_pointer = dcb_base
    if entry_count is None:
        entry_count = len(entries)

    rom = bytearray(size)

    # PCI expansion ROM header plus a PCIR structure at +0x170.
    rom[0:2] = b"\x55\xAA"
    pcir = 0x170
    struct.pack_into("<H", rom, 0x18, pcir)
    rom[pcir:pcir + 4] = b"PCIR"
    struct.pack_into("<H", rom, pcir + 0x04, 0x10DE)
    struct.pack_into("<H", rom, pcir + 0x06, 0x25A2)
    struct.pack_into("<H", rom, pcir + 0x0A, 0x18)
    length = size if image_length is None else image_length
    struct.pack_into("<H", rom, pcir + 0x10, length // 512)
    rom[pcir + 0x14] = 0x00
    rom[pcir + 0x15] = 0x80 if last_image else 0x00

    # The classic DCB pointer.
    struct.pack_into("<H", rom, dcb.DCB_PTR_OFFSET, dcb_pointer)

    # The DCB header itself.
    rom[dcb_base + 0x00] = version
    rom[dcb_base + 0x01] = header_size
    rom[dcb_base + 0x02] = entry_count
    rom[dcb_base + 0x03] = entry_size
    struct.pack_into("<H", rom, dcb_base + 0x04, SUB_CCB)
    struct.pack_into("<I", rom, dcb_base + 0x06, signature)
    struct.pack_into("<H", rom, dcb_base + 0x0A, SUB_GPIO)
    struct.pack_into("<H", rom, dcb_base + 0x12, SUB_I2C)
    struct.pack_into("<H", rom, dcb_base + 0x14, SUB_CONN)
    rom[dcb_base + 0x16] = 0xC1

    # Display path entries, then an explicit EOL word.
    area = dcb_base + header_size
    for index, (word0, word1) in enumerate(entries):
        struct.pack_into("<II", rom, area + index * entry_size, word0, word1)
    struct.pack_into("<I", rom, area + len(entries) * entry_size, 0x0000000E)

    # Sibling sub-tables. The connector table is the one a caller may want
    # POPULATED rather than merely counted: conn_types supersedes conn_count and
    # writes one entry per element with that type in the low byte. build_subtable
    # fills every row with the SAME byte, which is exactly the fill a decoder bug
    # returning row 0 for every index would survive -- so a per-row assertion
    # needs per-row content. Left None this whole block is byte-identical to what
    # it was, so every pre-existing fixture is unchanged.
    conn_rows = conn_count if conn_types is None else len(conn_types)
    for offset, count, esize in ((SUB_CCB, ccb_count, 4),
                                 (SUB_GPIO, gpio_count, 6),
                                 (SUB_I2C, i2c_count, 4),
                                 (SUB_CONN, conn_rows, 4)):
        table = build_subtable(0x41, 5, count, esize)
        rom[offset:offset + len(table)] = table

    if conn_types is not None:
        for index, ctype in enumerate(conn_types):
            rom[SUB_CONN + CONN_HEADER_SIZE + index * CONN_ENTRY_SIZE] = (
                ctype & 0xFF)

    return bytes(rom)


def path_entry(dtype, head=1, connector=0, output_resource=0, boot=0):
    """Compose one display-path entry pair from its bit fields."""
    word0 = (dtype & 0xF) | ((head & 0xF) << 8) | ((connector & 0xF) << 12)
    word0 |= (boot & 0x1) << 22
    word1 = (output_resource & 0xF) << 24
    return word0, word1


TMDS = 0x2
DISPLAYPORT = 0x6
LVDS = 0x3
EOL = 0xE
SKIP = 0xF


# ---------------------------------------------------------------------------
# The fixture itself must be trustworthy before anything is asserted over it
# ---------------------------------------------------------------------------

class FixtureTests(unittest.TestCase):
    """A control on build_image(): the default must be the all-pass case."""

    def test_default_image_is_found_and_parsed(self):
        rom = build_image()
        base = dcb.find_dcb(rom)
        self.assertEqual(DCB_BASE, base)
        header = dcb.parse_header(rom, base)
        self.assertEqual(0x41, header["version"])
        self.assertEqual(dcb.DCB_HEADER_41_SIZE, header["header_size"])

    def test_default_image_corroborates_every_arm(self):
        """If this ever reddens, every negative control below is unattributable."""
        rom = build_image()
        header = dcb.parse_header(rom, dcb.find_dcb(rom))
        result = dcb.corroborate_absence(rom, header, [])
        self.assertIsNotNone(result)
        failed = [label for label, ok in result["arms"] if not ok]
        self.assertEqual([], failed, "the all-pass fixture failed arms: %r"
                         % failed)
        self.assertTrue(result["authored"])

    def test_header_field_widths_sum_to_the_declared_size(self):
        """The table and DCB_HEADER_41_SIZE are two spellings of one fact."""
        total = sum(width for _, _, width in dcb.DCB_HEADER_FIELDS)
        self.assertEqual(dcb.DCB_HEADER_41_SIZE, total)

    def test_header_fields_are_contiguous_and_ordered(self):
        """A gap or overlap would silently misread every field after it."""
        cursor = 0
        for name, offset, width in dcb.DCB_HEADER_FIELDS:
            self.assertEqual(cursor, offset, "%s is not contiguous" % name)
            cursor += width


# ---------------------------------------------------------------------------
# find_dcb() refuses rather than guessing
# ---------------------------------------------------------------------------

class FindDcbRefusalTests(unittest.TestCase):
    """Each refusal is a separate reason, and none may be silently absorbed."""

    def test_zero_pointer_refuses(self):
        rom = build_image(dcb_pointer=0)
        with self.assertRaises(dcb.DcbError) as caught:
            dcb.find_dcb(rom)
        self.assertIn("zero", str(caught.exception))

    def test_pointer_past_end_of_image_refuses(self):
        rom = build_image(dcb_pointer=IMAGE_SIZE - 4)
        with self.assertRaises(dcb.DcbError) as caught:
            dcb.find_dcb(rom)
        self.assertIn("past", str(caught.exception))

    def test_wrong_signature_refuses(self):
        rom = build_image(signature=0xDEADBEEF)
        with self.assertRaises(dcb.DcbError) as caught:
            dcb.find_dcb(rom)
        self.assertIn("signature", str(caught.exception))

    def test_image_too_short_for_the_pointer_refuses(self):
        with self.assertRaises(dcb.DcbError) as caught:
            dcb.find_dcb(b"\x55\xAA")
        self.assertIn("too short", str(caught.exception))

    def test_a_signature_elsewhere_is_not_followed(self):
        """The pointer at 0x36 is the only way in.

        A signature scan as a fallback would be worse than no decoder: a 32-bit
        constant occurs by chance often enough in 64 KiB that finding one proves
        nothing, and a decode landing somewhere plausible reports fabricated
        board wiring. So an image whose pointer is zero must refuse EVEN WHEN a
        valid signature is present elsewhere.
        """
        rom = bytearray(build_image(dcb_pointer=0))
        struct.pack_into("<I", rom, 0x0400, dcb.DCB_SIGNATURE)
        with self.assertRaises(dcb.DcbError):
            dcb.find_dcb(bytes(rom))


# ---------------------------------------------------------------------------
# Entry decoding
# ---------------------------------------------------------------------------

class EntryDecodeTests(unittest.TestCase):

    def test_each_field_is_read_from_its_own_bits(self):
        """Distinct values per field, so a swapped pair cannot pass."""
        word0, word1 = path_entry(TMDS, head=0x4, connector=0x7,
                                  output_resource=0x3, boot=1)
        entry = dcb.decode_entry(word0, word1)
        self.assertEqual(TMDS, entry["type"])
        self.assertEqual(0x4, entry["head_mask"])
        self.assertEqual(0x7, entry["connector"])
        self.assertEqual(0x3, entry["or"])
        self.assertEqual(1, entry["boot_device"])

    def test_every_type_nibble_has_a_name(self):
        """All sixteen values, so a deleted row reddens on the commit."""
        for value in range(16):
            self.assertIn(value, dcb.DCB_TYPE_NAMES)
            self.assertTrue(dcb.DCB_TYPE_NAMES[value])

    def test_type_names_are_pairwise_distinct(self):
        """A copy-paste row returning a sibling's name satisfies 'has a name'."""
        names = list(dcb.DCB_TYPE_NAMES.values())
        self.assertEqual(len(names), len(set(names)))

    def test_external_types_are_tmds_and_displayport(self):
        """The whole output question rests on this set; pin it explicitly."""
        self.assertEqual((TMDS, DISPLAYPORT), dcb.EXTERNAL_TYPES)
        self.assertNotIn(LVDS, dcb.EXTERNAL_TYPES)

    def test_entries_are_walked_at_the_declared_stride(self):
        entries = [path_entry(TMDS, connector=1), path_entry(LVDS, connector=2)]
        rom = build_image(entries=entries)
        header = dcb.parse_header(rom, dcb.find_dcb(rom))
        walked = dcb.parse_entries(rom, header)
        self.assertEqual(2, len(walked))
        self.assertEqual(TMDS, walked[0]["type"])
        self.assertEqual(LVDS, walked[1]["type"])
        self.assertEqual(walked[1]["offset"] - walked[0]["offset"],
                         header["entry_size"])

    def test_entry_size_too_small_refuses(self):
        rom = build_image(entry_size=4, entries=())
        header = dcb.parse_header(rom, dcb.find_dcb(rom))
        with self.assertRaises(dcb.DcbError) as caught:
            dcb.parse_entries(rom, header)
        self.assertIn("entry_size", str(caught.exception))

    def test_count_running_past_the_image_refuses(self):
        """A count is untrusted input; it must not read outside the file.

        The count is ONE BYTE, so 255 entries at 8 bytes each is the largest
        overrun expressible -- 2040 bytes from a header at 0x823, which is the
        first value that leaves the 4096-byte fixture. An earlier draft used 200
        and reddened: 200 entries FIT, so the control was asserting a refusal the
        subject was correct not to make.
        """
        rom = build_image(entry_count=255)
        header = dcb.parse_header(rom, dcb.find_dcb(rom))
        with self.assertRaises(dcb.DcbError) as caught:
            dcb.parse_entries(rom, header)
        self.assertIn("past", str(caught.exception))


# ---------------------------------------------------------------------------
# The absence corroboration: every arm must be individually falsifiable
# ---------------------------------------------------------------------------

class AbsenceCorroborationTests(unittest.TestCase):
    """Each control moves ONE thing and asserts WHICH arm broke.

    A control asserting only "the verdict is not AUTHORED" would pass for the
    wrong reason -- any other arm breaking satisfies it too. So each names its
    own arm, and additionally asserts every OTHER arm stayed green.
    """

    def arms(self, rom):
        header = dcb.parse_header(rom, dcb.find_dcb(rom))
        result = dcb.corroborate_absence(rom, header, [])
        self.assertIsNotNone(result)
        return result, dict(result["arms"])

    def assert_only_arm_failed(self, arms, needle):
        failed = [label for label, ok in arms.items() if not ok]
        self.assertEqual(1, len(failed),
                         "expected exactly one failed arm, got %r" % failed)
        self.assertIn(needle, failed[0])

    def test_absence_is_none_when_a_live_path_exists(self):
        """Corroboration is for the zero case only; a present path needs none."""
        rom = build_image(entries=[path_entry(TMDS)])
        header = dcb.parse_header(rom, dcb.find_dcb(rom))
        entries = dcb.parse_entries(rom, header)
        self.assertIsNone(dcb.corroborate_absence(rom, header, entries))

    def test_eol_and_skip_entries_do_not_count_as_live(self):
        """Otherwise a terminator would mask a genuine absence."""
        rom = build_image(entries=[path_entry(EOL), path_entry(SKIP)])
        header = dcb.parse_header(rom, dcb.find_dcb(rom))
        entries = dcb.parse_entries(rom, header)
        self.assertEqual(2, len(entries))
        self.assertIsNotNone(dcb.corroborate_absence(rom, header, entries))

    def test_truncated_image_breaks_only_the_chain_arm(self):
        rom = build_image(image_length=IMAGE_SIZE * 4)
        result, arms = self.arms(rom)
        self.assert_only_arm_failed(arms, "image chain complete")
        self.assertFalse(result["authored"])

    def test_unterminated_chain_breaks_only_the_chain_arm(self):
        rom = build_image(last_image=False)
        result, arms = self.arms(rom)
        self.assert_only_arm_failed(arms, "image chain complete")
        self.assertFalse(result["authored"])

    def test_a_second_signature_breaks_only_the_uniqueness_arm(self):
        """Two candidates mean the pointer chose, and the choice needs stating.

        Planted ABOVE the real header on purpose. `signature_occurrences()` scans
        forward, so a hit planted below becomes hits[0] and breaks the
        pointer-selection arm as well -- see the companion test. Isolating the
        uniqueness arm requires the real header to stay first.
        """
        rom = bytearray(build_image())
        struct.pack_into("<I", rom, 0x0F00, dcb.DCB_SIGNATURE)
        result, arms = self.arms(bytes(rom))
        self.assert_only_arm_failed(arms, "exactly once")
        self.assertFalse(result["authored"])

    def test_a_signature_below_the_header_breaks_the_selection_arm_too(self):
        """The coupled case, asserted as a PAIR rather than pretended singular.

        This is a genuine coupling in the subject and not a defect in it: the
        pointer-selection arm compares against hits[0], and a candidate at a
        lower offset legitimately means the first signature in the image is NOT
        the one the pointer chose. Both arms are true statements about that
        image, so the control names both. Writing it as a single-arm control
        (which an earlier draft did) reddens against correct code.
        """
        rom = bytearray(build_image())
        struct.pack_into("<I", rom, 0x0400, dcb.DCB_SIGNATURE)
        result, arms = self.arms(bytes(rom))
        failed = [label for label, ok in arms.items() if not ok]
        self.assertEqual(2, len(failed), "expected the pair, got %r" % failed)
        self.assertTrue(any("exactly once" in label for label in failed))
        self.assertTrue(any("selected that one header" in label
                            for label in failed))
        self.assertFalse(result["authored"])

    def test_emptying_the_siblings_breaks_only_the_sibling_arm(self):
        """The load-bearing arm: a decode landing on the wrong table reads
        garbage counts out of every sibling, while a board that genuinely wires
        no display outputs still has populated GPIO and I2C tables.
        """
        rom = build_image(ccb_count=0, gpio_count=0, i2c_count=0)
        result, arms = self.arms(rom)
        self.assert_only_arm_failed(arms, "sibling sub-tables")
        self.assertFalse(result["authored"])

    def test_one_populated_sibling_is_not_enough(self):
        """The threshold is two; a single table could be coincidence."""
        rom = build_image(ccb_count=0, gpio_count=0, i2c_count=32)
        result, arms = self.arms(rom)
        self.assert_only_arm_failed(arms, "sibling sub-tables")
        self.assertFalse(result["authored"])

    def test_two_populated_siblings_are_enough(self):
        """The positive side of the same threshold, so it is not a one-way test."""
        rom = build_image(ccb_count=0, gpio_count=36, i2c_count=32)
        result, _ = self.arms(rom)
        self.assertTrue(result["authored"])

    def test_unknown_header_version_breaks_only_the_version_arm(self):
        rom = build_image(version=0x99)
        result, arms = self.arms(rom)
        self.assert_only_arm_failed(arms, "known DCB version")
        self.assertFalse(result["authored"])

    def test_wrong_header_size_breaks_only_the_layout_arm(self):
        rom = build_image(header_size=0x1B)
        result, arms = self.arms(rom)
        self.assert_only_arm_failed(arms, "header_size matches")
        self.assertFalse(result["authored"])

    def test_populated_list_reports_the_counts_it_relied_on(self):
        """A verdict that cannot show its own inputs is unauditable."""
        rom = build_image()
        result, _ = self.arms(rom)
        counts = {row["name"]: row["entry_count"] for row in result["populated"]}
        self.assertEqual(15, counts["ccb_block_ptr"])
        self.assertEqual(36, counts["gpio_table"])
        self.assertEqual(32, counts["i2c_devices"])


# ---------------------------------------------------------------------------
# Sub-table reading and the report surface
# ---------------------------------------------------------------------------

class SubtableTests(unittest.TestCase):

    def test_absent_pointer_is_reported_absent_not_readable(self):
        rom = build_image()
        header = dcb.parse_header(rom, dcb.find_dcb(rom))
        rows = {row["name"]: row for row in dcb.subtable_counts(rom, header)}
        self.assertFalse(rows["spread_spectrum"]["readable"])
        self.assertEqual("absent", rows["spread_spectrum"]["note"])

    def test_connector_table_absent_returns_none_rather_than_raising(self):
        """The display paths are the finding; a secondary table must not
        destroy the primary one by raising."""
        rom = bytearray(build_image())
        struct.pack_into("<H", rom, DCB_BASE + 0x14, 0)
        header = dcb.parse_header(bytes(rom), DCB_BASE)
        self.assertIsNone(dcb.parse_connectors(bytes(rom), header))

    def test_every_connector_type_name_is_distinct(self):
        names = list(dcb.CONNECTOR_TYPE_NAMES.values())
        self.assertEqual(len(names), len(set(names)))

    # -- a POPULATED connector table ------------------------------------
    #
    # Every connector assertion above this line drives a table with ZERO rows,
    # and the real GA107 table has zero rows too, so parse_connectors()'s row
    # loop has never executed in this tree. Three distinct defects survive that:
    # a loop returning row 0's decode for every index, a stride that advances by
    # the wrong number of bytes, and a name lookup that drops what it cannot
    # name. All three need per-row CONTENT to separate, which is why the fixture
    # gained conn_types -- build_subtable() fills every row with the same byte,
    # and a uniform fill cannot tell a correct loop from one stuck on row 0.

    def populated(self, types):
        """Build an image whose connector table carries one row per type."""
        rom = build_image(conn_types=types)
        header = dcb.parse_header(rom, dcb.find_dcb(rom))
        return rom, dcb.parse_connectors(rom, header)

    @staticmethod
    def known_types(count):
        """Distinct type values the tool ITSELF names.

        Taken from the tool's own table rather than spelled here: a hand-written
        0x01 that the tool stops naming would turn this into an assertion about
        the string 'unknown', which is the opposite of what it claims to test.
        """
        return sorted(dcb.CONNECTOR_TYPE_NAMES)[:count]

    @staticmethod
    def unknown_type():
        """The lowest byte value the tool's table does NOT name."""
        for value in range(0x100):
            if value not in dcb.CONNECTOR_TYPE_NAMES:
                return value
        raise AssertionError("the name table covers every byte value")

    def test_a_populated_table_decodes_one_row_per_declared_entry(self):
        types = self.known_types(4)
        _, conns = self.populated(types)
        self.assertIsNotNone(conns)
        self.assertEqual(len(types), conns["entry_count"])
        self.assertEqual(len(types), len(conns["rows"]))
        self.assertEqual(CONN_ENTRY_SIZE, conns["entry_size"])

    def test_each_row_carries_its_own_type_rather_than_row_zeros(self):
        """The discriminating one.

        A loop that decodes row 0 and repeats it satisfies every count-shaped
        assertion in this file. Distinct planted types are the only thing that
        separates it from a correct walk.
        """
        types = self.known_types(4)
        _, conns = self.populated(types)
        self.assertEqual(types, [row["type"] for row in conns["rows"]])
        self.assertEqual(list(range(len(types))),
                         [row["index"] for row in conns["rows"]])

    def test_row_offsets_advance_by_the_entry_size_from_the_header(self):
        """Pins the stride. A walk that advanced by the HEADER size, or by one
        byte, still yields the right row count and would pass everything else.
        """
        types = self.known_types(4)
        _, conns = self.populated(types)
        base = conns["pointer"] + CONN_HEADER_SIZE
        self.assertEqual(
            [base + index * CONN_ENTRY_SIZE for index in range(len(types))],
            [row["offset"] for row in conns["rows"]])

    def test_each_named_type_is_rendered_with_the_tools_own_name(self):
        types = self.known_types(4)
        _, conns = self.populated(types)
        for row in conns["rows"]:
            self.assertEqual(dcb.CONNECTOR_TYPE_NAMES[row["type"]],
                             row["type_name"])

    def test_an_unnamed_type_is_reported_unknown_rather_than_dropped(self):
        """A row the tool cannot name must still be a row.

        Silently skipping it would UNDERCOUNT this board's connectors, which on
        a table whose whole job is corroborating an absence is the one direction
        that reads as confirmation.
        """
        stranger = self.unknown_type()
        types = self.known_types(2) + [stranger]
        _, conns = self.populated(types)
        self.assertEqual(3, len(conns["rows"]))
        self.assertEqual(stranger, conns["rows"][2]["type"])
        self.assertIn("unknown", conns["rows"][2]["type_name"])
        self.assertIn("%02x" % stranger, conns["rows"][2]["type_name"])

    def test_the_row_raw_word_keeps_the_bytes_above_the_type(self):
        """type is the low byte of a wider word; raw must not be truncated to
        it, or the report's raw column silently becomes a copy of the type."""
        types = self.known_types(2)
        _, conns = self.populated(types)
        for row in conns["rows"]:
            self.assertEqual(row["type"], row["raw"] & 0xFF)
            self.assertGreater(row["raw"], 0xFF)

    def test_the_report_renders_every_populated_connector_row(self):
        types = self.known_types(3)
        rom = build_image(conn_types=types)
        text = "\n".join(dcb.report(rom, "synthetic"))
        self.assertIn("connector table @0x%04x (%d entries, %d bytes each)"
                      % (SUB_CONN, len(types), CONN_ENTRY_SIZE), text)
        for ctype in types:
            self.assertIn(dcb.CONNECTOR_TYPE_NAMES[ctype], text)

    def test_a_connector_table_running_past_the_end_is_refused(self):
        """Containment is checked over the WHOLE table, not just its header."""
        rom = bytearray(build_image())
        ptr = len(rom) - 8
        struct.pack_into("<H", rom, DCB_BASE + 0x14, ptr)
        rom[ptr:ptr + 4] = bytes([0x41, CONN_HEADER_SIZE, 4, CONN_ENTRY_SIZE])
        header = dcb.parse_header(bytes(rom), DCB_BASE)
        self.assertIsNone(dcb.parse_connectors(bytes(rom), header))

    def test_the_same_table_one_row_shorter_at_the_same_offset_is_read(self):
        """Partner to the refusal above.

        Without it, a parse_connectors() that returned None for every relocated
        table would satisfy the refusal test while reading nothing at all.
        """
        rom = bytearray(build_image())
        ptr = len(rom) - 32
        struct.pack_into("<H", rom, DCB_BASE + 0x14, ptr)
        rom[ptr:ptr + 4] = bytes([0x41, CONN_HEADER_SIZE, 4, CONN_ENTRY_SIZE])
        header = dcb.parse_header(bytes(rom), DCB_BASE)
        conns = dcb.parse_connectors(bytes(rom), header)
        self.assertIsNotNone(conns)
        self.assertEqual(4, len(conns["rows"]))

    def test_a_zero_entry_size_is_refused_rather_than_looping(self):
        """An entry_size of zero makes every row land on the same offset, which
        is the decoder-stuck-on-row-zero shape arriving from the DATA side."""
        rom = bytearray(build_image(conn_types=self.known_types(3)))
        rom[SUB_CONN + 3] = 0
        header = dcb.parse_header(bytes(rom), DCB_BASE)
        self.assertIsNone(dcb.parse_connectors(bytes(rom), header))


class ReportTests(unittest.TestCase):

    def test_report_states_the_absence_verdict_when_there_are_no_paths(self):
        lines = dcb.report(build_image(), "synthetic")
        text = "\n".join(lines)
        self.assertIn("NO DISPLAY PATHS", text)
        self.assertIn("AUTHORED", text)

    def test_report_omits_the_absence_block_when_a_path_exists(self):
        rom = build_image(entries=[path_entry(TMDS, connector=1)])
        text = "\n".join(dcb.report(rom, "synthetic"))
        self.assertNotIn("NO DISPLAY PATHS", text)
        self.assertIn("external-capable", text)

    def test_report_marks_an_unproven_absence_rather_than_authored(self):
        """The direction that matters: a broken arm must not read as AUTHORED."""
        text = "\n".join(dcb.report(build_image(last_image=False), "synthetic"))
        self.assertIn("UNPROVEN", text)
        self.assertNotIn("AUTHORED", text)

    def test_report_always_carries_the_not_a_measurement_caveat(self):
        for rom in (build_image(), build_image(entries=[path_entry(TMDS)])):
            text = "\n".join(dcb.report(rom, "synthetic"))
            self.assertIn("NOT A MEASUREMENT", text.upper())

    def test_report_is_pure_ascii(self):
        """The report is a committed artifact; a stray non-ASCII byte makes it
        console-encoding dependent and the regeneration diff unstable."""
        text = "\n".join(dcb.report(build_image(), "synthetic"))
        text.encode("ascii")


# ---------------------------------------------------------------------------
# The real firmware. Skips loudly rather than passing vacuously.
# ---------------------------------------------------------------------------

@unittest.skipUnless(os.path.isfile(REAL_ROM),
                     "reference/ga107-firmware/vbios-ga107.rom is absent; "
                     "the real-firmware assertions are SKIPPED, not passing")
class RealGa107RomTests(unittest.TestCase):
    """Drives the actual 65024-byte GA107 image.

    These are the assertions the finding rests on. Every value here was measured
    on 2026-08-19 by running the tool, not predicted.
    """

    @classmethod
    def setUpClass(cls):
        with open(REAL_ROM, "rb") as handle:
            cls.rom = handle.read()
        cls.base = dcb.find_dcb(cls.rom)
        cls.header = dcb.parse_header(cls.rom, cls.base)
        cls.entries = dcb.parse_entries(cls.rom, cls.header)

    def test_image_is_the_size_the_report_records(self):
        self.assertEqual(65024, len(self.rom))

    def test_dcb_is_found_at_the_measured_offset(self):
        self.assertEqual(0x58BA, self.base)

    def test_header_is_version_41_with_the_measured_pointers(self):
        self.assertEqual(0x41, self.header["version"])
        self.assertEqual(0x23, self.header["header_size"])
        self.assertEqual(8, self.header["entry_size"])
        self.assertEqual(0x595D, self.header["ccb_block_ptr"])
        self.assertEqual(0x3FB2, self.header["gpio_table"])
        self.assertEqual(0x599F, self.header["i2c_devices"])
        self.assertEqual(0x5A24, self.header["connectors"])

    def test_the_signature_occurs_exactly_once_in_the_whole_image(self):
        """This is what makes the pointer at 0x36 trustworthy rather than one
        candidate among several."""
        self.assertEqual([0x58BA], dcb.signature_occurrences(self.rom))

    def test_the_image_chain_is_complete_and_terminated(self):
        """127 x 512 = 65024, exactly the file size, last-image bit set. Without
        this, 'no display paths' and 'the dump stops before them' are the same
        reading."""
        chain = dcb.check_image_chain(self.rom)
        self.assertTrue(chain["terminated"])
        self.assertFalse(chain["truncated"])
        self.assertTrue(chain["complete"])
        self.assertEqual(1, len(chain["images"]))
        self.assertEqual(65024, chain["images"][0]["length"])
        self.assertEqual(0x10DE, chain["images"][0]["vendor"])
        self.assertEqual(0x25A2, chain["images"][0]["device"])

    def test_declares_zero_display_paths(self):
        """THE FINDING. The board's firmware describes no path from this chip to
        any connector."""
        self.assertEqual(0, self.header["entry_count"])
        self.assertEqual([], self.entries)

    def test_no_external_capable_path_exists(self):
        """Stated over the type set rather than over the count, so an entry
        added later is classified rather than ignored."""
        external = [e for e in self.entries
                    if e["type"] in dcb.EXTERNAL_TYPES]
        self.assertEqual([], external)

    def test_the_sibling_tables_are_populated(self):
        """The separating evidence. A decode landing on the wrong offset reads
        garbage out of these too; instead all three carry sane counts, so the
        zero above is authored rather than a misread."""
        rows = {row["name"]: row
                for row in dcb.subtable_counts(self.rom, self.header)}
        self.assertEqual(15, rows["ccb_block_ptr"]["entry_count"])
        self.assertEqual(36, rows["gpio_table"]["entry_count"])
        self.assertEqual(32, rows["i2c_devices"]["entry_count"])
        self.assertEqual(0, rows["connectors"]["entry_count"])
        for name in ("ccb_block_ptr", "gpio_table", "i2c_devices"):
            self.assertTrue(rows[name]["well_formed"])

    def test_the_absence_is_corroborated_on_every_arm(self):
        result = dcb.corroborate_absence(self.rom, self.header, self.entries)
        self.assertIsNotNone(result)
        failed = [label for label, ok in result["arms"] if not ok]
        self.assertEqual([], failed)
        self.assertTrue(result["authored"])


# ---------------------------------------------------------------------------
# The committed artifact
# ---------------------------------------------------------------------------

@unittest.skipUnless(os.path.isfile(REAL_ROM) and os.path.isfile(REPORT),
                     "vbios-ga107.rom or dcb-report.txt is absent; the "
                     "regeneration assertions are SKIPPED, not passing")
class CommittedReportTests(unittest.TestCase):
    """`dcb-report.txt` is committed output, in `vbios-report.txt`'s idiom.

    A committed artifact that nothing regenerates is prose: it ages silently
    while the tool that produced it moves underneath it, and a reader has no way
    to tell a current report from one three refactors stale. So the artifact is
    pinned two ways -- byte-for-byte against a fresh run, AND on the meaning it
    carries, because a regeneration check alone passes a report whose verdict
    flipped as long as somebody re-ran the tool.
    """

    @classmethod
    def setUpClass(cls):
        with open(REAL_ROM, "rb") as handle:
            cls.rom = handle.read()
        with open(REPORT, "r", encoding="ascii", newline="") as handle:
            cls.committed = handle.read()

    def test_the_report_regenerates_from_the_rom(self):
        """Compared as LINES, because the committed file is CRLF (Windows text
        mode) while report() returns bare strings. Comparing raw bytes would
        redden on the newline convention rather than on content.
        """
        path = os.path.join("reference", "ga107-firmware", "vbios-ga107.rom")
        produced = "\n".join(dcb.report(self.rom, path))
        self.assertEqual(produced.splitlines(), self.committed.splitlines(),
                         "dcb-report.txt is stale; regenerate with\n"
                         "  python tools/parse-dcb.py > dcb-report.txt")

    def test_the_committed_report_is_pure_ascii(self):
        self.committed.encode("ascii")

    def test_the_committed_report_states_the_finding(self):
        """A regeneration check passes any report somebody re-ran. This is the
        arm that notices the verdict CHANGING.
        """
        self.assertIn("NO DISPLAY PATHS", self.committed)
        self.assertIn("AUTHORED", self.committed)
        self.assertNotIn("UNPROVEN", self.committed)

    def test_the_committed_report_names_its_subject(self):
        """Without the filename and size, the report is unattributable to the
        image it was taken from."""
        self.assertIn("vbios-ga107.rom", self.committed)
        self.assertIn("65024", self.committed)


if __name__ == "__main__":
    unittest.main(verbosity=2)
