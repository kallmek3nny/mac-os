// SPDX-License-Identifier: MIT
// Pure TU102-through-Ada GSP WPR metadata ABI + layout rehearsal.
#ifndef NV_GSP_WPR_META_HPP
#define NV_GSP_WPR_META_HPP

using NVGspWprU8  = unsigned char;
using NVGspWprU16 = unsigned short;
using NVGspWprU32 = unsigned int;
using NVGspWprU64 = unsigned long long;

// NVIDIA open RM: gsp_fw_wpr_meta*.h and kernel_gsp_tu102.c.
#define NV_GSP_WPR_META_V1_BYTES                  256u
#define NV_GSP_WPR_META_MAGIC                     0xdc3aae21371a60b3ull
#define NV_GSP_WPR_META_REVISION                  1ull
#define NV_GSP_WPR_META_VERIFIED                  0xa0a0a0a0a0a0a0a0ull
#define NV_GSP_WPR_ALIGNMENT_BYTES                0x00020000ull
#define NV_GSP_WPR_META_RESERVED_BYTES            0x00100000ull
#define NV_GSP_WPR_HEAP_ALIGNMENT_BYTES           0x00100000ull
#define NV_GSP_WPR_ELF_ALIGNMENT_BYTES            0x00010000ull
#define NV_GSP_WPR_BOOT_ALIGNMENT_BYTES           0x00001000ull

#define NV_GSP_WPR_PLAN_NOT_ATTEMPTED             0u
#define NV_GSP_WPR_PLAN_OK                        1u
#define NV_GSP_WPR_PLAN_NULL_INPUT                2u
#define NV_GSP_WPR_PLAN_NULL_OUTPUT               3u
#define NV_GSP_WPR_PLAN_BAD_FRAMEBUFFER           4u
#define NV_GSP_WPR_PLAN_BAD_VBIOS_RESERVATION     5u
#define NV_GSP_WPR_PLAN_BAD_IMAGE_SIZE            6u
#define NV_GSP_WPR_PLAN_BAD_HEAP_SIZE             7u
#define NV_GSP_WPR_PLAN_UNDERFLOW                 8u
#define NV_GSP_WPR_PLAN_OVERFLOW                  9u
#define NV_GSP_WPR_PLAN_BAD_LAYOUT                10u
#define NV_GSP_WPR_PLAN_LIMIT                     11u

// Exact 256-byte Turing-through-Ada Booter metadata ABI.
struct NVGspWprMetaV1 {
    NVGspWprU64 magic;
    NVGspWprU64 revision;
    NVGspWprU64 sysmemAddrOfRadix3Elf;
    NVGspWprU64 sizeOfRadix3Elf;
    NVGspWprU64 sysmemAddrOfBootloader;
    NVGspWprU64 sizeOfBootloader;
    NVGspWprU64 bootloaderCodeOffset;
    NVGspWprU64 bootloaderDataOffset;
    NVGspWprU64 bootloaderManifestOffset;
    NVGspWprU64 sysmemAddrOfSignature;
    NVGspWprU64 sizeOfSignature;
    NVGspWprU64 gspFwRsvdStart;
    NVGspWprU64 nonWprHeapOffset;
    NVGspWprU64 nonWprHeapSize;
    NVGspWprU64 gspFwWprStart;
    NVGspWprU64 gspFwHeapOffset;
    NVGspWprU64 gspFwHeapSize;
    NVGspWprU64 gspFwOffset;
    NVGspWprU64 bootBinOffset;
    NVGspWprU64 frtsOffset;
    NVGspWprU64 frtsSize;
    NVGspWprU64 gspFwWprEnd;
    NVGspWprU64 fbSize;
    NVGspWprU64 vgaWorkspaceOffset;
    NVGspWprU64 vgaWorkspaceSize;
    NVGspWprU64 bootCount;
    NVGspWprU8  reserved1[16];
    NVGspWprU64 sysmemAddrOfCrashReportQueue;
    NVGspWprU32 sizeOfCrashReportQueue;
    NVGspWprU32 reserved2;
    NVGspWprU8  gspFwHeapVfPartitionCount;
    NVGspWprU8  flags;
    NVGspWprU16 pagingConfig;
    NVGspWprU32 reserved3;
    NVGspWprU64 verified;
};

// This planner starts after the pieces that are intentionally delegated to
// other RM helpers. In particular, wprHeapBytes is the result of
// kgspGetFwHeapSize(), and nonWprHeapRawBytes is the result of
// kgspGetNonWprHeapSize(). The planner reproduces TU102's placement order and
// alignment only; it does not invent either policy.
struct NVGspWprLayoutInput {
    NVGspWprU64 fbSize;
    NVGspWprU64 vbiosReservedOffset;
    NVGspWprU64 wprEndMargin;
    NVGspWprU64 frtsSize;
    NVGspWprU64 bootloaderSize;
    NVGspWprU64 radix3ElfSize;
    NVGspWprU64 nonWprHeapRawBytes;
    NVGspWprU64 wprHeapBytes;
};

