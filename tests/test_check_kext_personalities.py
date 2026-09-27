#!/usr/bin/env python3
"""
test_check_kext_personalities.py -- negative controls for
``tools/check-kext-personalities.py``, plus a positive control over the real
phase0-kext/Info.plist.

Why this exists
---------------
The checker under test passed on its first run against the real tree. So does a
body of ``return 0``. The defect it exists to catch is the quietest one this
project has: a personality matching the WRONG PCI device is not a build error and
not a parse error -- the kext compiles, installs, loads, and never probes, which
on the operator's screen is indistinguishable from a broken ``probe()``, from
SIP, and from an unsigned-kext refusal. Nothing else in the tree reads
``Info.plist`` at all.

So every rejection the checker can hand out gets a fixture that must turn it red,
and a near-identical fixture that must stay green. A rejection with only the green
half tested is the blind spot moved one level up.

The three defects worth naming
------------------------------
Each of these produces a plausible-looking hex string that a reader scanning the
file would accept:

* **Wrong die, right marketing name.** ``0x2523`` is an RTX 3050 Ti -- in
  DEVICE-IDS.md, right vendor, same marketing family, but GA106 rather than
  GA107. Different architecture, different register headers. This is the one a
  human is least likely to spot.
* **Vendor-wide wildcard.** ``0x000010de`` is what this personality started as.
  It claims every NVIDIA card in the machine, including on a host that is not
  the target laptop.
* **Swapped pair.** ``IOPCIMatch`` is ``(device << 16) | vendor``. Writing it the
  other way round yields a valid 32-bit number that matches nothing.

Isolation
---------
Each test builds its own ``tempfile.mkdtemp()`` root holding a synthetic
DEVICE-IDS.md, CLAUDE.md and Info.plist, and passes it as ``argv[1]``. Nothing
here writes inside the real project, so this is safe to run while other terminals
are editing -- except ``RealRepository``, which reads the live plist deliberately
and is the point of the whole module.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKER = os.path.join(ROOT, "tools", "check-kext-personalities.py")

#
# A DEVICE-IDS.md reduced to the rows the checker parses, with the real values.
# GA106 is present on purpose: without a second die in the table, the
# "real silicon, wrong die" verdict could not be reached at all, and the test for
# it would be measuring the "appears nowhere" branch instead.
#
IDS_DOC = (
    "# RTX 3050 laptop device IDs\n"
    "\n"
    "| Marketing name | Device IDs | Die | Architecture |\n"
    "|---|---|---|---|\n"
    "| GeForce RTX 3050 Laptop GPU | `0x25A2`, `0x25A5`, `0x25E2`, `0x25E5` |"
    " GA107 | **Ampere** (arch `0x17`) |\n"
    "| GeForce RTX 3050 Ti Laptop GPU | `0x2523`, `0x2563` | GA106 | Ampere |\n"
)

CLAUDE_DOC = "An exploratory macOS driver for a mobile RTX 3050 (GA107).\n"


def run(root):
    """Run the checker over a repository root; return (exit code, output)."""
    proc = subprocess.run(
        [sys.executable, CHECKER, root],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
    )
    return proc.returncode, proc.stdout


def xml_escape(text):
    """Escape a value for an XML text node.

    Not incidental to the fixture. IOKit match strings may carry an ``&mask``
    suffix, and a bare ``&`` is invalid XML -- the first version of this module
    wrote it raw, so the two mask tests failed at the PARSER and never reached
    the mask logic they were written to exercise. One of them was a positive
    control, which means it was red for a reason that had nothing to do with the
    property under test. The real Info.plist would spell it ``&amp;`` too.
    """
    return (text.replace("&", "&amp;")
                .replace("<", "&lt;")
                .replace(">", "&gt;"))


def plist_for(body, personality="NV3050Probe"):
    """An Info.plist whose single personality carries `body`'s keys.

    Written as text rather than through plistlib so a test can plant malformed
    XML, which is one of the failures under test.
    """
    keys = "".join(
        "\t\t\t<key>%s</key>\n\t\t\t<string>%s</string>\n"
        % (xml_escape(k), xml_escape(v))
        for k, v in body.items())
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0">\n'
        "<dict>\n"
        "\t<key>CFBundleIdentifier</key>\n"
        "\t<string>local.nv3050.NV3050Probe</string>\n"
        "\t<key>IOKitPersonalities</key>\n"
        "\t<dict>\n"
        "\t\t<key>%s</key>\n"
        "\t\t<dict>\n"
        "%s"
        "\t\t</dict>\n"
        "\t</dict>\n"
        "</dict>\n"
        "</plist>\n" % (personality, keys))


def plist_for_many(bodies):
    """An Info.plist carrying several personalities, {name: body}.

    Separate from ``plist_for`` rather than a generalisation of it, because
    ``plist_for`` is what every single-personality test above already builds and
    a rewritten shared helper would quietly re-author twenty passing fixtures.
    Both emit the same text for one personality; ``test_the_two_builders_agree``
    pins that, so this cannot drift into testing a plist shape the rest of the
    suite never sees.
    """
    blocks = "".join(
        "\t\t<key>%s</key>\n\t\t<dict>\n%s\t\t</dict>\n"
        % (xml_escape(name),
           "".join("\t\t\t<key>%s</key>\n\t\t\t<string>%s</string>\n"
                   % (xml_escape(k), xml_escape(v))
                   for k, v in body.items()))
        for name, body in bodies.items())
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0">\n'
        "<dict>\n"
        "\t<key>CFBundleIdentifier</key>\n"
        "\t<string>local.nv3050.NV3050Probe</string>\n"
        "\t<key>IOKitPersonalities</key>\n"
        "\t<dict>\n"
        "%s"
        "\t</dict>\n"
        "</dict>\n"
        "</plist>\n" % blocks)


GOOD_BODY = {
    "CFBundleIdentifier": "local.nv3050.NV3050Probe",
    "IOClass": "NV3050Probe",
    "IOProviderClass": "IOPCIDevice",
    "IOPCIMatch": "0x25a210de",
}


class Fixtures(unittest.TestCase):
    """Builds synthetic repository roots one file at a time."""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="personalities-")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.write("DEVICE-IDS.md", IDS_DOC)
        self.write("CLAUDE.md", CLAUDE_DOC)

    def write(self, relpath, text):
        full = os.path.join(self.root, relpath.replace("/", os.sep))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        return full

    def plist(self, match=None, body=None, personality="NV3050Probe"):
        """Write phase0-kext/Info.plist, defaulting to the correct personality."""
        keys = dict(GOOD_BODY if body is None else body)
        if match is not None:
            keys["IOPCIMatch"] = match
        return self.write("phase0-kext/Info.plist",
                          plist_for(keys, personality=personality))


class CorrectPlistPasses(Fixtures):
    """The green half. Without it every rejection below could pass because the
    checker rejects everything, which is the opposite failure."""

    def test_the_target_device_id_is_accepted(self):
        self.plist()
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertIn("0x25A2", out)

    def test_any_other_ga107_id_in_the_table_is_also_accepted(self):
        """The accept set is the table's GA107 row, not one hardcoded value.

        If the checker compared against a single typed ID, this would go red --
        and the project would have a second table nothing keeps true.
        """
        self.plist(match="0x25e510de")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)

    def test_an_uppercase_match_string_is_accepted(self):
        """Hex case is not a defect, and rejecting it would be noise."""
        self.plist(match="0x25A210DE")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)


class WrongDieIsCaught(Fixtures):
    """The defect this checker exists for, and the hardest to see by eye."""

    def test_a_ga106_rtx_3050_ti_id_is_rejected(self):
        self.plist(match="0x252310de")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("GA106", out)
        self.assertIn("0x2523", out)

    def test_the_message_names_the_die_it_found_not_just_a_mismatch(self):
        """'does not match' sends the reader to the plist; naming GA106 sends
        them to the actual question, which is which laptop this is."""
        self.plist(match="0x256310de")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("GA107", out)
        self.assertIn("GA106", out)

    def test_an_id_absent_from_the_table_is_rejected_differently(self):
        """A device nowhere in NVIDIA's table is a distinct diagnosis from one
        that is real silicon on the wrong die; conflating them would point the
        reader at the wrong file."""
        self.plist(match="0xbeef10de")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("appears nowhere", out)


class WildcardIsCaught(Fixtures):
    """What this personality started as, per Info.plist's own comment."""

    def test_a_vendor_wide_wildcard_is_rejected(self):
        self.plist(match="0x000010de")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("wildcard", out)

    def test_a_mask_that_frees_the_device_half_is_rejected(self):
        """``&0x0000ffff`` constrains only the vendor, so the exact-looking
        device half is decoration -- the match is vendor-wide again."""
        self.plist(match="0x25a210de&0x0000ffff")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("device half", out)

    def test_a_mask_that_constrains_both_halves_is_accepted(self):
        """The paired positive: a mask is not itself the defect."""
        self.plist(match="0x25a210de&0xffffffff")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)


