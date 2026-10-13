// SPDX-License-Identifier: MIT
// Experimental Tiger Lake BCS0 DMA/PPGTT/execlist backend.
// Register-state layout derives from the vendored Linux v6.12 intel_lrc.c
// gen12_xcs_offsets; descriptor fields from intel_lrc.h. No probe is modified.
#include "IntelGtBringup.hpp"
#include <pexpert/pexpert.h>
#include <mach/task.h>
#include <libkern/libkern.h>
#include <IOKit/IORegistryEntry.h>

#define GTLOG(fmt, ...) IOLog("IntelGtBringup: " fmt "\n", ##__VA_ARGS__)
OSDefineMetaClassAndStructors(IntelGtBringup, IOService)
namespace {
constexpr UInt32 BCS = 0x22000;
constexpr UInt32 GttTable = 0x800000;
constexpr UInt64 AddressMask = 0x00003ffffffff000ULL;
// First sixteen pages are reserved for context, including its HWS page.
constexpr UInt32 Ring = 16 * 4096, Source = 17 * 4096, Dest = 18 * 4096;
constexpr UInt32 Fence = 19 * 4096, Pml4 = 20 * 4096;
constexpr UInt32 Pdp = 21 * 4096, Pd = 22 * 4096, Pt = 23 * 4096;
constexpr UInt32 Batch = 24 * 4096;
constexpr UInt32 Cookie = 0x47544350; // GTCP; CPU never stores this to Fence.
inline void barrier() { __asm__ volatile("mfence" ::: "memory"); }
inline UInt32 lri(UInt32 n) { return (0x22u << 23) | (1u << 19) | (1u << 12) | (2*n-1); }
void group(UInt32 *r, UInt32 &pos, const UInt16 *offsets, UInt32 n) {
    r[pos++] = lri(n);
    for (UInt32 i=0; i<n; ++i) { r[pos++] = BCS + offsets[i]; r[pos++] = 0; }
}
}

bool IntelGtBringup::init(OSDictionary *dict) {
    if (!super::init(dict)) return false;
    fPCI=nullptr; fBarMap=nullptr; fPoolMap=nullptr; fPool=nullptr; fDMA=nullptr;
    fMMIO=nullptr; fCPU=nullptr; fBarBytes=0; fBusAddress=0;
    bzero(fSavedPtes,sizeof(fSavedPtes));
    fGGTTBase=0; fGGTTEntries=0; fStage=0; fTrace=0; fPteCount=0;
    fOldRingMode=0; fOldMiMode=0; fOldHws=0; fPatIndex=0; fOldPCICommand=0;
    fOpened=false; fPrepared=false; fDMABound=false; fPCIChanged=false;
    fRenderWake=false; fGTWake=false; fEngineChanged=false;
    fSubmissionPending=false; fCompleted=false; fQuarantined=false;
    return true;
}

UInt32 IntelGtBringup::read32(UInt32 o) {
    if (!fMMIO || (o & 3) || (UInt64)o+4 > fBarBytes) return 0xffffffff;
    UInt32 v = *reinterpret_cast<volatile UInt32 *>(fMMIO+o);
    barrier();
    GTLOG("%u R32 %08x = %08x", ++fTrace, o, v);
    return v;
}
bool IntelGtBringup::write32(UInt32 o, UInt32 v) {
    if (!fStage || !fOpened || !fMMIO || (o & 3) || (UInt64)o+4 > fBarBytes) return false;
    GTLOG("%u W32 %08x <- %08x", ++fTrace, o, v);
    *reinterpret_cast<volatile UInt32 *>(fMMIO+o) = v;
    barrier();
    // Masked/self-clearing registers need semantic checks by their caller.
    return read32(o) != 0xffffffff;
}
bool IntelGtBringup::wait32(UInt32 o, UInt32 mask, UInt32 wanted, UInt32 polls) {
    for (UInt32 i=0; i<polls; ++i) {
        UInt32 v=read32(o);
        if (v==0xffffffff) return false;
        if ((v & mask)==wanted) return true;
        IOSleep(1);
    }
    return false;
}
bool IntelGtBringup::acquireWake(UInt32 req, UInt32 ack, bool &held) {
    // Never reset somebody else's forcewake references.
    if (!wait32(ack,1,0,50)) return false;
    held=true; // also release our request if acknowledgement times out
    return write32(req,0x00010001) && wait32(ack,1,1,50);
}
void IntelGtBringup::releaseWake() {
    if (fRenderWake) { write32(0xa278,0x00010000); wait32(0xd84,1,0,50); fRenderWake=false; }
    if (fGTWake) { write32(0xa188,0x00010000); wait32(0x130044,1,0,50); fGTWake=false; }
}
void IntelGtBringup::fail(const char *why) {
    GTLOG("STOP: %s",why);
    setProperty("GTBringupError",why);
}

