#!/usr/bin/env python3
"""
test_check_count_floor.py -- a suite that runs no checks must not read as green.

Why this exists
---------------
Every C++ suite ends on a count line, and ``tests/test_check_count_last_line.py``
guarantees that line is LAST. It says nothing about the numbers in it. So this
output:

    0/0 passed, 0 failed

passes that gate, passes ctest (exit 0, because zero failures), and prints a
sentence a human reads as success. Gut ``main()`` down to a ``return 0``, comment
out a whole block of check calls, or lose a suite to a ``#if 0`` that nobody
notices, and every signal this project has says green.

That is not hypothetical here. ``test_riscv_ucode`` was once a placeholder whose
recorded count was never filled in, and ``test_falcon`` was recorded as 165 while
the binary printed 169 -- a table described as "measured" that was false on the
day it was written. So this module keeps exactly ONE hand-recorded number per
suite, dates it, derives the floor from it in code rather than typing a second
number, and derives the SET of suites from the filesystem so a new suite cannot
be quietly omitted.

What each test is for
---------------------
- The table's keys are checked against ``tests/test_*.cpp`` on disk. A hand-kept
  list is the one thing that fails silently: add a suite, forget the entry, and
  this module would report success for the four it knows about.
- The floor is ``measured // 2``. Halving is deliberately generous -- the defect
  being caught is a count collapsing to zero or near it, not a count drifting by
  a dozen as tests are refactored. A floor that goes red on ordinary churn gets
  edited rather than investigated, which is how a gate becomes decoration.
- The CEILING is ``measured + max(8, measured // 8)``, and it exists because a
  floor bounds one side only. A suite that grows past its recorded number could
  never redden anything, so every reading in this file went stale silently by
  construction -- five times, twice inside one day. The ceiling fails instead,
  and its message names the entry to update. It is tight where the floor is
  generous because the two edges catch opposite defects: a collapse is a real
  bug in the suite, while outgrowing the band is a one-line edit here.
- Binaries are run when they exist. If ``tests/build`` exists but a listed suite's
  binary does not, that is a failure, not a skip: it means CMake stopped
  registering it or the link broke.
- A stale binary is refused. A ``.exe`` older than its ``.cpp`` prints a count
  that describes code nobody is running any more, so asserting a floor against it
  is evidence about the wrong source.
- The source-side floor runs unconditionally, because ``run-python-tests.bat`` is
  the one gate that works on a bare checkout, where no binary exists at all. It
  pins a SHAPE (a suite calls many check helpers) rather than a per-suite
  magnitude it has no way to measure without building.
- The parser has its own negative control. If ``parse_count`` returned ``None``
  for ``0/0 passed, 0 failed`` instead of ``0``, the dynamic test would skip the
  exact line it exists to catch and still pass.

Relationship to test_check_count_last_line.py
---------------------------------------------
That module reads source and answers "is the count the final line?". This one
runs the binaries and answers "is the count a real number?". Neither implies the
other: a suite can end perfectly on ``0 checks, 0 failures``.
"""

import glob
import os
import re
import subprocess
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(HERE, "build")

