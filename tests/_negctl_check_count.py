#!/usr/bin/env python3
"""
_negctl_check_count.py -- negative control for test_check_count_last_line.py.

A gate that passes everything converts "never checked" into "believed checked",
which is worse than no gate. This script builds mutated copies of the five C++
suites in a temp directory, points the gate's module-level _TESTS_DIR at them,
and asserts each planted defect is DIAGNOSED.

This is a throwaway harness, not part of the gate. It is invoked by hand:

    python tests/_negctl_check_count.py

Exit 0 means every planted defect failed the gate as intended.
"""

import importlib.util
import os
import re
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_GATE = os.path.join(_HERE, "test_check_count_last_line.py")

SUITES = (
    "test_decode.cpp",
    "test_falcon.cpp",
    "test_gsp_boot.cpp",
    "test_gsp_stage.cpp",
    "test_riscv_ucode.cpp",
)


def load_gate(tests_dir):
    """Import a FRESH copy of the gate with _TESTS_DIR redirected."""
    spec = importlib.util.spec_from_file_location("gate_under_test", _GATE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module._TESTS_DIR = tests_dir
    return module


def run_gate(tests_dir):
    """Return (failures + errors) count for the gate run against tests_dir."""
    module = load_gate(tests_dir)
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(module)
    result = unittest.TextTestRunner(stream=open(os.devnull, "w"),
                                     verbosity=0).run(suite)
    return len(result.failures) + len(result.errors)


def fresh_copy(tmp):
    directory = tempfile.mkdtemp(prefix="negctl-", dir=tmp)
    for name in SUITES:
        shutil.copyfile(os.path.join(_HERE, name),
                        os.path.join(directory, name))
    return directory


# --- the planted defects ---------------------------------------------------


def defect_prose_after_count(directory):
    """The exact 2026-08-10 regression: caveat printed AFTER the summary."""
    path = os.path.join(directory, "test_decode.cpp")
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    marker = 'std::printf("\\n%d/%d passed, %d failed\\n",\n                gChecks - gFailures, gChecks, gFailures);'
    assert marker in text, "anchor not found -- update this defect"
    text = text.replace(
        marker,
        marker + '\n\n    std::printf("\\nDecode logic is correct, but unproven '
                 'until Step 7.\\n");')
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def defect_iostream_output(directory):
    """A second output channel defeats a printf-only check."""
    path = os.path.join(directory, "test_gsp_boot.cpp")
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    marker = 'std::printf("\\n%d checks, %d failures\\n", gChecks, gFailures);'
    assert marker in text, "anchor not found -- update this defect"
    text = text.replace(marker, marker + '\n    std::cout << "all good\\n";')
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def defect_call_after_count(directory):
    """A helper invoked after the summary can print anything."""
    path = os.path.join(directory, "test_gsp_stage.cpp")
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    marker = 'std::printf("\\n%d checks, %d failures\\n", gChecks, gFailures);'
    assert marker in text, "anchor not found -- update this defect"
    text = text.replace(marker, marker + '\n    testWindowConstants();')
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def defect_count_removed(directory):
    """No count line at all."""
    path = os.path.join(directory, "test_riscv_ucode.cpp")
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    text = text.replace('"\\n%d checks, %d failures\\n"', '"\\ndone\\n"')
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def defect_suite_renamed(directory):
    """A renamed suite must not let the gate pass vacuously."""
    os.rename(os.path.join(directory, "test_falcon.cpp"),
              os.path.join(directory, "test_falcon_v2.cpp"))


DEFECTS = (
    ("prose printed after the count", defect_prose_after_count),
    ("std::cout after the count", defect_iostream_output),
    ("a call after the count", defect_call_after_count),
    ("the count line removed entirely", defect_count_removed),
    ("a suite renamed out from under the list", defect_suite_renamed),
)


def main():
    failures = 0
    tmp = tempfile.mkdtemp(prefix="negctl-root-")
    try:
        print("baseline -- unmodified sources must PASS")
        baseline = fresh_copy(tmp)
        bad = run_gate(baseline)
        if bad == 0:
            print("  ok   baseline passes")
        else:
            failures += 1
            print("  FAIL baseline reported %d problem(s)" % bad)

        print("")
        print("planted defects -- each must FAIL")
        for label, mutate in DEFECTS:
            directory = fresh_copy(tmp)
            mutate(directory)
            bad = run_gate(directory)
            if bad > 0:
                print("  ok   %-40s diagnosed (%d)" % (label, bad))
            else:
                failures += 1
                print("  FAIL %-40s passed clean -- gate too weak" % label)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    if failures:
        print("NEGATIVE CONTROL FAILED (%d)" % failures)
        return 1
    print("negative control: all %d defects diagnosed, baseline clean"
          % len(DEFECTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
