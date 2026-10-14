// SPDX-License-Identifier: MIT
// Opt-in runtime allocation for the Intel/3050 sink candidate.
#include "IntelSinkCandidateService.hpp"

#include <IOKit/IOLib.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>
#include <mach/task.h>

#define IGSINKLOG(fmt, ...) IOLog("IntelSinkCandidate: " fmt "\n", ##__VA_ARGS__)
OSDefineMetaClassAndStructors(IntelSinkCandidateService, IOService);

namespace {
static bool numberProperty(IOService *service, const char *key, UInt64 &value)
{
    if (!service || !key) return false;
    OSNumber *number=OSDynamicCast(OSNumber,service->getProperty(key));
    if (!number) return false;
    value=number->unsigned64BitValue();
    return true;
}
}

bool IntelSinkCandidateService::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;
    fPCIDevice=nullptr;
    fBacking=nullptr;
    fPrepared=false;
    return true;
}

void IntelSinkCandidateService::releaseBacking(void)
{
    if (fPrepared && fBacking) {
        fBacking->complete(kIODirectionInOut);
        fPrepared=false;
    }
    OSSafeReleaseNULL(fBacking);
}

void IntelSinkCandidateService::publishNotReady(void)
{
    setProperty("IGSinkCandidateBackingAllocated",false);
    setProperty("IGSinkCandidateBackingContiguous",false);
    setProperty("IGSinkCandidateGgttMapped",false);
    setProperty("IGSinkCandidatePeerDmaMapped",false);
    setProperty("IGSinkCandidatePlaneProgrammed",false);
    setProperty("IGSinkCandidateProducerReady",false);
    setProperty("IGSinkCandidateScanoutReady",false);
}

void IntelSinkCandidateService::free(void)
{
    releaseBacking();
    OSSafeReleaseNULL(fPCIDevice);
    super::free();
}

IOService *IntelSinkCandidateService::probe(IOService *provider, SInt32 *score)
{
    IOService *result=super::probe(provider,score);
    if (!result) return nullptr;
    IOPCIDevice *pci=OSDynamicCast(IOPCIDevice,provider);
    if (!pci) return nullptr;
    const UInt16 vendor=pci->configRead16(kIOPCIConfigVendorID);
    const UInt16 device=pci->configRead16(kIOPCIConfigDeviceID);
    return igIsSupportedDevice(vendor,device) ? result : nullptr;
}

