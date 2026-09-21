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

/* Packaged device-scheduled RDMA kernel for ralloc pools (compiled at install time by bisheng,
 * same mechanism as libmf_hybm_copy_extend.so; NOT part of the main cmake build).
 *
 * The kernel reads its context (rank, QP rings, MR table) from the fixed meta window via
 * smem_ralloc_aicore_base_rdma.h, posts `iters` RDMA WRITEs and quiets the connection, all on
 * device. The whole sequence contains no host interaction, so the host-side launcher below can
 * be captured into an NPU graph and replayed.
 */
#include "kernel_operator.h"
#include "smem_ralloc_aicore_base_rdma.h"

extern "C" __global__ __aicore__ void smem_ralloc_device_write_run_kernel(GM_ADDR dst, GM_ADDR src, uint64_t len,
                                                                          uint32_t iters, uint32_t entityId,
                                                                          uint32_t dstRank)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    /* single block drives one connection serially; extra blocks (if any) exit immediately */
    if (AscendC::GetBlockIdx() != 0) {
        return;
    }

    AscendC::TPipe pipe;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que64;
    AscendC::TQue<AscendC::TPosition::VECIN, 1> que32;
    pipe.InitBuffer(que64, 1, 32);
    pipe.InitBuffer(que32, 1, 32);
    AscendC::LocalTensor<uint64_t> ubLocal64 = que64.AllocTensor<uint64_t>();
    AscendC::LocalTensor<uint32_t> ubLocal32 = que32.AllocTensor<uint32_t>();

    for (uint32_t i = 0; i < iters; i++) {
        smem_ralloc_roce_write(entityId, (__gm__ uint8_t *)src, (__gm__ uint8_t *)dst, dstRank, 0, len, ubLocal64,
                               ubLocal32);
    }
    (void)smem_ralloc_roce_quiet(entityId, dstRank, 0, ubLocal64, ubLocal32);

    que32.FreeTensor(ubLocal32);
    que64.FreeTensor(ubLocal64);
}

extern "C" void smem_ralloc_device_write_run_submit(uint32_t entityId, uint32_t dstRank, void *dst, void *src,
                                                    uint64_t len, uint32_t iters, uint32_t dim, void *stream)
{
    smem_ralloc_device_write_run_kernel<<<dim, nullptr, stream>>>((uint8_t *)dst, (uint8_t *)src, len, iters,
                                                                  entityId, dstRank);
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
    pipe.InitBuffer(que64, 1, 32);
    pipe.InitBuffer(que32, 1, 32);
    AscendC::LocalTensor<uint64_t> ubLocal64 = que64.AllocTensor<uint64_t>();
    AscendC::LocalTensor<uint32_t> ubLocal32 = que32.AllocTensor<uint32_t>();

    /* single one-sided READ: remote src slot -> local dst buffer */
    smem_ralloc_roce_read(entityId, (__gm__ uint8_t *)src, (__gm__ uint8_t *)dst, srcRank, 0, len, ubLocal64,
                          ubLocal32);
    (void)smem_ralloc_roce_quiet(entityId, srcRank, 0, ubLocal64, ubLocal32);

    que32.FreeTensor(ubLocal32);
    que64.FreeTensor(ubLocal64);
}

extern "C" void smem_ralloc_device_read_run_submit(uint32_t entityId, uint32_t srcRank, void *dst, void *src,
                                                   uint64_t len, uint32_t iters, uint32_t dim, void *stream)
{
    /* iters kept for launcher signature symmetry, the read kernel issues exactly one READ */
    (void)iters;
    smem_ralloc_device_read_run_kernel<<<dim, nullptr, stream>>>((uint8_t *)src, (uint8_t *)dst, len, entityId,
                                                                 srcRank);
}
