#!/usr/bin/env python3
"""
test_capture_integrity.py -- the Tiger Lake captures are unrepeatable, so the
tree must notice if they change.

Why this exists
---------------
``igpu-results/`` holds the only artifacts in this project produced by register
reads on the target silicon. There are exactly two, and there will never be a
third: the laptop cannot hold Linux and macOS at once, and macOS is going on it.

    tgl-capture-2026-08-16-022556.txt        first boot, ~1 hour, one script
    tgl-final-capture-2026-08-18-201412.txt  second boot, the one-file capture

Constants in ``phase0-kext/IntelDisplay.hpp`` are marked ``*_OBSERVED_*`` and
cite these files. If a byte above the READING NOTES divider changes, a value
this project calls *measured* silently becomes a value somebody typed, and the
evidence trail -- the only thing separating this tree from a pile of guesses --
is gone with no way to re-derive it.

What is frozen and what is not
------------------------------
Each capture is two documents in one file. Above the divider is the machine's
output verbatim. Below it is host-side analysis written afterwards, which is
ordinary prose and must stay editable. So the hash covers the region ABOVE the
divider only, and arm 3 exists to prove that editing the notes does not move it.

The divider line and the rule of ``=`` immediately above it belong to the NOTES
half -- they were typed on the host, not printed by the boot. Neither capture
has the same line count, and the 2026-08-18 one grew a divider only after it
landed in the tree, so the split is found by CONTENT, never by line number.

Why the file pattern is ``tgl-*capture-*.txt`` and not ``tgl-capture-*.txt``
---------------------------------------------------------------------------
MEASURED 2026-08-18, and it is the defect this revision fixes. The pattern was
``tgl-capture-*.txt``. The second artifact landed as ``tgl-final-capture-...``,
which that glob does NOT match -- so the one module in the tree whose entire job
is protecting unrepeatable machine output could not see the larger of the two
captures, and reported green while doing it. A gate that silently narrows its
own subject set is worse than no gate: it converts an absence of coverage into a
positive-looking result. ``test_every_artifact_on_disk_is_pinned`` is the fix
that generalises -- a third capture under any ``tgl-*capture-*`` spelling reddens
until somebody measures it and adds a row.

Why the anchors are per-file rather than two module globals
-----------------------------------------------------------
Also measured 2026-08-18. ``BELOW_ANCHOR`` was ``UNPROGRAMMED``, chosen because
it occurs only in the 2026-08-16 notes. In the 2026-08-18 capture that token
occurs **21 times ABOVE the divider** -- it is a verdict word the script itself
prints. Arm 3 rewrites the below-anchor and then requires the hash to HOLD, so
reusing that anchor on the new file would have rewritten 21 bytes of frozen
machine output and asserted that the digest did not move. The arm would have
failed loudly, but for the wrong reason, and the tempting fix ("skip that file")
is exactly the narrowing this module just got caught doing.

Both anchors are therefore verified BY COUNT against each artifact before use.
``0x9a68`` is 1-above / 0-below in both files (the PCI device id of the Tiger
Lake iGPU -- a capture that no longer contains it is not a capture of this
machine). ``MACHINE:`` is 0-above / 1-below in both (it opens the host-written
notes header). ``a-mutation-that-matches-nothing-prints-ok``: a replace that
matches zero bytes is a no-op, and a no-op mutation makes an arm print a
confident green while testing nothing.

Measured 2026-08-18 by hashing the live artifacts, per the table below.
"""
import glob
import hashlib
import os
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, os.pardir))

# Broad on purpose. See the module docstring: the previous, narrower pattern
# silently excluded the second capture.
_PATTERN = os.path.join(_ROOT, "igpu-results", "tgl-*capture-*.txt")

# The divider line itself, and the rule of '=' characters immediately above it,
# both belong to the NOTES half -- they were typed on the host, not printed by
# the boot. The frozen region ends at the last line the script emitted.
DIVIDER_MARK = b"READING NOTES"
RULE_PREFIX = b"===="

# Occurs once above the divider and zero times below, in EVERY artifact --
# asserted by count in arm 2 before it is used. It is the PCI device id of the
# Tiger Lake iGPU, so a capture that no longer contains it is not a capture of
# this machine.
ABOVE_ANCHOR = b"0x9a68"

# Occurs once below the divider and zero times above, in EVERY artifact --
# asserted by count in arm 3 before it is used. It opens the host-written notes
# header. Do NOT substitute a word the capture script can print; see the module
# docstring on why ``UNPROGRAMMED`` was wrong here.
BELOW_ANCHOR = b"MACHINE:"

