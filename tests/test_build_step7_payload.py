#!/usr/bin/env python3
"""test_build_step7_payload.py -- does the rebuild tool actually protect the
payload, or does it just move files?

Why this module exists
----------------------
``tools/build-step7-payload.py`` replaced three hand-typed commands whose
ordering requirement had already produced one real defect: both
``}``-instead-of-``fi`` fixes were made in ``tools/guest-build-kext.sh`` while the
staged copy and the burned ISO still carried the broken version, so the syntax
error was reported fixed while the artifact the operator would actually run still
had it.

The tool's whole value claim is **verify before anything lands**: build into a
temp directory, compare against the working tree there, and only then move into
``build-payload/``. If that check is weak -- or if it silently passes -- the tool
is strictly worse than the three commands, because it *looks* like a guarantee.

So this module does two separate things:

1. Drives the tool end to end into a temp ``--out-dir`` and asserts the artifact
   it produces has the shape the guest script needs.
2. Calls ``verify()`` directly against **deliberately wrong** payloads and
   asserts each is caught, naming the right fault. That is the negative control,
   and it lives here rather than in a hand-run ``_negctl_`` probe because it
   never writes anywhere but a temp directory -- unlike
   ``_negctl_payload_freshness.py``, which has to repoint a class attribute on
   another module.

Nothing here touches ``build-payload/`` or ``phase0-kext/``. A test that mutated
the tree would fake a named FAIL for any gate run happening concurrently in
another terminal, which has already happened once in this project.
"""

import ast
import importlib.util
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_TESTS_DIR)
TOOL = os.path.join(_REPO, "tools", "build-step7-payload.py")


def code_only(text):
    """``text`` with the module docstring removed.

    Every check in ``ShapeTests`` is a claim about what the tool DOES. The module
    docstring is prose about what it deliberately does NOT do -- it names
    ``payload-stage/`` as the staging directory that was abandoned, and quotes
    the three hand-typed commands it replaced. A substring test over the whole
    file therefore fails on the tool's own documentation, which is a defect in
    the test rather than in the tool. That is exactly what happened the first
    time this module ran.

    Parsed out with ``ast`` rather than sliced on a ``\"\"\"`` pair, because the
    file below the docstring contains several more of those and a naive slice
    would cut at the wrong one -- silently returning too much and making these
    checks vacuous rather than wrong. ``ShapeTests`` has a guard test that the
    strip actually removed the prose and kept the code.
    """
    doc = ast.get_docstring(ast.parse(text), clean=False)
    if doc is None:
        return text
    return text.replace(doc, "", 1)


def load_tool():
    """Import the tool by path -- its filename has hyphens, so it is not a
    legal module name and cannot be imported by ``import``.
    """
    spec = importlib.util.spec_from_file_location("build_step7_payload", TOOL)
    module = importlib.util.module_from_spec(spec)
    sys.modules["build_step7_payload"] = module
    spec.loader.exec_module(module)
    return module


TOOLMOD = load_tool()


