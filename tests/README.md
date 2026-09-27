# Host-side tests

The kext cannot be **built** on Windows — that needs Apple's clang with
`-mkernel -fapple-kext` and the Kernel framework headers. But two things can be
done here: the *decode logic* can be compiled **and run**, and the IOKit shell
can be **parsed**.

```
tests\build-msvc.bat          # five C++ suites, MSVC + ctest
tests\check-kext-syntax.bat   # clang front end over the kext, via kextshim/
tests\run-python-tests.bat    # tool and doc tests (CPython only, no MSVC needed)
```

The third gate is the only one that runs on a bare checkout: it needs nothing
but CPython. It covers `tools/dmg2raw.py`, whose mish header offsets were wrong
once and failed *silently* — `start_sector` and `sector_count` sit at 8 and 16
and those were right, so the census output looked correct while the chunk count
at 200 and the chunk array at 204 were being read from the wrong place.
`tests/test_dmg2raw.py` pins every one of those offsets with distinct sentinel
values, plus an end-to-end conversion of a synthetic UDIF image that checks raw,
zero-fill and zlib chunks each land on the right sector.

The same gate also checks the **docs**, in one module.
`tests/test_documented_paths.py` covers all five present-tense docs — `CLAUDE.md`,
this file, `phase0-kext/README.md`, `kextshim/README.md` and `tools/README.md` —
and every path any of them names in prose must exist, plus the assertions that are
specifically about `CLAUDE.md` (all four gate commands named, the
`unittest discover` recipe still valid). Measured 2026-08-13, the four non-CLAUDE
docs named 73 path tokens between them with nothing checking any of them.

It was two modules for part of 2026-08-13, because two terminals wrote the
multi-doc extension at once. They were folded into one that day. The fold was not
a formality: the second module resolved paths **root-relative first**, this one
tries the **doc's own directory first**, and `README.md` written inside this file
resolves to a different real file under each ordering — both `README.md` and
`tests/README.md` exist. Doc-relative wins, because that is what an author in a
subdirectory means. `test_doc_relative_beats_root_relative_on_a_real_collision`
is what stops that being re-decided by accident, and it fails if the root
`README.md` is ever deleted, because then the collision it tests no longer exists.

The rule that extension needed is **doc-relative resolution**. A doc inside a
subdirectory writes paths as seen from its own directory — this file says
`../STEPS.md`, `phase0-kext/README.md` says `../PLAN.md` — and all of those are
correct while being nonsense from the repo root. So the resolver tries the doc's
own directory first (`DOCREL`), then the repo root (`DIRECT`), then one level
above the doc (`PARENT`, which is how `kextshim/` inside
`tests/kextshim/README.md` resolves to itself), and only then a bare basename
(`TAIL`). A `.../` elision is globbed separately, and a join that climbs out of
the tree is reported as `ESCAPES` rather than resolved or silently dropped.

Two of those verdicts are only correct because of what they refuse. `TAIL` is
anchored at a separator, so `kext/NVDecode.hpp` must NOT be satisfied by the real
`phase0-kext/NVDecode.hpp` — and the obvious test for that, `Decode.hpp`, proves
nothing, because the index is keyed by basename and the lookup misses on the key
before the comparison runs. `TAIL` and `ELISION` both return `AMBIGUOUS` on two
or more hits instead of the first one, or the verdict would depend on `os.walk`
order. Every token that still cannot resolve is declared with a reason —
`EXPECTED_ABSENT` for a path whose *absence* is the documented fact,
`OUT_OF_TREE` for one that names something outside this repository, and
`EXPECTED_ABSENT_DIRS` as the directory counterpart of the first. A test asserts
each entry is still named by some doc, so an exemption cannot outlive the text it
was written for. The tables are named rather than counted on purpose: a count in
prose is a magnitude nothing checks, and it goes stale the moment one is added.

