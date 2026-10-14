// Host tests for NVGspLibosInit.hpp. No IOKit, mailbox or hardware required.
#include <cstdio>
#include <cstring>
#include <cstddef>
#include "NVGspLibosInit.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspLibosU32 got, NVGspLibosU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspLibosU64 got, NVGspLibosU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}

static NVGspLibosMemoryRegion region(const char *id, NVGspLibosU64 pa, NVGspLibosU64 bytes) {
    NVGspLibosMemoryRegion r={};
    r.id8=nvGspGenerateInitArgId(id);
    r.pa=pa;
    r.size=bytes;
    r.kind=NV_GSP_LIBOS_REGION_CONTIGUOUS;
    r.loc=NV_GSP_LIBOS_REGION_LOC_SYSMEM;
    return r;
}

static void testAbi(void) {
    std::printf("\n1. LibOS region-list ABI and GA107 mailbox registers\n");
    checkU32("region sizeof",(NVGspLibosU32)sizeof(NVGspLibosMemoryRegion),32u);
    checkU32("list bytes",NV_GSP_LIBOS_INIT_LIST_BYTES,4096u);
    checkU32("max regions",NV_GSP_LIBOS_INIT_MAX_REGIONS,128u);
    checkU32("mailbox0",NV_GSP_LIBOS_MAILBOX0_REG_TU102,0x00110040u);
    checkU32("mailbox1",NV_GSP_LIBOS_MAILBOX1_REG_TU102,0x00110044u);
    checkU64("RMARGS id",NV_GSP_LIBOS_RMARGS_ID,0x0000524d41524753ull);
    checkU32("id offset",(NVGspLibosU32)offsetof(NVGspLibosMemoryRegion,id8),0u);
    checkU32("pa offset",(NVGspLibosU32)offsetof(NVGspLibosMemoryRegion,pa),8u);
    checkU32("size offset",(NVGspLibosU32)offsetof(NVGspLibosMemoryRegion,size),16u);
    checkU32("kind offset",(NVGspLibosU32)offsetof(NVGspLibosMemoryRegion,kind),24u);
    checkU32("loc offset",(NVGspLibosU32)offsetof(NVGspLibosMemoryRegion,loc),25u);
}

static void testAppendAfterExistingRegions(void) {
    std::printf("\n2. RMARGS appends after existing dense LibOS regions\n");
    NVGspLibosMemoryRegion list[NV_GSP_LIBOS_INIT_MAX_REGIONS]={};
    list[0]=region("LOG0",0x10000000ull,0x2000ull);
    list[1]=region("LOG1",0x20000000ull,0x3000ull);
    const NVGspLibosMemoryRegion before0=list[0], before1=list[1];

    NVGspLibosInitPlan p={};
    checkU32("verdict",nvGspApplyRmArgsRegion(list,sizeof(list),0x0000001234500000ull,4096ull,&p),NV_GSP_LIBOS_INIT_OK);
    checkU32("index",p.rmArgsIndex,2u);
    checkU32("terminator",p.terminatorIndex,3u);
    checkU32("before count",p.regionsBeforeRmArgs,2u);
    checkU32("used bytes",p.usedBytesAfterAppend,96u);
    check("prefix 0 preserved",std::memcmp(&list[0],&before0,sizeof(before0))==0);
    check("prefix 1 preserved",std::memcmp(&list[1],&before1,sizeof(before1))==0);
    checkU64("rmargs id",list[2].id8,NV_GSP_LIBOS_RMARGS_ID);
    checkU64("rmargs address",list[2].pa,0x0000001234500000ull);
    checkU64("rmargs size",list[2].size,4096ull);
    checkU32("rmargs kind",list[2].kind,NV_GSP_LIBOS_REGION_CONTIGUOUS);
    checkU32("rmargs loc",list[2].loc,NV_GSP_LIBOS_REGION_LOC_SYSMEM);
    check("terminator remains zero",nvGspLibosRegionEmpty(list[3]));
}

static void testEmptyList(void) {
    std::printf("\n3. empty list places RMARGS in slot zero and leaves slot one zero\n");
    NVGspLibosMemoryRegion list[NV_GSP_LIBOS_INIT_MAX_REGIONS]={};
    NVGspLibosInitPlan p={};
    checkU32("verdict",nvGspApplyRmArgsRegion(list,sizeof(list),0x40000000ull,4096ull,&p),NV_GSP_LIBOS_INIT_OK);
    checkU32("index",p.rmArgsIndex,0u);
    checkU32("terminator",p.terminatorIndex,1u);
    checkU64("slot0 id",list[0].id8,NV_GSP_LIBOS_RMARGS_ID);
    check("slot1 empty",nvGspLibosRegionEmpty(list[1]));
}

