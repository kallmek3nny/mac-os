# handoff10 — the first credential-free path to STEP 7, and a guest script
# tested on the host

Newest handoff. Read `handoff9.md` for the state this continues from, and
`CLAUDE.md` for the four verification gates.

**This session did not change the project's ceiling either — but it is the first
one that moved toward STEP 7 rather than around it.** The kext still has never
been compiled with Apple clang. What changed is that everything standing between
here and that answer is now built, verified, and reduced to one power cycle plus
one command typed at the guest console. That is written up as
`OPERATOR-ACTION-step7-build.md`.

All four gates were re-run at the end and the literal verdicts are in §5.

---

## 1. Why the VM could not simply be told to build the kext

The guest is up: `vmrun list` reports `Sonoma.vmx` running, `checkToolsState`
returns `running`, and `getGuestIPAddress` returns `192.168.255.129`. So the
tools channel is live — and none of that helps, because every `vmrun` operation
that *executes* something inside the guest (`runScriptInGuest`, `runProgram-
InGuest`, `captureScreen`) requires `-gu/-gp`, and the account password is still
unknown (`OPERATOR-ACTION-guest-creds.md`, crew card `t-396740`).

A live tools channel is not an execution channel. Worth stating plainly, because
`checkToolsState: running` looks like a green light and is not one.

What *is* credential-free is a virtual CD. `tools/deliver-via-cd.py` already
existed for exactly this and its 53-check self-test passes. So the shape of the
session was: build a payload the guest can mount, and a script the operator can
run in one line.

---

## 2. `tools/guest-build-kext.sh` — the guest half of STEP 7

It finds the payload, unpacks it to `$HOME` (not to the ISO — that medium is
read-only and `make` would fail on its first write with an error describing the
medium), records the toolchain, runs `make`, and prints the tail of its own log.

Two decisions worth keeping:

**The toolchain block runs BEFORE `make`, not after.** `xcode-select` pointing
at the Command Line Tools rather than full Xcode is the most likely reason a
first `-fapple-kext` build fails on a fresh install, and `Kernel.framework`
ships only with full Xcode. Recorded after a failing `make`, that block may
never be reached — and a build log that does not name its clang cannot be acted
on from the host.

**Payload discovery globs `/Volumes/*/kext-src.tar` rather than assuming the
mount point.** macOS appends a disambiguating suffix when a stale `PAYLOAD`
volume is still mounted, so a hardcoded path fails with "no such file" — which
reads as a missing payload rather than a renamed one.

It does **not** load the kext. `kextload` needs SIP lowered and a reboot, and
this VM has no GA107 to probe, so loading here would prove nothing about the
target laptop. The claim on offer is exactly one: the source compiles, or here
is the error.

---

## 3. The script had to be testable on the host, and finding out why cost two
## red runs

A script delivered on a CD gets **one attempt** in front of the one person who
cannot easily report back what it printed. A defect in it burns a power cycle
and an operator round trip, and it reports the harness rather than the code under
test — the same failure shape `tests/test_line_endings.py` exists to prevent.

So `NV_PAYLOAD_ROOT` and `NV_WORK_DIR` are overridable, defaults are the real
guest paths, and `tests/test_guest_build_kext.py` (17 tests) runs the script for
real under Git Bash's `sh` against fake payloads.

Three defects were caught this way, and the order matters:

**Two `if` blocks closed with `}` instead of `fi`.** `sh -n` rejected the entire
file, so in the guest it would have died before line 1 and produced **no log at
all** — the worst possible outcome, because "nothing printed" is
indistinguishable from "the CD never mounted".

**`find_payload()` globbed a hardcoded `/Volumes`** while the rest of the script
had been parameterised on `$PAYLOAD_ROOT`. The override was dead code. This is
the interesting one: `/Volumes` does not exist on Windows, so every execution
test would have taken the "no payload" branch and **passed for the wrong
reason** — six green tests measuring nothing. It is the
`an-assertion-that-cannot-fail-is-not-coverage` finding arriving through the
parameterisation rather than through the assertion. The module now pins both
sides: `find_payload` must reference `$PAYLOAD_ROOT` and must not contain
`/Volumes`, *and* the default must still be `/Volumes` — a parameterisation that
defaulted to a temp directory would pass every test here and find nothing in the
guest.

**Windows paths handed to `sh` fail asymmetrically.** The harness first passed
`C:\Users\...` straight through. The glob in `find_payload` expanded it and
`[ -f ]` agreed the tar was there — discovery *passed* — and then `tar -xf -C`
failed, because backslashes are escape characters to both the shell and tar. The
script printed `tar could not unpack`, a truthful message about a fault that
existed only in the test. Fixed with a `posix_path()` helper (`C:\x` → `/c/x`)
and documented at length, because this is the failure mode the module exists to
prevent arriving from the test side. `cygpath` would also do it, but it is not
guaranteed present and a missing helper would *skip* these tests rather than run
them.

---

## 3b. The payload is a build artifact checked in beside its sources

Added after the gates were first green, because it is the one failure this
session's work could produce that nobody would notice.

`build-payload/kext-src.tar` is a copy of `phase0-kext/`. Nothing asserted it
still *was* one. A tar that predates the last edit to the tree sends the operator
through a power cycle, a CD repoint, a boot and a console session to compile code
nobody is working on — and the compile error, or the clean build, describes a
version of the driver that no longer exists. Nothing about the output looks
wrong.

It fails in the flattering direction, which is what makes it worth a gate. Every
check already in `test_guest_build_kext.py` passes against a stale tar: the
Makefile is still at the right depth, every header basename is still present, no
`build/` rides along. Those were written against the archive's **shape**, and
shape is exactly what an outdated copy preserves.

