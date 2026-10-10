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

/* Packaged device-scheduled RDMA kernels for ralloc pools (compiled at install time by
 * bisheng, same mechanism as libmf_hybm_copy_extend.so; NOT part of the main cmake build).
 *
 * The kernels read their context (rank, QP rings, MR table) from the fixed meta window via
 * smem_ralloc_aicore_base_rdma.h and post one-sided WRs by themselves. The whole sequence
 * contains no host interaction, so the host-side launchers below can be captured into an
 * NPU graph and replayed. Segment direction/peer verdicts come precheck-resolved in the
 * launch arguments (see smem_ralloc_device_launch_def.h).
 */
#include "kernel_operator.h"
#include "smem_ralloc_aicore_base_rdma.h"
#include "smem_ralloc_device_launch_def.h"
#include <cstdlib>

/* host-side launch config: one kernel block per QP lane. Reads the same env as the
 * transport layer (FixedRanksQpManager::ResolveQpsPerPeer), so table layout, block
 * count and lane mapping stay consistent, and graph capture replays identically. */
static uint32_t smem_ralloc_device_qps_per_peer()
{
    const char *env = getenv("MF_QPS_PER_PEER");
    if (env == nullptr) {
        return 1;
    }
    long v = atol(env);
    if (v < 1) {
        v = 1;
    }
    if (v > 4) {
        v = 4;
    }
    return (uint32_t)v;
}

/* host-side doorbell batch size for the DVA kernel (MF_DVA_DB_BATCH): publish every K WQEs
 * with one SQ doorbell. Default 1 keeps the historical per-WQE doorbell; clamped to the
 * lane budget so a pending batch never exceeds the outstanding-WQE bound of the in-kernel
 * quiet. Host-only: kernels cannot getenv, the value travels in the launch args. */
static uint32_t smem_ralloc_device_db_batch()
{
    const char *env = getenv("MF_DVA_DB_BATCH");
    if (env == nullptr) {
        return 1;
    }
    long v = atol(env);
    if (v < 1) {
        v = 1;
    }
    if (v > (long)SMEM_RALLOC_DEVICE_DVA_LANE_BUDGET) {
        v = (long)SMEM_RALLOC_DEVICE_DVA_LANE_BUDGET;
    }
    return (uint32_t)v;
}

/* must carry __aicore__: helpers without it compile as host functions and cannot be called
 * from __global__ __aicore__ kernels, nor use aicore-only TPipe/TQue APIs inside */
__aicore__ static void smem_ralloc_device_ub_alloc(AscendC::TPipe &pipe,
                                                   AscendC::TQue<AscendC::TPosition::VECIN, 1> &que64,
                                                   AscendC::TQue<AscendC::TPosition::VECIN, 1> &que32,
                                                   AscendC::LocalTensor<uint64_t> &ubLocal64,
                                                   AscendC::LocalTensor<uint32_t> &ubLocal32)
{
    pipe.InitBuffer(que64, 1, 32);
    pipe.InitBuffer(que32, 1, 32);
    ubLocal64 = que64.AllocTensor<uint64_t>();
    ubLocal32 = que32.AllocTensor<uint32_t>();
}

/* flush the pending (peer, head) doorbell batch of one lane, if any */
__aicore__ static void smem_ralloc_device_dva_flush_pending(uint32_t entityId, uint32_t lane, uint32_t &pendPeer,
                                                            uint32_t &pendHead, uint32_t &pendCnt,
                                                            AscendC::LocalTensor<uint64_t> &ubLocal64,
                                                            AscendC::LocalTensor<uint32_t> &ubLocal32)
{
    if (pendCnt != 0U) {
        smem_ralloc_rdma_post_send_flush(entityId, pendPeer, lane, pendHead, ubLocal64, ubLocal32);
        pendCnt = 0U;
    }
}

extern "C" __global__ __aicore__ void smem_ralloc_device_write_run_kernel(GM_ADDR dst, GM_ADDR src, uint64_t len,
                                                                          uint32_t entityId, uint32_t dstRank)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    /* single block drives one connection serially; extra blocks (if any) exit immediately */
    if (AscendC::GetBlockIdx() != 0) {
        return;
    }

    AscendC::TPipe pipe;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que64;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que32;
    AscendC::LocalTensor<uint64_t> ubLocal64;
    AscendC::LocalTensor<uint32_t> ubLocal32;
    smem_ralloc_device_ub_alloc(pipe, que64, que32, ubLocal64, ubLocal32);

    smem_ralloc_roce_write(entityId, (__gm__ uint8_t *)src, (__gm__ uint8_t *)dst, dstRank, 0, len, ubLocal64,
                           ubLocal32);
    (void)smem_ralloc_roce_quiet(entityId, dstRank, 0, ubLocal64, ubLocal32);

    que32.FreeTensor(ubLocal32);
    que64.FreeTensor(ubLocal64);
}

