#!/usr/bin/env python3
"""parse-dcb.py -- decode the Display Configuration Block out of an NVIDIA VBIOS.

Why this exists
---------------
`tools/parse-vbios.py` decodes the PCI ROM image chain, the BIT table, the string
table and the falcon ucode table pointer. It says nothing about DISPLAY. The DCB
is the table that enumerates this board's display paths -- which encoder drives
which connector, on which output pins -- and it is the only artifact in this tree
that bears on whether the GA107 can reach a physical port at all.

That question is the one the muxless finding (3050-handoff27 §1) makes decisive.
All internal-panel graphics route through the Intel chip, so `PLAN.md`
termination condition A is SATISFIED and the internal display is unreachable from
this chip no matter how complete the driver gets. HDMI was named as the only
bypass -- and "named" is where it has stood. This decodes the board's own table
to find out whether that bypass is real, and what it is wired to.

Provenance of the layout
------------------------
The header layout is NOT invented here. It is
`reference/open-gpu-kernel-modules/src/common/nvswitch/kernel/inc/rom_nvswitch.h`
`_nvswitch_vbios_dcb_header_41` (`:836`-`:855`), whose format string "4b1w1d6w1b2w2d"
pins every field width, plus `NVSWITCH_DCB_PTR_OFFSET 0x36` (`:834`) for the
pointer. The signature constant and the entry bit-fields are read out of
`reference/open-gpu-kernel-modules` as well -- see CITED_PATHS below, which the
Python gate resolves.

What this does NOT establish
----------------------------
A DCB entry is a description of BOARD WIRING as the vendor's firmware states it.
It is not a measurement of a live link, it does not prove the port is populated,
and on a muxless laptop it does not prove macOS can be made to drive it. A
connector this table names may still be routed to the Intel chip's outputs at the
board level. Read every verdict here as "the firmware says", never "the machine
does".
"""

import os
import struct
import sys

# Paths this file cites, resolved by tests/test_source_cited_paths.py.
#   reference/open-gpu-kernel-modules/src/common/nvswitch/kernel/inc/rom_nvswitch.h
#   reference/ga107-firmware/vbios-ga107.rom
#   tools/parse-vbios.py

DCB_PTR_OFFSET = 0x36          # rom_nvswitch.h:834, NVSWITCH_DCB_PTR_OFFSET
DCB_SIGNATURE = 0x4EDCBDCB     # the version-4x DCB signature

# _nvswitch_vbios_dcb_header_41, rom_nvswitch.h:836-855, fmt "4b1w1d6w1b2w2d".
# name, offset, width in bytes
DCB_HEADER_FIELDS = (
    ("version",          0x00, 1),
    ("header_size",      0x01, 1),
    ("entry_count",      0x02, 1),
    ("entry_size",       0x03, 1),
    ("ccb_block_ptr",    0x04, 2),
    ("dcb_signature",    0x06, 4),
    ("gpio_table",       0x0A, 2),
    ("input_devices",    0x0C, 2),
    ("personal_cinema",  0x0E, 2),
    ("spread_spectrum",  0x10, 2),
    ("i2c_devices",      0x12, 2),
    ("connectors",       0x14, 2),
    ("flags",            0x16, 1),
    ("hdtv",             0x17, 2),
    ("switched_outputs", 0x19, 2),
    ("display_patch",    0x1B, 4),
    ("connector_patch",  0x1F, 4),
)
DCB_HEADER_41_SIZE = 0x23      # sum of the widths above

# Display type, entry word 0 bits 3:0.
DCB_TYPE_NAMES = {
    0x0: "CRT",
    0x1: "TV",
    0x2: "TMDS",          # DVI / HDMI
    0x3: "LVDS",          # internal panel
    0x4: "reserved 0x4",
    0x5: "SDI",
    0x6: "DisplayPort",
    0x7: "reserved 0x7",
    0x8: "reserved 0x8",
    0x9: "reserved 0x9",
    0xA: "reserved 0xA",
    0xB: "reserved 0xB",
    0xC: "reserved 0xC",
    0xD: "reserved 0xD",
    0xE: "EOL",           # end of the entry list
    0xF: "SKIP",          # entry present but not to be used
}

# The two type values that can reach an EXTERNAL port on a laptop.
EXTERNAL_TYPES = (0x2, 0x6)

