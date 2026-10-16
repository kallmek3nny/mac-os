// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>

#include "../phase0-kext/NVGspWprColdBootCompose.hpp"

static int gChecks=0;
static int gFailures=0;

static void checkU32(const char *name,NVGspWprU32 got,NVGspWprU32 expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = %u\n",name,got); return; }
    ++gFailures;
    std::printf("FAIL %s got %u expected %u\n",name,got,expected);
}

static void checkU64(const char *name,NVGspWprU64 got,NVGspWprU64 expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = 0x%llx\n",name,got); return; }
    ++gFailures;
    std::printf("FAIL %s got 0x%llx expected 0x%llx\n",name,got,expected);
}

static void checkBool(const char *name,bool got,bool expected)
{
    ++gChecks;
    if (got==expected) { std::printf("ok  %s = %s\n",name,got?"true":"false"); return; }
    ++gFailures;
    std::printf("FAIL %s got %s expected %s\n",name,got?"true":"false",expected?"true":"false");
}

static void checkSame(const char *name,const void *a,const void *b,size_t n)
{
    ++gChecks;
    if (std::memcmp(a,b,n)==0) { std::printf("ok  %s unchanged\n",name); return; }
    ++gFailures;
    std::printf("FAIL %s mutated\n",name);
}

static NVGspWprColdBootInput normalInput(void)
{
    NVGspWprColdBootInput in={};

    in.sysmem.radix3RootGpuAddress=0x0000001200000000ull;
    in.sysmem.radix3ImageBytes=0x04000000ull;
    in.sysmem.bootGpuAddress=0x0000001300000000ull;
    in.sysmem.bootImageBytes=0x00006000ull;
    in.sysmem.bootCodeOffset=0x00001800ull;
    in.sysmem.bootDataOffset=0x00000800ull;
    in.sysmem.bootManifestOffset=0x00000000ull;
    in.sysmem.valid=true;

    in.facts.fbSizeBytes=0x0000000100000000ull;
    in.facts.displaySupported=true;
    in.facts.vgaWorkspaceRegisterValid=true;
    in.facts.vgaWorkspaceRegisterOffset=0x00000000fe000000ull;
    in.facts.vgaWorkspacePolicyOffset=0x00000000fffe0000ull;
    in.facts.mmuLockValid=false;
    in.facts.mmuLockLo=0ull;

    // These remain explicit caller policy rather than being guessed by the
    // compositor. Use nonzero known-good values to prove the pass-through.
    in.options.signatureGpuAddress=0x0000001400000000ull;
    in.options.signatureBytes=0x00001000ull;
    in.options.crashQueueGpuAddress=0ull;
    in.options.crashQueueBytes=0ull;
    in.options.vfPartitionCount=3u;
    in.options.flags=(NVGspWprU8)(NV_GSP_WPR_FLAG_CLOCK_BOOST |
                                  NV_GSP_WPR_FLAG_PPCIE_ENABLED);
    in.options.pagingConfig=0x1234u;
    return in;
}

static void golden(void)
{
    NVGspWprColdBootInput in=normalInput();
    NVGspWprColdBootPlan p={};
    checkU32("compose verdict",nvGspComposeGa107ColdBootWpr(&in,&p),NV_GSP_WPR_COLD_BOOT_OK);
    checkBool("plan valid",p.valid,true);

    checkU64("policy WPR end",p.ga107.layout.gspFwWprEnd,0x00000000fffe0000ull);
    checkU64("policy FRTS",p.ga107.layout.frtsOffset,0x00000000ffee0000ull);
    checkU64("policy boot bin",p.ga107.layout.bootBinOffset,0x00000000ffeda000ull);
    checkU64("policy firmware",p.ga107.layout.gspFwOffset,0x00000000fbed0000ull);
    checkU64("policy heap",p.ga107.layout.gspFwHeapOffset,0x00000000f3f00000ull);
    checkU64("policy WPR start",p.ga107.layout.gspFwWprStart,0x00000000f3e00000ull);

    checkU64("image radix root",p.imageInput.radix3ElfGpuAddress,in.sysmem.radix3RootGpuAddress);
    checkU64("image radix bytes",p.imageInput.radix3ElfBytes,in.sysmem.radix3ImageBytes);
    checkU64("image boot base",p.imageInput.bootloaderGpuAddress,in.sysmem.bootGpuAddress);
    checkU64("image boot bytes",p.imageInput.bootloaderBytes,in.sysmem.bootImageBytes);
    checkU64("image VGA bytes",p.imageInput.vgaWorkspaceBytes,0x00020000ull);

    checkU64("meta magic",p.metadata.magic,NV_GSP_WPR_META_MAGIC);
    checkU64("meta radix root",p.metadata.sysmemAddrOfRadix3Elf,in.sysmem.radix3RootGpuAddress);
    checkU64("meta radix bytes",p.metadata.sizeOfRadix3Elf,in.sysmem.radix3ImageBytes);
    checkU64("meta boot base",p.metadata.sysmemAddrOfBootloader,in.sysmem.bootGpuAddress);
    checkU64("meta boot bytes",p.metadata.sizeOfBootloader,in.sysmem.bootImageBytes);
    checkU64("meta code offset",p.metadata.bootloaderCodeOffset,0x1800ull);
    checkU64("meta data offset",p.metadata.bootloaderDataOffset,0x800ull);
    checkU64("meta manifest offset",p.metadata.bootloaderManifestOffset,0ull);
    checkU64("meta signature base",p.metadata.sysmemAddrOfSignature,in.options.signatureGpuAddress);
    checkU64("meta signature bytes",p.metadata.sizeOfSignature,in.options.signatureBytes);
    checkU64("meta fb size",p.metadata.fbSize,in.facts.fbSizeBytes);
    checkU64("meta VGA offset",p.metadata.vgaWorkspaceOffset,in.facts.vgaWorkspacePolicyOffset);
    checkU64("meta VGA bytes",p.metadata.vgaWorkspaceSize,0x00020000ull);
    checkU32("meta VF count",p.metadata.gspFwHeapVfPartitionCount,3u);
    checkU32("meta flags",p.metadata.flags,0x05u);
    checkU32("meta paging config",p.metadata.pagingConfig,0x1234u);
    checkU64("meta verified remains Booter-owned",p.metadata.verified,0ull);
}

