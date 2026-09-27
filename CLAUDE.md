# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

An exploratory attempt at a macOS GPU driver for a mobile RTX 3050 (GA107). It is
not a shipping driver and `PLAN.md` is explicit that Phases 0-5 are the honest
ceiling — even a complete driver would not put a picture on the target laptop's
screen. The work that exists is real, buildable, and tested; the scope is
deliberately bounded.

Development happens on **Windows**. Building the kext needs Apple's clang with
`-mkernel -fapple-kext` and the Kernel framework headers, so it happens in the
macOS VMware guest — and **as of 2026-08-14 it has happened**: `make exit
status: 0`, bundle PRESENT, Apple clang version 17.0.0 (clang-1700.0.13.5) on
macOS 15.7.9 (24G830). The log is `step7-results/step7-build-SUCCESS.log`. Every
doc in this tree older than that date says the kext has never been compiled;
that sentence is left standing in the numbered handoffs on purpose, because they
are a chronological record rather than a status page.

What the Windows side can do without the guest is compile and *run* the pure
decode logic, and parse the IOKit shell with clang's front end. Both are wired
up as gates — see below. Those four gates are still the fast loop and still the
entire test story; the guest build is the slow confirmation that Apple's real
KPI signatures match `tests/kextshim/`.

What the build does **not** establish: that the kext loads, matches, or runs.
It was never `kextload`ed — that needs SIP lowered and a reboot, and the guest
has no NVIDIA card to match against anyway.

## Read order on resume

The numbered handoff files are a chronological record, newest last. Read the
highest-numbered one first, then work backwards only as far as you need.

**Naming, as of 2026-08-18.** Handoffs up to and including `handoff25.md` are
bare-numbered and interleave both tracks in one number line. From 26 onward
each handoff carries a TRACK PREFIX ahead of the number — `intel-` for the
Intel Gen12 display track, `3050-` for the NVIDIA GA107 track — and the two
sequences are independent, so `intel-handoff26` and `3050-handoff26` are
different sessions rather than two halves of one. (The prefixes are spelled
here as bare prefixes rather than as a filename with a placeholder number,
because `tools/check-documented-paths.py` resolves every path-shaped token in
this file against the checkout and an angle-bracket placeholder reddens two
gates — the same trap as the SDK-include note below.) The convention exists because
the two tracks are worked by concurrent agents and collided on the bare number
26 twice in one day, the first collision destroying a file that had to be
recovered from a session transcript. A disjoint namespace is the prevention;
the detection is in `intel-handoff26.md` §8 and is worth reading once — a
directory listing in a concurrent tree is a measurement with a timestamp, and
the tell is a `Write` reporting "updated" on a path you believe is new.
`tests/test_documented_paths.py` exempts all three spellings from its
root-`.md` coverage rule, so a handoff added under either prefix needs no gate
edit.

**Before you read them:** every handoff up to and including `handoff12.md` ends
by naming STEP 7 as the highest-value open item. That is no longer true. STEP 7
was answered on 2026-08-14 and `OPERATOR-ACTION-step7-build.md` carries a
superseded banner explaining what actually blocked it — not the CD delivery
channel every card was built around, but the guest having no toolchain at all.
Do not open a card assuming its premise still holds.

- `intel-handoff40.md` — most recent state on the Intel track, and the one to read
  first on this side. It lands the **COMPLETION** layer:
  `phase0-kext/IntelGtFence.hpp` is the ninth GT header,
  `tests/test_intel_gt_fence.cpp` (**73** checks) drives it on Windows,
  `phase0-kext/IntelGtProbe.cpp` grew `composeFence()` and
  `publishComposedFence()`, and `tests/test_intel_probe_fence_shape.py` (**22**
  tests) is the FIFTH structural observer over that shell. §1 is why it is the
  last structural gap rather than another packet layer: the four headers beneath
  it compose what the engine would DO and none of them answers **how this driver
  would ever learn the work finished** — a submission whose completion cannot be
  observed is indistinguishable, from the driver's side, from one that never ran,
  and the two call for opposite next actions. The ordering is the whole content
  of the layer, because a fence store reordered ahead of the work it fences
  reports completion for work that has not happened, which is worse than no fence
  — it is a fence that lies in the direction of proceeding. **§3 is the one to
  read, and it is not about the fence.** The suite's final printf was
  `test_intel_gt_fence %d checks, %d failures` — a suite name inside the literal,
  friendly to a human reading a scrollback of twenty suites — and it reddened
  **five** checks across two modules in two different ways, with no failure
  message anywhere saying "format string". `test_check_count_last_line` matches
  against exactly two COMPILED patterns and a named format matches NEITHER, so
  `_format_index()` returned `None` and the two tests that INDEX on that return
  **ERRORED** rather than failed; two more FAILED as list-equality against a
  hardcoded roster of which suite uses which format; and `test_check_count_floor`
  could not read the binary's own output at all, because its extractor is the
  same pair of patterns — **the suite ran clean and printed 73 while the gate
  over it reported the count as MISSING**. The durable half is that last one, and
  it is not new: `test_intel_gt_ring` hit the identical thing on 2026-08-28 and
  the floor table's own provenance comment in
  `tests/test_check_count_floor.py` at line 1254 records it in those words — *the final
  printf is an INTERFACE, and a suite name in it is not a decoration* — so this
  is the SECOND session to hit it, one session later, having read that comment.
  The rule is flat rather than a lesson: **the last line of a C++ suite is
  consumed by machines and there are exactly two legal spellings of it. Copy the
  line from a sibling; do not compose it.** §3a is a sixth failure from a
  **comment** — quoted text on a `#` line in `tests/CMakeLists.txt` at line 461
  (spelled that way rather than as a colon-suffixed path, because
  `tools/check-documented-paths.py` reads the whole token as a repo file and
  reddens two gates over it — the same trap as the SDK-include note below), where
  the premise test exists precisely to pin that the comment stripper's
  every-`#`-starts-a-comment assumption HOLDS in this file; the gate was right
  and my prose was the counterexample, the third distinct time this tree has
  reddened a gate with a comment rather than with code. §2 is the five gated
  properties, and property 5 is that **no MMIO store exists on the fence path** —
  the absence is GATED rather than claimed in prose, handoff37 §3's construction
  again. Python 1385 → **1407**, the +22 attributed by running the module ALONE
  before the gate rather than inferred from arithmetic, which is the correction
  `intel-handoff39.md` §5a had to make; C++ suites 19 → **20**; register gate
  unmoved at **260**, correctly, since every constant in the new header is POLICY
  and handoff37 §1a's asymmetry means **a POLICY constant going wrong is
  invisible to that gate by construction**. Shipping code CHANGED, so
  `build-payload/` was rebuilt — the **FIFTY FOURTH** image, both md5s and both
  byte sizes moving by exactly three 10240-byte tar blocks each. §6 item 1 is the
  honest limit: **the fence has never fired**, because nothing in this tree has
  ever submitted anything. §7 item 1 is the new highest-value item and it is now
  a CLASS rather than an instance — all five Intel shape gates spell their
  subject by NAME, so a second composer added later yields something the gate
  does not consider its subject at all and it reports NOTHING rather than
  failing; `intel-handoff26.md` §2a already solved this once by DERIVING the
  subject set, and the pattern has not been reached for since.
- `intel-handoff39.md` — the state handoff40 continues from. It closes TWO items
  from `intel-handoff38.md` §6: item 1
  (this very read-order list, which stopped at `intel-handoff28.md` while the tree
  reached 38) and item 3, the older of the two by seven handoffs — no gate of any
  kind covered the 295 lines of Python inside a quoted heredoc in
  `tools/igpu-final-capture.sh`. `phase0-kext/` is BYTE-IDENTICAL, so
  `build-payload/` was NOT rebuilt and the fifty third image is still current.
  **§1a is the one to read**, because it is the argument for why a gate beats the
  hand check `intel-handoff27.md` §4 recorded: every other script in this tree can
  be re-run and this one cannot, so a `SyntaxError` or a `NameError` discovered at
  capture time is not a bug to fix — it is evidence that no longer exists. That
  makes "it does not parse" the UNINTERESTING failure, since `compile()` catches
  it and a hand check caught it once. The three that survive a `compile()` are a
  name read and never bound (Python resolves globals at RUN time, so a typo'd
  identifier compiles cleanly and raises only when its branch executes — which on
  this script is during the one capture), the heredoc losing its QUOTING (`<<'PYEOF'`
  passes the body through untouched while a bare one lets the shell expand `$VAR`
  and backticks inside a Python program, so the script still runs and runs a
  DIFFERENT program), and a `%`-format whose placeholder count disagrees with its
  argument tuple. `tests/test_igpu_final_capture_embedded.py` is **20 tests** and
  pins all three by walking the AST rather than grepping. §2 is the two predicates
  that could have been vacuous and their own controls, and both are permissive in
  the same deliberate direction: `unbound_names()` is NOT scope-accurate, because
  the defect it exists to catch is a name bound NOWHERE and a scope-accurate
  resolver would redden on correct code that shadows; `format_mismatches()` skips
  every non-literal operand rather than guessing. The false-positive direction is
  the one that gets a gate deleted. §3's third control is the one justifying the
  module — it asserts the mutated body still `compile()`s and THEN asserts the
  typo appears in the unbound set, so a gate that only compiled would be green on
  it; §3a is a property that was not on any open list, the script's own header
  promising read-only with nothing checking it. **§5 is four annotations I got
  wrong** while closing item 1, three of them written from the shape of a
  neighbouring file rather than from the file itself — 31/32 are
  `IntelFbPresentProbe` IOFramebuffer work rather than GT headers, 29 is the FIRST
  of the four Intel shape gates rather than an ordinary continuation, and a cited
  §9 item 2 belonged to a framebuffer session and was removed rather than
  restated. An annotation written from a neighbour is a guess wearing a citation,
  and the read-order list is the first thing a cold reader trusts. **§5a closes
  the attribute-the-Python-total item** that had been inherited since
  `intel-handoff31.md`, and it closes it by finding that the item named a total
  nobody ever measured: "1279 → 1315" is 1347 minus 32, arithmetic written in the
  register of a measurement, inside an item whose whole subject is that numbers
  need attributing. Handoffs 35 and 36 BOTH read 1279, so the entire +68 landed in
  handoff37's own session rather than "across two sessions" as 37 §6 item 3 says —
  ring-write 15, blit 21, submit 32, and **15 + 21 + 32 = 68 with 1279 + 68 = 1347
  exactly**, no other combination of the measured modules summing to 68. Each
  module was run ALONE; that older handoff is not edited, same rule as handoff28
  §7 item 5. Python 1365 → **1385**, the +20 attributed the same way BEFORE the
  gate; register gate unmoved at 260, correctly. §4a is the syntax-gate trap firing at
  line **292** rather than 272 — the line number moved and the trap did not, which
  is the whole argument for reading the exit code. §6 item 1 is the honest limit:
  the embedded program has still never RUN, and the machine that could run it is
  gone. §7 item 3 carries a grep-first warning, because the inherited
  `IOFramebuffer` item is written as though `IntelFbPresentProbe` did not already
  exist.
- `intel-handoff38.md` — the state handoff39 continues from. It closes
  `intel-handoff37.md` §6 item 1 and adds
  `tests/test_intel_probe_image_shape.py` (**18 tests**), the FOURTH and last
  structural observer over `phase0-kext/IntelGtProbe.cpp` — the MMIO path, the
  ring write, the blit and the submit rehearsal each had one, and the twelve-dword
  ring context image was the gap. `phase0-kext/` is BYTE-IDENTICAL, so
  `build-payload/` was NOT rebuilt and the fifty third image is still current.
  **§2 is the one to read**, and it is `bareness-is-not-nesting-depth` arrived at
  from a third direction: property 4's negative control (wrap the verdict's
  `setProperty()` in an `if`) returned an EMPTY failure list, and the mutation was
  right while the predicate was wrong. Brace depth cannot see the mutation that
  matters, because `if (x) setProperty(...);` has no braces at all and the guarded
  call sits at exactly the same depth as an unconditional one; the walk also
  started INSIDE `setProperty()`'s own argument list, so the first thing it met
  going backwards was that call's unmatched `(` and it unwound into nonsense. It
  is now a paren-balanced walk back to the previous statement boundary with the
  counter CLAMPED at zero, then a guard-keyword read of what stands between the
  boundary and the key. Treating `}` as a boundary is the load-bearing half: walk
  THROUGH it and the `if` heading the PRECEDING block is read as this statement's
  own guard, so a correct unconditional publication sitting below any guarded
  block reports as guarded and the positive control reddens on correct shipping
  code. §1a is the six properties; the separating mutation for the whole module is
  one line — delete the `igGtiWriteRingImage()` call from `composeImage()` and
  leave the publisher alone, and all four gates stay green while
  `IGGtImageDwords` publishes `init()`'s zero as though composed, handoff18 §1d's
  class for the eighth time. Python 1347 → **1365**, the +18 attributed by running
  the module alone BEFORE the gate; register gate unmoved at 260, correctly — a
  Python shape gate carries no `#define`s. §5 item 2 is the honest limit and it is
  the VACUITY direction: the gate spells `igGtiWriteRingImage(` by NAME, so a
  second image writer added later yields a composition the gate does not consider
  one at all, and it reports nothing rather than failing — the same hole
  `intel-handoff26.md` §6 item 4 names for `regRead32(`. §6 item 1 is the oldest
  item in this tree and is now the highest-value one precisely because every
  cheaper one has been closed: this very read-order list stopped at
  `intel-handoff28.md` while the tree reached 38.
