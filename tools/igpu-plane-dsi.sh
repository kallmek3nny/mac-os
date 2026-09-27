#!/bin/sh
#
# igpu-plane-dsi.sh -- ONE file, ONE command:
#
#     sudo sh igpu-plane-dsi.sh
#
# Read-only. mmaps BAR0 through sysfs, binds nothing, sets no mode.
#
# WHY THIS EXISTS AND WHAT IT ADDS OVER igpu-capture.sh / igpu-followup.sh
# ----------------------------------------------------------------------
# Those two stop at pipe/transcoder C (followup adds D + DDI/AUX). NEITHER has
# ever read the DSI pages, and NEITHER has decoded a single plane register --
# igpu-capture.sh captured the bytes of 0x70000..0x73FFF but only ever printed
# them as hex. So every plane constant handoff14 sec.2 landed is untouched by
# live silicon, and IG_TRANSCODER_DSI0/DSI1 are pure transcription.
#
# PARTIALLY SUPERSEDED 2026-08-16 BY THIS SCRIPT'S OWN RUN. The paragraph above
# describes the state at AUTHORING time and is kept for that reason. What the
# 02:30:53 UTC run on 8086:9a68 actually changed (igpu-results/, and see the
# per-question notes below):
#
#   Q4  SETTLED. PIPESRC 0x077F0437 +1 == eDP-1's advertised 1920x1080, and
#       PLANE_SIZE 0x0437077F was read in the same breath -- so PLANE_SIZE's -1
#       convention is witnessed too, with its halves INVERTED relative to
#       PIPESRC. That pair is the whole finding.
#   Q1/Q2/Q3  NOT settled, and this script cannot settle them on an eDP-only
#       machine. Every unused display page on this silicon reads 0x00000000,
#       so a disabled plane and an absent slot are indistinguishable, and the
#       0x6C000 stride probe reading zero separates nothing. Nothing will ever
#       program a DSI transcoder on a laptop with no DSI panel.
#
# So IG_TRANSCODER_DSI0/DSI1 are STILL pure transcription. Only the transcoder-A
# timing and pipe-A plane-1 constants met silicon.
#
# This script cross-examines FOUR claims IntelDisplay.hpp makes. Each is stated
# as a question the hardware can answer, and each prints ITS OWN verdict rather
# than leaving a reader to infer one from a hex dump:
#
#   Q1  Are 0x6B000 / 0x6B800 (DSI0/DSI1 transcoders) claimed pages at all?
#   Q2  Is DSI1 - DSI0 really 0x800 and not 0x1000? A 0x1000 stride would put
#       DSI1 at 0x6C000, which is DPCLKA -- so the two readings are separable
#       by asking whether 0x6C000 looks like PLL distribution or like timings.
#   Q3  How many universal planes does a Gen12 pipe actually expose? The header
#       does not commit. Slots run pipe+0x180 + n*0x100 (_PLANE_CTL_1_A 0x70180,
#       _PLANE_CTL_2_A 0x70280 => stride 0x100, both from
#       reference/i915/skl_universal_plane_regs.h).
#   Q4  Does PIPESRC's +1 convention hold against the panel's real mode? This is
#       the one the header flags as unsettled, and a LIT PANEL is the only thing
#       that settles it. Perishable -- needs i915 bound and a mode set.
#
# WHAT IS GROUNDED VS WHAT IS DUMPED RAW
# --------------------------------------
# Only offsets read out of the vendored i915 headers in this tree are DECODED
# by name below:
#
#   0x70000 PIPE_A_OFFSET            intel_display_device.c:56
#   0x60000 TRANSCODER_A_OFFSET      intel_display_device.c:73
#   0x6b000 TRANSCODER_DSI0_OFFSET   intel_display_device.c:79
#   0x6b800 TRANSCODER_DSI1_OFFSET   intel_display_device.c:80
#   0x7b000 PIPE_DSI0_OFFSET         intel_display_device.c:70
#   0x7b800 PIPE_DSI1_OFFSET         intel_display_device.c:71
#   0x70180 _PLANE_CTL_1_A           skl_universal_plane_regs.h:31
#   0x70280 _PLANE_CTL_2_A           skl_universal_plane_regs.h:32
#   0x70188 _PLANE_STRIDE_1_A        skl_universal_plane_regs.h:102
#   bit 31  PLANE_CTL_ENABLE         skl_universal_plane_regs.h:38
#   27:23   PLANE_CTL_FORMAT_MASK_ICL skl_universal_plane_regs.h:49
#
# Every OTHER word in a plane slot is dumped as a raw offset with no name. That
# is deliberate: a name invented here, sitting next to names traced to a file
# and line, would destroy the evidence trail. Decode the unnamed words offline
# against the headers.
#
# A NULL IS ONLY EVIDENCE IF THE READER IS PROVEN TO WORK
# ------------------------------------------------------
# An unclaimed page and a failed mmap both read as 0xFFFFFFFF. So the script
# reads a POSITIVE CONTROL first -- transcoder A's HTOTAL on the lit pipe. If
# the control is 0xFFFFFFFF or 0, every subsequent null is reported as
# INCONCLUSIVE rather than ABSENT, because the reader itself is then unproven.