# Connector type, from the connector table's own entries.
CONNECTOR_TYPE_NAMES = {
    0x00: "VGA 15-pin",
    0x01: "DVI-A",
    0x02: "pod VGA 15-pin",
    0x10: "TV composite out",
    0x11: "TV S-Video out",
    0x12: "TV S-Video breakout composite",
    0x13: "TV HDTV component",
    0x14: "TV SCART",
    0x16: "TV composite SCART over blue",
    0x17: "TV HDTV EIAJ4120",
    0x18: "pod HDTV YPrPb",
    0x19: "pod composite",
    0x20: "DVI-I TV S-Video",
    0x21: "DVI-I TV composite",
    0x30: "DVI-I",
    0x31: "DVI-D",
    0x32: "App Dual-Link DVI-D",
    0x38: "DVI-D plus DVI-D",
    0x39: "DVI-I plus DVI-I",
    0x3A: "HDMI-A",
    0x3B: "LFH-DVI-I 1",
    0x3C: "LFH-DVI-I 2",
    0x3D: "BNC",
    0x40: "LVDS SPWG",
    0x41: "LVDS OEM",
    0x42: "LVDS SPWG detached",
    0x43: "LVDS OEM detached",
    0x45: "TMDS OEM",
    0x46: "DisplayPort external",
    0x47: "DisplayPort internal",
    0x48: "DisplayPort external detached",
    0x49: "DisplayPort internal detached",
    0x50: "eDP",
    0x51: "eDP detached",
    0x60: "3-pin DIN stereo",
    0x61: "HDMI-C mini",
    0x63: "LFH-DP 1",
    0x64: "LFH-DP 2",
    0x70: "virtual WFD",
    0xFF: "skip entry",
}


class DcbError(Exception):
    """The image does not carry a DCB this decoder can read."""


def read_u(blob, offset, width):
    """Read a little-endian unsigned integer of 1, 2 or 4 bytes."""
    if width == 1:
        return blob[offset]
    if width == 2:
        return struct.unpack_from("<H", blob, offset)[0]
    if width == 4:
        return struct.unpack_from("<I", blob, offset)[0]
    raise ValueError("unsupported width %d" % width)


def find_dcb(rom):
    """Return the DCB header offset, refusing rather than guessing.

    The pointer at 0x36 is the only sanctioned way in (rom_nvswitch.h:834). A
    signature scan is deliberately NOT a fallback: a 32-bit constant occurs in a
    64 KiB image by chance often enough that finding one proves nothing, and a
    decoder that lands somewhere plausible reports fabricated wiring.
    """
    if len(rom) < DCB_PTR_OFFSET + 2:
        raise DcbError("image is %d bytes, too short to hold the DCB pointer"
                       % len(rom))
    ptr = read_u(rom, DCB_PTR_OFFSET, 2)
    if ptr == 0:
        raise DcbError("DCB pointer at 0x%02x is zero" % DCB_PTR_OFFSET)
    if ptr + DCB_HEADER_41_SIZE > len(rom):
        raise DcbError(
            "DCB pointer 0x%04x plus a 0x%02x-byte header runs past the "
            "%d-byte image" % (ptr, DCB_HEADER_41_SIZE, len(rom)))
    sig = read_u(rom, ptr + 0x06, 4)
    if sig != DCB_SIGNATURE:
        raise DcbError(
            "signature at 0x%04x is 0x%08x, not the expected 0x%08x"
            % (ptr + 6, sig, DCB_SIGNATURE))
    return ptr


def parse_header(rom, base):
    """Decode the version-4x DCB header into a dict."""
    header = {}
    for name, offset, width in DCB_HEADER_FIELDS:
        header[name] = read_u(rom, base + offset, width)
    header["_base"] = base
    return header


def decode_entry(word0, word1):
    """Decode one DCB display-path entry.

    Bit assignments are the version-4x layout: type in 3:0, edid port in 7:4,
    head mask in 11:8, connector index in 15:12, bus in 19:16, location in 21:20,
    boot device in 22, and the output-resource / link fields in word 1.
    """
    dtype = word0 & 0xF
    return {
        "type": dtype,
        "type_name": DCB_TYPE_NAMES.get(dtype, "unknown 0x%x" % dtype),
        "edid_port": (word0 >> 4) & 0xF,
        "head_mask": (word0 >> 8) & 0xF,
        "connector": (word0 >> 12) & 0xF,
        "bus": (word0 >> 16) & 0xF,
        "location": (word0 >> 20) & 0x3,
        "boot_device": (word0 >> 22) & 0x1,
        "or": (word1 >> 24) & 0xF,
        "link": (word1 >> 30) & 0x3,
        "word0": word0,
        "word1": word1,
    }


