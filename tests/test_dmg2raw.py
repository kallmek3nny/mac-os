#!/usr/bin/env python3
"""
test_dmg2raw.py -- regression tests for the mish table offsets in
``tools/dmg2raw.py``.

Why this exists
---------------
The mish header was parsed with the wrong offsets once already. The failure was
*silent*: ``start_sector`` and ``sector_count`` live at offsets 8 and 16, and
those were correct, so the census output looked plausible. What was wrong sat
further in -- the chunk count at 200 and the chunk array base at 204. A wrong
count either walks off the end of the blob (loud) or stops early (silent, and
the converted image is short by however many chunks were skipped).

So these tests do not test "does it run". They pin the exact byte offsets of
every field the parser reads, by building mish blobs whose fields all hold
distinct sentinel values. If any offset drifts, a field picks up a neighbour's
sentinel and the assertion names which one.

Run:
    python tests/test_dmg2raw.py          # plain, no dependencies
    python -m unittest discover -s tests  # or via unittest
"""

import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "tools"))

import dmg2raw  # noqa: E402


# ---------------------------------------------------------------------------
# mish blob construction
#
# The layout under test, from the dmg2raw.py header comment:
#
#   off  size  field
#   0    4     'mish'
#   4    4     version
#   8    8     start sector
#   16   8     sector count
#   24   8     data start (absolute file offset the chunk in_offs are relative to)
#   ...
#   200  4     chunk count
#   204  ...   chunk array, 40 bytes each
#
# Chunk entry:
#   0  4  type     4  4  comment    8  8  out sector offset
#   16 8  out sector count          24 8  in offset (relative to data start)
#   32 8  in length
# ---------------------------------------------------------------------------

MISH_HEADER_LEN = 204
CHUNK_LEN = 40
CHUNK_COUNT_OFF = 200


def make_chunk(ctype, out_off, out_cnt, in_off, in_len, comment=0):
    """One 40-byte chunk entry, big-endian."""
    return struct.pack(">IIQQQQ", ctype, comment, out_off, out_cnt,
                       in_off, in_len)


def make_mish(start_sector, sector_count, data_start, chunks,
              version=1, filler=0xAA):
    """A mish table with every unused header byte set to `filler`.

    The filler matters: it is a non-zero, non-sentinel value, so a parser
    reading a field at the wrong offset picks up 0xAAAA... rather than a
    plausible-looking zero. Wrong offsets fail loudly instead of decoding as
    empty.
    """
    head = bytearray([filler]) * MISH_HEADER_LEN
    head[0:4] = b"mish"
    struct.pack_into(">I", head, 4, version)
    struct.pack_into(">Q", head, 8, start_sector)
    struct.pack_into(">Q", head, 16, sector_count)
    struct.pack_into(">Q", head, 24, data_start)
    struct.pack_into(">I", head, CHUNK_COUNT_OFF, len(chunks))
    return bytes(head) + b"".join(chunks)