- `intel-handoff37.md` — the state handoff38 continues from. The execlist layer
  finally has a caller: `IntelGtProbe::rehearseSubmit()` reads `EXECLIST_STATUS`,
  composes a context descriptor over `IG_GTP_LRCA_POLICY`, judges the whole
  submission through `igGtxSubmitReady()`, and `publishSubmitRehearsal()` puts the
  verdict, its name, the descriptor and its validity into the IORegistry. **It
  submits NOTHING**, and §3's property 5 is what makes that claim worth something
  — `igGtxExeclistSqReg(`, `igGtxExeclistControlReg(` and the three
  `IG_GTX_EXECLIST_*_POLICY` constants must not occur ANYWHERE in the file, so the
  absence is a gated property rather than a prose claim. §1 is the vendoring:
  `reference/i915/` grew `intel_lrc_reg.h`, `intel_engine_regs.h` and
  `intel_gpu_commands.h`, which closed handoff36 §6 item 5 —
  `RING_CONTEXT_CONTROL(base)` is `(base) + 0x244` at `intel_engine_regs.h:181`,
  so `IG_GTP_FIRST_PAIR_UNCITED` became `IG_GTP_FIRST_PAIR_I915_0x244` and **the
  value did not move, it stopped being a guess.** Register gate 242 → **260**.
  §1a is the asymmetry to carry: the vendoring makes three specific things cited
  and leaves everything else exactly as uncertain, so the 260 counts only the
  first kind and **a POLICY constant going wrong is invisible to that gate by
  construction** — every constant in `IntelGtExec.hpp` is still POLICY, confirmed
  by greps for `EXECLIST`, `ELSP`, `CTX_DESC`, `GEN8_PDE`, `GEN8_PTE`, `PPAT` and
  `GGTT` all returning ZERO across `reference/i915/`. **§2a is the durable rule
  and it is narrower than "grep the name"**: two clang errors were both ARITY, and
  both new constants are COMPOSED from the sibling that owns them rather than
  spelled — `IG_GTP_BLIT_DEPTH_POLICY` = `IG_GTB_DEPTH_32_I915_0x03000000`
  because the value is a PRE-SHIFTED header dword (`BLT_DEPTH_32` is `(3 << 24)`,
  so a hand-written `3` composes an 8bpp packet while reading as thirty-two), and
  `IG_GTP_SUBMIT_LENGTH_BYTES_POLICY` is a product of the packet's dword count and
  the word size. **Compose the value from the sibling that owns it, so that
  getting the NAME wrong is the only mistake available** — a wrong name is a
  compile error clang points at, while a wrong VALUE compiles cleanly and every
  gate stays green over it. The length literal is the worse of the two because
  twelve bytes is what three dwords ARE today, so it is CORRECT until somebody
  adds a dword. §3b is the control that had to be narrowed twice: a mutation
  swapping the LRCA in BOTH the composer and the join reddens the property and
  proves nothing, because the property is about their AGREEMENT — so the mutation
  hits the join's argument list only and the assertion pins the composer as
  untouched. Keys are matched WITH their closing quote, because
  `IGGtSubmitReadyVerdict` is a PREFIX of `IGGtSubmitReadyVerdictName`. §4a is the
  three reading traps, all three still live. §6 item 1 is what handoff38 closed.
- `intel-handoff36.md` — the state handoff37 continues from. Its §6 item 5
  forward-referenced an `intel-handoff37.md` that did not exist until that write,
  and its subject (`RING_CONTEXT_CONTROL` being uncited) is handoff37 §1. Its §2
  is the same finding as handoff37 §2 one session earlier — the syntax gate is the
  only gate that can find a name-and-signature defect — and §2b is the
  grep-the-tree-do-not-recall-the-name rule handoff37 §2a narrows.
- `intel-handoff35.md` — the state handoff36 continues from. It added
  `phase0-kext/IntelGtImage.hpp` and its suite and rebuilt the payload. **§2 is
  the one to read and the session's real result is not the header**: the suite
  CRASHED on its first run, and the two defects that crash measured were both
  mine, both in the header, and both of the kind that compose a WELL-FORMED page
  — which is the dangerous direction, because a malformed page is caught by the
  next thing that reads it and a well-formed wrong one is not. §2a is why
  handoff38's property 5 exists: two spellings of one length DRIFT.
- `intel-handoff34.md` — the state handoff35 continues from. It added
  `phase0-kext/IntelGtCmd.hpp`'s test suite, registered it in every gate that has
  to know about it, and closed the payload red that had stood since 2026-08-20.
  **§0 is the sentence it is easy to misread**: the operator asked for an
  ACCELERATED driver and retargeted it to the Intel iGPU, and what lands each
  session is command-packet arithmetic with a host suite over it — not
  acceleration, and the file says so in its own first section. §6 item 4 is still
  open and still inherited: `tests/test_intel_scanout.cpp` has no `MEASURED`
  entry, so its check count has no floor.
- `intel-handoff33.md` — the state handoff34 continues from, dated 2026-08-25.
  `phase0-kext/` is byte-identical. **§0 is the one to read**, because it is where
  the whole GT track is justified: the request was an accelerated driver, then
  redirected to the Intel iGPU, and §0 states the two things that must be said
  before any of the work reads as progress — both of them measurements in this
  tree rather than opinions.
- `intel-handoff32.md` — the state handoff33 continues from, dated 2026-08-20.
  `phase0-kext/` byte-identical, FORTY SECOND image still current. §0 is the same
  finding as handoff31's arrived at one session later and it is worth knowing
  before anybody "makes an IOFramebuffer" a third time: **one already exists** —
  `IntelFbPresentProbe`, wired into `SOURCES`, `Info.plist`,
  `tests/kextshim/IOKit/graphics/IOFramebuffer.h`, with eleven overrides, a
  `presentable()` guard and a shape gate over it. Grep the tree before building
  the thing the tree already has.
- `intel-handoff31.md` — the state handoff32 continues from, dated 2026-08-20.
  Shipping code CHANGED (`phase0-kext/IntelFbPresentProbe.cpp`), so
  `build-payload/` was rebuilt — the FORTY SECOND image. §0 is the durable half
  and it is the hole this tree files after EVERY IOKit shell lands: the
  `IOFramebuffer` the operator asked for already existed, and the honest
  continuation was not a second copy but the fact that **no Python module named
  it**, so deleting a verdict guard out of it left every gate green. That is the
  same argument handoff38 closes for `IntelGtProbe.cpp`'s fourth composition.
- `intel-handoff30.md` — the state handoff31 continues from. It closes
  `intel-handoff29.md` §8 item 2 — harvest the remaining configuration registers
  out of the final capture — and unlike the several Intel sessions before it, it
  CHANGED shipping code: `phase0-kext/IntelDisplay.hpp` moved, so
  `build-payload/` was rebuilt (the **THIRTY NINTH** image) and
  `OPERATOR-ACTION-step7-build.md` updated. Both were obligations rather than
  choices — the rule that decides a rebuild is CONTENT, and the Python gate is
  the thing that reads content.
- `intel-handoff29.md` — the state handoff30 continues from, and the FIRST of the
  four structural observers over an Intel IOKit shell:
  `tests/test_intel_probe_mmio_path.py`, **17 tests**, closing item 2 of the
  operator's five-item list. `phase0-kext/` byte-identical. Its §8 item 2 is what
  handoff30 closed.
- `intel-handoff28.md` — the state the Intel track built the GT work on top of, and the session that
  **closes the operator directive**: the Linux phase is finished and that laptop
  can be overwritten with macOS. §0 is the one to read first, because it flips
  the asymmetry every recent Intel session has worked under — DUMP-when-in-doubt
  has expired, and from here every remaining Intel question is answered out of
  the two files in `igpu-results/` or it is not answered at all. §1 is a failure
  worth carrying: the FIRST run of `tools/igpu-final-capture.sh` produced a full
  provenance block and then zero bytes of MMIO, dying on `PermissionError` at the
  `mmap` — Secure Boot puts the kernel in `lockdown=integrity`, which refuses
  `mmap` of a PCI BAR through sysfs **even to root**, and `CAP_SYS_RAWIO` cannot
  lift it because it is a policy refusal rather than an access-control one. It
  was diagnosable only because the script prints `lockdown` and `secureboot`
  BEFORE attempting the map; without those two lines the honest reading of the
  artifact would have been "the embedded Python is wrong" and the correct next
  action would have been editing a script that was already right. The script was
  NOT changed. The operator disabled Secure Boot, the second run
  (`20260818-201412 UTC`) returned **718 live dwords across 25 ranges**, and it
  landed as `igpu-results/tgl-final-capture-2026-08-18-201412.txt` — raw run plus
  host-side notes below a divider, same two-documents-in-one-file convention as
  the 2026-08-16 capture. **§3 is the one to read after §0**, and it is two
  defects in the one module whose entire job is protecting unrepeatable machine
  output. Its glob was the bare `tgl-capture` spelling with a trailing wildcard,
  which does NOT match a name of the `tgl-final-capture` form (spelled in words
  here rather than as the pattern itself, because `tools/check-documented-paths.py`
  reads a wildcard-plus-extension token as a repo path and reddens two gates over
  it — the same trap as the SDK-include note above), so the larger of the two
  irreplaceable captures was
  invisible to its own gate — unhashed, unguarded, and reported green while being
  so. A gate that silently narrows its own subject set converts an absence of
  coverage into a positive-looking result, which is worse than no gate. Widening
  the glob is not the whole fix, because a future edit can narrow it again with
  every other arm staying green on a smaller set; the widening is made
  load-bearing by a set-EQUALITY arm (a third capture reddens until somebody
  measures it) and a strict-subset arm pinning that the broad pattern is provably
  broader in this tree. §3b is the second defect and the more interesting one:
  `BELOW_ANCHOR` was `UNPROGRAMMED`, which occurs only in the 2026-08-16 notes
  and **21 times ABOVE the divider** in the new capture, because it is a verdict
  word the script itself prints. Arm 3 rewrites the below-anchor and then
  requires the hash to HOLD, so that anchor would have rewritten 21 bytes of
  frozen machine output and failed loudly for entirely the wrong reason — the
  over-match direction of `a-mutation-that-matches-nothing-prints-ok`, and the
  tempting fix ("skip that file") is §3a's narrowing arrived at from the other
  side. Both anchors are now verified BY COUNT against every artifact before use:
  `0x9a68` (the iGPU's PCI device id) 1-above/0-below, `MACHINE:` 0-above/1-below.
  §4 is what the capture answers — the DSI `0x800` stride is **witnessed** for the
  first time, as two pages at that spacing holding identical non-trivial reset
  state, which is weaker than reading a programmed mode out of both and far
  stronger than a stride this project invented; Q4's PIPESRC `+1` is confirmed on
  a second boot; Q3 (plane count) is now permanently unsettleable. **§4d is the
  rule anyone harvesting constants must follow**: two reads of the same register
  inside one run disagreed, clustering at `0x064000` (`DDI_BUF_IS_IDLE` is a live
  status bit) and the watermark band around `_PLANE_WM_1_A_0` — so a register read
  on a running display is a SAMPLE, not a constant, and only configuration
  registers may become `*_OBSERVED_*`. §5 records the Python gate RED on one
  failure that was the peer's (`build-payload/` built 14:29, their
  `NV3050Probe.cpp` 16:39 and `NVGspBoot.hpp` 16:45), deliberately not rebuilt.
  Register gate unmoved at **240**; `phase0-kext/` byte-identical. §7 item 1 is
  the expired one worth knowing: attaching an external display before the one
  boot is now permanently unreachable, and it is the largest piece of evidence
  this track will never have. §7 item 2 is the highest-value open work —
  harvesting configuration registers out of that capture into
  `phase0-kext/IntelDisplay.hpp`.
