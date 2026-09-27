# `kextshim/` signature audit

Every symbol this shim declares, against the signature Apple publishes for it.

## Why this file exists

`check-kext-syntax.bat` section 3 proves `NV3050Probe.cpp` parses **against the
shim**. It says nothing about whether the shim agrees with IOKit. Wherever a stub
is *looser* than Apple's declaration — `void*` where the real parameter is a
typed pointer, `UInt32` where the real one is an enum, a nullary callback
typedef where the real one takes four arguments — the gate is **vacuous for that
call**, and the error first appears under Apple clang on the operator's single
build attempt (`OPERATOR-ACTION-step7-build.md`).

A stub that is *stricter* than Apple's is harmless to correctness but produces a
false red, so those are corrected too — a gate that cries wolf gets ignored.

## Provenance, and the limit of this audit

**There is no macOS SDK on this machine.** `find` over the whole tree returns no
`IOKit/` directory outside `tests/kextshim/` itself, and `reference/` vendors
NVIDIA's driver, not Apple's headers. So the "real signature" column is
*transcribed from Apple's published headers*, not diffed against a local copy.

That is a weaker form of evidence than this project normally accepts, and it is
labelled as such rather than presented as settled. The re-verification recipe is
at the bottom: on any macOS host it is four `grep` commands, and it should be run
before the operator's build rather than after it.

## The table

`MATCHES` = shim agrees. `TIGHTENED` = shim was looser, corrected 2026-08-13.
`NARROWER` = shim deliberately declares less than Apple does (a subset of
overloads or enumerators), which cannot hide a defect in code that compiles
against it.

### `IOKit/IODMACommand.h`

| Symbol | Real signature | Shim | Verdict |
|---|---|---|---|
| `SegmentFunction` | `bool (*)(IODMACommand*, Segment64, void*, UInt32)` | was `IOReturn (*)(void)` | **TIGHTENED** |
| `withSpecification` arg 4 | `MappingOptions` (unscoped enum) | was `UInt32` | **TIGHTENED** |
| `withSpecification` arg 7 | `IOMapper*` | was `void*` | **TIGHTENED** |
| `OutputHost64` | `static bool (IODMACommand*, Segment64, void*, UInt32)` | was `static IOReturn (void)` | **TIGHTENED** |
| `MappingOptions` | also `kContinuous` and friends | `kMapped`/`kBypassed`/`kNonCoherent`/`kTypeMask` | NARROWER |
| `setMemoryDescriptor` | `IOReturn (const IOMemoryDescriptor*, bool = true)` | same | MATCHES |
| `clearMemoryDescriptor` | `IOReturn (bool = true)` | same | MATCHES |
| `gen64IOVMSegments` | `IOReturn (UInt64*, Segment64*, UInt32*)` | same | MATCHES |
| `Segment64` | `{ UInt64 fIOVMAddr; UInt64 fLength; }` | same | MATCHES |

### `IOKit/IOBufferMemoryDescriptor.h`

| Symbol | Real signature | Shim | Verdict |
|---|---|---|---|
| `inTaskWithPhysicalMask` arg 3 | `mach_vm_size_t capacity` | was `mach_vm_address_t` | **TIGHTENED** |
| `getCapacity` | `virtual vm_size_t () const` | was `IOByteCount` | **TIGHTENED** |
| `getBytesNoCopy` | `virtual void* ()` | same | MATCHES |
| `prepare` / `complete` | `virtual IOReturn (IODirection = kIODirectionNone)` | same | MATCHES |

Both `IOBufferMemoryDescriptor` corrections alias to the same underlying type on
LP64, so neither was a live defect. They are fixed anyway so this table can say
`MATCHES` without carrying an exception nobody would re-check — an exception in a
reference table is how the next reader learns to skim it.

### `IOKit/IOService.h`

