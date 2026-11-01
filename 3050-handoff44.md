# 3050-handoff44 -- the guard that handoff43 left unguarded

NVIDIA GA107 track. Continues `3050-handoff43.md`. Written 2026-08-20.

This session closes `3050-handoff43.md` §7 item 2 -- the hole that file's own fix
left behind. It found no defect in shipping code: `phase0-kext/` is
**CONTENT-IDENTICAL** (proven by md5 on both sides of a deliberate mutation, §3),
so `build-payload/` was NOT rebuilt and nothing is owed by this session. §5 is the
attribution for why the rebuild handoff43 §6 deferred is already done, by somebody
else.

**All five gates are GREEN on the last reading, and the Python gate was RED in
between.** §4 is the first reading; §4c is the sequence of three, with the
attribution for the red and for who cleared it. None of the three is edited away,
because a reading taken is a measurement. Every exit status was captured before
any pipe.

---

## 0. The operator's request, and the thing that has to be said again

The standing request is "continue making the driver render, then use the Intel
driver to output". That is muxless PRIME-style offload. It is the architecturally
right shape and it is **unreachable in this tree**, on three facts already
measured rather than on an opinion:

1. **The laptop is muxless** -- all graphics route through the Intel chip, HDMI
   being the only bypass (`3050-handoff27.md` §1, from the operator).
   `PLAN.md` termination condition A is SATISFIED.
2. **The GA107 VBIOS declares zero display paths.** `entry_count` is `0x00` in a
   DCB that is present, well formed, version `0x41`, at exactly the offset the
   pointer at ROM offset `0x36` names, corroborated seven ways by
   `tools/parse-dcb.py` (`3050-handoff37.md` §0). A driver cannot invent a path
   the board does not carry -- and this DCB describes no path of any kind, not
   even handoff27's HDMI bypass.
3. **No Tiger Lake framebuffer exists in the Sequoia recovery image** -- 430 kext
   paths, none matching. Condition B, already measured.

Offload needs, in order: GSP firmware actually booting on real silicon; a channel
and pushbuffer implementation; an IOAccelerator stack; a shared-surface
allocator; and an Intel framebuffer able to inherit a foreign buffer. **None of
the first four exist even in skeleton**, and the kext has never been
`kextload`ed. `PLAN.md` caps this project at Phases 0-5 for exactly that reason.

This section exists so the next session does not spend a cycle re-deriving it.
`3050-handoff42.md` §7 item 1 and `3050-handoff43.md` §0 are the same guard. The
work below is hygiene; it moves the offload distance by zero, and saying so is a
standing obligation rather than a caveat.

---

## 1. The subject: a fix whose guard nothing held in place

`3050-handoff43.md` fixed the eighth occurrence of `handoff18.md` §1d's
fabricated-publication class. `readUsableFbSize()` had stored a masked value into
the caller's out-parameter **before** its three refusal tests, and the mask is
`0xFFFFFFFF` -- so a refused read left **4294967295** in `fbSizeMB`, and
`publishFirmwareState()`'s `if (fbSizeMB > 0)` let it into the IORegistry as
`NVUsableFramebufferMB`. handoff43 replaced the magnitude test with an EQUALITY
against `NV_FB_SIZE_OK` and moved the store to the accepting arm.

And then handoff43's own §7 item 2 recorded the hole plainly: **delete the
equality guard and restore the magnitude test, and all five gates stay green.**

That item was measured this session rather than inherited -- `3050-handoff42.md`
§1 is the standing reason (an open item is a claim with an age), and this one came
back TRUE as written. §3 is the proof, and it is the same shape
`3050-handoff30.md` filled for the GSP boot publisher.

### 1a. Why all five gates are blind to it, re-derived

1. **The register gate** (`tools/check-registers.py`) checks `#define`s against
   NVIDIA's headers. A property KEY is not a `#define` and a guard is not a
   constant.
2. **The C++ suites** cannot compile `NV3050Probe.cpp` -- it reaches IOKit, which
   does not exist off macOS. The publisher lives there.
3. **`-Werror`** has nothing to bite on. An unguarded `setProperty()` is a
   well-formed call, and `fbSizeMB > 0` is a well-typed comparison.
4. **`tools/check-documented-paths.py`** resolves path tokens. Silent by
   construction.
5. **The Python gate** modelled publication SHAPE for five other families
   (`GSPEngineReadable`, `NVIdentityReadable`, `NVBoot42Readable`,
   `NVGspBootReadMask`, the GSP stage verdict). Nothing modelled this one.

So the answer is the sixth member of that family:
`tests/test_probe_firmware_state_publication.py`, **26 tests**, in the idiom of
`tests/test_probe_gsp_boot_publication.py` -- one `audit()` shared with the live
tree, comment- and string-stripped source, one positive control over the real
file, and eleven negative controls each asserting WHICH property broke.

