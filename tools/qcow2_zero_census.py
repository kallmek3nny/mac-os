#!/usr/bin/env python3
# qcow2_zero_census.py -- read-only census of the clusters a qcow2 image stores.
#
# Written for crew task t-9f4907 (QEMU-LOWSTORAGE.md: reclaim the untrimmed
# slack before the laptop copy). A zero-detecting `qemu-img convert` can only
# drop guest clusters whose contents decode to all-zero bytes. This tool decodes
# every allocated cluster in the active L1 table and reports how many are
# all-zero, how many host bytes they occupy, and how many hold non-zero data.
# Non-zero clusters that the guest filesystem has freed can only be reclaimed
# from inside the guest (TRIM/discard, or zero-filling free space); host-side
# zero detection cannot tell them apart from live data.
#
# The image is opened read-only. Nothing is written anywhere.
#
# Supported: qcow2 v2/v3, zlib compression, standard (non-extended) L2 entries,
# no backing file, no external data file, no encryption. Anything else is
# refused rather than guessed at.
#
# Usage:
#   python tools/qcow2_zero_census.py F:/nv3050-qemu/sonoma-base.qcow2
#   python tools/qcow2_zero_census.py IMAGE --max-clusters 2000    (smoke run)
#
# Exit status: 0 = full or partial census with no decode errors, 1 = decode
# errors were counted, 2 = the image was refused or could not be read.

import argparse
import struct
import sys
import time
import zlib

MAGIC = bytes([0x51, 0x46, 0x49, 0xFB])  # 'QFI' followed by 0xfb
OFFSET_MASK = 0x00FFFFFFFFFFFE00         # bits 9-55: L2 table / standard cluster offset
FLAG_COMPRESSED = 1 << 62
FLAG_ZERO = 1                            # L2 bit 0, qcow2 v3 only
INCOMPAT_CORRUPT = 1 << 1
INCOMPAT_EXTERNAL_DATA = 1 << 2
INCOMPAT_EXTENDED_L2 = 1 << 4
SMALL_BLOCK = 4096                       # qemu-img convert default -S
READ_WINDOW = 8 << 20


def die(msg):
    print('qcow2_zero_census: error: ' + msg, file=sys.stderr)
    sys.exit(2)


def read_exact(f, offset, length):
    f.seek(offset)
    data = f.read(length)
    if len(data) != length:
        die('short read: wanted %d bytes at offset %d, got %d' % (length, offset, len(data)))
    return data


def gib(n):
    return '%d bytes (%.2f GiB)' % (n, n / 2.0 ** 30)


def parse_header(f):
    h = read_exact(f, 0, 72)
    if h[:4] != MAGIC:
        die('not a qcow2 image (bad magic)')
    (version, backing_off, _backing_len, cluster_bits, virtual_size, crypt,
     l1_size, l1_off, _rc_off, _rc_clusters, nb_snapshots, _snap_off) = struct.unpack('>IQIIQIIQQIIQ', h[4:72])
    if version not in (2, 3):
        die('unsupported qcow2 version %d' % version)
    incompat = 0
    compression_type = 0
    if version == 3:
        incompat, _compat, _autoclear, _rc_order, header_len = struct.unpack('>QQQII', read_exact(f, 72, 32))
        if header_len > 104:
            compression_type = read_exact(f, 104, 1)[0]
    if backing_off:
        die('image has a backing file, so unallocated clusters would not read as zero')
    if crypt:
        die('encrypted images are not supported')
    if not 9 <= cluster_bits <= 21:
        die('implausible cluster_bits %d' % cluster_bits)
    if incompat & INCOMPAT_CORRUPT:
        die('image header has the corrupt bit set')
    if incompat & INCOMPAT_EXTERNAL_DATA:
        die('image keeps its data in an external file')
    if incompat & INCOMPAT_EXTENDED_L2:
        die('extended L2 entries (subclusters) are not supported')
    if compression_type != 0:
        die('compression type %d is not supported (only zlib = 0)' % compression_type)
    return dict(version=version, cluster_bits=cluster_bits, virtual_size=virtual_size,
                l1_size=l1_size, l1_off=l1_off, nb_snapshots=nb_snapshots)


def collect(f, hdr):
    cluster_bits = hdr['cluster_bits']
    cs = 1 << cluster_bits
    l2_entries = cs // 8
    total = (hdr['virtual_size'] + cs - 1) // cs
    x = 62 - (cluster_bits - 8)            # compressed descriptor: host offset in bits 0..x-1
    coff_mask = (1 << x) - 1
    csec_mask = (1 << (62 - x)) - 1        # bits x..61: additional 512-byte sectors
    l1 = struct.unpack('>%dQ' % hdr['l1_size'], read_exact(f, hdr['l1_off'], 8 * hdr['l1_size']))
    st = dict(unallocated=0, zero_flag=0, zero_flag_with_host=0, standard=0, compressed=0)
    records = []                           # (host_offset, host_length, is_compressed)
    covered = 0
    for i, e1 in enumerate(l1):
        first = i * l2_entries
        if first >= total:
            break
        n = min(l2_entries, total - first)
        covered += n
        l2_off = e1 & OFFSET_MASK
        if not l2_off:
            st['unallocated'] += n
            continue
        l2 = struct.unpack('>%dQ' % l2_entries, read_exact(f, l2_off, cs))
        for e2 in l2[:n]:
            if e2 & FLAG_COMPRESSED:
                off = e2 & coff_mask
                nsec = (e2 >> x) & csec_mask
                records.append((off, (nsec + 1) * 512 - (off & 511), True))
                st['compressed'] += 1
            elif hdr['version'] == 3 and e2 & FLAG_ZERO:
                st['zero_flag'] += 1
                if e2 & OFFSET_MASK:
                    st['zero_flag_with_host'] += 1
            elif e2 & OFFSET_MASK:
                records.append((e2 & OFFSET_MASK, cs, False))
                st['standard'] += 1
            else:
                st['unallocated'] += 1
    st['unallocated'] += total - covered
    records.sort()
    return cs, total, st, records