extern "C" void smem_ralloc_device_write_run_submit(uint32_t entityId, uint32_t dstRank, void *dst, void *src,
                                                    uint64_t len, void *stream)
{
    smem_ralloc_device_write_run_kernel<<<1, nullptr, stream>>>((uint8_t *)dst, (uint8_t *)src, len, entityId,
                                                                dstRank);
}

extern "C" __global__ __aicore__ void smem_ralloc_device_read_run_kernel(GM_ADDR src, GM_ADDR dst, uint64_t len,
                                                                         uint32_t entityId, uint32_t srcRank)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if (AscendC::GetBlockIdx() != 0) {
        return;
    }

    AscendC::TPipe pipe;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que64;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que32;
    AscendC::LocalTensor<uint64_t> ubLocal64;
    AscendC::LocalTensor<uint32_t> ubLocal32;
    smem_ralloc_device_ub_alloc(pipe, que64, que32, ubLocal64, ubLocal32);

    /* single one-sided READ: remote src slot -> local dst buffer */
    smem_ralloc_roce_read(entityId, (__gm__ uint8_t *)src, (__gm__ uint8_t *)dst, srcRank, 0, len, ubLocal64,
                          ubLocal32);
    (void)smem_ralloc_roce_quiet(entityId, srcRank, 0, ubLocal64, ubLocal32);

    que32.FreeTensor(ubLocal32);
    que64.FreeTensor(ubLocal64);
}

extern "C" void smem_ralloc_device_read_run_submit(uint32_t entityId, uint32_t srcRank, void *dst, void *src,
                                                   uint64_t len, void *stream)
{
    smem_ralloc_device_read_run_kernel<<<1, nullptr, stream>>>((uint8_t *)src, (uint8_t *)dst, len, entityId,
                                                               srcRank);
}

extern "C" __global__ __aicore__ void smem_ralloc_device_batch_run_kernel(struct smem_ralloc_device_batch_args args)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    /* multi-QP: one block per QP lane, block b exclusively owns lane b (the SQ head is a
     * single-producer index -- two blocks posting into one lane would race it). Blocks
     * beyond the table's lane count (stale table vs newer launch config) exit. */
    __gm__ void *qpInfoVa = smem_ralloc_get_qp_info_address(args.entityId);
    uint32_t laneNum = ((__gm__ SmemRallocRdmaInfo *)qpInfoVa)->qpNum;
    uint32_t blockIdx = AscendC::GetBlockIdx();
    if (laneNum == 0 || blockIdx >= laneNum) {
        return;
    }

    AscendC::TPipe pipe;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que64;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que32;
    AscendC::LocalTensor<uint64_t> ubLocal64;
    AscendC::LocalTensor<uint32_t> ubLocal32;
    smem_ralloc_device_ub_alloc(pipe, que64, que32, ubLocal64, ubLocal32);

    /* even deterministic distribution: segment j goes to block j % laneNum, so a graph
     * replay issues the identical WQE stream per lane */
    for (uint32_t i = blockIdx; i < args.count; i += laneNum) {
        const struct smem_ralloc_device_batch_seg *seg = &args.segs[i];
        if (seg->isWrite != 0U) {
            smem_ralloc_roce_write(args.entityId, (__gm__ uint8_t *)seg->src, (__gm__ uint8_t *)seg->dst,
                                   seg->peerRank, blockIdx, seg->size, ubLocal64, ubLocal32);
        } else {
            smem_ralloc_roce_read(args.entityId, (__gm__ uint8_t *)seg->src, (__gm__ uint8_t *)seg->dst,
                                  seg->peerRank, blockIdx, seg->size, ubLocal64, ubLocal32);
        }
    }

    /* per-block quiet: each block waits only for the WQEs it posted on its own lane, the
     * N lanes' completion waits overlap in parallel (single-lane configs keep today's
     * one-quiet-per-distinct-peer semantics on the lanes used) */
    for (uint32_t i = blockIdx; i < args.count; i += laneNum) {
        uint32_t peer = args.segs[i].peerRank;
        bool first = true;
        for (uint32_t j = blockIdx; j < i; j += laneNum) {
            if (args.segs[j].peerRank == peer) {
                first = false;
                break;
            }
        }
        if (first) {
            (void)smem_ralloc_roce_quiet(args.entityId, peer, blockIdx, ubLocal64, ubLocal32);
        }
    }

    que32.FreeTensor(ubLocal32);
    que64.FreeTensor(ubLocal64);
}

