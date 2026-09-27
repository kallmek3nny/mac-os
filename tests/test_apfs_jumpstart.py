#!/usr/bin/env python3
"""
test_apfs_jumpstart.py -- offset and refusal tests for ``tools/apfs-jumpstart.py``.

Why this exists
---------------
The tool under test decides, from bytes on a disk image, whether the APFS
container carries Apple's own ``apfs.efi``. Every failure mode it can have is
silent:

* A wrong field offset reads a neighbouring field. ``nej_efi_file_len`` and
  ``nej_num_extents`` are adjacent uint32s, so swapping them yields a plausible
  extent count and a plausible length -- and an extraction of the wrong size
  that still starts with ``MZ`` because the first extent block is right.
* A wrong extent base reads ``nej_reserved[16]`` as extents. That is 128 bytes
  of zeros, which decodes as "extent at block 0, length 0" -- an empty file, not
  an exception.
* An extent list that covers fewer bytes than the declared length produces a
  short read that ``open(..., 'wb').write()`` accepts without complaint.

So the fixtures here write every field at a **bare literal** offset rather than
through the module's own constants. A fixture built from ``NEJ_FILE_LEN_OFF``
and read back through ``NEJ_FILE_LEN_OFF`` cannot fail no matter what that
constant is set to -- this project has already shipped one such suite (see
``tests/test_parse_vbios.py`` and the container/descriptor offsets it had to be
rewritten to pin).

The last test is the non-vacuity proof. It mutates each anchor constant in
memory and asserts the happy path stops agreeing. A green suite that stays green
under a corrupted constant is measuring nothing.
"""

import importlib.util
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(os.path.dirname(HERE), "tools")


def _load(name, filename):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(TOOLS, filename))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


js = _load("apfs_jumpstart_under_test", "apfs-jumpstart.py")

BLOCK = 4096


def make_pe(length=512, machine=0x8664, subsystem=11, lfanew=0x80):
    """A minimal but structurally real PE/COFF image.

    Offsets are spelled as literals on purpose: 0x3C is ``e_lfanew``, the PE
    signature is 4 bytes, the COFF header is 20 bytes, and ``Subsystem`` sits 68
    bytes into the optional header. Writing those through the tool's own names
    would make the check circular.
    """
    buf = bytearray(b"\x00" * max(length, lfanew + 24 + 70))
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, lfanew)
    buf[lfanew:lfanew + 4] = b"PE\x00\x00"
    struct.pack_into("<H", buf, lfanew + 4, machine)
    struct.pack_into("<H", buf, lfanew + 24 + 68, subsystem)
    return bytes(buf)


def make_jumpstart_block(extents, file_len, o_type=0x40000014,
                         magic=b"JSDR", version=1, num_extents=None,
                         extents_off=0xB0):
    """Build an ``nx_efi_jumpstart_t`` block from bare literal offsets."""
    blk = bytearray(b"\x00" * BLOCK)
    struct.pack_into("<I", blk, 0x18, o_type)          # obj_phys_t.o_type
    blk[0x20:0x24] = magic                             # nej_magic
    struct.pack_into("<I", blk, 0x24, version)         # nej_version
    struct.pack_into("<I", blk, 0x28, file_len)        # nej_efi_file_len
    count = len(extents) if num_extents is None else num_extents
    struct.pack_into("<I", blk, 0x2C, count)           # nej_num_extents
    for i, (start, blocks) in enumerate(extents):
        struct.pack_into("<QQ", blk, extents_off + i * 16, start, blocks)
    return bytes(blk)


def make_superblock(jumpstart_paddr):
    """Build the 4 KiB NXSB slice this tool reads, at literal offsets."""
    sb = bytearray(b"\x00" * BLOCK)
    sb[0x20:0x24] = b"NXSB"
    struct.pack_into("<I", sb, 0x24, BLOCK)
    struct.pack_into("<Q", sb, 0x4F8, jumpstart_paddr)  # nx_efi_jumpstart
    return bytes(sb)


class StubReader(object):
    """Serves fixed bytes for the container-base read only."""

    def __init__(self, base_bytes):
        self.base_bytes = base_bytes
        self.reads = []

    def read(self, off, length):
        self.reads.append((off, length))
        return self.base_bytes[:length]


