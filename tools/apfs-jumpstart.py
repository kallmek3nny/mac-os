#!/usr/bin/env python3
"""Read the APFS container's EFI JumpStart record, and EXTRACT the driver.

Why this exists
---------------
The converted guest stops at the EFI shell because stock edk2 cannot read APFS,
so `boot.efi` in the Preboot volume is invisible to BDS (QEMU-LOWSTORAGE.md
section 5, and `qemu-results/boot-probe-serial.log`).

The usual answer is to go get a third-party `ApfsDriverLoader.efi` or build
edk2 with an APFS package. Neither is necessary if the container carries its
own driver -- and the APFS format says it may. `nx_superblock_t` has a
`nx_efi_jumpstart` field (offset 0x4F8) holding the physical block of an
`nx_efi_jumpstart_t` record, whose extent list IS the bytes of Apple's own
`apfs.efi`. Apple put it there for exactly this purpose: firmware that cannot
parse APFS can still be pointed at a contiguous run of blocks.

So the smallest possible artifact set for booting this image is *already inside
the image*. This tool measures whether that is true here, and if it is, writes
the driver out.

What it asserts, and what it only reports
----------------------------------------
The assertion is the record's `nej_magic`, which is `'RDSJ'` as a uint32 and
therefore the bytes `JSDR` on disk. That is unambiguous -- no other structure
in the container spells it.

`o_type` is REPORTED, not asserted. The documented value is
`OBJECT_TYPE_EFI_JUMPSTART` (0x14), and this prints what it actually found
alongside that expectation. A tool that refuses on a constant it half-remembers
turns a good image into a fake negative, and this project has already been
bitten by a probe that returned a confident zero because its addressing was
wrong rather than the disk being empty (see `apfs-volumes.py`, "Why the object
map, and not a scan for magic bytes").

The extracted bytes are checked as PE/COFF -- `MZ`, then `PE\0\0` at
`e_lfanew`, machine, and subsystem -- because "I wrote 400 KB to a file" is not
"I extracted a driver". A wrong extent list produces a file of exactly the
right length full of the wrong bytes, and nothing about that announces itself.

Usage
-----
    python tools/apfs-jumpstart.py --qcow2 F:\\nv3050-qemu\\sonoma-base.qcow2
    python tools/apfs-jumpstart.py --qcow2 IMG --extract F:\\nv3050-qemu\\apfs.efi
    python tools/apfs-jumpstart.py --raw disk.img --lba 409640 --json

Exit codes follow this project's convention: 2 for "you asked wrongly", 1 for
"the thing you asked about is not there" (no jumpstart record, or the extracted
bytes are not a PE image), 0 only when the requested action completed.
"""

import argparse
import importlib.util
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# nx_superblock_t. Offsets that this file needs and apfs-volumes.py does not.
NX_EFI_JUMPSTART_OFF = 0x4F8

# nx_efi_jumpstart_t.
NEJ_MAGIC = b"JSDR"          # 'RDSJ' stored little-endian
NEJ_MAGIC_OFF = 0x20
NEJ_VERSION_OFF = 0x24
NEJ_FILE_LEN_OFF = 0x28
NEJ_NUM_EXTENTS_OFF = 0x2C
NEJ_EXTENTS_OFF = 0xB0       # after uint64_t nej_reserved[16]
PRANGE_SIZE = 16             # paddr_t pr_start_paddr; uint64_t pr_block_count

OBJ_TYPE_OFF = 0x18
OBJECT_TYPE_EFI_JUMPSTART = 0x14   # documented; reported, not asserted

# PE/COFF, enough to tell a driver from 400 KB of the wrong blocks.
PE_SUBSYSTEMS = {
    10: "EFI_APPLICATION",
    11: "EFI_BOOT_SERVICE_DRIVER",
    12: "EFI_RUNTIME_DRIVER",
    13: "EFI_ROM",
}
PE_MACHINES = {
    0x8664: "x86_64",
    0x014C: "i386",
    0xAA64: "aarch64",
}


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, filename))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def live_superblock(container):
    """Return the bytes of the checkpoint-current NXSB.

    Deliberately re-reads rather than caching a copy inside Container: the
    checkpoint choice is that class's business, and duplicating the selection
    here is how the two would drift apart.
    """
    live = container.checkpoint["block"]
    if live == 0:
        return container.reader.read(container.base, container.block_size)
    return container.block(live)