`PayloadFreshnessTests` therefore compares **contents**, byte for byte, and
derives the file list by walking the tree — a hardcoded list would go stale the
moment a sixth header lands, which is the same defect one level up. Byte
comparison rather than mtime, because the tar is rebuilt by hand and its member
timestamps say when the *archive* was made, not what is inside it.

There are **three** copies, not one, and the middle one had already drifted:
`tools/guest-build-kext.sh` → `build-payload/guest-build-kext.sh` → the burned
ISO. Both brace-instead-of-`fi` fixes in §3 were made in `tools/` while the
staged copy and the ISO still carried the broken version, and every static check
in the module reads `tools/` — so the syntax error was reported fixed while the
artifact the operator would actually run still had it. All three are now pinned.
The ISO is compared by size rather than bytes: reading ISO9660 content needs
`pycdlib`, which `run-python-tests.bat` deliberately does not require. A size
match is weak, but a payload larger than the image that should contain it is
conclusive proof the image is older than its inputs.

### The negative control matters more than usual here

The real payload is currently correct, so all five freshness checks pass — and a
check that has only ever seen a correct input is indistinguishable from one whose
comparison silently does nothing. `tests/_negctl_payload_freshness.py` builds
stale tars in a temp directory and asserts each is caught: an edited header, a
missing file, an extra file, a truncated file, and **a single flipped byte at
unchanged length**. That last one is the narrowest statement of the defect, since
any comparison based on size or mtime survives it.

Two details worth reusing. It asserts **misfires** as well as catches — an edited
file must not be reported as missing or extra, because a message naming the wrong
failure sends the reader to the wrong problem. And it treats a **SKIP as a
survivor**: `PayloadFreshnessTests` skips when the tar is absent, so a mutant
that somehow removed the artifact would skip every check and read as green,
making the entire probe vacuous.

Run: `python tests\_negctl_payload_freshness.py`. Exit 0 = all caught. It is
underscore-prefixed so discovery ignores it, the same way `_negctl_check_count.py`
is.

### 3c. Detecting the drift is the weaker half

`tools/build-step7-payload.py` removes the class rather than reporting it. The
rebuild was three hand-typed commands with an ordering requirement, recorded only
inside a docstring's `REBUILD` string — and forgetting the middle one is exactly
the drift that already happened in §3b, where both `fi` fixes landed in `tools/`
while the staged copy and the ISO kept the broken version.

The design claim is **nothing lands until it verifies**: tar, script and ISO are
built in a system temp directory, compared against the working tree there, and
only then moved into `build-payload/`. A failed run cannot leave a truncated tar
or an ISO that predates its inputs, which are the two shapes the operator cannot
detect from the guest console — a short tar unpacks cleanly and just omits a
header. The temp directory is outside the repo because `payload-stage/` inside it
duplicated every kext basename and the doc-path gate resolves by basename.

It writes the tar with `tarfile` rather than shelling out: GNU tar is not
guaranteed on a Windows PATH and a missing binary reads as a broken payload.
USTAR, sorted members, uid/gid 0 and empty uname/gname — extraction in the guest
happens as an ordinary user, and a member owned by a uid that does not exist there
makes bsdtar attempt a chown it cannot perform, whose warning reads as a corrupt
archive.

`tests/test_build_step7_payload.py` (34 checks) gates it: static shape, an
end-to-end run into a temp `--out-dir`, and nine refusals handed to `verify()`
directly, including a single flipped byte at unchanged length and a staged script
that drifted from `tools/`.

**Two of those tests were wrong on their first run**, and both failure shapes are
reusable. `test_it_declines_to_run_the_gate_against_a_different_artifact` lived in
the class that passes `--skip-gate`, and `--skip-gate` is checked *first* — so the
branch it asserted was never reached and the assertion measured a different
message. It now has its own class that omits the flag. And the `ShapeTests`
substring checks ran over the whole file including the module docstring, which
names `payload-stage/` as abandoned history — so the tool's own documentation
failed a test about the tool's behaviour. `code_only()` strips the docstring with
`ast` rather than slicing on a `"""` pair, because the file below it contains
several more and a naive slice would cut at the wrong one, silently returning too
much and making every check *vacuous* rather than red. A guard test pins that the
strip removed the prose and kept the code.

---

## 3d. The Makefile had a real defect, and nothing had ever read it closely

Found by reading `phase0-kext/Makefile` line by line for the first time. It is
the single most consequential unexercised file in the repository: every other
artifact has a host-side gate, and this one has run zero times on any machine.

The link line was:

```
-Xlinker -kext \
-Xlinker -object_path_lto \
-lkmod \
```

`-Xlinker` passes exactly **one** token to ld. ld64 splits its flags into two
classes that are indistinguishable by shape — `-kext` takes no argument,
`-object_path_lto` takes a **path**, read from the next argv entry. So ld saw
`-object_path_lto -lkmod`, took `-lkmod` as the LTO object path, and
**`libkmod.a` was never linked.** That library supplies `kmod_info` and the
start/stop glue; without it the bundle is not something the kernel will load.

It fails in the quiet direction: not a rejected flag, a silently dropped library.
And the flag was dead weight regardless — no `-flto` appears anywhere in
`CXXFLAGS`, so it was removed rather than given a path.

`tests/test_kext_makefile.py` (32 checks) is the gate. The generalisation is the
point: a test that only banned `-object_path_lto` would be a patch for one line.
`ARG_TAKING_LD_FLAGS` lists fifteen ld64 flags that consume a following argv
entry, and the rule is applied to every one — if present it must have a value,
and routed through `-Xlinker` it needs a **second** `-Xlinker` to carry that
value. `LinkFlagRuleTests` drives the parser against synthetic strings, including
the verbatim broken line, the fix, a correctly-paired `-Xlinker` value, and the
near-miss where the path is present but goes to the compiler driver instead of
ld. `NO_ARG_LD_FLAGS` proves the parser distinguishes the classes rather than
condemning `-kext`, which every kext needs.

