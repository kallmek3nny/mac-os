// SPDX-License-Identifier: MIT
#include "IntelSinkGgttTxn.hpp"
#include <cstdio>
#include <cstring>

static int checks=0;
static int failures=0;
static void ck32(unsigned g,unsigned w,const char*s){++checks;if(g!=w){std::printf("FAIL: %s got=%u want=%u\n",s,g,w);++failures;}}
static void ck64(unsigned long long g,unsigned long long w,const char*s){++checks;if(g!=w){std::printf("FAIL: %s got=%llx want=%llx\n",s,g,w);++failures;}}

struct Fake {
    unsigned long long pte[32];
    unsigned entries=32;
    unsigned failWrite=0xffffffffu;
};
static bool rd(void *p,unsigned i,unsigned long long *v){auto*f=(Fake*)p;if(!f||!v||i>=f->entries)return false;*v=f->pte[i];return true;}
static bool wr(void *p,unsigned i,unsigned long long v){auto*f=(Fake*)p;if(!f||i>=f->entries||i==f->failWrite)return false;f->pte[i]=v;return true;}
static void images(unsigned long long *n,unsigned long long *o){for(unsigned i=0;i<3;++i){n[i]=igSinkGgttPteForPage(0x100000ull,i);o[i]=0x200ull+i*2ull;}}

struct CorruptVisit {
    Fake f;
    unsigned index=0xffffffffu;
    unsigned wantedVisit=0xffffffffu;
    unsigned visits=0;
};
static bool cvrd(void *p,unsigned i,unsigned long long *v){
    auto*c=(CorruptVisit*)p;if(!c||!v||i>=c->f.entries)return false;*v=c->f.pte[i];
    if(i==c->index){unsigned visit=c->visits++;if(visit==c->wantedVisit)*v^=0x1000ull;}
    return true;
}
static bool cvwr(void *p,unsigned i,unsigned long long v){auto*c=(CorruptVisit*)p;if(!c||i>=c->f.entries)return false;c->f.pte[i]=v;return true;}

static void happy(){
    std::puts("1. exact prepare/commit/restore");
    Fake f; unsigned long long n[3],old[3],snap[3]={}; images(n,old); for(unsigned i=0;i<3;++i)f.pte[8+i]=old[i];
    IGSinkGgttTxn t={}; IGSinkGgttTxnStats st={};
    ck32(igSinkGgttTxnPrepare(8,n,3,32,snap,3,rd,&f,&t),IG_SINK_GGTT_TXN_OK,"prepare");
    ck32(igSinkGgttTxnCommit(&t,n,snap,3,rd,wr,&f,&st),IG_SINK_GGTT_TXN_OK,"commit"); ck32(st.writesVerified,3,"install verified");
    for(unsigned i=0;i<3;++i)ck64(f.pte[8+i],n[i],"installed exact");
    std::memset(&st,0,sizeof(st));
    ck32(igSinkGgttTxnRestore(&t,n,snap,3,rd,wr,&f,&st),IG_SINK_GGTT_TXN_OK,"restore"); ck32(st.writesVerified,3,"restore verified");
    for(unsigned i=0;i<3;++i)ck64(f.pte[8+i],old[i],"restored exact");
}

