// SPDX-License-Identifier: MIT
// Fixed-size, pointer-free userspace ABI. No raw GPU address or command input.
#ifndef IntelGtBlitABI_h
#define IntelGtBlitABI_h
#define IGGT_COPY32_SELECTOR 0
#define IGGT_FILL32_SELECTOR 1
#define IGGT_SURFACE_ALLOC32_SELECTOR 2
#define IGGT_SURFACE_FREE_SELECTOR 3
#define IGGT_SURFACE_FILL32_SELECTOR 4
#define IGGT_SURFACE_COPY32_SELECTOR 5
#define IGGT_SCANOUT_FILL32_SELECTOR 6
#define IGGT_COPY32_ABI_VERSION 2
#define IGGT_FILL32_ABI_VERSION 1
#define IGGT_SURFACE32_ABI_VERSION 1
#define IGGT_SCANOUT32_ABI_VERSION 1
#define IGGT_COPY32_MAX_BYTES (64u * 1024u)
#define IGGT_FILL32_MAX_BYTES IGGT_COPY32_MAX_BYTES
struct IgGtCopy32Request {
    unsigned int width, height, pitch, bytes;
    unsigned char pixels[IGGT_COPY32_MAX_BYTES];
};
struct IgGtCopy32Response {
    unsigned char pixels[IGGT_COPY32_MAX_BYTES];
};
struct IgGtFill32Request {
    unsigned int width, height, pitch, bytes, color;
};
struct IgGtFill32Response {
    unsigned char pixels[IGGT_FILL32_MAX_BYTES];
};

// Persistent-surface ABI. The only surface identity that crosses the user/kernel
// boundary is the opaque generation-tagged 64-bit handle. No arena page index,
// PPGTT address, IOVA, DMA address or CPU pointer is ever returned.
struct IgGtSurfaceAlloc32Request {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int reserved;
};
struct IgGtSurfaceAlloc32Response {
    unsigned long long handle;
};
struct IgGtSurfaceFreeRequest {
    unsigned long long handle;
};
struct IgGtSurfaceFreeResponse {
    unsigned int released;
    unsigned int reserved;
};
struct IgGtSurfaceFill32Request {
    unsigned long long handle;
    unsigned int color;
    unsigned int reserved;
};
struct IgGtSurfaceFill32Response {
    unsigned int completed;
    unsigned int reserved;
};
struct IgGtSurfaceCopy32Request {
    unsigned long long sourceHandle;
    unsigned long long destinationHandle;
};
struct IgGtSurfaceCopy32Response {
    unsigned int completed;
    unsigned int reserved;
};

// Inherited-scanout fill (GT-1k, t-619d61). x, y, width and height are pixels
// of the pipe A plane 1 surface sampled at iggt-bringup=4. No address crosses
// the boundary. The kernel refuses unless that sample was IG_PRESENT_OK and
// x+width and y+height fit inside it. reserved must be zero.
struct IgGtScanoutFill32Request {
    unsigned int x;
    unsigned int y;
    unsigned int width;
    unsigned int height;
    unsigned int color;
    unsigned int reserved;
};
struct IgGtScanoutFill32Response {
    unsigned int completed;
    unsigned int reserved;
};
#endif
