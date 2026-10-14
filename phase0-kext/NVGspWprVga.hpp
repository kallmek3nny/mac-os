// SPDX-License-Identifier: MIT
// Pure GA107 VGA-workspace / VBIOS-reservation policy for cold-boot WPR metadata.
#ifndef NV_GSP_WPR_VGA_HPP
#define NV_GSP_WPR_VGA_HPP

typedef unsigned long long NVGspWprVgaU64;
typedef unsigned int       NVGspWprVgaU32;

#define NV_GSP_WPR_VGA_NOT_ATTEMPTED       0u
#define NV_GSP_WPR_VGA_OK                  1u
#define NV_GSP_WPR_VGA_NULL_OUTPUT         2u
#define NV_GSP_WPR_VGA_BAD_FRAMEBUFFER     3u
#define NV_GSP_WPR_VGA_DEAD_FUSE_READ      4u
#define NV_GSP_WPR_VGA_DEAD_WORKSPACE_READ 5u
#define NV_GSP_WPR_VGA_BAD_WORKSPACE       6u
#define NV_GSP_WPR_VGA_LIMIT               7u

// NVIDIA open RM sources:
// - GA102/GA107 dispatch uses gpuFuseSupportsDisplay_GA100().
// - ga100/dev_fuse.h: STATUS_OPT_DISPLAY = 0x00820c04, DATA bit 0,
//   DATA_ENABLE = 0. Therefore bit 0 clear means display is supported.
// - disp/v04_00/dev_disp.h: VGA_WORKSPACE_BASE = 0x00625f04,
//   STATUS bit 3, ADDR bits 31:8.
// - kdispGetVgaWorkspaceBase_v04_00(): offset = ADDR << 16.
// - kgspPopulateWprMeta_TU102(): invalid/unsupported display falls back to
//   fbSize - DRF_SIZE(NV_PRAMIN). The BAR0 PRAMIN window is the last 1 MiB of
//   FB. A valid workspace below that window is relocated to the last 128 KiB.
// - GA107's memmgrReadMmuLock HAL is the no-op implementation which sets
//   bIsValid=false, so vbiosReservedOffset == vgaWorkspaceOffset on GA107.
#define NV_GSP_GA107_DISPLAY_FUSE_REGISTER            0x00820c04u
#define NV_GSP_GA107_DISPLAY_FUSE_DATA_MASK           0x00000001u
#define NV_GSP_GA107_VGA_WORKSPACE_REGISTER           0x00625f04u
#define NV_GSP_GA107_VGA_WORKSPACE_STATUS_VALID_MASK  0x00000008u
#define NV_GSP_GA107_VGA_WORKSPACE_ADDR_MASK          0xffffff00u
#define NV_GSP_GA107_VGA_WORKSPACE_ADDR_SHIFT         8u
#define NV_GSP_GA107_VGA_WORKSPACE_UNIT_SHIFT         16u
#define NV_GSP_GA107_PRAMIN_BYTES                     0x00100000ull
#define NV_GSP_GA107_RELOCATED_WORKSPACE_BYTES        0x00020000ull

struct NVGspGa107VgaPlan {
    NVGspWprVgaU32 fuseRaw;
    NVGspWprVgaU32 workspaceRaw;
    bool displaySupported;
    bool workspaceRegisterValid;
    bool usedPraminFallback;
    bool relocatedToTail;
    NVGspWprVgaU64 decodedWorkspaceOffset;
    NVGspWprVgaU64 workspaceOffset;
    NVGspWprVgaU64 workspaceBytes;
    NVGspWprVgaU64 vbiosReservedOffset;
    bool valid;
};

static inline bool nvGspGa107VgaSucceeded(NVGspWprVgaU32 verdict)
{
    return verdict == NV_GSP_WPR_VGA_OK;
}

static inline const char *nvGspGa107VgaVerdictName(NVGspWprVgaU32 verdict)
{
    switch (verdict) {
        case NV_GSP_WPR_VGA_NOT_ATTEMPTED:       return "not-attempted";
        case NV_GSP_WPR_VGA_OK:                  return "ok";
        case NV_GSP_WPR_VGA_NULL_OUTPUT:         return "null-output";
        case NV_GSP_WPR_VGA_BAD_FRAMEBUFFER:     return "bad-framebuffer";
        case NV_GSP_WPR_VGA_DEAD_FUSE_READ:      return "dead-fuse-read";
        case NV_GSP_WPR_VGA_DEAD_WORKSPACE_READ: return "dead-workspace-read";
        case NV_GSP_WPR_VGA_BAD_WORKSPACE:       return "bad-workspace";
        default:                                  return "unknown";
    }
}

