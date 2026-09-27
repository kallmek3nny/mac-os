#!/usr/bin/env python3
"""
test_parse_vbios.py -- regression tests for ``tools/parse-vbios.py``.

Why this exists
---------------
`parse-vbios.py` is an *evidence producer*. It decides whether the GA107 falcon
ucode table is absent from `reference/ga107-firmware/vbios-ga107.rom` or merely
outside it -- and Phase 4 firmware work depends on that answer. A wrong struct
layout does not crash a binary parser; it prints a plausible, confidently
formatted, wrong number.

That is not hypothetical here. Two specific errors were made while developing
this parser and both are pinned below:

1. **PCIR ``ds_revision`` read as u16 instead of u8.** Every field from +0x0c
   onward shifted one byte late, and the output still looked reasonable: class
   code "7f 03 02" (nonsense, but three plausible-looking bytes), code type 128,
   and ``last_image=NO`` -- the exact opposite of the truth. See
   ``PcirLayoutTests``.

2. **``rom.find(b"PCIR")`` treated as structure discovery.** A VBIOS contains
   x86 code that walks its own image chain, so "PCIR"/"NPDS"/"NPDE" appear as
   immediate operands. Decoding the hit at 0xf3b4 in the real ROM as an NPDE
   yielded "image_size 18208768 bytes" for a 65024-byte file. See
   ``ChainWalkTests.test_scan_only_hits_are_not_images``.

Most tests build synthetic images in memory so the suite passes on a bare
checkout. The tests that pin measured values from the real ROM skip cleanly when
that file is absent -- it is an NVIDIA-signed blob that must not be
re-published, so it may legitimately not be present.
"""

import hashlib
import importlib.util
import io
import os
import struct
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(ROOT, "tools", "parse-vbios.py")
REAL_ROM = os.path.join(ROOT, "reference", "ga107-firmware", "vbios-ga107.rom")


def load_tool():
    """Import tools/parse-vbios.py by path -- the hyphen makes it unimportable."""
    spec = importlib.util.spec_from_file_location("parse_vbios", TOOL)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load %s" % TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pv = load_tool()


# --------------------------------------------------------------------------
# synthetic image builders
# --------------------------------------------------------------------------

def make_pcir(vendor=0x10DE, device=0x25A2, ds_length=24, ds_revision=0,
              class_low=0x00, class_mid=0x02, class_high=0x03,
              image_blocks=2, code_revision=0x0001, code_type=0x00,
              indicator=0x80):
    """A 24-byte PCI Data Structure with every field independently settable."""
    out = bytearray(24)
    out[0:4] = b"PCIR"
    struct.pack_into("<HHHH", out, 4, vendor, device, 0x0000, ds_length)
    out[0x0C] = ds_revision
    out[0x0D] = class_low
    out[0x0E] = class_mid
    out[0x0F] = class_high
    struct.pack_into("<HH", out, 0x10, image_blocks, code_revision)
    out[0x14] = code_type
    out[0x15] = indicator
    return bytes(out)


def make_npde(revision=0x0101, length=20, image_blocks=2, indicator=0x00):
    out = bytearray(16)
    out[0:4] = b"NPDE"
    struct.pack_into("<HHH", out, 4, revision, length, image_blocks)
    out[0x0A] = indicator
    return bytes(out)


def make_bit_header(bcd_version=0x0100, header_size=12, token_size=6,
                    token_entries=0):
    """A BIT header whose 12 bytes sum to zero mod 256, as real ones do."""
    out = bytearray(12)
    out[0:6] = b"\xff\xb8BIT\x00"
    struct.pack_into("<H", out, 6, bcd_version)
    out[8] = header_size
    out[9] = token_size
    out[10] = token_entries
    out[11] = (-sum(out[:11])) & 0xFF
    return bytes(out)


def make_bit_token(token_id, version, length, pointer):
    return struct.pack("<BBHH", token_id, version, length, pointer)


