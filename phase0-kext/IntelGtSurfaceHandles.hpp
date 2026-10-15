// SPDX-License-Identifier: MIT
// Dependency-free allocator for the already-mapped Tiger Lake persistent arena.
//
// Handles are opaque 32-bit generation-tagged slot IDs. They never contain a
// GPU virtual address, DMA address, CPU pointer, PTE index, or byte offset.
// The kernel may resolve a live handle to an arena page offset internally.
#ifndef IntelGtSurfaceHandles_hpp
#define IntelGtSurfaceHandles_hpp

#include "IntelGtSurface.hpp"

#define IG_GTH_MAX_SLOTS_16                 16u
#define IG_GTH_BITMAP_WORDS_14              14u
#define IG_GTH_HANDLE_SLOT_MASK_0xFF         0xffu
#define IG_GTH_HANDLE_GENERATION_MAX_24     0x00ffffffu

#define IG_GTH_OK                            0u
#define IG_GTH_ERR_NULL                      1u
#define IG_GTH_ERR_PLAN                      2u
#define IG_GTH_ERR_NO_SLOT                   3u
#define IG_GTH_ERR_NO_PAGES                  4u
#define IG_GTH_ERR_BAD_HANDLE                5u
#define IG_GTH_ERR_STALE_HANDLE              6u
#define IG_GTH_ERR_CORRUPT                   7u

struct IGGtSurfaceHandleSlot {
    IGSurfaceU32 generation;
    IGSurfaceU32 firstArenaPage;
    IGSurfaceU32 pageCount;
    IGSurfaceU32 width;
    IGSurfaceU32 height;
    IGSurfaceU32 pitch;
    IGSurfaceU64 usedBytes;
    bool allocated;
};

struct IGGtSurfaceHandleArena {
    IGGtSurfaceHandleSlot slots[IG_GTH_MAX_SLOTS_16];
    IGSurfaceU32 pageBitmap[IG_GTH_BITMAP_WORDS_14];
};

struct IGGtSurfaceHandleView {
    IGSurfaceU32 handle;
    IGSurfaceU32 slotIndex;
    IGSurfaceU32 generation;
    IGSurfaceU32 firstArenaPage;
    IGSurfaceU32 pageCount;
    IGSurfaceU32 width;
    IGSurfaceU32 height;
    IGSurfaceU32 pitch;
    IGSurfaceU64 usedBytes;
};

static inline bool igGtSurfaceHandleSucceeded(IGSurfaceU32 verdict)
{
    return verdict == IG_GTH_OK;
}

static inline IGSurfaceU32 igGtSurfaceHandleNextGeneration(IGSurfaceU32 generation)
{
    generation &= IG_GTH_HANDLE_GENERATION_MAX_24;
    if (generation == 0u || generation == IG_GTH_HANDLE_GENERATION_MAX_24)
        return 1u;
    return generation + 1u;
}

static inline IGSurfaceU32 igGtSurfaceHandleEncode(IGSurfaceU32 slotIndex,
                                                    IGSurfaceU32 generation)
{
    if (slotIndex >= IG_GTH_MAX_SLOTS_16 || generation == 0u ||
        generation > IG_GTH_HANDLE_GENERATION_MAX_24)
        return 0u;
    return (generation << 8) | (slotIndex + 1u);
}

static inline bool igGtSurfaceHandleDecode(IGSurfaceU32 handle,
                                           IGSurfaceU32 *slotIndex,
                                           IGSurfaceU32 *generation)
{
    const IGSurfaceU32 slotField = handle & IG_GTH_HANDLE_SLOT_MASK_0xFF;
    const IGSurfaceU32 gen = handle >> 8;
    if (slotField == 0u || slotField > IG_GTH_MAX_SLOTS_16 || gen == 0u ||
        gen > IG_GTH_HANDLE_GENERATION_MAX_24)
        return false;
    if (slotIndex) *slotIndex = slotField - 1u;
    if (generation) *generation = gen;
    return true;
}

static inline bool igGtSurfaceHandlePageUsed(const IGGtSurfaceHandleArena *arena,
                                              IGSurfaceU32 page)
{
    if (!arena || page >= IG_GTS_ARENA_LEAVES_448)
        return false;
    const IGSurfaceU32 word = page >> 5;
    const IGSurfaceU32 bit = page & 31u;
    return (arena->pageBitmap[word] & (1u << bit)) != 0u;
}

