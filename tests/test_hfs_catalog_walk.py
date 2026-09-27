#!/usr/bin/env python3
"""
test_hfs_catalog_walk.py -- regression tests for ``tools/_hfs-catalog-walk.py``.

Why this exists
---------------
This walker is an *evidence producer*. Its output is what decides whether the
``sata0:3`` disk presentation of ``darwin.iso`` will hand the macOS guest a
mountable VMware Tools volume (see ``../VM-BRINGUP.md`` Failure 5). It never
crashes on a wrong offset -- it prints a plausible, confidently-formatted,
wrong number.

It already did exactly that. The first revision read ``BTHeaderRec`` relative to
the B-tree *node* base without accounting for the 14-byte ``BTNodeDescriptor``
that precedes it, so ``totalNodes`` was read at node+40. Node+40 is
``freeNodes``, and on a full read-only installer image ``freeNodes`` is
legitimately ``0``. The run printed::

    catalog totalNodes : 0

which reads as "the image has no catalog" and would have discredited a correct
fix. Nothing in the output flagged it, because ``0`` is a number a real image
could produce. That is the same failure class as the split ARCHITECTURE field in
``test_decode.cpp`` and the mish chunk offsets in ``test_dmg2raw.py``: right on
the field you happened to check, wrong on the one that mattered.

So these tests pin the offsets with **distinct sentinel values**. Every field
gets a value no other field has, which is the whole point -- a test image whose
``totalNodes`` and ``freeNodes`` were both 12 would pass with the old broken
reader. ``test_total_nodes_is_not_read_from_the_free_nodes_offset`` is the
negative control: it asserts the specific wrong answer does not come back.

What is and is not covered
--------------------------
Covered: ``partitions`` (APM map walking, including the self-describing entry
count and rejection of non-APM images), ``volume_header`` (HFS+ field offsets
and the catalog fork's extent list), ``btree_header`` (the offsets above),
``node_offset`` (extent-list arithmetic, including a *fragmented* fork, and
out-of-range returning None), and ``leaf_names`` (record extraction, UTF-16BE
names, and thread-record exclusion).

Not covered: ``main``'s printing. A synthetic image is built here rather than
reading ``darwin.iso``, so this suite runs on a bare checkout with no VM, no
unlocker, and no Windows.

Run with::

    tests\\run-python-tests.bat
    python -m unittest discover -s tests
"""

import importlib.util
import io
import os
import struct
import unittest

_TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                     "tools", "_hfs-catalog-walk.py")
_spec = importlib.util.spec_from_file_location("hfs_catalog_walk", _TOOL)
hfs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hfs)


# Sentinels. Every value is distinct and none is a plausible neighbour of
# another, so a field read from the wrong offset produces an obviously wrong
# number instead of a believable one. This is deliberate: the defect being
# pinned was invisible precisely because 0 was believable.
SENTINEL = {
    "tree_depth": 0x0A11,
    "root_node": 0x0B22,
    "leaf_records": 0x0C33,
    "first_leaf": 0x0D44,
    "last_leaf": 0x0E55,
    "node_size": 4096,
    "max_key_length": 0x0F66,
    "total_nodes": 12,          # what a 49152-byte fork of 4096-byte nodes has
    "free_nodes": 0,            # what the OLD, WRONG offset returned
}


def build_btree_header_node():
    """A 512-byte B-tree header node with every field at its documented place.

    Layout, relative to the NODE base -- the 14-byte BTNodeDescriptor first,
    then BTHeaderRec at +14:

        +0  fLink, +4 bLink, +8 kind(int8), +9 height, +10 numRecords, +12 res
        +14 treeDepth  +16 rootNode  +20 leafRecords  +24 firstLeafNode
        +28 lastLeafNode  +32 nodeSize  +34 maxKeyLength  +36 totalNodes
        +40 freeNodes
    """
    node = bytearray(512)
    struct.pack_into(">II", node, 0, 0, 0)                    # fLink, bLink
    struct.pack_into(">b", node, 8, hfs.KIND_HEADER)
    struct.pack_into(">B", node, 9, 0)                        # height
    struct.pack_into(">H", node, 10, 3)                       # numRecords
    struct.pack_into(">H", node, 14, SENTINEL["tree_depth"])
    struct.pack_into(">I", node, 16, SENTINEL["root_node"])
    struct.pack_into(">I", node, 20, SENTINEL["leaf_records"])
    struct.pack_into(">I", node, 24, SENTINEL["first_leaf"])
    struct.pack_into(">I", node, 28, SENTINEL["last_leaf"])
    struct.pack_into(">H", node, 32, SENTINEL["node_size"])
    struct.pack_into(">H", node, 34, SENTINEL["max_key_length"])
    struct.pack_into(">I", node, 36, SENTINEL["total_nodes"])
    struct.pack_into(">I", node, 40, SENTINEL["free_nodes"])
    return bytes(node)


