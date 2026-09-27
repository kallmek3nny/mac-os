#!/bin/sh
#
# read-igpu-mmio.sh -- dump bounded DISPLAY-ENGINE register ranges from a live
# Intel iGPU using nothing but /bin/sh, dd, od and awk.
#
#     sudo sh read-igpu-mmio.sh --pci 0000:00:02.0 --out FILE
#
# This is the no-Python twin of read-igpu-mmio.py. Same ranges, same output
# format, same REACH guard. It exists because the interpreter should not be a
# precondition for a capture that is otherwise pure coreutils.
#
# WHY THERE ARE THREE ACCESS PATHS AND NOT ONE
# --------------------------------------------
# The Python version mmap()s BAR0, which is the only universally supported way
# to touch a memory BAR from userspace. A shell has no mmap, so it has to read
# through a file descriptor, and read() on a memory BAR is NOT universally
# supported:
#
#   A. /sys/bus/pci/devices/<addr>/resource0 -- the kernel implements mmap on
#      this file for memory BARs. Plain read() is implemented for I/O BARs and
#      may return EINVAL for memory ones depending on kernel version. Tried
#      first because it needs no extra privilege beyond root and is bounded to
#      this one device.
#
#   B. /dev/mem at the BAR's physical base. CONFIG_STRICT_DEVMEM permits
#      non-RAM (device) ranges, which is exactly what a BAR is. But
#      CONFIG_IO_STRICT_DEVMEM -- enabled on Ubuntu -- refuses any range that
#      a driver has claimed with request_mem_region(), and i915 has claimed
#      this one. So this path is expected to fail while i915 is bound, and is
#      tried anyway because the failure is cheap and the success is free.
#
#   C. intel_reg from intel-gpu-tools, if it happens to be installed. It takes
#      forcewake properly and is the correct tool; it is last only because it
#      is the one thing here that may not be present.
#
# The script reports WHICH path produced the dump. That matters: a value read
# through a working path is evidence, and a file produced by a path that
# silently returned short reads is not.
#
# WHAT A NULL RESULT DOES AND DOES NOT PROVE
# ------------------------------------------
# Identical to the Python version. An all-0xFFFFFFFF range means the read did
# not reach the device. An all-zero range means the same thing often enough to
# be untrustworthy. Neither is evidence that a register "is zero on this
# hardware".
#
# So the REACH line comes first: how many distinct non-trivial 32-bit values
# were seen across everything read. Under 16 and the dump is declared VACUOUS
# and nothing in it may be recorded as an observed constant.

set -u

PCI=0000:00:02.0
OUTFILE=""

while [ $# -gt 0 ]; do
  case "$1" in
    --pci)  PCI="${2:?--pci needs a value}"; shift 2 ;;
    --out)  OUTFILE="${2:?--out needs a value}"; shift 2 ;;
    -h|--help)
      echo "usage: sudo sh read-igpu-mmio.sh [--pci 0000:00:02.0] [--out FILE]"
      exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# The range table. MIRRORED in read-igpu-mmio.py, and
# tests/test_igpu_mmio_ranges.py fails if the two ever disagree -- two copies
# of a register map that drift apart silently is exactly the failure this
# project keeps writing gates against.
#
# Bases are 4 KiB aligned because dd seeks in whole blocks. Labels are
# descriptive only; the authority for any individual offset is
# reference/i915/i915_reg.h.
ranges() {
# BEGIN-RANGES
cat <<'EOF'
0x044000 0x2000 north display engine interrupts / DE misc
0x046000 0x1000 CDCLK, DPLL control
0x060000 0x1000 transcoder A timings
0x061000 0x1000 transcoder B timings
0x062000 0x1000 transcoder C timings
0x06C000 0x1000 DPCLKA / PLL distribution
0x06F000 0x1000 transcoder EDP timings (pre-Gen12 layout)
0x070000 0x1000 pipe A configuration and universal plane 1
0x071000 0x1000 pipe B configuration and universal plane 1
0x072000 0x1000 pipe C configuration and universal plane 1
0x0C0000 0x1000 south display / port control
0x0C7000 0x1000 panel power sequencing
0x0C8000 0x1000 backlight PWM
0x160000 0x2000 combo PHY A/B
0x162000 0x2000 combo PHY C and DDI aux
EOF
# END-RANGES
}

