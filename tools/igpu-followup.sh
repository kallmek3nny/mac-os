#!/bin/sh
#
# igpu-followup.sh -- the second capture, plus a decode of the first.
#
#     sudo sh igpu-followup.sh
#
# TWO JOBS, and they are split on purpose:
#
#   PART A reads the LIVE display engine for ranges the 15-range table in
#   igpu-capture.sh does not reach. This is the perishable half -- it needs
#   Secure Boot off, lockdown [none], i915 bound and the panel lit, which is
#   exactly the state of the 2026-08-15 18:38 UTC window and nothing else.
#
#   PART B decodes the tarball that run ALREADY produced. Not perishable at
#   all -- that file is on disk and can be decoded any time. It runs here only
#   because it costs nothing and turns the terminal output into numbers instead
#   of a REACH line.
#
# WHAT PART A ADDS AND WHY
# ------------------------
# Read out of reference/i915/intel_display_device.c at v6.12 this session:
# XE_D_DISPLAY (the Gen12 entry this silicon uses) sets
#
#   trans_offsets[] = A 0x60000  B 0x61000  C 0x62000  D 0x63000
#   pipe_offsets[]  = A 0x70000  B 0x71000  C 0x72000  D 0x73000
#
# The existing table stops at C. So transcoder D and pipe D were never dumped,
# and their absence from the artifact is a gap in the table rather than a fact
# about the hardware -- the exact confusion this project keeps writing guards
# against. 0x64000 is added for DDI_BUF_CTL and the AUX channels, which is where
# the eDP link actually terminates and which no dumped range covers either.
#
# Note XE_D_DISPLAY does NOT list TRANSCODER_EDP. Gen12 dropped the separate
# eDP transcoder; the panel runs on a normal transcoder through DDI A. So the
# 0x6F000 range in the original table is expected to read as unclaimed, and
# that is a correct null rather than a failed read.
#
# Read-only throughout. Binds nothing, changes no mode.

set -u

PCI="${PCI:-0000:00:02.0}"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="${1:-$HOME/igpu-followup-$STAMP}"

mkdir -p "$OUT" || { echo "cannot create $OUT"; exit 1; }

[ "$(id -u)" = 0 ] || { echo "FAIL: not root. Re-run with sudo."; exit 1; }

echo "=== igpu-followup.sh $(date -u '+%Y-%m-%d %H:%M UTC') ==="
echo "pci    : $PCI"
echo "outdir : $OUT"

LD="unreadable"
[ -r /sys/kernel/security/lockdown ] && LD=$(cat /sys/kernel/security/lockdown)
echo "lockdown: $LD"
case "$LD" in
  *"[none]"*) echo "PASS   : lockdown none -- BAR mmap should work." ;;
  *) echo "WARN   : lockdown is NOT none. Part A will likely EPERM; that is the"
     echo "         kernel refusing, not a hardware fact. Part B still works." ;;
esac

if [ ! -L "/sys/bus/pci/devices/$PCI/driver" ]; then
  echo "FAIL   : no driver bound to $PCI. The display power well is down and"
  echo "         every read below would be noise rather than small numbers."
  exit 1
fi
echo "PASS   : driver = $(basename "$(readlink -f "/sys/bus/pci/devices/$PCI/driver")")"

command -v python3 >/dev/null 2>&1 || {
  echo "FAIL   : python3 absent. A memory BAR is reachable only via mmap and a"
  echo "         shell cannot mmap, so there is no fallback here."
  exit 1
}

# ---------------------------------------------------------------------------
echo ""
echo "=== PART A: the ranges the original table missed ==="

PCI="$PCI" OUTF="$OUT/09-gap-mmio.txt" python3 - <<'PY'
import mmap, os, struct, sys

PCI, OUTF = os.environ["PCI"], os.environ["OUTF"]

# NOT a copy of the 15-range table -- these are the ranges it does not reach.
# tests/test_igpu_mmio_ranges.py deliberately does not compare against this
# list, because a gap table that had to match the main table would be empty.
RANGES = [
    (0x063000, 0x1000, "transcoder D timings (XE_D trans_offsets[TRANSCODER_D])"),
    (0x064000, 0x1000, "DDI_BUF_CTL + AUX channels -- where eDP link terminates"),
    (0x073000, 0x1000, "pipe D config + universal plane 1"),
]

path = "/sys/bus/pci/devices/%s/resource0" % PCI
body, distinct, total, eperm = [], set(), 0, 0

try:
    fd = os.open(path, os.O_RDONLY)