def build_leaf_node(node_size, records):
    """Build one catalog leaf node.

    `records` is a list of (parent_id, record_type, name). Record data begins on
    the even boundary after the key, and the node's trailing offset array holds
    numRecords+1 big-endian uint16 offsets, in order.
    """
    node = bytearray(node_size)
    struct.pack_into(">b", node, 8, hfs.KIND_LEAF)
    struct.pack_into(">H", node, 10, len(records))

    offsets = []
    cursor = 14
    for parent_id, record_type, name in records:
        offsets.append(cursor)
        encoded = name.encode("utf-16-be")
        # HFSPlusCatalogKey: keyLength counts the bytes AFTER itself, i.e.
        # parentID (4) + the HFSUniStr255 (2 + 2*len).
        key_len = 4 + 2 + len(encoded)
        struct.pack_into(">H", node, cursor, key_len)
        struct.pack_into(">I", node, cursor + 2, parent_id)
        struct.pack_into(">H", node, cursor + 6, len(name))
        node[cursor + 8:cursor + 8 + len(encoded)] = encoded
        data_off = cursor + 2 + key_len
        data_off += data_off & 1
        struct.pack_into(">h", node, data_off, record_type)
        cursor = data_off + 88          # room for a catalog record body
        cursor += cursor & 1
    offsets.append(cursor)              # free-space offset

    for i, off in enumerate(offsets):
        struct.pack_into(">H", node, node_size - 2 * (i + 1), off)
    return bytes(node)


BLOCK_SIZE = 2048
PART_START_SECTOR = 64                  # 64 * 512 == 16 * 2048, as in darwin.iso
TOTAL_BLOCKS = 1637
CATALOG_LOGICAL_SIZE = 49152
CATALOG_BLOCKS = 24
CATALOG_START_BLOCK = 1

ROOT_RECORDS = [
    (2, 2, "Install VMware Tools.app"),
    (2, 2, "VMware Tools.pkg"),
    (2, 1, "a folder"),
    # A thread record. It repeats a name and must NOT be reported, or every
    # entry appears twice and the count silently doubles.
    (2, 3, "Install VMware Tools.app"),
    (99, 2, "not at the root"),
]


def build_image(extents=((CATALOG_START_BLOCK, CATALOG_BLOCKS),),
                apm_signature=hfs.APM_SIG, hfs_signature=hfs.HFSP_SIG,
                map_entries=2):
    """Assemble a minimal APM + HFS+ image in memory.

    Sizes mirror darwin.iso so the numbers in the tests are the numbers in
    VM-BRINGUP.md: a 63-sector Apple_partition_map, then an Apple_HFS partition
    of 6548 sectors == 1637 * 2048 bytes.
    """
    part_sectors = TOTAL_BLOCKS * BLOCK_SIZE // hfs.SECTOR
    total_sectors = PART_START_SECTOR + part_sectors
    img = bytearray(total_sectors * hfs.SECTOR)

    struct.pack_into(">H", img, 0, apm_signature)

    def dpme(index, name, ptype, start, count):
        base = index * hfs.SECTOR
        struct.pack_into(">H", img, base, hfs.DPME_SIG)
        struct.pack_into(">III", img, base + 4, map_entries, start, count)
        img[base + 16:base + 16 + len(name)] = name.encode("mac_roman")
        img[base + 48:base + 48 + len(ptype)] = ptype.encode("mac_roman")

    dpme(1, "Apple", "Apple_partition_map", 1, 63)
    dpme(2, "DiscRecording 9.0.3d5", "Apple_HFS",
         PART_START_SECTOR, part_sectors)

    vol_base = PART_START_SECTOR * hfs.SECTOR
    vh_off = vol_base + 1024
    img[vh_off:vh_off + 2] = hfs_signature
    struct.pack_into(">H", img, vh_off + 2, 4)                 # version
    struct.pack_into(">I", img, vh_off + 40, BLOCK_SIZE)
    struct.pack_into(">I", img, vh_off + 44, TOTAL_BLOCKS)
    struct.pack_into(">I", img, vh_off + 48, 0)               # freeBlocks

    fork = vh_off + 272
    struct.pack_into(">Q", img, fork, CATALOG_LOGICAL_SIZE)
    struct.pack_into(">I", img, fork + 8, BLOCK_SIZE)          # clumpSize
    struct.pack_into(">I", img, fork + 12, CATALOG_BLOCKS)
    for i, (start, count) in enumerate(extents):
        struct.pack_into(">II", img, fork + 16 + i * 8, start, count)

    # Node 0 of the catalog fork is the B-tree header node; node 1 is a leaf.
    first_extent_off = vol_base + extents[0][0] * BLOCK_SIZE
    header = build_btree_header_node()
    img[first_extent_off:first_extent_off + len(header)] = header
    leaf = build_leaf_node(SENTINEL["node_size"], ROOT_RECORDS)
    leaf_off = first_extent_off + SENTINEL["node_size"]
    img[leaf_off:leaf_off + len(leaf)] = leaf

    return bytes(img)