---

## 2. The six pinned properties, and the three that are load-bearing

1. **`NVUsableFramebufferVerdict` is published unconditionally**, through
   `nvFbSizeVerdictName()` rather than a hand-spelled string. An ABSENT key is a
   question rather than an answer, indistinguishable from an older kext that
   never published it -- verbatim the reasoning behind the four existing
   readable-flag families. Requiring the NAME FUNCTION rather than any string is
   what stops a future edit publishing `"ok"` on an arm the enum calls something
   else.
2. **`NVUsableFramebufferMeasured` is published unconditionally**, as an equality
   against `NV_FB_SIZE_OK`.
3. **`NVUsableFramebufferMB` is published exactly ONCE and is GUARDED.** The
   once-ness matters: a second bare `setProperty()` beside a guarded one is the
   copy-paste shape, and it satisfies every is-it-guarded assertion.
4. **The guard is an EQUALITY against `NV_FB_SIZE_OK`.** Three findings, not one:
   not an equality at all; `!=` anywhere in the condition (a negation of one
   error arm is TRUE for the other two refusals -- `3050-handoff33.md` §2's
   `NVPartIsTarget` reasoning); or a magnitude test on `fbSizeMB`, which is the
   exact defect handoff43 removed.
5. **Verdict and measured flag sort ABOVE the value they qualify.** An operator
   reading the registry top-down meets the qualification before the number.
6. **The accepting enumerator is DERIVED from `phase0-kext/NVDecode.hpp`**, not
   spelled in the test. `3050-handoff30.md` §2's derive-the-subject-set
   reasoning: rename the enumerator and this module reddens on the commit that
   renames it, naming it.

### 2a. The guard detector is a balanced walk, deliberately

`guard_controlling()` walks BACKWARDS from the statement to the previous
statement boundary, then requires an `if (...)` in front of it, and captures the
condition by a balanced-paren walk.

It is not a regex over the preceding line and it is not a negated character
class. `handoff25.md` §1c is the cost of the latter: a class excluding only the
semicolon runs past the `if`'s closing paren, the newline and the whole guarded
body to land on the next statement's terminator, and the resulting gate reddens
on CORRECT shipping code. `bareness-is-not-nesting-depth` is the off-by-one that
lives next door, which is why the walk handles the enclosing-brace case
explicitly rather than trusting a depth count.

### 2b. Stripping is mandatory here, not stylistic

The subject's own comment block **spells the refused forms**: it quotes
`fbVerdict != NV_FB_SIZE_UNREADABLE` and `fbSizeMB > 0` in order to explain why
neither is acceptable. A prose-reading gate would find both patterns it exists to
forbid on a publisher that is correct, and would find every required marker on
one whose guard had been deleted. Both directions are wrong; both are closed by
scanning code only.

### 2c. The controls run against a synthetic body, not the live tree

The eleven negative controls drive a `GOOD` string through the same property
extraction the audit uses. The live file is never mutated by the test suite --
that mutation happens once, by hand, in §3, with a guaranteed restore. A control
asserting against a re-implementation would prove the re-implementation, so the
extraction is shared rather than copied.

---

## 3. The non-vacuity proof

The mutation is the one handoff43 §7 item 2 named: delete the
`if (fbVerdict == NV_FB_SIZE_OK)` line from the live
`phase0-kext/NV3050Probe.cpp`, run the audit, restore in a `finally`.

```
anchor occurrences: 1
BASELINE  : []
MUTATED   : ['NVUsableFramebufferMB is published UNGUARDED -- a refused read
              lands in the IORegistry as a framebuffer size']
md5 before: b08e84b3a8dc359fc70b7b1e4d36b542
md5 after : b08e84b3a8dc359fc70b7b1e4d36b542
RESTORED  : True
POST-REVERT: []
```

Three things in that block are the evidence rather than decoration.

**`anchor occurrences: 1`** is checked BEFORE the edit. A mutation anchor that
matches more than it means raises loudly and reads as though the mutation is
wrong when the precondition is working (`intel-handoff26.md` §3's over-match
direction); one that matches nothing prints `ok` silently and proves the test
vacuous. Counting first separates both from a real result.

**Exactly ONE finding, and it is the new one.** `3050-handoff34.md` §3's rule: a
mutation that reddens MORE checks is usually WEAKER evidence, because the extra
failures are pre-existing checks that already sufficed. One new finding with an
empty baseline is the discriminating reading.

**Both md5s are equal.** The file's mtime moved to 15:36:40; its content did not.
The rule that decides a `build-payload/` rebuild is CONTENT, and the payload
comparison in the Python gate is the thing that reads content -- so its silence
is the proof rather than my assertion. `a-byte-size-is-not-a-fingerprint` is the
reason this is an md5 and not a size.

---

## 4. Gate readings

