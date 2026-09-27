#!/usr/bin/env python3
"""
_negctl_check_count_floor.py -- negative control for test_check_count_floor.py.

That module passed 12/12 on its first run. So does a module full of assertions
that cannot fail. This harness plants each defect the gate claims to catch and
asserts the gate DIAGNOSES it, naming the suite.

Run by hand -- it is not discovered (no ``test_`` prefix), so it never runs under
the gate:

    python tests/_negctl_check_count_floor.py

It mutates NOTHING in the real tree. Every defect is planted by monkeypatching
the gate's module-level state or by pointing it at a ``tempfile.mkdtemp()``
directory. That constraint is deliberate: a probe that writes into tests/ gives a
concurrent gate run a specific, plausible FAIL with a real traceback and no
defect behind it.

The defects planted here
------------------------
1. A suite whose binary reports ``0 checks, 0 failures`` -- the exact output the
   gate exists to reject. Must fail the floor assertion.
2. A suite reporting just under its floor. Pins the boundary, so a floor of 0 or
   a ``>=`` written as ``>= 0`` is observable.
3. A suite reporting exactly its floor. Must PASS -- otherwise defect 2 above
   could be passing because the gate rejects everything.
4. A new tests/*.cpp with no MEASURED entry -- the silent-omission case.
5. A MEASURED entry naming a suite that is gone.
6. A built tree missing one listed binary.
7. A binary older than its source.
8. A source stripped of its check calls.
"""

import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import test_check_count_floor as gate  # noqa: E402

FAILURES = []


def expect_fail(label, func, must_mention=()):
    """Run one gate test under a planted defect; require it to FAIL."""
    try:
        func()
    except AssertionError as exc:
        text = str(exc)
        missing = [m for m in must_mention if m not in text]
        if missing:
            FAILURES.append("%s: diagnosed, but the message never names %s\n    %s"
                            % (label, ", ".join(missing), text.splitlines()[0]))
        else:
            print("  ok   %-52s diagnosed" % label)
        return
    FAILURES.append("%s: NOT DIAGNOSED -- the gate passed a planted defect" % label)


def expect_pass(label, func):
    """Run one gate test on a correct fixture; require it to PASS."""
    try:
        func()
    except AssertionError as exc:
        FAILURES.append("%s: rejected a CORRECT fixture -- %s"
                        % (label, str(exc).splitlines()[0]))
        return
    print("  ok   %-52s passed" % label)


class Harness(unittest.TestCase):
    """Only here to supply assertion methods to the gate's bound tests."""

    def runTest(self):  # pragma: no cover
        pass


def bound(cls, name):
    """One of the gate's test methods, callable directly."""
    return getattr(cls(name), name)


def stub_runner(reported):
    """Replace the gate's subprocess.run with one that prints `reported`.

    Faking the OUTPUT rather than compiling a stub binary is what keeps this
    harness portable and keeps it out of the real build directory.
    """
    class Result(object):
        def __init__(self, text):
            self.stdout = text

    def run(cmd, **kwargs):
        stem = os.path.splitext(os.path.basename(cmd[0]))[0]
        text = reported.get(stem)
        if text is None:
            text = "%d checks, 0 failures\n" % gate.MEASURED[stem]
        return Result(text)
    return run


