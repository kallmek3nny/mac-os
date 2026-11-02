#!/bin/sh
#
# guest-install-bootkc.sh -- put NV3050Probe.kext into the guest's Boot KC, so
# the IntelFbPresentProbe personality is in the IOCatalogue before the IGPU nub
# is first matched (crew card t-d70ad5, DRIVER FB-4). Run it inside the macOS
# guest, from the console, after guest-build-kext.sh has built the bundle.
#
# Why the Boot KC (xnu main from apple-oss-distributions, read 2026-11-01)
# ------------------------------------------------------------------------
# `make install` copies the bundle to /Library/Extensions (phase0-kext/Makefile
# :15 and :194-199), which puts it in the auxiliary KC. Only Primary-KC
# personalities reach the IOCatalogue at bootstrap (libsa/bootstrap.cpp:206 and
# :220). Aux-KC personalities are sent only at the kernelmanagerd check-in
# (libkern/c++/OSKext.cpp:13426-13438). By then IONDRVFramebuffer holds the
# IOFramebuffer category, and a personality that is not from the Primary KC is
# skipped (iokit/Kernel/IOService.cpp:3945-3966, IOCatalogue.cpp:592-602).
# Taking the slot later needs kIOPrimaryDriverTerminateOptions on the holder
# (IOService.cpp:5452-5477). A release kernel never sets it: :1604-1617 is
# DEVELOPMENT or DEBUG only and :5114-5118 is dead code.
#
# What is UNPROVEN
# ----------------
# kmutil's source is not published. These come from kmutil(8), bless(8) and
# common practice, not from source: that a kext in System/Library/Extensions
# with OSBundleRequired goes into the Boot KC, the kmutil options, the KDK
# location, and the mount and bless steps. So the script records the --help of
# the kmutil subcommand it picks and refuses when an option it would pass is
# missing.
#
# The kmutil arguments follow OpenCore Legacy Patcher's _kmutil_arguments() in
# sys_patch/kernelcache/kernel_collection/boot_system.py (main, read
# 2026-11-02): `create` from Ventura (Darwin 22) on, `install` before it, and
# always --volume-root MNT --update-all --variant-suffix release. OCLP also
# passes --allow-missing-kdk from Ventura on. This script does not: it requires
# a KDK below and would rather kmutil fail than build a collection without one.
#
# The bless steps follow OCLP's sys_patch/mount/snapshot.py (main, read
# 2026-11-02). On Intel, create_snapshot() runs bless --folder
# MNT/System/Library/CoreServices --bootefi --create-snapshot, and
# revert_snapshot() runs bless --mount PATH --bootefi --last-sealed-snapshot.
# Both are skipped before Big Sur. This script targets an Intel guest only.
#
# Modes
# -----
#   (default)   dry run: read-only checks, the commands --apply would run, and a
#               verdict. Mounts, copies and blesses nothing. Exit 0 = ready.
#   --apply     as root: mount the system volume read-write, copy the bundle to
#               its System/Library/Extensions, rebuild the collections, and
#               bless a new root snapshot. It takes effect at the next boot. The
#               script does not reboot.
#   --rollback  as root: go back to the last sealed snapshot (bless(8)).
#
# The WHOLE bundle moves, not one personality. IntelGtBringup and every other
# personality in Info.plist also match from the Boot KC. A panic in any start()
# then repeats on every boot. Recover from Recovery (SETUP.md Step 7b).
#
# POSIX sh, like guest-build-kext.sh. Usage errors exit 2. Refusals and
# failures exit 1.

set -u

MODE=dry-run
KEXT="${NV_KEXT_PATH:-${NV_WORK_DIR:-$HOME/nv3050-step7}/phase0-kext/build/NV3050Probe.kext}"
MNT="${NV_SYSVOL_MNT:-/tmp/nv3050-sysvol}"
DEV="${NV_SYSVOL_DEV:-}"
DEV_GIVEN=no
[ -n "$DEV" ] && DEV_GIVEN=yes
LOG="${NV_BOOTKC_LOG:-/tmp/nv3050-bootkc.log}"
KDK_DIR=/Library/Developer/KDKs

