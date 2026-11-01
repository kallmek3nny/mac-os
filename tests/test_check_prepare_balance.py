#!/usr/bin/env python3
"""
test_check_prepare_balance.py -- negative controls for
``tools/check-prepare-balance.py``, plus a positive control over the real
``phase0-kext/``.

Why this exists
---------------
The checker printed ``OK -- 8 checks`` on its first run against the real tree.
So does a body of ``return 0``. This project has already been burned by gates
that went green immediately and turned out to be green for the wrong reason: a
negative control whose input never reached the line it aimed at, a regex that
could not match the construct it was written for, two sides of an assertion
computed from the same source so they agreed no matter which was wrong.

So every check in that script gets a synthetic tree where THAT check, and as far
as possible only that check, is broken. A mutation that does not turn the run red
means the corresponding check is decoration.

The fixture is asserted clean FIRST (``GoodTreePasses``). Without that, every
mutation below could be red for some unrelated malformation of the fixture --
which is indistinguishable, from the outside, from successfully catching the
mutation.

What the mutations model
------------------------
The defect this checker exists for was real, and was in this driver on
2026-08-13: ``releaseGspFirmware()`` called ``complete()`` unconditionally, and
``loadGspFirmware()`` calls it from every failure path -- including the one whose
whole purpose is "``prepare()`` returned non-success". Every mutation here shares
the property that made it survive four gates: none is a compile error, none is a
parse error, and the damage lands in a kernel wire count that nothing in this
project prints.

The two directions matter and are tested separately. Setting the flag too late
LEAKS wired pages; clearing it too late DOUBLE-COMPLETES them. A checker that
only caught one direction would read as complete coverage.

Known limitation, asserted rather than hidden
---------------------------------------------
``strip_comments()`` deliberately preserves string literals, so a ``->complete(``
appearing inside a string would be read as a real call and flagged. That is the
safe direction to be wrong in -- it fails loudly instead of quietly excusing an
unguarded call -- and there is no such literal in the driver. The comment case,
which WOULD be a silent false pass if mishandled, is tested below.

Verdict word, not exit code
---------------------------
Every assertion reads the literal ``FAILED`` or ``OK`` out of the output. A
checker that crashes before reaching its verdict also exits non-zero, and "it
exited 1" would accept a traceback as proof the mutation was caught when nothing
was ever evaluated.
"""

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHECKER = os.path.join(ROOT, "tools", "check-prepare-balance.py")


def load_checker_module():
    """Import the checker by path; its filename has dashes in it."""
    spec = importlib.util.spec_from_file_location("check_prepare_balance", CHECKER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


#
# The synthetic driver. Deliberately minimal: enough shape for the checker to
# reason about, and nothing else, so a mutation to one line cannot accidentally
# disturb an unrelated check.
#
GOOD_HPP = """\
class FakeDriver : public IOService
{
    IOBufferMemoryDescriptor *fBuf;
    bool fPrepared;
};
"""

GOOD_INIT = """\
bool FakeDriver::init(OSDictionary *dict)
{
    fBuf = nullptr;
    fPrepared = false;
    return true;
}
"""

GOOD_LOAD = """\
bool FakeDriver::load(void)
{
    if (!fBuf) {
        return false;
    }

    if (fBuf->prepare(kIODirectionOut) != kIOReturnSuccess) {
        teardown();
        return false;
    }

    fPrepared = true;

    return true;
}
"""

GOOD_TEARDOWN = """\
void FakeDriver::teardown(void)
{
    if (fBuf) {
        if (fPrepared) {
            fPrepared = false;
            fBuf->complete(kIODirectionOut);
        }
        OSSafeReleaseNULL(fBuf);
    }
}
"""


def good_cpp():
    return GOOD_INIT + "\n" + GOOD_LOAD + "\n" + GOOD_TEARDOWN


class TreeBuilder(unittest.TestCase):
    """Builds a throwaway ``phase0-kext/`` and runs the checker over its root."""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="preparebalance_")
        self.addCleanup(shutil.rmtree, self.root, True)
        self.kext = os.path.join(self.root, "phase0-kext")
        os.makedirs(self.kext)

    def write(self, cpp=None, hpp=GOOD_HPP):
        if cpp is None:
            cpp = good_cpp()
        if cpp is not False:
            self._text("FakeDriver.cpp", cpp)
        if hpp is not False:
            self._text("FakeDriver.hpp", hpp)

    def _text(self, name, body):
        with open(os.path.join(self.kext, name), "w", encoding="utf-8") as handle:
            handle.write(body)

    def run_checker(self):
        proc = subprocess.run(
            [sys.executable, CHECKER, self.root],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)
        return proc.returncode, proc.stdout

    def assertRejected(self, needle):
        code, out = self.run_checker()
        self.assertNotEqual(code, 0, "checker accepted the mutation:\n%s" % out)
        self.assertIn("check-prepare-balance: FAILED", out,
                      "no verdict line -- the checker died rather than "
                      "rejecting, so this control proves nothing:\n%s" % out)
        self.assertIn(needle, out,
                      "rejected, but for a reason that does not mention %r, so "
                      "this control does not show the intended check fired:\n%s"
                      % (needle, out))
        return out

    def assertAccepted(self):
        code, out = self.run_checker()
        self.assertEqual(code, 0, "checker rejected a good tree:\n%s" % out)
        self.assertIn("check-prepare-balance: OK", out, out)
        return out


