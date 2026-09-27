# handoff8 — the kext syntax gate got three headers it never had, and there is
# now a credential-free host→guest file path

Newest handoff. Read `handoff7.md` for the state this continues from, and
`CLAUDE.md` for the four verification gates. **Nothing here changes the
project's ceiling.** Phases 0–5 are still the honest limit, the kext has still
never been compiled with Apple clang, and it has still never been loaded on
hardware.

Everything below was run in this session. Where a claim is *not* verified, it
says so in the same sentence.

---

## 1. `check-kext-syntax.bat` section 1 was parsing two of five headers

This is the finding that matters most in this handoff, because the gate was
printing a clean pass while three headers went unparsed standalone.

Before this session, section 1 (`=== 1. headers standalone ===`) invoked clang
on exactly two files:

```
phase0-kext/NVDecode.hpp
phase0-kext/NVFalcon.hpp
```

`NVRiscvUcode.hpp`, `NVGspBoot.hpp` and `NVGspStage.hpp` were **absent**. They
were reached only transitively, through `NV3050Probe.cpp` in section 2. That is
a materially weaker claim: a construct in a header that no other translation
unit instantiates — an unused template branch, a function body only reached
under a different `#if` — is never diagnosed, and the gate still prints `ok`.

Note that handoff7 and the card I filed both understated this. The card said
`NVGspStage.hpp` was missing. Three were.

Section 3's `sources scanned : 7` line is what made this easy to misread. That
count comes from the structors pairing scan, which globs the whole kext
directory. It has never had anything to do with which files clang parses
standalone. Two different numbers, one screen.

### The patch

Six lines added after the `NVFalcon.hpp` check, matching the existing idiom
exactly (`"!CLANG!" %HDRFLAGS% -x c++ ...` then an `if errorlevel 1` pair).

It was applied as a **byte-level** edit rather than a text edit, and the
line-ending counts were measured before and after:

```
CRLF 93 -> 99, bare LF 0 -> 0, bytes 3712 -> 4168
```

That check is not ceremony. `tests/test_line_endings.py` is a gate because
`cmd.exe` seeks batch files by byte offset, so a single bare LF introduced here
would make the gate run its blocks out of order and report failures that
describe the harness rather than the kext. The edit also refused to apply unless
its anchor line matched exactly once, and refused to double-apply.

### Gate output after the patch

```
=== 1. headers standalone ===
  ok   NVDecode.hpp
  ok   NVFalcon.hpp
  ok   NVRiscvUcode.hpp
  ok   NVGspBoot.hpp
  ok   NVGspStage.hpp

=== 2. kext against the IOKit shim ===
  ok   NV3050Probe.cpp

=== 3. structors pairing ===
sources scanned : 7 (NV3050Probe.cpp, NV3050Probe.hpp, NVDecode.hpp,
                     NVFalcon.hpp, NVGspBoot.hpp, NVGspStage.hpp,
                     NVRiscvUcode.hpp)
structors pairs : 1 declared, 1 defined
  ok   NV3050Probe : IOService
```

All three newly-parsed headers passed on the first run. Nothing was broken and
then fixed — they were simply never checked.

---

## 2. `tools/deliver-via-cd.py` — the first credential-free host→guest path

`handoff3.md` records the blocker this works around: the Sonoma install is
finished and every `vmrun` guest operation is blocked on authentication.
`kenny` is settled as the account short name (from the `.vmem` scan); the
password is not, and a real user and a nonexistent one both return the identical
`Invalid user name or password` at exit 127 — so the error text is not a
username oracle and probing yields nothing.

A CD needs no credential. The virtual optical drive is emulated by the *host*
VMware process; the guest kernel mounts whatever is behind it. And
`Sonoma.vmx` already declares a real one at lines 134–137:

```
sata0:2.present        = "TRUE"
sata0:2.fileName       = "C:\Program Files (x86)\...\darwin.iso"
sata0:2.deviceType     = "cdrom-image"
sata0:2.startConnected = "TRUE"
```

So nothing has to be *added* — only repointed. That distinction is deliberate:
adding a SATA device shifts `pciSlotNumber` allocation, and this project has
already been bitten by miscounting VMX keys.

### What it does, and the four refusals that matter

| Refusal | Why it exists |
|---|---|
| `deviceType != "cdrom-image"` → refuse | `sata0:0` and `sata0:1` are both `disk` carrying real VMDKs. Repointing either detaches the installed system, and the symptom appears at next power-on, far from the tool that caused it. |
| VM appears running → refuse without `--force` | VMware may rewrite the VMX from memory on suspend, silently discarding the edit. |
| 8.3 name collision → refuse, naming both files | `NV3050Probe.hpp` and `NV3050ProbeShim.hpp` both mangle to `NV3050PR.HPP;1`. |
| VMX validated *before* the ISO is written | Otherwise a failed repoint leaves a stale image behind that looks delivered. |

The edit is **in place**, never appended. The VMX authored/runtime boundary is
unlabelled — authored content ends at `uuid.bios`, VMware's write-back begins at
`uuid.location` — so an appended key lands in VMware's region and is discarded.
A test asserts the rewritten `fileName` still sits above `uuid.bios`.

Key parsing splits on `=` and compares the stripped left side literally, with no
regex. An anchored regex previously found 2 of 9 `pciSlotNumber` keys in this
project because its character class excluded digits.

Joliet **and** Rock Ridge are both enabled, and a test asserts the guest would
see `NV3050Probe.kext.tar` rather than `NV3050PR.TAR`.

### What it does NOT claim

Writing the ISO and repointing the VMX is a **host-side** claim, and that is the
only claim the tool makes. `--verify` re-reads the image; it says nothing about
the guest. Whether the guest mounted the volume is visible only in the
framebuffer — and per the `read-the-artifact-not-the-verdict` finding, an OCR
substring test is not that evidence. Open the PNG.

### CORRECTION: it HAS now been run against the real VMX, and the running-VM
### guard did not fire

This section originally said the tool had never been run against the real
`Sonoma.vmx`. That is no longer true, and what the run exposed matters more than
the run itself.

Evidence read from disk, not from a report:

```
C:\Users\Kentl\macos-vm\sonoma\delivery.iso        73728 bytes   2026-08-10T22:14:28
C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx.bak      10466 bytes   2026-08-10T22:14:28
C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx          10466 bytes   2026-08-09T14:48:11
```

The ISO was written and the backup was taken, both at 22:14:28. `Sonoma.vmx`
still carries its 08-09 mtime and is byte-identical to the backup, and
`sata0:2.fileName` still reads `darwin.iso`. So the repoint did **not** land.
`--verify` on the image returns `vol ident: PAYLOAD` with one entry,
`README.md`, 9648 bytes — the ISO half of the run is sound.

The VM was powered on throughout. `vmrun list` reports:

```
Total running VMs: 1
C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx
```

So the tool wrote an ISO and a backup against a **live** VM before stopping. The
refusal in §"the four refusals that matter" — *VM appears running → refuse
without `--force`* — did not fire at the point the table implies it would.

**Why it missed.** The original `vm_appears_running()` looked for lock artifacts
by VMX *stem*: `Sonoma.vmx.lck`, `Sonoma.vmem`, `Sonoma.vmss`. This VM names
none of those. Its real artifacts are:

```
564d3ffb-d11c-edd5-265a-225898e79f2b.vmem
564d3ffb-d11c-edd5-265a-225898e79f2b.vmem.lck
BaseSystem.vmdk.lck
Sonoma.vmdk.lck
```