set -u

if [ "$(id -u)" != "0" ]; then
    echo "must run as root: sudo sh $0" >&2
    exit 1
fi

for p in /sys/bus/pci/devices/0000:00:02.0 /sys/bus/pci/devices/0000:00:02.1; do
    [ -e "$p/resource0" ] && PCI="$p" && break
done
if [ -z "${PCI:-}" ]; then
    echo "no Intel iGPU at 00:02.x -- is this the right machine?" >&2
    exit 1
fi

STAMP=$(date -u +%Y%m%d-%H%M%S)
OUT="/root/igpu-plane-dsi-$STAMP.txt"

{
echo "igpu-plane-dsi $STAMP UTC"
echo "pci      $PCI"
echo "device   $(cat $PCI/device 2>/dev/null)"
echo "driver   $(basename $(readlink -f $PCI/driver 2>/dev/null) 2>/dev/null)"
echo "lockdown $(cat /sys/kernel/security/lockdown 2>/dev/null || echo unavailable)"
echo "cmdline  $(cat /proc/cmdline)"
echo "---- connectors ----"
for c in /sys/class/drm/card*-*; do
    [ -e "$c/status" ] || continue
    echo "$(basename $c) $(cat $c/status) modes=[$(head -1 $c/modes 2>/dev/null)]"
done
echo "---- mmio ----"
} > "$OUT" 2>&1

python3 - "$PCI/resource0" >> "$OUT" 2>&1 <<'PYEOF'
import mmap, os, sys

path = sys.argv[1]
size = os.path.getsize(path)
fd = os.open(path, os.O_RDONLY)
mm = mmap.mmap(fd, min(size, 0x100000), mmap.MAP_SHARED, mmap.PROT_READ)

def rd(off):
    if off + 4 > len(mm):
        return None
    return int.from_bytes(mm[off:off+4], 'little')

def h(v):
    return 'unreadable' if v is None else '0x%08X' % v

TRANS = [('A',0x60000),('B',0x61000),('C',0x62000),('D',0x63000),
         ('DSI0',0x6B000),('DSI1',0x6B800)]
PIPE  = [('A',0x70000),('B',0x71000),('C',0x72000),('D',0x73000),
         ('DSI0',0x7B000),('DSI1',0x7B800)]
# offsets within a transcoder block, from IntelDisplay.hpp IG_TRANS_*
TREG = [('HTOTAL',0x00),('HBLANK',0x04),('HSYNC',0x08),
        ('VTOTAL',0x0C),('VBLANK',0x10),('VSYNC',0x14),('PIPESRC',0x1C)]

def dead(v):
    """No usable signal at this offset.

    MEASURED 2026-08-16 on 8086:9a68: an unused display page on this silicon
    reads 0x00000000, NOT all-ones. The first version of this function treated
    only None/0xFFFFFFFF as dead, so transcoders B/C/D -- which certainly EXIST
    on Tiger Lake -- were reported with the verdict word ABSENT. That word was
    false; the true state is DECODED-BUT-UNPROGRAMMED. Zero is included here so
    the verdict word stops overclaiming, and the word itself is now
    UNPROGRAMMED-OR-ABSENT because a single read cannot separate the two.

    The dry-run harness could not have caught this: its synthetic BAR was filled
    with 0xFF, which is what the author expected hardware to do rather than what
    this hardware does. tools/_igpu-reader-dryrun.py now has a zero-fill arm.
    """
    return v is None or v == 0xFFFFFFFF or v == 0

# ---- positive control: prove the reader before believing any null ----
ctrl = None
for name, base in TRANS[:4]:
    v = rd(base + 0x00)
    if not dead(v) and v != 0:
        ctrl = (name, v)
        break
