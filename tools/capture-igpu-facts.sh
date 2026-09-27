#!/bin/sh
#
# capture-igpu-facts.sh -- settle the four hardware facts this project has never
# measured, from an INSTALLED Ubuntu on the target laptop.
#
#     sudo sh capture-igpu-facts.sh
#
# Read-only. Loads nothing, unloads nothing, changes no state. Unlike
# capture.sh it does NOT want the NVIDIA driver blacklisted -- the opposite,
# in fact: several checks below are only meaningful when both drivers are
# loaded and bound.
#
# WHY THIS EXISTS
# ---------------
# HARDWARE.md's muxless verdict rests on two checks taken from a LIVE USB
# session, and that session reported `Providers: number : 0` -- meaning
# NEITHER GPU registered a DRI3 provider, because the live image was running on
# a generic EFI framebuffer with no accelerated driver bound at all.
#
# A zero from a session where the answering subsystem was not running is not a
# measurement of the mux. It is a measurement of the live USB. The verdict may
# well be right, but the evidence for it is an absence produced by an untested
# method -- the exact failure mode already recorded twice in VM-BRINGUP.md.
#
# Termination Condition A in PLAN.md ends the project. It currently rests on
# that evidence. This script replaces it with positive evidence either way.
#
# EVERY CHECK BELOW STATES WHAT ITS NULL RESULT DOES NOT PROVE.

set -u

OUT="${1:-$HOME/igpu-facts-$(date -u +%Y%m%dT%H%M%SZ).txt}"

say() { echo "$*" | tee -a "$OUT"; }
rule() { say ""; say "=== $* ==="; }

: > "$OUT" || { echo "cannot write $OUT"; exit 1; }

say "=== capture-igpu-facts.sh $(date -u '+%Y-%m-%d %H:%M UTC') ==="
say "kernel : $(uname -r)"
say "distro : $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
say "output : $OUT"

# ---------------------------------------------------------------------------
# 1. The iGPU's PCI ID. Never recorded anywhere in this project.
# ---------------------------------------------------------------------------
rule "1. PCI display devices (the iGPU ID this tree has never captured)"
say "Everything with a display class code, vendor:device in brackets."
say ""
lspci -nn | grep -Ei 'vga|3d controller|display' | tee -a "$OUT"

rule "1b. Driver binding for each display device"
say "'Kernel driver in use' is the load-bearing line. A device with NO such"
say "line is unclaimed, which changes how every later check reads."
say ""
for SLOT in $(lspci -n | grep -Ei ' (0300|0302|0380):' | cut -d' ' -f1); do
  say "--- $SLOT ---"
  lspci -k -s "$SLOT" | tee -a "$OUT"
done

# ---------------------------------------------------------------------------
# 2. The mux. This is the check that decides Termination Condition A.
# ---------------------------------------------------------------------------
rule "2. vgaswitcheroo"
if [ -e /sys/kernel/debug/vgaswitcheroo/switch ]; then
  say "PRESENT -- a mux exists and is switchable:"
  cat /sys/kernel/debug/vgaswitcheroo/switch | tee -a "$OUT"
else
  say "ABSENT."
  say ""
  say "CAVEAT: this file only appears when a driver REGISTERS a handler. If"
  say "neither i915 nor nouveau/nvidia is bound (see section 1b), absence here"
  say "describes the driver state, not the hardware. Absence WITH both drivers"
  say "bound is real evidence of no mux."
fi

rule "2b. Is i915 actually loaded and bound?"
say "This is the control the live-USB run did not have. Without it, a zero in"
say "section 3 is uninterpretable."
say ""
if lsmod 2>/dev/null | grep -q '^i915'; then
  say "PASS   : i915 module is loaded"
else
  say "FAIL   : i915 NOT loaded -- sections 3 and 4 will under-report."
  say "         Do not read their output as a hardware fact."
fi
if lsmod 2>/dev/null | grep -Eq '^(nvidia|nouveau)'; then
  say "PASS   : an NVIDIA-side driver is loaded"
  lsmod | grep -E '^(nvidia|nouveau)' | tee -a "$OUT"
else
  say "NOTE   : no nvidia/nouveau module loaded -- the 3050 side of every"
  say "         provider/connector check below will be silent for that reason"
  say "         alone, which is NOT evidence about the wiring."
fi

rule "2c. Is either driver blacklisted on the kernel command line?"
say "capture.sh asks the operator to boot with the NVIDIA stack blacklisted, and"
say "that setting persists in GRUB. If it is still present the 3050 has no"
say "driver bound, and it CANNOT register a DRI3 provider or a DRM connector no"
say "matter how it is wired. Every NVIDIA-side absence below is then"
say "pre-determined by this one line, and is not evidence about the hardware."
say ""
BLACKLIST=$(tr ' ' '\n' < /proc/cmdline 2>/dev/null | grep '^modprobe.blacklist=')
if [ -n "$BLACKLIST" ]; then
  say "PRESENT: $BLACKLIST"
  case "$BLACKLIST" in
    *nvidia*|*nouveau*)
      say "FAIL   : the NVIDIA stack is blacklisted. Sections 3, 4 and 5 cannot"
      say "         say anything about the 3050. Re-run from a boot WITHOUT"
      say "         this argument to settle the NVIDIA side."
      ;;
  esac
else
  say "ABSENT : no modprobe.blacklist on the kernel command line"
fi

rule "3. DRI3 render providers"
say "A muxless Optimus box with both drivers bound reports TWO providers."
say "Read this ONLY if 2b passed AND 2c came back ABSENT."
say ""
if [ -z "${DISPLAY:-}" ]; then
  say "SKIP   : no DISPLAY set. Run this from a graphical session, not a TTY"
  say "         and not over plain ssh -- otherwise the zero means 'no X"
  say "         connection', which is what the live-USB capture actually"
  say "         measured."
