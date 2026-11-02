//
//  IOBufferMemoryDescriptor.h -- minimal stand-in.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//
//  Added for Phase 5: NV3050Probe::loadGspFirmware() allocates a physically
//  contiguous, 256-byte-aligned kernel buffer for the GSP bootloader blob and
//  hands its physical address to NVGspBoot.hpp. Only the four members that call
//  site actually uses are declared -- inTaskWithPhysicalMask(), getBytesNoCopy(),
//  getCapacity() and prepare()/complete() -- because a wider stub would let a
//  wrong argument list through the one gate that can see it.
//

#ifndef NVSHIM_IOKIT_IOBUFFERMEMORYDESCRIPTOR_H
#define NVSHIM_IOKIT_IOBUFFERMEMORYDESCRIPTOR_H

#include <IOKit/IOTypes.h>
#include <IOKit/IOMemoryDescriptor.h>

//
// Apple spells these in <IOKit/IOMemoryDescriptor.h> via mach headers. The
// values are irrelevant to a syntax parse; what matters is that the driver
// passes kIODirectionOut where Apple expects a direction and not, say, a
// map option.
//
#define kIODirectionNone  0x0
#define kIODirectionIn    0x1
#define kIODirectionOut   0x2
#define kIODirectionOutIn 0x3
//
// kIODirectionInOut added for t-4bfda4. Transcribed from
// step7-results/apple-headers/IOKit/IOMemoryDescriptor.h:
//
//     kIODirectionInOut = kIODirectionIn  | kIODirectionOut,
//
// NARROWER: the prepare()/complete()-only flags that follow it in Apple's
// enum (kIODirectionPrepareToPhys32 and the rest) are not declared.
// phase0-kext passes none of them.
//
#define kIODirectionInOut 0x3

#define kIOMemoryPhysicallyContiguous 0x00000010

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    //
    //  Added for t-4bfda4. Eight phase0-kext sources allocate their backing
    //  with inTaskWithOptions(kernel_task, kIODirectionInOut[|...], capacity,
    //  alignment): IntelGtBringupSurface, IntelGtSurfaceStageService,
    //  NVGspBootArgsRehearsalService (twice), NVGspQueueBackingService,
    //  NVGspRmImageStageService, NVGspWprBootUcodeStageService,
    //  NVGspWprFactsObserver and NVGspWprMetaStageService.
    //
    //  Transcribed from step7-results/apple-headers/IOKit/
    //  IOBufferMemoryDescriptor.h:
    //
    //      static OSPtr<IOBufferMemoryDescriptor> inTaskWithOptions(
    //          task_t       inTask,
    //          IOOptionBits options,
    //          vm_size_t    capacity,
    //          vm_offset_t  alignment = 1);
    //
    //  OSPtr<IOBufferMemoryDescriptor> is spelled IOBufferMemoryDescriptor*
    //  on the same unverified OSPtr.h assumption as IORegistryEntry.h's
    //  iterateOver. alignment keeps Apple's default of 1 even though every
    //  call site passes it, because dropping the default would reject a call
    //  Apple accepts.
    //
    //  NARROWER: the six-argument overload that adds kernTag and userTag is
    //  not declared. phase0-kext never calls it.
    //
    static IOBufferMemoryDescriptor *inTaskWithOptions(
        task_t       inTask,
        IOOptionBits options,
        vm_size_t    capacity,
        vm_offset_t  alignment = 1);

    //
    //  capacity is mach_vm_size_t in Apple's header, not mach_vm_address_t, and
    //  getCapacity() returns vm_size_t rather than IOByteCount. Both alias to
    //  unsigned long long on LP64, so neither is a live defect -- they are
    //  corrected so the table in SIGNATURES.md can say MATCHES without an
    //  exception nobody would re-check. See SIGNATURES.md.
    //
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(
        task_t inTask,
        IOOptionBits options,
        mach_vm_size_t capacity,
        mach_vm_address_t physicalMask);

    virtual void     *getBytesNoCopy(void);
    virtual vm_size_t getCapacity(void) const;

    virtual IOReturn prepare(IODirection forDirection = kIODirectionNone);
    virtual IOReturn complete(IODirection forDirection = kIODirectionNone);
};

#endif // NVSHIM_IOKIT_IOBUFFERMEMORYDESCRIPTOR_H
