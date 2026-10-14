// SPDX-License-Identifier: MIT
// Pure LibOS memory-region-list + TU102/GA107 boot-args mailbox contract.
#ifndef NV_GSP_LIBOS_INIT_HPP
#define NV_GSP_LIBOS_INIT_HPP

#include "NVGspCachedArgs.hpp"

using NVGspLibosU8  = unsigned char;
using NVGspLibosU32 = unsigned int;
using NVGspLibosU64 = unsigned long long;

// NVIDIA open RM, libos_init_args.h and kernel_gsp_tu102.c.
#define NV_GSP_LIBOS_INIT_LIST_BYTES              4096u
#define NV_GSP_LIBOS_INIT_REGION_BYTES            32u
#define NV_GSP_LIBOS_INIT_MAX_REGIONS             128u
#define NV_GSP_LIBOS_MAILBOX0_REG_TU102           0x00110040u
#define NV_GSP_LIBOS_MAILBOX1_REG_TU102           0x00110044u

#define NV_GSP_LIBOS_INIT_NOT_ATTEMPTED           0u
#define NV_GSP_LIBOS_INIT_OK                      1u
#define NV_GSP_LIBOS_INIT_NULL_LIST               2u
#define NV_GSP_LIBOS_INIT_NULL_OUTPUT             3u
#define NV_GSP_LIBOS_INIT_LIST_TOO_SHORT          4u
#define NV_GSP_LIBOS_INIT_BAD_RMARGS_ADDRESS      5u
#define NV_GSP_LIBOS_INIT_BAD_RMARGS_SIZE         6u
#define NV_GSP_LIBOS_INIT_DUPLICATE_RMARGS        7u
#define NV_GSP_LIBOS_INIT_SPARSE_LIST             8u
#define NV_GSP_LIBOS_INIT_LIST_FULL               9u
#define NV_GSP_LIBOS_INIT_LIMIT                   10u

#define NV_GSP_LIBOS_MAILBOX_NOT_ATTEMPTED        0u
#define NV_GSP_LIBOS_MAILBOX_OK                   1u
#define NV_GSP_LIBOS_MAILBOX_NULL_OUTPUT          2u
#define NV_GSP_LIBOS_MAILBOX_BAD_ADDRESS          3u
#define NV_GSP_LIBOS_MAILBOX_LIMIT                4u

struct NVGspLibosInitPlan {
    NVGspLibosU32 rmArgsIndex;
    NVGspLibosU32 terminatorIndex;
    NVGspLibosU32 regionsBeforeRmArgs;
    NVGspLibosU32 usedBytesAfterAppend;
    NVGspLibosMemoryRegion rmArgsRegion;
};

struct NVGspLibosMailboxWrite {
    NVGspLibosU32 reg;
    NVGspLibosU32 value;
};

struct NVGspLibosMailboxPlan {
    NVGspLibosU64 listGpuAddress;
    NVGspLibosMailboxWrite writes[2];
};

static inline bool nvGspLibosInitSucceeded(NVGspLibosU32 verdict)
{
    return verdict == NV_GSP_LIBOS_INIT_OK;
}

static inline const char *nvGspLibosInitVerdictName(NVGspLibosU32 verdict)
{
    switch (verdict) {
        case NV_GSP_LIBOS_INIT_NOT_ATTEMPTED:      return "not-attempted";
        case NV_GSP_LIBOS_INIT_OK:                 return "ok";
        case NV_GSP_LIBOS_INIT_NULL_LIST:          return "null-list";
        case NV_GSP_LIBOS_INIT_NULL_OUTPUT:        return "null-output";
        case NV_GSP_LIBOS_INIT_LIST_TOO_SHORT:     return "list-too-short";
        case NV_GSP_LIBOS_INIT_BAD_RMARGS_ADDRESS: return "bad-rmargs-address";
        case NV_GSP_LIBOS_INIT_BAD_RMARGS_SIZE:    return "bad-rmargs-size";
        case NV_GSP_LIBOS_INIT_DUPLICATE_RMARGS:   return "duplicate-rmargs";
        case NV_GSP_LIBOS_INIT_SPARSE_LIST:        return "sparse-list";
        case NV_GSP_LIBOS_INIT_LIST_FULL:          return "list-full";
        default:                                    return "unknown";
    }
}

static inline bool nvGspLibosMailboxSucceeded(NVGspLibosU32 verdict)
{
    return verdict == NV_GSP_LIBOS_MAILBOX_OK;
}

static inline const char *nvGspLibosMailboxVerdictName(NVGspLibosU32 verdict)
{
    switch (verdict) {
        case NV_GSP_LIBOS_MAILBOX_NOT_ATTEMPTED: return "not-attempted";
        case NV_GSP_LIBOS_MAILBOX_OK:            return "ok";
        case NV_GSP_LIBOS_MAILBOX_NULL_OUTPUT:   return "null-output";
        case NV_GSP_LIBOS_MAILBOX_BAD_ADDRESS:   return "bad-address";
        default:                                  return "unknown";
    }
}

static inline bool nvGspLibosRegionEmpty(const NVGspLibosMemoryRegion &r)
{
    if (r.id8 || r.pa || r.size || r.kind || r.loc) return false;
    for (NVGspLibosU32 i = 0u; i < sizeof(r.padding); ++i)
        if (r.padding[i] != 0u) return false;
    return true;
}

