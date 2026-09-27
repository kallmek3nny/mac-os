//
//  negative-control.cpp -- proof that the shim can FAIL.
//
//  A syntax gate that passes everything is worse than no gate: it converts
//  "never compiled" into "believed compiled". This file deliberately contains
//  the exact mistakes the shim is claimed to catch. check-kext-syntax.bat
//  compiles it and FAILS THE RUN IF IT SUCCEEDS.
//
//  Each block is guarded by its own macro so the harness can enable one at a
//  time and confirm each diagnostic independently, rather than trusting that
//  one error masked the rest.
//

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <libkern/c++/OSObject.h>

//
// Unguarded positive control for the two IODMACommand parameters tightened on
// 2026-08-13. Without it, NVNEG_DMA_MAPPING_INT and NVNEG_DMA_BAD_SEGFUNC below
// would both stay red if the shim were tightened past what Apple accepts --
// "rejects everything" is indistinguishable from "catches the defect" when only
// the negative half is tested. This is the call NV3050Probe.cpp actually makes.
//
IODMACommand *nvPosDmaCorrectCall(void)
{
    return IODMACommand::withSpecification(&IODMACommand::OutputHost64,
                                           41,
                                           0x10000ull,
                                           IODMACommand::kMapped,
                                           0x10000ull,
                                           256);
}

#if defined(NVNEG_BAD_OVERRIDE)
// start() takes IOService*, not IOPCIDevice*. Without APPLE_KEXT_OVERRIDE
// expanding to `override`, this would compile on macOS and then simply never
// be called -- a silent, boot-time-only failure.
class NVNegBadOverride : public IOService {
    OSDeclareDefaultStructors(NVNegBadOverride);
public:
    virtual bool start(IOPCIDevice *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVNegBadOverride, IOService);
bool NVNegBadOverride::start(IOPCIDevice *provider) { (void)provider; return true; }
#endif

#if defined(NVNEG_BAD_FORMAT)
// %llx against a 32-bit value. This is the project's recurring bug class:
// it prints a plausible-looking number and sends you debugging the wrong thing.
void nvNegBadFormat(UInt32 reg)
{
    IOLog("BOOT_0 = 0x%llx", reg);
}
#endif

//
// NVNEG_MISSING_STRUCTORS was here and has been REMOVED, because it did not
// fail. That is worth recording rather than quietly deleting.
//
// The case: OSDeclareDefaultStructors(X) in a class body with no matching
// OSDefineMetaClassAndStructors(X, Super) anywhere. The declared constructor
// and destructor then have no definition. But -fsyntax-only never links, so
// clang has nothing to complain about -- it compiled clean, exit 0, while the
// harness expected a failure.
//
// Compiling to an object and linking is not an option here: a kext links
// against Apple's kernel KPI stubs, which do not exist on this machine.
//
// So this defect class is checked textually instead, by
// tools/check-structors.py, which asserts a 1:1 pairing between every
// OSDeclareDefaultStructors and its OSDefineMetaClassAndStructors across
// phase0-kext/. check-kext-syntax.bat runs it as part of the same gate.
//

#if defined(NVNEG_TYPO_MEMBER)
// Misspelled IOPCIDevice member. A typo here is invisible to every other gate
// in this project.
UInt16 nvNegTypoMember(IOPCIDevice *pci)
{
    return pci->configRead16Typo(kIOPCIConfigVendorID);
}
#endif

#if defined(NVNEG_UNRELATED_CAST)
// OSDynamicCast to a type unrelated to the source.
class NVNegUnrelated : public OSObject {};
NVNegUnrelated *nvNegUnrelatedCast(IOPCIDevice *pci)
{
    return OSDynamicCast(NVNegUnrelated, pci);
}
#endif

#if defined(NVNEG_DMA_MAPPING_INT)
// A bare 0 for mappingOptions. This is EXACTLY what NV3050Probe.cpp:1261 said
// until 2026-08-13, and it parsed clean because this shim declared the
// parameter UInt32. Apple declares an unscoped enum, and C++ performs no
// implicit int -> enum conversion, so the operator's Apple-clang build would
// have been the first thing to report it.
IODMACommand *nvNegDmaMappingInt(void)
{
    return IODMACommand::withSpecification(&IODMACommand::OutputHost64,
                                           41,
                                           0x10000ull,
                                           0,
                                           0x10000ull,
                                           256);
}
#endif

#if defined(NVNEG_DMA_BAD_SEGFUNC)
// An unrelated nullary callback where a SegmentFunction belongs. The shim's
// old `typedef IOReturn (*SegmentFunction)(void)` accepted this, so ANY
// argument-less function would have satisfied the first parameter.
static IOReturn nvNegNullaryCallback(void) { return kIOReturnSuccess; }

IODMACommand *nvNegDmaBadSegFunc(void)
{
    return IODMACommand::withSpecification(&nvNegNullaryCallback,
                                           41,
                                           0x10000ull,
                                           IODMACommand::kMapped,
                                           0x10000ull,
                                           256);
}
#endif

#if defined(NVNEG_DMA_VOID_MAPPER)
// A void* where Apple takes IOMapper*. The shim declared void*, which accepts
// any pointer at all -- including, as here, a completely unrelated object.
IODMACommand *nvNegDmaVoidMapper(IOPCIDevice *pci)
{
    return IODMACommand::withSpecification(&IODMACommand::OutputHost64,
                                           41,
                                           0x10000ull,
                                           IODMACommand::kMapped,
                                           0x10000ull,
                                           256,
                                           pci);
}
#endif

//
// Unguarded positive control for IOBufferMemoryDescriptor, mirroring the call
// NV3050Probe.cpp:1209 actually makes. Same reasoning as nvPosDmaCorrectCall
// above: without it, the four NVNEG_BMD_* blocks below would all stay red if
// the shim were tightened past what Apple accepts, and "rejects everything"
// reads identically to "catches the defect".
//
IOBufferMemoryDescriptor *nvPosBmdCorrectCall(void)
{
    return IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionOut | kIOMemoryPhysicallyContiguous,
        (mach_vm_size_t)0x10000ull,
        (mach_vm_address_t)0x1FFFFFFFF00ull);
}