A UUID-named `.vmem` and two `.vmdk.lck` directories — not one of which shares
the VMX stem. The stem-relative reading returned an empty list, and an empty
list means "nothing found", which the caller treated as permission to proceed.
This is the same failure shape as the anchored-regex undercount: a matcher whose
shape excludes the real data reports zero and reads as clean.

The function now scans the directory rather than constructing stem-relative
paths, and calling it against the real VMX returns **4 artifacts**. Its own
self-test now includes the separating cases, including one asserting that the
old stem-relative reading would have found none of them — an assertion that
fails under the old code and passes under the new, rather than passing under
both.

**Still not claimed:** the guest never saw anything. `sata0:2` was never
repointed, so `delivery.iso` is a host-side file sitting beside the VM and
nothing more. `t-0696fc` remains open and its precondition — VM powered off — is
still unmet. Powering down an operator's running VM is an operator decision.

### 2026-08-11: the fix verified end-to-end, and the VMX put back

On 2026-08-11 the repoint *did* land against the live VMX before the guard was
fixed — `sata0:2.fileName` read
`C:\\Users\\Kentl\\macos-vm\\sonoma\\delivery.iso` at line 135 and `Sonoma.vmx`
carried a fresh mtime. That is a stronger version of the same defect than the
08-10 observation above: not just an ISO and a backup written beside a live VM,
but the live VMX itself rewritten. It was restored from
`Sonoma.vmx.bak-20260811-predeliver`, taken before the run:

```
e20b7667bea364e33c04cffb41976839 *Sonoma.vmx
e20b7667bea364e33c04cffb41976839 *Sonoma.vmx.bak-20260811-predeliver
135:sata0:2.fileName = "C:\\Program Files (x86)\\VMware\\VMware Workstation\\darwin.iso"
```

Both checksums match and `sata0:2` points back at `darwin.iso`, so the VMX is
byte-identical to its pre-run state. `delivery.iso` and the tool-written
`Sonoma.vmx.bak` were removed; `Sonoma.vmx.bak-20260811-predeliver` was kept.

The fixed guard was then re-run against the same live VMX with no `--force`:

```
error: the VM appears to be running or suspended:
         ...564d3ffb-d11c-edd5-265a-225898e79f2b.vmem
         ...564d3ffb-d11c-edd5-265a-225898e79f2b.vmem.lck
         ...BaseSystem.vmdk.lck
         ...Sonoma.vmdk.lck
       Power the VM off, or pass --force.
exit=1
```

It refuses, names all four artifacts, and writes nothing — no ISO, no backup, no
VMX edit. That is the end-to-end proof the earlier entry could not offer, since
it only reported the artifact count the function returns in isolation.

The premise is now pinned by a test as well as by the fix:
`UuidNamedArtifactTests.test_no_artifact_shares_the_vmx_stem` asserts that none
of `Sonoma.vmx.lck` / `Sonoma.vmem` / `Sonoma.vmss` exists in the fixture, so if
someone reintroduces the stem-relative construction the class fails rather than
quietly passing. `VMX_LOCK_SUFFIXES` is retained *only* so that assertion can
name the old shape; `vm_appears_running()` reads `VM_DIR_LOCK_SUFFIXES` and
scans the directory.

### Coverage

Re-measured 2026-08-11 after the `verify_repoint()` write-path fix landed. This
section has now been stale twice — it carried 46/41 (pre-guard-fix) and then
53/47/307 (pre-write-path-fix). Both were accurate when written and wrong within
the hour. See the `prose-carrying-a-derived-value-goes-stale-silently` finding;
nothing checks a number written in prose, so re-measure before trusting these.

- `python tools/deliver-via-cd.py --self-test` → **53 checks, 0 failures**
  (42 → 46 → 53: four checks for the UUID-named artifact cases plus the
  assertion that the *old* stem-relative reading would have found none of them,
  then seven for `verify_repoint()`)
- `tests/test_deliver_via_cd.py` → **55 tests, OK** (41 → 47 → 55: six
  `UuidNamedArtifactTests`, then eight `VerifyRepointTests`)
- `tests\run-python-tests.bat` → **371 tests, OK**, exit 0. Only 55 of those
  are this section's; the rest are peers' suites landing during the same
  session. Do not read a gate total as a measure of any one change — see the
  gate-status section below for the full attribution.

The eight `VerifyRepointTests` each have a negative twin on purpose. A stub
`verify_repoint()` that returns `True` unconditionally would satisfy every
happy-path assertion, so the load-bearing property under test is that it
*separates*: an un-rewritten VMX must report `False` **and** report the
`darwin.iso` it actually found, a device with no `fileName` must report `None`
rather than `""`, and a genuinely different ISO must not be collapsed into a
match by path normalisation.

One caution on measuring these: running the self-test and the unittest discovery
*concurrently* produced a spurious `FAILED (errors=1)` with no error block in the
output, because both build ISOs under `%TEMP%`. Run them serially. Each was then
green on its own, three times.

The ISO-writing tests `skipUnless(HAVE_PYCDLIB)`, because
`run-python-tests.bat` is the one gate that must work on a bare checkout.
`pycdlib` is present here (it exposes no `__version__`; check with `pip show`).

### `run-python-tests.bat` now retains a log on failure

Added 2026-08-11, and it is the direct consequence of the caution above. A run
reported `Ran 307 tests` + `FAILED (errors=1)`; four immediate re-runs passed;
the traceback was gone. The gate streamed straight to the console and kept no
copy, so the one run that could have identified the fault left **no artifact at
all** — and a transient failure you cannot read is indistinguishable from a
transient failure you imagined. The temptation at that point is to file it as
flakiness, which is a verdict, not evidence.

The gate now writes to `%TEMP%\macos-gpu-python-gate-%RANDOM%%RANDOM%.log`,
`type`s it to the console as before, and on a nonzero exit prints `[FAIL]` plus
the retained log path. It deletes the log **only** on success — a passing run's
log has no value, a failing run's log is the entire point of writing one.

The random suffix is deliberate rather than tidy. Several terminals run this
gate concurrently in this project, so a fixed log name would let a passing run
overwrite the failing run's traceback — reintroducing the exact loss the change
exists to prevent.

