// SPDX-License-Identifier: MIT
// Production Gen12 blitter command encoder; no IOKit or host shim dependency.
#ifndef IntelGtBlitter_hpp
#define IntelGtBlitter_hpp
using GTBlitU32 = unsigned int;
using GTBlitU64 = unsigned long long;
struct GTBlitSurface32 {
    GTBlitU32 address; // private PPGTT byte address, not a CPU/physical pointer
    GTBlitU32 bytes;
    GTBlitU32 pitch;
};
// Emits a ten-DW fast copy, a four-DW flushing GPU fence, BBE and a NOOP.
// The caller owns both surfaces and the page-aligned fence allocation.
bool gtEncodeCopy32(GTBlitU32 *batch, GTBlitU32 capacityDwords,
                    const GTBlitSurface32 &source, const GTBlitSurface32 &dest,
                    GTBlitU32 width, GTBlitU32 height,
                    GTBlitU32 fenceAddress, GTBlitU32 sequence);
#endif
