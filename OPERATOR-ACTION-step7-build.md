# OPERATOR ACTION — one power cycle attempts STEP 7

**Filed 2026-08-13.** This is the second operator card. The other one
(`OPERATOR-ACTION-guest-creds.md`) asks for the account password and unblocks
every `vmrun` guest operation. **This card does not need the password.** It
needs a power cycle and one command typed at the guest console.

---

## STATUS 2026-08-14 — STEP 7 IS ANSWERED. The procedure below is superseded.

The build ran and it succeeded: `make exit status: 0`, bundle PRESENT, under
Apple clang version 17.0.0 (clang-1700.0.13.5) on macOS 15.7.9 (24G830). The
full log is `step7-results/step7-build-SUCCESS.log`. The earlier failed
attempt is kept alongside it as `step7-results/step7-build.log`, because the
contrast between the two is the finding.

Everything below this banner is the procedure that was current *before* the
guest had the host's SSH key. Three of its premises are no longer true:

| What the procedure below assumes | What is true as of 2026-08-14 |
|---|---|
| a human types a command at the guest console | the host drives it over `ssh -p 2222 kenny@127.0.0.1`, unattended |
| there is no credential-free guest to host file path | `scp -P 2222` pulls the log back; the host's public key is installed, see `OPERATOR-ACTION-guest-key.md` |
| repointing the CD is what stands between here and a build | the guest had **no toolchain at all** |

The third row is the one worth keeping. The delivery channel this card is
about was never the blocker. The first attempt reached the guest exactly as
designed and still produced nothing, because a fresh macOS install has no
compiler: `xcode-select` errored, and the log said `Kernel.framework: ABSENT`.
The actual missing step was `xcode-select --install`, which no amount of
getting the source into the guest would have supplied.

---

## What this gets you

STEP 7's first half: **does the kext source compile with Apple clang.**

That has never been answered. `CLAUDE.md` is explicit that the four host-side
gates do not cover it — they compile the *decode logic* under MSVC and *parse*
the IOKit shell with clang's front end. Neither one is a kext build. The
specific things still unproven are the `-mkernel -fapple-kext` flags, the
`OSDeclareDefaultStructors` / `OSDefineMetaClassAndStructors` macro expansion
against the real `libkern`, and every IOKit API call in `NV3050Probe.cpp`
against the real headers rather than `tests/kextshim/`'s stubs.

It does **not** load the kext, and loading it in this VM would prove nothing
about the target laptop anyway — the VM has no GA107 to probe. The claim on
offer is exactly one: the source compiles, or here is the error.

---

## What you have to do