class ShapeTests(unittest.TestCase):
    """Static properties of the tool that no end-to-end run would notice."""

    def setUp(self):
        with open(TOOL, "r", encoding="utf-8") as handle:
            self.text = handle.read()
        self.code = code_only(self.text)

    def test_the_docstring_strip_removed_prose_and_kept_code(self):
        # The guard on code_only(). If the strip silently returned the whole
        # file, every check below would still pass on the prose and measure
        # nothing; if it over-cut, they would pass on an empty string.
        self.assertNotIn("Why this exists", self.code)
        self.assertNotIn("payload-stage", self.code)
        self.assertIn("def tree_members", self.code)
        self.assertIn("def verify", self.code)

    def test_the_file_list_is_walked_not_hardcoded(self):
        # A hardcoded list is the same defect the tool exists to remove, one
        # level up: it goes stale the moment a sixth header lands, and the
        # payload silently ships nine files while the tree has ten.
        self.assertIn("os.walk", self.code)
        for name in ("NVDecode.hpp", "NVFalcon.hpp", "NVGspBoot.hpp",
                     "NVGspStage.hpp", "NVRiscvUcode.hpp", "NV3050Probe.cpp"):
            self.assertNotIn(
                name, self.code,
                "%s is named literally; the member list must be derived by"
                " walking phase0-kext/" % name)

    def test_the_tar_is_written_by_tarfile_not_by_shelling_out(self):
        # GNU tar is not guaranteed on a Windows PATH, and a missing binary
        # would fail in a way that reads as a broken payload.
        self.assertIn("tarfile.open", self.code)
        self.assertNotIn('"tar"', self.code)
        self.assertNotIn("'tar'", self.code)

    def test_the_staging_directory_is_outside_the_repo(self):
        # payload-stage/ inside the tree duplicated every kext header basename
        # for hours, and the doc-path gate resolves by basename.
        self.assertIn("tempfile.mkdtemp", self.code)
        self.assertNotIn("payload-stage", self.code)

    def test_the_vmx_is_never_edited(self):
        # That edit is power-off only. deliver-via-cd.py refuses on a live VMX,
        # and this tool must not be the thing that argues with it.
        self.assertIn("--no-vmx", self.code)
        self.assertNotIn("--vmx", self.code.replace("--no-vmx", ""))

    def test_the_excluded_directory_is_the_host_build_output(self):
        self.assertEqual(TOOLMOD.EXCLUDED_DIRS, ("build",))

    def test_the_volume_ident_matches_what_the_operator_card_says(self):
        # The card tells the operator to run `sh /Volumes/PAYLOAD/...`.
        self.assertEqual(TOOLMOD.VOL_IDENT, "PAYLOAD")


class TreeMembersTests(unittest.TestCase):

    def test_it_finds_the_real_kext_sources(self):
        members = TOOLMOD.tree_members()
        names = [arcname for arcname, _ in members]
        self.assertIn("phase0-kext/Makefile", names)
        self.assertIn("phase0-kext/NV3050Probe.cpp", names)
        self.assertIn("phase0-kext/NVDecode.hpp", names)

    def test_every_member_is_rooted_at_the_depth_the_guest_expects(self):
        # The guest script cds into phase0-kext/ and runs make. A tar rooted one
        # level deeper passes every shape check and makes the guest report
        # "no Makefile", which reads as a missing file rather than a bad archive.
        for arcname, _ in TOOLMOD.tree_members():
            self.assertTrue(arcname.startswith("phase0-kext/"), arcname)
            self.assertNotIn("\\", arcname)

    def test_the_host_build_directory_is_excluded(self):
        for arcname, _ in TOOLMOD.tree_members():
            self.assertNotIn("/build/", "/" + arcname + "/")

    def test_the_list_is_sorted_so_two_rebuilds_are_comparable(self):
        members = TOOLMOD.tree_members()
        self.assertEqual(members, sorted(members))