The module also pins what the Makefile's own comment says costs a load-time
failure rather than a build error: the nine kernel dialect flags, `-DKERNEL`,
that `KERNEL_FLAGS` is actually *referenced* by `CXXFLAGS` (existing is not the
same as being used), both `Kernel.framework` include paths, and that the SDK and
compiler are discovered via `xcrun` rather than hardcoded — a pinned
`/Applications/Xcode.app/...` path is wrong on every machine but the one it was
written on, and the guest is not that machine. `SourceListTests` asserts
`SOURCES` equals exactly the `.cpp` files on disk, in both directions: a header
appearing there would mean the logic/IOKit split had quietly stopped being real,
and a second `.cpp` added but not listed produces an undefined symbol at load
time rather than a build error.

**The same docstring defect appeared a third time.** `test_lto_is_not_requested`
read the whole file, and the comment explaining why the LTO flag was removed
necessarily contains `-flto` — so the Makefile's own documentation failed a test
about the Makefile's flags. `code_only()` strips whole-line `#` comments, and
deliberately does *not* try to strip a trailing `#` from a recipe line, because a
`#` inside a shell command is not a make comment and a parser clever enough to
sometimes be right there would be silently wrong the rest of the time.
`CodeOnlyTests` pins both directions — prose gone, flags kept, recipe `#`
survives.

What this does **not** claim: that the kext builds. There is no Apple clang here
and no ld64. It asserts properties of a command line that are true or false by
inspection — and the `-object_path_lto` bug was exactly that shape.

---

## 4. What the payload is, and what is verified about it

`build-payload/step7-payload.iso` (266240 bytes, vol ident `PAYLOAD`) carries
`kext-src.tar` and `guest-build-kext.sh`. Verified host-side:

| Thing | How |
|---|---|
| The ISO is well-formed; both files round-trip under their **long** names, not 8.3 | `deliver-via-cd.py --verify` plus its 53-check self-test |
| The tar is rooted at `phase0-kext/` where the script looks | `test_guest_build_kext.py` |
| Every header the kext includes is present | same |
| No stale host `build/` rides along and overwrites the guest's output | same |
| The script parses under POSIX `sh`, is LF-only, and its three refusals are distinguishable | same |

An early attempt staged the tree through a `payload-stage/` directory. Don't:
it duplicates every header basename in the repo, and the doc-path gate resolves
by basename. `tar --exclude=build phase0-kext` needs no staging directory at all.

### The VMX repoint is a power-off operation and the tool already knows

`--dry-run` against the live VMX **refused**, exit 1, naming four lock artifacts
(the UUID-named `.vmem` and `.vmem.lck`, `BaseSystem.vmdk.lck`,
`Sonoma.vmdk.lck`). That is the guard working. VMware holds the VMX in memory
while a VM runs and rewrites it from memory on suspend, so an edit made now is
silently discarded. The optical device is **`sata0:2`**, currently pointing at
`darwin.iso`.

Also: the CD is host→guest only. There is **no credential-free guest→host file
path**, so the build log comes back off the screen, through a shared folder, or
by supplying the password from the other operator card.

---

## 5. Gate state at the end of the session

| Gate | Result |
|---|---|
| `python tools/check-registers.py` | exit 0, **114/114 passed, 0 failed, 0 skipped** |
| `tests\build-msvc.bat` | exit 0, ctest **5/5 Passed**, 100% of 5 |
| `tests\check-kext-syntax.bat` | exit 0, **sections 1–11 ok**, negative controls firing (sections 8 and 9 are new — see §8 and §10 below) |
| `tests\run-python-tests.bat` | exit 0, **Ran 671 tests, OK** on the last run of this session (448 at session start; it read 498, 532, 536, 539, 568, 645, 648 and 671 as peers landed modules and as §8/§10 added their own negative controls) |

Suite counts read off the binaries in `tests/build/` after the gate run —
**747 total**: decode 64, falcon 169, riscv_ucode 116, gsp_boot 217,
gsp_stage **181**. Nothing the *lead* did this session touched the kext headers,
so 728 was the expected reading right up until 12:23, when `wise-otter-74`
landed `nvGspStagePolicyFitsRegisters()` in `NVGspStage.hpp` and moved
`test_gsp_stage` 162 -> 174. Re-measured off the binaries after that landed, and
again after §7 below took it 174 -> 181.

`tests/README.md`'s Python count was stale at 440. It was corrected to 498, then
532, then 536 inside one hour, and the gate read 539 on the next run — so the
number was **removed from that line entirely** rather than corrected a fourth
time. A count in prose next to the command that prints it is not documentation,
it is a second source of truth that loses every race. The gate output is the
authority, and the line now says so by omission.

The C++ per-suite counts in the table below are different and stay: they are
measured off named binaries, each suite prints its own count as its last line,
and `tests/test_check_count_last_line.py` gates that. A wrong number there is
caught; a wrong number in the aggregate Python line is not.

---

## 6. What is still open

- **`OPERATOR-ACTION-step7-build.md`** — one power cycle and one command. This
  is now the highest-value open item in the project, because it is the only one
  that can answer a question no host-side gate can.
- `OPERATOR-ACTION-guest-creds.md` / `t-396740` — the password. Unblocks
  `runScriptInGuest`, `captureScreen`, and the guest→host direction.
- Extending the prose-path gate beyond `CLAUDE.md` (`t-16588d`).

