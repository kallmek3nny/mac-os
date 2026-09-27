#!/usr/bin/env python3
"""
test_check_kext_bundle.py -- negative controls for ``tools/check-kext-bundle.py``,
plus a positive control over the real ``phase0-kext/``.

Why this exists
---------------
The checker under test passed on its first run against the real tree, printing
``OK -- 25 checks``. So does a body of ``return 0``. Every gate in this project
that was written and immediately went green has, at least once, turned out to be
green for the wrong reason: a negative control that missed the line it aimed at,
a regex that could not match the construct it was written for, an assertion
whose two sides were computed from the same source and so agreed no matter which
was wrong.

So each check in the checker gets a tree in which THAT check, and as far as
possible only that check, is broken. If a mutation does not turn the run red,
the corresponding check is decoration.

The first test in this module is the one that makes the rest mean anything:
``GoodTreePasses`` asserts the unmutated fake tree comes out OK. Without it,
every negative control below could be passing because the fixture is malformed
in some unrelated way -- red for a reason that has nothing to do with the
mutation, and indistinguishable from success at catching it.

What the mutations model
------------------------
All of them share one property, which is the reason the checker exists: none is
a compile error, and none is visible to the other three gates. A wrong
``CFBundleExecutable`` still builds. A drifted ``CFBundleIdentifier`` still
builds, and makes ``make unload`` a no-op that exits 0. A missing
``OSBundleLibraries`` entry still builds and fails as an undefined symbol at load
time on the target laptop -- the most expensive place in this project for a
defect to appear, because reaching it costs a power cycle and physical access.

Verdict word, not exit code
---------------------------
Every assertion below checks for the literal ``FAILED`` or ``OK`` in the
checker's output. A checker that crashes before reaching its verdict also exits
non-zero, and "it exited 1" would accept that as proof the mutation was caught
when in fact nothing was ever evaluated.
"""

import importlib.util
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHECKER = os.path.join(ROOT, "tools", "check-kext-bundle.py")


def load_checker_module():
    """Import the checker by path.

    Its filename contains dashes, so it is not importable by name. The helper
    functions are worth unit-testing directly -- ``expand()`` in particular has
    a recursion bound that no end-to-end run exercises.
    """
    spec = importlib.util.spec_from_file_location("check_kext_bundle", CHECKER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GOOD_MAKEFILE = (
    "#\n"
    "# A comment mentioning KEXT_NAME := DECOY that must not be parsed.\n"
    "#\n"
    "KEXT_NAME    := NV3050Probe\n"
    "BUNDLE_ID    := local.nv3050.$(KEXT_NAME)\n"
    "CXXFLAGS     := -mkernel\n"
    "CXXFLAGS     += -fapple-kext\n"
)

GOOD_HPP = (
    "class NV3050Probe : public IOService {\n"
    "    OSDeclareDefaultStructors(NV3050Probe)\n"
    "};\n"
)

GOOD_CPP = (
    "OSDefineMetaClassAndStructors(NV3050Probe, IOService);\n"
    "static void use(IOService *provider) {\n"
    "    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);\n"
    "    IOLog(\"%p\", pci);\n"
    "}\n"
)


def good_plist():
    """A property list the checker must accept. Fresh dict on every call."""
    return {
        "CFBundleExecutable": "NV3050Probe",
        "CFBundleIdentifier": "local.nv3050.NV3050Probe",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "NV3050Probe",
        "CFBundlePackageType": "KEXT",
        "CFBundleShortVersionString": "0.1.0",
        "CFBundleVersion": "0.1.0",
        "OSBundleLibraries": {
            "com.apple.iokit.IOPCIFamily": "2.9",
            "com.apple.kpi.iokit": "19.0",
            "com.apple.kpi.libkern": "19.0",
        },
        "IOKitPersonalities": {
            "NV3050Probe": {
                "CFBundleIdentifier": "local.nv3050.NV3050Probe",
                "IOClass": "NV3050Probe",
                "IOProviderClass": "IOPCIDevice",
                "IOPCIMatch": "0x25a210de",
            },
        },
    }


class TreeBuilder(unittest.TestCase):
    """Base class: builds a throwaway kext tree and runs the checker over it."""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="kextbundle_")
        self.addCleanup(shutil.rmtree, self.root, True)
        self.kext = os.path.join(self.root, "phase0-kext")
        os.makedirs(self.kext)

    def write(self, plist=None, makefile=GOOD_MAKEFILE,
              cpp=GOOD_CPP, hpp=GOOD_HPP):
        if plist is not None:
            with open(os.path.join(self.kext, "Info.plist"), "wb") as handle:
                plistlib.dump(plist, handle)
        if makefile is not None:
            self._text("Makefile", makefile)
        if cpp is not None:
            self._text("NV3050Probe.cpp", cpp)
        if hpp is not None:
            self._text("NV3050Probe.hpp", hpp)

    def _text(self, name, body):
        with open(os.path.join(self.kext, name), "w", encoding="utf-8") as handle:
            handle.write(body)

    def run_checker(self, root=None):
        proc = subprocess.run(
            [sys.executable, CHECKER, root or self.root],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)
        return proc.returncode, proc.stdout

    def assertRejected(self, needle):
        """The run must be red, must reach a verdict, and must say why."""
        code, out = self.run_checker()
        self.assertNotEqual(code, 0, "checker accepted the mutation:\n%s" % out)
        self.assertIn("check-kext-bundle: FAILED", out,
                      "no verdict line -- the checker died rather than "
                      "rejecting:\n%s" % out)
        self.assertIn(needle, out,
                      "rejected, but for a reason that does not name %r, so "
                      "this control does not show the intended check fired:\n%s"
                      % (needle, out))
        return out

    def assertAccepted(self):
        code, out = self.run_checker()
        self.assertEqual(code, 0, "checker rejected a good tree:\n%s" % out)
        self.assertIn("check-kext-bundle: OK", out, out)
        return out


