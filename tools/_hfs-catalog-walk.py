#!/usr/bin/env python3
"""
_hfs-catalog-walk.py -- enumerate top-level filenames from the HFS+ catalog
inside an Apple Partition Map disk image.

Why this exists
---------------
`darwin.iso` (the VMware Tools image shipped with DrDonk unlocker 4.2.8) is NOT
ISO9660 -- block 0 carries `0x4552` ('ER'), the APM driver-descriptor
signature. That is why `sata0:2.deviceType = "cdrom-image"` fails with
`sense key 0x5 asc 0x24` (ILLEGAL REQUEST / invalid field in CDB) and the guest
never sees a volume. The fix under test is to present it as a plain disk via
`DarwinTools.vmdk` (monolithicFlat, `RW 6612 FLAT "...darwin.iso" 0`).

"The partition map parses" is NOT evidence that the guest will get a mountable
volume, and it is certainly not evidence that the Tools installer is inside.
Both of those are positive claims and each needs its own positive measurement:

  1. the HFS+ volume header is real and fits its partition   (done: 'H+' v4,
     1637 blocks x 2048 = 3352576 bytes == partition size, freeBlocks 0)
  2. the catalog B-tree is walkable and names real files     <- THIS SCRIPT

The offset bug this script exists to correct
--------------------------------------------
The first attempt reported `catalog totalNodes : 0`, which is impossible for a
48 KiB catalog fork, and therefore said nothing about the image -- it was a
parser defect. Root cause: BTHeaderRec does not start at the node base. A
B-tree header node begins with a 14-byte BTNodeDescriptor; BTHeaderRec follows
it at +14. So, relative to the NODE base:

    +14+ 0  treeDepth        (uint16)
    +14+ 2  rootNode         (uint32)
    +14+ 6  leafRecords      (uint32)
    +14+10  firstLeafNode    (uint32)
    +14+14  lastLeafNode     (uint32)
    +14+18  nodeSize         (uint16)  -> node base +32   <-- was read right
    +14+20  maxKeyLength     (uint16)  -> node base +34
    +14+22  totalNodes       (uint32)  -> node base +36   <-- WAS READ AT +40
    +14+26  freeNodes        (uint32)  -> node base +40   <-- what +40 really is

Reading totalNodes at +40 returns freeNodes. For a full read-only installer
image freeNodes is legitimately 0 -- which is exactly why the wrong offset
produced a plausible-looking 0 instead of garbage. A wrong number that looks
like a real answer is the failure mode this whole project documents.

Expected, from evidence already in hand: 49152-byte fork / 4096-byte nodes =
12 total nodes.

Usage
-----
    python _hfs-catalog-walk.py <image>            # darwin.iso by default

Exit status
-----------
    0   catalog walked and at least one filename recovered
    1   image parsed but the catalog did not yield names
    2   the image is not APM/HFS+ at all
"""

import os
import struct
import sys

DEFAULT_IMAGE = r"C:\Users\Kentl\macos-vm\unlocker428\iso\darwin.iso"

SECTOR = 512
APM_SIG = 0x4552          # 'ER' -- driver descriptor record, block 0
DPME_SIG = 0x504D         # 'PM' -- partition map entry
HFSP_SIG = b"H+"          # HFS Plus volume header, partition + 1024
HFSX_SIG = b"HX"          # HFSX, same layout for our purposes

# B-tree node kinds (BTNodeDescriptor.kind, signed int8 at +8)
KIND_LEAF = -1
KIND_INDEX = 0
KIND_HEADER = 1
KIND_MAP = 2


def read_at(fh, offset, length):
    fh.seek(offset)
    data = fh.read(length)
    if len(data) != length:
        raise EOFError("short read of %d bytes at 0x%X (got %d)"
                       % (length, offset, len(data)))
    return data


def partitions(fh):
    """Yield (name, type, start_sector, sector_count) for every DPME entry.

    The map is self-describing: entry 1 states how many entries there are, so
    this does not guess a count.
    """
    if struct.unpack(">H", read_at(fh, 0, 2))[0] != APM_SIG:
        raise ValueError("block 0 is not an APM driver descriptor ('ER')")
    total = None
    index = 1
    while total is None or index <= total:
        base = index * SECTOR
        entry = read_at(fh, base, SECTOR)
        if struct.unpack(">H", entry[0:2])[0] != DPME_SIG:
            break
        map_entries, start, count = struct.unpack(">III", entry[4:16])
        if total is None:
            total = map_entries
        name = entry[16:48].split(b"\x00")[0].decode("mac_roman", "replace")
        ptype = entry[48:80].split(b"\x00")[0].decode("mac_roman", "replace")
        yield (name, ptype, start, count)
        index += 1


