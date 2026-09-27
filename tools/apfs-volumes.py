#!/usr/bin/env python3
"""List the APFS volumes inside a disk image, by WALKING the object map.

Why this exists
---------------
The converted guest does not boot under stock edk2. The serial log says why in
plain text:

    BdsDxe: failed to load Boot0001 "UEFI QEMU HARDDISK QM00013 "
            from PciRoot(0x0)/Pci(0x1,0x0)/Sata(0x0,0xFFFF,0x0): Not Found
    BdsDxe: loading Boot0002 "EFI Internal Shell"

and a FAT walk of the EFI System Partition explains it: the ESP root holds
exactly one 32-byte slot, `attr=0x28` (VOLID|ARCH) -- a volume label. There is no
`EFI/` directory and no `BOOTX64.EFI`. The same walk against the SOURCE
`Sonoma.vmdk`, which VMware still boots, returns the identical single VOLID slot,
so the empty ESP is the guest's real state and not conversion damage.

That leaves one question: where does the bootloader live? The answer decides
whether this is fixable. This tool answers it by naming the volumes.

Why the object map, and not a scan for magic bytes
--------------------------------------------------
The first attempt swept 512 MiB of the container for the `APSB` volume-superblock
magic and found **zero**, while the same sweep counted 141 `NXSB` blocks. The
NXSB count is the positive control, and it fired -- so the offset was right and
the METHOD was wrong. APFS does not lay volume superblocks out contiguously: the
container superblock names them by *virtual* object id in `nx_fs_oid[]`, and each
id resolves to a physical block only through the container's object map B-tree.
A linear magic scan can therefore return a confident zero on a container full of
volumes, which is precisely the class of answer this project refuses to file.

So this reads the documented structures: NXSB -> nx_omap_oid -> omap_phys ->
B-tree -> {virtual oid: physical block} -> APSB -> apfs_volname.

Usage:
    python tools/apfs-volumes.py --qcow2 F:/nv3050-qemu/sonoma-base.qcow2 --lba 409640
    python tools/apfs-volumes.py --vmdk  C:/path/Sonoma.vmdk --lba 409640

Exit codes: 2 for a malformed invocation, 1 when the container cannot be read,
0 only when the volume list was produced.
"""

import argparse
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

NX_MAGIC = b"NXSB"
APSB_MAGIC = b"APSB"

# obj_phys_t is 32 bytes; every offset below is from the START of the block.
NX_MAGIC_OFF = 0x20
NX_BLOCK_SIZE_OFF = 0x24
NX_BLOCK_COUNT_OFF = 0x28
NX_OMAP_OID_OFF = 0xA0
NX_MAX_FS_OFF = 0xB4
NX_FS_OID_OFF = 0xB8
NX_FS_OID_COUNT = 100
NX_XID_OFF = 0x10           # obj_phys_t.o_xid
NX_XP_DESC_BLOCKS_OFF = 0x68
NX_XP_DESC_BASE_OFF = 0x70

OBJ_TYPE_OFF = 0x18        # obj_phys_t.o_type / o_subtype
OBJECT_TYPE_OMAP = 0x000B
OBJECT_TYPE_BTREE = 0x0002

OMAP_TREE_OID_OFF = 0x30

# btree_node_phys_t
BTN_FLAGS_OFF = 0x20
BTN_DATA_OFF = 0x38
BTNODE_ROOT = 0x0001
BTNODE_LEAF = 0x0002
BTNODE_FIXED_KV_SIZE = 0x0004
BTREE_INFO_SIZE = 40  # btree_info_t, present only in the ROOT node's tail

# apfs_superblock_t offsets, DERIVED FROM THIS IMAGE rather than copied.
#
# The first attempt guessed 0x2D0 for the name and got 'ata' with 24272 files and
# role 0x0003 -- a fragment, not a failure. Nothing raised, and 'ata' is exactly
# the kind of output that reads as a weird-but-real volume name. The fix came
# from dumping every printable run in a real APSB block:
#
#   off 0x0110  'storagekitd (2332.101.1)'
#   off 0x0140  'apfs_kext (2332.140.13.702.2)'
#   off 0x0170  'apfs_kext (2332.101.1)'
#   off 0x02C0  'Macintosh HD - Data'
#
# The 0x30 spacing IS sizeof(apfs_modified_by_t) = 32-byte id + 8 + 8 = 48. So
# apfs_formatted_by sits at 0x110, apfs_modified_by[8] runs 0x140..0x2BF, and the
# name begins at 0x2C0 -- confirmed directly by the fourth run above. The name is
# 256 bytes, so apfs_next_doc_id (u32) follows at 0x3C0 and apfs_role (u16) at
# 0x3C4. Walking back from 0x110 through the documented field list puts
# apfs_num_files at 0xB8 and apfs_fs_flags at 0x108.
APFS_MAGIC = b"APSB"
APFS_VOLNAME_OFF = 0x2C0
APFS_VOLNAME_LEN = 256
APFS_FS_FLAGS_OFF = 0x108
APFS_NUM_FILES_OFF = 0xB8
APFS_ROLE_OFF = 0x3C4

