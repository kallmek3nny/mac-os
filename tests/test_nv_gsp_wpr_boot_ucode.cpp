// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVGspWprBootUcode.hpp"
#include "../phase0-kext/NVGspWprBootDma.hpp"

static int gChecks=0;
static int gFailures=0;

static void checkU32(const char *name, NVU32 got, NVU32 expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = 0x%X\n",name,got); return; }
    ++gFailures;
    std::printf("FAIL %s got 0x%X expected 0x%X\n",name,got,expected);
}

static void checkU64(const char *name, unsigned long long got,
                     unsigned long long expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = 0x%llX\n",name,got); return; }
    ++gFailures;
    std::printf("FAIL %s got 0x%llX expected 0x%llX\n",name,got,expected);
}

static void checkBool(const char *name, bool got, bool expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = %s\n",name,got?"true":"false"); return; }
    ++gFailures;
    std::printf("FAIL %s got %s expected %s\n",name,got?"true":"false",expected?"true":"false");
}

static void checkSame(const char *name, const void *a, const void *b, size_t n)
{
    ++gChecks;
    if (std::memcmp(a,b,n)==0) { std::printf("ok  %s unchanged\n",name); return; }
    ++gFailures;
    std::printf("FAIL %s mutated\n",name);
}

static unsigned char gBlob[0x8000];

static void put32(NVU32 off, NVU32 v)
{
    gBlob[off+0]=(unsigned char)(v&0xffu);
    gBlob[off+1]=(unsigned char)((v>>8)&0xffu);
    gBlob[off+2]=(unsigned char)((v>>16)&0xffu);
    gBlob[off+3]=(unsigned char)((v>>24)&0xffu);
}

static void putDesc(NVU32 field, NVU32 v)
{
    put32(NV_RISCV_GA107_BL_HDR_OFFSET+field,v);
}

static NVU32 buildBlob(NVU32 dataBytes)
{
    std::memset(gBlob,0xA5,sizeof(gBlob));
    const NVU32 hdr=NV_RISCV_GA107_BL_HDR_OFFSET;
    const NVU32 data=NV_RISCV_GA107_BL_DATA_OFFSET;
    const NVU32 total=data+dataBytes;

    put32(NV_RISCV_BIN_HDR_MAGIC_OFFSET,NV_RISCV_BIN_HDR_MAGIC);
    put32(NV_RISCV_BIN_HDR_VERSION_OFFSET,NV_RISCV_BIN_HDR_VERSION_1);
    put32(NV_RISCV_BIN_HDR_BIN_SIZE_OFFSET,total);
    put32(NV_RISCV_BIN_HDR_HDR_OFFSET_OFFSET,hdr);
    put32(NV_RISCV_BIN_HDR_DATA_OFFSET_OFFSET,data);
    put32(NV_RISCV_BIN_HDR_DATA_SIZE_OFFSET,dataBytes);

    std::memset(gBlob+hdr,0,NV_RISCV_GA107_BL_DESC_BYTES);
    putDesc(NV_RISCV_DESC_VERSION_OFFSET,NV_RISCV_DESC_VERSION_5);
    putDesc(NV_RISCV_DESC_BOOTLOADER_OFFSET,0x5000u);
    putDesc(NV_RISCV_DESC_BOOTLOADER_SIZE,0x0880u);
    putDesc(NV_RISCV_DESC_BOOTLOADER_PARAM_OFFSET,0x5880u);
    putDesc(NV_RISCV_DESC_BOOTLOADER_PARAM_SIZE,0x0010u);
    putDesc(NV_RISCV_DESC_MANIFEST_OFFSET,0x0100u);
    putDesc(NV_RISCV_DESC_MANIFEST_SIZE,0x0100u);
    putDesc(NV_RISCV_DESC_MONITOR_DATA_OFFSET,0x0800u);
    putDesc(NV_RISCV_DESC_MONITOR_DATA_SIZE,0x1000u);
    putDesc(NV_RISCV_DESC_MONITOR_CODE_OFFSET,0x1800u);
    putDesc(NV_RISCV_DESC_MONITOR_CODE_SIZE,0x2900u);
    putDesc(NV_RISCV_DESC_FB_RESERVED_SIZE,dataBytes);
    putDesc(NV_RISCV_DESC_SIGNED_AS_CODE,1u);
    return total;
}

static void testHappy(void)
{
    NVU32 len=buildBlob(0x6000u);
    NVGspWprBootUcodePlan p={};
    NVU32 v=nvGspPlanWprBootUcode(gBlob,len,&p);

    checkU32("happy verdict",v,NV_GSP_WPR_BOOT_OK);
    checkBool("happy valid",p.valid,true);
    checkU32("source offset is nvfw payload",p.sourceOffset,108u);
    checkU32("source bytes is complete boot ucode",p.sourceBytes,0x6000u);
    checkU32("already-page-aligned allocation",p.allocationBytes,0x6000u);
    checkU32("monitor code offset",p.monitorCodeOffset,0x1800u);
    checkU32("monitor data offset",p.monitorDataOffset,0x0800u);
    checkU32("manifest offset",p.manifestOffset,0x0100u);
    checkU32("structural ucode verdict",(NVU32)p.ucodeVerdict,(NVU32)NV_RISCV_UCODE_OK);

    // The old Phase-5 transfer slice is intentionally NOT this WPR image.
    checkBool("WPR source is larger than IMEM transfer slice",
              p.sourceBytes>NV_RISCV_GA107_BL_BOOTLOADER_SIZE,true);
    checkBool("monitor code precedes old transfer slice",
              p.monitorCodeOffset<NV_RISCV_GA107_BL_BOOTLOADER_OFFSET,true);
}

