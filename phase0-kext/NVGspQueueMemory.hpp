// SPDX-License-Identifier: MIT
// Pure GSP-RM shared queue backing + cached init-argument composition.
#ifndef NV_GSP_QUEUE_MEMORY_HPP
#define NV_GSP_QUEUE_MEMORY_HPP

#include "NVGspQueuePlan.hpp"

using NVGspMemU32 = unsigned int;
using NVGspMemU64 = unsigned long long;

#define NV_GSP_QUEUE_INIT_FLAG_ENCRYPTION          0x01ull
#define NV_GSP_QUEUE_INIT_ARGUMENT_BYTES           64u
#define NV_GSP_QUEUE_NORMAL_MAX_PAGES              129u

#define NV_GSP_QUEUE_MEMORY_NOT_ATTEMPTED          0u
#define NV_GSP_QUEUE_MEMORY_OK                     1u
#define NV_GSP_QUEUE_MEMORY_NULL_PLAN              2u
#define NV_GSP_QUEUE_MEMORY_NULL_PAGES             3u
#define NV_GSP_QUEUE_MEMORY_NULL_OUTPUT            4u
#define NV_GSP_QUEUE_MEMORY_BAD_PAGE_COUNT         5u
#define NV_GSP_QUEUE_MEMORY_BAD_PAGE_ADDRESS       6u
#define NV_GSP_QUEUE_MEMORY_BAD_LAYOUT             7u
#define NV_GSP_QUEUE_MEMORY_ALIASED_PAGE           8u
#define NV_GSP_QUEUE_MEMORY_LIMIT                  9u

// Mirrors NVIDIA's ABI-stable MESSAGE_QUEUE_INIT_ARGUMENTS field-for-field.
// Natural C++ layout is 64 bytes: four 64-bit fields, six 32-bit fields, 8
// reserved bytes. A host test pins sizeof and the serialized dword image.
struct NVGspQueueInitArguments {
    NVGspMemU64 flags;
    NVGspMemU64 sharedMemPhysAddr;
    NVGspMemU64 cmdQueueOffset;
    NVGspMemU64 statQueueOffset;
    NVGspMemU32 queueElementHdrSize;
    NVGspMemU32 queueElementSizeMin;
    NVGspMemU32 queueElementSizeMax;
    NVGspMemU32 queueHeaderAlign;
    NVGspMemU32 queueElementAlign;
    NVGspMemU32 pageTableEntryCount;
    unsigned char reserved[8];
};

struct NVGspQueueMemoryPlan {
    NVGspMemU32 sharedPageCount;
    NVGspMemU32 pageTableEntryCount;
    NVGspMemU32 pageTableBytes;
    NVGspMemU32 commandFirstPage;
    NVGspMemU32 commandPageCount;
    NVGspMemU32 statusFirstPage;
    NVGspMemU32 statusPageCount;
    NVGspMemU64 firstPageAddress;
    NVGspQueueInitArguments initArgs;
};

static inline bool nvGspQueueMemorySucceeded(NVGspMemU32 verdict)
{
    return verdict == NV_GSP_QUEUE_MEMORY_OK;
}

static inline const char *nvGspQueueMemoryVerdictName(NVGspMemU32 verdict)
{
    switch (verdict) {
        case NV_GSP_QUEUE_MEMORY_NOT_ATTEMPTED:    return "not-attempted";
        case NV_GSP_QUEUE_MEMORY_OK:               return "ok";
        case NV_GSP_QUEUE_MEMORY_NULL_PLAN:        return "null-plan";
        case NV_GSP_QUEUE_MEMORY_NULL_PAGES:       return "null-pages";
        case NV_GSP_QUEUE_MEMORY_NULL_OUTPUT:      return "null-output";
        case NV_GSP_QUEUE_MEMORY_BAD_PAGE_COUNT:   return "bad-page-count";
        case NV_GSP_QUEUE_MEMORY_BAD_PAGE_ADDRESS: return "bad-page-address";
        case NV_GSP_QUEUE_MEMORY_BAD_LAYOUT:       return "bad-layout";
        case NV_GSP_QUEUE_MEMORY_ALIASED_PAGE:     return "aliased-page";
        default:                                   return "unknown";
    }
}

