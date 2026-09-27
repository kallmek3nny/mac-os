#!/bin/sh
#
# guest-build-kext.sh -- run STEP 7 inside the macOS guest.
#
# Why this exists
# ---------------
# The kext has never been compiled. That needs Apple's clang with `-mkernel
# -fapple-kext` and the Kernel framework headers, which exist only on macOS.
# Every `vmrun` guest operation is blocked on the account password
# (OPERATOR-ACTION-guest-creds.md), so the host cannot *invoke* anything in the
# guest -- but a CD needs no credential, and a human sitting at the guest
# console can run one command.
#
# So this script is the guest half of the delivery: it is written on the host,
# shipped on the payload ISO by tools/deliver-via-cd.py, and run from the guest
# console. It does the build and it writes a log the host can read back.
#
# What it is NOT
# --------------
# It does not load the kext. `kextload` on Apple Silicon or on a T2 Mac needs
# SIP lowered and a reboot, and this VM is not the target laptop anyway. The
# claim this script can support is exactly one: *does the source compile with
# Apple clang*. That is STEP 7's first half and it is the gate every host-side
# test explicitly does not cover.
#
# Usage, from the guest console:
#
#     sh /Volumes/PAYLOAD/guest-build-kext.sh
#
# It is deliberately POSIX sh, not bash: the guest's default shell is zsh and
# /bin/sh is the one interpreter guaranteed present on a bare Sonoma install.

set -u

PAYLOAD_TAR_NAME="kext-src.tar"
LOG_NAME="step7-build.log"

# Both of these are overridable so the script's FAILURE paths can be exercised
# on the host, where /Volumes does not exist and there is no Apple clang. A
# script that can only be run inside the guest can only be tested by the one
# person who cannot report back -- see tests/test_guest_build_kext.py.
# Defaults are the real guest paths; nothing in the guest sets these.
PAYLOAD_ROOT="${NV_PAYLOAD_ROOT:-/Volumes}"
WORK_DIR="${NV_WORK_DIR:-$HOME/nv3050-step7}"

# ---------------------------------------------------------------------------
# Command-line overrides.
#
# Until 2026-08-14 this script had NO argument parser at all. It read the two
# NV_* variables and nothing else, so `--payload-root /tmp/x` was accepted in
# total silence and the run went on searching the default root anyway. That is
# this project's signature defect shape: an option that appears to be honoured,
# reports success, and describes a different run than the one you asked for. It
# was found the only way it could be -- by passing the flag and watching the
# refusal name the root the caller had just overridden.
#
# So an unknown argument is a REFUSAL, not a warning. A warning would scroll off
# the guest console above several screens of build output, and the operator would
# read a verdict produced by a run that ignored them.
#
# Precedence is CLI > environment > default. That order is deliberate: it leaves
# tests/test_guest_build_kext.py driving the script through NV_* exactly as
# before, while a human at the console can still override on one line.
#
# Usage errors exit 2, never 1. Every existing refusal in this script exits 1,
# and "you asked wrongly" is a different fact from "the payload is not there" --
# one code covering both cannot be acted on remotely.
# ---------------------------------------------------------------------------
usage() {
    cat <<'USAGE'
Usage: sh guest-build-kext.sh [--payload-root DIR] [--work-dir DIR]

  --payload-root DIR   directory the mounted volumes appear under
                       (default: the guest's volume directory; the
                       NV_PAYLOAD_ROOT environment variable sets it too)
  --work-dir DIR       directory to unpack and build in
                       (default: nv3050-step7 under your home directory;
                       NV_WORK_DIR sets it too)
  -h, --help           print this and exit 0

A command-line value wins over the environment variable, which wins over the
default. Unknown arguments are refused with exit 2 rather than ignored.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --payload-root)
            if [ $# -lt 2 ]; then
                echo "FAIL: --payload-root needs a directory"
                exit 2
            fi
            PAYLOAD_ROOT="$2"
            shift 2
            ;;
        --payload-root=*)
            PAYLOAD_ROOT="${1#*=}"
            shift
            ;;
        --work-dir)
            if [ $# -lt 2 ]; then
                echo "FAIL: --work-dir needs a directory"
                exit 2
            fi
            WORK_DIR="$2"
            shift 2
            ;;
        --work-dir=*)
            WORK_DIR="${1#*=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "FAIL: unknown argument: $1"
            echo "      Refusing rather than ignoring it. An ignored flag would"
            echo "      print a verdict for a run you did not ask for."
            echo
            usage
            exit 2
            ;;
    esac