static inline bool nvGspGa107DisplaySupported(NVGspWprVgaU32 fuseRaw)
{
    return (fuseRaw & NV_GSP_GA107_DISPLAY_FUSE_DATA_MASK) == 0u;
}

static inline bool nvGspGa107WorkspaceRegisterValid(NVGspWprVgaU32 workspaceRaw)
{
    return (workspaceRaw & NV_GSP_GA107_VGA_WORKSPACE_STATUS_VALID_MASK) != 0u;
}

static inline NVGspWprVgaU64 nvGspGa107DecodeWorkspaceOffset(NVGspWprVgaU32 workspaceRaw)
{
    const NVGspWprVgaU64 field =
        (NVGspWprVgaU64)((workspaceRaw & NV_GSP_GA107_VGA_WORKSPACE_ADDR_MASK) >>
                         NV_GSP_GA107_VGA_WORKSPACE_ADDR_SHIFT);
    return field << NV_GSP_GA107_VGA_WORKSPACE_UNIT_SHIFT;
}

// Reproduce the GA107/TU102 WPR workspace decision from read-only register
// observations. `workspaceRaw` is ignored when the display fuse says the GPU is
// displayless, exactly as open RM avoids calling kdispGetVgaWorkspaceBase() in
// that case.
//
// The function is transactional: every refusal leaves *out untouched.
static inline NVGspWprVgaU32 nvGspPlanGa107VgaWorkspace(
    NVGspWprVgaU64 framebufferBytes,
    NVGspWprVgaU32 fuseRaw,
    NVGspWprVgaU32 workspaceRaw,
    NVGspGa107VgaPlan *out)
{
    if (!out)
        return NV_GSP_WPR_VGA_NULL_OUTPUT;

    if (framebufferBytes < NV_GSP_GA107_PRAMIN_BYTES)
        return NV_GSP_WPR_VGA_BAD_FRAMEBUFFER;

    // All-ones is this driver's established dead/unmapped MMIO sentinel. It is
    // especially dangerous here because bit 0 would otherwise claim
    // displayless and silently select the fallback path.
    if (fuseRaw == 0xffffffffu)
        return NV_GSP_WPR_VGA_DEAD_FUSE_READ;

    NVGspGa107VgaPlan staged = {};
    staged.fuseRaw = fuseRaw;
    staged.workspaceRaw = workspaceRaw;
    staged.displaySupported = nvGspGa107DisplaySupported(fuseRaw);

    if (!staged.displaySupported) {
        staged.usedPraminFallback = true;
        staged.workspaceOffset = framebufferBytes - NV_GSP_GA107_PRAMIN_BYTES;
    }
    else {
        // Only consult the display register on the same branch open RM does.
        if (workspaceRaw == 0xffffffffu)
            return NV_GSP_WPR_VGA_DEAD_WORKSPACE_READ;

        staged.workspaceRegisterValid = nvGspGa107WorkspaceRegisterValid(workspaceRaw);
        if (!staged.workspaceRegisterValid) {
            staged.usedPraminFallback = true;
            staged.workspaceOffset = framebufferBytes - NV_GSP_GA107_PRAMIN_BYTES;
        }
        else {
            staged.decodedWorkspaceOffset = nvGspGa107DecodeWorkspaceOffset(workspaceRaw);
            if (staged.decodedWorkspaceOffset >= framebufferBytes)
                return NV_GSP_WPR_VGA_BAD_WORKSPACE;

            const NVGspWprVgaU64 praminStart =
                framebufferBytes - NV_GSP_GA107_PRAMIN_BYTES;
            if (staged.decodedWorkspaceOffset < praminStart) {
                staged.relocatedToTail = true;
                staged.workspaceOffset =
                    framebufferBytes - NV_GSP_GA107_RELOCATED_WORKSPACE_BYTES;
            }
            else {
                staged.workspaceOffset = staged.decodedWorkspaceOffset;
            }
        }
    }

    if (staged.workspaceOffset >= framebufferBytes)
        return NV_GSP_WPR_VGA_BAD_WORKSPACE;

    staged.workspaceBytes = framebufferBytes - staged.workspaceOffset;

    // GA107 dispatches memmgrReadMmuLock() to the no-op HAL which reports
    // bIsValid=false, so the generic TU102 min(mmuLockLo, workspace) branch is
    // unreachable for this chip.
    staged.vbiosReservedOffset = staged.workspaceOffset;
    staged.valid = true;
    *out = staged;
    return NV_GSP_WPR_VGA_OK;
}

#endif // NV_GSP_WPR_VGA_HPP
