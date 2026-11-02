// SPDX-License-Identifier: MIT
// Host-only tests for the opaque persistent-surface handle allocator.
#include <cstdio>
#include <cstring>
#include "IntelGtSurfaceHandles.hpp"

static int checks = 0;
static int failures = 0;

static void checkBool(bool ok, const char *what)
{
    ++checks;
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static void checkU32(unsigned int got, unsigned int want, const char *what)
{
    ++checks;
    if (got != want) {
        std::printf("FAIL: %s got=%08x want=%08x\n", what, got, want);
        ++failures;
    }
}

static void checkU64(IGSurfaceU64 got, IGSurfaceU64 want, const char *what)
{
    ++checks;
    if (got != want) {
        std::printf("FAIL: %s got=%016llx want=%016llx\n", what,
                    (unsigned long long)got, (unsigned long long)want);
        ++failures;
    }
}

static IGSurfaceU64 allocatePages(IGGtSurfaceHandleArena &arena, unsigned pages)
{
    IGSurfaceU64 handle = 0ull;
    const unsigned verdict = igGtSurfaceHandleAllocate(
        &arena, 1u, pages, 4096u, &handle);
    checkU32(verdict, IG_GTH_OK, "page-count allocation succeeds");
    return handle;
}

static IGGtSurfaceHandleView resolve(IGGtSurfaceHandleArena &arena, IGSurfaceU64 handle)
{
    IGGtSurfaceHandleView view = {};
    checkU32(igGtSurfaceHandleResolve(&arena, handle, &view), IG_GTH_OK,
            "resolve succeeds");
    return view;
}

static void testEncodingAndInit()
{
    std::puts("1. opaque 64-bit handle encoding and clean arena");
    IGGtSurfaceHandleArena arena;
    std::memset(&arena, 0xa5, sizeof(arena));
    igGtSurfaceHandleArenaInit(&arena);

    for (unsigned i = 0; i < IG_GTH_BITMAP_WORDS_14; ++i)
        checkU32(arena.pageBitmap[i], 0u, "bitmap word starts clear");
    for (unsigned i = 0; i < IG_GTH_MAX_SLOTS_16; ++i) {
        checkBool(!arena.slots[i].allocated, "slot starts free");
        checkBool(!arena.slots[i].retired, "slot starts reusable");
        checkU64(arena.slots[i].generation, 1ull, "slot generation starts at one");
    }

    checkU64(igGtSurfaceHandleEncode(0u, 1ull), 0x0000000000000101ull,
            "slot zero generation one encoding");
    checkU64(igGtSurfaceHandleEncode(15u, 1ull), 0x0000000000000110ull,
            "slot fifteen encoding");
    checkU64(igGtSurfaceHandleEncode(3u, 0x123456789abcull),
            0x00123456789abc04ull,
            "wide generation survives opaque handle encoding");
    checkU64(igGtSurfaceHandleEncode(16u, 1ull), 0ull, "slot overflow refused");
    checkU64(igGtSurfaceHandleEncode(0u, 0ull), 0ull, "zero generation refused");
    checkU64(igGtSurfaceHandleEncode(0u, IG_GTH_HANDLE_GENERATION_MAX_56 + 1ull),
            0ull, "generation overflow refused");

    unsigned slot = 99u;
    IGSurfaceU64 generation = 99ull;
    checkBool(igGtSurfaceHandleDecode(0x00123456789abc04ull, &slot, &generation),
          "wide valid handle decodes");
    checkU32(slot, 3u, "decoded slot");
    checkU64(generation, 0x123456789abcull, "decoded wide generation");
    checkBool(!igGtSurfaceHandleDecode(0ull, &slot, &generation), "zero handle refused");
    checkBool(!igGtSurfaceHandleDecode(0x0000000000000111ull, &slot, &generation),
          "slot field above fixed table refused");
}

static void testFirstFitAndResolve()
{
    std::puts("2. first-fit allocation and internal resolution");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);

    const IGSurfaceU64 a = allocatePages(arena, 100u);
    const IGSurfaceU64 b = allocatePages(arena, 50u);
    checkU64(a, 0x0000000000000101ull, "first handle contains only generation and slot");
    checkU64(b, 0x0000000000000102ull, "second handle contains only generation and slot");

    const IGGtSurfaceHandleView av = resolve(arena, a);
    const IGGtSurfaceHandleView bv = resolve(arena, b);
    checkU32(av.firstArenaPage, 0u, "first allocation begins at arena page zero");
    checkU32(av.pageCount, 100u, "first allocation page count");
    checkU32(bv.firstArenaPage, 100u, "second allocation follows first");
    checkU32(bv.pageCount, 50u, "second allocation page count");
    checkU64(av.usedBytes, 99ull * 4096ull + 4ull, "first allocation used bytes");
    checkU32(av.pitch, 4096u, "resolved pitch");

    for (unsigned i = 0; i < 150u; ++i)
        checkBool(igGtSurfaceHandlePageUsed(&arena, i), "allocated page marked used");
    checkBool(!igGtSurfaceHandlePageUsed(&arena, 150u), "next page remains free");
}

