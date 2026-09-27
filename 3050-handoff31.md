# 3050-handoff31 — the driver diagnosed the dead read and published it anyway

NVIDIA track, 2026-08-18, continuing from `3050-handoff30.md`. It closes that
file's §6 item 3 — `readIdentity()`'s decode-and-publish path, the last of the
four items `handoff23.md` §5 item 2 named — and the closing found a **live defect
in shipping code**.

`phase0-kext/NV3050Probe.cpp` CHANGED, so `build-payload/` was rebuilt and
`OPERATOR-ACTION-step7-build.md` updated. That is the **THIRTY FIRST** image.

---

## §1 — the defect, and why it is the sharpest of the six

This is the **sixth** occurrence of handoff18 §1d's fabricated-publication class
and the **fifth** in `NV3050Probe.cpp`. What makes it different from the other
five is that the driver **already knew**.

`decodeBoot42()` at `:456` opens with this arm:

```cpp
if (nvLooksLikeDeadRead(value)) {
    NVLOG("NV_PMC_BOOT_42 = 0x%08x -- unexpected dead read at 0x%x while "
          "NV_PMC_BOOT_0 responded. Aperture may be shorter than 16 MiB.",
          (unsigned)value, (unsigned)NV_PMC_BOOT_42);
    return;
}
```

So the case is named, diagnosed, and its likely cause written down. The register
is not decoded and no field is derived from it. And then:

- `readIdentity()` at `:400`–`:406` reads BOOT_42, stores it through `outBoot42`,
  calls `decodeBoot42()` **discarding nothing because it returns void**, and
  returns `true`;
- `start()` at `:175` sees `true` and calls `publishIdentity(boot0, boot42)`;
- `publishIdentity()` at `:922` published `setProperty("NV_PMC_BOOT_42", boot42, 32)`
  **unconditionally**.

The log says *unexpected dead read*. The IORegistry says `NV_PMC_BOOT_42 =
0xFFFFFFFF`, byte-identical to a chip that genuinely reports all-ones. An
operator reading the registry — which is what `IORegistryExplorer` shows and what
every previous handoff describes as the inspection surface — cannot tell a short
BAR0 aperture from a real value. Only one of those is a fact about the machine.

The previous five occurrences were all *unknowing*: a value nobody read, a
refusal nobody consumed, an initialiser zero. This one is a **diagnosis that
reached the log and not the registry**, which is worse, because the tree already
contained the predicate and the prose explaining why the case matters.

### §1a — why the four gates were blind, again

The same four by-construction reasons, restated because they keep holding:

- **register gate** — checks `#define`s against NVIDIA's headers. Both constants
  involved are correct. The relation between a decode verdict and a publication
  is not a constant. Unmoved at 240.
- **C++ suites** — cannot compile `NV3050Probe.cpp` at all; it pulls in IOKit's
  own `IOService` declarations. `nvLooksLikeDeadRead()` itself has coverage in
  `tests/test_decode.cpp:274`–`:276`, which is exactly the trap: a **heavily
  tested predicate with a caller that ignores it** passes every suite it has.
  `intel-handoff26.md` §1's sharpest blindness, one track over, verbatim.
- **syntax gate** — `setProperty()` with a well-typed `UInt32` is well-formed
  C++. clang has nothing to say.
- **Python gate** — `tests/test_probe_identity_publication.py` existed and was
  green. Its four properties were all about BOOT_42 on the **unreadable** path
  (`publishIdentityUnreadable` must not publish it, `readIdentity`'s dead-read
  branch must not fabricate it, `start()` must branch, exactly one publication
  site). **Nothing asked what the readable path does with a dead BOOT_42**,
  because the previous session's subject was BOOT_0.

---

## §2 — the fix, and the one choice worth arguing about

Two edits, both in `publishIdentity()`/`publishIdentityUnreadable()`:

```cpp
bool boot42Readable = !nvLooksLikeDeadRead(boot42);

setProperty("NVBoot42Readable", boot42Readable);

if (boot42Readable) {
    setProperty("NV_PMC_BOOT_42", boot42, 32);
} else {
    setProperty("NVBoot42UnreadableReason", "...");
}
```

plus `setProperty("NVBoot42Readable", false);` on the unreadable arm.