class BTreeHeaderOffsetTests(unittest.TestCase):
    """The 2026-08-07 defect, pinned field by field.

    BTHeaderRec starts at +14 inside the node, after the BTNodeDescriptor. Drop
    that and every field below nodeSize shifts by four bytes.
    """

    def setUp(self):
        self.fh = io.BytesIO(build_image())
        self.vh = hfs.volume_header(self.fh, PART_START_SECTOR)
        self.hdr_off = hfs.node_offset(self.vh, 0, 512)
        self.bt = hfs.btree_header(self.fh, self.hdr_off)

    def test_total_nodes_is_not_read_from_the_free_nodes_offset(self):
        """The negative control -- this is the entire reason the file exists.

        The broken reader took totalNodes from node+40, which is freeNodes.
        The sentinels make those two values differ (12 vs 0), so the wrong read
        is now detectable. If someone reintroduces +40, this fails and the two
        assertions below name exactly what happened.
        """
        self.assertEqual(self.bt["total_nodes"], SENTINEL["total_nodes"])
        self.assertNotEqual(self.bt["total_nodes"], SENTINEL["free_nodes"])

    def test_free_nodes_is_still_read_correctly_at_plus_40(self):
        """Fixing totalNodes must not move freeNodes off its real offset."""
        self.assertEqual(self.bt["free_nodes"], SENTINEL["free_nodes"])

    def test_total_nodes_matches_the_arithmetic_check(self):
        """logicalSize / nodeSize must equal totalNodes.

        This is the check that caught the bug by hand. A catalog fork of 49152
        bytes cut into 4096-byte nodes is 12 nodes; any parse that disagrees is
        a parser defect, not a finding about the image.
        """
        expected = self.vh["catalog_logical_size"] // self.bt["node_size"]
        self.assertEqual(expected, 12)
        self.assertEqual(self.bt["total_nodes"], expected)

    def test_node_size_offset(self):
        self.assertEqual(self.bt["node_size"], SENTINEL["node_size"])

    def test_max_key_length_sits_between_node_size_and_total_nodes(self):
        """maxKeyLength at +34 is the field a 4-byte drift would swallow."""
        self.assertEqual(self.bt["max_key_length"],
                         SENTINEL["max_key_length"])

    def test_every_remaining_header_field_reads_its_own_sentinel(self):
        for field in ("tree_depth", "root_node", "leaf_records",
                      "first_leaf", "last_leaf"):
            with self.subTest(field=field):
                self.assertEqual(self.bt[field], SENTINEL[field])

    def test_all_sentinels_are_distinct(self):
        """Guards the guard: duplicate sentinels would let a wrong offset pass."""
        values = list(SENTINEL.values())
        self.assertEqual(len(values), len(set(values)))

    def test_header_node_kind_is_identified(self):
        self.assertEqual(self.bt["kind"], hfs.KIND_HEADER)


