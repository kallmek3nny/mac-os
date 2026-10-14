// SPDX-License-Identifier: MIT
// Pure, bounded extraction plan for the .fwimage section of NVIDIA GSP ELF containers.
#ifndef NV_GSP_FW_CONTAINER_HPP
#define NV_GSP_FW_CONTAINER_HPP

using NVGspFwU8  = unsigned char;
using NVGspFwU16 = unsigned short;
using NVGspFwU32 = unsigned int;
using NVGspFwU64 = unsigned long long;

#define NV_GSP_FW_ELFCLASS32                  1u
#define NV_GSP_FW_ELFCLASS64                  2u
#define NV_GSP_FW_ELFDATA2LSB                 1u
#define NV_GSP_FW_EV_CURRENT                  1u
#define NV_GSP_FW_SHT_PROGBITS                1u
#define NV_GSP_FW_SHT_STRTAB                  3u
#define NV_GSP_FW_ELF32_EHDR_BYTES            52u
#define NV_GSP_FW_ELF64_EHDR_BYTES            64u
#define NV_GSP_FW_ELF32_SHDR_BYTES            40u
#define NV_GSP_FW_ELF64_SHDR_BYTES            64u
#define NV_GSP_FW_IMAGE_NAME                  ".fwimage"

#define NV_GSP_FW_CONTAINER_NOT_ATTEMPTED     0u
#define NV_GSP_FW_CONTAINER_OK                1u
#define NV_GSP_FW_CONTAINER_NULL_BLOB         2u
#define NV_GSP_FW_CONTAINER_NULL_OUTPUT       3u
#define NV_GSP_FW_CONTAINER_SHORT             4u
#define NV_GSP_FW_CONTAINER_BAD_MAGIC         5u
#define NV_GSP_FW_CONTAINER_BAD_CLASS         6u
#define NV_GSP_FW_CONTAINER_BAD_ENDIAN        7u
#define NV_GSP_FW_CONTAINER_BAD_VERSION       8u
#define NV_GSP_FW_CONTAINER_BAD_EHSIZE        9u
#define NV_GSP_FW_CONTAINER_BAD_SHENTSIZE     10u
#define NV_GSP_FW_CONTAINER_BAD_SECTION_TABLE 11u
#define NV_GSP_FW_CONTAINER_BAD_SHSTRNDX      12u
#define NV_GSP_FW_CONTAINER_BAD_STRTAB        13u
#define NV_GSP_FW_CONTAINER_BAD_NAME          14u
#define NV_GSP_FW_CONTAINER_NOT_FOUND         15u
#define NV_GSP_FW_CONTAINER_DUPLICATE         16u
#define NV_GSP_FW_CONTAINER_BAD_IMAGE_TYPE    17u
#define NV_GSP_FW_CONTAINER_EMPTY_IMAGE       18u
#define NV_GSP_FW_CONTAINER_IMAGE_OUTSIDE     19u
#define NV_GSP_FW_CONTAINER_LIMIT             20u

struct NVGspFwImageSection {
    NVGspFwU8 elfClass;
    NVGspFwU32 sectionIndex;
    NVGspFwU64 fileOffset;
    NVGspFwU64 fileBytes;
};

static inline bool nvGspFwContainerSucceeded(NVGspFwU32 verdict)
{
    return verdict == NV_GSP_FW_CONTAINER_OK;
}

static inline const char *nvGspFwContainerVerdictName(NVGspFwU32 verdict)
{
    switch (verdict) {
        case NV_GSP_FW_CONTAINER_NOT_ATTEMPTED:     return "not-attempted";
        case NV_GSP_FW_CONTAINER_OK:                return "ok";
        case NV_GSP_FW_CONTAINER_NULL_BLOB:         return "null-blob";
        case NV_GSP_FW_CONTAINER_NULL_OUTPUT:       return "null-output";
        case NV_GSP_FW_CONTAINER_SHORT:             return "short";
        case NV_GSP_FW_CONTAINER_BAD_MAGIC:         return "bad-magic";
        case NV_GSP_FW_CONTAINER_BAD_CLASS:         return "bad-class";
        case NV_GSP_FW_CONTAINER_BAD_ENDIAN:        return "bad-endian";
        case NV_GSP_FW_CONTAINER_BAD_VERSION:       return "bad-version";
        case NV_GSP_FW_CONTAINER_BAD_EHSIZE:        return "bad-ehsize";
        case NV_GSP_FW_CONTAINER_BAD_SHENTSIZE:     return "bad-shentsize";
        case NV_GSP_FW_CONTAINER_BAD_SECTION_TABLE: return "bad-section-table";
        case NV_GSP_FW_CONTAINER_BAD_SHSTRNDX:      return "bad-shstrndx";
        case NV_GSP_FW_CONTAINER_BAD_STRTAB:        return "bad-strtab";
        case NV_GSP_FW_CONTAINER_BAD_NAME:          return "bad-name";
        case NV_GSP_FW_CONTAINER_NOT_FOUND:         return "not-found";
        case NV_GSP_FW_CONTAINER_DUPLICATE:         return "duplicate";
        case NV_GSP_FW_CONTAINER_BAD_IMAGE_TYPE:    return "bad-image-type";
        case NV_GSP_FW_CONTAINER_EMPTY_IMAGE:       return "empty-image";
        case NV_GSP_FW_CONTAINER_IMAGE_OUTSIDE:     return "image-outside";
        default:                                    return "unknown";
    }
}

