# handoff25 — `NV_PMC_BOOT_42` was published on a path that never read it, and the gate I wrote to catch it reddened on correct code first

Written 2026-08-18. Continues `handoff23.md` on the NVIDIA track and closes the
first half of its §5 item 2. **This one changed shipping code**: two files in
`phase0-kext/` moved, which is why `build-payload/` was rebuilt and
`OPERATOR-ACTION-step7-build.md` records a TWENTY SIXTH image. No register
constant moved. No decoder changed. No state machine changed.

**The Intel boundary held for the whole session.** A separate agent owns that
track. No Intel header, shell, or suite was read or written at any point.

**On the number.** This file was written as `handoff24.md` and is now 25. The
Intel agent had already landed their own `handoff24.md` at 14:59 UTC — the
BAR-refusal consumer work `CLAUDE.md` line 44 describes — and my `Write` an
hour later destroyed it. It was recovered byte-for-byte from that session's
transcript and restored; this file took the next free number. The mistake is
handoff17 §5 exactly, in a tree with a concurrent peer rather than after a
compaction: **list a numbered directory before writing into it.** A `Write` to
a path you have not read is unconditional, and `handoff24.md` not being in MY
context said nothing about whether it was on disk.

---

## 1. The finding: a fabricated register in the IORegistry

This is the THIRD occurrence of the class handoff18 §1d names, and the SECOND
in this same shell. The shape is always the same: a function returns early
without performing a read, and something downstream publishes a number anyway.

Three separate pieces, none of which is a defect alone:

| Site | What it did |
|---|---|
| `readIdentity()` dead-read branch | `*outBoot42 = boot0;` then `return false;` |
| `start()` call site | `readIdentity(&boot0, &boot42);` — bool DISCARDED |
| `publishIdentity()` | `setProperty("NV_PMC_BOOT_42", boot42, 32);` unconditionally |

Join them and a register that was **never read** reached the IORegistry.
`NV_PMC_BOOT_42` lives at `0xA00`; on a dead aperture `readIdentity()`
deliberately skips it — correctly, so the log shows one clear failure instead of
two — and then handed `boot0` back through the out-parameter as if it were the
answer.

Why that is the bad direction: `setProperty("NV_PMC_BOOT_42", 0xFFFFFFFF, 32)`
is **byte-identical in IORegistryExplorer** to a `setProperty()` carrying a
register that genuinely returned `0xFFFFFFFF`. An operator sees two equal
all-ones words and cannot tell a dead aperture from a card that answered. It is
handoff18 §1d's sentence one level over: *an absent key is a question the
operator has to ask; a fabricated key is an answer they will believe.*

And `publishIdentity()` goes further than publishing the raw word. It derives
`NVArchitecture` from `nvBoot0Architecture(boot0)` — a decode of a value that is
not a chip ID returns a real string, so a machine whose BAR never decoded could
publish an architecture NAME.

### 1a. Why no gate could see it

Verbatim the four reasons handoff20 §1, handoff22 §1 and handoff23 §1 each give,
and they are worth restating because this is now the fourth time they applied:

- The register gate checks CONSTANTS. `NV_PMC_BOOT_42`'s offset is correct; the
  question is whether the value beside it was measured, which is not a constant.
- No C++ suite can link a translation unit that reaches `IOService`, so nothing
  executes a line of `NV3050Probe.cpp`.
- `-Werror` has nothing to say. `*outBoot42 = boot0;` is a well-typed store, and
  a discarded `bool` return is not a warning.
- Nothing in the Python gate modelled "the publication surface must be a subset
  of what was read". `test_probe_dead_read_publication.py` pins exactly one
  function, `inspectGspEngine()`, and stops there.

### 1b. The fix

Three edits, each of which is load-bearing on its own — a partial fix leaves the
defect intact by another road, which is why the gate below has a negative
control per piece.

1. `readIdentity()`'s dead-read branch no longer writes through `outBoot42` at
   all. The `false` return is the only honest signal, and now it is the only
   signal.
2. `start()` consumes that bool: `if (readIdentity(...)) publishIdentity(...);
   else publishIdentityUnreadable(boot0);`.
3. New `publishIdentityUnreadable()` publishes `NV_PMC_BOOT_0` (the one register
   actually read), `NVIdentityReadable false`, a reason string, the expected
   chip ID, and the PCI config words — which come from config space rather than
   BAR0 and are therefore still real. It publishes **no** `NV_PMC_BOOT_42` and
   derives **no** architecture name.

`NVIdentityReadable` is published on **both** paths, `true` and `false`. A flag
published only where things went wrong is indistinguishable from an older kext
that never published it, and absence would read as healthy. Same reasoning as
`GSPEngineReadable` in handoff18 §1d, and the gate pins it on both arms.

