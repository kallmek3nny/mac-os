@echo off
REM
REM check-kext-syntax.bat -- third verification gate.
REM
REM The kext has never been compiled by Apple's toolchain, and it cannot be on
REM this machine. What CAN be done is parse it: clang 19.1.5 ships inside VS
REM 2022, and tests\kextshim\ provides just enough of IOKit and libkern for the
REM front end to get through NV3050Probe.cpp.
REM
REM WHAT THIS PROVES
REM   - No typos in IOKit member names or argument counts
REM   - Every APPLE_KEXT_OVERRIDE really overrides something
REM   - Every IOLog format specifier matches its argument width
REM   - Every OSDeclareDefaultStructors has its OSDefineMetaClassAndStructors
REM   - Both dependency-free headers parse standalone
REM
REM WHAT THIS DOES NOT PROVE
REM   The shim is NOT Apple's SDK. Real KPI signatures may differ. Nothing here
REM   links, and nothing here says the kext loads. STEP 7 in ..\STEPS.md is
REM   still required. A green run means "structurally plausible", not "builds".
REM
REM The negative-control pass is the load-bearing part: a syntax gate that
REM passes everything would convert "never compiled" into "believed compiled",
REM which is worse than no gate at all. Each planted defect must FAIL.
REM

setlocal enabledelayedexpansion
cd /d "%~dp0.."

set "CLANG=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\bin\clang++.exe"

if not exist "!CLANG!" (
    echo ERROR: clang not found at:
    echo   !CLANG!
    echo.
    echo Install the "C++ Clang tools for Windows" component via the Visual
    echo Studio Installer, or edit CLANG above to point at any clang 13+.
    exit /b 1
)

set "FLAGS=-fsyntax-only -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter -Werror -I tests/kextshim -I phase0-kext"
REM -Wno-unused-function: a header parsed alone has no callers for its own
REM static inlines. That is an artifact of the standalone parse, not a defect.
set "HDRFLAGS=-fsyntax-only -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-function -Werror -I phase0-kext"

set FAILED=0

echo === clang ===
"!CLANG!" --version | findstr /i version
echo.