static void zeroOptionalPairs(void)
{
    NVGspWprColdBootInput in=normalInput();
    in.options.signatureGpuAddress=0ull;
    in.options.signatureBytes=0ull;
    in.options.crashQueueGpuAddress=0ull;
    in.options.crashQueueBytes=0ull;

    NVGspWprColdBootPlan p={};
    checkU32("zero optional pairs accepted",
             nvGspComposeGa107ColdBootWpr(&in,&p),NV_GSP_WPR_COLD_BOOT_OK);
    checkU64("zero signature address",p.metadata.sysmemAddrOfSignature,0ull);
    checkU64("zero signature size",p.metadata.sizeOfSignature,0ull);
    checkU64("zero crash address",p.metadata.sysmemAddrOfCrashReportQueue,0ull);
    checkU32("zero crash size",p.metadata.sizeOfCrashReportQueue,0u);
}

static void refusalsAtomic(void)
{
    NVGspWprColdBootPlan poison;
    std::memset(&poison,0xA5,sizeof(poison));
    NVGspWprColdBootPlan before=poison;

    NVGspWprColdBootInput in=normalInput();
    in.sysmem.valid=false;
    checkU32("invalid sysmem",nvGspComposeGa107ColdBootWpr(&in,&poison),NV_GSP_WPR_COLD_BOOT_BAD_SYSMEM);
    checkSame("invalid sysmem atomic",&poison,&before,sizeof(poison));

    in=normalInput(); in.facts.vgaWorkspacePolicyOffset=0ull;
    checkU32("missing facts workspace",nvGspComposeGa107ColdBootWpr(&in,&poison),NV_GSP_WPR_COLD_BOOT_BAD_FACTS);
    checkSame("bad facts atomic",&poison,&before,sizeof(poison));

    in=normalInput(); in.sysmem.radix3ImageBytes=in.facts.fbSizeBytes;
    checkU32("impossible WPR placement",nvGspComposeGa107ColdBootWpr(&in,&poison),NV_GSP_WPR_COLD_BOOT_POLICY_REFUSED);
    checkSame("policy refusal atomic",&poison,&before,sizeof(poison));

    in=normalInput(); in.options.signatureBytes=0ull;
    checkU32("half signature pair",nvGspComposeGa107ColdBootWpr(&in,&poison),NV_GSP_WPR_COLD_BOOT_META_REFUSED);
    checkSame("signature refusal atomic",&poison,&before,sizeof(poison));

    in=normalInput(); in.options.flags=0x80u;
    checkU32("unknown WPR flag",nvGspComposeGa107ColdBootWpr(&in,&poison),NV_GSP_WPR_COLD_BOOT_META_REFUSED);
    checkSame("flag refusal atomic",&poison,&before,sizeof(poison));

    checkU32("null input",nvGspComposeGa107ColdBootWpr(nullptr,&poison),NV_GSP_WPR_COLD_BOOT_NULL_INPUT);
    checkU32("null output",nvGspComposeGa107ColdBootWpr(&in,nullptr),NV_GSP_WPR_COLD_BOOT_NULL_OUTPUT);
}

int main(void)
{
    golden();
    zeroOptionalPairs();
    refusalsAtomic();

    std::printf("\nGA107 cold-boot WPR compose: %d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
