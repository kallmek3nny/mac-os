# handoff9 — three silent-truncation bugs on the Phase 5 address path, and a
# parser that could not read its own composed constants

Newest handoff. Read `handoff8.md` for the state this continues from, and
`CLAUDE.md` for the four verification gates. **Nothing here changes the
project's ceiling.** Phases 0–5 are still the honest limit, the kext has still
never been compiled with Apple clang, and it has still never been loaded on
hardware.

Everything below was run in this session. All four gates were re-run after each
change and the state at the end of the session is recorded in §5 with the
literal output each gate printed.

This session ran as a crew of four terminals. Peer work is recorded in §4 and is
attributed; §1–§3 are the lead's changes.

---

## 1. `nvGspStageAddressOk()` bounded the BASE and nothing bounded the END

`phase0-kext/NVGspStage.hpp` checked a physical base two ways — 256-byte
alignment, and `physBase <= NV_GSP_FW_PHYS_LIMIT` (the 41-bit policy ceiling).
Both checks take **one address and no length**.

The DMA engine is handed a base in `DMATRFBASE`/`DMATRFBASE1` and the transfer is
driven by a **length**. So a chunk-aligned base a few hundred bytes below the
ceiling passed every existing check while the *tail* of the staged image
addressed outside the window. Nothing in the suite could see it, because every
assertion in the file took an address and no length.

Fix: `nvGspStageRangeOk(physBase, lengthBytes)`, plus a new
`NVGSP_STAGE_ERR_RANGE_TOO_HIGH`. It calls `nvGspStageAddressOk()` first and
**forwards its codes unchanged**, so a misalignment is still reported as
`ERR_ADDR_MISALIGNED` rather than collapsed into a range fault — the caller logs
that name and a reader chasing the wrong constraint is a real cost.
`NV3050Probe::loadGspFirmware()` now calls the range form with `padded`.

### The comparison is a subtraction on the ceiling, not an addition on the base

```c
if (lengthBytes > (NV_GSP_FW_PHYS_LIMIT + 1ull) - physBase)
```

not `physBase + lengthBytes <= LIMIT`. The addition **wraps 64 bits**: a length
near 2^64 produces a small sum that passes the naive test. `physBase` is already
known `<= LIMIT` from the call above, so the subtraction cannot underflow.

`tests/test_gsp_stage.cpp` section **3b** pins the four separating cases — the
flush base whose last byte is the last addressable byte, one chunk past it with
the same length, one byte too long from a flush base, and the wrapping length.
The low-base cases in that section are **negative controls only**: they pass
under both the correct reading and one that ignores `lengthBytes` entirely, so
they prove nothing on their own and say so in a comment.

Zero length is **accepted** deliberately. `nvGspStagePlan()` refuses an empty
bootloader before any address exists; refusing it again here would report a
length fault as a range fault.

142 -> 162 checks.

---

## 2. `NVGSP_STATE_FIRMWARE_LOAD` never bounded `physAddrHi`

Same class of bug, one header over. The state validated `sizeBytes` and
`physAddrLo` alignment. `physAddrHi` is 32 bits wide in `NVGspFirmwareImage` and
the register pair carries **17 of them**:

| Function | Contribution | Bits of `physHi` |
|---|---|---|
| `nvGspDmaTrfBase()` | `physHi << 24` into a 32-bit register | low 8 |
| `nvGspDmaTrfBase1()` | `(physHi >> 8) & DMATRFBASE1_BASE_MASK` | next 9 |

8 + 9 = 17, i.e. physical bits 48:32. Above that the packing **truncates rather
than faults**, and the engine is handed a well-formed address pointing at the
wrong page. This is the same invisible-below-4-GiB shape as the `DMATRFBASE1`
overlay bug in handoff8 §7, and for the same reason: every value the MMIO trace
witnessed had `physAddrHi == 0`.

Added `NVGSP_DMA_PHYSHI_MASK` and `nvGspDmaPhysHiFits()` to `NVGspBoot.hpp`, and
a refusal in `FIRMWARE_LOAD` returning `ERR_FIRMWARE_INVALID`.

### Two things worth reusing

