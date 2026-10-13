// SPDX-License-Identifier: MIT
// Fixed-size, pointer-free v1 userspace ABI. No raw GPU address or command input.
#ifndef IntelGtBlitABI_h
#define IntelGtBlitABI_h
#define IGGT_COPY32_SELECTOR 0
#define IGGT_COPY32_MAX_BYTES 1024
struct IgGtCopy32Request {
    unsigned int width, height, pitch, bytes;
    unsigned char pixels[IGGT_COPY32_MAX_BYTES];
};
struct IgGtCopy32Response {
    unsigned char pixels[IGGT_COPY32_MAX_BYTES];
};
#endif
