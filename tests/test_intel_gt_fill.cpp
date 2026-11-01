// Host-only golden tests for the production Tiger Lake BCS0 fill encoder.
#include <cstdio>
#include <cstring>
#include "IntelGtBlitter.hpp"
#include "IntelGtBlitABI.h"

static int checks=0;
static int failures=0;
static void checkTrue(bool ok,const char *what) {
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
    checkTrue(sizeof(IgGtFill32Request)==5u*sizeof(unsigned int),"fill request stays pointer-free/fixed");
    checkTrue(sizeof(IgGtFill32Response)==IGGT_FILL32_MAX_BYTES,"fill response is exact fixed surface span");
}

static void testGoldenFill() {
    std::puts("2. exact fenced XY_COLOR_BLT image");
    unsigned int batch[16];
    for (auto &v:batch) v=0xdeadbeefu;
    const GTBlitSurface32 dst={0x00012000u,4096u,256u};
    checkTrue(gtEncodeFill32(batch,16,dst,64u,8u,0xaabbccddu,0x00013000u,0x12345678u),
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

static void checkAtomicRefusal(const GTBlitSurface32 &dst,unsigned int width,
                                unsigned int height,unsigned int fence,
                                unsigned int sequence,unsigned int capacity,
                                const char *what) {
    unsigned int batch[16];
    for (auto &v:batch) v=0x5a5aa5a5u;
    unsigned int before[16];
    std::memcpy(before,batch,sizeof(batch));
    checkTrue(!gtEncodeFill32(batch,capacity,dst,width,height,0x11223344u,fence,sequence),what);
    checkTrue(std::memcmp(batch,before,sizeof(batch))==0,"refusal leaves batch untouched");
}

static void testRefusals() {
    std::puts("3. atomic refusal edges");
    const GTBlitSurface32 good={0x00012000u,4096u,256u};
    checkAtomicRefusal(good,64u,8u,0x00013004u,1u,16u,"unaligned fence refused");
    checkAtomicRefusal(good,64u,8u,0x00013000u,0u,16u,"zero sequence refused");
    checkAtomicRefusal(good,64u,8u,0x00012000u,1u,16u,"overlapping fence refused");
    checkAtomicRefusal(good,64u,8u,0x00013000u,1u,15u,"short batch refused");
    const GTBlitSurface32 shortDst={0x00012000u,1024u,256u};
    checkAtomicRefusal(shortDst,64u,8u,0x00013000u,1u,16u,"short destination refused");
    const GTBlitSurface32 badPitch={0x00012000u,4096u,252u};
    checkAtomicRefusal(badPitch,64u,8u,0x00013000u,1u,16u,"row wider than pitch refused");
    // The remaining guards in gtEncodeFill32 all run before the BLT image is
    // built, so each must refuse without touching the caller's batch.
    checkAtomicRefusal(good,0u,8u,0x00013000u,1u,16u,"zero width refused");
    checkAtomicRefusal(good,64u,0u,0x00013000u,1u,16u,"zero height refused");
    checkAtomicRefusal(good,16385u,8u,0x00013000u,1u,16u,"width over 16384 refused");
    checkAtomicRefusal(good,64u,16385u,0x00013000u,1u,16u,"height over 16384 refused");
    const GTBlitSurface32 oddAddress={0x00012002u,4096u,256u};
    checkAtomicRefusal(oddAddress,64u,8u,0x00013000u,1u,16u,"unaligned destination refused");
    const GTBlitSurface32 oddPitch={0x00012000u,4096u,258u};
    checkAtomicRefusal(oddPitch,64u,8u,0x00013000u,1u,16u,"unaligned pitch refused");
    // Eight 32 KiB rows fit in this span; only the pitch ceiling refuses.
    const GTBlitSurface32 widePitch={0x00100000u,0x00040000u,32768u};
    checkAtomicRefusal(widePitch,64u,8u,0x00013000u,1u,16u,"pitch over 32767 refused");
    const GTBlitSurface32 past4G={0xfffff000u,8192u,256u};
    checkAtomicRefusal(past4G,64u,8u,0x00013000u,1u,16u,"surface past 4 GiB refused");
    const GTBlitSurface32 abovePage={0x00012800u,4096u,256u};
    checkAtomicRefusal(abovePage,64u,8u,0x00012000u,1u,16u,"fence overlapping from below refused");
    checkTrue(!gtEncodeFill32(nullptr,16,good,64u,8u,0x11223344u,0x00013000u,1u),
              "null batch refused");
}

static void testFenceBoundary() {
    std::puts("4. half-open fence boundary");
    // A fence page ending exactly where the surface starts does not overlap it.
    const GTBlitSurface32 dst={0x00012000u,4096u,256u};
    unsigned int batch[16];
    for (auto &v:batch) v=0xdeadbeefu;
    checkTrue(gtEncodeFill32(batch,16,dst,64u,8u,0xaabbccddu,0x00011000u,1u),
              "fence ending at surface start accepted");
    checkU32(batch[9],0x00011000u,"abutting fence address encoded");
}

int main() {
    testAbi();
    testGoldenFill();
    testRefusals();
    testFenceBoundary();
    std::printf("\n%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
