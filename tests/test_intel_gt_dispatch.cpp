// Host tests for IntelGtDispatch.hpp. No IOKit, MMIO or GPU required.
#include <cstdio>
#include "IntelGtDispatch.hpp"

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

static void checkU32(const char *label, IGU32 got, IGU32 want)
{
    ++gChecks;
    if (got != want) {
        ++gFailures;
        std::printf("  FAIL %s: got 0x%08X want 0x%08X\n",
                    label, (unsigned)got, (unsigned)want);
    }
}

static void fill(IGU32 *ring, IGU32 count)
{
    for (IGU32 i = 0; i < count; ++i) ring[i] = 0xA5A5A5A5u;
}

static bool allFill(const IGU32 *ring, IGU32 count)
{
    for (IGU32 i = 0; i < count; ++i) {
        if (ring[i] != 0xA5A5A5A5u) return false;
    }
    return true;
}

static void testCommandWord(void)
{
    std::printf("\n1. private-PPGTT batch-start word\n");
    checkU32("generic Gen8 BB_START", igGtMiBatchBufferStart(), 0x18800001u);
    checkU32("PPGTT BB_START", igGtdPpgttBatchStartWord(), 0x18800101u);
    checkU32("only PPGTT bit differs",
             igGtdPpgttBatchStartWord() ^ igGtMiBatchBufferStart(), 0x00000100u);
}

static void testStraightWrite(void)
{
    std::printf("\n2. empty ring straight write + qword pad\n");
    IGU32 ring[1024];
    fill(ring, 1024);
    IGU32 batchVerdict = 99u;
    IGU32 tail = 99u;

    IGU32 verdict = igGtdWritePpgttBatchStartToEmptyRing(
        ring, 1024, IG_GT_ENGINE_BLT, 0u, 0x18000ull,
        &batchVerdict, &tail);

    checkU32("ring write succeeds", verdict, IG_GTR_RING_OK);
    checkU32("batch composition succeeds", batchVerdict, IG_GTQ_BATCH_OK);
    checkU32("command word", ring[0], 0x18800101u);
    checkU32("address low", ring[1], 0x00018000u);
    checkU32("address high", ring[2], 0u);
    checkU32("qword alignment NOOP", ring[3], 0u);
    checkU32("tail advanced by 16 bytes", tail, 16u);
    checkU32("next dword untouched", ring[4], 0xA5A5A5A5u);
}

static void testWrap(void)
{
    std::printf("\n3. packet and pad wrap as one ring operation\n");
    IGU32 ring[1024];
    fill(ring, 1024);
    IGU32 batchVerdict = 0u;
    IGU32 tail = 0u;

    IGU32 verdict = igGtdWritePpgttBatchStartToEmptyRing(
        ring, 1024, IG_GT_ENGINE_BLT, 4088u, 0x18000ull,
        &batchVerdict, &tail);

    checkU32("wrapped write succeeds", verdict, IG_GTR_RING_OK);
    checkU32("wrapped batch succeeds", batchVerdict, IG_GTQ_BATCH_OK);
    checkU32("command at dword 1022", ring[1022], 0x18800101u);
    checkU32("address low at dword 1023", ring[1023], 0x00018000u);
    checkU32("address high wraps to dword 0", ring[0], 0u);
    checkU32("NOOP wraps to dword 1", ring[1], 0u);
    checkU32("tail wraps to byte 8", tail, 8u);
    checkU32("unrelated dword untouched", ring[2], 0xA5A5A5A5u);
}

static void testRefusals(void)
{
    std::printf("\n4. refusals leave the ring untouched\n");
    IGU32 ring[1024];
    IGU32 batchVerdict;
    IGU32 tail;

    fill(ring, 1024); batchVerdict = 99u; tail = 99u;
    checkU32("misaligned batch returns empty payload",
             igGtdWritePpgttBatchStartToEmptyRing(
                 ring, 1024, IG_GT_ENGINE_BLT, 0u, 0x18002ull,
                 &batchVerdict, &tail),
             IG_GTR_RING_EMPTY_PAYLOAD);
    checkU32("misaligned batch verdict preserved",
             batchVerdict, IG_GTQ_BATCH_ADDR_UNALIGNED);
    checkU32("misaligned batch tail unchanged", tail, 0u);
    check("misaligned batch writes nothing", allFill(ring, 1024));

    fill(ring, 1024); batchVerdict = 99u; tail = 99u;
    checkU32("bad engine refused by ring geometry",
             igGtdWritePpgttBatchStartToEmptyRing(
                 ring, 1024, 99u, 0u, 0x18000ull,
                 &batchVerdict, &tail),
             IG_GTR_RING_BAD_GEOMETRY);
    checkU32("batch was valid before engine refusal", batchVerdict, IG_GTQ_BATCH_OK);
    checkU32("bad-engine tail unchanged", tail, 0u);
    check("bad engine writes nothing", allFill(ring, 1024));

    fill(ring, 1024); batchVerdict = 99u; tail = 99u;
    checkU32("unaligned tail refused",
             igGtdWritePpgttBatchStartToEmptyRing(
                 ring, 1024, IG_GT_ENGINE_BLT, 4u, 0x18000ull,
                 &batchVerdict, &tail),
             IG_GTR_RING_BAD_GEOMETRY);
    checkU32("unaligned tail unchanged", tail, 4u);
    check("unaligned tail writes nothing", allFill(ring, 1024));
}

int main(void)
{
    std::printf("IntelGtDispatch.hpp -- Tiger Lake private-PPGTT ring dispatch\n");
    testCommandWord();
    testStraightWrite();
    testWrap();
    testRefusals();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
