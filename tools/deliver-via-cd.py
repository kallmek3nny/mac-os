#!/usr/bin/env python3
"""
deliver-via-cd.py -- move host files into the macOS guest without a guest login.

Why this exists
---------------
``handoff3.md`` records the state this works around: the Sonoma install is
finished, and every ``vmrun`` guest operation is blocked on authentication.
``kenny`` is settled as the account short name (see the ``.vmem`` scan), the
password is not, and a real user and a nonexistent one return the identical
``Invalid user name or password`` at exit 127 -- so the error text is not a
username oracle and brute-force probing produces no information.

A CD is the one delivery path that needs no credential at all. The virtual
optical drive is emulated by the *host* VMware process; the guest kernel mounts
whatever is behind it. ``Sonoma.vmx`` line 134-137 already declares a real
``cdrom-image`` device::

    sata0:2.present      = "TRUE"
    sata0:2.fileName     = "C:\\Program Files (x86)\\VMware\\VMware Workstation\\darwin.iso"
    sata0:2.deviceType   = "cdrom-image"
    sata0:2.startConnected = "TRUE"

So no device has to be *added* -- only repointed. That matters, because adding a
SATA device changes ``pciSlotNumber`` allocation, and this project has already
been bitten by miscounting VMX keys with an anchored regex.

What this does NOT claim
------------------------
Writing the ISO and repointing the VMX is a *host-side* claim, and that is the
only claim this tool makes. It does not prove the guest mounted the volume. The
guest side is observable only through the framebuffer, and per
``read-the-artifact-not-the-verdict``, an OCR substring test is not that proof --
open the PNG. ``--verify`` re-reads the ISO; it says nothing about the guest.

Also note: repointing takes effect on the next **power cycle**, or on a manual
reconnect from the VM's settings UI. Editing the VMX under a running VM edits a
file VMware may rewrite from memory on suspend -- the authored/runtime boundary
in this VMX is unlabelled (authored ends at ``uuid.bios``, VMware's write-back
begins at ``uuid.location``), so this tool refuses to edit while the VM is on
unless ``--force`` is given.

Usage
-----
::

    # pack files, write the ISO, and repoint sata0:2 at it
    python tools/deliver-via-cd.py --add notes.txt --add build/NV3050Probe.kext.tar \\
        --out C:/Users/Kentl/macos-vm/sonoma/payload.iso \\
        --vmx C:/Users/Kentl/macos-vm/sonoma/Sonoma.vmx

    # build the ISO only, touch no VMX
    python tools/deliver-via-cd.py --add notes.txt --out payload.iso --no-vmx

    # read an ISO back and print what a guest would see
    python tools/deliver-via-cd.py --verify payload.iso

    # self-check, no VM and no network required
    python tools/deliver-via-cd.py --self-test

Exit codes::

    0   success
    1   a check failed (bad path, VM running, unreadable image)
    2   pycdlib missing

Requires: pycdlib. It exposes no ``__version__`` -- check with ``pip show
pycdlib``, not ``import pycdlib; pycdlib.__version__``.
"""

import argparse
import os
import shutil
import contextlib
import sys
import tempfile
import time

# Joliet is what makes the guest see "NV3050Probe.kext.tar" instead of the
# 8.3-mangled "NV3050PR.TAR". macOS mounts Joliet; without it every long name
# arrives truncated and the failure surfaces inside the guest, after a reboot.
JOLIET_LEVEL = 3
ROCK_RIDGE = "1.09"

DEFAULT_VOL_IDENT = "PAYLOAD"
DEFAULT_DEVICE = "sata0:2"

# A volume identifier is 32 bytes of d-characters in the PVD. Longer silently
# truncates on write, which would make --verify disagree with --add for reasons
# that look like a pycdlib bug.
MAX_VOL_IDENT = 32

# Guard against pointing --add at a directory tree or a 15 GB installer by
# accident. This tool is for small payloads; the big-installer path is
# build-installassistant-iso.py, which streams instead of buffering.
PLAUSIBLE_TOTAL_BYTES = 512 * 1024 * 1024


def load_pycdlib():
    """Import pycdlib, or explain the one command that fixes its absence."""
    try:
        import pycdlib
    except ImportError:
        raise RuntimeError("pycdlib is required: pip install pycdlib")
    return pycdlib


def iso_names(name):
    """Return (iso9660_path, joliet_path) for a host file basename.

    The ISO-9660 name must be uppercase 8.3 with a version suffix; the Joliet
    name carries the real one. Returning both together keeps them from drifting
    apart, which is how a file ends up readable under one tree and not the
    other.
    """
    base = os.path.basename(name)
    stem, ext = os.path.splitext(base)

    keep = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
    short = "".join(c if c in keep else "_" for c in stem.upper())[:8] or "FILE"
    suffix = "".join(c if c in keep else "_" for c in ext.upper().lstrip("."))[:3]

    if suffix:
        return ("/%s.%s;1" % (short, suffix), "/" + base)
    return ("/%s.;1" % short, "/" + base)


