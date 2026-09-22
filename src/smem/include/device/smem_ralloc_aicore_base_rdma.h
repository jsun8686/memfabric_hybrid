/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* Device-side (AICore) inline RDMA data plane for ralloc pools.
 *
 * Layer contract (bm/shm/trans/ralloc are peer layers, no cross include):
 *   - the transport layer publishes, per entity, a 128B meta record into the fixed
 *     GVA window at the tail of the device VA space, and an AiQpRMAQueueInfo shaped
 *     QP/MR table in device memory referenced by meta.qpInfoAddress;
 *   - this header vendors the read side of that contract with its own constants and
 *     struct definitions. Layout below must stay in sync with:
 *       src/hybm/csrc/common/hybm_define.h          (meta window constants / HybmDeviceMeta)
 *       src/hybm/csrc/under_api/dl_hccp_def.h       (AiQpRMAQueueInfo / AiQpRMAWQ / AiQpRMACQ /
 *                                                    RdmaMemRegionInfo layouts, filled by
 *                                                    FixedRanksQpManager::FillQpInfo)
 *     the values are identical to the smem_shm device headers by design: shm objects and
 *     hybm entities share the same 511-slot meta window, ralloc entities are hybm entities.
 *
 * Usage: an AscendC kernel includes this header and calls the roce_* helpers with the
 * pool entity id; context (rank, QP ring, MR table) is self-discovered from the meta
 * window, no parameters other than the entity id are needed.
 */
#ifndef __MEMFABRIC_SMEM_RALLOC_AI_CORE_BASE_RDMA_H__
#define __MEMFABRIC_SMEM_RALLOC_AI_CORE_BASE_RDMA_H__

#include "kernel_operator.h"

#define SMEM_RALLOC_INLINE_AICORE __attribute__((always_inline)) inline __aicore__

/* ---- meta window constants, in sync with hybm_define.h ---- */
constexpr uint64_t SMEM_RALLOC_DEVICE_VA_START = 0x100000000000UL; /* 16T */
constexpr uint64_t SMEM_RALLOC_DEVICE_VA_SIZE = 0x80000000000UL;   /* 8T */
constexpr uint64_t SMEM_RALLOC_DEVICE_END_ADDR =
    SMEM_RALLOC_DEVICE_VA_START + SMEM_RALLOC_DEVICE_VA_SIZE - (1UL << 30UL); /* 1G guard below shm/hybm window */
constexpr uint64_t SMEM_RALLOC_ENTITY_PRE_META_SIZE = 128UL;                  /* 128B per entity */
constexpr uint64_t SMEM_RALLOC_ENTITY_NUM_MAX = 511UL;
constexpr uint64_t SMEM_RALLOC_DEVICE_GLOBAL_META_SIZE = SMEM_RALLOC_ENTITY_PRE_META_SIZE; /* 128B */
constexpr uint64_t SMEM_RALLOC_DEVICE_META_SIZE =
    SMEM_RALLOC_ENTITY_PRE_META_SIZE * SMEM_RALLOC_ENTITY_NUM_MAX + SMEM_RALLOC_DEVICE_GLOBAL_META_SIZE; /* 64K */
constexpr uint64_t SMEM_RALLOC_DEVICE_USER_CONTEXT_PRE_SIZE = 64UL * 1024UL;                             /* 64K */
constexpr uint64_t SMEM_RALLOC_DEVICE_INFO_SIZE =
    SMEM_RALLOC_DEVICE_USER_CONTEXT_PRE_SIZE * SMEM_RALLOC_ENTITY_NUM_MAX + SMEM_RALLOC_DEVICE_META_SIZE; /* 32M */
constexpr uint64_t SMEM_RALLOC_DEVICE_META_ADDR = SMEM_RALLOC_DEVICE_END_ADDR - SMEM_RALLOC_DEVICE_INFO_SIZE;
constexpr uint64_t SMEM_RALLOC_DEVICE_USER_CONTEXT_ADDR = SMEM_RALLOC_DEVICE_META_ADDR + SMEM_RALLOC_DEVICE_META_SIZE;

/* ---- meta record field offsets, in sync with hybm_define.h HybmDeviceMeta ---- */
constexpr uint64_t SMEM_RALLOC_META_ENTITY_ID_OFFSET = 0;
constexpr uint64_t SMEM_RALLOC_META_RANK_OFFSET = SMEM_RALLOC_META_ENTITY_ID_OFFSET + sizeof(uint32_t);
constexpr uint64_t SMEM_RALLOC_META_RANK_SIZE_OFFSET = SMEM_RALLOC_META_RANK_OFFSET + sizeof(uint32_t);
constexpr uint64_t SMEM_RALLOC_META_CONTEXT_OFFSET = SMEM_RALLOC_META_RANK_SIZE_OFFSET + sizeof(uint32_t);
constexpr uint64_t SMEM_RALLOC_META_SYMM_OFFSET = SMEM_RALLOC_META_CONTEXT_OFFSET + sizeof(uint32_t);
constexpr uint64_t SMEM_RALLOC_META_QP_INFO_OFFSET = SMEM_RALLOC_META_SYMM_OFFSET + sizeof(uint64_t);

constexpr uint64_t SMEM_RALLOC_DATA_CACHE_LINE_SIZE = 64;
constexpr uint32_t SMEM_RALLOC_NUM_CQE_PER_POLL_CQ = 100;
constexpr uint32_t SMEM_RALLOC_CQE_OWNER_SHIFT = 7;

/* ---- QP ring / MR table layouts, in sync with dl_hccp_def.h (AiQpRMAWQ etc.) ---- */
enum class SmemRallocDBMode : int32_t { INVALID_DB = -1, HW_DB = 0, SW_DB };

