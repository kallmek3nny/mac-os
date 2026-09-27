#!/usr/bin/env python3
"""test_igpu_dryrun_arms.py -- require the four arms of tools/_igpu-reader-dryrun.py
to reach FOUR DIFFERENT verdicts.

WHY THIS EXISTS
---------------
`tools/igpu-plane-dsi.sh` already shipped one FALSE verdict word. Its `dead()`
treated only None and 0xFFFFFFFF as unreadable, so every page reading
0x00000000 came out as the word ABSENT -- and on the real 8086:9a68 measured
2026-08-16, EVERY unused display page reads 0x00000000. Transcoders B, C and D
certainly exist on Tiger Lake, so ABSENT was simply false for them; the true
state is DECODED-BUT-UNPROGRAMMED, which one read cannot separate from absent.

The dry-run harness could not have caught that: its synthetic BAR was filled
with 0xFF, which is what the author EXPECTED hardware to do rather than what
this hardware does. A harness that encodes the author's expectation tests the
expectation. `dead()`, the verdict word, and the zero-fill arm were all added
AFTER the capture -- so as of writing, nothing had demonstrated that the four
arms reach four DIFFERENT verdicts. Four arms that print the same thing are
decoration, and this tree has been bitten repeatedly by checks that were green
because they matched nothing.

WHAT IS ASSERTED
----------------
Each arm is required to differ from the others in the specific quantity it
exists to vary:

  arm 0     Q4 must read raw=1919x1079 and +1=1920x1080
  arm 1     the SAME line must read raw=1920x1080 -- so a reader that ignored
            PIPESRC entirely and printed the panel mode both times fails here
  arm dead  CONTROL must FAIL, and the null verdict must be INCONCLUSIVE
            rather than UNPROGRAMMED-OR-ABSENT, because an unproven reader's
            nulls are not evidence
  arm zero  the bare word ABSENT must appear NOWHERE, and a zero-filled plane
            slot must NOT be counted as readable

NON-VACUITY is proved by mutation rather than by a green run: two temp copies of
the script revert `dead()` and the verdict word to their pre-2026-08-16 forms,
and each mutation is required to REDDEN a named assertion. A mutation that
matched nothing would pass silently, so each one also asserts its anchor
occurred exactly once before replacing it.

The verdict WORD is asserted, never an exit code -- the reader prints its
verdicts and exits 0 either way.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HARNESS = os.path.join(ROOT, "tools", "_igpu-reader-dryrun.py")
SCRIPT = os.path.join(ROOT, "tools", "igpu-plane-dsi.sh")

# "ABSENT" also occurs INSIDE "UNPROGRAMMED-OR-ABSENT", which is the corrected
# and correct word. \b would match there because '-' is a word boundary, so the
# bare-word test needs the compound excluded explicitly.
BARE_ABSENT = re.compile(r"(?<!OR-)\bABSENT\b")


def run_arm(arm, script=None):
    """Drive one arm and return its stdout. `script` overrides the file under test."""
    env = dict(os.environ)
    if script is not None:
        env["IGPU_DRYRUN_SCRIPT"] = script
    proc = subprocess.run([sys.executable, HARNESS, arm],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          env=env, cwd=ROOT)
    out = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        raise AssertionError("arm %s exited %d:\n%s" % (arm, proc.returncode, out))
    return out


def q4_line(out):
    """The one line arms 0 and 1 must disagree on."""
    for line in out.splitlines():
        if line.startswith("trans ") and "PIPESRC=" in line:
            return line.strip()
    raise AssertionError("no Q4 PIPESRC line in output:\n%s" % out)


class HarnessPresent(unittest.TestCase):
    def test_both_files_exist(self):
        """A missing file must name itself, not surface as a mysterious arm failure."""
        for p in (HARNESS, SCRIPT):
            self.assertTrue(os.path.exists(p), "%s is missing" % p)


class ArmsDisagree(unittest.TestCase):
    """The four arms must reach four DIFFERENT verdicts."""

    @classmethod
    def setUpClass(cls):
        cls.out = {a: run_arm(a) for a in ("0", "1", "dead", "zero")}

    def test_arm0_reports_the_minus_one_convention(self):
        line = q4_line(self.out["0"])
        self.assertIn("raw=1919x1079", line)
        self.assertIn("+1=1920x1080", line)

    def test_arm1_reports_a_different_pipesrc(self):
        """PIPESRC raw == active. A reader ignoring PIPESRC prints arm 0's line here."""
        line = q4_line(self.out["1"])
        self.assertIn("raw=1920x1080", line)
        self.assertNotIn("raw=1919x1079", line)

    def test_arm0_and_arm1_actually_differ(self):
        """The separating assertion. If these agree, Q4 measures nothing."""
        self.assertNotEqual(q4_line(self.out["0"]), q4_line(self.out["1"]))

    def test_dead_arm_fails_the_control(self):
        self.assertIn("CONTROL  FAILED", self.out["dead"])
        self.assertIn("reader unproven", self.out["dead"])

    def test_dead_arm_downgrades_nulls_to_inconclusive(self):
        """An unproven reader's nulls are not evidence of absence."""
        self.assertIn("INCONCLUSIVE", self.out["dead"])
        self.assertNotIn("UNPROGRAMMED-OR-ABSENT", self.out["dead"])

    def test_live_arms_do_not_say_inconclusive(self):
        """The other side of the same coin -- otherwise INCONCLUSIVE is constant."""
        for arm in ("0", "1", "zero"):
            self.assertNotIn("INCONCLUSIVE", self.out[arm],
                             "arm %s claimed INCONCLUSIVE with a live control" % arm)

    def test_zero_arm_never_says_bare_absent(self):
        """The exact word that was false on real silicon."""
        hit = BARE_ABSENT.search(self.out["zero"])
        self.assertIsNone(hit, "zero arm printed the bare word ABSENT: %r"
                          % (hit.group(0) if hit else None))

    def test_zero_arm_does_not_count_zero_slots_as_readable(self):
        """A zero-filled plane slot is not evidence a plane exists."""
        self.assertIn("(no readable plane slot)", self.out["zero"])
        self.assertNotIn("7 readable slots", self.out["zero"])

    def test_zero_arm_still_finds_the_programmed_pipe(self):
        """Positive control for the arm: it must not be blind to everything."""
        self.assertIn("CONTROL  transcoder A HTOTAL", self.out["zero"])
        self.assertIn("plane 1 @0x70180", self.out["zero"])

    def test_all_four_outputs_are_distinct(self):
        seen = {}
        for arm, out in self.out.items():
            self.assertNotIn(out, seen,
                             "arms %s and %s produced byte-identical output"
                             % (seen.get(out), arm))
            seen[out] = arm


