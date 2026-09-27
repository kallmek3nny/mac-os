# 3050-handoff34 — a bool is satisfied by every refusal at once

NVIDIA track, 2026-08-18, continuing from `3050-handoff33.md`. It closes that
file's §8 item 3 — the item named there as the cheapest remaining work, carried
forward unchanged from `3050-handoff32.md` §8 item 4.

`phase0-kext/` is **BYTE-IDENTICAL**. The header was mutated and reverted three
times during §3's non-vacuity proof and ends the session as it started, so
`build-payload/` was NOT rebuilt and the THIRTY THIRD image is still current.
The proof of that is the Python gate's silence, not an assertion: it compares
`build-payload/kext-src.tar` byte-for-byte against the live tree, and it is green
at 980.

---

## §1 — the item, and why `false` was the whole problem

`3050-handoff33.md` §8 item 3, verbatim:

> **Assert the real booters against the verdict, not the bool.** Carried forward
> unchanged from `3050-handoff32.md` §8 item 4 and now the cheapest item on
> this list. `testRealFirmwareFiles()` opens all three files in
> `reference/ga107-firmware/` and checks `nvRiscvUcodeIsWellFormed()` on them;
> changing those to `checkVerdict()` would pin that a real booter returns
> `ERR_DESC_VERSION` specifically, turning handoff27's measurement into a
> standing assertion.

True as written, and the second inherited item in two sessions to survive being
measured. What the section asserted before this session, at
`tests/test_riscv_ucode.cpp`:

| Subject | Assertion | Satisfied by |
|---|---|---|
| `bootloader-570.144.bin` | `IsWellFormed == true` | `NV_RISCV_UCODE_OK` only |
| `booter_load-570.144.bin` | `IsWellFormed == false` | **all seven refusal codes** |
| `booter_unload-570.144.bin` | `IsWellFormed == false` | **all seven refusal codes** |