def read_jumpstart(container):
    """Return a dict describing the container's EFI JumpStart record.

    `present` is False with a `reason` when the field is zero -- an image built
    by a tool that never wrote one is a legitimate outcome, not an error.
    """
    sb = live_superblock(container)
    paddr, = struct.unpack("<Q", sb[NX_EFI_JUMPSTART_OFF:NX_EFI_JUMPSTART_OFF + 8])
    out = {
        "nx_efi_jumpstart": paddr,
        "checkpoint_block": container.checkpoint["block"],
        "checkpoint_xid": container.xid,
        "block_size": container.block_size,
        "present": False,
    }
    if paddr == 0:
        out["reason"] = ("nx_efi_jumpstart is 0 -- this container carries no "
                         "embedded EFI driver")
        return out

    blk = container.block(paddr)
    otype, = struct.unpack("<I", blk[OBJ_TYPE_OFF:OBJ_TYPE_OFF + 4])
    out["o_type"] = otype
    out["o_type_low16"] = otype & 0xFFFF
    out["o_type_expected"] = OBJECT_TYPE_EFI_JUMPSTART
    out["o_type_matches"] = (otype & 0xFFFF) == OBJECT_TYPE_EFI_JUMPSTART

    magic = blk[NEJ_MAGIC_OFF:NEJ_MAGIC_OFF + 4]
    out["magic"] = magic.decode("latin-1")
    if magic != NEJ_MAGIC:
        out["reason"] = ("block %d is not an nx_efi_jumpstart_t: magic %r, "
                         "expected %r" % (paddr, magic, NEJ_MAGIC))
        return out

    version, = struct.unpack("<I", blk[NEJ_VERSION_OFF:NEJ_VERSION_OFF + 4])
    file_len, = struct.unpack("<I", blk[NEJ_FILE_LEN_OFF:NEJ_FILE_LEN_OFF + 4])
    count, = struct.unpack("<I", blk[NEJ_NUM_EXTENTS_OFF:NEJ_NUM_EXTENTS_OFF + 4])
    out["version"] = version
    out["efi_file_len"] = file_len
    out["num_extents"] = count

    # An extent count large enough to run past the end of the block would read
    # neighbouring structures as extents and produce plausible-looking garbage.
    max_extents = (container.block_size - NEJ_EXTENTS_OFF) // PRANGE_SIZE
    out["max_extents_in_block"] = max_extents
    if count > max_extents:
        out["reason"] = ("nej_num_extents %d does not fit in one %d-byte block "
                         "(max %d)" % (count, container.block_size, max_extents))
        return out

    extents = []
    covered = 0
    for i in range(count):
        off = NEJ_EXTENTS_OFF + i * PRANGE_SIZE
        start, blocks = struct.unpack("<QQ", blk[off:off + PRANGE_SIZE])
        extents.append({"start": start, "blocks": blocks})
        covered += blocks * container.block_size
    out["extents"] = extents
    out["bytes_covered"] = covered

    if covered < file_len:
        out["reason"] = ("extents cover %d bytes but nej_efi_file_len is %d -- "
                         "the record is truncated or misparsed"
                         % (covered, file_len))
        return out

    out["present"] = True
    return out


def read_extents(container, extents, file_len):
    """Concatenate the extent blocks and trim to the declared file length."""
    buf = bytearray()
    for e in extents:
        for i in range(e["blocks"]):
            buf += container.block(e["start"] + i)
            if len(buf) >= file_len:
                break
        if len(buf) >= file_len:
            break
    return bytes(buf[:file_len])


def describe_pe(data):
    """Report whether `data` is a PE/COFF image, and what kind.

    Returns a dict with `is_pe` plus whatever fields could be decoded. The
    point is to separate "wrote the right number of bytes" from "wrote a
    driver" -- a wrong extent list yields exactly the former.
    """
    out = {"is_pe": False, "length": len(data)}
    if len(data) < 0x40 or data[:2] != b"MZ":
        out["reason"] = "no MZ signature at offset 0"
        return out
    lfanew, = struct.unpack("<I", data[0x3C:0x40])
    out["e_lfanew"] = lfanew
    if lfanew + 24 > len(data):
        out["reason"] = "e_lfanew 0x%x runs past the end of the file" % lfanew
        return out
    if data[lfanew:lfanew + 4] != b"PE\x00\x00":
        out["reason"] = ("no PE\\0\\0 at e_lfanew 0x%x (found %r)"
                         % (lfanew, data[lfanew:lfanew + 4]))
        return out
    machine, = struct.unpack("<H", data[lfanew + 4:lfanew + 6])
    out["machine"] = machine
    out["machine_name"] = PE_MACHINES.get(machine, "unknown")
    opt_off = lfanew + 24
    if opt_off + 70 <= len(data):
        subsystem, = struct.unpack("<H", data[opt_off + 68:opt_off + 70])
        out["subsystem"] = subsystem
        out["subsystem_name"] = PE_SUBSYSTEMS.get(subsystem, "unknown")
    out["is_pe"] = True
    return out


