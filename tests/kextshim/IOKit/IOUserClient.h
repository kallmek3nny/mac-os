//
//  IOUserClient.h -- minimal stand-in for IOKit/IOUserClient.h.
//
//  NOT APPLE'S HEADER. See IOTypes.h for the full caveat.
//
//  Added for t-fccff1. IntelGtBlitUserClient.hpp and IntelGtBringup.hpp
//  include this header by name, so IntelGtBlitUserClient, IntelGtBringup,
//  IntelGtBringupSurface, IntelGtFill and IntelGtPersistentSurfaces all
//  stopped at a fatal file-not-found before clang read any driver code.
//
//  Transcribed from xnu iokit/IOKit/IOUserClient.h (apple-oss-distributions,
//  main). Apple wraps none of it in extern "C" or __BEGIN_DECLS:
//
//      class IOUserClient : public IOService
//      virtual bool initWithTask(
//          task_t owningTask, void * securityToken, UInt32 type,
//          OSDictionary * properties);
//      virtual void free() APPLE_KEXT_OVERRIDE;
//      virtual IOReturn clientClose( void );
//      virtual IOReturn clientDied( void );
//      MIG_SERVER_ROUTINE virtual IOReturn
//      externalMethod(uint32_t selector, IOExternalMethodArguments * arguments,
//          IOExternalMethodDispatch *dispatch = NULL,
//          OSObject *target = NULL, void *reference = NULL);
//
//  plus IOExternalMethodArguments, IOExternalMethodAction and
//  IOExternalMethodDispatch, whose fields are copied below in Apple's order.
//  IOUserClient declares no start(). IntelGtBlitUserClient's start() override
//  and its IOUserClient::start() call resolve to IOService::start(), as they
//  do on macOS.
//
//  Spelling: Apple's uint32_t and uint64_t are written UInt32 and UInt64 (the
//  same widths) because the shim has no <stdint.h>. MIG_SERVER_ROUTINE comes
//  from <mach/mig.h> on macOS. It is defined empty below: it is an attribute,
//  so leaving it out does not change the signature an override must match.
//
//  mach_port_t and io_user_reference_t also come from mach headers on macOS
//  (mach/port.h and device/device_types.h), in the branch a kext build sees:
//
//      typedef struct ipc_port         *ipc_port_t;
//      typedef ipc_port_t              mach_port_t;
//      typedef uint64_t                io_user_reference_t __kernel_ptr_semantics;
//
//  There is no mach shim for them, so they are declared here behind guards a
//  future mach shim can take over. struct ipc_port stays incomplete, as it
//  does for a kext. __kernel_ptr_semantics is an annotation and is left out.
//
//  NARROWER: the 3-argument initWithTask overload, the KERNEL_PRIVATE
//  IOUserClient2022 subclass, clientMemoryForType, the async-reference helpers
//  and the rest of the class are not declared. phase0-kext overrides or calls
//  only the members below.
//

#ifndef NVSHIM_IOKIT_IOUSERCLIENT_H
#define NVSHIM_IOKIT_IOUSERCLIENT_H

#include <IOKit/IOTypes.h>
#include <IOKit/IOService.h>

#ifndef NVSHIM_MACH_PORT_T
#define NVSHIM_MACH_PORT_T
struct ipc_port;
typedef struct ipc_port *ipc_port_t;
typedef ipc_port_t       mach_port_t;
#endif // NVSHIM_MACH_PORT_T

#ifndef NVSHIM_IO_USER_REFERENCE_T
#define NVSHIM_IO_USER_REFERENCE_T
typedef UInt64 io_user_reference_t;
#endif // NVSHIM_IO_USER_REFERENCE_T

#ifndef MIG_SERVER_ROUTINE
#define MIG_SERVER_ROUTINE
#endif

struct IOExternalMethodArguments {
    UInt32               version;

    UInt32               selector;

    mach_port_t          asyncWakePort;
    io_user_reference_t *asyncReference;
    UInt32               asyncReferenceCount;

    const UInt64        *scalarInput;
    UInt32               scalarInputCount;

    const void          *structureInput;
    UInt32               structureInputSize;

    IOMemoryDescriptor  *structureInputDescriptor;

    UInt64              *scalarOutput;
    UInt32               scalarOutputCount;

    void                *structureOutput;
    UInt32               structureOutputSize;

    IOMemoryDescriptor  *structureOutputDescriptor;
    UInt32               structureOutputDescriptorSize;

    UInt32               __reservedA;

    OSObject           **structureVariableOutputData;

    UInt32               __reserved[30];
};

typedef IOReturn (*IOExternalMethodAction)(OSObject *target, void *reference,
                                           IOExternalMethodArguments *arguments);

struct IOExternalMethodDispatch {
    IOExternalMethodAction function;
    UInt32                 checkScalarInputCount;
    UInt32                 checkStructureInputSize;
    UInt32                 checkScalarOutputCount;
    UInt32                 checkStructureOutputSize;
};

class IOUserClient : public IOService {
public:
    // Apple: "Currently ignores the all args, just passes up to IOService::init()"
    virtual bool initWithTask(task_t owningTask, void *securityToken,
                              UInt32 type, OSDictionary *properties);

    virtual void free() APPLE_KEXT_OVERRIDE;

    virtual IOReturn clientClose(void);
    virtual IOReturn clientDied(void);

    MIG_SERVER_ROUTINE virtual IOReturn
    externalMethod(UInt32 selector, IOExternalMethodArguments *arguments,
                   IOExternalMethodDispatch *dispatch = nullptr,
                   OSObject *target = nullptr, void *reference = nullptr);
};

#endif // NVSHIM_IOKIT_IOUSERCLIENT_H