**Ruled out as the cause of that failure: a fixed-path collision in `tests/`.**
`tests/test_deliver_via_cd.py:189` (`RepointVmxTests.ISO`) and `:234` look like
absolute host paths, but both are in-memory string fixtures — 189 is fed to
`repoint_vmx(SAMPLE_VMX, ...)`, which returns text and touches no filesystem.
Every real temp directory under `tests/` uses `tempfile.mkdtemp()` with a
distinct prefix (`deliver-cd-test-`, `-uuid-`, `-verify-`, and
`test_build_installassistant_iso.py`'s own), so concurrent terminals cannot
collide there.

**CORRECTION, same day.** I wrote that the cause "remains unidentified". It was
already identified, in a durable store I did not search first. A prior session
recorded that `tools/deliver-via-cd.py --self-test` and the unittest discovery
run by `run-python-tests.bat` **both build ISO images under `%TEMP%`**, and that
running them concurrently produces exactly this signature: the *correct* test
count, `errors=1`, and **no error block anywhere in the output**. The missing
traceback is the tell — a genuine test error prints one.

That also explains why `mkdtemp`'s uniqueness does not protect against it. The
collision is between two separate *invocations*, not two test cases sharing a
hard-coded path, so per-call unique prefixes are the wrong place to look. My
ruling-out of the fixed-path theory above is correct and still worth keeping;
it just was not the whole answer.

So: **do not overlap the two.** With several `crew` terminals on the board that
means checking `crew who` before running a gate. If you see the signature, re-run
serially before filing anything. The log-retention change below is still worth
having — it is what makes the *next* unexplained red diagnosable — but it is no
longer the only thing standing between you and this particular one.

Verified in **both** directions, because a green run exercises only the
delete-on-success path and proves nothing about the branch that was added:

- pass path — `315 tests, OK`, exit 0, no log left behind in `%TEMP%` (315 was
  the total at the moment this branch was exercised; it is 356 now, and every
  bit of that difference is peers' suites landing, not anything this change
  did)
- failure path — exercised against a byte-identical copy of the script in a
  scratch tree containing one deliberately raising test and one passing test:
  exit 1, `[FAIL]` printed, log path named on stdout, log present on disk, and
  the log contained the sentinel traceback, the `FAILED` line, **and** the
  passing test's output
- `51` CRLF, `0` bare LF; `test_line_endings` 9 tests OK

---

## 3. `python -m unittest tests.MODULE` is broken here — and CLAUDE.md was
## already right about it, for a different reason

**Correction to my own first draft of this section.** I wrote that CLAUDE.md's
documented single-module recipe does not work. That was wrong, and I had not read
the lines I was contradicting. CLAUDE.md §"To run a single Python test module"
(lines 58–67) already documents the *working* discovery form and already warns
against the package form by name:

```sh
python -m unittest discover -s tests -p test_line_endings.py -v
```

> Not `python -m unittest tests.test_line_endings` — `tests/` has no
> `__init__.py`, so that form fails … which reads as a broken test rather than a
> broken invocation.

So the recipe needs no change. What *is* incomplete is the **cause** it gives.
CLAUDE.md attributes the failure to the missing `__init__.py` alone. On this
machine there is a second, stronger cause, and it changes the error message:

```
ModuleNotFoundError: No module named 'tests.test_deliver_via_cd'
```

That reads as "my new file is broken." It is not. The *pre-existing*
`tests.test_line_endings` fails identically, which is what proves it:

```
tests package: C:\Users\Kentl\AppData\Roaming\Python\Python314\
               site-packages\tests\__init__.py
```

An installed site-packages package named `tests` shadows the project's `tests/`
directory, which has no `__init__.py`. This is **environment shadowing, not a
project bug** — editing project files will not fix it.

Working form, which resolves by path:

```sh
python -m unittest discover -s tests -p "test_deliver_via_cd.py" -v
```

Do **not** add `-t .` — that reintroduces the package import and breaks
discovery too. `tests\run-python-tests.bat` already uses discovery, so the gate
itself was never affected.

The practical difference between the two causes: with only a missing
`__init__.py`, `tests` would not resolve at all. Here it resolves — to the wrong
directory. That is why the error names *your module* (`No module named
'tests.test_deliver_via_cd'`) rather than the `tests` package itself, and why it
reads so convincingly as a broken new file.

---

## 4. Gate status as of this handoff

All four run from the project root, this session, after the section 1 patch:

```
python tools/check-registers.py     113/113 passed, 0 failed, 0 skipped
tests\build-msvc.bat                EXIT=0, ctest 5/5 passed
tests\check-kext-syntax.bat         EXIT=0, 1/1 passed, all sections ok
tests\run-python-tests.bat          EXIT=0, Ran 371 tests, OK
```

The Python gate moved **260 → 301** when `test_deliver_via_cd.py` landed (41
tests), **301 → 307** when the running-VM guard was fixed, **307 → 315** when
`verify_repoint()` and its eight tests landed, **315 → 327** when
keen-lynx-34's `test_check_count_last_line.py` landed at 08-11 11:06 (12 tests,
unrelated to `deliver-via-cd.py`), **327 → 342** when `test_cmake_registration.py`
landed at 11:16 (15 tests), **342 → 348** when I added six `NegativeControlTests`
to `test_check_count_last_line.py` at 11:25, and **348 → 356** when
keen-moth-e1's `test_check_header_coverage.py` landed at 11:29 (8 tests).
**356** is the figure re-run last; if you read 301, 307, 315, 327, 342 or 348
anywhere in this document it is a snapshot from earlier in the same session, not
a disagreement.

That this paragraph has now been rewritten six times in one session is itself
the point. A total is a *shared* number in a crew this size — a peer landing a
suite changes it without touching anything you wrote — so a bare count in prose
is stale the moment someone else commits. Per-module counts (`55 tests` for
`test_deliver_via_cd.py`) are attributable and survive; the gate total does not.
Re-measure it, never quote it from here.

### ctest "Passed" is not the claim — the binaries' own counts are

A suite that exits 0 with zero assertions reports PASS forever once it is wired
into ctest. So each binary was run directly:

| Suite | Its own summary |
|---|---|
| `test_decode` | `39/39 passed, 0 failed` |
| `test_falcon` | `169/169 passed, 0 failed` |
| `test_riscv_ucode` | `85 checks, 0 failures` |
| `test_gsp_boot` | `199 checks, 0 failures` |
| `test_gsp_stage` | `142 checks, 0 failures` |

Total **634**. `test_falcon` read `165/165` when this handoff was written and
reads `169/169` as of 2026-08-11 (`t-cb7f19`: four separating inputs for a
widened single-bit mask — see §9). The two tables further down this file are
records of what a specific card's verification run printed and still say 165
deliberately; **this** table is the one that claims current state, so it is the
one kept live. If they disagree, run the binary.

No zero-assertion suites. Note the two formats: `test_decode` and `test_falcon`
print `N/N passed`, the other three print `N checks, N failures`. Both forms are
machine-readable and, **as of card `t-5f567f`, both are the last line of output**
— see §8. When this handoff was first written they were not: `test_decode` and
`test_falcon` printed their caveat *after* the summary, so `tail -1` on those two
returned a sentence. That is fixed; the wording divergence between the two
formats remains and is deliberate.

The register banner still reads four headers (`NVRiscvUcode.hpp` contributes no
`#define`s, so it is not parsed by `check-registers.py`; it *is* now parsed by
the syntax gate, which is a different check):

```
kext headers  : NVDecode.hpp    (53 defines, 1 macros)
                NVFalcon.hpp    (177 defines, 10 macros)
                NVGspBoot.hpp   (7 defines, 0 macros)
                NVGspStage.hpp  (3 defines, 0 macros)
```

---

## 5. Things that will bite the next agent

Everything in handoff7 §"Things that will bite the next agent" still applies.
Four new ones:

- **A gate's own summary line can describe a different scan than the one you
  care about.** `sources scanned : 7` is the structors glob. It never meant
  seven files were parsed standalone; two were. Read which command produced a
  number before treating it as coverage.

- **`python -m unittest tests.X` fails for *every* module here**, old or new, and
  the error names your module rather than the shadowing package. Test the recipe
  against a file you did not write before concluding your file is broken. See §3.

- **`tail -1` on a test binary USED TO return prose.** `test_decode` and
  `test_falcon` printed their explanatory text *after* the summary. Fixed by
  card `t-5f567f` — all five suites now end on their count (§8). The general
  lesson still stands: prove a count by reading the count, not by trusting
  whatever the last line happens to be.

- **`ninja: no work to do` appeared in this session's `build-msvc.bat` run.**
  That is correct here — no `.cpp` under `tests/` changed — but it is not
  evidence the binaries match current sources. Prove freshness by mtime or by
  running the binary, which is what the table in §4 does.

---

## 6. What is still open