extern "C" void smem_ralloc_device_batch_run_submit(const struct smem_ralloc_device_batch_args *args, void *stream)
{
    /* host side: copy the precheck-resolved segment table into the launch argument area,
     * the kernel receives it by value and never dereferences host memory; launch one
     * block per QP lane so the kernel's block-to-lane mapping has a 1:1 producer */
    smem_ralloc_device_batch_run_kernel<<<smem_ralloc_device_qps_per_peer(), nullptr, stream>>>(*args);
}

/* distinct-peer tracking for the DVA kernel: one bit per peer rank, 8 words cover 512
 * ranks (today's pool worlds are far below that); an out-of-mask rank is quieted
 * immediately after its WQE -- correctness over batching for exotic world sizes */
#define SMEM_RALLOC_DVA_PEER_MASK_WORDS 8U

__aicore__ static void smem_ralloc_device_dva_quiet_mask(uint32_t entityId, const uint64_t *peerMask, uint32_t lane,
                                                         AscendC::LocalTensor<uint64_t> &ubLocal64,
                                                         AscendC::LocalTensor<uint32_t> &ubLocal32)
{
    for (uint32_t w = 0; w < SMEM_RALLOC_DVA_PEER_MASK_WORDS; w++) {
        const uint64_t m = peerMask[w];
        for (uint32_t b = 0; b < 64U; b++) {
            if (((m >> b) & 1ULL) != 0ULL) {
                (void)smem_ralloc_roce_quiet(entityId, (w << 6) | b, lane, ubLocal64, ubLocal32);
            }
        }
    }
}

