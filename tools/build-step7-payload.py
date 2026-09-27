#!/usr/bin/env python3
"""build-step7-payload.py -- rebuild the STEP 7 payload from the working tree.

Why this exists
---------------
``build-payload/`` is a build artifact checked in beside its own sources, and
``tests/test_guest_build_kext.py``'s ``PayloadFreshnessTests`` exists because it
can silently drift from ``phase0-kext/``. That gate DETECTS drift. It fired for
real twenty minutes after it was written, on a header a peer had just edited.

Detecting is the weaker half. Rebuilding was three commands typed by hand, and
they lived only inside a docstring's ``REBUILD`` string:

    tar -cf build-payload/kext-src.tar --exclude=build phase0-kext
    cp tools/guest-build-kext.sh build-payload/
    python tools/deliver-via-cd.py --add ... --out ... --vol-ident PAYLOAD --no-vmx

Three hand-typed commands with an ordering requirement is a defect generator.
Forget the second one and the ISO ships a stale script -- which is exactly the
drift that already happened once: both ``}``-instead-of-``fi`` fixes were made in
``tools/`` while the staged copy and the burned ISO still carried the broken
version, and every static check in the test module reads ``tools/``, so the
syntax error was reported fixed while the artifact the operator would actually
run still had it.

What this tool guarantees that the three commands did not
--------------------------------------------------------
**Nothing lands until it verifies.** Everything is built in a system temp
directory, checked against the working tree there, and only then moved into
``build-payload/``. A failed or half-finished build cannot leave a truncated tar
or an ISO that predates its inputs -- and those are the two shapes the operator
cannot detect from the guest console, because a short tar unpacks fine and just
omits a header, and a stale ISO compiles code nobody is working on.

The temp directory is outside the repo on purpose. An early attempt at this
staged through ``payload-stage/`` inside the tree, which duplicated every kext
header basename in the repo for hours -- and the doc-path gate resolves by
basename, so a second copy makes that lookup order-dependent.

**It writes the tar itself rather than shelling out to ``tar``.** GNU tar is not
guaranteed on a Windows PATH, and a missing binary would fail in a way that reads
as a broken payload rather than a missing tool. ``tarfile`` also lets the member
names be pinned explicitly, which matters: the guest script looks for
``phase0-kext/Makefile`` at exactly that depth, and a tar rooted one level too
deep passes every shape check while making the guest report "no Makefile".

Ownership fields are neutralised (uid/gid 0, empty uname/gname) so extraction in
the guest as an ordinary user does not attempt a chown it cannot perform.

Usage
-----
    python tools/build-step7-payload.py              # rebuild + verify + run the gate
    python tools/build-step7-payload.py --skip-gate  # rebuild + verify only
    python tools/build-step7-payload.py --out-dir DIR --skip-gate   # for tests

``--out-dir`` exists so ``tests/test_build_step7_payload.py`` can drive this end
to end without rewriting the real artifact. A test that mutated the tree would
fake a named FAIL for any gate run happening concurrently in another terminal.

``--skip-gate`` is implied by a non-default ``--out-dir``: ``PayloadFreshnessTests``
reads a hardcoded ``build-payload/`` path, so running it after building somewhere
else would report on the OLD artifact and call it a verification of the new one.
That is worse than not running it.

Exit 0 means the payload in the output directory matches the working tree byte
for byte. Exit 1 means nothing was moved into place.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

_TOOLS = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_TOOLS)

KEXT_DIR = "phase0-kext"
TAR_NAME = "kext-src.tar"
SCRIPT_NAME = "guest-build-kext.sh"
ISO_NAME = "step7-payload.iso"
VOL_IDENT = "PAYLOAD"
DEFAULT_OUT_DIR = os.path.join(_REPO, "build-payload")

# Directories never shipped. `build/` is the HOST's output directory; if it rode
# along, the guest's first `make` would find object files it did not produce and
# either skip work or link against them.
EXCLUDED_DIRS = ("build",)

DELIVER = os.path.join(_TOOLS, "deliver-via-cd.py")
GUEST_SCRIPT = os.path.join(_TOOLS, SCRIPT_NAME)


# ----------------------------------------------------------------------------
# The source of truth: walk the tree, never a hardcoded file list
# ----------------------------------------------------------------------------

def tree_members():
    """[(arcname, host_path)] for every file the payload must carry.

    Derived by walking, not listed. A hardcoded list goes stale the moment a
    sixth header lands -- which is the same defect this whole tool exists to
    remove, one level up.

    ``arcname`` is repo-relative with forward slashes, so the archive is rooted
    at ``phase0-kext/`` regardless of the host separator.
    """
    root_dir = os.path.join(_REPO, KEXT_DIR)
    found = []
    for root, dirs, files in os.walk(root_dir):
        dirs[:] = [d for d in dirs if d not in EXCLUDED_DIRS]
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, _REPO).replace(os.sep, "/")
            found.append((rel, full))
    return sorted(found)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


# ----------------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------------

def build_tar(tar_path, members):
    """USTAR, sorted, neutral ownership.

    USTAR rather than the ``tarfile`` default (PAX): every member name here is
    well under the 100-character limit, and USTAR is the format with the fewest
    ways to surprise the guest's bsdtar. Sorted order so two rebuilds of the
    same tree differ only in member mtimes.
    """
    with tarfile.open(tar_path, "w", format=tarfile.USTAR_FORMAT) as tar:
        for arcname, full in members:
            info = tar.gettarinfo(full, arcname=arcname)
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            with open(full, "rb") as handle:
                tar.addfile(info, handle)


def build_iso(work, tar_path, script_path, iso_path):
    """Hand the ISO off to deliver-via-cd.py, which owns ISO9660 and has a
    53-check self-test. ``--no-vmx`` because this tool never touches the VMX --
    that edit is power-off only and belongs to the operator card.
    """
    argv = [
        sys.executable, DELIVER,
        "--add", tar_path,
        "--add", script_path,
        "--out", iso_path,
        "--vol-ident", VOL_IDENT,
        "--no-vmx",
    ]
    proc = subprocess.run(argv, cwd=work, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, universal_newlines=True)
    return proc.returncode, proc.stdout


# ----------------------------------------------------------------------------
# Verify -- against the temp copies, BEFORE anything moves into place
# ----------------------------------------------------------------------------

def verify(tar_path, script_path, iso_path, members):
    """Return a list of problems. Empty list means the payload is honest.

    This repeats what ``PayloadFreshnessTests`` asserts, on purpose and in a
    different process. The gate checks the artifact that is already in place;
    this checks the artifact about to REPLACE it. Only the second one can stop a
    bad payload from landing.
    """
    problems = []

    expected = dict(members)
    with tarfile.open(tar_path, "r") as tar:
        shipped = {m.name: m for m in tar.getmembers() if m.isfile()}

        missing = sorted(set(expected) - set(shipped))
        if missing:
            problems.append("absent from the tar: %s" % ", ".join(missing))

        extra = sorted(set(shipped) - set(expected))
        if extra:
            problems.append("in the tar but not the tree: %s" % ", ".join(extra))

        # Contents, not names. Editing a header changes bytes and leaves both
        # lists above identical -- that is exactly how a stale payload hides.
        for name in sorted(set(expected) & set(shipped)):
            handle = tar.extractfile(shipped[name])
            in_tar = handle.read() if handle is not None else b""
            with open(expected[name], "rb") as disk:
                on_disk = disk.read()
            if in_tar != on_disk:
                problems.append("tar copy differs from the tree: %s" % name)

        if any(part == "build" for m in shipped for part in m.split("/")):
            problems.append("the tar carries a build/ directory")

    with open(GUEST_SCRIPT, "rb") as handle:
        authority = handle.read()
    with open(script_path, "rb") as handle:
        staged = handle.read()
    if staged != authority:
        problems.append("the staged %s differs from tools/" % SCRIPT_NAME)
    if b"\r\n" in staged:
        # The mirror of the CRLF-in-.bat gate. A CRLF shebang fails in the guest
        # as `bad interpreter: /bin/sh^M`, which reads as a missing shell.
        problems.append("the staged %s contains CRLF" % SCRIPT_NAME)

    iso_bytes = os.path.getsize(iso_path)
    payload_bytes = os.path.getsize(tar_path) + os.path.getsize(script_path)
    if iso_bytes <= payload_bytes:
        problems.append("the ISO (%d bytes) is not larger than its inputs (%d)"
                        % (iso_bytes, payload_bytes))

    return problems


def run_freshness_gate():
    """Run the real gate over the artifact now in place.

    Discovery form, not ``python -m unittest tests.test_guest_build_kext`` --
    ``tests/`` has no ``__init__.py``, and on this machine
    ``site-packages/tests/__init__.py`` makes that name resolve to the WRONG
    directory, so the failure names your module rather than the shadowing
    package.

    Note this is a nested Python-gate run. If ``run-python-tests.bat`` is running
    in another terminal, the two collide in %TEMP% and can fake a failure with no
    traceback. ``--skip-gate`` is the way out.
    """
    argv = [sys.executable, "-m", "unittest", "discover",
            "-s", "tests", "-p", "test_guest_build_kext.py"]
    proc = subprocess.run(argv, cwd=_REPO, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, universal_newlines=True)
    return proc.returncode, proc.stdout


# ----------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Rebuild build-payload/ from phase0-kext/ and verify it.")
    parser.add_argument("--out-dir", default=DEFAULT_OUT_DIR, metavar="DIR",
                        help="where the three artifacts land"
                             " (default: build-payload/)")
    parser.add_argument("--skip-gate", action="store_true",
                        help="do not run test_guest_build_kext.py afterwards")
    args = parser.parse_args(argv)

    out_dir = os.path.abspath(args.out_dir)
    default_target = os.path.normcase(out_dir) == os.path.normcase(
        os.path.abspath(DEFAULT_OUT_DIR))

    members = tree_members()
    if not members:
        print("FAIL: found no files under %s/" % KEXT_DIR)
        return 1
    if not os.path.isfile(GUEST_SCRIPT):
        print("FAIL: %s is missing" % GUEST_SCRIPT)
        return 1
    if not os.path.isfile(DELIVER):
        print("FAIL: %s is missing" % DELIVER)
        return 1

    print("source: %s/ -- %d file(s)" % (KEXT_DIR, len(members)))
    for arcname, _ in members:
        print("    %s" % arcname)

    # What is there now, so a replacement is visible rather than silent.
    print("\ncurrently in %s:" % out_dir)
    for name in (TAR_NAME, SCRIPT_NAME, ISO_NAME):
        existing = os.path.join(out_dir, name)
        if os.path.isfile(existing):
            print("    %-20s %8d bytes  sha256 %s"
                  % (name, os.path.getsize(existing), sha256(existing)[:16]))
        else:
            print("    %-20s absent" % name)

    work = tempfile.mkdtemp(prefix="nv-step7-payload-")
    try:
        tar_path = os.path.join(work, TAR_NAME)
        script_path = os.path.join(work, SCRIPT_NAME)
        iso_path = os.path.join(work, ISO_NAME)

        print("\nbuilding in %s" % work)
        build_tar(tar_path, members)
        shutil.copyfile(GUEST_SCRIPT, script_path)   # binary copy keeps LF

        code, output = build_iso(work, tar_path, script_path, iso_path)
        if code != 0 or not os.path.isfile(iso_path):
            print("FAIL: deliver-via-cd.py exited %d" % code)
            print(output)
            return 1

        problems = verify(tar_path, script_path, iso_path, members)
        if problems:
            print("\nFAIL: the freshly built payload does not match the tree.")
            print("Nothing was moved into place; %s is unchanged." % out_dir)
            for problem in problems:
                print("    %s" % problem)
            return 1

        print("verified against the working tree: %d file(s) byte-identical,"
              " no extras, no build/, staged script matches tools/, LF-only."
              % len(members))

        if not os.path.isdir(out_dir):
            os.makedirs(out_dir)
        for name in (TAR_NAME, SCRIPT_NAME, ISO_NAME):
            shutil.move(os.path.join(work, name), os.path.join(out_dir, name))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("\nnow in %s:" % out_dir)
    for name in (TAR_NAME, SCRIPT_NAME, ISO_NAME):
        landed = os.path.join(out_dir, name)
        print("    %-20s %8d bytes  sha256 %s"
              % (name, os.path.getsize(landed), sha256(landed)[:16]))

    if args.skip_gate:
        print("\n--skip-gate: test_guest_build_kext.py not run.")
        return 0
    if not default_target:
        # Running it here would report on build-payload/ -- the artifact this
        # invocation did NOT touch -- and read as a verification of the one it
        # did. A vacuous green is worse than no green.
        print("\n--out-dir is not build-payload/, so the freshness gate would"
              " report on a different artifact. Not run.")
        return 0

    print("\nrunning tests/test_guest_build_kext.py over what just landed...")
    code, output = run_freshness_gate()
    print(output.strip())
    if code != 0:
        print("\nFAIL: the gate is red against the payload just built.")
        return 1
    print("\nOK: payload rebuilt and the freshness gate agrees.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
