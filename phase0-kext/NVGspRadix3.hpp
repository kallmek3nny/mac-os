// SPDX-License-Identifier: MIT
// Pure embedded-data Radix3 planner/image builder matching open RM kgspCreateRadix3().
#ifndef NV_GSP_RADIX3_HPP
#define NV_GSP_RADIX3_HPP

using NVGspRadixU8  = unsigned char;
using NVGspRadixU32 = unsigned int;
using NVGspRadixU64 = unsigned long long;

#define NV_GSP_RADIX3_PAGE_SHIFT                  12u
#define NV_GSP_RADIX3_PAGE_BYTES                  (1u << NV_GSP_RADIX3_PAGE_SHIFT)
#define NV_GSP_RADIX3_ENTRY_BYTES                 8u
#define NV_GSP_RADIX3_ENTRIES_PER_PAGE            (NV_GSP_RADIX3_PAGE_BYTES / NV_GSP_RADIX3_ENTRY_BYTES)
#define NV_GSP_RADIX3_LEVELS                      4u
#define NV_GSP_RADIX3_MAX_DATA_PAGES              134217728ull /* 512^3 */

#define NV_GSP_RADIX3_NOT_ATTEMPTED               0u
#define NV_GSP_RADIX3_OK                          1u
#define NV_GSP_RADIX3_NULL_OUTPUT                 2u
#define NV_GSP_RADIX3_ZERO_SIZE                   3u
#define NV_GSP_RADIX3_TOO_LARGE                   4u
#define NV_GSP_RADIX3_OVERFLOW                    5u
#define NV_GSP_RADIX3_NULL_PAGES                  6u
#define NV_GSP_RADIX3_BAD_PAGE_COUNT              7u
#define NV_GSP_RADIX3_BAD_PAGE_ADDRESS            8u
#define NV_GSP_RADIX3_NULL_DATA                   9u
#define NV_GSP_RADIX3_NULL_IMAGE                  10u
#define NV_GSP_RADIX3_IMAGE_SHORT                 11u
#define NV_GSP_RADIX3_DATA_SIZE_MISMATCH          12u
#define NV_GSP_RADIX3_LIMIT                       13u

struct NVGspRadix3Plan {
    NVGspRadixU64 dataBytes;
    NVGspRadixU64 levelPages[NV_GSP_RADIX3_LEVELS];
    NVGspRadixU64 levelOffset[NV_GSP_RADIX3_LEVELS];
    NVGspRadixU64 tablePages;
    NVGspRadixU64 tableBytes;
    NVGspRadixU64 dataPages;
    NVGspRadixU64 paddedDataBytes;
    NVGspRadixU64 allocationPages;
    NVGspRadixU64 allocationBytes;
};

static inline bool nvGspRadix3Succeeded(NVGspRadixU32 verdict)
{
    return verdict == NV_GSP_RADIX3_OK;
}

static inline const char *nvGspRadix3VerdictName(NVGspRadixU32 verdict)
{
    switch (verdict) {
        case NV_GSP_RADIX3_NOT_ATTEMPTED:      return "not-attempted";
        case NV_GSP_RADIX3_OK:                 return "ok";
        case NV_GSP_RADIX3_NULL_OUTPUT:        return "null-output";
        case NV_GSP_RADIX3_ZERO_SIZE:          return "zero-size";
        case NV_GSP_RADIX3_TOO_LARGE:          return "too-large";
        case NV_GSP_RADIX3_OVERFLOW:           return "overflow";
        case NV_GSP_RADIX3_NULL_PAGES:         return "null-pages";
        case NV_GSP_RADIX3_BAD_PAGE_COUNT:     return "bad-page-count";
        case NV_GSP_RADIX3_BAD_PAGE_ADDRESS:   return "bad-page-address";
        case NV_GSP_RADIX3_NULL_DATA:          return "null-data";
        case NV_GSP_RADIX3_NULL_IMAGE:         return "null-image";
        case NV_GSP_RADIX3_IMAGE_SHORT:        return "image-short";
        case NV_GSP_RADIX3_DATA_SIZE_MISMATCH: return "data-size-mismatch";
        default:                               return "unknown";
    }
}