static inline void igGtSurfaceHandleSetPage(IGGtSurfaceHandleArena *arena,
                                             IGSurfaceU32 page,
                                             bool used)
{
    const IGSurfaceU32 word = page >> 5;
    const IGSurfaceU32 bit = page & 31u;
    const IGSurfaceU32 mask = 1u << bit;
    if (used) arena->pageBitmap[word] |= mask;
    else arena->pageBitmap[word] &= ~mask;
}

static inline void igGtSurfaceHandleArenaInit(IGGtSurfaceHandleArena *arena)
{
    if (!arena) return;
    for (IGSurfaceU32 i = 0u; i < IG_GTH_BITMAP_WORDS_14; ++i)
        arena->pageBitmap[i] = 0u;
    for (IGSurfaceU32 i = 0u; i < IG_GTH_MAX_SLOTS_16; ++i) {
        IGGtSurfaceHandleSlot clean = {};
        clean.generation = 1u;
        arena->slots[i] = clean;
    }
}

static inline bool igGtSurfaceHandleRangeState(const IGGtSurfaceHandleArena *arena,
                                                IGSurfaceU32 firstPage,
                                                IGSurfaceU32 pageCount,
                                                bool wantedUsed)
{
    if (!arena || pageCount == 0u || firstPage >= IG_GTS_ARENA_LEAVES_448 ||
        (IGSurfaceU64)firstPage + pageCount > IG_GTS_ARENA_LEAVES_448)
        return false;
    for (IGSurfaceU32 i = 0u; i < pageCount; ++i) {
        if (igGtSurfaceHandlePageUsed(arena, firstPage + i) != wantedUsed)
            return false;
    }
    return true;
}

static inline bool igGtSurfaceHandleFindFirstFit(const IGGtSurfaceHandleArena *arena,
                                                  IGSurfaceU32 pageCount,
                                                  IGSurfaceU32 *firstPage)
{
    if (!arena || !firstPage || pageCount == 0u ||
        pageCount > IG_GTS_ARENA_LEAVES_448)
        return false;

    IGSurfaceU32 run = 0u;
    for (IGSurfaceU32 page = 0u; page < IG_GTS_ARENA_LEAVES_448; ++page) {
        if (!igGtSurfaceHandlePageUsed(arena, page)) {
            ++run;
            if (run == pageCount) {
                *firstPage = page + 1u - pageCount;
                return true;
            }
        } else {
            run = 0u;
        }
    }
    return false;
}

static inline IGSurfaceU32 igGtSurfaceHandleResolve(
    const IGGtSurfaceHandleArena *arena,
    IGSurfaceU32 handle,
    IGGtSurfaceHandleView *out)
{
    if (!arena || !out)
        return IG_GTH_ERR_NULL;

    IGSurfaceU32 slotIndex = 0u, generation = 0u;
    if (!igGtSurfaceHandleDecode(handle, &slotIndex, &generation))
        return IG_GTH_ERR_BAD_HANDLE;

    const IGGtSurfaceHandleSlot &slot = arena->slots[slotIndex];
    if (!slot.allocated || slot.generation != generation)
        return IG_GTH_ERR_STALE_HANDLE;
    if (slot.pageCount == 0u ||
        (IGSurfaceU64)slot.firstArenaPage + slot.pageCount > IG_GTS_ARENA_LEAVES_448 ||
        !igGtSurfaceHandleRangeState(arena, slot.firstArenaPage, slot.pageCount, true))
        return IG_GTH_ERR_CORRUPT;

    IGGtSurfaceHandleView staged = {};
    staged.handle = handle;
    staged.slotIndex = slotIndex;
    staged.generation = generation;
    staged.firstArenaPage = slot.firstArenaPage;
    staged.pageCount = slot.pageCount;
    staged.width = slot.width;
    staged.height = slot.height;
    staged.pitch = slot.pitch;
    staged.usedBytes = slot.usedBytes;
    *out = staged;
    return IG_GTH_OK;
}