struct SmemRallocWQCtx {
    uint32_t wqn;      /* work queue number */
    uint64_t bufAddr;  /* start address of ring buffer */
    uint32_t wqeSize;  /* size of each WQE */
    uint32_t depth;    /* depth of ring buffer, power of 2 */
    uint64_t headAddr; /* work queue head (producer index) address */
    uint64_t tailAddr; /* work queue tail (consumer index) address */
    SmemRallocDBMode dbMode;
    uint64_t dbAddr; /* doorbell address */
    uint32_t sl;     /* service level */
};

struct SmemRallocCQCtx {
    uint32_t cqn;      /* completion queue number */
    uint64_t bufAddr;  /* start address of ring buffer */
    uint32_t cqeSize;  /* size of each CQE */
    uint32_t depth;    /* depth of ring buffer, power of 2 */
    uint64_t headAddr; /* completion queue head (producer index) address */
    uint64_t tailAddr; /* completion queue tail (consumer index) address */
    SmemRallocDBMode dbMode;
    uint64_t dbAddr; /* doorbell address */
};

struct SmemRallocWqeCtx { /* 32B WQE control + remote addressing segment */
    uint32_t byte4;       /* [0:4] opcode, [7] owner bit, [8] signaled */
    uint32_t msgLen;
    uint32_t immtdata;
    uint32_t byte16; /* [120:127] num_sge */
    uint32_t byte20; /* [128:151] start_sge_index */
    uint32_t rkey;
    uint64_t va; /* remote virtual address */
};

struct SmemRallocSegCtx { /* SGE following the WQE when num_sge == 1 */
    uint32_t len;
    uint32_t lkey;
    uint64_t addr; /* local virtual address */
};

struct SmemRallocCqeCtx {
    uint32_t byte4;   /* [7] owner bit, [8:15] status */
    uint32_t immtdata;
    uint32_t byte12;
    uint32_t byte16; /* [0:23] wqn */
    uint32_t byteCnt;
    uint32_t smac;
    uint32_t byte28;
    uint32_t byte32;
};

struct SmemRallocMemInfo { /* in sync with RdmaMemRegionInfo */
    uint64_t size;
    uint64_t addr;         /* GVA base of the pool block MR */
    uint32_t lkey;
    uint32_t rkey;
    uint64_t regAddress;   /* device-dma base the MR was registered under; for host-dram pools
                            * this differs from addr (HalHostRegister iova), for hbm it equals addr */
};

/* ---- user MR table (P1), in sync with ralloc publish side (smem_ralloc_entry.cpp PublishUserMrTable) ----
 * published by the host into this entity's 64K user context region via hybm_set_extra_context;
 * lets device-scheduled RDMA use locally registered user HBM as the local endpoint */
constexpr uint32_t SMEM_RALLOC_USER_MR_TABLE_MAGIC = 0x31524D53; /* "SMR1" */
constexpr uint32_t SMEM_RALLOC_USER_MR_TABLE_VERSION = 1;
constexpr uint64_t SMEM_RALLOC_USER_MR_TABLE_HEADER_SIZE = 64;
constexpr uint64_t SMEM_RALLOC_USER_MR_TABLE_ERRCODE_OFFSET = 12; /* device writes: 0 = ok, 1 = lkey lookup miss */
constexpr uint64_t SMEM_RALLOC_USER_MR_TABLE_ENTRY_SIZE = 32;
constexpr uint64_t SMEM_RALLOC_USER_MR_TABLE_CAPACITY =
    (SMEM_RALLOC_DEVICE_USER_CONTEXT_PRE_SIZE - SMEM_RALLOC_USER_MR_TABLE_HEADER_SIZE) /
    SMEM_RALLOC_USER_MR_TABLE_ENTRY_SIZE; /* 2046 slots, host publishes at most 2040 */

struct SmemRallocUserMrEntry { /* 32B, in sync with the ralloc publish side */
    uint64_t addr; /* device-dma-visible address returned by hybm_query_memory_key */
    uint64_t size;
    uint32_t lkey;
    uint32_t rkey;
    uint64_t reserved;
};

struct SmemRallocUserMrTable { /* 64B header + entries, serialized into the 64K user context region */
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t errCode;
    uint64_t reserved[6];
    SmemRallocUserMrEntry entries[SMEM_RALLOC_USER_MR_TABLE_CAPACITY];
};

/* in sync with AiQpRMAQueueInfo as published by FixedRanksQpManager::FillQpInfo */
struct SmemRallocRdmaInfo {
    uint32_t qpNum;  /* QP count per connection, always 1 today */
    uint64_t sqPtr;  /* send queue array of [rankCount][qpNum] WQCtx */
    uint64_t rqPtr;  /* receive queue array of [rankCount][qpNum] WQCtx */
    uint64_t scqPtr; /* send completion queue array of [rankCount][qpNum] CQCtx */
    uint64_t rcqPtr; /* receive completion queue array of [rankCount][qpNum] CQCtx */
    uint64_t memPtr; /* memory region array of [rankCount] MemInfo, pool block MR per rank */
};

/* ---- meta accessors ---- */

SMEM_RALLOC_INLINE_AICORE uint32_t smem_ralloc_get_global_rank(uint32_t entityId)
{
    if (entityId >= SMEM_RALLOC_ENTITY_NUM_MAX) {
        return 0xFFFFFFFFU;
    }
    uint64_t metaAddr = SMEM_RALLOC_DEVICE_META_ADDR + SMEM_RALLOC_DEVICE_GLOBAL_META_SIZE +
                        entityId * SMEM_RALLOC_ENTITY_PRE_META_SIZE;
    return (*(__gm__ uint32_t *)(metaAddr + SMEM_RALLOC_META_RANK_OFFSET));
}

