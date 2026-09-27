# intel-handoff34

Intel Gen12 (Tiger Lake) track. Continues from `intel-handoff33.md`.

This session added `phase0-kext/IntelGtCmd.hpp`'s test suite, registered it in
every gate that has to know about it, and closed the payload red that had stood
since 2026-08-20. All five gates end green and the readings are in section 4.

---

## 0. What this is, and the sentence it is easy to misread

The operator asked for an **accelerated** driver, retargeted it to the Intel
iGPU, and then said there are no peers and to code it myself. What landed is the
Gen12 render-engine **command submission** layer: engine MMIO bases, the ring
register block, ring geometry verdicts, head/tail wrap arithmetic, MI command
word composition, and the refusals that stand in front of a submission. It is
the first header in this tree that is not about display at all.

**It is not acceleration, and this file will not pretend otherwise.** Metal on
macOS does not talk to a framebuffer driver. It talks to an `IOAccelerator`
family subclass plus a userspace plugin bundle written against private,
unversioned Apple interfaces, and `PLAN.md` section 0 is explicit that this is
the wall the whole project stops at. A ring the driver can describe correctly is
a prerequisite for submitting work, not a path to a composited desktop.

And not one line of `phase0-kext/IntelGtCmd.hpp` has executed on an Intel GPU.
Everything below is arithmetic and composition properties of a pure header,
verified under MSVC on a machine with no Intel GPU in it.

---

## 1. The evidence split, and why the header says so twice

The constants in this header come from two different places and only one of them
is citable. The header's own comment says this, `tests/CMakeLists.txt` says it
again above the registration, and the suite prints it as its third banner line,
because a green run of a suite whose subject is half policy reads as a
verification of the whole thing.

**Cited.** The four engine bases and the ring block offset are in
`reference/i915/i915_reg.h`. `RENDER_RING_BASE` is at line 289; the GEN11 block
carries the blitter, BSD and VEBOX bases. The ring block begins `+0x30` **above**
the engine base rather than at it, and the reason is visible in the vendored
header rather than inferred: `PRB0_BASE` is spelled `(0x2030 - 0x30)`, so the
subtraction in i915's own definition is what fixes the offset.

**Policy.** Every MI opcode is this project's choice. i915 keeps its MI command
family in a subdirectory of headers that were never vendored into
`reference/i915/`, so there is no line to cite. `igGtMiHeader()` composes client
0 in bits 31:29, the opcode in 28:23 and the length in the low byte, and the
suite pins that **composition**. A green run says the words are self-consistent.
It says nothing whatsoever about whether the hardware wants those opcodes.

**No `*_OBSERVED_*` constant may enter this header.** `igpu-results/` holds two
captures from a machine whose render engine was idle — there is no MMIO capture
of a running render engine anywhere in this tree, and inventing one would file a
guess beside values witnessed on silicon, which is the one thing the naming
convention exists to prevent.

---

## 2. The arm-order pins

`igGtSubmitVerdict()` is a refusal chain, and its order is the part no reader can
check by inspection. An input that is bad in exactly one way returns the same
verdict under every possible ordering, so it distinguishes nothing. Each of the
six order claims in `tests/test_intel_gt_cmd.cpp` is therefore driven by an input
that is bad in **two** ways at once.

Three of them are load-bearing rather than tidy:

**Ring geometry is judged before head and tail.** A head is only "outside the
ring" relative to a size, and if the size was never legal the complaint names a
relationship that does not exist. `igGtSubmitVerdict(0u, 0x1800u, 0x9000u, 0u,
0x40u)` has both defects and must report the ring.

**Tail range is judged before tail alignment.** Alignment of a value outside the
buffer is not a meaningful complaint. A tail of `0x1004` in a `0x1000` ring is
unaligned *and* out of range, and out-of-range is the actionable half.

**Length zero is tested before the command-word multiple.** Zero is trivially a
multiple of the command word, so a multiple-first order makes
`IG_GT_SUBMIT_EMPTY` **unreachable** — the dead-arm shape `3050-handoff32.md`
section 3 exists to remove, reintroduced one subsystem over.

Two further properties in the suite are worth knowing before editing the header.
The ring register composers **refuse** off an invalid engine rather than adding
`0x30` to the sentinel, because `0xFFFFFFFF + 0x30` wraps to `0x2F`, a plausible
low register address. And `igGtRingAdvance()` answers an out-of-ring tail with
the ring size rather than a modular result, because a wrapped answer computed
from a bad input is indistinguishable from a good one.

The verdict-name and engine-name loops bound on `IG_GT_SUBMIT_LIMIT` and
`IG_GT_ENGINE_LIMIT` rather than on a count typed into the test, so a family that
grows reddens on the commit that grows it. Both loops also assert the names are
pairwise **distinct**, because a copy-paste arm returning a sibling's literal
satisfies every "has a name" assertion there is.

---

## 3. Four reading traps, three of which cost a round trip

**3a. Bare `ninja` printed a stale binary's answer.** Building the one suite
directly from a shell with no MSVC `INCLUDE` fails on `cstdio` at exit 2 — and
the `120 checks, 0 failures` line printed immediately afterwards came from the
**previous** binary, which was still sitting in `tests/build/`. A no-op build
reads exactly like a passing test. This is `3050-handoff28.md` section 4a
recurring, and the rule it produced is the rule: **run the gate, not the
generator.** Rebuilding through `tests\build-msvc.bat` gave exit 0 and 13/13.