class MishHeaderOffsets(unittest.TestCase):
    """Each header field is read from its own offset, not a neighbour's."""

    def test_header_fields_are_distinct_sentinels(self):
        blob = make_mish(start_sector=0x1111111111111111,
                         sector_count=0x2222222222222222,
                         data_start=0x3333333333333333,
                         chunks=[])
        start, count, chunks = dmg2raw.parse_mish(blob)

        self.assertEqual(start, 0x1111111111111111,
                         "start_sector must come from offset 8")
        self.assertEqual(count, 0x2222222222222222,
                         "sector_count must come from offset 16")
        self.assertEqual(chunks, [])

    def test_chunk_count_is_at_offset_200(self):
        blob = make_mish(1, 2, 3, [make_chunk(dmg2raw.T_ZERO, 0, 8, 0, 0)] * 3)
        _start, _count, chunks = dmg2raw.parse_mish(blob)
        self.assertEqual(len(chunks), 3,
                         "chunk count must be the u32 at offset 200")

        # Prove offset 200 specifically: poison every other plausible slot
        # near it and confirm the parse is unaffected.
        poisoned = bytearray(blob)
        for off in (188, 192, 196):
            struct.pack_into(">I", poisoned, off, 0xDEADBEEF)
        _s, _c, chunks2 = dmg2raw.parse_mish(bytes(poisoned))
        self.assertEqual(len(chunks2), 3,
                         "chunk count read from the wrong u32 near offset 200")

    def test_chunk_array_starts_at_204(self):
        # A single chunk with every field a distinct sentinel. If the array
        # base is off by even 4 bytes, all six fields shift and none match.
        chunk = make_chunk(ctype=dmg2raw.T_RAW,
                           out_off=0x0101010101010101,
                           out_cnt=0x0202020202020202,
                           in_off=0x0303030303030303,
                           in_len=0x0404040404040404,
                           comment=0x05050505)
        blob = make_mish(0, 0, data_start=0, chunks=[chunk])
        _start, _count, chunks = dmg2raw.parse_mish(blob)

        ctype, out_off, out_cnt, in_off, in_len = chunks[0]
        self.assertEqual(ctype, dmg2raw.T_RAW, "chunk type at entry offset 0")
        self.assertEqual(out_off, 0x0101010101010101,
                         "out sector offset at entry offset 8")
        self.assertEqual(out_cnt, 0x0202020202020202,
                         "out sector count at entry offset 16")
        self.assertEqual(in_len, 0x0404040404040404,
                         "in length at entry offset 32")

    def test_chunk_stride_is_40_bytes(self):
        # Three chunks whose out_off is its own index. A wrong stride reads
        # overlapping or skipped entries and the indices come back scrambled.
        chunks = [make_chunk(dmg2raw.T_ZERO, i, 1, 0, 0) for i in range(3)]
        blob = make_mish(0, 3, 0, chunks)
        _s, _c, parsed = dmg2raw.parse_mish(blob)
        self.assertEqual([c[1] for c in parsed], [0, 1, 2],
                         "chunk stride must be exactly 40 bytes")


class MishInputOffsetIsAbsolute(unittest.TestCase):
    """in_off is relative to data_start; the parser must fold that in."""

    def test_in_off_is_data_start_plus_relative(self):
        chunk = make_chunk(dmg2raw.T_RAW, 0, 1, in_off=0x100, in_len=0x200)
        blob = make_mish(0, 1, data_start=0x40000, chunks=[chunk])
        _s, _c, parsed = dmg2raw.parse_mish(blob)
        self.assertEqual(parsed[0][3], 0x40000 + 0x100,
                         "in_off must be absolute: data_start + relative")

    def test_zero_data_start_leaves_in_off_unchanged(self):
        chunk = make_chunk(dmg2raw.T_RAW, 0, 1, in_off=0x999, in_len=1)
        blob = make_mish(0, 1, data_start=0, chunks=[chunk])
        _s, _c, parsed = dmg2raw.parse_mish(blob)
        self.assertEqual(parsed[0][3], 0x999)


class MishTruncationGuard(unittest.TestCase):
    """The 204 + count*40 bounds check is the loud failure for bad offsets."""

    def test_count_larger_than_blob_is_rejected(self):
        blob = bytearray(make_mish(0, 0, 0, [make_chunk(0, 0, 0, 0, 0)]))
        struct.pack_into(">I", blob, CHUNK_COUNT_OFF, 1000)
        with self.assertRaises(SystemExit) as cm:
            dmg2raw.parse_mish(bytes(blob))
        self.assertIn("header offsets are wrong", str(cm.exception))

    def test_exactly_sized_blob_is_accepted(self):
        # The boundary: expected == len(blob) must pass, not trip the guard.
        chunks = [make_chunk(dmg2raw.T_ZERO, 0, 1, 0, 0) for _ in range(4)]
        blob = make_mish(0, 4, 0, chunks)
        self.assertEqual(len(blob), MISH_HEADER_LEN + 4 * CHUNK_LEN)
        _s, _c, parsed = dmg2raw.parse_mish(blob)
        self.assertEqual(len(parsed), 4)

    def test_one_byte_short_is_rejected(self):
        chunks = [make_chunk(dmg2raw.T_ZERO, 0, 1, 0, 0) for _ in range(4)]
        blob = make_mish(0, 4, 0, chunks)[:-1]
        with self.assertRaises(SystemExit):
            dmg2raw.parse_mish(blob)

    def test_non_mish_magic_is_rejected(self):
        blob = bytearray(make_mish(0, 0, 0, []))
        blob[0:4] = b"XXXX"
        with self.assertRaises(SystemExit) as cm:
            dmg2raw.parse_mish(bytes(blob))
        self.assertIn("not a mish table", str(cm.exception))