## 1c. The mistake: `[^;]*` crosses a statement boundary

The part worth carrying forward is that **my first version of the gate went RED
on the fixed tree**, and the failing test was the positive control. Third
consecutive handoff where that is the sentence, and the road in was new.

The call site must CONSUME `readIdentity()`'s bool, so I needed to recognise a
bare expression statement. I wrote:

```python
BARE_CALL_RE = re.compile(r"(?<![.\w])\breadIdentity\s*\([^;]*\)\s*;")
```

`[^;]*` matches anything that is not a semicolon — **including the closing paren
of the `if`, the newline, and the whole guarded body**. Against the FIXED source
it ran from `readIdentity(` past `&boot0, &boot42))`, past
`publishIdentity(boot0, boot42)`, and stopped at that statement's semicolon. So
the correct call site matched the pattern that defines the defect.

Generalisation, and it is the same family as handoff23 §1b one step over: **a
character class that excludes only the terminator is not scoped to the
statement.** The assertion was about a rendering of the code — a flat span of
text between two landmarks — rather than about its structure. Bareness is now
decided by `_discarded_calls()`, which walks BALANCED parens from the call's
opening paren and asks what the first non-space character after the matching
close is: `;` means discarded, `)` means the call sits inside an `if`.

If a structural claim can be stated as "the matching bracket", do not spell it
as "up to the next terminator".

### 1c-ii. A second red, and it was the test again