- `intel-handoff27.md` — the Intel-track state intel-handoff28 continues from, and independent of
  `3050-handoff27.md` (the peer's NVIDIA session; either order). It touched no
  shipping code — `phase0-kext/` is byte-identical, so the payload gate has
  nothing to say about it and `build-payload/` was not rebuilt by me. What moved
  is the CAPTURE layer, and it closes the operator directive rather than an item
  from the open list: the Linux phase is now ONE file and ONE command,
  `tools/igpu-final-capture.sh`, replacing six accumulated scripts. §0 is the
  constraint that shapes every decision in it — the laptop gets macOS installed
  over this Linux afterwards, so anything the script omits is permanently
  unknowable and the standing rule is DUMP when in doubt. **§2 is the one to
  read**: the three existing MMIO range tables in `tools/read-igpu-mmio.py`,
  `tools/read-igpu-mmio.sh` and `tools/igpu-capture.sh` are held IDENTICAL by
  `tests/test_igpu_mmio_ranges.py` — they answer one question three ways, so any
  difference is a bug in one of them — and the final capture needs ten pages they
  do not have (transcoder/pipe D, the DSI pages, the Gen12 PLL block at
  `0x164000`, the PCH fuse and DPLL pages). Folding it into that equality forces
  either the three canonical tools to grow ranges they have no reason to read or
  the final capture to lose its whole reason for existing, so the gated relation
  is CONTAINMENT in one direction only, and the drift check survives the
  asymmetry: adding a range to the canonical three without adding it here
  reddens. The pair is compared base AND length, because a base-only comparison
  passes a row that kept `0x044000` and shrank `0x2000` to `0x1000`, silently
  halving a block the one capture gets to read. The existing bounded parser
  needed no change — writing the new table in the same `RANGES = [` form at
  column 0 inside the heredoc means `_python_list_ranges()` reads it while the
  thirty-odd hex values in the header's citation table stay invisible. §3 is the
  non-vacuity proof and its shape is the cleanest argument in the file for why
  containment needs an anti-degeneracy check: a deleted row and a HALVED row both
  redden the same one check (correctly — it is one claim, but only because the
  pair is compared), an exact canonical copy reddens only
  `test_it_is_a_strict_superset`, and a renamed list reddens four with the floor
  firing first. Without the strictness check a future "unify the four tables"
  edit would strip all ten additions and leave the gate fully green. Mutations
  ran against a temp-directory copy with `FINAL_TOOL` repointed, never the live
  tree. §5a is a gate reading worth carrying: the Python gate was RED on two
  `test_operator_step7_doc` checks that were not mine — the peer edited
  `phase0-kext/NVRiscvUcode.hpp` at 14:25 and rebuilt `build-payload/` at 14:29
  against a card last written at 13:09 — and the correct action was to attribute
  by mtime and touch nothing; they rewrote the card at 14:34 and the gate went
  green underneath me. Rebuilding the payload would have vouched for a peer's
  half-landed work under this session's name. §4 is an honest gap: the script is
  a shell file with 295 lines of Python inside it and NO gate covers the embedded
  half — both halves were checked by hand (`sh -n` plus extracting the heredoc
  and `compile()`ing it), which is §7 item 2. Register gate unmoved at **240**,
  correctly: a capture script carries no `#define`s, only comments citing
  `reference/i915/`. Python 952 / OK; C++ 9/9; syntax exit 0. §6 item 5 is the
  one to know before editing the table: `0x0C7000` and `0x0C8000` are dumped
  with NO named anchors because `intel_pps_regs.h` and `intel_dp_aux_regs.h` were
  never vendored, so `PP_STATUS`, `PP_CONTROL` and the `AUX_CH_CTL` family have
  no citable line in this tree — confirmed by four greps rather than assumed, and
  inventing names would put fabricated citations beside real ones. §7 item 5 is
  time-critical rather than important: attaching an external display BEFORE the
  one boot makes Q3 answerable on a second pipe and converts several policy
  constants into measurements, and it cannot be done afterwards.
- `3050-handoff37.md` — most recent state on the NVIDIA track, and the one to read
  first on this side. `3050-handoff35.md` and `3050-handoff36.md` sit between it and
  handoff34 and were never added to this list; they are ordinary continuations of
  the log-only-diagnosis sweep and can be read after. **§0 is the section that
  matters, because it answers the question the whole track exists to ask**: the
  GA107 VBIOS in `reference/ga107-firmware/vbios-ga107.rom` **declares zero display
  paths**. Not "none we could decode" — `entry_count` is `0x00` in a Display
  Configuration Block that is present, well formed, version `0x41`, and sitting
  exactly where the pointer at ROM offset `0x36` says it sits, with the connector
  sub-table it points at also empty while its siblings are populated (CCB 15, GPIO
  36, I2C 32). `tools/parse-dcb.py` is the decoder and `dcb-report.txt` is its
  committed output, in `vbios-report.txt`'s idiom. This is `3050-handoff27.md` §1's
  muxless finding arrived at from the opposite end — from the GPU's own firmware
  rather than from the operator's report — and it is stronger in one specific way:
  handoff27 named HDMI as the bypass, and this DCB does not describe an HDMI path
  either, because it describes no paths at all. A driver cannot invent a path the
  board does not carry, so `PLAN.md` termination condition A is now confirmed from
  two independent directions and §6 item 2 is that somebody should write it into
  `PLAN.md` rather than leave it in a handoff. **§2c is the durable methodological
  half** and it is a new class: **an absence is the one finding a decoder cannot
  report honestly on its own evidence**, because landing one field off, landing on
  something that is not a DCB at all, and reading a truncated dump all print a
  byte-identical "no display paths". So `corroborate_absence()` runs seven arms and
  reports `AUTHORED` only when all seven agree, else `UNPROVEN -- treat as a
  MISREAD`; the load-bearing one is the last, because a decode that landed on
  garbage reads garbage counts out of the sibling tables too, while a board that
  genuinely wires no outputs still has populated GPIO and I2C tables for power
  sequencing and thermals. The threshold is two populated siblings and there is a
  test on BOTH sides of it. §2d is why a signature scan is deliberately NOT a
  fallback in `find_dcb()`: a 32-bit constant occurs by chance in 64 KiB often
  enough that finding one proves nothing, and a decode landing somewhere plausible
  reports fabricated board wiring — handoff18 §1d's class aimed at the one question
  this track turns on. §3a is four defects the new gate's own controls found, all
  four in the tests and none in the tool, and its item 3 is the interesting one: a
  negative control reddened TWO arms instead of one and that was CORRECT, because
  `signature_occurrences()` scans forward so a decoy planted below the header
  legitimately breaks both uniqueness and pointer-selection. The fix is to plant it
  ABOVE when isolating one arm and keep the below case as its own PAIR-asserting
  control — forcing a coupled pair into a single-arm control means weakening a
  correct assertion to accommodate a wrong test. Item 2 is the third session
  running where a first-draft failing test was the positive control rather than the
  subject. `tests/test_parse_dcb.py` is **49 tests**, Python 1024 → **1073**;
  register gate unmoved at **240**, syntax gate exit 0. §4a is the peer's red left
  alone for the second session running — `tests/test_intel_scanout.cpp` landed
  mid-session with no `MEASURED` entry, and adding one would vouch for a C++ suite
  this session never built. §5 item 1 is the honest limit: this is a firmware read,
  not a hardware measurement. §6 item 1 is the highest-value remaining NVIDIA work,
  and it is the same class for the fifth time — handoff37 §1 gave
  `nvRiscvErrorVerdict()` five codes in `phase0-kext/NVFalcon.hpp` and the shell
  still consumes only the bool.
- `3050-handoff34.md` — the state `3050-handoff35.md` continues from, and the direct
  continuation of `3050-handoff33.md`. It closes that file's §8 item 3, the item
  named there as the cheapest remaining work and carried forward unchanged from
  `3050-handoff32.md` §8 item 4. `phase0-kext/` is **BYTE-IDENTICAL** — the
  header was mutated and reverted three times during the non-vacuity proof and
  ends the session as it started — so `build-payload/` was NOT rebuilt and the
  thirty third image is still current; the proof of that is the Python gate's
  payload comparison being green rather than an assertion. **§1 is the one to
  read** and it is a property of BOOLS rather than of this subsystem:
  `testRealFirmwareFiles()` asserted the two real booters through
  `nvRiscvUcodeIsWellFormed()`, and `== false` is satisfied by **all seven**
  refusal codes at once. The accept side was already tight (handoff32 §3 made
  the wrapper an EQUALITY against `NV_RISCV_UCODE_OK`, so `true` has exactly one
  preimage, and this is the first thing that has depended on that choice); the
  refusal side had seven preimages and they are not interchangeable.
  `3050-handoff27.md` §2 measured why — both booters carry an
  `nvfw_hs_header_v2` whose first word is 60 (`0x3C`, that header's own
  `header_size`, not a version), and before handoff27 the function refused them
  three optional-region rows deep because signature bytes at `+0x24` happened to
  decode outside the payload. **Both readings satisfy `== false` identically**,
  so an assertion green through four sessions could not tell handoff27's fix from
  the bug handoff27 fixed, and a booter whose signature happened to decode in
  range would have been ACCEPTED and DMA'd into IMEM as bootloader code. §2's
  load-bearing choice is that the three `checkBool()` calls are **KEPT beside**
  the new `checkVerdict()` ones rather than replaced:
  `nvRiscvUcodeIsWellFormed()` is what the shell actually calls, and a wrapper
  that stopped consulting the verdict would leave a verdict-only suite fully
  green. The PAIR pins the wrapper to its own subject. **§3 is the other thing
  to read** and it is handoff18 §1e's signal in its cleanest form: three
  mutations reddened 3 → 3 → **2** checks, and only the LAST proves anything,
  because the first two each reddened a PRE-EXISTING synthetic check and were
  therefore evidence the old tests already sufficed. The discriminating mutation
  is literally the pre-handoff27 ordering — move the version test BELOW the
  six-row region walk and both booters refuse at `ERR_REGION_OUTSIDE`, every
  synthetic check staying green while exactly the two new booter assertions
  redden. **A mutation that reddens MORE checks is usually weaker evidence.**
  §4a is two reading traps that cost a round trip each: wrapping the C++ gate's
  batch file in a `cmd` call with a doubled-slash `c` flag reports *not
  recognized* and blames the file rather than the invocation (invoke the batch
  directly from `tests/`; the wrapper spelling is written in words here because
  `tools/check-documented-paths.py` reads that whole run as one repo path and
  reddens two gates over it, the same trap as the SDK-include note above), and
  the Python gate's `tail` shows an HFS catalog dump because a tool test prints
  its own parse output — grep for `Ran N tests` / `OK` / `FAILED` and the carried
  exit status. `test_riscv_ucode` 211 → **214**, well inside its band
  (211 + max(8, 26) = 237) so the ceiling correctly did not fire; register gate
  unmoved at 240 and Python unmoved at 980, both correctly. §5 item 4 is the
  honest limit: all three new checks live inside the section that SKIPS when
  `reference/ga107-firmware/` is absent, so a bare checkout reports 211 and tests
  none of §1's claims. §6 item 3 is the new highest-value item not blocked on
  hardware — `loadGspFirmware()` returns `bool` and both the stage verdict and
  the eight-way ucode verdict beneath it die in `NVLOG`, the same class handoff31
  and handoff33 closed for BOOT_42 and BOOT_0, with the detection already built
  and tested and only the publication side missing.
- `3050-handoff33.md` — the state handoff34 continues from, and the direct
  continuation of `3050-handoff32.md`. It closes that file's §8 item 3 —
  `decodeBoot0()`'s log-only diagnoses — and unlike the last two sessions the
  inherited item was **TRUE as written**, which is worth knowing on its own: an
  item with an age is a claim to be MEASURED, and the measurement can come back
  confirming it rather than killing it. **§1 is the one to read** and it is
  `3050-handoff31.md` §1's class for the second time — a diagnosis reaching the
  log and not the consumer, the other half of the same method. `decodeBoot0()`
  detects four facts about the part (a non-Ampere architecture, an Ada part
  SPECIFICALLY — the 'RTX 3050 A' case where the whole plan retargets — GA107
  confirmed, and an Ampere part that is not GA107) and writes all four to
  `NVLOG`, returning `void`. §1a is what the registry said instead, and its
  third item is the durable one: `NVArchitecture` published a bare NAME, so
  `"Ada"` landed byte-identically to `"Ampere"` with nothing carrying the log's
  retarget warning; `NVChipIdExpected` stated the target and never compared; and
  `NVImplementation` was guarded by the Ampere test, so a non-Ampere part left
  the key **ABSENT** — and an absence is a question rather than an answer,
  indistinguishable from an older kext that never published it. That is
  handoff18 §1d's fabricated publication arrived at from the opposite
  direction: there a value nobody measured looked measured, here a real
  measurement's absence looks like a version difference. §1b is why the four
  gates were blind, and its fourth entry is the one to carry:
  `tests/test_probe_identity_publication.py` existed and was GREEN with fifteen
  tests, because all five of its properties were about BOOT_42 and the dead-read
  path — nothing asked what the READABLE path says about the part it read, the
  same shape as handoff31 §1a's own fourth entry. §2's three choices are each
  load-bearing: the four-way decision goes into `NVDecode.hpp` rather than
  staying in the shell, because a pure function written in a shell is a pure
  function nothing runs (that split is the whole argument, and
  `IntelDisplayNames.hpp` is what it looks like ignored and repaired); the **Ada
  test runs FIRST**, because `NV_PART_ADA` is a SUBSET of "not Ampere" and a
  general-first order makes it unreachable — the dead-arm shape handoff32 exists
  to remove, reintroduced one level over, and it is the one finding of the four
  with an operator action attached, pinned by an Ada part carrying GA107's
  implementation number; and `NVPartIsTarget` is an **EQUALITY** against
  `NV_PART_TARGET`, never a negation of an error arm, because `!= NV_PART_ADA`
  is true for a Turing part this driver has never seen and would report an
  unknown part as the target one (handoff32 §3's `!= ERR_HEADER` reasoning one
  subsystem over). `NVImplementation` is now published on every arm.
  `test_decode` 96 → **121**, ABOVE its own ceiling (96 + max(8, 12) = 108) by
  thirteen, so the count band FIRED on its own — unlike yesterday's 188 → 211
  which grazed its edge. §4 extends the existing shape gate rather than adding a
  module (a second one would duplicate the `audit()` this family shares with the
  live tree), 15 → **21** tests, six negative controls each reddening only its
  own property with every pre-existing test green. §4a is `statement_is_bare()`
  and why it is a balanced walk back to the previous statement boundary rather
  than a regex over the preceding line — handoff25 §1c is the cost. **§5a is
  four mistakes of mine and three were referencing something that does not
  exist** (a `check()` helper this suite lacks, a `_BITS` constant the header
  never defined, a helper referenced before it was written, and a variable bound
  under another name); the dangerous one is the second, because a hand-written
  `5` in its place would have compiled and passed as a silent second copy of a
  field layout. Both files changed, so `build-payload/` was rebuilt — **THIRTY
  THIRD** image, and **neither byte size moved while both md5s did**, the
  padding case for the fourth time. §7 item 2 is the honest limit: the log and
  the registry now agree by construction only as long as nobody edits one arm of
  `decodeBoot0()` without the other, and no gate covers that agreement (§8 item
  4). §8 item 3 is the cheapest remaining work — asserting the real booters
  against the ucode verdict rather than the bool.
- `3050-handoff32.md` — the state handoff33 continues from, and the direct
  continuation of `3050-handoff31.md`. **§1 is the one to read and it is a
  correction rather than a finding**: handoff31's own §7 item 3 said
  `nvRiscvUcodeIsWellFormed()` has no caller in the IOKit shell, and that half of
  the item is FALSE. One grep answers it — `NVGspStage.hpp:273` calls it inside
  `nvGspStagePlan()`, `NV3050Probe.cpp:1369` calls that inside
  `loadGspFirmware()`, and `:1125` reaches it from the GSP boot path. The item
  was written from the shape of the previous session's defect rather than from a
  grep, and it named the wrong half as the one that matters. `3050-handoff29.md`
  §1 recurring in the same open list: an item inherited across handoffs is a
  claim with an age, and "nothing calls X" is a set-difference somebody has to
  actually run. handoff31 was NOT edited — same rule as handoff28 §7 item 5. The
  FIRST half was true and had a second defect underneath it. **§2 is the real
  subject**: the validator refused blobs for seven structurally distinct reasons
  and returned one `false`, `nvGspStagePlan()` mapped every one onto
  `NVGSP_STAGE_ERR_MALFORMED`, and `loadGspFirmware():1372` put that single word
  in its refusal log — so a truncated file, a booter handed in by mistake, and a
  descriptor pointing off the end of its own payload were indistinguishable to
  the one person who could act on them, and those call for three different
  actions. The version case is not hypothetical: `3050-handoff27.md` §2 measured
  that both booters in `reference/ga107-firmware/` carry an `nvfw_hs_header_v2`
  where the bootloader carries an `RM_RISCV_UCODE_DESC`. **§2a is the sharper
  half and a NEW member of handoff18 §1d's family**: `NVGSP_STAGE_ERR_EMPTY_BOOTLOADER`
  and `NVGSP_STAGE_ERR_REGION_OUTSIDE` re-read the SAME two descriptor fields the
  well-formed gate had already refused on, so both arms were provably
  unreachable — and `test_gsp_stage.cpp:601`–`:606` SAID SO in its own
  comment while asserting MALFORMED for cases the enum has precise names for.
  That is a gate documenting its subject's dead arm and then ratifying it in four
  assertions; the comment's local reasoning is correct and its conclusion was to
  pin the collapse rather than ask why the branch was dead. Call the class **a
  test that pins the collapse instead of the fact**, and the tell is a test
  comment explaining why a branch in the subject cannot run. §3's three choices
  are each load-bearing: the bool is KEPT as a one-line wrapper that is an
  EQUALITY against `NV_RISCV_UCODE_OK` (a `!= ERR_HEADER` form would call a
  version-mismatched booter well-formed), so no existing caller or assertion
  changed; the `blSize == 0 || !inPayload` is SPLIT with **emptiness tested
  first**, because an empty region trivially satisfies containment and a
  containment-first order would make `ERR_BL_EMPTY` unreachable — the same
  dead-arm shape reintroduced one level down, and a blob that is BOTH empty and
  outside pins the order; and the stage mapping is an exhaustive `switch` with
  **no `default:`**, so a ninth verdict fails to compile rather than silently
  landing on MALFORMED (handoff20 §1's `igModeVerdictName()` lesson at the
  mapping rather than the name function). The two re-tests below the switch are
  DELETED rather than kept as defence in depth, because a guard restating a
  guarantee is an unreachable guard and that is this session's own subject.
  `test_riscv_ucode` 188 → **211**. **§4a is the part to be suspicious of and it
  says so**: four `checkResult()` calls in `test_gsp_stage.cpp` changed their
  EXPECTED value with the count unmoved at 181, which is exactly what somebody
  covering a regression would do — the distinguishing evidence is that three of
  the eight verdict codes still map to MALFORMED and `:590`/`:598` still assert
  it, so a broken mapping would have moved those too. §5a is a reading that
  grazed its own edge: 211 lands EXACTLY on the previous ceiling
  (188 + max(8, 23) = 211) and the check is `>`, so it did not fire by one check.
  §5b is the Python gate red once on my own staleness, attributed by the failure
  MESSAGE naming the two headers I had edited. Both headers changed, so
  `build-payload/` was rebuilt — **THIRTY SECOND** image, both sizes moved again.
  §7 item 2 is the honest limit: no verdict reaches the IORegistry, so this is
  handoff31 §1's class made one level better (eight words instead of one) rather
  than fixed. §8 item 3 is the highest-value item not blocked on hardware —
  `decodeBoot0()`'s four log-only diagnoses, the half handoff31 did not sweep;
  item 4 is the cheap one, asserting the real booters against the verdict rather
  than the bool.
- `3050-handoff31.md` — the state handoff32 continues from, and the direct
  continuation of `3050-handoff30.md`. **Its §7 item 3 is WRONG about the
  validator having no caller — see handoff32 §1.** It closes that file's §6 item 3 —
  `readIdentity()`'s decode-and-publish path, the last of the four items
  `handoff23.md` §5 item 2 named — and the closing found a **live defect in
  shipping code**. `phase0-kext/NV3050Probe.cpp` CHANGED, so `build-payload/`
  was rebuilt and `OPERATOR-ACTION-step7-build.md` updated — the THIRTY FIRST
  image, and the first in five where **both byte sizes moved** (tar 358400 →
  368640, ISO 436224 → 446464, exactly one 10240-byte tar block each), which
  §5 is careful to call a coincidence of padding rather than a signal.
  **§1 is the one to read**, and it is the sixth occurrence of handoff18 §1d's
  class and the fifth in that file — but the sharpest, because the driver
  **already knew**. `decodeBoot42()` has always opened with an arm for "BOOT_0
  answered but `NV_PMC_BOOT_42` came back all-ones or all-zeroes": it logs
  *unexpected dead read ... aperture may be shorter than 16 MiB* and returns
  without decoding a single field. And then `readIdentity()` returned **true**
  anyway, `start()` saw true, and `publishIdentity()` published the word
  unconditionally — so the diagnosis reached the LOG and the IORegistry got
  `NV_PMC_BOOT_42 = 0xFFFFFFFF`, byte-identical to a chip that genuinely reports
  all-ones. The previous five occurrences were all *unknowing* (a value nobody
  read, a refusal nobody consumed, an initialiser zero); this one is a
  diagnosis the tree already contained, with the predicate and the prose
  explaining why it matters both sitting in the same file. §1a is why the four
  gates were blind and its second entry is the durable one:
  `nvLooksLikeDeadRead()` has direct coverage in `tests/test_decode.cpp`, so
  this is a **heavily tested predicate with a caller that ignores it** —
  `intel-handoff26.md` §1's tested-function-with-no-callers, one track over and
  one level down. The fourth entry matters too: `tests/test_probe_identity_publication.py`
  existed and was GREEN, because all four of its properties were about BOOT_42
  on the **unreadable** path and nothing asked what the readable path does with
  a dead BOOT_42. **§2 is the choice worth arguing about**: `readIdentity()`
  still returns `true`, deliberately. The obvious fix — return false and let
  `start()` route to `publishIdentityUnreadable()` — is wrong in the direction
  this project cares about, because BOOT_0 **decoded**, so `NVArchitecture`,
  `NVImplementation` and the PCI config words are real measurements and routing
  away would replace one fabricated word with four discarded true ones (and
  `publishIdentityUnreadable()`'s own reason string says BOOT_42 "was never
  read", which would become a lie). **The split belongs at publication, not at
  the read** — a genuinely different shape from `3050-handoff26.md`'s WPR2
  verdict, where the two halves compose ONE reported range and a partial answer
  is meaningless; here the two registers are independent measurements that
  happen to be read together. The guard is `!nvLooksLikeDeadRead(boot42)` rather
  than a hand-rolled equality against all-ones, because all-zeroes means the
  mapping succeeded and points at nothing — the WRONG BAR, the more dangerous
  half. `NVBoot42Readable` is published FIRST and on BOTH arms, the fourth
  family after `GSPEngineReadable`, `NVIdentityReadable` and
  `NVGspBootReadMask`. §3 is the gate **extended rather than added** — property
  5 plus four negative controls, 11 → **15** tests — and the reason to extend is
  that a second module over `publishIdentity()` would duplicate the `audit()`
  every module in this family deliberately shares with the live tree. The
  guarded span is found by a balanced brace walk, never a class excluding the
  statement terminator (`handoff25.md` §1c). §3a is the one place
  handoff18 §1e's rule DOES apply, unlike the last two sessions: the four
  controls ran against eleven pre-existing checks and each reddened only its own
  property with every pre-existing check green. Python 970 → **974**, the +4
  attributed by running the module alone (15) BEFORE the gate. Register gate
  unmoved at 240 (`NVBoot42Readable` is a property KEY, not a `#define`) and
  every C++ count unmoved, so `tests/test_check_count_floor.py` needed no edit.
  §7 item 3 is the new highest-value item not blocked on hardware —
  `nvRiscvUcodeIsWellFormed()` is a bare `bool` with six distinct refusal
  reasons behind it and **nothing in the IOKit shell calls it at all**. §7 item
  4 is the sweep this session named but did not do: `decodeBoot0()` detects a
  non-Ampere architecture, an Ada part specifically, and a non-GA107
  implementation, and all of those findings go to `NVLOG` and none reaches the
  IORegistry — the same class as §1, three more times.
- `3050-handoff30.md` — the state handoff31 continues from, and the direct
  continuation of `3050-handoff29.md`. It does exactly one thing: it closes that
  file's §9 item 3, which was the hole handoff29's own fix left behind.
  `phase0-kext/` is **BYTE-IDENTICAL**, so `build-payload/` was NOT rebuilt and
  the thirtieth image is still current — the rule that decides a rebuild is
  CONTENT, and the payload gate's silence is the proof. **§1 is the one to read**
  and it is the general shape of this whole project's blind spot:
  `publishGspBootResult()` lives in the IOKit shell, so no host suite compiles
  the translation unit, and **deleting all five `nvGspBootWasRead()` guards left
  all four gates green**. The C++ suite's 341 checks prove `readMask` is
  CORRECT; nothing whatsoever proved the publisher CONSUMES it, and clang has
  nothing to say about a deleted `if` around a well-formed `setProperty()`. The
  answer is `tests/test_probe_gsp_boot_publication.py`, **16 tests**, in the
  idiom of `tests/test_probe_identity_publication.py` — one `audit()` shared with
  the live tree, comment-stripped source, one positive control and twelve
  negative controls each asserting WHICH property broke. **§2's property 5 is the
  choice worth arguing about**: the guarded bit set is DERIVED from the
  `NVGSP_READ_*` enum in `phase0-kext/NVGspBoot.hpp` and compared for EQUALITY,
  not spelled in the test — so a sixth bit added to the header with no
  publication reddens on the commit that adds it, naming the bit. That is
  `intel-handoff26.md` §2a's derive-the-subject-set reasoning reached for a
  different family; the triple table still fixes the PAIRING of bit to field to
  key, because set equality says nothing about which word each bit guards. §2a is
  why comment stripping is mandatory rather than stylistic here — the fix's own
  comment block names every key and every field it publishes, so a prose-reading
  gate would count `NVGspBootPrivErrStat` as published on a publisher whose
  guards were all deleted, and a test asserts the stripper actually removes it.
  §2b is the pair of tests that stop the module passing by measuring NOTHING: a
  renamed subject and an enum-less header each produce a REFUSAL string rather
  than silence, because a gate reporting nothing when its anchor moves is
  indistinguishable from a green one. §2c is `handoff25.md` §1c avoided on
  purpose — the guard pattern is structural with captures at every compared
  point, never a class excluding only the statement terminator, and the mask's
  unconditional-ness is decided by walking back to the previous statement
  boundary rather than by a regex over the line. **§3 is honest that
  handoff18 §1e's every-pre-existing-check-green rule does NOT apply to an
  all-new module** and says what the twelve controls establish instead; its
  sharpest control is the duplicated-bit copy-paste shape, which reddens THREE
  independent properties at once (wrong pairing, duplicate flag, and set
  equality) — what a derived predicate buys. Python 954 → **970**, the +16
  attributed by running the module alone BEFORE the gate. Register gate unmoved
  at 240 and `test_gsp_boot` unmoved at 341, both correctly, so
  `tests/test_check_count_floor.py` needed no edit. §5 item 3 is the hole this
  gate leaves and it is the LOUD direction rather than the vacuous one: the
  predicate is spelled against `nvGspBootWasRead(` by name, so a second guard
  helper added later yields publications reported as BARE rather than ignored.
  §6 item 3 is the new highest-value item not blocked on hardware —
  `readIdentity()`'s decode-and-publish path, the last of the four
  `handoff23.md` §5 item 2 named.
- `3050-handoff29.md` — the state handoff30 continues from, and the direct
  continuation of `3050-handoff28.md`. Two things happened. **§1 is the cheap
  one and worth reading first**: handoff28's own §8 item 3 — "parse
  `vbios-ga107.rom`, the last file in `reference/ga107-firmware/` nothing in this
  tree has ever read" — is **FALSE, and was false when it was written**.
  `tools/parse-vbios.py` decodes the PCI ROM image chain, the BIT table, the
  string table and the falcon ucode table pointer; `vbios-report.txt` is its
  committed output and REGENERATES byte-identically (`diffexit=0`);
  `tests/test_parse_vbios.py` runs 53 tests with **zero skips** and its
  `RealGa107RomTests` class drives the actual 65024-byte file. Two greps and one
  diff cost four minutes. An open list inherited across handoffs is a claim with
  an age, and "nothing has ever read X" is a set-difference somebody has to
  actually run. handoff28 was NOT edited to remove the item — same rule as its
  own §7 item 5. **§2 is the real subject and it is a LIVE defect in shipping
  code**, the FIFTH occurrence of handoff18 §1d's class and the fourth in
  `NV3050Probe.cpp`. `publishGspBootResult()` published five diagnostic register
  words in five bare `setProperty()` calls, while `nvGspBootInit()`
  zero-initialises all five and each is assigned ONLY inside the state that reads
  it — `cpuctl` always, `hwcfg2` only if the core is halted, `riscvCpuctl` only in
  ACTIVE_WAIT, and the error pair only inside `nvGspBootCheckRiscvErrors()`. A run
  stopping at PROBE put three initialiser zeros into the IORegistry, and a literal
  zero is byte-identical to a register that genuinely read all-clear. **§2a is the
  durable half**: the separating case does NOT look like a failure. `ACTIVE_WAIT`
  stops on `NVGSP_RESULT_RUNNING` at `:805` BEFORE the error check at `:808`, and
  RUNNING is the SUCCESS terminal — so the healthy boot is exactly the path that
  fabricated two of the five words, and it is the path an operator is most likely
  to be reading when they decide to trust the registry. A defect confined to the
  failure path is annoying; one confined to the success path is a trap. §3's fix
  is a `readMask` plus `nvGspBootWasRead()`, and its four choices are each
  load-bearing: an **enum rather than `#define`s**, because
  `tools/check-registers.py` parses this header's `#define`s against NVIDIA's and
  these bits are the driver's own bookkeeping rather than hardware (the register
  gate is correctly unmoved at **240**); the bit is set **after** the read returns
  true, never before, since a mask set first would report a word as measured on
  the one path where nothing was; the mask is published on EVERY path and FIRST,
  verbatim `GSPEngineReadable`'s reasoning; and `nvGspBootWasRead(0, bit)` is
  FALSE. §4's four runs separate every boundary — A (healthy, `0x07`, error pair
  NOT read) and B (`ERR_ACTIVE_TIMEOUT`, `0x1F`) together make the mask a
  measurement rather than a constant, while C (`0x01`) and D (`0x03`) split the
  two falcon bits apart. **§4a is a reading error worth carrying**: run B first
  returned `ERR_STEP_BUDGET`, which is the one result code always about the
  FIXTURE rather than the simulated hardware — the DMA path plus the default
  200-poll budget trips the 1000-step backstop first, and the existing idiom is
  `skipFirmwareLoad` plus a small `maxActivePolls`. §5's mutation is the
  copy-paste shape C and D exist to catch and reddened exactly **2 checks, both
  new, every pre-existing check green**, first try. `test_gsp_boot` 313 → **341**,
  inside its band by eleven so the ceiling did not fire. Shipping code changed, so
  `build-payload/` was rebuilt — **thirtieth image**, and NEITHER size moved while
  both md5s did, the padding case for the fourth image running. §7 is the other
  gate lesson: the Python gate went red on `test_operator_step7_doc` and unlike
  `intel-handoff27.md` §5a the correct action was to FIX it, because the staleness
  was my own from sixty seconds earlier — the rule is attribute by mtime and by
  the failure MESSAGE, not "never touch a red gate". §9 item 3 is the hole this
  session's own fix leaves and the highest-value item not blocked on hardware:
  delete the five `nvGspBootWasRead()` guards and all four gates stay green, so
  the publisher needs a shape gate in the pattern of
  `tests/test_probe_wpr2_publication.py`.
