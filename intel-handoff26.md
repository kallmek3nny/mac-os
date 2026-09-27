# intel-handoff26 — the publishers' call shape now has an observer, and both reds were mine

Written 2026-08-18. Intel track. Continues `handoff24.md` and closes its §6 item
3 — which is the same thing as closing its §5 item 4, because the open item and
the hole are two descriptions of one gap. `handoff25.md` and `3050-handoff26.md`
are a peer's NVIDIA-side sessions from the same day and this file does not depend
on either.

**Read §8 before anything else if you are auditing file history.** This file was
written under the bare handoff26 name and destroyed the peer's NVIDIA handoff of
that same name. Theirs was recovered byte-for-byte from their transcript, and
both files then took a TRACK PREFIX — `intel-` here, `3050-` there — so the two
tracks can no longer collide on a shared number at all. (The old bare name is
spelled in words rather than as a filename above and throughout §8, because
`tools/check-documented-paths.py` resolves every path-shaped token in this tree
against the checkout and that name no longer exists.) That collision was the
second occurrence of handoff17 §5's class in two days and the second in a
concurrent tree rather than after a compaction.

**`phase0-kext/` is byte-identical to how this session found it.** No register
constant, no decoder, no shell line moved. The only file this session created
anywhere in the tree is `tests/test_intel_publisher_sample_shape.py`, and the
only files it edited are that one and this handoff. `build-payload/` was
therefore NOT rebuilt and `OPERATOR-ACTION-step7-build.md` was not touched — the
TWENTY FIFTH image is still current, proven by the payload gate's silence rather
than asserted.

---

## §1 — the gap: handoff24's own fix had no reason to stay

handoff24 §5 item 4 states it plainly, and it is worth quoting because it is the
entire subject of this session:

> **Nothing tests that the publishers still call the sampler.** Deleting either
> `sampled[]` block and its `setProperty()` returns the file to its defective
> state and all four gates stay green.

That is the identical hole handoff24 itself exploited, moved one level up. That
session found a guard whose *output* had no reader; this one is about a *call*
whose absence has no reader. The shapes rhyme and the consequence is the same:
a decode of a read nobody judged, published through `setProperty()` in a form
byte-identical to a decode of a read that was fine.

### Why none of the four gates can see it, by construction

The fourth session in a row this argument has to be re-derived, and the reasons
are not the same reasons in the same order — which is why it is re-derived
rather than cited:

- **The register gate checks constants.** `IG_SAMPLE_OK`, `IG_SAMPLE_REFUSED`
  and `IG_SAMPLE_VERDICT_LIMIT` are correct and stay correct when every call
  site is deleted. Whether anything *calls* the function is not a property of a
  `#define`.
- **The C++ suites cannot compile the subject.** The call sites are in
  `IntelDisplayProbe.cpp`, which reaches `IOService`, so no host suite can link
  it. `igSampleVerdict()` itself is a pure header function and is already
  well tested — 421 checks in `test_intel_display` — which is precisely the trap:
  a heavily tested function with no callers passes every suite it has.
- **`-Werror` has nothing to bite on.** Deleting a call whose result nothing
  consumed is not a warning. Deleting the `setProperty()` that consumed it
  removes the consumer first, so nothing is even unused.
- **The Python gate models file shapes, not this file's shape.** Nothing in it
  expressed "a function that reads MMIO must judge those reads before it
  publishes".

---

## §2 — `tests/test_intel_publisher_sample_shape.py`

handoff18 §1d's technique, third application in this tree: pin the code SHAPE
from Python, parse the source, assert the properties, **derive** the identifiers
rather than spelling them, and make the checker **REFUSE rather than pass** when
it cannot find its subject.

One `audit()`. The live tree and every mutant enter through it and nowhere else,
for `test_probe_dead_read_publication.py`'s stated reason: *a gate whose mutants
are checked by different code than its real input is two gates, and only one of
them has ever been seen to fail.*

**18 tests: six positive controls, twelve negative controls**, each negative
control asserting WHICH property broke rather than that something did.

### §2a — the load-bearing choice: the subject SET is derived, not spelled

This is the one to keep. A publisher is defined as **a method that both calls
`regRead32(` and calls `setProperty(`**. It is not a list of two names.

The naive rule — *every `setProperty()` in a publisher must be preceded by an
`igSampleVerdict()`* — is what handoff24 §6 item 3 actually wrote down, and
spelled that way it **reddens correct shipping code**. `publishIdentity()`
publishes four properties and reads **zero** registers: `fDeviceId` comes from
PCI config space, not from a BAR, so there is no refusal there to consume and
nothing for a sampler to judge. A blanket rule condemns it.