class GoodTreePasses(TreeBuilder):
    """The control on the controls. If this is red, nothing below means anything."""

    def test_the_unmutated_fixture_is_accepted(self):
        self.write()
        self.assertAccepted()

    def test_the_verdict_reports_a_positive_check_count(self):
        """``OK -- 0 checks`` is what a checker that examined nothing prints."""
        self.write()
        out = self.assertAccepted()
        digits = [int(w) for w in out.replace("--", " ").split() if w.isdigit()]
        self.assertTrue(digits, "verdict line carries no count: %s" % out)
        self.assertGreater(max(digits), 3, out)

    def test_a_commented_out_unguarded_complete_is_not_parsed(self):
        """The silent-false-pass direction of comment handling.

        If ``strip_comments()`` were skipped entirely this would go red, which is
        harmless. The dangerous direction is the opposite one -- a checker that
        treats a comment as code would train the operator to ignore it.
        """
        self.write(cpp=good_cpp() +
                   "\n// fBuf->complete(kIODirectionOut);  // no guard here\n"
                   "/*\n * fBuf->complete(kIODirectionOut);\n */\n")
        self.assertAccepted()


class TheDefectThatWasReal(TreeBuilder):
    """The 2026-08-13 defect itself, reintroduced."""

    def test_an_unguarded_complete_is_caught(self):
        cpp = good_cpp().replace(
            "        if (fPrepared) {\n"
            "            fPrepared = false;\n"
            "            fBuf->complete(kIODirectionOut);\n"
            "        }\n",
            "        fBuf->complete(kIODirectionOut);\n")
        self.assertNotEqual(cpp, good_cpp(),
                            "the mutation did not apply -- the anchor text no "
                            "longer matches the fixture, so this test would "
                            "pass against an unmutated tree")
        self.write(cpp=cpp)
        self.assertRejected("is not inside an `if (fPrepared)`")

    def test_a_complete_with_no_prepare_anywhere_is_caught(self):
        """The rawest form: a decrement with no increment in the whole file."""
        cpp = good_cpp().replace(
            "    if (fBuf->prepare(kIODirectionOut) != kIOReturnSuccess) {\n"
            "        teardown();\n"
            "        return false;\n"
            "    }\n", "")
        self.assertNotIn("prepare(", cpp)
        self.write(cpp=cpp)
        self.assertRejected("ever calls fBuf->prepare()")


class TheTwoOppositeErrors(TreeBuilder):
    """Setting the flag too late leaks; clearing it too late double-completes.

    Both are tested because a checker that caught only one would look complete.
    """

    def test_a_bail_out_between_prepare_and_the_flag_is_caught(self):
        """The LEAK direction. Pages are wired with nothing recording it."""
        cpp = good_cpp().replace(
            "    fPrepared = true;\n",
            "    if (!checkSomethingElse()) {\n"
            "        return false;\n"
            "    }\n\n"
            "    fPrepared = true;\n")
        self.write(cpp=cpp)
        self.assertRejected("there is a `return` between")

    def test_clearing_the_flag_after_the_complete_is_caught(self):
        """The DOUBLE-COMPLETE direction. A re-entrant teardown sees it true."""
        cpp = good_cpp().replace(
            "            fPrepared = false;\n"
            "            fBuf->complete(kIODirectionOut);\n",
            "            fBuf->complete(kIODirectionOut);\n"
            "            fPrepared = false;\n")
        self.assertNotEqual(cpp, good_cpp(), "the mutation did not apply")
        self.write(cpp=cpp)
        self.assertRejected("does not appear between")


class FlagLifetime(TreeBuilder):
    def test_a_flag_that_is_never_initialised_in_init_is_caught(self):
        """free() can run on an instance whose start() never staged anything."""
        cpp = good_cpp().replace("    fPrepared = false;\n    return true;\n",
                                 "    return true;\n")
        self.assertNotEqual(cpp, good_cpp(), "the mutation did not apply")
        self.write(cpp=cpp)
        self.assertRejected("does not appear in ::init()")

    def test_a_flag_that_is_not_a_member_is_caught(self):
        """A local resets to garbage and never survives to the teardown."""
        self.write(hpp=GOOD_HPP.replace("    bool fPrepared;\n", ""))
        self.assertRejected("is not declared in any of")

    def test_no_flag_at_all_after_prepare_is_caught(self):
        cpp = good_cpp().replace("    fPrepared = true;\n", "")
        self.assertNotEqual(cpp, good_cpp(), "the mutation did not apply")
        self.write(cpp=cpp)
        self.assertRejected("nothing is set true after")


