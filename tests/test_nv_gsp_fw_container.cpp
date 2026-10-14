// Host tests for NVGspFwContainer.hpp. No firmware execution or GPU access.
#include <cstdio>
#include <cstring>
#include <vector>
#include "NVGspFwContainer.hpp"
#include "NVGspRadix3.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspFwU32 got, NVGspFwU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspFwU64 got, NVGspFwU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}
static void put16(unsigned char *p, unsigned v) {
    p[0]=(unsigned char)(v&0xffu); p[1]=(unsigned char)((v>>8)&0xffu);
}
static void put32(unsigned char *p, unsigned long long v) {
    for (unsigned i=0;i<4;++i) p[i]=(unsigned char)((v>>(i*8u))&0xffu);
}
static void put64(unsigned char *p, unsigned long long v) {
    for (unsigned i=0;i<8;++i) p[i]=(unsigned char)((v>>(i*8u))&0xffu);
}

struct Fixture {
    std::vector<unsigned char> b;
    bool is64;
    unsigned shoff;
    unsigned shbytes;
    unsigned strOff;
    unsigned imageOff;
    unsigned imageBytes;
};

static Fixture makeElf(bool is64, bool duplicate=false) {
    Fixture f;
    f.b.assign(0x500u,0u);
    f.is64=is64;
    f.shoff=0x100u;
    f.shbytes=is64?64u:40u;
    f.strOff=0x300u;
    f.imageOff=0x380u;
    f.imageBytes=0x23u;

    unsigned char *b=f.b.data();
    b[0]=0x7f; b[1]='E'; b[2]='L'; b[3]='F';
    b[4]=is64?NV_GSP_FW_ELFCLASS64:NV_GSP_FW_ELFCLASS32;
    b[5]=NV_GSP_FW_ELFDATA2LSB; b[6]=NV_GSP_FW_EV_CURRENT;
    const unsigned shnum=4u;
    if (is64) {
        put64(b+0x28u,f.shoff);
        put16(b+0x34u,NV_GSP_FW_ELF64_EHDR_BYTES);
        put16(b+0x3au,NV_GSP_FW_ELF64_SHDR_BYTES);
        put16(b+0x3cu,shnum);
        put16(b+0x3eu,1u);
    } else {
        put32(b+0x20u,f.shoff);
        put16(b+0x28u,NV_GSP_FW_ELF32_EHDR_BYTES);
        put16(b+0x2eu,NV_GSP_FW_ELF32_SHDR_BYTES);
        put16(b+0x30u,shnum);
        put16(b+0x32u,1u);
    }

    const char names[]="\0.shstrtab\0.fwimage\0.other\0";
    std::memcpy(b+f.strOff,names,sizeof(names));

    auto setSection=[&](unsigned idx,unsigned name,unsigned type,
                        unsigned long long off,unsigned long long size) {
        unsigned char *s=b+f.shoff+idx*f.shbytes;
        put32(s+0u,name); put32(s+4u,type);
        if (f.is64) { put64(s+0x18u,off); put64(s+0x20u,size); }
        else { put32(s+0x10u,off); put32(s+0x14u,size); }
    };
    setSection(1u,1u,NV_GSP_FW_SHT_STRTAB,f.strOff,sizeof(names));
    setSection(2u,11u,NV_GSP_FW_SHT_PROGBITS,f.imageOff,f.imageBytes);
    setSection(3u,duplicate?11u:20u,NV_GSP_FW_SHT_PROGBITS,
               duplicate?0x3c0u:0x3d0u,duplicate?8u:5u);

    for (unsigned i=0;i<f.imageBytes;++i)
        b[f.imageOff+i]=(unsigned char)((i*17u+3u)&0xffu);
    return f;
}

static unsigned char *section(Fixture &f,unsigned idx) {
    return f.b.data()+f.shoff+idx*f.shbytes;
}
static void setSectionOffset(Fixture &f,unsigned idx,unsigned long long v) {
    if (f.is64) put64(section(f,idx)+0x18u,v); else put32(section(f,idx)+0x10u,v);
}
static void setSectionSize(Fixture &f,unsigned idx,unsigned long long v) {
    if (f.is64) put64(section(f,idx)+0x20u,v); else put32(section(f,idx)+0x14u,v);
}

static void testHappy(bool is64) {
    std::printf("\n1. %s .fwimage exact extraction\n",is64?"ELF64":"ELF32");
    Fixture f=makeElf(is64);
    NVGspFwImageSection s={};
    checkU32("verdict",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_OK);
    checkU32("class",s.elfClass,is64?NV_GSP_FW_ELFCLASS64:NV_GSP_FW_ELFCLASS32);
    checkU32("index",s.sectionIndex,2u);
    checkU64("offset",s.fileOffset,f.imageOff);
    checkU64("bytes",s.fileBytes,f.imageBytes);
    check("first byte",f.b[(size_t)s.fileOffset]==3u);

    NVGspRadix3Plan r={};
    checkU32("radix plan",nvGspPlanEmbeddedRadix3(s.fileBytes,&r),NV_GSP_RADIX3_OK);
    checkU64("radix data bytes",r.dataBytes,f.imageBytes);
    checkU64("radix data pages",r.dataPages,1ull);
}

