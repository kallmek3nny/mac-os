// SPDX-License-Identifier: MIT
// Host-only layout test for the pointer-free persistent-surface ABI.
#include <cstdio>
#include "IntelGtBlitABI.h"

static int failures=0;
static void check(bool ok,const char *what)
{
    if (!ok) { std::printf("FAIL: %s\n",what); ++failures; }
}

int main()
{
    check(IGGT_SURFACE_ALLOC32_SELECTOR==2,"alloc selector stays fixed");
    check(IGGT_SURFACE_FREE_SELECTOR==3,"free selector stays fixed");
    check(IGGT_SURFACE_FILL32_SELECTOR==4,"fill selector stays fixed");
    check(IGGT_SURFACE_COPY32_SELECTOR==5,"copy selector stays fixed");
    check(IGGT_SURFACE32_ABI_VERSION==1,"persistent surface ABI version stays fixed");

    check(sizeof(IgGtSurfaceAlloc32Request)==16,"alloc request is 16 bytes");
    check(sizeof(IgGtSurfaceAlloc32Response)==8,"alloc response is one opaque u64");
    check(sizeof(IgGtSurfaceFreeRequest)==8,"free request is one opaque u64");
    check(sizeof(IgGtSurfaceFreeResponse)==8,"free response is fixed 8 bytes");
    check(sizeof(IgGtSurfaceFill32Request)==16,"fill request is fixed 16 bytes");
    check(sizeof(IgGtSurfaceFill32Response)==8,"fill response is fixed 8 bytes");
    check(sizeof(IgGtSurfaceCopy32Request)==16,"copy request is two opaque u64 handles");
    check(sizeof(IgGtSurfaceCopy32Response)==8,"copy response is fixed 8 bytes");

    IgGtSurfaceAlloc32Response response={};
    response.handle=0x00123456789abc04ull;
    check(response.handle==0x00123456789abc04ull,"opaque handle preserves all 64 bits");

    if (failures) {
        std::printf("%d failure(s)\n",failures);
        return 1;
    }
    std::puts("intel persistent surface handle ABI tests passed");
    return 0;
}
