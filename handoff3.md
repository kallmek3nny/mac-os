# handoff3.md — resume record, written 2026-08-07 ~21:15Z by `bold-moth-69` (lead)

**Read order on resume: this file, then `handoff2.md` for the forward plan, then
`HANDOFF.md` for detail.** Every claim below is from a tool result in this
session. Where a prior doc is contradicted, the contradiction is called out
explicitly with the evidence that overturned it. Nothing here is from memory.

---

## 1. Headline: the install is FINISHED, and the blocker is now authentication

The project's open questions were "did the install finish" and "why can't we
talk to the guest". Both are answered, and the answers are not what the older
docs assume.

**macOS is fully installed and booted to a desktop.** `Sonoma.vmx` line 236
carries a *guest-originated* write-back:

```
guestInfo.detailed.data = "architecture='X86' bitness='64' buildNumber='24G830'
  distroName='macOS' distroVersion='15.7.9' familyName='Darwin' kernelVersion='24.6.0'"
```

That is **macOS 15.7.9, build 24G830** — a full installed system reporting its
own version through Tools. It is *not* the 15.4.1 recovery image (build
24E263) that `VM-BRINGUP.md` and the VMX header comment describe. Those
comments are now stale; the recovery installer did its job and the installed OS
superseded it.

> **`VM-BRINGUP.md` is now corrected** — a second block after its line-10
> warning records the `24G830` / `15.7.9` write-back and states plainly that
> anything in that file reading as "the guest is sitting in Recovery" is
> bring-up history, not current state. The original 15.4.1 sentence was left
> standing because it is still true *of the installer image*; only the reading
> of it as current guest state was wrong.
>
> **The VMX header comment is still stale and stays that way.** `Sonoma.vmx` is
> read-only by crew rule (ping the lead first), so `t-b8cf72` documents the
> discrepancy in an annotated inventory rather than editing the file.

**The desktop is confirmed from pixels**, not inferred:
`artifacts/boot-20260807T2110Z.png` (md5 `80bd080e8c735d91502ee731a6c61b76`,
1228x935, window `Sonoma - VMware Workstation` hwnd 528718) shows the macOS menu
bar with *System Settings* active, the clock reading `Fri Aug 7 3:56PM`, a
VMware "Background Items Added" notification, and an Apple ID sign-in pane.

> **Trap for the next agent:** `tools/vm-screenshot.py --ocr` printed `FAIL` on
> this capture. That verdict is a phrase-match artifact — the tool was looking
> for an expected phrase that a settled desktop doesn't contain. The OCR text it
> printed in the same breath contradicts its own verdict. **Read the PNG; do not
> trust the `--ocr` pass/fail string.**

---

## 2. Corrections to the task board and to handoff2

### VMware Tools is installed and running — `t-f8acfa` is DONE

Two independent pieces of positive evidence:

1. `vmrun -T ws checkToolsState` returned `running`, exit 0, on the fresh
   21:00Z boot.
2. The preserved log `vmware-PREBOOT-20260807T2100Z.log` records
   `Guest: toolbox-dnd: Version: 12.1.1.12 (build-22484250)` (L1469),
   `Tools: [AppStatus] Last heartbeat value 34 (last received 0s ago)` (L1486),
   and `Tools_SetVersionAndType: Updating the Tools Version in the disk` (L1492).

`tools/vm-type.py:10` said `checkToolsState -> unknown (no VMware Tools)`, which
would have misrouted anyone planning off it. **That docstring is now
corrected** — the original bullet list is kept and explicitly labelled `STALE`,
followed by the measured current state: `running` at exit 0 reproduced
23:15/16/17Z, the `12.1.1.12 (build-22484250)` toolbox version from the
preserved log, and the fact that guest ops fail on
`VIX_E_ANON_GUEST_OPERATIONS_PROHIBITED` (an auth gate) rather than on absent
Tools. The same docstring now also records that the tool does **not** currently
deliver input, so nobody reaches for it as a workaround.

