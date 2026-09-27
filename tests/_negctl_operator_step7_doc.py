#!/usr/bin/env python3
"""Negative control for test_operator_step7_doc.py -- can any of it fail?

Why this exists
---------------
That module went green on its first run against the real card. A doc gate that
is green the moment it is written is the single most likely kind of test to be
vacuous: every assertion is an ``assertIn`` against a large document, and a
large document contains a lot of strings by accident.

This mutates the card IN MEMORY, writes each mutant to a temp file, repoints the
module's ``DOC`` at it, and requires the named test to FAIL. It never touches
the real card, never touches the VMX, and never rebuilds the payload.

Every mutation asserts its anchor occurs a known number of times BEFORE
replacing. A mutation that matches nothing silently no-ops and the test it was
meant to break stays green, which reads as "the assertion is fine" when it means
"the probe missed".

Run:
    python tests/_negctl_operator_step7_doc.py
"""
import hashlib
import os
import re
import sys
import tempfile
import unittest

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_TESTS_DIR)
if _TESTS_DIR not in sys.path:
    sys.path.insert(0, _TESTS_DIR)

import test_operator_step7_doc as mod            # noqa: E402

DOC = mod.DOC
ISO = mod.ISO
STAGED_SH = mod.STAGED_SH


def original():
    with open(DOC, "r", encoding="utf-8") as handle:
        return handle.read()


def live_md5():
    with open(ISO, "rb") as handle:
        return hashlib.md5(handle.read()).hexdigest()


def run_one(test_name, klass, text):
    """Run ONE test against a mutated card. Returns True if it failed."""
    fd, path = tempfile.mkstemp(prefix="negctl-card-", suffix=".md")
    os.close(fd)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)

    saved = mod.DOC
    mod.DOC = path
    try:
        suite = unittest.TestSuite()
        suite.addTest(getattr(mod, klass)(test_name))
        result = unittest.TestResult()
        suite.run(result)
        broke = bool(result.failures or result.errors)
        skipped = bool(result.skipped)
        return broke, skipped
    finally:
        mod.DOC = saved
        os.unlink(path)


def mutate(text, anchor, replacement, expect_count=1):
    found = text.count(anchor)
    if found != expect_count:
        raise SystemExit(
            "PROBE BROKEN: anchor %r occurs %d times, expected %d -- the"
            " mutation would %s"
            % (anchor, found, expect_count,
               "no-op" if found == 0 else "hit the wrong places"))
    return text.replace(anchor, replacement)


