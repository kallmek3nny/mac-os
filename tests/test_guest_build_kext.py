#!/usr/bin/env python3
"""
test_guest_build_kext.py -- exercise tools/guest-build-kext.sh ON THE HOST.

Why this exists
---------------
``guest-build-kext.sh`` is the guest half of STEP 7: it is written on Windows,
shipped into the macOS guest on a payload ISO by ``tools/deliver-via-cd.py``,
and run once from the guest console by a human. That is the whole problem. A
script delivered that way gets exactly one attempt in front of the one person
who cannot easily report back what it printed, and every ``vmrun`` path that
would let the host run it is blocked on the account password
(``OPERATOR-ACTION-guest-creds.md``).

So a defect in this script is not a normal bug. It burns a power cycle and an
operator round trip, and it reports the *harness* rather than the code under
test -- the same failure shape as the bare-LF batch files that
``test_line_endings.py`` exists to prevent.

Two of those defects were already in it. The first draft closed two ``if``
blocks with ``}`` instead of ``fi``; ``sh -n`` rejected the whole file, so the
script would have died at line 1 in the guest with a syntax error and produced
no log at all. The second was subtler and is the reason this file is not just a
``sh -n`` call: ``find_payload()`` globbed a hardcoded ``/Volumes`` while the
rest of the script had been parameterised on ``$PAYLOAD_ROOT``, so the override
that makes host testing possible was dead code and every test below would have
exercised the real ``/Volumes`` -- which does not exist on Windows, meaning
every test would have taken the "no payload" branch and passed for the wrong
reason.

WHAT THIS CHECKS, AND WHAT IT CANNOT
------------------------------------
It runs the script for real under ``sh`` with ``NV_PAYLOAD_ROOT`` and
``NV_WORK_DIR`` pointed at temporary directories, and asserts the three refusal
paths. It does NOT and cannot check the build: ``make`` needs Apple clang with
``-mkernel -fapple-kext`` and the Kernel framework, which is the entire reason
STEP 7 is still open. Every test here stops before ``make`` is reached.

The narrowest honest claim: the script's *plumbing* -- payload discovery,
unpack, tree validation, and the messages each failure prints -- works. Whether
the kext compiles is unknown and this file does not speak to it.

Run:
    python -m unittest discover -s tests -p test_guest_build_kext.py -v
    tests\\run-python-tests.bat        # picked up automatically by discovery
"""

import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_TESTS_DIR)
SCRIPT = os.path.join(_REPO, "tools", "guest-build-kext.sh")

# Git Bash ships /bin/sh. If it is ever absent the tests skip rather than fail:
# a missing interpreter is an environment fault and reporting it as a script
# defect is exactly the confusion this module was written to avoid.
SH = shutil.which("sh")

PAYLOAD_TAR_NAME = "kext-src.tar"


def posix_path(win_path):
    """``C:\\Users\\x`` -> ``/c/Users/x``, the form Git Bash's tools accept.

    This is not cosmetic and it cost two red tests to find. A Windows path
    handed to ``sh`` survives some operations and not others: the glob in
    ``find_payload`` expanded a ``C:\\...`` root and ``[ -f ]`` agreed the tar
    was there, so discovery passed -- and then ``tar -xf ... -C "$WORK_DIR"``
    failed, because the backslashes are escape characters to the shell and to
    tar's own path handling. The script printed "tar could not unpack", which
    is a truthful message about a fault that existed only in this harness.

    That is the exact failure mode this module exists to prevent, arriving from
    the test side: a green-or-red signal that describes the harness rather than
    the code under test. Inside the guest every path is already POSIX, so the
    conversion is host-test-only and the script needs no knowledge of it.

    ``cygpath`` would do this too, but it is not guaranteed present and a
    missing helper would skip these tests rather than run them.
    """
    path = win_path.replace("\\", "/")
    if len(path) > 1 and path[1] == ":":
        path = "/%s%s" % (path[0].lower(), path[2:])
    return path


def read_script():
    with open(SCRIPT, "r", encoding="utf-8") as handle:
        return handle.read()


