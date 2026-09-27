# 3050-handoff37

NVIDIA GA107 track. Continues `3050-handoff36.md`. Written 2026-08-19.

Two phases. The first closed handoff36's own open item and is ordinary work. The
second asked the question the operator's instruction actually points at -- can
this chip reach a port? -- and answered it out of the board's own firmware. The
answer is no, and the interesting part is not the answer but what it took to be
allowed to believe it.

**`phase0-kext/` is BYTE-IDENTICAL for phase 2.** Nothing shipping changed after
the `NVFalcon.hpp` edit recorded in section 2 below, so `build-payload/` was
rebuilt once, not twice.

---

## 0. Read this first if you read nothing else

The GA107 VBIOS in `reference/ga107-firmware/vbios-ga107.rom` **declares zero
display paths**. Not "none we could decode" -- zero, authored, corroborated on
seven independent arms. The Display Configuration Block is present, well formed,
version 0x41, sitting where the pointer at ROM offset `0x36` says it sits, and
its `entry_count` field holds `0x00`. The connector sub-table it points at also
holds zero entries, while its sibling tables are populated (CCB 15, GPIO 36, I2C
32).

That is the board telling you, in its own firmware, that no encoder on this chip
is wired to any connector.

`3050-handoff27.md` section 1 recorded the operator's report that the laptop is
muxless. This is the same fact arrived at from the opposite end -- not from what
somebody said about the machine, but from what the GPU's own ROM describes -- and
it is **stronger in one specific way**: handoff27 named HDMI as the bypass. This
DCB does not describe an HDMI path either. There is no TMDS entry, no
DisplayPort entry, no LVDS entry, because there are no entries.

A driver cannot invent a path the board does not carry. `PLAN.md` termination
condition A was already satisfied; this closes the last direction anybody could
have hoped to route around it.

The report is committed at `dcb-report.txt`, in `vbios-report.txt`'s idiom.

---

## 1. What phase 1 closed

`3050-handoff36.md` section 6 item 3. Not repeated here; the work is recorded in
that file and in `tests/test_falcon.cpp`, which went 193 -> **239** checks. The
shape of it was handoff36's own subject one more time: `nvRiscvHasError()`
returned a bare `bool` over two registers that could each independently fail to
decode, so "neither register answered" and "neither register reports an error"
were the same `false`.

The fix is the five-value verdict in `phase0-kext/NVFalcon.hpp`:

```c
if (nvRiscvErrReadRefused(privErrStat)
    || nvRiscvErrReadRefused(hubErrStat)) {
    return NVRISCV_ERR_UNREADABLE;
}
if (privErrStat != 0 && hubErrStat != 0) return NVRISCV_ERR_BOTH;
if (privErrStat != 0)                    return NVRISCV_ERR_PRIV;
if (hubErrStat  != 0)                    return NVRISCV_ERR_HUB;
return NVRISCV_ERR_NONE;
```

`nvRiscvHasError()` is KEPT beside it as a one-line wrapper that is `!=
NVRISCV_ERR_NONE` -- the reasoning is `3050-handoff34.md` section 2's: the
wrapper is what the shell actually calls, so a verdict-only suite would be fully
green over a wrapper that stopped consulting the verdict. Non-vacuity proof
reddened exactly **5 checks, all five new, every pre-existing check green**.

---

## 2. Phase 2, and why an absence needed seven arms

### 2a. The tool

`tools/parse-dcb.py`, ~470 lines. `tools/parse-vbios.py` already decodes the PCI
ROM image chain, the BIT table, the string table and the falcon ucode pointer,
and says **nothing about display**. The DCB is the only artifact in this tree
that bears on whether this chip can drive a screen.

