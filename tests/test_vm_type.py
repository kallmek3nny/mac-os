#!/usr/bin/env python3
"""
test_vm_type.py -- regression tests for the host-independent logic in
``tools/vm-type.py``.

Why this exists
---------------
This tool synthesises keystrokes into a window. Every one of its failure modes
is *silent*: a wrong scancode types the wrong character, a dropped shift types
the lowercase twin, a mis-parsed spec presses a different key -- and the
process still exits 0 with a cheerful "key : Q" on stdout. There is no guest
feedback channel to catch it (no VMware Tools, no SSH), so nothing downstream
will notice either.

The bug that motivated this file is exactly that shape. ``parse_key`` used to
lowercase the *whole* spec::

    parts = [p.strip().lower() for p in spec.split("+") if p.strip()]

which made the shifted-face branch a few lines below it unreachable for
letters. ``--key Q`` resolved to plain ``q``, reported ``key : Q (0
modifiers)``, and typed the wrong character into the guest. Caught
2026-08-07 by a helper-level check, not by anything the tool itself printed.

What is and is not covered
--------------------------
Covered: the pure functions between the command line and ``SendInput`` --
``parse_key``, ``char_to_scan``, the scancode tables' internal consistency,
``_ordered_actions`` (which decides the *order* keys are pressed in),
``guest_center``, ``pick_window`` (ambiguity must raise, never guess), and
``resolve_click_at`` (out-of-bounds must raise, never clamp). Those are where a
wrong answer is both possible and invisible.

``resolve_click_at`` was extracted from ``main()`` on 2026-08-07 for exactly
that reason: the bounds check was inline and coupled to a live window rect, so
no test could reach it. A guard nothing exercises is a guard nobody can trust.

Not covered: ``_Win32``, ``chord``, focus confirmation, and the actual
``SendInput`` call. Faking the Win32 layer would only test the fake, and the
one property that matters there -- that focus is re-read rather than assumed --
is a two-line check better verified by reading it than by mocking it.

Run with::

    tests\\run-python-tests.bat
    python -m unittest discover -s tests
"""

import importlib.util
import os
import unittest

_TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                     "tools", "vm-type.py")
_spec = importlib.util.spec_from_file_location("vm_type", _TOOL)
vm_type = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vm_type)


# Scancodes are spelled out rather than read back from the module's own tables:
# a test that asks the table what the table says would pass even if every entry
# were wrong. These are set 1 make codes from the US layout.
SC_Q = 0x10
SC_G = 0x22
SC_C = 0x2E
SC_A = 0x1E
SC_SLASH = 0x35
SC_SPACE = 0x39
SC_ENTER = 0x1C
SC_LSHIFT = 0x2A
SC_LCTRL = 0x1D
SC_LWIN = 0x5B


class ParseKeyCaseTests(unittest.TestCase):
    """The 2026-08-07 defect: an uppercase key face lost its shift.

    Each of these would have passed before the fix *except* the ones asserting
    a shift modifier -- which is the whole bug. Keep them together so a future
    ``.lower()`` on the whole spec fails loudly instead of silently.
    """

    def test_uppercase_letter_gains_a_shift(self):
        self.assertEqual(vm_type.parse_key("Q"), ([SC_LSHIFT], SC_Q))

    def test_lowercase_letter_has_no_modifier(self):
        self.assertEqual(vm_type.parse_key("q"), ([], SC_Q))

    def test_upper_and_lower_differ(self):
        """The defect made these two identical; that is the observable symptom."""
        self.assertNotEqual(vm_type.parse_key("Q"), vm_type.parse_key("q"))

    def test_shifted_punctuation_gains_a_shift(self):
        self.assertEqual(vm_type.parse_key("?"), ([SC_LSHIFT], SC_SLASH))

    def test_unshifted_punctuation_does_not(self):
        self.assertEqual(vm_type.parse_key("/"), ([], SC_SLASH))

    def test_modifier_names_stay_case_insensitive(self):
        """The fix must not make modifiers case-sensitive as a side effect."""
        self.assertEqual(vm_type.parse_key("CMD+Enter"), ([SC_LWIN], SC_ENTER))
        self.assertEqual(vm_type.parse_key("CtRl+c"), ([SC_LCTRL], SC_C))

    def test_named_keys_stay_case_insensitive(self):
        self.assertEqual(vm_type.parse_key("ENTER"), ([], SC_ENTER))
        self.assertEqual(vm_type.parse_key("Enter"), ([], SC_ENTER))

    def test_explicit_shift_is_not_duplicated_by_an_uppercase_face(self):
        """``shift+Q`` must press shift once. Pressing it twice leaves an
        unbalanced key-down in the guest when the chord is released."""
        mods, scan = vm_type.parse_key("shift+Q")
        self.assertEqual(scan, SC_Q)
        self.assertEqual(mods.count(SC_LSHIFT), 1)

    def test_explicit_shift_not_duplicated_with_another_modifier(self):
        mods, scan = vm_type.parse_key("cmd+shift+G")
        self.assertEqual(scan, SC_G)
        self.assertEqual(mods.count(SC_LSHIFT), 1)
        self.assertIn(SC_LWIN, mods)


