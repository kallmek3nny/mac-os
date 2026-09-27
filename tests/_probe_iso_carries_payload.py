#!/usr/bin/env python3
"""Ad-hoc probe: does step7-payload.iso physically carry the CURRENT bytes?

The existing gates compare build-payload/kext-src.tar and
build-payload/guest-build-kext.sh against the live tree. Nothing asserts the
ISO -- the artifact the operator actually mounts -- was rebuilt after those.
tools/build-step7-payload.py's verify() only checks the ISO is LARGER than its
inputs, which a stale ISO also satisfies.

This prints the raw evidence; it is a probe, not a gate.
"""
import hashlib
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
bp = root / "build-payload"

iso = (bp / "step7-payload.iso").read_bytes()
tar = (bp / "kext-src.tar").read_bytes()
sh = (bp / "guest-build-kext.sh").read_bytes()
toolsh = (root / "tools" / "guest-build-kext.sh").read_bytes()


def sha(b):
    return hashlib.sha256(b).hexdigest()[:16]


def offsets(hay, needle):
    out, i = [], hay.find(needle)
    while i != -1:
        out.append(i)
        i = hay.find(needle, i + 1)
    return out


print("iso bytes       :", len(iso), sha(iso))
print("tar bytes       :", len(tar), sha(tar))
print("tar IN iso      :", iso.find(tar) != -1, "at offset", iso.find(tar))
print("tar copies      :", len(offsets(iso, tar)))
print("sh  bytes       :", len(sh), sha(sh))
print("sh  IN iso      :", iso.find(sh) != -1, "at offset", iso.find(sh))
print("sh  copies      :", len(offsets(iso, sh)))
print("staged sh == tools/sh :", sh == toolsh)
print("staged sh has CR:", b"\r" in sh)
print("tools/sh has CR :", b"\r" in toolsh)

names = sorted(set(re.findall(rb"[A-Z0-9_.\-]{3,40};1", iso)))
print("iso member names:", [n.decode() for n in names])

# Region map: everything the two known members occupy, so a third payload
# cannot hide. Report every kext-header mention by which region it lands in.
tar_lo, tar_hi = iso.find(tar), iso.find(tar) + len(tar)
sh_lo, sh_hi = iso.find(sh), iso.find(sh) + len(sh)


def region(off):
    if tar_lo <= off < tar_hi:
        return "inside kext-src.tar"
    if sh_lo <= off < sh_hi:
        return "inside guest-build-kext.sh"
    return "LOOSE (neither member)"


print("tar region      :", tar_lo, "..", tar_hi)
print("sh  region      :", sh_lo, "..", sh_hi)
for stem in (b"NVDecode", b"NVFalcon", b"NVGspBoot", b"NVGspStage",
             b"NVRiscvUcode", b"NV3050Probe"):
    hits = offsets(iso, stem)
    tally = {}
    for h in hits:
        tally[region(h)] = tally.get(region(h), 0) + 1
    print(f"  {stem.decode():14s} {len(hits):3d} hits  {tally}")