Two items that were open when this file was first written have since landed and
are struck rather than deleted, because the reason each was worth doing is the
reusable part:

- ~~Assert that `NV_GSP_FW_PHYS_LIMIT`'s high word satisfies
  `nvGspDmaPhysHiFits()`~~ — §7 below.
- ~~The check-count floor gate (`t-b29be8`)~~ — `tests/test_check_count_floor.py`
  exists and derives each floor as `MEASURED[name] // 2` rather than typing it,
  so a floor cannot disagree with its own provenance.

**STEP 7 is still required.** Nothing in this session compiled the kext. What
changed is that nothing but an operator now stands between the project and
finding out.

---

## 7. Comparing two constants is not testing the predicate

`test_gsp_stage` section 10 already asserted `NV_GSP_FW_PHYS_LIMIT <
NV_GSP_DMA_REPRESENTABLE_LIMIT`, with separating inputs proving the comparison
could come out false. That is a sound check and it is **not** the assertion
handoff9 §2 asked for.

It compares two integers. The DMA path does not consult either of them — it
calls `nvGspDmaPhysHiFits()`, and that predicate masks with
`NVGSP_DMA_PHYSHI_MASK`, which is assembled from NVIDIA's published
`DMATRFBASE1_BASE_MASK`. `NV_GSP_DMA_REPRESENTABLE_LIMIT` is assembled here,
from a bit count. **Two different assemblies, one of which nothing was driving.**
A mistake in either leaves the `<` green while the predicate admits or refuses a
different set of addresses than the limit claims.

That matters more than the usual coverage argument because of the failure
direction recorded in handoff9 §2: the BASE/BASE1 pair **truncates** above 17
bits rather than faulting. An address wrongly admitted is a silent overlapping
DMA, not an error anyone sees.

Seven checks were added, driving the predicate directly:

- the policy ceiling's high word is `0x1FF`, pinned as a **literal as well as
  derived** — deriving it from the same `NV_GSP_FW_ADDR_BITS` the predicate's
  margin came from would make the check agree with itself no matter which value
  is wrong;
- that high word packs (`nvGspDmaPhysHiFits(0x1FF)` is true) — if the driver's
  own maximum permitted address cannot be packed, every legal allocation at the
  top of the window programs the wrong `BASE1`;
- `0x1FFFF` accepted / `0x20000` refused — the boundary pair;
- `0x10000` accepted / `0xFFFFFFFF` refused — the far side, because with only one
  of a boundary pair present an off-by-one in the mask still reads correctly;
- and the tie-back: `REPRESENTABLE_LIMIT`'s own high word must be exactly the
  predicate's maximum (`fits(hi) && !fits(hi + 1)`). If the two assemblies ever
  disagree, this is the check that says so — the `<` comparison cannot tell you
  which one is wrong.

Note the shape, since it generalises past this file: **a green comparison between
two constants says nothing about the function that reads neither of them.** Ask
which expression the production path actually evaluates, and drive that.

`test_gsp_stage` 174 -> **181**, and `tests/test_check_count_floor.py`'s
`MEASURED` entry was re-read off the binary rather than adjusted by hand.

---

## 8. The load-bearing file nothing read: `phase0-kext/Info.plist`

Until this session, `Info.plist` was the last file in `phase0-kext/` with no
gate over its contents. Two references existed anywhere in `tests/` and neither
opened it: `tests/kextshim/README.md` mentions the personality is "unverified by
construction", and `tests/test_guest_build_kext.py` lists it as a payload member
by name. So the file that decides whether the bundle can be **loaded** was read
by the operator and by `kmutil`, and by nothing else.

That is a worse gap than it sounds, because of what the failures look like.
Every defect in that file shares one property: **none of them is a compile
error.** The kext builds, the bundle assembles, `make` exits 0, and the failure
surfaces on the target laptop — the single most expensive place in this project
for a defect to appear, since getting there costs a power cycle and physical
access. Specifically:

- `CFBundleExecutable` names the binary inside the bundle's `MacOS` directory. The Makefile
  builds `$(MACOS_DIR)/$(KEXT_NAME)`. Drift between those two strings makes
  `kmutil` refuse the bundle for a **missing executable** while naming a file the
  operator can plainly see is present, under a different name.
- `CFBundleIdentifier` is spelled **three** times: in the plist, again as the
  Makefile's `BUNDLE_ID` (which is what `make unload` targets), and a third time
  inside each personality (which tells IOKit which bundle owns it). Change one
  and `make unload` starts unloading nothing while still exiting 0.
- `IOClass` is resolved **by string** through the `OSMetaClass` registry. It is
  not a symbol the linker sees. A typo links clean and fails at match time on
  hardware, with nothing in the build log.
- An omitted `OSBundleLibraries` entry surfaces as an undefined symbol at load
  time.

Two pieces landed. A peer took the matching dictionary
(`tools/check-kext-personalities.py`, gate section 7): every `IOPCIMatch` names a
device `DEVICE-IDS.md` attributes to this die, every personality has an
`IOClass`, every `IOProviderClass` is `IOPCIDevice`. This session added the other
half — **`tools/check-kext-bundle.py`, gate section 8** — covering bundle
*identity*: the executable name, the three spellings of the identifier, the
package type, the version keys, and `OSBundleLibraries`.

### Every expectation is derived, because a second copy is the defect

The checker types **no** expected string. `KEXT_NAME` and `BUNDLE_ID` are parsed
out of the Makefile and `$(...)`-expanded; the legal `IOClass` names come from
the `OSDefineMetaClassAndStructors` calls in `phase0-kext/*.cpp`; the required
`OSBundleLibraries` entries are derived by matching API families against the
sources. Writing a literal here would have reproduced inside the gate the exact
defect — a second hand-kept copy of a string — that the gate exists to catch.

