// SPDX-License-Identifier: MIT
// Synchronous Tiger Lake BCS0 colour-fill entry point. No raw GPU address input.
#include "IntelGtBringup.hpp"
#include "IntelGtBlitter.hpp"
#include "IntelGtCopyPlan.hpp"
#include "IntelGtBlitABI.h"

#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

namespace {
// These are the same fixed private-PPGTT pool slots owned by IntelGtBringup.cpp.
// The user ABI never sees them. A later persistent-surface allocator can replace
// this scratch layout without changing the selector contract.
constexpr UInt32 FillDest  = 18u * 4096u;
constexpr UInt32 FillFence = 19u * 4096u;
constexpr UInt32 FillBatch = 24u * 4096u;
constexpr UInt32 FillScratchBytes = 4096u;
}

IOReturn IntelGtBringup::fillSurface32(void *destination,
                                      UInt32 width, UInt32 height, UInt32 pitch,
                                      UInt32 destinationBytes, UInt32 color) {
    if (!destination || !width || !height || width>1024 || height>1024 ||
        !pitch || pitch>4096 || (pitch & 3)) return kIOReturnBadArgument;

    UInt64 needed=0;
    if (!igGtCopy32SurfaceBytes(width,height,pitch,&needed) ||
        needed>IGGT_FILL32_MAX_BYTES || destinationBytes<needed) {
        return kIOReturnBadArgument;
    }

    static_assert(PoolBytes >= FillBatch + 16u*sizeof(UInt32),
                  "fixed fill batch must fit the owned GT pool");

    IOLockLock(fQueueLock);
    IOReturn result=kIOReturnNotReady;
    if (fStage==3 && fCompleted && fRetired && !fFaulted && fCPU) {
        UInt8 *staged=reinterpret_cast<UInt8 *>(IOMalloc((vm_size_t)needed));
        if (!staged) {
            result=kIOReturnNoMemory;
        } else {
            bzero(staged,(size_t)needed);
            UInt32 row=0, chunks=0;
            result=kIOReturnSuccess;
            const GTBlitSurface32 dst={FillDest,FillScratchBytes,pitch};

            while (row<height) {
                IGGtCopy32Chunk chunk={};
                const UInt32 planVerdict=igGtCopy32PlanChunk(
                    width,height,pitch,row,FillScratchBytes,&chunk);
                setProperty("GTBringupFillPlanVerdict",planVerdict,32);
                setProperty("GTBringupFillChunkIndex",chunks,32);
                setProperty("GTBringupFillChunkRows",chunk.rows,32);
                setProperty("GTBringupFillChunkOffset",chunk.byteOffset,64);
                if (!igGtCopy32PlanSucceeded(planVerdict)) {
                    result=kIOReturnBadArgument;
                    break;
                }

                bzero(fCPU+FillDest,FillScratchBytes);
                if (++fSequence==0) ++fSequence;
                if (!gtEncodeFill32(reinterpret_cast<UInt32 *>(fCPU+FillBatch),16,
                                    dst,width,chunk.rows,color,FillFence,fSequence)) {
                    result=kIOReturnBadArgument;
                    break;
                }
                if (!submitBatch(fSequence)) {
                    fFaulted=true;
                    result=kIOReturnIOError;
                    break;
                }

                // The GPU has fenced and the context has retired before this
                // scratch page can be reused. Preserve row padding as zero by
                // copying from the freshly-zeroed destination scratch page.
                memcpy(staged+(size_t)chunk.byteOffset,
                       fCPU+FillDest,(size_t)chunk.usedBytes);
                row+=chunk.rows;
                ++chunks;
            }

            if (result==kIOReturnSuccess && row==height) {
                memcpy(destination,staged,(size_t)needed);
                setProperty("GTBlitterFillBytes",(UInt32)needed,32);
                setProperty("GTBlitterFillChunks",chunks,32);
                setProperty("GTBlitterFillColor",color,32);
            } else if (result==kIOReturnSuccess) {
                result=kIOReturnIOError;
            }
            IOFree(staged,(vm_size_t)needed);
        }
    }
    IOLockUnlock(fQueueLock);
    return result;
}
