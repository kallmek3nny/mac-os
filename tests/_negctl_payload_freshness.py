#!/usr/bin/env python3
"""
_negctl_payload_freshness.py -- can PayloadFreshnessTests actually go RED?

Why this is a separate file
---------------------------
``tests/test_guest_build_kext.py``'s ``PayloadFreshnessTests`` compares
``build-payload/kext-src.tar`` against the live ``phase0-kext/`` tree. Right now
they match, so all five of its checks pass -- and a check that has only ever seen
a correct input is indistinguishable from one whose comparison silently does
nothing. That is the ``an-assertion-that-cannot-fail-is-not-coverage`` finding,
and this project has been bitten by it more than once.

The honest way to test the gate is to hand it a payload that IS stale and confirm
it says so. That means writing a deliberately-wrong tar, which must never happen
to the real ``build-payload/`` -- so this is a probe, run by hand, not a
``test_*`` module. The leading underscore keeps it out of ``unittest discover``
the same way ``_negctl_check_count.py`` and ``_negctl_cmake_registration.py`` are
kept out.

It never writes inside the repo. Every mutant tar is built in a temp directory
and the class attribute is repointed at it, so the real artifact is only ever
read.

Run:
    python tests/_negctl_payload_freshness.py

Exit 0 means every mutant was caught. Exit 1 means a mutant SURVIVED, which is a
hole in the gate -- not a passing run.
"""

import importlib.util
import os
import shutil
import sys
import tarfile
import tempfile
import unittest

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_TESTS_DIR)


def load_module():
    path = os.path.join(_TESTS_DIR, "test_guest_build_kext.py")
    spec = importlib.util.spec_from_file_location("gbk_under_test", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["gbk_under_test"] = module
    spec.loader.exec_module(module)
    return module


GBK = load_module()
KEXT = os.path.join(_REPO, "phase0-kext")


def tree_members():
    """(member_name, host_path) for every file the payload should carry."""
    out = []
    for root, dirs, files in os.walk(KEXT):
        dirs[:] = [d for d in dirs if d != "build"]
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, _REPO).replace(os.sep, "/")
            out.append((rel, full))
    return sorted(out)


def write_tar(path, members):
    with tarfile.open(path, "w") as tar:
        for name, full in members:
            tar.add(full, arcname=name)


def run_one(test_name, tar_path):
    """Run a single test of PayloadFreshnessTests against `tar_path`.

    Returns True if it PASSED. A mutant is caught when this returns False.
    """
    original = GBK.PayloadFreshnessTests.TAR
    GBK.PayloadFreshnessTests.TAR = tar_path
    try:
        suite = unittest.TestLoader().loadTestsFromName(
            test_name, GBK.PayloadFreshnessTests)
        result = unittest.TextTestRunner(
            stream=open(os.devnull, "w", encoding="utf-8"), verbosity=0
        ).run(suite)
        # A SKIP is not a pass and not a catch -- it means the mutant never
        # reached the assertion, which would make this whole probe vacuous.
        if result.skipped:
            print("    !! test SKIPPED -- the mutant never reached the check")
            return True
        return result.wasSuccessful()
    finally:
        GBK.PayloadFreshnessTests.TAR = original


