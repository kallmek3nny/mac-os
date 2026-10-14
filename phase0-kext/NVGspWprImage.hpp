// SPDX-License-Identifier: MIT
// Pure cold-boot GspFwWprMetaV1 image builder for TU102-through-Ada / GA107.
#ifndef NV_GSP_WPR_IMAGE_HPP
#define NV_GSP_WPR_IMAGE_HPP

#include "NVGspWprMeta.hpp"

#define NV_GSP_WPR_IMAGE_NOT_ATTEMPTED             0u
#define NV_GSP_WPR_IMAGE_OK                        1u
#define NV_GSP_WPR_IMAGE_NULL_INPUT                2u
#define NV_GSP_WPR_IMAGE_NULL_OUTPUT               3u
#define NV_GSP_WPR_IMAGE_BAD_LAYOUT                4u
#define NV_GSP_WPR_IMAGE_BAD_FIRMWARE              5u
#define NV_GSP_WPR_IMAGE_BAD_BOOTLOADER            6u
#define NV_GSP_WPR_IMAGE_BAD_BOOT_OFFSETS          7u
#define NV_GSP_WPR_IMAGE_BAD_SIGNATURE             8u
#define NV_GSP_WPR_IMAGE_BAD_VGA_WORKSPACE         9u
#define NV_GSP_WPR_IMAGE_BAD_CRASH_QUEUE           10u
#define NV_GSP_WPR_IMAGE_BAD_FLAGS                 11u
#define NV_GSP_WPR_IMAGE_LIMIT                     12u

// Public GSP_FW_FLAGS bits currently consumed by the TU102-through-Ada V1 ABI.
#define NV_GSP_WPR_FLAG_CLOCK_BOOST                 0x01u
#define NV_GSP_WPR_FLAG_PPCIE_ENABLED               0x04u
#define NV_GSP_WPR_FLAG_MULTI_GPU_NVLE_ENABLED      0x08u
#define NV_GSP_WPR_FLAG_KNOWN_MASK                  0x0du

struct NVGspWprImageInput {
    NVGspWprLayoutPlan layout;
    NVGspWprU64 framebufferBytes;

    NVGspWprU64 radix3ElfGpuAddress;
    NVGspWprU64 radix3ElfBytes;

    NVGspWprU64 bootloaderGpuAddress;
    NVGspWprU64 bootloaderBytes;
    NVGspWprU64 bootloaderCodeOffset;
    NVGspWprU64 bootloaderDataOffset;
    NVGspWprU64 bootloaderManifestOffset;

    // Signature is optional in open RM, but address and size must appear as a pair.
    NVGspWprU64 signatureGpuAddress;
    NVGspWprU64 signatureBytes;

    NVGspWprU64 vgaWorkspaceOffset;
    NVGspWprU64 vgaWorkspaceBytes;

    // CrashCat queue is optional. V1 stores its size as NvU32.
    NVGspWprU64 crashQueueGpuAddress;
    NVGspWprU64 crashQueueBytes;

    NVGspWprU8  vfPartitionCount;
    NVGspWprU8  flags;
    NVGspWprU16 pagingConfig;
};

static inline bool nvGspWprImageSucceeded(NVGspWprU32 verdict)
{
    return verdict == NV_GSP_WPR_IMAGE_OK;
}

static inline const char *nvGspWprImageVerdictName(NVGspWprU32 verdict)
{
    switch (verdict) {
        case NV_GSP_WPR_IMAGE_NOT_ATTEMPTED:     return "not-attempted";
        case NV_GSP_WPR_IMAGE_OK:                return "ok";
        case NV_GSP_WPR_IMAGE_NULL_INPUT:        return "null-input";
        case NV_GSP_WPR_IMAGE_NULL_OUTPUT:       return "null-output";
        case NV_GSP_WPR_IMAGE_BAD_LAYOUT:        return "bad-layout";
        case NV_GSP_WPR_IMAGE_BAD_FIRMWARE:      return "bad-firmware";
        case NV_GSP_WPR_IMAGE_BAD_BOOTLOADER:    return "bad-bootloader";
        case NV_GSP_WPR_IMAGE_BAD_BOOT_OFFSETS:  return "bad-boot-offsets";
        case NV_GSP_WPR_IMAGE_BAD_SIGNATURE:     return "bad-signature";
        case NV_GSP_WPR_IMAGE_BAD_VGA_WORKSPACE: return "bad-vga-workspace";
        case NV_GSP_WPR_IMAGE_BAD_CRASH_QUEUE:   return "bad-crash-queue";
        case NV_GSP_WPR_IMAGE_BAD_FLAGS:         return "bad-flags";
        default:                                  return "unknown";
    }
}

static inline bool nvGspWprOptionalPairValid(NVGspWprU64 address,
                                             NVGspWprU64 bytes)
{
    return (address == 0ull && bytes == 0ull) ||
           (address != 0ull && bytes != 0ull);
}