class ScriptShapeTests(unittest.TestCase):
    """Static checks. These run even where no ``sh`` exists."""

    def test_the_script_exists(self):
        self.assertTrue(os.path.isfile(SCRIPT), SCRIPT)

    def test_it_is_posix_sh_not_bash(self):
        """The guest's login shell is zsh; /bin/sh is the guaranteed one.

        A ``#!/bin/bash`` here would still work on Sonoma today, but bash 3.2 is
        deprecated on macOS and the script uses nothing bash-specific, so the
        weaker dependency is the correct one.
        """
        self.assertTrue(read_script().startswith("#!/bin/sh"))

    def test_line_endings_are_lf_only(self):
        """CRLF would break this the way bare LF breaks a .bat.

        ``test_line_endings.py`` enforces CRLF on ``.bat``/``.cmd`` because
        cmd.exe seeks by byte offset. The mirror-image hazard applies here: a
        shell script with CRLF fails in the guest with ``bad interpreter:
        /bin/sh^M``, which names the interpreter and reads as a broken guest.
        """
        with open(SCRIPT, "rb") as handle:
            data = handle.read()
        self.assertNotIn(b"\r\n", data,
                         "CRLF in a .sh breaks the shebang inside the guest")

    def test_payload_discovery_uses_the_overridable_root(self):
        """The dead-override defect, pinned.

        ``find_payload`` must glob ``$PAYLOAD_ROOT``, not a literal ``/Volumes``.
        If it hardcodes the path, every execution test below silently exercises
        a directory that does not exist on this host and passes vacuously.
        """
        text = read_script()
        start = text.index("find_payload()")
        body = text[start:text.index("\n}\n", start)]
        self.assertIn("$PAYLOAD_ROOT", body,
                      "find_payload must honour PAYLOAD_ROOT")
        self.assertNotIn("/Volumes", body,
                         "find_payload still hardcodes /Volumes")

    def test_the_default_root_is_still_the_real_guest_path(self):
        """The override must not change behaviour when it is unset.

        The far side of the test above. A parameterisation that defaulted to a
        temp directory would pass every test here and find nothing in the guest.
        """
        self.assertIn('PAYLOAD_ROOT="${NV_PAYLOAD_ROOT:-/Volumes}"',
                      read_script())

    def test_the_work_dir_default_is_the_home_directory(self):
        """Unpacking onto the mounted ISO would fail: the medium is read-only."""
        self.assertIn('WORK_DIR="${NV_WORK_DIR:-$HOME/nv3050-step7}"',
                      read_script())

    def test_the_toolchain_is_recorded_before_make_runs(self):
        """A build log that does not name its clang cannot be acted on remotely.

        ``xcode-select`` pointing at the Command Line Tools rather than a full
        Xcode is the most likely reason a first ``-fapple-kext`` build fails, and
        that is invisible unless the log says so. Ordering matters: recorded
        after a failing ``make``, the block may never be reached.
        """
        text = read_script()
        self.assertLess(text.index("xcode-select -p"), text.index("--- make ---"))
        self.assertLess(text.index("Kernel.framework"), text.index("--- make ---"))


@unittest.skipUnless(SH, "no sh on PATH")
class SyntaxTests(unittest.TestCase):

    def test_sh_n_accepts_the_script(self):
        """The `}` instead of `fi` defect. Two of these were really in it."""
        proc = subprocess.run([SH, "-n", SCRIPT],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0,
                         "sh -n rejected the script:\n%s" % proc.stderr)