echo === 1. headers standalone ===
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVDecode.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVDecode.hpp) else (echo   ok   NVDecode.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVFalcon.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVFalcon.hpp) else (echo   ok   NVFalcon.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVRiscvUcode.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVRiscvUcode.hpp) else (echo   ok   NVRiscvUcode.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBoot.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBoot.hpp) else (echo   ok   NVGspBoot.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspStage.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspStage.hpp) else (echo   ok   NVGspStage.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelDisplay.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelDisplay.hpp) else (echo   ok   IntelDisplay.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGpuId.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGpuId.hpp) else (echo   ok   IntelGpuId.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelFramebuffer.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelFramebuffer.hpp) else (echo   ok   IntelFramebuffer.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelScanout.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelScanout.hpp) else (echo   ok   IntelScanout.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelSinkSurface.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelSinkSurface.hpp) else (echo   ok   IntelSinkSurface.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelSinkPublish.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelSinkPublish.hpp) else (echo   ok   IntelSinkPublish.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelSinkAlloc.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelSinkAlloc.hpp) else (echo   ok   IntelSinkAlloc.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelPlaneProgram.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelPlaneProgram.hpp) else (echo   ok   IntelPlaneProgram.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelSinkRehearse.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelSinkRehearse.hpp) else (echo   ok   IntelSinkRehearse.hpp)
REM IntelPixel.hpp is the SECOND header in this family with an #include, after
REM IntelDisplayNames.hpp below, and for the same reason: it switches on the
REM IG_FB_FORMAT_* codes IntelFramebuffer.hpp owns and takes them from their owner
REM rather than respelling them. A local copy of XRGB_8888's value would give this
REM tree two answers to one question. The include is a QUOTED sibling inside
REM phase0-kext, so -I phase0-kext parses it standalone and
REM check-header-coverage.py -- which derives the exemption from the presence of an
REM ANGLE-BRACKET include -- expects it HERE rather than exempted.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelPixel.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelPixel.hpp) else (echo   ok   IntelPixel.hpp)
REM IntelDisplayNames.hpp carries QUOTED includes of its two siblings, because the
REM verdict constants it switches on are defined there and it has no forward
REM declarations available. Quoted siblings are still inside phase0-kext, so -I
REM phase0-kext parses it standalone and check-header-coverage.py -- which derives
REM the exemption from the presence of an ANGLE-BRACKET include -- expects it HERE
REM rather than exempted.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelDisplayNames.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelDisplayNames.hpp) else (echo   ok   IntelDisplayNames.hpp)
REM IntelFbPresent.hpp is the THIRD header in this family with #includes, and it
REM carries the most of any of them -- four QUOTED siblings, because it is a JOIN
REM rather than a decode layer and every number it produces is owned by one of
REM them. Respelling any of those constants locally would give this tree two
REM answers to one question, which is the whole reason the file exists as a header
REM instead of as code inside the IOFramebuffer subclass. Quoted siblings live in
REM phase0-kext, so -I phase0-kext parses it standalone and
REM check-header-coverage.py -- which derives the exemption from the presence of an
REM ANGLE-BRACKET include -- expects it HERE rather than exempted.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelFbPresent.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelFbPresent.hpp) else (echo   ok   IntelFbPresent.hpp)
REM IntelGtCmd.hpp is the Gen12 render-engine command submission layer and the
REM first header in this tree that is not about display at all. Unlike the three
REM above it has NO includes -- neither angle-bracket nor quoted -- so it is the
REM plain case for check-header-coverage.py: expected HERE, never exempted. It is
REM also not one of the two Intel headers tools/check-registers.py parses, so this
REM line is the only gate that reads it as C++ at all.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtCmd.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtCmd.hpp) else (echo   ok   IntelGtCmd.hpp)
REM IntelGtExec.hpp is the execlist/GTT layer directly above IntelGtCmd.hpp. It
REM has ONE include and that include is a QUOTED sibling, so it is the same case
REM as IntelFbPresent.hpp above: -I phase0-kext parses it standalone, and
REM check-header-coverage.py -- which derives the exemption from the presence of
REM an ANGLE-BRACKET include -- expects it HERE rather than exempted. Like its
REM sibling it is not one of the two Intel headers tools/check-registers.py
REM parses, so this line is the only gate that reads it as C++ at all.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtExec.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtExec.hpp) else (echo   ok   IntelGtExec.hpp)
REM IntelGtContext.hpp is the LOGICAL RING CONTEXT layer directly above
REM IntelGtExec.hpp, and the same case again: ONE include, and it is a QUOTED
REM sibling, so -I phase0-kext parses it standalone and check-header-coverage.py
REM -- which derives the exemption from the presence of an ANGLE-BRACKET include
REM -- expects it HERE rather than exempted. Like both siblings beneath it, it is
REM not one of the two Intel headers tools/check-registers.py parses, so this
REM line is the only gate that reads it as C++ at all.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtContext.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtContext.hpp) else (echo   ok   IntelGtContext.hpp)
REM IntelGtImage.hpp is the image WRITER directly above IntelGtContext.hpp, and
REM the same case a fourth time: ONE include, and it is a QUOTED sibling, so
REM -I phase0-kext parses it standalone and check-header-coverage.py -- which
REM derives the exemption from the presence of an ANGLE-BRACKET include --
REM expects it HERE rather than exempted. Like all three siblings beneath it it is
REM not one of the two Intel headers tools/check-registers.py parses, so this line
REM is the only gate that reads it as C++ at all. It is also the first GT header
REM that WRITES to caller memory, which this gate cannot see: a clean parse says
REM nothing about which slots it fills or whether a refusal wrote first.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtImage.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtImage.hpp) else (echo   ok   IntelGtImage.hpp)
REM IntelGtBlt.hpp is the 2D BLITTER packet composer and the same case a FIFTH
REM time: ONE include, and it is a QUOTED sibling, so -I phase0-kext parses it
REM standalone and check-header-coverage.py -- which derives the exemption from
REM the presence of an ANGLE-BRACKET include -- expects it HERE rather than
REM exempted. Like all four siblings beneath it it is not one of the two Intel
REM headers tools/check-registers.py parses, so this line is the only gate that
REM reads it as C++ at all.
REM
REM It differs from those four in one way worth stating. It is the first GT
REM header describing GPU WORK rather than the plumbing that would deliver it,
REM and unlike them it has a CITED half: reference/i915/intel_gpu_commands.h
REM carries the three opcodes, the BC client, and the ROP, depth and write-mask
REM flags. A clean parse says nothing about any of that. It does not check that
REM 0x50 is still XY_COLOR_BLT upstream, and it does not check that a refused
REM compose wrote no dwords before returning -- tests/test_intel_gt_blt.cpp is
REM what reads those, and it is the ONLY thing that does.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtBlt.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtBlt.hpp) else (echo   ok   IntelGtBlt.hpp)
REM IntelGtBatch.hpp is the batch-buffer JOIN and the first header in this tree
REM with TWO includes rather than one. Both are QUOTED siblings -- IntelGtCmd.hpp
REM for the MI opcodes and the address splitters, IntelGtBlt.hpp for the packets
REM it terminates -- so -I phase0-kext still parses it standalone and
REM check-header-coverage.py, which derives the exemption from the presence of an
REM ANGLE-BRACKET include, expects it HERE rather than exempted. Two quoted
REM includes are the same case as one for that rule; what they change is that a
REM clean parse here is also evidence the two siblings compose in one translation
REM unit, which no earlier line in this section established.
REM
REM What a clean parse does NOT establish is everything this header exists for.
REM It says nothing about whether igGtqWriteTerminator() actually emits
REM MI_BATCH_BUFFER_END -- an unterminated batch does not stop, the command
REM streamer keeps fetching past the last packet and executes whatever the
REM allocation happened to contain -- and nothing about whether a REFUSED compose
REM wrote its out-param before returning, which is what stops a caller advancing
REM a ring tail into dwords the engine reads as commands.
REM tests/test_intel_gt_batch.cpp is what reads those, and it is the ONLY thing
REM that does.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtBatch.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtBatch.hpp) else (echo   ok   IntelGtBatch.hpp)
REM IntelGtRing.hpp is the WRAPPING STORE above the batch join, and it carries the
REM same TWO quoted siblings -- IntelGtCmd.hpp for the ring arithmetic and the MI
REM opcodes, IntelGtBatch.hpp for the batch-start packet it stores -- so
REM -I phase0-kext parses it standalone and check-header-coverage.py, which derives
REM the exemption from the presence of an ANGLE-BRACKET include, expects it HERE
REM rather than exempted. Like every GT sibling it is not one of the two Intel
REM headers tools/check-registers.py parses, so this line is the only gate that
REM reads it as C++ at all.
REM What a clean parse here does NOT establish is the whole reason the header
REM exists. This is the layer that STORES into a circular span, and a store that
REM clamps instead of wrapping compiles identically, returns the identical verdict
REM and reports the identical new tail while leaving the tail of the payload
REM unwritten. Nor can a parse see whether a refusal arm wrote before it returned.
REM tests/test_intel_gt_ring.cpp is what reads those, and it is the ONLY thing that
REM does.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtRing.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtRing.hpp) else (echo   ok   IntelGtRing.hpp)
REM IntelGtFence.hpp is the COMPLETION layer -- the flush/store pair that would let
REM this driver observe that submitted work finished. It carries ONE quoted sibling
REM (IntelGtCmd.hpp, for IGU32/IGU64, the MI header composer and the qword-alignment
REM policy), so -I phase0-kext parses it standalone and check-header-coverage.py,
REM which derives the exemption from the presence of an ANGLE-BRACKET include,
REM expects it HERE rather than exempted. Like every GT sibling it is not one of the
REM two Intel headers tools/check-registers.py parses, so this line is the only gate
REM that reads it as C++ at all.
REM What a clean parse here does NOT establish is the ORDERING, which is the whole
REM reason the header exists. A flush composed AFTER the store compiles identically,
REM composes the identical SET of dwords, the identical count and the identical byte
REM length, and parses as a well-formed MI packet -- and retires nothing, because
REM the CPU observes the seqno while the render caches still hold unwritten pixels.
REM Nor can a parse see whether a refusal arm wrote before it returned.
REM tests/test_intel_gt_fence.cpp is what reads those, and it is the ONLY thing that
REM does.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtFence.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtFence.hpp) else (echo   ok   IntelGtFence.hpp)
REM IntelGtReset.hpp is the RECOVERY layer -- the one that decides this driver
REM should STOP waiting. IntelGtFence.hpp above it says how completion would be
REM OBSERVED; nothing beneath it says what happens when that observation never
REM arrives, and polling forever is a hang rather than a decision. It carries the
REM same ONE quoted sibling as its neighbour (IntelGtCmd.hpp, for IGU32/IGU64), so
REM -I phase0-kext parses it standalone and check-header-coverage.py, which derives
REM the exemption from the presence of an ANGLE-BRACKET include, expects it HERE
REM rather than exempted. Like every GT sibling it is not one of the two Intel
REM headers tools/check-registers.py parses, so this line is the only gate that
REM reads it as C++ at all.
REM What a clean parse here does NOT establish is the ORDER of the wait judgement,
REM which is the whole reason the header exists. A budget test written above the
REM seqno test compiles identically, returns a verdict from the same family, and
REM reports TIMED_OUT for work that in fact completed on the final poll -- and the
REM two verdicts call for opposite next actions, reset the engine or proceed.
REM Nor can a parse see that no MMIO store exists anywhere on this path.
REM tests/test_intel_gt_reset.cpp is what reads those, and it is the ONLY thing
REM that does.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtReset.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtReset.hpp) else (echo   ok   IntelGtReset.hpp)
REM IntelGtForcewake.hpp: one quoted sibling (IntelGtCmd.hpp). A parse cannot see
REM the all-ones-before-bit ordering of igGtwAckVerdict(); test_intel_gt_forcewake
REM is what reads that, and check-registers.py re-derives its constants and its
REM 43-row range table from reference/i915/.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtForcewake.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtForcewake.hpp) else (echo   ok   IntelGtForcewake.hpp)
REM IntelDisplayTransactionLifetime.hpp carries ONE quoted sibling,
REM IntelPlaneRetirementEvidence.hpp, which in turn quotes IntelPlaneTransitionPlan.hpp,
REM so -I phase0-kext parses it standalone and check-header-coverage.py, which derives
REM the exemption from the presence of an ANGLE-BRACKET include, expects it HERE
REM rather than exempted. What a clean parse here does NOT establish is the phase
REM ordering the header exists for: that direct GGTT restore stays blocked once
REM non-arming display state is programmed, that an armed flip keeps both surfaces
REM and the target mapping pinned until exact retirement evidence resolves it, and
REM that uncertainty quarantines rather than frees.
REM tests/test_intel_display_transaction_lifetime.cpp is what reads those.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelDisplayTransactionLifetime.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelDisplayTransactionLifetime.hpp) else (echo   ok   IntelDisplayTransactionLifetime.hpp)
REM t-1b120d (GATE GREEN 8): the 66 headers below are the dependency-free
REM ones tools/check-header-coverage.py listed as never parsed standalone here.
REM Each has no angle-bracket include, so -I phase0-kext alone must parse it.
REM A clean parse proves only that the header is valid C++ on its own; its
REM behaviour is checked by the matching suite under tests/, not by this line.
REM Intel GT family (10), dependency-free, added under t-1b120d (IntelGtRegSemantics.hpp: t-4f24b2).
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtBlitter.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtBlitter.hpp) else (echo   ok   IntelGtBlitter.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtCopyPlan.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtCopyPlan.hpp) else (echo   ok   IntelGtCopyPlan.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtCsb.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtCsb.hpp) else (echo   ok   IntelGtCsb.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtDispatch.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtDispatch.hpp) else (echo   ok   IntelGtDispatch.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtLrc.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtLrc.hpp) else (echo   ok   IntelGtLrc.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtPpgtt.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtPpgtt.hpp) else (echo   ok   IntelGtPpgtt.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtScanoutMap.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtScanoutMap.hpp) else (echo   ok   IntelGtScanoutMap.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtSurface.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtSurface.hpp) else (echo   ok   IntelGtSurface.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtSurfaceHandles.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtSurfaceHandles.hpp) else (echo   ok   IntelGtSurfaceHandles.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtRegSemantics.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtRegSemantics.hpp) else (echo   ok   IntelGtRegSemantics.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtSurfaceTxn.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtSurfaceTxn.hpp) else (echo   ok   IntelGtSurfaceTxn.hpp)
REM Scanout fill rect bounds check, dependency-free, added under t-619d61.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelGtScanoutRect.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtScanoutRect.hpp) else (echo   ok   IntelGtScanoutRect.hpp)
REM Intel plane family (4), dependency-free, added under t-1b120d.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelPlaneLiveObserve.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelPlaneLiveObserve.hpp) else (echo   ok   IntelPlaneLiveObserve.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelPlaneProgramPlan.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelPlaneProgramPlan.hpp) else (echo   ok   IntelPlaneProgramPlan.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelPlaneRetirementEvidence.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelPlaneRetirementEvidence.hpp) else (echo   ok   IntelPlaneRetirementEvidence.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelPlaneTransitionPlan.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelPlaneTransitionPlan.hpp) else (echo   ok   IntelPlaneTransitionPlan.hpp)
REM Intel sink family (4), dependency-free, added under t-1b120d.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelSinkCandidate.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelSinkCandidate.hpp) else (echo   ok   IntelSinkCandidate.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelSinkGgtt.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelSinkGgtt.hpp) else (echo   ok   IntelSinkGgtt.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelSinkGgttTxn.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelSinkGgttTxn.hpp) else (echo   ok   IntelSinkGgttTxn.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/IntelSinkPeer.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelSinkPeer.hpp) else (echo   ok   IntelSinkPeer.hpp)
REM NVIDIA Ampere family (8), dependency-free, added under t-1b120d.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVAmpereChannel.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVAmpereChannel.hpp) else (echo   ok   NVAmpereChannel.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVAmpereChannelRuntime.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVAmpereChannelRuntime.hpp) else (echo   ok   NVAmpereChannelRuntime.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVAmpereCopy.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVAmpereCopy.hpp) else (echo   ok   NVAmpereCopy.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVAmpereCopyPublicationBridge.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVAmpereCopyPublicationBridge.hpp) else (echo   ok   NVAmpereCopyPublicationBridge.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVAmpereCopyPublicationRuntime.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVAmpereCopyPublicationRuntime.hpp) else (echo   ok   NVAmpereCopyPublicationRuntime.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVAmpereFence.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVAmpereFence.hpp) else (echo   ok   NVAmpereFence.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVAmpereRmPlan.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVAmpereRmPlan.hpp) else (echo   ok   NVAmpereRmPlan.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVAmpereSubmit.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVAmpereSubmit.hpp) else (echo   ok   NVAmpereSubmit.hpp)
REM NVIDIA GA107 PCI identity family (1), dependency-free, added under t-1b120d.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGa107PciIdentity.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGa107PciIdentity.hpp) else (echo   ok   NVGa107PciIdentity.hpp)
REM NVIDIA GSP booter family (9), dependency-free, added under t-1b120d.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBooterExecSequencePlan.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBooterExecSequencePlan.hpp) else (echo   ok   NVGspBooterExecSequencePlan.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBooterFuse.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBooterFuse.hpp) else (echo   ok   NVGspBooterFuse.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBooterFusePatchPolicy.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBooterFusePatchPolicy.hpp) else (echo   ok   NVGspBooterFusePatchPolicy.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBooterHs.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBooterHs.hpp) else (echo   ok   NVGspBooterHs.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBooterPatch.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBooterPatch.hpp) else (echo   ok   NVGspBooterPatch.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBooterRecoveryPlan.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBooterRecoveryPlan.hpp) else (echo   ok   NVGspBooterRecoveryPlan.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBooterRunPlan.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBooterRunPlan.hpp) else (echo   ok   NVGspBooterRunPlan.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBooterSuccessRetirementPlan.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBooterSuccessRetirementPlan.hpp) else (echo   ok   NVGspBooterSuccessRetirementPlan.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspBooterTransactionLifetime.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspBooterTransactionLifetime.hpp) else (echo   ok   NVGspBooterTransactionLifetime.hpp)
REM NVIDIA GSP family (26), dependency-free, added under t-1b120d.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspCachedArgs.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspCachedArgs.hpp) else (echo   ok   NVGspCachedArgs.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspFwContainer.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspFwContainer.hpp) else (echo   ok   NVGspFwContainer.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspLibosInit.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspLibosInit.hpp) else (echo   ok   NVGspLibosInit.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspQueueDma.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspQueueDma.hpp) else (echo   ok   NVGspQueueDma.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspQueueInit.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspQueueInit.hpp) else (echo   ok   NVGspQueueInit.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspQueueLiveEvidence.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspQueueLiveEvidence.hpp) else (echo   ok   NVGspQueueLiveEvidence.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspQueueMemory.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspQueueMemory.hpp) else (echo   ok   NVGspQueueMemory.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspQueueObserve.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspQueueObserve.hpp) else (echo   ok   NVGspQueueObserve.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspQueuePlan.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspQueuePlan.hpp) else (echo   ok   NVGspQueuePlan.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspQueueRegs.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspQueueRegs.hpp) else (echo   ok   NVGspQueueRegs.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspQueueRx.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspQueueRx.hpp) else (echo   ok   NVGspQueueRx.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspQueueTx.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspQueueTx.hpp) else (echo   ok   NVGspQueueTx.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspRadix3.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspRadix3.hpp) else (echo   ok   NVGspRadix3.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspRmImage.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspRmImage.hpp) else (echo   ok   NVGspRmImage.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspRpcFrame.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspRpcFrame.hpp) else (echo   ok   NVGspRpcFrame.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspRpcReceiveTracker.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspRpcReceiveTracker.hpp) else (echo   ok   NVGspRpcReceiveTracker.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspRpcResponse.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspRpcResponse.hpp) else (echo   ok   NVGspRpcResponse.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspRpcTransaction.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspRpcTransaction.hpp) else (echo   ok   NVGspRpcTransaction.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspWprBootDma.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspWprBootDma.hpp) else (echo   ok   NVGspWprBootDma.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspWprBootUcode.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspWprBootUcode.hpp) else (echo   ok   NVGspWprBootUcode.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspWprCandidate.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspWprCandidate.hpp) else (echo   ok   NVGspWprCandidate.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspWprGa107.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspWprGa107.hpp) else (echo   ok   NVGspWprGa107.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspWprGa107Facts.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspWprGa107Facts.hpp) else (echo   ok   NVGspWprGa107Facts.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspWprImage.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspWprImage.hpp) else (echo   ok   NVGspWprImage.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspWprMeta.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspWprMeta.hpp) else (echo   ok   NVGspWprMeta.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVGspWprSysmem.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVGspWprSysmem.hpp) else (echo   ok   NVGspWprSysmem.hpp)
REM NVIDIA/Intel shared family (4), dependency-free, added under t-1b120d.
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NvIntelSharedCopyLifetime.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NvIntelSharedCopyLifetime.hpp) else (echo   ok   NvIntelSharedCopyLifetime.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NvIntelSharedSurfaceHandoff.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NvIntelSharedSurfaceHandoff.hpp) else (echo   ok   NvIntelSharedSurfaceHandoff.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NvIntelSharedSurfaceHandoffAnchoredProvenance.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NvIntelSharedSurfaceHandoffAnchoredProvenance.hpp) else (echo   ok   NvIntelSharedSurfaceHandoffAnchoredProvenance.hpp)
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NvIntelSharedSurfaceHandoffProvenance.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NvIntelSharedSurfaceHandoffProvenance.hpp) else (echo   ok   NvIntelSharedSurfaceHandoffProvenance.hpp)
echo.