> **UPDATED 2026-08-14 15:30 — the guest is RUNNING, but its disc is STALE.
> Step 1 (shut down) and step 3's boot are work to do again; step 2's repoint is
> still good.**
>
> An earlier 2026-08-14 revision of this line said steps 1, 2 and the boot were
> all done and only the console command remained. That was true when written and
> is not true now: `build-payload/step7-payload.iso` was rebuilt at 15:30 to ship
> a fix to `tools/guest-build-kext.sh`, and a running VM does not re-read its CD.
> The guest still holds the pre-fix image, so **running the console command
> without a power cycle would build the old script** — the one that reports
> `make exit status: 0` for a failed compile. Details in the rebuild note below.
>
> Step 2 does not need redoing: the VMX already points `sata0:2` at that exact
> path, and the rebuild replaced the file at the path rather than moving it.
>
> Everything the 2026-08-13 19:00 revision of this block said about power state
> is now false, and it was written minutes before the fact that falsified it.
> That revision reported `vmrun list` at `Total running VMs: 0` and no
> `vmware-vmx.exe` in `tasklist`, and concluded the guest was off. It was, at the
> time it looked. Three minutes later the VM was powered on, and the block was
> never revised. **Re-measure power state at the moment you act on it; do not
> read it out of this card.**
>
> Measured 2026-08-14: `vmrun list` reports `Total running VMs: 1` and names
> this VMX. The `vmware.log` that "stopped mid-session at 2026-08-13T20:08Z" did
> not stop — it now runs to 2070 lines, with a fresh `PowerOn` at
> 2026-08-13T23:03:45Z (19:03:45 local) and entries continuing to
> 2026-08-14T18:50Z.
>
> **The step 2 repoint did survive, because it happened BEFORE that power-on.**
> This is the one thing worth being careful about: an edit to a live VMX is
> silently discarded, so "the file on disk says `step7-payload.iso`" is not by
> itself evidence the running guest sees it. Two log facts settle it, and neither
> is the file on disk. The power-on `DICT` dump — the config VMware actually
> loaded — carries
> `sata0:2.fileName = ...build-payload\step7-payload.iso`, so the repoint was in
> the config at boot. And one second later the emulator reports
> `CDROM: Connecting sata0:2 to '...build-payload\step7-payload.iso'. type=2
> remote=0`, which is the host side opening that exact image for this VM. The ISO
> is also older (2026-08-13 19:00:49) than the power-on and unchanged since, so
> the image being read is the one on disk.
>
> **SUPERSEDED 2026-08-14 15:30 local — the last sentence above is no longer
> true.** The image on disk was REBUILT at 15:30 while this VM was running, so
> the disc the guest currently has mounted and the file at
> `build-payload/step7-payload.iso` are now two different images. Nothing was
> corrupted: the host emulator opened the CD once, at power-on
> (`CDROM: Connecting sata0:2`, 2026-08-13T23:03:46Z), and the last CDROM entry
> in `vmware.log` is 23:07:36Z — there is no re-read after 15:30, so the running
> guest simply still holds the OLD image. **The consequence is that a build run
> from the currently-mounted disc would compile the pre-fix script.** The power
> cycle in step 2 below is what makes the guest pick up the new one, and it is
> now mandatory rather than optional.
>
> Why it was rebuilt: `tools/guest-build-kext.sh` captured `$?` after a
> `make ... | tee -a` pipeline, which is `tee`'s status, not `make`'s. A failing
> compile therefore printed `make exit status: 0` and exited 0 — the worst
> possible outcome for a script that gets one run in front of an operator reading
> the result off a screen. POSIX `sh` has no `pipefail`, so the status is now
> carried out of the subshell through a scratch file, and an unrecoverable status
> is treated as failure rather than success.
>
> Rebuilt AGAIN at 16:23 by a peer terminal, and a THIRD time at 16:4x by the
> lead, which is why the numbers below differ from the 15:30 and 16:23 ones
> earlier revisions of this block carried. The kext tar has been byte-identical
> across all of them (`84c0a4e720adae61ff3818818c1bc725`, 204800 bytes), so
> nothing about the source under test has changed even once — every rebuild moved
> the guest-side script only.
>
> The third rebuild's cause: `guest-build-kext.sh` had **no argument parser at
> all**. It read `NV_PAYLOAD_ROOT`/`NV_WORK_DIR` and nothing else, so
> `--payload-root /tmp/x` was accepted in total silence and the run went on
> searching the default root — an option that appears honoured, reports success,
> and describes a different run than the one you asked for. It now parses both
> flags, and an unknown argument is **refused with exit 2** rather than ignored,
> because a warning would scroll off the guest console above several screens of
> build output.
>
> The consequence for you that these rebuild notes kept repeating — "the running
> guest holds neither image, so the power cycle remains mandatory" — **no longer
> applies.** See the banner at the top of this card: the build has been done, over
> SSH, with no power cycle and no disc at all.
>
> A FOURTH rebuild followed, for a reason worth reading: the script's own
> `Kernel.framework: ABSENT` message told the operator to install full Xcode from
> the App Store. That advice was false, and false in the direction that costs a
> multi-gigabyte download for nothing — see the corrected bullet under step 4.
>
> A FIFTH rebuild followed on 2026-08-15, and it is the first one that changed
> `kext-src.tar` rather than only the script. `phase0-kext/IntelDisplay.hpp` was
> added -- the Gen12 display-engine decode header -- so the sentence these notes
> carried through four rebuilds, "the kext tar has been byte-identical through
> every rebuild", is **no longer** true and is recorded here as superseded rather
> than deleted. The tar grew 204800 -> 215040 bytes. Nothing else about the
> payload changed: the script is the same 12677 bytes it was after the fourth
> rebuild.
>
> A SIXTH rebuild followed later the same day, 2026-08-15, for the same reason:
> `phase0-kext/IntelDisplay.hpp` gained the Gen12 PLANE layer -- `igPlaneReg()`,
> the PLANE_CTL field decodes, and the raw PLANE_SIZE accessors -- so the tar
> grew again, 215040 -> 225280 bytes. The script is still the same 12677 bytes.
> Two tar-changing rebuilds inside one day is the thing worth recording, not a
> footnote to it: every byte size and md5 quoted in this block goes stale the
> moment anything under `phase0-kext/` is edited, and the only things that will
> say so are `tests/test_guest_build_kext.py` and
> `tests/test_operator_step7_doc.py`.
>
> A SEVENTH rebuild followed on 2026-08-16, and it is the first one that changed
> the tar's CONTENT without changing its SIZE. `phase0-kext/IntelDisplay.hpp`
> gained the DECODED `PLANE_SIZE` wrappers -- `igPlaneSizeWidth()` and
> `igPlaneSizeHeight()`, added on the strength of the 2026-08-16 Tiger Lake
> capture that settled the -1 convention -- and `kext-src.tar` came out at 225280
> bytes again, because tar rounds its total up to a 10240-byte block boundary and
> the header change fit inside the existing padding. The image likewise stayed
> 303104 bytes.
>
> That is the case worth recording rather than the rebuild itself: two of the
> three derived-value tests in `tests/test_operator_step7_doc.py` -- the script
> size and the tar size -- PASSED against a card describing the PREVIOUS image,
> and only the md5 test failed. A byte size is not a fingerprint, so a card that
> quotes only sizes can be stale and green at the same time. Anything checking
> this payload should compare md5 or bytes, never size alone.
>
> An EIGHTH rebuild followed on 2026-08-16, and it is the only one so far whose
> cause was a defect in the BUILDER rather than a change in the payload. The
> seventh rebuild's md5 could not be kept current by editing this card, because
> the image was not byte-reproducible: two rebuilds from byte-identical inputs
> came out 303104 bytes both times and differed in **34 bytes across 28 runs**,
> while `kext-src.tar` stayed identical. Every differing byte was an ISO-9660 date
> field -- `0x24` (36) vs `0x36` (54) in the seconds field of binary directory
> records, and the ASCII `'36'`/`'54'` at offset 12 of the PVD date strings.
> pycdlib reads the wall clock itself -- five `time.time()` calls in its
> installed volume-descriptor and top-level modules -- and encodes it through
> `time.localtime()` in its date-record module, so the image also depended on the
> host timezone. The exact module names and line numbers are cited in
> `tools/deliver-via-cd.py` next to the pin, and deliberately not repeated here:
> they are paths inside the installed pycdlib package rather than files in this
> checkout, and `tests/test_documented_paths.py` cannot tell those apart.
>
> `tools/deliver-via-cd.py` now pins both names for the duration of the write
> (`pinned_iso_clock()`, `PINNED_ISO_EPOCH` = 2026-08-16T00:00:00Z, with
> `localtime` pointed at `gmtime` so the stored GMT offset is 0 as well). That is
> what makes the md5 below a fingerprint rather than a timestamp: verified by two
> consecutive `python tools\build-step7-payload.py --skip-gate` runs hashing
> identically. Before the pin, `test_every_md5_the_card_quotes_can_be_accounted_for`
> could only be satisfied until the next rebuild, however carefully this card was
> edited -- so the fix belonged in the generator, not in the prose or the
> assertion.
>
> Current image, measured 2026-10-12 after the FIFTY NINTH rebuild: md5
> `3183e58ebf1c817a80a46521874106a3`, 927744 bytes, carrying `kext-src.tar`
> (849920 bytes, md5 `4c9772c2e7a251038306a1740c4d8f71`) and
> `guest-build-kext.sh` (**12677** bytes, md5
> `599b51eb31500c9ef9ff4b1f2dee0476`; was 11538, was 8115,
> was 7596, was 6718) under vol ident `PAYLOAD`.
>
> The fifty ninth rebuild, 2026-10-12, moved both md5s and NEITHER byte size
> (tar 849920, ISO 927744, unchanged). Cause: `phase0-kext/IntelGtExec.hpp`
> was corrected against `reference/i915/` -- PTE address mask 47:12 -> 45:12
> (`intel_gtt.h:100`) and software context id bit 32 -> 37
> (`intel_lrc.h:103`). Same file count, a few hundred bytes of change that fit
> inside the existing tar blocks. Reproducible: two consecutive
> `python tools\build-step7-payload.py --skip-gate` runs hashed identically
> (sha256 prefix `5557aaef40010e51` both times). Kext binary behaviour is
> unchanged: the probe publishes the descriptor but writes nothing.
>
> Fifty eighth image (superseded): md5 `d853aba3fd2b82bfaa8072079dd19735`,
> 927744 bytes, tar md5 `99209a3b0ef435eb13257d41fff2cdad`.
>
> The fifty eighth rebuild, 2026-10-12, moved both md5s AND both byte sizes --
> tar 819200 -> 849920, ISO 897024 -> 927744, exactly THREE 10240-byte tar
> blocks each, read off the disk in one measurement. Reproducible: two
> consecutive `python tools\build-step7-payload.py --skip-gate` runs hashed
> identically (sha256 prefix `cad34f2a5f12978f` both times). The script is
> unchanged at 12677 bytes.
>
> What changed is ONE new dependency-free header,
> `phase0-kext/IntelGtForcewake.hpp` -- the GT power-well wake layer, the first
> prerequisite for ever writing to an engine. Nothing in the shipping kext
> includes it yet, so **the kext binary this image builds is UNCHANGED** from the
> fifty seventh's; the header rides in the tar so the guest build sees the same
> tree the host gates do. See `intel-handoff43.md`.
>
> Fifty seventh image (superseded): md5 `2792a35bc56fc5514e532e43ead4d2f6`,
> 897024 bytes, carrying `kext-src.tar` (819200 bytes, md5
> `ec7bc97fe1eeed144fd541026a44aef1`). Its notes follow and stay as written.
>
> The fifty seventh rebuild, 2026-10-12, moved both md5s AND both byte sizes --
> tar 808960 -> 819200, ISO 886784 -> 897024, exactly ONE 10240-byte tar
> block each, read off the disk in one measurement (`len()` and
> `hashlib.md5()` over the same bytes). Reproducible: two consecutive
> `python tools\build-step7-payload.py --skip-gate` runs hashed identically.
> The script is unchanged at 12677 bytes.
>
> What changed is ONE new dependency-free header, `phase0-kext/IntelSinkRehearse.hpp`,
> and the FIRST shipping consumer of the sink headers: `IntelDisplayProbe.cpp`
> now calls `publishSinkRehearsal()` from `start()`. It reads plane 1 on pipe
> A (CTL, STRIDE, SIZE, SURF, AUX_DIST) and BAR2's length, composes the
> alloc plan and the plane words a flip WOULD write, and publishes them as
> `IGSinkRehearsal*`, `IGSinkPlan*` and `IGSinkPlane*`. **The kext binary this
> image builds is therefore CHANGED**, unlike the fifty sixth. It still writes
> ZERO registers: `IGSinkRehearsalWroteRegisters` is the literal `false`, and
> `tests/test_intel_sink_rehearsal_shape.py` reddens on any write path in the
> method. What to read on hardware is in `intel-handoff42.md` section 4.
>
> Fifty sixth image (superseded): md5 `349da2f2868ff9a5ab82c3dbabcddbcc`,
> 886784 bytes, carrying `kext-src.tar` (808960 bytes, md5
> `777c03a2487fbfa4d09ac21289dc5866`). Its notes follow and stay as written.
>
> The fifty fifth rebuild moved both md5s AND both byte sizes -- tar 757760 ->
> 778240, ISO 835584 -> 856064, exactly TWO 10240-byte tar blocks each. Both
> properties were read off the disk in the same measurement (one command reading
> the size and the md5 off the same file), which is the forty fifth block's
> correction still applied.
>
> What changed is ONE new dependency-free header and nothing else:
> `phase0-kext/IntelGtReset.hpp`, the RECOVERY layer. It is the tenth GT header
> and the first that decides the driver should STOP waiting -- every layer
> beneath it composes work, delivers it, or reports its completion, and none of
> them answers what happens when that report never arrives. Polling forever is
> not a decision; it is a hang wearing the costume of one.
>
> The fifty sixth rebuild, 2026-10-11, moved both md5s AND both byte sizes --
> tar 778240 -> 808960, ISO 856064 -> 886784, exactly THREE 10240-byte tar
> blocks each, read off the disk in one measurement (`len()` and
> `hashlib.md5()` over the same bytes). Reproducible: two consecutive
> `python tools\build-step7-payload.py --skip-gate` runs hashed identically.
> The script is unchanged at 12677 bytes.
>
> What changed is FOUR dependency-free headers, the Intel side of the 3050
> sink: `phase0-kext/IntelSinkSurface.hpp` (is a proposed sink a legal
> scanout surface), `phase0-kext/IntelSinkAlloc.hpp` (where in the GGTT it
> goes), `phase0-kext/IntelSinkPublish.hpp` (the descriptor the 3050 aims a
> DMA copy at) and `phase0-kext/IntelPlaneProgram.hpp` (the plane register
> values that flip scanout onto it). None is included by a shipping `.cpp`
> yet, so the kext binary this image builds should be unchanged; they ride
> along so the guest tree matches the host tree. The fifty fifth image was
> also already stale before them: its copy of `phase0-kext/IntelGtProbe.hpp`
> differed from the committed tree, which this rebuild also corrects.
>
> The shell is UNCHANGED this rebuild. `IntelGtReset.hpp` has no caller in
> `phase0-kext/IntelGtProbe.cpp` yet, so the payload carries a header the kext
> compiles and does not consult. That is deliberate and it is the honest state:
> the header and its suite are real, the consumption is not written.
>
> `tests/test_intel_gt_reset.cpp` (**139** checks, 0 failures, first reading
> 2026-08-30) drives it on Windows, and before that reading the header and the
> suite were both on disk registered in NOTHING -- no CMake target, no
> syntax-gate line, no entry in `tests/test_check_count_last_line.py` and none in
> `tests/test_check_count_floor.py`. A suite that is not registered is a suite
> nobody runs, which is `intel-handoff26.md` section 1's measurement one level up
> from a header with no suite.
>
> The reset composition WRITES NOTHING to hardware, same as every layer beneath
> it. This layer DECIDES that a reset is warranted; performing one is a
> different act and it is not in this tree.
>
> The fifty fourth block below is the previous reading and stays as written.
>
> Fifty fourth image (superseded): md5 `91edf402f853a474edc599a8c077847c`,
> 835584 bytes, carrying `kext-src.tar` (757760 bytes, md5
> `86772c84463029b155db36a7801d7090`). That rebuild moved both md5s AND both
> byte sizes -- tar 727040 -> 757760, ISO 804864 -> 835584, exactly THREE
> 10240-byte tar blocks each. What changed was the SHELL plus one new
> dependency-free header: the COMPLETION layer,
> `phase0-kext/IntelGtFence.hpp`, with `composeFence()` and
> `publishComposedFence()` in `phase0-kext/IntelGtProbe.cpp` consuming it.
> `tests/test_intel_gt_fence.cpp` read **73** checks that day; it reads 138 now,
> because three call sites in its address round-trip group passed two half-words
> to a join taking a span pointer and a count, so the group did not compile and
> the 73 was measured on a tree without it.
>
> The fifty third block below is older still and also stays as written.
>
> Fifty third image (superseded): md5 `5dfe86ef5c171a47fccb528072d3ed0f`,
> 804864 bytes, carrying `kext-src.tar` (727040 bytes, md5
> `9091a49fbd4821a692600008d07ef718`). That rebuild moved both md5s AND both
> byte sizes -- tar 706560 -> 727040, ISO 784384 -> 804864, exactly TWO
> 10240-byte tar blocks each. What changed there was the SHELL rather than a header:
> `phase0-kext/IntelGtProbe.cpp` and `.hpp` both grew, because the EXECLIST
> layer now has a caller. `rehearseSubmit()` reads `EXECLIST_STATUS`, composes a
> context descriptor over `IG_GTP_LRCA_POLICY`, and judges the whole submission
> through `igGtxSubmitReady()`; `publishSubmitRehearsal()` puts the verdict, its
> name, the descriptor and its validity into the IORegistry. Two clang errors
> were cleared on the way -- both ARITY, both name-and-signature defects of the
> class only this gate can find: `igGtbWriteColorBlit()` was called with ten
> arguments where eleven are required (`depthFlag` missing) and
> `igGtxSubmitReady()` with seven where eight are (`lengthBytes` missing). Both
> new constants are COMPOSED rather than spelled --
> `IG_GTP_BLIT_DEPTH_POLICY` from `IntelGtBlt.hpp`'s pre-shifted
> `IG_GTB_DEPTH_32_I915_0x03000000`, and `IG_GTP_SUBMIT_LENGTH_BYTES_POLICY`
> from `IG_GT_MI_BB_START_DWORDS_POLICY_3 * IG_GT_CMD_WORD_BYTES_I915_4`.
>
> Still no MMIO write path in the file. `igGtxExeclistSqReg()` and
> `igGtxExeclistControlReg()` -- the two accessors that would actually SUBMIT --
> do not occur in the shell at all, and `IGGtSubmitted` is still published false.
>
> The fifty first block below is the previous reading and stays as written.
>
> Fifty first image (superseded): md5 `0dc332a2e86ed14264636e8fd7b55d2a`,
> 774144 bytes, carrying `kext-src.tar` (696320 bytes, md5
> `80ace1f91a391eac706c3ef5f650918c`). That rebuild moved both md5s AND both
> byte sizes -- tar 675840 -> 696320, ISO 753664 -> 774144, exactly TWO
> 10240-byte tar blocks each, two rather than one because it added a FILE rather
> than editing one in place: `phase0-kext/IntelGtRing.hpp`, the WRAPPING STORE
> above the batch join. It was on disk before that session and had never been
> compiled by anything: no suite drove it, no CMake target built it, no shell
> called it. `tests/test_intel_gt_ring.cpp` now does (331 checks).
>
> Two blocks rather than one because this rebuild added a FILE rather than
> editing one in place: `phase0-kext/IntelGtBatch.hpp`, the batch-buffer JOIN
> between the packet layer and the delivery layer. `IntelGtBlt.hpp` composes what
> the engine would DO; `IntelGtCmd.hpp` composes the ring geometry and the MI
> commands that DELIVER it. Nothing joined the two, so a caller holding a composed
> blit had no admissible way to ask where it goes, what terminates it, and what
> the ring must hold for the engine to find it.
>
> It is also the first header in `phase0-kext/` with TWO includes rather than one,
> and both are quoted siblings, so `-I phase0-kext` still parses it standalone and
> `check-header-coverage.py` -- which derives its exemption from the presence of an
> ANGLE-BRACKET include -- expects it in the syntax gate rather than exempted.
>
> Its verdict prefix is `IG_GTQ_` rather than `IG_GTB_` deliberately.
> `intel_verdict_set_checks()` in `tools/check-registers.py` groups verdict
> families BY PREFIX, so a family sharing the blitter's prefix would sweep the
> blitter's constants into this one's contiguity check -- handoff16 section 1's
> `IG_PIPE_TRANSCONF` trap, a failure correct about its own arithmetic and wrong
> about the world. The register gate is correctly unmoved at **260**: neither
> header is in its parsed set, so `IG_GTQ_` gets no set-check there and the
> verdict-name loop in `tests/test_intel_gt_batch.cpp` bounds on
> `IG_GTQ_BATCH_LIMIT` instead, so a family that grows reddens on the commit that
> grows it.
>
> What the rebuild does NOT mean is that anything new can run. No batch this
> header assembles has ever been submitted to an engine, there is no ring, no
> context and no GGTT binding, and `IntelGtProbe` still writes zero registers.
> What a green `tests/test_intel_gt_batch.cpp` (**234 checks, 0 failures**) says is
> that the composition is self-consistent and refuses what it claims to refuse --
> not that the command streamer would accept it. Every constant in the header is
> POLICY and it says so: `reference/i915/` carries no batch-assembly file in this
> tree, and `igpu-results/` captured a machine whose render engine was IDLE, so
> there is nothing about batch assembly to observe. Nothing here is `_OBSERVED_`.
>
> Two blocks rather than one because this rebuild added a FILE rather than
> editing one in place: `phase0-kext/IntelGtBlt.hpp`, the Gen12 2D blitter packet
> composer, which is the first GT header in this tree describing GPU WORK rather
> than the plumbing that would deliver it. It is also the first GT header with a
> CITED half -- `reference/i915/intel_gpu_commands.h` carries `XY_COLOR_BLT_CMD`
> (0x50), `XY_SRC_COPY_BLT_CMD` (0x53), `COLOR_BLT_CMD` (0x40), `INSTR_BC_CLIENT`
> (2) and the ROP, depth and write-mask flags, so unlike `IntelGtExec.hpp`,
> `IntelGtContext.hpp` and `IntelGtImage.hpp` its command encoding is not policy
> end to end.
>
> What the rebuild does NOT mean is that anything new can run. No packet this
> header composes has ever been submitted to an engine, there is no ring, no
> context and no GGTT binding, and `IntelGtProbe` still writes zero registers.
> The header was already on disk before this session and was NOT edited here --
> what changed is that `tests/test_intel_gt_blt.cpp` now compiles against it and
> reads 213 checks, so the tar is being rebuilt to carry a file the previous
> image predates rather than to ship a change.
>
> The forty ninth rebuild moved both md5s AND both byte sizes -- tar 634880 ->
> **727040**, ISO 712704 -> **804864**, nine 10240-byte tar blocks rather than the
> usual one or three, because `phase0-kext/` gained the submission-rehearsal half
> of `IntelGtProbe` plus `IntelGtExec.hpp` since the forty eighth image rather
> than having one member edited in place. Both properties were read off the disk
> in the SAME measurement (`md5sum` and `stat` in one command), which is the forty
> fifth block's own correction still applied: a byte size is not a fingerprint and
> measuring one property of a file is not measuring another. Tar
> `f3358804...` -> `3c0ce0fcf24505638a6d9b6b34734943`; ISO `d0b0e1cc...` ->
> `b5834230ec24b03bba68c3f79c8cad2e`. Every md5 above this block is SUPERSEDED.
>
> What the rebuild does NOT mean is that anything new can run. `IntelGtProbe`
> still writes ZERO registers. What landed is the first structural observer over
> the submission rehearsal: `tests/test_intel_probe_submit_shape.py`, **32 tests**,
> in the idiom of its three siblings -- one `audit()` shared with the live tree,
> comment-stripped source, and thirteen negative controls each asserting WHICH
> property broke. Its sharpest property is one no sibling has: the two execlist
> registers that would SUBMIT -- the submit queue at `0x510` and
> `EXECLIST_CONTROL` at `0x550` -- must never even be ADDRESSED, because
> `igGtxExeclistSqReg()` and `igGtxExeclistControlReg()` exist and are tested in
> `IntelGtExec.hpp`, and a shell that composed either offset would be one store
> away from submission while publishing `IGGtSubmitted` false.
>
> The forty eighth rebuild moved both md5s while NEITHER byte size moved -- tar
> held at 634880 and ISO at 712704, with tar `eaf09939...` -> `f3358804...` and
> ISO `08afb09e...` -> `d0b0e1cc...`. Both properties were read off the disk in
> the same measurement (`md5sum` and `ls -l` in one command), which is the forty
> fifth block's correction still applied. This is the padding case again and it
> is the reason a byte size is not a fingerprint: the edit renamed five constants
> and rewrote a comment block in `phase0-kext/IntelGtCmd.hpp`, and tar pads every
> member to a 10240-byte boundary, so a change of this size lands inside the
> existing padding and ships a different file at an identical length.
>
> What moved is the EVIDENCE class of five constants rather than any value. The
> MI command opcodes in `IntelGtCmd.hpp` were spelled `_POLICY_` on the stated
> grounds that i915's MI command family "lives in i915's gt/ headers, which were
> never vendored here". That was true when written and became false on
> 2026-08-26, when `reference/i915/intel_gpu_commands.h` was vendored alongside
> `intel_lrc_reg.h` and `intel_engine_regs.h`. All four opcode values were
> correct as guessed -- `MI_NOOP` 0x00, `MI_BATCH_BUFFER_END` 0x0a,
> `MI_STORE_DWORD_IMM` 0x20, `MI_BATCH_BUFFER_START` 0x31 -- so nothing about the
> composed command words changed; they stopped being guesses and are now
> `_I915_`-spelled with a cited line each. `tools/check-registers.py` grew a
> fourth GT citation section reading that third header, 253 -> **260** checks.
>
> The forty seventh rebuild moved both md5s AND both byte sizes -- tar 604160 ->
> 634880, ISO 681984 -> 712704, exactly THREE 10240-byte tar blocks each, with tar
> `7b8cef8a...` -> `eaf09939...` and ISO `887da01c...` -> `08afb09e...`. Three
> blocks rather than one because this rebuild added TWO files to the payload
> (`phase0-kext/IntelGtProbe.hpp` and `.cpp`) rather than editing one in place --
> the first Intel GT rebuild to grow the file COUNT. Both properties were read off
> the disk in the same measurement, `md5sum` and `stat` in one command, which is
> the forty fifth block's own correction applied: measuring one property of a file
> is not measuring another.
>
> What landed in it is the first IOKit consumer of the GT stack. Five GT headers
> and sixteen suites existed before this image and nothing called any of them from
> a translation unit that reaches hardware. `IntelGtProbe` maps BAR0, samples the
> render ring's four registers ONCE, composes a logical ring context image into a
> member array, publishes both, and **submits nothing** -- there is no write path
> in the file at all, because `reference/i915/` has no LRC file and the page
> layout is this project's choice end to end.
>
> The forty sixth rebuild moved both md5s AND both byte sizes -- tar 593920 ->
> 604160, ISO 671744 -> 681984, exactly one 10240-byte tar block each, with tar
> `86fec920...` -> `7b8cef8a...` and ISO `01ae1207...` -> `887da01c...`. Both
> properties were read off the disk in the same measurement this time rather than
> one being carried forward, which is the forty fifth block's own correction
> applied: measuring one property of a file is not measuring another.
>
> The forty fifth rebuild moved both md5s AND both byte sizes -- tar 573440 ->
> 593920, ISO 651264 -> 671744, exactly two 10240-byte tar blocks each, with tar
> `0bc35514...` -> `86fec920...` and ISO `cf263cba...` -> `01ae1207...`.
>
> The two sentences above were WRONG when first written and are corrected here
> rather than quietly replaced. This block originally claimed neither size moved,
> because after rebuilding I hashed the two files and did not `ls` them, then
> wrote sizes carried over from the previous block. Both md5s had changed, so
> every hash in the card was correct and only the sizes were stale -- and the two
> numbers sat one line apart in the same sentence, each reading as measured. The
> doc gate caught it in one check
> (`test_the_tar_byte_size_the_card_quotes_is_current`), which is the whole reason
> a derived value in prose gets a gate rather than a careful author: a size copied
> forward is indistinguishable, on the page, from a size read off the disk. The
> lesson is not that sizes are unreliable -- it is that measuring one property of
> a file is not measuring another.
>
> What moved is `phase0-kext/IntelGtContext.hpp`, the Gen12 LOGICAL RING CONTEXT
> image layer directly above `IntelGtExec.hpp`. `IntelGtExec.hpp` composes a
> context descriptor that NAMES an address; this header describes what must sit AT
> that address -- the register-state page the engine restores when it switches to
> a context. `tests/test_intel_gt_context.cpp` drives it under MSVC: **124 checks,
> 0 failures**, and the C++ gate is now 15 suites rather than 14.
>
> Its evidence story is the WEAKEST in this tree and the suite prints that caveat
> above its own count. Greps for LRC, CTX_DESC, RING_BUFFER_START,
> RING_BUFFER_CTL, BB_PER_CTX and CTX_TIMESTAMP across the whole of
> `reference/i915/` return ZERO matches, measured 2026-08-26 -- all seven vendored
> files are display. So unlike `IntelGtCmd.hpp` below, which has a cited half,
> this header has NO cited half at all: every constant in it is POLICY, and 124
> green checks establish that the arithmetic is self-consistent and that the
> refusals fire in the documented order. They establish nothing about Gen12
> silicon.
>
> The header shipped WRONG earlier the same day and was CORRECTED before this
> rebuild, which is the part worth reading. The register-state page opens with a
> NOOP dword at slot 0 and the load-immediate header at slot 1, so the first
> (register offset, value) pair begins at slot 2. The first version placed the LRI
> header at slot 0 and put the four ring-state slots at 0x02/0x04/0x06/0x08 -- one
> pair too low throughout. That does not fault on hardware. It slides every pair
> down by one dword, so the engine reads each register OFFSET out of the previous
> pair's VALUE slot and programs registers nobody chose, which is the same class
> as handoff15's byte-swapped PLANE_SIZE: it programs cleanly and does the wrong
> thing. The four slots are now 0x04/0x06/0x08/0x0A, each pinned to its own bare
> literal and asserted EVEN, since an odd value would name a value dword as though
> it were an offset.
>
> The refusal sentinel is ALL-ONES here, the INVERSE of `IntelGtExec.hpp`, and the
> inversion is load-bearing: zero is a legal slot offset because the leading NOOP
> lives at slot 0, so zero cannot mean "refused" in this family, while in a
> descriptor word zero is illegal by the hardware's own valid bit. The suite pins
> that the sentinel PROPAGATES through `igGtcSlotOffsetFromLrca()` rather than
> having 0x1000 added to it -- adding wraps it to 0xFFF, which is small, 4-byte
> aligned, and inside every containment test in the header: a refusal that reads
> as an answer.
>
> No `*_OBSERVED_*` constant may enter that header either, and none did, for the
> same reason as the header below: `igpu-results/` captured a machine whose render
> engine was idle.
>
> The forty fourth rebuild moved BOTH byte sizes and both md5s -- tar 552960 ->
> 573440 and ISO 630784 -> 651264, tar `97fbc1be...` -> `0bc35514...` and ISO
> `eba10536...` -> `cf263cba...`. Exactly two 10240-byte tar blocks each again,
> which is the padding quantum rather than the size of what was added.
> What moved is `phase0-kext/IntelGtExec.hpp`, the execlist/GTT layer directly
> above `IntelGtCmd.hpp`.
>
> The forty third rebuild moved BOTH byte sizes and both md5s -- tar 532480 ->
> 552960 and ISO 610304 -> 630784, tar `79188866...` -> `97fbc1be...` and ISO
> `8d6e4096...` -> `eba10536...`. Exactly two 10240-byte tar blocks each, which is
> the padding quantum rather than the size of what was added: the new header is
> under 20 KiB and the tar rounds up to the block. That is why this card compares
> md5s and not sizes -- the size moving here is a coincidence of where the padding
> landed, and three earlier rebuilds moved content with both sizes FIXED. A byte
> size is not a fingerprint in either direction.
>
> The numbers above were also wrong in this card once already, for a reason worth
> recording: `tools/build-step7-payload.py` prints a `currently in ...` listing
> BEFORE it rebuilds, so those are the sizes of the image being replaced. I quoted
> them, and `test_the_tar_byte_size_the_card_quotes_is_current` caught it. Read
> the artifact after the build, not the tool's own preamble.
>
> What moved is `phase0-kext/IntelGtCmd.hpp`, and it is the first header in this
> tree that is not about display at all: Gen12 (Tiger Lake) render-engine COMMAND
> SUBMISSION -- engine MMIO bases, the ring register block, ring geometry
> verdicts, head/tail wrap arithmetic, MI command word composition, and the
> refusals that stand in front of a submission. Dependency-free like the other
> eight logic headers, so `tests/test_intel_gt_cmd.cpp` drives it under MSVC on a
> machine with no Intel GPU: **120 checks, 0 failures**, and the C++ gate is now
> 13 suites rather than 12.
>
> Its evidence story is split, and the header says so in its own comment rather
> than leaving a reader to infer it. The four engine bases and the +0x30 ring
> block ARE citable to `reference/i915/i915_reg.h` -- `RENDER_RING_BASE` at `:289`,
> and `PRB0_BASE` spelled `(0x2030 - 0x30)`, which is what fixes the ring block
> ABOVE the engine base rather than at it. The MI opcode family is POLICY, because
> i915 keeps it in the per-engine subdirectory of its own source tree, which was
> never vendored here. (Spelled in words rather than as the directory name itself:
> `tools/check-documented-paths.py` resolves every path-shaped token in this card
> against the checkout and cannot tell an upstream Linux directory from a missing
> repo one, the same trap as the SDK-include note in CLAUDE.md.) So a green 120
> proves the composition is self-consistent and proves NOTHING about whether the
> hardware wants these words.
>
> No `*_OBSERVED_*` constant may enter that header, and none did. `igpu-results/`
> holds display registers captured from a machine whose render engine was IDLE, so
> this tree has no MMIO capture of a running render engine -- and a policy value
> renamed to look measured destroys the only evidence trail the project has.
>
> This is NOT acceleration and nothing in it moves toward acceleration. Metal
> needs an `IOAccelerator` family plus a userspace driver plugin against private,
> unversioned Apple interfaces, which `PLAN.md` SS0 calls the wall. This is the
> kernel-side command path that would sit underneath such a thing, and not one
> line of it has executed on any machine.
>
> The forty second rebuild moved both sizes by exactly ONE 10240-byte tar block --
> tar 522240 -> 532480 and ISO 600064 -> 610304 -- and the cause is comment volume
> rather than logic. `phase0-kext/IntelFbPresentProbe.cpp` gained two
> `presentable()` guards that are BEHAVIOURALLY INERT, in
> `getPixelFormatsForDisplayMode()` and `isConsoleDevice()`, plus the comment
> blocks explaining why an inert branch was the right call. Neither changes what
> the driver answers; both make the rule "every IOFramebuffer override consults
> presentable() first" true with NO exemptions, which is what makes it gateable at
> all. The alternative was a hand-typed exemption set inside
> `tests/test_intel_fb_present_shape.py`, and a typed exemption set is the anchor
> shape that silently narrows its own subject -- `intel-handoff28.md` SS3.
>
> The second of those two guards was found BY the new gate on its first run, not
> by reading: `isConsoleDevice()` had no guard while three separate prose claims in
> the same two files said every override consulted the verdict. That is the value
> of the module in one reading -- the first defect was found by hand and the second
> was found by the thing built to stop the first recurring.
>
> The forty first rebuild moved both sizes by EIGHT 10240-byte tar blocks each --
> tar 440320 -> 522240 and ISO 518144 -> 600064. Eight blocks is a large move by
> this card's history, and it is accounted for: the Intel track gained a whole
> IOKit shell rather than an edit to an existing one.
>
> What moved is the Intel Gen12 track, and it is the first `IOFramebuffer`
> subclass in this tree. `phase0-kext/IntelFbPresentProbe.cpp` / `.hpp` is a THIRD
> IOKit shell beside `NV3050Probe.cpp` and `IntelDisplayProbe.cpp`, and it gives
> `phase0-kext/IntelFbPresent.hpp` the consumer it never had -- that header's
> whole purpose is joining scanout, pixel and inheritability into one presentation
> verdict, and until now nothing of the right SHAPE called it. The class presents
> an INHERITED scanout: it samples four plane registers once, caches them, and
> writes not one register to the GPU. Every accessor answers out of the cached
> sample or refuses.
>
> Three choices in it are load-bearing. `init()` seeds the four sample words with
> `IG_PRESENT_INVALID32` rather than zero, because `IG_PRESENT_OK` IS zero and an
> initialiser zero reading as the success code is exactly the shape that put three
> fabricated register words into the NVIDIA track's IORegistry. `fSampleTaken` is a
> separate `bool` set AFTER the four reads and BEFORE the verdict, so "nothing was
> sampled" and "the sample came back OK" cannot collide. And `presentable()` is an
> EQUALITY against `IG_PRESENT_OK`, never a negation of an error arm -- a negation
> is true for every verdict this family has not yet been taught.
>
> `publishPresentation()` puts the verdict and the sampled flag FIRST and
> unconditionally, then the four raw words, and holds all eight DECODES behind
> `presentable()`. `IGPresentRefreshIsMeasured` is published `false` on purpose:
> the refresh rate is a policy constant this project chose, not a number read off
> the panel, and an unlabelled constant beside measured words is the distinction
> this tree exists to preserve.
>
> `phase0-kext/Info.plist` gained a third personality and
> `com.apple.iokit.IOGraphicsFamily`. Adding it exposed a pre-existing defect one
> line over: `IntelDisplayProbe` carried NO `IOMatchCategory`, and two
> personalities sharing one provider without a category on BOTH leaves the one
> lacking it in the provider's default -- exclusive -- category, so exactly one
> attaches and probe score silently decides which. Setting it on only the new
> personality is the shape that reads as fixed and is not. Both now carry one.
>
> Also in this tar: `phase0-kext/IntelDisplayNames.hpp` gained
> `igPresentVerdictName()`, which the shell called and which did not exist.
>
> Superseded by this rebuild: ISO md5 `a04fea290678d76c8d0fe05a7eb62069`, tar md5
> `80586918574534f3709c9ed054f60d72`.
>
> What moved on the NVIDIA side is `phase0-kext/NVDecode.hpp` and
> `phase0-kext/NV3050Probe.cpp`, and it is the log-only-diagnosis class for the
> third time. `decodeBoot42()` detected four facts about the die -- an unexpected
> dead read where BOOT_0 had answered, the composite chip ID matching GA107, it
> not matching, and the not-matching value being GA106 SPECIFICALLY, the
> 'RTX 3050 Ti Laptop GPU' near miss -- and wrote all four to `NVLOG` and nowhere
> else. The IORegistry carried `NV_PMC_BOOT_42` as a raw word beside
> `NVChipIdExpected` as a statement of what this driver wants, and never compared
> the two, so "is this the die the driver was written for?" was left to whoever
> happened to be reading. `nvChipVerdict()` is those four findings as a value and
> `NVChipVerdict` / `NVChipIsTarget` publish it on BOTH arms, ahead of the word
> they qualify.
>
> `NVChipIsTarget` is an EQUALITY against `NV_CHIP_TARGET`, never a negation of an
> error arm: `!= NV_CHIP_SIBLING_GA106` is true for a dead read and for every chip
> ID this tree has never seen, so a negation would report an unmeasured register
> as the target die.
>
> The part worth carrying is a mutation that LIED in the quiet direction. Hoisting
> the dead-read test below the two chip-ID equality arms reddened NOTHING, and the
> honest reading is that my CLAIM was false rather than the tests vacuous:
> `CHIP_ID` is ten bits, so a dead read decodes to `0x3FF` or `0x000`, and neither
> can equal GA107 (`0x177`) or GA106 (`0x176`). Only the position relative to the
> general FALL-THROUGH is load-bearing -- deleting the dead-read test, and moving
> it below the general return, each redden exactly 2 checks. The non-collision is
> now asserted through `nvBoot42ChipId()` rather than left as arithmetic in a
> comment, so a future chip ID landing on either value reddens.
> `tests/test_decode.cpp` 121 -> **148** checks, above its own ceiling
> (121 + max(8, 12) = 133) by fifteen, so the count band fired on this reading.
>
> Also in this tar, and NOT mine: `phase0-kext/IntelScanout.hpp` and
> `phase0-kext/IntelDisplayNames.hpp`, the concurrent Intel track's work, last
> written 21:25:57 and 21:13:38. They are shipped rather than held back because
> the tar is a whole-directory snapshot with no per-file staleness, and because
> both files held still across a 45-second re-measurement with all five gates
> green over the tree as it stood. This card vouches for the NVIDIA half only.
>
> Superseded: ISO md5 `1ad2f821d6119d4b566fd906b9ca1046`, tar md5
> `85bd23145082b07502589aeede973d3b`.
>
> The thirty ninth rebuild moved NEITHER size -- tar stayed 430080 and ISO
> stayed 507904 while both md5s changed. That is the padding case again, and the
> reason this card quotes hashes rather than byte counts: tar rounds to
> 10240-byte blocks, so a change that fits inside the existing padding ships
> different bytes at an identical length.
>
> What moved is `phase0-kext/IntelDisplay.hpp`, and unlike the last several
> rebuilds it is neither a defect nor a coverage hole -- it is EVIDENCE being
> harvested before it becomes unharvestable. A SECOND observed block was taken
> out of `igpu-results/tgl-final-capture-2026-08-18-201412.txt`: ten
> configuration words (`CDCLK_CTL` `0x0038017E`, `SKL_DFSM` `0x800C1240`,
> `SOUTH_CHICKEN1` `0x00000901`, `SFUSE_STRAP` `0x00000000`,
> `ICL_DPCLKA_CFGCR0` `0x01E17800`, four panel-power dwords at
> `0x0C7200..0x0C720C` and three backlight PWM dwords at `0x0C8250..0x0C8258`),
> plus `IG_CDCLK_FREQ_DECIMAL_MASK` and `igCdclkFreqDecimal()`.
>
> The EXCLUSIONS are the load-bearing half. Three section 6 reads look exactly
> as harvestable as those ten and are absent on purpose. `_DDI_BUF_CTL_A`
> (`0x064000`) read `0x80000087` in one pass and `0x8000008F` in the other
> within the SAME run, because bit 3 is `DDI_BUF_IS_IDLE` -- a live status bit,
> so a constant pinned there would be whichever pass happened to land first, and
> it is the one register in the capture that proves the capture's own rule that a
> register read on a running display is a SAMPLE rather than a constant.
> `PCH_DREF_CONTROL` (`0x0C6200`) and `_TRANS_DP_CTL_A` (`0x0E0300`) both read
> `0xFFFFFFFF`, which `igReadRefused()` calls a refusal rather than a reading, so
> filing either would file a refusal as a measurement. Two of those three
> exclusions are decidable by a predicate the Intel suite already had, so
> `tests/test_intel_display.cpp` ASSERTS them instead of leaving them as prose --
> 421 -> **480** checks, above its own ceiling (421 + max(8, 42) = 463) by
> seventeen, so the count band fired on this reading.
>
> Five of the ten carry their i915 names because those names have a citable line
> in this checkout (`i915_reg.h:4060`, `:2858`, `:3358`, `:4391`, `:4158`). The
> other five are named by OFFSET, because `intel_pps_regs.h` and
> `intel_dp_aux_regs.h` were never vendored -- so `PP_STATUS`, `PP_CONTROL` and
> the `BLC_PWM` family have no anchor here, and inventing those names would put
> fabricated citations beside real ones.
>
> Superseded: ISO md5 `98c398e1542c76e4764429e1fd92d51a`, tar md5
> `4067da3e40d597264bb07189611e2dba`.
>
> The thirty eighth rebuild moved both sizes by ONE 10240-byte tar block each --
> tar 419840 -> 430080 and ISO 497664 -> 507904.
>
> What moved is `phase0-kext/NVGspBoot.hpp` and `phase0-kext/NV3050Probe.cpp`, and
> it is a live defect rather than a coverage hole. `nvGspBootCheckRiscvErrors()`
> read the two RISC-V error latches and then stopped on `nvRiscvHasError()`, which
> is a one-line wrapper spelled `nvRiscvErrorVerdict(...) != NVRISCV_ERR_NONE` --
> so `true` has FOUR preimages, and a boot halted because one half answered with
> the `0xFFFFFFFF` refusal value was reported as `NVGSP_RESULT_ERR_RISCV_FAULT`.
> That is a claim about the CARD made from evidence that is a claim about THIS
> DRIVER's mapping, and only the second is a thing somebody can go and fix.
>
> The sharpest part is that the same driver already draws this line elsewhere:
> the inspect path's NOT-bootable arm splits "the error latches did not decode"
> out of the fault arm, in those words. One path was corrected and the other was
> not, over the same two registers on the same card.
>
> The boot path now stops on `NVGSP_RESULT_ERR_RISCV_ERR_UNREADABLE` when either
> half is refused, tested FIRST and as an EQUALITY against `NVRISCV_ERR_UNREADABLE`
> rather than as a negation, so a sixth verdict added later cannot be swept into
> the fault arm by default. The decode is recorded into a new `errVerdict` field
> BEFORE any stop, and `nvGspBootErrVerdictKnown()` is the conjunction of both
> `readMask` bits -- the field's initialiser is `NVRISCV_ERR_NONE`, which is also
> the HEALTHY reading, so an unguarded publication would report a run that never
> reached the error check as one that checked and found nothing.
> `publishGspBootResult()` publishes `NVGspBootErrVerdict` only behind that guard.
>
> Superseded: ISO md5 `4077114e94548482663170915e7aa1f2`, tar md5
> `4b24a2855e9c33824c9566ab6c33c64f`.
>
> The thirty seventh rebuild moved both sizes by ONE 10240-byte tar block each --
> tar 409600 -> 419840 and ISO 487424 -> 497664.
>
> What moved is `phase0-kext/IntelDisplayProbe.cpp` and
> `phase0-kext/IntelDisplayProbe.hpp`, and it closes the gap the thirty sixth
> image opened. That rebuild gave the `IG_SCAN_` family a name function; this one
> gives it a CALLER. Until now `igScanDescribe()` had 61 passing checks over it in
> `tests/test_intel_scanout.cpp` and occurred nowhere outside its own header --
> intel-handoff26 SS1's class exactly, a heavily tested pure function passing every
> suite it has while contributing nothing to the driver, because a suite proves a
> function is CORRECT and says nothing about whether it is CONSULTED.
>
> `IntelDisplayProbe::publishPlane()` now calls it with the four already-decoded
> values from `IntelFramebuffer.hpp` -- stride bytes, width, height,
> bytes-per-pixel -- and publishes three keys per pipe: `-Scan` (the verdict as a
> WORD), `-ScanUsable` (an EQUALITY against `IG_SCAN_OK`, never a negation of one
> error arm), and `-ScanExtent` (the 64-bit byte extent, published ONLY when the
> descriptor is usable, since an extent over a refused surf word is arithmetic
> over something nobody measured). The first two are published on BOTH paths, so
> the absence of the third is explained by a key that is present rather than being
> a question.
>
> The aperture argument is the one that could have fabricated evidence and did
> not. PLANE_SURF holds a GGTT offset; this driver has never mapped the graphics
> address space, since BAR0 is the register block and is the only mapping
> `mapBar0()` has ever made. There is no measured aperture size to pass, and
> `phase0-kext/IntelScanout.hpp` says writing a BAR index into it would be
> inventing evidence. The bound passed is `IG_FB_GGTT_POLICY_LIMIT`
> (`0x100000000`), already documented in `phase0-kext/IntelFramebuffer.hpp` as a
> POLICY CEILING -- the ceiling of what PLANE_SURF's own address field can
> EXPRESS, not a claim about how much memory exists -- and already the bound
> `igFbSurfaceRangeOk()` uses. So a refusal against it is a fact about the
> register, and an acceptance means the extent is expressible and has NOT been
> checked against a real mapping, because no such mapping exists to check.
>
> Superseded: ISO md5 `98e162d94c121b023bc0cd3996800590`, tar md5
> `df5724fba2179f811db43c3015233e8d`.
>
> The thirty sixth rebuild moved NEITHER size -- tar stayed 409600 and ISO stayed
> 487424 -- while both md5s changed. That is the padding case for the fifth image
> running and the reason this card quotes hashes at all: tar pads to a 10240-byte
> block boundary, so a change that fits inside the existing padding ships
> different bytes at an identical length. A size is not a fingerprint.
>
> What moved is `phase0-kext/IntelDisplayNames.hpp`. The `IG_SCAN_` verdict
> family in `phase0-kext/IntelScanout.hpp` -- seven values describing whether a
> firmware-programmed plane can be handed to IOFramebuffer at all -- was the only
> verdict family in the Intel track with NO name function, so a scanout verdict
> could only ever reach the IORegistry as a bare number beside five families that
> publish words. `igScanVerdictName()` supplies it as a `switch` with a `default:`
> falling through to `"unknown"`, in the idiom of the five beside it.
>
> The one trap this family carries is its LIMIT convention, and it is the reverse
> of the trap handoff20 measured. `IG_SCAN_VERDICT_LIMIT` is 6 and
> `IG_SCAN_ERR_OUTSIDE_APERTURE` is 6 -- the limit is INCLUSIVE, like
> `IG_READ_VERDICT_LIMIT` and unlike `IG_MODE_VERDICT_LIMIT` and
> `IG_FB_INHERIT_VERDICT_LIMIT`, which are exclusive. handoff20 SS1 was
> `igModeVerdictName()` holding arms 0..5 against an exclusive limit of 6, so
> `IG_MODE_SRC_MISMATCH` published `"unknown"` with all four gates green. An
> exhaustiveness loop written `< IG_SCAN_VERDICT_LIMIT` here fails the same way
> from the other side: it stops one short of `OUTSIDE_APERTURE`, which is the one
> verdict in the family that is a fact about how much memory got mapped rather
> than a bug in this driver, and therefore the one an operator most needs as a
> word. `tests/test_intel_display_names.cpp` walks it `<=` and pins the limit as
> an EQUALITY against `"outside-aperture"`, so flipping the convention reddens on
> the commit that does it. That suite is 245 -> **284** checks, above its own band
> ceiling (245 + max(8, 24) = 269) by fifteen, because a pairwise-distinct walk
> over seven arms is 21 pairs on its own.
>
> Superseded: ISO md5 `ce6ad0bfef4bc676a550256e550e6d71`, tar md5
> `062e4ca8f1cc0859eaf049b0eafba981`.
>
> The thirty fifth rebuild moved both sizes by ONE 10240-byte tar block each --
> tar 399360 -> 409600 and ISO 477184 -> 487424.
>
> What moved is `phase0-kext/NVFalcon.hpp` and `phase0-kext/NV3050Probe.cpp`.
> `nvRiscvHasError()` was a two-line bool over the RISC-V error latches, and it
> collapsed FOUR structurally different situations onto one answer: a priv fault,
> a hub fault, both, and NEITHER REGISTER DECODING. `regRead32()` reports every
> refusal it has by returning all-ones, so a BAR0 mapping that covers the Falcon
> block but stops before the RISC-V block one 4 KiB page above it makes both
> latches read `0xFFFFFFFF` -- and the old bool answered TRUE, reporting a bug in
> THIS DRIVER as a latched hardware fault. Those two call for opposite actions:
> reset the engine, or fix the mapping. Three of the five registers in the block
> (`PRIV_ERR_INFO`, `PRIV_ERR_ADDR`, `PRIV_ERR_ADDR_HI`) were also read by
> NOTHING, so a reported fault carried no address. `NVRISCV_ERR_*` is now a
> five-value verdict with UNREADABLE taking precedence, `publishGspErrorLatches()`
> puts the verdict and a readable flag in the IORegistry on BOTH arms with the
> three localization words only behind the readable one, and `nvRiscvHasError()`
> survives as a wrapper that is a NEGATION of the single OK arm. Superseded: ISO
> md5 `8083e0a3acb6a463c327a72e287b95bc`, tar md5
> `f20366d438c0e4e7f6fd9ca0d0a5ae66`.
>
> The thirty fourth rebuild moved BOTH sizes -- tar 378880 -> 399360 and ISO
> 456704 -> 477184, TWO 10240-byte tar blocks each rather than the usual one,
> because the change is a whole new method plus its comment block rather than
> an edit inside existing lines.
>
> What moved is `phase0-kext/NV3050Probe.cpp` and `phase0-kext/NV3050Probe.hpp`.
> `loadGspFirmware()` returned a bare `bool` and wrote its refusal to `NVLOG`
> and nowhere else, while `nvGspStagePlan()` had by then already computed TWO
> verdicts: the six-way `NVGspStageResult` and the EIGHT-way
> `NVRiscvUcodeVerdict` beneath it, the latter carried out of the plan
> deliberately because the stage mapping is lossy in both directions (three
> ucode verdicts land on `ERR_MALFORMED`, three more on `ERR_REGION_OUTSIDE`).
> Both died in the log -- a console nobody reads on a machine whose firmware
> just refused to boot. `publishGspStageVerdict()` now puts all four keys
> (`NVGspStageVerdict`, `NVGspStageOk`, `NVGspStageUcodeEvaluated`,
> `NVGspStageUcode`) in the IORegistry on EVERY arm, called unconditionally and
> BEFORE the refusal is acted on. Superseded: ISO md5
> `ddb849e49f431cc04600b9c265d8a92b`, tar md5
> `97b34eccd8303976f83d8e8ffdf93425`.
>
> The thirty third rebuild moved NEITHER size -- tar stayed 378880 and ISO
> stayed 456704 while both md5s changed. That is the padding case again, and it
> is the reason this card carries md5s at all: tar rounds to 10240-byte blocks,
> so a change of a couple of KiB inside an existing block ships a different
> image at an identical byte count. A reader comparing sizes alone would call
> this card current when it was stale.
>
> What moved is `phase0-kext/NVDecode.hpp` and `phase0-kext/NV3050Probe.cpp`.
> `decodeBoot0()` has always detected four facts about the part -- a non-Ampere
> architecture, an Ada part specifically (the 'RTX 3050 A' case, where the whole
> plan retargets), GA107 confirmed, and an Ampere part that is not GA107 -- and
> has always written all four to the LOG only. `publishIdentity()` derived a
> bare NAME from the same word, so `"Ada"` reached the IORegistry
> byte-identically to `"Ampere"` with nothing saying that one of those two means
> no constant in this driver applies, and `NVImplementation` was simply ABSENT
> for a non-Ampere part. The four findings are now `nvPartVerdict()` in the
> dependency-free header, published as `NVPartVerdict` and `NVPartIsTarget`
> ahead of the names they qualify. Superseded: ISO md5
> `12fcdcd3d157e4cea66f932773a732f0`, tar md5
> `e1f569762dbeb0db55149cf058152589`.
>
> The thirty second rebuild moved BOTH sizes -- tar 368640 -> 378880 and
> ISO 446464 -> 456704, one 10240-byte tar block each. What moved is
> `phase0-kext/NVRiscvUcode.hpp` and `phase0-kext/NVGspStage.hpp`, and it is a
> WIDENING plus two provably dead enum arms rather than a fabricated value.
> `nvRiscvUcodeIsWellFormed()` was a bare `bool` with six distinct ways to say
> no; `nvGspStagePlan()` mapped every one of them onto
> `NVGSP_STAGE_ERR_MALFORMED`, and `loadGspFirmware()` put that single word in
> its refusal log. A truncated file, a booter handed in by mistake, and a
> descriptor pointing off the end of its own payload were indistinguishable to
> the one person who could act on them -- and those call for three different
> actions (re-extract, use different firmware, file a parser bug).
> `nvRiscvUcodeVerdict()` now returns one of eight named codes and the bool is a
> one-line wrapper that is an EQUALITY against `NV_RISCV_UCODE_OK`, so every
> existing caller and test is unchanged. The second half is the sharper one:
> `NVGSP_STAGE_ERR_EMPTY_BOOTLOADER` and `NVGSP_STAGE_ERR_REGION_OUTSIDE` re-read
> the SAME two descriptor fields the well-formed gate had already refused on, so
> both arms were unreachable -- and `tests/test_gsp_stage.cpp` said so in its own
> comments while asserting MALFORMED for cases the enum has precise names for.
> Both are now reachable and asserted under their own names. Superseded: ISO md5
> `fda4f4ab7672ad94635793674095c7fe`, tar md5
> `a4cc43caaa267c25a557da07baa79899`.
>
> The thirty first rebuild moved BOTH sizes -- tar 358400 -> 368640 and ISO
> 436224 -> 446464, exactly one 10240-byte tar block each. What moved is
> `phase0-kext/NV3050Probe.cpp` alone, and it is a live defect rather than a
> coverage hole -- the SIXTH occurrence of handoff18 SS1d's class and the fifth
> in that file. `decodeBoot42()` has always carried an arm for the case where
> `NV_PMC_BOOT_0` answered but `NV_PMC_BOOT_42` came back all-ones or
> all-zeroes: it logs "unexpected dead read ... aperture may be shorter than 16
> MiB" and returns without decoding. But `readIdentity()` returned true anyway
> and `publishIdentity()` published the word unconditionally, so the driver
> DIAGNOSED the dead read in the log and then published it into the IORegistry
> as though measured. `setProperty()` with a dead read is byte-identical to
> `setProperty()` with a register that genuinely holds that value. The fix is at
> publication rather than at the read -- `readIdentity()` still returns true,
> deliberately, because BOOT_0 decoded and the architecture and implementation
> names below it ARE measurements that routing to `publishIdentityUnreadable()`
> would discard. `publishIdentity()` now derives `boot42Readable` from
> `nvLooksLikeDeadRead(boot42)`, publishes `NVBoot42Readable` FIRST and on BOTH
> arms, emits `NV_PMC_BOOT_42` only behind it, and publishes a reason string
> otherwise. Superseded: ISO md5 `1849a1d721a295450b52daa784abb7c8`, tar md5
> `a89bad29ffa914d6726fb67f2da74b9b`.
>
> The thirtieth rebuild moved NEITHER SIZE -- tar 358400 and ISO 436224 are
> unchanged from the twenty ninth image while both md5s moved, which is the
> padding case again and the reason these lines are hashes. What moved is
> `phase0-kext/NVGspBoot.hpp` and `NV3050Probe.cpp`, and it is a live defect
> rather than a coverage hole -- the FIFTH occurrence of handoff18 SS1d's class.
> `publishGspBootResult()` published five diagnostic register words
> unconditionally, while `NVGspBoot` zero-initialises all five and assigns each
> one only inside the state that reads it. A run stopping at PROBE never reaches
> ACTIVE_WAIT and never calls `nvGspBootCheckRiscvErrors()`, so `riscvCpuctl`,
> `privErrStat` and `hubErrStat` went into the IORegistry as initialiser zeros
> that an operator cannot tell from registers reading all clear. The separating
> case is the one that does not look like a failure: a HEALTHY boot stops on
> `NVGSP_RESULT_RUNNING` inside ACTIVE_WAIT before the error pair is ever read,
> so the SUCCESS path is exactly the path that fabricated two words. The fix is
> a `readMask` of `NVGSP_READ_*` bits set after each successful read, published
> as `NVGspBootReadMask` on every path and FIRST, with each word emitted only
> behind `nvGspBootWasRead()`. Superseded: ISO md5
> `fda15fc83b16d70cbb8a11d9220e5833`, tar md5
> `5de35bb7e42133fe8b6b228a27376dd7`.
>
> The twenty ninth rebuild moved BOTH sizes -- tar 348160 -> 358400 and ISO
> 425984 -> 436224, exactly one 10240-byte tar block each -- ending the run of
> three consecutive images that held both fixed. What moved is
> `phase0-kext/NVRiscvUcode.hpp`, and it is a live defect rather than a coverage
> hole. `nvRiscvUcodeIsWellFormed()` read the region between `hdrOffset` and
> `dataOffset` as an `RM_RISCV_UCODE_DESC` in EVERY blob. Measured 2026-08-18
> against all three files in `reference/ga107-firmware/`, only
> `bootloader-570.144.bin` actually carries one: both booters carry an
> `nvfw_hs_header_v2` there, same container magic, same `hdrOffset` of 24, first
> word 60 (`0x3C`) which is that header's own `header_size` and not a structure
> version at all. A booter WAS still rejected before this change -- but by
> accident, three optional-region rows deep, because signature bytes at `+0x24`
> happened to decode to a size of `0x1F3B3C30` that does not fit the payload.
> Nothing constrains a future signature to hold an out-of-range word at that
> offset, and the failure being guarded is not a parse error: it is a DMA of an HS
> signature block into IMEM as though it were bootloader code. The fix is
> `nvRiscvDescVersion()`, `NV_RISCV_DESC_VERSION_UNKNOWN`, and an EQUALITY against
> `NV_RISCV_DESC_VERSION_5` in the predicate. Superseded: ISO md5
> `e0c7da10e52ea3f3a6ad9dedc46d5f51`, tar md5
> `c6fd79724f671fb3ec62ee31245c2112`.
>
> The twenty eighth rebuild moved NEITHER SIZE for the SECOND time running -- tar
> 348160 and ISO 425984 were unchanged across the twenty sixth, twenty seventh and
> twenty eighth images -- while both md5s moved on each. Its cause is the
> smallest one yet: a stray duplicated `//` divider left behind above the go/no-go
> comment block in `inspectGspEngine()` by the twenty seventh rebuild's edit. A
> COMMENT-ONLY change to `NV3050Probe.cpp`, which compiles identically and ships
> differently -- so if the two sizes were the assertion, this rebuild would have
> been invisible and the guest would have built the previous source. Superseded
> there: ISO md5 `aa3ab84169a22d80f5194d343545e8d8`, tar md5
> `d68d15b6f5de3b48450244d600a821a6`.
>
> The twenty seventh rebuild, immediately before it, was the same padding case:
> tar rounds its total up to a 10240-byte block boundary, and both that change and
> this one fit inside the slack the twenty sixth rebuild's new block left behind.
> Superseded there: ISO md5 `822ad3a1760fa183a25f4bfbdb428b00`, tar md5
> `a8bbb01d7b4a6c5131e24545b920431d`. The script is byte-identical to the twenty
> fifth image's, which is why its md5 above is unchanged rather than missing.
>
> The twenty sixth rebuild moved BOTH sizes -- tar 337920 -> 348160 and ISO 415744
> -> 425984, exactly one 10240-byte tar block each -- which the twenty third,
> twenty fifth, twenty seventh and twenty eighth did not. Kept here because the
> readings side by side are the argument: a size that moves is a size that could
> have been checked, and three consecutive rebuilds with both sizes fixed is why
> the md5 is the assertion.
>
> What moved for the twenty seventh and twenty eighth images is
> `phase0-kext/NVFalcon.hpp`, `NV3050Probe.cpp` and `NV3050Probe.hpp` on the
> NVIDIA side, and it is a live defect rather than a coverage hole -- the FOURTH
> occurrence of handoff18 SS1d's class and the third in this shell, this time on
> the WPR2 path. `checkWpr2()` returned a bare `bool` meaning "the firmware boot
> precondition is satisfied", and that bool was false for two categorically
> different reasons: WPR2 is genuinely UP (a fact about the machine, fixed by an
> engine reset), or the registers did not decode at all (a bug in THIS driver,
> the one somebody can go and fix). Worse, it tested ADDR_HI **only** while the
> caller published BOTH words unconditionally -- so a refused ADDR_LO beside a
> healthy ADDR_HI reported the precondition SATISFIED and put
> `regRead32()`'s all-ones refusal value into the IORegistry as
> `GSP_WPR2_ADDR_LO`, byte-identical to a register that genuinely holds it.
>
> The fix is a verdict family in the dependency-free header -- `NVWPR2_DOWN`,
> `NVWPR2_UP`, `NVWPR2_UNREADABLE` with `nvWpr2Verdict()`, `nvWpr2ReadRefused()`
> and `nvWpr2VerdictName()` -- judged per read GROUP rather than per word,
> because the two halves compose ONE reported range. `nvWpr2ReadRefused(0u)` is
> deliberately FALSE: zero in ADDR_HI is exactly the healthy DOWN case. The
> sentinel is an equality, never a threshold, because the top of the address
> space is a legal WPR2 base. `GSPWpr2Verdict` and `GSPWpr2Readable` publish
> FIRST and on BOTH arms; the address words and `GSPWpr2Down` publish only behind
> the readable arm; and `wpr2Down` is an equality against DOWN rather than a
> negation of UP, so a precondition this driver could not measure never reads as
> satisfied. Observed by `tests/test_falcon.cpp` (169 -> **193** checks) for the
> pure side and `tests/test_probe_wpr2_publication.py` (12 tests, one positive
> control and ten negative controls each asserting WHICH property broke) for the
> shell side, which no C++ suite can execute.
>
> What moved before that is `phase0-kext/NV3050Probe.cpp` and `.hpp` on the
> NVIDIA side, and
> it is a live defect rather than a coverage hole -- the third occurrence of
> handoff18 SS1d's class, and the second in this shell. `readIdentity()` set
> `*outBoot42 = boot0` on its dead-read return and returned false; `start()`
> DISCARDED that bool; `publishIdentity()` then published `NV_PMC_BOOT_42`
> unconditionally. So a register that was never read reached the IORegistry, and
> `setProperty("NV_PMC_BOOT_42", <fabricated>, 32)` is byte-identical to
> `setProperty()` with a register that genuinely returned that value -- an
> operator staring at two equal all-ones words cannot tell a dead aperture from a
> card that answered. The fix leaves `outBoot42` alone on that path, makes
> `start()` consume the bool, and routes the failure to a new
> `publishIdentityUnreadable()` that publishes only BOOT_0 plus a reason string.
> `NVIdentityReadable` is published on BOTH paths, so its absence means "older
> kext" rather than "healthy". Observed by
> `tests/test_probe_identity_publication.py` (10 tests, one positive control and
> nine negative controls, each asserting WHICH property broke).
>
> What moved is the OTHER HALF of the twenty fourth rebuild's guard, and it is a
> live defect rather than a coverage hole. That rebuild taught `regRead32()` to
> REFUSE a read past the end of the mapping; nothing downstream consumed the
> refusal. `IG_MMIO_INVALID` occurred at exactly ONE place in the whole probe --
> inside `regRead32()` itself -- so both publishers took whatever came back and
> handed it straight to `igModeConsistent()`, `igPipeUsable()` and
> `igFbInheritable()`. `setProperty()` with a decode of a refused read is
> byte-identical to `setProperty()` with a decode of a register that genuinely
> read that value, so the IORegistry could not distinguish "transcoder B is
> unprogrammed" from "transcoder B was never read" -- and the second is a fact
> about this driver somebody could go and fix. This is handoff18 SS1d's
> `GSPEngineReadable` finding on the Intel side, one level down.
>
> `igReadRefused()` and `igSampleVerdict()` in `phase0-kext/IntelDisplay.hpp` are
> the consumer, judged per read-GROUP rather than per field because the decoders
> are joins: `igPipeUsable()` eats nine words and returns one verdict, so a single
> refusal anywhere poisons the whole answer. `IntelDisplayProbe` publishes
> `IGTranscoder<X>-Sample` and `IGPipe<X>Plane1-Sample` on BOTH paths and FIRST,
> so the verdict sorts above every value it qualifies -- a flag that appears only
> on failure is indistinguishable from an older kext that never published it.
> `test_intel_display` 391 -> **421**, `test_intel_display_names` 236 -> **245**,
> and `tools/check-registers.py` 239 -> **240** with the new `IG_SAMPLE_` family.
>
> What moved is a FOURTH guard in front of `IntelDisplayProbe::regRead32()`, and
> it closed a live defect rather than a coverage hole. `mapBar0()` logs a WARNING
> when BAR0 is shorter than `IG_BAR0_EXPECTED_LENGTH` (0x80000) and then attaches
> anyway -- deliberately, so a short BAR stays inspectable in the IORegistry
> instead of vanishing. That warning was the ONLY consequence there was:
> `regRead32()` bounded offsets against `IG_DISPLAY_POLICY_LIMIT` (0x7BFFF),
> which bounds the addresses the decode HEADERS can produce and says nothing
> about how many bytes the OS actually mapped. So on a 0x40000 BAR, transcoder A
> at 0x60000 was read 0x20000 bytes past the end of the mapping -- not a fault,
> just whatever the next mapping happens to hold, decoded as a transcoder
> register. `igReadDecision()` now takes the mapping length as a fourth
> parameter, `IntelDisplayProbe` records it in `fBar0Length` (zeroed on unmap, so
> the stale-bound direction refuses rather than trusts), and the test is
> `offset + IG_MMIO_ACCESS_BYTES <= length` because `offset < length` accepts an
> offset three bytes short of the end and reads past it. `test_intel_display`
> 373 -> **389**; `tools/check-registers.py` still 239/239 with the `IG_READ_`
> family bumped 4 -> 5.
>
> The twenty third rebuild moved NEITHER SIZE. Both stayed at 327680 and 405504
> while both md5s moved, which is the `a-byte-size-is-not-a-fingerprint` finding
> arriving for real rather than as a caution: tar pads to a 10240-byte boundary,
> the content grew by well under one block, and a card checked on size alone
> would read as current while shipping the previous source. The twenty second
> image's md5 `218e60946a73effcda6bf72ca3205401` and tar md5
> `4871b5a4e72e1492eade044390067a89` are superseded by it.
>
> What changed is the read guard. `IntelDisplayProbe::regRead32()` had three
> guards in front of its one dereference, all of them spelled out inside the IOKit
> shell, and all three return `IG_MMIO_INVALID` -- so no test could separate them
> through the return value, and no host suite could compile them at all. The
> decision is now `igReadDecision()` in `phase0-kext/IntelDisplay.hpp`, returning
> a REASON rather than a bool, and `regRead32()` switches on it. That matters
> because `IG_MMIO_INVALID` is `0xFFFFFFFF`, which is also outside the policy
> range: delete the sentinel arm and the function still refuses, still returns the
> same value, and only the log line changes -- it starts blaming the policy range
> for an offset helper's refusal, sending an operator reading NVLOG after a bad
> constant in the offset tables instead of the declining helper.
> `tests/test_intel_display.cpp` gained 19 checks pinning that precedence on
> inputs where two guards are true at once (354 -> 373), and
> `tools/check-registers.py` gained the `IG_READ_` family (238 -> 239).
>
> The twenty second rebuild moved both sizes by ONE tar block:
> `kext-src.tar` 317440 -> **327680** and `step7-payload.iso` 395264 ->
> **405504**. `phase0-kext/IntelDisplayNames.hpp` is a new file, and
> `IntelDisplayProbe.cpp`/`.hpp` shrank by what moved into it: the four
> verdict-name switches, `igTransName()` and the property-key composition. They
> were file statics inside a translation unit that includes `<IOKit/IOService.h>`,
> so no host suite could compile them and nothing ever EXECUTED them -- the parse
> gate proved they compile and said nothing about what they return. That gap held
> a real defect: `igModeVerdictName()` had arms for verdicts 0 through 5 while
> `IG_MODE_VERDICT_LIMIT` is 6, so `IG_MODE_SRC_MISMATCH` published the string
> `unknown` into the IORegistry. `snprintf` is also gone from the shell, and with
> it the `<libkern/libkern.h>` include -- `igComposeKey()` refuses and empties the
> buffer rather than truncating, so a key that did not fit is visibly empty
> instead of being a shortened string that sorts next to the real one. The twenty
> first image's md5 `a417c592309dc70d15525f5b3dd5e20c` and tar md5
> `37325f64a2f01f472cf94212bd5e1b7f` are superseded by it.
>
> The twenty first rebuild moved both sizes by THREE tar blocks:
> `kext-src.tar` 286720 -> **317440** and `step7-payload.iso` 364544 ->
> **395264**. It is the largest single payload change in this series, and it is
> structural rather than another header layer: the Intel track gained an IOKit
> shell. `phase0-kext/IntelDisplayProbe.cpp` and `.hpp` are new files, and
> `phase0-kext/Makefile` compiles both shells now rather than one. Until this
> rebuild the three Intel decode headers had no consumer anywhere outside
> `tests/` -- nothing in the bundle called them, so a build could not have told
> you whether a real `IOService` subclass could. `phase0-kext/Info.plist` also
> gained a second personality matching 8086:9A68. The twentieth image's md5
> `7815e46da7638c7ba096b0970de7de02` and tar md5
> `ba4fa9794847f6b30dd50342b1ec1ab1` are superseded by it.
>
> The twentieth rebuild is sizes FIXED and hashes moved, the same shape as the
> fifteenth and sixteenth: `kext-src.tar` stays 286720 bytes and
> `step7-payload.iso` stays 364544. The edit is NVIDIA-side and is the first in
> this series that is not an Intel header -- `phase0-kext/NV3050Probe.cpp` and
> `.hpp` gained `publishGspUnreadable()`, so the dead-read path stops handing
> `publishGspState()` six literal zeros for registers it never read. It fits
> inside the existing tar padding. The nineteenth image's md5
> `bb6197dec2cb5a121879deb7e4c47a32` and tar md5
> `17ea88f705ad119daf2ca70a7da7d7ba` are superseded by it.
>
> The seventeenth rebuild moved both sizes: `kext-src.tar` 276480 -> **286720**
> and `step7-payload.iso` 354304 -> **364544**, one 10240-byte tar block again.
> `phase0-kext/IntelDisplay.hpp` gained the `TRANS_DDI_FUNC_CTL` layer --
> `igTransDdiReg()`, `igDdiEnabled()`, `igDdiPortField()`, `igDdiPort()`,
> `igDdiMode()` and the `IG_DDI_*` constants -- which is the largest single edit
> to that header since the plane layer. The sixteenth image's md5
> `af16aaa6152117e57e5b3bd1487c90a2` and tar md5
> `84b51cd3844e4c532aaac73c1343302c` are superseded by it.
>
> The sixteenth rebuild is sizes FIXED again -- `kext-src.tar` 276480 bytes,
> `step7-payload.iso` 354304 bytes -- with `phase0-kext/IntelFramebuffer.hpp`
> gaining `igFbSurfaceRangeOk()` and `IG_FB_GGTT_POLICY_LIMIT`, the base+extent
> join that mirrors the NVIDIA side's `nvGspStageRangeOk()` fix. The fifteenth
> image's md5 `562dff2205ba290e51a58b5834b73f60` and tar md5
> `2ee001587bd4e9cc9f1199d9627bc2a0` are superseded by it.
>
> The fifteenth rebuild is back to sizes FIXED and hashes moved: `kext-src.tar`
> stays 276480 bytes and `step7-payload.iso` stays 354304. The edit was one
> `#define` -- `IG_FB_INHERIT_VERDICT_LIMIT` in `phase0-kext/IntelFramebuffer.hpp`
> -- plus its comment, which fits inside the padding the fourteenth's larger edit
> had just opened up. The fourteenth image's md5
> `cb710bc3aa13e7d3c09b0e713610b1b5` and tar md5
> `e52094363e9cce879248a1cb94564d1c` are superseded by it.
>
> The fourteenth rebuild is the first in three where the SIZES moved as well as
> the hashes: `kext-src.tar` 266240 -> **276480** bytes and `step7-payload.iso`
> 344064 -> **354304** bytes, both exactly one 10240-byte tar block larger.
> `phase0-kext/IntelDisplay.hpp` gained `igPlaneMatchesPipe()` and the four
> `IG_JOIN_*` verdicts -- the plane/raster join -- and that edit finally pushed
> the archive past the padding the twelfth and thirteenth fitted inside. Read
> the three together: two rebuilds with the sizes FIXED and the hashes moved,
> then one with both moving. A size line can only ever be a weak corroborator
> here; the md5 is the fingerprint. The thirteenth image's md5
> `5476ea689dca449376d03fccbafb872c` and tar md5
> `f216fd0cecfe517c560af83da3b0b891` are superseded by it.
>
> The thirteenth rebuild is the same shape as the twelfth -- an EDIT to an
> existing file. `phase0-kext/IntelDisplay.hpp` gained the whole-mode consistency
> layer (`igAxisConsistent()`, `igModeConsistent()`, and the seven `IG_MODE_*`
> verdicts), which is a much larger edit than the twelfth's and STILL moved
> neither size: `kext-src.tar` is 266240 bytes and `step7-payload.iso` is 344064
> bytes before and after, because tar pads to 10240 and the image to 2048 and
> this edit fits inside the existing padding. Two consecutive rebuilds with both
> sizes fixed and both hashes moved is the clearest statement available of why
> the size lines cannot stand in for the md5 lines. The twelfth image's md5
> `5ea939169f9f0a35d40b69b2a4a023d4` and tar md5
> `ad2f53bacc8331bf1d89ab7304f8f8dd` are superseded by it.
>
> The twelfth rebuild is a second instance of the ninth's shape rather than the
> tenth's or eleventh's: an EDIT to an existing file, not a new one. The dead
> `height != 0u` conjunct came out of `igFbSurfaceBytes()` in
> `phase0-kext/IntelFramebuffer.hpp` and a comment explaining why the divisor
> cannot be zero went in. `kext-src.tar` is **266240 bytes before and after** and
> `step7-payload.iso` is **344064 bytes before and after** -- both sizes are
> byte-identical across the rebuild while both hashes moved, which is the whole
> reason these lines are hashes. The eleventh image's md5
> `9ac338f8a0dd88f06a2d3a07f522587f` and tar md5
> `f8bcb41456d651a95e838141c2d2cb73` are superseded by it.
>
> The eleventh rebuild has the same cause as the tenth -- a NEW FILE,
> `phase0-kext/IntelFramebuffer.hpp`, the Gen12 plane-register decode layer that
> answers framebuffer inheritance. Both sizes moved again for the identical
> reason (tar 245760 -> 266240, image 323584 -> 344064): a new file adds a
> 512-byte tar header plus padded content and crosses both boundaries. The tenth
> image's md5 `566026b19e886bae765119dbd0c61e58` and tar md5
> `7b7f5d869edf1ec2c4f3cb3fde9a9500` are superseded by it.
>
> The tenth rebuild was triggered by a NEW FILE rather than an edit to an
> existing one: `phase0-kext/IntelGpuId.hpp`, the Gen12 PCI device-identification
> layer. It is the counter-example to the ninth immediately below -- here both
> sizes moved (tar 225280 -> 245760, image 303104 -> 323584), because a new file
> adds a 512-byte tar header plus its padded content and crosses both the
> 10240-byte tar boundary and the 2048-byte sector boundary. That is exactly why
> the size assertions cannot replace the md5 ones: they happen to move on this
> rebuild and provably did not on the last one.
>
> The ninth rebuild is worth its own sentence, because it is the cleanest example
> in this file of why every one of these lines is a HASH and not a size. It was
> triggered by the INTEL-CITE fix -- `phase0-kext/IntelDisplay.hpp` gained one
> `#define` (`IG_PLANE_SIZE_OBSERVED_2026_08_16`) and had four comment lines
> reworded. `kext-src.tar` is **225280 bytes before and after**, and
> `step7-payload.iso` is **303104 bytes before and after**, because tar pads to a
> 10240-byte boundary and pycdlib pads to a 2048-byte sector. Both md5s moved.
> Every size-based assertion in `tests/test_build_step7_payload.py` was satisfied
> by the stale image; only the md5 comparison was not.
>
> The superseded image md5s are
> `c6d3cec019a83d2bebd47b21eec61aca` (303104-byte image, 225280-byte tar) -- the
> ninth image, the last one before `IntelGpuId.hpp` existed -- then
> `b4cafd723cb2ee51a723f2bf507ae431` -- the eighth image, identical in size to
> the ninth for the reason above -- then
> `d9880069620e064b5f6751727a05e93a` -- the seventh image, and the LAST one whose
> hash was a function of the clock rather than of the content, so it is not
> reproducible even from the exact tree that produced it -- then
> `a4f1524f18664a6a113357c41d360e57` -- the sixth image, whose tar and image
> sizes are also IDENTICAL to the live one's, which is precisely why it is listed
> with its hash rather than its dimensions -- then
> `7268289248b6ad6419087dd177af1970` (215040-byte tar, 292864-byte image),
> `b1e4b361d7e55550585d5876715bc813` (204800-byte tar, 282624-byte image),
> `c3aa966c758150fd8a7bcb7e79583969`, `044a84eca00e8c933ee6af9e868d74b3` and
> `02fe7a0e73decbdd341a3ece0e6de81e`. The superseded tar md5s are
> `02a381ba54247e7ddc4aab9968b401ea` -- the eighth tar, and the one the seventh
> and eighth images both carried -- then
> `413ab5e2488bdcd0907bd7bfe1ef5cb1` -- a hand-assembled tar, not one this
> project's builder produced, which is why it differed from the then-canonical
> `02a381ba54247e7ddc4aab9968b401ea` at identical size and identical content --
> and `8c073e68dd3fbd0c14102e3de1aa3310`. `tests/test_build_step7_payload.py` (34 tests) plus
> `tests/test_guest_build_kext.py` (**39** tests, was 28, was 22 — the eleven
> newest pin the argument parser the script did not have until 2026-08-14) both
> pass against it, and the full Python gate passes.
> (No total is quoted for the full gate on purpose: several terminals are adding
> tests concurrently, so a project-wide count is stale within minutes and
> `tests/test_operator_step7_doc.py` would then fail on correct prose. The two
> per-module counts above ARE checked, because this card is the thing that
> ships them.) The ISO check in that second module compares **bytes**, not
> sizes, as of 2026-08-14.
>
> **macOS mounted it.** Measured 2026-08-14, with no credentials and without
> looking at the framebuffer. The earlier revision of this block said the mount
> was "only visible in the framebuffer"; that was wrong, and the method matters
> because it is reusable.
>
> Searching the running guest's 8 GiB `.vmem` for the string `/Volumes/PAYLOAD`
> is **not** the test, even though it returns 247 hits. `guest-build-kext.sh`
> spells that literal itself, so cached file text alone could produce hits and
> prove nothing. (Measured later the same day, that innocent explanation turns
> out not to apply here — see the cache note below — but the string count was
> never the evidence either way.) The test is the
> kernel's own `struct statfs`, whose layout pins two independent distances:
> `f_fstypename` at `0x48`, `f_mntonname` 16 bytes later at `0x58`, and
> `f_mntfromname` 1024 bytes past that at `0x458`. Of 176 distinct
> NUL-terminated `/Volumes/PAYLOAD` offsets, **114 have `cd9660` at exactly the
> predicted 16-byte gap**, and reading 1024 bytes on from those yields
> `f_mntfromname` = **`/dev/disk3s0`**, 71 times. Two struct constants honoured
> plus a real device node is the mount table, not page-cached script text, and
> `cd9660` is the filesystem an ISO9660 optical disc mounts as.
>
> So the disc is mounted at the path the command below uses. What is still
> unproven is that anything has been **executed** from it — mounting is not
> running, and that is exactly what the remaining step establishes.
>
> **Cache note — the truncation hazard is ruled out, measured after the 15:30
> rebuild.** Swapping a mounted medium invites a specific and nasty failure: the
> script grew 6718 → 7596 bytes, so if the guest had cached the OLD ISO9660
> directory record it would believe the file is 6718 bytes and a read through the
> live mount would return a script cut off mid-construct — which `sh` starts
> executing anyway, producing a garbage failure that looks like a build error.
> That cannot happen here. **Nothing from the payload volume is resident in guest
> memory**: no directory record, no filename under either descriptor encoding,
> and no script text (`MAKE_STATUS` and `nv3050-step7`, which both the old and
> new scripts contain, each return zero hits). The volume is mounted but its
> directory has never been read. The stale-disc problem is entirely in the
> **host** file handle described above, not in guest cache, and the power cycle
> in step 1 is the whole fix.
>
> Two method traps, because this measurement was wrong twice before it was right
> and both traps are reusable. First, do not search memory for a filename and
> step back 33 bytes to read the record — that lands in prose far more often than
> on a record, and it reported "STALE" off a declared length of `1814045984`,
> which is ASCII text read as a little-endian integer. ECMA-119 stores the extent
> LBA at `+2`/`+6` and the data length at `+10`/`+14`, each **both-endian**, so
> requiring the two halves to agree rejects noise at about 2⁻³². Second, and
> worse: with that check added the scan returned a clean "nothing cached" — while
> its own ground-truth pass over the real ISO could find only `KEXT_SRC.TAR` and
> never the script. The search key was wrong. The medium does not carry the
> string `guest-build-kext.sh` anywhere: the Primary descriptor 8.3-truncates it
> to `GUEST_BU.SH;1` and the Joliet descriptor stores it UTF-16BE. **A memory-scan
> null means nothing until the same parser is proven against ground truth that
> provably contains the thing**, and for ISO9660 the search key must come from
> walking the volume descriptors, never from the host filename.
>
> ⚠ Do **not** run `python tools\build-step7-payload.py` while the guest is up.
> Its `main()` moves a freshly built image over `step7-payload.iso`, which is the
> file this running VM currently has open.
>
> **This warning was violated on 2026-08-14 at 15:30, deliberately and with the
> outcome measured.** The rebuild was needed to ship the exit-status fix, and the
> guest is currently powered on. What the warning protects against did not
> happen: the host emulator opened the CD once at power-on and issued its last
> CDROM command at 23:07:36Z the previous night, so the replacement did not land
> mid-read. The cost is only the one stated above — the guest holds the old
> image until it is power-cycled. If you are reading this before starting, treat
> step 1 as work to do, not as done.