class ReleaseOrdering(TreeBuilder):
    def test_a_buffer_never_released_after_completing_is_caught(self):
        cpp = good_cpp().replace("        OSSafeReleaseNULL(fBuf);\n", "")
        self.assertNotEqual(cpp, good_cpp(), "the mutation did not apply")
        self.write(cpp=cpp)
        self.assertRejected("no release of fBuf appears after")

    def test_a_prepare_with_no_complete_anywhere_is_caught(self):
        """The leak in its rawest form -- an increment with no decrement."""
        cpp = good_cpp().replace(
            "            fBuf->complete(kIODirectionOut);\n", "")
        self.assertNotIn("complete(", cpp)
        self.write(cpp=cpp)
        self.assertRejected("no complete() on fBuf")


class TheSecondPairKind(TreeBuilder):
    """``setMemoryDescriptor``/``clearMemoryDescriptor``, the pair the first
    version of the checker could not see.

    This is the reason ``ACQUIRE_RELEASE_PAIRS`` is a table rather than two
    hardcoded method names. The same defect sat four lines below the one the
    checker was written for -- same object, same teardown, same failure path --
    and went green. Without the mutations below, the table entry is a claim with
    nothing behind it: the checker would report a larger check count and catch
    exactly as much as before.

    The fixture here uses a DIFFERENT receiver and a DIFFERENT flag name from
    the prepare/complete fixture, so a checker that had somehow bound itself to
    the first pair's identifiers cannot pass these by accident.
    """

    HPP = """\
class FakeDriver : public IOService
{
    IODMACommand *fDma;
    bool fHasDescriptor;
};
"""

    INIT = """\
bool FakeDriver::init(OSDictionary *dict)
{
    fDma = nullptr;
    fHasDescriptor = false;
    return true;
}
"""

    LOAD = """\
bool FakeDriver::load(void)
{
    if (!fDma) {
        return false;
    }

    if (fDma->setMemoryDescriptor(fBuf, true) != kIOReturnSuccess) {
        teardown();
        return false;
    }

    fHasDescriptor = true;

    return true;
}
"""

    TEARDOWN = """\
void FakeDriver::teardown(void)
{
    if (fDma) {
        if (fHasDescriptor) {
            fHasDescriptor = false;
            fDma->clearMemoryDescriptor(true);
        }
        OSSafeReleaseNULL(fDma);
    }
}
"""

    def good(self):
        return self.INIT + "\n" + self.LOAD + "\n" + self.TEARDOWN

    def test_the_unmutated_second_pair_fixture_is_accepted(self):
        """Asserted first: otherwise every mutation below could be red for an
        unrelated malformation of THIS fixture, which is a different fixture
        from the one GoodTreePasses cleared."""
        self.write(cpp=self.good(), hpp=self.HPP)
        self.assertAccepted()

    def test_an_unguarded_clear_is_caught(self):
        """The defect as it actually was in NV3050Probe.cpp before 2026-08-13."""
        cpp = self.good().replace(
            "        if (fHasDescriptor) {\n"
            "            fHasDescriptor = false;\n"
            "            fDma->clearMemoryDescriptor(true);\n"
            "        }\n",
            "        fDma->clearMemoryDescriptor(true);\n")
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("is not inside an `if (fHasDescriptor)`")

    def test_guarding_on_the_object_instead_of_the_flag_is_caught(self):
        """The exact wrong guard the driver had: `if (fDma)`.

        This is the separating input for this whole pair. An unguarded call is
        obviously wrong; a call guarded on the OBJECT looks defended and is not,
        because the object exists from the moment withSpecification() returns.
        A checker that only tested the unguarded form would pass this.
        """
        cpp = self.good().replace(
            "        if (fHasDescriptor) {\n"
            "            fHasDescriptor = false;\n"
            "            fDma->clearMemoryDescriptor(true);\n"
            "        }\n",
            "        if (fDma) {\n"
            "            fDma->clearMemoryDescriptor(true);\n"
            "        }\n")
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("is not inside an `if (fHasDescriptor)`")

    def test_clearing_the_flag_after_the_call_is_caught(self):
        cpp = self.good().replace(
            "            fHasDescriptor = false;\n"
            "            fDma->clearMemoryDescriptor(true);\n",
            "            fDma->clearMemoryDescriptor(true);\n"
            "            fHasDescriptor = false;\n")
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("does not appear between")

    def test_a_bail_out_before_the_flag_is_caught(self):
        """The leak direction for this pair: a descriptor held by nothing."""
        cpp = self.good().replace(
            "    fHasDescriptor = true;\n",
            "    if (!somethingElse()) {\n"
            "        return false;\n"
            "    }\n\n"
            "    fHasDescriptor = true;\n")
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("there is a `return` between")

    def test_the_failure_names_what_the_guard_would_assert(self):
        """"Add a guard" is not actionable without saying what it asserts.

        The third field of the pair table exists for this, so it has to reach
        the output rather than being a comment that happens to live in a tuple.
        """
        cpp = self.good().replace(
            "        if (fHasDescriptor) {\n"
            "            fHasDescriptor = false;\n"
            "            fDma->clearMemoryDescriptor(true);\n"
            "        }\n",
            "        fDma->clearMemoryDescriptor(true);\n")
        self.write(cpp=cpp, hpp=self.HPP)
        out = self.assertRejected("taken ownership of a descriptor")
        self.assertIn("withSpecification", out,
                      "the failure does not explain why `if (theCommand)` is "
                      "not a guard, which is the whole reason this pair was "
                      "missed:\n%s" % out)