| Symbol | Real signature | Shim | Verdict |
|---|---|---|---|
| `init` | `virtual bool (OSDictionary* = 0)` | same | MATCHES |
| `free` | `virtual void ()` | same | MATCHES |
| `probe` | `virtual IOService* (IOService*, SInt32*)` | same | MATCHES |
| `start` | `virtual bool (IOService*)` | same | MATCHES |
| `stop` | `virtual void (IOService*)` | same | MATCHES |
| `getName` | `virtual const char* () const` | same | MATCHES |
| `setProperty` | many overloads | four of them | NARROWER |
| `getDeviceMemoryWithIndex` | `virtual IODeviceMemory* (unsigned int)` | same | MATCHES |

The five driver virtuals are the ones that matter most, and they are already
load-bearing: `APPLE_KEXT_OVERRIDE` expands to `override` in this shim, so any
drift in the kext's own declarations is a compile error rather than a method
IOKit silently never calls. `NVNEG_BAD_OVERRIDE` pins that.

### `IOKit/pci/IOPCIDevice.h`

| Symbol | Real signature | Shim | Verdict |
|---|---|---|---|
| `configRead8/16/32` | `virtual UInt8/16/32 (UInt8 offset)` | same | MATCHES |
| `configWrite8/16/32` | `virtual void (UInt8, UIntN)` | same | MATCHES |
| `setMemoryEnable` | `virtual bool (bool)` | same | MATCHES |
| `setBusMasterEnable` | `virtual bool (bool)` | same | MATCHES |
| config-space offsets | IOPCIFamily published values | same | MATCHES |

### `IOKit/IOMemoryDescriptor.h`

| Symbol | Real signature | Shim | Verdict |
|---|---|---|---|
| `IOMemoryDescriptor::map` | `virtual IOMemoryMap* (IOOptionBits = 0)` | same | MATCHES |
| `IOMemoryMap::getVirtualAddress` | `virtual IOVirtualAddress ()` | same | MATCHES |
| `IOMemoryMap::getLength` | `virtual IOByteCount ()` | same | MATCHES |

### `IOKit/IODeviceMemory.h`

| Symbol | Real signature | Shim | Verdict |
|---|---|---|---|
| `IODeviceMemory` base class | `IOMemoryDescriptor` | same | MATCHES |
| `IODeviceMemory::getPhysicalAddress` | `virtual IOPhysicalAddress ()` | same | MATCHES |

The symbol set here is unchanged from what this shim always declared; what moved
is the FILE. `IODeviceMemory` used to sit inside `IOMemoryDescriptor.h`, which is
not where Apple puts it. That divergence is the same class of defect this audit
exists for, one level up from a signature: a shim whose include graph is flatter
than Apple's accepts a translation unit that omits an include Apple requires, and
the omission surfaces for the first time under Apple clang. It also failed in the
visible direction — `IntelDisplayProbe.hpp` includes the header by name, which is
correct, and got a fatal file-not-found from the shim rather than from its own
mistake.

### `IOKit/IOLib.h` and `libkern/c++/OSObject.h`

`IOLog` carries `__attribute__((format(printf, 1, 2)))`, which is the single
most valuable thing in the shim — every `NVLOG` call passes register values
through casts, and a `%llx` paired with a `UInt32` is exactly the silent bug this
project keeps hitting. `NVNEG_BAD_FORMAT` pins it.

`OSDeclareDefaultStructors` / `OSDefineMetaClassAndStructors` are structural
stand-ins rather than signature stubs; they cannot be "looser" in the sense this
audit is about. Section 5 of the gate checks the pairing separately.

## The one live defect this audit found

`NV3050Probe.cpp` passed a bare `0` for `mappingOptions`. C++ performs **no**
implicit conversion from `int` to an unscoped enum, so under Apple clang that
line is a hard error — and it parsed clean here for as long as the shim said
`UInt32`. It is now `IODMACommand::kMapped`.

That is precisely the failure mode the card describes: not a wrong value, not a
wrong call, just a stub permissive enough that the gate had nothing to say. It
would have surfaced on the operator's one build attempt, on a machine that has to
be power-cycled to try again.

## Negative controls

Each tightening is pinned by a block in `negative-control.cpp`, enabled one at a
time by `check-kext-syntax.bat` **section 11**. Every `IODMACommand` one below
**compiled clean before** the fix:

| Macro | What it plants |
|---|---|
| `NVNEG_DMA_MAPPING_INT` | bare `0` for `mappingOptions` — the live defect above |
| `NVNEG_DMA_BAD_SEGFUNC` | an unrelated nullary callback as `SegmentFunction` |
| `NVNEG_DMA_VOID_MAPPER` | an `IOPCIDevice*` where `IOMapper*` belongs |

`nvPosDmaCorrectCall()` is the paired positive, and it is **unguarded** — it
compiles on every run of the gate, including the section 10 baseline. Without it,
a shim tightened past what Apple actually accepts would turn all three controls
red and read exactly like success: "rejects everything" is indistinguishable from
"catches the defect" when only the negative half is tested.

### `IOBufferMemoryDescriptor` had none at all until 2026-08-14

The seven controls that existed before that date all probed `IODMACommand` or
`IOPCIDevice`. Nothing in `negative-control.cpp` so much as *named*
`IOBufferMemoryDescriptor` — so the whole first half of the Phase 5 allocation
path (`NV3050Probe.cpp:1209`) was unfalsifiable. The shim could have declared
any member name, any arity, any return type, and every gate would still have
printed `ok`. A stub nothing can contradict is not a stub that agrees with
Apple; it is a stub nobody has checked.

| Macro | What it plants |
|---|---|
| `NVNEG_BMD_TYPO_MEMBER` | `getBytesNoCopyTypo()` — a misspelled member name |
| `NVNEG_BMD_ARITY` | `inTaskWithPhysicalMask()` with 3 args, dropping the mask that carries the 41-bit ceiling |
| `NVNEG_BMD_VOID_PTR` | `void*` → `unsigned char*` without a cast, pinning the return type |
| `NVNEG_BMD_CONST_PREPARE` | `prepare()` through a `const` pointer, pinning it non-const |

`nvPosBmdCorrectCall()` is the paired unguarded positive, mirroring the real
call site argument for argument.

### One confusion that cannot be controlled for here

`IOOptionBits` (`unsigned long long`) and `IODirection` (`unsigned int`) are
both integer types, so passing a direction where options belong — or the
reverse — converts implicitly and diagnoses nothing. `IOTypes.h` keeps them as
distinct spellings for documentation value only. No shim can catch that; it
needs Apple's real headers, which means STEP 7. Recorded here so it is not
re-filed later as an oversight.

### The macro list itself is checked

Section 11's list lives inside a hand-typed `for %%M in (...)` loop, and it
fails silently in **both** directions: a block added to the `.cpp` but not to
the loop never runs, and a name left in the loop after its block is deleted
makes clang compile the bare baseline, exit non-zero never, and the gate print
`ok   NVNEG_GONE diagnosed` — a passing control that tests nothing.

That second direction is not hypothetical. `NVNEG_MISSING_STRUCTORS` was
removed from `negative-control.cpp` because `-fsyntax-only` never links, so it
could not fail; had its name stayed in the loop it would still be reporting
green today.

`tools/check-negative-controls.py` (gate **section 12**) derives both sets from
the two files at run time and asserts they are equal and non-empty. The gate
greps for its `VERDICT: PASS` line rather than trusting the exit code, because
an exit code alone cannot separate "compared and clean" from "found nothing to
compare". Its own negative controls are in `tests/test_check_negative_controls.py`.

## Re-verifying on a real macOS host

Run this before the Step 7 build, not after. `$SDK` is whatever
`xcrun --show-sdk-path -sdk macosx` prints.

```sh
SDK=$(xcrun --show-sdk-path -sdk macosx)
K=$SDK/System/Library/Frameworks/Kernel.framework/Headers

grep -n -A9  'withSpecification'        $K/IOKit/IODMACommand.h
grep -n -A5  'typedef bool (\*SegmentFunction)' $K/IOKit/IODMACommand.h
grep -n -A5  'inTaskWithPhysicalMask'   $K/IOKit/IOBufferMemoryDescriptor.h
grep -n      'getCapacity'              $K/IOKit/IOBufferMemoryDescriptor.h
```

Any disagreement with the table above is a finding about **this file**, and the
shim should be corrected to match the SDK rather than the other way round.
