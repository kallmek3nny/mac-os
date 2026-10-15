// SPDX-License-Identifier: MIT
// Host-only tests for the opaque persistent-surface handle allocator.
#include <cstdio>
#include <cstring>
#include "IntelGtSurfaceHandles.hpp"

static int failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static void check32(unsigned int got, unsigned int want, const char *what)
{
    if (got != want) {
        std::printf("FAIL: %s got=%08x want=%08x\n", what, got, want);
        ++failures;
    }
}

static void check64(unsigned long long got, unsigned long long want, const char *what)
{
    if (got != want) {
        std::printf("FAIL: %s got=%016llx want=%016llx\n", what, got, want);
        ++failures;
    }
}

static unsigned allocatePages(IGGtSurfaceHandleArena &arena, unsigned pages)
{
    unsigned handle = 0u;
    const unsigned verdict = igGtSurfaceHandleAllocate(
        &arena, 1u, pages, 4096u, &handle);
    check32(verdict, IG_GTH_OK, "page-count allocation succeeds");
    return handle;
}

static IGGtSurfaceHandleView resolve(IGGtSurfaceHandleArena &arena, unsigned handle)
{
    IGGtSurfaceHandleView view = {};
    check32(igGtSurfaceHandleResolve(&arena, handle, &view), IG_GTH_OK,
            "resolve succeeds");
    return view;
}

static void testEncodingAndInit()
{
    std::puts("1. opaque handle encoding and clean arena");
    IGGtSurfaceHandleArena arena;
    std::memset(&arena, 0xa5, sizeof(arena));
    igGtSurfaceHandleArenaInit(&arena);

    for (unsigned i = 0; i < IG_GTH_BITMAP_WORDS_14; ++i)
        check32(arena.pageBitmap[i], 0u, "bitmap word starts clear");
    for (unsigned i = 0; i < IG_GTH_MAX_SLOTS_16; ++i) {
        check(!arena.slots[i].allocated, "slot starts free");
        check32(arena.slots[i].generation, 1u, "slot generation starts at one");
    }

    check32(igGtSurfaceHandleEncode(0u, 1u), 0x00000101u,
            "slot zero generation one encoding");
    check32(igGtSurfaceHandleEncode(15u, 1u), 0x00000110u,
            "slot fifteen encoding");
    check32(igGtSurfaceHandleEncode(16u, 1u), 0u, "slot overflow refused");
    check32(igGtSurfaceHandleEncode(0u, 0u), 0u, "zero generation refused");

    unsigned slot = 99u, generation = 99u;
    check(igGtSurfaceHandleDecode(0x00012304u, &slot, &generation),
          "valid handle decodes");
    check32(slot, 3u, "decoded slot");
    check32(generation, 0x123u, "decoded generation");
    check(!igGtSurfaceHandleDecode(0u, &slot, &generation), "zero handle refused");
    check(!igGtSurfaceHandleDecode(0x00000111u, &slot, &generation),
          "slot field above fixed table refused");
}

static void testFirstFitAndResolve()
{
    std::puts("2. first-fit allocation and internal resolution");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);

    const unsigned a = allocatePages(arena, 100u);
    const unsigned b = allocatePages(arena, 50u);
    check32(a, 0x00000101u, "first handle contains only generation and slot");
    check32(b, 0x00000102u, "second handle contains only generation and slot");

    const auto av = resolve(arena, a);
    const auto bv = resolve(arena, b);
    check32(av.firstArenaPage, 0u, "first allocation begins at arena page zero");
    check32(av.pageCount, 100u, "first allocation page count");
    check32(bv.firstArenaPage, 100u, "second allocation follows first");
    check32(bv.pageCount, 50u, "second allocation page count");
    check64(av.usedBytes, 99ull * 4096ull + 4ull, "first allocation used bytes");
    check32(av.pitch, 4096u, "resolved pitch");

    for (unsigned i = 0; i < 150u; ++i)
        check(igGtSurfaceHandlePageUsed(&arena, i), "allocated page marked used");
    check(!igGtSurfaceHandlePageUsed(&arena, 150u), "next page remains free");
}

static void testFreeReuseAndStaleHandles()
{
    std::puts("3. generation bump makes old handles stale");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);

    const unsigned a = allocatePages(arena, 100u);
    const unsigned b = allocatePages(arena, 50u);
    check32(igGtSurfaceHandleFree(&arena, a), IG_GTH_OK, "first handle frees");

    IGGtSurfaceHandleView poisoned;
    std::memset(&poisoned, 0x5a, sizeof(poisoned));
    const IGGtSurfaceHandleView before = poisoned;
    check32(igGtSurfaceHandleResolve(&arena, a, &poisoned), IG_GTH_ERR_STALE_HANDLE,
            "freed handle is stale");
    check(std::memcmp(&poisoned, &before, sizeof(poisoned)) == 0,
          "stale resolve leaves output untouched");
    check32(igGtSurfaceHandleFree(&arena, a), IG_GTH_ERR_STALE_HANDLE,
            "double free refused as stale");

    const unsigned c = allocatePages(arena, 40u);
    check32(c, 0x00000201u, "reused slot carries next generation");
    const auto cv = resolve(arena, c);
    check32(cv.firstArenaPage, 0u, "freed first-fit range reused");
    check32(cv.pageCount, 40u, "reused range size");
    check32(igGtSurfaceHandleResolve(&arena, a, &poisoned), IG_GTH_ERR_STALE_HANDLE,
            "old generation stays stale after reuse");

    const auto bv = resolve(arena, b);
    check32(bv.firstArenaPage, 100u, "unrelated live handle unchanged");
}

