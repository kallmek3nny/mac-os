// SPDX-License-Identifier: MIT
// Pure parser/planner for NVIDIA's nvfw_hs_header_v2 SEC2 Booter container.
//
// Source of truth:
//   NVIDIA/open-gpu-kernel-modules, nouveau/extract-firmware-nouveau.py,
//   booter().  That extractor constructs the on-disk image as:
//     nvfw_bin_hdr
//     nvfw_hs_header_v2 (9 x u32)
//     production signatures
//     patch_loc, patch_sig
//     fuse_ver, engine_id, ucode_id
//     num_sig
//     nvfw_hs_load_header_v2
//     firmware payload
//
// This file only proves that an already-loaded byte buffer has that shape.  It
// does NOT select a signature, patch firmware, map BAR0, touch SEC2, write WPR,
// or execute Booter.
#ifndef NV_GSP_BOOTER_HS_HPP
#define NV_GSP_BOOTER_HS_HPP

#define NV_GSP_BOOTER_HS_OK                    0u
#define NV_GSP_BOOTER_HS_ERR_NULL_OUTPUT       1u
#define NV_GSP_BOOTER_HS_ERR_SHORT             2u
#define NV_GSP_BOOTER_HS_ERR_CONTAINER         3u
#define NV_GSP_BOOTER_HS_ERR_LAYOUT            4u
#define NV_GSP_BOOTER_HS_ERR_SIGNATURES        5u
#define NV_GSP_BOOTER_HS_ERR_METADATA          6u
#define NV_GSP_BOOTER_HS_ERR_LOAD_HEADER       7u
#define NV_GSP_BOOTER_HS_ERR_PATCH_RANGE       8u
#define NV_GSP_BOOTER_HS_ERR_LIMIT             9u

#define NV_GSP_BOOTER_NVFW_MAGIC               0x000010deu
#define NV_GSP_BOOTER_NVFW_VERSION             1u
#define NV_GSP_BOOTER_NVFW_HEADER_BYTES        24u
#define NV_GSP_BOOTER_HS_HEADER_BYTES          36u
#define NV_GSP_BOOTER_META_BYTES               12u
#define NV_GSP_BOOTER_ONE_APP_HEADER_BYTES     36u
#define NV_GSP_BOOTER_BLOCK_BYTES              256u
#define NV_GSP_BOOTER_MAX_SIGNATURES           15u

typedef unsigned int       NVGspBooterHsU32;
typedef unsigned long long NVGspBooterHsU64;

struct NVGspBooterHsPlan {
    NVGspBooterHsU32 paddedBytes;
    NVGspBooterHsU32 firmwareOffset;
    NVGspBooterHsU32 firmwareBytes;

    NVGspBooterHsU32 signatureOffset;
    NVGspBooterHsU32 signatureBytes;
    NVGspBooterHsU32 signatureCount;
    NVGspBooterHsU32 signatureStride;

    NVGspBooterHsU32 patchLocation;
    NVGspBooterHsU32 fuseVersion;
    NVGspBooterHsU32 engineId;
    NVGspBooterHsU32 ucodeId;

    NVGspBooterHsU32 osCodeOffset;
    NVGspBooterHsU32 osCodeBytes;
    NVGspBooterHsU32 osDataOffset;
    NVGspBooterHsU32 osDataBytes;
    NVGspBooterHsU32 appCodeOffset;
    NVGspBooterHsU32 appCodeBytes;
    NVGspBooterHsU32 appDataOffset;
    NVGspBooterHsU32 appDataBytes;
    bool valid;
};

static inline NVGspBooterHsU32 nvGspBooterHsRead32(const unsigned char *p)
{
    return (NVGspBooterHsU32)p[0]
         | ((NVGspBooterHsU32)p[1] << 8)
         | ((NVGspBooterHsU32)p[2] << 16)
         | ((NVGspBooterHsU32)p[3] << 24);
}

