#!/usr/bin/env python3
"""Read a byte range out of a qcow2 image using only the standard library.

Why this exists
---------------
`qemu-img dd` extracted the ESP out of `sonoma-base.qcow2` at `skip=40`, and
then refused the APFS container at `skip=409640` with:

    qemu-img.exe: sonoma-base.qcow2: cannot skip to specified offset

Retrying 4 KiB-aligned (`bs=4096 skip=51205`) failed identically, and in BOTH
cases the exit status arriving in the shell was 0 while the output file was
0 bytes -- the failure is on stderr only. That is exactly the shape this project
keeps getting bitten by: a command that reports success and produces nothing.
So the ESP evidence is real and the APFS evidence was simply never collected.

Rather than argue with `qemu-img dd`, this reads the qcow2 mapping directly.
That also removes a dependency from the measurement path: the structural claims
in QEMU-LOWSTORAGE.md (protective MBR, `EFI PART` at LBA 1) were made with
qemu-img, and being able to reproduce them through an independent decoder is
what turns them from "a tool said so" into a cross-checked reading.

The compressed-cluster case is not optional here
------------------------------------------------
The image was produced with `qemu-img convert -c`, so most clusters are zlib
DEFLATE streams rather than raw data. A reader that handles only the standard
L2 descriptor would return plausible garbage for those clusters instead of
failing -- so the compressed branch is implemented, and `--info` reports how
many clusters of each kind a range actually used.

Usage:
    python tools/qcow2-read.py IMG --info
    python tools/qcow2-read.py IMG --offset 512 --length 512 --out LBA1.bin
    python tools/qcow2-read.py IMG --offset 512 --length 8 --hex

Exit codes: 2 for a malformed invocation, 1 when the image cannot be decoded,
0 only when the requested bytes were produced.
"""

import argparse
import struct
import sys
import zlib

QCOW_MAGIC = b"QFI\xfb"

# L1/L2 entries carry flags in the top two bits and the host offset in 9..55.
L2_OFFSET_MASK = 0x00FFFFFFFFFFFE00
FLAG_COPIED = 1 << 63
FLAG_COMPRESSED = 1 << 62