def volume_header(fh, part_start_sector):
    """Parse the HFS+ volume header at partition + 1024 bytes."""
    base = part_start_sector * SECTOR
    vh = read_at(fh, base + 1024, 512)
    sig = vh[0:2]
    if sig not in (HFSP_SIG, HFSX_SIG):
        raise ValueError("no HFS+ volume header at 0x%X (got %r)"
                         % (base + 1024, sig))
    version = struct.unpack(">H", vh[2:4])[0]
    block_size = struct.unpack(">I", vh[40:44])[0]
    total_blocks = struct.unpack(">I", vh[44:48])[0]
    free_blocks = struct.unpack(">I", vh[48:52])[0]
    # HFSPlusForkData for the catalog file sits at +272: logicalSize (8),
    # clumpSize (4), totalBlocks (4), then 8 x HFSPlusExtentDescriptor (8 each).
    fork = vh[272:272 + 80]
    logical_size = struct.unpack(">Q", fork[0:8])[0]
    fork_blocks = struct.unpack(">I", fork[12:16])[0]
    extents = []
    for i in range(8):
        off = 16 + i * 8
        st, cnt = struct.unpack(">II", fork[off:off + 8])
        if cnt:
            extents.append((st, cnt))
    return {
        "signature": sig, "version": version,
        "block_size": block_size, "total_blocks": total_blocks,
        "free_blocks": free_blocks,
        "catalog_logical_size": logical_size,
        "catalog_blocks": fork_blocks,
        "catalog_extents": extents,
        "volume_base": base,
    }


def btree_header(fh, node_base):
    """Read BTHeaderRec from the header node at `node_base`.

    Offsets are relative to the NODE base and already include the 14-byte
    BTNodeDescriptor -- see the module docstring. Reading totalNodes at +40
    instead of +36 is the exact defect this function was rewritten to fix.
    """
    raw = read_at(fh, node_base, 512)
    kind = struct.unpack(">b", raw[8:9])[0]
    tree_depth = struct.unpack(">H", raw[14:16])[0]
    root_node = struct.unpack(">I", raw[16:20])[0]
    leaf_records = struct.unpack(">I", raw[20:24])[0]
    first_leaf = struct.unpack(">I", raw[24:28])[0]
    last_leaf = struct.unpack(">I", raw[28:32])[0]
    node_size = struct.unpack(">H", raw[32:34])[0]
    max_key_len = struct.unpack(">H", raw[34:36])[0]
    total_nodes = struct.unpack(">I", raw[36:40])[0]     # was wrongly +40
    free_nodes = struct.unpack(">I", raw[40:44])[0]      # what +40 really is
    return {
        "kind": kind, "tree_depth": tree_depth, "root_node": root_node,
        "leaf_records": leaf_records,
        "first_leaf": first_leaf, "last_leaf": last_leaf,
        "node_size": node_size, "max_key_length": max_key_len,
        "total_nodes": total_nodes, "free_nodes": free_nodes,
    }


def node_offset(vh, node_index, node_size):
    """Map a catalog node index to a byte offset in the image.

    Walks the fork's extent list so a fragmented catalog is handled rather than
    silently mis-read. Returns None if the index is past the fork.
    """
    bs = vh["block_size"]
    want = node_index * node_size
    seen = 0
    for start_block, block_count in vh["catalog_extents"]:
        span = block_count * bs
        if want < seen + span:
            within = want - seen
            return vh["volume_base"] + start_block * bs + within
        seen += span
    return None


