# intel-handoff27 -- the single Linux capture script, and the gate that keeps it honest

Intel Gen12 (Tiger Lake) display track. Written 2026-08-18. Continues
`intel-handoff26.md`; the NVIDIA track's newest is `3050-handoff27.md` and is
independent of this one -- either order.

This session did **not** touch `phase0-kext/`. Nothing shipping changed, so
`build-payload/` was not rebuilt by me and the payload gate has nothing to say
about this work. What moved is the **capture layer**: the Linux phase now has
one script instead of six, and the fourth MMIO range table it introduces is
gated rather than free-floating.

---

## 0. The operator directive this closes

The operator's words, and they are the whole design brief:

> "ill boot linux but i dont like running a ton of scripts so make 1 giant sh
> script to get everything u need form linux, and once youre COMPLETELY done
> with linux AND NEVER NEED IT AGAIN, then ill install macos and we can verify
> boot"

Three constraints, and the third is the one that shapes every decision below:

1. **One file.** Not a driver script plus a helper. One.
2. **One command.** `sudo sh igpu-final-capture.sh`, nothing else to remember.
3. **One boot, ever.** macOS gets installed over this Linux afterwards.
   Anything the script omits **cannot be captured later at any price**.

Constraint 3 inverts the usual economy of a capture script. Normally you read
what you need and keep the output small. Here an omitted page is permanently
unknowable, and an over-large dump costs nothing but disk. So the script's
standing rule is: **when in doubt, DUMP**, and it is written into the header so
nobody "tidies" it later.

---

## 1. `tools/igpu-final-capture.sh`

One file, 25-row range table, embedded Python dumper, seven decode sections.
It is the union of all six predecessors plus pages none of them ever read.

### 1a. What the six predecessors each contributed, and why six exist

| script | what it added |
|---|---|
| `capture-igpu-facts.sh` | PCI/sysfs/DRM inventory, no MMIO at all |
| `capture-igpu-display.sh` | first MMIO reads, pipe/transcoder A..C |
| `igpu-capture.sh` | embedded dumper, the 15 canonical ranges |
| `igpu-decode.sh` | decode pass over a previous capture's bytes |
| `igpu-followup.sh` | pipe/transcoder D plus DDI/AUX |
| `igpu-plane-dsi.sh` | plane slots, DSI pages, the PIPESRC verdict |

Each one exists because the previous one raised a question it could not answer.
That is a reasonable way to work when you can re-run at will, and it is exactly
the wrong artifact to hand somebody who gets **one** boot. Running six scripts
is six chances to run five, and the fifth one skipped is the one page nobody
ever sees.

### 1b. Why the dumper is embedded rather than a companion file

Not style. The 2026-08-15 15:50 UTC attempt failed for exactly one reason: a
second file was not copied alongside the first. `tools/igpu-capture.sh` already
embeds its dumper for that reason and this one does the same, via a
`<<'PYEOF'` heredoc.

Python is a hard dependency and the script says so up front rather than
discovering it at line 200: **a shell cannot `mmap` a memory BAR.** That is the
whole reason `tools/read-igpu-mmio.py` exists next to its shell twin. The twin
reads via `dd`, which is why its comment says bases are aligned "because dd
seeks in whole blocks" while the Python one says "because mmap requires it" --
same numbers, different reason, and both true.

### 1c. The REACH guard, and why zero counts as dead

An unclaimed page and a failed mmap both read `0xFFFFFFFF`. And -- **MEASURED
2026-08-16 on 8086:9a68, handoff15 §2** -- an unused display page on *this*
silicon reads `0x00000000`, not all-ones. So:

```python
TRIVIAL = (0x00000000, 0xFFFFFFFF)
```

Both values are counted dead. The first version of `igpu-plane-dsi.sh` treated
only `None`/all-ones as dead and consequently printed the verdict word **ABSENT**
for transcoders B/C/D, which certainly exist on Tiger Lake. That word was false.

The fix is not just widening `dead()` -- it is that the verdict word itself must
not overclaim. One read cannot separate a decoded-but-idle block from a missing
one, so the word is `UNPROGRAMMED-OR-ABSENT` and the gate asserts the bare string
`= 'ABSENT'` does **not** appear in the file.

Ahead of all of it, a positive control:

```python
ctrl = rd(0x60000)          # transcoder A HTOTAL -- driving the lit panel NOW
READER_OK = not dead(ctrl)
```

