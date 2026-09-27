# 3050-handoff29 — five words, three of them invented

NVIDIA track, 2026-08-18, continuing from `3050-handoff28.md`. Two things
happened: an open item was **falsified** rather than closed, and the item behind
it turned out to hold a live defect in shipping code.

`phase0-kext/` CHANGED — `NVGspBoot.hpp` and `NV3050Probe.cpp` — so
`build-payload/` was rebuilt and `OPERATOR-ACTION-step7-build.md` updated. That
is the THIRTIETH image.

---

## §1 — the open item that was already done

`3050-handoff28.md` §8 item 3 said:

> **Parse `vbios-ga107.rom` the same way.** It is the last file in
> `reference/ga107-firmware/` that nothing in this tree has ever read.

That sentence is **wrong**, and it was wrong when it was written — by me, in the
previous session. Measured 2026-08-18:

- `tools/parse-vbios.py` exists and decodes the PCI ROM image chain, the BIT
  table, the string table and the falcon ucode table pointer.
- `vbios-report.txt` is the committed output. Regenerating it against the real
  ROM and diffing produced **`diffexit=0`** — byte-identical, so the committed
  evidence is not stale prose, it is reproducible.
- `tests/test_parse_vbios.py` runs **53 tests, zero skips**, and the
  `RealGa107RomTests` class drives the actual 65024-byte file rather than a
  fixture.
- `reference/ga107-firmware/README.md:121` names both the report and the test.

So the last unread file in that directory does not exist. **Do not re-open this
item.** The lesson is small and cheap: an open list inherited across handoffs is
a claim with an age, and "nothing has ever read X" is a set-difference somebody
has to actually run. Two greps and one diff cost four minutes and saved a day of
writing a parser that already existed.

What I did NOT do is edit handoff28 to remove the item. Same rule as its own §7
item 5 — the numbered handoffs are a chronological record, not a status page,
and rewriting one destroys the only evidence of what was believed when.

---

## §2 — the defect, and it is a live one

With item 3 dead, item 4 was next: `publishGspBootResult()`. It holds the FIFTH
occurrence of handoff18 §1d's class and the fourth in `NV3050Probe.cpp`.

`publishGspBootResult()` published five diagnostic register words:

```
NVGspBootCpuctl  NVGspBootHwcfg2  NVGspBootRiscvCpuctl
NVGspBootPrivErrStat  NVGspBootHubErrStat
```

unconditionally, in five bare `setProperty()` calls. Meanwhile `nvGspBootInit()`
zero-initialises all five, and each is assigned **only inside the state that
reads it**:

| Word | Assigned at | Reached when |
|---|---|---|
| `cpuctl` | `NVGSP_STATE_PROBE` | always |
| `hwcfg2` | `NVGSP_STATE_PROBE`, after the halted test | core is halted |
| `riscvCpuctl` | `NVGSP_STATE_ACTIVE_WAIT` | the whole bringup got that far |
| `privErrStat` | `nvGspBootCheckRiscvErrors()` | ACTIVE_WAIT saw NOT-active |
| `hubErrStat` | `nvGspBootCheckRiscvErrors()` | same |

A run that stops at PROBE — core not halted, the first thing that can go wrong —
put **three initialiser zeros into the IORegistry**. `setProperty()` with a
literal zero is byte-identical to `setProperty()` with a register that genuinely
read all-clear, so an operator reading `NVGspBootPrivErrStat = 0` cannot tell
"the RISC-V privilege error latch is clean" from "nothing ever reached for it".
Only the first is a fact about the machine.

### §2a — the separating case does not look like a failure

This is the part worth reading, and it is why the class survived here longer
than in the other four places.

The obvious mental model is "a failed run publishes junk, a successful run
publishes truth". That is backwards. `ACTIVE_WAIT` reads RISCV_CPUCTL, sees
ACTIVE, and stops on `NVGSP_RESULT_RUNNING` at `:805` — **before**
`nvGspBootCheckRiscvErrors()` at `:808` is ever called. `NVGSP_RESULT_RUNNING`
is the SUCCESS terminal (`a-success-terminal-can-look-like-a-failure-code`,
handoff18 §1f), so:

> **The healthy boot is exactly the path that fabricated two of the five words.**

And it is the path an operator is most likely to be looking at when they decide
to trust what the registry says. A defect that only corrupts the failure path is
annoying; one that only corrupts the SUCCESS path is a trap.

---

## §3 — the fix, and its four choices

`NVGspBoot` gained `readMask`, a set of `NVGspBootReadBit` values ORed in **after**
each successful read, plus `nvGspBootWasRead(m, bit)`. The publisher emits
`NVGspBootReadMask` first and then each word only behind its bit.

