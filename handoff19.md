# handoff19 — the Intel track gets an IOKit shell

Written 2026-08-17. Continues `handoff18.md`, which was NVIDIA-side only.

This session is the mirror image of that one. handoff18 touched no register
constant and moved the evidence layer; this one adds no decode logic at all and
moves the **integration** layer. The three Intel headers have existed since
handoff14 and have been gated, mutation-probed and register-checked ever since —
and until today nothing outside `tests/` called a single one of them. handoff16
§8's first open item was that the Intel track had no IOKit shell. It has one now.

All four gates are green at the end of this session, measured after the last
edit:

| Gate | Verdict |
|---|---|
| `python tools/check-registers.py` | 238/238 passed, 0 failed, 0 skipped |
| `tests\build-msvc.bat` | 8/8 suites passed |
| `tests\check-kext-syntax.bat` | `=== ALL SYNTAX GATES PASSED ===` |
| `tests\run-python-tests.bat` | Ran 885 tests, OK |

---

## 1. What was added

`phase0-kext/IntelDisplayProbe.cpp` and `.hpp` — the Gen12 display track's
`IOService` subclass. It is the second IOKit shell in the bundle, alongside
`NV3050Probe`.

What it does: match `8086:9A68`, enable memory decode, map BAR0, publish the
device identity, then walk all four transcoders and all four pipes' plane 1 and
publish a verdict per unit into the IORegistry. Every decode delegates to
`IntelDisplay.hpp`, `IntelFramebuffer.hpp` or `IntelGpuId.hpp`; there is not one
inline shift or mask in either file, which is what keeps the logic inside the
reach of the three host suites.

**It has no write path.** Not a refusing one, not one behind a build flag —
none. That is a deliberate asymmetry with `NV3050Probe`, which carries
`regWrite32()` behind three refusals and `-DPHASE4_ALLOW_WRITES`, and the reason
is that the two tracks have opposite premises. The NVIDIA track's endpoint is
booting GSP firmware, which *is* a write sequence. The Intel track's premise is
that firmware has **already** brought the eDP panel up before any macOS driver
runs, so the useful first act is to read that state and judge whether it is
coherent enough to inherit. A write path here would be a facility with no caller
and no test, and the one thing it could do on real hardware is blank a working
panel. Adding one is a decision, not a convenience.

Two scan-shape choices worth carrying forward:

- **All four transcoders are walked, not just A.** The 2026-08-16 capture read
  `0x00000000` out of B/C/D and the capture script printed ABSENT for them. That
  was the script's word, not the silicon's — unused display pages on this die
  read zero rather than all-ones, so "absent" and "never programmed" are
  indistinguishable from a register read. A driver that hardcoded A because of
  that capture would be right on this laptop and silently wrong elsewhere.
- **DSI0/DSI1 are excluded.** `IG_TRANS_SCAN_COUNT` is 4, not
  `IG_TRANSCODER_COUNT`. `igPlaneReg()` refuses any pipe above
  `IG_TRANSCODER_D` because DSI is off the `0x1000` plane ladder entirely, so
  scanning DSI would produce transcoder verdicts with no plane to join them
  against. That is handoff15 Q1/Q2, and it stays *unanswerable* on an eDP-only
  machine rather than merely unanswered.

---

## 2. The section to read: the shim's include GRAPH was wrong, in both directions

This is the finding worth the session.

The new shell failed the syntax gate twice, and neither failure was in the
shell.

**First:** `fatal error: 'IOKit/IODeviceMemory.h' file not found`. The shim
declared `IODeviceMemory` **inside** `tests/kextshim/IOKit/IOMemoryDescriptor.h`.
Apple ships it in its own header. `NV3050Probe.cpp` never noticed for months
because it reaches the class *transitively* through `IOPCIDevice.h` and never
names the header. `IntelDisplayProbe.hpp` includes it by name — which is the
correct thing for a translation unit to do rather than leaning on another
header's include graph — and got a fatal error from the shim's layout on a
shell whose own include list was right.

**Second:** `use of undeclared identifier 'snprintf'`. There was no
`libkern/libkern.h` shim at all. `snprintf` is a kernel export that
`IOKit/IOLib.h` does **not** declare — Apple puts it in `libkern/libkern.h` —
so a kext that calls it without that include is relying on nothing. Here clang
was simply right: the shell was missing an include, and the front end caught
exactly the omission this gate exists to catch.

The generalisation, and the reason this is filed as a finding rather than two
chores: **matching Apple's symbol set is not matching Apple's include graph, and
only the graph decides whether a `.cpp`'s include list is correct.** The visible
direction is the harmless one — a shim flatter than Apple's reddens the gate for
the wrong reason, which is annoying and self-announcing. The dangerous direction
is the invisible one: **a shim that collapses two Apple headers into one accepts
a translation unit that OMITS an include Apple requires**, and the omission first
appears under Apple clang on the operator's single build attempt. That is the
same failure shape as `handoff8`-era `IODMACommand::mappingOptions` being typed
`UInt32` where Apple uses an unscoped enum, one level up: not a signature that is
too loose, a *file layout* that is too loose.

