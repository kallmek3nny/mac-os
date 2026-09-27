#!/usr/bin/env python3
"""_vbios-explore.py -- throwaway probe: locate the BIT table in a VBIOS image.

Ground truth gathering for t-08cd68. Prints nothing but measured bytes so the
real parser (parse-vbios.py) is written against facts, not against what the
spec says should be there.
"""

import struct
import sys
from pathlib import Path

rom = Path(sys.argv[1]).read_bytes()
print("size            : %d bytes (0x%x)" % (len(rom), len(rom)))
print("first 4 bytes   : %s" % rom[:4].hex())
print("first u32 LE    : 0x%08x" % struct.unpack_from("<I", rom, 0)[0])

# PCI expansion ROM header: 0x55AA at 0, pointer to PCIR at offset 0x18.
pcir_ptr = struct.unpack_from("<H", rom, 0x18)[0]
print("PCIR pointer@18 : 0x%04x (%d)" % (pcir_ptr, pcir_ptr))

for name in (b"PCIR", b"NPDE", b"NPDS", b"BIT\x00", b"\xff\xb8BIT\x00"):
    hits = []
    start = 0
    while True:
        i = rom.find(name, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
        if len(hits) > 8:
            break
    print("%-16s: %s" % (repr(name), [hex(h) for h in hits]))

# The BIT header per envytools / open-gpu-kernel-modules:
#   u16 id = 0xB8FF, char sig[4] = "BIT\0", u16 bcd_version,
#   u8 header_size, u8 token_size, u8 token_entries, u8 checksum
sig = rom.find(b"\xff\xb8BIT\x00")
if sig < 0:
    print("\nNO BIT SIGNATURE FOUND")
    sys.exit(0)

base = sig
bcd, hdr_size, tok_size, tok_entries, cksum = struct.unpack_from("<HBBBB", rom, base + 6)
print("\nBIT header at 0x%x" % base)
print("  bcd_version   : 0x%04x" % bcd)
print("  header_size   : %d" % hdr_size)
print("  token_size    : %d" % tok_size)
print("  token_entries : %d" % tok_entries)
print("  checksum      : 0x%02x" % cksum)

print("\n  raw header bytes: %s" % rom[base:base + hdr_size].hex())

print("\n  %-4s %-4s %-6s %-8s %s" % ("id", "ver", "len", "ptr", "chr"))
tbl = base + hdr_size
for n in range(tok_entries):
    off = tbl + n * tok_size
    tid, tver, tlen, tptr = struct.unpack_from("<BBHH", rom, off)
    ch = chr(tid) if 32 <= tid < 127 else "."
    inrange = "" if tptr < len(rom) else "   <- OUT OF RANGE"
    print("  0x%02x %-4d %-6d 0x%04x   %s%s" % (tid, tver, tlen, tptr, ch, inrange))

# checksum: bytes of the header should sum to 0 mod 256
print("\n  header byte sum mod 256: 0x%02x" % (sum(rom[base:base + hdr_size]) & 0xFF))