Three parsing decisions worth keeping:

- **`+=` deliberately does not parse as a definition.** The real Makefile appends
  to `CXXFLAGS` under an `ifdef`. A parser that treated append as assignment
  would let a later append win over the real `:=`. Pinned by
  `test_an_append_does_not_override_the_definition`.
- **An unresolvable `$(FOO)` is left unexpanded and fails loudly**, rather than
  collapsing to `""` the way make itself would. Expanding a typo to the empty
  string turns it into a comparison against a prefix that matches nothing, and
  the resulting message names neither file.
- **`OSDeclareDefaultStructors` and `OSDefineMetaClassAndStructors` are read as
  different things.** Declare alone registers nothing with the runtime, so an
  `IOClass` naming a declared-but-never-defined class is found by grep and still
  fails to instantiate. The checker says which of the two it found.

### The separating input the module turns on

`tests/test_check_kext_bundle.py` is 35 checks. The one that carries the design
is `test_removing_both_the_api_and_its_library_is_silent`: it deletes
`IOPCIDevice` from the sources **and** `com.apple.iokit.IOPCIFamily` from the
plist, then asserts the checker says **nothing**. A checker with the library list
hardcoded passes every other test in the module and fails only this one. Without
it, "derived from the sources" would be an unverified claim about the
implementation, and the actual benefit — that a newly-used API family cannot be
forgotten — would rest on nothing.

`SyntheticBaselineIsClean` runs first for the reason this repo has hit before: a
fixture that fails for an unrelated reason makes every negative control below it
green without any of them reaching the line it targets. And every assertion
checks the printed **verdict word**, not just the exit code — a checker that dies
before evaluating anything also exits non-zero, and "rejected the tree" and "never
reached its verdict" are not the same evidence.

### What this still does not prove

Nothing here contacts Apple's SDK. That the *versions* in `OSBundleLibraries`
exist, and that the KPI names are spelled the way a given macOS release spells
them, remains unproven until STEP 7. What is proven is **internal consistency**:
the plist, the Makefile and the C++ agree about what this kext is called and
what it uses. The `com.apple.kpi.*` entries are additionally checked against
*each other* — they are versioned with the kernel as a set, so a mixed set names
a kernel that never shipped — without typing a version number anywhere, which
would have been a fourth hand-kept copy.

---

## 9. A real defect in the kext: `complete()` with no `prepare` behind it

While reading `NV3050Probe.cpp` end to end — looking for driver work rather than
more gate work — the Phase 5 staging teardown turned out to be wrong.

`releaseGspFirmware()` was:

```cpp
if (fGspFwBuffer) {
    fGspFwBuffer->complete(kIODirectionOut);
    OSSafeReleaseNULL(fGspFwBuffer);
}
```

Unconditional. And `loadGspFirmware()` calls `releaseGspFirmware()` from **every**
failure path — including this one, at `NV3050Probe.cpp:1220`:

```cpp
if (fGspFwBuffer->prepare(kIODirectionOut) != kIOReturnSuccess) {
    NVLOG("GSP firmware: prepare() failed -- pages not wired, refusing to "
          "hand the GPU an address");
    releaseGspFirmware();      // <-- calls complete() on a descriptor
    return false;              //     that was never successfully prepared
}
```

So the one path whose entire reason for existing is "`prepare()` failed" answered
by calling `complete()`. `IOMemoryDescriptor` carries a **wire count**, and
`prepare`/`complete` are the increment and decrement. An unmatched `complete()`
takes that counter below its own floor.

### Why nothing here would have reported it

This is the same shape as the defects §7 and §8 are about, one layer down: the
damage lands somewhere that does not print. The count lives in kernel VM
bookkeeping, not in this driver's state — so:

- `NVLOG` says nothing, because the driver's own view is consistent. It allocated,
  it failed, it released, it returned false. Every line it logs is true.
- The clang gate cannot see it. `complete()` is a well-formed call with the right
  argument; there is no type error to find. Section 3 proves the file **parses**,
  and a balance property is not a parse property.
- The MSVC suites cannot see it. `IOBufferMemoryDescriptor` is IOKit, so it lives
  on the far side of the logic/IOKit split — `tests/*.cpp` compiles the
  dependency-free headers, and this is in the shell.
- The shim cannot see it. `tests/kextshim/IOKit/IOBufferMemoryDescriptor.h`
  declares `prepare()` and `complete()` and nothing else; a stub has no wire
  count to unbalance.

Four gates, and the defect sits in the gap between all of them — reachable only
on a real kernel, on the one path taken when memory is already under pressure.

### The fix, and why the flag is set where it is

A `bool fGspFwPrepared` member, false in `init()`, set `true` **immediately** after
the `prepare()` success check and before anything that can bail, and checked in
`releaseGspFirmware()`:

```cpp
if (fGspFwBuffer) {
    if (fGspFwPrepared) {
        fGspFwPrepared = false;
        fGspFwBuffer->complete(kIODirectionOut);
    }
    OSSafeReleaseNULL(fGspFwBuffer);
}
```

Two placement decisions matter, and they fail in opposite directions:

- **Cleared before the `complete()`, not after.** `releaseGspFirmware()` is
  documented as idempotent and really is called more than once — `stop()` on the
  normal path, `free()` as belt-and-braces, and the failure paths. Clearing after
  would leave a window where a re-entrant call completes the same preparation
  twice, which is the bug this fix exists to remove.
