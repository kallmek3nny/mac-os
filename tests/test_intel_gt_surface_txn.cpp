// Host-only tests for reversible persistent-surface PPGTT leaf transactions.
#include <cstdio>
#include <cstring>
#include "IntelGtSurfaceTxn.hpp"

static int failures=0;
static void check(bool ok,const char *what) {
    if (!ok) { std::printf("FAIL: %s\n",what); ++failures; }
}
static void check32(unsigned int got,unsigned int want,const char *what) {
    if (got!=want) {
        std::printf("FAIL: %s got=%08x want=%08x\n",what,got,want);
        ++failures;
    }
}
static void check64(unsigned long long got,unsigned long long want,const char *what) {
    if (got!=want) {
        std::printf("FAIL: %s got=%016llx want=%016llx\n",what,got,want);
        ++failures;
    }
}

struct MockPt {
    unsigned long long ptes[IG_GTS_PT_LEAVES_GEN8_PLUS_512];
    int failWriteLeaf;
    unsigned long long failWriteValue;
    int corruptWriteLeaf;
    unsigned long long corruptWriteValue;
    bool failConsumed;
    bool corruptConsumed;
    unsigned int writes;
};

static bool mockRead(void *opaque,unsigned int leaf,unsigned long long *value) {
    MockPt *m=reinterpret_cast<MockPt *>(opaque);
    if (!m || !value || leaf>=IG_GTS_PT_LEAVES_GEN8_PLUS_512) return false;
    *value=m->ptes[leaf];
    return true;
}

static bool mockWrite(void *opaque,unsigned int leaf,unsigned long long value) {
    MockPt *m=reinterpret_cast<MockPt *>(opaque);
    if (!m || leaf>=IG_GTS_PT_LEAVES_GEN8_PLUS_512) return false;
    ++m->writes;
    if (!m->failConsumed && (int)leaf==m->failWriteLeaf && value==m->failWriteValue) {
        m->failConsumed=true;
        return false;
    }
    if (!m->corruptConsumed && (int)leaf==m->corruptWriteLeaf && value==m->corruptWriteValue) {
        m->corruptConsumed=true;
        m->ptes[leaf]=value^0x1000ull;
        return true;
    }
    m->ptes[leaf]=value;
    return true;
}

static MockPt freshMock() {
    MockPt m={};
    m.failWriteLeaf=-1;
    m.corruptWriteLeaf=-1;
    return m;
}

static void buildThree(unsigned long long (&ptes)[3]) {
    const unsigned long long pages[3]={0x00100000ull,0x00500000ull,0x00800000ull};
    check(igGtSurfaceBuildPtes(pages,3u,3u,ptes,3u)==IG_GTS_PTE_OK,
          "three legal PTEs built");
}

static void testSuccessAndExactRestore() {
    std::puts("1. prepare, commit and exact restore");
    MockPt m=freshMock();
    // Non-present metadata is legal to preserve.  The transaction must restore
    // the exact old image, not simply zero leaves on teardown.
    m.ptes[64]=0x200ull;
    m.ptes[65]=0x400ull;
    m.ptes[66]=0x600ull;
    unsigned long long ptes[3]={}; buildThree(ptes);
    unsigned long long saved[3]={};
    IGGtSurfacePteTxn txn={};
    check32(igGtSurfaceTxnPrepare(64u,ptes,3u,saved,3u,mockRead,&m,&txn),
            IG_GTS_TXN_OK,"prepare succeeds");
    check64(saved[0],0x200ull,"snapshot leaf 64");
    check64(saved[1],0x400ull,"snapshot leaf 65");
    check64(saved[2],0x600ull,"snapshot leaf 66");

    IGGtSurfacePteTxnStats stats={};
    check32(igGtSurfaceTxnCommit(&txn,ptes,saved,3u,mockRead,mockWrite,&m,&stats),
            IG_GTS_TXN_OK,"commit succeeds");
    check32(stats.writesVerified,3u,"three writes verified");
    check64(m.ptes[64],ptes[0],"leaf 64 installed");
    check64(m.ptes[65],ptes[1],"leaf 65 installed");
    check64(m.ptes[66],ptes[2],"leaf 66 installed");

    std::memset(&stats,0,sizeof(stats));
    check32(igGtSurfaceTxnRestore(&txn,ptes,saved,3u,mockRead,mockWrite,&m,&stats),
            IG_GTS_TXN_OK,"restore succeeds");
    check64(m.ptes[64],0x200ull,"leaf 64 exactly restored");
    check64(m.ptes[65],0x400ull,"leaf 65 exactly restored");
    check64(m.ptes[66],0x600ull,"leaf 66 exactly restored");
}

static void testPrepareRefusals() {
    std::puts("2. prepare refusal policy");
    MockPt m=freshMock();
    unsigned long long ptes[3]={}; buildThree(ptes);
    unsigned long long saved[3]={0xaaaau,0xbbbbu,0xccccu};
    IGGtSurfacePteTxn txn={};
    check32(igGtSurfaceTxnPrepare(63u,ptes,1u,saved,3u,mockRead,&m,&txn),
            IG_GTS_TXN_BAD_RANGE,"guard leaf refused");
    check32(igGtSurfaceTxnPrepare(511u,ptes,2u,saved,3u,mockRead,&m,&txn),
            IG_GTS_TXN_BAD_RANGE,"PT end crossing refused");

    unsigned long long bad[1]={0x00100001ull};
    check32(igGtSurfaceTxnPrepare(64u,bad,1u,saved,3u,mockRead,&m,&txn),
            IG_GTS_TXN_BAD_NEW_PTE,"malformed PTE refused");

    m.ptes[65]=0x12345003ull;
    check32(igGtSurfaceTxnPrepare(64u,ptes,3u,saved,3u,mockRead,&m,&txn),
            IG_GTS_TXN_TARGET_OCCUPIED,"present target refused");
}

