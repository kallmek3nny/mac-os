// SPDX-License-Identifier: MIT
// Pure classifier for one read-only PGSP RM queue-register snapshot.
#ifndef NV_GSP_QUEUE_OBSERVE_HPP
#define NV_GSP_QUEUE_OBSERVE_HPP

#include "NVGspQueueRegs.hpp"

using NVGspObsU32 = unsigned int;

#define NV_GSP_QUEUE_OBSERVE_NOT_ATTEMPTED       0u
#define NV_GSP_QUEUE_OBSERVE_OK                  1u
#define NV_GSP_QUEUE_OBSERVE_NULL_OUTPUT         2u
#define NV_GSP_QUEUE_OBSERVE_BAD_INDEX           3u
#define NV_GSP_QUEUE_OBSERVE_DEAD_READ           4u
#define NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS  5u
#define NV_GSP_QUEUE_OBSERVE_CHANGED_DURING_READ 6u
#define NV_GSP_QUEUE_OBSERVE_NULL_INPUT          7u
#define NV_GSP_QUEUE_OBSERVE_LIMIT               8u

struct NVGspQueueCounterSample {
    NVGspObsU32 commandHead;
    NVGspObsU32 commandTail;
    NVGspObsU32 messageHead;
    NVGspObsU32 messageTail;
};

struct NVGspQueueSnapshot {
    NVGspObsU32 queueIndex;
    NVGspObsU32 commandHead;
    NVGspObsU32 commandTail;
    NVGspObsU32 messageHead;
    NVGspObsU32 messageTail;
    NVGspObsU32 commandUsed;
    NVGspObsU32 statusAvailable;
    NVGspObsU32 messageCount;
    bool allZero;
    bool allOnes;
    bool anyTrafficEvidence;
    bool countersPlausibleWithoutHistory;
    // Set only after two identical successive sets of live register reads.
    // This is NOT an atomic hardware snapshot or evidence GSP is initialized.
    bool sampledTwiceUnchanged;
};

static inline bool nvGspQueueObserveSucceeded(NVGspObsU32 verdict)
{
    return verdict == NV_GSP_QUEUE_OBSERVE_OK;
}

static inline const char *nvGspQueueObserveVerdictName(NVGspObsU32 verdict)
{
    switch (verdict) {
        case NV_GSP_QUEUE_OBSERVE_NOT_ATTEMPTED:      return "not-attempted";
        case NV_GSP_QUEUE_OBSERVE_OK:                 return "ok";
        case NV_GSP_QUEUE_OBSERVE_NULL_OUTPUT:        return "null-output";
        case NV_GSP_QUEUE_OBSERVE_BAD_INDEX:          return "bad-index";
        case NV_GSP_QUEUE_OBSERVE_DEAD_READ:          return "dead-read";
        case NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS: return "ambiguous-counters";
        case NV_GSP_QUEUE_OBSERVE_CHANGED_DURING_READ: return "changed-during-read";
        case NV_GSP_QUEUE_OBSERVE_NULL_INPUT:         return "null-input";
        default:                                      return "unknown";
    }
}

