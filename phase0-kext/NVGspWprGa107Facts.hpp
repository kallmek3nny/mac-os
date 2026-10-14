// SPDX-License-Identifier: MIT
// Pure GA107 WPR runtime-fact decoder. No IOKit, allocation, or hardware I/O.
#ifndef NV_GSP_WPR_GA107_FACTS_HPP
#define NV_GSP_WPR_GA107_FACTS_HPP

typedef unsigned int       NVGspGa107FactU32;
typedef unsigned long long NVGspGa107FactU64;

#define NV_GSP_GA107_FACTS_NOT_ATTEMPTED          0u
#define NV_GSP_GA107_FACTS_OK                     1u
#define NV_GSP_GA107_FACTS_NULL_INPUT             2u
#define NV_GSP_GA107_FACTS_NULL_OUTPUT            3u
#define NV_GSP_GA107_FACTS_DEAD_READ              4u
#define NV_GSP_GA107_FACTS_BAD_FRAMEBUFFER        5u
#define NV_GSP_GA107_FACTS_BAD_WORKSPACE          6u
#define NV_GSP_GA107_FACTS_LIMIT                  7u

// NVIDIA open RM, GA102/GA107 source-backed BAR0 registers.
//
// kmemsysReadUsableFbSize_GA102(): VBIOS devinit publishes usable FB in MiB
// through secure scratch group 42.
#define NV_GSP_GA107_USABLE_FB_MB_REG              0x001183a4u

// gpuFuseSupportsDisplay_GA100(), used by GA102..GA107: bit 0 == 0 means
// display enabled, bit 0 == 1 means display fused off.
#define NV_GSP_GA107_DISPLAY_FUSE_REG              0x00820c04u
#define NV_GSP_GA107_DISPLAY_FUSE_DISABLE_BIT      0x00000001u

// kdispGetVgaWorkspaceBase_v04_00(), used by GA102..GA107.
#define NV_GSP_GA107_VGA_WORKSPACE_REG             0x00625f04u
#define NV_GSP_GA107_VGA_WORKSPACE_VALID_BIT       0x00000008u
#define NV_GSP_GA107_VGA_WORKSPACE_ADDR_MASK       0xffffff00u

// kgspPopulateWprMeta_TU102() policy inherited by GA107.
#define NV_GSP_GA107_PRAMIN_BYTES                  0x00100000ull
#define NV_GSP_GA107_VBIOS_WORKSPACE_BYTES         0x00020000ull
#define NV_GSP_GA107_MIB_BYTES_FACTS               0x00100000ull

struct NVGspGa107ObservedRegs {
    NVGspGa107FactU32 usableFbMb;
    NVGspGa107FactU32 displayFuse;
    NVGspGa107FactU32 vgaWorkspace;
};

struct NVGspGa107RuntimeFacts {
    NVGspGa107FactU64 fbSizeBytes;
    bool displaySupported;
    bool vgaWorkspaceRegisterValid;
    NVGspGa107FactU64 vgaWorkspaceRegisterOffset;
    NVGspGa107FactU64 vgaWorkspacePolicyOffset;

    // GA107 is wired to open RM's stub memmgrReadMmuLock implementation, which
    // returns success with valid=false. Do not invent a GA107 MMU-lock read.
    bool mmuLockValid;
    NVGspGa107FactU64 mmuLockLo;
};

static inline bool nvGspGa107FactsSucceeded(NVGspGa107FactU32 verdict)
{
    return verdict == NV_GSP_GA107_FACTS_OK;
}

static inline const char *nvGspGa107FactsVerdictName(NVGspGa107FactU32 verdict)
{
    switch (verdict) {
        case NV_GSP_GA107_FACTS_NOT_ATTEMPTED:   return "not-attempted";
        case NV_GSP_GA107_FACTS_OK:              return "ok";
        case NV_GSP_GA107_FACTS_NULL_INPUT:      return "null-input";
        case NV_GSP_GA107_FACTS_NULL_OUTPUT:     return "null-output";
        case NV_GSP_GA107_FACTS_DEAD_READ:       return "dead-read";
        case NV_GSP_GA107_FACTS_BAD_FRAMEBUFFER: return "bad-framebuffer";
        case NV_GSP_GA107_FACTS_BAD_WORKSPACE:   return "bad-workspace";
        default:                                  return "unknown";
    }
}

