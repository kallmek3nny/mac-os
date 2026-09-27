#!/usr/bin/env python3
"""Negative control for MakeStatusTests -- does the pipeline fix actually matter?

Six green tests prove nothing on their own: if the assertions could pass against
the OLD code too, the fix is unmeasured. This reconstructs the pre-fix form and
runs it against the same fixture.

It does NOT mutate the working tree. A probe that plants a defect in the live
files makes a concurrent peer's gate run report named FAILs with real tracebacks
and no defect, so the buggy variant is written to a temp copy instead.

Run:
    python tests/_negctl_make_status.py

Expected: the buggy copy reports "make exit status: 0" and exits 0 for a make
that exited 2. The fixed copy reports 2 and exits 2.
"""
import os
import shutil
import subprocess
import tarfile
import tempfile

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_TESTS_DIR)
SCRIPT = os.path.join(_REPO, "tools", "guest-build-kext.sh")
SH = shutil.which("sh")

# The exact pre-fix two lines, as they stood before this session.
BUGGY = ('( cd "$KEXT_DIR" && make 2>&1 ) | tee -a "$LOG"\n'
         'MAKE_STATUS=$?\n')


def posix_path(win):
    p = win.replace("\\", "/")
    if len(p) > 1 and p[1] == ":":
        p = "/" + p[0].lower() + p[2:]
    return p


def build_buggy_copy(dest_dir):
    """Reconstruct the pre-fix script by replacing the whole fixed block."""
    with open(SCRIPT, "r", newline="") as handle:
        text = handle.read()

    start = text.index('# `$?` after a PIPELINE')
    end = text.index('\n', text.index('fi\n', text.index('MAKE_STATUS=1'))) + 1
    patched = text[:start] + BUGGY + text[end:]

    if 'MAKE_STATUS_FILE' in patched:
        raise SystemExit("PROBE BROKEN: the fixed block survived the splice; "
                         "the anchors no longer match the file")
    if BUGGY not in patched:
        raise SystemExit("PROBE BROKEN: the buggy form was not inserted")

    dest = os.path.join(dest_dir, "guest-build-kext.sh")
    with open(dest, "w", newline="") as handle:
        handle.write(patched)
    return dest


def make_fixture(root, shim_dir, make_exit):
    vol = os.path.join(root, "PAYLOAD")
    os.makedirs(vol, exist_ok=True)
    stage = os.path.join(root, "stage", "phase0-kext")
    os.makedirs(stage, exist_ok=True)
    with open(os.path.join(stage, "Makefile"), "w", newline="\n") as handle:
        handle.write("all:\n\t@true\n")
    with tarfile.open(os.path.join(vol, "kext-src.tar"), "w") as tar:
        tar.add(stage, arcname="phase0-kext")

    shim = os.path.join(shim_dir, "make")
    with open(shim, "w", newline="\n") as handle:
        handle.write("#!/bin/sh\necho 'simulated compile'\nexit %d\n" % make_exit)
    os.chmod(shim, 0o755)


def run(script, root, work, shim_dir):
    env = dict(os.environ)
    env["NV_PAYLOAD_ROOT"] = posix_path(root)
    env["NV_WORK_DIR"] = posix_path(work)
    env["PATH"] = shim_dir + os.pathsep + env.get("PATH", "")
    return subprocess.run([SH, script], capture_output=True, text=True,
                          env=env, timeout=120)


def trial(script, label, make_exit):
    root = tempfile.mkdtemp(prefix="negctl-root-")
    workp = tempfile.mkdtemp(prefix="negctl-work-")
    shim = tempfile.mkdtemp(prefix="negctl-shim-")
    try:
        work = os.path.join(workp, "w")
        make_fixture(root, shim, make_exit)
        proc = run(script, root, work, shim)
        reported = None
        for line in proc.stdout.splitlines():
            if line.startswith("make exit status:"):
                reported = line.split(":", 1)[1].strip()
        print("  %-8s make exited %-2d -> log says %-6s script exited %s"
              % (label, make_exit, reported, proc.returncode))
        return reported, proc.returncode
    finally:
        for d in (root, workp, shim):
            shutil.rmtree(d, ignore_errors=True)


def main():
    if not SH:
        raise SystemExit("no sh on PATH")

    tmp = tempfile.mkdtemp(prefix="negctl-script-")
    failures = []
    try:
        buggy = build_buggy_copy(tmp)

        print("BUGGY copy (pre-fix pipeline):")
        rep, code = trial(buggy, "buggy", 2)
        if rep == "2" or code == 2:
            failures.append("the buggy form reported make's real status -- the"
                            " tests would pass without the fix, so they are"
                            " vacuous")

        print("FIXED copy (working tree):")
        rep, code = trial(SCRIPT, "fixed", 2)
        if rep != "2" or code != 2:
            failures.append("the fixed script did not carry make's status")

        rep, code = trial(SCRIPT, "fixed", 0)
        if rep != "0" or code != 0:
            failures.append("the fixed script broke the success path")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if failures:
        for f in failures:
            print("NEGATIVE CONTROL FAILED:", f)
        raise SystemExit(1)
    print("NEGATIVE CONTROL PASSED: the buggy form is caught, the fix is not"
          " vacuous, and the success path survives.")


if __name__ == "__main__":
    main()