static inline bool nvGspBooterHsFits(NVGspBooterHsU64 len,
                                     NVGspBooterHsU64 off,
                                     NVGspBooterHsU64 bytes)
{
    return off <= len && bytes <= (len - off);
}

static inline bool nvGspBooterHsRegionFits(NVGspBooterHsU32 payloadBytes,
                                           NVGspBooterHsU32 off,
                                           NVGspBooterHsU32 bytes)
{
    return (NVGspBooterHsU64)off <= payloadBytes &&
           (NVGspBooterHsU64)bytes <=
               ((NVGspBooterHsU64)payloadBytes - off);
}

static inline bool nvGspBooterHsSucceeded(NVGspBooterHsU32 verdict)
{
    return verdict == NV_GSP_BOOTER_HS_OK;
}

static inline const char *nvGspBooterHsVerdictName(NVGspBooterHsU32 verdict)
{
    switch (verdict) {
        case NV_GSP_BOOTER_HS_OK:              return "ok";
        case NV_GSP_BOOTER_HS_ERR_NULL_OUTPUT: return "null-output";
        case NV_GSP_BOOTER_HS_ERR_SHORT:       return "short";
        case NV_GSP_BOOTER_HS_ERR_CONTAINER:   return "container";
        case NV_GSP_BOOTER_HS_ERR_LAYOUT:      return "layout";
        case NV_GSP_BOOTER_HS_ERR_SIGNATURES:  return "signatures";
        case NV_GSP_BOOTER_HS_ERR_METADATA:    return "metadata";
        case NV_GSP_BOOTER_HS_ERR_LOAD_HEADER: return "load-header";
        case NV_GSP_BOOTER_HS_ERR_PATCH_RANGE: return "patch-range";
        default:                               return "unknown";
    }
}