Provenance is cited to the vendored NVIDIA source rather than reconstructed:
`reference/open-gpu-kernel-modules/src/common/nvswitch/kernel/inc/rom_nvswitch.h`,
structure `_nvswitch_vbios_dcb_header_41` at `:836`-`:855` with its own format
string `"4b1w1d6w1b2w2d"`, and `NVSWITCH_DCB_PTR_OFFSET 0x36` at `:834`. Every
field width in `DCB_HEADER_FIELDS` comes from that format string, and a test
asserts the widths sum to `DCB_HEADER_41_SIZE` so the table and the constant
cannot drift apart.

### 2b. The finding, measured

DCB header at `0x58ba`:

```
41 23 00 08 5d 59 cb bd dc 4e b2 3f 00 00 00 00 00 00 9f 59 24 5a c1
^^ ^^ ^^ ^^ ^^^^^ ^^^^^^^^^^^ ^^^^^                   ^^^^^ ^^^^^ ^^
|  |  |  |  ccb   signature   gpio                    i2c   conn  flags
|  |  |  entry_size 8
|  |  ENTRY_COUNT 0
|  header_size 0x23
version 0x41
```

The eight bytes immediately past the header decode as type nibble `0xE` -- EOL,
the explicit terminator -- which is consistent with the count rather than
independent of it, and is recorded as consistency and not as evidence.

### 2c. Why the zero could not simply be believed

**An absence is the one finding a decoder cannot report honestly on its own
evidence.** Three completely different failures produce a byte-identical "no
display paths":

1. the decoder landed one field off, and read some neighbouring byte as the
   count;
2. the pointer led somewhere that is not a DCB at all, and the whole decode is
   noise that happened to contain a zero;
3. the ROM dump is truncated before the entry area.

All three print the same line. So `corroborate_absence()` exists, and it runs
**only** when the entry list is empty -- a present path needs no corroboration,
and a corroboration function that also ran on the positive case would be answering
a question nobody asked. Its seven arms, each measured on the real image:

| arm | reading |
|---|---|
| image chain complete (not truncated) | 127 x 512 = 65024 = exactly the file size, last-image bit `0x80` set |
| signature occurs exactly once in the image | one hit, at `0x58ba` |
| the pointer at `0x36` selected that one header | `hits[0] == header base` |
| header version is a known DCB version | `0x41`, inside `0x30..0x4F` |
| `header_size` matches the version-4x layout | `0x23`, equal to the format string's sum |
| `entry_size` holds two 32-bit words | 8 |
| at least two sibling sub-tables are populated | three: CCB 15, GPIO 36, I2C 32 |

The verdict is `AUTHORED` only when **all seven** agree; any failure prints
`UNPROVEN -- treat as a MISREAD`.

**The load-bearing arm is the last one**, and it is worth understanding because
it is the only one that separates a real zero from a plausible misread. Failures
1 and 2 above do not merely corrupt the count -- they corrupt every field in the
header, including the four sub-table pointers. A decode that landed on garbage
reads garbage counts out of the siblings too. A board that genuinely wires no
display outputs still has a populated GPIO table and a populated I2C table,
because those serve power sequencing, fan control and thermal sensors regardless
of whether any pixel ever leaves the chip. So *count zero beside three sane
sibling counts* is a shape that a misread does not produce.

The threshold is two rather than one because a single plausible-looking table
could be coincidence, and there is a test on **both** sides of that threshold --
one populated sibling must fail, two must pass -- so it is not a one-way
assertion.

### 2d. The three load-bearing refusals in the tool

**`find_dcb()` refuses rather than guessing, and a signature scan is
deliberately NOT a fallback.** A 32-bit constant occurs by chance in a 64 KiB
image often enough that finding one proves nothing, and a decode that lands
somewhere plausible reports *fabricated board wiring* -- which is worse than no
decoder, because it is `handoff18.md` section 1d's class applied to the one
question this whole track turns on. `tests/test_parse_dcb.py` pins this
directly: an image whose pointer is zero must refuse **even when a valid
signature is present elsewhere in it**.

