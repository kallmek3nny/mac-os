#!/bin/sh
#
# capture-igpu-display.sh -- one-shot capture of everything the Tiger Lake
# display work needs from the target laptop, while Linux is booted.
#
#     sudo sh capture-igpu-display.sh
#
# Read-only. Binds nothing, unbinds nothing, changes no mode. Safe to run on
# the machine you are currently looking at -- the panel stays lit throughout,
# which is the point: every register value captured is a value that is
# DEMONSTRABLY driving this screen right now.
#
# Produces a directory of artifacts and prints its name at the end. Send the
# whole directory back.
#
# WHY THIS EXISTS
# ---------------
# The NVIDIA half of this project rests on a captured MMIO trace, and its
# headers keep a hard line between values WITNESSED on hardware (*_OBSERVED_*)
# and values reasoned out. Nothing in this tree has ever read an Intel
# register. Every Tiger Lake constant written without this capture would be a
# value copied from Linux's headers and never confirmed against the silicon.
#
# EVERY SECTION STATES WHAT ITS NULL RESULT DOES NOT PROVE.

set -u

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="${1:-$HOME/igpu-display-$STAMP}"
PCI="${PCI:-0000:00:02.0}"

mkdir -p "$OUT" || { echo "cannot create $OUT"; exit 1; }
LOG="$OUT/00-summary.txt"

say() { echo "$*" | tee -a "$LOG"; }
rule() { say ""; say "=== $* ==="; }

say "=== capture-igpu-display.sh $(date -u '+%Y-%m-%d %H:%M UTC') ==="
say "kernel : $(uname -r)"
say "distro : $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
say "pci    : $PCI"
say "outdir : $OUT"

# ---------------------------------------------------------------------------
# 0. Preconditions. A capture taken under the wrong conditions is worse than
#    no capture, because it looks like data.
# ---------------------------------------------------------------------------
rule "0. Preconditions"

if [ "$(id -u)" != 0 ]; then
  say "FAIL   : not root. BAR0 cannot be mapped and most of this is useless."
  say "         Re-run with sudo."
  exit 1
fi
say "PASS   : running as root"

if [ -d "/sys/bus/pci/devices/$PCI" ]; then
  say "PASS   : $PCI exists"
else
  say "FAIL   : no such PCI device $PCI. Check 'lspci -nn' and re-run with"
  say "         PCI=<addr> sh capture-igpu-display.sh"
  exit 1
fi

if [ -L "/sys/bus/pci/devices/$PCI/driver" ]; then
  DRV=$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI/driver")")
  say "PASS   : driver bound = $DRV"
else
  DRV="(unclaimed)"
  say "FAIL   : $PCI has NO driver bound. The display power well is then down,"
  say "         BAR0 reads return 0xFFFFFFFF, and every register value below"
  say "         is noise rather than a small number. Do not record any of it."
fi

# Locate the MMIO dumper NOW rather than in section 2.
#
# The 2026-08-15 15:50 UTC run failed exactly here and did not find out until
# after five sections of work: the operator copied this script alone, so
# `dirname $0` was `.` and the dumper was not beside it. A missing dependency
# for the CENTREPIECE artifact has to be a precondition failure, not a late
# surprise buried in a wall of PASS lines.
# There are two dumpers and either is acceptable. The .sh twin is preferred
# because it needs nothing but dd/od/awk, so a missing interpreter can never
# block the capture. The .py twin is the fallback with the stronger access
# path: it can mmap() a memory BAR, which a shell cannot, and on kernels where
# read() on resource0 returns EINVAL the shell twin has no way in at all.
#
# Preferred order is therefore "whichever is present", .sh first, and python3
# is a precondition ONLY when the chosen tool is the .py one.
MMIO_TOOL=""
MMIO_KIND=""
for CAND in "$(dirname "$0")/read-igpu-mmio.sh" \
            "./read-igpu-mmio.sh" \
            "$HOME/read-igpu-mmio.sh" \
            "$HOME/Downloads/read-igpu-mmio.sh" \
            "$(dirname "$0")/tools/read-igpu-mmio.sh" \
            "$(dirname "$0")/read-igpu-mmio.py" \
            "./read-igpu-mmio.py" \
            "$HOME/read-igpu-mmio.py" \
            "$HOME/Downloads/read-igpu-mmio.py" \
            "$(dirname "$0")/tools/read-igpu-mmio.py"; do
  if [ -r "$CAND" ]; then
    MMIO_TOOL="$CAND"
    case "$CAND" in
      *.sh) MMIO_KIND=sh ;;
      *.py) MMIO_KIND=py ;;
    esac
    break
  fi
done

if [ -z "$MMIO_TOOL" ]; then
  say "FAIL   : neither read-igpu-mmio.sh nor read-igpu-mmio.py found next to"
  say "         this script or in . / \$HOME / \$HOME/Downloads."
  say ""
  say "         That file IS the capture. Everything else here is context for"
  say "         it. Copy one beside this script and re-run -- the rest of this"
  say "         run would only reproduce artifacts you already have."
  say ""
  say "         read-igpu-mmio.sh needs no interpreter. Prefer it."
  exit 1