That is the third session running where the first draft's failing test would
have been the *positive control* — handoff23 §1b and handoff25 §1c are the other
two — and it is the reason the definition is a predicate rather than a list. The
second payoff is the one that matters later: a **third** publisher added next
year is picked up with no edit to this file, which is what makes the gate
non-bypassable rather than merely correct today. `test_publish_identity_is_not_a_publisher`
pins the exclusion with its reason attached, so nobody "tightens" the predicate
back into the version that condemns honest code.

### §2b — what is pinned

Six properties, each with at least one negative control:

| Property | Why it is not redundant |
|---|---|
| Every derived publisher calls `igSampleVerdict()` **exactly once** | Zero is the handoff24 defect restored; two is a per-field flag wearing a group's name |
| The sample call **precedes every** `setProperty()` in that body | The verdict must sort ABOVE the values it qualifies — IORegistry output is read top-down |
| The array names **every** variable assigned from a `regRead32(` in that body | Ten for `publishTranscoder`, four for `publishPlane`, both DERIVED from the assignments |
| Every `regRead32(` call lands in a **named** variable | An unnamed read cannot be sampled, and would leave the count arithmetic correct |
| The count argument is derived with `sizeof`, not a literal | A literal `10u` stays correct when an eleventh read is added — silently |
| The verdict is assigned and then **published** | A verdict nobody publishes is a verdict nobody reads |

Plus two refusals, which are the tests that stop the gate from being vacuous: a
source with no methods reports `subject not located`, and a source whose methods
never both read and publish reports `no publisher found`. `function_body()`
returns `None`, never `""`, for the same reason — a caller must not be able to
mistake "could not find the subject" for "the subject contains nothing".

### §2c — scanning discipline

Everything is measured over **comment- and string-stripped** source, and here
that is mandatory rather than tidy. The subject's own comments discuss the
sampled group at length; `regRead32()`'s comment names `IG_MMIO_INVALID`;
`publishTranscoder()`'s comment describes the ten-word join. handoff18 §1d is
the canonical version of the trap — there the fix's own comment quoted the
defective call, so a prose-reading gate reddened a pristine tree.

The inverse direction is the dangerous one here and it has its own negative
control, `test_a_comment_naming_the_call_does_not_satisfy_the_gate`: comment the
sampler OUT and leave its text on the line. A prose-reading gate then sees the
identifier it requires while the driver publishes decodes of reads nobody
judged. The mutation asserts the name is still present in the mutated source
*before* asserting the audit fails, so it cannot pass by having removed the text
it meant to leave.

Stripping preserves byte offsets (each removed character becomes a space,
newlines kept), which is what lets a span found in stripped code be applied to
the raw bytes. Every mutation below relies on that, and
`test_stripping_preserves_offsets_and_removes_prose` measures it rather than
assuming it.

Statement extents are found by **balanced-bracket walking**, never by a negated
character class — handoff25 §1c, a class excluding only the terminator runs past
the `if`'s closing paren, the newline and the whole guarded body to land on the
next statement's semicolon.

---

## §3 — both reds were mine, and neither was the interesting kind

The first run was **2 failures on a pristine tree**, and both were defects in
the test module rather than in shipping code. Recording them because the second
one is a class this tree has only ever seen in its other direction.

### §3a — an assertion about comment wording I remembered writing

```
AssertionError: 4 not greater than 4 : expected igSampleVerdict to appear in
                                       comments as well as code
```

I asserted that stripping REDUCES the occurrence count of `igSampleVerdict`,
i.e. that the subject's comments spell it. They do not — they discuss the group
in words without naming the callee, so the count is 4 either way. The assertion
was about wording I remembered writing rather than wording on disk. That is
handoff25 §1c-ii in a new file, and the fix is the same shape: assert the
durable property instead of today's incidental one.

What replaced it is stronger and does not depend on which identifiers the
current comments happen to contain: stripping preserves length and line count,
no `//` and no `"` survive it, and the subject provably has both to begin with
(so the check cannot pass against a file with no comments at all). The claim I
actually wanted — *a comment naming the call does not satisfy the gate* — became
a negative control, where it belongs, because it is a property of the checker
rather than of the subject.

### §3b — a mutation that matches MORE than it meant to

```
AssertionError: mutation anchor occurs 2 times, expected 1:
                'vTotal, vBlank, vSync, src, planeSize'
```

The mutation meant to drop `planeSize` from `publishTranscoder`'s `sampled[]`
array. That exact run of names occurs **twice** in the file: once in the array
and once in the `igPipeUsable()` join the array feeds. A plain `str.replace()`
would have edited both and produced a mutant that is wrong in two places, so
reddening would prove nothing about the array.

This is `a-mutation-that-matches-nothing-prints-ok` in its **over**-match
direction, and it is worth naming separately because the two failures look
nothing alike. Matching nothing prints `ok` — silent, invisible. Matching too
much raises loudly and looks like the mutation is wrong, when in fact the
precondition is working exactly as designed. `mutate()` asserting the occurrence
COUNT before replacing catches both directions with one line; only one of them
had ever been observed in this tree before today.

