# handoff5.md — Phase 5 DMA states landed, tests landed, kext staging landed

**Written 2026-08-09 by `wise-fox-87` (lead).**

**Two corrections applied 2026-08-10, both because the file's own claims were
checked against the artifacts rather than believed:**

- **§7 rewritten.** It said the kext-side staging was "not started at all". It is
  implemented — see §7.
- **§6 and the title corrected.** The title said "tests NOT yet written" and §6
  said "Write the DMA tests" as the next action. **All nine were already
  written and passing.** `tests/build/test_gsp_boot.exe` reported **176 checks, 0
  failures, exit 0** when this was written; it reports **199 checks, 0 failures**
  as of 2026-08-10 (see the DMATRFBASE1 correction below), and `main()` calls
  `testDmaSingleChunk`,
  `testDmaMultiChunk`, `testDmaDrainPolls`, `testDmaWedged`,
  `testDmaImageValidation`, `testDmaImageBeatsSkip`, `testDmaRespectsDryRun`,
  `testFbifStateOrdering`, `testFbifWriteFaultPropagates` and `testDmaTransfer`.
  I filed a crew card to write them before checking, and had to withdraw it.

**Read order on resume: this file, then `handoff4.md` for how Phase 4 was built
and verified, then `PHASE4-GSP-BRINGUP.md` §3.3 for the DMA trace itself.**
`handoff2.md` / `handoff3.md` are about the macOS *guest VM* and its SSH and
credential blockers — none of that was touched, and none of it blocks driver
work.

Every claim below is from a tool result in this session. Where I contradict an
earlier doc, the evidence that overturned it is stated.

---

## 1. Headline — read this before you touch anything

Phase 5's DMA transfer states are **implemented in `NVGspBoot.hpp` and compile
clean**, but they are **NOT covered by a single test**. The suite is green only
because the default `NVGspBootParams` carries `image.valid = false`, so every
existing test still takes the old Phase 4 path and never enters a DMA state.

The build says so out loud:

```
tests\test_gsp_boot.cpp(226): warning C4505: 'fixtureImage':
    unreferenced function with internal linkage has been removed
```

I added the DMA fixture scaffolding (a simulated DMA engine and an image
builder) and then stopped before writing the test functions that use them.
**That warning is the honest marker of where I stopped.** Do not silence it by
deleting `fixtureImage` — delete it only if you deliberately abandon DMA
testing, and say so if you do.

Current verification state: `ctest` **5/5 passed** (4/4 when written; `test_gsp_stage` was added afterwards). That number is real but it
is *not* evidence that the DMA path works. It is evidence that adding the DMA
path did not break the Phase 4 path.

**Update 2026-08-10.** The kext-side staging that §7 described as "not started at
all" is now implemented — `copyGspFirmwareBlob()`, `loadGspFirmware()` and
`releaseGspFirmware()` exist in `NV3050Probe.cpp` and the syntax gate parses them
clean. What landed is the plumbing that would hand the state machine a real
physical address. See §7.

The DMA states themselves **are** covered now — `fixtureImage` is referenced by
seven tests and the suite is at 199 checks, 0 failures (176 when written). What remains uncovered is
the *staging* code in §7: `loadGspFirmware()` lives in `NV3050Probe.cpp`, which no
test binary compiles, so it is verified only by clang parsing it against a stub
IOKit. Nothing has run on hardware.

### A naming collision that will confuse you

"Phase 5" means two different things in this repo and both are load-bearing:

- **In this file and in `NVGspBoot.hpp`**: the firmware-load DMA states inserted
  between `SCRUB_WAIT` and `BROM_PARAMS`. These are a *part of* `PLAN.md`'s
  Phase 4.
- **In `PLAN.md` §3**: "Phase 5 — Memory + command submission (3–12 months)",
  which is a much later milestone and has not been started.

`PLAN.md` was deliberately **not** edited when the staging landed, because
nothing in its Phase 5 changed. Do not read "Phase 5 landed" here as progress
against `PLAN.md` §3.

---

## 2. Firmware blobs exist — this unblocked Phase 5

`handoff4.md` §7 said Phase 5 was blocked on not having a firmware image. **That
is no longer true, and it was already untrue when I wrote it.**
`reference/ga107-firmware/` was checked out on 2026-08-07 and contains:

| File | Size |
|---|---|
| `bootloader-570.144.bin` | 24,684 |
| `booter_load-570.144.bin` | 61,304 |
| `booter_unload-570.144.bin` | 41,080 |
| `vbios-ga107.rom` | 65,024 |

The bootloader's first bytes are `de 10 00 00 01 00 00 00` — magic `0x000010DE`,
version 1, matching `NV_RISCV_BIN_HDR_MAGIC`. `NVRiscvUcode.hpp` already carries
the parsed constants for this exact blob (`NV_RISCV_GA107_BL_*`), so the parsing
half of Phase 5 was done before this session.

**Licensing, from `reference/ga107-firmware/README.md`:** these are
NVIDIA-signed binaries redistributed with the Linux driver. They are *not* ours,
*not* MIT like `open-gpu-kernel-modules`, and **must not be re-published**. They
are reference input only. Do not commit them anywhere public and do not paste
their contents into a document.

---

## 3. What I changed in `NVGspBoot.hpp`

### New states, inserted between `SCRUB_WAIT` and `BROM_PARAMS`

```
FIRMWARE_LOAD   -> decide: valid image? DMA. skipFirmwareLoad? jump. else stop.
DMA_SETUP       -> program DMATRFBASE + DMATRFBASE1 once
DMA_XFER        -> per chunk: MOFFS, then FBOFFS, then CMD (CMD is the trigger)
DMA_DRAIN       -> poll DMATRFCMD until IDLE, then next chunk or BROM_PARAMS
```

### New results

- `ERR_DMA_TIMEOUT` — a chunk never went IDLE within `maxDmaPolls`.
- `ERR_FIRMWARE_INVALID` — size is zero, size is not a whole number of 256-byte
  chunks, or the physical address is not 256-byte aligned. All three are
  **refused, not rounded**: `DMATRFBASE` stores `addr >> 8`, so an unaligned
  address is not even expressible in the register, and a short tail would fail
  the boot ROM's signature check with no explanation.

### New params / state fields

`NVGspBootParams` gained `maxDmaPolls` (default 1000 per chunk) and
`NVGspFirmwareImage image`. `NVGspBoot` gained `dmaPolls`, `dmaPollsTotal`,
`chunkCount`, `chunksDone`, `dmatrfcmd`.

`NVGspFirmwareImage` deliberately carries a **physical address, not a pointer**:

```c
struct NVGspFirmwareImage {
    NVU32 physAddrLo, physAddrHi, sizeBytes, imemOffset;
    bool  valid;
};
```

A state machine that dereferenced host memory could not be driven against a
simulated register file, which is the whole reason this header is testable.

**The default is `valid = false`.** A caller with no blob gets exactly the old
`FIRMWARE_NOT_IMPLEMENTED` stop, unchanged.

---

## 4. The one real discovery this session: `DMATRFCMD = 0x614` has a fifth field

`PHASE4-GSP-BRINGUP.md` §3.3 decodes the observed `0x614` into a table of four
fields: `IMEM=1`, `WRITE=0`, `SIZE=6` (256B), `CTXDMA=0`.

**Those four fields produce `0x610`, not `0x614`.** Compose them and check.

The missing `0x4` is **bit 2, the low bit of `SEC` (3:2)**, so the command the
driver actually issued also carried `SEC = 1`. That field is absent from the
spec's decode table — the table is incomplete, not wrong about what it lists.

`NVGSP_DMATRFCMD_LOAD_IMEM` in `NVGspBoot.hpp` is composed from named
`NVFalcon.hpp` fields **including `SEC`**, so it reproduces `0x614` exactly.

**Do not "fix" this to `0x610`.** Dropping `SEC` issues a non-secure transfer
into a Falcon whose boot ROM is about to signature-check the result. A test
pinning `NVGSP_DMATRFCMD_LOAD_IMEM == 0x614` is listed in §6 below and does not
exist yet — writing it is the cheapest way to stop a future tidy-up from
reverting this.

**`PHASE4-GSP-BRINGUP.md` §3.3's decode table should be amended to add the `SEC`
row.** I did not edit the spec, because it is a record of what the trace showed
and I would rather the next person amend it deliberately than find it silently
rewritten.

---

## 5. One thing I could not verify — SETTLED 2026-08-10 (t-53137f)

**This section's original conclusion was wrong. It is kept below the correction
because the reasoning that produced the wrong answer is the useful part.**