### 1. Shut the guest down — REQUIRED AGAIN (see the 15:30 rebuild note above)

From the project root:

```
"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe" stop "C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx" soft
```

`soft` asks macOS to shut down cleanly. If it refuses (an unsaved dialog can
block it), shut down from the Apple menu inside the guest instead.

The power-off is not optional and not caution-for-its-own-sake. VMware holds
the VMX in memory while a VM runs and rewrites it from memory on suspend, so an
edit made now is silently discarded. `deliver-via-cd.py` already refuses to
edit a live VMX for that reason — it detected the four lock artifacts and exited
1 rather than write.

That guard fired again on 2026-08-13 with the VM genuinely dead, because it
scans for lock artifacts and those had been orphaned by the non-clean exit. It
is a conservative guard, not a power-state oracle. `--force` is the documented
way past it, but the thing to establish first is that no `vmware-vmx.exe`
process exists — `vmrun list` alone is not enough to justify forcing, since it
reports on VMware's own bookkeeping rather than on the host process table.

### 2. Point the CD at the payload — DONE 2026-08-13 19:00

Kept in full because step 5 reverses it, and because it has to be re-run if
anything under `phase0-kext/` or `tools/guest-build-kext.sh` changes after this.
In that case rebuild the payload first:

```
python tools\build-step7-payload.py
```

