#!/usr/bin/env python3
"""
test_control_bytes.py -- no repo-owned text file may carry a stray control byte.

Why this exists
---------------
This is a sibling of ``test_line_endings.py``, not an extension of it. That gate
is deliberately narrow: it covers ``.cmd``/``.bat`` only, because those are the
files ``cmd.exe`` seeks by byte offset. It says nothing about any other file
type, and nothing about control bytes other than CR and LF.

That gap was real. Measured 2026-08-11, ``handoff8.md`` carried two literal
control bytes:

    offset 40196   0x08   `tests<BS>uild-msvc.bat`        renders as testsuild-msvc.bat
    line 869       0x0D   `tests<CR>un-python-tests.bat`  renders as testsun-python-tests.bat

Both came from a writer interpreting the backslash sequences ``\\b`` and ``\\r``
as C escapes instead of emitting a literal backslash. Neither was visible to
``test_line_endings.py``: markdown is not in ``BATCH_SUFFIXES``, and the bare CR
check it does have applies only to batch files.

The tell, and why reading the file is not enough
-----------------------------------------------
A search tool renders ``tests<BS>uild`` as the *characters* ``testsuild``. So the
damage looks like an ordinary typo, and an ``Edit`` against that displayed string
fails to match -- which reads as "a peer changed the file under me" rather than
"the bytes differ from the rendering". Two edits were burned on that inversion
before the raw bytes were printed.

That is the same shape as the ``test_falcon.cpp`` backslash scare, where a search
tool rendered ``//`` comment lines as ``\\`` and the file was in fact clean. Same
symptom, opposite cause. Only a byte-level read separates them, so this check has
to be mechanical and it has to live in the gate that works on a bare checkout.

What this test does NOT claim
----------------------------
Passing means no repo-owned text file contains a C0 control byte other than the
line-ending characters and TAB. It does not mean those files are correct, it does
not check line endings (``test_line_endings.py`` owns that), and it says nothing
about the ``reference/`` tree, which is vendored and not ours to normalize.

Run:
    python -m unittest discover -s tests -p test_control_bytes.py -v
    tests\\run-python-tests.bat        # picked up automatically by discovery
"""

import os
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    os.pardir))

# Text file types this repo authors. Binary formats are excluded because a 0x08
# in a PNG or a VMDK is data, not damage -- flagging them would make the gate
# noisy enough to be switched off, which is worse than not having it.
TEXT_SUFFIXES = (".md", ".py", ".cmd", ".bat", ".cpp", ".hpp", ".txt",
                 ".json", ".yml", ".yaml", ".cmake")

# Same skip set as test_line_endings.py, for the same reasons: `reference/`
# holds vendored NVIDIA source that other tools diff against, and `shots/`
# holds captured artifacts that are evidence rather than source.
SKIP_DIRS = {".git", ".claude-crew", "reference", "build", "__pycache__",
             "shots", "node_modules", ".venv"}

# Files named exactly this are skipped wherever they appear.
SKIP_NAMES = {"CMakeCache.txt"}

# TAB is legitimate in source and in Makefiles. CR and LF are line endings and
# belong to test_line_endings.py -- checking them here would make one defect
# fail two gates and obscure which one is authoritative.
ALLOWED_CONTROL = frozenset((0x09, 0x0A, 0x0D))


def find_text_files():
    """Every repo-owned text file, as (relative_path, absolute_path) pairs."""
    found = []
    for dirpath, dirnames, filenames in os.walk(REPO):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name in SKIP_NAMES:
                continue
            if name.lower().endswith(TEXT_SUFFIXES):
                absolute = os.path.join(dirpath, name)
                found.append((os.path.relpath(absolute, REPO), absolute))
    return sorted(found)