Fixed by splitting `IOKit/IODeviceMemory.h` out as its own shim header, with
`IOKit/pci/IOPCIDevice.h` including it (it has to — `getDeviceMemoryWithIndex()`
returns that type, so a forward declaration would let a `.cpp` call the method
and never touch the returned object's members), and by adding
`libkern/libkern.h` with a `format(printf, 3, 4)` attribute on `snprintf`.
`SIGNATURES.md` gained a section for the new header and `README.md`'s file table
now names it; the audit's header count moved eight to nine.

---

## 3. Two tests that were stricter than the property they described

Both went red when the second shell landed. Both were correct behaviour by the
code and stale expectations by the test — and neither was obvious from its
failure text, because a positive control going red reads as a broken checker.

**`tests/test_check_header_coverage.py`** builds synthetic gate files to prove
`check-header-coverage.py` can fail. Its `full_gate()` fixture spelled
`NV3050Probe.cpp` as a literal in the compiled-sources list. A dependent
header's exemption is "some compiled source pulls it in", so the moment a second
shell existed, `IntelDisplayProbe.hpp` was reached by nothing in the fixture and
`test_a_synthetic_correct_gate_also_passes` — the *positive control* — went red
against a checker that was right. Now derived from `os.listdir(KEXT_DIR)`.

**`tests/test_kext_makefile.py`** had:

```python
self.assertEqual(self.sources, ["NV3050Probe.cpp"])
```

under a comment reading "the whole architecture rests on the headers being
header-only; a .hpp appearing here would mean the split had quietly stopped
being real". The assertion is a strictly stronger claim than its own comment: it
pins the *count of shells*, which is not a property the architecture cares
about. Split into `test_no_header_is_compiled` (every entry ends `.cpp`) and
`test_every_compiled_source_is_an_iokit_shell` (every entry contains
`OSDefineMetaClassAndStructors`). The second is the one that matters — "ends in
`.cpp`" would still admit a decode translation unit, which is precisely what the
split exists to prevent, because a `.cpp` of pure logic is a file `tests/` can no
longer compile under MSVC.

---

## 4. `check-kext-personalities.py` is now two-track

Done before the shell, because the shell needs a personality and a personality
naming an unvouched-for device must not pass.

The checker held ONE die and ONE vendor as module constants and looped over every
personality with them. That was correct while the plist held only `NV3050Probe`.
The moment an Intel personality appears it stops being correct in the worst way:
the vendor branch fires with "vendor half is 0x8086, not 0x10DE" — a message
right about its own arithmetic and wrong about the world, the identical failure
shape handoff16 §1 records for the `IG_PIPE_` prefix collision.

A personality is now classified against the track its **own vendor half names**,
via a `TRACKS` table where each track carries its own `doc`, `die`, `aliases`
and `min_ids`. The accept set is still parsed rather than typed —
`INTEL-DEVICE-IDS.md` for Intel, `DEVICE-IDS.md` for NVIDIA — because a
hardcoded set is a second table nothing keeps true. A vendor this project HAS a
track for, whose document is absent from the tree, is a FAIL rather than "no
opinion": a personality nothing can vouch for must not pass by default.

Twelve tests went green on the first run, which is also what twelve tests against
a checker ignoring the Intel track entirely would do, since the real tree had one
personality and it was the NVIDIA one. So the rewrite was mutation-probed: nine
cases, zero survivors, asserting the **verdict word** out of `classify_token()`
rather than an exit code. The probe was a scratch script and is **not** in the
tree — do not grep for it and conclude it was lost. It mutated the checker's
`TRACKS` table in memory and never wrote to `phase0-kext/` or `tools/`, which is
also why nothing of it remains to re-run. The probe's own first draft carried two defects, both producing a confident
wrong answer rather than an error:

- `states_for()` selected each track's parsed document **by vendor**, so the
  "Intel track pointed at DEVICE-IDS.md" mutation edited a field the probe then
  ignored, and reported a SURVIVING mutant against a correct checker. A mutation
  that changes a field nothing reads is indistinguishable from a checker that
  reads it and does not care. Now keyed by `track["doc"]`, the way `main()` keys
  it.
- The vacuity guard demanded two dies per document. `INTEL-DEVICE-IDS.md` has
  exactly one — GT1 and GT2 are both TGL on purpose, differing in
  execution-unit count and not in the display block — so the guard aborted the
  probe on a correct tree. Floors are now per-document and asymmetric.

The probe mutates the checker's `TRACKS` **in memory, never in the tree**: a
tree-mutating probe makes a concurrent gate run report named FAILs with real
tracebacks and no defect.

---

## 5. The plist personality, and the two numbers in it

```
IOClass          IntelDisplayProbe
IOProviderClass  IOPCIDevice
IOPCIMatch       0x9a688086
IOProbeScore     1000
```

`0x9a688086` is `(device << 16) | vendor` for `8086:9A68` — the **one** Intel
part this project has ever seen on real silicon
(`igpu-results/tgl-capture-2026-08-16-022556.txt`). Every other TGL id in
`INTEL-DEVICE-IDS.md` rests on the vendored i915 table alone, so only the
observed one is matched. `probe()` re-reads the device id from config space and
asks `IntelGpuId.hpp` anyway, because the plist is data an operator can edit and
`check-kext-personalities.py` runs on the HOST — nothing on the target enforces
it.

`IOProbeScore` is **1000**, against the NVIDIA personality's 100000. A shell that
only reads state firmware already programmed has no business outbidding a real
display driver. It still exceeds IOPCIFamily's generic 0.

`CFBundleIdentifier` inside the personality is the **bundle's** id
(`local.nv3050.NV3050Probe`), not a per-personality one, and both shells link
into one bundle on purpose: a bundle is the unit macOS loads, so two bundles
would mean two `Info.plist`s, two signatures and two `kextload` steps for what is
one project.

---

## 6. Payload staleness bit twice, and the second bite was self-inflicted

`build-payload/kext-src.tar` is compared byte-for-byte against the live tree, so
any `phase0-kext/` edit reddens the Python gate. Known. What is worth recording
is the shape of the two mistakes around it:

1. I rebuilt the tar **by hand** with `tar -cf`, which cleared the tar test and
   left `step7-payload.iso` — a third copy of the same bytes — still carrying
   the old one. Use `python tools/build-step7-payload.py`; it writes both and
   then runs `tests/test_guest_build_kext.py` over what just landed.
2. I updated `OPERATOR-ACTION-step7-build.md`'s derived numbers, then edited
   `IntelDisplayProbe.hpp` again (the `libkern.h` include), which moved both
   md5s and made the card stale a second time. **Edit the card last.**

The twenty-first rebuild is in the card. Sizes moved by three tar blocks —
`kext-src.tar` 286720 → 317440, `step7-payload.iso` 364544 → 395264 — the
largest single payload change in that series, and structural rather than another
header layer.

---

## 7. What this does NOT establish

Read this section before quoting the green table at the top of this file at
anyone.

- **`IntelDisplayProbe` has never run.** Never `kextload`ed, never matched,
  never handed a provider. The syntax gate proves clang's front end gets through
  it against `tests/kextshim/`; it says nothing about IOKit semantics.
- **No suite executes a line of `IntelDisplayProbe.cpp`.** The three Intel C++
  suites drive the *headers*. The shell's own content — the four verdict-name
  switches, `igTransName()`, the `snprintf` property-key composition, the
  BAR-length warning, the `regRead32()` policy-range refusal — has **zero**
  runtime coverage. Every one of those is reachable only by parse. A verdict-name
  switch returning a neighbouring name would be invisible to all four gates.

  **PARTLY SUPERSEDED 2026-08-17, later the same day — see `handoff20.md` §1.**
  The last sentence was not a hypothetical. The switches, `igTransName()` and the
  key composition moved into `phase0-kext/IntelDisplayNames.hpp` and are now
  driven by `tests/test_intel_display_names.cpp` (236 checks), which found that
  `igModeVerdictName()` had arms for verdicts 0..5 while
  `IG_MODE_VERDICT_LIMIT` is 6 — so `IG_MODE_SRC_MISMATCH` published the unknown
  string. What still has zero runtime coverage is everything that genuinely needs
  IOKit: the BAR-length warning, the `regRead32()` policy-range refusal, and the
  `publishTranscoder()`/`publishPlane()` call shape around those pure functions.
- **The IORegistry properties are the decode headers' opinion of the words we
  read**, not a claim the inheritance path works. Nothing has inherited anything.
- The kext still has not been built since the Intel files were added. The
  2026-08-14 Apple-clang build predates them; the payload on the ISO is current,
  the *build result* is not.

---

## 8. Open list

1. **Nothing tests the shell's own logic.** *CLOSED 2026-08-17, later the same
   day — `handoff20.md` §1.* The highest-value item, and it is
   the same argument that produced the `phase0-kext/*.hpp` split in the first
   place: the verdict-name switches and the property-key composition are pure
   functions of an integer and a string, and they are sitting in a file no test
   can compile. Either they move into a dependency-free header with a suite, or
   something has to drive them. Note the switches were written as switches
   rather than string tables **specifically** so that a family growing produces
   a compiler warning rather than a read past the end — that reasoning is only
   as good as the `-Werror` the gate already applies, and it does not cover a
   case arm returning the wrong literal.

   That last sentence is the one that mattered: they moved to
   `IntelDisplayNames.hpp`, `tests/test_intel_display_names.cpp` drives them, and
   the very first exhaustiveness check reddened on a **missing arm** rather than a
   wrong literal. A `switch` does fall to `default` instead of reading past the
   end of a table — but `default` returns the unknown string, which is honest and
   useless, and no gate in this tree could tell that apart from a working verdict.
2. **A second Apple-clang build.** `make` has not run since the Makefile gained
   a second source. The link is the one thing no host gate can model, and
   `SOURCES` now has two objects where the 2026-08-14 success had one.
3. handoff18 §5's NVIDIA-side items are untouched by this session.
4. handoff15 Q1/Q2 (DSI pages, the `0x800` stride) and Q3 (plane count) remain
   unanswerable on an eDP-only machine, and the scan shape in §1 encodes that
   rather than papering over it.