Every reading is from this session, after the last edit. Exit codes were captured
**before any pipe** -- `$?` after a pipeline is the last command's status, which
has produced a false green in this tree before (`3050-handoff42.md` §5,
`3050-handoff43.md` §4).

| Gate | Reading |
|---|---|
| `tests\run-python-tests.bat` | exit 0, **Ran 1245 tests**, `OK`, zero FAIL/ERROR lines |
| `python tools/check-registers.py` | **242 of 242** passed, 0 failed, 0 skipped, exit 0 |
| `python tools/check-documented-paths.py` | **134** paths accounted for, exit 0 |
| `tests\check-kext-syntax.bat` | exit 0, zero FAIL lines, `ALL SYNTAX GATES PASSED` |
| `tests\build-msvc.bat` | exit 0, **100% tests passed, 0 failed out of 12** |

The register gate is unmoved at 242 and correctly so: a property key and a guard
shape are not `#define`s. The C++ counts are all unmoved, so
`tests/test_check_count_floor.py` needed no edit -- and that is a measurement
rather than an assumption, because its `MEASURED` dict keys on `tests/*.cpp`
suite stems, so a Python module has no entry to add.
`tests/test_documented_paths.py` needs no edit either: its handoff exemption is
DERIVED by regex over the three spellings, so `3050-handoff44.md` is exempt
without a gate change.

### 4a. The syntax gate went GREEN underneath this session

`3050-handoff43.md` §4 recorded `tests\check-kext-syntax.bat` at **exit 1**, one
FAIL, attributed to the peer's `IntelFbPresentProbe.hpp` -- parsed by nothing,
since it is not dependency-free so section 1 skips it and no section-2 source
included it. It now exits 0 with zero FAIL lines.

Nothing I did fixed that. `handoff18.md` §3's rule: a gate's verdict in a
two-agent tree is a JOINT measurement -- red does not mean you broke it, green
does not mean you fixed it. The Intel side settled; the reading is recorded, the
credit is not claimed.

### 4b. The count closes exactly

`3050-handoff42.md` §5 recorded Python at **1219**. handoff43's Python gate was
RED (eight failures, seven peer-attributed and one payload staleness), so it
recorded no clean total to compare against.

The new module alone is **26** tests, measured by running it alone BEFORE the
gate rather than by subtracting totals. 1219 + 26 = **1245**, which is the gate's
own reading. Nothing else moved.

That is `3050-handoff42.md` §5a's method: attribute per module, because a total
difference is one number that can absorb two changes of opposite sign.

### 4c. Three Python readings, and the middle one was RED

The Python gate was run three times across this session. All three are recorded
because the first would otherwise have published a green that was already false.

| # | Reading | Attribution |
|---|---|---|
| 1 | exit 0, **1245 tests**, `OK` | The measurement §4's table carries |
| 2 | **exit 1**, **1256 tests**, 1 FAIL + 9 ERROR | **All ten the peer's.** Nine `ERROR:` in `tests/test_intel_fb_present_shape.py`, one FAIL naming `phase0-kext/IntelFbPresentProbe.cpp` |
| 3 | exit 0, **1257 tests**, `OK` | The peer fixed their own module and rebuilt the payload |

Reading 2 landed **thirteen minutes after this handoff was first written**, on a
tree this session had not touched since reading 1. The nine ERRORs were one
defect: `IndexError: no such group` raised inside the peer's own `audit()`, a
backreference to a capture group their regex does not define. The one FAIL was
`test_every_shipped_file_is_byte_identical_to_the_tree`, and it was correct about
the world -- their `IntelFbPresentProbe.cpp` was newer than `build-payload/`.

Nothing was touched on either count. `handoff18.md` §3's rule held exactly:
reading 3 shows the peer's module green at 15:59:45 and the payload rebuilt at
16:00:20, both by them. Rebuilding the payload myself would have stamped their
half-landed `.cpp` into an image under this session's name, and the red would have
gone away for the wrong reason -- `intel-handoff27.md` §5a, arrived at a third
time.

The durable half is about reading 1 rather than reading 2: **a gate reading has a
timestamp, and a handoff published on one is stale the moment a concurrent agent
writes.** This is the first time in this track the rule applied to a gate going
red AFTER the handoff was written rather than before.

### 4d. All five re-read after the peer settled

The other four gates were re-run too, for the same reason: the peer wrote
`phase0-kext/IntelFbPresentProbe.cpp` at 15:55:14, which is AFTER §4's syntax and
C++ readings, so those greens were about an older tree. Re-measured, exit status
captured before any pipe:

| Gate | Final reading |
|---|---|
| `tests\run-python-tests.bat` | exit 0, **1257 tests**, `OK` |
| `python tools/check-registers.py` | **242 of 242**, 0 failed, 0 skipped, exit 0 |
| `python tools/check-documented-paths.py` | **134** paths accounted for, exit 0 |
| `tests\check-kext-syntax.bat` | exit 0, `ALL SYNTAX GATES PASSED` |
| `tests\build-msvc.bat` | exit 0, **100% tests passed, 0 failed out of 12** |

