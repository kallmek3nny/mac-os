# intel-handoff42

Intel Gen12 (Tiger Lake) track. Continues from `intel-handoff41.md` and closes
its section 5 item 1: wire step (b) into a shell, compose-then-publish, with a
shape gate. For the FIRST time a shipping `.cpp` consumes the sink headers:
`phase0-kext/IntelDisplayProbe.cpp` composes the sink plan and the plane words
a flip would write, and publishes them. It still WRITES ZERO REGISTERS.

The prior session's sink work was committed first as `45806de`.

## 1. What landed

- `phase0-kext/IntelSinkRehearse.hpp` (new, pure, dependency-free). Given a
  firmware plane read (CTL, STRIDE, SIZE, SURF, AUX_DIST) and the aperture
  length, `igSinkRehearse()` computes the reserved end, calls
  `igSinkAllocPlan()` past it, then `igPlaneBuildLinear()` on the plan.
  Verdicts: OK, NULL_OUT, SAMPLE_REFUSED, PLANE_DISABLED, NO_BOUND,
  ALLOC_REFUSED, PLANE_REFUSED. Every stage field starts at
  `IG_SINK_REHEARSE_NOT_REACHED` (all-ones), so a stage that never ran can
  never read as `*_OK` (0).
- `tests/test_intel_sink_rehearse.cpp`: **60 checks, 0 failures**. The success
  case is the real 2026-08-18 sample, and the expected numbers were worked out
  by hand: reservedEnd 0x02A5D000, sink base 0x02A80000, stride 7680, bytes
  0x7E9000, regs CTL 0x82000000 / STRIDE 120 / SIZE copied / POS 0 /
  SURF = base.
- `IntelDisplayProbe::publishSinkRehearsal()`, called from `start()` after
  `scanPlanes()` and before `registerService()`. Keys, in order:
  - unconditional: `IGSinkRehearsalSample`, `IGSinkRehearsalVerdict`,
    `IGSinkRehearsalWroteRegisters` (literal `false`);
  - inputs on every path: `IGSinkRehearsalIn{Ctl,Stride,Size,Surf,AuxDist,ApertureBytes}`;
  - stage results on every path: `IGSinkRehearsalAllocResult`, `IGSinkRehearsalPlaneVerdict`;
  - behind `if (!(inSample == IG_SAMPLE_OK && igSinkRehearseUsable(verdict)))
    return;`: `IGSinkRehearsalReservedEnd`, `IGSinkPlan{Base,Stride,Bytes}`,
    `IGSinkPlane{Ctl,Stride,Pos,Size,Surf}`.
  The plane is pipe A plane 1 (`IG_SINK_REHEARSE_PIPE`) and the aperture is
  BAR2 (`IG_SINK_REHEARSE_APERTURE_INDEX`), measured with
  `getDeviceMemoryWithIndex()->getLength()`.
- `tests/test_intel_sink_rehearsal_shape.py` (new): **23 tests** covering
  P1-P5 (compose once, before any publish; ordering in `start()`; the three
  first keys unconditional with a literal `false`; every composed key and every
  `.plan`/`.regs`/`.reservedEnd` value behind a guard written as a conjunction
  of equalities that returns; no write path in the method or the pure header).
  It runs 20 in-memory negative controls and refuses rather than passing when
  the subject is renamed.
- Registered in: `tests/CMakeLists.txt`, `tests/check-kext-syntax.bat`
  section 1, `tests/test_check_count_floor.py` (MEASURED 60, dated reading),
  `tests/test_check_count_last_line.py` (SUITES + checks-format roster).

## 2. Findings worth keeping

- **The firmware plane is COMPRESSED.** The 2026-08-18 capture reads
  PLANE_CTL 0x82009000, which has render-compression bit 15 set. So
  `igFbInheritable()` refuses it, and `IntelFbPresentProbe` will not attach on
  this laptop. That is why the rehearsal lives in `IntelDisplayProbe`, which
  always attaches. The composed plane CTL (0x82000000) is linear and
  uncompressed by construction.