def main():
    text = original()
    md5 = live_md5()
    sh_size = str(os.path.getsize(STAGED_SH))

    cases = []

    # 1. Stale script byte size -- the REAL 2026-08-14 defect.
    cases.append((
        "stale script byte size",
        "DerivedValueTests", "test_the_script_byte_size_the_card_quotes_is_current",
        mutate(text, sh_size, "6718", expect_count=text.count(sh_size)),
    ))

    # 2. Stale ISO md5 -- the other half of the real defect.
    cases.append((
        "stale ISO md5",
        "DerivedValueTests", "test_every_md5_the_card_quotes_can_be_accounted_for",
        mutate(text, md5, "5828e278581172ee4845e276bfff3560"),
    ))

    # 3. Wrong per-module test count.
    m = re.search(r"test_guest_build_kext\.py`?\s*\((?:\*\*)?(\d+)", text)
    if not m:
        raise SystemExit("PROBE BROKEN: cannot find the quoted count to mutate")
    cases.append((
        "wrong quoted test count",
        "QuotedTestCountTests", "test_each_quoted_count_matches_the_module",
        text[:m.start(1)] + "999" + text[m.end(1):],
    ))

    # 4. The stale-disc warning deleted.
    #
    # This one was WRONG on its first writing and the wrongness is worth keeping
    # recorded: the mutation was `[Ss]tale`, but the card spells it `STALE` in
    # the top warning block. The mutation therefore changed SOME occurrences and
    # left others, so the `mutant == text` no-op guard did not fire and the test
    # stayed green for the entirely innocent reason that the word was still
    # there. Case-insensitive now, with an explicit post-condition below.
    stale_free = re.sub(r"stale", "fresh", text, flags=re.IGNORECASE)
    if "stale" in stale_free.lower():
        raise SystemExit("PROBE BROKEN: 'stale' survives the mutation, so the"
                         " test below would stay green for the wrong reason")
    cases.append((
        "stale-disc warning removed",
        "SelfConsistencyTests", "test_it_tells_the_operator_the_guest_may_hold_a_stale_disc",
        stale_free,
    ))

    # 5. An UNMARKED "do not power-cycle" reintroduced far from any correction.
    cases.append((
        "unmarked do-not-power-cycle",
        "SelfConsistencyTests", "test_it_does_not_both_forbid_and_require_the_power_cycle",
        text + ("\n\n## Addendum\n\n" + ("Filler sentence. " * 60)
                + "\n\nDo not power-cycle the guest.\n\n"
                + ("Filler sentence. " * 60) + "\n"),
    ))

    # 6. The guest command told to run under bash.
    cases.append((
        "guest command switched to bash",
        "CommandTests", "test_the_guest_command_uses_sh_not_bash",
        mutate(text, "sh /Volumes/PAYLOAD/guest-build-kext.sh",
               "bash /Volumes/PAYLOAD/guest-build-kext.sh",
               expect_count=text.count("sh /Volumes/PAYLOAD/guest-build-kext.sh")),
    ))

    # 7. The device the card names no longer the one the VMX uses.
    cases.append((
        "sata0:2 renamed throughout",
        "VmxAgreementTests", "test_the_card_names_the_device_the_vmx_actually_uses",
        mutate(text, "sata0:2", "sata0:9", expect_count=text.count("sata0:2")),
    ))

    # 8. The work dir the card names drifts from the script's default.
    cases.append((
        "work dir renamed",
        "PayloadAgreementTests", "test_the_work_dir_the_card_names_is_the_scripts_default",
        mutate(text, "nv3050-step7", "nv3050-elsewhere",
               expect_count=text.count("nv3050-step7")),
    ))

    # 9. The log name drifts.
    cases.append((
        "log name renamed",
        "PayloadAgreementTests", "test_the_log_name_the_card_names_matches_the_script",
        mutate(text, "step7-build.log", "build-output.log",
               expect_count=text.count("step7-build.log")),
    ))

    # 10. A step-1 heading reinstating "nothing to do".
    heading = re.search(r"^### 1\..*$", text, re.MULTILINE)
    if not heading:
        raise SystemExit("PROBE BROKEN: no '### 1.' heading to mutate")
    cases.append((
        "step 1 headed 'nothing to do'",
        "SelfConsistencyTests", "test_the_step_headings_do_not_claim_done_work_is_pending",
        text[:heading.start()] + "### 1. Shut the guest down — DONE, nothing to do"
        + text[heading.end():],
    ))

    print("Each mutation must make its test FAIL. A PASS means that assertion")
    print("cannot detect the defect it was written for.\n")

    survivors = []
    for label, klass, test_name, mutant in cases:
        if mutant == text:
            survivors.append("%s -- mutation produced an IDENTICAL document"
                             % label)
            print("  %-34s NO-OP (mutation changed nothing)" % label)
            continue
        broke, skipped = run_one(test_name, klass, mutant)
        if skipped:
            survivors.append("%s -- the test SKIPPED instead of running" % label)
            verdict = "SKIPPED"
        elif broke:
            verdict = "caught"
        else:
            survivors.append("%s -- %s.%s stayed GREEN" % (label, klass, test_name))
            verdict = "SURVIVED"
        print("  %-34s %s" % (label, verdict))

    # The other direction: the unmutated card must still pass everything, or a
    # "caught" above could just mean the module is broken for all inputs.
    print("\nControl -- the REAL card must still pass every mutated test:")
    for label, klass, test_name, _ in cases:
        broke, skipped = run_one(test_name, klass, text)
        if broke:
            survivors.append("CONTROL: %s.%s fails on the UNMUTATED card"
                             % (klass, test_name))
            print("  %-34s FAILS ON REAL CARD" % test_name[:34])
    print("  (silence above means all clean)")

    print()
    if survivors:
        for s in survivors:
            print("NEGATIVE CONTROL FAILED:", s)
        raise SystemExit(1)
    print("NEGATIVE CONTROL PASSED: all %d mutations were caught, and the real"
          " card passes every one of those tests." % len(cases))


if __name__ == "__main__":
    main()
