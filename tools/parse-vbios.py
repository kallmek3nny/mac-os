#!/usr/bin/env python3
"""parse-vbios.py -- decode an NVIDIA VBIOS image: the PCI ROM image chain, the
BIT table, the string table, and the falcon ucode table pointer.

Why this exists
---------------
Phase 4 needs to know where the GA107 falcon / GSP microcode lives. The obvious
guess is "inside the VBIOS", because `open-gpu-kernel-modules` locates the
falcon ucode table through BIT token 0x70 (`BIT_TOKEN_FALCON_DATA`) of the
VBIOS. `reference/ga107-firmware/README.md` claims those tables are *absent*
from our dump. This tool decides that from the bytes rather than from the
claim, and prints the offsets that back whatever it concludes.

Two traps this tool exists to avoid
-----------------------------------
1. **`rom.find(b"PCIR")` lies.** A VBIOS contains x86 code that walks its own
   image chain, so the byte sequences "PCIR", "NPDS" and "NPDE" also appear as
   *immediate operands* inside `cmp` instructions. In the GA107 dump the hits at
   0xf37d / 0xf385 / 0xf3b4 are exactly that -- `66 3d 50 43 49 52` is
   `cmp eax, 'PCIR'`. Decoding one of them as a real structure produced a
   confident "image size 18208768 bytes" during development. So structures are
   trusted ONLY when reached by following the +0x18 pointer and the image-length
   chain. Raw scan hits are reported separately and labelled as such.

2. **The PCIR revision field is ONE byte.** Per the PCI Firmware Specification
   the data structure runs

       +0x00 char sig[4] "PCIR"   +0x04 u16 vendor    +0x06 u16 device
       +0x08 u16 device_list_ptr  +0x0a u16 ds_length +0x0c u8  ds_revision
       +0x0d u8 class_low         +0x0e u8 class_mid  +0x0f u8  class_high
       +0x10 u16 image_length     +0x12 u16 code_rev  +0x14 u8  code_type
       +0x15 u8  indicator (bit7 = last image)

   Reading `ds_revision` as a u16 shifts the class code, image length, code type
   and the last-image bit one byte late -- and the result still looks plausible.
   During development that error reported class "7f 03 02", code type 128 and
   last_image=NO, none of which are true. tests/test_parse_vbios.py pins the
   layout field by field so that specific mistake cannot come back.

A note on BIT token names
-------------------------
The ID-to-name mapping below comes from public reverse-engineering work
(envytools / nouveau) and is NOT authoritative. It is presentation only. Every
verdict this tool reaches depends on three IDs whose meaning is corroborated by
`open-gpu-kernel-modules`: 0x42 BIOSDATA, 0x53 STRING_PTRS, 0x70 FALCON_DATA.
Names marked `?` are guesses; ignore them when reasoning about the ROM.

Usage:
    python tools/parse-vbios.py reference/ga107-firmware/vbios-ga107.rom
    python tools/parse-vbios.py ROM --strings 8
    python tools/parse-vbios.py ROM --scan-hits

Exit codes:
    0  parsed: the image chain, the BIT table and the falcon pointer all
       resolved to a definite answer
    1  usage error, or the file is missing / empty
    2  the file is not a parseable NVIDIA VBIOS -- no 0x55aa signature, no PCIR
       reachable through the chain, or no BIT table

Runs on Windows, macOS, or Linux. No dependencies.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

ROM_SIGNATURE = b"\x55\xaa"
PCIR_SIGNATURE = b"PCIR"
NPDE_SIGNATURE = b"NPDE"
BIT_SIGNATURE = b"\xff\xb8BIT\x00"

# Offset of the u16 that points from an image's start to its PCIR.
PCIR_POINTER_OFFSET = 0x18

# PCI expansion ROM images are counted in 512-byte blocks.
ROM_BLOCK = 512

PCI_CODE_TYPES = {
    0x00: "x86 PC-AT (legacy VGA BIOS)",
    0x01: "Open Firmware",
    0x02: "HP PA-RISC",
    0x03: "EFI / UEFI",
    0x70: "NVIDIA extended",
}

# Presentation only -- see the module docstring. '?' marks a guess.
BIT_TOKEN_NAMES = {
    0x32: "BIT_TOKEN_2?",
    0x42: "BIT_TOKEN_BIOSDATA",
    0x43: "BIT_TOKEN_CLOCK_PTRS?",
    0x44: "BIT_TOKEN_DAC_PTRS?",
    0x49: "BIT_TOKEN_NVINIT_PTRS?",
    0x4D: "BIT_TOKEN_MEMORY_PTRS?",
    0x4E: "BIT_TOKEN_NOP",
    0x50: "BIT_TOKEN_PERF_PTRS?",
    0x53: "BIT_TOKEN_STRING_PTRS",
    0x54: "BIT_TOKEN_TMDS_PTRS?",
    0x55: "BIT_TOKEN_DISPLAY_PTRS?",
    0x56: "BIT_TOKEN_VIRTUAL_PTRS?",
    0x64: "BIT_TOKEN_DP_PTRS?",
    0x69: "BIT_TOKEN_BIT_INIT?",
    0x70: "BIT_TOKEN_FALCON_DATA",
    0x75: "BIT_TOKEN_UEFI_DATA?",
    0x78: "BIT_TOKEN_X?",
}

# Container magics that a firmware blob carrying its own header would show.
# `nvfw_bin_hdr.binMagic` is 0x10de in open-gpu-kernel-modules.
CONTAINER_MAGICS = (
    (struct.pack("<I", 0x000010DE), "nvfw_bin_hdr binMagic 0x000010de LE"),
    (b"NVGI", "NVGI container"),
    (b"NVDA", "NVDA container"),
)


# --------------------------------------------------------------------------
# primitives
# --------------------------------------------------------------------------

def find_all(buf, needle):
    """Every offset of `needle` in `buf`, in order."""
    out, start = [], 0
    while True:
        i = buf.find(needle, start)
        if i < 0:
            return out
        out.append(i)
        start = i + 1


def u8(rom, off):
    return rom[off]


def u16(rom, off):
    return struct.unpack_from("<H", rom, off)[0]


def u32(rom, off):
    return struct.unpack_from("<I", rom, off)[0]


def hexdump(rom, off, length, indent="  "):
    """Classic offset / hex / ASCII dump, as a list of lines."""
    lines = []
    end = min(off + length, len(rom))
    for i in range(off, end, 16):
        chunk = rom[i:i + 16]
        ascii_col = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        lines.append("%s%06x  %-47s  %s" % (indent, i, chunk.hex(" "), ascii_col))
    return lines


# --------------------------------------------------------------------------
# PCI expansion ROM structures
# --------------------------------------------------------------------------

def parse_pcir(rom, off):
    """Decode the PCI Data Structure at absolute offset `off`.

    Returns None when `off` does not carry the "PCIR" signature or the 24-byte
    structure would run past the end of the buffer. Field widths follow the PCI
    Firmware Specification exactly -- see the module docstring for why the
    one-byte `ds_revision` matters.
    """
    if off < 0 or off + 24 > len(rom):
        return None
    if rom[off:off + 4] != PCIR_SIGNATURE:
        return None
    image_blocks = u16(rom, off + 0x10)
    indicator = u8(rom, off + 0x15)
    return {
        "offset": off,
        "vendor_id": u16(rom, off + 0x04),
        "device_id": u16(rom, off + 0x06),
        "device_list_pointer": u16(rom, off + 0x08),
        "ds_length": u16(rom, off + 0x0A),
        "ds_revision": u8(rom, off + 0x0C),
        "class_low": u8(rom, off + 0x0D),
        "class_mid": u8(rom, off + 0x0E),
        "class_high": u8(rom, off + 0x0F),
        "image_blocks": image_blocks,
        "image_length": image_blocks * ROM_BLOCK,
        "code_revision": u16(rom, off + 0x12),
        "code_type": u8(rom, off + 0x14),
        "indicator": indicator,
        "last_image": bool(indicator & 0x80),
    }


def pci_class_code(pcir):
    """The three class bytes as the 6-digit hex string lspci prints."""
    return "%02x%02x%02x" % (pcir["class_high"], pcir["class_mid"],
                             pcir["class_low"])


def parse_npde(rom, off):
    """Decode NVIDIA's PCI Data Extension at absolute offset `off`.

    Layout as read by nouveau (`nvbios_npdeTp`):
        +0x00 char sig[4] "NPDE"   +0x04 u16 revision   +0x06 u16 length
        +0x08 u16 subimage_blocks  +0x0a u8  indicator (bit7 = last image)

    Returns None when the signature is absent or the structure is truncated.
    """
    if off < 0 or off + 12 > len(rom):
        return None
    if rom[off:off + 4] != NPDE_SIGNATURE:
        return None
    blocks = u16(rom, off + 0x08)
    indicator = u8(rom, off + 0x0A)
    return {
        "offset": off,
        "revision": u16(rom, off + 0x04),
        "length": u16(rom, off + 0x06),
        "image_blocks": blocks,
        "image_length": blocks * ROM_BLOCK,
        "indicator": indicator,
        "last_image": bool(indicator & 0x80),
    }


def walk_rom_images(rom, max_images=16):
    """Follow the PCI expansion ROM image chain from offset 0.

    This is the ONLY sanctioned way to reach a PCIR or an NPDE in this tool.
    Each entry describes one image and carries a `problem` key that is None when
    the image decoded cleanly. The walk is the same one Linux performs in
    `pci_get_rom_size()`, which is why a dump taken from
    /sys/bus/pci/devices/*/rom ends where the PCIR last-image bit says it does.
    """
    images = []
    off = 0
    while off < len(rom) and len(images) < max_images:
        entry = {"index": len(images), "offset": off, "pcir": None,
                 "npde": None, "problem": None, "end": None}

        if rom[off:off + 2] != ROM_SIGNATURE:
            entry["problem"] = ("no 0x55aa signature at 0x%06x (found %s)"
                                % (off, rom[off:off + 2].hex() or "EOF"))
            images.append(entry)
            break

        if off + PCIR_POINTER_OFFSET + 2 > len(rom):
            entry["problem"] = "image header truncated before the PCIR pointer"
            images.append(entry)
            break

        pcir_rel = u16(rom, off + PCIR_POINTER_OFFSET)
        pcir = parse_pcir(rom, off + pcir_rel)
        entry["pcir_relative"] = pcir_rel
        if pcir is None:
            entry["problem"] = ("PCIR pointer 0x%04x -> 0x%06x is not a usable "
                                "PCI Data Structure" % (pcir_rel, off + pcir_rel))
            images.append(entry)
            break
        entry["pcir"] = pcir

        # NVIDIA's NPDE, when present, follows the PCIR on the next 16-byte
        # boundary. Reached by arithmetic from a chain-verified PCIR, never by
        # scanning.
        npde_off = (pcir["offset"] + pcir["ds_length"] + 15) & ~15
        entry["npde_expected_at"] = npde_off
        entry["npde"] = parse_npde(rom, npde_off)

        if pcir["image_length"] == 0:
            entry["problem"] = "PCIR declares an image length of 0 blocks"
            images.append(entry)
            break

        entry["end"] = off + pcir["image_length"]
        entry["truncated_by"] = max(0, entry["end"] - len(rom))
        images.append(entry)

        if pcir["last_image"]:
            break
        off = entry["end"]

    return images


def chain_last_image_conflict(images):
    """Report images whose PCIR and NPDE disagree about being the last one.

    This is not a corrupt-ROM check. NVIDIA ROMs are routinely built with the
    legacy PCIR indicator set so that generic PCI code stops after image 0,
    while NVIDIA's own NPDE extension records that further images follow. When
    the two disagree, a dump that ends at the PCIR's boundary is complete as an
    *image* and incomplete as a *ROM* -- which is exactly the distinction that
    decides whether a missing table is absent or merely off-file.
    """
    out = []
    for entry in images:
        pcir, npde = entry["pcir"], entry["npde"]
        if pcir is None or npde is None:
            continue
        if pcir["last_image"] != npde["last_image"]:
            out.append({
                "index": entry["index"],
                "pcir_last": pcir["last_image"],
                "npde_last": npde["last_image"],
                "pcir_offset": pcir["offset"],
                "npde_offset": npde["offset"],
            })
    return out


# --------------------------------------------------------------------------
# BIT table
# --------------------------------------------------------------------------

def find_bit_header(rom):
    """Offset of the BIT header, or None.

    The BIT signature is six bytes (`ff b8 42 49 54 00`), which is specific
    enough that scanning is safe here -- unlike the four-byte PCI signatures.
    The bare string "BIT\\0" alone would not be.
    """
    off = rom.find(BIT_SIGNATURE)
    return None if off < 0 else off


def parse_bit_header(rom, off):
    """Decode the BIT header. Returns None if it does not fit in the buffer.

        +0x00 u16 id 0xb8ff   +0x02 char sig[4] "BIT\\0"  +0x06 u16 bcd_version
        +0x08 u8 header_size  +0x09 u8 token_size  +0x0a u8 token_entries
        +0x0b u8 checksum

    The header's own bytes sum to zero modulo 256.
    """
    if off < 0 or off + 12 > len(rom):
        return None
    header_size = u8(rom, off + 0x08)
    if header_size < 12 or off + header_size > len(rom):
        return None
    checksum_sum = sum(rom[off:off + header_size]) & 0xFF
    return {
        "offset": off,
        "bcd_version": u16(rom, off + 0x06),
        "header_size": header_size,
        "token_size": u8(rom, off + 0x09),
        "token_entries": u8(rom, off + 0x0A),
        "checksum": u8(rom, off + 0x0B),
        "checksum_sum": checksum_sum,
        "checksum_valid": checksum_sum == 0,
    }


def parse_bit_tokens(rom, header):
    """Decode the token array following a BIT header.

    Each entry is `u8 id, u8 version, u16 length, u16 pointer`. Entries whose
    payload would run past the end of the buffer are still returned, with
    `payload_in_range` False -- a truncated dump must be visible, not silently
    dropped.
    """
    tokens = []
    if header is None or header["token_size"] < 6:
        return tokens
    table = header["offset"] + header["header_size"]
    for n in range(header["token_entries"]):
        off = table + n * header["token_size"]
        if off + 6 > len(rom):
            break
        token_id = u8(rom, off)
        length = u16(rom, off + 0x02)
        pointer = u16(rom, off + 0x04)
        in_range = bool(length) and pointer + length <= len(rom)
        tokens.append({
            "index": n,
            "entry_offset": off,
            "id": token_id,
            "char": chr(token_id) if 32 <= token_id < 127 else ".",
            "name": BIT_TOKEN_NAMES.get(token_id, "unknown to this parser"),
            "version": u8(rom, off + 0x01),
            "length": length,
            "pointer": pointer,
            "payload_in_range": in_range,
            "payload": rom[pointer:pointer + length] if in_range else b"",
        })
    return tokens


def tokens_by_id(tokens):
    return {t["id"]: t for t in tokens}


def parse_bios_version(rom, tokens):
    """BIOS version from BIT token 0x42, formatted the way NVIDIA prints it."""
    token = tokens_by_id(tokens).get(0x42)
    if token is None or token["length"] < 5 or not token["payload_in_range"]:
        return None
    raw = u32(rom, token["pointer"])
    oem = u8(rom, token["pointer"] + 4)
    return {
        "raw": raw,
        "oem": oem,
        "formatted": "%02X.%02X.%02X.%02X.%02X" % (
            (raw >> 24) & 0xFF, (raw >> 16) & 0xFF, (raw >> 8) & 0xFF,
            raw & 0xFF, oem),
    }


def parse_string_table(rom, tokens):
    """Dereference BIT token 0x53: a run of (u16 pointer, u8 length) triples."""
    token = tokens_by_id(tokens).get(0x53)
    out = []
    if token is None or not token["payload_in_range"]:
        return out
    payload = token["payload"]
    for i in range(0, len(payload) - 2, 3):
        pointer, length = struct.unpack_from("<HB", payload, i)
        usable = bool(pointer) and pointer + length <= len(rom)
        raw = rom[pointer:pointer + length] if usable else b""
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in raw)
        out.append({
            "slot": i // 3,
            "pointer": pointer,
            "length": length,
            "usable": usable,
            "text": text.rstrip(". "),
        })
    return out


# --------------------------------------------------------------------------
# the falcon ucode table -- the question this tool was written to answer
# --------------------------------------------------------------------------

def parse_falcon_ucode_table(rom, off):
    """Decode the falcon ucode table header at `off`.

    Layout per open-gpu-kernel-modules `FALCON_UCODE_TABLE_HDR_V1`:
        +0x00 u8 version  +0x01 u8 header_size  +0x02 u8 entry_size
        +0x03 u8 entry_count  +0x04 u8 desc_version  +0x05 u8 desc_size
    """
    if off < 0 or off + 6 > len(rom):
        return None
    return {
        "offset": off,
        "version": u8(rom, off + 0),
        "header_size": u8(rom, off + 1),
        "entry_size": u8(rom, off + 2),
        "entry_count": u8(rom, off + 3),
        "desc_version": u8(rom, off + 4),
        "desc_size": u8(rom, off + 5),
    }


def falcon_table_status(rom, tokens):
    """Resolve BIT token 0x70 to a definite verdict about the falcon table.

    `state` is one of:
        no_token   -- the BIT table has no 0x70 entry at all
        malformed  -- the 0x70 entry cannot hold a 32-bit pointer
        null       -- the pointer is 0: the ROM declares no falcon table
        off_file   -- the pointer is valid-looking but lies past this dump
        present    -- the pointer lands inside this dump and was decoded
    """
    token = tokens_by_id(tokens).get(0x70)
    if token is None:
        return {"state": "no_token", "pointer": None, "token": None,
                "detail": "the BIT table contains no token 0x70 "
                          "(BIT_TOKEN_FALCON_DATA)"}
    if token["length"] < 4 or token["pointer"] + 4 > len(rom):
        return {"state": "malformed", "pointer": None, "token": token,
                "detail": "token 0x70 declares %d payload bytes at 0x%04x -- "
                          "too small or out of range for a 32-bit pointer"
                          % (token["length"], token["pointer"])}

    pointer = u32(rom, token["pointer"])
    if pointer == 0:
        return {"state": "null", "pointer": 0, "token": token,
                "detail": "token 0x70 holds a NULL pointer: this ROM declares "
                          "no falcon ucode table"}

    if pointer + 6 > len(rom):
        return {
            "state": "off_file", "pointer": pointer, "token": token,
            "shortfall": pointer + 6 - len(rom),
            "needs_at_least": pointer + 6,
            "detail": "token 0x70 points to 0x%06x, which is %d bytes past the "
                      "end of this %d-byte dump"
                      % (pointer, pointer + 6 - len(rom), len(rom)),
        }

    return {"state": "present", "pointer": pointer, "token": token,
            "table": parse_falcon_ucode_table(rom, pointer),
            "detail": "token 0x70 points to 0x%06x, inside this dump" % pointer}


def scan_container_magics(rom):
    """Where each self-describing firmware container magic appears, if at all."""
    return [{"magic": magic, "label": label, "offsets": find_all(rom, magic)}
            for magic, label in CONTAINER_MAGICS]


def scan_signatures(rom, images):
    """Every raw hit for the PCI signatures, flagged by chain reachability.

    A hit that the chain walk never reached is not a structure -- in the GA107
    dump those hits are x86 immediate operands. Reporting them with the flag is
    the point: it makes the false positives visible instead of decoding them.
    """
    reached = set()
    for entry in images:
        if entry["pcir"]:
            reached.add(entry["pcir"]["offset"])
        if entry["npde"]:
            reached.add(entry["npde"]["offset"])

    out = []
    for signature in (PCIR_SIGNATURE, NPDE_SIGNATURE, b"NPDS"):
        for off in find_all(rom, signature):
            out.append({
                "signature": signature.decode("ascii"),
                "offset": off,
                "chain_reached": off in reached,
                "block_aligned": off % ROM_BLOCK == 0,
            })
    return sorted(out, key=lambda h: h["offset"])


def extract_strings(rom, min_length=12):
    """Printable ASCII runs of at least `min_length` characters."""
    out, current, start = [], [], 0
    for i, byte in enumerate(rom):
        if 32 <= byte < 127:
            if not current:
                start = i
            current.append(chr(byte))
            continue
        if len(current) >= min_length:
            out.append({"offset": start, "text": "".join(current)})
        current = []
    if len(current) >= min_length:
        out.append({"offset": start, "text": "".join(current)})
    return out


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------

def report(rom, path, show_scan_hits=False, string_length=12,
           out=sys.stdout):
    """Print the full decode. Returns the process exit code."""
    def line(text=""):
        print(text, file=out)

    line("file            : %s" % path)
    line("size            : %d bytes (0x%x)" % (len(rom), len(rom)))
    line("md5             : %s" % hashlib.md5(rom).hexdigest())
    line("sha256          : %s" % hashlib.sha256(rom).hexdigest())
    line("first u32 LE    : 0x%08x" % (u32(rom, 0) if len(rom) >= 4 else 0))
    line()

    if rom[:2] != ROM_SIGNATURE:
        line("VERDICT: not a PCI expansion ROM -- no 0x55aa at offset 0.")
        return 2

    # ---- image chain ----
    images = walk_rom_images(rom)
    line("=== PCI expansion ROM image chain ===")
    usable_images = 0
    for entry in images:
        pcir = entry["pcir"]
        if pcir is None:
            line("image %d @0x%06x : %s" % (entry["index"], entry["offset"],
                                            entry["problem"]))
            continue
        usable_images += 1
        line("image %d @0x%06x" % (entry["index"], entry["offset"]))
        line("  PCIR at       : 0x%06x (pointer 0x%04x at +0x18)"
             % (pcir["offset"], entry["pcir_relative"]))
        for dump_line in hexdump(rom, pcir["offset"], 24, indent="    "):
            line(dump_line)
        line("  vendor:device : 0x%04x:0x%04x"
             % (pcir["vendor_id"], pcir["device_id"]))
        line("  ds length/rev : %d bytes, revision %d"
             % (pcir["ds_length"], pcir["ds_revision"]))
        line("  class code    : %s (base 0x%02x, sub 0x%02x, prog-if 0x%02x)"
             % (pci_class_code(pcir), pcir["class_high"], pcir["class_mid"],
                pcir["class_low"]))
        line("  image length  : %d blocks = %d bytes (0x%x)"
             % (pcir["image_blocks"], pcir["image_length"],
                pcir["image_length"]))
        line("  code revision : 0x%04x" % pcir["code_revision"])
        line("  code type     : %d (%s)"
             % (pcir["code_type"],
                PCI_CODE_TYPES.get(pcir["code_type"], "unknown")))
        line("  indicator     : 0x%02x -> last_image=%s"
             % (pcir["indicator"], "YES" if pcir["last_image"] else "NO"))

        npde = entry["npde"]
        if npde is None:
            line("  NPDE          : none at 0x%06x (bytes %s)"
                 % (entry["npde_expected_at"],
                    rom[entry["npde_expected_at"]:
                        entry["npde_expected_at"] + 4].hex(" ")))
        else:
            line("  NPDE at       : 0x%06x  revision 0x%04x, length %d"
                 % (npde["offset"], npde["revision"], npde["length"]))
            line("  NPDE image    : %d blocks = %d bytes"
                 % (npde["image_blocks"], npde["image_length"]))
            line("  NPDE indicator: 0x%02x -> last_image=%s"
                 % (npde["indicator"], "YES" if npde["last_image"] else "NO"))

        if entry["end"] is not None:
            line("  spans         : 0x%06x .. 0x%06x  (file ends 0x%06x)"
                 % (entry["offset"], entry["end"], len(rom)))
            if entry["truncated_by"]:
                line("  -> DECLARED LENGTH EXCEEDS THE FILE by %d bytes"
                     % entry["truncated_by"])
            elif entry["end"] == len(rom):
                line("  -> ends exactly at end of file")
        if entry["problem"]:
            line("  problem       : %s" % entry["problem"])
        line()

    if not usable_images:
        line("VERDICT: 0x55aa is present but no PCIR is reachable through the "
             "chain. Not a parseable VBIOS.")
        return 2

    conflicts = chain_last_image_conflict(images)
    if conflicts:
        line("=== last-image indicators DISAGREE ===")
        for conflict in conflicts:
            line("  image %d: PCIR@0x%06x says last=%s, NPDE@0x%06x says last=%s"
                 % (conflict["index"], conflict["pcir_offset"],
                    "YES" if conflict["pcir_last"] else "NO",
                    conflict["npde_offset"],
                    "YES" if conflict["npde_last"] else "NO"))
        line("  Linux pci_get_rom_size() stops on the PCIR bit, so a dump taken")
        line("  from /sys/bus/pci/devices/*/rom ends here even when NVIDIA's own")
        line("  extension records that further images follow. Such a dump is")
        line("  complete as an IMAGE and incomplete as a ROM.")
        line()

    # ---- BIT table ----
    bit_offset = find_bit_header(rom)
    if bit_offset is None:
        line("VERDICT: no BIT table (signature ff b8 42 49 54 00 not found). "
             "Not a parseable NVIDIA VBIOS.")
        return 2

    header = parse_bit_header(rom, bit_offset)
    if header is None:
        line("VERDICT: BIT signature at 0x%06x but the header is truncated."
             % bit_offset)
        return 2

    line("=== BIT table ===")
    line("  header at     : 0x%06x" % header["offset"])
    for dump_line in hexdump(rom, header["offset"], header["header_size"],
                             indent="    "):
        line(dump_line)
    line("  bcd version   : 0x%04x" % header["bcd_version"])
    line("  header size   : %d bytes" % header["header_size"])
    line("  token size    : %d bytes" % header["token_size"])
    line("  token entries : %d" % header["token_entries"])
    line("  checksum      : 0x%02x  (header bytes sum to 0x%02x mod 256 -> %s)"
         % (header["checksum"], header["checksum_sum"],
            "VALID" if header["checksum_valid"] else "INVALID"))
    line()

    tokens = parse_bit_tokens(rom, header)
    line("  %-4s %-3s %-5s %-6s %-6s %s"
         % ("id", "chr", "ver", "len", "ptr", "name"))
    for token in tokens:
        flag = "" if token["payload_in_range"] or not token["length"] \
            else "  <- PAYLOAD OUT OF RANGE"
        line("  0x%02x %-3s %-5d %-6d 0x%04x %s%s"
             % (token["id"], token["char"], token["version"], token["length"],
                token["pointer"], token["name"], flag))
    line("  (%d of %d declared entries decoded)"
         % (len(tokens), header["token_entries"]))
    line()

    version = parse_bios_version(rom, tokens)
    if version:
        line("=== BIOS version (BIT token 0x42) ===")
        line("  raw u32       : 0x%08x, oem byte 0x%02x"
             % (version["raw"], version["oem"]))
        line("  formatted     : %s" % version["formatted"])
        line()

    strings = parse_string_table(rom, tokens)
    if strings:
        line("=== string table (BIT token 0x53) ===")
        for entry in strings:
            if entry["usable"]:
                line("  slot %-2d ptr 0x%04x len %-3d %r"
                     % (entry["slot"], entry["pointer"], entry["length"],
                        entry["text"]))
            else:
                line("  slot %-2d ptr 0x%04x len %-3d (unusable)"
                     % (entry["slot"], entry["pointer"], entry["length"]))
        line()

    # ---- the falcon question ----
    falcon = falcon_table_status(rom, tokens)
    line("=== falcon ucode table (BIT token 0x70) ===")
    token = falcon["token"]
    if token is not None:
        line("  token         : version %d, %d bytes at 0x%04x"
             % (token["version"], token["length"], token["pointer"]))
        line("  payload bytes : %s" % rom[token["pointer"]:
                                         token["pointer"] + token["length"]
                                         ].hex(" "))
    line("  state         : %s" % falcon["state"])
    line("  detail        : %s" % falcon["detail"])
    if falcon["state"] == "off_file":
        line("  pointer       : 0x%06x (%d)"
             % (falcon["pointer"], falcon["pointer"]))
        line("  dump covers   : 0x000000 .. 0x%06x (%d bytes)"
             % (len(rom), len(rom)))
        line("  needs a ROM of: >= %d bytes (%.0f KiB)"
             % (falcon["needs_at_least"], falcon["needs_at_least"] / 1024.0))
        line("  shortfall     : %d bytes" % falcon["shortfall"])
    elif falcon["state"] == "present":
        table = falcon["table"]
        line("  table header  : version %d, header %d B, entry %d B, "
             "%d entries, desc v%d/%d B"
             % (table["version"], table["header_size"], table["entry_size"],
                table["entry_count"], table["desc_version"],
                table["desc_size"]))
    line()

    line("=== self-describing container magics ===")
    for entry in scan_container_magics(rom):
        if entry["offsets"]:
            line("  %-38s: %s"
                 % (entry["label"],
                    ", ".join("0x%06x" % o for o in entry["offsets"][:8])))
        else:
            line("  %-38s: absent" % entry["label"])
    line()

    if show_scan_hits:
        line("=== raw signature scan (false positives included) ===")
        for hit in scan_signatures(rom, images):
            line("  %-4s 0x%06x  chain_reached=%-5s block_aligned=%s"
                 % (hit["signature"], hit["offset"], hit["chain_reached"],
                    hit["block_aligned"]))
        line("  A hit with chain_reached=False is NOT a structure. In an NVIDIA")
        line("  VBIOS these are immediate operands in the ROM's own image-walk")
        line("  code -- `66 3d 50 43 49 52` is `cmp eax, 'PCIR'`.")
        line()

    found = extract_strings(rom, string_length)
    line("=== printable strings >= %d chars (%d found) ==="
         % (string_length, len(found)))
    for entry in found:
        line("  0x%06x  %s" % (entry["offset"], entry["text"]))
    line()

    # ---- verdict ----
    line("=== VERDICT ===")
    pcir0 = images[0]["pcir"]
    line("  %d image(s) walked; image 0 is %04x:%04x, class %s, %d bytes."
         % (usable_images, pcir0["vendor_id"], pcir0["device_id"],
            pci_class_code(pcir0), pcir0["image_length"]))
    if falcon["state"] == "off_file":
        line("  Falcon ucode table: NOT IN THIS DUMP. The pointer is well-formed")
        line("  and lands %d bytes past the end, so the table is not absent from"
             % falcon["shortfall"])
        line("  the ROM -- it is outside the image that was dumped. Recovering it")
        line("  needs a full-ROM read, not a re-parse of this file.")
    elif falcon["state"] == "null":
        line("  Falcon ucode table: the ROM declares none (NULL pointer).")
    elif falcon["state"] == "present":
        line("  Falcon ucode table: present at 0x%06x and decoded."
             % falcon["pointer"])
    else:
        line("  Falcon ucode table: undecidable -- %s" % falcon["detail"])
    if not any(e["offsets"] for e in scan_container_magics(rom)):
        line("  No nvfw/NVGI/NVDA container magic anywhere in the file, so this")
        line("  image carries no self-describing firmware blob either.")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Decode an NVIDIA VBIOS: ROM image chain, BIT table, "
                    "string table, falcon ucode table pointer.")
    parser.add_argument("rom", metavar="ROM",
                        help="VBIOS image, e.g. "
                             "reference/ga107-firmware/vbios-ga107.rom")
    parser.add_argument("--strings", type=int, default=12, metavar="N",
                        help="minimum length for the printable-string dump "
                             "(default 12)")
    parser.add_argument("--scan-hits", action="store_true",
                        help="also list raw PCIR/NPDE/NPDS signature hits, "
                             "including the x86-immediate false positives")
    args = parser.parse_args(argv)

    path = Path(args.rom)
    if not path.is_file():
        print("error: no such file: %s" % path, file=sys.stderr)
        return 1

    rom = path.read_bytes()
    if not rom:
        print("error: %s is empty" % path, file=sys.stderr)
        return 1

    return report(rom, str(path), show_scan_hits=args.scan_hits,
                  string_length=args.strings)


if __name__ == "__main__":
    sys.exit(main())