done

# An empty value is a caller mistake that would otherwise turn into a glob over
# the filesystem root, or an unpack into the current directory.
if [ -z "$PAYLOAD_ROOT" ]; then
    echo "FAIL: --payload-root was given an empty value"
    exit 2
fi
if [ -z "$WORK_DIR" ]; then
    echo "FAIL: --work-dir was given an empty value"
    exit 2
fi

# ---------------------------------------------------------------------------
# Find the payload. The mount point is the volume identifier, but do not assume
# it: if the ISO was built with a different --vol-ident, or if macOS appended a
# disambiguating suffix because a stale PAYLOAD volume is still mounted, the
# path differs. Search rather than hardcode -- a wrong hardcoded path fails with
# "no such file", which reads as a missing payload rather than a wrong guess.
# ---------------------------------------------------------------------------
find_payload() {
    for candidate in "$PAYLOAD_ROOT"/*/"$PAYLOAD_TAR_NAME"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

SRC_TAR="$(find_payload)" || {
    echo "FAIL: no $PAYLOAD_TAR_NAME under $PAYLOAD_ROOT/*/"
    echo "      Mounted volumes:"
    ls -1 "$PAYLOAD_ROOT"/ 2>&1 | sed 's/^/        /'
    echo
    echo "      The CD is emulated by the host VMware process. If the ISO was"
    echo "      repointed while this VM was running, the change takes effect on"
    echo "      the next power cycle or a manual reconnect from VM settings."
    exit 1
}

echo "payload: $SRC_TAR"

# ---------------------------------------------------------------------------
# Unpack to the home directory, NOT to the volume. A mounted ISO is read-only,
# so `make` would fail on its first write with a permission error that describes
# the medium rather than the code.
# ---------------------------------------------------------------------------
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR" || { echo "FAIL: cannot create $WORK_DIR"; exit 1; }

if ! tar -xf "$SRC_TAR" -C "$WORK_DIR"; then
    echo "FAIL: tar could not unpack $SRC_TAR"
    exit 1
fi

KEXT_DIR="$WORK_DIR/phase0-kext"
if [ ! -f "$KEXT_DIR/Makefile" ]; then
    echo "FAIL: no Makefile at $KEXT_DIR"
    echo "      unpacked tree:"
    find "$WORK_DIR" -maxdepth 2 2>&1 | sed 's/^/        /'
    exit 1
fi

LOG="$WORK_DIR/$LOG_NAME"

# ---------------------------------------------------------------------------
# Record the toolchain BEFORE building. A build failure whose log does not say
# which clang produced it cannot be acted on from the host, and "the guest has no
# developer tools at all" is the single most likely reason a first `-fapple-kext`
# build fails on a fresh install.
#
# 2026-08-14: this comment used to say the likely cause was "xcode-select points
# at the CLT rather than a full Xcode". That was wrong, and it was wrong in the
# direction that wastes the operator's time -- it would have sent them to the App
# Store for a multi-gigabyte download they did not need. MEASURED in the guest
# after the successful build: `/Applications/Xcode.app` does not exist,
# `xcode-select -p` is `/Library/Developer/CommandLineTools`, and
# `$(xcrun --show-sdk-path)/System/Library/Frameworks/Kernel.framework` is
# present. The Command Line Tools SDK carries Kernel.framework on its own. What
# the first attempt actually hit was NO toolchain whatsoever: xcode-select
# errored, and `xcode-select --install` alone fixed it.
# ---------------------------------------------------------------------------
{
    echo "=== STEP 7 build attempt ==="
    echo "date          : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "uname         : $(uname -a)"
    echo "sw_vers       : $(sw_vers 2>&1 | tr '\n' ' ')"
    echo "xcode-select  : $(xcode-select -p 2>&1)"
    echo "clang         : $(clang --version 2>&1 | head -1)"
    echo
    echo "--- Kernel.framework presence ---"
    SDK="$(xcrun --show-sdk-path 2>&1)"
    echo "sdk path      : $SDK"
    if [ -d "$SDK/System/Library/Frameworks/Kernel.framework" ]; then
        echo "Kernel.framework: PRESENT"
    else
        echo "Kernel.framework: ABSENT -- a kext cannot build without it."
        echo "                  Do NOT reach for the App Store. Measured in this"
        echo "                  guest 2026-08-14: the Command Line Tools SDK"
        echo "                  carries Kernel.framework on its own, with no"
        echo "                  Xcode.app installed at all. ABSENT here means"
        echo "                  either no developer tools are installed --"
        echo "                  run: xcode-select --install -- or xcode-select"
        echo "                  points somewhere that has none."
    fi
    echo
} > "$LOG" 2>&1

