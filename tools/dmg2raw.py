#!/usr/bin/env python3
"""
dmg2raw.py -- convert an Apple compressed UDIF .dmg into a raw disk image.

Why this exists
---------------
VMware's `cdrom-image` / raw-disk paths cannot read a *compressed* UDIF image.
Attaching `BaseSystem.dmg` directly produces a device with `capacity=0`, and the
guest EFI then reports `msg.Backdoor.OsNotFound` -- there is no readable media,
so nothing boots. The log is not obviously about the dmg, which makes this an
easy hour to lose.

`dmg2img` is the usual tool, but it is not present on this Windows host and
there is no package manager to get it from. Everything needed is in the Python
standard library, so this does the job directly.

Format, briefly
---------------
A UDIF file ends with a 512-byte `koly` trailer. The trailer points at an XML
plist (`resource-fork` -> `blkx`), an array of entries each holding a
base64 `Data` blob. Each blob is a big-endian `mish` table:

    off  size  field
    0    4     'mish'
    4    4     version (1)
    8    8     first sector of this blkx run, in the OUTPUT image
    16   8     sector count
    24   8     dataStart -- base for every chunk's input offset
    32   4     decompressBufferRequested
    36   4     blocksDescriptor
    40   24    reserved (6 x uint32)
    64   136   UDIFChecksum (type 4 + size 4 + data 128)
    200  4     number of chunk entries
    204  ...   chunk entries, 40 bytes each

The checksum is 136 bytes, NOT 40. Getting that wrong puts the chunk count at
offset 104 instead of 200, which reads a garbage count of zero-type chunks: the
converter then reports "bytes decoded: 0" and writes a 3 GiB file of pure
zeroes that looks plausible until the guest fails to boot off it. That happened
once here; the sector ranges printed correctly (they live at offsets 8/16) which
made the output look right.

Each chunk entry:

    off  size  field
    0    4     type
    4    4     comment
    8    8     output sector offset, relative to the blkx run start
    16   8     output sector count
    24   8     input offset, absolute in the file
    32   8     input length

Chunk types actually seen in Apple images:

    0x00000000  zero fill        0x80000005  zlib
    0x00000001  raw             0x80000006  bzip2
    0x00000002  ignore (sparse) 0x80000007  LZFSE
    0x7FFFFFFE  comment         0x80000008  LZMA
    0xFFFFFFFF  terminator

LZFSE is the one that has no stdlib decoder. `--census` reports the mix before
any conversion is attempted, so an unsupported image is discovered in seconds
rather than after writing gigabytes.

Usage
-----
    python dmg2raw.py IMAGE.dmg --census
    python dmg2raw.py IMAGE.dmg -o OUTPUT.img
"""

import argparse
import base64
import bz2
import lzma
import plistlib
import struct
import sys
import zlib

SECTOR = 512

T_ZERO = 0x00000000
T_RAW = 0x00000001
T_IGNORE = 0x00000002
T_COMMENT = 0x7FFFFFFE
T_ADC = 0x80000004
T_ZLIB = 0x80000005
T_BZIP2 = 0x80000006
T_LZFSE = 0x80000007
T_LZMA = 0x80000008
T_TERM = 0xFFFFFFFF

NAMES = {
    T_ZERO: "zero",
    T_RAW: "raw",
    T_IGNORE: "ignore",
    T_COMMENT: "comment",
    T_ADC: "adc",
    T_ZLIB: "zlib",
    T_BZIP2: "bzip2",
    T_LZFSE: "lzfse",
    T_LZMA: "lzma",
    T_TERM: "terminator",
}

DECODABLE = {T_ZERO, T_RAW, T_IGNORE, T_COMMENT, T_TERM,
             T_ZLIB, T_BZIP2, T_LZMA}


def read_koly(fh):
    fh.seek(-SECTOR, 2)
    k = fh.read(SECTOR)
    if k[:4] != b"koly":
        raise SystemExit("FAIL: no koly trailer -- not a UDIF image")
    xml_off, xml_len = struct.unpack_from(">QQ", k, 0xD8)
    if xml_len == 0:
        raise SystemExit("FAIL: koly reports no XML plist (resource-fork only "
                         "images are not supported)")
    return xml_off, xml_len


def blkx_entries(fh):
    xml_off, xml_len = read_koly(fh)
    fh.seek(xml_off)
    root = plistlib.loads(fh.read(xml_len))
    entries = root["resource-fork"]["blkx"]
    out = []
    for e in entries:
        data = e["Data"]
        if isinstance(data, str):
            data = base64.b64decode(data)
        out.append((e.get("Name") or e.get("CFName") or "?", data))
    return out