def plan_entries(paths):
    """Resolve `paths` to [(host_path, iso_path, joliet_path, size)].

    Raises on a missing path, a directory, or an 8.3 collision. The collision
    check is the one that matters: two files whose first eight characters agree
    -- ``NV3050Probe.hpp`` and ``NV3050ProbeShim.hpp`` -- both mangle to
    ``NV3050PR.HPP;1``, and pycdlib would either raise deep inside the write or
    silently keep one. Catching it here names both offenders.
    """
    entries = []
    seen_iso = {}
    seen_joliet = {}
    total = 0

    for path in paths:
        if not os.path.exists(path):
            raise RuntimeError("no such file: %s" % path)
        if os.path.isdir(path):
            raise RuntimeError("directories are not supported, archive it first: %s" % path)

        iso_path, joliet_path = iso_names(path)

        if iso_path in seen_iso:
            raise RuntimeError(
                "ISO-9660 name collision: %s and %s both become %s"
                % (seen_iso[iso_path], path, iso_path))
        if joliet_path in seen_joliet:
            raise RuntimeError(
                "duplicate basename: %s and %s" % (seen_joliet[joliet_path], path))

        size = os.path.getsize(path)
        total += size
        seen_iso[iso_path] = path
        seen_joliet[joliet_path] = path
        entries.append((path, iso_path, joliet_path, size))

    if not entries:
        raise RuntimeError("nothing to deliver: pass at least one --add")
    if total > PLAUSIBLE_TOTAL_BYTES:
        raise RuntimeError(
            "payload is %d bytes, over the %d-byte sanity ceiling; "
            "use build-installassistant-iso.py for installer-sized images"
            % (total, PLAUSIBLE_TOTAL_BYTES))

    return entries


# ISO-9660 stores a wall clock in the PVD and in EVERY directory record, and
# pycdlib reads that clock itself -- headervd.py:307/326/423 and
# pycdlib.py:1894/2348 each call time.time(). So two builds from byte-identical
# inputs produce images that differ.
#
# MEASURED 2026-08-16 on build-payload/step7-payload.iso: two consecutive
# rebuilds were both 303104 bytes and differed in 34 bytes across 28 runs --
# 0x24 (36) vs 0x36 (54) in the seconds field of binary directory records, and
# the ASCII '36'/'54' at offset 12 of the PVD date strings. The tar inside was
# identical both times. Nothing but the timestamps moved.
#
# That makes an md5 useless as a fingerprint, which is exactly the property
# OPERATOR-ACTION-step7-build.md and tests/test_operator_step7_doc.py rest on.
# A byte size does not substitute: tar pads to a 10240-byte multiple, so the
# content can change with the size fixed (see the FIFTH-rebuild note in the
# card, where both size tests passed on a stale card and only the md5 failed).
#
# Pinning time.time() alone is NOT sufficient. Both date encoders --
# dates.py:111 and dates.py:264 -- convert through time.localtime(tm), and
# utils.gmtoffset_from_tm() derives the stored GMT offset from that, so a pinned
# epoch still yields a different image under a different TZ. localtime is
# therefore pointed at gmtime for the duration, which fixes the offset field at
# 0 too.
PINNED_ISO_EPOCH = 1786838400          # 2026-08-16T00:00:00Z, chosen once and frozen


@contextlib.contextmanager
def pinned_iso_clock(epoch=PINNED_ISO_EPOCH):
    """Make pycdlib's timestamping deterministic for the duration of the block.

    Patches the `time` module rather than passing an argument, because pycdlib
    exposes no hook -- every clock read is an internal `time.time()`. The scope
    is one build_iso() call inside a short-lived CLI, so the reach is bounded,
    but it IS process-global: nothing inside the block may depend on the real
    clock. Both names are restored in a finally, including on exception.
    """
    real_time, real_localtime = time.time, time.localtime
    time.time = lambda: float(epoch)
    time.localtime = time.gmtime
    try:
        yield epoch
    finally:
        time.time = real_time
        time.localtime = real_localtime


def build_iso(entries, out_path, vol_ident=DEFAULT_VOL_IDENT):
    """Write an ISO at `out_path` carrying `entries` from plan_entries().

    Joliet *and* Rock Ridge are both enabled. macOS will mount either, but the
    two carry the long name independently, and a reader that prefers one over
    the other should not see a different filename.

    The write runs under pinned_iso_clock(), so the same entries produce the
    same bytes -- see that function for what was measured before the pin.
    """
    pycdlib = load_pycdlib()

    if len(vol_ident) > MAX_VOL_IDENT:
        raise RuntimeError(
            "volume identifier is %d chars, max %d" % (len(vol_ident), MAX_VOL_IDENT))

    with pinned_iso_clock():
        iso = pycdlib.PyCdlib()
        iso.new(joliet=JOLIET_LEVEL, rock_ridge=ROCK_RIDGE, vol_ident=vol_ident)
        try:
            for host_path, iso_path, joliet_path, _size in entries:
                iso.add_file(host_path,
                             iso_path=iso_path,
                             joliet_path=joliet_path,
                             rr_name=os.path.basename(host_path))
            iso.write(out_path)
        finally:
            iso.close()

    return out_path