// NVIDIA zeroes the entire 4 KiB LibOS init page and then fills entries in
// increasing index order. Preserve that dense-list contract: once the first
// zero entry is observed, every later entry must also be zero. Keep one zero
// entry after RMARGS as an explicit conservative terminator rather than filling
// all 128 slots.
static inline NVGspLibosU32 nvGspPlanAppendRmArgsRegion(
    const NVGspLibosMemoryRegion *list,
    NVGspLibosU32 listBytes,
    NVGspLibosU64 rmArgsGpuAddress,
    NVGspLibosU64 rmArgsBytes,
    NVGspLibosInitPlan *out)
{
    if (!list) return NV_GSP_LIBOS_INIT_NULL_LIST;
    if (!out) return NV_GSP_LIBOS_INIT_NULL_OUTPUT;
    if (listBytes < NV_GSP_LIBOS_INIT_LIST_BYTES)
        return NV_GSP_LIBOS_INIT_LIST_TOO_SHORT;
    if (!rmArgsGpuAddress ||
        (rmArgsGpuAddress & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0ull)
        return NV_GSP_LIBOS_INIT_BAD_RMARGS_ADDRESS;
    if (rmArgsBytes != NV_GSP_CACHED_ARGS_PAGE_BYTES)
        return NV_GSP_LIBOS_INIT_BAD_RMARGS_SIZE;

    NVGspLibosU32 firstEmpty = NV_GSP_LIBOS_INIT_MAX_REGIONS;
    bool sawEmpty = false;
    for (NVGspLibosU32 i = 0u; i < NV_GSP_LIBOS_INIT_MAX_REGIONS; ++i) {
        const bool empty = nvGspLibosRegionEmpty(list[i]);
        if (list[i].id8 == NV_GSP_LIBOS_RMARGS_ID && !empty)
            return NV_GSP_LIBOS_INIT_DUPLICATE_RMARGS;
        if (empty) {
            if (!sawEmpty) firstEmpty = i;
            sawEmpty = true;
        }
        else if (sawEmpty) {
            return NV_GSP_LIBOS_INIT_SPARSE_LIST;
        }
    }

    // Reserve one zero slot following RMARGS. Open RM starts from a zeroed page;
    // retaining that sentinel means future runtime code never needs to infer an
    // implicit count from a fully populated 4 KiB array.
    if (firstEmpty >= NV_GSP_LIBOS_INIT_MAX_REGIONS - 1u)
        return NV_GSP_LIBOS_INIT_LIST_FULL;

    NVGspLibosInitPlan staged = {};
    staged.rmArgsIndex = firstEmpty;
    staged.terminatorIndex = firstEmpty + 1u;
    staged.regionsBeforeRmArgs = firstEmpty;
    staged.usedBytesAfterAppend = (firstEmpty + 1u) * NV_GSP_LIBOS_INIT_REGION_BYTES;
    staged.rmArgsRegion.id8 = NV_GSP_LIBOS_RMARGS_ID;
    staged.rmArgsRegion.pa = rmArgsGpuAddress;
    staged.rmArgsRegion.size = rmArgsBytes;
    staged.rmArgsRegion.kind = NV_GSP_LIBOS_REGION_CONTIGUOUS;
    staged.rmArgsRegion.loc = NV_GSP_LIBOS_REGION_LOC_SYSMEM;
    for (NVGspLibosU32 i = 0u; i < sizeof(staged.rmArgsRegion.padding); ++i)
        staged.rmArgsRegion.padding[i] = 0u;
    *out = staged;
    return NV_GSP_LIBOS_INIT_OK;
}

static inline NVGspLibosU32 nvGspApplyRmArgsRegion(
    NVGspLibosMemoryRegion *list,
    NVGspLibosU32 listBytes,
    NVGspLibosU64 rmArgsGpuAddress,
    NVGspLibosU64 rmArgsBytes,
    NVGspLibosInitPlan *outPlan)
{
    if (!list) return NV_GSP_LIBOS_INIT_NULL_LIST;
    if (!outPlan) return NV_GSP_LIBOS_INIT_NULL_OUTPUT;

    NVGspLibosInitPlan staged = {};
    const NVGspLibosU32 verdict = nvGspPlanAppendRmArgsRegion(
        list, listBytes, rmArgsGpuAddress, rmArgsBytes, &staged);
    if (verdict != NV_GSP_LIBOS_INIT_OK)
        return verdict;

    list[staged.rmArgsIndex] = staged.rmArgsRegion;
    *outPlan = staged;
    return NV_GSP_LIBOS_INIT_OK;
}

// TU102's GSP bootstrap HAL is also used for GA10x. Open RM writes the GPU
// address of pLibosInitArgumentsDescriptor to MAILBOX0 (low 32) then MAILBOX1
// (high 32). This helper only plans those writes; it never performs MMIO.
static inline NVGspLibosU32 nvGspPlanLibosMailboxWrites(
    NVGspLibosU64 listGpuAddress,
    NVGspLibosMailboxPlan *out)
{
    if (!out) return NV_GSP_LIBOS_MAILBOX_NULL_OUTPUT;
    if (!listGpuAddress ||
        (listGpuAddress & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0ull)
        return NV_GSP_LIBOS_MAILBOX_BAD_ADDRESS;

    NVGspLibosMailboxPlan staged = {};
    staged.listGpuAddress = listGpuAddress;
    staged.writes[0].reg = NV_GSP_LIBOS_MAILBOX0_REG_TU102;
    staged.writes[0].value = (NVGspLibosU32)(listGpuAddress & 0xffffffffull);
    staged.writes[1].reg = NV_GSP_LIBOS_MAILBOX1_REG_TU102;
    staged.writes[1].value = (NVGspLibosU32)(listGpuAddress >> 32);
    *out = staged;
    return NV_GSP_LIBOS_MAILBOX_OK;
}

#endif // NV_GSP_LIBOS_INIT_HPP
