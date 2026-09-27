#!/usr/bin/env python3
r"""test_find_display_matches.py -- gate for tools/find-display-matches.py.

What the subject claims
-----------------------
It reads IOKit match dictionaries out of a macOS image and reports whether a
given PCI device id is bound by any shipping graphics personality. Its headline
output is an ABSENCE -- the Tiger Lake id 0x9A688086 appears in none of the 203
device ids the scan recovered.

Why an absence needs its own gate
---------------------------------
An absence is the one finding a scanner cannot report honestly on its own
evidence. A wrong region, a compressed plist, a mis-spelled key and a genuinely
unbound part all print the identical zero. The tool answers that with
corroboration arms, so the arms are what this module actually tests: each one
must be individually falsifiable, and a control that breaks one must leave the
others standing. An arm that cannot fail is an `ok` line, not proof.

The committed-artifact half
---------------------------
`display-match-report.txt` is 3 GiB of scan reduced to a file, and rescanning it
from a test would take minutes and needs an image that lives outside the
checkout. So the committed tests do not regenerate it. They pin the property
that actually goes wrong: a listing whose HEADER states a reach its BODY does
not show. This tree has already published one of those -- `intel-kext-inventory.txt`
said 430 and showed 16, and a later grep for an absent thing read the truncation
as a fact about macOS. Set EQUALITY between the reach line and the rows below it
is the cheapest possible prevention, and it reddens on the commit that
reintroduces a filter.
"""

import os
import re
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import importlib

dm = importlib.import_module("find-display-matches")


# --------------------------------------------------------------------------
# synthetic image construction
# --------------------------------------------------------------------------

def personality(publisher, ioclass, ids,
                class_match="0x03000000&amp;0xff000000",
                provider="IOPCIDevice",
                match_key="IOPCIPrimaryMatch"):
    """Build one personality in the key order Apple's plists actually emit.

    The subject finds IOClass and IOPCIClassMatch by looking BACKWARD from the
    match key and IOPersonalityPublisher / IOProviderClass by looking FORWARD.
    That only works because these plists are serialised with their keys in
    alphabetical order, so a fixture that scrambles the order would be testing
    a document macOS never produces.
    """
    parts = []
    if ioclass:
        parts.append("<key>IOClass</key><string>%s</string>" % ioclass)
    if class_match:
        parts.append("<key>IOPCIClassMatch</key><string>%s</string>"
                     % class_match)
    parts.append("<key>%s</key><string>%s</string>" % (match_key, " ".join(ids)))
    if publisher:
        parts.append("<key>IOPersonalityPublisher</key><string>%s</string>"
                     % publisher)
    if provider:
        parts.append("<key>IOProviderClass</key><string>%s</string>" % provider)
    return "<dict>" + "".join(parts) + "</dict>"


CFL_ID = "0x3E9B8086"
ICL_ID = "0x8A528086"
TGL_ID = "0x9A688086"


