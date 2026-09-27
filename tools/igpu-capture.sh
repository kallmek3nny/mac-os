#!/bin/sh
#
# igpu-capture.sh -- ONE file, ONE command:
#
#     sudo sh igpu-capture.sh
#
# Everything the Tiger Lake display work needs, with the MMIO dumper embedded
# rather than sitting in a second file that has to be copied alongside. The
# 2026-08-15 15:50 UTC run failed for exactly that reason.
#
# Read-only. Binds nothing, changes no mode. Tars its own output at the end.
#
# WHY THE MMIO PART IS PYTHON AND CANNOT BE SHELL
# ----------------------------------------------
# Confirmed against the kernel source, drivers/pci/pci-sysfs.c v6.12:
#
#   Line 1187-1189: read()/write() on sysfs `resourceN` are wired up ONLY for
#   `IORESOURCE_IO` BARs. A MEMORY BAR gets `mmap` and nothing else. So the
#   dd/od approach was never possible here -- not a kernel-version quirk, not
#   something an option could enable.
#
#   Line 1047: pci_mmap_resource() calls security_locked_down(LOCKDOWN_PCI_ACCESS)
#   before anything else. Under kernel lockdown that returns -EPERM, which is
#   the "Operation not permitted" the 2026-08-15 16:xx run saw on all 15 ranges
#   WHILE RUNNING AS ROOT. Root is not the missing thing.
#
# Section 2 below reports the lockdown state FIRST, so an EPERM is attributed
# rather than guessed at.

set -u

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="${1:-$HOME/igpu-$STAMP}"
PCI="${PCI:-0000:00:02.0}"
NV="${NV:-0000:01:00.0}"

mkdir -p "$OUT" || { echo "cannot create $OUT"; exit 1; }
LOG="$OUT/00-summary.txt"
say() { echo "$*" | tee -a "$LOG"; }
rule() { say ""; say "=== $* ==="; }

say "=== igpu-capture.sh $(date -u '+%Y-%m-%d %H:%M UTC') ==="
say "kernel : $(uname -r)"
say "distro : $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
say "pci    : $PCI"
say "outdir : $OUT"

# ---------------------------------------------------------------------------
rule "1. Preconditions and identity"

[ "$(id -u)" = 0 ] || { say "FAIL   : not root. Re-run with sudo."; exit 1; }
say "PASS   : root"
[ -d "/sys/bus/pci/devices/$PCI" ] || { say "FAIL   : no device $PCI"; exit 1; }

if [ -L "/sys/bus/pci/devices/$PCI/driver" ]; then
  say "PASS   : driver = $(basename "$(readlink -f "/sys/bus/pci/devices/$PCI/driver")")"
else
  say "FAIL   : $PCI has no driver bound -- the display power well is down and"
  say "         every register read below would be noise, not small numbers."
fi

lspci -nn -s "$PCI" 2>&1 | tee -a "$LOG"
lspci -vvv -s "$PCI" > "$OUT/01-lspci.txt" 2>&1
cat "/sys/bus/pci/devices/$PCI/resource" > "$OUT/02-bars.txt" 2>/dev/null

BL=$(tr ' ' '\n' < /proc/cmdline 2>/dev/null | grep '^modprobe.blacklist=')
if [ -n "$BL" ]; then
  say "NOTE   : $BL"
  say "         Section 6 cannot speak for the 3050 -- that is this argument,"
  say "         not the hardware. Intel capture unaffected."
fi

# ---------------------------------------------------------------------------
rule "2. Kernel lockdown -- read this BEFORE any EPERM below"

