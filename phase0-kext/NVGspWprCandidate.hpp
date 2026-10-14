// SPDX-License-Identifier: MIT
// Pure composition of observed GA107 runtime facts + staged sysmem firmware
// into the exact cold-boot GspFwWprMetaV1 candidate. No IOKit or hardware I/O.
#ifndef NV_GSP_WPR_CANDIDATE_HPP
#define NV_GSP_WPR_CANDIDATE_HPP

#include "NVGspWprGa107Facts.hpp"
#include "NVGspWprGa107.hpp"
#include "NVGspWprSysmem.hpp"
#include "NVGspWprImage.hpp"

#define NV_GSP_WPR_CANDIDATE_NOT_ATTEMPTED  0u
#define NV_GSP_WPR_CANDIDATE_OK             1u
#define NV_GSP_WPR_CANDIDATE_NULL_INPUT     2u
#define NV_GSP_WPR_CANDIDATE_NULL_OUTPUT    3u
#define NV_GSP_WPR_CANDIDATE_FACTS_MISSING  4u
#define NV_GSP_WPR_CANDIDATE_SYSMEM_MISSING 5u
#define NV_GSP_WPR_CANDIDATE_BAD_FACTS      6u
#define NV_GSP_WPR_CANDIDATE_BAD_SYSMEM     7u
#define NV_GSP_WPR_CANDIDATE_POLICY_REFUSED 8u
#define NV_GSP_WPR_CANDIDATE_IMAGE_REFUSED  9u
#define NV_GSP_WPR_CANDIDATE_LIMIT          10u

struct NVGspWprCandidateInput {
    bool factsPrepared;
    NVGspGa107RuntimeFacts facts;

    bool sysmemPrepared;
    NVGspWprSysmemPlan sysmem;

    // Optional open-RM fields. Address + size must appear as a pair; the
    // underlying cold-boot image builder enforces that contract.
    NVGspWprU64 signatureGpuAddress;
    NVGspWprU64 signatureBytes;
    NVGspWprU64 crashQueueGpuAddress;
    NVGspWprU64 crashQueueBytes;

    NVGspWprU8  vfPartitionCount;
    NVGspWprU8  flags;
    NVGspWprU16 pagingConfig;
};

struct NVGspWprCandidatePlan {
    NVGspGa107WprPlan ga107;
    NVGspWprMetaV1 metadata;
    bool valid;
};

static inline bool nvGspWprCandidateSucceeded(NVGspWprU32 verdict)
{
    return verdict == NV_GSP_WPR_CANDIDATE_OK;
}

static inline const char *nvGspWprCandidateVerdictName(NVGspWprU32 verdict)
{
    switch (verdict) {
        case NV_GSP_WPR_CANDIDATE_NOT_ATTEMPTED:  return "not-attempted";
        case NV_GSP_WPR_CANDIDATE_OK:             return "ok";
        case NV_GSP_WPR_CANDIDATE_NULL_INPUT:     return "null-input";
        case NV_GSP_WPR_CANDIDATE_NULL_OUTPUT:    return "null-output";
        case NV_GSP_WPR_CANDIDATE_FACTS_MISSING:  return "facts-missing";
        case NV_GSP_WPR_CANDIDATE_SYSMEM_MISSING: return "sysmem-missing";
        case NV_GSP_WPR_CANDIDATE_BAD_FACTS:      return "bad-facts";
        case NV_GSP_WPR_CANDIDATE_BAD_SYSMEM:     return "bad-sysmem";
        case NV_GSP_WPR_CANDIDATE_POLICY_REFUSED: return "policy-refused";
        case NV_GSP_WPR_CANDIDATE_IMAGE_REFUSED:  return "image-refused";
        default:                                   return "unknown";
    }
}