class TheOpenClosePair(TreeBuilder):
    """``open``/``close`` on the provider, added by the t-d1480a audit.

    Both real callers retain the provider in start() BEFORE trying open(), and
    open() can refuse ("provider already owned"), so the provider pointer exists
    on exactly the path where nothing was opened. ``if (fProvider)`` is
    therefore the wrong guard for close() in the same way ``if (fDma)`` is for
    clearMemoryDescriptor(). Both real teardowns also cleared their flag AFTER
    close(), which is the double-release direction.

    Receiver and flag names differ from both other fixtures, so a checker bound
    to their identifiers cannot pass these by accident.
    """

    HPP = """\
class FakeDriver : public IOService
{
    IOPCIDevice *fProvider;
    bool fHoldsOpen;
};
"""

    INIT = """\
bool FakeDriver::init(OSDictionary *dict)
{
    fProvider = nullptr;
    fHoldsOpen = false;
    return true;
}
"""

    START = """\
bool FakeDriver::start(IOService *provider)
{
    fProvider = OSDynamicCast(IOPCIDevice, provider);
    if (!fProvider) {
        return false;
    }
    fProvider->retain();

    if (!fProvider->open(this)) {
        teardown();
        return false;
    }

    fHoldsOpen = true;

    return true;
}
"""

    TEARDOWN = """\
void FakeDriver::teardown(void)
{
    if (fProvider) {
        if (fHoldsOpen) {
            fHoldsOpen = false;
            fProvider->close(this);
        }
        OSSafeReleaseNULL(fProvider);
    }
}
"""

    GUARDED_ON_THE_PROVIDER = """\
void FakeDriver::teardown(void)
{
    if (fProvider) {
        fProvider->close(this);
        OSSafeReleaseNULL(fProvider);
    }
}
"""

    def good(self):
        return self.INIT + "\n" + self.START + "\n" + self.TEARDOWN

    def test_the_unmutated_open_close_fixture_is_accepted(self):
        """Asserted first, for the same reason as the second pair's fixture."""
        self.write(cpp=self.good(), hpp=self.HPP)
        self.assertAccepted()

    def test_an_unguarded_close_is_caught(self):
        cpp = self.good().replace(
            self.TEARDOWN,
            "void FakeDriver::teardown(void)\n"
            "{\n"
            "    fProvider->close(this);\n"
            "    OSSafeReleaseNULL(fProvider);\n"
            "}\n")
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("is not inside an `if (fHoldsOpen)`")

    def test_guarding_on_the_provider_instead_of_the_flag_is_caught(self):
        """The separating input for this pair: ``if (fProvider)`` reads as
        defended, and is true on the path where open() refused."""
        cpp = self.good().replace(self.TEARDOWN, self.GUARDED_ON_THE_PROVIDER)
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("is not inside an `if (fHoldsOpen)`")

    def test_clearing_the_flag_after_the_close_is_caught(self):
        """The order both real teardowns had before t-d1480a."""
        cpp = self.good().replace(
            "            fHoldsOpen = false;\n"
            "            fProvider->close(this);\n",
            "            fProvider->close(this);\n"
            "            fHoldsOpen = false;\n")
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("does not appear between")

    def test_a_bail_out_before_the_flag_is_caught(self):
        """The leak direction: the provider held open with nothing recording
        it, so the teardown skips close() and the provider cannot terminate."""
        cpp = self.good().replace(
            "    fHoldsOpen = true;\n",
            "    if (!somethingElse()) {\n"
            "        return false;\n"
            "    }\n\n"
            "    fHoldsOpen = true;\n")
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("there is a `return` between")

    def test_a_close_with_no_open_anywhere_is_caught(self):
        cpp = self.good().replace(
            "    if (!fProvider->open(this)) {\n",
            "    if (!fProvider->isOpen(this)) {\n")
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("ever calls fProvider->open()")

    def test_an_open_with_no_close_anywhere_is_caught(self):
        cpp = self.good().replace(
            "        if (fHoldsOpen) {\n"
            "            fHoldsOpen = false;\n"
            "            fProvider->close(this);\n"
            "        }\n",
            "")
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("no close() on fProvider")

    def test_the_failure_names_what_the_guard_would_assert(self):
        cpp = self.good().replace(self.TEARDOWN, self.GUARDED_ON_THE_PROVIDER)
        self.write(cpp=cpp, hpp=self.HPP)
        out = self.assertRejected("holds its provider open")
        self.assertIn("open() succeeded", out,
                      "the failure does not explain why `if (theProvider)` is "
                      "not a guard:\n%s" % out)