**`parse_connectors()` returns `None` rather than raising.** The display-path
entries are the finding; refusing the entire decode because a secondary table is
absent would throw away the primary answer to protect a footnote.

**`parse_entries()` bounds the count against the image.** `entry_count` is
untrusted input from a file, and a decoder that reads past its own buffer on a
bad count is reporting whatever follows in memory as board wiring.

---

## 3. The gate, and the four defects its own controls found

`tests/test_parse_dcb.py`, **49 tests**. Every sibling tool has a test module
(`tests/test_parse_vbios.py` is 53 tests); without one, `parse-dcb.py` would sit
outside the Python gate's reach except for line endings and cited paths, which is
precisely the shape `when-no-gate-can-see-it-pin-the-shape` forbids.

The module is deliberately weighted away from the happy path. The bulk of it is
twelve controls establishing that **each corroboration arm is individually
falsifiable**, because a corroboration function whose arms cannot fail is an `ok`
line dressed as proof, and it would ratify a misread as the board's own
statement. Each control moves exactly one thing and asserts **which** arm broke,
plus that every other arm stayed green -- a control asserting only "the verdict is
not AUTHORED" passes for the wrong reason, since any other arm breaking satisfies
it too.

Synthetic images are built by `build_image()` for the arms and the refusals; the
real 65024-byte file drives the finding, in a class that SKIPS with a banner when
`reference/ga107-firmware/` is absent. Both, for `3050-handoff27.md` section 2b
item 5's reason: a fixture that only ever constructs the shape it expects cannot
discover that a second shape exists. There is also a control on the fixture
itself -- the default image must pass all seven arms -- because if that ever
reddens, every negative control below it becomes unattributable.

### 3a. Four defects, all in the tests, none in the tool

Worth recording because two of them are new roads into familiar families.

**1. A hyphen in a method name.** `test_report_always_carries_the_not-a-measurement_caveat`
is a syntax error, and the module failed to *import*. The tell is that the gate
reports `ImportError: Failed to import test module`, which reads as a broken
dependency rather than a typo -- and is the same misreading trap `CLAUDE.md`
documents for `site-packages/tests/`.

**2. A negative control that reddened against correct code, by arithmetic.**
`test_count_running_past_the_image_refuses` used `entry_count=200`. But 200
entries at 8 bytes is 1600 bytes from a header at `0x823`, which **fits** inside
the 4096-byte fixture -- so the control was demanding a refusal the subject was
correct not to make. `entry_count` is one byte, so 255 is the largest overrun
expressible and is the value that actually leaves the image. Third session
running where a first-draft failing test was the control rather than the subject.

**3. The one that is a genuine coupling, and must be asserted as a PAIR.**
`test_a_second_signature_breaks_only_the_uniqueness_arm` planted a second
signature at `0x0400`, below the real header at `0x0800`, and reddened **two**
arms instead of one. That is not a defect in the tool. `signature_occurrences()`
scans forward, so a hit planted lower becomes `hits[0]`, and the
pointer-selection arm compares against `hits[0]` -- so for that image it is
genuinely true both that the signature does not occur exactly once **and** that
the first signature is not the one the pointer chose. Two true statements. The
fix is not to loosen the arm, it is to plant the decoy **above** the header when
isolating uniqueness, and to keep the below-header case as its own control that
names both arms. Forcing a coupled pair into a single-arm control would have
meant weakening a correct assertion to accommodate a wrong test.

**4. A non-ASCII byte in committed output.** The report printed `3050-handoff27
\xef\xbf\xbd1` -- a `§` mangled by the Windows console codepage. Replaced with the
word "section", and `test_report_is_pure_ascii` plus
`test_the_committed_report_is_pure_ascii` now pin it, because a committed
artifact containing a non-ASCII byte has a regeneration diff that depends on
which console produced it.

### 3b. The committed artifact is pinned two ways