The same gate also refuses a defect that has no red anywhere: a name bound
twice in one scope. Python takes the second definition and discards the first
without complaint, so two `def test_x` in one class, or two `class Fixtures` in
one module, delete the earlier tests from the run and the suite still prints
`OK`. The only symptom is a test count that quietly went *down*, which reads
like the suite got faster. `tests/test_no_duplicate_definitions.py` walks every
repo-owned `.py`, parses it, and reports any name bound twice in the same module
or class body — plus repeated keys in a dict literal, which is the same
overwrite in a different shape and matters because several tables here are keyed
by `(document, token)` tuples, where a duplicated pair would silently drop an
expectation and then redden the gate naming a token its own table looks like it
covers. Definitions under `if`/`try` fallbacks, `@property` setters and
`@overload` stacks are exempt, because exactly one of those binds at runtime or
the repetition *is* the API. The scan was clean the day it was written, so the
detector's own behaviour is driven from source strings rather than from the
tree — otherwise it could return "nothing found" unconditionally and every
assertion above it would be vacuously true.

The same gate also covers the **guest side of STEP 7**.
`tests/test_guest_build_kext.py` runs `tools/guest-build-kext.sh` for real under
Git Bash's `sh`, with `NV_PAYLOAD_ROOT` and `NV_WORK_DIR` redirected at
temporary directories, and asserts its three refusal paths print distinguishable
messages. That script gets exactly one attempt in front of an operator at the
guest console who cannot easily report back, so a defect in it costs a power
cycle and reports the *harness* rather than the code under test — the same shape
as a bare-LF `.bat`. Two defects were already in it: two `if` blocks closed with
`}` instead of `fi` (the whole file was rejected by `sh -n`, so it would have
produced no log at all), and `find_payload()` globbed a hardcoded `/Volumes`
while the rest of the script had been parameterised — so the override that makes
host testing possible was dead code and every execution test would have passed
vacuously against a directory Windows does not have. Both are pinned. The module
stops before `make`, so nothing in it speaks to whether the kext compiles. That
question was answered out-of-band on 2026-08-14 instead: the script ran in the
real guest over SSH and `make` came back 0 with the bundle present
(`step7-results/step7-build-SUCCESS.log`). Note which half of the script that
validates — the execution tests here cover its refusal paths, and the guest run
covered its success path exactly once.

The same module also gates the payload's **freshness**, which is a different
claim from its shape. `build-payload/` is a build artifact sitting beside its own
sources, and the delivery path is one-shot: the operator powers the guest down,
repoints the CD, boots, runs the script, and reads the result off the screen. A
tar that predates the last edit to `phase0-kext/` sends that whole round trip
after code nobody is working on, and nothing about the output would look wrong.
It also fails in the flattering direction — every earlier check in the module
passes against a stale tar, because they were written against the archive's
*shape* and shape is exactly what an outdated copy preserves. So the freshness
checks compare **contents**, byte for byte, deriving the file list by walking the
tree rather than listing it. `guest-build-kext.sh` and the burned ISO are checked
too: they are a second and third copy, and the staged script really did drift
from `tools/` once this session while every static check still read `tools/` and
reported the syntax error fixed.

Detecting drift is the weaker half, so `tools/build-step7-payload.py` removes the
class instead of reporting it. It walks `phase0-kext/`, writes the tar with
`tarfile` (GNU tar is not guaranteed on a Windows PATH, and a missing binary
fails in a way that reads as a broken payload), hands the ISO to
`deliver-via-cd.py --no-vmx`, and **verifies against the working tree before
anything moves into place** — everything is built in a system temp directory, so
a failed run cannot leave a truncated tar or an ISO older than its inputs. Those
are the two shapes the operator cannot detect from the guest console: a short tar
unpacks fine and just omits a header. The temp directory is outside the repo
because an earlier attempt that staged inside the tree duplicated every kext
header basename for hours, and the doc-path gate resolves by basename.