class Qcow2(object):
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(72)
        if len(head) < 72 or head[:4] != QCOW_MAGIC:
            raise ValueError("not a qcow2: first 4 bytes are %s" % head[:4].hex(" "))
        (self.version,
         self.backing_offset,
         self.backing_size,
         self.cluster_bits,
         self.size,
         self.crypt_method,
         self.l1_size,
         self.l1_offset) = struct.unpack(">IQIIQIIQ", head[4:48])
        if self.version not in (2, 3):
            raise ValueError("unsupported qcow2 version %d" % self.version)
        if self.crypt_method != 0:
            raise ValueError("encrypted images are not supported (crypt_method=%d)"
                             % self.crypt_method)
        if not (9 <= self.cluster_bits <= 21):
            raise ValueError("implausible cluster_bits %d" % self.cluster_bits)
        self.cluster_size = 1 << self.cluster_bits
        self.l2_entries = self.cluster_size // 8

        # Compressed descriptor geometry, straight off the qcow2 spec: the
        # boundary between "host offset" and "sector count" MOVES with the
        # cluster size, so it cannot be hardcoded.
        self.csize_shift = 62 - (self.cluster_bits - 8)
        self.csize_mask = (1 << (self.cluster_bits - 8)) - 1
        self.coffset_mask = (1 << self.csize_shift) - 1

        self.stats = {"raw": 0, "compressed": 0, "unallocated": 0}
        self._l1 = None

    def l1(self):
        if self._l1 is None:
            self.f.seek(self.l1_offset)
            raw = self.f.read(self.l1_size * 8)
            if len(raw) < self.l1_size * 8:
                raise ValueError("L1 table truncated: got %d of %d bytes"
                                 % (len(raw), self.l1_size * 8))
            self._l1 = list(struct.unpack(">%dQ" % self.l1_size, raw))
        return self._l1

    def _l2_entry(self, guest_cluster):
        l1_index = guest_cluster // self.l2_entries
        l2_index = guest_cluster % self.l2_entries
        l1 = self.l1()
        if l1_index >= len(l1):
            return 0
        l2_offset = l1[l1_index] & L2_OFFSET_MASK
        if l2_offset == 0:
            return 0
        self.f.seek(l2_offset + l2_index * 8)
        raw = self.f.read(8)
        if len(raw) < 8:
            raise ValueError("L2 entry truncated at guest cluster %d" % guest_cluster)
        return struct.unpack(">Q", raw)[0]

    def cluster(self, guest_cluster):
        """Return the full decoded contents of one guest cluster."""
        entry = self._l2_entry(guest_cluster)
        if entry == 0:
            self.stats["unallocated"] += 1
            return b"\x00" * self.cluster_size

        if entry & FLAG_COMPRESSED:
            self.stats["compressed"] += 1
            coffset = entry & self.coffset_mask
            nb_csectors = ((entry >> self.csize_shift) & self.csize_mask) + 1
            # The stream STARTS mid-sector, so the byte count is the sector span
            # minus the in-sector start. Reading nb_csectors*512 from coffset
            # would overrun into the next cluster's stream.
            csize = nb_csectors * 512 - (coffset & 511)
            self.f.seek(coffset)
            blob = self.f.read(csize)
            # Raw DEFLATE (-15): qcow2 stores no zlib header on these.
            data = zlib.decompressobj(-15).decompress(blob, self.cluster_size)
            if len(data) < self.cluster_size:
                data += b"\x00" * (self.cluster_size - len(data))
            return data[:self.cluster_size]

        self.stats["raw"] += 1
        host = entry & L2_OFFSET_MASK
        if host == 0:
            self.stats["raw"] -= 1
            self.stats["unallocated"] += 1
            return b"\x00" * self.cluster_size
        self.f.seek(host)
        data = self.f.read(self.cluster_size)
        if len(data) < self.cluster_size:
            data += b"\x00" * (self.cluster_size - len(data))
        return data

    def read(self, offset, length):
        if offset < 0 or length < 0:
            raise ValueError("negative offset/length: %d/%d" % (offset, length))
        if offset + length > self.size:
            raise ValueError("range %d+%d exceeds virtual size %d"
                             % (offset, length, self.size))
        out = bytearray()
        pos = offset
        remaining = length
        while remaining > 0:
            gc = pos // self.cluster_size
            within = pos % self.cluster_size
            take = min(self.cluster_size - within, remaining)
            out += self.cluster(gc)[within:within + take]
            pos += take
            remaining -= take
        return bytes(out)

    def describe(self):
        print("version        : %d" % self.version)
        print("cluster size   : %d bytes (cluster_bits=%d)"
              % (self.cluster_size, self.cluster_bits))
        print("virtual size   : %d bytes (%.2f GiB)"
              % (self.size, self.size / 1073741824.0))
        print("L1 table       : %d entries at host offset 0x%x"
              % (self.l1_size, self.l1_offset))
        print("crypt method   : %d" % self.crypt_method)
        l1 = self.l1()
        present = sum(1 for e in l1 if (e & L2_OFFSET_MASK))
        print("L1 entries set  : %d of %d" % (present, len(l1)))


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="qcow2-read.py",
        description="Read a byte range out of a qcow2 image with the stdlib only.")
    p.add_argument("image")
    p.add_argument("--info", action="store_true", help="print header geometry")
    p.add_argument("--offset", type=int, default=None, help="guest byte offset")
    p.add_argument("--lba", type=int, default=None,
                   help="guest offset expressed in 512-byte sectors")
    p.add_argument("--length", type=int, default=512, help="bytes to read")
    p.add_argument("--out", default=None, help="write the bytes here")
    p.add_argument("--hex", action="store_true", help="print the bytes as hex")
    args = p.parse_args(argv)

    if args.offset is not None and args.lba is not None:
        p.error("pass --offset or --lba, not both")
    if not args.info and args.offset is None and args.lba is None:
        p.error("nothing to do: pass --info, or --offset/--lba with --length")

    try:
        img = Qcow2(args.image)
    except (OSError, ValueError, struct.error) as exc:
        sys.stderr.write("FAIL: cannot open %s: %s\n" % (args.image, exc))
        return 1

    if args.info:
        img.describe()

    if args.offset is None and args.lba is None:
        return 0

    offset = args.offset if args.offset is not None else args.lba * 512
    try:
        data = img.read(offset, args.length)
    except ValueError as exc:
        sys.stderr.write("FAIL: %s\n" % exc)
        return 2
    except (OSError, zlib.error, struct.error) as exc:
        sys.stderr.write("FAIL: cannot read %d+%d: %s\n" % (offset, args.length, exc))
        return 1

    if args.info:
        print()
    print("read %d bytes at guest offset %d (LBA %.1f)"
          % (len(data), offset, offset / 512.0))
    print("clusters used  : raw=%d compressed=%d unallocated=%d"
          % (img.stats["raw"], img.stats["compressed"], img.stats["unallocated"]))
    if args.hex:
        print(data.hex(" "))
    if args.out:
        open(args.out, "wb").write(data)
        print("wrote %s (%d bytes)" % (args.out, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