`tests/test_documented_paths.py` was also run alone -- **33 tests, `OK`** -- which
is the module that actually resolves path tokens in root `.md` files, so it is the
one this file's own new tokens go through. `tools/check-documented-paths.py` reads
only `CLAUDE.md` (`DOC_NAME` at `:128`), measured rather than assumed, so its
unmoved 134 says nothing at all about this handoff and should not be read as
coverage of it.

One reading trap, measured here rather than inherited: `grep -c FAIL` over the
syntax gate's log returns **1** on a fully passing run, because line 255 is the
section title *negative control -- each defect must FAIL*. That is `handoff24.md`
§3's warning confirmed on this tree. Read the exit code, and if you grep, read the
MATCHED LINE rather than the count.

---

## 5. Why no rebuild is owed, and why one already happened

`3050-handoff43.md` §6 deliberately did NOT rebuild `build-payload/`, although
its own shipping-code change owed one -- because the tar packages the whole of
`phase0-kext/`, which then contained a peer header the syntax gate refused to
parse, and rebuilding would have stamped it into an image under that session's
name. §7 item 1 carried the debt.

That debt is **already paid, by somebody else, twice**. `build-payload/` was at
2026-08-20 **12:15:57** when this session opened -- after handoff43's writes and
before my first -- and it was rebuilt again at **16:00:20** by the peer, once
their own `IntelFbPresentProbe.cpp` (15:55:14) had settled. That second rebuild is
what cleared §4c reading 2's payload FAIL.

The current image is `build-payload/kext-src.tar` md5
`79188866f5d95f5534e07946e63ce1af` (532480 bytes) and
`build-payload/step7-payload.iso` md5 `8d6e4096986ef82f173fbec92c2acbfd` (610304
bytes). Both are recorded here as md5 rather than size on purpose:
`a-byte-size-is-not-a-fingerprint`, because tar pads to 10240.

This session owes nothing on top of either rebuild. The only `phase0-kext/` write
is §3's mutation-and-restore, and `NV3050Probe.cpp` still hashes to
`b08e84b3a8dc359fc70b7b1e4d36b542` -- the same value §3 records on both sides of
the mutation -- so the NVIDIA half of that tar is correct whichever of the two
images you are holding. The Python gate's payload comparison agrees, and it
compares bytes rather than mtimes.

Note for whoever next holds this: `OPERATOR-ACTION-step7-build.md` was NOT updated
for the 16:00:20 rebuild, and updating it is not mine to do -- the change it would
be recording is the peer's Intel work. Check it against the two md5s above before
trusting the number in it.

---

## 6. What this does NOT establish

1. **The gate is structural.** It parses comment- and string-stripped source and
   executes nothing. A future edit that keeps every marker while changing what an
   arm MEANS passes.
2. **`publishFirmwareState()` is unchanged.** Nothing was broken; this fixed
   nothing. It pins the shape of a function that was already correct, which is
   the whole point -- handoff43's fix now has an observer.
3. **The predicate is spelled against key NAMES.** Rename
   `NVUsableFramebufferMB` in the publisher and the module REFUSES rather than
   passing silently (that refusal is itself asserted, because a gate reporting
   nothing when its anchor moves is byte-indistinguishable from a green one) --
   but a SECOND framebuffer-size key added later under a different name is
   invisible to it. That is the vacuity direction and it is the honest limit,
   same shape as `3050-handoff30.md` §5 item 3.
4. **`readUsableFbSize()` is still reachable only by parse.** What executes on
   the host is `nvFbSizeVerdict()` in `tests/test_decode.cpp` -- the decision,
   not the read around it. handoff43 §2b's store-ordering fix remains verified by
   reading rather than by running.
5. **The kext has still never been loaded, matched, or run.** No suite executes a
   line of `NV3050Probe.cpp`.
6. **Nothing here moves §0's offload work forward by any distance.**

---

## 7. Open, highest value first

Each was measured this session rather than carried forward, except where noted.