SMEM_RALLOC_INLINE_AICORE uint32_t smem_ralloc_get_global_rank_size(uint32_t entityId)
{
    if (entityId >= SMEM_RALLOC_ENTITY_NUM_MAX) {
        return 0xFFFFFFFFU;
    }
    uint64_t metaAddr = SMEM_RALLOC_DEVICE_META_ADDR + SMEM_RALLOC_DEVICE_GLOBAL_META_SIZE +
                        entityId * SMEM_RALLOC_ENTITY_PRE_META_SIZE;
    return (*(__gm__ uint32_t *)(metaAddr + SMEM_RALLOC_META_RANK_SIZE_OFFSET));
}

SMEM_RALLOC_INLINE_AICORE __gm__ void *smem_ralloc_get_qp_info_address(uint32_t entityId)
{
    if (entityId >= SMEM_RALLOC_ENTITY_NUM_MAX) {
        return nullptr;
    }
    uint64_t metaAddr = SMEM_RALLOC_DEVICE_META_ADDR + SMEM_RALLOC_DEVICE_GLOBAL_META_SIZE +
                        entityId * SMEM_RALLOC_ENTITY_PRE_META_SIZE;
    return *(__gm__ void **)(metaAddr + SMEM_RALLOC_META_QP_INFO_OFFSET);
}

SMEM_RALLOC_INLINE_AICORE uint64_t smem_ralloc_user_context_address(uint32_t entityId)
{
    return SMEM_RALLOC_DEVICE_USER_CONTEXT_ADDR + entityId * SMEM_RALLOC_DEVICE_USER_CONTEXT_PRE_SIZE;
}

SMEM_RALLOC_INLINE_AICORE void smem_ralloc_cache_write_through(__gm__ uint8_t *sourceAddr, uint64_t length)
{
    __gm__ uint8_t *start = (__gm__ uint8_t *)((uint64_t)sourceAddr / SMEM_RALLOC_DATA_CACHE_LINE_SIZE *
                                               SMEM_RALLOC_DATA_CACHE_LINE_SIZE);
    __gm__ uint8_t *end = (__gm__ uint8_t *)(((uint64_t)sourceAddr + length) / SMEM_RALLOC_DATA_CACHE_LINE_SIZE *
                                             SMEM_RALLOC_DATA_CACHE_LINE_SIZE);
    AscendC::GlobalTensor<uint8_t> global;
    global.SetGlobalBuffer(start);
    for (uint64_t i = 0; i <= end - start; i += SMEM_RALLOC_DATA_CACHE_LINE_SIZE) {
        AscendC::DataCacheCleanAndInvalid<uint8_t, AscendC::CacheLine::SINGLE_CACHE_LINE,
                                          AscendC::DcciDst::CACHELINE_OUT>(global[i]);
    }
}

/**
 * @brief Look up the lkey of a locally registered user memory region (P1).
 *        Two-level scheme: the caller first tries the pool MR of this rank; on miss this helper
 *        scans the user MR table published (host side) into this entity's user context region.
 * @return the lkey covering localAddr, or 0 when the table is absent/invalid or no entry matches.
 */
SMEM_RALLOC_INLINE_AICORE uint32_t smem_ralloc_lookup_local_mr(uint32_t entityId, uint64_t localAddr)
{
    if (entityId >= SMEM_RALLOC_ENTITY_NUM_MAX) {
        return 0;
    }
    __gm__ SmemRallocUserMrTable *table = (__gm__ SmemRallocUserMrTable *)smem_ralloc_user_context_address(entityId);
    if (table->magic != SMEM_RALLOC_USER_MR_TABLE_MAGIC || table->version != SMEM_RALLOC_USER_MR_TABLE_VERSION) {
        return 0;
    }
    uint32_t count = table->count;
    if (count > SMEM_RALLOC_USER_MR_TABLE_CAPACITY) {
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        __gm__ SmemRallocUserMrEntry *entry = table->entries + i;
        if (localAddr >= entry->addr && localAddr < entry->addr + entry->size) {
            return entry->lkey;
        }
    }
    return 0;
}

/**
 * @brief Record an lkey lookup miss into the user MR table header (errCode = 1) so the host can
 *        read it back for diagnostics; the WQE is then not posted.
 */
SMEM_RALLOC_INLINE_AICORE void smem_ralloc_report_user_mr_lookup_miss(uint32_t entityId)
{
    if (entityId >= SMEM_RALLOC_ENTITY_NUM_MAX) {
        return;
    }
    __gm__ uint32_t *errCode = (__gm__ uint32_t *)(smem_ralloc_user_context_address(entityId) +
                                                   SMEM_RALLOC_USER_MR_TABLE_ERRCODE_OFFSET);
    *errCode = 1;
    smem_ralloc_cache_write_through((__gm__ uint8_t *)errCode, sizeof(uint32_t));
    AscendC::PipeBarrier<PIPE_ALL>();
}

/* ---- RDMA data plane ---- */

enum class SmemRallocOpcode : uint32_t {
    OP_SEND = 0,
    OP_SEND_WITH_INV,
    OP_SEND_WITH_IMM,
    OP_RDMA_WRITE,
    OP_RDMA_WRITE_WITH_IMM,
    OP_RDMA_READ
};

