# 3050-handoff27 — the descriptor nobody typed

NVIDIA track, 2026-08-18, continuing from `3050-handoff26.md`. This session did
not come from the open list. The operator asked how to go faster, confirmed a
fact that changes the project's shape, and picked the one item on my speed-up
list that runs on Windows today.

It found a **live defect in shipping code**, and — unlike the last five
handoffs — it was found by parsing the REAL firmware rather than by reading
source. That is the part worth carrying forward.

---

## §1 — the operator's fact, which outranks everything below it

**The laptop is muxless.** Verbatim: *"all graphics have to go thru the intel
chip first"*, with an HDMI cord as the only bypass and not one they want to
depend on.

`PLAN.md` termination condition A is therefore **SATISFIED, not open**. Every
percentage estimate in this project has been hedging on it. Combined with
condition B — already measured, the Sequoia recovery image holds 430 kext paths
and NO Tiger Lake framebuffer, Ice Lake being the newest display engine macOS
knows — the consequence is:

> A complete, perfect NVIDIA framebuffer driver still puts nothing on the
> internal panel. The 3050 is a render-offload device whose output must be
> copied into the Intel display engine's scanout buffer.

The display path on THIS track is not unfinished. It is structurally dead for
the internal panel. Work on `NV3050Probe.cpp` and the GSP boot path is worth
doing as Phase 4/5 engineering — inspection, firmware bring-up, eventually
compute — and is **not on the critical path to a picture**. Route display effort
to the Intel track.

A second operator constraint, and it is a scheduling one rather than a
technical one: **the laptop cannot hold Linux AND macOS at once.** So a Linux
boot for more i915 traces and a macOS boot for a `kextload` are MUTUALLY
EXCLUSIVE on that machine. §7 item 1 has been the top item for six handoffs on
the assumption that it was merely unscheduled; it is now known to be in direct
competition with the Intel track's own evidence needs. That is a decision for
the operator, not a task.

Saved as the memory `the-3050-is-muxless-on-this-laptop`.

---

## §2 — the defect

`nvRiscvUcodeIsWellFormed()` in `phase0-kext/NVRiscvUcode.hpp` read the region
between `hdrOffset` and `dataOffset` as an `RM_RISCV_UCODE_DESC` **in every
blob**. Measured 2026-08-18 against all three files in
`reference/ga107-firmware/`, only `bootloader-570.144.bin` actually carries one.

| File | Region | First word | What it is |
|---|---|---|---|
| `bootloader-570.144.bin` | 84 bytes / 21 fields | `0x05` | `RM_RISCV_UCODE_DESC`, version 5 |
| `booter_load-570.144.bin` | 864 bytes / 216 fields | `0x3C` | `nvfw_hs_header_v2` |
| `booter_unload-570.144.bin` | 864 bytes / 216 fields | `0x3C` | `nvfw_hs_header_v2` |

Same container magic `0x10DE`, same `hdrOffset` of 24, same accessor path. The
booters' first word is 60 — that header's own `header_size` field, **not a
structure version at all**. `reference/ga107-firmware/README.md:67` names the
type outright; nothing in the kext consumed that knowledge.

**§2a — why "it was rejected anyway" is not a defence.** A booter WAS still
refused before this change. It was refused three optional-region rows deep,
because the signature bytes at `+0x24` happened to decode to a size of
`0x1F3B3C30` which does not fit a 60416-byte payload. Measured, not assumed —
the first word above `0x0FFFFFFF` sits at index 9, i.e. the plausible-looking
prefix ends at **byte 36**, exactly `header_size`. Everything past that is
signature material being read as struct fields.

That rejection is an accident of one signature's contents. Nothing constrains a
future signature to hold an out-of-range word at that offset, and the failure
being guarded is not a parse error — it is a **DMA of an HS signature block into
IMEM as though it were bootloader code**, on a core with no MMU. Same family as
the project's other silent-address bugs: the wrong bytes at a valid address.

**§2b — why no gate could see it.** The four by-construction blindnesses, with a
fifth that is new and is the interesting one:

1. `tools/check-registers.py` does not parse `NVRiscvUcode.hpp` at all — its
   header list is `NVDecode`, `NVFalcon`, `NVGspBoot`, `NVGspStage` plus the two
   Intel ones. Confirmed by its own printed list; the gate reads **240/240** both
   before and after this session, correctly.
2. `-Werror` has nothing to say: reading a `u32` at offset 0 of a buffer is
   well-typed whatever the buffer holds.
