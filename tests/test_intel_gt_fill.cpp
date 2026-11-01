// Host-only golden tests for the production Tiger Lake BCS0 fill encoder.
#include <cstdio>
#include <cstring>
#include "IntelGtBlitter.hpp"
#include "IntelGtBlitABI.h"

static int checks=0;
static int failures=0;
static void check(bool ok,const char *what) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n",what); ++failures; }
}
static void checkU32(unsigned int got,unsigned int want,const char *what) {
    ++checks;
    if (got!=want) {
        std::printf("FAIL: %s got=%08x want=%08x\n",what,got,want);
        ++failures;
    }
}

static void testAbi() {
    std::puts("1. fixed fill ABI");
    checkU32(IGGT_FILL32_SELECTOR,1u,"fill selector");
    checkU32(IGGT_FILL32_ABI_VERSION,1u,"fill ABI version");
    checkU32(IGGT_FILL32_MAX_BYTES,65536u,"fill ABI byte ceiling");
    check(sizeof(IgGtFill32Request)==5u*sizeof(unsigned int),"fill request stays pointer-free/fixed");
    check(sizeof(IgGtFill32Response)==IGGT_FILL32_MAX_BYTES,"fill response is exact fixed surface span");
}

static void testGoldenFill() {
    std::puts("2. exact fenced XY_COLOR_BLT image");
    unsigned int batch[16];
    for (auto &v:batch) v=0xdeadbeefu;
    const GTBlitSurface32 dst={0x00012000u,4096u,256u};
    check(gtEncodeFill32(batch,16,dst,64u,8u,0xaabbccddu,0x00013000u,0x12345678u),
          "golden fill accepted");
    const unsigned int want[16]={
        0x54300005u, // XY_COLOR_BLT + RGBA writes + 7-DW length
        0x03f00100u, // COLOR_COPY + 32bpp + pitch 256
        0x00000000u, // top-left 0,0
        0x00080040u, // bottom-right x=64,y=8
        0x00012000u,0x00000000u, // destination PPGTT address
        0xaabbccddu, // fill colour
        0x00000000u, // alignment NOOP
        0x13004002u, // MI_FLUSH_DW + post-sync STORE_DWORD + PPGTT
        0x00013000u,0x00000000u,0x12345678u,
        0x05000000u, // MI_BATCH_BUFFER_END
        0x00000000u,0x00000000u,0x00000000u
    };
    for (unsigned int i=0;i<16;++i) {
        char label[48];
        std::snprintf(label,sizeof(label),"golden dword %u",i);
        checkU32(batch[i],want[i],label);
    }
}

static void expectAtomicRefusal(const GTBlitSurface32 &dst,unsigned int width,
                                unsigned int height,unsigned int fence,
                                unsigned int sequence,unsigned int capacity,
                                const char *what) {
    unsigned int batch[16];
    for (auto &v:batch) v=0x5a5aa5a5u;
    unsigned int before[16];
    std::memcpy(before,batch,sizeof(batch));
    check(!gtEncodeFill32(batch,capacity,dst,width,height,0x11223344u,fence,sequence),what);
    check(std::memcmp(batch,before,sizeof(batch))==0,"refusal leaves batch untouched");
}

static void testRefusals() {
    std::puts("3. atomic refusal edges");
    const GTBlitSurface32 good={0x00012000u,4096u,256u};
    expectAtomicRefusal(good,64u,8u,0x00013004u,1u,16u,"unaligned fence refused");
    expectAtomicRefusal(good,64u,8u,0x00013000u,0u,16u,"zero sequence refused");
    expectAtomicRefusal(good,64u,8u,0x00012000u,1u,16u,"overlapping fence refused");
    expectAtomicRefusal(good,64u,8u,0x00013000u,1u,15u,"short batch refused");
    const GTBlitSurface32 shortDst={0x00012000u,1024u,256u};
    expectAtomicRefusal(shortDst,64u,8u,0x00013000u,1u,16u,"short destination refused");
    const GTBlitSurface32 badPitch={0x00012000u,4096u,252u};
    expectAtomicRefusal(badPitch,64u,8u,0x00013000u,1u,16u,"row wider than pitch refused");
}

int main() {
    testAbi();
    testGoldenFill();
    testRefusals();
    std::printf("\n%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
