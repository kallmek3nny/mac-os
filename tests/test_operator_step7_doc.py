#!/usr/bin/env python3
"""
test_operator_step7_doc.py -- is OPERATOR-ACTION-step7-build.md still TRUE?

Why this module exists
----------------------
That card is the entire remaining path to STEP 7: a human reads it once, types a
handful of commands, and gets one attempt at the first Apple-clang build of the
kext. Every other artifact in this project is checked by something. The card is
not -- it is prose, and prose carrying derived values goes stale silently. That
is not hypothetical here: on 2026-08-14 the card still stated the payload script
was 6718 bytes and the ISO's md5 was ``5828e278...`` more than an hour after both
changed, and the staleness was caught by a hand grep rather than by any gate.

What this module checks, and why each one can actually fire:

* The **commands** the card tells the operator to type resolve to things that
  exist -- the payload script's real filename, the real vmrun path, the real VMX.
* The **sata0:2 claim** matches the live VMX. The card asserts the running VM is
  pointed at ``build-payload/step7-payload.iso``; if a peer repointed that device
  the card would send the operator to build whatever is on the other image.
* The **derived numbers** (script byte size, member sizes, the ISO md5) match the
  artifacts on disk. This is the check that would have caught the real stale
  values above.
* The **per-module test counts** the card quotes match what those modules
  actually contain.
* The card does not **contradict itself** about the power cycle. It was rewritten
  three times in one afternoon and briefly said both "do not power-cycle" and
  "the power cycle is mandatory".

What this module deliberately does NOT do
-----------------------------------------
It never powers the VM, never edits the VMX, and never rebuilds the payload. The
VM is running and ``tools/build-step7-payload.py`` moves a new image over the
file the running VM has open. Reading is the whole budget.

It also does not check a project-wide test total. Several terminals add tests
concurrently, so any such number is stale within minutes and the gate would fail
on correct prose. Only counts the card itself is responsible for are pinned.

Run:
    python -m unittest discover -s tests -p test_operator_step7_doc.py -v
"""

import hashlib
import os
import re
import unittest

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_TESTS_DIR)

DOC = os.path.join(_REPO, "OPERATOR-ACTION-step7-build.md")
PAYLOAD_DIR = os.path.join(_REPO, "build-payload")
ISO = os.path.join(PAYLOAD_DIR, "step7-payload.iso")
TAR = os.path.join(PAYLOAD_DIR, "kext-src.tar")
STAGED_SH = os.path.join(PAYLOAD_DIR, "guest-build-kext.sh")
TOOL_SH = os.path.join(_REPO, "tools", "guest-build-kext.sh")
VMX = r"C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx"


def read_doc():
    with open(DOC, "r", encoding="utf-8") as handle:
        return handle.read()


def fenced_blocks(text):
    """Every ``` fenced block in the doc, as a list of raw strings.

    The commands the operator types are the highest-stakes text in the file, and
    they are all fenced. Pulling them out separately keeps the command checks
    from accidentally being satisfied by discussion prose elsewhere.
    """
    return re.findall(r"```[a-z]*\n(.*?)```", text, re.DOTALL)


class DocExistsTests(unittest.TestCase):
    """Without this the whole module skips into vacuous green."""

    def test_the_card_exists(self):
        self.assertTrue(os.path.isfile(DOC), DOC)

    def test_the_card_is_not_empty(self):
        self.assertGreater(len(read_doc()), 2000,
                           "the card is the entire STEP 7 path; a stub means"
                           " something truncated it")