- **AUX must be reserved too.** AUX_DIST is 0x007F8000, so the CCS surface
  sits after the main surface. The reserved end therefore takes the max of the
  main end and surf+auxDist+main. This is a POLICY over-reservation: the AUX
  size is not measured, so the main size stands in for it. The mask
  `IG_SINK_AUX_DISTANCE_MASK_DOCUMENTED` cites
  `reference/i915/skl_universal_plane_regs.h:208`.
- **SURF moves between boots.** 0x01A7C000 on 2026-08-18 vs 0x01058000 on
  2026-08-16. The rehearsal reads SURF live at `start()`. A plan computed from
  one capture must never be reused on another boot.
- **PLANE_REFUSED is unreachable from a sound plan.** It is a defensive arm.
  The suite's agreement sweep pins that, so the day the alloc and plane-encoder
  contracts diverge, the suite reddens instead of the arm silently becoming
  live.
- **Shape gates spell their subject by text.** Adding the method made two
  mutation anchors in `tests/test_intel_publisher_sample_shape.py`
  (`ctl, stride, size, surf` and `UInt32 sample = igSampleVerdict(sampled,`)
  match three times. That test refused rather than silently passing. I renamed
  the rehearsal's locals to `in*` (with a comment saying why) rather than
  loosening that gate.
- `tests/test_source_cited_paths.py` caught a capture path broken across a
  comment line wrap. Citations must sit on one line.

## 3. Gate readings (2026-10-12, after the last code edit)

- `python tools/check-registers.py`: 260/260 passed.
- `tests\build-msvc.bat`: 26/26 suites passed (was 25).
- `tests\check-kext-syntax.bat`: ALL SYNTAX GATES PASSED (includes
  check-header-coverage and check-negative-controls).
- `g++ -std=c++11 -Wall -Wextra -Werror` on the new suite: 60 checks, 0 failures.
- Python gate (`unittest discover`): 1430 tests, 27 skipped. The only expected
  failure is the environment-only `test_the_shell_half_parses`: `bash` on PATH
  resolves to WSL's `System32\bash.exe`, which cannot open a `D:\` path.

## 4. The payload, and what to read on hardware

This is the fifty seventh image: ISO md5 `2792a35bc56fc5514e532e43ead4d2f6`,
897024 bytes; `kext-src.tar` 819200 bytes, md5
`ec7bc97fe1eeed144fd541026a44aef1`. Two consecutive builds were identical. The
kext binary this image builds is CHANGED.

After loading it, read `ioreg -l -w0 | grep IGSink` and record:

1. `IGSinkRehearsalSample` / `IGSinkRehearsalVerdict`. Expected `ok` / `ok`.
   Anything else: record `IGSinkRehearsalIn*` and the two stage numbers.
2. `IGSinkRehearsalInApertureBytes`, the first real measurement of BAR2's
   length on this laptop. The suite's 256 MiB is a fixture, not a reading.
3. `IGSinkRehearsalInSurf` against both prior captures (it is expected to move).
4. `IGSinkPlanBase` must be above `IGSinkRehearsalReservedEnd` and 256 KiB
   aligned. `IGSinkRehearsalWroteRegisters` must be `No`.

## 5. Still open, highest value first

1. **Real GGTT size and the other stolen-memory users.** The plan is bounded
   by BAR2's length and by the firmware plane's own reservation only. Other
   stolen-memory users (GuC/HuC, the WOPCM, firmware's other buffers) are not
   excluded yet. Do not write a flip before they are.
2. Read section 4's keys on hardware. Nothing about the composed values has
   been seen on silicon.
3. The WSL `bash` shell-half test: decide skip-when-WSL vs. fix PATH (pending
   the user).
4. Shape gates spell their subject by literal text (section 2). A rename in one
   shell can redden an unrelated gate's anchors. This is honest but noisy, and
   it is worth a shared anchor-uniqueness helper.