def read_back(iso_path):
    """Reopen `iso_path` and return (vol_ident, {joliet_name: size}).

    Building an image that cannot be read is the failure this guards. The
    Joliet names are UTF-16BE on disk; decoding them is how you confirm the
    guest sees ``NV3050Probe.kext.tar`` rather than ``NV3050PR.TAR``.
    """
    pycdlib = load_pycdlib()

    iso = pycdlib.PyCdlib()
    iso.open(iso_path)
    try:
        ident = iso.pvd.volume_identifier
        if isinstance(ident, bytes):
            ident = ident.decode("ascii", "replace")
        ident = ident.strip()

        entries = {}
        for child in iso.list_children(joliet_path="/"):
            if child is None or child.is_dot() or child.is_dotdot():
                continue
            name = child.file_identifier()
            if isinstance(name, bytes):
                name = name.decode("utf-16_be", "replace")
            entries[name] = child.get_data_length()
    finally:
        iso.close()

    return ident, entries


# --- VMX side -------------------------------------------------------------
#
# Everything below edits Sonoma.vmx. Two hazards, both already recorded in this
# project's notes:
#
#   * An anchored regex like '^[a-zA-Z_.]*pciSlotNumber' finds 2 of 9 keys
#     because the character class excludes digits. So key matching here splits
#     on '=' and compares the stripped left side literally -- no regex.
#   * The authored/runtime boundary is unlabelled. Authored content ends at
#     'uuid.bios'; VMware's write-back begins at 'uuid.location'. Repointing an
#     authored key in place preserves that ordering; appending a duplicate at
#     the end would land in VMware's region and be silently overwritten.

# Lock/state artifact suffixes, matched against every name in the VM directory
# rather than against the VMX stem. See vm_appears_running() for why the stem
# is not a reliable predictor of these names. All entries must be lowercase:
# the comparison lowercases the candidate, not the suffix.
VM_DIR_LOCK_SUFFIXES = (".lck", ".vmem", ".vmss")

# Retained for the stem-relative reading, which is what a VM whose artifacts
# ARE named after its VMX would produce. Kept so the historical shape of the
# check is still expressible in tests; vm_appears_running() no longer uses it.
VMX_LOCK_SUFFIXES = (".vmx.lck", ".vmem", ".vmss")


def parse_vmx(text):
    """Return [(raw_line, key_or_None, value_or_None)] for every line.

    Comments and blanks come back with key None so a rewrite can put them back
    byte-for-byte. A '#'-prefixed line is never treated as a key: this VMX
    documents four failed attach attempts in comments that mention the very
    keys being edited.
    """
    out = []
    for raw in text.splitlines():
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            out.append((raw, None, None))
            continue
        if "=" not in raw:
            out.append((raw, None, None))
            continue
        left, right = raw.split("=", 1)
        key = left.strip()
        value = right.strip().strip('"')
        out.append((raw, key, value))
    return out


def find_device(parsed, device=DEFAULT_DEVICE):
    """Return {suffix: (line_index, value)} for one device's keys.

    Suffix is the part after the device prefix -- 'present', 'fileName',
    'deviceType', 'startConnected'. Absence of a suffix is reported by its
    absence from the dict, never as an empty string, so a caller cannot confuse
    "declared empty" with "not declared".
    """
    prefix = device + "."
    found = {}
    for index, (_raw, key, value) in enumerate(parsed):
        if key is None:
            continue
        if key.startswith(prefix):
            found[key[len(prefix):]] = (index, value)
    return found


def check_device_usable(found, device=DEFAULT_DEVICE):
    """Raise unless `found` describes a cdrom-image device this tool can repoint.

    Repointing a *disk* at an ISO is the dangerous mistake here: sata0:0 and
    sata0:1 are both deviceType 'disk' and both carry real VMDKs. Overwriting
    either fileName detaches the installed system.
    """
    if not found:
        raise RuntimeError(
            "%s is not declared in this VMX; this tool only repoints an "
            "existing device" % device)

    if "deviceType" not in found:
        raise RuntimeError("%s has no deviceType line" % device)

    dev_type = found["deviceType"][1]
    if dev_type != "cdrom-image":
        raise RuntimeError(
            "%s has deviceType %r, refusing: only cdrom-image is safe to "
            "repoint (a 'disk' carries a real VMDK)" % (device, dev_type))

    if "fileName" not in found:
        raise RuntimeError("%s has no fileName line to repoint" % device)

    return True