def parse_entries(rom, header):
    """Walk the display-path entry array.

    Stops at the header's own entry_count, and reports an EOL entry as a fact
    rather than treating it as the count -- a count and a terminator disagreeing
    is itself a finding.
    """
    base = header["_base"] + header["header_size"]
    size = header["entry_size"]
    count = header["entry_count"]
    if size < 8:
        raise DcbError("entry_size %d cannot hold two 32-bit words" % size)
    entries = []
    for index in range(count):
        offset = base + index * size
        if offset + 8 > len(rom):
            raise DcbError(
                "entry %d at 0x%04x runs past the %d-byte image"
                % (index, offset, len(rom)))
        word0, word1 = struct.unpack_from("<II", rom, offset)
        entry = decode_entry(word0, word1)
        entry["index"] = index
        entry["offset"] = offset
        entries.append(entry)
    return entries


def parse_connectors(rom, header):
    """Decode the connector table the header points at, or return None.

    An absent or out-of-range pointer returns None rather than raising: the
    display-path entries are the finding, and the connector table is
    corroboration. Refusing the whole decode over a secondary table would throw
    away the primary one.
    """
    ptr = header.get("connectors", 0)
    if ptr == 0 or ptr + 4 > len(rom):
        return None
    version, hsize, count, esize = rom[ptr:ptr + 4]
    if esize == 0 or ptr + hsize + count * esize > len(rom):
        return None
    rows = []
    for index in range(count):
        offset = ptr + hsize + index * esize
        raw = int.from_bytes(rom[offset:offset + esize], "little")
        ctype = raw & 0xFF
        rows.append({
            "index": index,
            "offset": offset,
            "type": ctype,
            "type_name": CONNECTOR_TYPE_NAMES.get(
                ctype, "unknown 0x%02x" % ctype),
            "raw": raw,
        })
    return {
        "pointer": ptr,
        "version": version,
        "header_size": hsize,
        "entry_count": count,
        "entry_size": esize,
        "rows": rows,
    }


def check_image_chain(rom):
    """Walk the PCI expansion ROM image chain and report truncation.

    This exists because of the zero case below. A DCB reporting no display paths
    and a DCB dump that stops before its own display paths are the SAME reading,
    and only one of them is a fact about the board. The chain carries its own
    length in 512-byte units at PCIR+0x10 and its own terminator bit at +0x15, so
    the image can be checked against the file rather than assumed complete.
    """
    images = []
    off = 0
    while off + 0x1A <= len(rom):
        if rom[off:off + 2] != b"\x55\xAA":
            images.append({"offset": off, "error": "no 0x55AA signature"})
            break
        pcir_ptr = read_u(rom, off + 0x18, 2)
        pcir = off + pcir_ptr
        if pcir + 0x18 > len(rom) or rom[pcir:pcir + 4] != b"PCIR":
            images.append({"offset": off, "error": "PCIR unreadable"})
            break
        length = read_u(rom, pcir + 0x10, 2) * 512
        indicator = rom[pcir + 0x15]
        images.append({
            "offset": off,
            "pcir": pcir,
            "vendor": read_u(rom, pcir + 0x04, 2),
            "device": read_u(rom, pcir + 0x06, 2),
            "length": length,
            "code_type": rom[pcir + 0x14],
            "last": bool(indicator & 0x80),
            "complete": off + length <= len(rom),
        })
        if images[-1]["last"] or length == 0:
            break
        off += length
    truncated = any(i.get("complete") is False for i in images)
    terminated = any(i.get("last") for i in images)
    return {
        "images": images,
        "truncated": truncated,
        "terminated": terminated,
        "complete": terminated and not truncated,
    }


def subtable_counts(rom, header):
    """Read the (version, header_size, entry_count, entry_size) quartet of each
    sibling sub-table the DCB header points at.

    Every DCB sub-table opens with that same four-byte quartet, so the siblings
    can be counted without knowing any of their entry layouts. They are the
    corroboration for a zero display-path count: a decoder that landed on the
    wrong offset reads garbage counts out of ALL of them, while a board that
    genuinely wires no display outputs still has a populated GPIO and I2C table.
    """
    rows = []
    for name in ("ccb_block_ptr", "gpio_table", "i2c_devices", "connectors",
                 "spread_spectrum"):
        ptr = header.get(name, 0)
        if ptr == 0:
            rows.append({"name": name, "pointer": 0, "readable": False,
                         "note": "absent"})
            continue
        if ptr + 4 > len(rom):
            rows.append({"name": name, "pointer": ptr, "readable": False,
                         "note": "past end of image"})
            continue
        version, hsize, count, esize = rom[ptr:ptr + 4]
        rows.append({
            "name": name,
            "pointer": ptr,
            "readable": True,
            "version": version,
            "header_size": hsize,
            "entry_count": count,
            "entry_size": esize,
            "well_formed": (version in range(0x30, 0x50)
                            and 0 < hsize <= 0x40 and 0 < esize <= 0x40),
        })
    return rows