def main():
    members = tree_members()
    if not members:
        print("FAIL: found no files under phase0-kext/")
        return 1

    work = tempfile.mkdtemp(prefix="nv-negctl-fresh-")
    survivors = []

    def check(label, test_name, tar_path):
        passed = run_one(test_name, tar_path)
        verdict = "SURVIVED" if passed else "caught"
        print("  %-8s %s" % (verdict, label))
        if passed:
            survivors.append(label)

    try:
        # ------------------------------------------------------------------
        # 0. The baseline. Without this every "caught" below could be a
        #    mutant rejected because the HARNESS builds a broken tar, not
        #    because the mutation was detected.
        # ------------------------------------------------------------------
        good = os.path.join(work, "good.tar")
        write_tar(good, members)
        print("baseline (an honestly-current tar must PASS all three):")
        for name in ("test_every_source_file_is_in_the_payload",
                     "test_the_payload_carries_nothing_the_tree_does_not",
                     "test_every_shipped_file_is_byte_identical_to_the_tree"):
            if not run_one(name, good):
                print("  FAIL   baseline red: %s" % name)
                survivors.append("BASELINE %s" % name)
            else:
                print("  ok     %s" % name)

        print("\nmutants (each must be CAUGHT):")

        # ------------------------------------------------------------------
        # 1. A header edited after the tar was built. THE case this class
        #    exists for: the file list is unchanged, so the two list-
        #    comparing tests stay green and only the byte comparison sees it.
        # ------------------------------------------------------------------
        edited = os.path.join(work, "edited.tar")
        stage = os.path.join(work, "stage")
        os.makedirs(stage, exist_ok=True)
        victim_name, victim_path = next(
            (n, p) for n, p in members if n.endswith("NVGspBoot.hpp"))
        staged_copy = os.path.join(stage, "NVGspBoot.hpp")
        shutil.copyfile(victim_path, staged_copy)
        with open(staged_copy, "ab") as handle:
            handle.write(b"\n/* an edit the payload predates */\n")
        write_tar(edited, [(n, staged_copy if n == victim_name else p)
                           for n, p in members])
        check("a header edited after the tar was built",
              "test_every_shipped_file_is_byte_identical_to_the_tree", edited)
        # And the far side: the list tests must NOT fire here. If they did,
        # they would be reporting a content change as a missing file, which
        # sends the reader to the wrong problem.
        for name, label in (
                ("test_every_source_file_is_in_the_payload",
                 "an edit must not read as a MISSING file"),
                ("test_the_payload_carries_nothing_the_tree_does_not",
                 "an edit must not read as an EXTRA file")):
            if run_one(name, edited):
                print("  ok       %s" % label)
            else:
                print("  MISFIRE  %s" % label)
                survivors.append("MISFIRE %s" % label)

        # ------------------------------------------------------------------
        # 2. A file added to the tree after the tar was built -- the likely
        #    real-world direction, and a guest-side missing-include error.
        # ------------------------------------------------------------------
        short = os.path.join(work, "short.tar")
        write_tar(short, [m for m in members
                          if not m[0].endswith("NVGspStage.hpp")])
        check("a source file absent from the payload",
              "test_every_source_file_is_in_the_payload", short)

        # ------------------------------------------------------------------
        # 3. A file deleted from the tree but still shipped -- lets the guest
        #    build succeed against source the project abandoned.
        # ------------------------------------------------------------------
        extra = os.path.join(work, "extra.tar")
        ghost = os.path.join(stage, "NVGhost.hpp")
        with open(ghost, "w", encoding="utf-8") as handle:
            handle.write("/* deleted from the tree, still in the payload */\n")
        write_tar(extra, members + [("phase0-kext/NVGhost.hpp", ghost)])
        check("a payload file the tree no longer has",
              "test_the_payload_carries_nothing_the_tree_does_not", extra)

        # ------------------------------------------------------------------
        # 4. A truncated file. Same length class of mistake as an edit, but
        #    shorter rather than longer -- a comparison that read only the
        #    first N bytes of one side would miss exactly one of the two.
        # ------------------------------------------------------------------
        trunc = os.path.join(work, "trunc.tar")
        cut = os.path.join(stage, "NVDecode.hpp")
        decode_name, decode_path = next(
            (n, p) for n, p in members if n.endswith("NVDecode.hpp"))
        with open(decode_path, "rb") as handle:
            body = handle.read()
        with open(cut, "wb") as handle:
            handle.write(body[:len(body) // 2])
        write_tar(trunc, [(n, cut if n == decode_name else p)
                          for n, p in members])
        check("a truncated file in the payload",
              "test_every_shipped_file_is_byte_identical_to_the_tree", trunc)

        # ------------------------------------------------------------------
        # 5. A one-BYTE change, no length change at all. The narrowest
        #    statement of the defect: any comparison based on size or mtime
        #    rather than content survives this one.
        # ------------------------------------------------------------------
        onebyte = os.path.join(work, "onebyte.tar")
        flipped = os.path.join(stage, "flipped.hpp")
        data = bytearray(body)
        # Flip a byte in the middle, staying inside printable ASCII so the
        # mutation is a plausible edit rather than a corrupt file.
        mid = len(data) // 2
        data[mid] = 0x41 if data[mid] != 0x41 else 0x42
        with open(flipped, "wb") as handle:
            handle.write(bytes(data))
        write_tar(onebyte, [(n, flipped if n == decode_name else p)
                            for n, p in members])
        check("a single byte changed, same file length",
              "test_every_shipped_file_is_byte_identical_to_the_tree", onebyte)

    finally:
        shutil.rmtree(work, ignore_errors=True)

    print()
    if survivors:
        print("FAIL: %d mutant(s) survived or misfired:" % len(survivors))
        for label in survivors:
            print("  %s" % label)
        return 1
    print("OK: every mutant was caught, baseline green, no misfires.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