#### `getGuestIPAddress` is NOT a Tools-liveness test (measured 23:15–23:17Z)

The live log carries `VIX_E_TOOLS_NOT_RUNNING` at 22:54–22:56Z while
`checkToolsState` says `running`, which looks like a contradiction worth
chasing. I tried to use `getGuestIPAddress` as a real toolbox round trip to
break the tie. **It cannot serve that role.** Measured, 3 rounds back to back:

```
round 1 @ 23:17:16Z  ip=unknown        (exit 0)
round 2 @ 23:17:16Z  ip=unknown        (exit 0)
round 3 @ 23:17:17Z  ip=unknown        (exit 0)
log grew by exactly 6 lines — three pairs of:
  SOCKET nn recv error 10054 (connection forcibly closed by the remote host)
  Vix: VMAutomation: Connection Error (1) on connection nn
```

Two things follow, and both matter:

1. **Zero toolbox traffic was generated.** No `GuestRpcSendTimedOut`, no
   `VIX_E_TOOLS_NOT_RUNNING` appeared during the probe. The only new lines are
   the VIX **client** socket being torn down as each `vmrun` process exits —
   that is my own host-side process closing, not the guest failing. So the call
   never asked the guest anything; it read a cached `guestInfo` slot. A call
   that does not traverse the channel cannot measure the channel.
2. **Its answer is demonstrably wrong**, which proves the slot is simply
   unpopulated rather than authoritative. The guest's address is
   `192.168.255.129` — established by a DHCP lease whose MAC matches VMX L211
   exactly, corroborated by `ping` 3/3 at 0% loss, and further corroborated by
   TCP RSTs arriving from it. `unknown` is not a measurement of that; it is an
   empty field.

**Current honest state of Tools:** `checkToolsState` → `running` (exit 0),
reproduced at 23:15Z, 23:16Z, and 23:17Z, with **no new heartbeat timeout
during that specific 23:15–23:17Z window**. The `VIX_E_TOOLS_NOT_RUNNING`
entries are historical, not current.

> **The "last heartbeat timeout is 22:40:23Z" reading is now stale, and the
> sentence above is bounded to its own window on purpose.** The log has grown
> since. Full inventory as of the 2565-line live log, measured with
> `python tools/vmware-log-parse.py --only heartbeat_timeout --no-collapse`
> — six, not two:
>
> | # | timestamp | line |
> |---|-----------|------|
> | 1 | 22:39:49.627Z | L1372 |
> | 2 | 22:40:23.628Z | L1456 |
> | 3 | 23:36:55.626Z | L1994 |
> | 4 | 23:37:20.628Z | L2071 |
> | 5 | 23:39:26.626Z | L2394 |
> | 6 | 23:39:51.628Z | L2474 |
>
> Entries 3–6 are **newer than the 23:15–23:17Z window this section measured**,
> so the paragraph above must not be read as an open-ended claim about Tools
> health — it is a claim about three samples taken at 23:15–23:17Z and nothing
> later. Re-run the command before quoting any count; the number grows.
>
> This does **not** overturn "treat Tools as up." A heartbeat timeout is a
> *host-side observation of silence on the toolbox channel*, not proof the guest
> is hung — the same one-directional asymmetry as `vm-screenshot --compare`.
> `checkToolsState` → `running` remains positive evidence; the timeouts are the
> sampling gaps that evidence is drawn between. Note also that the collapsed
> (default) view prints these six as a single `x6` row spanning
> `22:39:49.627..23:39:51.628` — that range is a collapse artifact, not one
> hour-long timeout. Use `--no-collapse` when you need the individual events.

Treat Tools as up, and treat `checkToolsState` as the sampled
heartbeat it is — see the `:toolsguard` reasoning in
`tools/guest-enable-ssh-via-tools.cmd`, which already assumes the daemon can
vanish between two steps.

**The only call that would genuinely exercise the toolbox channel is a guest
operation, and every one of those is auth-gated.** So Tools liveness cannot be
proven any harder than this without operator credentials. Do not spend more
session time on it; it is not the blocker.