`tests/test_build_step7_payload.py` gates the tool, in three parts: static shape
(the member list must be walked, never named — a literal list goes stale the
moment a sixth header lands, which is the same defect one level up), an
end-to-end run into a temp `--out-dir`, and nine refusals handed to `verify()`
directly. Two of its own checks were wrong on the first run and both are worth
knowing: one asserted a message that `--skip-gate` short-circuits before
reaching, so it measured the wrong branch; the other ran a substring check over
the whole file including the module docstring, which names the abandoned staging
directory as history. The fix for the second is `code_only()`, which strips the
docstring with `ast` rather than slicing on a `"""` pair — a naive slice would cut
at the wrong one of several and silently return too much, making those checks
vacuous rather than red. A guard test asserts the strip removed the prose and
kept the code.

`tests/_negctl_payload_freshness.py` is the negative control, run by hand:

```
python tests\_negctl_payload_freshness.py
```

It is underscore-prefixed so `unittest discover` skips it — it writes
deliberately-wrong tars, and only ever in a temp directory. Five mutants, each
asserted **caught**: an edited header, a missing file, an extra file, a truncated
file, and a single flipped byte at unchanged length. That last one is the
narrowest statement of the defect, since any comparison based on size or mtime
survives it. It also pins two **misfires** — an edited file must not be reported
as missing or extra, or the message sends the reader to the wrong problem — and
treats a SKIP as a survivor, because a mutant that never reached the assertion
would make the whole probe vacuous.

The same gate also reads the **kext link line**, in `tests/test_kext_makefile.py`.
`phase0-kext/Makefile` is the most consequential unexercised file here: every
other artifact has a host-side gate and this one has run zero times, on any
machine, ever — and it is what STEP 7 puts in front of Apple clang on a single
attempt. Read closely for the first time on 2026-08-13 it had a real defect. The
link line carried `-Xlinker -kext -Xlinker -object_path_lto -lkmod`, and
`-Xlinker` passes exactly **one** token to ld. ld64's flags split into two
classes that are indistinguishable by shape: `-kext` takes no argument,
`-object_path_lto` takes a **path** read from the next argv entry. So ld consumed
`-lkmod` as the LTO object path and **`libkmod.a` was never linked** — the
library supplying `kmod_info` and the start/stop glue, without which the bundle
is not something the kernel loads. Not a rejected flag; a silently dropped
library. No `-flto` appears in `CXXFLAGS` either, so it was removed rather than
given a path.

The generalisation is the point, because a test that only banned that one flag
would be a patch for one line. `ARG_TAKING_LD_FLAGS` lists fifteen ld64 flags
that consume a following argv entry, and the rule applies to all of them: present
means it must have a value, and routed through `-Xlinker` it needs a **second**
`-Xlinker` to carry that value. The negative controls drive the parser against
the verbatim broken line, the fix, a correctly-paired value, and the near-miss
where the path is present but goes to the compiler driver instead of ld.
`NO_ARG_LD_FLAGS` proves the parser distinguishes the two classes rather than
condemning `-kext`, which every kext needs. The module also pins what the
Makefile's own comment says costs a load-time failure rather than a build error —
the kernel dialect flags, `-DKERNEL`, that `KERNEL_FLAGS` is actually
*referenced* by `CXXFLAGS` (existing is not the same as being used), both
`Kernel.framework` include paths, and that the SDK and compiler come from `xcrun`
rather than a hardcoded path that is wrong on every machine but the one it was
written on. `SourceListTests` pins `SOURCES` against the `.cpp` files on disk in
both directions: a header there would mean the logic/IOKit split had stopped
being real, and a second `.cpp` added but not listed is an undefined symbol at
load time rather than a build error.

It claims nothing about whether the kext builds — there is no Apple clang here
and no ld64. It asserts properties of a command line that are true or false by
inspection, and the `-object_path_lto` bug was exactly that shape.