#
# Measured 2026-08-13 by running each binary in tests/build and reading its final
# line. These are the only hand-kept magnitudes in this module; the floors below
# are computed from them so that a floor cannot disagree with its own provenance.
#
#   test_decode        64/64 passed, 0 failed
#   test_falcon      169/169 passed, 0 failed
#   test_riscv_ucode   116 checks, 0 failures
#   test_gsp_boot      217 checks, 0 failures
#   test_gsp_stage     181 checks, 0 failures
#
# Added 2026-08-15, measured the same way by running the binary:
#   test_intel_display 144 checks, 0 failures
#
# Re-measured 2026-08-15, LATER THE SAME DAY, after the plane layer was added to
# IntelDisplay.hpp (igPlaneReg and the field decodes) and its three sections were
# wired into main():
#   test_intel_display 205 checks, 0 failures
# The 144 above is left standing as the superseded reading rather than deleted --
# it is the fourth time in this file's history that a number described as
# measured went stale, and twice now inside the same day. 205 is what the binary
# printed at that time.
#
# Re-measured 2026-08-16, by running tests/build/test_intel_display.exe after the
# 2026-08-16 Tiger Lake capture settled PLANE_SIZE's -1 convention and the
# hazard-5 section was converted from raw-only to measured (decoded wrappers,
# fold negative controls, 65536 and zero-word edges):
#   test_intel_display 218 checks, 0 failures
# That is the fifth staleness in this file, and the reason the module no longer
# bounds a count on one side only. Until 2026-08-16 the sole assertion was
# MEASURED//2, so a suite that GREW could not redden anything -- these readings
# went stale silently BY CONSTRUCTION, and only a suite that SHRANK past half
# was ever caught. There is now a derived upper edge as well (ceiling_for), so a
# count that has outgrown its recorded number fails and names the entry.
#
# Re-measured 2026-08-16, LATER THE SAME DAY, after the INTEL-CITE fix: the
# header's PLANE_SIZE_OBSERVED_2026_08_16 was cited in three places and defined
# in zero, so the name was added as IG_PLANE_SIZE_OBSERVED_2026_08_16 and the
# hazard-5 section gained one row pinning it against a BARE LITERAL (the two
# decoded checks now drive THROUGH the constant instead of repeating 0x0437077F,
# which is what keeps the define load-bearing rather than merely present):
#   test_intel_display 219 checks, 0 failures
# Sixth staleness in this file, and the first one the new ceiling would have
# caught on its own rather than needing a human to notice.
#
# Re-measured 2026-08-17, LATER THE SAME DAY, after igFbSurfaceRangeOk() and
# IG_FB_GGTT_POLICY_LIMIT were added to IntelFramebuffer.hpp and
# testSurfaceRange() was wired into main():
#   test_intel_framebuffer 110 checks, 0 failures
# 110 is ABOVE the previous entry's ceiling (97 + max(8, 12) = 109) by one, so
# this is the second reading in this file's history the ceiling caught rather
# than a human noticing. One check over the edge is the smallest possible
# detection, which is the argument for keeping the upper band tight.
#
# Re-measured 2026-08-17, by running tests/build/test_intel_framebuffer.exe after
# IG_FB_INHERIT_VERDICT_LIMIT was added to IntelFramebuffer.hpp and
# testInheritVerdictCodes() was wired into main():
#   test_intel_framebuffer 97 checks, 0 failures
# 97 is inside the previous entry's band (87 + max(8, 10) = 97, exactly the
# ceiling), so this reading is recorded rather than discovered -- one more check
# and the gate would have named it. Recording at the edge is the point: the band
# is a backstop for a collapse or a jump, never a substitute for the reading.
#
# Re-measured 2026-08-17, LATER THE SAME DAY AGAIN, after the TRANS_DDI_FUNC_CTL
# layer was added to IntelDisplay.hpp (igTransDdiReg, igDdiEnabled,
# igDdiPortField, igDdiPort, igDdiMode, and the IG_DDI_* constants) and
# testTransDdi() was wired into main():
#   test_intel_display 319 checks, 0 failures
# 319 is above the previous entry's ceiling (281 + max(8, 35) = 316) by three, so
# this is the THIRD reading the ceiling caught rather than a human noticing --
# and all three came after the edge was added on 2026-08-16. Before it, six
# consecutive readings went stale in silence.
#
# Re-measured 2026-08-17, LATER THE SAME DAY, after igPlaneMatchesPipe() and the
# four IG_JOIN_* verdicts were added to IntelDisplay.hpp and testPlaneJoin() was
# wired into main():
#   test_intel_display 281 checks, 0 failures
# Recorded here even though NO GATE FIRED for it. 281 sits inside the previous
# entry's band (262 + max(8, 32) = 294), so the ceiling was silent and the floor
# was never in question -- this is precisely the interval in which the six
# historical stalenesses accumulated, one small growth at a time, each one
# individually too small to trip anything. The band exists to catch a collapse
# and a large jump; it cannot substitute for updating the reading.
#
# Re-measured 2026-08-17, by running tests/build/test_intel_display.exe after the
# whole-mode consistency layer was added to IntelDisplay.hpp (igAxisConsistent,
# igModeConsistent, and the seven IG_MODE_* verdicts) and its two sections were
# wired into main():
#   test_intel_display 262 checks, 0 failures
# This is the SEVENTH staleness in this file, and the first one caught by the
# ceiling rather than by a human noticing -- 262 exceeds 219 + max(8, 27) = 246,
# so test_each_built_suite_reports_at_least_its_floor went red and its message
# named this entry and the number to put in it. That is the edge working as
# designed; the six readings before it went stale silently.
#
# Added 2026-08-16, measured the same way -- built via tests/build-msvc.bat (7/7
# passed) and then ran tests/build/test_intel_gpu_id.exe directly:
#   test_intel_gpu_id 77 checks, 0 failures
# This is the first entry added AFTER the ceiling existed, so it is also the
# first one that cannot go stale silently: a suite that grows past
# 77 + max(8, 9) trips ceiling_for() and the failure names this entry.
#
# Re-measured 2026-08-17, by running tests/build/test_intel_display.exe (built
# 14:54 against IntelDisplay.hpp at 14:53 and test_intel_display.cpp at 14:52,
# so the binary is NEWER than both sources and its count describes the code
# that is there):
#   test_intel_display 354 checks, 0 failures
# This entry is different in kind from the seven above it. The dict already said
# 354 and the binary already printed 354, so every assertion in this module was
# GREEN -- floor, ceiling, staleness, last-line, all of it. What no test looked
# at was whether 354 appeared anywhere in this comment, and it did not: the
# newest reading here stopped at 319. So the table held a number described as
# measured whose only evidence was that it had been typed, and the ceiling is
# structurally unable to notice, because it compares the BINARY to the TABLE and
# a table edited to match a binary agrees with it by construction.
#
# EveryTableEntryHasProvenance now reads this comment block and requires a
# matching line per entry, so the fix for the next staleness cannot be a
# one-line dict edit any more -- it has to be a dict edit plus a reading. Note
# also that 354 sits INSIDE the previous entry's band (319 + max(8, 39) = 358),
# four checks under the edge, so nothing would have fired here on its own.
#
# Re-measured 2026-08-17, by running tests/build/test_gsp_boot.exe after
# testApertureGuard() was added to test_gsp_boot.cpp and wired into main():
#   test_gsp_boot 246 checks, 0 failures
# First NVIDIA-side entry to move in this file since 2026-08-13. The section it
# records covers NVGSP_RESULT_ERR_APERTURE_VIOLATION, which was produced at two
# sites in NVGspBoot.hpp and reached by no check in the suite -- found by
# set-differencing the NVGSP_RESULT_* names spelled in the header against those
# spelled in the suite (fourteen against thirteen). 246 is above the previous
# entry's ceiling (217 + max(8, 27) = 244) by two, so this reading was also due
# to be caught by the upper edge rather than only recorded. The 217 above is
# left standing as the superseded reading; test_at_least_one_suite_is_actually
# _backed still asserts it, deliberately, because that test's job is to prove
# the reader finds real lines rather than to track the current count.
#
# Re-measured 2026-08-17, same session, after testDmaStateSequence() was added
# to test_gsp_boot.cpp and wired into main() after testDmaMultiChunk():
#   test_gsp_boot 258 checks, 0 failures
# Second finding from the same set-difference technique: NVGspBoot.hpp spells
# fifteen NVGSP_STATE_* names and the suite spelled thirteen, missing DMA_XFER
# and DMA_DRAIN. Both were reached at runtime and both had their name-table arms
# covered, so this was not lost coverage -- what no test did was observe the
# machine WHILE in them, because testStepDriving() sets skipFirmwareLoad and so
# never enters the DMA path, and every DMA test inspects effects after
# nvGspBootRun() returned. 258 is inside the 246 band (246 + max(8, 30) = 276),
# so unlike the 217 -> 246 jump this one would NOT have been caught by the
# ceiling; the provenance requirement is the only reason it is recorded here.
#
# Re-measured 2026-08-17, same session, after the single manifest-overrun case in
# test_riscv_ucode.cpp was generalised into a loop over all six rows of
# nvRiscvUcodeIsWellFormed()'s pairs[] table:
#   test_riscv_ucode 134 checks, 0 failures
# First movement of this entry since 2026-08-13. The `size != 0 &&` short-circuit
# in that loop meant five of the six rows were never reached: the real bootloader
# leaves every optional size at zero and buildBlob() reproduces that faithfully,
# so only the row the suite happened to write a size into was ever validated.
# Verified by watching it fail -- replacing pairs[] row 6 with a duplicate of
# row 2 left the old 116-check suite green and makes the new one report
# "overrunning swbromData rejected" as true, one failure out of eight suites
# under ctest. Each row also carries a zero-size positive control so a rejection
# cannot be attributed to the offset write. 134 sits ABOVE the previous entry's
# ceiling (116 + max(8, 14) = 130) by four, so the upper edge was due to fire on
# this reading on its own -- like the 217 -> 246 jump and unlike 246 -> 258.
#
# Re-measured 2026-08-17, same session, after testBromParamsStateSequence() was
# added to test_gsp_boot.cpp and wired into main() after testStepDriving():
#   test_gsp_boot 276 checks, 0 failures
# Third movement of this entry today, and the last open NVIDIA-side item on
# handoff18.md's list. BROM_PARAMS had journal-index coverage and no state
# coverage: testFullSequenceOrder() pins the four BROM writes at whole-run
# indices 2..5, which cannot see a missing transition, and testDmaStateSequence()
# pins a walk, which cannot see a reordered write. Both now exist for this state.
#
# The non-vacuity proof took three mutations rather than one, and the first two
# are the reason the third is worth recording. Swapping MOD_SEL to the front of
# the block reddened 13 checks -- but four of them were the PRE-EXISTING journal
# checks, so that mutation proves the new order assertions are live and proves
# nothing about added reach. Splitting BROM_PARAMS across two entries reddened
# 30, again including pre-existing ones, because the extra entry shifted every
# downstream journal index. Only the third mutation isolates the new section:
# moving the MOD_SEL write out of BROM_PARAMS and into CORE_START leaves the
# same five writes in the same order at the same whole-run indices, the same
# state walk, and the same modSelWriteIndex -- every pre-existing check stayed
# GREEN, and exactly two failed, both in the new section ("one step emitted
# exactly four writes" got 3, and the fourth journal entry bounded to that step
# was absent). That is the shape of a check that adds reach rather than
# duplicating it.
#
# 276 sits inside the previous entry's band (258 + max(8, 32) = 290), fourteen
# checks under the edge, so nothing would have fired here on its own -- like
# 246 -> 258 and unlike 217 -> 246 or 116 -> 134.
#
# Re-measured 2026-08-17, same session, after testReadTranslators() was added to
# test_gsp_boot.cpp and wired into main() after testBromParamsStateSequence():
#   test_gsp_boot 313 checks, 0 failures
# Fourth movement of this entry today, and the first one about READS. Every
# WRITE site is already pinned to an absolute address by checkJournal(), so a
# write that reached for nvGspFalconReg() where it wanted nvGspRiscvReg()
# reddens immediately. Reads had no such pin: nvGspBootRead() took the address,
# used it, and discarded it, so a read one 4 KiB page off returns a plausible
# value from a real register on the WRONG engine and nothing in the suite could
# tell. nvIsInGspAperture() cannot help either -- 0x110000..0x113FFF contains
# both blocks, so the mistranslated address is inside the window.
#
# The fixture now records the SET of distinct read offsets (a set, not a
# sequence: ACTIVE_WAIT polls the same registers repeatedly, so read ORDER is
# dominated by poll counts and says nothing, while the set of addresses is
# exactly what distinguishes the two translators). Recorded before the aperture
# check, so an address the machine reached for is visible even when the read was
# refused.
#
# It takes TWO runs, and the first draft of the section got that wrong in a way
# worth recording. It asserted a single healthy run touches all six read sites
# and that the run ends with something other than RUNNING; it went red 4/297,
# and both premises were false. NVGSP_RESULT_RUNNING is the SUCCESS terminal --
# the core came up -- and ACTIVE_WAIT stops on it at NVGspBoot.hpp:805 BEFORE
# nvGspBootCheckRiscvErrors() at :808, so a healthy boot can never reach the two
# error-status reads at :400 and :404. Run A is the healthy one-chunk boot: four
# addresses, with the absence of the error pair asserted as the precedence claim
# (ACTIVE beats the error check). Run B releases a core that never reports
# ACTIVE, falls through into the error check, and reaches five. The union is
# pinned as runACount + runBCount - 3 == 6 rather than as a literal, so a read
# site added to EITHER path breaks it.
#
# 313 sits ABOVE the previous entry's ceiling (276 + max(8, 34) = 310) by three,
# so the upper edge was due to fire on this reading on its own -- like 217 -> 246
# and 116 -> 134, unlike 246 -> 258 or 258 -> 276.
#
# Re-measured 2026-08-17, same session, after testNameSwitches() was added to
# test_decode.cpp and wired into main() after testFbSize():
#   test_decode 96/96 passed, 0 failed
# FIRST movement of this entry since 2026-08-13, and the oldest unmoved number
# in this file. The subject is the two name switches in NVDecode.hpp --
# nvArchitectureName() and nvAmpereImplementationName() -- which are the only
# string-producing functions in the NVIDIA decode layer. The suite called each
# of them EXACTLY ONCE: nvArchitectureName(NV_ARCHITECTURE_GA100) inside
# testBoot0() and nvAmpereImplementationName(NV_IMPLEMENTATION_GA107) inside
# testSplitArchitecture(). So six of seven architecture arms and five of six
# implementation arms had never EXECUTED, which is handoff20's Intel finding one
# level down: there, the switches were unreachable because they lived in an
# IOKit translation unit; here they were reachable and simply unvisited, which
# is the same coverage hole reached by a different road.
#
# Both switches are complete as written -- verified by set-difference before
# anything was added, 7 NV_ARCHITECTURE_* defines against 7 case arms and 6
# NV_IMPLEMENTATION_* against 6, so unlike igModeVerdictName() there was no
# missing arm to find. What the section pins is that this stays true, on three
# properties: every arm returns its OWN string (per-arm, plus a pairwise-distinct
# check, because a copy-paste arm returning a sibling's name satisfies every
# "has a name" assertion); every value in each family is named (so an arm DELETED
# later reddens); and the value one past each family answers the sentinel (the
# tripwire -- an eighth architecture cannot be given an arm without coming here
# and extending the first two properties, which is exactly the step whose absence
# let the Intel defect through).
#
# The mistake worth recording is that the two switches do NOT share a sentinel.
# nvArchitectureName() answers "unknown" (NVDecode.hpp:278);
# nvAmpereImplementationName() answers "unrecognised Ampere part" (:291). I
# assumed one string, four assertions went RED against correct shipping code,
# and the FIFTH occurrence was not cosmetic: the implementation-side coverage
# loop compared against "unknown", a value that switch can never return, so it
# counted 6 named arms unconditionally -- including for an arm deleted from the
# switch. A sentinel guessed from a sibling function produces a coverage loop
# that cannot fail, which is this project's recurring shape rather than a new
# one. Both sentinels are now bound to named constants, each asserted against
# what the header actually returns, and asserted distinct from each other.
#
# Non-vacuity: the discriminating mutation is an arm returning a SIBLING's
# string -- NV_ARCHITECTURE_GB200 changed from "Blackwell 2" to "Blackwell".
# Two failures, both in the new section (the per-arm check for 0x1B, and the
# pairwise-distinct count going 0 -> 1), every pre-existing check GREEN. That is
# the class the old suite was blind to by construction: the mutated value is
# still a plausible architecture name, nothing downstream consumes it, and it
# only ever appears in a log line a human reads. NVDecode.hpp was mutated and
# reverted and is byte-identical.
#
# 96 is ABOVE the previous entry's ceiling (64 + max(8, 8) = 72) by twenty-four,
# the largest overshoot in this file's history -- unsurprising for the entry that
# had not moved in four days while the band around it stayed at its 2026-08-13
# width.
#
# Added 2026-08-17, measured by running tests/build-msvc.bat (9/9 passed) and
# then tests/build/test_intel_display_names.exe directly:
#   test_intel_display_names 236 checks, 0 failures
# A NEW suite rather than a re-measurement, and the first one whose subject was
# previously unreachable from tests/ at all: the verdict-name switches,
# igTransName() and the property-key composition were file statics inside
# IntelDisplayProbe.cpp, which includes <IOKit/IOService.h> and so cannot be
# compiled here. handoff19 recorded that as ZERO runtime coverage, and it was
# hiding a real defect -- igModeVerdictName() had arms for verdicts 0 through 5
# while IG_MODE_VERDICT_LIMIT is 6, so IG_MODE_SRC_MISMATCH published "unknown".
#
# There is no band to compare 236 against, because a first reading has no
# previous entry. That is worth saying rather than leaving implied: the ceiling
# catches a suite that GROWS past its recorded number, and it can say nothing
# about whether the first number was measured or typed. What backs this one is
# the run above and the fact that the checker's own count is printed by the
# binary, not by this file.
# Re-measured 2026-08-17, by running tests/build/test_intel_display.exe after
# igReadDecision() gained a FOURTH guard and a fourth parameter -- the length of
# the BAR0 mapping -- and testReadDecision() grew the block that drives it:
#   test_intel_display 391 checks, 0 failures
# 389 was the reading BEFORE the non-vacuity proof, and the proof is why it is
# not the number here. Planting `offset >= mappedLength` in place of
# `offset + IG_MMIO_ACCESS_BYTES > mappedLength` reddened exactly ONE check, and
# it was a new one -- every pre-existing check green, which is the only mutation
# signal this project accepts as evidence of added reach. But one failure also
# exposed that the block pinned one POINT of the overhang window rather than the
# window: `offset >= length` fails only the +3 case, `offset + 2 > length` fails
# only +1 and +2, and `offset + 3 > length` fails only +1. Two more checks pin
# the remaining two bytes, so a bound wrong by ANY amount reddens at least one.
# 389 -> 391.
# The section it records closed a live defect rather than a coverage hole.
# IntelDisplayProbe::mapBar0() logs a WARNING when BAR0 is shorter than
# IG_BAR0_EXPECTED_LENGTH (0x80000) and then attaches anyway, deliberately, so
# that a short BAR stays inspectable in the IORegistry. That warning was the
# only consequence there was: regRead32() bounded offsets against
# IG_DISPLAY_POLICY_LIMIT (0x7BFFF), which bounds the addresses the decode
# HEADERS can produce and says nothing about how many bytes the OS mapped. So on
# a 0x40000 BAR, transcoder A at 0x60000 was read 0x20000 bytes past the end of
# the mapping -- not a fault, just whatever the next mapping holds, decoded as a
# transcoder register. The two numbers are different on purpose and nothing
# joined them.
#
# 389 sits INSIDE the previous entry's band (354 + max(8, 39) = 393) by four, so
# the ceiling would not have fired on this reading and the provenance line above
# is the only thing recording it. That is the second time in three readings that
# a real growth landed just under the edge, which is the argument for the
# provenance requirement rather than for widening the band.
#
# Re-measured 2026-08-18, by running tests/build/test_intel_display.exe after
# igReadRefused() and igSampleVerdict() were added to IntelDisplay.hpp and
# testSampleVerdict() was wired into main():
#   test_intel_display 421 checks, 0 failures
# The section it records closed the OTHER HALF of the 2026-08-17 defect above,
# and it was a live defect rather than a coverage hole. igReadDecision() decides
# whether a dereference may happen and regRead32() returns IG_MMIO_INVALID when
# it may not -- but IG_MMIO_INVALID appeared at exactly ONE place in the whole
# probe, inside regRead32() itself. Both publishers handed whatever came back
# straight to igModeConsistent(), igPipeUsable() and igFbInheritable(), and
# setProperty() with a decode of a refused read is BYTE-IDENTICAL to
# setProperty() with a decode of a register that genuinely read that value. So a
# refusal the guard correctly produced was published as a mode.
#
# 421 sits INSIDE the previous entry's band (391 + max(8, 39) = 430) by nine, so
# the ceiling was silent again -- the third consecutive real growth to land under
# the edge.
#
# Re-measured 2026-08-18, the same way, after igSampleVerdictName() was added to
# IntelDisplayNames.hpp and testSampleNames() was wired into main(), with the
# exhaustiveness and pairwise-distinct loops extended over the new family:
#   test_intel_display_names 245 checks, 0 failures
# 245 is inside that entry's band as well (236 + max(8, 23) = 259). This is the
# first re-measurement of this suite; the 236 above is its first reading and is
# left standing rather than deleted.
#
# Re-measured 2026-08-19, the same way, after igScanVerdictName() was added to
# IntelDisplayNames.hpp and testScanNames() was wired into main(), with the
# exhaustiveness, sentinel and pairwise-distinct loops extended over the
# IG_SCAN_ family:
#   test_intel_display_names 284 checks, 0 failures
# 284 is ABOVE that entry's ceiling (245 + max(8, 24) = 269) by fifteen, so the
# band FIRED on this reading rather than absorbing it. That is correct and worth
# recording: IG_SCAN_ is the largest verdict family in the Intel track -- seven
# arms -- and a pairwise-distinct walk over seven is 21 pairs on its own, so a
# family this size cannot land inside a band sized for the previous growth.
#
# The IG_SCAN_ family's LIMIT is INCLUSIVE (IG_SCAN_VERDICT_LIMIT is 6 and
# IG_SCAN_ERR_OUTSIDE_APERTURE is 6), and so is every other Intel family's --
# measured 2026-08-19, each bound spelled "the largest verdict above" and equal
# to its largest member, with tools/check-registers.py:2440 enforcing
# limit == max(codes) per family. An earlier revision of this paragraph called
# IG_MODE_ and IG_FB_INHERIT_ the opposite convention; that was false. The walk
# is `<=` for every family in that suite, which is why the new arms are covered
# at all; a `<` written from habit would have stopped one short of
# OUTSIDE_APERTURE and this entry would have read 283 with the gap invisible.
#
# Re-measured 2026-08-18 by running tests/build/test_falcon.exe after the NVWPR2_*
# verdict family was added to NVFalcon.hpp and testWpr2Verdict() was wired into
# main():
#   test_falcon 193/193 passed, 0 failed
# The section it records closed a live defect on the NVIDIA side, and it is the
# same class as the two Intel entries above one track over. checkWpr2() returned
# a bare bool meaning "boot precondition satisfied", and that bool was FALSE for
# two categorically different reasons: WPR2 genuinely UP, or the registers not
# decoding at all. Only the second is a bug in this driver. Worse, the caller
# published GSP_WPR2_ADDR_LO and _HI unconditionally, and the bool tested
# ADDR_HI ONLY -- so a refused ADDR_LO with a healthy ADDR_HI reported the
# precondition SATISFIED and published an all-ones low word as a measurement.
#
# 193 is ABOVE this entry's own band (169 + max(8, 16) = 185) by eight, so unlike
# the last three growths the ceiling FIRED on this reading rather than staying
# silent. That is the band working as designed: 24 new checks over a 169-check
# suite is a 14% jump, which is exactly the magnitude the ceiling exists to make
# somebody look at.
# Re-measured 2026-08-18 by running tests/build/test_riscv_ucode.exe after
# nvRiscvDescVersion() and NV_RISCV_DESC_VERSION_UNKNOWN were added to
# NVRiscvUcode.hpp, a version equality was added to nvRiscvUcodeIsWellFormed(),
# and testDescriptorVersion() was wired into main():
#   test_riscv_ucode 144 checks, 0 failures
# The section it records closed a live defect rather than a coverage hole, and
# it was found by parsing the REAL firmware rather than by reading source.
# nvRiscvUcodeIsWellFormed() treated the region between hdrOffset and dataOffset
# as an RM_RISCV_UCODE_DESC in every blob. Measured against all three files in
# reference/ga107-firmware/, only bootloader-570.144.bin actually carries one:
# both booters carry an nvfw_hs_header_v2 there, same container magic, same
# hdrOffset of 24, first word 60 (0x3C) which is that header's own header_size
# and not a structure version. A booter WAS still rejected before this change --
# but by accident, three optional-region rows deep, because signature bytes at
# +0x24 happened to decode to a size of 0x1F3B3C30 that does not fit the
# payload. Nothing constrains a future signature to hold an out-of-range word at
# that offset, and the failure being guarded is not a parse error: it is a DMA
# of an HS signature block into IMEM as though it were bootloader code.
#
# The non-vacuity proof is the reason this entry is worth its length. Gutting
# the new guard to `if (false)` reddened exactly FOUR checks, all four in the new
# section, every pre-existing check green -- the signal handoff18 1e argues for.
# The separating input is a blob differing from the real bootloader in the
# version word ALONE, with every optional row still zero, so nothing else in the
# suite distinguishes the two.
#
# 144 sits INSIDE this entry's band (134 + max(8, 13) = 147) by three, so the
# ceiling did not fire and this provenance line is the only thing recording the
# growth. Fourth reading in five where a real growth landed just under the edge.
#
# Re-measured 2026-08-18, later the same day, by running
# tests/build/test_riscv_ucode.exe after testRealFirmwareFiles() was added and
# tests/CMakeLists.txt was taught to inject NV_FIRMWARE_DIR:
#   test_riscv_ucode 188 checks, 0 failures
# This is the entry above's own open item closed. That section found its defect
# by parsing the real firmware in a scratch script that was then deleted, so the
# SUITE still proved nothing about the files on disk -- every blob it drove came
# from buildBlob(), which builds the BOOTLOADER layout. The new section opens
# reference/ga107-firmware/*.bin directly and SKIPS with a banner and zero checks
# when the directory is absent, which preserves the bare-checkout property the
# suite's own header comment describes. A reading of 144 on a tree without the
# firmware is therefore correct and not a regression; the skip banner is the
# record of which reading you are looking at.
#
# The path comes from CMake as an absolute NV_FIRMWARE_DIR rather than relative
# to the binary's cwd, because a wrong path SKIPS rather than fails -- the
# vacuity direction, and the worst failure mode available to a section whose
# whole purpose is to read files.
#
# One measurement CONTRADICTS what the tree recorded. handoff18 1c says the real
# bootloader leaves all six optional regions zero and buildBlob() reproduces
# that. Three are not zero: manifest size 0x800, monitor data 0x800/0x1000,
# monitor code 0x1800/0x2900, and bIsMonitorEnabled is 1. Only the elf row and
# the two swbrom rows are genuinely zero. So the loop generalised in handoff18
# was exercising a blob strictly SIMPLER than the one that ships, which is the
# same fixture blindness one level down. Those six values are now pinned.
#
# The non-vacuity proof took two mutations and the first one is the lesson.
# Changing NV_RISCV_GA107_BL_BOOTLOADER_OFFSET by four reddened 4 checks, THREE
# of them pre-existing -- a loud mutation, which proves the OLD tests already
# covered that constant rather than proving the new section reaches anything.
# Capping nvRiscvDescFieldCount() at NV_RISCV_DESC_FIELDS_THROUGH_PLIC reddened
# exactly TWO checks, both new, every pre-existing check green: only a real file
# carries a 216-slot region, so no synthetic blob in the suite can notice a cap
# at 23. NVRiscvUcode.hpp was mutated and reverted twice and is byte-identical,
# md5 b20dc7dbfad14aff7b8c86adcf1c2904 before and after.
#
# A third reading worth recording: the first mutation appeared to redden NOTHING
# because it was rebuilt with bare `ninja`, which fails on cstdio in this shell
# for want of an MSVC INCLUDE and left the OLD binary in place. Run the gate, not
# the generator -- a no-op mutation reads exactly like proof of a vacuous test.
#
# 188 is ABOVE this entry's band (144 + max(8, 14) = 158) by thirty, so the
# ceiling FIRED on this reading, correctly: 44 new checks over a 144-check suite
# is a 31% jump and is exactly the magnitude the ceiling exists to surface.
# Re-measured 2026-08-18, LATER THE SAME DAY, by running
# tests/build/test_gsp_boot.exe after testReadMask() was added:
#   test_gsp_boot      341 checks, 0 failures
#
# What the section covers: NVGspBoot gained a readMask, and
# NV3050Probe::publishGspBootResult() now emits each of the five diagnostic
# words ONLY when this run actually read it. Before that it published all five
# unconditionally while three of them -- riscvCpuctl, privErrStat, hubErrStat --
# are assigned only inside ACTIVE_WAIT and nvGspBootCheckRiscvErrors(). A run
# stopping at PROBE therefore put three initialiser zeros into the IORegistry,
# byte-identical to three registers that genuinely read clear. handoff18
# section 1d's class for the fifth time.
#
# The separating case is the one that does NOT look like a failure: a HEALTHY
# boot stops on NVGSP_RESULT_RUNNING inside ACTIVE_WAIT and never calls the
# error-pair helper at all, so the successful path is exactly the path that
# fabricated two words -- and the path an operator is most likely to trust.
#
# Non-vacuity: setting NVGSP_READ_HWCFG2 beside NVGSP_READ_CPUCTL in PROBE --
# the copy-paste shape runs C and D exist to catch -- reddened exactly TWO
# checks, both new ("C: HWCFG2 was NOT read", "C: mask is CPUCTL alone"), every
# pre-existing check green. handoff18 section 1e's signal. NVGspBoot.hpp was
# mutated and reverted once and the suite is green at 341 after the revert.
#
# 341 sits INSIDE this entry's band (313 + max(8, 39) = 352) by eleven, so the
# ceiling did NOT fire on this reading and this provenance paragraph is the only
# record that a human ran the binary.
#
# Re-measured 2026-08-18, LATER THE SAME DAY, by running tests/build-msvc.bat
# (9/9 passed) and then tests/build/test_riscv_ucode.exe directly:
#   test_riscv_ucode   211 checks, 0 failures
#
# What the section covers: nvRiscvUcodeIsWellFormed() was a bare bool with six
# distinct ways to say no. nvGspStagePlan() mapped every one of them onto
# NVGSP_STAGE_ERR_MALFORMED and NV3050Probe::loadGspFirmware() logged that single
# word, so a truncated file, a booter handed in by mistake, and a descriptor
# pointing off the end of its own payload were indistinguishable to the one
# person who could act on them -- and those call for three different actions.
# nvRiscvUcodeVerdict() now names eight outcomes and testUcodeVerdict() drives a
# separating input for each, plus the per-arm name coverage bounded on
# NV_RISCV_UCODE_VERDICT_LIMIT and a pairwise-distinctness loop.
#
# The reachability half is the sharper one. NVGSP_STAGE_ERR_EMPTY_BOOTLOADER and
# NVGSP_STAGE_ERR_REGION_OUTSIDE re-read the SAME two descriptor fields the
# well-formed gate had already refused on, so both arms were provably dead --
# and tests/test_gsp_stage.cpp SAID SO in its own comments while asserting
# MALFORMED for cases the enum has precise names for. A suite documenting its
# subject's dead arm and then asserting the collapse is a gate agreeing with a
# defect. Both codes are now reachable and asserted under their own names;
# test_gsp_stage is unmoved at 181 because four assertions changed their expected
# value rather than being added.
#
# 211 lands EXACTLY on this entry's previous ceiling (188 + max(8, 23) = 211),
# and the check is `>`, so it did not fire -- one check further and it would
# have. Recorded because a reading that grazes its own edge is the one most
# likely to be misread later as comfortably inside.
#
# Re-measured 2026-08-18, after testPartVerdict() was added to test_decode.cpp
# and wired into main() after testNameSwitches():
#   test_decode 121/121 passed, 0 failed
# SECOND movement of this entry, one day after the first. The subject is
# nvPartVerdict() and nvPartVerdictName(), new in NVDecode.hpp, which carry the
# four findings decodeBoot0() has always detected and always written to the LOG
# ONLY: a non-Ampere architecture, an Ada part specifically (the 'RTX 3050 A'
# case, where the whole plan retargets), GA107 confirmed, and an Ampere part
# that is not GA107. Those four lived inside a method on the IOKit shell, so no
# host suite could compile them and nothing had ever EXECUTED one -- and
# publishIdentity() derived a bare NAME from the same word, so "Ada" reached the
# IORegistry byte-identically to "Ampere" with nothing saying one of them means
# no constant in this driver applies.
#
# The new section's sharpest check is the ORDER pin. NV_PART_ADA is a SUBSET of
# "not Ampere", so a version testing the general case first makes the Ada arm
# unreachable -- the dead-arm shape 3050-handoff32.md exists to remove,
# reintroduced one level over. An Ada part carrying GA107's implementation
# number is both, and only the Ada-first order returns NV_PART_ADA.
#
# 121 sits ABOVE this entry's previous ceiling (96 + max(8, 12) = 108) by
# thirteen, so the upper edge FIRED on this reading on its own -- like 217 -> 246
# and 276 -> 313, unlike yesterday's 188 -> 211 which grazed its edge without
# tripping it.
#
# Re-measured 2026-08-18, after testRealFirmwareFiles() was converted from the
# bool wrapper to the verdict:
#   test_riscv_ucode 214/214 passed, 0 failed
# THIRD movement of this entry in three days, and the smallest -- +3 rather than
# a new section. The subject is the REAL files in reference/ga107-firmware/,
# which were asserted through nvRiscvUcodeIsWellFormed() and therefore against a
# BOOL. `false` is satisfied by all seven refusal codes at once, so the
# pre-2026-08-18 behaviour -- refusing the two booters three optional-region
# rows deep because signature bytes at +0x24 decoded to an out-of-range size --
# satisfied that assertion IDENTICALLY to refusing them because the descriptor
# version is not 5. One of those is a fact about the file; the other is a
# coincidence of its signature bytes, and only the verdict code separates them.
# 3050-handoff27.md §2 MEASURED the version difference; this converts that
# measurement into a standing assertion, so a regression to accidental refusal
# reddens instead of passing.
#
# The bool assertions are KEPT beside the verdict ones rather than replaced.
# nvRiscvUcodeIsWellFormed() is what the IOKit shell actually calls, and the
# pair is what pins the wrapper to its own subject: a wrapper that stopped
# consulting the verdict would keep the bool checks green and break nothing
# else. That is the +3 -- one verdict check on the bootloader and one per
# booter, with the three bool checks retained.
#
# 214 sits well INSIDE this entry's band (211 + max(8, 26) = 237) by twenty
# three, so the upper edge did NOT fire -- correctly, since no new section was
# added and the growth is three assertions over existing fixtures.
#
# Re-measured 2026-08-19 by running tests/build/test_falcon.exe after
# testRiscvErrorVerdict() was added to tests/test_falcon.cpp and wired into
# main(), alongside the NVRISCV_ERR_* verdict family in NVFalcon.hpp:
#   test_falcon 239/239 passed, 0 failed
# The subject is the RISC-V error latches, which nvRiscvHasError() collapsed onto
# one bool over four structurally different situations -- priv fault, hub fault,
# both, and NEITHER REGISTER DECODING. regRead32() answers every refusal with
# all-ones, so a BAR0 mapping covering the Falcon block but stopping before the
# RISC-V block one 4 KiB page above made both latches read 0xFFFFFFFF and the old
# bool answered TRUE -- a bug in THIS DRIVER reported as a latched hardware
# fault. Those call for opposite actions: reset the engine, or fix the mapping.
#
# The four pre-existing checks in testEngineReset() are KEPT rather than
# replaced, and they are also the reason a bool could not see this: all four are
# satisfied IDENTICALLY by the old two-line bool and by the verdict wrapper that
# replaced it, because `== true` has three preimages here and `== false` has one.
# 3050-handoff34.md §1's lesson one register block over. The PAIR is what pins
# the wrapper to its own subject -- nine inputs assert
# `nvRiscvHasError(a, b) == (nvRiscvErrorVerdict(a, b) != NVRISCV_ERR_NONE)`, so
# a wrapper that stopped consulting the verdict reddens instead of passing.
#
# 239 sits ABOVE this entry's previous ceiling (193 + max(8, 19) = 212) by twenty
# seven, so the upper edge FIRED on this reading on its own -- like 96 -> 121 and
# 276 -> 313, and correctly, since this is a whole new section rather than
# assertions over existing fixtures.
# Measured 2026-08-19 by running tests/build/test_intel_scanout.exe, the FIRST
# reading of a suite that did not exist before this date:
#   test_intel_scanout 61 checks, 0 failures
# This entry is a new suite rather than a movement, so there is no previous
# ceiling for the upper edge to fire against and none is claimed. What is worth
# recording is WHY the number is a measurement rather than a guess: the header
# phase0-kext/IntelScanout.hpp was landed on 2026-08-18 with its own comment
# naming tests/test_intel_scanout.cpp as its suite, and that file did not exist.
# The header was registered in ZERO of its four places -- not parsed by section 1
# of tests/check-kext-syntax.bat, not built by tests/CMakeLists.txt, not listed
# in tests/test_check_count_last_line.py, and absent from this table -- so every
# function in it was a pure function nothing ran, which is the exact class
# CLAUDE.md's logic/IOKit split exists to prevent and the exact class
# IntelDisplayNames.hpp cost a live defect over.
#
# The 61 comes from eight sections, named rather than split by count. An earlier
# revision of this paragraph DID carry a per-section split, and it was wrong in
# two ways at once: it totalled a draft of the suite that no longer exists (58),
# and its per-section numbers were hand-derived rather than read off a run. That
# is the `prose-carrying-a-derived-value-goes-stale-silently` finding inside the
# one module written to warn about it, so what is recorded here is the SHAPE:
#
#   1   the verdict family and the policy constants, every one a bare literal
#   2   igScanSurfRefused()'s polarity, address masking, alignment
#   4   the two 64-bit products, driven past 0xFFFFFFFF
#   5   both edges of the aperture containment test
#   5b  each refusal arm reached by a SINGLY-bad input
#   5c  the last ACCEPTING value of each numeric test, and one byte past it
#   6   igScanDescribe()'s refusal ORDER, doubly-bad inputs
#   7   the usability walk and the 1920x1080 positive control
#
# The only authority for the total is running the binary. Sections 5b and 5c are
# the complement of section 6 and neither substitutes for the other: an ordering
# check names some OTHER arm as the winner, so it stays green over an arm that is
# provably unreachable, and a refusal test pinned only on its refusing side is
# satisfied by a guard that refuses everything.
#
# The seven ordering checks are the ones a future edit is most likely to break,
# and each is driven by an input that is bad in TWO ways at once -- an input bad
# in only one way cannot distinguish the order from any other. The three that
# matter: the MASKED sentinel is 4 KiB ALIGNED, so an alignment-first order
# reports a read that never happened as a well-formed plane; a zero width makes
# the minimum row zero and every stride satisfies >= 0, so a containment-first
# order calls a zero-width plane adequately strided; and a wrapped extent is a
# SMALL extent that fits inside any aperture, so testing containment before
# overflow makes the overflow arm unreachable for precisely the inputs that need
# it. That last one is the reason the header exists as a separate file.
# Re-measured 2026-08-19, by running tests/build/test_gsp_boot.exe after
# testRiscvErrorSplit() was added to test_gsp_boot.cpp and wired into main()
# between testTimeouts() and testStepDriving():
#   test_gsp_boot 368 checks, 0 failures
#
# nvGspBootCheckRiscvErrors() used to stop on nvRiscvHasError(), a one-line
# wrapper over nvRiscvErrorVerdict() that answers true for FOUR of its five
# codes at once -- so a boot halted because a half did NOT DECODE was reported
# as ERR_RISCV_FAULT, a claim about the card made from evidence about this
# driver's mapping. Section 7 already drove the fault arm and could not see it:
# a refused read and a set latch both satisfy `!= NVRISCV_ERR_NONE`. The new
# section pairs each half's refused run against a genuinely-latched one at the
# same construction, because a refusal test pinned only on its refusing side is
# satisfied by a machine that reports UNREADABLE for every nonzero reading.
#
# 368 lands INSIDE the band (341 + max(8, 34) = 375) by seven, so the ceiling
# correctly did not fire.
#
# Two mutations, planted and reverted separately, each read off a run:
#   A  restore the collapse (stop on nvRiscvHasError) -- 3 checks, all three in
#      section 7b, all three NEW, every pre-existing check green. That is
#      handoff18 s1e's signal: a mutation reddening a pre-existing check would
#      have proved the old tests already sufficed.
#   B  weaken nvGspBootErrVerdictKnown()'s conjunction to the PRIV bit alone --
#      exactly 1 check, and it is the arm naming the missing half. B exists
#      because A cannot isolate it: every run in this file that reads PRIV also
#      reads HUB (the refusal VALUE arrives through a read that SUCCEEDED), so
#      the conjunction is driven over readMask by hand rather than through a
#      run, including the far-side case of every OTHER bit set.
#
# Re-measured 2026-08-19, by running tests/build/test_intel_display.exe after a
# SECOND observed block was harvested into IntelDisplay.hpp out of
# igpu-results/tgl-final-capture-2026-08-18-201412.txt -- ten configuration
# words (CDCLK_CTL, SKL_DFSM, SOUTH_CHICKEN1, SFUSE_STRAP, ICL_DPCLKA_CFGCR0,
# four panel-power dwords, three backlight PWM dwords), one anchored field mask
# and igCdclkFreqDecimal(), with testObservedConfig20260818() wired into main():
#   test_intel_display 480 checks, 0 failures
# What this section pins is mostly the EXCLUSIONS. Three section 6 reads look
# exactly as harvestable as the ten and are absent from the header on purpose:
# _DDI_BUF_CTL_A read 0x80000087 and 0x8000008F in the SAME run because bit 3 is
# DDI_BUF_IS_IDLE, a live status bit -- so a value pinned there would be
# whichever pass landed first -- and PCH_DREF_CONTROL and _TRANS_DP_CTL_A both
# read 0xFFFFFFFF, which igReadRefused() calls a refusal rather than a reading.
# Two of those three exclusions are decidable by a predicate this suite already
# had, so they are asserted rather than left as prose in a comment.
#
# 480 is ABOVE the previous entry's ceiling (421 + max(8, 42) = 463) by
# seventeen, so the band FIRED on this reading rather than absorbing it. That is
# the second consecutive Intel reading to fire, after three in a row that landed
# just under the edge.
# Re-measured 2026-08-19, by running tests/build/test_decode.exe after
# testChipVerdict() was added to test_decode.cpp and wired into main() directly
# below testPartVerdict():
#   test_decode 148/148 passed, 0 failed
#
# decodeBoot42() detected four facts about the die -- an unexpected dead read,
# the composite chip ID matching GA107, it not matching, and the not-matching
# value being GA106 SPECIFICALLY -- and wrote all four to the log only, while the
# IORegistry carried NV_PMC_BOOT_42 as a raw word beside NVChipIdExpected and
# never compared the two. nvChipVerdict() is those four findings as a value, and
# the new section drives every arm plus the two orderings that are load-bearing.
#
# Four of the checks exist because a mutation LIED. Hoisting the dead-read test
# below the two chip-ID equality arms reddened NOTHING, and the honest reading is
# that the claim was false rather than the tests vacuous: CHIP_ID is ten bits, so
# a dead read decodes to 0x3FF or 0x000 and neither can equal GA107 (0x177) or
# GA106 (0x176). Only the position relative to the general FALL-THROUGH is
# load-bearing -- A1 (delete the dead-read test) and A2 (move it below the
# general return) each redden exactly 2 -- so the non-collision is now asserted
# through nvBoot42ChipId() rather than left as arithmetic in a comment, and a
# future chip ID landing on either value reddens.
#
# 148 is ABOVE the previous entry's ceiling (121 + max(8, 12) = 133) by fifteen,
# so the band FIRED on this reading rather than absorbing it.
# Measured 2026-08-20 by running tests/build/test_intel_pixel.exe, the FIRST
# reading of a suite that did not exist before this date:
#   test_intel_pixel 121 checks, 0 failures
# A new suite rather than a movement, so there is no previous ceiling for the
# upper edge to fire against and none is claimed.
#
# The subject is phase0-kext/IntelPixel.hpp, and its reason for existing is one
# collision: igFbBytesPerPixel() returns 4 for BOTH IG_FB_FORMAT_XRGB_8888 and
# IG_FB_FORMAT_XRGB_2101010, while IOPixelInformation needs 8 bits per component
# with masks 0x00FF0000/0x0000FF00/0x000000FF for the first and 10 bits with
# 0x3FF00000/0x000FFC00/0x000003FF for the second. A component width DERIVED
# from bytes-per-pixel is right for one format and wrong for the other, and the
# wrong direction is the quiet one -- a 2101010 surface described as 8-8-8
# programs cleanly and yields wrong COLOURS rather than a broken picture, which
# is handoff15 §1's byte-swapped PLANE_SIZE hazard in a second place.
#
# The 121 comes from seven sections, named rather than split by count, for the
# reason the test_intel_scanout entry above spells out -- a per-section split in
# a comment is a derived value nothing keeps true:
#
#   1   the constants and both mask families, every one a bare literal
#   2   the bytes-per-pixel COLLISION, and that component widths do not collide
#   3   describability as a whitelist, probed at the codes a range would admit
#   4   igPixelDescribe()'s order: UNKNOWN_FORMAT before UNDESCRIBABLE
#   5   the three scalars refusing with a sentinel no real answer can take
#   6   masks by value, width, contiguity, disjointness and the absent X channel
#   7   the usability walk and the measured-PLANE_CTL positive control
#
# Section 7b is worth knowing about before editing it. Its first draft drove
# igFbFormat(0x84000000u) -- a PLANE_CTL word I composed to look plausible --
# and the control went RED, because that word decodes to format 8, which this
# tree does not name. The measured word is 0x82009000, read off
# igpu-results/tgl-capture-2026-08-16-022556.txt line 134, and it decodes to 4
# (XRGB8888). The failure was loud only because igPixelDescribable() is a
# whitelist; a range test would have accepted the invented word and published a
# component layout for a surface nobody measured. The same section now also pins
# that the FOUR-bit reading of that word differs from the five-bit one -- Tiger
# Lake is icl+, so bit 23 is part of the format field, and (0x82009000 >> 24) &
# 0x0F is 2, which is ALSO a format this header names. A narrowed mask therefore
# yields a plausible wrong answer rather than an obvious one.
# Measured 2026-08-20 by running tests/build/test_intel_fb_present.exe, the FIRST
# reading of a suite that did not exist before this date:
#   test_intel_fb_present 140 checks, 0 failures
# A new suite rather than a movement, so there is no previous ceiling for the
# upper edge to fire against and none is claimed.
#
# The subject is phase0-kext/IntelFbPresent.hpp, and it is the JOIN rather than a
# new decode layer: four plane words plus an aperture size turned into the numbers
# an IOFramebuffer subclass hands IOKit, or a refusal. Everything it computes is
# owned by a sibling header, so what this suite covers is the ORDER of the four
# refusal arms and the property that NO accessor answers for a group the verdict
# refused -- section 7 walks all eight accessors against all eight refusing input
# classes for that reason.
#
# Two of the 140 are worth naming because the FIRST draft of this suite got them
# wrong in the direction that reads as green. The captured PLANE_CTL 0x82009000 has
# bit 15 SET -- PLANE_CTL_RENDER_DECOMP -- so igFbInheritable() returns COMPRESSED
# at its second arm and the observed word can NEVER be the accept-path input. The
# accept word is therefore COMPOSED (0x82009000 with bit 15 cleared = 0x82001000),
# and section 4b pins that derivation by three separately-failing claims rather
# than describing it in a comment. And the stride unit is 128 bytes, not 512: the
# first draft wrote field 16 with a comment reading "16 * 512 = 8192", which is
# 2048 bytes and SHORT of a 1280 * 4 = 5120-byte row, so the accept path refused at
# STRIDE as well. Both were comments carrying arithmetic nothing executed, which is
# `prose-carrying-a-derived-value-goes-stale-silently` inside a brand-new file; the
# byte count is now CHECKED.
#
# Measured 2026-08-20 by running tests/build/test_decode.exe after the C++ gate
# rebuilt it:
#   test_decode 186/186 passed, 0 failed
# 186 is ABOVE the previous entry's ceiling (148 + max(8, 18) = 166) by twenty, so
# the band FIRED on this reading rather than absorbing it.
#
# The growth is one new section, testFbSizeVerdict(), over the NVFbSizeVerdict
# family added to phase0-kext/NVDecode.hpp in the same session. It exists because
# nvFbSizeIsPlausible() is a BOOL and `false` is satisfied by all three refusals at
# once -- the class 3050-handoff34.md section 1 records one subsystem over. The
# defect it removes is in the IOKit shell: NV_USABLE_FB_SIZE_IN_MB_VALUE_MASK is
# 0xFFFFFFFF, so a REFUSED read masks to 4294967295, and publishFirmwareState()
# decided whether to publish NVUsableFramebufferMB from the MAGNITUDE of that
# number. 4294967295 is greater than zero, so a word the driver had itself
# diagnosed as not decoding reached the IORegistry byte-identically to a real 4096.
#
# Two of the section's checks are ORDER pins rather than value pins: 0xFFFFFFFF is
# ALSO implausible and 0 is ALSO implausible, so an implausibility-first subject
# returns IMPLAUSIBLE for both and exactly those two checks move.
# Measured 2026-08-25 by running tests/build/test_intel_gt_cmd.exe, the FIRST
# reading of a suite that did not exist before this date:
#   test_intel_gt_cmd 120 checks, 0 failures
# A new suite rather than a movement, so there is no previous ceiling for the
# upper edge to fire against and none is claimed.
#
# The subject is phase0-kext/IntelGtCmd.hpp, and it is the first header in this
# tree that is not about display: Gen12 render-engine command submission. Its
# evidence story is split and the header says so itself -- the four engine bases
# and the +0x30 ring register block are citable to reference/i915/i915_reg.h
# (RENDER_RING_BASE at :289, and PRB0_BASE spelled (0x2030 - 0x30), which is what
# fixes the ring block ABOVE the base rather than at it), while the MI opcode
# family is POLICY because i915 keeps it in gt/ headers never vendored here. So a
# green 120 proves the composition is self-consistent and proves NOTHING about
# whether the hardware wants these words.
#
# No *_OBSERVED_* constant may enter that header. igpu-results/ holds display
# registers captured from a machine whose render engine was IDLE, so this tree has
# no MMIO capture of a running render engine, and a policy value renamed to look
# measured destroys the only evidence trail the project has.
#
# Six of the 120 are ORDER pins rather than value pins, each driven by an input
# bad in TWO ways at once -- the only way an arm order is assertable at all:
#   * a bad engine beside a nonsense ring reports the ENGINE;
#   * a head outside an UNALIGNED ring reports the RING, because a head is only
#     "out of the ring" relative to a size that was never legal;
#   * a tail that is both out of range and unaligned reports OUT OF RANGE, since
#     the alignment of a value outside the buffer is not a meaningful complaint;
#   * length == 0 reports EMPTY, not UNALIGNED_LENGTH, because zero is trivially a
#     multiple of the command word and a multiple-first order makes
#     IG_GT_SUBMIT_EMPTY unreachable -- the dead-arm shape 3050-handoff32.md
#     section 3 exists to remove, reintroduced one subsystem over.
# Two more pin igGtRingSizeVerdict()'s alignment-before-minimum order, since zero
# is page-aligned and would otherwise be refused as unaligned instead of small.
#
# The verdict-name and engine-name loops bound on IG_GT_SUBMIT_LIMIT and
# IG_GT_ENGINE_LIMIT rather than a typed count, so a family that grows reddens on
# the commit that grows it. That matters more here than in the display headers:
# IntelGtCmd.hpp is NOT one of the two Intel headers tools/check-registers.py
# parses, so IG_GT_SUBMIT_ gets no set-check from the register gate at all.
# Measured 2026-08-26 by running tests/build/test_intel_gt_exec.exe, the FIRST
# reading of a suite that did not exist before this date:
# (first reading: 99 checks). A new suite rather than a movement, so there was
# no previous ceiling for the upper edge to fire against and none was claimed.
#
# Moved 2026-10-12, 99 -> 102, by a BUG FIX rather than growth: the PTE address
# mask was 47:12 and is now GENMASK_ULL(45,12) (intel_gtt.h:100), and the
# software context id moved from bit 32 to bit 37 (intel_lrc.h:103). Three
# checks were added -- the old 47:12 ceiling and bit 46 are now REFUSED by both
# igGtxPageTableEntry and igGtxPageAddressOk -- and three literals changed.
# Measured by running the rebuilt suite:
#   test_intel_gt_exec 102 checks, 0 failures
# Re-measured 2026-10-12 after explicit render descriptor composition:
#   test_intel_gt_exec 123 checks, 0 failures (21 new checks).
#
# The subject is phase0-kext/IntelGtExec.hpp, the execlist/GTT layer directly
# above IntelGtCmd.hpp. Its evidence story is WEAKER than every other entry in
# this table and the suite prints that as its own second banner line: every
# constant in that header is POLICY, with no cited half at all.
#
# THAT CLAIM WAS RE-MEASURED ON 2026-08-26 AND HALF OF IT IS NOW FALSE. It used
# to read: reference/i915/ holds seven files, all seven are display, and greps
# for EXECLIST, ELSP, LRC, CTX_DESC, GEN8_PDE, GEN8_PTE, PPAT and GGTT across
# that whole directory return ZERO matches. That was true when written and
# stopped being true the moment three gt/ headers were vendored later the same
# day -- intel_lrc_reg.h, intel_engine_regs.h and intel_gpu_commands.h, listed in
# reference/i915/PROVENANCE.md with their upstream paths and byte counts. The
# same greps now return, per file: intel_lrc_reg.h 5, intel_engine_regs.h 2,
# every display file still 0.
#
# The correction matters more than the number. A provenance comment carrying a
# measurement goes stale SILENTLY -- nothing executes a sentence in a comment --
# and this table exists because of exactly that class (see the FLOORS provenance
# note and handoff17 section 1). The honest reading is that the ZERO was a fact
# about what had been FETCHED, never a fact about what upstream documents, and
# a sentence phrased as though it were the second is the trap.
#
# What has NOT changed is this entry's subject. IntelGtExec.hpp is the execlist
# port and GTT layer, and the newly vendored headers do not cover it: the
# execlist submit-port registers and the Gen8 page-table entry format live in
# gt/intel_execlists_submission.c and gt/intel_gtt.h, neither of which is here.
# So every constant in IntelGtExec.hpp is still POLICY and 99 green checks still
# establish that the composition is self-consistent and NOTHING about whether
# silicon agrees with a single offset in it. The entry two below this one is the
# one whose evidence story actually improved.
#
# Two sentinel choices in that header are the INVERSE of the rest of the tree,
# and four of the 99 checks exist to pin exactly that. A refused context
# descriptor and an absent page-table entry each answer ZERO rather than
# all-ones, because bit 0 of both words is the valid/present bit: zero is
# invalid by the hardware's own definition, while all-ones is a VALID entry
# pointing at the top of the address space -- the scribble direction. The suite
# therefore drives an all-ones descriptor through BOTH halves and requires them
# to SPLIT it rather than refuse, which pins that the refusal is keyed on that
# bit rather than on a hand-rolled sentinel comparison that would happen to
# agree on every other input.
#
# The most dangerous single bit in the header is the port-busy polarity, pinned
# on both sides plus the refused-read case: a refused MMIO read is 0xFFFFFFFF,
# which has the busy bit SET, so a refusal reports BUSY -- the safe direction,
# because busy means do not write the queue. A predicate testing for the clear
# state instead would read a refused status as idle and overwrite a submit queue
# the scheduler is still consuming.
#
# Three ordering claims are each pinned by an input bad in TWO ways at once, and
# the last is the load-bearing one: ring geometry is judged BEFORE the port,
# because a ring defect is a bug in this driver while a busy port is a fact about
# the machine one millisecond from now. Report the one somebody can go and fix --
# handoff22 section 1b's rule applied to a transient rather than to a mapping.
#
# Like its sibling this header is NOT one of the two Intel headers
# tools/check-registers.py parses, so IG_GTX_READY_ gets no set-check from the
# register gate at all. The verdict-name loop bounds on IG_GTX_READY_LIMIT rather
# than a typed count, so a family that grows reddens on the commit that grows it.
# Measured 2026-08-26 by running tests/build/test_intel_gt_context.exe, the FIRST
# reading of a suite that did not exist before this date:
#   test_intel_gt_context 124 checks, 0 failures
# A new suite rather than a movement, so there is no previous ceiling for the
# upper edge to fire against and none is claimed.
#
# The subject is phase0-kext/IntelGtContext.hpp, the LOGICAL RING CONTEXT image
# layer directly above IntelGtExec.hpp. It carries the same evidence story as the
# entry above -- weaker, in fact, because its subject had NO cited half at all.
#
# THAT LAST CLAUSE WAS TRUE FOR ABOUT SIX HOURS. It used to read: greps for LRC,
# CTX_DESC, RING_BUFFER_START, RING_BUFFER_CTL, BB_PER_CTX and CTX_TIMESTAMP
# across the whole of reference/i915/ return ZERO matches. Three gt/ headers were
# vendored later on 2026-08-26 and the same greps now return 5 in
# intel_lrc_reg.h and 2 in intel_engine_regs.h, every display file still 0. See
# reference/i915/PROVENANCE.md for the upstream paths and byte counts.
#
# THIS ENTRY'S SUBJECT IS THE ONE THAT ACTUALLY GAINED A CITATION, and the shape
# of the gain is worth more than the citation. intel_lrc_reg.h spells
# CTX_RING_HEAD as (0x04 + 1), and the + 1 names the index of the VALUE dword --
# so the OFFSET dword sits at slot 0x04 and the pair occupies 4/5. Read that way
# upstream gives HEAD 4/5, TAIL 6/7, START 8/9, CTL 10/11, which is EXACTLY what
# IntelGtContext.hpp already chose as policy and exactly what the defect note
# below records this suite's own crash correcting it to. The layout was derived
# here independently and then corroborated, which is strictly better evidence
# than reading it off the header would have been -- two derivations agreeing is
# a measurement, one derivation copied is a transcription.
#
# The 124 checks still establish arithmetic self-consistency and refusal order
# and still establish NOTHING about THIS Gen12 part: igpu-results/ captured a
# render engine that was IDLE, so no slot in this image has been witnessed. A
# constant citable to these headers is I915, never OBSERVED. The suite prints
# that caveat ABOVE its own count so a reader cannot mistake the number for
# confidence.
#
# Roughly eighteen of the 124 exist because of a defect CORRECTED on 2026-08-26,
# in the same commit that added them. The register-state page opens with a NOOP
# dword at slot 0 and the load-immediate header at slot 1, so the first (offset,
# value) pair begins at slot 2 -- and the header first shipped with the LRI at
# slot 0 and the four ring-state slots at 0x02/0x04/0x06/0x08, one pair too low
# throughout. That does not fault on hardware. It slides every pair down one
# dword, so the engine reads each register OFFSET out of the previous pair's
# VALUE slot and programs registers nobody chose. The four slots are now
# 0x04/0x06/0x08/0x0A and each is pinned to its own BARE LITERAL, asserted
# pairwise distinct, and asserted EVEN and at-or-above the first pair slot -- an
# odd value would name a value dword as though it were an offset.
#
# The value dwords are pinned to bare literals (0x05/0x07/0x09/0x0B) rather than
# derived as slot + 1, for the reason this whole table exists: an expectation
# computed the same way the accessor computes its answer moves with it and stays
# green. igGtcPairValueSlot() refuses the NOOP slot and the LRI header slot
# rather than answering, because slot 1 + 1 is 2 -- the first pair's offset
# dword, a perfectly valid slot and completely wrong -- and refuses slot 1023,
# whose value dword would fall off the end of the page.
#
# The refusal sentinel here is ALL-ONES, the inverse of the entry above, and the
# inversion is load-bearing rather than stylistic: zero is a LEGAL slot offset
# because the leading NOOP lives at slot 0, so zero cannot carry the meaning
# "refused" in this family, while in a descriptor word zero is illegal by the
# hardware's own valid bit. The suite pins that the sentinel PROPAGATES through
# igGtcSlotOffsetFromLrca() rather than having 0x1000 added to it -- adding wraps
# it to 0xFFF, which is small, 4-byte aligned, and inside every containment test
# in the header: a refusal that reads as an answer.
#
# Like both siblings beneath it this header is NOT one of the two Intel headers
# tools/check-registers.py parses, so IG_GTC_IMAGE_ gets no set-check from the
# register gate at all. The verdict-name loop bounds on IG_GTC_IMAGE_LIMIT rather
# than a typed count, so a family that grows reddens on the commit that grows it.
# Measured 2026-08-26 by running tests/build/test_intel_gt_image.exe, the FIRST
# reading of a suite that did not exist before this date:
#   test_intel_gt_image 111 checks, 0 failures
# A new suite rather than a movement, so there is no previous ceiling for the
# upper edge to fire against and none is claimed.
#
# The subject is phase0-kext/IntelGtImage.hpp, the image WRITER directly above
# IntelGtContext.hpp. Its evidence story used to be stated as: greps for LRC,
# CTX_DESC, RING_BUFFER_START, RING_BUFFER_CTL, BB_PER_CTX and CTX_TIMESTAMP
# across the whole of reference/i915/ return ZERO matches. THAT IS NO LONGER
# TRUE. Three gt/ headers were vendored later on 2026-08-26 and the same greps
# now return 5 in intel_lrc_reg.h and 2 in intel_engine_regs.h, every display
# file still 0. Third occurrence of the same stale sentence in this one file,
# which is itself the finding: a measurement copied into three provenance blocks
# goes stale in three places at once and nothing executes any of them.
#
# The citation reaches this entry's subject UNEVENLY and the split is the point.
# The four ring slots and the slot 2/3 CTX_CONTEXT_CONTROL pair the writer
# composes are now citable (intel_lrc_reg.h, and RING_CONTEXT_CONTROL at
# (base) + 0x244 in intel_engine_regs.h). The LRI header word this file builds is
# citable too -- MI_LOAD_REGISTER_IMM(x) is MI_INSTR(0x22, 2*(x)-1) in
# intel_gpu_commands.h, which is the dword-count encoding IntelGtCmd.hpp derives
# independently. What remains POLICY is the ring GEOMETRY: no ring is allocated
# by this driver, so IG_GTP_RING_BASE_POLICY and IG_GTP_RING_BYTES_POLICY have no
# citable half and cannot acquire one from a header. The suite prints that caveat
# ABOVE its own count.
#
# What separates it from its siblings is that its subject MUTATES CALLER MEMORY,
# which admits a failure mode pure arithmetic does not have: a refusal that has
# already written. A half-composed page is not a broken page -- it is a
# well-formed image describing a ring nobody configured, the quiet direction
# because it programs cleanly. So the span is POISONED with a value the writer
# cannot produce at any slot, and after every refusal class the WHOLE span is
# asserted still poisoned. A suite asserting only the return code cannot
# distinguish those two outcomes at all.
#
# TWO DEFECTS IN THE HEADER WERE MEASURED BY THIS SUITE'S OWN CRASH, and both are
# recorded here rather than quietly corrected, because the defect and its
# detection are the same fact.
#
# First, IG_GTI_RING_IMAGE_DWORDS was 10 and the image is TWELVE dwords:
# IntelGtContext.hpp puts the CTL pair at slots 10 and 11, so ten dwords reaches
# only slot 9. The length guard therefore ADMITTED a span two dwords too small
# and then stored past the end of it -- a refusal that should have fired,
# followed by a write into memory the caller never offered. It surfaced as
# "Run-Time Check Failure #2 - Stack around the variable 'tight' was corrupted"
# because the tight-span case is driven through a STACK array; a heap span or a
# roomier buffer would have taken the two stores silently. The ten-dword span is
# now pinned as its own refusal arm.
#
# Second, the LRI header declared FOUR pairs while the composer skipped slots 2
# and 3. The engine reads pairs CONTIGUOUSLY from the slot after the header, so
# that does not describe a page with a hole in it: it would take whatever sat at
# 2/3 as the first register offset and walk the rest of the image one pair out of
# step, every dword after that naming a register nobody chose. handoff15's
# byte-swapped PLANE_SIZE hazard one subsystem over -- it programs cleanly.
# The pair count is now 5 and the first pair is CALLER-SUPPLIED, because this
# tree cannot cite what it holds and inventing a name would file a guess beside
# the four ring offsets that ARE citable to reference/i915/i915_reg.h. A zero
# offset is REFUSED rather than written, since zero is the caller's initialiser.
#
# Like the three headers beneath it this one is NOT parsed by
# tools/check-registers.py, so IG_GTI_WRITE_ gets no set-check from the register
# gate at all. The verdict-name loop bounds on IG_GTI_WRITE_LIMIT rather than a
# typed count, so a family that grows reddens on the commit that grows it.
# Measured 2026-08-28 by running tests/build/test_intel_gt_blt.exe, the FIRST
# reading of a suite that did not exist in a BUILDABLE form before this date:
#   test_intel_gt_blt 213 checks, 0 failures
# A new suite rather than a movement, so there is no previous ceiling for the
# upper edge to fire against and none is claimed.
#
# The qualifier "in a buildable form" is doing real work and is not modesty. A
# file of this name existed and was registered in tests/CMakeLists.txt, but it
# was authored against an EARLIER SKETCH of phase0-kext/IntelGtBlt.hpp and did
# not compile: it called igGtbWriteXyColorBlt / igGtbWriteXySrcCopyBlt where the
# header spells igGtbWriteColorBlit / igGtbWriteCopyBlit, expected an EIGHT-dword
# copy packet where the header writes ten, and omitted the source origin and the
# depth flag from the copy call entirely. It therefore had no entry here and
# could not honestly have had one -- a suite that does not build has no check
# count, and a number typed in beside a red build is the exact defect this gate
# exists to catch (a-measured-claim-can-be-false-on-day-one).
#
# The header was treated as the authority on a substantive argument rather than
# on convenience: the seven-dword fill already carries a SIXTY-FOUR-BIT
# destination address, low half in dword 4 and high half in dword 5, so an
# eight-dword copy cannot also carry a second 64-bit address, a source pitch and
# a source origin. Seven and ten are the Gen8+ 64-bit-address forms.
#
# Three of this suite's own first-draft expectations were WRONG and the subject
# was right in all three, which is worth recording because each is a class:
#   1. Six verdict constants were guessed from sibling families rather than read
#      out of the header (SPAN_SHORT for SPAN_TOO_SHORT, RECT_EMPTY for
#      EMPTY_RECT, RECT_RANGE for COORD_TOO_LARGE, PITCH_RANGE for
#      PITCH_TOO_LARGE, SURFACE_ALIGN for ADDR_UNALIGNED) and the admissibility
#      predicate is igGtbBlitSucceeded, not igGtbBlitOk. These failed loudly at
#      compile time, which is the harmless direction.
#   2. The header pins were composed against a bare 0u flags argument, which
#      would have passed on a writer that dropped the RGBA write mask -- and a
#      packet with no write mask enables no colour channels, so it executes
#      cleanly and paints NOTHING. The mask is ORed into the header word, not
#      into BR13.
#   3. igGtbRectVerdict() tests INVERTED before the coordinate ceiling, not
#      after. The draft asserted the reverse. The two orders are
#      indistinguishable on every input bad in only ONE way, which is every
#      other check in that section, so the ordering claim needs a rectangle that
#      is inverted AND out of range at once, plus one empty AND out of range to
#      pin the middle arm.
#
# Like the four headers beneath it this one is NOT parsed by
# tools/check-registers.py, so IG_GTB_BLIT_ gets no set-check from the register
# gate at all. The verdict-name loop bounds on IG_GTB_BLIT_LIMIT rather than a
# typed count, so a family that grows reddens on the commit that grows it.
# Measured 2026-08-28 by running tests/build/test_intel_gt_batch.exe, the FIRST
# reading of a suite that did not exist before this date:
#   test_intel_gt_batch 234 checks, 0 failures
# A new suite rather than a movement, so there is no previous ceiling for the
# upper edge to fire against and none is claimed.
#
# RE-MEASURED 2026-08-28, same day, same command:
#   test_intel_gt_batch 289 checks, 0 failures
# The
# growth is not new subject matter -- it is a REPAIR whose reading is worth
# recording, because the defect it repairs is a tooling class rather than a code
# one. An Edit adding sections 5-7 anchored its old_string on the OPENING lines
# of main(), which by then had grown further calls beneath them. old_string
# matches any unique substring, so the anchor still matched as a PREFIX, and the
# edit inserted a second copy of the terminator and predicate sections ABOVE the
# originals while reporting success. Two symbols collided outright (fillSpan and
# testIsTerminated), so the suite did not compile at all -- which is the harmless
# direction, and the only reason the duplication was visible. Had the new
# sections carried distinct names throughout, the file would have compiled, run
# both copies, and reported a doubled count that looked like coverage.
#
# 234 -> 289 is +55 and lands ABOVE this entry's own ceiling
# (234 + max(8, 23) = 257) by thirty two, so the upper band FIRED on its own and
# forced this entry to be re-measured rather than silently absorbing the change.
# That is the band working in the direction it exists for: a count moving by a
# third is a claim about the suite that somebody has to look at.
#
# The duplicated sections were KEPT rather than deleted, because they are not a
# verbatim second copy -- they carry three assertions the 234-check reading did
# not have, and each is about a boundary the originals never reached:
#   1. usedDwords == 0xFFFFFFFF, where usedDwords + need WRAPS. The span check is
#      written as (dwordCount - usedDwords) < need guarded by
#      dwordCount < usedDwords precisely so the subtraction cannot wrap, and
#      nothing previously drove the input that proves the guard is load-bearing.
#   2. A bare MI_BATCH_BUFFER_END with no pad reads as terminated. The predicate
#      walks back over trailing NOOPs, and a walk that started one dword early
#      would still pass every padded case in the file.
#   3. An all-NOOP span must not walk off the front of the buffer. The loop
#      bounds on i > 0u, and an off-by-one there reads span[-1].
# The retained originals are renamed rather than merged: testIsTerminatedExtra
# is the second one, and both run. Merging them would have meant deciding which
# of two overlapping bodies was authoritative on a suite that had just been
# proven unreliable to edit blind.
#
# Its subject, phase0-kext/IntelGtBatch.hpp, is the JOIN between the packet
# layer and the delivery layer and is about neither one. IntelGtBlt.hpp composes
# what the engine would DO; IntelGtCmd.hpp composes the ring geometry and the MI
# commands that DELIVER it. A caller holding a composed blit had no admissible
# way to ask where it goes, what terminates it, and what the ring must hold for
# the engine to find it -- and all three questions have wrong answers that
# produce a well-formed, executable, catastrophic result.
#
# The verdict prefix is IG_GTQ_ rather than IG_GTB_ deliberately.
# intel_verdict_set_checks() in tools/check-registers.py groups families BY
# PREFIX, so a family sharing the blitter's prefix would sweep the blitter's
# constants into this one's contiguity check -- handoff16 section 1's
# IG_PIPE_TRANSCONF trap, a failure correct about its own arithmetic and wrong
# about the world. Neither header is parsed by the register gate, so IG_GTQ_ has
# no set-check there either; the verdict-name loop bounds on
# IG_GTQ_BATCH_LIMIT rather than a typed count, so a family that grows reddens
# on the commit that grows it.
#
# The load-bearing assertions in this suite are the OUT-PARAM ones rather than
# the return values. A ring tail advanced by a count left over from a REFUSED
# compose walks the tail into dwords the engine executes as commands, so every
# refusal arm asserts a written count of zero AND an untouched span. The
# sharpest of them is the no-room-to-terminate case, because the BLIT SUCCEEDED
# there -- reporting its count is exactly what would dispatch a batch with no
# MI_BATCH_BUFFER_END in it, and an unterminated batch does not stop: the
# command streamer keeps fetching past the last packet and executes whatever the
# allocation happened to contain.
#
# Two of this suite's assertions pin an ORDER that is invisible to any input bad
# in only one way, which is the class the blitter suite's own third first-draft
# error belongs to (recorded above). igGtqWriteTerminator() must refuse a short
# span BEFORE an empty batch, so the case is a span of 1 with a used count of 0
# -- bad both ways at once. igGtqWriteBatchStart() must refuse a short span
# BEFORE a misaligned address, so the case is a span of 2 with an address of
# 0x1001. Either ordering satisfies every single-fault input in the file.
#
# The alignment rule itself is NOT restated here. Section 7 walks the four
# low-bit cases and requires igGtqWriteBatchStart() to AGREE with
# igGtBatchAddressOk() on each, so the two cannot drift apart -- a second
# spelling of a rule agrees with the first until one of them moves.
#
# Measured 2026-08-29 by running test_intel_sink_surface.cpp built with
# g++ -std=c++11 -Wall -Wextra -Werror -I../phase0-kext:
#   test_intel_sink_surface 20 checks, 0 failures
# The first draft had 19 checks, one below STATIC_CALL_FLOOR. The twentieth check
# is not padding. "extent overflow beats aperture" pins the one adjacent pair in
# the refusal order that no other doubly-bad input covered: a 32-bit extent
# overflow must be reported before running off the GGTT, because an extent that
# wrapped would otherwise compare as small and could pass the aperture test.
#
# Measured 2026-10-11 by running tests/build/test_intel_sink_publish.exe (built
# 2026-09-27, newer than its source). Its table entry predated any reading line:
#   test_intel_sink_publish 23 checks, 0 failures
#
# Measured 2026-10-11, sink plan step (b) and the alloc half of step (a), each
# built with g++ -std=c++11 -Wall -Wextra -Werror and again through MSVC:
#   test_intel_plane_program 91 checks, 0 failures
#   test_intel_sink_alloc 21 checks, 0 failures
# plane_program had 17 checks and a bare check() helper, which CALL_RE counts as
# ZERO call sites. sink_alloc had 16, the same bare helper, and a count line
# with no leading newline -- matching neither COUNT_FORMATS pattern. All three
# are the handoff40 section 3 class: the gate reads spellings, not intent.
#
# Measured 2026-10-12, the sink rehearsal (handoff41 section 5 item 1), built
# with g++ -std=c++11 -Wall -Wextra -Werror:
#   test_intel_sink_rehearse 60 checks, 0 failures
# A first measurement: the header is new, so there is no prior number.
MEASURED = {
    # Measured 2026-08-28 by running tests/build/test_intel_gt_ring.exe, the FIRST
    # target ever to compile phase0-kext/IntelGtRing.hpp:
    #   test_intel_gt_ring 331 checks, 0 failures
    # Before this reading the header was 413 lines that no suite drove and no shell
    # called, so 331 is a first measurement rather than a growth -- there is no
    # prior number for the band to be checked against and none is claimed.
    #
    # The FIRST reading of this suite was not this number and not in this format.
    # main() ended on a name-prefixed line, which parses as prose rather than as a
    # count: tests/test_check_count_last_line.py knows exactly two formats and a
    # third spelling made _format_index() return None, so an index into an empty
    # list raised rather than failed. Two of that module's checks ERRORED and the
    # floor gate could not read the binary's own output at all -- a suite that runs
    # clean and reports 331 while every gate over it says the count is missing.
    # The lesson is narrow and worth keeping: the final printf is an INTERFACE, and
    # a suite name in it is not a decoration.
    #
    # Most of the count is loops rather than hand-written cases, which is why it
    # lands high for a seven-group suite: the verdict family walks pairwise
    # distinctness over eight verdicts, and the pad group walks eight tails by
    # eight payload lengths asserting three properties each. Those two account for
    # roughly two hundred of the 331, so a future edit that deletes a whole GROUP
    # of hand-written store assertions moves this number by less than deleting one
    # loop bound -- read a drop of ten as a deleted group, not as noise.
    "test_intel_gt_ring": 331,
    # Measured 2026-08-29 by running tests/build/test_intel_gt_fence.exe, the FIRST
    # target ever to compile phase0-kext/IntelGtFence.hpp:
    #   test_intel_gt_fence 73 checks, 0 failures
    # Before this reading the header had no suite, no CMake target, no syntax-gate
    # line and no caller anywhere in the tree -- grep for igGtf found exactly two
    # files, the header and the suite. So 73 is a first measurement rather than a
    # growth; there is no prior number for the band to check against and none is
    # claimed.
    #
    # It lands LOW for a six-group suite next to test_intel_gt_ring's 331, and the
    # reason is that the fence suite is deliberately instalment one -- its own
    # header comment names what it does not yet cover: the seqno wrap arithmetic
    # (igGtfSeqnoPassed / igGtfSeqnoRetired / igGtfNextSeqno), the address
    # round-trip through igGtfComposedSeqnoAddress(), and the negative controls for
    # igGtfCompletionIsOrdered(). Those are named in the suite rather than left
    # silent, because a suite that quietly covers less than its subject reads as a
    # suite that covers all of it. Read the next growth of this entry as that
    # second instalment landing, not as noise.
    #
    # Most of the 73 is hand-written cases rather than loops, so a deleted GROUP
    # moves this number by roughly ten and a deleted loop bound moves it by less --
    # the opposite reading from the ring entry above. The one loop that matters is
    # the pairwise-distinctness walk over the seven verdicts, which is 21 of the 73
    # on its own.
    #
    # Re-measured 2026-08-30 by running tests/build/test_intel_gt_fence.exe:
    #   test_intel_gt_fence 138 checks, 0 failures
    # 73 -> 138 is the second instalment the comment above predicted, and it is
    # ABOVE the band (73 + max(8, 7) = 81) by fifty-seven, so the ceiling fired
    # correctly and this entry is a fresh reading rather than an arithmetic edit.
    #
    # The +65 is one group: property 9, the address round-trip. It did not compile
    # before this reading -- three call sites passed the two half-words to
    # igGtfComposedSeqnoAddress(), which takes (const IGU32 *, IGU32), so the suite
    # was a build failure rather than a suite and the 73 above was measured on a
    # tree where that group did not exist. The group now composes a real span per
    # sample address through igGtfWriteCompletion() and joins THAT, which is the
    # only form that is a round-trip: a join handed igGtfAddressLow/High()'s own
    # output agrees with them even when the composer stores the halves in the
    # wrong two slots. Six sample addresses times four checks is most of the +65;
    # the swap control now transposes two slots of a COPY of the composed span
    # rather than transposing two arguments, and the join's own two refusals
    # (null span, span one dword short) are asserted for the first time.
    "test_intel_gt_fence": 138,
    # Measured 2026-08-30 by running tests/build/test_intel_gt_reset.exe, the FIRST
    # target ever to compile phase0-kext/IntelGtReset.hpp:
    #   test_intel_gt_reset 139 checks, 0 failures
    # Before this reading the header and its suite were both on disk and neither
    # was registered anywhere -- no CMake target, no syntax-gate line, no entry in
    # tests/test_check_count_last_line.py and none here. So 139 is a first
    # measurement rather than a growth; there is no prior number for the band to
    # check against and none is claimed.
    #
    # This is the tenth GT header and the first that decides the driver should STOP
    # waiting. The count is dominated by two loops rather than by hand-written
    # cases: the pairwise-distinctness walk over the wait verdicts, and the engine
    # walk that drives igGtrResetVerdict() over every ring base to pin that the
    # judgement is about the register WORD rather than about which engine produced
    # it. Read a drop of twenty as a deleted loop bound; a deleted group of
    # hand-written ordering assertions moves it by less.
    "test_intel_gt_reset": 139,
    # Measured 2026-10-12 by running tests/build/test_intel_gt_forcewake.exe,
    # first reading of a new suite:
    #   test_intel_gt_forcewake 188 checks, 0 failures
    # Dominated by the three
    # pairwise-distinct verdict-name walks and the per-domain accessor walk; a
    # drop of ~30 is a deleted loop bound.
    "test_intel_gt_forcewake": 188,
    "test_intel_gt_batch": 289,
    "test_intel_gt_blt": 213,
    "test_intel_gt_image": 111,
    "test_intel_gt_context": 124,
    "test_intel_gt_exec": 123,
    "test_intel_gt_cmd": 120,
    "test_intel_fb_present": 140,
    "test_decode": 186,
    "test_falcon": 239,
    "test_riscv_ucode": 214,
    "test_gsp_boot": 368,
    "test_gsp_stage": 181,
    "test_intel_display": 480,
    "test_intel_gpu_id": 77,
    "test_intel_framebuffer": 110,
    "test_intel_display_names": 284,
    "test_intel_pixel": 121,
    # Gen12 scanout descriptor (IntelScanout.hpp). Had no entry since
    # intel-handoff34.md section 6 item 4; measured 61 checks, 0 failures.
    "test_intel_scanout": 61,
    # Driver-owned scanout sink (IntelSinkSurface.hpp), intel-handoff40 §7 (a).
    # Measured 2026-08-29, first reading of a new suite: 19 checks, 0 failures.
    "test_intel_sink_surface": 20,
    # Sink descriptor publish (IntelSinkPublish.hpp), next step after
    # intel-handoff40 §7 (a). Measured 2026-08-29: 23 checks, 0 failures.
    "test_intel_sink_publish": 23,
    # Plane flip encoder (IntelPlaneProgram.hpp), sink plan step (b).
    "test_intel_plane_program": 91,
    # Sink GGTT placement (IntelSinkAlloc.hpp), alloc half of step (a).
    "test_intel_sink_alloc": 21,
    # Step (b) composed over a firmware plane read (IntelSinkRehearse.hpp).
    "test_intel_sink_rehearse": 60,
    # Measured 2026-11-01: first readings of 35 suites that had no entry here,
    # no tests/CMakeLists.txt registration and no count-gate listing. Each was
    # built standalone with g++ 16.1.0 (WinLibs UCRT) -std=c++17 -Wall -Wextra
    # -Wpedantic, zero warnings, and run once; every one exited 0 and its final
    # line was the count below. These are NOT yet MSVC readings: tests/build held
    # no binary for any of them when this was written, so tests\build-msvc.bat is
    # what turns them into the gate's own numbers.
    #   test_intel_gt_copy_plan 75 checks, 0 failures
    #   test_intel_gt_csb 71 checks, 0 failures
    #   test_intel_gt_dispatch 30 checks, 0 failures
    #   test_intel_gt_lrc 74 checks, 0 failures
    #   test_intel_gt_ppgtt 82 checks, 0 failures
    #   test_intel_sink_candidate 48 checks, 0 failures
    #   test_intel_sink_ggtt 62 checks, 0 failures
    #   test_intel_sink_peer 34 checks, 0 failures
    #   test_nv_ampere_channel 71 checks, 0 failures
    #   test_nv_ampere_copy 66 checks, 0 failures
    #   test_nv_ampere_fence 61 checks, 0 failures
    #   test_nv_ampere_rm_plan 67 checks, 0 failures
    #   test_nv_ampere_submit 85 checks, 0 failures
    #   test_nv_gsp_bootargs_rehearsal 28 checks, 0 failures
    #   test_nv_gsp_booter_fuse 62 checks, 0 failures
    #   test_nv_gsp_cached_args 77 checks, 0 failures
    #   test_nv_gsp_fw_container 81 checks, 0 failures
    #   test_nv_gsp_libos_init 79 checks, 0 failures
    #   test_nv_gsp_queue_dma 207 checks, 0 failures
    #   test_nv_gsp_queue_init 84 checks, 0 failures
    #   test_nv_gsp_queue_memory 217 checks, 0 failures
    #   test_nv_gsp_queue_observe 102 checks, 0 failures
    #   test_nv_gsp_queue_plan 59 checks, 0 failures
    #   test_nv_gsp_queue_regs 69 checks, 0 failures
    #   test_nv_gsp_queue_rx 75 checks, 0 failures
    #   test_nv_gsp_queue_tx 73 checks, 0 failures
    #   test_nv_gsp_radix3 84 checks, 0 failures
    #   test_nv_gsp_rm_image 45 checks, 0 failures
    #   test_nv_gsp_rpc_frame 59 checks, 0 failures
    #   test_nv_gsp_rpc_response 108 checks, 0 failures
    #   test_nv_gsp_rpc_transaction 73 checks, 0 failures
    #   test_nv_gsp_wpr_boot_ucode 41 checks, 0 failures
    #   test_nv_gsp_wpr_ga107 61 checks, 0 failures
    #   test_nv_gsp_wpr_image 97 checks, 0 failures
    #   test_nv_gsp_wpr_meta 78 checks, 0 failures
    "test_intel_gt_copy_plan": 75,
    "test_intel_gt_csb": 71,
    "test_intel_gt_dispatch": 30,
    "test_intel_gt_lrc": 74,
    "test_intel_gt_ppgtt": 82,
    "test_intel_sink_candidate": 48,
    "test_intel_sink_ggtt": 62,
    "test_intel_sink_peer": 34,
    "test_nv_ampere_channel": 71,
    "test_nv_ampere_copy": 66,
    "test_nv_ampere_fence": 61,
    "test_nv_ampere_rm_plan": 67,
    "test_nv_ampere_submit": 85,
    "test_nv_gsp_bootargs_rehearsal": 28,
    "test_nv_gsp_booter_fuse": 62,
    "test_nv_gsp_cached_args": 77,
    "test_nv_gsp_fw_container": 81,
    "test_nv_gsp_libos_init": 79,
    "test_nv_gsp_queue_dma": 207,
    "test_nv_gsp_queue_init": 84,
    "test_nv_gsp_queue_memory": 217,
    "test_nv_gsp_queue_observe": 102,
    "test_nv_gsp_queue_plan": 59,
    "test_nv_gsp_queue_regs": 69,
    "test_nv_gsp_queue_rx": 75,
    "test_nv_gsp_queue_tx": 73,
    "test_nv_gsp_radix3": 84,
    "test_nv_gsp_rm_image": 45,
    "test_nv_gsp_rpc_frame": 59,
    "test_nv_gsp_rpc_response": 108,
    "test_nv_gsp_rpc_transaction": 73,
    "test_nv_gsp_wpr_boot_ucode": 41,
    "test_nv_gsp_wpr_ga107": 61,
    "test_nv_gsp_wpr_image": 97,
    "test_nv_gsp_wpr_meta": 78,
    # Measured 2026-11-01, later the same day: first readings of the three WPR
    # suites registered once their final printf was cut to the exact
    # "\n%d checks, %d failures\n" format (it had carried a "GA107 WPR
    # candidate:", "GA107 WPR facts:" or "WPR sysmem bridge:" label). Same g++
    # 16.1.0 build as above, zero warnings, each exited 0. NOT yet MSVC
    # readings; tests\build-msvc.bat turns them into the gate's own numbers.
    #   test_nv_gsp_wpr_candidate 45 checks, 0 failures
    #   test_nv_gsp_wpr_ga107_facts 38 checks, 0 failures
    #   test_nv_gsp_wpr_sysmem 35 checks, 0 failures
    "test_nv_gsp_wpr_candidate": 45,
    "test_nv_gsp_wpr_ga107_facts": 38,
    "test_nv_gsp_wpr_sysmem": 35,
    # Measured 2026-11-01, later again: first readings of the three Ampere
    # runtime and publication suites, registered once their final printf moved
    # from '%u checks, %u failures\n' on unsigned counters to the exact
    # "\n%d checks, %d failures\n" format with static_cast<int> arguments and
    # their tail from 'gFailures == 0u ? 0 : 1' to 'gFailures == 0 ? 0 : 1'.
    # Same g++ 16.1.0 build as above, zero warnings, each exited 0. NOT yet
    # MSVC readings; tests\build-msvc.bat turns them into the gate's own
    # numbers.
    #   test_nv_ampere_channel_runtime 164 checks, 0 failures
    #   test_nv_ampere_copy_publication_bridge 30 checks, 0 failures
    #   test_nv_ampere_copy_publication_runtime 182 checks, 0 failures
    "test_nv_ampere_channel_runtime": 164,
    "test_nv_ampere_copy_publication_bridge": 30,
    "test_nv_ampere_copy_publication_runtime": 182,
}

