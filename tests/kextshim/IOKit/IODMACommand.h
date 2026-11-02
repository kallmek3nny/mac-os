//
//  IODMACommand.h -- minimal stand-in.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//
//  Added for Phase 5. The only reason the kext uses IODMACommand rather than
//  IOBufferMemoryDescriptor::getPhysicalSegment() is the addressing width:
//  DMATRFBASE carries physical bits 39:8 and DMATRFBASE1 continues the SHIFTED
//  value with physical bits 48:40 (NVFalcon.hpp:217, corrected 2026-08-10), and
//  the driver constrains the buffer to 41 bits as a policy ceiling or the GPU
//  reads a truncated address without faulting.
//  withSpecification()'s numAddressBits argument is where that constraint is
//  expressed, which is why this stub declares it explicitly rather than
//  defaulting it.
//

#ifndef NVSHIM_IOKIT_IODMACOMMAND_H
#define NVSHIM_IOKIT_IODMACOMMAND_H

#include <IOKit/IOTypes.h>
#include <IOKit/IOMemoryDescriptor.h>

//
//  Apple takes an IOMapper* here, not a void*. Declaring it void* accepted any
//  pointer at all, so a wrong argument would have parsed clean on Windows.
//
class IOMapper;

class IODMACommand : public OSObject {
public:
    struct Segment64 {
        UInt64 fIOVMAddr;
        UInt64 fLength;
    };

    //
    //  mappingOptions is an ENUM in Apple's header, not an integer, and that is
    //  load-bearing rather than cosmetic: C++ performs no implicit conversion
    //  from int to an unscoped enum, so a bare `0` at the call site is a hard
    //  error under Apple clang. While this shim said `UInt32` that error was
    //  invisible -- see SIGNATURES.md, and NVNEG_DMA_MAPPING_INT in
    //  negative-control.cpp, which pins it.
    //
    enum MappingOptions {
        kMapped      = 0x00000000,
        kBypassed    = 0x00000001,
        kNonCoherent = 0x00000002,
        kTypeMask    = 0x0000000F,
    };

    //
    //  Apple's real SegmentFunction. The previous `IOReturn (*)(void)` accepted
    //  ANY nullary function, so handing withSpecification() an unrelated
    //  callback would have compiled here and failed on the operator's build.
    //
    typedef bool (*SegmentFunction)(IODMACommand *target,
                                    Segment64     segment,
                                    void         *segments,
                                    UInt32        segmentIndex);

    static IODMACommand *withSpecification(
        SegmentFunction  outSegFunc,
        UInt8            numAddressBits,
        UInt64           maxSegmentSize,
        MappingOptions   mappingOptions = kMapped,
        UInt64           maxTransferSize = 0,
        UInt32           alignment = 1,
        IOMapper        *mapper = nullptr,
        void            *refCon = nullptr);

    static bool OutputHost64(IODMACommand *target,
                             Segment64     segment,
                             void         *segments,
                             UInt32        segmentIndex);

    virtual IOReturn setMemoryDescriptor(const IOMemoryDescriptor *mem,
                                         bool autoPrepare = true);
    virtual IOReturn clearMemoryDescriptor(bool autoComplete = true);

    virtual IOReturn gen64IOVMSegments(UInt64    *offset,
                                       Segment64 *segments,
                                       UInt32    *numSegments);

    //
    //  Added for t-4bfda4. Five phase0-kext sources call
    //  synchronize(kIODirectionOut) on their IODMACommand:
    //  NVGspBootArgsRehearsalService (twice), NVGspQueueBackingService,
    //  NVGspRmImageStageService, NVGspWprBootUcodeStageService and
    //  NVGspWprMetaStageService.
    //
    //  Transcribed from step7-results/apple-headers/IOKit/IODMACommand.h:
    //
    //      virtual IOReturn synchronize(IOOptionBits options);
    //
    //  The parameter is IOOptionBits, not IODirection, in Apple's header as
    //  well, because it also carries kForceDoubleBuffer, which is not a
    //  direction.
    //
    //  NARROWER: enum SynchronizeOptions (kForceDoubleBuffer) is not declared.
    //  phase0-kext never passes it.
    //
    virtual IOReturn synchronize(IOOptionBits options);
};

#endif // NVSHIM_IOKIT_IODMACOMMAND_H
