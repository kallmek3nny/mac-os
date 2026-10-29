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
// Test-only stand-in for a live provider: report one fresh, normalized RX
// HEAD covering exactly the next single-flight record at the tracker's
// expected reader, then classify that record. When the witness gate refuses
// for a lifecycle reason (unpublished, awaiting consume, quarantined, ...)
// the tracker is unchanged and Observe re-reports the same guard. Tests of
// the witness gate itself call nvGspRpcReceiveTrackerRecordAvailability.
static NVGspRxTrackU32 observeFresh(NVGspRpcReceiveTracker *state,
                                    const NVGspRespU32 *prefix,
                                    NVGspRxTrackU32 recordBytes) {
    if (state)
        (void)nvGspRpcReceiveTrackerRecordAvailability(state,
            static_cast<NVGspRxTrackU32>(state->expectedReadPtr+
                                         state->expectedResponseElements),true);
    return nvGspRpcReceiveTrackerObserve(state,prefix,recordBytes);
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
       nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),NV_GSP_RX_TRACK_AWAIT_CONSUME);
    eq("bad transport blocked even before activation",
       nvGspRpcReceiveTrackerBegin(nullptr,&original),NV_GSP_RX_TRACK_NULL);
    NVGspRpcReceiveTracker never={};
    eq("uninitialized observe",observeFresh(&never,nullptr,56u),
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
       observeFresh(&state,reply.prefix,56u),
       NV_GSP_RX_TRACK_RESULT_PENDING);
    check("pending does not advance RX evidence",
          state.active && !state.awaitingConsume && !state.recordSucceeded);
    eq("pending cannot consume",nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),
       NV_GSP_RX_TRACK_AWAIT_CONSUME);
    reply.prefix[10]=0u;
    eq("matched result is provisional",
       observeFresh(&state,reply.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    check("matched still owns transaction until RX tail confirmation",
          state.active && state.awaitingConsume && state.recordSucceeded);
    eq("duplicate receive blocked until consume",
       observeFresh(&state,reply.prefix,56u),
       NV_GSP_RX_TRACK_AWAIT_CONSUME);
    eq("another request blocked until consume",
       nvGspRpcReceiveTrackerBegin(&state,&req),NV_GSP_RX_TRACK_BUSY);
    eq("matched reply retired only after consume",
       nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    check("retirement completes single flight",
          !state.active && !state.awaitingConsume && state.hasLastSequence);
    eq("replay blocked after completion",observeFresh(
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
       observeFresh(&state,event.prefix,56u),
       NV_GSP_RX_TRACK_ASYNC_EVENT);
    check("event owns pending consumption",
          state.active && state.awaitingConsume && !state.recordMatched);
    eq("event consumed without completing request",
       nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),NV_GSP_RX_TRACK_ASYNC_EVENT);
    check("request remains live",state.active && !state.awaitingConsume &&
          state.asyncRecordCount==1u);
    NVGspRpcFramePlan remoteErr=response(0x400u,30u,5u);
    eq("matched RM error distinguished",
       observeFresh(&state,remoteErr.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_ERROR);
    check("error still pending consume",state.active && state.awaitingConsume &&
          !state.recordSucceeded);
    eq("error retires as error",nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),
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
           observeFresh(&state,reply.prefix,56u),
           NV_GSP_RX_TRACK_ID_COLLISION);
        check("identity collision stays quarantined",state.quarantined && state.active);
        eq("collision cannot consume",
           nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),NV_GSP_RX_TRACK_QUARANTINED);
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
       observeFresh(&state,bad.prefix,56u),
       NV_GSP_RX_TRACK_MALFORMED);
    check("malformed cannot be retired as success",state.quarantined &&
          state.active && !state.awaitingConsume);
    eq("malformed disallows further observation",
       observeFresh(&state,bad.prefix,56u),
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
           observeFresh(&state,event.prefix,56u),
           NV_GSP_RX_TRACK_ASYNC_EVENT);
        eq("bounded async consume",nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),
           NV_GSP_RX_TRACK_ASYNC_EVENT);
    }
    check("bounded record count",state.asyncRecordCount==NV_GSP_RX_TRACK_MAX_ASYNC_RECORDS);
    eq("extra event quarantines",observeFresh(
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
    eq("last sequence reply",observeFresh(&state,ack.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    eq("last sequence consumes",nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),
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
    eq("cannot finish uncertain receipt",nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),
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
       observeFresh(&state,reply.prefix,60u),
       NV_GSP_RX_TRACK_GEOMETRY_MISMATCH);
    check("geometry mismatch never records successful receive",
          state.quarantined && state.active &&
          !state.awaitingConsume && !state.recordSucceeded);
    eq("right-size replay cannot remove quarantine",
       observeFresh(&state,reply.prefix,56u),
       NV_GSP_RX_TRACK_QUARANTINED);
    eq("wrong-size response cannot retire",
       nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),NV_GSP_RX_TRACK_QUARANTINED);
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
       observeFresh(&state,ack.prefix,56u),
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
       observeFresh(&state,ack.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    eq("no late publication during awaiting tail",
       nvGspRpcReceiveTrackerRecordPublication(&state,0xa00u,0x44u,true),
       NV_GSP_RX_TRACK_AWAIT_CONSUME);
    eq("post-tail retirement",nvGspRpcReceiveTrackerConfirmConsumed(&state,state.expectedConsumeTail,true),
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

static void testConsumeTailPublicationWitness(void) {
    NVGspRpcReceiveTracker pending={};
    eq("init tail",nvGspRpcReceiveTrackerInit(&pending),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan plan=transport(0xa11u,0x55u);
    eq("begin tail",nvGspRpcReceiveTrackerBegin(&pending,&plan),NV_GSP_RX_TRACK_OK);
    check("tail bound to original planned consume",pending.expectedConsumeTail==
          plan.responseConsume.messageTailValue && pending.expectedConsumeTail==1u);
    eq("TX published for tail",nvGspRpcReceiveTrackerRecordPublication(
        &pending,0xa11u,0x55u,true),NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan ack=response(0xa11u,0x55u);
    eq("record provisional before tail witness",observeFresh(
        &pending,ack.prefix,56u),NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    const NVGspRpcReceiveTracker pendingCopy=pending;
    eq("unconfirmed RX tail cannot retire",
       nvGspRpcReceiveTrackerConfirmConsumed(&pending,1u,false),
       NV_GSP_RX_TRACK_TAIL_UNCONFIRMED);
    check("unconfirmed tail leaves exactly the same pending record",
          std::memcmp(&pending,&pendingCopy,sizeof(pending))==0);
    eq("misaddressed provider-confirmed tail quarantines",
       nvGspRpcReceiveTrackerConfirmConsumed(&pending,2u,true),
       NV_GSP_RX_TRACK_TAIL_MISMATCH);
    check("wrong tail retains uncertain ownership",
          pending.quarantined && pending.active && pending.awaitingConsume);
    eq("correct tail cannot cure a mismatched publication",
       nvGspRpcReceiveTrackerConfirmConsumed(&pending,1u,true),
       NV_GSP_RX_TRACK_QUARANTINED);

    NVGspRpcReceiveTracker events={};
    eq("init tail events",nvGspRpcReceiveTrackerInit(&events),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan eventPlan=transport(0xa22u,0x56u);
    eq("begin tail events",nvGspRpcReceiveTrackerBegin(&events,&eventPlan),
       NV_GSP_RX_TRACK_OK);
    eq("publish tail events",nvGspRpcReceiveTrackerRecordPublication(
        &events,0xa22u,0x56u,true),NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan asynchronous=response(0xa23u,0x57u);
    eq("observe independent async",observeFresh(
        &events,asynchronous.prefix,56u),NV_GSP_RX_TRACK_ASYNC_EVENT);
    eq("first RX tail publication",nvGspRpcReceiveTrackerConfirmConsumed(
        &events,1u,true),NV_GSP_RX_TRACK_ASYNC_EVENT);
    check("async consume advances required next RX tail",
          events.active && events.expectedConsumeTail==2u &&
          events.asyncRecordCount==1u);
    const NVGspRpcFramePlan matched=response(0xa22u,0x56u);
    eq("matching response after async",observeFresh(
        &events,matched.prefix,56u),NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    eq("second RX tail publication",nvGspRpcReceiveTrackerConfirmConsumed(
        &events,2u,true),NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    check("completion retires only after correct incremented tail",
          !events.active && events.hasLastSequence && !events.quarantined);

    // Nvidia's queue read/write pointers are monotonic NvU32 counters.
    // The final slot may legitimately retire across the 32-bit wrap.
    NVGspQueuePlan layout={};
    eq("wrap queue layout",nvGspBuildQueuePlan(true,false,0u,&layout),
       NV_GSP_QUEUE_PLAN_OK);
    NVGspRpcTransportPlan rollover={};
    eq("wrap receive plan",nvGspPlanRpcTransport(
        &layout,0xa33u,0x57u,0u,0u,0u,0u,
        0xffffffffu,0xffffffffu,0u,56u,&rollover),NV_GSP_RPC_TXN_OK);
    NVGspRpcReceiveTracker wrapped={};
    eq("init wrap",nvGspRpcReceiveTrackerInit(&wrapped),NV_GSP_RX_TRACK_OK);
    eq("begin wrap",nvGspRpcReceiveTrackerBegin(&wrapped,&rollover),
       NV_GSP_RX_TRACK_OK);
    check("full 32-bit RX tail wrap accepts zero",wrapped.expectedConsumeTail==0u);
    eq("publish wrap",nvGspRpcReceiveTrackerRecordPublication(
        &wrapped,0xa33u,0x57u,true),NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan wrapAck=response(0xa33u,0x57u);
    eq("observe wrapped response",observeFresh(
        &wrapped,wrapAck.prefix,56u),NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    eq("confirmed zero-tail retirement",nvGspRpcReceiveTrackerConfirmConsumed(
        &wrapped,0u,true),NV_GSP_RX_TRACK_MATCHED_SUCCESS);
}

// The RX HEAD witness is a separate per-record provider fact. The planned
// transport writer is never reused as evidence, a refused report changes no
// tracker state, one witness arms exactly one read, and a writer outside the
// planned ring capacity from the expected reader is terminal quarantine.
static void testRxHeadAvailabilityWitness(void) {
    NVGspRpcReceiveTracker idle={};
    eq("null head witness",nvGspRpcReceiveTrackerRecordAvailability(
        nullptr,1u,true),NV_GSP_RX_TRACK_NULL);
    eq("uninitialized head witness",nvGspRpcReceiveTrackerRecordAvailability(
        &idle,1u,true),NV_GSP_RX_TRACK_UNINITIALIZED);
    eq("init idle head",nvGspRpcReceiveTrackerInit(&idle),NV_GSP_RX_TRACK_OK);
    eq("head witness needs an active request",
       nvGspRpcReceiveTrackerRecordAvailability(&idle,1u,true),
       NV_GSP_RX_TRACK_NO_REQUEST);

    NVGspQueuePlan layout={};
    eq("head queue layout",nvGspBuildQueuePlan(true,false,0u,&layout),
       NV_GSP_QUEUE_PLAN_OK);
    NVGspRpcTransportPlan ahead={};
    eq("plan with writer three records ahead",nvGspPlanRpcTransport(
        &layout,0xb11u,0x61u,0u,0u,0u,0u,0u,0u,3u,56u,&ahead),
        NV_GSP_RPC_TXN_OK);
    NVGspRpcReceiveTracker state={};
    eq("init head state",nvGspRpcReceiveTrackerInit(&state),NV_GSP_RX_TRACK_OK);
    eq("begin head state",nvGspRpcReceiveTrackerBegin(&state,&ahead),
       NV_GSP_RX_TRACK_OK);
    check("begin binds planned reader and capacity, not the planned writer",
          state.expectedReadPtr==ahead.responseAvailable.readPtr &&
          state.rxMessageCount==ahead.responseAvailable.messageCount &&
          ahead.responseAvailable.available==3u && !state.recordAvailable);
    eq("no head witness before TX publication",
       nvGspRpcReceiveTrackerRecordAvailability(&state,1u,true),
       NV_GSP_RX_TRACK_NOT_PUBLISHED);
    check("unpublished head grants nothing",!state.recordAvailable);
    eq("publish head state",nvGspRpcReceiveTrackerRecordPublication(
        &state,0xb11u,0x61u,true),NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan ack=response(0xb11u,0x61u);
    const NVGspRpcReceiveTracker unarmed=state;
    eq("planned writer is not an RX head witness",
       nvGspRpcReceiveTrackerObserve(&state,ack.prefix,56u),
       NV_GSP_RX_TRACK_NOT_AVAILABLE);
    check("refused read leaves tracker byte-identical",
          std::memcmp(&state,&unarmed,sizeof(state))==0);
    eq("unconfirmed head authorizes nothing",
       nvGspRpcReceiveTrackerRecordAvailability(&state,1u,false),
       NV_GSP_RX_TRACK_HEAD_UNCONFIRMED);
    check("unconfirmed head changes no state",
          std::memcmp(&state,&unarmed,sizeof(state))==0);
    eq("writer at the reader is an empty queue",
       nvGspRpcReceiveTrackerRecordAvailability(&state,0u,true),
       NV_GSP_RX_TRACK_HEAD_EMPTY);
    check("empty head changes no state",
          std::memcmp(&state,&unarmed,sizeof(state))==0);
    eq("empty head still blocks the read",
       nvGspRpcReceiveTrackerObserve(&state,ack.prefix,56u),
       NV_GSP_RX_TRACK_NOT_AVAILABLE);
    eq("fresh head covering one record",
       nvGspRpcReceiveTrackerRecordAvailability(&state,1u,true),
       NV_GSP_RX_TRACK_OK);
    check("witness arms exactly one read",
          state.recordAvailable && !state.awaitingConsume);
    const NVGspRpcReceiveTracker armed=state;
    eq("second head for the same read refuses",
       nvGspRpcReceiveTrackerRecordAvailability(&state,1u,true),
       NV_GSP_RX_TRACK_HEAD_DUPLICATE);
    check("duplicate head changes no state",
          std::memcmp(&state,&armed,sizeof(state))==0);
    eq("witnessed record classifies",
       nvGspRpcReceiveTrackerObserve(&state,ack.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    check("classification spends the witness",
          !state.recordAvailable && state.awaitingConsume);
    eq("no head witness while a record awaits consume",
       nvGspRpcReceiveTrackerRecordAvailability(&state,1u,true),
       NV_GSP_RX_TRACK_AWAIT_CONSUME);
    eq("retire witnessed record",nvGspRpcReceiveTrackerConfirmConsumed(
        &state,1u,true),NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    check("retirement leaves no residual witness",
          !state.active && !state.recordAvailable && !state.quarantined);
    eq("retired request accepts no head",
       nvGspRpcReceiveTrackerRecordAvailability(&state,2u,true),
       NV_GSP_RX_TRACK_NO_REQUEST);

    NVGspRpcReceiveTracker pending={};
    eq("init head pending",nvGspRpcReceiveTrackerInit(&pending),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan pendingPlan=transport(0xb22u,0x62u);
    eq("begin head pending",nvGspRpcReceiveTrackerBegin(&pending,&pendingPlan),
       NV_GSP_RX_TRACK_OK);
    eq("publish head pending",nvGspRpcReceiveTrackerRecordPublication(
        &pending,0xb22u,0x62u,true),NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan unfinished=response(0xb22u,0x62u,NV_GSP_RPC_RESULT_PENDING);
    const NVGspRpcFramePlan finished=response(0xb22u,0x62u);
    eq("head for the pending record",
       nvGspRpcReceiveTrackerRecordAvailability(&pending,1u,true),
       NV_GSP_RX_TRACK_OK);
    eq("pending sentinel under a head witness",
       nvGspRpcReceiveTrackerObserve(&pending,unfinished.prefix,56u),
       NV_GSP_RX_TRACK_RESULT_PENDING);
    check("pending reply spends the witness without consuming",
          !pending.recordAvailable && !pending.awaitingConsume &&
          pending.active && pending.expectedReadPtr==0u);
    eq("re-read of the pending record needs a newer head",
       nvGspRpcReceiveTrackerObserve(&pending,finished.prefix,56u),
       NV_GSP_RX_TRACK_NOT_AVAILABLE);
    eq("newer head re-arms the same record",
       nvGspRpcReceiveTrackerRecordAvailability(&pending,1u,true),
       NV_GSP_RX_TRACK_OK);
    eq("completed reply under the newer head",
       nvGspRpcReceiveTrackerObserve(&pending,finished.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    eq("retire completed pending record",nvGspRpcReceiveTrackerConfirmConsumed(
        &pending,1u,true),NV_GSP_RX_TRACK_MATCHED_SUCCESS);

    NVGspRpcReceiveTracker events={};
    eq("init head events",nvGspRpcReceiveTrackerInit(&events),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan eventPlan=transport(0xb33u,0x63u);
    eq("begin head events",nvGspRpcReceiveTrackerBegin(&events,&eventPlan),
       NV_GSP_RX_TRACK_OK);
    eq("publish head events",nvGspRpcReceiveTrackerRecordPublication(
        &events,0xb33u,0x63u,true),NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan event=response(0xb34u,0x64u);
    const NVGspRpcFramePlan reply=response(0xb33u,0x63u);
    eq("head for the async record",
       nvGspRpcReceiveTrackerRecordAvailability(&events,1u,true),
       NV_GSP_RX_TRACK_OK);
    eq("async record under head",nvGspRpcReceiveTrackerObserve(
        &events,event.prefix,56u),NV_GSP_RX_TRACK_ASYNC_EVENT);
    eq("retire async record",nvGspRpcReceiveTrackerConfirmConsumed(
        &events,1u,true),NV_GSP_RX_TRACK_ASYNC_EVENT);
    check("async retirement moves the reader to the retired tail",
          events.expectedReadPtr==1u && events.expectedConsumeTail==2u &&
          !events.recordAvailable && events.active);
    eq("stale head from the consumed record is empty",
       nvGspRpcReceiveTrackerRecordAvailability(&events,1u,true),
       NV_GSP_RX_TRACK_HEAD_EMPTY);
    eq("next record needs its own head",
       nvGspRpcReceiveTrackerObserve(&events,reply.prefix,56u),
       NV_GSP_RX_TRACK_NOT_AVAILABLE);
    eq("fresh head for the next record",
       nvGspRpcReceiveTrackerRecordAvailability(&events,2u,true),
       NV_GSP_RX_TRACK_OK);
    eq("matched reply after async under a fresh head",
       nvGspRpcReceiveTrackerObserve(&events,reply.prefix,56u),
       NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    eq("retire matched reply after async",nvGspRpcReceiveTrackerConfirmConsumed(
        &events,2u,true),NV_GSP_RX_TRACK_MATCHED_SUCCESS);

    NVGspRpcReceiveTracker full={};
    eq("init full ring",nvGspRpcReceiveTrackerInit(&full),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan fullPlan=transport(0xb44u,0x65u);
    eq("begin full ring",nvGspRpcReceiveTrackerBegin(&full,&fullPlan),
       NV_GSP_RX_TRACK_OK);
    eq("publish full ring",nvGspRpcReceiveTrackerRecordPublication(
        &full,0xb44u,0x65u,true),NV_GSP_RX_TRACK_OK);
    eq("writer exactly one ring ahead is legal",
       nvGspRpcReceiveTrackerRecordAvailability(&full,
           static_cast<NVGspRxTrackU32>(full.expectedReadPtr+full.rxMessageCount),true),
       NV_GSP_RX_TRACK_OK);

    NVGspRpcReceiveTracker beyond={};
    eq("init beyond ring",nvGspRpcReceiveTrackerInit(&beyond),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan beyondPlan=transport(0xb55u,0x66u);
    eq("begin beyond ring",nvGspRpcReceiveTrackerBegin(&beyond,&beyondPlan),
       NV_GSP_RX_TRACK_OK);
    eq("publish beyond ring",nvGspRpcReceiveTrackerRecordPublication(
        &beyond,0xb55u,0x66u,true),NV_GSP_RX_TRACK_OK);
    eq("writer beyond ring capacity quarantines",
       nvGspRpcReceiveTrackerRecordAvailability(&beyond,
           static_cast<NVGspRxTrackU32>(beyond.expectedReadPtr+beyond.rxMessageCount+1u),true),
       NV_GSP_RX_TRACK_HEAD_INVALID);
    check("impossible writer is terminal and arms nothing",
          beyond.quarantined && !beyond.recordAvailable);
    eq("quarantine refuses later heads",
       nvGspRpcReceiveTrackerRecordAvailability(&beyond,1u,true),
       NV_GSP_RX_TRACK_QUARANTINED);
    eq("quarantine refuses the read",nvGspRpcReceiveTrackerObserve(
        &beyond,ack.prefix,56u),NV_GSP_RX_TRACK_QUARANTINED);

    NVGspRpcReceiveTracker behind={};
    eq("init behind reader",nvGspRpcReceiveTrackerInit(&behind),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan behindPlan=transport(0xb66u,0x67u);
    eq("begin behind reader",nvGspRpcReceiveTrackerBegin(&behind,&behindPlan),
       NV_GSP_RX_TRACK_OK);
    eq("publish behind reader",nvGspRpcReceiveTrackerRecordPublication(
        &behind,0xb66u,0x67u,true),NV_GSP_RX_TRACK_OK);
    eq("writer behind the reader quarantines",
       nvGspRpcReceiveTrackerRecordAvailability(&behind,0xffffffffu,true),
       NV_GSP_RX_TRACK_HEAD_INVALID);
    check("regressed writer is terminal",
          behind.quarantined && !behind.recordAvailable);

    NVGspRpcTransportPlan rollover={};
    eq("head wrap plan",nvGspPlanRpcTransport(
        &layout,0xb77u,0x68u,0u,0u,0u,0u,
        0xffffffffu,0xffffffffu,0u,56u,&rollover),NV_GSP_RPC_TXN_OK);
    NVGspRpcReceiveTracker wrapped={};
    eq("init head wrap",nvGspRpcReceiveTrackerInit(&wrapped),NV_GSP_RX_TRACK_OK);
    eq("begin head wrap",nvGspRpcReceiveTrackerBegin(&wrapped,&rollover),
       NV_GSP_RX_TRACK_OK);
    eq("publish head wrap",nvGspRpcReceiveTrackerRecordPublication(
        &wrapped,0xb77u,0x68u,true),NV_GSP_RX_TRACK_OK);
    eq("head at the reader across wrap is empty",
       nvGspRpcReceiveTrackerRecordAvailability(&wrapped,0xffffffffu,true),
       NV_GSP_RX_TRACK_HEAD_EMPTY);
    eq("head wrapped to zero covers the record",
       nvGspRpcReceiveTrackerRecordAvailability(&wrapped,0u,true),
       NV_GSP_RX_TRACK_OK);
    const NVGspRpcFramePlan wrapAck=response(0xb77u,0x68u);
    eq("observe under wrapped head",nvGspRpcReceiveTrackerObserve(
        &wrapped,wrapAck.prefix,56u),NV_GSP_RX_TRACK_MATCHED_SUCCESS);
    eq("retire under wrapped head",nvGspRpcReceiveTrackerConfirmConsumed(
        &wrapped,0u,true),NV_GSP_RX_TRACK_MATCHED_SUCCESS);

    NVGspRpcReceiveTracker stopping={};
    eq("init head stop",nvGspRpcReceiveTrackerInit(&stopping),NV_GSP_RX_TRACK_OK);
    const NVGspRpcTransportPlan stopPlan=transport(0xb88u,0x69u);
    eq("begin head stop",nvGspRpcReceiveTrackerBegin(&stopping,&stopPlan),
       NV_GSP_RX_TRACK_OK);
    eq("publish head stop",nvGspRpcReceiveTrackerRecordPublication(
        &stopping,0xb88u,0x69u,true),NV_GSP_RX_TRACK_OK);
    eq("head armed before stop",
       nvGspRpcReceiveTrackerRecordAvailability(&stopping,1u,true),
       NV_GSP_RX_TRACK_OK);
    eq("stop with an armed read quarantines",nvGspRpcReceiveTrackerStop(&stopping),
       NV_GSP_RX_TRACK_QUARANTINED);
    eq("stopped tracker refuses heads",
       nvGspRpcReceiveTrackerRecordAvailability(&stopping,1u,true),
       NV_GSP_RX_TRACK_STOPPED);
    const NVGspRpcFramePlan stopAck=response(0xb88u,0x69u);
    eq("stopped tracker refuses the armed read",nvGspRpcReceiveTrackerObserve(
        &stopping,stopAck.prefix,56u),NV_GSP_RX_TRACK_STOPPED);
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
    testConsumeTailPublicationWitness();
    testRxHeadAvailabilityWitness();
    std::printf("%u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