**The choice worth arguing about is that `readIdentity()` still returns `true`.**
The obvious fix is to return `false` on a dead BOOT_42 and let `start()` route to
`publishIdentityUnreadable()`. That is wrong, and it is wrong in the direction
this project cares about: BOOT_0 **decoded**. `NVArchitecture`,
`NVImplementation` and the PCI config words below it are real measurements, and
throwing them away would replace one fabricated word with four discarded true
ones. `publishIdentityUnreadable()`'s own reason string says BOOT_42 "was never
read" — which would become a lie on a path that read it and got a dead answer.

So **the split belongs at publication, not at the read**. That is a genuinely
different shape from `3050-handoff26.md`'s WPR2 verdict (where the two halves
compose one reported range and a partial answer is meaningless) and worth
keeping distinct: here the two registers are independent measurements that happen
to be read together, so one dying does not poison the other.

Three smaller choices, each load-bearing:

- **The flag is published FIRST and on BOTH arms.** `GSPEngineReadable`
  (handoff18 §1d), `NVIdentityReadable` (handoff25 §1) and `NVGspBootReadMask`
  (3050-handoff29 §3) for the fourth time. Its absence must not be readable as an
  older kext, and it must sort above the word it qualifies.
- **The guard is `!nvLooksLikeDeadRead(boot42)`, not a hand-rolled comparison.**
  An equality against `0xFFFFFFFF` alone accepts the all-zeroes half, and
  `NVDecode.hpp:187` is explicit that all-zeroes means the mapping succeeded and
  points at nothing — which is the **wrong BAR**, the more dangerous of the two.
- **The else arm publishes a reason string rather than nothing.** An absent key
  is a question; a reason is an answer. Same reasoning as
  `NVIdentityUnreadableReason` and `GSPWpr2UnreadableReason`.

---

## §3 — the gate, extended rather than added

`tests/test_probe_identity_publication.py` gained **property 5** and four
negative controls, 11 → **15** tests. Extended rather than replaced, because the
subject function is the same one its four existing properties already parse, and
a second module over `publishIdentity()` would duplicate `audit()` — the thing
every module in this family deliberately shares with the live tree.

Property 5 pins four things:

| Claim | Why it is not redundant |
|---|---|
| the guard is DERIVED from `!nvLooksLikeDeadRead(boot42)` | a reconstructed predicate accepts the all-zeroes half |
| `NV_PMC_BOOT_42` occurs only INSIDE the guarded span | this is the defect itself |
| `NVBoot42Readable` occurs OUTSIDE it, and BEFORE it | a flag published only where the word was readable is always true and carries no information |
| the unreadable arm publishes the flag false | BOOT_42 was never read there — strictly less readable than dead |

The span is found by `guarded_span()`, which locates `if (<cond>) {` and matches
the brace by a **balanced walk**. Not a character class excluding the statement
terminator: `handoff25.md` §1c records that such a class runs past the `if`'s
closing paren and the entire guarded body to land on the next statement's
semicolon, and matches CORRECT code as a violation.

### §3a — the controls, and what they establish

Four mutations, each in memory, each asserting WHICH property broke:

1. **hoist the publication out of the guard** — the defect restored. Reddens the
   guard check.
2. **`(boot42 != 0xFFFFFFFF)` instead of the predicate** — reddens the derivation
   check. This is the all-zeroes half, the one that means the wrong BAR.
3. **move the flag inside the guard** — reddens the both-arms check.
4. **delete the flag from the unreadable arm** — reddens the unreadable-arm check.

Every anchor goes through `mutate()`, which asserts the occurrence COUNT before
replacing — `a-mutation-that-matches-nothing-prints-ok` in both directions.

Unlike an all-new module, handoff18 §1e's rule DOES apply here: the four
controls were run against a module with eleven pre-existing checks, and each
reddened only its own property with **every pre-existing check green**.

---

## §4 — gate state, all four run after the last edit

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped, exit 0 |
| `tests/build-msvc.bat` | **9/9** suites passed, exit 0 |
| `tests/check-kext-syntax.bat` | exit **0** |
| `tests/run-python-tests.bat` | **974** tests, OK, exit 0 |

