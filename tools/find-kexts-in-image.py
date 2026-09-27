#!/usr/bin/env python3
r"""find-kexts-in-image.py -- enumerate kext bundle names present in a raw disk image.

Why this exists
---------------
`VM-BRINGUP.md` line 712 states, in prose, that "every prelinked path
containing 'Intel' returns only graphics framebuffers, LPSS" -- a summary of a
walk that was performed once and never written down as a list. That one
sentence is load-bearing for the whole question of whether macOS carries any
Intel iGPU support, so it needs to be a measured list rather than a remembered
impression.

This is a byte scan, not a filesystem walk. It reads the raw image in
overlapping chunks and pulls out every ASCII run matching a kext path or a
bundle identifier. That means it sees strings inside the prelinked kernelcache's
info plist as well as strings on disk, which is exactly what is wanted here: the
question is "does this name appear anywhere in the shipped image", not "is there
a directory entry for it".

What a null result from this tool does and does not prove
--------------------------------------------------------
A HIT is positive evidence: the name is in the image.

A MISS is weaker than it looks, and must not be reported as "macOS does not
ship this driver":

  1. A compressed kernelcache (LZSS/LZFSE) hides its strings from a raw scan.
     The tool reports whether it found ANY `/System/Library/Extensions/` paths
     at all -- if that count is zero, the scan is blind and every miss is
     vacuous. Check that line before believing any absence.
  2. A recovery image is a trimmed subset of a full install. Absence in
     BaseSystem.img says nothing about the installed system volume.

Usage:
    python tools/find-kexts-in-image.py <image> [--filter SUBSTR] [--limit N]
"""

import argparse
import re
import sys

# 64 MiB reads with a 4 KiB overlap, so a path straddling a chunk boundary is
# still seen whole by one of the two reads.
CHUNK = 64 * 1024 * 1024
OVERLAP = 4096

# Bundle paths as they appear in a prelink info plist, and bare bundle
# identifiers (com.apple.driver.Foo). Both are plain ASCII in the plist.
PATH_RE = re.compile(rb"/System/Library/Extensions/[A-Za-z0-9_./+-]{1,180}\.kext")
BUNDLE_RE = re.compile(rb"com\.apple\.(?:driver|iokit)\.[A-Za-z0-9_.]{1,120}")


def scan(path, progress=True):
    """Return (kext_paths, bundle_ids) as sorted lists of str."""
    paths = set()
    bundles = set()
    tail = b""
    read_bytes = 0

    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(CHUNK)
            if not chunk:
                break
            read_bytes += len(chunk)
            buf = tail + chunk

            for m in PATH_RE.finditer(buf):
                paths.add(m.group(0).decode("ascii", "replace"))
            for m in BUNDLE_RE.finditer(buf):
                bundles.add(m.group(0).decode("ascii", "replace"))

            tail = buf[-OVERLAP:]
            if progress:
                sys.stderr.write(
                    "\r  scanned %6.2f GiB, %5d paths, %5d bundles"
                    % (read_bytes / (1024.0 ** 3), len(paths), len(bundles))
                )
                sys.stderr.flush()

    if progress:
        sys.stderr.write("\n")
    return sorted(paths), sorted(bundles)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", help="raw disk image to scan")
    ap.add_argument(
        "--filter",
        default=None,
        help="only print entries containing this substring (case-insensitive)",
    )
    ap.add_argument("--limit", type=int, default=0, help="cap printed entries (0 = all)")
    args = ap.parse_args(argv)

    try:
        paths, bundles = scan(args.image)
    except OSError as exc:
        print("ERROR: cannot read %s: %s" % (args.image, exc))
        return 2

    # This line decides whether every MISS below is meaningful or vacuous.
    print("scan reach: %d kext paths, %d bundle identifiers found in total"
          % (len(paths), len(bundles)))
    if not paths and not bundles:
        print("VACUOUS: no kext strings of any kind were found. The scan is blind")
        print("         (compressed kernelcache, or wrong image). Absence of any")
        print("         particular driver below proves NOTHING.")
        return 1

    def show(title, items):
        sel = items
        if args.filter:
            needle = args.filter.lower()
            sel = [i for i in items if needle in i.lower()]
        print("\n=== %s (%d shown of %d) ===" % (title, len(sel), len(items)))
        if not sel:
            print("  (none matched)")
        for i in sel[: args.limit] if args.limit else sel:
            print("  " + i)

    show("kext paths", paths)
    show("bundle identifiers", bundles)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
