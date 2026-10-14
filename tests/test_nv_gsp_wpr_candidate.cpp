#include <cstdio>
#include <cstring>

#include "NVGspWprCandidate.hpp"

static int gChecks=0;
static int gFailures=0;

static void checkU32(const char *label, unsigned int got, unsigned int want)
{
    ++gChecks;
    if (got!=want) {
        ++gFailures;
        std::printf("FAIL %s: got 0x%08x want 0x%08x\n",label,got,want);
    }
}

static void checkU64(const char *label, unsigned long long got,
                     unsigned long long want)
{
    ++gChecks;
    if (got!=want) {
        ++gFailures;
        std::printf("FAIL %s: got 0x%016llx want 0x%016llx\n",label,got,want);
    }
}

static void checkBool(const char *label, bool got, bool want)
{
    ++gChecks;
    if (got!=want) {
        ++gFailures;
        std::printf("FAIL %s: got %u want %u\n",label,(unsigned)got,(unsigned)want);
    }
}

static NVGspWprCandidateInput goodInput(void)
{
    NVGspWprCandidateInput in={};
    in.factsPrepared=true;
    in.facts.fbSizeBytes=0x0000000100000000ull; // arithmetic fixture: 4 GiB
    in.facts.displaySupported=true;
    in.facts.vgaWorkspaceRegisterValid=true;
    in.facts.vgaWorkspaceRegisterOffset=0x00000000fff80000ull;
    in.facts.vgaWorkspacePolicyOffset=0x00000000fff80000ull;
    in.facts.mmuLockValid=false;
    in.facts.mmuLockLo=0ull;

    in.sysmemPrepared=true;
    in.sysmem.radix3RootGpuAddress=0x0000001200000000ull;
    in.sysmem.radix3ImageBytes=0x02000000ull;
    in.sysmem.bootGpuAddress=0x0000001300000000ull;
    in.sysmem.bootImageBytes=0x00006000ull;
    in.sysmem.bootCodeOffset=0x00001800ull;
    in.sysmem.bootDataOffset=0x00000800ull;
    in.sysmem.bootManifestOffset=0x00000000ull;
    in.sysmem.valid=true;

    // Explicit default cold-boot optional fields.
    in.signatureGpuAddress=0ull;
    in.signatureBytes=0ull;
    in.crashQueueGpuAddress=0ull;
    in.crashQueueBytes=0ull;
    in.vfPartitionCount=0u;
    in.flags=0u;
    in.pagingConfig=0u;
    return in;
}

static NVGspWprCandidatePlan poisonPlan(void)
{
    NVGspWprCandidatePlan p;
    std::memset(&p,0xa5,sizeof(p));
    return p;
}

static void checkAtomic(const char *label,
                        const NVGspWprCandidatePlan &got,
                        const NVGspWprCandidatePlan &want)
{
    ++gChecks;
    if (std::memcmp(&got,&want,sizeof(got))!=0) {
        ++gFailures;
        std::printf("FAIL %s: refusal modified output\n",label);
    }
}

