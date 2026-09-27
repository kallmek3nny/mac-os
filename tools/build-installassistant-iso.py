#!/usr/bin/env python3
"""
build-installassistant-iso.py -- wrap a macOS ``InstallAssistant.pkg`` into the
ISO that OSX-KVM's offline install path expects, without ``mkisofs``.

Why this exists
---------------
``Reinstall macOS Sequoia`` is a *network* installer. The 3 GB
``BaseSystem.img`` on ``sata0:0`` is the recovery environment, not the ~15 GB
payload, so a Recovery session with no working egress stops at "An Internet
connection is required" and cannot proceed. The documented way around that is
OSX-KVM's ``run_offline.md``: attach a second CD carrying the full installer
package and let the recovery environment install from it.

That recipe calls for ``mkisofs``. This host has none of ``mkisofs``,
``genisoimage``, ``xorriso`` or ``oscdimg`` -- the Windows ADK here ships only
DISM and Licensing, with no ``Oscdimg`` directory at all. ``pycdlib`` is pure
Python, installs from PyPI, and produces an image the recovery environment
mounts identically. This script is that substitute.

The detail that silently breaks it
----------------------------------
``scripts/run_offline.sh`` hardcodes ``/Volumes/InstallAssistant``. macOS mounts
by *volume identifier*, so the identifier must be exactly ``InstallAssistant``
or the script's ``cp`` fails with a path that looks correct in the log. The
default here is that string and ``--vol-ident`` warns loudly if you change it.

The recipe is also Ventura-specific: it copies
``/Install macOS Ventura.app`` twice by literal path. Against this project's
Sequoia 15.4.1 (24E263) recovery both must read ``Install macOS Sequoia.app``.
``--emit-script`` writes a retargeted ``run_offline.sh`` onto the ISO so the
renaming is done for you rather than remembered.

Usage
-----
    python build-installassistant-iso.py --self-test
    python build-installassistant-iso.py InstallAssistant.pkg -o InstallAssistant.iso
    python build-installassistant-iso.py InstallAssistant.pkg -o out.iso --app "Install macOS Sequoia.app"
    python build-installassistant-iso.py --verify InstallAssistant.iso

Exit status
-----------
    0   ISO built (or verified, or self-test passed)
    1   inputs rejected, or the built image failed read-back
    2   pycdlib missing

Requires: pycdlib. Note it exposes no ``__version__`` -- check with
``pip show pycdlib``, not ``import pycdlib; pycdlib.__version__``.
"""

import argparse
import io
import os
import sys
import tempfile


# The volume identifier run_offline.sh mounts by. Not cosmetic.
REQUIRED_VOL_IDENT = "InstallAssistant"

# ISO9660 level 3 permits large files but still wants 8.3 primary names; the
# readable names live in the Joliet and Rock Ridge trees.
PKG_NAME = "InstallAssistant.pkg"
PKG_ISO_PATH = "/INSTALLA.PKG;1"
SCRIPT_NAME = "run_offline.sh"
SCRIPT_ISO_PATH = "/RUNOFFLI.SH;1"

# A real InstallAssistant.pkg is ~13-15 GB. Anything tiny is a placeholder or a
# truncated download, and finding that out inside the guest costs a reboot.
PLAUSIBLE_PKG_BYTES = 8 * 1024 * 1024 * 1024

DEFAULT_APP = "Install macOS Sequoia.app"


def load_pycdlib():
    """Import pycdlib, or explain the one command that fixes its absence."""
    try:
        import pycdlib
    except ImportError:
        raise RuntimeError("pycdlib is required: pip install pycdlib")
    return pycdlib