def build_image(bodies=None, filler=4096):
    """A byte image holding personalities embedded in non-XML padding."""
    if bodies is None:
        bodies = [
            personality("com.apple.driver.AppleIntelCFLGraphicsFramebuffer",
                        "AppleIntelFramebufferController", [CFL_ID]),
            personality("com.apple.driver.AppleIntelICLLPGraphicsFramebuffer",
                        "AppleIntelFramebufferController", [ICL_ID]),
        ]
    blob = b"\x00\xff\x13\x37" * (filler // 4)
    out = bytearray(blob)
    for body in bodies:
        out += body.encode("ascii")
        out += blob
    return bytes(out)


class ImageFixture(unittest.TestCase):
    """Writes a synthetic image to a temp file and scans it."""

    def scan_bytes(self, data):
        handle, path = tempfile.mkstemp(suffix=".img")
        os.close(handle)
        try:
            with open(path, "wb") as fh:
                fh.write(data)
            found, stats = dm.scan(path)
            text = "\n".join(dm.report(found, stats, path))
            return found, stats, text
        finally:
            os.unlink(path)

    def arms(self, text):
        """Return {label: ok} for the corroboration block."""
        out = {}
        for line in text.splitlines():
            m = re.match(r"\s*\[(ok|XX)\]\s+(.*)$", line)
            if m:
                out[m.group(2).strip()] = (m.group(1) == "ok")
        return out


# --------------------------------------------------------------------------

class FixtureTests(ImageFixture):

    def test_the_fixture_is_not_trivially_empty(self):
        data = build_image()
        self.assertIn(b"IOPCIPrimaryMatch", data)
        self.assertGreater(len(data), 8192)

    def test_a_synthetic_image_yields_its_personalities(self):
        found, stats, _ = self.scan_bytes(build_image())
        self.assertEqual(2, len(found))
        self.assertEqual(2, stats["key_hits"])

    def test_the_publisher_is_recovered_from_after_the_match_key(self):
        found, _, _ = self.scan_bytes(build_image())
        publishers = sorted(row[4] for row in found)
        self.assertEqual(
            ["com.apple.driver.AppleIntelCFLGraphicsFramebuffer",
             "com.apple.driver.AppleIntelICLLPGraphicsFramebuffer"],
            publishers)

    def test_the_ioclass_is_recovered_from_before_the_match_key(self):
        found, _, _ = self.scan_bytes(build_image())
        for row in found:
            self.assertEqual("AppleIntelFramebufferController", row[2])

    def test_the_class_match_entity_is_decoded(self):
        """&amp; must come back as & or the mask reads as two tokens."""
        found, _, _ = self.scan_bytes(build_image())
        for row in found:
            self.assertEqual("0x03000000&0xff000000", row[3])
            self.assertNotIn("amp", row[3])

    def test_an_image_with_no_personalities_parses_to_nothing(self):
        found, stats, _ = self.scan_bytes(b"\x00" * 65536)
        self.assertEqual(0, len(found))
        self.assertEqual(0, stats["key_hits"])

    def test_all_three_match_key_spellings_are_read(self):
        bodies = [
            personality("pub.a", "A", [CFL_ID], match_key="IOPCIPrimaryMatch"),
            personality("pub.b", "B", [ICL_ID], match_key="IOPCIMatch"),
            personality("pub.c", "C", ["0x11118086"],
                        match_key="IOPCISecondaryMatch"),
        ]
        found, _, _ = self.scan_bytes(build_image(bodies))
        self.assertEqual(3, len(found))
        self.assertEqual(set(dm.MATCH_KEYS), set(row[0] for row in found))

    def test_a_personality_spanning_a_chunk_boundary_is_not_lost(self):
        """The overlap exists for this; without it a hit at the seam vanishes."""
        original = dm.CHUNK
        try:
            dm.CHUNK = 4096
            body = personality("pub.seam", "Seam", [CFL_ID])
            pad = b"\x00" * (4096 - 20)
            data = pad + body.encode("ascii") + b"\x00" * 4096
            found, _, _ = self.scan_bytes(data)
            self.assertEqual(1, len(found))
        finally:
            dm.CHUNK = original

    def test_a_personality_is_not_double_counted_across_the_overlap(self):
        original = dm.CHUNK
        try:
            dm.CHUNK = 4096
            body = personality("pub.seam", "Seam", [CFL_ID])
            data = b"\x00" * (4096 - 20) + body.encode("ascii") + b"\x00" * 4096
            _, stats, _ = self.scan_bytes(data)
            self.assertEqual(1, stats["key_hits"])
        finally:
            dm.CHUNK = original


# --------------------------------------------------------------------------

class CorroborationTests(ImageFixture):
    """Each arm must be individually falsifiable.

    A corroboration block whose arms cannot fail ratifies a blind parse as
    authoritative, which is worse than having no arms at all -- it converts an
    absence of coverage into a positive-looking result.
    """

    def failed_arms(self, text):
        return [label for label, ok in self.arms(text).items() if not ok]

    def test_a_healthy_image_passes_every_arm(self):
        _, _, text = self.scan_bytes(build_image())
        self.assertEqual([], self.failed_arms(text))
        self.assertIn("parse REACHES the match dictionaries", text)
        self.assertNotIn("UNPROVEN", text)

    def test_an_empty_image_fails_the_parsed_arm(self):
        _, _, text = self.scan_bytes(b"\x00" * 65536)
        failed = self.failed_arms(text)
        self.assertTrue(any("at least one personality was parsed" in f
                            for f in failed), failed)
        self.assertIn("UNPROVEN", text)

    def test_dropping_a_control_id_fails_only_that_control_arm(self):
        bodies = [
            personality("pub.cfl", "C", [CFL_ID]),
            # Ice Lake deliberately omitted.
        ]
        _, _, text = self.scan_bytes(build_image(bodies))
        failed = self.failed_arms(text)
        self.assertEqual(1, len(failed), failed)
        self.assertIn(ICL_ID, failed[0])
        self.assertIn("UNPROVEN", text)

    def test_dropping_the_other_control_fails_the_other_arm(self):
        """Both sides of the control pair, so neither is decoration."""
        bodies = [personality("pub.icl", "I", [ICL_ID])]
        _, _, text = self.scan_bytes(build_image(bodies))
        failed = self.failed_arms(text)
        self.assertEqual(1, len(failed), failed)
        self.assertIn(CFL_ID, failed[0])

    def test_stripping_every_class_match_fails_the_class_match_arm(self):
        bodies = [
            personality("pub.cfl", "C", [CFL_ID], class_match=""),
            personality("pub.icl", "I", [ICL_ID], class_match=""),
        ]
        _, _, text = self.scan_bytes(build_image(bodies))
        failed = self.failed_arms(text)
        self.assertEqual(1, len(failed), failed)
        self.assertIn("IOPCIClassMatch", failed[0])

    def test_stripping_a_provider_class_fails_the_provider_arm(self):
        bodies = [
            personality("pub.cfl", "C", [CFL_ID]),
            personality("pub.icl", "I", [ICL_ID], provider=""),
        ]
        _, _, text = self.scan_bytes(build_image(bodies))
        failed = self.failed_arms(text)
        self.assertEqual(1, len(failed), failed)
        self.assertIn("provider class", failed[0])

    def test_the_arms_are_reported_even_when_the_verdict_is_unproven(self):
        """A refusal must not pre-empt the diagnosis that explains it."""
        _, _, text = self.scan_bytes(b"\x00" * 65536)
        self.assertIn("[XX]", text)
        self.assertIn("CORROBORATION", text)


# --------------------------------------------------------------------------

class TargetVerdictTests(ImageFixture):

    def test_the_target_is_reported_absent_when_it_is_absent(self):
        _, _, text = self.scan_bytes(build_image())
        self.assertIn("RESULT    : ABSENT", text)
        self.assertNotIn("RESULT    : PRESENT", text)

    def test_the_target_is_reported_present_when_it_is_present(self):
        """The positive control for the headline finding.

        Without this the ABSENT verdict is satisfied by a tool that can only
        ever say ABSENT, which is the same shape as an assertion that cannot
        fail.
        """
        bodies = [
            personality("pub.cfl", "C", [CFL_ID]),
            personality("pub.icl", "I", [ICL_ID]),
            personality("com.apple.driver.AppleIntelTGLGraphicsFramebuffer",
                        "AppleIntelFramebufferController", [TGL_ID]),
        ]
        _, _, text = self.scan_bytes(build_image(bodies))
        self.assertIn("RESULT    : PRESENT", text)
        self.assertIn("AppleIntelTGLGraphicsFramebuffer", text)
        self.assertNotIn("RESULT    : ABSENT", text)

    def test_the_target_is_matched_case_insensitively(self):
        """Case-folding a hex literal folds its 0x prefix too.

        A comparison done on rendered text rather than on the value is how a
        lowercase id in a plist becomes a false ABSENT.
        """
        bodies = [
            personality("pub.cfl", "C", [CFL_ID]),
            personality("pub.icl", "I", [ICL_ID]),
            personality("pub.tgl", "T", [TGL_ID.lower()]),
        ]
        _, _, text = self.scan_bytes(build_image(bodies))
        self.assertIn("RESULT    : PRESENT", text)

    def test_a_lowercase_control_id_still_satisfies_its_arm(self):
        bodies = [
            personality("pub.cfl", "C", [CFL_ID.lower()]),
            personality("pub.icl", "I", [ICL_ID.lower()]),
        ]
        _, _, text = self.scan_bytes(build_image(bodies))
        self.assertEqual([], [l for l, ok in self.arms(text).items() if not ok])


# --------------------------------------------------------------------------

class ReportShapeTests(ImageFixture):

    def test_the_reach_line_equals_the_rows_in_the_body(self):
        """The excerpt-file defect, gated at the source rather than the artifact."""
        bodies = [
            personality("pub.a", "A", [CFL_ID, "0x11118086"]),
            personality("pub.b", "B", [ICL_ID, "0x22228086", "0x33331002"]),
        ]
        _, _, text = self.scan_bytes(build_image(bodies))
        stated = int(re.search(r"(\d+) distinct device ids", text).group(1))
        rows = re.findall(r"^  0[xX][0-9A-Fa-f]{8}\s", text, re.M)
        self.assertEqual(stated, len(rows))
        self.assertEqual(5, stated)

    def test_the_report_is_pure_ascii(self):
        _, _, text = self.scan_bytes(build_image())
        text.encode("ascii")

    def test_the_report_states_what_it_does_not_establish(self):
        _, _, text = self.scan_bytes(build_image())
        self.assertIn("WHAT THIS DOES NOT ESTABLISH", text)
        self.assertIn("RECOVERY image", text)

    def test_the_report_names_the_injection_caveat(self):
        """An absence that omits the known workaround overstates itself."""
        _, _, text = self.scan_bytes(build_image())
        self.assertIn("fake", text.lower())
        self.assertIn("Hackintosh", text)


# --------------------------------------------------------------------------

REPORT = os.path.join(ROOT, "display-match-report.txt")


@unittest.skipUnless(os.path.exists(REPORT),
                     "display-match-report.txt is absent from this checkout")
class CommittedReportTests(unittest.TestCase):
    """Properties of the committed artifact, without a 3 GiB rescan."""

    @classmethod
    def setUpClass(cls):
        with open(REPORT, "rb") as fh:
            cls.raw = fh.read()
        cls.text = cls.raw.decode("ascii")

    def id_rows(self):
        section = self.text.split("EVERY DEVICE ID", 1)[1]
        section = section.split("WHAT THIS DOES NOT ESTABLISH", 1)[0]
        return re.findall(r"^  (0[xX][0-9A-Fa-f]{8})\s", section, re.M)

    def test_the_committed_report_is_pure_ascii(self):
        self.raw.decode("ascii")

    def test_the_reach_line_equals_the_rows_in_the_body(self):
        """The one property that would have caught the inventory excerpt.

        `intel-kext-inventory.txt` stated 430 and showed 16, and a later grep
        for an absent thing read that truncation as a fact about macOS. Both
        of its lines were honest; together they were a trap. Set equality is
        what makes the two lines answer to each other.
        """
        stated = int(re.search(r"(\d+) distinct device ids",
                               self.text).group(1))
        rows = self.id_rows()
        self.assertEqual(stated, len(rows),
                         "header claims %d device ids, body shows %d -- "
                         "regenerate with\n  python tools/find-display-matches.py "
                         "> display-match-report.txt" % (stated, len(rows)))

    def test_the_section_title_agrees_with_the_reach_line(self):
        stated = int(re.search(r"(\d+) distinct device ids",
                               self.text).group(1))
        titled = int(re.search(r"EVERY DEVICE ID \((\d+),",
                               self.text).group(1))
        self.assertEqual(stated, titled)

    def test_the_body_is_not_empty(self):
        """Set equality is satisfied by 0 == 0; a floor stops that."""
        self.assertGreater(len(self.id_rows()), 100)

    def test_every_corroboration_arm_passed(self):
        self.assertNotIn("[XX]", self.text)
        self.assertIn("parse REACHES the match dictionaries", self.text)
        self.assertNotIn("UNPROVEN", self.text)

    def test_both_control_ids_appear_in_the_body(self):
        rows = set(r.upper() for r in self.id_rows())
        for ident, _note in dm.CONTROL_IDS:
            self.assertIn(ident.upper(), rows)

    def test_the_target_is_absent_from_the_body(self):
        """Measured against the BODY, never the header prose.

        A grep of the whole file for 9a68 returns 1 -- the tool's own target
        declaration. Reading that as a match is the same category error as
        grepping an excerpt.
        """
        rows = set(r.upper() for r in self.id_rows())
        self.assertNotIn(dm.TARGET_ID.upper(), rows)

    def test_the_verdict_line_agrees_with_the_body(self):
        rows = set(r.upper() for r in self.id_rows())
        present = dm.TARGET_ID.upper() in rows
        self.assertEqual(present, "RESULT    : PRESENT" in self.text)
        self.assertEqual(not present, "RESULT    : ABSENT" in self.text)

    def test_the_report_declares_itself_unfiltered(self):
        self.assertIn("UNFILTERED", self.text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
