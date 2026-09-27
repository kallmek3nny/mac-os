# 3050-handoff35 — a diagnosis computed twice and published never

NVIDIA track, 2026-08-19, continuing from `3050-handoff34.md`. It closes that
file's §6 item 3 — the item named there as the highest-value work not blocked on
hardware.

`phase0-kext/NV3050Probe.cpp` and `phase0-kext/NV3050Probe.hpp` both CHANGED, so
`build-payload/` was rebuilt — the **THIRTY FOURTH** image — and
`OPERATOR-ACTION-step7-build.md` updated.

---

## §1 — the subject, and why it is the third occurrence of one class

`loadGspFirmware()` opened by calling `nvGspStagePlan()`, and then did this with
the answer:

```c
if (planned != NVGSP_STAGE_OK) {
    NVLOG("GSP firmware: refusing %u-byte blob -- %s",
          (unsigned)blobLen, nvGspStageResultName(planned));
    return false;
}
```

That is the whole consumption. The verdict reached `NVLOG` and nothing else, and
the function returned a bare `bool` to a caller that could not tell a truncated
file from a version mismatch from a descriptor pointing off the end of its own
payload.

This is the class `3050-handoff31.md` §1 and `3050-handoff33.md` §1 each closed
one subsystem over: **a diagnosis that reaches the log and not the consumer**.
The log line is written to a console nobody is reading, on a machine whose
firmware has just refused to boot; the IORegistry is the artifact that survives
and that an operator can actually inspect afterwards.

### §1a — what made it worse than the previous two

By the time that log line ran, `nvGspStagePlan()` had computed **two**
independent verdicts, not one:

- the six-way `NVGspStageResult`, which the log line printed;
- the **eight-way** `NVRiscvUcodeVerdict` beneath it, which it did not.

The second is carried out of the plan struct **deliberately**, and
`3050-handoff32.md` §2 is the session that put it there, for a reason spelled in
`NVGspStage.hpp` itself: the stage mapping is lossy in both directions that
matter. Three ucode verdicts land on `ERR_MALFORMED` and three more land on
`ERR_REGION_OUTSIDE`. So `plan.ucode` exists precisely so a consumer can
recover what `planned` collapsed — and the one consumer in the tree threw it
away.

That makes handoff32's fix the same shape as handoff31's own §7 item 3, one
level up: the *detection* was built, tested, and correct, and only the
publication side was missing. `3050-handoff34.md` §1 then made the ucode verdict
load-bearing in the test suite for the first time. Three sessions of work on a
value whose only consumer discarded it.

### §1b — why no gate could see it

The same four by-construction blindnesses every module in this family lists:

1. `tools/check-registers.py` parses `#define`s. This path adds none, and the
   register gate is correctly **unmoved at 240**.
2. No C++ suite can link a translation unit that reaches `IOService`, so
   `loadGspFirmware()` is unreachable from `tests/*.cpp` by construction. That
   is the whole argument for the logic/IOKit split, and it is also what puts
   every publisher outside the reach of the suite that proves the value correct.
3. `-Werror` has nothing to say about a `setProperty()` that was never written,
   nor about a struct field nobody reads.
4. **`tests/test_gsp_stage.cpp` has 181 checks over `nvGspStagePlan()`** and
   proves the verdict is CORRECT. Nothing whatsoever proved a consumer reads
   it. This is `intel-handoff26.md` §1's heavily-tested-function-with-no-callers
   for the third time in this tree, and the fourth entry is always the one worth
   carrying: a suite proving a value is right says nothing about whether it is
   used.

---

## §2 — the fix, and its four load-bearing choices

`publishGspStageVerdict()` publishes four keys:

| Key | Carries |
|---|---|
| `NVGspStageVerdict` | `nvGspStageResultName(planned)` — the six-way result |
| `NVGspStageOk` | `planned == NVGSP_STAGE_OK` |
| `NVGspStageUcodeEvaluated` | whether the eight-way verdict was reached |
| `NVGspStageUcode` | `nvRiscvUcodeVerdictName(plan->ucode)`, or `"not evaluated"` |

Each of the four choices below is load-bearing on its own.