If that control is dead the mapping is broken, and **every** null below
downgrades to `INCONCLUSIVE` rather than being read as a fact about the hardware.
The distinction is the difference between "transcoder D is unprogrammed" and
"transcoder D was never read", and only the second is a bug somebody can fix.

### 1d. Read-only, and why that is a safety property not a habit

`os.O_RDONLY` and `mmap.PROT_READ`; no `O_RDWR`, no `PROT_WRITE`. No bind, no
unbind, no `modprobe -r`, no `rmmod`, no mode set. The panel stays lit for the
whole run, which is what makes every value in the output a value that was
**driving the screen at the moment it was read**.

Unbinding i915 would blank the panel, and every timing register read afterwards
would describe a display that had stopped. That is not a degraded capture, it is
a capture of the wrong thing, and on a one-boot budget it is unrecoverable.

### 1e. The seven decode sections

1. **Raw range dump.** Every non-trivial dword in every range. Trivial words are
   *counted*, not printed -- on this silicon a page of zeros is the normal look
   of an unprogrammed block, and printing 1024 of them per page buries the
   evidence in the noise it is trying to distinguish itself from.
2. **Transcoder timing blocks**, A/B/C/D/DSI0/DSI1, with the halves decoded and
   the `+1` shown alongside the raw value rather than instead of it.
3. **Pipe configuration** -- CONF, MISC, MISC2 across all six.
4. **Universal plane slots (Q3)**, `pipe + 0x180 + n*0x100`, seven slots each,
   whole slot dumped raw after the named fields.
5. **DSI pages and the 0x800-vs-0x1000 stride (Q1/Q2).**
6. **Clocks, PLLs, fuses, DDI** -- including the Gen12 block at `0x164000` that
   no predecessor script ever read.
7. **PIPESRC +1 vs the lit panel (Q4)**, printed next to `/sys/class/drm/*/modes`.

Section 7 ends with a line that is a discipline rather than a hint:

```
  -> whichever of raw / +1 equals a mode above is the convention.
     If NEITHER matches, say so. Do not pick the closer one.
```

### 1f. What section 4 is really for

Q4 was settled on 2026-08-16: `PIPESRC 0x077F0437` `+1` == the panel's advertised
1920x1080, and `PLANE_SIZE 0x0437077F` was read in the same breath -- halves
**INVERTED** relative to PIPESRC. Byte-swapped. A decoder reused across the two
without noticing transposes the axes **and still programs cleanly**, which is the
worst failure shape available: no fault, no error, a plausible wrong picture.

So section 4 prints both halves of `PLANE_SIZE` and both `+1` values right under
the PIPESRC line, with the inversion named in the output text. The one silicon
run proved it once; this run should show it again beside a second lit mode if the
operator has an external display attached.

### 1g. What this script still cannot settle

Q1/Q2 (the DSI pages, the `0x800` stride) and Q3 (plane count) are **not
unanswered, they are unanswerable on an eDP-only machine**. Nothing on a laptop
with no DSI panel will ever program a DSI transcoder, so a null there separates
nothing. The script reads those pages anyway and says in its own output that a
null settles nothing either way -- so the absence is **documented rather than
inferred**. Somebody reading the artifact in six months must not mistake "we
looked and saw zeros" for "there is nothing there".

If the operator can attach an external display before running it, Q3 becomes
answerable on pipe B and several constants currently carried as policy become
measurements. That is the single highest-value thing that could change about the
run, and it costs one HDMI cable.

---

## 2. The fourth range table, and why it is a superset rather than a fourth twin

This is the design decision worth reading.

### 2a. The constraint I walked into

`tools/read-igpu-mmio.py`, `tools/read-igpu-mmio.sh` and `tools/igpu-capture.sh`
carry **three copies of one 15-row range table**, and
`tests/test_igpu_mmio_ranges.py` exists to fail when they drift apart. It demands
the three be **IDENTICAL**, not merely overlapping -- they answer the same
question three ways, so any difference between them is a bug in one of them.

The final capture needs pages those three do not have: transcoder/pipe D, the DSI
pages, the Gen12 PLL block at `0x164000`, the PCH fuse and DPLL pages, `SKL_DFSM`,
`TRANS_DP_CTL`. Ten additions in total.