class TheFieldFlag(TreeBuilder):
    """A flag kept in a separately allocated struct: ``state->dmaBound``.

    IntelGtBringupSurface.cpp keeps its IODMACommand in an IOMalloc'd runtime
    struct and records the bind there. Before the checker knew field flags it
    skipped ``state->dmaBound = true;``, derived the next bare ``x = true;`` in
    the function instead (``fQuarantined``), and reported three findings
    against a flag that has nothing to do with the descriptor: a ``return``
    before it, no ``::init()`` to initialise it in, and a release not guarded
    by it. This fixture carries the same decoy, after the same kind of
    bail-out, in a file with no ``init()``. So the unmutated fixture going
    green IS the mutation for that checker fix: take the field-flag
    derivation out and it reports exactly those three again, which
    ``test_a_field_flag_on_another_object_is_not_this_pairs_flag`` shows.

    The rule is narrow on purpose: a field flag counts only on the object that
    holds the receiver, and that object must be zeroed before the acquire,
    because IOMalloc() memory is not.
    """

    HPP = """\
struct FakeRuntimeState
{
    IODMACommand *dma;
    bool dmaBound;
};

class FakeDriver : public IOService
{
    FakeRuntimeState *fState;
    bool fQuarantined;
};
"""

    STAGE = """\
bool FakeDriver::stage(void)
{
    FakeRuntimeState *state = (FakeRuntimeState *)IOMalloc(sizeof(*state));
    if (!state) {
        return false;
    }
    bzero(state, sizeof(*state));
    fState = state;

    state->dma = makeCommand();
    if (!state->dma) {
        teardown(state);
        return false;
    }

    if (state->dma->setMemoryDescriptor(fBuf, true) != kIOReturnSuccess) {
        teardown(state);
        return false;
    }

    state->dmaBound = true;

    if (!publish(state)) {
        teardown(state);
        return false;
    }

    fQuarantined = true;

    return true;
}
"""

    TEARDOWN = """\
void FakeDriver::teardown(FakeRuntimeState *state)
{
    if (state->dmaBound) {
        state->dmaBound = false;
        state->dma->clearMemoryDescriptor();
    }
    OSSafeReleaseNULL(state->dma);
    IOFree(state, sizeof(*state));
}
"""

    GUARDED_BLOCK = ("    if (state->dmaBound) {\n"
                     "        state->dmaBound = false;\n"
                     "        state->dma->clearMemoryDescriptor();\n"
                     "    }\n")

    def good(self):
        return self.STAGE + "\n" + self.TEARDOWN

    def mutated(self, old, new):
        cpp = self.good().replace(old, new)
        self.assertNotEqual(cpp, self.good(), "the mutation did not apply")
        return cpp

    def test_the_unmutated_field_flag_fixture_is_accepted(self):
        """Asserted first, for the same reason as the other pair fixtures."""
        self.write(cpp=self.good(), hpp=self.HPP)
        self.assertAccepted()

    def test_a_field_flag_on_another_object_is_not_this_pairs_flag(self):
        """``fOther->dmaBound`` records some other object's bind. Accepting it
        would let any struct's flag excuse this command's release. Refused,
        the derivation falls through to the decoy and reproduces the three
        findings IntelGtBringupSurface.cpp had before field flags existed."""
        cpp = self.mutated("    state->dmaBound = true;\n",
                           "    fOther->dmaBound = true;\n")
        self.write(cpp=cpp, hpp=self.HPP)
        out = self.assertRejected("and `fQuarantined = true;`")
        self.assertIn("no ::init() body found", out, out)
        self.assertIn("is not inside an `if (fQuarantined)`", out, out)

    def test_an_unzeroed_holder_is_caught(self):
        """IOMalloc() is not IOMallocZero(): the flag starts as garbage, and
        the teardown runs on the failure path of every step after it."""
        cpp = self.mutated("    bzero(state, sizeof(*state));\n", "")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected(
            "nothing before the dma->setMemoryDescriptor() zeroes `state`")

    def test_the_compound_guard_the_real_teardown_had_is_caught(self):
        """IntelGtBringupSurface.cpp before t-73c1f8: ``if (flag && object)``
        with the flag cleared after the call."""
        cpp = self.mutated(
            self.GUARDED_BLOCK,
            "    if (state->dmaBound && state->dma) {\n"
            "        state->dma->clearMemoryDescriptor();\n"
            "        state->dmaBound = false;\n"
            "    }\n")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected(
            "dma->clearMemoryDescriptor() is not inside an "
            "`if (state->dmaBound)`")

    def test_clearing_the_field_flag_after_the_call_is_caught(self):
        cpp = self.mutated(
            "        state->dmaBound = false;\n"
            "        state->dma->clearMemoryDescriptor();\n",
            "        state->dma->clearMemoryDescriptor();\n"
            "        state->dmaBound = false;\n")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("`state->dmaBound = false;` does not appear between")

    def test_a_bail_out_before_the_field_flag_is_caught(self):
        cpp = self.mutated(
            "    state->dmaBound = true;\n",
            "    if (!somethingElse()) {\n"
            "        return false;\n"
            "    }\n\n"
            "    state->dmaBound = true;\n")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("and `state->dmaBound = true;`")

    def test_a_field_flag_that_is_not_a_bool_member_is_caught(self):
        hpp = self.HPP.replace("    bool dmaBound;\n", "    UInt32 dmaBound;\n")
        self.assertNotEqual(hpp, self.HPP, "the mutation did not apply")
        self.write(cpp=self.good(), hpp=hpp)
        self.assertRejected("`bool dmaBound;` is not declared")

    def test_a_command_never_dropped_after_its_clear_is_caught(self):
        cpp = self.mutated("    OSSafeReleaseNULL(state->dma);\n", "")
        self.write(cpp=cpp, hpp=self.HPP)
        self.assertRejected("no release of state->dma appears after")