**The call is UNCONDITIONAL and precedes the refusal test.** Publishing inside
the refusal arm would still publish on the path an operator debugging a refusal
reads, which is exactly why it looks harmless — and it would make the keys'
ABSENCE mean two different things: staged fine, or an older kext that never
published them. That is `3050-handoff33.md` §1a's *an absent key is a question
rather than an answer*, and it is why the keys land before anything acts on the
verdict.

**`NVGspStageOk` is an EQUALITY against `NVGSP_STAGE_OK`**, never a negation of
a particular error arm. `planned != NVGSP_STAGE_ERR_NO_BLOB` is TRUE for the
five other refusals and would report a refused blob as staged. Same reasoning as
handoff32 §3's `!= ERR_HEADER` and handoff33 §2's `NVPartIsTarget`, now in a
third subsystem — the pattern is general enough that it should be the default
assumption when publishing any boolean derived from an enum.

**The ucode NAME is taken only when `ucodeEvaluated` is true.**
`nvGspStagePlan()` initialises `plan.ucode` to `NV_RISCV_UCODE_VERDICT_LIMIT`
and sets the flag only AFTER `nvRiscvUcodeVerdict()` returns (`NVGspStage.hpp`
is explicit about the ordering and why). An unguarded name lookup on the
no-blob path would render the sentinel — a value nobody measured, published in
the same shape as one that was, which is handoff18 §1d's class arrived at from
the fabrication side.

**The unevaluated string is a LITERAL, not a sentinel name.** It would have been
shorter to publish `nvRiscvUcodeVerdictName(NV_RISCV_UCODE_VERDICT_LIMIT)` and
let that function's `default:` arm answer. That is exactly handoff21 §1b's
mistake: a value read out of a sibling function's default arm is a value this
file cannot vouch for, and it can change without this call site knowing.

The refusal log line now also carries the ucode verdict, so the two channels
agree by construction — with the same honest limit handoff33 §7 item 2 records:
nothing gates that agreement if somebody edits one and not the other.

---

## §3 — the gate, and the defect it found in itself

`tests/test_probe_gsp_stage_publication.py`, **13 tests**, in the idiom of the
four sibling probe publication modules: one `audit()` shared between the live
tree and every mutant, comment-stripped source, one positive control, and
negative controls each asserting WHICH property broke.

Nine properties. Three are worth naming:

- **Property 5 counts rather than checks presence.** The two ucode keys are
  expected **twice** each, because the null-plan arm returns early and needs its
  own pair. A single occurrence compiles, runs, and leaves the key absent on the
  arm that was deleted — the copy-paste shape, and presence cannot see it.
- **Property 9 exists because property 5's counts cannot see an emptied arm.**
  Two `setProperty()` calls moved BELOW the early `return` satisfy every count in
  the file while publishing nothing on that arm. It checks the guard's brace span
  directly.
- Both key anchors include the closing quote, because `NVGspStageUcode` is a
  strict PREFIX of `NVGspStageUcodeEvaluated`. A bare substring count of the
  shorter returns four where the answer is two, and the ucode arms would look
  published when only the evaluated flag was.

### §3a — the one to read: bareness is not depth

The negative control for property 3 (guard the call behind the refusal) went
green — but it reddened **property 4**, not property 3. Property 3 did not fire
at all, and the reason is a real defect in my own predicate.

`statement_is_bare()` — inherited from the sibling modules, where it is correct
for what they ask of it — walks BACK to the previous `;`, `{` or `}` and requires
only whitespace in between. A call nested inside an `if` block is preceded by
that block's opening `{`, which **is** a statement boundary. So bareness reports
True on exactly the mutation property 3 exists to catch.

**Statement level and nesting level are two different questions, and only the
second one means unguarded.** The fix is `brace_depth()`, which counts braces
from the function body's start (skipping string literals, or a `{` in a log
format string would shift every depth after it) and requires depth 1. Bareness
is kept beside it, testing the thing it can actually answer: whether the call
stands alone rather than hanging off another expression.

This is worth carrying because the module would have passed. Property 4 caught
the mutation for an unrelated reason — the guarded call happens to sit after the
refusal test textually — so the suite was green, the control asserted a failure
list that was non-empty for the wrong property, and had I written the control as
`assertNotEqual(failures, [])` rather than asserting WHICH property broke, the
defect would have shipped inside the gate. That is the entire argument for
per-property failure strings, demonstrated on the session that relied on it.

