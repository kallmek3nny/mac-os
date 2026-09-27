# intel-handoff39

Intel Gen12 (Tiger Lake) track. Continues from `intel-handoff38.md`.

Two items closed, both from that file's §6: item 1 (`CLAUDE.md`'s read-order
list) and item 3 (no gate over the embedded Python in
`tools/igpu-final-capture.sh`). Item 3 was the older of the two by seven
handoffs.

`phase0-kext/` is **BYTE-IDENTICAL**. No shipping code changed, so
`build-payload/` was NOT rebuilt and the fifty third image is still current --
the proof is the payload freshness check being green rather than an assertion.

All four gates end green. Readings are in §4.

---

## 0. What moved

`tests/test_igpu_final_capture_embedded.py`, **20 tests**. It is the first gate
of any kind over the 295 lines of Python living inside a quoted heredoc in
`tools/igpu-final-capture.sh`.

`CLAUDE.md`'s read-order list grew ten Intel entries (29 through 38) and one
stale banner was corrected: the `intel-handoff26.md` entry still opened "most
recent state, Intel side", which was false by twelve files.

Python 1365 -> **1385**, and the +20 is attributed by running the module alone
BEFORE the gate rather than by subtraction -- the numbers agree exactly.

---

## 1. Why a hand check was not enough

`intel-handoff27.md` §4 is honest that no gate covers the embedded half, and
records that both halves were checked BY HAND once: `sh -n` on the outside,
extract-and-`compile()` on the inside. That item then sat unmoved through
handoffs 28, 37 and 38.

**A hand check is a measurement with a date on it.** The date was 2026-08-18 and
the file has been read by four sessions since.

### 1a. The asymmetry that makes this file different

Every other script in this tree can be re-run. This one cannot. The laptop it
targets is being overwritten with macOS (`intel-handoff28.md` §0), so a
`SyntaxError` or a `NameError` discovered at capture time is not a bug to fix --
it is evidence that no longer exists.

So the interesting failure is NOT "it does not parse". `compile()` catches that,
and a hand check caught it once. The three that survive a `compile()` are:

1. **A name read and never bound.** Python resolves globals at RUN time, so a
   typo'd identifier compiles cleanly and raises `NameError` only when its
   branch executes -- which, on this script, is during the one capture.
2. **The heredoc losing its QUOTING.** `<<'PYEOF'` passes the body through
   untouched; a bare `<<PYEOF` makes the shell expand `$VAR`, backticks and
   backslashes inside what is supposed to be a Python program. The damage is
   silent: the script still runs, and it runs a DIFFERENT program.
3. **A `%`-format whose placeholder count disagrees with its argument tuple.**
   A `TypeError` three hundred lines into a capture that cannot be repeated.

### 1b. The six properties

1. **The delimiter is QUOTED.** §1a item 2.
2. **The extracted body COMPILES.**
3. **Every global name the body reads is bound in it, imported by it, or a
   builtin** -- by walking the AST, never by grepping.
4. **Every literal `%`-format matches its literal argument tuple.**
5. **There is exactly ONE heredoc**, and the body did not shrink by half.
6. **The shell half parses** under `sh -n`, and SKIPS loudly rather than passing
   quietly when no shell is on the host.

Property 5's second half exists because this module extracts by delimiter. A
restructure that leaves a two-line `PYEOF` behind would satisfy every other
property over a body that is not the program.

---

## 2. The two predicates that could have been vacuous, and their controls

The helpers get their own test class. A predicate that cannot fail turns every
property above into an `ok` line -- `an-assertion-that-cannot-fail-is-not-coverage`
for the whole module at once.

### 2a. `placeholder_count()` and the escaped percent

`%%` is a literal percent and consumes NO argument. Counting every `%` gives the
wrong number on any format string containing one, and the failure direction is
the loud one -- it reddens correct code, which is how a gate gets deleted. The
conversion regex reads width, precision and length modifiers so `%-13s` and
`%.2f` each count as one, and the `%%` group is filtered out by name rather than
by a second pass.

### 2b. `unbound_names()` is deliberately PERMISSIVE about scope

The walk collects every binding anywhere -- module-level assignment, import,
`def`, `class`, `for` target, comprehension, `except ... as`, `with ... as`,
and every argument name including `*args`/`**kwargs` and positional-only.

That is not scope-accurate and is not trying to be. The defect this exists to
catch is a name bound NOWHERE, and a scope-accurate resolver would redden on
correct code that shadows. Same reasoning as §2a: the false-positive direction
is the one that kills the gate.

