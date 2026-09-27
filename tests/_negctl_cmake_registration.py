#!/usr/bin/env python3
"""
_negctl_cmake_registration.py -- negative control for test_cmake_registration.py.

A gate that passes everything converts "never checked" into "believed checked",
which is worse than no gate at all. This harness copies ``tests/CMakeLists.txt``
and the five ``.cpp`` suites into a temp directory, redirects the gate's
module-level ``_TESTS_DIR``/``_CMAKELISTS`` at the copy, plants one defect at a
time, and requires each to be DIAGNOSED while the unmodified baseline passes.

Hand-run, not part of the gate. Underscore-prefixed so
``unittest discover -p "test_*.py"`` does not pick it up:

    python tests/_negctl_cmake_registration.py

Exit 0 means every planted defect failed the gate as intended.

NOTE on the sibling gate. ``test_cmake_registration.py`` loads
``test_check_count_last_line.py`` by path to cross-check the suite list, and it
resolves that path from ``_TESTS_DIR``. So the copy must include the sibling
gate too, otherwise CrossGateAgreementTests errors for a reason that has nothing
to do with the planted defect -- and an error still counts as "diagnosed", which
would make several of these controls pass for the wrong reason.
"""

import importlib.util
import os
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_GATE = os.path.join(_HERE, "test_cmake_registration.py")
_SIBLING = "test_check_count_last_line.py"

SUITES = (
    "test_decode.cpp",
    "test_falcon.cpp",
    "test_gsp_boot.cpp",
    "test_gsp_stage.cpp",
    "test_riscv_ucode.cpp",
)


def load_gate(tests_dir):
    """Import a FRESH copy of the gate with its paths redirected."""
    spec = importlib.util.spec_from_file_location("cmake_gate_under_test", _GATE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module._TESTS_DIR = tests_dir
    module._CMAKELISTS = os.path.join(tests_dir, "CMakeLists.txt")
    return module


def run_gate(tests_dir):
    """Return failures + errors for the gate run against tests_dir."""
    module = load_gate(tests_dir)
    suite = unittest.TestLoader().loadTestsFromModule(module)
    devnull = open(os.devnull, "w")
    try:
        result = unittest.TextTestRunner(stream=devnull, verbosity=0).run(suite)
    finally:
        devnull.close()
    return len(result.failures) + len(result.errors)


def fresh_copy(tmp):
    directory = tempfile.mkdtemp(prefix="negctl-cmake-", dir=tmp)
    shutil.copyfile(os.path.join(_HERE, "CMakeLists.txt"),
                    os.path.join(directory, "CMakeLists.txt"))
    shutil.copyfile(os.path.join(_HERE, _SIBLING),
                    os.path.join(directory, _SIBLING))
    for name in SUITES:
        shutil.copyfile(os.path.join(_HERE, name),
                        os.path.join(directory, name))
    return directory


def _read_cmake(directory):
    with open(os.path.join(directory, "CMakeLists.txt"), "r",
              encoding="utf-8") as handle:
        return handle.read()


def _write_cmake(directory, text):
    with open(os.path.join(directory, "CMakeLists.txt"), "w",
              encoding="utf-8") as handle:
        handle.write(text)


# --- the planted defects ---------------------------------------------------


def defect_unregistered_suite(directory):
    """THE defect this gate exists for: a suite ctest was never told about.

    ctest then reports '4/4 tests passed' and the fifth file has never been
    compiled. The count does not go down -- it just describes fewer things.
    """
    text = _read_cmake(directory)
    marker = "add_kext_test(test_gsp_stage test_gsp_stage.cpp)"
    assert marker in text, "anchor not found -- update this defect"
    _write_cmake(directory, text.replace(marker + "\n", ""))


def defect_commented_out_registration(directory):
    """A '#'-disabled registration must not count as a registration."""
    text = _read_cmake(directory)
    marker = "add_kext_test(test_riscv_ucode test_riscv_ucode.cpp)"
    assert marker in text, "anchor not found -- update this defect"
    _write_cmake(directory, text.replace(marker, "# " + marker))


def defect_name_source_mismatch(directory):
    """The copy-paste that updates the target name but not the source.

    Both halves are individually valid, so only the PAIRING catches it. ctest
    reports one MORE passing test than before, while the suite it names has
    never been compiled.
    """
    text = _read_cmake(directory)
    marker = "add_kext_test(test_gsp_stage test_gsp_stage.cpp)"
    assert marker in text, "anchor not found -- update this defect"
    _write_cmake(directory,
                 text.replace(marker,
                              "add_kext_test(test_gsp_stage2 test_gsp_boot.cpp)"))


def defect_phantom_source(directory):
    """A registration naming a file that does not exist."""
    text = _read_cmake(directory)
    marker = "add_kext_test(test_decode test_decode.cpp)"
    assert marker in text, "anchor not found -- update this defect"
    _write_cmake(directory,
                 text + "\nadd_kext_test(test_ghost test_ghost.cpp)\n")


def defect_duplicate_registration(directory):
    """The same source wired in twice -- ctest count inflates by one."""
    text = _read_cmake(directory)
    marker = "add_kext_test(test_decode test_decode.cpp)"
    assert marker in text, "anchor not found -- update this defect"
    _write_cmake(directory, text.replace(marker, marker + "\n" + marker))


def defect_new_suite_not_in_either_list(directory):
    """A sixth .cpp appears, registered nowhere and named by neither gate."""
    with open(os.path.join(directory, "test_newthing.cpp"), "w",
              encoding="utf-8") as handle:
        handle.write("int main(void) { return 0; }\n")


def defect_enable_testing_removed(directory):
    """Without enable_testing() nothing is wired into ctest at all."""
    text = _read_cmake(directory)
    assert "enable_testing()" in text, "anchor not found -- update this defect"
    _write_cmake(directory, text.replace("enable_testing()", ""))


def defect_include_path_dropped(directory):
    """Without phase0-kext on the include path the suites test nothing real."""
    text = _read_cmake(directory)
    assert "phase0-kext" in text, "anchor not found -- update this defect"
    _write_cmake(directory, text.replace("phase0-kext", "somewhere-else"))


DEFECTS = (
    ("a suite registered nowhere", defect_unregistered_suite),
    ("a registration commented out", defect_commented_out_registration),
    ("target name paired with the wrong source", defect_name_source_mismatch),
    ("a registration naming a missing file", defect_phantom_source),
    ("the same source registered twice", defect_duplicate_registration),
    ("a sixth suite in neither list", defect_new_suite_not_in_either_list),
    ("enable_testing() removed", defect_enable_testing_removed),
    ("phase0-kext dropped from the include path", defect_include_path_dropped),
)


def main():
    failures = 0
    tmp = tempfile.mkdtemp(prefix="negctl-cmake-root-")
    try:
        print("baseline -- unmodified CMakeLists must PASS")
        bad = run_gate(fresh_copy(tmp))
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
                print("  ok   %-42s diagnosed (%d)" % (label, bad))
            else:
                failures += 1
                print("  FAIL %-42s passed clean -- gate too weak" % label)
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