// Validate one raw booter_load/booter_unload container and atomically return a
// plan describing the signed image.  All offsets emitted by NVIDIA's extractor
// are absolute file offsets until the nvfw_hs_load_header_v2 fields, whose code
// and data offsets are relative to the firmware payload.
static inline NVGspBooterHsU32 nvGspPlanBooterHs(
    const unsigned char *buf,
    NVGspBooterHsU64 len,
    NVGspBooterHsPlan *out)
{
    if (!out)
        return NV_GSP_BOOTER_HS_ERR_NULL_OUTPUT;
    if (!buf || len < NV_GSP_BOOTER_NVFW_HEADER_BYTES)
        return NV_GSP_BOOTER_HS_ERR_SHORT;

    const NVGspBooterHsU32 magic       = nvGspBooterHsRead32(buf + 0u);
    const NVGspBooterHsU32 version     = nvGspBooterHsRead32(buf + 4u);
    const NVGspBooterHsU32 paddedBytes = nvGspBooterHsRead32(buf + 8u);
    const NVGspBooterHsU32 hsOffset    = nvGspBooterHsRead32(buf + 12u);
    const NVGspBooterHsU32 dataOffset  = nvGspBooterHsRead32(buf + 16u);
    const NVGspBooterHsU32 dataBytes   = nvGspBooterHsRead32(buf + 20u);

    if (magic != NV_GSP_BOOTER_NVFW_MAGIC ||
        version != NV_GSP_BOOTER_NVFW_VERSION ||
        hsOffset != NV_GSP_BOOTER_NVFW_HEADER_BYTES ||
        !paddedBytes ||
        (paddedBytes & (NV_GSP_BOOTER_BLOCK_BYTES - 1u)) != 0u ||
        paddedBytes < len ||
        (NVGspBooterHsU64)paddedBytes - len >= NV_GSP_BOOTER_BLOCK_BYTES ||
        !nvGspBooterHsFits(len,dataOffset,dataBytes) ||
        (NVGspBooterHsU64)dataOffset + dataBytes != len ||
        !nvGspBooterHsFits(len,hsOffset,NV_GSP_BOOTER_HS_HEADER_BYTES))
        return NV_GSP_BOOTER_HS_ERR_CONTAINER;

    const unsigned char *hs = buf + hsOffset;
    const NVGspBooterHsU32 sigOffset      = nvGspBooterHsRead32(hs + 0u);
    const NVGspBooterHsU32 sigBytes       = nvGspBooterHsRead32(hs + 4u);
    const NVGspBooterHsU32 patchLocOffset = nvGspBooterHsRead32(hs + 8u);
    const NVGspBooterHsU32 patchSigOffset = nvGspBooterHsRead32(hs + 12u);
    const NVGspBooterHsU32 metaOffset     = nvGspBooterHsRead32(hs + 16u);
    const NVGspBooterHsU32 metaBytes      = nvGspBooterHsRead32(hs + 20u);
    const NVGspBooterHsU32 numSigOffset   = nvGspBooterHsRead32(hs + 24u);
    const NVGspBooterHsU32 loadHdrOffset  = nvGspBooterHsRead32(hs + 28u);
    const NVGspBooterHsU32 loadHdrBytes   = nvGspBooterHsRead32(hs + 32u);

    // This is the exact chain produced by NVIDIA's booter() extractor.  Check
    // it with widened arithmetic before any indexed read so malformed offsets
    // cannot wrap back into an apparently-valid prefix of the file.
    const NVGspBooterHsU64 expectedSig =
        (NVGspBooterHsU64)hsOffset + NV_GSP_BOOTER_HS_HEADER_BYTES;
    const NVGspBooterHsU64 expectedPatchLoc = expectedSig + sigBytes;
    const NVGspBooterHsU64 expectedPatchSig = expectedPatchLoc + 4u;
    const NVGspBooterHsU64 expectedMeta = expectedPatchSig + 4u;
    const NVGspBooterHsU64 expectedNumSig = expectedMeta + NV_GSP_BOOTER_META_BYTES;
    const NVGspBooterHsU64 expectedLoadHdr = expectedNumSig + 4u;
    const NVGspBooterHsU64 expectedData = expectedLoadHdr + loadHdrBytes;

    if (sigOffset != expectedSig ||
        patchLocOffset != expectedPatchLoc ||
        patchSigOffset != expectedPatchSig ||
        metaOffset != expectedMeta ||
        metaBytes != NV_GSP_BOOTER_META_BYTES ||
        numSigOffset != expectedNumSig ||
        loadHdrOffset != expectedLoadHdr ||
        dataOffset != expectedData ||
        !sigBytes ||
        !nvGspBooterHsFits(len,sigOffset,sigBytes) ||
        !nvGspBooterHsFits(len,patchLocOffset,4u) ||
        !nvGspBooterHsFits(len,patchSigOffset,4u) ||
        !nvGspBooterHsFits(len,metaOffset,metaBytes) ||
        !nvGspBooterHsFits(len,numSigOffset,4u) ||
        !nvGspBooterHsFits(len,loadHdrOffset,loadHdrBytes))
        return NV_GSP_BOOTER_HS_ERR_LAYOUT;

    const NVGspBooterHsU32 signatureCount =
        nvGspBooterHsRead32(buf + numSigOffset);
    if (!signatureCount || signatureCount > NV_GSP_BOOTER_MAX_SIGNATURES ||
        (sigBytes % signatureCount) != 0u)
        return NV_GSP_BOOTER_HS_ERR_SIGNATURES;
    const NVGspBooterHsU32 signatureStride = sigBytes / signatureCount;
    if (!signatureStride)
        return NV_GSP_BOOTER_HS_ERR_SIGNATURES;

    // The linux-firmware image is intentionally unpatched. booter() writes a
    // zero placeholder here and later runtime code must choose one production
    // signature using the hardware fuse version before patching a private copy.
    if (nvGspBooterHsRead32(buf + patchSigOffset) != 0u)
        return NV_GSP_BOOTER_HS_ERR_SIGNATURES;

    const NVGspBooterHsU32 patchLocation =
        nvGspBooterHsRead32(buf + patchLocOffset);
    if (!nvGspBooterHsRegionFits(dataBytes,patchLocation,signatureStride))
        return NV_GSP_BOOTER_HS_ERR_PATCH_RANGE;

    const NVGspBooterHsU32 fuseVersion = nvGspBooterHsRead32(buf + metaOffset + 0u);
    const NVGspBooterHsU32 engineId    = nvGspBooterHsRead32(buf + metaOffset + 4u);
    const NVGspBooterHsU32 ucodeId     = nvGspBooterHsRead32(buf + metaOffset + 8u);
    if (!engineId || !ucodeId)
        return NV_GSP_BOOTER_HS_ERR_METADATA;

    if (loadHdrBytes < 20u)
        return NV_GSP_BOOTER_HS_ERR_LOAD_HEADER;
    const unsigned char *lh = buf + loadHdrOffset;
    const NVGspBooterHsU32 osCodeOffset   = nvGspBooterHsRead32(lh + 0u);
    const NVGspBooterHsU32 osCodeBytes    = nvGspBooterHsRead32(lh + 4u);
    const NVGspBooterHsU32 osDataOffset   = nvGspBooterHsRead32(lh + 8u);
    const NVGspBooterHsU32 osDataBytes    = nvGspBooterHsRead32(lh + 12u);
    const NVGspBooterHsU32 numApps        = nvGspBooterHsRead32(lh + 16u);

    // GA10x Booter has one app. NVIDIA's extractor explicitly checks the
    // generic formula 5*u32 + num_apps*(4*u32), then notes that its 120-byte
    // fixed prefix assumes num_apps == 1.
    if (numApps != 1u ||
        loadHdrBytes != NV_GSP_BOOTER_ONE_APP_HEADER_BYTES)
        return NV_GSP_BOOTER_HS_ERR_LOAD_HEADER;

    const NVGspBooterHsU32 appCodeOffset = nvGspBooterHsRead32(lh + 20u);
    const NVGspBooterHsU32 appCodeBytes  = nvGspBooterHsRead32(lh + 24u);
    const NVGspBooterHsU32 appDataOffset = nvGspBooterHsRead32(lh + 28u);
    const NVGspBooterHsU32 appDataBytes  = nvGspBooterHsRead32(lh + 32u);

    // Mirror the structural condition enforced by NVIDIA's extractor for Nova
    // and prove every declared IMEM/DMEM slice stays inside this payload.
    if (osCodeBytes != appCodeOffset ||
        !nvGspBooterHsRegionFits(dataBytes,osCodeOffset,osCodeBytes) ||
        !nvGspBooterHsRegionFits(dataBytes,osDataOffset,osDataBytes) ||
        !nvGspBooterHsRegionFits(dataBytes,appCodeOffset,appCodeBytes) ||
        !nvGspBooterHsRegionFits(dataBytes,appDataOffset,appDataBytes))
        return NV_GSP_BOOTER_HS_ERR_LOAD_HEADER;

    NVGspBooterHsPlan staged = {};
    staged.paddedBytes = paddedBytes;
    staged.firmwareOffset = dataOffset;
    staged.firmwareBytes = dataBytes;
    staged.signatureOffset = sigOffset;
    staged.signatureBytes = sigBytes;
    staged.signatureCount = signatureCount;
    staged.signatureStride = signatureStride;
    staged.patchLocation = patchLocation;
    staged.fuseVersion = fuseVersion;
    staged.engineId = engineId;
    staged.ucodeId = ucodeId;
    staged.osCodeOffset = osCodeOffset;
    staged.osCodeBytes = osCodeBytes;
    staged.osDataOffset = osDataOffset;
    staged.osDataBytes = osDataBytes;
    staged.appCodeOffset = appCodeOffset;
    staged.appCodeBytes = appCodeBytes;
    staged.appDataOffset = appDataOffset;
    staged.appDataBytes = appDataBytes;
    staged.valid = true;
    *out = staged;
    return NV_GSP_BOOTER_HS_OK;
}

#endif // NV_GSP_BOOTER_HS_HPP