bool IntelSinkCandidateService::start(IOService *provider)
{
    int enabled=0;
    if (!PE_parse_boot_argn("igsink-candidate",&enabled,sizeof(enabled)) || enabled!=1)
        return false;
    if (!super::start(provider)) return false;

    fPCIDevice=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCIDevice) {
        super::stop(provider);
        return false;
    }
    fPCIDevice->retain();

    setProperty("IGSinkCandidateEnabled",true);
    publishNotReady();
    setProperty("IGSinkCandidateAddressKind","physical-candidate-not-peer-mapped");

    // Consume the read-only display probe's already-tested rehearsal instead of
    // sampling display registers a third time or reimplementing its reserved-end
    // arithmetic here. A missing probe/plan is diagnostic, not a reason to touch
    // hardware or guess a geometry.
    OSDictionary *matching=IOService::serviceMatching("IntelDisplayProbe");
    IOService *display=matching
        ? IOService::waitForMatchingService(matching,IG_SINK_CANDIDATE_DISPLAY_WAIT_NS)
        : nullptr;
    OSSafeReleaseNULL(matching);

    if (!display) {
        setProperty("IGSinkCandidateState","display-probe-unavailable");
        registerService();
        return true;
    }

    UInt64 planBase=0,planStride=0,planBytes=0,inCtl=0,inSize=0,apertureBytes=0;
    const bool havePlan=
        numberProperty(display,"IGSinkPlanBase",planBase) &&
        numberProperty(display,"IGSinkPlanStride",planStride) &&
        numberProperty(display,"IGSinkPlanBytes",planBytes) &&
        numberProperty(display,"IGSinkRehearsalInCtl",inCtl) &&
        numberProperty(display,"IGSinkRehearsalInSize",inSize) &&
        numberProperty(display,"IGSinkRehearsalInApertureBytes",apertureBytes);
    display->release();

    if (!havePlan || !planBytes || planBase>0xFFFFFFFFull ||
        planStride>0xFFFFFFFFull || planBytes>0xFFFFFFFFull ||
        inCtl>0xFFFFFFFFull || inSize>0xFFFFFFFFull || !apertureBytes) {
        setProperty("IGSinkCandidateState","rehearsal-plan-unavailable");
        registerService();
        return true;
    }

    IgSinkAllocPlan plan;
    plan.base=(IGS32)planBase;
    plan.stride=(IGS32)planStride;
    plan.size=(IGS32)planBytes;
    const IGS32 width=igFbSizeWidth((IGFB32)inSize);
    const IGS32 height=igFbSizeHeight((IGFB32)inSize);
    const IGS32 bytesPerPixel=igFbBytesPerPixel(igFbFormat((IGFB32)inCtl));

    setProperty("IGSinkCandidatePlanBase",(UInt64)plan.base,32);
    setProperty("IGSinkCandidatePlanStride",(UInt64)plan.stride,32);
    setProperty("IGSinkCandidatePlanBytes",(UInt64)plan.size,32);
    setProperty("IGSinkCandidateWidth",(UInt64)width,32);
    setProperty("IGSinkCandidateHeight",(UInt64)height,32);
    setProperty("IGSinkCandidateBytesPerPixel",(UInt64)bytesPerPixel,32);
    setProperty("IGSinkCandidateBoundApertureBytes",apertureBytes,64);

    // This is intentionally a physical-contiguity experiment, and therefore
    // opt-in. Apple explicitly discourages large contiguous allocations; failure
    // is published and the display probe remains useful. No GGTT entry or plane
    // register is written here.
    fBacking=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut|kIOMemoryPhysicallyContiguous,
        (mach_vm_size_t)plan.size,
        (mach_vm_address_t)IG_SINK_CANDIDATE_PHYS_MASK);
    if (!fBacking) {
        setProperty("IGSinkCandidateState","backing-allocation-failed");
        registerService();
        return true;
    }
    setProperty("IGSinkCandidateBackingAllocated",true);

    if (fBacking->prepare(kIODirectionInOut)!=kIOReturnSuccess) {
        setProperty("IGSinkCandidateState","backing-prepare-failed");
        releaseBacking();
        registerService();
        return true;
    }
    fPrepared=true;

    void *bytes=fBacking->getBytesNoCopy();
    if (!bytes || fBacking->getCapacity()<(vm_size_t)plan.size) {
        setProperty("IGSinkCandidateState","backing-pointer-failed");
        releaseBacking();
        registerService();
        return true;
    }
    bzero(bytes,(size_t)plan.size);

    IOByteCount segmentBytes=0;
    const UInt64 physical=(UInt64)fBacking->getPhysicalSegment(
        0,&segmentBytes,kIOMemoryMapperNone);
    const bool contiguous=physical!=0 && segmentBytes>=(IOByteCount)plan.size;
    setProperty("IGSinkCandidatePhysicalAddress",physical,64);
    setProperty("IGSinkCandidatePhysicalSegmentBytes",(UInt64)segmentBytes,64);
    setProperty("IGSinkCandidateBackingContiguous",contiguous);

    IGSinkCandidate candidate;
    const IGS32 candidateVerdict=igSinkCandidateBuild(
        &plan,width,height,bytesPerPixel,
        physical,(IGS64)plan.size,contiguous,
        apertureBytes,
        0,0,0,
        &candidate);

    setProperty("IGSinkCandidateVerdict",igSinkCandidateVerdictName(candidateVerdict));
    setProperty("IGSinkCandidatePublishVerdict",(UInt64)candidate.publishVerdict,32);
    setProperty("IGSinkCandidateGgttMapped",candidate.ggttMapped!=0);
    setProperty("IGSinkCandidatePeerDmaMapped",candidate.peerDmaMapped!=0);
    setProperty("IGSinkCandidatePlaneProgrammed",candidate.planeProgrammed!=0);
    setProperty("IGSinkCandidateProducerReady",candidate.producerReady!=0);
    setProperty("IGSinkCandidateScanoutReady",candidate.scanoutReady!=0);

    if (igSinkCandidateSucceeded(candidateVerdict)) {
        setProperty("IGSinkCandidateDescriptorPhysical",candidate.descriptor.busAddress,64);
        setProperty("IGSinkCandidateDescriptorExtent",candidate.descriptor.extentBytes,64);
        setProperty("IGSinkCandidateDescriptorStride",(UInt64)candidate.descriptor.stride,32);
        setProperty("IGSinkCandidateState","backing-owned-not-mapped");
        IGSINKLOG("owned candidate: phys=%llx bytes=%u ggtt=0x%08x; peer/GGTT/plane all disabled",
                  (unsigned long long)physical,plan.size,plan.base);
    } else {
        setProperty("IGSinkCandidateState","candidate-validation-refused");
        IGSINKLOG("candidate refused: %s publish=%u",
                  igSinkCandidateVerdictName(candidateVerdict),candidate.publishVerdict);
    }

    registerService();
    return true;
}

void IntelSinkCandidateService::stop(IOService *provider)
{
    releaseBacking();
    OSSafeReleaseNULL(fPCIDevice);
    super::stop(provider);
}
