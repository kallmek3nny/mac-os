# intel-handoff28.md

Intel Gen12 (Tiger Lake) display track. Continues `intel-handoff27.md`. The
peer's NVIDIA sessions of the same day are `3050-handoff28.md` and
`3050-handoff29.md`; this file is independent of both and either order works.

This session shipped no kext code. `phase0-kext/` is byte-identical, so the
payload gate has nothing to say about it and `build-payload/` was not rebuilt
by me. What moved is the EVIDENCE layer, and it moved further in one session
than the whole track has since 2026-08-16.

---

## 0. The operator directive is CLOSED

The directive that has shaped the last two sessions:

> "u can boot the vmware instance for me on this pc, ill boot linux but i dont
> like running a ton of scripts so make 1 giant sh script to get everything u
> need form linux, and once youre COMPLETELY done with linux AND NEVER NEED IT
> AGAIN, then ill install macos and we can verify boot"

All three constraints are now satisfied. One script (`tools/igpu-final-capture.sh`,
written in intel-handoff27). One command. And as of the `20260818-201412 UTC`
run, **Linux is no longer needed on that laptop.** The answer to the operator's
direct question "is linux needed anymore?" was NO the second the MMIO section
came back populated, and it is recorded here so nobody re-opens it: macOS may
be installed over that Linux whenever the operator wants.

The thing to understand before reading anything else in this file is that the
asymmetry has now flipped. Up to 2026-08-18 the standing rule inside the
capture script was DUMP WHEN IN DOUBT, because an omitted page was permanently
unknowable. That rule has expired. From here on the constraint is the reverse:
**nothing new can ever be measured on this silicon**, so every remaining Intel
question is answered out of the two files in `igpu-results/` or it is not
answered at all.

---

## 1. The first run failed, and the failure was diagnosable only by accident of design

There were TWO runs. The first, `20260818-185413 UTC`, produced a complete
provenance block and then **zero bytes of MMIO**, dying on:

```
PermissionError: [Errno 1] Operation not permitted
```

at line 7 of the embedded heredoc — the `mmap.mmap(fd, span, ...)` call in
`tools/igpu-final-capture.sh`. Running as root. `python3` present. The BAR
resource file opened fine; it was the *mapping* that was refused.

The diagnosis is two screens ABOVE the traceback, in the script's own
provenance block:

```
lockdown   none [integrity] confidentiality
secureboot SecureBoot enabled
```

Secure Boot puts the kernel in `lockdown=integrity`, and integrity lockdown
**refuses `mmap` of a PCI BAR through sysfs even to root**. This is not a
permissions problem and not a missing capability — `CAP_SYS_RAWIO` does not
lift it, because it is a policy refusal rather than an access-control one. No
amount of `sudo` would have changed the outcome.

**§1a — the durable lesson, and it is about script authorship rather than
about Linux.** The script prints `lockdown` and `secureboot` BEFORE the MMIO
section, and that ordering is the only reason this failure read as a
diagnosable environment refusal rather than as "the capture script is broken."
A one-shot script whose whole premise is that it will never run again must
print the conditions that could make it fail *before* it attempts the thing
that fails. Had those two lines been absent, the honest conclusion available
from the artifact would have been "the embedded Python is wrong," and the
correct next action would have been to edit a script that was already correct.

The operator's own intervention closed it: "i can run without secure boot if
thats the issue." It was exactly the issue. Second run,
`20260818-201412 UTC`:

```
lockdown   [none] integrity confidentiality
secureboot SecureBoot disabled
```

and the mmap succeeded.

**No change was made to `tools/igpu-final-capture.sh` in this session.** The
script was right; the environment refused it. Worth stating plainly because
the tempting response to a red run is to edit the thing that ran.

---

## 2. The capture, and why there is exactly one copy of it

The artifact landed as:

```
igpu-results/tgl-final-capture-2026-08-18-201412.txt
```

