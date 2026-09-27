# 3050-handoff26 — the WPR2 refusal nobody consumed

NVIDIA track, 2026-08-18, continuing from `handoff25.md`. This session closes the
second half of `handoff23.md` §5 item 2 — the unpinned publication paths in
`phase0-kext/NV3050Probe.cpp`. `handoff25.md` took the identity path
(`NV_PMC_BOOT_42`); this one takes `checkWpr2()`.

It found a **live defect in shipping code**, not a coverage hole. It is the
fourth occurrence of handoff18 §1d's class and the third in this one shell.

---

## §1 — the defect

`checkWpr2()` returned a bare `bool` meaning *"the firmware boot precondition is
satisfied"*. Two things made that a fabrication rather than a measurement.

**§1a — one bool, two categorically different falses.** The bool was false when
WPR2 is genuinely UP — a previous boot left a write-protected region standing,
and the fix is a full engine reset — and *also* false when the registers did not
decode at all. Only the second is a bug in **this** driver, and it is the one
somebody can go and fix. This is the same argument handoff24 §1a makes for
`IG_SAMPLE_*` on the Intel side, arrived at independently one track over: a
verdict family, not a bool.

**§1b — the sharper half, and the reason this is §1d's class again.**
`regRead32()` returns `0xFFFFFFFF` for every refusal it has (BAR0 unmapped,
offset out of policy). `checkWpr2()` stored **both** words into its
out-parameters *before* any test, and then tested **ADDR_HI only** — ADDR_LO had
no refusal test at all. The caller in `inspectGspEngine()` published both words
unconditionally.

So a refused ADDR_LO beside a healthy ADDR_HI returned `true` — *precondition
satisfied* — and put `0xFFFFFFFF` into the IORegistry as `GSP_WPR2_ADDR_LO`.
`setProperty("GSP_WPR2_ADDR_LO", 0xFFFFFFFF, 32)` is byte-identical to a
register that genuinely holds that value. That is handoff18 §1d's sentence for
the fourth time, and this occurrence is worse than the previous three because
the fabricated value did not merely mislead an operator reading the registry —
it fed the go/no-go join, which then reported a boot precondition as met over a
word nobody measured.

**Why no gate could see it.** The four by-construction blindnesses handoff20 §1,
handoff22 §1, handoff23 §1 and handoff25 §1a each list, unchanged:

1. `tools/check-registers.py` checks **constants**, and both
   `NV_PFB_PRI_MMU_WPR2_ADDR_LO`/`_HI` are correct.
2. No C++ suite can link a translation unit that reaches `IOService`, so nothing
   executes a line of `NV3050Probe.cpp`.
3. `-Werror` has nothing to say: a `bool` return that discards a distinction is
   well-typed, and so is storing a sentinel into a `UInt32 *`.
4. Nothing in the Python gate modelled "a refusal must be consumed before the
   value it qualifies is published".

**§1c — the fix.** A verdict family in `phase0-kext/NVFalcon.hpp`, which is
dependency-free, so host suites can drive it. Putting it in the shell instead is
the mistake handoff20 §1 exists to record.

```cpp
#define NVWPR2_DOWN          0u
#define NVWPR2_UP            1u
#define NVWPR2_UNREADABLE    2u
#define NVWPR2_VERDICT_LIMIT 3u
#define NVWPR2_READ_REFUSED_VALUE 0xFFFFFFFFu
```

plus `nvWpr2ReadRefused()`, `nvWpr2Verdict()` and `nvWpr2VerdictName()`. Four
choices in it are load-bearing:

- **The verdict judges the read GROUP, not the field.** `nvWpr2AddressLo()` and
  `nvWpr2AddressHi()` compose ONE reported range, so a per-word flag would
  publish one good half, one sentinel, and a range arithmetically correct over a
  word that was never measured. Same reasoning as handoff24 §1a's
  `igSampleVerdict()`.
- **`nvWpr2ReadRefused(0u)` is FALSE, deliberately.** Zero in ADDR_HI is exactly
  the healthy DOWN case — the common one. Refusing it would redden every
  well-behaved machine. Note this is the *opposite* polarity to the Intel side,
  where handoff24 §1a records that every unprogrammed page read `0x00000000` and
  all-ones is what a non-decoding block reports. Both tracks end at all-ones as
  the sentinel, by different roads.
