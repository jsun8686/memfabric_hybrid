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

/* bring-up debug: writes the kernel-side QP context (40 x u64, see the dump layout contract in
 * smem_ralloc_aicore_base_rdma.h) into a caller-provided device-writable buffer; no UB needed,
 * plain GM stores plus a cache flush and a completion magic in slot 39 */
extern "C" __global__ __aicore__ void smem_ralloc_device_dump_run_kernel(uint32_t entityId, uint32_t peerRank,
                                                                         GM_ADDR out)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if (AscendC::GetBlockIdx() != 0) {
        return;
    }

    smem_ralloc_roce_qpinfo_dump(entityId, peerRank, 0, (__gm__ uint8_t *)out);
}

extern "C" void smem_ralloc_device_dump_run_submit(uint32_t entityId, uint32_t peerRank, void *out, void *stream)
{
    smem_ralloc_device_dump_run_kernel<<<1, nullptr, stream>>>(entityId, peerRank, (uint8_t *)out);
}

extern "C" __global__ __aicore__ void smem_ralloc_device_batch_run_kernel(struct smem_ralloc_device_batch_args args)
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

    for (uint32_t i = 0; i < args.count; i++) {
        const struct smem_ralloc_device_batch_seg *seg = &args.segs[i];
        if (seg->isWrite != 0U) {
            smem_ralloc_roce_write(args.entityId, (__gm__ uint8_t *)seg->src, (__gm__ uint8_t *)seg->dst,
                                   seg->peerRank, 0, seg->size, ubLocal64, ubLocal32);
        } else {
            smem_ralloc_roce_read(args.entityId, (__gm__ uint8_t *)seg->src, (__gm__ uint8_t *)seg->dst,
                                  seg->peerRank, 0, seg->size, ubLocal64, ubLocal32);
        }
    }

    /* one quiet per distinct peer: the CQE consumer index is per connection, a single
     * quiet waits for every segment already posted on that connection */
    for (uint32_t i = 0; i < args.count; i++) {
        uint32_t peer = args.segs[i].peerRank;
        bool first = true;
        for (uint32_t j = 0; j < i; j++) {
            if (args.segs[j].peerRank == peer) {
                first = false;
                break;
            }
        }
        if (first) {
            (void)smem_ralloc_roce_quiet(args.entityId, peer, 0, ubLocal64, ubLocal32);
        }
    }

    que32.FreeTensor(ubLocal32);
    que64.FreeTensor(ubLocal64);
}

extern "C" void smem_ralloc_device_batch_run_submit(const struct smem_ralloc_device_batch_args *args, void *stream)
{
    /* host side: copy the precheck-resolved segment table into the launch argument area,
     * the kernel receives it by value and never dereferences host memory */
    smem_ralloc_device_batch_run_kernel<<<1, nullptr, stream>>>(*args);
}