/**
 * @brief Poll the send completion queue of one connection until consumer index reaches idx.
 *        Recycles WQE space, checks CQE status, updates CQ/WQ tail and rings the CQ doorbell.
 *
 * @param entityId               [in] ralloc pool entity id
 * @param remoteRankId           [in] destination rank id
 * @param qpIdx                  [in] QP index in multi-QP scenario, 0 today
 * @param idx                    [in] expected completion queue consumer index after polling
 * @param ubLocal64              [in] UB workspace of uint64_t (1 element)
 * @param ubLocal32              [in] UB workspace of uint32_t (1 element)
 * @return 0 on success, non-zero CQE status on error
 */
SMEM_RALLOC_INLINE_AICORE uint32_t smem_ralloc_roce_poll_cq(uint32_t entityId, uint32_t remoteRankId, uint32_t qpIdx,
                                                            uint32_t idx, AscendC::LocalTensor<uint64_t> ubLocal64,
                                                            AscendC::LocalTensor<uint32_t> ubLocal32)
{
    __gm__ void *qpInfoVa = smem_ralloc_get_qp_info_address(entityId);
    __gm__ SmemRallocRdmaInfo *rdmaInfo = (__gm__ SmemRallocRdmaInfo *)qpInfoVa;
    uint32_t qpNum = rdmaInfo->qpNum;
    __gm__ SmemRallocCQCtx *cqCtxEntry =
        (__gm__ SmemRallocCQCtx *)(rdmaInfo->scqPtr + (remoteRankId * qpNum + qpIdx) * sizeof(SmemRallocCQCtx));
    auto cqBaseAddr = cqCtxEntry->bufAddr;
    auto cqeSize = cqCtxEntry->cqeSize;
    auto depth = cqCtxEntry->depth;
    auto curHardwareTailAddr = cqCtxEntry->tailAddr;
    smem_ralloc_cache_write_through((__gm__ uint8_t *)curHardwareTailAddr, 8);
    uint32_t curTail = *(__gm__ uint32_t *)(curHardwareTailAddr);

    AscendC::DataCopyExtParams copyParamsTail{1, 1 * sizeof(uint32_t), 0, 0, 0};
    while (curTail != idx) {
        __gm__ SmemRallocCqeCtx *cqeAddr =
            (__gm__ SmemRallocCqeCtx *)(cqBaseAddr + cqeSize * (curTail & (depth - 1)));
        uint32_t cqeByte4 = *(__gm__ uint32_t *)cqeAddr;
        while (((cqeByte4 & (1 << SMEM_RALLOC_CQE_OWNER_SHIFT)) != 0) == ((curTail & depth) != 0)) {
            int64_t tmp = AscendC::GetSystemCycle();
            (void)tmp;
            smem_ralloc_cache_write_through((__gm__ uint8_t *)cqeAddr, 32);
            cqeByte4 = *(__gm__ uint32_t *)cqeAddr;
        }
        curTail++;
        uint32_t wqn = cqeAddr->byte16 & 0xFFFFFF;
        (void)wqn;

        /* check CQE status */
        uint32_t status = (cqeAddr->byte4 >> 8) & 0xFF;
        if (status != 0) {
            return status;
        }
    }

    /* update CQ tail */
    ubLocal32.SetValue(0, (uint32_t)curTail);
    AscendC::GlobalTensor<uint32_t> tailGlobalTensor;
    tailGlobalTensor.SetGlobalBuffer((__gm__ uint32_t *)curHardwareTailAddr);
    AscendC::PipeBarrier<PIPE_ALL>();
    AscendC::DataCopyPad(tailGlobalTensor, ubLocal32, copyParamsTail);
    AscendC::PipeBarrier<PIPE_ALL>();
    smem_ralloc_cache_write_through((__gm__ uint8_t *)curHardwareTailAddr, 8);

    /* ring CQ doorbell */
    auto cqDBAddr = cqCtxEntry->dbAddr;
    if (cqCtxEntry->dbMode == SmemRallocDBMode::SW_DB) {
        ubLocal32.SetValue(0, (uint32_t)(curTail & 0xFFFFFF));
        AscendC::GlobalTensor<uint32_t> cqDbGlobalTensor;
        cqDbGlobalTensor.SetGlobalBuffer((__gm__ uint32_t *)cqDBAddr);
        AscendC::PipeBarrier<PIPE_ALL>();
        AscendC::DataCopyPad(cqDbGlobalTensor, ubLocal32, copyParamsTail);
        AscendC::PipeBarrier<PIPE_ALL>();
        smem_ralloc_cache_write_through((__gm__ uint8_t *)cqDBAddr, 8);
    } else if (cqCtxEntry->dbMode == SmemRallocDBMode::HW_DB) {
        uint64_t doorBellInfo = 0;
        doorBellInfo |= cqCtxEntry->cqn;                      /* [0:23] DB_TAG = cqn */
        doorBellInfo |= 3 << 24;                              /* [24:27] DB_CMD = HNS_ROCE_V2_CQ_DB_PTR */
        doorBellInfo |= (uint64_t)(curTail & 0xFFFFFF) << 32; /* [32:55] DB_CQ_CI = cq tail */
        doorBellInfo |= (uint64_t)1 << 56;                    /* [56:56] DB_CQ_CMD_SN = 1 */
        ubLocal64.SetValue(0, doorBellInfo);
        AscendC::GlobalTensor<uint64_t> dbGlobalTensor;
        dbGlobalTensor.SetGlobalBuffer((__gm__ uint64_t *)cqDBAddr);
        AscendC::DataCopyExtParams copyParams{1, 1 * sizeof(uint64_t), 0, 0, 0};
        AscendC::PipeBarrier<PIPE_ALL>();
        AscendC::DataCopyPad(dbGlobalTensor, ubLocal64, copyParams);
        AscendC::PipeBarrier<PIPE_ALL>();
    }

    /* update WQ tail (recycle WQE space) */
    __gm__ SmemRallocWQCtx *wqCtxEntry =
        (__gm__ SmemRallocWQCtx *)(rdmaInfo->sqPtr + (remoteRankId * qpNum + qpIdx) * sizeof(SmemRallocWQCtx));
    auto curWqTailAddr = wqCtxEntry->tailAddr;
    smem_ralloc_cache_write_through((__gm__ uint8_t *)curWqTailAddr, 8);
    uint32_t curWqTail = *(__gm__ uint32_t *)(curWqTailAddr);
    (void)curWqTail;
    ubLocal32.SetValue(0, curTail);
    AscendC::GlobalTensor<uint32_t> wqTailGlobalTensor;
    wqTailGlobalTensor.SetGlobalBuffer((__gm__ uint32_t *)curWqTailAddr);
    AscendC::PipeBarrier<PIPE_ALL>();
    AscendC::DataCopyPad(wqTailGlobalTensor, ubLocal32, copyParamsTail);
    AscendC::PipeBarrier<PIPE_ALL>();
    smem_ralloc_cache_write_through((__gm__ uint8_t *)curWqTailAddr, 8);
    return 0;
}

