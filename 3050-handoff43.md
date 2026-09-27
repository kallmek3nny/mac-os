# 3050-handoff43 -- the framebuffer size a refused read published as a measurement

NVIDIA GA107 track. Continues `3050-handoff42.md`. Written 2026-08-20.

This session found and closed a **live defect in shipping code**: the eighth
occurrence of `handoff18.md` §1d's fabricated-publication class, and the fifth in
`phase0-kext/NV3050Probe.cpp`. Shipping code CHANGED, so a `build-payload/`
rebuild is owed -- and §6 is the argument for why this session deliberately did
not perform it.

---

## 0. What the operator asked for, and what this is not

The standing request is "continue making the driver render, then use the Intel
driver to output". That is muxless PRIME-style offload and it is architecturally
the right shape -- it is also, in this tree, unreachable, and saying so is a
standing obligation rather than a caveat. Offload needs, in order: GSP firmware
actually booting on real silicon; a channel and pushbuffer implementation; an
IOAccelerator stack; a shared-surface allocator; and an Intel framebuffer able to
inherit a foreign buffer. **None of the first four exist even in skeleton**, and
the kext has never been `kextload`ed. `PLAN.md` caps this project at Phases 0-5
for exactly that reason.

`3050-handoff42.md` §7 item 1 exists to stop a future session presenting
driver-hygiene work as progress toward offload. This section is the same guard.
The work below is hygiene: it makes the IORegistry stop lying about one word. It
moves the offload distance by zero.

The irony is worth carrying. `3050-handoff37.md` §0 measured that the GA107 VBIOS
declares **zero display paths** -- `entry_count` is `0x00` in a well-formed DCB --
so offload is the only conceivable route to a picture on that laptop, and it is
the route with no layers built.

---

## 1. The defect

`readUsableFbSize()` reads `NV_USABLE_FB_SIZE_IN_MB` and masks it with
`NV_USABLE_FB_SIZE_IN_MB_VALUE_MASK`. That mask is `0xFFFFFFFF`.

The function stored the masked value into the caller's out-parameter **before**
any of its three refusal tests, then returned `false`. So a refused read left
**4294967295** in `fbSizeMB`, and nothing about that number looks refused --
masking a refusal with an all-ones mask returns the refusal unchanged.

`publishFirmwareState()` then guarded publication with `if (fbSizeMB > 0)`.
4294967295 is greater than zero, so `NVUsableFramebufferMB` went into the
IORegistry from a word the driver had **itself just diagnosed as not decoding**
and written a `NVLOG` line about. `setProperty()` with a fabricated value is
byte-identical to `setProperty()` with a measured one -- `handoff18.md` §1d's
sentence, for the eighth time.

### 1a. Why the magnitude guard looked like a guard

`if (fbSizeMB > 0)` reads as a real test, and it is the reason this survived
review. It genuinely rejects one of the three refusals -- the
firmware-has-not-published-yet case reads 0 and is correctly withheld. It rejects
neither of the other two. A guard that is correct on one arm of three is worse
than no guard, because it looks like the question was considered.

### 1b. The `!gfwOk` path was protected only by accident

When GFW boot does not complete, `readUsableFbSize()` is never called, so
`fbSizeMB` still holds the `= 0` its declaration gave it, and the magnitude guard
withholds the key. That is correct behaviour arrived at by an initialiser rather
than by a decision. Move the declaration, or give it any non-zero default, and the
healthy-looking path publishes a fabricated size. This is
`3050-handoff29.md` §2's shape one subsystem over: the code was right for a
reason nobody had written down.

### 1c. Why the four gates were blind

The familiar four, re-derived rather than inherited:

1. **The register gate** (`tools/check-registers.py`) checks constants against
   NVIDIA's headers. Both constants are CORRECT. The relation between a mask and
   a caller's magnitude test is not a constant.
2. **The C++ suites** cannot compile `NV3050Probe.cpp` -- it reaches IOKit, which
   does not exist off macOS. Both the defective store and the defective guard
   lived there.
3. **`-Werror`** has nothing to bite on. `*outMegabytes = mb;` is a well-typed
   store and `fbSizeMB > 0` is a well-typed comparison.
