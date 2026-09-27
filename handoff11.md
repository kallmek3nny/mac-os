# handoff11 — the prose-path gate stops being one document wide

Newest handoff. Read `handoff10.md` for the state this continues from, and
`CLAUDE.md` for the four verification gates.

**This session did not change the project's ceiling, and it did not touch the
kext.** STEP 7 is still unanswered: the kext has never been compiled with Apple
clang, and `OPERATOR-ACTION-step7-build.md` is still the highest-value open item
in the project. What this session closed is `t-16588d`, the last open item that
needed nothing from the operator.

---

## 1. Baseline first, because handoff10's numbers had to be re-earned

All four gates were run before anything was edited. Every one matched what
handoff10 §5 recorded, so nothing had regressed between sessions:

| Gate | Result at session start |
|---|---|
| `python tools/check-registers.py` | exit 0, 114/114 passed, 0 failed, 0 skipped |
| `tests\build-msvc.bat` | exit 0, ctest 5/5 Passed |
| `tests\check-kext-syntax.bat` | exit 0, sections 1–11 ok, all 7 negative controls diagnosed |
| `tests\run-python-tests.bat` | exit 0, Ran 678 tests, OK |

The Python gate was run alone both times. Running it concurrently with anything
else that writes `%TEMP%` fakes a `FAILED (errors=1)` with the right test count
and no traceback, which reads convincingly as a real defect.

---

## 2. `test_documented_paths.py` now scans eleven docs, not five

`DOCS` held `CLAUDE.md` plus the four READMEs. Six root docs were added:
`README.md`, `PLAN.md`, `HARDWARE.md`, `PHASE4-GSP-BRINGUP.md`,
`PHASE1-GSP-NOTES.md`, `PUBLISH-GSP-BRINGUP.md`.

**Every token in all six already resolved before they were added.** That was
measured against this module's own `resolve()` — not a fresh reimplementation of
it — so the verdicts are the ones the gate will actually produce. No new entry
was needed in `EXPECTED_ABSENT` or `OUT_OF_TREE`. So what the widening buys is a
guard from here forward, not a fix landing now, and the docstring says so in
those words rather than implying six documents were repaired.

### Why the other eight root docs stayed out

This is the part worth not losing. Each remaining root doc was probed the same
way, and each names at least one path that genuinely is not in this tree:

- `STEPS.md` — four host-side installer files this repo does not contain
  (`fetch-macOS-v2.py`, `win-install.cmd`, `run_offline.md`,
  `SystemVersion.plist`).
- `OPERATOR-ACTION-step7-build.md` — a `build/` directory that only exists
  inside the guest after the operator's build. It is named in a sentence whose
  clause begins "No stale host `build/` rides along", which is an absence claim
  the ABSENCE scoping does not currently reach in a table cell.
- `OPERATOR-ACTION-guest-creds.md`, `SETUP.md`, `VM-BRINGUP.md`,
  `NETWORK-DIAGNOSIS.md`, `VMX-INVENTORY.md`, `DEVICE-IDS.md` — guest-only
  paths, or paths under a Windows install root.

Gating those means declaring each token with its own reason, keyed by
`(doc, token)`. That is real work and was deliberately not folded into this
change, because the alternative — one blanket exemption per doc — is exactly the
pattern-skip this module exists to prevent. It is the natural next item.

---

## 3. The negative control ran in memory, and every mutation fired

A widened gate that cannot see a bad path in a newly added doc is worse than no
widening, because it reads as coverage. So `read_doc` was monkeypatched per doc,
in a throwaway module instance, to append one sentence naming
`tools/NoSuchFileZZZ.py`, and `test_every_documented_file_path_resolves` was run
against it.

All six fired, and the assertion message named **both** the doc and the token in
every case — a failure that named neither would be a different bug wearing a
green control's clothes.

That was done in memory on purpose. A probe that plants a defect in the working
tree makes any concurrent gate run report named FAILs with real tracebacks and no
actual defect, and the tell (a traceback line number that no longer matches the
file) is easy to miss.

---

## 4. The floors comment was already false, and nothing could see it

`FLOORS` carried the line `Measured 2026-08-13: CLAUDE.md 33/6/4, tests 21/1/3,
phase0-kext 9/2/4, kextshim 8/1/1, tools 37/3/17`. Re-running the extractors the
same day returned `CLAUDE.md 35/6/4`, `tests/README.md 50/8/5`,
`tests/kextshim/README.md 9/1/1`.

The cause is on the filesystem, not in anyone's reasoning: `tests/README.md`
and `tests/kextshim/README.md` have mtimes of **15:15** and **15:13**, against a
measurement taken when `CLAUDE.md` was last touched at **12:15**. The docs moved
and the comment describing them did not.

Nothing was wrong with the *floors*. Every one sits at or under its measured
value, so the gate was never at risk of a false red; the stale text was pure
provenance. It is corrected, and the comment now says outright that it is
provenance for the floors and never an assertion — because a reader who trusts
it as a current measurement is the whole failure mode.

The floors were **not** raised to chase the new numbers. `tests/README.md`
measuring 50 file tokens against a floor of 15 is not slack to be tightened; it
is room for ordinary doc edits. A floor exists to fire when an extractor sees
almost nothing, not to track a count.

The small docs are honest about their own weakness in the comment: `PLAN.md`
names two paths, so its floor is `(1, 0, 0)` and guards only against the
extractor seeing *nothing* there. Drift protection comes from the large docs,
whose floors sit roughly a quarter under measured and would have to lose eight
tokens to fire.

---

## 5. Gate state at the end of the session

All four re-run after the edits, in the order below, Python alone:

| Gate | Result |
|---|---|
| `python tools/check-registers.py` | exit 0, **114/114 passed, 0 failed, 0 skipped** |
| `tests\build-msvc.bat` | exit 0, ctest **5/5 Passed**, 100% of 5 |
| `tests\check-kext-syntax.bat` | exit 0, **sections 1–11 ok**, all **7** negative controls diagnosed |
| `tests\run-python-tests.bat` | exit 0, **Ran 678 tests, OK** |

`test_documented_paths.py` alone: **31 tests, ok**. The suite total did not move,
and that is expected rather than suspicious — the tests loop over `DOCS` inside
each method, so widening the tuple adds assertions, not test cases. The count
line is therefore not evidence about this change either way; §3's mutation run is.

---

## 6. What is still open

- **`OPERATOR-ACTION-step7-build.md`** — one power cycle and one command typed at
  the guest console. Unchanged from handoff10, and still the only open item that
  can answer a question no host-side gate can: does the kext source compile under
  Apple clang.
- `OPERATOR-ACTION-guest-creds.md` / `t-396740` — the account password. Unblocks
  `runScriptInGuest`, `captureScreen`, and the guest→host direction.
- Declaring the guest-only and installer-only tokens in the remaining eight root
  docs, per §2, so the prose-path gate can cover them without a blanket skip.
- Widening `ABSENCE_RE`'s scope so an absence claim inside a table cell reads as
  one. `OPERATOR-ACTION-step7-build.md`'s `build/` is the concrete case.