class MutationRedddensTheArms(unittest.TestCase):
    """Prove the assertions above are not vacuous, by breaking the script on purpose.

    Each mutation reverts one half of the 2026-08-16 fix. The temp copy is what
    the harness reads, via IGPU_DRYRUN_SCRIPT -- mutating the live file would make
    a concurrent gate run read a half-written script and report a named FAIL with
    no defect present.
    """

    def mutate(self, anchor, replacement):
        with open(SCRIPT, "r", encoding="utf-8", newline="") as fh:
            src = fh.read()
        self.assertEqual(src.count(anchor), 1,
                         "mutation anchor %r occurs %d times, not once -- the "
                         "mutation would no-op or hit the wrong site"
                         % (anchor, src.count(anchor)))
        d = tempfile.mkdtemp(prefix="igpu-mutant-")
        self.addCleanup(shutil.rmtree, d, True)
        path = os.path.join(d, "igpu-plane-dsi.sh")
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(src.replace(anchor, replacement))
        return path

    def test_pre_fix_dead_makes_zero_slots_look_readable(self):
        """Revert dead() to None/0xFFFFFFFF only -- the shipped bug."""
        mutant = self.mutate(
            "return v is None or v == 0xFFFFFFFF or v == 0",
            "return v is None or v == 0xFFFFFFFF")
        out = run_arm("zero", script=mutant)
        # This is what the fixed script must NOT print, and does not.
        self.assertIn("7 readable slots", out,
                      "the pre-fix dead() did not change the plane verdict, so "
                      "test_zero_arm_does_not_count_zero_slots_as_readable "
                      "proves nothing")
        self.assertNotIn("(no readable plane slot)", out)

    def test_pre_fix_verdict_word_prints_bare_absent(self):
        """Revert the verdict word -- the false claim about transcoders B/C/D."""
        mutant = self.mutate(
            "verdict_null = 'UNPROGRAMMED-OR-ABSENT' if READER_OK else 'INCONCLUSIVE'",
            "verdict_null = 'ABSENT' if READER_OK else 'INCONCLUSIVE'")
        out = run_arm("zero", script=mutant)
        hit = BARE_ABSENT.search(out)
        self.assertIsNotNone(
            hit, "the pre-fix verdict word did not produce a bare ABSENT, so "
                 "test_zero_arm_never_says_bare_absent proves nothing")

    def test_pipesrc_delta_is_what_separates_arm0_from_arm1(self):
        """Kill the reader's PIPESRC read; both live arms must then stop differing.

        This mutation targets the harness rather than the script: it is the only
        way to show that test_arm0_and_arm1_actually_differ is measuring PIPESRC
        and not some incidental byte.
        """
        mutant = self.mutate(
            "    v = rd(base + 0x1C)\n",
            "    v = rd(base + 0x00)\n")
        a = q4_line(run_arm("0", script=mutant))
        b = q4_line(run_arm("1", script=mutant))
        self.assertEqual(a, b,
                         "reading HTOTAL instead of PIPESRC still separated the "
                         "arms, so the Q4 assertion is anchored on the wrong word")


if __name__ == "__main__":
    unittest.main()
