// SPDX-License-Identifier: MIT
#include "IntelGtBlitter.hpp"
#include "IntelGtBlt.hpp"
namespace {
bool fits(const GTBlitSurface32 &s, GTBlitU32 width, GTBlitU32 height) {
    if (!s.bytes || !s.pitch || s.pitch>32767 || (s.pitch & 3) || (s.address & 3)) return false;
    GTBlitU64 row=(GTBlitU64)width*4;
    GTBlitU64 used=(GTBlitU64)(height-1)*s.pitch+row;
    return row<=s.pitch && used<=s.bytes && (GTBlitU64)s.address+s.bytes<=0x100000000ULL;
}
bool fenceSeparate(const GTBlitSurface32 &s, GTBlitU32 fence) {
    const GTBlitU64 surfaceEnd=(GTBlitU64)s.address+s.bytes;
    const GTBlitU64 fenceEnd=(GTBlitU64)fence+4096ull;
    return fenceEnd<=0x100000000ULL &&
           !((GTBlitU64)fence<surfaceEnd && (GTBlitU64)s.address<fenceEnd);
}
}
bool gtEncodeCopy32(GTBlitU32 *b, GTBlitU32 capacity,
                    const GTBlitSurface32 &src, const GTBlitSurface32 &dst,
                    GTBlitU32 width, GTBlitU32 height,
                    GTBlitU32 fence, GTBlitU32 sequence) {
    if (!b || capacity<16 || !width || !height || width>16384 || height>16384 ||
        !sequence || (fence & 4095) || !fits(src,width,height) || !fits(dst,width,height)) return false;
    GTBlitU64 srcEnd=(GTBlitU64)src.address+src.bytes;
    GTBlitU64 dstEnd=(GTBlitU64)dst.address+dst.bytes;
    if ((src.address<dstEnd && dst.address<srcEnd) ||
        !fenceSeparate(src,fence) || !fenceSeparate(dst,fence)) return false;
    b[0]=(2u<<29)|(0x42u<<22)|8; // GEN9_XY_FAST_COPY_BLT, linear
    b[1]=(3u<<24)|dst.pitch; // 32 bpp
    b[2]=0; b[3]=(height<<16)|width;
    b[4]=dst.address; b[5]=0; b[6]=0; b[7]=src.pitch;
    b[8]=src.address; b[9]=0;
    // Do not OR 2 into MI_FLUSH_DW's existing length=1: that encodes 5 DW!
    b[10]=(0x26u<<23)|(1u<<14)|2; // 4 DW, post-sync STORE_DWORD, private PPGTT
    b[11]=fence; b[12]=0; b[13]=sequence;
    b[14]=(0x0au<<23); b[15]=0;
    return true;
}

bool gtEncodeFill32(GTBlitU32 *b, GTBlitU32 capacity,
                    const GTBlitSurface32 &dst,
                    GTBlitU32 width, GTBlitU32 height, GTBlitU32 color,
                    GTBlitU32 fence, GTBlitU32 sequence) {
    if (!b || capacity<16 || !width || !height || width>16384 || height>16384 ||
        !sequence || (fence & 4095) || !fits(dst,width,height) ||
        !fenceSeparate(dst,fence)) return false;

    // Build into a temporary image so a source-backed BLT refusal cannot leave
    // a partially executable batch in driver-owned command memory.
    GTBlitU32 tmp[16]={};
    IGU32 blitWords=0;
    const IGU32 verdict=igGtbWriteColorBlit(
        tmp,16,(IGU64)dst.address,dst.pitch,
        0,0,width,height,color,IG_GTB_DEPTH_32_I915_0x03000000,&blitWords);
    if (!igGtbBlitSucceeded(verdict) || blitWords!=IG_GTB_XY_COLOR_BLT_DWORDS_POLICY_7)
        return false;

    // The BLT packet is seven dwords. Pad once so the flushing fence starts on
    // a qword boundary, then use the exact fence contract already proven by the
    // synchronous copy path. MI_NOOP is zero.
    tmp[7]=0;
    tmp[8]=(0x26u<<23)|(1u<<14)|2; // MI_FLUSH_DW, post-sync STORE_DWORD, PPGTT
    tmp[9]=fence;
    tmp[10]=0;
    tmp[11]=sequence;
    tmp[12]=(0x0au<<23); // MI_BATCH_BUFFER_END
    tmp[13]=0;
    tmp[14]=0;
    tmp[15]=0;

    for (GTBlitU32 i=0;i<16;++i) b[i]=tmp[i];
    return true;
}