def stray_control_bytes(path):
    """[(offset, byte_value, context_bytes), ...] for every disallowed C0 byte.

    Read as bytes, never as text. Opening in text mode with universal newlines
    translates 0x0D out of existence and can raise on a 0x08 under a strict
    codec -- either way the measurement stops describing the file.

    DEL (0x7F) is included because it is as invisible as the C0 set and arrives
    by the same route (a mishandled escape), even though it is not technically
    C0.
    """
    with open(path, "rb") as fh:
        blob = fh.read()
    found = []
    for offset, value in enumerate(blob):
        if (value < 0x20 or value == 0x7F) and value not in ALLOWED_CONTROL:
            found.append((offset, value, blob[max(0, offset - 24):offset + 24]))
    return found


class TextFilesHaveNoStrayControlBytes(unittest.TestCase):

    def test_at_least_one_text_file_is_discovered(self):
        # A walk that silently finds nothing makes every assertion below
        # vacuously true -- a green gate measuring an empty set. Same class of
        # failure as the stem-relative lock scan that returned zero of four
        # artifacts and was read as "proven absent".
        self.assertTrue(find_text_files(),
                        "no text files found under %s -- the walk, "
                        "TEXT_SUFFIXES or SKIP_DIRS is wrong, and this suite "
                        "is checking nothing" % REPO)

    def test_the_walk_reaches_the_files_this_gate_exists_for(self):
        # Naming them explicitly. A suffix list that silently stopped matching
        # .md would leave the discovery assertion above green (there are plenty
        # of .py files) while the file that actually got corrupted went
        # unscanned.
        relatives = {relative for relative, _absolute in find_text_files()}
        for expected in ("handoff8.md", "CLAUDE.md",
                         os.path.join("tests", "test_line_endings.py"),
                         os.path.join("tests", "run-python-tests.bat")):
            self.assertIn(expected, relatives,
                          "%s is not being scanned" % expected)

    def test_no_text_file_carries_a_stray_control_byte(self):
        offenders = []
        for relative, absolute in find_text_files():
            for offset, value, context in stray_control_bytes(absolute):
                offenders.append("%s: 0x%02X at offset %d, context %r"
                                 % (relative, value, offset, context))
        self.assertEqual(
            [], offenders,
            "text file(s) containing a stray control byte:\n  %s\n"
            "These are INVISIBLE in a rendered view and in most search tool "
            "output -- a 0x08 renders as if the preceding character were simply "
            "absent. If an Edit fails to match a string your search tool just "
            "displayed, that is evidence the bytes differ from the rendering, "
            "NOT evidence the file changed under you. Read the raw bytes:\n"
            "  data = open(p,'rb').read()\n"
            "  [(hex(b), i) for i,b in enumerate(data) if b < 0x20 "
            "and b not in (0x09,0x0a,0x0d)]\n"
            "Repair at byte level, asserting the anchor is unique first."
            % "\n  ".join(offenders))


