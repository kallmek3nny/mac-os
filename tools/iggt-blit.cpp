// SPDX-License-Identifier: MIT
// macOS user-space client: performs actual GPU copy/fill work, never a CPU fallback.
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include "IntelGtBlitABI.h"

static bool boundedNumber(const char *s, unsigned int limit, unsigned int &out) {
    char *end=nullptr; errno=0;
    unsigned long value=strtoul(s,&end,10);
    if (errno || !*s || *end || !value || value>limit) return false;
    out=(unsigned int)value; return true;
}
static bool number(const char *s, unsigned int &out) {
    return boundedNumber(s,1024u,out);
}
static bool colorNumber(const char *s, unsigned int &out) {
    char *end=nullptr; errno=0;
    unsigned long value=strtoul(s,&end,0);
    if (errno || !*s || *end || value>0xfffffffful) return false;
    out=(unsigned int)value; return true;
}
static bool handleNumber(const char *s, unsigned long long &out) {
    char *end=nullptr; errno=0;
    unsigned long long value=strtoull(s,&end,0);
    if (errno || !*s || *end || !value) return false;
    out=value; return true;
}
static bool surfaceBytes(unsigned int width,unsigned int height,unsigned int pitch,
                         unsigned int limit,unsigned int &out) {
    if (!width || !height || !pitch || (pitch & 3u) || width*4u>pitch) return false;
    unsigned long long used=(unsigned long long)(height-1u)*pitch+(unsigned long long)width*4u;
    if (!used || used>limit) return false;
    out=(unsigned int)used;
    return true;
}
static io_connect_t openBackend() {
    io_service_t service=IOServiceGetMatchingService(
        kIOMasterPortDefault,IOServiceMatching("IntelGtBringup"));
    if (!service) {
        std::fprintf(stderr,"IntelGtBringup is not registered; no CPU fallback\n");
        return IO_OBJECT_NULL;
    }
    io_connect_t connection=IO_OBJECT_NULL;
    kern_return_t kr=IOServiceOpen(service,mach_task_self(),0,&connection);
    IOObjectRelease(service);
    if (kr!=KERN_SUCCESS) {
        std::fprintf(stderr,"IOServiceOpen: 0x%x\n",kr);
        return IO_OBJECT_NULL;
    }
    return connection;
}
static bool writeOutput(const char *path,const unsigned char *pixels,unsigned int bytes) {
    FILE *output=std::fopen(path,"wb");
    if (!output) { std::perror("output"); return false; }
    bool wrote=std::fwrite(pixels,1,bytes,output)==bytes;
    bool closed=std::fclose(output)==0;
    if (!wrote || !closed) { std::fprintf(stderr,"Output write failed\n"); return false; }
    return true;
}
static int runCopy(char **argv) {
    IgGtCopy32Request request={}; IgGtCopy32Response response={};
    if (!number(argv[3],request.width) || !number(argv[4],request.height) ||
        !number(argv[5],request.pitch) ||
        !surfaceBytes(request.width,request.height,request.pitch,
                      IGGT_COPY32_MAX_BYTES,request.bytes)) return 2;
    FILE *input=std::fopen(argv[1],"rb");
    if (!input) { std::perror("input"); return 1; }
    bool read=std::fread(request.pixels,1,request.bytes,input)==request.bytes;
    std::fclose(input);
    if (!read) { std::fprintf(stderr,"Input surface is too short\n"); return 1; }

    io_connect_t connection=openBackend();
    if (!connection) return 1;
    size_t size=sizeof(response);
    kern_return_t kr=IOConnectCallStructMethod(
        connection,IGGT_COPY32_SELECTOR,&request,sizeof(request),&response,&size);
    IOServiceClose(connection);
    if (kr!=KERN_SUCCESS || size!=sizeof(response)) {
        std::fprintf(stderr,"GPU copy failed: 0x%x; output not written\n",kr);
        return 1;
    }
    if (!writeOutput(argv[2],response.pixels,request.bytes)) return 1;
    std::printf("GPU copied %u bytes (fence + context retirement per band)\n",request.bytes);
    return 0;
}
static int runFill(char **argv) {
    IgGtFill32Request request={}; IgGtFill32Response response={};
    if (!number(argv[3],request.width) || !number(argv[4],request.height) ||
        !number(argv[5],request.pitch) || !colorNumber(argv[6],request.color) ||
        !surfaceBytes(request.width,request.height,request.pitch,
                      IGGT_FILL32_MAX_BYTES,request.bytes)) return 2;

    io_connect_t connection=openBackend();
    if (!connection) return 1;
    size_t size=sizeof(response);
    kern_return_t kr=IOConnectCallStructMethod(
        connection,IGGT_FILL32_SELECTOR,&request,sizeof(request),&response,&size);
    IOServiceClose(connection);
    if (kr!=KERN_SUCCESS || size!=sizeof(response)) {
        std::fprintf(stderr,"GPU fill failed: 0x%x; output not written\n",kr);
        return 1;
    }
    if (!writeOutput(argv[2],response.pixels,request.bytes)) return 1;
    std::printf("GPU filled %u bytes with 0x%08x (fence + context retirement per band)\n",
                request.bytes,request.color);
    return 0;
}
static int runSurfaceAlloc(char **argv) {
    IgGtSurfaceAlloc32Request request={}; IgGtSurfaceAlloc32Response response={};
    if (!boundedNumber(argv[2],16384u,request.width) ||
        !boundedNumber(argv[3],16384u,request.height) ||
        !boundedNumber(argv[4],32767u,request.pitch) ||
        (request.pitch & 3u) || request.width*4u>request.pitch) return 2;

    io_connect_t connection=openBackend();
    if (!connection) return 1;
    size_t size=sizeof(response);
    kern_return_t kr=IOConnectCallStructMethod(
        connection,IGGT_SURFACE_ALLOC32_SELECTOR,&request,sizeof(request),&response,&size);
    IOServiceClose(connection);
    if (kr!=KERN_SUCCESS || size!=sizeof(response) || !response.handle) {
        std::fprintf(stderr,"persistent surface allocation failed: 0x%x\n",kr);
        return 1;
    }
    std::printf("0x%016llx\n",response.handle);
    return 0;
}
static int runSurfaceFree(char **argv) {
    IgGtSurfaceFreeRequest request={}; IgGtSurfaceFreeResponse response={};
    if (!handleNumber(argv[2],request.handle)) return 2;
    io_connect_t connection=openBackend();
    if (!connection) return 1;
    size_t size=sizeof(response);
    kern_return_t kr=IOConnectCallStructMethod(
        connection,IGGT_SURFACE_FREE_SELECTOR,&request,sizeof(request),&response,&size);
    IOServiceClose(connection);
    if (kr!=KERN_SUCCESS || size!=sizeof(response) || response.released!=1u) {
        std::fprintf(stderr,"persistent surface free failed: 0x%x\n",kr);
        return 1;
    }
    std::puts("persistent surface scrubbed and released");
    return 0;
}
static int runSurfaceFill(char **argv) {
    IgGtSurfaceFill32Request request={}; IgGtSurfaceFill32Response response={};
    if (!handleNumber(argv[2],request.handle) || !colorNumber(argv[3],request.color)) return 2;
    io_connect_t connection=openBackend();
    if (!connection) return 1;
    size_t size=sizeof(response);
    kern_return_t kr=IOConnectCallStructMethod(
        connection,IGGT_SURFACE_FILL32_SELECTOR,&request,sizeof(request),&response,&size);
    IOServiceClose(connection);
    if (kr!=KERN_SUCCESS || size!=sizeof(response) || response.completed!=1u) {
        std::fprintf(stderr,"persistent surface fill failed: 0x%x\n",kr);
        return 1;
    }
    std::puts("persistent surface GPU fill fenced and retired");
    return 0;
}
static int runSurfaceCopy(char **argv) {
    IgGtSurfaceCopy32Request request={}; IgGtSurfaceCopy32Response response={};
    if (!handleNumber(argv[2],request.sourceHandle) ||
        !handleNumber(argv[3],request.destinationHandle)) return 2;
    io_connect_t connection=openBackend();
    if (!connection) return 1;
    size_t size=sizeof(response);
    kern_return_t kr=IOConnectCallStructMethod(
        connection,IGGT_SURFACE_COPY32_SELECTOR,&request,sizeof(request),&response,&size);
    IOServiceClose(connection);
    if (kr!=KERN_SUCCESS || size!=sizeof(response) || response.completed!=1u) {
        std::fprintf(stderr,"persistent surface copy failed: 0x%x\n",kr);
        return 1;
    }
    std::puts("persistent surface GPU copy fenced and retired");
    return 0;
}
int main(int argc, char **argv) {
    if (argc==7 && std::strcmp(argv[1],"--fill")==0) return runFill(argv);
    if (argc==5 && std::strcmp(argv[1],"--surface-alloc")==0) return runSurfaceAlloc(argv);
    if (argc==3 && std::strcmp(argv[1],"--surface-free")==0) return runSurfaceFree(argv);
    if (argc==4 && std::strcmp(argv[1],"--surface-fill")==0) return runSurfaceFill(argv);
    if (argc==4 && std::strcmp(argv[1],"--surface-copy")==0) return runSurfaceCopy(argv);
    if (argc==6) return runCopy(argv);
    std::fprintf(stderr,
        "Usage:\n"
        "  iggt-blit input.bgra output.bgra width height pitch\n"
        "  iggt-blit --fill output.bgra width height pitch color\n"
        "  iggt-blit --surface-alloc width height pitch\n"
        "  iggt-blit --surface-fill handle color\n"
        "  iggt-blit --surface-copy source_handle destination_handle\n"
        "  iggt-blit --surface-free handle\n"
        "color/handles accept decimal or 0x-prefixed values. Staging surface max: %u bytes.\n"
        "Persistent handles expose no GPU/IOVA address; free them when finished.\n"
        "Requires administrator, iggt-bringup=3 and persistent surfaces additionally require iggt-surface-install=1. No CPU fallback.\n",
        (unsigned)IGGT_COPY32_MAX_BYTES);
    return 2;
}