static inline bool nvGspRadix3AddOverflow(NVGspRadixU64 a,
                                          NVGspRadixU64 b,
                                          NVGspRadixU64 *out)
{
    if (!out || a > ~0ull - b) return true;
    *out = a + b;
    return false;
}

// Mirrors open RM kgspCreateRadix3():
//   radix3[3] = ceil(data / 4 KiB)
//   radix3[i-1] = ceil(radix3[i] / 512)
// followed by low-to-high page-table placement. This helper models the path
// used for GSP-RM firmware where the data is embedded after the table pages in
// the same memory descriptor. The root must be exactly one 4 KiB page.
static inline NVGspRadixU32 nvGspPlanEmbeddedRadix3(NVGspRadixU64 dataBytes,
                                                    NVGspRadix3Plan *out)
{
    if (!out) return NV_GSP_RADIX3_NULL_OUTPUT;
    if (!dataBytes) return NV_GSP_RADIX3_ZERO_SIZE;
    if (dataBytes > ~0ull - (NV_GSP_RADIX3_PAGE_BYTES - 1ull))
        return NV_GSP_RADIX3_OVERFLOW;

    NVGspRadix3Plan staged = {};
    staged.dataBytes = dataBytes;
    staged.levelPages[3] =
        (dataBytes + (NV_GSP_RADIX3_PAGE_BYTES - 1ull)) >> NV_GSP_RADIX3_PAGE_SHIFT;
    if (!staged.levelPages[3] || staged.levelPages[3] > NV_GSP_RADIX3_MAX_DATA_PAGES)
        return NV_GSP_RADIX3_TOO_LARGE;

    for (NVGspRadixU32 i = 3u; i > 0u; --i)
        staged.levelPages[i - 1u] =
            ((staged.levelPages[i] - 1ull) / NV_GSP_RADIX3_ENTRIES_PER_PAGE) + 1ull;

    if (staged.levelPages[0] != 1ull)
        return NV_GSP_RADIX3_TOO_LARGE;

    NVGspRadixU64 priorPages = 0ull;
    staged.levelOffset[0] = 0ull;
    for (NVGspRadixU32 i = 1u; i < NV_GSP_RADIX3_LEVELS; ++i) {
        NVGspRadixU64 nextPrior = 0ull;
        if (nvGspRadix3AddOverflow(priorPages, staged.levelPages[i - 1u], &nextPrior))
            return NV_GSP_RADIX3_OVERFLOW;
        priorPages = nextPrior;
        if (priorPages > (~0ull >> NV_GSP_RADIX3_PAGE_SHIFT))
            return NV_GSP_RADIX3_OVERFLOW;
        staged.levelOffset[i] = priorPages << NV_GSP_RADIX3_PAGE_SHIFT;
    }

    staged.tablePages = priorPages;
    staged.tableBytes = staged.levelOffset[3];
    staged.dataPages = staged.levelPages[3];
    if (staged.dataPages > (~0ull >> NV_GSP_RADIX3_PAGE_SHIFT))
        return NV_GSP_RADIX3_OVERFLOW;
    staged.paddedDataBytes = staged.dataPages << NV_GSP_RADIX3_PAGE_SHIFT;

    if (nvGspRadix3AddOverflow(staged.tablePages, staged.dataPages,
                              &staged.allocationPages))
        return NV_GSP_RADIX3_OVERFLOW;
    if (staged.allocationPages > (~0ull >> NV_GSP_RADIX3_PAGE_SHIFT))
        return NV_GSP_RADIX3_OVERFLOW;
    staged.allocationBytes = staged.allocationPages << NV_GSP_RADIX3_PAGE_SHIFT;

    *out = staged;
    return NV_GSP_RADIX3_OK;
}

static inline void nvGspRadix3PutLe64(NVGspRadixU8 *p, NVGspRadixU64 v)
{
    for (NVGspRadixU32 i = 0u; i < 8u; ++i)
        p[i] = (NVGspRadixU8)((v >> (i * 8u)) & 0xffull);
}