LD="unknown"
if [ -r /sys/kernel/security/lockdown ]; then
  LD=$(cat /sys/kernel/security/lockdown)
  say "lockdown: $LD"
  # The ACTIVE mode is the bracketed one.
  case "$LD" in
    *"[none]"*)
      say "PASS   : lockdown is none. A BAR mmap should succeed as root." ;;
    *)
      say "FAIL   : lockdown is ACTIVE (the bracketed word above)."
      say "         security_locked_down(LOCKDOWN_PCI_ACCESS) rejects every"
      say "         mmap of a PCI BAR at pci-sysfs.c:1047, before the mapping"
      say "         is even attempted. Root does not bypass it, and neither"
      say "         does intel_reg -- intel-gpu-tools reaches the BAR through"
      say "         the SAME sysfs path, so installing it will not help."
      say ""
      say "         LOCKDOWN_PCI_ACCESS is in the INTEGRITY band, so a bracketed"
      say "         [integrity] is already enough to block this -- it does not"
      say "         take [confidentiality]. Nor does mokutil --disable-validation"
      say "         help: that turns off shim MOK validation, while lockdown"
      say "         keys off the firmware Secure Boot state it cannot change."
      say ""
      say "         On Ubuntu, lockdown is switched on by Secure Boot. To clear"
      say "         it, disable Secure Boot in firmware (MSI: Boot -> Secure"
      say "         Boot -> Disabled) and re-run. lockdown cannot be lowered at"
      say "         runtime, and lockdown=none on the cmdline is ignored while"
      say "         Secure Boot is on."
      say ""
      say "         If you would rather not touch Secure Boot: section 4's"
      say "         debugfs artifacts carry i915's own DECODED view of the"
      say "         active display, which is enough to proceed. It is a"
      say "         second-hand reading rather than the silicon, so anything"
      say "         derived from it gets marked as such, never as OBSERVED." ;;
  esac
else
  say "NOTE   : /sys/kernel/security/lockdown unreadable (securityfs not"
  say "         mounted, or no LSM). Absence is not proof lockdown is off."
fi
say "secureboot: $(mokutil --sb-state 2>/dev/null || echo 'mokutil not installed')"

# ---------------------------------------------------------------------------
rule "3. Display-engine MMIO"
say "Bounded display-power-well ranges only. Never the GT block -- that is"
say "behind FORCEWAKE and unsafe to read from userspace."

if command -v python3 >/dev/null 2>&1; then
  PCI="$PCI" OUTF="$OUT/03-mmio.txt" python3 - <<'PY'
import mmap, os, struct, sys

PCI = os.environ["PCI"]
OUTF = os.environ["OUTF"]

# MIRRORED in read-igpu-mmio.py / .sh. tests/test_igpu_mmio_ranges.py parses all
# THREE tables and fails if any pair drifts -- including this embedded one, which
# is the copy that actually ran on the laptop and so the one whose drift would be
# least visible. Authority for any single offset is reference/i915/i915_reg.h.
RANGES = [
    (0x044000, 0x2000, "north DE interrupts / DE misc"),
    (0x046000, 0x1000, "CDCLK, DPLL control"),
    (0x060000, 0x1000, "transcoder A timings"),
    (0x061000, 0x1000, "transcoder B timings"),
    (0x062000, 0x1000, "transcoder C timings"),
    (0x06C000, 0x1000, "DPCLKA / PLL distribution"),
    (0x06F000, 0x1000, "transcoder EDP timings"),
    (0x070000, 0x1000, "pipe A config + universal plane 1"),
    (0x071000, 0x1000, "pipe B config + universal plane 1"),
    (0x072000, 0x1000, "pipe C config + universal plane 1"),
    (0x0C0000, 0x1000, "south display / port control"),
    (0x0C7000, 0x1000, "panel power sequencing"),
    (0x0C8000, 0x1000, "backlight PWM"),
    (0x160000, 0x2000, "combo PHY A/B"),
    (0x162000, 0x2000, "combo PHY C + DDI aux"),
]
TRIVIAL = (0x00000000, 0xFFFFFFFF)

path = "/sys/bus/pci/devices/%s/resource0" % PCI
body, distinct, total, eperm = [], set(), 0, 0