- **`tools/deliver-via-cd.py` has been run against the real `Sonoma.vmx` once,
  and it did NOT complete.** Card `t-0696fc` is still open. The ISO
  (`delivery.iso`, 73728 bytes) and the backup (`Sonoma.vmx.bak`) were both
  written at 22:14:28; `Sonoma.vmx` is byte-identical to that backup and
  `sata0:2.fileName` still reads `darwin.iso`, so the repoint never landed. The
  VM was powered on throughout (`vmrun list` → `Total running VMs: 1`) and the
  stem-relative running-VM guard failed to refuse it — see the correction in
  §2. The guard is fixed and now returns 4 artifacts for this VM, but the
  precondition is unchanged: the operator must power the VM off. Confirm any
  eventual mount by opening the framebuffer PNG, not by OCR.
- **The kext has never been compiled with Apple clang** and has never been
  loaded. The syntax gate now parses all five headers plus the shell, but that
  is still a syntax claim — not codegen, not link.
- **`nvGspDmaTrfBase1()`'s overlap is no longer open — it was a BUG, and it is
  fixed.** See §7. What remains open is the *window*, not the arithmetic:
  `NV_GSP_FW_ADDR_BITS` is still 41 under a reading in which the register pair
  can express more, and widening it needs hardware evidence.
- **The guest-credential track is unchanged.** `t-396740` still needs an
  operator to supply the password; `deliver-via-cd.py` does *not* unblock it,
  because a CD is one-way and `runScriptInGuest` still authenticates.
- ~~**CLAUDE.md §"To run a single Python test module" gives the right command
  with an incomplete cause.**~~ **CLOSED.** CLAUDE.md now documents both causes:
  lines 67–70 give the missing-`__init__.py` reason and the discovery recipe,
  and lines 72–79 add the stronger machine-specific one — `site-packages/tests/
  __init__.py` exists, so `tests` *resolves* to the wrong directory and the
  error names your module rather than the shadowing package. It also warns
  against adding `-t .`. Verified by reading those lines, 2026-08-10.

---

## 7. `nvGspDmaTrfBase1()` was wrong above 4 GiB, and is now fixed

Card `t-53137f`. This is the first *arithmetic* bug found in the Phase 4/5
headers since the two NVDecode ones, and it is the kind this project exists to
catch: it produced a plausible-looking register pair that would have made the
DMA engine read the wrong page **without faulting**.

### What was wrong

`nvGspDmaTrfBase1()` masked the **unshifted** high word:

```c
return physHi & NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK;   /* WRONG */
```

That reads `DMATRFBASE1` as physical bits **40:32** — which *overlaps* the
39:32 the shifted `DMATRFBASE` already carries. The corrected form shifts
first:

```c
return (physHi >> 8) & NV_PFALCON_FALCON_DMATRFBASE1_BASE_MASK;
```

`nvGspDmaTrfBase()` was already correct and was not touched.

### Why the corrected reading is the right one

Two independent pieces of NVIDIA's **own vendored material**, not an argument
from plausibility:

- `reference/.../ampere/ga102/dev_falcon_v4.h` defines
  `NV_PFALCON_FALCON_DMATRFBASE1_BASE` as **`8:0`** — a field of the
  *register*, making no claim at all about which physical bits land in it. The
  40:32 reading was never sourced from NVIDIA.
- `reference/.../ampere/kernel_gsp_falcon_ga102.c:122-123`:

  ```c
  DMATRFBASE  = NvU64_LO32(srcPhysAddr >> 8);
  DMATRFBASE1 = NvU64_HI32(srcPhysAddr >> 8) & 0x1FF;
  ```

  Both halves come from the **same shifted quantity**. So the pair is a plain
  non-overlapping 64-bit split of `(phys >> 8)`, and `BASE1` carries bits 40:32
  *of the shifted value* — physical **48:40**.

### Why the trace could not catch it, and why the suite did not either

The two forms are **identical for every address below 4 GiB**. The observed
trace base is `0xFFEEE200`, so `physHi` was 0 and both forms emitted the same
pair of writes. Divergence starts at exactly `0x1_0000_0000`:

| physical base | old `BASE1` | correct `BASE1` |
|---|---|---|
| `0xFFEEE200` | 0 | 0 |
| `0x1_0000_0000` | 1 | 0 |
| `0x1_2345_6700` | 1 | 0 |
| 2^40 | `0x100` | 1 |
| 2^41 − 256 | `0x1FF` | 1 |

The suite missed it because `fixtureImage()` pinned `physAddrHi = 0` — the
trace-witnessed value, and the correct default — so **every** DMA assertion in
`test_gsp_boot.cpp` ran in the one region where `BASE1` is zero.

The assertion that makes an overlapping split detectable is the one that
**reassembles the pair and compares it in 64 bits** (`checkU64`). Comparing the
halves separately is exactly what let the old form stay green. If you add
address arithmetic here, assert the reassembled value, not the fields.

### Gates after the fix

All three re-run against the corrected tree:

| gate | result |
|---|---|
| `python tools/check-registers.py` | **113/113 passed**, 0 failed, 0 skipped |
| `tests\check-kext-syntax.bat` | **PASS** — all 4 negative controls fired |
| `tests\build-msvc.bat` | **ctest 5/5 passed** |

Per-binary counts, taken by running each executable rather than trusting
`ninja: no work to do`:

| binary | assertions |
|---|---|
| `test_decode` | 39/39 passed |
| `test_falcon` | 165/165 passed |
| `test_riscv_ucode` | 85 checks, 0 failures |
| `test_gsp_boot` | 199 checks, 0 failures |
| `test_gsp_stage` | 142 checks, 0 failures |

*(Record of that run. `test_falcon` is 169/169 as of 2026-08-11 — `t-cb7f19`.
The §4 table is the live one.)*

`test_gsp_boot` now carries a **`DMA: physical base above 4 GiB (BASE1
nonzero)`** section pinning `BASE1 = 0x0` when `physHi` fits in `BASE`, `0x1`
at the top of the field, `0x1` for physical bit 40 alone, and `physHi` bit 8
surviving *only* via `BASE1`.

### Comment consistency, and what deliberately did NOT change

Three comments asserted the superseded 40:32 wording and all three now read
correctly: `NVFalcon.hpp:217`, `NV3050Probe.cpp:1194`, `NV3050Probe.hpp:77`.

`NV_GSP_FW_ADDR_BITS` was **left at 41 on purpose**, and `NVGspStage.hpp` now
carries a PROVENANCE block saying so. 41 was originally derived from the
*wrong* reading; under the corrected one the register pair may express more.
It was not widened because 41 is now a **policy ceiling**, and the two
directions are not symmetric: refusing an address the hardware would have
accepted costs an allocation retry, while admitting one it would truncate hands
the engine the wrong page silently. Widening it needs hardware evidence, not an
arithmetic argument.

Nothing here was witnessed on the wire. `NVGspStage.hpp` still contains **zero**
`*_OBSERVED_*` names, which is correct — all 13 occurrences in `phase0-kext/`
are in `NVGspBoot.hpp` (8) and `NVFalcon.hpp` (5).

---

## 8. Every C++ suite now ends on its count

Card `t-5f567f`. §4 above originally warned that `tail -1` on `test_decode` and
`test_falcon` returned *prose* rather than a count. That is no longer true, and
§4 has been amended to match.

### What was wrong

Both suites printed the summary and *then* an explanatory caveat:

```
39/39 passed, 0 failed

Decode logic is correct. This does NOT mean the kext
builds -- IOKit and the kernel build flags are still
unproven until Step 7.
```

