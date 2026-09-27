#!/usr/bin/env python3
"""
test_vm_screenshot.py -- regression tests for the host-independent logic in
``tools/vm-screenshot.py``.

Why this exists
---------------
The tool exists because a screenshot lied: five consecutive captures on
2026-08-07 contained the *host desktop* rather than the guest, and nobody
noticed until OCR read back the operator's own shell prompt. The Win32 capture
path cannot be tested without a running VM, but the parts that turned a bad
capture into a wrong conclusion can be:

* ``check_expected`` -- if this matched too loosely, a FAIL would read as PASS
  and the tool would repeat the exact mistake it was written to prevent.
* ``crop_chrome`` -- if this trimmed too little, VMware's own toolbar text
  reaches OCR and produces convincing nonsense about the guest.
* argument parsing -- ``--expect`` is repeatable and drives the exit status,
  so a regression there changes a pass/fail gate silently.

Those are pure and are what these tests cover. ``_Win32`` construction is not
exercised: it is a thin wrapper over six documented API calls, and faking them
would only test the fake. ``pick_window`` is covered against a stub roster,
which is the part with real branching (no match / ambiguous / exactly one).

Run with::

    tests\\run-python-tests.bat
    python -m unittest discover -s tests
"""

import importlib.util
import os
import unittest

_TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                     "tools", "vm-screenshot.py")
_spec = importlib.util.spec_from_file_location("vm_screenshot", _TOOL)
vm_screenshot = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vm_screenshot)


class _FakeWin32(object):
    """Stands in for ``_Win32`` so ``pick_window`` can be driven off-Windows."""

    def __init__(self, windows):
        self._windows = windows

    def find_windows(self):
        return list(self._windows)


RECOVERY_OCR = """
Recovery File Edit Utilities Window
Restore from Time Machine
If you have backup of your system that you want to restore
Reinstall macOS Sequoia
Install a new copy of macOS Sequoia onto your Mac
Safari
Disk Utility
Continue
"""


class CheckExpectedTests(unittest.TestCase):
    """``check_expected`` is the pass/fail gate -- loose matching is the risk."""

    def test_all_phrases_present(self):
        found, missing = vm_screenshot.check_expected(
            RECOVERY_OCR, ["Reinstall macOS", "Disk Utility", "Time Machine"])
        self.assertEqual(missing, [])
        self.assertEqual(len(found), 3)

    def test_absent_phrase_is_reported_missing(self):
        found, missing = vm_screenshot.check_expected(
            RECOVERY_OCR, ["Disk Utility", "ThisIsNotOnScreen12345"])
        self.assertEqual(found, ["Disk Utility"])
        self.assertEqual(missing, ["ThisIsNotOnScreen12345"])

    def test_matching_is_case_insensitive(self):
        _, missing = vm_screenshot.check_expected(RECOVERY_OCR,
                                                  ["reINSTALL MACos"])
        self.assertEqual(missing, [])

    def test_line_wrapped_phrase_still_matches(self):
        """OCR breaks lines unpredictably; a phrase split across two lines
        must still match, or every multi-word --expect becomes a false FAIL."""
        text = "Reinstall\nmacOS Sequoia"
        _, missing = vm_screenshot.check_expected(text,
                                                  ["Reinstall macOS Sequoia"])
        self.assertEqual(missing, [])

    def test_runs_of_whitespace_collapse_on_both_sides(self):
        text = "Disk     Utility"
        _, missing = vm_screenshot.check_expected(text, ["Disk   Utility"])
        self.assertEqual(missing, [])

    def test_empty_ocr_fails_every_phrase(self):
        """A black screen must never read as PASS -- that is the whole point."""
        found, missing = vm_screenshot.check_expected("", ["Disk Utility"])
        self.assertEqual(found, [])
        self.assertEqual(missing, ["Disk Utility"])

    def test_no_phrases_means_nothing_missing(self):
        found, missing = vm_screenshot.check_expected(RECOVERY_OCR, [])
        self.assertEqual((found, missing), ([], []))