// Populate the CPU-RM-owned metadata image handed to Booter on a normal cold
// boot. `verified` deliberately remains zero: open RM initializes it to zero and
// Booter is the authority that changes it to GSP_FW_WPR_META_VERIFIED after
// authenticating and locking WPR2. This helper never fabricates that state.
static inline NVGspWprU32 nvGspBuildColdBootWprMeta(
    const NVGspWprImageInput *in,
    NVGspWprMetaV1 *out)
{
    if (!in) return NV_GSP_WPR_IMAGE_NULL_INPUT;
    if (!out) return NV_GSP_WPR_IMAGE_NULL_OUTPUT;

    const NVGspWprLayoutPlan &p = in->layout;
    if (!in->framebufferBytes ||
        !p.gspFwWprEnd || !p.gspFwWprStart || !p.gspFwHeapOffset ||
        !p.gspFwHeapSize || !p.gspFwOffset || !p.bootBinOffset ||
        p.gspFwRsvdStart > p.gspFwWprStart ||
        p.gspFwWprStart >= p.gspFwWprEnd ||
        p.gspFwWprEnd > in->framebufferBytes ||
        p.gspFwOffset >= p.bootBinOffset ||
        p.bootBinOffset > p.frtsOffset ||
        p.frtsOffset > p.gspFwWprEnd ||
        (p.gspFwWprStart & (NV_GSP_WPR_ALIGNMENT_BYTES - 1ull)) != 0ull ||
        (p.gspFwWprEnd & (NV_GSP_WPR_ALIGNMENT_BYTES - 1ull)) != 0ull ||
        (p.gspFwHeapOffset & (NV_GSP_WPR_HEAP_ALIGNMENT_BYTES - 1ull)) != 0ull ||
        (p.gspFwOffset & (NV_GSP_WPR_ELF_ALIGNMENT_BYTES - 1ull)) != 0ull ||
        (p.bootBinOffset & (NV_GSP_WPR_BOOT_ALIGNMENT_BYTES - 1ull)) != 0ull)
        return NV_GSP_WPR_IMAGE_BAD_LAYOUT;

    if (!in->radix3ElfGpuAddress || !in->radix3ElfBytes ||
        in->radix3ElfBytes > p.bootBinOffset - p.gspFwOffset)
        return NV_GSP_WPR_IMAGE_BAD_FIRMWARE;

    if (!in->bootloaderGpuAddress || !in->bootloaderBytes ||
        in->bootloaderBytes > p.frtsOffset - p.bootBinOffset)
        return NV_GSP_WPR_IMAGE_BAD_BOOTLOADER;

    if (in->bootloaderCodeOffset >= in->bootloaderBytes ||
        in->bootloaderDataOffset >= in->bootloaderBytes ||
        in->bootloaderManifestOffset >= in->bootloaderBytes)
        return NV_GSP_WPR_IMAGE_BAD_BOOT_OFFSETS;

    if (!nvGspWprOptionalPairValid(in->signatureGpuAddress,in->signatureBytes))
        return NV_GSP_WPR_IMAGE_BAD_SIGNATURE;

    if (in->vgaWorkspaceOffset > in->framebufferBytes ||
        in->vgaWorkspaceBytes != in->framebufferBytes - in->vgaWorkspaceOffset)
        return NV_GSP_WPR_IMAGE_BAD_VGA_WORKSPACE;

    if (!nvGspWprOptionalPairValid(in->crashQueueGpuAddress,in->crashQueueBytes) ||
        in->crashQueueBytes > 0xffffffffull)
        return NV_GSP_WPR_IMAGE_BAD_CRASH_QUEUE;

    if ((in->flags & (NVGspWprU8)~NV_GSP_WPR_FLAG_KNOWN_MASK) != 0u)
        return NV_GSP_WPR_IMAGE_BAD_FLAGS;

    NVGspWprMetaV1 staged = {};
    staged.magic = NV_GSP_WPR_META_MAGIC;
    staged.revision = NV_GSP_WPR_META_REVISION;

    staged.sysmemAddrOfRadix3Elf = in->radix3ElfGpuAddress;
    staged.sizeOfRadix3Elf = in->radix3ElfBytes;
    staged.sysmemAddrOfBootloader = in->bootloaderGpuAddress;
    staged.sizeOfBootloader = in->bootloaderBytes;
    staged.bootloaderCodeOffset = in->bootloaderCodeOffset;
    staged.bootloaderDataOffset = in->bootloaderDataOffset;
    staged.bootloaderManifestOffset = in->bootloaderManifestOffset;
    staged.sysmemAddrOfSignature = in->signatureGpuAddress;
    staged.sizeOfSignature = in->signatureBytes;

    staged.gspFwRsvdStart = p.gspFwRsvdStart;
    staged.nonWprHeapOffset = p.nonWprHeapOffset;
    staged.nonWprHeapSize = p.nonWprHeapSize;
    staged.gspFwWprStart = p.gspFwWprStart;
    staged.gspFwHeapOffset = p.gspFwHeapOffset;
    staged.gspFwHeapSize = p.gspFwHeapSize;
    staged.gspFwOffset = p.gspFwOffset;
    staged.bootBinOffset = p.bootBinOffset;
    staged.frtsOffset = p.frtsOffset;
    staged.frtsSize = p.gspFwWprEnd - p.frtsOffset;
    staged.gspFwWprEnd = p.gspFwWprEnd;
    staged.fbSize = in->framebufferBytes;
    staged.vgaWorkspaceOffset = in->vgaWorkspaceOffset;
    staged.vgaWorkspaceSize = in->vgaWorkspaceBytes;

    // Source cold-boot defaults. Booter increments/validates state later.
    staged.bootCount = 0ull;
    staged.sysmemAddrOfCrashReportQueue = in->crashQueueGpuAddress;
    staged.sizeOfCrashReportQueue = (NVGspWprU32)in->crashQueueBytes;
    staged.gspFwHeapVfPartitionCount = in->vfPartitionCount;
    staged.flags = in->flags;
    staged.pagingConfig = in->pagingConfig;
    staged.verified = 0ull;

    *out = staged;
    return NV_GSP_WPR_IMAGE_OK;
}

#endif // NV_GSP_WPR_IMAGE_HPP
