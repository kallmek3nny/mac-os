// Host tests for IntelGtCopyPlan.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include <cstring>
#include "IntelGtCopyPlan.hpp"
#include "IntelGtBlitABI.h"

static int gChecks = 0;
static int gFailures = 0;

static void check(const char *label, bool ok)
{
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("  FAIL %s\n", label);
    }
}

static void checkU32(const char *label, IGCopyU32 got, IGCopyU32 want)
{
    ++gChecks;
    if (got != want) {
        ++gFailures;
        std::printf("  FAIL %s: got %u want %u\n", label, got, want);
    }
}

static void checkU64(const char *label, IGCopyU64 got, IGCopyU64 want)
{
    ++gChecks;
    if (got != want) {
        ++gFailures;
        std::printf("  FAIL %s: got %llu want %llu\n",
                    label, (unsigned long long)got, (unsigned long long)want);
    }
}

static bool sameChunk(const IGGtCopy32Chunk &a, const IGGtCopy32Chunk &b)
{
    return a.byteOffset == b.byteOffset && a.rows == b.rows && a.usedBytes == b.usedBytes;
}

static void testSurfaceExtent(void)
{
    std::printf("\n1. surface extent\n");
    IGCopyU64 bytes = 99u;
    check("32x32 pitch128 extent succeeds", igGtCopy32SurfaceBytes(32u,32u,128u,&bytes));
    checkU64("32x32 exact extent", bytes, 4096ull);

    check("64x64 pitch256 extent succeeds", igGtCopy32SurfaceBytes(64u,64u,256u,&bytes));
    checkU64("64x64 exact extent", bytes, 16384ull);

    check("padded rows extent succeeds", igGtCopy32SurfaceBytes(100u,10u,512u,&bytes));
    checkU64("padded rows exact extent", bytes, 5008ull);

    bytes = 99u;
    check("zero width refused", !igGtCopy32SurfaceBytes(0u,1u,4u,&bytes));
    checkU64("refused extent resets output", bytes, 0ull);
    check("row wider than pitch refused", !igGtCopy32SurfaceBytes(2u,1u,4u,&bytes));
}

static void testAbiCeiling(void)
{
    std::printf("\n2. fixed ABI ceiling\n");
    checkU32("ABI version", IGGT_COPY32_ABI_VERSION, 2u);
    checkU32("ABI bytes", IGGT_COPY32_MAX_BYTES, 65536u);
    IGCopyU64 bytes=0;
    check("128x128 pitch512 extent succeeds", igGtCopy32SurfaceBytes(128u,128u,512u,&bytes));
    checkU64("128x128 occupies whole ABI", bytes, 65536ull);

    IGGtCopy32Chunk chunk={};
    checkU32("128x128 first band succeeds",
             igGtCopy32PlanChunk(128u,128u,512u,0u,4096u,&chunk), IG_GTCP_PLAN_OK);
    checkU32("128x128 rows per band", chunk.rows, 8u);
    checkU32("128x128 first band fills scratch", chunk.usedBytes, 4096u);
    checkU32("128x128 needs 16 retired bands", 128u/chunk.rows, 16u);
}

static void testOnePageChunk(void)
{
    std::printf("\n3. exact one-page copy\n");
    IGGtCopy32Chunk chunk = {};
    IGCopyU32 verdict = igGtCopy32PlanChunk(32u,32u,128u,0u,4096u,&chunk);
    checkU32("plan succeeds", verdict, IG_GTCP_PLAN_OK);
    checkU64("offset zero", chunk.byteOffset, 0ull);
    checkU32("all 32 rows fit", chunk.rows, 32u);
    checkU32("uses full page", chunk.usedBytes, 4096u);
}

static void testMultiChunk(void)
{
    std::printf("\n4. multi-page surface over reused 4 KiB scratch\n");
    IGGtCopy32Chunk chunk = {};

    checkU32("64x64 chunk0 succeeds",
             igGtCopy32PlanChunk(64u,64u,256u,0u,4096u,&chunk), IG_GTCP_PLAN_OK);
    checkU32("chunk0 rows", chunk.rows, 16u);
    checkU32("chunk0 bytes", chunk.usedBytes, 4096u);
    checkU64("chunk0 offset", chunk.byteOffset, 0ull);

    checkU32("64x64 chunk1 succeeds",
             igGtCopy32PlanChunk(64u,64u,256u,16u,4096u,&chunk), IG_GTCP_PLAN_OK);
    checkU32("chunk1 rows", chunk.rows, 16u);
    checkU32("chunk1 bytes", chunk.usedBytes, 4096u);
    checkU64("chunk1 offset", chunk.byteOffset, 4096ull);

    checkU32("64x64 final chunk succeeds",
             igGtCopy32PlanChunk(64u,64u,256u,48u,4096u,&chunk), IG_GTCP_PLAN_OK);
    checkU32("final rows", chunk.rows, 16u);
    checkU64("final offset", chunk.byteOffset, 12288ull);
}