class ParseKeyTests(unittest.TestCase):
    """General parsing -- a mis-parse presses a key nobody asked for."""

    def test_bare_named_key(self):
        self.assertEqual(vm_type.parse_key("enter"), ([], SC_ENTER))

    def test_single_modifier(self):
        self.assertEqual(vm_type.parse_key("cmd+space"), ([SC_LWIN], SC_SPACE))

    def test_two_modifiers_keep_their_order(self):
        self.assertEqual(vm_type.parse_key("cmd+shift+g"),
                         ([SC_LWIN, SC_LSHIFT], SC_G))

    def test_surrounding_whitespace_is_tolerated(self):
        self.assertEqual(vm_type.parse_key(" cmd + space "),
                         ([SC_LWIN], SC_SPACE))

    def test_unknown_modifier_is_rejected(self):
        with self.assertRaises(ValueError) as ctx:
            vm_type.parse_key("hyper+q")
        self.assertIn("hyper", str(ctx.exception))

    def test_unknown_multi_char_key_is_rejected(self):
        with self.assertRaises(ValueError):
            vm_type.parse_key("nosuchkey")

    def test_empty_spec_is_rejected(self):
        with self.assertRaises(ValueError):
            vm_type.parse_key("")

    def test_non_ascii_key_face_is_rejected_not_silently_dropped(self):
        """A US scancode keyboard cannot express these. Refusing is correct;
        typing an approximation into a Recovery terminal is not."""
        with self.assertRaises(ValueError):
            vm_type.parse_key("é")


class CharToScanTests(unittest.TestCase):
    """``--text`` runs every character through this; one wrong entry corrupts
    a typed path or password with no indication on stdout."""

    def test_lowercase_letter(self):
        self.assertEqual(vm_type.char_to_scan("a"), (SC_A, False))

    def test_uppercase_letter_needs_shift(self):
        self.assertEqual(vm_type.char_to_scan("A"), (SC_A, True))

    def test_unshifted_punctuation(self):
        self.assertEqual(vm_type.char_to_scan("/"), (SC_SLASH, False))

    def test_shifted_punctuation_shares_the_base_scancode(self):
        self.assertEqual(vm_type.char_to_scan("?"), (SC_SLASH, True))

    def test_space(self):
        self.assertEqual(vm_type.char_to_scan(" "), (SC_SPACE, False))

    def test_non_ascii_raises(self):
        with self.assertRaises(ValueError):
            vm_type.char_to_scan("é")

    def test_every_ascii_letter_and_digit_is_typable(self):
        """A gap here is only discovered when a command silently loses a
        character mid-word."""
        for ch in "abcdefghijklmnopqrstuvwxyz0123456789":
            with self.subTest(ch=ch):
                scan, shifted = vm_type.char_to_scan(ch)
                self.assertGreater(scan, 0)
                self.assertFalse(shifted)

    def test_every_uppercase_letter_is_shifted(self):
        for ch in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
            with self.subTest(ch=ch):
                scan, shifted = vm_type.char_to_scan(ch)
                self.assertTrue(shifted)
                self.assertEqual(scan, vm_type.char_to_scan(ch.lower())[0])


class ScancodeTableTests(unittest.TestCase):
    """Table-consistency checks. These catch a bad edit to the tables before it
    reaches a guest, where the only symptom is a wrong character."""

    def test_every_shifted_face_maps_to_a_known_base(self):
        dangling = [k for k, v in vm_type.SHIFTED.items()
                    if v not in vm_type.UNSHIFTED]
        self.assertEqual(dangling, [])

    def test_extended_codes_are_all_real_keys(self):
        """``EXTENDED`` decides which keys get the E0 prefix. An entry that
        matches no key is dead; worse, a *missing* entry sends an arrow as its
        numpad twin -- 'press Down' becomes 'type 2' under NumLock."""
        known = set(vm_type.NAMED.values()) | set(vm_type.MODIFIERS.values())
        self.assertEqual(sorted(set(vm_type.EXTENDED) - known), [])

    def test_arrow_keys_are_extended(self):
        for name in ("up", "down", "left", "right"):
            with self.subTest(name=name):
                self.assertIn(vm_type.NAMED[name], vm_type.EXTENDED)

    def test_no_scancode_collisions_among_unshifted_faces(self):
        """Two characters sharing a scancode means one of them is untypable."""
        seen = {}
        for ch, scan in vm_type.UNSHIFTED.items():
            self.assertNotIn(scan, seen,
                             "scancode 0x%02X claimed by both %r and %r"
                             % (scan, seen.get(scan), ch))
            seen[scan] = ch