So the last line of output was `unproven until Step 7.` — and on `test_falcon`,
`DMA plumbing, and the RPC layer are all still unwritten.` The other three
suites already ended on `N checks, N failures`. That divergence is precisely how
a suite that asserted nothing gets read as passing: a reader or a script
checking the final line sees a confident sentence.

### The fix

The caveat now prints **before** the summary in both `main()`s. The caveats
themselves are unchanged and still print — only their position moved. A comment
at both sites records why the order is load-bearing, because "tidying" the
summary back to the end of `main` is exactly how this regresses.

### Verified by running the binaries, not by reading the source

Each executable piped through `grep -v '^[[:space:]]*$' | tail -1`:

| binary | last line |
|---|---|
| `test_decode` | `39/39 passed, 0 failed` |
| `test_falcon` | `165/165 passed, 0 failed` |
| `test_riscv_ucode` | `85 checks, 0 failures` |
| `test_gsp_boot` | `199 checks, 0 failures` |
| `test_gsp_stage` | `142 checks, 0 failures` |

*(Record of that run. `test_falcon` is 169/169 as of 2026-08-11 — `t-cb7f19`.
The §4 table is the live one.)*

All five are counts. `build-msvc.bat` rebuilt `test_decode` and `test_falcon`
(visible in the ninja output, so this was not a stale-binary read) and ctest is
**5/5 passed**.

### What deliberately did NOT change

Two suites say `N/N passed, N failed` and three say `N checks, N failures`. Both
forms are machine-readable and both now terminate the output, which is what the
card asked for. Unifying the *wording* would churn five files for no
verification gain, so it was not done silently — if a future script wants one
regex, that is a separate decision.

### 2026-08-11: the convention is now a gate, not a comment

Card `t-b6ed95`. Everything above was true when written and is still true, but
nothing *enforced* it. The two comments added at the `main()` sites explain why
the order is load-bearing; a comment does not fail a build. The regression that
produced §8 in the first place would reappear silently.

`tests/test_check_count_last_line.py` — **12 tests** — pins it. It is a static
read of the five `.cpp` sources, so it needs CPython only and runs on a bare
checkout, the same constraint `run-python-tests.bat` is built around.

What it asserts:

- the count `printf` is the **last** `printf` in `main()` for all five suites
- the two specific 2026-08-10 caveats (`unproven until Step 7.`,
  `are all still unwritten.`) appear *before* the count, pinned by their own text
- `main()` contains no other output channel — `std::cout`, `std::cerr`,
  `std::clog`, `puts(`, `fputs(`, `std::fprintf` are all rejected, since a
  printf-only check is defeated by any one of them
- nothing but `return gFailures == 0 ? 0 : 1;` follows the count statement, so a
  helper cannot be called after the summary and print anything
- the five suites are named **explicitly**, not globbed, and
  `test_no_unlisted_suite_in_tests_dir` fails if a sixth appears or one is
  renamed. A glob would silently accept an unchecked suite, and would also pick
  up `tests/build/CMakeFiles/.../CMakeCXXCompilerId.cpp`, which has its own
  `main()`.

### Both count formats are asserted, not assumed

The card's premise was that there is one count format. There are two, and the
gate accepts both — pinning a single one would have failed two suites or three.
`BothFormatsStillInUseTests` asserts *which* suites use *which*, so if someone
unifies the wording that test fails and the paragraph above gets corrected
instead of going stale. That is the
`prose-carrying-a-derived-value-goes-stale-silently` failure mode applied to a
claim rather than a number.

### The negative control is the load-bearing part

`tests/_negctl_check_count.py`, hand-run, not part of the gate. It is
underscore-prefixed so `unittest discover -p "test_*.py"` does **not** pick it
up — verified by grepping the gate output for `negctl` and getting zero hits.
It copies the five suites to a temp directory, redirects the gate's
`_TESTS_DIR` at the copy, plants one defect at a time, and requires each to be
diagnosed while the unmodified baseline passes:

```
baseline -- unmodified sources must PASS
  ok   baseline passes

planted defects -- each must FAIL
  ok   prose printed after the count            diagnosed (2)
  ok   std::cout after the count                diagnosed (2)
  ok   a call after the count                   diagnosed (1)
  ok   the count line removed entirely          diagnosed (5)
  ok   a suite renamed out from under the list  diagnosed (12)

negative control: all 5 defects diagnosed, baseline clean
```

The first defect is the actual 2026-08-10 regression, replanted. A gate that
passes everything converts "never checked" into "believed checked", which is
worse than no gate — the same reasoning `check-kext-syntax.bat` gives for its
own negative-control pass.

### What this does NOT prove

It is a **static** check over the source. It pins the shape of `main()`; it does
not compile or run anything, so it cannot observe the actual final line of
output. `build-msvc.bat` plus running the `.exe` remains what confirms that.
The table earlier in this section is that evidence; this gate only stops the
source from drifting away from it.

### Gates after this landed

- `tests\run-python-tests.bat` → **327 tests, OK**, exit 0 (was 315; the +12 is
  exactly this module). It reads **356** now — `test_cmake_registration.py`
  (+15), six `NegativeControlTests` added to this module (+6), and
  `test_check_header_coverage.py` (+8) all landed after this line was written.
  The 327 is kept because it is what attributes the +12; the 356 is what a
  reader will measure.
- `test_check_count_last_line.py` alone → 12 tests when this section was
  written, **18** now (the six `NegativeControlTests` below)
- `test_line_endings.py` → 9 tests, OK (both new files are Python, which that
  gate excludes, but it was run rather than assumed)

---

## 9. Nothing checked that a C++ suite was registered with ctest

Card filed off `t-b6ed95`, done in the same pass. `tests/CMakeLists.txt`
registers each suite by hand, one `add_kext_test(name source)` call per suite,
and until now nothing verified that list was complete.

The failure it allows is quiet in exactly the way this project keeps getting
bitten by. A suite that is written, committed and never registered is never
built and never run -- and `ctest` reports `5/5 tests passed` while doing it,
because five is all it was ever told about. The number is true and describes the
harness, not the code. Compare section 1: `check-kext-syntax.bat` printed a clean
pass while three of five headers went unparsed, and its `sources scanned : 7`
line came from a different scan entirely.

### `tests/test_cmake_registration.py` -- 15 tests

Static read of `tests/CMakeLists.txt` plus a directory listing. No CMake, no
compiler, no ctest, so it runs on a bare checkout with CPython only.

- every `tests/*.cpp` appears in a registration, and every registration names a
  file that exists
- no source registered twice, no target name reused
- **the target name must be the source stem.** This is the subtle one.
  `add_kext_test(test_gsp_stage test_gsp_boot.cpp)` -- a copy-paste that updates
  the name but not the source -- has two individually valid halves, so only the
  *pairing* catches it. ctest then reports one **more** passing test than
  before while the new suite has never been compiled. A count going up is not
  evidence that coverage went up.
- a `#`-commented registration does not count as a registration
- `enable_testing()`, `add_test(`, `add_executable(` and `phase0-kext` on the
  include path are all asserted present -- the helper only counts if it actually
  wires each target into ctest, against the same headers the kext compiles

### The two static gates cross-check each other

`test_check_count_last_line.py` names the five suites explicitly rather than
globbing, on purpose. That list and the CMake registration list describe the
same set, so `CrossGateAgreementTests` asserts all three agree: the gate's
hardcoded list, the registration list, and the actual directory contents.
Adding a sixth suite must update both gates; updating only one fails here rather
than leaving one gate quietly checking four files and the other five.

The sibling gate is loaded **by path**, not by module name -- `tests/` has no
`__init__.py` and `site-packages/tests/` shadows the name, which produces an
error blaming the wrong file (section 3).