class SwappedPairIsCaught(Fixtures):

    def test_vendor_and_device_written_the_wrong_way_round(self):
        """0x10de25a2 is a valid 32-bit number that matches nothing."""
        self.plist(match="0x10de25a2")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("vendor half", out)

    def test_a_non_nvidia_vendor_is_rejected(self):
        self.plist(match="0x25a28086")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("0x8086", out)


class PersonalityShapeIsChecked(Fixtures):
    """A match string is inert unless the rest of the personality is sound."""

    def test_a_personality_with_no_match_key_is_rejected(self):
        body = dict(GOOD_BODY)
        del body["IOPCIMatch"]
        self.plist(body=body)
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("can never be selected", out)

    def test_a_non_pci_provider_class_is_rejected(self):
        """IOPCIMatch is only consulted for a PCI provider."""
        body = dict(GOOD_BODY, IOProviderClass="IOService")
        self.plist(body=body)
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("IOProviderClass", out)

    def test_a_missing_ioclass_is_rejected(self):
        body = dict(GOOD_BODY)
        del body["IOClass"]
        self.plist(body=body)
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("IOClass", out)

    def test_ioppci_primary_match_is_also_read(self):
        """IOPCIPrimaryMatch is legitimate; if it were ignored, a wrong device
        written there would be silently exempt."""
        body = dict(GOOD_BODY)
        del body["IOPCIMatch"]
        body["IOPCIPrimaryMatch"] = "0x252310de"
        self.plist(body=body)
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("GA106", out)