// Classify a single hardware snapshot without pretending to have the cached
// history NVIDIA's msgq recovery logic normally carries. A delta larger than
// the 63-entry production queue is therefore AMBIGUOUS, not proof of corrupt
// hardware: open RM documents transient head/tail reset observations where the
// cached counter is required to decide which value is real.
//
// One register reading 0xffffffff is NOT by itself a dead BAR signal. These are
// monotonic NvU32 counters, so 0xffffffff is a legal value immediately before
// rollover. Only the all-four-ones pattern is treated as a dead aperture by this
// one-shot observer; mixed snapshots continue through source-accurate wrap math.
static inline NVGspObsU32 nvGspClassifyQueueSnapshot(NVGspObsU32 queueIndex,
                                                     NVGspObsU32 commandHead,
                                                     NVGspObsU32 commandTail,
                                                     NVGspObsU32 messageHead,
                                                     NVGspObsU32 messageTail,
                                                     NVGspQueueSnapshot *out)
{
    if (!out) return NV_GSP_QUEUE_OBSERVE_NULL_OUTPUT;
    if (queueIndex >= NV_GSP_QUEUE_REGISTER_COUNT)
        return NV_GSP_QUEUE_OBSERVE_BAD_INDEX;

    const bool allOnes = commandHead == 0xffffffffu && commandTail == 0xffffffffu &&
                         messageHead == 0xffffffffu && messageTail == 0xffffffffu;
    if (allOnes)
        return NV_GSP_QUEUE_OBSERVE_DEAD_READ;

    NVGspMsgqTxHeaderPlan header = {};
    if (nvGspBuildMsgqTxHeader(NV_GSP_QUEUE_COMMAND_SILICON_BYTES, &header) !=
        NV_GSP_QUEUE_TX_OK)
        return NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS;

    const NVGspObsU32 commandUsed = (NVGspObsU32)(commandHead - commandTail);
    const NVGspObsU32 statusAvailable = (NVGspObsU32)(messageHead - messageTail);

    NVGspQueueSnapshot staged = {};
    staged.queueIndex = queueIndex;
    staged.commandHead = commandHead;
    staged.commandTail = commandTail;
    staged.messageHead = messageHead;
    staged.messageTail = messageTail;
    staged.commandUsed = commandUsed;
    staged.statusAvailable = statusAvailable;
    staged.messageCount = header.messageCount;
    staged.allZero = commandHead == 0u && commandTail == 0u &&
                     messageHead == 0u && messageTail == 0u;
    staged.allOnes = allOnes;
    staged.anyTrafficEvidence = commandHead != 0u || commandTail != 0u ||
                                messageHead != 0u || messageTail != 0u;
    staged.countersPlausibleWithoutHistory =
        commandUsed <= header.messageCount && statusAvailable <= header.messageCount;
    *out = staged;

    return staged.countersPlausibleWithoutHistory
        ? NV_GSP_QUEUE_OBSERVE_OK
        : NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS;
}

// A live queue can change any monotonic counter between individual BAR0
// reads. A single plausible set of four values does not establish that those
// values ever coexisted. Accept only a bounded pair of identical consecutive
// register samples; no retry loop, sleep, queue mutation, or write authority.
// Equality is a limited stability observation, not a synchronization contract.
static inline NVGspObsU32 nvGspClassifyStableQueueSnapshot(
    NVGspObsU32 queueIndex,
    const NVGspQueueCounterSample *first,
    const NVGspQueueCounterSample *second,
    NVGspQueueSnapshot *out)
{
    if (!out) return NV_GSP_QUEUE_OBSERVE_NULL_OUTPUT;
    if (!first || !second) return NV_GSP_QUEUE_OBSERVE_NULL_INPUT;
    if (queueIndex >= NV_GSP_QUEUE_REGISTER_COUNT)
        return NV_GSP_QUEUE_OBSERVE_BAD_INDEX;

    if (first->commandHead != second->commandHead ||
        first->commandTail != second->commandTail ||
        first->messageHead != second->messageHead ||
        first->messageTail != second->messageTail)
        return NV_GSP_QUEUE_OBSERVE_CHANGED_DURING_READ;

    NVGspQueueSnapshot staged = {};
    const NVGspObsU32 verdict = nvGspClassifyQueueSnapshot(
        queueIndex, second->commandHead, second->commandTail,
        second->messageHead, second->messageTail, &staged);
    if (verdict == NV_GSP_QUEUE_OBSERVE_OK ||
        verdict == NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS) {
        staged.sampledTwiceUnchanged = true;
        *out = staged;
    }
    return verdict;
}

// Publicly exposed counter evidence requires both a valid classification
// AND a stable paired observation. This does not prove an atomic MMIO read,
// GSP initialization, command completion, or RM channel ownership.
static inline bool nvGspQueueSnapshotCanPublish(NVGspObsU32 verdict,
                                                const NVGspQueueSnapshot *snapshot)
{
    return snapshot && snapshot->sampledTwiceUnchanged &&
           (verdict == NV_GSP_QUEUE_OBSERVE_OK ||
            verdict == NV_GSP_QUEUE_OBSERVE_AMBIGUOUS_COUNTERS);
}

#endif // NV_GSP_QUEUE_OBSERVE_HPP