class CommandTests(unittest.TestCase):
    """The commands the operator types must name things that exist."""

    def setUp(self):
        self.text = read_doc()
        self.blocks = fenced_blocks(self.text)

    def test_there_are_fenced_command_blocks(self):
        """Non-vacuity guard for every test below that greps the blocks."""
        self.assertGreaterEqual(len(self.blocks), 3,
                                "found %d fenced blocks; the command checks"
                                " below would pass by matching nothing"
                                % len(self.blocks))

    def test_the_guest_command_names_the_real_script(self):
        """``sh /Volumes/PAYLOAD/guest-build-kext.sh`` -- the basename must be
        the file actually burned into the ISO, or the operator types a command
        that finds nothing.
        """
        joined = "\n".join(self.blocks)
        basename = os.path.basename(TOOL_SH)
        self.assertIn(basename, joined,
                      "no fenced command names %s" % basename)
        self.assertIn("/Volumes/PAYLOAD/" + basename, joined,
                      "the guest command does not use the mount path the"
                      " script is actually delivered to")

    def test_the_guest_command_uses_sh_not_bash(self):
        """The script is ``#!/bin/sh`` and uses no bashisms deliberately.

        A card telling the operator ``bash ...`` would still work today, but the
        two would then disagree, and the next person to fix one would not know
        to fix the other.
        """
        joined = "\n".join(self.blocks)
        line = [l for l in joined.splitlines()
                if "guest-build-kext.sh" in l and "/Volumes/" in l]
        self.assertTrue(line, "no guest invocation line found")
        for l in line:
            self.assertTrue(l.strip().startswith("sh "),
                            "guest invocation is not `sh ...`: %r" % l)

    def test_the_vmrun_path_exists_on_this_machine(self):
        """A card naming a vmrun that is not installed is a dead end.

        Skipped rather than failed when VMware is absent: the card is still
        correct on the operator's machine, and this module also runs on
        checkouts that have no VM at all.
        """
        m = re.findall(r'"([A-Z]:\\[^"]*vmrun\.exe)"', self.text)
        self.assertTrue(m, "the card names no vmrun.exe path")
        self.assertEqual(len(set(m)), 1,
                         "the card names more than one vmrun path: %s"
                         % sorted(set(m)))
        if not os.path.isdir(os.path.dirname(m[0])):
            self.skipTest("VMware not installed at %s" % os.path.dirname(m[0]))
        self.assertTrue(os.path.isfile(m[0]), m[0])

    def test_the_vmx_path_the_card_names_exists(self):
        m = re.findall(r'"([A-Z]:\\[^"]*\.vmx)"', self.text)
        self.assertTrue(m, "the card names no .vmx path")
        self.assertEqual(len(set(m)), 1,
                         "the card names more than one VMX: %s"
                         % sorted(set(m)))
        if not os.path.isdir(os.path.dirname(m[0])):
            self.skipTest("no VM directory at %s" % os.path.dirname(m[0]))
        self.assertTrue(os.path.isfile(m[0]), m[0])


class VmxAgreementTests(unittest.TestCase):
    """The card's claim about the live VMX must match the live VMX.

    READ ONLY. The VM is running; a write here would be silently discarded at
    best and would corrupt the operator's one path at worst.
    """

    def setUp(self):
        if not os.path.isfile(VMX):
            self.skipTest("no VMX at %s" % VMX)
        with open(VMX, "r", encoding="windows-1252") as handle:
            self.vmx = handle.read()
        self.text = read_doc()

    def vmx_value(self, key):
        """Last uncommented assignment wins, which is VMware's own precedence."""
        found = None
        for line in self.vmx.splitlines():
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            m = re.match(re.escape(key) + r'\s*=\s*"(.*)"\s*$', stripped)
            if m:
                found = m.group(1)
        return found

    def test_the_card_names_the_device_the_vmx_actually_uses(self):
        """The card tells the operator to repoint ``sata0:2``.

        If the payload ISO were attached to a different device, following the
        card would repoint an unrelated drive and leave the payload unmounted.
        """
        device = None
        for key in ("sata0:2", "sata0:1", "sata0:0", "ide1:0", "sata1:0"):
            value = self.vmx_value(key + ".fileName")
            if value and value.lower().endswith("step7-payload.iso"):
                device = key
                break
        self.assertIsNotNone(
            device,
            "no VMX device points at step7-payload.iso; the card's repoint"
            " instruction cannot be verified and may be aimed at the wrong"
            " device")
        self.assertIn(device, self.text,
                      "the VMX carries the payload on %s but the card does not"
                      " mention that device" % device)

    def test_the_vmx_points_at_the_payload_iso_in_this_repo(self):
        """Not merely 'an ISO named step7-payload.iso' -- THIS repo's copy.

        A stale absolute path left over from a moved checkout would satisfy a
        basename-only check and boot the guest onto a different image.
        """
        value = self.vmx_value("sata0:2.fileName")
        self.assertIsNotNone(value, "sata0:2.fileName is not set in the VMX")
        actual = os.path.normcase(os.path.abspath(value.replace("\\\\", "\\")))
        expected = os.path.normcase(os.path.abspath(ISO))
        self.assertEqual(actual, expected,
                         "the VMX CD points somewhere other than this repo's"
                         " payload:\n  vmx : %s\n  repo: %s" % (actual, expected))

    def test_the_card_quotes_the_vmx_path_it_tells_you_to_edit(self):
        self.assertIn("sata0:2.fileName", self.text,
                      "the card never names the key the operator must change")


