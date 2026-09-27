#!/usr/bin/env python3
r"""find-display-matches.py -- every PCI device id a macOS graphics driver binds to.

Why this exists
---------------
`PLAN.md` termination condition B says macOS ships no Tiger Lake framebuffer
driver, therefore the iGPU cannot scan out. The measured half of that is solid:
`intel-kext-inventory.txt` holds 430 kext paths and 491 bundle identifiers and
Tiger Lake / Gen12 / Iris Xe is zero across all of them.

But the INFERENCE from that measurement has a premise nobody in this tree had
checked: that scanout requires a per-generation framebuffer driver at all. Two
things could have falsified it --

  1. a generic framebuffer that matches any PCI display-class device, or
  2. class-only matching (`IOPCIClassMatch`) in the drivers that DO ship,
     which a Gen12 part would satisfy.

This tool settles (2) by reading the match dictionaries themselves out of the
prelinked kernel's `__PRELINK_INFO` XML. (1) was settled separately: the image
does carry `IOBootFramebuffer`, an `IOFramebuffer` subclass, but it has NO IOKit
personality -- 108 occurrences of the class name and zero within 4000 bytes of
an `IOProviderClass` key, measured against controls that prove IOGraphicsFamily
personalities are visible to the same scan.

What a match dictionary looks like in this image
------------------------------------------------
    <key>IOClass</key>          <string>AppleIntelFramebufferController</string>
    <key>IOPCIClassMatch</key>  <string>0x03000000&amp;0xff000000</string>
    <key>IOPCIPrimaryMatch</key><string>0x3E9B8086 0x3EA58086 ...</string>
    <key>IOProviderClass</key>  <string>IOPCIDevice</string>

`IOPCIClassMatch` alone would be generic -- `0x03000000&0xff000000` is "any
display controller". `IOPCIPrimaryMatch` narrows it to an explicit allow-list of
`<device><vendor>` words, and IOKit requires BOTH when both are present. So the
allow-list is the gate, and the question is whether the laptop's id is in one.

Absence discipline
------------------
A device id that does not appear here is an absence, and an absence is the one
finding a scan cannot report honestly on its own evidence -- a wrong region, a
compressed plist, or a mis-parsed key all print the identical zero. So the
report carries positive controls: ids that are INDEPENDENTLY known to be in
this image (Coffee Lake, Ice Lake-LP) must be found by the same parse that
reports the target id missing. If a control is missing the parse is blind and
every zero below it proves nothing.

The listing is UNFILTERED on purpose. A committed listing whose header states a
reach its body does not show turns every later grep for an absent thing into a
false zero, and this tree has already paid for that once.

Usage
-----
    python tools/find-display-matches.py [image] > display-match-report.txt
"""

import os
import re
import sys

DEFAULT_IMAGE = r"C:\Users\Kentl\macos-vm\sonoma\BaseSystem.img"

CHUNK = 64 * 1024 * 1024
OVERLAP = 1 << 16

# A personality dict in these plists is well under 4 KiB, and the keys are
# emitted in alphabetical order, so IOClass precedes IOPCIPrimaryMatch and
# IOPersonalityPublisher follows it. That ordering is what makes the nearest
# preceding / nearest following lookup below reliable rather than a guess.
BACK = 2600
FWD = 2600

MATCH_KEYS = [
    "IOPCIPrimaryMatch",
    "IOPCIMatch",
    "IOPCISecondaryMatch",
]

# The laptop under test. 0x9a68 is the Tiger Lake GT2 iGPU device id, measured
# on the machine itself -- see igpu-results/ and intel-handoff28 section 3b,
# where it is the above-divider anchor for the capture artifacts.
TARGET_ID = "0x9A688086"
TARGET_NOTE = "Tiger Lake GT2 (the laptop's iGPU, device 0x9a68)"

# Ids independently known to be present, from the framebuffer kexts the
# inventory already lists. If the parse cannot find these it cannot find
# anything and the target's absence is vacuous.
CONTROL_IDS = [
    ("0x3E9B8086", "Coffee Lake GT2 (AppleIntelCFLGraphicsFramebuffer)"),
    ("0x8A528086", "Ice Lake-LP GT2 (AppleIntelICLLPGraphicsFramebuffer)"),
]