def parse_mish(blob):
    if blob[:4] != b"mish":
        raise SystemExit("FAIL: blkx entry is not a mish table")
    start_sector, sector_count, data_start = struct.unpack_from(">QQQ", blob, 8)
    count = struct.unpack_from(">I", blob, 200)[0]

    expected = 204 + count * 40
    if expected > len(blob):
        raise SystemExit(
            "FAIL: mish claims %d chunks (needs %d bytes) but the blkx blob is "
            "only %d bytes -- header offsets are wrong"
            % (count, expected, len(blob)))

    chunks = []
    off = 204
    for _ in range(count):
        ctype, _comment, out_off, out_cnt, in_off, in_len = \
            struct.unpack_from(">IIQQQQ", blob, off)
        chunks.append((ctype, out_off, out_cnt, data_start + in_off, in_len))
        off += 40
    return start_sector, sector_count, chunks


def census(path):
    with open(path, "rb") as fh:
        entries = blkx_entries(fh)
        tally = {}
        total_out = 0
        print("blkx runs: %d" % len(entries))
        print()
        for name, blob in entries:
            start, count, chunks = parse_mish(blob)
            end = start + count
            total_out = max(total_out, end)
            kinds = sorted({NAMES.get(c[0], "0x%08x" % c[0]) for c in chunks})
            print("  %-52s sectors %10d..%-10d  %s"
                  % (name[:52], start, end, ",".join(kinds)))
            for c in chunks:
                tally[c[0]] = tally.get(c[0], 0) + 1
        print()
        print("chunk types:")
        unsupported = []
        for ctype in sorted(tally):
            label = NAMES.get(ctype, "UNKNOWN")
            mark = "" if ctype in DECODABLE else "   <-- NO STDLIB DECODER"
            if ctype not in DECODABLE:
                unsupported.append(label)
            print("  0x%08x  %-12s %8d%s" % (ctype, label, tally[ctype], mark))
        print()
        print("output size: %d sectors = %d bytes (%.2f GiB)"
              % (total_out, total_out * SECTOR,
                 total_out * SECTOR / (1024 ** 3)))
        print()
        if unsupported:
            print("RESULT: cannot convert with the standard library alone -- "
                  "needs %s" % ", ".join(sorted(set(unsupported))))
            return 1
        print("RESULT: every chunk type is decodable, conversion will work")
        return 0


def convert(path, out_path):
    with open(path, "rb") as fh:
        entries = blkx_entries(fh)

        total = 0
        for _name, blob in entries:
            start, count, _chunks = parse_mish(blob)
            total = max(total, start + count)

        with open(out_path, "wb") as out:
            out.truncate(total * SECTOR)

            written = 0
            for name, blob in entries:
                start, _count, chunks = parse_mish(blob)
                for ctype, out_off, out_cnt, in_off, in_len in chunks:
                    if ctype in (T_TERM, T_COMMENT):
                        continue

                    pos = (start + out_off) * SECTOR
                    size = out_cnt * SECTOR

                    if ctype == T_IGNORE or ctype == T_ZERO:
                        # File was truncated to full length, so holes already
                        # read as zero. Skip rather than write megabytes of
                        # nulls.
                        continue

                    fh.seek(in_off)
                    raw = fh.read(in_len)

                    if ctype == T_RAW:
                        data = raw
                    elif ctype == T_ZLIB:
                        data = zlib.decompress(raw)
                    elif ctype == T_BZIP2:
                        data = bz2.decompress(raw)
                    elif ctype == T_LZMA:
                        data = lzma.decompress(raw)
                    else:
                        raise SystemExit(
                            "FAIL: chunk type 0x%08x (%s) has no decoder"
                            % (ctype, NAMES.get(ctype, "unknown")))

                    if len(data) != size:
                        raise SystemExit(
                            "FAIL: chunk at sector %d decoded to %d bytes, "
                            "expected %d" % (start + out_off, len(data), size))

                    out.seek(pos)
                    out.write(data)
                    written += size

                print("  done  %s" % name[:60])

        print()
        print("wrote %s" % out_path)
        print("  image size    : %d bytes (%.2f GiB)"
              % (total * SECTOR, total * SECTOR / (1024 ** 3)))
        print("  bytes decoded : %d" % written)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dmg")
    ap.add_argument("-o", "--output")
    ap.add_argument("--census", action="store_true",
                    help="report partition layout and chunk types, convert nothing")
    a = ap.parse_args()

    if a.census or not a.output:
        return census(a.dmg)
    return convert(a.dmg, a.output)


if __name__ == "__main__":
    sys.exit(main())