**3b. The payload tool prints the OLD sizes first.**
`tools/build-step7-payload.py` opens with a `currently in ...` listing of what is
already on disk, *then* rebuilds. I quoted that preamble into
`OPERATOR-ACTION-step7-build.md`, i.e. recorded the sizes of the image being
replaced, and `test_the_tar_byte_size_the_card_quotes_is_current` caught it. The
second half is worse than the first: my own prose then claimed the rebuild moved
**neither** byte size, when both moved by exactly two 10240-byte tar blocks. A
number copied from the wrong half of a tool's output does not merely go stale —
it generates a false observation about the change, written in the confident
voice of a measurement.

**3c. A subdirectory name read as a missing repo path.** Writing i915's command
subdirectory as a bare name with a trailing slash makes
`tools/check-documented-paths.py` resolve it against the checkout, find nothing,
and redden two gates. Same trap as the SDK-include note in `CLAUDE.md`; the fix
is the same one — spell it in words. `tests/CMakeLists.txt` and the header both
say "i915 gt/ headers are not vendored here" only where a gate does not read it.

**3d. Double quotes inside a CMake comment reddened a premise test.** A
cross-gate module asserts `_strip_comments`' own premise, and a quoted phrase in
my registration comment broke it. Not a defect in the gate: the assertion is
about what the stripper can be relied on to see, and my prose walked into it.

---

## 4. Gate readings

All five green, measured after the last edit rather than remembered from before
it.

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | 242 of 242 passed, 0 failed, 0 skipped, exit 0 |
| `tests\build-msvc.bat` | exit 0, **13/13** suites (was 12) |
| `tests\check-kext-syntax.bat` | exit 0, `ok IntelGtCmd.hpp`, `standalone IntelGtCmd.hpp`, 19 sources |
| `tests\run-python-tests.bat` | exit 0, **1279 tests**, OK |
| `test_intel_gt_cmd.exe` | **120 checks, 0 failures**, exit 0 |

Two of those numbers are unmoved and both are unmoved **correctly**, which is
worth saying rather than leaving as an absence. The register gate stays at 242
because `tools/check-registers.py` parses only two Intel headers and
`IntelGtCmd.hpp` is not one of them — so `IG_GT_SUBMIT_` is the second Intel
verdict family with no set-check from that gate, and section 2's contiguity and
limit loops in the suite are the only thing holding the relation. The Python
total stays at 1279 because every Python edit this session added a list **entry**
rather than a test method.

**The payload red is CLOSED.** It had stood since 2026-08-20 and every session
since left it alone under `intel-handoff27.md` section 5a's rule — attribute by
mtime, never green a gate by vouching for a peer's half-landed work. The
operator has now said there are no peers. That rule's premise is gone, so the
staleness was this session's to own: `build-payload/` was rebuilt (the **forty
third** image) and `OPERATOR-ACTION-step7-build.md` carries the measured sizes
and md5s. Both the tar and the ISO moved.

### 4a. Registering one header and one suite touched five places

Worth listing, because four of the five are non-obvious and each reddens a
different gate:

1. `tests/CMakeLists.txt` — `add_kext_test`, or the suite never builds.
2. `tests/check-kext-syntax.bat` — `tests/test_check_header_coverage.py` **globs**
   `phase0-kext/*.hpp`, so a new header is auto-discovered and its absence from
   the syntax gate is a failure rather than an omission.
3. `tests/test_check_count_floor.py` — a `MEASURED` entry with a **dated**
   provenance line, since a band around a table cannot audit the table.
4. `tests/test_check_count_last_line.py` — **two** independent hardcoded lists:
   `SUITES`, and the list of suites using the `%d checks, %d failures` format.
5. The count format itself. I wrote it without the leading `\n`; the module's
   `COUNT_FORMATS` pins both real shapes and neither matched.

---

## 5. What this does not establish

1. Not one line of `phase0-kext/IntelGtCmd.hpp` has run on an Intel GPU, or on
   any GPU. The suite drives a pure header under MSVC.
2. The MI opcodes are unverified against hardware and cannot be verified from
   anything in this tree. A self-consistent command word is not a correct one.
3. Nothing writes a ring. There is no submission path, no context, no GTT
   mapping, no fence, no interrupt handling — this header describes a ring, and
   describing is the whole of it.
4. This is not acceleration and does not approach it. See section 0.
5. `IG_GT_SUBMIT_` has no set-check from the register gate, per section 4.

---

## 6. Open, highest value first

1. **`CLAUDE.md`'s read-order list stops at `intel-handoff28.md` and
   `3050-handoff37.md`** while the tree reaches `intel-handoff34.md` and
   `3050-handoff44.md`. Six Intel sessions and seven NVIDIA ones are
   undocumented in the one file every session reads first. The reason it was
   never updated — concurrent agents colliding — no longer holds.
2. **Attribute the +14 in the Python total.** Inherited through
   `intel-handoff31.md` section 9 item 2 to handoff32 and handoff33 unchanged,
   which makes it a claim with an age rather than a fact. Run the
   payload-freshness module alone and count.
3. **No gate covers the embedded Python in `tools/igpu-final-capture.sh`** — 295
   lines inside a heredoc, checked by hand once. Inherited from
   `intel-handoff27.md` section 7 item 2.
4. **`tests/test_intel_scanout.cpp` still has no `MEASURED` entry**, so its check
   count has no floor.
5. An `IOFramebuffer` subclass remains the largest piece of unwritten Intel work
   that is not blocked on hardware, and `phase0-kext/IntelFbPresent.hpp` already
   computes what it would need to publish.
