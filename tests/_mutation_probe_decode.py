#!/usr/bin/env python3
"""
Separating-input sweep for NVDecode.hpp and NVRiscvUcode.hpp.

NOT a gate and NOT part of any suite -- a throwaway analysis tool, run by hand
while closing t-23d65d. It answers ONE question:

    For each decoder, is there a plausible WRONG variant that every input the
    suite already passes would evaluate identically?

The method is the one that found the four survivors in test_falcon.cpp: build
the mutants a refactor actually produces, evaluate correct-vs-mutant on the
EXACT arguments the .cpp passes, and report any mutant no input separates.

TWO THINGS THIS VERSION FIXES over the first draft, both of which made it
OVERSTATE the survivor count:

  1. Inputs are keyed PER DECODER, not per register. The first draft gave every
     BOOT_0 field the same four-value list, which credits nvBoot0MinorRevision
     with the split-architecture assertions it never receives -- and, worse,
     credits nvBoot0Architecture0 with inputs that only reach it through
     nvBoot0Architecture(). Per-register keying inflates coverage for narrow
     fields and deflates it for none, so it is wrong in the unsafe direction.

  2. Constant-level assertions are modelled. test_decode.cpp:117 pins
     ARCHITECTURE_0_MASK == (1<<WIDTH)-1 and :154 assembles 0x17700000 from the
     SHIFT/MASK constants. Those kill mask and shift mutants without ever
     calling the decoder, so a probe that ignores them reports survivors that
     the suite does in fact catch.

Constants are PARSED OUT OF THE HEADERS, not restated here. A probe that
restates them tests the restatement.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEXT = os.path.join(ROOT, "phase0-kext")


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


D = parse_defines(os.path.join(KEXT, "NVDecode.hpp"))
R = parse_defines(os.path.join(KEXT, "NVRiscvUcode.hpp"))

M32 = 0xFFFFFFFF


def extract(value, shift, mask):
    return (value >> shift) & mask if 0 <= shift < 32 else 0


# ---------------------------------------------------------------------------
# The EXACT arguments each decoder receives, read off the .cpp by line number.
# A decoder's list contains ONLY the values that reach THAT function.
# ---------------------------------------------------------------------------
B0_GA107 = 0x177000A1
B42_GA107 = 0x177010A1

# testAdjacentBits() -- the synthetic separating inputs added while closing
# t-23d65d. Each sets exactly one bit: either the bit immediately ABOVE a field
# (which a widened mask swallows) or a field's own TOP bit (which a narrowed
# mask drops).
ADJ_B0_MINOR = [("adj bit 4  0x00000010", 0x00000010),
                ("adj bit 3  0x00000008", 0x00000008)]
ADJ_B0_MAJOR = [("adj bit 8  0x00000100", 0x00000100)]
ADJ_B0_IMPL  = [("adj bit 23 0x00800000", 0x00800000)]
ADJ_B0_ARCH  = [("adj bit 9  0x00000200", 0x00000200)]
ADJ_B42_MINOREXT = [("adj 11:8   0x00000F00", 0x00000F00),
                    ("adj bit 12 0x00001000", 0x00001000)]
ADJ_B42_MINOR    = [("adj 15:12  0x0000F000", 0x0000F000),
                    ("adj bit 16 0x00010000", 0x00010000)]
ADJ_B42_MAJOR    = [("adj 19:16  0x000F0000", 0x000F0000),
                    ("adj bit 20 0x00100000", 0x00100000)]
ADJ_B42_IMPL     = [("adj bit 23 0x00800000", 0x00800000)]
ADJ_B42_ARCH     = [("adj bit 30 0x40000000", 0x40000000),
                    ("adj bit 29 0x20000000", 0x20000000)]
ADJ_B42_CHIPID   = [("adj bit 30 0x40000000", 0x40000000),
                    ("adj bit 29 0x20000000", 0x20000000)]

# test_decode.cpp:77-96 (testBoot0) and :413-416 via NV3050Probe -- but only the
# suite counts, so: kBoot0_GA107 alone reaches these four.
PLAIN_B0 = [("kBoot0_GA107 0x177000A1", B0_GA107)]

# nvBoot0Architecture() is the ONLY caller of the ARCHITECTURE_0/_1 extractions,
# and testSplitArchitecture (:104-113) drives it with three more values.
ARCH_B0 = [
    ("kBoot0_GA107 0x177000A1", B0_GA107),
    ("bit8 clear   0x17000000", 0x17000000),
    ("bit8 set     0x17000100", 0x17000100),
    ("max          0x1F000100", 0x1F000100),
]

# testBoot42 (:131-133) calls Architecture/Implementation/ChipId only.
# MinorExt/Minor/Major have NO call site in the suite at all -- see the report.
PLAIN_B42 = [("kBoot42_GA107 0x177010A1", B42_GA107)]
NO_CALLSITE = []

# testGfwBoot (:190-199).
GFW_INPUTS = [
    ("0x000000FF", 0x000000FF),
    ("0x0000000F", 0x0000000F),
    ("0xDEADBEFF", 0xDEADBEFF),
    ("0xFFFFFFFF", 0xFFFFFFFF),
]


# ---------------------------------------------------------------------------
# Field table. Each entry mirrors ONE extraction in NVDecode.hpp as written.
# ---------------------------------------------------------------------------
FIELDS = [
    ("nvBoot0MinorRevision",     "NV_PMC_BOOT_0_MINOR_REVISION_SHIFT",
     "NV_PMC_BOOT_0_MINOR_REVISION_MASK", PLAIN_B0 + ADJ_B0_MINOR),
    ("nvBoot0MajorRevision",     "NV_PMC_BOOT_0_MAJOR_REVISION_SHIFT",
     "NV_PMC_BOOT_0_MAJOR_REVISION_MASK", PLAIN_B0 + ADJ_B0_MAJOR),
    ("nvBoot0Implementation",    "NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT",
     "NV_PMC_BOOT_0_IMPLEMENTATION_MASK", PLAIN_B0 + ADJ_B0_IMPL),
    ("nvBoot0Architecture:lo",   "NV_PMC_BOOT_0_ARCHITECTURE_0_SHIFT",
     "NV_PMC_BOOT_0_ARCHITECTURE_0_MASK", ARCH_B0 + ADJ_B0_ARCH),
    ("nvBoot0Architecture:hi",   "NV_PMC_BOOT_0_ARCHITECTURE_1_SHIFT",
     "NV_PMC_BOOT_0_ARCHITECTURE_1_MASK", ARCH_B0 + ADJ_B0_ARCH),
    ("nvBoot42MinorExtRevision", "NV_PMC_BOOT_42_MINOR_EXT_REVISION_SHIFT",
     "NV_PMC_BOOT_42_MINOR_EXT_REVISION_MASK", ADJ_B42_MINOREXT),
    ("nvBoot42MinorRevision",    "NV_PMC_BOOT_42_MINOR_REVISION_SHIFT",
     "NV_PMC_BOOT_42_MINOR_REVISION_MASK", ADJ_B42_MINOR),
    ("nvBoot42MajorRevision",    "NV_PMC_BOOT_42_MAJOR_REVISION_SHIFT",
     "NV_PMC_BOOT_42_MAJOR_REVISION_MASK", ADJ_B42_MAJOR),
    ("nvBoot42Implementation",   "NV_PMC_BOOT_42_IMPLEMENTATION_SHIFT",
     "NV_PMC_BOOT_42_IMPLEMENTATION_MASK", PLAIN_B42 + ADJ_B42_IMPL),
    ("nvBoot42Architecture",     "NV_PMC_BOOT_42_ARCHITECTURE_SHIFT",
     "NV_PMC_BOOT_42_ARCHITECTURE_MASK", PLAIN_B42 + ADJ_B42_ARCH),
    ("nvBoot42ChipId",           "NV_PMC_BOOT_42_CHIP_ID_SHIFT",
     "NV_PMC_BOOT_42_CHIP_ID_MASK", PLAIN_B42 + ADJ_B42_CHIPID),
    ("nvGfwBootProgress",        "NV_GFW_BOOT_PROGRESS_SHIFT",
     "NV_GFW_BOOT_PROGRESS_MASK", GFW_INPUTS),
]


def mutants_for(shift, mask):
    """The wrong variants a refactor actually produces."""
    out = []
    for delta in (-2, -1, 1, 2):
        s = shift + delta
        if 0 <= s < 32:
            out.append(("shift%+d" % delta, s, mask))
    if shift != 0:
        out.append(("shift dropped", 0, mask))
    wider = (mask << 1) | 1
    if wider <= M32 and wider != mask:
        out.append(("mask widened 0x%X->0x%X" % (mask, wider), shift, wider))
    narrower = mask >> 1
    if narrower != mask and narrower != 0:
        out.append(("mask narrowed 0x%X->0x%X" % (mask, narrower), shift, narrower))
    return out


# ---------------------------------------------------------------------------
# Constant-level assertions that kill mutants WITHOUT calling the decoder.
# Each returns True when the mutated (shift, mask) still satisfies it; a False
# means that assertion is the killer.
# ---------------------------------------------------------------------------
def killed_by_constant_checks(field, shift, mask):
    """Return the label of the constant assertion that catches this mutant."""
    # test_decode.cpp:117 -- ARCHITECTURE_0_MASK == (1 << WIDTH) - 1
    if field == "nvBoot0Architecture:lo":
        width = D["NV_PMC_BOOT_0_ARCHITECTURE_0_WIDTH"]
        if mask != (1 << width) - 1:
            return "test_decode.cpp:117 ARCHITECTURE_0_MASK matches WIDTH"

    # test_decode.cpp:154 -- assembled == 0x17700000, built from ARCH_0 MASK and
    # SHIFT plus IMPLEMENTATION_SHIFT.
    arch = D["NV_ARCHITECTURE_GA100"]
    impl = D["NV_IMPLEMENTATION_GA107"]
    a_shift = D["NV_PMC_BOOT_0_ARCHITECTURE_0_SHIFT"]
    a_mask = D["NV_PMC_BOOT_0_ARCHITECTURE_0_MASK"]
    i_shift = D["NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT"]
    if field == "nvBoot0Architecture:lo":
        a_shift, a_mask = shift, mask
    if field == "nvBoot0Implementation":
        i_shift = shift
    assembled = (((arch & a_mask) << a_shift) | (impl << i_shift)) & M32
    if assembled != 0x17700000:
        return "test_decode.cpp:159 assembled BOOT_0 base == 0x17700000"

    return None


def separating_input(shift, mask, ms, mm):
    """Smallest single-bit value on which correct and mutant disagree."""
    for bit in range(32):
        v = 1 << bit
        if extract(v, shift, mask) != extract(v, ms, mm):
            return v, bit
    return None, None


def sweep_fields():
    survivors = []
    total = 0
    for name, sname, mname, cases in FIELDS:
        if sname not in D or mname not in D:
            print("  !! missing constant for %s (%s / %s)" % (name, sname, mname))
            continue
        shift, mask = D[sname], D[mname]
        for label, ms, mm in mutants_for(shift, mask):
            total += 1
            if killed_by_constant_checks(name, ms, mm):
                continue
            killers = [c for c, v in cases
                       if extract(v, shift, mask) != extract(v, ms, mm)]
            if not killers:
                survivors.append((name, shift, mask, label, ms, mm, cases))
    return total, survivors


def main():
    print("=" * 74)
    print("NVDecode.hpp -- per-decoder mutation sweep")
    print("=" * 74)
    print("constants parsed: NVDecode.hpp %d, NVRiscvUcode.hpp %d" % (len(D), len(R)))
    print("decoders probed : %d" % len(FIELDS))
    print()

    print("FIELD TABLE (shift/mask parsed from the header)")
    for name, sname, mname, cases in FIELDS:
        s, m = D[sname], D[mname]
        w = m.bit_length()
        print("  %-26s shift %2d  mask 0x%-4X  bits %2d:%-2d  bit above %2d  "
              "inputs %d" % (name, s, m, s + w - 1, s, s + w, len(cases)))
    print()

    total, survivors = sweep_fields()
    print("mutants built   : %d" % total)
    print("killed          : %d" % (total - len(survivors)))
    print("SURVIVING       : %d" % len(survivors))
    print()

    for name, shift, mask, label, ms, mm, cases in survivors:
        sep, bit = separating_input(shift, mask, ms, mm)
        if sep is None:
            sepstr = "EQUIVALENT -- no separating input exists"
        else:
            sepstr = ("0x%08X bit %2d -> correct 0x%X mutant 0x%X"
                      % (sep, bit, extract(sep, shift, mask), extract(sep, ms, mm)))
        tag = "NO CALL SITE" if not cases else "%d input(s)" % len(cases)
        print("  %-26s %-26s [%-12s] %s" % (name, label, tag, sepstr))

    return 0


if __name__ == "__main__":
    sys.exit(main())