The operator has their own backup at `igpu-final-20260818-201412.txt` in the
project root and asked that no second copy be made. That file is the RAW run:
71,344 bytes, md5 `3bd389badd5cf980dce0f55c45452a55`. The file in
`igpu-results/` is that byte sequence plus host-side READING NOTES appended
below a divider, following the same two-documents-in-one-file convention the
2026-08-16 capture established. Total 78,577 bytes, zero CR bytes, divider at
byte 71,411.

The frozen region — everything above the divider — is 71,345 bytes, which is
the raw file's 71,344 plus one, because the notes were appended after a newline
the raw file did not end on. That off-by-one is written into the gate's own
provenance comment rather than left to be rediscovered.

What the run produced: **718 live dwords across 25 ranges.**

---

## 3. The capture-integrity gate could not see the capture. Two defects.

`tests/test_capture_integrity.py` is the one module in the tree whose entire
job is protecting unrepeatable machine output. It was green. It was also, at
the moment the second artifact landed, guarding exactly half the evidence.

### §3a — the glob silently narrowed the subject set

The pattern was:

```python
_PATTERN = os.path.join(_ROOT, "igpu-results", "tgl-capture-*.txt")
```

The new artifact is named `tgl-final-capture-2026-08-18-201412.txt`, which that
glob does **not** match. So the larger of the two irreplaceable captures — the
one carrying 718 measured dwords, twenty times the content of the first — was
invisible to its own gate, unhashed, unguarded, and reported green while being
so.

This is the vacuity direction and it is the worse one. A gate that fails
loudly is a gate. **A gate that silently narrows its own subject set converts
an absence of coverage into a positive-looking result.** It was found by
running `glob.glob` on both patterns side by side and reading the counts, not
by reading the source — reading the source is how the pattern got written in
the first place.

The fix is not merely widening to `tgl-*capture-*.txt`. A widened glob can be
narrowed again by a future edit with every other arm staying green, on a
smaller subject set. So the widening is made load-bearing by two new arms:

- `test_every_artifact_on_disk_is_pinned` requires set-EQUALITY both ways
  between the files on disk and the rows in `EXPECTED`. A third capture under
  any `tgl-*capture-*` spelling reddens until somebody measures it and adds a
  row; a row whose file vanished reddens too, and that second direction is the
  one that hides.
- `test_the_old_narrow_pattern_would_have_missed_an_artifact` asserts
  `narrow < broad` — strict subset — so the broad pattern is provably broader
  in THIS tree. Restoring the old spelling reddens immediately.

### §3b — the below-divider mutation anchor was inverted in the new file

The module has three arms that mutate the blob and assert what the hash does.
Arm 3 rewrites a token BELOW the divider and requires the digest to HOLD,
proving that ordinary note edits are not reported as changes to machine output.

The anchor was:

```python
BELOW_ANCHOR = b"UNPROGRAMMED"
```

correct for the 2026-08-16 file, where that word appears only in the host-written
notes. **In the 2026-08-18 capture it occurs 21 times ABOVE the divider** — it
is a verdict word the capture script itself prints, `UNPROGRAMMED-OR-ABSENT`,
once per dead page.

So running arm 3 against the new file would have rewritten 21 bytes of frozen
machine output and then asserted the digest did not move. The arm would have
failed — loudly, but for entirely the wrong reason. And the tempting fix at
that point ("this anchor doesn't work for that file, skip it") is exactly the
narrowing §3a just got caught doing, arrived at from the other side.

Both anchors are now verified BY COUNT against every artifact before use:

```python
ABOVE_ANCHOR = b"0x9a68"     # 1 above / 0 below, in BOTH files
BELOW_ANCHOR = b"MACHINE:"   # 0 above / 1 below, in BOTH files
```

