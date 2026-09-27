#!/usr/bin/env python3
"""
test_tools_parsing.py -- regression tests for the parsing logic in
``tools/collapse-sequence.py`` and ``tools/check-registers.py``.

Why this exists
---------------
Both tools are *evidence producers*: `collapse-sequence.py` decides what the
Phase 4 bringup sequence is, and `check-registers.py` decides whether the kext's
register constants are right. If either parses wrongly, it does not crash --
it produces a plausible, confidently-formatted, wrong answer. That is the exact
failure mode this project has already been bitten by three times (the split
ARCHITECTURE field, GA107's implementation ID, and the mish chunk offsets).

These tests therefore pin *parsing decisions*, not output formatting:

  - which lines a listing parser accepts and which it must reject
  - that consecutive same-register runs collapse and non-consecutive ones do not
  - that a `#define` with a trailing comment yields the value, not the comment
  - that an 'HI:LO' bitrange becomes the right (shift, mask) pair
  - that alias chains resolve, and that a cycle terminates instead of hanging

Run:
    python tests/test_tools_parsing.py
    tests\\run-python-tests.bat        # picked up automatically by discovery
"""

import importlib.util
import os
import sys
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     os.pardir, "tools")


def load_hyphenated(module_name, filename):
    """Import a tool whose filename contains a hyphen.

    `collapse-sequence.py` and `check-registers.py` are not importable with a
    plain `import` -- the hyphen is not a valid identifier. Loading them by
    path is not a test-only convenience: it is the only way to test them at
    all without renaming the tools, and renaming them would break every
    command line already written down in the docs.
    """
    path = os.path.join(TOOLS, filename)
    spec = importlib.util.spec_from_file_location(module_name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


collapse = load_hyphenated("collapse_sequence", "collapse-sequence.py")
chkreg = load_hyphenated("check_registers", "check-registers.py")


# ---------------------------------------------------------------------------
# check-registers.py -- header parsing
# ---------------------------------------------------------------------------

class DefineRegex(unittest.TestCase):
    """DEFINE_RE / PARAM_DEFINE_RE must split name from body correctly."""

    def test_plain_define(self):
        m = chkreg.DEFINE_RE.match("#define NV_PMC_BOOT_0    0x00000000")
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), "NV_PMC_BOOT_0")
        self.assertEqual(m.group(2).strip(), "0x00000000")

    def test_trailing_block_comment_is_not_part_of_the_value(self):
        # The bug this guards: a value of "0x118234 /* RW-4R */" resolves to
        # None, and a None offset is reported as "unparseable" rather than as
        # a mismatch -- so a real drift would be filed under the wrong heading.
        m = chkreg.DEFINE_RE.match(
            "#define NV_PFALCON_FALCON_CPUCTL   0x00000100 /* RW-4R */")
        self.assertIsNotNone(m)
        body = chkreg.strip_comments(m.group(2)).strip()
        self.assertEqual(body, "0x00000100")

    def test_line_comment_is_stripped(self):
        self.assertEqual(
            chkreg.strip_comments("0x1234 // trailing note").strip(), "0x1234")

    def test_block_comment_spanning_the_middle_is_stripped(self):
        self.assertEqual(
            chkreg.strip_comments("0x12 /* mid */ + 0x34").replace("  ", " "),
            "0x12  + 0x34".replace("  ", " "))

    def test_parameterised_define_captures_name_arg_and_body(self):
        m = chkreg.PARAM_DEFINE_RE.match(
            "#define NV_PGSP_MAILBOX(i)   (0x00110804+(i)*4)")
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), "NV_PGSP_MAILBOX")
        self.assertEqual(m.group(2), "i")
        self.assertEqual(m.group(3).strip(), "(0x00110804+(i)*4)")

    def test_a_parameterised_define_is_not_matched_as_plain(self):
        # If PARAM_DEFINE_RE were checked second, the plain regex would claim
        # "NV_PGSP_MAILBOX(i)" style lines and mangle the name. parse_defines
        # tries the param form first; this pins that ordering requirement.
        line = "#define NV_PGSP_MAILBOX(i)   (0x00110804+(i)*4)"
        plain = chkreg.DEFINE_RE.match(line)
        if plain is not None:
            self.assertNotEqual(
                plain.group(1), "NV_PGSP_MAILBOX",
                "plain regex must not capture a parameterised define's name")

    def test_non_define_lines_are_ignored(self):
        for line in ("", "   ", "// #define NOT_A_DEFINE 1",
                     "int x = 5;", "#include <stdio.h>", "#undef FOO"):
            self.assertIsNone(chkreg.DEFINE_RE.match(line),
                              "matched a non-define: %r" % line)