#if defined(NVNEG_BMD_TYPO_MEMBER)
// Misspelled IOBufferMemoryDescriptor member. Before this block, NOTHING in
// this file referenced IOBufferMemoryDescriptor at all -- section 11 had seven
// controls and every one of them probed IODMACommand or IOPCIDevice. Half the
// Phase 5 allocation path was therefore unfalsifiable: the shim could have
// declared any member name at all and no gate would have noticed.
void *nvNegBmdTypoMember(IOBufferMemoryDescriptor *bmd)
{
    return bmd->getBytesNoCopyTypo();
}
#endif

#if defined(NVNEG_BMD_ARITY)
// inTaskWithPhysicalMask() with three arguments. Apple takes four; dropping
// the physicalMask is the mistake that silently removes the 41-bit constraint
// the whole allocation exists to enforce (NV3050Probe.cpp:1196-1205).
IOBufferMemoryDescriptor *nvNegBmdArity(void)
{
    return IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionOut | kIOMemoryPhysicallyContiguous,
        (mach_vm_size_t)0x10000ull);
}
#endif

#if defined(NVNEG_BMD_VOID_PTR)
// getBytesNoCopy() returns void* in Apple's header, and C++ performs no
// implicit conversion from void* to a typed pointer. If this shim ever
// declared the return as unsigned char* -- which would be the convenient
// thing to do, since that is what the one call site wants -- this assignment
// would compile here and fail on the operator's machine. The real call site
// casts explicitly (NV3050Probe.cpp:952).
unsigned char *nvNegBmdVoidPtr(IOBufferMemoryDescriptor *bmd)
{
    unsigned char *bytes = bmd->getBytesNoCopy();
    return bytes;
}
#endif

#if defined(NVNEG_BMD_CONST_PREPARE)
// prepare() is non-const in Apple's header -- it mutates the wire count that
// check-prepare-balance.py (gate section 9) exists to keep balanced. Calling
// it through a const pointer must be a hard error; if the shim ever marked
// prepare() or complete() const, the balance argument would still hold but
// the shim would be describing a different object than IOKit's.
IOReturn nvNegBmdConstPrepare(const IOBufferMemoryDescriptor *bmd)
{
    return bmd->prepare(kIODirectionOut);
}
#endif

//
// NOT TESTABLE HERE, recorded so nobody files it as an oversight later:
// IOOptionBits (unsigned long long) and IODirection (unsigned int) are both
// integer types, so passing a direction where options belong -- or the
// reverse -- converts implicitly and diagnoses nothing. IOTypes.h keeps them
// distinct spellings for documentation value only. A control for that
// confusion cannot be written against any shim; it needs Apple's real headers,
// which means STEP 7.
//

// With no macro defined this file is empty apart from the two positive
// controls, and must compile cleanly -- that is the control's own control.