class PartitionMapTests(unittest.TestCase):
    """A wrong partition start makes every later offset wrong, and the volume
    header check would then fail in a way that looks like a bad image."""

    def test_both_entries_are_found(self):
        parts = list(hfs.partitions(io.BytesIO(build_image())))
        self.assertEqual(len(parts), 2)

    def test_entry_fields_are_parsed(self):
        parts = list(hfs.partitions(io.BytesIO(build_image())))
        self.assertEqual(parts[0][:2], ("Apple", "Apple_partition_map"))
        name, ptype, start, count = parts[1]
        self.assertEqual(name, "DiscRecording 9.0.3d5")
        self.assertEqual(ptype, "Apple_HFS")
        self.assertEqual(start, PART_START_SECTOR)
        self.assertEqual(count, 6548)

    def test_partition_bytes_match_the_volume_size(self):
        """6548 * 512 == 1637 * 2048. Two units for the same span agreeing is
        the check that says the map and the volume describe one thing."""
        parts = list(hfs.partitions(io.BytesIO(build_image())))
        self.assertEqual(parts[1][3] * hfs.SECTOR,
                         TOTAL_BLOCKS * BLOCK_SIZE)

    def test_entry_count_comes_from_the_map_not_a_guess(self):
        """Entry 1 states how many entries exist. If the walker guessed, a map
        claiming 1 entry would still yield 2."""
        parts = list(hfs.partitions(io.BytesIO(build_image(map_entries=1))))
        self.assertEqual(len(parts), 1)

    def test_non_apm_image_is_rejected_not_misparsed(self):
        """An ISO9660 image must raise, not produce a plausible empty map --
        the whole finding is that darwin.iso is APM *despite* its extension."""
        with self.assertRaises(ValueError):
            list(hfs.partitions(io.BytesIO(build_image(apm_signature=0x4344))))


class VolumeHeaderTests(unittest.TestCase):
    """HFS+ volume header field offsets. blockSize at +40 and totalBlocks at
    +44 are adjacent uint32s -- swap them and a 1637-block volume of 2048-byte
    blocks reads as a 2048-block volume of 1637-byte blocks, which is not an
    obviously wrong pair of numbers."""

    def setUp(self):
        self.vh = hfs.volume_header(io.BytesIO(build_image()),
                                   PART_START_SECTOR)

    def test_signature_and_version(self):
        self.assertEqual(self.vh["signature"], hfs.HFSP_SIG)
        self.assertEqual(self.vh["version"], 4)

    def test_block_size_and_total_blocks_are_not_transposed(self):
        self.assertEqual(self.vh["block_size"], BLOCK_SIZE)
        self.assertEqual(self.vh["total_blocks"], TOTAL_BLOCKS)

    def test_free_blocks_is_read_from_its_own_offset(self):
        self.assertEqual(self.vh["free_blocks"], 0)

    def test_volume_base_is_the_partition_start(self):
        self.assertEqual(self.vh["volume_base"],
                         PART_START_SECTOR * hfs.SECTOR)

    def test_catalog_fork_fields(self):
        self.assertEqual(self.vh["catalog_logical_size"], CATALOG_LOGICAL_SIZE)
        self.assertEqual(self.vh["catalog_blocks"], CATALOG_BLOCKS)

    def test_only_used_extents_are_returned(self):
        """Eight extent descriptors always exist; the seven with count 0 are
        padding and must not appear as real extents."""
        self.assertEqual(self.vh["catalog_extents"],
                         [(CATALOG_START_BLOCK, CATALOG_BLOCKS)])

    def test_hfsx_signature_is_accepted(self):
        vh = hfs.volume_header(io.BytesIO(build_image(hfs_signature=b"HX")),
                              PART_START_SECTOR)
        self.assertEqual(vh["signature"], hfs.HFSX_SIG)

    def test_missing_volume_header_raises(self):
        with self.assertRaises(ValueError):
            hfs.volume_header(io.BytesIO(build_image(hfs_signature=b"ZZ")),
                              PART_START_SECTOR)


class NodeOffsetTests(unittest.TestCase):
    """Node index -> byte offset. This walks the extent list on purpose: an
    implementation that assumed one contiguous run would read a fragmented
    catalog from the wrong place and still return names."""

    def setUp(self):
        self.vh = hfs.volume_header(io.BytesIO(build_image()),
                                   PART_START_SECTOR)
        self.base = PART_START_SECTOR * hfs.SECTOR

    def test_node_zero_is_the_first_extent_start(self):
        self.assertEqual(hfs.node_offset(self.vh, 0, 4096),
                         self.base + CATALOG_START_BLOCK * BLOCK_SIZE)

    def test_node_one_is_one_node_size_further(self):
        self.assertEqual(hfs.node_offset(self.vh, 1, 4096),
                         self.base + CATALOG_START_BLOCK * BLOCK_SIZE + 4096)

    def test_last_node_is_inside_the_fork(self):
        off = hfs.node_offset(self.vh, 11, 4096)
        end = self.base + (CATALOG_START_BLOCK + CATALOG_BLOCKS) * BLOCK_SIZE
        self.assertIsNotNone(off)
        self.assertLess(off + 4096, end + 1)

    def test_past_the_end_returns_none_rather_than_a_wrong_offset(self):
        self.assertIsNone(hfs.node_offset(self.vh, 12, 4096))
        self.assertIsNone(hfs.node_offset(self.vh, 9999, 4096))

    def test_fragmented_fork_crosses_into_the_second_extent(self):
        """Two extents of 12 blocks each: node 12 must land at the start of the
        second extent, not 12 nodes past the first. Assuming contiguity here
        yields an offset inside unrelated data that still parses as *something*.
        """
        vh = hfs.volume_header(
            io.BytesIO(build_image(extents=((1, 12), (700, 12)))),
            PART_START_SECTOR)
        self.assertEqual(vh["catalog_extents"], [(1, 12), (700, 12)])
        # 12 blocks * 2048 == 24576 bytes == 6 nodes of 4096.
        self.assertEqual(hfs.node_offset(vh, 6, 4096),
                         self.base + 700 * BLOCK_SIZE)
        self.assertEqual(hfs.node_offset(vh, 7, 4096),
                         self.base + 700 * BLOCK_SIZE + 4096)
        self.assertIsNone(hfs.node_offset(vh, 12, 4096))


