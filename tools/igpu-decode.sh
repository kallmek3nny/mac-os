#!/bin/sh
#
# igpu-decode.sh -- decode an igpu-capture.sh tarball into a paste-sized summary.
#
#     sudo sh igpu-decode.sh
#
# Reads NOTHING from the hardware. It opens the newest /root/igpu-*.tar.gz that
# igpu-capture.sh already produced and prints ~50 lines that can be pasted into
# a chat window, so a 48 KB artifact does not have to be transported off the
# machine to be useful.
#
# WHY THIS EXISTS
# ---------------
# The 2026-08-15 18:38 UTC run succeeded -- Secure Boot was disabled, lockdown
# read [none], and the BAR mmap returned 269 distinct non-trivial values across
# 73728 bytes. That capture is real register state from a display that was
# driving the panel at the time. But the summary printed to the terminal carries
# only the REACH line; every actual register value lives in 03-mmio.txt inside
# the tarball. This closes that gap without a second hardware run, which matters
# because the capture window was 15 minutes.
#
# EVERY DECODED FIELD BELOW IS DERIVED, NOT WITNESSED.
# The raw 32-bit word is printed beside each decode for exactly that reason. The
# WORD is the observation; the decode is this script's reading of it. If the two
# ever disagree with i915's own decoded view (printed at the end from
# 04-i915_display_info.txt), the word wins and the decode is the bug.

set -u

TARBALL="${1:-}"

if [ -z "$TARBALL" ]; then
  # Newest first. `ls -t` on a glob that matches nothing yields the literal
  # pattern, so the -f test below is what rejects it rather than an empty string.
  for CAND in $(ls -t /root/igpu-*.tar.gz "$HOME"/igpu-*.tar.gz 2>/dev/null); do
    [ -f "$CAND" ] || continue
    TARBALL="$CAND"
    break
  done
fi

if [ -z "$TARBALL" ] || [ ! -f "$TARBALL" ]; then
  echo "FAIL   : no igpu-*.tar.gz found in /root or \$HOME."
  echo "         Run igpu-capture.sh first, or pass the path:"
  echo "           sudo sh igpu-decode.sh /path/to/igpu-TIMESTAMP.tar.gz"
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "FAIL   : python3 absent. It was present for the capture (the embedded"
  echo "         MMIO dumper is Python and it ran), so this is unexpected."
  exit 1
fi

TARBALL="$TARBALL" python3 - <<'PY'
import os, re, sys, tarfile

TARBALL = os.environ["TARBALL"]

# ---------------------------------------------------------------------------
# The register map. Every offset here was read out of the vendored v6.12 headers
# in reference/i915/ during this session, and the SOURCE column names the file
# and the fact that settled it. Nothing is inferred from a stride.
#
# Transcoder addressing on Gen12 is a per-platform TABLE, not arithmetic:
# intel_display_device.c's XE_D_DISPLAY sets trans_offsets[] to
# A=0x60000 B=0x61000 C=0x62000 D=0x63000 DSI0=0x6b000 DSI1=0x6b800.
# The uniform 0x1000 spacing for A..D is a property of that table, not a rule.
#
# TRANSCONF is the trap. i915_reg.h:1698 defines it as _MMIO_PIPE2, so it lives
# in the PIPE block (0x70008 + pipe delta) while being indexed by the
# TRANSCODER. A single translate(base, offset) helper would put it in the
# transcoder block at 0x60008 -- which is _TRANS_HSYNC_A, a real register that
# reads and writes without faulting.
TRANS = [
    ("A", 0x60000, 0x70008),
    ("B", 0x61000, 0x71008),
    ("C", 0x62000, 0x72008),
]

# Offsets within a transcoder block. i915_reg.h:1073-1108.
HTOTAL, HBLANK, HSYNC = 0x00, 0x04, 0x08
VTOTAL, VBLANK, VSYNC = 0x0C, 0x10, 0x14
PIPESRC = 0x1C

CDCLK_CTL = 0x46000


def load_words(text):
    """Parse 03-mmio.txt's rows into {absolute_offset: word}.

    Rows look like:  0x060000: 083f077f 07ff0837 ...
    Four words per row, so word N sits at row_base + N*4.
    """
    words = {}
    for line in text.splitlines():
        m = re.match(r"^0x([0-9a-fA-F]{6}):\s+(.*)$", line)
        if not m:
            continue
        base = int(m.group(1), 16)
        for i, tok in enumerate(m.group(2).split()):
            if re.fullmatch(r"[0-9a-fA-F]{8}", tok):
                words[base + i * 4] = int(tok, 16)
    return words


def plus_one_pair(v):
    """Decode a Gen12 timing register.

    intel_display.c:2741-2743 writes HACTIVE(hdisplay - 1) | HTOTAL(htotal - 1),
    and the readback at 2911 adds 1 back. So BOTH halves are stored as value-1
    and neither is a raw count. Reading these without the +1 yields numbers that
    look almost right -- off by exactly one -- which is the kind of error a mode
    still nearly works under.
    """
    return ((v >> 16) & 0xFFFF) + 1, (v & 0xFFFF) + 1


try:
    tf = tarfile.open(TARBALL, "r:gz")
except Exception as exc:
    print("FAIL   : cannot open %s: %s" % (TARBALL, exc))
    sys.exit(1)