class DerivedValueTests(unittest.TestCase):
    """Numbers in prose that describe artifacts on disk.

    This is the class that would have caught the real 2026-08-14 staleness: the
    card said 6718 bytes and md5 5828e278... for more than an hour after the
    script became 7596 bytes and the image was rebuilt.

    The card deliberately QUOTES superseded values inside correction blocks, so
    none of these tests may demand that a value appears nowhere else. They check
    that the CURRENT value is present, which a stale-only card fails.
    """

    def setUp(self):
        self.text = read_doc()

    def test_the_script_byte_size_the_card_quotes_is_current(self):
        if not os.path.isfile(STAGED_SH):
            self.skipTest("payload not built")
        size = os.path.getsize(STAGED_SH)
        self.assertIn(str(size), self.text,
                      "the card does not mention the payload script's real"
                      " size (%d bytes); it is quoting a stale number."
                      " Rebuild note: python tools/build-step7-payload.py"
                      % size)

    def test_the_tar_byte_size_the_card_quotes_is_current(self):
        if not os.path.isfile(TAR):
            self.skipTest("payload not built")
        size = os.path.getsize(TAR)
        self.assertIn(str(size), self.text,
                      "the card does not mention kext-src.tar's real size"
                      " (%d bytes)" % size)

    def test_every_md5_the_card_quotes_can_be_accounted_for(self):
        """The live ISO's md5 must be among the md5s the card prints.

        Not 'the only one' -- the card quotes the superseded md5 on purpose so a
        reader can tell which image an older note described. What is not
        allowed is the current image being absent entirely, which is exactly
        the stale state this gate exists to catch.
        """
        if not os.path.isfile(ISO):
            self.skipTest("payload not built")
        with open(ISO, "rb") as handle:
            digest = hashlib.md5(handle.read()).hexdigest()
        quoted = set(re.findall(r"\b[0-9a-f]{32}\b", self.text))
        self.assertTrue(quoted, "the card quotes no md5 at all")
        self.assertIn(digest, quoted,
                      "the live ISO's md5 (%s) appears nowhere in the card."
                      " md5s the card does quote: %s"
                      % (digest, sorted(quoted)))

    def test_a_superseded_md5_is_marked_as_superseded(self):
        """Quoting an old hash is fine; quoting it as current is not.

        Without this, the previous test is satisfied by a card that prints both
        hashes flatly and leaves the reader to guess which image is live.
        """
        if not os.path.isfile(ISO):
            self.skipTest("payload not built")
        with open(ISO, "rb") as handle:
            digest = hashlib.md5(handle.read()).hexdigest()
        quoted = set(re.findall(r"\b[0-9a-f]{32}\b", self.text))
        stale = quoted - {digest}
        if not stale:
            self.skipTest("the card quotes only the current md5")
        lowered = self.text.lower()
        self.assertTrue(
            "superseded" in lowered or "no longer" in lowered,
            "the card quotes %d md5(s) that are not the live image (%s) but"
            " never marks any claim superseded" % (len(stale), sorted(stale)))

    def test_the_iso_the_card_references_is_the_one_the_builder_produces(self):
        """Path-level agreement between the card and tools/build-step7-payload.py."""
        self.assertIn("build-payload/step7-payload.iso", self.text.replace("\\", "/"),
                      "the card never names the payload ISO by its repo path")
        self.assertTrue(os.path.isfile(os.path.join(_REPO, "tools",
                                                    "build-step7-payload.py")),
                        "the rebuild tool the card points at is missing")


class QuotedTestCountTests(unittest.TestCase):
    """The per-module counts the card quotes must be real.

    Only modules the card is responsible for. A project-wide total is
    deliberately not checked -- peers add tests concurrently and it would fail
    on correct prose within minutes.
    """

    MODULES = ("test_build_step7_payload.py", "test_guest_build_kext.py")

    def setUp(self):
        self.text = read_doc()

    def count_tests(self, module):
        path = os.path.join(_TESTS_DIR, module)
        if not os.path.isfile(path):
            return None
        with open(path, "r", encoding="utf-8") as handle:
            body = handle.read()
        return len(re.findall(r"^    def test_", body, re.MULTILINE))

    def test_the_modules_the_card_names_exist(self):
        for module in self.MODULES:
            self.assertIn(module, self.text,
                          "the card no longer names %s; this class is then"
                          " checking counts nobody quotes" % module)
            self.assertTrue(os.path.isfile(os.path.join(_TESTS_DIR, module)),
                            module)

    def test_each_quoted_count_matches_the_module(self):
        for module in self.MODULES:
            actual = self.count_tests(module)
            self.assertIsNotNone(actual, module)
            # The card writes counts as "(34 tests)" or "(**28** tests, was 22".
            pattern = re.escape(module) + r"`?\s*\((?:\*\*)?(\d+)(?:\*\*)?\s*tests"
            m = re.search(pattern, self.text)
            self.assertIsNotNone(
                m, "the card does not state a test count for %s in the form"
                   " `%s` (N tests ...)" % (module, module))
            self.assertEqual(
                int(m.group(1)), actual,
                "the card says %s has %s tests; it has %d"
                % (module, m.group(1), actual))