Fixed by mutating **structurally** rather than textually: locate
`publishTranscoder`'s body by brace match, find the `sampled[]` initializer's
brace span inside it, and trim the name from *that* span only. The mutation now
cannot reach the join by construction rather than by careful text choice, and it
asserts the trim changed something before believing the audit's verdict.

A third, smaller correction rode along: `call_sites()` requires `\b` before the
name and `\s*\(` after it, so `igSampleVerdictName` cannot be counted as a
`igSampleVerdict` call. Without it every publisher would have looked like it
called the sampler twice and the exactly-once rule would have condemned the
live tree.

---

## §4 — non-vacuity, stated honestly

handoff18 §1e's rule is that only a mutation leaving **every pre-existing check
green** proves added reach. That rule does not directly apply here and saying
so is more useful than pretending it does: this module is entirely new, so every
failure it produces is a new failure by construction. There is no pre-existing
check for a mutation to spare.

What the twelve negative controls do establish is the property that actually
matters — that `audit()` is not a function that returns `[]` for everything.
Each plants a distinct, realistic defect in memory against the real source and
asserts the specific reason:

| Mutation | Caught by |
|---|---|
| Delete `publishTranscoder`'s whole sample statement | `never calls igSampleVerdict()` |
| Comment that statement out, leaving its text | same check — proves prose is not code |
| Trim `planeSize` from the ten-word array | `does not sample it` |
| Trim `surf` from the four-word plane array | `does not sample it: surf` |
| Move the sample below the first publish | `verdict must come first` |
| Replace the `sizeof` count with `10u` | `not derived with sizeof` |
| Discard the sampler's return value | assignment check |
| Never publish the verdict | `never publishes it` |
| Add a second sample call | `must be one call` |
| Add an unnamed `(void) regRead32(...)` | `land in named variables` |
| Rename the function so the body vanishes | `function_body()` returns `None` |
| Empty subject / subject with no publisher | `subject not located` / `no publisher found` |

The claim in handoff24 §5 item 4 — that deleting a `sampled[]` block leaves all
four gates green — is **cited, not re-measured**. Re-measuring it would mean
running all four gates against a deliberately broken working tree, and the
first mutation in that table is the same defect checked in memory. Worth knowing
that the distinction exists.

---

## §5 — gate readings, taken serially

All four green, each read from its own exit code carried out through a file
rather than after a pipe — handoff21 §3 is what happens otherwise.

| Gate | Reading |
|---|---|
| `python tools/check-registers.py` | exit 0 — **240/240** passed, 0 failed, 0 skipped |
| `tests\build-msvc.bat` | exit 0 — **9/9** under ctest |
| `tests\check-kext-syntax.bat` | exit 0 — `VERDICT: PASS`, `ALL SYNTAX GATES PASSED` |
| `tests\run-python-tests.bat` | exit 0 — **937** tests, `OK` |

The register gate is unmoved at 240 and that is the correct reading, not a stale
one: this session added no `#define` to either Intel header, so a moved number
would have been the surprise.

The syntax gate prints `FAIL` on line 194 and passes. That line is
`=== 11. negative control -- each defect must FAIL ===`, the gate's own section
header. Third handoff running that this needs saying: read the exit code, and if
you grep for FAIL, read what the FAIL line actually says.

### Python count attribution — 907 → 937

handoff25 recorded **907**. The gate now prints **937**, and the 30 are not all
mine:

| Module | Tests | Whose |
|---|---|---|
| `test_probe_wpr2_publication.py` | 12 | peer's, the `3050-handoff26.md` WPR2 session |
| `test_intel_publisher_sample_shape.py` | **18** | this session's |

907 + 12 + 18 = 937, measured by running each module alone rather than by
subtraction. Attribute a moving gate number before treating it as yours — this
is the fourth consecutive handoff on which a total moved underneath a session
that did not cause all of it.

The module joins the gate by discovery; `run-python-tests.bat` needs no
registration edit.

**No `MEASURED` entry was added to `tests/test_check_count_floor.py`.** That gate
globs `test_*.cpp` and bands C++ check counts; a Python module is outside its
subject entirely, and an entry there would be a number with nothing behind it —
the exact defect handoff17 §1 exists to prevent.

Also worth recording: the `scratch-note.md` failure that reddened the Python
gate for the previous session is **gone**, and not because of anything done
here. The peer landed their own work. handoff18 §3's rule held both ways: not
touching it was correct, and its going green says nothing about this session.

---

## §6 — what this does NOT establish

Read this before §5's green table, not after it.