The same gate also bounds the C++ suites' **check counts**, in
`tests/test_check_count_floor.py`. `tests/test_check_count_last_line.py` proves
each suite's count line is the *last* line; it says nothing about the numbers in
it. Gut a `main()` down to `return 0`, `#if 0` a block of check calls, or lose a
suite to a link change, and the output is `0/0 passed, 0 failed` — which
satisfies that gate, exits 0 under `ctest` because there are zero failures, and
reads to a human as success. Position and magnitude are independent properties
and only one of them was checked.

The floors are **derived**, not typed. One dated `MEASURED` entry per suite
carries the number read off the binary, and the floor is `measured // 2` computed
in code — two typed numbers drift apart, one cannot disagree with its own
provenance. Halving is deliberately generous: the defect is a count collapsing to
zero, not drifting by a dozen under refactoring, and a floor that reddens on
ordinary churn gets edited rather than investigated. The *set* of suites comes
from globbing `tests/test_*.cpp`, so a sixth suite cannot be silently omitted the
way a hand-kept list omits things. It also refuses a `.exe` older than its
`.cpp`, because a stale binary's count is evidence about code nobody is running,
and refuses a built `tests/build/` that is missing a listed binary — that is
CMake having stopped registering it, not an absence of news. The source-side half
(each suite calls check helpers many times over) runs unconditionally, since this
is the one gate that works on a bare checkout where no binary exists.

`tests/_negctl_check_count_floor.py` is its negative control, run by hand:

```
python tests\_negctl_check_count_floor.py
```

It monkeypatches the gate's `subprocess.run` and repoints its `HERE`/`BUILD_DIR`
at a `mkdtemp` copy, so it mutates nothing inside `tests/` — a probe that writes
here hands a concurrent gate run a specific, plausible failure with a real
traceback and no defect behind it. Eleven planted defects are each asserted
caught: a suite reporting `0 checks`, one reporting a single check below its
floor, a final line that is prose, a suite printing nothing, a new `.cpp` with no
`MEASURED` entry, a `MEASURED` entry whose source is gone, a recorded count of
zero (a floor that cannot fail), a missing binary, a stale binary, and a source
gutted of its check calls. It also asserts a suite landing *exactly* on its floor
**passes** — without that, the below-floor case could be green because the gate
rejects everything.

The second gate is new and is documented in `kextshim/README.md`. It uses the
clang 19.1.5 that ships inside Visual Studio 2022 plus a minimal IOKit/libkern
stub tree, and it is guarded by a negative control that fails the run if any
planted defect compiles clean. It is a **parse, not a build**. The build it stands in for has since happened
once, in the macOS guest on 2026-08-14 — Apple clang 17.0.0, `make` exit 0,
bundle present — which is the first evidence that this stub tree is not merely
self-consistent but actually agrees with Apple's real headers. It does not retire
the gate: the guest build takes minutes and needs a VM, this takes seconds and
runs on a bare Windows checkout, and only this one carries negative controls.

That gate also reads `phase0-kext/Info.plist`, which until 2026-08-13 was the
last load-bearing file in the kext with nothing checking its contents. What makes
it worth two sections is the shape of its failures: **not one of them is a
compile error.** The kext builds, the bundle assembles, `make` exits 0, and the
defect surfaces on the target laptop — which costs a power cycle and physical
access to reach.

Section 7, `tools/check-kext-personalities.py`, covers the matching dictionary:
every `IOPCIMatch` must name a device that `DEVICE-IDS.md` attributes to the die
this project targets, every personality needs an `IOClass`, and every
`IOProviderClass` must be `IOPCIDevice`. A personality matching the wrong device
is a one-nibble mistake that loads fine and then never probes, because IOKit
never hands it a provider — on the operator's screen indistinguishable from a
broken `probe()`, from SIP, and from an unsigned-kext refusal.