class OrderedActionsTests(unittest.TestCase):
    """argparse collects ``--key`` and ``--text`` into two separate lists, which
    loses the interleaving. ``cmd+space``, then ``Terminal``, then ``enter`` is
    not the same as pressing both keys and then typing -- the second ordering
    types into whatever had focus before Spotlight opened."""

    def _parse(self, argv):
        args = vm_type.build_parser().parse_args(argv)
        return vm_type._ordered_actions(argv, args)

    def test_key_text_key_keeps_cli_order(self):
        argv = ["--key", "cmd+space", "--text", "Terminal", "--key", "enter"]
        self.assertEqual(self._parse(argv),
                         [("key", "cmd+space"), ("text", "Terminal"),
                          ("key", "enter")])

    def test_text_first_ordering_is_preserved(self):
        argv = ["--text", "one", "--key", "enter", "--text", "two"]
        self.assertEqual(self._parse(argv),
                         [("text", "one"), ("key", "enter"),
                          ("text", "two")])

    def test_repeated_texts_keep_their_relative_order(self):
        argv = ["--text", "one", "--text", "two"]
        self.assertEqual(self._parse(argv),
                         [("text", "one"), ("text", "two")])

    def test_unrelated_flags_do_not_appear_as_actions(self):
        argv = ["--window", "Sonoma", "--key", "enter", "--dry-run"]
        self.assertEqual(self._parse(argv), [("key", "enter")])

    def test_no_actions_is_empty_not_none(self):
        self.assertEqual(self._parse(["--window", "Sonoma"]), [])


class _Rect(object):
    def __init__(self, left, top, right, bottom):
        self.left, self.top = left, top
        self.right, self.bottom = right, bottom


