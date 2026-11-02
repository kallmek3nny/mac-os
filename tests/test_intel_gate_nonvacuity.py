#!/usr/bin/env python3
"""
test_intel_gate_nonvacuity.py -- the Intel arm of check-registers.py must be
SENSITIVE, not merely present.

Why this exists
---------------
On 2026-08-16 ``tools/check-registers.py`` grew an Intel corpus: ``INTEL_HEADER``
(``phase0-kext/IntelDisplay.hpp``), ``I915_HEADERS`` (the vendored
``reference/i915/`` headers) and an ``INTEL_CHECKS`` table, plus
``run_intel_checks()`` and ``intel_sanity_checks()``. The printed verdict moved
from ``114/114`` to ``165/165`` and the summary began naming both corpora.

A moving count proves the rows are EVALUATED. It does not prove they are
SENSITIVE. Three shapes print the identical green:

  * a row whose two sides resolve to the same symbol, so it compares a value
    against itself;
  * a row whose i915 name is missing, which ``Report.skip()`` records -- and a
    skip is a silent pass;
  * a whole ``INTEL_CHECKS`` kind whose dispatcher branch is dead, carried by
    the other kinds' passes.

This tree has been bitten by each of those. See the memory notes
``an-assertion-that-cannot-fail-is-not-coverage`` and
``when-no-gate-can-see-it-pin-the-shape``.

So this module mutates ONE ``IG_`` constant at a time in an IN-MEMORY copy of
the parsed table and requires the report to redden NAMING that constant. One arm
per ``INTEL_CHECKS`` kind, discovered from the table rather than hardcoded, so a
newly added kind is covered the day it appears.

Nothing on disk is written. ``IntelDisplay.hpp`` and ``tools/check-registers.py``
are both opened read-only; the switch-base arm mutates the header TEXT in a
string, never the file.

Two harness defects this file was born with, kept as comments where they bit,
because both were silent rather than loud:

  1. ``parse_defines()`` and ``merge_i915_sources()` each return a TUPLE.
     Binding the tuple as the table does not raise -- ``resolve()`` simply
     misses every name, every row degrades to a skip, and the output is a
     plausible wall of "not found in vendored i915 headers". The magnitude
     assertions below exist to catch exactly that.
  2. The first switch-base arm used the literal ``0x60400``, which the header
     spells zero times, so the mutation could not be applied at all --
     ``a-mutation-that-matches-nothing-prints-ok``, one level up. Every anchor
     is now asserted to occur exactly once before it is replaced.
"""
import contextlib
import importlib.util
import io
import os
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, os.pardir))
_GATE = os.path.join(_ROOT, "tools", "check-registers.py")


def _load_gate():
    spec = importlib.util.spec_from_file_location("check_registers_under_test",
                                                  _GATE)
    module = importlib.util.module_from_spec(spec)
    # The gate parses sys.argv at import time only inside main(), but keep the
    # load quiet regardless -- it prints nothing at module scope today and this
    # costs nothing if that changes.
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(module)
    return module


HAVE_GATE = os.path.isfile(_GATE)
cr = _load_gate() if HAVE_GATE else None

# Pinned against the gate's own printed summary, measured 2026-08-16 11:55:
#   kext headers  : ... IntelDisplay.hpp (50 defines)
#   i915 headers  : 3180 defines from 3 files
#
# These are magnitudes, not values, and they are here for ONE reason: to catch a
# harness that builds its tables differently from main(). They are expected to
# drift as the header grows, and the assertions below use >= with a floor rather
# than equality so that growth does not redden the module -- but the floor is
# high enough that an empty or tuple-shaped table cannot slip past.
MIN_INTEL_DEFINES = 40
MIN_I915_DEFINES = 2000

# Measured the same run: run_intel_checks + intel_sanity_checks together report
# 53 passes, 0 failures, 0 skips on a clean tree.
MIN_INTEL_CHECKS = 30