`dcb-report.txt`, 3360 bytes, CRLF (matching `vbios-report.txt` and
`parse-report.txt`; `tests/test_line_endings.py` is narrow to `.cmd`/`.bat` by
design and has nothing to say about it).

It is checked for byte-for-byte regeneration **and** for the meaning it carries.
Regeneration alone is not enough: it passes any report somebody re-ran, including
one whose verdict flipped from `AUTHORED` to `UNPROVEN`. So
`test_the_committed_report_states_the_finding` asserts `NO DISPLAY PATHS` and
`AUTHORED` are present and `UNPROVEN` is absent. That is the arm that notices the
world changing rather than the file aging.

The regeneration comparison is over **lines**, not raw bytes -- the committed
file is CRLF from Windows text-mode redirection while `report()` returns bare
strings, so a byte comparison would redden on the newline convention rather than
on content.

---

## 4. Gate readings

| gate | reading | note |
|---|---|---|
| `python tools/check-registers.py` | **240/240**, 0 failed, 0 skipped | unmoved, correctly -- no `#define` added |
| `tests\check-kext-syntax.bat` | exit **0**, zero `FAIL` lines | read the exit code, not the last verdict line |
| `tests\run-python-tests.bat` | **1073** tests, **FAILED (failures=1)** | the one red is the peer's; see below |
| `tests\build-msvc.bat` | not re-run | no C++ changed in phase 2; `test_falcon` 193 -> 239 measured in phase 1 |

Python went 1024 -> **1073**, and the +49 is exactly this module, measured by
running it alone before running the gate.

### 4a. The one red is the peer's, and it stays

```
FAIL: test_every_suite_on_disk_has_a_recorded_count
      (test_check_count_floor.TableMatchesTheTree)
  tests/test_intel_scanout.cpp exists but has no MEASURED entry
```

Attributed by mtime: `tests/test_intel_scanout.cpp` landed at **13:01**, ten
minutes after `tools/parse-dcb.py` (12:52) and one minute before
`tests/test_parse_dcb.py` (13:02). `phase0-kext/IntelScanout.hpp` is from
2026-08-18 20:58. This is the Intel track's new suite, mid-landing.