class MalformedInputIsReportedNotCrashed(Fixtures):

    def test_a_double_hyphen_in_a_comment_is_diagnosed_by_name(self):
        """XML forbids ``--`` inside a comment and rejects the WHOLE file with a
        message that does not name the line. Info.plist carries a warning about
        this in its own prose, which means it has been hit before.
        """
        text = plist_for(GOOD_BODY).replace(
            "<dict>\n", "<dict>\n\t<!-- a comment with a -- double hyphen -->\n", 1)
        self.write("phase0-kext/Info.plist", text)
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("double hyphen", out)
        self.assertNotIn("Traceback", out)

    def test_a_plist_with_no_personalities_is_rejected(self):
        self.write("phase0-kext/Info.plist",
                   '<?xml version="1.0" encoding="UTF-8"?>\n'
                   '<plist version="1.0">\n<dict>\n'
                   "\t<key>CFBundleName</key>\n\t<string>NV3050Probe</string>\n"
                   "</dict>\n</plist>\n")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("no IOKitPersonalities", out)
        self.assertNotIn("Traceback", out)

    def test_a_missing_plist_is_a_clean_failure(self):
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("Info.plist", out)
        self.assertNotIn("Traceback", out)

    def test_a_non_hex_match_string_is_a_clean_failure(self):
        self.plist(match="not-a-number")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("hexadecimal", out)
        self.assertNotIn("Traceback", out)


class DerivationIsNotHardcoded(Fixtures):
    """The accept set must come from DEVICE-IDS.md, or this is a second table."""

    def test_a_missing_device_ids_document_is_a_failure(self):
        os.remove(os.path.join(self.root, "DEVICE-IDS.md"))
        self.plist()
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("DEVICE-IDS.md", out)

    def test_a_table_that_yields_too_few_ids_is_a_failure(self):
        """A broken table parse would otherwise reject a CORRECT plist and send
        the reader to Info.plist -- red for a true reason phrased as a false one.
        """
        self.write("DEVICE-IDS.md",
                   "| Name | Device IDs | Die | Arch |\n|---|---|---|---|\n"
                   "| RTX 3050 | `0x25A2` | GA107 | Ampere |\n")
        self.plist()
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("table parse is", out)

    def test_a_claude_md_that_no_longer_names_the_target_die_is_a_failure(self):
        """If the project re-targets, this checker must go red rather than keep
        validating against silicon nobody is working on.
        """
        self.write("CLAUDE.md", "An exploratory driver for some other GPU.\n")
        self.plist()
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("GA107", out)