# A suite must call check helpers many times over. Uniform across suites on
# purpose: this is the bare-checkout assertion, and a per-suite number here would
# be a second table nothing keeps true.
STATIC_CALL_FLOOR = 20

# Both count formats in use. Group 1 is the number of checks actually run.
COUNT_RE = re.compile(r"^\s*(\d+)\s*(?:/\s*\d+\s+passed|checks\b)")

# checkU32(...), checkBool(...), checkJournal(...), checkResult(...), checkU64(...)
CALL_RE = re.compile(r"\bcheck[A-Z][A-Za-z0-9_]*\s*\(")
DEF_RE = re.compile(r"\bstatic\s+\w+\s+check[A-Z][A-Za-z0-9_]*\s*\(")

# One recorded reading in this module's OWN provenance comment: a comment line
# naming a suite stem followed by either count format. Only lines that begin
# with a '#' are considered, which is the whole point -- MEASURED's dict entries
# are not comments, so a number that exists ONLY in the table cannot satisfy it.
READING_RE = re.compile(
    r"^\s*#\s*(test_[A-Za-z0-9_]+)\s+(?:(\d+)\s*/\s*\d+\s+passed|(\d+)\s+checks)")


def floor_for(name):
    """The floor for one suite, derived from its measured count."""
    return max(1, MEASURED[name] // 2)


def ceiling_for(name):
    """The ceiling for one suite, derived from the same measured count.

    A floor is a one-sided property: loosening it (growing the count) preserves
    it, so for five revisions nothing in this file could fail when a MEASURED
    entry fell behind the binary. This is the other edge. The band is
    ``measured + max(8, measured // 8)`` -- wide enough that adding a handful of
    checks does not redden the gate on the same pass that adds them, tight
    enough that a suite which has grown by a section trips it.

    Deliberately NOT symmetric with the floor. The floor is generous because
    what it catches (a suite collapsing toward zero) is a real defect that a
    noisy edge would train people to edit away. The ceiling is tight because
    what it catches is a stale number in this file, and the fix is one line
    here.
    """
    measured = MEASURED[name]
    return measured + max(8, measured // 8)


def parse_count(last_line):
    """The number of checks a suite reports, or None if the line is not a count.

    Must return 0 -- not None -- for ``0/0 passed, 0 failed``. Returning None
    there would turn the defect this module exists for into an unparseable line
    the dynamic test steps over.
    """
    match = COUNT_RE.match(last_line)
    if match is None:
        return None
    return int(match.group(1))


def suite_stems_on_disk():
    """Suite names taken from the filesystem, never from MEASURED."""
    stems = set()
    for path in glob.glob(os.path.join(HERE, "test_*.cpp")):
        stems.add(os.path.splitext(os.path.basename(path))[0])
    return stems


def read_source(stem):
    path = os.path.join(HERE, stem + ".cpp")
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def call_sites(text):
    """Calls to check helpers, with the helper definitions themselves removed."""
    return len(CALL_RE.findall(text)) - len(DEF_RE.findall(text))


def recorded_readings():
    """Suite stem -> the set of counts this file's comment records as measured.

    Reads this module's own source. The comment block above MEASURED is the only
    provenance any number in this file has, and nothing has ever checked that a
    table entry corresponds to a line in it.
    """
    readings = {}
    path = os.path.abspath(__file__)
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = READING_RE.match(line)
            if match is None:
                continue
            count = int(match.group(2) or match.group(3))
            readings.setdefault(match.group(1), set()).add(count)
    return readings


class TableMatchesTheTree(unittest.TestCase):
    """The recorded set is derived from disk, so a new suite cannot be skipped."""

    def test_every_suite_on_disk_has_a_recorded_count(self):
        missing = sorted(suite_stems_on_disk() - set(MEASURED))
        self.assertEqual(
            missing, [],
            "tests/%s.cpp exists but has no entry in MEASURED, so nothing "
            "bounds its check count" % ", ".join(missing))

    def test_no_recorded_count_names_a_suite_that_is_gone(self):
        extra = sorted(set(MEASURED) - suite_stems_on_disk())
        self.assertEqual(
            extra, [],
            "MEASURED names %s, which has no tests/*.cpp -- a stale entry "
            "reads as coverage of a suite nobody runs" % ", ".join(extra))

    def test_every_recorded_count_and_derived_floor_is_positive(self):
        """A zero floor is a floor that cannot fail, which is the whole defect."""
        for name, count in sorted(MEASURED.items()):
            self.assertGreater(count, 0, "%s recorded as %d" % (name, count))
            self.assertGreater(floor_for(name), 0, name)
            self.assertLessEqual(floor_for(name), count, name)

    def test_every_derived_ceiling_brackets_its_measured_count(self):
        """The upper edge must sit ABOVE the reading and BELOW twice it.

        Above, or the gate reddens on the very number it recorded. Below twice,
        or the band is so wide that a suite could double and still fit -- which
        is the one-sided floor all over again, wearing a ceiling's name.
        """
        for name, count in sorted(MEASURED.items()):
            self.assertGreater(ceiling_for(name), count, name)
            self.assertLess(ceiling_for(name), count * 2, name)
            self.assertGreater(ceiling_for(name), floor_for(name), name)


class EveryTableEntryHasProvenance(unittest.TestCase):
    """A number in MEASURED must appear as a dated reading in the comment above it.

    Every other test in this module bounds a BINARY against the table. Nothing
    has ever bounded the TABLE against its own stated provenance. That gap is
    not theoretical: the docstring of this file opens by describing a count
    ``test_falcon`` was recorded at 165 while the binary printed 169, and the
    fix for each of the seven stalenesses since has been to edit the dict --
    with the comment updated by hand, or not at all. A dict entry with no
    corresponding reading is a number whose only evidence is that somebody typed
    it, which is precisely what the phrase 'measured' in this file is supposed
    to rule out.

    The ceiling cannot catch this. It compares the binary to the table, so a
    table edited to match a binary is green by construction no matter whether
    anyone ever ran anything.
    """

    def test_every_measured_count_appears_as_a_recorded_reading(self):
        readings = recorded_readings()
        unbacked = []
        for name, count in sorted(MEASURED.items()):
            if count not in readings.get(name, set()):
                unbacked.append(
                    "%s=%d (comment records %s)"
                    % (name, count,
                       sorted(readings.get(name, set())) or "nothing"))
        self.assertEqual(
            unbacked, [],
            "MEASURED holds %s -- a number in the table with no matching line "
            "in this file's provenance comment. Run the binary, then record the "
            "reading with its date above MEASURED. A table entry edited to "
            "match a binary is green by construction; the comment is the only "
            "thing that says a human ran it." % "; ".join(unbacked))

    def test_the_reading_parser_reads_both_count_formats(self):
        """Negative control: a parser matching nothing would pass vacuously.

        If READING_RE failed to match the comment's real idiom, every suite
        would come back with an empty reading set and the test above would fail
        loudly rather than silently -- but the reverse mistake (matching the
        MEASURED dict lines themselves, which also pair a stem with a number)
        would make it pass no matter what. Both are pinned here.
        """
        line_checks = re.compile(READING_RE)
        hit = line_checks.match("#   test_gsp_boot      217 checks, 0 failures")
        self.assertIsNotNone(hit)
        self.assertEqual(hit.group(1), "test_gsp_boot")
        self.assertEqual(int(hit.group(3)), 217)

        hit = line_checks.match("#   test_falcon      169/169 passed, 0 failed")
        self.assertIsNotNone(hit)
        self.assertEqual(int(hit.group(2)), 169)

        # The dict line is NOT a reading. If it matched, every entry would be
        # its own evidence and this whole class would assert nothing.
        self.assertIsNone(line_checks.match('    "test_decode": 64,'))
        self.assertIsNone(line_checks.match('    "test_intel_display": 354,'))

    def test_at_least_one_suite_is_actually_backed(self):
        """Proves the reader finds real lines in the real file, not zero."""
        readings = recorded_readings()
        self.assertIn("test_gsp_boot", readings)
        self.assertIn(217, readings["test_gsp_boot"])


class SourcesStillCallChecks(unittest.TestCase):
    """The bare-checkout half: no compiler needed, so this always runs."""

    def test_each_suite_calls_check_helpers_many_times(self):
        for stem in sorted(suite_stems_on_disk()):
            calls = call_sites(read_source(stem))
            self.assertGreaterEqual(
                calls, STATIC_CALL_FLOOR,
                "%s.cpp contains %d check-helper calls, below the floor of %d -- "
                "a suite this thin cannot be asserting what its name claims"
                % (stem, calls, STATIC_CALL_FLOOR))


class BinariesReportRealCounts(unittest.TestCase):
    """The half with teeth, when a build exists to run."""

    def binary_for(self, stem):
        return os.path.join(BUILD_DIR, stem + ".exe")

    def test_no_listed_suite_is_missing_from_an_existing_build(self):
        """A built tree missing a binary is a failure, not an absence of news."""
        if not os.path.isdir(BUILD_DIR):
            self.skipTest("tests/build does not exist; nothing was built here")
        missing = [s for s in sorted(MEASURED) if not os.path.isfile(self.binary_for(s))]
        self.assertEqual(
            missing, [],
            "tests/build exists but %s has no binary -- CMake stopped "
            "registering it, or the link failed" % ", ".join(missing))

    def test_no_binary_is_older_than_its_source(self):
        """A stale binary's count describes code that is no longer there."""
        stale = []
        for stem in sorted(MEASURED):
            exe = self.binary_for(stem)
            src = os.path.join(HERE, stem + ".cpp")
            if os.path.isfile(exe) and os.path.isfile(src) \
                    and os.path.getmtime(exe) < os.path.getmtime(src):
                stale.append(stem)
        self.assertEqual(
            stale, [],
            "%s: binary predates its source, so its count is evidence about "
            "the wrong code -- run tests/build-msvc.bat" % ", ".join(stale))

    def test_each_built_suite_reports_at_least_its_floor(self):
        checked = []
        for stem in sorted(MEASURED):
            exe = self.binary_for(stem)
            if not os.path.isfile(exe):
                continue
            proc = subprocess.run([exe], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT,
                                  universal_newlines=True, cwd=BUILD_DIR)
            lines = [ln for ln in proc.stdout.splitlines() if ln.strip()]
            self.assertTrue(lines, "%s printed nothing at all" % stem)
            count = parse_count(lines[-1])
            self.assertIsNotNone(
                count,
                "%s final line is not a count: %r" % (stem, lines[-1]))
            self.assertGreaterEqual(
                count, floor_for(stem),
                "%s ran %d checks, below its floor of %d (measured %d) -- "
                "a collapsed count exits 0 and reads as success"
                % (stem, count, floor_for(stem), MEASURED[stem]))
            self.assertLessEqual(
                count, ceiling_for(stem),
                "%s ran %d checks, above its ceiling of %d -- MEASURED[%r] "
                "still says %d and has fallen behind the binary. Update that "
                "one entry to %d. This is the edge that did not exist for five "
                "revisions, which is why every reading in this file went stale "
                "silently."
                % (stem, count, ceiling_for(stem), stem, MEASURED[stem], count))
            checked.append(stem)

        if os.path.isdir(BUILD_DIR):
            self.assertEqual(
                checked, sorted(MEASURED),
                "only %s were actually run; the rest passed by absence"
                % ", ".join(checked))


class ParserNegativeControl(unittest.TestCase):
    """The parser must READ the defect, not fail to recognise it.

    If ``parse_count`` returned None for a zeroed count line, the dynamic test
    would report "not a count" -- or worse, an earlier version would skip it --
    and the one output this module exists to reject would slip through.
    """

    def test_a_zeroed_passed_line_parses_as_zero(self):
        self.assertEqual(parse_count("0/0 passed, 0 failed"), 0)

    def test_a_zeroed_checks_line_parses_as_zero(self):
        self.assertEqual(parse_count("0 checks, 0 failures"), 0)

    def test_both_real_formats_parse(self):
        self.assertEqual(parse_count("169/169 passed, 0 failed"), 169)
        self.assertEqual(parse_count("217 checks, 0 failures"), 217)

    def test_prose_is_not_a_count(self):
        self.assertIsNone(parse_count("unproven until Step 7."))
        self.assertIsNone(parse_count("all bit-fields verified"))

    def test_a_zeroed_count_would_fail_the_floor(self):
        """Ties the parser to the assertion, so neither can drift alone."""
        for name in MEASURED:
            self.assertGreater(floor_for(name), parse_count("0 checks, 0 failures"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