class StrayControlBytesItself(unittest.TestCase):
    """Negative controls -- the scanner above must be able to go RED.

    Every assertion in the class above runs against the real repo, which is
    currently clean. A scanner that silently stopped matching -- an inverted
    comparison, an ALLOWED_CONTROL set that swallowed everything -- would
    produce a green run indistinguishable from a correct one. Per the
    `an-assertion-that-cannot-fail-is-not-coverage` finding, a passing check
    over only-conforming inputs measures nothing.

    There is one control per byte class that actually caused damage, plus the
    allowed bytes asserted NOT to trip it. A scanner that flagged TAB would
    fail every .cpp in the repo and get deleted rather than fixed.
    """

    def _write(self, blob, suffix=".md"):
        import tempfile
        fd, path = tempfile.mkstemp(suffix=suffix, prefix="ctlbytes-")
        with os.fdopen(fd, "wb") as fh:
            fh.write(blob)
        self.addCleanup(os.remove, path)
        return path

    def test_a_clean_file_reports_nothing(self):
        # Anchors every mutant below. Without it, a mutant could be flagged
        # because the BASELINE is malformed rather than because the mutation
        # was caught, and all of them would pass for the wrong reason.
        self.assertEqual(
            stray_control_bytes(self._write(b"# Title\r\n\r\nProse.\r\n")), [])

    def test_the_exact_backspace_from_handoff8_is_detected(self):
        # The literal damage: `tests\build-msvc.bat` written with \b handled as
        # a C escape. This is the byte a search tool renders away entirely.
        found = stray_control_bytes(
            self._write(b"passes -- `tests\x08uild-msvc.bat` is what does\n"))
        self.assertEqual(len(found), 1, found)
        self.assertEqual(found[0][1], 0x08)

    def test_the_offset_and_context_are_reported_not_just_the_count(self):
        # "A control byte exists somewhere in a 48 KiB file" is not actionable.
        # The offset is what makes a byte-level repair possible at all.
        found = stray_control_bytes(self._write(b"abc\x08def\n"))
        self.assertEqual(found[0][0], 3)
        self.assertIn(b"abc", found[0][2])
        self.assertIn(b"def", found[0][2])

    def test_a_lone_cr_is_left_to_the_line_endings_gate(self):
        # Separating case. 0x0D was the SECOND corrupted byte in handoff8.md,
        # but it is a line ending, and test_line_endings.py owns that question.
        # If this gate claimed it too, one defect would fail two gates and
        # neither would be the authority on it.
        self.assertEqual(stray_control_bytes(self._write(b"a\rb\n")), [])

    def test_tab_and_newline_do_not_trip_it(self):
        # The mutant that matters most in the opposite direction: a scanner
        # rejecting TAB would flag most of phase0-kext/ and be switched off.
        self.assertEqual(
            stray_control_bytes(self._write(b"if (x) {\n\treturn 1;\n}\n")), [])

    def test_a_nul_byte_is_detected(self):
        found = stray_control_bytes(self._write(b"text\x00more\n"))
        self.assertEqual([(f[0], f[1]) for f in found], [(4, 0x00)])

    def test_a_form_feed_is_detected(self):
        found = stray_control_bytes(self._write(b"page one\x0cpage two\n"))
        self.assertEqual([(f[0], f[1]) for f in found], [(8, 0x0C)])

    def test_an_escape_byte_is_detected(self):
        # 0x1B arrives when captured terminal output with ANSI colour codes is
        # pasted into a markdown log. This repo pastes gate output constantly.
        found = stray_control_bytes(self._write(b"ok \x1b[32mgreen\x1b[0m\n"))
        self.assertEqual([f[1] for f in found], [0x1B, 0x1B])

    def test_a_del_byte_is_detected(self):
        found = stray_control_bytes(self._write(b"text\x7fmore\n"))
        self.assertEqual([(f[0], f[1]) for f in found], [(4, 0x7F)])

    def test_every_disallowed_c0_byte_is_detected_not_just_the_sampled_ones(self):
        # Enumerating the whole range rather than spot-checking. The tests above
        # pick four bytes; a scanner with an off-by-one bound could pass all
        # four and miss 0x1F. Per `cross-checks-probe-the-far-side-bit`, sample
        # the boundary, not the middle.
        missed = []
        for value in range(0x00, 0x20):
            if value in ALLOWED_CONTROL:
                continue
            path = self._write(b"a" + bytes([value]) + b"b\n")
            found = stray_control_bytes(path)
            if [(f[0], f[1]) for f in found] != [(1, value)]:
                missed.append("0x%02X -> %r" % (value, found))
        self.assertEqual([], missed,
                         "disallowed control byte(s) not detected: %s"
                         % ", ".join(missed))

    def test_multiple_offenders_in_one_file_are_all_reported(self):
        # handoff8.md had two. A scanner that returned on the first find would
        # have reported the 0x08 and left the 0x0D in place, and the repair
        # would have looked complete.
        found = stray_control_bytes(self._write(b"a\x08b\x00c\x0cd\n"))
        self.assertEqual([f[1] for f in found], [0x08, 0x00, 0x0C])

    def test_an_empty_file_reports_nothing(self):
        self.assertEqual(stray_control_bytes(self._write(b"")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