Folding it into the equality forces one of two bad outcomes: either the three
canonical tools grow ranges they have no reason to read, or the final capture
loses the pages that are its entire reason for existing.

### 2b. Containment, asserted in one direction only

So the relationship gated is **containment**, not equality:

- every canonical `(base, length)` pair appears in the final table **verbatim**;
- the final table carries **at least one** pair the canonical three lack.

What that buys is the thing the module exists for, preserved across the
asymmetry: **adding a range to the canonical three without adding it here reddens
the gate.** Drift is still caught; it is just caught by a one-directional claim
instead of a symmetric one.

The base-AND-length detail is load-bearing. Comparing bases alone would pass a
row that kept `0x044000` but shrank its length from `0x2000` to `0x1000` --
silently dropping half the north display engine interrupt block from the one
capture that gets to read it.

### 2c. The parser needed no change, and that is not luck

`_python_list_ranges()` is bounded by the `RANGES = [...]` list itself rather
than scanning for hex tuples anywhere in the file. Its docstring explains why:
`igpu-capture.sh`'s header quotes kernel line numbers and register addresses **in
prose**, so an unbounded scan would parse documentation and call it a range.

The final capture makes that far worse -- its header carries a whole citation
table of register offsets and header line numbers, thirty-odd hex values in
comments. Writing the new table in the same `RANGES = [` form at column 0 inside
the heredoc means the existing bounded parser reads it unchanged, and the prose
above it stays invisible. `final_ranges()` is four lines and a docstring.

This is also why the earlier grep for `BEGIN-RANGES` in `igpu-capture.sh` found
nothing: the embedded copy uses the Python-list form, not the shell markers. Two
extraction shapes in one module, and the marker form belongs only to the shell
twin.

### 2d. The two new test classes

`TheFinalCaptureIsASuperset` -- 7 checks:

- the file exists (a missing file makes every extraction vacuously empty);
- its table parses and meets `RANGE_FLOOR`;
- every canonical range survives verbatim -- **the containment claim itself**;
- it is a **strict** superset -- the anti-degeneracy check, because containment
  alone is satisfied by an exact copy, and an exact copy would mean the file's
  stated reason for existing is false;
- bases page-aligned, lengths whole pages;
- no two ranges overlap;
- every `ANCHORS` symbol falls inside **exactly one** range.

The overlap check matters more here than in the canonical table, and the reason
is in its docstring: this artifact is the only one that will ever exist, so a
block dumped twice under two labels reads as **corroboration**.

The anchor check is asserted against the final table independently rather than
inferred from containment. Containment says the canonical rows are present; an
anchor added later could still resolve in neither table.

`TheFinalCaptureKeepsItsCaptureDiscipline` -- 7 checks, and every one of them was
learned from a run that went wrong. None is visible in the range table:

- the dumper is embedded (no companion file);
- a positive control is read before any absence is reported;
- zero counts as dead;
- the verdict word does not overclaim;
- the BAR is opened read-only;
- nothing binds or unbinds;
- perishable provenance is recorded.

---

## 3. Non-vacuity: four mutations, and each reddens exactly its own check

A containment gate that matches nothing passes for free. An empty list is a
superset of nothing and contains nothing, so both directions are vacuously
satisfied. That failure mode is silent and it is the whole reason this section
exists.

The mutations were run against a **temp-directory copy**, with `FINAL_TOOL`
repointed and restored -- the working tree was never modified. A peer's probe
planting defects in the live tree fakes named FAILs for everybody else in a
two-agent checkout.

| mutation | red | which |
|---|---|---|
| baseline (unmutated copy) | **0** | -- |
| A: canonical row deleted | 1 | `test_every_canonical_range_survives_verbatim` |
| B: canonical length halved, base kept | 1 | `test_every_canonical_range_survives_verbatim` |
| C: table is an exact canonical copy | 1 | `test_it_is_a_strict_superset` |
| D: list renamed, parser matches nothing | 4 | floor, containment, strictness, anchors |

Every run was 35 tests, so nothing errored out early and skewed a count.

**B is the one to read.** A and B are different defects that a base-only
comparison would have merged: A removes a row, B keeps the row and shrinks it.
Both redden the same check, which is correct -- it is one claim, "the canonical
range survives verbatim" -- but only because the check compares the **pair**. Had
I written `set(b for b, _ in ...)` the B mutation would have stayed green while
halving a block the one capture gets to read.