- **The sentinel is an equality, never a threshold.** A `>=` test would sweep in
  the top of the address space, which is a legal WPR2 base. `0xFFFFFFFE` is
  pinned NOT refused for exactly this reason.
- **The honest ambiguity is written down rather than fixed.** A half that
  genuinely holds `0xFFFFFFFF` reports UNREADABLE. That is the safe direction:
  all-ones in ADDR_HI decodes to VAL `0x0FFFFFFF`, the maximum page number, so
  believing it publishes a WPR2 based at the top of a 40-bit space as though it
  had been measured. Same shape as handoff22 §1b's decision to write the
  ambiguity into the header rather than thread state through a pure function.

**§1d — publication.** The verdict and the readable flag publish **FIRST and on
BOTH arms**, so the verdict sorts above every value it qualifies and its absence
cannot be mistaken for a healthy read on an older kext — handoff18 §1d's rule
for `GSPEngineReadable` and handoff25's for `NVIdentityReadable`, third time. The
address words and `GSPWpr2Down` publish only inside the readable arm; the
unreadable arm publishes `GSPWpr2UnreadableReason` and no address at all.

And `wpr2Down` is an **equality against DOWN**, never a negation of UP:
`verdict != NVWPR2_UP` is TRUE for UNREADABLE, which would send a precondition
this driver could not measure straight through the go/no-go as satisfied.

---

## §2 — the two observers

**Pure side.** `tests/test_falcon.cpp` gained `testWpr2Verdict()`, **169 → 193**
checks. The separating input is `nvWpr2Verdict(0xFFFFFFFFu, 0x00000000u)` →
UNREADABLE: the old bool returned *precondition satisfied* on exactly this pair.
Also pinned: zero is not refused, `0xFFFFFFFE` is not refused, precedence
(UNREADABLE is neither UP nor DOWN), pairwise distinctness, an exhaustiveness
loop bounded on `NVWPR2_VERDICT_LIMIT` (handoff20 §1's rule, so a family that
grows reddens on the commit that grows it), per-arm name identity, and one past
the family answering the sentinel.

193 was **ABOVE** this entry's own ceiling by eight, so
`tests/test_check_count_floor.py` fired on its own — the band working as designed
on a 14% jump, unlike the last three growths. Re-measured with a dated provenance
paragraph.

**Shell side.** `tests/test_probe_wpr2_publication.py`, **12 tests**: one
positive control, ten negative controls each asserting WHICH property broke, and
one prose/code separation property. All share one `audit()` with the live tree —
the rule from the three sibling probe modules, because a gate whose mutants are
checked by different code than its real input is two gates and only one of them
has ever been seen to fail. Scanning is over **comment-stripped** source, and
here that is mandatory rather than tidy: the fix's own comments name
`GSP_WPR2_ADDR_LO` and the all-ones sentinel — they have to, because the point of
the unreadable arm is what it does *not* publish — so a prose-reading gate would
find the key it exists to forbid in a pristine tree.

The two mutations worth reading are the subtle pair. Hoisting a **second**
publication site above the branch leaves the readable arm intact and correct, so
every per-arm check stays green while the defect is fully restored; only the
whole-function site COUNT catches it. And rewriting `wpr2Down` as a negation of
UP changes nothing else in the file.

---

## §3 — my own gate went red on correct code, for a new reason

First run: 11 of 12, and the failure was
`test_a_bare_boolean_return_inside_checkWpr2_is_caught`. The audit's message said
*"never returns NVWPR2_UNREADABLE"* while the check was implemented as mere
**presence** of the token. Gutting the return to `return false;` leaves
`NVWPR2_UNREADABLE` in the `if (verdict == NVWPR2_UNREADABLE)` comparison three
lines above it, so the token was still there and the check stayed green.

That is an assertion whose message and whose behaviour disagreed — this module's
own subject, one level up. Fixed by matching `return <verdict>;` rather than the
bare identifier. It is the same family as handoff21 §1b and handoff23 §1b: **a
check that cannot distinguish the defect from the fix is an `ok` line.** Where
those two compared against a value the subject can never produce, this one
compared against a value the subject produces on *both* sides of the mutation.