static void testFreeReuseAndStaleHandles()
{
    std::puts("3. generation bump makes old handles stale");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);

    const IGSurfaceU64 a = allocatePages(arena, 100u);
    const IGSurfaceU64 b = allocatePages(arena, 50u);
    checkU32(igGtSurfaceHandleFree(&arena, a), IG_GTH_OK, "first handle frees");

    IGGtSurfaceHandleView poisoned;
    std::memset(&poisoned, 0x5a, sizeof(poisoned));
    const IGGtSurfaceHandleView before = poisoned;
    checkU32(igGtSurfaceHandleResolve(&arena, a, &poisoned), IG_GTH_ERR_STALE_HANDLE,
            "freed handle is stale");
    checkBool(std::memcmp(&poisoned, &before, sizeof(poisoned)) == 0,
          "stale resolve leaves output untouched");
    checkU32(igGtSurfaceHandleFree(&arena, a), IG_GTH_ERR_STALE_HANDLE,
            "double free refused as stale");

    const IGSurfaceU64 c = allocatePages(arena, 40u);
    checkU64(c, 0x0000000000000201ull, "reused slot carries next generation");
    const IGGtSurfaceHandleView cv = resolve(arena, c);
    checkU32(cv.firstArenaPage, 0u, "freed first-fit range reused");
    checkU32(cv.pageCount, 40u, "reused range size");
    checkU32(igGtSurfaceHandleResolve(&arena, a, &poisoned), IG_GTH_ERR_STALE_HANDLE,
            "old generation stays stale after reuse");

    const IGGtSurfaceHandleView bv = resolve(arena, b);
    checkU32(bv.firstArenaPage, 100u, "unrelated live handle unchanged");
}

static void testFragmentationAndCapacity()
{
    std::puts("4. contiguous first-fit fragmentation policy");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);

    const IGSurfaceU64 a = allocatePages(arena, 100u);
    const IGSurfaceU64 b = allocatePages(arena, 100u);
    const IGSurfaceU64 c = allocatePages(arena, 100u);
    const IGSurfaceU64 d = allocatePages(arena, 100u);
    (void)a; (void)c;

    checkU32(igGtSurfaceHandleFree(&arena, b), IG_GTH_OK, "middle block freed");
    IGSurfaceU64 refused = 0xfeedbeefdeadbeefull;
    checkU32(igGtSurfaceHandleAllocate(&arena, 1u, 120u, 4096u, &refused),
            IG_GTH_ERR_NO_PAGES,
            "120 pages refused when only 100-page gap plus 48-page tail exist");
    checkU64(refused, 0xfeedbeefdeadbeefull,
            "fragmentation refusal leaves handle output untouched");

    checkU32(igGtSurfaceHandleFree(&arena, d), IG_GTH_OK, "last block freed");
    const IGSurfaceU64 e = allocatePages(arena, 120u);
    const IGGtSurfaceHandleView ev = resolve(arena, e);
    checkU32(ev.firstArenaPage, 300u, "120 pages use newly contiguous 148-page tail");
}

static void testSlotExhaustion()
{
    std::puts("5. fixed handle table exhaustion is independent of free pages");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);
    IGSurfaceU64 handles[IG_GTH_MAX_SLOTS_16] = {};
    for (unsigned i = 0; i < IG_GTH_MAX_SLOTS_16; ++i)
        handles[i] = allocatePages(arena, 1u);

    IGSurfaceU64 out = 0x12345678abcdef00ull;
    checkU32(igGtSurfaceHandleAllocate(&arena, 1u, 1u, 4096u, &out),
            IG_GTH_ERR_NO_SLOT, "seventeenth live surface refused");
    checkU64(out, 0x12345678abcdef00ull, "slot refusal leaves output untouched");

    checkU32(igGtSurfaceHandleFree(&arena, handles[7]), IG_GTH_OK,
            "one slot released");
    const IGSurfaceU64 reused = allocatePages(arena, 1u);
    unsigned slot = 99u;
    IGSurfaceU64 generation = 0ull;
    checkBool(igGtSurfaceHandleDecode(reused, &slot, &generation), "reused handle decodes");
    checkU32(slot, 7u, "first free slot reused");
    checkU64(generation, 2ull, "reused slot generation advanced");
}