DEV="/sys/bus/pci/devices/$PCI"
RES="$DEV/resource0"

if [ "$(id -u)" != 0 ]; then
  echo "FAIL   : not root. A memory BAR cannot be read without it, and nothing" >&2
  echo "         below would have been read." >&2
  exit 2
fi

if [ ! -d "$DEV" ]; then
  echo "FAIL   : no such PCI device $PCI" >&2
  exit 2
fi

# BAR0 base and length, from sysfs. Line 1 of `resource` is BAR0:
# "<start> <end> <flags>", each already 0x-prefixed.
BAR0=0
BAR0LEN=0
if [ -r "$DEV/resource" ]; then
  # shellcheck disable=SC2034
  read -r R_START R_END R_FLAGS < "$DEV/resource"
  if [ -n "${R_START:-}" ] && [ -n "${R_END:-}" ]; then
    BAR0=$(( R_START ))
    if [ $(( R_END )) -gt "$BAR0" ]; then
      BAR0LEN=$(( R_END - BAR0 + 1 ))
    fi
  fi
fi

TMP=$(mktemp -d) || { echo "FAIL   : mktemp" >&2; exit 2; }
trap 'rm -rf "$TMP"' EXIT INT TERM

VALS="$TMP/vals"
BODY="$TMP/body"
: > "$VALS"
: > "$BODY"

# --- the three access paths ------------------------------------------------

read_sysfs() {  # base len -> raw bytes on stdout
  dd if="$RES" bs=4096 skip=$(( $1 / 4096 )) count=$(( $2 / 4096 )) 2>/dev/null
}

read_devmem() { # base len -> raw bytes on stdout
  [ "$BAR0" != 0 ] || return 1
  dd if=/dev/mem bs=4096 skip=$(( (BAR0 + $1) / 4096 )) count=$(( $2 / 4096 )) 2>/dev/null
}

# Probe a method by reading the first range and demanding the FULL length back.
# A short read is the failure mode that matters here: dd exits 0 having copied
# fewer blocks than asked, so the byte count is the only honest test.
probe() {
  _m=$1
  set -- $(ranges | head -1)
  _base=$(( $1 )); _len=$(( $2 ))
  _got=$("$_m" "$_base" "$_len" | wc -c)
  [ "$_got" = "$_len" ]
}

METHOD=""
METHOD_NOTE=""
if probe read_sysfs; then
  METHOD=read_sysfs
  METHOD_NOTE="sysfs resource0, read() -- bounded to this device"
elif probe read_devmem; then
  METHOD=read_devmem
  METHOD_NOTE="/dev/mem at BAR0 physical base"
fi

# Capture the ACTUAL failure text once, so a negative run is diagnostic rather
# than merely negative.
#
# `2>/dev/null` in the read helpers above is correct for the hot loop -- dd's
# "records in/out" chatter would otherwise flood every range in the artifact --
# but it means a TOTAL failure arrives carrying no errno. EINVAL (read() not
# implemented for this BAR), EPERM/EACCES (STRICT_DEVMEM refusing a claimed
# region), and ENXIO (device gone) are three different problems with three
# different fixes. The 2026-08-15 16:48 UTC run reported REACH 0 and could not
# distinguish them, so the message below guessed. It no longer guesses.
if [ -z "$METHOD" ]; then
  set -- $(ranges | head -1)
  _pb=$(( $1 )); _pl=$(( $2 ))

  dd if="$RES" bs=4096 skip=$(( _pb / 4096 )) count=$(( _pl / 4096 )) \
     of=/dev/null 2>"$TMP/err-sysfs"
  echo "dd exit status: $?" >> "$TMP/err-sysfs"

  if [ "$BAR0" != 0 ]; then
    dd if=/dev/mem bs=4096 skip=$(( (BAR0 + _pb) / 4096 )) count=$(( _pl / 4096 )) \
       of=/dev/null 2>"$TMP/err-devmem"
    echo "dd exit status: $?" >> "$TMP/err-devmem"
  else
    echo "not attempted: BAR0 physical base could not be read from sysfs" \
      > "$TMP/err-devmem"
  fi