**C is the anti-degeneracy proof and it is the one that would have been easiest
to omit.** Containment is satisfied by an exact copy. Without C, a future edit
that quietly stripped the ten additions -- someone "unifying the four tables" --
would leave the gate fully green while deleting the file's reason for existing.
C reddens exactly one check, and it is a new one.

**D is the vacuity control.** Renaming `RANGES` to `RANGE_TABLE` makes the parser
return `[]`. Four checks redden, and the important thing is *which*: the floor
fires first, which is what stops the other three from silently passing over an
empty list. Note strictness reddens here too -- `set() - set(canonical)` is empty,
so the strict-superset claim fails rather than passing on nothing.

All four mutations left every **pre-existing** check green. That is the signal
handoff18 §1e argues for: a mutation that reddens many pre-existing checks is
evidence the *old* tests already sufficed, and proves nothing about the reach the
new ones add.

---

## 4. Syntax: both halves of a two-language file

The script is a shell file with 295 lines of Python inside it. A syntax error in
the embedded half means the one-boot run dies partway through with the shell half
reporting nothing wrong, so both halves were checked:

```
heredoc found: True
embedded python lines: 295
EMBEDDED PYTHON SYNTAX OK      <- extracted the heredoc, compile()'d it
SHELL SYNTAX OK                <- sh -n
```

The Python check extracts the heredoc body by the same `<<'PYEOF'` ... `PYEOF`
bounds the shell uses and hands it to `compile()`. Neither gate covers this file
-- `check-kext-syntax.bat` parses the kext, not `tools/` -- so this is a
session-local check, recorded here because it is not repeatable by running a
gate. **If you edit that heredoc, re-run it by hand.**

That is an honest gap and it is worth naming: nothing in the four gates would
catch a Python syntax error in this script. §7 item 2.

---

## 5. The four gates, and a peer's staleness that closed underneath me

All four green at the end. Two readings are recorded because the first Python
reading was **red for reasons that were not mine**, and the attribution is the
useful part.

| gate | verdict |
|---|---|
| `python tools/check-registers.py` | **240/240**, 0 failed, 0 skipped |
| `tests\run-python-tests.bat` | first read **FAILED (failures=2)**; second read **952 / OK / exit 0** |
| `tests\check-kext-syntax.bat` | exit **0**, `ALL SYNTAX GATES PASSED` |
| `tests\build-msvc.bat` | exit **0**, **9/9** suites |

The register gate is unmoved at 240 and that is **correct**: no `#define` was
added this session. A capture script carries no register constants -- its offsets
are comments citing `reference/i915/`, and comments are not defines. A gate that
moved here would mean I had put constants somewhere they do not belong.

### 5a. The two red checks, and why touching them would have been wrong

First Python reading:

```
FAIL: test_every_md5_the_card_quotes_can_be_accounted_for
FAIL: test_the_tar_byte_size_the_card_quotes_is_current
Ran 952 tests -- FAILED (failures=2)
```

Both in `test_operator_step7_doc.DerivedValueTests` -- the payload/card staleness
family. I touched `tools/igpu-final-capture.sh` and
`tests/test_igpu_mmio_ranges.py`, neither of which the payload gate reads.

Attribution by mtime:

```
13:09:39  OPERATOR-ACTION-step7-build.md   <- the card, as it was
14:25:57  phase0-kext/NVRiscvUcode.hpp     <- peer's edit
14:29:11  build-payload/kext-src.tar       <- peer's rebuild
14:29:12  build-payload/step7-payload.iso
```

The payload was rebuilt at 14:29 against a card last written at 13:09. The card
quoted md5s for an image that no longer existed. That is the peer's NVIDIA-track
work mid-landing, and **rebuilding the payload or editing the card would have
greened a gate by vouching for a peer's half-landed work under my name.**

I measured instead. By the time I re-read it:

```
14:34:27  OPERATOR-ACTION-step7-build.md   <- rewritten, after the 14:29 payload
```

`test_operator_step7_doc` alone: **25/25 OK**. Full gate: **952 / OK / exit 0**.
The peer closed their own staleness while I was diagnosing it. **A gate's verdict
in a two-agent tree is a joint measurement** -- red does not mean you broke it,
green does not mean you fixed it. Attribute by mtime and by the failure MESSAGE
before touching anything.

### 5b. The syntax gate's benign FAIL