class CropChromeTests(unittest.TestCase):
    """Under-cropping feeds VMware's own menu text to OCR."""

    def setUp(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow not installed")
        self.Image = Image

    def test_default_crop_removes_toolbar_and_status_bar(self):
        img = self.Image.new("RGB", (1228, 933))
        out = vm_screenshot.crop_chrome(img)
        # 60 off the top, 20 off the bottom, 2 each side.
        self.assertEqual(out.size, (1224, 853))

    def test_crop_keeps_the_guest_pixels_not_the_chrome(self):
        """Paint chrome and guest distinct colours and prove which survives."""
        img = self.Image.new("RGB", (200, 200), (255, 0, 0))       # chrome red
        for y in range(60, 180):
            for x in range(2, 198):
                img.putpixel((x, y), (0, 255, 0))                  # guest green
        out = vm_screenshot.crop_chrome(img)
        colours = {c for _, c in out.getcolors(maxcolors=16)}
        self.assertIn((0, 255, 0), colours)
        self.assertNotIn((255, 0, 0), colours)

    def test_tiny_window_does_not_produce_an_inverted_box(self):
        """A window smaller than the chrome margins must not crash Pillow with
        a negative-size crop; it should degrade to a 1px sliver."""
        img = self.Image.new("RGB", (10, 10))
        out = vm_screenshot.crop_chrome(img)
        self.assertGreaterEqual(out.size[0], 1)
        self.assertGreaterEqual(out.size[1], 1)


class FrameDeltaTests(unittest.TestCase):
    """The bug this guards: on 2026-08-07 a whole-window diff of a *hung* guest
    reported 0.055% changed, and every one of those pixels was VMware's own
    title bar at y=7. The number looked like proof of life. Restricting the
    diff to the guest box is what makes the verdict honest."""

    def setUp(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow not installed")
        self.Image = Image

    def _frames(self):
        a = self.Image.new("RGB", (200, 200), (0, 0, 0))
        b = a.copy()
        return a, b

    def test_identical_frames_are_zero(self):
        a, b = self._frames()
        self.assertEqual(vm_screenshot.frame_delta(a, b), 0.0)

    def test_resized_window_returns_none(self):
        a, _ = self._frames()
        b = self.Image.new("RGB", (200, 201))
        self.assertIsNone(vm_screenshot.frame_delta(a, b))

    def test_chrome_only_change_is_invisible_to_the_guest_region(self):
        a, b = self._frames()
        for x in range(37, 200):          # the real 2026-08-07 strip, y=7..15
            for y in range(7, 16):
                b.putpixel((x, y), (255, 255, 255))
        guest_box = (2, 60, 198, 180)
        self.assertEqual(vm_screenshot.frame_delta(a, b, guest_box), 0.0)
        self.assertGreater(vm_screenshot.frame_delta(a, b), 0.0)

    def test_guest_change_is_reported(self):
        a, b = self._frames()
        for x in range(10, 190):
            for y in range(70, 170):
                b.putpixel((x, y), (255, 255, 255))
        guest_box = (2, 60, 198, 180)
        self.assertGreater(vm_screenshot.frame_delta(a, b, guest_box), 1.0)


class _FakeCaptureWin32(object):
    """Drives ``capture`` off-Windows: minimized state, rect, and pixel bytes."""

    def __init__(self, size, minimized=False, restores=True):
        self._size = size
        self._minimized = minimized
        self._restores = restores
        self.restore_calls = 0
        self.print_calls = 0

    def is_minimized(self, hwnd):
        return self._minimized

    def restore(self, hwnd):
        self.restore_calls += 1
        if self._restores:
            self._minimized = False

    def window_rect(self, hwnd):
        return self._size

    def print_window(self, hwnd, width, height):
        self.print_calls += 1
        return b"\x00\x00\x00\xff" * (width * height)


class MinimizedWindowTests(unittest.TestCase):
    """The bug this guards: ``PrintWindow`` does **not** fail on a minimized
    window. On 2026-08-07 it returned a 160x28 taskbar thumbnail, the tool
    reported a successful capture, and OCR returned zero lines -- a result that
    says nothing about the guest but reads like it does."""

    def setUp(self):
        try:
            from PIL import Image                                  # noqa: F401
        except ImportError:
            self.skipTest("Pillow not installed")

    def test_minimized_window_is_restored_then_captured(self):
        w = _FakeCaptureWin32((1228, 933), minimized=True)
        img = vm_screenshot.capture(w, 1)
        self.assertEqual(w.restore_calls, 1)
        self.assertEqual(img.size, (1228, 933))

    def test_no_restore_refuses_a_minimized_window(self):
        w = _FakeCaptureWin32((1228, 933), minimized=True)
        with self.assertRaises(RuntimeError) as ctx:
            vm_screenshot.capture(w, 1, restore=False)
        self.assertIn("minimized", str(ctx.exception))
        self.assertEqual(w.print_calls, 0)

    def test_window_that_stays_minimized_raises(self):
        w = _FakeCaptureWin32((1228, 933), minimized=True, restores=False)
        with self.assertRaises(RuntimeError) as ctx:
            vm_screenshot.capture(w, 1)
        self.assertIn("stayed minimized", str(ctx.exception))
        self.assertEqual(w.print_calls, 0)

    def test_thumbnail_sized_window_is_refused(self):
        """The literal 160x28 frame from 2026-08-07 must not come back as a
        successful capture, even if IsIconic somehow reports false."""
        w = _FakeCaptureWin32((160, 28))
        with self.assertRaises(RuntimeError) as ctx:
            vm_screenshot.capture(w, 1)
        msg = str(ctx.exception)
        self.assertIn("160x28", msg)
        self.assertIn("Refusing", msg)
        self.assertEqual(w.print_calls, 0)

    def test_zero_sized_window_is_refused(self):
        w = _FakeCaptureWin32((0, 0))
        with self.assertRaises(RuntimeError):
            vm_screenshot.capture(w, 1)

    def test_normal_window_is_not_restored(self):
        w = _FakeCaptureWin32((1228, 933))
        img = vm_screenshot.capture(w, 1)
        self.assertEqual(w.restore_calls, 0)
        self.assertEqual(w.print_calls, 1)
        self.assertEqual(img.size, (1228, 933))


class PickWindowTests(unittest.TestCase):
    """Capturing the wrong VM silently is the failure mode being prevented."""

    def test_single_match_is_returned(self):
        w = _FakeWin32([(1, "Sonoma - VMware Workstation"),
                        (2, "Notepad")])
        hwnd, title = vm_screenshot.pick_window(w, "Sonoma")
        self.assertEqual((hwnd, title), (1, "Sonoma - VMware Workstation"))

    def test_match_is_case_insensitive(self):
        w = _FakeWin32([(1, "Sonoma - VMware Workstation")])
        hwnd, _ = vm_screenshot.pick_window(w, "sonoma - vmware")
        self.assertEqual(hwnd, 1)

    def test_ambiguous_match_raises_rather_than_guessing(self):
        w = _FakeWin32([(1, "Sonoma - VMware Workstation"),
                        (2, "Ventura - VMware Workstation")])
        with self.assertRaises(RuntimeError) as ctx:
            vm_screenshot.pick_window(w, "VMware Workstation")
        msg = str(ctx.exception)
        self.assertIn("2 windows match", msg)
        self.assertIn("Sonoma", msg)
        self.assertIn("Ventura", msg)

    def test_no_match_lists_the_vmware_windows_that_do_exist(self):
        w = _FakeWin32([(1, "Sonoma - VMware Workstation")])
        with self.assertRaises(RuntimeError) as ctx:
            vm_screenshot.pick_window(w, "NoSuchWindowXYZ")
        msg = str(ctx.exception)
        self.assertIn("no window title matches", msg)
        self.assertIn("Sonoma - VMware Workstation", msg)

    def test_no_vmware_windows_at_all_says_so(self):
        w = _FakeWin32([(1, "Notepad")])
        with self.assertRaises(RuntimeError) as ctx:
            vm_screenshot.pick_window(w, "Sonoma")
        self.assertIn("No VMware windows are open", str(ctx.exception))

    def test_pattern_is_escaped_not_treated_as_regex(self):
        """A title containing regex metacharacters must match literally."""
        w = _FakeWin32([(1, "macOS (test) - VMware Workstation")])
        hwnd, _ = vm_screenshot.pick_window(w, "macOS (test)")
        self.assertEqual(hwnd, 1)


class DerivedFramePathTests(unittest.TestCase):
    """The bug this guards: ``--compare REF.png`` used to write its second frame
    straight over ``REF.png``. On 2026-08-07 that destroyed eight supposed
    before/after baselines -- md5sum collapsed them to two hashes, and every
    "baseline" mtime was later than its own "after" frame. The invented idle
    floor of 0.000% came from those files. Both frames must now land on derived
    paths and the named file must never be written."""

    def test_tag_is_inserted_before_the_extension(self):
        self.assertEqual(vm_screenshot.derived_frame_path("ref.png", "frame1"),
                         "ref.frame1.png")
        self.assertEqual(vm_screenshot.derived_frame_path("ref.png", "frame2"),
                         "ref.frame2.png")

    def test_neither_frame_path_equals_the_named_file(self):
        """The whole defect in one assertion."""
        base = "baseline.png"
        for tag in ("frame1", "frame2"):
            self.assertNotEqual(vm_screenshot.derived_frame_path(base, tag),
                                base)

    def test_the_two_frames_do_not_collide_with_each_other(self):
        self.assertNotEqual(vm_screenshot.derived_frame_path("r.png", "frame1"),
                            vm_screenshot.derived_frame_path("r.png", "frame2"))

    def test_missing_extension_gains_png(self):
        self.assertEqual(vm_screenshot.derived_frame_path("ref", "frame2"),
                         "ref.frame2.png")

    def test_dots_in_directory_names_are_not_mistaken_for_an_extension(self):
        out = vm_screenshot.derived_frame_path("a/b.c/ref.png", "frame1")
        self.assertEqual(out, "a/b.c/ref.frame1.png")

    def test_extension_case_is_preserved(self):
        self.assertEqual(
            vm_screenshot.derived_frame_path("C:/tmp/shot.PNG", "frame1"),
            "C:/tmp/shot.frame1.PNG")

    def test_compare_source_never_saves_to_the_named_argument(self):
        """Read the implementation, not just the helper: the historical line was
        ``second.save(args.compare)``. If it ever comes back, this fails."""
        with open(_TOOL, "r", encoding="utf-8") as fh:
            src = fh.read()
        self.assertNotIn("second.save(args.compare)", src)
        self.assertNotIn("shot.save(args.compare)", src)
        self.assertIn("derived_frame_path(args.compare", src)


class ArgumentTests(unittest.TestCase):
    """``--expect`` drives the exit status, so its parsing is load-bearing."""

    def setUp(self):
        self.parser = vm_screenshot.build_parser()

    def test_expect_is_repeatable_and_accumulates(self):
        args = self.parser.parse_args(["--expect", "Disk Utility",
                                       "--expect", "Safari"])
        self.assertEqual(args.expect, ["Disk Utility", "Safari"])

    def test_expect_defaults_to_empty_not_none(self):
        self.assertEqual(self.parser.parse_args([]).expect, [])

    def test_default_window_is_the_vmware_title(self):
        self.assertEqual(self.parser.parse_args([]).window,
                         "VMware Workstation")

    def test_settle_is_numeric(self):
        self.assertEqual(self.parser.parse_args(["--settle", "7.5"]).settle,
                         7.5)

    def test_no_restore_defaults_off_and_is_settable(self):
        self.assertFalse(self.parser.parse_args([]).no_restore)
        self.assertTrue(self.parser.parse_args(["--no-restore"]).no_restore)


if __name__ == "__main__":
    unittest.main()
