// SPDX-License-Identifier: MIT
// macOS user-space client: performs an actual GPU copy, never a CPU fallback.
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include "IntelGtBlitABI.h"
static bool number(const char *s, unsigned int &out) {
    char *end=nullptr; errno=0;
    unsigned long value=strtoul(s,&end,10);
    if (errno || !*s || *end || !value || value>1024) return false;
    out=(unsigned int)value; return true;
}
int main(int argc, char **argv) {
    if (argc!=6) {
        std::fprintf(stderr,"Usage: iggt-blit input.bgra output.bgra width height pitch\n"
                            "Maximum surface: %u bytes. Requires administrator and iggt-bringup=3.\n",
                            (unsigned)IGGT_COPY32_MAX_BYTES);
        return 2;
    }
    IgGtCopy32Request request={}; IgGtCopy32Response response={};
    if (!number(argv[3],request.width) || !number(argv[4],request.height) ||
        !number(argv[5],request.pitch) || (request.pitch & 3) || request.width*4>request.pitch) return 2;
    unsigned long long used=(unsigned long long)(request.height-1)*request.pitch+request.width*4;
    if (used>IGGT_COPY32_MAX_BYTES) {
        std::fprintf(stderr,"Surface exceeds %u-byte ABI limit\n",(unsigned)IGGT_COPY32_MAX_BYTES);
        return 2;
    }
    request.bytes=(unsigned int)used;
    FILE *input=std::fopen(argv[1],"rb");
    if (!input) { std::perror("input"); return 1; }
    bool read=std::fread(request.pixels,1,request.bytes,input)==request.bytes;
    std::fclose(input); if (!read) { std::fprintf(stderr,"Input surface is too short\n"); return 1; }
    io_service_t service=IOServiceGetMatchingService(kIOMasterPortDefault,IOServiceMatching("IntelGtBringup"));
    if (!service) { std::fprintf(stderr,"IntelGtBringup is not registered; no CPU fallback\n"); return 1; }
    io_connect_t connection=IO_OBJECT_NULL;
    kern_return_t kr=IOServiceOpen(service,mach_task_self(),0,&connection);
    IOObjectRelease(service);
    if (kr!=KERN_SUCCESS) { std::fprintf(stderr,"IOServiceOpen: 0x%x\n",kr); return 1; }
    size_t size=sizeof(response);
    kr=IOConnectCallStructMethod(connection,IGGT_COPY32_SELECTOR,&request,sizeof(request),&response,&size);
    IOServiceClose(connection);
    if (kr!=KERN_SUCCESS || size!=sizeof(response)) {
        std::fprintf(stderr,"GPU copy failed: 0x%x; output not written\n",kr); return 1;
    }
    FILE *output=std::fopen(argv[2],"wb");
    if (!output) { std::perror("output"); return 1; }
    bool wrote=std::fwrite(response.pixels,1,request.bytes,output)==request.bytes;
    bool closed=std::fclose(output)==0;
    if (!wrote || !closed) { std::fprintf(stderr,"Output write failed\n"); return 1; }
    std::printf("GPU copied %u bytes (fence + context retirement per band)\n",request.bytes);
    return 0;
}