class GoodTreePasses(TreeBuilder):
    """The control on the controls. If this is red, nothing below means anything."""

    def test_the_unmutated_fixture_is_accepted(self):
        self.write(plist=good_plist())
        self.assertAccepted()

    def test_the_verdict_reports_a_positive_check_count(self):
        """``OK -- 0 checks`` is the failure mode a bare exit 0 produces."""
        self.write(plist=good_plist())
        out = self.assertAccepted()
        digits = [int(w) for w in out.replace("--", " ").split() if w.isdigit()]
        self.assertTrue(digits, "verdict line carries no count: %s" % out)
        self.assertGreater(max(digits), 5, out)


class ExecutableName(TreeBuilder):
    def test_an_executable_that_is_not_what_the_makefile_builds_is_caught(self):
        plist = good_plist()
        plist["CFBundleExecutable"] = "NV3050Prob"      # one character short
        self.write(plist=plist)
        self.assertRejected("CFBundleExecutable")

    def test_a_missing_executable_key_is_caught(self):
        plist = good_plist()
        del plist["CFBundleExecutable"]
        self.write(plist=plist)
        self.assertRejected("CFBundleExecutable")

    def test_renaming_the_makefile_target_alone_is_caught(self):
        """Drift in the other direction. The plist is untouched here.

        Without this the check could be reading the plist twice and comparing it
        with itself -- which passes the mutation above for the wrong reason.
        """
        self.write(plist=good_plist(),
                   makefile=GOOD_MAKEFILE.replace("NV3050Probe", "NVSomething"))
        self.assertRejected("CFBundleExecutable")


class BundleIdentifier(TreeBuilder):
    def test_drift_from_the_makefile_bundle_id_is_caught(self):
        plist = good_plist()
        plist["CFBundleIdentifier"] = "local.nv3050.Probe"
        plist["IOKitPersonalities"]["NV3050Probe"]["CFBundleIdentifier"] = \
            "local.nv3050.Probe"
        self.write(plist=plist)
        self.assertRejected("BUNDLE_ID")

    def test_a_syntactically_invalid_identifier_is_caught(self):
        """Separating input: the Makefile agrees, so only the shape check fires.

        If this test also changed BUNDLE_ID's spelling away from the plist, the
        drift check above would fire and the shape check could be dead code.
        """
        bad = "local nv3050 probe"
        plist = good_plist()
        plist["CFBundleIdentifier"] = bad
        plist["IOKitPersonalities"]["NV3050Probe"]["CFBundleIdentifier"] = bad
        self.write(plist=plist,
                   makefile="KEXT_NAME := NV3050Probe\nBUNDLE_ID := %s\n" % bad)
        out = self.assertRejected("not a usable bundle identifier")
        self.assertNotIn("BUNDLE_ID is", out,
                         "the drift check fired too, so this input does not "
                         "isolate the shape check:\n%s" % out)

    def test_the_makefile_variable_is_expanded_before_comparing(self):
        """BUNDLE_ID is ``local.nv3050.$(KEXT_NAME)``.

        A checker that compared the literal text would reject the real tree, so
        this pins that expansion happens rather than being accidental.
        """
        self.write(plist=good_plist(),
                   makefile="KEXT_NAME := NV3050Probe\n"
                            "BUNDLE_ID := local.nv3050.$(KEXT_NAME)\n")
        self.assertAccepted()