struct NVGspWprLayoutPlan {
    NVGspWprU64 gspFwWprEnd;
    NVGspWprU64 frtsOffset;
    NVGspWprU64 bootBinOffset;
    NVGspWprU64 gspFwOffset;
    NVGspWprU64 postWprHeapSize;
    NVGspWprU64 wprMetaReservedBytes;
    NVGspWprU64 nonWprHeapSize;
    NVGspWprU64 preWprHeapSize;
    NVGspWprU64 gspFwHeapOffset;
    NVGspWprU64 gspFwHeapSize;
    NVGspWprU64 gspFwWprStart;
    NVGspWprU64 nonWprHeapOffset;
    NVGspWprU64 gspFwRsvdStart;
};

static inline bool nvGspWprPlanSucceeded(NVGspWprU32 verdict)
{
    return verdict == NV_GSP_WPR_PLAN_OK;
}

static inline const char *nvGspWprPlanVerdictName(NVGspWprU32 verdict)
{
    switch (verdict) {
        case NV_GSP_WPR_PLAN_NOT_ATTEMPTED:         return "not-attempted";
        case NV_GSP_WPR_PLAN_OK:                    return "ok";
        case NV_GSP_WPR_PLAN_NULL_INPUT:            return "null-input";
        case NV_GSP_WPR_PLAN_NULL_OUTPUT:           return "null-output";
        case NV_GSP_WPR_PLAN_BAD_FRAMEBUFFER:       return "bad-framebuffer";
        case NV_GSP_WPR_PLAN_BAD_VBIOS_RESERVATION: return "bad-vbios-reservation";
        case NV_GSP_WPR_PLAN_BAD_IMAGE_SIZE:        return "bad-image-size";
        case NV_GSP_WPR_PLAN_BAD_HEAP_SIZE:         return "bad-heap-size";
        case NV_GSP_WPR_PLAN_UNDERFLOW:             return "underflow";
        case NV_GSP_WPR_PLAN_OVERFLOW:              return "overflow";
        case NV_GSP_WPR_PLAN_BAD_LAYOUT:            return "bad-layout";
        default:                                     return "unknown";
    }
}

static inline NVGspWprU64 nvGspWprAlignDown(NVGspWprU64 value,
                                            NVGspWprU64 alignment)
{
    if (!alignment || (alignment & (alignment - 1ull)) != 0ull) return 0ull;
    return value & ~(alignment - 1ull);
}

static inline NVGspWprU64 nvGspWprAlignUp(NVGspWprU64 value,
                                          NVGspWprU64 alignment)
{
    if (!alignment || (alignment & (alignment - 1ull)) != 0ull) return 0ull;
    const NVGspWprU64 mask = alignment - 1ull;
    if (value > ~0ull - mask) return 0ull;
    return (value + mask) & ~mask;
}