bool IntelGtBringup::claimEngine() {
    // Only a cold, unconfigured copy engine is eligible. Idle alone is NOT
    // ownership: a driver can have queued work while the engine is idle.
    UInt32 ctl=read32(BCS+0x3c), mi=read32(BCS+0x9c), mode=read32(BCS+0x29c);
    if (ctl==0xffffffff || mi==0xffffffff || mode==0xffffffff) return false;
    if ((ctl & 1) || !(mi & (1u<<9)) || (mode & ((1u<<3)|(1u<<15)))) {
        fail("BCS0 already configured/busy; no takeover or reset attempted");
        return false;
    }
    fOldRingMode=mode; fOldMiMode=mi; fOldHws=read32(BCS+0x80);
    if (fOldHws==0xffffffff) return false;
    // Select an existing UC PAT entry, rather than changing a global table
    // which may also be used by display. Gen12 PAT memory type is bits 1:0.
    for (UInt32 i=0; i<8; ++i) {
        UInt32 pat=read32(0x4800+4*i);
        if (pat!=0xffffffff && (pat & 3)==0) { fPatIndex=i; return true; }
    }
    fail("no existing uncached PAT entry");
    return false;
}

bool IntelGtBringup::allocateMemory() {
    fPool=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut|kIOMemoryPhysicallyContiguous,PoolBytes,AddressMask);
    if (!fPool || fPool->prepare(kIODirectionInOut)!=kIOReturnSuccess) return false;
    fPrepared=true;
    // CPU completion reads must not see a stale cache line. Never use the
    // descriptor's default cached pointer while this uncached map is alive.
    fPoolMap=fPool->map(kIOMapAnywhere|kIOMapInhibitCache);
    if (!fPoolMap || fPoolMap->getLength()<PoolBytes) return false;
    fCPU=reinterpret_cast<UInt8 *>(fPoolMap->getVirtualAddress());
    if (!fCPU) return false;
    bzero(fCPU,PoolBytes);
    fDMA=IODMACommand::withSpecification(&IODMACommand::OutputHost64,46,
        PoolBytes,IODMACommand::kMapped,PoolBytes,4096);
    if (!fDMA || fDMA->setMemoryDescriptor(fPool,true)!=kIOReturnSuccess) return false;
    fDMABound=true;
    UInt64 offset=0; UInt32 count=1; IODMACommand::Segment64 seg={};
    if (fDMA->gen64IOVMSegments(&offset,&seg,&count)!=kIOReturnSuccess ||
        count!=1 || seg.fLength<PoolBytes || offset<PoolBytes ||
        (seg.fIOVMAddr & 4095) || seg.fIOVMAddr>((1ULL<<46)-PoolBytes)) return false;
    fBusAddress=seg.fIOVMAddr;
    GTLOG("owned DMA %llu bytes, bus=%llx",(unsigned long long)PoolBytes,(unsigned long long)fBusAddress);
    // Four-level private address space. No user-supplied GPU address exists.
    UInt64 *pml4=reinterpret_cast<UInt64 *>(fCPU+Pml4);
    UInt64 *pdp=reinterpret_cast<UInt64 *>(fCPU+Pdp);
    UInt64 *pd=reinterpret_cast<UInt64 *>(fCPU+Pd);
    UInt64 *pt=reinterpret_cast<UInt64 *>(fCPU+Pt);
    pml4[0]=(fBusAddress+Pdp)|3; pdp[0]=(fBusAddress+Pd)|3; pd[0]=(fBusAddress+Pt)|3;
    UInt64 pat=((fPatIndex&1)?(1ULL<<3):0)|((fPatIndex&2)?(1ULL<<4):0)|((fPatIndex&4)?(1ULL<<7):0);
    for (UInt32 i=0; i<PoolPages; ++i) pt[i]=(fBusAddress+4096*i)|3|pat;
    barrier();
    return true;
}

