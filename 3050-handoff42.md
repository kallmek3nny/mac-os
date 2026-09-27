# 3050-handoff42

NVIDIA track. Direct continuation of `3050-handoff41.md`. Three things happened,
and they are in descending order of how long they will stay useful:

1. **`3050-handoff41.md` §7 item 2 was FALSE when written** — the same class that
   file opens by recording about ITS predecessor, now three sessions running.
   The honest remainder is one log arm, and §2 closes it.
2. `tests/test_probe_gsp_stage_log_arms.py`, **17 tests**. Fourth member of the
   log-arm family.
3. **`PLAN.md` termination condition A is written into `PLAN.md`** rather than
   left in a handoff, closing `3050-handoff37.md` §6 item 2.

`phase0-kext/` is **BYTE-IDENTICAL** — no shipping file changed — so
`build-payload/` was NOT rebuilt and the FORTIETH image is still current.

**Three gates are RED and none of the failures is attributable to this session.**
§5 is the attribution, and it was done before anything was touched.

---

## 1. The open item was wrong again, and how it was measured

handoff41 §7 listed five items. Item 2 read: *no log-arm gate covers
`loadGspFirmware()`'s own refusal logging — the stage VERDICT is published and
gated, but the eight-way ucode verdict beneath it still reaches only `NVLOG`.*

The second clause is false. Four greps:

| Claim in the item | What is actually in the tree |
|---|---|
| the ucode verdict reaches only `NVLOG` | `publishGspStageVerdict()` publishes `NVGspStageUcodeEvaluated` and `NVGspStageUcode` |
| …on every arm | including the early-bail arm, where it publishes `"not evaluated"` |
| nothing gates it | `tests/test_probe_gsp_stage_publication.py`, nine properties |

I ALSO carried a second wrong claim into this session before measuring it:
that `3050-handoff37.md` §6 item 1 (`nvRiscvErrorVerdict()`'s five codes
consumed as a bare bool) was open. It is closed too.

The genuinely-remaining part of item 2 is the FIRST clause, and it is real: the
`NVLOG` at `NV3050Probe.cpp:1648` consumes the same two verdicts the registry
consumes, and nothing required the two to keep agreeing. That is what §2 closes.

### 1a. Why this is worth a section for the third time

`3050-handoff29.md` §1 recorded it, `3050-handoff32.md` §1 recorded it,
`3050-handoff41.md` §1 recorded it against an item its own author wrote sixty
minutes earlier — and I then repeated it in the session that inherited that
warning. Reading the lesson is not the same as running the grep.

The failure mode is specific and it is not laziness. The log-only-diagnosis
sweep has a RHYTHM — detection built, publication missing — and after five
sessions of it, an item that FITS the rhythm reads as measured. handoff41 was
NOT edited; the numbered handoffs are a chronological record, not a status page.

**The durable rule, stated one notch harder than handoff41 stated it: an open
item is a claim with an age even when the previous session's file OPENS by
warning you about exactly this.** Grep it. It costs four minutes.

---

## 2. The gate, and the defect it forbids

`loadGspFirmware()` publishes at `NV3050Probe.cpp:1645` and logs at `:1648`.
Both consume `planned` and `plan.ucode`. The publication half is pinned by nine
properties; the log half by nothing.

That is the split the three existing log-arm gates exist for
(`tests/test_probe_gsp_result_log_arms.py`, `tests/test_probe_identity_log_arms.py`,
`tests/test_probe_boot42_log_arms.py`). A publication gate proves the REGISTRY is
right and says nothing about whether the log still agrees with it. An operator
reading a serial console and an operator reading the IORegistry must not be told
different things about one refusal.

### 2a. The specific defect

The log's ucode half must stay guarded by `ucodeEvaluated`. `NVGspStage.hpp:297`
initialises `plan.ucode` to `NV_RISCV_UCODE_VERDICT_LIMIT` — measured, not
remembered — precisely because zero is `NV_RISCV_UCODE_OK`, so an unevaluated
field left at its initialiser would report **well-formed** for a blob nobody
validated. `NV_RISCV_UCODE_VERDICT_LIMIT` is the one value
`nvRiscvUcodeVerdictName()` deliberately REFUSES to name.