Four things in that are load-bearing.

**An enum, not `#define`s.** `tools/check-registers.py` parses `#define`s out of
`NVGspBoot.hpp` and checks them against NVIDIA's headers. These bits are not
hardware constants — they describe this driver's own bookkeeping — so spelling
them as `#define`s would file invented values in the one place the tree reserves
for witnessed ones. Same reasoning as handoff20 §1c's choice of which header a
family lives in, one level over. The register gate is correctly **unmoved at
240**.

**The bit is set AFTER the read returns true, never before.** `nvGspBootRead()`
returning false means no value landed — a refused callback, a null `read32`, an
aperture violation. A mask set before the check would report a word as measured
on the exact path where nothing was measured, which is the defect this family
exists to prevent, reintroduced one line up. Pinned by the last block of
`testReadMask()`: a machine with `io.read32 = 0` stops on `ERR_NO_IO` with the
mask still `0`.

**The mask is published on EVERY path and FIRST.** Its absence cannot then be
confused with an older kext, and it sorts above the words it qualifies. Verbatim
the reasoning behind `GSPEngineReadable` (handoff18 §1d) and `NVIdentityReadable`
(handoff25 §1).

**`nvGspBootWasRead(0, bit)` is FALSE.** A caller that cannot see the machine has
not measured anything. Refuse rather than trust, the direction the rest of this
header already takes.

One honest gap, written down rather than fixed: the mask says a read COMPLETED,
not that the value is trustworthy. A read that succeeds against a dead aperture
still returns all-ones and still sets its bit. That is `regRead32()`'s problem
and `nvWpr2ReadRefused()`'s shape (handoff26 §1c), and threading it through here
would couple the pure header to the shell's mapping state.

---

## §4 — the tests

`testReadMask()` in `tests/test_gsp_boot.cpp`, **313 → 341** checks. Six blocks:
the null machine; a fresh `nvGspBootInit()` (mask empty); the five bits asserted
DISJOINT as `0x1F`, because a copy-paste duplicate would make two words share one
flag and the publisher would emit an unmeasured value whenever its twin had been
read; then four runs that separate every boundary in the table above:

| Run | Ends at | Mask | What it separates |
|---|---|---|---|
| A | `RUNNING` (healthy) | `0x07` | the error pair is NOT read on success |
| B | `ERR_ACTIVE_TIMEOUT` | `0x1F` | the only route that reaches the error pair |
| C | `ERR_NOT_HALTED` | `0x01` | CPUCTL's bit from HWCFG2's |
| D | `ERR_NOT_RISCV_CAPABLE` | `0x03` | HWCFG2's bit from RISCV_CPUCTL's |

A and B together are what make the mask a measurement rather than a constant —
no single run distinguishes "the bit tracks the read" from "the bit is always
set". C and D are the pair that catches the copy-paste: a mask that set both
falcon bits at the first read passes C's sibling checks and fails C; one that set
neither until the state completed passes A and fails both C and D.

### §4a — a wrong result code that measured the harness

Run B failed on its first version: `got ERR_STEP_BUDGET, expected
ERR_ACTIVE_TIMEOUT`. Not a defect in the machine and not a defect in the claim —
I had driven it with the DMA path in and the default budget of 200 active polls,
so the **1000-step backstop tripped before the poll budget did**. The run ended
for a reason that has nothing to do with what the check is about.

The existing idiom is `skipFirmwareLoad = true` plus a small `maxActivePolls`,
which is what `testTimeouts()` and run B of `testReadTranslators()` both do, for
this exact reason. Worth carrying: `ERR_STEP_BUDGET` is the one result code that
is always about the fixture rather than the simulated hardware, so when it shows
up unexpectedly the question is what the test asked for, not what the machine
did.

---

## §5 — the non-vacuity proof

One mutation, and it is the copy-paste shape runs C and D exist to catch: set
`NVGSP_READ_HWCFG2` beside `NVGSP_READ_CPUCTL` at the first PROBE read, so the
bit is claimed before the register is touched.

```
FAIL  C: HWCFG2 was NOT read                 got true, expected false
FAIL  C: mask is CPUCTL alone                got 0x3, expected 0x1
```

**Exactly TWO checks, both new, every pre-existing check green.** handoff18 §1e's
signal, first try. The mutation was confirmed on disk by grep (two occurrences of
the token where one belongs) BEFORE the reading was believed, and the rebuild ran
through `tests/build-msvc.bat` rather than a bare `ninja` — `3050-handoff28.md`
§4a is one session old and the trap it records is the same one.

`NVGspBoot.hpp` was mutated and reverted once; the grep count is back to 1 and
the suite is green at 341.