def main():
    real_run = gate.subprocess.run
    real_measured = dict(gate.MEASURED)
    real_here = gate.HERE
    real_build = gate.BUILD_DIR

    cls = gate.BinariesReportRealCounts
    table = gate.TableMatchesTheTree
    static = gate.SourcesStillCallChecks

    print("=== floor assertion ===")

    # 1. the defect this gate exists for
    gate.subprocess.run = stub_runner({"test_falcon": "0 checks, 0 failures\n"})
    expect_fail("a suite reporting 0 checks",
                bound(cls, "test_each_built_suite_reports_at_least_its_floor"),
                must_mention=("test_falcon", "floor"))

    # 2. one below the floor -- pins the boundary
    below = gate.floor_for("test_gsp_boot") - 1
    gate.subprocess.run = stub_runner(
        {"test_gsp_boot": "%d checks, 0 failures\n" % below})
    expect_fail("a suite one check below its floor",
                bound(cls, "test_each_built_suite_reports_at_least_its_floor"),
                must_mention=("test_gsp_boot",))

    # 3. exactly the floor -- must pass, or 1 and 2 prove nothing
    exact = gate.floor_for("test_gsp_boot")
    gate.subprocess.run = stub_runner(
        {"test_gsp_boot": "%d checks, 0 failures\n" % exact})
    expect_pass("a suite exactly at its floor",
                bound(cls, "test_each_built_suite_reports_at_least_its_floor"))

    # 4. a final line that is prose, not a count
    gate.subprocess.run = stub_runner({"test_decode": "unproven until Step 7.\n"})
    expect_fail("a suite ending on prose",
                bound(cls, "test_each_built_suite_reports_at_least_its_floor"),
                must_mention=("test_decode",))

    # 5. a suite that prints nothing at all
    gate.subprocess.run = stub_runner({"test_decode": "\n\n"})
    expect_fail("a suite printing nothing",
                bound(cls, "test_each_built_suite_reports_at_least_its_floor"),
                must_mention=("test_decode",))

    gate.subprocess.run = real_run

    print("=== table vs tree ===")

    tmp = tempfile.mkdtemp(prefix="countfloor-negctl-")
    try:
        # A synthetic tests/ dir holding copies of the real sources, so the
        # table tests can be driven without touching the real tree.
        for stem in real_measured:
            shutil.copyfile(os.path.join(real_here, stem + ".cpp"),
                            os.path.join(tmp, stem + ".cpp"))
        gate.HERE = tmp
        gate.BUILD_DIR = os.path.join(tmp, "build")

        expect_pass("the synthetic tree is well-formed",
                    bound(table, "test_every_suite_on_disk_has_a_recorded_count"))

        # 6. a new suite nobody recorded
        with open(os.path.join(tmp, "test_brand_new.cpp"), "w") as fh:
            fh.write("int main(){return 0;}\n")
        expect_fail("a tests/*.cpp with no MEASURED entry",
                    bound(table, "test_every_suite_on_disk_has_a_recorded_count"),
                    must_mention=("test_brand_new",))
        os.remove(os.path.join(tmp, "test_brand_new.cpp"))

        # 7. a MEASURED entry for a suite that is gone
        gate.MEASURED = dict(real_measured, test_deleted_suite=42)
        expect_fail("a MEASURED entry with no source",
                    bound(table, "test_no_recorded_count_names_a_suite_that_is_gone"),
                    must_mention=("test_deleted_suite",))

        # 8. a zero recorded count, i.e. a floor that cannot fail
        gate.MEASURED = dict(real_measured, test_falcon=0)
        expect_fail("a recorded count of zero",
                    bound(table, "test_every_recorded_count_and_derived_floor_is_positive"),
                    must_mention=("test_falcon",))
        gate.MEASURED = dict(real_measured)

        print("=== build directory ===")

        os.makedirs(gate.BUILD_DIR, exist_ok=True)
        for stem in real_measured:
            open(os.path.join(gate.BUILD_DIR, stem + ".exe"), "wb").close()
            # binaries newer than sources
            os.utime(os.path.join(gate.BUILD_DIR, stem + ".exe"), (2e9, 2e9))
        expect_pass("a complete, fresh build dir",
                    bound(cls, "test_no_listed_suite_is_missing_from_an_existing_build"))
        expect_pass("a fresh build dir passes the staleness test",
                    bound(cls, "test_no_binary_is_older_than_its_source"))

        # 9. one binary missing from a built tree
        os.remove(os.path.join(gate.BUILD_DIR, "test_gsp_stage.exe"))
        expect_fail("a built tree missing one binary",
                    bound(cls, "test_no_listed_suite_is_missing_from_an_existing_build"),
                    must_mention=("test_gsp_stage",))
        open(os.path.join(gate.BUILD_DIR, "test_gsp_stage.exe"), "wb").close()
        os.utime(os.path.join(gate.BUILD_DIR, "test_gsp_stage.exe"), (2e9, 2e9))

        # 10. a binary older than its source
        os.utime(os.path.join(gate.BUILD_DIR, "test_riscv_ucode.exe"), (1e9, 1e9))
        os.utime(os.path.join(tmp, "test_riscv_ucode.cpp"), (1.5e9, 1.5e9))
        expect_fail("a binary older than its source",
                    bound(cls, "test_no_binary_is_older_than_its_source"),
                    must_mention=("test_riscv_ucode",))

        print("=== source-side floor ===")

        expect_pass("real sources clear the call-site floor",
                    bound(static, "test_each_suite_calls_check_helpers_many_times"))

        # 11. a suite gutted of its check calls
        with open(os.path.join(tmp, "test_decode.cpp"), "w") as fh:
            fh.write("int main(){ return 0; }\n")
        expect_fail("a source with no check calls",
                    bound(static, "test_each_suite_calls_check_helpers_many_times"),
                    must_mention=("test_decode",))
    finally:
        gate.subprocess.run = real_run
        gate.MEASURED = real_measured
        gate.HERE = real_here
        gate.BUILD_DIR = real_build
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if FAILURES:
        print("NEGATIVE CONTROL FAILED -- %d problem(s):" % len(FAILURES))
        for item in FAILURES:
            print("  " + item)
        return 1
    print("negative control passed: every planted defect was diagnosed, "
          "and every correct fixture passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
