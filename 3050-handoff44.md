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
2. **`CLAUDE.md`'s read-order list stops at `3050-handoff37.md`.** Six NVIDIA
   sessions are now unlisted (38 through 43, and this file makes seven). This
   cost handoff43 a wrong first read -- a stale handoff opened as current -- and
   cost this session a six-handoff-stale opening plan for the same reason. Not
   fixed here for the third session running, and the reason is unchanged: the
   file is shared with the concurrent Intel agent and editing it during their
   session invites the collision `intel-handoff26.md` §8 records. Worth one
   careful pass by whoever has the file uncontended, and it is now the item with
   the clearest measured cost.
3. **`test_intel_scanout` still has no `MEASURED` provenance entry**, for the
   sixth session. Adding one would vouch for a C++ suite this session did not
   author, though note the C++ gate now reads 12 of 12 green.
4. **The Python count's missing test in
   `tests/test_probe_boot42_log_arms.py`** (`3050-handoff42.md` §5a) -- it
   collects 19 where handoff41 §4 reports 20. The module is green and its
   positive control passes, so what is open is only WHICH check was lost, and
   that is unrecoverable without a previous revision in a tree that is not a git
   checkout.
5. **One `kextload` on a machine containing the card.** Unchanged, and still the
   single highest-value action in the whole project. Blocked on hardware, on SIP,
   and on `3050-handoff27.md` §1's mutual exclusion: the laptop cannot hold Linux
   and macOS at once.