**The 17 is derived, never written.** `NVGSP_DMA_PHYSHI_MASK` composes from
`NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK`, so correcting the field definition
cannot leave a stale width behind. A literal repeating a field width goes stale
silently at exactly the moment the definition it mirrors is edited.

**A refusal-only test passes for a mask of zero.** The bad-case table added to
`test_gsp_boot.cpp` — `0x20000`, top-bit-only, all-ones — would all pass for a
machine that rejected *every* nonzero `physAddrHi`. So `0x1FFFF`, the last legal
value, is asserted **accepted** on the far side, and the predicate is driven
directly with both boundary values. Widening the mask one bit breaks the
refusal; narrowing it one bit breaks the acceptance. Neither assertion alone can
tell a correct mask from a wrong one.

208 -> 217 checks.

### The bound in `NVGspBoot.hpp` is not the bound in `NVGspStage.hpp`

Two different quantities, deliberately independent:

- `NVGspStage.hpp`'s `NV_GSP_FW_ADDR_BITS` (41) is a **policy ceiling** — what
  this driver allows.
- `NVGSP_DMA_PHYSHI_MASK` is a **representability bound** — what the registers
  can express (49 bits total).

An address failing only the stage check is a policy refusal. One failing the
boot check would be silently truncated. The headers cannot reference each other
(`NVGspStage.hpp` includes `NVGspBoot.hpp`, not the reverse), which is why the
bound in §2 is derived locally from the field mask.

`stage_sanity_checks()` in `tools/check-registers.py` already asserted policy vs
hardware reach, so that relationship is **not** unchecked. What is still missing
is narrower and is filed as an open card: nothing asserts that
`NV_GSP_FW_PHYS_LIMIT`'s high word satisfies `nvGspDmaPhysHiFits()` — i.e. that
the policy ceiling sits inside the mask the *state machine* refuses on, not just
inside the raw field width.

---

## 3. `check-registers.py` could not parse ANY composed constant containing a
## hex literal

Adding a check for `NVGSP_DMA_PHYSHI_MASK` to `gspboot_sanity_checks()` failed
with `not found or unparseable in NVGspBoot.hpp`. The header was fine.

`resolve()` substituted symbols with:

```python
re.sub(r"[A-Za-z_][A-Za-z0-9_]*", substitute, stripped)
```

In `0xFF` the leading `0` is outside the character class, so the scan resumes at
`x` and **`xFF` matches as a symbol name**. It resolved to `None`, `"None"`
landed in the expression, and the whole body was reported unparseable — with
wording that names the *header*, so the natural next move is to go read the
header, which is the wrong place.

Invisible until now because every composed constant in the kext wrote its
literal operands in **decimal** (`(NVU32)1u << SHIFT`, `(1ULL << BITS) - 1ULL`).
`NVGSP_DMA_PHYSHI_MASK` is the first to write one in hex.

Fixed with a `(?<![0-9A-Za-z_])` lookbehind. That is correct rather than merely
narrower: a real C identifier can never be preceded by one of those characters,
or it would be part of the same token.

Six regression tests in `tests/test_tools_parsing.py`, including the far side —
a fix that over-rejected would silently **drop** unknown symbols instead of
returning `None`, turning a missing constant into a plausible number, which is
the worst possible outcome for a gate whose job is comparing values.

### A trap for anyone testing `resolve()`

`resolve()` has a **pure-literal fast path** guarded by a
`[\dxXa-fA-F\s()+\-*]+` fullmatch. Any test expression whose symbol name uses
only hex-digit letters — `B`, `C`, `ABC` — matches that fullmatch, is `eval`'d
as text, raises, and returns `None` **without ever reaching the substituter**.
The first version of the far-side test used `(8 + B)` and failed for a reason
unrelated to the defect. Name test symbols with at least one character outside
that class; the test now uses `SYM_X` and says why.

Register gate 113/113 -> 114/114. Python gate 415 -> 420.

---

## 4. Peer work this session

Attributed, and each peer verified their own gates:

