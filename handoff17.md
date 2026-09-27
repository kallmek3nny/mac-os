# handoff17 — the gate that could not fail, and the handoff I overwrote

Continues from `handoff16.md`. Read that one first for the four decode layers
and the `IG_USABLE_` naming collision; nothing here touches them.

No register constant, decoder, or state machine changed in this session. What
moved is the evidence layer: one gate was structurally unable to fail, two
claims in `CLAUDE.md` were false when measured, and I destroyed a handoff file
by writing over it. All three are the same family the tree has been collecting
for seventeen handoffs — a green signal, or a confident sentence, that says less
than it appears to.

Read §1 first. It is the only item that changes how a future session should
trust a number. Read §5 second, because it is a mistake I made rather than one I
found, and the recovery path is worth knowing.

---

## §1 — a band around a table cannot audit the table

`tests/test_check_count_floor.py` bounds every C++ suite's check count from both
sides. The floor is `measured // 2`, the ceiling is
`measured + max(8, measured // 8)`, and both are derived **in code** from a
single hand-recorded reading per suite in `MEASURED`. The ceiling was added on
2026-08-16 precisely because a floor is one-sided, and it has since caught three
stale readings no human noticed.

It could not catch this one, and cannot catch this class at all.

`MEASURED["test_intel_display"]` said 354. The binary printed 354. Floor,
ceiling, staleness (binary newer than source), last-line, and ctest were all
green. The provenance comment above the dict — the block that gives the word
"measured" its entire meaning in this file — recorded nothing newer than **319**.

So the table held a number described as measured whose only evidence was that
somebody had typed it.

**Why the ceiling is blind, structurally.** It is a relation between the BINARY
and the TABLE. Editing the table to match the binary satisfies it *identically*
to running the binary and recording what it printed. For each of the seven prior
stalenesses, the cheap fix (bump the dict) and the correct fix (bump the dict,
record the reading, date it) produced the same green. Nothing in the module had
ever read the comment. This is the same shape as the containment lesson in
`handoff16.md` §7 — a bound checked against the thing it was derived from
constrains nothing.

**What was added.** `EveryTableEntryHasProvenance` reads this module's own
source, extracts every line that looks like a recorded reading, and requires
each dict entry to have a matching one. Three tests, because a prose-scanning
test is exactly the kind that passes by matching nothing:

- The reading regex must match the comment's real idiom in **both** count
  formats — `169/169 passed` and `217 checks` — asserted against literal sample
  lines.
- The regex must **NOT** match a dict line such as `"test_intel_display": 354,`.
  A dict line also pairs a suite stem with a number, so a regex loose enough to
  match it would make every entry its own evidence and the whole class would
  assert nothing. `READING_RE` therefore anchors on a leading `#`.
- A third test asserts the reader finds a specific known reading in the live
  file (`test_gsp_boot` → 217), so a reader that returns empty for everything
  reddens rather than passing.

**Verified by watching it fail.** The class was added *before* the provenance
was fixed. It reddened naming `test_intel_display=354 (comment records ... 319)`,
then went green after the reading was recorded. That order matters — a gate added
alongside its own fix is a gate nobody has seen fail.

The 354 reading is now in the comment with its date, the mtime evidence that the
binary postdates both its sources (`.exe` 14:54, header 14:53, suite 14:52), and
an explicit note that 354 sits **four checks inside** the previous entry's band
(319 + max(8, 39) = 358), so nothing would have fired on its own.

---

## §2 — handoff14 §3 is closed, and its premise was false

`CLAUDE.md` pointed at `handoff14.md` §3 as "STILL the highest-value open item in
the tree." The claim: `tools/check-registers.py` never reads `IntelDisplay.hpp`,
the gate's header list stops at the four NVIDIA ones, and therefore every Intel
constant is unverified by the one gate whose purpose is catching
hand-transcription errors in register constants.

Measured by running the gate. Its own header list prints `IntelDisplay.hpp`
with 86 defines and `IntelFramebuffer.hpp` with 48, checks them against the 3180
defines vendored under `reference/i915/`, and handles `IntelGpuId.hpp` on a
separate path because PCI IDs on that side live inside a variadic macro body
rather than plain `#define`s. Verdict: 238 of 238 passed, zero failed, zero
skipped.

So the Intel constants **are** covered, and the register gate's clean run is now
a true statement about both tracks rather than only the NVIDIA one. The
`CLAUDE.md` bullet has been rewritten to say CLOSED with the measurement inline,
and warns against opening the card assuming its premise holds — the same warning
the file already carries for `OPERATOR-ACTION-step7-build.md`.

Worth naming why the false claim survived four handoffs: a grep for the Intel
table names in that script finds no consumer near them, because the consumers
sit some 900 lines lower under different names. Run it and read the count.

---

## §3 — the architecture section had drifted two headers behind

The dependency-free header list in `CLAUDE.md` named six headers and described
`IntelDisplay.hpp` as "the newest." There are eight, and the Intel track has
three. Corrected, with the asymmetry stated that the old list implied away:
**there is no IOKit shell on the Intel side at all**, so those three headers have
no consumer outside `tests/`. That is a real limit on what the Intel work
proves, and it was invisible in a list presenting all eight the same way.