**Nothing was touched**, for the second session running. Adding a `MEASURED`
entry would mean recording a check count for a C++ suite I have not built, under
this session's name -- vouching for a peer's half-landed work, which is
`handoff18.md` section 3's rule and `intel-handoff27.md` section 5a's. Note this
is a *different* failure from the one the previous session left alone
(`test_source_cited_paths`, over the same peer's files); that one has since gone
green underneath me, which is itself the argument for the rule.

A gate's verdict in a two-agent tree is a JOINT measurement. Red does not mean
you broke it.

---

## 5. What this does NOT establish

1. **It is a firmware read, not a hardware measurement.** The report says so in
   its own text. The DCB is what the board's ROM *describes*; nothing here probed
   a pin. A board could in principle carry silicon paths its VBIOS declines to
   enumerate. But a driver has nothing to go on except the enumeration, so for
   every practical purpose this is the answer.

2. **It says nothing about the Intel chip's outputs**, which are the ones that
   exist on this laptop. Everything the panel and the HDMI port do is on the
   other track.

3. **The seven arms are corroboration, not proof.** They are jointly hard to
   satisfy by accident; they are not a theorem. The honest statement is the one
   the tool prints: the zero is authored rather than a misread.

4. **`tools/parse-dcb.py` has never run against a second VBIOS.** Every arm is
   calibrated on synthetic images plus one real file. A GA107 board that *does*
   declare paths would exercise the positive side of `parse_entries()` in a way
   nothing here has.

5. **Phase 1's verdict still does not reach the IORegistry.** `NVFalcon.hpp` now
   produces five distinct codes; whether the shell publishes them is section 6
   item 1.

---

## 6. Open list

Ordered by value, hardware-blocked items last.

1. **Publish the RISC-V error verdict.** Phase 1 gave
   `nvRiscvErrorVerdict()` five codes and the shell still consumes only the
   bool. Same class as handoffs 31, 33, 34 and 36 -- a diagnosis reaching the
   log and not the consumer -- and the detection half is already built and
   tested. This is the cheapest remaining NVIDIA work and the pattern for it is
   established four times over.

2. **Decide what the DCB finding means for `PLAN.md`.** Termination condition A
   is now confirmed from two independent directions. Somebody should write that
   into `PLAN.md` rather than leaving it in a handoff, because a plan whose
   termination condition is satisfied and unmarked will be worked against.

3. **`tools/parse-dcb.py` is not referenced from `CLAUDE.md`.** The Layout table
   describes `tools/` generically, which is fine, but the read-order section is
   where somebody resuming would look for "has anybody asked whether this chip
   has outputs". Worth one line.

4. **The connector sub-table decoder is untested against a populated table.**
   `parse_connectors()` has a refusal test and a name-distinctness test; its
   actual decode has never run over a non-empty table, because the only real
   image has none. Synthetic coverage would fix that.

5. **One `kextload` on a machine containing the card.** Unchanged, still blocked,
   still the highest-value action in the project -- and now visibly less
   interesting than it was, since section 0 says what it would find at the end of
   the display path.

---

## 7. Addendum: the render-offload question, and a false absence I published

Added later the same day, after the operator asked the obvious follow-up to
section 0: if the 3050 cannot reach a connector, can it RENDER and let the Intel
chip scan the result out?

That architecture has a name -- muxless render offload, Optimus on Windows, PRIME
on Linux -- and it is exactly what this laptop's hardware is. Section 0 is the
same fact from the GPU's side: a part with no display paths in its own DCB is a
render-only part by construction. So the question is well posed, and the answer is
not "the idea is wrong", it is that both ENDS of that pipeline are missing on
macOS while the middle is present but wired for something else.

### 7a. What is actually in the recovery image, measured against all 430 paths

| looked for | matches in 430 paths / 491 bundle ids |
|---|---|
| Tiger Lake / Gen12 / Iris Xe | **0** |
| NVIDIA / GeForce / NVDA | **0** |
| AGDC family (AppleGraphicsControl and its plug-ins) | **15** |

Newest Intel display engine macOS knows: Ice Lake-LP, Gen11. Tiger Lake is Gen12,
the generation directly after -- a different display engine, not a stepping.

The AGDC row is the one that matters for the offload question, and it cuts the
other way from how it first looks. macOS **does** carry dual-GPU machinery:
AppleGraphicsControl, with AppleGPUWrangler, AppleGraphicsDevicePolicy,
AGDCBacklightControl and AGDCPluginDisplayMetrics beneath it. But that machinery
exists for Apple's own dual-GPU Macs, where **both** GPUs are Apple-supported
parts and **both** have display paths, arbitrated by policy or a hardware mux.
It is not a cross-vendor render-offload framework, and there is no macOS analogue
of `DRI_PRIME` -- no supported way to say "render on device A, scan out on device
B" when device A has no scanout at all.

### 7b. Why the pipeline has no end to attach to

The offload chain needs three links. This tree has measured all three:

1. **Render on the 3050.** Needs a Metal userspace driver compiling Metal IR to
   NVIDIA SASS. `PLAN.md` already calls this blocked: NVIDIA's shader compiler is
   closed and Apple's Metal plug-in ABI is private and undocumented. Nothing in
   Phases 0-5 touches it.
2. **Hand the buffer across.** No macOS API expresses this for an unsupported
   discrete part with no display path.
3. **Scan out on the Intel chip.** Zero Tiger Lake framebuffer, measured above.
   This is `PLAN.md` termination condition B, and it is the link that kills the
   plan independently of the other two -- even a working NVIDIA renderer would be
   handing frames to a scanout engine macOS cannot program.

So section 0 and this section are two independent confirmations of the same
ceiling reached from opposite directions, and condition B is the harder of the
two because it does not care what the NVIDIA side does.

### 7c. The mistake worth carrying, which is mine and was in this session

My first pass at 7a grepped `intel-kext-inventory.txt` for `AGDC` and
`AppleGraphicsControl` and got **zero matches**, and I was one sentence from
reporting that macOS ships no dual-GPU machinery at all.

The file opened with `scan reach: 430 kext paths` and then listed
`=== kext paths (16 shown of 430) ===`. Both lines honest; together a trap. The
header states the scope and the body is a sixteen-row excerpt, so a grep measures
the TRUNCATION and reads as a fact about the world.

That failure is silent in exactly one direction. Queries about things inside the
first sixteen rows keep working, so the artifact feels reliable; only queries about
ABSENT things go wrong -- and this tree settles termination conditions by absence,
which makes a filtered inventory a machine for manufacturing false termination.

`intel-kext-inventory.txt` is regenerated **unfiltered** (430 paths, 491 bundle
ids, 49244 bytes) with a header recording what the excerpt cost, and `PLAN.md`
line 161 now states the reach the absence was measured against rather than citing
the file bare. The two absences in 7a are citable **because** they were re-measured
against the full set; before this they were measured against a sixteen-row window
and were worth nothing.

This is section 2c's class one level up. There the DECODER had to prove its own
zero was authored rather than a misread; here the ARTIFACT had to prove its own
scope before any zero taken from it meant anything.

## 8. The display question, answered from Apple's own match dictionaries

The operator asked for a picture in macOS. Section 7 answered the render half and
named the scanout half as an absence of a NAME -- no kext in 430 paths is called
Tiger Lake. This section replaces that with a measurement of Apple's matching
logic, and in doing so **finds one thing this tree had wrong**.

### 8a. The premise nobody had checked

`PLAN.md` termination condition B reads: macOS ships no Tiger Lake framebuffer
driver, therefore the iGPU cannot scan out. The measured half is solid -- zero
TGL / Gen12 / Iris Xe across all 430 paths, now citable because section 7c fixed
the excerpt. But the INFERENCE from it rests on a premise that had never been
tested: **that scanout requires a per-generation framebuffer driver at all.**

Two things could falsify that. A generic framebuffer matching any PCI display
device, or class-only matching inside the drivers that DO ship. Both were
measured rather than reasoned about.

### 8b. A generic framebuffer DOES ship -- and the tree was wrong about this

The recovery image carries `IOBootFramebuffer`: 108 occurrences, an
`IOFramebuffer` subclass with a real vtable (`getDisplayModes`,
`getApertureRange`, `setGammaTable`, `isConsoleDevice`). That is the driver owning
the surface EFI already programmed at power-on, which is why a Mac shows a picture
during boot before any GPU kext has loaded. Beside it: `IOFramebuffer` 2035,
`IOGraphicsFamily` 95, `IONDRVFramebuffer` 195, `AAPL,boot-display` present --
all with three positive controls firing, so those are absences-of-nothing rather
than a blind scan.

So the belief that macOS has no generation-independent scanout path is **false**.
It has one.

It still cannot help, and the reason is the durable part: **`IOBootFramebuffer` has
no IOKit personality.** 108 occurrences of the class name, and ZERO of them within
4000 bytes of an `IOProviderClass` key. It is instantiated by code inside
IOGraphicsFamily, not by matching, so there is nothing that can be pointed at a
PCI device.

That zero is corroborated rather than believed, because it has an obvious way to be
fake -- if IOGraphicsFamily's plist simply sat outside the XML the scan can see,
every graphics personality would be invisible and the zero would measure the scan.
Three arms, all green: known-personality kexts produced joint windows
(`AppleIntelICLLPGraphicsFramebuffer`, `AppleGraphicsControl`, `IOPCIDevice`);
IOGraphicsFamily's OWN classes produced them too (`IODisplayWrangler`,
`IODisplayConnect`, `AppleBacklightDisplay`), so that bundle is demonstrably
visible; and only then does the subject's zero mean anything. Call the class **a
generic driver with no personality never matches** -- the code existing and the
code being reachable are different facts, and a symbol table answers only the
first.