That rebuilds the tar, the staged script and the ISO, verifies all three against
the working tree *before* moving anything into `build-payload/`, and then runs
`tests/test_guest_build_kext.py` over what landed. It never touches the VMX. If
it exits non-zero, nothing was replaced and the old payload is still in place.

Then repoint the drive:

```
python tools\deliver-via-cd.py --add build-payload\kext-src.tar --add build-payload\guest-build-kext.sh --out build-payload\step7-payload.iso --vmx "C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx" --device sata0:2
```

`sata0:2` is the guest's only optical device. It currently points at
`darwin.iso` (the VMware Tools installer), and that gets restored in step 5.

The tool re-reads the VMX after writing and reports whether the `fileName`
actually changed. Trust that line, not the presence of `Sonoma.vmx.bak` — the
backup is written *before* the rewrite, so it proves only that the tool got as
far as copying.

### 3. Boot, THEN run one command at the guest console

**CORRECTED 2026-08-14 15:30.** An earlier revision of this section said the
boot was done and told you not to power-cycle. That advice was correct when it
was written and is wrong now. The ISO was rebuilt at 15:30 while this VM was
running, and a running VM does not re-read its CD, so the guest is holding the
pre-fix image. Running the console command against the currently-mounted disc
would build the OLD script — the one that prints `make exit status: 0` for a
failed compile. **Shut the guest down (step 1) and start it again with the
command below.** The state that earlier advice protected is worth less than a
build whose verdict you can believe.