fi

# --- intel_reg fallback ----------------------------------------------------
#
# Different shape from the two above: intel_reg dumps by name, not by range,
# so it cannot produce the same output format. It is offered as a SEPARATE
# artifact rather than pretended to be the same dump.
if [ -z "$METHOD" ] && command -v intel_reg >/dev/null 2>&1; then
  {
    echo "=== read-igpu-mmio.sh ==="
    echo "pci    : $PCI"
    echo ""
    echo "REACH  : not applicable -- neither raw path worked, fell back to intel_reg"
    echo "PARTIAL: intel_reg dumps registers BY NAME, not by address range, so"
    echo "         this file is NOT the same artifact as a raw range dump and"
    echo "         the two must not be compared offset for offset. It is still"
    echo "         real register state from the live display engine."
    echo ""
    intel_reg dump 2>&1
  } > "$TMP/intelreg"
  if [ -n "$OUTFILE" ]; then
    cat "$TMP/intelreg" > "$OUTFILE"
    echo "wrote $OUTFILE (intel_reg fallback)" >&2
  else
    cat "$TMP/intelreg"
  fi
  exit 1
fi

if [ -z "$METHOD" ]; then
  {
    echo "=== read-igpu-mmio.sh ==="
    echo "pci    : $PCI"
    echo "bar0   : $(if [ "$BAR0LEN" != 0 ]; then printf '0x%X, %d bytes' "$BAR0" "$BAR0LEN"; else echo UNKNOWN; fi)"
    echo ""
    echo "REACH  : 0 distinct non-trivial 32-bit values across 0 bytes read"
    echo "VACUOUS: no access path worked, so NOTHING was read and this file"
    echo "         contains no register data at all."
    echo ""
    echo "         Tried, in order:"
    echo "           A. read() on $RES"
    echo "              Memory BARs support mmap; read() support is"
    echo "              kernel-version dependent and evidently absent here."
    echo "           B. read() on /dev/mem at the BAR0 base"
    echo "              CONFIG_IO_STRICT_DEVMEM refuses ranges a driver has"
    echo "              claimed, and i915 has claimed this one. Expected."
    echo "           C. intel_reg (intel-gpu-tools) -- not installed."
    echo ""
    echo "         VERBATIM failure text, so the cause is named rather than"
    echo "         guessed at:"
    echo ""
    sed 's/^/           A| /' "$TMP/err-sysfs" 2>/dev/null
    echo ""
    sed 's/^/           B| /' "$TMP/err-devmem" 2>/dev/null
    echo ""
    echo "         What to do, in this order:"
    echo ""
    echo "           1. sudo python3 read-igpu-mmio.py --pci $PCI --out FILE"
    echo "              INSTALLS NOTHING. python3 is part of Ubuntu's base"
    echo "              system -- apt itself depends on it -- so it is already"
    echo "              present. It mmap()s the BAR, which is the only"
    echo "              universally-supported way to reach a memory BAR from"
    echo "              userspace, and is why the .py twin exists at all."
    echo "              A shell cannot mmap; that is a shell limitation, not a"
    echo "              statement about this hardware."
    echo ""
    echo "           2. sudo apt install intel-gpu-tools, then re-run this."
    echo "              Only worth it if step 1 also fails."
  } > "$TMP/empty"
  if [ -n "$OUTFILE" ]; then
    cat "$TMP/empty" > "$OUTFILE"
    echo "wrote $OUTFILE (no access path worked)" >&2
  else
    cat "$TMP/empty"
  fi
  exit 1