class PackageTypeAndVersions(TreeBuilder):
    def test_a_non_kext_package_type_is_caught(self):
        plist = good_plist()
        plist["CFBundlePackageType"] = "BNDL"
        self.write(plist=plist)
        self.assertRejected("CFBundlePackageType")

    def test_a_non_numeric_bundle_version_is_caught(self):
        plist = good_plist()
        plist["CFBundleVersion"] = "0.1.0-dev"
        self.write(plist=plist)
        self.assertRejected("CFBundleVersion")

    def test_a_non_numeric_short_version_is_caught(self):
        plist = good_plist()
        plist["CFBundleShortVersionString"] = "alpha"
        self.write(plist=plist)
        self.assertRejected("CFBundleShortVersionString")


class BundleLibraries(TreeBuilder):
    def test_an_empty_library_dict_is_caught(self):
        plist = good_plist()
        plist["OSBundleLibraries"] = {}
        self.write(plist=plist)
        self.assertRejected("OSBundleLibraries")

    def test_dropping_iopcifamily_while_the_sources_use_it_is_caught(self):
        """The load-time undefined-symbol case, and the reason for the scan."""
        plist = good_plist()
        del plist["OSBundleLibraries"]["com.apple.iokit.IOPCIFamily"]
        self.write(plist=plist)
        self.assertRejected("com.apple.iokit.IOPCIFamily")

    def test_dropping_libkern_while_the_sources_use_osmetaclass_is_caught(self):
        plist = good_plist()
        del plist["OSBundleLibraries"]["com.apple.kpi.libkern"]
        self.write(plist=plist)
        self.assertRejected("com.apple.kpi.libkern")

    def test_a_library_requirement_is_read_from_the_sources_not_a_fixed_list(self):
        """Sources that never mention IOPCIDevice must not demand IOPCIFamily.

        This is what makes the requirement scan worth having rather than a typed
        list: it must be able to come out either way. If dropping the library is
        rejected regardless of what the sources say, the scan is not running.
        """
        plist = good_plist()
        del plist["OSBundleLibraries"]["com.apple.iokit.IOPCIFamily"]
        plist["IOKitPersonalities"]["NV3050Probe"]["IOProviderClass"] = "IOService"
        self.write(plist=plist,
                   cpp="OSDefineMetaClassAndStructors(NV3050Probe, IOService);\n"
                       "static void use(void) { IOLog(\"x\"); }\n")
        self.assertAccepted()

    def test_a_library_that_is_not_an_apple_kpi_is_caught(self):
        plist = good_plist()
        plist["OSBundleLibraries"]["org.example.Something"] = "1.0"
        self.write(plist=plist)
        self.assertRejected("org.example.Something")

    def test_a_malformed_library_version_is_caught(self):
        plist = good_plist()
        plist["OSBundleLibraries"]["com.apple.kpi.iokit"] = "19.0b1"
        self.write(plist=plist)
        self.assertRejected("com.apple.kpi.iokit")

    def test_mixed_kpi_versions_are_caught(self):
        plist = good_plist()
        plist["OSBundleLibraries"]["com.apple.kpi.libkern"] = "20.0"
        self.write(plist=plist)
        self.assertRejected("different versions")

    def test_iopcifamily_does_not_count_toward_the_kpi_version_set(self):
        """IOPCIFamily is a kext, versioned on its own -- 2.9 next to 19.0 is fine.

        The good tree already contains exactly that pair, so if the mixed-version
        check counted it, GoodTreePasses would be red. Asserted separately
        because that dependency is invisible from either test alone.
        """
        plist = good_plist()
        plist["OSBundleLibraries"]["com.apple.iokit.IOPCIFamily"] = "1.2.3"
        self.write(plist=plist)
        self.assertAccepted()