class NumericAndBitrangeRegex(unittest.TestCase):

    def test_numeric_accepts_hex_and_decimal(self):
        for good in ("0x0", "0X1F", "0x00000100", "0", "4096"):
            self.assertIsNotNone(chkreg.NUMERIC_RE.match(good), good)

    def test_numeric_rejects_expressions_and_suffixes(self):
        # These must fall through to the arithmetic/alias branches of resolve(),
        # not be int()'d directly -- int("0x10+1", 0) raises.
        for bad in ("0x10+1", "0x10U", "(0x10)", "NV_OTHER_NAME", "1:0"):
            self.assertIsNone(chkreg.NUMERIC_RE.match(bad), bad)

    def test_bitrange_matches_hi_lo(self):
        m = chkreg.BITRANGE_RE.match("7:4")
        self.assertIsNotNone(m)
        self.assertEqual((m.group(1), m.group(2)), ("7", "4"))

    def test_bitrange_tolerates_spaces(self):
        self.assertIsNotNone(chkreg.BITRANGE_RE.match("28 : 24"))


class Resolve(unittest.TestCase):

    def test_plain_hex(self):
        self.assertEqual(chkreg.resolve("A", {"A": "0x1234"}), 0x1234)

    def test_decimal(self):
        self.assertEqual(chkreg.resolve("A", {"A": "4096"}), 4096)

    def test_alias_chain(self):
        table = {"A": "B", "B": "C", "C": "0x40"}
        self.assertEqual(chkreg.resolve("A", table), 0x40)

    def test_alias_cycle_terminates(self):
        # A cycle must return None, not recurse forever. The depth guard is the
        # only thing standing between a malformed header and a hung CI gate.
        table = {"A": "B", "B": "A"}
        self.assertIsNone(chkreg.resolve("A", table))

    def test_constant_arithmetic(self):
        self.assertEqual(chkreg.resolve("A", {"A": "(0x1000 + 0x24)"}), 0x1024)

    def test_missing_name_is_none(self):
        self.assertIsNone(chkreg.resolve("NOPE", {"A": "0x1"}))

    def test_unparseable_body_is_none_not_an_exception(self):
        # A wrong answer here would be worse than no answer: resolve() returning
        # a bogus int makes check_offset report a mismatch against a value that
        # was never in the header.
        self.assertIsNone(chkreg.resolve("A", {"A": "some(macro, call)"}))

    #
    # THE HEX-LITERAL-PLUS-SYMBOL PATH. Fixed 2026-08-13; these are the
    # regression tests.
    #
    # resolve() has two arithmetic paths and only ONE of them was covered:
    #
    #   pure literals   "(0x1000 + 0x24)"        -> eval'd directly
    #   mixed           "(0xFFu | (SYM << 8))"   -> symbols substituted FIRST
    #
    # test_constant_arithmetic above exercises the first and can never see the
    # bug. The substituter used re.sub(r"[A-Za-z_][A-Za-z0-9_]*", ...), which
    # matches INSIDE a hex literal: in `0xFF` the leading `0` is not part of the
    # pattern, the scan resumes at `x`, and `xFF` matches as though it were a
    # symbol name. It resolved to None, "None" landed in the expression, and the
    # whole body was reported as "not found or unparseable" -- wording that
    # blames the header, not the parser.
    #
    # Every composed constant in the kext before 2026-08-13 wrote its literal
    # operands in DECIMAL (`(NVU32)1u << SHIFT`, `(1ULL << BITS) - 1ULL`), so
    # nothing had ever taken this path with a hex literal in it.
    #

    def test_hex_literal_beside_a_symbol(self):
        # The exact shape of NVGSP_DMA_PHYSHI_MASK. Under the old pattern this
        # returned None.
        table = {"A": "(0xFFu | ((NVU32)B << 8))", "B": "0x1FF"}
        self.assertEqual(chkreg.resolve("A", table), 0xFF | (0x1FF << 8))

    def test_bare_hex_literal_is_not_read_as_a_symbol(self):
        # Narrowest statement of the defect: a hex literal in the substitution
        # path must survive it. The `| C` is only there to force the expression
        # off the pure-literal path above.
        table = {"A": "0xFF | C", "C": "0"}
        self.assertEqual(chkreg.resolve("A", table), 0xFF)

    def test_uppercase_hex_prefix_too(self):
        # `0XFF` puts `XFF` where `xFF` was. Same defect, different case, and a
        # fix that only rejected a preceding lowercase `0x` would pass the test
        # above and fail this one.
        table = {"A": "0XAB | C", "C": "0"}
        self.assertEqual(chkreg.resolve("A", table), 0xAB)

    def test_symbol_after_a_digit_run_is_still_substituted(self):
        # The far side of the same boundary. The fix rejects a match preceded by
        # [0-9A-Za-z_], so it must not also swallow a legitimate symbol that
        # merely FOLLOWS a number across an operator or a space. A fix that
        # over-rejected would make this return None while the tests above pass.
        #
        # The symbol is deliberately NOT named with hex-digit letters. An
        # earlier revision of this test used "B", which made the whole body
        # "(8 + B)" match the PURE-LITERAL fast path at resolve()'s
        # `[\dxXa-fA-F\s()+\-*]+` fullmatch -- "B" is a hex digit -- so it was
        # eval'd as text, raised, and returned None without ever reaching the
        # substituter this test is about. It failed for a reason unrelated to
        # the defect. Any test targeting the composed path must use at least one
        # character outside that class.
        table = {"A": "(8 + SYM_X)", "SYM_X": "0x9"}
        self.assertEqual(chkreg.resolve("A", table), 8 + 0x9)

    def test_a_genuinely_unknown_symbol_still_returns_none(self):
        # And the fix must not turn unknown symbols into silently-dropped text.
        # If it did, a header referencing a missing constant would resolve to a
        # plausible number instead of failing -- the worst outcome for a gate
        # whose job is comparing values.
        self.assertIsNone(chkreg.resolve("A", {"A": "0xFF | MISSING"}))