static void testPageRounding(void)
{
    NVU32 rounded=0;
    checkBool("round 0x6000",nvGspWprBootRoundPage(0x6000u,&rounded),true);
    checkU32("round 0x6000 result",rounded,0x6000u);
    checkBool("round 0x6001",nvGspWprBootRoundPage(0x6001u,&rounded),true);
    checkU32("round 0x6001 result",rounded,0x7000u);
    checkBool("zero size refused",nvGspWprBootRoundPage(0u,&rounded),false);
    checkBool("u32 page-round overflow refused",
              nvGspWprBootRoundPage(0xfffff001u,&rounded),false);

    NVU32 len=buildBlob(0x6001u);
    NVGspWprBootUcodePlan p={};
    checkU32("unaligned image plan",nvGspPlanWprBootUcode(gBlob,len,&p),NV_GSP_WPR_BOOT_OK);
    checkU32("unaligned source remains exact",p.sourceBytes,0x6001u);
    checkU32("only allocation is padded",p.allocationBytes,0x7000u);
}

static void testRefusalAtomicity(void)
{
    NVU32 len=buildBlob(0x6000u);
    NVGspWprBootUcodePlan poison;
    std::memset(&poison,0x5A,sizeof(poison));
    NVGspWprBootUcodePlan before=poison;

    gBlob[NV_RISCV_BIN_HDR_MAGIC_OFFSET]^=0x01u;
    checkU32("bad container -> bad ucode",
             nvGspPlanWprBootUcode(gBlob,len,&poison),NV_GSP_WPR_BOOT_BAD_UCODE);
    checkSame("bad ucode output",&poison,&before,sizeof(poison));

    len=buildBlob(0x6000u);
    putDesc(NV_RISCV_DESC_MONITOR_CODE_OFFSET,0x6000u);
    putDesc(NV_RISCV_DESC_MONITOR_CODE_SIZE,0u);
    checkU32("zero-size region cannot smuggle end offset",
             nvGspPlanWprBootUcode(gBlob,len,&poison),NV_GSP_WPR_BOOT_BAD_OFFSETS);
    checkSame("bad offsets output",&poison,&before,sizeof(poison));

    checkU32("null output",nvGspPlanWprBootUcode(gBlob,len,nullptr),NV_GSP_WPR_BOOT_NULL_OUTPUT);
}

static void testDmaContract(void)
{
    const unsigned long long base=0x0000123456700000ull;
    NVGspWprBootDmaPlan p={};
    checkU32("single contiguous GA107 IOVA",
             nvGspPlanWprBootDma(base,0x6000ull,0x5801ull,0x6000ull,&p),
             NV_GSP_WPR_BOOT_DMA_OK);
    checkBool("DMA plan valid",p.valid,true);
    checkU64("DMA base",p.gpuAddress,base);
    checkU64("DMA exact source bytes",p.sourceBytes,0x5801ull);
    checkU64("DMA allocation bytes",p.allocationBytes,0x6000ull);

    // This path is AT_GPU/device-domain, not Falcon DMATRFBASE. Do not inherit
    // the unrelated 41-bit Phase-5 transfer policy without source evidence.
    checkU32("above Phase-5 41-bit policy remains admissible",
             nvGspPlanWprBootDma(0x0000040000000000ull,0x6000ull,
                                 0x5801ull,0x6000ull,&p),
             NV_GSP_WPR_BOOT_DMA_OK);

    NVGspWprBootDmaPlan poison;
    std::memset(&poison,0xA5,sizeof(poison));
    NVGspWprBootDmaPlan before=poison;

    checkU32("DMA source exceeds allocation",
             nvGspPlanWprBootDma(0x1000ull,0x6000ull,0x7000ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_BAD_ALLOCATION);
    checkSame("bad DMA allocation atomic",&poison,&before,sizeof(poison));

    checkU32("DMA base must be page aligned",
             nvGspPlanWprBootDma(0x1001ull,0x6000ull,0x5801ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_BAD_ALIGNMENT);
    checkSame("bad DMA alignment atomic",&poison,&before,sizeof(poison));

    checkU32("single segment must cover allocation",
             nvGspPlanWprBootDma(0x1000ull,0x5fffull,0x5801ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_SHORT_SEGMENT);
    checkSame("short DMA segment atomic",&poison,&before,sizeof(poison));

    checkU32("64-bit DMA range overflow refused",
             nvGspPlanWprBootDma(0xfffffffffffff000ull,0x6000ull,
                                 0x5801ull,0x6000ull,&poison),
             NV_GSP_WPR_BOOT_DMA_RANGE_OVERFLOW);
    checkSame("DMA overflow atomic",&poison,&before,sizeof(poison));
}

int main(void)
{
    testHappy();
    testPageRounding();
    testRefusalAtomicity();
    testDmaContract();

    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
