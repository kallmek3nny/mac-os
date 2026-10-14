// Host tests for NVAmpereSubmit.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "NVAmpereSubmit.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVSubmitU32 got, NVSubmitU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVSubmitU64 got, NVSubmitU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,(unsigned long long)got,(unsigned long long)want); }
}

static void testConstantsAndHeaders(void) {
    std::printf("\n1. GA107 class and packet constants\n");
    checkU32("Ampere channel class",NV_SUBMIT_C56F_CLASS_ID,0x0000c56fu);
    checkU32("Ampere copy class",NV_COPY_C7B5_CLASS_ID,0x0000c7b5u);
    checkU32("fixed CE subchannel",NV_SUBMIT_CE_SUBCHANNEL,4u);
    checkU32("host subchannel",NV_SUBMIT_HOST_SUBCHANNEL,0u);
    checkU32("push dwords",NV_SUBMIT_COPY_PUSH_DWORDS,14u);
    checkU32("push bytes",NV_SUBMIT_COPY_PUSH_BYTES,56u);

    NVSubmitU32 h=0u;
    checkU32("SET_OBJECT header verdict",
        nvSubmitIncHeader(0u,0x000u,1u,&h),NV_SUBMIT_PLAN_OK);
    checkU32("SET_OBJECT header",h,0x20010000u);
    checkU32("dst phys header verdict",
        nvSubmitIncHeader(4u,0x264u,1u,&h),NV_SUBMIT_PLAN_OK);
    checkU32("dst phys header",h,0x20018099u);
    checkU32("offset block header verdict",
        nvSubmitIncHeader(4u,0x400u,4u,&h),NV_SUBMIT_PLAN_OK);
    checkU32("offset block header",h,0x20048100u);
    checkU32("line block header verdict",
        nvSubmitIncHeader(4u,0x418u,2u,&h),NV_SUBMIT_PLAN_OK);
    checkU32("line block header",h,0x20028106u);
    checkU32("launch header verdict",
        nvSubmitIncHeader(4u,0x300u,1u,&h),NV_SUBMIT_PLAN_OK);
    checkU32("launch header",h,0x200180c0u);
}

static void testGoldenSubmission(void) {
    std::printf("\n2. complete push image and GPFIFO golden vector\n");
    const NVSubmitU64 src=0x0000123456789000ull;
    const NVSubmitU64 dst=0x0000012345600000ull;
    const NVSubmitU64 pushVa=0x0000001234567000ull;
    NVAmpereSubmissionPlan p={};
    checkU32("plan verdict",
        nvAmperePlanCopySubmission(src,dst,0x1000u,pushVa,&p),NV_SUBMIT_PLAN_OK);
    checkU32("plan dwords",p.pushDwords,14u);
    checkU32("plan bytes",p.pushBytes,56u);
    checkU64("plan push VA",p.pushGpuVa,pushVa);
    checkU64("GPFIFO exact",p.gpfifoEntry,0x0000381234567000ull);

    const NVSubmitU32 expected[14]={
        0x20010000u,0x0000c7b5u,
        0x20018099u,0x00000001u,
        0x20048100u,0x00001234u,0x56789000u,0x00000123u,0x45600000u,
        0x20028106u,0x00001000u,0x00000001u,
        0x200180c0u,0x00002186u
    };
    for (NVSubmitU32 i=0;i<14u;++i) {
        char label[64]; std::snprintf(label,sizeof(label),"push[%u]",i);
        checkU32(label,p.push[i],expected[i]);
    }
    checkU32("launch header is penultimate",p.push[12],0x200180c0u);
    checkU32("launch value is final",p.push[13],NV_COPY_C7B5_LAUNCH_REHEARSAL);
}