static void testCommitStateChangeIsAtomic() {
    std::puts("3. commit revalidates exact snapshot");
    MockPt m=freshMock();
    unsigned long long ptes[3]={}; buildThree(ptes);
    unsigned long long saved[3]={};
    IGGtSurfacePteTxn txn={};
    check32(igGtSurfaceTxnPrepare(64u,ptes,3u,saved,3u,mockRead,&m,&txn),
            IG_GTS_TXN_OK,"prepare before state change");
    m.ptes[65]=0x400ull;
    unsigned int beforeWrites=m.writes;
    check32(igGtSurfaceTxnCommit(&txn,ptes,saved,3u,mockRead,mockWrite,&m,nullptr),
            IG_GTS_TXN_STATE_CHANGED,"state change refused");
    check32(m.writes,beforeWrites,"state change causes zero writes");
    check64(m.ptes[64],0ull,"first leaf untouched");
}

static void testCommitFailureRollsBack() {
    std::puts("4. failed install rolls back exact snapshot");
    MockPt m=freshMock();
    m.ptes[64]=0x200ull; m.ptes[65]=0x400ull; m.ptes[66]=0x600ull;
    unsigned long long ptes[3]={}; buildThree(ptes);
    unsigned long long saved[3]={};
    IGGtSurfacePteTxn txn={};
    check32(igGtSurfaceTxnPrepare(64u,ptes,3u,saved,3u,mockRead,&m,&txn),
            IG_GTS_TXN_OK,"prepare for rollback");
    m.failWriteLeaf=65;
    m.failWriteValue=ptes[1];
    IGGtSurfacePteTxnStats stats={};
    check32(igGtSurfaceTxnCommit(&txn,ptes,saved,3u,mockRead,mockWrite,&m,&stats),
            IG_GTS_TXN_WRITE_FAILED_ROLLED_BACK,"write failure rolled back");
    check32(stats.failedLeaf,65u,"failed leaf reported");
    check64(m.ptes[64],saved[0],"leaf 64 rolled back");
    check64(m.ptes[65],saved[1],"leaf 65 rolled back");
    check64(m.ptes[66],saved[2],"leaf 66 remained old");
}

static void testRollbackFailureIsLoud() {
    std::puts("5. rollback failure is not hidden");
    MockPt m=freshMock();
    unsigned long long ptes[3]={}; buildThree(ptes);
    unsigned long long saved[3]={};
    IGGtSurfacePteTxn txn={};
    check32(igGtSurfaceTxnPrepare(64u,ptes,3u,saved,3u,mockRead,&m,&txn),
            IG_GTS_TXN_OK,"prepare for rollback-failure test");
    // Corrupt leaf 65 after its install write.  Recovery then reaches leaf 64,
    // where restoring the old zero is forced to fail.
    m.corruptWriteLeaf=65;
    m.corruptWriteValue=ptes[1];
    m.failWriteLeaf=64;
    m.failWriteValue=saved[0];
    check32(igGtSurfaceTxnCommit(&txn,ptes,saved,3u,mockRead,mockWrite,&m,nullptr),
            IG_GTS_TXN_ROLLBACK_FAILED,"rollback failure surfaced");
}

static void testRestoreMismatchAndReinstall() {
    std::puts("6. teardown ownership check and failed-restore recovery");
    MockPt m=freshMock();
    unsigned long long ptes[3]={}; buildThree(ptes);
    unsigned long long saved[3]={};
    IGGtSurfacePteTxn txn={};
    check32(igGtSurfaceTxnPrepare(64u,ptes,3u,saved,3u,mockRead,&m,&txn),
            IG_GTS_TXN_OK,"prepare restore test");
    check32(igGtSurfaceTxnCommit(&txn,ptes,saved,3u,mockRead,mockWrite,&m,nullptr),
            IG_GTS_TXN_OK,"commit restore test");

    m.ptes[66]^=0x1000ull;
    unsigned int beforeWrites=m.writes;
    check32(igGtSurfaceTxnRestore(&txn,ptes,saved,3u,mockRead,mockWrite,&m,nullptr),
            IG_GTS_TXN_RESTORE_STATE_MISMATCH,"foreign mutation blocks restore");
    check32(m.writes,beforeWrites,"restore mismatch causes zero writes");
    m.ptes[66]=ptes[2];

    m.failWriteLeaf=65;
    m.failWriteValue=saved[1];
    m.failConsumed=false;
    IGGtSurfacePteTxnStats stats={};
    check32(igGtSurfaceTxnRestore(&txn,ptes,saved,3u,mockRead,mockWrite,&m,&stats),
            IG_GTS_TXN_RESTORE_FAILED_REINSTALLED,"failed restore reinstalls image");
    check32(stats.failedLeaf,65u,"restore failed leaf reported");
    check64(m.ptes[64],ptes[0],"leaf 64 reinstalled");
    check64(m.ptes[65],ptes[1],"leaf 65 remains installed");
    check64(m.ptes[66],ptes[2],"leaf 66 remains installed");
}

int main() {
    testSuccessAndExactRestore();
    testPrepareRefusals();
    testCommitStateChangeIsAtomic();
    testCommitFailureRollsBack();
    testRollbackFailureIsLoud();
    testRestoreMismatchAndReinstall();
    if (failures) {
        std::printf("%d failure(s)\n",failures);
        return 1;
    }
    std::puts("Tiger Lake persistent surface PPGTT transaction tests passed");
    return 0;
}
