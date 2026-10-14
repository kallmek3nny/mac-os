// Host tests for NVGspRadix3.hpp. No IOKit, firmware execution or GPU access.
#include <cstdio>
#include <cstring>
#include <vector>
#include "NVGspRadix3.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspRadixU32 got, NVGspRadixU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspRadixU64 got, NVGspRadixU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}
static NVGspRadixU64 getLe64(const NVGspRadixU8 *p) {
    NVGspRadixU64 v=0;
    for (unsigned i=0;i<8;++i) v|=(NVGspRadixU64)p[i]<<(i*8u);
    return v;
}

static void testOneBytePlan(void) {
    std::printf("\n1. one byte still needs three table pages plus one data page\n");
    NVGspRadix3Plan p={};
    checkU32("verdict",nvGspPlanEmbeddedRadix3(1ull,&p),NV_GSP_RADIX3_OK);
    checkU64("L0 pages",p.levelPages[0],1ull);
    checkU64("L1 pages",p.levelPages[1],1ull);
    checkU64("L2 pages",p.levelPages[2],1ull);
    checkU64("data pages",p.levelPages[3],1ull);
    checkU64("L0 offset",p.levelOffset[0],0x0000ull);
    checkU64("L1 offset",p.levelOffset[1],0x1000ull);
    checkU64("L2 offset",p.levelOffset[2],0x2000ull);
    checkU64("data offset",p.levelOffset[3],0x3000ull);
    checkU64("table pages",p.tablePages,3ull);
    checkU64("table bytes",p.tableBytes,0x3000ull);
    checkU64("allocation pages",p.allocationPages,4ull);
    checkU64("allocation bytes",p.allocationBytes,0x4000ull);
}

static void testLevelExpansion(void) {
    std::printf("\n2. 513 data pages expand the leaf table to two pages\n");
    NVGspRadix3Plan p={};
    const NVGspRadixU64 bytes=513ull*NV_GSP_RADIX3_PAGE_BYTES;
    checkU32("verdict",nvGspPlanEmbeddedRadix3(bytes,&p),NV_GSP_RADIX3_OK);
    checkU64("L0 pages",p.levelPages[0],1ull);
    checkU64("L1 pages",p.levelPages[1],1ull);
    checkU64("L2 pages",p.levelPages[2],2ull);
    checkU64("data pages",p.levelPages[3],513ull);
    checkU64("L1 offset",p.levelOffset[1],0x1000ull);
    checkU64("L2 offset",p.levelOffset[2],0x2000ull);
    checkU64("data offset",p.levelOffset[3],0x4000ull);
    checkU64("table pages",p.tablePages,4ull);
    checkU64("allocation pages",p.allocationPages,517ull);
    checkU64("allocation bytes",p.allocationBytes,0x205000ull);
}

static void testRootCeiling(void) {
    std::printf("\n3. four-level radix3 root ceiling is 512 GiB of data\n");
    NVGspRadix3Plan p={};
    const NVGspRadixU64 maxBytes=NV_GSP_RADIX3_MAX_DATA_PAGES*NV_GSP_RADIX3_PAGE_BYTES;
    checkU64("max bytes",maxBytes,0x0000008000000000ull);
    checkU32("max accepted",nvGspPlanEmbeddedRadix3(maxBytes,&p),NV_GSP_RADIX3_OK);
    checkU64("root",p.levelPages[0],1ull);
    checkU64("L1",p.levelPages[1],512ull);
    checkU64("L2",p.levelPages[2],262144ull);
    checkU64("data",p.levelPages[3],134217728ull);
    checkU64("table pages",p.tablePages,262657ull);
    checkU64("data offset",p.levelOffset[3],0x0000000040201000ull);
    checkU32("one page too many",
             nvGspPlanEmbeddedRadix3(maxBytes+1ull,&p),NV_GSP_RADIX3_TOO_LARGE);
}