extern "C" __global__ __aicore__ void smem_ralloc_device_batch_v2_run_kernel(
    struct smem_ralloc_device_batch_dva_args args)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    /* same lane discipline as the chunked batch kernel: one block per QP lane, block b
     * exclusively owns lane b; blocks beyond the table's lane count exit */
    __gm__ void *qpInfoVa = smem_ralloc_get_qp_info_address(args.entityId);
    uint32_t laneNum = ((__gm__ SmemRallocRdmaInfo *)qpInfoVa)->qpNum;
    uint32_t blockIdx = AscendC::GetBlockIdx();
    if (laneNum == 0 || blockIdx >= laneNum) {
        return;
    }

    AscendC::TPipe pipe;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que64;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que32;
    AscendC::LocalTensor<uint64_t> ubLocal64;
    AscendC::LocalTensor<uint32_t> ubLocal32;
    smem_ralloc_device_ub_alloc(pipe, que64, que32, ubLocal64, ubLocal32);

    /* SoA descriptor arrays in global memory: strided even distribution as before, the
     * only difference is where each segment's five fields are loaded from */
    __gm__ const uint64_t *srcArr = (__gm__ const uint64_t *)args.srcArray;
    __gm__ const uint64_t *dstArr = (__gm__ const uint64_t *)args.dstArray;
    __gm__ const uint64_t *sizeArr = (__gm__ const uint64_t *)args.sizeArray;
    __gm__ const uint32_t *rankArr = (__gm__ const uint32_t *)args.rankArray;
    __gm__ const uint32_t *writeArr = (__gm__ const uint32_t *)args.writeArray;

    /* doorbell batching: WQEs accumulate on the per-(peer, lane) SQ ring and one doorbell
     * publishes every dbBatch of them (PI semantics). The pending batch is flushed on peer
     * switch, at the lane-budget quiet, at dbBatch and at the end -- quiet polls the CQ up
     * to the flushed head, so an unflushed post would hang it. dbBatch = 1 reproduces the
     * historical per-WQE doorbell. Outstanding WQEs per peer stay bounded by
     * LANE_BUDGET (2048) << SQ depth (8192), so the near-full CQ poll of the single-shot
     * path is not needed here. The K schedule is a pure function of the descriptor array,
     * so a graph replay issues the identical doorbell sequence. */
    const uint32_t dbBatch = (args.dbBatch == 0U) ? 1U : args.dbBatch;
    uint32_t pendPeer = 0xFFFFFFFFU;
    uint32_t pendHead = 0;
    uint32_t pendCnt = 0;

    uint64_t peerMask[SMEM_RALLOC_DVA_PEER_MASK_WORDS] = {0};
    uint32_t lanePosted = 0;
    for (uint32_t i = blockIdx; i < args.count; i += laneNum) {
        const uint64_t src = srcArr[i];
        const uint64_t dst = dstArr[i];
        const uint64_t size = sizeArr[i];
        const uint32_t peer = rankArr[i];
        const uint32_t isWrite = writeArr[i];
        if (peer != pendPeer) {
            smem_ralloc_device_dva_flush_pending(args.entityId, blockIdx, pendPeer, pendHead, pendCnt, ubLocal64,
                                                 ubLocal32);
            /* first post on this peer since its last flush: pick up its SQ producer head
             * (after a flush without a peer change the register copy is still current) */
            pendHead = smem_ralloc_rdma_read_sq_head(args.entityId, peer, blockIdx);
            pendPeer = peer;
        }
        if (isWrite != 0U) {
            smem_ralloc_rdma_post_send_nowait(args.entityId, (__gm__ uint8_t *)dst, (__gm__ uint8_t *)src, peer,
                                              blockIdx, SmemRallocOpcode::OP_RDMA_WRITE, size, pendHead);
        } else {
            smem_ralloc_rdma_post_send_nowait(args.entityId, (__gm__ uint8_t *)src, (__gm__ uint8_t *)dst, peer,
                                              blockIdx, SmemRallocOpcode::OP_RDMA_READ, size, pendHead);
        }
        pendCnt++;
        if (peer < SMEM_RALLOC_DVA_PEER_MASK_WORDS * 64U) {
            peerMask[peer >> 6] |= 1ULL << (peer & 63U);
        } else {
            smem_ralloc_device_dva_flush_pending(args.entityId, blockIdx, pendPeer, pendHead, pendCnt, ubLocal64,
                                                 ubLocal32);
            (void)smem_ralloc_roce_quiet(args.entityId, peer, blockIdx, ubLocal64, ubLocal32);
        }
        lanePosted++;
        if (pendCnt >= dbBatch || lanePosted >= SMEM_RALLOC_DEVICE_DVA_LANE_BUDGET) {
            smem_ralloc_device_dva_flush_pending(args.entityId, blockIdx, pendPeer, pendHead, pendCnt, ubLocal64,
                                                 ubLocal32);
        }
        if (lanePosted >= SMEM_RALLOC_DEVICE_DVA_LANE_BUDGET) {
            /* in-kernel sub-batch drain: bounds the per-lane outstanding WQEs well below
             * the SQ ring depth (8192) so arbitrary counts are safe without host chunking */
            smem_ralloc_device_dva_quiet_mask(args.entityId, peerMask, blockIdx, ubLocal64, ubLocal32);
            for (uint32_t w = 0; w < SMEM_RALLOC_DVA_PEER_MASK_WORDS; w++) {
                peerMask[w] = 0;
            }
            lanePosted = 0;
        }
    }
    smem_ralloc_device_dva_flush_pending(args.entityId, blockIdx, pendPeer, pendHead, pendCnt, ubLocal64, ubLocal32);
    smem_ralloc_device_dva_quiet_mask(args.entityId, peerMask, blockIdx, ubLocal64, ubLocal32);

    que32.FreeTensor(ubLocal32);
    que64.FreeTensor(ubLocal64);
}

extern "C" void smem_ralloc_device_batch_v2_run_submit(const struct smem_ralloc_device_batch_dva_args *args,
                                                        void *stream)
{
    /* host side: only the scalars travel in the launch parameter area; the descriptor
     * arrays stay in device memory and are read by the kernel directly. dbBatch == 0 (the
     * C layer leaves it unset) selects the MF_DVA_DB_BATCH default. */
    struct smem_ralloc_device_batch_dva_args local = *args;
    if (local.dbBatch == 0U) {
        local.dbBatch = smem_ralloc_device_db_batch();
    }
    smem_ralloc_device_batch_v2_run_kernel<<<smem_ralloc_device_qps_per_peer(), nullptr, stream>>>(local);
}