# Per-artifact frozen-region measurements. Keyed by basename so the table reads
# as a record of two boots rather than as a pair of magic numbers.
#
#   Measured 2026-08-16 -- first boot, provenance plus a small MMIO pass.
#   Measured 2026-08-18 -- second boot, the 25-range one-file capture. Its
#   region is byte-identical to the operator's backup of the raw run
#   (md5 3bd389badd5cf980dce0f55c45452a55, 71344 bytes); the region is 71345
#   because the notes were appended after a newline the raw file did not end on.
EXPECTED = {
    "tgl-capture-2026-08-16-022556.txt": {
        "bytes": 3539,
        "lines": 69,
        "sha256":
            "ca500bd06c906a08fbe2f8cce3df7c3b641153de192500e85328b387410c4dd9",
    },
    "tgl-final-capture-2026-08-18-201412.txt": {
        "bytes": 71345,
        "lines": 1384,
        "sha256":
            "30779a1078a081f980e990235b8a5181d7858302f1cf853aca45669857e06e9c",
    },
}


def artifacts():
    return sorted(glob.glob(_PATTERN))


def split_capture(blob):
    """Return (frozen_region, notes_region) for a capture file's bytes.

    The split point is found by content. Raises ValueError rather than guessing
    if the divider is absent -- a capture with no divider is a file this module
    does not understand, and guessing a boundary would hash the wrong bytes and
    then confidently report on them.
    """
    lines = blob.split(b"\n")
    marks = [i for i, ln in enumerate(lines) if DIVIDER_MARK in ln]
    if not marks:
        raise ValueError("no %r line found" % DIVIDER_MARK.decode())
    cut = marks[0]
    while cut - 1 >= 0 and lines[cut - 1].strip().startswith(RULE_PREFIX):
        cut -= 1
    return b"\n".join(lines[:cut]), b"\n".join(lines[cut:])


def digest(region):
    return hashlib.sha256(region).hexdigest()