def signature_occurrences(rom):
    """Count occurrences of the DCB signature in the whole image.

    Exactly one is the reading that makes the pointer at 0x36 trustworthy. More
    than one means the pointer chose among candidates and the choice needs
    stating; zero is impossible here, since find_dcb() already matched one.
    """
    needle = struct.pack("<I", DCB_SIGNATURE)
    hits = []
    index = rom.find(needle)
    while index != -1:
        hits.append(index - 6)
        index = rom.find(needle, index + 1)
    return hits


def corroborate_absence(rom, header, entries):
    """Decide whether a zero/absent display-path count is AUTHORED or a misread.

    An absence is the one finding a decoder cannot report honestly on its own
    evidence. Landing one field off, on the wrong table, or on a truncated dump
    all produce "no display paths" as convincingly as a board that has none. So
    when the live path count is zero this collects the positive evidence that the
    decode landed where it claims, and reports AUTHORED only when every arm
    agrees.
    """
    live = [e for e in entries if e["type"] not in (0xE, 0xF)]
    if live:
        return None
    chain = check_image_chain(rom)
    hits = signature_occurrences(rom)
    siblings = subtable_counts(rom, header)
    populated = [s for s in siblings
                 if s["readable"] and s.get("entry_count", 0) > 0
                 and s.get("well_formed")]
    arms = [
        ("image chain complete (not truncated)", chain["complete"]),
        ("signature occurs exactly once in the image", len(hits) == 1),
        ("pointer at 0x%02x selected that one header" % DCB_PTR_OFFSET,
         bool(hits) and hits[0] == header["_base"]),
        ("header version is a known DCB version", header["version"] in
         range(0x30, 0x50)),
        ("header_size matches the version-4x layout",
         header["header_size"] == DCB_HEADER_41_SIZE),
        ("entry_size holds two 32-bit words", header["entry_size"] >= 8),
        ("at least two sibling sub-tables are populated", len(populated) >= 2),
    ]
    return {
        "arms": arms,
        "authored": all(ok for _, ok in arms),
        "chain": chain,
        "signature_hits": hits,
        "siblings": siblings,
        "populated": populated,
    }


