// SPDX-License-Identifier: MIT
#include "IntelGtBlitUserClient.hpp"
#include "IntelGtBringup.hpp"
#include "IntelGtBlitABI.h"
OSDefineMetaClassAndStructors(IntelGtBlitUserClient, IOUserClient);
bool IntelGtBlitUserClient::initWithTask(task_t task, void *securityID,
                                      UInt32 type, OSDictionary *properties) {
    fOwner=nullptr;
    // Experimental privileged GPU backend is administrator-only. Kernel
    // validates the fixed ABI again; no entitlement grants arbitrary MMIO.
    if (type!=0 || clientHasPrivilege(task,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess) return false;
    return IOUserClient::initWithTask(task,securityID,type,properties);
}
bool IntelGtBlitUserClient::start(IOService *provider) {
    if (!IOUserClient::start(provider)) return false;
    fOwner=OSDynamicCast(IntelGtBringup,provider);
    if (!fOwner) return false;
    fOwner->retain();
    return true;
}
void IntelGtBlitUserClient::free() {
    if (fOwner) { fOwner->release(); fOwner=nullptr; }
    IOUserClient::free();
}
IOReturn IntelGtBlitUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}
IOReturn IntelGtBlitUserClient::externalMethod(UInt32 selector,
    IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) {
    if (!fOwner || !args || args->scalarInputCount || args->scalarOutputCount ||
        !args->structureInput || !args->structureOutput || args->asyncWakePort ||
        args->structureInputDescriptor || args->structureOutputDescriptor) return kIOReturnBadArgument;

    if (selector==IGGT_COPY32_SELECTOR) {
        if (args->structureInputSize!=sizeof(IgGtCopy32Request) ||
            args->structureOutputSize!=sizeof(IgGtCopy32Response)) return kIOReturnBadArgument;
        const auto *req=static_cast<const IgGtCopy32Request *>(args->structureInput);
        auto *response=static_cast<IgGtCopy32Response *>(args->structureOutput);
        if (!req->width || !req->height || !req->pitch || req->bytes>IGGT_COPY32_MAX_BYTES)
            return kIOReturnBadArgument;
        UInt64 row=(UInt64)req->width*4;
        UInt64 used=(UInt64)(req->height-1)*req->pitch+row;
        if (row>req->pitch || used>req->bytes || used>IGGT_COPY32_MAX_BYTES)
            return kIOReturnBadArgument;
        bzero(response,sizeof(*response));
        return fOwner->copySurface32(req->pixels,response->pixels,req->width,req->height,
                                    req->pitch,req->bytes,IGGT_COPY32_MAX_BYTES);
    }

    if (selector==IGGT_FILL32_SELECTOR) {
        if (args->structureInputSize!=sizeof(IgGtFill32Request) ||
            args->structureOutputSize!=sizeof(IgGtFill32Response)) return kIOReturnBadArgument;
        const auto *req=static_cast<const IgGtFill32Request *>(args->structureInput);
        auto *response=static_cast<IgGtFill32Response *>(args->structureOutput);
        if (!req->width || !req->height || !req->pitch || req->bytes>IGGT_FILL32_MAX_BYTES)
            return kIOReturnBadArgument;
        UInt64 row=(UInt64)req->width*4;
        UInt64 used=(UInt64)(req->height-1)*req->pitch+row;
        if (row>req->pitch || used>req->bytes || used>IGGT_FILL32_MAX_BYTES)
            return kIOReturnBadArgument;
        bzero(response,sizeof(*response));
        return fOwner->fillSurface32(response->pixels,req->width,req->height,
                                    req->pitch,IGGT_FILL32_MAX_BYTES,req->color);
    }

    if (selector==IGGT_SURFACE_ALLOC32_SELECTOR) {
        if (args->structureInputSize!=sizeof(IgGtSurfaceAlloc32Request) ||
            args->structureOutputSize!=sizeof(IgGtSurfaceAlloc32Response)) return kIOReturnBadArgument;
        const auto *req=static_cast<const IgGtSurfaceAlloc32Request *>(args->structureInput);
        auto *response=static_cast<IgGtSurfaceAlloc32Response *>(args->structureOutput);
        if (req->reserved) return kIOReturnBadArgument;
        bzero(response,sizeof(*response));
        UInt64 handle=0;
        const IOReturn result=fOwner->allocatePersistentSurface32(
            req->width,req->height,req->pitch,&handle);
        if (result==kIOReturnSuccess) response->handle=handle;
        return result;
    }

    if (selector==IGGT_SURFACE_FREE_SELECTOR) {
        if (args->structureInputSize!=sizeof(IgGtSurfaceFreeRequest) ||
            args->structureOutputSize!=sizeof(IgGtSurfaceFreeResponse)) return kIOReturnBadArgument;
        const auto *req=static_cast<const IgGtSurfaceFreeRequest *>(args->structureInput);
        auto *response=static_cast<IgGtSurfaceFreeResponse *>(args->structureOutput);
        bzero(response,sizeof(*response));
        const IOReturn result=fOwner->freePersistentSurface(req->handle);
        if (result==kIOReturnSuccess) response->released=1u;
        return result;
    }

    if (selector==IGGT_SURFACE_FILL32_SELECTOR) {
        if (args->structureInputSize!=sizeof(IgGtSurfaceFill32Request) ||
            args->structureOutputSize!=sizeof(IgGtSurfaceFill32Response)) return kIOReturnBadArgument;
        const auto *req=static_cast<const IgGtSurfaceFill32Request *>(args->structureInput);
        auto *response=static_cast<IgGtSurfaceFill32Response *>(args->structureOutput);
        if (req->reserved) return kIOReturnBadArgument;
        bzero(response,sizeof(*response));
        const IOReturn result=fOwner->fillPersistentSurface32(req->handle,req->color);
        if (result==kIOReturnSuccess) response->completed=1u;
        return result;
    }

    if (selector==IGGT_SURFACE_COPY32_SELECTOR) {
        if (args->structureInputSize!=sizeof(IgGtSurfaceCopy32Request) ||
            args->structureOutputSize!=sizeof(IgGtSurfaceCopy32Response)) return kIOReturnBadArgument;
        const auto *req=static_cast<const IgGtSurfaceCopy32Request *>(args->structureInput);
        auto *response=static_cast<IgGtSurfaceCopy32Response *>(args->structureOutput);
        bzero(response,sizeof(*response));
        const IOReturn result=fOwner->copyPersistentSurface32(
            req->sourceHandle,req->destinationHandle);
        if (result==kIOReturnSuccess) response->completed=1u;
        return result;
    }

    return kIOReturnUnsupported;
}