/**
 * @brief Post one RDMA work request: build the WQE+SGE in the send queue, flush cache and
 *        ring the HW doorbell. Polls the CQ first when the send queue is nearly full.
 *
 * @param entityId               [in] ralloc pool entity id
 * @param remoteAddr             [in] address in the remote rank pool block (GVA)
 * @param localAddr              [in] address in the local rank pool block (GVA)
 * @param destRankId             [in] destination rank id
 * @param qpIdx                  [in] QP index in multi-QP scenario, 0 today
 * @param opcode                 [in] SmemRallocOpcode, RDMA_WRITE / RDMA_READ today
 * @param messageLen             [in] message length in bytes
 * @param ubLocal64              [in] UB workspace of uint64_t (1 element)
 * @param ubLocal32              [in] UB workspace of uint32_t (1 element)
 */
SMEM_RALLOC_INLINE_AICORE void smem_ralloc_rdma_post_send(uint32_t entityId, __gm__ uint8_t *remoteAddr,
                                                          __gm__ uint8_t *localAddr, uint32_t destRankId,
                                                          uint32_t qpIdx, SmemRallocOpcode opcode,
                                                          uint64_t messageLen, AscendC::LocalTensor<uint64_t> ubLocal64,
                                                          AscendC::LocalTensor<uint32_t> ubLocal32)
{
    __gm__ void *qpInfoVa = smem_ralloc_get_qp_info_address(entityId);
    __gm__ SmemRallocRdmaInfo *rdmaInfo = (__gm__ SmemRallocRdmaInfo *)qpInfoVa;
    uint32_t qpNum = rdmaInfo->qpNum;
    __gm__ SmemRallocWQCtx *qpCtxEntry =
        (__gm__ SmemRallocWQCtx *)(rdmaInfo->sqPtr + (destRankId * qpNum + qpIdx) * sizeof(SmemRallocWQCtx));
    auto memInfoTable = rdmaInfo->memPtr;
    auto sqBaseAddr = qpCtxEntry->bufAddr;
    auto wqeSize = qpCtxEntry->wqeSize;
    auto curHardwareHeadAddr = qpCtxEntry->headAddr;
    smem_ralloc_cache_write_through((__gm__ uint8_t *)curHardwareHeadAddr, 8);
    uint32_t curHead = *(__gm__ uint32_t *)(curHardwareHeadAddr);
    auto curHardwareTailAddr = qpCtxEntry->tailAddr;
    auto depth = qpCtxEntry->depth;
    auto shift = 13; /* owner bit period = depth (8192 today, keep in sync with trans layer) */
    AscendC::PipeBarrier<PIPE_ALL>();

    /* poll CQ if the send queue is nearly full */
    smem_ralloc_cache_write_through((__gm__ uint8_t *)curHardwareTailAddr, 8);
    if ((curHead + 10) % depth == (*(__gm__ uint32_t *)(curHardwareTailAddr)) % depth) {
        (void)smem_ralloc_roce_poll_cq(entityId, destRankId, qpIdx,
                                       *(__gm__ uint32_t *)(curHardwareTailAddr) + SMEM_RALLOC_NUM_CQE_PER_POLL_CQ,
                                       ubLocal64, ubLocal32);
    }

    /* write WQE to HBM */
    __gm__ uint8_t *wqeAddr = (__gm__ uint8_t *)(sqBaseAddr + wqeSize * (curHead % depth));
    uint64_t ownBit = (curHead >> shift) & 0x1;
    uint32_t byte4 = (uint32_t)opcode & 0x1F; /* [0:4] opcode */
    byte4 |= ((~ownBit) << 7) & (1 << 7);     /* [7] owner bit */
    byte4 |= 1 << 8;                          /* [8] IBV_SEND_SIGNALED */
    *(__gm__ uint32_t *)(wqeAddr) = byte4;
    *(__gm__ uint32_t *)(wqeAddr + 4) = messageLen;
    *(__gm__ uint32_t *)(wqeAddr + 8) = 0;         /* immediate data, 0 */
    *(__gm__ uint32_t *)(wqeAddr + 12) = 1 << 24;  /* [120:127] num_sge = 1 */
    *(__gm__ uint32_t *)(wqeAddr + 16) = 0;        /* [128:151] start_sge_index = 0 */
    __gm__ SmemRallocMemInfo *remoteMemInfo = (__gm__ SmemRallocMemInfo *)(memInfoTable + sizeof(SmemRallocMemInfo) *
                                                                                      destRankId);
    *(__gm__ uint32_t *)(wqeAddr + 20) = remoteMemInfo->rkey;  /* remote key */
    /* the rkey covers the device-dma range the remote MR was registered under: translate the
     * pool GVA into that range (identity for hbm pools where regAddress == addr) */
    uint64_t remoteRegAddr = (remoteMemInfo->regAddress != 0)
                                 ? remoteMemInfo->regAddress + ((uint64_t)remoteAddr - remoteMemInfo->addr)
                                 : (uint64_t)remoteAddr;
    *(__gm__ uint64_t *)(wqeAddr + 24) = remoteRegAddr; /* remote VA */

    /* write SGE to HBM */
    __gm__ uint8_t *sgeAddr = wqeAddr + sizeof(SmemRallocWqeCtx);
    *(__gm__ uint32_t *)(sgeAddr) = messageLen;
    __gm__ SmemRallocMemInfo *localMemInfo = (__gm__ SmemRallocMemInfo *)(
        memInfoTable + sizeof(SmemRallocMemInfo) * smem_ralloc_get_global_rank(entityId));
    uint32_t localLkey = localMemInfo->lkey;
    if ((uint64_t)localAddr < localMemInfo->addr ||
        (uint64_t)localAddr >= localMemInfo->addr + localMemInfo->size) {
        /* local endpoint outside this rank's pool MR: fall back to the registered user MR table */
        localLkey = smem_ralloc_lookup_local_mr(entityId, (uint64_t)localAddr);
        if (localLkey == 0) {
            smem_ralloc_report_user_mr_lookup_miss(entityId);
            return;
        }
    }
    *(__gm__ uint32_t *)(sgeAddr + 4) = localLkey; /* local key */
    /* same translation for the local sge address: the lkey covers the device-dma range this
     * block was registered under, not the GVA range */
    uint64_t localRegAddr = (localMemInfo->regAddress != 0)
                                ? localMemInfo->regAddress + ((uint64_t)localAddr - localMemInfo->addr)
                                : (uint64_t)localAddr;
    *(__gm__ uint64_t *)(sgeAddr + 8) = localRegAddr;

    /* WQE & SGE cache flush */
    smem_ralloc_cache_write_through(wqeAddr, sizeof(SmemRallocWqeCtx) + sizeof(SmemRallocSegCtx));
    AscendC::PipeBarrier<PIPE_ALL>();
    curHead++;

    /* ring SQ doorbell (HW mode today, set by the transport layer) */
    uint64_t doorBellInfo = 0;
    doorBellInfo |= qpCtxEntry->wqn;                   /* [0:23] DB_TAG = qp num */
    doorBellInfo |= 0 << 24;                           /* [24:27] DB_CMD = HNS_ROCE_V2_SQ_DB */
    doorBellInfo |= ((uint64_t)curHead % 65536) << 32; /* [32:47] DB_PI = sq head */
    doorBellInfo |= (uint64_t)(qpCtxEntry->sl) << 48;  /* [48:50] DB_SL */

    __gm__ uint64_t *doorBellAddr = (__gm__ uint64_t *)(qpCtxEntry->dbAddr);
    AscendC::PipeBarrier<PIPE_ALL>();

    ubLocal64.SetValue(0, doorBellInfo);
    AscendC::GlobalTensor<uint64_t> dbGlobalTensor;
    dbGlobalTensor.SetGlobalBuffer(doorBellAddr);
    AscendC::DataCopyExtParams copyParams{1, 1 * sizeof(uint64_t), 0, 0, 0};
    AscendC::PipeBarrier<PIPE_ALL>();
    AscendC::DataCopyPad(dbGlobalTensor, ubLocal64, copyParams);
    AscendC::PipeBarrier<PIPE_ALL>();

    /* update SQ head */
    ubLocal32.SetValue(0, (uint32_t)curHead);
    AscendC::GlobalTensor<uint32_t> headGlobalTensor;
    headGlobalTensor.SetGlobalBuffer((__gm__ uint32_t *)curHardwareHeadAddr);
    AscendC::DataCopyExtParams copyParamsHead{1, 1 * sizeof(uint32_t), 0, 0, 0};
    AscendC::PipeBarrier<PIPE_ALL>();
    AscendC::DataCopyPad(headGlobalTensor, ubLocal32, copyParamsHead);
    AscendC::PipeBarrier<PIPE_ALL>();
}