def report(rom, path):
    """Produce the full text report as a list of lines."""
    out = []
    add = out.append
    add("file            : %s" % path)
    add("size            : %d bytes (0x%x)" % (len(rom), len(rom)))
    add("")

    base = find_dcb(rom)
    header = parse_header(rom, base)

    add("=== DCB header ===")
    add("pointer at 0x%02x -> 0x%04x" % (DCB_PTR_OFFSET, base))
    for name, offset, width in DCB_HEADER_FIELDS:
        value = header[name]
        note = ""
        if width == 2 and name not in ("ccb_block_ptr",):
            if value == 0:
                note = "  (absent)"
            elif value < len(rom):
                note = "  (offset in image)"
            else:
                note = "  (PAST END OF IMAGE)"
        add("  %-16s +0x%02x  0x%0*x%s"
            % (name, offset, width * 2, value, note))
    add("")

    entries = parse_entries(rom, header)
    add("=== display path entries (%d, %d bytes each) ==="
        % (header["entry_count"], header["entry_size"]))
    add("  idx  offset  word0     word1     type         head or link boot")
    for e in entries:
        add("  %3d  0x%04x  %08x  %08x  %-12s %#04x %#3x %#4x %d"
            % (e["index"], e["offset"], e["word0"], e["word1"],
               e["type_name"], e["head_mask"], e["or"], e["link"],
               e["boot_device"]))
    add("")

    conns = parse_connectors(rom, header)
    if conns is None:
        add("=== connector table ===")
        add("  ABSENT or unreadable -- pointer 0x%04x"
            % header.get("connectors", 0))
    else:
        add("=== connector table @0x%04x (%d entries, %d bytes each) ==="
            % (conns["pointer"], conns["entry_count"], conns["entry_size"]))
        for row in conns["rows"]:
            add("  %3d  0x%04x  raw 0x%0*x  type 0x%02x  %s"
                % (row["index"], row["offset"], conns["entry_size"] * 2,
                   row["raw"], row["type"], row["type_name"]))
    add("")

    add("=== PCI expansion ROM image chain ===")
    chain = check_image_chain(rom)
    for img in chain["images"]:
        if "error" in img:
            add("  image @0x%06x: %s" % (img["offset"], img["error"]))
            continue
        add("  image @0x%06x  vendor 0x%04x device 0x%04x  code type %d"
            % (img["offset"], img["vendor"], img["device"], img["code_type"]))
        add("      length %d bytes, ends at 0x%06x, file ends at 0x%06x -> %s"
            % (img["length"], img["offset"] + img["length"], len(rom),
               "COMPLETE" if img["complete"] else "TRUNCATED"))
        add("      last-image indicator: %s" % img["last"])
    add("  chain verdict           : %s"
        % ("complete" if chain["complete"] else "INCOMPLETE"))
    add("")

    add("=== sibling sub-table counts ===")
    for row in subtable_counts(rom, header):
        if not row["readable"]:
            add("  %-16s 0x%04x  %s" % (row["name"], row["pointer"],
                                        row["note"]))
            continue
        add("  %-16s 0x%04x  version 0x%02x  entries %3d  entry_size %2d  %s"
            % (row["name"], row["pointer"], row["version"],
               row["entry_count"], row["entry_size"],
               "well formed" if row["well_formed"] else "MALFORMED"))
    add("")

    add("=== what this says about reaching a port ===")
    live = [e for e in entries if e["type"] not in (0xE, 0xF)]
    external = [e for e in live if e["type"] in EXTERNAL_TYPES]
    internal = [e for e in live if e["type"] == 0x3]
    add("  live display paths        : %d" % len(live))
    add("  external-capable (TMDS/DP): %d" % len(external))
    add("  internal panel (LVDS)     : %d" % len(internal))
    for e in external:
        add("    entry %d: %s, output resource %#x, connector index %d"
            % (e["index"], e["type_name"], e["or"], e["connector"]))
    add("")

    absence = corroborate_absence(rom, header, entries)
    if absence is not None:
        add("  THIS IMAGE DECLARES NO DISPLAY PATHS AT ALL.")
        add("  An absence is the one finding a decoder cannot report on its own")
        add("  evidence: landing one field off, on the wrong table, or on a")
        add("  truncated dump each produce this same reading. So each arm below is")
        add("  positive evidence that the decode landed where it claims.")
        for label, ok in absence["arms"]:
            add("    [%s] %s" % ("yes" if ok else " NO", label))
        add("  verdict                 : %s"
            % ("AUTHORED -- the zero is the board's own statement"
               if absence["authored"]
               else "UNPROVEN -- at least one arm failed, treat as a MISREAD"))
        add("  signature occurrences   : %s"
            % ", ".join("0x%04x" % h for h in absence["signature_hits"]))
        add("  populated siblings      : %s"
            % ", ".join("%s=%d" % (s["name"], s["entry_count"])
                        for s in absence["populated"]))
        add("")
        add("  What it does NOT mean: not that the chip has no display hardware.")
        add("  It means THIS BOARD's firmware describes no path from this chip to")
        add("  any connector -- which is what a muxless laptop looks like from the")
        add("  discrete GPU's side, and corroborates the muxless finding recorded")
        add("  in section 1 of 3050-handoff27.md from the GPU's own firmware.")
        add("  A driver cannot invent a path the board does not carry.")
        add("")

    add("  READ THIS AS FIRMWARE'S CLAIM ABOUT BOARD WIRING, NOT A MEASUREMENT.")
    add("  A named connector is not a populated port, and on a muxless laptop a")
    add("  path this table describes may still be routed to the Intel chip's")
    add("  outputs at the board level. Nothing here has been driven.")
    return out


def main(argv):
    if len(argv) > 1:
        path = argv[1]
    else:
        path = os.path.join("reference", "ga107-firmware", "vbios-ga107.rom")
    if not os.path.isfile(path):
        sys.stderr.write("no such image: %s\n" % path)
        return 2
    with open(path, "rb") as handle:
        rom = handle.read()
    try:
        lines = report(rom, path)
    except DcbError as exc:
        sys.stderr.write("DCB refused: %s\n" % exc)
        return 1
    sys.stdout.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
