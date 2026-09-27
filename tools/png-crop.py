#!/usr/bin/env python3
"""Decode, measure and crop a PNG using only the standard library.

Why this exists
---------------
A framebuffer dump is only evidence if you actually read the pixels, and reading
them correctly is less obvious than it looks.

The first version of this measurement treated each PNG scanline as raw RGB after
skipping one leading byte. That works for PNGs written by `tools/ppm-to-png.py`,
because that script emits filter type 0 on every row. It is WRONG for PNGs
written by QEMU's own encoder, which picks a filter PER ROW -- measured on
qemu-results/boot-probe-png.png, the filter bytes present are {0, 1, 2, 4}, i.e.
None, Sub, Up and Paeth all appear in one image.

The damage from getting that wrong is quiet rather than loud: filtered rows are
DELTAS, so reading them as colours yields a plausible-looking palette and a
plausible-looking non-black percentage. Nothing raises. The numbers are simply
about a different image than the one on screen. That is the exact failure shape
this project keeps re-encountering -- a probe that returns a confident answer
about nothing -- so the filter set is asserted rather than assumed.

Usage:
    python tools/png-crop.py IN.png --measure
    python tools/png-crop.py IN.png --crop Y0 Y1 --scale 3 --out OUT.png

Exit codes: 2 for a malformed invocation, 1 when the input cannot be decoded,
0 only when the requested action completed.
"""

import argparse
import struct
import sys
import zlib
from collections import Counter


