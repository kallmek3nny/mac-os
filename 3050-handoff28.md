# 3050-handoff28 — the fixture, opened

NVIDIA track, 2026-08-18, continuing from `3050-handoff27.md`. This session
closes that file's §8 item 3, which it named as the highest-value item not
blocked on hardware.

It found no defect in shipping code. What it found is that a claim recorded in
`handoff18.md` about the real firmware is **wrong**, and that the suite could
not have discovered that either, for the same reason it could not discover the
booter shape.

`phase0-kext/` is BYTE-IDENTICAL after this session. Nothing shipped changed.

---

## §1 — what was open, and why it was worth doing

`3050-handoff27.md` §7 item 2 was honest about the hole its own fix left:

> **The suite still does no file I/O.** The version gate is driven by a
> synthesised blob carrying a version word I typed, not by
> `booter_load-570.144.bin` itself. The real-file parse that FOUND this ran in a
> scratch script and is gone.

So the defect was found by parsing the real firmware, fixed, and then observed
exclusively by blobs the suite builds for itself. A blob-type regression would
have been caught; a change to the files on disk would not, and nothing in the
tree had ever asserted that the firmware in `reference/ga107-firmware/` is the
firmware the constants describe.

---

## §2 — the section

`tests/test_riscv_ucode.cpp` gained `testRealFirmwareFiles()`, **144 → 188**
checks, and `tests/CMakeLists.txt` gained one line.

The suite is now explicitly two halves and its header comment says so. Everything
through `testDescriptorVersion()` is synthetic and unchanged; the new section
opens the actual files and **SKIPS — banner, zero checks, no failure — when the
directory is absent**. That preserves the portability property the old header
comment described (`test_riscv_ucode.cpp:6`, "deliberately NO FILE I/O"), which
was a correct decision for a bare checkout and a total blind spot for blob-type
discovery. The two are not in conflict; the old comment simply stated a
constraint as though it were the whole design.

A reading of 144 on a tree without the firmware is therefore CORRECT and not a
regression. The skip banner is the record of which reading you are looking at.

**The one choice worth reading is where the path comes from.** It is injected by
CMake as an absolute `NV_FIRMWARE_DIR`:

```cmake
set(NV_FIRMWARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../reference/ga107-firmware")
...
target_compile_definitions(${name} PRIVATE NV_FIRMWARE_DIR="${NV_FIRMWARE_DIR}")
```

A path relative to the binary's cwd would depend on where `ctest` was invoked
from — and a wrong path SKIPS rather than fails. That is the vacuity direction,
and it is the worst failure mode available to a section whose entire purpose is
to read files: it would print a banner, add nothing, and exit 0 forever. Same
family as `a-mutation-that-matches-nothing-prints-ok`, one layer up.

What it pins, driven by bytes rather than by literals I typed:

- The bootloader's container geometry against every `NV_RISCV_GA107_BL_*`
  constant in the header — file length, magic, `hdrOffset`, `dataOffset`,
  `dataSize`, 84 descriptor bytes, 21 fields, no SMP flags.
- **`nvRiscvDescVersion()` returning 5 off the real file.** This is the check
  that makes the section worth its cost: yesterday's fix was proven only against
  a version word this project wrote into a buffer.
- Both booters: container header **VALID**, `hdrOffset` 24, `dataOffset` 888, a
  864-byte middle region — **216 u32 slots**, which is the tell, since
  `RM_RISCV_UCODE_DESC` has 23 in the newest driver this tree has read — word 0
  equal to 60, that value asserted DISTINCT from 5, and the whole blob refused.

---

## §3 — the measurement that contradicts the tree

`handoff18.md` §1c records that the real bootloader leaves all six optional
regions zero, and `buildBlob()` reproduces exactly that. Measured 2026-08-18
from `bootloader-570.144.bin`, **three of them are not zero**:

| Row | Offset | Size |
|---|---|---|
| `riscvElf` | `0x00000000` | `0x00000000` |
| `manifest` | `0x00000000` | **`0x00000800`** |
| `monitorData` | **`0x00000800`** | **`0x00001000`** |
| `monitorCode` | **`0x00001800`** | **`0x00002900`** |
| `swbromCode` | `0x00000000` | `0x00000000` |
| `swbromData` | `0x00000000` | `0x00000000` |

`bIsMonitorEnabled` is **1**. Only the ELF row and the two swbrom rows are
genuinely zero.

That is not a bug in `NVRiscvUcode.hpp` — the predicate accepts the real file,
and accepted it before this session. It is a bug in what the tree BELIEVES about
its own evidence, and it has a consequence: the six-row loop that handoff18 §1c
generalised was exercising a blob **strictly simpler** than the one that ships.
Five of six rows carrying zero was treated as the firmware's shape when it was
the fixture's shape. Same class as the booter finding one level down — a
synthetic input cannot disagree with the belief that produced it.

All six rows plus the monitor-enabled flag are now pinned against the file.

---

## §4 — the non-vacuity proof, and a reading that lied

Two mutations, and the FIRST one is the lesson.

**Mutation A — `NV_RISCV_GA107_BL_BOOTLOADER_OFFSET` `0x5000` → `0x5004`.**
Reddened 4 checks, **three of them pre-existing**. That is a LOUD mutation and it
proves the opposite of what it looks like: the old synthetic tests already
covered that constant, because `buildBlob()` writes it and the assertions read it
back. It says nothing about whether the new section reaches anything.
handoff18 §1e's rule, hit again — a mutation that reddens pre-existing checks is
evidence the OLD tests sufficed.

**Mutation B — cap `nvRiscvDescFieldCount()` at
`NV_RISCV_DESC_FIELDS_THROUGH_PLIC`.** Reddened exactly **TWO** checks, both new,
**every pre-existing check green**:

```
FAIL  booter_load-570.144.bin middle region is 216 u32 slots   got 0x17, expected 0xD8
FAIL  booter_unload-570.144.bin middle region is 216 u32 slots got 0x17, expected 0xD8
```

That is the separating property stated as arithmetic: **no synthetic blob in this
suite can notice a cap at 23**, because none of them has more than 23 slots. Only
a real file carries 216. `NVRiscvUcode.hpp` was mutated and reverted twice and is
byte-identical, md5 `b20dc7dbfad14aff7b8c86adcf1c2904` before and after.

**§4a — the third reading, which was mine and was false.** Mutation A first
appeared to redden NOTHING — 188 checks, 0 failures, on a header I had just
edited. It was rebuilt with a bare `ninja`, which in this shell fails on `cstdio`
for want of an MSVC `INCLUDE` and left the OLD binary sitting in `tests/build/`.
The suite then ran happily against code that no longer existed on disk.

That is `3050-handoff27.md` §4 recurring inside the very session that recorded
it, and it is worse here than there: **a no-op mutation reads exactly like proof
that a test is vacuous.** I nearly concluded the new section had no reach. Run
the gate, not the generator — `tests/build-msvc.bat` does its own vcvars setup
and a bare `ninja` measures the environment.

---

## §5 — gate state, all four run after the last edit

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped |
| `tests/build-msvc.bat` | 9 suites, all passed; `test_riscv_ucode` **188/188** |
| `tests/check-kext-syntax.bat` | exit **0** |
| `tests/run-python-tests.bat` | **952** tests, OK, exit 0 |

The register gate is unmoved at 240 and that is correct — no `#define` it parses
was added; the section pins values, not constants.

Python is unmoved at 952, also correct: `test_check_count_floor.py` gained a
provenance paragraph and one changed integer, neither of which adds a test.

The syntax gate still prints `FAIL` on a passing run — line 198 is the *title*
`=== 11. negative control -- each defect must FAIL ===`. Read the exit code.
Verbatim `3050-handoff27.md` §5.