def scan(f, cs, records, limit, progress_every):
    small = min(SMALL_BLOCK, cs)
    zero_cluster = bytes(cs)
    zero_small = bytes(small)
    r = dict(scanned=0, zero=0, zero_host=0, nonzero=0, nonzero_host=0,
             zero_small_in_nonzero=0, errors=0, host_read=0, small=small)
    win = b''
    win_start = win_end = 0
    count = min(limit, len(records)) if limit > 0 else len(records)
    t0 = last = time.time()
    for idx in range(count):
        off, length, compressed = records[idx]
        if off < win_start or off + length > win_end:
            f.seek(off)
            win = f.read(max(READ_WINDOW, length))
            win_start = off
            win_end = off + len(win)
            r['host_read'] += len(win)
        buf = win[off - win_start:off - win_start + length]
        if compressed:
            d = zlib.decompressobj(-15)    # qcow2 zlib clusters are raw deflate streams
            try:
                out = d.decompress(buf, cs)
            except zlib.error:
                r['errors'] += 1
                continue
            host = len(buf) - len(d.unconsumed_tail) - len(d.unused_data)
        else:
            out = buf
            host = len(buf)
        if len(out) != cs:
            r['errors'] += 1
            continue
        r['scanned'] += 1
        if out == zero_cluster:
            r['zero'] += 1
            r['zero_host'] += host
        else:
            r['nonzero'] += 1
            r['nonzero_host'] += host
            for k in range(0, cs, small):
                if out[k:k + small] == zero_small:
                    r['zero_small_in_nonzero'] += 1
        if progress_every > 0:
            now = time.time()
            if now - last >= progress_every:
                last = now
                done = idx + 1
                rate = done / max(now - t0, 1e-9)
                print('progress: %d/%d clusters (%.1f%%), %.2f GiB read, %.0f s elapsed, ~%.0f s left'
                      % (done, count, 100.0 * done / count, r['host_read'] / 2.0 ** 30,
                         now - t0, (count - done) / rate), file=sys.stderr, flush=True)
    r['attempted'] = count
    r['seconds'] = time.time() - t0
    return r


def main():
    ap = argparse.ArgumentParser(
        description='Read-only census of the clusters a qcow2 image stores: which decode to '
                    'all-zero bytes (droppable by zero detection) and which hold non-zero data.')
    ap.add_argument('image')
    ap.add_argument('--max-clusters', type=int, default=0,
                    help='decode only the first N allocated clusters in host order (smoke run); '
                         'the summary is then marked PARTIAL')
    ap.add_argument('--progress', type=float, default=15.0,
                    help='seconds between progress lines on stderr (0 disables)')
    args = ap.parse_args()

    try:
        f = open(args.image, 'rb')
    except OSError as e:
        die('cannot open %s: %s' % (args.image, e))
    with f:
        f.seek(0, 2)
        file_len = f.tell()
        hdr = parse_header(f)
        cs, total, st, records = collect(f, hdr)
        r = scan(f, cs, records, args.max_clusters, args.progress)

    allocated = st['standard'] + st['compressed']
    partial = r['attempted'] < len(records)
    tag = 'PARTIAL ' if partial else ''
    print('image: %s' % args.image)
    print('file_length: %s' % gib(file_len))
    print('qcow2_version: %d  cluster_size: %d  virtual_size: %s'
          % (hdr['version'], cs, gib(hdr['virtual_size'])))
    if hdr['nb_snapshots']:
        print('WARNING: %d internal snapshot(s); only the active L1 table was scanned' % hdr['nb_snapshots'])
    print('guest_clusters: %d' % total)
    print('  unallocated: %d' % st['unallocated'])
    print('  zero_flag: %d (%d still hold a host cluster)' % (st['zero_flag'], st['zero_flag_with_host']))
    print('  allocated: %d (standard %d, compressed %d) = %s of guest data'
          % (allocated, st['standard'], st['compressed'], gib(allocated * cs)))
    print('%sdecoded: %d of %d allocated clusters in %.0f s (%d attempted)'
          % (tag, r['scanned'], allocated, r['seconds'], r['attempted']))
    print('  all_zero_clusters: %d  guest %s  host %s' % (r['zero'], gib(r['zero'] * cs), gib(r['zero_host'])))
    print('  nonzero_clusters: %d  guest %s  host %s' % (r['nonzero'], gib(r['nonzero'] * cs), gib(r['nonzero_host'])))
    print('  zero_%d_byte_blocks_inside_nonzero_clusters: %d = %s (a qcow2 target cannot drop these at %d-byte clusters)'
          % (r['small'], r['zero_small_in_nonzero'], gib(r['zero_small_in_nonzero'] * r['small']), cs))
    print('  decode_errors: %d' % r['errors'])
    if not partial:
        print('host_bytes_outside_decoded_clusters: %s (metadata, padding, leaked space; compressed sizes are +-a few bytes each)'
              % gib(file_len - r['zero_host'] - r['nonzero_host']))
    print('%sverdict: a zero-detecting rewrite can drop at most %s of host data (%.2f%% of the file). '
          '%s of guest data is non-zero (live files plus any blocks the guest freed without zeroing); '
          'only a discard or zero-fill inside the guest can reclaim the freed part.'
          % (tag, gib(r['zero_host']), 100.0 * r['zero_host'] / file_len, gib(r['nonzero'] * cs)))
    return 1 if r['errors'] else 0


if __name__ == '__main__':
    sys.exit(main())