class SelfConsistencyTests(unittest.TestCase):
    """The card was rewritten three times in one afternoon.

    It briefly told the operator both that the boot was done and must not be
    disturbed, and that a power cycle was mandatory. Two contradictory
    instructions in one document is worse than either instruction alone.
    """

    def setUp(self):
        self.text = read_doc()
        self.lower = self.text.lower()

    # Phrasings that tell the operator NOT to power-cycle. Derived from what
    # this card has actually said rather than from one canonical spelling: the
    # first version of this test grepped only for "do not power-cycle", a
    # string that appears nowhere in the file, so it passed by matching
    # nothing. The card's real idiom is "told you not to power-cycle".
    #
    # "without a power cycle" is deliberately absent: the card uses it to say
    # the OPPOSITE ("running the console command without a power cycle would
    # build the old script"), so matching it would redden correct prose.
    FORBID_RE = re.compile(
        r"(?:do|does|did)\s+not\s+power[- ]cycle"
        r"|don'?t\s+power[- ]cycle"
        r"|not\s+to\s+power[- ]cycle"
        r"|no\s+need\s+to\s+power[- ]cycle"
        r"|leave\s+(?:it|the\s+guest|the\s+vm)\s+running")

    # Words a heading uses to tell a skimming operator the step is settled.
    # "nothing to do" alone -- the original check -- is the one phrasing this
    # card has never used; every real instance has been "DONE <date>".
    HEADING_DONE_RE = re.compile(
        r"nothing to do|already done|already off|no action|skip to step"
        r"|\bdone\b")

    # Steps that a required power cycle puts back on the operator's plate.
    # Step 2 (repoint the CD) survives a reboot and is legitimately headed
    # DONE, so it stays exempt.
    PENDING_STEPS = ("1", "3")

    def test_the_forbid_and_done_patterns_are_not_vacuous(self):
        """Both patterns above are greps, and a grep that matches nothing is a
        test that cannot fail -- which is exactly how the two tests below
        passed against a card that contradicted itself. Pin each pattern to the
        phrasings it exists to catch, and to the opposite-sense prose it must
        not catch, so a future narrowing reddens here instead of going silent.
        """
        for phrase in ("do not power-cycle the guest",
                       "do NOT power cycle the VM",
                       "don't power-cycle it",
                       "told you not to power-cycle",
                       "no need to power-cycle the guest",
                       "leave it running"):
            self.assertTrue(self.FORBID_RE.search(phrase.lower()),
                            "FORBID_RE misses %r" % phrase)
        for phrase in ("without a power cycle it builds the old script",
                       "the power cycle is now mandatory",
                       "a running VM does not re-read its CD"):
            self.assertIsNone(self.FORBID_RE.search(phrase.lower()),
                              "FORBID_RE falsely matches %r" % phrase)
        for phrase in ("1. Shut the guest down - nothing to do",
                       "1. Shut the guest down - DONE 2026-08-13 19:00",
                       "1. Shut the guest down - ALREADY DONE, skip to step 3",
                       "3. Boot - already off, no action needed"):
            self.assertTrue(self.HEADING_DONE_RE.search(phrase.lower()),
                            "HEADING_DONE_RE misses %r" % phrase)
        for phrase in ("1. Shut the guest down - REQUIRED AGAIN",
                       "3. Boot, THEN run one command at the guest console"):
            self.assertIsNone(self.HEADING_DONE_RE.search(phrase.lower()),
                              "HEADING_DONE_RE falsely matches %r" % phrase)

    def test_it_does_not_both_forbid_and_require_the_power_cycle(self):
        requires = (("mandatory" in self.lower
                     or "required again" in self.lower
                     or "not optional" in self.lower)
                    and "power cycle" in self.lower)
        hits = list(self.FORBID_RE.finditer(self.lower))
        if not hits:
            return
        # A surviving forbid is only acceptable when the same sentence region
        # marks it as the corrected-away advice.
        for m in hits:
            window = self.lower[max(0, m.start() - 400):m.start() + 400]
            self.assertTrue(
                "corrected" in window or "superseded" in window
                or "earlier revision" in window or "was written" in window
                or "no longer" in window,
                "the card tells the operator not to power-cycle (%r) without"
                " marking that advice as superseded, while elsewhere calling"
                " the power cycle %s"
                % (self.text[max(0, m.start() - 40):m.end() + 40],
                   "mandatory" if requires else "required"))

    def test_the_step_headings_do_not_claim_done_work_is_pending(self):
        """Heading text is what a skimming operator acts on.

        A heading reading 'DONE, nothing to do' above a step that must be redone
        is the single most dangerous line in the file, because it is the one a
        hurried reader trusts without reading the body.
        """
        headings = re.findall(r"^###\s+(.*)$", self.text, re.MULTILINE)
        self.assertTrue(headings, "the card has no ### step headings")
        power_cycle_required = ("mandatory" in self.lower
                                or "required again" in self.lower
                                or "not optional" in self.lower)
        if not power_cycle_required:
            self.skipTest("the card does not currently require a power cycle")
        checked = 0
        for heading in headings:
            number = re.match(r"\s*(\d+)\.", heading)
            step = number.group(1) if number else ""
            if step not in self.PENDING_STEPS:
                continue
            checked += 1
            hit = self.HEADING_DONE_RE.search(heading.lower())
            self.assertIsNone(
                hit,
                "step %s is headed %r -- %r reads as settled work while the"
                " card elsewhere says the power cycle is required"
                % (step, heading, hit.group(0) if hit else ""))
        # Without this the loop is vacuous the moment a heading is renumbered
        # or the '### N.' shape changes: every heading would be skipped and the
        # test would pass having examined nothing.
        self.assertEqual(
            checked, len(self.PENDING_STEPS),
            "expected headings for steps %s; matched %d, so the loop above"
            " examined less than it claims"
            % (", ".join(self.PENDING_STEPS), checked))

    def test_it_tells_the_operator_the_guest_may_hold_a_stale_disc(self):
        """The single fact that makes the difference between a real and a fake
        STEP 7 result: a running VM does not re-read its CD.
        """
        self.assertIn("stale", self.lower,
                      "the card never warns that the mounted disc can be stale")
        self.assertTrue(
            "does not re-read" in self.lower or "re-read its cd" in self.lower
            or "power cycle" in self.lower,
            "the card mentions staleness without saying what fixes it")


