#!/usr/bin/env python3
"""Read a byte range out of a (split) sparse VMDK using only the standard library.

Why this exists
---------------
`qemu-img dd` is not usable as a random-access reader here. It extracted the ESP
out of the qcow2 at `skip=40`, then refused `skip=409640` on the same file, and
refused `skip=40` on `Sonoma.vmdk`, every time with:

    qemu-img.exe: <file>: cannot skip to specified offset

In all three failures the exit status arriving in the shell was **0** and the
output file was **0 bytes** -- the diagnosis is on stderr only. A command that
reports success and produces nothing is the exact failure shape this project
keeps re-encountering, so the reader was replaced rather than argued with.

`tools/qcow2-read.py` covers the converted image. This covers the SOURCE, and the
reason to want both is a specific question: the converted guest's EFI System
Partition contains one FAT volume-label entry and nothing else, which is why
edk2 reported `failed to load Boot0001 ... Not Found`. That is either the real
state of the guest or damage introduced by the conversion, and the only way to
tell is to read the same LBA out of the VMDK that VMware still boots.

Format notes that matter
------------------------
`Sonoma.vmdk` is a TEXT descriptor listing 26 `SPARSE` extents; each extent file
carries its own 512-byte `KDMV` header, grain directory and grain tables. So a
guest LBA resolves in three steps -- extent, then grain table, then grain -- and
a reader that skips the extent step silently returns data from the wrong 2 GB of
the disk. Unallocated grains read as zeros, which is correct and is also why
`--info` reports the allocated/unallocated split: a range that reads as all
zeros because nothing is mapped there must not be confused with a range that was
read and found empty.

Usage:
    python tools/vmdk-read.py Sonoma.vmdk --info
    python tools/vmdk-read.py Sonoma.vmdk --lba 40 --length 512 --out ESP.bin
    python tools/vmdk-read.py Sonoma.vmdk --lba 1 --length 16 --hex

Exit codes: 2 for a malformed invocation, 1 when the image cannot be decoded,
0 only when the requested bytes were produced.
"""

import argparse
import os
import re
import struct
import sys

SECTOR = 512
KDMV = b"KDMV"
FLAG_COMPRESSED = 1 << 16
GD_AT_END = 0xFFFFFFFFFFFFFFFF