### Negative control: `tests/_negctl_cmake_registration.py`

Hand-run, underscore-prefixed so `unittest discover -p "test_*.py"` does not
pick it up -- verified by grepping the gate output for `negctl_cmake` and
getting zero hits. It copies `CMakeLists.txt`, the five suites **and the sibling
gate** into a temp directory, redirects `_TESTS_DIR`/`_CMAKELISTS` at the copy,
and plants one defect at a time:

```
baseline -- unmodified CMakeLists must PASS
  ok   baseline passes

planted defects -- each must FAIL
  ok   a suite registered nowhere                 diagnosed (2)
  ok   a registration commented out               diagnosed (2)
  ok   target name paired with the wrong source   diagnosed (4)
  ok   a registration naming a missing file       diagnosed (2)
  ok   the same source registered twice           diagnosed (3)
  ok   a sixth suite in neither list              diagnosed (2)
  ok   enable_testing() removed                   diagnosed (1)
  ok   phase0-kext dropped from the include path  diagnosed (1)

negative control: all 8 defects diagnosed, baseline clean
```

Copying the sibling gate into the temp directory is not incidental. Without it
`CrossGateAgreementTests` errors for a reason unrelated to the planted defect --
and an error still counts as "diagnosed", so several controls would have passed
for the wrong reason. That is the same shape as
`an-assertion-that-cannot-fail-is-not-coverage`.

### What this does NOT prove

It proves ctest has been **told about** every suite. It does not prove any suite
builds, links or passes -- `tests\build-msvc.bat` is what does that. A green run
here means the registration list is complete and self-consistent, nothing more.

### Gates after this landed

- `tests\run-python-tests.bat` -> **342 tests, OK**, exit 0 (was 327; the +15 is
  exactly this module). It reads **356** now; the further +14 is six
  `NegativeControlTests` added to `test_check_count_last_line.py` at 11:25 plus
  keen-moth-e1's eight-test `test_check_header_coverage.py` at 11:29 — neither
  of them this module.

  Both figures are kept on purpose. 342 was true when this section was written
  and is the number that attributes the +15 to this module; 356 is the number a
  reader will see if they run the gate today. Replacing the first with the
  second would destroy the attribution and leave a total that goes stale again
  the next time a peer lands a suite.
- `test_cmake_registration.py` alone -> 15 tests, OK
- `_negctl_cmake_registration.py` -> exit 0, 8 of 8 defects diagnosed
- `test_line_endings.py` -> 9 tests, OK (both new files are Python, which that
  gate excludes, but it was run rather than assumed)

## 10. Section 1 of the kext gate audited nothing about its own completeness

Landed 2026-08-11. Card `t-8793c5`.

### What was wrong

Section 1 of `tests/check-kext-syntax.bat` is a hand-written list of clang
invocations, one line per header:

```bat
"!CLANG!" %HDRFLAGS% -x c++ phase0-kext/NVDecode.hpp
if errorlevel 1 (set FAILED=1 & echo   FAIL NVDecode.hpp) else (echo   ok   NVDecode.hpp)
```

A hand-written list has exactly one failure mode and it is silent. Add a header
to `phase0-kext/` and forget the line, and the gate does not print `5 of 6` --
it prints `ok` five times, because nothing in it knows a sixth header exists.
That is not hypothetical: handoff8 §1 records this gate parsing **two of five**
headers while reporting success.

The fix was not to maintain the list more carefully. It is to derive the
expected set from the filesystem and compare.

### `tools/check-header-coverage.py`

Wired in as the gate's new **section 2**; sections 2-5 renumbered to 3-6. The
script scans `phase0-kext/*.hpp`, extracts the headers named on `%HDRFLAGS%`
lines of the gate script, and requires the two sets to agree. Current output:

```
header coverage: 5/6 parsed standalone
  standalone  NVDecode.hpp
  standalone  NVFalcon.hpp
  standalone  NVGspBoot.hpp
  standalone  NVGspStage.hpp
  standalone  NVRiscvUcode.hpp
  via compile NV3050Probe.hpp (angle-bracket include; reached from NV3050Probe.cpp)
```

### The exemption is derived, not listed

`NV3050Probe.hpp` genuinely cannot be parsed standalone -- it includes
`<IOKit/...>`, which needs `-I tests/kextshim` and the full `%FLAGS%`, not
`%HDRFLAGS%`. Hardcoding the name `NV3050Probe.hpp` in the checker would have
moved the stale hand-kept list from the `.bat` into the `.py` and changed
nothing.

So the exemption is **computed**: a header is exempt from section 1 if and only
if it has an angle-bracket `#include`, i.e. it depends on something outside
`phase0-kext/`. That is the same property the architecture split in CLAUDE.md is
built on. If someone adds IOKit to `NVGspBoot.hpp`, the checker stops demanding
a standalone parse for it automatically, and section 3 becomes what must cover
it.

### An exempt header is not an unchecked header

Being skipped by section 1 is only legitimate if something else parses it. The
checker requires every exempt header to be reachable from a `.cpp` that section
3 compiles, following quoted `#include`s transitively.
`NV3050Probe.cpp` includes `"NV3050Probe.hpp"`, so the IOKit header is covered
with the shim on the include path.

A header that is neither parsed standalone nor pulled in by a compiled source is
reported as a failure -- `parsed by nothing` -- because a syntax error in it
would go unnoticed everywhere.

The reverse direction is checked too: a `%HDRFLAGS%` line naming a header that
no longer exists is reported as a `Stale line`, because a gate claiming coverage
it does not have is the same silence in the other direction.

### The negative control is the load-bearing part

A checker written to catch a silent failure, and never proven capable of failing
itself, has moved the silence up one level. Running it once against the real
gate and seeing exit 0 proves only that it does not crash -- a script whose body
was `return 0` passes that same test.

`tests/test_check_header_coverage.py` -- **8 tests**, all green. The checker
takes an optional gate-path argument for exactly this purpose, so the tests
point it at synthetic gate files in a `tempfile.mkdtemp()` scratch directory.
Nothing writes into `phase0-kext/`; the headers are read from the real tree
deliberately, since comparing a gate script against the headers that actually
exist is the function under test.

| Test | Planted defect |
|---|---|
| `test_the_checked_in_gate_is_currently_correct` | none -- if the real gate fails, every negative below is moot |
| `test_a_synthetic_correct_gate_also_passes` | none -- proves the fixtures are well-formed |
| `test_every_self_contained_header_is_individually_required` | each header's line dropped, one at a time |
| `test_an_empty_gate_fails_rather_than_reporting_zero_of_zero` | no clang lines at all |
| `test_a_line_naming_a_deleted_header_is_reported` | line for `NVDoesNotExist.hpp` |
| `test_a_dependent_header_parsed_standalone_is_rejected` | `NV3050Probe.hpp` on a `%HDRFLAGS%` line |
| `test_an_exempt_header_covered_by_nothing_is_rejected` | the `%FLAGS%` compile line removed |
| `test_a_missing_gate_file_is_a_clean_failure` | nonexistent path -- must not traceback |

Two of these are worth calling out. `test_a_synthetic_correct_gate_also_passes`
exists because without it a negative test could pass for the wrong reason --
every fixture rejected because the harness builds malformed gates, not because
the checker found the planted defect. And
`test_every_self_contained_header_is_individually_required` loops over all five
rather than spot-checking one, because a checker that hardcoded a single name
would pass a one-header test.

### What this does NOT prove