**`toolsInstallManager.lastInstallError = "21004"` was misread as an install
failure.** 21004 means *the guest holds the CD*. Combined with
`sata0:2.startConnected = "TRUE"`, that says macOS mounted `darwin.iso` itself —
the install-manager path errored *because* the payload had already gone in.

### The vmrun failures were never about Tools

Every VIX guest operation on this VM returns:

```
Anonymous guest operations are not allowed on this virtual machine.
You must call VixVM_LoginInGuest before performing guest operations
```

That is a **missing `-gu`/`-gp` guest login**, not absent Tools. It applies to
`captureScreen` too — that call failed this way at 21:05Z, exit 127, no file
written. So `captureScreen` is *not* an available screenshot route on this VM
even with Tools running; host-window capture via `tools/vm-screenshot.py` is.

---

## 3. The network blocker is closed with positive evidence

Previously the guest IP was a single unreproduced sighting. It is now solid.

`C:/ProgramData/VMware/vmnetdhcp.leases` holds **two successive lease blocks**
for the same address — a renewal, which is the reproduction that was missing:

```
lease 192.168.255.129 {
  ends 6 2026/08/08 00:26:54;   <- first block
  ends 6 2026/08/08 00:39:38;   <- second block (renewal)
  hardware ethernet 00:0c:29:e7:9f:2b;
  client-hostname "kennys-Mac";
}
```

That MAC matches `Sonoma.vmx` L211 `ethernet0.generatedAddress` exactly, so the
lease belongs to **this** guest and no other.

**Reachability measured, not assumed:** `ping -n 3 192.168.255.129` → 3 sent,
3 received, **0% loss, TTL=64**. Bidirectional IP works host↔guest. The host
sits on `192.168.255.1` for that vmnet, so host→guest TCP is directly routable
with no port forwarding.

---

## 4. sshd and Screen Sharing are OFF — measured, not inferred

Raw `/dev/tcp` connect attempts, 5 s timeout each, from the host to the guest IP:

| Port | Result | Underlying errno |
|---|---|---|
| 22 (sshd) | not listening | `Connection refused` |
| 5900 (Screen Sharing) | not listening | `Connection refused` |

**Re-measured 2026-08-07 ~21:40Z, and the result is sharper than the first
pass.** Both probes returned `bash: connect: Connection refused` — an **RST**,
not a timeout. That distinction matters and the wrapper text hides it:

- A *timeout* is ambiguous. It means "no answer", which a firewall DROP and a
  dead host produce identically.
- An *RST* is not ambiguous. Something at `192.168.255.129` received the SYN,
  processed it in its TCP stack, and actively refused because **no process is
  bound to that port**. A DROP-style firewall cannot produce an RST, and an
  unreachable host cannot answer at all.

So the claim is no longer "closed *or* filtered" — filtering is eliminated by
the RST itself, independently of any other test. The guest is up, its TCP stack
is serving, and nothing is listening on 22 or 5900.

The ICMP positive control still holds and still belongs in the record
(`ping -n 3 192.168.255.129` → 3 sent, 3 received, 0% loss, re-confirmed in the
same minute), because it independently proves the path. But it is now
corroboration rather than the load-bearing evidence.

> **Do not copy the phrase "closed-or-filtered" out of the shell output.** That
> string is hardcoded in the probe's `|| echo` fallback, which fires on *any*
> nonzero exit and cannot distinguish refused from timed-out. The real
> discriminator is the `connect:` errno on the line above it. Read the errno.

Remote Login has not been enabled.

So `t-ed1a53` is genuinely still open, and `t-80af15` (in-guest
`ifconfig`/`netstat`/`curl`) cannot start.

---

## 5. The one thing no agent can do

Three routes into the guest exist, and all three are shut:

| Route | State | Evidence |
|---|---|---|
| VIX guest ops (`runScriptInGuest`, `captureScreen`) | blocked | `VIX_E_ANON_GUEST_OPERATIONS_PROHIBITED`, needs `-gu`/`-gp` |
| SSH | blocked | TCP 22 closed while ICMP succeeds |
| Scancode injection | **not delivering unelevated** | `tools/_input-block-probe.py`: `sent=1 err=0`, cursor unmoved, `lastinput_advanced=False`, `DELIVERED=False`. **NOT** the framebuffer deltas — see the retraction below; those were byte-identical PNGs |

### The injection measurement, done properly (2026-08-07 ~21:35Z)

Earlier runs "proved" injection both ways because the comparison method was
broken. **`vm-screenshot.py --compare REF.png` did not compare against
`REF.png`.** As it stood that day, the `--compare` branch of
`tools/vm-screenshot.py` captured
frame 1, slept, captured frame 2, then saved frame 2 over the path named by
`--compare` — it **overwrote the file you named** and compared its own two
frames. Both frames are post-action. The mtimes prove it: `inject-after`
23:02:40Z but `inject-baseline` 23:02:43Z, and `menu-after` 23:04:58Z but
`menu-baseline` 23:05:01Z — every "baseline" was written *after* its "after".
(These are **UTC**, matching every other timestamp in this file. The host is
America/New_York = UTC−4, so the same instants read 19:02:40 / 19:02:43 /
19:04:58 / 19:05:01 in local time. An earlier revision of these three lines
printed the local values with no suffix, which looked like it contradicted the
UTC figures under *"Naming is not provenance"* below — it did not; re-measured
both ways 2026-08-08. Always write the `Z`.)
The `changed: yes (guest 0.212%)` that looked like success was two
post-injection frames of an idle desktop.

> **The clobber half is now fixed** (2026-08-07, `t-6751f1`). `--compare`
> takes a **base name**: frames land on `NAME.frame1.png` / `NAME.frame2.png`
> via `derived_frame_path()`, and `NAME` itself is never written. Guarded by
> `tests/test_vm_screenshot.py::DerivedFramePathTests` (7 tests), one of which
> greps the tool's source so the old call cannot return silently. Python gate:
> **198 tests, exit 0**. What is *not* fixed, because it is inherent: both
> frames are still captured by that one invocation, so `--compare` can never
> attribute a change to an action. Use two `--out` captures plus
> `tools/png-delta.py` for that.

`tools/png-delta.py` (written this session) diffs two already-saved PNGs and
never captures, so it cannot clobber a reference. With it:

| Run | Method | Guest delta | Standing |
|---|---|---|---|
| Control — capture, wait 6 s, capture, **no injection** | two separate `--out` runs | `0.000%` | **RETRACTED** |
| `--key cmd+space`, `1 event group accepted` | two separate `--out` runs, inject between | `0.000%` | **RETRACTED** |