members = {os.path.basename(m.name): m for m in tf.getmembers() if m.isfile()}


def read_member(name):
    m = members.get(name)
    if m is None:
        return None
    f = tf.extractfile(m)
    return f.read().decode("utf-8", "replace") if f else None


mmio = read_member("03-mmio.txt")
if mmio is None:
    print("FAIL   : 03-mmio.txt is not in %s" % TARBALL)
    print("         members: %s" % ", ".join(sorted(members)))
    sys.exit(1)

verdict = "UNKNOWN"
reach = ""
for line in mmio.splitlines():
    if line.startswith("REACH"):
        reach = line.strip()
    if line.startswith(("OK ", "VACUOUS", "BLOCKED")):
        verdict = line.split(":")[0].strip()

words = load_words(mmio)

print("=== igpu-decode.sh ===")
print("tarball: %s" % TARBALL)
print("%s" % reach)
print("verdict: %s   (parsed %d words from 03-mmio.txt)" % (verdict, len(words)))
if verdict != "OK":
    print("")
    print("STOP   : the dump did not reach a live device. Nothing below may be")
    print("         recorded as an observed hardware constant.")
print("")

# ---------------------------------------------------------------------------
print("--- transcoder timings (raw word, then this script's decode) ---")
print("Both halves of every timing register are stored as value-1.")
print("")

any_live = False
for name, tbase, conf_off in TRANS:
    conf = words.get(conf_off)
    if conf is None:
        print("transcoder %s: TRANSCONF 0x%05X not in the dump" % (name, conf_off))
        print("")
        continue

    enabled = bool(conf & (1 << 31))
    state = bool(conf & (1 << 30))
    print("transcoder %s  TRANSCONF 0x%05X = 0x%08X  enable=%d state=%d"
          % (name, conf_off, conf, enabled, state))

    if not enabled:
        print("  disabled -- the timing words below are stale, not current state.")

    for label, off in (("HTOTAL", HTOTAL), ("HBLANK", HBLANK), ("HSYNC", HSYNC),
                       ("VTOTAL", VTOTAL), ("VBLANK", VBLANK), ("VSYNC", VSYNC)):
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
        print("  PIPESRC 0x%05X = 0x%08X  -> %dx%d"
              % (tbase + PIPESRC, src, w, h))

    if enabled:
        any_live = True
        ht = words.get(tbase + HTOTAL)
        vt = words.get(tbase + VTOTAL)
        if ht is not None and vt is not None:
            htotal, hdisplay = plus_one_pair(ht)
            vtotal, vdisplay = plus_one_pair(vt)
            print("  ACTIVE MODE: %dx%d, blanked %dx%d"
                  % (hdisplay, vdisplay, htotal, vtotal))
    print("")

if not any_live:
    print("NOTE   : no transcoder reported enable=1. The panel was lit during the")
    print("         capture, so this is more likely a wrong TRANSCONF offset in")
    print("         THIS script than a dark display. Check against i915's own")
    print("         decode below before believing it.")
    print("")

cd = words.get(CDCLK_CTL)
if cd is not None:
    print("CDCLK_CTL 0x%05X = 0x%08X  (freq field decode not attempted -- the"
          % (CDCLK_CTL, cd))
    print("          Gen12 divider encoding was not verified this session)")
    print("")

# ---------------------------------------------------------------------------
# Ranges the capture is missing. Recorded here rather than left silent, because
# "not in the dump" and "zero on the hardware" are indistinguishable downstream.
print("--- known gaps in the range table ---")
print("transcoder D (0x63000) is NOT dumped. XE_D_DISPLAY's trans_offsets[]")
print("includes TRANSCODER_D, and Gen12 pipe_mask covers PIPE_D, so a fourth")
print("transcoder exists on this silicon and the 15-range table does not reach")
print("it. Absence of transcoder D below is this table's gap, not the hardware.")
print("The dumped 0x6F000 range is TRANSCODER_EDP, which XE_D_DISPLAY does NOT")
print("list -- Gen12 dropped the separate EDP transcoder. Expect it to read as")
print("unclaimed rather than as a configured display.")
print("")

# ---------------------------------------------------------------------------
# The independent path. i915 decoded the same silicon itself; two paths agreeing
# is what upgrades a decode from plausible to confirmed.
print("--- i915's own decode (04-i915_display_info.txt) ---")
info = read_member("04-i915_display_info.txt")
if info is None:
    print("not in the tarball")
else:
    kept = []
    for line in info.splitlines():
        s = line.strip()
        if not s:
            continue
        if re.search(r"CRTC|pipe |active|mode:|encoder|connector|"
                     r"[0-9]{3,}x[0-9]{3,}|dotclock|bpp|hw:", s, re.I):
            kept.append(s)
    if not kept:
        print("no lines matched the filter -- printing the first 25 raw")
        kept = [l for l in info.splitlines() if l.strip()][:25]
    for s in kept[:45]:
        print("  %s" % s)

print("")
modes = read_member("05-card1-eDP-1-modes.txt")
if modes:
    print("--- eDP-1 mode list ---")
    for s in [l.strip() for l in modes.splitlines() if l.strip()][:10]:
        print("  %s" % s)
PY
