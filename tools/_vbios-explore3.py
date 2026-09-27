#!/usr/bin/env python3
"""_vbios-explore3.py -- throwaway probe #3 for t-08cd68.

Probe #2 dereferenced BIT token 0x70 (FALCON_DATA) and got 0x00077f77, which is
beyond the end of this 65024-byte (0xfe00) file. Every other 32-bit pointer in
the token payloads sat in the same 0x06xxxx-0x07xxxx band. Two explanations:

  (a) the file is ONE PCI expansion-ROM image out of several, and the pointers
      are absolute offsets into the full concatenated ROM (~512KB or 1MB), or
  (b) the pointers are not byte offsets at all.

This probe tests (a) directly by walking the PCI ROM image chain: each image
starts with 55 aa, carries a PCIR at the offset stored in its u16 at +0x18, and
PCIR records the image length in 512-byte units plus a 'last image' indicator
bit. If the chain terminates at end-of-file with the last-image bit CLEAR, the
file is truncated and (a) is the answer.

Measured bytes only. No interpretation baked in.
"""

import struct
import sys
from pathlib import Path

rom = Path(sys.argv[1]).read_bytes()
print("file size : %d bytes (0x%x)" % (len(rom), len(rom)))
print("")


def find_all(buf, needle):
    out, start = [], 0
    while True:
        i = buf.find(needle, start)
        if i < 0:
            return out
        out.append(i)
        start = i + 1


print("all 55 aa occurrences at 512-byte boundaries:")
for i in find_all(rom, b"\x55\xaa"):
    if i % 512 == 0:
        print("  0x%06x" % i)
print("")

print("=== PCI expansion ROM image chain ===")
off = 0
image = 0
while off < len(rom) and image < 16:
    if rom[off:off + 2] != b"\x55\xaa":
        print("image %d @0x%06x : NO 55aa (bytes %s) -- chain broken"
              % (image, off, rom[off:off + 2].hex()))
        break
    pcir_rel = struct.unpack_from("<H", rom, off + 0x18)[0]
    pcir = off + pcir_rel
    if pcir + 24 > len(rom):
        print("image %d @0x%06x : PCIR ptr 0x%04x -> 0x%06x is past EOF"
              % (image, off, pcir_rel, pcir))
        break
    sig = rom[pcir:pcir + 4]
    (vendor, device, _rsvd, plen, prev, cls0, cls1, cls2,
     ilen512, crev, ctype, indicator) = struct.unpack_from(
        "<HHHHHBBBHHBB", rom, pcir + 4)
    ilen = ilen512 * 512
    print("image %d @0x%06x" % (image, off))
    print("  PCIR at        : 0x%06x (rel 0x%04x)  sig=%s" % (pcir, pcir_rel, sig))
    print("  vendor:device  : 0x%04x:0x%04x" % (vendor, device))
    print("  pcir struct len: %d   revision: %d" % (plen, prev))
    print("  class code     : %02x %02x %02x" % (cls2, cls1, cls0))
    print("  image length   : %d blocks = %d bytes (0x%x)" % (ilen512, ilen, ilen))
    print("  code revision  : 0x%04x" % crev)
    print("  code type      : %d (%s)" % (ctype, {
        0: "x86 PC-AT", 1: "OpenFirmware", 2: "HP PA-RISC",
        3: "EFI", 0x70: "NVIDIA ext"}.get(ctype, "?")))
    print("  indicator      : 0x%02x  last_image=%s"
          % (indicator, "YES" if indicator & 0x80 else "NO"))

    npde = off + pcir_rel + plen
    npde = (npde + 15) & ~15
    if rom[npde:npde + 4] == b"NPDE":
        nver, nlen, nsub, nindic = struct.unpack_from("<HHHB", rom, npde + 4)
        print("  NPDE at        : 0x%06x  ver=0x%04x len=%d subimages=%d "
              "indicator=0x%02x last=%s"
              % (npde, nver, nlen, nsub, nindic,
                 "YES" if nindic & 0x80 else "NO"))
    else:
        near = [hex(x) for x in find_all(rom, b"NPDE") if off <= x < off + max(ilen, 1)]
        print("  NPDE           : not at 0x%06x; in-image hits %s" % (npde, near))

    if ilen == 0:
        print("  -> image length 0, cannot advance")
        break
    end = off + ilen
    print("  image spans    : 0x%06x .. 0x%06x" % (off, end))
    if indicator & 0x80:
        print("  -> LAST IMAGE. chain ends at 0x%06x; file is 0x%x"
              % (end, len(rom)))
        if end < len(rom):
            print("     %d bytes of trailing data after the last image"
                  % (len(rom) - end))
        break
    off = end
    image += 1
print("")

# The second PCIR sits at 0xf37d, which is NOT 512-byte aligned -- so it is not
# a second ROM image, it is a copy inside the NPDS/expansion area. Confirm by
# printing the bytes around every PCIR/NPDE/NPDS hit.
print("=== signature offsets and alignment ===")
for name in (b"PCIR", b"NPDE", b"NPDS", b"\xff\xb8BIT\x00"):
    for i in find_all(rom, name):
        print("  %-10s 0x%06x  512-aligned=%s  16-aligned=%s"
              % (repr(name), i, i % 512 == 0, i % 16 == 0))
print("")

# Test hypothesis (a) numerically: if the file is image 0 of a larger ROM and
# BIT pointers are absolute, then falcon ptr 0x077f77 should be plausible for a
# 512KB (0x80000) or 1MB (0x100000) ROM.
FALCON_PTR = 0x00077F77
print("=== pointer plausibility ===")
print("  falcon table ptr : 0x%06x (%d)" % (FALCON_PTR, FALCON_PTR))
for size, label in ((0x10000, "64KB"), (0x20000, "128KB"), (0x40000, "256KB"),
                    (0x80000, "512KB"), (0x100000, "1MB")):
    print("    within %-6s (0x%06x): %s" % (label, size, FALCON_PTR < size))
print("  file covers      : 0x000000 .. 0x%06x" % len(rom))
print("")

# If instead the pointer were relative to the BIT base or to the image start,
# check whether it lands anywhere sane.
for label, cand in (("as-is", FALCON_PTR),
                    ("& 0xffff", FALCON_PTR & 0xFFFF),
                    ("- 0x70000", FALCON_PTR - 0x70000)):
    inside = 0 <= cand < len(rom)
    b = rom[cand:cand + 16].hex(" ") if inside else "(out of range)"
    print("  %-10s -> 0x%06x  in-file=%-5s  %s" % (label, cand, inside, b))
