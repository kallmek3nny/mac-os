// SPDX-License-Identifier: MIT
#include "IntelSinkGgttTxn.hpp"
#include <cstdio>
#include <cstring>

static int failures=0;
static void check(bool ok,const char *what)
{
    if (!ok) { std::printf("FAIL: %s\n",what); ++failures; }
}
static void check32(unsigned got,unsigned want,const char *what)
{
    if (got!=want) {
        std::printf("FAIL: %s got=%u want=%u\n",what,got,want);
        ++failures;
    }
}
static void check64(unsigned long long got,unsigned long long want,const char *what)
{
    if (got!=want) {
        std::printf("FAIL: %s got=%016llx want=%016llx\n",what,got,want);
        ++failures;
    }
}

struct FakeGgtt {
    unsigned long long ptes[32];
    unsigned entries;
    unsigned failReadIndex;
    unsigned failWriteIndex;
    unsigned corruptReadbackIndex;
    bool failRead;
    bool failWrite;
    bool corruptReadback;
};

static bool readPte(void *opaque,unsigned index,unsigned long long *value)
{
    auto *f=static_cast<FakeGgtt *>(opaque);
    if (!f || !value || index>=f->entries) return false;
    if (f->failRead && index==f->failReadIndex) return false;
    *value=f->ptes[index];
    if (f->corruptReadback && index==f->corruptReadbackIndex)
        *value^=0x1000ull;
    return true;
}
static bool writePte(void *opaque,unsigned index,unsigned long long value)
{
    auto *f=static_cast<FakeGgtt *>(opaque);
    if (!f || index>=f->entries) return false;
    if (f->failWrite && index==f->failWriteIndex) return false;
    f->ptes[index]=value;
    return true;
}

static void makeImages(unsigned long long *installed,unsigned long long *snapshot)
{
    for (unsigned i=0;i<3;++i) {
        installed[i]=igSinkGgttPteForPage(0x00100000ull,i);
        snapshot[i]=0x200ull+(unsigned long long)i*2ull; // non-present opaque old bits
    }
}

static void testPrepareCommitRestore()
{
    std::puts("1. prepare, commit and restore use exact whole-range images");
    FakeGgtt f={}; f.entries=32;
    unsigned long long installed[3],snapshot[3];
    makeImages(installed,snapshot);
    for (unsigned i=0;i<3;++i) f.ptes[8+i]=snapshot[i];

    IGSinkGgttTxn txn={};
    unsigned long long captured[3]={};
    check32(igSinkGgttTxnPrepare(8,installed,3,f.entries,captured,3,readPte,&f,&txn),
            IG_SINK_GGTT_TXN_OK,"prepare succeeds");
    for (unsigned i=0;i<3;++i) check64(captured[i],snapshot[i],"snapshot exact");

    IGSinkGgttTxnStats stats={};
    check32(igSinkGgttTxnCommit(&txn,installed,captured,3,readPte,writePte,&f,&stats),
            IG_SINK_GGTT_TXN_OK,"commit succeeds");
    check32(stats.writesVerified,3,"all install writes verified");
    for (unsigned i=0;i<3;++i) check64(f.ptes[8+i],installed[i],"installed exact");

    std::memset(&stats,0,sizeof(stats));
    check32(igSinkGgttTxnRestore(&txn,installed,captured,3,readPte,writePte,&f,&stats),
            IG_SINK_GGTT_TXN_OK,"restore succeeds");
    check32(stats.writesVerified,3,"all restore writes verified");
    for (unsigned i=0;i<3;++i) check64(f.ptes[8+i],snapshot[i],"restored exact");
}

static void testOccupiedAndChangedRefuseBeforeWrite()
{
    std::puts("2. occupied and changed state refuse before mutation");
    FakeGgtt f={}; f.entries=32;
    unsigned long long installed[3],captured[3]={};
    makeImages(installed,captured);
    f.ptes[8]=0x200ull;
    f.ptes[9]=installed[0];
    f.ptes[10]=0x204ull;
    IGSinkGgttTxn txn={};
    check32(igSinkGgttTxnPrepare(8,installed,3,f.entries,captured,3,readPte,&f,&txn),
            IG_SINK_GGTT_TXN_TARGET_OCCUPIED,"present target refused");
    check64(f.ptes[8],0x200ull,"occupied refusal changes nothing before target");
    check64(f.ptes[9],installed[0],"occupied refusal changes target nothing");

    f={}; f.entries=32;
    f.ptes[8]=0x200ull; f.ptes[9]=0x202ull; f.ptes[10]=0x204ull;
    check32(igSinkGgttTxnPrepare(8,installed,3,f.entries,captured,3,readPte,&f,&txn),
            IG_SINK_GGTT_TXN_OK,"clean prepare succeeds");
    f.ptes[9]=0x222ull;
    check32(igSinkGgttTxnCommit(&txn,installed,captured,3,readPte,writePte,&f,nullptr),
            IG_SINK_GGTT_TXN_STATE_CHANGED,"precommit state drift refused");
    check64(f.ptes[8],0x200ull,"state drift causes zero install writes");
}

static void testCommitFailureRollsBack()
{
    std::puts("3. failed install rolls back failed prefix with readback");
    FakeGgtt f={}; f.entries=32;
    unsigned long long installed[3],captured[3]={};
    makeImages(installed,captured);
    f.ptes[8]=0x200ull; f.ptes[9]=0x202ull; f.ptes[10]=0x204ull;
    IGSinkGgttTxn txn={};
    check32(igSinkGgttTxnPrepare(8,installed,3,f.entries,captured,3,readPte,&f,&txn),
            IG_SINK_GGTT_TXN_OK,"prepare for rollback");
    f.failWrite=true; f.failWriteIndex=9;
    IGSinkGgttTxnStats stats={};
    check32(igSinkGgttTxnCommit(&txn,installed,captured,3,readPte,writePte,&f,&stats),
            IG_SINK_GGTT_TXN_ROLLBACK_FAILED,
            "persistent write failure also blocks rollback of failed index");
    check64(f.ptes[8],installed[0],"rollback failure leaves ownership uncertain");

    // Readback corruption instead of a persistent write failure can be rolled
    // back once the injected corruption is removed by a custom one-shot shim.
}