#
# The Intel track.
#
# Everything above this line was written when the checker held ONE die and ONE
# vendor as module constants. The two-track rewrite has to be provable, and the
# proof cannot be "the real tree still passes" -- the real tree has exactly one
# personality and it is the NVIDIA one, so a checker that ignored the Intel
# track entirely would pass every test above.
#
# The fixtures deliberately keep INTEL-DEVICE-IDS.md OPTIONAL. setUp() does not
# write it, because a tree carrying only one track's document must stay checkable
# -- and because the absent-document path is itself a verdict this checker has to
# get right. `intel_ids()` opts a test in.
#
INTEL_IDS_DOC = (
    "# Intel Gen12 device IDs\n"
    "\n"
    "| Marketing name | Device IDs | Die | Architecture |\n"
    "|---|---|---|---|\n"
    "| TigerLake-H GT1 | `0x9A60`, `0x9A68`, `0x9A70` | TGL | Gen12 (Xe-LP) |\n"
    "| TigerLake GT2 | `0x9A40`, `0x9A49`, `0x9A59`, `0x9A78` | TGL |"
    " Gen12 (Xe-LP) |\n"
    "| Alder Lake-P GT2 | `0x46A6`, `0x46A8` | ADL | Gen12 (Xe-LP) |\n"
)

# CLAUDE.md never spells the bare token TGL. It says "Tiger Lake" and "Gen12",
# which is exactly why the checker carries an alias list per track -- a literal
# die-name search would report a disagreement against a document that DOES
# describe the silicon. This fixture reproduces the real document's idiom rather
# than the die name, so the alias list is load-bearing here too.
CLAUDE_DOC_BOTH = (
    "An exploratory macOS driver for a mobile RTX 3050 (GA107).\n"
    "A second track adds a Gen12 (Tiger Lake) Intel display layer.\n"
)

INTEL_BODY = {
    "CFBundleIdentifier": "local.tgl.IntelDisplayProbe",
    "IOClass": "IntelDisplayProbe",
    "IOProviderClass": "IOPCIDevice",
    "IOPCIMatch": "0x9a688086",
}


class TheTwoPlistBuildersAgree(unittest.TestCase):
    """``plist_for_many`` with one entry must emit what ``plist_for`` emits.

    Without this the multi-personality tests would be exercising a plist shape
    nothing else in the suite ever produces, and a divergence -- a missing
    CFBundleIdentifier, a differently-nested IOKitPersonalities dict -- would
    read as a checker defect rather than a fixture defect.
    """

    def test_the_two_builders_agree(self):
        self.assertEqual(plist_for(GOOD_BODY),
                         plist_for_many({"NV3050Probe": GOOD_BODY}))


class IntelTrackFixtures(Fixtures):
    """Fixtures plus the Intel document and a CLAUDE.md naming both targets."""

    def setUp(self):
        super().setUp()
        self.write("INTEL-DEVICE-IDS.md", INTEL_IDS_DOC)
        self.write("CLAUDE.md", CLAUDE_DOC_BOTH)

    def intel_plist(self, match=None):
        body = dict(INTEL_BODY)
        if match is not None:
            body["IOPCIMatch"] = match
        self.write("phase0-kext/Info.plist", plist_for(body))


