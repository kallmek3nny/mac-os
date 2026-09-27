#!/usr/bin/env python3
"""
Separating-input sweep for NVRiscvUcode.hpp.

NOT a gate and NOT part of any suite -- a throwaway analysis tool, run by hand
while closing t-23d65d. Companion to _mutation_probe_decode.py, which covers
NVDecode.hpp. It answers the same single question for the RISC-V parser:

    For each predicate, is there a plausible WRONG variant that every input
    test_riscv_ucode.cpp already passes would evaluate identically?

WHY THIS HALF IS SHAPED DIFFERENTLY FROM THE DECODE HALF

NVDecode.hpp is bit-field extraction, so its mutants are shift/mask slips and
the separating inputs are single bits. NVRiscvUcode.hpp is byte-offset
arithmetic over a buffer, so its mutants are boundary slips (<= vs <, off-by-one
on a length, a dropped overflow guard) and the separating inputs are lengths and
region extents. Enumerating shift+-1 here would be theatre.

THE SELF-CANCELLING CONSTANT, which is the finding this file exists to record:

    buildBlob() WRITES the container header through the SAME #define that
    nvRiscvBinHdr*() READS it through. put32(NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET,
    ...) and nvRiscvBinHdrDataOffset() both resolve to 0x10. Mutate that define
    and the fixture moves the field in lockstep with the reader, so every
    assertion still passes. Those mutants are UNDETECTABLE BY CONSTRUCTION --
    not weak coverage, but a fixture that cannot observe the thing it writes.

    The suite already defended SOME of the DESCRIPTOR offsets against this, by
    asserting each against a bare literal (0x10, 0x14, 0x1C ...). It defended
    none of the six CONTAINER-header offsets, and it skipped nine descriptor
    entries -- the size fields sitting between two pinned offset fields, on the
    reasoning that the pinned neighbours covered them. They did not: the gap
    between two pinned offsets is exactly where a define moves undetected.

    Both holes were closed on 2026-08-13 while working t-23d65d. All six
    container offsets and the full 0x00..0x58 descriptor sequence are now
    compared against bare literals, and re-running this probe reports 0
    undefended in each group. That is what this probe is for now: a regression
    check that the pinning has not been thinned out again.

Constants are PARSED OUT OF THE HEADER, not restated here. A probe that
restates constants tests the restatement.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEXT = os.path.join(ROOT, "phase0-kext")
TESTS = os.path.dirname(os.path.abspath(__file__))


def parse_defines(path):
    """#define NAME <int literal>  ->  {NAME: value}. Ignores everything else."""
    out = {}
    pat = re.compile(r"^#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+"
                     r"((?:0[xX][0-9A-Fa-f]+)|(?:\d+))\b")
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = pat.match(line)
            if m:
                out[m.group(1)] = int(m.group(2), 0)
    return out


R = parse_defines(os.path.join(KEXT, "NVRiscvUcode.hpp"))
M32 = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Part 1 -- predicate mutants.
#
# nvRiscvFits(len, off, size) is the single bounds primitive. Every region and
# descriptor check routes through it, so its mutants are the ones that matter.
# ---------------------------------------------------------------------------
def fits_correct(length, off, size):
    # Overflow-safe: never computes off + size.
    return off <= length and size <= (length - off)


def fits_naive_add(length, off, size):
    """The mistake the header's comment names: off + size wraps at 32 bits."""
    return ((off + size) & M32) <= length


def fits_strict_off(length, off, size):
    return off < length and size <= (length - off)


def fits_strict_size(length, off, size):
    return off <= length and size < (length - off)


def fits_no_off_guard(length, off, size):
    """Drops the off <= len guard; (len - off) then underflows to a huge value."""
    return size <= ((length - off) & M32)


def fits_off_by_one(length, off, size):
    return off <= length and size <= (length - off + 1)


FITS_MUTANTS = [
    ("naive off+size (wraps)", fits_naive_add),
    ("off < len instead of <=", fits_strict_off),
    ("size < remaining instead of <=", fits_strict_size),
    ("off <= len guard dropped", fits_no_off_guard),
    ("remaining off-by-one (+1)", fits_off_by_one),
]


# The EXACT (len, off, size) triples the suite drives nvRiscvFits with, read off
# test_riscv_ucode.cpp by line number. Region checks pass payload length as len.
DATA_SIZE = R["NV_RISCV_GA107_BL_DATA_SIZE"]
BL_OFF = R["NV_RISCV_GA107_BL_BOOTLOADER_OFFSET"]
BL_SIZE = R["NV_RISCV_GA107_BL_BOOTLOADER_SIZE"]
PARAM_OFF = R["NV_RISCV_GA107_BL_BOOTLOADER_PARAM_OFF"]
PARAM_SIZE = R["NV_RISCV_GA107_BL_BOOTLOADER_PARAM_SIZE"]

FITS_CASES = [
    # test_riscv_ucode.cpp:284-296 -- the real regions.
    (":284 bootloader region", DATA_SIZE, BL_OFF, BL_SIZE),
    (":294 param region", DATA_SIZE, PARAM_OFF, PARAM_SIZE),
    # :303-310 -- the boundary quartet.
    (":303 ends exactly at end", DATA_SIZE, DATA_SIZE - 16, 16),
    (":305 one byte past end", DATA_SIZE, DATA_SIZE - 16, 17),
    (":307 zero-size at end", DATA_SIZE, DATA_SIZE, 0),
    (":309 off past end, size 0", DATA_SIZE, DATA_SIZE + 1, 0),
    # :317-320 -- the overflow pair.
    (":317 0xFFFFFFF0 + 0x20", DATA_SIZE, 0xFFFFFFF0, 0x20),
    (":319 huge size", DATA_SIZE, 0, 0xFFFFFFFF),
]