except OSError as exc:
    open(OUTF, "w").write("FAIL: cannot open %s: %s\n" % (path, exc))
    print("FAIL   : cannot open %s: %s" % (path, exc))
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
        body.append("=== 0x%06X .. 0x%06X  %s ===" % (base, base + length - 1, label))
        # Word at a time. A bulk slice is a memcpy and glibc may satisfy it with
        # vector loads; MMIO needs naturally aligned 32-bit accesses or a read
        # can return a word that never existed on the bus.
        try:
            words = [struct.unpack("<I", mm[o:o + 4])[0] for o in range(0, length, 4)]
        finally:
            mm.close()
        total += length
        for i in range(0, len(words), 4):
            row = words[i:i + 4]
            for w in row:
                if w not in (0x00000000, 0xFFFFFFFF):
                    distinct.add(w)
            body.append("0x%06X: %s" % (base + i * 4, " ".join("%08x" % w for w in row)))
        body.append("")
finally:
    os.close(fd)

head = ["=== igpu-followup.sh gap dump ===",
        "pci    : %s" % PCI,
        "source : %s" % path,
        "",
        "REACH  : %d distinct non-trivial 32-bit values across %d bytes read"
        % (len(distinct), total)]
if eperm:
    head += ["BLOCKED: %d of %d ranges returned EPERM -- kernel lockdown, not a"
             % (eperm, len(RANGES)),
             "         hardware or permissions fact. Record nothing from here."]
elif len(distinct) >= 8:
    head += ["OK     : the mapping reached a live device."]
else:
    head += ["SPARSE : few distinct values. For transcoder/pipe D that is the",
             "         EXPECTED reading -- this laptop drives one eDP panel, so D",
             "         is unclaimed. Unclaimed is a real finding; a failed read is",
             "         not. The FAIL lines above, if any, distinguish them."]
open(OUTF, "w").write("\n".join(head + [""] + body) + "\n")

# Terminal gets the summary plus the handful of words that matter, so the useful
# part survives even if the file never leaves the machine.
print("\n".join(head[4:]))
print("")
for line in body:
    if line.startswith("FAIL") or line.startswith("=== 0x"):
        print("  " + line)
    elif line.startswith("0x06300") or line.startswith("0x07300") \
            or line.startswith("0x064000") or line.startswith("0x064004"):
        print("  " + line)
sys.exit(0)
PY
echo "partA exit=$?"

# ---------------------------------------------------------------------------
echo ""
echo "=== PART B: decode of the capture already on disk ==="

TAR=""
for CAND in $(ls -t /root/igpu-*.tar.gz "$HOME"/igpu-*.tar.gz 2>/dev/null); do
  [ -f "$CAND" ] || continue
  TAR="$CAND"
  break
done

if [ -z "$TAR" ]; then
  echo "SKIP   : no igpu-*.tar.gz found in /root or \$HOME."
else
  TARBALL="$TAR" python3 - <<'PY'
import os, re, sys, tarfile

TARBALL = os.environ["TARBALL"]

# Transcoder addressing is a per-platform TABLE, not arithmetic. TRANSCONF is
# the trap: i915_reg.h:1698 defines it as _MMIO_PIPE2, so it sits in the PIPE
# block (0x70008 + pipe delta) while being indexed by the TRANSCODER. A single
# translate(base, offset) helper would place it at 0x60008 -- which is
# _TRANS_HSYNC_A, a real register that reads and writes without faulting.
TRANS = [("A", 0x60000, 0x70008), ("B", 0x61000, 0x71008), ("C", 0x62000, 0x72008)]
FIELDS = (("HTOTAL", 0x00), ("HBLANK", 0x04), ("HSYNC", 0x08),
          ("VTOTAL", 0x0C), ("VBLANK", 0x10), ("VSYNC", 0x14))
PIPESRC = 0x1C


def plus_one_pair(v):
    # intel_display.c:2741 writes HACTIVE(hdisplay-1) | HTOTAL(htotal-1) and the
    # readback at 2911 adds 1 back, so BOTH halves are stored as value-1.
    # Omitting the +1 yields numbers off by exactly one -- a mode still nearly
    # works under that error, which is what makes it worth pinning.
    return ((v >> 16) & 0xFFFF) + 1, (v & 0xFFFF) + 1


try:
    tf = tarfile.open(TARBALL, "r:gz")
except Exception as exc:
    print("FAIL   : cannot open %s: %s" % (TARBALL, exc))
    sys.exit(1)

mem = {os.path.basename(m.name): m for m in tf.getmembers() if m.isfile()}


def read_member(n):
    m = mem.get(n)
    if m is None:
        return None
    f = tf.extractfile(m)
    return f.read().decode("utf-8", "replace") if f else None