static void refusals(){
    std::puts("2. occupied/drift/malformed inputs refuse before mutation");
    Fake f; unsigned long long n[3],old[3],snap[3]={}; images(n,old); for(unsigned i=0;i<3;++i)f.pte[8+i]=old[i];
    f.pte[9]=n[0]; IGSinkGgttTxn t={};
    ck32(igSinkGgttTxnPrepare(8,n,3,32,snap,3,rd,&f,&t),IG_SINK_GGTT_TXN_TARGET_OCCUPIED,"occupied");
    for(unsigned i=0;i<3;++i)f.pte[8+i]=old[i];
    ck32(igSinkGgttTxnPrepare(8,n,3,32,snap,3,rd,&f,&t),IG_SINK_GGTT_TXN_OK,"prepare clean");
    f.pte[9]=0x222ull;
    ck32(igSinkGgttTxnCommit(&t,n,snap,3,rd,wr,&f,nullptr),IG_SINK_GGTT_TXN_STATE_CHANGED,"drift"); ck64(f.pte[8],old[0],"drift zero writes");
    ck32(igSinkGgttTxnPrepare(31,n,2,32,snap,3,rd,&f,&t),IG_SINK_GGTT_TXN_BAD_RANGE,"bad range");
    auto bad=n[1];n[1]=0x3ull;ck32(igSinkGgttTxnPrepare(8,n,2,32,snap,3,rd,&f,&t),IG_SINK_GGTT_TXN_BAD_NEW_PTE,"bad pte");n[1]=bad;
    ck32(igSinkGgttTxnPrepare(8,n,2,32,snap,1,rd,&f,&t),IG_SINK_GGTT_TXN_SNAPSHOT_TOO_SMALL,"short snapshot");
}

static void installRollback(){
    std::puts("3. post-write readback failure rolls back attempted prefix");
    CorruptVisit c; c.index=9; c.wantedVisit=2; // prepare, commit revalidate, then post-write readback
    unsigned long long n[3],old[3],snap[3]={};images(n,old);for(unsigned i=0;i<3;++i)c.f.pte[8+i]=old[i];
    IGSinkGgttTxn t={};ck32(igSinkGgttTxnPrepare(8,n,3,32,snap,3,cvrd,&c,&t),IG_SINK_GGTT_TXN_OK,"prepare rollback");
    IGSinkGgttTxnStats st={};
    ck32(igSinkGgttTxnCommit(&t,n,snap,3,cvrd,cvwr,&c,&st),IG_SINK_GGTT_TXN_WRITE_FAILED_ROLLED_BACK,"install rollback");
    ck32(st.recoveryWrites,2,"two recovery writes"); for(unsigned i=0;i<3;++i)ck64(c.f.pte[8+i],old[i],"old image after rollback");

    Fake persistent;for(unsigned i=0;i<3;++i)persistent.pte[8+i]=old[i];
    ck32(igSinkGgttTxnPrepare(8,n,3,32,snap,3,rd,&persistent,&t),IG_SINK_GGTT_TXN_OK,"prepare persistent failure");
    persistent.failWrite=9;
    ck32(igSinkGgttTxnCommit(&t,n,snap,3,rd,wr,&persistent,nullptr),IG_SINK_GGTT_TXN_ROLLBACK_FAILED,"unrecoverable install quarantinable");
}

static void restoreSafety(){
    std::puts("4. restore mismatch refuses, failed restore reinstalls owned image");
    unsigned long long n[3],old[3];images(n,old); IGSinkGgttTxn t={8,3,32};
    CorruptVisit c;for(unsigned i=0;i<3;++i)c.f.pte[8+i]=n[i];
    c.f.pte[10]^=0x1000ull;
    ck32(igSinkGgttTxnRestore(&t,n,old,3,cvrd,cvwr,&c,nullptr),IG_SINK_GGTT_TXN_RESTORE_STATE_MISMATCH,"foreign state");
    ck64(c.f.pte[8],n[0],"mismatch zero writes"); c.f.pte[10]=n[2];
    c.index=9;c.wantedVisit=1;c.visits=0; // validation visit 0, post-restore readback visit 1
    IGSinkGgttTxnStats st={};
    ck32(igSinkGgttTxnRestore(&t,n,old,3,cvrd,cvwr,&c,&st),IG_SINK_GGTT_TXN_RESTORE_FAILED_REINSTALLED,"restore reinstall");
    ck64(c.f.pte[8],n[0],"owned first reinstalled");ck64(c.f.pte[9],n[1],"owned failed reinstalled");ck64(c.f.pte[10],n[2],"owned suffix unchanged");
}

int main(){
    happy();
    refusals();
    installRollback();
    restoreSafety();
    std::printf("\n%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
