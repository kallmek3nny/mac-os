#!/usr/bin/env python3
"""
test_line_endings.py -- every .cmd/.bat in this repo must use CRLF.

Why this exists
---------------
`cmd.exe` reads a batch file by BYTE OFFSET, not line by line. After a `call`
or a block, it seeks back to a remembered offset. When the file is saved with
bare-LF line endings, those offsets no longer land where the parser thinks they
do, so blocks execute out of order or partially -- and the script does not
crash. It produces confidently-formatted wrong output.

This is not hypothetical. Measured 2026-08-08:

  tools/_keyresolve-harness.cmd   LF=134  CRLF=0
  tools/_toolsguard-harness.cmd   LF=135  CRLF=0

The keyresolve harness printed its `=== CASE 4 ===` header and one `[FAIL]`
*after* its own run summary, and CASE 3 printed an empty `composed:` string.
Read naively, that is a defect in the public-key resolution block of
`tools/guest-enable-ssh-via-tools.cmd`: the key vanishing, the quoting
breaking, the wrong line being selected. It was none of those things. It was
the harness tearing itself apart.

Normalizing both files to CRLF -- same text, no assertion touched, 4959 -> 5093
and 4511 -> 4646 bytes -- turned exit 1 with FAILs into exit 0 with all five
cases `[ok]` in both. The pre-fix artifact `shots/keyresolve-harness.out.txt`
(md5 a3a491c93e2283e9831e45a16efd51e3) is retracted for that reason; see
`shots/keyresolve-harness.out.RETRACTED.md`.

The tell, and why a human check is not enough
---------------------------------------------
The signature is output whose sections appear out of order -- a case header
after the summary. That is easy to miss in a long log and impossible to see at
all by reading the batch source, because the source is correct. Nothing about
the text of an LF-only .cmd looks wrong. Only the bytes do. So the check has to
be mechanical, and it has to run in the gate that always works on a bare
checkout (`tests\\run-python-tests.bat`, which needs only CPython).

What this test does NOT claim
-----------------------------
Passing means no batch file in the repo has bare-LF endings. It does not mean
those batch files are correct, and it does not mean a CRLF-clean .cmd behaves
the same on every Windows build. It removes exactly one failure mode: the one
where the interpreter, not the assertions, produced the FAILs.

Run:
    python tests/test_line_endings.py
    tests\\run-python-tests.bat        # picked up automatically by discovery
"""

import os
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    os.pardir))

# Batch interpreters are the only thing here that is offset-sensitive. Python,
# Markdown and C++ do not care, and forcing CRLF on them would create churn for
# no measured benefit -- so this gate stays narrow on purpose.
BATCH_SUFFIXES = (".cmd", ".bat")

# Third-party trees are not ours to normalize. `reference/` holds vendored
# NVIDIA source; rewriting its line endings would corrupt a comparison baseline
# that other tools diff against.
SKIP_DIRS = {".git", ".claude-crew", "reference", "build", "__pycache__",
             "shots", "node_modules"}


def find_batch_files():
    """Every repo-owned .cmd/.bat, as (relative_path, absolute_path) pairs."""
    found = []
    for dirpath, dirnames, filenames in os.walk(REPO):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name.lower().endswith(BATCH_SUFFIXES):
                absolute = os.path.join(dirpath, name)
                found.append((os.path.relpath(absolute, REPO), absolute))
    return sorted(found)


def ending_counts(path):
    """(total_lf, crlf, bare_lf, bare_cr) counted on the raw bytes.

    Counted on bytes, never on text: opening in text mode with universal
    newlines translates the exact thing being measured out of existence.
    """
    with open(path, "rb") as fh:
        blob = fh.read()
    lf = blob.count(b"\n")
    crlf = blob.count(b"\r\n")
    cr = blob.count(b"\r")
    return lf, crlf, lf - crlf, cr - crlf