> ### ⚠ RETRACTION (2026-08-07, `bright-otter-11`) — both rows above are void
>
> Those two `0.000%` figures are **not measurements of the guest.** Every
> before/after PNG in `shots/` is byte-identical to its partner. `md5sum` over
> all eight frames collapses to **two** hashes:
>
> ```
> 090566eb0ad301a6633f1c6fa1d910fe  floor-a.png floor-b.png floor-c.png floor-d.png   (all 185840 bytes)
> 024e9d456fe7ed850ce3dff37ab58743  mouse-a-20260807T2018Z.png  mouse-b-20260807T2018Z.png
>                                   mouse-a2-20260807T2022Z.png mouse-b2-20260807T2022Z.png
>                                   applemenu-20260807T2015Z.png                     (all 82501 bytes)
> ```
>
> So each "pair" is **one frame counted twice**, and `0.000%` is arithmetic on a
> duplicate input. A duplicate capture and a genuinely unchanged guest print the
> identical line, and before the fix below both also **exited 0** — a failure of
> the *capture pipeline* was indistinguishable from a fact about the *guest*.
> This is the same error class as reading `SendInput`'s "accepted" as delivery.
>
> **Artifact-provenance audit, measured 2026-08-08 (bold-moth-69).** The
> duplicate-frame disease is NOT confined to `shots/`. Hashing every PNG in
> `artifacts/` and `shots/` together: **40 files, 27 distinct frames, 6
> duplicate classes covering 19 of the 40 files.** The classes, complete:
>
> | class | md5[:8] | x | files |
> |---|---|---|---|
> | 1 | `adf741a4` | 3 | `clean-after-…2135Z`, `inject-after-…2121Z`, `menu-after-…2125Z` |
> | 2 | `090566eb` | 5 | `clean-before-…2135Z`, `shots/floor-a`, `floor-b`, `floor-c`, `floor-d` |
> | 3 | `024e9d45` | 5 | `applemenu-…2015Z`, `mouse-a-…2018Z`, `mouse-a2-…2022Z`, `mouse-b-…2018Z`, `mouse-b2-…2022Z` |
> | 4 | `e2d7ad87` | 2 | `fg-after`, `ssh-t-ed1a53-tools-progress` |
> | 5 | `860058bb` | 2 | `resume-…1952Z`, `state-…1955Z` |
> | 6 | `3a3de802` | 2 | `wp85-ocr-…2245Z`, `wp85-t-ed1a53-…2245Z` |
>
> **Class 1 is the serious one.** The "after" frame of three *separate*
> experiments — scancode injection (21:21Z), menu click (21:25Z), and the clean
> control (21:35Z) — is one and the same file. `png-delta.py inject-after
> menu-after` exits **3, BYTE-IDENTICAL**. Three experiments taken minutes apart
> cannot legitimately share a frame, so at most one of those three names
> describes the capture, and nothing in the filenames says which.
>
> **Class 2 subsumes the whole `shots/floor-*` set into `clean-before`** — all
> four "floor" frames plus the clean baseline are one frame counted five times.
> That is the arithmetic behind the retracted `0.000%` floor, now measured
> directly rather than inferred.
>
> Deltas of the four named pairs, re-measured with `png-delta.py` today:
>
> | pair (as named) | guest | whole window | reading |
> |---|---|---|---|
> | `inject-baseline` → `inject-after` | 0.000% | 0.005% | frames share no hash, but `inject-after` is class 1 — provenance unknown |
> | `menu-baseline` → `menu-after` | 0.212% | 0.193% | the "success" number; `menu-after` is class 1 |
> | `ctrl-A` → `ctrl-B` | 0.000% | 0.630% | **both frames UNIQUE** — the one pair with clean provenance |
> | `clean-before` → `clean-after` | 0.000% | 0.055% | before is class 2, after is class 1 |
>
> **`ctrl-A` → `ctrl-B` is the only pair in the entire set where both frames are
> hash-unique**, and it reads guest **0.000%** with 0.630% outside the guest box.
> Its two frames are 7 s apart by mtime (23:08:49Z → 23:08:56Z). That makes it
> the only defensible idle datum on record — and it is an *idle* datum, not an
> injection one.
>
> **Naming is not provenance.** `-baseline`/`-after` in a filename is a label a
> script wrote, and the mtimes contradict the labels: every `*-baseline` was
> written *after* its own `*-after` (inject 23:02:43Z vs 23:02:40Z; menu
> 23:05:01Z vs 23:04:58Z). Before citing any pair from `artifacts/`, md5 both
> files and check both mtimes. Reproduce this audit with:
>
> ```
> python -c "import glob,hashlib,collections; fs=sorted(glob.glob('artifacts/*.png'))+sorted(glob.glob('shots/*.png')); h={f:hashlib.md5(open(f,'rb').read()).hexdigest() for f in fs}; g=collections.OrderedDict(); [g.setdefault(h[f],[]).append(f) for f in fs]; print('%d files, %d distinct' % (len(fs), len(g))); [print('x%d :: %s' % (len(v), ' | '.join(v))) for v in g.values() if len(v) > 1]"
> ```
>
> That is one physical line — no embedded newlines. Run it from the repo root.
> Expected today: `40 files, 27 distinct` followed by the six `x2`/`x3`/`x5`
> class lines above.
>
> **There is therefore no idle floor on record, and no framebuffer datum about
> injection at all** — the claim "the guest box is bit-static when untouched"
> was inferred from duplicates and is withdrawn. Host→guest injection remains
> **unproven, not refuted.**
>
> **Fix landed:** `tools/png-delta.py` now `md5`s both inputs *before* computing
> anything and exits **3** with `BEFORE and AFTER are BYTE-IDENTICAL. This is
> not a measurement.` plus a capture-pipeline checklist. `--allow-identical` is
> the escape hatch, for testing the tool itself only. All six exit paths were
> exercised: identical→3, identical+flag→0, distinct→0, missing file→2, size
> mismatch→2, distinct+`--floor 5.0`→1.
>
> **Also distrust the `Z` stamps in `shots/` filenames.**
> `mouse-a-20260807T2018Z.png` has mtime **20:02:48Z**. Trust mtime and md5,
> never the name.