int main(void)
{
    NVGspWprCandidateInput in=goodInput();
    NVGspWprCandidatePlan out={};
    checkU32("golden verdict",nvGspPlanGa107WprCandidate(&in,&out),
             NV_GSP_WPR_CANDIDATE_OK);
    checkBool("golden valid",out.valid,true);

    checkU64("magic",out.metadata.magic,NV_GSP_WPR_META_MAGIC);
    checkU64("revision",out.metadata.revision,NV_GSP_WPR_META_REVISION);
    checkU64("radix3 address",out.metadata.sysmemAddrOfRadix3Elf,
             in.sysmem.radix3RootGpuAddress);
    checkU64("radix3 bytes",out.metadata.sizeOfRadix3Elf,
             in.sysmem.radix3ImageBytes);
    checkU64("boot address",out.metadata.sysmemAddrOfBootloader,
             in.sysmem.bootGpuAddress);
    checkU64("boot bytes",out.metadata.sizeOfBootloader,
             in.sysmem.bootImageBytes);
    checkU64("code offset",out.metadata.bootloaderCodeOffset,
             in.sysmem.bootCodeOffset);
    checkU64("data offset",out.metadata.bootloaderDataOffset,
             in.sysmem.bootDataOffset);
    checkU64("manifest offset",out.metadata.bootloaderManifestOffset,
             in.sysmem.bootManifestOffset);
    checkU64("framebuffer bytes",out.metadata.fbSize,in.facts.fbSizeBytes);
    checkU64("workspace offset",out.metadata.vgaWorkspaceOffset,
             in.facts.vgaWorkspacePolicyOffset);
    checkU64("workspace bytes",out.metadata.vgaWorkspaceSize,0x00080000ull);
    checkU64("verified remains Booter-owned",out.metadata.verified,0ull);

    // Policy composition must agree with the image's layout fields.
    checkU64("WPR end mirrors policy",out.metadata.gspFwWprEnd,
             out.ga107.layout.gspFwWprEnd);
    checkU64("WPR start mirrors policy",out.metadata.gspFwWprStart,
             out.ga107.layout.gspFwWprStart);
    checkU64("GSP FW offset mirrors policy",out.metadata.gspFwOffset,
             out.ga107.layout.gspFwOffset);
    checkU64("boot bin mirrors policy",out.metadata.bootBinOffset,
             out.ga107.layout.bootBinOffset);
    checkU64("FRTS offset mirrors policy",out.metadata.frtsOffset,
             out.ga107.layout.frtsOffset);
    checkU64("FRTS size is policy constant",out.metadata.frtsSize,
             NV_GSP_GA107_FRTS_BYTES);
    checkU64("non-WPR heap is policy constant",out.ga107.layoutInput.nonWprHeapRawBytes,
             NV_GSP_GA107_NON_WPR_HEAP_BYTES);
    checkBool("heap nonzero",out.ga107.heap.heapBytes!=0ull,true);

    // Optional fields remain explicit. A valid pair is copied exactly.
    in=goodInput();
    in.signatureGpuAddress=0x0000001400000000ull;
    in.signatureBytes=0x1000ull;
    in.crashQueueGpuAddress=0x0000001500000000ull;
    in.crashQueueBytes=0x2000ull;
    in.flags=NV_GSP_WPR_FLAG_CLOCK_BOOST;
    in.pagingConfig=0x1234u;
    out={};
    checkU32("optional fields accepted",nvGspPlanGa107WprCandidate(&in,&out),
             NV_GSP_WPR_CANDIDATE_OK);
    checkU64("signature address",out.metadata.sysmemAddrOfSignature,
             in.signatureGpuAddress);
    checkU64("signature bytes",out.metadata.sizeOfSignature,in.signatureBytes);
    checkU64("crash address",out.metadata.sysmemAddrOfCrashReportQueue,
             in.crashQueueGpuAddress);
    checkU32("crash bytes",out.metadata.sizeOfCrashReportQueue,
             (unsigned int)in.crashQueueBytes);
    checkU32("paging config",out.metadata.pagingConfig,in.pagingConfig);

    // Every refusal below must be atomic.
    NVGspWprCandidatePlan poison=poisonPlan();
    NVGspWprCandidatePlan before=poison;
    checkU32("null input",nvGspPlanGa107WprCandidate(nullptr,&poison),
             NV_GSP_WPR_CANDIDATE_NULL_INPUT);
    checkAtomic("null input atomic",poison,before);

    in=goodInput(); in.factsPrepared=false; poison=before;
    checkU32("facts missing",nvGspPlanGa107WprCandidate(&in,&poison),
             NV_GSP_WPR_CANDIDATE_FACTS_MISSING);
    checkAtomic("facts missing atomic",poison,before);

    in=goodInput(); in.sysmemPrepared=false; poison=before;
    checkU32("sysmem missing",nvGspPlanGa107WprCandidate(&in,&poison),
             NV_GSP_WPR_CANDIDATE_SYSMEM_MISSING);
    checkAtomic("sysmem missing atomic",poison,before);

    in=goodInput(); in.facts.vgaWorkspacePolicyOffset=0ull; poison=before;
    checkU32("bad facts",nvGspPlanGa107WprCandidate(&in,&poison),
             NV_GSP_WPR_CANDIDATE_BAD_FACTS);
    checkAtomic("bad facts atomic",poison,before);

    in=goodInput(); in.sysmem.valid=false; poison=before;
    checkU32("bad sysmem",nvGspPlanGa107WprCandidate(&in,&poison),
             NV_GSP_WPR_CANDIDATE_BAD_SYSMEM);
    checkAtomic("bad sysmem atomic",poison,before);

    in=goodInput(); in.signatureGpuAddress=0x0000001400000000ull; poison=before;
    checkU32("signature pair mismatch",nvGspPlanGa107WprCandidate(&in,&poison),
             NV_GSP_WPR_CANDIDATE_IMAGE_REFUSED);
    checkAtomic("signature mismatch atomic",poison,before);

    in=goodInput(); in.flags=0x80u; poison=before;
    checkU32("unknown flags",nvGspPlanGa107WprCandidate(&in,&poison),
             NV_GSP_WPR_CANDIDATE_IMAGE_REFUSED);
    checkAtomic("unknown flags atomic",poison,before);

    // A huge image that cannot be placed under the observed VBIOS boundary is
    // a policy refusal, not a malformed sysmem descriptor.
    in=goodInput(); in.sysmem.radix3ImageBytes=in.facts.fbSizeBytes; poison=before;
    checkU32("layout policy refusal",nvGspPlanGa107WprCandidate(&in,&poison),
             NV_GSP_WPR_CANDIDATE_POLICY_REFUSED);
    checkAtomic("policy refusal atomic",poison,before);

    std::printf("GA107 WPR candidate: %d checks, %d failures\n",gChecks,gFailures);
    return gFailures ? 1 : 0;
}