- **`swift-seal-21`** extended the separating-input sweep to `test_decode.cpp`
  (39 -> 64) and `test_riscv_ucode.cpp` (85 -> 116). Two findings: an
  adjacent-bit check must assert on **bit N+1**, the side a widened mask
  reaches — `nvGfwBootProgress`'s "upper bits ignored" check used `0xDEADBEFF`,
  which has bit 8 *clear*, so a mask widened to 8:0 survived it (`0xDEADBFFF`
  separates them). And **self-cancelling constants**: `buildBlob()` wrote each
  header field *through the same `#define`* the accessor read it back with, so
  mutating the define moved fixture and reader in lockstep and every assertion
  still passed. All 6 `nvfw_bin_hdr` offsets and 9 descriptor offsets were
  undefended; fixed by comparing each define to a bare literal.
- **`wise-otter-74`** added `tests/test_documented_paths.py` (18 tests): every
  path-shaped token in `CLAUDE.md` must resolve, every backticked directory in
  the layout table must be a real dir, every script named in a fenced `sh` block
  must exist. Handoff files are deliberately out of scope — they are a dated
  record naming scratch probes that were correct when written, so gating them
  would force editing history to stay green. Two **documented absences** are
  asserted rather than ignored: `tests/__init__.py` must not exist (the
  `unittest discover` recipe in `CLAUDE.md` is only correct while `tests/` is not
  a package), and `site-packages/tests/__init__.py` is declared out-of-tree.
  Failure path proven with 9 mutants, all killed.
- **`witty-koala-a5`** is on the check-count floor gate: a C++ suite that runs
  zero checks prints `0/0 passed, 0 failed` and reads as green.

### Two environment gotchas worth keeping

**Bare `ninja` from Bash fails.** Running `ninja` directly in `tests/build`
reports `fatal error C1034: cstdio: no include path set` — MSVC's `INCLUDE`/`LIB`
are never in the Bash environment. That is an environment fault and it looks
exactly like a broken source tree. Relink via `tests/build-msvc.bat`, which
calls vcvars first. `CLAUDE.md`'s single-suite recipe only works from a vcvars
shell.

**`ninja: no work to do` means the ctest greens may describe stale binaries.**
The first gate run this session printed it; the counts it re-ran were already
current, which was fine, but it is not evidence of anything the change did.
Force the relink and read the printed counts.

---

## 5. Gate state at the end of the session

All four run from the project root. Each was re-run after the last change and
these are the literal verdicts:

| Gate | Result |
|---|---|
| `python tools/check-registers.py` | exit 0, **114/114 passed, 0 failed, 0 skipped** |
| `tests\build-msvc.bat` | exit 0, ctest **5/5 Passed**, 100% of 5 |
| `tests\check-kext-syntax.bat` | exit 0, all five headers standalone, sections 5–7 negative controls firing |
| `tests\run-python-tests.bat` | exit 0, **Ran 420 tests, OK** |

Suite counts, read off the binaries in `tests/build/` after a real relink
(ninja printed `Linking CXX executable test_gsp_boot.exe`, not "no work to do"):

| Suite | Checks |
|---|---|
| `test_decode` | 64 |
| `test_falcon` | 169 |
| `test_riscv_ucode` | 116 |
| `test_gsp_boot` | 217 |
| `test_gsp_stage` | 162 |
| **total** | **728** |

`tests/README.md` carries that total and its per-suite table matches. Both went
stale **twice within one afternoon** — three of the five suites moved, two of
them more than once, and a README revision six minutes old was already wrong.
The binaries are the only authority. If you find the total wrong, run them.

---

## 6. What is still open

- Assert that `NV_GSP_FW_PHYS_LIMIT`'s high word satisfies
  `nvGspDmaPhysHiFits()` — the policy ceiling inside the mask the state machine
  enforces (§2). Belongs in `test_gsp_stage.cpp`, which sees both headers.
  Derive from the constants; no literals for 41, 17 or 49.
- The check-count floor gate (`witty-koala-a5`). It must assert `count >= floor`,
  never `==`, for the reason in §5.
- Extending the prose-path gate beyond `CLAUDE.md` — `tests/README.md` alone
  names 21 path tokens nothing checks.
- Everything in the macOS *guest VM* track remains blocked on the operator
  supplying the guest account short name and password. Unchanged from handoff2/3
  and it does not block driver work.

**STEP 7 is still required.** Nothing in this session compiled the kext with
Apple clang or loaded it on hardware, and no change here brings that closer.