echo === 2. section 1 covers every header ===
REM A hand-written list of clang lines fails silently: add a header, forget the
REM line, and this gate prints ok for the ones it knows about and says nothing
REM about the one it does not. It has already parsed 2 of 5 while reporting
REM success. check-header-coverage.py derives the expected set from the
REM filesystem instead, and derives the one exemption from the header's own
REM includes rather than from a second hand-kept list.
python tools/check-header-coverage.py
if errorlevel 1 (set FAILED=1 & echo   FAIL check-header-coverage.py) else (echo   ok   check-header-coverage.py)
echo.

echo === 3. kext against the IOKit shim ===
REM Both IOKit shells are parsed here. NV3050Probe.cpp is the NVIDIA track's;
REM IntelDisplayProbe.cpp is the Gen12 display track's, and until it existed
REM the three Intel headers had no consumer outside tests/ at all -- so nothing
REM ever checked that a real IOService subclass could actually CALL them.
"!CLANG!" %FLAGS% phase0-kext/NV3050Probe.cpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NV3050Probe.cpp) else (echo   ok   NV3050Probe.cpp)
"!CLANG!" %FLAGS% phase0-kext/IntelDisplayProbe.cpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelDisplayProbe.cpp) else (echo   ok   IntelDisplayProbe.cpp)
REM IntelFbPresentProbe.cpp is the THIRD IOKit shell and the first IOFramebuffer
REM subclass in the tree. It is the only one whose base class is ABSTRACT in
REM Apple's SDK, so tests/kextshim/IOKit/graphics/IOFramebuffer.h declares the
REM eight pure virtuals `= 0` on purpose -- a concrete declaration there would
REM accept a subclass missing an override Apple demands, and that failure would
REM first appear on the operator's build rather than here.
"!CLANG!" %FLAGS% phase0-kext/IntelFbPresentProbe.cpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelFbPresentProbe.cpp) else (echo   ok   IntelFbPresentProbe.cpp)
REM IntelGtProbe.cpp is the FOURTH IOKit shell and the first consumer of the GT
REM stack from a translation unit that reaches hardware. Five GT headers and
REM sixteen suites existed before it and nothing called any of them from outside
REM tests/ -- the same gap IntelDisplayNames.hpp cost when it was left standing.
REM This line is also what makes IntelGtProbe.hpp's section 1 ABSENCE legitimate:
REM that header carries angle-bracket IOKit includes, so check-header-coverage.py
REM EXEMPTS it from standalone parsing and instead requires it to be reachable
REM from a .cpp compiled here. Delete this line and the exemption becomes an
REM omission, which that checker reddens rather than ignores.
REM What a clean parse here does NOT say: that the composed image is right, that
REM the ring offsets name real registers on Gen12, or that anything was submitted
REM -- nothing is, and there is no write path in the file to be wrong about.
"!CLANG!" %FLAGS% phase0-kext/IntelGtProbe.cpp
if errorlevel 1 (set FAILED=1 & echo   FAIL IntelGtProbe.cpp) else (echo   ok   IntelGtProbe.cpp)
echo.

