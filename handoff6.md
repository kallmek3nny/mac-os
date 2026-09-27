# handoff6 — Phase 5 staging wired into the kext

Newest handoff. Read `handoff5.md` for the state this one continues from, and
`CLAUDE.md` for the four verification gates. Nothing here changes the project's
ceiling: Phases 0–5 are still the honest limit, and the kext has still never
been compiled with Apple clang or loaded on hardware.

---

## What changed since handoff5

handoff5 §7 said the `IODMACommand` wiring into the kext was "not started at
all". It is now done, and one thing handoff5 asserted turned out to be wrong.

### 1. `NVGspStage.hpp` — new dependency-free header

The pure arithmetic behind `NV3050Probe::loadGspFirmware()` lives in
`phase0-kext/NVGspStage.hpp`, alongside `NVDecode.hpp` / `NVFalcon.hpp` /
`NVRiscvUcode.hpp` / `NVGspBoot.hpp`. No IOKit, no kernel types, no allocation —
every function is total in its arguments, so `tests/` can compile the identical
file under MSVC.

It owns three things that used to be scattered:

- `NV_GSP_FW_ADDR_BITS` (41), `NV_GSP_FW_PHYS_MASK` (`0x000001FFFFFFFF00`),
  `NV_GSP_FW_PHYS_LIMIT` (`(1<<41)-1`). These moved **out of**
  `NV3050Probe.hpp` so the mask-vs-limit distinction sits next to the code that
  enforces it. The mask is an *allocation constraint* for
  `inTaskWithPhysicalMask()` with its low bits cleared for alignment; the limit
  is a *magnitude ceiling* with every low bit set. Reusing one as the other is
  the bug recorded in memory as `dma-phys-mask-is-not-a-ceiling`.
- `NVGspStageResult` — an enum, not a bool, so a test can assert *which* check
  refused: `NVGSP_STAGE_ERR_NO_BLOB`, `_MALFORMED`, `_ADDR_MISALIGNED`,
  `_ADDR_TOO_HIGH`, and the rest.
- `NVGspStagePlan` — `{ sourceOffset, sourceBytes, paddedBytes }`, plus
  `nvGspStageAddressOk()` and `nvGspStageFillImage()`.

`paddedBytes >= sourceBytes` always. The caller must zero the **whole** buffer,
not just the tail: uninitialised kernel memory handed to a DMA engine is an
information leak as well as a nondeterminism.

### 2. `NV3050Probe` — the IOKit shell

New private members and methods on `NV3050Probe`:

| Member | Why it exists |
|---|---|
| `fGspFwBuffer` (`IOBufferMemoryDescriptor*`) | Physically contiguous, 256-byte-aligned staging for the bootloader blob |
| `fGspFwDma` (`IODMACommand*`) | Constrains the transfer to 41 bits via `withSpecification(..., numAddressBits, ...)` |
| `loadGspFirmware(blob, len, outImage)` | Validate → allocate → copy → resolve physical address |
| `releaseGspFirmware()` | Idempotent; called from **both** `stop()` and `free()` |
| `copyGspFirmwareBlob()` | Pulls the blob from the IORegistry as `OSData`, retained |

Three lifecycle decisions worth not re-litigating:

- **`init()` nulls both pointers**, not `start()`. `free()` can run on an
  instance whose `start()` never got that far, so `releaseGspFirmware()` has to
  be safe on a half-constructed object.
- **`stop()` releases the staging *before* `unmapBar0()`.** The GPU may still be
  mid-DMA out of that buffer. The only thing this driver can do about that
  ordering is `complete()`/`clear()` the descriptor while the register aperture
  is still mapped, so a future halt-the-engine step has somewhere to write.
- **`free()` calls it again anyway.** A `start()` that failed after
  `loadGspFirmware()` never reaches `stop()`, and leaking a wired contiguous
  allocation is worse than a redundant call.

**The blob is a property, not a file.** `kNVGspFirmwareProperty` =
`"NVGspFirmware"`. A kext reading the filesystem at match time has no ordering
guarantee that the volume is mounted, and the GA107 GSP binaries are
NVIDIA-signed and must not be redistributed inside this bundle.
`copyProperty()` rather than `getProperty()` — the latter hands back a borrowed
pointer the registry may drop underneath a DMA that is still in flight.

**Staging failure is not a run failure.** `nvGspBootObservedParams()` leaves
`params.image.valid` false, and with no blob the machine stops at
`FIRMWARE_NOT_IMPLEMENTED` exactly as it did in Phase 4. `attemptGspBoot()`
therefore logs the staging failure and *continues*: the dry rehearsal is still
worth doing, and stopping honestly at `FIRMWARE_NOT_IMPLEMENTED` reports more
than refusing to start.

### 3. `tests/kextshim/` — three new/extended headers

The shim exists so clang's front end can parse the kext on a machine with no
macOS SDK. It is **not Apple's header** and catches only structural errors:
typos in member names, wrong argument counts, missing declarations, bad
overrides. It cannot catch wrong Apple semantics or real KPI signature drift.

- `IOKit/IOBufferMemoryDescriptor.h` — **new**. Only
  `inTaskWithPhysicalMask()`, `getBytesNoCopy()`, `getCapacity()`,
  `prepare()`/`complete()`. A wider stub would let a wrong argument list through
  the one gate that can see it.
- `IOKit/IODMACommand.h` — **new**. `withSpecification()` declares
  `numAddressBits` explicitly rather than defaulting it, because that argument
  *is* the 41-bit constraint.
- `IOKit/IOTypes.h` — added `task_t`, `extern kernel_task`,
  `mach_vm_address_t`, `mach_vm_size_t`, and `IODirection`. `IODirection` is a
  distinct typedef rather than an alias of `IOOptionBits` so that passing a map
  option where a direction belongs is still a diagnosable mistake.
