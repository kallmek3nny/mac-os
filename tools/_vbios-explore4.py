#!/usr/bin/env python3
"""_vbios-explore4.py -- throwaway probe #4 for t-08cd68.

Why this replaces probe #3
--------------------------
Probe #3 unpacked the PCIR data structure as

    struct.unpack_from("<HHHHHBBBHHBB", rom, pcir + 4)
                            ^
                            revision as u16 -- WRONG

Per the PCI Firmware Specification the layout from the signature is:

    +0x00  char  signature[4]      "PCIR"
    +0x04  u16   vendor_id
    +0x06  u16   device_id
    +0x08  u16   device_list_ptr    (reserved in rev 0)
    +0x0A  u16   pci_ds_length
    +0x0C  u8    pci_ds_revision    <-- ONE byte, not two
    +0x0D  u8    class_code[0]      (low byte)
    +0x0E  u8    class_code[1]
    +0x0F  u8    class_code[2]      (high byte / base class)
    +0x10  u16   image_length       in 512-byte blocks
    +0x12  u16   code_revision
    +0x14  u8    code_type
    +0x15  u8    indicator          bit7 = last image
    +0x16  u16   reserved

Reading revision as u16 shifted class_code, image_length, code_revision,
code_type and the last-image bit each one byte late. That is why probe #3
reported class "7f 03 02", code type 128 and last_image=NO -- all three are
artifacts of the bad struct, not properties of the ROM.

So every conclusion probe #3 offered about truncation is withdrawn pending this
re-measurement. This probe prints the raw 24 bytes of each PCIR next to the
decode, so the decode can be checked by eye against the hex without trusting
the format string.
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


def decode_pcir(off):
    """Decode the PCIR at absolute offset `off`. Returns a dict of fields."""
    sig = rom[off:off + 4]
    vendor, device, dlp, plen = struct.unpack_from("<HHHH", rom, off + 4)
    prev = rom[off + 0x0C]
    cls = rom[off + 0x0D:off + 0x10]              # low, mid, high
    ilen512, crev = struct.unpack_from("<HH", rom, off + 0x10)
    ctype = rom[off + 0x14]
    indicator = rom[off + 0x15]
    return {
        "sig": sig, "vendor": vendor, "device": device, "dlp": dlp,
        "plen": plen, "prev": prev, "cls": cls, "ilen512": ilen512,
        "crev": crev, "ctype": ctype, "indicator": indicator,
    }


CODE_TYPES = {0: "x86 PC-AT", 1: "Open Firmware", 2: "HP PA-RISC",
              3: "EFI", 0x70: "NVIDIA extended"}

print("=== every PCIR in the file, decoded with the CORRECTED layout ===")
for pcir in find_all(rom, b"PCIR"):
    raw = rom[pcir:pcir + 24]
    f = decode_pcir(pcir)
    ilen = f["ilen512"] * 512
    print("PCIR @0x%06x  (512-aligned=%s)" % (pcir, pcir % 512 == 0))
    print("  raw 24 bytes   : %s" % raw.hex(" "))
    print("  field offsets  : 00-03 sig | 04 ven | 06 dev | 08 dlp | 0a plen |"
          " 0c rev | 0d-0f class | 10 ilen | 12 crev | 14 ctype | 15 indic")
    print("  vendor:device  : 0x%04x:0x%04x" % (f["vendor"], f["device"]))
    print("  device list ptr: 0x%04x" % f["dlp"])
    print("  pci_ds_length  : %d" % f["plen"])
    print("  pci_ds_revision: %d" % f["prev"])
    print("  class code     : %02x%02x%02x (base 0x%02x sub 0x%02x prog 0x%02x)"
          % (f["cls"][2], f["cls"][1], f["cls"][0],
             f["cls"][2], f["cls"][1], f["cls"][0]))
    print("  image length   : %d blocks = %d bytes (0x%x)"
          % (f["ilen512"], ilen, ilen))
    print("  code revision  : 0x%04x" % f["crev"])
    print("  code type      : %d (%s)"
          % (f["ctype"], CODE_TYPES.get(f["ctype"], "unknown")))
    print("  indicator      : 0x%02x  last_image=%s"
          % (f["indicator"], "YES" if f["indicator"] & 0x80 else "NO"))
    print("")

print("=== PCI expansion ROM image chain (corrected) ===")
off = 0
image = 0
while off < len(rom) and image < 16:
    if rom[off:off + 2] != b"\x55\xaa":
        print("image %d @0x%06x : no 55aa (bytes %s) -- chain ends here"
              % (image, off, rom[off:off + 2].hex()))
        break
    pcir_rel = struct.unpack_from("<H", rom, off + 0x18)[0]
    pcir = off + pcir_rel
    if pcir + 24 > len(rom) or rom[pcir:pcir + 4] != b"PCIR":
        print("image %d @0x%06x : PCIR ptr 0x%04x -> 0x%06x is not a PCIR"
              % (image, off, pcir_rel, pcir))
        break
    f = decode_pcir(pcir)
    ilen = f["ilen512"] * 512
    last = bool(f["indicator"] & 0x80)
    print("image %d @0x%06x  PCIR@0x%06x  %04x:%04x  ctype=%d  len=%d bytes"
          "  last=%s"
          % (image, off, pcir, f["vendor"], f["device"], f["ctype"], ilen,
             "YES" if last else "NO"))

    # NPDE, if present, follows the PCIR and is 16-byte aligned.
    npde = (pcir + f["plen"] + 15) & ~15
    if rom[npde:npde + 4] == b"NPDE":
        nver, nlen, nsub = struct.unpack_from("<HHH", rom, npde + 4)
        nindic = rom[npde + 10]
        print("    NPDE@0x%06x ver=0x%04x len=%d subimages=%d indicator=0x%02x"
              "  last=%s"
              % (npde, nver, nlen, nsub, nindic,
                 "YES" if nindic & 0x80 else "NO"))
    else:
        print("    NPDE          : not at 0x%06x (bytes %s)"
              % (npde, rom[npde:npde + 4].hex()))

    if ilen == 0:
        print("    image length 0 -- cannot advance, stopping")
        break
    end = off + ilen
    print("    spans 0x%06x .. 0x%06x  (file ends 0x%06x)"
          % (off, end, len(rom)))
    if end > len(rom):
        print("    -> DECLARED LENGTH EXCEEDS THE FILE by %d bytes"
              % (end - len(rom)))
    if last:
        print("    -> last image; chain complete")
        break
    off = end
    image += 1
print("")

# Re-test the falcon pointer against the image length the corrected decode
# reports, rather than against the file length alone.
FALCON_PTR = 0x00077F77
print("=== falcon table pointer vs measured extents ===")
print("  falcon ptr from BIT token 0x70 : 0x%06x (%d)" % (FALCON_PTR, FALCON_PTR))
print("  file length                    : 0x%06x (%d)" % (len(rom), len(rom)))
first = decode_pcir(struct.unpack_from("<H", rom, 0x18)[0])
print("  image 0 declared length        : 0x%06x (%d)"
      % (first["ilen512"] * 512, first["ilen512"] * 512))
print("  in file?                       : %s" % (FALCON_PTR < len(rom)))
print("  in declared image 0?           : %s"
      % (FALCON_PTR < first["ilen512"] * 512))