1. **`pollGfwBoot()` has the same store-then-test shape at its tail** -- it
   writes `*outProgress` before returning false. Carried from
   `3050-handoff43.md` §7 item 3 and still open. Milder than §1's defect, because
   progress is a diagnostic rather than a measurement claim and the caller
   publishes it on both arms deliberately, but it is the identical construction
   and it deserves a decision rather than an accident. Cheapest remaining work.
   **DONE 2026-10-30.** Decided in code rather than left to accident:
   `pollGfwBoot()` now returns an `NVGfwBootVerdict` instead of a bool. The
   UNREADABLE arm (an all-ones read) deliberately does NOT write
   `*outProgress`; the TIMEOUT arm does, because the register decoded and the
   value is a real measurement of how far GFW got. The publisher puts
   `NVGfwBootProgress` in the registry only under
   `nvGfwBootProgressWasRead(gfwVerdict)`, and `NVGfwBootOk` is an equality
   against the accepting verdict via `nvGfwBootVerdictIsOk()`. Guarded by
   `tests/test_probe_firmware_state_publication.py`, now 40 tests (up from the
   26 this file reports), green this session, including negative controls for
   a deleted or negated progress guard and for a guard spelled with `IsOk`.
   The change first appears in git at baseline `8d08c7b` (2026-09-26), after
   this file was written. Verified by reading and by the source-level gate,
   not by running `pollGfwBoot()`: the kext has still never been loaded.
   Local edit only, committed on 2026-10-30.
2. **`CLAUDE.md`'s read-order list stops at `3050-handoff37.md`.** Six NVIDIA
   sessions are now unlisted (38 through 43, and this file makes seven). This
   cost handoff43 a wrong first read -- a stale handoff opened as current -- and
   cost this session a six-handoff-stale opening plan for the same reason. Not
   fixed here for the third session running, and the reason is unchanged: the
   file is shared with the concurrent Intel agent and editing it during their
   session invites the collision `intel-handoff26.md` §8 records. Worth one
   careful pass by whoever has the file uncontended, and it is now the item with
   the clearest measured cost.
   **DONE 2026-10-29.** `CLAUDE.md`'s read-order list now names
   `3050-handoff44.md` as the NVIDIA file to read first and lists handoff38
   through handoff43 as direct continuations between it and handoff37. Made
   with the file uncontended: its last commit is the Intel `74d010f` of
   2026-10-12, which matches its modification time, and no Intel PR is open.
   Local edit only, committed on 2026-10-30.
3. **`test_intel_scanout` still has no `MEASURED` provenance entry**, for the
   sixth session. Adding one would vouch for a C++ suite this session did not
   author, though note the C++ gate now reads 12 of 12 green.
   **DONE 2026-10-30 -- already closed; nothing added.** The entry exists:
   `"test_intel_scanout": 61` is line 1402 of `tests/test_check_count_floor.py`,
   inside the `MEASURED` dict (lines 1289 to 1415), and its provenance comment
   near line 677 records the first reading of 2026-08-19, 61 checks, 0
   failures. The suite is also built: `tests/CMakeLists.txt` line 138 registers
   it with `add_kext_test`. `git log -S` finds the name in both files only at
   baseline `8d08c7b` (2026-09-26), so git cannot date the entry more closely.
   Re-measured this session: `tests/build/test_intel_scanout.exe` (built
   2026-09-27, newer than the suite and `phase0-kext/IntelScanout.hpp`, both
   2026-08-19) exits 0 with `61 checks, 0 failures`, equal to the floor. The C++
   gate's 12 of 12 was not re-run. No source or test file changed; this note is
   the only edit, local and committed on 2026-10-30.