usage() {
    cat <<'USAGE'
Usage: sh guest-install-bootkc.sh [--apply | --rollback] [--kext DIR]
           [--device DEV] [--mount-point DIR] [--log FILE]

  (no mode)          dry run: read-only checks and the planned commands
  --apply            install into the Boot KC; needs root; the next boot uses it
  --rollback         go back to the last sealed snapshot; needs root
  --kext DIR         the built bundle (default: build/NV3050Probe.kext under
                     nv3050-step7/phase0-kext in your home; NV_KEXT_PATH)
  --device DEV       system volume device node (default: derived from
                     diskutil info /; NV_SYSVOL_DEV)
  --mount-point DIR  where to mount it read-write (default /tmp/nv3050-sysvol;
                     NV_SYSVOL_MNT)
  --log FILE         log file (default /tmp/nv3050-bootkc.log; NV_BOOTKC_LOG)
  -h, --help         print this and exit 0

A command-line value wins over the environment, which wins over the default.
Unknown arguments are refused with exit 2 rather than ignored.
USAGE
}

need_value() {
    if [ "$2" -lt 2 ]; then
        echo "FAIL: $1 needs a value"
        exit 2
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --apply|--rollback)
            if [ "$MODE" != dry-run ]; then
                echo "FAIL: give at most one of --apply and --rollback"
                exit 2
            fi
            MODE="${1#--}"
            shift
            ;;
        --kext) need_value "$1" $#; KEXT="$2"; shift 2 ;;
        --kext=*) KEXT="${1#*=}"; shift ;;
        --device) need_value "$1" $#; DEV="$2"; DEV_GIVEN=yes; shift 2 ;;
        --device=*) DEV="${1#*=}"; DEV_GIVEN=yes; shift ;;
        --mount-point) need_value "$1" $#; MNT="$2"; shift 2 ;;
        --mount-point=*) MNT="${1#*=}"; shift ;;
        --log) need_value "$1" $#; LOG="$2"; shift 2 ;;
        --log=*) LOG="${1#*=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *)
            echo "FAIL: unknown argument: $1"
            echo "      Refusing rather than ignoring it."
            echo
            usage
            exit 2
            ;;
    esac
done

# A trailing slash would make cp -R copy the bundle's CONTENTS into
# System/Library/Extensions instead of the bundle itself.
KEXT="${KEXT%/}"

# Empty values are caller mistakes. "/" as the mount point would mount the
# system volume over the running root.
if [ -z "$KEXT" ] || [ -z "$LOG" ]; then
    echo "FAIL: --kext and --log need non-empty values"
    exit 2
fi
if [ "$DEV_GIVEN" = yes ] && [ -z "$DEV" ]; then
    echo "FAIL: --device was given an empty value"
    exit 2