echo === 4. gate-invoked tools have tests ===
REM Same shape of hole as section 2, one level up. The gates invoke Python
REM checkers, and a checker is an evidence PRODUCER -- when it is wrong it does
REM not crash, it prints a confident wrong verdict and the gate goes green.
REM check-structors.py sat in this gate with zero tests until this check found
REM it. The required set is derived from tools/ invocations in the gate scripts
REM and CLAUDE.md, so wiring in a new checker immediately demands tests for it.
python tools/check-tool-coverage.py
if errorlevel 1 (set FAILED=1 & echo   FAIL check-tool-coverage.py) else (echo   ok   check-tool-coverage.py)
echo.

echo === 5. structors pairing ===
python tools/check-structors.py
if errorlevel 1 (set FAILED=1 & echo   FAIL check-structors.py) else (echo   ok   check-structors.py)
echo.

echo === 6. documented paths resolve ===
REM CLAUDE.md is the first thing read on resume, and until now nothing
REM checked a word of it. It names the five headers, the four gate commands,
REM every handoff file and a layout table of directories -- rename any one of
REM them and the document keeps its confident tone while sending the reader to
REM a path that is not there. That is the same silent shape as sections 2 and
REM 4, one level up: the path is load-bearing for a human and no gate read it.
REM check-documented-paths.py resolves every path-shaped token in the document
REM and derives each exemption -- build products, guest-side absolute paths,
REM the one path the prose claims is ABSENT -- from a producer file or from the
REM sentence itself rather than from a second hand-kept list.
python tools/check-documented-paths.py
if errorlevel 1 (set FAILED=1 & echo   FAIL check-documented-paths.py) else (echo   ok   check-documented-paths.py)
echo.