fi

# --- the dump --------------------------------------------------------------

TOTAL=0
ranges | while read -r BASEH LENH LABEL; do
  BASE=$(( BASEH ))
  LEN=$(( LENH ))

  if [ "$BAR0LEN" != 0 ] && [ $(( BASE + LEN )) -gt "$BAR0LEN" ]; then
    {
      printf 'SKIP   0x%06X +0x%04X  %s\n' "$BASE" "$LEN" "$LABEL"
      echo   '       (past the end of BAR0 -- not an absence of the register)'
      echo
    } >> "$BODY"
    continue
  fi

  GOT=$("$METHOD" "$BASE" "$LEN" | wc -c)
  if [ "$GOT" != "$LEN" ]; then
    {
      printf 'FAIL   0x%06X +0x%04X  %s\n' "$BASE" "$LEN" "$LABEL"
      printf '       short read: %s of %s bytes. This range was NOT captured;\n' "$GOT" "$LEN"
      echo   '       do not read its absence as a hardware fact.'
      echo
    } >> "$BODY"
    continue
  fi

  printf '=== 0x%06X .. 0x%06X  %s ===\n' "$BASE" $(( BASE + LEN - 1 )) "$LABEL" >> "$BODY"

  # od -tx4 emits little-endian 32-bit words, four per line on x86 -- the same
  # word size and byte order the Python version unpacks with "<I". -v keeps
  # repeated lines instead of collapsing them to "*", which would silently
  # delete real register content.
  "$METHOD" "$BASE" "$LEN" \
    | od -An -tx4 -v \
    | awk -v base="$BASE" -v vals="$VALS" '
        {
          off = base + (NR - 1) * 16
          line = ""
          for (i = 1; i <= NF; i++) {
            line = line (i > 1 ? " " : "") $i
            if ($i != "00000000" && $i != "ffffffff") print $i >> vals
          }
          printf "0x%06x: %s\n", off, line
        }' >> "$BODY"
  echo >> "$BODY"

  echo "$LEN" >> "$TMP/read_bytes"
done

# The `while` above ran in a subshell (it is the right-hand side of a pipe), so
# TOTAL set inside it did not survive. Sum the per-range file instead -- this is
# the same class of bug as $? after a pipeline reporting the wrong process.
if [ -r "$TMP/read_bytes" ]; then
  TOTAL=$(awk '{s += $1} END {print s + 0}' "$TMP/read_bytes")
fi

DISTINCT=$(sort -u "$VALS" 2>/dev/null | wc -l)
DISTINCT=$(( DISTINCT ))

{
  echo "=== read-igpu-mmio.sh ==="
  echo "pci    : $PCI"
  if [ "$BAR0LEN" != 0 ]; then
    printf 'bar0   : 0x%X, %d bytes (0x%X)\n' "$BAR0" "$BAR0LEN" "$BAR0LEN"
  else
    echo "bar0   : UNKNOWN"
  fi
  echo "path   : $METHOD -- $METHOD_NOTE"
  echo ""
  printf 'REACH  : %d distinct non-trivial 32-bit values across %d bytes read\n' \
    "$DISTINCT" "$TOTAL"
  if [ "$DISTINCT" -lt 16 ]; then
    echo "VACUOUS: too few distinct values. The reads did not reach the device,"
    echo "         or the display power well is down. Do NOT record any value"
    echo "         below as an observed hardware constant."
  else
    echo "OK     : the reads reached a live device. Values below are real"
    echo "         register state from a display that is currently driving"
    echo "         the panel."
  fi
  echo ""
  cat "$BODY"
} > "$TMP/final"

if [ -n "$OUTFILE" ]; then
  cat "$TMP/final" > "$OUTFILE"
  echo "wrote $OUTFILE" >&2
else
  cat "$TMP/final"
fi

[ "$DISTINCT" -ge 16 ]