mmio = read_member("03-mmio.txt")
if mmio is None:
    print("FAIL   : 03-mmio.txt not in %s" % TARBALL)
    print("         members: %s" % ", ".join(sorted(mem)))
    sys.exit(1)

words, verdict, reach = {}, "UNKNOWN", ""
for line in mmio.splitlines():
    if line.startswith("REACH"):
        reach = line.strip()
    if line.startswith(("OK ", "VACUOUS", "BLOCKED")):
        verdict = line.split(":")[0].strip()
    m = re.match(r"^0x([0-9a-fA-F]{6}):\s+(.*)$", line)
    if m:
        base = int(m.group(1), 16)
        for i, tok in enumerate(m.group(2).split()):
            if re.fullmatch(r"[0-9a-fA-F]{8}", tok):
                words[base + i * 4] = int(tok, 16)

print("tarball: %s" % TARBALL)
print("%s" % reach)
print("verdict: %s   (%d words parsed)" % (verdict, len(words)))
if verdict != "OK":
    print("STOP   : dump did not reach a live device. Record nothing below.")
print("")
print("Raw word first, decode second. The WORD is the observation; the decode")
print("is this script's reading of it. If they disagree with i915 below, the")
print("word wins and the decode is the bug.")
print("")

any_live = False
for name, tbase, conf_off in TRANS:
    conf = words.get(conf_off)
    if conf is None:
        print("transcoder %s: TRANSCONF 0x%05X not in dump" % (name, conf_off))
        continue
    en, st = bool(conf & (1 << 31)), bool(conf & (1 << 30))
    print("transcoder %s  TRANSCONF 0x%05X = 0x%08X  enable=%d state=%d"
          % (name, conf_off, conf, en, st))
    if not en:
        print("  disabled -- timing words below are stale, not current state.")
    for label, off in FIELDS:
        v = words.get(tbase + off)
        if v is None:
            print("  %-6s 0x%05X   NOT IN DUMP" % (label, tbase + off))
            continue
        hi, lo = plus_one_pair(v)
        print("  %-6s 0x%05X = 0x%08X  -> hi=%-5d lo=%-5d"
              % (label, tbase + off, v, hi, lo))
    src = words.get(tbase + PIPESRC)
    if src is None:
        print("  PIPESRC 0x%05X   NOT IN DUMP" % (tbase + PIPESRC))
    else:
        w, h = plus_one_pair(src)
        print("  PIPESRC 0x%05X = 0x%08X  -> %dx%d" % (tbase + PIPESRC, src, w, h))
    if en:
        any_live = True
        ht, vt = words.get(tbase + 0x00), words.get(tbase + 0x0C)
        if ht is not None and vt is not None:
            htotal, hdisp = plus_one_pair(ht)
            vtotal, vdisp = plus_one_pair(vt)
            print("  ACTIVE MODE: %dx%d, blanked %dx%d"
                  % (hdisp, vdisp, htotal, vtotal))
    print("")

if not any_live:
    print("NOTE   : no transcoder reported enable=1, yet the panel was lit during")
    print("         the capture. That points at a wrong TRANSCONF offset in THIS")
    print("         script rather than a dark display. i915's decode below is the")
    print("         arbiter.")
    print("")

cd = words.get(0x46000)
if cd is not None:
    print("CDCLK_CTL 0x46000 = 0x%08X  (Gen12 divider encoding NOT verified" % cd)
    print("          this session, so no frequency is claimed from it)")
    print("")

print("--- i915's own decode, the independent path ---")
info = read_member("04-i915_display_info.txt")
if info is None:
    print("  not in the tarball")
else:
    kept = [l.strip() for l in info.splitlines()
            if l.strip() and re.search(
                r"CRTC|pipe |active|mode:|encoder|connector|[0-9]{3,}x[0-9]{3,}"
                r"|dotclock|bpp|hw:", l, re.I)]
    if not kept:
        kept = [l for l in info.splitlines() if l.strip()][:25]
    for s in kept[:40]:
        print("  %s" % s)
PY
  echo "partB exit=$?"
fi

# ---------------------------------------------------------------------------
TARGZ="$OUT.tar.gz"
if tar czf "$TARGZ" -C "$(dirname "$OUT")" "$(basename "$OUT")" 2>/dev/null; then
  echo ""
  echo "=== done ==="
  echo "gap dump saved: $TARGZ ($(wc -c < "$TARGZ") bytes)"
  echo "Everything above is already paste-sized. The tarball is only needed if"
  echo "the full per-word gap dump turns out to matter."
else
  echo ""
  echo "=== done, tar failed -- directory is $OUT ==="
fi