/**
 * @brief Asynchronous one-sided RDMA WRITE from the local pool block to a remote pool block.
 */
template <typename T>
SMEM_RALLOC_INLINE_AICORE void smem_ralloc_roce_write(uint32_t entityId, __gm__ T *srcDmaAddr,
                                                      __gm__ T *destDmaAddr, uint32_t destRankId, uint32_t qpIdx,
                                                      uint64_t messageLen, AscendC::LocalTensor<uint64_t> ubLocal64,
                                                      AscendC::LocalTensor<uint32_t> ubLocal32)
{
    smem_ralloc_rdma_post_send(entityId, (__gm__ uint8_t *)destDmaAddr, (__gm__ uint8_t *)srcDmaAddr, destRankId,
                               qpIdx, SmemRallocOpcode::OP_RDMA_WRITE, messageLen, ubLocal64, ubLocal32);
}

/**
 * @brief Asynchronous one-sided RDMA READ from a remote pool block into the local pool block.
 */
template <typename T>
SMEM_RALLOC_INLINE_AICORE void smem_ralloc_roce_read(uint32_t entityId, __gm__ T *srcDmaAddr,
                                                     __gm__ T *destDmaAddr, uint32_t srcRankId, uint32_t qpIdx,
                                                     uint64_t messageLen, AscendC::LocalTensor<uint64_t> ubLocal64,
                                                     AscendC::LocalTensor<uint32_t> ubLocal32)
{
    smem_ralloc_rdma_post_send(entityId, (__gm__ uint8_t *)srcDmaAddr, (__gm__ uint8_t *)destDmaAddr, srcRankId,
                               qpIdx, SmemRallocOpcode::OP_RDMA_READ, messageLen, ubLocal64, ubLocal32);
}