`0x9a68` is the PCI device id of the Tiger Lake iGPU: a capture that no longer
contains it is not a capture of this machine, which makes it a good positive
anchor for the frozen half specifically. `MACHINE:` opens the host-written
notes header and the capture script cannot print it. The count assertions run
before each mutation, and their failure messages say which side the anchor
leaked onto and why that breaks the arm — because
`a-mutation-that-matches-nothing-prints-ok` has a quiet direction (zero matches,
arm passes testing nothing) and a loud one (too many matches, arm fails looking
like the subject is broken), and this was the loud one waiting to happen.

### §3c — the expectation table is now per-artifact

Three module globals became a dict keyed by basename, so the table reads as a
record of two boots rather than as a pair of magic numbers:

```python
EXPECTED = {
    "tgl-capture-2026-08-16-022556.txt": {
        "bytes": 3539, "lines": 69, "sha256": "ca500bd0...",
    },
    "tgl-final-capture-2026-08-18-201412.txt": {
        "bytes": 71345, "lines": 1384, "sha256": "30779a10...",
    },
}
```

All three of bytes / lines / sha256 are kept per row because they fail
differently: a digest mismatch says a byte moved, a length mismatch says how
much, and a line-count mismatch distinguishes an in-place edit from an
insertion.

The subject-set generator REFUSES rather than skips on an empty set. A capture
directory that lost its files must redden; an empty loop prints `ok`.

7 checks, up from 5. Arms 1–5 were preserved and converted to loop over both
artifacts with `subTest(capture=name)`.

---

## 4. What the capture answers

### §4a — Q1/Q2, the DSI 0x800 stride: WITNESSED

These were listed as "not merely unanswered but unanswerable on an eDP-only
machine" in handoff15 §2, and that judgement was wrong — not about the machine,
about the method. Nothing on this laptop will ever program a DSI transcoder,
so no DSI transcoder will ever hold a mode. But the two DSI pages at the
suspected 0x800 spacing both hold **identical non-trivial reset state**, and
two pages at a fixed spacing holding the same non-trivial pattern is the
signature of two instances of one block at that stride.

That is weaker than reading a programmed mode out of both and it is much
stronger than nothing. It is the difference between a stride this project
invented and a stride this project witnessed. The dumping of pages nobody
expected to be interesting is what produced it, which is the DUMP-when-in-doubt
rule paying for itself once, at the last possible moment.

### §4b — Q4, PIPESRC `+1`: confirmed a second time

Same convention, same panel, second independent boot. Not new, but a
repeated measurement across boots is worth more than one.

### §4c — Q3, plane count: still unsettleable

An eDP-only single-pipe machine does not tell you how many planes the hardware
has, only how many the driver lit. This one is now permanently open on this
track — see §7 item 1 for the thing that would have fixed it and no longer
can.

### §4d — two reads of the same register disagreed

The capture reads several ranges more than once, and some words differ between
reads inside a single run. The disagreements cluster in two places:
`0x064000` (the DDI buffer block — `DDI_BUF_IS_IDLE` at
`reference/i915/i915_reg.h:3869` is a live status bit, so it SHOULD change) and
the watermark/allocation band around `_PLANE_WM_1_A_0` at
`reference/i915/skl_universal_plane_regs.h:315`.

The rule this forces is the most important single sentence for whoever
harvests constants out of this file:

> **A register read on a running display is a SAMPLE, not a constant.**

Only configuration registers may become `*_OBSERVED_*` constants. Status bits
and watermark counters read a value that was true for one instant on one boot,
and filing one of those beside a real configuration measurement destroys the
distinction the whole `*_OBSERVED_*` convention exists to protect.

### §4e — the reader is proven, so the nulls mean something

Transcoder A's HTOTAL read `0x081F077F`, which decodes to the panel that was
lit at the time. That is the positive control: if it had read dead, the mapping
would be broken and every null in the file would downgrade to INCONCLUSIVE.
It did not, so ABSENT continues to mean UNPROGRAMMED (handoff15 §2 — dead pages
on this silicon read `0x00000000`, not all-ones), and the 718 live dwords are
real reads of real registers.