The accept side was already tight — `true` has exactly one preimage, because
`nvRiscvUcodeIsWellFormed()` is an EQUALITY against `NV_RISCV_UCODE_OK`
(handoff32 §3's choice, and this is the first thing that has depended on it).
The refusal side had seven, and the seven are not interchangeable.

`3050-handoff27.md` §2 measured why. Both booters carry an `nvfw_hs_header_v2`
where the bootloader carries an `RM_RISCV_UCODE_DESC` — same container magic,
same `hdrOffset` of 24, first word 60 (`0x3C`) which is that header's own
`header_size` and not a structure version at all. Before handoff27's fix the
function refused them anyway, but for the wrong reason: three optional-region
rows deep, because signature bytes at `+0x24` happened to decode to a size
outside the payload. **Both readings satisfy `== false` identically.** One is a
fact about the file's structure; the other is a coincidence of its signature
bytes, and a file whose signature happened to decode in range would have been
ACCEPTED and DMA'd into IMEM as though it were bootloader code.

So the assertion that was green through handoff27, handoff28's real-file
section, and handoff32's verdict family could not tell handoff27's fix from the
bug handoff27 fixed. That is `a-test-can-pin-the-collapse-instead-of-the-fact`
one level up: there a test ratified a dead arm it had itself explained, here a
test ratifies a refusal without asking which refusal.

---

## §2 — the change, and why the bool assertions were KEPT

Three `checkVerdict()` calls added, three `checkBool()` calls retained beside
them. **211 → 214.**

```cpp
checkVerdict("THE REAL BOOTLOADER IS ACCEPTED",
             nvRiscvUcodeVerdict(gFile, blLen), NV_RISCV_UCODE_OK);
checkBool("  and the wrapper agrees on the real file",
          nvRiscvUcodeIsWellFormed(gFile, blLen), true);
```

and per booter:

```cpp
checkVerdict(label, nvRiscvUcodeVerdict(gFile, len),
             NV_RISCV_UCODE_ERR_DESC_VERSION);
checkBool(label, nvRiscvUcodeIsWellFormed(gFile, len), false);
```

The label spells the claim — `booter_load-570.144.bin IS REFUSED for a VERSION
that is not 5` — because `checkVerdict()` prints the NAME on both arms, so a
regression reads *got optional region outside payload, expected descriptor
version not 5* rather than two integers.

### Keeping the bool is the load-bearing choice

The obvious edit is to REPLACE each `checkBool()` with a `checkVerdict()`, and
it is wrong in a way this tree has been bitten by before.
`nvRiscvUcodeIsWellFormed()` is what the IOKit shell actually calls —
`NVGspStage.hpp:273` inside `nvGspStagePlan()`, reached from
`loadGspFirmware()` (handoff32 §1 ran that grep). It is a one-line wrapper, and
a wrapper that stopped consulting the verdict at all would leave a
verdict-only suite fully green while the shell's answer came from nowhere.

The PAIR is what pins the wrapper to its own subject. On each of the three real
files the suite now asserts both the specific code and that the bool agrees
with it, which is a joint claim neither half makes alone.

### Nothing about the synthetic half changed

handoff28 §2 split this suite into a synthetic half and a real-file half, and
the real-file half SKIPS with a banner and zero checks when
`reference/ga107-firmware/` is absent. A reading of 211 on a bare checkout is
therefore still correct rather than a regression, exactly as 144 was; the three
new checks live entirely inside the skippable section.

---

## §3 — the non-vacuity proof, and two mutations that proved nothing

This is the part worth reading, because the first two mutations both looked
like proof and were not. Every mutation was planted in
`phase0-kext/NVRiscvUcode.hpp`, rebuilt through `tests/build-msvc.bat`, and run
as the built binary — never as a source read.

| Mutation | Failures | Pre-existing among them |
|---|---|---|
| A — version arm returns `ERR_HEADER` | 3 | **1** (`descriptor version 6 -> ERR_DESC_VERSION`) |
| B — accept word 60 as a version too | 3 | **1** (`and is rejected`) |
| C — move the version test BELOW the region walk | **2** | **0** |

Only C discriminates, and it is the one that matters because **C is literally
the pre-handoff27 ordering.** With the version test after the six-row optional
region walk, both booters refuse at `ERR_REGION_OUTSIDE` — the accidental
refusal handoff27 diagnosed — every synthetic check stays green, and exactly
the two new booter assertions redden:

```
FAIL  booter_load-570.144.bin IS REFUSED for a VERSION that is not 5 \
      got optional region outside payload, expected descriptor version not 5
FAIL  booter_unload-570.144.bin IS REFUSED for a VERSION that is not 5 \
      got optional region outside payload, expected descriptor version not 5
214 checks, 2 failures
```

That is handoff18 §1e's signal in its cleanest form yet: 3 → 3 → **2**, with
the count going DOWN as the mutation gets sharper. A and B each reddened a
synthetic check the suite already had, so both were evidence the OLD tests
sufficed for what they perturbed. C leaves every pre-existing check green and
fails only the new ones, which is the only shape that proves added reach.

Worth stating plainly because it inverts the intuition: **a mutation that
reddens more checks is usually weaker evidence.** A reddened pre-existing check
means the behaviour you broke was already observed.

### §3a — why A and B failed to isolate

- **A** changed the code the version arm RETURNS, and
  `testDescriptorVersion()` already drives a synthetic descriptor with version
  6 and asserts `ERR_DESC_VERSION`. So A perturbs a synthetic input as much as
  a real one; the real-file failures ride along.
- **B** widened the accepted set to include 60, which makes the booters get
  past the version gate and refuse further down — but it also changes the
  synthetic `and is rejected` check, because that fixture's descriptor version
  is not 5 either. The tell is that B's failure line reported
  `optional region outside payload`, i.e. it had already reproduced C's
  behaviour on the real files while ALSO touching a synthetic one.

C changes only the ORDER of two refusals, both of which the function already
returns for something, so no synthetic blob in the suite reaches a different
verdict than it did before.

### §3b — the header ends byte-identical

Three mutations planted, three reverted, and the suite is back to **214 checks,
0 failures**. The proof that `phase0-kext/` is clean is not the revert edits — it
is the Python gate at 980/OK, whose payload comparison reads
`build-payload/kext-src.tar` byte-for-byte against the live headers. A stale
revert would have reddened it, exactly as `editing-a-kext-header-reddens-the-python-gate`
records.

---

## §4 — gate state, all four run after the last edit

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped, exit 0 |
| `tests/build-msvc.bat` | **9/9** suites passed, exit 0 |
| `tests/check-kext-syntax.bat` | exit **0** |
| `tests/run-python-tests.bat` | **980** tests, OK, exit 0 |

Register gate unmoved at 240, correctly: nothing added a `#define`. Python
unmoved at 980, correctly: no Python test was added — `tests/test_check_count_floor.py`
changed one dict VALUE and gained a provenance paragraph, which is a table edit
rather than a new test.

`test_riscv_ucode` 211 → **214**, well INSIDE its own band
(211 + max(8, 26) = 237) by twenty three, so the count-floor ceiling did not
fire — correctly, since three assertions over existing fixtures is not a new
section. Third movement of this entry in three days; the provenance paragraph
records all three.

### §4a — a batch invocation that failed for the usual reason

The first attempt at the C++ gate was `cmd //c build-msvc.bat` and returned
*'build-msvc.bat' is not recognized*. That is
`cmd-slash-slash-c-mangles-batch-invocation` again — invoke `./build-msvc.bat`
directly from the `tests/` directory. Recorded because it cost a round trip and
the error text blames the file rather than the invocation.

The Python gate's `tail` also showed an HFS catalog dump rather than a verdict,
because one of the tool tests prints its own parse output. Read the gate by
grepping for `Ran N tests` / `OK` / `FAILED` and the carried exit status, never
the last lines of the log.

---

## §5 — what this does NOT establish

1. **Nothing here ran on hardware.** The real files are read from disk by a
   host suite; no register was touched.
2. **The verdict still does not reach the IORegistry.** `loadGspFirmware()`
   returns `bool` and the eight-way verdict dies in the log — handoff32 §7
   item 2 unchanged, and the same class §1 of handoff33 closed for BOOT_0 one
   subsystem over. §6 item 2.
3. **`vbios-ga107.rom` is still the only file in `reference/ga107-firmware/`
   with no verdict assertion of this kind**, because it is not a RISC-V ucode
   container at all — `tools/parse-vbios.py` covers it separately
   (handoff29 §1).
4. **The three new checks are inside the skippable section.** On a checkout
   without `reference/ga107-firmware/` the suite prints its banner and reports
   211, and none of §1's claims are tested at all.
5. Nothing published here changes what happens on the internal panel. Per
   `3050-handoff27.md` §1 the laptop is muxless, so this is Phase 4/5
   engineering and is not on the critical path to a picture.

---

## §6 — open, most valuable first

1. **The operator's scheduling decision.** Unchanged and still theirs: the
   laptop holds Linux or macOS, not both, so more i915 traces and one
   `kextload` are mutually exclusive. Only the Intel track can produce a
   picture on the internal panel, so the traces are almost certainly worth
   more.
2. **One `kextload` on a machine containing the card.** Unchanged from
   handoff22 §4 through here. Still the only thing that turns thirteen handoffs
   of source claims into measurements.
3. **The GSP refusal reason is not published.** Carried forward from handoff33
   §8 item 5 and now the highest-value item not blocked on hardware.
   `loadGspFirmware()` returns `bool`; `nvGspStagePlan()`'s stage verdict and
   the eight-way ucode verdict underneath it both die in `NVLOG`. This is
   exactly the class handoff31 and handoff33 closed for BOOT_42 and BOOT_0 —
   a diagnosis reaching the log and not the consumer — with the detection
   already built and tested. The publication side is the missing half.
4. **Nothing pins that `decodeBoot0()`'s log and `publishIdentity()`'s registry
   agree.** Carried forward from handoff33 §8 item 4. Both read `boot0` and
   both decide the same four-way question, one in prose and one through
   `nvPartVerdict()`. A gate could require `decodeBoot0()` to call the verdict
   function too, making the agreement structural rather than conventional.
5. **`NVChipIdExpected` still states the target without comparing.**
   handoff33 §7 item 4. `NVPartIsTarget` answers the question it implied, but
   the two properties sit side by side and a reader could still take the chip
   ID for a measurement.
6. Nothing tests that `regRead32()`'s refusal value is *the same constant* the
   consumers compare against. `NVWPR2_READ_REFUSED_VALUE` in
   `phase0-kext/NVFalcon.hpp` and `IG_MMIO_INVALID` in
   `phase0-kext/IntelDisplay.hpp` are both `0xFFFFFFFF` by agreement rather
   than by construction. Unchanged from `3050-handoff26.md` §7 item 4.