class GuestCenterTests(unittest.TestCase):
    """A click aimed at the window centre lands in VMware's chrome on a short
    window. The centre must be computed over the *guest* box, the same region
    vm-screenshot.py crops to."""

    def test_center_excludes_toolbar_and_status_bar(self):
        self.assertEqual(vm_type.guest_center(_Rect(100, 200, 1100, 1000)),
                         (600, (200 + 60 + 1000 - 20) // 2))

    def test_center_is_inside_the_window(self):
        r = _Rect(0, 0, 1228, 933)
        x, y = vm_type.guest_center(r)
        self.assertTrue(r.left < x < r.right)
        self.assertTrue(r.top < y < r.bottom)

    def test_center_is_below_the_toolbar(self):
        """y must clear the 60px chrome strip, or the click hits a menu."""
        r = _Rect(0, 0, 1228, 933)
        self.assertGreater(vm_type.guest_center(r)[1], 60)


class PickWindowTests(unittest.TestCase):
    """Ambiguity must be an error, never a guess.

    ``vm-screenshot.py`` refuses to *capture* the wrong VM. Typing into the
    wrong VM is strictly worse -- a shell command aimed at a Recovery terminal
    that lands in a different guest is unrecoverable -- so a pattern matching
    two windows has to fail rather than pick the first hit.
    """

    WINDOWS = [
        (0x1001, "Sonoma - VMware Workstation"),
        (0x1002, "Ventura - VMware Workstation"),
        (0x1003, "Notepad"),
    ]

    def test_unique_substring_matches_one_window(self):
        hwnd, title = vm_type.pick_window(self.WINDOWS, "Sonoma")
        self.assertEqual(hwnd, 0x1001)
        self.assertEqual(title, "Sonoma - VMware Workstation")

    def test_match_is_case_insensitive(self):
        self.assertEqual(vm_type.pick_window(self.WINDOWS, "sonoma")[0], 0x1001)
        self.assertEqual(vm_type.pick_window(self.WINDOWS, "SONOMA")[0], 0x1001)

    def test_two_matches_raise_rather_than_guess(self):
        """'VMware' matches both VMs. Picking either one is a coin flip with an
        unrecoverable downside, so this must raise."""
        with self.assertRaises(RuntimeError) as ctx:
            vm_type.pick_window(self.WINDOWS, "VMware")
        msg = str(ctx.exception)
        self.assertIn("Sonoma - VMware Workstation", msg)
        self.assertIn("Ventura - VMware Workstation", msg)

    def test_no_match_raises(self):
        with self.assertRaises(RuntimeError):
            vm_type.pick_window(self.WINDOWS, "NoSuchVM_zzz")

    def test_empty_window_list_raises(self):
        with self.assertRaises(RuntimeError):
            vm_type.pick_window([], "Sonoma")

    def test_pattern_is_a_literal_not_a_regex(self):
        """The pattern is escaped before compiling. If it were not, a title
        containing '.' or '(' would match things the operator never typed --
        and '.' alone would match every window and read as ambiguity."""
        with self.assertRaises(RuntimeError):
            vm_type.pick_window(self.WINDOWS, "S.noma")


class ResolveClickAtTests(unittest.TestCase):
    """``--click-at`` is guest-relative: (0,0) is the top-left of the guest
    framebuffer, not of the VMware window.

    Out-of-range must raise, not clamp. A clamped click still lands somewhere,
    and "somewhere" here means VMware's own toolbar or another application --
    the same silent-wrong-action class the ambiguous-window guard prevents.
    Refusing costs one rerun; clicking host chrome costs a wrong diagnosis.
    """

    # A 1000x800 guest surface whose top-left sits at screen (100, 200).
    # Distinct origin and size on purpose: if the offset were dropped, an
    # origin of (0,0) would make the bug invisible.
    RECT = _Rect(100, 200, 1100, 1000)

    def test_origin_maps_to_the_guest_top_left(self):
        self.assertEqual(vm_type.resolve_click_at("0,0", self.RECT), (100, 200))

    def test_offset_is_added_to_the_rect_origin(self):
        self.assertEqual(vm_type.resolve_click_at("10,20", self.RECT),
                         (110, 220))

    def test_last_in_bounds_pixel_is_accepted(self):
        self.assertEqual(vm_type.resolve_click_at("999,799", self.RECT),
                         (1099, 999))

    def test_whitespace_around_the_fields_is_tolerated(self):
        self.assertEqual(vm_type.resolve_click_at(" 10 , 20 ", self.RECT),
                         (110, 220))

    def test_x_equal_to_width_is_rejected(self):
        """Bounds are half-open. Width 1000 means 999 is the last valid x."""
        with self.assertRaises(ValueError):
            vm_type.resolve_click_at("1000,0", self.RECT)

    def test_y_equal_to_height_is_rejected(self):
        with self.assertRaises(ValueError):
            vm_type.resolve_click_at("0,800", self.RECT)

    def test_negative_coordinates_are_rejected(self):
        for spec in ("-1,0", "0,-1"):
            with self.subTest(spec=spec):
                with self.assertRaises(ValueError):
                    vm_type.resolve_click_at(spec, self.RECT)

    def test_out_of_bounds_message_names_the_surface_size(self):
        """The operator needs to know what the valid range *was*; a bare
        'out of range' sends them guessing."""
        with self.assertRaises(ValueError) as ctx:
            vm_type.resolve_click_at("5000,5000", self.RECT)
        self.assertIn("1000x800", str(ctx.exception))

    def test_wrong_field_count_is_rejected(self):
        for spec in ("", "50", "1,2,3"):
            with self.subTest(spec=spec):
                with self.assertRaises(ValueError):
                    vm_type.resolve_click_at(spec, self.RECT)

    def test_non_integer_fields_are_rejected(self):
        """'1.5' must not be truncated to 1 -- a silently moved click is the
        failure mode this whole tool is defensive about."""
        for spec in ("a,b", "1.5,2", "0x10,0"):
            with self.subTest(spec=spec):
                with self.assertRaises(ValueError):
                    vm_type.resolve_click_at(spec, self.RECT)

    def test_result_is_inside_the_rect_for_every_in_bounds_input(self):
        for gx, gy in ((0, 0), (1, 1), (500, 400), (999, 799)):
            with self.subTest(gx=gx, gy=gy):
                x, y = vm_type.resolve_click_at("%d,%d" % (gx, gy), self.RECT)
                self.assertTrue(self.RECT.left <= x < self.RECT.right)
                self.assertTrue(self.RECT.top <= y < self.RECT.bottom)


class ArgumentTests(unittest.TestCase):
    """``--dry-run`` is the safety valve; a regression that makes it default to
    off would inject keystrokes during what the operator thinks is a rehearsal."""

    def setUp(self):
        self.parser = vm_type.build_parser()

    def test_dry_run_defaults_off_and_is_settable(self):
        self.assertFalse(self.parser.parse_args([]).dry_run)
        self.assertTrue(self.parser.parse_args(["--dry-run"]).dry_run)

    def test_key_is_repeatable_and_accumulates(self):
        args = self.parser.parse_args(["--key", "enter", "--key", "esc"])
        self.assertEqual(args.key, ["enter", "esc"])

    def test_text_is_repeatable_and_accumulates(self):
        args = self.parser.parse_args(["--text", "a", "--text", "b"])
        self.assertEqual(args.text, ["a", "b"])

    def test_key_and_text_default_to_empty_not_none(self):
        args = self.parser.parse_args([])
        self.assertEqual(args.key, [])
        self.assertEqual(args.text, [])

    def test_default_window_is_the_vmware_title(self):
        self.assertEqual(self.parser.parse_args([]).window,
                         "VMware Workstation")


if __name__ == "__main__":
    unittest.main()