class SparseExtent(object):
    """One `-sNNN.vmdk` file: header, grain directory, grain tables."""

    def __init__(self, path, nominal_sectors):
        self.path = path
        self.nominal_sectors = nominal_sectors
        self.f = open(path, "rb")
        hdr = self.f.read(512)
        if len(hdr) < 512 or hdr[:4] != KDMV:
            raise ValueError("%s is not a sparse extent: first 4 bytes are %s"
                             % (path, hdr[:4].hex(" ")))
        self.version, self.flags = struct.unpack("<II", hdr[4:12])
        (self.capacity, self.grain_size,
         self.desc_offset, self.desc_size) = struct.unpack("<QQQQ", hdr[12:44])
        self.num_gtes, = struct.unpack("<I", hdr[44:48])
        (self.rgd_offset, self.gd_offset,
         self.overhead) = struct.unpack("<QQQ", hdr[48:72])

        if self.flags & FLAG_COMPRESSED:
            raise ValueError("%s uses compressed grains (flags=0x%x); this reader "
                             "handles plain SPARSE only" % (path, self.flags))
        if self.gd_offset == GD_AT_END:
            raise ValueError("%s is streamOptimized (grain directory in a footer); "
                             "not supported" % path)
        if self.grain_size == 0 or self.num_gtes == 0:
            raise ValueError("%s has grain_size=%d num_gtes=%d"
                             % (path, self.grain_size, self.num_gtes))

        self.stats = {"grain": 0, "hole_gt": 0, "hole_grain": 0}
        self._gd = None

    def gd(self):
        if self._gd is None:
            # One GD entry per grain table; each table maps num_gtes grains.
            grains = (self.capacity + self.grain_size - 1) // self.grain_size
            n = int((grains + self.num_gtes - 1) // self.num_gtes)
            self.f.seek(self.gd_offset * SECTOR)
            raw = self.f.read(n * 4)
            if len(raw) < n * 4:
                raise ValueError("%s: grain directory truncated (%d of %d bytes)"
                                 % (self.path, len(raw), n * 4))
            self._gd = list(struct.unpack("<%dI" % n, raw))
        return self._gd

    def read_sector(self, local_sector):
        grain = local_sector // self.grain_size
        within = local_sector % self.grain_size
        gt_index = grain // self.num_gtes
        gt_slot = grain % self.num_gtes

        gd = self.gd()
        if gt_index >= len(gd) or gd[gt_index] == 0:
            self.stats["hole_gt"] += 1
            return b"\x00" * SECTOR

        self.f.seek(gd[gt_index] * SECTOR + gt_slot * 4)
        raw = self.f.read(4)
        if len(raw) < 4:
            raise ValueError("%s: grain table entry truncated at grain %d"
                             % (self.path, grain))
        grain_sector, = struct.unpack("<I", raw)
        if grain_sector == 0:
            self.stats["hole_grain"] += 1
            return b"\x00" * SECTOR

        self.stats["grain"] += 1
        self.f.seek((grain_sector + within) * SECTOR)
        data = self.f.read(SECTOR)
        if len(data) < SECTOR:
            data += b"\x00" * (SECTOR - len(data))
        return data


class Vmdk(object):
    def __init__(self, path):
        self.path = path
        raw = open(path, "rb").read(64 * 1024)
        if raw[:4] == KDMV:
            # Monolithic sparse: the descriptor lives INSIDE the extent.
            desc_off, desc_size = struct.unpack("<QQ", raw[28:44])
            with open(path, "rb") as f:
                f.seek(desc_off * SECTOR)
                text = f.read(desc_size * SECTOR).decode("latin-1")
        else:
            text = raw.decode("latin-1")
        self.descriptor = text

        self.create_type = None
        m = re.search(r'createType\s*=\s*"([^"]+)"', text)
        if m:
            self.create_type = m.group(1)

        base = os.path.dirname(os.path.abspath(path))
        self.extents = []
        cursor = 0
        for line in text.splitlines():
            m = re.match(r'\s*(RW|RDONLY|NOACCESS)\s+(\d+)\s+(\w+)\s+"([^"]+)"', line)
            if not m:
                continue
            _access, sectors, kind, name = m.groups()
            sectors = int(sectors)
            if kind.upper() != "SPARSE":
                raise ValueError("extent %s is %s, not SPARSE" % (name, kind))
            epath = name if os.path.isabs(name) else os.path.join(base, name)
            if not os.path.exists(epath):
                raise ValueError("extent file missing: %s" % epath)
            ext = SparseExtent(epath, sectors)
            self.extents.append((cursor, cursor + sectors, ext))
            cursor += sectors
        if not self.extents:
            raise ValueError("%s declares no SPARSE extents" % path)
        self.total_sectors = cursor

    def read(self, offset, length):
        if offset < 0 or length < 0:
            raise ValueError("negative offset/length: %d/%d" % (offset, length))
        end = offset + length
        if end > self.total_sectors * SECTOR:
            raise ValueError("range %d+%d exceeds capacity %d bytes"
                             % (offset, length, self.total_sectors * SECTOR))
        out = bytearray()
        pos = offset
        while pos < end:
            sector = pos // SECTOR
            within = pos % SECTOR
            take = min(SECTOR - within, end - pos)
            data = None
            for start, stop, ext in self.extents:
                if start <= sector < stop:
                    data = ext.read_sector(sector - start)
                    break
            if data is None:
                raise ValueError("guest sector %d falls outside every extent" % sector)
            out += data[within:within + take]
            pos += take
        return bytes(out)

    def stats(self):
        agg = {"grain": 0, "hole_gt": 0, "hole_grain": 0}
        for _s, _e, ext in self.extents:
            for k in agg:
                agg[k] += ext.stats[k]
        return agg

    def describe(self):
        print("descriptor     : %s" % self.path)
        print("createType     : %s" % self.create_type)
        print("extents        : %d" % len(self.extents))
        print("capacity       : %d sectors (%.2f GiB)"
              % (self.total_sectors, self.total_sectors * SECTOR / 1073741824.0))
        first = self.extents[0][2]
        print("extent[0] hdr  : version=%d flags=0x%x grainSize=%d numGTEsPerGT=%d"
              % (first.version, first.flags, first.grain_size, first.num_gtes))
        print("extent[0] gd   : sector %d, overhead %d"
              % (first.gd_offset, first.overhead))
        for i, (start, stop, ext) in enumerate(self.extents[:4]):
            print("   #%-2d LBA %-10d-%-11d %s"
                  % (i + 1, start, stop - 1, os.path.basename(ext.path)))
        if len(self.extents) > 4:
            print("   ... %d more" % (len(self.extents) - 4))


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="vmdk-read.py",
        description="Read a byte range out of a sparse VMDK with the stdlib only.")
    p.add_argument("image", help="the .vmdk DESCRIPTOR (not a -sNNN extent)")
    p.add_argument("--info", action="store_true", help="print descriptor and header geometry")
    p.add_argument("--offset", type=int, default=None, help="guest byte offset")
    p.add_argument("--lba", type=int, default=None, help="guest offset in 512-byte sectors")
    p.add_argument("--length", type=int, default=512, help="bytes to read")
    p.add_argument("--out", default=None, help="write the bytes here")
    p.add_argument("--hex", action="store_true", help="print the bytes as hex")
    args = p.parse_args(argv)

    if args.offset is not None and args.lba is not None:
        p.error("pass --offset or --lba, not both")
    if not args.info and args.offset is None and args.lba is None:
        p.error("nothing to do: pass --info, or --offset/--lba with --length")

    try:
        img = Vmdk(args.image)
    except (OSError, ValueError, struct.error) as exc:
        sys.stderr.write("FAIL: cannot open %s: %s\n" % (args.image, exc))
        return 1

    if args.info:
        img.describe()

    if args.offset is None and args.lba is None:
        return 0

    offset = args.offset if args.offset is not None else args.lba * SECTOR
    try:
        data = img.read(offset, args.length)
    except ValueError as exc:
        sys.stderr.write("FAIL: %s\n" % exc)
        return 2
    except (OSError, struct.error) as exc:
        sys.stderr.write("FAIL: cannot read %d+%d: %s\n" % (offset, args.length, exc))
        return 1

    st = img.stats()
    if args.info:
        print()
    print("read %d bytes at guest offset %d (LBA %.1f)"
          % (len(data), offset, offset / float(SECTOR)))
    print("sectors served : mapped=%d unmapped_gt=%d unmapped_grain=%d"
          % (st["grain"], st["hole_gt"], st["hole_grain"]))
    if args.hex:
        print(data.hex(" "))
    if args.out:
        open(args.out, "wb").write(data)
        print("wrote %s (%d bytes)" % (args.out, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