echo === 7. Info.plist personalities name the target silicon ===
REM A personality matching the WRONG PCI device is not a build error and
REM not a parse error. The kext compiles, installs, loads, and then never
REM probes, because IOKit never hands it a provider -- which on the
REM operator's screen is indistinguishable from a broken probe(), from SIP,
REM and from an unsigned-kext refusal. It is also a one-nibble mistake, and
REM nothing else in this tree reads Info.plist at all.
REM The accepted device IDs are derived from DEVICE-IDS.md (transcribed from
REM NVIDIA's own g_nv_name_released.h) rather than typed here, so there is no
REM second table to go stale. 0x2523 is an RTX 3050 Ti on GA106: right
REM vendor, same marketing family, wrong die -- that is the case a human
REM reading the plist is least likely to catch.
python tools/check-kext-personalities.py
if errorlevel 1 (set FAILED=1 & echo   FAIL check-kext-personalities.py) else (echo   ok   check-kext-personalities.py)
echo.
echo === 8. Info.plist bundle identity agrees with the Makefile and sources ===
REM Section 7 covers the MATCHING dictionary. This covers the rest of the
REM plist -- the keys that decide whether the bundle can be LOADED at all,
REM and every one of them is invisible to the other gates by construction.
REM CFBundleExecutable names a binary the Makefile builds under KEXT_NAME;
REM CFBundleIdentifier is spelled a second time as the Makefile BUNDLE_ID and
REM a third time inside each personality; IOClass is resolved by STRING
REM through the OSMetaClass registry, so a typo links clean and fails at
REM match time on hardware; a missing OSBundleLibraries entry surfaces as an
REM undefined symbol at load time on the target laptop -- the most expensive
REM place in this project for a defect to appear. Every expectation is
REM DERIVED from the Makefile variables and the OSDefineMetaClassAndStructors
REM calls in phase0-kext/, because a second hand-kept copy of a string is
REM precisely the defect being checked for.
python tools/check-kext-bundle.py
if errorlevel 1 (set FAILED=1 & echo   FAIL check-kext-bundle.py) else (echo   ok   check-kext-bundle.py)
echo.

echo === 9. every complete() is guarded by a flag that tracks its prepare() ===
REM IOMemoryDescriptor carries a wire count: prepare() increments it and
REM complete() decrements it. A complete() with no successful prepare() behind
REM it drives that counter below its own floor, inside kernel VM bookkeeping --
REM which is nowhere this driver prints, so the driver's own view stays
REM consistent and NVLOG says nothing. Not hypothetical: on 2026-08-13
REM releaseGspFirmware() called complete() unconditionally while being invoked
REM from the very failure path whose purpose is "prepare() ITSELF failed".
REM Every other gate is blind to it by construction. Sections 1-3 prove the
REM file PARSES, and a prepare/complete balance is not a parse property. The
REM MSVC suites compile only the dependency-free headers, and this code is in
REM the IOKit shell on the far side of that split. tests/kextshim/ declares
REM both methods and has no wire count to unbalance. So the SHAPE of the code
REM is the only thing any gate here can assert, and the flag's NAME is DERIVED
REM from the prepare() site rather than typed, so a rename follows by itself.
python tools/check-prepare-balance.py
if errorlevel 1 (set FAILED=1 & echo   FAIL check-prepare-balance.py) else (echo   ok   check-prepare-balance.py)
echo.

echo === 10. negative control -- baseline must PASS ===
"!CLANG!" %FLAGS% tests/kextshim/negative-control.cpp
if errorlevel 1 (set FAILED=1 & echo   FAIL baseline should compile clean) else (echo   ok   baseline)
echo.

echo === 11. negative control -- each defect must FAIL ===
for %%M in (NVNEG_BAD_OVERRIDE NVNEG_BAD_FORMAT NVNEG_TYPO_MEMBER NVNEG_UNRELATED_CAST NVNEG_DMA_MAPPING_INT NVNEG_DMA_BAD_SEGFUNC NVNEG_DMA_VOID_MAPPER NVNEG_BMD_TYPO_MEMBER NVNEG_BMD_ARITY NVNEG_BMD_VOID_PTR NVNEG_BMD_CONST_PREPARE) do (
    "!CLANG!" %FLAGS% -D%%M tests/kextshim/negative-control.cpp >nul 2>&1
    if errorlevel 1 (
        echo   ok   %%M diagnosed
    ) else (
        set FAILED=1
        echo   FAIL %%M compiled clean -- the shim is too permissive
    )
)
echo.

echo === 12. every NVNEG_* block is enumerated, and vice versa ===
REM Section 11's macro list is hand-typed inside the for-loop, and it fails
REM silently in BOTH directions: a block added here but not there never runs,
REM and a name left there after the block is deleted prints "ok ... diagnosed"
REM while clang compiles the bare baseline. NVNEG_MISSING_STRUCTORS was removed
REM from negative-control.cpp for exactly that reason -- had its name stayed in
REM the loop, the gate would still be reporting it green today.
REM
REM The verdict WORD is what is asserted, not the exit code: an exit code alone
REM cannot separate "compared and clean" from "found nothing to compare".
python tools/check-negative-controls.py > "%TEMP%\nvneg-verdict.txt" 2>&1
type "%TEMP%\nvneg-verdict.txt"
findstr /C:"VERDICT: PASS" "%TEMP%\nvneg-verdict.txt" >nul
if errorlevel 1 (set FAILED=1 & echo   FAIL check-negative-controls.py) else (echo   ok   check-negative-controls.py)
del "%TEMP%\nvneg-verdict.txt" >nul 2>&1
echo.

if %FAILED%==0 (
    echo === ALL SYNTAX GATES PASSED ===
    echo Reminder: this is a parse, not a build. STEP 7 still required.
    exit /b 0
) else (
    echo === SYNTAX GATE FAILED ===
    exit /b 1
)