// First-fit contiguous allocation inside the 448-page arena. The geometry is
// validated by the same planner used by the private-PPGTT runtime. No arena or
// output mutation occurs until a free slot and a complete free page run have
// both been found.
static inline IGSurfaceU32 igGtSurfaceHandleAllocate(
    IGGtSurfaceHandleArena *arena,
    IGSurfaceU32 width,
    IGSurfaceU32 height,
    IGSurfaceU32 pitch,
    IGSurfaceU32 *outHandle)
{
    if (!arena || !outHandle)
        return IG_GTH_ERR_NULL;

    IGGtSurfacePlan basePlan = {};
    if (!igGtSurfacePlanSucceeded(
            igGtSurfacePlan32(width, height, pitch, 0u, &basePlan)))
        return IG_GTH_ERR_PLAN;

    IGSurfaceU32 slotIndex = IG_GTH_MAX_SLOTS_16;
    for (IGSurfaceU32 i = 0u; i < IG_GTH_MAX_SLOTS_16; ++i) {
        if (!arena->slots[i].allocated) {
            slotIndex = i;
            break;
        }
    }
    if (slotIndex == IG_GTH_MAX_SLOTS_16)
        return IG_GTH_ERR_NO_SLOT;

    IGSurfaceU32 firstPage = 0u;
    if (!igGtSurfaceHandleFindFirstFit(arena, basePlan.pageCount, &firstPage))
        return IG_GTH_ERR_NO_PAGES;
    if (!igGtSurfaceHandleRangeState(arena, firstPage, basePlan.pageCount, false))
        return IG_GTH_ERR_CORRUPT;

    IGGtSurfaceHandleSlot &slot = arena->slots[slotIndex];
    IGSurfaceU32 generation = slot.generation & IG_GTH_HANDLE_GENERATION_MAX_24;
    if (generation == 0u) generation = 1u;
    const IGSurfaceU32 handle = igGtSurfaceHandleEncode(slotIndex, generation);
    if (handle == 0u)
        return IG_GTH_ERR_CORRUPT;

    for (IGSurfaceU32 i = 0u; i < basePlan.pageCount; ++i)
        igGtSurfaceHandleSetPage(arena, firstPage + i, true);

    slot.generation = generation;
    slot.firstArenaPage = firstPage;
    slot.pageCount = basePlan.pageCount;
    slot.width = width;
    slot.height = height;
    slot.pitch = pitch;
    slot.usedBytes = basePlan.usedBytes;
    slot.allocated = true;
    *outHandle = handle;
    return IG_GTH_OK;
}

// Free validates the complete slot + bitmap image before touching state. The
// generation advances immediately, so every old handle becomes stale before
// the slot can be reused. Page contents are intentionally not cleared here;
// the runtime owner must zero the backing before exposing a reused allocation.
static inline IGSurfaceU32 igGtSurfaceHandleFree(IGGtSurfaceHandleArena *arena,
                                                  IGSurfaceU32 handle)
{
    if (!arena)
        return IG_GTH_ERR_NULL;

    IGSurfaceU32 slotIndex = 0u, generation = 0u;
    if (!igGtSurfaceHandleDecode(handle, &slotIndex, &generation))
        return IG_GTH_ERR_BAD_HANDLE;

    IGGtSurfaceHandleSlot &slot = arena->slots[slotIndex];
    if (!slot.allocated || slot.generation != generation)
        return IG_GTH_ERR_STALE_HANDLE;
    if (slot.pageCount == 0u ||
        (IGSurfaceU64)slot.firstArenaPage + slot.pageCount > IG_GTS_ARENA_LEAVES_448 ||
        !igGtSurfaceHandleRangeState(arena, slot.firstArenaPage, slot.pageCount, true))
        return IG_GTH_ERR_CORRUPT;

    const IGSurfaceU32 firstPage = slot.firstArenaPage;
    const IGSurfaceU32 pageCount = slot.pageCount;
    const IGSurfaceU32 nextGeneration = igGtSurfaceHandleNextGeneration(slot.generation);

    for (IGSurfaceU32 i = 0u; i < pageCount; ++i)
        igGtSurfaceHandleSetPage(arena, firstPage + i, false);

    IGGtSurfaceHandleSlot clean = {};
    clean.generation = nextGeneration;
    slot = clean;
    return IG_GTH_OK;
}

#endif // IntelGtSurfaceHandles_hpp