So a "simplification" that drops the guard and names `plan.ucode`
unconditionally neither crashes nor reddens any existing gate. It prints a
refusal string for the one case the flag exists to distinguish — a blob that
failed staging BEFORE the ucode was ever examined — and the console then
contradicts an IORegistry that got this right. handoff18 §1d's
fabricated-publication class, arriving at the log instead of the registry.

### 2b. Five properties

1. The refusal arm logs at all.
2. Both `nvGspStageResultName()` and `nvRiscvUcodeVerdictName()` are reached.
3. **Every** `nvRiscvUcodeVerdictName(` call is guarded by `ucodeEvaluated`.
4. The initialiser sentinel is still `NV_RISCV_UCODE_VERDICT_LIMIT`.
5. `publishGspStageVerdict()` is still called, so the log is not the sole
   consumer.

Property 4 is the one worth arguing for. P3 is load-bearing **only while P4
holds**: if a future edit initialises `plan.ucode` to something nameable, the
guard stops mattering and this module would keep passing while protecting
nothing. The premise is asserted rather than assumed.

Property 5 is deliberately NOT a re-assertion of the publication gate's nine.
That module owns those, and two modules with a claim on one subject is how a
disagreement becomes ambiguous. P5 pins only the claim the log arm depends on.

---

## 3. Two ways the gate was wrong before the subject was

### 3a. The positive control failed, for the FOURTH session running

v1's guard predicate looked for `if (ucodeEvaluated) { ... }`. The subject uses a
**ternary** inside the `NVLOG` call. `test_audit_is_green_on_the_live_tree`
reddened on correct shipping code.

Both shapes are real guards. A gate that recognises only the one its author
pictured reports a defect in the subject when the defect is in the gate — so
`guard_verdict()` accepts either, and the module docstring says why.

Fourth session in this tree where a first draft's failing check is its own
positive control (`3050-handoff37.md` §3a item 2 counts the first three).

### 3b. A negative control found a real hole in the predicate it guards

This is the one to read. v2 asked only whether the flag PRECEDED a `?`. So:

```c
    plan.ucodeEvaluated ? "not evaluated" : nvRiscvUcodeVerdictName(plan.ucode)
```

read as **guarded** — and that call runs exactly when the verdict was never
evaluated. The flag is there, the `?` is there, and the meaning is inverted.

`test_a_call_in_the_false_arm_reddens` caught it: `Ran 17 tests`,
`FAILED (failures=1)`, `AssertionError: [] is not true`. The fix is
`ternary_guards()`, which requires no bare `:` between the `?` and the call —
the colon is what separates the arms — and steps over `::` so a qualified name
in the true arm is not misread as an arm break.

Worth stating plainly: **the controls caught the first draft of the thing they
guard.** That only worked because every control drives `guard_verdict()` — the
function the audit uses — rather than a second copy of its logic. A control
asserting against a reimplementation proves the reimplementation.

### 3c. Coverage

**17 tests.** One positive control over the live tree, three structural controls,
two REFUSAL controls (a renamed subject refuses rather than passing — a module
reporting nothing when its anchor moves is byte-indistinguishable from a green
one), four stripper controls, two positive controls proving BOTH guard shapes
are accepted, and five negative controls:

| Mutation | Property it must redden |
|---|---|
| drop the ternary guard | the call is unguarded |
| drop the `if` guard | the call is unguarded |
| hoist the call out of the guard | the call is unguarded |
| a second bare call beside a guarded one | the copy-paste shape |
| the call in the ternary's FALSE arm | §3b |

Comment and string stripping is mandatory rather than stylistic, and has its own
controls: the subject's comment block discusses `ucodeEvaluated` by name and
quotes both verdict functions, and the log's own format string carries `ucode`
while the fallback carries `not evaluated`. A prose-reading gate would find every
token it exists to require on a function whose guard had been deleted.

---

## 4. `PLAN.md` (§6 item 2 of handoff37, closed)