4. **The Python count's missing test in
   `tests/test_probe_boot42_log_arms.py`** (`3050-handoff42.md` §5a) -- it
   collects 19 where handoff41 §4 reports 20. The module is green and its
   positive control passes, so what is open is only WHICH check was lost, and
   that is unrecoverable without a previous revision in a tree that is not a git
   checkout.
   **DONE 2026-10-30.** Not one lost check but four lost properties. Measured
   against `3050-handoff41.md` §2 and §4 rather than by guessing at the
   twentieth name: the module carried none of handoff41 §2 items 2, 3 and 6,
   and only the header half of item 5 -- the bare-`return;` count, the
   dead-read-before-fall-through ordering, the log-only rule, and the rule that
   the shell must not grow its own `nvChipVerdict()`. Three of the nine negative
   controls handoff41 §4 tabulates went with them (`NVLOG` replaced by
   `setProperty`, the verdict defined in the shell, the deleted dead-read
   `return;`). All four are restored in `audit()` as properties 6 through 9,
   re-measured against `decodeBoot42()` as it stands, each with one negative
   control that asserts its own message AND that nothing else reddened. The
   module now collects **23**, green this session. 23 is not handoff41's 20;
   handoff42 §5a's taxonomy mismatch still holds and the count was not forced to
   match. Only `tests/` changed, so `phase0-kext/` owes no `build-payload/`
   rebuild. Local edit only, committed on 2026-10-30.
   **Full Python gate, re-run after this edit (log 2026-10-30 16:29):**
   `tests\run-python-tests.bat` exit 1, **1442 tests**, `FAILED (failures=14)`,
   0 skipped. None of the 14 is this session's: all 23 boot42 tests read `ok` in
   that run, and no failure detail names `tests/test_probe_boot42_log_arms.py`,
   `CLAUDE.md` or this file. What is red, all in files this session did not
   touch: 69 C++ suites under `tests/` with no CMake registration, no `MEASURED`
   entry and no count-gate listing (the `test_nv_gsp_*`, `test_nv_ampere_*` and
   `test_intel_gt_*` families among them), one of which,
   `test_intel_display_transaction_lifetime.cpp`, makes 7 check-helper calls
   against a floor of 20; `IntelDisplayTransactionLifetime.hpp` absent from the
   header gate's section 1; `build-payload/` stale (shipped files differ from
   the tree and sources are missing from it); `check-prepare-balance` at 16
   failures across 74 checks in `phase0-kext/`; `publishPlaneLiveObservation`
   never calling `igSampleVerdict()`, with the shape gate's
   `IntelDisplayProbe::publishPlane` mutation anchor now matching twice;
   `phase0-kext/iggt-blit.cpp` missing from the kext Makefile's source list and
   not an IOKit shell; and `NVGspBooterHs.hpp` citing a
   `nouveau/extract-firmware-nouveau.py` that does not resolve. Against
   `.pygate-baseline.log` (2026-09-27: 1407 tests, `failures=14, skipped=27`)
   the equal count hides a changed set: seven baseline failures are gone and
   seven new ones appeared. `test_intel_scanout` is not among the 69 suites
   lacking a `MEASURED` entry, so item 3 above may already be closed; not
   checked further here.
   **Cited-path failure closed 2026-10-30.** `NVGspBooterHs.hpp` line 5 names
   `nouveau/extract-firmware-nouveau.py` as the source of truth for the
   `nvfw_hs_header_v2` Booter layout. The only copy on disk is under
   `reference/open-gpu-kernel-modules/`, which `.gitignore` line 2 excludes
   (`git check-ignore -v` exit 0; `git ls-files` lists nothing under its
   `nouveau/` directory), so a clean checkout has no file to resolve to. The
   comment is true and the tool is upstream's, so this is an allowance, not a
   defect -- the same call `handoff15.md` made for the bare
   `extract-firmware-nouveau.py` cited at line 40 of `NVRiscvUcode.hpp`. The
   directory-qualified token is now its own `ALLOWED` entry in
   `tests/test_source_cited_paths.py`, with that reason; nothing was vendored.
   That module alone, on `agent/handoff44-section7-closeout`: 5 tests, OK. This
   note and the `ALLOWED` entry landed together in `8a46077`; not pushed.
   **Full Python gate re-run at `8a46077`:** `Ran 1442 tests`,
   `FAILED (failures=13)`, down from 14, and none of the 13 is in
   `test_source_cited_paths`. By module: `test_cmake_registration` 2,
   `test_check_count_floor` 2, `test_kext_makefile` 2, `test_guest_build_kext` 2,
   `test_intel_publisher_sample_shape` 2, `test_check_count_last_line` 1,
   `test_check_header_coverage` 1, `test_check_prepare_balance` 1. All 13 stay
   open; this session did not touch them.
   **Registration gap narrowed 2026-11-01 (`c6e07bb`).** 35 of the 69
   unregistered suites, the ones whose `main()` already ends on an exact count
   `printf`, are now in `tests/CMakeLists.txt`, in `SUITES` in
   `tests/test_check_count_last_line.py` and in `MEASURED` in
   `tests/test_check_count_floor.py`, each floor taken from a first reading with
   g++ 16.1.0 on 2026-11-01. ctest now builds and runs 62 suites. The
   count-last-line gate also lists the new `checks, failures` users and accepts
   `return gFailures ? 1 : 0;` alongside `return gFailures == 0 ? 0 : 1;` (same
   exit status). Building them surfaced MSVC C4310 (level 4) at
   `phase0-kext/NVGspWprImage.hpp` line 140: the complemented known-flags mask
   was cast down to a byte, truncating a constant. The one-byte `flags` is now
   widened instead; the verdict is unchanged for every input. Readings after the
   last edit: `tests\build-msvc.bat` exit 0, 62 of 62 passed, 0 C4310 lines;
   `tests\run-python-tests.bat` `Ran 1442 tests`, `FAILED (failures=13)`, the
   same 13 test methods as at `8a46077`. Still open: the other 34 suites, whose
   final line is not a count format (`test_intel_gt_fill` also needs
   `phase0-kext/IntelGtBlitter.cpp` linked; a trial g++ link with it compiled
   and ran clean). `phase0-kext/` changed, so this adds to the `build-payload/`
   staleness `test_guest_build_kext` already reports; not rebuilt, and the
   header edit was not built in the guest. Hygiene only: the offload distance
   moves by zero and the kext has still never been loaded. Not pushed.
   **Three WPR suites registered 2026-11-01 (`6045861`).**
   `test_nv_gsp_wpr_candidate`, `test_nv_gsp_wpr_ga107_facts` and
   `test_nv_gsp_wpr_sysmem` were unregistered only because `main()` ended on a
   labelled count line (`GA107 WPR candidate: ...`, `GA107 WPR facts: ...`,
   `WPR sysmem bridge: ...`). The label is gone: each now prints the exact
   `"\n%d checks, %d failures\n"` line, and their existing `gFailures ? 1 : 0`
   tails already pass the count-last-line gate (`test_nv_gsp_wpr_ga107_facts`
   writes it `gFailures?1:0`, which the whitespace strip folds in). No Python
   gate matched the old labels. All three are now in `tests/CMakeLists.txt`, in
   `SUITES` and the `checks, failures` list in
   `tests/test_check_count_last_line.py`, and in `MEASURED` in
   `tests/test_check_count_floor.py`, each floor taken from a first g++ 16.1.0
   reading on 2026-11-01: 45, 38 and 35 checks, 0 failures, zero warnings. The
   new CMakeLists comment quotes the old labels with single quotes, because a
   first draft with double quotes tripped `test_no_quoted_hash_in_cmakelists`.
   Readings after the last edit: `tests\build-msvc.bat` exit 0, 65 of 65
   passed; `tests\run-python-tests.bat` `Ran 1442 tests`,
   `FAILED (failures=13)`, the same 13 test methods as at `c6e07bb`. Still
   open: the other 31 suites, whose final line is not a count format. Only
   `tests/` changed, so the `build-payload/` staleness is unchanged. Hygiene
   only: the offload distance moves by zero and the kext has still never been
   loaded. Not pushed.
   **Three Ampere suites registered 2026-11-01 (`c066cf7`).**
   `test_nv_ampere_channel_runtime`, `test_nv_ampere_copy_publication_bridge`
   and `test_nv_ampere_copy_publication_runtime` were unregistered only
   because `main()` printed `"%u checks, %u failures\n"` from `unsigned`
   counters and returned `gFailures == 0u ? 0 : 1`, and the count-last-line
   gate accepts neither. Each now prints the exact
   `"\n%d checks, %d failures\n"` line with `static_cast<int>` arguments (a
   C4477 lookup did not settle whether `%d` with an `unsigned` argument warns
   under MSVC) and returns `gFailures == 0 ? 0 : 1`. All three are now in
   `tests/CMakeLists.txt`, in `SUITES` and the `checks, failures` list in
   `tests/test_check_count_last_line.py`, and in `MEASURED` in
   `tests/test_check_count_floor.py`, each floor taken from a first g++ 16.1.0
   reading on 2026-11-01: 164, 30 and 182 checks, 0 failures, zero warnings.
   Readings after the last edit: `tests\build-msvc.bat` exit 0, 68 of 68
   passed; `tests\run-python-tests.bat` `Ran 1442 tests`,
   `FAILED (failures=13)`, the same 13 test methods as at `6045861`. Still
   open: the other 28 suites, whose final line is not a count format. Only
   `tests/` changed, so the `build-payload/` staleness is unchanged. Hygiene
   only: the offload distance moves by zero and the kext has still never been
   loaded. Not pushed.
   **Two NVIDIA suites registered 2026-11-01 (`1ae367d`).**
   `test_nv_ga107_pci_identity` and `test_nv_gsp_rpc_receive_tracker` were
   unregistered only because `main()` printed `"%u checks, %u failures\n"`
   from `unsigned` counters, which the count-last-line gate does not accept.
   Each now prints the exact `"\n%d checks, %d failures\n"` line with
   `static_cast<int>` arguments and keeps its `return failures?1:0;` tail,
   which the tail pattern in `tests/test_check_count_last_line.py` now accepts
   next to `gFailures==0?0:1` and `gFailures?1:0`. Both are now in
   `tests/CMakeLists.txt`, in `SUITES` and the `checks, failures` list in
   `tests/test_check_count_last_line.py`, and in `MEASURED` in
   `tests/test_check_count_floor.py`, each floor taken from a first g++ 16.1.0
   reading on 2026-11-01: 24 and 325 checks, 0 failures, zero warnings.
   Neither calls a `check[A-Z]*` helper, so `SourcesStillCallChecks` counts 0
   for both; that test was already red at `c066cf7`. Readings after the last
   edit: `tests\build-msvc.bat` exit 0, 70 of 70 passed;
   `tests\run-python-tests.bat` `Ran 1442 tests`, `FAILED (failures=13)`, the
   same 13 test methods as at `c066cf7`. Still open: the other 26 suites,
   whose final line is not a count format. Only `tests/` changed, so the
   `build-payload/` staleness is unchanged. Hygiene only: the offload distance
   moves by zero and the kext has still never been loaded. Not pushed.
   **Three NVIDIA suites registered 2026-11-01 (`f5110c1`).**
   `test_nv_gsp_booter_success_retirement_plan`,
   `test_nv_intel_shared_surface_handoff_anchored_provenance` and
   `test_nv_intel_shared_surface_handoff_provenance` were unregistered only
   because `main()` printed `"checks=%u failures=%u\n"` (the booter suite) or
   `"%u checks, %u failures\n"` (the two handoff suites) from `unsigned`
   counters, with a `gFailures == 0u` tail. Each now prints the exact
   `"\n%d checks, %d failures\n"` line with `static_cast<int>` arguments and
   returns `gFailures == 0 ? 0 : 1`. All three are now in
   `tests/CMakeLists.txt`, in `SUITES` and the `checks, failures` list in
   `tests/test_check_count_last_line.py`, and in `MEASURED` in
   `tests/test_check_count_floor.py`, each floor taken from a first g++ 16.1.0
   reading on 2026-11-01: 59, 44 and 67 checks, 0 failures, zero warnings.
   Registering them exposed three MSVC-only faults that the g++ readings did
   not show. The booter suite opened its fixture through a cwd-relative path,
   and ctest runs it from `tests/build`, so MSVC failed it on `fixture open`.
   It now opens the fixture through `NV_FIRMWARE_DIR`, which
   `tests/CMakeLists.txt` injects. A fallback keeps a hand-built binary run
   from the repo root working. `phase0-kext/NVGspBooterPatch.hpp` used
   `__UINTPTR_TYPE__`, which cl.exe does not have (C4430/C2146).
   `NVGspBooterPatchPtr` now falls back to `unsigned __int64` on `_WIN64` and
   `unsigned int` otherwise, and any other compiler without it stops at
   `#error`. `phase0-kext/NVGspBooterRunPlan.hpp` now passes the 32-bit
   `hs.firmwareBytes`, already proven equal to `privateImageBytes`, to the
   three `nvGspBooterHsRegionFits` calls. That clears C4244, and the verdict is
   unchanged for every input. Readings after the last edit:
   `tests\build-msvc.bat` exit 0, 73 of 73 passed, no compiler error or
   warning lines; `tests\run-python-tests.bat` `Ran 1442 tests`,
   `FAILED (failures=13)`, the same 13 test methods as at `1ae367d`. Still
   open: the other 23 suites, whose final line is not a count format. This
   commit also changed two `phase0-kext/` headers, and `build-payload/` was
   not rebuilt. Hygiene only: the offload distance moves by zero and the kext
   has still never been loaded. Not pushed.
   **Six Intel display/plane and GSP booter suites registered 2026-11-01
   (`35ec60b`).** `test_intel_display_transaction_lifetime`,
   `test_intel_plane_program_plan`, `test_intel_plane_retirement_evidence`,
   `test_intel_plane_transition_plan`, `test_nv_gsp_booter_exec_sequence_plan`
   and `test_nv_gsp_booter_recovery_plan` were unregistered only because
   `main()` ended on a labelled `"all %u ... checks passed"` or
   `"... %u checks passed"` line from `unsigned` counters, after a
   `"%u/%u ... checks failed"` early return. Each now prints the exact
   `"\n%d checks, %d failures\n"` line with `static_cast<int>` arguments and
   returns `gFailures == 0 ? 0 : 1`. All six are now in
   `tests/CMakeLists.txt`, in `SUITES` and the `checks, failures` list in
   `tests/test_check_count_last_line.py`, and in `MEASURED` in
   `tests/test_check_count_floor.py`, each floor taken from a first g++ 16.1.0
   reading on 2026-11-01: 119, 127, 116, 77, 67 and 141 checks, 0 failures,
   zero warnings. The two GSP booter suites now open their firmware fixtures
   through `NV_FIRMWARE_DIR`, which `tests/CMakeLists.txt` injects, because
   ctest runs them from `tests/build`. Readings after the last edit:
   `tests\build-msvc.bat` exit 0, 79 of 79 passed, no compiler error or
   warning lines; `tests\run-python-tests.bat` `Ran 1442 tests`,
   `FAILED (failures=13)`, the same 13 test methods as at `de3e641`.
   `test_each_suite_calls_check_helpers_many_times` still stops on
   `test_intel_display_transaction_lifetime.cpp`: 7 static check-helper calls
   against a floor of 20, although the suite reports 119 checks at run time,
   so registering it did not close that failure. Still open: the other 17
   suites, whose final line is not a count format. Only `tests/` changed, so
   the `build-payload/` staleness is unchanged. Hygiene only: the offload
   distance moves by zero and the kext has still never been loaded. Not
   pushed.
5. **One `kextload` on a machine containing the card.** Unchanged, and still the
   single highest-value action in the whole project. Blocked on hardware, on SIP,
   and on `3050-handoff27.md` §1's mutual exclusion: the laptop cannot hold Linux
   and macOS at once.