class PairsAreDistinguished(TreeBuilder):
    """``prepare`` must not match ``prepareForDMA``.

    They are separate entries with separate releases. If the acquire pattern
    matched a prefix, a ``prepareForDMA()`` would be read as a ``prepare()``
    whose ``complete()`` is missing, and the checker would report a fabricated
    defect on correct code -- which is how a gate gets switched off.
    """

    HPP = """\
class FakeDriver : public IOService
{
    IODMACommand *fDma;
    bool fDmaLive;
};
"""

    def test_prepare_for_dma_is_not_read_as_a_bare_prepare(self):
        cpp = ("bool FakeDriver::init(OSDictionary *d)\n"
               "{\n"
               "    fDma = nullptr;\n"
               "    fDmaLive = false;\n"
               "    return true;\n"
               "}\n"
               "\n"
               "bool FakeDriver::load(void)\n"
               "{\n"
               "    if (fDma->prepareForDMA(0) != kIOReturnSuccess) {\n"
               "        teardown();\n"
               "        return false;\n"
               "    }\n"
               "\n"
               "    fDmaLive = true;\n"
               "\n"
               "    return true;\n"
               "}\n"
               "\n"
               "void FakeDriver::teardown(void)\n"
               "{\n"
               "    if (fDma) {\n"
               "        if (fDmaLive) {\n"
               "            fDmaLive = false;\n"
               "            fDma->completeDMA(0);\n"
               "        }\n"
               "        OSSafeReleaseNULL(fDma);\n"
               "    }\n"
               "}\n")
        self.write(cpp=cpp, hpp=self.HPP)
        out = self.assertAccepted()
        self.assertNotIn("complete() on", out, out)


class RefusalPaths(TreeBuilder):
    """Conditions under which no check can run. These must not read as passes.

    A checker that cannot find its subject and prints OK is worse than no checker
    at all: it converts an unexamined file into a green tick.
    """

    def test_a_prepare_outside_a_failure_check_is_a_refusal(self):
        """With no `if (...) { }` around it there is no anchor for the flag."""
        self.write(cpp=GOOD_INIT + "\n"
                   "bool FakeDriver::load(void)\n"
                   "{\n"
                   "    fBuf->prepare(kIODirectionOut);\n"
                   "    fBuf->complete(kIODirectionOut);\n"
                   "    return true;\n"
                   "}\n")
        self.assertRejected("is not inside a recognisable")

    def test_a_tree_with_no_sources_is_a_failure(self):
        self.write(cpp=False)
        self.assertRejected("nothing was examined")

    def test_a_tree_with_no_wiring_at_all_is_reported_not_passed(self):
        """Legitimate state, but indistinguishable from a dead regex from here."""
        self.write(cpp="bool FakeDriver::init(OSDictionary *d) { return true; }\n")
        self.assertRejected("stopped matching")


class HelperUnits(unittest.TestCase):
    """The parsing helpers, driven directly."""

    @classmethod
    def setUpClass(cls):
        cls.mod = load_checker_module()

    def test_strip_comments_preserves_line_numbering(self):
        """Line numbers are what the operator opens the file at."""
        text = "a\n// b\n/* c\n d */\ne\n"
        self.assertEqual(len(self.mod.strip_comments(text).split("\n")),
                         len(text.split("\n")))

    def test_strip_comments_removes_a_line_comment_but_keeps_code(self):
        stripped = self.mod.strip_comments("call(); // fBuf->complete(x);\n")
        self.assertIn("call();", stripped)
        self.assertNotIn("complete", stripped)

    def test_a_slash_slash_inside_a_string_does_not_start_a_comment(self):
        """An NVLOG format string must not swallow the rest of its line."""
        stripped = self.mod.strip_comments('IOLog("a//b"); keep();\n')
        self.assertIn("keep();", stripped)

    def test_enclosing_openers_excludes_the_line_that_opens_the_block(self):
        lines = ["if (x) {", "    body;", "}"]
        openers = self.mod.enclosing_openers(lines)
        self.assertEqual(openers[0], [])
        self.assertEqual(openers[1], [0])

    def test_block_close_returns_none_when_no_block_opens(self):
        """This is the refusal path's trigger; it has to be reachable."""
        self.assertIsNone(self.mod.block_close(["plain();", "}"], 0))

    def test_block_close_finds_the_matching_brace_not_the_first_one(self):
        lines = ["if (a) {", "    if (b) {", "    }", "}"]
        self.assertEqual(self.mod.block_close(lines, 0), 3)


#
# The edits made to the real driver for t-73c1f8 and t-d1480a, each paired
# with its own revert.
#
REAL_KEXT = os.path.join(ROOT, "phase0-kext")


def flag_first(indent, flag, call):
    """``if (flag) { flag=false; call; }`` -- the shape every fix landed on."""
    pad = " " * indent
    return ("%sif (%s) {\n%s    %s=false;\n%s    %s;\n%s}\n"
            % (pad, flag, pad, flag, pad, call, pad))