@unittest.skipUnless(HAVE_GATE, "tools/check-registers.py is absent")
class IntelGateNonVacuityTests(unittest.TestCase):

    def fresh(self):
        """Parse the real corpora afresh. Mutations must not leak between arms."""
        kext, _kext_param = cr.parse_defines(cr.INTEL_HEADER)
        src, _missing = cr.merge_i915_sources()
        text = cr.INTEL_HEADER.read_text(encoding="utf-8", errors="replace")
        return kext, src, text

    def run_intel(self, kext, src, text=None):
        """Run both Intel entry points and return the Report.

        stdout is swallowed because Report.failed() prints as it goes, and a
        deliberately-reddened arm would otherwise spray FAIL lines through the
        gate's own output and read as a real defect.
        """
        rep = cr.Report(False)
        with contextlib.redirect_stdout(io.StringIO()):
            cr.run_intel_checks(rep, kext, src)
            if text is not None:
                cr.intel_sanity_checks(rep, kext, src, text)
        return rep

    # ------------------------------------------------------------ baseline

    def test_both_corpora_parse_to_the_shape_main_uses(self):
        """Guards harness defect 1 -- a tuple bound as a table is silent."""
        kext, src, _text = self.fresh()
        self.assertIsInstance(kext, dict)
        self.assertIsInstance(src, dict)
        self.assertGreaterEqual(
            len(kext), MIN_INTEL_DEFINES,
            "IntelDisplay.hpp parsed to %d defines; the gate's summary printed "
            "50. A short table means the harness is not building it the way "
            "main() does." % len(kext))
        self.assertGreaterEqual(
            len(src), MIN_I915_DEFINES,
            "the vendored i915 corpus parsed to %d defines; the gate's summary "
            "printed 3180." % len(src))

    def test_the_clean_tree_is_green_and_skips_nothing(self):
        """A skip is a silent pass, so skips must be named, not tolerated."""
        kext, src, text = self.fresh()
        rep = self.run_intel(kext, src, text)
        self.assertEqual(
            [], rep.failures,
            "the Intel rows are not green on a clean tree: %s"
            % "; ".join("%s: %s" % f for f in rep.failures[:5]))
        self.assertEqual(
            [], rep.skipped,
            "the Intel arm skipped %d row(s), and a skip prints as a pass: %s"
            % (len(rep.skipped),
               "; ".join("%s: %s" % s for s in rep.skipped[:5])))
        self.assertGreaterEqual(
            rep.ok, MIN_INTEL_CHECKS,
            "only %d Intel rows were evaluated; expected at least %d."
            % (rep.ok, MIN_INTEL_CHECKS))

    # ------------------------------------------------------- mutation arms

    def mutate_one(self, kname):
        """Flip one bit of one IG_ constant; return (before, after, report)."""
        kext, src, text = self.fresh()

        self.assertIn(kname, kext,
                      "%s is not in the parsed IntelDisplay table, so the "
                      "mutation would apply to nothing" % kname)

        before = cr.resolve(kname, kext)
        self.assertIsNotNone(
            before, "%s does not resolve on a clean tree, so a mutation of it "
                    "proves nothing" % kname)

        # XOR rather than +1: preserves the width, guarantees a change, and
        # cannot land on a neighbouring legitimate constant the way an
        # increment can.
        kext[kname] = "0x%X" % (before ^ 0x40)
        after = cr.resolve(kname, kext)
        self.assertNotEqual(before, after,
                            "the mutation did not change %s's resolved value"
                            % kname)

        return before, after, self.run_intel(kext, src, text)

    def test_every_intel_checks_kind_has_a_live_dispatcher_branch(self):
        """One arm per kind, discovered from the table.

        Hardcoding the kinds would leave a newly added one silently uncovered,
        which is the same defect class this module exists to catch.
        """
        kinds = {}
        for row in cr.INTEL_CHECKS:
            kinds.setdefault(row[0], row[1])

        self.assertGreaterEqual(len(kinds), 2,
                               "INTEL_CHECKS declares only %d kind(s); the "
                               "table is expected to mix offsets, values, "
                               "deltas and field positions" % len(kinds))

        for kind, kname in sorted(kinds.items()):
            with self.subTest(kind=kind, constant=kname):
                before, after, rep = self.mutate_one(kname)
                named = [f for f in rep.failures
                         if kname in f[0] or kname in f[1]]
                self.assertTrue(
                    named,
                    "mutating %s (kind %r) from 0x%X to 0x%X produced %d "
                    "failure(s), none naming it. That kind's branch is not "
                    "sensitive to the constant it claims to check."
                    % (kname, kind, before, after, len(rep.failures)))

    def test_mutating_a_transcoder_switch_base_reddens(self):
        """The twelve switch bases are invisible to parse_defines().

        igTransBase()/igPipeBase() are switch statements, so their bases live in
        the header TEXT rather than in any #define. intel_sanity_checks() reads
        them out of the source. If that reader were wrong, every base would be
        unverified while the summary still named the header.

        Transcoder B is the subject because moving it also breaks the uniform
        0x1000 ladder, so the failure is attributable to the base rather than to
        one row's own comparison.
        """
        kext, src, text = self.fresh()

        anchor = "0x61000u"
        self.assertEqual(
            1, text.count(anchor),
            "the mutation anchor %s occurs %d times in IntelDisplay.hpp; it "
            "must occur exactly once or the mutation is not the one described"
            % (anchor, text.count(anchor)))

        rep = self.run_intel(kext, src, text.replace(anchor, "0x61040u"))
        self.assertTrue(
            rep.failures,
            "moving transcoder B's switch base by 0x40 did not redden "
            "anything, so the switch bases are unverified")

    # --------------------------------------------- empty-corpus guard arms

    def test_an_empty_intel_table_fails_rather_than_skipping(self):
        """The guard at the top of run_intel_checks() is the whole story here.

        Without it, a rename or a moved file empties the table, every row
        becomes a skip, and the gate prints green while checking nothing.

        Note what is NOT asserted: that nothing passes. intel_sanity_checks()
        reads the header TEXT for the switch bases, so with an empty #define
        table those text-driven rows legitimately still pass. An earlier version
        of this arm required rep.ok == 0 and reddened against a correct guard --
        the arm was measuring the wrong quantity.
        """
        _kext, src, text = self.fresh()
        rep = self.run_intel({}, src, text)
        named = [f for f in rep.failures if "IntelDisplay" in f[0]]
        self.assertTrue(named,
                        "an empty IntelDisplay table produced no failure "
                        "naming the header")
        self.assertEqual([], rep.skipped,
                         "an empty table produced %d skip(s), and a skip "
                         "prints as a pass" % len(rep.skipped))

    def test_an_empty_i915_corpus_fails_rather_than_skipping(self):
        """Same property, the other corpus.

        This is the likelier of the two in practice: reference/i915/ is vendored
        at a pinned version, so a path change there is a plausible accident.
        """
        kext, _src, text = self.fresh()
        rep = self.run_intel(kext, {}, text)
        named = [f for f in rep.failures if "i915" in f[0]]
        self.assertTrue(named,
                        "an empty i915 corpus produced no failure naming it")
        self.assertEqual([], rep.skipped,
                         "an empty corpus produced %d skip(s), and a skip "
                         "prints as a pass" % len(rep.skipped))