static void testAtomicRefusalsAndCorruption()
{
    std::puts("6. bad geometry, bad IDs and corruption refuse atomically");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);

    IGSurfaceU64 output = 0xabcdef0123456789ull;
    checkU32(igGtSurfaceHandleAllocate(&arena, 0u, 1u, 4u, &output),
            IG_GTH_ERR_PLAN, "zero-width geometry refused");
    checkU64(output, 0xabcdef0123456789ull, "bad geometry leaves output untouched");
    checkU32(arena.pageBitmap[0], 0u, "bad geometry reserves no page");

    IGGtSurfaceHandleView view = {};
    checkU32(igGtSurfaceHandleResolve(&arena, 0ull, &view), IG_GTH_ERR_BAD_HANDLE,
            "zero handle refused");
    checkU32(igGtSurfaceHandleFree(&arena, 0x0000000000000111ull),
            IG_GTH_ERR_BAD_HANDLE, "out-of-table slot handle refused");

    const IGSurfaceU64 live = allocatePages(arena, 3u);
    const IGGtSurfaceHandleSlot beforeSlot = arena.slots[0];
    igGtSurfaceHandleSetPage(&arena, 1u, false); // simulate corrupted ownership bitmap
    checkU32(igGtSurfaceHandleResolve(&arena, live, &view), IG_GTH_ERR_CORRUPT,
            "resolve notices missing owned page");
    checkU32(igGtSurfaceHandleFree(&arena, live), IG_GTH_ERR_CORRUPT,
            "free refuses corrupted ownership image");
    checkBool(arena.slots[0].allocated == beforeSlot.allocated,
          "corrupt free keeps slot allocation state");
    checkU64(arena.slots[0].generation, beforeSlot.generation,
            "corrupt free does not advance generation");
    checkBool(igGtSurfaceHandlePageUsed(&arena, 0u), "corrupt free keeps page zero owned");
    checkBool(!igGtSurfaceHandlePageUsed(&arena, 1u), "corrupt marker remains for diagnosis");
    checkBool(igGtSurfaceHandlePageUsed(&arena, 2u), "corrupt free keeps page two owned");
}

static void testGenerationRetirement()
{
    std::puts("7. 56-bit generation exhaustion permanently retires the slot");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);
    arena.slots[0].generation = IG_GTH_HANDLE_GENERATION_MAX_56;

    const IGSurfaceU64 last = allocatePages(arena, 1u);
    unsigned slot = 0u;
    IGSurfaceU64 generation = 0ull;
    checkBool(igGtSurfaceHandleDecode(last, &slot, &generation), "max-generation handle decodes");
    checkU32(slot, 0u, "max-generation allocation uses first slot");
    checkU64(generation, IG_GTH_HANDLE_GENERATION_MAX_56, "max generation emitted");
    checkU64(last, 0xffffffffffffff01ull, "max generation occupies upper 56 handle bits");
    checkU32(igGtSurfaceHandleFree(&arena, last), IG_GTH_OK, "max generation frees");
    checkBool(!arena.slots[0].allocated, "exhausted slot is no longer allocated");
    checkBool(arena.slots[0].retired, "exhausted slot is permanently retired");
    checkU64(arena.slots[0].generation, IG_GTH_HANDLE_GENERATION_MAX_56,
            "retired slot keeps terminal generation");
    checkBool(!igGtSurfaceHandlePageUsed(&arena, 0u), "retired slot releases its page range");

    const IGSurfaceU64 next = allocatePages(arena, 1u);
    checkU64(next, 0x0000000000000102ull,
            "allocator skips retired slot instead of resurrecting generation one");
    checkBool(igGtSurfaceHandleDecode(next, &slot, &generation), "next handle decodes");
    checkU32(slot, 1u, "next allocation advances to second slot");
    checkU64(generation, 1ull, "next slot starts at generation one");

    IGGtSurfaceHandleView probe = {};
    checkU32(igGtSurfaceHandleResolve(&arena, last, &probe),
            IG_GTH_ERR_STALE_HANDLE, "terminal handle remains stale forever");
    checkU32(igGtSurfaceHandleFree(&arena, last),
            IG_GTH_ERR_STALE_HANDLE, "terminal handle cannot free retired slot twice");
}

int main()
{
    testEncodingAndInit();
    testFirstFitAndResolve();
    testFreeReuseAndStaleHandles();
    testFragmentationAndCapacity();
    testSlotExhaustion();
    testAtomicRefusalsAndCorruption();
    testGenerationRetirement();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