class LeafNameTests(unittest.TestCase):
    """Record extraction. A wrong record-type offset silently includes thread
    records, which doubles every name -- a listing that looks richer, not
    broken."""

    def setUp(self):
        self.fh = io.BytesIO(build_image())
        self.vh = hfs.volume_header(self.fh, PART_START_SECTOR)
        self.leaf_off = hfs.node_offset(self.vh, 1, SENTINEL["node_size"])
        self.names = hfs.leaf_names(self.fh, self.leaf_off,
                                    SENTINEL["node_size"])

    def test_files_and_folders_are_returned(self):
        got = {n for (_pid, _rt, n) in self.names}
        self.assertIn("Install VMware Tools.app", got)
        self.assertIn("VMware Tools.pkg", got)
        self.assertIn("a folder", got)

    def test_thread_records_are_excluded_so_names_are_not_doubled(self):
        appearances = [n for (_pid, _rt, n) in self.names
                       if n == "Install VMware Tools.app"]
        self.assertEqual(len(appearances), 1)
        self.assertNotIn(3, {rt for (_pid, rt, _n) in self.names})

    def test_record_count_excludes_the_thread_record(self):
        real = [r for r in ROOT_RECORDS if r[1] in (1, 2)]
        self.assertEqual(len(self.names), len(real))

    def test_parent_id_distinguishes_root_from_nested_entries(self):
        roots = sorted({n for (pid, _rt, n) in self.names if pid == 2})
        self.assertEqual(roots, ["Install VMware Tools.app", "VMware Tools.pkg",
                                 "a folder"])
        self.assertNotIn("not at the root", roots)

    def test_record_types_are_preserved(self):
        by_name = {n: rt for (_pid, rt, n) in self.names}
        self.assertEqual(by_name["VMware Tools.pkg"], 2)
        self.assertEqual(by_name["a folder"], 1)

    def test_a_header_node_yields_no_names(self):
        """Only leaves carry names. Reading a header or index node as a leaf
        would emit garbage strings that look like filenames."""
        hdr_off = hfs.node_offset(self.vh, 0, SENTINEL["node_size"])
        self.assertEqual(hfs.leaf_names(self.fh, hdr_off,
                                        SENTINEL["node_size"]), [])


class EndToEndTests(unittest.TestCase):
    """``main`` on a synthetic image, exercising the same path the darwin.iso
    run takes -- APM -> volume header -> B-tree header -> leaf scan."""

    def setUp(self):
        import tempfile
        self.tmp = tempfile.NamedTemporaryFile(suffix=".iso", delete=False)
        self.tmp.write(build_image())
        self.tmp.close()
        self.addCleanup(os.unlink, self.tmp.name)

    def test_exit_zero_when_names_are_recovered(self):
        self.assertEqual(hfs.main([self.tmp.name]), 0)

    def test_non_apm_image_exits_two(self):
        import tempfile
        tmp = tempfile.NamedTemporaryFile(suffix=".iso", delete=False)
        tmp.write(build_image(apm_signature=0x4344))
        tmp.close()
        self.addCleanup(os.unlink, tmp.name)
        self.assertEqual(hfs.main([tmp.name]), 2)

    def test_missing_file_exits_two(self):
        self.assertEqual(hfs.main([self.tmp.name + ".nope"]), 2)


if __name__ == "__main__":
    unittest.main()
