// Host tests for NVGspWprMeta.hpp. No IOKit, VRAM or GPU writes required.
#include <cstdio>
#include <cstring>
#include <cstddef>
#include "NVGspWprMeta.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspWprU32 got, NVGspWprU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspWprU64 got, NVGspWprU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}

static void testAbi(void) {
    std::printf("\n1. exact GspFwWprMetaV1 ABI\n");
    checkU32("sizeof",(NVGspWprU32)sizeof(NVGspWprMetaV1),256u);
    checkU64("magic",NV_GSP_WPR_META_MAGIC,0xdc3aae21371a60b3ull);
    checkU64("revision",NV_GSP_WPR_META_REVISION,1ull);
    checkU64("verified token",NV_GSP_WPR_META_VERIFIED,0xa0a0a0a0a0a0a0a0ull);
    checkU32("magic offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,magic),0u);
    checkU32("revision offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,revision),8u);
    checkU32("radix3 addr offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,sysmemAddrOfRadix3Elf),16u);
    checkU32("bootloader addr offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,sysmemAddrOfBootloader),32u);
    checkU32("signature addr offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,sysmemAddrOfSignature),72u);
    checkU32("reserved-start offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,gspFwRsvdStart),88u);
    checkU32("WPR-start offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,gspFwWprStart),112u);
    checkU32("FW offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,gspFwOffset),136u);
    checkU32("boot-bin offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,bootBinOffset),144u);
    checkU32("WPR-end offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,gspFwWprEnd),168u);
    checkU32("boot-count offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,bootCount),200u);
    checkU32("crash-queue offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,sysmemAddrOfCrashReportQueue),224u);
    checkU32("VF count offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,gspFwHeapVfPartitionCount),240u);
    checkU32("flags offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,flags),241u);
    checkU32("paging offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,pagingConfig),242u);
    checkU32("verified offset",(NVGspWprU32)offsetof(NVGspWprMetaV1,verified),248u);
    checkU64("WPR alignment",NV_GSP_WPR_ALIGNMENT_BYTES,0x20000ull);
    checkU64("meta reserved",NV_GSP_WPR_META_RESERVED_BYTES,0x100000ull);
}

static NVGspWprLayoutInput goldenInput(void) {
    NVGspWprLayoutInput in={};
    in.fbSize=0x0000000200000000ull;             // 8 GiB
    in.vbiosReservedOffset=0x00000001ff000000ull;
    in.wprEndMargin=0x00200000ull;
    in.frtsSize=0x00200000ull;
    in.bootloaderSize=0x00180000ull;
    in.radix3ElfSize=0x03400000ull;
    in.nonWprHeapRawBytes=0x00180000ull;
    in.wprHeapBytes=0x02000000ull;
    return in;
}

static void testGoldenLayout(void) {
    std::printf("\n2. exact TU102/GA107 placement order\n");
    const NVGspWprLayoutInput in=goldenInput();
    NVGspWprLayoutPlan p={};
    checkU32("verdict",nvGspPlanWprLayout(&in,&p),NV_GSP_WPR_PLAN_OK);
    checkU64("WPR end",p.gspFwWprEnd,0x00000001fee00000ull);
    checkU64("FRTS offset",p.frtsOffset,0x00000001fec00000ull);
    checkU64("boot bin",p.bootBinOffset,0x00000001fea80000ull);
    checkU64("FW ELF",p.gspFwOffset,0x00000001fb680000ull);
    checkU64("post-WPR heap input",p.postWprHeapSize,0x04a00000ull);
    checkU64("metadata reserve",p.wprMetaReservedBytes,0x00100000ull);
    checkU64("non-WPR heap size",p.nonWprHeapSize,0x00200000ull);
    checkU64("pre-WPR heap input",p.preWprHeapSize,0x00300000ull);
    checkU64("WPR heap offset",p.gspFwHeapOffset,0x00000001f9600000ull);
    checkU64("WPR heap size",p.gspFwHeapSize,0x02000000ull);
    checkU64("WPR start",p.gspFwWprStart,0x00000001f9500000ull);
    checkU64("non-WPR heap offset",p.nonWprHeapOffset,0x00000001f9300000ull);
    checkU64("reserved start",p.gspFwRsvdStart,0x00000001f9300000ull);
    check("WPR start 128K aligned",(p.gspFwWprStart&0x1ffffull)==0ull);
    check("WPR end 128K aligned",(p.gspFwWprEnd&0x1ffffull)==0ull);
    check("heap 1M aligned",(p.gspFwHeapOffset&0xfffffull)==0ull);
    check("ELF 64K aligned",(p.gspFwOffset&0xffffull)==0ull);
    check("boot 4K aligned",(p.bootBinOffset&0xfffull)==0ull);
}

static void testFRTSZero(void) {
    std::printf("\n3. zero-size FRTS leaves boot placement at WPR end\n");
    NVGspWprLayoutInput in=goldenInput();
    in.frtsSize=0ull;
    NVGspWprLayoutPlan p={};
    checkU32("verdict",nvGspPlanWprLayout(&in,&p),NV_GSP_WPR_PLAN_OK);
    checkU64("FRTS offset equals end",p.frtsOffset,p.gspFwWprEnd);
    check("boot below end",p.bootBinOffset<p.gspFwWprEnd);
}

static void testAtomicRefusals(void) {
    std::printf("\n4. invalid geometry refuses without output mutation\n");
    NVGspWprLayoutPlan poison;
    std::memset(&poison,0xa5,sizeof(poison));
    const NVGspWprLayoutPlan before=poison;
    NVGspWprLayoutInput in=goldenInput();

    checkU32("null input",nvGspPlanWprLayout(nullptr,&poison),NV_GSP_WPR_PLAN_NULL_INPUT);
    check("null input atomic",std::memcmp(&poison,&before,sizeof(poison))==0);
    checkU32("null output",nvGspPlanWprLayout(&in,nullptr),NV_GSP_WPR_PLAN_NULL_OUTPUT);

    in.fbSize=0ull;
    checkU32("zero framebuffer",nvGspPlanWprLayout(&in,&poison),NV_GSP_WPR_PLAN_BAD_FRAMEBUFFER);
    check("fb atomic",std::memcmp(&poison,&before,sizeof(poison))==0);

    in=goldenInput(); in.vbiosReservedOffset=in.fbSize+1ull;
    checkU32("VBIOS outside FB",nvGspPlanWprLayout(&in,&poison),NV_GSP_WPR_PLAN_BAD_VBIOS_RESERVATION);

    in=goldenInput(); in.wprEndMargin=in.vbiosReservedOffset;
    checkU32("margin underflow",nvGspPlanWprLayout(&in,&poison),NV_GSP_WPR_PLAN_UNDERFLOW);

    in=goldenInput(); in.bootloaderSize=0ull;
    checkU32("zero bootloader",nvGspPlanWprLayout(&in,&poison),NV_GSP_WPR_PLAN_BAD_IMAGE_SIZE);

    in=goldenInput(); in.radix3ElfSize=0ull;
    checkU32("zero ELF",nvGspPlanWprLayout(&in,&poison),NV_GSP_WPR_PLAN_BAD_IMAGE_SIZE);

    in=goldenInput(); in.wprHeapBytes=0ull;
    checkU32("zero heap",nvGspPlanWprLayout(&in,&poison),NV_GSP_WPR_PLAN_BAD_HEAP_SIZE);

    in=goldenInput(); in.frtsSize=in.vbiosReservedOffset;
    checkU32("FRTS underflow",nvGspPlanWprLayout(&in,&poison),NV_GSP_WPR_PLAN_UNDERFLOW);
    check("all refusals atomic",std::memcmp(&poison,&before,sizeof(poison))==0);
}

static void testVerdicts(void) {
    std::printf("\n5. verdict family\n");
    for (NVGspWprU32 v=0u;v<NV_GSP_WPR_PLAN_LIMIT;++v) {
        check("named",std::strcmp(nvGspWprPlanVerdictName(v),"unknown")!=0);
        check("success",nvGspWprPlanSucceeded(v)==(v==NV_GSP_WPR_PLAN_OK));
    }
}

int main(void) {
    std::printf("NVGspWprMeta.hpp -- TU102/GA107 WPR metadata layout\n");
    testAbi();
    testGoldenLayout();
    testFRTSZero();
    testAtomicRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