To produce a real result: capture, act, capture again, **confirm the two md5s
differ**, then run `python tools/png-delta.py A.png B.png --floor <pct>`.
`--floor` takes a **percentage, not a file path** — the guest delta printed by a
control pair (two captures, no action between them, md5s confirmed different).
Measure the control first, read its percentage, then pass that number. Whole-window deltas
(0.055–0.630%) are host chrome and the host clock, outside the guest box, and
mean nothing either way.

**The narrowest claim the evidence supports:** on this host, unelevated
`SendInput` accepts keystroke events and the guest does not consume them.
`tools/_input-block-probe.py` agrees from the other side —
`sent=1 err=0`, cursor unmoved, `lastinput_advanced=False`, `DELIVERED=False`,
`VERDICT: injection is NOT delivering`. Do **not** read vm-type.py's
`N event group(s) accepted` as delivery; the tool says so itself.

One earlier click run returned **`0 event group(s) accepted`** — a *different*
failure (nothing left the host at all, likely foreground acquisition), not
"accepted but discarded". Both are non-delivery; don't conflate them.

**Elevation is the documented fix, and it needs a human.**
`tools/_vm-type-elevated.cmd` records that the operator accepted an admin
Python UAC prompt and *saw the mouse move*, and that six other causes were
blamed first and all six were wrong. The wrapper cannot self-elevate — UAC is
an interactive consent gate. So injection does **not** route around
`t-396740`; it is a second thing needing the operator, not an alternative to
the first.

**The tooling is already written and waiting.**
`tools/guest-enable-ssh-via-tools.cmd` does the whole job — authenticates,
copies the script in, runs it under `sudo -S`, copies the output back, then
verifies with `systemsetup -getremotelogin`, `launchctl`, and `netstat`. It
deliberately **refuses to run** without `MACOS_GUEST_USER` and
`MACOS_GUEST_PASS`, because calling vmrun guest ops without them hangs on an
interactive `Guest user:` prompt rather than failing cleanly.

**No guest credentials exist anywhere in this project.** A grep across all
`*.md` surfaced only `kenny@192.168.0.180`, which is the **physical laptop**,
not the guest — do not confuse the two.

> **Hypothesis, explicitly not a fact:** the DHCP request carried
> `client-hostname "kennys-Mac"`. macOS derives that from the ComputerName set
> during Setup Assistant, which makes `kenny` a *plausible* account short name.
> ComputerName and account short name are independent values. Do not write an
> unverified username into any doc as fact, and do not burn attempts guessing —
> a wrong `-gu` just re-raises the same auth error.

---

## 6. Exact next actions, in order

1. **Operator supplies guest credentials** (indispensable; nothing below starts
   without it):
   ```
   set MACOS_GUEST_USER=<account short name from Setup Assistant>
   set MACOS_GUEST_PASS=<that account's password>
   tools\guest-enable-ssh-via-tools.cmd
   ```
   Alternatively the operator enables Remote Login by hand at the console
   (System Settings → General → Sharing → Remote Login), which unblocks
   everything without ever handing over a password.
