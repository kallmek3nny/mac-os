#!/usr/bin/env python3
"""
test_check_documented_paths.py -- negative controls for
``tools/check-documented-paths.py``, plus one positive control over the real
CLAUDE.md.

Why this exists
---------------
The checker under test reads prose and decides whether the paths in it are
real. Every verdict it can return is a judgement call about English, which
means it has more ways to be quietly wrong than the checkers below it: it can
fail on a correct sentence, and -- much worse -- it can pass on a broken one.

Both happened during its first hour. It reported six existing headers as
deliberately ABSENT because the paragraph introducing them says ``They include
no IOKit, no kernel types``, and it exited 0 while doing so. Delete every header
in phase0-kext/ and that run would still have printed success. It also excused
``tests/build`` by quoting ``BUILD_DIR`` from phase0-kext/Makefile, a build file
with no involvement in that directory at all.

So the rule this module enforces is the one that would have caught those: for
every verdict the checker can hand out, there is a mutation of a synthetic
repository that must turn it red, and a near-identical repository that must stay
green. A verdict with only the green half tested is the blind spot the checker
was written to remove, moved one level up.

How these tests are isolated
----------------------------
Each test builds its own ``tempfile.mkdtemp()`` repository and passes it to the
checker as ``argv[1]``. Nothing here writes inside the real project, so these
are safe to run while other terminals are editing CLAUDE.md -- except
``RealRepository``, which reads the live document deliberately and is the point.

What this does not prove
------------------------
That the checker understands English. It proves that its specific documented
rules -- adjacency, clause boundaries, unique basenames, producer attribution --
are each load-bearing, and that removing any one of them is observable.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKER = os.path.join(ROOT, "tools", "check-documented-paths.py")

#
# A Makefile and a CMakeLists that declare exactly what the real ones declare,
# reduced to the lines the checker reads. Fixtures that need a build product
# exemption start from these and mutate one line.
#
MAKEFILE_OK = (
    "KEXT_NAME    := NV3050Probe\n"
    "KEXT_BUNDLE  := $(KEXT_NAME).kext\n"
    "BUILD_DIR    := build\n"
)

CMAKE_OK = "add_kext_test(test_gsp_boot test_gsp_boot.cpp)\n"


def run(root, doc=None):
    """Run the checker over a repository root; return (exit code, output).

    `doc` supplies the optional second argument. Omitting it is the real gate's
    invocation, which reads root/CLAUDE.md.
    """
    argv = [sys.executable, CHECKER, root]
    if doc is not None:
        argv.append(doc)
    proc = subprocess.run(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
    )
    return proc.returncode, proc.stdout


class Fixtures(unittest.TestCase):
    """Builds synthetic repositories one file at a time."""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="docpaths-")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def write(self, relpath, text="placeholder\n"):
        full = os.path.join(self.root, relpath.replace("/", os.sep))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        return full

    def mkdir(self, relpath):
        full = os.path.join(self.root, relpath.replace("/", os.sep))
        os.makedirs(full, exist_ok=True)
        return full

    def doc(self, text):
        """Write the synthetic CLAUDE.md the checker will read."""
        return self.write("CLAUDE.md", text)

    def verdict_for(self, output, token):
        """The verdict word the checker printed for one token, or None.

        Asserting on the verdict rather than on the exit code is what catches a
        wrong GREEN. Six headers were once reported ABSENT at exit 0; a test
        that only checked the exit code would have called that a pass.
        """
        for line in output.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[1] == token:
                return parts[0]
        return None


class BrokenPaths(Fixtures):
    """A path named in prose that resolves nowhere must fail."""

    def test_missing_file_fails(self):
        self.doc("The decoder lives in `phase0-kext/NVDecode.hpp` today.\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("phase0-kext/NVDecode.hpp", out)

    def test_present_file_passes(self):
        self.write("phase0-kext/NVDecode.hpp")
        self.doc("The decoder lives in `phase0-kext/NVDecode.hpp` today.\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "phase0-kext/NVDecode.hpp"),
                         "DIRECT")

    def test_missing_documented_directory_fails(self):
        """A layout-table directory is a path even though it has no extension.

        This is the regression for a bug that made the checker skip every
        directory in the table: the trailing slash was stripped before the
        path-shape test, so `tools/` became `tools`, which has neither a
        separator nor a known extension and was discarded as prose.
        """
        self.doc("| Path | What it holds |\n"
                 "|---|---|\n"
                 "| `no-such-dir/` | something that does not exist |\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("no-such-dir", out)

    def test_present_documented_directory_passes(self):
        self.mkdir("kextshim")
        self.doc("| Path | What it holds |\n"
                 "|---|---|\n"
                 "| `kextshim/` | a fake IOKit |\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "kextshim"), "DIRECT")


class BareBasenames(Fixtures):
    """Prose names headers by basename alone; that must resolve, or be refused."""

    def test_unique_basename_resolves(self):
        self.write("phase0-kext/NVFalcon.hpp")
        self.doc("`NVFalcon.hpp` decodes the Falcon registers.\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "NVFalcon.hpp"), "TAIL")

    def test_ambiguous_basename_fails(self):
        """Two files sharing a name make a bare mention a coin flip, not a link."""
        self.write("phase0-kext/NVFalcon.hpp")
        self.write("attic/NVFalcon.hpp")
        self.doc("`NVFalcon.hpp` decodes the Falcon registers.\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("2 files", out)

    def test_vendored_tree_cannot_create_ambiguity(self):
        """A basename collision inside reference/ is NVIDIA's, not this repo's.

        reference/ holds vendored open-gpu-kernel-modules. If its filenames fed
        the index, an unrelated CLAUDE.md mention could become ambiguous through
        no change to this project at all -- a red gate nobody could fix.
        """
        self.write("phase0-kext/NVFalcon.hpp")
        self.write("reference/src/NVFalcon.hpp")
        self.doc("`NVFalcon.hpp` decodes the Falcon registers.\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "NVFalcon.hpp"), "TAIL")


class AbsenceClaims(Fixtures):
    """Prose that says a path is missing is checked in the opposite direction."""

    DOC = ("`tests/` has no `__init__.py`, so the module form of the command "
           "fails.\n")

    def test_satisfied_absence_passes(self):
        self.mkdir("tests")
        self.doc(self.DOC)
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "__init__.py"), "ABSENT")

    def test_violated_absence_fails(self):
        """The file appearing is what falsifies this sentence, not it missing."""
        self.write("tests/__init__.py")
        self.doc(self.DOC)
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("does not exist, but it does", out)

    def test_negation_in_a_neighbouring_clause_does_not_grant_absence(self):
        """The wrong-GREEN regression, and the reason this module exists.

        The sentence introducing the headers reads ``They include no IOKit, no
        kernel types``. A checker that asks only whether a negation appears
        somewhere nearby reads that as a claim the NEXT path does not exist, and
        hands it ABSENT -- passing whether or not the file is there. So the
        assertion here is on the verdict, not the exit code: this must resolve,
        not be excused.
        """
        self.write("phase0-kext/NVDecode.hpp")
        self.doc("They include no IOKit, no kernel types, and sleep never. "
                 "`NVDecode.hpp` holds the register constants.\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "NVDecode.hpp"), "TAIL")

    def test_neighbouring_clause_still_fails_when_the_path_is_dead(self):
        """The same sentence, with the file removed, must go red.

        Without this the test above proves nothing: a checker that granted
        ABSENT to everything would also exit 0 there. The pair together pins the
        behaviour -- the nearby negation must not be able to rescue a dead path.
        """
        self.doc("They include no IOKit, no kernel types, and sleep never. "
                 "`NVDecode.hpp` holds the register constants.\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("NVDecode.hpp", out)

    def test_negation_across_a_comma_does_not_grant_absence(self):
        """``A virtual CD does not, so `tools/x.sh` ...`` negates the CD."""
        self.write("tools/guest-build-kext.sh")
        self.doc("Every vmrun op needs the password. A virtual CD does not, so "
                 "`tools/guest-build-kext.sh` reduces it to one command.\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "tools/guest-build-kext.sh"),
                         "DIRECT")


class AbsenceInTables(Fixtures):
    """A markdown table is one passage, so cell boundaries must scope negation.

    A table row has no sentence punctuation in it. The paragraph split keeps the
    whole table together and the whitespace join turns it into a single line, so
    before the pipe joined SENTENCE_BREAK_RE the clause preceding any mention
    ran backwards through every earlier cell and every earlier row. Every test
    below asserts the VERDICT, not the exit code: a checker that hands out
    ABSENT freely also exits 0.
    """

    def test_absence_claim_inside_one_cell_is_still_read(self):
        """The real case from OPERATOR-ACTION-step7-build.md must keep working.

        Negation and path sit in the same cell (``| No stale host `build/`
        rides along |``), so scoping to the cell must not cost the claim.
        """
        self.mkdir("tests")
        self.doc("| Thing | How |\n|---|---|\n"
                 "| The tar has no `tests/__init__.py` inside it | same |\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "tests/__init__.py"), "ABSENT")

    def test_negation_in_a_previous_cell_does_not_grant_absence(self):
        self.write("tools/guest-build-kext.sh")
        self.doc("| Thing | How |\n|---|---|\n"
                 "| No credentials are needed "
                 "| `tools/guest-build-kext.sh` does the build |\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "tools/guest-build-kext.sh"),
                         "DIRECT")

    def test_previous_cell_still_fails_when_the_path_is_dead(self):
        """Without this the test above proves nothing -- see the clause pair."""
        self.doc("| Thing | How |\n|---|---|\n"
                 "| No credentials are needed "
                 "| `tools/guest-build-kext.sh` does the build |\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("tools/guest-build-kext.sh", out)

    def test_negation_in_a_previous_row_does_not_grant_absence(self):
        self.write("tools/guest-build-kext.sh")
        self.doc("| Thing | How |\n|---|---|\n"
                 "| No password is needed | ok |\n"
                 "| `tools/guest-build-kext.sh` runs the build | ok |\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "tools/guest-build-kext.sh"),
                         "DIRECT")

    def test_previous_row_still_fails_when_the_path_is_dead(self):
        self.doc("| Thing | How |\n|---|---|\n"
                 "| No password is needed | ok |\n"
                 "| `tools/guest-build-kext.sh` runs the build | ok |\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("tools/guest-build-kext.sh", out)


class GateCommands(Fixtures):
    """A command in a fenced block is an instruction and gets no tail match."""

    FENCE = "```sh\npython tools/check-registers.py\n```\n"

    def test_real_command_passes(self):
        self.write("tools/check-registers.py")
        self.doc(self.FENCE)
        code, out = run(self.root)
        self.assertEqual(code, 0, out)

    def test_moved_command_fails_despite_a_copy_elsewhere(self):
        """``python tools/check-registers.py`` fails if the file moved, even
        when some other copy of the basename still exists."""
        self.write("attic/check-registers.py")
        self.doc(self.FENCE)
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("documented as a command to run", out)


class BuildProducts(Fixtures):
    """Products absent on a bare checkout: assert the producer, not the file."""

    def test_kext_product_passes_when_the_makefile_declares_it(self):
        self.write("phase0-kext/Makefile", MAKEFILE_OK)
        self.doc("```sh\ncd phase0-kext && make\n```\n\n"
                 "That produces `build/NV3050Probe.kext`.\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "build/NV3050Probe.kext"),
                         "BUILD")

    def test_kext_product_fails_when_the_makefile_stops_making_a_bundle(self):
        self.write("phase0-kext/Makefile",
                   MAKEFILE_OK.replace("$(KEXT_NAME).kext", "$(KEXT_NAME)"))
        self.doc("That produces `build/NV3050Probe.kext`.\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("KEXT_BUNDLE", out)

    def test_test_binary_passes_when_cmake_registers_it(self):
        self.write("tests/CMakeLists.txt", CMAKE_OK)
        self.doc("```sh\ncd tests/build && ninja && ./test_gsp_boot.exe\n```\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "test_gsp_boot.exe"), "BUILD")

    def test_test_binary_fails_when_cmake_stops_registering_it(self):
        self.write("tests/CMakeLists.txt",
                   "add_kext_test(test_decode test_decode.cpp)\n")
        self.doc("```sh\ncd tests/build && ninja && ./test_gsp_boot.exe\n```\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("add_kext_test(test_gsp_boot)", out)

    def test_cmake_build_dir_is_not_excused_by_the_makefile(self):
        """``tests/build`` must be justified by CMake, not by the Makefile.

        Both build directories are literally named ``build``. Matching on the
        name alone let the CMake one pass while quoting the Makefile's
        ``BUILD_DIR`` -- a green verdict citing a build file with no
        involvement in that path. Here the Makefile is present and healthy and
        tests/CMakeLists.txt registers nothing, so the only correct answer is
        red.
        """
        self.write("phase0-kext/Makefile", MAKEFILE_OK)
        self.write("tests/CMakeLists.txt", "# nothing registered here\n")
        self.doc("```sh\ncd tests/build && ninja\n```\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("add_kext_test", out)


class Patterns(Fixtures):
    """A glob cannot be resolved, but it can be required to match something."""

    DOC = "`tests/*.cpp` compiles the identical header files under MSVC.\n"

    def test_matching_pattern_passes(self):
        self.write("tests/test_gsp_boot.cpp")
        self.doc(self.DOC)
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "tests/*.cpp"), "GLOB")

    def test_pattern_matching_nothing_fails(self):
        self.mkdir("tests")
        self.doc(self.DOC)
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("matches no files", out)


class OutOfRepositoryPaths(Fixtures):
    """Paths on another host or another install are not this gate's business."""

    def test_guest_absolute_path_is_not_a_repo_path(self):
        self.doc("The ISO mounts under `/Volumes` inside the guest.\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "/Volumes"), "ABSOLUTE")

    def test_site_packages_path_is_not_a_repo_path(self):
        self.doc("`site-packages/tests/__init__.py` exists on this machine.\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(
            self.verdict_for(out, "site-packages/tests/__init__.py"), "OUTSIDE")


class Extraction(Fixtures):
    """The checker must not pass by finding nothing to check."""

    def test_no_tokens_at_all_fails(self):
        """Silence is the failure mode this whole family of gates exists for.

        Break the token regex and a checker that reports success on zero paths
        is indistinguishable from one that verified them all.
        """
        self.doc("This document names `NV_PGSP_BASE` and `nvGspFalconReg()` "
                 "and nothing else.\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("extraction", out)

    def test_missing_document_fails(self):
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("CLAUDE.md", out)

    def test_relative_prefix_is_not_turned_into_an_absolute_path(self):
        """``./test_gsp_boot.exe`` must not be read as ``/test_gsp_boot.exe``.

        Stripping punctuation before handling the ``./`` prefix left a leading
        slash, which resolves nowhere and reported a dead path for a command
        that works.
        """
        self.write("tests/CMakeLists.txt", CMAKE_OK)
        self.doc("```sh\n./test_gsp_boot.exe\n```\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertIsNone(self.verdict_for(out, "/test_gsp_boot.exe"))


class DocumentScope(Fixtures):
    """The checker must REFUSE any document that is not the root CLAUDE.md.

    Every resolution in ``classify()`` is root-relative -- there is no
    doc-relative concept in the tool at all. That is correct only while the one
    document sits at the repository root, where root-relative and doc-relative
    coincide. A subdirectory doc writes ``../PLAN.md`` and means the root; judged
    against the root that is a dead path, so a correct reference would be
    reported broken. The reverse is worse: a doc-relative ``tools/x.py`` could
    resolve to a same-named root-level file and be called fine.

    Until this class existed, that constraint lived only in how ``main()``
    happened to be written. Pointing the tool at a second document was a
    one-line change and nothing would have objected -- which is precisely the
    collision the crew spent a task settling between this module and
    ``tests/test_documented_paths.py``. These tests turn the constraint into a
    refusal with a reason.
    """

    DOC = "The decoder lives in `phase0-kext/NVDecode.hpp` today.\n"

    def test_a_document_that_is_not_claude_md_is_refused(self):
        self.write("phase0-kext/NVDecode.hpp")
        self.doc(self.DOC)
        other = self.write("tests/README.md", self.DOC)
        code, out = run(self.root, other)
        self.assertEqual(code, 1, out)
        self.assertIn("CLAUDE.md only", out)

    def test_a_claude_md_below_the_root_is_refused(self):
        """Right filename, wrong base -- the case that reads as harmless.

        A subdirectory CLAUDE.md passes any name check, so only the parent test
        catches it. This is the shape of the doc-relative bug: nothing about the
        filename is wrong, the BASE is.
        """
        self.write("phase0-kext/NVDecode.hpp")
        self.doc(self.DOC)
        nested = self.write("phase0-kext/CLAUDE.md", self.DOC)
        code, out = run(self.root, nested)
        self.assertEqual(code, 1, out)
        self.assertIn("not at the repository root", out)

    def test_the_refusal_names_the_multi_doc_alternative(self):
        """A refusal that does not say where to go instead gets worked around."""
        self.doc(self.DOC)
        nested = self.write("tools/CLAUDE.md", self.DOC)
        code, out = run(self.root, nested)
        self.assertEqual(code, 1, out)
        self.assertIn("test_documented_paths.py", out)

    def test_the_root_document_named_explicitly_is_accepted(self):
        """The paired positive.

        Without it the three refusals above could pass because the guard rejects
        every second argument, including the correct one -- which would make the
        guard a break rather than a boundary.
        """
        self.write("phase0-kext/NVDecode.hpp")
        explicit = self.doc(self.DOC)
        code, out = run(self.root, explicit)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "phase0-kext/NVDecode.hpp"),
                         "DIRECT")

    def test_the_default_invocation_still_reads_the_root_document(self):
        """The gate passes no second argument; the guard must not disturb it."""
        self.write("phase0-kext/NVDecode.hpp")
        self.doc(self.DOC)
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.verdict_for(out, "phase0-kext/NVDecode.hpp"),
                         "DIRECT")

    #
    # The two below arrived from another terminal as a SECOND class also named
    # DocumentScope, appended after RealRepository. Python keeps the last
    # binding, so that definition silently replaced this one and the five tests
    # above never ran -- discovery reported ok for a class that had been
    # overwritten. Merged here rather than renamed: two classes asserting the
    # same property under the same name is the collision, not the fix.
    #
    # They approach scope from the other side. The five above check the CLI
    # guard rejects a wrong document; these two check that no second document is
    # read even when one is present.
    #

    def test_only_the_root_document_is_read(self):
        """A path broken in tests/README.md must not appear in the output.

        ``README.md`` inside ``tests/README.md`` resolves to a different real
        file under root-relative and doc-relative ordering -- both exist, and
        neither ordering errors. The gate would report a path verified while
        naming a file the doc was not talking about.
        """
        self.write("tools/check-registers.py")
        self.write("tests/README.md",
                   "This names `no-such-file-anywhere.py`, which is not here.\n")
        self.doc("`tools/check-registers.py` is a gate.\n")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertNotIn("no-such-file-anywhere.py", out,
                         "a doc other than CLAUDE.md was scanned -- if that is "
                         "now intended, this checker needs doc-relative "
                         "resolution before it is true")

    def test_a_broken_path_in_the_root_document_still_fails(self):
        """The negative control for the test above. Without it, exit 0 proves
        nothing about scope -- a checker that cannot fail returns it too.
        """
        self.doc("This names `no-such-file-anywhere.py`, which is not here.\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("no-such-file-anywhere.py", out)


class RealRepository(unittest.TestCase):
    """The live CLAUDE.md, which is the only reason any of the above matters.

    This is the assertion with teeth: rename a header, move a gate, delete a
    handoff file, or add ``tests/__init__.py``, and this goes red in the one
    gate that runs on a bare checkout.
    """

    def test_live_document_paths_all_resolve(self):
        code, out = run(ROOT)
        self.assertEqual(code, 0, out)

    def test_live_document_yields_a_substantial_number_of_paths(self):
        """Pin the SHAPE, not the magnitude.

        An exact count here would be a number in prose that nothing keeps true
        -- it would go red every time a peer adds a handoff file, and the fix
        would be to edit the expectation, which trains the reflex that turns a
        gate into decoration. A floor catches the failure that matters: an
        extraction bug that quietly narrows what is checked.
        """
        code, out = run(ROOT)
        self.assertEqual(code, 0, out)
        counted = [line for line in out.splitlines()
                   if line.startswith("  ") and len(line.split()) >= 2]
        self.assertGreaterEqual(len(counted), 30, out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