### 8c. The real gate is a device-id allow-list, and it excludes this silicon

The control window handed over the actual mechanism. Apple's framebuffer
personalities carry BOTH keys:

```
IOClass           AppleIntelFramebufferController
IOPCIClassMatch   0x03000000&0xff000000      <- any display controller
IOPCIPrimaryMatch 0x3E9B8086 0x3EA58086 ...  <- explicit allow-list
IOProviderClass   IOPCIDevice
```

IOKit requires both when both are present, so the class match is not the gate --
the allow-list is. `tools/find-display-matches.py` reads those dictionaries out of
the prelinked XML and `display-match-report.txt` is its committed output, in
`vbios-report.txt`'s idiom. Reach: **203 distinct device ids** across every
personality carrying `IOPCIPrimaryMatch` / `IOPCIMatch` / `IOPCISecondaryMatch`,
unfiltered, with the reach line equal to the rows in the body by construction.

**`0x9A688086` -- the Tiger Lake GT2 id measured on the laptop itself -- is ABSENT
from all 203.** Both positive controls are present in the same parse
(`0x3E9B8086` Coffee Lake, `0x8A528086` Ice Lake-LP), so the absence is a fact
about Apple's allow-lists rather than a property of the scan.

This is strictly stronger than the kext-name absence already in `PLAN.md`. It is
not merely that no kext is *called* Tiger Lake; it is that the allow-lists inside
the kexts that DO ship exclude this silicon **by number**. The Intel graphics ids
that are present stop at Ice Lake-LP, which is exactly the generation before.

