#!/bin/sh
#
# igpu-final-capture.sh -- ONE file, ONE command, ONE boot:
#
#     sudo sh igpu-final-capture.sh
#
# Read-only. mmaps BAR0 through sysfs, binds nothing, unbinds nothing, sets no
# mode. The panel stays lit for the whole run, so every value below is a value
# that was driving the screen at the moment it was read.
#
# WHY THIS FILE EXISTS
# --------------------
# The Intel track has had exactly ONE hour of silicon in its whole life:
# igpu-results/tgl-capture-2026-08-16-022556.txt, produced by igpu-plane-dsi.sh.
# Six capture scripts accumulated in tools/ across that effort, each answering
# the question the previous one raised:
#
#   capture-igpu-facts.sh     PCI/sysfs/DRM inventory, no MMIO at all
#   capture-igpu-display.sh   first MMIO reads, pipe/transcoder A..C
#   igpu-capture.sh           embedded dumper, 15 canonical ranges
#   igpu-decode.sh            decode pass over a previous capture's bytes
#   igpu-followup.sh          pipe/transcoder D plus DDI/AUX
#   igpu-plane-dsi.sh         plane slots, DSI pages, the PIPESRC verdict
#
# Running six scripts is six chances to run five. This file is the UNION of all
# six plus the pages none of them ever read, so the Linux phase is one command
# and then finished forever. That last clause is the binding constraint: after
# this run the machine gets macOS installed over it, so anything omitted here
# cannot be captured later at any price. When in doubt this script DUMPS.
#
# WHY THE DUMPER IS EMBEDDED RATHER THAN A COMPANION FILE
# -------------------------------------------------------
# The 2026-08-15 15:50 UTC attempt failed for exactly one reason: a second file
# was not copied alongside the first. tools/igpu-capture.sh already embeds its
# dumper for that reason and this one does the same. There is no second file.
#
# RELATIONSHIP TO THE THREE CANONICAL RANGE TABLES  (read this before editing)
# ---------------------------------------------------------------------------
# tools/read-igpu-mmio.py, tools/read-igpu-mmio.sh and tools/igpu-capture.sh
# carry three copies of one 15-row range table, and tests/test_igpu_mmio_ranges.py
# exists to fail when they drift apart -- it demands the three be IDENTICAL, not
# merely overlapping. This file's table therefore CANNOT join that equality: it
# needs pages those three do not have (transcoder/pipe D, the DSI pages, the
# Gen12 PLL block at 0x164000, the PCH fuse and DPLL pages).
#
# So it is a documented SUPERSET instead, and the gate asserts that relationship
# rather than equality. Every one of the 15 canonical rows appears below with its
# base and length UNCHANGED; the additions are marked. If you add a range to the
# canonical three, add it here too or the superset assertion reddens -- which is
# the point of it.
#
# WHAT IS GROUNDED VS WHAT IS DUMPED RAW
# --------------------------------------
# Only offsets read out of the headers vendored in reference/i915/ are DECODED
# by name in the sections below. Everything else is dumped as a raw offset with
# no name, deliberately: a name invented here, sitting next to names traced to a
# file and a line, would destroy the only evidence trail this project has.
#
# Two pages are dumped by ADDRESS with no named anchor at all, and that is a
# statement about this checkout rather than about the hardware: panel power
# sequencing (0x0C7000) and backlight PWM (0x0C8000) are in the canonical table,
# but intel_pps_regs.h and intel_dp_aux_regs.h were never vendored into
# reference/i915/, so PP_STATUS, PP_CONTROL and the AUX_CH_CTL family have no
# citable line in this tree. Their words are captured; naming them is offline
# work against a header this repo does not carry.
#
# A NULL IS ONLY EVIDENCE IF THE READER IS PROVEN TO WORK
# ------------------------------------------------------
# An unclaimed page and a failed mmap both read as 0xFFFFFFFF, and -- MEASURED
# 2026-08-16 on 8086:9a68, handoff15 sec.2 -- an unused display page on THIS
# silicon reads 0x00000000 rather than all-ones. So a single read cannot tell an
# absent block from a present-but-unprogrammed one, and the script never claims
# it can. It reads a POSITIVE CONTROL first (transcoder A's HTOTAL on the lit
# pipe); if that control is dead, every subsequent null is reported INCONCLUSIVE
# rather than UNPROGRAMMED-OR-ABSENT, because the reader itself is then unproven.