Python 970 → **974**, and the +4 is exactly this session's four controls —
measured by running the module alone (15 tests) before the gate. Register gate
unmoved at 240, correctly: no constant was added, and `NVBoot42Readable` is a
property KEY rather than a `#define`. C++ unmoved at 9 suites with every check
count unchanged, correctly: no header and no test `.cpp` was touched, so
`tests/test_check_count_floor.py` needed no edit.

The syntax gate still prints `FAIL` on a passing run — line 227 is the *title*
`=== 11. negative control -- each defect must FAIL ===`. Read the exit code.

---

## §5 — the payload, and an attribution that came out my way

`phase0-kext/` changed, so `build-payload/` was rebuilt. Thirty first image:

| Artifact | Bytes | md5 |
|---|---|---|
| `step7-payload.iso` | 446464 | `fda4f4ab7672ad94635793674095c7fe` |
| `kext-src.tar` | 368640 | `a4cc43caaa267c25a557da07baa79899` |

**Both sizes moved this time** — tar 358400 → 368640 and ISO 436224 → 446464,
exactly one 10240-byte tar block each. That ends a four-image run where a byte
size vouched for nothing, and it is a coincidence rather than a signal: the fix
is ~1.4 KiB of code and comment, which happened to cross a padding boundary the
last four changes did not. The md5 is still the only fingerprint.

The operator card was stale and the staleness was **mine**: card last written
16:50:11, my `NV3050Probe.cpp` edit 17:29:27, the rebuild 17:34:59. Same call as
`3050-handoff29.md` §7 and the opposite of `intel-handoff27.md` §5a — attribute
by mtime and by the failure MESSAGE, then act on what you find. Here every
timestamp was inside this session.

---

## §6 — what this does NOT establish

1. **Nothing here ran on hardware.** `publishIdentity()` is inside the IOKit
   shell, so no suite executes a line of it. That the publisher consumes the
   predicate is proven by parse, which is exactly what §3 exists to be.
2. `nvLooksLikeDeadRead()` treats all-ones and all-zeroes as dead. A register
   that genuinely holds either value publishes `NVBoot42Readable false`. That
   ambiguity is inherent and is written into the reason string rather than
   fixed — same shape as `nvWpr2ReadRefused()` (3050-handoff26 §1c) and
   `igSampleVerdict()` (handoff24 §1a).
3. The `NVBoot42UnreadableReason` arm has never executed. Neither has the flag on
   either path.
4. `NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED` is still in the enum and the state
   machine has still never had real firmware bytes pass through it.
5. `nvRiscvUcodeIsWellFormed()` is still a bare `bool` with six ways to say no,
   and still nothing in `NV3050Probe.cpp` calls it.

---

## §7 — open, most valuable first

1. **The operator's scheduling decision.** Unchanged and still theirs: the laptop
   holds Linux or macOS, not both, so more i915 traces and one `kextload` are
   mutually exclusive. Only the Intel track can produce a picture on the internal
   panel, so the traces are almost certainly worth more.
2. **One `kextload` on a machine containing the card.** Unchanged from handoff22
   §4 through here. Still the only thing that turns ten handoffs of source claims
   into measurements.
3. **`nvRiscvUcodeIsWellFormed()` returns a bare `bool` with six distinct refusal
   reasons behind it, and nothing in the IOKit shell calls it at all.** Both
   halves are worth fixing and the second is the one that matters — a validator
   with no caller is `intel-handoff26.md` §1's tested-function-with-no-callers in
   its purest form, and this session's defect is the same family (a predicate
   whose answer reached the log and not the consumer). This is now the
   highest-value item not blocked on hardware.
4. **`decodeBoot0()`'s and `decodeBoot42()`'s log-only diagnoses.** This session
   fixed ONE of them. `decodeBoot0()` at `:432` detects a non-Ampere architecture
   and an Ada part specifically, and at `:447` detects a non-GA107
   implementation — all four findings go to `NVLOG` and none reaches the
   IORegistry. The class is now named; the sweep has not been done.
5. Nothing tests that `regRead32()`'s refusal value is *the same constant* the
   consumers compare against. `NVWPR2_READ_REFUSED_VALUE` in
   `phase0-kext/NVFalcon.hpp` and `IG_MMIO_INVALID` in
   `phase0-kext/IntelDisplay.hpp` are both `0xFFFFFFFF` by agreement rather than
   by construction. Unchanged from `3050-handoff26.md` §7 item 4.