2. **Confirm the unblock host-side** — re-probe TCP 22. Expect `OPEN` plus an
   `SSH-2.0-OpenSSH_...` banner. This is the gate; do not proceed on the script's
   own say-so.
3. **Then** run `handoff2.md` §3 Step 1 network checks over SSH (`t-80af15`).
4. **Then** the remaining in-guest work, which finally has a real shell.

---

## 7. Housekeeping done this session

- **VMX survived the power-cycle** (handoff2 §3 Step 0 re-run, passes):
  `guestOS = "darwin24-64"`, `ethernet0.virtualDev = "vmxnet3"`,
  `connectionType = "nat"`, and **exactly one** `ethernet0.pciSlotNumber`
  (`"160"`, at L228). Comment blocks intact, file 10251 B.
- **`ethernet0.pciSlotNumber = "160"` is not the clobber.** I initially misread
  it as one, because L161 says the key is "deliberately ABSENT". It is VMware's
  own post-power-on write-back for the vmxnet3 device, which `HANDOFF.md:908–913`
  already records as expected. The value that caused the historical crashes was
  a *hand-pinned* one. Leave L228 alone.
- **`vmware.log` preserved before the boot rotated it** as
  `vmware-PREBOOT-20260807T2100Z.log` (165272 B). All L-numbers cited in this
  file refer to that preserved copy, not to the live log.
- VM powered on GUI, `vmrun list` → `Total running VMs: 1`.

---

## 8. Host-side work verified this session (needs no guest, no credentials)

With every guest route operator-gated, I re-derived the GA107 firmware claims
from the raw bytes rather than trusting the docs. **All of them hold.** This is
an independent reproduction, not a re-read.

### The three falcon blobs

Parsed `nvfw_bin_hdr` at offset 0 in each of the three `.bin` files:

| Blob | magic | ver | bin_size | header_offset | data_offset | data_size | file size |
|---|---|---|---|---|---|---|---|
| `bootloader-570.144.bin` | `0x10de` | 1 | 24832 | 24 | 108 | 24576 | 24684 |
| `booter_load-570.144.bin` | `0x10de` | 1 | 61440 | 24 | 888 | 60416 | 61304 |
| `booter_unload-570.144.bin` | `0x10de` | 1 | 41216 | 24 | 888 | 40192 | 41080 |

`data_offset + data_size` lands **exactly** on the file size in all three
(61304 == 61304, etc.), so the payload is fully present.

> **Do not "fix" the `bin_size` mismatch.** My parser flagged
> `bin_size == file size: False` (0xf000 = 61440 vs 61304 on disk). That is the
> *same* false alarm `reference/ga107-firmware/README.md:41-44` already records
> and already dismissed: `bin_size` is the **padded/aligned** length and the
> trailing pad is not stored. The check is wrong, not the data. I reproduced a
> known non-bug; anyone re-running this will hit it too.

### The inner descriptors really do differ by blob type

This is the README's central claim and the one worth being sure of. Read at
`header_offset = 24` in each file, the bytes are not the same shape:

**Booters — `nvfw_hs_header_v2`, byte-identical between the two:**

```
sig_prod_offset 60    sig_prod_size 768   patch_loc      828
patch_sig      832    meta_data_off 836   meta_data_size  12
num_sig        848    header_offset 852   header_size     36
```

Those are exactly the values at `README.md:71-73`. Both booters produce the
identical nine words, which is what "identical header shape in both" predicts.

**Bootloader — not that structure at all.** The first word at offset 24 is
`0x00000005`, i.e. `RM_RISCV_UCODE_DESC` **version 5**, followed by
`0x5000`/`0x880`/`0x5880` and a nonzero region at +100 (`0x6000` = 24576, the
full payload length). If the HS layout applied here, offset 24 would read 60.
It reads 5. **The two-parser requirement is confirmed from the bytes.**