---

## §4 — a documentation edit reddened a gate

Writing the measured verdict into `CLAUDE.md` in its native form broke
`test_check_documented_paths.py`, which read the slash in `238/238` as a
directory separator and demanded a file that does not exist:

```
FAIL: CLAUDE.md:86: 238/238 passed, 0 failed, 0 skipped: does not exist,
      and no producer or absence claim explains it
```

Reworded to prose — "238 of 238 passed, zero failed, zero skipped." Two tests
failed and both named the token, so the diagnosis took one read.

The rule for this tree: **anything slash-separated in a doc will be tokenised as
a path.** Write counts, ratios, and version pairs as words.

---

## §5 — I overwrote handoff16.md, and how it came back

This is the one to read if you are resuming after a context compaction.

The session was compacted mid-flight. The summary that survived did not mention
that a `handoff16.md` had already been written earlier in the *same* session.
So when I went to file this session's notes, I called `Write` on
`handoff16.md` — and Write overwrites. The 15752-byte "four joins" document
became an 8523-byte document about check counts.

**What caught it.** Not a gate — none of the four can see this. An `Edit` failed
with "String to replace not found" while I was trying to *add* a handoff16 bullet
to `CLAUDE.md`'s read order. The bullet was already there, describing content I
had not written. A file I believed I had just created was already documented by
a file I had not touched.

**Two tells I walked past.** The Write tool result said `has been updated`, not
`File created successfully` — the harness distinguishes them and I did not read
the distinction. And `ls -la handoff1*.md` would have shown eight handoffs before
I wrote, where the read-order list in `CLAUDE.md` accounts for every one.

**Recovery.** Claude Code's session transcripts under
`C:\Users\Kentl\.claude\projects\D--nerd-shit-other-projects-mac-os\` are JSONL
with every `tool_use` block recorded verbatim, including full `Write` payloads.
A scan across all transcripts for the string `handoff16` found the original write
at line 2569 of one file with `LEN 15752`, and the content was recoverable
byte-for-byte. Restored with `newline='\n'` — markdown is excluded from
`test_line_endings.py` deliberately, and the file is LF-only as it was.

**The rule.** Before `Write` to any path that follows a numbering convention,
list the directory. `Write` has no create-only mode, and a numbered series is
precisely where a stale mental model produces a collision. When resuming from a
compaction, the summary is a lossy record of your own prior actions, not a
ledger — check the filesystem for what you did, do not recall it.

---

## §6 — gate state

All four run from the project root, all green after every change above, and each
of the last two edits was followed by a full re-run rather than a spot check.

| Gate | Verdict |
|---|---|
| `python tools/check-registers.py` | 238 of 238 passed, zero failed, zero skipped |
| `tests\build-msvc.bat` | 8 of 8 suites passed under ctest |
| `tests\check-kext-syntax.bat` | VERDICT PASS, all syntax gates passed |
| `tests\run-python-tests.bat` | 854 tests, OK, exit 0 |

The Python total moved 851 → 854 in this session; the three are the new
provenance class in §1.

The eight C++ suites and the counts their binaries printed, measured this
session by running each `.exe` directly:

| suite | checks |
|---|---|
| `test_decode` | 64 |
| `test_falcon` | 169 |
| `test_riscv_ucode` | 116 |
| `test_gsp_boot` | 217 |
| `test_gsp_stage` | 181 |
| `test_intel_display` | 354 |
| `test_intel_gpu_id` | 77 |
| `test_intel_framebuffer` | 110 |

Every one of those now has a dated line in the provenance comment backing it,
which was not true before this session.

The kext build state is unchanged from `handoff13.md`: it compiles and links
under Apple clang 17.0.0 in the guest, and it has still never been loaded.
Nothing here moved that.

---

## §7 — what is open

With handoff14 §3 closed, the highest-value items are all on the Intel side and
all of the same kind — the track has headers and tests but no consumer.

- **No IOKit shell for the Intel path.** The three Intel headers are driven only
  by `tests/`. Nothing constructs them the way `NV3050Probe.cpp` constructs the
  NVIDIA side, so the mode-programming *order* those headers encode has never
  been exercised as a sequence, only as arithmetic. This is the analogue of
  `NVGspBoot.hpp`'s journal-index assertions, which exist precisely because final
  register contents pass even with the write order reversed. Everything the
  Intel track knows about ordering is currently unasserted.
- **The capture's unanswerable questions stay unanswerable.** `handoff15.md` §2
  is unchanged: every unused display page on that silicon reads `0x00000000`
  rather than all-ones, the eDP-only panel cannot settle the DSI pages, the
  `0x800` stride, or the plane count, and there is no second boot of that
  machine. Read every ABSENT in
  `igpu-results/tgl-capture-2026-08-16-022556.txt` as UNPROGRAMMED.
- **The provenance idea generalises.** §1's defect is not specific to check
  counts. Anywhere this tree carries a derived bound next to prose claiming a
  measurement — the doc-path gate's `FLOORS` has already gone stale this way once
  — the same audit applies: make a test read the prose. Before trusting any
  numeric gate, ask what two things it compares and whether one was computed from
  the other.