fi
say "PASS   : MMIO dumper at $MMIO_TOOL (kind=$MMIO_KIND)"

if [ "$MMIO_KIND" = py ]; then
  if command -v python3 >/dev/null 2>&1; then
    say "PASS   : python3 present ($(python3 -V 2>&1))"
  else
    say "FAIL   : the dumper found is the Python one and python3 is absent."
    say "         Copy read-igpu-mmio.sh over instead -- it needs only dd, od"
    say "         and awk, all in coreutils."
    exit 1
  fi
else
  say "NOTE   : using the shell dumper -- no interpreter required."
fi

# The blacklist that invalidated the previous run's NVIDIA-side conclusions.
BLACKLIST=$(tr ' ' '\n' < /proc/cmdline 2>/dev/null | grep '^modprobe.blacklist=')
if [ -n "$BLACKLIST" ]; then
  say "NOTE   : $BLACKLIST"
  case "$BLACKLIST" in
    *nvidia*|*nouveau*)
      say "         The NVIDIA stack is blacklisted. Section 6 cannot say"
      say "         anything about the 3050 -- that is this argument talking,"
      say "         not the hardware. The Intel capture is unaffected."
      ;;
  esac
else
  say "NOTE   : no modprobe.blacklist on the kernel command line"
fi

# ---------------------------------------------------------------------------
# 1. Identity. Cheap, and it dates every other artifact in the directory.
# ---------------------------------------------------------------------------
rule "1. Device identity"
lspci -nn -s "$PCI" 2>&1 | tee -a "$LOG"
lspci -vvv -s "$PCI" > "$OUT/01-lspci-verbose.txt" 2>&1
say "wrote 01-lspci-verbose.txt (BAR sizes, capabilities, link state)"

for F in vendor device revision subsystem_vendor subsystem_device class; do
  [ -r "/sys/bus/pci/devices/$PCI/$F" ] && \
    say "  $F = $(cat "/sys/bus/pci/devices/$PCI/$F")"
done

{
  echo "# BAR layout from sysfs. Line 1 is BAR0: start, end, flags."
  echo "# The MMIO dumper reads BAR0, so its length bounds every offset that"
  echo "# can be dumped at all."
  cat "/sys/bus/pci/devices/$PCI/resource" 2>/dev/null
} > "$OUT/02-bars.txt"
say "wrote 02-bars.txt"

# ---------------------------------------------------------------------------
# 2. The display MMIO dump. The centrepiece.
# ---------------------------------------------------------------------------
rule "2. Display-engine MMIO"
say "Bounded ranges only -- display power well, never the GT block, which is"
say "behind FORCEWAKE and unsafe to read from userspace."
say ""

# MMIO_TOOL and its interpreter (if any) were both established in section 0;
# this cannot be reached without them.
if [ "$MMIO_KIND" = sh ]; then
  sh "$MMIO_TOOL" --pci "$PCI" --out "$OUT/03-display-mmio.txt"
else
  python3 "$MMIO_TOOL" --pci "$PCI" --out "$OUT/03-display-mmio.txt"
fi
RC=$?
say "$(basename "$MMIO_TOOL") exit=$RC"
# The REACH line decides whether the dump is evidence or noise. Surface it
# here so it is in the summary rather than only in the big file.
grep -E '^(REACH|VACUOUS|OK) ' "$OUT/03-display-mmio.txt" 2>/dev/null \
  | sed 's/^/  /' | tee -a "$LOG"
if [ "$RC" != 0 ]; then
  say "FAIL   : the dump did not reach a live device. Read the VACUOUS note"
  say "         in 03-display-mmio.txt before using any value from it."
fi

# ---------------------------------------------------------------------------
# 3. What the display engine is actually configured to do, from i915 itself.
#    This is the cross-check that makes the raw dump interpretable: if the
#    dump says the pipe is 1920x1080 and so does i915, the offset reading is
#    confirmed by two independent paths.
# ---------------------------------------------------------------------------
rule "3. i915 state (debugfs)"
if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then
  mount -t debugfs none /sys/kernel/debug 2>/dev/null && say "mounted debugfs"
fi

