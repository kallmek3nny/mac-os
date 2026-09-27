#!/bin/sh
#
# capture.sh -- one-shot mmiotrace capture for the RTX 3050 (GA107).
#
# PREREQUISITE -- boot with the driver blacklisted. Add this to the kernel
# line in GRUB (press 'e' at the boot menu), then Ctrl+X to boot:
#
#     modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm,nouveau
#
# Then, from the desktop that comes up on the Intel iGPU:
#     sudo sh capture.sh
#
# This script will NOT unload a running nvidia driver and will NOT stop the
# display manager. Both hard-lock this muxless laptop. If a driver is loaded,
# the script refuses to run and tells you to reboot with the blacklist.
#
# Writes mmio-boot.log.gz next to this script, so it lands on whatever disk
# the script was run from -- normally the Windows partition, which is exactly
# where it needs to end up. Nothing is written to the live filesystem, which
# lives in RAM and would fill.
#
# Every step prints PASS or FAIL. If anything says FAIL, stop and report it
# rather than continuing -- a partial capture looks like a real one.
#

set -u

#
# Survive SIGHUP. If the controlling terminal goes away mid-run for any reason,
# the capture should still complete and still write its summary rather than
# dying silently, so there is always a file to report.
#
trap '' HUP

OUT="$(cd "$(dirname "$0")" && pwd)"
LOG="$OUT/mmio-boot.log"
SUMMARY="$OUT/capture-summary.txt"

say() { echo "$@" | tee -a "$SUMMARY"; }

: > "$SUMMARY"

say "=== capture.sh $(date -u '+%Y-%m-%d %H:%M UTC') ==="
say "kernel : $(uname -r)"
say "output : $OUT"
say ""

#
# Writability check FIRST. An NTFS partition with Windows fast-startup enabled
# mounts read-only, and finding that out after a 20-second capture means doing
# the whole thing again. Fail here instead.
#
if ! touch "$OUT/.writetest" 2>/dev/null; then
  echo "FAIL   : $OUT is not writable."
  echo "         Likely an NTFS partition mounted read-only because Windows"
  echo "         fast-startup left it dirty. Copy capture.sh onto the USB"
  echo "         stick and run it from there instead."
  exit 1
fi
rm -f "$OUT/.writetest"
say "PASS   : output directory is writable"

#
# Writable is not sufficient -- it also has to be a REAL disk. The live
# session's home directory is tmpfs/overlayfs, i.e. RAM. A trace can run to
# hundreds of megabytes, and filling RAM kills the session mid-capture with no
# log to show for it. Writable-but-in-RAM is the trap this catches.
#
OUTFS=$(df -T "$OUT" 2>/dev/null | awk 'NR==2 {print $2}')
OUTFREE=$(df -Pk "$OUT" 2>/dev/null | awk 'NR==2 {print $4}')
say "outfs  : $OUTFS, $((OUTFREE / 1024)) MB free"

case "$OUTFS" in
  tmpfs|overlay|ramfs|squashfs)
    say "FAIL   : $OUT is on $OUTFS -- that is RAM, not disk."
    say "         Only applies to LIVE USB sessions, whose filesystem is in"
    say "         memory. A capture can exhaust it and kill the session"
    say "         mid-trace. On an installed Ubuntu this branch never fires."
    say "         Move capture.sh onto a real disk and re-run from there."
    exit 1
    ;;
esac

if [ "${OUTFREE:-0}" -lt 2097152 ]; then
  say "WARN   : under 2 GB free -- a large capture may not fit"
fi

# ---------------------------------------------------------------- identity --
CARD=$(lspci -nn | grep -i nvidia)
say "GPU    : $CARD"

case "$CARD" in
  *10de:25a2*) say "PASS   : GA107 confirmed" ;;
  *)           say "WARN   : expected 10de:25a2, got the above" ;;
esac

# ----------------------------------------------------------------- debugfs --
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
if [ -r /sys/kernel/debug/tracing/current_tracer ]; then
  say "PASS   : debugfs mounted"
else
  say "FAIL   : debugfs not available -- are you root? (use sudo)"
  exit 1
fi

# --------------------------------------------------------------- mmiotrace --
if grep -q mmiotrace /sys/kernel/debug/tracing/available_tracers; then
  say "PASS   : mmiotrace present in this kernel"
else
  say "FAIL   : mmiotrace missing -- kernel lacks CONFIG_MMIOTRACE"
  exit 1
fi

# ------------------------------------------------------- driver must be out --
# The trace must capture driver INITIALISATION, so the card has to start
# unclaimed. This script will NOT unload it for you: unloading nvidia on a
# running system is what hard-locked the machine on a previous attempt, and it
# can lock it again with no log to show for it. The only supported route is to
# boot with the modules blacklisted so they were never loaded in the first
# place. The check below enforces that and refuses to go further.