3. The Python gate models file shapes, not blob contents.
4. `tests/check-kext-syntax.bat` is a parse.
5. **The C++ suite covers this file heavily and could not see it either.** 134
   checks over `NVRiscvUcode.hpp`, and every one of them drove a blob
   synthesised by `buildBlob()` — which builds the BOOTLOADER layout. A suite
   that only ever constructs the shape it expects cannot discover that a second
   shape exists. handoff18 §1c generalised the optional-region rows to a loop
   for exactly this reason and still never left the bootloader's geometry.

That fifth one is the durable lesson and it is new: **the fixture was the
blindness.** `test_riscv_ucode.cpp:6` says so in its own header comment —
"deliberately NO FILE I/O: every blob here is synthesised in memory from the
values parsed out of the real firmware" — which is a correct decision for
portability and a complete blind spot for blob-type discovery. The real files
have been sitting in `reference/ga107-firmware/` since 2026-08-07 and nothing in
the tree had parsed all three of them side by side.

**§2c — the fix.** Two additions to `NVRiscvUcode.hpp`:

```cpp
#define NV_RISCV_DESC_VERSION_UNKNOWN            0xFFFFFFFFu

static inline NVU32 nvRiscvDescVersion(const unsigned char *buf)
{
    return nvRiscvDescField(buf, NV_RISCV_DESC_VERSION_OFFSET,
                            NV_RISCV_DESC_VERSION_UNKNOWN);
}
```

plus an equality in the predicate, placed AFTER the field-count test. Four
choices in it are load-bearing:

- **Read through `nvRiscvDescField()`, not at offset 0 directly.** A zero-length
  descriptor is a shape the container header can legally describe, and a direct
  read at offset 0 would then return the PAYLOAD's first word as a version. The
  file already has a presence-checked accessor; using it is free.
- **The fallback is `0xFFFFFFFF`, deliberately not 5.** A blob too short to hold
  its own version is not "probably the version we know" — it is a blob whose type
  we failed to establish, and the value that flows onward must be one no consumer
  accepts. Same refuse-rather-than-trust direction as handoff22's zeroing of
  `fBar0Length` on unmap.
- **An EQUALITY, never a floor.** A version 6 descriptor would append fields the
  way 5 appended them and would very likely parse. "Very likely" is the wrong
  standard for a buffer that becomes instructions. Whoever meets a version 6 blob
  extends the test deliberately — the tripwire is the point, same shape as
  handoff21 §1's sentinel-past-the-family.
- **It runs AFTER the length test**, so a short descriptor is refused for being
  short rather than for its version word. Both refuse; only one of them is a
  claim somebody can act on. Same precedence reasoning as handoff22 §1b's
  ordering of the policy and mapping guards.

---

## §3 — the observer, and the separating input

`tests/test_riscv_ucode.cpp` gained `testDescriptorVersion()`, **134 → 144**
checks.

The separating input is the one this section exists for: a blob whose container
header is valid, whose descriptor is 84 bytes, and whose optional rows are ALL
ZERO — so **every pre-existing check in the file passes it** — differing from the
real bootloader in the version word alone. Before the gate,
`nvRiscvUcodeIsWellFormed()` returned `true` on exactly that shape.

Also pinned: `60` reads back as the version (the HS header's `header_size`,
named as such); a HIGHER version is rejected rather than accepted as a superset;
a lower one too; zero is not a free pass; a zero-length descriptor reports
UNKNOWN rather than 5; UNKNOWN is asserted DISTINCT from 5 — the mistake
handoff21 §1b records is comparing against a value the subject can never
produce, and a sentinel that silently equalled the accepted version would make
six checks vacuous at once.

**Non-vacuity.** Gutting the guard to `if (false)` reddened exactly **FOUR**
checks, all four in the new section, **every pre-existing check green**. That is
the signal handoff18 §1e argues for, and it is the cleanest reading the tree has
produced — 4 failures, 0 pre-existing, first try. Contrast handoff18 §1e's own
13 → 30 → 2 hunt.

144 sits INSIDE this entry's band (134 + max(8, 13) = 147) by three, so the
ceiling did NOT fire and the dated provenance paragraph in
`tests/test_check_count_floor.py` is the only thing recording the growth. Fourth
reading in five where a real growth landed just under the edge.

---

## §4 — a build failure that was not mine

Bare `ninja` in `tests/build` failed on `test_gsp_stage.cpp` with `cstdio: No
such file or directory`. `cstdio` is the first include in that file and I had
not touched it; the failure is that this shell has no MSVC `INCLUDE` set.
`tests/build-msvc.bat` does its own vcvars setup and built both objects
immediately. **Run the gate, not the generator** — a `ninja` invocation from a
bare shell measures the environment rather than the code, and its error text
names a file you did not edit, which reads convincingly as a real break.

---

