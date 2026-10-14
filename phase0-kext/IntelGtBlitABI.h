// SPDX-License-Identifier: MIT
// Fixed-size, pointer-free userspace ABI. No raw GPU address or command input.
#ifndef IntelGtBlitABI_h
#define IntelGtBlitABI_h
#define IGGT_COPY32_SELECTOR 0
#define IGGT_FILL32_SELECTOR 1
#define IGGT_COPY32_ABI_VERSION 2
#define IGGT_FILL32_ABI_VERSION 1
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
#endif