The one NVIDIA id in all 203 is worth recording because of what it is NOT:
`0x0D9810DE`, owned by `AppleMCP89RootPortPM` -- a root-port power-management
driver for a 2010 MCP89 chipset. Vendor 10de appears once in Apple's entire match
space and not as a GPU driver at all.

### 8d. Two reading errors, both the same shape

First, three vendor counts came back `0 / 0 / 0` against a report that certainly
contains 62 Intel ids. The report renders ids uppercase (`0X3E9B8086`) and the
grep spelled `0x` lowercase, so the zero measured the pattern. Same family as the
sentinel compared through `.upper()` -- **a case-folded hex literal folds its own
prefix**, and the tell is a zero that contradicts a count you already have.

Second, and more dangerous: grepping the whole report for `9a68` returns **1**,
which reads instantly as "the id IS there." It is line 35 -- the tool's own target
declaration in its verdict block. Measured properly, the count is 0 in the
203-row id list and 0 in the personalities section. This is section 7's own defect
reappearing 40 minutes later in a file I had just written: **a document that names
its subject contains its subject**, so a grep over prose-plus-body cannot answer a
question about the body. The committed gate therefore parses the id-list section
and asserts against THAT, never against the whole file.

### 8e. The VM desktop is not evidence, and nearly was

The strongest-looking argument available was that this project's own macOS guest
reaches a Finder desktop (`HANDOFF.md:222`, `handoff3.md:41`, confirmed from
pixels) while the host logs `SVGA3dCaps: Disabling 3d support` and the vmx carries
`mks.enable3d = "FALSE"`. A desktop with 3D disabled looks exactly like proof that
the generic path carries a compositor unaided.

It is not, and the confound is measurable: the 8 GiB `.vmem` of that live guest
contains **`VMwareGfx` 941 times**. VMware ships a macOS display driver and it is
resident, so the guest desktop runs on a real matched framebuffer driver for a
real supported device. The inference to the laptop collapses entirely. The
measurement that would have been quoted as decisive was one grep away from being
backwards, and the only reason it was caught is that the probe listed the confound
as a needle instead of assuming its absence.