def build_reader(args):
    if args.qcow2:
        return load("qr", "qcow2-read.py").Qcow2(args.qcow2), args.qcow2
    if args.vmdk:
        return load("vmr", "vmdk-read.py").Vmdk(args.vmdk), args.vmdk

    class Raw(object):
        def __init__(self, path):
            self.f = open(path, "rb")

        def read(self, off, length):
            self.f.seek(off)
            return self.f.read(length)

    return Raw(args.raw), args.raw


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="apfs-jumpstart.py",
        description="Read (and extract) the APFS container's embedded EFI driver.")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--qcow2", help="read through tools/qcow2-read.py")
    src.add_argument("--vmdk", help="read through tools/vmdk-read.py")
    src.add_argument("--raw", help="read a plain raw image")
    p.add_argument("--lba", type=int, default=409640,
                   help="512-byte LBA where the APFS container starts "
                        "(default 409640, the Sonoma image's partition 2)")
    p.add_argument("--extract", metavar="PATH",
                   help="write the driver to PATH")
    p.add_argument("--json", action="store_true", help="machine-readable output")
    args = p.parse_args(argv)

    vol = load("apfsvol", "apfs-volumes.py")
    reader, label = build_reader(args)
    container = vol.Container(reader, args.lba)
    info = read_jumpstart(container)
    info["image"] = label
    info["lba"] = args.lba

    if not info["present"]:
        if args.json:
            print(json.dumps(info, indent=2))
        else:
            print("image      : %s (lba %d)" % (label, args.lba))
            print("checkpoint : block %d, xid %d"
                  % (info["checkpoint_block"], info["checkpoint_xid"]))
            print("JUMPSTART  : ABSENT")
            print("reason     : %s" % info.get("reason", "unknown"))
        return 1

    data = read_extents(container, info["extents"], info["efi_file_len"])
    pe = describe_pe(data)
    info["pe"] = pe

    if args.extract and pe["is_pe"]:
        with open(args.extract, "wb") as f:
            f.write(data)
        info["extracted_to"] = args.extract
        info["extracted_bytes"] = len(data)
    elif args.extract:
        info["extracted_to"] = None
        info["extract_refused"] = ("not written: the extracted bytes are not a "
                                   "PE image (%s)" % pe.get("reason", "unknown"))

    if args.json:
        print(json.dumps(info, indent=2))
    else:
        print("image      : %s (lba %d)" % (label, args.lba))
        print("checkpoint : block %d, xid %d"
              % (info["checkpoint_block"], info["checkpoint_xid"]))
        print("JUMPSTART  : block %d, magic %s, version %d"
              % (info["nx_efi_jumpstart"], info["magic"], info["version"]))
        print("o_type     : 0x%08x (low16 0x%04x; documented 0x%04x -- %s)"
              % (info["o_type"], info["o_type_low16"], info["o_type_expected"],
                 "matches" if info["o_type_matches"] else "DIFFERS, reported not asserted"))
        print("driver     : %d bytes in %d extent(s), %d bytes covered"
              % (info["efi_file_len"], info["num_extents"], info["bytes_covered"]))
        for e in info["extents"]:
            print("             block %d + %d" % (e["start"], e["blocks"]))
        if pe["is_pe"]:
            print("PE/COFF    : %s, subsystem %s (%s)"
                  % (pe["machine_name"], pe.get("subsystem"),
                     pe.get("subsystem_name", "unknown")))
        else:
            print("PE/COFF    : NOT A PE IMAGE -- %s" % pe.get("reason", "unknown"))
        if info.get("extracted_to"):
            print("wrote      : %s (%d bytes)"
                  % (info["extracted_to"], info["extracted_bytes"]))
        elif info.get("extract_refused"):
            print("wrote      : nothing -- %s" % info["extract_refused"])

    return 0 if pe["is_pe"] else 1


if __name__ == "__main__":
    sys.exit(main())