The eleventh test — the one asserting that comment prose alone cannot satisfy
the gate — also reddened on a pristine tree, for a duller reason worth one
sentence: it asserted the fix's comment contains the literal `outBoot42 =
boot0`, and the comment as actually written says outBoot42 is "left ALONE
rather than filled with boot0". The assertion was about wording I remembered
writing rather than wording on disk. It now asserts the property that matters —
the raw branch MENTIONS `outBoot42`, the comment-stripped branch does not — which
is true of any phrasing and is exactly the thing that makes stripping necessary.

## 1d. What `tests/test_probe_identity_publication.py` pins

One `audit()` function; the live tree and every mutant enter through it and
nowhere else. Scanning is over **comment-stripped source** — and this file is the
sharpest case of that rule so far, because the fix's own comment in
`readIdentity()` discusses the defect it replaced by name. A gate that counted
prose would find the identifier it exists to forbid and redden a pristine tree,
which is handoff18 §1d's lesson arriving for the second time in this family.

Four properties:

1. **The dead-read branch invents no BOOT_42 measurement.** The branch is
   brace-matched from the `nvLooksLikeDeadRead(boot0)` test and asserted to
   contain no store through `outBoot42` and to `return false;`. The LIVE path is
   asserted to still fill it — a fix that costs the healthy case its measurement
   is not a fix.
2. **`start()` consumes the bool.** No bare-statement call (balanced-paren walk,
   §1c), an `if (readIdentity(` present, and a call to each of the two
   publishers so both arms stay reachable.
3. **Only the honest publisher spells `NV_PMC_BOOT_42`.** Present in
   `publishIdentity()`, absent from `publishIdentityUnreadable()`, and exactly
   one publication site in the WHOLE FILE — a third site publishing it stands
   behind neither arm of the split. `publishIdentityUnreadable()` is also
   asserted to publish `NV_PMC_BOOT_0` and to derive none of
   `nvArchitectureName(`, `nvAmpereImplementationName(`, `nvBoot0Architecture(`
   or `nvBoot0Implementation(`.
4. **`NVIdentityReadable` is published on both paths**, `true` on the healthy one
   and `false` on the dead one.

And a refusal: if a function body cannot be found, `audit()` reports
`body not found` rather than passing over an empty string. `function_body()`
returns `None`, never `""`.

**11 tests: one positive control, nine negative controls, one prose/code
separation property.** Every negative control asserts WHICH property broke.

## 2. Non-vacuity

Nine mutations, each planted in memory against the real source and run through
the same `audit()` as the live tree:

| Mutation | Caught by |
|---|---|
| Restore `if (outBoot42) *outBoot42 = boot0;` in the dead branch | fabrication check |
| Restore the bare `readIdentity(...)` call site | branch-consumed AND unreadable-path checks |
| `publishIdentityUnreadable()` publishes `NV_PMC_BOOT_42` | dead-path check AND whole-file count |
| `publishIdentityUnreadable()` derives `nvArchitectureName(` | derived-name check |
| Drop `NVIdentityReadable` from the healthy path | both-paths flag check |
| Dead branch returns `true` instead of `false` | honest-signal check |
| Live path stops publishing BOOT_42 | whole-file count (0 sites) |
| `publishIdentityUnreadable()` stops publishing BOOT_0 | one-real-measurement check |
| Rename `publishIdentityUnreadable` so the body cannot be found | refusal check |

The first three are the original defect planted back in its three separable
forms, and that is the point of splitting them: fixing any two of the three
leaves a live path to a fabricated key. The third is the subtlest — both earlier
halves correct, and the unreadable publisher inventing the key anyway.

`mutate()` asserts its anchor's occurrence COUNT before replacing, so a mutation
whose anchor has drifted raises rather than silently matching nothing.

## 3. Gates

Every exit status was carried out through a file rather than read after a pipe.

| Gate | Verdict | Exit |
|---|---|---|
| `python tools/check-registers.py` | 240/240 passed, 0 failed, 0 skipped | 0 |
| `tests/build-msvc.bat` | 9 of 9 suites passed under ctest | 0 |
| `tests/check-kext-syntax.bat` | `ok NV3050Probe.cpp`, 13 sources scanned | 0 |
| `tests/run-python-tests.bat` | Ran 907 tests, OK | 0 |

Three things about that table are worth more than the table.

**The syntax gate is the only one that compiles the edited file**, and it is the
reason `publishIdentityUnreadable()` was declared in the header before its body
was written. It still prints `FAIL` on its own line 192 — the header of its
negative-control block — so read the exit code, grep for FAIL, then read what the
FAIL line says.

**The Python gate went red twice, and both failures were MINE**, which is the
opposite of handoff23 §3 and worth recording for that reason:

| Reading | Failure | Cause |
|---|---|---|
| 1 | `test_docs_covers_every_root_md` + `test_every_shipped_file_is_byte_identical_to_the_tree` | A throwaway `scratch-note.md` at the root, and my own `phase0-kext/` edits making `kext-src.tar` stale |
| 2 | `test_every_md5_the_card_quotes_can_be_accounted_for` + `test_the_tar_byte_size_the_card_quotes_is_current` | The rebuild moved both md5s and both sizes; the card still quoted the twenty fifth image |

Both were fixed rather than left, which is the correct call here and the reverse
of handoff23's: handoff23 declined to rebuild `build-payload/` because the
staleness was a PEER's half-landed Intel work. This time the staleness is this
session's own `phase0-kext/` edit, so rebuilding vouches for nothing but my own
change. Attribute before you touch, then touch what is yours.

**The twenty sixth image moved both SIZES**, tar 337920 → 348160 and ISO 415744
→ 425984, exactly one 10240-byte tar block each. The twenty third and twenty
fifth rebuilds moved neither, because tar pads to a block boundary and those
changes fit inside the padding. A size that moves is a size that could have been
checked; a size that does not is why the md5 test exists.

**907 is 896 + 11.** The new module contributed every added test and no
pre-existing test changed status other than the four above. The module joins the
gate by discovery — `run-python-tests.bat` needs no registration edit. No
`MEASURED` entry was added to `tests/test_check_count_floor.py`; that gate globs
`test_*.cpp` and a Python module is outside its subject.

## 4. What this does NOT establish

- **Nothing about hardware.** Unchanged since `handoff13.md`: the kext compiled
  and linked under Apple clang 17.0.0 on 2026-08-14 and has never been
  `kextload`ed. There is no NVIDIA card in the guest to match against.
- **The fix has never executed.** `publishIdentityUnreadable()` is reachable only
  by parse, exactly like `publishGspUnreadable()` before it. The shape gate
  proves the right function is called on the right branch and publishes the
  right key set. It does not prove the strings render correctly in
  IORegistryExplorer, because nothing in this tree can run them.
- **The kext has not been rebuilt.** `phase0-kext/` changed and `make` has not
  run since. The syntax gate parses under `tests/kextshim/`, which is not Apple's
  SDK — see handoff19 §2 on why matching a symbol set is not matching an include
  graph.
- **The rest of the shell is still unobserved.** `NV3050Probe.cpp` now has three
  properties pinned. §5 item 2 is the remaining list.

## 5. Open items, NVIDIA side

1. **One `kextload` on a machine that contains the card.** Unchanged, and still
   the single highest-information action available to this project.
2. **`NV3050Probe.cpp` now has three properties pinned, not two.** Still
   unpinned: the full published property-key set as a SET (three subsets are now
   covered — dead-read GSP, register-access refusals, identity), `checkWpr2()`,
   and `publishGspBootResult()`. The technique is proven three times over now and
   costs one Python module each.
3. **The kext has not been compiled since this change.** Lower value than it
   sounds — the syntax gate parses the same sources — but it is the one thing
   between here and a known-good bundle.
4. **No test observes a GSP boot from inside the DMA states with a real multi-MiB
   image.** Carried forward unchanged from handoff21 §6 item 4. Low value.

Nothing on this list is blocked on the Intel track, and nothing on it requires
editing an Intel file.