---

## §4 — gate state, all four run after the last edit

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped |
| `tests/build-msvc.bat` | 9 suites, all passed; `test_falcon` **193/193** |
| `tests/check-kext-syntax.bat` | exit **0** |
| `tests/run-python-tests.bat` | **937** tests, OK, exit 0 |

Two readings in there need their own note.

**`ninja: no work to do` after `test_falcon.cpp` grew.** Checked by mtime rather
than trusted: source 12:51:12, binary 12:51:16 — the binary is newer, so ninja
was honest. Then the binary was run directly and printed its own `193/193`.

**The syntax gate prints `FAIL` on a passing run.** Line 194 is the *title*
`=== 11. negative control -- each defect must FAIL ===`. A bare `grep FAIL`
returns a hit on a clean run; read the exit code. Verbatim handoff24 §3.

Python 907 → **937** (+18 from `test_probe_identity_publication.py` in the
previous segment, +12 here).

---

## §5 — payload and card

This session CHANGED shipping code, so `build-payload/` was rebuilt and
`OPERATOR-ACTION-step7-build.md` updated. The staleness was **my own** edit,
which is what makes rebuilding correct here and made it incorrect in handoff23
§3 — never green a gate by vouching for a peer's half-landed work.

Two images: the twenty seventh (the fix) and the twenty eighth (a stray
duplicated `//` divider the twenty seventh's edit left above the go/no-go comment
block). Current: ISO md5 `e0c7da10e52ea3f3a6ad9dedc46d5f51`, 425984 bytes; tar
md5 `c6fd79724f671fb3ec62ee31245c2112`, 348160 bytes.

**Neither size moved on either rebuild** — three consecutive images now with tar
at 348160 and ISO at 425984 while all six md5s differ. tar pads to a 10240-byte
block boundary and both changes fit in the slack the twenty sixth rebuild's new
block left. The twenty eighth is the sharpest case the tree has produced: a
**comment-only** change, which compiles identically and ships differently. If the
size were the assertion, that rebuild would have been invisible and the guest
would have built the previous source.

---

## §6 — what this does NOT establish

1. Nothing here ran on hardware. `nvWpr2Verdict()` has been executed by MSVC on
   Windows against literals; `checkWpr2()` and the publication block have been
   **parsed** and never executed.
2. The `NVWPR2_UNREADABLE` arm, its `NVLOG`, and the whole
   `GSPWpr2UnreadableReason` else-branch are reachable only by parse. Same hole
   handoff22 §3 item 1 records for `IG_READ_PAST_MAPPING`.
3. Deleting the entire `if (wpr2Readable)` split and publishing unconditionally
   would be caught — that is the positive control's whole point — but deleting
   `tests/test_probe_wpr2_publication.py` restores the defect with all four gates
   green. This is the shape gate's inherent limit, identical to handoff24 §5
   item 4.
4. The turing/tu102 offsets are still unverified against GA107. NVIDIA ships no
   `ga102 dev_fb.h`, and the log line in the UNREADABLE arm says so on purpose,
   because "these offsets are wrong for this chip" is one of the two things an
   all-ones read can mean.

---

## §7 — open, most valuable first

1. **One `kextload` on a machine containing the card.** Unchanged from
   handoff22 §4, handoff23 §5, handoff24 §6 and handoff25. Every finding in the
   last six handoffs is a statement about source, and the whole IOKit shell has
   never run.
2. `readIdentity()`'s decode-and-publish path and `publishGspBootResult()` — the
   remaining two of the four items handoff23 §5 item 2 named. The 37-key set and
   the identity path are now closed (handoff25 and this session).
3. The `NVGSP_RESULT_*` publication in `publishGspBootResult()` has never been
   checked for the same class: a result code produced on a path that did not
   perform the reads it summarises.
4. Nothing in the tree tests that `regRead32()`'s refusal value is *the same
   constant* the consumers compare against. `NVWPR2_READ_REFUSED_VALUE` is
   spelled in `NVFalcon.hpp` and `IG_MMIO_INVALID` in `IntelDisplay.hpp`, and
   both are `0xFFFFFFFF` by agreement rather than by construction.
