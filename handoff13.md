# handoff13 — STEP 7 answered: the kext compiles, and the blocker was never the delivery channel

Newest handoff. Read `handoff12.md` for the state this continues from, and
`CLAUDE.md` for the four verification gates.

**This session moved the project's ceiling for the first time in eight
handoffs.** The kext has now been compiled by Apple's clang with `-mkernel
-fapple-kext` against the real `Kernel.framework`, and it linked into a Mach-O
kext bundle. `make exit status: 0`, `bundle: PRESENT`. The log is
`step7-results/step7-build-SUCCESS.log`.

Every handoff up to and including `handoff12.md` closes by naming STEP 7 as the
highest-value open item, and every one of them is wrong as of today. Those
sentences are left standing on purpose — the numbered handoffs are a
chronological record, not a status page. `CLAUDE.md` is the status page.

---

## 1. What was actually proven

Exactly one claim, and it is worth being precise about its edges because the
project has spent four handoffs describing this build as the thing that would
settle the question.

What the build establishes:

- The `-mkernel -fapple-kext` flag pair is accepted and the translation unit
  compiles under **Apple clang version 17.0.0 (clang-1700.0.13.5)**.
- `OSDeclareDefaultStructors` / `OSDefineMetaClassAndStructors` expand correctly
  against the **real `libkern`**, not against `tests/kextshim/`.
- Every IOKit API call in `NV3050Probe.cpp` type-checks against the real
  `Kernel.framework` headers.
- It **linked**, which is a strictly stronger claim than compiling. `nm` on the
  product shows the metaclass constructors and the three `IOService` overrides:

  ```
  __ZN11NV3050Probe4stopEP9IOService
  __ZN11NV3050Probe5probeEP9IOServicePi
  __ZN11NV3050Probe5startEP9IOService
  __ZN11NV3050ProbeC1EPK11OSMetaClass
  __ZN11NV3050ProbeC2EPK11OSMetaClass
  ```

- `file` reports `Mach-O 64-bit kext bundle x86_64`, and the `Info.plist` copied
  into the bundle passes its own validation (`Info.plist: OK`).

What it does **not** establish, unchanged from `PLAN.md`:

- It was never `kextload`ed. That needs SIP lowered and a reboot.
- It was never matched against hardware. The guest has no GA107, so a load in
  this VM would prove nothing about the target laptop even if it succeeded.
- The guest is **x86_64**, so this is not a proof about an arm64 kext build.

## 2. What actually blocked it — not the thing four cards were built around

This is the part worth reading, because it is the same failure mode this project
keeps hitting: a great deal of engineering aimed at the wrong constraint.

`OPERATOR-ACTION-step7-build.md`, `tools/deliver-via-cd.py`,
`tools/build-step7-payload.py`, `build-payload/step7-payload.iso` and
`tools/guest-build-kext.sh` were all built on one premise — that the hard part
was **getting the source into the guest** without credentials. A virtual CD was
the answer to that, and it worked: `handoff10` through the ISO byte-identity
gate all confirmed the payload was well-formed, the disc was mounted at
`/Volumes/PAYLOAD`, and macOS had it in its mount table.

None of that was the blocker. The first real attempt
(`step7-results/step7-build.log`, 2026-08-14T20:15:22Z) died before `make` was
ever invoked:

```
xcode-select  : xcode-select: error: Unable to get active developer directory.
clang         : xcode-select: note: No developer tools were found, requesting install.
sdk path      : xcode-select: note: No developer tools were found, requesting install.
```

The guest had **no toolchain at all**. Not "Command Line Tools but not full
Xcode" — the outcome the card called the single most likely one and wrote a
whole `Kernel.framework: ABSENT` branch for. Nothing. `clang` did not exist on
that machine.

The card's `Kernel.framework: ABSENT` check did its job: it fired *before*
`make`, so the log says why rather than burying it under a hundred lines of
include errors. That check earning its placement is the one design decision in
those four cards that paid off exactly as intended.

Installing the Command Line Tools is what unblocked STEP 7. Once
`/Library/Developer/CommandLineTools` existed, the SDK carried
`Kernel.framework` and the build had everything it needed — no full Xcode, no
App Store download, and no change to a single line of source.

**The lesson for the next card:** every one of those four artifacts was correct,
tested, and aimed at a constraint that did not exist. The cheap measurement
(`which clang`, one command, no credentials once SSH was up) was never taken,
because the delivery problem was more interesting than the environment problem.

## 3. The middle run, and why exit 141 is not a compile failure

Between the two is `step7-results/step7-build-clang.log`
(2026-08-14T20:25:02Z), which reports:

```
make exit status: 141
bundle: ABSENT -- the build did not get as far as a bundle.
```

141 is 128+13 — **SIGPIPE**. The compiler was killed by a broken pipe, not by an
error in the source. Its log tail is full of ordinary deprecation warnings and
no `error:` line anywhere. Fifty-five seconds later the same tree built clean.

This matters for two reasons. First, `bundle: ABSENT` is *true* and yet
completely misleading about the cause — a reader who stopped at the verdict line
would conclude the kext does not compile. Second, this is the same wound as the
`$?`-after-a-pipeline bug fixed in `tools/guest-build-kext.sh` earlier the same
day: statuses that travel through a pipe describe the pipe. A status of 128+N
should be read as "the process was signalled" before it is read as "the code is
wrong".

## 4. The channel that actually carried it: SSH over the NAT forward

The build did not go over the CD. It went over **SSH**, which turned out to have
been live and unnoticed:

- `vmnat.exe` listens on `0.0.0.0:2222` and
  `C:\ProgramData\VMware\vmnetnat.conf` forwards it to `192.168.255.129:22`.
  The listener is VMware's NAT process, so the server behind it is the guest's.
- The guest answers `SSH-2.0-OpenSSH_9.9`, host key
  `ed25519 SHA256:AyD71CYuH5igumu932qnr5kLA6ofbXlQ00pNR3oGIgY`, and offers
  `publickey,password,keyboard-interactive`.
- The account short name is `kenny`, settled independently by guest-memory
  evidence before any credential existed.

Use `127.0.0.1:2222`. Do **not** dial `192.168.255.129` directly — the host
firewall rejects that at `connect()` with `WinError 10013`, which describes the
host and says nothing at all about the guest.

This is the first **bidirectional** host↔guest channel in the project. The CD
was host→guest only, which is why every operator card had to ask for the build
log to be read off a screen. `scp -P 2222` in both directions replaces that
entirely, and it is how the three logs in `step7-results/` got here.

## 5. What the compiler said that is worth acting on

The build produced **17 warnings** and zero errors. Three groups, none fatal,
two of them worth a follow-up:

- **`IOPCIDevice` is deprecated** — `__kpi_deprecated("Use PCIDriverKit")`, at
  `NV3050Probe.cpp:143`. This is Apple saying the entire kext-based PCI approach
  is on its way out. It is consistent with `PLAN.md`'s ceiling and not a defect,
  but it is the clearest signal yet that a DriverKit shell is the only
  forward-looking shape for this code.
- **`setBusMasterEnable` is deprecated in favour of `setBusLeadEnable`**
  (macOS 12.4), at `NV3050Probe.cpp:267`. Apple's own fix-it names the
  replacement. Cheap to change; the only reason not to is that
  `tests/kextshim/` would need the same rename to keep the syntax gate honest.
- **A `setProperty` override hides a base overload** at `NV3050Probe.hpp:63`,
  against `IORegistryEntry.h:1115`. This is the C++ name-hiding rule, and it is
  the kind of warning that becomes a real bug the first time someone calls the
  hidden overload and gets a silent conversion instead.

The link line is worth recording because the card predicted a failure here that
did not happen:

```
clang++ -arch x86_64 -isysroot <SDK> -nostdlib -Xlinker -kext -lkmod -lcc_kext
```

`OPERATOR-ACTION-step7-build.md` §4 warned that C++ runtime symbols
(`___cxa_pure_virtual`, `__ZdlPv`, static-initialiser glue) might come back
undefined and that `-lkmodc++` would be the fix. **It was not needed.**
`-lkmod -lcc_kext` alone resolved everything. Do not add `-lkmodc++`
speculatively — the card's own reasoning for leaving it out (a library that
turns out not to exist in the guest's SDK fails the link with "library not
found") still stands.

Two `ld` warnings note that `libkmod.a` members were built for macOS 15.5 while
the link targets 15.0. Harmless here, and a deployment-target mismatch rather
than anything about this code.

## 6. Guest environment, for the record

```
uname   : Darwin kennys-Mac.local 24.6.0 ... xnu-11417.140.69.711.44~1/RELEASE_X86_64 x86_64
sw_vers : macOS 15.7.9 (24G830)
toolchain: /Library/Developer/CommandLineTools
clang   : Apple clang version 17.0.0 (clang-1700.0.13.5)
build dir: /Users/kenny/nv3050-step7d/phase0-kext
```

The `nv3050-step7d` suffix is the fourth attempt directory. Earlier attempts
left `nv3050-step7`, and a run against a stale unpack is a real hazard on that
machine — unpack fresh rather than reusing whichever directory is already there.

## 7. What this session did not do

- **No source change.** Not one line of `phase0-kext/` was edited to make the
  build pass. The dependency-free-header split described in `CLAUDE.md` held up
  against the real headers on the first attempt that had a compiler.
- **No load, no match, no hardware.** See §1.
- **No arm64 build.** The guest is x86_64.
- **The four host-side gates are unchanged and still green.** They remain the
  fast loop; the guest build is the slow confirmation, not a replacement.

## 8. What is open now, in descending value

1. **Diff Apple's real KPI signatures against `tests/kextshim/`.** This is now
   possible for the first time and it is the highest-value item in the project,
   because it is the only thing that tells us whether the *host* syntax gate has
   been lying. `tests/kextshim/` was hand-written to be just enough for clang to
   parse the kext; a stub that is looser than Apple's real declaration makes the
   gate vacuous in exactly the direction that hides bugs. One instance of that
   is already on record — `IODMACommand`'s `mappingOptions` was typed `UInt32`
   in the shim where Apple uses an unscoped enum, so a bare `0` parsed clean.
   With a real SDK reachable over `scp`, every stub can be checked against its
   real counterpart instead of reasoned about. Claimed as `t-4dce56`.
2. **Fix the three warnings in §5**, keeping `tests/kextshim/` in step so the
   syntax gate stays honest.
3. **Retire or re-banner the four CD-delivery artifacts.** They are correct and
   now largely pointless. `OPERATOR-ACTION-step7-build.md` already carries a
   superseded banner; `tests/test_operator_step7_doc.py` gates it, so any
   rewrite has a test that will notice contradictions.
4. **`tools/guest-build-kext.sh` has no argument parser** — `--payload-root` is
   documented and silently ignored. Claimed as `t-8b649a`.

Everything that was blocked on the guest account password is now unblocked by
the SSH channel, so `OPERATOR-ACTION-guest-creds.md` is no longer the root of
four cards. It is still the cleanest way to make the channel permanent — a
one-time `authorized_keys` paste at the console hands over no secret at all.