def released_first(indent, cond, flag, call):
    """``if (cond) { call; flag=false; }`` -- the order every teardown had."""
    pad = " " * indent
    return ("%sif (%s) {\n%s    %s;\n%s    %s=false;\n%s}\n"
            % (pad, cond, pad, call, pad, flag, pad))


def teardown_reverts(source, flag, obj, method, args,
                     indent=4, was=None, was_fixed=None):
    """The two halves of one flag-before-release fix, reverted separately.

    ``was`` is the teardown exactly as it stood before the fix -- by default
    the compound ``if (flag && obj)`` guard that hid the order from the
    checker. The second row keeps the plain ``if (flag)`` the fix introduced
    and puts back only the ORDER, so that half is shown to be load-bearing on
    its own rather than riding on the guard.
    """
    call = "%s->%s(%s)" % (obj, method, args)
    fixed = flag_first(indent, flag, call)
    if was is None:
        was = released_first(indent, "%s && %s" % (flag, obj), flag, call)
    receiver = obj.split("->")[-1]
    return (
        (source, "%s teardown as it was" % flag, was_fixed or fixed, was,
         "%s->%s() is not inside an `if (%s)`" % (receiver, method, flag)),
        (source, "%s cleared after %s() again" % (flag, method), fixed,
         released_first(indent, flag, flag, call),
         "`%s = false;` does not appear between" % flag),
    )


#
# (source, what is reverted, the fix as it stands, the fix reverted, what the
# checker must say.)
#
# Real defects: every teardown row. Each had the flag cleared AFTER its
# release, mostly behind a compound ``if (flag && object)`` guard the checker
# could not read. False positives fixed in the source: the shape rows at the
# end. The three false positives fixed in the CHECKER (the
# IntelGtBringupSurface.cpp field flag) have no row: reverting a checker change
# is not a source edit, so TheFieldFlag carries that mutation.
#
REVERTED_FIXES = (
    teardown_reverts("NVGspWprBootUcodeStageService.cpp",
                     "fDMABound", "fDMA", "clearMemoryDescriptor", "")
    + teardown_reverts("NVGspWprMetaStageService.cpp",
                       "fDMABound", "fDMA", "clearMemoryDescriptor", "")
    + teardown_reverts("NVGspRmImageStageService.cpp",
                       "fDMABound", "fDMA", "clearMemoryDescriptor", "")
    + teardown_reverts("NVGspQueueBackingService.cpp",
                       "fDMABound", "fDMA", "clearMemoryDescriptor", "")
    + teardown_reverts("IntelGtSurfaceStageService.cpp",
                       "fDMABound", "fDMA", "clearMemoryDescriptor", "")
    + teardown_reverts("IntelSinkCandidateService.cpp",
                       "fPeerDMABound", "fPeerDMA", "clearMemoryDescriptor", "")
    + teardown_reverts("IntelSinkCandidateService.cpp",
                       "fDMABound", "fIntelDMA", "clearMemoryDescriptor", "")
    + teardown_reverts("IntelSinkCandidateService.cpp",
                       "fPrepared", "fBacking", "complete", "kIODirectionInOut")
    + teardown_reverts(
        "IntelSinkCandidateService.cpp", "fOpened", "fPCIDevice", "close",
        "this", indent=8,
        was_fixed=("    if (fPCIDevice) {\n"
                   + flag_first(8, "fOpened", "fPCIDevice->close(this)")
                   + "    }\n"),
        was=released_first(4, "fOpened && fPCIDevice", "fOpened",
                           "fPCIDevice->close(this)"))
    + teardown_reverts("NVGspBootArgsRehearsalService.cpp",
                       "fLibosBound", "fLibosDMA", "clearMemoryDescriptor", "")
    + teardown_reverts("NVGspBootArgsRehearsalService.cpp",
                       "fRmArgsBound", "fRmArgsDMA", "clearMemoryDescriptor", "")
    + teardown_reverts("IntelGtBringupSurface.cpp",
                       "state->dmaBound", "state->dma",
                       "clearMemoryDescriptor", "")
    + teardown_reverts(
        "IntelGtBringup.cpp", "fDMABound", "fDMA", "clearMemoryDescriptor", "",
        was=("    if (fDMABound) { fDMA->clearMemoryDescriptor(); "
             "fDMABound=false; }\n"))
    + teardown_reverts(
        "IntelGtBringup.cpp", "fPrepared", "fPool", "complete",
        "kIODirectionInOut",
        was=("    if (fPrepared) { fPool->complete(kIODirectionInOut); "
             "fPrepared=false; }\n"))
    + teardown_reverts(
        "IntelGtBringup.cpp", "fOpened", "fPCI", "close", "this",
        was="    if (fOpened) { fPCI->close(this); fOpened=false; }\n")
    + (
        #
        # Shape rewrites: false positives cleared by putting the source in a
        # form this checker can read. Reverting one must still redden;
        # otherwise the rewrite was not what cleared the finding.
        #
        ("NVGspBootArgsRehearsalService.cpp",
         "fRmArgsBound no longer set beside its bind",
         "    fRmArgsBound=true;\n", "",
         "there is a `return` between the fRmArgsDMA->setMemoryDescriptor() "
         "success check"),
        ("IntelGtBringup.cpp", "init() flags back on one line",
         "    fOpened=false;\n    fPrepared=false;\n"
         "    fDMABound=false;\n    fPCIChanged=false;\n",
         "    fOpened=false; fPrepared=false; fDMABound=false; "
         "fPCIChanged=false;\n",
         "`fPrepared = false;` does not appear in ::init()"),
        #
        # A one-line `if (...) return false;` gives block_close() no block to
        # delimit, so the flag derivation runs on into some later block. WHICH
        # check fires then depends on where it lands, so these two assert only
        # that a failure names the receiver. The single-source baseline is
        # what makes that enough: nothing else in the run can fail.
        #
        ("IntelGtBringup.cpp", "prepare() failure check back on one line",
         "    if (!fPool || fPool->prepare(kIODirectionInOut)"
         "!=kIOReturnSuccess) {\n        return false;\n    }\n",
         "    if (!fPool || fPool->prepare(kIODirectionInOut)"
         "!=kIOReturnSuccess) return false;\n",
         "fPool->"),
        ("IntelGtBringup.cpp",
         "setMemoryDescriptor() failure check back on one line",
         "    if (!fDMA || fDMA->setMemoryDescriptor(fPool,true)"
         "!=kIOReturnSuccess) {\n        return false;\n    }\n",
         "    if (!fDMA || fDMA->setMemoryDescriptor(fPool,true)"
         "!=kIOReturnSuccess) return false;\n",
         "fDMA->"),
    )
)