Section 8, `tools/check-kext-bundle.py`, covers bundle **identity**: the
executable name, the identifier, the package type, the version keys and
`OSBundleLibraries`. `CFBundleExecutable` names the binary inside
the bundle's `MacOS` directory, which the Makefile builds under `KEXT_NAME`, so drift makes
`kmutil` refuse the bundle for a *missing executable* while naming a file that is
plainly present under a different name. `CFBundleIdentifier` is spelled three
times — in the plist, again as the Makefile's `BUNDLE_ID` (what `make unload`
targets), and a third time inside each personality (which tells IOKit who owns
it) — so changing one makes `make unload` unload nothing while still exiting 0.
`IOClass` is resolved **by string** through the `OSMetaClass` registry rather
than by the linker, so a typo links clean and fails at match time on hardware
with nothing in the build log. A missing `OSBundleLibraries` entry is an
undefined symbol at load time.

The checker types **no** expected string. `KEXT_NAME` and `BUNDLE_ID` are parsed
from `phase0-kext/Makefile` and `$(...)`-expanded; the legal `IOClass` names come
from the `OSDefineMetaClassAndStructors` calls in the sources; the required
libraries are derived by matching API families against those same sources. A
literal would have reproduced inside the gate the exact defect — a second
hand-kept copy of a string — that the gate exists to catch. Three parsing
decisions follow from that: `+=` deliberately does not parse as a definition (the
real Makefile appends to `CXXFLAGS` under an `ifdef`, and treating append as
assignment would let a later append beat the real `:=`); an unresolvable `$(FOO)`
is left unexpanded and fails loudly rather than collapsing to `""` the way make
would, since an empty expansion turns a typo into a comparison whose message
names neither file; and `OSDeclareDefaultStructors` is read as different from
`OSDefineMetaClassAndStructors`, because declare alone registers nothing with the
runtime — grep finds the class name and the bundle still cannot instantiate it.

`tests/test_check_kext_bundle.py` is the negative control, and the assertion
carrying the design is `test_removing_both_the_api_and_its_library_is_silent`: it
deletes `IOPCIDevice` from the sources **and** `com.apple.iokit.IOPCIFamily` from
the plist, then asserts the checker says nothing at all. A checker with the
library list hardcoded passes every other test in the module and fails only that
one, so without it "derived from the sources" would be an unverified claim about
the implementation. `SyntheticBaselineIsClean` runs first, because a fixture that
fails for an unrelated reason makes every mutation below it green without any of
them reaching the line it targets. Every assertion checks the printed **verdict
word** rather than only the exit code — a checker that dies before evaluating
anything also exits non-zero, and "rejected the tree" and "never reached its
verdict" are not the same evidence.

Neither section contacts Apple's SDK. That the *versions* in `OSBundleLibraries`
exist, and that the KPI names are spelled the way a given macOS release spells
them, was unproven here until the 2026-08-14 guest build compiled and linked the
kext against Apple's real KPI. What these two sections prove on their own,
without any SDK, is internal consistency: the
plist, the Makefile and the C++ agree about what this kext is called and what it
uses. The `com.apple.kpi.*` entries are additionally checked against *each other*
— they are versioned with the kernel as a set, so a mixed set names a kernel that
never shipped — without typing a version number anywhere, which would have been
yet another hand-kept copy.

Current status: **five suites, 747/747 passing** (64 + 169 + 116 + 217 + 181).
Measured 2026-08-13 by running all five binaries in `tests/build/` after a real
relink — not by editing the previous numbers. Four of the five moved that day:
`test_decode` 39 -> 64 and `test_riscv_ucode` 85 -> 92 -> 116 from the
separating-input sweep (the second step pinned the container-header and the
full descriptor offset sequence against bare literals), `test_gsp_stage`
142 -> 162 -> 174 -> 181 (the second step from `nvGspStagePolicyFitsRegisters()`
-- the POLICY ceiling must stay inside what DMATRFBASE/BASE1 can represent,
driven with separating inputs so the comparison is shown able to come out false;
the third drives `nvGspDmaPhysHiFits()` itself with the policy ceiling's high
word, because comparing two limit *constants* is not a statement about the
predicate the DMA path actually consults, and the two are assembled from
different sources), and
`test_gsp_boot` 199 -> 208 -> 217
(the second step from `nvGspDmaPhysHiFits()` -- three refusal cases above the
17-bit DMATRFBASE1 window, the accepted 0x1FFFF boundary on the far side, and
the predicate driven directly).
The counts below were read off the binaries, not off a previous revision of this
file — earlier revisions said "two suites, 204/204" and then "three suites,
289/289", and each became a *false regression report* the moment a suite was
added without touching this line. If you add a suite, run it and update the
total; if you find the total wrong, the binaries are the authority.

