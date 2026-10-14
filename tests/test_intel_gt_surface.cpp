// Host-only golden tests for the persistent Tiger Lake private-PPGTT arena.
#include <cstdio>
#include <cstring>
#include "IntelGtSurface.hpp"

static int failures=0;
static void check(bool ok,const char *what) {
    if (!ok) { std::printf("FAIL: %s\n",what); ++failures; }
}
static void check32(unsigned int got,unsigned int want,const char *what) {
    if (got!=want) {
        std::printf("FAIL: %s got=%08x want=%08x\n",what,got,want);
        ++failures;
    }
}
static void check64(unsigned long long got,unsigned long long want,const char *what) {
    if (got!=want) {
        std::printf("FAIL: %s got=%016llx want=%016llx\n",what,got,want);
        ++failures;
    }
}

static void testArenaConstants() {
    std::puts("1. arena constants and non-overlap");
    check32(IG_GTS_EXISTING_POOL_LEAVES_32,32u,"existing pool leaves");
    check32(IG_GTS_GUARD_FIRST_LEAF_32,32u,"guard starts after pool");
    check32(IG_GTS_GUARD_LEAVES_32,32u,"guard leaves");
    check32(IG_GTS_ARENA_FIRST_LEAF_64,64u,"arena first leaf");
    check32(IG_GTS_ARENA_LEAVES_448,448u,"arena leaf count");
    check32(IG_GTS_ARENA_GPU_BASE_0x40000,0x00040000u,"arena VA base");
    check32(IG_GTS_ARENA_BYTES_0x1C0000,0x001c0000u,"arena bytes");
    check32(IG_GTS_PT_GPU_LIMIT_EXCLUSIVE_0x200000,0x00200000u,"one-PT VA ceiling");
    check(IG_GTS_ARENA_FIRST_LEAF_64 >=
          IG_GTS_EXISTING_POOL_LEAVES_32+IG_GTS_GUARD_LEAVES_32,
          "arena cannot overlap pool/guard");
    check(IG_GTS_ARENA_FIRST_LEAF_64+IG_GTS_ARENA_LEAVES_448==512u,
          "arena ends at PT leaf 511");
}

static void testSurfacePlans() {
    std::puts("2. exact persistent surface plans");
    IGGtSurfacePlan p={};
    check(igGtSurfacePlan32(640u,480u,2560u,0u,&p)==IG_GTS_PLAN_OK,
          "640x480 surface accepted");
    check32(p.firstLeaf,64u,"640x480 first leaf");
    check32(p.pageCount,300u,"640x480 pages");
    check32(p.gpuAddress,0x00040000u,"640x480 private VA");
    check32(p.allocationBytes,0x0012c000u,"640x480 allocation bytes");
    check64(p.usedBytes,1228800ull,"640x480 used bytes");

    IGGtSurfacePlan last={};
    check(igGtSurfacePlan32(1u,1u,4u,447u,&last)==IG_GTS_PLAN_OK,
          "last arena page accepted");
    check32(last.firstLeaf,511u,"last leaf is 511");
    check32(last.pageCount,1u,"last plan one page");
    check32(last.gpuAddress,0x001ff000u,"last legal page VA");
    check(last.gpuAddress+last.allocationBytes==0x00200000u,
          "last surface ends exactly at PT ceiling");

    IGGtSurfacePlan full={};
    check(igGtSurfacePlan32(1024u,448u,4096u,0u,&full)==IG_GTS_PLAN_OK,
          "full 448-page arena accepted");
    check32(full.pageCount,448u,"full arena page count");
    check32(full.allocationBytes,IG_GTS_ARENA_BYTES_0x1C0000,"full arena bytes");
}

static void expectPlanRefusal(unsigned int width,unsigned int height,unsigned int pitch,
                              unsigned int offset,unsigned int verdict,const char *what) {
    IGGtSurfacePlan p;
    std::memset(&p,0x5a,sizeof(p));
    IGGtSurfacePlan before=p;
    check(igGtSurfacePlan32(width,height,pitch,offset,&p)==verdict,what);
    check(std::memcmp(&p,&before,sizeof(p))==0,"plan refusal is atomic");
}

static void testPlanRefusals() {
    std::puts("3. plan refusal edges");
    expectPlanRefusal(0u,1u,4u,0u,IG_GTS_PLAN_BAD_GEOMETRY,"zero width refused");
    expectPlanRefusal(2u,1u,4u,0u,IG_GTS_PLAN_BAD_PITCH,"row wider than pitch refused");
    expectPlanRefusal(1u,1u,6u,0u,IG_GTS_PLAN_BAD_PITCH,"unaligned pitch refused");
    expectPlanRefusal(1u,1u,4u,448u,IG_GTS_PLAN_BAD_ARENA_OFFSET,"offset 448 refused");
    expectPlanRefusal(1024u,448u,4096u,1u,IG_GTS_PLAN_ARENA_TOO_SMALL,
                      "full arena shifted by one page refused");
}