def sweep_fits():
    survivors = []
    for label, fn in FITS_MUTANTS:
        killers = [c for c, l, o, s in FITS_CASES
                   if fits_correct(l, o, s) != fn(l, o, s)]
        if not killers:
            survivors.append((label, fn))
    return survivors


def separating_fits(fn):
    """Smallest plausible triple on which correct and mutant disagree."""
    for length in (0, 1, 16, DATA_SIZE):
        for off in (0, 1, length, length + 1, 0xFFFFFFF0):
            for size in (0, 1, 16, 0xFFFFFFFF):
                if fits_correct(length, off, size) != fn(length, off, size):
                    return (length, off, size)
    return None


# ---------------------------------------------------------------------------
# Part 2 -- the self-cancelling constants.
#
# An offset is DEFENDED only if the suite compares it to a bare literal. If the
# only thing that reads it is an accessor whose fixture wrote it through the
# same define, the two move together and no assertion can separate them.
# ---------------------------------------------------------------------------
def literal_pinned_offsets():
    """Offset defines the suite compares against a bare hex literal."""
    src = os.path.join(TESTS, "test_riscv_ucode.cpp")
    with open(src, encoding="utf-8") as fh:
        text = fh.read()
    # checkU32("...", NV_RISCV_..., 0x..u)  -- the constant-pinning form.
    pat = re.compile(r"(NV_RISCV_[A-Z0-9_]+)\s*,\s*(0[xX][0-9A-Fa-f]+|\d+)u")
    return {m.group(1) for m in pat.finditer(text)}


CONTAINER_OFFSETS = [
    "NV_RISCV_BIN_HDR_MAGIC_OFFSET",
    "NV_RISCV_BIN_HDR_VERSION_OFFSET",
    "NV_RISCV_BIN_HDR_BIN_SIZE_OFFSET",
    "NV_RISCV_BIN_HDR_HDR_OFFSET_OFFSET",
    "NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET",
    "NV_RISCV_BIN_HDR_DATA_SIZE_OFFSET",
]

# NV_RISCV_DESC_* is not all byte offsets. FIELDS_THROUGH_SIGNED_AS_CODE (21)
# and FIELDS_THROUGH_PLIC (23) are field COUNTS, and they are small enough to
# clear the < 0x100 filter, so an earlier version of this list printed them as
# offsets 0x15 and 0x17 and reported them SELF-CANCELLING. That verdict was
# meaningless: a count is not written into the blob by buildBlob() at all, so
# there is no fixture/accessor pair to cancel. They are pinned in the suite by
# asserting the RELATIONSHIP to the offset they describe -- (offset / 4) + 1 --
# which is stronger than a literal, and is invisible to the regex below.
DESC_OFFSETS = [k for k in sorted(R) if k.startswith("NV_RISCV_DESC_")
                and not k.endswith("_5")
                and "FIELDS_THROUGH" not in k
                and R[k] < 0x100]


def main():
    print("=" * 74)
    print("NVRiscvUcode.hpp -- predicate and offset mutation sweep")
    print("=" * 74)
    print("constants parsed: NVRiscvUcode.hpp %d" % len(R))
    print()

    print("PART 1 -- nvRiscvFits() mutants vs the suite's exact triples")
    print("  payload length = NV_RISCV_GA107_BL_DATA_SIZE = 0x%X" % DATA_SIZE)
    for c, l, o, s in FITS_CASES:
        print("    %-28s len 0x%-6X off 0x%-10X size 0x%-10X -> %s"
              % (c, l, o, s, fits_correct(l, o, s)))
    print()

    survivors = sweep_fits()
    print("  mutants built   : %d" % len(FITS_MUTANTS))
    print("  killed          : %d" % (len(FITS_MUTANTS) - len(survivors)))
    print("  SURVIVING       : %d" % len(survivors))
    for label, fn in survivors:
        sep = separating_fits(fn)
        if sep is None:
            print("    %-34s EQUIVALENT -- no separating input" % label)
        else:
            l, o, s = sep
            print("    %-34s len 0x%X off 0x%X size 0x%X -> correct %s mutant %s"
                  % (label, l, o, s, fits_correct(l, o, s), fn(l, o, s)))
    print()

    print("PART 2 -- offsets the fixture writes through the define it reads")
    pinned = literal_pinned_offsets()
    print("  offsets pinned to a bare literal in the suite: %d" % len(pinned))
    print()

    print("  CONTAINER header (nvfw_bin_hdr):")
    undefended = []
    for k in CONTAINER_OFFSETS:
        ok = k in pinned
        if not ok:
            undefended.append(k)
        print("    %-40s 0x%02X  %s"
              % (k, R[k], "pinned" if ok else "SELF-CANCELLING"))
    print()

    print("  DESCRIPTOR (RM_RISCV_UCODE_DESC), first 8:")
    desc_undefended = [k for k in DESC_OFFSETS if k not in pinned]
    for k in DESC_OFFSETS[:8]:
        print("    %-40s 0x%02X  %s"
              % (k, R[k], "pinned" if k in pinned else "SELF-CANCELLING"))
    print("    ... %d descriptor offsets total, %d unpinned"
          % (len(DESC_OFFSETS), len(desc_undefended)))
    print()

    print("SUMMARY")
    print("  nvRiscvFits mutants surviving : %d" % len(survivors))
    print("  container offsets undefended  : %d of %d"
          % (len(undefended), len(CONTAINER_OFFSETS)))
    for k in undefended:
        print("    %s" % k)

    return 0


if __name__ == "__main__":
    sys.exit(main())