@unittest.skipUnless(SH, "no sh on PATH")
class RefusalPathTests(unittest.TestCase):
    """Run the script for real, with the two paths redirected to temp dirs.

    Each test builds only as much of a fake payload as its own branch needs, so
    a test that reached further than intended would fail rather than skip ahead.
    """

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="nv-payload-root-")
        self.work = os.path.join(tempfile.mkdtemp(prefix="nv-work-"), "w")
        # What the SCRIPT is given. Not the same strings: see posix_path().
        self.root_sh = posix_path(self.root)
        self.work_sh = posix_path(self.work)
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.addCleanup(shutil.rmtree, os.path.dirname(self.work),
                        ignore_errors=True)

    def run_script(self):
        env = dict(os.environ)
        env["NV_PAYLOAD_ROOT"] = self.root_sh
        env["NV_WORK_DIR"] = self.work_sh
        return subprocess.run([SH, SCRIPT], capture_output=True, text=True,
                              env=env, timeout=120)

    def make_volume(self, name="PAYLOAD"):
        path = os.path.join(self.root, name)
        os.makedirs(path, exist_ok=True)
        return path

    def test_no_payload_at_all_is_refused(self):
        proc = self.run_script()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("FAIL: no %s" % PAYLOAD_TAR_NAME, proc.stdout)

    def test_the_refusal_names_the_root_it_actually_searched(self):
        """A message naming /Volumes while searching elsewhere misleads twice.

        This is what makes the previous test non-vacuous: it proves the script
        looked where it was told, not that it failed for some unrelated reason.
        """
        proc = self.run_script()
        self.assertIn(self.root_sh, proc.stdout)

    def test_a_volume_without_the_tar_is_still_refused(self):
        """An empty mount point must not read as a payload."""
        self.make_volume()
        proc = self.run_script()
        self.assertEqual(proc.returncode, 1)
        self.assertIn("FAIL: no %s" % PAYLOAD_TAR_NAME, proc.stdout)

    def test_a_corrupt_tar_is_reported_as_a_tar_failure(self):
        """Not as a missing payload, and not as a missing Makefile.

        Three refusals share one exit code, so the message is the only thing
        distinguishing them -- and it is the only thing the operator can relay.
        """
        vol = self.make_volume()
        with open(os.path.join(vol, PAYLOAD_TAR_NAME), "wb") as handle:
            handle.write(b"this is not a tar archive" * 64)
        proc = self.run_script()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("tar could not unpack", proc.stdout)

    def test_a_tar_without_the_kext_tree_is_refused(self):
        """A well-formed archive of the WRONG thing.

        This is the case that proves the payload was actually found, unpacked,
        and inspected -- the two tests above stop earlier, so on their own they
        cannot tell a working discovery path from a broken one.
        """
        vol = self.make_volume()
        stray = os.path.join(vol, "stray.txt")
        with open(stray, "w", encoding="utf-8") as handle:
            handle.write("wrong payload\n")
        with tarfile.open(os.path.join(vol, PAYLOAD_TAR_NAME), "w") as tar:
            tar.add(stray, arcname="something-else/stray.txt")
        proc = self.run_script()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("no Makefile at", proc.stdout)
        self.assertIn("something-else", proc.stdout,
                      "the refusal must show what it DID unpack")

    def test_a_volume_under_any_name_is_found(self):
        """macOS appends a suffix when a stale PAYLOAD volume is still mounted.

        The glob is ``$PAYLOAD_ROOT/*/`` for that reason. Pinning the volume
        name would fail with "no such file" on the second mount of the session,
        which reads as a missing payload rather than a renamed one.
        """
        vol = self.make_volume("PAYLOAD 1")
        stray = os.path.join(vol, "stray.txt")
        with open(stray, "w", encoding="utf-8") as handle:
            handle.write("x\n")
        with tarfile.open(os.path.join(vol, PAYLOAD_TAR_NAME), "w") as tar:
            tar.add(stray, arcname="something-else/stray.txt")
        proc = self.run_script()
        # It got past discovery -- the verdict is the Makefile refusal, not the
        # payload refusal.
        self.assertIn("no Makefile at", proc.stdout)
        self.assertNotIn("FAIL: no %s" % PAYLOAD_TAR_NAME, proc.stdout)