print('CONTROL  transcoder %s HTOTAL = %s' % (ctrl[0], h(ctrl[1])) if ctrl
      else 'CONTROL  FAILED -- no transcoder A..D has a live HTOTAL')
READER_OK = ctrl is not None
# Not 'ABSENT': see dead(). A silent page may be unprogrammed rather than absent,
# and one read cannot tell those apart.
verdict_null = 'UNPROGRAMMED-OR-ABSENT' if READER_OK else 'INCONCLUSIVE'
if not READER_OK:
    print('         reader unproven; every null below is INCONCLUSIVE, not ABSENT')
print()

# ---- Q1/Q2: DSI pages, and the 0x800 vs 0x1000 stride ----
print('==== Q1/Q2  DSI transcoders ====')
for name, base in TRANS:
    vals = [(n, rd(base+o)) for n, o in TREG]
    live = [v for _, v in vals if not dead(v) and v != 0]
    state = 'LIVE' if live else verdict_null
    print('trans %-5s @0x%05X  %s' % (name, base, state))
    for n, v in vals:
        print('    %-8s +0x%02X  %s' % (n, dict(TREG)[n], h(v)))
probe = rd(0x6C000)
print('stride probe: 0x6C000 (DPCLKA if DSI1 is at 0x6B800) = %s' % h(probe))
print('  -> if 0x6B800 is LIVE and 0x6C000 does not look like timings,')
print('     the 0x800 stride in IntelDisplay.hpp is confirmed on silicon.')
print()

# ---- Q3: how many universal planes does a pipe expose? ----
# _PLANE_CTL_1_A 0x70180, _PLANE_CTL_2_A 0x70280 -> slot stride 0x100.
print('==== Q3  universal planes per pipe ====')
PLANE_BASE, PLANE_STRIDE, SLOTS = 0x180, 0x100, 7
for name, base in PIPE:
    found = []
    for n in range(SLOTS):
        ctl = rd(base + PLANE_BASE + n*PLANE_STRIDE)
        if not dead(ctl):
            found.append((n, ctl))
    if not found:
        print('pipe %-5s @0x%05X  %s (no readable plane slot)' % (name, base, verdict_null))
        continue
    print('pipe %-5s @0x%05X  %d readable slots' % (name, base, len(found)))
    for n, ctl in found:
        slot = base + PLANE_BASE + n*PLANE_STRIDE
        en  = (ctl >> 31) & 1                      # PLANE_CTL_ENABLE, bit 31
        fmt = (ctl >> 23) & 0x1F                   # PLANE_CTL_FORMAT_MASK_ICL 27:23
        std = rd(slot + 0x08)                      # _PLANE_STRIDE_1_A 0x70188
        print('  plane %d @0x%05X CTL=%s enable=%d format=0x%02X stride=%s'
              % (n+1, slot, h(ctl), en, fmt, h(std)))
        if en:
            words = ['+0x%02X %s' % (o, h(rd(slot+o)))
                     for o in range(0x00, 0x100, 4)]
            for i in range(0, len(words), 4):
                print('      ' + '  '.join(words[i:i+4]))
print()

# ---- Q4: does PIPESRC's +1 convention match the real mode? ----
print('==== Q4  PIPESRC +1 convention vs the lit panel ====')
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
    print('drm %-16s first mode %s' % (d, m))
for name, base in TRANS:
    v = rd(base + 0x1C)
    if dead(v) or v == 0:
        continue
    raw_w, raw_h = (v >> 16) & 0xFFFF, v & 0xFFFF
    print('trans %-5s PIPESRC=%s  raw=%dx%d  +1=%dx%d'
          % (name, h(v), raw_w, raw_h, raw_w+1, raw_h+1))
print('  -> whichever of raw / +1 equals a mode above is the convention.')
print('     If neither matches, say so: do NOT pick the closer one.')

mm.close()
os.close(fd)
PYEOF

tar -czf "/root/igpu-plane-dsi-$STAMP.tar.gz" -C /root "$(basename $OUT)" 2>/dev/null

echo
echo "================================================================="
sed -n '1,12p' "$OUT"
echo "..."
grep -E '^(CONTROL|====|trans (A|DSI0|DSI1)|pipe A|  plane|  ->|stride probe)' "$OUT" | head -40
echo "================================================================="
echo "full: $OUT"
echo "tar:  /root/igpu-plane-dsi-$STAMP.tar.gz"