// Compose only already-proven inputs. This helper intentionally does not decode
// BAR0 registers and does not reconstruct stage-service DMA facts. Those are
// separate contracts with separate verdicts. Refusals are atomic: *out changes
// only after policy + exact 256-byte metadata construction both succeed.
static inline NVGspWprU32 nvGspPlanGa107WprCandidate(
    const NVGspWprCandidateInput *in,
    NVGspWprCandidatePlan *out)
{
    if (!in)
        return NV_GSP_WPR_CANDIDATE_NULL_INPUT;
    if (!out)
        return NV_GSP_WPR_CANDIDATE_NULL_OUTPUT;
    if (!in->factsPrepared)
        return NV_GSP_WPR_CANDIDATE_FACTS_MISSING;
    if (!in->sysmemPrepared)
        return NV_GSP_WPR_CANDIDATE_SYSMEM_MISSING;

    const NVGspGa107RuntimeFacts &facts = in->facts;
    const NVGspWprSysmemPlan &sysmem = in->sysmem;

    if (!facts.fbSizeBytes || !facts.vgaWorkspacePolicyOffset ||
        facts.vgaWorkspacePolicyOffset > facts.fbSizeBytes ||
        (facts.mmuLockValid && (!facts.mmuLockLo || facts.mmuLockLo > facts.fbSizeBytes)))
        return NV_GSP_WPR_CANDIDATE_BAD_FACTS;

    if (!sysmem.valid || !sysmem.radix3RootGpuAddress || !sysmem.radix3ImageBytes ||
        !sysmem.bootGpuAddress || !sysmem.bootImageBytes ||
        (sysmem.radix3RootGpuAddress & (NV_GSP_WPR_SYSMEM_PAGE_BYTES - 1ull)) != 0ull ||
        (sysmem.bootGpuAddress & (NV_GSP_WPR_SYSMEM_PAGE_BYTES - 1ull)) != 0ull ||
        sysmem.bootCodeOffset >= sysmem.bootImageBytes ||
        sysmem.bootDataOffset >= sysmem.bootImageBytes ||
        sysmem.bootManifestOffset >= sysmem.bootImageBytes)
        return NV_GSP_WPR_CANDIDATE_BAD_SYSMEM;

    NVGspWprCandidatePlan staged = {};

    NVGspGa107WprInput policyInput = {};
    policyInput.fbSize = facts.fbSizeBytes;
    policyInput.vgaWorkspaceOffset = facts.vgaWorkspacePolicyOffset;
    policyInput.mmuLockValid = facts.mmuLockValid;
    policyInput.mmuLockLo = facts.mmuLockLo;
    policyInput.bootloaderSize = sysmem.bootImageBytes;
    policyInput.radix3ElfSize = sysmem.radix3ImageBytes;

    const NVGspWprU32 policyVerdict =
        nvGspPlanGa107ColdBootWpr(&policyInput, &staged.ga107);
    if (!nvGspGa107WprPolicySucceeded(policyVerdict))
        return NV_GSP_WPR_CANDIDATE_POLICY_REFUSED;

    NVGspWprImageInput image = {};
    image.layout = staged.ga107.layout;
    image.framebufferBytes = facts.fbSizeBytes;
    image.radix3ElfGpuAddress = sysmem.radix3RootGpuAddress;
    image.radix3ElfBytes = sysmem.radix3ImageBytes;
    image.bootloaderGpuAddress = sysmem.bootGpuAddress;
    image.bootloaderBytes = sysmem.bootImageBytes;
    image.bootloaderCodeOffset = sysmem.bootCodeOffset;
    image.bootloaderDataOffset = sysmem.bootDataOffset;
    image.bootloaderManifestOffset = sysmem.bootManifestOffset;
    image.signatureGpuAddress = in->signatureGpuAddress;
    image.signatureBytes = in->signatureBytes;
    image.vgaWorkspaceOffset = facts.vgaWorkspacePolicyOffset;
    image.vgaWorkspaceBytes = facts.fbSizeBytes - facts.vgaWorkspacePolicyOffset;
    image.crashQueueGpuAddress = in->crashQueueGpuAddress;
    image.crashQueueBytes = in->crashQueueBytes;
    image.vfPartitionCount = in->vfPartitionCount;
    image.flags = in->flags;
    image.pagingConfig = in->pagingConfig;

    const NVGspWprU32 imageVerdict =
        nvGspBuildColdBootWprMeta(&image, &staged.metadata);
    if (!nvGspWprImageSucceeded(imageVerdict))
        return NV_GSP_WPR_CANDIDATE_IMAGE_REFUSED;

    // Cross-check the fields copied from independently proven runtime/staging
    // contracts. This is deliberately redundant with the image builder: it
    // catches accidental rewiring in this composer itself.
    if (staged.metadata.sysmemAddrOfRadix3Elf != sysmem.radix3RootGpuAddress ||
        staged.metadata.sizeOfRadix3Elf != sysmem.radix3ImageBytes ||
        staged.metadata.sysmemAddrOfBootloader != sysmem.bootGpuAddress ||
        staged.metadata.sizeOfBootloader != sysmem.bootImageBytes ||
        staged.metadata.bootloaderCodeOffset != sysmem.bootCodeOffset ||
        staged.metadata.bootloaderDataOffset != sysmem.bootDataOffset ||
        staged.metadata.bootloaderManifestOffset != sysmem.bootManifestOffset ||
        staged.metadata.fbSize != facts.fbSizeBytes ||
        staged.metadata.vgaWorkspaceOffset != facts.vgaWorkspacePolicyOffset ||
        staged.metadata.vgaWorkspaceSize != facts.fbSizeBytes - facts.vgaWorkspacePolicyOffset ||
        staged.metadata.verified != 0ull)
        return NV_GSP_WPR_CANDIDATE_IMAGE_REFUSED;

    staged.valid = true;
    *out = staged;
    return NV_GSP_WPR_CANDIDATE_OK;
}

#endif // NV_GSP_WPR_CANDIDATE_HPP