- `3050-handoff28.md` — the state handoff29 continues from, and the direct continuation of
  `3050-handoff27.md`: it closes that file's §8 item 3, the item named there as
  the highest-value one not blocked on hardware. **Its §8 item 3 is superseded —
  see handoff29 §1; `vbios-ga107.rom` was already parsed and tested.** It found no defect in shipping
  code — `phase0-kext/` is BYTE-IDENTICAL, so `build-payload/` was NOT rebuilt
  and the twenty ninth image is still current, which §6 argues is the correct
  call: the rule that decides a rebuild is CONTENT and the Python gate is the
  thing that reads content, so a rebuild here would have produced a thirtieth
  image differing only in tar timestamps and vouching for no change. What moved
  is the FIXTURE. `tests/test_riscv_ucode.cpp` gained `testRealFirmwareFiles()`,
  **144 → 188** checks, and the suite is now explicitly two halves: everything
  through `testDescriptorVersion()` stays synthetic, and the new section opens
  `reference/ga107-firmware/*.bin` directly and SKIPS with a banner and zero
  checks when the directory is absent. A reading of 144 on a bare checkout is
  therefore CORRECT rather than a regression, and the banner is the record of
  which reading you are looking at. **§2's one load-bearing choice** is that the
  path is injected by CMake as an absolute `NV_FIRMWARE_DIR` rather than taken
  relative to the binary's cwd — a wrong path SKIPS rather than fails, which is
  the vacuity direction and the worst failure mode available to a section whose
  whole purpose is reading files. **§3 is the one to read**: a claim recorded in
  `handoff18.md` §1c is WRONG. The real bootloader does NOT leave all six
  optional regions zero — manifest size is `0x800`, monitor data `0x800`/`0x1000`,
  monitor code `0x1800`/`0x2900`, and `bIsMonitorEnabled` is 1; only the ELF row
  and the two swbrom rows are genuinely zero. `buildBlob()` reproduced the belief
  rather than the firmware, so the six-row loop handoff18 generalised was
  exercising a blob strictly SIMPLER than the one that ships. Not a bug in
  `NVRiscvUcode.hpp` — the predicate accepted the real file before and after —
  but a bug in what the tree believed about its own evidence, and the same class
  as the booter finding one level down. All six rows plus the flag are now pinned
  against the file. §4 is the non-vacuity proof and its FIRST mutation is the
  lesson: changing `NV_RISCV_GA107_BL_BOOTLOADER_OFFSET` reddened 4 checks with
  THREE pre-existing, which proves the old synthetic tests already covered that
  constant rather than proving the new section reaches anything; capping
  `nvRiscvDescFieldCount()` at 23 reddened exactly TWO, both new, every
  pre-existing check green — because no synthetic blob in the suite has more than
  23 slots and only a real file carries 216. §4a is a reading that LIED and it is
  `3050-handoff27.md` §4 recurring inside the session that recorded it: mutation A
  first appeared to redden nothing because a bare `ninja` failed on `cstdio` for
  want of an MSVC `INCLUDE` and left the OLD binary in place, and a no-op mutation
  reads exactly like proof that a test is vacuous. Run the gate, not the
  generator. Count band: 188 is ABOVE its own band (144 + max(8, 14) = 158) by
  thirty, so the ceiling FIRED correctly for the first time in several growths.
  Register gate unmoved at **240** and Python unmoved at **952**, both correctly —
  the section pins values, not constants, and adds no Python test. §8 item 3 is
  the new highest-value item not blocked on hardware: `vbios-ga107.rom` is the
  last file in `reference/ga107-firmware/` that nothing in this tree has ever
  parsed.
