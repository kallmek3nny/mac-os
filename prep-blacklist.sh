#!/bin/sh
#
# prep-blacklist.sh -- install the NVIDIA modprobe blacklist permanently, so you
# never have to type it at the GRUB menu again.
#
# Run from the desktop, then REBOOT:
#
#     sudo sh prep-blacklist.sh
#     sudo reboot
#
# The reboot is not optional and not a formality. This script only writes config
# files; the driver already in memory stays loaded until the machine restarts,
# so capture.sh will keep failing its guard until you reboot.
#
# After the reboot the desktop comes up on the Intel iGPU with the 3050
# completely unclaimed, and capture.sh will pass its guard.
#
# SAFE TO RE-RUN. It overwrites its own config files every time, so you never
# need to --undo first. Do not run --undo as a "reset" before re-running: undo
# removes the blacklist, so the next boot loads the driver again and puts you
# straight back where you started. (That is exactly what happened at
# 2026-08-06 20:53 UTC.)
#
# This writes real files to the installed system, unlike the one-time GRUB edit.
# That is the point -- it survives reboots. Use --undo ONLY when you are
# finished with the whole project and want the NVIDIA driver back:
#
#     sudo sh prep-blacklist.sh --undo
#     sudo reboot
#
# Every step prints PASS or FAIL.
#

set -u

trap '' HUP

CONF=/etc/modprobe.d/zz-mmiotrace-blacklist.conf
GRUBD=/etc/default/grub.d/zz-mmiotrace-blacklist.cfg
MODS="nvidia nvidia_drm nvidia_modeset nvidia_uvm nouveau"
CMDLINE="modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm,nouveau"

say() { echo "$@"; }

# ----------------------------------------------------------------- must be root --
if [ "$(id -u)" -ne 0 ]; then
  say "FAIL   : run this with sudo:  sudo sh $0"
  exit 1
fi

# ------------------------------------------------------------------------ undo --
if [ "${1:-}" = "--undo" ]; then
  say "=== prep-blacklist.sh --undo ==="
  REMOVED=0

  if [ -f "$CONF" ]; then
    rm -f "$CONF" && say "PASS   : removed $CONF" && REMOVED=1
  else
    say "note   : $CONF was not present"
  fi

  if [ -f "$GRUBD" ]; then
    rm -f "$GRUBD" && say "PASS   : removed $GRUBD" && REMOVED=1
  else
    say "note   : $GRUBD was not present"
  fi

  if [ "$REMOVED" -eq 1 ]; then
    if command -v update-initramfs >/dev/null 2>&1; then
      update-initramfs -u >/dev/null 2>&1 \
        && say "PASS   : initramfs rebuilt" \
        || say "WARN   : initramfs rebuild failed -- rerun 'sudo update-initramfs -u'"
    fi
    if command -v update-grub >/dev/null 2>&1; then
      update-grub >/dev/null 2>&1 \
        && say "PASS   : GRUB config regenerated" \
        || say "WARN   : update-grub failed -- rerun 'sudo update-grub'"
    fi
  fi

  for s in gpu-manager nvidia-powerd nvidia-hibernate nvidia-resume nvidia-suspend nvidia-suspend-then-hibernate; do
    if systemctl list-unit-files 2>/dev/null | grep -q "^$s.service"; then
      systemctl enable "$s" >/dev/null 2>&1
    fi
  done
  say "PASS   : re-enabled the NVIDIA systemd services"

  say ""
  say "Done. Reboot and the NVIDIA driver loads normally again:  sudo reboot"
  say ""
  say "NOTE   : the blacklist is now GONE. If you were trying to reset before"
  say "         re-running the capture, that is the wrong move -- this script"
  say "         overwrites its own files, so just run it without --undo. Run"
  say "         --undo only when the whole project is finished."
  exit 0
fi

# --------------------------------------------------------------------- install --
say "=== prep-blacklist.sh $(date -u '+%Y-%m-%d %H:%M UTC') ==="
say "kernel : $(uname -r)"
say ""

#
# Belt and braces, because the two mechanisms cover different load paths:
#
#   1. /etc/modprobe.d/  'blacklist' lines -- suppress udev/alias-driven loads
#      once userspace is up, while leaving an explicit 'modprobe nvidia' by
#      exact module name fully working, dependencies and all. That explicit
#      load is what capture.sh does inside the trace window.
#
#      DO NOT add 'install <mod> /bin/true' here. It looks like a stronger
#      blacklist, but it makes modprobe run /bin/true and exit 0 without
#      loading anything -- including when capture.sh calls it. The 2026-08-06
#      zero-line capture was exactly this: modprobe "succeeded" without doing
#      anything, the then-current insmod fallback inserted bare nvidia.ko with
#      no nvidia_modeset/nvidia_drm, the card was never bound or initialised,
#      and no MMIO was ever traced. capture.sh now falls back to
#      'modprobe --ignore-install', which keeps the dependency chain -- but the
#      right fix is simply not to write 'install' directives here at all.
#
#   2. modprobe.blacklist= on the kernel cmdline -- covers anything loaded from
#      the initramfs, before /etc/modprobe.d is even readable. This is the same
#      string you were typing by hand at GRUB; it is now written to disk.
#