### The correction

`nvGspDmaTrfBase1()` now masks the **shifted** high word:

```c
static inline NVU32 nvGspDmaTrfBase1(NVU32 physHi)
{
    return (physHi >> 8) & (NVU32)NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK;
}
```

BASE1 is a **continuation of the shifted value, not an overlay on the unshifted
one**, so it carries physical bits **48:40** — there is no overlap with the
39:32 that `DMATRFBASE` already holds. Two NVIDIA sources settle it, neither of
which is the trace:

- `reference/.../ampere/ga102/dev_falcon_v4.h:78-79` defines
  `NV_PFALCON_FALCON_DMATRFBASE1_BASE` as `8:0`. That is a field of the
  **register**, and it makes no claim at all about which physical bits land in
  it — reading "40:32" out of it was an inference, not a transcription.
- `reference/.../ampere/kernel_gsp_falcon_ga102.c:122-123` writes
  `DMATRFBASE  = NvU64_LO32(srcPhysAddr >> 8)` and
  `DMATRFBASE1 = NvU64_HI32(srcPhysAddr >> 8) & 0x1FF`. **Both halves come from
  the same shifted quantity.** That is dispositive.

`nvGspDmaTrfBase()` was always correct and is unchanged.

`NV_GSP_FW_ADDR_BITS` was deliberately **left at 41**. Under the corrected
reading the register pair reaches 49 bits, so 41 is no longer a derived hardware
width — it is a **policy ceiling**. It stays because refusing an address the
hardware would have accepted is safe, while admitting one it would truncate is
not. Widening it needs hardware evidence, not an arithmetic argument.
`tools/check-registers.py` was rewritten to check that relationship (ceiling ≤
the pair's reach, ceiling > what BASE reaches alone, literal matches the
recorded policy) instead of deriving 41 from the old premise.

### What the original section said, and why it was reasonable

`NVFalcon.hpp:217` documented `DMATRFBASE1` as bits **40:32** with a 9-bit
field. But `DMATRFBASE` already carries `addr >> 8`, i.e. bits **39:8** — so
bits 39:32 appeared in *both* registers.

**The trace could not settle it.** The observed base was `0xFFEEE200`,
comfortably below 4 GiB, so `DMATRFBASE1` was written `0` and either reading
produces the identical pair of writes. The header's documented field was
followed and the uncertainty was recorded at the function rather than hidden.
That was the right call with the evidence to hand; what was missed is that the
answer was already sitting in the vendored reference driver.

The hazard was real, not theoretical: on a machine with more than 4 GiB of RAM a
high-allocated buffer lands above 4 GiB, and the old form would have written a
BASE1 that overlapped BASE and targeted the wrong page **without faulting**.
`tests/test_gsp_boot.cpp` now pins the corrected values at a high address.

---

## 6. The DMA tests — WRITTEN AND PASSING (corrected 2026-08-10)

**This section originally read "Your next action, concretely: write the DMA
tests". All nine cases below already exist in `tests/test_gsp_boot.cpp` and
pass.** Verified by running the binary, not by reading the file: exit 0, **176
checks, 0 failures**, with sections `DMA: one chunk, full write order`, `DMA:
two chunks`, `DMA: drain polls`, `DMA: wedged transfer`, `DMA: image
validation`, `DMA: a real image outranks skipFirmwareLoad`, `DMA: dry-run gate
still refuses the first write`, plus the FBIF ordering and fault-propagation
tests.

`main()` calls `testDmaSingleChunk`, `testDmaMultiChunk`, `testDmaDrainPolls`,
`testDmaWedged`, `testDmaImageValidation`, `testDmaImageBeatsSkip`,
`testDmaRespectsDryRun`, `testDmaTransfer`, `testFbifStateOrdering` and
`testFbifWriteFaultPropagates` — so they are registered, not merely defined.

I filed a crew card to write these before checking the artifact, and had to
withdraw it. **Run `tests/build/test_gsp_boot.exe` before believing any claim in
this file about what is or is not tested.**

The original text is kept below because the *rationale* for each case is still
the best record of why the case matters — read it as the specification the
existing tests satisfy, not as a to-do list.

### Scaffolding (still accurate)

- `fixtureImage(chunks, imemOffset)` — builds a valid image at
  `OBS_IMAGE_PHYS_LO` (`0xFFEEE200`, the address the trace used).