```
"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe" start "C:\Users\Kentl\macos-vm\sonoma\Sonoma.vmx"
```

In Terminal inside the guest:

```
sh /Volumes/PAYLOAD/guest-build-kext.sh
```

The script finds the payload by globbing `/Volumes/*/kext-src.tar` rather than
assuming the mount point, unpacks to `~/nv3050-step7`, records the toolchain,
runs `make`, and prints the tail of its own log.

### 4. Report back one of three things

What matters is which, and the script prints all three plainly:

- **`Kernel.framework: ABSENT`** — **corrected 2026-08-14; the original wording
  of this bullet was wrong in the expensive direction.** It said ABSENT meant
  "Command Line Tools but not full Xcode" and told you to install Xcode from the
  App Store. Both halves are false. MEASURED in this guest after the build that
  succeeded: `/Applications/Xcode.app` does not exist, `xcode-select -p` is
  `/Library/Developer/CommandLineTools`, and `Kernel.framework` **is** present at
  `$(xcrun --show-sdk-path)/System/Library/Frameworks/Kernel.framework`. The
  Command Line Tools SDK carries it on its own, so full Xcode was never needed
  and the App Store download would have cost gigabytes for nothing. What ABSENT
  actually meant was that the guest had **no developer tools whatsoever** —
  `xcode-select` errored and `clang` did not exist. The whole fix was one
  command, `xcode-select --install`, with zero source changes. The check still
  runs *before* `make` for the original good reason: nothing else in the log
  means anything until a toolchain exists.