class StubContainer(object):
    """The narrow slice of ``apfs-volumes.Container`` this tool consumes.

    Deliberately not the real class: building a checkpoint descriptor area to
    exercise four struct offsets would test ``apfs-volumes.py`` instead, which
    has its own suite. What this DOES pin is the attribute surface -- if
    ``Container`` renames ``checkpoint`` or ``block_size``, the tool breaks and
    this stub keeps passing, so ``test_container_attribute_surface`` below reads
    the real class and asserts the names still exist.
    """

    def __init__(self, blocks, checkpoint_block=191, xid=15491, base_bytes=None):
        self.blocks = blocks
        self.checkpoint = {"block": checkpoint_block}
        self.xid = xid
        self.block_size = BLOCK
        self.base = 0
        self.reader = StubReader(base_bytes or (b"\x00" * BLOCK))

    def block(self, paddr):
        if paddr not in self.blocks:
            raise AssertionError("unexpected block read: %d" % paddr)
        return self.blocks[paddr]


def happy_container(driver=None, file_len=None, extents=None, o_type=0x40000014):
    """A container whose jumpstart record points at a real PE image."""
    driver = driver if driver is not None else make_pe(700)
    file_len = len(driver) if file_len is None else file_len
    extents = extents or [(2507744, 1)]
    payload = driver + b"\x00" * (BLOCK - (len(driver) % BLOCK or BLOCK))
    blocks = {
        191: make_superblock(9224229),
        9224229: make_jumpstart_block(extents, file_len, o_type=o_type),
    }
    off = 0
    for start, count in extents:
        for i in range(count):
            blocks[start + i] = payload[off:off + BLOCK].ljust(BLOCK, b"\x00")
            off += BLOCK
    return StubContainer(blocks)


class JumpstartFieldOffsets(unittest.TestCase):
    """Each field is read from where the format puts it, not from its neighbour."""

    def test_reports_every_field(self):
        cont = happy_container(driver=make_pe(700), extents=[(2507744, 1)])
        info = js.read_jumpstart(cont)
        self.assertTrue(info["present"], info.get("reason"))
        self.assertEqual(info["nx_efi_jumpstart"], 9224229)
        self.assertEqual(info["magic"], "JSDR")
        self.assertEqual(info["version"], 1)
        self.assertEqual(info["num_extents"], 1)
        self.assertEqual(info["extents"], [{"start": 2507744, "blocks": 1}])

    def test_file_len_is_not_the_extent_count(self):
        """Separating input: swapping the two adjacent uint32s changes the answer.

        ``nej_efi_file_len`` (0x28) and ``nej_num_extents`` (0x2C) are adjacent.
        A count of 3 and a length of 700 are both small and both plausible, so a
        reader that transposed them would still parse. These values differ, so
        only the correct pairing satisfies both assertions.
        """
        cont = happy_container(driver=make_pe(700), file_len=700,
                               extents=[(400, 1), (500, 1), (600, 1)])
        info = js.read_jumpstart(cont)
        self.assertEqual(info["efi_file_len"], 700)
        self.assertEqual(info["num_extents"], 3)

    def test_extents_are_not_the_reserved_array(self):
        """``nej_reserved[16]`` sits between the header and the extents.

        Reading extents at 0x30 rather than 0xB0 lands in that zero-filled
        array, which decodes as (start 0, blocks 0) -- an empty file rather than
        an exception. A non-zero start block is what separates the two.
        """
        cont = happy_container(extents=[(2507744, 1)])
        info = js.read_jumpstart(cont)
        self.assertNotEqual(info["extents"][0]["start"], 0)
        self.assertEqual(info["extents"][0]["start"], 2507744)


class JumpstartRefusals(unittest.TestCase):
    """Every way the record can be wrong is reported, with a reason."""

    def test_absent_field_is_not_an_error(self):
        cont = StubContainer({191: make_superblock(0)})
        info = js.read_jumpstart(cont)
        self.assertFalse(info["present"])
        self.assertIn("no embedded EFI driver", info["reason"])

    def test_wrong_magic_is_refused_and_named(self):
        blocks = {
            191: make_superblock(9224229),
            9224229: make_jumpstart_block([(400, 1)], 512, magic=b"OMAP"),
        }
        info = js.read_jumpstart(StubContainer(blocks))
        self.assertFalse(info["present"])
        self.assertIn("OMAP", info["reason"])

    def test_extent_count_past_the_block_is_refused(self):
        """245 extents fit in a 4 KiB block after the 0xB0 header; 246 do not."""
        blocks = {
            191: make_superblock(9224229),
            9224229: make_jumpstart_block([(400, 1)], 512, num_extents=246),
        }
        info = js.read_jumpstart(StubContainer(blocks))
        self.assertFalse(info["present"])
        self.assertEqual(info["max_extents_in_block"], 245)
        self.assertIn("246", info["reason"])

    def test_boundary_extent_count_is_accepted(self):
        """The refusal is off-by-one safe: exactly 245 must NOT be refused."""
        blocks = {
            191: make_superblock(9224229),
            9224229: make_jumpstart_block([(400, 1)], 512, num_extents=245),
        }
        info = js.read_jumpstart(StubContainer(blocks))
        self.assertNotIn("does not fit", info.get("reason", ""))

    def test_short_extent_coverage_is_refused(self):
        """One 4 KiB block cannot hold a 713784-byte driver."""
        blocks = {
            191: make_superblock(9224229),
            9224229: make_jumpstart_block([(400, 1)], 713784),
        }
        info = js.read_jumpstart(StubContainer(blocks))
        self.assertFalse(info["present"])
        self.assertIn("713784", info["reason"])