static void testHeaderRefusals(void) {
    std::printf("\n2. malformed ELF headers refuse\n");
    Fixture f=makeElf(true);
    NVGspFwImageSection s={};
    checkU32("short",nvGspFindFwImageSection(f.b.data(),40u,&s),NV_GSP_FW_CONTAINER_SHORT);
    f=makeElf(true); f.b[0]=0;
    checkU32("magic",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_MAGIC);
    f=makeElf(true); f.b[4]=0xff;
    checkU32("class",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_CLASS);
    f=makeElf(true); f.b[5]=2;
    checkU32("endian",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_ENDIAN);
    f=makeElf(true); f.b[6]=0;
    checkU32("version",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_VERSION);
    f=makeElf(true); put16(f.b.data()+0x34u,63u);
    checkU32("ehsize",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_EHSIZE);
    f=makeElf(false); put16(f.b.data()+0x2eu,64u);
    checkU32("shentsize",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_SHENTSIZE);
}

static void testTableRefusals(void) {
    std::printf("\n3. section/string table bounds refuse\n");
    NVGspFwImageSection s={};
    Fixture f=makeElf(true); put16(f.b.data()+0x3cu,0u);
    checkU32("zero sections",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_SECTION_TABLE);
    f=makeElf(true); put16(f.b.data()+0x3eu,4u);
    checkU32("bad shstrndx",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_SHSTRNDX);
    f=makeElf(true); put64(f.b.data()+0x28u,0x4f0ull);
    checkU32("table outside",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_SECTION_TABLE);
    f=makeElf(true); put32(section(f,1u)+4u,NV_GSP_FW_SHT_PROGBITS);
    checkU32("strtab type",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_STRTAB);
    f=makeElf(true); setSectionOffset(f,1u,0x4f0ull); setSectionSize(f,1u,0x40ull);
    checkU32("strtab outside",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_STRTAB);
    f=makeElf(true); put32(section(f,2u),0x1000u);
    checkU32("name offset",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_NAME);
    f=makeElf(true);
    // Point .fwimage's name at the last byte and make it non-NUL.
    put32(section(f,2u),(NVGspFwU32)(sizeof("\0.shstrtab\0.fwimage\0.other\0")-2u));
    f.b[f.strOff+sizeof("\0.shstrtab\0.fwimage\0.other\0")-2u]='X';
    checkU32("unterminated name",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_NAME);
}

static void testImageRefusals(void) {
    std::printf("\n4. .fwimage identity and file span refuse\n");
    NVGspFwImageSection s={};
    Fixture f=makeElf(true,true);
    checkU32("duplicate",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_DUPLICATE);
    f=makeElf(true); put32(section(f,2u)+4u,8u);
    checkU32("nobits",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_IMAGE_TYPE);
    f=makeElf(true); setSectionSize(f,2u,0ull);
    checkU32("empty",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_EMPTY_IMAGE);
    f=makeElf(true); setSectionOffset(f,2u,0x4f0ull); setSectionSize(f,2u,0x40ull);
    checkU32("outside",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_IMAGE_OUTSIDE);
    f=makeElf(true); put32(section(f,2u),20u); // rename .fwimage -> .other
    checkU32("missing",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_NOT_FOUND);
}

static void testAtomicityAndVerdicts(void) {
    std::printf("\n5. refusal is atomic and verdict family is total\n");
    Fixture f=makeElf(true);
    NVGspFwImageSection s;
    std::memset(&s,0xa5,sizeof(s));
    const NVGspFwImageSection before=s;
    f.b[0]=0;
    checkU32("refuse",nvGspFindFwImageSection(f.b.data(),f.b.size(),&s),NV_GSP_FW_CONTAINER_BAD_MAGIC);
    check("output unchanged",std::memcmp(&s,&before,sizeof(s))==0);
    checkU32("null blob",nvGspFindFwImageSection(nullptr,0,&s),NV_GSP_FW_CONTAINER_NULL_BLOB);
    checkU32("null output",nvGspFindFwImageSection(f.b.data(),f.b.size(),nullptr),NV_GSP_FW_CONTAINER_NULL_OUTPUT);
    for (NVGspFwU32 v=0;v<NV_GSP_FW_CONTAINER_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspFwContainerVerdictName(v),"unknown")!=0);
        check("success equality",nvGspFwContainerSucceeded(v)==(v==NV_GSP_FW_CONTAINER_OK));
    }
}

int main(void) {
    std::printf("NVGspFwContainer.hpp -- GSP .fwimage extraction\n");
    testHappy(false);
    testHappy(true);
    testHeaderRefusals();
    testTableRefusals();
    testImageRefusals();
    testAtomicityAndVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