---

## §4 — gate readings

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240**, unmoved — correctly, no `#define` added |
| `tests\build-msvc.bat` | **9/9** suites passed; `ninja: no work to do` |
| `tests\check-kext-syntax.bat` | exit **0** |
| `tests\run-python-tests.bat` | **1010** tests, **1 failure, not mine** — see §4a |

No C++ suite was touched, so `tests/test_check_count_floor.py` needed no edit and
is green at 16 tests. The Python gate uses discovery, so the new module needed no
registration.

Python 980 → 1010 is +30. Mine is **13**, measured by running the module alone
BEFORE the gate. The remaining 17 are not mine.

### §4a — the one failure, and why it was not fixed

`test_source_cited_paths.test_every_cited_path_resolves` fails on one token:
`phase0-kext/IntelScanout.hpp` cites `tests/test_intel_scanout.cpp` at its line
9, and that file does not exist.

Attributed by mtime, which is the rule `intel-handoff27.md` §5a establishes:
`IntelScanout.hpp` was written **2026-08-18 20:58:31**, thirteen hours before my
first edit in this session (`NV3050Probe.hpp` at 09:51, `.cpp` at 09:52), and
`intel-handoff29.md` is in the tree. This is the peer's Intel-track work,
half-landed: the header is a dependency-free logic header whose comment promises
a host suite that has not been written yet.

**Nothing was touched.** Writing that suite, or deleting the citation, would vouch
for a peer's in-flight work under this session's name — and the citation is
almost certainly correct about its own intent, just early. The gate's verdict in
a two-agent tree is a JOINT measurement: red does not mean you broke it.

Worth noting the failure is also *the gate working*: a comment promising a test
file is a claim, and this is the tree catching the claim before the file exists.

---

## §5 — what this does NOT establish

1. **Nothing has run.** `publishGspStageVerdict()` is reachable only by parse.
   No suite executes a line of it, by construction, and the kext has never been
   `kextload`ed. The keys are correct in shape, and their VALUES on real silicon
   are unmeasured.
2. The four keys are only as useful as the verdicts behind them, and those are
   verdicts about a **blob**, not about the card. Every one of them can be
   produced on a machine with no NVIDIA hardware at all.
3. The log line and the registry now agree by construction only as long as
   nobody edits one and not the other. No gate covers that agreement —
   handoff33 §7 item 2's limit, inherited unchanged.
4. `brace_depth()` skips string literals but not raw strings or trigraphs. The
   shell contains neither; a future one might.
5. The gate is spelled against `publishGspStageVerdict(` by name. A second
   publisher added later is a function this module does not consider a subject
   at all — and it reports nothing rather than failing, which is the vacuity
   direction. Same hole `3050-handoff30.md` §5 item 3 names for its own family.

---

## §6 — open, highest value first

1. **One `kextload` on a machine containing the card.** Unchanged for fifteen
   sessions and still the largest single piece of evidence this project does not
   have. It needs SIP lowered and a reboot, and the VMware guest has no NVIDIA
   card to match against.
2. **`3050-handoff27.md` §1 outranks everything on this track and should be read
   before planning more NVIDIA work.** The laptop is **muxless** — all graphics
   route through the Intel chip, HDMI being the only bypass — so `PLAN.md`
   termination condition A is SATISFIED. A complete, perfect NVIDIA framebuffer
   driver still puts nothing on the internal panel. Everything on this track is
   Phase 4/5 engineering and is not on the critical path to a picture.
3. The publication sweep this session continues is now three-quarters done
   (BOOT_42, BOOT_0, GSP boot mask, stage verdict). The remaining log-only
   diagnosis worth auditing is `nvGspBootCheckRiscvErrors()`'s own distinctions
   — the error pair reaches the registry as WORDS, but the decode of those words
   is `NVLOG`-only. Same class, one level down.
4. A gate over the log/registry agreement in §5 item 3, which would close a
   limit now inherited across three handoffs.
5. `tests/test_intel_scanout.cpp` (§4a) is the peer's to write, not this
   track's. Do not close it from here.