class RevertedFixes(unittest.TestCase):
    """Every t-73c1f8 / t-d1480a edit to the real driver, reverted one at a time.

    RealTree shows the driver is accepted as it stands. It cannot show WHICH
    edits that acceptance depends on: a fix the checker never looked at would
    leave it just as green. So each row puts its own edit back, in an
    otherwise real tree, and the checker has to go red for the reason the row
    names.

    Each run holds every real header and ONE real source, and that source is
    asserted clean on its own first. A red run can then only come from the
    revert -- not from another file, and not from a source that was already
    failing. The fixed text must occur exactly once: if the driver moves on,
    the row fails as "occurs N time(s)" instead of quietly testing nothing.
    """

    def run_alone(self, source, text):
        root = tempfile.mkdtemp(prefix="preparebalance_real_")
        self.addCleanup(shutil.rmtree, root, True)
        kext = os.path.join(root, "phase0-kext")
        os.makedirs(kext)
        for name in os.listdir(REAL_KEXT):
            if name.endswith(".hpp"):
                shutil.copy(os.path.join(REAL_KEXT, name), kext)
        with open(os.path.join(kext, source), "w", encoding="utf-8",
                  errors="surrogateescape") as handle:
            handle.write(text)
        proc = subprocess.run(
            [sys.executable, CHECKER, root],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)
        return proc.returncode, proc.stdout

    def test_every_fix_reddens_when_reverted(self):
        baselines = {}
        for source, what, fixed, reverted, needle in REVERTED_FIXES:
            with self.subTest(source=source, revert=what):
                with open(os.path.join(REAL_KEXT, source), "r",
                          encoding="utf-8", errors="surrogateescape") as handle:
                    text = handle.read()

                if source not in baselines:
                    baselines[source] = self.run_alone(source, text)
                code, out = baselines[source]
                self.assertEqual(code, 0,
                                 "%s is not accepted on its own, so a red run "
                                 "below would prove nothing:\n%s"
                                 % (source, out))
                self.assertIn("check-prepare-balance: OK", out, out)

                count = text.count(fixed)
                self.assertEqual(
                    count, 1,
                    "the fixed text for %r occurs %d time(s) in %s, not once "
                    "-- the driver has moved on and this row must follow:\n%s"
                    % (what, count, source, fixed))

                code, out = self.run_alone(source,
                                           text.replace(fixed, reverted))
                self.assertNotEqual(code, 0,
                                    "reverting %r in %s left the checker "
                                    "green:\n%s" % (what, source, out))
                self.assertIn("check-prepare-balance: FAILED", out,
                              "no verdict line -- the checker died rather "
                              "than rejecting:\n%s" % out)
                self.assertIn(needle, out,
                              "reverting %r in %s was rejected, but not for "
                              "%r:\n%s" % (what, source, needle, out))


class RealTree(unittest.TestCase):
    """The positive control: the driver as it stands must be accepted."""

    def test_the_real_phase0_kext_is_accepted(self):
        proc = subprocess.run(
            [sys.executable, CHECKER, ROOT],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertIn("check-prepare-balance: OK", proc.stdout)

    def test_the_real_run_examined_something(self):
        """Guards against the whole gate passing over a file it never matched."""
        proc = subprocess.run(
            [sys.executable, CHECKER, ROOT],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)
        digits = [int(w) for w in proc.stdout.replace("--", " ").split()
                  if w.isdigit()]
        self.assertTrue(digits, proc.stdout)
        self.assertGreater(max(digits), 3, proc.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