class ObjectTypeIsReportedNotAsserted(unittest.TestCase):
    """A half-remembered constant must not turn a good image into a fake ABSENT."""

    def test_unexpected_o_type_still_parses(self):
        cont = happy_container(o_type=0x40000099)
        info = js.read_jumpstart(cont)
        self.assertTrue(info["present"], info.get("reason"))
        self.assertFalse(info["o_type_matches"])
        self.assertEqual(info["o_type_low16"], 0x99)

    def test_documented_o_type_matches(self):
        info = js.read_jumpstart(happy_container())
        self.assertTrue(info["o_type_matches"])
        self.assertEqual(info["o_type_expected"], 0x14)


class ExtentReads(unittest.TestCase):
    """Concatenation order and the trim to the declared length."""

    def test_multiple_extents_concatenate_in_order(self):
        blocks = {
            10: b"A" * BLOCK,
            11: b"B" * BLOCK,
            50: b"C" * BLOCK,
        }
        cont = StubContainer(blocks)
        data = js.read_extents(
            cont, [{"start": 10, "blocks": 2}, {"start": 50, "blocks": 1}],
            3 * BLOCK)
        self.assertEqual(data[:BLOCK], b"A" * BLOCK)
        self.assertEqual(data[BLOCK:2 * BLOCK], b"B" * BLOCK)
        self.assertEqual(data[2 * BLOCK:], b"C" * BLOCK)

    def test_result_is_trimmed_to_the_declared_length(self):
        cont = StubContainer({10: b"A" * BLOCK, 11: b"B" * BLOCK})
        data = js.read_extents(cont, [{"start": 10, "blocks": 2}], 5000)
        self.assertEqual(len(data), 5000)
        self.assertEqual(data[4096:], b"B" * 904)

    def test_stops_before_reading_blocks_it_does_not_need(self):
        """A 1-byte file must not read the second block of its own extent.

        StubContainer raises on an unexpected paddr, so the absence of block 11
        is the assertion.
        """
        cont = StubContainer({10: b"A" * BLOCK})
        data = js.read_extents(cont, [{"start": 10, "blocks": 2}], 1)
        self.assertEqual(data, b"A")


class PeIdentification(unittest.TestCase):
    """"Wrote N bytes" is not "extracted a driver"."""

    def test_real_pe_is_identified(self):
        pe = js.describe_pe(make_pe(700, machine=0x8664, subsystem=11))
        self.assertTrue(pe["is_pe"])
        self.assertEqual(pe["machine_name"], "x86_64")
        self.assertEqual(pe["subsystem_name"], "EFI_BOOT_SERVICE_DRIVER")

    def test_right_length_wrong_bytes_is_rejected(self):
        """The exact failure a wrong extent list produces."""
        pe = js.describe_pe(b"\x00" * 713784)
        self.assertFalse(pe["is_pe"])
        self.assertEqual(pe["length"], 713784)
        self.assertIn("MZ", pe["reason"])

    def test_mz_without_pe_header_is_rejected(self):
        """A DOS stub alone must not pass -- this is what a truncated read looks like."""
        buf = bytearray(make_pe(700))
        buf[0x80:0x84] = b"\x00\x00\x00\x00"
        pe = js.describe_pe(bytes(buf))
        self.assertFalse(pe["is_pe"])
        self.assertIn("e_lfanew", pe["reason"])

    def test_lfanew_past_end_is_rejected_without_raising(self):
        buf = bytearray(make_pe(700))
        struct.pack_into("<I", buf, 0x3C, 0x7FFFFFF0)
        pe = js.describe_pe(bytes(buf))
        self.assertFalse(pe["is_pe"])
        self.assertIn("past the end", pe["reason"])

    def test_unknown_subsystem_is_reported_not_refused(self):
        pe = js.describe_pe(make_pe(700, subsystem=99))
        self.assertTrue(pe["is_pe"])
        self.assertEqual(pe["subsystem"], 99)
        self.assertEqual(pe["subsystem_name"], "unknown")