bool IntelGtBringup::installGGTT() {
    // Gen8+ GGTT PTE window is BAR0 + 8 MiB. GGC[7:6] encodes 2/4/8 MiB.
    UInt16 ggc=fPCI->configRead16(0x50);
    UInt32 ggms=(ggc>>6)&3;
    if (ggc==0xffff || !ggms) return false;
    UInt32 bytes=(1u<<ggms)<<20;
    if ((UInt64)GttTable+bytes>fBarBytes) return false;
    fGGTTEntries=bytes/8;
    // No overwriting present/scratch mappings: reserve ONLY a wholly absent
    // range while holding the PCI provider exclusively and with no accelerator.
    UInt32 run=0, first=0;
    volatile UInt64 *pt=reinterpret_cast<volatile UInt64 *>(fMMIO+GttTable);
    for (UInt32 i=fGGTTEntries-1; i>0; --i) {
        UInt64 pte=pt[i];
        if (!(pte & 1)) ++run; else run=0;
        if (run==PoolPages) { first=i; break; }
    }
    if (!first) { fail("no unoccupied GGTT range; existing mappings preserved"); return false; }
    fGGTTBase=first*4096;
    for (UInt32 i=0; i<PoolPages; ++i) {
        UInt32 idx=first+i;
        fSavedPtes[i]=pt[idx];
        if (fSavedPtes[i] & 1) return false;
        UInt64 pte=(fBusAddress+4096*i)|1;
        GTLOG("%u GGTT[%u] <- %llx",++fTrace,idx,(unsigned long long)pte);
        pt[idx]=pte; barrier(); fPteCount=i+1;
        UInt64 back=pt[idx];
        GTLOG("%u GGTT[%u] = %llx",++fTrace,idx,(unsigned long long)back);
        if (back!=pte) return false;
    }
    // Fresh addresses on a cold engine. TGL's gen12 GGTT invalidation is
    // a no-op (intel_ggtt.c); do not emit the older GFX_FLSH_CNTL write.
    setProperty("GTBringupGGTT",fGGTTBase,32);
    return true;
}

bool IntelGtBringup::buildContext() {
    UInt32 *r=reinterpret_cast<UInt32 *>(fCPU+4096);
    static const UInt16 common[]={0x244,0x034,0x030,0x038,0x03c,0x168,0x140,
                                 0x110,0x1c0,0x1c4,0x1c8,0x180,0x2b4};
    static const UInt16 pdp[]={0x3a8,0x28c,0x288,0x284,0x280,0x27c,0x278,0x274,0x270};
    UInt32 pos=1; group(r,pos,common,13); pos+=5; group(r,pos,pdp,9);
    r[pos]=(0x0au<<23)|1; // close Gen12 register-state stream
    r[0x03]=0x00090009; // mask+enable inhibit sync and first restore inhibit
    r[0x05]=0; r[0x07]=0; r[0x09]=fGGTTBase+Ring; r[0x0b]=1;
    r[0x23]=0;
    r[0x31]=(UInt32)((fBusAddress+Pml4)>>32);
    r[0x33]=(UInt32)(fBusAddress+Pml4);
    // The copy surface is 32 x 32 x 4 bytes, linear, entirely in owned pages.
    UInt32 *src=reinterpret_cast<UInt32 *>(fCPU+Source);
    for (UInt32 i=0; i<1024; ++i) src[i]=0xff000000u|((i*0x10203u)&0xffffffu);
    UInt32 *b=reinterpret_cast<UInt32 *>(fCPU+Batch);
    // Gen9+ XY_FAST_COPY_BLT, ten dwords, linear source/destination, 32 bpp.
    b[0]=(2u<<29)|(0x42u<<22)|8;
    b[1]=(3u<<24)|128; b[2]=0; b[3]=(32u<<16)|32;
    b[4]=Dest; b[5]=0; b[6]=0; b[7]=128; b[8]=Source; b[9]=0;
    // Flush the blitter and GPU-store the fence in private PPGTT (not GGTT).
    b[10]=(0x26u<<23)|(1u<<14)|2;
    b[11]=Fence; b[12]=0; b[13]=Cookie;
    b[14]=(0x0au<<23); b[15]=0;
    UInt32 *ring=reinterpret_cast<UInt32 *>(fCPU+Ring);
    ring[0]=(0x31u<<23)|(1u<<8)|1; ring[1]=Batch; ring[2]=0; ring[3]=0;
    r[0x07]=16;
    barrier();
    return true;
}