#
# nvidia_drm typically has a nonzero refcount because the desktop compositor
# holds it open as a PRIME render-offload provider -- which is exactly what a
# muxless laptop does.
#
# An earlier version of this script stopped the display manager here. That was
# a real bug: when run from a terminal INSIDE the desktop, stopping the display
# manager tears down the terminal too, so the script is killed mid-run by
# SIGHUP and leaves a summary that just stops after the note. Never do that
# from a graphical session.
#
if lsmod | grep -qE '^(nvidia|nouveau)'; then
  if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    say "FAIL   : the driver is held open by this desktop session."
    say ""
    lsmod | grep -E '^(nvidia|nouveau)' | tee -a "$SUMMARY"
    say ""
    say "         Stopping the display manager from inside the desktop would"
    say "         also kill this script, so it will not be attempted here."
    say ""
    say "         DO NOT run 'systemctl stop gdm3' to work around this. On a"
    say "         muxless NVIDIA laptop that hangs the machine hard: gdm3 tears"
    say "         down the display while nvidia_drm still owns KMS, and nothing"
    say "         is left to hand the console back to. Confirmed on this exact"
    say "         hardware -- it required a REISUB to recover."
    say ""
    say "         THE ONLY SUPPORTED ROUTE -- blacklist the driver at boot, so"
    say "         the card is never claimed and nothing has to be torn down."
    say "         This also yields a strictly better trace: a genuine cold"
    say "         initialisation, the one and only one the firmware ever sees."
    say ""
    say "         Run the helper, which installs the blacklist to disk so you"
    say "         never type it at the GRUB menu:"
    say ""
    say "             sudo sh $OUT/prep-blacklist.sh"
    say "             sudo reboot"
    say ""
    if grep -q 'modprobe.blacklist=nvidia' /proc/cmdline 2>/dev/null; then
      say "         !! This boot ALREADY has the blacklist on its cmdline, yet"
      say "            the driver is loaded anyway. Something is loading it"
      say "            explicitly -- check /etc/modprobe.d and any local unit"
      say "            or script that runs 'modprobe nvidia' at login."
    elif [ -f /etc/modprobe.d/zz-mmiotrace-blacklist.conf ]; then
      say "         !! The blacklist IS installed on disk but this boot did not"
      say "            pick it up. You have not rebooted since installing it."
      say "            Just reboot -- do NOT run prep-blacklist.sh --undo,"
      say "            which would remove the blacklist again."
    else
      say "         !! No blacklist is installed on disk. Run the helper above."
      say "            If you ran it with --undo, that REMOVED it: run it again"
      say "            without --undo, then reboot."
    fi
    say ""
    say "         Then, back on the Intel iGPU desktop:"
    say ""
    say "             cd $OUT && sudo sh capture.sh"
    say ""
    say "         When the whole project is done, put the driver back with:"
    say ""
    say "             sudo sh $OUT/prep-blacklist.sh --undo && sudo reboot"
    say ""
    say "         (Manual alternative, if you would rather not touch the"
    say "         installed system: at the GRUB menu press  e , append"
    say "         modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm,nouveau"
    say "         to the  linux  line, and press Ctrl+X. That edit is not"
    say "         saved, so it must be retyped on every boot.)"
    exit 1
  fi

  #
  # No DISPLAY, so this is a text console. Even here the script does NOT stop
  # the display manager: on a muxless laptop that hangs the machine outright,
  # because gdm3 releases the display while nvidia_drm still owns KMS and no
  # console driver is left to take over. Being on a TTY does not help -- the
  # TTY is one of the things that dies.
  #
  # Blacklisting at boot is the only route that works, and it is also the only
  # route that produces a clean cold-init trace.
  #
  say "FAIL   : the driver is loaded and in use, and cannot be unloaded safely."
  say ""
  lsmod | grep -E '^(nvidia|nouveau)' | tee -a "$SUMMARY"
  say ""
  say "         Boot with the driver blacklisted so it is never claimed."
  say "         The helper installs it to disk once, so there is nothing to"
  say "         type at the GRUB menu:"
  say ""
  say "             sudo sh $OUT/prep-blacklist.sh"
  say "             sudo reboot"
  say ""
  say "         Then re-run this script:"
  say ""
  say "             cd $OUT && sudo sh capture.sh"
  say ""
  say "         Undo when the project is finished:"
  say ""
  say "             sudo sh $OUT/prep-blacklist.sh --undo && sudo reboot"
  exit 1
fi

say "PASS   : card is unclaimed"

# -------------------------------------------------------------- capture run --
say ""
say "tracing for ~20s, do not touch the machine..."

TR=/sys/kernel/debug/tracing