fi
case "$MNT" in
    /) echo "FAIL: --mount-point must not be /"; exit 2 ;;
    /*) ;;
    *) echo "FAIL: --mount-point must be an absolute path: $MNT"; exit 2 ;;
esac

OUT="$LOG.out"
( : > "$LOG" ) 2>/dev/null || { echo "FAIL: cannot write $LOG (pass --log)"; exit 1; }

log() {
    printf '%s\n' "$*" | tee -a "$LOG"
}
fail() {
    log "FAIL: $*"
    exit 1
}
PROBLEMS=0
problem() {
    log "NOT READY: $*"
    PROBLEMS=$((PROBLEMS + 1))
}
# run CMD...: log the command, run it with both streams captured, copy the
# output to the console and the log, and return the command's OWN status. No
# pipeline, so the status is never tee's (see guest-build-kext.sh).
run() {
    log "+ $*"
    "$@" > "$OUT" 2>&1
    rc=$?
    tee -a "$LOG" < "$OUT"
    rm -f "$OUT"
    log "  exit status: $rc"
    return "$rc"
}

OS="$(uname -s)"
[ "$OS" = Darwin ] || fail "this is $OS, not macOS; run it inside the guest"

log "=== FB-4 Boot KC install, mode: $MODE ==="
log "date     : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
log "sw_vers  : $(sw_vers 2>&1 | tr '\n' ' ')"

if [ "$MODE" != dry-run ] && [ "$(id -u)" != 0 ]; then
    fail "--$MODE needs root: sudo sh $0 --$MODE"
fi

if [ "$MODE" = rollback ]; then
    # bless(8), --last-sealed-snapshot: "Reverts back to using the previously
    # signed APFS root snapshot reenabling Authenticated Root Volume." --bootefi
    # copies the boot objects for it. UNPROVEN: that --mount / is the right
    # target while booted from a snapshot that --apply created.
    run bless --mount / --bootefi --last-sealed-snapshot ||
        fail "bless refused, so the boot snapshot is unchanged. If the guest cannot boot, use the Recovery rollback in SETUP.md Step 7b."
    log "Done. Reboot to boot the sealed snapshot. Check csrutil authenticated-root status before another --apply."
    exit 0
fi

KEXT_BASE="$(basename "$KEXT")"
NAME="$(basename "$KEXT" .kext)"
PLIST="$KEXT/Contents/Info.plist"
SLE="$MNT/System/Library/Extensions"

log "--- bundle ---"
log "kext     : $KEXT"
case "$KEXT_BASE" in
    *.kext) ;;
    *) problem "--kext must name a .kext bundle" ;;
esac
if [ -f "$PLIST" ] && [ -f "$KEXT/Contents/MacOS/$NAME" ]; then
    PL="$(plutil -p "$PLIST" 2>&1)"
    printf '%s\n' "$PL" | grep -E '"(OSBundleRequired|IOClass)"' | tee -a "$LOG"
    case "$PL" in
        *'"OSBundleRequired"'*) ;;
        *) problem "no OSBundleRequired in $PLIST, so kmutil would not take it as a boot kext (UNPROVEN rule)" ;;
    esac
else
    problem "no built bundle at $KEXT (run guest-build-kext.sh, or pass --kext)"
fi
if [ -e "/Library/Extensions/$KEXT_BASE" ]; then
    problem "/Library/Extensions/$KEXT_BASE still exists. Remove it first (sudo make uninstall in phase0-kext) so one bundle id is not in two collections"
fi

log "--- SIP (SETUP.md Step 2) ---"
SIP="$(csrutil status 2>&1)"
log "$SIP"
case "$SIP" in
    *"status: disabled"*) ;;
    *) problem "csrutil status does not say disabled" ;;
esac
ARV="$(csrutil authenticated-root status 2>&1)"
log "$ARV"
case "$ARV" in
    *"status: disabled"*) ;;
    *) problem "csrutil authenticated-root status does not say disabled" ;;
esac

log "--- KDK (kmutil(8): needed since macOS 13.0 to create a boot KC; UNPROVEN location) ---"
BUILD="$(sw_vers -buildVersion 2>&1)"
KDK_ANY=no
KDK_MATCH=no
for k in "$KDK_DIR"/*.kdk; do
    [ -e "$k" ] || continue
    KDK_ANY=yes
    log "kdk      : $k"
    case "$k" in *"$BUILD"*) KDK_MATCH=yes ;; esac
done
if [ "$KDK_ANY" = no ]; then
    problem "no KDK under $KDK_DIR; install the one for build $BUILD"
elif [ "$KDK_MATCH" = no ]; then
    log "note     : no KDK name contains build $BUILD; kmutil may refuse (UNPROVEN)"
fi

log "--- kmutil and bless ---"
# OCLP boot_system.py: `create` when detected_os >= ventura, else `install`.
KREL="$(uname -r)"
KMAJ="${KREL%%.*}"
KM_SUB=create
case "$KMAJ" in
    ''|*[!0-9]*)
        problem "cannot read the Darwin major version from uname -r: $KREL" ;;
    *)
        [ "$KMAJ" -ge 22 ] || KM_SUB=install ;;
esac
log "darwin   : $KREL, so kmutil $KM_SUB"
KM_HELP="$(kmutil "$KM_SUB" --help 2>&1)"
printf '%s\n' "$KM_HELP" >> "$LOG"
for opt in --volume-root --update-all --variant-suffix; do
    case "$KM_HELP" in
        *"$opt"*) log "kmutil $KM_SUB --help lists $opt" ;;
        *) problem "kmutil $KM_SUB --help does not list $opt; not guessing a replacement" ;;
    esac
done
command -v bless > /dev/null 2>&1 || problem "no bless on PATH"

log "--- system volume ---"
if [ "$DEV_GIVEN" = no ]; then
    NODE="$(diskutil info / 2>/dev/null | awk -F': *' '/Device Node:/ {print $2; exit}')"
    log "/ is on  : ${NODE:-unknown}"
    # A sealed boot runs from a snapshot device /dev/diskNsMsK of the system
    # volume /dev/diskNsM. UNPROVEN beyond what diskutil prints here.
    case "$NODE" in
        /dev/disk*s*s*) DEV="${NODE%s*}" ;;
        *) problem "/ is not on a snapshot device; check diskutil list and pass --device" ;;
    esac
fi
log "device   : ${DEV:-none}"
if mount | grep -F -q " on $MNT ("; then
    problem "$MNT is already mounted; sudo umount $MNT first"
fi

log "--- plan for --apply ---"
log "mount -o nobrowse -t apfs $DEV $MNT"
log "cp -R $KEXT $SLE/, then chown -R root:wheel and chmod -R 755 (as phase0-kext/Makefile:195-198)"
log "kmutil $KM_SUB --volume-root $MNT --update-all --variant-suffix release"
log "bless --folder $MNT/System/Library/CoreServices --bootefi --create-snapshot"

if [ "$PROBLEMS" -gt 0 ]; then
    log "VERDICT: NOT READY, $PROBLEMS problem(s) above. --apply refuses. Log: $LOG"
    exit 1
fi
if [ "$MODE" = dry-run ]; then
    log "VERDICT: READY. Run it again as root with --apply. Log: $LOG"
    exit 0
fi

mkdir -p "$MNT" || fail "cannot create $MNT"
# UNPROVEN: with authenticated root disabled, mount_apfs gives a writable
# system volume.
run mount -o nobrowse -t apfs "$DEV" "$MNT" || fail "mount failed; nothing changed"
[ -d "$SLE" ] || fail "no $SLE after the mount, so this is the wrong device. Run sudo umount $MNT"
run rm -rf "$SLE/$KEXT_BASE" || fail "could not remove the old copy in $SLE"
run cp -R "$KEXT" "$SLE/" || fail "copy failed. Run sudo umount $MNT"
run chown -R root:wheel "$SLE/$KEXT_BASE" || fail "chown failed"
run chmod -R 755 "$SLE/$KEXT_BASE" || fail "chmod failed"
run kmutil "$KM_SUB" --volume-root "$MNT" --update-all --variant-suffix release ||
    fail "kmutil $KM_SUB failed, so nothing was blessed. The next boot should use the same snapshot as this one. To remove the copy: sudo rm -rf $SLE/$KEXT_BASE; sudo umount $MNT"
# bless(8), --create-snapshot: "Attempts to create an APFS root snapshot of the
# target APFS system volume and set it as root snapshot of the system volume."
# "The target system will boot from this snapshot on its next boot."
run bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot ||
    fail "bless --create-snapshot failed. The next boot should use the old snapshot. Run sudo umount $MNT"
run umount "$MNT" || log "note     : umount $MNT failed; harmless, it goes away at reboot"
log "Done. Reboot, then run the read-only checks in SETUP.md Step 7b. Log: $LOG"
exit 0