@unittest.skipUnless(SH, "no sh on PATH")
class MakeStatusTests(unittest.TestCase):
    """The script must report ``make``'s status, not ``tee``'s.

    ``make | tee -a "$LOG"`` followed by ``MAKE_STATUS=$?`` reads the status of
    the LAST command in the pipeline. ``tee`` succeeds whenever it can write the
    log, so a failing compile produced ``make exit status: 0`` and ``exit 0``.
    POSIX sh has no ``pipefail``; the status has to be carried out of the
    subshell by hand, and only running the script can prove it is.

    Every test here substitutes a ``make`` shim on PATH whose exit code it
    chooses, so the two directions -- failure reported as failure, success still
    reported as success -- are measured against known-different inputs rather
    than against whatever the host toolchain happens to do.
    """

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="nv-payload-root-")
        self.work = os.path.join(tempfile.mkdtemp(prefix="nv-work-"), "w")
        self.shim = tempfile.mkdtemp(prefix="nv-make-shim-")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.addCleanup(shutil.rmtree, os.path.dirname(self.work),
                        ignore_errors=True)
        self.addCleanup(shutil.rmtree, self.shim, ignore_errors=True)

    def install_make(self, exit_code, stdout="simulated compile"):
        """Put a ``make`` on PATH that exits with the code this test wants."""
        path = os.path.join(self.shim, "make")
        body = ("#!/bin/sh\n"
                "echo '%s'\n"
                "exit %d\n" % (stdout, exit_code))
        with open(path, "w", newline="\n") as handle:
            handle.write(body)
        os.chmod(path, 0o755)
        return path

    def make_payload(self):
        """A minimal tar the script will accept: phase0-kext/Makefile exists.

        Nothing past the Makefile check is inspected before ``make`` runs, so a
        one-file tree is enough to reach the code under test and keeps the
        fixture from depending on the real kext.
        """
        vol = os.path.join(self.root, "PAYLOAD")
        os.makedirs(vol, exist_ok=True)
        stage = os.path.join(self.root, "stage", "phase0-kext")
        os.makedirs(stage, exist_ok=True)
        with open(os.path.join(stage, "Makefile"), "w", newline="\n") as handle:
            handle.write("all:\n\t@true\n")
        tar_path = os.path.join(vol, PAYLOAD_TAR_NAME)
        with tarfile.open(tar_path, "w") as tar:
            tar.add(stage, arcname="phase0-kext")
        return tar_path

    def run_script(self):
        env = dict(os.environ)
        env["NV_PAYLOAD_ROOT"] = posix_path(self.root)
        env["NV_WORK_DIR"] = posix_path(self.work)
        env["PATH"] = self.shim + os.pathsep + env.get("PATH", "")
        return subprocess.run([SH, SCRIPT], capture_output=True, text=True,
                              env=env, timeout=120)

    def test_the_shim_is_the_make_that_runs(self):
        """Without this, every test below could be measuring 127.

        A PATH shim that the shell never finds exits 127, which is non-zero and
        would satisfy the failure assertions for entirely the wrong reason.
        """
        self.install_make(3, stdout="SHIM-MARKER-7f21")
        self.make_payload()
        proc = self.run_script()
        self.assertIn("SHIM-MARKER-7f21", proc.stdout,
                      "the PATH shim did not run; the exit codes below would"
                      " describe a missing make, not the pipeline\n"
                      + proc.stdout + proc.stderr)
        # NOT a bare "command not found" check: on Windows the script's own
        # toolchain probes (sw_vers, xcode-select, clang, xcrun) all legitimately
        # print that, so the broad form fails for a reason that has nothing to do
        # with make. Only make's absence would invalidate the codes below.
        self.assertNotIn("make: command not found", proc.stdout + proc.stderr)
        self.assertNotIn("line 133: make", proc.stdout + proc.stderr)

    def test_a_failing_make_is_reported_as_failing(self):
        """The bug: tee's success masked make's failure. Exit code and log."""
        self.install_make(2)
        self.make_payload()
        proc = self.run_script()
        self.assertIn("make exit status: 2", proc.stdout,
                      "the log reported tee's status, not make's\n"
                      + proc.stdout + proc.stderr)
        self.assertEqual(proc.returncode, 2,
                         "the script exited with tee's status\n"
                         + proc.stdout + proc.stderr)

    def test_a_different_failure_code_is_carried_verbatim(self):
        """Two distinct non-zero codes separate 'carried' from 'clamped to 1'.

        A fix that mapped every failure to 1 would pass the test above.
        """
        self.install_make(17)
        self.make_payload()
        proc = self.run_script()
        self.assertIn("make exit status: 17", proc.stdout,
                      proc.stdout + proc.stderr)
        self.assertEqual(proc.returncode, 17, proc.stdout + proc.stderr)

    def test_a_succeeding_make_is_still_reported_as_succeeding(self):
        """The other direction: the fix must not turn success into failure."""
        self.install_make(0)
        self.make_payload()
        proc = self.run_script()
        self.assertIn("make exit status: 0", proc.stdout,
                      proc.stdout + proc.stderr)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_the_build_output_still_reaches_the_console(self):
        """Carrying the status out of the subshell must not cost the tee.

        The operator reads the outcome off a screen; a fix that captured the
        status by dropping the pipeline would be silently worse than the bug.
        """
        self.install_make(0, stdout="COMPILE-LINE-c93a")
        self.make_payload()
        proc = self.run_script()
        self.assertIn("COMPILE-LINE-c93a", proc.stdout,
                      proc.stdout + proc.stderr)

    def test_the_status_file_is_not_left_behind(self):
        """The scratch file lives in the work dir the operator keeps."""
        self.install_make(0)
        self.make_payload()
        self.run_script()
        leftovers = [n for n in os.listdir(self.work) if "status" in n]
        self.assertEqual(leftovers, [],
                         "scratch status file survived: %s" % leftovers)