---

## 5. Gate readings

Every reading, not just the final one — a gate's verdict in a two-agent tree is
a joint measurement.

| Gate | Reading |
|---|---|
| `tools/check-registers.py` | **240 / 240**, unmoved and correctly so — a capture carries no `#define`s |
| `tests/test_capture_integrity.py` alone | 5 → **7**, green |
| `tests/test_igpu_mmio_ranges.py` alone | **35**, green, untouched this session |
| `tests\run-python-tests.bat` (full) | **RED, then GREEN** — 954 tests, exit 0. See §5a |
| `tests\check-kext-syntax.bat` | **RED, then GREEN** — exit 0. The red was MINE. See §5b |
| `tests\build-msvc.bat` | **9 / 9 suites**, 100% passed, 0 failed |

The red:

```
test_every_shipped_file_is_byte_identical_to_the_tree
  (test_guest_build_kext.PayloadFreshnessTests)
AssertionError: Lists differ:
  ['phase0-kext/NV3050Probe.cpp', 'phase0-kext/NVGspBoot.hpp'] != []
```

Attributed by mtime: `build-payload/` was built 14:29:11–14:29:12; the peer's
`phase0-kext/NV3050Probe.cpp` is 16:39:04 and `phase0-kext/NVGspBoot.hpp` is
16:45:23, both AFTER the payload. Their `3050-handoff28.md` at 16:10:54 shows
they were mid-session.

**I did not rebuild `build-payload/`, deliberately.** Doing so would green a
gate by vouching for a peer's half-landed shipping-code edit under this
session's name — the same call intel-handoff27 §5a made and the same one
handoff18 §3 made before it. Attribute by mtime and by the failure MESSAGE,
then touch nothing.

### §5a — and then it went green underneath me

A later reading of the same gate: **954 tests, exit 0.** Nothing I did fixed
it. `build-payload/kext-src.tar` is now stamped **16:47:54**, which is after
both of the peer's edits (16:39:04 and 16:45:23) — they finished their session
and rebuilt their own payload.

That is the third session running where waiting was the correct action, and it
is worth stating as the rule rather than as an anecdote: **in a two-agent tree,
a red gate naming files you did not touch is a measurement of somebody else's
work in flight.** Rebuilding would have produced an identical green with this
session's name on a payload containing a peer's half-landed edits. The
distinguishing evidence is always the same three mtimes — payload, each named
file, and the peer's own handoff — and it costs one `ls`.

My own two modules were green in isolation throughout, which is what made
waiting safe rather than merely patient: `test_capture_integrity` 7/7 and
`test_igpu_mmio_ranges` 35/35, both before and after the peer's rebuild.

### §5b — the syntax gate went red on PROSE, and that red was mine

`tests\check-kext-syntax.bat` exited 1 on:

```
FAIL: CLAUDE.md:85: tgl-capture-*.txt: is a pattern that matches no files --
the sentence describing what it holds is no longer true of this repository
```

No code was involved. `tools/check-documented-paths.py` resolves every
path-shaped token in `CLAUDE.md` against the checkout, and my new read-order
entry described §3a's defect by spelling the OLD glob literally — a
wildcard-plus-extension token that resolves to nothing, because the whole point
of §3a is that it matches no file worth matching.

This is the **third** occurrence of that trap in this file's history: the
SDK-include note and the backslash-and-slash regex note above it are the other
two, and both are already written in words for exactly this reason. The fix is
the same one they took — describe the pattern in prose rather than spelling it
— and the durable form of the rule is that **a gate reading prose cannot tell a
quoted counter-example from a claim.** Anything in `CLAUDE.md` that names a
path in order to say it is WRONG has to be said without writing the path.