DRI=""
for D in /sys/kernel/debug/dri/*; do
  [ -d "$D" ] || continue
  if [ "$(basename "$(readlink -f "$D/../../../" 2>/dev/null)")" = "$PCI" ] \
     || grep -qs i915 "$D/name" 2>/dev/null; then
    DRI="$D"
  fi
done

if [ -z "$DRI" ]; then
  say "SKIP   : no i915 debugfs node found. Absence here is a kernel-config"
  say "         fact (CONFIG_DEBUG_FS), NOT a statement about the hardware."
else
  say "i915 debugfs at $DRI"
  for F in i915_display_info i915_dp_mst_info i915_current_bpc \
           i915_ddb_info i915_dpcd i915_frontbuffer_tracking \
           i915_gem_framebuffer i915_power_domain_info i915_runtime_pm_status; do
    [ -r "$DRI/$F" ] && cat "$DRI/$F" > "$OUT/04-$F.txt" 2>/dev/null \
      && say "  wrote 04-$F.txt"
  done
fi

# ---------------------------------------------------------------------------
# 4. The panel. A framebuffer driver has to reproduce these timings exactly.
# ---------------------------------------------------------------------------
rule "4. Panel and connectors"
for C in /sys/class/drm/card*-*; do
  [ -e "$C/status" ] || continue
  NAME=$(basename "$C")
  STATUS=$(cat "$C/status" 2>/dev/null)
  say "$NAME  status=$STATUS"

  # Report the mode list explicitly. The 2026-08-15 15:50 UTC run wrote this
  # file and said nothing about it, in the same run where BOTH connectors
  # reported no readable EDID -- so the one surviving source of panel timings
  # was captured silently while the summary drew attention only to what was
  # missing. An artifact nobody is told about is an artifact nobody reads.
  if [ -r "$C/modes" ]; then
    cat "$C/modes" > "$OUT/05-$NAME-modes.txt" 2>/dev/null
    NMODES=$(wc -l < "$OUT/05-$NAME-modes.txt" 2>/dev/null || echo 0)
    if [ "$NMODES" -gt 0 ]; then
      say "  wrote 05-$NAME-modes.txt ($NMODES modes)"
      say "  preferred: $(head -1 "$OUT/05-$NAME-modes.txt")"
    else
      say "  05-$NAME-modes.txt is EMPTY. On a disconnected connector that is"
      say "  expected; on a connected one it means the mode list was not"
      say "  readable, which is a capture failure rather than a panel fact."
    fi
  fi

  if [ -r "$C/edid" ] && [ -s "$C/edid" ]; then
    cat "$C/edid" > "$OUT/05-$NAME-edid.bin" 2>/dev/null
    say "  wrote 05-$NAME-edid.bin ($(wc -c < "$OUT/05-$NAME-edid.bin") bytes)"
    command -v edid-decode >/dev/null 2>&1 && \
      edid-decode "$OUT/05-$NAME-edid.bin" > "$OUT/05-$NAME-edid.txt" 2>&1 && \
      say "  wrote 05-$NAME-edid.txt (decoded)"
  else
    say "  no EDID readable. On eDP this is normal when the panel is driven"
    say "  from a fixed mode; it is NOT evidence the panel has no timings."
    say "  The timings then live in the mode list above and in i915's own"
    say "  04-i915_display_info.txt -- use those, and do not read this line"
    say "  as 'the panel is undocumented'."
  fi
done

# ---------------------------------------------------------------------------
# 5. Firmware the display engine depends on. macOS would need equivalents.
# ---------------------------------------------------------------------------
rule "5. Display firmware (DMC)"
dmesg 2>/dev/null | grep -iE 'dmc|guc|huc|firmware' | tail -30 > "$OUT/06-firmware.txt"
grep -iE 'dmc' "$OUT/06-firmware.txt" | sed 's/^/  /' | tee -a "$LOG"
say "wrote 06-firmware.txt"
ls -la /lib/firmware/i915/ > "$OUT/06-firmware-files.txt" 2>&1 && \
  say "wrote 06-firmware-files.txt"

# ---------------------------------------------------------------------------
# 6. The 3050, only if it has a driver bound. Otherwise skip loudly.
# ---------------------------------------------------------------------------
rule "6. NVIDIA side"
NV=0000:01:00.0
if [ -L "/sys/bus/pci/devices/$NV/driver" ]; then
  say "driver bound = $(basename "$(readlink -f "/sys/bus/pci/devices/$NV/driver")")"
  for C in /sys/class/drm/card*-*; do
    OWNER=$(readlink -f "$C" 2>/dev/null)
    case "$OWNER" in
      *"$NV"*) say "  3050 owns connector $(basename "$C") " \
                    "status=$(cat "$C/status" 2>/dev/null)" ;;
    esac
  done
  ROM="/sys/bus/pci/devices/$NV/rom"
  if [ -e "$ROM" ] && echo 1 > "$ROM" 2>/dev/null; then
    cat "$ROM" > "$OUT/07-vbios-25a2.rom" 2>/dev/null
    echo 0 > "$ROM" 2>/dev/null
    SZ=$(wc -c < "$OUT/07-vbios-25a2.rom" 2>/dev/null || echo 0)
    say "  VBIOS $SZ bytes"
    [ "$SZ" -lt 4096 ] && say "  FAIL: too small to be a VBIOS -- not a dump."
  fi
else
  say "SKIP   : $NV has no driver bound (see the blacklist note in section 0)."
  say "         It therefore registers no DRM connector and its ROM is"
  say "         unreadable from D3cold. Both absences are pre-determined by"
  say "         that, and are NOT evidence about how the card is wired."
fi

rule "7. Kernel log"
dmesg 2>/dev/null > "$OUT/08-dmesg-full.txt"
say "wrote 08-dmesg-full.txt"

say ""
say "=== done ==="
say "send back the whole directory: $OUT"
echo ""
echo "tar it up with:"
echo "  tar czf ${OUT}.tar.gz -C \"$(dirname "$OUT")\" \"$(basename "$OUT")\""