static inline NVGspGa107FactU64 nvGspGa107DecodeVgaWorkspaceOffset(
    NVGspGa107FactU32 reg)
{
    // Published field is ADDR[31:8], then open RM shifts the field left by 16.
    return ((NVGspGa107FactU64)(reg & NV_GSP_GA107_VGA_WORKSPACE_ADDR_MASK)) << 8;
}

// Reproduce the GA107 facts consumed by kgspPopulateWprMeta_TU102().
// Refusals are atomic: *out is touched only after every validation succeeds.
static inline NVGspGa107FactU32 nvGspDecodeGa107WprFacts(
    const NVGspGa107ObservedRegs *in,
    NVGspGa107RuntimeFacts *out)
{
    if (!in)
        return NV_GSP_GA107_FACTS_NULL_INPUT;
    if (!out)
        return NV_GSP_GA107_FACTS_NULL_OUTPUT;

    // On the exact GA107 target, a 0xffffffff value in the usable-FB scratch
    // register would claim roughly 4 PiB of local VRAM and is therefore a dead
    // aperture read, not a plausible VBIOS publication. The other two reads
    // use the same refusal so a powered-down BAR cannot be mistaken for facts.
    if (in->usableFbMb == 0xffffffffu ||
        in->displayFuse == 0xffffffffu ||
        in->vgaWorkspace == 0xffffffffu)
        return NV_GSP_GA107_FACTS_DEAD_READ;

    if (!in->usableFbMb)
        return NV_GSP_GA107_FACTS_BAD_FRAMEBUFFER;

    const NVGspGa107FactU64 fbSizeBytes =
        ((NVGspGa107FactU64)in->usableFbMb) << 20;
    if (fbSizeBytes < NV_GSP_GA107_PRAMIN_BYTES)
        return NV_GSP_GA107_FACTS_BAD_FRAMEBUFFER;

    NVGspGa107RuntimeFacts staged = {};
    staged.fbSizeBytes = fbSizeBytes;
    staged.displaySupported =
        (in->displayFuse & NV_GSP_GA107_DISPLAY_FUSE_DISABLE_BIT) == 0u;
    staged.vgaWorkspaceRegisterValid =
        (in->vgaWorkspace & NV_GSP_GA107_VGA_WORKSPACE_VALID_BIT) != 0u;
    staged.vgaWorkspaceRegisterOffset =
        nvGspGa107DecodeVgaWorkspaceOffset(in->vgaWorkspace);

    const NVGspGa107FactU64 praminFallback =
        fbSizeBytes - NV_GSP_GA107_PRAMIN_BYTES;

    if (staged.displaySupported && staged.vgaWorkspaceRegisterValid) {
        if (staged.vgaWorkspaceRegisterOffset > fbSizeBytes)
            return NV_GSP_GA107_FACTS_BAD_WORKSPACE;

        if (staged.vgaWorkspaceRegisterOffset < praminFallback) {
            if (fbSizeBytes < NV_GSP_GA107_VBIOS_WORKSPACE_BYTES)
                return NV_GSP_GA107_FACTS_BAD_FRAMEBUFFER;
            staged.vgaWorkspacePolicyOffset =
                fbSizeBytes - NV_GSP_GA107_VBIOS_WORKSPACE_BYTES;
        }
        else {
            staged.vgaWorkspacePolicyOffset =
                staged.vgaWorkspaceRegisterOffset;
        }
    }
    else {
        staged.vgaWorkspacePolicyOffset = praminFallback;
    }

    // Generated GA107 HAL wiring selects memmgrReadMmuLock_ccda6f(), whose
    // complete behavior is `*pbIsValid = NV_FALSE; return NV_OK;`.
    staged.mmuLockValid = false;
    staged.mmuLockLo = 0ull;

    *out = staged;
    return NV_GSP_GA107_FACTS_OK;
}

#endif // NV_GSP_WPR_GA107_FACTS_HPP