class CaptureIntegrityTests(unittest.TestCase):
    """Five arms over every artifact, plus the subject-set check above them."""

    def each(self):
        """Yield (basename, blob, expected) for every artifact on disk.

        Refuses rather than skips when the set is empty. A capture directory
        that lost its files must redden; an empty loop prints ok.
        """
        paths = artifacts()
        self.assertTrue(
            paths,
            "no capture matched %s. The artifacts are unrepeatable, so an "
            "empty subject set is a missing-evidence failure, not a skip."
            % _PATTERN)
        for path in paths:
            name = os.path.basename(path)
            with open(path, "rb") as handle:
                yield name, handle.read(), EXPECTED.get(name)

    # ------------------------------------------------------------- arm 0

    def test_every_artifact_on_disk_is_pinned(self):
        """The subject set and the expectation table must agree both ways.

        This is the arm that would have caught 2026-08-18's defect. A capture
        can be added to igpu-results/ without touching this file, and under the
        old narrow glob such a file was simply invisible -- unhashed, unguarded,
        and reported green. Requiring a row per artifact means the only way to
        land a new capture is to measure it.
        """
        on_disk = {os.path.basename(p) for p in artifacts()}
        pinned = set(EXPECTED)

        unpinned = sorted(on_disk - pinned)
        self.assertFalse(
            unpinned,
            "%s present in igpu-results/ with no row in EXPECTED. Unrepeatable "
            "machine output is sitting in the tree with nothing hashing it. "
            "Measure the frozen region and add a row -- do not narrow the glob."
            % ", ".join(unpinned))

        missing = sorted(pinned - on_disk)
        self.assertFalse(
            missing,
            "%s has a row in EXPECTED but is not on disk. Either an "
            "irreplaceable capture was deleted, or the glob stopped matching "
            "it -- both are failures, and the second is the one that hides."
            % ", ".join(missing))

    def test_the_old_narrow_pattern_would_have_missed_an_artifact(self):
        """Non-vacuity for the widening, stated as a fact about the tree.

        Without this, a future edit could quietly restore ``tgl-capture-*.txt``
        and every other arm would keep passing -- on a smaller subject set. The
        assertion is that the broad pattern is STRICTLY broader here, so the
        widening is load-bearing rather than cosmetic.
        """
        narrow = set(glob.glob(
            os.path.join(_ROOT, "igpu-results", "tgl-capture-*.txt")))
        broad = set(artifacts())

        self.assertTrue(
            narrow < broad,
            "the broad pattern matches no more than the old narrow one (%d vs "
            "%d). Either an artifact was renamed or deleted, or this module is "
            "back to guarding a subset of the evidence."
            % (len(broad), len(narrow)))

    # ------------------------------------------------------------- arm 1

    def test_the_frozen_region_matches_its_measurement(self):
        """Byte count, line count and digest, per artifact.

        All three, because they fail differently: a digest mismatch says a byte
        moved, a length mismatch says how much, and a line-count mismatch
        distinguishes an in-place edit from an insertion.
        """
        for name, blob, want in self.each():
            with self.subTest(capture=name):
                region, _notes = split_capture(blob)
                self.assertEqual(
                    want["bytes"], len(region),
                    "frozen region of %s is %d bytes, measured at %d"
                    % (name, len(region), want["bytes"]))
                self.assertEqual(
                    want["lines"], len(region.split(b"\n")),
                    "frozen region of %s is %d lines, measured at %d"
                    % (name, len(region.split(b"\n")), want["lines"]))
                self.assertEqual(
                    want["sha256"], digest(region),
                    "the machine output in %s changed. This is unrepeatable "
                    "silicon; every *_OBSERVED_* constant citing it is now "
                    "unsourced." % name)

    # ------------------------------------------------------------- arm 2

    def test_an_edit_above_the_divider_changes_the_hash(self):
        """Non-vacuity. Arm 1 passing proves the bytes are hashed, not that the
        hash covers the half that matters. The anchor is counted before use, so
        a mutation that matches nothing cannot print a confident green.
        """
        for name, blob, want in self.each():
            with self.subTest(capture=name):
                region, notes = split_capture(blob)
                self.assertEqual(
                    1, region.count(ABOVE_ANCHOR),
                    "%s occurs %d times above the divider in %s; the mutation "
                    "below assumes exactly one."
                    % (ABOVE_ANCHOR.decode(), region.count(ABOVE_ANCHOR), name))
                self.assertEqual(
                    0, notes.count(ABOVE_ANCHOR),
                    "%s also occurs below the divider in %s, so this arm no "
                    "longer isolates the frozen half."
                    % (ABOVE_ANCHOR.decode(), name))

                # Same byte width, so the length assertions in arm 1 stay in
                # play and the only thing that can move is the digest.
                mutated = blob.replace(ABOVE_ANCHOR, b"0x9a69")
                self.assertNotEqual(blob, mutated,
                                    "the mutation applied to nothing")

                mut_region, _ = split_capture(mutated)
                self.assertEqual(
                    len(region), len(mut_region),
                    "the mutation changed the region LENGTH, so a differing "
                    "hash would prove nothing about content sensitivity.")
                self.assertNotEqual(
                    want["sha256"], digest(mut_region),
                    "a byte of machine output changed in %s and the digest did "
                    "not move. This gate cannot see the thing it exists for."
                    % name)

    # ------------------------------------------------------------- arm 3

    def test_an_edit_below_the_divider_leaves_the_hash_alone(self):
        """The other half of the property, and the one that keeps this gate from
        becoming a nuisance that people disable. Notes are prose and get edited;
        if that reddened the gate, the gate would be removed and the machine
        output would end up unguarded.
        """
        for name, blob, want in self.each():
            with self.subTest(capture=name):
                region, notes = split_capture(blob)
                self.assertEqual(
                    1, notes.count(BELOW_ANCHOR),
                    "%s occurs %d times below the divider in %s; the mutation "
                    "below assumes exactly one."
                    % (BELOW_ANCHOR.decode(), notes.count(BELOW_ANCHOR), name))
                self.assertEqual(
                    0, region.count(BELOW_ANCHOR),
                    "%s also occurs ABOVE the divider in %s. Rewriting it would "
                    "edit frozen machine output while asserting the hash holds "
                    "-- pick an anchor the capture script cannot print."
                    % (BELOW_ANCHOR.decode(), name))

                mutated = blob.replace(BELOW_ANCHOR, b"MACHINF:")
                self.assertNotEqual(blob, mutated,
                                    "the mutation applied to nothing")

                mut_region, _ = split_capture(mutated)
                self.assertEqual(
                    want["sha256"], digest(mut_region),
                    "editing the READING NOTES moved the capture hash for %s. "
                    "The divider split is wrong, and ordinary note edits are "
                    "being reported as changes to unrepeatable machine output."
                    % name)

    # ------------------------------------------------------------- arm 4

    def test_the_divider_is_found_by_content_not_by_line_number(self):
        """Inserting a line into the notes must not shift the frozen region.

        A line-number split would pass arm 1 today and silently start hashing
        note prose the first time somebody adds a paragraph.
        """
        for name, blob, _want in self.each():
            with self.subTest(capture=name):
                region, _notes = split_capture(blob)
                shifted = blob + b"\nan extra trailing note line\n"
                shifted_region, _ = split_capture(shifted)
                self.assertEqual(
                    region, shifted_region,
                    "appending a line to the notes moved the frozen region of "
                    "%s" % name)

    # ------------------------------------------------------------- arm 5

    def test_the_capture_is_lf_only(self):
        """A CRLF normalisation pass would change every line of the frozen half
        at once. That is the most likely accidental corruption on a Windows
        checkout, and it is invisible in a diff viewer.
        """
        for name, blob, _want in self.each():
            with self.subTest(capture=name):
                self.assertEqual(
                    0, blob.count(b"\r"),
                    "%s contains %d CR bytes. Something rewrote line endings "
                    "in machine output." % (name, blob.count(b"\r")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