4. **The Python gate** models publication SHAPE for four other families
   (`GSPEngineReadable`, `NVIdentityReadable`, `NVBoot42Readable`,
   `NVGspBootReadMask`). Nothing modelled this one. §7 item 2.

There is a fifth blindness specific to this defect and it is the durable one:
**`nvFbSizeIsPlausible()` had direct coverage in `tests/test_decode.cpp` and five
green checks over it, including one asserting that `0xFFFFFFFF` is implausible.**
The predicate knew. The caller consulted it only after storing. That is
`3050-handoff31.md` §1a's tested-predicate-with-a-caller-that-ignores-it, and it
is now the third file in which this tree has found that exact shape.

---

## 2. The fix, and the four choices in it that are load-bearing

A verdict family, `NVFbSizeVerdict`, added to `phase0-kext/NVDecode.hpp`.

**The verdict lives in the dependency-free header, not in the shell.** A pure
function written in an IOKit shell is a pure function no host suite runs --
`CLAUDE.md`'s whole architectural argument, and `IntelDisplayNames.hpp` is what
it looks like ignored and then repaired. Putting `nvFbSizeVerdict()` in
`NVDecode.hpp` is what makes §3 possible at all.

**A verdict, not a bool.** `3050-handoff34.md` §1: `== false` is satisfied by
every refusal at once, so a bool cannot distinguish a fix from the bug it fixed.
Three refusals here -- unreadable, unpublished, implausible -- call for three
different operator actions, and only the first is a fact about the driver.

**`NV_FB_SIZE_NOT_ATTEMPTED` is the ZERO value, and `nvFbSizeVerdict()` never
returns it.** It is the caller's initialiser and nothing else. An unassigned
verdict variable therefore reads as "nobody looked" rather than as a good value.
That is the `NV_CHIP_UNREADABLE = 0` convention, and it is what makes §1b's
accidental correctness deliberate: the `!gfwOk` path now publishes
*not attempted -- GFW boot did not complete* because the enum's zero SAYS that,
not because a magnitude test happened to withhold a key.

**Arm ORDER is load-bearing.** `0xFFFFFFFF` is ALSO implausible and `0` is ALSO
implausible, so an implausibility-first order makes two arms unreachable --
`3050-handoff32.md` §2a's dead-arm shape and `3050-handoff33.md` §2's Ada-first
rule, arrived at a third time. Specific arms run first; the plausibility test runs
last. Two of §3's checks are ORDER pins rather than value pins for this reason.

### 2a. Publication

`publishFirmwareState()` now takes the verdict rather than a bool, and:

- `NVUsableFramebufferVerdict` and `NVUsableFramebufferMeasured` are published
  **FIRST and on EVERY path**. Verbatim the reasoning behind the four existing
  readable-flag families: an ABSENT key is a question rather than an answer,
  indistinguishable from an older kext that never published it.
- `NVUsableFramebufferMB` is guarded by an **EQUALITY against
  `NV_FB_SIZE_OK`** -- never a magnitude test, and never a negation of one error
  arm. `fbVerdict != NV_FB_SIZE_UNREADABLE` is true for UNPUBLISHED and for
  IMPLAUSIBLE, so a negation reintroduces the defect through two of the three
  doors it came in by. `3050-handoff33.md` §2's `NVPartIsTarget` reasoning, one
  subsystem over.

### 2b. The store moved to the accepting arm

`readUsableFbSize()` writes `*outMegabytes` **only** on `NV_FB_SIZE_OK`. That
single ordering change is the fix; everything else is what makes the fix hold and
what makes it visible in the registry.

---

## 3. Coverage, where it can actually execute

`testFbSizeVerdict()` in `tests/test_decode.cpp`, which compiles the identical
header under MSVC. Four properties:

1. **Every arm reached** -- 4096 and 6144 to OK, all-ones to UNREADABLE, zero to
   UNPUBLISHED, 128 and 1 TiB to IMPLAUSIBLE; plus `NOT_ATTEMPTED == 0` and
   `VERDICT_LIMIT == 5`.