static void testPaddedBands(void)
{
    std::printf("\n5. padded row bands\n");
    IGGtCopy32Chunk chunk = {};
    checkU32("first padded band succeeds",
             igGtCopy32PlanChunk(100u,10u,512u,0u,4096u,&chunk), IG_GTCP_PLAN_OK);
    checkU32("first padded band rows", chunk.rows, 8u);
    checkU32("first padded band used", chunk.usedBytes, 3984u);
    checkU64("first padded band offset", chunk.byteOffset, 0ull);

    checkU32("second padded band succeeds",
             igGtCopy32PlanChunk(100u,10u,512u,8u,4096u,&chunk), IG_GTCP_PLAN_OK);
    checkU32("second padded band rows", chunk.rows, 2u);
    checkU32("second padded band used", chunk.usedBytes, 912u);
    checkU64("second padded band offset", chunk.byteOffset, 4096ull);
}

static void testSingleRowBands(void)
{
    std::printf("\n6. maximum-pitch single-row bands\n");
    IGGtCopy32Chunk chunk = {};
    checkU32("row0 succeeds",
             igGtCopy32PlanChunk(1u,3u,4096u,0u,4096u,&chunk), IG_GTCP_PLAN_OK);
    checkU32("row0 count", chunk.rows, 1u);
    checkU32("row0 used pixels only", chunk.usedBytes, 4u);
    checkU64("row0 offset", chunk.byteOffset, 0ull);

    checkU32("row2 succeeds",
             igGtCopy32PlanChunk(1u,3u,4096u,2u,4096u,&chunk), IG_GTCP_PLAN_OK);
    checkU32("row2 count", chunk.rows, 1u);
    checkU32("row2 used pixels only", chunk.usedBytes, 4u);
    checkU64("row2 offset", chunk.byteOffset, 8192ull);
}

static void testRefusalsAtomic(void)
{
    std::printf("\n7. refusals are atomic\n");
    const IGGtCopy32Chunk poison = {0x1111111111111111ull,0x22222222u,0x33333333u};
    IGGtCopy32Chunk out;

    checkU32("null output refused",
             igGtCopy32PlanChunk(1u,1u,4u,0u,4096u,0), IG_GTCP_PLAN_NULL_OUT);

    out = poison;
    checkU32("zero dimensions refused",
             igGtCopy32PlanChunk(0u,1u,4u,0u,4096u,&out), IG_GTCP_PLAN_BAD_DIMENSIONS);
    check("dimension refusal untouched", sameChunk(out,poison));

    out = poison;
    checkU32("zero pitch refused",
             igGtCopy32PlanChunk(1u,1u,0u,0u,4096u,&out), IG_GTCP_PLAN_BAD_PITCH);
    check("pitch refusal untouched", sameChunk(out,poison));

    out = poison;
    checkU32("row wider than pitch refused",
             igGtCopy32PlanChunk(2u,1u,4u,0u,4096u,&out), IG_GTCP_PLAN_BAD_PITCH);
    check("wide-row refusal untouched", sameChunk(out,poison));

    out = poison;
    checkU32("start at height refused",
             igGtCopy32PlanChunk(1u,3u,4u,3u,4096u,&out), IG_GTCP_PLAN_START_OUT_OF_RANGE);
    check("start refusal untouched", sameChunk(out,poison));

    out = poison;
    checkU32("scratch below row refused",
             igGtCopy32PlanChunk(1025u,1u,4100u,0u,4096u,&out), IG_GTCP_PLAN_SCRATCH_TOO_SMALL);
    check("scratch refusal untouched", sameChunk(out,poison));
}

static void testVerdicts(void)
{
    std::printf("\n8. verdict family\n");
    check("only OK succeeds", igGtCopy32PlanSucceeded(IG_GTCP_PLAN_OK));
    for (IGCopyU32 v=0u; v<IG_GTCP_PLAN_LIMIT; ++v) {
        if (v!=IG_GTCP_PLAN_OK) check("non-OK refuses", !igGtCopy32PlanSucceeded(v));
        check("every verdict named",
              std::strcmp(igGtCopy32PlanVerdictName(v),"unknown copy-plan verdict")!=0);
    }
}

int main(void)
{
    std::printf("IntelGtCopyPlan.hpp -- multi-submit 32-bpp surface chunking\n");
    testSurfaceExtent();
    testAbiCeiling();
    testOnePageChunk();
    testMultiChunk();
    testPaddedBands();
    testSingleRowBands();
    testRefusalsAtomic();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures ? 1 : 0;
}