static inline NVGspWprU32 nvGspPlanWprLayout(const NVGspWprLayoutInput *in,
                                             NVGspWprLayoutPlan *out)
{
    if (!in) return NV_GSP_WPR_PLAN_NULL_INPUT;
    if (!out) return NV_GSP_WPR_PLAN_NULL_OUTPUT;
    if (!in->fbSize) return NV_GSP_WPR_PLAN_BAD_FRAMEBUFFER;
    if (!in->vbiosReservedOffset || in->vbiosReservedOffset > in->fbSize)
        return NV_GSP_WPR_PLAN_BAD_VBIOS_RESERVATION;
    if (!in->bootloaderSize || !in->radix3ElfSize)
        return NV_GSP_WPR_PLAN_BAD_IMAGE_SIZE;
    if (!in->wprHeapBytes)
        return NV_GSP_WPR_PLAN_BAD_HEAP_SIZE;
    if (in->wprEndMargin >= in->vbiosReservedOffset)
        return NV_GSP_WPR_PLAN_UNDERFLOW;

    NVGspWprLayoutPlan staged = {};
    staged.gspFwWprEnd = nvGspWprAlignDown(
        in->vbiosReservedOffset - in->wprEndMargin,
        NV_GSP_WPR_ALIGNMENT_BYTES);
    if (!staged.gspFwWprEnd || in->frtsSize > staged.gspFwWprEnd)
        return NV_GSP_WPR_PLAN_UNDERFLOW;

    staged.frtsOffset = staged.gspFwWprEnd - in->frtsSize;
    if (in->bootloaderSize > staged.frtsOffset)
        return NV_GSP_WPR_PLAN_UNDERFLOW;
    staged.bootBinOffset = nvGspWprAlignDown(
        staged.frtsOffset - in->bootloaderSize,
        NV_GSP_WPR_BOOT_ALIGNMENT_BYTES);
    if (!staged.bootBinOffset || in->radix3ElfSize > staged.bootBinOffset)
        return NV_GSP_WPR_PLAN_UNDERFLOW;

    staged.gspFwOffset = nvGspWprAlignDown(
        staged.bootBinOffset - in->radix3ElfSize,
        NV_GSP_WPR_ELF_ALIGNMENT_BYTES);
    if (!staged.gspFwOffset || staged.gspFwOffset > in->fbSize)
        return NV_GSP_WPR_PLAN_BAD_LAYOUT;

    const NVGspWprU64 postRaw = in->fbSize - staged.gspFwOffset;
    staged.postWprHeapSize = nvGspWprAlignUp(
        postRaw, NV_GSP_WPR_HEAP_ALIGNMENT_BYTES);
    staged.wprMetaReservedBytes = nvGspWprAlignUp(
        (NVGspWprU64)NV_GSP_WPR_META_V1_BYTES,
        NV_GSP_WPR_HEAP_ALIGNMENT_BYTES);
    staged.nonWprHeapSize = nvGspWprAlignUp(
        in->nonWprHeapRawBytes,
        NV_GSP_WPR_HEAP_ALIGNMENT_BYTES);
    if (!staged.postWprHeapSize || !staged.wprMetaReservedBytes ||
        (in->nonWprHeapRawBytes && !staged.nonWprHeapSize))
        return NV_GSP_WPR_PLAN_OVERFLOW;
    if (staged.wprMetaReservedBytes > ~0ull - staged.nonWprHeapSize)
        return NV_GSP_WPR_PLAN_OVERFLOW;
    staged.preWprHeapSize = staged.wprMetaReservedBytes + staged.nonWprHeapSize;

    if (in->wprHeapBytes > staged.gspFwOffset)
        return NV_GSP_WPR_PLAN_UNDERFLOW;
    staged.gspFwHeapOffset = nvGspWprAlignDown(
        staged.gspFwOffset - in->wprHeapBytes,
        NV_GSP_WPR_HEAP_ALIGNMENT_BYTES);
    if (!staged.gspFwHeapOffset)
        return NV_GSP_WPR_PLAN_UNDERFLOW;

    staged.gspFwHeapSize = nvGspWprAlignDown(
        staged.gspFwOffset - staged.gspFwHeapOffset,
        NV_GSP_WPR_HEAP_ALIGNMENT_BYTES);
    if (!staged.gspFwHeapSize ||
        staged.wprMetaReservedBytes > staged.gspFwHeapOffset)
        return NV_GSP_WPR_PLAN_BAD_HEAP_SIZE;

    staged.gspFwWprStart = staged.gspFwHeapOffset - staged.wprMetaReservedBytes;
    if (staged.nonWprHeapSize > staged.gspFwWprStart)
        return NV_GSP_WPR_PLAN_UNDERFLOW;
    staged.nonWprHeapOffset = staged.gspFwWprStart - staged.nonWprHeapSize;
    staged.gspFwRsvdStart = staged.nonWprHeapOffset;

    if ((staged.gspFwWprEnd & (NV_GSP_WPR_ALIGNMENT_BYTES - 1ull)) != 0ull ||
        (staged.gspFwWprStart & (NV_GSP_WPR_ALIGNMENT_BYTES - 1ull)) != 0ull ||
        (staged.gspFwHeapOffset & (NV_GSP_WPR_HEAP_ALIGNMENT_BYTES - 1ull)) != 0ull ||
        (staged.gspFwOffset & (NV_GSP_WPR_ELF_ALIGNMENT_BYTES - 1ull)) != 0ull ||
        (staged.bootBinOffset & (NV_GSP_WPR_BOOT_ALIGNMENT_BYTES - 1ull)) != 0ull ||
        staged.gspFwRsvdStart > staged.gspFwWprStart ||
        staged.gspFwWprStart >= staged.gspFwWprEnd ||
        staged.gspFwOffset >= staged.bootBinOffset ||
        staged.bootBinOffset > staged.frtsOffset ||
        staged.frtsOffset > staged.gspFwWprEnd)
        return NV_GSP_WPR_PLAN_BAD_LAYOUT;

    *out = staged;
    return NV_GSP_WPR_PLAN_OK;
}

#endif // NV_GSP_WPR_META_HPP
