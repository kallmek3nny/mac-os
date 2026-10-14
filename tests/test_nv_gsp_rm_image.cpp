// Host tests for NVGspRmImage.hpp. No IOKit, allocation or GPU access.
#include <cstdio>
#include <cstring>
#include <vector>
#include "NVGspRmImage.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspRmU32 got, NVGspRmU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspRmU64 got, NVGspRmU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}
static void put16(unsigned char *p,unsigned v) {
    p[0]=(unsigned char)(v&0xffu); p[1]=(unsigned char)((v>>8)&0xffu);
}
static void put32(unsigned char *p,unsigned long long v) {
    for (unsigned i=0;i<4;++i) p[i]=(unsigned char)((v>>(i*8u))&0xffu);
}
static void put64(unsigned char *p,unsigned long long v) {
    for (unsigned i=0;i<8;++i) p[i]=(unsigned char)((v>>(i*8u))&0xffu);
}

static std::vector<unsigned char> makeElf64(unsigned imageBytes=0x2345u) {
    const unsigned shoff=0x100u, shentsize=64u, shnum=3u;
    const unsigned strOff=0x200u, imageOff=0x300u;
    const char names[]="\0.shstrtab\0.fwimage\0";
    std::vector<unsigned char> b((size_t)imageOff+imageBytes,0u);
    b[0]=0x7f; b[1]='E'; b[2]='L'; b[3]='F';
    b[4]=NV_GSP_FW_ELFCLASS64; b[5]=NV_GSP_FW_ELFDATA2LSB; b[6]=NV_GSP_FW_EV_CURRENT;
    put64(b.data()+0x28u,shoff);
    put16(b.data()+0x34u,NV_GSP_FW_ELF64_EHDR_BYTES);
    put16(b.data()+0x3au,shentsize);
    put16(b.data()+0x3cu,shnum);
    put16(b.data()+0x3eu,1u);
    std::memcpy(b.data()+strOff,names,sizeof(names));

    unsigned char *str=b.data()+shoff+shentsize;
    put32(str+0u,1u); put32(str+4u,NV_GSP_FW_SHT_STRTAB);
    put64(str+0x18u,strOff); put64(str+0x20u,sizeof(names));
    unsigned char *img=b.data()+shoff+2u*shentsize;
    put32(img+0u,11u); put32(img+4u,NV_GSP_FW_SHT_PROGBITS);
    put64(img+0x18u,imageOff); put64(img+0x20u,imageBytes);
    for (unsigned i=0;i<imageBytes;++i)
        b[imageOff+i]=(unsigned char)((i*13u+5u)&0xffu);
    return b;
}

static void testNamespaceSeparation(void) {
    std::printf("\n1. full GSP-RM and bootloader inputs are mechanically distinct\n");
    check("property names differ",std::strcmp(NV_GSP_BOOTLOADER_PROPERTY_NAME,
                                               NV_GSP_RM_FIRMWARE_PROPERTY_NAME)!=0);
    check("bootloader property exact",std::strcmp(NV_GSP_BOOTLOADER_PROPERTY_NAME,"NVGspFirmware")==0);
    check("RM property exact",std::strcmp(NV_GSP_RM_FIRMWARE_PROPERTY_NAME,"NVGspRmFirmware")==0);
    check("stage boot arg exact",std::strcmp(NV_GSP_RM_STAGE_BOOT_ARG,"nvgsp-rm-image-stage")==0);
    checkU32("page bytes",NV_GSP_RM_STAGE_PAGE_BYTES,4096u);
    check("noncontiguous",NV_GSP_RM_STAGE_NONCONTIGUOUS==1u);
    check("cached",NV_GSP_RM_STAGE_CACHED==1u);
}

static void testExactPlan(void) {
    std::printf("\n2. .fwimage span feeds exact embedded Radix3 geometry\n");
    std::vector<unsigned char> blob=makeElf64();
    NVGspRmImagePlan p={};
    checkU32("verdict",nvGspPlanRmFirmwareImage(blob.data(),blob.size(),&p),NV_GSP_RM_IMAGE_OK);
    checkU64("container bytes",p.containerBytes,blob.size());
    checkU32("container verdict",p.containerVerdict,NV_GSP_FW_CONTAINER_OK);
    checkU32("section index",p.imageSection.sectionIndex,2u);
    checkU64("image offset",p.imageSection.fileOffset,0x300ull);
    checkU64("image bytes",p.imageSection.fileBytes,0x2345ull);
    checkU64("image end",p.imageEndOffset,0x2645ull);
    checkU32("radix verdict",p.radix3Verdict,NV_GSP_RADIX3_OK);
    checkU64("radix data bytes",p.radix3.dataBytes,0x2345ull);
    checkU64("radix data pages",p.radix3.dataPages,3ull);
    checkU64("table pages",p.radix3.tablePages,3ull);
    checkU64("stage pages",p.stageAllocationPages,6ull);
    checkU64("stage bytes",p.stageAllocationBytes,0x6000ull);
    checkU32("stage page size",p.stagePageBytes,4096u);
    check("stage noncontiguous",p.stageNoncontiguous);
    check("stage cached",p.stageCached);
}

static void testBoundarySizes(void) {
    std::printf("\n3. section size alone determines Radix3 expansion\n");
    std::vector<unsigned char> one=makeElf64(1u);
    NVGspRmImagePlan p={};
    checkU32("one byte",nvGspPlanRmFirmwareImage(one.data(),one.size(),&p),NV_GSP_RM_IMAGE_OK);
    checkU64("one data page",p.radix3.dataPages,1ull);
    checkU64("one alloc pages",p.stageAllocationPages,4ull);

    std::vector<unsigned char> cross=makeElf64(4097u);
    checkU32("4097 bytes",nvGspPlanRmFirmwareImage(cross.data(),cross.size(),&p),NV_GSP_RM_IMAGE_OK);
    checkU64("two data pages",p.radix3.dataPages,2ull);
    checkU64("two alloc pages",p.stageAllocationPages,5ull);
}

static void testRefusalPropagationAndAtomicity(void) {
    std::printf("\n4. malformed container refuses without publishing a partial plan\n");
    std::vector<unsigned char> blob=makeElf64();
    blob[0]=0u;
    NVGspRmImagePlan p;
    std::memset(&p,0xa5,sizeof(p));
    const NVGspRmImagePlan before=p;
    checkU32("container refused",nvGspPlanRmFirmwareImage(blob.data(),blob.size(),&p),NV_GSP_RM_IMAGE_CONTAINER_REFUSED);
    check("plan atomic",std::memcmp(&p,&before,sizeof(p))==0);

    checkU32("null blob",nvGspPlanRmFirmwareImage(nullptr,0u,&p),NV_GSP_RM_IMAGE_NULL_BLOB);
    checkU32("null output",nvGspPlanRmFirmwareImage(blob.data(),blob.size(),nullptr),NV_GSP_RM_IMAGE_NULL_OUTPUT);
}

static void testVerdicts(void) {
    std::printf("\n5. verdict family\n");
    for (NVGspRmU32 v=0u;v<NV_GSP_RM_IMAGE_LIMIT;++v) {
        check("verdict named",std::strcmp(nvGspRmImageVerdictName(v),"unknown")!=0);
        check("success equality",nvGspRmImageSucceeded(v)==(v==NV_GSP_RM_IMAGE_OK));
    }
}

int main(void) {
    std::printf("NVGspRmImage.hpp -- full GSP-RM firmware staging plan\n");
    testNamespaceSeparation();
    testExactPlan();
    testBoundarySizes();
    testRefusalPropagationAndAtomicity();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