- The fixture's `read32` **simulates the DMA engine**: after a write to
  `ABS_DMATRFCMD` it serves `dmaDelayPolls` non-idle reads, then sets the `IDLE`
  bit. Without this, `DMA_DRAIN` would poll a stored `0x614` (IDLE bit clear)
  forever and every DMA test would degenerate into a timeout test.
- `dmaNeverIdle` — set it to simulate a wedged transfer.
- Absolute addresses are already defined: `ABS_DMATRFBASE` `0x110110`,
  `ABS_DMATRFMOFFS` `0x110114`, `ABS_DMATRFCMD` `0x110118`, `ABS_DMATRFFBOFFS`
  `0x11011C`, `ABS_DMATRFBASE1` `0x110128`.

The cases that matter, roughly in value order:

1. **`NVGSP_DMATRFCMD_LOAD_IMEM == 0x614`** — one line, and it is the guard on
   §4. Write this first.
2. **`DMATRFBASE == 0x00FFEEE2`** for `physLo = 0xFFEEE200` — proves the `>> 8`.
   `OBS_DMATRFBASE_VALUE` is already defined for this.
3. **Per-chunk write order: MOFFS, FBOFFS, then CMD.** Use `checkJournal()`, the
   same way the existing `MOD_SEL`-last test works. Checking final register
   contents would pass even if CMD went first — and CMD-first is not a fault,
   it silently transfers using the *previous* chunk's offsets
   (`NVFalcon.hpp:191` describes exactly this off-by-one).
4. **A 2-chunk image issues two full MOFFS/FBOFFS/CMD triples**, with MOFFS
   advancing by `0x100` and FBOFFS tracking it — the trace's `+0x100/iter`.
5. **`imemOffset` shifts MOFFS but not FBOFFS.** These are different address
   spaces; the machine adds `imemOffset` only to MOFFS.
6. **`dmaNeverIdle` → `ERR_DMA_TIMEOUT`**, with `dmaPolls` landing exactly on
   `maxDmaPolls`.
7. **`ERR_FIRMWARE_INVALID`** for each of: `sizeBytes = 0`, `sizeBytes = 300`
   (not a chunk multiple), `physAddrLo` unaligned. Assert **zero writes reached
   the fixture** in all three — the machine must refuse before touching the GPU.
8. **A valid image reaches `RUNNING` without `skipFirmwareLoad`** — that is the
   whole point of Phase 5, and it is the test that proves the new states are
   wired into the sequence rather than merely present.
9. **The dry-run gate still holds with a valid image.** `allowWrites = false`
   must still stop at `ERR_WRITES_DISABLED` with the fixture's independent store
   counter at zero. A new write path is a new way to leak.

The existing suite's conventions are worth matching exactly: `section()`,
`checkU32()`, `checkBool()`, `checkResult()`, `checkJournal()`, and
`main()` returning non-zero on failure. Note `FIXTURE_MAX_WRITES` is **32** and
the fixture sets `journalOverflow` when exceeded — a 2-chunk image needs 8 DMA
writes plus 7 others, which fits, but a larger test image will not. Raise the
constant rather than letting the journal silently truncate.

---

## 7. Wiring the blob into the kext — LANDED 2026-08-10

**This section was "not started at all" when it was written. It is now
implemented.** The three bodies declared in `NV3050Probe.hpp` exist in
`NV3050Probe.cpp`:

| Function | What it does |
|---|---|
| `copyGspFirmwareBlob()` | `copyProperty(kNVGspFirmwareProperty)` → `OSData`, caller owns the reference |
| `loadGspFirmware()` | validate → extract bootloader region → pad → allocate → copy → resolve physical address |
| `releaseGspFirmware()` | idempotent teardown, called from every `loadGspFirmware()` failure path plus `stop()` and `free()` |

`attemptGspBoot()` already had the call site (it fetches the blob, calls
`loadGspFirmware()`, and assigns `params.image` on success), so no change was
needed there. With no blob property present the behaviour is **unchanged**:
`params.image.valid` stays false and the machine still stops at
`FIRMWARE_NOT_IMPLEMENTED`. That is the normal case and is not an error.

### How the blob arrives

**Not a file read.** It arrives as an IORegistry property under
`kNVGspFirmwareProperty` (`"NVGspFirmware"`). Two reasons, both recorded at the
`#define` in `NV3050Probe.hpp`:

- A kext reading the filesystem inside `start()` has no ordering guarantee that
  the volume holding the blob is mounted at match time.
- The GA107 GSP binaries are NVIDIA-signed and must not be redistributed inside
  this bundle (§2). Keeping the blob outside the bundle is what makes the kext
  committable.

`copyProperty()` rather than `getProperty()` on purpose: `getProperty()` hands
back a *borrowed* pointer the registry may drop underneath a DMA that is still
in flight.

### The validation chain, in order

1. `nvRiscvUcodeIsWellFormed()` — container header, descriptor length, and every
   region the descriptor names contained in the payload. The blob comes from
   outside the kext; a truncated or hostile one must produce a refusal, not an
   out-of-bounds read in kernel space.
2. `nvRiscvDescField(NV_RISCV_DESC_BOOTLOADER_OFFSET / _SIZE)` for the region.
3. `nvRiscvRegionInPayload()` then `nvRiscvRegionFileOffset()` then
   `nvRiscvFits()`. Region offsets are relative to `dataOffset` — **not** to the
   file start and **not** to the descriptor. Getting that wrong reads the
   descriptor bytes as firmware, which is a plausible-looking image that boots
   nothing.
4. Pad **UP** to a whole number of `NVGSP_DMA_CHUNK_BYTES` (256) and zero-fill
   the tail. Truncating the last partial chunk would load a bootloader missing
   its final instructions, which is worse than loading padding the core never
   executes.

### The two address constants are NOT interchangeable

This is the one thing in this section most likely to be broken by a later edit.
Both encode the same 41-bit window and they are used for opposite purposes:

| Constant | Value | Role |
|---|---|---|
| `NV_GSP_FW_PHYS_MASK` | `0x000001FFFFFFFF00` | allocation **constraint** for `inTaskWithPhysicalMask()` |
| `NV_GSP_FW_PHYS_LIMIT` | `(1<<41)-1` | magnitude **ceiling** to compare a returned address against |

The mask's low 8 bits are deliberately **cleared** — that is what forces the
256-byte alignment `DMATRFBASE`'s bits-39:8 encoding requires. Comparing a
returned address against the mask therefore wrongly rejects any address in the
low 256 bytes of an otherwise-valid region, **and it reads as correct** to a
reviewer who sees "41-bit mask, 41-bit check". I wrote it that way first and had
to correct it. Check alignment with `% NVGSP_DMA_CHUNK_BYTES == 0` and magnitude
against the limit, as two separate tests.

### Why `IODMACommand` and not `getPhysicalSegment()`

`withSpecification()`'s `numAddressBits` argument is where the 41-bit width is
actually enforced against the mapper. `NVFalcon.hpp:217`: `DMATRFBASE` carries
physical bits 39:8 and `DMATRFBASE1` continues the *shifted* value with physical
bits 48:40 (corrected 2026-08-10, section 5), so the pair reaches further than
41 — 41 is the ceiling **this driver allows**, enforced here so that an address
above it is refused up front rather than **truncated into a valid-looking one**
that makes the engine read the wrong page without faulting.

The returned segment is then re-checked against reality rather than against the
request: exactly one segment (the engine takes a single base, not a scatter
list), length covering the padded size, 256-byte aligned, within the limit.
Every failure path calls `releaseGspFirmware()` and leaves `*outImage` invalid,
so a caller that ignores the return value still cannot hand the GPU a half-built
descriptor.

### Teardown ordering

`releaseGspFirmware()` calls `clearMemoryDescriptor()` **before**
`complete()` + release. The `IODMACommand` must let go of the descriptor first,
or the mapping is torn down under an object that still references it. The buffer
must outlive the state-machine run — `DMATRFBASE` hands the GPU a physical
address and the engine reads it asynchronously, so freeing at the end of
`attemptGspBoot()` would race the transfer.

### Shim gap this exposed

`memcpy` / `memset` were **undeclared** — clang 19.1.5 errored at
`NV3050Probe.cpp:1285-1286`. They now live in `tests/kextshim/IOKit/IOLib.h`,
because Apple's real `IOKit/IOLib.h` includes `<string.h>`. Adding a shim
`<string.h>` instead would have diverged the driver's include graph from the one
Apple's toolchain sees: `NV3050Probe.cpp` includes only IOKit headers.