/**
 * @brief Wait until every WQE posted on the connection has completed (device-side quiet).
 * @return 0 on success, non-zero CQE status on error.
 */
SMEM_RALLOC_INLINE_AICORE uint32_t smem_ralloc_roce_quiet(uint32_t entityId, uint32_t remoteRankId, uint32_t qpIdx,
                                                          AscendC::LocalTensor<uint64_t> ubLocal64,
                                                          AscendC::LocalTensor<uint32_t> ubLocal32)
{
    __gm__ void *qpInfoVa = smem_ralloc_get_qp_info_address(entityId);
    __gm__ SmemRallocRdmaInfo *rdmaInfo = (__gm__ SmemRallocRdmaInfo *)qpInfoVa;
    uint32_t qpNum = rdmaInfo->qpNum;
    __gm__ SmemRallocWQCtx *qpCtxEntry =
        (__gm__ SmemRallocWQCtx *)(rdmaInfo->sqPtr + (remoteRankId * qpNum + qpIdx) * sizeof(SmemRallocWQCtx));
    auto curHardwareHeadAddr = qpCtxEntry->headAddr;
    smem_ralloc_cache_write_through((__gm__ uint8_t *)curHardwareHeadAddr, 8);
    uint32_t curHead = *(__gm__ uint32_t *)(curHardwareHeadAddr);
    return smem_ralloc_roce_poll_cq(entityId, remoteRankId, qpIdx, curHead, ubLocal64, ubLocal32);
}

/**
 * @brief Debug helper: dump the kernel-side QP context into a 328B device buffer (41 x u64) as a
 *        staged reachability probe. Each stage writes its slots and flushes them IMMEDIATELY, so
 *        when the kernel faults midway the host still reads back every stage that completed --
 *        the highest numbered non-zero slot marks exactly how far execution got. The host polls
 *        slot 41 (final magic) for completion. Layout contract (must stay in sync with
 *        SmemRallocDumpQpInfo in smem_ralloc.cpp):
 *        [0] 0xBEEF00000001  DVA-store probe: the very first store of the kernel, no meta access
 *        [1] qpInfoVa        meta-window read probe   [2] globalRank  [3] rankSize
 *        [4] qpNum           [5] sqPtr [6] rqPtr [7] scqPtr [8] rcqPtr [9] memPtr (QP-table read)
 *        [10..18] sq[dest] WQCtx: wqn/bufAddr/wqeSize/depth/headAddr/tailAddr/dbMode/dbAddr/sl
 *        [19] head value (dcci + read of the hardware headAddr)  [20] tail value (same for tailAddr)
 *        [21..28] scq[dest] CQCtx: cqn/bufAddr/cqeSize/depth/headAddr/tailAddr/dbMode/dbAddr
 *        [29] cq tail value (dcci + read of the hardware cq tailAddr)
 *        [30..34] mr[dest]: size/addr/lkey/rkey/regAddress
 *        [35..39] mr[local]: size/addr/lkey/rkey/regAddress
 *        [40] 0x52414E444D5031 DVA-store probe magic (written first, NOT a completion marker)
 *        [41] 0x46494E414C3132 final completion magic (written last, polled by the host)
 */