It does not prove the headers are *correct*, only that every one of them is
parsed by something. It does not prove the kext builds under Apple's toolchain
-- that is still unproven and still needs `-mkernel -fapple-kext` on macOS. And
the `via compile` reachability walk follows quoted includes only; a header
pulled in by an angle-bracket include from inside the shim would not be counted.

### Gates after this landed

All four run **serially** (per the `%TEMP%` collision caution in §2) and green:

- `python tools/check-registers.py` -> **113/113 passed, 0 failed, 0 skipped**, exit 0
- `tests\build-msvc.bat` -> **100% tests passed, 0 failed out of 5**, exit 0
- `tests\check-kext-syntax.bat` -> exit 0, all **6** sections ok
- `tests\run-python-tests.bat` -> **Ran 356 tests, OK**, exit 0

The Python count moved 315 -> 356 in this pass. Reconciled by per-module
discovery rather than assumed: 59 `test_vm_type` + 55 `test_deliver_via_cd` +
53 `test_parse_vbios` + 38 `test_vm_screenshot` + 35 `test_hfs_catalog_walk` +
31 `test_tools_parsing` + 23 `test_build_installassistant_iso` +
18 `test_check_count_last_line` + 15 `test_cmake_registration` +
12 `test_dmg2raw` + 9 `test_line_endings` + 8 `test_check_header_coverage`
= **356** exactly. Only 8 of the 41 new tests are mine; the rest arrived with
§8's and §9's suites landing in the same window.

`tests/check-kext-syntax.bat` remains **CRLF-only** after the edit (110 CRLF, 0
bare LF) -- verified by byte count, not by eye, because `test_line_endings.py`
is a gate and a bare-LF batch file runs its blocks out of order.
`tools/check-header-coverage.py` and `tests/test_check_header_coverage.py` are
LF, matching every other `tools/*.py` and `tests/*.py` in the tree.

---

## 11. `tests/test_control_bytes.py` -- the gap `test_line_endings.py` never covered

### The defect that produced it

This file carried two literal control bytes, written by a peer whose writer
interpreted the backslash sequences `\b` and `\r` as C escapes instead of
emitting a literal backslash:

```
offset 40196   0x08   `tests<BS>uild-msvc.bat`        renders as testsuild-msvc.bat
line   869     0x0D   `tests<CR>un-python-tests.bat`  renders as testsun-python-tests.bat
```

Both are repaired. The file is now byte-clean: 48720 bytes, 1027 lines, zero
disallowed control bytes, zero CR.

The repair itself is worth recording, because the obvious approach fails. A
search tool renders `tests<BS>uild` as the *characters* `testsuild`, so the
damage looks like an ordinary typo -- and an `Edit` against that displayed
string does not match, because the file holds one `0x08` byte, not the letters
the tool drew. Two edits were burned reading that non-match as "a peer changed
the file under me" before the raw bytes were printed. It was fixed at byte
level, asserting each anchor was unique first:

```python
bad  = b'`tests\x08uild-msvc.bat`'
good = b'`tests\x5cbuild-msvc.bat`'
assert data.count(bad) == 1, 'refusing: anchor is not unique'
```

This is the **mirror image** of the `test_falcon.cpp` scare in §8, where a search
tool rendered `//` comment lines as `\` and the file was in fact clean. Same
symptom, opposite cause. Only `open(path, 'rb')` separates them.

### Why an existing gate did not catch it

`tests/test_line_endings.py` is deliberately narrow: it covers `.cmd`/`.bat`
only, because those are the files `cmd.exe` seeks by byte offset, and the one
byte it cares about is a bare LF. Markdown is not in its `BATCH_SUFFIXES`, and
its bare-CR check applies to batch files alone. Neither corrupted byte was
visible to it, and neither should have been -- widening that gate would blur
which failure it is the authority on.

So the new module is a **sibling, not an extension**. `0x0D` is explicitly left
to `test_line_endings.py` (`test_a_lone_cr_is_left_to_the_line_endings_gate`
asserts this gate stays silent about it), so one defect never fails two gates.

### What it checks

Every repo-owned text file -- `.md .py .cmd .bat .cpp .hpp .txt .json .yml
.yaml .cmake` -- must contain no C0 byte other than TAB, LF and CR, plus DEL
(`0x7F`), which is equally invisible and arrives by the same route. Binary
formats are excluded because a `0x08` in a PNG or a VMDK is data, not damage;
flagging them would make the gate noisy enough to be switched off, which is
worse than not having it. `reference/` and `shots/` are skipped for the same
reasons `test_line_endings.py` skips them.

The failure message is written for the person who will hit this next: it prints
the offset, the byte, and 24 bytes of surrounding context, and it says outright
that an `Edit` failing on a string a search tool just displayed is evidence the
**bytes differ from the rendering**, not evidence the file changed underneath.

### Negative controls -- 12 of the 15 tests

Every assertion in the first class runs against the real repo, which is now
clean. Per §8's `an-assertion-that-cannot-fail-is-not-coverage` finding, that is
not coverage on its own: a scanner with an inverted comparison would produce a
green run indistinguishable from a correct one. So the scanner is driven against
synthetic files it must reject, and against ones it must not.

| Test | Planted input |
|---|---|
| `test_a_clean_file_reports_nothing` | anchors every mutant below |
| `test_the_exact_backspace_from_handoff8_is_detected` | the literal `0x08` from this file |
| `test_the_offset_and_context_are_reported_not_just_the_count` | asserts offset 3, context both sides |
| `test_a_lone_cr_is_left_to_the_line_endings_gate` | `0x0D` -- must NOT trip |
| `test_tab_and_newline_do_not_trip_it` | TAB -- must NOT trip |
| `test_a_nul_byte_is_detected` | `0x00` |
| `test_a_form_feed_is_detected` | `0x0C` |
| `test_an_escape_byte_is_detected` | `0x1B` ANSI colour codes pasted from terminal output |
| `test_a_del_byte_is_detected` | `0x7F` |
| `test_every_disallowed_c0_byte_is_detected_not_just_the_sampled_ones` | all of `0x00`-`0x1F` |
| `test_multiple_offenders_in_one_file_are_all_reported` | three, in order |
| `test_an_empty_file_reports_nothing` | empty |

The whole-range enumeration matters more than the four sampled bytes: a scanner
with an off-by-one bound passes every spot check and still misses `0x1F`. And
`test_tab_and_newline_do_not_trip_it` is the mutant in the opposite direction --
a scanner that rejected TAB would fail most of `phase0-kext/` and get deleted
rather than fixed.

The discovery walk is asserted too, by name. A suffix list that silently stopped
matching `.md` would leave a bare "found some files" assertion green -- there are
plenty of `.py` files -- while the file that actually got corrupted went
unscanned. That is the same shape as the stem-relative lock scan in §2 that
returned zero of four artifacts and was read as "proven absent".

### What this does NOT prove

Passing means no repo-owned text file carries a stray control byte. It does not
mean those files are *correct*, it does not check line endings, and it says
nothing about `reference/`, which is vendored and not ours to normalize.

### Gates after this landed

All four run **serially** (per the `%TEMP%` collision caution in §2) and green:

- `python tools/check-registers.py` -> **113/113 passed, 0 failed, 0 skipped**, exit 0
- `tests\build-msvc.bat` -> **100% tests passed, 0 failed out of 5**, exit 0
- `tests\check-kext-syntax.bat` -> **1/1 passed, 0 failed**, exit 0
- `tests\run-python-tests.bat` -> **Ran 371 tests, OK**, exit 0

356 -> 371, and the +15 is entirely this module. Reconciled by per-module
discovery rather than assumed, because peers were landing suites in the same
window: 59 `test_vm_type` + 55 `test_deliver_via_cd` + 53 `test_parse_vbios` +
38 `test_vm_screenshot` + 35 `test_hfs_catalog_walk` + 31 `test_tools_parsing` +
23 `test_build_installassistant_iso` + 18 `test_check_count_last_line` +
15 `test_cmake_registration` + **15 `test_control_bytes`** + 12 `test_dmg2raw` +
9 `test_line_endings` + 8 `test_check_header_coverage` = **371** exactly. No
peer suite moved in this pass.

The module needs nothing but CPython, so it works on a bare checkout -- which is
the point, since the corruption it detects arrives through writers rather than
through compilers.

## 12. Gate-invoked tools were not required to have tests, and `check-structors.py` did not

Landed 2026-08-11. Card `t-a49d87`.

### The same hole as §10, one level up

§10 made section 1 of the kext gate audit its own header list. This section does
the same for the gate's *tools*. The gates invoke Python checkers, and a checker
is an **evidence producer**: when one is wrong it does not crash, it prints a
plausible, confidently-formatted, wrong verdict and the gate goes green.

A gate-invoked checker with no tests is therefore worse than an ordinary
untested script. It is a green light nobody has proven can turn red.

That was not hypothetical. `tools/check-structors.py` had been running as a
section of `check-kext-syntax.bat` with **zero** test coverage, while three of
its four sibling tools had suites. The new checker found it on its first run:

```
FAIL: check-structors.py: invoked by check-kext-syntax.bat, but no tests/test_*.py
      mentions it. A gate-invoked checker with no tests is a green light nobody
      has proven can turn red.