Keep every register decision in `NVGspBoot.hpp`. The kext glue stays what it is:
build the I/O struct, stage the image, spin the machine, sleep between
transitions, report. If you find yourself adding a register offset to
`NV3050Probe.cpp`, it belongs in the header.

### What is still NOT done here

- **The staging code itself has ZERO tests.** `loadGspFirmware()`,
  `copyGspFirmwareBlob()` and `releaseGspFirmware()` live in `NV3050Probe.cpp`,
  which no test binary compiles — they are verified only by clang parsing them
  against a stub IOKit. The §6 DMA tests cover `NVGspBoot.hpp`, not this. To
  test the staging you would have to factor the validation-and-padding
  arithmetic out into a dependency-free header the way `NVRiscvUcode.hpp`
  already is.
- **Nothing has been run on hardware.** This is clang parsing a kext against a
  stub IOKit on Windows. It has never been built with Apple clang.
- **No blob is injected anywhere.** Nothing sets `NVGspFirmware` on the matched
  service, so on a real machine this code path allocates nothing and the machine
  still stops at `FIRMWARE_NOT_IMPLEMENTED`. Deciding *how* the property gets
  set (personality dictionary vs. a userspace tool) is an open question.

### Gates re-run after this landed

| Gate | Result |
|---|---|
| `tests\check-kext-syntax.bat` | 5/5 stages, exit 0 |
| `ctest` via `tests\build-msvc.bat` | 4/4 passed at the time; **5/5, 634 checks** as of 2026-08-11 |
| `tests\run-python-tests.bat` | `EXIT=0` |
| `pytest tests/test_line_endings.py` | 9 passed |

`ctest` reported `ninja: no work to do`. That was **verified genuine by mtime,
not assumed**: all four `.exe` are newer than every source that feeds them. The
Phase 5 change touched only `NV3050Probe.cpp/.hpp`, which no test binary
compiles.

---

## 8. Standing constraints — do not quietly relax these

- **The driver is still read-only, enforced twice.** `fAllowGspWrites` can only
  be true via `make PHASE4_ALLOW_WRITES=1` (a fact about the binary, not a
  runtime switch), and that flag feeds `NVGspBootIo::allowWrites` so the machine
  self-refuses. A stock build stops at `ERR_WRITES_DISABLED` reporting
  `faultingOffset 0x1103C0` with `writeCount 0`.
- **`skipFirmwareLoad` is TEST ONLY.** On hardware it releases a RISC-V core
  whose IMEM was never filled. Now that a real image path exists, this flag has
  no remaining reason to be set outside `tests/`.
- **Nothing has ever run on real hardware.** Every result in this file and in
  `handoff4.md` is a host compiler against a simulated register file. The kext
  has never been built with Apple clang (no macOS toolchain here) and has never
  been loaded.
- **The test fixture spells addresses as literals from the trace on purpose.**
  The machine derives its own via `nvGspFalconReg()` / `nvGspRiscvReg()`. If the
  fixture derived them the same way, a wrong base constant would cancel out and
  the suite would pass while the driver scribbled on the wrong engine. Keep the
  two independent.

---

## 9. Build note

`cmake .` inside `tests/build/` **fails its compiler check** when run outside the
MSVC environment — `cl` is not on `PATH`, and the error surfaces as a confusing
ninja failure during `project()`. Use `tests\build-msvc.bat`, which sets up
vcvars, configures, builds, and runs `ctest` in one go.

Also run `python -m pytest tests/test_line_endings.py -q` after adding files —
it passed (9 tests) after this session's additions, and it catches the bare-LF
hazard that has bitten this project before.

---

## 10. Crew state

All five Phase 4 cards are closed: `t-094198` (header), `t-594099` (suite),
`t-8706c5` (kext wiring), plus the two stale handoff cards `t-d68164` and
`t-a82c63`.

**No card exists for the Phase 5 work described in §6 and §7.** File one.

`witty-lynx-3f` was handed the Phase 4 test suite with the full API, never
answered `q-44b002`, and never produced the file, so I wrote it. **`tests/` has
one suite — do not open a second.**

The rest of the board is stale VM / SSH / credential work blocked on a human
operator supplying the macOS guest account short name and password. None of it
blocks driver progress. I left it alone deliberately, and so should you unless
the user asks.