def make_image(size=1024, pcir_at=0x40, pcir=None, npde=None, npde_at=None):
    """A minimal single-image PCI expansion ROM."""
    rom = bytearray(size)
    rom[0:2] = b"\x55\xaa"
    struct.pack_into("<H", rom, 0x18, pcir_at)
    body = pcir if pcir is not None else make_pcir(image_blocks=size // 512)
    rom[pcir_at:pcir_at + len(body)] = body
    if npde is not None:
        at = npde_at if npde_at is not None else ((pcir_at + 24 + 15) & ~15)
        rom[at:at + len(npde)] = npde
    return bytes(rom)


# --------------------------------------------------------------------------
# PCIR layout -- the u8-vs-u16 revision regression
# --------------------------------------------------------------------------

class PcirLayoutTests(unittest.TestCase):
    def test_every_field_decodes_at_its_spec_offset(self):
        raw = make_pcir(vendor=0x1234, device=0x5678, ds_length=24,
                        ds_revision=3, class_low=0x11, class_mid=0x22,
                        class_high=0x33, image_blocks=0x0102,
                        code_revision=0xABCD, code_type=0x03,
                        indicator=0x80)
        got = pv.parse_pcir(raw, 0)
        self.assertEqual(got["vendor_id"], 0x1234)
        self.assertEqual(got["device_id"], 0x5678)
        self.assertEqual(got["ds_length"], 24)
        self.assertEqual(got["ds_revision"], 3)
        self.assertEqual(got["class_low"], 0x11)
        self.assertEqual(got["class_mid"], 0x22)
        self.assertEqual(got["class_high"], 0x33)
        self.assertEqual(got["image_blocks"], 0x0102)
        self.assertEqual(got["image_length"], 0x0102 * 512)
        self.assertEqual(got["code_revision"], 0xABCD)
        self.assertEqual(got["code_type"], 0x03)
        self.assertEqual(got["indicator"], 0x80)
        self.assertTrue(got["last_image"])

    def test_ds_revision_is_one_byte_not_two(self):
        """The exact regression: a u16 revision shifts everything after it.

        Under the buggy layout, class_low would read 0x22 (the byte after the
        real class_low) and code_type would read the indicator byte. Asserting
        the correct values here fails loudly if the widths ever slip again.
        """
        raw = make_pcir(ds_revision=0, class_low=0x11, class_mid=0x22,
                        class_high=0x33, code_type=0x00, indicator=0x80)
        got = pv.parse_pcir(raw, 0)
        self.assertEqual(got["class_low"], 0x11, "class shifted by one byte")
        self.assertEqual(got["code_type"], 0x00, "code type shifted")
        self.assertEqual(got["indicator"], 0x80, "indicator shifted")

    def test_last_image_bit_is_bit7_only(self):
        self.assertFalse(pv.parse_pcir(make_pcir(indicator=0x7F), 0)["last_image"])
        self.assertTrue(pv.parse_pcir(make_pcir(indicator=0x80), 0)["last_image"])
        self.assertTrue(pv.parse_pcir(make_pcir(indicator=0xFF), 0)["last_image"])

    def test_class_code_string_is_high_mid_low(self):
        pcir = pv.parse_pcir(make_pcir(class_low=0x00, class_mid=0x02,
                                       class_high=0x03), 0)
        self.assertEqual(pv.pci_class_code(pcir), "030200")

    def test_rejects_wrong_signature_and_truncation(self):
        self.assertIsNone(pv.parse_pcir(b"XXXX" + bytes(20), 0))
        self.assertIsNone(pv.parse_pcir(make_pcir()[:20], 0))
        self.assertIsNone(pv.parse_pcir(make_pcir(), -4))


class NpdeLayoutTests(unittest.TestCase):
    def test_fields_decode(self):
        got = pv.parse_npde(make_npde(revision=0x0101, length=20,
                                      image_blocks=127, indicator=0x00), 0)
        self.assertEqual(got["revision"], 0x0101)
        self.assertEqual(got["length"], 20)
        self.assertEqual(got["image_blocks"], 127)
        self.assertEqual(got["image_length"], 65024)
        self.assertFalse(got["last_image"])

    def test_last_image_bit(self):
        self.assertTrue(pv.parse_npde(make_npde(indicator=0x80), 0)["last_image"])

    def test_rejects_wrong_signature(self):
        self.assertIsNone(pv.parse_npde(b"NPDS" + bytes(12), 0))


# --------------------------------------------------------------------------
# chain walk -- the "find() lies" regression
# --------------------------------------------------------------------------

class ChainWalkTests(unittest.TestCase):
    def test_single_image_walk(self):
        rom = make_image(size=1024, pcir=make_pcir(image_blocks=2,
                                                   indicator=0x80))
        images = pv.walk_rom_images(rom)
        self.assertEqual(len(images), 1)
        self.assertEqual(images[0]["pcir"]["offset"], 0x40)
        self.assertEqual(images[0]["end"], 1024)
        self.assertEqual(images[0]["truncated_by"], 0)
        self.assertIsNone(images[0]["problem"])

    def test_two_image_chain_follows_declared_length(self):
        first = make_image(size=512, pcir=make_pcir(image_blocks=1,
                                                    indicator=0x00))
        second = make_image(size=512, pcir=make_pcir(image_blocks=1,
                                                     code_type=0x03,
                                                     indicator=0x80))
        images = pv.walk_rom_images(first + second)
        self.assertEqual(len(images), 2)
        self.assertEqual(images[1]["offset"], 512)
        self.assertEqual(images[1]["pcir"]["code_type"], 0x03)
        self.assertTrue(images[1]["pcir"]["last_image"])

    def test_scan_only_hits_are_not_images(self):
        """A literal b"PCIR" in the payload must not become an image.

        This is the 0xf37d/0xf385/0xf3b4 case in the real ROM: x86 immediates
        inside the ROM's own image-walking code.
        """
        rom = bytearray(make_image(size=1024,
                                   pcir=make_pcir(image_blocks=2,
                                                  indicator=0x80)))
        rom[0x200:0x206] = b"\x66\x3d" + b"PCIR"      # cmp eax, 'PCIR'
        rom[0x300:0x304] = b"NPDE"
        rom = bytes(rom)

        images = pv.walk_rom_images(rom)
        self.assertEqual(len(images), 1, "a scan hit was walked as an image")

        hits = pv.scan_signatures(rom, images)
        by_offset = {h["offset"]: h for h in hits}
        self.assertIn(0x202, by_offset)
        self.assertFalse(by_offset[0x202]["chain_reached"])
        self.assertIn(0x300, by_offset)
        self.assertFalse(by_offset[0x300]["chain_reached"])
        self.assertTrue(by_offset[0x40]["chain_reached"])

    def test_declared_length_past_eof_is_flagged(self):
        rom = make_image(size=1024, pcir=make_pcir(image_blocks=8,
                                                   indicator=0x80))
        images = pv.walk_rom_images(rom)
        self.assertEqual(images[0]["truncated_by"], 8 * 512 - 1024)

    def test_zero_length_image_does_not_loop_forever(self):
        rom = make_image(size=1024, pcir=make_pcir(image_blocks=0,
                                                   indicator=0x00))
        images = pv.walk_rom_images(rom)
        self.assertEqual(len(images), 1)
        self.assertIn("length of 0", images[0]["problem"])

    def test_missing_rom_signature_reported_not_crashed(self):
        images = pv.walk_rom_images(bytes(64))
        self.assertEqual(len(images), 1)
        self.assertIn("no 0x55aa", images[0]["problem"])

    def test_bad_pcir_pointer_reported(self):
        rom = bytearray(make_image(size=1024))
        struct.pack_into("<H", rom, 0x18, 0x300)      # points at zeroes
        images = pv.walk_rom_images(bytes(rom))
        self.assertIsNone(images[0]["pcir"])
        self.assertIn("not a usable", images[0]["problem"])

    def test_conflicting_last_image_indicators_are_detected(self):
        rom = make_image(size=65024,
                         pcir=make_pcir(image_blocks=127, indicator=0x80),
                         npde=make_npde(image_blocks=127, indicator=0x00))
        images = pv.walk_rom_images(rom)
        conflicts = pv.chain_last_image_conflict(images)
        self.assertEqual(len(conflicts), 1)
        self.assertTrue(conflicts[0]["pcir_last"])
        self.assertFalse(conflicts[0]["npde_last"])

    def test_agreeing_indicators_produce_no_conflict(self):
        rom = make_image(size=65024,
                         pcir=make_pcir(image_blocks=127, indicator=0x80),
                         npde=make_npde(image_blocks=127, indicator=0x80))
        images = pv.walk_rom_images(rom)
        self.assertEqual(pv.chain_last_image_conflict(images), [])


# --------------------------------------------------------------------------
# BIT table
# --------------------------------------------------------------------------

class BitTableTests(unittest.TestCase):
    def build(self, tokens):
        """A buffer with a BIT header at 0x100 and `tokens` after it."""
        rom = bytearray(0x1000)
        header = make_bit_header(token_entries=len(tokens))
        rom[0x100:0x100 + len(header)] = header
        at = 0x100 + len(header)
        for token in tokens:
            rom[at:at + 6] = token
            at += 6
        return rom

    def test_header_found_and_checksum_valid(self):
        rom = bytes(self.build([]))
        offset = pv.find_bit_header(rom)
        self.assertEqual(offset, 0x100)
        header = pv.parse_bit_header(rom, offset)
        self.assertEqual(header["bcd_version"], 0x0100)
        self.assertEqual(header["header_size"], 12)
        self.assertEqual(header["token_size"], 6)
        self.assertTrue(header["checksum_valid"])
        self.assertEqual(header["checksum_sum"], 0)

    def test_corrupt_header_fails_checksum(self):
        rom = bytearray(self.build([]))
        rom[0x106] ^= 0x5A                     # flip a bcd_version byte
        header = pv.parse_bit_header(bytes(rom), 0x100)
        self.assertFalse(header["checksum_valid"])
        self.assertNotEqual(header["checksum_sum"], 0)

    def test_absent_bit_table_returns_none(self):
        self.assertIsNone(pv.find_bit_header(bytes(0x400)))

    def test_bare_bit_string_is_not_enough(self):
        """"BIT\\0" alone must not be accepted -- the id word is part of it."""
        rom = bytearray(0x400)
        rom[0x80:0x84] = b"BIT\x00"
        self.assertIsNone(pv.find_bit_header(bytes(rom)))

    def test_tokens_decode_with_names_and_range_flags(self):
        rom = self.build([
            make_bit_token(0x42, 2, 37, 0x0800),
            make_bit_token(0x4E, 0, 0, 0x0000),
            make_bit_token(0x70, 2, 4, 0x0900),
            make_bit_token(0x99, 1, 8, 0x0F00),      # unknown id, in range
            make_bit_token(0x9A, 1, 8, 0x0FFC),      # payload past the end
        ])
        rom = bytes(rom)
        header = pv.parse_bit_header(rom, 0x100)
        tokens = pv.parse_bit_tokens(rom, header)
        self.assertEqual(len(tokens), 5)

        by_id = pv.tokens_by_id(tokens)
        self.assertEqual(by_id[0x42]["name"], "BIT_TOKEN_BIOSDATA")
        self.assertEqual(by_id[0x70]["name"], "BIT_TOKEN_FALCON_DATA")
        self.assertEqual(by_id[0x99]["name"], "unknown to this parser")
        self.assertEqual(by_id[0x42]["char"], "B")
        self.assertTrue(by_id[0x42]["payload_in_range"])
        self.assertFalse(by_id[0x4E]["payload_in_range"])   # zero length
        self.assertFalse(by_id[0x9A]["payload_in_range"])   # past the end

    def test_token_table_truncated_at_buffer_end(self):
        rom = bytearray(0x120)
        header = make_bit_header(token_entries=40)
        rom[0x100:0x10C] = header
        rom = bytes(rom)
        tokens = pv.parse_bit_tokens(rom, pv.parse_bit_header(rom, 0x100))
        self.assertLess(len(tokens), 40)

    def test_bios_version_formatting(self):
        rom = bytearray(self.build([make_bit_token(0x42, 2, 37, 0x0800)]))
        struct.pack_into("<I", rom, 0x0800, 0x94074E00)
        rom[0x0804] = 0x4B
        tokens = pv.parse_bit_tokens(bytes(rom),
                                     pv.parse_bit_header(bytes(rom), 0x100))
        version = pv.parse_bios_version(bytes(rom), tokens)
        self.assertEqual(version["formatted"], "94.07.4E.00.4B")

    def test_string_table_dereferences_triples(self):
        rom = bytearray(self.build([make_bit_token(0x53, 2, 6, 0x0800)]))
        struct.pack_into("<HB", rom, 0x0800, 0x0900, 6)
        struct.pack_into("<HB", rom, 0x0803, 0x0000, 4)      # unusable slot
        rom[0x0900:0x0906] = b"NVIDIA"
        rom = bytes(rom)
        tokens = pv.parse_bit_tokens(rom, pv.parse_bit_header(rom, 0x100))
        strings = pv.parse_string_table(rom, tokens)
        self.assertEqual(len(strings), 2)
        self.assertTrue(strings[0]["usable"])
        self.assertEqual(strings[0]["text"], "NVIDIA")
        self.assertFalse(strings[1]["usable"])


# --------------------------------------------------------------------------
# the falcon verdict -- the question the tool exists to answer
# --------------------------------------------------------------------------

class FalconStatusTests(unittest.TestCase):
    def build(self, tokens, size=0x1000):
        rom = bytearray(size)
        header = make_bit_header(token_entries=len(tokens))
        rom[0x100:0x100 + len(header)] = header
        at = 0x100 + len(header)
        for token in tokens:
            rom[at:at + 6] = token
            at += 6
        return rom

    def status_for(self, rom):
        rom = bytes(rom)
        header = pv.parse_bit_header(rom, pv.find_bit_header(rom))
        return pv.falcon_table_status(rom, pv.parse_bit_tokens(rom, header))

    def test_no_token(self):
        status = self.status_for(self.build([make_bit_token(0x42, 2, 4, 0x800)]))
        self.assertEqual(status["state"], "no_token")

    def test_null_pointer_means_the_rom_declares_no_table(self):
        rom = self.build([make_bit_token(0x70, 2, 4, 0x800)])
        struct.pack_into("<I", rom, 0x800, 0)
        self.assertEqual(self.status_for(rom)["state"], "null")

    def test_malformed_when_payload_too_small(self):
        rom = self.build([make_bit_token(0x70, 2, 2, 0x800)])
        self.assertEqual(self.status_for(rom)["state"], "malformed")

    def test_off_file_pointer_reports_shortfall_not_absence(self):
        """The GA107 case: a well-formed pointer past the end of the dump."""
        rom = self.build([make_bit_token(0x70, 2, 4, 0x800)], size=0x1000)
        struct.pack_into("<I", rom, 0x800, 0x00077F77)
        status = self.status_for(rom)
        self.assertEqual(status["state"], "off_file")
        self.assertEqual(status["pointer"], 0x00077F77)
        self.assertEqual(status["needs_at_least"], 0x00077F77 + 6)
        self.assertEqual(status["shortfall"], 0x00077F77 + 6 - 0x1000)

    def test_in_range_pointer_decodes_the_table_header(self):
        rom = self.build([make_bit_token(0x70, 2, 4, 0x800)], size=0x2000)
        struct.pack_into("<I", rom, 0x800, 0x1000)
        rom[0x1000:0x1006] = bytes([1, 6, 12, 4, 3, 44])
        status = self.status_for(rom)
        self.assertEqual(status["state"], "present")
        table = status["table"]
        self.assertEqual(table["version"], 1)
        self.assertEqual(table["header_size"], 6)
        self.assertEqual(table["entry_size"], 12)
        self.assertEqual(table["entry_count"], 4)
        self.assertEqual(table["desc_version"], 3)
        self.assertEqual(table["desc_size"], 44)


class StringAndMagicTests(unittest.TestCase):
    def test_extract_strings_respects_min_length(self):
        rom = b"\x00" + b"SHORT" + b"\x00" + b"LONG ENOUGH STRING" + b"\x00"
        found = pv.extract_strings(rom, 12)
        self.assertEqual([e["text"] for e in found], ["LONG ENOUGH STRING"])
        self.assertEqual(found[0]["offset"], 7)

    def test_extract_strings_catches_a_run_at_end_of_buffer(self):
        found = pv.extract_strings(b"\x00" + b"TRAILING RUN OK", 12)
        self.assertEqual(len(found), 1)

    def test_container_magics_absent_on_a_blank_buffer(self):
        for entry in pv.scan_container_magics(bytes(0x400)):
            self.assertEqual(entry["offsets"], [])

    def test_container_magic_found_when_present(self):
        rom = bytes(0x100) + struct.pack("<I", 0x000010DE) + bytes(0x100)
        found = {e["label"]: e["offsets"] for e in pv.scan_container_magics(rom)}
        self.assertEqual(found["nvfw_bin_hdr binMagic 0x000010de LE"], [0x100])


class ReportTests(unittest.TestCase):
    def test_report_rejects_a_non_rom(self):
        buf = io.StringIO()
        code = pv.report(bytes(0x400), "synthetic", out=buf)
        self.assertEqual(code, 2)
        self.assertIn("no 0x55aa", buf.getvalue())

    def test_report_rejects_a_rom_without_a_bit_table(self):
        rom = make_image(size=1024, pcir=make_pcir(image_blocks=2,
                                                   indicator=0x80))
        buf = io.StringIO()
        code = pv.report(rom, "synthetic", out=buf)
        self.assertEqual(code, 2)
        self.assertIn("no BIT table", buf.getvalue())

    def test_report_succeeds_on_a_synthetic_full_image(self):
        rom = bytearray(make_image(
            size=0x2000,
            pcir=make_pcir(image_blocks=16, indicator=0x80),
            npde=make_npde(image_blocks=16, indicator=0x00)))
        header = make_bit_header(token_entries=2)
        rom[0x100:0x10C] = header
        rom[0x10C:0x112] = make_bit_token(0x42, 2, 5, 0x800)
        rom[0x112:0x118] = make_bit_token(0x70, 2, 4, 0x810)
        struct.pack_into("<I", rom, 0x800, 0x94074E00)
        rom[0x804] = 0x4B
        struct.pack_into("<I", rom, 0x810, 0x00077F77)

        buf = io.StringIO()
        code = pv.report(bytes(rom), "synthetic", show_scan_hits=True, out=buf)
        text = buf.getvalue()
        self.assertEqual(code, 0)
        self.assertIn("94.07.4E.00.4B", text)
        self.assertIn("NOT IN THIS DUMP", text)
        self.assertIn("last-image indicators DISAGREE", text)


# --------------------------------------------------------------------------
# measured values from the real GA107 dump
# --------------------------------------------------------------------------

@unittest.skipUnless(os.path.isfile(REAL_ROM),
                     "reference/ga107-firmware/vbios-ga107.rom not present "
                     "(NVIDIA-signed blob, may legitimately be absent)")
class RealGa107RomTests(unittest.TestCase):
    """Pin the values measured from the real dump on 2026-08-09.

    Every number here was read off the bytes, not taken from a datasheet. If the
    file is ever replaced these tests fail, which is the intended behaviour:
    the identity of the artifact is part of the evidence.
    """

    @classmethod
    def setUpClass(cls):
        with open(REAL_ROM, "rb") as handle:
            cls.rom = handle.read()
        cls.images = pv.walk_rom_images(cls.rom)
        cls.header = pv.parse_bit_header(cls.rom, pv.find_bit_header(cls.rom))
        cls.tokens = pv.parse_bit_tokens(cls.rom, cls.header)

    def test_file_identity(self):
        self.assertEqual(len(self.rom), 65024)
        self.assertEqual(hashlib.md5(self.rom).hexdigest(),
                         "2299fa8b2b12518c475f1024a197091f")

    def test_image_zero_matches_the_target_die(self):
        self.assertEqual(len(self.images), 1)
        pcir = self.images[0]["pcir"]
        self.assertEqual(pcir["offset"], 0x170)
        self.assertEqual(pcir["vendor_id"], 0x10DE)
        self.assertEqual(pcir["device_id"], 0x25A2)      # RTX 3050 Laptop GA107
        self.assertEqual(pcir["ds_length"], 24)
        self.assertEqual(pcir["ds_revision"], 0)

    def test_class_code_is_3d_controller_matching_hardware_md(self):
        """030200 -- the muxless signature HARDWARE.md records from lspci."""
        self.assertEqual(pv.pci_class_code(self.images[0]["pcir"]), "030200")

    def test_image_zero_ends_exactly_at_end_of_file(self):
        pcir = self.images[0]["pcir"]
        self.assertEqual(pcir["image_blocks"], 127)
        self.assertEqual(pcir["image_length"], 65024)
        self.assertEqual(self.images[0]["end"], len(self.rom))
        self.assertEqual(self.images[0]["truncated_by"], 0)

    def test_legacy_image_is_x86_and_marked_last(self):
        pcir = self.images[0]["pcir"]
        self.assertEqual(pcir["code_type"], 0x00)        # x86 PC-AT
        self.assertEqual(pcir["indicator"], 0x80)
        self.assertTrue(pcir["last_image"])

    def test_npde_contradicts_the_pcir_last_image_bit(self):
        """The decisive fact: this dump is one image of a larger ROM."""
        npde = self.images[0]["npde"]
        self.assertIsNotNone(npde)
        self.assertEqual(npde["offset"], 0x190)
        self.assertEqual(npde["image_blocks"], 127)
        self.assertFalse(npde["last_image"])
        conflicts = pv.chain_last_image_conflict(self.images)
        self.assertEqual(len(conflicts), 1)
        self.assertTrue(conflicts[0]["pcir_last"])
        self.assertFalse(conflicts[0]["npde_last"])

    def test_tail_signature_hits_are_x86_immediates_not_structures(self):
        hits = {h["offset"]: h for h in pv.scan_signatures(self.rom, self.images)}
        for offset in (0xF37D, 0xF385, 0xF3B4):
            self.assertIn(offset, hits)
            self.assertFalse(hits[offset]["chain_reached"],
                             "0x%06x was walked as a structure" % offset)
        self.assertTrue(hits[0x170]["chain_reached"])
        self.assertTrue(hits[0x190]["chain_reached"])

    def test_bit_header(self):
        self.assertEqual(self.header["offset"], 0x1B0)
        self.assertEqual(self.header["bcd_version"], 0x0100)
        self.assertEqual(self.header["header_size"], 12)
        self.assertEqual(self.header["token_size"], 6)
        self.assertEqual(self.header["token_entries"], 17)
        self.assertEqual(self.header["checksum"], 0x46)
        self.assertTrue(self.header["checksum_valid"])

    def test_all_seventeen_tokens_decode_in_order(self):
        self.assertEqual(len(self.tokens), 17)
        self.assertEqual([t["id"] for t in self.tokens],
                         [0x32, 0x42, 0x43, 0x44, 0x49, 0x4D, 0x4E, 0x50,
                          0x53, 0x54, 0x55, 0x56, 0x78, 0x64, 0x70, 0x75,
                          0x69])

    def test_token_pointers_and_lengths(self):
        by_id = pv.tokens_by_id(self.tokens)
        self.assertEqual((by_id[0x42]["length"], by_id[0x42]["pointer"]),
                         (37, 0x023E))
        self.assertEqual((by_id[0x53]["length"], by_id[0x53]["pointer"]),
                         (24, 0x03C8))
        self.assertEqual((by_id[0x70]["length"], by_id[0x70]["pointer"]),
                         (4, 0x03F7))
        self.assertEqual((by_id[0x4E]["length"], by_id[0x4E]["pointer"]),
                         (0, 0x0000))

    def test_bios_version_matches_the_string_table(self):
        version = pv.parse_bios_version(self.rom, self.tokens)
        self.assertEqual(version["raw"], 0x94074E00)
        self.assertEqual(version["formatted"], "94.07.4E.00.4B")
        strings = pv.parse_string_table(self.rom, self.tokens)
        texts = [s["text"] for s in strings if s["usable"]]
        self.assertTrue(any("Version 94.07.4E.00.4B" in t for t in texts),
                        "string table does not corroborate the version u32")

    def test_string_table_slots(self):
        strings = pv.parse_string_table(self.rom, self.tokens)
        self.assertEqual(len(strings), 8)
        self.assertEqual(strings[0]["pointer"], 0x0068)
        self.assertIn("E4736 SKU 10 VGA BIOS", strings[0]["text"])
        self.assertIn("Copyright (C) 1996-2021 NVIDIA Corp", strings[2]["text"])

    def test_falcon_table_is_off_file_not_absent(self):
        """The card's actual question, answered from the bytes.

        BIT token 0x70 holds 77 7f 07 00 = 0x00077f77 = 491383. That is a
        well-formed pointer, 426365 bytes past the end of this 65024-byte dump.
        So the falcon ucode table is not absent from the ROM -- it is outside
        the image that was dumped, and only a full-ROM read can reach it.
        """
        status = pv.falcon_table_status(self.rom, self.tokens)
        self.assertEqual(status["state"], "off_file")
        self.assertEqual(status["pointer"], 0x00077F77)
        self.assertEqual(status["needs_at_least"], 0x00077F77 + 6)
        self.assertEqual(status["shortfall"], 0x00077F77 + 6 - 65024)
        self.assertEqual(self.rom[0x03F7:0x03FB], b"\x77\x7f\x07\x00")

    def test_no_self_describing_container_anywhere(self):
        for entry in pv.scan_container_magics(self.rom):
            self.assertEqual(entry["offsets"], [],
                             "%s unexpectedly present" % entry["label"])

    def test_head_strings_and_build_markers(self):
        found = {e["offset"]: e["text"] for e in pv.extract_strings(self.rom, 12)}
        self.assertIn(0x1E, found)
        self.assertEqual(found[0x1E], "IBM VGA Compatible")
        self.assertIn(0x4EEB, found)
        self.assertEqual(found[0x4EEB], "BIOS Certificate Check Failed!!!")

    def test_report_returns_zero_on_the_real_rom(self):
        buf = io.StringIO()
        code = pv.report(self.rom, REAL_ROM, show_scan_hits=True, out=buf)
        text = buf.getvalue()
        self.assertEqual(code, 0)
        self.assertIn("0x10de:0x25a2", text)
        self.assertIn("NOT IN THIS DUMP", text)
        self.assertIn("last-image indicators DISAGREE", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
