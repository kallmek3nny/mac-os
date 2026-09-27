# `kextshim/` — parsing the kext on Windows

This is **not** Apple's SDK and is not a substitute for one. It is the smallest
set of headers that lets clang's C++ front end get all the way through
`phase0-kext/NV3050Probe.cpp` on a machine with no macOS.

## Why it exists

`HANDOFF.md` §7 recorded the project's sharpest open blocker: **the kext had
never been compiled.** 102 verified register constants and 204 passing decode
tests said nothing about whether the IOKit shell parses —
`OSDeclareDefaultStructors`, `APPLE_KEXT_OVERRIDE`, `IOMemoryMap` calls, and
the `Info.plist` personality were all unverified by construction.

clang 19.1.5 ships inside Visual Studio 2022
(`VC\Tools\Llvm\bin\clang++.exe`). Pointed at these stubs it parses the kext.

## Run it

```
tests\check-kext-syntax.bat
```

## What a green run proves

| Defect class | Caught by |
|---|---|
| Typo in an IOKit member name, wrong argument count | clang parse |
| `APPLE_KEXT_OVERRIDE` on a method that overrides nothing | `override` expansion |
| `IOLog` format specifier vs argument width mismatch | `__attribute__((format(printf,1,2)))` |
| `OSDynamicCast` to an unrelated type | `static_cast` in the shim |
| Missing `OSDefineMetaClassAndStructors` | `tools/check-structors.py` |
| Unbalanced braces, missing declarations, bad member init | clang parse |

## What it does **not** prove

- Real Apple KPI signatures may be wider or differ outright. The stubs were
  transcribed to match the *uses* in `NV3050Probe.cpp`, deliberately narrow —
  a permissive stub would hide the mistakes this exists to find.
- **Nothing links.** A kext links against Apple's kernel KPI, which is not on
  this machine.
- Nothing here says the kext loads, matches, or runs.

`STEP 7` in `../../STEPS.md` — a real `make` inside the macOS VM — is still
required and is still where genuinely new failures will appear.

## Is the shim itself right?

Section 3 of the gate proves the kext parses **against these headers**. It says
nothing about whether these headers agree with IOKit. A stub that is *looser*
than Apple's real declaration makes the gate silently vacuous for that call.

`SIGNATURES.md` tables every symbol here against its real signature, records the
one live defect that audit found (a bare `0` for `IODMACommand`'s
`mappingOptions`, which is an enum in Apple's header and so a hard error under
Apple clang), and carries a four-command recipe for re-verifying the table
against a real SDK. **Run that recipe before the Step 7 build, not after it.**

## The negative control

`negative-control.cpp` is the load-bearing piece. A syntax gate that passes
everything is worse than no gate: it converts *"never compiled"* into
*"believed compiled"*. So the file contains the exact defects listed above,
each behind its own `-D` macro, and the harness **fails the run if any of them
compiles clean**.

One planted case was removed rather than kept as decoration:
`NVNEG_MISSING_STRUCTORS` did **not** fail. `-fsyntax-only` never links, so a
declared-but-undefined constructor is invisible to it. That defect class moved
to `tools/check-structors.py`, which checks the pairing textually — and also
verifies the superclass named in the macro matches the one the class actually
derives from.

## Files

| File | Stands in for |
|---|---|
| `IOKit/IOTypes.h` | `SInt*`/`UInt*`, `IOReturn`, map options, `APPLE_KEXT_OVERRIDE` |
| `IOKit/IOLib.h` | `IOLog` (format-checked), `IOSleep`, `OSReadLittleInt32`, `memcpy`/`memset` |
| `IOKit/IOService.h` | `IORegistryEntry`, `IOService`, the five driver virtuals |
| `IOKit/IOMemoryDescriptor.h` | `IOMemoryMap`, `IOMemoryDescriptor` |
| `IOKit/IODeviceMemory.h` | `IODeviceMemory` (Apple's own header; see the file for why it is not folded into the line above) |
| `IOKit/IOBufferMemoryDescriptor.h` | `IOBufferMemoryDescriptor`, `kIODirection*`, `kIOMemoryPhysicallyContiguous` |
| `IOKit/IODMACommand.h` | `IODMACommand`, `Segment64`, `withSpecification` (explicit `numAddressBits`) |
| `IOKit/pci/IOPCIDevice.h` | `IOPCIDevice`, config-space offsets, power states |
| `libkern/c++/OSObject.h` | `OSObject`, `OSData`, `OSDictionary`, the structors macros, `OSDynamicCast` |

`SIGNATURES.md` is not a stand-in for anything — it is the audit of the nine
headers above against Apple's published declarations, plus the recipe for
re-checking it on a host that actually has the SDK.

`memcpy`/`memset` live in `IOLib.h` because Apple's real `IOKit/IOLib.h`
includes `<string.h>`. Declaring them there rather than shipping a shim
`<string.h>` keeps the driver's include graph identical to the one Apple's
toolchain will see: `NV3050Probe.cpp` includes only IOKit headers.
