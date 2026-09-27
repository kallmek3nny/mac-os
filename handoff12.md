# handoff12 — the table-cell absence leak, found the other way round

Newest handoff. Read `handoff11.md` for the state this continues from, and
`CLAUDE.md` for the four verification gates.

**This session did not change the project's ceiling, and it did not touch the
kext.** STEP 7 is still unanswered: the kext has never been compiled with Apple
clang, and `OPERATOR-ACTION-step7-build.md` is still the highest-value open item
in the project. What this session closed is the last item in handoff11 §6 that
needs nothing from the operator — the absence-claim scope inside a markdown
table — and it closed it as the *opposite* defect from the one that was filed.

---

## 1. Baseline first

`tests\run-python-tests.bat`, before any edit: exit 0, **Ran 678 tests, OK**.
That matches handoff11 §5 exactly, so nothing had drifted between sessions and
the count below is attributable to this session alone.

---

## 2. The filed item said the checker could not read the claim. It could.

handoff11 §6 asked for this:

> Widening `ABSENCE_RE`'s scope so an absence claim inside a table cell reads as
> one. `OPERATOR-ACTION-step7-build.md`'s `build/` is the concrete case.

The row in question is in that card's "What is already verified on the host"
table:

```
| No stale host `build/` rides along | same |
```

Measured rather than assumed, `claims_absence(doc, "build/")` on that document
already returned **True**. The claim was being read. `ABSENCE_RE` needed no
widening at all, and widening it would have made the real defect worse.

The measurement is worth recording because the first probe got it backwards.
`claims_absence(doc, token)` takes the document first; the probe called it
`claims_absence(token, doc)` and got `False` for all three tokens it tried,
which reads exactly like the filed diagnosis. Both parameters are `str`, so
nothing raised. A three-token run returning a uniform `False` is the tell —
the swapped call searches the token for the document, which can never match.

---

## 3. The real defect: a table row has no punctuation in it

`negated_before()` looks back at most `ABSENCE_WINDOW` (120) characters, throws
away everything up to and including the **last** `SENTENCE_BREAK_RE` match in
that window, and asks whether a negation survives in what remains. That is the
clause the mention actually sits in, and it is the guard that stopped the
paragraph-wide leak recorded in the memory note *Paragraph-wide negation grants
a false ABSENT*.

The break set was `[.!?;,—]|\s-\s`. **The pipe was not in it.** A markdown table
survives the paragraph split as one block, and the whitespace join flattens the
whole table into a single line, so for any mention inside a table the last break
in the window was whatever prose happened to precede the table. Consequences,
both measured on fixtures:

- a negation in an **earlier cell of the same row** reached the path in a later
  cell — `| No credentials are needed | \`tools/guest-build-kext.sh\` does the
  build |` returned ABSENT for a file that exists and is load-bearing;
- a negation in an **earlier row** reached every path in the rest of the table.

That is the same wrong-GREEN this regex exists to prevent, spelled with pipes
instead of character distance: a path could be deleted from the tree and the
gate would report ABSENT and exit 0. The fix is one character —
`SENTENCE_BREAK_RE` now includes `|` — and it is scoped to the cell, so the real
case in the operator card is untouched: its negation and its path sit in the
**same** cell. Re-measured after the change, `build/` still returns True, and
the clause it now sees is `' No stale host \`'` rather than a run backwards
through two earlier cells.

---

## 4. Five tests, and why three of them come in pairs

`AbsenceInTables` in `tests/test_check_documented_paths.py`:

| Test | What it pins |
|---|---|
| `test_absence_claim_inside_one_cell_is_still_read` | the real case keeps working; cell scoping did not cost the claim |
| `test_negation_in_a_previous_cell_does_not_grant_absence` | verdict is DIRECT, not ABSENT |
| `test_previous_cell_still_fails_when_the_path_is_dead` | positive control for the line above |
| `test_negation_in_a_previous_row_does_not_grant_absence` | verdict is DIRECT, not ABSENT |
| `test_previous_row_still_fails_when_the_path_is_dead` | positive control for the line above |

The two controls are not padding. A test that asserts "this path resolves" would
also pass against a checker that resolves everything, so each negative case is
paired with a fixture where the file is genuinely missing and the checker must go
red and name it. Every assertion is on the **verdict word**, never the exit code:
a checker that hands out ABSENT freely exits 0 too, which is the failure recorded
in the memory note *A mutation that matches nothing prints ok*.

The class name matters as well. `AbsenceClaims` already existed in that module,
and a second class with the same name would have been silently shadowed by the
last definition, with discovery still printing `ok` — see *A duplicate test class
name silently shadows*. `AbsenceInTables` is a distinct name, and the five test
names were confirmed present in the verbose discovery output rather than inferred
from the rising total.

One fixture note: `unittest` runs classes alphabetically, so `AbsenceInTables`
sorts ahead of `BrokenPaths` and a `tail -40` of the verbose run cuts it off
entirely. The first check of the new tests looked like they had not run at all.
Grep the names.

---

## 5. There is no second copy of this logic

`tests/test_documented_paths.py` is the eleven-document gate and could plausibly
carry its own negation handling, in which case the leak would still be live
against the gated docs. It does not: it declares absence per `(doc, token)` in
`EXPECTED_ABSENT` and `EXPECTED_ABSENT_DIRS` rather than inferring it from prose,
and a grep for break/window/negation constants in that module returns only
unrelated regexes. So this fix has exactly one home, and the folding hazard in
*Two resolvers can agree on all but one token* does not apply here.

---

## 6. Gate state at the end of the session

All four run after the edits:

| Gate | Result |
|---|---|
| `python tools/check-registers.py` | exit 0, **114/114 passed, 0 failed, 0 skipped** |
| `tests\build-msvc.bat` | exit 0, ctest **5/5 Passed** |
| `tests\check-kext-syntax.bat` | exit 0, sections 1–11 ok, all **7** negative controls diagnosed |
| `tests\run-python-tests.bat` | exit 0, **Ran 683 tests, OK** |

683 − 678 = 5, which is exactly the five tests added in §4 and nothing else.
The kext syntax gate still prints its own reminder that a parse is not a build.

---

## 7. What is still open

- **`OPERATOR-ACTION-step7-build.md`** — one power cycle and one command typed at
  the guest console. Still the only open item that can answer a question no
  host-side gate can: does the kext source compile under Apple clang.
- `OPERATOR-ACTION-guest-creds.md` — the account password. Unblocks
  `runScriptInGuest`, `captureScreen`, and the guest→host direction.
- Declaring the guest-only and installer-only tokens in the remaining eight root
  docs, per handoff11 §2, so the prose-path gate can cover them without a
  blanket skip. This is now the largest remaining host-side item.
- The `FLOORS` provenance comment from handoff11 §4 is still prose carrying a
  measured number, and nothing reads a number in a comment. Unchanged by this
  session.