Two claims in `PLAN.md` still asked whether the laptop is muxless — one as an
outstanding request to the operator (§2 item 2), one as termination condition A
hedged with *most likely, check the BIOS*.

It is answered from two independent directions and has been for sessions:

- The operator's report that all graphics route through the Intel chip
  (`3050-handoff27.md` §1).
- The GA107's own VBIOS. Its Display Configuration Block declares **zero display
  paths** — `entry_count` is `0x00` in a DCB that is present, well formed,
  version `0x41`, at exactly the offset the pointer at ROM offset `0x36` names,
  with an empty connector sub-table while CCB, GPIO and I2C are populated
  (`3050-handoff37.md` §0; `tools/parse-dcb.py`; `dcb-report.txt`).

Both sites now read as findings, cite all three artifacts, and say which
direction is stronger: a driver cannot invent a path the board does not carry,
and this DCB describes no path of any kind — not even the HDMI bypass handoff27
named.

The risk in this edit is `tools/check-documented-paths.py`, which resolves every
path-shaped token in a doc against the checkout. Wildcards, angle-bracket
placeholders and backslash-slash tokens each redden two gates. The edit was
written to that constraint and the gate is green at **134 documented paths**.

---

## 5. Gate readings, and why three are RED without being mine

Measured after the last edit. Every exit status was captured BEFORE any pipe —
`$?` after a pipeline is the last command's status, and I made exactly that
mistake once earlier in this session, reading `head`'s `0` as the Python gate's.

| Gate | Reading |
|---|---|
| `python tools/check-registers.py` | exit 0, **242/242** passed |
| `python tools/check-documented-paths.py` | exit 0, **134** documented paths |
| `tests\run-python-tests.bat` | exit 1, **Ran 1219 — FAILED (failures=7)** |
| `tests\check-kext-syntax.bat` | exit 1, `IntelFbPresent.hpp` not parsed standalone |
| `tests\build-msvc.bat` | exit 8, 11 of 12 suites, `test_intel_fb_present` failed |

All nine failures across the three red gates name `phase0-kext/IntelFbPresent.hpp`
or `tests/test_intel_fb_present`. The mtimes settle it — the peer's Intel landing
was still moving after my last write:

| File | mtime |
|---|---|
| `tests/test_intel_fb_present.cpp` | 10:38:16 |
| `tests/CMakeLists.txt` | 10:42:59 |
| `phase0-kext/IntelFbPresent.hpp` | **10:44:38** |
| `tests/test_probe_gsp_stage_log_arms.py` (mine) | 10:33:31 |
| `PLAN.md` (mine) | 10:42:25 |

My only writes this session are one `tests/*.py` module and `PLAN.md`. Neither is
named by any failure.

The correct action is to touch NOTHING, and specifically not to rebuild
`build-payload/`. Rebuilding would put a peer's header into the shipped tar and
sign the operator card for it under this session's name — while that header's own
C++ suite is RED and the syntax gate refuses it. That is vouching for half-landed
work (`intel-handoff27.md` §5a). This is the same call handoff41 §5 made, for the
same reason, against the same peer's previous landing.

Register gate **241 → 242**: also the peer's. This session added no `#define` at
all.

### 5a. The count was attributed rather than explained, and it closes exactly

Python was **1203** at handoff41. The new module is **17** tests, measured by
running it alone BEFORE the gate. 1203 + 17 = 1220. The gate reads **1219**.

This section first recorded that as UNATTRIBUTED. It was then MEASURED, in this
session, by counting discovered tests PER MODULE rather than reasoning about the
total — the difference between the two being the whole point of §1.

`tests/test_probe_boot42_log_arms.py` collects and runs **19**, in one class,
and `3050-handoff41.md` §4 reports that module as **20** in the session that
created it. 1203 − 1 + 17 = **1219**, which is the gate's own reading, so the
entire discrepancy is inside that one module and nothing else moved.

The mtimes bound WHEN. `3050-handoff41.md` is 09:56-adjacent at **09:43:46**;
the module is **09:56:00** — twelve minutes AFTER the handoff that reported 20,
and thirty-seven minutes BEFORE this session's first write at 10:33:31. The edit
therefore sits in the gap between handoff41's authorship and this session, and
is neither the state handoff41 reported nor mine.