# apfs_fs_flags bits worth naming
APFS_FS_UNENCRYPTED = 0x00000001
APFS_FS_ONEKEY = 0x00000004

# apfs_role values (APFS_VOL_ROLE_*)
ROLES = {
    0x0000: "NONE",
    0x0001: "SYSTEM",
    0x0002: "USER",
    0x0004: "RECOVERY",
    0x0008: "VM",
    0x0010: "PREBOOT",
    0x0020: "INSTALLER",
    0x0040: "DATA",
    0x0080: "BASEBAND",
}


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, filename))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Container(object):
    """An APFS container read through a caller-supplied image reader."""

    def __init__(self, reader, base_lba):
        self.reader = reader
        self.base = base_lba * 512
        head = reader.read(self.base, 4096)
        magic = head[NX_MAGIC_OFF:NX_MAGIC_OFF + 4]
        if magic != NX_MAGIC:
            raise ValueError("no NXSB at LBA %d: magic is %r" % (base_lba, magic))
        self.block_size, = struct.unpack("<I", head[NX_BLOCK_SIZE_OFF:NX_BLOCK_SIZE_OFF + 4])
        if self.block_size not in (4096, 8192, 16384, 32768, 65536):
            raise ValueError("implausible APFS block size %d" % self.block_size)
        self.block_count, = struct.unpack("<Q", head[NX_BLOCK_COUNT_OFF:NX_BLOCK_COUNT_OFF + 8])
        self.blocks_read = 0

        # BLOCK 0 IS A COPY, AND IT LAGS.
        #
        # Measured on this image: block 0's NXSB carries o_xid 12589 and
        # nx_omap_oid 380165, while the newest superblock in the checkpoint
        # descriptor area carries o_xid 15491 and nx_omap_oid 382546 -- 2902
        # transactions newer. Following block 0's pointer landed on a block whose
        # o_type is 0x40000003 (a B-tree node of the BLOCKREFTREE) rather than
        # 0x4000000b (OBJECT_TYPE_OMAP), and reading it as an omap_phys yielded
        # om_tree_oid = 78819521836614232 -- a block number larger than the disk.
        #
        # Nothing about that failure announced itself as "stale pointer". The
        # struct parsed, every field decoded, and the only tell was a magnitude
        # that happened to be absurd. Had the misread bytes produced a plausible
        # block number instead, this would have walked a wrong tree and reported
        # confident nonsense. So the o_type of every block this walk lands on is
        # now CHECKED against the type it is being parsed as.
        self.checkpoint = self._latest_checkpoint(head)
        live = self.checkpoint["block"]
        sb = head if live == 0 else self.block(live)
        self.xid, = struct.unpack("<Q", sb[NX_XID_OFF:NX_XID_OFF + 8])
        self.omap_oid, = struct.unpack("<Q", sb[NX_OMAP_OID_OFF:NX_OMAP_OID_OFF + 8])
        self.max_fs, = struct.unpack("<I", sb[NX_MAX_FS_OFF:NX_MAX_FS_OFF + 4])
        fs = struct.unpack("<%dQ" % NX_FS_OID_COUNT,
                           sb[NX_FS_OID_OFF:NX_FS_OID_OFF + NX_FS_OID_COUNT * 8])
        self.fs_oids = [o for o in fs if o]

    def _latest_checkpoint(self, head):
        """Return the newest NXSB in the checkpoint descriptor area.

        `copies` is reported by the caller as a positive control: a scan that
        finds zero superblocks has measured its own addressing, not the disk, and
        silently falling back to block 0 would reintroduce the exact stale-pointer
        bug this function exists to avoid.
        """
        blocks, = struct.unpack("<I", head[NX_XP_DESC_BLOCKS_OFF:NX_XP_DESC_BLOCKS_OFF + 4])
        base, = struct.unpack("<Q", head[NX_XP_DESC_BASE_OFF:NX_XP_DESC_BASE_OFF + 8])
        head_xid, = struct.unpack("<Q", head[NX_XID_OFF:NX_XID_OFF + 8])
        best = {"xid": head_xid, "block": 0, "copies": 0, "from_block0": True}
        if base >> 63:
            # High bit set means the descriptor area is itself a tree rather than
            # a contiguous run. Not walked here -- and SAID so, rather than
            # quietly using block 0 as though it were current.
            best["tree"] = True
            return best
        if blocks == 0 or base == 0:
            return best
        copies = 0
        winner = None
        for i in range(min(blocks, 4096)):
            b = self.block(base + i)
            if b[NX_MAGIC_OFF:NX_MAGIC_OFF + 4] != NX_MAGIC:
                continue
            copies += 1
            xid, = struct.unpack("<Q", b[NX_XID_OFF:NX_XID_OFF + 8])
            # Strictly greater, so the FIRST block holding the winning xid wins
            # and the choice does not depend on scan order.
            if winner is None or xid > winner[0]:
                winner = (xid, base + i)
        if winner is None:
            best["copies"] = 0
            return best
        if winner[0] < best["xid"]:
            # Block 0 is newer than anything in the descriptor area. Legal, and
            # worth saying out loud rather than silently preferring one.
            best["copies"] = copies
            return best
        return {"xid": winner[0], "block": winner[1], "copies": copies,
                "from_block0": False}

    def _typed_block(self, paddr, want_type, label):
        """Read a block and REFUSE it if o_type is not what the caller expects."""
        b = self.block(paddr)
        otype, = struct.unpack("<I", b[OBJ_TYPE_OFF:OBJ_TYPE_OFF + 4])
        if (otype & 0xFFFF) != want_type:
            raise ValueError("block %d is not %s: o_type 0x%08x (low16 0x%04x, "
                             "expected 0x%04x)"
                             % (paddr, label, otype, otype & 0xFFFF, want_type))
        return b

    def block(self, paddr):
        if paddr <= 0 or paddr >= self.block_count:
            raise ValueError("block %d outside container (0..%d)" % (paddr, self.block_count))
        self.blocks_read += 1
        return self.reader.read(self.base + paddr * self.block_size, self.block_size)

    def omap(self):
        """Return {virtual oid: (xid, physical block)} from the container omap.

        The whole tree is traversed rather than binary-searched. The container
        omap of a normal install holds a handful of entries, so an exhaustive
        walk costs nothing and removes an entire class of off-by-one from the
        key-comparison path.
        """
        om = self._typed_block(self.omap_oid, OBJECT_TYPE_OMAP, "omap_phys")
        tree_oid, = struct.unpack("<Q", om[OMAP_TREE_OID_OFF:OMAP_TREE_OID_OFF + 8])
        best = {}
        self._walk(tree_oid, best, depth=0)
        return best

    def _walk(self, paddr, best, depth):
        if depth > 8:
            raise ValueError("omap B-tree deeper than 8 levels at block %d" % paddr)
        node = self.block(paddr)
        flags, level, nkeys = struct.unpack("<HHI", node[BTN_FLAGS_OFF:BTN_FLAGS_OFF + 8])
        ts_off, ts_len = struct.unpack("<HH", node[0x28:0x2C])
        is_root = bool(flags & BTNODE_ROOT)
        is_leaf = bool(flags & BTNODE_LEAF)
        fixed = bool(flags & BTNODE_FIXED_KV_SIZE)

        key_base = BTN_DATA_OFF + ts_off + ts_len
        # Values are addressed from the END of the node. The ROOT node -- and
        # only the root -- carries a 40-byte btree_info_t tail, so measuring
        # from block_size on a root reads 40 bytes into that struct instead of
        # the last value. Nothing raises; the value simply decodes to garbage.
        val_base = self.block_size - (BTREE_INFO_SIZE if is_root else 0)
        ent_size = 4 if fixed else 8

        for i in range(nkeys):
            eo = BTN_DATA_OFF + ts_off + i * ent_size
            ent = node[eo:eo + ent_size]
            if len(ent) < ent_size:
                raise ValueError("node %d: table entry %d truncated" % (paddr, i))
            if fixed:
                koff, voff = struct.unpack("<HH", ent)
            else:
                koff, _klen, voff, _vlen = struct.unpack("<HHHH", ent)
            ka = key_base + koff
            oid, xid = struct.unpack("<QQ", node[ka:ka + 16])
            if is_leaf:
                va = val_base - voff
                _flags, _size, paddr_v = struct.unpack("<IIQ", node[va:va + 16])
                prev = best.get(oid)
                if prev is None or xid >= prev[0]:
                    best[oid] = (xid, paddr_v)
            else:
                va = val_base - voff
                child, = struct.unpack("<Q", node[va:va + 8])
                self._walk(child, best, depth + 1)

    def volumes(self):
        omap = self.omap()
        out = []
        for oid in self.fs_oids:
            hit = omap.get(oid)
            if hit is None:
                out.append({"oid": oid, "error": "not present in container omap"})
                continue
            xid, paddr = hit
            blk = self.block(paddr)
            magic = blk[NX_MAGIC_OFF:NX_MAGIC_OFF + 4]
            if magic != APSB_MAGIC:
                out.append({"oid": oid, "paddr": paddr,
                            "error": "magic %r is not APSB" % magic})
                continue
            raw = blk[APFS_VOLNAME_OFF:APFS_VOLNAME_OFF + APFS_VOLNAME_LEN]
            name = raw.split(b"\x00")[0].decode("utf-8", "replace")
            role, = struct.unpack("<H", blk[APFS_ROLE_OFF:APFS_ROLE_OFF + 2])
            fs_flags, = struct.unpack("<Q", blk[APFS_FS_FLAGS_OFF:APFS_FS_FLAGS_OFF + 8])
            nfiles, = struct.unpack("<Q", blk[APFS_NUM_FILES_OFF:APFS_NUM_FILES_OFF + 8])
            out.append({"oid": oid, "xid": xid, "paddr": paddr, "name": name,
                        "role": role, "fs_flags": fs_flags, "files": nfiles})
        return out


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="apfs-volumes.py",
        description="List APFS volumes by walking the container object map.")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--qcow2", help="read through tools/qcow2-read.py")
    src.add_argument("--vmdk", help="read through tools/vmdk-read.py")
    src.add_argument("--raw", help="read a plain raw image")
    p.add_argument("--lba", type=int, default=409640,
                   help="first sector of the APFS container (default: this guest's GPT #2)")
    args = p.parse_args(argv)

    try:
        if args.qcow2:
            reader = load("q2r", "qcow2-read.py").Qcow2(args.qcow2)
            label = args.qcow2
        elif args.vmdk:
            reader = load("vmr", "vmdk-read.py").Vmdk(args.vmdk)
            label = args.vmdk
        else:
            class Raw(object):
                def __init__(self, path):
                    self.f = open(path, "rb")

                def read(self, off, length):
                    self.f.seek(off)
                    return self.f.read(length)
            reader = Raw(args.raw)
            label = args.raw

        cont = Container(reader, args.lba)
    except (OSError, ValueError, struct.error) as exc:
        sys.stderr.write("FAIL: %s\n" % exc)
        return 1

    print("image          : %s" % label)
    print("container LBA  : %d" % args.lba)
    print("block size     : %d" % cont.block_size)
    print("block count    : %d (%.2f GiB)"
          % (cont.block_count, cont.block_count * cont.block_size / 1073741824.0))
    print("nx_omap_oid    : %d" % cont.omap_oid)
    print("nx_fs_oid set  : %d of %d" % (len(cont.fs_oids), cont.max_fs))

    try:
        vols = cont.volumes()
    except (OSError, ValueError, struct.error) as exc:
        sys.stderr.write("FAIL: cannot resolve volumes: %s\n" % exc)
        return 1

    print()
    print("%-4s %-10s %-24s %-9s %s" % ("oid", "block", "volume name", "role", "files"))
    named = 0
    for v in vols:
        if "error" in v:
            print("%-4d %-10s %s" % (v["oid"], v.get("paddr", "-"), "ERROR: " + v["error"]))
            continue
        named += 1
        print("%-4d %-10d %-24s %-9s %d"
              % (v["oid"], v["paddr"], repr(v["name"]),
                 ROLES.get(v["role"], "0x%04X" % v["role"]), v["files"]))

    print()
    print("volumes resolved: %d of %d" % (named, len(vols)))
    print("container blocks read: %d" % cont.blocks_read)

    roles = set(ROLES.get(v.get("role"), None) for v in vols if "error" not in v)
    if "PREBOOT" in roles:
        print()
        print("A volume with role PREBOOT is present. On APFS, macOS's boot.efi lives")
        print("in the Preboot volume, NOT on the EFI System Partition -- which is why")
        print("the ESP holds only a volume label in BOTH images, and why firmware with")
        print("no APFS driver reports Boot0001 'Not Found' on a perfectly good disk.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