class ConvertRoundTrip(unittest.TestCase):
    """End-to-end: a synthetic dmg converts to exactly the expected bytes.

    This is the test that would have caught the original bug at the level the
    user sees it -- a short or misplaced output image, rather than a plausible
    census listing.
    """

    def _write_dmg(self, path, runs):
        """Build a minimal UDIF file: payload data, then a koly-referenced plist.

        `runs` is a list of (name, start_sector, sector_count, chunk_specs)
        where each chunk spec is (ctype, out_off, out_cnt, payload_bytes).
        """
        import base64

        body = bytearray()
        blkx_xml = []

        for name, start, count, specs in runs:
            chunks = []
            for ctype, out_off, out_cnt, payload in specs:
                in_off = len(body)
                body += payload
                chunks.append(make_chunk(ctype, out_off, out_cnt,
                                         in_off, len(payload)))
            # data_start is 0: in_off values above are already absolute.
            mish = make_mish(start, count, 0, chunks)
            blkx_xml.append(
                "<dict><key>Name</key><string>%s</string>"
                "<key>Data</key><data>%s</data></dict>"
                % (name, base64.b64encode(mish).decode("ascii")))

        plist = (
            '<?xml version="1.0" encoding="UTF-8"?>'
            '<plist version="1.0"><dict><key>resource-fork</key><dict>'
            '<key>blkx</key><array>%s</array>'
            '</dict></dict></plist>' % "".join(blkx_xml)
        ).encode("utf-8")

        plist_off = len(body)
        body += plist

        # koly trailer: 512 bytes, magic at 0, plist offset/length at 216/224.
        koly = bytearray(512)
        koly[0:4] = b"koly"
        struct.pack_into(">Q", koly, 216, plist_off)
        struct.pack_into(">Q", koly, 224, len(plist))
        body += koly

        with open(path, "wb") as fh:
            fh.write(body)

    def test_raw_zlib_and_zero_chunks_land_at_correct_offsets(self):
        sec = dmg2raw.SECTOR
        raw_payload = b"R" * sec
        zlib_plain = b"Z" * (2 * sec)
        zlib_payload = zlib.compress(zlib_plain)

        runs = [
            # Run starts at sector 4. Chunk out_offs are relative to that.
            ("synthetic", 4, 5, [
                (dmg2raw.T_RAW, 0, 1, raw_payload),      # -> sectors 4
                (dmg2raw.T_ZERO, 1, 2, b""),             # -> sectors 5,6
                (dmg2raw.T_ZLIB, 3, 2, zlib_payload),    # -> sectors 7,8
                (dmg2raw.T_TERM, 5, 0, b""),
            ]),
        ]

        tmp = tempfile.mkdtemp()
        dmg = os.path.join(tmp, "synthetic.dmg")
        out = os.path.join(tmp, "synthetic.img")
        try:
            self._write_dmg(dmg, runs)
            rc = dmg2raw.convert(dmg, out)
            self.assertEqual(rc, 0)

            with open(out, "rb") as fh:
                image = fh.read()

            self.assertEqual(len(image), (4 + 5) * sec,
                             "image length must be (start + count) sectors")
            self.assertEqual(image[0:4 * sec], b"\x00" * (4 * sec),
                             "sectors before the run start must be zero")
            self.assertEqual(image[4 * sec:5 * sec], raw_payload,
                             "raw chunk must land at (start + out_off)")
            self.assertEqual(image[5 * sec:7 * sec], b"\x00" * (2 * sec),
                             "zero-fill chunk must read back as zeros")
            self.assertEqual(image[7 * sec:9 * sec], zlib_plain,
                             "zlib chunk must decompress in place")
        finally:
            for p in (dmg, out):
                if os.path.exists(p):
                    os.remove(p)
            os.rmdir(tmp)

    def test_decoded_size_mismatch_is_caught(self):
        sec = dmg2raw.SECTOR
        # Claims 2 sectors of output but the payload only holds 1.
        runs = [("bad", 0, 2, [(dmg2raw.T_RAW, 0, 2, b"R" * sec)])]

        tmp = tempfile.mkdtemp()
        dmg = os.path.join(tmp, "bad.dmg")
        out = os.path.join(tmp, "bad.img")
        try:
            self._write_dmg(dmg, runs)
            with self.assertRaises(SystemExit) as cm:
                dmg2raw.convert(dmg, out)
            self.assertIn("decoded to", str(cm.exception))
        finally:
            for p in (dmg, out):
                if os.path.exists(p):
                    os.remove(p)
            os.rmdir(tmp)


if __name__ == "__main__":
    unittest.main(verbosity=2)