1. **Nothing executes.** This is a SHAPE gate. It proves the sampler is called,
   called once, called before every publish, called over every register the
   publisher read, and that its verdict is published. It does not prove any of
   that behaves correctly at runtime, because nothing in this tree can run
   `IntelDisplayProbe.cpp`. A publisher with correct structure and an inverted
   verdict passes every check in the file.
2. **`igSampleVerdict()` has still never run on hardware** and the two `-Sample`
   keys have never been published. Unchanged from handoff24 §5 item 1; this
   session narrowed the surrounding gap without closing that one.
3. **No `make` has run since handoff22.** Now overdue by seven sessions of
   `phase0-kext/` edits. The syntax gate parses; parsing is not compiling and
   `tests/kextshim/` is not Apple's SDK.
4. **The gate cannot see a publisher that reads through a different accessor.**
   The predicate is spelled against `regRead32(` by name. A second MMIO accessor
   added later would produce a method this gate does not consider a publisher at
   all — and it would report nothing rather than fail, which is the vacuity
   direction. handoff23 §1a pins "the guarded accessors are the ONLY MMIO path"
   on the NVIDIA side; the Intel side has no equivalent check. See §7 item 3.

---

## §7 — open, in priority order

1. **One `kextload` on a machine containing the card.** Unchanged since
   handoff13 and still the highest-value action in the project. Needs hardware
   nobody in this loop has.
2. **`make` in the macOS guest.** See §6 item 3. Still the only open item that
   needs nothing but the operator booting the VM and mounting the ISO, and the
   TWENTY FIFTH image is current and unchanged by this session.
3. **Pin the Intel shell's MMIO path as singular.** §6 item 4 is the hole:
   `IntelDisplayProbe.cpp` has no equivalent of handoff23 §1a's property 2, so a
   second dereference added outside `regRead32()` would be invisible to every
   gate including this one. The technique is proven three times over now and
   costs one Python module. This is the highest-value item on the list that is
   not blocked on hardware.
4. **`check-documented-paths.py` tokenizes quoted gate output as paths.**
   Harmless — the TAIL rule catches it — which means it passes for a reason
   other than the one it claims. Carried forward from handoff24 §6 item 4,
   unchanged and still the least interesting item here.

---

## §8 — I destroyed the peer's handoff26 and recovered it

This is the second occurrence of handoff17 §5's class in two days, and the
second in a row where the cause was a CONCURRENT tree rather than a compaction.
handoff25 §'s own header note records the first. Recording mine in the same
detail, because the two failures have different tells and only one of them is
the one that doc warns about.

**What happened.** I ran `ls -1 handoff*.md` before writing, read `handoff25.md`
as the highest number, and wrote the next bare number. Between that listing and
the write, the peer landed their own file at that same name — the NVIDIA WPR2
session, now `3050-handoff26.md`. My `Write` overwrote it in full.

**The tell, and why the listing did not give it.** The listing was correct when
it ran. What caught this was the tool result wording: `Write` reported the file
**"updated"** rather than **"created"**. On a file I believed did not exist, those
are different claims, and only one of them is compatible with my premise. The
second tell arrived immediately after — `CLAUDE.md`'s read-order section already
described that file as the peer's NVIDIA WPR2 session, and its mtime
(13:14:29) was **31 seconds AFTER** my write (13:13:58). A doc describing a file
I had just created, written after I created it, describing contents that were
not mine.

So the durable rule is not "list the directory first" — I did that, and it was
true when it ran. It is:

> In a concurrent tree, a directory listing is a measurement with a
> **timestamp**, not a fact. Read the WRITE VERB in the tool result:
> `updated` on a file you believe is new means somebody landed it between your
> listing and your write.

**The recovery.** The peer's session transcript
(`cf944819-...jsonl`) held exactly one `Write` to that path — 12429 characters at
line 4592 — extracted with `json.loads` and written back with `newline=''` so no
CRs were introduced. Verified before restoring rather than after: 12429 chars,
237 lines, **zero** U+FFFD, 32 em-dashes and 36 section signs intact, 0 CRs. The
mojibake in the console echo was the terminal's code page, not the file; reading
the file back through Python with an explicit `utf-8` decode is what separated
those two. Their file is restored at 12535 bytes and this session's work was
copied to a free number first, so neither was lost at any point. That copy is
this file, and it now carries a track prefix: the operator asked for
`intel-handoff<N>.md` on this track starting at 26, so the two tracks can no
longer collide on a shared number at all. This is the structural fix for the
class above — the write-verb tell is a detection, a disjoint namespace is a
prevention.

**What I did NOT do.** I did not edit the recovered content, did not renumber it,
and did not touch `CLAUDE.md`'s description of it. It is theirs and it is
byte-for-byte what they wrote; the peer applied their own `3050-` prefix
afterwards.