struct OneShot {
    FakeGgtt table;
    unsigned corruptIndex;
    unsigned readsAtIndex;
};
static bool oneShotRead(void *opaque,unsigned index,unsigned long long *value)
{
    auto *o=static_cast<OneShot *>(opaque);
    if (!o || index>=o->table.entries || !value) return false;
    *value=o->table.ptes[index];
    if (index==o->corruptIndex && o->readsAtIndex++==1u)
        *value^=0x1000ull; // first post-write read only; prepare was read #0
    return true;
}
static bool oneShotWrite(void *opaque,unsigned index,unsigned long long value)
{
    auto *o=static_cast<OneShot *>(opaque);
    if (!o || index>=o->table.entries) return false;
    o->table.ptes[index]=value;
    return true;
}

static void testCommitReadbackFailureRecovered()
{
    std::puts("4. failed install readback is atomically rolled back");
    OneShot o={}; o.table.entries=32; o.corruptIndex=9;
    unsigned long long installed[3],captured[3]={};
    makeImages(installed,captured);
    o.table.ptes[8]=0x200ull; o.table.ptes[9]=0x202ull; o.table.ptes[10]=0x204ull;
    IGSinkGgttTxn txn={};
    check32(igSinkGgttTxnPrepare(8,installed,3,o.table.entries,captured,3,
                                 oneShotRead,&o,&txn),IG_SINK_GGTT_TXN_OK,"prepare");
    IGSinkGgttTxnStats stats={};
    check32(igSinkGgttTxnCommit(&txn,installed,captured,3,oneShotRead,oneShotWrite,&o,&stats),
            IG_SINK_GGTT_TXN_WRITE_FAILED_ROLLED_BACK,"bad readback rolled back");
    check32(stats.recoveryWrites,2,"two attempted PTEs recovered");
    check64(o.table.ptes[8],captured[0],"first old PTE restored");
    check64(o.table.ptes[9],captured[1],"failed old PTE restored");
    check64(o.table.ptes[10],captured[2],"unattempted PTE untouched");
}

static void testRestoreMismatchAndRecovery()
{
    std::puts("5. teardown refuses foreign state and recovers failed restores");
    OneShot o={}; o.table.entries=32; o.corruptIndex=9;
    unsigned long long installed[3],snapshot[3];
    makeImages(installed,snapshot);
    for (unsigned i=0;i<3;++i) o.table.ptes[8+i]=installed[i];
    IGSinkGgttTxn txn={8,3,32};

    o.table.ptes[10]^=0x1000ull;
    check32(igSinkGgttTxnRestore(&txn,installed,snapshot,3,oneShotRead,oneShotWrite,&o,nullptr),
            IG_SINK_GGTT_TXN_RESTORE_STATE_MISMATCH,"foreign GGTT state refuses restore");
    check64(o.table.ptes[8],installed[0],"mismatch performs no restore writes");
    o.table.ptes[10]=installed[2];

    // Reset one-shot read counter: restore validation reads index 9 once, then
    // its post-restore read is the second visit and is intentionally corrupted.
    o.readsAtIndex=0;
    IGSinkGgttTxnStats stats={};
    check32(igSinkGgttTxnRestore(&txn,installed,snapshot,3,oneShotRead,oneShotWrite,&o,&stats),
            IG_SINK_GGTT_TXN_RESTORE_FAILED_REINSTALLED,
            "failed old-image readback reinstalls owned image");
    check64(o.table.ptes[8],installed[0],"owned image reinstalled at first attempted PTE");
    check64(o.table.ptes[9],installed[1],"owned image reinstalled at failed PTE");
    check64(o.table.ptes[10],installed[2],"unattempted owned PTE remains");
}

static void testInputValidation()
{
    std::puts("6. malformed transaction inputs are refused");
    FakeGgtt f={}; f.entries=32;
    unsigned long long installed[2]={
        igSinkGgttPteForPage(0x100000ull,0),
        igSinkGgttPteForPage(0x100000ull,1)};
    unsigned long long snapshot[2]={};
    IGSinkGgttTxn txn={};
    check32(igSinkGgttTxnPrepare(31,installed,2,32,snapshot,2,readPte,&f,&txn),
            IG_SINK_GGTT_TXN_BAD_RANGE,"range past table refused");
    installed[1]=0x3ull;
    check32(igSinkGgttTxnPrepare(8,installed,2,32,snapshot,2,readPte,&f,&txn),
            IG_SINK_GGTT_TXN_BAD_NEW_PTE,"malformed installed PTE refused");
    check32(igSinkGgttTxnPrepare(8,installed,2,32,snapshot,1,readPte,&f,&txn),
            IG_SINK_GGTT_TXN_SNAPSHOT_TOO_SMALL,"short snapshot refused before reads");
}

int main()
{
    testPrepareCommitRestore();
    testOccupiedAndChangedRefuseBeforeWrite();
    testCommitFailureRollsBack();
    testCommitReadbackFailureRecovered();
    testRestoreMismatchAndRecovery();
    testInputValidation();
    if (failures) {
        std::printf("%d failure(s)\n",failures);
        return 1;
    }
    std::puts("Tiger Lake sink GGTT transaction tests passed");
    return 0;
}