This also re-confirms the negative result the README calls the real finding:
the `0x3c` = 60 that once looked like `FALCON_UCODE_DESC_V2_SIZE_60` is
`sig_prod_offset` in the HS header — a coincidence. There is no VBIOS-embedded
`FALCON_UCODE_DESC` in any of these files.

**Phase 4 consequence, unchanged and still binding:** `sig_prod_size = 768`
across `num_sig` entries is the production signature block and `patch_loc = 828`
is where the selected signature gets patched in. A driver cannot load the SEC2
booters without performing that patch step.

### The VBIOS dump is a real dump, and it is the right die

```
bytes 0..1     : 55 aa            valid PCI option-ROM signature
nvfw magic     : 0xeb7faa55       NOT 0x000010de -- correctly not an nvfw container
PCIR struct at : offset 368
  vendor id    : 0x10de           NVIDIA
  device id    : 0x25a2
NPDE at 400, NPDS at 62341, version string at 185
head strings   : "IBM VGA Compatible", build date 07/27/21
```

`0x25a2` maps to **GeForce RTX 3050 Laptop GPU, GA107, Ampere** per
`DEVICE-IDS.md:17` — the die this whole project targets. The VBIOS is a PCI
option ROM and is correctly *not* an `nvfw_bin_hdr` container, which is why it
needs a different parser again.

**Nothing in the firmware reference needs changing.** This branch of the work is
sound and can proceed the moment someone picks it up — it never needed the VM.

### Where the GSP/firmware work actually lives

> **Correction to a pointer I broadcast to the crew.** I told the crew to take
> "the GSP/firmware blob tasks in handoff2 §5+". That is wrong. `handoff2.md`
> line 487 is `## 5. Safety envelope — do not widen it`; there is no firmware
> work in §5 or later, and that envelope must not be widened.

The real material is in two other files:

- **`PHASE4-GSP-BRINGUP.md`** — §4 records the two timing gaps
  (`0.9228 → 1.1195` ≈ 197 ms between BROM setup and core start,
  `1.1195 → 1.8407` ≈ 721 ms while GSP boots). **Neither is a fixed delay — a
  real implementation polls.** §5 is the kext state machine: assert
  `CPUCTL.HALTED` → check `HWCFG2` RISC-V capable and `nvFalconScrubbingDone()`
  → pulse `FALCON_ENGINE` reset 1→0 → program `DMATRFBASE`/`DMATRFBASE1` → loop
  256 B IMEM transfers polling `DMATRFCMD.IDLE`. The **WPR2 hypothesis in §4 was
  wrong and the trace disproves it** — do not resurrect it.
- **`reference/ga107-firmware/README.md`** — blob layout, now byte-verified above.

### Host-side tasks filed this session (no guest, no credentials)

Every guest-side task is operator-gated, so these exist to give the crew real
work that cannot hit the auth wall. Each card carries its verified starting
facts and the specific trap that already cost this session a wrong turn.

| Task | Work | Assigned |
|---|---|---|
| `t-08cd68` | Parse `vbios-ga107.rom` past the PCI header — BIT tokens/BIOSInfo. **Card premise was wrong: the falcon ucode table is NOT absent, it is off-file.** BIT token 0x70 = `77 7f 07 00` = 0x077f77 (491383), 426365 B past the end of this 65024-B dump. See `reference/ga107-firmware/README.md` § *VBIOS: BIT table and the falcon ucode pointer* | **DONE** (bright-yak-d7) |
| `t-381352` | Write `tools/vmware-log-parse.py` — timeline extractor; must not mislabel host-side `SOCKET 10054` teardown as guest failure | bright-otter-11 |
| `t-b8cf72` | Annotated `Sonoma.vmx` inventory, **read-only** | sunny-crane-ab |
| `t-6751f1` | Fix `vm-screenshot.py --compare` (overwrote its reference) and the `--ocr` FAIL string | **clobber half DONE** (eager-fox-fd); `--ocr` FAIL string still open |