class BatchFilesUseCRLF(unittest.TestCase):

    def test_at_least_one_batch_file_is_discovered(self):
        # A walk that silently finds nothing would make every other assertion
        # here vacuously true -- a green gate that checks nothing. That is the
        # same class of failure as a 0.000%% delta computed from one frame
        # counted twice: arithmetic on an empty set, reported as a pass.
        self.assertTrue(find_batch_files(),
                        "no .cmd/.bat files found under %s -- the walk or "
                        "SKIP_DIRS is wrong, and this suite is checking "
                        "nothing" % REPO)

    def test_no_batch_file_has_bare_lf_endings(self):
        offenders = []
        for relative, absolute in find_batch_files():
            _lf, crlf, bare_lf, _bare_cr = ending_counts(absolute)
            if bare_lf:
                offenders.append("%s (bare LF=%d, CRLF=%d)"
                                 % (relative, bare_lf, crlf))
        self.assertEqual(
            [], offenders,
            "batch file(s) saved with bare-LF endings:\n  %s\n"
            "cmd.exe seeks these by byte offset and will run blocks OUT OF "
            "ORDER without crashing. Any FAIL such a script prints describes "
            "the script itself, not the code under test. Fix by rewriting the "
            "bytes:\n"
            "  b = open(p,'rb').read()\n"
            "  n = b.replace(b'\\r\\n', b'\\n').replace(b'\\n', b'\\r\\n')\n"
            "  open(p,'wb').write(n if n.endswith(b'\\r\\n') else n + b'\\r\\n')"
            % "\n  ".join(offenders))

    def test_no_batch_file_has_a_bare_cr(self):
        # A lone CR (classic-Mac ending, or a botched half-normalization) is
        # worse than bare LF: cmd.exe treats the whole file as one line, so the
        # failure is loud rather than subtle. Cheap to pin while we are here.
        offenders = []
        for relative, absolute in find_batch_files():
            _lf, _crlf, _bare_lf, bare_cr = ending_counts(absolute)
            if bare_cr:
                offenders.append("%s (bare CR=%d)" % (relative, bare_cr))
        self.assertEqual([], offenders,
                         "batch file(s) containing a CR not followed by LF:"
                         "\n  %s" % "\n  ".join(offenders))

    def test_every_batch_file_ends_with_a_newline(self):
        # A final command with no terminating newline is read inconsistently
        # across Windows builds -- sometimes executed, sometimes dropped. Every
        # batch file in tools/ ends in `exit /b`, and silently skipping that
        # line turns a nonzero exit into a zero one.
        offenders = []
        for relative, absolute in find_batch_files():
            with open(absolute, "rb") as fh:
                blob = fh.read()
            if blob and not blob.endswith(b"\r\n"):
                offenders.append("%s (last bytes %r)" % (relative, blob[-4:]))
        self.assertEqual([], offenders,
                         "batch file(s) not terminated by CRLF:\n  %s"
                         % "\n  ".join(offenders))


class EndingCountsItself(unittest.TestCase):
    """The measurement must be right, or the gate above is decoration."""

    def _write(self, blob):
        import tempfile
        fd, path = tempfile.mkstemp(suffix=".cmd")
        with os.fdopen(fd, "wb") as fh:
            fh.write(blob)
        self.addCleanup(os.remove, path)
        return path

    def test_pure_crlf_reports_no_bare_endings(self):
        lf, crlf, bare_lf, bare_cr = ending_counts(
            self._write(b"@echo off\r\necho hi\r\n"))
        self.assertEqual((lf, crlf, bare_lf, bare_cr), (2, 2, 0, 0))

    def test_pure_lf_is_detected(self):
        lf, crlf, bare_lf, bare_cr = ending_counts(
            self._write(b"@echo off\necho hi\n"))
        self.assertEqual((lf, crlf, bare_lf, bare_cr), (2, 0, 2, 0))

    def test_mixed_endings_are_detected(self):
        # The nastiest real case: mostly correct, one stray LF. A CRLF count
        # alone would look healthy here, which is why bare_lf is the assertion
        # the gate uses rather than "crlf > 0".
        lf, crlf, bare_lf, bare_cr = ending_counts(
            self._write(b"@echo off\r\necho hi\r\ncall :x\nexit /b\r\n"))
        self.assertEqual(crlf, 3)
        self.assertEqual(bare_lf, 1)

    def test_bare_cr_is_detected(self):
        lf, crlf, bare_lf, bare_cr = ending_counts(
            self._write(b"@echo off\recho hi\r"))
        self.assertEqual((lf, crlf, bare_lf, bare_cr), (0, 0, 0, 2))

    def test_empty_file_is_not_reported_as_an_offender(self):
        self.assertEqual(ending_counts(self._write(b"")), (0, 0, 0, 0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