def vm_appears_running(vmx_path):
    """Return a list of lock/state artifacts that suggest the VM is powered on.

    This is deliberately named 'appears'. A .lck directory, a .vmem or a .vmss
    beside the VMX is host-side evidence of a running or suspended VM; their
    absence is weaker evidence of the opposite, since a crashed process can
    leave stale locks. Empty list means "nothing found", never "proven off" --
    ``vmrun list`` is the only positive test for powered-off.

    WHY THE WHOLE DIRECTORY IS SCANNED AND NOT JUST THE VMX STEM
    ------------------------------------------------------------
    The first version of this function built candidates as ``stem + suffix``,
    so against ``Sonoma.vmx`` it looked for exactly three paths:
    ``Sonoma.vmx.lck``, ``Sonoma.vmem``, ``Sonoma.vmss``. On 2026-08-11 it
    returned ``[]`` for a VM that ``vmrun list`` reported as running, and this
    tool went on to rewrite the live ``Sonoma.vmx`` without refusing. None of
    those three paths exist in that directory. The artifacts VMware actually
    created are named after the VM's *UUID*:

        564d3ffb-d11c-edd5-265a-225898e79f2b.vmem
        564d3ffb-d11c-edd5-265a-225898e79f2b.vmem.lck/
        Sonoma.vmdk.lck/  BaseSystem.vmdk.lck/

    so the VMX stem does not predict the artifact name and the suffix does.
    Over-reporting here is safe -- the caller can pass ``--force`` -- while
    under-reporting silently defeats the whole guard, which is the failure that
    was observed. A stale ``.lck`` left by a crashed process will therefore
    report as "appears running"; that is the intended bias.
    """
    directory = os.path.dirname(os.path.abspath(vmx_path))

    artifacts = []
    try:
        names = sorted(os.listdir(directory))
    except OSError:
        return artifacts

    for name in names:
        if name.lower().endswith(VM_DIR_LOCK_SUFFIXES):
            artifacts.append(os.path.join(directory, name))
    return artifacts


def repoint_vmx(text, iso_path, device=DEFAULT_DEVICE):
    """Return (new_text, old_filename) with `device`.fileName pointing at the ISO.

    Only the fileName line is rewritten, and it is rewritten *in place* so it
    stays in the authored region. startConnected is forced TRUE if it is
    declared FALSE -- a repointed drive that is not connected at power-on
    delivers nothing, and that failure looks identical to a guest that ignored
    the disc.
    """
    parsed = parse_vmx(text)
    found = find_device(parsed, device)
    check_device_usable(found, device)

    lines = [raw for raw, _key, _value in parsed]

    name_index, old_filename = found["fileName"]
    lines[name_index] = '%s.fileName = "%s"' % (device, iso_path.replace("\\", "\\\\"))

    if "startConnected" in found:
        conn_index, conn_value = found["startConnected"]
        if conn_value.upper() != "TRUE":
            lines[conn_index] = '%s.startConnected = "TRUE"' % device

    # VMX files are CRLF in practice and VMware writes them that way. Preserving
    # that is not cosmetic: this project already has a gate for byte-offset
    # sensitivity in batch files, and there is no reason to be the first tool
    # here to emit bare LF into a Windows config VMware rewrites.
    return "\r\n".join(lines) + "\r\n", old_filename


def verify_repoint(vmx_path, iso_path, device=DEFAULT_DEVICE):
    """Re-read `vmx_path` from disk and confirm `device`.fileName is `iso_path`.

    Returns (ok, actual_filename_or_None).

    This exists because of an observed failure. A run on 2026-08-10 left
    ``Sonoma.vmx.bak`` on disk at 22:14:28 while ``Sonoma.vmx`` kept its
    2026-08-09 mtime and its old ``darwin.iso`` fileName. The backup is written
    *before* the rewrite, so its presence proves only that the tool got as far
    as copying -- never that the edit landed. Nothing checked, and the success
    message printed unconditionally after the write.

    Comparison is on the normalised path because the VMX stores doubled
    backslashes, so a literal string compare would fail on a correct edit.
    """
    with open(vmx_path, "r", encoding="windows-1252", errors="replace") as handle:
        found = find_device(parse_vmx(handle.read()), device)

    if "fileName" not in found:
        return False, None

    actual = found["fileName"][1].replace("\\\\", "\\")
    expected = os.path.abspath(iso_path)

    same = (os.path.normcase(os.path.normpath(actual))
            == os.path.normcase(os.path.normpath(expected)))
    return same, actual


# --- self-test ------------------------------------------------------------

SAMPLE_VMX = "\r\n".join([
    '.encoding = "windows-1252"',
    '# Attempt 3 attached BaseSystem.dmg with deviceType = "cdrom-image". The VM',
    '# refused to boot from it, so sata0:0 is a disk now.',
    'sata0:0.present = "TRUE"',
    'sata0:0.fileName = "..\\\\sonoma\\\\BaseSystem.vmdk"',
    'sata0:0.deviceType = "disk"',
    'sata0:0.startConnected = "TRUE"',
    'sata0:1.present = "TRUE"',
    'sata0:1.fileName = "Sonoma.vmdk"',
    'sata0:1.deviceType = "disk"',
    'sata0:1.startConnected = "TRUE"',
    'sata0:2.present = "TRUE"',
    'sata0:2.fileName = "C:\\\\Program Files (x86)\\\\VMware\\\\VMware Workstation\\\\darwin.iso"',
    'sata0:2.deviceType = "cdrom-image"',
    'sata0:2.startConnected = "FALSE"',
    'uuid.bios = "56 4d 1f aa"',
    'uuid.location = "56 4d 1f aa"',
]) + "\r\n"