- `libkern/c++/OSObject.h` — added `OSData`. `getBytesNoCopy()` is
  **const-returning on purpose**: the blob is signed firmware this driver copies
  into its own staging buffer and must never edit in place.
- `IOKit/IOService.h` — added both `getProperty()` and `copyProperty()`.
  Declaring both makes the call site say which lifetime it meant.

### 4. `tools/check-registers.py` — coverage gap closed

It now parses `NVGspBoot.hpp` as well (third banner line). Three parser changes
were needed to get there:

- `strip_int_suffixes()` — `u` / `ULL` suffixes were breaking `resolve()`.
- Composed bit expressions — strips `(NVU32)` casts, recursively substitutes
  symbols, evaluates `| & ^ << >> ~`.
- `join_continuations()` — folds backslash-continued `#define`s before parsing.
  `NVGSP_BCR_CTRL_BRFETCH_SHIFT` is one of these.

Plus `gspboot_sanity_checks()`, seven assertions over the merged table.

**Result: 109/109 passed** (was 102/102).

---

## Gate status as of this handoff

```
python tools/check-registers.py     109/109 passed, 0 failed, 0 skipped
tests\check-kext-syntax.bat         all sections ok, incl. NV3050Probe.cpp
                                    structors 1/1, negative control ok
tests\build-msvc.bat                NOT re-run since NVGspStage.hpp landed
tests\run-python-tests.bat          NOT re-run since check-registers.py changed
```

The kext syntax gate now scans **7 sources** — `NVGspStage.hpp` is picked up
automatically.

---

## The one open gap

**`NVGspStage.hpp` has no test suite.** It is the only dependency-free header in
`phase0-kext/` without one, which defeats the entire reason the header was split
out. `tests/CMakeLists.txt` has no `add_kext_test(test_gsp_stage ...)` line.

The suite to write, matching the `test_riscv_ucode.cpp` idiom exactly
(`gChecks`/`gFailures` counters, `section()`, `checkU32()`, one static blob
buffer rebuilt per test, **no file I/O**):

1. **Observed geometry** — a synthesised GA107 bootloader blob produces the
   `sourceOffset` / `sourceBytes` / `paddedBytes` the trace implies.
2. **Padding** — `paddedBytes` is `sourceBytes` rounded up to
   `NVGSP_DMA_CHUNK_BYTES` (256), and equals `sourceBytes` exactly when already
   aligned. An off-by-one-chunk bug is invisible to a "was it rejected" test.
3. **Every refusal reached by its own path** — assert the specific enum, not
   just non-OK. `NO_BLOB` (null, zero length), `MALFORMED` (fails
   `nvRiscvUcodeIsWellFormed`, truncated *declared* length over a full-size
   allocation), `ADDR_MISALIGNED`, `ADDR_TOO_HIGH`.
4. **The 41-bit boundary, both sides** — `NV_GSP_FW_PHYS_LIMIT` itself must be
   rejected or accepted deliberately, and `LIMIT+1` must refuse. This is the
   check that catches a mask reused as a ceiling.
5. **`nvGspStageFillImage()` split** — `physAddrLo` masks to 32 bits,
   `physAddrHi` is `physBase >> 32`. A `physAddrHi` that dropped the shift looks
   right for every address below 4 GiB and silently targets page zero above it.

Then add to `tests/CMakeLists.txt` after the `test_gsp_boot` line:

```cmake
# Phase 5 -- the staging arithmetic behind loadGspFirmware().
add_kext_test(test_gsp_stage test_gsp_stage.cpp)
```

and re-run `tests\build-msvc.bat`.

---

## Things that will bite the next agent

- **`ninja` from Bash fails with `cstdio` not found.** The MSVC environment is
  not initialised in that shell. Run `./tests/build-msvc.bat` instead — it
  resets PATH and calls `vcvars64`. Invoke `.bat` files directly from Bash;
  `cmd //c` mangles the invocation and you get a banner with an empty prompt.
- **Grep renders some comment lines as `\ ` instead of `// `.** Seen on
  `NVRiscvUcode.hpp:175` and `NVGspStage.hpp:39`. The files are fine — a `Read`
  of the same lines shows `// ` correctly. Do not "fix" a comment based on grep
  output.
- **`crew next` keeps handing out `t-ed1a53`** (operator credential-gated). It
  has no recorded `blockedBy`, so the board thinks it is ready. It is not. Drop
  it and take the next card.
- **Write count in the DMA journal is `11 + 3n`** for an n-chunk image, not a
  flat number. n=1 → 14 writes, indices 0..13. `FIXTURE_MAX_WRITES` is 32, so
  n=7 fits exactly and n≥8 overflows the fixture.
- **`FBIF` is already implemented**, contrary to a reading of handoff5.
  `NVGSP_STATE_FBIF_CFG` (NVGspBoot.hpp:563) writes `TRANSCFG(0)` ← `0x115`
  then `FBIF_CTL` ← `0x190`, and `FIRMWARE_LOAD` routes to it at :538.
- **`handoff5.md` understated test coverage.** It said the Phase 5 DMA tests
  were unwritten and `fixtureImage` unused; all nine existed and passed. Run the
  binary before filing work from a doc.

---

## Still true, still the ceiling

Every gate here is a host compiler against a simulated register file or a
syntax-only parse. **STEP 7 in `STEPS.md` is still required.** A green run means
"structurally plausible", not "builds on macOS" and certainly not "loads".

No write has ever reached this GPU. Both gates still stand: `fAllowGspWrites`
is false without `-DPHASE4_ALLOW_WRITES`, and the machine refuses its own first
write with `ERR_WRITES_DISABLED`.
