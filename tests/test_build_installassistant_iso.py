#!/usr/bin/env python3
"""
test_build_installassistant_iso.py -- regression tests for
``tools/build-installassistant-iso.py``.

Why this exists
---------------
The offline-install fallback is the escape hatch for a Recovery session with no
working egress. If the ISO it produces is subtly wrong, the failure surfaces
*inside the guest*, after a reboot, as a ``cp`` against a path that looks
correct in the log. Three things are load-bearing and all three are cheap to
check here:

* **The volume identifier must be exactly ``InstallAssistant``.** macOS mounts
  by volume identifier and ``run_offline.sh`` hardcodes
  ``/Volumes/InstallAssistant``. Any other string and the copy fails.
* **The emitted script must be retargeted off Ventura.** Upstream hardcodes
  ``Install macOS Ventura.app`` twice; this project's recovery is Sequoia
  15.4.1 (24E263). A Ventura path against a Sequoia recovery is a version
  mismatch that trades one blocker for another.
* **The Joliet tree must carry readable names.** The ISO9660 primary names are
  8.3 (``INSTALLA.PKG``); the guest needs ``InstallAssistant.pkg``.

The size guard is tested too. A real package is ~13-15 GiB, and discovering a
truncated download inside the guest costs a reboot.

Run with::

    tests\\run-python-tests.bat
    python -m unittest discover -s tests
"""

import importlib.util
import os
import shutil
import tempfile
import unittest

_TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                     "tools", "build-installassistant-iso.py")
_spec = importlib.util.spec_from_file_location("build_ia_iso", _TOOL)
build_ia_iso = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(build_ia_iso)


def _pycdlib_available():
    try:
        build_ia_iso.load_pycdlib()
    except RuntimeError:
        return False
    return True


class RenderScriptTests(unittest.TestCase):
    """The emitted script is what actually runs in the guest."""

    def test_ventura_is_gone(self):
        script = build_ia_iso.render_script("Install macOS Sequoia.app")
        self.assertNotIn("Ventura", script)

    def test_app_name_appears_twice(self):
        """Upstream copies the .app then cds into it -- both paths matter.
        Retargeting only one leaves a script that fails on the second line."""
        app = "Install macOS Sequoia.app"
        self.assertEqual(build_ia_iso.render_script(app).count(app), 2)

    def test_mounts_the_required_volume_path(self):
        script = build_ia_iso.render_script("Install macOS Sequoia.app")
        self.assertIn("/Volumes/InstallAssistant/InstallAssistant.pkg", script)

    def test_package_is_renamed_to_sharedsupport_dmg(self):
        """The mechanism is a rename, not an extraction. If this line drifts
        the springboard finds no payload and silently falls back to network."""
        script = build_ia_iso.render_script("Install macOS Sequoia.app")
        self.assertIn("Contents/SharedSupport/SharedSupport.dmg", script)

    def test_runs_the_springboard_directly(self):
        script = build_ia_iso.render_script("Install macOS Sequoia.app")
        self.assertIn("./Contents/MacOS/InstallAssistant_springboard", script)

    def test_a_different_app_name_is_honoured(self):
        script = build_ia_iso.render_script("Install macOS Tahoe.app")
        self.assertIn("Install macOS Tahoe.app", script)
        self.assertNotIn("Sequoia", script)


class VolumeIdentTests(unittest.TestCase):
    """A wrong identifier fails inside the guest, not here -- so check here."""

    def test_required_ident_is_the_string_run_offline_mounts(self):
        self.assertEqual(build_ia_iso.REQUIRED_VOL_IDENT, "InstallAssistant")

    def test_default_matches_required(self):
        args = build_ia_iso.build_parser().parse_args([])
        self.assertEqual(args.vol_ident, build_ia_iso.REQUIRED_VOL_IDENT)


class ArgumentTests(unittest.TestCase):

    def setUp(self):
        self.parser = build_ia_iso.build_parser()

    def test_pkg_and_out_are_optional_so_self_test_can_run_alone(self):
        args = self.parser.parse_args(["--self-test"])
        self.assertTrue(args.self_test)
        self.assertIsNone(args.pkg)

    def test_default_app_is_sequoia_not_ventura(self):
        self.assertEqual(self.parser.parse_args([]).app,
                         "Install macOS Sequoia.app")

    def test_flags_default_off(self):
        args = self.parser.parse_args([])
        self.assertFalse(args.no_script)
        self.assertFalse(args.allow_small_pkg)


