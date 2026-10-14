// Host tests for NVGspWprImage.hpp. No IOKit, VRAM or firmware execution.
#include <cstdio>
#include <cstring>
#include "NVGspWprImage.hpp"

static int gChecks=0, gFailures=0;
static void check(const char *label, bool ok) {
    ++gChecks; if (!ok) { ++gFailures; std::printf("  FAIL %s\n",label); }
}
static void checkU32(const char *label, NVGspWprU32 got, NVGspWprU32 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%08x want 0x%08x\n",label,got,want); }
}
static void checkU64(const char *label, NVGspWprU64 got, NVGspWprU64 want) {
    ++gChecks; if (got!=want) { ++gFailures; std::printf("  FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want); }
}

static NVGspWprLayoutInput layoutInput(void) {
    NVGspWprLayoutInput in={};
    in.fbSize=0x0000000200000000ull;
    in.vbiosReservedOffset=0x00000001ff000000ull;
    in.wprEndMargin=0x00200000ull;
    in.frtsSize=0x00200000ull;
    in.bootloaderSize=0x00180000ull;
    in.radix3ElfSize=0x03400000ull;
    in.nonWprHeapRawBytes=0x00180000ull;
    in.wprHeapBytes=0x02000000ull;
    return in;
}

static NVGspWprImageInput imageInput(void) {
    NVGspWprImageInput in={};
    const NVGspWprLayoutInput li=layoutInput();
    (void)nvGspPlanWprLayout(&li,&in.layout);
    in.framebufferBytes=li.fbSize;
    in.radix3ElfGpuAddress=0x0000001800000000ull;
    in.radix3ElfBytes=li.radix3ElfSize;
    in.bootloaderGpuAddress=0x0000001900000000ull;
    in.bootloaderBytes=li.bootloaderSize;
    in.bootloaderCodeOffset=0x00001000ull;
    in.bootloaderDataOffset=0x00080000ull;
    in.bootloaderManifestOffset=0x00000400ull;
    in.signatureGpuAddress=0x0000001a00000000ull;
    in.signatureBytes=0x00001000ull;
    in.vgaWorkspaceOffset=0x00000001ff000000ull;
    in.vgaWorkspaceBytes=0x01000000ull;
    in.crashQueueGpuAddress=0x0000001b00000000ull;
    in.crashQueueBytes=0x00010000ull;
    in.vfPartitionCount=0u;
    in.flags=NV_GSP_WPR_FLAG_CLOCK_BOOST;
    in.pagingConfig=0x0003u;
    return in;
}

static void testGolden(void) {
    std::printf("\n1. exact cold-boot WPR metadata image\n");
    const NVGspWprImageInput in=imageInput();
    NVGspWprMetaV1 m={};
    checkU32("verdict",nvGspBuildColdBootWprMeta(&in,&m),NV_GSP_WPR_IMAGE_OK);
    checkU64("magic",m.magic,NV_GSP_WPR_META_MAGIC);
    checkU64("revision",m.revision,NV_GSP_WPR_META_REVISION);
    checkU64("ELF address",m.sysmemAddrOfRadix3Elf,in.radix3ElfGpuAddress);
    checkU64("ELF bytes",m.sizeOfRadix3Elf,in.radix3ElfBytes);
    checkU64("boot address",m.sysmemAddrOfBootloader,in.bootloaderGpuAddress);
    checkU64("boot bytes",m.sizeOfBootloader,in.bootloaderBytes);
    checkU64("code offset",m.bootloaderCodeOffset,in.bootloaderCodeOffset);
    checkU64("data offset",m.bootloaderDataOffset,in.bootloaderDataOffset);
    checkU64("manifest offset",m.bootloaderManifestOffset,in.bootloaderManifestOffset);
    checkU64("signature address",m.sysmemAddrOfSignature,in.signatureGpuAddress);
    checkU64("signature bytes",m.sizeOfSignature,in.signatureBytes);
    checkU64("reserved start",m.gspFwRsvdStart,in.layout.gspFwRsvdStart);
    checkU64("non-WPR offset",m.nonWprHeapOffset,in.layout.nonWprHeapOffset);
    checkU64("non-WPR size",m.nonWprHeapSize,in.layout.nonWprHeapSize);
    checkU64("WPR start",m.gspFwWprStart,in.layout.gspFwWprStart);
    checkU64("heap offset",m.gspFwHeapOffset,in.layout.gspFwHeapOffset);
    checkU64("heap size",m.gspFwHeapSize,in.layout.gspFwHeapSize);
    checkU64("FW offset",m.gspFwOffset,in.layout.gspFwOffset);
    checkU64("boot-bin offset",m.bootBinOffset,in.layout.bootBinOffset);
    checkU64("FRTS offset",m.frtsOffset,in.layout.frtsOffset);
    checkU64("FRTS size",m.frtsSize,0x00200000ull);
    checkU64("WPR end",m.gspFwWprEnd,in.layout.gspFwWprEnd);
    checkU64("FB bytes",m.fbSize,in.framebufferBytes);
    checkU64("VGA offset",m.vgaWorkspaceOffset,in.vgaWorkspaceOffset);
    checkU64("VGA bytes",m.vgaWorkspaceSize,in.vgaWorkspaceBytes);
    checkU64("boot count cold",m.bootCount,0ull);
    checkU64("CrashCat address",m.sysmemAddrOfCrashReportQueue,in.crashQueueGpuAddress);
    checkU32("CrashCat bytes",m.sizeOfCrashReportQueue,(NVGspWprU32)in.crashQueueBytes);
    checkU32("VF count",m.gspFwHeapVfPartitionCount,0u);
    checkU32("flags",m.flags,NV_GSP_WPR_FLAG_CLOCK_BOOST);
    checkU32("paging config",m.pagingConfig,0x0003u);
    checkU64("Booter verification not forged",m.verified,0ull);
    for (unsigned i=0;i<sizeof(m.reserved1);++i) check("reserved1 zero",m.reserved1[i]==0u);
    checkU32("reserved2 zero",m.reserved2,0u);
    checkU32("reserved3 zero",m.reserved3,0u);
}

static void testOptionalFields(void) {
    std::printf("\n2. optional signature and CrashCat pairs may both be absent\n");
    NVGspWprImageInput in=imageInput();
    in.signatureGpuAddress=0ull;
    in.signatureBytes=0ull;
    in.crashQueueGpuAddress=0ull;
    in.crashQueueBytes=0ull;
    NVGspWprMetaV1 m={};
    checkU32("verdict",nvGspBuildColdBootWprMeta(&in,&m),NV_GSP_WPR_IMAGE_OK);
    checkU64("signature address zero",m.sysmemAddrOfSignature,0ull);
    checkU64("signature size zero",m.sizeOfSignature,0ull);
    checkU64("CrashCat address zero",m.sysmemAddrOfCrashReportQueue,0ull);
    checkU32("CrashCat size zero",m.sizeOfCrashReportQueue,0u);
    checkU64("verified still zero",m.verified,0ull);
}

static void testFlags(void) {
    std::printf("\n3. public V1 flag combinations are preserved exactly\n");
    NVGspWprImageInput in=imageInput();
    in.flags=(NVGspWprU8)(NV_GSP_WPR_FLAG_CLOCK_BOOST|
                          NV_GSP_WPR_FLAG_PPCIE_ENABLED|
                          NV_GSP_WPR_FLAG_MULTI_GPU_NVLE_ENABLED);
    NVGspWprMetaV1 m={};
    checkU32("verdict",nvGspBuildColdBootWprMeta(&in,&m),NV_GSP_WPR_IMAGE_OK);
    checkU32("all known flags",m.flags,NV_GSP_WPR_FLAG_KNOWN_MASK);
}

static void testAtomicRefusals(void) {
    std::printf("\n4. malformed cold-boot inputs refuse atomically\n");
    NVGspWprMetaV1 poison;
    std::memset(&poison,0xa5,sizeof(poison));
    const NVGspWprMetaV1 before=poison;
    NVGspWprImageInput in=imageInput();

    checkU32("null input",nvGspBuildColdBootWprMeta(nullptr,&poison),NV_GSP_WPR_IMAGE_NULL_INPUT);
    check("null input atomic",std::memcmp(&poison,&before,sizeof(poison))==0);
    checkU32("null output",nvGspBuildColdBootWprMeta(&in,nullptr),NV_GSP_WPR_IMAGE_NULL_OUTPUT);

    in=imageInput(); in.layout.gspFwWprEnd=in.framebufferBytes+0x20000ull;
    checkU32("bad layout",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_LAYOUT);

    in=imageInput(); in.radix3ElfGpuAddress=0ull;
    checkU32("missing ELF address",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_FIRMWARE);

    in=imageInput(); in.radix3ElfBytes=in.layout.bootBinOffset-in.layout.gspFwOffset+1ull;
    checkU32("ELF exceeds placement",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_FIRMWARE);

    in=imageInput(); in.bootloaderGpuAddress=0ull;
    checkU32("missing boot address",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_BOOTLOADER);

    in=imageInput(); in.bootloaderManifestOffset=in.bootloaderBytes;
    checkU32("bad manifest offset",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_BOOT_OFFSETS);

    in=imageInput(); in.signatureBytes=0ull;
    checkU32("half signature pair",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_SIGNATURE);

    in=imageInput(); in.vgaWorkspaceBytes--;
    checkU32("bad VGA extent",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_VGA_WORKSPACE);

    in=imageInput(); in.crashQueueGpuAddress=0ull;
    checkU32("half CrashCat pair",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_CRASH_QUEUE);

    in=imageInput(); in.crashQueueBytes=0x100000000ull;
    checkU32("CrashCat size overflow",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_CRASH_QUEUE);

    in=imageInput(); in.flags=0x02u;
    checkU32("unknown flag",nvGspBuildColdBootWprMeta(&in,&poison),NV_GSP_WPR_IMAGE_BAD_FLAGS);
    check("all refusals atomic",std::memcmp(&poison,&before,sizeof(poison))==0);
}

static void testVerdicts(void) {
    std::printf("\n5. verdict family\n");
    for (NVGspWprU32 v=0u;v<NV_GSP_WPR_IMAGE_LIMIT;++v) {
        check("named",std::strcmp(nvGspWprImageVerdictName(v),"unknown")!=0);
        check("success",nvGspWprImageSucceeded(v)==(v==NV_GSP_WPR_IMAGE_OK));
    }
}

int main(void) {
    std::printf("NVGspWprImage.hpp -- cold-boot GA107 WPR metadata image\n");
    testGolden();
    testOptionalFields();
    testFlags();
    testAtomicRefusals();
    testVerdicts();
    std::printf("\n%d checks, %d failures\n",gChecks,gFailures);
    return gFailures?1:0;
}