def self_test():
    """Exercise every check without a VM, a network, or the real VMX.

    Returns 0 on success, 1 on the first failure, and prints one line per
    assertion so a pass is a count and not a bare exit code.
    """
    checks = 0
    failures = 0

    def check(label, condition):
        nonlocal checks, failures
        checks += 1
        if condition:
            print("  ok   %s" % label)
        else:
            failures += 1
            print("  FAIL %s" % label)

    def raises(label, fn, needle):
        nonlocal checks, failures
        checks += 1
        try:
            fn()
        except RuntimeError as exc:
            if needle in str(exc):
                print("  ok   %s" % label)
                return
            failures += 1
            print("  FAIL %s -- wrong message: %s" % (label, exc))
            return
        except Exception as exc:  # noqa: BLE001 - a non-RuntimeError is a failure
            failures += 1
            print("  FAIL %s -- wrong type: %r" % (label, exc))
            return
        failures += 1
        print("  FAIL %s -- no exception raised" % label)

    print("iso_names")
    check("long name keeps its Joliet form",
          iso_names("NV3050Probe.kext.tar")[1] == "/NV3050Probe.kext.tar")
    check("long name mangles to 8.3 for ISO-9660",
          iso_names("NV3050Probe.kext.tar")[0] == "/NV3050PR.TAR;1")
    check("lowercase is upcased", iso_names("notes.txt")[0] == "/NOTES.TXT;1")
    check("illegal chars become underscores",
          iso_names("my-file.txt")[0] == "/MY_FILE.TXT;1")
    check("extensionless name keeps the version suffix",
          iso_names("README")[0] == "/README.;1")
    check("a host directory prefix is stripped",
          iso_names("C:/tmp/notes.txt")[1] == "/notes.txt")

    print("parse_vmx")
    parsed = parse_vmx(SAMPLE_VMX)
    keys = [k for _r, k, _v in parsed if k is not None]
    # Counted by listing SAMPLE_VMX, not by eye: 17 lines, 2 comment lines,
    # 15 keys. A hand-counted expectation is exactly how this project already
    # undercounted pciSlotNumber keys 2-of-9, so the count is asserted against
    # an explicit enumeration below rather than a bare literal.
    expected_keys = [
        ".encoding",
        "sata0:0.present", "sata0:0.fileName", "sata0:0.deviceType",
        "sata0:0.startConnected",
        "sata0:1.present", "sata0:1.fileName", "sata0:1.deviceType",
        "sata0:1.startConnected",
        "sata0:2.present", "sata0:2.fileName", "sata0:2.deviceType",
        "sata0:2.startConnected",
        "uuid.bios", "uuid.location",
    ]
    check("every non-comment key is found (17 lines, 2 comments, 15 keys)",
          keys == expected_keys)
    check("the two comment lines are preserved as non-keys",
          len(parsed) == 17 and len(keys) == 15)
    check("a comment mentioning cdrom-image is not a key",
          not any(k.startswith("# ") for k in keys))
    check("values are unquoted",
          dict((k, v) for _r, k, v in parsed if k)["sata0:1.fileName"] == "Sonoma.vmdk")

    print("find_device")
    found = find_device(parsed, "sata0:2")
    check("all four sata0:2 suffixes located", sorted(found) ==
          ["deviceType", "fileName", "present", "startConnected"])
    check("sata0:2 does not pick up sata0:1 keys",
          found["fileName"][1].endswith("darwin.iso"))
    check("an undeclared device yields an empty dict",
          find_device(parsed, "sata0:3") == {})

    print("check_device_usable")
    check("the real cdrom-image device passes",
          check_device_usable(found, "sata0:2") is True)
    raises("a disk device is refused",
           lambda: check_device_usable(find_device(parsed, "sata0:1"), "sata0:1"),
           "refusing")
    raises("an undeclared device is refused",
           lambda: check_device_usable(find_device(parsed, "sata0:9"), "sata0:9"),
           "not declared")

    print("repoint_vmx")
    new_text, old_name = repoint_vmx(SAMPLE_VMX, "C:/Users/Kentl/macos-vm/sonoma/payload.iso")
    check("the old filename is reported back", old_name.endswith("darwin.iso"))
    check("the new filename is present", "payload.iso" in new_text)
    check("the old filename is gone", "darwin.iso" not in new_text)
    check("startConnected FALSE was forced to TRUE",
          'sata0:2.startConnected = "TRUE"' in new_text)
    check("sata0:1 was not touched",
          'sata0:1.fileName = "Sonoma.vmdk"' in new_text)
    check("sata0:0 was not touched", 'sata0:0.deviceType = "disk"' in new_text)
    check("the comment block survived", "# Attempt 3 attached" in new_text)
    check("output is CRLF", "\r\n" in new_text)
    check("output has no bare LF", "\n" not in new_text.replace("\r\n", ""))
    check("line count is unchanged (in-place edit, no append)",
          len(new_text.rstrip("\r\n").split("\r\n"))
          == len(SAMPLE_VMX.rstrip("\r\n").split("\r\n")))
    check("fileName stayed above uuid.bios (authored region)",
          new_text.index("payload.iso") < new_text.index("uuid.bios"))

    print("plan_entries")
    tmpdir = tempfile.mkdtemp(prefix="deliver-cd-selftest-")
    try:
        one = os.path.join(tmpdir, "notes.txt")
        with open(one, "wb") as handle:
            handle.write(b"hello from the host\n")

        two = os.path.join(tmpdir, "NV3050Probe.kext.tar")
        with open(two, "wb") as handle:
            handle.write(b"x" * 4096)

        entries = plan_entries([one, two])
        check("two files planned", len(entries) == 2)
        check("sizes come from the filesystem",
              sorted(e[3] for e in entries) == [20, 4096])

        raises("a missing file is refused",
               lambda: plan_entries([os.path.join(tmpdir, "absent.txt")]),
               "no such file")
        raises("a directory is refused", lambda: plan_entries([tmpdir]),
               "directories are not supported")
        raises("an empty list is refused", lambda: plan_entries([]),
               "nothing to deliver")

        collide_a = os.path.join(tmpdir, "NV3050Probe.hpp")
        collide_b = os.path.join(tmpdir, "NV3050ProbeShim.hpp")
        for path in (collide_a, collide_b):
            with open(path, "wb") as handle:
                handle.write(b"//\n")
        raises("an 8.3 collision names both offenders",
               lambda: plan_entries([collide_a, collide_b]),
               "collision")

        print("build_iso / read_back")
        iso_path = os.path.join(tmpdir, "payload.iso")
        build_iso(plan_entries([one, two]), iso_path, vol_ident="PAYLOAD")
        check("the ISO exists and is non-empty", os.path.getsize(iso_path) > 0)

        ident, back = read_back(iso_path)
        check("volume identifier round-trips", ident == "PAYLOAD")
        check("the guest would see the long name, not the 8.3 name",
              "NV3050Probe.kext.tar" in back)
        check("the second long name round-trips", "notes.txt" in back)
        check("sizes survive the round trip", back.get("notes.txt") == 20)
        check("the mangled name is not what Joliet reports",
              "NV3050PR.TAR" not in back)

        raises("an over-long volume identifier is refused",
               lambda: build_iso(plan_entries([one]), os.path.join(tmpdir, "x.iso"),
                                 vol_ident="A" * 40),
               "max 32")

        print("vm_appears_running")
        fake_vmx = os.path.join(tmpdir, "Sonoma.vmx")
        with open(fake_vmx, "w", newline="\r\n") as handle:
            handle.write(SAMPLE_VMX)
        check("no lock artifacts beside a bare VMX",
              vm_appears_running(fake_vmx) == [])
        with open(os.path.join(tmpdir, "Sonoma.vmem"), "wb") as handle:
            handle.write(b"\0")
        check("a .vmem beside the VMX is reported",
              any(p.endswith(".vmem") for p in vm_appears_running(fake_vmx)))

        # REGRESSION, 2026-08-11. The stem-relative version of this check
        # returned [] for the live Sonoma VM while vmrun reported it running,
        # and the tool then rewrote a live VMX. The artifacts are UUID-named,
        # so nothing here shares the VMX stem. This is the negative control:
        # if it passes with a stem-relative implementation, the guard is back
        # to the shape that failed.
        uuid_dir = os.path.join(tmpdir, "uuidnames")
        os.mkdir(uuid_dir)
        uuid_vmx = os.path.join(uuid_dir, "Sonoma.vmx")
        with open(uuid_vmx, "w", newline="\r\n") as handle:
            handle.write(SAMPLE_VMX)
        uuid_stem = "564d3ffb-d11c-edd5-265a-225898e79f2b"
        with open(os.path.join(uuid_dir, uuid_stem + ".vmem"), "wb") as handle:
            handle.write(b"\0")
        os.mkdir(os.path.join(uuid_dir, uuid_stem + ".vmem.lck"))
        os.mkdir(os.path.join(uuid_dir, "Sonoma.vmdk.lck"))
        uuid_found = vm_appears_running(uuid_vmx)
        check("a UUID-named .vmem is reported even though it shares no stem",
              any(p.endswith(uuid_stem + ".vmem") for p in uuid_found))
        check("a UUID-named .vmem.lck directory is reported",
              any(p.endswith(uuid_stem + ".vmem.lck") for p in uuid_found))
        check("a .vmdk.lck is reported (no Sonoma.vmx.lck exists here)",
              any(p.endswith("Sonoma.vmdk.lck") for p in uuid_found))
        check("the stem-relative reading would have found none of them",
              not any(os.path.exists(os.path.join(uuid_dir, "Sonoma" + s))
                      for s in VMX_LOCK_SUFFIXES))

        # REGRESSION, 2026-08-11. A run left Sonoma.vmx.bak on disk while
        # Sonoma.vmx kept its old mtime and its old darwin.iso fileName. The
        # backup is written BEFORE the rewrite, so its presence proves only
        # that the tool got as far as copying. Nothing re-read the file, and
        # the success message printed unconditionally. These checks are the
        # separating pair: an un-rewritten VMX must report False.
        print("verify_repoint")
        vdir = os.path.join(tmpdir, "verify")
        os.mkdir(vdir)
        target_iso = os.path.join(vdir, "payload.iso")
        with open(target_iso, "wb") as handle:
            handle.write(b"\0")

        untouched = os.path.join(vdir, "Untouched.vmx")
        with open(untouched, "w", encoding="windows-1252", newline="") as handle:
            handle.write(SAMPLE_VMX)
        ok_untouched, actual_untouched = verify_repoint(untouched, target_iso)
        check("an un-rewritten VMX reports False", ok_untouched is False)
        check("and reports the filename it actually found",
              actual_untouched is not None
              and actual_untouched.endswith("darwin.iso"))

        rewritten = os.path.join(vdir, "Rewritten.vmx")
        new_vmx_text, _old = repoint_vmx(SAMPLE_VMX, os.path.abspath(target_iso))
        with open(rewritten, "w", encoding="windows-1252", newline="") as handle:
            handle.write(new_vmx_text)
        ok_rewritten, actual_rewritten = verify_repoint(rewritten, target_iso)
        check("a rewritten VMX reports True", ok_rewritten is True)
        check("the doubled backslashes are normalised before comparing",
              actual_rewritten is not None and "\\\\" not in actual_rewritten)

        # A differently-spelled but equivalent path must still verify:
        # main() passes os.path.abspath() into repoint_vmx, so the comparison
        # has to normalise both sides or a correct edit reports as a failed
        # one. Forward slashes plus a redundant "/./" segment is the cheapest
        # separating spelling.
        #
        # NOT os.path.relpath() -- tempfile.mkdtemp() lands on C: while this
        # project lives on D:, and relpath raises ValueError across drives.
        # That is a real Windows constraint, not a test artifact.
        odd_spelling = os.path.dirname(target_iso).replace(os.sep, "/") \
            + "/./" + os.path.basename(target_iso)
        ok_odd, _actual = verify_repoint(rewritten, odd_spelling)
        check("an equivalent path spelled differently still verifies",
              ok_odd is True)

        # A device with no fileName line cannot be verified either way, and
        # must not be reported as landed.
        headless = os.path.join(vdir, "Headless.vmx")
        with open(headless, "w", encoding="windows-1252", newline="") as handle:
            handle.write('sata0:2.present = "TRUE"\r\n'
                         'sata0:2.deviceType = "cdrom-image"\r\n')
        ok_headless, actual_headless = verify_repoint(headless, target_iso)
        check("a device with no fileName reports False, not True",
              ok_headless is False)
        check("and reports None rather than an empty string",
              actual_headless is None)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("")
    print("%d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


# --- CLI ------------------------------------------------------------------


def build_parser():
    parser = argparse.ArgumentParser(
        description="Deliver host files into the macOS guest over a virtual CD, "
                    "with no guest credentials.")
    parser.add_argument("--add", action="append", default=[], metavar="FILE",
                        help="a host file to place on the ISO; repeatable")
    parser.add_argument("--out", metavar="ISO",
                        help="where to write the ISO")
    parser.add_argument("--vol-ident", default=DEFAULT_VOL_IDENT,
                        help="volume identifier, max 32 chars "
                             "(default: %s)" % DEFAULT_VOL_IDENT)
    parser.add_argument("--vmx", metavar="PATH",
                        help="the VMX whose optical device should be repointed")
    parser.add_argument("--device", default=DEFAULT_DEVICE,
                        help="which optical device to repoint "
                             "(default: %s)" % DEFAULT_DEVICE)
    parser.add_argument("--no-vmx", action="store_true",
                        help="build the ISO only; touch no VMX")
    parser.add_argument("--force", action="store_true",
                        help="edit the VMX even when lock artifacts suggest the "
                             "VM is running or suspended")
    parser.add_argument("--dry-run", action="store_true",
                        help="print what would change; write nothing")
    parser.add_argument("--verify", metavar="ISO",
                        help="read an existing ISO back and print its contents")
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in checks; needs no VM")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)

    try:
        load_pycdlib()
    except RuntimeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    if args.self_test:
        return self_test()

    if args.verify:
        try:
            ident, entries = read_back(args.verify)
        except Exception as exc:  # noqa: BLE001 - pycdlib raises its own types
            print("error: cannot read %s: %s" % (args.verify, exc), file=sys.stderr)
            return 1
        print("vol ident: %s" % ident)
        for name in sorted(entries):
            print("  %-32s %d bytes" % (name, entries[name]))
        print("")
        print("This is a host-side read of the image. It does not show that the")
        print("guest mounted it -- that is only visible in the framebuffer.")
        return 0

    if not args.add:
        print("error: pass at least one --add, or use --verify / --self-test",
              file=sys.stderr)
        return 1
    if not args.out:
        print("error: --out is required when building an ISO", file=sys.stderr)
        return 1
    if not args.no_vmx and not args.vmx:
        print("error: pass --vmx PATH to repoint a device, or --no-vmx to skip",
              file=sys.stderr)
        return 1

    try:
        entries = plan_entries(args.add)
    except RuntimeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1

    # Validate the VMX side BEFORE writing the ISO. Producing an image and then
    # discovering the device is a disk leaves a stale file behind that looks
    # delivered.
    vmx_text = None
    if not args.no_vmx:
        if not os.path.exists(args.vmx):
            print("error: no such VMX: %s" % args.vmx, file=sys.stderr)
            return 1
        with open(args.vmx, "r", encoding="windows-1252", errors="replace") as handle:
            vmx_text = handle.read()
        try:
            check_device_usable(find_device(parse_vmx(vmx_text), args.device),
                               args.device)
        except RuntimeError as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 1

        artifacts = vm_appears_running(args.vmx)
        if artifacts and not args.force:
            print("error: the VM appears to be running or suspended:",
                  file=sys.stderr)
            for path in artifacts:
                print("         %s" % path, file=sys.stderr)
            print("       VMware may rewrite the VMX from memory on suspend, "
                  "discarding this edit.", file=sys.stderr)
            print("       Power the VM off, or pass --force.", file=sys.stderr)
            return 1

    total = sum(e[3] for e in entries)
    print("payload: %d file(s), %d bytes" % (len(entries), total))
    for host_path, iso_path, joliet_path, size in entries:
        print("  %-40s -> %-24s %d bytes" % (host_path, joliet_path, size))

    if args.dry_run:
        print("")
        print("dry run: would write %s" % args.out)
        if not args.no_vmx:
            print("dry run: would repoint %s.fileName in %s"
                  % (args.device, args.vmx))
        return 0

    try:
        build_iso(entries, args.out, vol_ident=args.vol_ident)
    except RuntimeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1

    ident, back = read_back(args.out)
    print("")
    print("wrote %s (%d bytes), vol ident %s"
          % (args.out, os.path.getsize(args.out), ident))
    for name in sorted(back):
        print("  %-32s %d bytes" % (name, back[name]))

    missing = [e[2].lstrip("/") for e in entries if e[2].lstrip("/") not in back]
    if missing:
        print("error: these files are not readable back from the image: %s"
              % ", ".join(missing), file=sys.stderr)
        return 1

    if not args.no_vmx:
        new_text, old_name = repoint_vmx(vmx_text, os.path.abspath(args.out),
                                        args.device)
        backup = args.vmx + ".bak"
        shutil.copyfile(args.vmx, backup)

        # Stage to a sibling file and os.replace() it into place. The previous
        # code opened the real VMX with mode "w", which truncates before the
        # first byte is written -- a run that died there would leave an EMPTY
        # file describing an installed macOS system. os.replace is atomic for
        # same-volume paths on Windows.
        staged = args.vmx + ".new"
        try:
            with open(staged, "w", encoding="windows-1252", newline="") as handle:
                handle.write(new_text)
            os.replace(staged, args.vmx)
        except OSError as exc:
            if os.path.exists(staged):
                os.remove(staged)
            print("error: could not rewrite %s: %s" % (args.vmx, exc),
                  file=sys.stderr)
            print("       %s is UNCHANGED; backup at %s" % (args.vmx, backup),
                  file=sys.stderr)
            return 1

        # Read the file back. A .bak on disk is not evidence of a repoint --
        # it is written first, so it survives a run that never rewrote
        # anything. That exact state was found on 2026-08-10.
        landed, actual = verify_repoint(args.vmx, args.out, args.device)
        if not landed:
            print("error: the rewrite did not land -- %s.fileName reads %r"
                  % (args.device, actual), file=sys.stderr)
            print("       restoring from %s" % backup, file=sys.stderr)
            shutil.copyfile(backup, args.vmx)
            return 1

        print("")
        print("verified by re-reading %s" % args.vmx)
        print("%s.fileName: %s" % (args.device, old_name))
        print("%s        -> %s" % (" " * len(args.device), os.path.abspath(args.out)))
        print("backup: %s" % backup)
        print("")
        print("Takes effect on the next power cycle, or on a manual reconnect "
              "from the VM's settings UI.")
        print("Whether the guest mounted the volume is NOT shown here -- check "
              "the framebuffer, and open the PNG rather than trusting OCR.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