**Count band.** 188 is ABOVE this entry's band (144 + max(8, 14) = 158) by
thirty, so the ceiling **FIRED** on this reading, correctly: 44 new checks over a
144-check suite is a 31% jump, which is exactly the magnitude the ceiling exists
to make somebody look at. Contrast the last four growths, which all landed just
under their edges and left the provenance paragraph as the only record.

---

## §6 — payload NOT rebuilt, and why that is right here

`phase0-kext/NVRiscvUcode.hpp` has an mtime of 16:06 from the mutation revert,
and md5 `b20dc7dbfad14aff7b8c86adcf1c2904` — identical to the content the
14:29 payload was built from. The Python gate compares `build-payload/kext-src.tar`
byte-for-byte and is green, so the twenty ninth image is still current:
ISO `fda15fc83b16d70cbb8a11d9220e5833`, tar `5de35bb7e42133fe8b6b228a27376dd7`.

Rebuilding would have produced a thirtieth image differing only in the tar's
internal timestamps — a new md5 vouching for no change at all. The rule that
decides this is CONTENT, not mtime, and the gate is the one that reads content.

---

## §7 — what this does NOT establish

1. Nothing here ran on hardware. Three files were parsed on Windows by MSVC; no
   firmware image has ever been DMA'd anywhere.
2. **`vbios-ga107.rom` is still unparsed by anything in the tree.**
   `3050-handoff27.md` §8 item 3 said the same question should be asked of it,
   and this session did not ask it. It is 65024 bytes and has sat beside the
   three blobs since 2026-08-07.
3. The section reads the files that are on disk TODAY. If somebody replaces them
   with a different driver release, twenty-odd checks redden and every one of
   those failures will be correct about the arithmetic and wrong about the
   intent. That is deliberate — a firmware swap should be a decision, not a
   silent input change — but whoever does it needs to know the failures are the
   gate working.
4. `nvRiscvUcodeIsWellFormed()` is still a bare `bool` with six ways to say no.
   Unchanged from `3050-handoff27.md` §7 item 4 and still not worth fixing until
   something calls it: nothing in `NV3050Probe.cpp` does.
5. handoff18 §1c's all-zero claim is CONTRADICTED by §3 above but that file was
   not edited — the numbered handoffs are a chronological record, not a status
   page, and rewriting one destroys the only evidence trail of what was believed
   when.

---

## §8 — open, most valuable first

1. **The operator's scheduling decision.** Unchanged and still theirs: the laptop
   holds Linux or macOS, not both, so more i915 traces and one `kextload` are
   mutually exclusive. `3050-handoff27.md` §1 establishes only the Intel track
   can produce a picture on the internal panel, so the traces are almost
   certainly worth more.
2. **One `kextload` on a machine containing the card.** Unchanged from handoff22
   §4 through here. Still the only thing that turns seven handoffs of source
   claims into measurements.
3. **Parse `vbios-ga107.rom` the same way.** §7 item 2. It is the last file in
   `reference/ga107-firmware/` that nothing in this tree has ever read, and the
   method that found yesterday's defect is exactly the method that has not been
   pointed at it.
4. `readIdentity()`'s decode-and-publish path and `publishGspBootResult()` — the
   remaining two of the four items `handoff23.md` §5 item 2 named.
5. `NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED` is still in the enum. The state
   machine has never had real firmware bytes pass through it — and now that the
   suite can OPEN those bytes, feeding a real payload through the DMA fixture is
   a smaller step than it was this morning.
6. Nothing tests that `regRead32()`'s refusal value is *the same constant* the
   consumers compare against. `NVWPR2_READ_REFUSED_VALUE` in `NVFalcon.hpp` and
   `IG_MMIO_INVALID` in `IntelDisplay.hpp` are both `0xFFFFFFFF` by agreement
   rather than by construction. Unchanged from `3050-handoff26.md` §7 item 4.