KEY_STRING = re.compile(
    rb"<key>([A-Za-z0-9_]+)</key>\s*<string>([^<]*)</string>")
ID_TOKEN = re.compile(r"0x[0-9A-Fa-f]{8}")


def _decode(raw):
    text = raw.decode("ascii", "replace")
    return text.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")


def _nearest(pairs, name, hit, before):
    """Nearest (offset, value) for `name` before or after `hit` in one window."""
    best = None
    for off, key, value in pairs:
        if key != name:
            continue
        if before and off < hit:
            if best is None or off > best[0]:
                best = (off, value)
        elif not before and off > hit:
            if best is None or off < best[0]:
                best = (off, value)
    return best[1] if best else ""


def scan(path):
    """Return (personalities, stats).

    A personality is a dict with the match key, its id list, and whatever
    identifying keys sit around it. Duplicates are collapsed on the full
    tuple, because the prelink XML appears more than once in a disk image
    (installer payload plus the mounted system) and a doubled row would
    misreport the population without changing any conclusion.
    """
    found = {}
    seen_offsets = set()
    stats = {"bytes": 0, "key_hits": 0}

    base = 0
    tail = b""
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(CHUNK)
            if not chunk:
                break
            buf = tail + chunk
            origin = base - len(tail)
            stats["bytes"] += len(chunk)

            for key_name in MATCH_KEYS:
                needle = ("<key>%s</key>" % key_name).encode("ascii")
                start = 0
                while True:
                    i = buf.find(needle, start)
                    if i < 0:
                        break
                    start = i + 1
                    absolute = origin + i
                    if absolute in seen_offsets:
                        continue
                    seen_offsets.add(absolute)
                    stats["key_hits"] += 1

                    lo = max(0, i - BACK)
                    hi = min(len(buf), i + FWD)
                    window = buf[lo:hi]
                    pairs = [(m.start() + lo,
                              m.group(1).decode("ascii", "replace"),
                              _decode(m.group(2)))
                             for m in KEY_STRING.finditer(window)]

                    value = ""
                    for off, key, val in pairs:
                        if off == i and key == key_name:
                            value = val
                            break
                    if not value:
                        continue

                    row = (
                        key_name,
                        value,
                        _nearest(pairs, "IOClass", i, True),
                        _nearest(pairs, "IOPCIClassMatch", i, True),
                        _nearest(pairs, "IOPersonalityPublisher", i, False),
                        _nearest(pairs, "IOProviderClass", i, False),
                    )
                    found[row] = found.get(row, 0) + 1

            base += len(chunk)
            tail = buf[-OVERLAP:]
            sys.stderr.write("\r  %5.2f GiB, %d match keys"
                             % (stats["bytes"] / (1024.0 ** 3),
                                stats["key_hits"]))
            sys.stderr.flush()
    sys.stderr.write("\n")
    return found, stats