static inline NVGspFwU16 nvGspFwRead16(const NVGspFwU8 *p)
{
    return (NVGspFwU16)((NVGspFwU16)p[0] | ((NVGspFwU16)p[1] << 8));
}
static inline NVGspFwU32 nvGspFwRead32(const NVGspFwU8 *p)
{
    return (NVGspFwU32)p[0] |
           ((NVGspFwU32)p[1] << 8) |
           ((NVGspFwU32)p[2] << 16) |
           ((NVGspFwU32)p[3] << 24);
}
static inline NVGspFwU64 nvGspFwRead64(const NVGspFwU8 *p)
{
    return (NVGspFwU64)nvGspFwRead32(p) |
           ((NVGspFwU64)nvGspFwRead32(p + 4) << 32);
}

static inline bool nvGspFwRangeFits(NVGspFwU64 blobBytes,
                                    NVGspFwU64 offset,
                                    NVGspFwU64 bytes)
{
    return offset <= blobBytes && bytes <= (blobBytes - offset);
}

static inline bool nvGspFwMulFits(NVGspFwU64 a, NVGspFwU64 b, NVGspFwU64 *out)
{
    if (!out) return false;
    if (a != 0ull && b > ~0ull / a) return false;
    *out = a * b;
    return true;
}

static inline bool nvGspFwNameIsExact(const NVGspFwU8 *strtab,
                                      NVGspFwU64 strtabBytes,
                                      NVGspFwU32 nameOffset,
                                      const char *wanted,
                                      bool *wellFormed)
{
    if (wellFormed) *wellFormed = false;
    if (!strtab || !wanted || (NVGspFwU64)nameOffset >= strtabBytes) return false;

    NVGspFwU64 i = (NVGspFwU64)nameOffset;
    NVGspFwU64 w = 0ull;
    while (i < strtabBytes) {
        const NVGspFwU8 c = strtab[i++];
        const NVGspFwU8 q = (NVGspFwU8)wanted[w];
        if (c == 0u) {
            if (wellFormed) *wellFormed = true;
            return q == 0u;
        }
        if (q == 0u || c != q) {
            // Still prove this string is terminated inside the string table.
            while (i < strtabBytes) {
                if (strtab[i++] == 0u) {
                    if (wellFormed) *wellFormed = true;
                    return false;
                }
            }
            return false;
        }
        ++w;
    }
    return false;
}