- `3050-handoff27.md` — the state handoff28 continues from, and the first recent handoff
  that did NOT come from the open list. The operator asked how to go faster and
  confirmed a fact that outranks everything else in this file: **the laptop is
  muxless** — all graphics route through the Intel chip, HDMI being the only
  bypass. `PLAN.md` termination condition A is therefore SATISFIED rather than
  open, and combined with condition B (already measured — the Sequoia recovery
  image holds 430 kext paths and no Tiger Lake framebuffer) the consequence is
  that a complete, perfect NVIDIA framebuffer driver still puts nothing on the
  internal panel. The display path on THIS track is not unfinished, it is
  structurally dead for that screen; NVIDIA work is Phase 4/5 engineering and is
  not on the critical path to a picture. §1 also records a scheduling constraint
  that now gates the six-handoff-old top item: the laptop cannot hold Linux AND
  macOS at once, so one `kextload` and more i915 traces are MUTUALLY EXCLUSIVE
  and the choice is the operator's. **§2 is the one to read**, and its interest
  is the method rather than the bug: this is the first live defect in six
  sessions found by parsing the REAL firmware instead of by reading source.
  `nvRiscvUcodeIsWellFormed()` read the region between `hdrOffset` and
  `dataOffset` as an `RM_RISCV_UCODE_DESC` in EVERY blob, and only one of the
  three files in `reference/ga107-firmware/` carries one — both booters carry an
  `nvfw_hs_header_v2` there, same container magic, same `hdrOffset` of 24, first
  word 60 (`0x3C`) which is that header's own `header_size` and not a structure
  version at all. §2a is why "it was rejected anyway" is not a defence: the
  refusal happened three optional-region rows deep because signature bytes at
  `+0x24` happened to decode to an out-of-range size, the plausible prefix ending
  at byte 36 exactly (measured, not assumed), and the failure being guarded is
  not a parse error but a DMA of an HS signature block into IMEM as though it
  were bootloader code. **§2b item 5 is the durable lesson and it is new**: the
  C++ suite covers this file heavily — 134 checks — and could not see it either,
  because every one of them drove a blob from `buildBlob()`, which builds the
  BOOTLOADER layout. The FIXTURE was the blindness, and the suite's own header
  comment says so while describing a decision that is correct for portability.
  A suite that only ever constructs the shape it expects cannot discover that a
  second shape exists. The fix is `nvRiscvDescVersion()` plus an EQUALITY against
  version 5, read through the presence-checked accessor rather than at offset 0
  (a zero-length descriptor would otherwise return the payload's first word), with
  a fallback of all-ones deliberately NOT 5, and placed AFTER the length test so a
  short descriptor is refused for being short. §3's non-vacuity proof is the
  cleanest the tree has produced: `if (false)` reddened exactly FOUR checks, all
  four new, every pre-existing check green, first try. §4 is a build failure that
  was not mine — bare `ninja` from a shell with no MSVC `INCLUDE` fails on
  `cstdio` in a file I never touched; run the gate, not the generator.
  `test_riscv_ucode` 134 → **144** (inside its own band by three, so the ceiling
  did not fire); Python 937 → **952**, the +15 being the peer's. Shipping code
  changed, so `build-payload/` was rebuilt — twenty ninth image, and both sizes
  moved for the first time in four. §8 item 3 is the highest-value item not
  blocked on hardware: drive the suite from the real files, which would have
  found this on 2026-08-07.
- `intel-handoff26.md` — the oldest of the prefixed Intel handoffs, and independent of handoff25/26
  (a peer's NVIDIA sessions from the same day; either order). It closes
  `handoff24.md` §6 item 3, which is the same thing as closing its §5 item 4 —
  the open item and the hole are two descriptions of one gap. handoff24 taught
  both Intel publishers to judge their reads as a GROUP through
  `igSampleVerdict()` before decoding, and then left **nothing testing that they
  still call it**: delete either `sampled[]` block and its `setProperty()` and
  all four gates stay green. Same four by-construction blindnesses, re-derived
  in §1 — and the sharpest new one is that `igSampleVerdict()` is a pure header
  function with 421 checks over it, so a heavily tested function with no callers
  passes every suite it has. `tests/test_intel_publisher_sample_shape.py`, **18
  tests**, six positive controls and twelve negative controls each asserting
  WHICH property broke, all sharing one `audit()` with the live tree, all
  scanning COMMENT- AND STRING-STRIPPED source. **§2a is the one to read**: the
  subject SET is derived, not spelled — a publisher is a method that BOTH calls
  `regRead32(` and `setProperty(`. handoff24 §6 item 3's own wording ("every
  `setProperty()` in a publisher must be preceded by an `igSampleVerdict()`")
  REDDENS correct shipping code if taken literally, because `publishIdentity()`
  publishes four properties and reads ZERO registers — `fDeviceId` comes from PCI
  config space, not a BAR, so there is no refusal to consume. Third session
  running where the first draft's failing test would have been the POSITIVE
  control. Deriving the predicate also picks up a third publisher added later
  with no edit to the gate. §3 is two reds on a pristine tree, both mine: an
  assertion that the subject's comments spell `igSampleVerdict` (they discuss the
  group in words; the count is 4 either way — handoff25 §1c-ii in a new file),
  and a mutation anchor occurring TWICE because that run of variable names
  appears in both the array and the `igPipeUsable()` join it feeds. That second
  one is `a-mutation-that-matches-nothing-prints-ok` in its **over**-match
  direction, and the two look nothing alike: matching nothing prints `ok`
  silently, matching too much raises loudly and looks like the mutation is wrong
  when the precondition is working. Fixed by mutating structurally inside the
  initializer's brace span. §4 is honest that handoff18 §1e's every-pre-existing-
  check-green rule does NOT apply to an all-new module, and says what the twelve
  controls establish instead. Python gate 907 → **937**, attributed by running
  each module alone: +12 is the peer's WPR2 module, +18 is this one. Register
  gate unmoved at 240, correctly — no `#define` was added. `phase0-kext/` is
  BYTE-IDENTICAL, so `build-payload/` was not rebuilt and the TWENTY FIFTH image
  is still current. §6 item 4 is the hole this session's own gate leaves: the
  predicate is spelled against `regRead32(` by name, so a second MMIO accessor
  added later produces a method the gate does not consider a publisher at all —
  and it reports nothing rather than failing, which is the vacuity direction.
  §7 item 3 is that gap and is the highest-value item not blocked on hardware.
  **§8 is the other thing to read**: this file was written under the bare
  handoff26 name and DESTROYED the peer's NVIDIA handoff26 of the same name,
  recovered byte-for-byte
  from their transcript and renumbered. (Both files then took a track prefix, so
  neither of those bare names exists in the tree any more — that is why they are
  spelled here in words rather than as filenames.) I *did* list the directory
  first, and the
  listing was correct when it ran — so the durable rule is not handoff17 §5's.
  It is that a listing in a concurrent tree is a measurement with a TIMESTAMP,
  and the tell is the write verb: `Write` reported "updated" on a file I believed
  was new.
- `3050-handoff26.md` — the peer's NVIDIA-side session from the same day, and the direct continuation of
  handoff25: same shell, same class, one path over. (Renamed from the bare
  handoff26 name on 2026-08-18: the two tracks collided on that number twice in
  one day, so NVIDIA-track handoffs now carry a `3050-` prefix and Intel-track
  ones an `intel-` prefix. The bare numbered files below are pre-split and keep
  their names.) It closes the second half of
  handoff23 §5 item 2 and it found a **live defect in shipping code**, the FOURTH
  occurrence of handoff18 §1d's fabricated-publication class and the third in
  `NV3050Probe.cpp`. `checkWpr2()` returned a bare `bool` meaning "the firmware
  boot precondition is satisfied", and that bool was false for two categorically
  different reasons — WPR2 genuinely UP (a fact about the machine, fixed by an
  engine reset) or the registers not decoding at all (a bug in THIS driver, the
  one somebody can go and fix). §1b is the sharper half and the reason this is
  §1d's class again: `regRead32()` answers every refusal with all-ones, the
  function stored BOTH words into its out-params before any test, and it tested
  ADDR_HI **only** — ADDR_LO had no refusal test at all — while the caller
  published both words unconditionally. So a refused ADDR_LO beside a healthy
  ADDR_HI returned *true* and put the refusal value into the IORegistry as
  `GSP_WPR2_ADDR_LO`. Worse than the previous three occurrences, because the
  fabricated word did not merely mislead somebody reading the registry — it fed
  the go/no-go join, which then reported a boot precondition as met over a word
  nobody measured. The fix is a verdict family in `NVFalcon.hpp` (dependency-free,
  so host suites can drive it) and §1c is where its four load-bearing choices
  live: the verdict judges the read GROUP rather than the field, because the two
  halves compose ONE reported range — handoff24 §1a's `igSampleVerdict()`
  reasoning arrived at independently one track over; `nvWpr2ReadRefused(0u)` is
  deliberately FALSE because zero in ADDR_HI is exactly the healthy DOWN case,
  which is the OPPOSITE polarity to the Intel side and lands on the same sentinel
  by a different road; the sentinel is an equality never a threshold, since the
  top of the address space is a legal WPR2 base; and the one honest ambiguity (a
  half genuinely holding all-ones reports UNREADABLE) is written into the header
  rather than fixed, because believing it would publish a WPR2 based at the top
  of a 40-bit space as though measured. §1d is publication — verdict and readable
  flag FIRST and on BOTH arms, address words only behind the readable arm, and
  `wpr2Down` an equality against DOWN rather than a negation of UP, because a
  negation is TRUE for UNREADABLE and would send an unmeasurable precondition
  through the go/no-go as satisfied. **§3 is the one to read**: my own gate went
  red on correct code for a new reason — the audit's message said "never returns
  NVWPR2_UNREADABLE" while the check was implemented as mere PRESENCE of the
  token, and gutting the return leaves that token in the comparison three lines
  above it. An assertion whose message and whose behaviour disagree, which is this
  module's own subject one level up; same family as handoff21 §1b and handoff23
  §1b, except those compared against a value the subject can never produce and
  this one compared against a value the subject produces on BOTH sides of the
  mutation. `test_falcon` 169 → **193** (above its own ceiling by eight, so the
  band fired on its own for the first time in four growths); Python 907 → **937**.
  Shipping code changed, so `build-payload/` was rebuilt twice — twenty seventh
  and twenty eighth images — and the twenty eighth is the sharpest case the tree
  has produced for why a byte size is not a fingerprint: a COMMENT-ONLY change,
  which compiles identically and ships differently, with three consecutive images
  now holding both sizes fixed while all six md5s differ.
- `handoff25.md` — the state handoff26 continues from, NVIDIA side, and the mirror of handoff24 on
  the other track: the same class of defect, found the same day, one level over.
  `NV_PMC_BOOT_42` was published into the IORegistry on a path that **never read
  it**. Three sites, none a defect alone: `readIdentity()`'s dead-read branch did
  `*outBoot42 = boot0;` before `return false;`, `start()` DISCARDED that bool, and
  `publishIdentity()` spelled `setProperty("NV_PMC_BOOT_42", …)` unconditionally.
  Join them and an operator sees two equal all-ones words and cannot tell a dead
  aperture from a card that answered — `setProperty()` with a fabricated value is
  byte-identical to `setProperty()` with a measured one, which is handoff18 §1d's
  sentence for the third time and the second time in this same shell. Worse, the
  live publisher derives `NVArchitecture` from `nvBoot0Architecture(boot0)`, so a
  machine whose BAR never decoded could publish an architecture NAME. §1a is the
  same four by-construction blindnesses handoff20/22/23 each list, with one new
  member worth carrying: `-Werror` has nothing to say, because `*outBoot42 =
  boot0;` is a well-typed store and a discarded `bool` return is not a warning.
  The fix is three edits and each is load-bearing alone — a partial fix leaves the
  defect live by another road — plus a new `publishIdentityUnreadable()` that
  publishes `NV_PMC_BOOT_0`, `NVIdentityReadable false`, a reason string, the
  expected chip ID and the PCI CONFIG words (kept deliberately: config space is
  not BAR0, so those are still real measurements), and publishes no BOOT_42 and
  derives no name. `NVIdentityReadable` goes on BOTH arms, same reasoning as
  `GSPEngineReadable`. **§1c is the one to read**: my first gate went RED on the
  fixed tree and the failing test was the positive control, for a new reason —
  a regex spelling the call name, then a negated character class excluding only
  the semicolon, then a closing paren and a semicolon, matched the CORRECT call
  site — because a class that excludes the terminator crossed the `if`'s closing
  paren, the newline and the whole guarded body to land on the next statement's
  semicolon. (Written in words rather than as the pattern itself on purpose:
  `tools/check-documented-paths.py` reads a backslash-and-slash token as a path
  and reddens two gates over it, the same trap the SDK-include note above
  describes.) **A character class that excludes only the terminator is not
  scoped to the statement**; bareness is now decided by a balanced-paren walk. §1c-ii is a second red on the same pristine tree and the
  duller cousin of it: an assertion about comment wording I remembered writing
  rather than wording on disk. `tests/test_probe_identity_publication.py`, 11
  tests, one positive control and nine negative controls each asserting WHICH
  property broke, all sharing one `audit()` with the live tree and all scanning
  COMMENT-STRIPPED source — mandatory here, because the fix's own comment
  discusses `outBoot42` by name and a prose-reading gate would find the identifier
  it exists to forbid. 896 → **907** Python tests. Unlike the last four handoffs
  this one CHANGED shipping code, so `build-payload/` was rebuilt (twenty sixth
  image, and the first in three to move both byte SIZES) and
  `OPERATOR-ACTION-step7-build.md` updated — both this session's own staleness,
  which is why rebuilding was correct here and was not in handoff23 §3. The
  header note is the other thing to read: this file was written as `handoff24.md`
  and **destroyed the peer's Intel handoff24 of the same name**, recovered
  byte-for-byte from their transcript and renumbered. handoff17 §5 in a
  concurrent tree rather than after a compaction — list a numbered directory
  before writing into it.
- `handoff24.md` — the Intel-side session from the same day, written by a peer and
  independent of handoff25; either order. It is the **second half of a fix**
  rather than a new subject: it closes `handoff22.md` §1b, and what it closes is a
  live defect that handoff22's own fix created. That session taught `regRead32()`
  to REFUSE a read past the end of the BAR0 mapping; **nothing consumed the
  refusal**. `IG_MMIO_INVALID` occurred at exactly ONE place in the whole of
  `IntelDisplayProbe.cpp` — inside `regRead32()`, where it is produced — so both
  publishers handed it straight into `igModeConsistent()`, `igPipeUsable()` and
  `igFbInheritable()`, and `setProperty()` with a decode of a refused read is
  BYTE-IDENTICAL to `setProperty()` with a decode of a register that genuinely read
  that value. The IORegistry could not distinguish "transcoder B is unprogrammed"
  from "transcoder B was never read", and only the second is a fact about this
  driver somebody could go and fix. Same class as handoff18 §1d one level down; the
  four gates were blind for the four by-construction reasons handoff20 §1 lists,
  re-derived in §1. **§1a is the one to read**, and the load-bearing choice is that
  `igSampleVerdict()` judges a read GROUP rather than a field: the decoders are
  joins — `igPipeUsable()` eats nine words and returns one verdict — so a per-field
  flag would publish eight good values, one sentinel, and a verdict arithmetically
  correct over an input that was never measured. `igReadRefused(0u)` is FALSE on
  evidence rather than taste (every unprogrammed page read `0x00000000` in the
  2026-08-16 capture, handoff15 §2 — all-ones is what a block that is not decoding
  reports); a null pointer or zero count answers REFUSED, the same
  refuse-rather-than-trust direction as zeroing `fBar0Length` on unmap; and the one
  honest ambiguity (a register genuinely reading `0xFFFFFFFF` publishes `"refused"`)
  is written into the header rather than fixed, because threading the decision
  through would couple the pure header to the shell's mapping state. `IG_SAMPLE_*`
  lives in `IntelDisplay.hpp` and only the NAME in `IntelDisplayNames.hpp`, because
  the register gate parses two Intel headers and a family in the third gets no
  set-check. §1b is publication: both paths and FIRST, so the verdict sorts above
  every value it qualifies. §2 is the non-vacuity proof — `values[0]`-only reddened
  **13 checks, all new, every pre-existing check green** — and it is also the
  argument for walking the refusal across every index of BOTH the 4-word and
  10-word groups, since a bound written for the plane group passes every four-word
  check. §3 warns that the syntax gate's section 11 is TITLED "each defect must
  FAIL", so a bare `grep FAIL` returns a hit on a passing run — read the exit code.
  Register gate 239 → **240** (a new SET, unlike `IG_READ_` growing 4 → 5);
  `test_intel_display` 391 → **421**, `test_intel_display_names` 236 → **245**, both
  landing INSIDE their own bands. §5 is what this does not establish and its item 4
  is the hole this session's own fix leaves: deleting a publisher's `sampled[]`
  block restores the defect with all four gates green. §6 item 1 is unchanged and
  still the highest-value action in the project: one `kextload` on a machine
  containing the card.
- `handoff23.md` — the NVIDIA-side session from the same day, written by a peer and
  independent of handoff24; either order. It is the mirror of handoff22 one
  level up: it asks handoff22's question of `NV3050Probe.cpp`'s register accessors
  and gets the opposite answer. `regRead32()` and `regWrite32()` are the funnel
  every MMIO access in the NVIDIA track passes through — two refusals in front of
  the read, three in front of the write — and **nothing observed any of them**.
  Delete the bounds guard and all four gates stay green, for the same four reasons
  handoff20 §1 and handoff22 §1 each spell out. Unlike those two this one found no
  live defect: the accessors are correct as written, so §1a is a reason for them to
  STAY correct rather than a fix. `tests/test_probe_register_access_refusals.py`,
  11 tests, one positive control and ten negative controls, each asserting WHICH
  property broke. It pins four things: both accessors refuse in order before the
  single dereference (the bounds condition is CAPTURED, not merely matched, so the
  `sizeof(UInt32)` term inside it is inspectable — handoff22 §1b's rule arrived at
  independently); the two guarded accessors are the only MMIO path in the whole
  file; a stock build physically cannot open the write gate, checked arm-by-arm
  because flipping `#else` to `true` leaves the assignment COUNT at two; and
  `mapBar0()` WARNS about a short BAR rather than refusing, which is deliberate for
  handoff22 §1's reason. Scanning is over COMMENT-STRIPPED source, because
  `regWrite32()`'s own comment says "Three refusals stand in front of the store"
  and a gate reading prose would pass on a function whose guards were all deleted.
  **§1b is the one to read**: my first version went RED on correct shipping code,
  and the failing test was the positive control. The sentinel was read out of the
  subject's own body — correctly, avoiding handoff21 §1b's mistake — and then
  compared with `value.upper() != "0xFFFFFFFF"`, which is false for EVERY input
  because `.upper()` uppercases the `x` in the prefix too. Same root as
  handoff21 §1b in the loud direction: the comparison was against a *rendering* of
  the value rather than the value. Hold hex sentinels as `int` and parse found
  literals with `int(token, 16)`. §2's ten mutations include the one worth reading
  — hoisting the read above both guards takes TWO edits, because a one-edit version
  leaves two stores and reddens the COUNT check, which proves nothing about
  ordering. §3 is the gate table and why three of its four numbers are joint
  measurements with the Intel agent: 240 registers not 239, a syntax gate that
  prints `FAIL` on its own negative-control header and passes, and a Python gate
  that went red twice with a DIFFERENT peer-attributed failure each time. Neither
  was fixed, on purpose. 885 → **896** Python tests; `phase0-kext/` byte-identical.
  §5 item 1 is unchanged and still the highest-value action in the project.
- `handoff22.md` — the Intel-side state handoff23 continues from, and the only
  recent handoff whose finding is
  a **live defect in shipping code** rather than a coverage hole. `mapBar0()` logs
  a WARNING when BAR0 is shorter than `IG_BAR0_EXPECTED_LENGTH` (`0x80000`) and
  attaches anyway — deliberately, because declining would delete the IORegistry
  node that makes a short BAR inspectable. §1 is the one to read: that warning was
  the ONLY consequence there was. `regRead32()` bounded its dereference against
  `IG_DISPLAY_POLICY_LIMIT` (`0x7BFFF`), and `IntelDisplay.hpp:164` is explicit
  that the pair bounds THE ADDRESSES THIS HEADER CAN PRODUCE — it says nothing
  about what the hardware exposes and nothing about what the OS *mapped*. Three
  different numbers (largest emittable offset, the length the block NEEDS, and
  `getLength()` which was read, logged and discarded) and the code joined none of
  them, so on a `0x40000` BAR transcoder A at `0x60000` was read `0x20000` bytes
  past the end of the mapping — not a fault, just whatever the next mapping holds,
  decoded as `TRANS_HTOTAL` and published as a mode. Why no gate could see it: the
  register gate checks constants and both constants are CORRECT, their relation to
  a runtime length not being a constant; the C++ suites could not pin an input the
  function did not accept, because `igReadDecision()` took `(bool, IGU32)` and
  there was no parameter to get wrong; and `-Werror` has no enum to bite on
  because the verdicts are `#define`d — verbatim handoff20 §1's reason. §1b is what
  changed: a fourth parameter, `IG_READ_PAST_MAPPING`, and three load-bearing
  choices. The length is a new `IGU64` typedef rather than `IGU32` because
  `IOByteCount` is 64 bits and the truncation is ASYMMETRIC — `0x1_0000_0000` → `0`
  refuses everything, which is wrong and loud, while `0x1_0007_0000` → `0x70000`
  ACCEPTS reads it should refuse, and that direction scribbles. The test is
  `offset + IG_MMIO_ACCESS_BYTES <= mappedLength`, never `offset < mappedLength`,
  because an offset at `length - 1` is inside the mapping by any naive test and
  still reads three bytes past the end of it. And the new guard runs **LAST**,
  which is the reverse of the intuitive order: an out-of-policy offset is a bug in
  this driver while a short BAR is a fact about the machine, so an offset that is
  both is reported as the bug — the one somebody can actually go and fix. On the
  shell side `fBar0Length` is zeroed in `init()` and in `unmapBar0()`, so a stale
  bound refuses rather than trusts. **§2 is the one to read after §1**, because the
  non-vacuity proof found a second defect in my own test: planting
  `offset >= mappedLength` reddened exactly ONE check and it was a new one, every
  pre-existing check green — the signal handoff18 §1e argues for — but one failure
  revealed the block pinned one POINT of the overhang window rather than the
  window. `offset >= length` fails only the +3 case, `offset + 2 > length` only +1
  and +2, `offset + 3 > length` only +1, so a PARTIAL fix would have stayed green;
  two more checks pin the remaining bytes. 373 → 389 → **391**. §3 is what this
  does not establish, and its first item is that the `case IG_READ_PAST_MAPPING`
  arm, its `IGLOG`, and the `fBar0Length` assignment are still reachable only by
  parse. §4 item 1 is unchanged and still the highest-value action in the project:
  one `kextload` on a machine containing the card.
- `handoff21.md` — the NVIDIA-side state from the same day, and the deliberate mirror of handoff20: it
  asks handoff20's question of the NVIDIA side and gets the same answer by a
  different road. `NVDecode.hpp` has exactly two string-producing functions,
  `nvArchitectureName()` and `nvAmpereImplementationName()`, and
  `tests/test_decode.cpp` called each of them **exactly once** — both times with
  the target part — so six of seven architecture arms and five of six
  implementation arms had never EXECUTED while 64 checks printed green over them.
  Unlike the Intel case there was no missing arm to find: set-difference was run
  first and both switches are complete (7 defines against 7 arms, 6 against 6),
  so §1 adds no fix, it adds the thing whose absence let the Intel defect through
  — a reason for the completeness to stay true. Three properties: every arm
  returns its OWN string (per-arm plus pairwise-distinct, because a copy-paste arm
  returning a sibling's name satisfies every "has a name" assertion, and
  `GA106`/`GA107` are one character apart and adjacent in the switch); every value
  in each family is named, so a DELETED arm reddens; and the value one past each
  family answers the sentinel, which is the tripwire that forces whoever adds an
  eighth architecture to come back and extend the first two. The implementation
  side is deliberately NOT a range walk — `0x01` and `0x05` are genuine holes in
  NVIDIA's numbering and are pinned to the sentinel so nobody "simplifies" the
  list and gets a failure that is right about its arithmetic and wrong about the
  world. **§1b is the one to read**, because my first version went RED on correct
  shipping code and the fifth occurrence of the mistake was not cosmetic: the two
  switches do NOT share a sentinel (`"unknown"` at `:278`, `"unrecognised Ampere
  part"` at `:291`), and property 2's implementation-side coverage loop compared
  against a string that switch can never return — so it counted six named arms
  unconditionally, including for an arm deleted from the switch. A comparison
  against a value the subject cannot produce is not a test, it is an `ok` line,
  and the new road into that family is guessing a sentinel from a sibling function
  five lines away. Both sentinels are now named constants, each asserted against
  what the header actually returns and asserted distinct from each other. §2 is
  the non-vacuity proof and needed only one mutation because the class is narrow:
  the `GB200` arm returning `"Blackwell"` reddens 2 checks, both new, every
  pre-existing check green — the value is consumed by nothing, so `-Werror` has no
  enum to bite on, verbatim the reason handoff20 gives for the Intel defect
  surviving four gates. §3 records two reading errors worth repeating: the
  register gate now prints **239** not 238 because of the peer's Intel work, and
  my first Python-gate reading printed `0` that was `tail`'s status rather than
  the gate's — in a session whose whole subject is assertions that cannot fail.
  `test_decode` 64 → 96; `phase0-kext/` byte-identical, proven by the payload
  gate's silence rather than asserted. §6 item 1 is unchanged and still the
  highest-value action in the project: one `kextload` on a machine containing the
  card.
- `handoff20.md` — the state handoff21 continues from, and the shortest of the
  recent ones because
  it does exactly one thing: it closes handoff19 §8 item 1 and the closing found
  a live defect in the IORegistry output. The Intel shell's pure functions — four
  verdict-name switches, `igTransName()`, and the property-key composition — were
  file statics inside `phase0-kext/IntelDisplayProbe.cpp`, i.e. inside a
  translation unit no host suite can compile, so nothing had ever **executed**
  one. They now live in `phase0-kext/IntelDisplayNames.hpp` and are driven by
  `tests/test_intel_display_names.cpp` (236 checks), which made eight C++ suites
  nine. §1 is the one to read: `igModeVerdictName()` had arms for verdicts 0
  through 5 while `IG_MODE_VERDICT_LIMIT` is 6, so `IG_MODE_SRC_MISMATCH`
  published the unknown string — and that is the worst verdict in the family to
  lose, because it is the one that names the byte-swapped `PLANE_SIZE`/`PIPESRC`
  shape handoff15 measured on silicon, a mistake that programs cleanly and yields
  a plausible wrong picture. §1 also spells out why each of the four gates was
  blind to it *by construction* — the register gate checks constants, the C++
  suites cannot compile a file that reaches IOKit, `-Werror` has no enum to be
  exhaustive over because the verdicts are `#define`d, and nothing in the Python
  gate models "every verdict below the family's own limit has a name". The
  exhaustiveness loops bound on each family's own `_VERDICT_LIMIT` rather than a
  typed count, so a family that grows reddens on the commit that grows it. §1b is
  the `snprintf` → `igComposeKey()` swap, and the reason is truncation rather
  than availability: `snprintf` reports a short buffer only in a return value
  nobody read, so one tight byte publishes `IGTranscoderDSI0-Inher`, a key that
  looks deliberate and sorts next to the real one. §1c is the same move one level
  down and the sharper of the two: `regRead32()`'s three guards ALL return
  `IG_MMIO_INVALID`, and `IG_MMIO_INVALID` is `0xFFFFFFFF` which is itself outside
  the policy range — so deleting the sentinel guard changes nothing the function
  RETURNS and only corrupts the log line, which then blames the policy range for
  an offset helper's refusal. `igReadDecision()` in `IntelDisplay.hpp` returns a
  REASON rather than a bool, which is what makes the precedence assertable at all;
  the family went into that header rather than `IntelDisplayNames.hpp` because
  `tools/check-registers.py` parses only two Intel headers and a family in the
  third would get no set-check. §1d is a peer editing `test_decode` underneath the
  session — 64 → 93 → 96 checks while `NVDecode.hpp` moved too — and the correct
  action on the resulting red count-floor gate was to touch nothing and let the
  peer's own table entry land. §3 is two gates going red on a
  *prose* edit — an angle-bracket SDK include spelled in this file, which
  `tools/check-documented-paths.py` cannot tell from a missing repo file — plus a
  second reading whose two named FAILs did not reproduce serially. §4 is what
  this does not establish, and §5 item 1 is that `make` still has not run.
- `handoff19.md` — the state handoff20 continues from, and the mirror image of
  handoff18: it adds
  no decode logic at all and moves the **integration** layer. The Intel track
  finally has an IOKit shell — `phase0-kext/IntelDisplayProbe.cpp` / `.hpp` —
  which closes handoff16 §8's first open item. Until it landed, the three Intel
  headers had no consumer outside `tests/`, so nothing established that a real
  `IOService` subclass could call them. §2 is the one to read, and it is a
  `tests/kextshim/` finding rather than a kext one: matching Apple's SYMBOL set
  is not matching Apple's INCLUDE GRAPH, and only the graph decides whether a
  translation unit's include list is correct. The shim declared `IODeviceMemory`
  inside `IOMemoryDescriptor.h` (Apple ships it in its own header) and had no
  `libkern/libkern.h` at all, so `snprintf` was undeclared. `NV3050Probe.cpp`
  never noticed either for months because it reaches `IODeviceMemory`
  transitively through `IOPCIDevice.h` and calls no `snprintf`. The dangerous
  direction is the invisible one: a shim FLATTER than Apple's accepts a `.cpp`
  that OMITS an include Apple requires, and that first appears under Apple clang
  on the operator's one build attempt — the same shape as handoff8-era
  `IODMACommand::mappingOptions` being typed too loosely, one level up from a
  signature to a file layout. §3 is two tests that were stricter than the
  property their own comments described and went red on correct code:
  `test_check_header_coverage.py`'s fixture spelled the one shell as a literal,
  so its POSITIVE CONTROL reddened against a checker that was right, and
  `test_kext_makefile.py` pinned the COUNT of shells while its comment claimed to
  pin "no header is compiled". §7 is what this does NOT establish, and it is the
  section most likely to be misread from that file's green gate table:
  `IntelDisplayProbe` has
  never run, and **no suite executes a line of it** — the verdict-name switches,
  `igTransName()`, the property-key composition and the `regRead32()` refusal are
  reachable only by parse. §8 item 1 is that gap; item 2 is that `make` has not
  run since `SOURCES` gained a second object. **Both §7's list and §8 item 1 are
  partly superseded by handoff20 and carry banners saying so** — the switches, the
  name function and the key composition are now tested; the BAR-length warning and
  the `regRead32()` refusal still are not, because those genuinely need a mapping.
- `handoff18.md` — the state handoff19 continues from. NVIDIA-side only and deliberately so: a
  separate agent was working the Intel track concurrently, so the three Intel
  headers and their three suites were read-only for the whole session and the
  Intel check counts in its table are copied from `handoff17.md` rather than
  re-measured. §1 is the one to read:
  `NVGSP_RESULT_ERR_APERTURE_VIOLATION` is produced at two sites in
  `NVGspBoot.hpp` — the read guard and the write guard — and **no check in
  `tests/test_gsp_boot.cpp` ever reached either one**, so deleting either
  `nvIsInGspAperture()` block left 217 checks green. That is the worst possible
  place for a blind spot, because the guard exists precisely because the RISC-V
  block sits one 4 KiB page above the Falcon block, so a base constant short by a
  page yields a VALID address into the wrong engine and scribbles on it silently.
  It was found by set-difference, not by reading: fourteen `NVGSP_RESULT_*` names
  in the header against thirteen in the suite. `testApertureGuard()` now covers
  it, including the precedence pins (`NO_IO` beats aperture; aperture beats
  writes-disabled) and a negative control, and the separating assertion is
  `gFix.readCount == 0` — a guard that fired AFTER invoking the callback would
  produce the identical result code while having already performed the illegal
  access on hardware. §1b ran the same set-difference against the other enums,
  which is where the calibration lives: `NVGspStage.hpp` and `NVFalcon.hpp` came
  back CLEAN, and `NVGSP_STATE_*` reported `DMA_XFER`/`DMA_DRAIN` missing but that
  was **not** lost coverage — both are reached at runtime and both name-table arms
  are covered, so a spelling absence is a proxy for a coverage absence rather than
  the thing itself. What it did imply is that no test observed the machine WHILE
  in those states: every DMA test inspects effects after `nvGspBootRun()` returned,
  and `testStepDriving()` sets `skipFirmwareLoad` so it never enters the DMA path.
  `testDmaStateSequence()` now steps a two-chunk image and pins the whole walk
  including the `DRAIN → XFER` loop-back edge, which is the one thing a journal
  cannot distinguish — a machine that skipped DRAIN between chunks produces an
  identical journal and lands chunk N+1's offsets mid-flight on silicon. Count
  217 to 246 to 258. §1c is a third finding, harvested from the concurrent audit
  workflow and re-derived before anything was written: `nvRiscvUcodeIsWellFormed()`
  guards each row of its six-row optional-region table with `size != 0 &&`, the
  real bootloader leaves all six sizes zero, and `buildBlob()` reproduces that —
  so five of the six rows were never reached and a row DELETED from the table was
  indistinguishable from a row present in it. Generalised to a loop with a
  zero-size positive control per row, 116 to 134, and verified by planting the
  mutation the audit named (row 6 duplicating row 2) and watching the new check
  redden. The header is correct as written and was reverted; nothing in
  `phase0-kext/` changed. §2 is the sharpest statement available of what the band
  and the provenance class each catch: 246 was ABOVE the old ceiling and would
  have fired on its own, 258 sits eighteen inside the new one and would not have.
  §1d is the only item in this handoff that changed SHIPPING code, and the only
  one not found by set-difference — enumerating `NV3050Probe.cpp`'s 37 published
  property keys led to `:699`, where the dead-read early return in
  `inspectGspEngine()` called `publishGspState(hwcfg2, 0, 0, 0, 0, 0, 0, false)`.
  HWCFG2 is the only register read before that branch; the other six were literal
  zeros written into the IORegistry, and `setProperty()` with a literal 0 is
  BYTE-IDENTICAL to a register that genuinely read zero — so an operator could not
  tell a powered-down engine from one reporting all clear, and `GSPImemBytes` was
  `nvFalconImemSizeBytes(0)`, a value DERIVED from a register nobody read.
  `publishGspUnreadable()` now publishes only the one real measurement plus
  `GSPEngineReadable false` and a reason string; the flag is published on BOTH
  paths so its absence cannot be confused with an older kext. **No existing gate
  can see this class** — the register gate checks constants, the C++ suites cannot
  link the IOKit shell, and clang is happy with a fabricated `0` — so the shape is
  pinned by `tests/test_probe_dead_read_publication.py` (18 tests, nine negative
  controls asserting the failure REASON, all sharing one `audit()` with the live
  tree). It went red on a pristine tree first: the fix's own comment quotes the
  defective call, so a structural gate over source must scan CODE, not prose.
  `phase0-kext/` changed for real, so `build-payload/` was rebuilt and
  `OPERATOR-ACTION-step7-build.md` records the TWENTIETH image. Python total
  854 → 872. §1e is the fifth and last finding, and it closes the final NVIDIA
  item on §5's own list: `BROM_PARAMS` had journal-index coverage and no state
  coverage. `testBromParamsStateSequence()` supplies it, bounding the four BROM
  writes to a SINGLE step rather than to whole-run journal indices 2..5, and
  controlling the `skipFirmwareLoad` route against the real DMA route by
  requiring both entry edges to land on `CORE_START`. 258 → 276. The part worth
  reading is the non-vacuity proof, because the first two mutations were
  worthless and looked convincing: swapping MOD_SEL to the front reddened 13
  checks and splitting the state across two entries reddened 30, but in both
  cases PRE-EXISTING journal checks were among the failures — the first because
  the old gate already caught reordering, the second because an extra entry
  shifts every downstream index. Only the third discriminates: relocating the
  MOD_SEL write into `CORE_START` keeps the same five writes, same order, same
  whole-run indices, same walk and same `modSelWriteIndex`, so every
  pre-existing check stayed green and exactly two failed, both new. A mutation
  that reddens many checks is usually evidence the OLD tests sufficed; 13 → 30 →
  2 is the signal. `NVGspBoot.hpp` was mutated and reverted three times and is
  byte-identical, proven by its absence from the payload gate's stale list.
  §1f is the sixth finding and closes the §5 item directly below that one, and
  the asymmetry it names is the one to carry forward: every WRITE in
  `NVGspBoot.hpp` is pinned to an ABSOLUTE address by `checkJournal()`, so a
  write through the wrong engine translator has always reddened instantly, while
  every READ discarded its address entirely — `nvGspBootRead()` kept the value
  and threw the address away. That is the project's most dangerous class with no
  observer at all: one page off returns a plausible value from a real register on
  the WRONG engine, and `nvIsInGspAperture()` cannot help because
  `0x110000..0x113FFF` contains both blocks. The fixture now records the SET of
  distinct read offsets (a set, not a sequence — `ACTIVE_WAIT` polls repeatedly so
  read ORDER is dominated by poll counts, while the address set is exactly what
  distinguishes the translators), recorded BEFORE the aperture check so a refused
  read is still visible as an address the machine reached for.
  `testReadTranslators()` pins all six sites, 276 → 313. Two things in it are
  worth reading before touching this suite. First, **my initial version went RED
  on correct shipping code and the TEST was wrong**: it asserted one healthy run
  touches all six sites and ends with something other than `RUNNING`, and both
  premises were false — `NVGSP_RESULT_RUNNING` is the SUCCESS terminal, and
  `ACTIVE_WAIT` stops on it at `:805` BEFORE `nvGspBootCheckRiscvErrors()` at
  `:808`, so a healthy boot cannot reach `:400`/`:404` by construction. It takes
  two runs: A is the healthy one-chunk boot (four addresses, with the error pair's
  ABSENCE asserted as the precedence claim), B releases a core that never reports
  ACTIVE and reaches five, and the union is pinned as arithmetic over both counts
  (`runA + runB - 3 == 6`) so a site added to EITHER path breaks it. Second, the
  non-vacuity proof repeats §1e's lesson in a new place: mistranslating `:400`
  reddened 5 with THREE pre-existing, mistranslating `:730` reddened 54, and only
  a STRAY read at a wrong-engine address whose value nothing consumes left every
  pre-existing check green — 5 failures, all new. That stray read is the honest
  model of the class this section adds; the two behaviour-visible mutations were
  already covered. 313 is ABOVE the old ceiling (276 + max(8, 34) = 310) by three,
  so the band would have fired on this reading on its own.
  **§3 ends all-green, and that is the least informative thing in it.** Two of
  the four gates were RED during the session, both went green underneath me, and
  nothing I did fixed either — so the section records every reading rather than
  the final one. The syntax gate exited 1 on section 3's `IntelDisplayProbe.cpp`
  line because `tests/kextshim/` had no `IOKit/IODeviceMemory.h`; the peer added
  it at 18:43 and `tests/kextshim/libkern/libkern.h` at 18:47, and section 3 now
  prints `ok` for both shells.
  The Python gate went red, green, red, green across four readings: 5 failures
  naming the peer's `IntelDisplayProbe` files, then green after they rebuilt
  `build-payload/kext-src.tar` at 18:34, then 6 failures naming their newer
  `IntelDisplayNames.hpp` (19:14) and `test_intel_display_names.cpp` (19:17),
  then green again once they added their own `MEASURED` entry at 19:24 and
  rebuilt the payload at 19:27. I deliberately did NOT rebuild `build-payload/`
  or add a `MEASURED` entry for a suite I cannot read, because both would green a
  gate by vouching for a peer's half-landed work under this session's name. Two
  durable lessons: **a gate's verdict in a two-agent tree is a JOINT measurement**
  — red does not mean you broke it, green does not mean you fixed it, so attribute
  by mtime and by the failure MESSAGE before touching anything; and
  `check-kext-syntax.bat` printed `VERDICT: PASS` and `ALL SYNTAX GATES PASSED`
  from a later checker while exiting 1 on section 3, so read the exit code and
  grep for `FAIL`, never the last verdict line. Note the C++ gate now reports
  **9** suites, not 8 — the ninth is the peer's, never built by me on purpose.
  §5 is the open list; the workflow item, the
  set-difference item, the `BROM_PARAMS` item and the read-translator item are
  all now CLOSED.
- `handoff17.md` — the state handoff18 continues from. Touched no register constant, decoder, or
  state machine; what moved is the evidence layer. §1 is the one to read: the
  check-count gate in `tests/test_check_count_floor.py` could not audit its own
  table, because the floor and ceiling are a relation between the BINARY and the
  TABLE, and a dict entry edited to match a binary satisfies that relation
  identically to a real measurement. The entry for `test_intel_display` said 354,
  the binary printed 354, every assertion in the module was green, and the
  provenance comment above the table recorded nothing newer than 319 — a number
  described as measured whose only evidence was that somebody typed it. A new
  class now reads that comment and requires a dated reading per entry, with two
  negative controls so it cannot pass by matching nothing. §2 records that
  handoff14 §3 is CLOSED and its premise was false when measured. §5 is a mistake
  I made rather than one I found: after a context compaction I called Write on
  `handoff16.md` and destroyed it, an `Edit` failure was the only thing that
  caught it, and it was recovered byte-for-byte from the session transcript —
  list a numbered directory before writing into it. §7 is the open list, and it
  is now entirely on the Intel side.
- `handoff16.md` — the state handoff17 continues from. Fourth session on the
  Intel track, and it
  added no new evidence — the 2026-08-16 capture is still the only silicon this
  track has seen. What moved is the *joining* of constants that already existed:
  four decode layers in `phase0-kext/IntelDisplay.hpp` (whole-mode consistency,
  plane/raster, transcoder/DDI, and the whole-pipe join over all three). §1 is
  the one to read: the whole-pipe verdict family was first named `IG_PIPE_`, and
  `intel_verdict_set_checks()` groups families BY PREFIX — so it swept in
  `IG_PIPE_TRANSCONF 0x08u`, a register offset that has sat at line 140 since the
  first Intel session, and printed a contiguity failure that was correct about
  its own arithmetic and wrong about the world. Renamed `IG_USABLE_`; grep the
  header for a prefix before adding a verdict family. §3 is the placement rule —
  these headers have no includes and therefore no forward declarations, so a join
  placed above any decoder it calls is a compile error. §4 explains why the test
  rasters are 1280x800 and must not be "fixed" to the captured 1920x1080. §8 is
  the honest open list, and its first item is that the Intel track still has no
  IOKit shell at all.
- `handoff15.md` — the state handoff16 continues from, and the first session in
  the tree where an Intel constant met live silicon. The operator had a Linux boot on the target
  Tiger Lake laptop for about an hour and ran exactly one script; the artifact is
  `igpu-results/tgl-capture-2026-08-16-022556.txt`, and nothing above its READING
  NOTES divider may be edited — there is no second run of that boot. §1 is the
  three claims that moved: timing registers store value − 1 on both halves,
  PIPESRC's `+1` equals the panel's advertised 1920x1080, and `PLANE_SIZE`
  follows the same convention with its halves INVERTED relative to PIPESRC
  (`0x077F0437` vs `0x0437077F` — byte-swapped, so one decoder reused for the
  other transposes the axes and still programs cleanly). §2 is the longer and
  more important list: every unused display page on this silicon reads
  `0x00000000` rather than all-ones, which made the script print the verdict word
  ABSENT for transcoders B/C/D that certainly exist, and which leaves Q1/Q2 (the
  DSI pages and the `0x800` stride) and Q3 (plane count) not merely unanswered
  but unanswerable on an eDP-only machine. Read every ABSENT in that capture as
  UNPROGRAMMED. §5 is the crew card state.
- `handoff14.md` — the state handoff15 continues from. The first handoff on a
  SECOND track: a Gen12 (Tiger Lake) Intel display layer, requested by the
  operator after being told the ordering is backwards on difficulty. **§3 is
  CLOSED and its premise no longer holds** — do not open it as an open item. It
  says `tools/check-registers.py` never reads `IntelDisplay.hpp` and that the
  gate's header list stops at the four NVIDIA ones. Measured 2026-08-17 by
  running the gate: it prints `IntelDisplay.hpp (86 defines)` and
  `IntelFramebuffer.hpp (48 defines)` in its own header list, checks them against
  the 3180 defines vendored in `reference/i915/`, and handles `IntelGpuId.hpp`
  separately because those are PCI IDs living inside a variadic macro body rather
  than plain `#define`s. Its verdict line reads 238 of 238 passed, zero failed,
  zero skipped. So the
  Intel constants ARE covered by the register gate, and its clean run now says
  something about both tracks. §2 records the plane layer and
  why plane addressing is the exact INVERSE of transcoder addressing (planes are
  `_PICK_EVEN` arithmetic; transcoders must be a switch). §6 is the honest
  distance to a working driver on either path, and it is the section most likely
  to be misread from the green gate output in §1.
- `handoff13.md` — the state handoff14 continues from, and the first handoff in
  eight that moved the project's ceiling. The kext compiled and LINKED under Apple clang 17.0.0:
  `make exit status: 0`, `bundle: PRESENT`, Mach-O 64-bit kext bundle x86_64,
  with the metaclass constructors and all three `IOService` overrides present in
  `nm`. §2 is the one to read: the blocker was never the CD delivery channel all
  four operator artifacts were built around — the guest had **no toolchain at
  all**, and installing the Command Line Tools fixed it with zero source
  changes. §3 explains why the middle log's `make exit status: 141` is SIGPIPE
  rather than a compile failure. §5 records the 17 warnings and that the
  predicted `-lkmodc++` link failure never happened.
- `handoff12.md` — the state handoff13 continues from. It did not touch the kext
  either: at the
  time of writing, STEP 7 was still unanswered and
  `OPERATOR-ACTION-step7-build.md` was still the highest-value open item. What it closed is handoff11 §6's table-cell absence item, and it
  closed it as the opposite defect from the one filed. §2 is the one to read: the
  claim was already being read, and the first probe only appeared to show
  otherwise because it called `claims_absence(token, doc)` with the parameters
  swapped — both are `str`, so nothing raised and every token came back `False`.
  §3 is the real bug: `|` was missing from `SENTENCE_BREAK_RE`, a table survives
  the paragraph split as one flattened line, so a negation in an earlier cell or
  an earlier row granted ABSENT to every path after it.
- `handoff11.md` — the state handoff12 continues from. It did not touch the kext:
  `OPERATOR-ACTION-step7-build.md` was already the highest-value open
  item. What it closed is the last item needing nothing from the operator —
  `test_documented_paths.py` now scans eleven docs rather than five, and §3
  records the in-memory mutation run that proves the six new ones are not
  vacuous. §4 is the one to read: the `FLOORS` provenance comment was already
  false, because two of the docs it measured were edited three hours after the
  measurement and nothing reads a number in a comment.
- `handoff10.md` — the state handoff11 continues from, and the first session that
  moved toward STEP 7 rather than around it. The guest is up and its tools channel is live,
  but a live tools channel is not an *execution* channel — every `vmrun` op that
  runs something inside the guest needs the password. A virtual CD does not, so
  `tools/guest-build-kext.sh` plus `build-payload/step7-payload.iso` reduce the
  first Apple-clang build to one power cycle and one command
  (`OPERATOR-ACTION-step7-build.md`). §3 records three defects that script had,
  including a `find_payload()` that globbed a hardcoded `/Volumes` while the rest
  of the file was parameterised — so six execution tests would have passed
  vacuously against a directory Windows does not have.
- `handoff9.md` — the state handoff10 continues from: three silent-truncation bugs on the Phase 5
  address path plus a parser that could not read its own composed constants.
  §1 `nvGspStageAddressOk()` bounded only the BASE, so nothing bounded
  `base + paddedBytes` — added `nvGspStageRangeOk()`; §2
  `NVGSP_STATE_FIRMWARE_LOAD` never bounded `physAddrHi`, which the
  DMATRFBASE/BASE1 pair TRUNCATES above 17 bits rather than faulting on —
  added `NVGSP_DMA_PHYSHI_MASK` and `nvGspDmaPhysHiFits()`; §3
  `check-registers.py`'s `resolve()` matched `xFF` inside `0xFF` as a symbol, so
  no composed constant containing a hex literal could be parsed at all. §5
  carries the gate state and the measured suite counts, §6 what is still open.
- `handoff8.md` — the state handoff9 continues from, and the longest: `check-kext-syntax.bat`
  section 1 was parsing two of five headers standalone and now parses all five;
  `tools/deliver-via-cd.py` is the first credential-free host→guest file path;
  §7 records the `nvGspDmaTrfBase1()` bug — it masked the UNSHIFTED high word,
  so every physical base at or above 4 GiB programmed an overlapping
  `DMATRFBASE1` — and §8 records that all five C++ suites now end on their
  check count. All four gates were run and passed after each change.
- `handoff7.md` — Phase 5 staging is tested (`tests/test_gsp_stage.cpp`,
  142 checks) and `check-registers.py` covers `NVGspStage.hpp`.
- `handoff6.md` — how the Phase 5 staging arithmetic was split out of the kext
  into `NVGspStage.hpp`
- `handoff5.md` — the Phase 5 DMA state handoff6 continues from
- `handoff4.md` — how Phase 4 was built and verified
- `handoff2.md` / `handoff3.md` — the macOS *guest VM*, SSH and credential
  blockers. Separate track; none of it blocks driver work.
- `HANDOFF.md` — the long-form detail behind all of the above
- `PLAN.md` — phase definitions and termination conditions
- `PHASE4-GSP-BRINGUP.md` — the register derivation the state machine encodes

## The four verification gates

Run from the project root. These are the entire test story; there is no CI.

```sh
python tools/check-registers.py        # constants vs NVIDIA's headers
tests\build-msvc.bat                   # C++ decode suites, MSVC + CMake + Ninja + ctest
tests\check-kext-syntax.bat            # clang front end over the kext, via tests/kextshim/
tests\run-python-tests.bat             # Python tool tests, CPython only, no deps
```

`run-python-tests.bat` is the one gate that works on a bare checkout — it needs
nothing but CPython. `build-msvc.bat` and `check-kext-syntax.bat` both need
Visual Studio 2022.

To run a single C++ suite, build once and invoke the executable directly:

```sh
cd tests/build && ninja && ./test_gsp_boot.exe
```

To run a single Python test module:

```sh
python -m unittest discover -s tests -p test_line_endings.py -v
```

Not `python -m unittest tests.test_line_endings` — `tests/` has no
`__init__.py`, so that form fails with `ImportError: Failed to import test
module`, which reads as a broken test rather than a broken invocation.
`run-python-tests.bat` uses discovery for the same reason.

On this machine there is a second, stronger cause worth knowing, because it
changes the error text. `site-packages/tests/__init__.py` exists, so the name
`tests` *does* resolve — to the wrong directory — and the failure is
`ModuleNotFoundError: No module named 'tests.<your module>'`. That names your
file rather than the shadowing package, and reads convincingly as a broken new
test. Verified 2026-08-10 against the pre-existing `test_line_endings`, which
fails identically. Also do not add `-t .` to the discovery form; it reintroduces
the package import and breaks discovery too.

The kext is built only on macOS, via plain make rather than an Xcode project:

```sh
cd phase0-kext && make          # produces build/NV3050Probe.kext
```

## Architecture: why the headers are dependency-free

The kext lives in `phase0-kext/`. Its design centre is a hard split between
**logic** and **IOKit**:

- `NVDecode.hpp`, `NVFalcon.hpp`, `NVRiscvUcode.hpp`, `NVGspBoot.hpp`,
  `NVGspStage.hpp`, `IntelDisplay.hpp`, `IntelFramebuffer.hpp`,
  `IntelGpuId.hpp` and `IntelDisplayNames.hpp` contain every register constant,
  bit-field decoder, state machine, and the Phase 5 staging arithmetic. They
  include no IOKit, no kernel types, allocate nothing, and sleep never.
  The four Intel ones belong to the Intel track rather than the
  NVIDIA one, but they obey the identical rule, which is why
  `tests/test_intel_display.cpp`, `tests/test_intel_framebuffer.cpp`,
  `tests/test_intel_gpu_id.cpp` and `tests/test_intel_display_names.cpp` can
  drive them on Windows.

  `IntelDisplayNames.hpp` is the newest of them and the only one that exists
  because of a defect rather than a register layer. Its contents — four
  verdict-name switches, `igTransName()`, and `igComposeKey()` for IORegistry
  property keys — were file statics inside the Intel IOKit shell until
  2026-08-17, which means no host suite could compile them and nothing ever
  *executed* one. `igModeVerdictName()` had arms for verdicts 0 through 5 while
  `IG_MODE_VERDICT_LIMIT` is 6, so `IG_MODE_SRC_MISMATCH` published the unknown
  string into the IORegistry and all four gates were green over it. It is also
  the one dependency-free header with `#include`s: quoted siblings
  (`IntelDisplay.hpp`, `IntelFramebuffer.hpp`) for the verdict constants it
  switches on, which is still inside `phase0-kext/` and so still parses
  standalone under `-I phase0-kext`.
- `NV3050Probe.cpp` / `.hpp` is the NVIDIA track's IOKit shell, and
  `IntelDisplayProbe.cpp` / `.hpp` (added 2026-08-17) is the Intel track's — the
  only files that touch `IOService`, BAR mappings, or anything Apple-specific.
  **No host suite executes a line of either one, by construction**: both pull in
  IOKit's own `IOService` declarations, which do not exist off macOS. (Written in
  words rather than as an angle-bracket include path on purpose:
  `tools/check-documented-paths.py` resolves every path-shaped token in this file
  against the checkout, and it cannot tell an Apple SDK header from a missing repo
  file. Spelling that include here reddens both the syntax gate and the Python
  gate.) Anything landed in a
  shell is outside the reach of every C++ suite, so a pure function written
  there is a pure function nothing runs — that is the whole argument for this
  split, and `IntelDisplayNames.hpp` above is what it looks like when the
  argument is ignored and then repaired.