def report(found, stats, path):
    lines = []
    add = lines.append

    rows = sorted(found.keys(), key=lambda r: (r[4] or r[2] or "", r[0]))

    every_id = {}
    for row in rows:
        for token in ID_TOKEN.findall(row[1]):
            every_id.setdefault(token.upper(), []).append(row[4] or row[2])

    add("macOS PCI DISPLAY MATCH REPORT")
    add("=" * 74)
    add("")
    add("source image : %s" % path)
    add("               %d bytes" % os.path.getsize(path))
    add("bytes read   : %d" % stats["bytes"])
    add("")
    add("scan reach   : %d match keys read, %d distinct personalities,"
        % (stats["key_hits"], len(rows)))
    add("               %d distinct device ids" % len(every_id))
    add("")
    add("This listing is UNFILTERED. Every personality below carrying an")
    add("IOPCIPrimaryMatch / IOPCIMatch / IOPCISecondaryMatch key is shown, and")
    add("the reach line above equals the number of rows in the body. Do not")
    add("reintroduce a filter: an absence measured against an excerpt is not an")
    add("absence, and this tree has already published one by accident.")
    add("")

    add("-" * 74)
    add("CORROBORATION -- is a zero below an absence, or a blind parse?")
    add("-" * 74)
    add("")
    arms = []
    arms.append(("at least one personality was parsed", len(rows) > 0))
    arms.append(("at least one IOPCIClassMatch was recovered",
                 any(r[3] for r in rows)))
    arms.append(("every parsed row names a provider class",
                 all(r[5] for r in rows) if rows else False))
    for ident, note in CONTROL_IDS:
        arms.append(("control id %s present -- %s" % (ident, note),
                     ident.upper() in every_id))
    for label, ok in arms:
        add("  [%s] %s" % ("ok" if ok else "XX", label))
    add("")
    trustworthy = all(ok for _, ok in arms)
    if trustworthy:
        add("  VERDICT: parse REACHES the match dictionaries, so an id absent")
        add("           from the body below is genuinely not bound by any")
        add("           driver in this image.")
    else:
        add("  VERDICT: UNPROVEN -- a control failed. Treat every absence below")
        add("           as a property of this parse, not of macOS.")
    add("")

    add("-" * 74)
    add("THE QUESTION THIS TREE IS ASKING")
    add("-" * 74)
    add("")
    hit = TARGET_ID.upper() in every_id
    add("  target id : %s -- %s" % (TARGET_ID, TARGET_NOTE))
    if hit:
        add("  RESULT    : PRESENT, bound by %s"
            % ", ".join(sorted(set(every_id[TARGET_ID.upper()]))))
    else:
        add("  RESULT    : ABSENT from all %d distinct device ids."
            % len(every_id))
        add("")
        add("  No shipping personality names this part, so no framebuffer")
        add("  driver will ever attach to it. Note WHY this is stronger than")
        add("  the kext-name absence already in PLAN.md condition B: it is not")
        add("  merely that no kext is called Tiger Lake, it is that the")
        add("  allow-lists inside the kexts that DO ship exclude this silicon")
        add("  by number.")
    add("")
    add("  Matching is BOTH-of when both keys are present: IOPCIClassMatch")
    add("  0x03000000&0xff000000 accepts any display controller, and")
    add("  IOPCIPrimaryMatch then narrows it to the explicit list. A part")
    add("  outside the list fails the personality regardless of class.")
    add("")

    add("-" * 74)
    add("PERSONALITIES (%d, unfiltered)" % len(rows))
    add("-" * 74)
    add("")
    for row in rows:
        key_name, value, ioclass, classmatch, publisher, provider = row
        add("  publisher    : %s" % (publisher or "(none nearby)"))
        add("  IOClass      : %s" % (ioclass or "(none nearby)"))
        add("  provider     : %s" % (provider or "(none nearby)"))
        if classmatch:
            add("  class match  : %s" % classmatch)
        add("  %-12s : %s" % (key_name, value))
        add("")

    add("-" * 74)
    add("EVERY DEVICE ID (%d, unfiltered, sorted)" % len(every_id))
    add("-" * 74)
    add("")
    for ident in sorted(every_id):
        owners = sorted(set(o for o in every_id[ident] if o))
        add("  %s  %s" % (ident, ", ".join(owners) or "(unattributed)"))
    add("")

    add("-" * 74)
    add("WHAT THIS DOES NOT ESTABLISH")
    add("-" * 74)
    add("")
    add("1. This is the RECOVERY image, a trimmed subset of a full install.")
    add("   Strong evidence about the installed system volume, not proof.")
    add("2. An id absent here could still be reached by injecting a fake")
    add("   device id at the PCI level, which is what the Hackintosh")
    add("   community does. That makes a driver ATTACH; it does not make the")
    add("   driver correct. Gen11 and Gen12 are different display engines,")
    add("   so an Ice Lake driver driving Tiger Lake registers programs the")
    add("   wrong block -- the failure mode this tree already measured for")
    add("   PLANE_SIZE versus PIPESRC, where a plausible wrong picture is the")
    add("   dangerous outcome rather than a blank screen.")
    add("3. Nothing here is a hardware measurement. It is a read of Apple's")
    add("   shipped match dictionaries.")
    return lines


def main(argv):
    path = argv[1] if len(argv) > 1 else DEFAULT_IMAGE
    if not os.path.exists(path):
        sys.stderr.write("no such image: %s\n" % path)
        return 2
    found, stats = scan(path)
    for line in report(found, stats, path):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
