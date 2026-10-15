// SPDX-License-Identifier: MIT
// Pure GA107 SEC2 Booter ucode-fuse observation helpers.
//
// Source facts are pinned to NVIDIA/open-gpu-kernel-modules:
//   kernel_sec2_ga100.c:ksec2ReadUcodeFuseVersion_GA100()
//   generated g_kernel_sec2_nvoc.c GA107 HAL dispatch
//   published ga100/dev_fuse.h
//
// GA107 dispatches the GA100 SEC2 fuse reader. Ucode IDs are 1-indexed and
// select NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION + 4*(ucodeId-1). NVIDIA turns the
// returned fuse bitmask into a version by taking highest_set_bit + 1; zero is
// version zero. This header performs no I/O and contains no write primitive.
#ifndef NV_GSP_BOOTER_FUSE_HPP
#define NV_GSP_BOOTER_FUSE_HPP

#include "NVDecode.hpp"

#define NV_GSP_GA107_PCI_VENDOR_ID                    0x10deu
#define NV_GSP_GA107_PCI_DEVICE_ID                    0x25a2u

#define NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG      0x00824140u
#define NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE        0x00000004u
#define NV_GSP_GA107_SEC2_UCODE_FIRST_ID              1u
#define NV_GSP_GA107_SEC2_UCODE_COUNT                 16u
#define NV_GSP_GA107_SEC2_UCODE_LAST_ID               16u

// Both committed GA107 570.144 Booter HS containers carry ucodeId == 3.
// Runtime firmware consumers must still use the parsed metadata rather than
// assuming every future container keeps this ID.
#define NV_GSP_GA107_BOOTER_HS_UCODE_ID               3u

#define NV_GSP_BOOTER_FUSE_NOT_ATTEMPTED              0u
#define NV_GSP_BOOTER_FUSE_OK                         1u
#define NV_GSP_BOOTER_FUSE_NULL_OUTPUT                2u
#define NV_GSP_BOOTER_FUSE_WRONG_PCI                  3u
#define NV_GSP_BOOTER_FUSE_DEAD_IDENTITY              4u
#define NV_GSP_BOOTER_FUSE_WRONG_SILICON              5u
#define NV_GSP_BOOTER_FUSE_BAD_UCODE_ID               6u
#define NV_GSP_BOOTER_FUSE_BAD_PLAN                   7u
#define NV_GSP_BOOTER_FUSE_DEAD_READ                  8u

struct NVGspBooterFuseReadPlan {
    NVU32 ucodeId;
    NVU32 registerOffset;
    bool valid;
};

struct NVGspBooterFuseObservation {
    NVU32 ucodeId;
    NVU32 registerOffset;
    NVU32 rawFuse;
    NVU32 fuseVersion;
    bool valid;
};

static inline const char *nvGspBooterFuseVerdictName(NVU32 verdict)
{
    switch (verdict) {
        case NV_GSP_BOOTER_FUSE_NOT_ATTEMPTED: return "not-attempted";
        case NV_GSP_BOOTER_FUSE_OK:            return "ok";
        case NV_GSP_BOOTER_FUSE_NULL_OUTPUT:   return "null-output";
        case NV_GSP_BOOTER_FUSE_WRONG_PCI:     return "wrong-pci";
        case NV_GSP_BOOTER_FUSE_DEAD_IDENTITY: return "dead-identity";
        case NV_GSP_BOOTER_FUSE_WRONG_SILICON: return "wrong-silicon";
        case NV_GSP_BOOTER_FUSE_BAD_UCODE_ID:  return "bad-ucode-id";
        case NV_GSP_BOOTER_FUSE_BAD_PLAN:      return "bad-plan";
        case NV_GSP_BOOTER_FUSE_DEAD_READ:     return "dead-read";
        default:                                return "unknown";
    }
}

static inline bool nvGspBooterFuseSucceeded(NVU32 verdict)
{
    return verdict == NV_GSP_BOOTER_FUSE_OK;
}

static inline bool nvGspIsExactGa107Target(NVU32 pciVendor,
                                            NVU32 pciDevice,
                                            NVU32 boot0,
                                            NVU32 boot42)
{
    if (pciVendor != NV_GSP_GA107_PCI_VENDOR_ID ||
        pciDevice != NV_GSP_GA107_PCI_DEVICE_ID)
        return false;
    if (nvLooksLikeDeadRead(boot0) || nvLooksLikeDeadRead(boot42))
        return false;
    if (nvBoot0Architecture(boot0) != NV_ARCHITECTURE_GA100 ||
        nvBoot0Implementation(boot0) != NV_IMPLEMENTATION_GA107)
        return false;
    if (nvBoot42Architecture(boot42) != NV_ARCHITECTURE_GA100 ||
        nvBoot42Implementation(boot42) != NV_IMPLEMENTATION_GA107 ||
        nvBoot42ChipId(boot42) != NV_CHIP_ID_GA107)
        return false;
    return true;
}