- **Set immediately after the success check.** Every failure path *below* that
  point calls `releaseGspFirmware()`. A missed assignment would not
  double-complete — it would **leak the wired pages**, since the guard would
  suppress the one legitimate `complete()`. Both directions are wrong, so the
  flag has to track `prepare()` exactly rather than approximately. The comment at
  the assignment says so, because the next person to add a bail-out between the
  `prepare()` and the assignment would introduce the leak silently.

### Still unproven

This is reasoning about IOKit semantics, not a test result. Nothing on Windows can
exercise a wire count. What the gates confirm is that the change parses and
breaks nothing: clang exit 0, ctest 5/5, registers 114/114, python `Ran 645 OK`
at the time of the fix. **§10 below closed the half of this that a gate CAN
reach** — the shape of the code, as opposed to the count itself. The `build-payload/kext-src.tar` was rebuilt with
`tools/build-step7-payload.py` in the same pass, because that tar is compared
**byte-for-byte** against the live tree and any `phase0-kext/` edit reddens the
Python gate until it is regenerated.

### The rest of the shell was then audited the same way, and balances

One unbalanced pair is a reason to read every other pair, not just the one that
bit. Every acquire/release in `NV3050Probe.cpp` was walked against the failure
path that fires when the ACQUIRE ITSELF fails — the path a shared teardown helper
gets wrong. Result: no further defects.

| Pair | Acquire | Released on failure | Released on teardown |
|---|---|---|---|
| `fPCIDevice` | explicit `retain()` at `:147`, after the `OSDynamicCast` succeeds | `fail:` label at `:246` | `stop()` |
| `fBar0Map` | `bar->map(kIOMapInhibitCache)` at `:289` | `:298`, on the null-virtual-address path | `unmapBar0()` |
| `fGspFwDma` | `IODMACommand::withSpecification` | `releaseGspFirmware()` | same |
| `fGspFwBuffer` wire count | `prepare()` | now guarded by `fGspFwPrepared` | same |

Three details are right for the same reason the flag fix is:

- `fPCIDevice->retain()` sits **after** the `OSDynamicCast` null check, so the
  `fail:` path can only reach a `release()` that has a `retain()` behind it. The
  cast returning null jumps to `fail:` with the member still `nullptr`, and
  `OSSafeReleaseNULL` on `nullptr` is a no-op.
- `bar` itself is never released. `getDeviceMemoryWithIndex()` is a
  **non-retaining** accessor; the `+1` in that function belongs to `map()`'s
  return value, which is what `fBar0Map` holds.
- `unmapBar0()` nulls `fBar0Base` **before** releasing `fBar0Map`, because the
  pointer is derived from the mapping. Releasing first would leave a member
  pointing into a torn-down mapping for the duration of one statement — the same
  clear-before-release discipline the `fGspFwPrepared` guard uses.

So the defect in §9 was a genuine single miss rather than a pattern, and the
remaining question about this file's IOKit half is still STEP 7: whether it
compiles against the real headers at all.

## 10. Pinning the shape of the §9 fix: `tools/check-prepare-balance.py`

§9 fixed a real defect and left it resting on nothing. The reasoning was sound
and every gate was green *before* the fix too — which is the whole problem. A
future edit could reintroduce it and all four gates would stay green again.

So this session added the gate. It cannot assert the balance itself: nothing
available on Windows can count a wire count, and that limitation is inherent
rather than a gap to be closed later. What it *can* assert is the **shape**, and
the shape is where the defect actually lived.

### What it checks, and why each one

Wired into `tests/check-kext-syntax.bat` as **section 9**; the two negative-control
sections renumbered 9/10 → 10/11. Sixteen checks against the real tree, across
three pair kinds — see §10b for why it is a table of pairs and not one hardcoded
method name:

| Check | The failure it prevents |
|---|---|
| Flag initialised `false` in `init()` | `free()` can run on an instance whose `start()` never staged anything; an uninitialised member read as `true` completes a buffer that was never prepared |
| Flag declared `bool` in a header | A local resets to garbage and never survives from `loadGspFirmware()` to the teardown that reads it |
| No `return` between the `prepare()` success check and `flag = true` | The **leak** direction: a bail-out in that window runs a teardown that skips `complete()` and strands the wiring |
| Every `complete()` inside an `if (flag)` | The §9 defect itself, verbatim |
| Flag cleared **before** the `complete()` | The **double-complete** direction: clearing after leaves a window where a re-entrant teardown completes a second time |
| Object released **after** the `complete()` | Otherwise the descriptor is completed after its reference was dropped, or leaks |
| `complete()` with no `prepare()` anywhere | A decrement with no increment in the whole translation unit |
| `prepare()` with no `complete()` anywhere | The same error inverted — wired pages never released |

The two middle rows are the point. Being *approximate* about where the
assignments sit fails in **opposite directions**, so one check cannot stand in
for the other.

### Nothing is typed

The flag's name does not appear in the checker. It is derived: find the
`prepare()` failure check, and the boolean set `true` immediately after it is the
flag by definition. A second hand-kept copy of a driver identifier is the defect
class this project keeps rediscovering — a rename in `NV3050Probe.cpp` now
follows by itself instead of silently disarming the gate.

### It refuses rather than passing vacuously

Two paths print FAILED instead of OK: a `prepare()` that is not inside a
delimitable `if (...) { }` (no anchor from which to locate the flag), and a
source set containing no `prepare()`/`complete()` pair at all. The second is a
legitimate state — but from the outside it is indistinguishable from a regex that
has stopped matching, and a checker that cannot find its subject and reports OK
converts an unexamined file into evidence.

### The controls