static void testFragmentationAndCapacity()
{
    std::puts("4. contiguous first-fit fragmentation policy");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);

    const unsigned a = allocatePages(arena, 100u);
    const unsigned b = allocatePages(arena, 100u);
    const unsigned c = allocatePages(arena, 100u);
    const unsigned d = allocatePages(arena, 100u);
    (void)a; (void)c;

    check32(igGtSurfaceHandleFree(&arena, b), IG_GTH_OK, "middle block freed");
    unsigned refused = 0xfeedbeefu;
    check32(igGtSurfaceHandleAllocate(&arena, 1u, 120u, 4096u, &refused),
            IG_GTH_ERR_NO_PAGES,
            "120 pages refused when only 100-page gap plus 48-page tail exist");
    check32(refused, 0xfeedbeefu, "fragmentation refusal leaves handle output untouched");

    check32(igGtSurfaceHandleFree(&arena, d), IG_GTH_OK, "last block freed");
    const unsigned e = allocatePages(arena, 120u);
    const auto ev = resolve(arena, e);
    check32(ev.firstArenaPage, 300u, "120 pages use newly contiguous 148-page tail");
}

static void testSlotExhaustion()
{
    std::puts("5. fixed handle table exhaustion is independent of free pages");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);
    unsigned handles[IG_GTH_MAX_SLOTS_16] = {};
    for (unsigned i = 0; i < IG_GTH_MAX_SLOTS_16; ++i)
        handles[i] = allocatePages(arena, 1u);

    unsigned out = 0x12345678u;
    check32(igGtSurfaceHandleAllocate(&arena, 1u, 1u, 4096u, &out),
            IG_GTH_ERR_NO_SLOT, "seventeenth live surface refused");
    check32(out, 0x12345678u, "slot refusal leaves output untouched");

    check32(igGtSurfaceHandleFree(&arena, handles[7]), IG_GTH_OK,
            "one slot released");
    const unsigned reused = allocatePages(arena, 1u);
    unsigned slot = 99u, generation = 0u;
    check(igGtSurfaceHandleDecode(reused, &slot, &generation), "reused handle decodes");
    check32(slot, 7u, "first free slot reused");
    check32(generation, 2u, "reused slot generation advanced");
}

static void testAtomicRefusalsAndCorruption()
{
    std::puts("6. bad geometry, bad IDs and corruption refuse atomically");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);

    unsigned output = 0xabcdef01u;
    check32(igGtSurfaceHandleAllocate(&arena, 0u, 1u, 4u, &output),
            IG_GTH_ERR_PLAN, "zero-width geometry refused");
    check32(output, 0xabcdef01u, "bad geometry leaves output untouched");
    check32(arena.pageBitmap[0], 0u, "bad geometry reserves no page");

    IGGtSurfaceHandleView view = {};
    check32(igGtSurfaceHandleResolve(&arena, 0u, &view), IG_GTH_ERR_BAD_HANDLE,
            "zero handle refused");
    check32(igGtSurfaceHandleFree(&arena, 0x00000111u), IG_GTH_ERR_BAD_HANDLE,
            "out-of-table slot handle refused");

    const unsigned live = allocatePages(arena, 3u);
    const auto beforeSlot = arena.slots[0];
    igGtSurfaceHandleSetPage(&arena, 1u, false); // simulate corrupted ownership bitmap
    check32(igGtSurfaceHandleResolve(&arena, live, &view), IG_GTH_ERR_CORRUPT,
            "resolve notices missing owned page");
    check32(igGtSurfaceHandleFree(&arena, live), IG_GTH_ERR_CORRUPT,
            "free refuses corrupted ownership image");
    check(arena.slots[0].allocated == beforeSlot.allocated,
          "corrupt free keeps slot allocation state");
    check32(arena.slots[0].generation, beforeSlot.generation,
            "corrupt free does not advance generation");
    check(igGtSurfaceHandlePageUsed(&arena, 0u), "corrupt free keeps page zero owned");
    check(!igGtSurfaceHandlePageUsed(&arena, 1u), "corrupt marker remains for diagnosis");
    check(igGtSurfaceHandlePageUsed(&arena, 2u), "corrupt free keeps page two owned");
}

static void testGenerationWrap()
{
    std::puts("7. 24-bit generation wrap skips zero");
    IGGtSurfaceHandleArena arena = {};
    igGtSurfaceHandleArenaInit(&arena);
    arena.slots[0].generation = IG_GTH_HANDLE_GENERATION_MAX_24;

    const unsigned last = allocatePages(arena, 1u);
    unsigned slot = 0u, generation = 0u;
    check(igGtSurfaceHandleDecode(last, &slot, &generation), "max-generation handle decodes");
    check32(generation, IG_GTH_HANDLE_GENERATION_MAX_24, "max generation emitted");
    check32(igGtSurfaceHandleFree(&arena, last), IG_GTH_OK, "max generation frees");
    check32(arena.slots[0].generation, 1u, "generation wraps to one, never zero");

    const unsigned wrapped = allocatePages(arena, 1u);
    check32(wrapped, 0x00000101u, "wrapped handle uses generation one");
    check32(igGtSurfaceHandleResolve(&arena, last, &resolve(arena, wrapped)),
            IG_GTH_ERR_STALE_HANDLE, "pre-wrap max-generation handle is stale");
}

int main()
{
    testEncodingAndInit();
    testFirstFitAndResolve();
    testFreeReuseAndStaleHandles();
    testFragmentationAndCapacity();
    testSlotExhaustion();
    testAtomicRefusalsAndCorruption();
    testGenerationWrap();

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::puts("Tiger Lake persistent surface handle allocator tests passed");
    return 0;
}