static void testRefusalsAtomic(void) {
    std::printf("\n4. malformed lists and RMARGS inputs refuse atomically\n");
    NVGspLibosMemoryRegion list[NV_GSP_LIBOS_INIT_MAX_REGIONS]={};
    list[0]=region("LOG0",0x10000000ull,0x1000ull);
    NVGspLibosMemoryRegion before[NV_GSP_LIBOS_INIT_MAX_REGIONS];
    std::memcpy(before,list,sizeof(list));
    NVGspLibosInitPlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspLibosInitPlan beforePlan=p;

    checkU32("misaligned address",nvGspApplyRmArgsRegion(list,sizeof(list),0x1234500080ull,4096ull,&p),NV_GSP_LIBOS_INIT_BAD_RMARGS_ADDRESS);
    check("misaligned list atomic",std::memcmp(list,before,sizeof(list))==0);
    check("misaligned plan atomic",std::memcmp(&p,&beforePlan,sizeof(p))==0);
    checkU32("zero address",nvGspApplyRmArgsRegion(list,sizeof(list),0ull,4096ull,&p),NV_GSP_LIBOS_INIT_BAD_RMARGS_ADDRESS);
    checkU32("bad size",nvGspApplyRmArgsRegion(list,sizeof(list),0x1234500000ull,8192ull,&p),NV_GSP_LIBOS_INIT_BAD_RMARGS_SIZE);
    checkU32("short list",nvGspApplyRmArgsRegion(list,4095u,0x1234500000ull,4096ull,&p),NV_GSP_LIBOS_INIT_LIST_TOO_SHORT);
    checkU32("null list",nvGspApplyRmArgsRegion(nullptr,sizeof(list),0x1234500000ull,4096ull,&p),NV_GSP_LIBOS_INIT_NULL_LIST);
    checkU32("null plan",nvGspApplyRmArgsRegion(list,sizeof(list),0x1234500000ull,4096ull,nullptr),NV_GSP_LIBOS_INIT_NULL_OUTPUT);

    NVGspLibosMemoryRegion duplicate[NV_GSP_LIBOS_INIT_MAX_REGIONS]={};
    duplicate[0]=region("RMARGS",0x50000000ull,4096ull);
    NVGspLibosMemoryRegion duplicateBefore[NV_GSP_LIBOS_INIT_MAX_REGIONS];
    std::memcpy(duplicateBefore,duplicate,sizeof(duplicate));
    checkU32("duplicate RMARGS",nvGspApplyRmArgsRegion(duplicate,sizeof(duplicate),0x60000000ull,4096ull,&p),NV_GSP_LIBOS_INIT_DUPLICATE_RMARGS);
    check("duplicate atomic",std::memcmp(duplicate,duplicateBefore,sizeof(duplicate))==0);

    NVGspLibosMemoryRegion sparse[NV_GSP_LIBOS_INIT_MAX_REGIONS]={};
    sparse[0]=region("LOG0",0x10000000ull,4096ull);
    sparse[2]=region("LOG2",0x20000000ull,4096ull);
    checkU32("sparse",nvGspApplyRmArgsRegion(sparse,sizeof(sparse),0x60000000ull,4096ull,&p),NV_GSP_LIBOS_INIT_SPARSE_LIST);

    NVGspLibosMemoryRegion full[NV_GSP_LIBOS_INIT_MAX_REGIONS]={};
    for (NVGspLibosU32 i=0u;i<NV_GSP_LIBOS_INIT_MAX_REGIONS-1u;++i) {
        full[i]=region("LOG",0x10000000ull+(NVGspLibosU64)i*0x1000ull,4096ull);
        full[i].id8+=(NVGspLibosU64)i+1ull;
    }
    checkU32("no terminator after append",nvGspApplyRmArgsRegion(full,sizeof(full),0x70000000ull,4096ull,&p),NV_GSP_LIBOS_INIT_LIST_FULL);
}

static void testMailboxPlan(void) {
    std::printf("\n5. TU102/GA107 mailbox handoff is low dword then high dword\n");
    NVGspLibosMailboxPlan p={};
    const NVGspLibosU64 addr=0x00000012abcdf000ull;
    checkU32("verdict",nvGspPlanLibosMailboxWrites(addr,&p),NV_GSP_LIBOS_MAILBOX_OK);
    checkU64("address",p.listGpuAddress,addr);
    checkU32("write0 reg",p.writes[0].reg,0x00110040u);
    checkU32("write0 value",p.writes[0].value,0xabcdf000u);
    checkU32("write1 reg",p.writes[1].reg,0x00110044u);
    checkU32("write1 value",p.writes[1].value,0x00000012u);

    NVGspLibosMailboxPlan poison;
    std::memset(&poison,0xa5,sizeof(poison));
    const NVGspLibosMailboxPlan before=poison;
    checkU32("misaligned",nvGspPlanLibosMailboxWrites(addr+1ull,&poison),NV_GSP_LIBOS_MAILBOX_BAD_ADDRESS);
    check("mailbox atomic",std::memcmp(&poison,&before,sizeof(poison))==0);
    checkU32("zero",nvGspPlanLibosMailboxWrites(0ull,&poison),NV_GSP_LIBOS_MAILBOX_BAD_ADDRESS);
    checkU32("null",nvGspPlanLibosMailboxWrites(addr,nullptr),NV_GSP_LIBOS_MAILBOX_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n6. verdict families\n");
    for (NVGspLibosU32 v=0u;v<NV_GSP_LIBOS_INIT_LIMIT;++v) {
        check("init named",std::strcmp(nvGspLibosInitVerdictName(v),"unknown")!=0);
        check("init success",nvGspLibosInitSucceeded(v)==(v==NV_GSP_LIBOS_INIT_OK));
    }
    for (NVGspLibosU32 v=0u;v<NV_GSP_LIBOS_MAILBOX_LIMIT;++v) {
        check("mailbox named",std::strcmp(nvGspLibosMailboxVerdictName(v),"unknown")!=0);
        check("mailbox success",nvGspLibosMailboxSucceeded(v)==(v==NV_GSP_LIBOS_MAILBOX_OK));
    }
}

int main(void) {
    std::printf("NVGspLibosInit.hpp -- LibOS region list + mailbox handoff\n");
    testAbi();
    testAppendAfterExistingRegions();
    testEmptyList();
    testRefusalsAtomic();
    testMailboxPlan();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
