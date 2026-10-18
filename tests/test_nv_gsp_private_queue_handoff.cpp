// SPDX-License-Identifier: MIT
// Pure GA107 private GSP queue handoff contract. No IOKit or GPU writes.
#include "NVGspQueuePrivateHandoff.hpp"
#include <cstdio>
#include <cstring>

static unsigned checks=0u, failures=0u;
static void check(bool ok,const char *name)
{
    ++checks;
    if(!ok){++failures;std::printf("FAIL: %s\n",name);}
}
static NVGspQueuePlan layout()
{
    NVGspQueuePlan p={};
    check(nvGspBuildQueuePlan(true,false,0u,&p)==NV_GSP_QUEUE_PLAN_OK,
          "normal queue layout");
    return p;
}
static NVGspQueueInitArguments args(const NVGspQueuePlan &p)
{
    NVGspQueueInitArguments a={};
    a.sharedMemPhysAddr=0x100000ull;
    a.cmdQueueOffset=p.commandQueueOffset;
    a.statQueueOffset=p.statusQueueOffset;
    a.queueElementHdrSize=p.queueElementHeaderBytes;
    a.queueElementSizeMin=p.queueElementMinBytes;
    a.queueElementSizeMax=p.queueElementMaxBytes;
    a.queueHeaderAlign=p.queueHeaderAlignBytes;
    a.queueElementAlign=p.queueElementAlignShift;
    a.pageTableEntryCount=p.pageTableEntryCount;
    return a;
}
static void expectRefused(NVGspQueuePlan &p,NVGspQueueInitArguments &a,
                          bool owned,bool dma,bool synced,bool notified,
                          NVGspPrivateU32 want,const char *name)
{
    NVGspQueueInitArguments output;
    std::memset(&output,0xa5,sizeof(output));
    const NVGspQueueInitArguments original=output;
    check(nvGspCopyPrivateQueueHandoff(&p,&a,owned,dma,synced,notified,
                                      &output)==want,name);
    check(std::memcmp(&output,&original,sizeof(output))==0,
          "refusal preserves output");
}
int main()
{
    NVGspQueuePlan p=layout();
    NVGspQueueInitArguments a=args(p),out={};
    check(nvGspCopyPrivateQueueHandoff(&p,&a,true,true,true,false,&out)==
          NV_GSP_QUEUE_PRIVATE_OK,"valid private handoff");
    check(std::memcmp(&a,&out,sizeof(a))==0,"preserves exact ABI arguments");
    expectRefused(p,a,false,true,true,false,NV_GSP_QUEUE_PRIVATE_NOT_OWNED,
                  "released backing refused");
    expectRefused(p,a,true,false,true,false,NV_GSP_QUEUE_PRIVATE_NOT_OWNED,
                  "unbound DMA refused");
    expectRefused(p,a,true,true,false,false,NV_GSP_QUEUE_PRIVATE_NOT_SYNCED,
                  "DMA sync failure refused");
    expectRefused(p,a,true,true,true,true,NV_GSP_QUEUE_PRIVATE_ALREADY_EXPOSED,
                  "post-notify needs ownership protocol");
    auto corruptP=p; ++corruptP.statusQueueOffset;
    expectRefused(corruptP,a,true,true,true,false,NV_GSP_QUEUE_PRIVATE_BAD_LAYOUT,
                  "drifted layout refused");
    auto corruptA=a; ++corruptA.pageTableEntryCount;
    expectRefused(p,corruptA,true,true,true,false,NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS,
                  "ABI count drift refused");
    corruptA=a; ++corruptA.queueElementAlign;
    expectRefused(p,corruptA,true,true,true,false,NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS,
                  "ABI alignment drift refused");
    corruptA=a; corruptA.flags=NV_GSP_QUEUE_INIT_FLAG_ENCRYPTION;
    expectRefused(p,corruptA,true,true,true,false,NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS,
                  "unexpected CC refused");
    corruptA=a; corruptA.sharedMemPhysAddr=0ull;
    expectRefused(p,corruptA,true,true,true,false,NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS,
                  "missing address refused");
    corruptA=a; ++corruptA.sharedMemPhysAddr;
    expectRefused(p,corruptA,true,true,true,false,NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS,
                  "unaligned address refused");
    corruptA=a; corruptA.reserved[7]=1u;
    expectRefused(p,corruptA,true,true,true,false,NV_GSP_QUEUE_PRIVATE_BAD_ARGUMENTS,
                  "reserved byte refused");
    check(nvGspCopyPrivateQueueHandoff(nullptr,&a,true,true,true,false,&out)==
          NV_GSP_QUEUE_PRIVATE_NULL,"null layout refused");
    check(nvGspCopyPrivateQueueHandoff(&p,nullptr,true,true,true,false,&out)==
          NV_GSP_QUEUE_PRIVATE_NULL,"null arguments refused");
    check(nvGspCopyPrivateQueueHandoff(&p,&a,true,true,true,false,nullptr)==
          NV_GSP_QUEUE_PRIVATE_NULL,"null output refused");
    for(unsigned i=0u;i<NV_GSP_QUEUE_PRIVATE_LIMIT;++i)
        check(std::strcmp(nvGspPrivateQueueVerdictName(i),"unknown")!=0,
              "all verdicts named");
    std::printf("%u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