`tests/test_check_prepare_balance.py`, 30 checks, all green. One mutation per
assertion in the checker, the unmutated synthetic tree asserted clean **first**
(otherwise every mutation could be red for an unrelated malformation of the
fixture), and each mutation required to be rejected **by name** — a run that goes
red for the wrong reason cannot masquerade as coverage. Every mutation also
asserts it actually applied, since an anchor that no longer matches would leave
the test passing against an unmutated tree.

One control runs the other way: a commented-out unguarded `complete()`, in both
`//` and `/* */` form, must still be **accepted**. That is the silent-false-pass
direction of comment handling, and the one that would train the operator to
ignore the gate.

### Gate state after this section

All four re-run from a clean tree: registers **114/114**; ctest **5/5**;
`check-kext-syntax.bat` exit 0, **sections 1–11 ok**; `run-python-tests.bat`
exit 0, **Ran 678 tests, OK** (671 when this section first landed; the rise is
§10b's controls plus peers' work).

Still unproven, and unprovable here: whether the real `IOMemoryDescriptor` does
what this code assumes with a directional `complete()`. Section 9 asserts the
shape, not the runtime. That settles on hardware, at STEP 7 and beyond.

## 10b. The same defect, four lines below, and the gate could not see it

The gate in §10 went green and I went looking for peer work. That is the moment
this section is about.

`loadGspFirmware()` has **two** acquires, not one. Four lines below the
`prepare()` that §9 fixed:

```cpp
if (fGspFwDma->setMemoryDescriptor(fGspFwBuffer, true) != kIOReturnSuccess) {
    NVLOG("GSP firmware: setMemoryDescriptor() failed");
    releaseGspFirmware();       // <-- the same shared teardown
    return false;
}
```

and in that teardown:

```cpp
if (fGspFwDma) {
    fGspFwDma->clearMemoryDescriptor(true);   // <-- reached on the path above
    OSSafeReleaseNULL(fGspFwDma);
}
```

Same object, same teardown, same failure path, same defect. The §9 audit missed
it and so did the brand-new gate written *for that exact defect class*.

### Why the audit missed it: the wrong guard looks like a guard

§9's audit table asked "is the release guarded?" and this one **is** — by
`if (fGspFwDma)`. But that asks whether the **command object exists**, which is
true from the instant `withSpecification()` returns. It says nothing about
whether the object ever *took* a descriptor. On the failure path above, the
object exists and holds nothing, and the guard waves it straight through.

An unguarded release is visibly wrong. A release guarded on the wrong predicate
reads as defended code, which is why it survived a deliberate audit for this
very class. `tests/test_check_prepare_balance.py` now carries that as its own
control — `test_guarding_on_the_object_instead_of_the_flag_is_caught` — because
a module that only mutated the *unguarded* form would pass the real defect.

### Why the gate missed it: a gate written for one instance of a class

`check-prepare-balance.py` matched the strings `prepare` and `complete`. It
reported `OK -- 8 checks` over a file containing the defect it existed to
prevent, because the defect was spelled with different method names.

That is worse than a missing gate. It converts "this pair is fine" into "this
file is fine", and it does so with a green tick and a check count that looks
like diligence.

The fix is `ACQUIRE_RELEASE_PAIRS`, a table of
`(acquire, release, what-the-flag-asserts)`. Three entries now:
`prepare`/`complete`, `setMemoryDescriptor`/`clearMemoryDescriptor`,
`prepareForDMA`/`completeDMA`. Adding a pair is the entire cost of covering it.
The third field is printed in the failure text, because "add a guard" is not
actionable without saying what the guard must assert — and in this case the
answer (`if (theCommand)` is not it) is the whole lesson.

One pair is **deliberately absent** and says so in the source: `map`/`unmap`.
The release for an `IOMemoryMap` is its own `OSSafeReleaseNULL` — there is no
`->unmap()` to pair with — so an entry would report the correct, idiomatic
`unmapBar0()` as an unreleased acquire. A gate that cries wolf on correct code
is a gate someone switches off.

### The fix in the driver

`fGspFwDmaHasDescriptor`, mirroring `fGspFwPrepared` exactly: `false` in
`init()`, `true` immediately after the `setMemoryDescriptor()` success check
with nothing that can bail between the two, and in the teardown cleared
**before** the `clearMemoryDescriptor()` call so a re-entrant teardown is a
no-op.

The header comment states what this fix does **not** claim. Whether Apple's
`clearMemoryDescriptor()` actually misbehaves on a command that never took a
descriptor is **not knowable from this tree** — `tests/kextshim/IOKit/IODMACommand.h`
only declares it, and no header here carries an implementation. The flag is not
an assertion that the old code was harmful; it is a refusal to depend on an
answer this project cannot check. One bool makes the code correct without
needing to know, and that is cheaper than being right by luck.

### One more separating control

`prepare` must not prefix-match `prepareForDMA` — they are different pairs with
different releases, and a prefix match would report a fabricated "missing
`complete()`" on correct code. `PairsAreDistinguished` pins it. Note the
direction: this control guards against the gate producing a **false alarm**,
which is the failure mode that gets a gate ignored rather than the one that lets
a bug through. Both are worth a test.

### Gate state after this section

registers **114/114**; ctest **5/5**; `check-kext-syntax.bat` exit 0, **sections
1–11 ok**; `run-python-tests.bat` exit 0, **Ran 678 tests, OK**.
`check-prepare-balance.py` now reports **16 checks** over the real tree, across
**3 pair kinds**; its controls are **30 checks**. `build-step7-payload.py`
re-run after the `.cpp`/`.hpp` edits, freshness gate agreeing on all ten members.

One housekeeping fix went in alongside: the FAILED line said `%d of %d checks
failed`, and the vacuity refusal fires *before* any check runs — printing
`1 of 0 check(s) failed`, which reads as a broken counter rather than as a
refusal to examine. It now reads `N failure(s) across M check(s)`.