bool IntelGtBringup::submitCopy() {
    // Changes below are limited to a cold BCS0. No render/display resets.
    fOldPCICommand=fPCI->configRead16(0x04);
    if (fOldPCICommand==0xffff) return false;
    fPCIChanged=true;
    fPCI->configWrite16(0x04,fOldPCICommand|0x0006);
    UInt16 command=fPCI->configRead16(0x04);
    GTLOG("PCI COMMAND %04x -> %04x",fOldPCICommand,command);
    if ((command & 6)!=6) return false;
    fEngineChanged=true;
    if (!write32(BCS+0x80,fGGTTBase) || read32(BCS+0x80)!=fGGTTBase ||
        !write32(BCS+0x29c,0x00080008) || !wait32(BCS+0x29c,8,8,50) ||
        !write32(BCS+0x9c,0x01000000) || !wait32(BCS+0x9c,0x100,0,50)) return false;
    // COPY_CLASS=1, instance 0; software context id 1 at bit 37. Both
    // addressing mode (legacy 64-bit) and privileged bit are explicit.
    UInt64 desc=(1ULL<<61)|(1ULL<<37)|fGGTTBase|0x11dULL;
    // Clear port 1 first, then port 0, exactly the Gen12 ELSQ order.
    if (!write32(BCS+0x518,0) || !write32(BCS+0x51c,0) ||
        !write32(BCS+0x510,(UInt32)desc) || !write32(BCS+0x514,(UInt32)(desc>>32))) return false;
    fSubmissionPending=true;
    // Pin the service/module BEFORE hardware can acquire a DMA reference.
    // There is no reset/retirement manager yet. Even a completed fence does
    // not prove the final context save is finished, so keep DMA until reboot.
    fQuarantined=true; retain();
    GTLOG("BCS0 submit descriptor=%llx",(unsigned long long)desc);
    if (!write32(BCS+0x550,1)) return false;
    volatile UInt32 *fence=reinterpret_cast<volatile UInt32 *>(fCPU+Fence);
    for (UInt32 i=0; i<1000; ++i) {
        barrier();
        if (*fence==Cookie) {
            volatile UInt32 *src=reinterpret_cast<volatile UInt32 *>(fCPU+Source);
            volatile UInt32 *dst=reinterpret_cast<volatile UInt32 *>(fCPU+Dest);
            for (UInt32 n=0; n<1024; ++n) {
                if (src[n]!=dst[n]) { fail("GPU fence arrived but copy data differs"); return false; }
            }
            fCompleted=true; fSubmissionPending=false;
            setProperty("GTBlitterCopyCompleted",true);
            setProperty("GTBlitterCopyBytes",4096,32);
            GTLOG("GPU COPIED 4096 bytes; GPU fence=%08x; resources pinned until reboot",*fence);
            read32(BCS+0x34); read32(BCS+0x30); read32(BCS+0x234); read32(BCS+0x238);
            return true;
        }
        IOSleep(1);
    }
    read32(BCS+0x64); read32(BCS+0x68); read32(BCS+0xb0);
    read32(BCS+0x234); read32(BCS+0x238);
    fail("GPU copy timed out; DMA/GT ownership retained, reboot without boot argument");
    return false;
}