set -u

if [ "$(id -u)" != "0" ]; then
    echo "must run as root: sudo sh $0" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found -- it is the only hard dependency (a shell cannot mmap a BAR)" >&2
    exit 1
fi

PCI=""
for p in /sys/bus/pci/devices/0000:00:02.0 /sys/bus/pci/devices/0000:00:02.1; do
    [ -e "$p/resource0" ] && PCI="$p" && break
done
if [ -z "$PCI" ]; then
    echo "no Intel iGPU at 00:02.x -- is this the right machine?" >&2
    exit 1
fi

STAMP=$(date -u +%Y%m%d-%H%M%S)
OUT="/root/igpu-final-$STAMP.txt"
EDIDDIR="/root/igpu-final-$STAMP-edid"
mkdir -p "$EDIDDIR"

{
echo "igpu-final-capture $STAMP UTC"
echo "script    $0"
echo "kernel    $(uname -a)"
echo "distro    $(cat /etc/os-release 2>/dev/null | head -2 | tr '\n' ' ')"
echo
echo "==== PROVENANCE ===="
echo "pci       $PCI"
echo "vendor    $(cat $PCI/vendor 2>/dev/null)"
echo "device    $(cat $PCI/device 2>/dev/null)"
echo "subsystem $(cat $PCI/subsystem_vendor 2>/dev/null):$(cat $PCI/subsystem_device 2>/dev/null)"
echo "revision  $(cat $PCI/revision 2>/dev/null)"
echo "class     $(cat $PCI/class 2>/dev/null)"
echo "driver    $(basename $(readlink -f $PCI/driver 2>/dev/null) 2>/dev/null)"
echo "enable    $(cat $PCI/enable 2>/dev/null)"
echo "lockdown  $(cat /sys/kernel/security/lockdown 2>/dev/null || echo unavailable)"
echo "secureboot $(mokutil --sb-state 2>/dev/null || echo 'mokutil unavailable')"
echo "cmdline   $(cat /proc/cmdline)"
echo "iomem-cap $(cat /proc/sys/kernel/kptr_restrict 2>/dev/null)"
echo
echo "---- BAR layout (sysfs resource lines: start end flags) ----"
cat "$PCI/resource" 2>/dev/null | head -8
echo "resource0 bytes = $(stat -c %s $PCI/resource0 2>/dev/null)"
echo
echo "---- lspci ----"
lspci -nn -s "$(basename $PCI)" 2>/dev/null
lspci -vv -s "$(basename $PCI)" 2>/dev/null | head -60
echo
echo "---- PCI config space (first 256 bytes, hex) ----"
od -A x -t x1 "$PCI/config" 2>/dev/null | head -16
echo
echo "---- i915 module parameters ----"
for f in /sys/module/i915/parameters/*; do
    [ -e "$f" ] || continue
    echo "  $(basename $f) = $(cat $f 2>/dev/null)"
done
echo
echo "---- DRM connectors ----"
for c in /sys/class/drm/card*-*; do
    [ -e "$c/status" ] || continue
    echo "connector $(basename $c)"
    echo "    status    $(cat $c/status 2>/dev/null)"
    echo "    enabled   $(cat $c/enabled 2>/dev/null)"
    echo "    dpms      $(cat $c/dpms 2>/dev/null)"
    echo "    modes     [$(cat $c/modes 2>/dev/null | tr '\n' ' ')]"
    if [ -s "$c/edid" ]; then
        cp "$c/edid" "$EDIDDIR/$(basename $c).edid" 2>/dev/null
        echo "    edid      $(stat -c %s $c/edid 2>/dev/null) bytes -> saved"
        od -A x -t x1 "$c/edid" 2>/dev/null | head -8 | sed 's/^/      /'
    else
        echo "    edid      absent or empty"
    fi
done
echo
echo "---- DRM cards ----"
for d in /sys/class/drm/card*; do
    [ -L "$d/device" ] || continue
    echo "$(basename $d) -> $(readlink -f $d/device 2>/dev/null)"
done
echo
echo "---- dmesg, i915 and drm lines ----"
dmesg 2>/dev/null | grep -iE 'i915|drm|intel' | tail -80
echo
echo "---- debugfs, if mounted ----"
for f in /sys/kernel/debug/dri/*/i915_display_info \
         /sys/kernel/debug/dri/*/i915_dp_mst_info \
         /sys/kernel/debug/dri/*/i915_frequency_info \
         /sys/kernel/debug/dri/*/i915_capabilities; do
    [ -r "$f" ] || continue
    echo "===== $f ====="
    head -120 "$f" 2>/dev/null
done
echo
echo "==== MMIO ===="
} > "$OUT" 2>&1

python3 - "$PCI/resource0" >> "$OUT" 2>&1 <<'PYEOF'
import mmap, os, sys

path = sys.argv[1]
size = os.path.getsize(path)
fd = os.open(path, os.O_RDONLY)
span = min(size, 0x200000)
mm = mmap.mmap(fd, span, mmap.MAP_SHARED, mmap.PROT_READ)
print('BAR0 file size 0x%X, mapped 0x%X' % (size, span))
print()

# (base, length, label). Bases are 4 KiB aligned because mmap requires it.
# Labels are descriptive only -- the authority for any individual offset is
# reference/i915/i915_reg.h, never this list.
#
# SUPERSET of the 15-row canonical table carried by tools/read-igpu-mmio.py,
# tools/read-igpu-mmio.sh and tools/igpu-capture.sh. Those three must stay
# IDENTICAL to each other; this one must CONTAIN them. Rows marked (+) are the
# additions and exist because no predecessor script ever read them.
RANGES = [
    (0x044000, 0x2000, "north display engine interrupts / DE misc"),
    (0x046000, 0x1000, "CDCLK, DPLL control"),
    (0x051000, 0x1000, "(+) display fuse / SKL_DFSM"),
    (0x060000, 0x1000, "transcoder A timings"),
    (0x061000, 0x1000, "transcoder B timings"),
    (0x062000, 0x1000, "transcoder C timings"),
    (0x063000, 0x1000, "(+) transcoder D timings"),
    (0x064000, 0x1000, "(+) DDI buffer control and DP_TP_CTL"),
    (0x06B000, 0x1000, "(+) transcoder DSI0 and DSI1 (0x800 stride)"),
    (0x06C000, 0x1000, "DPCLKA / PLL distribution"),
    (0x06F000, 0x1000, "transcoder EDP timings (pre-Gen12 layout)"),
    (0x070000, 0x1000, "pipe A configuration and universal plane 1"),
    (0x071000, 0x1000, "pipe B configuration and universal plane 1"),
    (0x072000, 0x1000, "pipe C configuration and universal plane 1"),
    (0x073000, 0x1000, "(+) pipe D configuration and universal plane 1"),
    (0x07B000, 0x1000, "(+) pipe DSI0 and DSI1 (0x800 stride)"),
    (0x0C0000, 0x1000, "south display / port control"),
    (0x0C2000, 0x1000, "(+) SOUTH_CHICKEN1 and SFUSE_STRAP"),
    (0x0C6000, 0x1000, "(+) PCH DPLL and DREF control"),
    (0x0C7000, 0x1000, "panel power sequencing"),
    (0x0C8000, 0x1000, "backlight PWM"),
    (0x0E0000, 0x1000, "(+) TRANS_DP_CTL"),
    (0x160000, 0x2000, "combo PHY A/B"),
    (0x162000, 0x2000, "combo PHY C and DDI aux"),
    (0x164000, 0x1000, "(+) Gen12 DPLL config and DPCLKA_CFGCR"),
]

# Values that carry no information: an unmapped read, a cleared register, and
# the two obvious tell-tales of a dead mapping.
TRIVIAL = (0x00000000, 0xFFFFFFFF)


def rd(off):
    if off + 4 > len(mm):
        return None
    return int.from_bytes(mm[off:off+4], 'little')


def h(v):
    return 'unreadable' if v is None else '0x%08X' % v


def dead(v):
    """No usable signal at this offset.

    MEASURED 2026-08-16 on 8086:9a68: an unused display page on this silicon
    reads 0x00000000, NOT all-ones. Zero is therefore counted dead so no verdict
    word overclaims -- but the word itself stays UNPROGRAMMED-OR-ABSENT, because
    one read cannot separate a decoded-but-idle block from a missing one.
    """
    return v is None or v in TRIVIAL


# ---------------------------------------------------------------- REACH guard
# Prove the reader before believing any null. Transcoder A's HTOTAL is driving
# the lit panel at this instant, so a dead control means the MAPPING is broken,
# not that the hardware is quiet.
ctrl = rd(0x60000)
READER_OK = not dead(ctrl)
print('CONTROL   transcoder A HTOTAL @0x60000 = %s' % h(ctrl))
if READER_OK:
    print('          reader PROVEN -- a null below is a fact about the block')
    verdict_null = 'UNPROGRAMMED-OR-ABSENT'
else:
    print('          reader UNPROVEN -- every null below is INCONCLUSIVE.')
    print('          Do not read absences out of this run; fix the mapping first.')
    verdict_null = 'INCONCLUSIVE'
print()

# ------------------------------------------------------------- raw range dump
print('==== SECTION 1  raw range dump ====')
print('Every non-trivial dword in every range. Trivial words (0x00000000 and')
print('0xFFFFFFFF) are counted rather than printed, because on this silicon a')
print('page of zeros is the normal look of an unprogrammed block and printing')
print('1024 of them per page buries the evidence.')
print()
total_live = 0
for base, length, label in RANGES:
    words = []
    trivial = 0
    unreadable = 0
    for off in range(base, base + length, 4):
        v = rd(off)
        if v is None:
            unreadable += 1
        elif v in TRIVIAL:
            trivial += 1
        else:
            words.append((off, v))
    total_live += len(words)
    state = 'LIVE' if words else verdict_null
    print('range 0x%06X +0x%04X  %-44s %s' % (base, length, label, state))
    print('    %d live, %d trivial, %d unreadable' % (len(words), trivial, unreadable))
    for i in range(0, len(words), 4):
        row = words[i:i+4]
        print('      ' + '  '.join('0x%06X %s' % (o, h(v)) for o, v in row))
print()
print('total live dwords across all ranges: %d' % total_live)
print()

# ------------------------------------------------------- decoded, named, cited
# Only offsets traceable to a vendored header are named here.
#
#   0x60000 _TRANS_HTOTAL_A          i915_reg.h
#   0x60004 _TRANS_HBLANK_A          i915_reg.h
#   0x60008 _TRANS_HSYNC_A           i915_reg.h
#   0x6000C _TRANS_VTOTAL_A          i915_reg.h
#   0x60010 _TRANS_VBLANK_A          i915_reg.h
#   0x60014 _TRANS_VSYNC_A           i915_reg.h
#   0x6001C _PIPEASRC                i915_reg.h
#   0x60030 _PIPEA_DATA_M1           i915_reg.h
#   0x60040 _PIPEA_LINK_M1           i915_reg.h
#   0x60400 _TRANS_DDI_FUNC_CTL_A    i915_reg.h
#   0x60410 _TRANSA_MSA_MISC         i915_reg.h
#   0x64000 _DDI_BUF_CTL_A           i915_reg.h
#   0x64040 _DP_TP_CTL_A             i915_reg.h
#   0x70008 _TRANSACONF              i915_reg.h
#   0x7002C _PIPE_MISC2_A            i915_reg.h
#   0x70030 _PIPE_MISC_A             i915_reg.h
#   0x46000 CDCLK_CTL                i915_reg.h
#   0x51000 SKL_DFSM                 i915_reg.h
#   0xC2000 SOUTH_CHICKEN1           i915_reg.h:3358
#   0xC2014 SFUSE_STRAP              i915_reg.h:4391
#   0xC6200 PCH_DREF_CONTROL         i915_reg.h:3110
#   0xE0300 _TRANS_DP_CTL_A          i915_reg.h
#   0x164280 ICL_DPCLKA_CFGCR0       i915_reg.h
#   0x164284 _TGL_DPLL0_CFGCR0       i915_reg.h
#   0x164288 _TGL_DPLL0_CFGCR1       i915_reg.h
#   0x70180 _PLANE_CTL_1_A           skl_universal_plane_regs.h:31
#   0x70280 _PLANE_CTL_2_A           skl_universal_plane_regs.h:32
#   0x70188 _PLANE_STRIDE_1_A        skl_universal_plane_regs.h:102
#   0x7018C _PLANE_POS_1_A           skl_universal_plane_regs.h
#   0x70190 _PLANE_SIZE_1_A          skl_universal_plane_regs.h
#   0x7019C _PLANE_SURF_1_A          skl_universal_plane_regs.h
#   0x701A4 _PLANE_OFFSET_1_A        skl_universal_plane_regs.h
#   bit 31  PLANE_CTL_ENABLE         skl_universal_plane_regs.h:38
#   27:23   PLANE_CTL_FORMAT_MASK_ICL skl_universal_plane_regs.h:49
# Transcoder/pipe bases: intel_display_device.c lines 56, 70, 71, 73, 79, 80.

TRANS = [('A', 0x60000), ('B', 0x61000), ('C', 0x62000), ('D', 0x63000),
         ('DSI0', 0x6B000), ('DSI1', 0x6B800)]
PIPE = [('A', 0x70000), ('B', 0x71000), ('C', 0x72000), ('D', 0x73000),
        ('DSI0', 0x7B000), ('DSI1', 0x7B800)]
TREG = [('HTOTAL', 0x00), ('HBLANK', 0x04), ('HSYNC', 0x08),
        ('VTOTAL', 0x0C), ('VBLANK', 0x10), ('VSYNC', 0x14),
        ('PIPESRC', 0x1C), ('DATA_M1', 0x30), ('LINK_M1', 0x40),
        ('DDI_FUNC_CTL', 0x400), ('MSA_MISC', 0x410)]

print('==== SECTION 2  transcoder timing blocks ====')
for name, base in TRANS:
    vals = [(n, rd(base + o)) for n, o in TREG]
    live = [v for _, v in vals if not dead(v)]
    print('trans %-5s @0x%05X  %s' % (name, base, 'LIVE' if live else verdict_null))
    for n, o in TREG:
        v = rd(base + o)
        extra = ''
        if not dead(v) and n in ('HTOTAL', 'HBLANK', 'HSYNC', 'VTOTAL', 'VBLANK', 'VSYNC'):
            lo, hi = v & 0xFFFF, (v >> 16) & 0xFFFF
            extra = '  halves %d / %d  (+1: %d / %d)' % (lo, hi, lo + 1, hi + 1)
        if not dead(v) and n == 'PIPESRC':
            w, ht = (v >> 16) & 0xFFFF, v & 0xFFFF
            extra = '  raw %dx%d  (+1: %dx%d)' % (w, ht, w + 1, ht + 1)
        print('    %-13s +0x%03X  %s%s' % (n, o, h(v), extra))
print()

print('==== SECTION 3  pipe configuration ====')
PREG = [('CONF', 0x008), ('MISC2', 0x02C), ('MISC', 0x030)]
for name, base in PIPE:
    vals = [rd(base + o) for _, o in PREG]
    print('pipe %-5s @0x%05X  %s' % (name, base, 'LIVE' if any(not dead(v) for v in vals) else verdict_null))
    for n, o in PREG:
        print('    %-6s +0x%03X  %s' % (n, o, h(rd(base + o))))
print()

# ------------------------------------------------------------- Q3 plane slots
# _PLANE_CTL_1_A 0x70180, _PLANE_CTL_2_A 0x70280 -> slot stride 0x100.
print('==== SECTION 4  universal plane slots (Q3) ====')
PLANE_BASE, PLANE_STRIDE, SLOTS = 0x180, 0x100, 7
for name, base in PIPE:
    found = []
    for n in range(SLOTS):
        ctl = rd(base + PLANE_BASE + n * PLANE_STRIDE)
        if not dead(ctl):
            found.append((n, ctl))
    if not found:
        print('pipe %-5s @0x%05X  %s (no live plane slot)' % (name, base, verdict_null))
        continue
    print('pipe %-5s @0x%05X  %d live slots' % (name, base, len(found)))
    for n, ctl in found:
        slot = base + PLANE_BASE + n * PLANE_STRIDE
        en = (ctl >> 31) & 1
        fmt = (ctl >> 23) & 0x1F
        print('  plane %d @0x%05X CTL=%s enable=%d format=0x%02X' % (n + 1, slot, h(ctl), en, fmt))
        print('      STRIDE +0x08 %s   POS +0x0C %s   SIZE +0x10 %s'
              % (h(rd(slot + 0x08)), h(rd(slot + 0x0C)), h(rd(slot + 0x10))))
        sz = rd(slot + 0x10)
        if not dead(sz):
            a, b = sz & 0xFFFF, (sz >> 16) & 0xFFFF
            print('      PLANE_SIZE halves %d / %d  (+1: %d / %d) -- compare with PIPESRC above;'
                  % (a, b, a + 1, b + 1))
            print('      measured 2026-08-16 the two are BYTE-SWAPPED relative to each other')
        print('      SURF +0x1C %s   OFFSET +0x24 %s'
              % (h(rd(slot + 0x1C)), h(rd(slot + 0x24))))
        # Whole slot raw, because a name invented here would poison the trail.
        words = ['+0x%02X %s' % (o, h(rd(slot + o))) for o in range(0x00, 0x100, 4)]
        for i in range(0, len(words), 4):
            print('      ' + '  '.join(words[i:i+4]))
print()

# --------------------------------------------------------- Q1/Q2 DSI + stride
print('==== SECTION 5  DSI pages and the 0x800-vs-0x1000 stride (Q1/Q2) ====')
print('If DSI1 sits at 0x6B800 then 0x6C000 is DPCLKA, not a timing block. The')
print('two readings are separable ONLY if something programmed a DSI transcoder,')
print('and nothing on an eDP-only laptop ever will -- so a null here settles')
print('nothing either way. Recorded so the absence is documented rather than')
print('inferred.')
for label, off in (('0x6B000 DSI0 HTOTAL', 0x6B000), ('0x6B800 DSI1 HTOTAL', 0x6B800),
                   ('0x6C000 DPCLKA-or-DSI1', 0x6C000), ('0x7B000 pipe DSI0', 0x7B000),
                   ('0x7B800 pipe DSI1', 0x7B800)):
    print('  %-24s %s' % (label, h(rd(off))))
print()

# ------------------------------------------------------------ clocks and PLLs
print('==== SECTION 6  clocks, PLLs, fuses, DDI ====')
NAMED = [
    (0x046000, 'CDCLK_CTL'),
    (0x051000, 'SKL_DFSM'),
    (0x06C000, 'DPCLKA (pre-Gen12 location)'),
    (0x0C2000, 'SOUTH_CHICKEN1'),
    (0x0C2014, 'SFUSE_STRAP'),
    (0x0C6200, 'PCH_DREF_CONTROL'),
    (0x0E0300, '_TRANS_DP_CTL_A'),
    (0x164280, 'ICL_DPCLKA_CFGCR0'),
    (0x164284, '_TGL_DPLL0_CFGCR0'),
    (0x164288, '_TGL_DPLL0_CFGCR1'),
    (0x064000, '_DDI_BUF_CTL_A'),
    (0x064040, '_DP_TP_CTL_A'),
]
for off, name in NAMED:
    print('  %-28s @0x%06X  %s' % (name, off, h(rd(off))))
print()
print('  DDI buffer control walk (0x64000 + n*0x100, unnamed beyond A):')
for n in range(8):
    off = 0x64000 + n * 0x100
    print('    DDI slot %d @0x%06X  %s' % (n, off, h(rd(off))))
print()

# --------------------------------------------------- Q4 PIPESRC vs the panel
print('==== SECTION 7  PIPESRC +1 convention vs the lit panel (Q4) ====')
modes = []
try:
    for d in sorted(os.listdir('/sys/class/drm')):
        f = '/sys/class/drm/%s/modes' % d
        if os.path.exists(f):
            with open(f) as fh:
                first = fh.readline().strip()
            if first:
                modes.append((d, first))
except OSError:
    pass
for d, m in modes:
    print('  drm %-18s first mode %s' % (d, m))
for name, base in TRANS:
    v = rd(base + 0x1C)
    if dead(v):
        continue
    w, ht = (v >> 16) & 0xFFFF, v & 0xFFFF
    print('  trans %-5s PIPESRC=%s  raw=%dx%d  +1=%dx%d'
          % (name, h(v), w, ht, w + 1, ht + 1))
print('  -> whichever of raw / +1 equals a mode above is the convention.')
print('     If NEITHER matches, say so. Do not pick the closer one.')
print()

print('==== END OF MMIO ====')
mm.close()
os.close(fd)
PYEOF

tar -czf "/root/igpu-final-$STAMP.tar.gz" -C /root \
    "$(basename $OUT)" "$(basename $EDIDDIR)" 2>/dev/null

echo
echo "================================================================="
sed -n '1,20p' "$OUT"
echo "..."
grep -E '^(CONTROL|====|total live|  trans |  drm |trans (A|DSI0|DSI1)|pipe A|  plane|  ->)' "$OUT" | head -50
echo "================================================================="
echo
echo "full text : $OUT"
echo "edid dir  : $EDIDDIR"
echo "tarball   : /root/igpu-final-$STAMP.tar.gz"
echo
echo "Send the TARBALL. It is the whole Linux phase -- there is no second run."