| Suite | Covers | Checks |
|---|---|---|
| `test_decode` | `NVDecode.hpp` — Phase 3 identity, Phase 3.5 firmware health | 64 |
| `test_falcon` | `NVFalcon.hpp` — Phase 4 Falcon / RISC-V / GSP / WPR2 | 169 |
| `test_riscv_ucode` | `NVRiscvUcode.hpp` — `RM_RISCV_UCODE_DESC` parsing, GA107 blob geometry, offset pinning | 116 |
| `test_gsp_boot` | `NVGspBoot.hpp` — Phase 4/5 bringup state machine, DMA write order, physHi representability | 217 |
| `test_gsp_stage` | `NVGspStage.hpp` — Phase 5 staging arithmetic, 41-bit window, chunk padding, base+length range, policy-vs-representability | 181 |

`test_gsp_boot` and `test_gsp_stage` are the two that assert *sequence* rather
than values: the former pins journal indices (see the write-order note in
`../CLAUDE.md`), the latter pins the blob-relative to file-relative offset
translation. Both are meaningless as final-state checks.

The wrapper runs them through `ctest`, not by name. An earlier version invoked
`test_decode.exe` directly, so when `test_falcon.cpp` was added it compiled,
linked, and then silently never ran — a green wrapper that was not testing what
it claimed. Suites registered via `add_kext_test()` in `CMakeLists.txt` are now
picked up automatically.

## How the split works

`phase0-kext/NVDecode.hpp` holds every register constant and every bit-field
decode. It is deliberately dependency-free — no includes at all, every type a
plain builtin — so it compiles two ways:

| Compiled by | Where | Can it run? |
|---|---|---|
| Apple clang, `-mkernel -fapple-kext` | inside the kext | on the laptop, eventually |
| MSVC 14.51, `/W4` | `tests/test_decode.cpp` | **yes, right now** |

`NV3050Probe.hpp/.cpp` is the IOKit shell around it and declares no registers of
its own. So the bit twiddling that ships is the identical code the tests
exercise — not a copy that can drift.

## The test that justifies the whole arrangement

The first version of the architecture decoder read `NV_PMC_BOOT_0` bits 28:24
only. That returns the **correct** answer for Ampere, because bit 8 happens to
be zero on every Ampere part. The bug was invisible on precisely the hardware
being targeted.

`testSplitArchitecture()` synthesises a value with `ARCHITECTURE_1` set and
asserts the high bit is folded in:

```
bit8 clear: arch 0x17      -> 0x17
bit8 set:   arch 0x37      -> 0x37    <- a single-range decoder returns 0x17
max: low 0x1F + bit8       -> 0x3F
```

A one-range decoder passes every other check in the file and fails only this.
That is the shape of bug worth building a harness for.

It also asserts `ARCHITECTURE_0_MASK == (1 << WIDTH) - 1`, so the high bit lands
exactly above the low field rather than overlapping it or leaving a gap.

## What else is covered

- **`BOOT_0` / `BOOT_42` agreement** — both registers must report the same
  architecture for the same part
- **GA106 vs GA107** — chip ID `0x176` vs `0x177`. GA106 also sells as "RTX 3050
  Ti Laptop GPU", so recognising "Ampere" is not sufficient
- **Documented values** — assembles `0x17700000` from the constants and compares
  against what `DEVICE-IDS.md` and `phase0-kext/README.md` print. If the
  constants drift, the docs start lying to whoever is reading a kernel log at 2am