- **a compile error** — copy the first error and the line it names. That is the
  answer the whole project has been waiting for, whichever way it goes.
- **a LINK error naming undefined C++ runtime symbols** — things like
  `___cxa_pure_virtual`, `__ZdlPv`, or static-initialiser glue. Add `-lkmodc++`
  to `LDFLAGS` in `phase0-kext/Makefile`, immediately above `-lcc_kext`, and run
  `make` again. Xcode's kext product type links that library for C++ kexts, and
  `OSDefineMetaClassAndStructors` is exactly the kind of construct that needs it.
  It is deliberately *not* in the Makefile already: a library that turns out not
  to exist in the guest's SDK fails the link with "library not found", which
  would burn this attempt on a fault the host introduced blind.
- **`bundle: PRESENT`** plus the `nm` output showing `IOService` / `OSMetaClass`
  symbols — it compiled and linked.

The log is at `~/nv3050-step7/step7-build.log`.

**Corrected 2026-08-14.** This paragraph used to say there was *no*
credential-free guest→host file path, and that without a password the log had to
be read off the screen. That is false, and it was not disproved by argument — it
was disproved by pulling the file: `scp -P 2222 kenny@127.0.0.1:<log>` returned
`step7-results/step7-build.log` byte-for-byte, 1582 bytes, with no password and
no human at the console. The public key that makes it work is installed; see
`OPERATOR-ACTION-guest-key.md`.