void IntelGtBringup::restoreGGTT() {
    if (!fMMIO) return;
    volatile UInt64 *pt=reinterpret_cast<volatile UInt64 *>(fMMIO+GttTable);
    for (UInt32 i=0; i<fPteCount; ++i) {
        pt[fGGTTBase/4096+i]=fSavedPtes[i]; barrier();
        GTLOG("GGTT restore[%u]=%llx",fGGTTBase/4096+i,
              (unsigned long long)pt[fGGTTBase/4096+i]);
    }
    fPteCount=0;
}
void IntelGtBringup::cleanup() {
    if (fQuarantined) return; // Never free/unmap memory hardware may reference.
    if (fEngineChanged) {
        write32(BCS+0x9c,0x01000000|(fOldMiMode&0x100));
        write32(BCS+0x29c,0x00080000|(fOldRingMode&8));
        write32(BCS+0x80,fOldHws); fEngineChanged=false;
    }
    restoreGGTT();
    if (fDMABound) { fDMA->clearMemoryDescriptor(); fDMABound=false; }
    if (fDMA) { fDMA->release(); fDMA=nullptr; }
    if (fPoolMap) { fPoolMap->release(); fPoolMap=nullptr; }
    if (fPrepared) { fPool->complete(kIODirectionInOut); fPrepared=false; }
    if (fPool) { fPool->release(); fPool=nullptr; }
    fCPU=nullptr;
    if (fPCIChanged) { fPCI->configWrite16(4,fOldPCICommand); fPCIChanged=false; }
    releaseWake();
    if (fBarMap) { fBarMap->release(); fBarMap=nullptr; }
    fMMIO=nullptr;
    if (fOpened) { fPCI->close(this); fOpened=false; }
    if (fPCI) { fPCI->release(); fPCI=nullptr; }
}

bool IntelGtBringup::start(IOService *provider) {
    int stage=0;
    // No boot argument = no PCI open, no MMIO map and no hardware write.
    if (!PE_parse_boot_argn("iggt-bringup",&stage,sizeof(stage)) || stage<1 || stage>3) return false;
    if (!super::start(provider)) return false;
    fStage=(UInt32)stage;
    fPCI=OSDynamicCast(IOPCIDevice,provider);
    if (!fPCI) return false;
    fPCI->retain();
    if (fPCI->configRead16(0)!=0x8086 || fPCI->configRead16(2)!=0x9a68) {
        fail("not the supported 8086:9a68 Tiger Lake device"); cleanup(); return false;
    }
    IORegistryIterator *it=IORegistryIterator::iterateOver(fPCI,gIOServicePlane,kIORegistryIterateRecursively);
    if (!it) { cleanup(); return false; }
    bool accelerator=false;
    while (OSObject *entry=it->getNextObject()) {
        if (entry->metaCast("IOAccelerator") || entry->metaCast("IOGraphicsAccelerator") ||
            entry->metaCast("IOGraphicsAccelerator2")) accelerator=true;
    }
    it->release();
    if (accelerator || !fPCI->open(this)) {
        fail("accelerator/provider already owned; no hardware writes"); cleanup(); return false;
    }
    fOpened=true;
    IOMemoryDescriptor *bar=fPCI->getDeviceMemoryWithIndex(0);
    if (bar) fBarMap=bar->map(kIOMapAnywhere|kIOMapInhibitCache);
    if (!fBarMap) { cleanup(); return false; }
    fMMIO=reinterpret_cast<volatile UInt8 *>(fBarMap->getVirtualAddress());
    fBarBytes=fBarMap->getLength();
    if (!fMMIO || fBarBytes<0x1000000 ||
        !acquireWake(0xa188,0x130044,fGTWake) || !acquireWake(0xa278,0xd84,fRenderWake)) {
        fail("BAR/forcewake acquisition failed"); cleanup(); return false;
    }
    setProperty("GTBringupStage",fStage,32);
    setProperty("GTBlitterCopyCompleted",false);
    setProperty("MetalSupported",false);
    if (fStage==1) { releaseWake(); registerService(); return true; }
    if (!claimEngine() || !allocateMemory() || !installGGTT() || !buildContext()) {
        fail("memory/context preparation failed"); cleanup(); return false;
    }
    if (fStage==2) { cleanup(); setProperty("GTMemorySetupCompleted",true); registerService(); return true; }
    bool copied=submitCopy();
    if (!copied && !fQuarantined) { cleanup(); return false; }
    // Keep the diagnostic service registered even after submission failure.
    // Its retained DMA cannot safely be torn down without engine retirement.
    registerService();
    return true;
}
void IntelGtBringup::stop(IOService *provider) { cleanup(); super::stop(provider); }
void IntelGtBringup::free() { cleanup(); super::free(); }