Worth recording alongside it, from the recovery image: `AppleSoftwareRenderer` 0,
`AppleMetalSW` 0, `AppleParavirtGPU` 0. **No software-Metal fallback ships.** So
even a matched framebuffer would give an unaccelerated surface with no Metal
device behind it.

### 8f. The gate

`tests/test_find_display_matches.py`, **33 tests**, Python 1073 -> 1117 (the other
+11 is the peer's `tests/test_intel_publisher_scan_shape.py`, 14:20, attributed by
mtime and left alone). Four choices in it are load-bearing:

1. The `PRESENT` verdict has a **positive control** -- a synthetic image carrying a
   fabricated `AppleIntelTGLGraphicsFramebuffer` personality. Without it, `ABSENT`
   is satisfied by a tool that can only ever say `ABSENT`.
2. Each corroboration arm is broken **individually**, with the control asserting
   exactly one arm failed. Both halves of the control-id pair are dropped
   separately, so neither is decoration.
3. The reach line and the body are pinned by **set EQUALITY**, plus a non-empty
   floor -- because `0 == 0` satisfies equality, and that is precisely how the
   inventory excerpt in section 7c would have passed its own header check.
4. Ids are matched **case-insensitively**, with a test driving lowercase ids
   through the whole path, so 8d's first error cannot be reintroduced silently.

A chunk-boundary test and its double-count partner sit beside them: the scan reads
64 MiB at a time with a 64 KiB overlap, and a personality straddling the seam is
both losable and countable-twice.

### 8g. What this settles for the operator, plainly

A picture on that laptop panel under macOS needs a driver that ATTACHES to
`0x9A688086`. Measured against Apple's own dictionaries: nothing does, the generic
framebuffer that exists cannot be pointed at anything, and no software renderer
ships. The remaining route is the Hackintosh one -- inject a fake device id so an
Ice Lake personality attaches -- and that is written into the report's own
limitations rather than sold as a plan, because Gen11 and Gen12 are different
display engines. An ICL driver programming TGL registers is the `PLANE_SIZE` /
`PIPESRC` transposition this tree already measured on silicon, one level up: it
programs cleanly and yields a plausible wrong picture, which is worse than a blank
screen because it looks like progress.

### 8h. Gate readings

| gate | reading |
|---|---|
| `tools/check-registers.py` | 240/240, 0 failed, 0 skipped, exit 0 -- unmoved, correctly: no `#define` added |
| `tests/run-python-tests.bat` | **1117** tests, OK, exit 0 (+33 mine, +11 the peer's) |
| `tests/check-kext-syntax.bat` | exit 0; the single `FAIL` hit is section 11's own title, not a failure |
| `tools/check-documented-paths.py` | exit 0 |

`phase0-kext/` is **BYTE-IDENTICAL** this section -- the work is a host tool, a
committed report and a gate -- so `build-payload/` was NOT rebuilt and the
thirty-third image is still current. The payload gate's silence is the proof
rather than an assertion.

### 8i. What section 8 does not establish

1. This is the RECOVERY image, a trimmed subset of a full install. Strong
   evidence about the installed system volume, not proof.
2. `IOBootFramebuffer`'s missing personality is measured by proximity to an
   `IOProviderClass` key within 4000 bytes. A personality serialised more than
   4000 bytes from its own class name would read as absent. The controls make that
   unlikely rather than impossible.
3. Nothing here is a hardware measurement. It is a read of Apple's shipped match
   dictionaries, and the one thing that would settle the whole question remains
   what it has been for eight sessions: one `kextload` on a machine containing the
   card.
4. `parse_connectors()` in `tools/parse-dcb.py` is still un-driven over a populated
   table (section 6 item 4, unchanged).