### 2c. `format_mismatches()` skips what it cannot judge

Only literal-format / literal-tuple pairs are judged. A non-literal right
operand is SKIPPED rather than guessed at, and a starred element in the tuple
is skipped too. A name on the right could hold a tuple at run time, so it is
judged only when the format wants at most one argument.

This leaves real mismatches invisible, and that is the correct trade: the
vacuity direction here reports nothing, while the alternative reports a
fabricated defect in a script nobody can re-run to disprove it.

---

## 3. What the eight controls establish

`handoff18.md` §1e's every-pre-existing-check-green rule does NOT apply to an
all-new module -- there are no pre-existing checks to leave green. What the
controls establish instead is ISOLATION, and each asserts WHICH property broke.

| Control | Reddens |
|---|---|
| `<<'PYEOF'` -> `<<PYEOF` | property 1, with extraction still working |
| `total_live = 0` -> `total_live = = 0` | property 2 (`SyntaxError`) |
| `verdict_null` -> `verdict_nul` | property 3, **and still compiles** |
| drop one `%X` argument | property 4 (`2 placeholders, 1 arguments`) |
| `PROT_READ` -> `PROT_READ \| PROT_WRITE` | the read-only property |
| plant a second heredoc | REFUSAL |
| rename the delimiter | REFUSAL |
| comment out the closing delimiter | REFUSAL |

**The third control is the one that justifies the module.** It asserts the
mutated body still `compile()`s, then asserts `verdict_nul` appears in the
unbound set. A gate that only compiled would be green on it.

The first control asserts the body is still FOUND after unquoting. Without that
second half it would pass by breaking EXTRACTION rather than by breaking
quoting, which is a control measuring its own plumbing.

The three REFUSAL controls are `intel-handoff38.md` §3's rule inherited: a gate
that reports nothing when its own subject has moved is indistinguishable from a
green one.

### 3a. The read-only property was not on the open list

The script's own header promises read-only in its first ten lines and **nothing
checked it**. `mmap.PROT_READ` and `os.O_RDONLY` are asserted present;
`PROT_WRITE`, `O_RDWR` and `MAP_POPULATE` are asserted absent.

A capture that writes to a BAR is not a capture, it is a mode set on a panel
somebody is looking at -- and it would be discovered by the screen going dark on
the one boot.

---

## 4. Gate readings

All four green, measured 2026-08-29 in this order:

| Gate | Reading |
|---|---|
| `tests\run-python-tests.bat` | **Ran 1385 tests ... OK**, exit 0, zero FAIL/ERROR lines |
| `tools/check-registers.py` | **260/260** passed, 0 failed, 0 skipped |
| `tests\check-kext-syntax.bat` | exit 0 |
| `tests\build-msvc.bat` | **100% tests passed, 0 failed out of 19** |

The register gate is unmoved at 260, correctly: a Python gate over a shell
script carries no `#define`s. Every C++ count is unmoved, so
`tests/test_check_count_floor.py` needed no edit. Payload freshness is green
without a rebuild, because `phase0-kext/` did not change.

The Python gate was also run once BEFORE the new module, to confirm the
`CLAUDE.md` edits alone left `tools/check-documented-paths.py` resolving every
path-shaped token: **Ran 1365 tests ... OK**, exit 0.

### 4a. The two reading traps fired again, for the fourth handoff running

Both expected, both cost nothing.

- **The Python gate's `tail` shows an HFS catalog dump**, not a verdict. The
  reading that matters is `grep -E "^(Ran |OK|FAILED)"` plus the exit code
  captured BEFORE any pipe.
- **`grep FAIL` on a PASSING syntax run returns a hit.** This run it was line
  **292** rather than 272 -- section 11's own title,
  `negative control -- each defect must FAIL`. The line number moved and the
  trap did not, which is the argument for reading the exit code rather than
  memorising a line.

---

## 5. `CLAUDE.md`'s read-order list, and four annotations I got wrong

The list stopped at `intel-handoff28.md` while the tree reached 38. It was the
oldest item on the open list, inherited unchanged through 37 §6 item 2, 36 §7
item 2, 35 §6 item 1 and 34 §6 item 1.

**I wrote the first draft of entries 29-33 from inference rather than from the
files, and three of them were wrong.** Caught before the gate ran, by reading
the files.

- Filed 31/32/33 as "the GT stack built out header by header." FALSE -- 31 and
  32 are `IntelFbPresentProbe`, the IOFramebuffer work; 33 is where the GT track
  gets justified. GT headers start at 34.
