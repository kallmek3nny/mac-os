// SPDX-License-Identifier: MIT
// Host-only layout test for the pointer-free persistent-surface ABI.
#include <cstddef>
#include <cstdio>
#include "IntelGtBlitABI.h"

static int checks=0;
static int failures=0;
static void checkTrue(bool ok,const char *what)
{
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n",what); ++failures; }
}

int main()
{
    checkTrue(IGGT_SURFACE_ALLOC32_SELECTOR==2,"alloc selector stays fixed");
    checkTrue(IGGT_SURFACE_FREE_SELECTOR==3,"free selector stays fixed");
    checkTrue(IGGT_SURFACE_FILL32_SELECTOR==4,"fill selector stays fixed");
    checkTrue(IGGT_SURFACE_COPY32_SELECTOR==5,"copy selector stays fixed");
    checkTrue(IGGT_SURFACE32_ABI_VERSION==1,"persistent surface ABI version stays fixed");

    checkTrue(sizeof(IgGtSurfaceAlloc32Request)==16,"alloc request is 16 bytes");
    checkTrue(sizeof(IgGtSurfaceAlloc32Response)==8,"alloc response is one opaque u64");
    checkTrue(sizeof(IgGtSurfaceFreeRequest)==8,"free request is one opaque u64");
    checkTrue(sizeof(IgGtSurfaceFreeResponse)==8,"free response is fixed 8 bytes");
    checkTrue(sizeof(IgGtSurfaceFill32Request)==16,"fill request is fixed 16 bytes");
    checkTrue(sizeof(IgGtSurfaceFill32Response)==8,"fill response is fixed 8 bytes");
    checkTrue(sizeof(IgGtSurfaceCopy32Request)==16,"copy request is two opaque u64 handles");
    checkTrue(sizeof(IgGtSurfaceCopy32Response)==8,"copy response is fixed 8 bytes");

    IgGtSurfaceAlloc32Response response={};
    response.handle=0x00123456789abc04ull;
    checkTrue(response.handle==0x00123456789abc04ull,"opaque handle preserves all 64 bits");

    // Sizes alone would miss a field reorder; both sides copy these structs
    // byte for byte, so every field offset is part of the ABI too.
    checkTrue(offsetof(IgGtSurfaceAlloc32Request,width)==0,"alloc width at offset 0");
    checkTrue(offsetof(IgGtSurfaceAlloc32Request,height)==4,"alloc height at offset 4");
    checkTrue(offsetof(IgGtSurfaceAlloc32Request,pitch)==8,"alloc pitch at offset 8");
    checkTrue(offsetof(IgGtSurfaceAlloc32Request,reserved)==12,"alloc reserved at offset 12");
    checkTrue(offsetof(IgGtSurfaceAlloc32Response,handle)==0,"alloc handle at offset 0");
    checkTrue(offsetof(IgGtSurfaceFreeRequest,handle)==0,"free handle at offset 0");
    checkTrue(offsetof(IgGtSurfaceFreeResponse,released)==0,"free released at offset 0");
    checkTrue(offsetof(IgGtSurfaceFreeResponse,reserved)==4,"free reserved at offset 4");
    checkTrue(offsetof(IgGtSurfaceFill32Request,handle)==0,"fill handle at offset 0");
    checkTrue(offsetof(IgGtSurfaceFill32Request,color)==8,"fill color at offset 8");
    checkTrue(offsetof(IgGtSurfaceFill32Request,reserved)==12,"fill reserved at offset 12");
    checkTrue(offsetof(IgGtSurfaceFill32Response,completed)==0,"fill completed at offset 0");
    checkTrue(offsetof(IgGtSurfaceFill32Response,reserved)==4,"fill reserved at offset 4");
    checkTrue(offsetof(IgGtSurfaceCopy32Request,sourceHandle)==0,"copy source handle at offset 0");
    checkTrue(offsetof(IgGtSurfaceCopy32Request,destinationHandle)==8,"copy destination handle at offset 8");
    checkTrue(offsetof(IgGtSurfaceCopy32Response,completed)==0,"copy completed at offset 0");
    checkTrue(offsetof(IgGtSurfaceCopy32Response,reserved)==4,"copy reserved at offset 4");

    std::printf("\n%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