class SizeGuardTests(unittest.TestCase):
    """A truncated download must be caught on the host, not in the guest."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="iasize-")
        self.pkg = os.path.join(self.tmp, "InstallAssistant.pkg")
        with open(self.pkg, "wb") as fp:
            fp.write(b"far too small to be real\n")
        self.out = os.path.join(self.tmp, "out.iso")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_small_package_is_rejected(self):
        rc = build_ia_iso.main([self.pkg, "-o", self.out])
        self.assertEqual(rc, 1)
        self.assertFalse(os.path.exists(self.out))

    def test_missing_package_is_rejected(self):
        rc = build_ia_iso.main([os.path.join(self.tmp, "nope.pkg"),
                                "-o", self.out])
        self.assertEqual(rc, 1)

    def test_out_without_pkg_is_rejected(self):
        self.assertEqual(build_ia_iso.main(["-o", self.out]), 1)

    def test_threshold_is_in_the_right_ballpark(self):
        """8 GiB: below any real payload, above any plausible placeholder."""
        self.assertEqual(build_ia_iso.PLAUSIBLE_PKG_BYTES,
                         8 * 1024 * 1024 * 1024)


@unittest.skipUnless(_pycdlib_available(), "pycdlib not installed")
class BuildAndReadBackTests(unittest.TestCase):
    """Building an image that cannot be read back is the failure to prevent."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="iabuild-")
        self.pkg = os.path.join(self.tmp, "InstallAssistant.pkg")
        with open(self.pkg, "wb") as fp:
            fp.write(b"placeholder payload\n")
        self.out = os.path.join(self.tmp, "InstallAssistant.iso")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_round_trip_carries_both_files_with_readable_names(self):
        build_ia_iso.build_iso(self.pkg, self.out)
        ident, entries = build_ia_iso.read_back(self.out)
        self.assertEqual(ident, "InstallAssistant")
        self.assertIn("InstallAssistant.pkg", entries)
        self.assertIn("run_offline.sh", entries)

    def test_read_back_omits_the_dot_entries(self):
        """'.' and '..' are directory records, not files. They appeared in the
        listing on first run and would read as content on the ISO."""
        build_ia_iso.build_iso(self.pkg, self.out)
        _, entries = build_ia_iso.read_back(self.out)
        self.assertNotIn(".", entries)
        self.assertNotIn("..", entries)
        self.assertEqual(len(entries), 2)

    def test_package_size_survives_the_round_trip(self):
        build_ia_iso.build_iso(self.pkg, self.out)
        _, entries = build_ia_iso.read_back(self.out)
        self.assertEqual(entries["InstallAssistant.pkg"],
                         os.path.getsize(self.pkg))

    def test_no_script_leaves_only_the_package(self):
        build_ia_iso.build_iso(self.pkg, self.out, emit_script=False)
        _, entries = build_ia_iso.read_back(self.out)
        self.assertEqual(list(entries), ["InstallAssistant.pkg"])

    def test_custom_vol_ident_is_written_verbatim(self):
        """The tool warns rather than refuses, so prove it does what it says."""
        build_ia_iso.build_iso(self.pkg, self.out, vol_ident="WRONGNAME")
        ident, _ = build_ia_iso.read_back(self.out)
        self.assertEqual(ident, "WRONGNAME")

    def test_verify_rejects_a_wrong_volume_ident(self):
        build_ia_iso.build_iso(self.pkg, self.out, vol_ident="WRONGNAME")
        self.assertEqual(build_ia_iso.main(["--verify", self.out]), 1)

    def test_verify_accepts_a_correct_image(self):
        build_ia_iso.build_iso(self.pkg, self.out)
        self.assertEqual(build_ia_iso.main(["--verify", self.out]), 0)

    def test_self_test_passes_and_cleans_up(self):
        before = set(os.listdir(tempfile.gettempdir()))
        self.assertEqual(build_ia_iso.self_test(), 0)
        leaked = [n for n in set(os.listdir(tempfile.gettempdir())) - before
                  if n.startswith("iatest-")]
        self.assertEqual(leaked, [])


if __name__ == "__main__":
    unittest.main()