That split is what makes `tests/` possible at all. `tests/*.cpp` compiles the
*identical* header files under MSVC and drives them against a simulated
register file, so the code path exercised on Windows is the code path that will
run on hardware.

Every GPU access in `NVGspBoot.hpp` goes through a caller-supplied
`read32`/`write32` callback pair carried in `NVGspBootIo`. The kext supplies
real MMIO; `tests/test_gsp_boot.cpp` supplies a `Fixture` that journals writes
and can simulate a DMA engine, a scrub that never finishes, or an aperture
violation.

## Two conventions that have each caused a silent bug

**Address translation is per-engine on purpose.** `nvGspFalconReg()` and
`nvGspRiscvReg()` are separate functions, not one `translate(base, offset)`
helper. The RISC-V block sits exactly one 4 KiB page above the Falcon block, so
a single helper called with the wrong base produces a *valid* address into the
wrong engine. That does not fault — it reads and writes real hardware silently.
Every access is also checked against `nvIsInGspAperture()`
(`NV_PGSP_BASE`..`NV_PGSP_LIMIT`, `0x00110000`..`0x00113FFF`).

**Write order is the assertion, not final register contents.** Several
sequences latch on their last write — `MOD_SEL` latches the BROM parameter
block, `DMATRFCMD` triggers the transfer, `FBIF_CTL` enables physical DMA after
`TRANSCFG` configures the target. Checking the registers afterwards would pass
even with the order reversed, so `tests/test_gsp_boot.cpp` asserts *journal
indices* via `checkJournal(label, index, offset, value)`. If you reorder writes
in the state machine, those index assertions are what will catch it.