sed -n '1,$p' "$LOG"

# ---------------------------------------------------------------------------
# The build itself. Both streams into the log AND to the console, so the person
# at the guest console sees it and the host can read the same bytes back later.
# ---------------------------------------------------------------------------
echo "--- make ---" >> "$LOG"

# `$?` after a PIPELINE is the status of the LAST command in it -- `tee` -- which
# succeeds whenever it can write the log. Reading it here would report "make exit
# status: 0" and `exit 0` for a build that failed to compile, which is the worst
# possible lie for a script that gets ONE run in front of an operator who has to
# read the outcome off a screen. POSIX sh has no `pipefail`, so carry make's own
# status out of the subshell through a file.
MAKE_STATUS_FILE="$WORK_DIR/.make-status"
rm -f "$MAKE_STATUS_FILE"
( cd "$KEXT_DIR" && make 2>&1; echo $? > "$MAKE_STATUS_FILE" ) | tee -a "$LOG"
MAKE_STATUS=$(cat "$MAKE_STATUS_FILE" 2>/dev/null)
rm -f "$MAKE_STATUS_FILE"
# An empty file means the subshell died before recording anything (killed, disk
# full). Unknown is a failure, never a success.
if [ -z "$MAKE_STATUS" ]; then
    MAKE_STATUS=1
    echo "make exit status could not be recovered -- treating as failure" >> "$LOG"
fi

{
    echo
    echo "--- result ---"
    echo "make exit status: $MAKE_STATUS"
    if [ -d "$KEXT_DIR/build/NV3050Probe.kext" ]; then
        echo "bundle: PRESENT"
        find "$KEXT_DIR/build/NV3050Probe.kext" -type f | sed 's/^/  /'
        BIN="$KEXT_DIR/build/NV3050Probe.kext/Contents/MacOS/NV3050Probe"
        if [ -f "$BIN" ]; then
            echo
            echo "--- binary ---"
            file "$BIN" 2>&1
            echo
            echo "--- IOKit symbols (proves the shell linked, not just compiled) ---"
            nm -gU "$BIN" 2>&1 | grep -i -E 'IOService|OSMetaClass' | head -20
        fi
    else
        echo "bundle: ABSENT -- the build did not get as far as a bundle."
    fi
    echo
    echo "=== end ==="
} >> "$LOG" 2>&1

tail -n 40 "$LOG"

echo
echo "Full log: $LOG"
echo
# 2026-08-14: this block used to say "there is no credential-free guest->host
# file path -- that direction needs the password". That was true when the only
# host->guest channel was a CD. It is false now: the host's public key is in
# ~/.ssh/authorized_keys (OPERATOR-ACTION-guest-key.md), so the host pulls this
# log itself with scp over the 127.0.0.1:2222 NAT forward, no password and no
# human. The claim was corrected only after the pull was actually MEASURED --
# step7-results/step7-build.log came back byte-for-byte, 1582 bytes.
echo "The host retrieves this itself:"
echo "    scp -P 2222 kenny@127.0.0.1:$LOG step7-results/"
echo "No password is needed for that -- see OPERATOR-ACTION-guest-key.md. If the"
echo "key is not installed, the log is small enough to read off the screen."

exit "$MAKE_STATUS"