@unittest.skipUnless(SH, "no sh on PATH")
class ArgumentParsingTests(unittest.TestCase):
    """The script had NO argument parser at all until 2026-08-14.

    It read ``NV_PAYLOAD_ROOT``/``NV_WORK_DIR`` and nothing else, so
    ``--payload-root /tmp/x`` was consumed by nobody: ``sh`` put it in ``$1``, the
    script never looked at ``$1``, and the run proceeded against the default root
    while printing a refusal that named the very directory the caller had just
    overridden. That is this project's signature defect shape -- an option that
    appears honoured, reports plausibly, and describes a different run than the
    one you asked for.

    Why it needs execution tests rather than a shape test
    -----------------------------------------------------
    A ``case`` statement that *mentions* ``--payload-root`` proves nothing: the
    old script's own text mentioned it too, in a comment. What separates parsed
    from ignored is whether the value CHANGES WHERE THE SCRIPT LOOKS, so every
    test below points the environment variable at one directory and the flag at a
    different one, and reads which of the two the script actually searched.

    Precedence under test is CLI > environment > default, in that order. The
    environment half is what ``RefusalPathTests`` and ``MakeStatusTests`` above
    drive the script with, so a "fix" that made the flag win by breaking the
    variable would redden this whole module rather than pass quietly.
    """

    def setUp(self):
        # Two roots, deliberately: one carries a payload, one is empty. Which
        # directory the refusal names is the measurement.
        self.with_payload = tempfile.mkdtemp(prefix="nv-arg-full-")
        self.empty = tempfile.mkdtemp(prefix="nv-arg-empty-")
        self.work = os.path.join(tempfile.mkdtemp(prefix="nv-arg-work-"), "w")
        self.addCleanup(shutil.rmtree, self.with_payload, ignore_errors=True)
        self.addCleanup(shutil.rmtree, self.empty, ignore_errors=True)
        self.addCleanup(shutil.rmtree, os.path.dirname(self.work),
                        ignore_errors=True)
        self.plant_payload()

    def plant_payload(self):
        """A tar the script accepts as far as the Makefile check.

        Same one-file tree ``MakeStatusTests`` uses. Reaching ``make`` is not the
        point here -- reaching *past discovery* is, because "found the payload"
        versus "did not" is what distinguishes the two roots.
        """
        vol = os.path.join(self.with_payload, "PAYLOAD")
        os.makedirs(vol, exist_ok=True)
        stage = os.path.join(self.with_payload, "stage", "phase0-kext")
        os.makedirs(stage, exist_ok=True)
        with open(os.path.join(stage, "Makefile"), "w", newline="\n") as handle:
            handle.write("all:\n\t@true\n")
        with tarfile.open(os.path.join(vol, PAYLOAD_TAR_NAME), "w") as tar:
            tar.add(stage, arcname="phase0-kext")

    def run_script(self, args, env_root=None, env_work=None):
        env = dict(os.environ)
        # Pop rather than leave inherited: a stray NV_* in the developer's shell
        # would otherwise decide these tests' outcome.
        env.pop("NV_PAYLOAD_ROOT", None)
        env.pop("NV_WORK_DIR", None)
        if env_root is not None:
            env["NV_PAYLOAD_ROOT"] = posix_path(env_root)
        if env_work is not None:
            env["NV_WORK_DIR"] = posix_path(env_work)
        return subprocess.run([SH, SCRIPT] + args, capture_output=True,
                              text=True, env=env, timeout=120)

    # -- the flag is honoured at all ---------------------------------------

    def test_payload_root_flag_changes_where_the_script_looks(self):
        """The load-bearing test. Env points at the empty root, flag at the full
        one; if the flag is ignored the script refuses naming the empty root.
        """
        proc = self.run_script(
            ["--payload-root", posix_path(self.with_payload),
             "--work-dir", posix_path(self.work)],
            env_root=self.empty, env_work=self.work)
        self.assertNotIn("FAIL: no %s" % PAYLOAD_TAR_NAME, proc.stdout,
                         "the flag was ignored and the environment root won\n"
                         + proc.stdout + proc.stderr)
        self.assertIn("payload:", proc.stdout, proc.stdout + proc.stderr)

    def test_the_environment_variable_still_works_on_its_own(self):
        """The other direction. Every other execution class in this module drives
        the script through NV_*, so a parser that stole precedence outright would
        be a regression disguised as a fix.
        """
        proc = self.run_script([], env_root=self.with_payload,
                               env_work=self.work)
        self.assertIn("payload:", proc.stdout, proc.stdout + proc.stderr)

    def test_the_flag_beats_the_environment_variable(self):
        """Precedence, measured in the direction that can fail silently.

        Reversed from the first test: env has the payload, the flag points at the
        empty root. A parser that read the flag but let the variable overwrite it
        afterwards -- the obvious ordering mistake, assigning defaults after
        parsing -- passes the first test and fails this one.
        """
        proc = self.run_script(
            ["--payload-root", posix_path(self.empty)],
            env_root=self.with_payload, env_work=self.work)
        self.assertIn("FAIL: no %s" % PAYLOAD_TAR_NAME, proc.stdout,
                      "the environment variable overwrote the flag\n"
                      + proc.stdout + proc.stderr)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)

    def test_the_equals_form_is_accepted(self):
        """``--payload-root=DIR`` is one token, so it needs its own case arm.

        Without it the token falls through to the catch-all and the script exits
        2 complaining about an unknown argument -- refusing a spelling it
        documents.
        """
        proc = self.run_script(
            ["--payload-root=" + posix_path(self.with_payload),
             "--work-dir=" + posix_path(self.work)])
        self.assertIn("payload:", proc.stdout, proc.stdout + proc.stderr)

    def test_work_dir_flag_decides_where_the_unpack_lands(self):
        """``--work-dir`` is only observable by looking at the filesystem.

        The script prints the payload path, not the work directory, so a
        stdout-only assertion would pass with the flag ignored.
        """
        elsewhere = os.path.join(os.path.dirname(self.work), "asked-for-here")
        proc = self.run_script(
            ["--payload-root", posix_path(self.with_payload),
             "--work-dir", posix_path(elsewhere)])
        self.assertTrue(
            os.path.isdir(os.path.join(elsewhere, "phase0-kext")),
            "nothing was unpacked into the directory the flag named\n"
            + proc.stdout + proc.stderr)

    # -- refusals -----------------------------------------------------------

    def test_an_unknown_argument_is_refused_not_ignored(self):
        """The whole point. A warning would scroll off the guest console above
        several screens of build output, and the operator would read a verdict
        produced by a run that ignored them.
        """
        proc = self.run_script(
            ["--payload-root", posix_path(self.with_payload),
             "--not-a-real-flag"])
        self.assertEqual(proc.returncode, 2,
                         "an unknown argument did not refuse\n"
                         + proc.stdout + proc.stderr)
        self.assertIn("--not-a-real-flag", proc.stdout,
                      "the refusal must quote the argument it rejected\n"
                      + proc.stdout + proc.stderr)
        self.assertNotIn("payload:", proc.stdout,
                         "it refused but ran anyway\n"
                         + proc.stdout + proc.stderr)

    def test_usage_errors_exit_2_and_payload_errors_exit_1(self):
        """Two codes, because they are two different facts.

        "you asked wrongly" and "the payload is not there" need different
        remedies, and the host reads only the code when it drives this over ssh.
        One code covering both cannot be acted on remotely. Asserting them in the
        same test keeps the pair from drifting into agreement.
        """
        usage = self.run_script(["--nonsense"])
        payload = self.run_script(["--payload-root", posix_path(self.empty)])
        self.assertEqual(usage.returncode, 2, usage.stdout + usage.stderr)
        self.assertEqual(payload.returncode, 1, payload.stdout + payload.stderr)
        self.assertNotEqual(usage.returncode, payload.returncode)

    def test_a_flag_with_no_value_is_refused(self):
        """``--payload-root`` last on the line would otherwise shift past the end
        and search an empty root, reporting a missing payload instead of a
        missing argument.
        """
        for flag in ("--payload-root", "--work-dir"):
            proc = self.run_script([flag])
            self.assertEqual(proc.returncode, 2,
                             "%s with no value did not refuse\n%s"
                             % (flag, proc.stdout + proc.stderr))
            self.assertIn(flag, proc.stdout, proc.stdout + proc.stderr)

    def test_an_empty_value_is_refused(self):
        """``--payload-root=''`` would glob the filesystem root, and an empty
        work dir would unpack into the current directory -- on the guest, that is
        whatever the operator's shell happened to be sitting in.
        """
        for flag in ("--payload-root=", "--work-dir="):
            proc = self.run_script([flag])
            self.assertEqual(proc.returncode, 2,
                             "%s (empty) did not refuse\n%s"
                             % (flag, proc.stdout + proc.stderr))

    def test_help_exits_zero_and_describes_both_flags(self):
        proc = self.run_script(["--help"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("--payload-root", proc.stdout)
        self.assertIn("--work-dir", proc.stdout)
        self.assertNotIn("payload:", proc.stdout,
                         "--help ran the build\n" + proc.stdout)

    def test_every_flag_the_parser_accepts_appears_in_its_own_usage(self):
        """Derived from the script, not listed here.

        A hardcoded pair would go stale the moment a third flag is added, and the
        failure mode of a stale usage block is the operator not knowing an option
        exists -- silent, and only discoverable by reading the source they were
        given a card to avoid reading.

        The case labels are read out of the script and each one is required in
        ``--help`` output. The count is asserted non-zero first: if the label
        regex ever stops matching, an empty set would satisfy the loop below
        having checked nothing.
        """
        with open(SCRIPT, "r", encoding="utf-8") as handle:
            source = handle.read()
        flags = set()
        for label in re.findall(r"^\s+(-[^)\s]*)\)\s*$", source, re.M):
            for token in label.split("|"):
                token = token.split("=")[0]
                if token.startswith("--"):
                    flags.add(token)
        self.assertTrue(
            flags,
            "no case labels matched, so this test examined nothing -- the"
            " parser's shape changed and the regex above did not")
        proc = self.run_script(["--help"])
        for flag in sorted(flags):
            self.assertIn(flag, proc.stdout,
                          "%s is accepted by the parser but absent from --help"
                          % flag)


@unittest.skipUnless(SH, "no sh on PATH")
class RealPayloadTests(unittest.TestCase):
    """The real ``kext-src.tar``, if it has been built, must satisfy the script.

    Every test above feeds the script something deliberately wrong. That leaves
    the case that matters unmeasured: the archive ``deliver-via-cd.py`` actually
    ships. If the tar were rooted one level too deep, all six refusal tests
    would still pass and the guest would report "no Makefile".
    """

    TAR = os.path.join(_REPO, "build-payload", PAYLOAD_TAR_NAME)

    def test_the_shipped_tar_contains_the_makefile_where_the_script_looks(self):
        if not os.path.isfile(self.TAR):
            self.skipTest("build-payload/%s not built" % PAYLOAD_TAR_NAME)
        with tarfile.open(self.TAR) as tar:
            names = tar.getnames()
        self.assertIn("phase0-kext/Makefile", names,
                      "the script looks for $WORK_DIR/phase0-kext/Makefile")

    def test_the_shipped_tar_carries_every_header_the_kext_includes(self):
        """A missing header is a guest-side compile error, not a delivery error.

        Distinguishing those from the host is impossible once the ISO is burned,
        so the check belongs here.
        """
        if not os.path.isfile(self.TAR):
            self.skipTest("build-payload/%s not built" % PAYLOAD_TAR_NAME)
        with tarfile.open(self.TAR) as tar:
            names = set(tar.getnames())
        for header in ("NVDecode.hpp", "NVFalcon.hpp", "NVRiscvUcode.hpp",
                       "NVGspBoot.hpp", "NVGspStage.hpp", "NV3050Probe.hpp",
                       "NV3050Probe.cpp", "Info.plist"):
            self.assertIn("phase0-kext/%s" % header, names, header)

    def test_the_shipped_tar_excludes_the_build_directory(self):
        """A stale host build/ would be unpacked over the guest's own output."""
        if not os.path.isfile(self.TAR):
            self.skipTest("build-payload/%s not built" % PAYLOAD_TAR_NAME)
        with tarfile.open(self.TAR) as tar:
            names = tar.getnames()
        self.assertEqual(
            [n for n in names if n.startswith("phase0-kext/build")], [],
            "the payload carries a build/ directory")


REBUILD = ("rebuild it:\n"
           "    tar -cf build-payload/kext-src.tar --exclude=build phase0-kext\n"
           "    cp tools/guest-build-kext.sh build-payload/\n"
           "    python tools/deliver-via-cd.py --add build-payload/kext-src.tar"
           " --add build-payload/guest-build-kext.sh"
           " --out build-payload/step7-payload.iso --vol-ident PAYLOAD --no-vmx")


class PayloadFreshnessTests(unittest.TestCase):
    """The payload must still be the source tree it claims to be.

    Why this is a gate and not a convention
    ---------------------------------------
    ``build-payload/`` is a *build artifact checked in beside its sources*, which
    is the single most reliable way to ship the wrong thing. The delivery path is
    one-shot and one-directional: the operator powers the guest down, repoints
    ``sata0:2``, boots, runs the script, and reads the result off the screen
    (``OPERATOR-ACTION-step7-build.md``). If the tar predates the last edit to
    ``phase0-kext/``, that entire round trip compiles code nobody is working on,
    and the compile error -- or the clean build -- describes a version of the
    driver that no longer exists. Nothing about the output would look wrong.

    Worse, it fails in the flattering direction. Every other test in this module
    passes against a stale tar: the Makefile is still at the right depth, every
    header basename is still present, no ``build/`` rides along. Those checks
    were written against the tar's SHAPE, and shape is exactly what an outdated
    copy preserves.

    So this compares CONTENT, byte for byte, and it derives the file list from
    the tree rather than listing it. A hardcoded list would go stale the moment a
    sixth header is added -- which is the same defect one level up.
    """

    TAR = os.path.join(_REPO, "build-payload", PAYLOAD_TAR_NAME)
    KEXT = os.path.join(_REPO, "phase0-kext")

    def setUp(self):
        if not os.path.isfile(self.TAR):
            self.skipTest("build-payload/%s not built" % PAYLOAD_TAR_NAME)

    def tree_files(self):
        """Every file under phase0-kext/ except build/, as tar member names.

        Walked, not listed. ``build/`` is pruned in place rather than filtered
        afterwards so a large host build tree is never read.
        """
        found = {}
        for root, dirs, files in os.walk(self.KEXT):
            dirs[:] = [d for d in dirs if d != "build"]
            for name in files:
                full = os.path.join(root, name)
                rel = os.path.relpath(full, _REPO).replace(os.sep, "/")
                found[rel] = full
        return found

    def test_every_source_file_is_in_the_payload(self):
        """A file added to phase0-kext/ after the tar was built is missing.

        This is the likely direction of the failure: someone adds a header, the
        kext includes it, and the guest build dies on a missing include for a
        file that exists on the host.
        """
        with tarfile.open(self.TAR) as tar:
            shipped = set(tar.getnames())
        missing = sorted(set(self.tree_files()) - shipped)
        self.assertEqual(missing, [],
                         "phase0-kext file(s) absent from the payload: %s\n%s"
                         % (", ".join(missing), REBUILD))

    def test_the_payload_carries_nothing_the_tree_does_not(self):
        """The other direction: a file deleted from the tree but still shipped.

        A removed header that the payload still carries lets the guest build
        succeed against source the project has abandoned -- a green result that
        cannot be reproduced from the tree.
        """
        tracked = set(self.tree_files())
        with tarfile.open(self.TAR) as tar:
            # Directory members are structure, not content; only files matter.
            shipped = set(m.name for m in tar.getmembers() if m.isfile())
        extra = sorted(shipped - tracked)
        self.assertEqual(extra, [],
                         "payload carries file(s) not in the tree: %s\n%s"
                         % (", ".join(extra), REBUILD))

    def test_every_shipped_file_is_byte_identical_to_the_tree(self):
        """The load-bearing one. Names matching is not contents matching.

        The two tests above compare only the file LIST, and a stale tar has the
        identical list -- editing a header changes its bytes, not its name. So on
        their own both would stay green through exactly the failure this class
        exists to catch.

        Byte comparison rather than mtime: the tar is rebuilt by hand, so its
        member timestamps say when the archive was made, which is not evidence
        about the bytes inside it.
        """
        stale = []
        with tarfile.open(self.TAR) as tar:
            for rel, full in sorted(self.tree_files().items()):
                try:
                    member = tar.extractfile(rel)
                except KeyError:
                    continue        # reported by the missing-files test
                if member is None:
                    continue
                with open(full, "rb") as handle:
                    if handle.read() != member.read():
                        stale.append(rel)
        self.assertEqual(stale, [],
                         "payload copy differs from the working tree: %s\n%s"
                         % (", ".join(stale), REBUILD))

    def test_the_shipped_script_matches_tools(self):
        """build-payload/guest-build-kext.sh is a COPY and drifted once already.

        Both brace-instead-of-fi fixes were made in ``tools/`` while an older
        copy sat in ``build-payload/`` and inside the burned ISO. Every static
        check in this module reads ``tools/``, so the syntax error was reported
        fixed while the artifact the operator would actually run still had it.
        """
        copy = os.path.join(_REPO, "build-payload", "guest-build-kext.sh")
        if not os.path.isfile(copy):
            self.skipTest("build-payload/guest-build-kext.sh not staged")
        with open(copy, "rb") as handle:
            shipped = handle.read()
        with open(SCRIPT, "rb") as handle:
            current = handle.read()
        self.assertEqual(shipped, current,
                         "the staged script differs from tools/\n%s" % REBUILD)

    def test_the_iso_carries_the_same_two_files(self):
        """And the ISO is a THIRD copy, one level further from the source.

        The tar and the staged script can both be current while the burned image
        predates them -- that happened once this session, and the ISO is the only
        one of the three the guest ever reads.

        Reading ISO9660 *structure* needs pycdlib, which run-python-tests.bat
        does not require. Reading ISO9660 *content* does not. Small files are
        stored contiguously and unencoded, so each payload member appears in the
        image as a verbatim byte run and a plain search for that run is a
        stdlib-only BYTE-IDENTITY check -- strictly stronger than the size
        comparison this test used to make, which passed for any image merely
        larger than its inputs.

        Measured 2026-08-14 against the real image: kext-src.tar at offset 71680,
        guest-build-kext.sh at 63488. Run with a negative control in the same
        window -- flipping one byte of the script makes the search fail -- so a
        hit is evidence rather than a property of every large file.

        The size assertion is kept below. It is the weaker claim, but it survives
        a future builder that compresses content and defeats the byte search; if
        that happens the assertion above is the one that fails, loudly, rather
        than this gate quietly reverting to size-only.
        """
        iso = os.path.join(_REPO, "build-payload", "step7-payload.iso")
        if not os.path.isfile(iso):
            self.skipTest("build-payload/step7-payload.iso not built")
        with open(iso, "rb") as handle:
            image = handle.read()
        payload_bytes = 0
        checked = 0
        for name in (PAYLOAD_TAR_NAME, "guest-build-kext.sh"):
            member = os.path.join(_REPO, "build-payload", name)
            if not os.path.isfile(member):
                continue
            with open(member, "rb") as handle:
                body = handle.read()
            payload_bytes += len(body)
            checked += 1
            self.assertIn(body, image,
                          "%s is not carried by the ISO byte-for-byte -- the"
                          " image was burned from an older copy\n%s"
                          % (name, REBUILD))
        # Without this, a build-payload/ missing both members leaves the loop
        # body unexecuted and the size assertion comparing against zero, so the
        # whole test passes having examined nothing.
        self.assertEqual(checked, 2,
                         "expected both payload members to be staged; only %d"
                         " was, so the byte comparison above examined less than"
                         " it claims\n%s" % (checked, REBUILD))
        # ISO9660 rounds every file up to a 2048-byte sector and adds its own
        # descriptors and two directory trees, so the image is always larger --
        # but not unboundedly so. A payload that grew past the image is proof the
        # image is older than its inputs.
        self.assertGreater(len(image), payload_bytes,
                           "the ISO is smaller than the files it should carry"
                           " -- it predates them\n%s" % REBUILD)


if __name__ == "__main__":
    unittest.main(verbosity=2)