try:
    fd = os.open(path, os.O_RDONLY)
except OSError as exc:
    open(OUTF, "w").write("FAIL: cannot open %s: %s\n" % (path, exc))
    sys.exit(2)

try:
    for base, length, label in RANGES:
        try:
            mm = mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
        except OSError as exc:
            if getattr(exc, "errno", None) == 1:
                eperm += 1
            body.append("FAIL   0x%06X +0x%04X  %s -- mmap: %s"
                        % (base, length, label, exc))
            body.append("")
            continue
        body.append("=== 0x%06X .. 0x%06X  %s ==="
                    % (base, base + length - 1, label))
        # Word at a time, NOT a bulk slice. A slice is a memcpy and glibc may
        # satisfy it with vector loads; MMIO needs naturally aligned 32-bit
        # accesses or a read can return a word that never existed on the bus.
        try:
            words = [struct.unpack("<I", mm[o:o + 4])[0]
                     for o in range(0, length, 4)]
        finally:
            mm.close()
        total += length
        for i in range(0, len(words), 4):
            row = words[i:i + 4]
            for w in row:
                if w not in TRIVIAL:
                    distinct.add(w)
            body.append("0x%06X: %s" % (base + i * 4,
                                        " ".join("%08x" % w for w in row)))
        body.append("")
finally:
    os.close(fd)

head = ["=== igpu-capture.sh embedded MMIO dump ===",
        "pci    : %s" % PCI,
        "source : %s" % path,
        "",
        "REACH  : %d distinct non-trivial 32-bit values across %d bytes read"
        % (len(distinct), total)]
if len(distinct) >= 16:
    head += ["OK     : the mapping reached a live device. Values below are real",
             "         register state from a display currently driving the panel."]
elif eperm:
    head += ["BLOCKED: %d of %d ranges returned EPERM. That is kernel lockdown"
             % (eperm, len(RANGES)),
             "         (pci-sysfs.c:1047), not a hardware or permissions fact.",
             "         Nothing here may be recorded as an observed constant.",
             "         See section 2 of the summary for what to do."]
else:
    head += ["VACUOUS: too few distinct values -- the mapping did not reach the",
             "         device, or the display power well is down. Do NOT record",
             "         any value below as an observed hardware constant."]
open(OUTF, "w").write("\n".join(head + [""] + body) + "\n")
print("\n".join(head[4:]))
sys.exit(0 if len(distinct) >= 16 else 1)
PY
  say "mmio exit=$?"
else
  say "FAIL   : python3 absent. A memory BAR is reachable ONLY via mmap"
  say "         (pci-sysfs.c:1187 wires read() for IORESOURCE_IO BARs only),"
  say "         and a shell cannot mmap. Install python3, or rely on the"
  say "         debugfs artifacts in section 4."
fi

# ---------------------------------------------------------------------------
rule "4. i915 decoded state (debugfs)"
say "This is what makes section 3 interpretable -- or the fallback when"
say "section 3 is blocked. Decoded timings from MMIO agreeing with i915's own"
say "reading is two independent paths to the same answer."