class EndToEndTests(unittest.TestCase):
    """Run the tool for real into a temp directory.

    ``--out-dir`` is the reason this is possible without rewriting the real
    artifact, and ``--skip-gate`` keeps it from starting a nested Python-gate run
    that would collide in %TEMP% with a concurrent ``run-python-tests.bat``.
    """

    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="nv-t-step7-out-")
        cls.proc = subprocess.run(
            [sys.executable, TOOL, "--out-dir", cls.out, "--skip-gate"],
            cwd=_REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def test_it_exits_zero(self):
        self.assertEqual(self.proc.returncode, 0, self.proc.stdout)

    def test_it_produces_all_three_artifacts(self):
        for name in (TOOLMOD.TAR_NAME, TOOLMOD.SCRIPT_NAME, TOOLMOD.ISO_NAME):
            path = os.path.join(self.out, name)
            self.assertTrue(os.path.isfile(path), "%s not produced" % name)
            self.assertGreater(os.path.getsize(path), 0, name)

    def test_the_tar_carries_every_tree_file_byte_for_byte(self):
        expected = dict(TOOLMOD.tree_members())
        with tarfile.open(os.path.join(self.out, TOOLMOD.TAR_NAME)) as tar:
            shipped = {m.name: m for m in tar.getmembers() if m.isfile()}
            self.assertEqual(sorted(shipped), sorted(expected))
            for name, member in shipped.items():
                with open(expected[name], "rb") as disk:
                    self.assertEqual(tar.extractfile(member).read(),
                                     disk.read(), name)

    def test_member_ownership_is_neutralised(self):
        # Extraction in the guest happens as an ordinary user. A member owned by
        # a uid that does not exist there makes bsdtar attempt a chown it cannot
        # perform, and the warning reads as a corrupt archive.
        with tarfile.open(os.path.join(self.out, TOOLMOD.TAR_NAME)) as tar:
            for member in tar.getmembers():
                self.assertEqual(member.uid, 0, member.name)
                self.assertEqual(member.gid, 0, member.name)
                self.assertEqual(member.uname, "", member.name)
                self.assertEqual(member.gname, "", member.name)

    def test_the_archive_carries_no_pax_extension_members(self):
        # USTAR was chosen so the guest's bsdtar has the fewest ways to
        # surprise. PAX would prepend PaxHeaders.* members, which are files as
        # far as a naive reader is concerned.
        with tarfile.open(os.path.join(self.out, TOOLMOD.TAR_NAME)) as tar:
            for member in tar.getmembers():
                self.assertNotIn("PaxHeader", member.name)

    def test_the_staged_script_is_byte_identical_to_tools(self):
        with open(os.path.join(_REPO, "tools", TOOLMOD.SCRIPT_NAME),
                  "rb") as handle:
            authority = handle.read()
        with open(os.path.join(self.out, TOOLMOD.SCRIPT_NAME), "rb") as handle:
            self.assertEqual(handle.read(), authority)

    def test_the_staged_script_is_still_lf_only(self):
        # shutil.copyfile is binary, but a future refactor to a text-mode copy
        # would silently rewrite these to CRLF and the guest would report
        # `bad interpreter: /bin/sh^M` -- which reads as a missing shell.
        with open(os.path.join(self.out, TOOLMOD.SCRIPT_NAME), "rb") as handle:
            self.assertNotIn(b"\r\n", handle.read())

    def test_the_iso_is_larger_than_what_it_contains(self):
        iso = os.path.getsize(os.path.join(self.out, TOOLMOD.ISO_NAME))
        inputs = (os.path.getsize(os.path.join(self.out, TOOLMOD.TAR_NAME))
                  + os.path.getsize(os.path.join(self.out, TOOLMOD.SCRIPT_NAME)))
        self.assertGreater(iso, inputs)

    def test_skip_gate_says_so_rather_than_running_it_silently(self):
        # "the gate was skipped" and "the gate passed" must not be the same
        # output. A run that quietly omitted it would read as verified.
        self.assertIn("--skip-gate", self.proc.stdout)
        self.assertIn("not run", self.proc.stdout)
        self.assertNotIn("Ran ", self.proc.stdout)

    def test_it_does_not_touch_the_real_payload_directory(self):
        self.assertNotIn(os.path.join(_REPO, "build-payload"),
                         self.proc.stdout.replace(self.out, ""))


class GateTargetingTests(unittest.TestCase):
    """The branch ``--skip-gate`` short-circuits before reaching.

    ``PayloadFreshnessTests`` reads a hardcoded ``build-payload/`` path, so
    running it after building into some other directory would report on the
    artifact this invocation did NOT touch and read as a verification of the one
    it did. That is worse than no verification, because it is indistinguishable
    from a real green.

    The first version of this assertion lived in ``EndToEndTests``, which passes
    ``--skip-gate`` -- and ``--skip-gate`` is checked first, so the branch was
    never reached and the test measured the wrong message. Reaching it needs a
    non-default ``--out-dir`` with the gate NOT skipped, which is what this class
    does. No nested gate run happens, so there is no %TEMP% collision with a
    concurrent ``run-python-tests.bat``.
    """

    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="nv-t-step7-gate-")
        cls.proc = subprocess.run(
            [sys.executable, TOOL, "--out-dir", cls.out],
            cwd=_REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def test_it_still_succeeds(self):
        self.assertEqual(self.proc.returncode, 0, self.proc.stdout)

    def test_it_declines_to_run_the_gate_against_a_different_artifact(self):
        self.assertIn("not build-payload", self.proc.stdout)
        self.assertIn("Not run", self.proc.stdout)

    def test_it_did_not_actually_run_the_gate(self):
        # The far side: the decline message must be a refusal, not a label
        # printed next to a run that happened anyway. unittest's summary line
        # would appear in the captured output if it had.
        self.assertNotIn("Ran ", self.proc.stdout)
        self.assertNotIn("OK: payload rebuilt", self.proc.stdout)

    def test_it_still_built_and_verified_the_payload(self):
        # Declining the gate must not mean declining the check. The in-process
        # verify() runs either way.
        self.assertIn("verified against the working tree", self.proc.stdout)
        for name in (TOOLMOD.TAR_NAME, TOOLMOD.SCRIPT_NAME, TOOLMOD.ISO_NAME):
            self.assertTrue(os.path.isfile(os.path.join(self.out, name)), name)


class VerifyRefusalTests(unittest.TestCase):
    """The negative control: hand ``verify()`` a wrong payload, per fault.

    ``verify()`` currently only ever sees a correct payload in production, and a
    check that has only seen correct input is indistinguishable from one whose
    comparison silently does nothing. Each test below is a separating input: it
    must produce a problem, and the message must name the fault that is actually
    present rather than a neighbouring one.
    """

    def setUp(self):
        self.work = tempfile.mkdtemp(prefix="nv-t-step7-neg-")
        self.members = TOOLMOD.tree_members()
        self.tar = os.path.join(self.work, "p.tar")
        self.script = os.path.join(self.work, "s.sh")
        self.iso = os.path.join(self.work, "p.iso")
        # A staged script that matches tools/, so script faults do not leak into
        # tar tests and vice versa.
        shutil.copyfile(os.path.join(_REPO, "tools", TOOLMOD.SCRIPT_NAME),
                        self.script)

    def tearDown(self):
        shutil.rmtree(self.work, ignore_errors=True)

    def write_tar(self, members):
        TOOLMOD.build_tar(self.tar, members)

    def write_iso(self, padding=4096):
        # Big enough to clear the size check, so it never fires accidentally.
        size = (os.path.getsize(self.tar) + os.path.getsize(self.script)
                + padding)
        with open(self.iso, "wb") as handle:
            handle.write(b"\0" * size)

    def run_verify(self):
        return TOOLMOD.verify(self.tar, self.script, self.iso, self.members)

    def test_an_honest_payload_produces_no_problems(self):
        # The baseline. Without it, every "caught" below could be a fault this
        # harness introduced rather than the one under test.
        self.write_tar(self.members)
        self.write_iso()
        self.assertEqual(self.run_verify(), [])

    def test_a_missing_source_file_is_caught(self):
        dropped = [m for m in self.members
                   if not m[0].endswith("NVGspStage.hpp")]
        self.write_tar(dropped)
        self.write_iso()
        problems = self.run_verify()
        self.assertTrue(any("absent from the tar" in p and "NVGspStage.hpp" in p
                            for p in problems), problems)

    def test_a_file_the_tree_no_longer_has_is_caught(self):
        ghost = os.path.join(self.work, "NVGhost.hpp")
        with open(ghost, "w", encoding="utf-8") as handle:
            handle.write("/* deleted from the tree, still shipped */\n")
        self.write_tar(self.members + [("phase0-kext/NVGhost.hpp", ghost)])
        self.write_iso()
        problems = self.run_verify()
        self.assertTrue(any("not the tree" in p and "NVGhost.hpp" in p
                            for p in problems), problems)

    def test_a_single_flipped_byte_at_unchanged_length_is_caught(self):
        # The narrowest statement of the defect: any comparison based on size,
        # mtime, or member name survives this one.
        victim, path = next((n, p) for n, p in self.members
                            if n.endswith("NVDecode.hpp"))
        with open(path, "rb") as handle:
            body = bytearray(handle.read())
        mid = len(body) // 2
        body[mid] = 0x41 if body[mid] != 0x41 else 0x42
        flipped = os.path.join(self.work, "flipped.hpp")
        with open(flipped, "wb") as handle:
            handle.write(bytes(body))
        self.write_tar([(n, flipped if n == victim else p)
                        for n, p in self.members])
        self.write_iso()
        problems = self.run_verify()
        self.assertTrue(any("differs from the tree" in p and victim in p
                            for p in problems), problems)
        # And the far side: a content change must not be reported as a missing
        # or extra file, or the message sends the reader to the wrong problem.
        self.assertFalse(any("absent from the tar" in p or "not the tree" in p
                             for p in problems), problems)

    def test_a_truncated_file_is_caught(self):
        victim, path = next((n, p) for n, p in self.members
                            if n.endswith("NVDecode.hpp"))
        with open(path, "rb") as handle:
            body = handle.read()
        cut = os.path.join(self.work, "cut.hpp")
        with open(cut, "wb") as handle:
            handle.write(body[:len(body) // 2])
        self.write_tar([(n, cut if n == victim else p)
                        for n, p in self.members])
        self.write_iso()
        self.assertTrue(any("differs from the tree" in p
                            for p in self.run_verify()))

    def test_a_shipped_build_directory_is_caught(self):
        obj = os.path.join(self.work, "stale.o")
        with open(obj, "wb") as handle:
            handle.write(b"\x7fELF stale host object\n")
        self.write_tar(self.members
                       + [("phase0-kext/build/NV3050Probe.o", obj)])
        self.write_iso()
        problems = self.run_verify()
        self.assertTrue(any("build/" in p for p in problems), problems)

    def test_a_staged_script_that_drifted_from_tools_is_caught(self):
        # The drift that really happened: both `}`-instead-of-`fi` fixes were
        # made in tools/ while the staged copy kept the broken version.
        with open(self.script, "ab") as handle:
            handle.write(b"\n# an edit tools/ does not have\n")
        self.write_tar(self.members)
        self.write_iso()
        problems = self.run_verify()
        self.assertTrue(any("differs from tools/" in p for p in problems),
                        problems)

    def test_a_crlf_staged_script_is_caught(self):
        with open(os.path.join(_REPO, "tools", TOOLMOD.SCRIPT_NAME),
                  "rb") as handle:
            body = handle.read()
        with open(self.script, "wb") as handle:
            handle.write(body.replace(b"\n", b"\r\n"))
        self.write_tar(self.members)
        self.write_iso()
        problems = self.run_verify()
        self.assertTrue(any("CRLF" in p for p in problems), problems)

    def test_an_iso_smaller_than_its_inputs_is_caught(self):
        # Conclusive proof the image is older than what it should contain --
        # the shape a hand-run rebuild produces when the ISO step is forgotten.
        self.write_tar(self.members)
        with open(self.iso, "wb") as handle:
            handle.write(b"\0" * 2048)
        problems = self.run_verify()
        self.assertTrue(any("is not larger than its inputs" in p
                            for p in problems), problems)


if __name__ == "__main__":
    unittest.main(verbosity=2)