2. **Two ORDER pins** -- `nvFbSizeIsPlausible()` is asserted false for BOTH
   `0xFFFFFFFF` and `0`, which is the evidence that those two values satisfy the
   implausible arm as well. An implausibility-first subject returns IMPLAUSIBLE
   for both, and exactly those checks move.
3. **Names** -- the value one past `VERDICT_LIMIT` answers the sentinel; every
   verdict below the limit differs from it; all names pairwise distinct. The
   pairwise clause is `handoff21.md` §1's lesson: a copy-paste arm returning a
   sibling's string satisfies every has-a-name assertion.
4. **NOT_ATTEMPTED is never produced** -- twelve probe values spanning both
   boundaries and both sentinels, each asserted to differ from it. This is the
   claim §2's zero-value convention rests on, so it is asserted rather than
   assumed.

`test_decode` 148 -> **186**. The ceiling was 148 + max(8, 18) = 166, so the count
band in `tests/test_check_count_floor.py` **FIRED correctly** by twenty, and the
table entry carries a DATED provenance reading rather than a bare number --
`handoff17.md` §1's rule, since a dict entry edited to match a binary satisfies
the floor relation identically to a real measurement.

---

## 4. Gate readings

Every reading below is from this session, taken after the last edit. Exit codes
were captured **before any pipe** -- `$?` after a pipeline is the last command's
status, which has produced a false green in this tree before.

| Gate | Reading | Attribution |
|---|---|---|
| `tools/check-registers.py` | **242 of 242**, 0 failed, 0 skipped, exit 0 | Unmoved by me, correctly -- a verdict enum is not a `#define`, so the register gate has nothing to say about it |
| `tools/check-documented-paths.py` | **134** paths accounted for, exit 0 | Green |
| `tests/build-msvc.bat` | exit 0, all suites, `test_decode` **186/186** | Green |
| `tests/check-kext-syntax.bat` | **exit 1**, one FAIL | **Peer's.** `IntelFbPresentProbe.hpp` is parsed by nothing -- not dependency-free so section 1 skips it, and no section-2 source includes it |
| `tests/run-python-tests.bat` | **exit 1**, eight failures | **Seven peer, one mine** (payload staleness) |

### 4a. A reading error of mine, corrected

Earlier in this session I attributed the peer's tree as "four hours quiet" and
used that to justify re-running two gates as though the tree had settled. That
comparison was **a UTC clock against local mtimes**. The peer's last write is
11:17:18 local = 15:17:18 UTC, against a 15:49:59 UTC clock: **33 minutes**, not
four hours. An actively-written tree, not a settled one.

The conclusion I drew from the bad comparison happened to be harmless -- the
re-runs were real measurements and the C++ gate genuinely had gone green
underneath me. The reasoning was not. **A timestamp comparison across two zones
is not a measurement**, and this one was off by exactly the offset, which is the
direction that reads as "safe to proceed".

### 4b. A false green that a status would have produced

Running the C++ gate as a `cmd.exe /c` invocation reported `CPP_EXIT=0` over a
**137-byte, three-line** log containing a cmd banner and an empty prompt. The
batch never started; the 0 was cmd's own status. Invoking the batch directly from
`tests/` produced a 44-line build log and a genuine 0.

The tell is the ARTIFACT SIZE, not the status. Read what the gate wrote before
reading what it returned -- and a `grep` returning nothing over such a log reads
as "the suites did not run" when it actually means "there is no log".

### 4c. Attribution of the eight Python failures

Seven name the peer's half-landed Intel FB-present work, by these mtimes
(local): `phase0-kext/IntelPixel.hpp` 09:51:25, `tests/test_check_count_last_line.py`
09:54:46, `tests/CMakeLists.txt` 10:42:59, `phase0-kext/IntelFbPresent.hpp`
10:44:38, `phase0-kext/IntelFbPresentProbe.hpp` 11:17:18. The eighth is the
payload comparison, and it names my three files.

None were touched. `handoff18.md` §3's rule: a gate's verdict in a two-agent tree
is a JOINT measurement -- red does not mean you broke it, green does not mean you
fixed it. Attribute by mtime and by the failure MESSAGE first.