## §5 — gate state, all four run after the last edit

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240/240** passed, 0 failed, 0 skipped |
| `tests/build-msvc.bat` | 9 suites, all passed; `test_riscv_ucode` **144/144** |
| `tests/check-kext-syntax.bat` | exit **0** |
| `tests/run-python-tests.bat` | **952** tests, OK, exit 0 |

The register gate is unmoved at 240 and that is correct — no `#define` it parses
was added. `NV_RISCV_DESC_VERSION_UNKNOWN` lives in a header outside its list.

The syntax gate still prints `FAIL` on a passing run: line 198 is the *title*
`=== 11. negative control -- each defect must FAIL ===`. Read the exit code.
Verbatim handoff24 §3 and 3050-handoff26 §4.

Python 937 → **952**. The +15 is the peer's; my own Python change was the
provenance paragraph and the `MEASURED` value, neither of which adds a test.

---

## §6 — payload and card

Shipping code changed, so `build-payload/` was rebuilt and
`OPERATOR-ACTION-step7-build.md` updated. The staleness was **my own** edit to
`NVRiscvUcode.hpp` — which is what makes rebuilding correct here and made it
incorrect in handoff23 §3.

Twenty ninth image. Current: ISO md5 `fda15fc83b16d70cbb8a11d9220e5833`, 436224
bytes; tar md5 `5de35bb7e42133fe8b6b228a27376dd7`, 358400 bytes.

**Both sizes moved** — tar 348160 → 358400 and ISO 425984 → 436224, exactly one
10240-byte tar block each — ending the run of three consecutive images that held
both fixed while all six md5s differed. Nothing to read into that: the twenty
ninth's diff simply exceeded the slack the twenty sixth's new block left. The
size is still not the assertion.

---

## §7 — what this does NOT establish

1. Nothing here ran on hardware. `nvRiscvDescVersion()` has been executed by
   MSVC on Windows against synthesised blobs; no firmware image has ever been
   DMA'd anywhere.
2. **The suite still does no file I/O.** The version gate is driven by a
   synthesised blob carrying a version word I typed, not by
   `booter_load-570.144.bin` itself. The real-file parse that FOUND this ran in
   a scratch script and is gone. A blob-type regression would be caught; a
   change to the real files on disk would not. §8 item 2.
3. The equality means this project now refuses every RISC-V ucode blob except
   GA107 570.144's bootloader. That is intended and it is also narrow — a
   different driver release could ship version 5 with a longer descriptor and
   pass, or ship version 6 and be refused for a reason the log does not yet
   explain, because the predicate returns a bare `bool`. See item 4.
4. **`nvRiscvUcodeIsWellFormed()` is a bare `bool` with now six ways to say
   no** — bad header, short descriptor, wrong version, and three region-overrun
   families. That is `3050-handoff26.md` §1a's argument verbatim, and I did not
   fix it here because the function has no publication path: nothing calls it
   from `NV3050Probe.cpp` yet. When something does, it needs a verdict family
   before it needs anything else.

---

## §8 — open, most valuable first

1. **The operator's scheduling decision, which now gates item 2.** The laptop
   holds Linux or macOS, not both. One `kextload` needs macOS on a machine
   containing the card; the Intel track needs more i915 traces from Linux. §1
   establishes that only the Intel track can produce a picture, so the traces
   are probably worth more — but that is the operator's call and it should be
   made explicitly rather than by whoever boots first.
2. **One `kextload` on a machine containing the card.** Unchanged from
   handoff22 §4 through `3050-handoff26.md` §7. Still the only thing that turns
   six handoffs of source claims into measurements. Now known to be in
   competition with item 1.
3. **Drive the suite from the real files.** A test that opens
   `reference/ga107-firmware/*.bin` when present and skips when absent would have
   found this defect on 2026-08-07. The portability argument in
   `test_riscv_ucode.cpp:6` is sound for the SYNTHETIC checks and does not
   preclude an additional file-driven section that skips cleanly. This is the
   highest-value item not blocked on hardware and it generalises: the same
   question should be asked of `vbios-ga107.rom`.
4. `readIdentity()`'s decode-and-publish path and `publishGspBootResult()` —
   the remaining two of the four items `handoff23.md` §5 item 2 named.
5. `NVGSP_RESULT_FIRMWARE_NOT_IMPLEMENTED` is still in the enum. The state
   machine has never had real firmware bytes pass through it, and item 3 is the
   cheap first half of fixing that.
6. Nothing in the tree tests that `regRead32()`'s refusal value is *the same
   constant* the consumers compare against. `NVWPR2_READ_REFUSED_VALUE` in
   `NVFalcon.hpp` and `IG_MMIO_INVALID` in `IntelDisplay.hpp` are both
   `0xFFFFFFFF` by agreement rather than by construction. Unchanged from
   `3050-handoff26.md` §7 item 4.