static void testExactImage(void) {
    std::printf("\n4. exact table entries point through noncontiguous GPU pages\n");
    const NVGspRadixU64 dataBytes=5000ull;
    NVGspRadix3Plan p={};
    checkU32("plan",nvGspPlanEmbeddedRadix3(dataBytes,&p),NV_GSP_RADIX3_OK);
    checkU64("pages",p.allocationPages,5ull);

    const NVGspRadixU64 pages[5]={
        0x0000000100000000ull,
        0x0000000200000000ull,
        0x0000000300000000ull,
        0x0000000400000000ull,
        0x0000000500000000ull
    };
    std::vector<NVGspRadixU8> data((size_t)dataBytes);
    for (size_t i=0;i<data.size();++i) data[i]=(NVGspRadixU8)((i*29u+7u)&0xffu);
    std::vector<NVGspRadixU8> image((size_t)p.allocationBytes,0xccu);
    NVGspRadixU64 written=0;
    checkU32("build",nvGspBuildEmbeddedRadix3Image(
        &p,pages,5ull,data.data(),dataBytes,image.data(),p.allocationBytes,&written),NV_GSP_RADIX3_OK);
    checkU64("written",written,p.allocationBytes);

    checkU64("root -> L1 page",getLe64(image.data()+0x0000),pages[1]);
    checkU64("L1 -> L2 page",getLe64(image.data()+0x1000),pages[2]);
    checkU64("L2 -> data page 0",getLe64(image.data()+0x2000),pages[3]);
    checkU64("L2 -> data page 1",getLe64(image.data()+0x2008),pages[4]);
    checkU64("unused root entry zero",getLe64(image.data()+0x0008),0ull);
    checkU64("unused L1 entry zero",getLe64(image.data()+0x1008),0ull);
    checkU64("unused leaf entry zero",getLe64(image.data()+0x2010),0ull);

    check("data exact",std::memcmp(image.data()+p.levelOffset[3],data.data(),data.size())==0);
    bool padZero=true;
    for (NVGspRadixU64 i=p.levelOffset[3]+dataBytes;i<p.allocationBytes;++i)
        if (image[(size_t)i]!=0u) { padZero=false; break; }
    check("partial final page zeroed",padZero);
}

static void testRefusalsAreAtomic(void) {
    std::printf("\n5. malformed spans refuse before image mutation\n");
    NVGspRadix3Plan p={};
    checkU32("plan",nvGspPlanEmbeddedRadix3(4097ull,&p),NV_GSP_RADIX3_OK);
    NVGspRadixU64 pages[5]={0x100000000ull,0x200000000ull,0x300000000ull,0x400000000ull,0x500000000ull};
    std::vector<NVGspRadixU8> data(4097u,0x33u);
    std::vector<NVGspRadixU8> image((size_t)p.allocationBytes,0x5au);
    const std::vector<NVGspRadixU8> before=image;
    NVGspRadixU64 written=0xaaaaaaaaaaaaaaaaull;

    pages[4]+=1ull;
    checkU32("bad page address",nvGspBuildEmbeddedRadix3Image(
        &p,pages,5ull,data.data(),4097ull,image.data(),p.allocationBytes,&written),NV_GSP_RADIX3_BAD_PAGE_ADDRESS);
    check("bad page atomic",image==before);
    checkU64("written atomic",written,0xaaaaaaaaaaaaaaaaull);
    pages[4]-=1ull;

    checkU32("short image",nvGspBuildEmbeddedRadix3Image(
        &p,pages,5ull,data.data(),4097ull,image.data(),p.allocationBytes-1ull,&written),NV_GSP_RADIX3_IMAGE_SHORT);
    check("short image atomic",image==before);

    checkU32("data mismatch",nvGspBuildEmbeddedRadix3Image(
        &p,pages,5ull,data.data(),4096ull,image.data(),p.allocationBytes,&written),NV_GSP_RADIX3_DATA_SIZE_MISMATCH);
    check("data mismatch atomic",image==before);

    NVGspRadix3Plan corrupt=p;
    corrupt.levelOffset[2]+=0x1000ull;
    checkU32("corrupt plan",nvGspBuildEmbeddedRadix3Image(
        &corrupt,pages,5ull,data.data(),4097ull,image.data(),p.allocationBytes,&written),NV_GSP_RADIX3_BAD_PAGE_COUNT);
    check("corrupt plan atomic",image==before);
}

static void testVerdicts(void) {
    std::printf("\n6. verdict family\n");
    for (NVGspRadixU32 v=0u;v<NV_GSP_RADIX3_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspRadix3VerdictName(v),"unknown")!=0);
        check("success equality",nvGspRadix3Succeeded(v)==(v==NV_GSP_RADIX3_OK));
    }
    NVGspRadix3Plan p={};
    checkU32("zero size",nvGspPlanEmbeddedRadix3(0ull,&p),NV_GSP_RADIX3_ZERO_SIZE);
    checkU32("null plan",nvGspPlanEmbeddedRadix3(1ull,nullptr),NV_GSP_RADIX3_NULL_OUTPUT);
}

int main(void) {
    std::printf("NVGspRadix3.hpp -- embedded GSP firmware Radix3 image\n");
    testOneBytePlan();
    testLevelExpansion();
    testRootCeiling();
    testExactImage();
    testRefusalsAreAtomic();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