---

## 5. What changed on disk

- `phase0-kext/NVDecode.hpp` -- `NVFbSizeVerdict`, `nvFbSizeVerdictName()`,
  `nvFbSizeVerdict()`, plus the comment block recording why the bool could not
  carry the distinction and why arm order is load-bearing.
- `phase0-kext/NV3050Probe.hpp` -- both signatures.
- `phase0-kext/NV3050Probe.cpp` -- three sites: the read, the publisher, and the
  `start()` call site.
- `tests/test_decode.cpp` -- `testFbSizeVerdict()` plus runner wiring.
- `tests/test_check_count_floor.py` -- table 148 -> 186 and a dated provenance
  block.

---

## 6. Why `build-payload/` was NOT rebuilt

Shipping code genuinely changed, so a rebuild is owed. It was deliberately not
done, and this is the reasoning rather than an omission.

The rebuild tars the whole of `phase0-kext/`. That directory currently contains
`IntelFbPresentProbe.hpp`, written 33 minutes before the last reading and **the
subject of the syntax gate's one FAIL**. Rebuilding would package a header no gate
can parse into an image stamped with this session's name, and
`OPERATOR-ACTION-step7-build.md` would record it as a measured artifact.

`intel-handoff27.md` §5a is the precedent and it points the same way: the correct
action on a peer-attributed red is to attribute it and touch nothing, because
greening a gate by rebuilding vouches for a peer's half-landed work.
`3050-handoff29.md` §7 is the counter-case that defines the boundary -- there the
staleness was the session's own, sixty seconds old, and fixing it was right. Here
one of the two staleness sources is mine and the other is not, and a rebuild
cannot separate them.

So the payload gate's red is **expected and correct**, the thirty-third image is
still current, and §7 item 1 is the first thing for whoever picks this up.

Note for that person: a byte size is not a fingerprint. Tar pads to 10240, so an
image whose size does not move can still differ in content -- compare the md5.

---

## 7. Open, highest value first

1. **Rebuild `build-payload/` and update `OPERATOR-ACTION-step7-build.md`**, once
   the Intel side settles and the syntax gate is green. Owed by this session's
   own changes. §6 is why it waited.
2. **A shape gate for `publishFirmwareState()`.** Delete the equality guard and
   restore the magnitude test, and all five gates stay green -- the publisher
   lives in the shell, so no C++ suite compiles it. Same hole
   `3050-handoff30.md` filled for the GSP boot publisher, in the idiom of
   `tests/test_probe_gsp_boot_publication.py`. This is the cheapest remaining
   work and it is the reason §1's defect could exist.
3. **`pollGfwBoot()` has the same store-then-test shape at its tail** -- it writes
   `*outProgress` before returning false. Milder, because progress is a
   diagnostic rather than a measurement claim and the caller publishes it on both
   arms deliberately, but it is the identical construction and worth a decision
   rather than an accident.
4. **`CLAUDE.md`'s read-order list stops at `3050-handoff37.md`.** Five NVIDIA
   sessions are unlisted. That cost this session a wrong first read -- a stale
   handoff opened as current. Not fixed here because the file is shared with the
   concurrent Intel agent and editing it during their session invites the
   collision `intel-handoff26.md` §8 records.
5. **One `kextload` on a machine containing the card.** Unchanged, and still the
   highest-value action in the project. Blocked on hardware and on
   `3050-handoff27.md` §1's mutual exclusion: the laptop cannot hold Linux and
   macOS at once.

---

## 8. What this does NOT establish

- The kext has still never been loaded, matched, or run. The syntax gate parses
  it; nothing executes a line of `NV3050Probe.cpp`.
- `readUsableFbSize()` itself is still reachable only by parse. What
  `tests/test_decode.cpp` executes is `nvFbSizeVerdict()` -- the decision -- not
  the read around it. That split is deliberate and is the whole point of the
  header/shell architecture, but it means §2b's store-ordering fix is verified by
  reading, not by running.
- Nothing here moves the offload work of §0 forward by any distance.