## 11. The syntax gate was only as strict as its fakes: `tests/kextshim/SIGNATURES.md`

Section 3 of `tests\check-kext-syntax.bat` parses `NV3050Probe.cpp` **against
`tests/kextshim/`**. That is not the same claim as "against IOKit", and the
difference is not academic: wherever a hand-written stub is *looser* than Apple's
real declaration, the gate does not give a wrong answer, it gives **no** answer.
The call compiles here and the error first appears under Apple clang — on the
operator's single power-cycled build attempt (`OPERATOR-ACTION-step7-build.md`),
which is the most expensive place in this project for a defect to surface.

So every symbol the shim declares — 58 `virtual`/`static` members across the
eight headers — was tabled against its published Apple signature in
`tests/kextshim/SIGNATURES.md`, with a three-word verdict vocabulary:

| Verdict | Meaning |
|---|---|
| `MATCHES` | Shim agrees with Apple |
| `TIGHTENED` | Shim was looser; corrected in this pass |
| `NARROWER` | Shim declares a strict subset — cannot hide a defect in code that compiles against it |

### The one live defect

`NV3050Probe.cpp` passed a bare `0` for `withSpecification()`'s `mappingOptions`.
Apple declares that parameter as an **unscoped enum**, `IODMACommand::MappingOptions`,
and C++ performs no implicit conversion from `int` to an unscoped enum — so the
literal `0` is a hard error under Apple clang. The shim had declared the
parameter `UInt32`, which accepts `0` happily. Fixed at `NV3050Probe.cpp:1268`:

```cpp
    fGspFwDma = IODMACommand::withSpecification(
        &IODMACommand::OutputHost64,
        NV_GSP_FW_ADDR_BITS,
        (UInt64)padded,
        IODMACommand::kMapped,      // not 0 -- see tests/kextshim/SIGNATURES.md
        (UInt64)padded,
        (UInt32)NVGSP_DMA_CHUNK_BYTES);
```

This is the same shape as every other defect this project has found: not a wrong
result from a check, but a check that could not fail.

### Two more vacuity holes in the same header, and two cosmetic ones

- `SegmentFunction` was `typedef IOReturn (*)(void)`. Apple's takes
  `(IODMACommand*, Segment64, void*, UInt32)` and returns `bool`. The stub
  therefore accepted **any nullary function at all** — handing
  `withSpecification()` an unrelated callback would have parsed clean.
- Argument 7 was `void*` where Apple takes `IOMapper*`, so any pointer
  whatsoever satisfied it.
- In `IOBufferMemoryDescriptor.h`: `capacity` was `mach_vm_address_t` where
  Apple says `mach_vm_size_t`, and `getCapacity()` returned `IOByteCount` where
  Apple returns `vm_size_t`. Both alias to `unsigned long long` on LP64, so
  neither was a live defect. They were corrected anyway, so the table can read
  `MATCHES` with no footnoted exception — a reader who learns to skim one
  exception skims the next one too.

### The controls, including the half that is easy to skip

Three defect macros were added to the section-11 loop, which now runs seven:
`NVNEG_DMA_MAPPING_INT` (the bare `0`, verbatim what the kext said),
`NVNEG_DMA_BAD_SEGFUNC` (an unrelated nullary callback),
`NVNEG_DMA_VOID_MAPPER` (an `IOPCIDevice*` where `IOMapper*` belongs). Each must
be **diagnosed**.

That alone would not be coverage. A shim tightened *past* what Apple accepts
rejects all three too, and "rejects everything" is indistinguishable from
"catches the defect" when only the negative half is tested. So
`negative-control.cpp` also carries an **unguarded** positive control,
`nvPosDmaCorrectCall()` — the exact call the kext makes — which section 10
(baseline must PASS) requires to compile clean. The pair is the assertion; either
half alone is not.

### What this audit cannot claim

**There is no macOS SDK on this host.** `reference/` vendors NVIDIA's driver, not
Apple's headers, and there is no `IOKit/` anywhere outside `tests/kextshim/`. The
real-signature column is therefore **transcribed from Apple's published headers,
not diffed against them** — `SIGNATURES.md` says so in its own header rather than
leaving a reader to assume otherwise, and carries a four-command
`xcrun`/`grep` recipe to re-verify the table on a host that has the SDK. **Run
that recipe before the Step 7 build, not after it** — after the build, it is
re-deriving an answer clang already gave.

Nothing gates `SIGNATURES.md` itself, and nothing can: it is a transcription of a
file this machine does not have. That is the same failure class one level up, and
the recipe is the only mitigation available here. `tests/kextshim/README.md` now
points at it under **"Is the shim itself right?"**, because no gate reads that
README either, and a reference document nothing links to is a document nobody
opens.

### Gate state

The measurements recorded in §10 — registers **114/114**, ctest **5/5**,
`check-kext-syntax.bat` exit 0 with **sections 1–11 ok**, `run-python-tests.bat`
**Ran 671 tests, OK** — were taken on a tree that already carried every change in
this section, including the `kMapped` fix and the three new defect macros. They
are not re-stated here from a separate run.

`build-payload/kext-src.tar` was verified byte-identical to the live
`phase0-kext/` on all ten members afterwards, carrying both this change and §9's.

This section itself was then checked the only way prose in this tree can be:
`test_documented_paths.py` alone (**24 tests, ok**) and the full Python gate
(**Ran 678 tests ... OK**, 2026-08-13). The handoff files are deliberately
*excluded* from the documented-paths scan, so that green does not vouch for the
paths written above — every one of them was resolved against the live tree by
hand while writing. The rise from §10's 671 to 678 is peers' work landing
alongside, not this section's.