- **Dead-read detection** — only all-ones and all-zeroes are dead. `0xFFFFFFFE`
  is live data; classifying it as dead would hide a real misread behind a
  friendlier error
- **GFW boot progress** — field is 7:0, so a dirty upper half must not change the
  verdict (`0xDEADBEFF` is complete)
- **Aperture bounds** — both GC6 island offsets sit inside the 16 MiB BAR0, or
  Phase 3.5 reads run off the end of the mapping

## Why the batch wrapper exists

`build-msvc.bat` looks over-engineered for "call vcvars, run cmake". Two real
quoting hazards forced it, both recorded in its comments:

1. **No `if not exist "%VCVARS%"` guard.** `cmd` parses the parentheses in
   `Program Files (x86)` as block delimiters inside an `IF`, producing
   `\Microsoft was unexpected at this time.`
2. **`PATH` is reset to a minimal value before calling vcvars.** The inherited
   `PATH` contains unquoted entries with parentheses — VMware among them — and
   vcvars' own internal `IF` statements break on them:
   `\VMware\VMware was unexpected at this time.` vcvars rebuilds the compiler
   `PATH` from scratch, so nothing is lost; cmake and ninja are added back
   explicitly afterward.

## The Phase 4 equivalent: three address spaces

`test_falcon.cpp` exists for a bug with the same shape as the split-architecture
one — wrong, and silent.

A GSP register access can be expressed relative to three different bases, and
NVIDIA's headers use all three without ever stating the relationship in one
place:

| Header | Base | Example |
|---|---|---|
| `dev_falcon_v4.h` | engine-relative | `MAILBOX0` = `0x40` |
| `dev_riscv_pri.h` | `NV_FALCON2_GSP_BASE` = `0x111000` | `CPUCTL` = `0x388` |
| `dev_gsp.h` | already absolute | `NV_PGSP_FALCON_MAILBOX0` = `0x110040` |

The RISC-V base is **`0x111000`, not `0x110000`** — one 4 KiB page above the
Falcon block. Collapse those two and every RISC-V access lands inside the Falcon
register file, *where every offset is a valid register*. It does not fault. It
reads and writes real hardware at the wrong address, and the only symptom is a
firmware boot that hangs with no diagnostic.

So the header exposes `nvGspFalconReg()` and `nvGspRiscvReg()` rather than
letting anyone write a bare offset, and the tests assert:

```
falcon(0x388) -> 0x110388
riscv(0x388)  -> 0x111388     <- must NOT be the same address
```

plus the anchor that proves the base is right rather than merely consistent:
`nvGspFalconReg(0x40)` must equal `dev_gsp.h`'s independently-spelled
`0x110040`. Two NVIDIA headers that never reference each other, agreeing.

## Other Phase 4 coverage

- **Indexed port strides.** `IMEMC/IMEMD/IMEMT` are a 16-byte triple;
  `DMEMC/DMEMD` an 8-byte pair; `QUEUE_HEAD` an 8-byte slot despite only 32 bits
  being defined. One stride used everywhere aliases ports onto each other
- **`MEM_SCRUBBING` has inverted sense** — it reads **0** when scrubbing is
  *done*. Read it the obvious way and you poll forever on a Falcon that was
  ready immediately
- **Adjacent bits do not bleed.** `HALTED` (bit 4) and `ALIAS_EN` (bit 6) share
  a register; `DMA FULL` (bit 0) and `IDLE` (bit 1) are adjacent. Each decoder
  is tested against the other's bit
- **`IMEM_SIZE` is in 256-byte blocks**, not bytes
- **The assembled boot `BCR_CTRL`** — `VALID | CORE_SELECT=RISCV | BRFETCH`
  must come out to `0x111`, confirming the shifts do not overlap