**The guest is running an SSH server and it is reachable from the host right
now.** Measured 2026-08-14:

- `vmnat.exe` (PID 33804) listens on `0.0.0.0:2222`, and
  `C:\ProgramData\VMware\vmnetnat.conf` line 84 reads `2222 = 192.168.255.129:22`.
  The listener is VMware's NAT process, not a host `sshd`, so the server on the
  far side is the guest's.
- It answers `SSH-2.0-OpenSSH_9.9` and completes a full key exchange. Host key
  `ed25519 SHA256:AyD71CYuH5igumu932qnr5kLA6ofbXlQ00pNR3oGIgY`, which matches
  what is already recorded in the host's `known_hosts`.
- Offered methods are `publickey,password,keyboard-interactive`.
- The host's existing `id_ed25519` is **refused** — the guest has not been given
  that public key — so `publickey` does not get in today.

Do not try to reach `192.168.255.129` directly; the host firewall rejects it at
`connect()` with `WinError 10013`, which describes the host and says nothing
about the guest. Go through `127.0.0.1:2222`.

**This has since been done, so nothing here is still asked of the operator.**
The key below is in the guest's `~/.ssh/authorized_keys` as of 2026-08-14, which
is why `publickey` now gets in and why the line above about it being refused
describes only that morning. It is recorded here so the key can be recognised,
not pasted again:

```
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPFGC+QqUOIS/ZqcceE8rN1uQzFwNKE08XGOtCgNTb3T kentl-windows
```

### 5. Put the CD back

```
python tools\deliver-via-cd.py --add build-payload\kext-src.tar --out C:\Users\Kentl\macos-vm\sonoma\darwin.iso --dry-run
```

Don't actually run that — it is shown only to make the point that the restore is
a VMX edit, not a file copy. Shut the guest down and set `sata0:2.fileName`
back to `darwin.iso` by hand, or re-run the tool with `--out` pointed at the
original path. Leaving the payload mounted is harmless; it just means VMware
Tools cannot be reinstalled without another swap.

---

## What is already verified on the host

So that nothing below has to be re-checked by hand:

| Thing | How |
|---|---|
| The ISO is well-formed and both files round-trip under their long names | `deliver-via-cd.py --verify`, plus its own 53-check self-test |
| The tar is rooted at `phase0-kext/` where the script looks | `tests/test_guest_build_kext.py` |
| Every header the kext includes is in the payload | same |
| No stale host `build/` rides along | same |
| The script parses under POSIX `sh` | `sh -n`, asserted in the same module |
| Its three refusal paths print distinguishable messages | same, run for real against fake payloads |
| It is LF-only, so the shebang survives | same |

What is **not** verified, and cannot be from Windows: that `make` succeeds. That
is the entire point of the exercise.