// Even a correctly sized table cannot safely share one IOVA between logically
// different GSP pages. Check alignment and uniqueness before writing any
// descriptor or returning cached initialization arguments.
static inline NVGspMemU32 nvGspValidateQueuePageAddresses(
    const NVGspMemU64 *pages, NVGspMemU32 pageCount)
{
    for (NVGspMemU32 i = 0u; i < pageCount; ++i) {
        if ((pages[i] & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0ull)
            return NV_GSP_QUEUE_MEMORY_BAD_PAGE_ADDRESS;
    }
    for (NVGspMemU32 i = 0u; i < pageCount; ++i) {
        for (NVGspMemU32 j = 0u; j < i; ++j) {
            if (pages[i] == pages[j])
                return NV_GSP_QUEUE_MEMORY_ALIASED_PAGE;
        }
    }
    return NV_GSP_QUEUE_MEMORY_OK;
}

// Independently validate the entire shared queue geometry before dereferencing
// an untrusted page list or writing a page table. In particular, the page
// table must have enough space for every 64-bit IOVA: a valid-looking count
// alone is not sufficient to bound the caller's destination buffer.
static inline bool nvGspQueueMemoryLayoutValid(const NVGspQueuePlan *layout)
{
    if (!layout || !layout->pageTableEntryCount || !layout->sharedBytes ||
        layout->pageTableBytes < NV_GSP_QUEUE_PAGE_BYTES ||
        (layout->pageTableBytes & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0u ||
        (layout->sharedBytes & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0u ||
        (layout->commandQueueBytes & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0u ||
        (layout->statusQueueBytes & (NV_GSP_QUEUE_PAGE_BYTES - 1u)) != 0u ||
        !layout->commandQueueBytes || !layout->statusQueueBytes ||
        layout->pageTableEntryCount >
            layout->pageTableBytes / NV_GSP_QUEUE_PTE_BYTES)
        return false;

    const NVGspMemU64 commandEnd =
        (NVGspMemU64)layout->commandQueueOffset +
        (NVGspMemU64)layout->commandQueueBytes;
    const NVGspMemU64 statusEnd =
        (NVGspMemU64)layout->statusQueueOffset +
        (NVGspMemU64)layout->statusQueueBytes;
    const NVGspMemU64 queuePages =
        ((NVGspMemU64)layout->commandQueueBytes +
         (NVGspMemU64)layout->statusQueueBytes) >>
        NV_GSP_QUEUE_PAGE_SHIFT;
    const NVGspMemU64 tablePages =
        (NVGspMemU64)layout->pageTableBytes >> NV_GSP_QUEUE_PAGE_SHIFT;

    return layout->commandQueueOffset == layout->pageTableBytes &&
           (NVGspMemU64)layout->statusQueueOffset == commandEnd &&
           (NVGspMemU64)layout->sharedBytes == statusEnd &&
           layout->queuePageCount == queuePages &&
           (NVGspMemU64)layout->pageTableEntryCount ==
               queuePages + tablePages &&
           (NVGspMemU64)layout->pageTableEntryCount ==
               ((NVGspMemU64)layout->sharedBytes >>
                NV_GSP_QUEUE_PAGE_SHIFT);
}

// NVIDIA GspMsgQueuesInit() stores the AT_GPU address of EVERY shared 4 KiB
// page into the first page(s) of the same allocation. That count includes the
// pages occupied by the address table itself. sharedMemPhysAddr then points at
// page-address entry 0, allowing GSP to discover the remaining noncontiguous
// pages. These are deliberately named GPU-visible addresses: on an IOMMU host
// the future macOS implementation must derive them in the GA107 DMA domain,
// not assume CPU physical == device-visible.
static inline NVGspMemU32 nvGspBuildQueueMemoryPlan(
    const NVGspQueuePlan *layout,
    bool confidentialCompute,
    const NVGspMemU64 *gpuVisiblePageAddress,
    NVGspMemU32 pageAddressCount,
    NVGspQueueMemoryPlan *out)
{
    if (!layout) return NV_GSP_QUEUE_MEMORY_NULL_PLAN;
    if (!gpuVisiblePageAddress) return NV_GSP_QUEUE_MEMORY_NULL_PAGES;
    if (!out) return NV_GSP_QUEUE_MEMORY_NULL_OUTPUT;

    if (!nvGspQueueMemoryLayoutValid(layout))
        return NV_GSP_QUEUE_MEMORY_BAD_LAYOUT;

    const NVGspMemU32 sharedPages = layout->sharedBytes >> NV_GSP_QUEUE_PAGE_SHIFT;
    if (sharedPages != layout->pageTableEntryCount ||
        pageAddressCount != layout->pageTableEntryCount)
        return NV_GSP_QUEUE_MEMORY_BAD_PAGE_COUNT;

    const NVGspMemU32 addressVerdict =
        nvGspValidateQueuePageAddresses(gpuVisiblePageAddress,pageAddressCount);
    if (addressVerdict != NV_GSP_QUEUE_MEMORY_OK)
        return addressVerdict;

    const NVGspMemU32 commandPages = layout->commandQueueBytes >> NV_GSP_QUEUE_PAGE_SHIFT;
    const NVGspMemU32 statusPages = layout->statusQueueBytes >> NV_GSP_QUEUE_PAGE_SHIFT;
    const NVGspMemU32 commandFirst = layout->commandQueueOffset >> NV_GSP_QUEUE_PAGE_SHIFT;
    const NVGspMemU32 statusFirst = layout->statusQueueOffset >> NV_GSP_QUEUE_PAGE_SHIFT;
    if (commandFirst >= sharedPages || statusFirst >= sharedPages ||
        commandFirst + commandPages > sharedPages ||
        statusFirst + statusPages > sharedPages)
        return NV_GSP_QUEUE_MEMORY_BAD_LAYOUT;

    NVGspQueueMemoryPlan staged = {};
    staged.sharedPageCount = sharedPages;
    staged.pageTableEntryCount = layout->pageTableEntryCount;
    staged.pageTableBytes = layout->pageTableBytes;
    staged.commandFirstPage = commandFirst;
    staged.commandPageCount = commandPages;
    staged.statusFirstPage = statusFirst;
    staged.statusPageCount = statusPages;
    staged.firstPageAddress = gpuVisiblePageAddress[0];

    staged.initArgs.flags = confidentialCompute ? NV_GSP_QUEUE_INIT_FLAG_ENCRYPTION : 0ull;
    staged.initArgs.sharedMemPhysAddr = gpuVisiblePageAddress[0];
    staged.initArgs.cmdQueueOffset = layout->commandQueueOffset;
    staged.initArgs.statQueueOffset = layout->statusQueueOffset;
    staged.initArgs.queueElementHdrSize = layout->queueElementHeaderBytes;
    staged.initArgs.queueElementSizeMin = layout->queueElementMinBytes;
    staged.initArgs.queueElementSizeMax = layout->queueElementMaxBytes;
    staged.initArgs.queueHeaderAlign = layout->queueHeaderAlignBytes;
    staged.initArgs.queueElementAlign = layout->queueElementAlignShift;
    staged.initArgs.pageTableEntryCount = layout->pageTableEntryCount;
    *out = staged;
    return NV_GSP_QUEUE_MEMORY_OK;
}

// Compose exactly the first page's 64-bit address table. The caller supplies a
// span large enough for pageTableEntryCount entries; no padding bytes are
// touched, so a later runtime layer can zero the full page before this copy.
static inline NVGspMemU32 nvGspWriteQueuePageTable(
    const NVGspQueuePlan *layout,
    const NVGspMemU64 *gpuVisiblePageAddress,
    NVGspMemU32 pageAddressCount,
    NVGspMemU64 *outEntries,
    NVGspMemU32 outEntryCapacity,
    NVGspMemU32 *outWritten)
{
    if (outWritten) *outWritten = 0u;
    if (!layout) return NV_GSP_QUEUE_MEMORY_NULL_PLAN;
    if (!gpuVisiblePageAddress) return NV_GSP_QUEUE_MEMORY_NULL_PAGES;
    if (!outEntries) return NV_GSP_QUEUE_MEMORY_NULL_OUTPUT;
    if (!nvGspQueueMemoryLayoutValid(layout))
        return NV_GSP_QUEUE_MEMORY_BAD_LAYOUT;
    if (pageAddressCount != layout->pageTableEntryCount ||
        outEntryCapacity < layout->pageTableEntryCount)
        return NV_GSP_QUEUE_MEMORY_BAD_PAGE_COUNT;

    const NVGspMemU32 addressVerdict =
        nvGspValidateQueuePageAddresses(gpuVisiblePageAddress,pageAddressCount);
    if (addressVerdict != NV_GSP_QUEUE_MEMORY_OK)
        return addressVerdict;

    for (NVGspMemU32 i = 0u; i < pageAddressCount; ++i)
        outEntries[i] = gpuVisiblePageAddress[i];
    if (outWritten) *outWritten = pageAddressCount;
    return NV_GSP_QUEUE_MEMORY_OK;
}

#endif // NV_GSP_QUEUE_MEMORY_HPP