---

## §6 — gate state, all four run after the last edit

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped |
| `tests/build-msvc.bat` | 9 suites, all passed; `test_gsp_boot` **341/341** |
| `tests/check-kext-syntax.bat` | exit **0** |
| `tests/run-python-tests.bat` | **954** tests, OK, exit 0 |

Register gate unmoved at 240 and that is correct — the new family is an enum, so
the gate has nothing to parse. Python 952 → 954, and both are the peer's; the
count-floor edit changes an integer and adds a provenance paragraph, neither of
which is a test.

**Count band.** 341 sits INSIDE this entry's band (313 + max(8, 39) = 352) by
eleven, so the ceiling did **not** fire and the dated provenance paragraph above
`MEASURED` is the only record that a human ran the binary. Contrast
`3050-handoff28.md` §5, where 188 over a 144 band fired correctly.

The syntax gate still prints `FAIL` on a passing run — line 198 is the *title*
`=== 11. negative control -- each defect must FAIL ===`. Read the exit code.

---

## §7 — the payload, and a red gate that WAS mine

`phase0-kext/` changed, so `build-payload/` was rebuilt. Thirtieth image:

| Artifact | Bytes | md5 |
|---|---|---|
| `step7-payload.iso` | 436224 | `1849a1d721a295450b52daa784abb7c8` |
| `kext-src.tar` | 358400 | `a89bad29ffa914d6726fb67f2da74b9b` |

**Neither size moved.** Tar 358400 and ISO 436224 are identical to the twenty
ninth image while both md5s differ — tar pads to 10240-byte blocks, so a change
this size disappears into the padding. Fourth consecutive image where a byte size
vouches for nothing.

The Python gate then went RED on one check,
`test_every_md5_the_card_quotes_can_be_accounted_for`, and unlike
`intel-handoff27.md` §5a the correct action here was the OPPOSITE one: **fix it**.
The staleness was my own — I had rebuilt the payload sixty seconds earlier and
`OPERATOR-ACTION-step7-build.md` still quoted the twenty ninth image's hashes.
Attribution is the whole rule, not "never touch a red gate": attribute by mtime
and by the failure MESSAGE, then act on what you find. Here the message named the
two md5s I had just superseded.

---

## §8 — what this does NOT establish

1. Nothing here ran on hardware. `publishGspBootResult()` is inside the IOKit
   shell, so **no suite executes a line of it** — the five guarded
   `setProperty()` calls are reachable only by clang's front end. What the C++
   suite proves is that `readMask` is correct; that the publisher CONSUMES it is
   proven by parse and by reading, which is exactly the gap `intel-handoff26.md`
   closed on the Intel side with a shape gate. See §9 item 3.
2. The mask records that a read COMPLETED, not that its value is meaningful. A
   successful read of a dead aperture returns all-ones and still sets the bit.
3. `NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED` is still in the enum and the state
   machine has still never had real firmware bytes pass through it.
4. `nvRiscvUcodeIsWellFormed()` is still a bare `bool` with six ways to say no,
   and still nothing in `NV3050Probe.cpp` calls it.
5. `3050-handoff28.md` §8 item 3 is contradicted by §1 above and that file was
   not edited.

---

## §9 — open, most valuable first

1. **The operator's scheduling decision.** Unchanged and still theirs: the laptop
   holds Linux or macOS, not both, so more i915 traces and one `kextload` are
   mutually exclusive. Only the Intel track can produce a picture on the internal
   panel, so the traces are almost certainly worth more.
2. **One `kextload` on a machine containing the card.** Unchanged from handoff22
   §4 through here. Still the only thing that turns eight handoffs of source
   claims into measurements.
3. **A shape gate over `publishGspBootResult()`.** §8 item 1 is the hole this
   session's own fix leaves: delete the five `nvGspBootWasRead()` guards and all
   four gates stay green. `tests/test_probe_wpr2_publication.py` and
   `tests/test_probe_identity_publication.py` are the pattern — one `audit()`
   shared with the live tree, comment-stripped source, one positive control and
   a negative control per property asserting WHICH one broke. This is now the
   highest-value item not blocked on hardware.
4. `readIdentity()`'s decode-and-publish path — the last of the four items
   `handoff23.md` §5 item 2 named.
5. Nothing tests that `regRead32()`'s refusal value is *the same constant* the
   consumers compare against. `NVWPR2_READ_REFUSED_VALUE` in `NVFalcon.hpp` and
   `IG_MMIO_INVALID` in `IntelDisplay.hpp` are both `0xFFFFFFFF` by agreement
   rather than by construction. Unchanged from `3050-handoff26.md` §7 item 4.