One reading note that has now cost time twice: `grep -c FAIL` on that gate's log
returned **4** on the passing run, because section 11 is TITLED "negative
control -- each defect must FAIL" and its control names are printed. Read the
exit code, and grep for `FAIL:` with the colon if you must grep at all.

---

## 6. What this session does NOT establish

1. Nothing in `phase0-kext/` was executed, compiled or changed. The Intel
   decode layers are exactly as tested as they were at the end of
   intel-handoff27.
2. No constant was harvested from the new capture. The file is evidence in the
   tree with a hash over it; turning any of its 718 dwords into an
   `*_OBSERVED_*` `#define` is future work, and §4d is the discipline that work
   must follow.
3. The DSI stride is witnessed as a RESET-STATE pattern at a spacing, not as
   two programmed transcoders. Do not upgrade that sentence when citing it.
4. `tools/igpu-final-capture.sh` still has 295 lines of Python inside it with
   **no gate over the embedded half** — intel-handoff27 §4's honest gap, and it
   is unchanged. It matters less now that the script has run and will never run
   again, but the three-table drift relation in `tests/test_igpu_mmio_ranges.py`
   still reads that heredoc, so a bad edit there is still reachable.
5. The capture-integrity gate proves the BYTES have not moved. It says nothing
   about whether the numbers in them were read correctly — that is what §4e's
   positive control is for, and a positive control is an argument, not a proof.

---

## 7. Open list

1. **Expired and worth recording as expired.** intel-handoff27 §7 item 5 was
   "attach an external display BEFORE the one boot" — it would have made Q3
   answerable on a second pipe and converted several policy constants into
   measurements. The boot has happened. This item is now permanently
   unreachable on this machine, and it is the single largest piece of evidence
   this track will never have.
2. **Highest value, not blocked on hardware:** harvest configuration registers
   from `igpu-results/tgl-final-capture-2026-08-18-201412.txt` into
   `phase0-kext/IntelDisplay.hpp` as `*_OBSERVED_*` constants, citing the file
   and following §4d strictly. This is the first time the Intel track has had
   enough silicon evidence to do this at scale.
3. Cross-check the harvested values against `reference/i915/` before filing
   any of them. A value that agrees with the vendored header AND with the
   silicon is worth more than either alone; a value that disagrees is the most
   interesting thing in the file and must be written down as a disagreement
   rather than resolved silently in favour of one side.
4. intel-handoff26 §6 item 4 is still open: the publisher-shape gate spells its
   predicate against `regRead32(` by name, so a second MMIO accessor added
   later produces a method the gate does not consider a publisher at all — and
   it reports nothing rather than failing, which is the vacuity direction
   again. Same family as §3a above.
5. A gate over the embedded Python in `tools/igpu-final-capture.sh` (§6 item 4).
6. One `kextload` on a machine containing the card remains the highest-value
   action in the project overall, and remains blocked on the operator. Note
   that per `3050-handoff27.md` the laptop cannot hold Linux and macOS at once,
   and as of §0 the Linux side is finished — so that constraint has resolved in
   favour of the macOS install.

---

## 8. One-paragraph summary for whoever reads this next

Linux is done on the target laptop and can be overwritten. Two boots produced
two captures, both in `igpu-results/`, both hashed. The first run of the final
script failed on Secure Boot lockdown refusing an mmap of the BAR, which was
diagnosable only because the script printed its own lockdown state before
attempting the map; the operator disabled Secure Boot and the second run
returned 718 live dwords across 25 ranges. The DSI 0x800 stride is witnessed
for the first time. The gate protecting these irreplaceable files was guarding
only the older and smaller of them, because its glob did not match the newer
name and because its below-divider mutation anchor is a word the new capture
prints 21 times above the divider — both fixed, with two new arms making the
widening load-bearing. No shipping code changed. The next real work is
harvesting configuration registers out of that capture, and §4d is the rule
that decides which of the 718 words are allowed to become constants.