elif [ -n "${WAYLAND_DISPLAY:-}" ] || [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
  say "SKIP   : Wayland session (WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}, "
  say "         XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-}). xrandr talks to"
  say "         XWayland, which owns no DRM device and registers no providers,"
  say "         so it prints 'Providers: number : 0' on EVERY machine --"
  say "         single-GPU, dual-GPU, muxed, muxless. That number would be a"
  say "         property of the compositor, not of this laptop. Read section 4"
  say "         instead."
  say ""
  say "         Printed below for the record, and uninterpretable as data:"
  xrandr --listproviders 2>&1 | sed 's/^/         | /' | tee -a "$OUT"
else
  xrandr --listproviders 2>&1 | tee -a "$OUT"
fi

# ---------------------------------------------------------------------------
# 4. Which GPU owns the laptop panel. The decisive positive test.
# ---------------------------------------------------------------------------

# Resolve a sysfs node to the PCI device that owns it, by walking up parents
# until a directory name is a PCI BDF.
#
# The obvious `$C/device/..` shortcut is WRONG for connectors, and the first
# run of this script on the target laptop (2026-08-15) proved it:
# /sys/class/drm/cardN-eDP-1/device points at the CARD, not at the PCI
# device, so `device/..` landed on the intermediate `drm/` directory and every
# row printed `pci=drm driver=driver`. Two rows of that read like data. They
# were the resolver failing on every input, which is why the loop below now
# reports UNRESOLVED loudly instead of printing whatever basename it reached.
pci_owner() {
  P=$(readlink -f "$1" 2>/dev/null)
  while [ -n "$P" ] && [ "$P" != "/" ] && [ "$P" != "." ]; do
    B=$(basename "$P")
    case "$B" in
      [0-9a-f][0-9a-f][0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f].[0-9a-f])
        echo "$B"; return 0 ;;
    esac
    P=$(dirname "$P")
  done
  echo "UNRESOLVED"
  return 1
}

rule "4. DRM connectors, resolved to their owning PCI device"
say "This is the strongest available evidence and it is POSITIVE rather than"
say "an absence: the eDP connector is the built-in panel. Whichever PCI device"
say "owns it is the device physically wired to the screen."
say ""
say "If eDP sits under the Intel device and the NVIDIA device has no connectors"
say "at all, the laptop is muxless and Termination Condition A holds."
say ""
BAD=0
for C in /sys/class/drm/card*-*; do
  [ -e "$C/status" ] || continue
  CARD=$(basename "$C")
  PCI=$(pci_owner "$C")
  STATUS=$(cat "$C/status" 2>/dev/null)
  if [ -L "/sys/bus/pci/devices/$PCI/driver" ]; then
    DRV=$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI/driver")")
  else
    DRV="(unclaimed)"
  fi
  [ "$PCI" = "UNRESOLVED" ] && BAD=1
  say "$CARD  status=$STATUS  pci=$PCI  driver=$DRV"
done
if [ "$BAD" = 1 ]; then
  say ""
  say "FAIL   : at least one connector did not resolve to a PCI address."
  say "         Those rows are resolver output, NOT hardware facts. Do not"
  say "         read a panel owner out of them."
fi

rule "4b. Raw card -> PCI mapping"
for D in /sys/class/drm/card[0-9]; do
  [ -e "$D" ] || continue
  say "$(basename "$D") -> $(basename "$(readlink -f "$D/device")")"
done

# ---------------------------------------------------------------------------
# 5. VBIOS. Useful regardless of how the mux question lands.
# ---------------------------------------------------------------------------
rule "5. NVIDIA VBIOS"
ROM=/sys/bus/pci/devices/0000:01:00.0/rom
if [ -e "$ROM" ]; then
  DEST="$(dirname "$OUT")/vbios-25a2.rom"
  if echo 1 > "$ROM" 2>/dev/null; then
    if cat "$ROM" > "$DEST" 2>/dev/null; then
      echo 0 > "$ROM" 2>/dev/null
      SZ=$(wc -c < "$DEST")
      say "wrote $DEST ($SZ bytes)"
      # A zero-byte or tiny file is the documented failure, not a ROM.
      if [ "$SZ" -lt 4096 ]; then
        say "FAIL   : $SZ bytes is too small to be a VBIOS. The ROM was not"
        say "         readable (common when the device is not the boot VGA)."
        say "         Do not treat this file as a dump."
      else
        say "PASS   : plausible VBIOS size"
        head -c 2 "$DEST" | od -An -tx1 | tee -a "$OUT"
        say "         (expect 55 aa)"
      fi
    else
      say "FAIL   : ROM present but unreadable."
      say "         The enable write SUCCEEDED, so this is not a permissions"
      say "         problem. A PCI ROM is only readable while the device is"
      say "         powered and enabled; an unclaimed device (no driver bound --"
      say "         see 1b and 2c) sits in D3cold and the read returns EIO."
      say "         Clear the blacklist so nouveau or nvidia binds, then re-run."
      say "         Do NOT record this as 'the card has no ROM'."
    fi
  else
    say "FAIL   : could not enable ROM reading (need root)"
  fi
else
  say "ABSENT : no ROM attribute at $ROM"
fi

rule "6. Kernel messages from the graphics drivers"
dmesg 2>/dev/null | grep -Ei 'i915|nouveau|nvidia|drm' | tail -60 | tee -a "$OUT"

say ""
say "=== done -- send back $OUT ==="