class IntelPersonalityIsClassifiedAgainstItsOwnTrack(IntelTrackFixtures):
    """The rewrite's whole point.

    Before it, a correct Intel personality produced ``vendor half is 0x8086, not
    0x10DE`` -- a message right about its own arithmetic and wrong about the
    world, the same failure shape handoff16 records for the IG_PIPE_ prefix
    collision. A message like that is worse than silence: it names a real
    constant, quotes a real number, and describes a defect that does not exist.
    """

    def test_the_observed_tiger_lake_id_is_accepted(self):
        """0x9A68 is the one Intel ID witnessed on the target laptop."""
        self.intel_plist()
        code, out = run(self.root)
        self.assertEqual(code, 0, out)

    def test_any_other_tgl_id_in_the_table_is_also_accepted(self):
        """The accept set is the table's TGL rows, not the one observed value.

        GT1 and GT2 both decode to TGL on purpose -- they differ in execution-unit
        count, not in the display block this track touches.
        """
        self.intel_plist(match="0x9a408086")
        code, out = run(self.root)
        self.assertEqual(code, 0, out)

    def test_a_wrong_die_intel_id_is_rejected_and_names_the_die(self):
        """0x46A6 is real Intel Gen12 silicon on a DIFFERENT die.

        Same vendor, same architecture family, same marketing register names --
        and a different display block. Naming ADL sends the reader to the actual
        question rather than to the plist.
        """
        self.intel_plist(match="0x46a68086")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("TGL", out)
        self.assertIn("ADL", out)

    def test_an_intel_id_absent_from_the_table_is_rejected_differently(self):
        self.intel_plist(match="0xbeef8086")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("appears nowhere", out)
        self.assertIn("INTEL-DEVICE-IDS.md", out)

    def test_an_intel_wildcard_names_intel_not_nvidia(self):
        """The wildcard message must name the vendor the token actually carries.

        A wildcard diagnosis reading "claims EVERY NVIDIA card" against an 0x8086
        token would be the pre-rewrite bug wearing a different sentence.
        """
        self.intel_plist(match="0x00008086")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("wildcard", out)
        self.assertIn("Intel", out)
        self.assertNotIn("EVERY NVIDIA", out)

    def test_a_swapped_intel_pair_is_rejected(self):
        """0x8086 in the DEVICE half is not a vendor this project tracks."""
        self.intel_plist(match="0x80869a68")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("vendor half", out)


class BothTracksCoexist(IntelTrackFixtures):
    """Two personalities in one plist, one per vendor.

    This is the configuration the plist is about to grow, and it is the one the
    old checker could not express at all: whichever personality it classified
    second would be measured against the other's die.
    """

    def both(self, nv_match="0x25a210de", ig_match="0x9a688086"):
        nv = dict(GOOD_BODY)
        nv["IOPCIMatch"] = nv_match
        ig = dict(INTEL_BODY)
        ig["IOPCIMatch"] = ig_match
        self.write("phase0-kext/Info.plist",
                   plist_for_many({"NV3050Probe": nv, "IntelDisplayProbe": ig}))

    def test_one_personality_per_vendor_passes(self):
        self.both()
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertIn("2 personality(ies)", out)

    def test_a_wrong_die_on_either_track_still_fails(self):
        """A correct personality must not vouch for a broken sibling.

        Order matters: NV3050Probe sorts first, so a checker that stopped at the
        first OK would pass this. Both directions are pinned.
        """
        self.both(ig_match="0x46a68086")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("ADL", out)

        self.both(nv_match="0x252310de")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("GA106", out)

    def test_both_tracks_are_reported_as_examined(self):
        """Two personalities checking one token each is two tokens, not one."""
        self.both()
        code, out = run(self.root)
        self.assertEqual(code, 0, out)
        self.assertIn("2 match token(s)", out)


class AnUnvouchedTrackDoesNotPassByDefault(Fixtures):
    """The trap the two-track rewrite could most easily have fallen into.

    ``Fixtures.setUp`` writes DEVICE-IDS.md and never INTEL-DEVICE-IDS.md, so
    these run against a tree where the Intel document is absent. The tempting
    implementation -- skip any track whose document is missing -- turns an
    unvouched personality into a silent pass, which is the exact defect class
    this checker exists for. An absent document must be fatal FOR THAT VENDOR and
    harmless for the other.
    """

    def test_an_intel_personality_without_its_document_is_rejected(self):
        self.plist(match="0x9a688086")
        code, out = run(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn("INTEL-DEVICE-IDS.md", out)

    def test_the_nvidia_track_still_passes_with_the_intel_document_absent(self):
        """The negative control for the test above.

        Without this, that test would pass just as well against a checker that
        refused every plist whenever any document was missing.
        """
        self.plist()
        code, out = run(self.root)
        self.assertEqual(code, 0, out)


class RealRepository(unittest.TestCase):
    """The live Info.plist, which is the only reason any of the above matters.

    Change the match string, widen it back to a wildcard, break the plist XML, or
    re-target the project without updating both files, and this goes red in the
    one gate that runs on a bare checkout.
    """

    def test_the_live_plist_passes(self):
        code, out = run(ROOT)
        self.assertEqual(code, 0, out)

    def test_the_live_run_examined_at_least_one_match_token(self):
        """Exit 0 having checked nothing is the vacuous pass this whole family of
        gates exists to break."""
        code, out = run(ROOT)
        self.assertEqual(code, 0, out)
        self.assertIn("match token(s)", out)
        self.assertNotIn("0 match token(s)", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