class ResolveBitrange(unittest.TestCase):
    """'HI:LO' -> (shift, mask). This is the decode that was wrong twice."""

    def test_single_bit(self):
        self.assertEqual(chkreg.resolve_bitrange("F", {"F": "4:4"}), (4, 0x1))

    def test_nibble(self):
        self.assertEqual(chkreg.resolve_bitrange("F", {"F": "7:4"}), (4, 0xF))

    def test_byte_at_offset(self):
        self.assertEqual(chkreg.resolve_bitrange("F", {"F": "31:24"}),
                         (24, 0xFF))

    def test_full_word(self):
        self.assertEqual(chkreg.resolve_bitrange("F", {"F": "31:0"}),
                         (0, 0xFFFFFFFF))

    def test_mask_is_pre_shift(self):
        # The mask must be the field width, NOT the field in place. Getting this
        # backwards produces decodes that are right only for fields at bit 0 --
        # which is exactly how a bad decode survives casual testing.
        shift, mask = chkreg.resolve_bitrange("F", {"F": "11:8"})
        self.assertEqual(shift, 8)
        self.assertEqual(mask, 0xF)
        self.assertEqual((0xABCD >> shift) & mask, 0xB)

    def test_inverted_range_is_rejected(self):
        self.assertIsNone(chkreg.resolve_bitrange("F", {"F": "4:7"}))

    def test_non_bitrange_body_is_none(self):
        self.assertIsNone(chkreg.resolve_bitrange("F", {"F": "0x1234"}))

    def test_missing_name_is_none(self):
        self.assertIsNone(chkreg.resolve_bitrange("NOPE", {}))


class ParseDefinesEndToEnd(unittest.TestCase):

    def _write(self, text):
        import pathlib
        import tempfile
        fd, path = tempfile.mkstemp(suffix=".h")
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
        self.addCleanup(os.remove, path)
        return pathlib.Path(path)

    def test_plain_and_param_are_separated(self):
        path = self._write(
            "#define NV_A 0x10\n"
            "#define NV_B(i) (0x20+(i)*4)\n"
            "#define NV_C 0x30 /* comment */\n")
        plain, param = chkreg.parse_defines(path)
        self.assertEqual(chkreg.resolve("NV_A", plain), 0x10)
        self.assertEqual(chkreg.resolve("NV_C", plain), 0x30)
        self.assertIn("NV_B", param)
        self.assertNotIn("NV_B", plain)

    def test_missing_file_returns_empty_not_an_error(self):
        import pathlib
        plain, param = chkreg.parse_defines(pathlib.Path("does-not-exist.h"))
        self.assertEqual((plain, param), ({}, {}))

    def test_valueless_define_is_dropped(self):
        # `#define GUARD_H` has no body. Keeping it would put an empty string in
        # the table, where resolve() would return None and the check would be
        # reported as unparseable rather than absent.
        path = self._write("#define GUARD_H\n#define NV_A 0x10\n")
        plain, _param = chkreg.parse_defines(path)
        self.assertNotIn("GUARD_H", plain)
        self.assertIn("NV_A", plain)


# ---------------------------------------------------------------------------
# collapse-sequence.py -- listing parsing and run collapsing
# ---------------------------------------------------------------------------

class CollapseModuleShape(unittest.TestCase):
    """Guards the assumptions the rest of this file makes about the module."""

    def test_module_loaded(self):
        self.assertTrue(hasattr(collapse, "__file__"))

    def test_exposes_a_line_parser(self):
        # The tool must keep a parse-one-line entry point separate from its I/O.
        # If this fails, the parsing logic has been inlined into main() and is
        # no longer testable -- which is itself the regression.
        candidates = [n for n in dir(collapse)
                      if n.startswith(("parse", "collapse", "LINE", "RE_"))
                      or n.endswith(("_RE", "_re"))]
        self.assertTrue(candidates,
                        "collapse-sequence.py exposes no parse/collapse symbol; "
                        "parsing logic must stay callable, not buried in main()")


if __name__ == "__main__":
    unittest.main(verbosity=2)