- **WPR2's two different shifts.** The `VAL` field is *extracted* with a shift
  of 4, but the value it holds is a 4 KiB page number, so recovering the
  physical address shifts back up by **12**. Use 4 in both directions and a
  2 GiB WPR base reads as 8 MiB — a plausible-looking number, which is the
  worst kind of wrong. The suite asserts the correct round-trip *and*
  explicitly asserts the mis-scaled value does not appear

## Section 9: every `complete()` is guarded by a flag that tracks its `prepare()`

`tools/check-prepare-balance.py` exists for one defect that was really in this
driver, found 2026-08-13: `releaseGspFirmware()` called
`fGspFwBuffer->complete(kIODirectionOut)` unconditionally, and
`loadGspFirmware()` calls that teardown from every failure path — including the
one whose entire purpose is "`prepare()` returned non-success".

`IOMemoryDescriptor` carries a **wire count**. `prepare()` increments it,
`complete()` decrements it, and a `complete()` with nothing behind it drives that
counter below its own floor. The damage lands in kernel VM bookkeeping, which is
nowhere this driver prints — the driver's own view of itself stays perfectly
consistent, so `NVLOG` says nothing.

What makes it worth a gate is that all four existing gates are blind to it
**structurally**, not by oversight:

| Gate | Why it cannot see this |
|---|---|
| Sections 1–3 (clang) | They prove the file *parses*. `fGspFwBuffer->complete()` is a well-formed call whatever preceded it; a prepare/complete balance is not a parse property |
| The MSVC suites | They compile only the dependency-free headers. This code is in the IOKit shell, on the far side of that split |
| `tests/kextshim/` | It declares both methods and has no wire count to unbalance, so even driving the shell there would register nothing |
| `check-registers.py` | It reads constants |

That leaves the **shape** of the code as the only thing anything here can assert,
so that is what the script asserts: the flag is initialised `false` in `init()`,
set `true` immediately after the `prepare()` success check with no `return`
between the two, every `complete()` sits inside an `if (flag)`, the flag is
cleared *before* that call, and the object is released *after* it.

The two ways of being approximate fail in **opposite** directions, which is why
both are checked rather than one standing in for the other:

- **Set `true` too late** — a bail-out between `prepare()` and the assignment
  runs a teardown that skips `complete()`, *leaking* wired pages.
- **Clear too late** — a re-entrant teardown sees the flag still `true` and
  completes a *second* time, double-decrementing.

The flag's name is not typed anywhere in the checker. It is derived: find the
`prepare()` failure check, and the boolean set `true` immediately after it is the
flag by definition. A second hand-kept copy of a driver identifier is the defect
class this project keeps finding, so a rename in the driver follows by itself
rather than silently disarming the gate.

If the `prepare()` site is not in a shape the script can delimit, it **fails**
rather than reporting OK. A checker that cannot find its subject and prints a
green tick is worse than no checker, because it converts an unexamined file into
evidence. `tests/test_check_prepare_balance.py` pins that: 23 checks, one
mutation per assertion, the unmutated fixture asserted clean first, and each
mutation required to be rejected *by name* so a run that goes red for an
unrelated reason cannot masquerade as coverage.

## What this does NOT prove

The wire count itself is still unobserved. Nothing available on Windows can
count it — section 9 asserts the code's shape, not its runtime behaviour, and a
shape that is correct here could still be wrong about what the real
`IOMemoryDescriptor` does with a directional `complete()`. That only settles on
hardware.

The kext has still never been compiled. IOKit API usage and the `OSDeclare`/
`OSDefine` macro expansion against the real `libkern` are unproven until Step 7
puts them in front of Apple's clang. Expect that first `make` to fail on
something.

`phase0-kext/Makefile` is a narrower case and worth stating precisely, because
"unproven" covers two different things there. Whether the flags are *correct for
this kernel* is unknowable here. Whether they are *well-formed as a command line*
is decidable by inspection, and `test_kext_makefile.py` decides it — that is how
the `-object_path_lto` defect was found. A flag that needs an argument and has
none will now fail here rather than in the guest; a flag that is spelled
perfectly and is simply the wrong flag will not.