class SuperblockSelection(unittest.TestCase):
    """The checkpoint-current NXSB is used, not block 0.

    APFS block 0's superblock lags the checkpoint area -- following it is how
    ``apfs-volumes.py`` once decoded a block number larger than the disk. This
    tool must inherit that fix rather than re-make the mistake.
    """

    def test_reads_the_checkpoint_block(self):
        blocks = {
            191: make_superblock(9224229),
            9224229: make_jumpstart_block([(400, 1)], 512),
            400: make_pe(512).ljust(BLOCK, b"\x00"),
        }
        cont = StubContainer(blocks, checkpoint_block=191)
        info = js.read_jumpstart(cont)
        self.assertEqual(info["checkpoint_block"], 191)
        self.assertEqual(info["nx_efi_jumpstart"], 9224229)

    def test_falls_back_to_the_base_read_only_when_checkpoint_is_zero(self):
        cont = StubContainer({}, checkpoint_block=0,
                             base_bytes=make_superblock(0))
        info = js.read_jumpstart(cont)
        self.assertFalse(info["present"])
        self.assertEqual(cont.reader.reads, [(0, BLOCK)])


class ContainerAttributeSurface(unittest.TestCase):
    """The stub above is only honest while the real Container still looks like it."""

    def test_real_container_exposes_what_the_tool_uses(self):
        vol = _load("apfs_volumes_surface", "apfs-volumes.py")
        self.assertTrue(hasattr(vol, "Container"))
        for name in ("block", "__init__"):
            self.assertTrue(hasattr(vol.Container, name),
                            "Container lost %s -- apfs-jumpstart.py calls it" % name)
        with open(os.path.join(TOOLS, "apfs-volumes.py"), encoding="utf-8") as f:
            src = f.read()
        for attr in ("self.checkpoint", "self.block_size", "self.xid", "self.reader"):
            self.assertIn(attr, src,
                          "Container no longer sets %s -- apfs-jumpstart.py reads it"
                          % attr)


class NonVacuity(unittest.TestCase):
    """Prove the anchors are load-bearing by corrupting them in memory.

    A suite whose fixtures are built from the module's own constants passes
    identically whatever those constants say. Each mutation below must break the
    happy path; if one does not, the test it protects was measuring nothing.
    """

    def _happy_is_correct(self):
        """Every field the suite claims to pin, checked in one place.

        ``efi_file_len`` is in here because leaving it out is what made the
        first run of this test fail honestly: with ``NEJ_FILE_LEN_OFF`` moved to
        0x2C the tool reads ``nej_num_extents`` (1) as the length, 1 is less
        than the 4096 bytes the extent covers, so nothing refused and every
        other field still agreed. A predicate that omits a field cannot prove
        that field's offset is load-bearing.
        """
        info = js.read_jumpstart(happy_container())
        return (info["present"]
                and info["magic"] == "JSDR"
                and info["version"] == 1
                and info["efi_file_len"] == 700
                and info["num_extents"] == 1
                and info["extents"] == [{"start": 2507744, "blocks": 1}])

    def test_baseline_passes(self):
        self.assertTrue(self._happy_is_correct())

    def test_each_offset_is_load_bearing(self):
        mutations = {
            "NX_EFI_JUMPSTART_OFF": 0x500,
            "NEJ_MAGIC_OFF": 0x24,
            "NEJ_FILE_LEN_OFF": 0x2C,
            "NEJ_NUM_EXTENTS_OFF": 0x28,
            "NEJ_EXTENTS_OFF": 0x30,
            "PRANGE_SIZE": 8,
        }
        for name, wrong in mutations.items():
            original = getattr(js, name)
            self.assertNotEqual(original, wrong,
                                "%s mutation is not a mutation" % name)
            setattr(js, name, wrong)
            try:
                still_right = self._happy_is_correct()
            except Exception:
                still_right = False
            finally:
                setattr(js, name, original)
            self.assertFalse(
                still_right,
                "corrupting %s left the happy path agreeing -- nothing tests it"
                % name)

    def test_pe_signature_check_is_load_bearing(self):
        """A 700-byte fixture must be rejected once its MZ is removed."""
        buf = bytearray(make_pe(700))
        buf[0:2] = b"ZM"
        self.assertFalse(js.describe_pe(bytes(buf))["is_pe"])


if __name__ == "__main__":
    unittest.main()