```

### `tools/check-tool-coverage.py`

Wired in as the gate's new **section 4**; later sections renumbered, so the gate
now runs 1-7. The required set is **derived**: every `tools/<name>.py`
invocation found in any `tests/*.bat` gate script or in `CLAUDE.md`. Nothing
hardcodes a tool name, so wiring a new checker into a gate immediately makes
this check demand tests for it — including itself, which is the correct outcome
and not a joke at its own expense. An audit that exempts itself is exactly the
hand-maintained blind spot it was written to remove.

Current output: **5/5 gate-invoked tools have a test module**, with 23 tools
listed as not gate-invoked and therefore not required.

### The exemption is deliberate, and has its own control

The `_`-prefixed probes, one-shot explorers and operator utilities are **not**
required to have tests. Demanding suites for all 23 would make this check noise,
and noise gets suppressed rather than fixed. The exemption is what keeps the
signal credible, so `test_a_tool_no_gate_invokes_does_not_require_tests` exists
to prove it holds.

### A docstring is not a test — the false positive this check produced on itself

The first version matched the tool name anywhere in a test module's text. On its
first run against the real tree it reported `check-registers.py` as covered by
`test_check_tool_coverage.py` — purely because that file's own **docstrings**
name it while testing nothing of the sort.

That is not cosmetic. Under that reading, deleting `test_check_structors.py`
would have left this gate green on the strength of a sentence.

The binding is now a **code string literal**, extracted via `ast`: comments never
reach the parse tree at all, and module/class/function docstrings are removed
explicitly. What remains is the strings the code actually uses — the
`spec_from_file_location` and `subprocess` arguments by which every suite here
binds to its hyphenated subject, since those names cannot be imported.

A test module that will not parse is reported as a failure rather than skipped.
Skipping would be the worst option: the file sits in `tests/` looking like
coverage while contributing nothing, and it will not run under discovery either.

### `check-structors.py` gained an injectable path

Its defects — a missing metaclass registration, a superclass mismatch, a
duplicate declaration — cannot be planted in `phase0-kext/` without breaking the
tree for every other terminal. So it now takes an optional kext-directory
argument, exactly as `check-header-coverage.py` takes a gate path, and the tests
build `tempfile.mkdtemp()` fixture trees. Nothing writes into `phase0-kext/`.

### Two new suites, both negative-control-first

`tests/test_check_structors.py` — **10 tests**:

| Test | Planted defect |
|---|---|
| `test_the_real_kext_is_currently_correct` | none — if the real tree fails, every negative is moot |
| `test_a_synthetic_correct_tree_also_passes` | none — proves the fixtures are well-formed |
| `test_declared_but_never_defined` | metaclass registration removed |
| `test_defined_but_never_declared` | class-body declaration removed |
| `test_metaclass_names_a_different_superclass` | `IOUserClient` where the class derives from `IOService` |
| `test_declared_twice_in_two_files` | duplicate declaration |
| `test_defined_twice_in_two_files` | duplicate registration |
| `test_osdeclareabstractstructors_is_also_required_to_pair` | Abstract variant, unregistered |
| `test_a_missing_directory_is_a_clean_failure` | nonexistent path — must not traceback |
| `test_an_empty_directory_fails_rather_than_reporting_zero_of_zero` | empty tree |

The superclass-mismatch case earns its own entry because a checker that only
counted declarations against definitions would pass it — one of each. And the
Abstract case exists because an alternation branch no test exercises is an
assumption, not a feature.

`tests/test_check_tool_coverage.py` — **16 tests**, covering: an untested
gate-invoked tool; a non-`test_`-prefixed helper not counting as coverage; a gate
invoking a deleted tool; the deliberate non-gate exemption; backslash
invocations (`tools\x.py`, since gate scripts are cmd.exe); `CLAUDE.md` counting
as a gate source (`check-registers.py` is invoked from there and nowhere else);
comment-only and docstring-only prose *not* counting, with a paired positive so
those two cannot pass merely because the checker rejects everything; an
unparseable test module; and both vacuous-pass cases — no gate scripts at all,
and gate scripts matching no tool, which is what a broken regex looks like from
the outside.

### Gates after this landed

All four run **serially** (per the `%TEMP%` collision caution in §2) and green:

- `python tools/check-registers.py` -> **113/113 passed, 0 failed, 0 skipped**, exit 0
- `tests\build-msvc.bat` -> **100% tests passed, 0 failed out of 5**, exit 0
- `tests\check-kext-syntax.bat` -> exit 0, all **7** sections ok
- `tests\run-python-tests.bat` -> **Ran 397 tests, OK**, exit 0

The Python count moved 356 -> 397. Reconciled by per-module discovery rather
than assumed: 59 `test_vm_type` + 55 `test_deliver_via_cd` + 53 `test_parse_vbios`
+ 38 `test_vm_screenshot` + 35 `test_hfs_catalog_walk` + 31 `test_tools_parsing`
+ 23 `test_build_installassistant_iso` + 18 `test_check_count_last_line`
+ 16 `test_check_tool_coverage` + 15 `test_control_bytes`
+ 15 `test_cmake_registration` + 12 `test_dmg2raw` + 10 `test_check_structors`
+ 9 `test_line_endings` + 8 `test_check_header_coverage` = **397** exactly.

Of the 41 new tests, **26 are from this card** (10 + 16); the other 15 are
`test_control_bytes`, which landed from another terminal in the same window.
Attribution matters here because a bare "+41" would read as one agent's work.

`tests/check-kext-syntax.bat` remains **CRLF-only** (121 CRLF, 0 bare LF),
verified by byte count after every edit. The two new Python files are LF, like
every other `tools/*.py` and `tests/*.py`.

### What this does NOT prove

That a tool is *well* tested. A suite of one trivial assertion satisfies this
check. It proves only that a gate-invoked evidence producer is not completely
unexercised — the difference between "thinly covered" and "nothing anywhere
would notice if this returned the wrong answer". It also says nothing about the
23 non-gate tools, by design.