#
# ---------------------------------------------------------------------------
#  The SECOND Intel arm: run_intel_id_checks() over IntelGpuId.hpp.
# ---------------------------------------------------------------------------
#
#  Added 2026-08-16, the same day the rows were. The gate's printed verdict
#  moved 167 -> 184, and a moving count proves the rows are EVALUATED and
#  nothing else -- which is the identical position the display arm was in when
#  the class above was written.
#
#  This arm has one hazard the display arm does not. The vendored PCI IDs are
#  not #defines: i915_pciids.h spells them as bare literals inside variadic
#  macro bodies, so i915_id_list() walks the macro body with a regex. A regex
#  that stopped matching would return None for every table, every row would
#  become a skip or a guard failure, and the CONSTANTS would never be compared
#  to anything. So the extractor is pinned against ground truth first -- the
#  eleven literals, in order, typed out again from the vendored file -- before
#  any mutation arm runs. Without that, a broken extractor and a correct one
#  produce the same green.
#

GT1_GROUND_TRUTH = (0x9A60, 0x9A68, 0x9A70)
GT2_GROUND_TRUTH = (0x9A40, 0x9A49, 0x9A59, 0x9A78,
                    0x9AC0, 0x9AC9, 0x9AD9, 0x9AF8)


@unittest.skipUnless(HAVE_GATE, "tools/check-registers.py is absent")
class IntelIdGateNonVacuityTests(unittest.TestCase):

    def fresh(self):
        kext, _param = cr.parse_defines(cr.INTEL_ID_HEADER)
        text = cr.I915_PCIIDS.read_text(encoding="utf-8", errors="replace")
        return kext, text

    def run_ids(self, kext, text):
        rep = cr.Report(False)
        with contextlib.redirect_stdout(io.StringIO()):
            cr.run_intel_id_checks(rep, kext, text)
        return rep

    # ------------------------------------------------- the extractor itself

    def test_the_extractor_returns_the_vendored_literals_in_order(self):
        """Ground truth, retyped. Everything below rests on this.

        Compared as an ORDERED tuple rather than a set: IntelGpuId.hpp's
        accessors are index-addressed, so a permuted table still classifies
        every device correctly while handing the wrong slot to anything that
        iterates -- and a set comparison cannot see that at all.
        """
        _kext, text = self.fresh()
        self.assertEqual(GT1_GROUND_TRUTH,
                         tuple(cr.i915_id_list("INTEL_TGL_GT1_IDS", text)))
        self.assertEqual(GT2_GROUND_TRUTH,
                         tuple(cr.i915_id_list("INTEL_TGL_GT2_IDS", text)))

    def test_the_extractor_returns_none_rather_than_an_empty_list(self):
        """An empty list would zip() against nothing and check nothing.

        None is a value every caller tests for; [] is falsy in the same way but
        reads downstream as \"a real table that happens to be empty\", and
        len([]) == 0 would then satisfy a count check against a header whose
        count was also wrong.
        """
        _kext, text = self.fresh()
        self.assertIsNone(cr.i915_id_list("INTEL_NOSUCH_IDS", text))
        self.assertIsNone(cr.i915_id_list("INTEL_TGL_GT1_IDS", ""))
        self.assertIsNone(cr.i915_id_list("INTEL_TGL_GT1_IDS", None))

    def test_the_extractor_does_not_bleed_into_the_next_macro(self):
        """The GT1 body must stop at its own last continuation line.

        INTEL_TGL_GT2_IDS begins two lines below INTEL_TGL_GT1_IDS in the
        vendored file. A body regex that kept consuming would return all eleven
        IDs for GT1, the count row would then compare 3 against 11 and redden --
        but the SLOT rows would still pass, because the first three entries are
        the right ones. That is a partial failure that reads as a transcription
        error in the header rather than as a broken extractor.
        """
        _kext, text = self.fresh()
        gt1 = cr.i915_id_list("INTEL_TGL_GT1_IDS", text)
        self.assertEqual(3, len(gt1))
        self.assertNotIn(0x9A40, gt1,
                         "the GT1 body swallowed GT2's first entry")

    # ------------------------------------------------------------ baseline

    def test_the_clean_tree_is_green_and_skips_nothing(self):
        kext, text = self.fresh()
        rep = self.run_ids(kext, text)
        self.assertEqual(
            [], rep.failures,
            "the ID rows are not green on a clean tree: %s"
            % "; ".join("%s: %s" % f for f in rep.failures[:5]))
        self.assertEqual(
            [], rep.skipped,
            "the ID arm skipped %d row(s), and a skip prints as a pass: %s"
            % (len(rep.skipped),
               "; ".join("%s: %s" % s for s in rep.skipped[:5])))
        # 11 slots + 2 counts + union + vendor + observed + distinctness.
        self.assertGreaterEqual(
            rep.ok, 17,
            "only %d ID rows were evaluated; expected at least 17" % rep.ok)

    # ------------------------------------------------------- mutation arms

    def mutate_one(self, kname):
        """Flip one IG_ constant in memory; return (before, after, report)."""
        kext, text = self.fresh()

        before = cr.resolve(kname, kext)
        self.assertIsNotNone(
            before,
            "%s does not resolve on a clean tree, so mutating it proves "
            "nothing" % kname)

        # XOR 0x4 rather than +1. Every neighbour of a PCI ID in these tables
        # is a plausible-looking value, and +1 on IG_TGL_GT2_ID_1 (0x9A49)
        # lands on 0x9A4A -- still absent, but the increment on GT1's 0x9A60
        # would land adjacent to nothing while GT2's 0x9A78 + 1 sits between
        # two real entries. A bit flip keeps the width and cannot alias onto
        # another slot's value.
        kext[kname] = "0x%X" % (before ^ 0x4)
        after = cr.resolve(kname, kext)
        self.assertNotEqual(before, after, kname)

        return before, after, self.run_ids(kext, text)

    def test_every_transcribed_id_is_individually_sensitive(self):
        """One arm per row of IG_ID_EXPECT, discovered from the table.

        Hardcoding the eleven names would leave a twelfth silently uncovered
        the day someone adds it -- the same defect class this module exists
        for, one level up.
        """
        self.assertEqual(11, len(cr.IG_ID_EXPECT),
                         "IG_ID_EXPECT lists %d rows; the vendored Tiger Lake "
                         "tables hold 11 IDs" % len(cr.IG_ID_EXPECT))

        for _macro, _index, kname in cr.IG_ID_EXPECT:
            with self.subTest(constant=kname):
                before, after, rep = self.mutate_one(kname)
                named = [f for f in rep.failures
                         if kname in f[0] or kname in f[1]]
                self.assertTrue(
                    named,
                    "mutating %s from 0x%X to 0x%X produced %d failure(s), "
                    "none naming it -- that ID is not actually compared "
                    "against the vendored table"
                    % (kname, before, after, len(rep.failures)))

    def test_each_count_is_sensitive(self):
        """The counts are what go red when i915 adds an ID upstream.

        Every slot row would still match a grown table; only these three see
        the new entry the driver cannot reach.
        """
        for kname in ("IG_TGL_GT1_ID_COUNT", "IG_TGL_GT2_ID_COUNT",
                      "IG_TGL_ID_COUNT"):
            with self.subTest(constant=kname):
                kext, text = self.fresh()
                before = cr.resolve(kname, kext)
                self.assertIsNotNone(before, kname)
                kext[kname] = str(before + 1)
                rep = self.run_ids(kext, text)
                named = [f for f in rep.failures
                         if kname in f[0] or kname in f[1]]
                self.assertTrue(
                    named,
                    "%s went from %d to %d and nothing reddened naming it"
                    % (kname, before, before + 1))

    def test_the_vendor_id_is_sensitive(self):
        before, after, rep = self.mutate_one("IG_PCI_VENDOR_INTEL")
        named = [f for f in rep.failures if "IG_PCI_VENDOR_INTEL" in f[0]]
        self.assertTrue(
            named,
            "the vendor ID moved 0x%X -> 0x%X with no failure naming it, so "
            "it is not compared against INTEL_VGA_DEVICE's own field"
            % (before, after))

    def test_the_observed_device_is_sensitive(self):
        """The capture's reading, checked as MEMBERSHIP of the vendored set.

        This row and IG_TGL_GT1_ID_1's row are deliberately separate: both are
        0x9A68, from two independent readings (the target laptop's sysfs, and
        Linux's table). Checking only that they equal each other would let two
        matching errors cancel.
        """
        before, after, rep = self.mutate_one("IG_DEVICE_OBSERVED_2026_08_16")
        named = [f for f in rep.failures
                 if "IG_DEVICE_OBSERVED_2026_08_16" in f[0]]
        self.assertTrue(
            named,
            "the observed device moved 0x%X -> 0x%X and stayed green, so the "
            "capture is not checked against the vendored table at all"
            % (before, after))

    def test_a_permuted_table_reddens(self):
        """Order, not just membership.

        Swapping two transcribed IDs leaves the SET identical, so every
        set-shaped check -- distinctness, counts, the observed-device
        membership row -- stays green. Only the index-addressed slot rows can
        see it, and a driver with a permuted table returns the wrong device to
        anything that iterates by slot.
        """
        kext, text = self.fresh()
        a, b = "IG_TGL_GT2_ID_0", "IG_TGL_GT2_ID_1"
        kext[a], kext[b] = kext[b], kext[a]

        rep = self.run_ids(kext, text)
        named = [f for f in rep.failures if a in f[0] or b in f[0]]
        self.assertEqual(
            2, len(named),
            "swapping %s and %s produced %d naming failure(s); both slots must "
            "redden, or the table is checked as a set and a permutation is "
            "invisible" % (a, b, len(named)))

    # --------------------------------------------- empty-corpus guard arms

    def test_an_empty_id_table_fails_rather_than_skipping(self):
        _kext, text = self.fresh()
        rep = self.run_ids({}, text)
        named = [f for f in rep.failures if "IntelGpuId" in f[0]]
        self.assertTrue(named,
                        "an empty IntelGpuId table produced no failure naming "
                        "the header")
        self.assertEqual([], rep.skipped,
                         "an empty table produced %d skip(s), and a skip "
                         "prints as a pass" % len(rep.skipped))

    def test_an_unreadable_pciids_file_fails_rather_than_skipping(self):
        """The likelier accident: reference/i915/ is vendored at a pinned tag.

        Both None and empty text must refuse. An earlier guard elsewhere in
        this tree tested only None and let \"\" through, which is what a
        truncated or zero-byte vendored file actually looks like.
        """
        kext, _text = self.fresh()
        for corpus in (None, ""):
            with self.subTest(corpus=repr(corpus)):
                rep = self.run_ids(kext, corpus)
                named = [f for f in rep.failures if "i915_pciids" in f[0]]
                self.assertTrue(
                    named, "an unreadable i915_pciids.h produced no failure "
                           "naming it")
                self.assertEqual([], rep.skipped, len(rep.skipped))