// Build the exact table/data image for the embedded-data path. `gpuPageAddress`
// is the device-visible address of every page in the newly allocated descriptor,
// in descriptor order. All inputs are validated before caller-visible output is
// touched. Table entries are raw 64-bit GPU page addresses, matching
// memdescGetPhysAddrs(... AT_GPU ...) in open RM.
static inline NVGspRadixU32 nvGspBuildEmbeddedRadix3Image(
    const NVGspRadix3Plan *plan,
    const NVGspRadixU64 *gpuPageAddress,
    NVGspRadixU64 gpuPageCount,
    const NVGspRadixU8 *data,
    NVGspRadixU64 dataBytes,
    NVGspRadixU8 *image,
    NVGspRadixU64 imageCapacity,
    NVGspRadixU64 *outImageBytes)
{
    if (!plan || !outImageBytes) return NV_GSP_RADIX3_NULL_OUTPUT;
    if (!gpuPageAddress) return NV_GSP_RADIX3_NULL_PAGES;
    if (!data) return NV_GSP_RADIX3_NULL_DATA;
    if (!image) return NV_GSP_RADIX3_NULL_IMAGE;
    if (dataBytes != plan->dataBytes) return NV_GSP_RADIX3_DATA_SIZE_MISMATCH;
    if (gpuPageCount != plan->allocationPages) return NV_GSP_RADIX3_BAD_PAGE_COUNT;
    if (imageCapacity < plan->allocationBytes) return NV_GSP_RADIX3_IMAGE_SHORT;

    for (NVGspRadixU64 i = 0ull; i < gpuPageCount; ++i) {
        if (!gpuPageAddress[i] ||
            (gpuPageAddress[i] & (NV_GSP_RADIX3_PAGE_BYTES - 1ull)) != 0ull)
            return NV_GSP_RADIX3_BAD_PAGE_ADDRESS;
    }

    // Revalidate the plan's structural invariants before mutation.
    NVGspRadix3Plan expected = {};
    const NVGspRadixU32 planVerdict = nvGspPlanEmbeddedRadix3(plan->dataBytes, &expected);
    if (planVerdict != NV_GSP_RADIX3_OK) return planVerdict;
    for (NVGspRadixU32 i = 0u; i < NV_GSP_RADIX3_LEVELS; ++i) {
        if (plan->levelPages[i] != expected.levelPages[i] ||
            plan->levelOffset[i] != expected.levelOffset[i])
            return NV_GSP_RADIX3_BAD_PAGE_COUNT;
    }
    if (plan->tablePages != expected.tablePages ||
        plan->tableBytes != expected.tableBytes ||
        plan->dataPages != expected.dataPages ||
        plan->paddedDataBytes != expected.paddedDataBytes ||
        plan->allocationPages != expected.allocationPages ||
        plan->allocationBytes != expected.allocationBytes)
        return NV_GSP_RADIX3_BAD_PAGE_COUNT;

    // Validation is complete. Zero the full descriptor like open RM clears the
    // page-table region and the partial final data page.
    for (NVGspRadixU64 i = 0ull; i < plan->allocationBytes; ++i)
        image[i] = 0u;

    // Levels 0 and 1 point to the pages of the next table level.
    for (NVGspRadixU32 level = 0u; level < 2u; ++level) {
        const NVGspRadixU64 nextFirstPage = plan->levelOffset[level + 1u] >> NV_GSP_RADIX3_PAGE_SHIFT;
        for (NVGspRadixU64 e = 0ull; e < plan->levelPages[level + 1u]; ++e) {
            const NVGspRadixU64 dst = plan->levelOffset[level] + e * NV_GSP_RADIX3_ENTRY_BYTES;
            nvGspRadix3PutLe64(image + dst, gpuPageAddress[nextFirstPage + e]);
        }
    }

    // Level 2 is the leaf page-address table for the embedded firmware pages.
    const NVGspRadixU64 dataFirstPage = plan->levelOffset[3] >> NV_GSP_RADIX3_PAGE_SHIFT;
    for (NVGspRadixU64 e = 0ull; e < plan->dataPages; ++e) {
        const NVGspRadixU64 dst = plan->levelOffset[2] + e * NV_GSP_RADIX3_ENTRY_BYTES;
        nvGspRadix3PutLe64(image + dst, gpuPageAddress[dataFirstPage + e]);
    }

    for (NVGspRadixU64 i = 0ull; i < dataBytes; ++i)
        image[plan->levelOffset[3] + i] = data[i];

    *outImageBytes = plan->allocationBytes;
    return NV_GSP_RADIX3_OK;
}

#endif // NV_GSP_RADIX3_HPP
