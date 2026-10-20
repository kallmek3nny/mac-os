// SPDX-License-Identifier: MIT
// Pure RM/GSP RX ordering regression. No IOKit, GPU memory or MMIO involved.
#include <cstdio>
#include <cstring>
#include "NVGspRpcReceiveTracker.hpp"

static unsigned checks=0u, failures=0u;
static void check(const char *label,bool ok) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n",label); }
}
static void eq(const char *label,NVGspRxTrackU32 got,NVGspRxTrackU32 expected) {
    check(label,got==expected);
}
static NVGspRpcTransportPlan transport(NVGspRxTrackU32 fn,NVGspRxTrackU32 seq) {
    NVGspQueuePlan layout={};
    eq("queue layout",nvGspBuildQueuePlan(true,false,0u,&layout),NV_GSP_QUEUE_PLAN_OK);
    NVGspRpcTransportPlan plan={};
    eq("response-sized transport",nvGspPlanRpcTransport(
        &layout,fn,seq,0u,0u,0u,0u,0u,0u,1u,56u,&plan),
        NV_GSP_RPC_TXN_OK);
    return plan;
}
static NVGspRpcFramePlan response(NVGspRxTrackU32 fn,NVGspRxTrackU32 seq,
                                   NVGspRxTrackU32 result=0u) {
    NVGspRpcFramePlan frame={};
    eq("frame",nvGspBuildRmRpcFrameNoCc(fn,seq,0u,&frame),NV_GSP_RPC_FRAME_OK);
    frame.prefix[10]=result;
    return frame;
}
static void testInitAndIdentity(void) {
    NVGspRpcReceiveTracker state={};
    const NVGspRpcTransportPlan original=transport(0x100u,7u);
    eq("no begin before init",nvGspRpcReceiveTrackerBegin(&state,&original),
       NV_GSP_RX_TRACK_UNINITIALIZED);
    eq("init",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    eq("duplicate init refuses",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_REINIT);
    eq("first begin",nvGspRpcReceiveTrackerBegin(&state,&original),NV_GSP_RX_TRACK_OK);
    check("active binds expected identity",state.active &&
          state.function==0x100u && state.sequence==7u);
    check("begin does not authorize success",!state.recordSucceeded && !state.awaitingConsume);
    eq("second request blocked",nvGspRpcReceiveTrackerBegin(&state,&original),
       NV_GSP_RX_TRACK_BUSY);
    eq("no consume without observed record",
       nvGspRpcReceiveTrackerConfirmConsumed(&state),NV_GSP_RX_TRACK_AWAIT_CONSUME);
    eq("bad transport blocked even before activation",
       nvGspRpcReceiveTrackerBegin(nullptr,&original),NV_GSP_RX_TRACK_NULL);
    NVGspRpcReceiveTracker never={};
    eq("uninitialized observe",nvGspRpcReceiveTrackerObserve(&never,nullptr,56u),
       NV_GSP_RX_TRACK_NULL);
}

static void testPendingAndConsumeOrder(void) {
    NVGspRpcReceiveTracker state={};
    eq("init pending state",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan req=transport(0x101u,11u);
    eq("begin pending",nvGspRpcReceiveTrackerBegin(&state,&req),NV_GSP_RX_TRACK_OK);
    eq("pending request publication",nvGspRpcReceiveTrackerRecordPublication(&state,0x101u,11u,true),
       NV_GSP_RX_TRACK_OK);
    NVGspRpcFramePlan reply=response(0x101u,11u,NV_GSP_RPC_RESULT_PENDING);
    eq("pending sentinel not an error",
       nvGspRpcReceiveTrackerObserve(&state,reply.prefix,56u),
       NV_GSP_RX_TRACK_RESULT_PENDING);
    check("pending does not advance RX evidence",
          state.active && !state.awaitingConsume && !state.recordSucceeded);
    eq("pending cannot consume",nvGspRpcReceiveTrackerConfirmConsumed(&state),
       NV_GSP_RX_TRACK_AWAIT_CONSUME);
    reply.prefix[10]=0u;
    eq("matched result is provisional",
       nvGspRpcReceiveTrackerObserve(&state,reply.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    check("matched still owns transaction until RX tail confirmation",
          state.active && state.awaitingConsume && state.recordSucceeded);
    eq("duplicate receive blocked until consume",
       nvGspRpcReceiveTrackerObserve(&state,reply.prefix,56u),
       NV_GSP_RX_TRACK_AWAIT_CONSUME);
    eq("another request blocked until consume",
       nvGspRpcReceiveTrackerBegin(&state,&req),NV_GSP_RX_TRACK_BUSY);
    eq("matched reply retired only after consume",
       nvGspRpcReceiveTrackerConfirmConsumed(&state),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    check("retirement completes single flight",
          !state.active && !state.awaitingConsume && state.hasLastSequence);
    eq("replay blocked after completion",nvGspRpcReceiveTrackerObserve(
        &state,reply.prefix,56u),NV_GSP_RX_TRACK_NO_REQUEST);
    eq("duplicate request sequence blocked",
       nvGspRpcReceiveTrackerBegin(&state,&req),NV_GSP_RX_TRACK_SEQUENCE);
    const NVGspRpcTransportPlan next=transport(0x101u,12u);
    eq("monotonic next sequence allowed",
       nvGspRpcReceiveTrackerBegin(&state,&next),NV_GSP_RX_TRACK_OK);
}

static void testEventsAndRemoteErrors(void) {
    NVGspRpcReceiveTracker state={};
    eq("init event",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan request=transport(0x400u,30u);
    eq("begin event",nvGspRpcReceiveTrackerBegin(&state,&request),NV_GSP_RX_TRACK_OK);
    eq("event request publication",nvGspRpcReceiveTrackerRecordPublication(&state,0x400u,30u,true),
       NV_GSP_RX_TRACK_OK);
    NVGspRpcFramePlan event=response(0x500u,31u,0u);
    eq("distinct event classified, not completion",
       nvGspRpcReceiveTrackerObserve(&state,event.prefix,56u),
       NV_GSP_RX_TRACK_ASYNC_EVENT);
    check("event owns pending consumption",
          state.active && state.awaitingConsume && !state.recordMatched);
    eq("event consumed without completing request",
       nvGspRpcReceiveTrackerConfirmConsumed(&state),NV_GSP_RX_TRACK_ASYNC_EVENT);
    check("request remains live",state.active && !state.awaitingConsume &&
          state.asyncRecordCount==1u);
    NVGspRpcFramePlan remoteErr=response(0x400u,30u,5u);
    eq("matched RM error distinguished",
       nvGspRpcReceiveTrackerObserve(&state,remoteErr.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_ERROR);
    check("error still pending consume",state.active && state.awaitingConsume &&
          !state.recordSucceeded);
    eq("error retires as error",nvGspRpcReceiveTrackerConfirmConsumed(&state),
       NV_GSP_RX_TRACK_MATCHED_ERROR);
    check("no false success",!state.recordSucceeded && !state.active);
}

static void testPartialMismatchQuarantine(void) {
    for (unsigned i=0u;i<2u;++i) {
        NVGspRpcReceiveTracker state={};
        eq("init collision",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
        const NVGspRpcTransportPlan p=transport(0x701u,4u);
        eq("begin collision",nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_OK);
        eq("collision request publication",nvGspRpcReceiveTrackerRecordPublication(&state,0x701u,4u,true),
           NV_GSP_RX_TRACK_OK);
        const NVGspRpcFramePlan reply=i==0u
            ? response(0x701u,5u) : response(0x702u,4u);
        eq("one matching identity axis is ambiguous",
           nvGspRpcReceiveTrackerObserve(&state,reply.prefix,56u),
           NV_GSP_RX_TRACK_ID_COLLISION);
        check("identity collision stays quarantined",state.quarantined && state.active);
        eq("collision cannot consume",
           nvGspRpcReceiveTrackerConfirmConsumed(&state),NV_GSP_RX_TRACK_QUARANTINED);
        eq("collision cannot start another RPC",
           nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_QUARANTINED);
    }
}

static void testCorruptReceiveQuarantine(void) {
    NVGspRpcReceiveTracker state={};
    eq("init malformed",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan p=transport(0x888u,8u);
    eq("begin malformed",nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_OK);
    eq("malformed request publication",nvGspRpcReceiveTrackerRecordPublication(&state,0x888u,8u,true),
       NV_GSP_RX_TRACK_OK);
    NVGspRpcFramePlan bad=response(0x888u,8u);
    bad.prefix[7]^=1u;
    eq("bad signature quarantines unknown RX ownership",
       nvGspRpcReceiveTrackerObserve(&state,bad.prefix,56u),
       NV_GSP_RX_TRACK_MALFORMED);
    check("malformed cannot be retired as success",state.quarantined &&
          state.active && !state.awaitingConsume);
    eq("malformed disallows further observation",
       nvGspRpcReceiveTrackerObserve(&state,bad.prefix,56u),
       NV_GSP_RX_TRACK_QUARANTINED);
    eq("no reset after malformed",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_REINIT);
    eq("stop retains quarantined",nvGspRpcReceiveTrackerStop(&state),
       NV_GSP_RX_TRACK_QUARANTINED);
    eq("stop cannot restore readiness",nvGspRpcReceiveTrackerBegin(&state,&p),
       NV_GSP_RX_TRACK_STOPPED);
}

static void testBoundedEventsAndStop(void) {
    NVGspRpcReceiveTracker state={};
    eq("init bound",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan p=transport(0x345u,2u);
    eq("begin bound",nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_OK);
    eq("bounded request publication",nvGspRpcReceiveTrackerRecordPublication(&state,0x345u,2u,true),
       NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan event=response(0x999u,3u);
    for (unsigned i=0;i<NV_GSP_RX_TRACK_MAX_ASYNC_RECORDS;++i) {
        eq("bounded async classification",
           nvGspRpcReceiveTrackerObserve(&state,event.prefix,56u),
           NV_GSP_RX_TRACK_ASYNC_EVENT);
        eq("bounded async consume",nvGspRpcReceiveTrackerConfirmConsumed(&state),
           NV_GSP_RX_TRACK_ASYNC_EVENT);
    }
    check("bounded record count",state.asyncRecordCount==NV_GSP_RX_TRACK_MAX_ASYNC_RECORDS);
    eq("extra event quarantines",nvGspRpcReceiveTrackerObserve(
        &state,event.prefix,56u),NV_GSP_RX_TRACK_QUARANTINED);
    check("terminal async overflow",state.quarantined);
    eq("stop does not claim retirement",nvGspRpcReceiveTrackerStop(&state),
       NV_GSP_RX_TRACK_QUARANTINED);

    NVGspRpcReceiveTracker idle={};
    eq("init idle",nvGspRpcReceiveTrackerInit(&idle),NV_GSP_RX_TRACK_OK);
    eq("idle stop",nvGspRpcReceiveTrackerStop(&idle),NV_GSP_RX_TRACK_OK);
    eq("stopped cannot begin",nvGspRpcReceiveTrackerBegin(&idle,&p),
       NV_GSP_RX_TRACK_STOPPED);
}

static void testUncertainAndSequenceRollover(void) {
    NVGspRpcReceiveTracker state={};
    eq("init rollover",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan high=transport(0x121u,0xffffffffu);
    eq("begin last sequence",nvGspRpcReceiveTrackerBegin(&state,&high),NV_GSP_RX_TRACK_OK);
    eq("last sequence publication",nvGspRpcReceiveTrackerRecordPublication(&state,0x121u,0xffffffffu,true),
       NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan ack=response(0x121u,0xffffffffu);
    eq("last sequence reply",nvGspRpcReceiveTrackerObserve(&state,ack.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    eq("last sequence consumes",nvGspRpcReceiveTrackerConfirmConsumed(&state),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    const NVGspRpcTransportPlan zero=transport(0x121u,0u);
    eq("natural wrap to zero",nvGspRpcReceiveTrackerBegin(&state,&zero),
       NV_GSP_RX_TRACK_OK);
    check("wrap not success proof",state.active && !state.recordSucceeded);
    eq("uncertain retirement quarantines",
       nvGspRpcReceiveTrackerRetirementUncertain(&state),
       NV_GSP_RX_TRACK_QUARANTINED);
    eq("terminal poison survives",nvGspRpcReceiveTrackerRetirementUncertain(&state),
       NV_GSP_RX_TRACK_QUARANTINED);
    eq("cannot finish uncertain receipt",nvGspRpcReceiveTrackerConfirmConsumed(&state),
       NV_GSP_RX_TRACK_QUARANTINED);
}


static void testPinnedReceiveGeometry(void) {
    NVGspRpcReceiveTracker state={};
    eq("init geometry",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    NVGspRpcTransportPlan p=transport(0xaccu,91u);
    const NVGspRpcReceiveTracker original=state;
    ++p.responseConsume.oldReadPtr;
    eq("mismatched receive pointer refused at begin",
       nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_BAD_TRANSPORT);
    check("geometry-refused begin atomic",std::memcmp(&state,&original,sizeof(state))==0);
    p=transport(0xaccu,91u);
    ++p.responseElement[0].sharedByteOffset;
    eq("mismatched response slot offset refused",
       nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_BAD_TRANSPORT);
    p=transport(0xaccu,91u);
    ++p.responseConsume.remainingAvailable;
    eq("invalid remaining element count refused",
       nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_BAD_TRANSPORT);
    p=transport(0xaccu,91u);
    p.responseElementCount=0u;
    eq("missing response elements refused",
       nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_BAD_TRANSPORT);
    check("all malformed plans leave session reusable",
          std::memcmp(&state,&original,sizeof(state))==0);

    p=transport(0xaccu,91u);
    eq("begin valid response geometry",
       nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_OK);
    eq("geometry request publication",nvGspRpcReceiveTrackerRecordPublication(&state,0xaccu,91u,true),
       NV_GSP_RX_TRACK_OK);
    check("bounded geometry held by tracker",
          state.expectedRecordBytes==p.responseRecordBytes &&
          state.expectedResponseElements==p.responseElementCount);
    const NVGspRpcFramePlan reply=response(0xaccu,91u);
    eq("different complete RX bytes quarantine",
       nvGspRpcReceiveTrackerObserve(&state,reply.prefix,60u),
       NV_GSP_RX_TRACK_GEOMETRY_MISMATCH);
    check("geometry mismatch never records successful receive",
          state.quarantined && state.active &&
          !state.awaitingConsume && !state.recordSucceeded);
    eq("right-size replay cannot remove quarantine",
       nvGspRpcReceiveTrackerObserve(&state,reply.prefix,56u),
       NV_GSP_RX_TRACK_QUARANTINED);
    eq("wrong-size response cannot retire",
       nvGspRpcReceiveTrackerConfirmConsumed(&state),NV_GSP_RX_TRACK_QUARANTINED);
    eq("stopped poisoned receive remains poisoned",
       nvGspRpcReceiveTrackerStop(&state),NV_GSP_RX_TRACK_QUARANTINED);
}

static void testBadTransportAtomicity(void) {
    NVGspRpcReceiveTracker state={};
    eq("init transport",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    NVGspRpcTransportPlan p=transport(0x5u,100u);
    const NVGspRpcReceiveTracker prior=state;
    ++p.requestFrame.prefix[12];
    eq("forged frame sequence rejected",
       nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_BAD_TRANSPORT);
    check("bad begin leaves session intact",
          std::memcmp(&state,&prior,sizeof(state))==0);
    p=transport(0x5u,100u);
    eq("valid plan accepted after refusal",
       nvGspRpcReceiveTrackerBegin(&state,&p),NV_GSP_RX_TRACK_OK);
    eq("provider stop with outstanding request quarantines",
       nvGspRpcReceiveTrackerStop(&state),NV_GSP_RX_TRACK_QUARANTINED);
}

static void testPublicationReceiptGate(void) {
    NVGspRpcReceiveTracker state={};
    eq("init publication",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan plan=transport(0xa00u,0x44u);
    eq("begin planned-only",nvGspRpcReceiveTrackerBegin(&state,&plan),
       NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan ack=response(0xa00u,0x44u);
    const NVGspRpcReceiveTracker planned=state;
    eq("planned request cannot consume apparent success",
       nvGspRpcReceiveTrackerObserve(&state,ack.prefix,56u),
       NV_GSP_RX_TRACK_NOT_PUBLISHED);
    check("planned observe refusal is atomic",
          std::memcmp(&state,&planned,sizeof(state))==0);
    eq("denied publication is atomic",
       nvGspRpcReceiveTrackerRecordPublication(&state,0xa00u,0x44u,false),
       NV_GSP_RX_TRACK_PUBLISH_UNCONFIRMED);
    check("denied publication retains planned identity",
          std::memcmp(&state,&planned,sizeof(state))==0);
    eq("one matching provider-reported publication",
       nvGspRpcReceiveTrackerRecordPublication(&state,0xa00u,0x44u,true),
       NV_GSP_RX_TRACK_OK);
    check("publisher witness does not complete request",
          state.active && state.requestPublished && !state.awaitingConsume);
    eq("repeat publication never accepted as a second send",
       nvGspRpcReceiveTrackerRecordPublication(&state,0xa00u,0x44u,true),
       NV_GSP_RX_TRACK_PUBLISH_DUPLICATE);
    eq("receive after publication is provisional",
       nvGspRpcReceiveTrackerObserve(&state,ack.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    eq("no late publication during awaiting tail",
       nvGspRpcReceiveTrackerRecordPublication(&state,0xa00u,0x44u,true),
       NV_GSP_RX_TRACK_AWAIT_CONSUME);
    eq("post-tail retirement",nvGspRpcReceiveTrackerConfirmConsumed(&state),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    check("receipt and active lease both cleared on completed request",
          !state.active && !state.requestPublished);
    eq("publication after retirement cannot resurrect request",
       nvGspRpcReceiveTrackerRecordPublication(&state,0xa00u,0x44u,true),
       NV_GSP_RX_TRACK_NO_REQUEST);

    NVGspRpcReceiveTracker mismatch={};
    eq("init mismatch",nvGspRpcReceiveTrackerInit(&mismatch),NV_GSP_RX_TRACK_OK);
    eq("begin mismatch",nvGspRpcReceiveTrackerBegin(&mismatch,&plan),
       NV_GSP_RX_TRACK_OK);
    eq("wrong provider-reported TX identity quarantined",
       nvGspRpcReceiveTrackerRecordPublication(&mismatch,0xa00u,0x45u,true),
       NV_GSP_RX_TRACK_PUBLISH_IDENTITY);
    check("misidentified publication preserves active uncertainty",
          mismatch.quarantined && mismatch.active && !mismatch.requestPublished);
    eq("corrected publication not accepted after mismatch",
       nvGspRpcReceiveTrackerRecordPublication(&mismatch,0xa00u,0x44u,true),
       NV_GSP_RX_TRACK_QUARANTINED);
}

int main(void) {
    std::printf("GSP-RM single-flight response receive/retirement guards\n");
    testInitAndIdentity();
    testPendingAndConsumeOrder();
    testEventsAndRemoteErrors();
    testPartialMismatchQuarantine();
    testCorruptReceiveQuarantine();
    testBoundedEventsAndStop();
    testUncertainAndSequenceRollover();
    testPinnedReceiveGeometry();
    testBadTransportAtomicity();
    testPublicationReceiptGate();
    std::printf("%u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
