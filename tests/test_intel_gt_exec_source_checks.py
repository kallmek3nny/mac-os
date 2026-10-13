"""Negative controls for the source-derived Intel GT descriptor constants."""
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("exec_register_checker", ROOT / "tools" / "check-registers.py")
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class Report:
    def __init__(self):
        self.passes = []
        self.failures = []

    def passed(self, label, detail):
        self.passes.append((label, detail))

    def failed(self, label, detail):
        self.failures.append((label, detail))


class ExecSourceChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.i915 = self.root / "reference" / "i915"
        self.i915.mkdir(parents=True)
        (self.root / "phase0-kext").mkdir()
        self.hdr = self.root / "phase0-kext" / "IntelGtExec.hpp"
        self.original = (ROOT / "phase0-kext" / "IntelGtExec.hpp").read_text(encoding="utf-8")
        self.hdr.write_text(self.original, encoding="utf-8")
        for name in ("intel_gtt.h", "intel_lrc.h", "intel_engine_regs.h"):
            (self.i915 / name).write_bytes((ROOT / "reference" / "i915" / name).read_bytes())

    def run_check(self):
        report = Report()
        with patch.object(CHECKER, "ROOT", self.root), patch.object(CHECKER, "I915", self.i915):
            CHECKER.intel_gt_exec_checks(report)
        return report

    def test_live_baseline_and_all_new_checks_run(self):
        report = self.run_check()
        self.assertEqual(report.failures, [])
        self.assertEqual(len(report.passes), 24)

    def test_each_wrong_descriptor_constant_fails_its_own_comparison(self):
        for name in ("IG_GTX_CTXDESC_PRIVILEGE_I915_0x100",
                     "IG_GTX_CTXDESC_FORCE_RESTORE_I915_0x4",
                     "IG_GTX_CTXDESC_ADDRESS_MODE_SHIFT_I915_3",
                     "IG_GTX_CTXDESC_LEGACY32_I915_1",
                     "IG_GTX_CTXDESC_LEGACY64_I915_3"):
            with self.subTest(name=name):
                source, replaced = re.subn(r"(#define\s+" + name + r"\s+)\S+", r"\g<1>0u", self.original)
                self.assertEqual(replaced, 1)
                self.hdr.write_text(source, encoding="utf-8")
                report = self.run_check()
                self.assertEqual(len(report.failures), 1, report.failures)
                self.assertIn(name + " vs ", report.failures[0][0])

    def test_missing_upstream_header_fails_not_skips(self):
        (self.i915 / "intel_lrc.h").unlink()
        report = self.run_check()
        self.assertEqual(len(report.failures), 1)
        self.assertIn("intel_lrc.h not found", report.failures[0][1])

    def test_missing_mode_enum_does_not_supply_default_values(self):
        path = self.i915 / "intel_lrc.h"
        source = path.read_text(encoding="utf-8")
        source = source.replace("INTEL_ADVANCED_CONTEXT", "REMOVED_CONTEXT_ENUM")
        path.write_text(source, encoding="utf-8")
        report = self.run_check()
        self.assertEqual(len(report.failures), 2, report.failures)
        self.assertTrue(all("CTXDESC_LEGACY" in label for label, _ in report.failures))

    def test_removing_new_citations_fails_coverage(self):
        source, replaced = re.subn(r"// intel_lrc\.h:(?:95|93|96|78|80) [^\n]+", "// citation removed", self.original)
        self.assertEqual(replaced, 5)
        self.hdr.write_text(source, encoding="utf-8")
        report = self.run_check()
        self.assertEqual(len(report.failures), 1)
        self.assertEqual(report.failures[0][0], "exec citations")


if __name__ == "__main__":
    unittest.main()