static void testFlatten() {
    std::puts("4. DMA segment flattening");
    unsigned long long pages[8]={};
    const IGGtSurfaceDmaSegment one[]={{0x00100000ull,3ull*4096ull}};
    check(igGtSurfaceFlattenSegments(one,1u,3u,pages,8u)==IG_GTS_FLAT_OK,
          "one contiguous segment accepted");
    check64(pages[0],0x00100000ull,"contiguous page 0");
    check64(pages[1],0x00101000ull,"contiguous page 1");
    check64(pages[2],0x00102000ull,"contiguous page 2");

    std::memset(pages,0,sizeof(pages));
    const IGGtSurfaceDmaSegment split[]={
        {0x00200000ull,2ull*4096ull},
        {0x00500000ull,1ull*4096ull}
    };
    check(igGtSurfaceFlattenSegments(split,2u,3u,pages,8u)==IG_GTS_FLAT_OK,
          "split segments accepted");
    check64(pages[0],0x00200000ull,"split page 0");
    check64(pages[1],0x00201000ull,"split page 1");
    check64(pages[2],0x00500000ull,"split page 2");

    const unsigned long long top=IG_GTPG_DMA_ADDR_MASK_POLICY_46;
    const IGGtSurfaceDmaSegment topPage[]={{top,4096ull}};
    check(igGtSurfaceFlattenSegments(topPage,1u,1u,pages,8u)==IG_GTS_FLAT_OK,
          "exact 46-bit policy top page accepted");
    check64(pages[0],top,"top page preserved");
}

static void expectFlatRefusal(const IGGtSurfaceDmaSegment *segments,unsigned int segmentCount,
                              unsigned int expectedPages,unsigned int capacity,
                              unsigned int verdict,const char *what) {
    unsigned long long pages[8];
    for (auto &v:pages) v=0x5a5aa5a55a5aa5a5ull;
    unsigned long long before[8];
    std::memcpy(before,pages,sizeof(pages));
    check(igGtSurfaceFlattenSegments(segments,segmentCount,expectedPages,pages,capacity)==verdict,what);
    check(std::memcmp(pages,before,sizeof(pages))==0,"flatten refusal is atomic");
}

static void testFlattenRefusals() {
    std::puts("5. DMA flatten refusal edges");
    const IGGtSurfaceDmaSegment unaligned[]={{0x00100004ull,4096ull}};
    expectFlatRefusal(unaligned,1u,1u,8u,IG_GTS_FLAT_BAD_SEGMENT,"unaligned address refused");
    const IGGtSurfaceDmaSegment shortLen[]={{0x00100000ull,4097ull}};
    expectFlatRefusal(shortLen,1u,1u,8u,IG_GTS_FLAT_BAD_SEGMENT,"unaligned length refused");
    const IGGtSurfaceDmaSegment tooFew[]={{0x00100000ull,4096ull}};
    expectFlatRefusal(tooFew,1u,2u,8u,IG_GTS_FLAT_PAGE_COUNT_MISMATCH,"short mapping refused");
    const IGGtSurfaceDmaSegment tooMany[]={{0x00100000ull,3ull*4096ull}};
    expectFlatRefusal(tooMany,1u,2u,8u,IG_GTS_FLAT_PAGE_COUNT_MISMATCH,"long mapping refused");
    const IGGtSurfaceDmaSegment crossing[]={{IG_GTPG_DMA_ADDR_MASK_POLICY_46,8192ull}};
    expectFlatRefusal(crossing,1u,2u,8u,IG_GTS_FLAT_BAD_ADDRESS,"46-bit crossing refused");
    const IGGtSurfaceDmaSegment good[]={{0x00100000ull,2ull*4096ull}};
    expectFlatRefusal(good,1u,2u,1u,IG_GTS_FLAT_OUTPUT_TOO_SMALL,"short output refused");
}

static void testPtes() {
    std::puts("6. exact Gen12 PPGTT leaf image");
    const unsigned long long pages[]={0x00100000ull,0x00500000ull,0x00800000ull};
    unsigned long long ptes[3]={};
    check(igGtSurfaceBuildPtes(pages,3u,3u,ptes,3u)==IG_GTS_PTE_OK,
          "PAT3 PTE image accepted");
    check64(ptes[0],0x0010001bull,"PTE 0 exact bits");
    check64(ptes[1],0x0050001bull,"PTE 1 exact bits");
    check64(ptes[2],0x0080001bull,"PTE 2 exact bits");

    unsigned long long poisoned[3]={0xaaaaaaaaaaaaaaaaull,0xbbbbbbbbbbbbbbbbull,0xccccccccccccccccull};
    unsigned long long before[3]; std::memcpy(before,poisoned,sizeof(poisoned));
    check(igGtSurfaceBuildPtes(pages,3u,8u,poisoned,3u)==IG_GTS_PTE_BAD_PAT,
          "invalid PAT refused");
    check(std::memcmp(poisoned,before,sizeof(poisoned))==0,"PTE PAT refusal is atomic");

    const unsigned long long badPages[]={0x00100001ull};
    check(igGtSurfaceBuildPtes(badPages,1u,0u,poisoned,3u)==IG_GTS_PTE_BAD_PAGE,
          "unaligned PTE page refused");
    check(std::memcmp(poisoned,before,sizeof(poisoned))==0,"PTE page refusal is atomic");
}

int main() {
    testArenaConstants();
    testSurfacePlans();
    testPlanRefusals();
    testFlatten();
    testFlattenRefusals();
    testPtes();
    if (failures) {
        std::printf("%d failure(s)\n",failures);
        return 1;
    }
    std::puts("Tiger Lake persistent surface arena tests passed");
    return 0;
}