- Cited `intel-handoff31.md` §9 item 2 as the origin of the
  attribute-the-Python-total item, carried forward from 37's open list without
  checking it against a file that turns out to be about a framebuffer shell.
  **Removed rather than restated.**
- Called 29 and 30 "ordinary continuations." FALSE -- 29 is the FIRST of the
  four shape gates over an Intel shell (`tests/test_intel_probe_mmio_path.py`,
  17 tests); 30 changed shipping code and forced a payload rebuild.

A fourth mistake was mechanical: the corrective edit re-introduced duplicate
entries for 34 and 35 alongside the researched ones. Found by grep, deleted,
re-grepped.

The durable half is the same sentence this tree keeps arriving at from new
directions: **an annotation written from the shape of a neighbouring file is a
guess wearing a citation.** The read-order list is the first thing a cold reader
trusts, so a wrong entry there costs more than a wrong entry anywhere else.

---

## 5a. The Python total is now fully attributed, and the item named a total nobody measured

`intel-handoff38.md` §6 item 2 and this file's own first draft of §7 both said
what remained unattributed was "the older 1279 -> 1315 stretch". **1315 is not a
reading anybody ever took.** It is 1347 minus 32 -- arithmetic written in the
register of a measurement, inside an item whose entire subject is that numbers
need attributing. That is `a-band-around-a-table-cannot-audit-the-table` one
level up: a derived number inherited as though it were evidence.

The totals that WERE read, out of the handoffs themselves:

| Handoff | Python reading |
|---|---|
| 35 | Ran 1279 tests |
| 36 | Ran 1279 tests |
| 37 | Ran 1347 tests |
| 38 | 1347 -> 1365 |
| 39 | 1365 -> **1385** |

Each candidate module run ALONE, which is the only attribution worth anything:

| Module | Tests | mtime |
|---|---|---|
| `tests/test_intel_probe_ring_write_shape.py` | 15 | 2026-08-28 20:37 |
| `tests/test_intel_probe_blit_shape.py` | 21 | 2026-08-29 15:12 |
| `tests/test_intel_probe_submit_shape.py` | 32 | 2026-08-29 09:22 |
| `tests/test_intel_probe_image_shape.py` | 18 | 2026-08-29 16:05 |
| `tests/test_igpu_final_capture_embedded.py` | 20 | 2026-08-29 |

**15 + 21 + 32 = 68, and 1279 + 68 = 1347 exactly.** No other combination of the
measured modules sums to 68. The stretch decomposes into three shape gates and
nothing else.

A second correction falls out of the same measurement. `intel-handoff37.md` §6
item 3 calls the +68 "across two sessions". It is not: 35 and 36 BOTH read 1279,
so the whole +68 landed in handoff37's own session -- ring-write the evening
before, blit and submit the same day, and only submit was ever recorded. That
older handoff is NOT edited, same rule as handoff28 §7 item 5 and handoff32 §1;
the correction lives here.

---

## 6. What this does not establish

1. **The embedded program has still never RUN on this host.** Every property in
   §1b is static. `compile()` plus an unbound-name walk is strictly weaker than
   execution, and the machine that could execute it is gone.
2. Property 6 SKIPS when no shell is present. A skip is not a pass, and the
   test says so in its own skip message.
3. `format_mismatches()` judges only literal pairs -- §2c.
4. Still no suite executes a line of `IntelGtProbe.cpp`, by construction.
5. Nothing was submitted to an engine. `intel-handoff37.md` §0.
6. Not acceleration.

---

## 7. Open, highest value first

1. **`tests/test_intel_scanout.cpp` still has no `MEASURED` entry**, so its
   check count has no floor. Inherited from handoff34 §6 item 4. Now the oldest
   item on this list.
3. An `IOFramebuffer` subclass remains the largest piece of unwritten DISPLAY
   work not blocked on hardware -- but **grep before opening this one**:
   `IntelFbPresentProbe` already exists, wired into `SOURCES`, `Info.plist` and
   `tests/kextshim/IOKit/graphics/IOFramebuffer.h`, with eleven overrides, a
   `presentable()` guard and a shape gate over it. The item as inherited from 38
   §6 item 5 is written as though none of that were there.
4. **No gate covers the agreement between `tools/igpu-final-capture.sh`'s
   embedded RANGES table and the verdict words the script prints below it.**
   §0's module checks the program is well formed; `tests/test_igpu_mmio_ranges.py`
   checks the table contains the canonical three. Neither checks that a range
   added to the table gets read.