mountpoint -q /sys/kernel/debug 2>/dev/null || mount -t debugfs none /sys/kernel/debug 2>/dev/null
DRI=""
for D in /sys/kernel/debug/dri/*; do
  [ -d "$D" ] || continue
  grep -qs i915 "$D/name" 2>/dev/null && DRI="$D"
done
if [ -z "$DRI" ]; then
  say "SKIP   : no i915 debugfs node. That is a CONFIG_DEBUG_FS fact, NOT a"
  say "         statement about the hardware."
else
  say "i915 debugfs: $DRI"
  for F in i915_display_info i915_ddb_info i915_current_bpc i915_dpcd \
           i915_power_domain_info i915_runtime_pm_status \
           i915_frontbuffer_tracking i915_gem_framebuffer i915_dp_mst_info; do
    [ -r "$DRI/$F" ] && cp "$DRI/$F" "$OUT/04-$F.txt" 2>/dev/null && say "  04-$F.txt"
  done
fi

# ---------------------------------------------------------------------------
rule "5. Panel and connectors"
for C in /sys/class/drm/card*-*; do
  [ -e "$C/status" ] || continue
  N=$(basename "$C")
  say "$N  status=$(cat "$C/status" 2>/dev/null)"
  if [ -r "$C/modes" ]; then
    cat "$C/modes" > "$OUT/05-$N-modes.txt" 2>/dev/null
    M=$(wc -l < "$OUT/05-$N-modes.txt" 2>/dev/null || echo 0)
    if [ "$M" -gt 0 ]; then
      say "  $M modes, preferred $(head -1 "$OUT/05-$N-modes.txt")"
    else
      say "  mode list EMPTY -- expected when disconnected; on a CONNECTED"
      say "  connector that is a capture failure, not a panel fact."
    fi
  fi
  if [ -r "$C/edid" ] && [ -s "$C/edid" ]; then
    cat "$C/edid" > "$OUT/05-$N-edid.bin" 2>/dev/null
    say "  EDID $(wc -c < "$OUT/05-$N-edid.bin") bytes"
    command -v edid-decode >/dev/null 2>&1 && \
      edid-decode "$OUT/05-$N-edid.bin" > "$OUT/05-$N-edid.txt" 2>&1
  else
    say "  no EDID. Normal on a fixed-mode eDP panel; the timings then live in"
    say "  the mode list and in 04-i915_display_info.txt. NOT evidence the"
    say "  panel is undocumented."
  fi
done

# ---------------------------------------------------------------------------
rule "6. NVIDIA side"
if [ -L "/sys/bus/pci/devices/$NV/driver" ]; then
  say "driver = $(basename "$(readlink -f "/sys/bus/pci/devices/$NV/driver")")"
  for C in /sys/class/drm/card*-*; do
    case "$(readlink -f "$C" 2>/dev/null)" in
      *"$NV"*) say "  3050 owns $(basename "$C") status=$(cat "$C/status" 2>/dev/null)" ;;
    esac
  done
  ROM="/sys/bus/pci/devices/$NV/rom"
  if [ -e "$ROM" ] && echo 1 > "$ROM" 2>/dev/null; then
    cat "$ROM" > "$OUT/07-vbios.rom" 2>/dev/null
    echo 0 > "$ROM" 2>/dev/null
    SZ=$(wc -c < "$OUT/07-vbios.rom" 2>/dev/null || echo 0)
    say "  VBIOS $SZ bytes"
    [ "$SZ" -lt 4096 ] && say "  FAIL: too small to be a VBIOS -- not a dump."
  fi
else
  say "SKIP   : $NV has no driver bound. It therefore registers no connector"
  say "         and its ROM reads EIO from D3cold. Both absences are"
  say "         pre-determined by that, NOT evidence about the wiring."
fi

# ---------------------------------------------------------------------------
rule "7. Firmware and kernel log"
dmesg 2>/dev/null | grep -iE 'dmc|i915|drm' > "$OUT/06-i915-dmesg.txt"
grep -i dmc "$OUT/06-i915-dmesg.txt" | sed 's/^/  /' | tee -a "$LOG"
ls -la /lib/firmware/i915/ > "$OUT/06-firmware-files.txt" 2>&1
dmesg 2>/dev/null > "$OUT/08-dmesg-full.txt"

# ---------------------------------------------------------------------------
TAR="$OUT.tar.gz"
if tar czf "$TAR" -C "$(dirname "$OUT")" "$(basename "$OUT")" 2>/dev/null; then
  say ""
  say "=== done ==="
  say "send back: $TAR ($(wc -c < "$TAR") bytes)"
else
  say ""
  say "=== done, but tar failed ==="
  say "send back the directory: $OUT"
fi