# Pinned against the gate's own printed summary, measured 2026-08-17:
#   kext headers  : ... IntelFramebuffer.hpp (46 defines)
# and the verbose run showed 34 rows whose label begins IG_FB_.
#
# Floors, not equalities, for the same reason as MIN_INTEL_DEFINES above: the
# header is expected to grow. The floor is high enough that an empty table or a
# tuple bound where a dict belongs cannot slip past.
MIN_INTEL_FB_DEFINES = 40
MIN_INTEL_FB_CHECKS = 30


@unittest.skipUnless(HAVE_GATE, "tools/check-registers.py is absent")
class IntelFramebufferGateNonVacuityTests(unittest.TestCase):
    """The framebuffer arm must be SENSITIVE, not merely present.

    ``run_intel_fb_checks()`` and ``intel_fb_sanity_checks()`` were added on
    2026-08-17 and moved the gate's verdict from 184/184 to 218/218. A verdict
    moving is not evidence the new rows can FAIL -- per the
    ``an-assertion-that-cannot-fail-is-not-coverage`` finding, 34 rows that
    resolve nothing and skip silently produce exactly the same green output.

    Every test below mutates one constant IN MEMORY and asserts the arm goes
    red. Nothing on disk is touched: per
    ``a-tree-mutating-probe-fakes-a-named-fail``, a probe that plants defects in
    the working tree makes a concurrent gate run report named FAILs with real
    tracebacks and no defect.
    """

    def fresh(self):
        """Parse the real corpora afresh. Mutations must not leak between arms."""
        kext, _kext_param = cr.parse_defines(cr.INTEL_FB_HEADER)
        src, _missing = cr.merge_i915_sources()
        return kext, src

    def run_fb(self, kext, src):
        """Run both framebuffer entry points and return the Report.

        stdout is swallowed because Report.failed() prints as it goes, and a
        deliberately-reddened arm would otherwise spray FAIL lines and read as
        a real defect in whatever else is running.
        """
        rep = cr.Report(False)
        with contextlib.redirect_stdout(io.StringIO()):
            cr.run_intel_fb_checks(rep, kext, src)
            cr.intel_fb_sanity_checks(rep, kext, src)
        return rep

    # ------------------------------------------------------------ baseline

    def test_both_corpora_parse_to_the_shape_main_uses(self):
        """A tuple bound as a table does not raise -- it misses every name."""
        kext, src = self.fresh()
        self.assertIsInstance(kext, dict)
        self.assertIsInstance(src, dict)
        self.assertGreaterEqual(
            len(kext), MIN_INTEL_FB_DEFINES,
            "IntelFramebuffer.hpp parsed to %d defines; the gate's summary "
            "printed 46. A short table means the harness is not building it "
            "the way main() does." % len(kext))
        self.assertGreaterEqual(
            len(src), MIN_I915_DEFINES,
            "the vendored i915 corpus parsed to %d defines; the gate's "
            "summary printed 3180." % len(src))

    def test_the_clean_tree_is_green_and_skips_nothing(self):
        """A skip is a silent pass, so skips must be named, not tolerated."""
        kext, src = self.fresh()
        rep = self.run_fb(kext, src)
        self.assertEqual(
            [], rep.failures,
            "the framebuffer rows are not green on a clean tree: %s"
            % "; ".join("%s: %s" % f for f in rep.failures[:5]))
        self.assertEqual(
            [], rep.skipped,
            "the framebuffer arm skipped %d row(s), and a skip prints as a "
            "pass: %s" % (len(rep.skipped),
                          "; ".join("%s: %s" % s for s in rep.skipped[:5])))
        self.assertGreaterEqual(
            rep.ok, MIN_INTEL_FB_CHECKS,
            "only %d framebuffer rows were evaluated; the verbose run showed "
            "34" % rep.ok)

    def test_every_checks_kind_is_reachable(self):
        """A dispatcher branch no row selects is dead code that reads as live.

        Derived from the table rather than hardcoded: adding a row of a new
        kind with no dispatcher branch would otherwise report a single
        `unknown INTEL_FB_CHECKS kind` failure buried in 200+ ok lines.
        """
        kinds = {row[0] for row in cr.INTEL_FB_CHECKS}
        self.assertEqual({"delta", "value", "shift", "mask"}, kinds,
                         "INTEL_FB_CHECKS kinds are %s; the dispatcher in "
                         "run_intel_fb_checks() handles exactly four" % kinds)

    # ------------------------------------------------------- mutation arms

    #
    # Per-constant mutation bits, for the constants where bit 0 is not read.
    #
    # This is the a-negative-control-can-miss-before-it-tests finding in its
    # exact original shape: 0x82009000 XOR 1 is 0x82009001, which has the same
    # format field, the same tiling field, the same decompression bit and the
    # same enable bit. Nothing in the arm reads bit 0 of PLANE_CTL, so the
    # mutation lands somewhere no check looks and the arm stays green -- and a
    # green arm under a mutation reads as "insensitive" when the real fault is
    # that the probe never reached the line under test.
    #
    # 1 << 23 is the low bit of the five-bit format field: it turns the
    # observed field 4, which decodes to XRGB_2101010 (2), into 5, which
    # decodes to the icl-only code 0x12 (IG_FB_CTL_FORMAT_ICL_FLAG | 2) that
    # the header does not name. CORRECTED 2026-11-01 (t-a7929c): this line
    # used to call the raw field 4 XRGB_8888 and the mutated one 12.
    #
    MUTATION_BIT = {
        "IG_FB_CTL_OBSERVED_2026_08_16": 1 << 23,
    }

    def mutate_one(self, kname):
        """Flip one IG_FB_ constant in memory; return (before, after, report)."""
        kext, src = self.fresh()

        before = cr.resolve(kname, kext)
        self.assertIsNotNone(
            before,
            "%s does not resolve on a clean tree, so mutating it proves "
            "nothing" % kname)

        # XOR rather than +1: an increment on a shift of 0 lands on 1, which is
        # a legitimate shift for a different field, whereas a flip keeps the
        # magnitude class and still differs. It is also total -- every integer
        # has a distinct XOR partner, so no row can silently mutate to itself.
        bit = self.MUTATION_BIT.get(kname, 0x1)
        kext[kname] = "0x%X" % (before ^ bit)
        after = cr.resolve(kname, kext)
        self.assertNotEqual(before, after, kname)

        return before, after, self.run_fb(kext, src)

    def test_every_table_row_is_individually_sensitive(self):
        """One arm per row of INTEL_FB_CHECKS, discovered from the table.

        Hardcoding the names would leave the next row silently uncovered the
        day someone adds it -- the same defect class this module exists for,
        one level up.
        """
        names = [row[1] for row in cr.INTEL_FB_CHECKS]
        self.assertGreaterEqual(len(names), 23,
                                "INTEL_FB_CHECKS has only %d rows; the arm "
                                "was written with 23" % len(names))

        for kname in names:
            with self.subTest(constant=kname):
                before, after, rep = self.mutate_one(kname)
                named = [f for f in rep.failures if f[0] == kname]
                self.assertTrue(
                    named,
                    "%s mutated 0x%X -> 0x%X and the arm did not fail on it; "
                    "that row checks nothing" % (kname, before, after))

    def test_the_observed_capture_constants_are_individually_sensitive(self):
        """The *_OBSERVED_* values are only read by intel_fb_sanity_checks().

        No INTEL_FB_CHECKS row names them -- they are the one live capture,
        and there is no vendored constant to compare them against. So the
        table-driven test above cannot reach them, and without this they would
        sit in the header witnessed by nothing.
        """
        for kname in ("IG_FB_CTL_OBSERVED_2026_08_16",
                      "IG_FB_STRIDE_OBSERVED_2026_08_16",
                      "IG_FB_SIZE_OBSERVED_2026_08_16",
                      "IG_FB_STRIDE_UNIT_DERIVED_2026_08_16",
                      "IG_FB_PANEL_WIDTH_OBSERVED_2026_08_16",
                      "IG_FB_PANEL_HEIGHT_OBSERVED_2026_08_16"):
            with self.subTest(constant=kname):
                before, after, rep = self.mutate_one(kname)
                self.assertTrue(
                    rep.failures,
                    "%s mutated 0x%X -> 0x%X and nothing failed; the one live "
                    "capture is checked by nothing"
                    % (kname, before, after))

    def test_a_transposed_plane_size_decode_is_caught(self):
        """The handoff15 hazard, as a test rather than a comment.

        PLANE_SIZE's halves are INVERTED relative to PIPESRC (0x0437077F vs
        0x077F0437). Swapping the two shifts produces a 1080x1920 surface on a
        1920x1080 panel -- a valid register write that scrambles the screen and
        faults nowhere. Both shifts individually still match a real vendored
        mask after the swap, so the per-row checks alone cannot see it: this is
        what intel_fb_sanity_checks() row 4 exists for.
        """
        kext, src = self.fresh()
        w = cr.resolve("IG_FB_SIZE_WIDTH_SHIFT", kext)
        h = cr.resolve("IG_FB_SIZE_HEIGHT_SHIFT", kext)
        self.assertIsNotNone(w)
        self.assertIsNotNone(h)
        self.assertNotEqual(w, h, "the two shifts are already equal, so "
                                  "swapping them would be a no-op")

        kext["IG_FB_SIZE_WIDTH_SHIFT"] = "0x%X" % h
        kext["IG_FB_SIZE_HEIGHT_SHIFT"] = "0x%X" % w

        rep = self.run_fb(kext, src)
        named = [f for f in rep.failures
                 if f[0] == "IG_FB_SIZE_OBSERVED_2026_08_16"]
        self.assertTrue(named,
                        "a transposed PLANE_SIZE decode produced no failure "
                        "naming the observed capture")
        self.assertIn("TRANSPOSED", named[0][1],
                      "the failure did not identify the transposition, which "
                      "is the whole point of checking both halves: %s"
                      % named[0][1])

    def test_the_four_bit_skl_format_reading_is_caught(self):
        """The most consequential single mutation on this header.

        Shifting the format field to 24 and narrowing the mask to 4 bits is
        the pre-icl reading of the FIELD. Tiger Lake is icl+, so bit 23 is
        part of it and the shift/mask pins must go red. The observed-capture
        row must go red too: the decode takes the code from the field's top
        four bits, so on 0x82009000 the narrowed field 2 decodes to 1, a code
        this header does not name, instead of 2 (XRGB_2101010).

        CORRECTED 2026-11-01 (t-a7929c): this used to say the narrowed
        reading gave 2 (XRGB_2101010) "instead of 4 (XRGB_8888)". 4 is the
        raw five-bit field, never a decoded code; the captured word spells
        XRGB_2101010 (reference/i915/skl_universal_plane_regs.h:52).
        """
        kext, src = self.fresh()
        kext["IG_FB_CTL_FORMAT_SHIFT"] = "24"
        kext["IG_FB_CTL_FORMAT_MASK"] = "0xF"

        rep = self.run_fb(kext, src)
        self.assertTrue(rep.failures,
                        "the four-bit SKL format reading produced no failure")
        labels = {f[0] for f in rep.failures}
        self.assertIn("IG_FB_CTL_FORMAT_SHIFT", labels,
                      "the shift row did not fail: %s" % labels)
        self.assertIn("IG_FB_CTL_OBSERVED_2026_08_16", labels,
                      "the observed capture still decoded to XRGB_2101010 "
                      "under the four-bit mask, so the capture is not "
                      "separating the two readings: %s" % labels)

    def test_a_cleared_render_decompression_bit_is_caught(self):
        """The header's most counterintuitive claim, pinned.

        The ONE live capture is an inheritance REFUSAL case, not an acceptance
        case, because bit 15 is set in 0x82009000. If that ever silently became
        false, every test built around the refusal would keep passing while
        exercising the wrong branch of igFbInheritable().
        """
        kext, src = self.fresh()
        ctl = cr.resolve("IG_FB_CTL_OBSERVED_2026_08_16", kext)
        decomp = cr.resolve("IG_FB_CTL_RENDER_DECOMP", kext)
        self.assertIsNotNone(ctl)
        self.assertIsNotNone(decomp)
        self.assertTrue(ctl & decomp,
                        "bit 15 is already clear in the observed CTL, so this "
                        "mutation tests nothing")

        kext["IG_FB_CTL_OBSERVED_2026_08_16"] = "0x%X" % (ctl & ~decomp)

        rep = self.run_fb(kext, src)
        named = [f for f in rep.failures
                 if f[0] == "IG_FB_CTL_RENDER_DECOMP"]
        self.assertTrue(named,
                        "clearing render decompression in the observed CTL "
                        "produced no failure naming it")

    def test_a_surf_mask_that_swallows_the_flags_is_caught(self):
        """Widening the address mask downward is silent by construction.

        REG_GENMASK(31, 12) widened to bit 2 would make igFbSurfaceOffset()
        return a GGTT offset with the DECRYPT flag baked in -- a 4 KiB
        misaligned address that still looks entirely plausible. The per-row
        value check catches the widening only while the vendored name exists;
        the disjointness check catches the CONSEQUENCE.
        """
        kext, src = self.fresh()
        decrypt = cr.resolve("IG_FB_SURF_DECRYPT", kext)
        addr = cr.resolve("IG_FB_SURF_ADDR_MASK", kext)
        self.assertIsNotNone(decrypt)
        self.assertIsNotNone(addr)

        kext["IG_FB_SURF_ADDR_MASK"] = "0x%X" % (addr | decrypt)

        rep = self.run_fb(kext, src)
        labels = {f[0] for f in rep.failures}
        self.assertIn("IG_FB_SURF_ADDR_MASK", labels,
                      "a SURF mask overlapping DECRYPT produced no failure "
                      "naming it: %s" % labels)

    def test_a_duplicated_format_code_is_caught(self):
        """Two codes colliding is a C++ duplicate-case error -- once compiled.

        This gate runs on a bare checkout where nothing compiles the header, so
        the collision has to be checked arithmetically or not at all.
        """
        kext, src = self.fresh()
        eight = cr.resolve("IG_FB_FORMAT_XRGB_8888", kext)
        self.assertIsNotNone(eight)
        kext["IG_FB_FORMAT_RGB_565"] = "0x%X" % eight

        rep = self.run_fb(kext, src)
        labels = {f[0] for f in rep.failures}
        self.assertIn("IG_FB_FORMAT_*", labels,
                      "two identical format codes produced no failure: %s"
                      % labels)

    def test_a_format_code_wider_than_the_field_is_caught(self):
        """An unreachable case arm warns from no compiler.

        A code above the five-bit mask can never be produced by igFbFormat(),
        so its arm in igFbBytesPerPixel() is dead and the format falls through
        to the 0 default -- refused as unreadable rather than decoded.
        """
        kext, src = self.fresh()
        mask = cr.resolve("IG_FB_CTL_FORMAT_MASK", kext)
        self.assertIsNotNone(mask)
        kext["IG_FB_FORMAT_XRGB_16161616F"] = "0x%X" % (mask + 1)

        rep = self.run_fb(kext, src)
        labels = {f[0] for f in rep.failures}
        self.assertIn("IG_FB_FORMAT_*", labels,
                      "a format code outside the field produced no failure: "
                      "%s" % labels)

    def test_a_collided_inheritance_verdict_is_caught(self):
        """The verdicts are an enumeration precisely so they can be told apart.

        COMPRESSED means refuse permanently; DISABLED means wait. Collapsing
        two of them sends different situations down one path, and a caller that
        switches on the value cannot detect it.
        """
        kext, src = self.fresh()
        disabled = cr.resolve("IG_FB_INHERIT_DISABLED", kext)
        self.assertIsNotNone(disabled)
        kext["IG_FB_INHERIT_COMPRESSED"] = "0x%X" % disabled

        rep = self.run_fb(kext, src)
        labels = {f[0] for f in rep.failures}
        self.assertIn("IG_FB_INHERIT_*", labels,
                      "two identical verdict codes produced no failure: %s"
                      % labels)

    def test_a_nonzero_inherit_ok_is_caught(self):
        """OK must be the zero. Every other verdict is a refusal reason.

        A caller writing `if (igFbInheritable(...))` is idiomatic C and would
        invert on any nonzero OK -- reading every acceptance as a refusal and
        the DISABLED case as success.
        """
        kext, src = self.fresh()
        kext["IG_FB_INHERIT_OK"] = "0x9"

        rep = self.run_fb(kext, src)
        labels = {f[0] for f in rep.failures}
        self.assertIn("IG_FB_INHERIT_OK", labels,
                      "a nonzero IG_FB_INHERIT_OK produced no failure: %s"
                      % labels)

    def test_a_wrong_stride_unit_is_caught(self):
        """The 128-byte unit is DERIVED from one measurement, not vendored.

        Nothing in reference/i915/ states a stride multiplier, so no row can
        compare it against anything -- it is re-derived from the four other
        pinned numbers instead. 64 is the specific wrong answer a reader will
        suspect, and it describes a 960-pixel-wide surface on a lit
        1920-pixel panel.
        """
        kext, src = self.fresh()
        kext["IG_FB_STRIDE_UNIT_DERIVED_2026_08_16"] = "64"

        rep = self.run_fb(kext, src)
        named = [f for f in rep.failures
                 if f[0] == "IG_FB_STRIDE_UNIT_DERIVED_2026_08_16"]
        self.assertTrue(named,
                        "a 64-byte stride unit produced no failure naming it")

    # ------------------------------------------------- refusal, not skipping

    def test_an_empty_kext_table_fails_rather_than_skipping(self):
        """The realistic accident: the header is renamed or moved.

        Every row would degrade to "not found", and if the arm returned
        quietly the gate would stay green while reading nothing at all.
        """
        _kext, src = self.fresh()
        rep = self.run_fb({}, src)
        named = [f for f in rep.failures if "IntelFramebuffer" in f[0]]
        self.assertTrue(named,
                        "an empty IntelFramebuffer table produced no failure "
                        "naming the header")
        self.assertEqual([], rep.skipped,
                         "an empty table produced %d skip(s), and a skip "
                         "prints as a pass" % len(rep.skipped))

    def test_an_empty_i915_corpus_fails_rather_than_skipping(self):
        """reference/i915/ is vendored at a pinned tag and can go missing.

        Both None and empty must refuse: an earlier guard elsewhere in this
        tree tested only None and let the empty dict through, which is what a
        truncated or zero-byte vendored file actually looks like.
        """
        kext, _src = self.fresh()
        for corpus in ({}, None):
            with self.subTest(corpus=repr(corpus)):
                rep = self.run_fb(kext, corpus)
                named = [f for f in rep.failures if "i915" in f[0]]
                self.assertTrue(
                    named,
                    "an empty i915 corpus produced no failure naming it")
                self.assertEqual([], rep.skipped, len(rep.skipped))


if __name__ == "__main__":
    unittest.main(verbosity=2)