What is NOT recoverable is WHICH method went. This tree is not a git checkout,
so there is no previous revision to diff, and handoff41 §4 describes its twenty
by CATEGORY — one positive, four structural, four stripping, two wrapped-literal,
nine negative — a taxonomy that does not map cleanly onto the nineteen names now
present, twelve of which are negative controls. So the category counts cannot
name the absence either.

The module is GREEN at 19 and its positive control over the live tree still
passes, so this is a lost CHECK rather than a broken one. One command
reproduces it: `python -m unittest discover -s tests -p test_probe_boot42_log_arms.py`.

### 5b. A rendering artifact that looked like file corruption

Search output rendered four comment lines in `NV3050Probe.cpp` and one in
`NVGspStage.hpp` as opening `\ ` rather than `//`, which would break the syntax
gate outright. Two greps for a leading backslash returned **no matches in either
file**. A display artifact, not corruption — worth one grep before believing a
tool's rendering of bytes over the bytes.

---

## 6. What this does NOT establish

1. **The log and the registry still agree by construction only.** This gate is
   structural, over comment- and string-stripped source. It executes nothing. A
   future edit keeping every marker while changing what an arm MEANS passes.
2. **`loadGspFirmware()` is unchanged.** It still returns `bool`. The gate pins
   the shape of a correct function; it fixed nothing, because nothing was broken.
3. **Nothing here has executed on hardware**, and no host suite compiles
   `NV3050Probe.cpp` — it reaches IOKit.
4. **The operator's standing request is still architecturally unreachable in this
   tree.** §7 item 1.

---

## 7. Open items, highest value first

Each was MEASURED this session rather than carried forward, except where noted.

1. **"Render on the 3050, output through the Intel driver" has no layers in this
   tree, and saying otherwise would be the worst thing in this file.** That
   request describes muxless PRIME-style offload. It needs, in order: GSP
   firmware actually booting on real silicon; a channel/pushbuffer
   implementation; an IOAccelerator stack; a shared-surface allocator; and an
   Intel framebuffer able to inherit a foreign buffer. **None of the first four
   exist even in skeleton**, and `PLAN.md` caps the project at Phases 0–5 for
   exactly this reason. §4's finding is what makes offload the ONLY conceivable
   path to a picture — and it is a path this tree has no layers for. Driver
   hygiene work like §2 is real and is not progress toward it. Do not let a
   future session imply otherwise.
2. **`CLAUDE.md`'s read-order list stops at `3050-handoff37.md`.** Handoffs 38
   through 42 are absent. Deliberately not edited for the second session running,
   and the reason is stronger now than when handoff41 gave it: the file is shared
   with the concurrent Intel agent, who was writing `phase0-kext/` and
   `tests/CMakeLists.txt` DURING this session's gate runs. Worth one careful pass
   by whoever has the file uncontended.
3. **The Intel peer's tree is mid-landing.** Nine failures across three gates, all
   naming `IntelFbPresent.hpp` / `test_intel_fb_present`, newest write 10:44:38.
   Nothing for this track to do; noted so the next NVIDIA session does not
   attribute them to itself and "fix" them.
4. **The Python count's missing test is attributed to
   `tests/test_probe_boot42_log_arms.py`** (§5a) — it collects **19** where
   handoff41 §4 reports 20, its mtime falls in the gap between that handoff and
   this session, and 1203 − 1 + 17 matches the gate exactly. The module is green
   and its positive control still passes, so what remains open is only WHICH
   check was lost, and that is unrecoverable without a previous revision in a
   tree that is not a git checkout. Worth one look by whoever holds the module.
5. **`test_intel_scanout` still has no `MEASURED` provenance entry**, for the
   fifth session. Adding one would vouch for a C++ suite this session never built
   successfully.
6. **One `kextload` on a machine containing the card** remains the single
   highest-value action in the whole project, and remains blocked on hardware,
   SIP, and the mutually-exclusive Linux/macOS choice on the target laptop
   (`3050-handoff27.md` §1).