def leaf_names(fh, offset, node_size):
    """Extract Unicode filenames from one catalog leaf node.

    Record layout per leaf: the node ends with `numRecords + 1` big-endian
    uint16 offsets. Each record begins with an HFSPlusCatalogKey:
      +0 keyLength (uint16), +2 parentID (uint32), +6 nodeName
    where nodeName is HFSUniStr255: length (uint16) then that many UTF-16BE
    code units.

    Only records whose recordType is a file (2) or folder (1) are reported;
    thread records (3, 4) repeat names and would double every entry.
    """
    node = read_at(fh, offset, node_size)
    kind = struct.unpack(">b", node[8:9])[0]
    num_records = struct.unpack(">H", node[10:12])[0]
    if kind != KIND_LEAF or not num_records:
        return []
    out = []
    for i in range(num_records):
        pos = node_size - 2 * (i + 1)
        rec = struct.unpack(">H", node[pos:pos + 2])[0]
        if rec + 8 > node_size:
            continue
        key_len = struct.unpack(">H", node[rec:rec + 2])[0]
        parent_id = struct.unpack(">I", node[rec + 2:rec + 6])[0]
        name_len = struct.unpack(">H", node[rec + 6:rec + 8])[0]
        name_end = rec + 8 + name_len * 2
        if name_len == 0 or name_end > node_size:
            continue
        try:
            name = node[rec + 8:name_end].decode("utf-16-be")
        except UnicodeDecodeError:
            continue
        # recordType is the first field of the record data, which starts on the
        # even boundary after the key (keyLength counts bytes after itself).
        data_off = rec + 2 + key_len
        data_off += data_off & 1
        if data_off + 2 > node_size:
            continue
        record_type = struct.unpack(">h", node[data_off:data_off + 2])[0]
        if record_type in (1, 2):
            out.append((parent_id, record_type, name))
    return out


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    image = argv[0] if argv else DEFAULT_IMAGE
    if not os.path.isfile(image):
        print("error: no such image: %s" % image, file=sys.stderr)
        return 2

    print("image : %s" % image)
    print("bytes : %d (%d sectors)"
          % (os.path.getsize(image), os.path.getsize(image) // SECTOR))

    with open(image, "rb") as fh:
        try:
            parts = list(partitions(fh))
        except (ValueError, EOFError) as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 2

        print("\npartition map (%d entries)" % len(parts))
        for i, (name, ptype, start, count) in enumerate(parts, 1):
            print("  %d. %-24s %-28s start=%-8d sectors=%-8d (%d bytes)"
                  % (i, name, ptype, start, count, count * SECTOR))

        hfs = [(n, t, s, c) for (n, t, s, c) in parts
               if "HFS" in t.upper()]
        if not hfs:
            print("error: no HFS partition in the map", file=sys.stderr)
            return 2

        name, ptype, start, count = hfs[0]
        vh = volume_header(fh, start)
        vol_bytes = vh["total_blocks"] * vh["block_size"]
        print("\nHFS+ volume in partition %r (%s)" % (name, ptype))
        print("  signature        : %r version %d"
              % (vh["signature"], vh["version"]))
        print("  allocation block : %d bytes" % vh["block_size"])
        print("  totalBlocks      : %d -> %d bytes"
              % (vh["total_blocks"], vol_bytes))
        print("  freeBlocks       : %d" % vh["free_blocks"])
        print("  partition bytes  : %d" % (count * SECTOR))
        print("  volume fits      : %s" % (vol_bytes <= count * SECTOR))
        print("  catalog size     : %d bytes, %d blocks, extents %s"
              % (vh["catalog_logical_size"], vh["catalog_blocks"],
                 vh["catalog_extents"]))

        # The catalog's header node is node 0 of the catalog fork.
        hdr_off = node_offset(vh, 0, 512)
        if hdr_off is None:
            print("error: catalog fork has no extents", file=sys.stderr)
            return 1
        bt = btree_header(fh, hdr_off)
        expected = vh["catalog_logical_size"] // bt["node_size"] \
            if bt["node_size"] else 0
        print("\ncatalog B-tree header at 0x%X" % hdr_off)
        print("  node kind        : %d (1 == header node)" % bt["kind"])
        print("  nodeSize         : %d" % bt["node_size"])
        print("  totalNodes       : %d   (expected %d from %d/%d)"
              % (bt["total_nodes"], expected,
                 vh["catalog_logical_size"], bt["node_size"]))
        print("  freeNodes        : %d   <- the value the old +40 read returned"
              % bt["free_nodes"])
        print("  treeDepth        : %d" % bt["tree_depth"])
        print("  rootNode         : %d" % bt["root_node"])
        print("  leafRecords      : %d" % bt["leaf_records"])
        print("  firstLeaf/last   : %d / %d"
              % (bt["first_leaf"], bt["last_leaf"]))

        if bt["total_nodes"] == 0:
            print("\nFAIL: totalNodes is still 0 -- the offset is not the only "
                  "defect.", file=sys.stderr)
            return 1

        # Walk every node in the fork and take names from the leaves. Scanning
        # all nodes rather than following firstLeaf->fLink is deliberate: a
        # broken forward link would silently truncate the listing, and here a
        # short listing must not be mistaken for a short volume.
        names = []
        scanned = 0
        for idx in range(bt["total_nodes"]):
            off = node_offset(vh, idx, bt["node_size"])
            if off is None:
                break
            try:
                got = leaf_names(fh, off, bt["node_size"])
            except EOFError:
                break
            scanned += 1
            names.extend(got)

        print("\nnodes scanned      : %d of %d" % (scanned, bt["total_nodes"]))
        print("catalog records    : %d file/folder entries" % len(names))

        roots = sorted({n for (pid, rt, n) in names if pid == 2})
        print("\ntop-level entries (parentID 2 == volume root): %d" % len(roots))
        for n in roots:
            print("  %s" % n)

        pkgs = sorted({n for (pid, rt, n) in names
                       if n.lower().endswith((".pkg", ".mpkg", ".app", ".dmg"))})
        if pkgs:
            print("\ninstaller payloads found (%d):" % len(pkgs))
            for n in pkgs:
                print("  %s" % n)

        return 0 if names else 1


if __name__ == "__main__":
    sys.exit(main())