#
# Reset any state a previous run left behind. tracing_on=0 survives across runs
# and silently produces an empty capture: the tracer attaches, trace_pipe stays
# open, and not one line is ever emitted. That is exactly the zero-line failure
# this script hit on 2026-08-06.
#
echo nop        > "$TR/current_tracer"  2>/dev/null
echo 1          > "$TR/tracing_on"      2>/dev/null
echo            > "$TR/trace"           2>/dev/null

# mmiotrace is high-volume; the default per-CPU buffer overflows and drops the
# early init writes, which are the ones Phase 4 needs most.
echo 131072     > "$TR/buffer_size_kb"  2>/dev/null

if ! echo mmiotrace > "$TR/current_tracer" 2>/dev/null; then
  say "FAIL   : could not select mmiotrace -- is another tracer in use?"
  exit 1
fi

NOW=$(cat "$TR/current_tracer" 2>/dev/null)
if [ "$NOW" != mmiotrace ]; then
  say "FAIL   : current_tracer is '$NOW', not mmiotrace"
  exit 1
fi
say "PASS   : mmiotrace active, tracing_on=$(cat "$TR/tracing_on" 2>/dev/null)"

cat "$TR/trace_pipe" > "$LOG" &
PIPE=$!
sleep 2

if ! kill -0 "$PIPE" 2>/dev/null; then
  say "FAIL   : the trace_pipe reader died immediately"
  exit 1
fi

#
# Loading the driver HERE is the whole point -- this is the initialisation the
# tracer is watching. Booting with modprobe.blacklist= does not stop this:
# the blacklist only suppresses automatic/alias-driven loading, so an explicit
# modprobe by exact module name still inserts the module, dependencies and all.
#
#
# The fallback is 'modprobe --ignore-install', NOT insmod. Both bypass an
# 'install ... /bin/true' directive, but insmod loads exactly one .ko by path
# and resolves NO dependencies -- so it inserts bare nvidia.ko without
# nvidia_modeset/nvidia_drm, which never binds the card, never calls ioremap,
# and yields a zero-line trace while still reporting DRIVER=nvidia. That is
# precisely how the 2026-08-06 20:34 UTC capture failed silently.
# --ignore-install keeps the full dependency chain.
#
if modprobe nvidia 2>/dev/null && lsmod | grep -q '^nvidia'; then
  DRIVER=nvidia
elif modprobe --ignore-install nvidia 2>/dev/null && lsmod | grep -q '^nvidia'; then
  say "note   : plain modprobe was a no-op -- used --ignore-install."
  say "         Something in /etc/modprobe.d has an 'install' directive for"
  say "         nvidia. Check for a stale prep-blacklist.sh conf file."
  DRIVER=nvidia
elif modprobe nouveau 2>/dev/null && lsmod | grep -q '^nouveau'; then
  DRIVER=nouveau
elif modprobe --ignore-install nouveau 2>/dev/null && lsmod | grep -q '^nouveau'; then
  DRIVER=nouveau
else
  DRIVER=none
fi

sleep 15
kill "$PIPE" 2>/dev/null
wait "$PIPE" 2>/dev/null

#
# trace_pipe is a CONSUMING read, but anything still sitting in the ring buffer
# when the reader was killed is only visible via 'trace'. Append it so nothing
# captured is thrown away.
#
cat "$TR/trace" >> "$LOG" 2>/dev/null

echo nop > "$TR/current_tracer" 2>/dev/null
sync

say "driver : $DRIVER"

if [ "$DRIVER" = none ]; then
  say "FAIL   : neither nvidia nor nouveau would load -- nothing to trace"
  exit 1
fi

#
# The module inserting is not the same as the module BINDING to the card. If
# nvidia loaded but never claimed 01:00.0 -- which is what happens when only
# bare nvidia.ko went in without nvidia_modeset/nvidia_drm -- there is no
# ioremap, so there is no MMIO to trace and the log comes out empty.
#
DRV=$(lspci -k -s 01:00.0 2>/dev/null | grep -i 'Kernel driver in use' | cut -d: -f2)
if [ -n "$DRV" ]; then
  say "bound  : $DRV"
else
  say "WARN   : no kernel driver bound to 01:00.0 -- the module loaded but"
  say "         never claimed the card, so nothing was mapped or traced."
fi

# ---------------------------------------------------------------- verdict ---
LINES=$(wc -l < "$LOG")
say "lines  : $LINES"

if [ "$LINES" -lt 1000 ]; then
  say "FAIL   : capture is too small -- the tracer did not attach"
  say "         (a real capture is tens of thousands of lines)"
  exit 1
fi

say "PASS   : capture looks real"

gzip -f "$LOG"
say "saved  : $LOG.gz ($(du -h "$LOG.gz" | cut -f1))"
say ""
say "Done. Both files are in $(dirname "$LOG")."
say "       If that is a Linux filesystem (ext4), copy them to a USB stick or"
say "       to the Windows partition first -- Windows cannot read ext4."