static void testGpfifoEdges(void) {
    std::printf("\n3. GPFIFO field/range validation\n");
    NVSubmitU64 entry=0xfeedfacecafebeefull;
    checkU32("sample entry",
        nvSubmitGpfifoEntry(0x0000001234567000ull,56u,&entry),NV_SUBMIT_PLAN_OK);
    checkU64("sample exact",entry,0x0000381234567000ull);

    const NVSubmitU64 maxAlignedStart=(NV_SUBMIT_GPFIFO_VA_LIMIT_40-(NVSubmitU64)56u+1ull)&~3ull;
    checkU32("last fitting aligned range",
        nvSubmitGpfifoEntry(maxAlignedStart,56u,&entry),NV_SUBMIT_PLAN_OK);

    entry=0x1122334455667788ull;
    checkU32("zero VA refused",nvSubmitGpfifoEntry(0u,56u,&entry),NV_SUBMIT_PLAN_ZERO_PUSH_VA);
    checkU64("zero VA leaves output",entry,0x1122334455667788ull);
    checkU32("unaligned VA refused",nvSubmitGpfifoEntry(0x1002ull,56u,&entry),NV_SUBMIT_PLAN_PUSH_VA_UNALIGNED);
    checkU64("unaligned leaves output",entry,0x1122334455667788ull);
    checkU32("40-bit crossing refused",
        nvSubmitGpfifoEntry(maxAlignedStart+4ull,56u,&entry),NV_SUBMIT_PLAN_PUSH_VA_RANGE);
    checkU64("range refusal leaves output",entry,0x1122334455667788ull);
    checkU32("non-dword length refused",nvSubmitGpfifoEntry(0x1000ull,55u,&entry),NV_SUBMIT_PLAN_PUSH_LENGTH);
    checkU64("length refusal leaves output",entry,0x1122334455667788ull);
}

static void testAtomicSubmissionRefusals(void) {
    std::printf("\n4. full-plan refusals are atomic\n");
    NVAmpereSubmissionPlan p;
    std::memset(&p,0xa5,sizeof(p));
    NVAmpereSubmissionPlan before=p;

    checkU32("bad copy source mapped to copy refusal",
        nvAmperePlanCopySubmission(0ull,0x0000012345600000ull,0x1000u,
                                   0x0000001234567000ull,&p),
        NV_SUBMIT_PLAN_COPY_REFUSED);
    check("copy refusal leaves plan untouched",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("bad push alignment refused",
        nvAmperePlanCopySubmission(0x0000123456789000ull,0x0000012345600000ull,0x1000u,
                                   0x0000001234567002ull,&p),
        NV_SUBMIT_PLAN_PUSH_VA_UNALIGNED);
    check("push refusal leaves plan untouched",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("null plan refused",
        nvAmperePlanCopySubmission(0x1000ull,0x2000ull,4u,0x4000ull,nullptr),
        NV_SUBMIT_PLAN_NULL_OUTPUT);
}

static void testHeaderRefusalsAndVerdicts(void) {
    std::printf("\n5. header refusals and verdict family\n");
    NVSubmitU32 h=0xdeadbeefu;
    checkU32("unaligned method",nvSubmitIncHeader(4u,0x401u,1u,&h),NV_SUBMIT_PLAN_BAD_METHOD);
    checkU32("method refusal leaves header",h,0xdeadbeefu);
    checkU32("subchannel overflow",nvSubmitIncHeader(8u,0x400u,1u,&h),NV_SUBMIT_PLAN_BAD_SUBCHANNEL);
    checkU32("subchannel refusal leaves header",h,0xdeadbeefu);
    checkU32("zero count",nvSubmitIncHeader(4u,0x400u,0u,&h),NV_SUBMIT_PLAN_BAD_COUNT);
    checkU32("count refusal leaves header",h,0xdeadbeefu);
    checkU32("method range",nvSubmitIncHeader(4u,0x4000u,1u,&h),NV_SUBMIT_PLAN_BAD_METHOD);
    checkU32("range refusal leaves header",h,0xdeadbeefu);

    for (NVSubmitU32 v=0u;v<NV_SUBMIT_PLAN_LIMIT;++v) {
        check("verdict named",std::strcmp(nvSubmitPlanVerdictName(v),"unknown")!=0);
        check("success equality",nvSubmitPlanSucceeded(v)==(v==NV_SUBMIT_PLAN_OK));
    }
}

int main(void) {
    std::printf("NVAmpereSubmit.hpp -- GA107 pushbuffer/GPFIFO rehearsal\n");
    testConstantsAndHeaders();
    testGoldenSubmission();
    testGpfifoEdges();
    testAtomicSubmissionRefusals();
    testHeaderRefusalsAndVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