## Values from the trace vs. values this project invented

Constants captured from the real MMIO trace are named `*_OBSERVED_*` and carry
a comment with the timestamp they came from. Where a value could not be settled
by the trace, the header says so explicitly rather than presenting a guess as
fact — `NV_GSP_FW_ADDR_BITS` (41) is documented as a POLICY CEILING rather than a
derived hardware width, and `NVGSP_DMATRFCMD_LOAD_IMEM` records why the observed `0x614`
differs from the `0x610` a naive field decode produces (bit 2, the low bit of
SEC).

Preserve that distinction when adding constants. A value invented to make a
test pass, filed next to values witnessed on hardware, destroys the only
evidence trail this project has.

## Batch files must be CRLF

`tests/test_line_endings.py` is a gate, not a style check. `cmd.exe` seeks
batch files by byte offset, so a bare-LF `.cmd` or `.bat` runs its blocks out of
order and reports failures that describe the harness rather than the code under
test. Markdown, C++ and the vendored NVIDIA source under `reference/` are
excluded deliberately.

## Layout

| Path | What it holds |
|---|---|
| `phase0-kext/` | The kext: dependency-free headers plus the IOKit shell |
| `tests/` | Host-side C++ and Python suites, plus the three `.bat` gates |
| `tests/kextshim/` | Just enough fake IOKit/libkern for clang to parse the kext |
| `tools/` | Trace parsing, register verification, VM and VBIOS helpers |
| `reference/` | Vendored NVIDIA `open-gpu-kernel-modules` and GA107 firmware |
| `reference/i915/` | Vendored Linux i915 headers at v6.12 — the Intel track's only evidence |
| `.claude-crew/` | Multi-terminal coordination state (task board, notes) |