def render_script(app_name):
    """Return a ``run_offline.sh`` retargeted from Ventura to `app_name`.

    Upstream hardcodes ``Install macOS Ventura.app`` in two places. The rest of
    the mechanism is worth reading once: the package is copied *into* the
    installer app as ``Contents/SharedSupport/SharedSupport.dmg`` -- renamed,
    not extracted -- and then the springboard binary is run directly, which is
    what makes it skip the network fetch.
    """
    return (
        "#!/usr/bin/env bash\n"
        "set -e\n"
        "\n"
        "cd /Volumes/macOS\n"
        "mkdir -p private/tmp\n"
        'cp -R "/%s" private/tmp\n'
        'cd "private/tmp/%s"\n'
        "mkdir -p Contents/SharedSupport\n"
        "cp -R /Volumes/%s/%s Contents/SharedSupport/SharedSupport.dmg\n"
        "./Contents/MacOS/InstallAssistant_springboard\n"
        % (app_name, app_name, REQUIRED_VOL_IDENT, PKG_NAME)
    )


def build_iso(pkg_path, out_path, vol_ident=REQUIRED_VOL_IDENT,
              app_name=DEFAULT_APP, emit_script=True):
    """Write an ISO carrying `pkg_path` (and optionally the offline script).

    The package is streamed from disk rather than read into memory -- a real
    payload is ~15 GB and ``open().read()`` on it is not survivable.
    """
    pycdlib = load_pycdlib()

    pkg_size = os.path.getsize(pkg_path)
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=3, joliet=3, rock_ridge="1.09",
            vol_ident=vol_ident)

    with open(pkg_path, "rb") as fp:
        iso.add_fp(fp, pkg_size, PKG_ISO_PATH,
                   joliet_path="/" + PKG_NAME, rr_name=PKG_NAME)

        if emit_script:
            script = render_script(app_name).encode("utf-8")
            iso.add_fp(io.BytesIO(script), len(script), SCRIPT_ISO_PATH,
                       joliet_path="/" + SCRIPT_NAME, rr_name=SCRIPT_NAME)

        iso.write(out_path)

    iso.close()
    return pkg_size


def read_back(iso_path):
    """Reopen `iso_path` and return (vol_ident, {name: size}).

    Building an image that cannot be read is the failure this guards. The
    Joliet names are UTF-16BE on disk; decoding them is how you confirm the
    guest will see ``InstallAssistant.pkg`` rather than ``INSTALLA.PKG``.
    """
    pycdlib = load_pycdlib()

    iso = pycdlib.PyCdlib()
    iso.open(iso_path)
    try:
        ident = iso.pvd.volume_identifier.decode("ascii", "replace").strip()
        entries = {}
        for child in iso.list_children(joliet_path="/"):
            if child.is_dot() or child.is_dotdot():
                continue
            raw = child.file_identifier()
            try:
                name = raw.decode("utf-16_be")
            except UnicodeDecodeError:
                name = raw.decode("ascii", "replace")
            entries[name] = child.get_data_length()
        return ident, entries
    finally:
        iso.close()


def self_test():
    """Build a two-file ISO from throwaway inputs and read it back.

    Proves the pycdlib path works on this host without needing the ~15 GB
    package. Everything lands in a temp directory and is removed.
    """
    tmp = tempfile.mkdtemp(prefix="iatest-")
    try:
        pkg = os.path.join(tmp, PKG_NAME)
        with open(pkg, "wb") as fp:
            fp.write(b"placeholder installassistant payload\n")
        out = os.path.join(tmp, "InstallAssistant.iso")

        build_iso(pkg, out)
        print("built    : %s (%d bytes)" % (out, os.path.getsize(out)))

        ident, entries = read_back(out)
        print("vol ident: %s" % ident)
        for name in sorted(entries):
            print("  %-24s %d bytes" % (name, entries[name]))

        problems = []
        if ident != REQUIRED_VOL_IDENT:
            problems.append("volume identifier is %r, not %r"
                            % (ident, REQUIRED_VOL_IDENT))
        for expected in (PKG_NAME, SCRIPT_NAME):
            if expected not in entries:
                problems.append("%s missing from the Joliet tree" % expected)

        script = render_script(DEFAULT_APP)
        if "Ventura" in script:
            problems.append("emitted script still references Ventura")
        if "/Volumes/%s/" % REQUIRED_VOL_IDENT not in script:
            problems.append("emitted script does not mount /Volumes/%s"
                            % REQUIRED_VOL_IDENT)

        for p in problems:
            print("FAIL     : %s" % p)
        if problems:
            return 1
        print("result   : PASS (built and read back)")
        return 0
    finally:
        for name in os.listdir(tmp):
            os.remove(os.path.join(tmp, name))
        os.rmdir(tmp)