// Build the only MMIO address a caller is allowed to read for this observation.
// Refusals are atomic: *out is untouched unless every target/ID check succeeds.
static inline NVU32 nvGspPlanGa107BooterFuseRead(NVU32 pciVendor,
                                                  NVU32 pciDevice,
                                                  NVU32 boot0,
                                                  NVU32 boot42,
                                                  NVU32 ucodeId,
                                                  NVGspBooterFuseReadPlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_FUSE_NULL_OUTPUT;
    if (pciVendor != NV_GSP_GA107_PCI_VENDOR_ID ||
        pciDevice != NV_GSP_GA107_PCI_DEVICE_ID)
        return NV_GSP_BOOTER_FUSE_WRONG_PCI;
    if (nvLooksLikeDeadRead(boot0) || nvLooksLikeDeadRead(boot42))
        return NV_GSP_BOOTER_FUSE_DEAD_IDENTITY;
    if (!nvGspIsExactGa107Target(pciVendor, pciDevice, boot0, boot42))
        return NV_GSP_BOOTER_FUSE_WRONG_SILICON;
    if (ucodeId < NV_GSP_GA107_SEC2_UCODE_FIRST_ID ||
        ucodeId > NV_GSP_GA107_SEC2_UCODE_LAST_ID)
        return NV_GSP_BOOTER_FUSE_BAD_UCODE_ID;

    NVGspBooterFuseReadPlan staged = {};
    staged.ucodeId = ucodeId;
    staged.registerOffset = NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG +
        (ucodeId - NV_GSP_GA107_SEC2_UCODE_FIRST_ID) *
        NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE;
    staged.valid = true;
    *out = staged;
    return NV_GSP_BOOTER_FUSE_OK;
}

static inline bool nvGspBooterFusePlanIsCanonical(
    const NVGspBooterFuseReadPlan *plan)
{
    if (!plan || !plan->valid ||
        plan->ucodeId < NV_GSP_GA107_SEC2_UCODE_FIRST_ID ||
        plan->ucodeId > NV_GSP_GA107_SEC2_UCODE_LAST_ID)
        return false;

    const NVU32 expected = NV_GSP_GA107_SEC2_UCODE_VERSION_BASE_REG +
        (plan->ucodeId - NV_GSP_GA107_SEC2_UCODE_FIRST_ID) *
        NV_GSP_GA107_SEC2_UCODE_VERSION_STRIDE;
    return plan->registerOffset == expected;
}

// NVIDIA's Ampere HAL uses the complete register value as a bitmask and returns
// highest_set_bit + 1. Do not mask to 16 bits here: mirroring the open RM is
// more useful than silently inventing a different policy for reserved bits.
static inline NVU32 nvGspBooterFuseVersionFromMask(NVU32 rawFuse)
{
    NVU32 version = 0u;
    while (rawFuse != 0u) {
        ++version;
        rawFuse >>= 1;
    }
    return version;
}

// Decode one already-read fuse word. 0xffffffff is this driver's BAR refusal
// sentinel and must not become the very plausible-looking version 32. Zero is
// different: NVIDIA explicitly maps a zero fuse bitmask to version zero.
// Refusals are atomic: *out is touched only on success.
static inline NVU32 nvGspDecodeGa107BooterFuse(
    const NVGspBooterFuseReadPlan *plan,
    NVU32 rawFuse,
    NVGspBooterFuseObservation *out)
{
    if (!out)
        return NV_GSP_BOOTER_FUSE_NULL_OUTPUT;
    if (!nvGspBooterFusePlanIsCanonical(plan))
        return NV_GSP_BOOTER_FUSE_BAD_PLAN;
    if (rawFuse == 0xffffffffu)
        return NV_GSP_BOOTER_FUSE_DEAD_READ;

    NVGspBooterFuseObservation staged = {};
    staged.ucodeId = plan->ucodeId;
    staged.registerOffset = plan->registerOffset;
    staged.rawFuse = rawFuse;
    staged.fuseVersion = nvGspBooterFuseVersionFromMask(rawFuse);
    staged.valid = true;
    *out = staged;
    return NV_GSP_BOOTER_FUSE_OK;
}

#endif // NV_GSP_BOOTER_FUSE_HPP