// Mirror the section lookup open RM performs through LibosElfFindSectionByName,
// but validate every table/string/data span before returning a caller-visible
// result. Both ELF32 and ELF64 are accepted because NVIDIA's libelf supports
// both and the GSP callsite does not assert one class. Only little-endian
// current-version containers are accepted on this GA107/macOS path.
static inline NVGspFwU32 nvGspFindFwImageSection(const NVGspFwU8 *blob,
                                                 NVGspFwU64 blobBytes,
                                                 NVGspFwImageSection *out)
{
    if (!blob) return NV_GSP_FW_CONTAINER_NULL_BLOB;
    if (!out) return NV_GSP_FW_CONTAINER_NULL_OUTPUT;
    if (blobBytes < NV_GSP_FW_ELF32_EHDR_BYTES)
        return NV_GSP_FW_CONTAINER_SHORT;

    if (blob[0] != 0x7fu || blob[1] != 'E' || blob[2] != 'L' || blob[3] != 'F')
        return NV_GSP_FW_CONTAINER_BAD_MAGIC;
    const NVGspFwU8 elfClass = blob[4];
    if (elfClass != NV_GSP_FW_ELFCLASS32 && elfClass != NV_GSP_FW_ELFCLASS64)
        return NV_GSP_FW_CONTAINER_BAD_CLASS;
    if (blob[5] != NV_GSP_FW_ELFDATA2LSB)
        return NV_GSP_FW_CONTAINER_BAD_ENDIAN;
    if (blob[6] != NV_GSP_FW_EV_CURRENT)
        return NV_GSP_FW_CONTAINER_BAD_VERSION;

    NVGspFwU64 shoff = 0ull;
    NVGspFwU16 ehsize = 0u, shentsize = 0u, shnum = 0u, shstrndx = 0u;
    NVGspFwU32 expectedEh = 0u, expectedSh = 0u;
    NVGspFwU32 nameField = 0u, typeField = 4u;
    NVGspFwU32 offsetField = 0u, sizeField = 0u;

    if (elfClass == NV_GSP_FW_ELFCLASS32) {
        expectedEh = NV_GSP_FW_ELF32_EHDR_BYTES;
        expectedSh = NV_GSP_FW_ELF32_SHDR_BYTES;
        shoff = (NVGspFwU64)nvGspFwRead32(blob + 0x20u);
        ehsize = nvGspFwRead16(blob + 0x28u);
        shentsize = nvGspFwRead16(blob + 0x2eu);
        shnum = nvGspFwRead16(blob + 0x30u);
        shstrndx = nvGspFwRead16(blob + 0x32u);
        offsetField = 0x10u;
        sizeField = 0x14u;
    } else {
        if (blobBytes < NV_GSP_FW_ELF64_EHDR_BYTES)
            return NV_GSP_FW_CONTAINER_SHORT;
        expectedEh = NV_GSP_FW_ELF64_EHDR_BYTES;
        expectedSh = NV_GSP_FW_ELF64_SHDR_BYTES;
        shoff = nvGspFwRead64(blob + 0x28u);
        ehsize = nvGspFwRead16(blob + 0x34u);
        shentsize = nvGspFwRead16(blob + 0x3au);
        shnum = nvGspFwRead16(blob + 0x3cu);
        shstrndx = nvGspFwRead16(blob + 0x3eu);
        offsetField = 0x18u;
        sizeField = 0x20u;
    }

    if ((NVGspFwU32)ehsize != expectedEh)
        return NV_GSP_FW_CONTAINER_BAD_EHSIZE;
    if ((NVGspFwU32)shentsize != expectedSh)
        return NV_GSP_FW_CONTAINER_BAD_SHENTSIZE;
    if (shnum == 0u) return NV_GSP_FW_CONTAINER_BAD_SECTION_TABLE;
    if (shstrndx >= shnum) return NV_GSP_FW_CONTAINER_BAD_SHSTRNDX;

    NVGspFwU64 sectionTableBytes = 0ull;
    if (!nvGspFwMulFits((NVGspFwU64)shnum, (NVGspFwU64)expectedSh,
                        &sectionTableBytes) ||
        !nvGspFwRangeFits(blobBytes, shoff, sectionTableBytes))
        return NV_GSP_FW_CONTAINER_BAD_SECTION_TABLE;

    const NVGspFwU64 strHdrOff = shoff + (NVGspFwU64)shstrndx * expectedSh;
    const NVGspFwU8 *strHdr = blob + strHdrOff;
    if (nvGspFwRead32(strHdr + typeField) != NV_GSP_FW_SHT_STRTAB)
        return NV_GSP_FW_CONTAINER_BAD_STRTAB;
    const NVGspFwU64 strOff = elfClass == NV_GSP_FW_ELFCLASS32
        ? (NVGspFwU64)nvGspFwRead32(strHdr + offsetField)
        : nvGspFwRead64(strHdr + offsetField);
    const NVGspFwU64 strBytes = elfClass == NV_GSP_FW_ELFCLASS32
        ? (NVGspFwU64)nvGspFwRead32(strHdr + sizeField)
        : nvGspFwRead64(strHdr + sizeField);
    if (!strBytes || !nvGspFwRangeFits(blobBytes, strOff, strBytes))
        return NV_GSP_FW_CONTAINER_BAD_STRTAB;
    const NVGspFwU8 *strtab = blob + strOff;

    bool found = false;
    NVGspFwImageSection staged = {};
    for (NVGspFwU32 i = 0u; i < (NVGspFwU32)shnum; ++i) {
        const NVGspFwU8 *sh = blob + shoff + (NVGspFwU64)i * expectedSh;
        const NVGspFwU32 nameOff = nvGspFwRead32(sh + nameField);
        bool wellFormed = false;
        const bool isWanted = nvGspFwNameIsExact(strtab, strBytes, nameOff,
                                                 NV_GSP_FW_IMAGE_NAME,
                                                 &wellFormed);
        if (!wellFormed) return NV_GSP_FW_CONTAINER_BAD_NAME;
        if (!isWanted) continue;
        if (found) return NV_GSP_FW_CONTAINER_DUPLICATE;

        const NVGspFwU32 type = nvGspFwRead32(sh + typeField);
        if (type != NV_GSP_FW_SHT_PROGBITS)
            return NV_GSP_FW_CONTAINER_BAD_IMAGE_TYPE;
        const NVGspFwU64 imageOff = elfClass == NV_GSP_FW_ELFCLASS32
            ? (NVGspFwU64)nvGspFwRead32(sh + offsetField)
            : nvGspFwRead64(sh + offsetField);
        const NVGspFwU64 imageBytes = elfClass == NV_GSP_FW_ELFCLASS32
            ? (NVGspFwU64)nvGspFwRead32(sh + sizeField)
            : nvGspFwRead64(sh + sizeField);
        if (!imageBytes) return NV_GSP_FW_CONTAINER_EMPTY_IMAGE;
        if (!nvGspFwRangeFits(blobBytes, imageOff, imageBytes))
            return NV_GSP_FW_CONTAINER_IMAGE_OUTSIDE;

        staged.elfClass = elfClass;
        staged.sectionIndex = i;
        staged.fileOffset = imageOff;
        staged.fileBytes = imageBytes;
        found = true;
    }

    if (!found) return NV_GSP_FW_CONTAINER_NOT_FOUND;
    *out = staged;
    return NV_GSP_FW_CONTAINER_OK;
}

#endif // NV_GSP_FW_CONTAINER_HPP
