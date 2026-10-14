// SPDX-License-Identifier: MIT
#include "IntelGtSurfaceInstall.hpp"

#include <cstdio>
#include <cstring>

static int failures=0;
static void check(bool ok,const char *what) {
    if (!ok) { std::printf("FAIL: %s\n",what); ++failures; }
}
static void check32(unsigned got,unsigned want,const char *what) {
    if (got!=want) { std::printf("FAIL: %s got=%u want=%u\n",what,got,want); ++failures; }
}

static void buildPtes(unsigned long long *ptes)
{
    unsigned long long pages[IG_GTS_ARENA_LEAVES_448];
    for (unsigned i=0;i<IG_GTS_ARENA_LEAVES_448;++i)
        pages[i]=0x00100000ull+(unsigned long long)i*4096ull;
    check(igGtSurfaceBuildPtes(pages,IG_GTS_ARENA_LEAVES_448,3u,
                               ptes,IG_GTS_ARENA_LEAVES_448)==IG_GTS_PTE_OK,
          "build exact arena PTEs");
}

static void expectRefusal(unsigned long long *pt,
                          unsigned long long *ptes,
                          unsigned count,
                          unsigned verdict,
                          const char *what)
{
    IGGtSurfaceInstallPlan p;
    std::memset(&p,0x5a,sizeof(p));
    IGGtSurfaceInstallPlan before=p;
    check(igGtSurfacePlanInstall(pt,512u,ptes,count,&p)==verdict,what);
    check(std::memcmp(&p,&before,sizeof(p))==0,"install refusal is atomic");
}

int main()
{
    unsigned long long pt[512]={};
    unsigned long long ptes[IG_GTS_ARENA_LEAVES_448]={};
    buildPtes(ptes);

    // Existing pool mappings and guard contents are outside the install span.
    for (unsigned i=0;i<IG_GTS_EXISTING_POOL_LEAVES_32;++i)
        pt[i]=0x100003ull+(unsigned long long)i*4096ull;
    for (unsigned i=IG_GTS_GUARD_FIRST_LEAF_32;
         i<IG_GTS_ARENA_FIRST_LEAF_64;++i)
        pt[i]=0xfeed000000000000ull+i;

    IGGtSurfaceInstallPlan plan={};
    check(igGtSurfacePlanInstall(pt,512u,ptes,IG_GTS_ARENA_LEAVES_448,&plan)==IG_GTSI_OK,
          "full empty arena accepted");
    check32(plan.firstLeaf,64u,"install begins at leaf 64");
    check32(plan.leafCount,448u,"install covers 448 leaves");
    check32(plan.gpuBase,0x40000u,"fixed private GPU base");
    check32(plan.bytes,0x1c0000u,"fixed arena bytes");

    unsigned long long saved[IG_GTS_ARENA_LEAVES_448];
    for (unsigned i=0;i<IG_GTS_ARENA_LEAVES_448;++i) {
        saved[i]=pt[64u+i];
        pt[64u+i]=ptes[i];
    }
    check(igGtSurfaceLeafImageMatches(pt,512u,64u,ptes,448u),
          "installed image exact readback");
    check(pt[31]!=0 && pt[32]!=0 && pt[63]!=0,
          "pool and guard leaves untouched by arena write");

    for (unsigned i=0;i<IG_GTS_ARENA_LEAVES_448;++i)
        pt[64u+i]=saved[i];
    check(igGtSurfaceLeafImageMatches(pt,512u,64u,saved,448u),
          "rollback image exact readback");

    pt[64u]=0x123ull;
    expectRefusal(pt,ptes,448u,IG_GTSI_WINDOW_OCCUPIED,
                  "occupied first arena leaf refused");
    pt[64u]=0u;
    pt[511u]=0x123ull;
    expectRefusal(pt,ptes,448u,IG_GTSI_WINDOW_OCCUPIED,
                  "occupied final arena leaf refused");
    pt[511u]=0u;

    expectRefusal(pt,ptes,447u,IG_GTSI_BAD_COUNT,
                  "partial full-arena transaction refused");

    const auto good=ptes[17];
    ptes[17]=0x00100001ull; // present but missing RW / exact PAT encoding.
    expectRefusal(pt,ptes,448u,IG_GTSI_BAD_NEW_PTE,
                  "malformed new leaf refused");
    ptes[17]=good;

    IGGtSurfaceInstallPlan untouched={};
    check(igGtSurfacePlanInstall(pt,511u,ptes,448u,&untouched)==IG_GTSI_BAD_PT_GEOMETRY,
          "wrong PT geometry refused");
    check(!igGtSurfaceLeafImageMatches(pt,511u,64u,ptes,448u),
          "readback helper rejects wrong PT geometry");

    if (failures) {
        std::printf("%d failure(s)\n",failures);
        return 1;
    }
    std::puts("Tiger Lake private-PPGTT surface install tests passed");
    return 0;
}