class Personalities(TreeBuilder):
    def test_no_personalities_at_all_is_caught(self):
        plist = good_plist()
        plist["IOKitPersonalities"] = {}
        self.write(plist=plist)
        self.assertRejected("IOKitPersonalities")

    def test_a_personality_owned_by_a_different_bundle_is_caught(self):
        plist = good_plist()
        plist["IOKitPersonalities"]["NV3050Probe"]["CFBundleIdentifier"] = \
            "local.nv3050.Other"
        self.write(plist=plist)
        self.assertRejected("does not exist and never instantiate")

    def test_an_ioclass_no_source_defines_is_caught(self):
        plist = good_plist()
        plist["IOKitPersonalities"]["NV3050Probe"]["IOClass"] = "NV3050Prove"
        self.write(plist=plist)
        self.assertRejected("NV3050Prove")

    def test_declared_but_never_defined_is_reported_distinctly(self):
        """OSDeclareDefaultStructors alone registers nothing with the runtime.

        A different defect from a typo, and the operator needs to be told which:
        one is fixed in the plist, the other in the .cpp.
        """
        self.write(plist=good_plist(),
                   cpp="static void use(IOService *p) {\n"
                       "    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, p);\n"
                       "    IOLog(\"%p\", pci);\n"
                       "}\n")
        self.assertRejected("nothing \nregisters it".replace("\n", ""))

    def test_a_personality_that_is_not_a_dictionary_is_caught(self):
        plist = good_plist()
        plist["IOKitPersonalities"]["Bogus"] = "not a dict"
        self.write(plist=plist)
        self.assertRejected("not a dictionary")


class RefusalPaths(TreeBuilder):
    """Conditions under which no check can run. These must not read as passes."""

    def test_a_missing_plist_is_a_failure_not_a_pass(self):
        self.write(plist=None)
        self.assertRejected("Info.plist")

    def test_an_unparseable_plist_is_a_failure(self):
        self.write(plist=good_plist())
        with open(os.path.join(self.kext, "Info.plist"), "w",
                  encoding="utf-8") as handle:
            handle.write("<plist><dict><key>oops")
        self.assertRejected("does not parse")

    def test_a_missing_makefile_is_a_failure(self):
        self.write(plist=good_plist(), makefile=None)
        self.assertRejected("Makefile")

    def test_a_makefile_with_no_kext_name_is_a_failure(self):
        self.write(plist=good_plist(),
                   makefile="BUNDLE_ID := local.nv3050.NV3050Probe\n")
        self.assertRejected("KEXT_NAME")

    def test_a_makefile_with_no_bundle_id_is_a_failure(self):
        self.write(plist=good_plist(), makefile="KEXT_NAME := NV3050Probe\n")
        self.assertRejected("BUNDLE_ID")

    def test_a_tree_with_no_sources_is_a_failure(self):
        self.write(plist=good_plist(), cpp=None, hpp=None)
        self.assertRejected("no sources found")


class HelperUnits(unittest.TestCase):
    """The parsing helpers, driven directly."""

    @classmethod
    def setUpClass(cls):
        cls.mod = load_checker_module()

    def test_comments_are_not_parsed_as_assignments(self):
        variables = self.mod.make_variables(GOOD_MAKEFILE)
        self.assertEqual(variables["KEXT_NAME"], "NV3050Probe")

    def test_append_is_not_treated_as_a_definition(self):
        """``CXXFLAGS += -fapple-kext`` must not overwrite the ``:=`` value."""
        variables = self.mod.make_variables(GOOD_MAKEFILE)
        self.assertEqual(variables["CXXFLAGS"], "-mkernel")

    def test_expand_resolves_a_reference(self):
        self.assertEqual(
            self.mod.expand("a.$(B).c", {"B": "mid"}), "a.mid.c")

    def test_expand_leaves_an_unknown_reference_visible(self):
        """An unresolved $(FOO) must survive, not silently become ''.

        An empty expansion would make BUNDLE_ID compare equal to a truncated
        identifier in some trees, which is the quiet direction to be wrong in.
        """
        self.assertEqual(self.mod.expand("a.$(NOPE)", {}), "a.$(NOPE)")

    def test_expand_terminates_on_a_self_reference(self):
        self.assertIsInstance(self.mod.expand("$(A)", {"A": "$(A)"}), str)

    def test_defined_and_declared_are_distinguished(self):
        self.assertEqual(self.mod.defined_classes(GOOD_CPP),
                         {"NV3050Probe": "IOService"})
        self.assertEqual(self.mod.declared_classes(GOOD_HPP), {"NV3050Probe"})
        self.assertEqual(self.mod.defined_classes(GOOD_HPP), {})


class RealTree(unittest.TestCase):
    """The positive control: the checker must accept the tree as it stands."""

    def test_the_real_phase0_kext_is_accepted(self):
        proc = subprocess.run(
            [sys.executable, CHECKER, ROOT],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertIn("check-kext-bundle: OK", proc.stdout)

    def test_the_checker_exists_and_is_executable_by_python(self):
        self.assertTrue(os.path.isfile(CHECKER), CHECKER)


if __name__ == "__main__":
    unittest.main(verbosity=2)
