#!/usr/bin/env python3
"""_vbios-explore5.py -- throwaway probe #5 for t-08cd68.

Probe #4 established, with a hexdump-checked PCIR decode, that image 0 declares
127 x 512 = 65024 bytes -- EXACTLY the file length -- and that its indicator
byte at +0x15 is 0x80, i.e. "last image".

That matters more than it looks. Linux's pci_get_rom_size() walks this very
chain and stops on that bit, so a `dd` of /sys/.../rom can only ever return
65024 bytes for this card. The dump is therefore complete AS AN IMAGE while
possibly being only the FIRST image of a physically larger ROM.

This probe collects the bytes needed to decide that:

  * NPDE at 0x190  -- NVIDIA's extension; nouveau reads image_size at +0x08
    (u16, 512-byte blocks) and the last-image flag at +0x0a bit 7. If NPDE says
    last=NO while the legacy PCIR says last=YES, more images exist off-file.
  * the PCIR / NPDS / NPDE cluster at 0xf37d..0xf3b4, near the image tail.
  * the raw 4 bytes behind BIT token 0x70 (FALCON_DATA), so the 0x00077f77
    falcon-ucode-table offset is confirmed from bytes rather than from an
    earlier struct.unpack that has already been wrong once.
  * BIT token 0x53 ('S') string table, to cross-check the version strings that
    reference/ga107-firmware/README.md quotes.
  * absence checks for the container magics a VBIOS-embedded ucode would carry.

Measured bytes only.
"""

import hashlib
import struct
import sys
from pathlib import Path

rom = Path(sys.argv[1]).read_bytes()
print("file      : %s" % sys.argv[1])
print("size      : %d bytes (0x%x)" % (len(rom), len(rom)))
print("md5       : %s" % hashlib.md5(rom).hexdigest())
print("sha256    : %s" % hashlib.sha256(rom).hexdigest())
print("")


def dump(a, n, label):
    print(label)
    for i in range(a, min(a + n, len(rom)), 16):
        c = rom[i:i + 16]
        asc = "".join(chr(x) if 32 <= x < 127 else "." for x in c)
        print("  %06x  %-47s  %s" % (i, c.hex(" "), asc))
    print("")


dump(0x170, 0x40, "=== PCIR @0x170 + NPDE @0x190 ===")
dump(0xf370, 0x60, "=== tail cluster: PCIR @0xf37d / NPDS @0xf385 / NPDE @0xf3b4 ===")

print("=== NPDE @0x190 decoded (nouveau nvbios_npdeTp layout) ===")
npde = 0x190
print("  signature      : %s" % rom[npde:npde + 4])
print("  u16 @+0x04     : 0x%04x" % struct.unpack_from("<H", rom, npde + 4)[0])
print("  u16 @+0x06     : 0x%04x" % struct.unpack_from("<H", rom, npde + 6)[0])
blocks = struct.unpack_from("<H", rom, npde + 8)[0]
print("  image_size @+08: %d blocks = %d bytes (0x%x)"
      % (blocks, blocks * 512, blocks * 512))
last = rom[npde + 0x0A]
print("  last @+0x0a    : 0x%02x  -> last_image=%s"
      % (last, "YES" if last & 0x80 else "NO"))
print("")

print("=== NPDE @0xf3b4 decoded (same layout) ===")
npde2 = 0xf3b4
if rom[npde2:npde2 + 4] == b"NPDE":
    b2 = struct.unpack_from("<H", rom, npde2 + 8)[0]
    l2 = rom[npde2 + 0x0A]
    print("  image_size     : %d blocks = %d bytes" % (b2, b2 * 512))
    print("  last           : 0x%02x -> %s" % (l2, "YES" if l2 & 0x80 else "NO"))
else:
    print("  not an NPDE: %s" % rom[npde2:npde2 + 4].hex(" "))
print("")

# ---- BIT tokens, re-read so this probe stands alone ----------------------
base = rom.find(b"\xff\xb8BIT\x00")
hdr_size = rom[base + 8]
tok_size = rom[base + 9]
tok_entries = rom[base + 10]
tokens = {}
for n in range(tok_entries):
    off = base + hdr_size + n * tok_size
    tid, tver, tlen, tptr = struct.unpack_from("<BBHH", rom, off)
    tokens[tid] = (tver, tlen, tptr)

print("=== BIT token 0x70 'p' FALCON_DATA -- raw bytes ===")
if 0x70 in tokens:
    tver, tlen, tptr = tokens[0x70]
    print("  token: ver=%d len=%d ptr=0x%04x" % (tver, tlen, tptr))
    print("  payload bytes  : %s" % rom[tptr:tptr + tlen].hex(" "))
    val = struct.unpack_from("<I", rom, tptr)[0]
    print("  u32 LE         : 0x%08x (%d)" % (val, val))
    print("  in this file?  : %s  (file ends at 0x%06x)"
          % (val < len(rom), len(rom)))
    print("  plausible for  : 128KB=%s 256KB=%s 512KB=%s 1MB=%s"
          % (val < 0x20000, val < 0x40000, val < 0x80000, val < 0x100000))
print("")

print("=== BIT token 0x53 'S' string table -- payload + dereference ===")
if 0x53 in tokens:
    tver, tlen, tptr = tokens[0x53]
    pay = rom[tptr:tptr + tlen]
    print("  ver=%d len=%d ptr=0x%04x" % (tver, tlen, tptr))
    print("  payload        : %s" % pay.hex(" "))
    # layout is a run of (u16 ptr, u8 len) triples
    for i in range(0, tlen - 2, 3):
        sptr, slen = struct.unpack_from("<HB", pay, i)
        if sptr and sptr + slen <= len(rom):
            raw = rom[sptr:sptr + slen]
            txt = "".join(chr(c) if 32 <= c < 127 else "." for c in raw)
            print("    +%02d ptr=0x%04x len=%-3d %r" % (i, sptr, slen, txt))
        else:
            print("    +%02d ptr=0x%04x len=%-3d (unusable)" % (i, sptr, slen))
print("")

print("=== BIT token 0x42 'B' BIOSDATA version cross-check ===")
if 0x42 in tokens:
    _, tlen, tptr = tokens[0x42]
    ver = struct.unpack_from("<I", rom, tptr)[0]
    oem = rom[tptr + 4]
    print("  raw u32        : 0x%08x" % ver)
    print("  as NVIDIA ver  : %02X.%02X.%02X.%02X.%02X"
          % ((ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF,
             ver & 0xFF, oem))
    print("  README quotes  : Version 94.07.4E.00.4B")
print("")

print("=== container-magic absence checks ===")
for magic, label in ((struct.pack("<I", 0x10DE), "nvfw bin_magic 0x000010de LE"),
                     (b"\x9a\x99\x99\x99", "nvfw alt magic"),
                     (b"NVGI", "NVGI"),
                     (b"NVDA", "NVDA")):
    i = rom.find(magic)
    print("  %-30s: %s" % (label, ("first hit 0x%06x" % i) if i >= 0 else "absent"))
print("")

print("=== printable strings >= 12 chars ===")
cur, start = [], 0
found = 0
for i, c in enumerate(rom):
    if 32 <= c < 127:
        if not cur:
            start = i
        cur.append(chr(c))
    else:
        if len(cur) >= 12:
            print("  0x%06x  %s" % (start, "".join(cur)))
            found += 1
        cur = []
if len(cur) >= 12:
    print("  0x%06x  %s" % (start, "".join(cur)))
    found += 1
print("  (%d strings)" % found)
