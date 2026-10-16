// SPDX-License-Identifier: MIT
// Pure composition from staged GA107 sysmem + observed runtime facts to the
// complete cold-boot GspFwWprMetaV1 image. No IOKit, allocation, or hardware I/O.
#ifndef NV_GSP_WPR_COLD_BOOT_COMPOSE_HPP
#define NV_GSP_WPR_COLD_BOOT_COMPOSE_HPP

#include "NVGspWprSysmem.hpp"
#include "NVGspWprGa107Facts.hpp"
#include "NVGspWprGa107.hpp"
#include "NVGspWprImage.hpp"

#define NV_GSP_WPR_COLD_BOOT_NOT_ATTEMPTED  0u
#define NV_GSP_WPR_COLD_BOOT_OK             1u
#define NV_GSP_WPR_COLD_BOOT_NULL_INPUT     2u
#define NV_GSP_WPR_COLD_BOOT_NULL_OUTPUT    3u
#define NV_GSP_WPR_COLD_BOOT_BAD_SYSMEM     4u
#define NV_GSP_WPR_COLD_BOOT_BAD_FACTS      5u
#define NV_GSP_WPR_COLD_BOOT_POLICY_REFUSED 6u
#define NV_GSP_WPR_COLD_BOOT_META_REFUSED   7u
#define NV_GSP_WPR_COLD_BOOT_LIMIT          8u

// Inputs that open RM may vary at runtime and that this project has not reduced
// to a universal GA107 constant. Zero is legal where the downstream V1 ABI says
// it is legal; this helper deliberately does not invent platform policy.
struct NVGspWprColdBootOptions {
    NVGspWprU64 signatureGpuAddress;
    NVGspWprU64 signatureBytes;
    NVGspWprU64 crashQueueGpuAddress;
    NVGspWprU64 crashQueueBytes;
    NVGspWprU8  vfPartitionCount;
    NVGspWprU8  flags;
    NVGspWprU16 pagingConfig;
};

struct NVGspWprColdBootInput {
    NVGspWprSysmemPlan sysmem;
    NVGspGa107RuntimeFacts facts;
    NVGspWprColdBootOptions options;
};

struct NVGspWprColdBootPlan {
    NVGspGa107WprPlan ga107;
    NVGspWprImageInput imageInput;
    NVGspWprMetaV1 metadata;
    bool valid;
};

static inline bool nvGspWprColdBootSucceeded(NVGspWprU32 verdict)
{
    return verdict == NV_GSP_WPR_COLD_BOOT_OK;
}

static inline const char *nvGspWprColdBootVerdictName(NVGspWprU32 verdict)
{
    switch (verdict) {
        case NV_GSP_WPR_COLD_BOOT_NOT_ATTEMPTED:  return "not-attempted";
        case NV_GSP_WPR_COLD_BOOT_OK:             return "ok";
        case NV_GSP_WPR_COLD_BOOT_NULL_INPUT:     return "null-input";
        case NV_GSP_WPR_COLD_BOOT_NULL_OUTPUT:    return "null-output";
        case NV_GSP_WPR_COLD_BOOT_BAD_SYSMEM:     return "bad-sysmem";
        case NV_GSP_WPR_COLD_BOOT_BAD_FACTS:      return "bad-facts";
        case NV_GSP_WPR_COLD_BOOT_POLICY_REFUSED: return "policy-refused";
        case NV_GSP_WPR_COLD_BOOT_META_REFUSED:   return "meta-refused";
        default:                                   return "unknown";
    }
}

// Compose already-validated staging and observation results into the exact V1
// metadata image Booter would consume. Every refusal is atomic: *out changes
// only after both the GA107 placement policy and the V1 metadata builder accept.
static inline NVGspWprU32 nvGspComposeGa107ColdBootWpr(
    const NVGspWprColdBootInput *in,
    NVGspWprColdBootPlan *out)
{
    if (!in)
        return NV_GSP_WPR_COLD_BOOT_NULL_INPUT;
    if (!out)
        return NV_GSP_WPR_COLD_BOOT_NULL_OUTPUT;

    if (!in->sysmem.valid ||
        !in->sysmem.radix3RootGpuAddress || !in->sysmem.radix3ImageBytes ||
        !in->sysmem.bootGpuAddress || !in->sysmem.bootImageBytes)
        return NV_GSP_WPR_COLD_BOOT_BAD_SYSMEM;

    if (!in->facts.fbSizeBytes ||
        !in->facts.vgaWorkspacePolicyOffset ||
        in->facts.vgaWorkspacePolicyOffset > in->facts.fbSizeBytes)
        return NV_GSP_WPR_COLD_BOOT_BAD_FACTS;

    NVGspWprColdBootPlan staged = {};

    NVGspGa107WprInput policy = {};
    policy.fbSize = in->facts.fbSizeBytes;
    policy.vgaWorkspaceOffset = in->facts.vgaWorkspacePolicyOffset;
    policy.mmuLockValid = in->facts.mmuLockValid;
    policy.mmuLockLo = in->facts.mmuLockLo;
    policy.bootloaderSize = in->sysmem.bootImageBytes;
    policy.radix3ElfSize = in->sysmem.radix3ImageBytes;

    const NVGspWprU32 policyVerdict =
        nvGspPlanGa107ColdBootWpr(&policy, &staged.ga107);
    if (!nvGspGa107WprPolicySucceeded(policyVerdict))
        return NV_GSP_WPR_COLD_BOOT_POLICY_REFUSED;

    NVGspWprImageInput image = {};
    image.layout = staged.ga107.layout;
    image.framebufferBytes = in->facts.fbSizeBytes;

    image.radix3ElfGpuAddress = in->sysmem.radix3RootGpuAddress;
    image.radix3ElfBytes = in->sysmem.radix3ImageBytes;

    image.bootloaderGpuAddress = in->sysmem.bootGpuAddress;
    image.bootloaderBytes = in->sysmem.bootImageBytes;
    image.bootloaderCodeOffset = in->sysmem.bootCodeOffset;
    image.bootloaderDataOffset = in->sysmem.bootDataOffset;
    image.bootloaderManifestOffset = in->sysmem.bootManifestOffset;

    image.signatureGpuAddress = in->options.signatureGpuAddress;
    image.signatureBytes = in->options.signatureBytes;

    image.vgaWorkspaceOffset = in->facts.vgaWorkspacePolicyOffset;
    image.vgaWorkspaceBytes =
        in->facts.fbSizeBytes - in->facts.vgaWorkspacePolicyOffset;

    image.crashQueueGpuAddress = in->options.crashQueueGpuAddress;
    image.crashQueueBytes = in->options.crashQueueBytes;
    image.vfPartitionCount = in->options.vfPartitionCount;
    image.flags = in->options.flags;
    image.pagingConfig = in->options.pagingConfig;

    const NVGspWprU32 imageVerdict =
        nvGspBuildColdBootWprMeta(&image, &staged.metadata);
    if (!nvGspWprImageSucceeded(imageVerdict))
        return NV_GSP_WPR_COLD_BOOT_META_REFUSED;

    staged.imageInput = image;
    staged.valid = true;
    *out = staged;
    return NV_GSP_WPR_COLD_BOOT_OK;
}

#endif // NV_GSP_WPR_COLD_BOOT_COMPOSE_HPP