class PayloadAgreementTests(unittest.TestCase):
    """The card describes the script's behaviour; the script must agree."""

    def setUp(self):
        self.text = read_doc()
        with open(TOOL_SH, "r", encoding="utf-8") as handle:
            self.script = handle.read()

    def test_the_work_dir_the_card_names_is_the_scripts_default(self):
        """The card says the script unpacks to ``~/nv3050-step7``.

        If that drifted, the operator would look for the build log in a
        directory that does not exist, on the one run they get.
        """
        m = re.search(r'NV_WORK_DIR:-\$HOME/([A-Za-z0-9._-]+)', self.script)
        self.assertIsNotNone(m, "cannot read the script's default work dir")
        self.assertIn(m.group(1), self.text,
                      "the card does not name the script's actual work"
                      " directory (~/%s)" % m.group(1))

    def test_the_log_name_the_card_names_matches_the_script(self):
        m = re.search(r'LOG_NAME="([^"]+)"', self.script)
        self.assertIsNotNone(m, "cannot read LOG_NAME from the script")
        self.assertIn(m.group(1), self.text,
                      "the card does not name the log the script writes (%s)"
                      % m.group(1))

    def test_the_glob_the_card_describes_matches_the_script(self):
        """The card explains the script globs for the tar rather than assuming
        the mount point. That explanation is why an operator seeing a different
        volume name does not panic.
        """
        m = re.search(r'PAYLOAD_TAR_NAME="([^"]+)"', self.script)
        self.assertIsNotNone(m, "cannot read PAYLOAD_TAR_NAME from the script")
        self.assertIn(m.group(1), self.text,
                      "the card does not name the tar the script looks for"
                      " (%s)" % m.group(1))

    def test_the_staged_script_is_the_one_the_card_describes(self):
        """tools/ and build-payload/ copies must not have diverged.

        test_guest_build_kext.py already checks this, but the card's numbers are
        measured against the STAGED copy while its description is written from
        the tools/ copy, so this module depends on them being the same file.
        """
        if not os.path.isfile(STAGED_SH):
            self.skipTest("payload not built")
        with open(STAGED_SH, "rb") as handle:
            staged = handle.read()
        with open(TOOL_SH, "rb") as handle:
            tool = handle.read()
        self.assertEqual(staged, tool,
                         "build-payload/guest-build-kext.sh has drifted from"
                         " tools/guest-build-kext.sh; rebuild the payload")


if __name__ == "__main__":
    unittest.main()