`grep -cE '\bFAIL\b'` returns **1** on a passing run. That hit is section 11's own
header -- the section is *titled* "each defect must FAIL" -- so a bare grep for
the word reports a hit on a clean gate. Read the **exit code**. It was 0, and the
tail printed `=== ALL SYNTAX GATES PASSED ===`.

---

## 6. What this session did NOT establish

1. **The script has never run.** Not on this hardware, not on any hardware. Every
   claim about its *output* is a claim about code I read, not bytes I saw. The
   syntax of both halves parses; that is all.
2. **No range in the ten additions has been read on silicon.** Transcoder/pipe D,
   the DSI pages, `0x164000`, the PCH pages -- all ten are additions on the
   argument that a one-boot capture should over-read. None is evidence yet.
3. **`phase0-kext/` is byte-identical.** No shipping code changed, so nothing here
   moves the driver's completion at all. This is capture infrastructure.
4. **The superset gate cannot check the script does what its table says.** It
   asserts the table's shape and the file's discipline via string presence. A
   `dead()` that ignored `TRIVIAL` entirely would still contain the string
   `TRIVIAL = (0x00000000, 0xFFFFFFFF)` and pass. The gate pins the shape, not
   the behaviour -- there is no way to execute this file off Linux.
5. **`0x0C7000` and `0x0C8000` are dumped without named anchors.** Panel power
   sequencing and backlight PWM are in the canonical table, but
   `intel_pps_regs.h` and `intel_dp_aux_regs.h` were never vendored into
   `reference/i915/`, so `PP_STATUS`, `PP_CONTROL` and the `AUX_CH_CTL` family
   have no citable line in this tree. Four greps confirmed the absence rather
   than assuming it. Their words are captured; **naming them is offline work
   against headers this repo does not carry**, and inventing names here would put
   fabricated citations next to real ones.

---

## 7. Open, highest value first

1. **One `kextload` on a machine containing the card.** Unchanged since
   handoff13, still the highest-value action in the project, still needs
   hardware. Note the scheduling constraint from `3050-handoff27.md` §1: the
   laptop cannot hold Linux and macOS at once, so **the Linux capture and a
   macOS `kextload` are mutually exclusive** and the order is the operator's
   call. This session's work is what makes the Linux side cost one command.
2. **Nothing gates the embedded Python's syntax.** §4 checked it by hand. A
   `tests/test_capture_script_syntax.py` that extracts every `<<'PYEOF'` heredoc
   in `tools/*.sh` and `compile()`s it would make that repeatable, and it would
   cover `igpu-capture.sh` too. Cheap, and not blocked on anything.
3. **Pin the Intel shell's MMIO path as singular.** Carried from
   `intel-handoff26.md` §7 item 3 and still the highest-value item not blocked on
   hardware. The shape gate names `regRead32(` literally, so a second MMIO
   accessor added later yields a method the gate silently declines to treat as a
   publisher -- the **vacuity** direction, which reports nothing rather than
   failing.
4. **`make` in the macOS guest.** Overdue by eight sessions of `phase0-kext/`
   edits. The guest is booted; `OPERATOR-ACTION-step7-build.md` reduces it to one
   command.
5. **Attach an external display before the Linux run.** §1g. Makes Q3 answerable
   on a second pipe and converts several policy constants into measurements.
   Costs one cable and must happen *before* the one boot, which is why it is on
   this list rather than in a future session's.
6. **The six predecessor capture scripts are now superseded but not marked.**
   Nothing stops somebody running `igpu-followup.sh` and thinking they captured
   everything. A one-line banner in each pointing at
   `tools/igpu-final-capture.sh` would prevent it. Lowest priority, and note the
   doc gate tokenizes path-shaped strings -- a banner naming a real file is safe,
   a placeholder is not.

---

## 8. If you are the next session on this track

The deliverable is done and waiting on the operator, not on you. Do not rewrite
`tools/igpu-final-capture.sh` to be tidier -- its verbosity is the point, and
§0 constraint 3 is why. If you add a range to it, add it to the canonical three
too if it belongs there; if it does not belong there, the superset gate already
allows it and nothing needs changing.

If you add a range to the **canonical three**, you must add it here as well or
`test_every_canonical_range_survives_verbatim` reddens. That is deliberate. It is
the drift check surviving the asymmetry, and it is the only thing standing
between a fourth table and four tables that quietly disagree.