def read_chunks(data):
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("not a PNG: first 8 bytes are %s" % data[:8].hex(" "))
    pos = 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        yield tag, payload
        pos += 12 + length


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode(path):
    """Return (width, height, rgb_bytes, filters_seen).

    Implements all five PNG filter types. `filters_seen` is returned so callers
    can record WHICH filters the image used -- that set is the whole reason this
    decoder exists, and printing it is what makes the correctness claim checkable
    rather than asserted.
    """
    data = open(path, "rb").read()
    width = height = None
    depth = colortype = None
    idat = b""
    for tag, payload in read_chunks(data):
        if tag == b"IHDR":
            width, height, depth, colortype = struct.unpack(">IIBB", payload[:10])
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
    if width is None:
        raise ValueError("no IHDR chunk")
    if depth != 8 or colortype != 2:
        raise ValueError("only 8-bit truecolour supported (got depth=%d colortype=%d)"
                         % (depth, colortype))

    raw = zlib.decompress(idat)
    stride = width * 3
    expect = (stride + 1) * height
    if len(raw) < expect:
        raise ValueError("inflated data short: %d bytes, need %d" % (len(raw), expect))

    out = bytearray(stride * height)
    filters_seen = set()
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        filters_seen.add(ftype)
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ftype == 0:
            pass
        elif ftype == 1:
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - 3] if i >= 3 else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                left = line[i - 3] if i >= 3 else 0
                upleft = prev[i - 3] if i >= 3 else 0
                line[i] = (line[i] + paeth(left, prev[i], upleft)) & 0xFF
        else:
            raise ValueError("unknown PNG filter type %d on row %d" % (ftype, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, bytes(out), filters_seen


def measure(width, height, rgb, filters_seen, band=50):
    print("geometry        : %dx%d" % (width, height))
    print("filters present : %s" % sorted(filters_seen))
    if filters_seen - {0}:
        print("                  (adaptive filtering IS in use -- a raw scanline")
        print("                   read of this file would report a different image)")
    stride = width * 3
    counts = Counter()
    nonblack = 0
    for i in range(0, stride * height, 3):
        t = (rgb[i], rgb[i + 1], rgb[i + 2])
        counts[t] += 1
        if t != (0, 0, 0):
            nonblack += 1
    total = width * height
    print("distinct colors : %d" % len(counts))
    print("non-black       : %d of %d (%.3f%%)" % (nonblack, total, 100.0 * nonblack / total))
    for colour, n in counts.most_common(8):
        print("   rgb%-16s %8d  (%.3f%%)" % (str(colour), n, 100.0 * n / total))
    print()
    print("non-black per %d-row band:" % band)
    for top in range(0, height, band):
        bottom = min(top + band, height)
        k = 0
        for y in range(top, bottom):
            base = y * stride
            for x in range(0, stride, 3):
                if rgb[base + x] or rgb[base + x + 1] or rgb[base + x + 2]:
                    k += 1
        print("   y=%4d-%4d : %7d" % (top, bottom - 1, k))


def write_crop(width, height, rgb, y0, y1, scale, out_path, x0=0, x1=None):
    """Write rows [y0,y1) and columns [x0,x1), integer-upscaled by `scale`.

    Column cropping exists because of a display limit, not a decode limit. A
    1280-wide framebuffer band is downscaled to fit whatever is viewing it, and
    an 8x16 console font does not survive that -- the crop renders, and the text
    in it is unreadable, which looks exactly like a blank screen. Cutting the
    columns down to the region that actually holds glyphs is what makes an
    upscale change anything.
    """
    y0 = max(0, y0)
    y1 = min(height, y1)
    if y1 <= y0:
        raise ValueError("empty crop: y0=%d y1=%d" % (y0, y1))
    x0 = max(0, x0)
    x1 = width if x1 is None else min(width, x1)
    if x1 <= x0:
        raise ValueError("empty crop: x0=%d x1=%d" % (x0, x1))
    stride = width * 3
    out_w = x1 - x0
    rows = bytearray()
    for y in range(y0, y1):
        base = y * stride
        line = bytearray()
        for x in range(x0 * 3, x1 * 3, 3):
            line += rgb[base + x:base + x + 3] * scale
        for _ in range(scale):
            rows.append(0)
            rows += line

    def chunk(tag, payload):
        head = struct.pack(">I", len(payload)) + tag + payload
        return head + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    # out_w, NOT width -- a column crop that keeps the old IHDR width writes a
    # header describing an image wider than the IDAT it ships. Decoders trust
    # IHDR, so the rows get re-sliced at the wrong stride and the picture shears
    # diagonally rather than raising.
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", out_w * scale,
                                     (y1 - y0) * scale, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += chunk(b"IEND", b"")
    open(out_path, "wb").write(png)
    print("wrote %s: %dx%d, %d bytes (src x=%d-%d y=%d-%d scale=%d)"
          % (out_path, out_w * scale, (y1 - y0) * scale, len(png),
             x0, x1 - 1, y0, y1 - 1, scale))


def main(argv=None):
    p = argparse.ArgumentParser(prog="png-crop.py",
                               description="Decode, measure and crop a PNG with the stdlib only.")
    p.add_argument("image")
    p.add_argument("--measure", action="store_true",
                   help="print geometry, filter set, palette and per-band content")
    p.add_argument("--crop", nargs=2, type=int, metavar=("Y0", "Y1"),
                   help="crop rows [Y0, Y1)")
    p.add_argument("--xcrop", nargs=2, type=int, metavar=("X0", "X1"),
                   default=None, help="crop columns [X0, X1); default is the full width")
    p.add_argument("--scale", type=int, default=1, help="integer upscale factor")
    p.add_argument("--out", default=None, help="where to write the crop")
    p.add_argument("--band", type=int, default=50, help="row band size for --measure")
    args = p.parse_args(argv)

    if not args.measure and not args.crop:
        p.error("nothing to do: pass --measure and/or --crop Y0 Y1")
    if args.crop and not args.out:
        p.error("--crop needs --out")

    try:
        width, height, rgb, filters_seen = decode(args.image)
    except (OSError, ValueError, zlib.error) as exc:
        sys.stderr.write("FAIL: cannot decode %s: %s\n" % (args.image, exc))
        return 1

    if args.measure:
        measure(width, height, rgb, filters_seen, band=args.band)
    if args.crop:
        if args.measure:
            print()
        x0, x1 = (args.xcrop if args.xcrop else (0, width))
        # An inverted or empty range is a MALFORMED INVOCATION, not a corrupt
        # input, so it exits 2 like every other usage error in this tree. Left
        # to propagate it printed a traceback and exited 1 -- the code this
        # project reserves for "the thing you asked about is not there", which
        # would read as a decode failure on a file that decoded fine.
        try:
            write_crop(width, height, rgb, args.crop[0], args.crop[1],
                       max(1, args.scale), args.out, x0=x0, x1=x1)
        except ValueError as exc:
            sys.stderr.write("FAIL: %s\n" % exc)
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