# -- 1. modprobe.d ------------------------------------------------------------
{
  echo "# Written by prep-blacklist.sh for the mmiotrace capture."
  echo "# Remove with:  sudo sh prep-blacklist.sh --undo"
  for m in $MODS; do
    echo "blacklist $m"
  done
} > "$CONF"

if [ -s "$CONF" ]; then
  say "PASS   : wrote $CONF"
else
  say "FAIL   : could not write $CONF"
  exit 1
fi

# -- 2. kernel cmdline --------------------------------------------------------
#
# /etc/default/grub.d/*.cfg is sourced by update-grub after /etc/default/grub,
# so appending here leaves the user's own GRUB_CMDLINE_LINUX_DEFAULT untouched
# and the whole change is one deletable file.
#
if [ -d /etc/default/grub.d ] || mkdir -p /etc/default/grub.d 2>/dev/null; then
  {
    echo "# Written by prep-blacklist.sh for the mmiotrace capture."
    echo "# Remove with:  sudo sh prep-blacklist.sh --undo"
    echo "GRUB_CMDLINE_LINUX_DEFAULT=\"\$GRUB_CMDLINE_LINUX_DEFAULT $CMDLINE\""
  } > "$GRUBD"
  if [ -s "$GRUBD" ]; then
    say "PASS   : wrote $GRUBD"
  else
    say "FAIL   : could not write $GRUBD"
    exit 1
  fi
else
  say "WARN   : no /etc/default/grub.d -- relying on modprobe.d alone"
fi

# -- 2b. NVIDIA systemd services ----------------------------------------------
#
# THE BLACKLIST ALONE IS NOT ENOUGH. Confirmed on this laptop 2026-08-06:
# /proc/cmdline carried the full modprobe.blacklist= string, yet nvidia was
# loaded with refcount 45 by 4.9s into the boot. The culprit is the packaged
# NVIDIA systemd units -- nvidia-powerd in particular -- which run an explicit
# 'modprobe nvidia' by exact module name. Blacklisting only suppresses
# alias/udev-driven loading; an explicit modprobe sails straight through it.
#
# So the units have to be disabled too. --undo re-enables every one of them.
#
# gpu-manager is the big one. It is Ubuntu's ubuntu-drivers-common service, it
# runs before display-manager.service, and it calls /sbin/modprobe directly to
# probe what the discrete GPU can do. Confirmed on this laptop 2026-08-06:
# with nvidia-powerd already disabled and the full blacklist on /proc/cmdline,
# gpu-manager ran 3.99s -> 4.95s and nvidia appeared at 4.35s, right inside
# that window, leaving /run/u-d-c-nvidia-was-loaded behind as its receipt.
NVSVC="gpu-manager nvidia-powerd nvidia-hibernate nvidia-resume nvidia-suspend nvidia-suspend-then-hibernate"
DISABLED=""
for s in $NVSVC; do
  if systemctl list-unit-files 2>/dev/null | grep -q "^$s.service"; then
    systemctl disable --now "$s" >/dev/null 2>&1
    DISABLED="$DISABLED $s"
  fi
done
if [ -n "$DISABLED" ]; then
  say "PASS   : disabled NVIDIA services:$DISABLED"
else
  say "note   : no NVIDIA systemd units present to disable"
fi

# -- 3. regenerate ------------------------------------------------------------
if command -v update-initramfs >/dev/null 2>&1; then
  update-initramfs -u >/dev/null 2>&1 \
    && say "PASS   : initramfs rebuilt" \
    || say "WARN   : initramfs rebuild failed -- rerun 'sudo update-initramfs -u'"
else
  say "note   : no update-initramfs on this system, skipping"
fi

if command -v update-grub >/dev/null 2>&1; then
  update-grub >/dev/null 2>&1 \
    && say "PASS   : GRUB config regenerated" \
    || say "WARN   : update-grub failed -- rerun 'sudo update-grub'"
else
  say "note   : no update-grub on this system, skipping"
fi

# -- 4. confirm ---------------------------------------------------------------
say ""
if grep -q "modprobe.blacklist=nvidia" /boot/grub/grub.cfg 2>/dev/null; then
  say "PASS   : blacklist is present in /boot/grub/grub.cfg"
else
  say "note   : could not confirm in /boot/grub/grub.cfg -- modprobe.d still applies"
fi

say ""
say "Done. Now:"
say ""
say "    sudo reboot"
say ""
say "The desktop will come up on the Intel iGPU. Verify with:"
say ""
say "    lsmod | grep -E 'nvidia|nouveau' || echo 'card is unclaimed -- good'"
say ""
say "Then run the capture:  sudo sh capture.sh"
say ""
say "When you are finished with the whole project, put the driver back:"
say ""
say "    sudo sh prep-blacklist.sh --undo && sudo reboot"
