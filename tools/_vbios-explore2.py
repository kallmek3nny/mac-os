#!/usr/bin/env python3
"""_vbios-explore2.py -- throwaway probe #2 for t-08cd68.

Probe #1 located the BIT header at 0x1b0 and listed 17 tokens. This one dumps
each token's payload bytes and dereferences the two that matter for Phase 4:

  0x42 'B'  BIT_TOKEN_BIOSDATA   -- version, build date, board id
  0x70 'p'  BIT_TOKEN_FALCON_DATA -- pointer to the falcon ucode table, which
            reference/ga107-firmware/README.md claims is ABSENT from this image

Again: prints measured bytes only. No interpretation is baked in yet.
"""

import struct
import sys
from pathlib import Path

rom = Path(sys.argv[1]).read_bytes()
base = rom.find(b"\xff\xb8BIT\x00")
bcd, hdr_size, tok_size, tok_entries, cksum = struct.unpack_from("<HBBBB", rom, base + 6)
tbl = base + hdr_size

print("rom size    : %d (0x%x)" % (len(rom), len(rom)))
print("BIT base    : 0x%x  entries=%d tok_size=%d" % (base, tok_entries, tok_size))
print("")

tokens = {}
for n in range(tok_entries):
    off = tbl + n * tok_size
    tid, tver, tlen, tptr = struct.unpack_from("<BBHH", rom, off)
    tokens[tid] = (tver, tlen, tptr)
    ch = chr(tid) if 32 <= tid < 127 else "."
    payload = rom[tptr:tptr + tlen] if tlen and tptr + tlen <= len(rom) else b""
    print("token 0x%02x '%s' ver=%d len=%d ptr=0x%04x" % (tid, ch, tver, tlen, tptr))
    if payload:
        for i in range(0, len(payload), 16):
            chunk = payload[i:i + 16]
            asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
            print("    +%03x  %-47s  %s" % (i, chunk.hex(" "), asc))
    print("")

# ---- BIOSDATA ('B', 0x42) ------------------------------------------------
# nvbios BIT_TOKEN_BIOSDATA v2: u32 bios_version, u8 bios_oem_version, ...
if 0x42 in tokens:
    _, tlen, tptr = tokens[0x42]
    ver = struct.unpack_from("<I", rom, tptr)[0]
    oem = rom[tptr + 4]
    print("BIOSDATA:")
    print("  bios_version raw : 0x%08x" % ver)
    print("  bios_version dot : %d.%d.%d.%d" % (
        (ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF, ver & 0xFF))
    print("  oem_version      : 0x%02x" % oem)
    print("")

# ---- FALCON_DATA ('p', 0x70) ---------------------------------------------
# v2 payload is a single u32: pointer to the FALCON_UCODE_TABLE.
if 0x70 in tokens:
    tver, tlen, tptr = tokens[0x70]
    print("FALCON_DATA token: ver=%d len=%d ptr=0x%04x" % (tver, tlen, tptr))
    if tlen >= 4:
        ftbl = struct.unpack_from("<I", rom, tptr)[0]
        print("  falcon ucode table ptr : 0x%08x (%d)" % (ftbl, ftbl))
        if ftbl == 0:
            print("  -> NULL: no falcon ucode table in this image")
        elif ftbl >= len(rom):
            print("  -> OUT OF RANGE for a %d-byte image" % len(rom))
        else:
            hdr = rom[ftbl:ftbl + 16]
            print("  header bytes @0x%x : %s" % (ftbl, hdr.hex(" ")))
            v, hlen, elen, ecount, dpt = struct.unpack_from("<BBBBB", rom, ftbl)
            print("    version       : %d" % v)
            print("    header_size   : %d" % hlen)
            print("    entry_size    : %d" % elen)
            print("    entry_count   : %d" % ecount)
            print("    desc_version  : %d" % dpt)
    print("")

# ---- absence check -------------------------------------------------------
# open-gpu-kernel-modules looks for these container magics when it expects
# to find ucode inside a VBIOS image. README.md says they are not here.
for magic, label in ((b"\x10\xde\x99\x99", "nvfw_bin_hdr magic 0x9999de10 LE"),
                     (b"NVGI", "NVGI"),
                     (b"\x9a\x99\x99\x99", "decoy")):
    print("%-34s: %s" % (label, [hex(i) for i in _find_all(rom, magic)] if False else
                         [hex(rom.find(magic))] if rom.find(magic) >= 0 else "absent"))
