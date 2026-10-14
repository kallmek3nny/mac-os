// SPDX-License-Identifier: MIT
// Pure row-band planner for reusing the 4 KiB BCS0 source/destination scratch pages.
#ifndef IntelGtCopyPlan_hpp
#define IntelGtCopyPlan_hpp

using IGCopyU32 = unsigned int;
using IGCopyU64 = unsigned long long;

#define IG_GTCP_SCRATCH_BYTES_POLICY_4096       4096u
#define IG_GTCP_BYTES_PER_PIXEL_POLICY_4        4u

#define IG_GTCP_PLAN_NOT_ATTEMPTED              0u
#define IG_GTCP_PLAN_OK                         1u
#define IG_GTCP_PLAN_NULL_OUT                   2u
#define IG_GTCP_PLAN_BAD_DIMENSIONS             3u
#define IG_GTCP_PLAN_BAD_PITCH                  4u
#define IG_GTCP_PLAN_START_OUT_OF_RANGE         5u
#define IG_GTCP_PLAN_SCRATCH_TOO_SMALL          6u
#define IG_GTCP_PLAN_OVERFLOW                   7u
#define IG_GTCP_PLAN_LIMIT                      8u

struct IGGtCopy32Chunk {
    IGCopyU64 byteOffset;
    IGCopyU32 rows;
    IGCopyU32 usedBytes;
};

static inline bool igGtCopy32PlanSucceeded(IGCopyU32 verdict)
{
    return verdict == IG_GTCP_PLAN_OK;
}

static inline const char *igGtCopy32PlanVerdictName(IGCopyU32 verdict)
{
    switch (verdict) {
        case IG_GTCP_PLAN_NOT_ATTEMPTED:      return "not attempted";
        case IG_GTCP_PLAN_OK:                 return "ok";
        case IG_GTCP_PLAN_NULL_OUT:           return "null output";
        case IG_GTCP_PLAN_BAD_DIMENSIONS:     return "bad dimensions";
        case IG_GTCP_PLAN_BAD_PITCH:          return "bad pitch";
        case IG_GTCP_PLAN_START_OUT_OF_RANGE: return "start row out of range";
        case IG_GTCP_PLAN_SCRATCH_TOO_SMALL:  return "scratch page too small";
        case IG_GTCP_PLAN_OVERFLOW:           return "surface arithmetic overflow";
        default:                              return "unknown copy-plan verdict";
    }
}

static inline bool igGtCopy32SurfaceBytes(IGCopyU32 width,
                                          IGCopyU32 height,
                                          IGCopyU32 pitch,
                                          IGCopyU64 *outBytes)
{
    if (outBytes != 0) *outBytes = 0ull;
    if (!width || !height || !pitch) return false;

    const IGCopyU64 rowBytes = (IGCopyU64)width * IG_GTCP_BYTES_PER_PIXEL_POLICY_4;
    if (rowBytes > (IGCopyU64)pitch) return false;

    const IGCopyU64 preceding = (IGCopyU64)(height - 1u) * (IGCopyU64)pitch;
    const IGCopyU64 used = preceding + rowBytes;
    if (used < preceding) return false;

    if (outBytes != 0) *outBytes = used;
    return true;
}

// Plan the next row band. The scratch span is reused only after the previous
// BCS submission has retired, so every returned chunk starts at scratch offset
// zero and `byteOffset` is solely the offset in the caller's larger surface.
// Refusals are atomic: `out` is touched only on success.
static inline IGCopyU32 igGtCopy32PlanChunk(IGCopyU32 width,
                                            IGCopyU32 height,
                                            IGCopyU32 pitch,
                                            IGCopyU32 startRow,
                                            IGCopyU32 scratchBytes,
                                            IGGtCopy32Chunk *out)
{
    if (out == 0) return IG_GTCP_PLAN_NULL_OUT;
    if (!width || !height) return IG_GTCP_PLAN_BAD_DIMENSIONS;
    if (!pitch) return IG_GTCP_PLAN_BAD_PITCH;
    if (startRow >= height) return IG_GTCP_PLAN_START_OUT_OF_RANGE;

    const IGCopyU64 rowBytes = (IGCopyU64)width * IG_GTCP_BYTES_PER_PIXEL_POLICY_4;
    if (rowBytes > (IGCopyU64)pitch) return IG_GTCP_PLAN_BAD_PITCH;
    if (rowBytes > (IGCopyU64)scratchBytes) return IG_GTCP_PLAN_SCRATCH_TOO_SMALL;

    // One row always fits at this point. Every additional row consumes one full
    // pitch because its pixel bytes begin that far after the previous row.
    const IGCopyU64 extraRows = ((IGCopyU64)scratchBytes - rowBytes) / (IGCopyU64)pitch;
    IGCopyU64 rows64 = 1ull + extraRows;
    const IGCopyU64 remaining = (IGCopyU64)height - (IGCopyU64)startRow;
    if (rows64 > remaining) rows64 = remaining;
    if (!rows64 || rows64 > 0xFFFFFFFFull) return IG_GTCP_PLAN_OVERFLOW;

    const IGCopyU64 used = (rows64 - 1ull) * (IGCopyU64)pitch + rowBytes;
    const IGCopyU64 offset = (IGCopyU64)startRow * (IGCopyU64)pitch;
    if (used > (IGCopyU64)scratchBytes || used > 0xFFFFFFFFull) {
        return IG_GTCP_PLAN_OVERFLOW;
    }
    if (offset + used < offset) return IG_GTCP_PLAN_OVERFLOW;

    IGGtCopy32Chunk chunk;
    chunk.byteOffset = offset;
    chunk.rows = (IGCopyU32)rows64;
    chunk.usedBytes = (IGCopyU32)used;
    *out = chunk;
    return IG_GTCP_PLAN_OK;
}

#endif // IntelGtCopyPlan_hpp