def build_parser():
    p = argparse.ArgumentParser(
        description="Wrap InstallAssistant.pkg into the offline-install ISO.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="exit 0 = ok, 1 = rejected or read-back failed, "
               "2 = pycdlib missing")
    p.add_argument("pkg", nargs="?", help="path to InstallAssistant.pkg")
    p.add_argument("-o", "--out", help="path of the ISO to write")
    p.add_argument("--vol-ident", default=REQUIRED_VOL_IDENT,
                   help="volume identifier (default: %s -- run_offline.sh "
                        "mounts this literal path)" % REQUIRED_VOL_IDENT)
    p.add_argument("--app", default=DEFAULT_APP,
                   help="installer app name the emitted script targets "
                        "(default: %s)" % DEFAULT_APP)
    p.add_argument("--no-script", action="store_true",
                   help="do not place a run_offline.sh on the ISO")
    p.add_argument("--allow-small-pkg", action="store_true",
                   help="skip the size sanity check on the package")
    p.add_argument("--verify", metavar="ISO",
                   help="read back an existing ISO and exit")
    p.add_argument("--self-test", action="store_true",
                   help="build and read back a throwaway ISO, then exit")
    return p


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
        ident, entries = read_back(args.verify)
        print("vol ident: %s" % ident)
        for name in sorted(entries):
            print("  %-24s %d bytes" % (name, entries[name]))
        if ident != REQUIRED_VOL_IDENT:
            print("error: volume identifier is %r; run_offline.sh mounts "
                  "/Volumes/%s and will fail"
                  % (ident, REQUIRED_VOL_IDENT), file=sys.stderr)
            return 1
        if PKG_NAME not in entries:
            print("error: %s is not on this ISO" % PKG_NAME, file=sys.stderr)
            return 1
        print("result   : PASS")
        return 0

    if not args.pkg or not args.out:
        print("error: give both a package and --out (or use --self-test)",
              file=sys.stderr)
        return 1

    if not os.path.isfile(args.pkg):
        print("error: no such file: %s" % args.pkg, file=sys.stderr)
        return 1

    size = os.path.getsize(args.pkg)
    if size < PLAUSIBLE_PKG_BYTES and not args.allow_small_pkg:
        print("error: %s is %.2f GiB. A real InstallAssistant.pkg is ~13-15 "
              "GiB, so this is a placeholder or a truncated download. "
              "Discovering that inside the guest costs a reboot -- pass "
              "--allow-small-pkg if you meant it."
              % (args.pkg, size / (1024.0 ** 3)), file=sys.stderr)
        return 1

    if args.vol_ident != REQUIRED_VOL_IDENT:
        print("warning: volume identifier %r is not %r. run_offline.sh "
              "mounts /Volumes/%s by literal path and will fail."
              % (args.vol_ident, REQUIRED_VOL_IDENT, REQUIRED_VOL_IDENT),
              file=sys.stderr)

    print("package  : %s (%.2f GiB)" % (args.pkg, size / (1024.0 ** 3)))
    build_iso(args.pkg, args.out, vol_ident=args.vol_ident,
              app_name=args.app, emit_script=not args.no_script)
    print("built    : %s (%d bytes)" % (args.out, os.path.getsize(args.out)))

    ident, entries = read_back(args.out)
    print("vol ident: %s" % ident)
    for name in sorted(entries):
        print("  %-24s %d bytes" % (name, entries[name]))

    if PKG_NAME not in entries or entries[PKG_NAME] != size:
        print("error: read-back mismatch -- %s is %s on the ISO, %d on disk"
              % (PKG_NAME, entries.get(PKG_NAME), size), file=sys.stderr)
        return 1

    print("result   : PASS (built and read back)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
