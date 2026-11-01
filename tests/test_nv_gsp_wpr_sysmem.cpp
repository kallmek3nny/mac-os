#include <cstdio>
#include <cstring>
#include "NVGspWprSysmem.hpp"

static int gChecks=0;
static int gFailures=0;

static void checkU32(const char *label, unsigned got, unsigned want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("FAIL %s: got %u want %u\n",label,got,want); }
}
static void checkU64(const char *label, unsigned long long got, unsigned long long want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("FAIL %s: got 0x%llx want 0x%llx\n",label,got,want); }
}
static void checkBool(const char *label, bool got, bool want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("FAIL %s: got %d want %d\n",label,(int)got,(int)want); }
}

static NVGspWprSysmemInput goodInput(void) {
    NVGspWprSysmemInput in={};
    in.radix3Prepared=true;
    in.radix3RootGpuAddress=0x0000001234500000ull;
    in.radix3ImageBytes=0x0000000004567890ull;
    in.bootPrepared=true;
    in.bootGpuAddress=0x0000001a00000000ull;
    in.bootImageBytes=0x6000ull;
    in.bootCodeOffset=0x1800ull;
    in.bootDataOffset=0x0800ull;
    in.bootManifestOffset=0x0000ull;
    return in;
}

static void checkAtomicRefusal(const char *label, const NVGspWprSysmemInput &in, unsigned want) {
    NVGspWprSysmemPlan out;
    std::memset(&out,0xa5,sizeof(out));
    NVGspWprSysmemPlan before=out;
    unsigned got=nvGspPlanWprSysmem(&in,&out);
    checkU32(label,got,want);
    ++gChecks;
    if (std::memcmp(&out,&before,sizeof(out))!=0) {
        ++gFailures;
        std::printf("FAIL %s mutated output on refusal\n",label);
    }
}

int main(void) {
    NVGspWprSysmemInput in=goodInput();
    NVGspWprSysmemPlan out={};
    checkU32("golden verdict",nvGspPlanWprSysmem(&in,&out),NV_GSP_WPR_SYSMEM_OK);
    checkBool("golden valid",out.valid,true);
    checkU64("radix root",out.radix3RootGpuAddress,in.radix3RootGpuAddress);
    checkU64("radix image bytes",out.radix3ImageBytes,in.radix3ImageBytes);
    checkU64("boot address",out.bootGpuAddress,in.bootGpuAddress);
    checkU64("boot image bytes",out.bootImageBytes,0x6000ull);
    checkU64("monitor code",out.bootCodeOffset,0x1800ull);
    checkU64("monitor data",out.bootDataOffset,0x0800ull);
    checkU64("manifest offset zero is valid",out.bootManifestOffset,0ull);

    checkU32("null output",nvGspPlanWprSysmem(&in,nullptr),NV_GSP_WPR_SYSMEM_NULL_OUTPUT);

    in=goodInput(); in.radix3Prepared=false;
    checkAtomicRefusal("radix stage missing",in,NV_GSP_WPR_SYSMEM_STAGE_MISSING);
    in=goodInput(); in.bootPrepared=false;
    checkAtomicRefusal("boot stage missing",in,NV_GSP_WPR_SYSMEM_STAGE_MISSING);

    in=goodInput(); in.radix3RootGpuAddress=0;
    checkAtomicRefusal("zero radix root",in,NV_GSP_WPR_SYSMEM_BAD_RADIX3);
    in=goodInput(); in.radix3ImageBytes=0;
    checkAtomicRefusal("zero radix bytes",in,NV_GSP_WPR_SYSMEM_BAD_RADIX3);
    in=goodInput(); in.bootGpuAddress=0;
    checkAtomicRefusal("zero boot address",in,NV_GSP_WPR_SYSMEM_BAD_BOOT);
    in=goodInput(); in.bootImageBytes=0;
    checkAtomicRefusal("zero boot bytes",in,NV_GSP_WPR_SYSMEM_BAD_BOOT);

    in=goodInput(); in.radix3RootGpuAddress+=1;
    checkAtomicRefusal("misaligned radix root",in,NV_GSP_WPR_SYSMEM_BAD_ALIGNMENT);
    in=goodInput(); in.bootGpuAddress+=0x800;
    checkAtomicRefusal("misaligned boot address",in,NV_GSP_WPR_SYSMEM_BAD_ALIGNMENT);

    in=goodInput(); in.bootCodeOffset=in.bootImageBytes;
    checkAtomicRefusal("code at end",in,NV_GSP_WPR_SYSMEM_BAD_OFFSETS);
    in=goodInput(); in.bootDataOffset=in.bootImageBytes+1;
    checkAtomicRefusal("data beyond end",in,NV_GSP_WPR_SYSMEM_BAD_OFFSETS);
    in=goodInput(); in.bootManifestOffset=in.bootImageBytes;
    checkAtomicRefusal("manifest at end",in,NV_GSP_WPR_SYSMEM_BAD_OFFSETS);

    in=goodInput(); in.bootGpuAddress=0xfffffffffffff000ull; in.bootImageBytes=0x2000ull;
    checkAtomicRefusal("boot range wraps",in,NV_GSP_WPR_SYSMEM_RANGE_OVERFLOW);

    // The Radix3 root is a page-table root, not a contiguous firmware base.
    // An image byte count much larger than the distance to UINT64_MAX is legal
    // here because the hierarchy holds the actual firmware page addresses.
    in=goodInput(); in.radix3RootGpuAddress=0xfffffffffffff000ull; in.radix3ImageBytes=0x400000ull;
    std::memset(&out,0,sizeof(out));
    checkU32("radix root is not contiguous range",nvGspPlanWprSysmem(&in,&out),NV_GSP_WPR_SYSMEM_OK);

    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures ? 1 : 0;
}