SMEM_RALLOC_INLINE_AICORE void smem_ralloc_roce_qpinfo_dump(uint32_t entityId, uint32_t destRankId, uint32_t qpIdx,
                                                            __gm__ uint8_t *out)
{
    /* stage 1: pure DVA-store probe -- proves the out buffer itself is writable by the AI core
     * before touching the meta window or the QP table */
    *(__gm__ uint64_t *)(out + 40 * 8) = 0x52414E444D5031ULL; /* "RANDMP1" DVA-store probe magic */
    *(__gm__ uint64_t *)(out + 0) = 0xBEEF00000001ULL;
    smem_ralloc_cache_write_through(out, 8);
    smem_ralloc_cache_write_through(out + 40 * 8, 8);

    /* stage 2: meta window read (0x17ffbe... region) -- the suspected unreachable mapping */
    __gm__ void *qpInfoVa = smem_ralloc_get_qp_info_address(entityId);
    *(__gm__ uint64_t *)(out + 1 * 8) = (uint64_t)qpInfoVa;
    *(__gm__ uint64_t *)(out + 2 * 8) = smem_ralloc_get_global_rank(entityId);
    *(__gm__ uint64_t *)(out + 3 * 8) = smem_ralloc_get_global_rank_size(entityId);
    smem_ralloc_cache_write_through(out + 8, 3 * 8);

    /* stage 3: QP table header (AclrtMalloc device heap) */
    __gm__ SmemRallocRdmaInfo *rdmaInfo = (__gm__ SmemRallocRdmaInfo *)qpInfoVa;
    *(__gm__ uint64_t *)(out + 4 * 8) = rdmaInfo->qpNum;
    *(__gm__ uint64_t *)(out + 5 * 8) = rdmaInfo->sqPtr;
    *(__gm__ uint64_t *)(out + 6 * 8) = rdmaInfo->rqPtr;
    *(__gm__ uint64_t *)(out + 7 * 8) = rdmaInfo->scqPtr;
    *(__gm__ uint64_t *)(out + 8 * 8) = rdmaInfo->rcqPtr;
    *(__gm__ uint64_t *)(out + 9 * 8) = rdmaInfo->memPtr;
    smem_ralloc_cache_write_through(out + 4 * 8, 6 * 8);

    /* stage 4: WQ context */
    __gm__ SmemRallocWQCtx *wq = (__gm__ SmemRallocWQCtx *)(rdmaInfo->sqPtr +
                                                            (destRankId * rdmaInfo->qpNum + qpIdx) *
                                                                sizeof(SmemRallocWQCtx));
    *(__gm__ uint64_t *)(out + 10 * 8) = wq->wqn;
    *(__gm__ uint64_t *)(out + 11 * 8) = wq->bufAddr;
    *(__gm__ uint64_t *)(out + 12 * 8) = wq->wqeSize;
    *(__gm__ uint64_t *)(out + 13 * 8) = wq->depth;
    *(__gm__ uint64_t *)(out + 14 * 8) = wq->headAddr;
    *(__gm__ uint64_t *)(out + 15 * 8) = wq->tailAddr;
    *(__gm__ uint64_t *)(out + 16 * 8) = (uint64_t)wq->dbMode;
    *(__gm__ uint64_t *)(out + 17 * 8) = wq->dbAddr;
    *(__gm__ uint64_t *)(out + 18 * 8) = wq->sl;
    smem_ralloc_cache_write_through(out + 10 * 8, 9 * 8);

    /* stage 5: dereference the hardware head/tail shadow addresses (dcci + load) */
    smem_ralloc_cache_write_through((__gm__ uint8_t *)wq->headAddr, 8);
    *(__gm__ uint64_t *)(out + 19 * 8) = *(__gm__ uint32_t *)(wq->headAddr);
    smem_ralloc_cache_write_through((__gm__ uint8_t *)wq->tailAddr, 8);
    *(__gm__ uint64_t *)(out + 20 * 8) = *(__gm__ uint32_t *)(wq->tailAddr);
    smem_ralloc_cache_write_through(out + 19 * 8, 2 * 8);

    /* stage 6: CQ context + hardware cq tail */
    __gm__ SmemRallocCQCtx *cq = (__gm__ SmemRallocCQCtx *)(rdmaInfo->scqPtr +
                                                            (destRankId * rdmaInfo->qpNum + qpIdx) *
                                                                sizeof(SmemRallocCQCtx));
    *(__gm__ uint64_t *)(out + 21 * 8) = cq->cqn;
    *(__gm__ uint64_t *)(out + 22 * 8) = cq->bufAddr;
    *(__gm__ uint64_t *)(out + 23 * 8) = cq->cqeSize;
    *(__gm__ uint64_t *)(out + 24 * 8) = cq->depth;
    *(__gm__ uint64_t *)(out + 25 * 8) = cq->headAddr;
    *(__gm__ uint64_t *)(out + 26 * 8) = cq->tailAddr;
    *(__gm__ uint64_t *)(out + 27 * 8) = (uint64_t)cq->dbMode;
    *(__gm__ uint64_t *)(out + 28 * 8) = cq->dbAddr;
    smem_ralloc_cache_write_through((__gm__ uint8_t *)cq->tailAddr, 8);
    *(__gm__ uint64_t *)(out + 29 * 8) = *(__gm__ uint32_t *)(cq->tailAddr);
    smem_ralloc_cache_write_through(out + 21 * 8, 9 * 8);

    /* stage 7: MR table entries */
    __gm__ SmemRallocMemInfo *remoteMemInfo = (__gm__ SmemRallocMemInfo *)(rdmaInfo->memPtr +
                                                                           sizeof(SmemRallocMemInfo) * destRankId);
    *(__gm__ uint64_t *)(out + 30 * 8) = remoteMemInfo->size;
    *(__gm__ uint64_t *)(out + 31 * 8) = remoteMemInfo->addr;
    *(__gm__ uint64_t *)(out + 32 * 8) = remoteMemInfo->lkey;
    *(__gm__ uint64_t *)(out + 33 * 8) = remoteMemInfo->rkey;
    *(__gm__ uint64_t *)(out + 34 * 8) = remoteMemInfo->regAddress;
    __gm__ SmemRallocMemInfo *localMemInfo = (__gm__ SmemRallocMemInfo *)(
        rdmaInfo->memPtr + sizeof(SmemRallocMemInfo) * smem_ralloc_get_global_rank(entityId));
    *(__gm__ uint64_t *)(out + 35 * 8) = localMemInfo->size;
    *(__gm__ uint64_t *)(out + 36 * 8) = localMemInfo->addr;
    *(__gm__ uint64_t *)(out + 37 * 8) = localMemInfo->lkey;
    *(__gm__ uint64_t *)(out + 38 * 8) = localMemInfo->rkey;
    *(__gm__ uint64_t *)(out + 39 * 8) = localMemInfo->regAddress;
    smem_ralloc_cache_write_through(out + 30 * 8, 10 * 8);

    /* final completion marker -- distinct from the stage-1 DVA probe magic in slot 40 */
    *(__gm__ uint64_t *)(out + 41 * 8) = 0x46494E414C3132ULL; /* "FINAL12" */
    smem_ralloc_cache_write_through(out + 41 * 8, 8);
}

#endif /* __MEMFABRIC_SMEM_RALLOC_AI_CORE_BASE_RDMA_H__ */
